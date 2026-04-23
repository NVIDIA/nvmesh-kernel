#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
"""
tpv_trim_smoke.py - data-integrity smoke test for TPV DISCARD/TRIM.

Writes a self-describing pattern to a raw TPV block device, issues a set of
BLKDISCARD ioctls targeting aligned, misaligned, and already-trimmed ranges,
and then reads every 4 KiB block back to verify:

  - Blocks fully inside a trimmed range must read as all-zeros.
  - Blocks outside any trimmed range must still hold the original pattern.
  - Blocks only partially overlapped by a trim MUST NOT be zeroed (the TPV
    layer's "misaligned skip" invariant - see TPV_Trimming.md Step 1).

The pattern is:  byte[B] is the LSB of (B // 8), i.e. every u64 at offset B
encodes B itself (so any block position self-identifies).  This lets a
mismatch point at the exact byte and tell us whether the data is stale,
zeroed-wrong, or overwritten from another extent.

Usage:  sudo ./tpv_trim_smoke.py --device /dev/nvmesh/<tpv> [--size-mib 64]

Exits 0 on pass, 1 on any mismatch, 2 on setup error.
"""
import argparse
import ctypes
import fcntl
import mmap
import os
import struct
import sys

PAGE = 4096  # O_DIRECT alignment requirement on all architectures we care about


def aligned_buf(size):
    """Page-aligned writable buffer for O_DIRECT I/O.  mmap with MAP_ANONYMOUS
    always returns page-aligned memory; bytearray / bytes do not."""
    return mmap.mmap(-1, size, mmap.MAP_PRIVATE | mmap.MAP_ANONYMOUS)

SECTOR = 4096               # TPV uses 4 KiB sectors (NVMEIBC_SECTOR_SHIFT=12)
BLKDISCARD = 0x1277         # _IO(0x12, 119)
BLKGETSIZE64 = 0x80081272   # _IOR(0x12, 114, size_t)


def fill_pattern(buf, byte_offset, length):
    """Write into pre-allocated aligned buf[0:length]: u64 at k = byte_offset+k."""
    for k in range(0, length, 8):
        struct.pack_into("<Q", buf, k, byte_offset + k)


def mk_pattern(byte_offset, length):
    """Return a (non-aligned) bytes object with the expected pattern for
    verification comparisons."""
    buf = bytearray(length)
    for k in range(0, length, 8):
        struct.pack_into("<Q", buf, k, byte_offset + k)
    return bytes(buf)


def verify_block(data, byte_offset):
    """Return (ok, first_bad_off, actual_u64, expected_u64) or (True, None, None, None)."""
    expected = mk_pattern(byte_offset, len(data))
    if data == expected:
        return True, None, None, None
    # Find first mismatching u64 for diagnostics.
    for k in range(0, len(data), 8):
        actual = struct.unpack_from("<Q", data, k)[0]
        wanted = byte_offset + k
        if actual != wanted:
            return False, k, actual, wanted
    return False, 0, None, None  # unreachable


def is_zero_block(data):
    return data == b"\x00" * len(data)


def dev_size(fd):
    buf = ctypes.c_uint64(0)
    fcntl.ioctl(fd, BLKGETSIZE64, buf)
    return buf.value


def discard(fd, start, length):
    rng = struct.pack("QQ", start, length)
    fcntl.ioctl(fd, BLKDISCARD, rng)


class Range:
    __slots__ = ("start", "end", "label", "extent_aligned")

    def __init__(self, start, length, label, extent_size):
        self.start = start
        self.end = start + length
        self.label = label
        head_aligned = (start % extent_size) == 0
        tail_aligned = (self.end % extent_size) == 0
        self.extent_aligned = head_aligned and tail_aligned

    def fully_covers(self, blk_start, blk_end, extent_size):
        """Is the 4 KiB block [blk_start, blk_end) inside any *full extent*
        fully contained in this trim range?
        """
        first_full_extent = ((self.start + extent_size - 1) // extent_size) * extent_size
        last_full_extent = (self.end // extent_size) * extent_size
        return first_full_extent <= blk_start and blk_end <= last_full_extent


def phase_fill(fd, size):
    print(f"[fill] writing {size // (1 << 20)} MiB of pattern...")
    CHUNK = 1 << 20
    buf = aligned_buf(CHUNK)
    try:
        os.lseek(fd, 0, os.SEEK_SET)
        for off in range(0, size, CHUNK):
            n = min(CHUNK, size - off)
            fill_pattern(buf, off, n)
            mv = memoryview(buf)
            try:
                written = os.write(fd, mv[:n])
            finally:
                mv.release()
            if written != n:
                raise IOError(f"short write at {off}: {written} of {n}")
        os.fsync(fd)
    finally:
        buf.close()


def phase_trim(fd, ranges):
    print(f"[trim] issuing {len(ranges)} BLKDISCARD calls:")
    for r in ranges:
        length = r.end - r.start
        aligned = "aligned" if r.extent_aligned else "MISALIGNED"
        print(f"  {r.label:30s} start={r.start:>10d} len={length:>10d} ({aligned})")
        try:
            discard(fd, r.start, length)
        except OSError as e:
            print(f"    -> ioctl failed: {e}")


def phase_verify(fd, size, ranges, extent_size):
    print(f"[verify] reading {size // (1 << 20)} MiB block-by-block...")
    CHUNK = 1 << 20
    rbuf = aligned_buf(CHUNK)
    zero_ok = zero_bad = pat_ok = pat_bad = 0
    mismatches = []
    try:
        for off in range(0, size, CHUNK):
            n = min(CHUNK, size - off)
            mv = memoryview(rbuf)
            try:
                nread = os.preadv(fd, [mv[:n]], off)
                if nread != n:
                    print(f"  short read at {off}: got {nread} want {n}")
                    return 2
                buf = bytes(mv[:n])
            finally:
                mv.release()
            for bk in range(0, n, SECTOR):
                blk_start = off + bk
                blk_end = blk_start + SECTOR
                blk = buf[bk : bk + SECTOR]
                in_trimmed_full = any(
                    r.fully_covers(blk_start, blk_end, extent_size) for r in ranges
                )
                if in_trimmed_full:
                    if is_zero_block(blk):
                        zero_ok += 1
                    else:
                        zero_bad += 1
                        if len(mismatches) < 10:
                            mismatches.append(
                                (blk_start, "expected ZERO", blk[:32].hex())
                            )
                else:
                    ok, bad_k, actual, wanted = verify_block(blk, blk_start)
                    if ok:
                        pat_ok += 1
                    else:
                        pat_bad += 1
                        if len(mismatches) < 10:
                            if actual is None:
                                mismatches.append(
                                    (blk_start, "pattern mismatch (all-zero?)", blk[:32].hex())
                                )
                            else:
                                mismatches.append(
                                    (
                                        blk_start + bad_k,
                                        f"want=0x{wanted:016x} got=0x{actual:016x}",
                                        "",
                                    )
                                )
    finally:
        rbuf.close()
    print(f"  pattern preserved: {pat_ok:>7d} blocks  bad: {pat_bad}")
    print(f"  zero-after-trim  : {zero_ok:>7d} blocks  bad: {zero_bad}")
    if mismatches:
        print("  first mismatches:")
        for o, kind, hx in mismatches:
            print(f"    @{o:>10d}: {kind}  {hx}")
    return 0 if (pat_bad == 0 and zero_bad == 0) else 1


def phase_rewrite(fd, ranges):
    """Write pattern back into each trimmed range to exercise the
    re-allocation path, then read back and verify."""
    print(f"[rewrite] re-filling {len(ranges)} trimmed ranges...")
    bad = 0
    # Use a single buffer sized to the largest trim range.
    max_len = max(r.end - r.start for r in ranges)
    wbuf = aligned_buf(max_len)
    rbuf = aligned_buf(max_len)
    try:
        for r in ranges:
            length = r.end - r.start
            fill_pattern(wbuf, r.start, length)
            mv = memoryview(wbuf)
            try:
                nw = os.pwritev(fd, [mv[:length]], r.start)
            finally:
                mv.release()
            if nw != length:
                raise IOError(f"rewrite short write at {r.start}: {nw} of {length}")
        os.fsync(fd)
        print("[rewrite-verify] reading back rewritten ranges...")
        for r in ranges:
            length = r.end - r.start
            mv = memoryview(rbuf)
            try:
                nr = os.preadv(fd, [mv[:length]], r.start)
                if nr != length:
                    raise IOError(f"rewrite-verify short read at {r.start}: {nr}")
                data = bytes(mv[:length])
            finally:
                mv.release()
            expected = mk_pattern(r.start, length)
            if data != expected:
                bad += 1
                for k in range(0, length, 8):
                    if data[k : k + 8] != expected[k : k + 8]:
                        actual = struct.unpack_from("<Q", data, k)[0]
                        wanted = struct.unpack_from("<Q", expected, k)[0]
                        print(
                            f"  {r.label}: @{r.start+k} want=0x{wanted:016x} got=0x{actual:016x}"
                        )
                        break
    finally:
        wbuf.close()
        rbuf.close()
    return 0 if bad == 0 else 1


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--device", required=True, help="e.g. /dev/nvmesh/mytpv")
    p.add_argument("--size-mib", type=int, default=64, help="test region size in MiB")
    p.add_argument(
        "--extent-kib",
        type=int,
        default=64,
        help="TPV extent size in KiB (must match tpvConfig.tpvExtentSizeKB)",
    )
    p.add_argument(
        "--skip-fill",
        action="store_true",
        help="skip the pattern-fill phase (assume device already holds the pattern from a prior run)",
    )
    p.add_argument(
        "--skip-trim",
        action="store_true",
        help="skip the BLKDISCARD phase (assume trims were already issued in a prior run)",
    )
    p.add_argument(
        "--skip-verify",
        action="store_true",
        help="skip the read-and-verify phase (use to stage state for a later run)",
    )
    p.add_argument(
        "--skip-rewrite",
        action="store_true",
        help="skip the rewrite-trimmed-ranges phase",
    )
    args = p.parse_args()

    size = args.size_mib << 20
    extent_size = args.extent_kib << 10

    try:
        fd = os.open(args.device, os.O_RDWR | os.O_DIRECT | os.O_SYNC)
    except OSError as e:
        print(f"open({args.device}) failed: {e}", file=sys.stderr)
        return 2

    try:
        dsize = dev_size(fd)
        if size > dsize:
            print(f"requested {size} > device size {dsize}", file=sys.stderr)
            return 2

        # Build trim plan.  All offsets are sector-aligned (required by the
        # block layer ioctl), but only some are TPV-extent aligned.
        assert extent_size >= SECTOR and extent_size % SECTOR == 0
        E = extent_size
        ranges = [
            Range(2 * E, E, "single aligned extent", E),
            Range(16 * E, 64 * E, "64 aligned extents", E),
            # Head-misaligned by one sector.
            Range(128 * E + SECTOR, 4 * E, "head-misaligned", E),
            # Tail-misaligned.
            Range(160 * E, 4 * E - SECTOR, "tail-misaligned", E),
            # Both ends misaligned, spans multiple full extents in the middle.
            Range(192 * E + SECTOR, 4 * E - 2 * SECTOR, "both-ends-misaligned", E),
        ]
        for r in ranges:
            if r.end > size:
                print(
                    f"trim range '{r.label}' exceeds test size; increase --size-mib",
                    file=sys.stderr,
                )
                return 2

        if args.skip_fill:
            print("[fill] SKIPPED (--skip-fill); assuming pattern is already on device")
        else:
            phase_fill(fd, size)

        if args.skip_trim:
            print("[trim] SKIPPED (--skip-trim); assuming trims were already issued")
        else:
            phase_trim(fd, ranges)
            # Issue the aligned ranges a second time: must be harmless.
            for r in ranges:
                if r.extent_aligned:
                    try:
                        discard(fd, r.start, r.end - r.start)
                    except OSError as e:
                        print(f"[trim-2nd] re-trim of {r.label} failed: {e}")
            os.fsync(fd)

        if args.skip_verify:
            print("[verify] SKIPPED (--skip-verify)")
            print("STAGED  (state left on device for a later --skip-fill --skip-trim run)")
            return 0

        rv = phase_verify(fd, size, ranges, extent_size)
        if rv != 0:
            return rv

        if not args.skip_rewrite:
            rv = phase_rewrite(fd, ranges)
            if rv != 0:
                return rv

        print("PASS")
        return 0
    finally:
        os.close(fd)


if __name__ == "__main__":
    sys.exit(main())
