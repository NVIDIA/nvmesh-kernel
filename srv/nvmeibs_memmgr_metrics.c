#include "nvmeibs_memmgr_metrics.h"
#include "nvmeib_memmgr_metrics.h"
#include "nvmeib_jdr.h"

ssize_t nvmeibs_memmgr_metrics_info(void *_ctx, char *buffer, size_t len)
{
	bool const dump_all_cpus = true;
	bool const dump_each_cpu_separately = false;
	struct charvec const jdr_buffer = {.base = buffer, .len = len};
	ssize_t const count = nvmesh_memmgr_metrics_json_serialize(jdr_buffer, dump_all_cpus,  dump_each_cpu_separately,
		__start_nvmeibs_memmgr_metrics, __stop_nvmeibs_memmgr_metrics);
	(void)(_ctx);
	return count;
} 