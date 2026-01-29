/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/nvmeib.h"
#include "common/nvmeib_memmgr_metrics.h"
#include <sys/utsname.h>
#ifdef __KERNEL__
	#error "This file cannot be used in kernel, only in user space"
#endif

void *nvmeib_alloc(struct nvmeib_alloc_info *ai, u32 size, struct nvmesh_memmgr_metrics *mem_audit) {
	void *addr = NULL;
	struct page *pg;
	int i, order = get_order(size);

	/* Allocate the array to store the pages */
	ai->pages = kmalloc(sizeof(*ai->pages), GFP_ATOMIC);
	if (!ai->pages)
		goto out;

	/* Allocate the pages */
	addr = (void *)__get_free_pages(GFP_ATOMIC, order);
	if (!addr) {
		kfree(ai->pages);
		goto out;
	}

	/* Fill in the pages array */
	ai->n = 1 << order;
	for (i = 0, pg = virt_to_page(addr); i < ai->n; i++, pg++)
		ai->pages[i] = pg;

out:
	if (addr && mem_audit) {
		nvmesh_memmgr_metric_on_alloc_update(mem_audit, ksize(ai->pages) + ai->n * PAGE_SIZE, true /* success */);
	}
	return addr;
}

void nvmeib_release(struct nvmeib_alloc_info *ai, void *vaddr, struct nvmesh_memmgr_metrics *mem_audit) {
	BUG_ON(page_address(ai->pages[0]) != vaddr);
	if (ai && mem_audit) {
		nvmesh_memmgr_metric_on_free_update(mem_audit, ksize(ai->pages) + ai->n * PAGE_SIZE);
	}
	free_pages((unsigned long)vaddr, ilog2(ai->n));
	kfree(ai->pages);
}

const char *nvmeib_get_utsname_nodename(void) {
	static char nodename[80 /* __NEW_UTS_LEN + 1 */ ] = {0};
	if (!nodename[0]) {		/* Init on first use */
		#if defined(BLKDEV_SIMULATOR)
			snprintf(nodename, sizeof(nodename), "%s", utsname()->nodename);
		#else
			struct utsname uts;
			if (uname(&uts) < 0)
				return NULL;
			strlcpy(nodename, uts.nodename, sizeof(nodename));
		#endif
	}
	return &(nodename[0]);
}
