/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_cpu_masks.h"
#include "common/kr_incs.h"

int nvmeibc_b_dp_cpu_masks_init(struct nvmeibc_b_dp_cpu_masks *cpu_masks)
{
	cpu_masks->percpu = nvmeib_public_alloc_percpu_cacheline(struct nvmeib_cpu_mask_info);
	if (!cpu_masks->percpu){
		return -ENOMEM;
	}

	nvmeib_public_zero_percpu(cpu_masks->percpu);

	return 0;
}

void nvmeibc_b_dp_cpu_masks_fini(struct nvmeibc_b_dp_cpu_masks *cpu_masks)
{
	if (cpu_masks){
		nvmeib_public_free_percpu(cpu_masks->percpu);
	}
}

struct add_del_on_cpu_arg {
	struct nvmeibc_b_dp_cpu_masks *cpu_masks;
	const struct nvmeib_cpu_mask *cpu_mask;
	union {
		struct {
			u64 gen;
		} add;
	};
};

static void __add_on_cpu(void *_arg)
{
	struct add_del_on_cpu_arg *arg = _arg;
	const struct nvmeib_cpu_mask *cpu_mask = arg->cpu_mask;
	int const cpu = smp_processor_id();
	struct nvmeib_cpu_mask_info* this_cpu_mask_info = per_cpu_ptr(arg->cpu_masks->percpu, cpu);

	if ((cpu < NVMEIB_CPU_MASK_MAX_CPUS) && NVMEIB_CPU_MASK_TEST_CPU(cpu, *cpu_mask)) {
		NVMEIB_CPU_MASK_COPY(this_cpu_mask_info->mask, *cpu_mask);
		this_cpu_mask_info->gen = arg->add.gen;
	}
}

static void __del_on_cpu(void *_arg)
{
	struct add_del_on_cpu_arg *arg = _arg;
	const struct nvmeib_cpu_mask *cpu_mask = arg->cpu_mask;
	int const cpu = smp_processor_id();
	struct nvmeib_cpu_mask_info* this_cpu_mask_info = per_cpu_ptr(arg->cpu_masks->percpu, cpu);

	if ((cpu < NVMEIB_CPU_MASK_MAX_CPUS) && NVMEIB_CPU_MASK_TEST_CPU(cpu, *cpu_mask))
		bitmap_zero(this_cpu_mask_info->mask.cpus, NVMEIB_CPU_MASK_MAX_CPUS);
}

void nvmeibc_b_dp_cpu_masks_add(struct nvmeibc_b_dp_cpu_masks *cpu_masks, struct nvmeib_cpu_mask *cpu_mask, u64 gen)
{
	struct add_del_on_cpu_arg arg = { .cpu_masks = cpu_masks, .cpu_mask = cpu_mask, .add = { .gen = gen } };
	on_each_cpu(__add_on_cpu, &arg, true /* wait */);
}

void nvmeibc_b_dp_cpu_masks_del(struct nvmeibc_b_dp_cpu_masks *cpu_masks, struct nvmeib_cpu_mask *cpu_mask)
{
	struct add_del_on_cpu_arg arg = { .cpu_masks = cpu_masks, .cpu_mask = cpu_mask };
	on_each_cpu(__del_on_cpu, &arg, true /* wait */);
}

void nvmeibc_b_dp_cpu_masks_get_on_cpu(const struct nvmeibc_b_dp_cpu_masks *cpu_masks, struct nvmeib_cpu_mask_info *cpu_mask_info)
{
	#if defined(UM_APP)
		/*UM_APP does not use this functionality*/
		(void)cpu_masks;                
		*cpu_mask_info = (struct nvmeib_cpu_mask_info){ 0 };
	#else
		*cpu_mask_info = *this_cpu_ptr(cpu_masks->percpu);
	#endif
}
