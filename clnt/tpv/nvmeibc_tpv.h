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
 * TPV.allocator (this file) and persisted in a per-TPV L1/L2 tree stored
 * in the TPV's own "tree extent" (a CDV_extent whose slots are reserved
 * for metadata, not data).
 */

#include <linux/xarray.h>		/* struct xarray, xa_store/xa_load */
#include "common/nvmeib.h"		/* NVMEIBC_BD_UUID_LEN, NVMEIB_HOST_NAME_LEN */
#include "common_public/nvmeib_public_procfs.h"	/* nvmeib_public_procfs_ent, proc_fill_t */
#include "clnt/atom/nvmeiba_nvmesh_api.h"	/* nvmeiba_atom_os_api, nvmeiba_status_* */

/* Forward declarations — full definitions live outside this header. */
struct nvmeibc_volume;
struct nvmeibc_cinst_params_main;

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
 *
 * cdv_extent_index records which data CDV_extent holds this slot.  This lets
 * nvmeibc_tpv_free_extent() find the parent nvmeibc_cdv_extent_ref and
 * decrement its allocated_count without needing A and E from the CDV config.
 */
struct nvmeibc_tpv_extent_entry {
	u64 phys_offset;
	u64 cdv_extent_index;	/* data CDV_extent index; for ref-count bookkeeping on free */
	bool persisted;		/* true once flush_state has written this mapping to CDV */
	struct rcu_head rcu;	/* deferred free via kfree_rcu after xa_erase */
};

/*
 * Tracks one available physical TPV_extent slot within an already-allocated
 * CDV_extent.  Lives on nvmeibc_tpv_allocator.free_tpv_extents.
 * Shared between allocator and persistence code.
 */
struct nvmeibc_tpv_free_slot {
	u64              phys_offset;		/* CDV byte offset of this slot */
	u64              cdv_extent_index;	/* CDV_extent containing this slot */
	struct list_head node;
};

/*
 * Tracks a single CDV_extent allocated from the CDV.allocator (TOMA).
 * One CDV_extent holds n_slots = (cdv_extent_size / tpv_extent_size) TPV_extents.
 *
 * Slots within this extent may be (a) free — on alloc->free_tpv_extents,
 * (b) a data slot referenced by the xarray, (c) an L2 table (dynamic L2
 * placement, see nvmeibc_tpv_persist.c), or (d) slot 0 of the L1 extent.
 *
 * allocated_count counts (b) + (c); slot 0 of the L1 extent is pinned
 * separately by is_l1_extent and is not reflected in allocated_count.
 *
 * In change 1 L2 tables are never migrated or freed during the TPV
 * lifetime, so l2_slots is sticky-monotonic.  Because an extent with
 * l2_slots > 0 also has allocated_count > 0, the "can this extent be
 * returned to TOMA" check is simply (allocated_count == 0 && !is_l1_extent).
 */
struct nvmeibc_cdv_extent_ref {
	u64              extent_index;		/* CDV_extent index i */
	u64              allocated_count;	/* data + L2 slots in use */
	u64              l2_slots;		/* L2 tables currently in this extent */
	bool             is_l1_extent;		/* slot 0 holds the L1 table; extent pinned */
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

	/*
	 * CDV geometry — learned from the parent CDV's config at attach time.
	 * Needed to compute n_slots and physical offsets of each TPV_extent slot
	 * within a newly allocated CDV_extent.
	 */
	u32              cdv_extent_size_mb;	/* E in MB; CDV property */
	u64              allocator_size_gb;	/* A in GB; byte offset of first data extent */

	struct list_head cdv_extent_list;	/* nvmeibc_cdv_extent_ref entries */
	u64              cdv_extents_count;

	struct list_head free_tpv_extents;	/* available physical TPV_extent slots */
	u64              free_tpv_extent_count;

	/*
	 * CDV_extents whose allocated_count reached zero (all TPV_extents freed).
	 * Kept here rather than freed immediately so that nvmeibc_tpv_free_extent()
	 * can run without blocking in IO context.  cdv_alloc_work processes this
	 * list before requesting new CDV_extents.
	 */
	struct list_head pending_return_list;	/* nvmeibc_cdv_extent_ref entries */

	u64              low_watermark;		/* schedule CDV alloc when count drops below */
						/* default: 50 MB / tpv_extent_size */

	/*
	 * Per-allocator statistics — updated via atomic ops (no lock required).
	 * Readable at any time from /proc; reset via the stats proc entry.
	 */
	atomic64_t       stat_tpv_alloc_ok;	/* successful alloc_extent() calls */
	atomic64_t       stat_tpv_alloc_eagain; /* pool-empty hits (-EAGAIN) */
	atomic64_t       stat_tpv_alloc_enomem; /* OOM failures (-ENOMEM) */
	atomic64_t       stat_tpv_free_ok;	/* successful free_extent() calls */
	atomic64_t       stat_cdv_alloc_ok;	/* CDV_ALLOC_EXTENT OK responses */
	atomic64_t       stat_cdv_alloc_full;	/* CDV_ALLOC_EXTENT CDV_FULL responses */
	atomic64_t       stat_cdv_alloc_wgen;	/* CDV_ALLOC_EXTENT WRONG_GEN responses */
	atomic64_t       stat_cdv_alloc_err;	/* CDV_ALLOC_EXTENT transport send failures */
	atomic64_t       stat_cdv_free_ok;	/* CDV_FREE_EXTENT sends (successful) */
	atomic64_t       stat_cdv_alloc_ns;	/* cumulative CDV alloc round-trip time (ns) */

	/*
	 * Per-TPV L1/L2 tree metadata (dynamic L2 placement).
	 *
	 * L1 lives in slot 0 of the "L1 extent" — the first CDV_extent ever
	 * allocated to this TPV.  The L1 extent is a normal data extent in
	 * every other respect: its remaining slots (1..n_slots-1) enter
	 * free_tpv_extents and are used for data or for L2 tables.
	 *
	 * L2 tables are allocated lazily from free_tpv_extents by
	 * nvmeibc_tpv_flush_state(); any TPV-owned CDV_extent may host an
	 * L2 slot.  An extent that currently holds any L2 slot has
	 * allocated_count > 0 (since L2 slots count), so the normal
	 * free-extent path already keeps it attached.
	 *
	 * l1_to_l2_phys maps L1_idx -> CDV byte offset of the L2 table
	 * (encoded via xa_mk_value).  It is populated by load_state from the
	 * on-disk L1 and extended by flush_state when new L1 indices first
	 * become non-null.
	 */
	u64              l1_extent_index;	/* CDV_extent holding L1 in slot 0; 0 = none yet */
	u64              n_l2_tables_used;	/* L2 tables currently allocated */
	struct xarray    l1_to_l2_phys;		/* L1_idx -> CDV byte offset of L2 table */

	/*
	 * Cached CDV_LIST_EXTENTS result from load_state.
	 * Owned by load_state; transferred to recovery to avoid a second
	 * TOMA round-trip.  kvmalloc'd; callers must use kvfree().
	 */
	u64             *toma_extent_list;
	u64              toma_extent_count;
};

/* ── TPV state enum ────────────────────────────────────────────────────── */

enum nvmeibc_tpv_state {
	TPV_ATTACHING  = 0,
	TPV_ATTACHED   = 1,
	TPV_DETACHING  = 2,
	TPV_ORPHAN     = 3,	/* NDU: nvmeibc gone, ATOM buffering BIOs */
};

/* ── Per-TPV instance ──────────────────────────────────────────────────── */

struct nvmeibc_tpv {
	struct nvmeiba_atom_os_api    atom;		/* MUST be first for container_of */
	struct nvmeibc_volume        *cdv_vol;		/* parent CDV volume pointer */
	struct nvmeibc_tpv_allocator  allocator;

	char                          tpv_uuid[NVMEIBC_BD_UUID_LEN];
	char                          tpv_name[NVMEIBC_BD_NAME_LEN];	/* human-readable name */
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

	/*
	 * Bios blocked waiting for free TPV_extent slots.
	 * Protected by pending_bio_lock (irqsave).
	 * Drained by nvmeibc_tpv_retry_pending_bios() after new CDV_extent slots
	 * arrive via tpv_on_cdv_alloc_ok().
	 */
	struct bio_list               pending_bios;
	spinlock_t                    pending_bio_lock;

	/*
	 * Synchronous L1 flush mode.  When true, data bios for freshly-
	 * allocated extents are parked on pending_l1_flush_bios until the
	 * L1/L2 tree is flushed to the tree extent.  When false, the
	 * deferred-flush path is used (persist_work fires asynchronously).
	 * Set at attach time from the sourceUUID CM field and immutable
	 * afterwards.
	 */
	bool                          sync_flush;

	/*
	 * Bios parked waiting for L1 flush (sync_flush mode only).
	 * Protected by pending_bio_lock.  Drained by persist_work after
	 * a successful flush_state via nvmeibc_tpv_forward_l1_flush_bios().
	 */
	struct bio_list               pending_l1_flush_bios;

	/*
	 * Deferred load of allocator state from the per-TPV tree extent.
	 * Scheduled at attach; retries on I/O failure (CDV not ready).
	 * state_loaded is set under pending_bio_lock; readers use
	 * double-checked locking (READ_ONCE + re-check under lock).
	 */
	struct delayed_work           load_state_work;
	bool                          state_loaded;

	/* Deferred persistence of allocator state to the per-TPV tree extent. */
	struct work_struct            persist_work;
	spinlock_t                    persist_lock;
	bool                          dirty;

	/*
	 * IO timeout for parked bios — mirrors regular volume max_retry_jiffies.
	 * Uses nvmeibc_io_max_retry_secs module param (shared with regular
	 * volumes); when 0, falls back to IO_TIME_OUT_ATTACH (30 s) at
	 * attach or IO_TIME_OUT_NORMAL (~infinite) after state_loaded.
	 * Reduced to HZ / 100 (10 ms) at detach for fast drain.
	 */
	unsigned long                 max_retry_jiffies;

	/*
	 * Timeout sweep for pending_bios / pending_l1_flush_bios.
	 * Scheduled when the first bio is parked; fires after max_retry_jiffies
	 * to fail all parked bios with -EIO.  Cancelled when bios are drained
	 * successfully by retry_pending_bios / forward_l1_flush_bios.
	 */
	struct delayed_work           timeout_work;

	/* Entry in the per-client active TPV list. */
	struct list_head              list_node;

	/* /proc/nvmeibc/tpv/<name>/ entries; NULL until attach completes. */
	struct proc_dir_entry                *proc_dir;
	struct nvmeib_public_procfs_ent      *proc_status;
	struct nvmeib_public_procfs_ent      *proc_allocator;
	struct nvmeib_public_procfs_ent      *proc_tpv_extent_map;
	struct nvmeib_public_procfs_ent      *proc_cdv_extent_map;
	struct nvmeib_public_procfs_ent      *proc_stats;
	struct nvmeib_public_procfs_ent      *proc_selftest;

	/*
	 * Per-TPV copy of fops, kept in kzalloc'd memory so it survives NDU.
	 * .owner = nvmeiba module, .open/.close = nvmeiba handlers,
	 * .submit_bio = nvmeibc_tpv_submit_bio_wrapper (set at adopt/attach).
	 */
	struct block_device_operations tpv_live_fops;

	/*
	 * In-flight IO counter for NDU drain.  Incremented on
	 * nvmeibc_tpv_make_request entry, decremented after CDV hand-off.
	 * Abandon waits for this to reach zero before orphaning the atom.
	 */
	atomic_t                      io_inflight;
};

/* ── ATOM disk/queue accessors ─────────────────────────────────────────── */

#define tpv_disk(tpv)   ((tpv)->atom.disk)
#define tpv_queue(tpv)  ((tpv)->atom.queue)

/* ── L1/L2 tree on-disk entry format ────────────────────────────────────── */

/*
 * Every entry at every level (L1, L2) is 16 bytes.
 * extent_index == TPV_TREE_NULL means the slot is empty / not present.
 * CDV_extent index 0 is never allocated by TOMA (it is the TOMA allocator
 * area's own L1), so 0 is a safe null sentinel.
 *
 * L1/L2 tree model (dynamic L2 placement):
 *   Each TPV has a private L1 table stored in slot 0 of the "L1 extent"
 *   (the first CDV_extent ever allocated to this TPV).  L2 tables live in
 *   arbitrary TPV-owned slots allocated lazily from the free pool by
 *   nvmeibc_tpv_flush_state().  Data slots live in all remaining slots.
 *
 *   L1 entries:    extent_index = CDV_extent holding the L2 table,
 *                  debug_meta   = slot within that extent
 *   L2 leaf entries: extent_index = data CDV_extent index,
 *                    debug_meta   = slot within that CDV_extent
 *
 *   Address translation (2-level):
 *     L1_idx = V / N_L2;  L2_idx = V % N_L2
 *     data_idx = L2[L2_idx].extent_index
 *     slot     = L2[L2_idx].debug_meta
 *     phys_offset = A + (data_idx - 1) * E + slot * T
 */
struct tpv_tree_entry {
	u64 extent_index;		/* CDV_extent index of child table or data extent */
	u64 debug_meta;			/* L1: slot of L2 within extent_index; L2 leaf: slot of data */
};

#define TPV_TREE_NULL  0ULL

/* ── L1 table on-disk header (first 64 bytes of L1 extent slot 0) ───── */

#define TPV_L1_MAGIC		0x5450564C31544142ULL	/* "TPVL1TAB" */
#define TPV_L1_VERSION		1

struct tpv_l1_header {
	u64 magic;			/* TPV_L1_MAGIC */
	u64 version;			/* TPV_L1_VERSION */
	u8  tpv_uuid[16];		/* owning TPV UUID */
	u64 l1_extent_index;		/* CDV_extent holding the L1 in its slot 0 */
	u64 n_l2_tables_used;		/* number of L2 tables currently allocated */
	u8  reserved[16];		/* pad to 64 bytes total */
};

/* ── IO API (implemented in nvmeibc_tpv_io.c) ─────────────────────────── */

/*
 * Module-level init/exit for the IO subsystem (bioset for bio splitting).
 * Must be called once at module load/unload.
 */
int  nvmeibc_tpv_io_init(void);
void nvmeibc_tpv_io_exit(void);

/*
 * Re-dispatch bios parked on tpv->pending_bios after new TPV_extent slots
 * arrive.  Called from cdv_alloc_work context (process context, may sleep).
 */
void nvmeibc_tpv_retry_pending_bios(struct nvmeibc_tpv *tpv);

/*
 * Forward bios parked on pending_l1_flush_bios after a successful L1 flush.
 * Called from persist_work context (process context, may sleep).
 */
void nvmeibc_tpv_forward_l1_flush_bios(struct nvmeibc_tpv *tpv);

/* ── Public API (implemented in nvmeibc_tpv.c) ────────────────────────── */

/*
 * Returns the new nvmeibc_tpv on success, NULL on error.
 * cdv_extent_size_mb and allocator_size_gb are properties of the parent CDV,
 * received from management in the AttachVolumes MCS cdvConf payload.
 */
struct nvmeibc_tpv *nvmeibc_tpv_attach(struct nvmeibc_volume *cdv,
					const char *tpv_name,
					const char *tpv_uuid,
					u64 virtual_size_bytes,
					u32 tpv_extent_size_kb,
					u32 cdv_extent_size_mb,
					u64 allocator_size_gb,
					bool sync_flush);

void nvmeibc_tpv_detach(struct nvmeibc_tpv *tpv);

/* Handle UpdateVolume MCS: grow virtual size and update gendisk capacity. */
void nvmeibc_tpv_grow(struct nvmeibc_tpv *tpv, u64 new_virtual_size_bytes);

/* Look up an active TPV by UUID across all instances. */
struct nvmeibc_tpv *nvmeibc_tpv_find_by_uuid(const char *uuid);

/*
 * Detach every active TPV whose parent CDV belongs to @cinst.
 * Must be called BEFORE the CDVs of the same instance are detached.
 */
void nvmeibc_tpv_detach_all_for_inst(const struct nvmeibc_cinst_params_main *cinst);

/*
 * NDU abandon: orphan every active TPV whose parent CDV belongs to @cinst.
 * Flushes dirty state, cancels workers, and hands the TPV atom to ATOM's
 * orphan-buffering mode.  Must be called BEFORE CDV abandon.
 */
void nvmeibc_tpv_abandon_all_for_inst(const struct nvmeibc_cinst_params_main *cinst);

/*
 * Update the CDV.allocator TOMA identity after a topology push.
 * Callers must fence in-flight CDV_ALLOC_EXTENT requests with the old
 * generation before accepting responses from the new allocator.
 */
void nvmeibc_tpv_update_allocator_id(struct nvmeibc_tpv *tpv,
				     const char *toma_id,
				     u64 generation);

/*
 * nvmeibc_tpv_update_allocator_for_cdv — update allocator identity for all
 * TPVs backed by the given CDV UUID.
 *
 * Called from the topology handler when a CDV_ALLOCATOR_UPDATE message is
 * received from TOMA.  Iterates the active TPV list, matches by parent CDV
 * UUID, and calls nvmeibc_tpv_update_allocator_id() on each match.
 *
 * Safe to call from any context (uses spinlock internally).
 */
void nvmeibc_tpv_update_allocator_for_cdv(const char *cdv_uuid,
					   const char *toma_id,
					   u64 generation);

/*
 * nvmeibc_tpv_alloc_l2_slot — claim a free TPV_extent slot for use as an L2
 * table.  Pops one slot off alloc->free_tpv_extents and increments the owning
 * cdv_extent_ref's allocated_count and l2_slots.  Used by flush_state when a
 * new L1 index becomes non-null and must be backed by a fresh L2 table.
 *
 * Returns 0 on success (*phys_offset_out populated), -EAGAIN when the free
 * pool is empty (caller should defer the flush; cdv_alloc_work will be
 * re-armed by the allocator as usual).
 */
int nvmeibc_tpv_alloc_l2_slot(struct nvmeibc_tpv *tpv, u64 *phys_offset_out);

/* ── IB admin CDV response dispatch (implemented in nvmeibc_tpv_ib_admin.c) ── */

struct nvmeibc_cdv_alloc_resp;
struct nvmeibc_cdv_list_resp;
void nvmeibc_cdv_dispatch_alloc_response(const struct nvmeibc_cdv_alloc_resp *resp);
void nvmeibc_cdv_dispatch_list_response(const struct nvmeibc_cdv_list_resp *resp,
					const u64 *indices, u64 n_indices);

/* Test hook pointers (NULL in production; set by nvmeibc_tpv_test.c) */
struct nvmeibc_cdv_alloc_req;
struct nvmeibc_cdv_free_req;
extern int (*nvmeibc_tpv_test_cdv_alloc_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_alloc_req *req,
	struct nvmeibc_cdv_alloc_resp *resp);
extern int (*nvmeibc_tpv_test_cdv_free_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_free_req *req);

/* ── Allocator API (implemented in nvmeibc_tpv_allocator.c) ───────────── */

int  nvmeibc_tpv_alloc_extent(struct nvmeibc_tpv *tpv, u64 virt_idx,
			      struct nvmeibc_tpv_extent_entry **out);

int  nvmeibc_tpv_free_extent(struct nvmeibc_tpv *tpv, u64 virt_idx);

/* Free all nvmeibc_tpv_free_slot entries on a list. Called at detach. */
void nvmeibc_tpv_free_slots_list(struct list_head *free_tpv_extents);

/* Background work handler: return empty CDV_extents, then request new ones. */
void nvmeibc_tpv_cdv_alloc_work_fn(struct work_struct *work);

/* ── Persistence API (implemented in nvmeibc_tpv_persist.c) ───────────── */

int  nvmeibc_tpv_load_state(struct nvmeibc_tpv *tpv);
int  nvmeibc_tpv_flush_state(struct nvmeibc_tpv *tpv);

/* Install a newly allocated data CDV_extent into the L1 tree (no-op in flat-L1 model). */
int  nvmeibc_tpv_install_data_extent(struct nvmeibc_tpv *tpv, u64 extent_index);

/* Background work handler: flush dirty allocator state to the tree extent. */
void nvmeibc_tpv_persist_work_fn(struct work_struct *work);

/* Timeout sweep: fail parked bios that exceeded max_retry_jiffies. */
void nvmeibc_tpv_timeout_work_fn(struct work_struct *work);

/* Background work handler: load allocator state from the tree extent + recovery. */
void nvmeibc_tpv_load_state_work_fn(struct work_struct *work);

/* ── Recovery API (implemented in nvmeibc_tpv_recovery.c) ─────────────── */

int  nvmeibc_tpv_recovery(struct nvmeibc_tpv *tpv);

/* ── Proc API (implemented in nvmeibc_tpv_proc.c) ─────────────────────── */

/*
 * nvmeibc_tpv_proc_register — create /proc/nvmeibc/tpv/<name>/ entries.
 * Called from nvmeibc_tpv_attach() after the TPV is added to the active list.
 * Silently skips registration if the module proc root is not yet available.
 */
void nvmeibc_tpv_proc_register(struct nvmeibc_tpv *tpv);

/*
 * nvmeibc_tpv_proc_deregister — remove /proc/nvmeibc/tpv/<name>/ entries.
 * Called from nvmeibc_tpv_detach() before the allocator is freed.
 */
void nvmeibc_tpv_proc_deregister(struct nvmeibc_tpv *tpv);

/*
 * nvmeibc_tpv_proc_destroy_root — remove /proc/nvmeibc/tpv/ at module unload.
 * Must be called after all per-TPV entries are gone and before the parent
 * /proc/nvmeibc/ directory is removed.
 */
void nvmeibc_tpv_proc_destroy_root(void);

#endif /* NVMEIBC_TPV_H */
