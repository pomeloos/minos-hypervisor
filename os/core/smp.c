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
#include <config/config.h>
#include <minos/percpu.h>
#include <minos/platform.h>
#include <minos/irq.h>

#define SMP_CALL_LOCKED		(1 << 0)

extern unsigned char __smp_affinity_id;
uint64_t *smp_affinity_id;
phy_addr_t smp_holding_address[CONFIG_NR_CPUS];

cpumask_t cpu_online;
static int cpus_all_up;

struct smp_call {
	smp_function fn;
	unsigned long flags;
	void *data;
};

struct smp_call_data {
	struct smp_call smp_calls[NR_CPUS];
};

static DEFINE_PER_CPU(struct smp_call_data, smp_call_data);

static void inline smp_call_wait(struct smp_call *call)
{
	/* need wait for last call finished */
	while (call->flags & SMP_CALL_LOCKED)
		cpu_relax();
}

static void inline smp_call_lock(struct smp_call *call)
{
	if (call->flags & SMP_CALL_LOCKED)
		pr_warn("smp call is already locked\n");

	call->flags |= SMP_CALL_LOCKED;
	wmb();
}

static void inline smp_call_unlock(struct smp_call *call)
{
	call->flags &= ~SMP_CALL_LOCKED;
	wmb();
}

int is_cpus_all_up(void)
{
	return cpus_all_up;
}

int smp_function_call(int cpu, smp_function fn, void *data, int wait)
{
	int cpuid;
	struct smp_call *call;
	struct smp_call_data *cd;
	unsigned long flags;

	preempt_disable();
	cpuid = smp_processor_id();

	if (cpu >= NR_CPUS)
		return -EINVAL;

	/* function call itself just call the function */
	if (cpu == cpuid) {
		local_irq_save(flags);
		fn(data);
		local_irq_restore(flags);
		preempt_enable();
		return 0;
	}

	cd = &get_per_cpu(smp_call_data, cpu);
	call = &cd->smp_calls[cpuid];

	smp_call_wait(call);
	call->fn = fn;
	call->data = data;
	smp_call_lock(call);

	send_sgi(SMP_FUNCTION_CALL_IRQ, cpu);

	if (wait)
		smp_call_wait(call);

	preempt_enable();

	return 0;
}

static int smp_function_call_handler(uint32_t irq, void *data)
{
	int i;
	struct smp_call_data *cd;
	struct smp_call *call;

	cd = &get_cpu_var(smp_call_data);

	for (i = 0; i < NR_CPUS; i++) {
		call = &cd->smp_calls[i];
		if (call->flags & SMP_CALL_LOCKED) {
			call->fn(call->data);
			call->fn = NULL;
			call->data = NULL;
			smp_call_unlock(call);
		}
	}

	return 0;
}

int smp_cpu_up(unsigned long cpu, unsigned long entry)
{
	if (platform->cpu_on)
		return platform->cpu_on(cpu, entry);

	pr_warn("no cpu on function\n");
	return 0;
}

void smp_cpus_up(void)
{
	int i, ret, cnt;
	uint64_t affinity;

	flush_cache_all();

	for (i = 1; i < CONFIG_NR_CPUS; i++) {
		cnt = 0;
		affinity = cpuid_to_affinity(i);

		ret = smp_cpu_up(affinity, CONFIG_MINOS_ENTRY_ADDRESS);
		if (ret) {
			pr_fatal("failed to bring up cpu-%d\n", i);
			continue;
		}

		pr_info("waiting 2 seconds for cpu-%d up\n", i);
		while ((smp_affinity_id[i] == 0) && (cnt < 2000)) {
			mdelay(1);
			cnt++;
		}

		if (smp_affinity_id[i] == 0) {
			pr_err("cpu-%d is not up with affinity id 0x%p\n",
					i, smp_affinity_id[i]);
		} else
			cpumask_set_cpu(i, &cpu_online);
	}

	cpus_all_up = 1;
	wmb();
}

void smp_init(void)
{
	int i;
	struct smp_call_data *cd;

	smp_affinity_id = (uint64_t *)&__smp_affinity_id;
	memset(smp_affinity_id, 0, sizeof(uint64_t) * NR_CPUS);

	cpumask_clearall(&cpu_online);
	cpumask_set_cpu(0, &cpu_online);

	for (i = 0; i < NR_CPUS; i++) {
		cd = &get_per_cpu(smp_call_data, i);
		memset(cd, 0, sizeof(struct smp_call_data));
	}

	arch_smp_init(smp_holding_address);

	request_irq_percpu(SMP_FUNCTION_CALL_IRQ,
			smp_function_call_handler, 0,
			"smp_function_call", NULL);
}
