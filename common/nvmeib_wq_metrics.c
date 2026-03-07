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

void nvmeib_wq_metrics_visit(struct nvmeib_wq_metrics *self, struct nvmesh_metrics_closure *closure,
			     bool visit_all_cpus)
{
	struct nvmesh_metric_id const id = { .name = "wq.wait_time", .labels = self->labels };

	struct nvmeib_wq_metric_counters merged = visit_all_cpus
			? nvmeib_wq_metrics_merge_cpus(self)
			: *per_cpu_ptr(self->counters, smp_processor_id());

	nvmesh_metric_visit_ptr(closure, NULL, &merged.wait_time, id);
}
EXPORT_SYMBOL(nvmeib_wq_metrics_visit);

size_t nvmeib_wq_metrics_json_serialize(struct charvec buffer, bool dump_all_cpus, struct nvmeib_wq_metrics *start,
					struct nvmeib_wq_metrics *stop)
{
	struct charvec result = { 0 };
	struct jdr jdr_inst = jdr_make(buffer);
	struct nvmeib_jdr_write_closure jdr_writer = nvmeib_jdr_write_closure_create(&jdr_inst);
	struct nvmeib_wq_metrics *curr = NULL;

	jdr_write_var(&jdr_inst, version, 1);
	{
		static char const *all_cpu = "metrics.all_cpus";
		jdr_array_scope((&jdr_inst), all_cpu);
		for (curr = start; curr < stop; curr++) {
			nvmeib_wq_metrics_visit(curr, &(jdr_writer.base), dump_all_cpus);
		}
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
