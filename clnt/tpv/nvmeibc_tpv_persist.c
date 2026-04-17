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
#include "clnt/nvmeibc_volume.h"	/* struct nvmeibc_volume, cdv_allocator_toma_id */

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

/* Module param: enable L1/L2 ownership sanity checks during load_state. */
extern bool tp_verify_l1_l2_extent_ownership;

/* Module param: retry delay in milliseconds for CDV operations. */
extern unsigned int tpv_cdv_retry_msecs;

/*
 * Bail early from load_state / flush_state when the TPV is being detached.
 * Without this, sync_read/sync_write can block indefinitely waiting for a
 * CDV bio that will never complete (TOMA already down during shutdown).
 */
static inline bool tpv_is_detaching(const struct nvmeibc_tpv *tpv)
{
	return atomic_read(&tpv->state) == TPV_DETACHING;
}

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

/*
 * CDV byte offset of slot @slot within extent @extent_index.
 * Extent indices are 1-based: extent 1 starts at byte offset A.
 */
static inline u64 persist_tree_slot_offset(const struct nvmeibc_tpv_allocator *a,
					   u64 extent_index, u64 slot)
{
	return persist_alloc_bytes(a) +
	       (extent_index - 1) * persist_extent_bytes(a) +
	       slot * persist_slot_bytes(a);
}

/* Reconstruct phys_offset from 1-based CDV extent index and slot. */
static inline u64 persist_phys_of(const struct nvmeibc_tpv_allocator *a,
				   u64 cdv_extent_index, u64 slot)
{
	return persist_alloc_bytes(a) +
	       (cdv_extent_index - 1) * persist_extent_bytes(a) +
	       slot * persist_slot_bytes(a);
}

/* Reconstruct slot number from phys_offset and 1-based CDV extent index. */
static inline u64 persist_slot_of(const struct nvmeibc_tpv_allocator *a,
				   u64 phys_offset, u64 cdv_extent_index)
{
	u64 base = persist_alloc_bytes(a) +
		   (cdv_extent_index - 1) * persist_extent_bytes(a);
	return (phys_offset - base) / persist_slot_bytes(a);
}

/*
 * Decode a raw CDV byte offset into (1-based extent_index, slot-within-extent).
 * Used to convert an in-memory phys_offset (returned by alloc_l2_slot, or
 * stored in the extent_map) into the (extent_index, debug_meta) fields
 * written to an L1 entry.
 */
static inline void persist_decode_phys(const struct nvmeibc_tpv_allocator *a,
				       u64 phys_offset,
				       u64 *extent_index_out,
				       u64 *slot_out)
{
	u64 E   = persist_extent_bytes(a);
	u64 T   = persist_slot_bytes(a);
	u64 off = phys_offset - persist_alloc_bytes(a);

	*extent_index_out = (off / E) + 1;
	*slot_out         = (off % E) / T;
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
/*
 * Resolve the CDV byte offset of the L2 table backing L1 index @l1_idx,
 * allocating a new L2 slot from the free pool if none exists yet.  Updates
 * l1_to_l2_phys and n_l2_tables_used on first allocation.
 *
 * Returns 0 on success (*phys_out set), -EAGAIN when the free pool is
 * empty, -ENOMEM if the xa_store fails.
 */
static int persist_get_or_alloc_l2_phys(struct nvmeibc_tpv *tpv,
					u64 l1_idx, u64 *phys_out)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	void *slot_p;
	u64 phys;
	int rv;

	slot_p = xa_load(&alloc->l1_to_l2_phys, l1_idx);
	if (slot_p) {
		*phys_out = (u64)xa_to_value(slot_p);
		return 0;
	}

	rv = nvmeibc_tpv_alloc_l2_slot(tpv, &phys);
	if (rv)
		return rv;

	rv = xa_err(xa_store(&alloc->l1_to_l2_phys, l1_idx,
			     xa_mk_value((unsigned long)phys), GFP_NOIO));
	if (rv) {
		/*
		 * The L2 slot is reserved on the owning cdv_extent_ref but
		 * not yet recorded in l1_to_l2_phys.  It stays pinned for
		 * the TPV lifetime — effectively a small leak in the unlikely
		 * xa_store failure path.  Returning an error lets the caller
		 * back off; the next flush will try a fresh slot.
		 */
		return rv;
	}

	alloc->n_l2_tables_used++;
	*phys_out = phys;
	return 0;
}

int nvmeibc_tpv_flush_state(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64  T       = persist_slot_bytes(alloc);
	u64  N_L1    = persist_n_l1(alloc);
	u64  N_L2    = persist_n_l2(alloc);
	u64  l1_ei   = alloc->l1_extent_index;
	struct tpv_l1_header *hdr;
	struct tpv_tree_entry *l1_entries;	/* entries portion of L1 buffer */
	struct tpv_tree_entry *l2 = NULL;	/* reusable L2 buffer */
	void *l1_buf = NULL;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;
	u64  prev_l1_idx = (u64)-1;
	int  rv = 0;

	if (tpv_is_detaching(tpv))
		return -ECANCELED;

	if (l1_ei == 0) {
		_NW(tpv_flush_no_l1_ext,
		    "TPV: @STR: flush_state: no L1 extent yet; nothing to flush",
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
	hdr->l1_extent_index    = l1_ei;
	/* n_l2_tables_used is written after the loop below. */

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
				u64 l2_phys;
				u64 l2_ext, l2_slot;

				rcu_read_unlock();

				rv = persist_get_or_alloc_l2_phys(tpv,
					prev_l1_idx, &l2_phys);
				if (rv) {
					_NE(tpv_flush_l2_slot_fail,
					    "TPV: @STR: get_or_alloc L2 slot for L1_idx=@LLU failed rv=@INT",
					    tpv->tpv_name, prev_l1_idx, rv);
					goto out;
				}

				/* Write L2 to CDV. */
				if (tpv_is_detaching(tpv)) {
					rv = -ECANCELED;
					goto out;
				}
				rv = nvmeibc_tpv_cdv_sync_write(tpv,
					l2_phys, l2, T);
				if (rv) {
					_NE(tpv_flush_l2_write_fail,
					    "TPV: @STR: L2 write for L1_idx=@LLU failed rv=@INT",
					    tpv->tpv_name, prev_l1_idx, rv);
					goto out;
				}

				/* Set L1 entry: encode (extent, slot) of L2. */
				persist_decode_phys(alloc, l2_phys,
						    &l2_ext, &l2_slot);
				l1_entries[prev_l1_idx].extent_index = l2_ext;
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
		u64 l2_phys;
		u64 l2_ext, l2_slot;

		rv = persist_get_or_alloc_l2_phys(tpv, prev_l1_idx, &l2_phys);
		if (rv) {
			_NE(tpv_flush_l2_slot_fail2,
			    "TPV: @STR: get_or_alloc L2 slot for L1_idx=@LLU failed rv=@INT (last)",
			    tpv->tpv_name, prev_l1_idx, rv);
			goto out;
		}

		if (tpv_is_detaching(tpv)) {
			rv = -ECANCELED;
			goto out;
		}
		rv = nvmeibc_tpv_cdv_sync_write(tpv, l2_phys, l2, T);
		if (rv) {
			_NE(tpv_flush_l2_write_fail2,
			    "TPV: @STR: L2 write for L1_idx=@LLU failed rv=@INT",
			    tpv->tpv_name, prev_l1_idx, rv);
			goto out;
		}

		persist_decode_phys(alloc, l2_phys, &l2_ext, &l2_slot);
		l1_entries[prev_l1_idx].extent_index = l2_ext;
		l1_entries[prev_l1_idx].debug_meta   = l2_slot;
	}

	/*
	 * Also write L1 entries for L1_idxs that have L2 slots but no current
	 * mappings (all entries freed since last flush).  The L2 slot still
	 * exists; L1 must still point to it so load_state can re-read the
	 * (now all-null) L2 table and not re-allocate a new L2 slot next flush.
	 */
	{
		unsigned long li;
		void *slot_p;

		xa_for_each(&alloc->l1_to_l2_phys, li, slot_p) {
			u64 l2_phys = (u64)xa_to_value(slot_p);
			u64 l2_ext, l2_slot;

			if (li >= N_L1)
				continue;
			if (l1_entries[li].extent_index != TPV_TREE_NULL)
				continue;	/* already set in the loop above */

			if (tpv_is_detaching(tpv)) {
				rv = -ECANCELED;
				goto out;
			}
			memset(l2, 0, T);
			rv = nvmeibc_tpv_cdv_sync_write(tpv, l2_phys, l2, T);
			if (rv) {
				_NE(tpv_flush_l2_write_empty,
				    "TPV: @STR: empty L2 write for L1_idx=@LLU failed rv=@INT",
				    tpv->tpv_name, (u64)li, rv);
				goto out;
			}

			persist_decode_phys(alloc, l2_phys, &l2_ext, &l2_slot);
			l1_entries[li].extent_index = l2_ext;
			l1_entries[li].debug_meta   = l2_slot;
		}
	}

	/* Finalize header and write L1 to CDV. */
	if (tpv_is_detaching(tpv)) {
		rv = -ECANCELED;
		goto out;
	}
	hdr->n_l2_tables_used = alloc->n_l2_tables_used;

	rv = nvmeibc_tpv_cdv_sync_write(tpv,
		persist_tree_slot_offset(alloc, l1_ei, 0),
		l1_buf, T);
	if (rv)
		_NE(tpv_flush_l1_write_fail,
		    "TPV: @STR: L1 write failed rv=@INT",
		    tpv->tpv_name, rv);

	/* Mark all xarray entries as persisted so the IO path can release
	 * parked sync_flush bios.  Only on success — failed flushes must
	 * not let data reach CDV with an unpersisted mapping. */
	if (!rv) {
		struct nvmeibc_tpv_extent_entry *e;
		unsigned long xi;

		rcu_read_lock();
		xa_for_each(&alloc->extent_map, xi, e)
			WRITE_ONCE(e->persisted, true);
		rcu_read_unlock();
	}

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
	unsigned long   *used_bm;	/* bit set = data slot referenced by xarray */
	unsigned long   *l2_bm;		/* bit set = L2 table slot (dynamic placement) */
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
	le->l2_bm        = bitmap_zalloc(n_slots, GFP_NOIO);
	if (!le->l2_bm) {
		bitmap_free(le->used_bm);
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
		bitmap_free(le->l2_bm);
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
	u64  l1_ei   = 0;	/* CDV_extent holding the L1 table */
	u64  loaded  = 0;
	u64  i;
	int  rv;
	unsigned long flags;
	LIST_HEAD(le_list);

	if (tpv_is_detaching(tpv))
		return -ECANCELED;

	/* ── 1. Snapshot TOMA identity ──────────────────────────────────
	 *
	 * The TPV's allocator_toma_id is seeded once from cdv->cdv_allocator_toma_id
	 * at attach/adopt time (see nvmeibc_tpv_attach:764-770 and
	 * nvmeibc_tpv_adopt:493-503).  A CDV_ALLOCATOR_UPDATE push that lands
	 * while the TPV is not yet in nvmeibc_tpv_active_list populates the CDV
	 * cache but does not reach the TPV.  Close that race here by re-reading
	 * the CDV cache whenever the local snapshot is empty.
	 */
	spin_lock_irqsave(&tpv->allocator_id_lock, flags);
	strncpy(toma_id, tpv->allocator_toma_id, sizeof(toma_id) - 1);
	toma_id[sizeof(toma_id) - 1] = '\0';
	spin_unlock_irqrestore(&tpv->allocator_id_lock, flags);

	if (toma_id[0] == '\0' && tpv->cdv_vol) {
		struct nvmeibc_volume *cdv = tpv->cdv_vol;
		unsigned long vflags;
		char   cdv_toma[NVMEIB_HOST_NAME_LEN] = {0};
		u64    cdv_gen = 0;

		spin_lock_irqsave(&cdv->spinlock, vflags);
		strncpy(cdv_toma, cdv->cdv_allocator_toma_id,
			sizeof(cdv_toma) - 1);
		cdv_gen = cdv->cdv_allocator_generation;
		spin_unlock_irqrestore(&cdv->spinlock, vflags);

		if (cdv_toma[0]) {
			nvmeibc_tpv_update_allocator_id(tpv, cdv_toma, cdv_gen);
			strncpy(toma_id, cdv_toma, sizeof(toma_id) - 1);
			toma_id[sizeof(toma_id) - 1] = '\0';
			_NI(tpv_load_toma_from_cdv,
			    "TPV: @STR: picked up allocator toma=@STR gen=@LLU from CDV cache",
			    tpv->tpv_name, cdv_toma, cdv_gen);
		}
	}

	if (toma_id[0] == '\0') {
		/*
		 * Allocator TOMA not yet known on TPV or CDV — can't query
		 * CDV_LIST_EXTENTS.  Return -EAGAIN so load_state_work_fn
		 * retries; the CDV cache is updated on each CDV_ALLOCATOR_UPDATE
		 * push and will be re-read next iteration.
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
		kvfree(toma_indices);
		return 0;
	}

	/* ── 3. Find L1 extent (scan TOMA list for magic in slot 0) ───── */
	{
		/*
		 * Read a full page for the probe — the L1 header is only 64 bytes
		 * but the CDV block device may have a 4096-byte sector size.  A
		 * sub-sector bio is rejected as "wrong IO" by the block layer
		 * and triggers a rider retry storm.
		 */
		void *probe_buf = (void *)__get_free_page(GFP_NOIO);

		if (!probe_buf) {
			rv = -ENOMEM;
			goto out_free;
		}

		for (i = 0; i < toma_count; i++) {
			struct tpv_l1_header *probe;
			u64 off = persist_tree_slot_offset(alloc,
							   toma_indices[i], 0);

			if (tpv_is_detaching(tpv)) {
				rv = -ECANCELED;
				free_page((unsigned long)probe_buf);
				goto out_free;
			}
			rv = nvmeibc_tpv_cdv_sync_read(tpv, off, probe_buf,
							PAGE_SIZE);
			if (rv)
				continue;

			probe = (struct tpv_l1_header *)probe_buf;
			if (probe->magic == TPV_L1_MAGIC &&
			    memcmp(probe->tpv_uuid, tpv->tpv_uuid,
				   min_t(size_t, sizeof(probe->tpv_uuid),
					 sizeof(tpv->tpv_uuid))) == 0) {
				l1_ei = toma_indices[i];
				break;
			}
		}
		free_page((unsigned long)probe_buf);
	}

	if (l1_ei == 0) {
		/*
		 * No L1 extent found.  This happens for a fresh TPV whose
		 * extents were allocated but the L1 was never written.
		 * TOMA reports the extents; recovery will adopt them as orphans.
		 */
		_NW(tpv_load_no_l1_ext,
		    "TPV: @STR: no L1 extent found among @LLU TOMA extents; empty allocator",
		    tpv->tpv_name, toma_count);
		goto store_toma_list;
	}

	/* ── 4. Read L1 table ─────────────────────────────────────────── */
	l1_buf = vzalloc(T);
	if (!l1_buf) {
		rv = -ENOMEM;
		goto out_free;
	}

	if (tpv_is_detaching(tpv)) {
		rv = -ECANCELED;
		goto out_free;
	}
	rv = nvmeibc_tpv_cdv_sync_read(tpv,
		persist_tree_slot_offset(alloc, l1_ei, 0),
		l1_buf, T);
	if (rv) {
		_NE(tpv_load_l1_read_fail,
		    "TPV: @STR: L1 read from L1 extent[@LLU] failed rv=@INT",
		    tpv->tpv_name, l1_ei, rv);
		goto out_free;
	}

	hdr = l1_buf;
	l1_entries = (struct tpv_tree_entry *)((u8 *)l1_buf + sizeof(*hdr));

	/* Populate L1 extent tracking. */
	alloc->l1_extent_index  = l1_ei;
	alloc->n_l2_tables_used = hdr->n_l2_tables_used;

	/* ── 5. Walk L1, read L2 tables, populate xarray ──────────────── */
	l2 = vzalloc(T);
	if (!l2) {
		rv = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < N_L1; i++) {
		u64 l2_extent_idx, l2_slot, l2_phys;
		struct persist_load_extent *l2_le;
		u64 j;

		if (l1_entries[i].extent_index == TPV_TREE_NULL)
			continue;

		l2_extent_idx = l1_entries[i].extent_index;
		l2_slot       = l1_entries[i].debug_meta;

		/*
		 * Under dynamic L2 placement, L1 entries may point at any
		 * TPV-owned CDV_extent.  Sanity check: the referenced extent
		 * must appear in the TOMA-reported list.
		 */
		if (tp_verify_l1_l2_extent_ownership) {
			u64 k;
			bool owned = false;

			for (k = 0; k < toma_count; k++) {
				if (toma_indices[k] == l2_extent_idx) {
					owned = true;
					break;
				}
			}
			if (!owned) {
				_NE(tpv_load_bad_l1,
				    "TPV: @STR: L1[@LLU] references extent @LLU not in TOMA list; skipping",
				    tpv->tpv_name, i, l2_extent_idx);
				continue;
			}
		}

		l2_phys = persist_phys_of(alloc, l2_extent_idx, l2_slot);

		/*
		 * Record L1→L2 location (phys offset).  flush_state will reuse
		 * this slot for the L1_idx until the TPV is deleted.
		 */
		rv = xa_err(xa_store(&alloc->l1_to_l2_phys, i,
			    xa_mk_value((unsigned long)l2_phys), GFP_NOIO));
		if (rv)
			goto out_free;

		/*
		 * Mark the L2 slot in its owning extent so it is excluded
		 * from the free pool when building cdv_extent_list below.
		 */
		l2_le = persist_find_or_create_le(&le_list, l2_extent_idx,
						  n_slots);
		if (!l2_le) {
			rv = -ENOMEM;
			goto out_free;
		}
		set_bit(l2_slot, l2_le->l2_bm);

		/* Read L2 table from CDV. */
		if (tpv_is_detaching(tpv)) {
			rv = -ECANCELED;
			goto out_free;
		}
		rv = nvmeibc_tpv_cdv_sync_read(tpv, l2_phys, l2, T);
		if (rv) {
			_NE(tpv_load_l2_read_fail,
			    "TPV: @STR: L2 read L1_idx=@LLU ext=@LLU slot=@LLU failed rv=@INT",
			    tpv->tpv_name, i, l2_extent_idx, l2_slot, rv);
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

			/* Sanity: L2 leaf must reference an extent that
			 * TOMA says belongs to us. */
			if (tp_verify_l1_l2_extent_ownership) {
				u64 k;
				bool owned = false;

				for (k = 0; k < toma_count; k++) {
					if (toma_indices[k] == data_idx) {
						owned = true;
						break;
					}
				}
				if (!owned) {
					_NE(tpv_load_bad_extent,
					    "TPV: @STR: L2[L1=@LLU,@LLU] references extent @LLU not in TOMA list; skipping",
					    tpv->tpv_name, i, j, data_idx);
					continue;
				}
			}

			V    = i * N_L2 + j;
			phys = persist_phys_of(alloc, data_idx, slot_in);

			ee = kzalloc(sizeof(*ee), GFP_NOIO);
			if (!ee) {
				rv = -ENOMEM;
				goto out_free;
			}
			ee->phys_offset      = phys;
			ee->cdv_extent_index = data_idx;
			ee->persisted        = true;

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

	/* ── 6. Ensure every TOMA-owned extent has an le entry ──────────
	 *
	 * An extent that has no data mappings and no L2 slots would not be
	 * in le_list (it wouldn't have been added by the L1/L2 walk above).
	 * Under change 1, such an extent is still part of the TPV and its
	 * slots must enter free_tpv_extents.  The L1 extent always needs
	 * an le entry too (its slot 0 is reserved even if nothing is mapped).
	 */
	for (i = 0; i < toma_count; i++) {
		if (!persist_find_or_create_le(&le_list, toma_indices[i],
					       n_slots)) {
			rv = -ENOMEM;
			goto out_free;
		}
	}

	/* ── 7. Build cdv_extent_list and free_tpv_extents ───────────── */
	{
		struct persist_load_extent *le;

		list_for_each_entry(le, &le_list, node) {
			struct nvmeibc_cdv_extent_ref *ref;
			u64 s;
			u64 data_cnt = bitmap_weight(le->used_bm, le->n_slots);
			u64 l2_cnt   = bitmap_weight(le->l2_bm,   le->n_slots);
			bool is_l1   = (le->extent_index == l1_ei);

			ref = kzalloc(sizeof(*ref), GFP_NOIO);
			if (!ref) {
				rv = -ENOMEM;
				goto out_free;
			}
			ref->extent_index    = le->extent_index;
			ref->allocated_count = data_cnt + l2_cnt;
			ref->l2_slots        = l2_cnt;
			ref->is_l1_extent    = is_l1;
			INIT_LIST_HEAD(&ref->node);
			list_add_tail(&ref->node, &alloc->cdv_extent_list);
			alloc->cdv_extents_count++;

			_NT(tpv_load_cdv_ext,
			    "TPV: @STR: load_state CDV_extent[@LLU] is_l1=@INT data=@LLU l2=@LLU",
			    tpv->tpv_name, le->extent_index, (int)is_l1,
			    data_cnt, l2_cnt);

			for (s = 0; s < le->n_slots; s++) {
				struct nvmeibc_tpv_free_slot *fs;

				/* Skip slot 0 of the L1 extent (holds L1 table). */
				if (is_l1 && s == 0)
					continue;
				/* Skip data slots (already mapped in xarray). */
				if (test_bit(s, le->used_bm))
					continue;
				/* Skip L2 slots (reserved for L2 tables). */
				if (test_bit(s, le->l2_bm))
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
	    "TPV: @STR: loaded @LLU mapped extents across @LLU CDV_extents (@LLU free slots) l1_extent=@LLU",
	    tpv->tpv_name, loaded, alloc->cdv_extents_count,
	    alloc->free_tpv_extent_count, l1_ei);

store_toma_list:
	/* ── 7. Store TOMA list for recovery ──────────────────────────── */
	alloc->toma_extent_list  = toma_indices;
	alloc->toma_extent_count = toma_count;
	return 0;

out_free:
	persist_free_le_list(&le_list);
	vfree(l2);
	vfree(l1_buf);
	kvfree(toma_indices);
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
		    "TPV: @STR: load_state failed rv=@INT; retrying in @UINT ms",
		    tpv->tpv_name, rv, tpv_cdv_retry_msecs);
		schedule_delayed_work(&tpv->load_state_work,
				      msecs_to_jiffies(tpv_cdv_retry_msecs));
		return;
	}

	nvmeibc_tpv_recovery(tpv);

	spin_lock_irqsave(&tpv->pending_bio_lock, flags);
	tpv->state_loaded = true;
	spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

	/*
	 * Transition from the attach timeout to the normal-operation timeout.
	 * When io_max_retry_secs is 0 (default), fall back to ~infinite —
	 * same as IO_TIME_OUT_NORMAL for regular volumes.  When nonzero, the
	 * configured value is used (same value at all stages).
	 */
	{
		extern unsigned nvmeibc_io_max_retry_secs;

		tpv->max_retry_jiffies =
			(nvmeibc_io_max_retry_secs ? :
			 (unsigned long)(1 << 20)) * (unsigned long)HZ;
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
