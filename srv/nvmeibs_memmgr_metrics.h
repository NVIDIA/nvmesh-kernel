#ifndef NVMEIBS_MEMMGR_METRICS_H_INCLUDED
#define NVMEIBS_MEMMGR_METRICS_H_INCLUDED

#include "nvmeib_memmgr_metrics.h"

#define NVMEIBS_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, "nvmeibs_memmgr_metrics", "module=nvmeibs;name="#name";" labels)

extern struct nvmesh_memmgr_metrics __start_nvmeibs_memmgr_metrics[];
extern struct nvmesh_memmgr_metrics __stop_nvmeibs_memmgr_metrics[];

ssize_t nvmeibs_memmgr_metrics_info(void *_ctx, char *buffer, size_t len);

#endif // NVMEIBS_MEMMGR_METRICS_H_INCLUDED 