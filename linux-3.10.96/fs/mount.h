#include <linux/mount.h>
#include <linux/seq_file.h>
#include <linux/poll.h>

struct mnt_namespace {
	atomic_t		count;
	unsigned int		proc_inum;
	struct mount *	root;
    //貌似属于一个命名空间的文件系统的struct mount结构体都链接在这个链表，不同命名空间彼此看不到对应命名空间的文件系统mount结构
	struct list_head	list;
	struct user_namespace	*user_ns;
	u64			seq;	/* Sequence number to prevent loops */
	wait_queue_head_t poll;
	int event;
};

struct mnt_pcp {
	int mnt_count;
	int mnt_writers;
};

//块设备的挂载点目录
struct mountpoint {
	struct list_head m_hash;
	struct dentry *m_dentry;//挂点点目录dentry
	int m_count;
};
/*见attach_mnt().关于父子mount的理解，每一次挂载，都会针对挂载源生成一个mount结构，即source mount，而针对挂载点目录所处文件系统的
dest mount，就是source mount的父mount。source mount是子mount,dest mount是父mount，source mnt->mnt_child链接到dest mount的parent->mnt_mounts。
举例，dest mount是sda3 挂在到根目录'/'生成的，然后sda5挂载到/home目录，这次生成的mount，即souce mount，与sda3的dest mount是父子关系。 */

//每一个挂载的块设备都要生成一个mount结构体，每一次挂载都会生成的一个mount结构
struct mount {
    //mount靠mnt_hash链入mount hash链表，__lookup_mnt()是从该mount hash链表搜索mount结构。commit_tree()和attach_mnt()中靠
    //mnt_hash把mount链入mount hash链表，并且链入hash表的键值是(父mount结构的vfsmount成员+该mount的挂载点dentry)
	struct list_head mnt_hash;
    //父mount,attach_recursive_mnt->mnt_set_mountpoint(),竟然设置为挂点目录所在文件系统的mount，
    //也说也是，挂载源的mount的父mount是挂载点目录所在的文件系统的mount结构
	struct mount *mnt_parent;
    //挂载点dentry，attach_recursive_mnt->mnt_set_mountpoint()设置为挂载点目录dentry
	struct dentry *mnt_mountpoint;
    //包含块设备的根目录dentry
	struct vfsmount mnt;
#ifdef CONFIG_SMP
	struct mnt_pcp __percpu *mnt_pcp;
#else
	int mnt_count;
	int mnt_writers;
#endif
    //commit_tree()靠mnt_child把mount结构添加到mount的parent mount的mnt_mounts链表，所以这个看着是mount的子mount结构保存的链表
	struct list_head mnt_mounts;	/* list of children, anchored here */
    //next_mnt()里根据mnt_child返回其mount结构，commit_tree()和attach_mnt()靠mnt_child把mount结构添加到mount的mnt_parent的mnt_mounts链表
	struct list_head mnt_child;	/* and going through their mnt_child */
	struct list_head mnt_instance;	/* mount instance on sb->s_mounts */
   
	const char *mnt_devname;	/* Name of device e.g. /dev/dsk/hda1 */
    //copy_tree()创建的新mount并靠mnt_list添加到该链表，搞不懂有什么用?
	struct list_head mnt_list;
	struct list_head mnt_expire;	/* link in fs-specific expiry list */
    //clone_mnt()把本次挂载的source mount通过其mnt_share链接到克隆母体的mnt_share链表
	struct list_head mnt_share;	/* circular list of shared mounts */
    //clone_mnt()中，把本次挂载slave属性的source mount结构链接到克隆母体mount的mnt_slave_list链表。mount结构的mnt_slave_list链表
    //是保存子slave mount的，凡是照着一个mount结构克隆生成的mount，都添加到克隆母体的mnt_slave_list链表，克隆的mount是母体的子slave mount
	struct list_head mnt_slave_list;/* list of slave mounts */
    // 1 clone_mnt()中，把本次挂载source slave属性的mount结构链接到克隆母体mount的mnt_slave_list链表
    /* 2 clone_mnt()中，克隆母体是slave属性而本次source mount没有指定属性，则source mount被添加到与克隆母体同一个mount salve组链表
       具体添加形式是，source mount结构靠其mnt_slave添加到克隆母体的mnt_slave链表。source mount和克隆母体靠各自的mnt_slave构成链表,
       二者是同一个mount slave组成员。如果source mount靠其mnt_slave添加到克隆母体的mnt_slave_list链表，则二者是父子关系，不是同组关系。
       */
	struct list_head mnt_slave;	/* slave list entry */
    /* 1 clone_mnt()中，本次挂载是slave属性，克隆生成的source mount^，即mnt，其mnt_master指向克隆母体的mount结构
    // 2 clone_mnt()中，本次挂载没有指定mount属性，而克隆母体又是slave属性，则souece mount的mnt_master就是克隆母体的mount->mnt_master，
    //二者属于同一个mount slave组
       3 正常mount /dev/sda3 /home这样生成的mount其mnt_master是NULL，mount bind的share属性的mount其mnt_master是NULL
     */
	struct mount *mnt_master;	/* slave is on master->mnt_slave_list */
    //mount所属命名空间，commit_tree()中把mount结构添加到父mount的mnt_ns的list链表
	struct mnt_namespace *mnt_ns;	/* containing namespace */
    //挂载点结构，包含挂载点dentry，attach_recursive_mnt->mnt_set_mountpoint()中设置
	struct mountpoint *mnt_mp;	/* where is it mounted */
#ifdef CONFIG_FSNOTIFY
	struct hlist_head mnt_fsnotify_marks;
	__u32 mnt_fsnotify_mask;
#endif
    //mount id, alloc_vfsmnt， mnt_alloc_id()中分配
	int mnt_id;			/* mount identifier */
    //mount group id，一个mount组里，所有的mount结构的mnt_group_id一样.就是靠这个判断两个mount是否属于同一个peer group
    //do_loopback()->clone_mnt() 中赋值
	int mnt_group_id;		/* peer group identifier */
	int mnt_expiry_mark;		/* true if marked for expiry */
	int mnt_pinned;
	int mnt_ghosts;
};

#define MNT_NS_INTERNAL ERR_PTR(-EINVAL) /* distinct from any mnt_namespace */

static inline struct mount *real_mount(struct vfsmount *mnt)
{
	return container_of(mnt, struct mount, mnt);
}

static inline int mnt_has_parent(struct mount *mnt)
{
	return mnt != mnt->mnt_parent;
}

static inline int is_mounted(struct vfsmount *mnt)
{
	/* neither detached nor internal? */
	return !IS_ERR_OR_NULL(real_mount(mnt)->mnt_ns);
}

extern struct mount *__lookup_mnt(struct vfsmount *, struct dentry *, int);

static inline void get_mnt_ns(struct mnt_namespace *ns)
{
	atomic_inc(&ns->count);
}
//大部分成员在mounts_open_common()或者show_mountinfo()中赋值
struct proc_mounts {
	struct seq_file m;
	struct mnt_namespace *ns;//命名空间，来自当前进程task的struct nsproxy的struct mnt_namespace成员
	struct path root;	//指向当前进程所属的根文件系统
	int (*show)(struct seq_file *, struct vfsmount *);//mounts_open_common赋值为show_vfsmnt
};

#define proc_mounts(p) (container_of((p), struct proc_mounts, m))

extern const struct seq_operations mounts_op;
