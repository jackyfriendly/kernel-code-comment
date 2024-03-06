/*
 *  linux/fs/namespace.c
 *
 * (C) Copyright Al Viro 2000, 2001
 *	Released under GPL v2.
 *
 * Based on code from fs/super.c, copyright Linus Torvalds and others.
 * Heavily rewritten.
 */

#include <linux/syscalls.h>
#include <linux/export.h>
#include <linux/capability.h>
#include <linux/mnt_namespace.h>
#include <linux/user_namespace.h>
#include <linux/namei.h>
#include <linux/security.h>
#include <linux/idr.h>
#include <linux/acct.h>		/* acct_auto_close_mnt */
#include <linux/ramfs.h>	/* init_rootfs */
#include <linux/fs_struct.h>	/* get_fs_root et.al. */
#include <linux/fsnotify.h>	/* fsnotify_vfsmount_delete */
#include <linux/uaccess.h>
#include <linux/proc_ns.h>
#include <linux/magic.h>
#include "pnode.h"
#include "internal.h"

#define HASH_SHIFT ilog2(PAGE_SIZE / sizeof(struct list_head))
#define HASH_SIZE (1UL << HASH_SHIFT)

static int event;
static DEFINE_IDA(mnt_id_ida);
static DEFINE_IDA(mnt_group_ida);
static DEFINE_SPINLOCK(mnt_id_lock);
static int mnt_id_start = 0;
static int mnt_group_start = 1;

//__lookup_mnt()从mount_hashtable链表搜索mount
static struct list_head *mount_hashtable __read_mostly;
static struct list_head *mountpoint_hashtable __read_mostly;
static struct kmem_cache *mnt_cache __read_mostly;
static struct rw_semaphore namespace_sem;

/* /sys/fs */
struct kobject *fs_kobj;
EXPORT_SYMBOL_GPL(fs_kobj);

/*
 * vfsmount lock may be taken for read to prevent changes to the
 * vfsmount hash, ie. during mountpoint lookups or walking back
 * up the tree.
 *
 * It should be taken for write in all cases where the vfsmount
 * tree or hash is modified or when a vfsmount structure is modified.
 */
DEFINE_BRLOCK(vfsmount_lock);

static inline unsigned long hash(struct vfsmount *mnt, struct dentry *dentry)
{
	unsigned long tmp = ((unsigned long)mnt / L1_CACHE_BYTES);
	tmp += ((unsigned long)dentry / L1_CACHE_BYTES);
	tmp = tmp + (tmp >> HASH_SHIFT);
	return tmp & (HASH_SIZE - 1);
}

#define MNT_WRITER_UNDERFLOW_LIMIT -(1<<16)

/*
 * allocation is serialized by namespace_sem, but we need the spinlock to
 * serialize with freeing.
 */
static int mnt_alloc_id(struct mount *mnt)
{
	int res;

retry:
	ida_pre_get(&mnt_id_ida, GFP_KERNEL);
	spin_lock(&mnt_id_lock);
	res = ida_get_new_above(&mnt_id_ida, mnt_id_start, &mnt->mnt_id);
	if (!res)
		mnt_id_start = mnt->mnt_id + 1;
	spin_unlock(&mnt_id_lock);
	if (res == -EAGAIN)
		goto retry;

	return res;
}

static void mnt_free_id(struct mount *mnt)
{
	int id = mnt->mnt_id;
	spin_lock(&mnt_id_lock);
	ida_remove(&mnt_id_ida, id);
	if (mnt_id_start > id)
		mnt_id_start = id;
	spin_unlock(&mnt_id_lock);
}

/*
 * Allocate a new peer group ID
 *
 * mnt_group_ida is protected by namespace_sem
 */
static int mnt_alloc_group_id(struct mount *mnt)
{
	int res;

	if (!ida_pre_get(&mnt_group_ida, GFP_KERNEL))
		return -ENOMEM;

	res = ida_get_new_above(&mnt_group_ida,
				mnt_group_start,
				&mnt->mnt_group_id);
	if (!res)
		mnt_group_start = mnt->mnt_group_id + 1;

	return res;
}

/*
 * Release a peer group ID
 */
void mnt_release_group_id(struct mount *mnt)
{
	int id = mnt->mnt_group_id;
	ida_remove(&mnt_group_ida, id);
	if (mnt_group_start > id)
		mnt_group_start = id;
	mnt->mnt_group_id = 0;
}

/*
 * vfsmount lock must be held for read
 */
static inline void mnt_add_count(struct mount *mnt, int n)
{
#ifdef CONFIG_SMP
	this_cpu_add(mnt->mnt_pcp->mnt_count, n);
#else
	preempt_disable();
	mnt->mnt_count += n;
	preempt_enable();
#endif
}

/*
 * vfsmount lock must be held for write
 */
unsigned int mnt_get_count(struct mount *mnt)
{
#ifdef CONFIG_SMP
	unsigned int count = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		count += per_cpu_ptr(mnt->mnt_pcp, cpu)->mnt_count;
	}

	return count;
#else
	return mnt->mnt_count;
#endif
}

static struct mount *alloc_vfsmnt(const char *name)
{
	struct mount *mnt = kmem_cache_zalloc(mnt_cache, GFP_KERNEL);
	if (mnt) {
		int err;

		err = mnt_alloc_id(mnt);
		if (err)
			goto out_free_cache;

		if (name) {
            //赋予mount的mnt_devname
			mnt->mnt_devname = kstrdup(name, GFP_KERNEL);
			if (!mnt->mnt_devname)
				goto out_free_id;
		}

#ifdef CONFIG_SMP
		mnt->mnt_pcp = alloc_percpu(struct mnt_pcp);
		if (!mnt->mnt_pcp)
			goto out_free_devname;

		this_cpu_add(mnt->mnt_pcp->mnt_count, 1);
#else
		mnt->mnt_count = 1;
		mnt->mnt_writers = 0;
#endif

		INIT_LIST_HEAD(&mnt->mnt_hash);
		INIT_LIST_HEAD(&mnt->mnt_child);
		INIT_LIST_HEAD(&mnt->mnt_mounts);
		INIT_LIST_HEAD(&mnt->mnt_list);
		INIT_LIST_HEAD(&mnt->mnt_expire);
		INIT_LIST_HEAD(&mnt->mnt_share);
		INIT_LIST_HEAD(&mnt->mnt_slave_list);
		INIT_LIST_HEAD(&mnt->mnt_slave);
#ifdef CONFIG_FSNOTIFY
		INIT_HLIST_HEAD(&mnt->mnt_fsnotify_marks);
#endif
	}
	return mnt;

#ifdef CONFIG_SMP
out_free_devname:
	kfree(mnt->mnt_devname);
#endif
out_free_id:
	mnt_free_id(mnt);
out_free_cache:
	kmem_cache_free(mnt_cache, mnt);
	return NULL;
}

/*
 * Most r/o checks on a fs are for operations that take
 * discrete amounts of time, like a write() or unlink().
 * We must keep track of when those operations start
 * (for permission checks) and when they end, so that
 * we can determine when writes are able to occur to
 * a filesystem.
 */
/*
 * __mnt_is_readonly: check whether a mount is read-only
 * @mnt: the mount to check for its write status
 *
 * This shouldn't be used directly ouside of the VFS.
 * It does not guarantee that the filesystem will stay
 * r/w, just that it is right *now*.  This can not and
 * should not be used in place of IS_RDONLY(inode).
 * mnt_want/drop_write() will _keep_ the filesystem
 * r/w.
 */
int __mnt_is_readonly(struct vfsmount *mnt)
{
	if (mnt->mnt_flags & MNT_READONLY)
		return 1;
	if (mnt->mnt_sb->s_flags & MS_RDONLY)
		return 1;
	return 0;
}
EXPORT_SYMBOL_GPL(__mnt_is_readonly);

static inline void mnt_inc_writers(struct mount *mnt)
{
#ifdef CONFIG_SMP
	this_cpu_inc(mnt->mnt_pcp->mnt_writers);
#else
	mnt->mnt_writers++;
#endif
}

static inline void mnt_dec_writers(struct mount *mnt)
{
#ifdef CONFIG_SMP
	this_cpu_dec(mnt->mnt_pcp->mnt_writers);
#else
	mnt->mnt_writers--;
#endif
}

static unsigned int mnt_get_writers(struct mount *mnt)
{
#ifdef CONFIG_SMP
	unsigned int count = 0;
	int cpu;

	for_each_possible_cpu(cpu) {
		count += per_cpu_ptr(mnt->mnt_pcp, cpu)->mnt_writers;
	}

	return count;
#else
	return mnt->mnt_writers;
#endif
}

static int mnt_is_readonly(struct vfsmount *mnt)
{
	if (mnt->mnt_sb->s_readonly_remount)
		return 1;
	/* Order wrt setting s_flags/s_readonly_remount in do_remount() */
	smp_rmb();
	return __mnt_is_readonly(mnt);
}

/*
 * Most r/o & frozen checks on a fs are for operations that take discrete
 * amounts of time, like a write() or unlink().  We must keep track of when
 * those operations start (for permission checks) and when they end, so that we
 * can determine when writes are able to occur to a filesystem.
 */
/**
 * __mnt_want_write - get write access to a mount without freeze protection
 * @m: the mount on which to take a write
 *
 * This tells the low-level filesystem that a write is about to be performed to
 * it, and makes sure that writes are allowed (mnt it read-write) before
 * returning success. This operation does not protect against filesystem being
 * frozen. When the write operation is finished, __mnt_drop_write() must be
 * called. This is effectively a refcount.
 */
int __mnt_want_write(struct vfsmount *m)
{
	struct mount *mnt = real_mount(m);
	int ret = 0;

	preempt_disable();
	mnt_inc_writers(mnt);
	/*
	 * The store to mnt_inc_writers must be visible before we pass
	 * MNT_WRITE_HOLD loop below, so that the slowpath can see our
	 * incremented count after it has set MNT_WRITE_HOLD.
	 */
	smp_mb();
	while (ACCESS_ONCE(mnt->mnt.mnt_flags) & MNT_WRITE_HOLD)
		cpu_relax();
	/*
	 * After the slowpath clears MNT_WRITE_HOLD, mnt_is_readonly will
	 * be set to match its requirements. So we must not load that until
	 * MNT_WRITE_HOLD is cleared.
	 */
	smp_rmb();
	if (mnt_is_readonly(m)) {
		mnt_dec_writers(mnt);
		ret = -EROFS;
	}
	preempt_enable();

	return ret;
}

/**
 * mnt_want_write - get write access to a mount
 * @m: the mount on which to take a write
 *
 * This tells the low-level filesystem that a write is about to be performed to
 * it, and makes sure that writes are allowed (mount is read-write, filesystem
 * is not frozen) before returning success.  When the write operation is
 * finished, mnt_drop_write() must be called.  This is effectively a refcount.
 */
int mnt_want_write(struct vfsmount *m)
{
	int ret;

	sb_start_write(m->mnt_sb);
	ret = __mnt_want_write(m);
	if (ret)
		sb_end_write(m->mnt_sb);
	return ret;
}
EXPORT_SYMBOL_GPL(mnt_want_write);

/**
 * mnt_clone_write - get write access to a mount
 * @mnt: the mount on which to take a write
 *
 * This is effectively like mnt_want_write, except
 * it must only be used to take an extra write reference
 * on a mountpoint that we already know has a write reference
 * on it. This allows some optimisation.
 *
 * After finished, mnt_drop_write must be called as usual to
 * drop the reference.
 */
int mnt_clone_write(struct vfsmount *mnt)
{
	/* superblock may be r/o */
	if (__mnt_is_readonly(mnt))
		return -EROFS;
	preempt_disable();
	mnt_inc_writers(real_mount(mnt));
	preempt_enable();
	return 0;
}
EXPORT_SYMBOL_GPL(mnt_clone_write);

/**
 * __mnt_want_write_file - get write access to a file's mount
 * @file: the file who's mount on which to take a write
 *
 * This is like __mnt_want_write, but it takes a file and can
 * do some optimisations if the file is open for write already
 */
int __mnt_want_write_file(struct file *file)
{
	struct inode *inode = file_inode(file);

	if (!(file->f_mode & FMODE_WRITE) || special_file(inode->i_mode))
		return __mnt_want_write(file->f_path.mnt);
	else
		return mnt_clone_write(file->f_path.mnt);
}

/**
 * mnt_want_write_file - get write access to a file's mount
 * @file: the file who's mount on which to take a write
 *
 * This is like mnt_want_write, but it takes a file and can
 * do some optimisations if the file is open for write already
 */
int mnt_want_write_file(struct file *file)
{
	int ret;

	sb_start_write(file->f_path.mnt->mnt_sb);
	ret = __mnt_want_write_file(file);
	if (ret)
		sb_end_write(file->f_path.mnt->mnt_sb);
	return ret;
}
EXPORT_SYMBOL_GPL(mnt_want_write_file);

/**
 * __mnt_drop_write - give up write access to a mount
 * @mnt: the mount on which to give up write access
 *
 * Tells the low-level filesystem that we are done
 * performing writes to it.  Must be matched with
 * __mnt_want_write() call above.
 */
void __mnt_drop_write(struct vfsmount *mnt)
{
	preempt_disable();
	mnt_dec_writers(real_mount(mnt));
	preempt_enable();
}

/**
 * mnt_drop_write - give up write access to a mount
 * @mnt: the mount on which to give up write access
 *
 * Tells the low-level filesystem that we are done performing writes to it and
 * also allows filesystem to be frozen again.  Must be matched with
 * mnt_want_write() call above.
 */
void mnt_drop_write(struct vfsmount *mnt)
{
	__mnt_drop_write(mnt);
	sb_end_write(mnt->mnt_sb);
}
EXPORT_SYMBOL_GPL(mnt_drop_write);

void __mnt_drop_write_file(struct file *file)
{
	__mnt_drop_write(file->f_path.mnt);
}

void mnt_drop_write_file(struct file *file)
{
	mnt_drop_write(file->f_path.mnt);
}
EXPORT_SYMBOL(mnt_drop_write_file);

static int mnt_make_readonly(struct mount *mnt)
{
	int ret = 0;

	br_write_lock(&vfsmount_lock);
	mnt->mnt.mnt_flags |= MNT_WRITE_HOLD;
	/*
	 * After storing MNT_WRITE_HOLD, we'll read the counters. This store
	 * should be visible before we do.
	 */
	smp_mb();

	/*
	 * With writers on hold, if this value is zero, then there are
	 * definitely no active writers (although held writers may subsequently
	 * increment the count, they'll have to wait, and decrement it after
	 * seeing MNT_READONLY).
	 *
	 * It is OK to have counter incremented on one CPU and decremented on
	 * another: the sum will add up correctly. The danger would be when we
	 * sum up each counter, if we read a counter before it is incremented,
	 * but then read another CPU's count which it has been subsequently
	 * decremented from -- we would see more decrements than we should.
	 * MNT_WRITE_HOLD protects against this scenario, because
	 * mnt_want_write first increments count, then smp_mb, then spins on
	 * MNT_WRITE_HOLD, so it can't be decremented by another CPU while
	 * we're counting up here.
	 */
	if (mnt_get_writers(mnt) > 0)
		ret = -EBUSY;
	else
		mnt->mnt.mnt_flags |= MNT_READONLY;
	/*
	 * MNT_READONLY must become visible before ~MNT_WRITE_HOLD, so writers
	 * that become unheld will see MNT_READONLY.
	 */
	smp_wmb();
	mnt->mnt.mnt_flags &= ~MNT_WRITE_HOLD;
	br_write_unlock(&vfsmount_lock);
	return ret;
}

static void __mnt_unmake_readonly(struct mount *mnt)
{
	br_write_lock(&vfsmount_lock);
	mnt->mnt.mnt_flags &= ~MNT_READONLY;
	br_write_unlock(&vfsmount_lock);
}

int sb_prepare_remount_readonly(struct super_block *sb)
{
	struct mount *mnt;
	int err = 0;

	/* Racy optimization.  Recheck the counter under MNT_WRITE_HOLD */
	if (atomic_long_read(&sb->s_remove_count))
		return -EBUSY;

	br_write_lock(&vfsmount_lock);
	list_for_each_entry(mnt, &sb->s_mounts, mnt_instance) {
		if (!(mnt->mnt.mnt_flags & MNT_READONLY)) {
			mnt->mnt.mnt_flags |= MNT_WRITE_HOLD;
			smp_mb();
			if (mnt_get_writers(mnt) > 0) {
				err = -EBUSY;
				break;
			}
		}
	}
	if (!err && atomic_long_read(&sb->s_remove_count))
		err = -EBUSY;

	if (!err) {
		sb->s_readonly_remount = 1;
		smp_wmb();
	}
	list_for_each_entry(mnt, &sb->s_mounts, mnt_instance) {
		if (mnt->mnt.mnt_flags & MNT_WRITE_HOLD)
			mnt->mnt.mnt_flags &= ~MNT_WRITE_HOLD;
	}
	br_write_unlock(&vfsmount_lock);

	return err;
}

static void free_vfsmnt(struct mount *mnt)
{
	kfree(mnt->mnt_devname);
	mnt_free_id(mnt);
#ifdef CONFIG_SMP
	free_percpu(mnt->mnt_pcp);
#endif
	kmem_cache_free(mnt_cache, mnt);
}

/*
 * find the first or last mount at @dentry on vfsmount @mnt depending on
 * @dir. If @dir is set return the first mount else return the last mount.
 * vfsmount_lock must be held for read or write.
 */
 
/*这个函数是遍历挂载点目录的核心。mount /dev/sda3 /home/，/home/所在文件系统的是mount1，本次是dest mount，挂载点目录是home目录dentry，
成为dentry1，此时home目录还没有块设备挂载。
本次的source mount是/dev/sda3生成，成为mount2。在挂载的最后，会设置mount2->parent=mount1，mount2->mnt_mountpoint=dentry1。
dest mount是source mount的父亲，这是规定!当再mount /dev/sd2 /home，本次的挂载点目录依然是home，但是home的dentry属性表示它是个挂载点
目录，此时就要执行__follow_mount_rcu->__lookup_mnt(mount1->mnt,dentry1)返回最后一次挂载到home目录的块设备挂载时生成的mount结构。
判断方法很简单，就是if (&p->mnt_parent->mnt == mnt && p->mnt_mountpoint == dentry)，从mount树中找到一个mount_x，
如果mount_x->mnt_parent->mnt=mount1->mnt,并且mount_x->mnt_mountpoint=dentry1，那说明这个mount_x一定是挂载到home目录的那个
块设备挂载时生成，返回mount_x即可。当然这个home目录会有多个块设备或者tmpfs挂载，那就循环执行__lookup_mnt()，
直到找到最后一次挂载到home目录的那个块设备的mount结构。重点注意，上边表述有误导的地方，
/home/所在文件系统挂载时生成，mount1
mount /dev/sda3 /home/  source mount:mount2，dest mount是mount1，mount2->parent=mount1。挂载点目录是dentry1
mount /dev/sda5 /home/  source mount:mount5，dest mount是mount2，mount5->parent=mount2。挂载点目录是dentry2
__lookup_mnt()则要判断两次，第一次是if (&p->mnt_parent->mnt ==mount1->vfsmnt  && p->mnt_mountpoint == dentry1)，此时是要遍历父mount
是mount1的mount，即mount2。接着要if (&p->mnt_parent->mnt ==mount2->vfsmnt  && p->mnt_mountpoint == dentry2)，
此时是要遍历父mount是mount2的mount，即mount5。所以说，当有多个块设备挂载到/home目录时，每次的父mount都是不一样的。

总结一句话:__follow_mount_rcu->__lookup_mnt 就是找到最后一次挂载到/home的块设备或者tmpfs的文件系统生成的mount结构
*/
//在mount hash链表中找到mount的mnt_parent，如果这个parent mount与传入的vfsmount的mount是同一个，并且parent mount的mnt_mountpoint
//与传入的挂载点目录dentry是同一个，那就返回这个parent mount
struct mount *__lookup_mnt(struct vfsmount *mnt, struct dentry *dentry,
			      int dir)//dir:__lookup_mnt()传进来是1
{
    //以vfsmount和挂载点目录dentry找到mount hash链表头，commit_tree()把mount加入该链表，
    //用的键值也是(父mount结构的vfsmount成员+该mount的挂载点dentry),所以该函数传入的vfsmount，是父mount的vfsmount
	struct list_head *head = mount_hashtable + hash(mnt, dentry);
	struct list_head *tmp = head;
	struct mount *p, *found = NULL;

	for (;;) {
        //如果dir为1，向链表后搜索，否则向链表前搜索
		tmp = dir ? tmp->next : tmp->prev;
		p = NULL;

        //这里应该是找到链表尾了吧，没有找到匹配的
		if (tmp == head)
			break;
        //由mnt_hash找到mount
		p = list_entry(tmp, struct mount, mnt_hash);

        //这是说hash链表上的mount的mnt_parent与当前传入的vfsmount的mount一致，并且这个mount的挂载点目录dentry与传入的一致
		if (&p->mnt_parent->mnt == mnt && p->mnt_mountpoint == dentry) {
			found = p;
			break;
		}
	}
	return found;
}

/*
 * lookup_mnt - Return the first child mount mounted at path
 *
 * "First" means first mounted chronologically.  If you create the
 * following mounts:
 *
 * mount /dev/sda1 /mnt
 * mount /dev/sda2 /mnt
 * mount /dev/sda3 /mnt
 *
 * Then lookup_mnt() on the base /mnt dentry in the root mount will
 * return successively the root dentry and vfsmount of /dev/sda1, then
 * /dev/sda2, then /dev/sda3, then NULL.
 *
 * lookup_mnt takes a reference to the found vfsmount.
 */
//比如本次mount /dev/sda0 /mnt,而之前已经有sda1、sda2、sda3挂载到了/mmt，__lookup_mnt()这里边依次找到sda1、sda2、sda3的mount并返回
//给child_mnt,最后一次是返回NULL，此时才开始处理mount /dev/sda0 /mnt的mount
struct vfsmount *lookup_mnt(struct path *path)
{
	struct mount *child_mnt;

	br_read_lock(&vfsmount_lock);
//在mount hash链表中找到mount的mnt_parent，如果这个parent mount与传入的path->mnt的mount是同一个，并且parent mount的mnt_mountpoint
//与传入的挂载点目录path->dentry是同一个，那就返回这个parent mount。这样看来的话，搜索sda1、sda2、sda3挂载到/mnt/的mount结构
//实际是搜索的mount hash链表中找到mount的mnt_parent呀
	child_mnt = __lookup_mnt(path->mnt, path->dentry, 1);
	if (child_mnt) {
		mnt_add_count(child_mnt, 1);
		br_read_unlock(&vfsmount_lock);
		return &child_mnt->mnt;
	} else {
		br_read_unlock(&vfsmount_lock);
		return NULL;
	}
}

static struct mountpoint *new_mountpoint(struct dentry *dentry)
{
    //显然，又是一个hash链表，基于挂载点目录dentry
	struct list_head *chain = mountpoint_hashtable + hash(NULL, dentry);
	struct mountpoint *mp;
    
    //先在hash链表中查找已有的mountpoint，是否它的挂载点目录dentry与传入的dentry相等
	list_for_each_entry(mp, chain, m_hash) {
	    
		if (mp->m_dentry == dentry) {
			/* might be worth a WARN_ON() */
			if (d_unlinked(dentry))
				return ERR_PTR(-ENOENT);
			mp->m_count++;
			return mp;
		}
	}
    //找不到就分配一个新的mp
	mp = kmalloc(sizeof(struct mountpoint), GFP_KERNEL);
	if (!mp)
		return ERR_PTR(-ENOMEM);

	spin_lock(&dentry->d_lock);
	if (d_unlinked(dentry)) {
		spin_unlock(&dentry->d_lock);
		kfree(mp);
		return ERR_PTR(-ENOENT);
	}
	dentry->d_flags |= DCACHE_MOUNTED;
	spin_unlock(&dentry->d_lock);
    //挂载点目录dentry
	mp->m_dentry = dentry;
	mp->m_count = 1;
	list_add(&mp->m_hash, chain);
	return mp;
}

static void put_mountpoint(struct mountpoint *mp)
{
	if (!--mp->m_count) {
		struct dentry *dentry = mp->m_dentry;
		spin_lock(&dentry->d_lock);
		dentry->d_flags &= ~DCACHE_MOUNTED;
		spin_unlock(&dentry->d_lock);
		list_del(&mp->m_hash);
		kfree(mp);
	}
}

static inline int check_mnt(struct mount *mnt)
{
	return mnt->mnt_ns == current->nsproxy->mnt_ns;
}

/*
 * vfsmount lock must be held for write
 */
static void touch_mnt_namespace(struct mnt_namespace *ns)
{
	if (ns) {
		ns->event = ++event;
		wake_up_interruptible(&ns->poll);
	}
}

/*
 * vfsmount lock must be held for write
 */
static void __touch_mnt_namespace(struct mnt_namespace *ns)
{
	if (ns && ns->event != event) {
		ns->event = event;
		wake_up_interruptible(&ns->poll);
	}
}

/*
 * vfsmount lock must be held for write
 */
static void detach_mnt(struct mount *mnt, struct path *old_path)
{
	old_path->dentry = mnt->mnt_mountpoint;
	old_path->mnt = &mnt->mnt_parent->mnt;
	mnt->mnt_parent = mnt;
	mnt->mnt_mountpoint = mnt->mnt.mnt_root;
	list_del_init(&mnt->mnt_child);
	list_del_init(&mnt->mnt_hash);
	put_mountpoint(mnt->mnt_mp);
	mnt->mnt_mp = NULL;
}

/*
 * vfsmount lock must be held for write
 */
//主要设置本次新生成的挂载源mount->mnt_mountpoint为本次挂载点目录的dentry
void mnt_set_mountpoint(struct mount *mnt,//dest_mnt  挂载点目录所在的文件系统的mount结构
			struct mountpoint *mp,//dest_mp
			struct mount *child_mnt)//child_mnt 挂载源，就是本次mount新生成的
{
	mp->m_count++;
	mnt_add_count(mnt, 1);	/* essentially, that's mntget */
    //设置为挂载点目录dentry
	child_mnt->mnt_mountpoint = dget(mp->m_dentry);
    //mnt_parent竟然为挂点目录所在文件系统的mount
	child_mnt->mnt_parent = mnt;
    //设置为挂载点mountpoint
	child_mnt->mnt_mp = mp;
}

/*
 * vfsmount lock must be held for write
 */
//mnt是本次挂载的挂载源新生成的mount，设置挂载点目录等等，并把mnt添加到mnt_hash，mnt_child链表。parent是挂载点目录所属文件系统mount
static void attach_mnt(struct mount *mnt,
			struct mount *parent,
			struct mountpoint *mp)
{
    //主要设置本次新生成的挂载源mnt->mnt_mountpoint为本次挂载点目录的dentry
	mnt_set_mountpoint(parent, mp, mnt);
    //本次新生成的挂载源mnt添加到mount_hashtable表
	list_add_tail(&mnt->mnt_hash, mount_hashtable +
			hash(&parent->mnt, mp->m_dentry));
    //本次新生成的挂载源mnt添加到挂载点目录所在文件系统mount的mnt_mounts链表。
  /*关于父子mount的理解，每一次挂载，都会针对挂载源生成一个mount结构，即source mount，而针对挂载点目录所处文件系统的dest mount，
    就是本次挂在的父mount。source mount是子mount,dest mount是父mount，source mnt->mnt_child链接到dest mount的parent->mnt_mounts。
    举例，dest mount是sda3 挂在到根目录'/'生成的，然后sda5挂载到/home目录，这次生成的mount，即souce mount，与sda3的dest mount是父子关系。 */
	list_add_tail(&mnt->mnt_child, &parent->mnt_mounts);
}

/*
 * vfsmount lock must be held for write
 */
//把mount结构添加到各个链表，设置mount的文件系统命名空间为父mount的命名空间
static void commit_tree(struct mount *mnt)
{
    //mnt的父mount，这是mnt的挂载点目录所在的文件系统mount
	struct mount *parent = mnt->mnt_parent;
	struct mount *m;
	LIST_HEAD(head);
    //文件系统命名空间，来自父mount
	struct mnt_namespace *n = parent->mnt_ns;

	BUG_ON(parent == mnt);
    //把mnt添加到head链表，
	list_add_tail(&head, &mnt->mnt_list);
    
    //设置mount的命名空间，为什么要循环设置呢??????不就是一个mount结构?看这个循环的意思是，以该mount为链表头，有多个mount结构
    //通过其mnt_list结构链入这个链表，这个循环是设置所有这个链表上的mount的命名空间为父mount的命名空间
	list_for_each_entry(m, &head, mnt_list)
		m->mnt_ns = n;

    //把mount结构添加到父mount的mnt_ns的list链表，本次的mount结构在head链表上
	list_splice(&head, n->list.prev);

    //靠mnt_hash把当前的mount结构链入mount hash链表，并且链入hash表的键值是(父mount结构的vfsmount成员+该mount的挂载点dentry)
	list_add_tail(&mnt->mnt_hash, mount_hashtable +
				hash(&parent->mnt, mnt->mnt_mountpoint));
    //靠mnt_child把mount结构添加到mount的mnt_parent的mnt_mounts链表
	list_add_tail(&mnt->mnt_child, &parent->mnt_mounts);
	touch_mnt_namespace(n);
}
//取出p->mnt_mounts链表上的下一个子mount结构
static struct mount *next_mnt(struct mount *p, struct mount *root)//root:invent_group_ids传过来的是NULL
{
    //下一个mount
	struct list_head *next = p->mnt_mounts.next;
    //下一个mount和当前指向的mount结构是同一个??????这是啥意思，应该是特殊情况吧
	if (next == &p->mnt_mounts) {
		while (1) {
			if (p == root)
				return NULL;
			next = p->mnt_child.next;
			if (next != &p->mnt_parent->mnt_mounts)
				break;
            //依次取出mnt_parent父mount
			p = p->mnt_parent;
		}
	}
    //根据next返回对应的mount结构
	return list_entry(next, struct mount, mnt_child);
}

static struct mount *skip_mnt_tree(struct mount *p)
{
	struct list_head *prev = p->mnt_mounts.prev;
	while (prev != &p->mnt_mounts) {
		p = list_entry(prev, struct mount, mnt_child);
		prev = p->mnt_mounts.prev;
	}
	return p;
}

/*
 查找到/dev/mmcblk0p5的dentry、mnt、inode结构，并由inode得到块设备的bdev，创建superblock
 结构并初始化其成员。然后读取ext4文件系统flash中的超级快数据，并分析超级快数据，给
 ext4_super_block、ext4_sb_info、super_block赋值。得到ext4文件系统的root inode，
 以"/"目录名字分配并初始化root dentry，并对vfsmont初始化
 */

struct vfsmount *
vfs_kern_mount(struct file_system_type *type, int flags, const char *name, void *data)
{
	struct mount *mnt;
	struct dentry *root;

	if (!type)
		return ERR_PTR(-ENODEV);
   //分配mnt结构
	mnt = alloc_vfsmnt(name);
	if (!mnt)
		return ERR_PTR(-ENOMEM);

	if (flags & MS_KERNMOUNT)
		mnt->mnt.mnt_flags = MNT_INTERNAL;
    //核心操作
      /*
     查找到/dev/mmcblk0p5的dentry、mnt、inode结构，并由inode得到块设备的bdev，创建superblock
     结构并初始化其成员。然后读取ext4文件系统flash中的超级快数据，并分析超级快数据，给
     ext4_super_block、ext4_sb_info、super_block赋值。得到ext4文件系统的root inode，
     以"/"目录名字分配并初始化root dentry，并对vfsmont初始化
     */
	root = mount_fs(type, flags, name, data);
	if (IS_ERR(root)) {
		free_vfsmnt(mnt);
		return ERR_CAST(root);
	}

    //这就是ext4文件系统管道root dentry，由此可以得到root inode
	mnt->mnt.mnt_root = root;
	mnt->mnt.mnt_sb = root->d_sb;
	mnt->mnt_mountpoint = mnt->mnt.mnt_root;
	mnt->mnt_parent = mnt;
	br_write_lock(&vfsmount_lock);
	list_add_tail(&mnt->mnt_instance, &root->d_sb->s_mounts);
	br_write_unlock(&vfsmount_lock);
	return &mnt->mnt;
}
EXPORT_SYMBOL_GPL(vfs_kern_mount);

/*克隆创建本次挂载的mount结构，即本次的source mount，大部分成员复制了old的，old是mount bind源目录所在文件系统的mount结构*/
//假设源目录/home，则old是/home/所在文件系统挂载生成的struct mount，old_path.dentry是/home挂载源的。如果/home/目录已经有块设备挂载了，
//则要转换成之前挂载的块设备的mount结构，赋予old，然后是它根目录dentry保存到old_path.dentry。mount和目录dentry刨根问底到最后的块设备，终极mount和dentry
static struct mount *clone_mnt(struct mount *old, struct dentry *root,
					int flag)//old 是mount --bind /home/ /home/test 源目录/home的终极mount，是本次的克隆母体，克隆生成本次挂载的source mount
					         //root是/home目录终极dentry，终极的意思是如果该目录可能有多个块设备挂载，要遍历到最后一个块设备的根目录
{                            //否则，只是/home目录的dentry
	struct super_block *sb = old->mnt.mnt_sb;
	struct mount *mnt;
	int err;
    //为本次挂载分配一个新的mount，并赋予mnt的mnt_devname为old->mnt_devname.
    /*mnt就是本次的克隆mount，即本次挂载的source mount*/
	mnt = alloc_vfsmnt(old->mnt_devname);
	if (!mnt)
		return ERR_PTR(-ENOMEM);

    //如果本次的挂载是slave或者private属性
	if (flag & (CL_SLAVE | CL_PRIVATE | CL_SHARED_TO_SLAVE))
		mnt->mnt_group_id = 0; // not a peer of original 
	else/*充分说明本次mount bind的新生成的source mount即mnt，group id来自mount bind操作源目录所在终极文件系统mount的group id*/
		mnt->mnt_group_id = old->mnt_group_id;

    //只有mount是share属性并且没有group id时才会分配新的group id。默认的mount都是shared属性，一般mount操作的soucer mount不会在这里分配
	if ((flag & CL_MAKE_SHARED) && !mnt->mnt_group_id) {
		err = mnt_alloc_group_id(mnt);
		if (err)
			goto out_free;
	}

	mnt->mnt.mnt_flags = old->mnt.mnt_flags & ~MNT_WRITE_HOLD;
	// Don't allow unprivileged users to change mount flags 
	if (flag & CL_UNPRIVILEGED) {
		mnt->mnt.mnt_flags |= MNT_LOCK_ATIME;

		if (mnt->mnt.mnt_flags & MNT_READONLY)
			mnt->mnt.mnt_flags |= MNT_LOCK_READONLY;

		if (mnt->mnt.mnt_flags & MNT_NODEV)
			mnt->mnt.mnt_flags |= MNT_LOCK_NODEV;

		if (mnt->mnt.mnt_flags & MNT_NOSUID)
			mnt->mnt.mnt_flags |= MNT_LOCK_NOSUID;

		if (mnt->mnt.mnt_flags & MNT_NOEXEC)
			mnt->mnt.mnt_flags |= MNT_LOCK_NOEXEC;
	}

	atomic_inc(&sb->s_active);
    /*本次挂载克隆生成的mount，即mnt的超级快是克隆母体的*/
	mnt->mnt.mnt_sb = sb;
    
    //本次挂载源的根目录dentry，mount --bind模式下，根目录可不一定是块设备的根目录，而是挂载源的那个目录。比如
    //mount --bind  /home/test/test2  /home/test,此时挂载源目录dentry是/home/test/test2的.如果之前已经有块设备sda3挂载到/home/，
    //此时的源目录实际是sda3文件系统里的/test/test2目录的dentry,这应该是就是cat /proc/self/mountinfo看到第4列的挂载源不是'/'的原因吧
    /*总之，mount bind 操作生成的source mount，即mnt的mnt_root，可能只是mount bind源目录dentry*/
	mnt->mnt.mnt_root = dget(root);
    
	mnt->mnt_mountpoint = mnt->mnt.mnt_root;//初始挂载点目录先赋值成mnt_root，后续会再改
	mnt->mnt_parent = mnt;//mnt_parent也是暂时赋值为自己，后续再改
	
	br_write_lock(&vfsmount_lock);
	list_add_tail(&mnt->mnt_instance, &sb->s_mounts);
	br_write_unlock(&vfsmount_lock);


	if ((flag & CL_SLAVE) ||
	    ((flag & CL_SHARED_TO_SLAVE) && IS_MNT_SHARED(old))) {
	    /*如果挂载属性slave，本次挂载的source mount通过其mnt_slave链接到克隆母体mount结构的mnt_slave_list*/
		list_add(&mnt->mnt_slave, &old->mnt_slave_list);

        /*mnt的mnt_master指向克隆母体的mount结构*/
		mnt->mnt_master = old;
		CLEAR_MNT_SHARED(mnt);
    //如果挂载没有private属性，即share或者没有指mount属性
	} else if (!(flag & CL_PRIVATE)) {
	
	 //本次挂载指定shared属性。或挂载源的mount(克隆母体)是shared属性，这就是mount时没有指定属性，则默认就是shared，因为它的克隆母体是shared
		if ((flag & CL_MAKE_SHARED) || IS_MNT_SHARED(old))
        /*把本次挂载的source mount通过其mnt_share链接到克隆母体的mnt_share链表*/
			list_add(&mnt->mnt_share, &old->mnt_share);
			
        //如果本次mount bind没有指定mount属性，但是mount bind源目录的mount即克隆母体是slave属性，则本次挂载的source mount添加到
        //克隆母体mount结构的mnt_slave链表。这样本次的source mount和克隆目录mount就属于同一个mount slave组，不是父子关系
        /*如果克隆母体slave属性而本次mount没有指定属性，则本次mount会被添加到与克隆母体同一个mount salve组链表*/
		if (IS_MNT_SLAVE(old))
			list_add(&mnt->mnt_slave, &old->mnt_slave);//把本次的mount链接到mnt_slave链表
			
	    //默认设置source mount的mnt_master与克隆母体的old->mnt_master一致，表示两个mount都是一个mount slave组的
	    //就是说，mount指定share属性，其mount->mnt_master不会为NULL???????????????，不是吧，应该有地方清NULL的吧???????
		mnt->mnt_master = old->mnt_master;
	}
    
    //设置mount的shared属性
	if (flag & CL_MAKE_SHARED)
		set_mnt_shared(mnt);

	/* stick the duplicate mount on the same expiry list
	 * as the original if that was on one */
	if (flag & CL_EXPIRE) {
		if (!list_empty(&old->mnt_expire))
			list_add(&mnt->mnt_expire, &old->mnt_expire);
	}

	return mnt;

 out_free:
	free_vfsmnt(mnt);
	return ERR_PTR(err);
}

static inline void mntfree(struct mount *mnt)
{
	struct vfsmount *m = &mnt->mnt;
	struct super_block *sb = m->mnt_sb;

	/*
	 * This probably indicates that somebody messed
	 * up a mnt_want/drop_write() pair.  If this
	 * happens, the filesystem was probably unable
	 * to make r/w->r/o transitions.
	 */
	/*
	 * The locking used to deal with mnt_count decrement provides barriers,
	 * so mnt_get_writers() below is safe.
	 */
	WARN_ON(mnt_get_writers(mnt));
	fsnotify_vfsmount_delete(m);
	dput(m->mnt_root);
	free_vfsmnt(mnt);
	deactivate_super(sb);
}

static void mntput_no_expire(struct mount *mnt)
{
put_again:
#ifdef CONFIG_SMP
	br_read_lock(&vfsmount_lock);
	if (likely(mnt->mnt_ns)) {
		/* shouldn't be the last one */
		mnt_add_count(mnt, -1);
		br_read_unlock(&vfsmount_lock);
		return;
	}
	br_read_unlock(&vfsmount_lock);

	br_write_lock(&vfsmount_lock);
	mnt_add_count(mnt, -1);
	if (mnt_get_count(mnt)) {
		br_write_unlock(&vfsmount_lock);
		return;
	}
#else
	mnt_add_count(mnt, -1);
	if (likely(mnt_get_count(mnt)))
		return;
	br_write_lock(&vfsmount_lock);
#endif
	if (unlikely(mnt->mnt_pinned)) {
		mnt_add_count(mnt, mnt->mnt_pinned + 1);
		mnt->mnt_pinned = 0;
		br_write_unlock(&vfsmount_lock);
		acct_auto_close_mnt(&mnt->mnt);
		goto put_again;
	}

	list_del(&mnt->mnt_instance);
	br_write_unlock(&vfsmount_lock);
	mntfree(mnt);
}

void mntput(struct vfsmount *mnt)
{
	if (mnt) {
		struct mount *m = real_mount(mnt);
		/* avoid cacheline pingpong, hope gcc doesn't get "smart" */
		if (unlikely(m->mnt_expiry_mark))
			m->mnt_expiry_mark = 0;
		mntput_no_expire(m);
	}
}
EXPORT_SYMBOL(mntput);

struct vfsmount *mntget(struct vfsmount *mnt)
{
	if (mnt)
		mnt_add_count(real_mount(mnt), 1);
	return mnt;
}
EXPORT_SYMBOL(mntget);

void mnt_pin(struct vfsmount *mnt)
{
	br_write_lock(&vfsmount_lock);
	real_mount(mnt)->mnt_pinned++;
	br_write_unlock(&vfsmount_lock);
}
EXPORT_SYMBOL(mnt_pin);

void mnt_unpin(struct vfsmount *m)
{
	struct mount *mnt = real_mount(m);
	br_write_lock(&vfsmount_lock);
	if (mnt->mnt_pinned) {
		mnt_add_count(mnt, 1);
		mnt->mnt_pinned--;
	}
	br_write_unlock(&vfsmount_lock);
}
EXPORT_SYMBOL(mnt_unpin);

static inline void mangle(struct seq_file *m, const char *s)
{
	seq_escape(m, s, " \t\n\\");
}

/*
 * Simple .show_options callback for filesystems which don't want to
 * implement more complex mount option showing.
 *
 * See also save_mount_options().
 */
int generic_show_options(struct seq_file *m, struct dentry *root)
{
	const char *options;

	rcu_read_lock();
	options = rcu_dereference(root->d_sb->s_options);

	if (options != NULL && options[0]) {
		seq_putc(m, ',');
		mangle(m, options);
	}
	rcu_read_unlock();

	return 0;
}
EXPORT_SYMBOL(generic_show_options);

/*
 * If filesystem uses generic_show_options(), this function should be
 * called from the fill_super() callback.
 *
 * The .remount_fs callback usually needs to be handled in a special
 * way, to make sure, that previous options are not overwritten if the
 * remount fails.
 *
 * Also note, that if the filesystem's .remount_fs function doesn't
 * reset all options to their default value, but changes only newly
 * given options, then the displayed options will not reflect reality
 * any more.
 */
void save_mount_options(struct super_block *sb, char *options)
{
	BUG_ON(sb->s_options);
	rcu_assign_pointer(sb->s_options, kstrdup(options, GFP_KERNEL));
}
EXPORT_SYMBOL(save_mount_options);

void replace_mount_options(struct super_block *sb, char *options)
{
	char *old = sb->s_options;
	rcu_assign_pointer(sb->s_options, options);
	if (old) {
		synchronize_rcu();
		kfree(old);
	}
}
EXPORT_SYMBOL(replace_mount_options);

#ifdef CONFIG_PROC_FS
/* iterator; we want it to have access to namespace_sem, thus here... */
static void *m_start(struct seq_file *m, loff_t *pos)
{
	struct proc_mounts *p = proc_mounts(m);

	down_read(&namespace_sem);
	return seq_list_start(&p->ns->list, *pos);
}

static void *m_next(struct seq_file *m, void *v, loff_t *pos)
{
	struct proc_mounts *p = proc_mounts(m);

	return seq_list_next(v, &p->ns->list, pos);
}

static void m_stop(struct seq_file *m, void *v)
{
	up_read(&namespace_sem);
}

static int m_show(struct seq_file *m, void *v)
{
	struct proc_mounts *p = proc_mounts(m);
	struct mount *r = list_entry(v, struct mount, mnt_list);
	return p->show(m, &r->mnt);
}

const struct seq_operations mounts_op = {
	.start	= m_start,
	.next	= m_next,
	.stop	= m_stop,
	.show	= m_show,
};
#endif  /* CONFIG_PROC_FS */

/**
 * may_umount_tree - check if a mount tree is busy
 * @mnt: root of mount tree
 *
 * This is called to check if a tree of mounts has any
 * open files, pwds, chroots or sub mounts that are
 * busy.
 */
int may_umount_tree(struct vfsmount *m)
{
	struct mount *mnt = real_mount(m);
	int actual_refs = 0;
	int minimum_refs = 0;
	struct mount *p;
	BUG_ON(!m);

	/* write lock needed for mnt_get_count */
	br_write_lock(&vfsmount_lock);
	for (p = mnt; p; p = next_mnt(p, mnt)) {
		actual_refs += mnt_get_count(p);
		minimum_refs += 2;
	}
	br_write_unlock(&vfsmount_lock);

	if (actual_refs > minimum_refs)
		return 0;

	return 1;
}

EXPORT_SYMBOL(may_umount_tree);

/**
 * may_umount - check if a mount point is busy
 * @mnt: root of mount
 *
 * This is called to check if a mount point has any
 * open files, pwds, chroots or sub mounts. If the
 * mount has sub mounts this will return busy
 * regardless of whether the sub mounts are busy.
 *
 * Doesn't take quota and stuff into account. IOW, in some cases it will
 * give false negatives. The main reason why it's here is that we need
 * a non-destructive way to look for easily umountable filesystems.
 */
int may_umount(struct vfsmount *mnt)
{
	int ret = 1;
	down_read(&namespace_sem);
	br_write_lock(&vfsmount_lock);
	if (propagate_mount_busy(real_mount(mnt), 2))
		ret = 0;
	br_write_unlock(&vfsmount_lock);
	up_read(&namespace_sem);
	return ret;
}

EXPORT_SYMBOL(may_umount);

static LIST_HEAD(unmounted);	/* protected by namespace_sem */

static void namespace_unlock(void)
{
	struct mount *mnt;
	LIST_HEAD(head);

	if (likely(list_empty(&unmounted))) {
		up_write(&namespace_sem);
		return;
	}

	list_splice_init(&unmounted, &head);
	up_write(&namespace_sem);

	while (!list_empty(&head)) {
		mnt = list_first_entry(&head, struct mount, mnt_hash);
		list_del_init(&mnt->mnt_hash);
		if (mnt_has_parent(mnt)) {
			struct dentry *dentry;
			struct mount *m;

			br_write_lock(&vfsmount_lock);
			dentry = mnt->mnt_mountpoint;
			m = mnt->mnt_parent;
			mnt->mnt_mountpoint = mnt->mnt.mnt_root;
			mnt->mnt_parent = mnt;
			m->mnt_ghosts--;
			br_write_unlock(&vfsmount_lock);
			dput(dentry);
			mntput(&m->mnt);
		}
		mntput(&mnt->mnt);
	}
}

static inline void namespace_lock(void)
{
	down_write(&namespace_sem);
}

/*
 * vfsmount lock must be held for write
 * namespace_sem must be held for write
 */
void umount_tree(struct mount *mnt, int propagate)
{
	LIST_HEAD(tmp_list);
	struct mount *p;

	for (p = mnt; p; p = next_mnt(p, mnt))
		list_move(&p->mnt_hash, &tmp_list);

	if (propagate)
		propagate_umount(&tmp_list);

	list_for_each_entry(p, &tmp_list, mnt_hash) {
		list_del_init(&p->mnt_expire);
		list_del_init(&p->mnt_list);
		__touch_mnt_namespace(p->mnt_ns);
		p->mnt_ns = NULL;
		list_del_init(&p->mnt_child);
		if (mnt_has_parent(p)) {
			p->mnt_parent->mnt_ghosts++;
			put_mountpoint(p->mnt_mp);
			p->mnt_mp = NULL;
		}
		change_mnt_propagation(p, MS_PRIVATE);
	}
	list_splice(&tmp_list, &unmounted);
}

static void shrink_submounts(struct mount *mnt);

static int do_umount(struct mount *mnt, int flags)
{
	struct super_block *sb = mnt->mnt.mnt_sb;
	int retval;

	retval = security_sb_umount(&mnt->mnt, flags);
	if (retval)
		return retval;

	/*
	 * Allow userspace to request a mountpoint be expired rather than
	 * unmounting unconditionally. Unmount only happens if:
	 *  (1) the mark is already set (the mark is cleared by mntput())
	 *  (2) the usage count == 1 [parent vfsmount] + 1 [sys_umount]
	 */
	if (flags & MNT_EXPIRE) {
		if (&mnt->mnt == current->fs->root.mnt ||
		    flags & (MNT_FORCE | MNT_DETACH))
			return -EINVAL;

		/*
		 * probably don't strictly need the lock here if we examined
		 * all race cases, but it's a slowpath.
		 */
		br_write_lock(&vfsmount_lock);
		if (mnt_get_count(mnt) != 2) {
			br_write_unlock(&vfsmount_lock);
			return -EBUSY;
		}
		br_write_unlock(&vfsmount_lock);

		if (!xchg(&mnt->mnt_expiry_mark, 1))
			return -EAGAIN;
	}

	/*
	 * If we may have to abort operations to get out of this
	 * mount, and they will themselves hold resources we must
	 * allow the fs to do things. In the Unix tradition of
	 * 'Gee thats tricky lets do it in userspace' the umount_begin
	 * might fail to complete on the first run through as other tasks
	 * must return, and the like. Thats for the mount program to worry
	 * about for the moment.
	 */

	if (flags & MNT_FORCE && sb->s_op->umount_begin) {
		sb->s_op->umount_begin(sb);
	}

	/*
	 * No sense to grab the lock for this test, but test itself looks
	 * somewhat bogus. Suggestions for better replacement?
	 * Ho-hum... In principle, we might treat that as umount + switch
	 * to rootfs. GC would eventually take care of the old vfsmount.
	 * Actually it makes sense, especially if rootfs would contain a
	 * /reboot - static binary that would close all descriptors and
	 * call reboot(9). Then init(8) could umount root and exec /reboot.
	 */
	if (&mnt->mnt == current->fs->root.mnt && !(flags & MNT_DETACH)) {
		/*
		 * Special case for "unmounting" root ...
		 * we just try to remount it readonly.
		 */
		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		down_write(&sb->s_umount);
		if (!(sb->s_flags & MS_RDONLY))
			retval = do_remount_sb(sb, MS_RDONLY, NULL, 0);
		up_write(&sb->s_umount);
		return retval;
	}

	namespace_lock();
	br_write_lock(&vfsmount_lock);
	event++;

	if (!(flags & MNT_DETACH))
		shrink_submounts(mnt);

	retval = -EBUSY;
	if (flags & MNT_DETACH || !propagate_mount_busy(mnt, 2)) {
		if (!list_empty(&mnt->mnt_list))
			umount_tree(mnt, 1);
		retval = 0;
	}
	br_write_unlock(&vfsmount_lock);
	namespace_unlock();
	return retval;
}

/* 
 * Is the caller allowed to modify his namespace?
 */
static inline bool may_mount(void)
{
	return ns_capable(current->nsproxy->mnt_ns->user_ns, CAP_SYS_ADMIN);
}

/*
 * Now umount can handle mount points as well as block devices.
 * This is important for filesystems which use unnamed block devices.
 *
 * We now support a flag for forced unmount like the other 'big iron'
 * unixes. Our API is identical to OSF/1 to avoid making a mess of AMD
 */

SYSCALL_DEFINE2(umount, char __user *, name, int, flags)
{
	struct path path;
	struct mount *mnt;
	int retval;
	int lookup_flags = 0;

	if (flags & ~(MNT_FORCE | MNT_DETACH | MNT_EXPIRE | UMOUNT_NOFOLLOW))
		return -EINVAL;

	if (!may_mount())
		return -EPERM;

	if (!(flags & UMOUNT_NOFOLLOW))
		lookup_flags |= LOOKUP_FOLLOW;

	retval = user_path_at(AT_FDCWD, name, lookup_flags, &path);
	if (retval)
		goto out;
	mnt = real_mount(path.mnt);
	retval = -EINVAL;
	if (path.dentry != path.mnt->mnt_root)
		goto dput_and_out;
	if (!check_mnt(mnt))
		goto dput_and_out;
	retval = -EPERM;
	if (flags & MNT_FORCE && !capable(CAP_SYS_ADMIN))
		goto dput_and_out;

	retval = do_umount(mnt, flags);
dput_and_out:
	/* we mustn't call path_put() as that would clear mnt_expiry_mark */
	dput(path.dentry);
	mntput_no_expire(mnt);
out:
	return retval;
}

#ifdef __ARCH_WANT_SYS_OLDUMOUNT

/*
 *	The 2.0 compatible umount. No flags.
 */
SYSCALL_DEFINE1(oldumount, char __user *, name)
{
	return sys_umount(name, 0);
}

#endif

static bool mnt_ns_loop(struct path *path)
{
	/* Could bind mounting the mount namespace inode cause a
	 * mount namespace loop?
	 */
	struct inode *inode = path->dentry->d_inode;
	struct proc_ns *ei;
	struct mnt_namespace *mnt_ns;

	if (!proc_ns_inode(inode))
		return false;

	ei = get_proc_ns(inode);
	if (ei->ns_ops != &mntns_operations)
		return false;
    //当前文件的命名空间
	mnt_ns = ei->ns;
    //current->nsproxy->mnt_ns当前进程的命名空间
	return current->nsproxy->mnt_ns->seq >= mnt_ns->seq;
}
/*
mnt是挂载点目录所在文件系统的mount结构，dentry是挂载点目录dentry。注意，已经转换成挂载源块设备的。比如，sda1挂载到/home/目录，则此时
mnt是sda1挂载到/home/目录生成的，dentry是sda1块设备文件系统的根目录。接着sda2挂载待/home/test2目录，挂载时生成mount2,挂载点dentry2。
sda3挂载到/home/test3目录，挂载时生成mount3,挂载点dentry2。mount2和mount3与mnt是父子关系，因为mount2和mount3的挂载点目录是/home/目录下的
test2和test3目录，准确说是sda1块设备下的test2和test3目录。然后，sda5挂在到/home/test2/test5目录，生成mount5,挂载点dentry5。sda6挂载到
/home/test3/test6目录，生成mount6，挂载点dentry6。则mount5和mount2是父子关系，mount6和mount3是父子关系。*/


/*只是q=res=clone_mnt()克隆一个mount，然后q->mnt_mountpoint = mnt->mnt_mountpoint设置克隆的mount的mnt_mountpoint为克隆母体
的mnt_mountpoint，最后返回克隆的mount。list_for_each_entry()那段代码就没有执行到*/
struct mount *copy_tree(struct mount *mnt, struct dentry *dentry,
					int flag)//mnt是克隆母体，dentry是克隆母体mount挂载源目录dentry，不一定是块设备根目录dentry，mount bind操作是挂载源目录dentry
{
	struct mount *res, *p, *q, *r, *parent;

	if (!(flag & CL_COPY_ALL) && IS_MNT_UNBINDABLE(mnt))
		return ERR_PTR(-EINVAL);

    //创建新的mount并对大部分成员初始化并返回给res和p，好多成员信息复制了mnt的。
	res = q = clone_mnt(mnt, dentry, flag);
	if (IS_ERR(q))
		return q;
    //赋值挂载点dentry
	q->mnt_mountpoint = mnt->mnt_mountpoint;

    //p是最原始的
	p = mnt;

    /*下边这个实际测试发现，就没有执行过，mount bind也测试过，一样。什么情况下会执行到?????????????*/
    //遍历mnt->mnt_mounts链表上的mounte结构，这个链表上的mount应该都是mnt的子mount结构
	list_for_each_entry(r, &mnt->mnt_mounts, mnt_child) {
		struct mount *s;
        
        //is_subdir:判断r->mnt_mountpoint是否是dentry的子目录，是则返回1，if不成立
		if (!is_subdir(r->mnt_mountpoint, dentry))
			continue;

        //以r为源头，遍历r->mnt_mounts链表上子mount结构
		for (s = r; s; s = next_mnt(s, r)) {
            //
			if (!(flag & CL_COPY_ALL) && IS_MNT_UNBINDABLE(s)) {
				s = skip_mnt_tree(s);
				continue;
			}
            //
			while (p != s->mnt_parent) {
				p = p->mnt_parent;
				q = q->mnt_parent;
			}
			p = s;
            //显然parent本次for循环源头
			parent = q;
            
            //创建新的mount并对大部分成员初始化并返回给q，好多成员信息复制了p的。
			q = clone_mnt(p, p->mnt.mnt_root, flag);
			if (IS_ERR(q))
				goto out;
			br_write_lock(&vfsmount_lock);
            //创建的新mount即q靠mnt_list添加到该链表
			list_add_tail(&q->mnt_list, &res->mnt_list);
          //q作为挂载源mount，parent是挂载点目录的mount，q作为子mount，parent作为父mount，把q->mnt_child添加到parent->mnt_mounts链表。
          //令q->mnt_mountpoint=p->mnt_mp->m_dentry设置本次挂载源mount的挂载点dentry
			attach_mnt(q, parent, p->mnt_mp);
			br_write_unlock(&vfsmount_lock);
		}
	}
	return res;
out:
	if (res) {
		br_write_lock(&vfsmount_lock);
		umount_tree(res, 0);
		br_write_unlock(&vfsmount_lock);
	}
	return q;
}

/* Caller should check returned pointer for errors */

struct vfsmount *collect_mounts(struct path *path)
{
	struct mount *tree;
	namespace_lock();
	tree = copy_tree(real_mount(path->mnt), path->dentry,
			 CL_COPY_ALL | CL_PRIVATE);
	namespace_unlock();
	if (IS_ERR(tree))
		return ERR_CAST(tree);
	return &tree->mnt;
}

void drop_collected_mounts(struct vfsmount *mnt)
{
	namespace_lock();
	br_write_lock(&vfsmount_lock);
	umount_tree(real_mount(mnt), 0);
	br_write_unlock(&vfsmount_lock);
	namespace_unlock();
}

int iterate_mounts(int (*f)(struct vfsmount *, void *), void *arg,
		   struct vfsmount *root)
{
	struct mount *mnt;
	int res = f(root, arg);
	if (res)
		return res;
	list_for_each_entry(mnt, &real_mount(root)->mnt_list, mnt_list) {
		res = f(&mnt->mnt, arg);
		if (res)
			return res;
	}
	return 0;
}

static void cleanup_group_ids(struct mount *mnt, struct mount *end)
{
	struct mount *p;

	for (p = mnt; p != end; p = next_mnt(p, mnt)) {
		if (p->mnt_group_id && !IS_MNT_SHARED(p))
			mnt_release_group_id(p);
	}
}
//遍历source_mnt树下的mount结构，如果这些mount不支持shared属性并且没有mount group id，那分配一个mount group id
static int invent_group_ids(struct mount *mnt, bool recurse)//recurse:true
{
	struct mount *p;
    //遍历mnt下mount树???
	for (p = mnt; p; p = recurse ? next_mnt(p, mnt) : NULL) {
        //如果p没有mount group id并且不支持共享
		if (!p->mnt_group_id && !IS_MNT_SHARED(p)) {
            //分配mount group id赋值给mnt->mnt_group_id
			int err = mnt_alloc_group_id(p);
			if (err) {
				cleanup_group_ids(mnt, p);
				return err;
			}
		}
	}

	return 0;
}

/*
 *  @source_mnt : mount tree to be attached
 *  @nd         : place the mount tree @source_mnt is attached
 *  @parent_nd  : if non-null, detach the source_mnt from its parent and
 *  		   store the parent mount and mountpoint dentry.
 *  		   (done when source_mnt is moved)
 *
 *  NOTE: in the table below explains the semantics when a source mount
 *  of a given type is attached to a destination mount of a given type.
 * ---------------------------------------------------------------------------
 * |         BIND MOUNT OPERATION                                            |
 * |**************************************************************************
 * | source-->| shared        |       private  |       slave    | unbindable |
 * | dest     |               |                |                |            |
 * |   |      |               |                |                |            |
 * |   v      |               |                |                |            |
 * |**************************************************************************
 * |  shared  | shared (++)   |     shared (+) |     shared(+++)|  invalid   |
 * |          |               |                |                |            |
 * |non-shared| shared (+)    |      private   |      slave (*) |  invalid   |
 * ***************************************************************************
 * A bind operation clones the source mount and mounts the clone on the
 * destination mount.
 *
 * (++)  the cloned mount is propagated to all the mounts in the propagation
 * 	 tree of the destination mount and the cloned mount is added to
 * 	 the peer group of the source mount.
 * (+)   the cloned mount is created under the destination mount and is marked
 *       as shared. The cloned mount is added to the peer group of the source
 *       mount.
 * (+++) the mount is propagated to all the mounts in the propagation tree
 *       of the destination mount and the cloned mount is made slave
 *       of the same master as that of the source mount. The cloned mount
 *       is marked as 'shared and slave'.
 * (*)   the cloned mount is made a slave of the same master as that of the
 * 	 source mount.
 *
 * ---------------------------------------------------------------------------
 * |         		MOVE MOUNT OPERATION                                 |
 * |**************************************************************************
 * | source-->| shared        |       private  |       slave    | unbindable |
 * | dest     |               |                |                |            |
 * |   |      |               |                |                |            |
 * |   v      |               |                |                |            |
 * |**************************************************************************
 * |  shared  | shared (+)    |     shared (+) |    shared(+++) |  invalid   |
 * |          |               |                |                |            |
 * |non-shared| shared (+*)   |      private   |    slave (*)   | unbindable |
 * ***************************************************************************
 *
 * (+)  the mount is moved to the destination. And is then propagated to
 * 	all the mounts in the propagation tree of the destination mount.
 * (+*)  the mount is moved to the destination.
 * (+++)  the mount is moved to the destination and is then propagated to
 * 	all the mounts belonging to the destination mount's propagation tree.
 * 	the mount is marked as 'shared and slave'.
 * (*)	the mount continues to be a slave at the new location.
 *
 * if the source mount is a tree, the operations explained above is
 * applied to each mount in the tree.
 * Must be called without spinlocks held, since this function can sleep
 * in allocations.
 */
static int attach_recursive_mnt(struct mount *source_mnt,
			struct mount *dest_mnt,
			struct mountpoint *dest_mp,
			struct path *parent_path)//一般parent_path为NULL
{
    //tree_list 链表
	LIST_HEAD(tree_list);
	struct mount *child, *p;
	int err;

	if (IS_MNT_SHARED(dest_mnt)) {//一般mount默认都是shared属性
	    //遍历source_mnt树下的mount结构，如果这些mount不支持shared属性并且没有mount group id，那分配一个mount group id
		err = invent_group_ids(source_mnt, true);
		if (err)
			goto out;
	}
    //遍历dest mount树下的slave  mount组或者share mount组的所有mount，每个这种mount作为dest mount^, 同时以source mount为克隆母体
    //克隆生成一个mount，作为source mount^，dest mount^和source mount^构成父子关系，二者不是本次mount 挂载的原始source mount和dest mount
    //只是中途生成的，有区别。这个就是传播mount:本次与dest mount同一个slave 或者share mount组的mount，要作为dest mount^，本次mount挂载的
    //原始source mount要作为克隆母体，一一为dest mount^们克隆生成一个source mount^，这就是mount组传播mount的原理。克隆生成的mount
    //添加到tree_list链表，下边执行commit_tree()再把这些mount链表添加到各个mount结构有关的链表。
	err = propagate_mnt(dest_mnt, dest_mp, source_mnt, &tree_list);
	if (err)
		goto out_cleanup_ids;

	br_write_lock(&vfsmount_lock);

	if (IS_MNT_SHARED(dest_mnt)) {
		for (p = source_mnt; p; p = next_mnt(p, source_mnt))
			set_mnt_shared(p);
	}
	if (parent_path) {//一般parent_path为NULL
		detach_mnt(source_mnt, parent_path);
		attach_mnt(source_mnt, dest_mnt, dest_mp);
		touch_mnt_namespace(source_mnt->mnt_ns);
	} else {
	    //设置source_mnt的挂载点目录dentry、mnt_parent竟然为挂点目录所在文件系统的mount，即dest_mnt，还有设置mountpoint
		mnt_set_mountpoint(dest_mnt, dest_mp, source_mnt);
        //把source_mnt这个mount结构添加到各个链表，设置mount的文件系统命名空间为父mount的命名空间
		commit_tree(source_mnt);
	}

	list_for_each_entry_safe(child, p, &tree_list, mnt_hash) {
		list_del_init(&child->mnt_hash);
        //把child这个mount结构添加到各个链表，设置mount的文件系统命名空间为父mount的命名空间
		commit_tree(child);
	}
	br_write_unlock(&vfsmount_lock);

	return 0;

 out_cleanup_ids:
	if (IS_MNT_SHARED(dest_mnt))
		cleanup_group_ids(source_mnt, NULL);
 out:
	return err;
}
//path代表了挂载点目录，比如本次/dev/sda0块设备要挂载到的/mnt目录，遍历/mnt获取挂载点目录的vfsmount和根目录dentry。
//如果之前sda1，sda2，sda3陆续挂载到了/mnt，所以要遍历sda1、sda2、sda3找到最后一次挂载到/mnt的sda3块设备mount结构的vfsmount
//和根目录dentry，然后赋予mountpoint并返回。如果/mnt目录之前没有挂载块设备，则是找到/mnt目录原始的dentry的赋予mountpoint并返回
static struct mountpoint *lock_mount(struct path *path)
{
	struct vfsmount *mnt;
    //得到挂载点的dentry
	struct dentry *dentry = path->dentry;
retry:
	mutex_lock(&dentry->d_inode->i_mutex);
	if (unlikely(cant_mount(dentry))) {
		mutex_unlock(&dentry->d_inode->i_mutex);
		return ERR_PTR(-ENOENT);
	}
	namespace_lock();
//比如本次mount /dev/sda0 /mnt,而之前已经有sda1、sda2、sda3依次挂载到了/mmt，__lookup_mnt()这里边依次找到sda1、sda2、sda3的mount并返回
//mount结构的vfsmount给mnt，这样下边执行goto retry接着循环，直到最后一次是返回NULL，此时才开始处理mount /dev/sda0 /mnt的mount
	mnt = lookup_mnt(path);
    //如果mnt不为NULL，下边会执行goto retry继续循环，直到lookup_mnt返回NULL if才成立
	if (likely(!mnt)) {
        //为本次mount分配一个新的mountpoint，然后mp->m_dentry = dentry，如果/mnt目录之前没有挂载块设备，则dentry就是/mnt目录的
        //现在sda1、sda2、sda3依次挂载到了/mmt，此时dentry是sda3块设备的根目录dentry
		struct mountpoint *mp = new_mountpoint(dentry);
		if (IS_ERR(mp)) {
			namespace_unlock();
			mutex_unlock(&dentry->d_inode->i_mutex);
			return mp;
		}
		return mp;
	}
	namespace_unlock();
	mutex_unlock(&path->dentry->d_inode->i_mutex);
	path_put(path);
    //path->mnt依次保存sda1、sda2、sda3挂载到/mnt目录时的vfsmount
	path->mnt = mnt;
    //dentry和path->dentry都保存mnt->mnt_root，即依次是sda1、sda2、sda3块设备的根目录dentry,最后一次是sda3块设备的根目录dentry
	dentry = path->dentry = dget(mnt->mnt_root);
    //调到retry循环
	goto retry;
}

static void unlock_mount(struct mountpoint *where)
{
	struct dentry *dentry = where->m_dentry;
	put_mountpoint(where);
	namespace_unlock();
	mutex_unlock(&dentry->d_inode->i_mutex);
}

static int graft_tree(struct mount *mnt, struct mount *p, struct mountpoint *mp)
{
	if (mnt->mnt.mnt_sb->s_flags & MS_NOUSER)
		return -EINVAL;

	if (S_ISDIR(mp->m_dentry->d_inode->i_mode) !=
	      S_ISDIR(mnt->mnt.mnt_root->d_inode->i_mode))
		return -ENOTDIR;

	return attach_recursive_mnt(mnt, p, mp, NULL);
}

/*
 * Sanity check the flags to change_mnt_propagation.
 */

static int flags_to_propagation_type(int flags)
{
	int type = flags & ~(MS_REC | MS_SILENT);

	/* Fail if any non-propagation flags are set */
	if (type & ~(MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE))
		return 0;
	/* Only one propagation flag should be set */
	if (!is_power_of_2(type))
		return 0;
	return type;
}

/*
 * recursively change the type of the mountpoint.
 */
static int do_change_type(struct path *path, int flag)
{
	struct mount *m;
	struct mount *mnt = real_mount(path->mnt);
	int recurse = flag & MS_REC;
	int type;
	int err = 0;

	if (path->dentry != path->mnt->mnt_root)
		return -EINVAL;

	type = flags_to_propagation_type(flag);
	if (!type)
		return -EINVAL;

	namespace_lock();
	if (type == MS_SHARED) {
		err = invent_group_ids(mnt, recurse);
		if (err)
			goto out_unlock;
	}

	br_write_lock(&vfsmount_lock);
	for (m = mnt; m; m = (recurse ? next_mnt(m, mnt) : NULL))
		change_mnt_propagation(m, type);
	br_write_unlock(&vfsmount_lock);

 out_unlock:
	namespace_unlock();
	return err;
}

/*
 * do loopback mount.
 */
//mount --bind /home/  /home/test  path代表/home/test挂载点目录 ，old_name是/home/ 挂载源目录
static int do_loopback(struct path *path, const char *old_name,
				int recurse)
{
	struct path old_path;
	struct mount *mnt = NULL, *old, *parent;
	struct mountpoint *mp;
	int err;
	if (!old_name || !*old_name)
		return -EINVAL;
    //探测/home/挂载源，得到该目录的所在文件系统的vfsmount和目录dentry信息保存到old_path。如果/home/目录已经有块设备挂载了
    //则要把vfsmount和dentry转成挂载的块设备文件系统的vfsmount和根目录dentry保存到old_path。挂载源的目录也要刨根问底
	err = kern_path(old_name, LOOKUP_FOLLOW|LOOKUP_AUTOMOUNT, &old_path);
	if (err)
		return err;

	err = -EINVAL;
    //比较命名空间
	if (mnt_ns_loop(&old_path))
		goto out; 
    //遍历/home/test获取挂载点目录，由于该目录之前可能有其他块设备挂载，所以要遍历找到最后一次挂载的块设备的vfsmount和根目录dentry
 /*lock_mount()验证下来就没什么用，因为path.mnt和path.dentry就已经是挂载点目录的终极mount和dentry。显然在前边do_mount->path_init()
 函数中得到path.mnt和path.dentry已经是挂载点目录的最后一次挂载的文件系统(块设备或者tmpfs等)，所以lock_mount()->lookup_mnt()->__lookup_mnt()
 ，__lookup_mnt()直接返回NULL，即没有mount，lock_mount()的作用仅仅是mp->dentry=path.dentry设置本次的挂载点目录dentry就返回了。*/
	mp = lock_mount(path);
	err = PTR_ERR(mp);
	if (IS_ERR(mp))
		goto out;
    // /home/ 挂载源目录所处文件系统的struct mount，这个mount是之前的挂载生成的
	old = real_mount(old_path.mnt);
    // /home/test 挂载点目录所处文件系统的struct mount，这个mount是之前的挂载生成的
	parent = real_mount(path->mnt);

	err = -EINVAL;
	if (IS_MNT_UNBINDABLE(old))
		goto out2;

	if (!check_mnt(parent) || !check_mnt(old))
		goto out2;

	if (recurse)
		mnt = copy_tree(old, old_path.dentry, 0);
	else/*克隆创建本次挂载的mount结构，即本次的source mount，大部分成员复制了old的，old是mount bind源目录所在文件系统的mount结构*/
//假设源目录/home，则old是/home/所在文件系统挂载生成的struct mount，old_path.dentry是/home挂载源的。如果/home/目录已经有块设备挂载了，
//则要转换成之前挂载的块设备的mount结构，赋予old，然后是它根目录dentry保存到old_path.dentry。mount和目录dentry刨根问底到最后的块设备
		mnt = clone_mnt(old, old_path.dentry, 0);

	if (IS_ERR(mnt)) {
		err = PTR_ERR(mnt);
		goto out2;
	}
    //mnt:本次mount操作创建source mount，parent dest mount，mp挂载点
	err = graft_tree(mnt, parent, mp);
	if (err) {
		br_write_lock(&vfsmount_lock);
		umount_tree(mnt, 0);
		br_write_unlock(&vfsmount_lock);
	}
out2:
	unlock_mount(mp);
out:
	path_put(&old_path);
	return err;
}

static int change_mount_flags(struct vfsmount *mnt, int ms_flags)
{
	int error = 0;
	int readonly_request = 0;

	if (ms_flags & MS_RDONLY)
		readonly_request = 1;
	if (readonly_request == __mnt_is_readonly(mnt))
		return 0;

	if (readonly_request)
		error = mnt_make_readonly(real_mount(mnt));
	else
		__mnt_unmake_readonly(real_mount(mnt));
	return error;
}

/*
 * change filesystem flags. dir should be a physical root of filesystem.
 * If you've mounted a non-root directory somewhere and want to do remount
 * on it - tough luck.
 */
static int do_remount(struct path *path, int flags, int mnt_flags,
		      void *data)
{
	int err;
	struct super_block *sb = path->mnt->mnt_sb;
	struct mount *mnt = real_mount(path->mnt);

	if (!check_mnt(mnt))
		return -EINVAL;

	if (path->dentry != path->mnt->mnt_root)
		return -EINVAL;

	/* Don't allow changing of locked mnt flags.
	 *
	 * No locks need to be held here while testing the various
	 * MNT_LOCK flags because those flags can never be cleared
	 * once they are set.
	 */
	if ((mnt->mnt.mnt_flags & MNT_LOCK_READONLY) &&
	    !(mnt_flags & MNT_READONLY)) {
		return -EPERM;
	}
	if ((mnt->mnt.mnt_flags & MNT_LOCK_NODEV) &&
	    !(mnt_flags & MNT_NODEV)) {
		/* Was the nodev implicitly added in mount? */
		if ((mnt->mnt_ns->user_ns != &init_user_ns) &&
		    !(sb->s_type->fs_flags & FS_USERNS_DEV_MOUNT)) {
			mnt_flags |= MNT_NODEV;
		} else {
			return -EPERM;
		}
	}
	if ((mnt->mnt.mnt_flags & MNT_LOCK_NOSUID) &&
	    !(mnt_flags & MNT_NOSUID)) {
		return -EPERM;
	}
	if ((mnt->mnt.mnt_flags & MNT_LOCK_NOEXEC) &&
	    !(mnt_flags & MNT_NOEXEC)) {
		return -EPERM;
	}
	if ((mnt->mnt.mnt_flags & MNT_LOCK_ATIME) &&
	    ((mnt->mnt.mnt_flags & MNT_ATIME_MASK) != (mnt_flags & MNT_ATIME_MASK))) {
		return -EPERM;
	}

	err = security_sb_remount(sb, data);
	if (err)
		return err;

	down_write(&sb->s_umount);
	if (flags & MS_BIND)
		err = change_mount_flags(path->mnt, flags);
	else if (!capable(CAP_SYS_ADMIN))
		err = -EPERM;
	else
		err = do_remount_sb(sb, flags, data, 0);
	if (!err) {
		br_write_lock(&vfsmount_lock);
		mnt_flags |= mnt->mnt.mnt_flags & ~MNT_USER_SETTABLE_MASK;
		mnt->mnt.mnt_flags = mnt_flags;
		br_write_unlock(&vfsmount_lock);
	}
	up_write(&sb->s_umount);
	if (!err) {
		br_write_lock(&vfsmount_lock);
		touch_mnt_namespace(mnt->mnt_ns);
		br_write_unlock(&vfsmount_lock);
	}
	return err;
}

static inline int tree_contains_unbindable(struct mount *mnt)
{
	struct mount *p;
	for (p = mnt; p; p = next_mnt(p, mnt)) {
		if (IS_MNT_UNBINDABLE(p))
			return 1;
	}
	return 0;
}

static int do_move_mount(struct path *path, const char *old_name)
{
	struct path old_path, parent_path;
	struct mount *p;
	struct mount *old;
	struct mountpoint *mp;
	int err;
	if (!old_name || !*old_name)
		return -EINVAL;
	err = kern_path(old_name, LOOKUP_FOLLOW, &old_path);
	if (err)
		return err;

	mp = lock_mount(path);
	err = PTR_ERR(mp);
	if (IS_ERR(mp))
		goto out;

	old = real_mount(old_path.mnt);
	p = real_mount(path->mnt);

	err = -EINVAL;
	if (!check_mnt(p) || !check_mnt(old))
		goto out1;

	err = -EINVAL;
	if (old_path.dentry != old_path.mnt->mnt_root)
		goto out1;

	if (!mnt_has_parent(old))
		goto out1;

	if (S_ISDIR(path->dentry->d_inode->i_mode) !=
	      S_ISDIR(old_path.dentry->d_inode->i_mode))
		goto out1;
	/*
	 * Don't move a mount residing in a shared parent.
	 */
	if (IS_MNT_SHARED(old->mnt_parent))
		goto out1;
	/*
	 * Don't move a mount tree containing unbindable mounts to a destination
	 * mount which is shared.
	 */
	if (IS_MNT_SHARED(p) && tree_contains_unbindable(old))
		goto out1;
	err = -ELOOP;
	for (; mnt_has_parent(p); p = p->mnt_parent)
		if (p == old)
			goto out1;

	err = attach_recursive_mnt(old, real_mount(path->mnt), mp, &parent_path);
	if (err)
		goto out1;

	/* if the mount is moved, it should no longer be expire
	 * automatically */
	list_del_init(&old->mnt_expire);
out1:
	unlock_mount(mp);
out:
	if (!err)
		path_put(&parent_path);
	path_put(&old_path);
	return err;
}

static struct vfsmount *fs_set_subtype(struct vfsmount *mnt, const char *fstype)
{
	int err;
	const char *subtype = strchr(fstype, '.');
	if (subtype) {
		subtype++;
		err = -EINVAL;
		if (!subtype[0])
			goto err;
	} else
		subtype = "";

	mnt->mnt_sb->s_subtype = kstrdup(subtype, GFP_KERNEL);
	err = -ENOMEM;
	if (!mnt->mnt_sb->s_subtype)
		goto err;
	return mnt;

 err:
	mntput(mnt);
	return ERR_PTR(err);
}

/*
 * add a mount into a namespace's mount tree
 */
//path是安装点的，newmnt是挂载源块设备
static int do_add_mount(struct mount *newmnt, struct path *path, int mnt_flags)
{
	struct mountpoint *mp;
	struct mount *parent;
	int err;

	mnt_flags &= ~(MNT_SHARED | MNT_WRITE_HOLD | MNT_INTERNAL);
    //得到挂载点目录的struct mountpoint *mp
    /*lock_mount()验证下来就没什么用，因为path.mnt和path.dentry就已经是挂载点目录的终极mount和dentry。显然在前边do_mount->path_init()
    函数中得到path.mnt和path.dentry已经是挂载点目录的最后一次挂载的文件系统(块设备或者tmpfs等)，所以lock_mount()->lookup_mnt()->__lookup_mnt()
    ，__lookup_mnt()直接返回NULL，即没有mount，lock_mount()的作用仅仅是mp->dentry=path.dentry设置本次的挂载点目录dentry就返回了。*/
	mp = lock_mount(path);
	if (IS_ERR(mp))
		return PTR_ERR(mp);
    //挂载点目录所处文件系统的mount结构
	parent = real_mount(path->mnt);
	err = -EINVAL;
	if (unlikely(!check_mnt(parent))) {
		/* that's acceptable only for automounts done in private ns */
		if (!(mnt_flags & MNT_SHRINKABLE))
			goto unlock;
		/* ... and for those we'd better have mountpoint still alive */
		if (!parent->mnt_ns)
			goto unlock;
	}

	/* Refuse the same filesystem on the same mount point */
	err = -EBUSY;
    //path->mnt->mnt_sb:挂载点目录所在文件系统的超级快  newmnt->mnt.mnt_sb 挂载源块设备所在文件系统的超级块
    //path->mnt->mnt_root:挂载点目录所在文件系统的根目录   path->dentry 挂载点目录dentry。
    
    //因为挂载点目录遍历时，会转换成挂载源的块设备的，所以path->mnt其实代表挂载源的块设备，newmnt->mnt 也是挂载源的。
    //并且path->mnt->mnt_root==path->dentry则说明。本次挂在点目录就是挂载源块设备的根目录，这就是说同一个块设备又挂载到同一个块设备
	if (path->mnt->mnt_sb == newmnt->mnt.mnt_sb &&
	    path->mnt->mnt_root == path->dentry)
		goto unlock;

	err = -EINVAL;
	if (S_ISLNK(newmnt->mnt.mnt_root->d_inode->i_mode))
		goto unlock;

	newmnt->mnt.mnt_flags = mnt_flags;
    //newmnt:块设备ext4文件系统的mount结构  parent:挂载点文件系统的mount,mp挂载点目录的mp
	err = graft_tree(newmnt, parent, mp);

unlock:
	unlock_mount(mp);
	return err;
}

/*
 * create a new mount for userspace and request it to be added into the
 * namespace's tree
 */
//path是挂载点点目录
static int do_new_mount(struct path *path, const char *fstype, int flags,
			int mnt_flags, const char *name, void *data)
{
	struct file_system_type *type;
	struct user_namespace *user_ns = current->nsproxy->mnt_ns->user_ns;
	struct vfsmount *mnt;
	int err;

	if (!fstype)
		return -EINVAL;

	type = get_fs_type(fstype);
	if (!type)
		return -ENODEV;

	if (user_ns != &init_user_ns) {
		if (!(type->fs_flags & FS_USERNS_MOUNT)) {
			put_filesystem(type);
			return -EPERM;
		}
		/* Only in special cases allow devices from mounts
		 * created outside the initial user namespace.
		 */
		if (!(type->fs_flags & FS_USERNS_DEV_MOUNT)) {
			flags |= MS_NODEV;
			mnt_flags |= MNT_NODEV | MNT_LOCK_NODEV;
		}
	}
    /*
     查找到/dev/mmcblk0p5的dentry、mnt、inode结构，并由inode得到块设备的bdev，创建superblock
     结构并初始化其成员。然后读取ext4文件系统flash中的超级快数据，并分析超级快数据，给
     ext4_super_block、ext4_sb_info、super_block赋值。得到ext4文件系统的root inode，
     以"/"目录名字分配并初始化root dentry，并对vfsmont初始化
     */
	mnt = vfs_kern_mount(type, flags, name, data);
	if (!IS_ERR(mnt) && (type->fs_flags & FS_HAS_SUBTYPE) &&
	    !mnt->mnt_sb->s_subtype)
		mnt = fs_set_subtype(mnt, fstype);

	put_filesystem(type);
	if (IS_ERR(mnt))
		return PTR_ERR(mnt);

	err = do_add_mount(real_mount(mnt), path, mnt_flags);
	if (err)
		mntput(mnt);
	return err;
}

int finish_automount(struct vfsmount *m, struct path *path)
{
	struct mount *mnt = real_mount(m);
	int err;
	/* The new mount record should have at least 2 refs to prevent it being
	 * expired before we get a chance to add it
	 */
	BUG_ON(mnt_get_count(mnt) < 2);

	if (m->mnt_sb == path->mnt->mnt_sb &&
	    m->mnt_root == path->dentry) {
		err = -ELOOP;
		goto fail;
	}

	err = do_add_mount(mnt, path, path->mnt->mnt_flags | MNT_SHRINKABLE);
	if (!err)
		return 0;
fail:
	/* remove m from any expiration list it may be on */
	if (!list_empty(&mnt->mnt_expire)) {
		namespace_lock();
		br_write_lock(&vfsmount_lock);
		list_del_init(&mnt->mnt_expire);
		br_write_unlock(&vfsmount_lock);
		namespace_unlock();
	}
	mntput(m);
	mntput(m);
	return err;
}

/**
 * mnt_set_expiry - Put a mount on an expiration list
 * @mnt: The mount to list.
 * @expiry_list: The list to add the mount to.
 */
void mnt_set_expiry(struct vfsmount *mnt, struct list_head *expiry_list)
{
	namespace_lock();
	br_write_lock(&vfsmount_lock);

	list_add_tail(&real_mount(mnt)->mnt_expire, expiry_list);

	br_write_unlock(&vfsmount_lock);
	namespace_unlock();
}
EXPORT_SYMBOL(mnt_set_expiry);

/*
 * process a list of expirable mountpoints with the intent of discarding any
 * mountpoints that aren't in use and haven't been touched since last we came
 * here
 */
void mark_mounts_for_expiry(struct list_head *mounts)
{
	struct mount *mnt, *next;
	LIST_HEAD(graveyard);

	if (list_empty(mounts))
		return;

	namespace_lock();
	br_write_lock(&vfsmount_lock);

	/* extract from the expiration list every vfsmount that matches the
	 * following criteria:
	 * - only referenced by its parent vfsmount
	 * - still marked for expiry (marked on the last call here; marks are
	 *   cleared by mntput())
	 */
	list_for_each_entry_safe(mnt, next, mounts, mnt_expire) {
		if (!xchg(&mnt->mnt_expiry_mark, 1) ||
			propagate_mount_busy(mnt, 1))
			continue;
		list_move(&mnt->mnt_expire, &graveyard);
	}
	while (!list_empty(&graveyard)) {
		mnt = list_first_entry(&graveyard, struct mount, mnt_expire);
		touch_mnt_namespace(mnt->mnt_ns);
		umount_tree(mnt, 1);
	}
	br_write_unlock(&vfsmount_lock);
	namespace_unlock();
}

EXPORT_SYMBOL_GPL(mark_mounts_for_expiry);

/*
 * Ripoff of 'select_parent()'
 *
 * search the list of submounts for a given mountpoint, and move any
 * shrinkable submounts to the 'graveyard' list.
 */
static int select_submounts(struct mount *parent, struct list_head *graveyard)
{
	struct mount *this_parent = parent;
	struct list_head *next;
	int found = 0;

repeat:
	next = this_parent->mnt_mounts.next;
resume:
	while (next != &this_parent->mnt_mounts) {
		struct list_head *tmp = next;
		struct mount *mnt = list_entry(tmp, struct mount, mnt_child);

		next = tmp->next;
		if (!(mnt->mnt.mnt_flags & MNT_SHRINKABLE))
			continue;
		/*
		 * Descend a level if the d_mounts list is non-empty.
		 */
		if (!list_empty(&mnt->mnt_mounts)) {
			this_parent = mnt;
			goto repeat;
		}

		if (!propagate_mount_busy(mnt, 1)) {
			list_move_tail(&mnt->mnt_expire, graveyard);
			found++;
		}
	}
	/*
	 * All done at this level ... ascend and resume the search
	 */
	if (this_parent != parent) {
		next = this_parent->mnt_child.next;
		this_parent = this_parent->mnt_parent;
		goto resume;
	}
	return found;
}

/*
 * process a list of expirable mountpoints with the intent of discarding any
 * submounts of a specific parent mountpoint
 *
 * vfsmount_lock must be held for write
 */
static void shrink_submounts(struct mount *mnt)
{
	LIST_HEAD(graveyard);
	struct mount *m;

	/* extract submounts of 'mountpoint' from the expiration list */
	while (select_submounts(mnt, &graveyard)) {
		while (!list_empty(&graveyard)) {
			m = list_first_entry(&graveyard, struct mount,
						mnt_expire);
			touch_mnt_namespace(m->mnt_ns);
			umount_tree(m, 1);
		}
	}
}

/*
 * Some copy_from_user() implementations do not return the exact number of
 * bytes remaining to copy on a fault.  But copy_mount_options() requires that.
 * Note that this function differs from copy_from_user() in that it will oops
 * on bad values of `to', rather than returning a short copy.
 */
static long exact_copy_from_user(void *to, const void __user * from,
				 unsigned long n)
{
	char *t = to;
	const char __user *f = from;
	char c;

	if (!access_ok(VERIFY_READ, from, n))
		return n;

	while (n) {
		if (__get_user(c, f)) {
			memset(t, 0, n);
			break;
		}
		*t++ = c;
		f++;
		n--;
	}
	return n;
}

int copy_mount_options(const void __user * data, unsigned long *where)
{
	int i;
	unsigned long page;
	unsigned long size;

	*where = 0;
	if (!data)
		return 0;

	if (!(page = __get_free_page(GFP_KERNEL)))
		return -ENOMEM;

	/* We only care that *some* data at the address the user
	 * gave us is valid.  Just in case, we'll zero
	 * the remainder of the page.
	 */
	/* copy_from_user cannot cross TASK_SIZE ! */
	size = TASK_SIZE - (unsigned long)data;
	if (size > PAGE_SIZE)
		size = PAGE_SIZE;

	i = size - exact_copy_from_user((void *)page, data, size);
	if (!i) {
		free_page(page);
		return -EFAULT;
	}
	if (i != PAGE_SIZE)
		memset((char *)page + i, 0, PAGE_SIZE - i);
	*where = page;
	return 0;
}

int copy_mount_string(const void __user *data, char **where)
{
	char *tmp;

	if (!data) {
		*where = NULL;
		return 0;
	}

	tmp = strndup_user(data, PAGE_SIZE);
	if (IS_ERR(tmp))
		return PTR_ERR(tmp);

	*where = tmp;
	return 0;
}

/*
 * Flags is a 32-bit value that allows up to 31 non-fs dependent flags to
 * be given to the mount() call (ie: read-only, no-dev, no-suid etc).
 *
 * data is a (void *) that can point to any structure up to
 * PAGE_SIZE-1 bytes, which can contain arbitrary fs-dependent
 * information (or be NULL).
 *
 * Pre-0.97 versions of mount() didn't have a flags word.
 * When the flags word was introduced its top half was required
 * to have the magic value 0xC0ED, and this remained so until 2.4.0-test9.
 * Therefore, if this magic number is present, it carries no information
 * and must be discarded.
 */
long do_mount(const char *dev_name, const char *dir_name,
		const char *type_page, unsigned long flags, void *data_page)
{
	struct path path;
	int retval = 0;
	int mnt_flags = 0;

	/* Discard magic */
	if ((flags & MS_MGC_MSK) == MS_MGC_VAL)
		flags &= ~MS_MGC_MSK;

	/* Basic sanity checks */

	if (!dir_name || !*dir_name || !memchr(dir_name, 0, PAGE_SIZE))
		return -EINVAL;

	if (data_page)
		((char *)data_page)[PAGE_SIZE - 1] = 0;

	/* ... and get the mountpoint */
    /*得到安装点的dentry等信息，比如，mount /dev/mmcblk0p5 /home/config/,就是得到
      /home/config的dentry信息，这个过程与open类似，最终核心还是调用path_init和
      link_path_walk两个函数，根据dir_name文件路径名查找目录，最终得到/home/config/
      的dentry和vfsmount信息保存到path结构*/
	retval = kern_path(dir_name, LOOKUP_FOLLOW, &path);
	if (retval)
		return retval;

	retval = security_sb_mount(dev_name, &path,
				   type_page, flags, data_page);
	if (!retval && !may_mount())
		retval = -EPERM;
	if (retval)
		goto dput_out;

	/* Default to relatime unless overriden */
	if (!(flags & MS_NOATIME))
		mnt_flags |= MNT_RELATIME;

	/* Separate the per-mountpoint flags */
	if (flags & MS_NOSUID)
		mnt_flags |= MNT_NOSUID;
	if (flags & MS_NODEV)
		mnt_flags |= MNT_NODEV;
	if (flags & MS_NOEXEC)
		mnt_flags |= MNT_NOEXEC;
	if (flags & MS_NOATIME)
		mnt_flags |= MNT_NOATIME;
	if (flags & MS_NODIRATIME)
		mnt_flags |= MNT_NODIRATIME;
	if (flags & MS_STRICTATIME)
		mnt_flags &= ~(MNT_RELATIME | MNT_NOATIME);
	if (flags & MS_RDONLY)
		mnt_flags |= MNT_READONLY;

	/* The default atime for remount is preservation */
	if ((flags & MS_REMOUNT) &&
	    ((flags & (MS_NOATIME | MS_NODIRATIME | MS_RELATIME |
		       MS_STRICTATIME)) == 0)) {
		mnt_flags &= ~MNT_ATIME_MASK;
		mnt_flags |= path.mnt->mnt_flags & MNT_ATIME_MASK;
	}

	flags &= ~(MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_ACTIVE | MS_BORN |
		   MS_NOATIME | MS_NODIRATIME | MS_RELATIME| MS_KERNMOUNT |
		   MS_STRICTATIME);

	if (flags & MS_REMOUNT)
		retval = do_remount(&path, flags & ~MS_REMOUNT, mnt_flags,
				    data_page);
	else if (flags & MS_BIND)
		retval = do_loopback(&path, dev_name, flags & MS_REC);
	else if (flags & (MS_SHARED | MS_PRIVATE | MS_SLAVE | MS_UNBINDABLE))
		retval = do_change_type(&path, flags);
	else if (flags & MS_MOVE)
		retval = do_move_mount(&path, dev_name);
	else//path.dentry是/home/config的，dev_name是/dev/mmcblk0p4的
		retval = do_new_mount(&path, type_page, flags, mnt_flags,
				      dev_name, data_page);
dput_out:
	path_put(&path);
	return retval;
}

static void free_mnt_ns(struct mnt_namespace *ns)
{
	proc_free_inum(ns->proc_inum);
	put_user_ns(ns->user_ns);
	kfree(ns);
}

/*
 * Assign a sequence number so we can detect when we attempt to bind
 * mount a reference to an older mount namespace into the current
 * mount namespace, preventing reference counting loops.  A 64bit
 * number incrementing at 10Ghz will take 12,427 years to wrap which
 * is effectively never, so we can ignore the possibility.
 */
static atomic64_t mnt_ns_seq = ATOMIC64_INIT(1);

static struct mnt_namespace *alloc_mnt_ns(struct user_namespace *user_ns)
{
	struct mnt_namespace *new_ns;
	int ret;

	new_ns = kmalloc(sizeof(struct mnt_namespace), GFP_KERNEL);
	if (!new_ns)
		return ERR_PTR(-ENOMEM);
	ret = proc_alloc_inum(&new_ns->proc_inum);
	if (ret) {
		kfree(new_ns);
		return ERR_PTR(ret);
	}
	new_ns->seq = atomic64_add_return(1, &mnt_ns_seq);
	atomic_set(&new_ns->count, 1);
	new_ns->root = NULL;
	INIT_LIST_HEAD(&new_ns->list);
	init_waitqueue_head(&new_ns->poll);
	new_ns->event = 0;
	new_ns->user_ns = get_user_ns(user_ns);
	return new_ns;
}

/*
 * Allocate a new namespace structure and populate it with contents
 * copied from the namespace of the passed in task structure.
 */
static struct mnt_namespace *dup_mnt_ns(struct mnt_namespace *mnt_ns,
		struct user_namespace *user_ns, struct fs_struct *fs)
{
	struct mnt_namespace *new_ns;
	struct vfsmount *rootmnt = NULL, *pwdmnt = NULL;
	struct mount *p, *q;
	struct mount *old = mnt_ns->root;
	struct mount *new;
	int copy_flags;

	new_ns = alloc_mnt_ns(user_ns);
	if (IS_ERR(new_ns))
		return new_ns;

	namespace_lock();
	/* First pass: copy the tree topology */
	copy_flags = CL_COPY_ALL | CL_EXPIRE;
	if (user_ns != mnt_ns->user_ns)
		copy_flags |= CL_SHARED_TO_SLAVE | CL_UNPRIVILEGED;
	new = copy_tree(old, old->mnt.mnt_root, copy_flags);
	if (IS_ERR(new)) {
		namespace_unlock();
		free_mnt_ns(new_ns);
		return ERR_CAST(new);
	}
	new_ns->root = new;
	br_write_lock(&vfsmount_lock);
	list_add_tail(&new_ns->list, &new->mnt_list);
	br_write_unlock(&vfsmount_lock);

	/*
	 * Second pass: switch the tsk->fs->* elements and mark new vfsmounts
	 * as belonging to new namespace.  We have already acquired a private
	 * fs_struct, so tsk->fs->lock is not needed.
	 */
	p = old;
	q = new;
	while (p) {
		q->mnt_ns = new_ns;
		if (fs) {
			if (&p->mnt == fs->root.mnt) {
				fs->root.mnt = mntget(&q->mnt);
				rootmnt = &p->mnt;
			}
			if (&p->mnt == fs->pwd.mnt) {
				fs->pwd.mnt = mntget(&q->mnt);
				pwdmnt = &p->mnt;
			}
		}
		p = next_mnt(p, old);
		q = next_mnt(q, new);
	}
	namespace_unlock();

	if (rootmnt)
		mntput(rootmnt);
	if (pwdmnt)
		mntput(pwdmnt);

	return new_ns;
}

struct mnt_namespace *copy_mnt_ns(unsigned long flags, struct mnt_namespace *ns,
		struct user_namespace *user_ns, struct fs_struct *new_fs)
{
	struct mnt_namespace *new_ns;

	BUG_ON(!ns);
	get_mnt_ns(ns);

	if (!(flags & CLONE_NEWNS))
		return ns;

	new_ns = dup_mnt_ns(ns, user_ns, new_fs);

	put_mnt_ns(ns);
	return new_ns;
}

/**
 * create_mnt_ns - creates a private namespace and adds a root filesystem
 * @mnt: pointer to the new root filesystem mountpoint
 */
static struct mnt_namespace *create_mnt_ns(struct vfsmount *m)
{
	struct mnt_namespace *new_ns = alloc_mnt_ns(&init_user_ns);
	if (!IS_ERR(new_ns)) {
		struct mount *mnt = real_mount(m);
		mnt->mnt_ns = new_ns;
		new_ns->root = mnt;
		list_add(&mnt->mnt_list, &new_ns->list);
	} else {
		mntput(m);
	}
	return new_ns;
}

struct dentry *mount_subtree(struct vfsmount *mnt, const char *name)
{
	struct mnt_namespace *ns;
	struct super_block *s;
	struct path path;
	int err;

	ns = create_mnt_ns(mnt);
	if (IS_ERR(ns))
		return ERR_CAST(ns);

	err = vfs_path_lookup(mnt->mnt_root, mnt,
			name, LOOKUP_FOLLOW|LOOKUP_AUTOMOUNT, &path);

	put_mnt_ns(ns);

	if (err)
		return ERR_PTR(err);

	/* trade a vfsmount reference for active sb one */
	s = path.mnt->mnt_sb;
	atomic_inc(&s->s_active);
	mntput(path.mnt);
	/* lock the sucker */
	down_write(&s->s_umount);
	/* ... and return the root of (sub)tree on it */
	return path.dentry;
}
EXPORT_SYMBOL(mount_subtree);

SYSCALL_DEFINE5(mount, char __user *, dev_name, char __user *, dir_name,
		char __user *, type, unsigned long, flags, void __user *, data)
{
	int ret;
	char *kernel_type;
	struct filename *kernel_dir;
	char *kernel_dev;
	unsigned long data_page;

	ret = copy_mount_string(type, &kernel_type);
	if (ret < 0)
		goto out_type;

	kernel_dir = getname(dir_name);
	if (IS_ERR(kernel_dir)) {
		ret = PTR_ERR(kernel_dir);
		goto out_dir;
	}

	ret = copy_mount_string(dev_name, &kernel_dev);
	if (ret < 0)
		goto out_dev;

	ret = copy_mount_options(data, &data_page);
	if (ret < 0)
		goto out_data;

	ret = do_mount(kernel_dev, kernel_dir->name, kernel_type, flags,
		(void *) data_page);

	free_page(data_page);
out_data:
	kfree(kernel_dev);
out_dev:
	putname(kernel_dir);
out_dir:
	kfree(kernel_type);
out_type:
	return ret;
}

/*
 * Return true if path is reachable from root
 *
 * namespace_sem or vfsmount_lock is held
 */
bool is_path_reachable(struct mount *mnt, struct dentry *dentry,
			 const struct path *root)
{
	while (&mnt->mnt != root->mnt && mnt_has_parent(mnt)) {
		dentry = mnt->mnt_mountpoint;
		mnt = mnt->mnt_parent;
	}
	return &mnt->mnt == root->mnt && is_subdir(dentry, root->dentry);
}

int path_is_under(struct path *path1, struct path *path2)
{
	int res;
	br_read_lock(&vfsmount_lock);
	res = is_path_reachable(real_mount(path1->mnt), path1->dentry, path2);
	br_read_unlock(&vfsmount_lock);
	return res;
}
EXPORT_SYMBOL(path_is_under);

/*
 * pivot_root Semantics:
 * Moves the root file system of the current process to the directory put_old,
 * makes new_root as the new root file system of the current process, and sets
 * root/cwd of all processes which had them on the current root to new_root.
 *
 * Restrictions:
 * The new_root and put_old must be directories, and  must not be on the
 * same file  system as the current process root. The put_old  must  be
 * underneath new_root,  i.e. adding a non-zero number of /.. to the string
 * pointed to by put_old must yield the same directory as new_root. No other
 * file system may be mounted on put_old. After all, new_root is a mountpoint.
 *
 * Also, the current root cannot be on the 'rootfs' (initial ramfs) filesystem.
 * See Documentation/filesystems/ramfs-rootfs-initramfs.txt for alternatives
 * in this situation.
 *
 * Notes:
 *  - we don't move root/cwd if they are not at the root (reason: if something
 *    cared enough to change them, it's probably wrong to force them elsewhere)
 *  - it's okay to pick a root that isn't the root of a file system, e.g.
 *    /nfs/my_root where /nfs is the mount point. It must be a mountpoint,
 *    though, so you may need to say mount --bind /nfs/my_root /nfs/my_root
 *    first.
 */
SYSCALL_DEFINE2(pivot_root, const char __user *, new_root,
		const char __user *, put_old)
{
	struct path new, old, parent_path, root_parent, root;
	struct mount *new_mnt, *root_mnt, *old_mnt;
	struct mountpoint *old_mp, *root_mp;
	int error;

	if (!may_mount())
		return -EPERM;

	error = user_path_dir(new_root, &new);
	if (error)
		goto out0;

	error = user_path_dir(put_old, &old);
	if (error)
		goto out1;

	error = security_sb_pivotroot(&old, &new);
	if (error)
		goto out2;

	get_fs_root(current->fs, &root);
	old_mp = lock_mount(&old);
	error = PTR_ERR(old_mp);
	if (IS_ERR(old_mp))
		goto out3;

	error = -EINVAL;
	new_mnt = real_mount(new.mnt);
	root_mnt = real_mount(root.mnt);
	old_mnt = real_mount(old.mnt);
	if (IS_MNT_SHARED(old_mnt) ||
		IS_MNT_SHARED(new_mnt->mnt_parent) ||
		IS_MNT_SHARED(root_mnt->mnt_parent))
		goto out4;
	if (!check_mnt(root_mnt) || !check_mnt(new_mnt))
		goto out4;
	error = -ENOENT;
	if (d_unlinked(new.dentry))
		goto out4;
	error = -EBUSY;
	if (new_mnt == root_mnt || old_mnt == root_mnt)
		goto out4; /* loop, on the same file system  */
	error = -EINVAL;
	if (root.mnt->mnt_root != root.dentry)
		goto out4; /* not a mountpoint */
	if (!mnt_has_parent(root_mnt))
		goto out4; /* not attached */
	root_mp = root_mnt->mnt_mp;
	if (new.mnt->mnt_root != new.dentry)
		goto out4; /* not a mountpoint */
	if (!mnt_has_parent(new_mnt))
		goto out4; /* not attached */
	/* make sure we can reach put_old from new_root */
	if (!is_path_reachable(old_mnt, old.dentry, &new))
		goto out4;
	/* make certain new is below the root */
	if (!is_path_reachable(new_mnt, new.dentry, &root))
		goto out4;
	root_mp->m_count++; /* pin it so it won't go away */
	br_write_lock(&vfsmount_lock);
	detach_mnt(new_mnt, &parent_path);
	detach_mnt(root_mnt, &root_parent);
	/* mount old root on put_old */
	attach_mnt(root_mnt, old_mnt, old_mp);
	/* mount new_root on / */
	attach_mnt(new_mnt, real_mount(root_parent.mnt), root_mp);
	touch_mnt_namespace(current->nsproxy->mnt_ns);
	br_write_unlock(&vfsmount_lock);
	chroot_fs_refs(&root, &new);
	put_mountpoint(root_mp);
	error = 0;
out4:
	unlock_mount(old_mp);
	if (!error) {
		path_put(&root_parent);
		path_put(&parent_path);
	}
out3:
	path_put(&root);
out2:
	path_put(&old);
out1:
	path_put(&new);
out0:
	return error;
}

static void __init init_mount_tree(void)
{
	struct vfsmount *mnt;
	struct mnt_namespace *ns;
	struct path root;
	struct file_system_type *type;

	type = get_fs_type("rootfs");
	if (!type)
		panic("Can't find rootfs type");
	mnt = vfs_kern_mount(type, 0, "rootfs", NULL);
	put_filesystem(type);
	if (IS_ERR(mnt))
		panic("Can't create rootfs");

	ns = create_mnt_ns(mnt);
	if (IS_ERR(ns))
		panic("Can't allocate initial namespace");

	init_task.nsproxy->mnt_ns = ns;
	get_mnt_ns(ns);

	root.mnt = mnt;
	root.dentry = mnt->mnt_root;

	set_fs_pwd(current->fs, &root);
	set_fs_root(current->fs, &root);
}

void __init mnt_init(void)
{
	unsigned u;
	int err;

	init_rwsem(&namespace_sem);

	mnt_cache = kmem_cache_create("mnt_cache", sizeof(struct mount),
			0, SLAB_HWCACHE_ALIGN | SLAB_PANIC, NULL);

	mount_hashtable = (struct list_head *)__get_free_page(GFP_ATOMIC);
	mountpoint_hashtable = (struct list_head *)__get_free_page(GFP_ATOMIC);

	if (!mount_hashtable || !mountpoint_hashtable)
		panic("Failed to allocate mount hash table\n");

	printk(KERN_INFO "Mount-cache hash table entries: %lu\n", HASH_SIZE);

	for (u = 0; u < HASH_SIZE; u++)
		INIT_LIST_HEAD(&mount_hashtable[u]);
	for (u = 0; u < HASH_SIZE; u++)
		INIT_LIST_HEAD(&mountpoint_hashtable[u]);

	br_lock_init(&vfsmount_lock);

	err = sysfs_init();
	if (err)
		printk(KERN_WARNING "%s: sysfs_init error: %d\n",
			__func__, err);
	fs_kobj = kobject_create_and_add("fs", NULL);
	if (!fs_kobj)
		printk(KERN_WARNING "%s: kobj create error\n", __func__);
	init_rootfs();
	init_mount_tree();
}

void put_mnt_ns(struct mnt_namespace *ns)
{
	if (!atomic_dec_and_test(&ns->count))
		return;
	namespace_lock();
	br_write_lock(&vfsmount_lock);
	umount_tree(ns->root, 0);
	br_write_unlock(&vfsmount_lock);
	namespace_unlock();
	free_mnt_ns(ns);
}

struct vfsmount *kern_mount_data(struct file_system_type *type, void *data)
{
	struct vfsmount *mnt;
	mnt = vfs_kern_mount(type, MS_KERNMOUNT, type->name, data);
	if (!IS_ERR(mnt)) {
		/*
		 * it is a longterm mount, don't release mnt until
		 * we unmount before file sys is unregistered
		*/
		real_mount(mnt)->mnt_ns = MNT_NS_INTERNAL;
	}
	return mnt;
}
EXPORT_SYMBOL_GPL(kern_mount_data);

void kern_unmount(struct vfsmount *mnt)
{
	/* release long term mount so mount point can be released */
	if (!IS_ERR_OR_NULL(mnt)) {
		br_write_lock(&vfsmount_lock);
		real_mount(mnt)->mnt_ns = NULL;
		br_write_unlock(&vfsmount_lock);
		mntput(mnt);
	}
}
EXPORT_SYMBOL(kern_unmount);

bool our_mnt(struct vfsmount *mnt)
{
	return check_mnt(real_mount(mnt));
}

bool current_chrooted(void)
{
	/* Does the current process have a non-standard root */
	struct path ns_root;
	struct path fs_root;
	bool chrooted;

	/* Find the namespace root */
	ns_root.mnt = &current->nsproxy->mnt_ns->root->mnt;
	ns_root.dentry = ns_root.mnt->mnt_root;
	path_get(&ns_root);
	while (d_mountpoint(ns_root.dentry) && follow_down_one(&ns_root))
		;

	get_fs_root(current->fs, &fs_root);

	chrooted = !path_equal(&fs_root, &ns_root);

	path_put(&fs_root);
	path_put(&ns_root);

	return chrooted;
}

void update_mnt_policy(struct user_namespace *userns)
{
	struct mnt_namespace *ns = current->nsproxy->mnt_ns;
	struct mount *mnt;

	down_read(&namespace_sem);
	list_for_each_entry(mnt, &ns->list, mnt_list) {
		switch (mnt->mnt.mnt_sb->s_magic) {
		case SYSFS_MAGIC:
			userns->may_mount_sysfs = true;
			break;
		case PROC_SUPER_MAGIC:
			userns->may_mount_proc = true;
			break;
		}
		if (userns->may_mount_sysfs && userns->may_mount_proc)
			break;
	}
	up_read(&namespace_sem);
}

static void *mntns_get(struct task_struct *task)
{
	struct mnt_namespace *ns = NULL;
	struct nsproxy *nsproxy;

	rcu_read_lock();
	nsproxy = task_nsproxy(task);
	if (nsproxy) {
		ns = nsproxy->mnt_ns;
		get_mnt_ns(ns);
	}
	rcu_read_unlock();

	return ns;
}

static void mntns_put(void *ns)
{
	put_mnt_ns(ns);
}

static int mntns_install(struct nsproxy *nsproxy, void *ns)
{
	struct fs_struct *fs = current->fs;
	struct mnt_namespace *mnt_ns = ns;
	struct path root;

	if (!ns_capable(mnt_ns->user_ns, CAP_SYS_ADMIN) ||
	    !nsown_capable(CAP_SYS_CHROOT) ||
	    !nsown_capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (fs->users != 1)
		return -EINVAL;

	get_mnt_ns(mnt_ns);
	put_mnt_ns(nsproxy->mnt_ns);
	nsproxy->mnt_ns = mnt_ns;

	/* Find the root */
	root.mnt    = &mnt_ns->root->mnt;
	root.dentry = mnt_ns->root->mnt.mnt_root;
	path_get(&root);
	while(d_mountpoint(root.dentry) && follow_down_one(&root))
		;

	/* Update the pwd and root */
	set_fs_pwd(fs, &root);
	set_fs_root(fs, &root);

	path_put(&root);
	return 0;
}

static unsigned int mntns_inum(void *ns)
{
	struct mnt_namespace *mnt_ns = ns;
	return mnt_ns->proc_inum;
}

const struct proc_ns_operations mntns_operations = {
	.name		= "mnt",
	.type		= CLONE_NEWNS,
	.get		= mntns_get,
	.put		= mntns_put,
	.install	= mntns_install,
	.inum		= mntns_inum,
};
