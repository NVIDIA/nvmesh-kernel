/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_persist.c - TPV allocator persistence: per-TPV L1/L2 tree.
 *
 * Each TPV stores its own mapping tree across the CDV_extents allocated to
 * it.  Slot 0 of the "L1 extent" (the first CDV_extent allocated to this
 * TPV) holds the L1 table (with a tpv_l1_header for identification).  L2
 * tables are placed lazily in any TPV-owned slot by nvmeibc_tpv_flush_state().
 *
 * L1 and L2 entries share the same 8-byte format: a single u64 holding the
 * raw CDV byte offset of the referenced object.  0 means unmapped.
 * Address translation (2-level, TPV_L1_VERSION 2):
 *
 *   L1_idx = V / N_L2;  L2_idx = V % N_L2
 *   phys_offset = L2[L2_idx].cdv_offset   (0 = unmapped)
 *
 * Entry points:
 *   nvmeibc_tpv_load_state()  - attach: CDV_LIST_EXTENTS -> find L1 extent
 *                                 -> read L1/L2 -> populate xarray.
 *   nvmeibc_tpv_flush_state() - write current xarray into L1/L2 tree on CDV.
 *   nvmeibc_tpv_persist_work_fn()  - deferred background flush.
 *   nvmeibc_tpv_install_data_extent() - no-op; TOMA records ownership.
 *
 * Crash-consistency model:
 *   - CDV_extent ownership (cdv_extent_md) is written by TOMA before the
 *     client receives CDV_ALLOC_OK, so ownership survives client crashes.
 *   - Virtual-extent mappings are persisted by flush_state.  Unflushed
 *     mappings are lost on crash (standard volatile-write semantics).
 *   - CDV_extents that were allocated (cdv_extent_md shows DATA/tpv_uuid)
 *     but have no tree leaves after a crash are orphaned.  NVCK detects
 *     and reclaims them by cross-referencing cdv_extent_md vs. the tree.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_block.h"		/* KERNEL_SECTOR_SHIFT */
#include "clnt/nvmeibc_volume.h"	/* struct nvmeibc_volume, cdv_allocator_toma_id */

/* -- Forward declarations for synchronous CDV IO ------------------------- */

extern int nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
				      u64 cdv_offset, void *buf, u64 len);

extern int nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				       u64 cdv_offset, const void *buf,
				       u64 len);

/* -- Forward declaration for CDV_LIST_EXTENTS IB admin message ---------- */

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

/* -- Geometry helpers ---------------------------------------------------- */

static inline u64 persist_alloc_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return a->allocator_size_gib << 30;		/* A in bytes */
}

static inline u64 persist_extent_bytes(const struct nvmeibc_tpv_allocator *a)
{
	return (u64)a->cdv_extent_size_mib << 20;	/* E in bytes */
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

/* Number of L2 entries per table = T / sizeof(tpv_tree_entry) = T / 8. */
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

/*
 * Partial-page flush (S.3.4.3).  Each L1/L2 table is flushed at 4 KB
 * granularity; a per-table bitmap records which pages carry uncommitted
 * changes.  TPV extent sizes are always a multiple of 4 KB (tpvExtentSizeKB
 * is a power of 2 from 64 up to 65536), so every page is exactly 4 KB.
 */
#define PERSIST_PAGE_BYTES	4096ULL

static inline u64 persist_n_pages(const struct nvmeibc_tpv_allocator *a)
{
	return (persist_slot_bytes(a) + PERSIST_PAGE_BYTES - 1) /
	       PERSIST_PAGE_BYTES;
}

/* Bit index of the 4 KB page that holds L2[l2_idx]. */
static inline u64 persist_l2_leaf_page(u64 l2_idx)
{
	return (l2_idx * sizeof(struct tpv_tree_entry)) / PERSIST_PAGE_BYTES;
}

/* Bit index of the 4 KB page that holds L1[l1_idx] (after the header). */
static inline u64 persist_l1_entry_page(u64 l1_idx)
{
	u64 byte_off = sizeof(struct tpv_l1_header) +
		       l1_idx * sizeof(struct tpv_tree_entry);
	return byte_off / PERSIST_PAGE_BYTES;
}

/*
 * Write pages marked in @dirty_pages from @buf to CDV at @base_phys.
 * Adjacent set bits are coalesced into a single contiguous write.  Cleared
 * bits are skipped.  The bitmap is zeroed for the pages successfully
 * written.  Partial failure leaves the bitmap in the "remaining dirty"
 * state so the next flush can retry.
 */
static int persist_write_dirty_pages(struct nvmeibc_tpv *tpv, u64 base_phys,
				     const void *buf, unsigned long *dirty_pages,
				     u64 n_pages)
{
	unsigned long pos = 0;
	int rv = 0;

	while (pos < n_pages) {
		unsigned long start = find_next_bit(dirty_pages, n_pages, pos);
		unsigned long end;

		if (start >= n_pages)
			break;

		end = find_next_zero_bit(dirty_pages, n_pages, start);
		if (end > n_pages)
			end = n_pages;

		rv = nvmeibc_tpv_cdv_sync_write(tpv,
			base_phys + start * PERSIST_PAGE_BYTES,
			(const u8 *)buf + start * PERSIST_PAGE_BYTES,
			(end - start) * PERSIST_PAGE_BYTES);
		if (rv)
			return rv;

		bitmap_clear(dirty_pages, start, end - start);
		pos = end;
	}
	return 0;
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

/* -- nvmeibc_tpv_flush_state --------------------------------------------- */

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
 * Resolve the tpv_l2_ctx for L1 index @l1_idx, allocating a fresh ctx
 * (plus a brand-new L2 slot from the free pool and its dirty-page bitmap)
 * if none exists yet.  Updates l1_to_l2_ctx and n_l2_tables_used.
 *
 * On first allocation the L2 slot on disk is garbage, so the ctx's
 * dirty_pages bitmap is set with every bit - forcing flush_state to write
 * the entire T-byte L2 table once.  Caller is responsible for marking
 * corresponding L1 dirty bits (the L1 entry page and the L1 header page).
 *
 * Returns 0 on success (*ctx_out set), -EAGAIN when the free pool is
 * empty, -ENOMEM on allocation failure.
 */
static int persist_get_or_alloc_l2_ctx(struct nvmeibc_tpv *tpv, u64 l1_idx,
				       struct tpv_l2_ctx **ctx_out)
{
	/* L1/L2 tree bookkeeping lives on the metadata-side allocator in
	 * split mode; nvmeibc_tpv_meta_alloc() returns the data allocator
	 * in single-CDV mode so this helper keeps working unchanged. */
	struct nvmeibc_tpv_allocator *alloc = nvmeibc_tpv_meta_alloc(tpv);
	struct tpv_l2_ctx *ctx;
	u64 n_pages = persist_n_pages(alloc);
	u64 phys;
	int rv;

	ctx = xa_load(&alloc->l1_to_l2_ctx, l1_idx);
	if (ctx) {
		*ctx_out = ctx;
		return 0;
	}

	rv = nvmeibc_tpv_alloc_l2_slot(tpv, &phys);
	if (rv)
		return rv;

	ctx = kzalloc(sizeof(*ctx), GFP_NOIO);
	if (!ctx)
		return -ENOMEM;

	ctx->phys        = phys;
	ctx->dirty_pages = bitmap_zalloc(n_pages, GFP_NOIO);
	if (!ctx->dirty_pages) {
		kfree(ctx);
		return -ENOMEM;
	}
	bitmap_set(ctx->dirty_pages, 0, n_pages);	/* fresh: full write */

	rv = xa_err(xa_store(&alloc->l1_to_l2_ctx, l1_idx, ctx, GFP_NOIO));
	if (rv) {
		bitmap_free(ctx->dirty_pages);
		kfree(ctx);
		return rv;
	}

	alloc->n_l2_tables_used++;
	*ctx_out = ctx;
	return 0;
}

/*
 * nvmeibc_tpv_mark_l2_leaf_dirty - IO-path hook for partial-page flush.
 *
 * Called by alloc_extent and free_extent after the xarray mutation.  If
 * an L2 ctx exists for the owning L1 index, mark the 4 KB page holding
 * that leaf as dirty so the next flush writes only that page.
 *
 * If no ctx exists yet (first-ever leaf under this L1_idx), no marking
 * is required: flush_state will create the ctx with all pages dirty the
 * first time it walks the xarray and hits this L1 index.
 *
 * Safe from any context (set_bit is atomic; xa_load under rcu).
 */
void nvmeibc_tpv_mark_l2_leaf_dirty(struct nvmeibc_tpv *tpv, u64 virt_idx)
{
	struct nvmeibc_tpv_allocator *alloc = nvmeibc_tpv_meta_alloc(tpv);
	u64 N_L2   = persist_n_l2(alloc);
	u64 l1_idx = virt_idx / N_L2;
	u64 l2_idx = virt_idx % N_L2;
	struct tpv_l2_ctx *ctx;

	ctx = xa_load(&alloc->l1_to_l2_ctx, l1_idx);
	if (!ctx || !ctx->dirty_pages)
		return;

	set_bit(persist_l2_leaf_page(l2_idx), ctx->dirty_pages);
}
EXPORT_SYMBOL(nvmeibc_tpv_mark_l2_leaf_dirty);

/*
 * nvmeibc_tpv_mark_l1_full_dirty - called after the L1 extent is first
 * assigned (by tpv_on_cdv_alloc_ok or by recovery orphan promotion).
 * Marks every L1 page dirty so the initial flush writes the header and a
 * fresh all-null entry table to the CDV.
 */
void nvmeibc_tpv_mark_l1_full_dirty(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator *alloc = nvmeibc_tpv_meta_alloc(tpv);
	u64 n_pages = persist_n_pages(alloc);

	if (!alloc->l1_dirty_pages)
		return;
	bitmap_set(alloc->l1_dirty_pages, 0, n_pages);
}
EXPORT_SYMBOL(nvmeibc_tpv_mark_l1_full_dirty);

/*
 * flush_state_write_l2_ctx - write the just-built L2 buffer to CDV at
 * ctx->phys, respecting ctx->dirty_pages (partial-page flush).  The L1
 * entry for @l1_idx is set to ctx->phys after a successful write, and
 * the L1-entry page is marked dirty if its value changed.
 */
static int flush_state_write_l2_ctx(struct nvmeibc_tpv *tpv,
				    u64 l1_idx,
				    struct tpv_l2_ctx *ctx,
				    const void *l2_buf,
				    u64 n_pages,
				    struct tpv_tree_entry *l1_entries)
{
	/* L1 dirty-page bitmap lives on the metadata-side allocator. */
	struct nvmeibc_tpv_allocator *alloc = nvmeibc_tpv_meta_alloc(tpv);
	int rv;

	if (tpv_is_detaching(tpv))
		return -ECANCELED;

	rv = persist_write_dirty_pages(tpv, ctx->phys, l2_buf,
				       ctx->dirty_pages, n_pages);
	if (rv) {
		_NE(tpv_flush_l2_write_fail,
		    "TPV: @STR: L2 write for L1_idx=@LLU failed rv=@INT",
		    tpv->tpv_name, l1_idx, rv);
		return rv;
	}

	if (l1_entries[l1_idx].cdv_offset != ctx->phys) {
		l1_entries[l1_idx].cdv_offset = ctx->phys;
		if (alloc->l1_dirty_pages)
			set_bit(persist_l1_entry_page(l1_idx),
				alloc->l1_dirty_pages);
	}
	return 0;
}

int nvmeibc_tpv_flush_state(struct nvmeibc_tpv *tpv)
{
	/*
	 * The L1/L2 tree lives on the metadata side. Geometry constants (T,
	 * N_L1, N_L2, n_pages) come from the metadata allocator because the
	 * metadata CDV may use a different cdv_extent_size_mib /
	 * tpv_extent_size_kb than the data CDV. In single-CDV mode both
	 * allocators are the same. extent_map iteration (data leaves) still
	 * happens against the data-side allocator - see below.
	 */
	struct nvmeibc_tpv_allocator *alloc      = nvmeibc_tpv_meta_alloc(tpv);
	struct nvmeibc_tpv_allocator *data_alloc = nvmeibc_tpv_data_alloc(tpv);
	u64  T       = persist_slot_bytes(alloc);
	u64  N_L1    = persist_n_l1(alloc);
	u64  N_L2    = persist_n_l2(alloc);
	u64  n_pages = persist_n_pages(alloc);
	u64  l1_ei   = alloc->l1_extent_index;
	struct tpv_l1_header *hdr;
	struct tpv_tree_entry *l1_entries;	/* entries portion of L1 buffer */
	struct tpv_tree_entry *l2 = NULL;	/* reusable L2 buffer */
	void *l1_buf = NULL;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;
	struct tpv_l2_ctx *prev_ctx = NULL;
	u64  prev_l1_idx = (u64)-1;
	u64  prev_n_l2_tables_used;
	int  rv = 0;

	if (tpv_is_detaching(tpv))
		return -ECANCELED;

	if (l1_ei == 0) {
		_NW(tpv_flush_no_l1_ext,
		    "TPV: @STR: flush_state: no L1 extent yet; nothing to flush",
		    tpv->tpv_name);
		return 0;
	}

	prev_n_l2_tables_used = alloc->n_l2_tables_used;

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

	/*
	 * Pre-populate l1_entries from the existing l1_to_l2_ctx map so any
	 * L1 pages we do NOT need to rewrite retain their correct content in
	 * case find_next_bit picks up a dirty page that straddles an L1 entry
	 * we're not touching this flush.
	 */
	{
		unsigned long li;
		struct tpv_l2_ctx *ctx;

		xa_for_each(&alloc->l1_to_l2_ctx, li, ctx) {
			if (li < N_L1 && ctx)
				l1_entries[li].cdv_offset = ctx->phys;
		}
	}

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
	/* Iterate the data-side extent map: leaves point at data CDV offsets. */
	rcu_read_lock();
	xa_for_each(&data_alloc->extent_map, idx, entry) {
		u64 V       = idx;
		u64 l1_idx  = V / N_L2;
		u64 l2_idx  = V % N_L2;

		if (unlikely(l1_idx >= N_L1)) {
			_NW(tpv_flush_l1_overflow,
			    "TPV: @STR: V=@LLU L1_idx=@LLU >= N_L1=@LLU; skipped",
			    tpv->tpv_name, V, l1_idx, N_L1);
			continue;
		}

		if (l1_idx != prev_l1_idx) {
			/*
			 * Transitioning to a new L1_idx: flush the previous L2
			 * table (if any), then resolve/create the new ctx.
			 * Both operations need to sleep (CDV IO, kzalloc), so
			 * we drop RCU for the duration.
			 */
			rcu_read_unlock();

			if (prev_l1_idx != (u64)-1) {
				rv = flush_state_write_l2_ctx(tpv, prev_l1_idx,
					prev_ctx, l2, n_pages, l1_entries);
				if (rv)
					goto out;
			}

			rv = persist_get_or_alloc_l2_ctx(tpv, l1_idx, &prev_ctx);
			if (rv) {
				_NE(tpv_flush_l2_ctx_fail,
				    "TPV: @STR: get_or_alloc L2 ctx for L1_idx=@LLU failed rv=@INT",
				    tpv->tpv_name, l1_idx, rv);
				goto out;
			}

			memset(l2, 0, T);
			prev_l1_idx = l1_idx;
			rcu_read_lock();
		}

		/* L2 leaf entry: raw CDV byte offset of the data slot. */
		l2[l2_idx].cdv_offset = entry->phys_offset;
	}
	rcu_read_unlock();

	/* Flush the last L2 table. */
	if (prev_l1_idx != (u64)-1) {
		rv = flush_state_write_l2_ctx(tpv, prev_l1_idx, prev_ctx,
					      l2, n_pages, l1_entries);
		if (rv)
			goto out;
	}

	/*
	 * Also flush L1 indices that have a ctx but no mapped leaves (all
	 * leaves freed since last flush).  ctx->dirty_pages still carries
	 * the bits set by free_extent - write only those pages.
	 */
	{
		unsigned long li;
		struct tpv_l2_ctx *ctx;

		xa_for_each(&alloc->l1_to_l2_ctx, li, ctx) {
			if (li >= N_L1 || !ctx || !ctx->dirty_pages)
				continue;
			/* Already handled in the main loop? (prev_l1_idx hit). */
			if (bitmap_empty(ctx->dirty_pages, n_pages))
				continue;
			/* If the main loop already rewrote this L2, its dirty
			 * bits were cleared; nothing to do. */
			if (tpv_is_detaching(tpv)) {
				rv = -ECANCELED;
				goto out;
			}
			memset(l2, 0, T);
			rv = flush_state_write_l2_ctx(tpv, li, ctx, l2,
						      n_pages, l1_entries);
			if (rv)
				goto out;
		}
	}

	/* Finalize header; mark header page dirty if n_l2_tables_used changed. */
	if (tpv_is_detaching(tpv)) {
		rv = -ECANCELED;
		goto out;
	}
	hdr->n_l2_tables_used = alloc->n_l2_tables_used;
	if (alloc->n_l2_tables_used != prev_n_l2_tables_used &&
	    alloc->l1_dirty_pages)
		set_bit(0, alloc->l1_dirty_pages);

	if (alloc->l1_dirty_pages) {
		rv = persist_write_dirty_pages(tpv,
			persist_tree_slot_offset(alloc, l1_ei, 0),
			l1_buf, alloc->l1_dirty_pages, n_pages);
	} else {
		/* Fallback: init-time OOM left l1_dirty_pages NULL.  Write the
		 * full L1 slot so correctness is preserved even though write
		 * amplification is back to the pre-S.3.4.3 level. */
		rv = nvmeibc_tpv_cdv_sync_write(tpv,
			persist_tree_slot_offset(alloc, l1_ei, 0),
			l1_buf, T);
	}
	if (rv)
		_NE(tpv_flush_l1_write_fail,
		    "TPV: @STR: L1 write failed rv=@INT",
		    tpv->tpv_name, rv);

	/* Mark all xarray entries as persisted so the IO path can release
	 * parked sync_flush bios.  Only on success - failed flushes must
	 * not let data reach CDV with an unpersisted mapping. */
	if (!rv) {
		struct nvmeibc_tpv_extent_entry *e;
		unsigned long xi;

		rcu_read_lock();
		xa_for_each(&data_alloc->extent_map, xi, e)
			WRITE_ONCE(e->persisted, true);
		(void)xi;
		rcu_read_unlock();
	}

out:
	vfree(l2);
	vfree(l1_buf);
	return rv;
}
EXPORT_SYMBOL(nvmeibc_tpv_flush_state);

/* -- nvmeibc_tpv_load_state ---------------------------------------------- */

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

	if (!list_empty(le_list)) {
		list_for_each_entry(le, le_list, node) {
			if (le->extent_index == extent_index)
				return le;
		}
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
	struct persist_load_extent *le, *next;

	list_for_each_entry_safe(le, next, le_list, node) {
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
 *   1. CDV_LIST_EXTENTS -> get all CDV_extents owned by this TPV.
 *   2. Scan for the tree extent (L1 magic + matching tpv_uuid).
 *   3. Read L1; for each non-null L1 entry, read L2 and populate xarray.
 *   4. Build cdv_extent_list and free_tpv_extents for data extents.
 *   5. Store the TOMA extent list for recovery to reuse.
 *
 * Returns 0 on success, negative errno on hard error.
 * Called at attach time (single-threaded, no concurrent IO).
 */
/*
 * load_state_snapshot_toma_id - pull the TOMA identity for a given allocator
 * side into @toma_id_out (NVMEIB_HOST_NAME_LEN bytes). If the TPV's cached
 * copy is empty, re-reads the CDV's cache and primes the TPV if possible.
 *
 * Returns 0 on success, -EAGAIN if no TOMA identity is available yet (caller
 * should retry via load_state_work_fn).
 */
static int load_state_snapshot_toma_id(struct nvmeibc_tpv *tpv,
					bool is_meta_side,
					char *toma_id_out)
{
	struct nvmeibc_volume *cdv;
	spinlock_t *id_lock;
	const char *id_src;
	unsigned long flags;

	if (is_meta_side) {
		cdv     = tpv->meta_cdv_vol;
		id_lock = &tpv->meta_allocator_id_lock;
		id_src  = tpv->meta_allocator_toma_id;
	} else {
		cdv     = tpv->cdv_vol;
		id_lock = &tpv->allocator_id_lock;
		id_src  = tpv->allocator_toma_id;
	}

	spin_lock_irqsave(id_lock, flags);
	strncpy(toma_id_out, id_src, NVMEIB_HOST_NAME_LEN - 1);
	toma_id_out[NVMEIB_HOST_NAME_LEN - 1] = '\0';
	spin_unlock_irqrestore(id_lock, flags);

	if (toma_id_out[0] == '\0' && cdv) {
		unsigned long vflags;
		char   cdv_toma[NVMEIB_HOST_NAME_LEN] = {0};
		u64    cdv_gen = 0;

		spin_lock_irqsave(&cdv->spinlock, vflags);
		strncpy(cdv_toma, cdv->cdv_allocator_toma_id,
			sizeof(cdv_toma) - 1);
		cdv_gen = cdv->cdv_allocator_generation;
		spin_unlock_irqrestore(&cdv->spinlock, vflags);

		if (cdv_toma[0]) {
			if (is_meta_side)
				nvmeibc_tpv_update_meta_allocator_id(tpv, cdv_toma, cdv_gen);
			else
				nvmeibc_tpv_update_allocator_id(tpv, cdv_toma, cdv_gen);
			strncpy(toma_id_out, cdv_toma, NVMEIB_HOST_NAME_LEN - 1);
			toma_id_out[NVMEIB_HOST_NAME_LEN - 1] = '\0';
			_NI(tpv_load_toma_from_cdv,
			    "TPV: @STR: picked up allocator toma=@STR gen=@LLU from CDV cache (meta=@INT)",
			    tpv->tpv_name, cdv_toma, cdv_gen, (int)is_meta_side);
		}
	}

	if (toma_id_out[0] == '\0') {
		_NW(tpv_load_no_toma,
		    "TPV: @STR: no allocator TOMA ID yet (meta=@INT); deferring load_state",
		    tpv->tpv_name, (int)is_meta_side);
		return -EAGAIN;
	}
	return 0;
}

/*
 * load_state_populate_data_side_from_toma_list - split-mode helper.
 *
 * In split mode the data CDV never hosts L1/L2 - every extent TOMA reports
 * as owned by this TPV is a pure data extent. Walk the list, match each
 * extent against the xarray leaves already installed (so already-used slots
 * are excluded from the free pool), and splice the remaining slots into the
 * data-side free pool.
 *
 * Runs AFTER the L1/L2 walk in the meta side has populated data_alloc's
 * extent_map. Called only when nvmeibc_tpv_is_split(tpv).
 */
static int load_state_populate_data_side(struct nvmeibc_tpv *tpv,
					  const u64 *data_indices,
					  u64 data_count)
{
	struct nvmeibc_tpv_allocator *data_alloc = nvmeibc_tpv_data_alloc(tpv);
	u64  n_slots = persist_slots_per_extent(data_alloc);
	u64  i;
	LIST_HEAD(le_list);
	int  rv = 0;

	for (i = 0; i < data_count; i++) {
		if (!persist_find_or_create_le(&le_list, data_indices[i], n_slots)) {
			rv = -ENOMEM;
			goto out;
		}
	}

	/*
	 * Mark slots that are already referenced by the xarray (data leaves
	 * installed from the L2 walk) so they don't end up on the free list.
	 */
	{
		struct nvmeibc_tpv_extent_entry *ee;
		unsigned long xi;

		rcu_read_lock();
		xa_for_each(&data_alloc->extent_map, xi, ee) {
			u64 data_idx, slot_in;
			struct persist_load_extent *le;

			persist_decode_phys(data_alloc, ee->phys_offset,
					     &data_idx, &slot_in);
			le = persist_find_or_create_le(&le_list, data_idx, n_slots);
			if (le)
				set_bit(slot_in, le->used_bm);
			(void)xi;
		}
		rcu_read_unlock();
	}

	{
		struct persist_load_extent *le;

		list_for_each_entry(le, &le_list, node) {
			struct nvmeibc_cdv_extent_ref *ref;
			u64 s;
			u64 data_cnt = bitmap_weight(le->used_bm, le->n_slots);

			ref = kzalloc(sizeof(*ref), GFP_NOIO);
			if (!ref) { rv = -ENOMEM; goto out; }
			ref->extent_index    = le->extent_index;
			ref->allocated_count = data_cnt;
			ref->l2_slots        = 0;		/* no L2 on data side */
			ref->is_l1_extent    = false;	/* no L1 on data side */
			INIT_LIST_HEAD(&ref->node);
			list_add_tail(&ref->node, &data_alloc->cdv_extent_list);
			data_alloc->cdv_extents_count++;

			for (s = 0; s < le->n_slots; s++) {
				struct nvmeibc_tpv_free_slot *fs;

				if (test_bit(s, le->used_bm))
					continue;
				fs = kzalloc(sizeof(*fs), GFP_NOIO);
				if (!fs) { rv = -ENOMEM; goto out; }
				fs->phys_offset      = persist_phys_of(data_alloc,
							le->extent_index, s);
				fs->cdv_extent_index = le->extent_index;
				INIT_LIST_HEAD(&fs->node);
				list_add_tail(&fs->node, &data_alloc->free_tpv_extents);
				data_alloc->free_tpv_extent_count++;
			}
		}
	}

out:
	persist_free_le_list(&le_list);
	return rv;
}

int nvmeibc_tpv_load_state(struct nvmeibc_tpv *tpv)
{
	/*
	 * In split mode the L1/L2 tree lives on the metadata CDV, so the
	 * tree walk, CDV_LIST_EXTENTS for the L1 scan, and L1/L2 bookkeeping
	 * all use the metadata-side allocator and CDV. Data leaves (pointers
	 * into the data CDV) are stored on the data-side allocator's xarray.
	 * In single-CDV mode nvmeibc_tpv_meta_* return the data side, so the
	 * code below degrades to the pre-split behavior automatically.
	 */
	struct nvmeibc_tpv_allocator *alloc      = nvmeibc_tpv_meta_alloc(tpv);
	struct nvmeibc_tpv_allocator *data_alloc = nvmeibc_tpv_data_alloc(tpv);
	struct nvmeibc_volume        *meta_cdv   = nvmeibc_tpv_meta_cdv(tpv);
	u64  T       = persist_slot_bytes(alloc);
	u64  N_L1    = persist_n_l1(alloc);
	u64  N_L2    = persist_n_l2(alloc);
	u64  n_slots = persist_slots_per_extent(alloc);
	char toma_id[NVMEIB_HOST_NAME_LEN];
	u64 *toma_indices = NULL;
	u64  toma_count   = 0;
	u64 *data_toma_indices = NULL;	/* split-mode data-side list */
	u64  data_toma_count   = 0;
	void *l1_buf      = NULL;
	struct tpv_l1_header  *hdr;
	struct tpv_tree_entry *l1_entries;
	struct tpv_tree_entry *l2 = NULL;
	u64  l1_ei   = 0;	/* CDV_extent holding the L1 table */
	u64  loaded  = 0;
	u64  i;
	int  rv;
	LIST_HEAD(le_list);

	if (tpv_is_detaching(tpv))
		return -ECANCELED;

	/* -- 1. Snapshot TOMA identity for the tree-owning side -------- */
	rv = load_state_snapshot_toma_id(tpv, nvmeibc_tpv_is_split(tpv), toma_id);
	if (rv)
		return rv;

	/* -- 2. CDV_LIST_EXTENTS on the tree-owning CDV --------------- */
	rv = nvmeibc_ib_admin_cdv_list_extents(meta_cdv, toma_id,
					       tpv->tpv_uuid,
					       &toma_indices, &toma_count);
	if (rv == -ENOTSUPP) {
		/*
		 * CDV transport stub (test mode) - no extents.  Treat as
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

	/* -- 2b. Split mode: also fetch the data-CDV extent list up front --
	 *
	 * The L1/L2 walk installs leaves pointing at data-CDV offsets into the
	 * data-side allocator's extent_map. Decoding those offsets requires
	 * the DATA allocator's geometry (cdv_extent_size_mib, allocator_size_gib),
	 * and the ownership sanity check needs the DATA-CDV TOMA list. Fetch
	 * both here so the leaf walk below has what it needs.
	 *
	 * Single-CDV mode: the meta side IS the data side, so this step is a
	 * no-op (toma_indices already covers both L1/L2 and data).
	 */
	if (nvmeibc_tpv_is_split(tpv)) {
		char data_toma[NVMEIB_HOST_NAME_LEN];

		rv = load_state_snapshot_toma_id(tpv, /*is_meta_side=*/false,
						  data_toma);
		if (rv == -EAGAIN) {
			_NW(tpv_load_data_toma_unknown,
			    "TPV: @STR: data-side TOMA not yet known; leaf extent-ownership check will be skipped",
			    tpv->tpv_name);
		} else if (rv == 0) {
			rv = nvmeibc_ib_admin_cdv_list_extents(
				nvmeibc_tpv_data_cdv(tpv), data_toma,
				tpv->tpv_uuid,
				&data_toma_indices, &data_toma_count);
			if (rv == -ENOTSUPP) {
				/* test stub - fresh data side */
				data_toma_indices = NULL;
				data_toma_count   = 0;
			} else if (rv) {
				_NE(tpv_load_data_list_fail,
				    "TPV: @STR: data-side CDV_LIST_EXTENTS failed rv=@INT",
				    tpv->tpv_name, rv);
				kvfree(toma_indices);
				return rv;
			}
		}
	}

	/* -- 3. Find L1 extent (scan TOMA list for magic in slot 0) ----- */
	{
		/*
		 * Read a full page for the probe - the L1 header is only 64 bytes
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

	/* -- 4. Read L1 table ------------------------------------------- */
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

	/* -- 5. Walk L1, read L2 tables, populate xarray ---------------- */
	l2 = vzalloc(T);
	if (!l2) {
		rv = -ENOMEM;
		goto out_free;
	}

	for (i = 0; i < N_L1; i++) {
		u64 l2_extent_idx, l2_slot, l2_phys;
		struct persist_load_extent *l2_le;
		u64 j;

		if (l1_entries[i].cdv_offset == TPV_TREE_NULL)
			continue;

		l2_phys = l1_entries[i].cdv_offset;
		persist_decode_phys(alloc, l2_phys, &l2_extent_idx, &l2_slot);

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

		/*
		 * Record L1->L2 location.  Create a fresh tpv_l2_ctx whose
		 * dirty_pages bitmap is initially zero: the on-disk image we
		 * just read IS the authoritative state, so no pages need
		 * rewriting until alloc/free marks them.
		 */
		{
			struct tpv_l2_ctx *ctx;

			ctx = kzalloc(sizeof(*ctx), GFP_NOIO);
			if (!ctx) {
				rv = -ENOMEM;
				goto out_free;
			}
			ctx->phys        = l2_phys;
			ctx->dirty_pages = bitmap_zalloc(persist_n_pages(alloc),
							 GFP_NOIO);
			if (!ctx->dirty_pages) {
				kfree(ctx);
				rv = -ENOMEM;
				goto out_free;
			}

			rv = xa_err(xa_store(&alloc->l1_to_l2_ctx, i, ctx,
					     GFP_NOIO));
			if (rv) {
				bitmap_free(ctx->dirty_pages);
				kfree(ctx);
				goto out_free;
			}
		}

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

		/* Walk L2 entries (per-virtual-extent leaves).
		 *
		 * L2 leaves point at byte offsets on the DATA CDV. Decoding
		 * them with the meta-side allocator (different E / A) would
		 * yield a wrong extent index and a wrong slot. Use the data-
		 * side allocator for the decode; single-CDV mode's data_alloc
		 * == alloc so this is a no-op there. Likewise the TOMA-
		 * ownership check must use the data-side TOMA list
		 * (data_toma_indices) in split mode; fall back to toma_indices
		 * in single-CDV mode where the two are identical.
		 */
		for (j = 0; j < N_L2; j++) {
			u64 data_phys = l2[j].cdv_offset;
			u64 data_idx, slot_in;
			u64 V;
			struct nvmeibc_tpv_extent_entry *ee;
			struct persist_load_extent *le;
			const u64 *own_list;
			u64        own_count;

			if (data_phys == TPV_TREE_NULL)
				continue;

			persist_decode_phys(data_alloc, data_phys, &data_idx, &slot_in);

			if (nvmeibc_tpv_is_split(tpv)) {
				own_list  = data_toma_indices;
				own_count = data_toma_count;
			} else {
				own_list  = toma_indices;
				own_count = toma_count;
			}

			/* Sanity: L2 leaf must reference an extent that
			 * TOMA says belongs to us. Skip the check when the
			 * data-side TOMA list is unavailable (split mode,
			 * data TOMA identity not yet known - already warned
			 * at the S.2b fetch site). */
			if (tp_verify_l1_l2_extent_ownership && own_list) {
				u64 k;
				bool owned = false;

				for (k = 0; k < own_count; k++) {
					if (own_list[k] == data_idx) {
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

			V = i * N_L2 + j;

			ee = kzalloc(sizeof(*ee), GFP_NOIO);
			if (!ee) {
				rv = -ENOMEM;
				goto out_free;
			}
			ee->phys_offset      = data_phys;
			ee->cdv_extent_index = data_idx;
			ee->persisted        = true;

			/* Leaves point at data-CDV offsets and live on the
			 * data-side allocator's extent_map. */
			rv = xa_err(xa_store(&data_alloc->extent_map, V,
					     ee, GFP_NOIO));
			if (rv) {
				kfree(ee);
				goto out_free;
			}
			loaded++;

			/*
			 * Track per-CDV_extent slot usage for the tree-owning
			 * side's le_list. In single-CDV mode that side owns
			 * both L1/L2 and data, so data-leaf slots need marking
			 * here. In split mode data leaves live on a different
			 * CDV - their extent tracking is handled by
			 * load_state_populate_data_side() below and they must
			 * not enter meta's le_list (extent-index collisions
			 * between the two CDVs would poison it).
			 */
			if (!nvmeibc_tpv_is_split(tpv)) {
				le = persist_find_or_create_le(&le_list,
							      data_idx,
							      n_slots);
				if (!le) {
					rv = -ENOMEM;
					goto out_free;
				}
				set_bit(slot_in, le->used_bm);
			} else {
				(void)le;
			}
		}
	}

	vfree(l2);
	l2 = NULL;
	vfree(l1_buf);
	l1_buf = NULL;

	/* -- 6. Ensure every TOMA-owned extent has an le entry ----------
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

	/* -- 7. Build cdv_extent_list and free_tpv_extents ------------- */
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
	/* -- 7. Store TOMA list for recovery ---------------------------- */
	alloc->toma_extent_list  = toma_indices;
	alloc->toma_extent_count = toma_count;

	/*
	 * Split-mode: the data-side TOMA list was already fetched in S.2b so
	 * the L1/L2 walk could use it for leaf-ownership checks. Use it here
	 * to populate the data-side allocator's cdv_extent_list and free
	 * pool. Data-leaf slots already installed in data_alloc->extent_map
	 * are excluded from the free pool by load_state_populate_data_side.
	 */
	if (nvmeibc_tpv_is_split(tpv) && data_toma_count > 0) {
		int rv2 = load_state_populate_data_side(tpv,
				data_toma_indices, data_toma_count);
		if (rv2) {
			_NE(tpv_load_data_side_populate_fail,
			    "TPV: @STR: data-side populate failed rv=@INT",
			    tpv->tpv_name, rv2);
			kvfree(data_toma_indices);
			return rv2;
		}
		data_alloc->toma_extent_list  = data_toma_indices;
		data_alloc->toma_extent_count = data_toma_count;
		data_toma_indices = NULL;
	} else {
		/* No data-side TOMA list (single-CDV, no extents, or TOMA not
		 * yet known). Free any allocation; recovery will retry later. */
		kvfree(data_toma_indices);
	}
	return 0;

out_free:
	persist_free_le_list(&le_list);
	vfree(l2);
	vfree(l1_buf);
	kvfree(toma_indices);
	kvfree(data_toma_indices);
	return rv;
}
EXPORT_SYMBOL(nvmeibc_tpv_load_state);

/* -- nvmeibc_tpv_persist_work_fn ----------------------------------------- */

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

/* -- nvmeibc_tpv_load_state_work_fn ------------------------------------- */

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
	 * When io_max_retry_secs is 0 (default), fall back to ~infinite -
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
		    "TPV: @STR: data pool empty after load; scheduling CDV alloc",
		    tpv->tpv_name);
		if (!atomic_xchg(&tpv->cdv_alloc_pending, 1))
			schedule_work(&tpv->cdv_alloc_work);
	}

	/*
	 * Split mode: the metadata allocator has its own free pool that
	 * flush_state's L2-slot allocator draws from. If it's empty at
	 * load time the first flush would stall until a CDV_ALLOCATOR_UPDATE
	 * topology push kicks the meta side. Prefetch symmetrically.
	 */
	if (tpv->meta_allocator &&
	    tpv->meta_allocator->free_tpv_extent_count == 0) {
		_NI(tpv_meta_pool_empty_after_load,
		    "TPV: @STR: meta pool empty after load; scheduling meta CDV alloc",
		    tpv->tpv_name);
		if (!atomic_xchg(&tpv->meta_cdv_alloc_pending, 1))
			schedule_work(&tpv->meta_cdv_alloc_work);
	}
}
EXPORT_SYMBOL(nvmeibc_tpv_load_state_work_fn);

/* -- nvmeibc_tpv_install_data_extent ------------------------------------- */

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
	(void)tpv;
	(void)extent_index;
	return 0;
}
EXPORT_SYMBOL(nvmeibc_tpv_install_data_extent);
