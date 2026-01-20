#!/usr/bin/env python3
import argparse
import gzip
import io
import sys
from pathlib import Path
from typing import Dict, Tuple, List, TextIO, Optional

Sym = Tuple[str, str]  # (crc, export_type)


def open_maybe_gzip(path: Path) -> TextIO:
    """
    Open a file as text. If it is gzip-compressed (by extension or magic bytes),
    transparently decompress.
    """
    with path.open("rb") as f:
        head = f.read(2)

    is_gz = (path.suffix == ".gz") or (head == b"\x1f\x8b")
    if is_gz:
        return io.TextIOWrapper(gzip.open(path, "rb"), encoding="utf-8", errors="replace")
    return path.open("r", encoding="utf-8", errors="replace")


def normalize_crc(raw: str) -> Optional[str]:
    s = raw.strip().lower()
    if s.startswith("0x"):
        # ensure it's valid hex
        try:
            int(s[2:], 16)
            return s
        except ValueError:
            return None
    # allow plain hex
    try:
        int(s, 16)
        return "0x" + s
    except ValueError:
        return None


def mod_basename(mod_field: str) -> str:
    """
    Normalize the module column to a basename so that:
      - "drivers/net/ethernet/intel/e1000e/e1000e" -> "e1000e"
      - "kernel/drivers/.../foo.ko" -> "foo"
      - "foo.ko" -> "foo"
      - "foo" -> "foo"
    """
    m = mod_field.strip().replace("\\", "/")
    base = m.rsplit("/", 1)[-1]
    if base.endswith(".ko"):
        base = base[:-3]
    return base


def parse_symvers(path: Path, use_basename: bool) -> Dict[str, Dict[str, Sym]]:
    """
    Returns: modules[module_key][symbol] = (crc, export_type)
    Where module_key is either the raw module column or its basename, depending on use_basename.
    Supports plain Module.symvers and RHEL symvers*.gz.
    """
    modules: Dict[str, Dict[str, Sym]] = {}

    with open_maybe_gzip(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue

            # Expected common format:
            #   0x<crc>  <symbol>  <module>  <export_type>
            parts = line.split()
            if len(parts) < 3:
                continue

            crc = normalize_crc(parts[0])
            if crc is None:
                continue

            sym = parts[1]
            mod = parts[2]
            export_type = parts[3] if len(parts) >= 4 else ""

            key = mod_basename(mod) if use_basename else mod
            modules.setdefault(key, {})[sym] = (crc, export_type)

    return modules


def compare_module(
    new_symvers: Path,
    orig_symvers: Path,
    module: str,
    strict: bool,
    basename_mode: bool,
) -> int:
    new = parse_symvers(new_symvers, use_basename=basename_mode)
    orig = parse_symvers(orig_symvers, use_basename=basename_mode)

    mod_key = mod_basename(module) if basename_mode else module

    new_mod = new.get(mod_key, {})
    orig_mod = orig.get(mod_key, {})

    if not orig_mod:
        print(
            f"ERROR: module '{mod_key}' not found in original symvers: {orig_symvers}",
            file=sys.stderr,
        )
        return 2
    if not new_mod:
        print(
            f"ERROR: module '{mod_key}' not found in new symvers: {new_symvers}",
            file=sys.stderr,
        )
        return 2

    new_syms = set(new_mod.keys())
    orig_syms = set(orig_mod.keys())

    missing_in_new = sorted(orig_syms - new_syms)
    extra_in_new = sorted(new_syms - orig_syms)

    mismatched: List[Tuple[str, str, str]] = []
    for sym in sorted(orig_syms & new_syms):
        orig_crc, _ = orig_mod[sym]
        new_crc, _ = new_mod[sym]
        if orig_crc != new_crc:
            mismatched.append((sym, orig_crc, new_crc))

    ok = (not missing_in_new) and (not mismatched) and (not (strict and extra_in_new))

    print(f"Module key: {mod_key}" + (" (basename mode)" if basename_mode else ""))
    print(f"Original symbols: {len(orig_syms)}")
    print(f"New symbols:      {len(new_syms)}")
    print()

    if missing_in_new:
        print(f"Missing in NEW ({len(missing_in_new)}):")
        for s in missing_in_new:
            print(f"  {s}")
        print()

    if mismatched:
        print(f"CRC mismatches ({len(mismatched)}):")
        for sym, o, n in mismatched:
            print(f"  {sym}: orig={o} new={n}")
        print()

    if extra_in_new:
        print(f"Extra in NEW ({len(extra_in_new)}):")
        for s in extra_in_new:
            print(f"  {s}")
        print()

    if ok:
        if extra_in_new and not strict:
            print("RESULT: PASS (CRCs match for all original symbols; new has extra symbols, allowed)")
        else:
            print("RESULT: PASS (CRCs match and symbol set matches policy)")
        return 0

    if strict and extra_in_new:
        print("RESULT: FAIL (strict mode: new has extra symbols)")
    else:
        print("RESULT: FAIL")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description=(
            "Compare per-module symbol CRCs between two Module.symvers files "
            "(supports RHEL symvers*.gz; supports module column paths via basename matching)."
        )
    )
    ap.add_argument("--new", required=True, type=Path, help="Path to NEW Module.symvers (optionally .gz)")
    ap.add_argument("--orig", required=True, type=Path, help="Path to ORIGINAL Module.symvers or RHEL symvers*.gz")
    ap.add_argument(
        "--module",
        required=True,
        help=(
            "Module identifier to compare. In basename mode (default), you can pass 'e1000e' "
            "even if the symvers module column contains a path."
        ),
    )
    ap.add_argument(
        "--no-basename",
        action="store_true",
        help="Disable basename mode; compare using the raw module column value exactly.",
    )
    ap.add_argument("--strict", action="store_true", help="Also fail if NEW has extra symbols")
    args = ap.parse_args()

    for p in (args.new, args.orig):
        if not p.exists():
            print(f"ERROR: file not found: {p}", file=sys.stderr)
            return 2

    basename_mode = not args.no_basename
    return compare_module(args.new, args.orig, args.module, args.strict, basename_mode)


if __name__ == "__main__":
    raise SystemExit(main())

