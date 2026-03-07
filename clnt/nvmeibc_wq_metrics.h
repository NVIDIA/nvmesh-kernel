#ifndef NVMEIBC_WQ_METRICS_H
#define NVMEIBC_WQ_METRICS_H

#include "common/nvmeib_wq_metrics.h"

/**
 * NVMEIBC_WQ_METRIC() - declare a client-module work-queue metric.
 * @var_name:      name of the pointer variable created for the metric.
 * @metric_labels: metric labels string, e.g. "reason=throttle".
 *
 * Wrapper around NVMESH_DEFINE_WQ_METRIC() that places the metric into
 * the .nvmeibc_wq_metrics linker section (kernel) or nvmeibc_wq_metrics
 * section (userspace simulator). The section bounds are used at module
 * init/cleanup to allocate/free per-CPU counters and at procfs read time
 * to serialize all metrics.
 */
#define NVMEIBC_WQ_METRIC(var_name, metric_labels) \
	NVMESH_DEFINE_WQ_METRIC(var_name, "nvmeibc_wq_metrics", "module=nvmeibc;"metric_labels)

/*
 * Linker-generated section bounds for client wq metrics.
 * Populated by the linker script (clnt/nvmeibc.lds).
 * Used to iterate all NVMEIBC_WQ_METRIC instances at init, cleanup,
 * and serialization time.
 */
extern struct nvmeib_wq_metrics __start_nvmeibc_wq_metrics[];
extern struct nvmeib_wq_metrics __stop_nvmeibc_wq_metrics[];

/**
 * nvmeibc_wq_metrics_info() - procfs read handler for client wq metrics.
 * @_ctx:         unused context (required by procfs callback signature).
 * @buffer:       destination buffer for the JSON output.
 * @len:          size of @buffer in bytes.
 * @dump_per_cpu: false for aggregated output, true for per-CPU breakdown.
 *
 * Serializes all client work-queue metrics in the
 * [__start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics) range
 * as JDR JSON into the provided buffer.
 *
 * Return: number of bytes written into @buffer.
 */
ssize_t nvmeibc_wq_metrics_info(void *_ctx, char *buffer, size_t len, bool dump_per_cpu);

#endif /* NVMEIBC_WQ_METRICS_H */
