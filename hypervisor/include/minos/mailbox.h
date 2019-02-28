#ifndef __MINOS_MAILBOX_H__
#define __MINOS_MAILBOX_H__

#define MAILBOX_MAX_EVENT	4

#define MAILBOX_EVENT_RING_ID	0

struct mailbox_vm_entry {
	uint32_t connect_virq;
	uint32_t disconnect_virq;
	uint32_t event[MAILBOX_MAX_EVENT];
};

/*
 * owner : owner[0] is the server vm and the 
 *         owner[1] is the client vm
 *
 */
struct mailbox {
	uint32_t owner[2];
	int vm_status[2];
	void *shared_memory;
	struct mailbox_vm_entry vm_entry[2];
};

#endif
