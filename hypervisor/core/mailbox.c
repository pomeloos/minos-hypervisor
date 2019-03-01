/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
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
#include <minos/mailbox.h>
#include <asm/svccc.h>
#include <minos/hypercall.h>

#define MAX_MAILBOX_NR	20
#define MAILBOX_MAGIC	0xabcdefeeUL

typedef unsigned long (*mailbox_hvc_handler)(uint64_t arg0, uint64_t arg1);

static int mailbox_index = 0;
static struct mailbox *mailboxs[MAX_MAILBOX_NR];

static uint32_t inline generate_mailbox_cookie(int o1, int o2, int index)
{
	return (MAILBOX_MAGIC << 32) | (o1 << 16) | (o2 << 8) | index;
}

static void inline exract_mailbox_cookie(uint64_t cookie,
		int *o1, int *o2, int *index, uint32_t *magic)
{
	*o1 = (cookie & 0x0000000000ff0000) >> 16;
	*o1 = (cookie & 0x000000000000ff00) >> 8;
	*index = cookie & 0xff;
	*magic = cookie >> 32;
}

static void mailbox_vm_init(struct mailbox *mb, int event)
{
	int i, j;
	struct mailbox_vm_entry *entry;
	struct vm *vm;

	for (i = 0; i < 2; i++) {
		/*
		 * map the shared memory for vm and allocate
		 * the virq for the mailbox
		 */
		entry = &mb->vm_entry[i];
		vm = mb->owner[i];
		entry->iomem = vm_map_shmem(vm,
				mb->shmem, mb->shmem_size, VM_IO);
		if (!entry->iomem)
			panic("no enough shared memory vm-%d\n", vm->vmid);

		entry->connect_virq = alloc_vm_virq(vm);
		entry->disconnect_virq = alloc_vm_virq(vm);
		if (!entry->connect_virq || !entry->disconnect_virq)
			panic("no enough virq for vm-%d\n", vm->vmid);

		for (j = 0; j < event; j++) {
			entry->evnet[j] = alloc_vm_virq(vm);
			if (!entry->evnet[j])
				panic("no enough virq for vm-%d\n", vm->vmid);
		}
	}
}

struct mailbox *create_mailbox(char *name,
		int o1, int o2, size_t size, int event)
{
	struct vm *vm1, *vm2;
	struct mailbox *mailbox;

	if (mailbox_index >= MAX_MAILBOX_NR) {
		pr_error("mailbox count beyond the max size\n");
		return NULL;
	}

	vm1 = get_vm_by_id(o1);
	vm2 = get_vm_by_id(o2);
	if (!vm1 || !vm2)
		return NULL;

	mailbox = zalloc(sizeof(struct mailbox));
	if (!mailbox)
		return NULL;

	mailbox->owner[0] = vm1;
	mailbox->owner[1] = vm2;
	mailbox->vm_status[0] = MAILBOX_VM_DISCONNECT;
	mailbox->vm_status[1] = MAILBOX_VM_DISCONNECT;
	spin_lock_init(&mailbox->lock);
	mailbox->cookie = generate_mailbox_cookie(o1, o2, mailbox_index);
	mailboxs[mailbox_index] = mailbox;
	mailbox_index++;

	if (unlikely(!size))
		goto out;

	/*
	 * the current memory allocation system has a
	 * limitation that get_io_pages can not get
	 * memory which bigger than 2M.
	 */
	size = PAGE_BALIGN(size);
	mailbox->shmem = get_io_pages(PAGE_NR(size));
	if (!mailbox->shared_memory)
		panic("no more memory for %s\n", name);

	mailbox_vm_init(mailbox);
out:
	return mailbox;
}

static mailbox_hvc_handlers[] = {
	mailbox_query_instance,
	mailbox_get_info,
	NULL
};

static int mailbox_hvc_handler(gp_regs *c, uint32_t id, uint64_t *args)
{
	struct vm *vm = get_current_vm();
	uint64_t cookie = args[0];
	int o1, o2, index;
	uint32_t magic;
	struct mailbox *mailbox;
	unsigned long ret;

	/*
	 * args[0] is the cookie and other is the par
	 * args[1] is the parment0
	 * args[2] is the parment2
	 * */
	exract_mailbox_cookie(cookie, &o1, &o2, &index, &magic);
	if (unlikely((vm->vmid != o1) && (vm->vmid != o2)))
		panic("mailbox is not belong to vm-%d\n", vm->vmid);

	if (unlikely(magic != MAILBOX_MAGIC))
		panic("invalid mailbox\n");

	if (unlikey(index >= MAX_MAILBOX_NR))
		panic("invalid mailbox index\n");

	mailbox = mailboxs[index];
	if (unlikey(!mailbox)) {
		pr_error("mailbox-%d is not created\n", index);
		HVC_RET1(c, -EINVAL);
	}

	index = id - HVC_MAILBOX_FN(0);
	if (index >= sizeof(mailbox_hvc_handlers)) {
		pr_error("unsupport mailbox hypercall %d\n", index);
		HVC_RET1(c, -EINVAL)
	}

	ret = mailbox_hvc_handlers[index](args[1], args[2]);
	HVC_RET1(c, ret);
}

DEFINE_HVC_HANDLER("vm_mailbox_handler", HVC_TYPE_HVC_MAILBOX,
		HVC_TYPE_HVC_MAILBOX, mailbox_hvc_handler);
