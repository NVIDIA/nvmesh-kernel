#ifndef NVMEIB_NUMA_H
#define NVMEIB_NUMA_H

#include "kr_incs.h"
#include "kr_version.h"
#include "nvmeib_types.h"
#include "ib_incs.h"

struct nvmeib_numa_alloc_diag { /* Diagnostics */
	char dev_name[40];
	struct list_head link;
	u64 fallback;
	u64 alloc[16];
};

extern unsigned nvmeib_numa_alloc_policy;
extern unsigned nvmeib_numa_alloc_granularity;

struct nvmeib_numa_iter {
	int node;
	enum nvmeib_numa_alloc_policy_type policy;
	struct {
		int target;
		int running;
	} gran; /* granularity */
	union {
		nodemask_t nodemask;
	} data;
	struct device *dev;
	struct nvmeib_numa_alloc_diag *diag;
};

ssize_t nvmeib_numa_iter_diag_fill(void *dummy, char *buffer, size_t len);
void nvmeib_numa_iter_init(struct nvmeib_numa_iter *iter,
                           enum nvmeib_numa_alloc_policy_type policy,
                           struct device *dev);
void nvmeib_numa_iter_next_rr(struct nvmeib_numa_iter *iter, int step);
int nvmeib_numa_iter_alloc_n_pages(struct nvmeib_numa_iter *iter, 
				   struct page **pages, int n_pages,
                                   u64 init_val, gfp_t gfp);
void nvmeib_numa_iter_free_n_pages(struct page **pages, int n_pages);
void *nvmeib_numa_iter_dma_alloc(struct nvmeib_numa_iter *iter,
                                 struct ib_device *dev, size_t size,
                                 dma_addr_t *dma_handle, gfp_t flag);
void nvmeib_numa_iter_dma_free(struct ib_device *dev, size_t size,
                               void *cpu_addr, dma_addr_t dma_handle);


int nvmeib_socket_from_numa(int dev_node);
void nvmeib_numa_iter_diag_store_init(void);
void nvmeib_numa_iter_diag_store_destroy(void);

int nvmeib_alloc_pages_respecting_numa_policy(
    struct nvmeib_alloc_n_map *mem, gfp_t gfp);
    
void nvmeib_free_pages_respecting_numa_policy(
    struct nvmeib_alloc_n_map *mem);
#endif /*NVMEIB_NUMA_H*/
