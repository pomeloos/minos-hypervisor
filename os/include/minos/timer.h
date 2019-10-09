#ifndef _MINOS_TIMER_H_
#define _MINOS_TIMER_H_

/*
 * refer to linux kernel timer code
 */
#include <minos/time.h>

#define DEFAULT_TIMER_MARGIN	(10)

typedef void (*timer_func_t)(unsigned long);

struct timer_list {
	int cpu;
	atomic_t del_request;
	struct list_head entry;
	unsigned long expires;
	timer_func_t function;
	unsigned long data;
	struct timers *timers;
};

struct timers {
	struct list_head active;
	unsigned long running_expires;
	struct timer_list *running_timer;
	spinlock_t lock;
};

void init_timer(struct timer_list *timer);
void init_timer_on_cpu(struct timer_list *timer, int cpu);
void add_timer(struct timer_list *timer);
int del_timer(struct timer_list *timer);
int mod_timer(struct timer_list *timer, unsigned long expires);

#endif
