#ifndef __MINOS_TASK_H__
#define __MINOS_TASK_H__

#include <minos/minos.h>
#include <minos/flag.h>
#include <config/config.h>

/* the max realtime task will be 64 */
#define OS_NR_TASKS		512
#define OS_REALTIME_TASK	64

#define OS_LOWEST_PRIO		(OS_REALTIME_TASK - 1)
#define OS_PRIO_PCPU		(OS_LOWEST_PRIO + 1)
#define OS_PRIO_IDLE		(OS_LOWEST_PRIO + 2)

#define OS_RDY_TBL_SIZE		(OS_REALTIME_TASK / 8)

#define OS_TASK_RESERVED	((struct task *)1)

#define TASK_FLAGS_IDLE		0x0001
#define TASK_FLAGS_VCPU		0x0002
#define TASK_FLAGS_PERCPU	0x0004

#define PCPU_AFF_NONE		0xffff
#define PCPU_AFF_PERCPU		0xfffe

#define TASK_DEF_STACK_SIZE	(4096)

#define TASK_NAME_SIZE		(31)

#define TASK_STAT_RDY           0x00  /* Ready to run */
#define TASK_STAT_SEM           0x01  /* Pending on semaphore */
#define TASK_STAT_MBOX          0x02  /* Pending on mailbox */
#define TASK_STAT_Q             0x04  /* Pending on queue */
#define TASK_STAT_SUSPEND       0x08  /* Task is suspended */
#define TASK_STAT_MUTEX         0x10  /* Pending on mutual exclusion semaphore */
#define TASK_STAT_FLAG          0x20  /* Pending on event flag group */
#define TASK_STAT_MULTI         0x80  /* Pending on multiple events */
#define TASK_STAT_RUNNING	0x100 /* Task is running */

#define TASK_STAT_PEND_ANY      (OS_STAT_SEM | OS_STAT_MBOX | OS_STAT_Q | OS_STAT_MUTEX | OS_STAT_FLAG)

#define TASK_STAT_PEND_OK       0u  /* Pending status OK, not pending, or pending complete */
#define TASK_STAT_PEND_TO       1u  /* Pending timed out */
#define TASK_STAT_PEND_ABORT    2u  /* Pending aborted */

#define TASK_DEFAULT_STACK_SIZE CONFIG_TASK_DEFAULT_STACK_SIZE

extern struct task *os_task_table[OS_NR_TASKS];

typedef void (*task_func_t)(void *data);
struct event;

struct task {
	void *stack_base;
	void *stack_origin;
	uint32_t stack_size;
	void *udata;
	int pid;

	unsigned long flags;

	/*
	 * link to the global task list or the
	 * cpu task list, and stat list used for
	 * pcpu task to link to the state list.
	 */
	struct list_head list;
	struct list_head stat_list;
	struct list_head event_list;

	void *msg;
	flag_t flags_rdy;

	uint32_t delay;
	volatile uint16_t stat;
	volatile uint16_t pend_stat;
	uint8_t del_req;
	uint8_t prio;
	uint8_t bx;
	uint8_t by;
	prio_t bitx;
	prio_t bity;

	/* the event that this task hold currently */
	atomic_t lock_cpu;
	struct event *lock_event;
	struct event *wait_event;

	/* used to the flag type */
	int flag_rdy;
	struct flag_node *flag_node;

	uint16_t affinity;

	spinlock_t lock;


	/* stat information */
	unsigned long ctx_sw_cnt;
	unsigned long cycle_total;
	unsigned long cycle_start;
	void *stack_current;
	uint32_t stack_used;

	char name[TASK_NAME_SIZE + 1];

	void *pdata;		/* pointer to the vcpu data */
	void *arch_data;	/* arch data to this task */
	void **context;
} __align_cache_line;

struct task_desc {
	char name[TASK_NAME_SIZE + 1];
	task_func_t func;
	void *arg;
	prio_t prio;
	uint16_t aff;
	uint32_t stk_size;
	unsigned long flags;
};

#define DEFINE_TASK(tn, f, a, p, af, ss, fl) \
	static const struct task_desc __used \
	task_desc_##tn __section(.__task_desc) = { \
		.name = #tn,		\
		.func = f,		\
		.arg = a,		\
		.prio = p,		\
		.aff = af,		\
		.stk_size = ss,		\
		.flags = fl		\
	}

#define DEFINE_TASK_PERCPU(tn, f, a, ss, fl) \
	static const struct task_desc __used \
	task_desc_##tn __section(.__task_desc) = { \
		.name = #tn,		\
		.func = f,		\
		.arg = a,		\
		.prio = OS_PRIO_PCPU,	\
		.aff = PCPU_AFF_PERCPU,	\
		.stk_size = ss,		\
		.flags = fl		\
	}

#define DEFINE_REALTIME(tn, f, a, p, ss, fl) \
	static const struct task_desc __used \
	task_desc_##tn __section(.__task_desc) = { \
		.name = #tn,		\
		.func = f,		\
		.arg = a,		\
		.prio = p,		\
		.aff = 0,		\
		.stk_size = ss,		\
		.flags = fl		\
	}

static int inline is_idle_task(struct task *task)
{
	return !!(task->flags & TASK_FLAGS_IDLE);
}

static inline int get_task_pid(struct task *task)
{
	return task->pid;
}

static inline prio_t get_task_prio(struct task *task)
{
	return task->prio;
}

int alloc_pid(prio_t prio, int cpuid);
void release_pid(int pid);

int create_task(char *name, task_func_t func,
		void *arg, prio_t prio, uint16_t aff,
		uint32_t stk_size, unsigned long opt);

#endif
