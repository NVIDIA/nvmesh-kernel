#include "nvmeibc_memmgr_metrics.h"

bool memmgr_metrics_info_dump_each_cpu_separately = false;
module_param(memmgr_metrics_info_dump_each_cpu_separately, bool, 0644);
MODULE_PARM_DESC(memmgr_metrics_info_dump_each_cpu_separately, "Dump memmgr metrics info per cpu");

ssize_t nvmeibc_memmgr_metrics_info(void *_ctx, char *buffer, size_t len)
{
	bool const dump_all_cpus = true;
	struct charvec const jdr_buffer = {.base = buffer, .len = len};
	ssize_t const count = nvmesh_memmgr_metrics_json_serialize(jdr_buffer, dump_all_cpus, memmgr_metrics_info_dump_each_cpu_separately,
								   __start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	(void)(_ctx);
	return count;
}
