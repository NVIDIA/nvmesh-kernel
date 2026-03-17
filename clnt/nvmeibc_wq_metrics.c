/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "clnt/nvmeibc_wq_metrics.h"
#include "clnt/nvmeibc_trace.h"

ssize_t nvmeibc_wq_metrics_info(void *_ctx, char *buffer, size_t len, bool dump_per_cpu)
{
	struct charvec const jdr_buffer = { .base = buffer, .len = len };
	(void)_ctx;

	return nvmeib_wq_metrics_json_serialize(
		jdr_buffer,
		true /* dump_all_cpus */,
		dump_per_cpu,
		__start_nvmeibc_wq_metrics,
		__stop_nvmeibc_wq_metrics);
}

static void trace_highres_histogram(struct nvmesh_metrics_closure *base,
			const char *name,
			struct nvmesh_metric_highres_histogram const *const metric,
			struct nvmesh_metric_id const id)
{
	(void)base;
	(void)name;
	NVMEIB_LOG_METRICS("@HIGHRES_HISTOGRAM", _I, tracer_nvmeibc, wq_metrics_trace,
			   id.name, id.labels, (uint32_t)nvmeib_public_tsc_khz(),
			   metric->max, metric->bins);
}

void nvmeibc_wq_metrics_trace_dump(void)
{
	struct nvmesh_metrics_closure tracer = {
		.visit_highres_histogram = trace_highres_histogram,
	};
	struct nvmeib_wq_metrics *curr;

	for (curr = __start_nvmeibc_wq_metrics; curr < __stop_nvmeibc_wq_metrics; curr++) {
		nvmeib_wq_metrics_visit(curr, &tracer, true /* visit_all_cpus */);
	}
}
