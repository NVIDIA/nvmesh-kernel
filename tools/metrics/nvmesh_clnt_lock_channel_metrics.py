#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import json
import math
import sys
import time

"""
Usage:
./pager.py --mode msg-stream-json -t 'now-1m' -l nvmeibc_trace_metrics \
  -f 'func=disk_periodic_lock_channel_work_func' \
  | python3 ./nvmesh_clnt_lock_channel_metrics.py

Reads pager JSON (one object per line) for the four lock-channel periodic
metric traces and prints per-interval deltas:

  info_disk_lock_channel_usage
      → ops delta and rate per channel
  info_disk_lock_channel_latency
      → latency percentiles per channel (delta histogram)
  info_disk_lock_channel_deferred_queue_max
      → max deferred-queue depth per channel
  info_disk_lock_channel_deferred_latency
      → deferred-op latency percentiles per channel (delta histogram)

Example output:
  2026-03-17 12:00:10.123456  DISK=S3HCNX0K800441.1
    lock_opr_count         ch0=1024 (+102/s)  ch1=512 (+51/s)
    lock_opr_deferred_max  ch0=max:3(Δ1)  ch1=max:0(Δ0)
    lock_opr_latency       ch0  OPS=1024  P50=32.8us  P90=65.5us  P95=131.1us  P99=524.3us
    lock_opr_def_latency   ch0  OPS=8     P50=65.5us  P90=131.1us P95=131.1us  P99=262.1us
"""

U64_ROLLOVER = 1 << 64
LAT_BINS     = 16
LAT_SHIFT_NS = 10   # matches nvmesh_metric_latency_histogram_update (shift=10)
PCT_POINTS   = [50, 90, 95, 99]

TRACE_USAGE   = "info_disk_lock_channel_usage"
TRACE_LAT     = "info_disk_lock_channel_latency"
TRACE_DEF_MAX = "info_disk_lock_channel_deferred_queue_max"
TRACE_DEF_LAT = "info_disk_lock_channel_deferred_latency"


def u64_delta(curr, prev):
    d = curr - prev
    return d if d >= 0 else d + U64_ROLLOVER


def bin_to_ns(idx):
    return 1 << (idx + LAT_SHIFT_NS)


def percentile_us(delta_bins, p):
    total = sum(delta_bins)
    if total <= 0:
        return 0.0
    target = int(math.ceil(total * p / 100.0))
    cumulative = 0
    for i, count in enumerate(delta_bins):
        cumulative += count
        if cumulative >= target:
            return bin_to_ns(i) / 1000.0   # ns → us
    return bin_to_ns(LAT_BINS - 1) / 1000.0


def parse_lat_bins(js):
    """Parse @NS_12_BINS — a JSON-array string of 16 u64 counts."""
    raw = js.get("NS_12_BINS", "[]")
    bins = json.loads(raw)
    while len(bins) < LAT_BINS:
        bins.append(0)
    return [int(b) for b in bins[:LAT_BINS]]


def parse_flex_arr(js):
    """Parse @LOCK_CH_OPR_ARR — a variable-length JSON-array string."""
    raw = js.get("LOCK_CH_OPR_ARR", "[]")
    return [int(v) for v in json.loads(raw)]


def fmt_ts(ns):
    t = ns / 1e9
    s = time.localtime(t)
    us6 = f"{(ns // 1000) % 1_000_000:06d}"
    return time.strftime("%Y-%m-%d", s), time.strftime("%H:%M:%S", s) + f".{us6}"


def pct_line(delta_bins):
    return "  ".join(f"P{p}={percentile_us(delta_bins, p):.1f}us" for p in PCT_POINTS)


def stream():
    # Keyed by disk name: (prev_ns, [prev_vals])
    count_state   = {}
    def_max_state = {}
    # Keyed by (disk, channel_idx): (prev_ns, [prev_bins])
    lat_state     = {}
    def_lat_state = {}

    for raw_line in sys.stdin:
        try:
            js = json.loads(raw_line)
        except json.JSONDecodeError:
            continue

        try:
            trace = js.get("trace_name", "")
            ns    = int(js["nanoseconds"])
            disk  = js["DISK_NAME"]
        except (KeyError, ValueError):
            continue

        date_str, time_str = fmt_ts(ns)
        prefix = f"{date_str} {time_str}  DISK={disk}"
        dt_s   = ns / 1e9

        try:
            # ── monotonic op-count array ──────────────────────────────────────
            if trace == TRACE_USAGE:
                curr = parse_flex_arr(js)
                if disk in count_state:
                    prev_ns, prev = count_state[disk]
                    dt = dt_s - prev_ns / 1e9
                    if dt > 0 and len(curr) == len(prev):
                        parts = []
                        for i, (c, p) in enumerate(zip(curr, prev)):
                            d = u64_delta(c, p)
                            parts.append(f"ch{i}={d}(+{d/dt:.0f}/s)")
                        print(f"{prefix}  lock_opr_count  {'  '.join(parts)}")
                        sys.stdout.flush()
                count_state[disk] = (ns, curr)

            # ── deferred queue max array ──────────────────────────────────────
            elif trace == TRACE_DEF_MAX:
                curr = parse_flex_arr(js)
                if disk in def_max_state:
                    prev_ns, prev = def_max_state[disk]
                    if len(curr) == len(prev):
                        parts = [f"ch{i}=max:{c}(Δ{u64_delta(c,p)})"
                                 for i, (c, p) in enumerate(zip(curr, prev))]
                        print(f"{prefix}  lock_opr_deferred_max  {'  '.join(parts)}")
                        sys.stdout.flush()
                def_max_state[disk] = (ns, curr)

            # ── latency histogram (opr or deferred) ───────────────────────────
            elif trace in (TRACE_LAT, TRACE_DEF_LAT):
                ch_idx     = int(js["INT"])
                curr_bins  = parse_lat_bins(js)
                key        = (disk, ch_idx)
                state      = lat_state if trace == TRACE_LAT else def_lat_state
                label      = ("lock_opr_latency" if trace == TRACE_LAT
                              else "lock_opr_def_latency")

                if key in state:
                    prev_ns, prev_bins = state[key]
                    dt = dt_s - prev_ns / 1e9
                    if dt > 0:
                        delta = [u64_delta(c, p)
                                 for c, p in zip(curr_bins, prev_bins)]
                        ops = sum(delta)
                        print(f"{prefix}  {label}  ch{ch_idx}"
                              f"  OPS={ops}  {pct_line(delta)}")
                        sys.stdout.flush()
                state[key] = (ns, curr_bins)

        except (KeyError, ValueError, TypeError):
            continue
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    stream()
