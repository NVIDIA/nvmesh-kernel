#ifndef NVMEIBC_MEMMGR_METRICS_H_INCLUDED
#define NVMEIBC_MEMMGR_METRICS_H_INCLUDED

#include "nvmeib_memmgr_metrics.h"

#if defined(__KERNEL__)
	#define NVMEIBC_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, ".nvmeibc_memmgr_metrics", "module=nvmeibc;name="#name";" labels)
#else
	#define NVMEIBC_MEMMGR_METRIC(name, labels) NVMESH_DEFINE_MEMMGR_METRIC(name, "nvmeibc_memmgr_metrics", "module=nvmeibc;name="#name";" labels)
#endif


/* the diff above is a single "." dot in the section name
 * without dot - if the section name is a valid identifier, ld will generate the symbols below by itself for user space apps
 * with dot - is the only way to work with the Linux kernel module loader. The symbols below are generated
 * during the link time via "nvmeibc.lds" (ld script) */

extern struct nvmesh_memmgr_metrics __start_nvmeibc_memmgr_metrics[];
extern struct nvmesh_memmgr_metrics __stop_nvmeibc_memmgr_metrics[];

ssize_t nvmeibc_memmgr_metrics_info(void *_ctx, char *buffer, size_t len);

#endif // NVMEIBC_MEMMGR_METRICS_H_INCLUDED
