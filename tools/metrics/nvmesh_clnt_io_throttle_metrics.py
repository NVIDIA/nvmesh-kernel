#!/usr/bin/env python3
import json
import sys
import time

"""
Usage:
./pager.py --mode msg-stream-json -t 'now-1m' -l nvmeibc_trace_metrics \
  -f 'func=dev_periodic_io_throttle_work_func' \
  | python3 ./nvmesh_clnt_io_throttle_metrics.py

Reads pager JSON (one object per line) for io_throttle periodic metric traces
and prints per-interval deltas:

  info_dev_io_throttle_event_count
      → per-cpu monotonic count of throttled IOs (delta and rate)
  info_dev_io_throttle_latency
      → per-cpu highres_histogram (p50/p95/p99 in microseconds; exact max in
        HIGHRES_HISTOGRAM.MAX_TICKS with tsc_khz for conversion)
  info_dev_io_throttle_num_throttled
      → per-cpu gauge of IOs currently waiting in the throttle queue

Example output:
  2026-03-17 12:00:10.123456  DEV=vol0
    io_throttle_event_count  cpu0=0(+0/s)  cpu1=102(+10/s)  cpu2=0(+0/s)  cpu3=45(+5/s)
    io_throttle_latency      cpu1: p50=12us p95=45us p99=78us
"""

U64_ROLLOVER = 1 << 64

TRACE_EVENT_COUNT   = "info_dev_io_throttle_event_count"
TRACE_LATENCY       = "info_dev_io_throttle_latency"
TRACE_NUM_THROTTLED = "info_dev_io_throttle_num_throttled"

# Matches nvmesh_metric_highres_histogram (nvmeib_metrics.h)
HIGHRES_HIST_SHIFT = 7
HIGHRES_HIST_BINS = 28


def u64_delta(curr, prev):
    d = curr - prev
    return d if d >= 0 else d + U64_ROLLOVER


def parse_cpu_arr(js):
    """Parse @IO_THROTTLE_CPU_ARR — a JSON-array string of per-cpu u64 values."""
    raw = js.get("IO_THROTTLE_CPU_ARR", "[]")
    return [int(v) for v in json.loads(raw)]


def parse_highres_bins(js):
    """Parse HIGHRES_HISTOGRAM.HIST_BINS — JSON array of 28 u64 bin counts."""
    raw = js.get("HIGHRES_HISTOGRAM.HIST_BINS", "[]")
    return [int(v) for v in json.loads(raw)]


def ticks_to_ns(ticks, tsc_khz):
    if tsc_khz == 0:
        return 0.0
    return float(ticks) * 1_000_000.0 / float(tsc_khz)


def bin_mid_ns(i, tsc_khz):
    """Approximate bin center in ns for percentile reporting (matches nvmesh_highres_hist bin ranges)."""
    if i == 0:
        lo_ticks, hi_ticks = 0, (1 << (HIGHRES_HIST_SHIFT + 1)) - 1
    else:
        lo_ticks = 1 << (i + HIGHRES_HIST_SHIFT)
        hi_ticks = (1 << (i + HIGHRES_HIST_SHIFT + 1)) - 1
    mid_ticks = (lo_ticks + hi_ticks) // 2
    return ticks_to_ns(mid_ticks, tsc_khz)


def percentile_us(bins, tsc_khz, pct):
    """Return pct-th percentile in microseconds from a highres histogram."""
    total = sum(bins)
    if total == 0:
        return None
    target = total * pct / 100.0
    cumulative = 0
    for i, count in enumerate(bins):
        cumulative += count
        if cumulative >= target:
            return bin_mid_ns(i, tsc_khz) / 1000.0
    return None


def fmt_ts(ns):
    t = ns / 1e9
    s = time.localtime(t)
    us6 = f"{(ns // 1000) % 1_000_000:06d}"
    return time.strftime("%Y-%m-%d", s), time.strftime("%H:%M:%S", s) + f".{us6}"


def stream():
    # Keyed by dev name: (prev_ns, [prev_vals])
    count_state = {}
    # Keyed by (dev, cpu_id): list of bins
    latency_state = {}

    for raw_line in sys.stdin:
        try:
            js = json.loads(raw_line)
        except json.JSONDecodeError:
            continue

        try:
            trace = js.get("trace_name", "")
            ns    = int(js["nanoseconds"])
            dev   = js["DEV_NAME"]
        except (KeyError, ValueError):
            continue

        date_str, time_str = fmt_ts(ns)
        prefix = f"{date_str} {time_str}  DEV={dev}"
        dt_s   = ns / 1e9

        try:
            if trace == TRACE_EVENT_COUNT:
                curr = parse_cpu_arr(js)
                if dev in count_state:
                    prev_ns, prev = count_state[dev]
                    dt = dt_s - prev_ns / 1e9
                    if dt > 0 and len(curr) == len(prev):
                        parts = []
                        for i, (c, p) in enumerate(zip(curr, prev)):
                            d = u64_delta(c, p)
                            if d > 0 or p > 0:
                                parts.append(f"cpu{i}={d}(+{d/dt:.0f}/s)")
                        if parts:
                            print(f"{prefix}  io_throttle_event_count  {'  '.join(parts)}")
                            sys.stdout.flush()
                count_state[dev] = (ns, curr)

            elif trace == TRACE_LATENCY:
                try:
                    cpu_id = int(js["cpu_index"])
                    tsc_khz = int(js["HIGHRES_HISTOGRAM.TSC_KHZ"])
                except (KeyError, ValueError):
                    continue
                bins = parse_highres_bins(js)
                if len(bins) == HIGHRES_HIST_BINS and any(bins):
                    p50 = percentile_us(bins, tsc_khz, 50)
                    p95 = percentile_us(bins, tsc_khz, 95)
                    p99 = percentile_us(bins, tsc_khz, 99)
                    parts = []
                    if p50 is not None:
                        parts.append(f"p50={p50:.0f}us")
                    if p95 is not None:
                        parts.append(f"p95={p95:.0f}us")
                    if p99 is not None:
                        parts.append(f"p99={p99:.0f}us")
                    if parts:
                        print(f"{prefix}  io_throttle_latency  cpu{cpu_id}: {' '.join(parts)}")
                        sys.stdout.flush()

            elif trace == TRACE_NUM_THROTTLED:
                curr = parse_cpu_arr(js)
                parts = []
                for i, v in enumerate(curr):
                    if v > 0:
                        parts.append(f"cpu{i}: n={v}")
                if parts:
                    print(f"{prefix}  io_throttle_num_throttled  {'  '.join(parts)}")
                    sys.stdout.flush()

        except (KeyError, ValueError, TypeError):
            continue
        except KeyboardInterrupt:
            break


if __name__ == "__main__":
    stream()
