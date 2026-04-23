#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
"""
tpv_alloc_audit.py - TPV trim invariant correlator.

For every CDV with at least one attached TPV, asserts that the elected
allocator TOMA's n_free_returns_received equals the sum of stat_cdv_free_ok
across all clients hosting a TPV backed by that CDV.

    Sigma(client.cdv_free_ok for tpv.cdv_uuid == C) == toma(C).n_free_returns_received

Any drift indicates a lost CDV_FREE_EXTENT message, a double-free, or a
counter bug - all Step 3 observability targets in TPV_Trimming.md.

Data sources:
  - Per TPV on each client: /proc/nvmeibc/tpv/<name>/status   (cdv_uuid)
                            /proc/nvmeibc/tpv/<name>/stats    (cdv_free_ok)
  - Per TOMA:               `toma_rpc status cdv_detailed`    (free_returns,
                                                               allocator=<host>)

Usage:
    tpv_alloc_audit.py --clients c1,c2,c3 --tomas t1,t2,t3 [--ssh-user root]

Exit codes:
    0   all CDVs balanced
    1   at least one CDV mismatched (details printed to stderr)
    2   data collection failure (SSH, missing proc, etc.)
"""
import argparse
import collections
import re
import subprocess
import sys


def ssh(host, cmd, ssh_user):
    target = f"{ssh_user}@{host}" if ssh_user else host
    try:
        out = subprocess.run(
            ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", target, cmd],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except subprocess.TimeoutExpired:
        print(f"[{host}] ssh timeout running: {cmd}", file=sys.stderr)
        return None
    if out.returncode != 0:
        print(f"[{host}] ssh failed ({out.returncode}): {cmd}\n{out.stderr}",
              file=sys.stderr)
        return None
    return out.stdout


def collect_client(host, ssh_user):
    """Return {cdv_uuid: cdv_free_ok_sum}."""
    listing = ssh(host, "ls /proc/nvmeibc/tpv 2>/dev/null || true", ssh_user)
    if listing is None:
        return None
    by_cdv = collections.defaultdict(int)
    for name in listing.split():
        name = name.strip()
        if not name:
            continue
        status = ssh(host,
                     f"cat /proc/nvmeibc/tpv/{name}/status 2>/dev/null || true",
                     ssh_user)
        stats = ssh(host,
                    f"cat /proc/nvmeibc/tpv/{name}/stats 2>/dev/null || true",
                    ssh_user)
        if status is None or stats is None:
            return None
        m = re.search(r"^cdv_uuid:\s+(\S+)", status, re.MULTILINE)
        if not m:
            print(f"[{host}] tpv {name}: cdv_uuid missing from status",
                  file=sys.stderr)
            continue
        cdv_uuid = m.group(1)
        m = re.search(r"^cdv_free_ok:\s+(\d+)", stats, re.MULTILINE)
        if not m:
            print(f"[{host}] tpv {name}: cdv_free_ok missing from stats",
                  file=sys.stderr)
            continue
        by_cdv[cdv_uuid] += int(m.group(1))
    return dict(by_cdv)


CDV_DETAIL_LINE = re.compile(
    r"cdv=\S+\s+\[(?P<cdv_uuid>[0-9a-fA-F-]+)\]\s+"
    r"allocator=(?P<allocator>\S+)\s+"
    r".*?free_returns=(?P<free_returns>\d+)",
)


def collect_toma(host, ssh_user):
    """Return {cdv_uuid: (allocator_toma, n_free_returns_received)}."""
    raw = ssh(host, "toma_rpc status cdv_detailed 2>/dev/null || true", ssh_user)
    if raw is None:
        return None
    out = {}
    for line in raw.splitlines():
        m = CDV_DETAIL_LINE.search(line)
        if not m:
            continue
        out[m.group("cdv_uuid")] = (m.group("allocator"),
                                    int(m.group("free_returns")))
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--clients", required=True,
                   help="comma-separated client hostnames")
    p.add_argument("--tomas", required=True,
                   help="comma-separated TOMA hostnames")
    p.add_argument("--ssh-user", default="root")
    args = p.parse_args()

    clients = [h.strip() for h in args.clients.split(",") if h.strip()]
    tomas = [h.strip() for h in args.tomas.split(",") if h.strip()]

    # Sum client free-ok counters per CDV.
    client_sum = collections.defaultdict(int)
    for c in clients:
        got = collect_client(c, args.ssh_user)
        if got is None:
            return 2
        for cdv, n in got.items():
            client_sum[cdv] += n

    # For each CDV, find the elected allocator's counter (TOMAs that aren't
    # elected will also publish a row but with allocator_toma != self; picking
    # any row by cdv_uuid is safe since n_free_returns_received is kept only
    # by the handler that actually processes CDV_FREE_EXTENT - the elected
    # allocator.  Follower TOMAs therefore always report 0, and sum() is
    # identical to "elected allocator's value" in a non-partitioned cluster.)
    toma_sum = collections.defaultdict(int)
    elected = {}
    for t in tomas:
        got = collect_toma(t, args.ssh_user)
        if got is None:
            return 2
        for cdv, (alloc_host, n) in got.items():
            toma_sum[cdv] += n
            if alloc_host and alloc_host != "(unelected)":
                elected[cdv] = alloc_host

    all_cdvs = set(client_sum) | set(toma_sum)
    if not all_cdvs:
        print("no CDVs found on clients or TOMAs")
        return 0

    rc = 0
    print(f"{'CDV':<40}  {'allocator':<24}  {'client_free_ok':>14}  "
          f"{'toma_returns':>12}  status")
    for cdv in sorted(all_cdvs):
        cs = client_sum.get(cdv, 0)
        ts = toma_sum.get(cdv, 0)
        ok = (cs == ts)
        if not ok:
            rc = 1
        print(f"{cdv:<40}  {elected.get(cdv, '(unknown)'):<24}  "
              f"{cs:>14}  {ts:>12}  {'OK' if ok else 'MISMATCH'}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
