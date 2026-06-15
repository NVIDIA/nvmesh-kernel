/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_MEMMGR_METRICS_H_INCLUDED
#define NVMEIB_MEMMGR_METRICS_H_INCLUDED

#include "kr_incs.h"
#include "nvmeib_metrics.h"
#include "nvmeib_jdr.h"

//Important: pay attention to update nvmesh_memmgr_metrics_json_serialize function, if you modify the metrics set

struct nvmesh_memmgr_metric_counters {
	/* the allocated gauge might be negative, as alloc/free may occur on different cpus */
	struct nvmesh_metric_gauge allocated;             /* metric name: memory.allocated */
	struct nvmesh_metric_gauge active_allocations;    /* metric name: memory.active_allocations */
	struct nvmesh_metric_max_value max_allocated;     /* metric name: memory.max_allocated */
	struct nvmesh_metric_monotonic_counter failures;  /* metric name: memory.allocation_failures */
	struct nvmesh_metric_bytes_histogram allocation_distribution; /* metric name: memory.allocation_distribution */
};

void nvmesh_memmgr_metric_counters_merge(struct nvmesh_memmgr_metric_counters *dst, struct nvmesh_memmgr_metric_counters const* src);

struct nvmesh_memmgr_metrics {
	char const * const labels; /* module=(nvmeib|nvmeibc|nvmeibs);component=(datapath|preread|...) */
	struct nvmesh_memmgr_metric_counters __percpu *counters;
};

static inline void nvmesh_memmgr_metric_on_alloc_update(struct nvmesh_memmgr_metrics *mgr, size_t size, bool success)
{
	struct nvmesh_memmgr_metric_counters *self;
	if (!mgr->counters)
		return;

	self = per_cpu_ptr(mgr->counters, get_cpu());
	if (success) {
		nvmesh_metric_update(self->active_allocations, +1);
		nvmesh_metric_update(self->allocated, size);
		nvmesh_metric_update(self->max_allocated, size);
		nvmesh_metric_update(self->allocation_distribution, size);
	} else {
		nvmesh_metric_update(self->failures, 1);
	}
	put_cpu();
}

static inline void nvmesh_memmgr_metric_on_free_update(struct nvmesh_memmgr_metrics *mgr, size_t size)
{
	struct nvmesh_memmgr_metric_counters *self;
	if (!mgr->counters)
		return;

	self = per_cpu_ptr(mgr->counters, get_cpu());
	nvmesh_metric_update(self->active_allocations, -1);
	nvmesh_metric_update(self->allocated, -size);

	put_cpu();
}

/* if visit_all_cpus == false - visit the current cpu */
void nvmesh_memmgr_metrics_visit(struct nvmesh_memmgr_metrics* self, struct nvmesh_metrics_closure* closure, bool visit_all_cpus);

/* if cpu == -1 - visit all CPUs, else the specific one */
void nvmesh_memmgr_metrics_visit2(struct nvmesh_memmgr_metrics* self, struct nvmesh_metrics_closure* closure, int cpu);

// Macro to define and initialize per-CPU counters and global variable in a specified section
#define NVMESH_DEFINE_MEMMGR_METRIC(name, section, mm_labels) 	   \
	static const char * const name##_labels = mm_labels; \
	static struct nvmesh_memmgr_metrics name##_mm NVMESH_USED NVMESH_SECTION(section) NVMESH_ALIGNED(8) = {  \
		.labels = name##_labels, 									 \
		.counters = NULL \
	}; \
	static struct nvmesh_memmgr_metrics* name = &name##_mm;

struct nvmesh_memmgr_metric_counters nvmesh_memmgr_metrics_merge_cpus(struct nvmesh_memmgr_metrics const* metrics);

size_t nvmesh_memmgr_metrics_json_serialize(struct charvec buffer, bool dump_all_cpus, bool dump_each_cpu_separately,
					    struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop);

void nvmesh_memmgr_metrics_clear(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop);

int nvmesh_memmgr_metrics_verify_idle(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop);

// Initialize all memmgr metrics in a section
int nvmesh_memmgr_metrics_alloc_pcpu(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop);

// Cleanup all memmgr metrics in a section
void nvmesh_memmgr_metrics_free_pcpu(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop);

#ifndef __KERNEL__
int nvmesh_memmgr_metrics_dump_to_file(const char *filename, struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop);
#endif
#endif // NVMEIB_MEMMGR_METRICS_H_INCLUDED
