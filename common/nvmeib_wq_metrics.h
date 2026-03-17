/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_WQ_METRICS_H
#define NVMEIB_WQ_METRICS_H

#include "common/kr_incs.h"
#include "common/nvmeib_metrics.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

/**
 * struct nvmeib_wq_metric_counters - per-CPU counters for a work-queue metric.
 * @wait_time: histogram of queue-wait durations in raw TSC ticks.
 *
 * Groups every metric field that is accumulated per CPU for a single
 * work-queue metric point. To add a new metric (gauge, counter, etc.),
 * add a field here and update nvmeib_wq_metric_counters_merge(),
 * nvmeib_wq_metrics_visit(), and nvmeib_wq_metrics_clear() accordingly.
 */
struct nvmeib_wq_metric_counters {
	struct nvmesh_metric_highres_histogram wait_time;
	/* future: gauges, counters, etc. */
};

/**
 * nvmeib_wq_metric_counters_merge() - merge all counter fields from src into dst.
 * @dst: destination counters to accumulate into.
 * @src: source counters (typically a single-CPU snapshot).
 *
 * Accumulates every metric field of @src into @dst.
 * Called once per CPU inside nvmeib_wq_metrics_merge_cpus().
 */
void nvmeib_wq_metric_counters_merge(struct nvmeib_wq_metric_counters *dst,
					struct nvmeib_wq_metric_counters const *src);

/**
 * struct nvmeib_wq_metrics - a labeled work-queue metric point.
 * @labels:   metric labels, e.g. "module=nvmeibc;reason=throttle".
 * @counters: per-CPU counters allocated by nvmeib_wq_metrics_alloc_pcpu().
 *            NULL until allocation; must not be dereferenced before init.
 *
 * Each instance tracks one logical metric with per-CPU counters. Instances
 * are placed into a linker section via NVMESH_DEFINE_WQ_METRIC so they can
 * be iterated at init/cleanup/serialization time using the section
 * [start, stop) range.
 *
 * The metric name (e.g. "wq.wait_time") is a property of each counter
 * field, not of the instance, and is hardcoded in nvmeib_wq_metrics_visit().
 */
struct nvmeib_wq_metrics {
	char const *const labels;
	struct nvmeib_wq_metric_counters *__percpu counters;
};

/**
 * NVMESH_DEFINE_WQ_METRIC() - define a nvmeib_wq_metrics instance in a linker section.
 * @var_name:      name of the pointer variable created for the metric.
 * @section:       ELF section name string, e.g. ".nvmeibc_wq_metrics".
 * @metric_labels: metric labels string, e.g. "module=nvmeibc;reason=throttle".
 *
 * Creates a static &struct nvmeib_wq_metrics placed into the given ELF section
 * (8-byte aligned), and a convenience pointer variable for call-site use.
 * The .counters field is initialized to %NULL; the caller must call
 * nvmeib_wq_metrics_alloc_pcpu() during module init before any updates.
 */
#define NVMESH_DEFINE_WQ_METRIC(var_name, section, metric_labels) \
	static struct nvmeib_wq_metrics var_name##_wqh \
		NVMESH_USED NVMESH_SECTION(section) NVMESH_ALIGNED(8) = { \
		.labels = metric_labels, \
		.counters = NULL \
	}; \
	static struct nvmeib_wq_metrics * NVMESH_USED var_name = &var_name##_wqh

/**
 * nvmeib_wq_metrics_update() - record a queue-wait duration on the current CPU.
 * @wqh:   metric point to update.
 * @ticks: queue-wait duration in raw TSC ticks.
 *
 * Hot-path inline function. Must be called only after
 * nvmeib_wq_metrics_alloc_pcpu() has initialized the counters.
 *
 * Context: Disables preemption internally via get_cpu()/put_cpu().
 */
static inline void nvmeib_wq_metrics_update(struct nvmeib_wq_metrics *wqh, u64 ticks)
{
	struct nvmeib_wq_metric_counters *c = per_cpu_ptr(wqh->counters, get_cpu());
	nvmesh_metric_highres_histogram_update(&c->wait_time, ticks);
	put_cpu();
}

/**
 * nvmeib_wq_metrics_merge_cpus() - aggregate counters from all CPUs.
 * @src: metric point whose per-CPU counters are summed.
 *
 * Iterates every possible CPU and merges per-CPU counters into a single
 * snapshot. The returned struct is a stack copy; it does not alias any
 * per-CPU memory.
 *
 * Return: aggregated counters snapshot (zero-initialized if no CPU has data).
 */
struct nvmeib_wq_metric_counters nvmeib_wq_metrics_merge_cpus(struct nvmeib_wq_metrics const *src);

/**
 * nvmeib_wq_metrics_visit_cpu() - visit metrics for a specific CPU.
 * @self:    metric point to visit.
 * @closure: visitor closure, e.g. a JDR writer.
 * @cpu:     CPU index to visit, or -1 to merge all CPUs.
 */
void nvmeib_wq_metrics_visit_cpu(struct nvmeib_wq_metrics *self, struct nvmesh_metrics_closure *closure, int cpu);

/**
 * nvmeib_wq_metrics_visit() - visit metrics via a metrics closure.
 * @self:          metric point to visit.
 * @closure:       visitor closure, e.g. a JDR writer.
 * @visit_all_cpus: true to merge all CPUs, false for current CPU only.
 *
 * If @visit_all_cpus is true, merges counters from all CPUs before visiting;
 * otherwise visits only the current CPU's counters.
 */
void nvmeib_wq_metrics_visit(struct nvmeib_wq_metrics *self, struct nvmesh_metrics_closure *closure,
				bool visit_all_cpus);

/**
 * nvmeib_wq_metrics_json_serialize() - serialize a section range as JDR JSON.
 * @buffer:        destination buffer for serialized JSON.
 * @dump_all_cpus: true to merge all CPUs per metric point.
 * @dump_per_cpu:  true to emit per-CPU sections instead of one aggregate.
 * @start:         first metric in the linker section range.
 * @stop:          one-past-last metric in the linker section range.
 *
 * When @dump_all_cpus && !@dump_per_cpu, emits a single "metrics.all_cpus"
 * array with merged counters (current behaviour).
 * When @dump_all_cpus && @dump_per_cpu, emits one "metrics.cpu{N}" array per
 * online CPU with that CPU's raw counters.
 *
 * Return: number of bytes written into @buffer.
 */
size_t nvmeib_wq_metrics_json_serialize(struct charvec buffer, bool dump_all_cpus, bool dump_per_cpu,
					struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop);

/**
 * nvmeib_wq_metrics_alloc_pcpu() - allocate per-CPU counters for a section range.
 * @start: first metric in the linker section range.
 * @stop:  one-past-last metric in the linker section range.
 *
 * Must be called during module init before any nvmeib_wq_metrics_update() calls.
 * On failure, frees all counters already allocated in [@start, curr) and returns
 * -%ENOMEM. Warns if counters are already allocated.
 *
 * Return: 0 on success, -%ENOMEM on allocation failure.
 */
int nvmeib_wq_metrics_alloc_pcpu(struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop);

/**
 * nvmeib_wq_metrics_free_pcpu() - free per-CPU counters for a section range.
 * @start: first metric in the linker section range.
 * @stop:  one-past-last metric in the linker section range.
 *
 * Must be called during module cleanup. Safe to call on already-%NULL counters.
 * After this call, nvmeib_wq_metrics_update() must not be called on any
 * metric in the range.
 */
void nvmeib_wq_metrics_free_pcpu(struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop);

/**
 * nvmeib_wq_metrics_clear() - zero all per-CPU counters for a section range.
 * @start: first metric in the linker section range.
 * @stop:  one-past-last metric in the linker section range.
 *
 * Runs on every online CPU via on_each_cpu() to clear counters locally.
 * This avoids cross-CPU writes to per-CPU data. Blocks until all CPUs finish.
 */
void nvmeib_wq_metrics_clear(struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop);

#endif /* NVMEIB_WQ_METRICS_H */
