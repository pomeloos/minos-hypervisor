#ifndef __MINOS_MAILBOX_H__
#define __MINOS_MAILBOX_H__

#define MAILBOX_MAX_EVENT	4

#define MAILBOX_EVENT_RING_ID	0

struct mailbox_vm_entry {
	uint32_t connect_virq;
	uint32_t disconnect_virq;
	uint32_t ring_event_virq;
	uint32_t event[MAILBOX_MAX_EVENT];
};

/*
 * owner : owner[0] is the server vm and the 
 *         owner[1] is the client vm
 *
 */
struct mailbox {
	char name[32];
	uint32_t cookie;
	int vm_status[2];
	struct vm *owner[2];
	spinlock_t lock;
	void *shared_memory;
	struct mailbox_vm_entry vm_entry[2];
};

#endif
