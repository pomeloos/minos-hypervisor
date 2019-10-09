/*
 * BSD 3-Clause License
 *
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <inttypes.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netinet/ether.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <linux/netlink.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <time.h>

#include <mvm.h>
#include <vdev.h>
#include <mevent.h>
#include <barrier.h>
#include <list.h>

struct vm *mvm_vm = NULL;
static struct vm_config *global_config = NULL;

int verbose;

static void free_vm_config(struct vm_config *config);
int vm_shutdown(struct vm *vm);

extern int virtio_mmio_init(struct vm *vm, int nr_devs);
extern int virtio_mmio_deinit(struct vm *vm);

void *map_vm_memory(struct vm *vm)
{
	uint64_t args[2];
	void *addr = NULL;

	args[0] = 0;
	args[1] = vm->mem_size;

	addr = mmap(NULL, args[1], PROT_READ | PROT_WRITE,
			MAP_SHARED, vm->vm_fd, args[0]);
	if (addr == (void *)-1)
		return NULL;

	if (ioctl(vm->vm_fd, IOCTL_VM_MMAP, args)) {
		pr_err("mmap memory failed 0x%lx 0x%lx\n", args[0],
				vm->mem_size);
		munmap(addr, vm->mem_size);
		addr = 0;
	}

	return addr;
}

static int create_new_vm(struct vm *vm)
{
	int fd, vmid = -1;
	struct vmtag info;

	strcpy(info.name, vm->name);
	strcpy(info.os_type, vm->os_type);
	info.nr_vcpu = vm->nr_vcpus;
	info.mem_size = vm->mem_size;
	info.mem_base = vm->mem_start;
	info.entry = (void *)vm->entry;
	info.setup_data = (void *)vm->setup_data;
	info.mmap_base = 0;
	info.flags = vm->flags;
	info.vmid = vm->vmid;

	fd = open("/dev/mvm/mvm0", O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		perror("/dev/mvm/mvm0");
		return -EIO;
	}

	pr_info("create new vm *\n");
	pr_info("        -name       : %s\n", info.name);
	pr_info("        -os_type    : %s\n", info.os_type);
	pr_info("        -nr_vcpu    : %d\n", info.nr_vcpu);
	pr_info("        -bit64      : %d\n",
			!!vm->flags & VM_FLAGS_64BIT);
	pr_info("        -mem_size   : 0x%lx\n", info.mem_size);
	pr_info("        -mem_base  : 0x%lx\n", info.mem_base);
	pr_info("        -entry      : 0x%p\n", info.entry);
	pr_info("        -setup_data : 0x%p\n", info.setup_data);

	vmid = ioctl(fd, IOCTL_CREATE_VM, &info);
	if (vmid <= 0) {
		perror("vmid");
		return vmid;
	}

	vm->hvm_paddr = info.mmap_base;
	close(fd);

	return vmid;
}

static int release_vm(int vmid)
{
	int fd, ret;

	fd = open("/dev/mvm/mvm0", O_RDWR);
	if (fd < 0)
		return -ENODEV;

	ret = ioctl(fd, IOCTL_DESTROY_VM, vmid);
	close(fd);

	return ret;
}

int destroy_vm(struct vm *vm)
{
	int i;
	struct vdev *vdev;
	struct epoll_event ee;

	if (!vm)
		return -EINVAL;

	memset(&ee, 0, sizeof(struct epoll_event));

	if (vm->epfds && vm->eventfds) {
		for (i = 0; i < vm->nr_vcpus; i++) {
			if ((vm->epfds[i] > 0) && (vm->eventfds[i] > 0)) {
				ee.events = EPOLLIN;
				epoll_ctl(vm->epfds[i], EPOLL_CTL_DEL,
						vm->eventfds[i], &ee);
			}

			if (vm->eventfds[i] > 0) {
				close(vm->eventfds[i]);
				vm->eventfds[i] = -1;
			}

			if (vm->epfds[i] > 0) {
				close(vm->epfds[i]);
				vm->epfds[i] = -1;
			}
		}
	}

	if (vm->irqs) {
		for (i = 0; i < vm->nr_vcpus; i++) {
			if (vm->irqs[i] <= 0)
				continue;

			ioctl(vm->vm_fd, IOCTL_UNREGISTER_VCPU,
					(unsigned long)vm->irqs[i]);
		}
	}

	list_for_each_entry(vdev, &vm->vdev_list, list)
		release_vdev(vdev);

	virtio_mmio_deinit(vm);
	mevent_deinit();

	if (vm->mmap) {
		if (vm->vm_fd)
			ioctl(vm->vm_fd, IOCTL_VM_UNMAP, 0);
		munmap(vm->mmap, vm->mem_size);
	}

	if (vm->vm_fd > 0)
		close(vm->vm_fd);

	if (vm->vmid > 0)
		release_vm(vm->vmid);

	if (vm->image_fd > 0)
		close(vm->image_fd);

	if (vm->kfd > 0)
		close(vm->kfd);

	if (vm->rfd > 0)
		close(vm->rfd);

	if (vm->dfd > 0)
		close(vm->rfd);

	if (vm->os_data > 0)
		free(vm->os_data);

	free(vm);
	mvm_vm = NULL;

	free_vm_config(global_config);
	global_config = NULL;

	return 0;
}

static void signal_handler(int signum)
{
	int vmid = 0xfff;

	pr_info("recevied signal %i\n", signum);

	switch (signum) {
	case SIGTERM:
	case SIGBUS:
	case SIGKILL:
	case SIGSEGV:
	case SIGSTOP:
	case SIGTSTP:
		if (mvm_vm)
			vmid = mvm_vm->vmid;

		pr_info("shutdown vm-%d\n", vmid);
		vm_shutdown(mvm_vm);
		break;
	default:
		break;
	}

	exit(0);
}

void print_usage(void)
{
	fprintf(stderr, "\nUsage: mvm [options] \n\n");
	fprintf(stderr, "    -c <vcpu_count>            (set the vcpu numbers of the vm)\n");
	fprintf(stderr, "    -m <mem_size_in_MB>        (set the memsize of the vm - 2M align)\n");
	fprintf(stderr, "    -i <boot or kernel image>  (the kernel or bootimage to use)\n");
	fprintf(stderr, "    -s <mem_base>             (set the membase of the vm if not a boot.img)\n");
	fprintf(stderr, "    -n <vm name>               (the name of the vm)\n");
	fprintf(stderr, "    -t <vm type>               (the os type of the vm )\n");
	fprintf(stderr, "    -b <32 or 64>              (32bit or 64 bit )\n");
	fprintf(stderr, "    -r                         (do not load ramdisk image)\n");
	fprintf(stderr, "    -v                         (verbose print debug information)\n");
	fprintf(stderr, "    -d                         (run as a daemon process)\n");
	fprintf(stderr, "    -D                         (create a platform bus device)\n");
	fprintf(stderr, "    -V                         (create a virtio device)\n");
	fprintf(stderr, "    -K                         (kernel image path)\n");
	fprintf(stderr, "    -S                         (second image path - like dtb image)\n");
	fprintf(stderr, "    -R                         (Ramdisk image path)\n");
	fprintf(stderr, "    --gicv2                    (using the gicv2 interrupt controller)\n");
	fprintf(stderr, "    --gicv3                    (using the gicv3 interrupt controller)\n");
	fprintf(stderr, "    --gicv4                    (using the gicv4 interrupt controller)\n");
	fprintf(stderr, "    --earlyprintk              (enable the earlyprintk based on virtio-console)\n");
	fprintf(stderr, "\n");
	exit(EXIT_FAILURE);
}

void *hvm_map_iomem(void *base, size_t size)
{
	void *iomem;
	int fd = open("/dev/mvm/mvm0", O_RDWR);

	if (fd < 0) {
		pr_err("open /dev/mvm/mvm0 failed\n");
		return (void *)-1;
	}

	iomem = mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, (unsigned long)base);
	close(fd);

	return iomem;
}

static int vm_create_vmcs(struct vm *vm)
{
	int ret;
	void *vmcs;

	ret = ioctl(vm->vm_fd, IOCTL_CREATE_VMCS, &vmcs);
	if (ret)
		return ret;

	if (!vmcs)
		return -ENOMEM;

	vm->vmcs = hvm_map_iomem(vmcs, VMCS_SIZE(vm->nr_vcpus));
	if (vm->vmcs == (void *)-1)
		return -ENOMEM;

	return 0;
}

static int create_and_init_vm(struct vm *vm)
{
	int ret = 0;
	char path[32];

	vm->vmid = create_new_vm(vm);
	if (vm->vmid <= 0)
		return (vm->vmid);

	memset(path, 0, 32);
	sprintf(path, "/dev/mvm/mvm%d", vm->vmid);
	vm->vm_fd = open(path, O_RDWR | O_NONBLOCK);
	if (vm->vm_fd < 0) {
		perror(path);
		return -EIO;
	}

	ret = vm_create_vmcs(vm);
	if (ret)
		return -ENOMEM;

	/*
	 * map a fix region for this vm, need to call ioctl
	 * to informe hypervisor to map the really physical
	 * memory
	 */
	vm->mmap = map_vm_memory(vm);
	if (!vm->mmap)
		return -EAGAIN;

	/* load the image into the vm memory */
	ret = vm->os->load_image(vm);
	if (ret)
		return ret;

	return 0;
}

static struct vm_os *get_vm_os(char *os_type)
{
	struct vm_os **os_start = (struct vm_os **)&__start_mvm_os;
	struct vm_os **os_end = (struct vm_os **)&__stop_mvm_os;
	struct vm_os *default_os = NULL;
	struct vm_os *os;

	for (; os_start < os_end; os_start++) {
		os = *os_start;
		if (strcmp(os_type, os->name) == 0)
			return os;

		if (strcmp("default", os->name) == 0)
			default_os = os;
	}

	return default_os;
}

static int vm_create_host_vdev(struct vm *vm)
{
	return ioctl(vm->vm_fd, IOCTL_CREATE_HOST_VDEV, NULL);
}

static int vm_vdev_init(struct vm *vm, struct vm_config *config)
{
	int i, ret;
	char *tmp, *pos;
	char *type = NULL, *arg = NULL;
	struct device_info *info = &config->device_info;

	vdev_subsystem_init();

	/* init the virtio framework */
	ret = virtio_mmio_init(vm, info->nr_virtio_dev);
	if (ret) {
		pr_err("unable to init the virtio framework\n");
		return ret;
	}

	for (i = 0; i < info->nr_device; i++) {
		tmp = info->device_args[i];
		if (!tmp || (strlen(tmp) == 0)) {
			pr_warn("invaild device argument index%d\n", i);
			continue;
		}

		pos = strchr(tmp, ',');
		if (pos == NULL) {
			pr_err("invaild device argumet %s", tmp);
			continue;
		}

		type = tmp;
		arg = pos + 1;
		*pos = '\0';

		if (create_vdev(vm, type, arg))
			pr_err("create %s-%s failed\n", type, arg);
	}

	return 0;
}

static void vmcs_ack(struct vmcs *vmcs)
{
	if (vmcs->guest_index == vmcs->host_index)
		return;

	vmcs->guest_index++;
	wmb();
}

static int vcpu_handle_mmio(struct vm *vm, int trap_reason,
		unsigned long trap_data, unsigned long *trap_result)
{
	int ret = -EIO;
	struct vdev *vdev;
	unsigned long base, size;

	list_for_each_entry(vdev, &vm->vdev_list, list) {
		base = (unsigned long)vdev->guest_iomem;
		size = vdev->iomem_size;
		if ((trap_data >= base) && (trap_data < base + size)) {
			pthread_mutex_lock(&vdev->lock);
			ret = vdev->ops->event(vdev, trap_reason,
					trap_data, trap_result);
			pthread_mutex_unlock(&vdev->lock);

			return ret;
		}
	}

	return -ENODEV;
}

static int vcpu_handle_common_trap(struct vm *vm, int trap_reason,
		unsigned long trap_data, unsigned long *trap_result)
{
	switch (trap_reason) {
	case VMTRAP_REASON_REBOOT:
	case VMTRAP_REASON_SHUTDOWN:
	case VMTRAP_REASON_WDT_TIMEOUT:
		mvm_queue_push(&vm->queue, trap_reason, NULL, 0);
		break;
	case VMTRAP_REASON_VM_SUSPEND:
		vm->state = VM_STAT_SUSPEND;
		pr_info("vm-%d is suspend\n", vm->vmid);
		break;
	case VMTRAP_REASON_VM_RESUMED:
		vm->state = VM_STAT_RUNNING;
		pr_info("vm-%d is resumed\n", vm->vmid);
		break;
	case VMTRAP_REASON_GET_TIME:
		*trap_result = (unsigned long)time(NULL);
		break;
	default:
		break;
	}

	return 0;
}

static void handle_vcpu_event(struct vmcs *vmcs)
{
	int ret;
	uint32_t trap_type = vmcs->trap_type;
	uint32_t trap_reason = vmcs->trap_reason;
	unsigned long trap_data = vmcs->trap_data;
	unsigned long trap_result = vmcs->trap_result;

	switch (trap_type) {
	case VMTRAP_TYPE_COMMON:
		ret = vcpu_handle_common_trap(mvm_vm, trap_reason,
				trap_data, &trap_result);
		break;

	case VMTRAP_TYPE_MMIO:
		ret = vcpu_handle_mmio(mvm_vm, trap_reason,
				trap_data, &trap_result);
		break;

	default:
		break;
	}

	vmcs->trap_ret = ret;
	vmcs->trap_result = trap_result;

	vmcs_ack(vmcs);
}

void *vm_vcpu_thread(void *data)
{
	int ret;
	int eventfd;
	int epfd;
	struct vm *vm = mvm_vm;
	struct epoll_event event;
	struct epoll_event ep_events;
	struct vmcs *vmcs;
	unsigned long i = (unsigned long)data;
	eventfd_t value;
	char buf[32];

	memset(buf, 0, 32);
	sprintf(buf, "vm%d-vcpu-event", vm->vmid);
	prctl(PR_SET_NAME, buf);

	if (i >= vm->nr_vcpus)
		return NULL;

	eventfd = vm->eventfds[i];
	epfd = vm->epfds[i];
	vmcs = (struct vmcs *)(vm->vmcs + i * sizeof(struct vmcs));

	if (eventfd <= 0 || epfd <= 0)
		return NULL;

	memset(&event, 0, sizeof(struct epoll_event));
	event.events = (unsigned long)EPOLLIN;
	event.data.fd = eventfd;
	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, eventfd, &event);
	if (ret)
		return NULL;

	while (1) {
		ret = epoll_wait(epfd, &ep_events, 1, -1);
		if (ret <= 0) {
			pr_err("epoll failed for vcpu\n");
			break;
		}

		eventfd_read(eventfd, &value);
		if (value > 1)
			pr_err("invaild vmcs state\n");

		handle_vcpu_event(vmcs);
	}

	return NULL;
}

int __vm_shutdown(struct vm *vm)
{
	pr_info("***************************\n");
	pr_info("vm-%d shutdown exit mvm\n", vm->vmid);
	pr_info("***************************\n");

	destroy_vm(vm);
	exit(0);
}

int vm_shutdown(struct vm *vm)
{
	int ret;

	ret = ioctl(vm->vm_fd, IOCTL_POWER_DOWN_VM, 0);
	if (ret) {
		pr_err("can not power-off vm-%d now, try again\n",
				vm->vmid);
		return -EAGAIN;
	}

	return __vm_shutdown(vm);
}

int __vm_reboot(struct vm *vm)
{
	int ret;
	struct vdev *vdev;

	/* hypervisor has been disable the vcpu */
	pr_info("***************************\n");
	pr_info("reboot the vm-%d\n", vm->vmid);
	pr_info("***************************\n");

	list_for_each_entry(vdev, &vm->vdev_list, list) {
		if (vdev->ops->reset)
			vdev->ops->reset(vdev);
	}

	/* load the image into the vm memory */
	ret = vm->os->load_image(vm);
	if (ret)
		return ret;

	ret = mvm_vm->os->setup_vm_env(vm, global_config->cmdline);
	if (ret)
		return ret;

	if (ioctl(vm->vm_fd, IOCTL_POWER_UP_VM, 0)) {
		pr_err("power up vm-%d failed\n", vm->vmid);
		return -EAGAIN;
	}

	return 0;
}

int vm_reboot(struct vm *vm)
{
	int ret;

	ret = ioctl(vm->vm_fd, IOCTL_RESTART_VM, 0);
	if (ret) {
		pr_err("can not reboot vm-%d now, try again\n",
				vm->vmid);
		return -EAGAIN;
	}

	return __vm_reboot(vm);
}

static void handle_vm_event(struct vm *vm, struct mvm_node *node)
{
	pr_info("handle vm event %d\n", node->type);

	switch (node->type) {
	case VMTRAP_REASON_WDT_TIMEOUT:
		pr_err("vm-%d watchdog timeout reboot vm\n", vm->vmid);
		vm_reboot(vm);
		break;
	case VMTRAP_REASON_REBOOT:
		__vm_reboot(vm);
		break;
	case VMTRAP_REASON_SHUTDOWN:
		__vm_shutdown(vm);
		break;
	default:
		pr_err("unsupport vm event %d\n", node->type);
		break;
	}

	mvm_queue_free(node);
}

static int mvm_main_loop(void)
{
	int ret, i, irq;
	struct vm *vm = mvm_vm;
	pthread_t vcpu_thread;
	int *base;
	uint64_t arg;
	struct mvm_node *node;

	/*
	 * create the eventfd and the epoll_fds for
	 * this vm
	 */
	base = malloc(sizeof(*base) * vm->nr_vcpus * 3);
	if (!base)
		return -ENOMEM;

	memset(base, -1, sizeof(int) * vm->nr_vcpus * 3);
	vm->eventfds = base;
	vm->epfds = base + vm->nr_vcpus;
	vm->irqs = base + vm->nr_vcpus * 2;

	for (i = 0; i < vm->nr_vcpus; i++) {
		irq = ioctl(vm->vm_fd, IOCTL_CREATE_VMCS_IRQ, (unsigned long)i);
		if (irq < 0)
			return -ENOENT;

		vm->eventfds[i] = eventfd(0, 0);
		if (vm->eventfds[i] < 0)
			return -ENOENT;

		vm->epfds[i] = epoll_create(1);
		if (vm->epfds[i] < 0)
			return -ENOENT;

		/*
		 * register the irq and eventfd to kernel
		 */
		vm->irqs[i] = irq;
		arg = ((unsigned long)vm->eventfds[i] << 32) | irq;
		ret = ioctl(vm->vm_fd, IOCTL_REGISTER_VCPU, &arg);
		if (ret)
			return ret;

		ret = pthread_create(&vcpu_thread, NULL,
				vm_vcpu_thread, (void *)(unsigned long)i);
		if (ret) {
			pr_err("create vcpu thread failed\n");
			return ret;
		}
	}

	ret = pthread_create(&vcpu_thread, NULL,
			mevent_dispatch, (void *)vm);
	if (ret) {
		pr_err("create mevent thread failed\n");
		return ret;
	}

	/* now start the vm */
	ret = ioctl(vm->vm_fd, IOCTL_POWER_UP_VM, NULL);
	if (ret)
		return ret;

	for (;;) {
		/* here wait for the trap type for VM */
		node = mvm_queue_pop(&vm->queue);
		if (node == NULL) {
			pr_err("mvm queue is abnormal shutdown vm\n");
			vm_shutdown(vm);
			break;
		} else
			handle_vm_event(vm, node);
	}

	return -EAGAIN;
}

static int mvm_open_images(struct vm *vm, struct vm_config *config)
{
	if (vm->flags & VM_FLAGS_NO_BOOTIMAGE) {
		vm->kfd = open(config->kernel_image, O_RDONLY | O_NONBLOCK);
		if (vm->kfd < 0) {
			pr_err("can not open the kernel image file %s\n",
					config->bootimage_path);
			return -ENOENT;
		}

		vm->dfd = open(config->dtb_image, O_RDONLY | O_NONBLOCK);
		if (vm->dfd < 0) {
			pr_err("can not open the dtb image file %s\n",
					config->dtb_image);
			return -ENOENT;
		}

		if (!(vm->flags & VM_FLAGS_NO_RAMDISK)) {
			vm->rfd = open(config->ramdisk_image,
					O_RDONLY | O_NONBLOCK);
			if (vm->rfd < 0)
				vm->flags |= VM_FLAGS_NO_RAMDISK;
		}
	} else {
		/* read the image to get the entry and other args */
		vm->image_fd = open(config->bootimage_path,
					O_RDONLY | O_NONBLOCK);
		if (vm->image_fd < 0) {
			pr_err("can not open the bootimage %s\n",
					config->bootimage_path);
			return -ENOENT;
		}
	}

	return 0;
}

static int mvm_main(struct vm_config *config)
{
	int ret;
	struct vm *vm;
	struct vm_os *os;
	struct vmtag *vmtag = &config->vmtag;

	signal(SIGTERM, signal_handler);
	signal(SIGBUS, signal_handler);
	signal(SIGKILL, signal_handler);
	signal(SIGSEGV, signal_handler);
	signal(SIGSTOP, signal_handler);
	signal(SIGTSTP, signal_handler);

	mvm_vm = vm = (struct vm *)calloc(1, sizeof(struct vm));
	if (!vm)
		return -ENOMEM;

	os = get_vm_os(vmtag->os_type);
	if (!os)
		return -EINVAL;

	/* udpate the vm from vmtag */
	vm->os = os;
	vm->flags = vmtag->flags;
	vm->vmid = -1;
	vm->vm_fd = -1;
	vm->entry = (uint64_t)vmtag->entry;
	vm->mem_start = vmtag->mem_base;
	vm->mem_size = vmtag->mem_size;
	vm->nr_vcpus = vmtag->nr_vcpu;
	strcpy(vm->name, vmtag->name);
	strcpy(vm->os_type, vmtag->os_type);
	init_list(&vm->vdev_list);
	vm->vm_config = config;

	ret = mvm_open_images(vm, config);
	if (ret) {
		free(vm);
		return ret;
	}

	ret = os->early_init(vm);
	if (ret) {
		pr_err("os early init faild %d\n", ret);
		goto release_vm;
	}

	if (vm->entry == 0)
		vm->entry = VM_MEM_START;
	if (vm->mem_start == 0)
		vm->mem_start = VM_MEM_START;
	if (vm->mem_size == 0)
		vm->mem_size = VM_MIN_MEM_SIZE;

	ret = create_and_init_vm(vm);
	if (ret)
		goto release_vm;

	mvm_queue_init(&vm->queue);

	/* io events init before vdev init */
	ret = mevent_init();
	if (ret)
		goto release_vm;

	ret = vm_vdev_init(vm, config);
	if (ret)
		goto release_vm;

	ret = mvm_vm->os->setup_vm_env(vm, config->cmdline);
	if (ret)
		return ret;

	ret = vm_create_host_vdev(vm);
	if (ret)
		pr_warn("failed to create some host virtual devices\n");

	/* free the global config */
	//free_vm_config(global_config);
	//global_config = NULL;

	mvm_main_loop();

release_vm:
	destroy_vm(mvm_vm);
	return ret;
}

static struct option options[] = {
	{"vcpu_number", required_argument, NULL, 'c'},
	{"mem_size",	required_argument, NULL, 'm'},
	{"image",	required_argument, NULL, 'i'},
	{"mem_base",	required_argument, NULL, 's'},
	{"name",	required_argument, NULL, 'n'},
	{"os_type",	required_argument, NULL, 't'},
	{"bit",		required_argument, NULL, 'b'},
	{"no_ramdisk",	no_argument,	   NULL, 'r'},
	{"gicv3",	no_argument,	   NULL, '0'},
	{"gicv2",	no_argument,	   NULL, '1'},
	{"gicv4",	no_argument,	   NULL, '2'},
	{"earlyprintk",	no_argument,	   NULL, '3'},
	{"help",	no_argument,	   NULL, 'h'},
	{NULL,		0,		   NULL,  0}
};

static int parse_vm_memsize(char *buf, uint64_t *size)
{
	int len = 0;

	if (!buf)
		return -EINVAL;

	len = strlen(buf) - 1;

	if ((buf[len] != 'm') && (buf[len] != 'M'))
		return -EINVAL;

	buf[len] = '\0';
	*size = atol(buf) * 1024 * 1024;

	return 0;
}

static int parse_vm_membase(char *buf, unsigned long *value)
{
	if (strlen(buf) < 3)
		return -EINVAL;

	if ((buf[0] == '0') && (buf[1] == 'x')) {
		sscanf(buf, "0x%lx", value);
		return 0;
	}

	return -EINVAL;
}

static int add_device_info(struct device_info *dinfo, char *name, int type)
{
	char *arg = NULL;

	if (dinfo->nr_device == VM_MAX_DEVICES) {
		pr_err("support max %d vdev\n", VM_MAX_DEVICES);
		return -EINVAL;
	}

	arg = calloc(1, strlen(name) + 1);
	if (!arg)
		return -ENOMEM;

	strcpy(arg, name);
	dinfo->device_args[dinfo->nr_device] = arg;
	dinfo->nr_device++;
	if (type == VDEV_TYPE_VIRTIO)
		dinfo->nr_virtio_dev++;

	return 0;
}

static int check_vm_config(struct vm_config *config)
{
	/* default will use bootimage as the vm image */
	if (config->bootimage_path[0] == 0) {
		config->vmtag.flags |= VM_FLAGS_NO_BOOTIMAGE;
		if ((config->kernel_image[0] == 0) ||
				config->dtb_image[0] == 0) {
			pr_err("no bootimage and kernel image\n");
			return -EINVAL;
		}

		if (config->ramdisk_image[0] == 0)
			config->vmtag.flags |= VM_FLAGS_NO_RAMDISK;
	}

	if (config->vmtag.nr_vcpu > VM_MAX_VCPUS) {
		pr_warn("support max %d vcpus\n", VM_MAX_VCPUS);
		config->vmtag.nr_vcpu = VM_MAX_VCPUS;
	}

	if (config->vmtag.name[0] == 0)
		strcpy(config->vmtag.name, "unknown");
	if (config->vmtag.os_type[0] == 0)
		strcpy(config->vmtag.os_type, "unknown");

	return 0;
}

static void free_vm_config(struct vm_config *config)
{
	int i;

	if (!config)
		return;

	for (i = 0; i < config->device_info.nr_device; i++) {
		if (!config->device_info.device_args[i])
			continue;
		free(config->device_info.device_args[i]);
	}

	free(config);
}

int main(int argc, char **argv)
{
	int ret, opt, idx;
	int run_as_daemon = 0;
	struct vmtag *vmtag;
	struct device_info *device_info;
	static char *optstr = "K:R:S:c:C:m:i:s:n:D:V:t:b:rv?hd0123";

	global_config = calloc(1, sizeof(struct vm_config));
	if (!global_config)
		return -ENOMEM;

	vmtag = &global_config->vmtag;
	device_info = &global_config->device_info;
	vmtag->flags = VM_FLAGS_64BIT | VM_FLAGS_DYNAMIC_AFF;

	while ((opt = getopt_long(argc, argv, optstr, options, &idx)) != -1) {
		switch(opt) {
		case 'c':
			vmtag->nr_vcpu = atoi(optarg);
			break;
		case 'm':
			ret = parse_vm_memsize(optarg, &vmtag->mem_size);
			if (ret)
				print_usage();
			break;
		case 'i':
			if (strlen(optarg) > 255) {
				pr_err("invaild boot_image path %s\n", optarg);
				ret = -EINVAL;
				goto exit;
			}

			strcpy(global_config->bootimage_path, optarg);
			break;
		case 's':
			ret = parse_vm_membase(optarg, &vmtag->mem_base);
			if (ret) {
				print_usage();
				ret = -EINVAL;
				goto exit;
			}
			break;
		case 'n':
			strncpy(vmtag->name, optarg, VM_NAME_SIZE - 1);
			break;
		case 't':
			strncpy(vmtag->os_type, optarg, VM_TYPE_SIZE - 1);
			break;
		case 'b':
			ret = atoi(optarg);
			if ((ret != 32) && (ret != 64)) {
				free(global_config);
				print_usage();
			}
			if (ret == 32)
				vmtag->flags &= ~VM_FLAGS_64BIT;
			break;
		case 'r':
			vmtag->flags |= VM_FLAGS_NO_RAMDISK;
			break;
		case 'v':
			verbose = 1;
			break;
		case 'd':
			run_as_daemon = 1;
			break;
		case 'D':
			add_device_info(device_info,
					optarg, VDEV_TYPE_PLATFORM);
			break;
		case 'V':
			add_device_info(device_info,
					optarg, VDEV_TYPE_VIRTIO);
			break;
		case 'C':
			if (strlen(optarg) > 255) {
				pr_err("cmdline is too long\n");
				ret = -EINVAL;
				goto exit;
			}
			strcpy(global_config->cmdline, optarg);
			break;
		case 'h':
			free(global_config);
			print_usage();
			return 0;
		case '3':
			vmtag->flags |= VM_FLAGS_HAS_EARLYPRINTK;
			break;
		case '2':
			global_config->gic_type = 2;
			break;
		case '0':
			global_config->gic_type = 0;
			break;
		case '1':
			global_config->gic_type = 1;
			break;
		/* the below argument is deicated for linux vm
		 * and will use the fixed loading address which
		 * kernel will loaded at 0x80080000 and dtb will
		 * loaded at (endadress - 2M) */
		case 'K':
			if (strlen(optarg) > 255) {
				pr_err("kernel image is too long\n");
				ret = -EINVAL;
				goto exit;
			}
			strcpy(global_config->kernel_image, optarg);
			break;
		case 'S':
			if (strlen(optarg) > 255) {
				pr_err("kernel image is too long\n");
				ret = -EINVAL;
				goto exit;
			}
			strcpy(global_config->dtb_image, optarg);
			break;
		case 'R':
			if (strlen(optarg) > 255) {
				pr_err("kernel image is too long\n");
				ret = -EINVAL;
				goto exit;
			}
			strcpy(global_config->ramdisk_image, optarg);
			break;
		default:
			break;
		}
	}

	ret = check_vm_config(global_config);
	if (ret)
		goto exit;

	if (run_as_daemon) {
		if (daemon(1, 1)) {
			pr_err("failed to run as daemon\n");
			ret = -EFAULT;
			goto exit;
		}
	}

	ret = mvm_main(global_config);

exit:
	free_vm_config(global_config);
	global_config = NULL;
	exit(ret);
}
