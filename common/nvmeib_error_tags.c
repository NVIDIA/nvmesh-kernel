#include "nvmeib_error_tags.h"
#include "compat/kr_incs_asserts.h"
#include "nvmeib_metrics_jdr.h"

struct nvmesh_metric_monotonic_counter nvmesh_error_tag_merge_cpus(struct nvmesh_error_tag const* src)
{
	int cpu;
	struct nvmesh_metric_monotonic_counter dst_counter = {0};

	for_each_possible_cpu(cpu) {
		struct nvmesh_metric_monotonic_counter const *src_counter = per_cpu_ptr(src->counters, cpu);
		nvmesh_metric_merge(&dst_counter, src_counter);
	}
	return dst_counter;
}

EXPORT_SYMBOL(nvmesh_error_tag_merge_cpus);

void nvmesh_error_tag_visit(struct nvmesh_error_tag* self, struct nvmesh_metrics_closure* closure, bool visit_all_cpus)
{
	char buffer[512];
	struct nvmesh_metric_id const metric_id = {
		.name = "error_tag",
		.labels = buffer
	};

	struct nvmesh_metric_monotonic_counter merged 
		= visit_all_cpus ? nvmesh_error_tag_merge_cpus(self)
						: *per_cpu_ptr(self->counters, smp_processor_id());

	int const rv = snprintf(buffer, sizeof(buffer), "name=%s;file=%s;func=%s;", self->varname, self->filename, self->function);
	BUG_ON(rv < 0 || rv > (int)sizeof(buffer)); //should never happen in production, also because this code is 100% covered by the usage

	nvmesh_metric_visit_ptr(closure, NULL, &merged, metric_id);
}
EXPORT_SYMBOL(nvmesh_error_tag_visit);

size_t nvmesh_error_tags_json_serialize(struct charvec buffer, bool dump_all_cpus, struct nvmesh_error_tag *start, struct nvmesh_error_tag *stop)
{
	struct charvec result = {0};
	struct jdr jdr_inst = jdr_make(buffer);
	struct nvmeib_jdr_write_closure jdr_writer = nvmeib_jdr_write_closure_create(&jdr_inst);
	jdr_write_var(&jdr_inst, version, 1);
	{
		static char const* curr_cpu = "metrics.current";
		static char const* all_cpu = "metrics.all_cpus";
		struct nvmesh_error_tag* curr = NULL;
		jdr_array_scope((&jdr_inst), dump_all_cpus ? all_cpu :  curr_cpu);

		for (curr = start; curr < stop; ++curr) {
			nvmesh_error_tag_visit(curr, &(jdr_writer.base), dump_all_cpus);
		}
	}
	result = jdr_finalize(&jdr_inst); /* the resulted json resides in the user-buffer */
	return result.len;
}

EXPORT_SYMBOL(nvmesh_error_tags_json_serialize);

#ifndef __KERNEL__
int nvmesh_error_tags_dump_to_file(const char *filename, struct nvmesh_error_tag *start, struct nvmesh_error_tag *stop)
{
	size_t buf_size = 1024 * 1024;
	size_t result_size;
	int rv;

	struct charvec buffer = {.base = malloc(buf_size), .len = buf_size};
	if (!buffer.base) {
		rv = -ENOMEM;
		goto out;
	}

	result_size = nvmesh_error_tags_json_serialize(buffer, true /* dump_all_cpus */, start, stop);
	if (result_size >= buf_size) { /* buffer overflow */
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
