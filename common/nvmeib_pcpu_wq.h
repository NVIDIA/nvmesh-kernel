#ifndef NVMEIB_PUBLIC_PCPU_WQ_H
#define NVMEIB_PUBLIC_PCPU_WQ_H	1

#include "kr_incs.h"

struct nvmeib_pcpu_wq;

struct nvmeib_pcpu_wq *nvmeib_pcpu_wq_create(const char *name);

void nvmeib_pcpu_wq_destroy(struct nvmeib_pcpu_wq *pcpu_wq);

bool nvmeib_pcpu_wq_add_work_on_core(struct nvmeib_pcpu_wq *pcpu_wq, 
				     unsigned int cpu, struct workqe_struct *entry);

void nvmeib_pcpu_wq_flush_on_core(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu);

void nvmeib_pcpu_wq_drain_on_core(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu);

bool nvmeib_pcpu_wq_flush_work_on_core(struct nvmeib_pcpu_wq *pcpu_wq,
				       unsigned int cpu, struct workqe_struct *entry);

bool nvmeib_pcpu_wq_cancel_work_on_core(struct nvmeib_pcpu_wq *pcpu_wq,
				       unsigned int cpu, struct workqe_struct *entry);

struct workq_struct *nvmeib_pcpu_wq_get_queue(struct nvmeib_pcpu_wq *pcpu_wq, unsigned int cpu);

#endif /* NVMEIB_PUBLIC_PCPU_WQ_H */
