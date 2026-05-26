/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_IO_PET_ADMISSION_H_INCLUDED
#define NVMEIBC_IO_PET_ADMISSION_H_INCLUDED

#include "kr_incs.h"
#include "common/pet/nvmeib_pet_specification.h"

enum { NVMEIBC_IO_PET_MAX_RELEASE_CPU = (int)(((u16)~0U) >> 1) };

struct nvmeibc_io_pet_pcpu_counter {
	atomic_t active;
} ____cacheline_aligned;

static inline int nvmeibc_io_pet_effective_cpu_cap(unsigned const *max_traced_ops_per_cpu)
{
	unsigned const cap = READ_ONCE(*max_traced_ops_per_cpu);

	return cap > INT_MAX ? INT_MAX : (int)cap;
}

static inline atomic_t *nvmeibc_io_pet_active_counter(struct nvmeibc_io_pet_pcpu_counter __percpu *active_traced_ops,
						      int cpu)
{
	return &per_cpu_ptr(active_traced_ops, cpu)->active;
}

static inline bool nvmeibc_io_pet_try_acquire_slot(struct nvmeibc_io_pet_pcpu_counter __percpu *active_traced_ops,
						   unsigned const *max_traced_ops_per_cpu, s16 *release_cpu)
{
	int const cap = nvmeibc_io_pet_effective_cpu_cap(max_traced_ops_per_cpu);
	int cpu;
	atomic_t *active;
	int old;

	*release_cpu = NVMEIB_PET_NO_RELEASE_CPU;
	if (!cap) {
		return true;
	}

	BUILD_BUG_ON(NR_CPUS > NVMEIBC_IO_PET_MAX_RELEASE_CPU + 1);
	cpu = get_cpu();
	active = nvmeibc_io_pet_active_counter(active_traced_ops, cpu);
	old = atomic_read(active);

	/*
	 * This relies on admission running only from the new-BIO operation
	 * creation path, not from same-CPU IRQ/softirq reentry while get_cpu()
	 * is held. Releases may race from other CPUs, but they only decrement
	 * the counter, so old < cap followed by atomic_inc() cannot exceed cap.
	 */
	if (old < cap) {
		atomic_inc(active);
		*release_cpu = (s16)cpu;
		put_cpu();
		return true;
	}

	put_cpu();
	return false;
}

static inline void nvmeibc_io_pet_release_slot(struct nvmeibc_io_pet_pcpu_counter __percpu *active_traced_ops,
					       s16 release_cpu)
{
	if (release_cpu >= 0) {
		atomic_dec(nvmeibc_io_pet_active_counter(active_traced_ops, release_cpu));
	}
}

#endif /* NVMEIBC_IO_PET_ADMISSION_H_INCLUDED */
