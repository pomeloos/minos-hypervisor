#ifndef _MINOS_IRQ_H_
#define _MINOS_IRQ_H_

#include <minos/types.h>
#include <minos/arch.h>
#include <minos/device_id.h>
#include <minos/init.h>
#include <config/config.h>
#include <minos/smp.h>
#include <minos/spinlock.h>
#include <minos/cpumask.h>

#define BAD_IRQ			(1023)

#define IRQ_FLAGS_NONE           		(0x00000000)
#define IRQ_FLAGS_EDGE_RISING    		(0x00000001)
#define IRQ_FLAGS_EDGE_FALLING  		(0x00000002)
#define IRQ_FLAGS_LEVEL_HIGH     		(0x00000004)
#define IRQ_FLAGS_LEVEL_LOW      		(0x00000008)
#define IRQ_FLAGS_SENSE_MASK     		(0x0000000f)
#define IRQ_FLAGS_INVALID        		(0x00000010)
#define IRQ_FLAGS_EDGE_BOTH \
    (IRQ_FLAGS_EDGE_FALLING | IRQ_FLAGS_EDGE_RISING)
#define IRQ_FLAGS_LEVEL_BOTH \
    (IRQ_FLAGS_LEVEL_LOW | IRQ_FLAGS_LEVEL_HIGH)
#define IRQ_FLAGS_TYPE_MASK			(0x000000ff)

#define IRQ_FLAGS_MASKED_BIT			(8)
#define IRQ_FLAGS_MASKED			(BIT(IRQ_FLAGS_MASKED_BIT))
#define IRQ_FLAGS_VCPU_BIT			(9)
#define IRQ_FLAGS_VCPU				(BIT(IRQ_FLAGS_VCPU_BIT))

#define RESCHED_IRQ				(7)
#define SMP_FUNCTION_CALL_IRQ			(6)

typedef enum sgi_mode {
	SGI_TO_LIST = 0,
	SGI_TO_OTHERS,
	SGI_TO_SELF,
} sgi_mode_t;

enum irq_type {
	IRQ_TYPE_SGI = 0,
	IRQ_TYPE_PPI,
	IRQ_TYPE_SPI,
	IRQ_TYPE_LPI,
	IRQ_TYPE_SPECIAL,
	IRQ_TYPE_MAX,
};

enum irq_domain_type {
	IRQ_DOMAIN_SGI = 0,
	IRQ_DOMAIN_PPI,
	IRQ_DOMAIN_SPI,
	IRQ_DOMAIN_LPI,
	IRQ_DOMAIN_SPECIAL,
	IRQ_DOMAIN_MAX,
};

struct irq_desc;
struct virq_desc;

typedef int (*irq_handle_t)(uint32_t irq, void *data);

struct irq_chip {
	uint32_t (*get_pending_irq)(void);
	void (*irq_mask)(uint32_t irq);
	void (*irq_mask_cpu)(uint32_t irq, int cpu);
	void (*irq_unmask_cpu)(uint32_t irq, int cpu);
	void (*irq_unmask)(uint32_t irq);
	void (*irq_eoi)(uint32_t irq);
	void (*irq_dir)(uint32_t irq);
	int (*irq_set_affinity)(uint32_t irq, uint32_t pcpu);
	int (*irq_set_type)(uint32_t irq, unsigned int flow_type);
	int (*irq_set_priority)(uint32_t irq, uint32_t pr);
	void (*irq_clear_pending)(uint32_t irq);
	int (*irq_xlate)(struct device_node *node, uint32_t *intspec,
			unsigned int intsize, uint32_t *hwirq, unsigned long *f);
	void (*send_sgi)(uint32_t irq, enum sgi_mode mode, cpumask_t *mask);
	int (*init)(struct device_node *node);
	int (*secondary_init)(void);
};

/*
 * if a irq is handled by minos, then need to register
 * the irq handler otherwise it will return the vnum
 * to the handler and pass the virq to the vm
 */
struct irq_desc {
	uint16_t hno;
	uint16_t affinity;
	unsigned long flags;
	spinlock_t lock;
	unsigned long irq_count;
	irq_handle_t handler;
	void *pdata;
	char *name;
};

struct irq_domain;
struct irq_domain_ops {
	struct irq_desc **(*alloc_irqs)(uint32_t s, uint32_t c, int type);
	struct irq_desc *(*get_irq_desc)(struct irq_domain *d, uint32_t irq);
	int (*irq_handler)(struct irq_domain *d, struct irq_desc *irq);
};

struct irq_domain {
	uint32_t start;
	uint32_t count;
	int type;
	struct irq_desc **irqs;
	struct irq_domain_ops *ops;
};

#define local_irq_enable() arch_enable_local_irq()
#define local_irq_disable() arch_disable_local_irq()
#define irq_disabled()	arch_irq_disabled()

int irq_init(void);
int irq_secondary_init(void);
void setup_irqs(void);
int do_irq_handler(void);

int request_irq(uint32_t irq, irq_handle_t handler,
		unsigned long flags, char *name, void *data);
int request_irq_percpu(uint32_t irq, irq_handle_t handler,
		unsigned long flags, char *name, void *data);

int irq_alloc_spi(uint32_t start, uint32_t cnt);
int irq_alloc_sgi(uint32_t start, uint32_t cnt);
int irq_alloc_ppi(uint32_t start, uint32_t cnt);
int irq_alloc_lpi(uint32_t start, uint32_t cnt);
int irq_alloc_special(uint32_t start, uint32_t cnt);

void __irq_enable(uint32_t irq, int enable);
void send_sgi(uint32_t sgi, int cpu);

void irq_set_affinity(uint32_t irq, int cpu);
void irq_set_type(uint32_t irq, int type);
int irq_get_virq_nr(void);
void irq_clear_pending(uint32_t irq);

int irq_xlate(struct device_node *node, uint32_t *intspec,
		unsigned int intsize, uint32_t *hwirq, unsigned long *f);

struct irq_desc *get_irq_desc(uint32_t irq);

static inline void irq_unmask(uint32_t irq)
{
	__irq_enable(irq, 1);
}

static inline void irq_mask(uint32_t irq)
{
	__irq_enable(irq, 0);
}

#endif
