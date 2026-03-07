#include "clnt/nvmeibc_wq_metrics.h"

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
