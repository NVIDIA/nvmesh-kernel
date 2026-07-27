<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# `siw_4k_mr_blk_test` &mdash; NVMESH-8849 corner-case driver

User-space round-trip test for the SIW *MR page size = 4 KiB on a 64 KiB
kernel* fix (NVMESH-8849, SIW commit `616a176bf5dd`).

It does **not** talk to libibverbs directly; instead it issues O_DIRECT
`pread`/`pwrite`/`preadv`/`pwritev` against an NVMesh block device. The
NVMesh client kernel module:

1. Picks `mr_page_shift = max(12, ffs(page_size_cap) - 1)` &mdash; on a
   64 KiB ARM64 kernel with `siw.mr_min_page_4k=Y` this is 12 (4 KiB).
2. Calls `ib_alloc_mr(IB_MR_TYPE_MEM_REG, ...)` and
   `ib_map_mr_sg(... page_size=4096)` against the bio's SGL.
3. Posts an RDMA WRITE / READ over the resulting fast-reg MR. SIW
   builds the FPDU from the PBL.

That last step is the path the fix rewrote, so the *user-buffer
alignment* of the read/write determines the PBL layout the kernel ends
up handing to `siw_tx_hdt`. By picking buffer offsets at every 4 KiB
stride inside a 64 KiB system page, and by using `pwritev`/`preadv` to
build multi-element SGLs, the test reaches every corner the commit
touches.

## DESTRUCTIVE

The test issues plain writes to the device. Run it against a **scratch
volume** that does not hold valuable data.

## Build

```bash
make
```

(Build deps: a C99 compiler, `linux/fs.h`. No libibverbs / librdmacm
dependency.)

## Run

```bash
./siw_4k_mr_blk_test /dev/nvmesh/<scratch-vol>
./siw_4k_mr_blk_test -v /dev/nvmesh/<scratch-vol>
./siw_4k_mr_blk_test -s 12345 /dev/nvmesh/<scratch-vol>   # reproduce a seed
```

Options:

| Flag                   | Meaning                                    | Default |
| ---------------------- | ------------------------------------------ | ------- |
| `-B`, `--buf-size`     | mmap'd test buffer size (bytes)            | 32 MiB  |
| `-D`, `--dev-off`      | starting device offset (bytes, LBS-aligned)| 1 MiB   |
| `-r`, `--rounds`       | randomized-suite rounds                    | 200     |
| `-s`, `--seed`         | PRNG seed (for reproducing a failure)      | `time()`|
| `-i`, `--reread-iters` | reread-stability iterations                | 64      |
| `-v`, `--verbose`      | print PASS lines, not just FAIL/skip       | off     |
| `-h`, `--help`         | usage                                      |         |

On success the program prints `PASS: N / FAIL: 0` and exits 0. On any
mismatch it prints a line of the form

```
[NNNN] FAIL  <suite>                  mismatch @0x<off> exp=0x<eb> got=0x<gb> \
                                      src_page_off=0x<spo> dst_page_off=0x<dpo> \
                                      sz=0x<sz> dev_off=0x<doff>
```

and exits non-zero. `src_page_off` / `dst_page_off` are the intra-system-page
offsets of the user buffer pointers handed to `pwrite`/`pread`; these
are the same intra-page offsets the kernel sees in the bio's bvecs and
therefore the same offsets that flow into the SIW PBL.

## What each suite covers

1. **`intra_page`** &mdash; one I/O at every 4 KiB stride within (and
   slightly past) one system page, for sizes `{4, 8, 12, 16, 32, 60,
   64, 68, 128} KiB`. Produces 4 KiB PBEs at every intra-system-page
   offset. Primary check for the `(paddr & ~PAGE_MASK)` vs
   `(laddr & ~PAGE_MASK)` fix in `siw_tx_hdt` / `siw_try_1seg` /
   `check_sent_fpdu_crc`.

2. **`cross_page`** &mdash; I/Os that straddle one or more system pages
   with non-aligned starts, including very small heads/tails. Forces
   multi-bvec bios where the first bvec has a non-zero `bv_offset` and
   `bv_len < PAGE_SIZE`, then full middle pages, then a partial tail.

3. **`large`** &mdash; 1, 2, 4, 8 MiB transfers offset by 4 KiB so each
   FPDU still inherits a non-zero starting intra-page offset. Stresses
   `MAX_ARRAY` sizing
   (`64 KiB / SIW_MR_MIN_PAGE_SIZE + 1 + 2*(SIW_MAX_SGE-1) + 2`) and
   the per-QP `page_off[]` / `page_len[]` arrays in `siw_tx_hdt`.

4. **`vec_intra_page`** &mdash; `pwritev`/`preadv` with N iovecs of
   4 KiB each, for `N ∈ {2,3,4,8,16,32,64,128,256}`. Each iovec lands
   at a distinct intra-page offset, exercising `siw_pages_for_bytes`
   and `siw_0copy_tx`'s SGE-boundary accounting along with the new
   `merge_with_prev` predicate (which must *not* fold a chunk into
   `page_array[seg-1]` across an SGE boundary). The `N = 256` case
   matches `SIW_MAX_SGE_PBL` and the new
   `max_fast_reg_page_list_len` clamp.

5. **`vec_mixed`** &mdash; multi-iovec writes with mixed sizes (4 KiB,
   60 KiB, 128 KiB, 256 KiB, &hellip;) at offsets that combine
   PAGE_SIZE-aligned, sub-PAGE_SIZE-aligned, and PAGE-crossing
   placements within a single `pwritev`. Catches the "easy" full-page
   bvecs interacting with sub-PAGE_SIZE bvecs in one MR registration.

6. **`same_page_pbl`** &mdash; the corner case the new
   `merge_with_prev` predicate was added for: multiple 4 KiB PBEs in
   *the same* system page, presented in iovec orders that are
   **non-monotonic** (descending intra-page offset) or **have gaps**
   between PBEs. Without the predicate's intra-page-contiguity check
   the old `page_array[seg-1] == p` code would fold these into a
   single sendpage that either reads through the gap (gap case) or
   sends the bytes in the wrong order (reversed case), corrupting
   the FPDU. Cases covered:

   | Name           | n | iovec layout within one system page          |
   | -------------- | - | --------------------------------------------- |
   | `2-gap-4K`     | 2 | `{0K,4K}, {8K,4K}`                            |
   | `2-gap-24K`    | 2 | `{0K,4K}, {28K,4K}`                           |
   | `2-rev`        | 2 | `{8K,4K}, {0K,4K}`                            |
   | `2-rev-adj`    | 2 | `{4K,4K}, {0K,4K}`                            |
   | `3-gap`        | 3 | `{0K,4K}, {8K,4K}, {16K,4K}`                  |
   | `3-mixed`      | 3 | `{16K,4K}, {0K,4K}, {8K,4K}`                  |
   | `3-rev`        | 3 | `{16K,4K}, {8K,4K}, {0K,4K}`                  |
   | `4-rev`        | 4 | `{28K,4K}, {20K,4K}, {8K,4K}, {0K,4K}`        |
   | `4-mix`        | 4 | `{12K,4K}, {4K,4K}, {0K,4K}, {20K,4K}`        |
   | `wide-gap-2`   | 2 | `{0K,8K}, {12K,8K}` (each iov &rarr; 2 PBEs)  |
   | `wide-gap-3`   | 3 | `{0K,8K}, {12K,4K}, {20K,8K}`                 |
   | `wide-rev-2`   | 2 | `{12K,8K}, {0K,8K}`                           |

   On kernels where `PAGE_SIZE <= 4 KiB` these cases SKIP &mdash; one
   PBE per page means the same-page corner is unreachable.

7. **`random`** &mdash; randomized stress, reproducible from the
   recorded seed. Catch-all for layouts the explicit suites missed.

8. **`reread`** &mdash; one fixed write followed by N consecutive reads
   to the same device offset, each landing in a slightly different
   destination intra-page offset. Catches TX-path state that survives
   across FPDUs (e.g. stale `page_off[]` / `page_len[]` re-used between
   iterations of the same QP).

## Interpreting `siw.mr_min_page_4k` and `PAGE_SIZE`

At startup the program prints both. The combinations:

| PAGE_SIZE | `mr_min_page_4k` | What the test exercises in SIW                |
| --------- | ---------------- | ---------------------------------------------- |
| 4 KiB     | Y or N           | Same as before the fix; corner case not reach- |
|           |                  | able. Test still validates the                 |
|           |                  | `page_off[]`/`page_len[]` and merge-with-prev  |
|           |                  | rewrite.                                       |
| 64 KiB    | N                | Same as before the fix; PBEs are 64 KiB.       |
| 64 KiB    | Y                | Sub-PAGE_SIZE PBE path. Without the fix the    |
|           |                  | test reports byte mismatches.                  |

A clean run on a 64 KiB-page kernel with `siw.mr_min_page_4k=Y`
constitutes a positive regression test for NVMESH-8849.

## Related code

- `softiwarp/kernel/siw.h` &mdash; `SIW_MR_MIN_PAGE_SIZE`,
  `SIW_MAX_SGE_PBL`, `MAX_ARRAY`, `siw_tx_hdt_dbg_entry`.
- `softiwarp/kernel/siw_verbs.c` &mdash; `mr_min_page_4k` modparam,
  `siw_query_device` `page_size_cap`, `siw_map_mr_sg`.
- `softiwarp/kernel/siw_qp_tx.c` &mdash; `siw_pbl_get_paddr`,
  `siw_tx_hdt` merge predicate, `siw_tcp_sendpages` /
  `siw_pages_for_bytes` per-entry offsets, `check_sent_fpdu_crc`.
- `common/nvmeib.c` &mdash; `nvmeib_init_fast_reg`
  (`ffs(page_size_cap) - 1`).
