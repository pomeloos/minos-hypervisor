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
#include <asm/svccc.h>
#include <minos/string.h>

extern unsigned char __hvc_handler_start;
extern unsigned char __hvc_handler_end;
extern unsigned char __smc_handler_start;
extern unsigned char __smc_handler_end;

static struct svc_desc *smc_descs[SVC_STYPE_MAX];
static struct svc_desc *hvc_descs[SVC_STYPE_MAX];

int do_svc_handler(gp_regs *regs, uint32_t svc_id, uint64_t *args, int smc)
{
	uint16_t type;
	struct svc_desc **table;
	struct svc_desc *desc;

	if (smc)
		table = smc_descs;
	else
		table = hvc_descs;

	type = (svc_id & SVC_STYPE_MASK) >> 24;

	if (unlikely(type > SVC_STYPE_MAX)) {
		pr_err("Unsupported SVC type %d\n", type);
		goto invalid;
	}

	desc = table[type];
	if (unlikely(!desc))
		goto invalid;

	pr_debug("doing SVC Call %s:0x%x\n", desc->name, svc_id);

	return desc->handler(regs, svc_id, args);

invalid:
	SVC_RET1(regs, -EINVAL);
}

static void parse_svc_desc(unsigned long start, unsigned long end,
			   struct svc_desc **table)
{
	struct svc_desc *desc;
	int32_t j;

	section_for_each_item_addr(start, end, desc) {
		BUG_ON((desc->type_start > desc->type_end) ||
		       (desc->type_end >= SVC_STYPE_MAX));
		for (j = desc->type_start; j <= desc->type_end; j++) {
			if (table[j])
				pr_warn("overwrite SVC_DESC:%d %s\n", j,
					desc->name);
			table[j] = desc;
		}
	}
}

static int svc_service_init(void)
{
	pr_info("parsing SMC/HVC handler\n");

	parse_svc_desc((unsigned long)&__hvc_handler_start,
		       (unsigned long)&__hvc_handler_end, hvc_descs);
	parse_svc_desc((unsigned long)&__smc_handler_start,
		       (unsigned long)&__smc_handler_end, smc_descs);

	return 0;
}

arch_initcall(svc_service_init);
