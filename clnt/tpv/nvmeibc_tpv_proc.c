/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_proc.c — /proc/nvmeibc/tpv/<name>/ entries for TPV diagnostics.
 *
 * Per-TPV proc directory layout:
 *
 *   status      — geometry, state, CDV.allocator identity and generation
 *   allocator   — live pool counters (cdv extents, free slots, watermark,
 *                 pending returns)
 *   tpv_extent_map  — full xarray dump: virtual index → physical offset in CDV,
 *                     owning CDV_extent index
 *   cdv_extent_map  — per-allocated CDV_extent: sequence number → CDV extent index
 *                     and how many TPV_extent slots within it are currently in use
 *   stats       — allocation/free counts and CDV round-trip timing (writable
 *                 to reset counters)
 *
 * All fill functions snapshot state under the allocator spinlock where needed.
 * The xarray dump acquires xa_lock via rcu_read_lock() + xa_for_each() to
 * avoid holding the allocator lock across potentially thousands of entries.
 *
 * Proc root lifecycle:
 *   /proc/nvmeibc/tpv/ is created lazily on the first TPV attach (under
 *   nvmeibc_tpv_proc_root_lock) and intentionally left alive until module
 *   unload; it is a stable sibling of the existing "disks" directory.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"	/* nvmeibc_volume_get_size, NVMEIBC_SECTOR_SHIFT */
#include "nvmeibc_tpv_test.h"		/* nvmeibc_tpv_run_selftests */
#include "module/nvmeibc_module_main.h"	/* nvmeibc_get_module_proc_dir_entry */

/* ── Module-level TPV proc root (/proc/nvmeibc/tpv/) ───────────────────── */

static struct proc_dir_entry *nvmeibc_tpv_proc_root;
static DEFINE_MUTEX(nvmeibc_tpv_proc_root_lock);

/*
 * tpv_proc_ensure_root — create /proc/nvmeibc/tpv/ on the first call.
 * Returns the root dir on success, NULL on error.
 */
static struct proc_dir_entry *tpv_proc_ensure_root(void)
{
	struct proc_dir_entry *module_dir;

	if (nvmeibc_tpv_proc_root)
		return nvmeibc_tpv_proc_root;

	mutex_lock(&nvmeibc_tpv_proc_root_lock);
	if (!nvmeibc_tpv_proc_root) {
		module_dir = nvmeibc_get_module_proc_dir_entry();
		if (module_dir)
			nvmeibc_tpv_proc_root = proc_mkdir("tpv", module_dir);
	}
	mutex_unlock(&nvmeibc_tpv_proc_root_lock);

	return nvmeibc_tpv_proc_root;
}

/* ── Helper: state string ───────────────────────────────────────────────── */

static const char *tpv_state_str(int state)
{
	switch (state) {
	case TPV_ATTACHING:  return "attaching";
	case TPV_ATTACHED:   return "attached";
	case TPV_DETACHING:  return "detaching";
	case TPV_ORPHAN:     return "orphan";
	default:             return "unknown";
	}
}

/* ── status fill ────────────────────────────────────────────────────────── */

static ssize_t tpv_proc_status_fill(void *arg, char *buf, size_t len)
{
	struct nvmeibc_tpv           *tpv   = arg;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	char   toma_id[NVMEIB_HOST_NAME_LEN];
	u64    gen;
	u64    cdv_extents_allocated, free_tpv_slots;
	u64    cdv_extents_total = 0;
	unsigned long flags;
	ssize_t count = 0;

#define BUF_ADD(...) count += scnprintf(buf + count, len - count, __VA_ARGS__)

	spin_lock_irqsave(&tpv->allocator_id_lock, flags);
	memcpy(toma_id, tpv->allocator_toma_id, sizeof(toma_id));
	gen = tpv->allocator_generation;
	spin_unlock_irqrestore(&tpv->allocator_id_lock, flags);

	spin_lock(&alloc->lock);
	cdv_extents_allocated = alloc->cdv_extents_count;
	free_tpv_slots        = alloc->free_tpv_extent_count;
	spin_unlock(&alloc->lock);

	/* Compute total data extents available in the CDV (same formula as the
	 * CDV_ALLOC_EXTENT request: subtract allocator region, divide by extent size). */
	if (tpv->cdv_vol && alloc->cdv_extent_size_mb > 0) {
		u64 cdv_bytes  = (u64)nvmeibc_volume_get_size(tpv->cdv_vol)
				  << NVMEIBC_SECTOR_SHIFT;
		u64 meta_bytes = alloc->allocator_size_gb << 30;
		u64 data_bytes = (cdv_bytes > meta_bytes) ? cdv_bytes - meta_bytes : 0;
		cdv_extents_total = data_bytes / ((u64)alloc->cdv_extent_size_mb << 20);
	}

	BUF_ADD("name:                %s\n",  tpv->tpv_name);
	BUF_ADD("uuid:                %s\n",  tpv->tpv_uuid);
	BUF_ADD("state:               %s\n",  tpv_state_str(atomic_read(&tpv->state)));
	BUF_ADD("state_loaded:        %s\n",  READ_ONCE(tpv->state_loaded) ? "yes" : "no");
	BUF_ADD("sync_flush:          %s\n",  tpv->sync_flush ? "yes" : "no");
	BUF_ADD("virtual_size_mb:     %llu\n", tpv->virtual_size >> 20);
	BUF_ADD("tpv_extent_size_kb:  %u\n",  alloc->tpv_extent_size_kb);
	BUF_ADD("cdv_extent_size_mb:  %u\n",  alloc->cdv_extent_size_mb);
	BUF_ADD("allocator_size_gb:   %llu\n", alloc->allocator_size_gb);
	BUF_ADD("virtual_extents:     %llu\n", alloc->virtual_extents_total);
	BUF_ADD("low_watermark:       %llu\n", alloc->low_watermark);
	BUF_ADD("cdv_extents_allocated: %llu / %llu\n",
		cdv_extents_allocated, cdv_extents_total);
	BUF_ADD("free_tpv_slots:      %llu\n", free_tpv_slots);
	BUF_ADD("l1_extent_index:     %llu\n", alloc->l1_extent_index);
	BUF_ADD("n_l2_tables_used:    %llu\n", alloc->n_l2_tables_used);
	BUF_ADD("allocator_toma:      %s\n",  toma_id[0] ? toma_id : "(none)");
	BUF_ADD("allocator_gen:       %llu\n", gen);

#undef BUF_ADD
	return count;
}

/* ── allocator fill ─────────────────────────────────────────────────────── */

static ssize_t tpv_proc_allocator_fill(void *arg, char *buf, size_t len)
{
	struct nvmeibc_tpv           *tpv   = arg;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64    cdv_extents, free_slots, pending;
	ssize_t count = 0;

#define BUF_ADD(...) count += scnprintf(buf + count, len - count, __VA_ARGS__)

	spin_lock(&alloc->lock);
	cdv_extents = alloc->cdv_extents_count;
	free_slots  = alloc->free_tpv_extent_count;
	pending     = 0;
	{
		struct nvmeibc_cdv_extent_ref *ref;
		list_for_each_entry(ref, &alloc->pending_return_list, node)
			pending++;
	}
	spin_unlock(&alloc->lock);

	BUF_ADD("state_loaded:            %s\n",  READ_ONCE(tpv->state_loaded) ? "yes" : "no");
	BUF_ADD("cdv_extents_count:       %llu\n", cdv_extents);
	BUF_ADD("free_tpv_extent_count:   %llu\n", free_slots);
	BUF_ADD("low_watermark:           %llu\n", alloc->low_watermark);
	BUF_ADD("pending_return_count:    %llu\n", pending);
	BUF_ADD("cdv_alloc_pending:       %d\n",   atomic_read(&tpv->cdv_alloc_pending));
	BUF_ADD("l1_extent_index:         %llu\n", alloc->l1_extent_index);
	BUF_ADD("n_l2_tables_used:        %llu\n", alloc->n_l2_tables_used);

#undef BUF_ADD
	return count;
}

/* ── tpv_extent_map fill ────────────────────────────────────────────────── */

/*
 * Dumps every mapped virtual extent from the xarray, annotated with its
 * position in the on-disk L1/L2 tree and whether it has been persisted.
 *
 * For virtual extent index V:
 *   L1_idx = V / N_L2    (which L2 table covers this extent)
 *   L2_idx = V % N_L2    (slot within that L2 table)
 *
 * "persisted" indicates whether flush_state has written this mapping to CDV.
 * Dirty entries (persisted=no) are volatile and would be lost on crash.
 *
 * Format per line:
 *   virt_idx  l1_idx  l2_idx  phys_offset_hex  persisted
 */
static ssize_t tpv_proc_tpv_extent_map_fill(void *arg, char *buf, size_t len)
{
	struct nvmeibc_tpv           *tpv   = arg;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_tpv_extent_entry *entry;
	u64 n_l2, n_persisted = 0, n_dirty = 0;
	unsigned long idx;
	ssize_t count = 0;

#define BUF_ADD(...) count += scnprintf(buf + count, len - count, __VA_ARGS__)

	/* N_L2 entries per L2 table = T / sizeof(tpv_tree_entry) = T / 8 */
	n_l2 = ((u64)alloc->tpv_extent_size_kb << 10) / sizeof(struct tpv_tree_entry);

	BUF_ADD("%-16s  %-8s  %-8s  %-18s  %s\n",
		"virt_idx", "l1_idx", "l2_idx", "phys_offset", "persisted");

	rcu_read_lock();
	xa_for_each(&alloc->extent_map, idx, entry) {
		if (count + 96 >= (ssize_t)len) {
			BUF_ADD("... (truncated at %llu entries; buffer too small)\n",
				n_persisted + n_dirty);
			break;
		}
		BUF_ADD("%-16lu  %-8llu  %-8llu  0x%016llx  %s\n",
			idx, (u64)idx / n_l2, (u64)idx % n_l2,
			entry->phys_offset,
			entry->persisted ? "yes" : "no");
		if (entry->persisted)
			n_persisted++;
		else
			n_dirty++;
	}
	rcu_read_unlock();

	if (n_persisted + n_dirty == 0)
		BUF_ADD("(empty)\n");
	else
		BUF_ADD("[%llu entries: %llu persisted, %llu dirty]\n",
			n_persisted + n_dirty, n_persisted, n_dirty);

#undef BUF_ADD
	return count;
}

/* ── cdv_extent_map fill ────────────────────────────────────────────────── */

/*
 * Lists every CDV_extent currently allocated to this TPV from the
 * cdv_extent_list.  Acquired under alloc->lock (the list is short —
 * typically O(tens) of entries — so the brief hold is acceptable).
 *
 * Format per line:
 *   seq ==> cdv_extent_idx  (allocated_slots in use)
 */
static ssize_t tpv_proc_cdv_extent_map_fill(void *arg, char *buf, size_t len)
{
	struct nvmeibc_tpv           *tpv   = arg;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref *ref;
	u64 seq = 0;
	ssize_t count = 0;

#define BUF_ADD(...) count += scnprintf(buf + count, len - count, __VA_ARGS__)

	BUF_ADD("%-6s  ==>  %-16s  %-6s  %-6s  %s\n",
		"seq", "cdv_extent_idx", "is_l1", "l2", "in_use");

	spin_lock(&alloc->lock);
	list_for_each_entry(ref, &alloc->cdv_extent_list, node) {
		if (count + 80 >= (ssize_t)len) {
			spin_unlock(&alloc->lock);
			BUF_ADD("... (truncated at %llu entries; buffer too small)\n", seq);
			goto out;
		}
		BUF_ADD("%-6llu  ==>  %-16llu  %-6d  %-6llu  %llu\n",
			seq, ref->extent_index, (int)ref->is_l1_extent,
			ref->l2_slots, ref->allocated_count);
		seq++;
	}
	spin_unlock(&alloc->lock);

	if (seq == 0)
		BUF_ADD("(empty)\n");

out:
#undef BUF_ADD
	return count;
}

/* ── stats fill ─────────────────────────────────────────────────────────── */

static ssize_t tpv_proc_stats_fill(void *arg, char *buf, size_t len)
{
	struct nvmeibc_tpv           *tpv   = arg;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	s64    cdv_alloc_ok, cdv_alloc_ns, cdv_avg_us;
	ssize_t count = 0;

#define BUF_ADD(...) count += scnprintf(buf + count, len - count, __VA_ARGS__)

	cdv_alloc_ok = atomic64_read(&alloc->stat_cdv_alloc_ok);
	cdv_alloc_ns = atomic64_read(&alloc->stat_cdv_alloc_ns);
	cdv_avg_us   = (cdv_alloc_ok > 0) ? (cdv_alloc_ns / cdv_alloc_ok / 1000) : 0;

	BUF_ADD("# TPV extent allocation (IO path)\n");
	BUF_ADD("tpv_alloc_ok:        %lld\n", (s64)atomic64_read(&alloc->stat_tpv_alloc_ok));
	BUF_ADD("tpv_alloc_eagain:    %lld\n", (s64)atomic64_read(&alloc->stat_tpv_alloc_eagain));
	BUF_ADD("tpv_alloc_enomem:    %lld\n", (s64)atomic64_read(&alloc->stat_tpv_alloc_enomem));
	BUF_ADD("tpv_free_ok:         %lld\n", (s64)atomic64_read(&alloc->stat_tpv_free_ok));
	BUF_ADD("\n");
	BUF_ADD("# CDV extent allocation (background work)\n");
	BUF_ADD("cdv_alloc_ok:        %lld\n", cdv_alloc_ok);
	BUF_ADD("cdv_alloc_full:      %lld\n", (s64)atomic64_read(&alloc->stat_cdv_alloc_full));
	BUF_ADD("cdv_alloc_wrong_gen: %lld\n", (s64)atomic64_read(&alloc->stat_cdv_alloc_wgen));
	BUF_ADD("cdv_alloc_err:       %lld\n", (s64)atomic64_read(&alloc->stat_cdv_alloc_err));
	BUF_ADD("cdv_free_ok:         %lld\n", (s64)atomic64_read(&alloc->stat_cdv_free_ok));
	BUF_ADD("cdv_alloc_total_us:  %lld\n", cdv_alloc_ns / 1000);
	BUF_ADD("cdv_alloc_avg_us:    %lld\n", cdv_avg_us);

#undef BUF_ADD
	return count;
}

/*
 * Writing anything to the stats file resets all counters.
 */
static ssize_t tpv_proc_stats_reset(void *arg, char *buf, size_t len)
{
	struct nvmeibc_tpv           *tpv   = arg;
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;

	atomic64_set(&alloc->stat_tpv_alloc_ok,    0);
	atomic64_set(&alloc->stat_tpv_alloc_eagain, 0);
	atomic64_set(&alloc->stat_tpv_alloc_enomem, 0);
	atomic64_set(&alloc->stat_tpv_free_ok,      0);
	atomic64_set(&alloc->stat_cdv_alloc_ok,     0);
	atomic64_set(&alloc->stat_cdv_alloc_full,   0);
	atomic64_set(&alloc->stat_cdv_alloc_wgen,   0);
	atomic64_set(&alloc->stat_cdv_alloc_err,    0);
	atomic64_set(&alloc->stat_cdv_free_ok,      0);
	atomic64_set(&alloc->stat_cdv_alloc_ns,     0);

	_NI(tpv_proc_stats_reset_done, "TPV: @STR: proc stats reset", tpv->tpv_name);
	return (ssize_t)len;
}

/* ── Public registration / deregistration ───────────────────────────────── */

void nvmeibc_tpv_proc_register(struct nvmeibc_tpv *tpv)
{
	struct proc_dir_entry *root;

	root = tpv_proc_ensure_root();
	if (!root) {
		_NW(tpv_proc_no_root, "TPV: @STR: proc root unavailable; skipping proc registration",
		    tpv->tpv_name);
		return;
	}

	tpv->proc_dir = proc_mkdir(tpv->tpv_name, root);
	if (!tpv->proc_dir) {
		_NW(tpv_proc_mkdir_fail, "TPV: @STR: proc_mkdir failed", tpv->tpv_name);
		return;
	}

	tpv->proc_status = nvmeib_public_proc_create(
		"status", tpv->proc_dir, tpv_proc_status_fill, NULL, tpv);
	tpv->proc_allocator = nvmeib_public_proc_create(
		"allocator", tpv->proc_dir, tpv_proc_allocator_fill, NULL, tpv);
	tpv->proc_tpv_extent_map = nvmeib_public_proc_create(
		"tpv_extent_map", tpv->proc_dir, tpv_proc_tpv_extent_map_fill, NULL, tpv);
	tpv->proc_cdv_extent_map = nvmeib_public_proc_create(
		"cdv_extent_map", tpv->proc_dir, tpv_proc_cdv_extent_map_fill, NULL, tpv);
	tpv->proc_stats = nvmeib_public_proc_create(
		"stats", tpv->proc_dir, tpv_proc_stats_fill, tpv_proc_stats_reset, tpv);
	tpv->proc_selftest = nvmeib_public_proc_create(
		"selftest", tpv->proc_dir, nvmeibc_tpv_run_selftests, NULL, tpv);

	_ND(tpv_proc_registered, "TPV: @STR: proc entries registered", tpv->tpv_name);
}
EXPORT_SYMBOL(nvmeibc_tpv_proc_register);

void nvmeibc_tpv_proc_deregister(struct nvmeibc_tpv *tpv)
{
	if (!tpv->proc_dir)
		return;

	nvmeib_public_proc_remove(tpv->proc_selftest);
	nvmeib_public_proc_remove(tpv->proc_stats);
	nvmeib_public_proc_remove(tpv->proc_cdv_extent_map);
	nvmeib_public_proc_remove(tpv->proc_tpv_extent_map);
	nvmeib_public_proc_remove(tpv->proc_allocator);
	nvmeib_public_proc_remove(tpv->proc_status);

	remove_proc_entry(tpv->tpv_name, nvmeibc_tpv_proc_root);
	tpv->proc_dir = NULL;

	_ND(tpv_proc_deregistered, "TPV: @STR: proc entries removed", tpv->tpv_name);
}
EXPORT_SYMBOL(nvmeibc_tpv_proc_deregister);

/*
 * nvmeibc_tpv_proc_destroy_root — remove /proc/nvmeibc/tpv/.
 *
 * Called at module unload (nvmeibc_module_procs_destroy) BEFORE the parent
 * /proc/nvmeibc/ directory is removed.  All per-TPV subdirectories must
 * already be gone (nvmeibc_tpv_proc_deregister called for each detached TPV)
 * before this function is invoked; otherwise remove_proc_entry will warn.
 */
void nvmeibc_tpv_proc_destroy_root(void)
{
	mutex_lock(&nvmeibc_tpv_proc_root_lock);
	if (nvmeibc_tpv_proc_root) {
		remove_proc_entry("tpv", nvmeibc_get_module_proc_dir_entry());
		nvmeibc_tpv_proc_root = NULL;
	}
	mutex_unlock(&nvmeibc_tpv_proc_root_lock);
}
EXPORT_SYMBOL(nvmeibc_tpv_proc_destroy_root);
