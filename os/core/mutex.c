/*
 * Copyright (C) 2019 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <minos/event.h>
#include <minos/task.h>
#include <minos/mutex.h>
#include <minos/sched.h>

#define invalid_mutex(mutex)	\
	((mutex == NULL) && (mutex->type != OS_EVENT_TYPE_MUTEX))

mutex_t *mutex_create(char *name)
{
	mutex_t *mutex;

	mutex = (mutex_t *)create_event(OS_EVENT_TYPE_MUTEX, NULL, name);
	if (!mutex)
		return NULL;

	mutex->cnt = OS_MUTEX_AVAILABLE;

	return mutex;
}

int mutex_accept(mutex_t *mutex)
{
	int ret = -EBUSY;
	unsigned long flags;
	struct task *task = get_current_task();

	if (invalid_mutex(mutex))
		return -EPERM;

	/* if the mutex is avaliable now, lock it */
	spin_lock_irqsave(&mutex->lock, flags);
	if (mutex->cnt == OS_MUTEX_AVAILABLE) {
		mutex->owner = task->pid;
		mutex->data = task;
		mutex->cnt = task->prio;
		ret = 0;
	}
	spin_unlock_irqrestore(&mutex->lock, flags);

	return ret;
}

int mutex_del(mutex_t *mutex, int opt)
{
	unsigned long flags;
	int tasks_waiting;
	int ret;
	struct task *task;

	if (invalid_mutex(mutex))
		return -EINVAL;

	spin_lock_irqsave(&mutex->lock, flags);

	if (event_has_waiter(to_event(mutex)))
		tasks_waiting = 1;
	else
		tasks_waiting = 0;

	switch (opt) {
	case OS_DEL_NO_PEND:
		if (tasks_waiting) {
			pr_err("can not delete mutex task waitting for it\n");
			ret = -EPERM;
		} else {
			spin_unlock_irqrestore(&mutex->lock, flags);
			release_event(to_event(mutex));
			return 0;
		}
		break;

	case OS_DEL_ALWAYS:
		/* 
		 * need to unlock the cpu if the cpu is lock
		 * by this task, the mutex can not be accessed
		 * by the task after it has been locked.
		 */
		task = (struct task *)mutex->data;
		if (task != NULL)
			task->lock_event = NULL;

		event_del_always(to_event(mutex));
		spin_unlock_irqrestore(&mutex->lock, flags);
		release_event(to_event(mutex));

		if (tasks_waiting)
			sched();

		return 0;
	default:
		ret = -EINVAL;
		break;
	}

	spin_unlock_irqrestore(&mutex->lock, flags);
	return ret;
}

int mutex_pend(mutex_t *m, uint32_t timeout)
{
	int ret;
	unsigned long flags = 0;
	struct task *task = get_current_task();

	might_sleep();

	spin_lock(&m->lock);
	if (m->cnt == OS_MUTEX_AVAILABLE) {
		m->owner = task->pid;
		m->data = (void *)task;
		m->cnt = task->pid;

		wmb();

		/* to be done  need furture design */
		task->lock_event = to_event(m);
		spin_unlock(&m->lock);
		return 0;
	}


	/*
	 * priority inversion - only check the realtime task
	 *
	 * check the current task's prio to see whether the
	 * current task's prio is lower than mutex's owner, if
	 * yes, need to increase the owner's prio
	 *
	 * on smp it not easy to deal with the priority, then
	 * just lock the cpu until the task(own the mutex) has
	 * finish it work, but there is a big problem, if the
	 * task need to get two mutex, how to deal with this ?
	 */

	/* set the task's state and suspend the task */
	task_lock_irqsave(task, flags);
	task->stat |= TASK_STAT_MUTEX;
	task->pend_stat = TASK_STAT_PEND_OK;
	task->delay = timeout;
	task->wait_event = to_event(m);
	set_task_sleep(task);
	task_unlock_irqrestore(task, flags);

	event_task_wait(task, to_event(m));
	spin_unlock(&m->lock);
	
	sched();

	switch (task->pend_stat) {
	case TASK_STAT_PEND_OK:
		ret = 0;
		break;

	case TASK_STAT_PEND_ABORT:
		ret = -EABORT;
		break;
	
	case TASK_STAT_PEND_TO:
	default:
		ret = -ETIMEDOUT;
		spin_lock_irqsave(&m->lock, flags);
		event_task_remove(task, (struct event *)m);
		spin_unlock_irqrestore(&m->lock, flags);
		break;
	}

	task->pend_stat = TASK_STAT_PEND_OK;
	task->wait_event = 0;
	mb();
	
	return ret;
}

int mutex_post(mutex_t *m)
{
	struct task *task = get_current_task();

	if (invalid_mutex(m))
		return -EPERM;

	spin_lock(&m->lock);
	if (task != (struct task *)m->data) {
		pr_err("mutex-%s not belong to this task\n", m->name);
		spin_unlock(&m->lock);
		return -EINVAL;
	}

	task->lock_event = NULL;

	/* 
	 * find the highest prio task to run, if there is
	 * no task, then set the mutex is available else
	 * resched
	 */
	task = event_highest_task_ready((struct event *)m, NULL,
			TASK_STAT_MUTEX, TASK_STAT_PEND_OK);
	if (task) {
		m->cnt = task->pid;
		m->data = task;
		m->owner = task->pid;
		mb();

		spin_unlock(&m->lock);
		sched_task(task);

		return 0;
	}

	m->cnt = OS_MUTEX_AVAILABLE;
	m->data = NULL;
	m->owner = 0;
	mb();

	spin_unlock(&m->lock);

	return 0;
}
