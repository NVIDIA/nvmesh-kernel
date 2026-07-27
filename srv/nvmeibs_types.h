#ifndef NVMEIBS_TYPES_H
#define NVMEIBS_TYPES_H

#include "nvmeib_types.h"
#include "nvmeibs_srv_toma_messages.h"

#define NVMEIBS_CLIENT_UID_BASE 0 /* using 1 and above */


struct nvmeibs_protocol_version {
	u64 version;
};

struct page_info {
	void *buf;
	dma_addr_t dma;
};

struct page_list_info {
	int n_pages;
	void *vaddr;
	struct page_info *pages;
	dma_addr_t *dma_pages;
	u64 io_addr;
	u32 lkey;
	u32 rkey;
	bool use_fmr;
	void *fmr;
};

#endif

