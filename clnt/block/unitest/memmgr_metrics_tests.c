/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "memmgr_metrics_tests.h"
#include "nvmeibc_memmgr_metrics.h"
#include "kr_incs.h"

NVMEIBC_MEMMGR_METRIC(mm_zero, "test");
NVMEIBC_MEMMGR_METRIC(mm_one, "test");
NVMEIBC_MEMMGR_METRIC(mm_two, "test");

void __ut_mm_populate(void)
{
	nvmesh_memmgr_metric_on_alloc_update(mm_zero, +1024, false); //failed

	nvmesh_memmgr_metric_on_alloc_update(mm_one, +1024, true);
	nvmesh_memmgr_metric_on_alloc_update(mm_two, +2048, true);

	nvmesh_memmgr_metric_on_free_update(mm_one, 1024);
	nvmesh_memmgr_metric_on_free_update(mm_two, 2048);
}

void __ut_mm_test_section(void)
{
	size_t mm_total_count = 0;
	size_t mm_xyz_found = 0;
	for(struct nvmesh_memmgr_metrics* curr = __start_nvmeibc_memmgr_metrics; curr != __stop_nvmeibc_memmgr_metrics; ++curr){
		mm_total_count += 1;
		mm_xyz_found += (curr == mm_zero || curr == mm_one || curr == mm_two);
	}
	BUG_ON(mm_total_count < 3);
	BUG_ON(mm_xyz_found != 3);
}

void __ut_mm_test_json_serialize(void)
{
	struct charvec buffer = {.base = malloc(1024*1024), .len = 1024*1024};
	nvmesh_memmgr_metrics_json_serialize(buffer, true /* dump_all_cpus */, false /* dump_each_cpu_separately */, __start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);

#ifdef DO_JSON_DUMP
	printf("%s\n", buffer.base);
#endif
	 __ut_mm_populate();

	nvmesh_memmgr_metrics_json_serialize(buffer, true /* dump_all_cpus */, true /* dump_each_cpu_separately */, __start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);

#ifdef DO_JSON_DUMP
	printf("%s\n", buffer.base);
#endif

	fflush(stdout);
	free(buffer.base);
}

static void __ut_mm_test_clear(void)
{
	struct nvmesh_memmgr_metric_counters merged;

	nvmesh_memmgr_metrics_clear(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);

	merged = nvmesh_memmgr_metrics_merge_cpus(mm_zero);
	BUG_ON(merged.allocated.counter != 0);
	BUG_ON(merged.active_allocations.counter != 0);
	BUG_ON(merged.max_allocated.counter != 0);
	BUG_ON(merged.failures.counter != 0);
	BUG_ON(nvmesh_metric_bytes_histogram_total_allocations(&merged.allocation_distribution) != 0);

	merged = nvmesh_memmgr_metrics_merge_cpus(mm_one);
	BUG_ON(merged.allocated.counter != 0);
	BUG_ON(merged.active_allocations.counter != 0);
	BUG_ON(merged.max_allocated.counter != 0);
	BUG_ON(merged.failures.counter != 0);
	BUG_ON(nvmesh_metric_bytes_histogram_total_allocations(&merged.allocation_distribution) != 0);

	merged = nvmesh_memmgr_metrics_merge_cpus(mm_two);
	BUG_ON(merged.allocated.counter != 0);
	BUG_ON(merged.active_allocations.counter != 0);
	BUG_ON(merged.max_allocated.counter != 0);
	BUG_ON(merged.failures.counter != 0);
	BUG_ON(nvmesh_metric_bytes_histogram_total_allocations(&merged.allocation_distribution) != 0);
}

void test_memmgr_metrics(void)
{
	__ut_mm_test_section();
	__ut_mm_test_json_serialize();
	__ut_mm_test_clear();
}
