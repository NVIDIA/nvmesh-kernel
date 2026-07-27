#ifndef NVMEIB_METRICS_JDR_H_INCLUDED
#define NVMEIB_METRICS_JDR_H_INCLUDED

#include "nvmeib_metrics.h"
#include "nvmeib_jdr.h"

/* this structure is used to output metrics to json format via nvmesh_metric_visit(..) */

struct nvmeib_jdr_write_closure {
	struct nvmesh_metrics_closure base;
	struct jdr *writer;
};

XDS_NONNULL(1)
struct nvmeib_jdr_write_closure nvmeib_jdr_write_closure_create(struct jdr *writer);

XDS_NONNULL(2)
ssize_t nvmeib_jdr_serialize_meta_metrics(void *dummy, char *buffer, size_t len);


#endif
