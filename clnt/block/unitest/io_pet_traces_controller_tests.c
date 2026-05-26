/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "io_pet_traces_controller_tests.h"
#include "kr_incs.h"
#include "nvmeibc_io_pet_admission.h"

static void __test_admission_cap_exceeded(void)
{
	struct nvmeibc_io_pet_pcpu_counter active_traced_ops[NR_CPUS] = { 0 };
	unsigned const cap = 1;
	s16 first_release_cpu = NVMEIB_PET_NO_RELEASE_CPU;
	s16 second_release_cpu = NVMEIB_PET_NO_RELEASE_CPU;
	int cpu;

	BUG_ON(!nvmeibc_io_pet_try_acquire_slot(active_traced_ops, &cap, &first_release_cpu));
	BUG_ON(first_release_cpu < 0);
	BUG_ON(nvmeibc_io_pet_try_acquire_slot(active_traced_ops, &cap, &second_release_cpu));
	BUG_ON(second_release_cpu != NVMEIB_PET_NO_RELEASE_CPU);

	nvmeibc_io_pet_release_slot(active_traced_ops, first_release_cpu);
	BUG_ON(!nvmeibc_io_pet_try_acquire_slot(active_traced_ops, &cap, &second_release_cpu));
	nvmeibc_io_pet_release_slot(active_traced_ops, second_release_cpu);

	for_each_possible_cpu(cpu) {
		BUG_ON(atomic_read(&active_traced_ops[cpu].active));
	}
}

static void __test_admission_unlimited_does_not_use_release_cpu(void)
{
	struct nvmeibc_io_pet_pcpu_counter active_traced_ops[NR_CPUS] = { 0 };
	unsigned const cap = 0;
	s16 first_release_cpu = NVMEIB_PET_NO_RELEASE_CPU;
	s16 second_release_cpu = NVMEIB_PET_NO_RELEASE_CPU;
	int cpu;

	BUG_ON(!nvmeibc_io_pet_try_acquire_slot(active_traced_ops, &cap, &first_release_cpu));
	BUG_ON(!nvmeibc_io_pet_try_acquire_slot(active_traced_ops, &cap, &second_release_cpu));
	BUG_ON(first_release_cpu != NVMEIB_PET_NO_RELEASE_CPU);
	BUG_ON(second_release_cpu != NVMEIB_PET_NO_RELEASE_CPU);

	for_each_possible_cpu(cpu) {
		BUG_ON(atomic_read(&active_traced_ops[cpu].active));
	}
}

void test_io_pet_controller_admission(void)
{
	__test_admission_cap_exceeded();
	__test_admission_unlimited_does_not_use_release_cpu();
}
