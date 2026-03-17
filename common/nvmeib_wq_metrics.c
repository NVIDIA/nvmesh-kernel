/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_wq_metrics.h"
#include "nvmeib_metrics_jdr.h"

void nvmeib_wq_metric_counters_merge(struct nvmeib_wq_metric_counters *dst, struct nvmeib_wq_metric_counters const *src)
{
	nvmesh_metric_highres_histogram_merge(&dst->wait_time, &src->wait_time);
}
EXPORT_SYMBOL(nvmeib_wq_metric_counters_merge);

struct nvmeib_wq_metric_counters nvmeib_wq_metrics_merge_cpus(struct nvmeib_wq_metrics const *src)
{
	int cpu;
	struct nvmeib_wq_metric_counters dst = { 0 };

	for_each_possible_cpu(cpu) {
		struct nvmeib_wq_metric_counters *src_counters = per_cpu_ptr(src->counters, cpu);
		nvmeib_wq_metric_counters_merge(&dst, src_counters);
	}
	return dst;
}
EXPORT_SYMBOL(nvmeib_wq_metrics_merge_cpus);

void nvmeib_wq_metrics_visit_cpu(struct nvmeib_wq_metrics *self, struct nvmesh_metrics_closure *closure, int cpu)
{
	struct nvmesh_metric_id const id = { .name = "wq.wait_time", .labels = self->labels };

	struct nvmeib_wq_metric_counters merged = (cpu == -1)
			? nvmeib_wq_metrics_merge_cpus(self)
			: *per_cpu_ptr(self->counters, cpu);

	nvmesh_metric_visit_ptr(closure, NULL, &merged.wait_time, id);
}
EXPORT_SYMBOL(nvmeib_wq_metrics_visit_cpu);

void nvmeib_wq_metrics_visit(struct nvmeib_wq_metrics *self, struct nvmesh_metrics_closure *closure,
			     bool visit_all_cpus)
{
	nvmeib_wq_metrics_visit_cpu(self, closure, visit_all_cpus ? -1 : smp_processor_id());
}
EXPORT_SYMBOL(nvmeib_wq_metrics_visit);

static void __wq_metrics_json_serialize_all_cpus(struct jdr *jdr_inst, struct nvmeib_jdr_write_closure *jdr_writer,
						 struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop)
{
	static char const *all_cpu = "metrics.all_cpus";
	struct nvmeib_wq_metrics *curr = NULL;

	jdr_array_scope(jdr_inst, all_cpu);
	for (curr = start; curr < stop; curr++)
		nvmeib_wq_metrics_visit_cpu(curr, &(jdr_writer->base), -1 /* visit all cpus */);
}

static void __wq_metrics_json_serialize_cpu(struct jdr *jdr_inst, struct nvmeib_jdr_write_closure *jdr_writer, int cpu,
					    struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop)
{
	char scope_str[32];

	snprintf(scope_str, ARRAY_SIZE(scope_str), "metrics.cpu%d", cpu);
	{
		struct nvmeib_wq_metrics *curr = NULL;

		jdr_array_scope(jdr_inst, scope_str);
		for (curr = start; curr < stop; curr++)
			nvmeib_wq_metrics_visit_cpu(curr, &(jdr_writer->base), cpu);
	}
}

size_t nvmeib_wq_metrics_json_serialize(struct charvec buffer, bool dump_all_cpus, bool dump_per_cpu,
					struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop)
{
	int cpu;
	struct charvec result = { 0 };
	struct jdr jdr_inst = jdr_make(buffer);
	struct nvmeib_jdr_write_closure jdr_writer = nvmeib_jdr_write_closure_create(&jdr_inst);

	jdr_write_var(&jdr_inst, version, 1);
	if (dump_all_cpus && !dump_per_cpu) {
		__wq_metrics_json_serialize_all_cpus(&jdr_inst, &jdr_writer, start, stop);
	} else if (dump_all_cpus && dump_per_cpu) {
		for_each_online_cpu(cpu) {
			__wq_metrics_json_serialize_cpu(&jdr_inst, &jdr_writer, cpu, start, stop);
		}
	} else /* dump_all_cpus == false */ {
		__wq_metrics_json_serialize_cpu(&jdr_inst, &jdr_writer, smp_processor_id(), start, stop);
	}
	result = jdr_finalize(&jdr_inst);
	return result.len;
}
EXPORT_SYMBOL(nvmeib_wq_metrics_json_serialize);

static void __wq_metrics_clear_cpu(void *info)
{
	struct nvmeib_wq_metrics **range = info;
	struct nvmeib_wq_metrics *start = range[0];
	struct nvmeib_wq_metrics *stop = range[1];
	struct nvmeib_wq_metrics *curr;
	int cpu = get_cpu();

	for (curr = start; curr < stop; curr++) {
		struct nvmeib_wq_metric_counters *c = per_cpu_ptr(curr->counters, cpu);
		nvmesh_metric_highres_histogram_clear(&c->wait_time);
	}

	put_cpu();
}

void nvmeib_wq_metrics_clear(struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop)
{
	struct nvmeib_wq_metrics *range[2] = { start, stop };

	on_each_cpu(__wq_metrics_clear_cpu, range, 1 /* wait */);
}
EXPORT_SYMBOL(nvmeib_wq_metrics_clear);

int nvmeib_wq_metrics_alloc_pcpu(struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop)
{
	struct nvmeib_wq_metrics *curr = NULL;
	int rv = 0;

	for (curr = start; curr < stop; ++curr) {
		WARN(curr->counters, "counters already allocated");
		curr->counters = nvmeib_public_alloc_percpu_zeroed(struct nvmeib_wq_metric_counters);
		if (!curr->counters) {
			WARN(1, "Failed to alloc percpu counters");
			rv = -ENOMEM;
			nvmeib_wq_metrics_free_pcpu(start, curr);
			break;
		}
	}
	return rv;
}
EXPORT_SYMBOL(nvmeib_wq_metrics_alloc_pcpu);

void nvmeib_wq_metrics_free_pcpu(struct nvmeib_wq_metrics *start, struct nvmeib_wq_metrics *stop)
{
	struct nvmeib_wq_metrics *curr = NULL;

	for (curr = start; curr < stop; ++curr) {
		if (curr->counters) {
			nvmeib_public_free_percpu(curr->counters);
			curr->counters = NULL;
		}
	}
}
EXPORT_SYMBOL(nvmeib_wq_metrics_free_pcpu);
