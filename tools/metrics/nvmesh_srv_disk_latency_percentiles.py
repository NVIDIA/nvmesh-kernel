#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import json
import math
import sys
import time

"""
Usage:
./pager.py -l nvmeibs_trace_metrics -t 'now-1m' --mode msg-stream-json -f 'trace=info_disk_periodic_timer_latency_buckets' \
  | python3 ./nvmesh_srv_disk_latency_percentiles.py

Prints a table. Each output line contains:
- key fields (disk/dev/verb/bin, timestamp)
- interval ops (sum of delta buckets)
- percentile columns in microseconds (P1, P5, ... P99)

Example output:
Latency percentiles

2026-03-05 20:13:59.017661 DISK=S3HCNX0K701047.1 DEV=1 VERB=WRITE BIN=4KiB OPS=181511
  P1=8.19us  P5=8.19us  P10=8.19us  P20=8.19us  P30=8.19us
  P40=8.19us  P50=8.19us  P60=8.19us  P70=8.19us  P80=8.19us
  P90=16.38us  P95=16.38us  P99=16.38us

The script is intentionally simple and supports only DELTA mode:
it computes bucket deltas between two consecutive traces with the same key
(DISK_ID_STR, SEQ, IO_STAT_VERB, STR).
"""

U64_ROLLOVER = 1 << 64
LAT_BUCKETS = 16
LAT_SHIFT_NS = 10  # matches nvmeib_io_histograms.c
PCT_POINTS = [1, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 95, 99]


def bucket_to_ns(bucket_idx):
    return 1 << (bucket_idx + LAT_SHIFT_NS)


def percentile_from_hist(delta_buckets, p):
    total = sum(delta_buckets)
    if total <= 0:
        return 0.0

    target = int(math.ceil(total * p / 100.0))
    cumulative = 0
    for i, count in enumerate(delta_buckets):
        cumulative += count
        if cumulative >= target:
            return bucket_to_ns(i) / 1000.0  # us
    return bucket_to_ns(LAT_BUCKETS - 1) / 1000.0


def format_pct_key(p):
    return f"P{p}"


def u64_delta(curr, prev):
    d = curr - prev
    if d < 0:
        d += U64_ROLLOVER
    return d


def read_bucket_vector(js):
    # NS_12_BINS is a JSON array string like "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]"
    ns_12_bins_str = js.get("NS_12_BINS", "[]")
    buckets = json.loads(ns_12_bins_str)
    # Ensure we have exactly LAT_BUCKETS elements
    while len(buckets) < LAT_BUCKETS:
        buckets.append(0)
    return [int(b) for b in buckets[:LAT_BUCKETS]]


def print_sample(date_str, time_str, disk, dev, verb, bn, ops, pct_vals):
    print(f"{date_str} {time_str} DISK={disk} DEV={dev} VERB={verb} BIN={bn} OPS={ops}")
    per_line = 5
    items = [f"{format_pct_key(p)}={pct_vals[i]:.2f}us" for i, p in enumerate(PCT_POINTS)]
    for i in range(0, len(items), per_line):
        print("  " + "  ".join(items[i:i + per_line]))
    print("")


def stream():
    # key -> (prev_ns, prev_buckets)
    state = {}
    print("Latency percentiles\n")

    for line in sys.stdin:
        try:
            js = json.loads(line)

            if js.get("trace_name") != "info_disk_periodic_timer_latency_buckets":
                continue

            disk = js["DISK_ID_STR"]
            dev = js["SEQ"]
            verb = js["IO_STAT_VERB"]
            bn = js.get("STR", "n/a")
            ns = int(js["nanoseconds"])
            curr_buckets = read_bucket_vector(js)

            key = (disk, dev, bn, verb)
            if key not in state:
                state[key] = (ns, curr_buckets)
                continue

            prev_ns, prev_buckets = state[key]
            dt_ns = ns - prev_ns
            if dt_ns <= 0:
                state[key] = (ns, curr_buckets)
                continue

            hist = [u64_delta(curr_buckets[i], prev_buckets[i]) for i in range(LAT_BUCKETS)]
            ops = sum(hist)

            curr_ts = ns / 1e9
            tm_struct = time.localtime(curr_ts)
            date_str = time.strftime("%Y-%m-%d", tm_struct)
            us6 = f"{(ns // 1000) % 1_000_000:06d}"
            time_str = time.strftime("%H:%M:%S", tm_struct) + f".{us6}"
            pct_vals = [percentile_from_hist(hist, p) for p in PCT_POINTS]
            print_sample(date_str, time_str, disk, str(dev), verb, bn, ops, pct_vals)
            sys.stdout.flush()

            state[key] = (ns, curr_buckets)

        except (json.JSONDecodeError, KeyError, ValueError):
            continue
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    stream()

