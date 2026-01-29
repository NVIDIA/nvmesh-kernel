/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_ERROR_TAGS_H_INCLUDED
#define NVMEIB_ERROR_TAGS_H_INCLUDED

#include "kr_incs.h"
#include "nvmeib_jdr.h"
#include "nvmeib_metrics.h"
#include "nvmeib_macro_utils.h"

//Important: pay attention to update nvmesh_error_tags_json_serialize function, if you modify the metrics set

struct nvmesh_error_tag {
	char const * const varname;
	char const * const filename;
	char const * const function;
	struct nvmesh_metric_monotonic_counter __percpu *counters;
};

static inline void nvmesh_error_tag_update(struct nvmesh_error_tag *err_tag, int rv)
{
	if (unlikely(rv)){
		struct nvmesh_metric_monotonic_counter *self = per_cpu_ptr(err_tag->counters, get_cpu());
		nvmesh_metric_update_ptr(self, +1);
		put_cpu();
	}
}

#define NVMESH_DEFINE_ERROR_TAG(name, section) 	\
	static DEFINE_PER_CPU(struct nvmesh_metric_monotonic_counter, name##_counters) = {0}; 			\
	static struct nvmesh_error_tag name##_err_tag NVMESH_USED NVMESH_SECTION(section) NVMESH_ALIGNED(8) = { \
		.varname = #name,		\
		.filename = __FILE_NAME__, 	\
		.function = __FUNCTION__, 	\
		.counters = nvmesh_get_percpu_variable_address(struct nvmesh_metric_monotonic_counter, name##_counters)	\
	};					\
	static struct nvmesh_error_tag* name = &(name##_err_tag);

void nvmesh_error_tag_visit(struct nvmesh_error_tag* self, struct nvmesh_metrics_closure* closure, bool visit_all_cpus);
struct nvmesh_metric_monotonic_counter nvmesh_error_tag_merge_cpus(struct nvmesh_error_tag const* src);
size_t nvmesh_error_tags_json_serialize(struct charvec buffer, bool dump_all_cpus, struct nvmesh_error_tag *start, struct nvmesh_error_tag *stop);
#ifndef __KERNEL__
int nvmesh_error_tags_dump_to_file(const char *filename, struct nvmesh_error_tag *start, struct nvmesh_error_tag *stop);
#endif
#endif //NVMEIB_ERROR_TAGS_H_INCLUDED
