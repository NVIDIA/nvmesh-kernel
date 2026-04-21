/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_cdv.c — CDV block-layer integration for TPV.
 *
 * Implements the three CDV transport functions declared as externs in the
 * TPV core files:
 *
 *   nvmeibc_tpv_cdv_sync_read()   — synchronous read from CDV (persist.c)
 *   nvmeibc_tpv_cdv_sync_write()  — synchronous write to CDV  (persist.c)
 *   nvmeibc_tpv_cdv_submit_bio()  — async bio forwarding to CDV (io.c)
 *
 * Sync I/O path (flush_state / load_state):
 *   Allocates a fresh bio per page of the L1 buffer, sets the CDV as the
 *   target device, and waits for completion via an on-stack completion.
 *   Called from process context (work queue or attach) only.
 *
 * Async I/O path (tpv_handle_one_bio):
 *   Re-targets the user's existing bio at the CDV by updating bi_sector and
 *   bi_disk/bi_bdev, then directly invokes the CDV queue's submit function.
 *   This is the standard Linux stacking block-device pattern.
 *
 * Test hooks:
 *   nvmeibc_tpv_cdv_test_sync_read_fn and _write_fn are NULL in production.
 *   nvmeibc_tpv_test.c sets them to in-memory buffer helpers before running
 *   self-tests and clears them afterwards.  Checked under the test's g_tc_lock.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_block.h"			/* KERNEL_SECTOR_SHIFT, nvmeibc_block_get_os_api */
#include "clnt/nvmeibc_volume.h"		/* struct nvmeibc_volume */
#include "clnt/block/nvmeibc_block_api_os.h"	/* block_api_os_get_bdev, CALL_SUBMIT_BIO_FN */
#include "common/nvmeib_common_os_block_api.h"	/* KS_BIO_HAS_BI_GENDISK_PTR, bio_gendisk */

/* ── Forward declarations ────────────────────────────────────────────────── */

int  nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
				u64 cdv_offset, void *buf, u64 len);
int  nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				 u64 cdv_offset, const void *buf, u64 len);
void nvmeibc_tpv_cdv_submit_bio(struct nvmeibc_tpv *tpv,
				 struct bio *bio, u64 cdv_phys_offset);

/* ── Test hook pointers ──────────────────────────────────────────────────── */

/*
 * Set to non-NULL by nvmeibc_tpv_test.c before kernel self-tests run;
 * cleared to NULL after tests complete.  Both are NULL in production.
 * Access is serialised by g_tc_lock in nvmeibc_tpv_test.c.
 */
int (*nvmeibc_tpv_cdv_test_sync_read_fn)(struct nvmeibc_tpv *tpv,
					  u64 cdv_offset, void *buf, u64 len);
int (*nvmeibc_tpv_cdv_test_sync_write_fn)(struct nvmeibc_tpv *tpv,
					   u64 cdv_offset, const void *buf,
					   u64 len);

/* ── Synchronous bio completion helper ──────────────────────────────────── */

struct tpv_cdv_bio_sync {
	struct completion done;
	int              error;
};

#if KS_ENDIO_1ARG
static void tpv_cdv_bio_end(struct bio *bio)
{
	struct tpv_cdv_bio_sync *ctx = bio->bi_private;

	ctx->error = bio_error(bio);
	complete(&ctx->done);
}
#else
static void tpv_cdv_bio_end(struct bio *bio, int error_arg)
{
	struct tpv_cdv_bio_sync *ctx = bio->bi_private;

	ctx->error = error_arg;
	complete(&ctx->done);
}
#endif

/*
 * disk_part0_bdev — return the whole-disk struct block_device * from a gendisk.
 *
 * The shape of gendisk.part0 changed across kernel versions:
 *   5.11–~6.8 : struct block_device part0  (embedded value) → &disk->part0
 *   ≥ ~6.9    : struct block_device *part0 (pointer field)  →  disk->part0
 *
 * __builtin_choose_expr + __builtin_types_compatible_p selects the right form
 * purely from the actual type at compile time — no compat-script macro needed.
 * The non-taken branch is parsed but never evaluated; both are syntactically
 * valid expressions so the compiler does not warn about the discarded form.
 */
#define disk_part0_bdev(disk)							\
	__builtin_choose_expr(							\
		__builtin_types_compatible_p(__typeof__((disk)->part0),		\
					     struct block_device *),		\
		(disk)->part0,							\
		&(disk)->part0)

/* ── tpv_cdv_sync_io — page-by-page synchronous CDV block I/O ───────────── */

/*
 * Issue synchronous block I/O to the CDV.  The buffer (L1 tree) is
 * vzalloc'd so individual pages may not be physically contiguous; we
 * submit one bio per page to avoid scatter-gather complexity.
 *
 * Called only from process context (work queue or attach path).
 */
static int tpv_cdv_sync_io(struct nvmeibc_tpv *tpv, u64 cdv_off,
			    void *buf, u64 len, bool is_write)
{
	/*
	 * Synchronous I/O on the TPV is exclusively for the L1/L2 tree
	 * (persist.c). In split mode that tree lives on the metadata CDV;
	 * nvmeibc_tpv_meta_cdv() returns tpv->cdv_vol in single-CDV mode so
	 * the existing path is unchanged. See TPV_MetadataCDV.md §6.3.
	 */
	struct nvmeibc_volume  *cdv_vol = nvmeibc_tpv_meta_cdv(tpv);
	struct nvmeibc_os_api  *os   = nvmeibc_block_get_os_api(cdv_vol->block_dev);
	struct gendisk         *disk = os->atom.disk;
	u8                     *ptr  = (u8 *)buf;
	u64                     remaining = len;
	u64                     off       = cdv_off;

	/*
	 * We must NOT use block_api_os_get_bdev() here — that helper reads
	 * os->unsafe_self_ref.bdev_during_detach, which is NULL during normal
	 * operation (it is only populated by block_api_os_get(), a detach-path
	 * routine that cannot be called mid-I/O due to its "get() twice" guard).
	 *
	 * On kernels that address bios via bi_disk (KS_BIO_HAS_BI_GENDISK_PTR,
	 * 4.14–5.12), we set bi_disk/bi_partno directly — no bdev needed.
	 *
	 * On bi_bdev kernels (>= 5.12), disk_part0_bdev() returns the
	 * whole-disk struct block_device * from the gendisk, handling both the
	 * embedded-struct layout (5.11–~6.8) and the pointer-field layout
	 * (≥ ~6.9) transparently.
	 */
#if !KS_BIO_HAS_BI_GENDISK_PTR
	struct block_device *bdev = disk_part0_bdev(disk);
#endif

	while (remaining > 0) {
		unsigned int          page_off = (unsigned int)((uintptr_t)ptr & (PAGE_SIZE - 1));
		unsigned int          chunk    = min_t(u64, PAGE_SIZE - page_off, remaining);
		struct page          *page;
		struct bio           *bio;
		struct tpv_cdv_bio_sync ctx;
		int rv;

		page = is_vmalloc_addr(ptr) ? vmalloc_to_page(ptr) : virt_to_page(ptr);

#if KS_BIO_ALLOC_HAS_BLOCK_DEVICE
		bio = bio_alloc(bdev, 1,
				is_write ? REQ_OP_WRITE : REQ_OP_READ,
				GFP_NOIO);
#else
		bio = bio_alloc(GFP_NOIO, 1);
		if (bio) {
#if KS_BIO_HAS_BI_GENDISK_PTR
			bio->bi_disk   = disk;
			bio->bi_partno = 0;
#else
			bio_set_dev(bio, bdev);
#endif
			__SET_BI_RW(bio, is_write ? WRITE : READ);
		}
#endif
		if (unlikely(!bio))
			return -ENOMEM;

#if KS_BVEC_ITER
		bio->bi_iter.bi_sector = off >> KERNEL_SECTOR_SHIFT;
#else
		bio->bi_sector = off >> KERNEL_SECTOR_SHIFT;
#endif

		if (unlikely(bio_add_page(bio, page, chunk, page_off) != chunk)) {
			bio_put(bio);
			return -EIO;
		}

		init_completion(&ctx.done);
		ctx.error       = 0;
		bio->bi_end_io  = &tpv_cdv_bio_end;
		bio->bi_private = &ctx;

		CALL_SUBMIT_BIO_FN(os->atom.queue, disk, bio);

		wait_for_completion(&ctx.done);
		rv = ctx.error;
		bio_put(bio);

		if (rv)
			return rv;

		ptr       += chunk;
		off       += chunk;
		remaining -= chunk;
	}
	return 0;
}

/* ── Public CDV transport functions ─────────────────────────────────────── */

/*
 * nvmeibc_tpv_cdv_sync_read — synchronous read of @len bytes from the CDV
 * starting at byte offset @cdv_offset.
 *
 * Returns 0 on success, negative errno on failure.
 * -ENOTSUPP is returned only via the test hook; the real path never returns it.
 */
int nvmeibc_tpv_cdv_sync_read(struct nvmeibc_tpv *tpv,
			       u64 cdv_offset, void *buf, u64 len)
{
	if (unlikely(nvmeibc_tpv_cdv_test_sync_read_fn))
		return nvmeibc_tpv_cdv_test_sync_read_fn(tpv, cdv_offset,
							  buf, len);

	if (unlikely(!tpv->cdv_vol))
		return -ENODEV;

	return tpv_cdv_sync_io(tpv, cdv_offset, buf, len, false);
}
EXPORT_SYMBOL(nvmeibc_tpv_cdv_sync_read);

/*
 * nvmeibc_tpv_cdv_sync_write — synchronous write of @len bytes to the CDV
 * starting at byte offset @cdv_offset.
 *
 * Returns 0 on success, negative errno on failure.
 */
int nvmeibc_tpv_cdv_sync_write(struct nvmeibc_tpv *tpv,
				u64 cdv_offset, const void *buf, u64 len)
{
	if (unlikely(nvmeibc_tpv_cdv_test_sync_write_fn))
		return nvmeibc_tpv_cdv_test_sync_write_fn(tpv, cdv_offset,
							   buf, len);

	if (unlikely(!tpv->cdv_vol))
		return -ENODEV;

	return tpv_cdv_sync_io(tpv, cdv_offset, (void *)buf, len, true);
}
EXPORT_SYMBOL(nvmeibc_tpv_cdv_sync_write);

/*
 * nvmeibc_tpv_cdv_submit_bio — forward a user bio to the CDV at the given
 * physical byte offset.
 *
 * Re-targets the bio at the CDV block device (updating bi_sector and
 * bi_disk/bi_bdev) and submits it through the generic block layer so that
 * the CDV's queue limits (chunk_sectors, max_sectors, max_segments) are
 * enforced.  This is the standard Linux stacking-device pattern.
 *
 * Using submit_bio_noacct / generic_make_request instead of calling the
 * CDV's make_request directly is critical: the TPV's chunk_sectors (= TPV
 * extent size) may be larger than the CDV's internal alignment, and a bio
 * that exceeds the CDV's alignment can be split incorrectly inside the
 * CDV datapath — the split portions get independent address translations
 * and can land at wrong disk offsets.
 *
 * After this call the bio is owned by the CDV transport; the caller must
 * not access it again.
 *
 * cdv_phys_offset is already the absolute CDV byte offset (A + i×E + s×T).
 */
void nvmeibc_tpv_cdv_submit_bio(struct nvmeibc_tpv *tpv,
				 struct bio *bio,
				 u64 cdv_phys_offset)
{
	struct nvmeibc_os_api  *os;
	struct gendisk         *cdv_disk;

	if (unlikely(!tpv->cdv_vol)) {
		bio_io_error(bio);
		return;
	}

	os       = nvmeibc_block_get_os_api(tpv->cdv_vol->block_dev);
	cdv_disk = os->atom.disk;

	/*
	 * Update the bio sector to the CDV physical address.
	 * KERNEL_SECTOR_SHIFT = 9 (512-byte kernel sectors).
	 */
#if KS_BVEC_ITER
	bio->bi_iter.bi_sector = cdv_phys_offset >> KERNEL_SECTOR_SHIFT;
#else
	bio->bi_sector = cdv_phys_offset >> KERNEL_SECTOR_SHIFT;
#endif

	/*
	 * Re-target the bio at the CDV block device so that
	 * block_api_os_get_os(bio) resolves to the CDV's nvmeibc_os_api,
	 * directing I/O to the CDV's RDMA transport.
	 *
	 * For KS_BIO_HAS_BI_GENDISK_PTR (4.14–5.12): set bi_disk/bi_partno.
	 * For bi_bdev kernels (>= 5.12): disk_part0_bdev() returns the
	 * whole-disk bdev from the gendisk, handling both the embedded-struct
	 * layout (5.11–~6.8) and the pointer-field layout (>= ~6.9).
	 * We must NOT use block_api_os_get_bdev() — it reads unsafe_self_ref
	 * which is NULL except during the detach sequence.
	 */
#if KS_BIO_HAS_BI_GENDISK_PTR
	bio->bi_disk   = cdv_disk;
	bio->bi_partno = 0;
#else
	bio_set_dev(bio, disk_part0_bdev(cdv_disk));
#endif

	/*
	 * Submit through the generic block layer so the CDV's queue limits
	 * are enforced.  submit_bio_noacct (>= 5.9) / generic_make_request
	 * (< 5.9) splits the bio at the CDV's chunk_sectors and max_sectors
	 * before calling the CDV's make_request, preventing oversized bios
	 * from being mishandled by the CDV datapath.
	 *
	 * KS_REQUEST_QUEUE_HAS_REQUEST_FN tracks the same 5.9 boundary:
	 * kernels with make_request_fn use generic_make_request; kernels
	 * without it (>= 5.9) use submit_bio_noacct.
	 */
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	generic_make_request(bio);
#else
	submit_bio_noacct(bio);
#endif
}
EXPORT_SYMBOL(nvmeibc_tpv_cdv_submit_bio);
