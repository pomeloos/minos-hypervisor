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

#define MAX_MAILBOX_NR	20

static int mailbox_index = 0;
static struct mailbox *mailboxs[MAX_MAILBOX_NR];

static uint32_t inline generate_mailbox_cookie(int o1, int o2, int index)
{
	return (0xab << 24) | (o1 << 16) | (o2 << 8) | index;
}

struct mailbox *create_mailbox(char *name, int o1, o2, size_t size)
{
	struct vm *vm1, vm2;
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

	mailbox->cookie = generate_mailbox_cookie(o1, o2, mailbox_index);
	mailbox->owner[0] = vm1;
	mailbox->owner[1] = vm2;
	mailbox->vm_status[0] = MAILBOX_VM_DISCONNECT;
	mailbox->vm_status[1] = MAILBOX_VM_DISCONNECT;
	spin_lock_init(&mailbox->lock);
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
	mailbox->shared_memory = get_io_pages(PAGE_NR(size));
	if (!mailbox->shared_memory)
		panic("no more memory for %s\n", name);

out:
	return mailbox;
}
