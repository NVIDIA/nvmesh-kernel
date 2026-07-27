#include "nvmeib_memmgr_metrics.h"
#include "nvmeib_metrics_jdr.h"

void nvmesh_memmgr_metric_counters_merge(struct nvmesh_memmgr_metric_counters *dst, struct nvmesh_memmgr_metric_counters const* src)
{
	nvmesh_metric_merge(&(dst->allocated), &(src->allocated));
	nvmesh_metric_merge(&(dst->active_allocations), &(src->active_allocations));
	nvmesh_metric_merge(&(dst->max_allocated), &(src->max_allocated));
	nvmesh_metric_merge(&(dst->failures), &(src->failures));
	nvmesh_metric_merge(&(dst->allocation_distribution), &(src->allocation_distribution));
}
EXPORT_SYMBOL(nvmesh_memmgr_metric_counters_merge);

struct nvmesh_memmgr_metric_counters nvmesh_memmgr_metrics_merge_cpus(struct nvmesh_memmgr_metrics const* src)
{
	int cpu;
	struct nvmesh_memmgr_metric_counters dst_counters = {0};

	for_each_possible_cpu(cpu) {
		struct nvmesh_memmgr_metric_counters *src_counters = per_cpu_ptr(src->counters, cpu);

		nvmesh_memmgr_metric_counters_merge(&dst_counters, src_counters);
	}
	return dst_counters;
}

EXPORT_SYMBOL(nvmesh_memmgr_metrics_merge_cpus);

static void __memmgr_metrics_visit(struct nvmesh_memmgr_metrics* self, struct nvmesh_metrics_closure* closure, int cpu)
{
	bool visit_all_cpus = (cpu == -1); /* if cpu == -1 - visit all CPUs, else the specific one */

	struct nvmesh_metric_id const allocated_id = {
		.name = "memory.allocated",
		.labels = self->labels
	};

	struct nvmesh_metric_id const active_allocations_id = {
		.name = "memory.active_allocations",
		.labels = self->labels
	};

	struct nvmesh_metric_id const max_allocated_id = {
		.name = "memory.max_allocated",
		.labels = self->labels
	};

	struct nvmesh_metric_id const failures_id = {
		.name = "memory.allocation_failures",
		.labels = self->labels
	};

	struct nvmesh_metric_id const allocation_distribution_id = {
		.name = "memory.allocation_distribution",
		.labels = self->labels
	};

	struct nvmesh_memmgr_metric_counters merged
		= visit_all_cpus ? nvmesh_memmgr_metrics_merge_cpus(self)
						 : *per_cpu_ptr(self->counters, cpu);

	nvmesh_metric_visit_ptr(closure, NULL, &(merged.allocated), allocated_id);
	nvmesh_metric_visit_ptr(closure, NULL, &(merged.active_allocations), active_allocations_id);
	nvmesh_metric_visit_ptr(closure, NULL, &(merged.max_allocated), max_allocated_id);
	nvmesh_metric_visit_ptr(closure, NULL, &(merged.failures), failures_id);
	nvmesh_metric_visit_ptr(closure, NULL, &(merged.allocation_distribution), allocation_distribution_id);
}

void nvmesh_memmgr_metrics_visit(struct nvmesh_memmgr_metrics* self, struct nvmesh_metrics_closure* closure, bool visit_all_cpus)
{
	int cpu = visit_all_cpus ? -1 : (int)smp_processor_id();

	__memmgr_metrics_visit(self, closure, cpu);
}
EXPORT_SYMBOL(nvmesh_memmgr_metrics_visit);

void nvmesh_memmgr_metrics_visit2(struct nvmesh_memmgr_metrics* self, struct nvmesh_metrics_closure* closure, int cpu)
{
	__memmgr_metrics_visit(self, closure, cpu);
}
EXPORT_SYMBOL(nvmesh_memmgr_metrics_visit2);

static void __memmgr_metrics_json_serialize_all_cpus(struct jdr *jdr_inst, struct nvmeib_jdr_write_closure *jdr_writer,
						     struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	{
		static char const *all_cpu = "metrics.all_cpus";
		struct nvmesh_memmgr_metrics *curr = NULL;
		jdr_array_scope((jdr_inst), all_cpu);
		for (curr = start; curr < stop; curr++) {
			nvmesh_memmgr_metrics_visit2(curr, &(jdr_writer->base), -1  /* visit all cpus */);
		}
	}
}

static void __memmgr_metrics_json_serialize_cpu(struct jdr *jdr_inst, struct nvmeib_jdr_write_closure *jdr_writer, int cpu,
						struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	char scope_str[32];
	sprintf(scope_str, "metrics.cpu%d", cpu);
	{
		struct nvmesh_memmgr_metrics *curr = NULL;
		jdr_array_scope((jdr_inst), scope_str);
		for (curr = start; curr < stop; curr++) {
			nvmesh_memmgr_metrics_visit2(curr, &(jdr_writer->base), cpu /* visit specific cpu */);
		}
	}
}

size_t nvmesh_memmgr_metrics_json_serialize(struct charvec buffer, bool dump_all_cpus, bool dump_each_cpu_separately,
					    struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	int cpu;
	struct charvec result = {0};
	struct jdr jdr_inst = jdr_make(buffer);
	struct nvmeib_jdr_write_closure jdr_writer = nvmeib_jdr_write_closure_create(&jdr_inst);

	jdr_write_var(&jdr_inst, version, 1);
	if (dump_all_cpus && !dump_each_cpu_separately) {
		__memmgr_metrics_json_serialize_all_cpus(&jdr_inst, &jdr_writer, start, stop);
	} else if (dump_all_cpus && dump_each_cpu_separately) {
		for_each_online_cpu(cpu)
			__memmgr_metrics_json_serialize_cpu(&jdr_inst, &jdr_writer, cpu, start, stop);
	} else /* dump_all_cpus == false */ {
		__memmgr_metrics_json_serialize_cpu(&jdr_inst, &jdr_writer, smp_processor_id(), start, stop);
	}
	result = jdr_finalize(&jdr_inst); /* the resulted json resides in the user-buffer */
    return result.len;
}
EXPORT_SYMBOL(nvmesh_memmgr_metrics_json_serialize);

struct memmgr_metrics_range {
		struct nvmesh_memmgr_metrics *start;
		struct nvmesh_memmgr_metrics *stop;
	};

static void __memmgr_metrics_clear_cpu(void *info)
{
	struct nvmesh_memmgr_metric_counters *curr_counters;
	struct memmgr_metrics_range *range = info;
	struct nvmesh_memmgr_metrics *curr_mgr;
	int cpu = get_cpu();

	for (curr_mgr = range->start; curr_mgr < range->stop; curr_mgr++) {
		curr_counters = per_cpu_ptr(curr_mgr->counters, cpu);
		/* don't touch the gauges, as they are updated by the consumer module */
		nvmesh_metric_clear(curr_counters->max_allocated);
		nvmesh_metric_clear(curr_counters->failures);
		nvmesh_metric_clear(curr_counters->allocation_distribution);
	}

	put_cpu();
}

void nvmesh_memmgr_metrics_clear(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	struct memmgr_metrics_range range = {.start = start, .stop = stop};

	on_each_cpu(__memmgr_metrics_clear_cpu, &range, 1 /* wait */);
}
EXPORT_SYMBOL(nvmesh_memmgr_metrics_clear);

#ifndef __KERNEL__
int nvmesh_memmgr_metrics_dump_to_file(const char *filename, struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	size_t buf_size = 1024 * 1024;
	size_t result_size;
	int rv;

	struct charvec buffer = {.base = malloc(buf_size), .len = buf_size};
	if (!buffer.base) {
		rv = -ENOMEM;
		goto out;
	}

	result_size = nvmesh_memmgr_metrics_json_serialize(buffer, true /* dump_all_cpus */, false /* dump_each_cpu_separately */, start, stop);
	if (result_size >= buf_size) { // buffer overflow
		rv = -1;
		goto out;
	}

	rv = nvmeib_write_file(filename, buffer.base, result_size);
out:
	if (buffer.base)
		free(buffer.base);
	return rv;
}
#endif

int nvmesh_memmgr_metrics_verify_idle(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	struct nvmesh_memmgr_metric_counters merged;
	struct nvmesh_memmgr_metrics* curr = NULL;
	int rv = 0;

	for (curr = start; curr < stop; ++curr) {
		merged = nvmesh_memmgr_metrics_merge_cpus(curr);
#if defined(BLKDEV_SIMULATOR) || defined(NVMESH_SIMULATOR)
		BUG_ON(merged.active_allocations.counter);
		BUG_ON(merged.allocated.counter);
#endif
		rv |= (merged.active_allocations.counter != 0);
		rv |= (merged.allocated.counter != 0);
	}
	return rv;
}

int nvmesh_memmgr_metrics_alloc_pcpu(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	struct nvmesh_memmgr_metrics* curr = NULL;
	int rv = 0;

	for (curr = start; curr < stop; ++curr) {
		WARN(curr->counters, "counters already allocated");
		curr->counters = nvmeib_public_alloc_percpu_zeroed(struct nvmesh_memmgr_metric_counters);
		if (!curr->counters) {
			WARN(1, "Failed to alloc percpu counters");
			rv = -ENOMEM;
			/* Cleanup any previously allocated counters */
			nvmesh_memmgr_metrics_free_pcpu(start, curr);
			break;
		}
	}
	return rv;
}
EXPORT_SYMBOL(nvmesh_memmgr_metrics_alloc_pcpu);

void nvmesh_memmgr_metrics_free_pcpu(struct nvmesh_memmgr_metrics *start, struct nvmesh_memmgr_metrics *stop)
{
	struct nvmesh_memmgr_metrics* curr = NULL;

	for (curr = start; curr < stop; ++curr) {
		if (curr->counters) {
			nvmeib_public_free_percpu(curr->counters);
			curr->counters = NULL;
		}
	}
}
EXPORT_SYMBOL(nvmesh_memmgr_metrics_free_pcpu);
