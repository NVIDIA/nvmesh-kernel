/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef UTILS_H_INCLUDED
#define UTILS_H_INCLUDED

#include "xkr_incs.h"
#include <linux/dma-mapping.h>
#include <linux/dma-direction.h>

typedef ssize_t proc_chng_cb(void *arg, char *buf, size_t len);
void * proc_create_entry(char *name, struct proc_dir_entry *dir,
	proc_chng_cb *chng, void *arg);
void proc_remove_entry(void *p);

#define NVMEIB_REF_WAIT_RELEASE (5 * HZ)
struct x_ref {
	atomic_t cnt;
	struct completion *comp;
	atomic_t dying;
};

void x_ref_init(struct x_ref *r);

/**
 * nvmeib_ref_get - increment @r->cnt unless its zero.
 *
 * Returns non-zero if @r->cnt was non-zero, and zero
 * otherwise.
 */
int __must_check x_ref_get(struct x_ref *r);

/**
 * nvmeib_ref_put - decrement @r->cnt. If new value is 0,
 * complete @r's completion item.
 *
 * Returns the new value of @r->cnt.
 */
int x_ref_put(struct x_ref *r);

/**
 * nvmeib_ref_release_start - inc @r->dying
 *
 * Returns 0 if this is the first call of this function for @r,
 * in which case caller should (may) call release-wait API.
 * Otherwise, return -1.
 *
 * This API lets the caller to first block new users of
 * @r, trigger the release of exiting ones and then call
 * release-wait API.
 *
 * This API allows having multiple contexts triggering the
 * release of but only one waiting (blocking on) till @r is
 * released.
 */
int x_ref_release_start(struct x_ref *r);

/**
 * nvmeib_ref_release_wait - wait on @r->comp, for @r->cnt to be
 * 0.
 *
 */
void x_ref_release_wait(struct x_ref *r);

/**
 * nvmeib_ref_read - return @r->cnt.
 *
 */
static inline int x_ref_read(struct x_ref *r)
{
	return atomic_read(&r->cnt);
}

/**
 * nvmeib_ref_read - return @r->cnt.
 *
 */
static inline bool x_ref_is_dying(struct x_ref *r)
{
	return !!atomic_read(&r->dying);
}

struct sockaddr_storage;
char * x_tss(struct sockaddr_storage *a, char buf[], int len);
bool x_cmp_addr(struct sockaddr_storage *a, struct sockaddr_storage *b);
u64 x_get_guid(void);
union service_id;
char * x_tsid(union service_id *sid, char buf[], int len);

struct device;
struct sg_table;
static inline void __dma_sync_sgtable_for_cpu(struct device *dev,
		struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_sync_sg_for_cpu(dev, sgt->sgl, sgt->orig_nents, dir);
}

static inline void __dma_sync_sgtable_for_device(struct device *dev,
		struct sg_table *sgt, enum dma_data_direction dir)
{
	dma_sync_sg_for_device(dev, sgt->sgl, sgt->orig_nents, dir);
}

#endif
