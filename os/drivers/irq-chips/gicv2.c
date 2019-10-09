/*
 * xen/arch/arm/gic-v2.c
 *
 * Tim Deegan <tim@xen.org>
 * Copyright (c) 2011 Citrix Systems.
 *
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <minos/minos.h>
#include <asm/io.h>
#include <minos/percpu.h>
#include <minos/spinlock.h>
#include <minos/print.h>
#include <asm/gicv2.h>
#include <minos/errno.h>
#include <minos/vmodule.h>
#include <asm/arch.h>
#include <minos/cpumask.h>
#include <minos/irq.h>
#include <minos/of.h>
#include <minos/mmu.h>

/*
 * LR register definitions are GIC v2 specific.
 * Moved these definitions from header file to here
 */
#define GICH_V2_LR_VIRTUAL_MASK    0x3ff
#define GICH_V2_LR_VIRTUAL_SHIFT   0
#define GICH_V2_LR_PHYSICAL_MASK   0x3ff
#define GICH_V2_LR_PHYSICAL_SHIFT  10
#define GICH_V2_LR_STATE_MASK      0x3
#define GICH_V2_LR_STATE_SHIFT     28
#define GICH_V2_LR_PENDING         (1U << 28)
#define GICH_V2_LR_ACTIVE          (1U << 29)
#define GICH_V2_LR_PRIORITY_SHIFT  23
#define GICH_V2_LR_PRIORITY_MASK   0x1f
#define GICH_V2_LR_HW_SHIFT        31
#define GICH_V2_LR_HW_MASK         0x1
#define GICH_V2_LR_GRP_SHIFT       30
#define GICH_V2_LR_GRP_MASK        0x1
#define GICH_V2_LR_MAINTENANCE_IRQ (1U << 19)
#define GICH_V2_LR_GRP1            (1U << 30)
#define GICH_V2_LR_HW              (1U << GICH_V2_LR_HW_SHIFT)
#define GICH_V2_LR_CPUID_SHIFT     10
#define GICH_V2_LR_CPUID_MASK      0x7
#define GICH_V2_VTR_NRLRGS         0x3f

#define GICH_V2_VMCR_PRIORITY_MASK   0x1f
#define GICH_V2_VMCR_PRIORITY_SHIFT  27

#define GIC_PRI_LOWEST     0xf0
#define GIC_PRI_IRQ        0xa0
#define GIC_PRI_IPI        0x90 /* IPIs must preempt normal interrupts */
#define GIC_PRI_HIGHEST    0x80 /* Higher priorities belong to Secure-World */

static DEFINE_SPIN_LOCK(gicv2_lock);
static void *gicv2_dbase;
static void *gicv2_cbase;
static void *gicv2_hbase;
static int gicv2_nr_lines;

static DEFINE_PER_CPU(uint8_t, gic_cpu_id);

extern int vgicv2_init(uint64_t *data, int len);
extern int gic_xlate_irq(struct device_node *node,
		uint32_t *intspec, unsigned int intsize,
		uint32_t *hwirq, unsigned long *type);

/* Maximum cpu interface per GIC */
#define NR_GIC_CPU_IF 8

static inline void writeb_gicd(uint8_t val, unsigned int offset)
{
	writeb_relaxed(val, gicv2_dbase + offset);
}

static inline void writel_gicd(uint32_t val, unsigned int offset)
{
	writel_relaxed(val, gicv2_dbase + offset);
}

static inline uint32_t readl_gicd(unsigned int offset)
{
	return readl_relaxed(gicv2_dbase + offset);
}

static inline void writel_gicc(uint32_t val, unsigned int offset)
{
	writel_relaxed(val, gicv2_cbase + offset);
}

static inline uint32_t readl_gicc(unsigned int offset)
{
	return readl_relaxed(gicv2_cbase + offset);
}

static inline void writel_gich(uint32_t val, unsigned int offset)
{
	writel_relaxed(val, gicv2_hbase + offset);
}

static inline uint32_t readl_gich(int unsigned offset)
{
	return readl_relaxed(gicv2_hbase + offset);
}

static void gicv2_eoi_irq(uint32_t irq)
{
	writel_gicc(irq, GICC_EOIR);
	dsb();
}

static void gicv2_dir_irq(uint32_t irq)
{
	writel_gicc(irq, GICC_DIR);
	dsb();
}

static uint32_t gicv2_read_irq(void)
{
	uint32_t irq;

	irq = readl_gicc(GICC_IAR);
	isb();
	irq = irq & GICC_IA_IRQ;

	return irq;
}

static int gicv2_set_irq_type(uint32_t irq, uint32_t type)
{
	uint32_t cfg, edgebit;

	if (irq < 16)
		return 0;

	spin_lock(&gicv2_lock);

	/* Set edge / level */
	cfg = readl_gicd(GICD_ICFGR + (irq / 16) * 4);
	edgebit = 2u << (2 * (irq % 16));
	if ( type & IRQ_FLAGS_LEVEL_BOTH)
		cfg &= ~edgebit;
	else if (type & IRQ_FLAGS_EDGE_BOTH)
		cfg |= edgebit;

	writel_gicd(cfg, GICD_ICFGR + (irq / 16) * 4);
	spin_unlock(&gicv2_lock);

	return 0;
}

static void __used gicv2_clear_pending(uint32_t irq)
{
	writel_gicd(1UL << (irq % 32), GICD_ICPENDR + (irq / 32) * 4);
	dsb();
}

static int gicv2_set_irq_priority(uint32_t irq, uint32_t pr)
{
	spin_lock(&gicv2_lock);

	/* Set priority */
	writeb_gicd(pr, GICD_IPRIORITYR + irq);

	spin_unlock(&gicv2_lock);
	return 0;
}

static int gicv2_set_irq_affinity(uint32_t irq, uint32_t pcpu)
{
	if (pcpu > NR_GIC_CPU_IF || irq < 32)
		return -EINVAL;

	spin_lock(&gicv2_lock);
	/* Set target CPU mask (RAZ/WI on uniprocessor) */
	writeb_gicd(1 << pcpu, GICD_ITARGETSR + irq);
	spin_unlock(&gicv2_lock);
	return 0;
}

static void gicv2_send_sgi(uint32_t sgi, enum sgi_mode mode, cpumask_t *mask)
{
	unsigned int cpu;
	unsigned int value = 0;

	switch (mode) {
	case SGI_TO_OTHERS:
		writel_gicd(GICD_SGI_TARGET_OTHERS | sgi, GICD_SGIR);
		break;
	case SGI_TO_SELF:
		writel_gicd(GICD_SGI_TARGET_SELF | sgi, GICD_SGIR);
		break;
	case SGI_TO_LIST:
		for_each_cpu(cpu, mask)
			value |= (1 << cpu);

		writel_gicd(GICD_SGI_TARGET_LIST |
			(value << GICD_SGI_TARGET_SHIFT) | sgi,
			GICD_SGIR);
		break;
	default:
		break;;
    }
}

static void gicv2_mask_irq(uint32_t irq)
{
	unsigned long flags;

	spin_lock_irqsave(&gicv2_lock, flags);
	writel_gicd(1UL << (irq % 32), GICD_ICENABLER + (irq / 32) * 4);
	dsb();
	spin_unlock_irqrestore(&gicv2_lock, flags);
}

static void gicv2_unmask_irq(uint32_t irq)
{
	unsigned long flags;

	spin_lock_irqsave(&gicv2_lock, flags);
	writel_gicd(1UL << (irq % 32), GICD_ISENABLER + (irq / 32) * 4);
	dsb();
	spin_unlock_irqrestore(&gicv2_lock, flags);
}

static void gicv2_mask_irq_cpu(uint32_t irq, int cpu)
{
	pr_warn("not support mask irq_percpu\n");
}

static void gicv2_unmask_irq_cpu(uint32_t irq, int cpu)
{
	pr_warn("not support unmask irq_percpu\n");
}

static int gicv2_is_aliased(unsigned long base, unsigned long size)
{
	uint32_t val_low, val_high;

	if (size != SIZE_1K * 128)
		return 0;

	val_low = readl_gicc(GICC_IIDR);
	val_high = readl_gicc(GICC_IIDR + 0xf000);

	return ((val_low & 0xfff0fff) == 0x0202043B &&
			val_low == val_high);
}

static void gicv2_cpu_init(void)
{
	int i;

	get_cpu_var(gic_cpu_id) = readl_gicd(GICD_ITARGETSR) & 0xff;

	/* The first 32 interrupts (PPI and SGI) are banked per-cpu, so
	 * even though they are controlled with GICD registers, they must
	 * be set up here with the other per-cpu state. */
	writel_gicd(0xffff0000, GICD_ICENABLER); /* Disable all PPI */
	writel_gicd(0x0000ffff, GICD_ISENABLER); /* Enable all SGI */

	/* Set SGI priorities */
	for ( i = 0; i < 16; i += 4 )
		writel_gicd(GIC_PRI_IPI << 24 | GIC_PRI_IPI << 16 |
			GIC_PRI_IPI << 8 | GIC_PRI_IPI,
			GICD_IPRIORITYR + (i / 4) * 4);

	/* Set PPI priorities */
	for ( i = 16; i < 32; i += 4 )
		writel_gicd(GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 |
			GIC_PRI_IRQ << 8 | GIC_PRI_IRQ,
			GICD_IPRIORITYR + (i / 4) * 4);

	/* Local settings: interface controller */
	/* Don't mask by priority */
	writel_gicc(0xff, GICC_PMR);
	/* Finest granularity of priority */
	writel_gicc(0x0, GICC_BPR);
	/* Turn on delivery */
	writel_gicc(GICC_CTL_ENABLE|GICC_CTL_EOI, GICC_CTLR);
	dsb();
}

static void gicv2_hyp_init(void)
{

}

static void gicv2_dist_init(void)
{
	uint32_t type;
	uint32_t cpumask;
	uint32_t gic_cpus;
	unsigned int nr_lines;
	int i;

	cpumask = readl_gicd(GICD_ITARGETSR) & 0xff;
	cpumask |= cpumask << 8;
	cpumask |= cpumask << 16;

	/* Disable the distributor */
	writel_gicd(0, GICD_CTLR);

	type = readl_gicd(GICD_TYPER);
	nr_lines = 32 * ((type & GICD_TYPE_LINES) + 1);
	gic_cpus = 1 + ((type & GICD_TYPE_CPUS) >> 5);
	pr_info("GICv2: %d lines, %d cpu%s%s (IID %x).\n",
		nr_lines, gic_cpus, (gic_cpus == 1) ? "" : "s",
		(type & GICD_TYPE_SEC) ? ", secure" : "",
		readl_gicd(GICD_IIDR));


	irq_alloc_spi(32, nr_lines);

	/* Default all global IRQs to level, active low */
	for ( i = 32; i < nr_lines; i += 16 )
		writel_gicd(0x0, GICD_ICFGR + (i / 16) * 4);

	/* Route all global IRQs to this CPU */
	for ( i = 32; i < nr_lines; i += 4 )
		writel_gicd(cpumask, GICD_ITARGETSR + (i / 4) * 4);

	/* Default priority for global interrupts */
	for ( i = 32; i < nr_lines; i += 4 )
		writel_gicd(GIC_PRI_IRQ << 24 | GIC_PRI_IRQ << 16 |
			GIC_PRI_IRQ << 8 | GIC_PRI_IRQ,
			GICD_IPRIORITYR + (i / 4) * 4);

	/* Disable all global interrupts */
	for ( i = 32; i < nr_lines; i += 32 )
		writel_gicd(~0x0, GICD_ICENABLER + (i / 32) * 4);

	/* Only 1020 interrupts are supported */
	gicv2_nr_lines = min(1020U, nr_lines);

	/* Turn on the distributor */
	writel_gicd(GICD_CTL_ENABLE, GICD_CTLR);
	dsb();
}

static int gicv2_init(struct device_node *node)
{
	uint64_t array[10];

	pr_info("*** gicv2 init ***\n");
	memset(array, 0, sizeof(array));

	translate_device_address_index(node, &array[0], &array[1], 0);
	translate_device_address_index(node, &array[2], &array[3], 1);
	translate_device_address_index(node, &array[4], &array[5], 2);
	translate_device_address_index(node, &array[6], &array[7], 3);

	pr_info("gicv2 information: gic_dist_addr=%p size=0x%x "
		"gic_cpu_addr=%p size=0x%x gic_hyp_addr=%p size=0x%x "
		"gic_vcpu_addr=%p size=0x%x\n",
		array[0], array[1], array[2], array[3],
		array[4], array[5], array[6], array[7]);

	gicv2_dbase = (void *)array[0];
	gicv2_cbase = (void *)array[2];
	gicv2_hbase = (void *)array[4];

	io_remap((vir_addr_t)array[0], (phy_addr_t)array[0],
			(size_t)array[1]);
	io_remap((vir_addr_t)array[2], (phy_addr_t)array[2],
			(size_t)array[3]);
	io_remap((vir_addr_t)array[4], (phy_addr_t)array[4],
			(size_t)array[5]);

	if (gicv2_is_aliased((unsigned long)array[2],
				(unsigned long)array[3])) {
		gicv2_cbase += 0xf000;
		pr_info("gicv2 : adjust cpu interface base to 0x%x\n",
				(unsigned long)gicv2_cbase);
	}

	spin_lock(&gicv2_lock);

	irq_alloc_sgi(0, 16);
	irq_alloc_ppi(16, 16);

	gicv2_dist_init();
	gicv2_cpu_init();
	gicv2_hyp_init();

	spin_unlock(&gicv2_lock);

#if defined CONFIG_VIRQCHIP_VGICV2 && defined CONFIG_VIRT
	vgicv2_init(array, 8);
#endif

	return 0;
}

static int gicv2_secondary_init(void)
{
	spin_lock(&gicv2_lock);

	gicv2_cpu_init();
	gicv2_hyp_init();

	spin_unlock(&gicv2_lock);

	return 0;
}

static struct irq_chip gicv2_chip = {
	.irq_mask 		= gicv2_mask_irq,
	.irq_mask_cpu		= gicv2_mask_irq_cpu,
	.irq_unmask 		= gicv2_unmask_irq,
	.irq_unmask_cpu 	= gicv2_unmask_irq_cpu,
	.irq_eoi 		= gicv2_eoi_irq,
	.irq_dir		= gicv2_dir_irq,
	.irq_set_type 		= gicv2_set_irq_type,
	.irq_set_affinity 	= gicv2_set_irq_affinity,
	.send_sgi		= gicv2_send_sgi,
	.get_pending_irq	= gicv2_read_irq,
	.irq_set_priority	= gicv2_set_irq_priority,
	.irq_xlate		= gic_xlate_irq,
	.init			= gicv2_init,
	.secondary_init		= gicv2_secondary_init,
};
IRQCHIP_DECLARE(gicv2_chip, gicv2_match_table, (void *)&gicv2_chip);
