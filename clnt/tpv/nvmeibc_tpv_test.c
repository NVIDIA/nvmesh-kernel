/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_test.c — TPV kernel self-tests and CDV transport stubs.
 *
 * This file has two responsibilities:
 *
 *   1. CDV transport stubs.  The TPV production code declares several
 *      extern functions that represent the CDV block-layer integration and
 *      IB admin channel additions (scheduled for later milestones):
 *
 *        nvmeibc_tpv_cdv_sync_read()          (nvmeibc_tpv_persist.c)
 *        nvmeibc_tpv_cdv_sync_write()         (nvmeibc_tpv_persist.c)
 *        nvmeibc_tpv_cdv_submit_bio()         (nvmeibc_tpv_io.c)
 *        nvmeibc_ib_admin_cdv_alloc_extent()  (nvmeibc_tpv_allocator.c)
 *        nvmeibc_ib_admin_cdv_free_extent()   (nvmeibc_tpv_allocator.c)
 *        nvmeibc_ib_admin_cdv_list_extents()  (nvmeibc_tpv_recovery.c)
 *
 *      This file provides concrete implementations for all six so that the
 *      kernel module links cleanly.  During a self-test run the global test
 *      context (g_tc) is active and the stubs use in-memory state; outside
 *      of a self-test they are no-ops or return -ENOTSUPP.
 *
 *   2. Five self-tests exercising the TPV allocator, persistence, and
 *      recovery paths:
 *
 *        tpv_ktest_alloc_free      — basic xarray alloc / free cycle
 *        tpv_ktest_persist         — flush_state / load_state roundtrip
 *        tpv_ktest_pool_exhaustion — -EAGAIN when free pool is empty
 *        tpv_ktest_double_free     — -ENOENT on second free of same virt_idx
 *        tpv_ktest_recovery        — orphan extent adoption
 *
 *      The tests are self-contained: each constructs a minimal nvmeibc_tpv
 *      directly (bypassing the full attach path which registers a gendisk),
 *      exercises the specific code path, and tears down cleanly.
 *
 * Test geometry (mirrors the userspace simulator):
 *   A = 0 GB   allocator_size_gb = 0  (no metadata region)
 *   E = 1 MB   cdv_extent_size_mb = 1
 *   T = 64 KB  tpv_extent_size_kb = 64
 *   n_slots = E / T = 16  slots per data CDV_extent
 *   n_data  = 4           data CDV_extents (indices 1 .. 4)
 *   CDV buf = 5 MB        L1 at [0, E) + data extents 1..4 at [E, 5E)
 *
 * Locking / synchronisation notes:
 *   • The allocator spinlock (alloc->lock) is always held correctly because
 *     we call the real production functions (alloc_extent / free_extent).
 *   • Work functions are initialised with the real handlers.  With an empty
 *     allocator_toma_id the cdv_alloc_work_fn bails early before accessing
 *     cdv_vol; persist_work_fn writes to the CDV buffer via sync_write.
 *   • tpv_ktest_destroy() sets TPV_DETACHING before cancel_work_sync() so
 *     both work handlers exit immediately without CDV access.
 *   • rcu_barrier() after cancel_work_sync() drains all kfree_rcu callbacks
 *     from free_extent() calls before we walk the xarray for cleanup.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_msgs_shared.h"	/* nvmeibc_cdv_alloc_req/resp, free_req */
#include "nvmeibc_tpv_test.h"

/* ── Forward declarations (IB admin stubs defined below) ────────────────── */
/*
 * The three IB admin functions are the only stubs still provided here.
 * The CDV block-layer transport functions (sync_read, sync_write, submit_bio)
 * are now implemented in nvmeibc_tpv_cdv.c; self-tests redirect them via the
 * nvmeibc_tpv_cdv_test_sync_{read,write}_fn hook pointers below.
 *
 * Prototypes satisfy -Werror=missing-prototypes at the definition sites.
 */
int  nvmeibc_ib_admin_cdv_alloc_extent(struct nvmeibc_volume *cdv,
					const char *toma_id,
					const struct nvmeibc_cdv_alloc_req *req,
					struct nvmeibc_cdv_alloc_resp *resp);
int  nvmeibc_ib_admin_cdv_free_extent(struct nvmeibc_volume *cdv,
				       const char *toma_id,
				       const struct nvmeibc_cdv_free_req *req);
int  nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
					const char *toma_id,
					const char *tpv_uuid,
					u64 **out_indices, u64 *out_count);

/* ── Test geometry constants ────────────────────────────────────────────── */

#define TPV_KTEST_CDV_EXT_MB	1u		/* E: CDV_extent size in MB */
#define TPV_KTEST_ALLOC_GB	0u		/* A: metadata region in GB (none) */
#define TPV_KTEST_TPV_EXT_KB	64u		/* T: TPV_extent size in KB */
#define TPV_KTEST_N_SLOTS	((u64)(TPV_KTEST_CDV_EXT_MB) * 1024u / (u64)(TPV_KTEST_TPV_EXT_KB))	/* 16 */
#define TPV_KTEST_N_DATA_EXTS	4u		/* data CDV_extents: indices 1..4 */
#define TPV_KTEST_VIRT_SIZE	((u64)64u << 20)	/* 64 MB virtual volume size */
/* CDV buffer: L1 at [0, E) + data extents 1..4 at [E, 5E). */
#define TPV_KTEST_CDV_BUF_SZ	((u64)(TPV_KTEST_N_DATA_EXTS + 1u) * ((u64)(TPV_KTEST_CDV_EXT_MB) << 20))

/* ── Global test context ────────────────────────────────────────────────── */

/*
 * Active only while nvmeibc_tpv_run_selftests() is executing.
 * Serialised by g_tc_lock; the proc read handler holds it for the entire
 * selftest run so that concurrent reads do not interfere.
 */
static struct nvmeibc_tpv_ktest_ctx {
	/* CDV backing store (vmalloc'd, used by sync_read/write) */
	void    *cdv_buf;
	u64      cdv_len;

	/* TOMA simulation: extent index counter for cdv_alloc_extent */
	u64      next_extent_idx;	/* next data extent to hand out (start: 1) */
	u64      max_extents;		/* maximum data extents available */

	/* TOMA simulation: forced error injection */
	bool     inject_full;		/* return NVMEIBC_CDV_ALLOC_CDV_FULL */
	bool     inject_wrong_gen;	/* return NVMEIBC_CDV_ALLOC_WRONG_GEN */
	u64      wrong_gen_toma_val;	/* allocator_generation in WRONG_GEN response */

	/* TOMA simulation: extent list for recovery (vmalloc'd array) */
	u64     *recovery_extents;	/* extent indices to return from cdv_list_extents */
	u64      recovery_count;	/* number of entries in recovery_extents */
} g_tc;

static DEFINE_MUTEX(g_tc_lock);	/* serialises concurrent selftest invocations */

/* ── CDV sync I/O test hooks ─────────────────────────────────────────────── */

/*
 * The real CDV transport functions live in nvmeibc_tpv_cdv.c.  During kernel
 * self-tests the TPV structs have cdv_vol == NULL (no real block device), so
 * the real functions would fail.  nvmeibc_tpv_cdv.c exposes two function-
 * pointer hooks; we set them to the in-memory helpers below for the duration
 * of the test run and clear them afterward.
 */
extern int (*nvmeibc_tpv_cdv_test_sync_read_fn)(struct nvmeibc_tpv *tpv,
						  u64 cdv_offset, void *buf,
						  u64 len);
extern int (*nvmeibc_tpv_cdv_test_sync_write_fn)(struct nvmeibc_tpv *tpv,
						   u64 cdv_offset,
						   const void *buf, u64 len);

static int ktest_cdv_sync_read(struct nvmeibc_tpv *tpv,
			       u64 cdv_offset, void *buf, u64 len)
{
	if (!g_tc.cdv_buf)
		return -ENOTSUPP;
	if (cdv_offset + len > g_tc.cdv_len)
		return -ERANGE;
	memcpy(buf, (char *)g_tc.cdv_buf + cdv_offset, len);
	return 0;
}

static int ktest_cdv_sync_write(struct nvmeibc_tpv *tpv,
				u64 cdv_offset, const void *buf, u64 len)
{
	if (!g_tc.cdv_buf)
		return -ENOTSUPP;
	if (cdv_offset + len > g_tc.cdv_len)
		return -ERANGE;
	memcpy((char *)g_tc.cdv_buf + cdv_offset, buf, len);
	return 0;
}

/*
 * CDV extent allocation from TOMA (IB admin channel).
 * Simulates the TOMA response: CDV_FULL / WRONG_GEN injection, or hands
 * out the next sequential extent index.
 *
 * Called from nvmeibc_tpv_allocator.c:nvmeibc_tpv_cdv_alloc_work_fn().
 * The cdv parameter is unused in tests (tests set toma_id to "" so the
 * work function bails before reaching this for most tests, or provide a
 * fake that doesn't need dereferencing here).
 */
int nvmeibc_ib_admin_cdv_alloc_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_alloc_req   *req,
	struct nvmeibc_cdv_alloc_resp        *resp)
{
	memset(resp, 0, sizeof(*resp));
	resp->req_id = req->req_id;

	if (g_tc.inject_full) {
		resp->status = NVMEIBC_CDV_ALLOC_CDV_FULL;
		return 0;
	}
	if (g_tc.inject_wrong_gen) {
		resp->status             = NVMEIBC_CDV_ALLOC_WRONG_GEN;
		resp->allocator_generation = g_tc.wrong_gen_toma_val;
		return 0;
	}
	if (g_tc.next_extent_idx > g_tc.max_extents) {
		resp->status = NVMEIBC_CDV_ALLOC_CDV_FULL;
		return 0;
	}

	resp->extent_index         = g_tc.next_extent_idx++;
	resp->allocator_generation = 1;
	resp->status               = NVMEIBC_CDV_ALLOC_OK;
	return 0;
}
EXPORT_SYMBOL(nvmeibc_ib_admin_cdv_alloc_extent);

/*
 * CDV extent return to TOMA: always succeeds in tests.
 * Called from nvmeibc_tpv_allocator.c:tpv_drain_pending_returns().
 */
int nvmeibc_ib_admin_cdv_free_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_free_req    *req)
{
	return 0;
}
EXPORT_SYMBOL(nvmeibc_ib_admin_cdv_free_extent);

/*
 * CDV extent list query for recovery: returns the pre-configured test list.
 * The caller (nvmeibc_tpv_recovery.c) vfree()s *out_indices.
 *
 * Called from nvmeibc_tpv_recovery.c:nvmeibc_tpv_recovery().
 */
int nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
				       const char *toma_id,
				       const char *tpv_uuid,
				       u64 **out_indices,
				       u64 *out_count)
{
	u64 *arr;

	if (!g_tc.recovery_extents || g_tc.recovery_count == 0) {
		*out_indices = NULL;
		*out_count   = 0;
		return 0;
	}

	arr = vmalloc(g_tc.recovery_count * sizeof(u64));
	if (!arr)
		return -ENOMEM;

	memcpy(arr, g_tc.recovery_extents, g_tc.recovery_count * sizeof(u64));
	*out_indices = arr;
	*out_count   = g_tc.recovery_count;
	return 0;
}
EXPORT_SYMBOL(nvmeibc_ib_admin_cdv_list_extents);

/* ── Test helper: output accumulator ────────────────────────────────────── */

struct tpv_ktest_output {
	char   *buf;
	size_t  len;
	size_t  pos;
	int     failures;
};

#define KTO_ADD(kto, fmt, ...) \
	((kto)->pos += scnprintf((kto)->buf + (kto)->pos, \
				  (kto)->len - (kto)->pos, fmt, ##__VA_ARGS__))

#define KTO_PASS(kto, name) \
	KTO_ADD(kto, "  %-24s PASS\n", name)

#define KTO_FAIL(kto, name, fmt, ...) \
	do { \
		KTO_ADD(kto, "  %-24s FAIL: " fmt "\n", name, ##__VA_ARGS__); \
		(kto)->failures++; \
	} while (0)

/* ── Test helper: TPV construction ──────────────────────────────────────── */

/*
 * tpv_ktest_create — allocate and initialise a minimal struct nvmeibc_tpv
 * for use in self-tests.
 *
 * Differences from a production attach:
 *   • No gendisk or request_queue is registered.
 *   • low_watermark is set to 0, preventing proactive CDV_extent pre-fetch
 *     (cdv_alloc_work is only armed when the free pool is empty).
 *   • allocator_toma_id is set to the empty string by default so that
 *     cdv_alloc_work_fn bails immediately without accessing cdv_vol.
 *   • cdv_vol is NULL; tests that need the work function to proceed past
 *     the toma_id check set a non-empty toma_id instead.
 */
static struct nvmeibc_tpv *tpv_ktest_create(void)
{
	struct nvmeibc_tpv           *tpv;
	struct nvmeibc_tpv_allocator *alloc;

	tpv = kzalloc(sizeof(*tpv), GFP_KERNEL);
	if (!tpv)
		return NULL;

	alloc = &tpv->allocator;

	xa_init(&alloc->extent_map);
	spin_lock_init(&alloc->lock);

	alloc->tpv_extent_size_kb    = TPV_KTEST_TPV_EXT_KB;
	alloc->cdv_extent_size_mb    = TPV_KTEST_CDV_EXT_MB;
	alloc->allocator_size_gb     = TPV_KTEST_ALLOC_GB;
	alloc->virtual_extents_total = TPV_KTEST_VIRT_SIZE /
				       ((u64)TPV_KTEST_TPV_EXT_KB << 10);

	INIT_LIST_HEAD(&alloc->cdv_extent_list);
	alloc->cdv_extents_count       = 0;
	INIT_LIST_HEAD(&alloc->free_tpv_extents);
	alloc->free_tpv_extent_count   = 0;
	INIT_LIST_HEAD(&alloc->pending_return_list);
	alloc->low_watermark           = 0;	/* disable proactive pre-fetch */

	spin_lock_init(&tpv->allocator_id_lock);
	tpv->allocator_toma_id[0] = '\0';	/* empty → cdv_alloc_work bails early */
	tpv->allocator_generation = 0;

	INIT_WORK(&tpv->cdv_alloc_work, nvmeibc_tpv_cdv_alloc_work_fn);
	atomic_set(&tpv->cdv_alloc_pending, 0);

	INIT_WORK(&tpv->persist_work, nvmeibc_tpv_persist_work_fn);
	spin_lock_init(&tpv->persist_lock);
	tpv->dirty = false;

	spin_lock_init(&tpv->pending_bio_lock);
	/* pending_bios zeroed by kzalloc → head = tail = NULL (empty) */

	INIT_LIST_HEAD(&tpv->list_node);
	atomic_set(&tpv->state, TPV_ATTACHED);

	tpv->virtual_size = TPV_KTEST_VIRT_SIZE;
	tpv->cdv_vol      = NULL;

	strncpy(tpv->tpv_uuid, "ktest-tpv-uuid-00", NVMEIBC_BD_UUID_LEN - 1);
	strncpy(tpv->tpv_name, "ktest-tpv", sizeof(tpv->tpv_name) - 1);

	return tpv;
}

/*
 * tpv_ktest_seed_pool — inject n_extents data CDV_extents directly into
 * tpv->allocator without going through the TOMA work path.
 *
 * Each extent gets extent_index = 1 .. n_extents and n_slots free slots.
 * Mirrors the work done by tpv_on_cdv_alloc_ok() (static in the allocator).
 */
static int tpv_ktest_seed_pool(struct nvmeibc_tpv *tpv, u64 n_extents)
{
	struct nvmeibc_tpv_allocator *alloc = &tpv->allocator;
	u64 E = (u64)TPV_KTEST_CDV_EXT_MB << 20;	/* bytes per CDV_extent */
	u64 T = (u64)TPV_KTEST_TPV_EXT_KB << 10;	/* bytes per TPV_extent */
	u64 A = (u64)TPV_KTEST_ALLOC_GB << 30;		/* metadata region (= 0) */
	u64 ei, s;

	for (ei = 1; ei <= n_extents; ei++) {
		struct nvmeibc_cdv_extent_ref *ref;

		ref = kzalloc(sizeof(*ref), GFP_KERNEL);
		if (!ref)
			return -ENOMEM;
		ref->extent_index    = ei;
		ref->allocated_count = 0;
		INIT_LIST_HEAD(&ref->node);
		list_add_tail(&ref->node, &alloc->cdv_extent_list);
		alloc->cdv_extents_count++;

		for (s = 0; s < TPV_KTEST_N_SLOTS; s++) {
			struct nvmeibc_tpv_free_slot *fs;

			fs = kzalloc(sizeof(*fs), GFP_KERNEL);
			if (!fs)
				return -ENOMEM;
			fs->phys_offset      = A + ei * E + s * T;
			fs->cdv_extent_index = ei;
			INIT_LIST_HEAD(&fs->node);
			list_add_tail(&fs->node, &alloc->free_tpv_extents);
			alloc->free_tpv_extent_count++;
		}
	}
	return 0;
}

/*
 * tpv_ktest_destroy — quiesce and free a test TPV.
 *
 * Order matters:
 *   1. Signal TPV_DETACHING so both work handlers exit immediately.
 *   2. cancel_work_sync() to drain any in-flight work.
 *   3. rcu_barrier() to flush pending kfree_rcu callbacks from free_extent.
 *   4. Release xarray entries, ref lists, and free-slot list.
 *   5. kfree the TPV itself.
 */
static void tpv_ktest_destroy(struct nvmeibc_tpv *tpv)
{
	struct nvmeibc_tpv_allocator    *alloc = &tpv->allocator;
	struct nvmeibc_cdv_extent_ref   *ref, *rtmp;
	struct nvmeibc_tpv_extent_entry *entry;
	unsigned long idx;

	atomic_set(&tpv->state, TPV_DETACHING);
	cancel_work_sync(&tpv->cdv_alloc_work);
	cancel_work_sync(&tpv->persist_work);

	/*
	 * Drain any kfree_rcu callbacks queued by free_extent() before we
	 * iterate the xarray.  Without this, concurrent RCU callbacks might
	 * access memory we are about to free.
	 */
	rcu_barrier();

	/* Free xarray entries that were not freed by free_extent(). */
	xa_for_each(&alloc->extent_map, idx, entry)
		kfree(entry);
	xa_destroy(&alloc->extent_map);

	/* Free active CDV_extent refs. */
	list_for_each_entry_safe(ref, rtmp, &alloc->cdv_extent_list, node) {
		list_del(&ref->node);
		kfree(ref);
	}

	/* Free extent refs pending return to TOMA. */
	list_for_each_entry_safe(ref, rtmp, &alloc->pending_return_list, node) {
		list_del(&ref->node);
		kfree(ref);
	}

	/* Free all free_slot entries. */
	nvmeibc_tpv_free_slots_list(&alloc->free_tpv_extents);

	kfree(tpv);
}

/* ── Self-test functions ─────────────────────────────────────────────────── */

/*
 * tpv_ktest_alloc_free — basic xarray alloc / free cycle.
 *
 * Verifies:
 *   • alloc_extent() returns 0 and provides a valid entry for each call.
 *   • stat_tpv_alloc_ok increments correctly.
 *   • free_extent() returns 0 and removes the xarray entry.
 *   • stat_tpv_free_ok increments correctly.
 *   • free_extent() on an unmapped index returns -ENOENT.
 */
static void tpv_ktest_alloc_free(struct tpv_ktest_output *kto)
{
	struct nvmeibc_tpv           *tpv;
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_tpv_extent_entry *entry0 = NULL, *entry1 = NULL;
	u64 expect_phys0, expect_phys1;
	int rc;

	tpv = tpv_ktest_create();
	if (!tpv) {
		KTO_FAIL(kto, "alloc_free", "kzalloc failed");
		return;
	}
	alloc = &tpv->allocator;

	/* Seed 1 CDV_extent (16 free slots). */
	if (tpv_ktest_seed_pool(tpv, 1) < 0) {
		KTO_FAIL(kto, "alloc_free", "seed_pool failed");
		goto done;
	}

	/*
	 * Slots are served FIFO from free_tpv_extents (list_first_entry).
	 * After seeding extent 1, slot 0 is at the head.
	 *   phys0 = 0 + 1*1MB + 0*64KB = 1MB
	 *   phys1 = 0 + 1*1MB + 1*64KB = 1MB + 64KB
	 */
	expect_phys0 = (u64)1 << 20;
	expect_phys1 = ((u64)1 << 20) + ((u64)64 << 10);

	/* First alloc. */
	rc = nvmeibc_tpv_alloc_extent(tpv, 0, &entry0);
	if (rc != 0) {
		KTO_FAIL(kto, "alloc_free", "first alloc_extent rc=%d", rc);
		goto done;
	}
	if (!entry0 || entry0->phys_offset != expect_phys0) {
		KTO_FAIL(kto, "alloc_free",
			 "entry0 phys 0x%llx != expected 0x%llx",
			 entry0 ? entry0->phys_offset : 0ULL, expect_phys0);
		goto done;
	}
	if (atomic64_read(&alloc->stat_tpv_alloc_ok) != 1) {
		KTO_FAIL(kto, "alloc_free", "stat_tpv_alloc_ok != 1");
		goto done;
	}

	/* Second alloc (different virt_idx to avoid xarray key collision). */
	rc = nvmeibc_tpv_alloc_extent(tpv, 1, &entry1);
	if (rc != 0 || !entry1 || entry1->phys_offset != expect_phys1) {
		KTO_FAIL(kto, "alloc_free",
			 "second alloc_extent rc=%d phys=0x%llx", rc,
			 entry1 ? entry1->phys_offset : 0ULL);
		goto done;
	}

	/* Free virt_idx=1 while virt_idx=0 is still allocated (no pending). */
	rc = nvmeibc_tpv_free_extent(tpv, 1);
	if (rc != 0) {
		KTO_FAIL(kto, "alloc_free", "free_extent(1) rc=%d", rc);
		goto done;
	}
	if (atomic64_read(&alloc->stat_tpv_free_ok) != 1) {
		KTO_FAIL(kto, "alloc_free", "stat_tpv_free_ok != 1 after first free");
		goto done;
	}
	/* xarray entry for virt_idx=1 must be gone. */
	if (xa_load(&alloc->extent_map, 1) != NULL) {
		KTO_FAIL(kto, "alloc_free", "xa_load(1) non-NULL after free");
		goto done;
	}

	/* -ENOENT on repeat free. */
	rc = nvmeibc_tpv_free_extent(tpv, 1);
	if (rc != -ENOENT) {
		KTO_FAIL(kto, "alloc_free",
			 "second free_extent(1) returned %d, want -ENOENT", rc);
		goto done;
	}

	KTO_PASS(kto, "alloc_free");
done:
	tpv_ktest_destroy(tpv);
}

/*
 * tpv_ktest_persist — flush_state / load_state roundtrip.
 *
 * Verifies:
 *   • flush_state() serialises the xarray to the CDV buffer.
 *   • load_state() on a fresh TPV reconstructs the identical xarray
 *     (same virtual-extent → phys-offset mapping) and the correct
 *     free-slot pool (mapped slots consumed; unmapped slots free).
 */
static void tpv_ktest_persist(struct tpv_ktest_output *kto)
{
	struct nvmeibc_tpv *tpv = NULL, *tpv2 = NULL;
	struct nvmeibc_tpv_extent_entry *e;
	u64 expect_phys;	/* phys of virt_idx=0, slot 0 of extent 1 */
	int rc;

	/* The CDV buffer is set up by the caller; zero it to start fresh. */
	memset(g_tc.cdv_buf, 0, g_tc.cdv_len);

	tpv = tpv_ktest_create();
	if (!tpv) {
		KTO_FAIL(kto, "persist", "create failed");
		return;
	}

	if (tpv_ktest_seed_pool(tpv, 1) < 0) {
		KTO_FAIL(kto, "persist", "seed_pool failed");
		goto done;
	}

	/* Alloc virt_idx=0 → slot 0 of extent 1 → phys = 1 MB. */
	expect_phys = (u64)1 << 20;
	rc = nvmeibc_tpv_alloc_extent(tpv, 0, &e);
	if (rc != 0 || !e || e->phys_offset != expect_phys) {
		KTO_FAIL(kto, "persist", "alloc_extent rc=%d phys=0x%llx",
			 rc, e ? e->phys_offset : 0ULL);
		goto done;
	}

	/*
	 * Explicitly flush state to the CDV buffer.  Clear dirty first so
	 * the background persist_work (which may have been armed by
	 * alloc_extent) does not race with our direct call.
	 */
	spin_lock(&tpv->persist_lock);
	tpv->dirty = false;
	spin_unlock(&tpv->persist_lock);
	cancel_work_sync(&tpv->persist_work);

	rc = nvmeibc_tpv_flush_state(tpv);
	if (rc != 0) {
		KTO_FAIL(kto, "persist", "flush_state rc=%d", rc);
		goto done;
	}

	/* Tear down the first TPV (CDV buffer retains the flushed state). */
	tpv_ktest_destroy(tpv);
	tpv = NULL;

	/* Load state into a fresh TPV — no pool seeding, load_state does it. */
	tpv2 = tpv_ktest_create();
	if (!tpv2) {
		KTO_FAIL(kto, "persist", "create (phase 2) failed");
		return;
	}

	rc = nvmeibc_tpv_load_state(tpv2);
	if (rc != 0) {
		KTO_FAIL(kto, "persist", "load_state rc=%d", rc);
		goto done;
	}

	/* Verify xarray reconstruction. */
	e = xa_load(&tpv2->allocator.extent_map, 0);
	if (!e) {
		KTO_FAIL(kto, "persist", "xa_load(0) NULL after load_state");
		goto done;
	}
	if (e->phys_offset != expect_phys) {
		KTO_FAIL(kto, "persist",
			 "loaded phys 0x%llx != flushed 0x%llx",
			 e->phys_offset, expect_phys);
		goto done;
	}
	if (e->cdv_extent_index != 1) {
		KTO_FAIL(kto, "persist",
			 "loaded cdv_extent_index %llu != 1",
			 e->cdv_extent_index);
		goto done;
	}

	/*
	 * Verify free pool: slot 0 of extent 1 is used; slots 1..15 are free.
	 * free_tpv_extent_count should be 15.
	 */
	if (tpv2->allocator.free_tpv_extent_count != TPV_KTEST_N_SLOTS - 1) {
		KTO_FAIL(kto, "persist",
			 "free_tpv_extent_count %llu, want %llu",
			 tpv2->allocator.free_tpv_extent_count,
			 TPV_KTEST_N_SLOTS - 1);
		goto done;
	}

	KTO_PASS(kto, "persist");
done:
	if (tpv)
		tpv_ktest_destroy(tpv);
	if (tpv2)
		tpv_ktest_destroy(tpv2);
}

/*
 * tpv_ktest_pool_exhaustion — exhaust all free slots, verify -EAGAIN.
 *
 * Verifies:
 *   • After allocating all TPV_KTEST_N_SLOTS slots, alloc_extent returns
 *     -EAGAIN.
 *   • stat_tpv_alloc_eagain increments.
 *   • Freeing one slot restores pool to 1 slot.
 *   • A subsequent alloc succeeds.
 */
static void tpv_ktest_pool_exhaustion(struct tpv_ktest_output *kto)
{
	struct nvmeibc_tpv           *tpv;
	struct nvmeibc_tpv_allocator *alloc;
	struct nvmeibc_tpv_extent_entry *entry;
	u64 v;
	int rc;

	memset(g_tc.cdv_buf, 0, g_tc.cdv_len);

	tpv = tpv_ktest_create();
	if (!tpv) {
		KTO_FAIL(kto, "pool_exhaustion", "create failed");
		return;
	}
	alloc = &tpv->allocator;

	if (tpv_ktest_seed_pool(tpv, 1) < 0) {
		KTO_FAIL(kto, "pool_exhaustion", "seed_pool failed");
		goto done;
	}

	/* Exhaust the pool: alloc all 16 slots. */
	for (v = 0; v < TPV_KTEST_N_SLOTS; v++) {
		rc = nvmeibc_tpv_alloc_extent(tpv, v, &entry);
		if (rc != 0) {
			KTO_FAIL(kto, "pool_exhaustion",
				 "alloc_extent(%llu) rc=%d (expected 0)", v, rc);
			goto done;
		}
	}

	if (alloc->free_tpv_extent_count != 0) {
		KTO_FAIL(kto, "pool_exhaustion",
			 "free_tpv_extent_count %llu after full alloc (want 0)",
			 alloc->free_tpv_extent_count);
		goto done;
	}

	/* Next alloc must return -EAGAIN. */
	rc = nvmeibc_tpv_alloc_extent(tpv, TPV_KTEST_N_SLOTS, &entry);
	if (rc != -EAGAIN) {
		KTO_FAIL(kto, "pool_exhaustion",
			 "alloc past full returned %d (want -EAGAIN)", rc);
		goto done;
	}
	if (atomic64_read(&alloc->stat_tpv_alloc_eagain) < 1) {
		KTO_FAIL(kto, "pool_exhaustion", "stat_tpv_alloc_eagain not incremented");
		goto done;
	}

	/*
	 * Free one slot (virt_idx=0) while others remain allocated so the
	 * extent stays on cdv_extent_list (allocated_count = 15 after free).
	 * The slot returns to free_tpv_extents immediately.
	 */
	rc = nvmeibc_tpv_free_extent(tpv, 0);
	if (rc != 0) {
		KTO_FAIL(kto, "pool_exhaustion", "free_extent(0) rc=%d", rc);
		goto done;
	}

	/*
	 * Allocating a new virt_idx (beyond the ones already in xarray)
	 * should now succeed.  virt_idx = N_SLOTS is unused (the failed
	 * -EAGAIN alloc did not store anything).
	 */
	rc = nvmeibc_tpv_alloc_extent(tpv, TPV_KTEST_N_SLOTS, &entry);
	if (rc != 0) {
		KTO_FAIL(kto, "pool_exhaustion",
			 "alloc after free returned %d (want 0)", rc);
		goto done;
	}

	KTO_PASS(kto, "pool_exhaustion");
done:
	tpv_ktest_destroy(tpv);
}

/*
 * tpv_ktest_double_free — free the same virt_idx twice, expect -ENOENT.
 *
 * Verifies:
 *   • First free_extent() returns 0.
 *   • Second free_extent() on the same virt_idx returns -ENOENT.
 */
static void tpv_ktest_double_free(struct tpv_ktest_output *kto)
{
	struct nvmeibc_tpv           *tpv;
	struct nvmeibc_tpv_extent_entry *entry;
	int rc;

	memset(g_tc.cdv_buf, 0, g_tc.cdv_len);

	tpv = tpv_ktest_create();
	if (!tpv) {
		KTO_FAIL(kto, "double_free", "create failed");
		return;
	}

	if (tpv_ktest_seed_pool(tpv, 1) < 0) {
		KTO_FAIL(kto, "double_free", "seed_pool failed");
		goto done;
	}

	/* Alloc virt_idx=7 (arbitrary). */
	rc = nvmeibc_tpv_alloc_extent(tpv, 7, &entry);
	if (rc != 0) {
		KTO_FAIL(kto, "double_free", "alloc_extent rc=%d", rc);
		goto done;
	}

	/* First free: must succeed. */
	rc = nvmeibc_tpv_free_extent(tpv, 7);
	if (rc != 0) {
		KTO_FAIL(kto, "double_free", "first free_extent rc=%d", rc);
		goto done;
	}

	/* Second free: must return -ENOENT. */
	rc = nvmeibc_tpv_free_extent(tpv, 7);
	if (rc != -ENOENT) {
		KTO_FAIL(kto, "double_free",
			 "second free_extent returned %d (want -ENOENT)", rc);
		goto done;
	}

	KTO_PASS(kto, "double_free");
done:
	tpv_ktest_destroy(tpv);
}

/*
 * tpv_ktest_recovery — orphaned CDV_extent adoption.
 *
 * Verifies:
 *   • When load_state() reads an empty CDV buffer, 0 slots are in the pool.
 *   • When nvmeibc_ib_admin_cdv_list_extents returns extent_index=1 but
 *     that extent is absent from the in-memory tree, nvmeibc_tpv_recovery()
 *     adopts it: adds a CDV_extent_ref and TPV_KTEST_N_SLOTS free slots.
 */
static void tpv_ktest_recovery(struct tpv_ktest_output *kto)
{
	struct nvmeibc_tpv           *tpv;
	struct nvmeibc_tpv_allocator *alloc;
	u64 orphan_idx = 1;
	int rc;

	/* Zero CDV buffer → empty L1 tree → load_state maps nothing. */
	memset(g_tc.cdv_buf, 0, g_tc.cdv_len);

	/*
	 * Configure the list_extents stub to report extent_index=1 as
	 * allocated to this TPV on the TOMA side.
	 */
	g_tc.recovery_extents = &orphan_idx;
	g_tc.recovery_count   = 1;

	tpv = tpv_ktest_create();
	if (!tpv) {
		KTO_FAIL(kto, "recovery", "create failed");
		goto cleanup;
	}
	alloc = &tpv->allocator;

	/*
	 * Give the TPV a non-empty toma_id so that nvmeibc_tpv_recovery()
	 * proceeds past its early-exit guard and calls list_extents.
	 */
	strncpy(tpv->allocator_toma_id, "test-toma-001",
		sizeof(tpv->allocator_toma_id) - 1);

	/* load_state: reads empty CDV buf, no mapped extents. */
	rc = nvmeibc_tpv_load_state(tpv);
	if (rc != 0) {
		KTO_FAIL(kto, "recovery", "load_state rc=%d", rc);
		goto done;
	}
	if (alloc->free_tpv_extent_count != 0) {
		KTO_FAIL(kto, "recovery",
			 "free_tpv_extent_count %llu after empty load (want 0)",
			 alloc->free_tpv_extent_count);
		goto done;
	}

	/* recovery: adopts extent_index=1 → adds N_SLOTS free slots. */
	rc = nvmeibc_tpv_recovery(tpv);
	if (rc != 0) {
		KTO_FAIL(kto, "recovery", "nvmeibc_tpv_recovery rc=%d", rc);
		goto done;
	}
	if (alloc->free_tpv_extent_count != TPV_KTEST_N_SLOTS) {
		KTO_FAIL(kto, "recovery",
			 "free_tpv_extent_count %llu after recovery (want %llu)",
			 alloc->free_tpv_extent_count, TPV_KTEST_N_SLOTS);
		goto done;
	}
	if (alloc->cdv_extents_count != 1) {
		KTO_FAIL(kto, "recovery",
			 "cdv_extents_count %llu after recovery (want 1)",
			 alloc->cdv_extents_count);
		goto done;
	}

	KTO_PASS(kto, "recovery");
done:
	tpv_ktest_destroy(tpv);
cleanup:
	g_tc.recovery_extents = NULL;
	g_tc.recovery_count   = 0;
}

/* ── Proc fill function ─────────────────────────────────────────────────── */

/*
 * nvmeibc_tpv_run_selftests — proc fill function for "selftest".
 *
 * Reading /proc/nvmeibc/tpv/<name>/selftest runs all five kernel self-tests
 * and writes a summary to the proc read buffer.
 *
 * arg is the nvmeibc_tpv * registered at proc creation time (unused here;
 * tests construct their own TPV instances for full isolation).
 */
ssize_t nvmeibc_tpv_run_selftests(void *arg, char *buf, size_t len)
{
	struct tpv_ktest_output kto = {
		.buf      = buf,
		.len      = len,
		.pos      = 0,
		.failures = 0,
	};

	if (!mutex_trylock(&g_tc_lock)) {
		return scnprintf(buf, len,
			"selftest already running (try again)\n");
	}

	/* Allocate the CDV backing buffer. */
	g_tc.cdv_buf = vzalloc(TPV_KTEST_CDV_BUF_SZ);
	if (!g_tc.cdv_buf) {
		mutex_unlock(&g_tc_lock);
		return scnprintf(buf, len, "selftest: vzalloc %llu B failed\n",
				 TPV_KTEST_CDV_BUF_SZ);
	}
	g_tc.cdv_len         = TPV_KTEST_CDV_BUF_SZ;
	g_tc.next_extent_idx = 1;
	g_tc.max_extents     = TPV_KTEST_N_DATA_EXTS;
	g_tc.inject_full     = false;
	g_tc.inject_wrong_gen = false;
	g_tc.recovery_extents = NULL;
	g_tc.recovery_count   = 0;

	/* Route CDV sync I/O through the in-memory test buffer. */
	nvmeibc_tpv_cdv_test_sync_read_fn  = ktest_cdv_sync_read;
	nvmeibc_tpv_cdv_test_sync_write_fn = ktest_cdv_sync_write;

	KTO_ADD(&kto,
		"TPV kernel self-tests  "
		"(A=%uGB E=%uMB T=%uKB slots=%llu):\n",
		TPV_KTEST_ALLOC_GB, TPV_KTEST_CDV_EXT_MB,
		TPV_KTEST_TPV_EXT_KB, TPV_KTEST_N_SLOTS);

	tpv_ktest_alloc_free(&kto);
	tpv_ktest_persist(&kto);
	tpv_ktest_pool_exhaustion(&kto);
	tpv_ktest_double_free(&kto);
	tpv_ktest_recovery(&kto);

#define TPV_KTEST_N_TESTS	5
	KTO_ADD(&kto, "\n");
	if (kto.failures == 0)
		KTO_ADD(&kto, "all %d tests passed\n", TPV_KTEST_N_TESTS);
	else
		KTO_ADD(&kto, "%d/%d test(s) FAILED\n",
			kto.failures, TPV_KTEST_N_TESTS);

	/* Restore production CDV sync I/O path. */
	nvmeibc_tpv_cdv_test_sync_read_fn  = NULL;
	nvmeibc_tpv_cdv_test_sync_write_fn = NULL;

	vfree(g_tc.cdv_buf);
	g_tc.cdv_buf = NULL;
	g_tc.cdv_len = 0;

	mutex_unlock(&g_tc_lock);
	return (ssize_t)kto.pos;
}
EXPORT_SYMBOL(nvmeibc_tpv_run_selftests);
