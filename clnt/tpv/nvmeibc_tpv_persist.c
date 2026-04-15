/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_persist.c — TPV allocator persistence: per-TPV L1/L2 tree.
 *
 * Each TPV stores its own mapping tree in a dedicated "tree extent" — one
 * of the CDV_extents allocated to this TPV.  Slot 0 of the tree extent
 * holds the L1 table (with a tpv_l1_header for identification); slots 1+
 * hold L2 tables allocated on demand.
 *
 * L1 entries point to L2 tables within the tree extent.  L2 leaf entries
 * record per-virtual-extent mappings (cdv_extent_index + slot).
 * Address translation (2-level):
 *
 *   L1_idx = V / N_L2;  L2_idx = V % N_L2
 *   data_idx = L2[L2_idx].extent_index  (0 = unmapped)
 *   slot     = L2[L2_idx].debug_meta
 *   phys_offset = A + data_idx × E + slot × T
 *
 * Entry points:
 *   nvmeibc_tpv_load_state()  — attach: CDV_LIST_EXTENTS → find tree extent
 *                                 → read L1/L2 → populate xarray.
 *   nvmeibc_tpv_flush_state() — write current xarray into L1/L2 tree on CDV.
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

extern int nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
				      u64 cdv_offset, void *buf, u64 len);

extern int nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				       u64 cdv_offset, const void *buf,
				       u64 len);

/* ── Forward declaration for CDV_LIST_EXTENTS IB admin message ────────── */

extern int nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
					     const char *toma_id,
					     const char *tpv_uuid,
					     u64 **out_indices,
					     u64 *out_count);

/* ── Geometry helpers ──────────────────────────────────────────────────── */

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

/* Number of L1 entries = (T - header) / 16. */
static inline u64 persist_n_l1(const struct nvmeibc_tpv_allocator *a)
{
	return (persist_slot_bytes(a) - sizeof(struct tpv_l1_header)) /
	       sizeof(struct tpv_tree_entry);
}

/* Number of L2 entries per table = T / 16. */
static inline u64 persist_n_l2(const struct nvmeibc_tpv_allocator *a)
{
	return persist_slot_bytes(a) / sizeof(struct tpv_tree_entry);
}

/* CDV byte offset of slot @slot within CDV_extent @extent_index. */
static inline u64 persist_tree_slot_offset(const struct nvmeibc_tpv_allocator *a,
					   u64 extent_index, u64 slot)
{
	return persist_alloc_bytes(a) +
	       extent_index * persist_extent_bytes(a) +
	       slot * persist_slot_bytes(a);
}

/* Reconstruct phys_offset from CDV_extent index and slot. */
static inline u64 persist_phys_of(const struct nvmeibc_tpv_allocator *a,
				   u64 cdv_extent_index, u64 slot)
{
	return persist_alloc_bytes(a) +
	       cdv_extent_index * persist_extent_bytes(a) +
	       slot * persist_slot_bytes(a);
}

/* Reconstruct slot number from phys_offset and CDV_extent index. */
static inline u64 persist_slot_of(const struct nvmeibc_tpv_allocator *a,
				   u64 phys_offset, u64 cdv_extent_index)
{
	u64 base = persist_alloc_bytes(a) +
		   cdv_extent_index * persist_extent_bytes(a);
	return (phys_offset - base) / persist_slot_bytes(a);
}

/* ── nvmeibc_tpv_flush_state ───────────────────────────────────────────── */

/*
 * Write a full snapshot of the allocator xarray into the per-TPV L1/L2 tree.
 *
 * Algorithm:
 *   1. Allocate zeroed L1 buffer (T bytes).  Fill header.
 *   2. Walk the xarray grouped by L1_idx.  For each L1_idx with mappings:
 *      a. Ensure an L2 slot is allocated in the tree extent.
 *      b. Build L2 table from group's entries, write to CDV.
 *      c. Set L1 entry pointing to L2 slot.
 *   3. Write L1 (header + entries) to tree extent slot 0.
 *
 * Called from persist_work (work context, may sleep) or synchronously
 * at detach.
 */
int nvmeibc_tpv_flush_state(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64  T       = persist_slot_bytes(alloc);
	u64  N_L1    = persist_n_l1(alloc);
	u64  N_L2    = persist_n_l2(alloc);
	u64  tree_ei = alloc->tree_extent_index;
	struct tpv_l1_header *hdr;
	struct tpv_tree_entry *l1_entries;	/* entries portion of L1 buffer */
	struct tpv_tree_entry *l2 = NULL;	/* reusable L2 buffer */
	void *l1_buf = NULL;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;
	u64  prev_l1_idx = (u64)-1;
	int  rv = 0;

	if (tree_ei == 0) {
		_NW(tpv_flush_no_tree,
		    "TPV: @STR: flush_state: no tree extent; nothing to flush",
		    tpv->tpv_name);
		return 0;
	}

	/* Allocate L1 buffer (T bytes, zeroed). */
	l1_buf = vzalloc(T);
	if (!l1_buf)
		return -ENOMEM;

	hdr = l1_buf;
	l1_entries = (struct tpv_tree_entry *)((u8 *)l1_buf + sizeof(*hdr));

	/* Fill L1 header. */
	hdr->magic              = TPV_L1_MAGIC;
	hdr->version            = TPV_L1_VERSION;
	memcpy(hdr->tpv_uuid, tpv->tpv_uuid,
	       min_t(size_t, sizeof(hdr->tpv_uuid), sizeof(tpv->tpv_uuid)));
	hdr->tree_extent_index  = tree_ei;
	/* n_l2_slots_used is written after the loop below. */

	/* Allocate reusable L2 buffer (T bytes). */
	l2 = vzalloc(T);
	if (!l2) {
		rv = -ENOMEM;
		goto out;
	}

	/*
	 * Walk the xarray in V-sorted order (guaranteed by xa_for_each).
	 * L2 entries are per-virtual-extent: L1_idx = V / N_L2, L2_idx = V % N_L2.
	 * Group entries by L1_idx.  When L1_idx changes, flush the previous
	 * L2 table to CDV and start a fresh one.
	 */
	rcu_read_lock();
	xa_for_each(&alloc->extent_map, idx, entry) {
		u64 V       = idx;
		u64 l1_idx  = V / N_L2;
		u64 l2_idx  = V % N_L2;
		u64 data_idx = entry->cdv_extent_index;
		u64 slot_in_ext = persist_slot_of(alloc, entry->phys_offset,
						  data_idx);

		if (unlikely(l1_idx >= N_L1)) {
			_NW(tpv_flush_l1_overflow,
			    "TPV: @STR: V=@LLU L1_idx=@LLU >= N_L1=@LLU; skipped",
			    tpv->tpv_name, V, l1_idx, N_L1);
			continue;
		}

		if (l1_idx != prev_l1_idx) {
			/*
			 * Flush the previous L2 table (if any) before starting
			 * the new one.  The first iteration (prev == -1) skips.
			 */
			if (prev_l1_idx != (u64)-1) {
				void *slot_p;
				u64 l2_slot;

				rcu_read_unlock();

				/* Look up or allocate L2 slot for prev_l1_idx. */
				slot_p = xa_load(&alloc->l1_to_l2_slot, prev_l1_idx);
				if (!slot_p) {
					if (unlikely(alloc->tree_l2_next_slot >=
						     persist_slots_per_extent(alloc))) {
						_NE(tpv_flush_tree_full,
						    "TPV: @STR: tree extent L2 slots exhausted",
						    tpv->tpv_name);
						rv = -ENOSPC;
						goto out;
					}
					l2_slot = alloc->tree_l2_next_slot++;
					alloc->n_l2_slots_used++;
					xa_store(&alloc->l1_to_l2_slot,
						 prev_l1_idx,
						 xa_mk_value(l2_slot),
						 GFP_NOIO);
				} else {
					l2_slot = xa_to_value(slot_p);
				}

				/* Write L2 to CDV. */
				rv = nvmeibc_tpv_cdv_sync_write(tpv,
					persist_tree_slot_offset(alloc, tree_ei, l2_slot),
					l2, T);
				if (rv) {
					_NE(tpv_flush_l2_write_fail,
					    "TPV: @STR: L2 write for L1_idx=@LLU failed rv=@INT",
					    tpv->tpv_name, prev_l1_idx, rv);
					goto out;
				}

				/* Set L1 entry for prev_l1_idx. */
				l1_entries[prev_l1_idx].extent_index = tree_ei;
				l1_entries[prev_l1_idx].debug_meta   = l2_slot;

				rcu_read_lock();
			}

			/* Zero the L2 buffer for the new L1_idx. */
			memset(l2, 0, T);
			prev_l1_idx = l1_idx;
		}

		/* Set L2 leaf entry: cdv_extent_index + slot within it. */
		l2[l2_idx].extent_index = data_idx;
		l2[l2_idx].debug_meta   = slot_in_ext;
	}
	rcu_read_unlock();

	/* Flush the last L2 table. */
	if (prev_l1_idx != (u64)-1) {
		void *slot_p;
		u64 l2_slot;

		slot_p = xa_load(&alloc->l1_to_l2_slot, prev_l1_idx);
		if (!slot_p) {
			if (unlikely(alloc->tree_l2_next_slot >=
				     persist_slots_per_extent(alloc))) {
				_NE(tpv_flush_tree_full2,
				    "TPV: @STR: tree extent L2 slots exhausted (last)",
				    tpv->tpv_name);
				rv = -ENOSPC;
				goto out;
			}
			l2_slot = alloc->tree_l2_next_slot++;
			alloc->n_l2_slots_used++;
			xa_store(&alloc->l1_to_l2_slot,
				 prev_l1_idx,
				 xa_mk_value(l2_slot),
				 GFP_NOIO);
		} else {
			l2_slot = xa_to_value(slot_p);
		}

		rv = nvmeibc_tpv_cdv_sync_write(tpv,
			persist_tree_slot_offset(alloc, tree_ei, l2_slot),
			l2, T);
		if (rv) {
			_NE(tpv_flush_l2_write_fail2,
			    "TPV: @STR: L2 write for L1_idx=@LLU failed rv=@INT",
			    tpv->tpv_name, prev_l1_idx, rv);
			goto out;
		}

		l1_entries[prev_l1_idx].extent_index = tree_ei;
		l1_entries[prev_l1_idx].debug_meta   = l2_slot;
	}

	/* Also write L1 entries for L1_idxs that have L2 slots but no current
	 * mappings (all entries freed since last flush).  The L2 slot still
	 * exists; L1 must still point to it so load_state can read the (now
	 * all-null) L2 table and not lose the slot assignment.  Walk l1_to_l2_slot
	 * for any entries not already set above.
	 */
	{
		unsigned long li;
		void *slot_p;

		xa_for_each(&alloc->l1_to_l2_slot, li, slot_p) {
			u64 l2_slot = xa_to_value(slot_p);

			if (li >= N_L1)
				continue;
			if (l1_entries[li].extent_index != TPV_TREE_NULL)
				continue;	/* already set in the loop above */

			/*
			 * This L2 table has no mapped entries.  Write a zeroed L2
			 * table so load_state sees all-null leaves.
			 */
			memset(l2, 0, T);
			rv = nvmeibc_tpv_cdv_sync_write(tpv,
				persist_tree_slot_offset(alloc, tree_ei, l2_slot),
				l2, T);
			if (rv) {
				_NE(tpv_flush_l2_write_empty,
				    "TPV: @STR: empty L2 write for L1_idx=@LLU failed rv=@INT",
				    tpv->tpv_name, (u64)li, rv);
				goto out;
			}
			l1_entries[li].extent_index = tree_ei;
			l1_entries[li].debug_meta   = l2_slot;
		}
	}

	/* Finalize header and write L1 to CDV. */
	hdr->n_l2_slots_used = alloc->n_l2_slots_used;

	rv = nvmeibc_tpv_cdv_sync_write(tpv,
		persist_tree_slot_offset(alloc, tree_ei, 0),
		l1_buf, T);
	if (rv)
		_NE(tpv_flush_l1_write_fail,
		    "TPV: @STR: L1 write failed rv=@INT",
		    tpv->tpv_name, rv);

out:
	vfree(l2);
	vfree(l1_buf);
	return rv;
}
EXPORT_SYMBOL(nvmeibc_tpv_flush_state);

/* ── nvmeibc_tpv_load_state ────────────────────────────────────────────── */

/*
 * Per-CDV_extent tracking used during load to reconstruct the free-slot pool.
 */
struct persist_load_extent {
	u64              extent_index;
	u64              n_slots;
	unsigned long   *used_bm;
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
 * Read the per-TPV L1/L2 tree, reconstruct the xarray and the
 * CDV_extent ref / free-slot lists.
 *
 * Steps:
 *   1. CDV_LIST_EXTENTS → get all CDV_extents owned by this TPV.
 *   2. Scan for the tree extent (L1 magic + matching tpv_uuid).
 *   3. Read L1; for each non-null L1 entry, read L2 and populate xarray.
 *   4. Build cdv_extent_list and free_tpv_extents for data extents.
 *   5. Store the TOMA extent list for recovery to reuse.
 *
 * Returns 0 on success, negative errno on hard error.
 * Called at attach time (single-threaded, no concurrent IO).
 */
int nvmeibc_tpv_load_state(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64  T       = persist_slot_bytes(alloc);
	u64  N_L1    = persist_n_l1(alloc);
	u64  N_L2    = persist_n_l2(alloc);
	u64  n_slots = persist_slots_per_extent(alloc);
	char toma_id[NVMEIB_HOST_NAME_LEN];
	u64 *toma_indices = NULL;
	u64  toma_count   = 0;
	void *l1_buf      = NULL;
	struct tpv_l1_header  *hdr;
	struct tpv_tree_entry *l1_entries;
	struct tpv_tree_entry *l2 = NULL;
	u64  tree_ei = 0;
	u64  loaded  = 0;
	u64  i;
	int  rv;
	unsigned long flags;
	LIST_HEAD(le_list);

	/* ── 1. Snapshot TOMA identity ────────────────────────────────── */
	spin_lock_irqsave(&tpv->allocator_id_lock, flags);
	strncpy(toma_id, tpv->allocator_toma_id, sizeof(toma_id) - 1);
	toma_id[sizeof(toma_id) - 1] = '\0';
	spin_unlock_irqrestore(&tpv->allocator_id_lock, flags);

	if (toma_id[0] == '\0') {
		/*
		 * Allocator TOMA not yet known — can't query CDV_LIST_EXTENTS.
		 * Return -EAGAIN so load_state_work_fn retries in 1 second.
		 */
		_NW(tpv_load_no_toma,
		    "TPV: @STR: no allocator TOMA ID; deferring load_state",
		    tpv->tpv_name);
		return -EAGAIN;
	}

	/* ── 2. CDV_LIST_EXTENTS ──────────────────────────────────────── */
	rv = nvmeibc_ib_admin_cdv_list_extents(tpv->cdv_vol, toma_id,
					       tpv->tpv_uuid,
					       &toma_indices, &toma_count);
	if (rv == -ENOTSUPP) {
		/*
		 * CDV transport stub (test mode) — no extents.  Treat as
		 * fresh TPV: empty allocator, load_state succeeds.
		 */
		_NI(tpv_load_no_cdv_bl,
		    "TPV: @STR: CDV_LIST_EXTENTS not available; starting empty",
		    tpv->tpv_name);
		return 0;
	}
	if (rv) {
		_NE(tpv_load_list_fail,
		    "TPV: @STR: CDV_LIST_EXTENTS failed rv=@INT",
		    tpv->tpv_name, rv);
		return rv;
	}

	if (toma_count == 0) {
		_NI(tpv_load_fresh,
		    "TPV: @STR: TOMA reports 0 extents; fresh TPV",
		    tpv->tpv_name);
		vfree(toma_indices);
		return 0;
	}

	/* ── 3. Find tree extent (scan TOMA list for L1 magic) ────────── */
	{
		struct tpv_l1_header probe;

		for (i = 0; i < toma_count; i++) {
			u64 off = persist_tree_slot_offset(alloc,
							   toma_indices[i], 0);

			rv = nvmeibc_tpv_cdv_sync_read(tpv, off, &probe,
						       sizeof(probe));
			if (rv)
				continue;

			if (probe.magic == TPV_L1_MAGIC &&
			    memcmp(probe.tpv_uuid, tpv->tpv_uuid,
				   min_t(size_t, sizeof(probe.tpv_uuid),
					 sizeof(tpv->tpv_uuid))) == 0) {
				tree_ei = toma_indices[i];
				break;
			}
		}
	}

	if (tree_ei == 0) {
		/*
		 * No tree extent found.  This happens for a fresh TPV whose
		 * extents were allocated but tree was never written, or after
		 * an upgrade from the old flat-L1 format.  TOMA reports the
		 * extents; recovery will adopt them as orphans.
		 */
		_NW(tpv_load_no_tree,
		    "TPV: @STR: no tree extent found among @LLU TOMA extents; empty allocator",
		    tpv->tpv_name, toma_count);
		goto store_toma_list;
	}

	/* ── 4. Read L1 table ─────────────────────────────────────────── */
	l1_buf = vzalloc(T);
	if (!l1_buf) {
		rv = -ENOMEM;
		goto out_free;
	}

	rv = nvmeibc_tpv_cdv_sync_read(tpv,
		persist_tree_slot_offset(alloc, tree_ei, 0),
		l1_buf, T);
	if (rv) {
		_NE(tpv_load_l1_read_fail,
		    "TPV: @STR: L1 read from tree extent[@LLU] failed rv=@INT",
		    tpv->tpv_name, tree_ei, rv);
		goto out_free;
	}

	hdr = l1_buf;
	l1_entries = (struct tpv_tree_entry *)((u8 *)l1_buf + sizeof(*hdr));

	/* Populate tree extent tracking. */
	alloc->tree_extent_index = tree_ei;
	alloc->n_l2_slots_used   = hdr->n_l2_slots_used;
	alloc->tree_l2_next_slot = hdr->n_l2_slots_used + 1;

	/* Add tree extent to cdv_extent_list as all-reserved. */
	{
		struct nvmeibc_cdv_extent_ref *tree_ref;

		tree_ref = kzalloc(sizeof(*tree_ref), GFP_NOIO);
		if (!tree_ref) {
			rv = -ENOMEM;
			goto out_free;
		}
		tree_ref->extent_index    = tree_ei;
		tree_ref->allocated_count = n_slots;	/* all reserved for tree */
		INIT_LIST_HEAD(&tree_ref->node);
		list_add_tail(&tree_ref->node, &alloc->cdv_extent_list);
		alloc->cdv_extents_count++;
	}

	/* ── 5. Walk L1, read L2 tables, populate xarray ──────────────── */
	l2 = vzalloc(T);
	if (!l2) {
		rv = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < N_L1; i++) {
		u64 l2_extent_idx, l2_slot;
		u64 j;

		if (l1_entries[i].extent_index == TPV_TREE_NULL)
			continue;

		l2_extent_idx = l1_entries[i].extent_index;
		l2_slot       = l1_entries[i].debug_meta;

		/* Record L1→L2 slot mapping. */
		xa_store(&alloc->l1_to_l2_slot, i,
			 xa_mk_value(l2_slot), GFP_NOIO);

		/* Read L2 table from CDV. */
		rv = nvmeibc_tpv_cdv_sync_read(tpv,
			persist_tree_slot_offset(alloc, l2_extent_idx, l2_slot),
			l2, T);
		if (rv) {
			_NE(tpv_load_l2_read_fail,
			    "TPV: @STR: L2 read L1_idx=@LLU slot=@LLU failed rv=@INT",
			    tpv->tpv_name, i, l2_slot, rv);
			goto out_free;
		}

		/* Walk L2 entries (per-virtual-extent leaves). */
		for (j = 0; j < N_L2; j++) {
			u64 data_idx = l2[j].extent_index;
			u64 slot_in  = l2[j].debug_meta;
			u64 V, phys;
			struct nvmeibc_tpv_extent_entry *ee;
			struct persist_load_extent *le;

			if (data_idx == TPV_TREE_NULL)
				continue;

			V    = i * N_L2 + j;
			phys = persist_phys_of(alloc, data_idx, slot_in);

			ee = kzalloc(sizeof(*ee), GFP_NOIO);
			if (!ee) {
				rv = -ENOMEM;
				goto out_free;
			}
			ee->phys_offset      = phys;
			ee->cdv_extent_index = data_idx;

			rv = xa_err(xa_store(&alloc->extent_map, V,
					     ee, GFP_NOIO));
			if (rv) {
				kfree(ee);
				goto out_free;
			}
			loaded++;

			/* Track per-CDV_extent slot usage. */
			le = persist_find_or_create_le(&le_list,
						      data_idx,
						      n_slots);
			if (!le) {
				rv = -ENOMEM;
				goto out_free;
			}
			set_bit(slot_in, le->used_bm);
		}
	}

	vfree(l2);
	l2 = NULL;
	vfree(l1_buf);
	l1_buf = NULL;

	/* ── 6. Build cdv_extent_list and free_tpv_extents for data extents ── */
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

			_NT(tpv_load_cdv_ext,
			    "TPV: @STR: load_state CDV_extent[@LLU] allocated=@LLU free=@LLU",
			    tpv->tpv_name, le->extent_index,
			    ref->allocated_count,
			    le->n_slots - ref->allocated_count);

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
	    "TPV: @STR: loaded @LLU mapped extents across @LLU CDV_extents (@LLU free slots) tree_extent=@LLU",
	    tpv->tpv_name, loaded, alloc->cdv_extents_count,
	    alloc->free_tpv_extent_count, tree_ei);

store_toma_list:
	/* ── 7. Store TOMA list for recovery ──────────────────────────── */
	alloc->toma_extent_list  = toma_indices;
	alloc->toma_extent_count = toma_count;
	return 0;

out_free:
	persist_free_le_list(&le_list);
	vfree(l2);
	vfree(l1_buf);
	vfree(toma_indices);
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
	if (rv) {
		_NE(tpv_bg_flush_fail, "TPV: @STR: background flush failed rv=@INT",
		    tpv->tpv_name, rv);
		spin_lock(&tpv->persist_lock);
		tpv->dirty = true;
		spin_unlock(&tpv->persist_lock);

		if (tpv->sync_flush) {
			struct bio_list  failed;
			struct bio      *bio;
			unsigned long    flags;

			bio_list_init(&failed);
			spin_lock_irqsave(&tpv->pending_bio_lock, flags);
			bio_list_merge(&failed, &tpv->pending_l1_flush_bios);
			bio_list_init(&tpv->pending_l1_flush_bios);
			spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

			while ((bio = bio_list_pop(&failed)) != NULL)
				bio_endio(bio, -EIO);
		}
	} else if (tpv->sync_flush) {
		nvmeibc_tpv_forward_l1_flush_bios(tpv);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_persist_work_fn);

/* ── nvmeibc_tpv_load_state_work_fn ───────────────────────────────────── */

/*
 * Background worker: load allocator state from the per-TPV tree extent,
 * run recovery, then open the IO gates by setting state_loaded.
 */
#define TPV_LOAD_STATE_RETRY_DELAY	HZ	/* 1 second */

void nvmeibc_tpv_load_state_work_fn(struct work_struct *work)
{
	struct nvmeibc_tpv *tpv = container_of(work, struct nvmeibc_tpv,
					       load_state_work.work);
	unsigned long flags;
	int rv;

	if (atomic_read(&tpv->state) == TPV_DETACHING)
		return;

	rv = nvmeibc_tpv_load_state(tpv);
	if (rv) {
		_NW(tpv_load_state_retry,
		    "TPV: @STR: load_state failed rv=@INT; retrying in 1s",
		    tpv->tpv_name, rv);
		schedule_delayed_work(&tpv->load_state_work,
				      TPV_LOAD_STATE_RETRY_DELAY);
		return;
	}

	nvmeibc_tpv_recovery(tpv);

	spin_lock_irqsave(&tpv->pending_bio_lock, flags);
	tpv->state_loaded = true;
	spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

	/*
	 * Transition from the attach timeout (30 s) to the normal-operation
	 * timeout (effectively infinite, or the io_max_retry_secs module param
	 * shared with regular volumes).  This ensures bios parked after state
	 * load (CDV extent pool exhaustion) are not subject to the short
	 * attach timeout.
	 */
	{
		extern unsigned nvmeibc_io_max_retry_secs;
		unsigned long normal_timeout =
			(nvmeibc_io_max_retry_secs ? :
			 (unsigned)TPV_IO_TIMEOUT_NORMAL) * (unsigned long)HZ;

		tpv->max_retry_jiffies = normal_timeout;
	}

	_NI(tpv_state_loaded, "TPV: @STR: state loaded; draining pending bios",
	    tpv->tpv_name);

	nvmeibc_tpv_retry_pending_bios(tpv);

	if (tpv->allocator.free_tpv_extent_count == 0) {
		_NI(tpv_pool_empty_after_load,
		    "TPV: @STR: pool empty after load; scheduling CDV alloc",
		    tpv->tpv_name);
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_load_state_work_fn);

/* ── nvmeibc_tpv_install_data_extent ───────────────────────────────────── */

/*
 * Called from tpv_on_cdv_alloc_ok() after TOMA allocates a new data
 * CDV_extent.  L2 leaves are written lazily by flush_state after
 * alloc_extent stores in the xarray.  No tree update is needed here.
 *
 * CDV_extent ownership (cdv_extent_md: type=DATA, tpv_uuid) is already
 * persisted by TOMA as part of the write-before-respond CDV_ALLOC flow.
 */
int nvmeibc_tpv_install_data_extent(struct nvmeibc_tpv *tpv,
				    u64 extent_index)
{
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_install_data_extent);
