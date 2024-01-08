/*
 * Block multiqueue core code
 *
 * Copyright (C) 2013-2014 Jens Axboe
 * Copyright (C) 2013-2014 Christoph Hellwig
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/kmemleak.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/smp.h>
#include <linux/llist.h>
#include <linux/list_sort.h>
#include <linux/cpu.h>
#include <linux/cache.h>
#include <linux/sched/sysctl.h>
#include <linux/delay.h>
#include <linux/crash_dump.h>

#include <trace/events/block.h>

#include <linux/blk-mq.h>
#include "blk.h"
#include "blk-mq.h"
#include "blk-mq-debugfs.h"
#include "blk-mq-tag.h"
#include "blk-mq-sched.h"
#include "blk-stat.h"

static DEFINE_MUTEX(all_q_mutex);
static LIST_HEAD(all_q_list);

static void blk_mq_poll_stats_start(struct request_queue *q);
static void blk_mq_poll_stats_fn(struct blk_stat_callback *cb);

/*
 * Check if any of the ctx's have pending work in this hardware queue
 */
static bool blk_mq_hctx_has_pending(struct blk_mq_hw_ctx *hctx)
{
	return !list_empty_careful(&hctx->dispatch) ||
		sbitmap_any_bit_set(&hctx->ctx_map) ||
			blk_mq_sched_has_work(hctx);
}

/*
 * Mark this ctx as having pending work in this hardware queue
 */
//该软件队列有req了，对应的硬件队列hctx->ctx_map里的bit位被置1，表示激活
static void blk_mq_hctx_mark_pending(struct blk_mq_hw_ctx *hctx,
				     struct blk_mq_ctx *ctx)
{
	if (!sbitmap_test_bit(&hctx->ctx_map, ctx->index_hw))
		sbitmap_set_bit(&hctx->ctx_map, ctx->index_hw);
}

static void blk_mq_hctx_clear_pending(struct blk_mq_hw_ctx *hctx,
				      struct blk_mq_ctx *ctx)
{
	sbitmap_clear_bit(&hctx->ctx_map, ctx->index_hw);
}

struct mq_inflight {
	struct hd_struct *part;
	unsigned int *inflight;
};

static void blk_mq_check_inflight(struct blk_mq_hw_ctx *hctx,
				  struct request *rq, void *priv,
				  bool reserved)
{
	struct mq_inflight *mi = priv;

    //957.27 内核有这个判断，957内核没有这个判断，rq->atomic_flags的取值有1、2、3
    //如果rq->atomic_flags为1，返回0，if成立，此时表示req没有设置过start状态
	if (!blk_mq_request_started(rq))
		return;

	/*
	 * index[0] counts the specific partition that was asked
	 * for. index[1] counts the ones that are active on the
	 * whole device, so increment that if mi->part is indeed
	 * a partition, and not a whole device.
	 */
	//看part_round_stats->part_in_flight->blk_mq_in_flight函数，这里的inflight[0]和inflight[1]就是来自part_round_stats()的局参
	if (rq->part == mi->part)
		mi->inflight[0]++;
	if (mi->part->partno)
		mi->inflight[1]++;
}

void blk_mq_in_flight(struct request_queue *q, struct hd_struct *part,
		      unsigned int inflight[2])
{
	struct mq_inflight mi = { .part = part, .inflight = inflight, };

	inflight[0] = inflight[1] = 0;
	blk_mq_queue_tag_busy_iter(q, blk_mq_check_inflight, &mi);
}

static void blk_mq_check_inflight_rw(struct blk_mq_hw_ctx *hctx,
				     struct request *rq, void *priv,
				     bool reserved)
{
	struct mq_inflight *mi = priv;

	if (!blk_mq_request_started(rq))
		return;

	if (rq->part == mi->part)
		mi->inflight[rq_data_dir(rq)]++;
}

void blk_mq_in_flight_rw(struct request_queue *q, struct hd_struct *part,
			 unsigned int inflight[2])
{
	struct mq_inflight mi = { .part = part, .inflight = inflight, };

	inflight[0] = inflight[1] = 0;
	blk_mq_queue_tag_busy_iter(q, blk_mq_check_inflight_rw, &mi);
}

void blk_freeze_queue_start(struct request_queue *q)
{
	int freeze_depth;

	freeze_depth = atomic_inc_return(&q->mq_freeze_depth);
	if (freeze_depth == 1) {
		percpu_ref_kill(&q->q_usage_counter);
		if (q->mq_ops)
			blk_mq_run_hw_queues(q, false);
	}
}
EXPORT_SYMBOL_GPL(blk_freeze_queue_start);

void blk_mq_freeze_queue_wait(struct request_queue *q)
{
	wait_event(q->mq_freeze_wq, percpu_ref_is_zero(&q->q_usage_counter));
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_wait);

int blk_mq_freeze_queue_wait_timeout(struct request_queue *q,
				     unsigned long timeout)
{
	return wait_event_timeout(q->mq_freeze_wq,
					percpu_ref_is_zero(&q->q_usage_counter),
					timeout);
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue_wait_timeout);

/*
 * Guarantee no request is in use, so we can change any data structure of
 * the queue afterward.
 */
void blk_freeze_queue(struct request_queue *q)
{
	/*
	 * In the !blk_mq case we are only calling this to kill the
	 * q_usage_counter, otherwise this increases the freeze depth
	 * and waits for it to return to zero.  For this reason there is
	 * no blk_unfreeze_queue(), and blk_freeze_queue() is not
	 * exported to drivers as the only user for unfreeze is blk_mq.
	 */
	blk_freeze_queue_start(q);
	if (!q->mq_ops)
		blk_drain_queue(q);
	blk_mq_freeze_queue_wait(q);
}

void blk_mq_freeze_queue(struct request_queue *q)
{
	/*
	 * ...just an alias to keep freeze and unfreeze actions balanced
	 * in the blk_mq_* namespace
	 */
	blk_freeze_queue(q);
}
EXPORT_SYMBOL_GPL(blk_mq_freeze_queue);

void blk_mq_unfreeze_queue(struct request_queue *q)
{
	int freeze_depth;

	freeze_depth = atomic_dec_return(&q->mq_freeze_depth);
	WARN_ON_ONCE(freeze_depth < 0);
	if (!freeze_depth) {
		percpu_ref_reinit(&q->q_usage_counter);
		wake_up_all(&q->mq_freeze_wq);
	}
}
EXPORT_SYMBOL_GPL(blk_mq_unfreeze_queue);

/*
 * FIXME: replace the scsi_internal_device_*block_nowait() calls in the
 * mpt3sas driver such that this function can be removed.
 */
void blk_mq_quiesce_queue_nowait(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	queue_flag_set(QUEUE_FLAG_QUIESCED, q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}
EXPORT_SYMBOL_GPL(blk_mq_quiesce_queue_nowait);

/**
 * blk_mq_quiesce_queue() - wait until all ongoing dispatches have finished
 * @q: request queue.
 *
 * Note: this function does not prevent that the struct request end_io()
 * callback function is invoked. Once this function is returned, we make
 * sure no dispatch can happen until the queue is unquiesced via
 * blk_mq_unquiesce_queue().
 */
void blk_mq_quiesce_queue(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned int i;
	bool rcu = false;

	blk_mq_quiesce_queue_nowait(q);

	queue_for_each_hw_ctx(q, hctx, i) {
		if (hctx->flags & BLK_MQ_F_BLOCKING)
			synchronize_srcu(&hctx->queue_rq_srcu);
		else
			rcu = true;
	}
	if (rcu)
		synchronize_rcu();
}
EXPORT_SYMBOL_GPL(blk_mq_quiesce_queue);

/*
 * blk_mq_unquiesce_queue() - counterpart of blk_mq_quiesce_queue()
 * @q: request queue.
 *
 * This function recovers queue into the state before quiescing
 * which is done by blk_mq_quiesce_queue.
 */
void blk_mq_unquiesce_queue(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	queue_flag_clear(QUEUE_FLAG_QUIESCED, q);
	spin_unlock_irqrestore(q->queue_lock, flags);

	/* dispatch requests which are inserted during quiescing */
	blk_mq_run_hw_queues(q, true);
}
EXPORT_SYMBOL_GPL(blk_mq_unquiesce_queue);

void blk_mq_wake_waiters(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned int i;

	queue_for_each_hw_ctx(q, hctx, i)
		if (blk_mq_hw_queue_mapped(hctx))
			blk_mq_tag_wakeup_all(hctx->tags, true);
}

bool blk_mq_can_queue(struct blk_mq_hw_ctx *hctx)
{
	return blk_mq_has_free_tags(hctx->tags);
}
EXPORT_SYMBOL(blk_mq_can_queue);

void blk_mq_rq_ctx_init(struct request_queue *q, struct blk_mq_ctx *ctx,
			struct request *rq, unsigned int rw_flags)
{
	if (blk_queue_io_stat(q))
		rw_flags |= REQ_IO_STAT;

	INIT_LIST_HEAD(&rq->queuelist);
	/* csd/requeue_work/fifo_time is initialized before use */
	rq->q = q;
    //赋值软件队列
	rq->mq_ctx = ctx;
	rq->cmd_flags |= rw_flags;
	/* do not touch atomic flags, it needs atomic ops against the timer */
	rq->cpu = -1;
	INIT_HLIST_NODE(&rq->hash);
	RB_CLEAR_NODE(&rq->rb_node);
	rq->rq_disk = NULL;
	rq->part = NULL;
    //req起始时间
	rq->start_time = jiffies;
#ifdef CONFIG_BLK_CGROUP
	rq->rl = NULL;
	set_start_time_ns(rq);
	rq->io_start_time_ns = 0;
#endif
	rq->nr_phys_segments = 0;
#if defined(CONFIG_BLK_DEV_INTEGRITY)
	rq->nr_integrity_segments = 0;
#endif
	rq->special = NULL;
	/* tag was already set */
	rq->errors = 0;

	rq->cmd = rq->__cmd;

	rq->extra_len = 0;
	rq->sense_len = 0;
	rq->resid_len = 0;
	rq->sense = NULL;

	INIT_LIST_HEAD(&rq->timeout_list);
	rq->timeout = 0;

	rq->end_io = NULL;
	rq->end_io_data = NULL;
	rq->next_rq = NULL;

	ctx->rq_dispatched[rw_is_sync(rw_flags)]++;
}
EXPORT_SYMBOL_GPL(blk_mq_rq_ctx_init);

/*从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag，然后req = tags->static_rqs[tag]
从static_rqs[]分配一个req，再req->tag=tag。接着hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。
分配失败则启动硬件IO数据派发，之后再尝试分配tag*/
struct request *__blk_mq_alloc_request(struct blk_mq_alloc_data *data, int rw)
{
	struct request *rq;
	unsigned int tag;

   //从硬件队列有关的blk_mq_tags结构体的static_rqs[]数组里得到空闲的request。获取失败则启动硬件IO数据派发，
   //之后再尝试从blk_mq_tags结构体的static_rqs[]数组里得到空闲的request。注意，这里返回的是空闲的request在
   //static_rqs[]数组的下标
	tag = blk_mq_get_tag(data);
	if (tag != BLK_MQ_TAG_FAIL) {
        //有调度器时返回硬件队列的hctx->sched_tags,无调度器时返回硬件队列的hctx->tags
		struct blk_mq_tags *tags = blk_mq_tags_from_data(data);
        
        //看到没，这里才是从tags->static_rqs[tag]得到空闲的req，tag是req在tags->static_rqs[ ]数组的下标
		rq = tags->static_rqs[tag];

		if (data->flags & BLK_MQ_REQ_INTERNAL) {//用调度器时设置
			rq->tag = -1;
			__rq_aux(rq, data->q)->internal_tag = tag;//这是req的tag
		} else {
		
		    //如果没有设置共享tag返回false，否则返回true。这里应该是标记该硬件队列处于繁忙状态?????????
			if (blk_mq_tag_busy(data->hctx)) {
				rq->cmd_flags = REQ_MQ_INFLIGHT;
				atomic_inc(&data->hctx->nr_active);
			}
            //赋值为空闲req在blk_mq_tags结构体的static_rqs[]数组的下标
			rq->tag = tag;
			__rq_aux(rq, data->q)->internal_tag = -1;
            //这里边保存的req是刚从static_rqs[]得到的空闲的req
			data->hctx->tags->rqs[rq->tag] = rq;
		}
        //对新分配的req进行初始化，赋值软件队列、req起始时间等
		blk_mq_rq_ctx_init(data->q, data->ctx, rq, rw);
		if (data->flags & BLK_MQ_REQ_PREEMPT)
			rq->cmd_flags |= REQ_PREEMPT;

		return rq;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(__blk_mq_alloc_request);

struct request *blk_mq_alloc_request(struct request_queue *q, int rw,
		unsigned int flags)
{
	struct blk_mq_alloc_data alloc_data = { .flags = flags };
	struct request *rq;
	int ret;

	ret = blk_queue_enter(q, flags);
	if (ret)
		return ERR_PTR(ret);

	rq = blk_mq_sched_get_request(q, NULL, rw, &alloc_data);

	blk_mq_put_ctx(alloc_data.ctx);
	blk_queue_exit(q);

	if (!rq)
		return ERR_PTR(-EWOULDBLOCK);
	return rq;
}
EXPORT_SYMBOL(blk_mq_alloc_request);

struct request *blk_mq_alloc_request_hctx(struct request_queue *q, int rw,
		unsigned int flags, unsigned int hctx_idx)
{
	struct blk_mq_alloc_data alloc_data = { .flags = flags };
	struct request *rq;
	unsigned int cpu;
	int ret;

	/*
	 * If the tag allocator sleeps we could get an allocation for a
	 * different hardware context.  No need to complicate the low level
	 * allocator for this for the rare use case of a command tied to
	 * a specific queue.
	 */
	if (WARN_ON_ONCE(!(flags & BLK_MQ_REQ_NOWAIT)))
		return ERR_PTR(-EINVAL);

	if (hctx_idx >= q->nr_hw_queues)
		return ERR_PTR(-EIO);

	ret = blk_queue_enter(q, flags);
	if (ret)
		return ERR_PTR(ret);

	/*
	 * Check if the hardware context is actually mapped to anything.
	 * If not tell the caller that it should skip this queue.
	 */
	alloc_data.hctx = q->queue_hw_ctx[hctx_idx];
	if (!blk_mq_hw_queue_mapped(alloc_data.hctx)) {
		blk_queue_exit(q);
		return ERR_PTR(-EXDEV);
	}
	cpu = cpumask_first(alloc_data.hctx->cpumask);
	alloc_data.ctx = __blk_mq_get_ctx(q, cpu);

	rq = blk_mq_sched_get_request(q, NULL, rw, &alloc_data);

	blk_queue_exit(q);

	if (!rq)
		return ERR_PTR(-EWOULDBLOCK);

	return rq;
}
EXPORT_SYMBOL_GPL(blk_mq_alloc_request_hctx);

static void
blk_mq_sched_completed_request(struct request *rq)
{
	struct elevator_queue *e = rq->q->elevator;

	if (e && e->aux->ops.mq.completed_request)
		e->aux->ops.mq.completed_request(rq);
}

void __blk_mq_finish_request(struct blk_mq_hw_ctx *hctx, struct blk_mq_ctx *ctx,
			     struct request *rq)
{
	const int sched_tag = rq_aux(rq)->internal_tag;
	struct request_queue *q = rq->q;

	if (rq->cmd_flags & REQ_MQ_INFLIGHT)
		atomic_dec(&hctx->nr_active);
	rq->cmd_flags = 0;

	clear_bit(REQ_ATOM_STARTED, &rq->atomic_flags);
	if (rq->tag != -1)
		blk_mq_put_tag(hctx, hctx->tags, ctx, rq->tag);
	if (sched_tag != -1)
		blk_mq_put_tag(hctx, hctx->sched_tags, ctx, sched_tag);
	blk_mq_sched_restart(hctx);
	blk_queue_exit(q);
}

static void blk_mq_finish_hctx_request(struct blk_mq_hw_ctx *hctx,
				       struct request *rq)
{
	struct blk_mq_ctx *ctx = rq->mq_ctx;

	ctx->rq_completed[rq_is_sync(rq)]++;
	__blk_mq_finish_request(hctx, ctx, rq);
}
EXPORT_SYMBOL_GPL(blk_mq_finish_request);

void blk_mq_finish_request(struct request *rq)
{
	blk_mq_finish_hctx_request(blk_mq_map_queue(rq->q, rq->mq_ctx->cpu), rq);
 }

void blk_mq_free_request(struct request *rq)
{
	blk_mq_sched_put_request(rq);
}
EXPORT_SYMBOL_GPL(blk_mq_free_request);

inline void __blk_mq_end_request(struct request *rq, int error)
{
    //有req传输完成了，增加ios、ticks、time_in_queue、io_ticks、flight等使用计数
	blk_account_io_done(rq);

	if (rq->end_io) {
		rq->end_io(rq, error);
	} else {
		if (unlikely(blk_bidi_rq(rq)))
			blk_mq_free_request(rq->next_rq);
		blk_mq_free_request(rq);
	}
}
EXPORT_SYMBOL(__blk_mq_end_request);
//有req传输完成了，增加ios、ticks、time_in_queue、io_ticks、flight、sectors扇区数等使用计数。
//依次取出req->bio链表上所有req对应的bio,一个一个更新bio结构体成员数据，执行bio的回调函数.还更新req->__data_len和req->buffer。
void blk_mq_end_request(struct request *rq, int error)
{
    /* 1 增加sectors扇区数IO使用计数，即传输的扇区数。更新req->__data_len和req->buffer
     2 依次取出req->bio链表上所有req对应的bio,一个一个更新bio结构体成员数据，执行bio的回调函数*/
	if (blk_update_request(rq, error, blk_rq_bytes(rq)))
		BUG();
    //有req传输完成了，增加ios、ticks、time_in_queue、io_ticks、flight等使用计数
	__blk_mq_end_request(rq, error);
}
EXPORT_SYMBOL(blk_mq_end_request);

static void __blk_mq_complete_request_remote(void *data)
{
	struct request *rq = data;

	rq->q->softirq_done_fn(rq);
}

static void blk_mq_ipi_complete_request(struct request *rq)
{
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	bool shared = false;
	int cpu;

	if (!test_bit(QUEUE_FLAG_SAME_COMP, &rq->q->queue_flags)) {
		rq->q->softirq_done_fn(rq);
		return;
	}

	cpu = get_cpu();
	if (!test_bit(QUEUE_FLAG_SAME_FORCE, &rq->q->queue_flags))
		shared = cpus_share_cache(cpu, ctx->cpu);

	if (cpu != ctx->cpu && !shared && cpu_online(ctx->cpu)) {
		rq->csd.func = __blk_mq_complete_request_remote;
		rq->csd.info = rq;
		rq->csd.flags = 0;
		smp_call_function_single_async(ctx->cpu, &rq->csd);
	} else {
		rq->q->softirq_done_fn(rq);
	}
	put_cpu();
}

static void blk_mq_stat_add(struct request *rq)
{
	if (rq->cmd_flags & REQ_STATS) {
		blk_mq_poll_stats_start(rq->q);
		blk_stat_add(rq);
	}
}

static void __blk_mq_complete_request(struct request *rq, bool sync)
{
	struct request_queue *q = rq->q;

	if (rq_aux(rq)->internal_tag != -1)
		blk_mq_sched_completed_request(rq);

	blk_mq_stat_add(rq);

	if (!q->softirq_done_fn)
		blk_mq_end_request(rq, rq->errors);
	else if (sync)
		rq->q->softirq_done_fn(rq);
	else
		blk_mq_ipi_complete_request(rq);
}

static void hctx_unlock(struct blk_mq_hw_ctx *hctx, int srcu_idx)
	__releases(hctx->srcu)
{
	if (!(hctx->flags & BLK_MQ_F_BLOCKING))
		rcu_read_unlock();
	else
		srcu_read_unlock(&hctx->queue_rq_srcu, srcu_idx);
}

static void hctx_lock(struct blk_mq_hw_ctx *hctx, int *srcu_idx)
	__acquires(hctx->srcu)
{
	if (!(hctx->flags & BLK_MQ_F_BLOCKING)) {
		/* shut up gcc false positive */
		*srcu_idx = 0;
		rcu_read_lock();
	} else
		*srcu_idx = srcu_read_lock(&hctx->queue_rq_srcu);
}

/**
 * blk_mq_complete_request - end I/O on a request
 * @rq:		the request being processed
 *
 * Description:
 *	Ends all I/O on a request. It does not handle partial completions.
 *	The actual completion happens out-of-order, through a IPI handler.
 **/
void blk_mq_complete_request(struct request *rq, int error)
{
	struct request_queue *q = rq->q;

	if (unlikely(blk_should_fake_timeout(q)))
		return;
	if (!blk_mark_rq_complete(rq)) {
		rq->errors = error;
		__blk_mq_complete_request(rq, false);
	}
}
EXPORT_SYMBOL(blk_mq_complete_request);

void blk_mq_complete_request_sync(struct request *rq, int error)
{
	if (!blk_mark_rq_complete(rq)) {
		rq->errors = error;
		__blk_mq_complete_request(rq, true);
	}
}
EXPORT_SYMBOL_GPL(blk_mq_complete_request_sync);

int blk_mq_request_started(struct request *rq)
{
	return test_bit(REQ_ATOM_STARTED, &rq->atomic_flags);
}
EXPORT_SYMBOL_GPL(blk_mq_request_started);

void blk_mq_start_request(struct request *rq)
{
	struct request_queue *q = rq->q;

	blk_mq_sched_started_request(rq);

	trace_block_rq_issue(q, rq);

	rq->resid_len = blk_rq_bytes(rq);//req代表的磁盘len
	if (unlikely(blk_bidi_rq(rq)))
		rq->next_rq->resid_len = blk_rq_bytes(rq->next_rq);

	if (test_bit(QUEUE_FLAG_STATS, &q->queue_flags)) {
		blk_stat_set_issue_time(&rq_aux(rq)->issue_stat);
		rq->cmd_flags |= REQ_STATS;
	}
    //把req添加到q->timeout_list，并且启动q->timeout
	blk_add_timer(rq);

	/*
	 * Ensure that ->deadline is visible before set the started
	 * flag and clear the completed flag.
	 */
	smp_mb__before_atomic();

	/*
	 * Mark us as started and clear complete. Complete might have been
	 * set if requeue raced with timeout, which then marked it as
	 * complete. So be sure to clear complete again when we start
	 * the request, otherwise we'll ignore the completion event.
	 */
	if (!test_bit(REQ_ATOM_STARTED, &rq->atomic_flags))
		set_bit(REQ_ATOM_STARTED, &rq->atomic_flags);
	if (test_bit(REQ_ATOM_COMPLETE, &rq->atomic_flags))
		clear_bit(REQ_ATOM_COMPLETE, &rq->atomic_flags);

	if (q->dma_drain_size && blk_rq_bytes(rq)) {
		/*
		 * Make sure space for the drain appears.  We know we can do
		 * this because max_hw_segments has been adjusted to be one
		 * fewer than the device can handle.
		 */
		rq->nr_phys_segments++;
	}
}
EXPORT_SYMBOL(blk_mq_start_request);

/*
 * When we reach here because queue is busy, REQ_ATOM_COMPLETE
 * flag isn't set yet, so there may be race with timeout hanlder,
 * but given rq->deadline is just set in .queue_rq() under
 * this situation, the race won't be possible in reality because
 * rq->timeout should be set as big enough to cover the window
 * between blk_mq_start_request() called from .queue_rq() and
 * clearing REQ_ATOM_STARTED here.
 */
static void __blk_mq_requeue_request(struct request *rq)
{
	struct request_queue *q = rq->q;
    //tags->bitmap_tags中按照req->tag这个tag编号释放tag
	blk_mq_put_driver_tag(rq);

	trace_block_rq_requeue(q, rq);

	if (test_and_clear_bit(REQ_ATOM_STARTED, &rq->atomic_flags)) {
		if (q->dma_drain_size && blk_rq_bytes(rq))
			rq->nr_phys_segments--;
	}
}

void blk_mq_requeue_request(struct request *rq, bool kick_requeue_list)
{
	__blk_mq_requeue_request(rq);

	/* this request will be re-inserted to io scheduler queue */
	blk_mq_sched_requeue_request(rq);

	BUG_ON(blk_queued_rq(rq));
	blk_mq_add_to_requeue_list(rq, true, kick_requeue_list);
}
EXPORT_SYMBOL(blk_mq_requeue_request);

static void blk_mq_requeue_work(struct work_struct *work)
{
	struct request_queue *q =
		container_of(work, struct request_queue, requeue_work.work);
	LIST_HEAD(rq_list);
	struct request *rq, *next;
	unsigned long flags;

	spin_lock_irqsave(&q->requeue_lock, flags);
	list_splice_init(&q->requeue_list, &rq_list);
	spin_unlock_irqrestore(&q->requeue_lock, flags);

	list_for_each_entry_safe(rq, next, &rq_list, queuelist) {
		if (!(rq->cmd_flags & REQ_SOFTBARRIER))
			continue;

		rq->cmd_flags &= ~REQ_SOFTBARRIER;
		list_del_init(&rq->queuelist);
		blk_mq_sched_insert_request(rq, true, false, false);
	}

	while (!list_empty(&rq_list)) {
		rq = list_entry(rq_list.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		blk_mq_sched_insert_request(rq, false, false, false);
	}

	blk_mq_run_hw_queues(q, false);
}

void blk_mq_add_to_requeue_list(struct request *rq, bool at_head,
				bool kick_requeue_list)
{
	struct request_queue *q = rq->q;
	unsigned long flags;

	/*
	 * We abuse this flag that is otherwise used by the I/O scheduler to
	 * request head insertation from the workqueue.
	 */
	BUG_ON(rq->cmd_flags & REQ_SOFTBARRIER);

	spin_lock_irqsave(&q->requeue_lock, flags);
	if (at_head) {
		rq->cmd_flags |= REQ_SOFTBARRIER;
		list_add(&rq->queuelist, &q->requeue_list);
	} else {
		list_add_tail(&rq->queuelist, &q->requeue_list);
	}
	spin_unlock_irqrestore(&q->requeue_lock, flags);

	if (kick_requeue_list)
		blk_mq_kick_requeue_list(q);
}
EXPORT_SYMBOL(blk_mq_add_to_requeue_list);

void blk_mq_kick_requeue_list(struct request_queue *q)
{
	kblockd_mod_delayed_work_on(WORK_CPU_UNBOUND, &q->requeue_work, 0);
}
EXPORT_SYMBOL(blk_mq_kick_requeue_list);

void blk_mq_delay_kick_requeue_list(struct request_queue *q,
				    unsigned long msecs)
{
	kblockd_schedule_delayed_work(&q->requeue_work,
				      msecs_to_jiffies(msecs));
}
EXPORT_SYMBOL(blk_mq_delay_kick_requeue_list);

struct request *blk_mq_tag_to_rq(struct blk_mq_tags *tags, unsigned int tag)
{
	if (tag < tags->nr_tags)
		return tags->rqs[tag];

	return NULL;
}
EXPORT_SYMBOL(blk_mq_tag_to_rq);

struct blk_mq_timeout_data {
	unsigned long next;
	unsigned int next_set;
};

void blk_mq_rq_timed_out(struct request *req, bool reserved)
{
	const struct blk_mq_ops *ops = req->q->mq_ops;
	enum blk_eh_timer_return ret = BLK_EH_RESET_TIMER;

	/*
	 * We know that complete is set at this point. If STARTED isn't set
	 * anymore, then the request isn't active and the "timeout" should
	 * just be ignored. This can happen due to the bitflag ordering.
	 * Timeout first checks if STARTED is set, and if it is, assumes
	 * the request is active. But if we race with completion, then
	 * we both flags will get cleared. So check here again, and ignore
	 * a timeout event with a request that isn't active.
	 */
	if (!test_bit(REQ_ATOM_STARTED, &req->atomic_flags))
		return;

	if (ops->timeout)
		ret = ops->timeout(req, reserved);

	switch (ret) {
	case BLK_EH_HANDLED:
		__blk_mq_complete_request(req, false);
		break;
	case BLK_EH_RESET_TIMER:
		blk_add_timer(req);
		blk_clear_rq_complete(req);
		break;
	case BLK_EH_NOT_HANDLED:
		break;
	default:
		printk(KERN_ERR "block: bad eh return: %d\n", ret);
		break;
	}
}

static void blk_mq_check_expired(struct blk_mq_hw_ctx *hctx,
		struct request *rq, void *priv, bool reserved)
{
	struct blk_mq_timeout_data *data = priv;

	if (!test_bit(REQ_ATOM_STARTED, &rq->atomic_flags))
		return;

	/*
	 * The rq being checked may have been freed and reallocated
	 * out already here, we avoid this race by checking rq->deadline
	 * and REQ_ATOM_COMPLETE flag together:
	 *
	 * - if rq->deadline is observed as new value because of
	 *   reusing, the rq won't be timed out because of timing.
	 * - if rq->deadline is observed as previous value,
	 *   REQ_ATOM_COMPLETE flag won't be cleared in reuse path
	 *   because we put a barrier between setting rq->deadline
	 *   and clearing the flag in blk_mq_start_request(), so
	 *   this rq won't be timed out too.
	 */
	if (time_after_eq(jiffies, rq->deadline)) {
		if (!blk_mark_rq_complete(rq))
			blk_mq_rq_timed_out(rq, reserved);
	} else if (!data->next_set || time_after(data->next, rq->deadline)) {
		data->next = rq->deadline;
		data->next_set = 1;
	}
}
//blk_mq_init_allocated_queue初始化
static void blk_mq_timeout_work(struct work_struct *work)
{
	struct request_queue *q =
		container_of(work, struct request_queue, timeout_work);
	struct blk_mq_timeout_data data = {
		.next		= 0,
		.next_set	= 0,
	};
	int i;

	/* A deadlock might occur if a request is stuck requiring a
	 * timeout at the same time a queue freeze is waiting
	 * completion, since the timeout code would not be able to
	 * acquire the queue reference here.
	 *
	 * That's why we don't use blk_queue_enter here; instead, we use
	 * percpu_ref_tryget directly, because we need to be able to
	 * obtain a reference even in the short window between the queue
	 * starting to freeze, by dropping the first reference in
	 * blk_freeze_queue_start, and the moment the last request is
	 * consumed, marked by the instant q_usage_counter reaches
	 * zero.
	 */
	if (!percpu_ref_tryget(&q->q_usage_counter))
		return;

	blk_mq_queue_tag_busy_iter(q, blk_mq_check_expired, &data);

	if (data.next_set) {
		data.next = blk_rq_timeout(round_jiffies_up(data.next));
		mod_timer(&q->timeout, data.next);
	} else {
		struct blk_mq_hw_ctx *hctx;

		queue_for_each_hw_ctx(q, hctx, i) {
			/* the hctx may be unmapped, so check it here */
			if (blk_mq_hw_queue_mapped(hctx))
				blk_mq_tag_idle(hctx);
		}
	}
	blk_queue_exit(q);
}

/*
 * Reverse check our software queue for entries that we could potentially
 * merge with. Currently includes a hand-wavy stop count of 8, to not spend
 * too much time checking for merges.
 */
//这个函数看着不复杂呀，就是依次遍历软件队列ctx->rq_list链表上的req，然后看req能否与bio前项或者后项合并
static bool blk_mq_attempt_merge(struct request_queue *q,
				 struct blk_mq_ctx *ctx, struct bio *bio)
{
	struct request *rq;
	int checked = 8;
    //依次遍历软件队列ctx->rq_list链表上的req
	list_for_each_entry_reverse(rq, &ctx->rq_list, queuelist) {
		int el_ret;

		if (!checked--)
			break;

		if (!blk_rq_merge_ok(rq, bio))
			continue;
        
        //检查bio和req代表的磁盘范围是否挨着，挨着则可以合并
		el_ret = blk_try_merge(rq, bio);
		if (el_ret == ELEVATOR_NO_MERGE)
			continue;

		if (!blk_mq_sched_allow_merge(q, rq, bio))
			break;

        //前项合并
		if (el_ret == ELEVATOR_BACK_MERGE) {
			if (bio_attempt_back_merge(q, rq, bio)) {
				ctx->rq_merged++;
				return true;
			}
			break;
        //前项合并
		} else if (el_ret == ELEVATOR_FRONT_MERGE) {
			if (bio_attempt_front_merge(q, rq, bio)) {
				ctx->rq_merged++;
				return true;
			}
			break;
		}
	}

	return false;
}

struct flush_busy_ctx_data {
	struct blk_mq_hw_ctx *hctx;
	struct list_head *list;
};

static bool flush_busy_ctx(struct sbitmap *sb, unsigned int bitnr, void *data)
{
	struct flush_busy_ctx_data *flush_data = data;
	struct blk_mq_hw_ctx *hctx = flush_data->hctx;
	struct blk_mq_ctx *ctx = hctx->ctxs[bitnr];

	spin_lock(&ctx->lock);
    //把hctx->ctxs[[bitnr]]这个软件队列上的ctx->rq_list链表上req转移到flush_data->list链表尾部，然后清空ctx->rq_list链表
	list_splice_tail_init(&ctx->rq_list, flush_data->list);
	sbitmap_clear_bit(sb, bitnr);
	spin_unlock(&ctx->lock);
	return true;
}

/*
 * Process software queues that have been marked busy, splicing them
 * to the for-dispatch
 */
void blk_mq_flush_busy_ctxs(struct blk_mq_hw_ctx *hctx, struct list_head *list)
{
	struct flush_busy_ctx_data data = {
		.hctx = hctx,
		.list = list,
	};
    
   //flush_busy_ctx:把硬件队列hctx关联的软件队列上的ctx->rq_list链表上req转移到传入的list链表尾部，然后清空ctx->rq_list链表
   //这样貌似是把硬件队列hctx关联的所有软件队列ctx->rq_list链表上的req全部移动到list链表尾部
	sbitmap_for_each_set(&hctx->ctx_map, flush_busy_ctx, &data);
}
EXPORT_SYMBOL_GPL(blk_mq_flush_busy_ctxs);

struct dispatch_rq_data {
	struct blk_mq_hw_ctx *hctx;
	struct request *rq;
};
//从软件ctx->rq_list取出req，然后从软件队列中剔除req，接着清除hctx->ctx_map中软件队列对应的标志位???????
static bool dispatch_rq_from_ctx(struct sbitmap *sb, unsigned int bitnr,
		void *data)
{
	struct dispatch_rq_data *dispatch_data = data;
	struct blk_mq_hw_ctx *hctx = dispatch_data->hctx;
	struct blk_mq_ctx *ctx = hctx->ctxs[bitnr];

	spin_lock(&ctx->lock);
	if (unlikely(!list_empty(&ctx->rq_list))) {
        //从软件ctx取出req
		dispatch_data->rq = list_entry_rq(ctx->rq_list.next);
        //从软件队列中剔除req
		list_del_init(&dispatch_data->rq->queuelist);
        //清除hctx->ctx_map中软件队列对应的标志位
		if (list_empty(&ctx->rq_list))
			sbitmap_clear_bit(sb, bitnr);
	}
	spin_unlock(&ctx->lock);

	return !dispatch_data->rq;
}

struct request *blk_mq_dequeue_from_ctx(struct blk_mq_hw_ctx *hctx,
					struct blk_mq_ctx *start)
{
	unsigned off = start ? start->index_hw : 0;
	struct dispatch_rq_data data = {
		.hctx = hctx,
		.rq   = NULL,
	};
    //从软件ctx->rq_list取出req，然后从软件队列中剔除req，接着清除hctx->ctx_map中软件队列对应的标志位???????
	__sbitmap_for_each_set(&hctx->ctx_map, off,
			       dispatch_rq_from_ctx, &data);

	return data.rq;
}

static inline unsigned int queued_to_index(unsigned int queued)
{
	if (!queued)
		return 0;

	return min(BLK_MQ_MAX_DISPATCH_ORDER - 1, ilog2(queued) + 1);
}
//从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
//hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环。

//rq即req来自当前进程的plug->mq_list链表或者其他链表，现在赋值到了硬件队列hctx->tags->rqs[rq->tag]结构。这个过程叫做给req在
//blk_mq_tags里分配一个空闲tag，建立req与硬件队列的关系吧。每一个req启动硬件传输前都得从blk_mq_tags里分配一个空闲tag!!!!!
/*有一点需要注意，凡是执行blk_mq_get_driver_tag()的情况，都是该req在第一次派发时遇到硬件队列繁忙，就把tag释放了，然后rq->tag=-1。
接着启动异步派发，才会执行该函数，if (rq->tag != -1)的判断应该就是判断req的tag是否被释放过，释放了才会接着执行*/
bool blk_mq_get_driver_tag(struct request *rq, struct blk_mq_hw_ctx **hctx,
			   bool wait)//req一种情况来自当前进程plug->mq_list链表，也有hctx->dispatch链表，还有软件队列rq_list链表
			   //wait 为false即便获取tag失败也不会休眠
{
	struct blk_mq_alloc_data data = {
		.q = rq->q,
		.hctx = blk_mq_map_queue(rq->q, rq->mq_ctx->cpu),
		.flags = wait ? 0 : BLK_MQ_REQ_NOWAIT,
	};

    //如果req对应的tag没有被释放，则直接返回完事，其实还有一种情况rq->tag被置-1，就是__blk_mq_alloc_request()函数分配过tag和req后，
    //如果使用了调度器，则rq->tag = -1。这种情况，rq->tag != -1也成立，但是再直接执行blk_mq_get_driver_tag()分配tag也没啥意思呀，
    //因为tag已经分配过了。所以感觉该函数主要还是针对req因磁盘硬件驱动繁忙无法派送，然后释放了tag，再派发时分配tag的情况。
	if (rq->tag != -1)
		goto done;
    
    //判断tag是否预留的，是则加上BLK_MQ_REQ_RESERVED标志
	if (blk_mq_tag_is_reserved(data.hctx->sched_tags, rq_aux(rq)->internal_tag))
		data.flags |= BLK_MQ_REQ_RESERVED;

    //从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
    //hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环。
	rq->tag = blk_mq_get_tag(&data);
	if (rq->tag >= 0) {
        //如果硬件队列繁忙
		if (blk_mq_tag_busy(data.hctx)) {
			rq->cmd_flags |= REQ_MQ_INFLIGHT;
			atomic_inc(&data.hctx->nr_active);
		}
        //对hctx->tags->rqs[rq->tag]赋值
		data.hctx->tags->rqs[rq->tag] = rq;
	}

done:
    
    //之所以这里重新赋值，是因为blk_mq_get_tag中可能会休眠，等再次唤醒进程所在CPU就变了，就会重新获取一次硬件队列保存到data.hctx
	if (hctx)
		*hctx = data.hctx;
    
    //分配成功返回1
	return rq->tag != -1;
}

static int blk_mq_dispatch_wake(wait_queue_t *wait, unsigned mode,
				int flags, void *key)
{
	struct blk_mq_hw_ctx *hctx;

	hctx = container_of(wait, struct blk_mq_hw_ctx, dispatch_wait);

	list_del_init(&wait->task_list);
	blk_mq_run_hw_queue(hctx, true);
	return 1;
}

/*
 * Mark us waiting for a tag. For shared tags, this involves hooking us into
 * the tag wakeups. For non-shared tags, we can simply mark us nedeing a
 * restart. For both caes, take care to check the condition again after
 * marking us as waiting.
 */
static bool blk_mq_mark_tag_wait(struct blk_mq_hw_ctx **hctx,
				 struct request *rq)
{
	struct blk_mq_hw_ctx *this_hctx = *hctx;
	struct sbq_wait_state *ws;
	wait_queue_t *wait;
	bool ret;

	if (!(this_hctx->flags & BLK_MQ_F_TAG_SHARED)) {
		if (!test_bit(BLK_MQ_S_SCHED_RESTART, &this_hctx->state))
			set_bit(BLK_MQ_S_SCHED_RESTART, &this_hctx->state);
		/*
		 * It's possible that a tag was freed in the window between the
		 * allocation failure and adding the hardware queue to the wait
		 * queue.
		 *
		 * Don't clear RESTART here, someone else could have set it.
		 * At most this will cost an extra queue run.
		 */
		return blk_mq_get_driver_tag(rq, hctx, false);
	}

	wait = &this_hctx->dispatch_wait;
	if (!list_empty_careful(&wait->task_list))
		return false;

	spin_lock(&this_hctx->lock);
	if (!list_empty(&wait->task_list)) {
		spin_unlock(&this_hctx->lock);
		return false;
	}

	ws = bt_wait_ptr(&this_hctx->tags->bitmap_tags, this_hctx);
	add_wait_queue(&ws->wait, wait);

	/*
	 * It's possible that a tag was freed in the window between the
	 * allocation failure and adding the hardware queue to the wait
	 * queue.
	 */
	//blk_mq_get_driver_tag里获取tag失败就会休眠
	ret = blk_mq_get_driver_tag(rq, hctx, false);

	if (!ret) {
		spin_unlock(&this_hctx->lock);
		return false;
	}

	/*
	 * We got a tag, remove ourselves from the wait queue to ensure
	 * someone else gets the wakeup.
	 */
	spin_lock_irq(&ws->wait.lock);
	list_del_init(&wait->task_list);
	spin_unlock_irq(&ws->wait.lock);
	spin_unlock(&this_hctx->lock);

	return true;
}

#define BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT  8
#define BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR  4
/*
 * Update dispatch busy with the Exponential Weighted Moving Average(EWMA):
 * - EWMA is one simple way to compute running average value
 * - weight(7/8 and 1/8) is applied so that it can decrease exponentially
 * - take 4 as factor for avoiding to get too small(0) result, and this
 *   factor doesn't matter because EWMA decreases exponentially
 */
//__blk_mq_issue_directly()启动req硬件队列派发后，busy为true执行该函数设置硬件队列繁忙，busy为false应该是不繁忙
static void blk_mq_update_dispatch_busy(struct blk_mq_hw_ctx *hctx, bool busy)
{
	unsigned int ewma;

	if (hctx->queue->elevator)
		return;

	ewma = hctx->dispatch_busy;

	if (!ewma && !busy)
		return;

	ewma *= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT - 1;
	if (busy)
		ewma += 1 << BLK_MQ_DISPATCH_BUSY_EWMA_FACTOR;
	ewma /= BLK_MQ_DISPATCH_BUSY_EWMA_WEIGHT;

	hctx->dispatch_busy = ewma;
}

#define BLK_MQ_RESOURCE_DELAY	3		/* ms units */

/*
 * Returns true if we did some work AND can potentially do more.
 */
//list来自hctx->dispatch硬件派发队列、软件队列rq_list链表上等req。遍历list上的req，先给req在硬件队列hctx的blk_mq_tags里分配一个空闲tag，
//然后调用磁盘驱动queue_rq函数派发req。任一个req要启动硬件传输前，都要从blk_mq_tags结构里得到一个空闲的tag。
//如果遇到磁盘驱动硬件繁忙，还要把list剩余的req转移到hctx->dispatch队列，然后启动异步传输.下发给驱动的req成功减失败总个数不为0返回true
bool blk_mq_dispatch_rq_list(struct request_queue *q, struct list_head *list,
			     bool got_budget)//list来自hctx->dispatch硬件派发队列或者其他待派发的队列
{
	struct blk_mq_hw_ctx *hctx;
	bool no_tag = false;
	struct request *rq, *nxt;
	LIST_HEAD(driver_list);
	struct list_head *dptr;
	int errors, queued, ret = BLK_MQ_RQ_QUEUE_OK;

	if (list_empty(list))
		return false;

	WARN_ON(!list_is_singular(list) && got_budget);

	/*
	 * Start off with dptr being NULL, so we start the first request
	 * immediately, even if we have more pending.
	 */
	dptr = NULL;

	/*
	 * Now process all the entries, sending them to the driver.
	 */
	errors = queued = 0;
	do {
		struct blk_mq_queue_data bd;
        //从list链表取出一个req
		rq = list_first_entry(list, struct request, queuelist);
        //先根据rq->mq_ctx->cpu这个CPU编号从q->mq_map[cpu]找到硬件队列编号，再q->queue_hw_ctx[硬件队列编号]返回
        //硬件队列唯一的blk_mq_hw_ctx结构体,每个CPU都对应了唯一的硬件队列
		hctx = blk_mq_map_queue(rq->q, rq->mq_ctx->cpu);
		if (!got_budget && !blk_mq_get_dispatch_budget(hctx))
			break;

        //从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
        //hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag

		if (!blk_mq_get_driver_tag(rq, NULL, false)) {
			/*
			 * The initial allocation attempt failed, so we need to
			 * rerun the hardware queue when a tag is freed. The
			 * waitqueue takes care of that. If the queue is run
			 * before we add this entry back on the dispatch list,
			 * we'll re-run it below.
			 */
			//获取tag失败，则要尝试开始休眠了，再尝试分配，函数返回时获取tag就成功了�
			if (!blk_mq_mark_tag_wait(&hctx, rq)) {
				blk_mq_put_dispatch_budget(hctx);
				/*
				 * For non-shared tags, the RESTART check
				 * will suffice.
				 */
				//如果还是分配tag失败，但是硬件队列有共享tag标志
				if (hctx->flags & BLK_MQ_F_TAG_SHARED)
					no_tag = true;//设置no_tag标志位
                
                //直接跳出循环，不再进行req派发
				break;
			}
		}
        //从list链表剔除req
		list_del_init(&rq->queuelist);
        //bd.rq保存要传输的req
		bd.rq = rq;
		bd.list = dptr;

		/*
		 * Flag last if we have no more requests, or if we have more
		 * but can't assign a driver tag to it.
		 */
		if (list_empty(list))
			bd.last = true;//list链表空bd.last设为TRUE
		else {
            //获取链表第一个req于nxt，如果这个req获取不到tag，bd.last置为TRUE，这有啥用
			nxt = list_first_entry(list, struct request, queuelist);
			bd.last = !blk_mq_get_driver_tag(nxt, NULL, false);
		}

       //根据req设置nvme_command,把req添加到q->timeout_list，并且启动q->timeout,把新的cmd复制到nvmeq->sq_cmds[]队列。
       //真正把req派发给驱动，启动硬件nvme硬件传输
		ret = q->mq_ops->queue_rq(hctx, &bd);//nvme_queue_rq
		switch (ret) {
		case BLK_MQ_RQ_QUEUE_OK://派送成功，queued++表示传输完成的req
			queued++;
			break;
		case BLK_MQ_RQ_QUEUE_BUSY:
		case BLK_MQ_RQ_QUEUE_DEV_BUSY:
			/*
			 * If an I/O scheduler has been configured and we got a
			 * driver tag for the next request already, free it again.
			 */
			if (!list_empty(list)) {
				nxt = list_first_entry(list, struct request, queuelist);
				blk_mq_put_driver_tag(nxt);
			}
            //磁盘驱动硬件繁忙，要把req再添加到list链表
			list_add(&rq->queuelist, list);
            //tags->bitmap_tags中按照req->tag把req的tag编号释放掉,与blk_mq_get_driver_tag()获取tag相反
			__blk_mq_requeue_request(rq);
			break;
		default:
			pr_err("blk-mq: bad return on queue: %d\n", ret);
		case BLK_MQ_RQ_QUEUE_ERROR:
			errors++;//下发给驱动时出错errors加1，这种情况一般不会有吧，除非磁盘硬件有问题了
			rq->errors = -EIO;
			blk_mq_end_request(rq, rq->errors);
			break;
		}

        //如果磁盘驱动硬件繁忙，break跳出do...while循环
		if (ret == BLK_MQ_RQ_QUEUE_BUSY || ret == BLK_MQ_RQ_QUEUE_DEV_BUSY)
			break;

		/*
		 * We've done the first request. If we have more than 1
		 * left in the list, set dptr to defer issue.
		 */
		if (!dptr && list->next != list->prev)
			dptr = &driver_list;
	}
    while (!list_empty(list));

    //这是什么操作?????传输完成一个req加1??????
	hctx->dispatched[queued_to_index(queued)]++;

	/*
	 * Any items that need requeuing? Stuff them into hctx->dispatch,
	 * that is where we will continue on next queue run.
	 */
    //list链表不空，说明磁盘驱动硬件繁忙，有部分req没有派送给驱动
	if (!list_empty(list)) {
		bool needs_restart;

		spin_lock(&hctx->lock);
        //这里是把list链表上没有派送给驱动的的req再移动到hctx->dispatch链表!!!!!!!!!!!!!!!!!!!!
		list_splice_init(list, &hctx->dispatch);
		spin_unlock(&hctx->lock);

		/*
		 * the queue is expected stopped with BLK_MQ_RQ_QUEUE_BUSY, but
		 * it's possible the queue is stopped and restarted again
		 * before this. Queue restart will dispatch requests. And since
		 * requests in rq_list aren't added into hctx->dispatch yet,
		 * the requests in rq_list might get lost.
		 *
		 * blk_mq_run_hw_queue() already checks the STOPPED bit
		 *
		 * If RESTART or TAG_WAITING is set, then let completion restart
		 * the queue instead of potentially looping here.
		 *
		 * If 'no_tag' is set, that means that we failed getting
		 * a driver tag with an I/O scheduler attached. If our dispatch
		 * waitqueue is no longer active, ensure that we run the queue
		 * AFTER adding our entries back to the list.
		 *
		 * If driver returns BLK_MQ_RQ_QUEUE_BUSY and SCHED_RESTART
		 * bit is set, run queue after a delay to avoid IO stalls
		 * that could otherwise occur if the queue is idle.
		 */

        /*因为硬件队列繁忙没有把hctx->dispatch上的req全部派送给驱动，则下边就再执行一次blk_mq_run_hw_queue()或者
         blk_mq_delay_run_hw_queue()，再进行一次异步派发，就那几招，一个套路*/
        
		//测试hctx->state是否设置了BLK_MQ_S_SCHED_RESTART位，blk_mq_sched_dispatch_requests()就会设置这个标志位
		needs_restart = blk_mq_sched_needs_restart(hctx);
		if (!needs_restart ||(no_tag && list_empty_careful(&hctx->dispatch_wait.task_list)))
		    //再次调用blk_mq_run_hw_queue()启动异步req派发true表示允许异步
			blk_mq_run_hw_queue(hctx, true);
        
	    //如果设置了BLK_MQ_S_SCHED_RESTART标志位，并且硬件队列繁忙导致了部分req没有来得及传输完
		else if (needs_restart && (ret == BLK_MQ_RQ_QUEUE_BUSY))
            //再次调用blk_mq_delay_run_hw_queue，但这次是异步传输，即开启kblockd_workqueue内核线程传输
			blk_mq_delay_run_hw_queue(hctx, BLK_MQ_RESOURCE_DELAY);
        
        //更新hctx->dispatch_busy，设置硬件队列繁忙
		blk_mq_update_dispatch_busy(hctx, true);

        //返回false，说明硬件队列繁忙
		return false;
	}
    else
		blk_mq_update_dispatch_busy(hctx, false);//设置硬件队列不忙

	/*
	 * If the host/device is unable to accept more work, inform the
	 * caller of that.
	 */
	if (ret == BLK_MQ_RQ_QUEUE_BUSY || ret == BLK_MQ_RQ_QUEUE_DEV_BUSY)
		return false;//返回false表示硬件队列忙

    //queued表示成功派发给驱动的req个数，errors表示下发给驱动时出错的req个数，二者加起来不为0才返回非。
    //下发给驱动的req成功减失败总个数不为0返回true
	return (queued + errors) != 0;
}

static void __blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	int srcu_idx;

	WARN_ON(!cpumask_test_cpu(raw_smp_processor_id(), hctx->cpumask) &&
		cpu_online(hctx->next_cpu));

	might_sleep_if(hctx->flags & BLK_MQ_F_BLOCKING);

    //上硬件队列锁，这时如果是同一个硬件队列，就有锁抢占了
	hctx_lock(hctx, &srcu_idx);
//各种各样场景的req派发，hctx->dispatch硬件队列dispatch链表上的req派发;有deadline调度算法时红黑树或者fifo调度队列上的req派发，
//无IO调度算法时，硬件队列关联的所有软件队列ctx->rq_list上的req的派发等等。派发过程应该都是调用blk_mq_dispatch_rq_list()，
//磁盘驱动硬件不忙直接启动req传输，繁忙的话则把剩余的req转移到hctx->dispatch队列，然后启动nvme异步传输
	blk_mq_sched_dispatch_requests(hctx);
	hctx_unlock(hctx, srcu_idx);
}

/*
 * It'd be great if the workqueue API had a way to pass
 * in a mask and had some smarts for more clever placement.
 * For now we just round-robin here, switching for every
 * BLK_MQ_CPU_WORK_BATCH queued items.
 */
static int blk_mq_hctx_next_cpu(struct blk_mq_hw_ctx *hctx)
{
	if (hctx->queue->nr_hw_queues == 1)
		return WORK_CPU_UNBOUND;

	if (--hctx->next_cpu_batch <= 0) {
		int next_cpu;

		next_cpu = cpumask_next(hctx->next_cpu, hctx->cpumask);
		if (next_cpu >= nr_cpu_ids)
			next_cpu = cpumask_first(hctx->cpumask);

		hctx->next_cpu = next_cpu;
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}

	return hctx->next_cpu;
}

static void __blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async,//async为true表示异步传输，false表示同步
					unsigned long msecs)//msecs决定派发延时
{
	if (unlikely(blk_mq_hctx_stopped(hctx) ||
		     !blk_mq_hw_queue_mapped(hctx)))
		return;
    //同步传输
	if (!async && !(hctx->flags & BLK_MQ_F_BLOCKING)) {
		int cpu = get_cpu();
		if (cpumask_test_cpu(cpu, hctx->cpumask)) {
//各种各样场景的req派发，hctx->dispatch硬件队列dispatch链表上的req派发;有deadline调度算法时红黑树或者fifo调度队列上的req派发;
//无IO调度器时，硬件队列关联的所有软件队列ctx->rq_list上的req的派发等等。派发过程应该都是调用blk_mq_dispatch_rq_list()，
//磁盘驱动硬件不忙直接启动req传输，繁忙的话则把剩余的req转移到hctx->dispatch队列，然后启动异步传输
			__blk_mq_run_hw_queue(hctx);
			put_cpu();
			return;
		}

		put_cpu();
	}
    //显然这是启动异步传输，开启kblockd_workqueue内核线程workqueue，异步执行hctx->run_work对应的work函数blk_mq_run_work_fn
    //实际blk_mq_run_work_fn里执行的还是__blk_mq_run_hw_queue，估计是延迟msecs时间再执行一遍__blk_mq_run_hw_queue，nvme硬件
    //队列就不再繁忙了吧??????万一此时还是繁忙怎么办???????是否有可能这种场景下还是会nvme硬件队列繁忙?????????????????????
	kblockd_mod_delayed_work_on(blk_mq_hctx_next_cpu(hctx), &hctx->run_work,
				    msecs_to_jiffies(msecs));
}

void blk_mq_delay_run_hw_queue(struct blk_mq_hw_ctx *hctx, unsigned long msecs)
{
	__blk_mq_delay_run_hw_queue(hctx, true, msecs);
}
EXPORT_SYMBOL(blk_mq_delay_run_hw_queue);

//启动硬件队列上的req派发到块设备驱动
bool blk_mq_run_hw_queue(struct blk_mq_hw_ctx *hctx, bool async)//async为true表示异步传输，false表示同步
{
	int srcu_idx;
	bool need_run;

	/*
	 * When queue is quiesced, we may be switching io scheduler, or
	 * updating nr_hw_queues, or other things, and we can't run queue
	 * any more, even __blk_mq_hctx_has_pending() can't be called safely.
	 *
	 * And queue will be rerun in blk_mq_unquiesce_queue() if it is
	 * quiesced.
	 */
	hctx_lock(hctx, &srcu_idx);
	need_run = !blk_queue_quiesced(hctx->queue) &&
		blk_mq_hctx_has_pending(hctx);
	hctx_unlock(hctx, srcu_idx);

    //有req需要硬件传输
	if (need_run) {
		__blk_mq_delay_run_hw_queue(hctx, async, 0);
		return true;
	}

	return false;
}
EXPORT_SYMBOL(blk_mq_run_hw_queue);

void blk_mq_run_hw_queues(struct request_queue *q, bool async)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (blk_mq_hctx_stopped(hctx))
			continue;

		blk_mq_run_hw_queue(hctx, async);
	}
}
EXPORT_SYMBOL(blk_mq_run_hw_queues);

/**
 * blk_mq_queue_stopped() - check whether one or more hctxs have been stopped
 * @q: request queue.
 *
 * The caller is responsible for serializing this function against
 * blk_mq_{start,stop}_hw_queue().
 */
bool blk_mq_queue_stopped(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i)
		if (blk_mq_hctx_stopped(hctx))
			return true;

	return false;
}
EXPORT_SYMBOL(blk_mq_queue_stopped);

/*
 * This function is often used for pausing .queue_rq() by driver when
 * there isn't enough resource or some conditions aren't satisfied, and
 * BLK_MQ_RQ_QUEUE_BUSY is usually returned.
 *
 * We do not guarantee that dispatch can be drained or blocked
 * after blk_mq_stop_hw_queue() returns. Please use
 * blk_mq_quiesce_queue() for that requirement.
 */
void blk_mq_stop_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	cancel_delayed_work(&hctx->run_work);
	cancel_delayed_work(&hctx->delay_work);
	set_bit(BLK_MQ_S_STOPPED, &hctx->state);
}
EXPORT_SYMBOL(blk_mq_stop_hw_queue);

/*
 * This function is often used for pausing .queue_rq() by driver when
 * there isn't enough resource or some conditions aren't satisfied, and
 * BLK_MQ_RQ_QUEUE_BUSY is usually returned.
 *
 * We do not guarantee that dispatch can be drained or blocked
 * after blk_mq_stop_hw_queues() returns. Please use
 * blk_mq_quiesce_queue() for that requirement.
 */
void blk_mq_stop_hw_queues(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_stop_hw_queue(hctx);
}
EXPORT_SYMBOL(blk_mq_stop_hw_queues);

void blk_mq_start_hw_queue(struct blk_mq_hw_ctx *hctx)
{
	clear_bit(BLK_MQ_S_STOPPED, &hctx->state);

	blk_mq_run_hw_queue(hctx, false);
}
EXPORT_SYMBOL(blk_mq_start_hw_queue);

void blk_mq_start_hw_queues(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i)
		blk_mq_start_hw_queue(hctx);
}
EXPORT_SYMBOL(blk_mq_start_hw_queues);

void blk_mq_start_stopped_hw_queues(struct request_queue *q, bool async)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (!blk_mq_hctx_stopped(hctx))
			continue;

		clear_bit(BLK_MQ_S_STOPPED, &hctx->state);
		blk_mq_run_hw_queue(hctx, async);
	}
}
EXPORT_SYMBOL(blk_mq_start_stopped_hw_queues);

static void blk_mq_run_work_fn(struct work_struct *work)
{
	struct blk_mq_hw_ctx *hctx;

	hctx = container_of(work, struct blk_mq_hw_ctx, run_work.work);
//各种各样场景的req派发，hctx->dispatch硬件队列dispatch链表上的req派发;有deadline调度算法时红黑树或者fifo调度队列上的req派发，
//无IO调度算法时，硬件队列关联的所有软件队列ctx->rq_list上的req的派发等等。派发过程应该都是调用blk_mq_dispatch_rq_list()，
//nvme硬件队列不忙直接启动req传输，繁忙的话则把剩余的req转移到hctx->dispatch队列，然后启动nvme异步传输
	__blk_mq_run_hw_queue(hctx);
}

static void blk_mq_delay_work_fn(struct work_struct *work)
{
	struct blk_mq_hw_ctx *hctx;

	hctx = container_of(work, struct blk_mq_hw_ctx, delay_work.work);

	if (test_and_clear_bit(BLK_MQ_S_STOPPED, &hctx->state))
		__blk_mq_run_hw_queue(hctx);
}
//把req插入到软件队列ctx->rq_list链表
static inline void __blk_mq_insert_req_list(struct blk_mq_hw_ctx *hctx,
					    struct request *rq,
					    bool at_head)
{
	struct blk_mq_ctx *ctx = rq->mq_ctx;

	trace_block_rq_insert(hctx->queue, rq);

	if (at_head)
		list_add(&rq->queuelist, &ctx->rq_list);
	else
		list_add_tail(&rq->queuelist, &ctx->rq_list);
}
//把req插入到软件队列ctx->rq_list链表,对应的硬件队列hctx->ctx_map里的bit位被置1，表示激活
void __blk_mq_insert_request(struct blk_mq_hw_ctx *hctx, struct request *rq,
			     bool at_head)
{
	struct blk_mq_ctx *ctx = rq->mq_ctx;
    //把req插入到软件队列ctx->rq_list链表
	__blk_mq_insert_req_list(hctx, rq, at_head);
    //该软件队列有req了，对应的硬件队列hctx->ctx_map里的bit位被置1，表示激活
	blk_mq_hctx_mark_pending(hctx, ctx);
}

/*
 * Should only be used carefully, when the caller knows we want to
 * bypass a potential IO scheduler on the target device.
 */
//把req添加到硬件队列hctx->dispatch队列，如果run_queue为true，则同步启动req硬件派发
void blk_mq_request_bypass_insert(struct request *rq, bool run_queue)
{
	struct blk_mq_ctx *ctx = rq->mq_ctx;
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(rq->q, ctx->cpu);

	spin_lock(&hctx->lock);
    //把req添加到硬件队列hctx->dispatch队列
	list_add_tail(&rq->queuelist, &hctx->dispatch);
	spin_unlock(&hctx->lock);

	if (run_queue)//启动req派发
		blk_mq_run_hw_queue(hctx, false);
}
//把list链表的成员插入到到ctx->rq_list链表后边，然后对list清0，这个list链表源自当前进程的plug链表。每一个req在分配时，
//req->mq_ctx会指向当前CPU的软件队列，但是真正把req插入到软件队列，看着得执行blk_mq_insert_requests才行呀
void blk_mq_insert_requests(struct blk_mq_hw_ctx *hctx, struct blk_mq_ctx *ctx,
			    struct list_head *list)

{
	struct request *rq;

	/*
	 * preemption doesn't flush plug list, so it's possible ctx->cpu is
	 * offline now
	 */
	list_for_each_entry(rq, list, queuelist) {
		BUG_ON(rq->mq_ctx != ctx);
		trace_block_rq_insert(hctx->queue, rq);
	}

	spin_lock(&ctx->lock);
    //把list链表的成员插入到到ctx->rq_list链表后边，然后对list清0，这个list链表源自当前进程的plug链表
	list_splice_tail_init(list, &ctx->rq_list);
    //该软件队列有req了，对应的硬件队列hctx->ctx_map里的bit位被置1，表示激活
	blk_mq_hctx_mark_pending(hctx, ctx);
	spin_unlock(&ctx->lock);
}
//a<b返回0
static int plug_ctx_cmp(void *priv, struct list_head *a, struct list_head *b)
{
	struct request *rqa = container_of(a, struct request, queuelist);
	struct request *rqb = container_of(b, struct request, queuelist);

	return !(rqa->mq_ctx < rqb->mq_ctx ||
		 (rqa->mq_ctx == rqb->mq_ctx &&
		  blk_rq_pos(rqa) < blk_rq_pos(rqb)));
}
/*每次循环，取出plug->mq_list上的req，添加到ctx_list局部链表。如果每两次取出的req都属于一个软件队列，只是把这些req添加到局部ctx_list
链表，最后执行blk_mq_sched_insert_requests把ctx_list链表上的req进行派发。如果前后两次取出的req不属于一个软件队列，则立即执行
blk_mq_sched_insert_requests()将ctx_list链表已经保存的req进行派发，然后把本次循环取出的req继续添加到ctx_list局部链表。简单来说，
blk_mq_sched_insert_requests()只会派发同一个软件队列上的req。blk_mq_sched_insert_requests()函数req的派发，如果有调度器，则把req先插入
到IO算法队列，如果无调度器，会尝试执行blk_mq_try_issue_list_directly直接派发req。最后再执行blk_mq_run_hw_queue()把剩余的req再次派发。*/
void blk_mq_flush_plug_list(struct blk_plug *plug, bool from_schedule)
{
	struct blk_mq_ctx *this_ctx;
	struct request_queue *this_q;
	struct request *rq;
	LIST_HEAD(list);
	LIST_HEAD(ctx_list);//ctx_list临时保存了当前进程plug->mq_list链表上的部分req
	unsigned int depth;
    //就是令list指向plug->mq_list的吧
	list_splice_init(&plug->mq_list, &list);
    //对plug->mq_list链表上的req进行排序吧，排序规则基于req的扇区起始地址
	list_sort(NULL, &list, plug_ctx_cmp);

	this_q = NULL;
	this_ctx = NULL;
	depth = 0;

    //循环直到plug->mq_list链表上的req空
	while (!list_empty(&list)) {
        //plug->mq_list取一个req
		rq = list_entry_rq(list.next);
        //从链表删除req
		list_del_init(&rq->queuelist);
		BUG_ON(!rq->q);
        
        
        //this_ctx是上一个req的软件队列，rq->mq_ctx是当前req的软件队列。二者软件队列相等则if不成立，只是把req添加到局部ctx_list链表
        //如果二者软件队列不等，则执行if里边的blk_mq_sched_insert_requests把局部ctx_list链表上的req进行派送。
        //然后把局部ctx_list链表清空，重复上述循环。

        //第一次循环肯定成立，req在分配后就会初始化指向当前CPU的软件队列。
        if (rq->mq_ctx != this_ctx) {//this_ctx都是上一次循环取出的req的
            
			if (this_ctx) {//第二次循环开始才成立
				trace_block_unplug(this_q, depth, from_schedule);
                
//如果有IO调度算法，则把ctx_list(来自plug->mq_list)链表上的req插入elv的hash队列，mq-deadline算法的还要插入红黑树和fifo队列。
//如果没有IO调度算法，取出plug->mq_list链表的上的req，从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags
//分配一个空闲tag赋于req->tag，然后调用磁盘驱动queue_rq接口函数把req派发给驱动。如果遇到磁盘驱动硬件忙，则设置硬件队列忙，
//还释放req的tag，然后把这个失败派送的req插入hctx->dispatch链表,如果此时list链表空则同步派发。最后把把ctx_list
//链表的上剩余的req插入到软件队列ctx->rq_list链表上，然后执行blk_mq_run_hw_queue()再进行req派发。

				blk_mq_sched_insert_requests(this_q, this_ctx,//this_q和this_ctx都是上一次循环取出的req的
								&ctx_list,//ctx_list临时保存了当前进程plug->mq_list链表上的部分req
								from_schedule);//from_schedule从blk_finish_plug和blk_mq_make_request过来的是false
			}
            //this_ctx赋值为req软件队列，何理?
			this_ctx = rq->mq_ctx;
			this_q = rq->q;
            //遇到不同软件队列的req，depth清0
			depth = 0;
		}

		depth++;
        //把req添加到局部变量ctx_list链表，看着是向ctx_list插入一个req，depth深度就加1
		list_add_tail(&rq->queuelist, &ctx_list);
	}

	/*
	 * If 'this_ctx' is set, we know we have entries to complete
	 * on 'ctx_list'. Do those.
	 */
	//如果plug->mq_list上的req，rq->mq_ctx都指向同一个软件队列，前边的blk_mq_sched_insert_requests执行不了，则在这里执行一次，将
	//ctx_list链表上的req进行派发。还有一种情况，是plug->mq_list链表上的最后一个req也只能在这里派发。
	if (this_ctx) {
		trace_block_unplug(this_q, depth, from_schedule);
		blk_mq_sched_insert_requests(this_q, this_ctx, &ctx_list,
						from_schedule);
	}
}

//赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，统计磁盘使用率等数据
static void blk_mq_bio_to_request(struct request *rq, struct bio *bio)
{
    //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio
	init_request_from_bio(rq, bio);

	if (blk_do_io_stat(rq))
		blk_account_io_start(rq, true);//统计磁盘使用率等数据
}

static inline bool hctx_allow_merges(struct blk_mq_hw_ctx *hctx)
{
	return (hctx->flags & BLK_MQ_F_SHOULD_MERGE) &&
		!blk_queue_nomerges(hctx->queue);
}

/* attempt to merge bio into current sw queue */
static inline bool blk_mq_merge_bio(struct request_queue *q, struct bio *bio)
{
	bool ret = false;
    //根据进程当前所属CPU获取软件队列
	struct blk_mq_ctx *ctx = blk_mq_get_ctx(q);
    //获取软件队列关联的硬件队列
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(q, ctx->cpu);

	if (hctx_allow_merges(hctx) && bio_mergeable(bio) &&
			!list_empty_careful(&ctx->rq_list)) {
	    //这是软件队列锁，每个CPU独有，多进程读写文件，避免多核竞争锁
		spin_lock(&ctx->lock);
		ret = blk_mq_attempt_merge(q, ctx, bio);
		spin_unlock(&ctx->lock);
	}

	blk_mq_put_ctx(ctx);
	return ret;
}

static inline void blk_mq_queue_io(struct blk_mq_hw_ctx *hctx,
				   struct blk_mq_ctx *ctx,
				   struct request *rq)
{
	spin_lock(&ctx->lock);
    //把req插入到软件队列ctx->rq_list链表
	__blk_mq_insert_request(hctx, rq, false);
	spin_unlock(&ctx->lock);
}

static int __blk_mq_issue_directly(struct blk_mq_hw_ctx *hctx, struct request *rq)
{//req来自当前进程plug->mq_list链表，有时是刚分配的新req
	struct request_queue *q = rq->q;
	struct blk_mq_queue_data bd = {
		.rq = rq,
		.list = NULL,
		.last = true,
	};
	int ret;

	/*
	 * For OK queue, we are done. For error, caller may kill it.
	 * Any other error (busy), just add it to our list as we
	 * previously would have done.
	 */
	//根据req设置磁盘驱动 command,把req添加到q->timeout_list，并且启动q->timeout,把command复制到nvmeq->sq_cmds[]队列
	ret = q->mq_ops->queue_rq(hctx, &bd);//nvme_queue_rq
	switch (ret) {
	case BLK_MQ_RQ_QUEUE_OK:
		blk_mq_update_dispatch_busy(hctx, false);//设置硬件队列不忙，看着就hctx->dispatch_busy = ewma
		break;
	case BLK_MQ_RQ_QUEUE_BUSY:
	case BLK_MQ_RQ_QUEUE_DEV_BUSY:
		blk_mq_update_dispatch_busy(hctx, true);//设置硬件队列忙
		//硬件队列繁忙，则从tags->bitmap_tags或者breserved_tags中按照req->tag这个tag编号释放tag
		__blk_mq_requeue_request(rq);
		break;
	default:
		blk_mq_update_dispatch_busy(hctx, false);
		break;
	}

	return ret;
}
//从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
//hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环,
//直到分配req成功。然后调用磁盘驱动queue_rq接口函数向驱动派发req:根据req设置磁盘驱动 command，启动q->timeout定时器等等,
//这看着是req直接发给磁盘硬件传输了。如果遇到磁盘驱动硬件忙，则释放req的tag,设置硬件队列忙.

//如果执行blk_mq_get_driver_tag分配不到tag，则执行blk_mq_request_bypass_insert.把req添加到硬件队列hctx->dispatch队列，间接启动req硬件派发.
static int __blk_mq_try_issue_directly(struct blk_mq_hw_ctx *hctx,
						struct request *rq,//req来自当前进程plug->mq_list链表，有时是刚分配的新req
						bool bypass_insert)
{
	struct request_queue *q = rq->q;
	bool run_queue = true;

	/*
	 * RCU or SRCU read lock is needed before checking quiesced flag.
	 *
	 * When queue is stopped or quiesced, ignore 'bypass_insert' from
	 * blk_mq_request_issue_directly(), and return BLK_STS_OK to caller,
	 * and avoid driver to try to dispatch again.
	 */
	if (blk_mq_hctx_stopped(hctx) || blk_queue_quiesced(q)) {
		run_queue = false;
		bypass_insert = false;
		goto insert;
	}

	if (q->elevator && !bypass_insert)
		goto insert;

	if (!blk_mq_get_dispatch_budget(hctx))
		goto insert;

    //从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
    //hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环。

    /*有个很大疑问，blk_mq_make_request->blk_mq_sched_get_request()时每个bio转成req时，分配的req是必然有一个tag对应的，为什么这里启动
    req派发时，还要再为req获取一个tag?这是什么道理???分析见blk_mq_get_tag()*/
	if (!blk_mq_get_driver_tag(rq, NULL, false)) {//大部分情况if不会成立
		blk_mq_put_dispatch_budget(hctx);//没啥实质操作
		goto insert;
	}
    
    //调用磁盘驱动queue_rq接口函数，根据req设置command,把req添加到q->timeout_list，并且启动q->timeout定时器,把新的command复制到
    //sq_cmds[]命令队列，这看着是req直接发给磁盘驱动进行数据传输了。如果遇到磁盘驱动硬件忙，则设置硬件队列忙，还释放req的tag。
	return __blk_mq_issue_directly(hctx, rq);
    
insert:
	if (bypass_insert)
		return BLK_MQ_RQ_QUEUE_BUSY;
    
//这里一般应该执行不到
    //执行这个函数，说明req没有直接发送给磁盘驱动硬件硬件。
    //把req添加到硬件队列hctx->dispatch队列，间接启动req硬件派发，里边会执行blk_mq_run_hw_queue()
	blk_mq_request_bypass_insert(rq, run_queue);
	return BLK_MQ_RQ_QUEUE_OK;
}
//
static void blk_mq_try_issue_directly(struct blk_mq_hw_ctx *hctx,
				      struct request *rq)
{
	int ret;
	int srcu_idx;

	might_sleep_if(hctx->flags & BLK_MQ_F_BLOCKING);
	hctx_lock(hctx, &srcu_idx);
    
  //从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
  //hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环,直到分配req成功。
  //然后调用磁盘驱动queue_rq接口函数向驱动派发req，启动磁盘数据传输。如果遇到磁盘驱动硬件忙，则释放req的tag,设置硬件队列忙.
	ret = __blk_mq_try_issue_directly(hctx, rq, false);
    //如果硬件队列忙，把req添加到硬件队列hctx->dispatch队列，间接启动req硬件派发
	if (ret == BLK_MQ_RQ_QUEUE_BUSY || ret == BLK_MQ_RQ_QUEUE_DEV_BUSY)
		blk_mq_request_bypass_insert(rq, true);
    
//req磁盘数据传输完成了，增加ios、ticks、time_in_queue、io_ticks、flight、sectors扇区数等使用计数。
//依次取出req->bio链表上所有req对应的bio,一个一个更新bio结构体成员数据，执行bio的回调函数.还更新req->__data_len和req->buffer�
	else if (ret != BLK_MQ_RQ_QUEUE_OK)
		blk_mq_end_request(rq, ret);

	hctx_unlock(hctx, srcu_idx);
}

int blk_mq_request_issue_directly(struct request *rq)//req来自当前进程plug->mq_list链表，有时是刚分配的新req
{
	int ret;
	int srcu_idx;
    //req所在的软件队列
	struct blk_mq_ctx *ctx = rq->mq_ctx;
    //与ctx->cpu这个CPU编号对应的硬件队列
	struct blk_mq_hw_ctx *hctx = blk_mq_map_queue(rq->q, ctx->cpu);

	hctx_lock(hctx, &srcu_idx);
    //从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
    //hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环。然后
    //调用磁盘驱动queue_rq接口函数，根据req设置 command，启动q->timeout定时器等等,将req直接派发磁盘硬件传输了。
    //如果遇到磁盘驱动硬件忙，则设置硬件队列忙，还释放req的tag。如果分配不到tag，则执行blk_mq_request_bypass_insert
    //把req添加到硬件队列hctx->dispatch队列，间接启动req硬件派发.

	ret = __blk_mq_try_issue_directly(hctx, rq, true);
	hctx_unlock(hctx, srcu_idx);

	return ret;
}
//依次遍历当前进程list(来自plug->mq_list链表或者其他)链表上的req，从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags
//分配一个空闲tag赋于rq->tag，调用磁盘驱动queue_rq接口函数把req派发给驱动。如果遇到磁盘驱动硬件忙，则设置硬件队列忙，还释放req的tag，
//然后把这个派送失败的req插入hctx->dispatch链表，如果此时list链表空则同步派发。如果遇到req传输完成则执行blk_mq_end_request()统计IO使用率等数据并唤醒进程
void blk_mq_try_issue_list_directly(struct blk_mq_hw_ctx *hctx,
		struct list_head *list)
{
    //list临时保存了当前进程plug->mq_list链表上的部分req,遍历该链表上的req
	while (!list_empty(list)) {
		int ret;
		struct request *rq = list_first_entry(list, struct request,
				queuelist);
        //从list链表剔除req
		list_del_init(&rq->queuelist);
        //从硬件队列的blk_mq_tags结构体的tags->bitmap_tags或者tags->nr_reserved_tags分配一个空闲tag赋于rq->tag，然后
        //hctx->tags->rqs[rq->tag] = rq，一个req必须分配一个tag才能IO传输。分配失败则启动硬件IO数据派发，之后再尝试分配tag，循环。然后
        //调用磁盘驱动queue_rq接口函数，根据req设置nvme command，启动q->timeout定时器等等,这看着是req直接发给磁盘硬件传输了。
        //如果遇到磁盘驱动硬件忙，则设置硬件队列忙，还释放req的tag。如果执行分配不到tag，则执行blk_mq_request_bypass_insert
        //把req添加到硬件队列hctx->dispatch队列，间接启动req硬件派发.
		ret = blk_mq_request_issue_directly(rq);
        //如果ret为BLK_MQ_RQ_QUEUE_OK，说明只是把req派发给磁盘驱动。如果是BLK_MQ_RQ_QUEUE_BUSY或者BLK_MQ_RQ_QUEUE_DEV_BUSY，则说明
        //遇到磁盘驱动硬件繁忙，直接break。如果req是其他值，说明这个req传输完成了，则执行blk_mq_end_request()进行IO统计。
		if (ret != BLK_MQ_RQ_QUEUE_OK) {
			if (ret == BLK_MQ_RQ_QUEUE_BUSY ||
					ret == BLK_MQ_RQ_QUEUE_DEV_BUSY) {
				///磁盘驱动硬件繁忙，把req添加到硬件队列hctx->dispatch队列，如果list链表空为true，则同步启动req硬件派发
				blk_mq_request_bypass_insert(rq,
							list_empty(list));
                //注意，磁盘驱动硬件的话，直接直接跳出循环，函数返回了
				break;
			}
        /*好神奇呀，貌似走到这里，就说明这个req硬件数据传输完成了，是的，就是，没有传输的情况，上边break跳出了。
         也就是说，上边执行的blk_mq_request_issue_directly(),是直接从当前进程plug->mq_list链表取出req，然后启动硬件传输了，
         如果执行到这里，就说明req硬件传输完成了?????不会吧，这个req经过IO合并没?经过IO调度算法的合并没?没有合并的话，岂不是效率很低*/

        //该req磁盘数据传输完成了，增加ios、ticks、time_in_queue、io_ticks、flight、sectors扇区数等使用计数。
       //依次取出req->bio链表上所有req对应的bio,一个一个更新bio结构体成员数据，执行bio的回调函数.还更新req->__data_len和req->buffer。
			blk_mq_end_request(rq, ret);
		}
	}
}
/*
submit_bio->generic_make_request->blk_mq_make_request->blk_mq_bio_to_request->blk_account_io_start->part_round_stats->part_round_stats_single
handle_irq_event_percpu->nvme_irq->nvme_process_cq->blk_mq_end_request->blk_account_io_done->part_round_stats->part_round_stats_single*/
static void blk_mq_make_request(struct request_queue *q, struct bio *bio)
{
	const int is_sync = rw_is_sync(bio->bi_rw);
	const int is_flush_fua = bio->bi_rw & (REQ_FLUSH | REQ_FUA);
	struct blk_mq_alloc_data data = { .flags = 0 };
	struct request *rq;
	unsigned int request_count = 0;
	struct blk_plug *plug;
	struct request *same_queue_rq = NULL;

	blk_queue_bounce(q, &bio);

	if (bio_integrity_enabled(bio) && bio_integrity_prep(bio)) {
		bio_endio(bio, -EIO);
		return;
	}

    //blk_queue_nomerges是判断设备队列是否支持IO合并
    /*遍历当前进程plug_list链表上的所有req，检查bio和req代表的磁盘范围是否挨着，挨着则把bio合并到req*/
    //如果遇到同一个块设备的req，则req赋值于same_queue_rq，这个赋值可能会进行多次
	if (!is_flush_fua && !blk_queue_nomerges(q) &&
	    blk_attempt_plug_merge(q, bio, &request_count, &same_queue_rq))
		return;
    
    //在IO调度器队列里查找是否有可以合并的req，找到则可以bio后项或前项合并到req，还会触发二次合并，还会对合并后的req
    //在IO调度算法队列里重新排序，这个合并跟软件队列和硬件队列没有半毛钱的关系吧
	if (blk_mq_sched_bio_merge(q, bio))
		return;
    
    /*依次遍历软件队列ctx->rq_list链表上的req，然后看req能否与bio前项或者后项合并*/
	if (blk_mq_merge_bio(q, bio))
		return;

	trace_block_getrq(q, bio, bio->bi_rw);
    
    /*从硬件队列相关的blk_mq_tags结构体的static_rqs[]数组里得到空闲的request。获取失败则启动硬件IO数据派发，
      之后再尝试从blk_mq_tags结构体的static_rqs[]数组里得到空闲的request并返回*/
	rq = blk_mq_sched_get_request(q, bio, bio->bi_rw, &data);//有调度器或者没有调度器获取req都走这里
	if (unlikely(!rq))
		return;
    
    //当前进程的blk_plug队列
	plug = current->plug;
	if (unlikely(is_flush_fua)) {//如果是flush或者fua请求
		blk_mq_put_ctx(data.ctx);
        //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，并且统计磁盘使用率等数据
		blk_mq_bio_to_request(rq, bio);

		/* bypass scheduler for flush rq */
        //将request插入到flush队列
		blk_insert_flush(rq);
		blk_mq_run_hw_queue(data.hctx, true);
	} else if (plug && q->nr_hw_queues == 1) {//如果进程使用plug链表，并且硬件队列数是1
		struct request *last = NULL;

		blk_mq_put_ctx(data.ctx);
        //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，并且统计磁盘使用率等数据
		blk_mq_bio_to_request(rq, bio);

		/*
		 * @request_count may become stale because of schedule
		 * out, so check the list again.
		 */
		if (list_empty(&plug->mq_list))
			request_count = 0;
		else if (blk_queue_nomerges(q))//不支持合并才会走这里，所以这里一般不会成立
            //统计当前进程的plug_list链表上的req数据量
			request_count = blk_plug_queued_count(q);

		if (!request_count)
			trace_block_plug(q);
		else
			last = list_entry_rq(plug->mq_list.prev);
        
        //当前进程的plug_list链表上的req数据量大于BLK_MAX_REQUEST_COUNT，执行blk_flush_plug_list强制把req刷到磁盘
        //但是由于request_count一般是0，这里一般不成立
		if (request_count >= BLK_MAX_REQUEST_COUNT || (last &&
		    blk_rq_bytes(last) >= BLK_PLUG_FLUSH_SIZE)) {
			blk_flush_plug_list(plug, false);
			trace_block_plug(q);
		}
        //否则，只是先把req添加到plug->mq_list链表上，等后续再一次性把plug->mq_list链表req向块设备驱动派发
		list_add_tail(&rq->queuelist, &plug->mq_list);
	}
    else if (plug && !blk_queue_nomerges(q)) {//如果进程使用plug链表，并且支持IO合并。多硬件队列使用plug时走这个分支
        
        //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，统计磁盘使用率等数据
		blk_mq_bio_to_request(rq, bio);

		/*
		 * We do limited plugging. If the bio can be merged, do that.
		 * Otherwise the existing request in the plug list will be
		 * issued. So the plug list will have one request at most
		 * The plug list might get flushed before this. If that happens,
		 * the plug list is empty, and same_queue_rq is invalid.
		 */
		if (list_empty(&plug->mq_list))//如果plug->mq_list上没有req，same_queue_rq清NULL
			same_queue_rq = NULL;
        
        //same_queue_rq 在上边遍历plug链表上的req时，发先是同一个块设备的req，req就会赋值于same_queue_rq，这个大概率会成立
		if (same_queue_rq)
			list_del_init(&same_queue_rq->queuelist);

        //把req插入mq_list链表
		list_add_tail(&rq->queuelist, &plug->mq_list);

		blk_mq_put_ctx(data.ctx);

		if (same_queue_rq) {
            //得到same_queue_rq这个req所处的硬件队列
			data.hctx = blk_mq_map_queue(q,
					same_queue_rq->mq_ctx->cpu);
            //将req直接派发到设备驱动
			blk_mq_try_issue_directly(data.hctx, same_queue_rq);
		}
	}
    //两个成立条件 1:硬件队列数大于1，并且是write sync操作。多硬件队列write sync且没有使用率plug的走这个分支
    //             2:没有使用调度器，并且硬件队列不忙，普通的没有使用plug、且没有使用调度器、且硬件队列不忙的submit_bio应该走这个分支
    else if ((q->nr_hw_queues > 1 && is_sync) || (!q->elevator &&
			!data.hctx->dispatch_busy)) {
		blk_mq_put_ctx(data.ctx);
        //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，并且统计磁盘使用率等数据
		blk_mq_bio_to_request(rq, bio);
		blk_mq_try_issue_directly(data.hctx, rq);//直接将req派发给驱动
		
	} else if (q->elevator) {//使用调度器的走这个分支
		blk_mq_put_ctx(data.ctx);
        //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，并且统计磁盘使用率等数据
		blk_mq_bio_to_request(rq, bio);
        //将req插入IO调度器队列，并执行blk_mq_run_hw_queue()将IO派发到块设备驱动
		blk_mq_sched_insert_request(rq, false, true, true);
        
	} else {//这里应该是，没有调用调度算法，并且没有使用plug链表
		blk_mq_put_ctx(data.ctx);
        //赋值req扇区起始地址，req结束地址，rq->bio = rq->biotail=bio，并且统计磁盘使用率等数据
		blk_mq_bio_to_request(rq, bio);
        //把req插入到软件队列ctx->rq_list链表
		blk_mq_queue_io(data.hctx, data.ctx, rq);
        //启动硬件队列上的req派发到块设备驱动
		blk_mq_run_hw_queue(data.hctx, true);
	}
}

void blk_mq_free_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags,
		     unsigned int hctx_idx)
{
	struct page *page;

	if (tags->rqs && set->ops->exit_request) {
		int i;

		for (i = 0; i < tags->nr_tags; i++) {
			struct request *rq = tags->static_rqs[i];

			if (!rq)
				continue;
			set->ops->exit_request(set, rq, hctx_idx);
			tags->static_rqs[i] = NULL;
		}
	}

	while (!list_empty(&tags->page_list)) {
		page = list_first_entry(&tags->page_list, struct page, lru);
		list_del_init(&page->lru);
		/*
		 * Remove kmemleak object previously allocated in
		 * blk_mq_init_rq_map().
		 */
		kmemleak_free(page_address(page));
		__free_pages(page, page->private);
	}
}

void blk_mq_free_rq_map(struct blk_mq_tags *tags)
{
	kfree(tags->rqs);
	tags->rqs = NULL;
	kfree(tags->static_rqs);
	tags->static_rqs = NULL;

	blk_mq_free_tags(tags);
}
//分配blk_mq_tags结构，分配设置其成员nr_reserved_tags、nr_tags、rqs、static_rqs、bitmap_tags、breserved_tags。
//主要是分配struct blk_mq_tags *tags的tags->rqs[]、tags->static_rqs[]这两个req指针数组
struct blk_mq_tags *blk_mq_alloc_rq_map(struct blk_mq_tag_set *set,
					unsigned int hctx_idx,
					unsigned int nr_tags,//nr_tags竟然是set->queue_depth
					unsigned int reserved_tags)
{
	struct blk_mq_tags *tags;
    //分配一个每个硬件队列结构独有的blk_mq_tags结构，设置其成员nr_reserved_tags和nr_tags，分配blk_mq_tags的bitmap_tags、breserved_tags结构
	tags = blk_mq_init_tags(nr_tags, reserved_tags,
				set->numa_node,
				BLK_MQ_FLAG_TO_ALLOC_POLICY(set->flags));
	if (!tags)
		return NULL;
    //分配nr_tags个struct request *指针，不是分配struct request结构，这些指针每个存储一个struct request指针吧
    //nr_tags应该就是nvme支持的最大硬件队列数吧，不是的，应该是最多的req数
	tags->rqs = kzalloc_node(nr_tags * sizeof(struct request *),
				 GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY,
				 set->numa_node);
	if (!tags->rqs) {
		blk_mq_free_tags(tags);
		return NULL;
	}
    //分配nr_tags个struct request *指针赋予static_rqs
	tags->static_rqs = kzalloc_node(nr_tags * sizeof(struct request *),
				 GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY,
				 set->numa_node);
	if (!tags->static_rqs) {
		kfree(tags->rqs);
		blk_mq_free_tags(tags);
		return NULL;
	}

	return tags;
}

static size_t order_to_size(unsigned int order)
{
	return (size_t)PAGE_SIZE << order;
}
//针对hctx_idx编号的硬件队列，分配set->queue_depth个req存于tags->static_rqs[i]。具体是分配N个page，将page的内存一片片分割成req结构大小
//然后tags->static_rqs[i]记录每一个req首地址，接着执行nvme_init_request()底层驱动初始化函数,建立request与nvme队列的关系吧
int blk_mq_alloc_rqs(struct blk_mq_tag_set *set, struct blk_mq_tags *tags,//tags来自set->tags[hctx_idx]，见__blk_mq_alloc_rq_map
		     unsigned int hctx_idx, unsigned int depth)//depth来自set->queue_depth是硬件队列的队列深度，hctx_idx是硬件队列编号
{
	unsigned int i, j, entries_per_page, max_order = 4;
	size_t rq_size, left;

	INIT_LIST_HEAD(&tags->page_list);

	/*
	 * rq_size is the size of the request plus driver payload, rounded
	 * to the cacheline size
	 */
	 //每一个req单元的大小，比实际的request结构大
	rq_size = round_up(sizeof(struct request) + set->cmd_size +
			   sizeof(struct request_aux), cache_line_size());
    //需要分配的的req占的总空间
	left = rq_size * depth;
    //就是分配depth个即set->queue_depth个req存于tags->static_rqs[i]
    //i在for循环最后有i++
	for (i = 0; i < depth; ) {
		int this_order = max_order;
		struct page *page;
		int to_do;
		void *p;

		while (this_order && left < order_to_size(this_order - 1))
			this_order--;
        //按照this_order=4分配page，分配2^4个page
		do {
			page = alloc_pages_node(set->numa_node,
				GFP_NOIO | __GFP_NOWARN | __GFP_NORETRY | __GFP_ZERO,
				this_order);
            //分配成功直接跳出了
			if (page)
				break;
            //分配失败降阶分配
			if (!this_order--)
				break;
			if (order_to_size(this_order) < rq_size)
				break;
		} while (1);

		if (!page)
			goto fail;
        //记录page大小
		page->private = this_order;
        //page加入tags->page_list链表
		list_add_tail(&page->lru, &tags->page_list);
        //p指向page首地址
		p = page_address(page);
		/*
		 * Allow kmemleak to scan these pages as they contain pointers
		 * to additional allocations like via ops->init_request().
		 */
		kmemleak_alloc(p, order_to_size(this_order), 1, GFP_NOIO);
        //分配的总page内存大小除以rq_size，rq_size是一个request集合大小，这是计算这片page内存可以容纳多少个request集合呀
		entries_per_page = order_to_size(this_order) / rq_size;//刚分配的page内存能容纳的req个数
		//取entries_per_page和(depth - i)最小者赋于to_do，depth - i表示还有多少个req没分配
		to_do = min(entries_per_page, depth - i);
        //to_do是本次分配的内存能容纳的req个数，left -= to_do * rq_size后表示还剩下的req需要的空间，下次循环继续分配
		left -= to_do * rq_size;
        //将page的内存一片片分割成request集合大小，然后tags->static_rqs保存每一个request首地址
		for (j = 0; j < to_do; j++) {
            //rq指向page内存首地址
			struct request *rq = p;
            //记录一个request内存的首地址，每一层队列深度，都对应一个request
			tags->static_rqs[i] = rq;
			if (set->ops->init_request) {//nvme_init_request
				if (set->ops->init_request(set, rq, hctx_idx,
						set->numa_node)) {
					tags->static_rqs[i] = NULL;
					goto fail;
				}
			}
            //p偏移rq_size
			p += rq_size;
            //哎，i这里也自加了，i表示的是硬件队列编号呀
			i++;
		}
	}
	return 0;

fail:
	blk_mq_free_rqs(set, tags, hctx_idx);
	return -ENOMEM;
}

/*
 * 'cpu' is going away. splice any existing rq_list entries from this
 * software queue to the hw queue dispatch list, and ensure that it
 * gets run.
 */
static int blk_mq_hctx_cpu_offline(struct blk_mq_hw_ctx *hctx, int cpu)
{
	struct blk_mq_ctx *ctx;
	LIST_HEAD(tmp);

	ctx = __blk_mq_get_ctx(hctx->queue, cpu);

	spin_lock(&ctx->lock);
	if (!list_empty(&ctx->rq_list)) {
		list_splice_init(&ctx->rq_list, &tmp);
		blk_mq_hctx_clear_pending(hctx, ctx);
	}
	spin_unlock(&ctx->lock);

	if (list_empty(&tmp))
		return NOTIFY_OK;

	spin_lock(&hctx->lock);
	list_splice_tail_init(&tmp, &hctx->dispatch);
	spin_unlock(&hctx->lock);

	blk_mq_run_hw_queue(hctx, true);
	return NOTIFY_OK;
}

static int blk_mq_hctx_notify(void *data, unsigned long action,
			      unsigned int cpu)
{
	struct blk_mq_hw_ctx *hctx = data;

	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN)
		return blk_mq_hctx_cpu_offline(hctx, cpu);

	/*
	 * In case of CPU online, tags may be reallocated
	 * in blk_mq_map_swqueue() after mapping is updated.
	 */

	return NOTIFY_OK;
}

/* hctx->ctxs will be freed in queue's release handler */
static void blk_mq_exit_hctx(struct request_queue *q,
		struct blk_mq_tag_set *set,
		struct blk_mq_hw_ctx *hctx, unsigned int hctx_idx)
{
	blk_mq_debugfs_unregister_hctx(hctx);

	if (blk_mq_hw_queue_mapped(hctx))
		blk_mq_tag_idle(hctx);

	if (set->ops->exit_request)
		set->ops->exit_request(set, hctx->fq->flush_rq, hctx_idx);

	blk_mq_sched_exit_hctx(q, hctx, hctx_idx);

	if (set->ops->exit_hctx)
		set->ops->exit_hctx(hctx, hctx_idx);

	if (hctx->flags & BLK_MQ_F_BLOCKING)
		cleanup_srcu_struct(&hctx->queue_rq_srcu);

	blk_mq_unregister_cpu_notifier(&hctx->cpu_notifier);
	blk_free_flush_queue(hctx->fq);
	sbitmap_free(&hctx->ctx_map);
}

static void blk_mq_exit_hw_queues(struct request_queue *q,
		struct blk_mq_tag_set *set, int nr_queue)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (i == nr_queue)
			break;
		blk_mq_exit_hctx(q, set, hctx, i);
	}
}

static void blk_mq_free_hw_queues(struct request_queue *q,
		struct blk_mq_tag_set *set)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned int i;

	queue_for_each_hw_ctx(q, hctx, i)
		free_cpumask_var(hctx->cpumask);
}
/* 1 为分配的struct blk_mq_hw_ctx *hctx 硬件队列结构大部分成员赋初值。
     重点是赋值hctx->tags=blk_mq_tags，即每个硬件队列唯一对应一个blk_mq_tags，blk_mq_tags来自struct blk_mq_tag_set 的成员
     struct blk_mq_tags[hctx_idx]。然后分配hctx->ctxs软件队列指针数组，注意只是指针数组!
   2 为硬件队列结构hctx->sched_tags分配blk_mq_tags，这是调度算法的tags。接着根据为这个blk_mq_tags分配q->nr_requests个request，
     存于tags->static_rqs[]，这是调度算法的blk_mq_tags的request!
*/
static int blk_mq_init_hctx(struct request_queue *q,
		struct blk_mq_tag_set *set,
		struct blk_mq_hw_ctx *hctx, unsigned hctx_idx)
{
	int node;

	node = hctx->numa_node;
	if (node == NUMA_NO_NODE)
		node = hctx->numa_node = set->numa_node;

	INIT_DELAYED_WORK(&hctx->run_work, blk_mq_run_work_fn);
	INIT_DELAYED_WORK(&hctx->delay_work, blk_mq_delay_work_fn);
	spin_lock_init(&hctx->lock);
	INIT_LIST_HEAD(&hctx->dispatch);
	hctx->queue = q;
    //硬件队列编号
	hctx->queue_num = hctx_idx;
	hctx->flags = set->flags & ~BLK_MQ_F_TAG_SHARED;

	blk_mq_init_cpu_notifier(&hctx->cpu_notifier,
					blk_mq_hctx_notify, hctx);
	blk_mq_register_cpu_notifier(&hctx->cpu_notifier);
    //赋值hctx->tags的blk_mq_tags，每个硬件队列对应一个blk_mq_tags，这个tags在__blk_mq_alloc_rq_map()中赋值
	hctx->tags = set->tags[hctx_idx];

	/*
	 * Allocate space for all possible cpus to avoid allocation at
	 * runtime
	 */
	//为每个CPU分配软件队列blk_mq_ctx指针，只是指针
	hctx->ctxs = kmalloc_node(nr_cpu_ids * sizeof(void *),
					GFP_KERNEL, node);
	if (!hctx->ctxs)
		goto unregister_cpu_notifier;

	if (sbitmap_init_node(&hctx->ctx_map, nr_cpu_ids, ilog2(8), GFP_KERNEL,
			      node))
		goto free_ctxs;

	hctx->nr_ctx = 0;

	init_waitqueue_func_entry(&hctx->dispatch_wait, blk_mq_dispatch_wake);
	INIT_LIST_HEAD(&hctx->dispatch_wait.task_list);

	if (set->ops->init_hctx &&
	    set->ops->init_hctx(hctx, set->driver_data, hctx_idx))//nvme_init_hctx
		goto free_bitmap;
    
    //为硬件队列结构hctx->sched_tags分配blk_mq_tags，一个硬件队列一个blk_mq_tags，这是调度算法的blk_mq_tags，
    //与硬件队列专属的blk_mq_tags不一样。然后根据为这个blk_mq_tags分配q->nr_requests个request，存于tags->static_rqs[]
	if (blk_mq_sched_init_hctx(q, hctx, hctx_idx))
		goto exit_hctx;

	hctx->fq = blk_alloc_flush_queue(q, hctx->numa_node, set->cmd_size +
			sizeof(struct request_aux));
	if (!hctx->fq)
		goto sched_exit_hctx;

	if (set->ops->init_request &&//nvme_init_request
	    set->ops->init_request(set, hctx->fq->flush_rq, hctx_idx,
				   node))
		goto free_fq;

	if (hctx->flags & BLK_MQ_F_BLOCKING)
		init_srcu_struct(&hctx->queue_rq_srcu);

	blk_mq_debugfs_register_hctx(q, hctx);

	return 0;

 free_fq:
	kfree(hctx->fq);
 sched_exit_hctx:
	blk_mq_sched_exit_hctx(q, hctx, hctx_idx);
 exit_hctx:
	if (set->ops->exit_hctx)
		set->ops->exit_hctx(hctx, hctx_idx);
 free_bitmap:
	sbitmap_free(&hctx->ctx_map);
 free_ctxs:
	kfree(hctx->ctxs);
 unregister_cpu_notifier:
	blk_mq_unregister_cpu_notifier(&hctx->cpu_notifier);

	return -1;
}
//依次取出每个CPU唯一的软件队列struct blk_mq_ctx *__ctx ，__ctx->cpu记录CPU编号，还根据CPU编号取出该CPU对应的硬件队列blk_mq_hw_ctx
//我感觉没有什么实质的操作!!!!!!
static void blk_mq_init_cpu_queues(struct request_queue *q,
				   unsigned int nr_hw_queues)
{
	unsigned int i;

	for_each_possible_cpu(i) {
        //软件队列，每个CPU一个
		struct blk_mq_ctx *__ctx = per_cpu_ptr(q->queue_ctx, i);
        //硬件队列
		struct blk_mq_hw_ctx *hctx;

		memset(__ctx, 0, sizeof(*__ctx));
		__ctx->cpu = i;
		spin_lock_init(&__ctx->lock);
		INIT_LIST_HEAD(&__ctx->rq_list);
        //软件队列结构blk_mq_ctx赋值运行队列
		__ctx->queue = q;

		/* If the cpu isn't online, the cpu is mapped to first hctx */
		if (!cpu_online(i))
			continue;
        
    //根据CPU编号先从q->mq_map[cpu]找到硬件队列编号，再q->queue_hw_ctx[硬件队列编号]返回硬件队列唯一的blk_mq_hw_ctx结构体
    //如果硬件队列只有一个，那总是返回0号硬件队列的blk_mq_hw_ctx，呵呵，所谓的软件硬件队列建立映射竟然只是这个!!!!!!!!
		hctx = blk_mq_map_queue(q, i);

		/*
		 * Set local node, IFF we have more than one hw queue. If
		 * not, we remain on the home node of the device
		 */
		if (nr_hw_queues > 1 && hctx->numa_node == NUMA_NO_NODE)
			hctx->numa_node = local_memory_node(cpu_to_node(i));
	}
}
//分配每个硬件队列独有的blk_mq_tags结构并初始化其成员，根据硬件队列的深度queue_depth分配对应个数的request存到tags->static_rqs[]
static bool __blk_mq_alloc_rq_map(struct blk_mq_tag_set *set, int hctx_idx)
{
	int ret = 0;
    //分配并返回硬件队列专属的blk_mq_tags结构，分配设置其成员nr_reserved_tags、nr_tags、rqs、static_rqs。主要是分配struct 
    //blk_mq_tags *tags的tags->rqs[]、tags->static_rqs[]这两个req指针数组。hctx_idx是硬件队列编号，每一个硬件队列独有一个blk_mq_tags结构
	set->tags[hctx_idx] = blk_mq_alloc_rq_map(set, hctx_idx,
					set->queue_depth, set->reserved_tags);
	if (!set->tags[hctx_idx])
		return false;


 //针对hctx_idx编号的硬件队列，分配set->queue_depth个req存于tags->static_rqs[i]。具体是分配N个page，将page的内存一片片分割成req结构大小
 //然后tags->static_rqs[i]记录每一个req首地址，接着执行nvme_init_request()底层驱动初始化函数,建立request与nvme队列的关系吧
	ret = blk_mq_alloc_rqs(set, set->tags[hctx_idx], hctx_idx,
				set->queue_depth);
	if (!ret)
		return true;

	blk_mq_free_rq_map(set->tags[hctx_idx]);
	set->tags[hctx_idx] = NULL;
	return false;
}

static void blk_mq_free_map_and_requests(struct blk_mq_tag_set *set,
					 unsigned int hctx_idx)
{
	if (set->tags[hctx_idx]) {
		blk_mq_free_rqs(set, set->tags[hctx_idx], hctx_idx);
		blk_mq_free_rq_map(set->tags[hctx_idx]);
		set->tags[hctx_idx] = NULL;
	}
}
/*1:根据CPU编号依次取出每一个软件队列，再根据CPU编号取出硬件队列struct blk_mq_hw_ctx *hctx，对硬件队列结构的hctx->ctxs[]赋值软件队列结构
**2:根据硬件队列数，依次从q->queue_hw_ctx[i]数组取出硬件队列结构体struct blk_mq_hw_ctx *hctx，然后对hctx->tags赋值blk_mq_tags结构*/
static void blk_mq_map_swqueue(struct request_queue *q,
			       const struct cpumask *online_mask)
{
	unsigned int i, hctx_idx;
	struct blk_mq_hw_ctx *hctx;
	struct blk_mq_ctx *ctx;
	struct blk_mq_tag_set *set = q->tag_set;

	/*
	 * Avoid others reading imcomplete hctx->cpumask through sysfs
	 */
	mutex_lock(&q->sysfs_lock);

    //就是根据硬件队列数，依次从hctx=q->queue_hw_ctx[i]数组取出硬件队列结构体
	queue_for_each_hw_ctx(q, hctx, i) {
		cpumask_clear(hctx->cpumask);
        //关联的软件队列个数清0??????
		hctx->nr_ctx = 0;
	}

	/*
	 * Map software to hardware queues
	 */
//根据CPU编号依次取出每一个软件队列，再根据CPU编号取出硬件队列struct blk_mq_hw_ctx *hctx，对硬件队列结构的hctx->ctxs[]赋值软件队列结构
	for_each_possible_cpu(i) {
		/* If the cpu isn't online, the cpu is mapped to first hctx */
		if (!cpumask_test_cpu(i, online_mask))
			continue;
        //根据CPU编号取出硬件队列编号
		hctx_idx = q->mq_map[i];
		/* unmapped hw queue can be remapped after CPU topo changed */
        //set->tags[hctx_idx]正常应是硬件队列blk_mq_tags结构体指针
		if (!set->tags[hctx_idx] &&
		    !__blk_mq_alloc_rq_map(set, hctx_idx)) {
			/*
			 * If tags initialization fail for some hctx,
			 * that hctx won't be brought online.  In this
			 * case, remap the current ctx to hctx[0] which
			 * is guaranteed to always have tags allocated
			 */
			q->mq_map[i] = 0;
		}
        //根据CPU编号取出每个CPU对应的软件队列结构指针struct blk_mq_ctx *ctx
		ctx = per_cpu_ptr(q->queue_ctx, i);
        //根据CPU编号取出每个CPU对应的硬件队列struct blk_mq_hw_ctx *hctx
		hctx = blk_mq_map_queue(q, i);

		cpumask_set_cpu(i, hctx->cpumask);
        //硬件队列关联的第几个软件队列。硬件队列每关联一个软件队列，都hctx->ctxs[hctx->nr_ctx++] = ctx，把软件队列结构保存到
        //hctx->ctxs[hctx->nr_ctx++]，即硬件队列结构的hctx->ctxs[]数组，而ctx->index_hw会先保存hctx->nr_ctx。
		ctx->index_hw = hctx->nr_ctx;
        //软件队列结构以hctx->nr_ctx为下标保存到hctx->ctxs[]
		hctx->ctxs[hctx->nr_ctx++] = ctx;
	}

	mutex_unlock(&q->sysfs_lock);

    //根据硬件队列数，依次从q->queue_hw_ctx[i]数组取出硬件队列结构体struct blk_mq_hw_ctx *hctx，然后对
    //hctx->tags赋值blk_mq_tags结构
	queue_for_each_hw_ctx(q, hctx, i) {
		/*
		 * If no software queues are mapped to this hardware queue,
		 * disable it and free the request entries.
		 */
		//硬件队列没有关联的软件队列
		if (!hctx->nr_ctx) {
			/* Never unmap queue 0.  We need it as a
			 * fallback in case of a new remap fails
			 * allocation
			 */
			if (i && set->tags[i])
				blk_mq_free_map_and_requests(set, i);

			hctx->tags = NULL;
			continue;
		}
        //i是硬件队列编号，这是根据硬件队列编号i从blk_mq_tag_set取出硬件队列专属的blk_mq_tags
		hctx->tags = set->tags[i];
		WARN_ON(!hctx->tags);

		/*
		 * Set the map size to the number of mapped software queues.
		 * This is more accurate and more efficient than looping
		 * over all possibly mapped software queues.
		 */
		sbitmap_resize(&hctx->ctx_map, hctx->nr_ctx);

		/*
		 * Initialize batch roundrobin counts
		 */
		hctx->next_cpu = cpumask_first(hctx->cpumask);
		hctx->next_cpu_batch = BLK_MQ_CPU_WORK_BATCH;
	}
}

/*
 * Caller needs to ensure that we're either frozen/quiesced, or that
 * the queue isn't live yet.
 */
//共享tag，设置的话，在blk_mq_dispatch_rq_list()启动req nvme硬件传输前获取tag时，即便分配不到tag也不会失败，因为共享tag
static void queue_set_hctx_shared(struct request_queue *q, bool shared)
{
	struct blk_mq_hw_ctx *hctx;
	int i;

	queue_for_each_hw_ctx(q, hctx, i) {
		if (shared) {
			if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
				atomic_inc(&q->shared_hctx_restart);
			hctx->flags |= BLK_MQ_F_TAG_SHARED;
		} else {
			if (test_bit(BLK_MQ_S_SCHED_RESTART, &hctx->state))
				atomic_dec(&q->shared_hctx_restart);
			hctx->flags &= ~BLK_MQ_F_TAG_SHARED;
		}
	}
}

static void blk_mq_update_tag_set_depth(struct blk_mq_tag_set *set,
					bool shared)
{
	struct request_queue *q;

	lockdep_assert_held(&set->tag_list_lock);

	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		blk_mq_freeze_queue(q);
		queue_set_hctx_shared(q, shared);
		blk_mq_unfreeze_queue(q);
	}
}

static void blk_mq_del_queue_tag_set(struct request_queue *q)
{
	struct blk_mq_tag_set *set = q->tag_set;

	mutex_lock(&set->tag_list_lock);
	list_del_rcu(&q->tag_set_list);
	if (list_is_singular(&set->tag_list)) {
		/* just transitioned to unshared */
		set->flags &= ~BLK_MQ_F_TAG_SHARED;
		/* update existing queue */
		blk_mq_update_tag_set_depth(set, false);
	}
	mutex_unlock(&set->tag_list_lock);
	synchronize_rcu();
	INIT_LIST_HEAD(&q->tag_set_list);
}

static void blk_mq_add_queue_tag_set(struct blk_mq_tag_set *set,
				     struct request_queue *q)
{
	q->tag_set = set;

	mutex_lock(&set->tag_list_lock);

	/* Check to see if we're transitioning to shared (from 1 to 2 queues). */
	if (!list_empty(&set->tag_list) && !(set->flags & BLK_MQ_F_TAG_SHARED)) {
		set->flags |= BLK_MQ_F_TAG_SHARED;
		/* update existing queue */
		blk_mq_update_tag_set_depth(set, true);
	}
    //设置共享tag
	if (set->flags & BLK_MQ_F_TAG_SHARED)
		queue_set_hctx_shared(q, true);
	list_add_tail_rcu(&q->tag_set_list, &set->tag_list);

	mutex_unlock(&set->tag_list_lock);
}

/*
 * It is the actual release handler for mq, but we do it from
 * request queue's release handler for avoiding use-after-free
 * and headache because q->mq_kobj shouldn't have been introduced,
 * but we can't group ctx/kctx kobj without it.
 */
void blk_mq_release(struct request_queue *q)
{
	struct blk_mq_hw_ctx *hctx;
	unsigned int i;

	/* hctx kobj stays in hctx */
	queue_for_each_hw_ctx(q, hctx, i) {
		if (!hctx)
			continue;
		kfree(hctx->ctxs);
		kfree(hctx);
	}

	q->mq_map = NULL;

	kfree(q->queue_hw_ctx);

	/* ctx kobj stays in queue_ctx */
	free_percpu(q->queue_ctx);
}
//块设备初始化时通过blk_mq_init_queue()创建request_queue并初始化，分配每个CPU专属的软件队列，分配硬件队列，对二者做初始化，并建立软件队列和硬件队列联系
struct request_queue *blk_mq_init_queue(struct blk_mq_tag_set *set)
{
	struct request_queue *uninit_q, *q;
    //分配struct request_queue并初始化
	uninit_q = blk_alloc_queue_node(GFP_KERNEL, set->numa_node, NULL);
	if (!uninit_q)
		return ERR_PTR(-ENOMEM);
    //分配每个CPU专属的软件队列，分配硬件队列，对二者做初始化，并建立软件队列和硬件队列联系
	q = blk_mq_init_allocated_queue(set, uninit_q);
	if (IS_ERR(q))
		blk_cleanup_queue(uninit_q);

	return q;
}
EXPORT_SYMBOL(blk_mq_init_queue);

static void blk_mq_realloc_hw_ctxs(struct blk_mq_tag_set *set,
						struct request_queue *q)
{
	int i, j;
	struct blk_mq_hw_ctx **hctxs = q->queue_hw_ctx;

	blk_mq_sysfs_unregister(q);

	/* protect against switching io scheduler  */
	mutex_lock(&q->sysfs_lock);
/* 1 循环分配每个硬件队列结构blk_mq_hw_ctx并初始化，即对每个struct blk_mq_hw_ctx *hctx硬件队列结构大部分成员赋初值。
     重点是赋值hctx->tags=blk_mq_tags，即每个硬件队列唯一对应一个blk_mq_tags，blk_mq_tags来自struct blk_mq_tag_set 的成员
     struct blk_mq_tags[hctx_idx]。然后分配hctx->ctxs软件队列指针数组，注意只是指针数组!
   2 为硬件队列结构hctx->sched_tags分配blk_mq_tags，这是调度算法的tags。接着为这个blk_mq_tags分配q->nr_requests个request，
     存于tags->static_rqs[]，这是调度算法的blk_mq_tags的request!*/     
	for (i = 0; i < set->nr_hw_queues; i++) {//为了简单起见，假设硬件队列数set->nr_hw_queues是1
		int node;

		if (hctxs[i])
			continue;
        //内存节点编号
		node = blk_mq_hw_queue_to_node(q->mq_map, i);
        //分配硬件队列结构blk_mq_hw_ctx
		hctxs[i] = kzalloc_node(sizeof(struct blk_mq_hw_ctx),
					GFP_KERNEL, node);
		if (!hctxs[i])
			break;

		if (!zalloc_cpumask_var_node(&hctxs[i]->cpumask, GFP_KERNEL,
						node)) {
			kfree(hctxs[i]);
			hctxs[i] = NULL;
			break;
		}

		atomic_set(&hctxs[i]->nr_active, 0);
		hctxs[i]->numa_node = node;
		hctxs[i]->queue_num = i;
        
        /* 1 为分配的struct blk_mq_hw_ctx *hctx 硬件队列结构大部分成员赋初值。
             重点是赋值hctx->tags=blk_mq_tags，即每个硬件队列唯一对应一个blk_mq_tags，blk_mq_tags来自struct blk_mq_tag_set 的成员
             struct blk_mq_tags[hctx_idx]。然后分配hctx->ctxs软件队列指针数组，注意只是指针数组!
           2 为硬件队列结构hctx->sched_tags分配blk_mq_tags，这是调度算法的tags。接着根据为这个blk_mq_tags分配q->nr_requests个request，
             存于tags->static_rqs[]，这是调度算法的blk_mq_tags的request!*/   
		if (blk_mq_init_hctx(q, set, hctxs[i], i)) {
			free_cpumask_var(hctxs[i]->cpumask);
			kfree(hctxs[i]);
			hctxs[i] = NULL;
			break;
		}
		blk_mq_hctx_kobj_init(hctxs[i]);
	}
    //j从i开始，释放hctx，这是什么神经逻辑??????
	for (j = i; j < q->nr_hw_queues; j++) {
		struct blk_mq_hw_ctx *hctx = hctxs[j];

		if (hctx) {
			if (hctx->tags)
				blk_mq_free_map_and_requests(set, j);
			blk_mq_exit_hctx(q, set, hctx, j);
			free_cpumask_var(hctx->cpumask);
			kobject_put(&hctx->kobj);
			kfree(hctx->ctxs);
			kfree(hctx);
			hctxs[j] = NULL;

		}
	}
    //设置硬件队列数
	q->nr_hw_queues = i;
	mutex_unlock(&q->sysfs_lock);
	blk_mq_sysfs_register(q);
}
//分配每个CPU专属的软件队列，分配硬件队列，对二者做初始化，分配，并建立软件队列和硬件队列联系
struct request_queue *blk_mq_init_allocated_queue(struct blk_mq_tag_set *set,
						  struct request_queue *q)
{
	/* mark the queue as mq asap */
	q->mq_ops = set->ops;

	q->poll_cb = blk_stat_alloc_callback(blk_mq_poll_stats_fn,
					     blk_stat_rq_ddir, 2, q);
	if (!q->poll_cb)
		goto err_exit;
    //为每个CPU分配一个软件队列struct blk_mq_ctx
	q->queue_ctx = alloc_percpu(struct blk_mq_ctx);
	if (!q->queue_ctx)
		goto err_exit;
    //分配硬件队列，这看着也是每个CPU分配一个queue_hw_ctx指针
	q->queue_hw_ctx = kzalloc_node(nr_cpu_ids * sizeof(*(q->queue_hw_ctx)),
						GFP_KERNEL, set->numa_node);
	if (!q->queue_hw_ctx)
		goto err_percpu;
    //赋值q->mq_map，这个数组保存了每个CPU对应的硬件队列编号
	q->mq_map = set->mq_map;
    
    /* 1 循环分配每个硬件队列结构blk_mq_hw_ctx并初始化，即对每个struct blk_mq_hw_ctx *hctx硬件队列结构大部分成员赋初值。
         重点是赋值hctx->tags=blk_mq_tags，即每个硬件队列唯一对应一个blk_mq_tags，blk_mq_tags来自struct blk_mq_tag_set 的成员
         struct blk_mq_tags[hctx_idx]。然后分配hctx->ctxs软件队列指针数组，注意只是指针数组!
       2 为硬件队列结构hctx->sched_tags分配blk_mq_tags，这是调度算法的tags。接着根据为这个blk_mq_tags分配q->nr_requests个request，
         存于tags->static_rqs[]，这是调度算法的blk_mq_tags的request!*/
	blk_mq_realloc_hw_ctxs(set, q);
	if (!q->nr_hw_queues)
		goto err_hctxs;

	INIT_WORK(&q->timeout_work, blk_mq_timeout_work);
	blk_queue_rq_timeout(q, set->timeout ? set->timeout : 30 * HZ);
    //q->nr_queues 看着是CPU总个数
	q->nr_queues = nr_cpu_ids;

	q->queue_flags |= QUEUE_FLAG_MQ_DEFAULT;

	if (!(set->flags & BLK_MQ_F_SG_MERGE))
		q->queue_flags |= 1 << QUEUE_FLAG_NO_SG_MERGE;

	q->sg_reserved_size = INT_MAX;

	INIT_DELAYED_WORK(&q->requeue_work, blk_mq_requeue_work);
	INIT_LIST_HEAD(&q->requeue_list);
	spin_lock_init(&q->requeue_lock);
    //就是在这里设置rq的make_request_fn为blk_mq_make_request
	blk_queue_make_request(q, blk_mq_make_request);

	/*
	 * Do this after blk_queue_make_request() overrides it...
	 */
	//nr_requests被设置为队列深度
	q->nr_requests = set->queue_depth;

    //q->softirq_done_fn设置为nvme_pci_complete_rq
	if (set->ops->complete)
		blk_queue_softirq_done(q, set->ops->complete);
    
 //依次取出每个CPU唯一的软件队列struct blk_mq_ctx *__ctx ，__ctx->cpu记录CPU编号，还根据CPU编号取出该CPU对应的硬件队列blk_mq_hw_ctx
//我感觉没有什么实质的操作!!!!!!
	blk_mq_init_cpu_queues(q, set->nr_hw_queues);

	get_online_cpus();
	mutex_lock(&all_q_mutex);

	list_add_tail(&q->all_q_node, &all_q_list);
    //共享tag设置
	blk_mq_add_queue_tag_set(set, q);
/*1:根据CPU编号依次取出每一个软件队列，再根据CPU编号取出硬件队列struct blk_mq_hw_ctx *hctx，对硬件队列结构的hctx->ctxs[]赋值软件队列结构
  2:根据硬件队列数，依次从q->queue_hw_ctx[i]数组取出硬件队列结构体struct blk_mq_hw_ctx *hctx，然后对hctx->tags赋值blk_mq_tags结构，前边
  的blk_mq_realloc_hw_ctxs()函数已经对hctx->tags赋值blk_mq_tags结构，这里又赋值，有猫腻???????????????*/
	blk_mq_map_swqueue(q, cpu_online_mask);

	mutex_unlock(&all_q_mutex);
	put_online_cpus();

	if (!(set->flags & BLK_MQ_F_NO_SCHED)) {
		int ret;
        //mq调度算法初始化
		ret = blk_mq_sched_init(q);
		if (ret)
			return ERR_PTR(ret);
	}

	return q;

err_hctxs:
	kfree(q->queue_hw_ctx);
err_percpu:
	free_percpu(q->queue_ctx);
err_exit:
	q->mq_ops = NULL;
	return ERR_PTR(-ENOMEM);
}
EXPORT_SYMBOL(blk_mq_init_allocated_queue);

void blk_mq_free_queue(struct request_queue *q)
{
	struct blk_mq_tag_set	*set = q->tag_set;

	mutex_lock(&all_q_mutex);
	list_del_init(&q->all_q_node);
	mutex_unlock(&all_q_mutex);

	blk_mq_del_queue_tag_set(q);

	blk_mq_exit_hw_queues(q, set, set->nr_hw_queues);
	blk_mq_free_hw_queues(q, set);
}

/* Basically redo blk_mq_init_queue with queue frozen */
static void blk_mq_queue_reinit(struct request_queue *q,
				const struct cpumask *online_mask)
{
	WARN_ON_ONCE(!atomic_read(&q->mq_freeze_depth));

	blk_mq_debugfs_unregister_hctxs(q);
	blk_mq_sysfs_unregister(q);

	/*
	 * redo blk_mq_init_cpu_queues and blk_mq_init_hw_queues. FIXME: maybe
	 * we should change hctx numa_node according to new topology (this
	 * involves free and re-allocate memory, worthy doing?)
	 */

	blk_mq_map_swqueue(q, online_mask);

	blk_mq_sysfs_register(q);
	blk_mq_debugfs_register_hctxs(q);
}

static void blk_mq_freeze_queue_list(struct list_head *list)
{
	struct request_queue *q;

	/*
	 * We need to freeze and reinit all existing queues.  Freezing
	 * involves synchronous wait for an RCU grace period and doing it
	 * one by one may take a long time.  Start freezing all queues in
	 * one swoop and then wait for the completions so that freezing can
	 * take place in parallel.
	 */
	list_for_each_entry(q, list, all_q_node)
		blk_freeze_queue_start(q);
	list_for_each_entry(q, list, all_q_node) {
		blk_mq_freeze_queue_wait(q);

		/*
		 * timeout handler can't touch hw queue during the
		 * reinitialization
		 */
		del_timer_sync(&q->timeout);
	}
}

/*
 * When freezing queues in blk_mq_queue_reinit_notify(), we have to freeze
 * queues in order from the list of 'all_q_list' for avoid IO deadlock:
 *
 * 1) DM queue or other queue which is at the top of usual queues, it
 * has to be frozen before the underlying queues, otherwise once the
 * underlying queue is frozen, any IO from upper layer queue can't be
 * drained up, and blk_mq_freeze_queue_wait() will wait for ever on this
 * kind of queue
 *
 * 2) NVMe admin queue is used in NVMe's reset handler, and IO queue is
 * frozen and quiesced before resetting controller, if there is any pending
 * IO before sending requests to admin queue, IO hang is caused because admin
 * queue may has been frozon, so reset can't move on, and finally
 * blk_mq_freeze_queue_wait() waits for ever on NVMe IO queue in
 * blk_mq_queue_reinit_notify(). Avoid this issue by freezing admin queue
 * after NVMe namespace queue is frozen.
 */
static void __blk_mq_freeze_all_queue_list(void)
{
	struct request_queue *q, *next;
	LIST_HEAD(front);
	LIST_HEAD(tail);

	list_for_each_entry_safe(q, next, &all_q_list, all_q_node) {
		if (q->front_queue)
			list_move(&q->all_q_node, &front);
		else if (q->tail_queue)
			list_move(&q->all_q_node, &tail);
	}

	blk_mq_freeze_queue_list(&front);
	blk_mq_freeze_queue_list(&all_q_list);
	blk_mq_freeze_queue_list(&tail);

	list_splice(&front, &all_q_list);
	list_splice_tail(&tail, &all_q_list);
}

static int blk_mq_queue_reinit_notify(struct notifier_block *nb,
				      unsigned long action, void *hcpu)
{
	struct request_queue *q;
	int cpu = (unsigned long)hcpu;
	/*
	 * New online cpumask which is going to be set in this hotplug event.
	 * Declare this cpumasks as global as cpu-hotplug operation is invoked
	 * one-by-one and dynamically allocating this could result in a failure.
	 */
	static struct cpumask online_new;

	/*
	 * Before hotadded cpu starts handling requests, new mappings must
	 * be established.  Otherwise, these requests in hw queue might
	 * never be dispatched.
	 *
	 * For example, there is a single hw queue (hctx) and two CPU queues
	 * (ctx0 for CPU0, and ctx1 for CPU1).
	 *
	 * Now CPU1 is just onlined and a request is inserted into
	 * ctx1->rq_list and set bit0 in pending bitmap as ctx1->index_hw is
	 * still zero.
	 *
	 * And then while running hw queue, blk_mq_flush_busy_ctxs() finds
	 * bit0 is set in pending bitmap and tries to retrieve requests in
	 * hctx->ctxs[0]->rq_list. But htx->ctxs[0] is a pointer to ctx0, so
	 * the request in ctx1->rq_list is ignored.
	 */
	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DEAD:
	case CPU_UP_CANCELED:
		cpumask_copy(&online_new, cpu_online_mask);
		break;
	case CPU_UP_PREPARE:
		cpumask_copy(&online_new, cpu_online_mask);
		cpumask_set_cpu(cpu, &online_new);
		break;
	default:
		return NOTIFY_OK;
	}

	mutex_lock(&all_q_mutex);

	__blk_mq_freeze_all_queue_list();

	list_for_each_entry(q, &all_q_list, all_q_node)
		blk_mq_queue_reinit(q, &online_new);

	list_for_each_entry(q, &all_q_list, all_q_node)
		blk_mq_unfreeze_queue(q);

	mutex_unlock(&all_q_mutex);
	return NOTIFY_OK;
}
//分配每个硬件队列独有的blk_mq_tags结构，根据硬件队列的深度queue_depth分配对应个数的request存到tags->static_rqs[]
static int __blk_mq_alloc_rq_maps(struct blk_mq_tag_set *set)
{
	int i;
    //又是根据硬件队列数分配设置blk_mq_tags及其nr_reserved_tags、nr_tags、rqs、static_rqs成员
    //一个硬件队列分配一次
	for (i = 0; i < set->nr_hw_queues; i++)
        //分配每个硬件队列独有的blk_mq_tags结构，根据硬件队列的深度queue_depth分配对应个数的request存到tags->static_rqs[]
		if (!__blk_mq_alloc_rq_map(set, i))
			goto out_unwind;

	return 0;

out_unwind:
	while (--i >= 0)
		blk_mq_free_rq_map(set->tags[i]);

	return -ENOMEM;
}

/*
 * Allocate the request maps associated with this tag_set. Note that this
 * may reduce the depth asked for, if memory is tight. set->queue_depth
 * will be updated to reflect the allocated depth.
 */
//分配每个硬件队列独有的blk_mq_tags结构，根据硬件队列的深度queue_depth分配对应个数的request存到tags->static_rqs[]
static int blk_mq_alloc_rq_maps(struct blk_mq_tag_set *set)
{
	unsigned int depth;
	int err;

	depth = set->queue_depth;
    //根据队列深度分配rq_maps???????????
	do {
        //分配每个硬件队列独有的blk_mq_tags结构，根据硬件队列的深度queue_depth分配对应个数的request存到tags->static_rqs[]
		err = __blk_mq_alloc_rq_maps(set);
        //注意，__blk_mq_alloc_rq_maps分配成功返回0，这里就直接break了
		if (!err)
			break;
        //每次除以2，这是什么意思?????，这是减少分配的req个数
		set->queue_depth >>= 1;
		if (set->queue_depth < set->reserved_tags + BLK_MQ_TAG_MIN) {
			err = -ENOMEM;
			break;
		}
	} while (set->queue_depth);

	if (!set->queue_depth || err) {
		pr_err("blk-mq: failed to allocate request map\n");
		return -ENOMEM;
	}

	if (depth != set->queue_depth)
		pr_info("blk-mq: reduced tag depth (%u -> %u)\n",
						depth, set->queue_depth);

	return 0;
}

static int blk_mq_update_queue_map(struct blk_mq_tag_set *set)
{
	if (set->ops->aux_ops && set->ops->aux_ops->map_queues) {
		int cpu;
		/*
		 * transport .map_queues is usually done in the following
		 * way:
		 * ----------set->nr_hw_queues 是硬件队列数，貌似一般是1，所有CPU使用一个硬件队列
		 * for (queue = 0; queue < set->nr_hw_queues; queue++) {
		 * 	mask = get_cpu_mask(queue)
		 * 	for_each_cpu(cpu, mask)
		 * 		set->mq_map[cpu] = queue;---------set->mq_map[cpu编号]=硬件队列编号
		 * }
		 *
		 * When we need to remap, the table has to be cleared for
		 * killing stale mapping since one CPU may not be mapped
		 * to any hw queue.
		 */
		for_each_possible_cpu(cpu)
			set->mq_map[cpu] = 0;//初值全是0
		return set->ops->aux_ops->map_queues(set);//应该是nvme_pci_map_queues
	} else
		return blk_mq_map_queues(set);
}

/*
 * Alloc a tag set to be associated with one or more request queues.
 * May fail with EINVAL for various error conditions. May adjust the
 * requested depth down, if if it too large. In that case, the set
 * value will be stored in set->queue_depth.
 */
int blk_mq_alloc_tag_set(struct blk_mq_tag_set *set)
{
	int ret;

	BUILD_BUG_ON(BLK_MQ_MAX_DEPTH > 1 << BLK_MQ_UNIQUE_TAG_BITS);

	if (!set->nr_hw_queues)
		return -EINVAL;
	if (!set->queue_depth)
		return -EINVAL;
	if (set->queue_depth < set->reserved_tags + BLK_MQ_TAG_MIN)
		return -EINVAL;

	if (!set->ops->queue_rq)
		return -EINVAL;

	if (set->queue_depth > BLK_MQ_MAX_DEPTH) {
		pr_info("blk-mq: reduced tag depth to %u\n",
			BLK_MQ_MAX_DEPTH);
		set->queue_depth = BLK_MQ_MAX_DEPTH;
	}

	/*
	 * If a crashdump is active, then we are potentially in a very
	 * memory constrained environment. Limit us to 1 queue and
	 * 64 tags to prevent using too much memory.
	 */
	//什么，启用了kdump，硬件队列数被强制设置为1
	if (is_kdump_kernel()) {
		set->nr_hw_queues = 1;
        //队列深度
		set->queue_depth = min(64U, set->queue_depth);
	}
	/*
	 * There is no use for more h/w queues than cpus.
	 */
	//硬件队列数大于CPU个数，
	if (set->nr_hw_queues > nr_cpu_ids)
		set->nr_hw_queues = nr_cpu_ids;

    //按照CPU个数分配struct blk_mq_tag_set需要的struct blk_mq_tags指针数组，每个CPU都有一个blk_mq_tags
	set->tags = kzalloc_node(nr_cpu_ids * sizeof(struct blk_mq_tags *),
				 GFP_KERNEL, set->numa_node);
	if (!set->tags)
		return -ENOMEM;

	ret = -ENOMEM;
    
    //分配mq_map[]指针数组，按照CPU的个数分配nr_cpu_ids个unsigned int类型数据，该数组成员对应一个CPU
	set->mq_map = kzalloc_node(sizeof(*set->mq_map) * nr_cpu_ids,
			GFP_KERNEL, set->numa_node);
	if (!set->mq_map)
		goto out_free_tags;
    //为每个set->mq_map[cpu]分配一个硬件队列编号。该数组下标是CPU的编号，数组成员是硬件队列的编号
	ret = blk_mq_update_queue_map(set);
	if (ret)
		goto out_free_mq_map;

    //分配每个硬件队列独有的blk_mq_tags结构，根据硬件队列的深度queue_depth分配对应个数的request存到tags->static_rqs[]
    //根据硬件队列数分配blk_mq_tags结构，设置及其nr_reserved_tags、nr_tags、rqs、static_rqs成员
	ret = blk_mq_alloc_rq_maps(set);
	if (ret)
		goto out_free_mq_map;

	mutex_init(&set->tag_list_lock);
	INIT_LIST_HEAD(&set->tag_list);

	return 0;

out_free_mq_map:
	kfree(set->mq_map);
	set->mq_map = NULL;
out_free_tags:
	kfree(set->tags);
	set->tags = NULL;
	return ret;
}
EXPORT_SYMBOL(blk_mq_alloc_tag_set);

void blk_mq_free_tag_set(struct blk_mq_tag_set *set)
{
	int i;

	for (i = 0; i < nr_cpu_ids; i++)
		blk_mq_free_map_and_requests(set, i);

	kfree(set->mq_map);
	set->mq_map = NULL;

	kfree(set->tags);
	set->tags = NULL;
}
EXPORT_SYMBOL(blk_mq_free_tag_set);

int blk_mq_update_nr_requests(struct request_queue *q, unsigned int nr)
{
	struct blk_mq_tag_set *set = q->tag_set;
	struct blk_mq_hw_ctx *hctx;
	int i, ret;

	if (!set)
		return -EINVAL;

	blk_mq_freeze_queue(q);
	blk_mq_quiesce_queue(q);

	ret = 0;
	queue_for_each_hw_ctx(q, hctx, i) {
		if (!hctx->tags)
			continue;
		/*
		 * If we're using an MQ scheduler, just update the scheduler
		 * queue depth. This is similar to what the old code would do.
		 */
		if (!hctx->sched_tags) {
			ret = blk_mq_tag_update_depth(hctx, &hctx->tags, nr,
							false);
		} else {
			ret = blk_mq_tag_update_depth(hctx, &hctx->sched_tags,
							nr, true);
		}
		if (ret)
			break;
	}

	if (!ret)
		q->nr_requests = nr;

	blk_mq_unquiesce_queue(q);
	blk_mq_unfreeze_queue(q);

	return ret;
}

static void __blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set,
							int nr_hw_queues)
{
	struct request_queue *q;

	lockdep_assert_held(&set->tag_list_lock);

	if (nr_hw_queues > nr_cpu_ids)
		nr_hw_queues = nr_cpu_ids;
	if (nr_hw_queues < 1 || nr_hw_queues == set->nr_hw_queues)
		return;

	list_for_each_entry(q, &set->tag_list, tag_set_list)
		blk_mq_freeze_queue(q);

	set->nr_hw_queues = nr_hw_queues;
	blk_mq_update_queue_map(set);
	list_for_each_entry(q, &set->tag_list, tag_set_list) {
		blk_mq_realloc_hw_ctxs(set, q);
		blk_mq_queue_reinit(q, cpu_online_mask);
	}

	list_for_each_entry(q, &set->tag_list, tag_set_list)
		blk_mq_unfreeze_queue(q);
}

void blk_mq_update_nr_hw_queues(struct blk_mq_tag_set *set, int nr_hw_queues)
{
	mutex_lock(&set->tag_list_lock);
	__blk_mq_update_nr_hw_queues(set, nr_hw_queues);
	mutex_unlock(&set->tag_list_lock);
}
EXPORT_SYMBOL_GPL(blk_mq_update_nr_hw_queues);

static void blk_mq_poll_stats_start(struct request_queue *q)
{
	/*
	 * We don't arm the callback if polling stats are not enabled or the
	 * callback is already active.
	 */
	if (!test_bit(QUEUE_FLAG_POLL_STATS, &q->queue_flags) ||
	    blk_stat_is_active(q->poll_cb))
		return;

	blk_stat_activate_msecs(q->poll_cb, 100);
}

static void blk_mq_poll_stats_fn(struct blk_stat_callback *cb)
{
	struct request_queue *q = cb->data;

	if (cb->stat[READ].nr_samples)
		q->poll_stat[READ] = cb->stat[READ];
	if (cb->stat[WRITE].nr_samples)
		q->poll_stat[WRITE] = cb->stat[WRITE];
}

void blk_mq_disable_hotplug(void)
{
	mutex_lock(&all_q_mutex);
}

void blk_mq_enable_hotplug(void)
{
	mutex_unlock(&all_q_mutex);
}

static int __init blk_mq_init(void)
{
	blk_mq_cpu_init();

	hotcpu_notifier(blk_mq_queue_reinit_notify, 0);

	return 0;
}
subsys_initcall(blk_mq_init);
