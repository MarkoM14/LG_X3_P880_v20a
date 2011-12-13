#ifndef IOCONTEXT_H
#define IOCONTEXT_H

#include <linux/radix-tree.h>
#include <linux/rcupdate.h>
#include <linux/workqueue.h>

struct cfq_queue;
struct cfq_ttime {
	unsigned long last_end_request;

	unsigned long ttime_total;
	unsigned long ttime_samples;
	unsigned long ttime_mean;
};

enum {
	CIC_IOPRIO_CHANGED,
	CIC_CGROUP_CHANGED,
};

struct cfq_io_context {
	void *key;
	struct request_queue *q;

	struct cfq_queue *cfqq[2];

	struct io_context *ioc;

	struct cfq_ttime ttime;

	struct list_head queue_list;
	struct hlist_node cic_list;

	unsigned long changed;

	void (*exit)(struct cfq_io_context *);
	void (*release)(struct cfq_io_context *);

	struct rcu_head rcu_head;
};

/*
 * I/O subsystem state of the associated processes.  It is refcounted
 * and kmalloc'ed. These could be shared between processes.
 */
struct io_context {
	atomic_long_t refcount;
	atomic_t nr_tasks;

	/* all the fields below are protected by this lock */
	spinlock_t lock;

	unsigned short ioprio;

	/*
	 * For request batching
	 */
	int nr_batch_requests;     /* Number of requests left in the batch */
	unsigned long last_waited; /* Time last woken after wait for request */

	struct radix_tree_root radix_root;
	struct hlist_head cic_list;
	void __rcu *ioc_data;

	struct work_struct release_work;
};

static inline struct io_context *ioc_task_link(struct io_context *ioc)
{
	/*
	 * if ref count is zero, don't allow sharing (ioc is going away, it's
	 * a race).
	 */
	if (ioc && atomic_long_inc_not_zero(&ioc->refcount)) {
		atomic_inc(&ioc->nr_tasks);
		return ioc;
	}

	return NULL;
}

struct task_struct;
#ifdef CONFIG_BLOCK
void put_io_context(struct io_context *ioc, struct request_queue *locked_q);
void exit_io_context(struct task_struct *task);
struct io_context *get_task_io_context(struct task_struct *task,
				       gfp_t gfp_flags, int node);
void ioc_ioprio_changed(struct io_context *ioc, int ioprio);
void ioc_cgroup_changed(struct io_context *ioc);
#else
struct io_context;
static inline void put_io_context(struct io_context *ioc,
				  struct request_queue *locked_q) { }
static inline void exit_io_context(struct task_struct *task) { }
#endif

#endif
