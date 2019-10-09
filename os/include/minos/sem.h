#ifndef __MINOS_SEM_H__
#define __MINOS_SEM_H__

#include <minos/event.h>

typedef struct event sem_t;

#define DEFINE_SEMAPHORE(name) \
	sem_t name = { \
		.type = 0xff, \
	}

sem_t *sem_create(uint32_t cnt, char *name);
uint32_t sem_accept(sem_t *sem);
int sem_del(sem_t *sem, int opt);
int sem_pend(sem_t *sem, uint32_t timeout);
int sem_pend_abort(sem_t *sem, int opt);
int sem_post(sem_t *sem);

static void inline sem_init(sem_t *sem, uint32_t cnt, char *name)
{
	event_init(to_event(sem), OS_EVENT_TYPE_SEM, NULL, name);
	sem->cnt = cnt;
}

#endif
