/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 *
 * NVMESH-8849 corner-case driver for the NVMesh block device.
 *
 * The kernel-side fix this test exercises lives in SIW:
 *   - softiwarp/kernel/siw.h           (SIW_MR_MIN_PAGE_SIZE, MAX_ARRAY)
 *   - softiwarp/kernel/siw_verbs.c     (mr_min_page_4k modparam, siw_map_mr_sg)
 *   - softiwarp/kernel/siw_qp_tx.c     (paddr-based intra-page offset,
 *                                       merge-with-prev predicate,
 *                                       per-entry page_off[]/page_len[])
 *
 * When mr_min_page_4k=Y on a kernel with PAGE_SIZE > 4096 (e.g. 64 KiB
 * ARM64 kernels), NVMesh's nvmeib_init_fast_reg() picks a 4 KiB MR page
 * shift from ffs(page_size_cap)-1 and calls ib_map_mr_sg(...) with
 * page_size=4096. The resulting PBL has sub-PAGE_SIZE entries, and the
 * SIW TX path must:
 *   1) derive every intra-page offset from the *resolved paddr*, not
 *      from sge->laddr -- two PBEs of the same SGE can land at different
 *      intra-page offsets within different system pages.
 *   2) only merge a new chunk into page_array[seg-1] when the new
 *      intra-page offset equals where the previous chunk ended (true
 *      physical contiguity), not just when they share a struct page.
 *   3) size MAX_ARRAY and the per-entry page_off[]/page_len[] arrays by
 *      SIW_MR_MIN_PAGE_SIZE so a 64 KiB FPDU built from 4 KiB PBEs
 *      still fits.
 *
 * The original symptom: NVMesh block reads after writes returned wrong
 * bytes on aarch64 GB300 nodes (PAGE_SIZE=64 KiB, mr_min_page_4k=Y),
 * caught by nvmeibc_check_metadata_read_cmd's EDIC verification.
 *
 * This program reproduces (or, post-fix, confirms the absence of) that
 * bug entirely from user space by driving I/O through /dev/nvmesh/<vol>
 * with O_DIRECT and verifying the round-tripped bytes.
 *
 * How the corner cases get into SIW
 * ---------------------------------
 * pread/pwrite/preadv/pwritev with O_DIRECT on the block device builds
 * a bio whose bvecs carry the (page, offset, length) tuple from the
 * user buffer. The NVMesh client maps that bio into an SGL and calls
 * ib_map_mr_sg(..., page_size=4096), so the *user-buffer alignment*
 * determines how the resulting PBL is laid out. By choosing buffer
 * offsets at every 4 KiB stride within a 64 KiB system page, we
 * generate PBEs at every intra-page offset; by using preadv/pwritev
 * with multiple disjoint iovecs we generate multi-SGE I/Os that
 * exercise the SGE-boundary path (siw_pages_for_bytes, siw_0copy_tx's
 * cross-SGE accounting).
 *
 * **DESTRUCTIVE**: this test issues plain writes to the device. Pass a
 * scratch volume.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>

#define LBS_FALLBACK    4096u
#define BUF_DEFAULT     (32ull << 20)   /* 32 MiB */
#define DEV_OFF_DEFAULT (1ull << 20)    /* 1 MiB */
#define MAX_IOV         256             /* matches SIW_MAX_SGE_PBL */

#define POISON_BYTE     0xAAu

struct ctx {
	int       fd;
	void *    buf;
	size_t    buf_size;
	size_t    half;           /* offset of dst region within buf */
	uint8_t * src;            /* first half of buf */
	uint8_t * dst;            /* second half of buf */
	size_t    page_size;      /* system PAGE_SIZE */
	size_t    lbs;            /* device logical block size */
	uint64_t  dev_off_base;
	uint64_t  dev_size;
	int       verbose;
	unsigned  seed;
	int       pass;
	int       fail;
	int       skipped;
};

/* ------------------------------------------------------------------ */
/* deterministic byte pattern                                          */
/* ------------------------------------------------------------------ */

/*
 * Mix a per-test id and a per-byte global index so an off-by-one in
 * the TX path (e.g. ship paddr_off instead of laddr_off) produces a
 * clearly different byte. Pure inline arithmetic so the compiler can
 * vectorize the fill/verify loops if it wants.
 */
static inline uint8_t pattern_byte(uint32_t test_id, uint64_t byte_idx)
{
	uint64_t v = byte_idx + ((uint64_t)test_id << 32);
	v ^= v >> 17;
	v *= 0xed5ad4bbull;
	v ^= v >> 11;
	v *= 0xac4c1b51ull;
	v ^= v >> 15;
	return (uint8_t)v;
}

static void fill_pattern(uint8_t *p, size_t n, uint32_t test_id,
			 uint64_t global_base)
{
	for (size_t i = 0; i < n; i++)
		p[i] = pattern_byte(test_id, global_base + i);
}

static bool verify_pattern(const uint8_t *p, size_t n, uint32_t test_id,
			   uint64_t global_base, size_t *bad_off,
			   uint8_t *exp, uint8_t *got)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t e = pattern_byte(test_id, global_base + i);
		if (p[i] != e) {
			if (bad_off) *bad_off = i;
			if (exp)     *exp     = e;
			if (got)     *got     = p[i];
			return false;
		}
	}
	return true;
}

static void fill_pattern_vec(const struct iovec *iov, int n, uint32_t test_id)
{
	uint64_t global = 0;
	for (int i = 0; i < n; i++) {
		fill_pattern(iov[i].iov_base, iov[i].iov_len, test_id, global);
		global += iov[i].iov_len;
	}
}

static bool verify_pattern_vec(const struct iovec *iov, int n,
			       uint32_t test_id, uint64_t *bad_global,
			       int *bad_iov, size_t *bad_iov_off,
			       uint8_t *exp, uint8_t *got)
{
	uint64_t global = 0;
	for (int i = 0; i < n; i++) {
		size_t bad;
		if (!verify_pattern(iov[i].iov_base, iov[i].iov_len,
				    test_id, global, &bad, exp, got)) {
			if (bad_global)  *bad_global  = global + bad;
			if (bad_iov)     *bad_iov     = i;
			if (bad_iov_off) *bad_iov_off = bad;
			return false;
		}
		global += iov[i].iov_len;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* small diagnostics                                                   */
/* ------------------------------------------------------------------ */

static void diag_iov(const char *which, const struct iovec *iov, int n,
		     size_t page_size)
{
	fprintf(stderr, "  %s iov (%d entries):\n", which, n);
	for (int i = 0; i < n; i++) {
		uintptr_t a = (uintptr_t)iov[i].iov_base;
		fprintf(stderr,
			"    [%2d] base=0x%016lx page_off=0x%05lx len=%zu (0x%zx)\n",
			i, (unsigned long)a,
			(unsigned long)(a & (page_size - 1)),
			iov[i].iov_len, iov[i].iov_len);
	}
}

/* ------------------------------------------------------------------ */
/* I/O wrappers                                                        */
/* ------------------------------------------------------------------ */

/*
 * O_DIRECT pwrite/pread are supposed to either fully complete or
 * return -1. Treat any partial completion as an outright failure --
 * we don't want a half-written test region to mask a real corruption
 * on the read-back.
 */
static ssize_t do_pwrite(int fd, const void *buf, size_t n, off_t off)
{
	ssize_t rv = pwrite(fd, buf, n, off);
	if (rv < 0) return rv;
	if ((size_t)rv != n) { errno = EIO; return -1; }
	return rv;
}

static ssize_t do_pread(int fd, void *buf, size_t n, off_t off)
{
	ssize_t rv = pread(fd, buf, n, off);
	if (rv < 0) return rv;
	if ((size_t)rv != n) { errno = EIO; return -1; }
	return rv;
}

static ssize_t do_pwritev(int fd, const struct iovec *iov, int n,
			  off_t off, size_t total)
{
	ssize_t rv = pwritev(fd, iov, n, off);
	if (rv < 0) return rv;
	if ((size_t)rv != total) { errno = EIO; return -1; }
	return rv;
}

static ssize_t do_preadv(int fd, const struct iovec *iov, int n,
			 off_t off, size_t total)
{
	ssize_t rv = preadv(fd, iov, n, off);
	if (rv < 0) return rv;
	if ((size_t)rv != total) { errno = EIO; return -1; }
	return rv;
}

/* ------------------------------------------------------------------ */
/* per-test result helpers                                             */
/* ------------------------------------------------------------------ */

static void test_pass(struct ctx *c, uint32_t id, const char *what,
		      const char *detail)
{
	c->pass++;
	if (c->verbose)
		fprintf(stderr, "[%04u] PASS  %-32s %s\n",
			id, what, detail ? detail : "");
}

static void test_fail(struct ctx *c, uint32_t id, const char *what,
		      const char *fmt, ...)
{
	c->fail++;
	fprintf(stderr, "[%04u] FAIL  %-32s ", id, what);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
}

static void test_skip(struct ctx *c, uint32_t id, const char *what,
		      const char *fmt, ...)
{
	c->skipped++;
	if (c->verbose) {
		fprintf(stderr, "[%04u] SKIP  %-32s ", id, what);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
		fprintf(stderr, "\n");
	}
}

/* ------------------------------------------------------------------ */
/* single-buffer test (pwrite + pread, contiguous src/dst regions)     */
/* ------------------------------------------------------------------ */

static bool run_single(struct ctx *c, uint32_t id, const char *what,
		       size_t src_off, size_t dst_off, size_t io_size,
		       uint64_t dev_off)
{
	if (io_size == 0 || (io_size % c->lbs) != 0) {
		test_skip(c, id, what, "size 0x%zx not LBS-aligned", io_size);
		return true;
	}
	if (src_off + io_size > c->half ||
	    dst_off + io_size > c->half) {
		test_skip(c, id, what,
			  "src=0x%zx dst=0x%zx size=0x%zx exceeds half=0x%zx",
			  src_off, dst_off, io_size, c->half);
		return true;
	}
	if (dev_off + io_size > c->dev_size) {
		test_skip(c, id, what,
			  "dev_off=0x%llx size=0x%zx exceeds dev=0x%llx",
			  (unsigned long long)dev_off, io_size,
			  (unsigned long long)c->dev_size);
		return true;
	}
	if ((dev_off % c->lbs) != 0) {
		test_skip(c, id, what,
			  "dev_off=0x%llx not LBS-aligned",
			  (unsigned long long)dev_off);
		return true;
	}

	fill_pattern(c->src + src_off, io_size, id, 0);

	if (do_pwrite(c->fd, c->src + src_off, io_size, dev_off) < 0) {
		test_fail(c, id, what,
			  "pwrite(off=0x%llx, sz=0x%zx): %s",
			  (unsigned long long)dev_off, io_size,
			  strerror(errno));
		return false;
	}

	memset(c->dst + dst_off, POISON_BYTE, io_size);

	if (do_pread(c->fd, c->dst + dst_off, io_size, dev_off) < 0) {
		test_fail(c, id, what,
			  "pread(off=0x%llx, sz=0x%zx): %s",
			  (unsigned long long)dev_off, io_size,
			  strerror(errno));
		return false;
	}

	size_t bad = 0;
	uint8_t exp = 0, got = 0;
	if (!verify_pattern(c->dst + dst_off, io_size, id, 0,
			    &bad, &exp, &got)) {
		uintptr_t sa = (uintptr_t)(c->src + src_off);
		uintptr_t da = (uintptr_t)(c->dst + dst_off);
		test_fail(c, id, what,
			  "mismatch @0x%zx exp=0x%02x got=0x%02x "
			  "src_page_off=0x%05lx dst_page_off=0x%05lx "
			  "sz=0x%zx dev_off=0x%llx",
			  bad, exp, got,
			  (unsigned long)(sa & (c->page_size - 1)),
			  (unsigned long)(da & (c->page_size - 1)),
			  io_size, (unsigned long long)dev_off);
		return false;
	}

	char detail[160];
	snprintf(detail, sizeof detail,
		 "src_po=0x%05lx dst_po=0x%05lx sz=0x%zx dev=0x%llx",
		 (unsigned long)(((uintptr_t)(c->src + src_off))
				 & (c->page_size - 1)),
		 (unsigned long)(((uintptr_t)(c->dst + dst_off))
				 & (c->page_size - 1)),
		 io_size, (unsigned long long)dev_off);
	test_pass(c, id, what, detail);
	return true;
}

/* ------------------------------------------------------------------ */
/* vectored test (pwritev + preadv)                                    */
/* ------------------------------------------------------------------ */

static bool run_vec(struct ctx *c, uint32_t id, const char *what,
		    const size_t *src_offs, const size_t *dst_offs,
		    const size_t *lens, int niov, uint64_t dev_off)
{
	if (niov <= 0 || niov > MAX_IOV) {
		test_skip(c, id, what, "bad niov=%d", niov);
		return true;
	}

	struct iovec siov[MAX_IOV], div[MAX_IOV];
	size_t total = 0;

	for (int i = 0; i < niov; i++) {
		if (lens[i] == 0 || (lens[i] % c->lbs) != 0) {
			test_skip(c, id, what,
				  "iov[%d] len=0x%zx not LBS-aligned",
				  i, lens[i]);
			return true;
		}
		if (src_offs[i] + lens[i] > c->half ||
		    dst_offs[i] + lens[i] > c->half) {
			test_skip(c, id, what,
				  "iov[%d] src=0x%zx dst=0x%zx len=0x%zx exceeds half",
				  i, src_offs[i], dst_offs[i], lens[i]);
			return true;
		}
		siov[i].iov_base = c->src + src_offs[i];
		siov[i].iov_len  = lens[i];
		div[i].iov_base  = c->dst + dst_offs[i];
		div[i].iov_len   = lens[i];
		total += lens[i];
	}
	if (dev_off + total > c->dev_size || (dev_off % c->lbs) != 0) {
		test_skip(c, id, what,
			  "dev_off=0x%llx total=0x%zx not fitting",
			  (unsigned long long)dev_off, total);
		return true;
	}

	fill_pattern_vec(siov, niov, id);

	if (do_pwritev(c->fd, siov, niov, dev_off, total) < 0) {
		test_fail(c, id, what,
			  "pwritev(off=0x%llx, total=0x%zx, n=%d): %s",
			  (unsigned long long)dev_off, total, niov,
			  strerror(errno));
		diag_iov("src", siov, niov, c->page_size);
		return false;
	}

	for (int i = 0; i < niov; i++)
		memset(div[i].iov_base, POISON_BYTE, div[i].iov_len);

	if (do_preadv(c->fd, div, niov, dev_off, total) < 0) {
		test_fail(c, id, what,
			  "preadv(off=0x%llx, total=0x%zx, n=%d): %s",
			  (unsigned long long)dev_off, total, niov,
			  strerror(errno));
		diag_iov("dst", div, niov, c->page_size);
		return false;
	}

	uint64_t bad_glob = 0;
	int bad_iov = 0;
	size_t bad_iov_off = 0;
	uint8_t exp = 0, got = 0;

	if (!verify_pattern_vec(div, niov, id, &bad_glob, &bad_iov,
				&bad_iov_off, &exp, &got)) {
		uintptr_t da = (uintptr_t)div[bad_iov].iov_base + bad_iov_off;
		test_fail(c, id, what,
			  "mismatch @global=0x%llx iov[%d]+0x%zx "
			  "exp=0x%02x got=0x%02x dst_page_off=0x%05lx "
			  "total=0x%zx n=%d dev_off=0x%llx",
			  (unsigned long long)bad_glob, bad_iov, bad_iov_off,
			  exp, got,
			  (unsigned long)(da & (c->page_size - 1)),
			  total, niov, (unsigned long long)dev_off);
		diag_iov("src", siov, niov, c->page_size);
		diag_iov("dst", div, niov, c->page_size);
		return false;
	}

	char detail[120];
	snprintf(detail, sizeof detail,
		 "n=%d total=0x%zx dev=0x%llx",
		 niov, total, (unsigned long long)dev_off);
	test_pass(c, id, what, detail);
	return true;
}

/* ------------------------------------------------------------------ */
/* Test suites                                                         */
/* ------------------------------------------------------------------ */

/*
 * Suite 1: a single contiguous I/O at every 4 KiB stride within (and
 * past) one system page, for several sizes. This generates 4 KiB PBEs
 * at every intra-system-page offset and is the primary check for the
 * (paddr & ~PAGE_MASK) vs (laddr & ~PAGE_MASK) fix in siw_tx_hdt /
 * siw_try_1seg / check_sent_fpdu_crc.
 */
static void suite_intra_page(struct ctx *c)
{
	static const size_t sizes[] = {
		4u  * 1024,
		8u  * 1024,
		12u * 1024,
		16u * 1024,
		32u * 1024,
		60u * 1024,    /* nearly a 64 KiB page */
		64u * 1024,
		68u * 1024,    /* 64 KiB + one 4 KiB PBE */
		128u * 1024,
	};
	uint32_t id = 1000;
	size_t stride = c->lbs;                       /* 4 KiB */
	size_t up_to  = c->page_size + 4u * c->lbs;   /* page + a few PBEs */

	for (size_t src_off = 0; src_off + stride <= up_to; src_off += stride) {
		/*
		 * Also vary the *destination* intra-page offset so the
		 * pread side covers the same PBE layout space on the
		 * receiver. dst_off rotates relative to src_off so the
		 * two never share the same page_off systematically.
		 */
		size_t dst_off = src_off + 2u * c->lbs;
		if (dst_off + 16u * c->lbs > c->half / 2)
			dst_off = 0;
		for (unsigned s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
			char what[64];
			snprintf(what, sizeof what,
				 "intra src_po=0x%05zx sz=%zuK",
				 src_off & (c->page_size - 1),
				 sizes[s] >> 10);
			run_single(c, id++, what, src_off, dst_off,
				   sizes[s], c->dev_off_base);
		}
	}
}

/*
 * Suite 2: I/Os that straddle one or more system page boundaries with
 * non-aligned start offsets. Forces multi-bvec bios where:
 *   - bvec[0] has a non-zero intra-page offset and length < PAGE_SIZE,
 *   - middle bvecs are full pages,
 *   - bvec[last] has intra-page offset 0 and length < PAGE_SIZE.
 *
 * Each of these bvecs then gets carved into 4 KiB PBEs by ib_map_mr_sg.
 */
static void suite_cross_page(struct ctx *c)
{
	static const struct { size_t off_K, size_K; } cases[] = {
		{   4,    64 },   /* 4K start, ends at 68K */
		{   4,   128 },   /* 3 system pages on 64K kernel */
		{  60,     8 },   /* off=60K, ends at 68K */
		{  60,    64 },
		{  60,   192 },
		{   8,  1024 },   /* 1 MiB, off=8K */
		{  16,  1024 },
		{  32,  1024 },
		{ 124,  1024 },   /* off=124K, just before next 64K boundary */
		{   4,  4096 },   /* 4 MiB, off=4K */
	};
	uint32_t id = 2000;
	for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
		char what[64];
		size_t off  = cases[i].off_K  * 1024;
		size_t size = cases[i].size_K * 1024;
		snprintf(what, sizeof what,
			 "cross off=%zuK sz=%zuK",
			 cases[i].off_K, cases[i].size_K);
		run_single(c, id++, what, off, off + size, size,
			   c->dev_off_base);
	}
}

/*
 * Suite 3: large single-buffer I/Os. NVMesh / blk-mq may split these
 * into multiple FPDUs, but each resulting FPDU still flows through
 * siw_tx_hdt with sub-PAGE_SIZE PBEs and is subject to the same
 * MAX_ARRAY / page_off / page_len bookkeeping. The test verifies the
 * full round-trip.
 */
static void suite_large(struct ctx *c)
{
	static const size_t sizes_MB[] = { 1, 2, 4, 8 };
	uint32_t id = 3000;
	for (unsigned i = 0; i < sizeof sizes_MB / sizeof sizes_MB[0]; i++) {
		size_t sz  = sizes_MB[i] * 1024 * 1024;
		size_t off = c->page_size + c->lbs; /* PAGE_SIZE + 4K */
		if (sz + off > c->half) continue;
		char what[64];
		snprintf(what, sizeof what, "large sz=%zuM off=%zuK",
			 sizes_MB[i], off >> 10);
		run_single(c, id++, what, off, off, sz, c->dev_off_base);
	}
}

/*
 * Suite 4: vectored I/Os where each iovec is exactly one 4 KiB chunk,
 * placed at a different intra-system-page offset. This is the
 * multi-SGE corollary of suite_intra_page: it exercises
 * siw_pages_for_bytes / siw_0copy_tx's SGE-boundary accounting and
 * forces every iovec to produce its own PBE at a controlled
 * intra-page offset.
 */
static void suite_vec_intra_page(struct ctx *c)
{
	static const int niovs[] = { 2, 3, 4, 8, 16, 32, 64, 128, MAX_IOV };
	uint32_t id = 4000;

	for (unsigned k = 0; k < sizeof niovs / sizeof niovs[0]; k++) {
		int n = niovs[k];
		if (n > MAX_IOV) continue;

		size_t src_off[MAX_IOV], dst_off[MAX_IOV], lens[MAX_IOV];

		/*
		 * src iov[i] starts at i*4K (so its intra-page offset
		 * cycles through every 4 KiB slot of a 64 KiB system
		 * page when n >= 16). dst iov[i] is rotated by 3 slots
		 * so src/dst hit different intra-page offsets within
		 * the same test.
		 */
		size_t base_src = 0;
		size_t base_dst = (size_t)(n + 4) * c->lbs;
		bool fits = true;
		for (int i = 0; i < n; i++) {
			src_off[i] = base_src + (size_t)i * c->lbs;
			dst_off[i] = base_dst + (size_t)((i + 3) % n) * c->lbs;
			lens[i]    = c->lbs;
			if (src_off[i] + lens[i] > c->half / 2 ||
			    dst_off[i] + lens[i] > c->half) {
				fits = false;
				break;
			}
		}
		if (!fits) continue;

		char what[64];
		snprintf(what, sizeof what, "vec_intra n=%d sz=4K-each", n);
		run_vec(c, id++, what, src_off, dst_off, lens, n,
			c->dev_off_base);
	}
}

/*
 * Suite 5: vectored I/Os with mixed iovec sizes at distinct intra-page
 * offsets. The intent is to mix:
 *   - iovecs whose len is exactly one PBE (4 KiB),
 *   - iovecs that span multiple PBEs within one system page,
 *   - iovecs that cross a system-page boundary at a non-zero offset,
 *   - iovecs at PAGE_SIZE alignment (the "easy" case).
 * Plus a final iovec aligned to 1 MiB to force a large bvec chain.
 */
static void suite_vec_mixed(struct ctx *c)
{
	struct entry { size_t off_K, len_K; };

	static const struct entry layouts[][8] = {
		{
			{   0,   4 }, {  64,  60 }, { 260,  12 }, { 524, 256 },
			{ 1024, 64 }, {   0,   0 },
		},
		{
			{   4,   4 }, {  68,   8 }, { 132,  64 }, { 200,  16 },
			{ 280, 128 }, {   0,   0 },
		},
		{
			{  60,   4 }, {  68,   4 }, {  76, 128 }, { 256, 256 },
			{ 600,  64 }, {   0,   0 },
		},
		{
			{   0,  64 }, {  64,  64 }, { 128, 128 }, { 320, 192 },
			{   0,   0 },
		},
	};
	uint32_t id = 5000;
	const size_t dst_base = 4 * 1024 * 1024; /* offset all dst by 4 MiB */

	for (unsigned r = 0; r < sizeof layouts / sizeof layouts[0]; r++) {
		size_t src_off[MAX_IOV], dst_off[MAX_IOV], lens[MAX_IOV];
		int n = 0;
		for (int i = 0; i < (int)(sizeof layouts[r] / sizeof layouts[r][0]); i++) {
			if (layouts[r][i].len_K == 0) break;
			src_off[n] = layouts[r][i].off_K * 1024;
			dst_off[n] = dst_base + layouts[r][i].off_K * 1024;
			lens[n]    = layouts[r][i].len_K * 1024;
			n++;
		}
		char what[40];
		snprintf(what, sizeof what, "vec_mixed[%u] n=%d", r, n);
		run_vec(c, id++, what, src_off, dst_off, lens, n,
			c->dev_off_base);
	}
}

/*
 * Suite 6: same-system-page, non-contiguous and out-of-order PBEs.
 *
 * pwritev/preadv with iovecs that, on a kernel with PAGE_SIZE > 4 KiB,
 * land inside the SAME system page but at non-contiguous (gap) or
 * non-monotonic (out-of-order) intra-page offsets. This is the case
 * the new `merge_with_prev` predicate in siw_tx_hdt guards: the old
 * `page_array[seg - 1] == p` check would have folded the second
 * iter's chunk into page_array[seg-1] (same struct page), and
 * siw_tcp_sendpages would either over-read past the previous PBE's
 * end (the gap case) or send wrong bytes in the wrong order (the
 * reversed case). The new
 *
 *   intra_off == page_off[seg-1] + decode(page_len[seg-1])
 *
 * predicate rejects both.
 *
 * Notes
 * -----
 * - All iovecs of a single test are placed inside one system page of
 *   the source buffer. On a kernel with PAGE_SIZE <= 4 KiB the
 *   per-iovec offsets within the page can't fit, so the corresponding
 *   cases are SKIPped; that environment doesn't reach the bug
 *   (PAGE_SIZE == PBE size, so only one PBE per page).
 *
 * - The kernel's O_DIRECT path coalesces adjacent virtually-contiguous
 *   user-space chunks into a single bvec, so an iov list like
 *   `{0,4K}, {4K,4K}` collapses to one bvec and doesn't test merging
 *   across iovec boundaries. We therefore deliberately use either
 *   gaps or non-monotonic orderings so the kernel keeps each iovec
 *   as its own bvec, which then becomes its own PBE block under
 *   ib_map_mr_sg(page_size=4 KiB).
 *
 * Worked example -- `2-gap-4K`
 * ----------------------------
 * On a 64 KiB-page kernel with mr_min_page_4k=Y:
 *
 *   pwritev iov[]      bvec list             PBL after ib_map_mr_sg
 *   ------------       -------------         ----------------------
 *   {page+0K, 4K}      (page, 0K, 4K)        PBE[0]: page+0K  size 4K  iova [0..4K)
 *   {page+8K, 4K}      (page, 8K, 4K)        PBE[1]: page+8K  size 4K  iova [4K..8K)
 *
 * The RDMA WRITE FPDU's only SGE covers mr->iova .. mr->iova + 8K.
 * siw_tx_hdt walks it twice:
 *
 *   iter1: paddr = page+0K  intra_off = 0K   plen = 4K  -> new page_array[0]
 *   iter2: paddr = page+8K  intra_off = 8K   plen = 4K
 *          merge_with_prev?
 *            seg(1) > seg_at_sge_start(0)            yes
 *            page_array[0] == p (same struct page)   yes
 *            intra_off (8K) == 0 + page_len[0] (4K)  NO  -> new page_array[1]
 *
 * With the fix: page_array = { (page, 0K, 4K), (page, 8K, 4K) } --
 * siw_tcp_sendpages emits exactly the bytes the user wrote.
 *
 * Without the fix (old `page_array[seg-1] == p` check): iter2 folds
 * into entry 0, page_len[0] grows to 8K, and siw_tcp_sendpages emits
 * 8 KiB starting at page+0K -- the second 4 KiB are the *gap* (zeros
 * from mmap, or stale bytes from earlier suites), not iov[1]'s data,
 * which is never sent. The receiver's verify trips on the second
 * 4 KiB, which is exactly what this suite is here to catch.
 *
 * - The first 4 system pages of src/dst are skipped to avoid
 *   clobbering buffers already in use by earlier suites' patterns
 *   (each test is self-contained, but spacing keeps post-mortem
 *   inspection easier).
 */
static void suite_same_page_pbl(struct ctx *c)
{
	struct slot { uint16_t off_K, len_K; };

	static const struct {
		const char *name;
		int niov;
		struct slot iov[8];
	} cases[] = {
		/* === two PBEs in the same page === */

		/* one 4 KiB gap between two 4 KiB chunks */
		{ "2-gap-4K",      2, { { 0, 4 }, {  8, 4 } } },
		/* larger gap (24 KiB) so the merge_with_prev arithmetic
		 * is well past anything a naive memcmp could excuse */
		{ "2-gap-24K",     2, { { 0, 4 }, { 28, 4 } } },
		/* same-page, reversed: prev page_off > new page_off, so
		 * the old `same-page` check would have folded a 4 KiB
		 * chunk with intra_off=0 into an entry whose
		 * page_off=8K -- silently sending 4 KiB of page-tail
		 * garbage to the peer */
		{ "2-rev",         2, { { 8, 4 }, {  0, 4 } } },
		/* reversed AND adjacent (no gap between offsets, but
		 * still presented out of order to the bio layer) */
		{ "2-rev-adj",     2, { { 4, 4 }, {  0, 4 } } },

		/* === three PBEs in the same page === */

		{ "3-gap",         3, { { 0, 4 }, {  8, 4 }, { 16, 4 } } },
		{ "3-mixed",       3, { { 16, 4 }, { 0, 4 }, {  8, 4 } } },
		{ "3-rev",         3, { { 16, 4 }, { 8, 4 }, {  0, 4 } } },

		/* === four PBEs in the same page === */

		/* fully reversed (intra-page offsets strictly
		 * descending); exercises three consecutive merge
		 * rejections in one SGE */
		{ "4-rev",         4, { { 28, 4 }, { 20, 4 },
					{  8, 4 }, {  0, 4 } } },
		/* mixed: ascending, descending, and skip-around */
		{ "4-mix",         4, { { 12, 4 }, {  4, 4 },
					{  0, 4 }, { 20, 4 } } },

		/* === wider iovecs (each iovec becomes a multi-PBE
		 * bvec under ib_map_mr_sg(page_size=4 KiB)). These
		 * cases combine an intra-bvec merge (which MUST happen)
		 * with an inter-bvec non-merge (which MUST NOT happen),
		 * so the merge_with_prev predicate has to flip the
		 * right way at the right time. === */

		{ "wide-gap-2",    2, { { 0, 8 }, { 12, 8 } } },
		{ "wide-gap-3",    3, { { 0, 8 }, { 12, 4 }, { 20, 8 } } },
		{ "wide-rev-2",    2, { { 12, 8 }, { 0, 8 } } },
	};

	uint32_t id = 6000;
	const size_t src_page_base = 4 * c->page_size;
	const size_t dst_page_base = 4 * c->page_size;

	for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
		size_t src_off[8], dst_off[8], lens[8];
		const struct slot *s = cases[k].iov;
		int n = cases[k].niov;
		bool fits = true;
		char what[64];

		snprintf(what, sizeof what, "same_page %s", cases[k].name);

		for (int i = 0; i < n; i++) {
			size_t end = ((size_t)s[i].off_K + s[i].len_K) * 1024;
			if (end > c->page_size) {
				/* Doesn't fit in one system page on this
				 * kernel -- the same-page corner case is
				 * unreachable, so SKIP rather than
				 * masquerade as a different-page test. */
				fits = false;
				break;
			}
			src_off[i] = src_page_base
				   + (size_t)s[i].off_K * 1024;
			dst_off[i] = dst_page_base
				   + (size_t)s[i].off_K * 1024;
			lens[i]    = (size_t)s[i].len_K * 1024;
		}
		if (!fits) {
			test_skip(c, id++, what,
				  "case exceeds one system page (PAGE_SIZE=%zu)",
				  c->page_size);
			continue;
		}
		run_vec(c, id++, what, src_off, dst_off, lens, n,
			c->dev_off_base);
	}
}

/*
 * Suite 7: randomized stress. Generates a moderate number of pseudo-
 * random (iov_count, per-iov len, per-iov src/dst offsets) tuples,
 * each LBS-aligned. This is the catch-all for layouts the explicit
 * suites missed; with a recorded seed it is also reproducible.
 */
static void suite_random(struct ctx *c, int rounds)
{
	uint32_t id = 7000;
	for (int r = 0; r < rounds; r++) {
		int n = 1 + (rand() % 8);   /* 1..8 iovecs */
		size_t src_off[MAX_IOV], dst_off[MAX_IOV], lens[MAX_IOV];
		size_t total = 0;
		size_t src_cursor = ((size_t)(rand() % 32)) * c->lbs;
		size_t dst_cursor = c->half / 2 +
				    ((size_t)(rand() % 32)) * c->lbs;
		int kept = 0;
		for (int i = 0; i < n; i++) {
			size_t len = c->lbs * (1u + (rand() % 32));
			if (src_cursor + len > c->half / 2 ||
			    dst_cursor + len > c->half) {
				break;
			}
			src_off[kept] = src_cursor;
			dst_off[kept] = dst_cursor;
			lens[kept]    = len;
			total       += len;
			src_cursor  += len + (size_t)(rand() % 8) * c->lbs;
			dst_cursor  += len + (size_t)(rand() % 8) * c->lbs;
			kept++;
		}
		if (kept == 0) continue;
		char what[40];
		snprintf(what, sizeof what,
			 "random n=%d total=0x%zx", kept, total);
		if (kept == 1) {
			run_single(c, id++, what, src_off[0], dst_off[0],
				   lens[0], c->dev_off_base);
		} else {
			run_vec(c, id++, what, src_off, dst_off, lens,
				kept, c->dev_off_base);
		}
	}
}

/*
 * Suite 8: read-back stability. Write a known pattern at one device
 * offset, then read it back many times in a row. Catches TX-path
 * state corruption that only shows up after the first FPDU has flowed
 * through (e.g. stale page_off[] / page_len[] reused between
 * iterations of the same QP).
 */
static void suite_reread(struct ctx *c, int iters)
{
	uint32_t id = 8000;
	const size_t io_size = 64 * 1024;
	const size_t src_off = c->page_size + c->lbs;  /* PAGE_SIZE+4K */

	if (src_off + io_size > c->half) return;
	if (c->dev_off_base + io_size > c->dev_size) return;

	fill_pattern(c->src + src_off, io_size, id, 0);
	if (do_pwrite(c->fd, c->src + src_off, io_size, c->dev_off_base) < 0) {
		test_fail(c, id, "reread setup",
			  "pwrite: %s", strerror(errno));
		return;
	}

	for (int i = 0; i < iters; i++) {
		size_t dst_off = src_off + (size_t)(i & 7) * c->lbs;
		if (dst_off + io_size > c->half) dst_off = src_off;
		memset(c->dst + dst_off, POISON_BYTE, io_size);
		if (do_pread(c->fd, c->dst + dst_off, io_size,
			     c->dev_off_base) < 0) {
			test_fail(c, id, "reread",
				  "iter=%d pread: %s",
				  i, strerror(errno));
			return;
		}
		size_t bad; uint8_t exp, got;
		if (!verify_pattern(c->dst + dst_off, io_size, id, 0,
				    &bad, &exp, &got)) {
			test_fail(c, id, "reread",
				  "iter=%d mismatch @0x%zx exp=0x%02x got=0x%02x "
				  "dst_page_off=0x%05lx",
				  i, bad, exp, got,
				  (unsigned long)(((uintptr_t)(c->dst + dst_off))
					& (c->page_size - 1)));
			return;
		}
	}
	char detail[40];
	snprintf(detail, sizeof detail, "iters=%d", iters);
	test_pass(c, id, "reread", detail);
}

/* ------------------------------------------------------------------ */
/* device info / SIW state                                             */
/* ------------------------------------------------------------------ */

static void cat_one_line(const char *path, const char *label)
{
	FILE *f = fopen(path, "r");
	if (!f) {
		fprintf(stderr, "%-22s <not available: %s>\n",
			label, strerror(errno));
		return;
	}
	char buf[128] = {0};
	if (fgets(buf, sizeof buf, f)) {
		size_t l = strlen(buf);
		if (l && buf[l - 1] == '\n') buf[l - 1] = 0;
		fprintf(stderr, "%-22s %s\n", label, buf);
	} else {
		fprintf(stderr, "%-22s <empty>\n", label);
	}
	fclose(f);
}

static void print_setup(struct ctx *c, const char *path)
{
	fprintf(stderr, "==== SIW 64k-kernel / 4k-MR corner-case test ====\n");
	fprintf(stderr, "Device:                %s\n", path);
	fprintf(stderr, "System PAGE_SIZE:      %zu (%zu KiB)\n",
		c->page_size, c->page_size >> 10);
	fprintf(stderr, "Logical block size:    %zu\n", c->lbs);
	fprintf(stderr, "Device size:           %llu bytes (%llu MiB)\n",
		(unsigned long long)c->dev_size,
		(unsigned long long)(c->dev_size >> 20));
	fprintf(stderr, "Test buffer:           %zu bytes (%zu MiB), each half = %zu MiB\n",
		c->buf_size, c->buf_size >> 20, c->half >> 20);
	fprintf(stderr, "Buf base:              %p (page_off=0x%lx)\n",
		c->buf,
		(unsigned long)((uintptr_t)c->buf & (c->page_size - 1)));
	fprintf(stderr, "Device offset base:    0x%llx\n",
		(unsigned long long)c->dev_off_base);
	fprintf(stderr, "Seed:                  %u\n", c->seed);

	cat_one_line("/sys/module/siw/parameters/mr_min_page_4k",
		     "siw mr_min_page_4k:");

	if (c->page_size > 4096) {
		fprintf(stderr,
			"NOTE: PAGE_SIZE > 4 KiB. With mr_min_page_4k=Y the\n"
			"      kernel exercises sub-PAGE_SIZE PBL entries -- the\n"
			"      mode the NVMESH-8849 fix targets. Without the fix,\n"
			"      this test produces data mismatches.\n");
	} else {
		fprintf(stderr,
			"NOTE: PAGE_SIZE == 4 KiB. The fast-reg page size and\n"
			"      system page size coincide, so the\n"
			"      paddr-vs-laddr corner case is not reachable on\n"
			"      this host; the test still verifies the\n"
			"      page_off[]/page_len[] / merge-with-prev rewrite.\n");
	}
	fprintf(stderr, "\n");
}

/* ------------------------------------------------------------------ */
/* usage                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
	fprintf(stderr,
"Usage: %s [options] <block-device>\n"
"\n"
"  Drives O_DIRECT I/Os through an NVMesh block device to exercise the\n"
"  SIW MR-page-size=4K-on-64K-kernel corner cases (NVMESH-8849).\n"
"  Each test writes a deterministic byte pattern and reads it back; any\n"
"  byte mismatch is reported with the originating intra-page offset.\n"
"\n"
"  Options:\n"
"    -B, --buf-size N      mmap'd buffer size, bytes  (default %llu)\n"
"    -D, --dev-off    N    starting device offset     (default %llu)\n"
"    -r, --rounds     N    randomized-suite rounds    (default 200)\n"
"    -s, --seed       N    PRNG seed                  (default time)\n"
"    -i, --reread-iters N  reread-stability iters     (default 64)\n"
"    -v, --verbose         print PASS lines too\n"
"    -h, --help            this help\n"
"\n"
"  DESTRUCTIVE: this writes to <block-device>. Pass a scratch volume.\n",
		prog,
		(unsigned long long)BUF_DEFAULT,
		(unsigned long long)DEV_OFF_DEFAULT);
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	struct ctx c = {
		.fd       = -1,
		.page_size = (size_t)sysconf(_SC_PAGESIZE),
		.lbs      = LBS_FALLBACK,
		.buf_size = BUF_DEFAULT,
		.dev_off_base = DEV_OFF_DEFAULT,
		.seed     = (unsigned)time(NULL),
	};
	int rounds = 200;
	int reread_iters = 64;
	const char *path = NULL;

	static const struct option longopts[] = {
		{ "buf-size",     required_argument, NULL, 'B' },
		{ "dev-off",      required_argument, NULL, 'D' },
		{ "rounds",       required_argument, NULL, 'r' },
		{ "seed",         required_argument, NULL, 's' },
		{ "reread-iters", required_argument, NULL, 'i' },
		{ "verbose",      no_argument,       NULL, 'v' },
		{ "help",         no_argument,       NULL, 'h' },
		{ 0, 0, 0, 0 },
	};
	int ch;
	while ((ch = getopt_long(argc, argv, "B:D:r:s:i:vh",
				 longopts, NULL)) != -1) {
		switch (ch) {
		case 'B':
			c.buf_size = strtoull(optarg, NULL, 0);
			break;
		case 'D':
			c.dev_off_base = strtoull(optarg, NULL, 0);
			break;
		case 'r':
			rounds = atoi(optarg);
			break;
		case 's':
			c.seed = (unsigned)strtoul(optarg, NULL, 0);
			break;
		case 'i':
			reread_iters = atoi(optarg);
			break;
		case 'v':
			c.verbose = 1;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}
	if (optind >= argc) {
		fprintf(stderr, "error: missing block-device argument\n\n");
		usage(argv[0]);
		return 1;
	}
	path = argv[optind];

	srand(c.seed);

	c.fd = open(path, O_RDWR | O_DIRECT);
	if (c.fd < 0) {
		fprintf(stderr, "open(%s, O_RDWR|O_DIRECT): %s\n",
			path, strerror(errno));
		return 1;
	}
	unsigned int lbs = LBS_FALLBACK;
	if (ioctl(c.fd, BLKSSZGET, &lbs) == 0 && lbs)
		c.lbs = lbs;
	uint64_t dsize = 0;
	if (ioctl(c.fd, BLKGETSIZE64, &dsize) == 0 && dsize) {
		c.dev_size = dsize;
	} else {
		struct stat st;
		if (fstat(c.fd, &st) == 0 && st.st_size > 0)
			c.dev_size = (uint64_t)st.st_size;
		else
			c.dev_size = (uint64_t)1 << 40;
	}

	/* Round buf_size up to a multiple of (2 * page_size) so each half
	 * starts on a system-page boundary. */
	size_t align = c.page_size * 2;
	c.buf_size = (c.buf_size + align - 1) & ~(align - 1);

	c.buf = mmap(NULL, c.buf_size, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (c.buf == MAP_FAILED) {
		fprintf(stderr, "mmap(%zu): %s\n",
			c.buf_size, strerror(errno));
		close(c.fd);
		return 1;
	}
	c.half = c.buf_size / 2;
	c.src  = (uint8_t *)c.buf;
	c.dst  = (uint8_t *)c.buf + c.half;

	print_setup(&c, path);

	/* Run the suites. Order matters only for human readability; each
	 * test is self-contained (write -> read -> verify) so a failure
	 * in one does not corrupt the next. */
	suite_intra_page(&c);
	suite_cross_page(&c);
	suite_large(&c);
	suite_vec_intra_page(&c);
	suite_vec_mixed(&c);
	suite_same_page_pbl(&c);
	suite_random(&c, rounds);
	suite_reread(&c, reread_iters);

	fprintf(stderr,
		"\n=== Summary ===\n"
		"PASS:     %d\n"
		"FAIL:     %d\n"
		"SKIPPED:  %d\n"
		"Seed:     %u   (re-run with `-s %u` to reproduce)\n",
		c.pass, c.fail, c.skipped, c.seed, c.seed);

	munmap(c.buf, c.buf_size);
	close(c.fd);
	return c.fail ? 1 : 0;
}
