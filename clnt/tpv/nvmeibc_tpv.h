/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_TPV_H
#define NVMEIBC_TPV_H

/*
 * Thin-Provisioned Volume (TPV) data structures.
 *
 * A TPV is a virtual volume that rides on a Carrier Direct Volume (CDV).
 * The CDV provides the physical storage; the TPV presents a sparse virtual
 * address space to one exclusive client.  The mapping from virtual TPV_extents
 * to physical locations inside the CDV is managed by the client-local
 * TPV.allocator (this file) and persisted in an L1/L2/L3 tree stored in
 * CDV_extent[0].
 */

#include <linux/xarray.h>		/* struct xarray, xa_store/xa_load */
#include "common/nvmeib.h"		/* NVMEIBC_BD_UUID_LEN, NVMEIB_HOST_NAME_LEN */

/* Forward declarations — full definitions live outside this header. */
struct nvmeibc_volume;
struct nvmeibc_block_device;

/* ── Volume class discriminator ────────────────────────────────────────── */

enum nvmeibc_volume_class {
	NVC_REGULAR = 0,		/* thick-provisioned, default */
	NVC_CDV     = 1,		/* Carrier Direct Volume */
	NVC_TPV     = 2,		/* Thin-Provisioned Volume */
};

/* ── TPV.allocator state ───────────────────────────────────────────────── */

/*
 * A single mapping entry: virtual_extent_index -> physical byte offset in CDV.
 * phys_offset == 0 means unmapped.  CDV offset 0 is inside the allocator area
 * and is never a valid TPV_extent location, so 0 is a safe sentinel.
 */
struct nvmeibc_tpv_extent_entry {
	u64 phys_offset;
};

/*
 * Tracks a single data CDV_extent allocated from the CDV.allocator (TOMA).
 * One CDV_extent holds n_slots = (cdv_extent_size / tpv_extent_size) TPV_extents.
 */
struct nvmeibc_cdv_extent_ref {
	u64              extent_index;		/* data CDV_extent index i */
	u64              allocated_count;	/* TPV_extents in use within this CDV_extent */
	struct list_head node;
};

/*
 * Sparse map: xarray keyed by virtual extent index -> nvmeibc_tpv_extent_entry*.
 * Manages the client-local allocation of TPV_extents within allocated CDV_extents.
 */
struct nvmeibc_tpv_allocator {
	struct xarray    extent_map;		/* V -> nvmeibc_tpv_extent_entry* */
	spinlock_t       lock;

	u32              tpv_extent_size_kb;
	u64              virtual_extents_total;

	struct list_head cdv_extent_list;	/* nvmeibc_cdv_extent_ref entries */
	u64              cdv_extents_count;

	struct list_head free_tpv_extents;	/* available physical TPV_extent slots */
	u64              free_tpv_extent_count;

	u64              low_watermark;		/* schedule CDV alloc when count drops below */
						/* default: 50 MB / tpv_extent_size */
};

/* ── TPV state enum ────────────────────────────────────────────────────── */

enum nvmeibc_tpv_state {
	TPV_ATTACHING  = 0,
	TPV_ATTACHED   = 1,
	TPV_DETACHING  = 2,
};

/* ── Per-TPV instance ──────────────────────────────────────────────────── */

struct nvmeibc_tpv {
	struct nvmeibc_volume        *cdv_vol;		/* parent CDV volume pointer */
	struct nvmeibc_tpv_allocator  allocator;
	struct nvmeibc_block_device  *block_dev;	/* virtual gendisk for this TPV */

	char                          tpv_uuid[NVMEIBC_BD_UUID_LEN];
	u64                           virtual_size;	/* bytes */
	atomic_t                      state;		/* enum nvmeibc_tpv_state */

	/*
	 * CDV.allocator identity — learned from CDV topology at attach time,
	 * updated via topology push when allocator TOMA changes.
	 */
	char                          allocator_toma_id[NVMEIB_HOST_NAME_LEN];
	u64                           allocator_generation;
	spinlock_t                    allocator_id_lock;

	/* Background CDV_extent allocation from TOMA via ADMIN channel. */
	struct work_struct            cdv_alloc_work;
	atomic_t                      cdv_alloc_pending;

	/* Deferred persistence of allocator state to CDV_extent[0]. */
	struct work_struct            persist_work;
	spinlock_t                    persist_lock;
	bool                          dirty;
};

/* ── L1/L2/L3 tree on-disk entry format (CDV_extent[0]) ───────────────── */

/*
 * Every entry at every level (L1, L2, L2a, L3) is 16 bytes.
 * extent_index == TPV_TREE_NULL means the slot is empty / not present.
 * CDV_extent[0] is the permanent L1 root and is never a valid child target,
 * so 0 is safe as the null sentinel.
 */
struct tpv_tree_entry {
	u64 extent_index;		/* CDV_extent index of child table or data extent */
	u64 debug_meta;			/* sequence number, extent_type hint, reserved */
};

#define TPV_TREE_NULL  0ULL

/* ── Public API (implemented in nvmeibc_tpv.c) ────────────────────────── */

int  nvmeibc_tpv_attach(struct nvmeibc_volume *cdv,
			const char *tpv_uuid,
			u64 virtual_size_bytes,
			u32 tpv_extent_size_kb);

void nvmeibc_tpv_detach(struct nvmeibc_tpv *tpv);

/* ── Allocator API (implemented in nvmeibc_tpv_allocator.c) ───────────── */

int  nvmeibc_tpv_alloc_extent(struct nvmeibc_tpv *tpv, u64 virt_idx,
			      struct nvmeibc_tpv_extent_entry **out);

int  nvmeibc_tpv_free_extent(struct nvmeibc_tpv *tpv, u64 virt_idx);

/* ── Persistence API (implemented in nvmeibc_tpv_persist.c) ───────────── */

int  nvmeibc_tpv_load_state(struct nvmeibc_tpv *tpv);
int  nvmeibc_tpv_flush_state(struct nvmeibc_tpv *tpv);

/* ── Recovery API (implemented in nvmeibc_tpv_recovery.c) ─────────────── */

int  nvmeibc_tpv_recovery(struct nvmeibc_tpv *tpv);

#endif /* NVMEIBC_TPV_H */
