/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_persist.c — TPV allocator persistence: flat-L1 tree in CDV_extent[0].
 *
 * CDV_extent[0] (at CDV byte offset A) stores a flat array of tpv_tree_entry
 * structs indexed by virtual extent index V.  Each non-null entry records
 * the data CDV_extent and slot that hold V's data:
 *
 *   entry.extent_index = CDV_extent index (data_idx)
 *   entry.debug_meta   = slot within that CDV_extent
 *   phys_offset        = A + data_idx×E + slot×T
 *
 * Max addressable V = E / sizeof(tpv_tree_entry) − 1.  For E = 64 MiB this
 * gives ~4 Mi entries, covering 256 GiB of virtual space at T = 64 KiB.
 *
 * Entry points:
 *   nvmeibc_tpv_load_state()  — attach: read CDV_extent[0], populate xarray
 *                                 and free-slot pool from on-disk tree.
 *   nvmeibc_tpv_flush_state() — write current xarray to CDV_extent[0]
 *                                 (full snapshot; unmapped V → null entry).
 *   nvmeibc_tpv_persist_work_fn()  — deferred background flush.
 *   nvmeibc_tpv_install_data_extent() — no-op; TOMA records ownership.
 *
 * Crash-consistency model:
 *   • CDV_extent ownership (cdv_extent_md) is written by TOMA before the
 *     client receives CDV_ALLOC_OK, so ownership survives client crashes.
 *   • Virtual-extent mappings are persisted by flush_state.  Unflushed
 *     mappings are lost on crash (standard volatile-write semantics).
 *   • CDV_extents that were allocated (cdv_extent_md shows DATA/tpv_uuid)
 *     but have no tree leaves after a crash are orphaned.  NVCK detects
 *     and reclaims them by cross-referencing cdv_extent_md vs. the tree.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_block.h"		/* KERNEL_SECTOR_SHIFT */

/* ── Forward declarations for synchronous CDV IO ───────────────────────── */

/*
 * Synchronous read/write of arbitrary byte ranges on the CDV.
 * Called only from process context (work functions or attach path).
 * Implemented in nvmeibc_tpv_cdv.c (CDV block-layer integration step).
 */
extern int nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
				      u64 cdv_offset, void *buf, u64 len);

extern int nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				       u64 cdv_offset, const void *buf,
				       u64 len);

/* ── Geometry helpers (mirror allocator definitions) ───────────────────── */

static inline u64 persist_alloc_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return a->allocator_size_gb << 30;		/* A in bytes */
}

static inline u64 persist_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->cdv_extent_size_mb << 20;	/* E in bytes */
}

static inline u64 persist_slot_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->tpv_extent_size_kb << 10;	/* T in bytes */
}

static inline u64 persist_slots_per_extent(const struct nvmeibc_tpv_allocator *a)
{
	return persist_extent_bytes(a) / persist_slot_bytes(a);
}

/* Number of tpv_tree_entry in CDV_extent[0]. */
static inline u64 persist_l1_entries(const struct nvmeibc_tpv_allocator *a)
{
	return persist_extent_bytes(a) / sizeof(struct tpv_tree_entry);
}

/* CDV byte offset of CDV_extent[0] (= A). */
static inline u64 persist_l1_offset(const struct nvmeibc_tpv_allocator *a)
{
	return persist_alloc_bytes(a);
}

/* Reconstruct slot number from phys_offset and CDV_extent index. */
static inline u64 persist_slot_of(const struct nvmeibc_tpv_allocator *a,
				   u64 phys_offset, u64 cdv_extent_index)
{
	u64 base = persist_alloc_bytes(a) +
		   cdv_extent_index * persist_extent_bytes(a);
	return (phys_offset - base) / persist_slot_bytes(a);
}

/* Reconstruct phys_offset from CDV_extent index and slot. */
static inline u64 persist_phys_of(const struct nvmeibc_tpv_allocator *a,
				   u64 cdv_extent_index, u64 slot)
{
	return persist_alloc_bytes(a) +
	       cdv_extent_index * persist_extent_bytes(a) +
	       slot * persist_slot_bytes(a);
}

/* ── nvmeibc_tpv_flush_state ───────────────────────────────────────────── */

/*
 * Write a full snapshot of the allocator xarray to CDV_extent[0].
 *
 * Algorithm:
 *   1. Allocate a zeroed buffer of E bytes (= all entries TPV_TREE_NULL).
 *   2. Walk the xarray; for each mapped V, set l1[V].
 *   3. Write the buffer to CDV_extent[0].
 *
 * A full rewrite is correct for both new allocations and frees: any V
 * previously mapped but now erased will have a null entry (the buffer
 * starts zeroed).  The cost is one CDV write of E bytes, acceptable for
 * a deferred background flush.
 *
 * Called from persist_work (work context, may sleep) or synchronously
 * at detach.  The caller must have cleared tpv->dirty under persist_lock
 * before calling; new modifications will re-arm dirty.
 */
int nvmeibc_tpv_flush_state(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct tpv_tree_entry        *l1;
	u64  n_entries = persist_l1_entries(alloc);
	u64  l1_size   = n_entries * sizeof(struct tpv_tree_entry);
	u64  l1_off    = persist_l1_offset(alloc);
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;
	int  rv;

	l1 = vzalloc(l1_size);
	if (!l1)
		return -ENOMEM;

	/*
	 * Snapshot the xarray into the L1 buffer.  xa_for_each is
	 * RCU-protected and lock-free; concurrent alloc/free may cause
	 * the snapshot to be slightly stale, but the next flush (triggered
	 * by the dirty flag) will pick up any missed updates.
	 */
	rcu_read_lock();
	xa_for_each(&alloc->extent_map, idx, entry) {
		u64 V = idx;

		if (unlikely(V >= n_entries)) {
			_NW(tpv_flush_virt_overflow,
			    "TPV: @STR: virt_idx @LLU exceeds L1 capacity @LLU; skipped",
			    tpv->tpv_name, V, n_entries);
			continue;
		}

		l1[V].extent_index = entry->cdv_extent_index;
		l1[V].debug_meta   = persist_slot_of(alloc,
						     entry->phys_offset,
						     entry->cdv_extent_index);
	}
	rcu_read_unlock();

	rv = nvmeibc_tpv_cdv_sync_write(tpv, l1_off, l1, l1_size);
	vfree(l1);

	if (rv)
		_NE(tpv_flush_write_fail, "TPV: @STR: flush_state write failed rv=@INT",
		    tpv->tpv_name, rv);

	return rv;
}
EXPORT_SYMBOL(nvmeibc_tpv_flush_state);

/* ── nvmeibc_tpv_load_state ────────────────────────────────────────────── */

/*
 * Per-CDV_extent tracking used during load to reconstruct the free-slot pool.
 * Allocated on a local list and freed after the pool is built.
 */
struct persist_load_extent {
	u64              extent_index;
	u64              n_slots;
	unsigned long   *used_bm;	/* bitmap: bit s set ↔ slot s is mapped */
	struct list_head node;
};

static struct persist_load_extent *
persist_find_or_create_le(struct list_head *le_list,
			  u64 extent_index, u64 n_slots)
{
	struct persist_load_extent *le;

	list_for_each_entry(le, le_list, node) {
		if (le->extent_index == extent_index)
			return le;
	}

	le = kzalloc(sizeof(*le), GFP_NOIO);
	if (!le)
		return NULL;

	le->extent_index = extent_index;
	le->n_slots      = n_slots;
	le->used_bm      = bitmap_zalloc(n_slots, GFP_NOIO);
	if (!le->used_bm) {
		kfree(le);
		return NULL;
	}
	INIT_LIST_HEAD(&le->node);
	list_add_tail(&le->node, le_list);
	return le;
}

static void persist_free_le_list(struct list_head *le_list)
{
	struct persist_load_extent *le, *tmp;

	list_for_each_entry_safe(le, tmp, le_list, node) {
		list_del(&le->node);
		bitmap_free(le->used_bm);
		kfree(le);
	}
}

/*
 * Read CDV_extent[0], reconstruct the xarray and the CDV_extent ref /
 * free-slot lists.
 *
 * Returns  0   — success, allocator fully populated.
 * Returns >0   — success, but orphan-like inconsistencies detected
 *                (caller should run nvmeibc_tpv_recovery).
 * Returns <0   — hard error, attach should fail.
 *
 * Called at attach time (no concurrent IO, single-threaded).
 * Uses GFP_NOIO: we are a storage driver.
 */
int nvmeibc_tpv_load_state(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct tpv_tree_entry        *l1;
	u64  n_entries   = persist_l1_entries(alloc);
	u64  l1_size     = n_entries * sizeof(struct tpv_tree_entry);
	u64  l1_off      = persist_l1_offset(alloc);
	u64  n_slots     = persist_slots_per_extent(alloc);
	u64  V;
	u64  loaded      = 0;
	int  rv;
	LIST_HEAD(le_list);		/* persist_load_extent tracking */

	l1 = vzalloc(l1_size);
	if (!l1)
		return -ENOMEM;

	rv = nvmeibc_tpv_cdv_sync_read(tpv, l1_off, l1, l1_size);
	if (rv) {
		_NE(tpv_load_read_fail, "TPV: @STR: load_state read failed rv=@INT",
		    tpv->tpv_name, rv);
		vfree(l1);
		return rv;
	}

	/* ── Phase 1: populate xarray from tree leaves ────────────────────── */
	for (V = 0; V < n_entries; V++) {
		struct nvmeibc_tpv_extent_entry *entry;
		struct persist_load_extent      *le;
		u64 data_idx, slot, phys;

		if (l1[V].extent_index == TPV_TREE_NULL)
			continue;

		data_idx = l1[V].extent_index;
		slot     = l1[V].debug_meta;

		/* CDV_extent[0] is the L1 table itself; data extents start at 1. */
		if (unlikely(data_idx == 0)) {
			_NW(tpv_load_data_idx_zero,
			    "TPV: @STR: V=@LLU has data_idx=0 (L1 root); corrupt entry skipped",
			    tpv->tpv_name, V);
			continue;
		}

		if (unlikely(slot >= n_slots)) {
			_NW(tpv_load_slot_overflow,
			    "TPV: @STR: V=@LLU slot=@LLU >= n_slots=@LLU; skipped",
			    tpv->tpv_name, V, slot, n_slots);
			continue;
		}

		phys = persist_phys_of(alloc, data_idx, slot);

		entry = kzalloc(sizeof(*entry), GFP_NOIO);
		if (!entry) {
			rv = -ENOMEM;
			goto out_free;
		}
		entry->phys_offset      = phys;
		entry->cdv_extent_index = data_idx;

		rv = xa_err(xa_store(&alloc->extent_map, V, entry, GFP_NOIO));
		if (rv) {
			kfree(entry);
			goto out_free;
		}
		loaded++;

		/* Track per-CDV_extent slot usage. */
		le = persist_find_or_create_le(&le_list, data_idx, n_slots);
		if (!le) {
			rv = -ENOMEM;
			goto out_free;
		}
		set_bit(slot, le->used_bm);
	}

	vfree(l1);
	l1 = NULL;

	/*
	 * ── Phase 2: build cdv_extent_list and free_tpv_extents ─────────
	 *
	 * No locking needed: load_state runs at attach time before IO gates
	 * open and before work structs are scheduled — single-threaded.
	 */
	{
		struct persist_load_extent *le;

		list_for_each_entry(le, &le_list, node) {
			struct nvmeibc_cdv_extent_ref *ref;
			u64 s;

			ref = kzalloc(sizeof(*ref), GFP_NOIO);
			if (!ref) {
				rv = -ENOMEM;
				goto out_free;
			}
			ref->extent_index    = le->extent_index;
			ref->allocated_count = bitmap_weight(le->used_bm,
							     le->n_slots);
			INIT_LIST_HEAD(&ref->node);
			list_add_tail(&ref->node, &alloc->cdv_extent_list);
			alloc->cdv_extents_count++;

			/* Add unmapped slots to free pool. */
			for (s = 0; s < le->n_slots; s++) {
				struct nvmeibc_tpv_free_slot *fs;

				if (test_bit(s, le->used_bm))
					continue;

				fs = kzalloc(sizeof(*fs), GFP_NOIO);
				if (!fs) {
					rv = -ENOMEM;
					goto out_free;
				}
				fs->phys_offset      = persist_phys_of(alloc,
							le->extent_index, s);
				fs->cdv_extent_index = le->extent_index;
				INIT_LIST_HEAD(&fs->node);
				list_add_tail(&fs->node,
					      &alloc->free_tpv_extents);
				alloc->free_tpv_extent_count++;
			}
		}
	}

	persist_free_le_list(&le_list);

	_NI(tpv_load_done,
	    "TPV: @STR: loaded @LLU mapped extents across @LLU CDV_extents (@LLU free slots)",
	    tpv->tpv_name, loaded, alloc->cdv_extents_count,
	    alloc->free_tpv_extent_count);

	/*
	 * Return 1 if the tree contained any CDV_extents so that
	 * nvmeibc_tpv_attach() will invoke nvmeibc_tpv_recovery() to
	 * cross-check the TOMA extent list for orphaned CDV_extents.
	 * A fresh volume (no prior allocations) returns 0 — no recovery
	 * needed.
	 */
	return alloc->cdv_extents_count > 0 ? 1 : 0;

out_free:
	persist_free_le_list(&le_list);
	vfree(l1);
	return rv;
}
EXPORT_SYMBOL(nvmeibc_tpv_load_state);

/* ── nvmeibc_tpv_persist_work_fn ───────────────────────────────────────── */

/*
 * Deferred background flush scheduled by alloc_extent / free_extent when
 * they set tpv->dirty.  Runs in process context (work queue, may sleep).
 *
 * Clears the dirty flag before flushing so that modifications arriving
 * during the flush re-arm the flag and schedule a follow-up flush.
 */
void nvmeibc_tpv_persist_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv *tpv = container_of(work, struct nvmeibc_tpv,
					       persist_work);
	int rv;

	if (atomic_read(&tpv->state) == TPV_DETACHING)
		return;

	spin_lock(&tpv->persist_lock);
	if (!tpv->dirty) {
		spin_unlock(&tpv->persist_lock);
		return;
	}
	tpv->dirty = false;
	spin_unlock(&tpv->persist_lock);

	rv = nvmeibc_tpv_flush_state(tpv);
	if (rv)
		_NE(tpv_bg_flush_fail, "TPV: @STR: background flush failed rv=@INT",
		    tpv->tpv_name, rv);
}
EXPORT_SYMBOL(nvmeibc_tpv_persist_work_fn);

/* ── nvmeibc_tpv_install_data_extent ───────────────────────────────────── */

/*
 * Called from tpv_on_cdv_alloc_ok() after TOMA allocates a new data
 * CDV_extent.  In the flat-L1 model, tree leaves are written per
 * virtual extent by flush_state after alloc_extent stores in the xarray.
 * No tree update is needed here because no virtual extent maps to this
 * CDV_extent yet.
 *
 * CDV_extent ownership (cdv_extent_md: type=DATA, tpv_uuid) is already
 * persisted by TOMA as part of the write-before-respond CDV_ALLOC flow.
 *
 * If the client crashes before any slot from this CDV_extent is flushed
 * to the tree, the extent is orphaned (cdv_extent_md says DATA/tpv_uuid,
 * but no tree leaf references it).  NVCK detects and reclaims orphans.
 */
int nvmeibc_tpv_install_data_extent(struct nvmeibc_tpv *tpv,
				    u64 extent_index)
{
	/*
	 * Intentional no-op in the flat-L1 model.  See file header and
	 * comment above for crash-consistency rationale.
	 */
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_install_data_extent);
