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
#include <asm/arch.h>
#include <asm/io.h>
#include <minos/mmu.h>
#include <asm/bcm_irq.h>
#include <libfdt/libfdt.h>
#include <minos/of.h>
#include <asm/cpu.h>
#include <minos/platform.h>

#ifdef CONFIG_VIRT
#include <virt/virq.h>
#include <virt/vm.h>
#include <virt/vmm.h>

static int raspberry3_setup_hvm(struct vm *vm, void *dtb)
{
	int i, offset, node;
	char name[16];
	uint64_t addr;
	uint64_t dtb_addr = 0;
	uint32_t *tmp = (uint32_t *)&dtb_addr;

	offset = of_get_node_by_name(dtb, 0, "cpus");
	if (offset < 0) {
		pr_err("can not find vcpus node for hvm\n");
		return -ENOENT;
	}

	/*
	 * using spin table boot methold redirect the
	 * relase addr to the interrupt controller space
	 */
	for (i = 0; i < vm->vcpu_nr; i++) {
		memset(name, 0, 16);
		sprintf(name, "cpu@%d", i);
		node = fdt_subnode_offset(dtb, offset, name);
		if (node <= 0)
			continue;

		addr = BCM2836_RELEASE_ADDR + i * sizeof(uint64_t);
		pr_info("vcpu-%d release addr redirect to 0x%p\n", i, addr);
		tmp[0] = cpu_to_fdt32(addr >> 32);
		tmp[1] = cpu_to_fdt32(addr & 0xffffffff);

		fdt_setprop(dtb, node, "cpu-release-addr", (void *)tmp,
				2 * sizeof(uint32_t));
	}

	/*
	 * redirect the bcm2835 interrupt controller's iomem
	 * to 0x40000200
	 */
	node = fdt_path_offset(dtb, "/soc/interrupt-controller@7e00b200");
	if (node <= 0) {
		pr_warn("can not find interrupt-controller@7e00b200\n");
		return -ENOENT;
	}

	tmp[0] = cpu_to_fdt32(0x40000200);
	tmp[1] = cpu_to_fdt32(0x200);
	fdt_setprop(dtb, node, "reg", (void *)tmp, 2 * sizeof(uint32_t));
	fdt_set_name(dtb, node, "interrupt-controller@40000200");

	/* mask 40 - 52 virq for hvm which is internal use */
	for (i = 40; i <= 52; i++)
		request_virq(vm, i, 0);

	pr_info("raspberry3 setup vm done\n");

	return 0;
}
#endif

static void raspberry3_system_reboot(int mode, const char *cmd)
{

}

static void raspberry3_system_shutdown(void)
{

}

static void raspberry3_parse_mem_info(void)
{
	/* memory start at 0x3b400000 may has been used
	 * by other hardware, need to resve it ? */
	//split_memory_region(0x3b400000, 60 * 1024 * 1024, 0);
}

static struct platform platform_raspberry3 = {
	.name 		 = "raspberrypi,3-model-b-plus",
	.cpu_on		 = spin_table_cpu_on,
	.system_reboot	 = raspberry3_system_reboot,
	.system_shutdown = raspberry3_system_shutdown,
#ifdef CONFIG_VIRT
	.setup_hvm	 = raspberry3_setup_hvm,
#endif
	.parse_mem_info  = raspberry3_parse_mem_info,
};
DEFINE_PLATFORM(platform_raspberry3);
