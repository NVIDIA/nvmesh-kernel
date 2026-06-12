# tools/performance/kc — NVMesh/fio Performance Tooling

Scripts for capturing, comparing, and triage-ing NVMesh RAID-1 randwrite
performance regressions using fio and Linux perf flamegraphs.

## Directory Contents

| File | Purpose |
|---|---|
| `setup.sh` | Install FlameGraph and apply `nvmeibc` module parameters on client hosts |
| `workload.sh` | Run RAID-1 randwrite-4K benchmark with optional perf flamegraph capture |
| `perf_run.sh` | End-to-end driver: setup → baseline workload → candidate workload → compare |
| `compare_fio_perf_runs.py` | Parse fio JSON + flamegraph SVG pairs and produce a regression report |

---

## Quick Start — Full Pipeline

`perf_run.sh` orchestrates all four steps in one shot.

```bash
# Uses default output dir /tmp/raid1_perf_YYYYMMDD_HHMMSS
sudo ./perf_run.sh

# Explicit output dir
sudo ./perf_run.sh /tmp/my_perf_run
```

The script applies two `nvmeibc` module configurations (baseline then candidate),
runs `workload.sh` for each, and then calls `compare_fio_perf_runs.py` to write
a Markdown report to `OUTPUT_DIR/report.md`.

Default configurations compared:

| | `disk_locks_use_max_rd_atomic_limit` | `nvmeibc_io_pet_disable` |
|---|---|---|
| Baseline | `N` | `0` |
| Candidate | `Y` | `1` |

---

## setup.sh

Installs the FlameGraph toolkit and pushes a `modprobe.d` configuration to each
client host, then restarts `nvmeshclient` and verifies the parameters loaded.

```bash
# Use the built-in default modprobe options
./setup.sh

# Override with a custom options string
./setup.sh "options nvmeibc lock_ch_get_method=0 lock_shard_type=0 disk_locks_use_max_rd_atomic_limit=Y nvmeibc_io_pet_disable=1"
```

Target hosts are defined by the `HOSTS` array at the top of the script:

```bash
HOSTS=(nvme171 nvme172 nvme173 nvme180)
```

The script:
1. Clones `https://github.com/brendangregg/FlameGraph` to `~/FlameGraph` if not
   already present.
2. Writes the options string to `/etc/modprobe.d/nvmesh_lock_ch.conf` on each
   host via SSH.
3. Restarts `nvmeshclient` on each host.
4. Reads back `/sys/module/nvmeibc/parameters/<param>` and the startup log to
   confirm the parameters took effect.

---

## workload.sh

Runs the RAID-1 randwrite-4K benchmark, collecting fio JSON output and (by
default) a system-wide perf flamegraph per run.

```bash
./workload.sh OUTPUT_DIR [options]
```

Options:

```text
--vol-name NAME    NVMesh volume name          (default: perf-test-raid1)
--capacity SIZE    Volume capacity             (default: 60G)
--runs N           Number of fio runs          (default: 5)
--size SIZE        fio --size per job          (default: 30G)
--io-size SIZE     fio --io_size               (default: 60G)
--no-perf          Skip perf recording and flamegraph generation
```

Each run produces a `run_N/` subdirectory:

```text
OUTPUT_DIR/
  sysinfo/                   <- module params, NVMesh config, diagnostics
  run_1/
    fio.json                 <- machine-readable fio results
    fio_params.txt           <- fio invocation parameters
    fio_summary.txt          <- human-readable IOPS/latency summary
    perf.data                <- raw perf capture (if perf enabled)
    flamegraph.svg           <- FlameGraph SVG (if perf enabled)
    lock_diag_pre/           <- nvmeibc disk lock state before run
    lock_diag_post/          <- nvmeibc disk lock state after run
  run_2/
    ...
```

The script creates and attaches the NVMesh volume, runs the benchmark loop, then
detaches and deletes the volume.

### fio parameters (fixed)

```text
rw=randwrite   bs=4K   iodepth=32   numjobs=24
ioengine=libaio   direct=1   ramp_time=15s   group_reporting=1
```

### Perf collection

```bash
perf record -F 99 -a -g -o run_N/perf.data
```

Flamegraph generated with:

```bash
perf script -i perf.data | stackcollapse-perf.pl | flamegraph.pl > flamegraph.svg
```

---

## compare_fio_perf_runs.py

Standalone regression analysis tool (stdlib-only, no pip dependencies). Reads
`fio.json` and `flamegraph.svg` from two run trees and writes a Markdown report.

### Usage

Compare two individual runs:

```bash
./compare_fio_perf_runs.py base/run_2 candidate/run_2
```

Compare version directories (medians across `run_*` children):

```bash
./compare_fio_perf_runs.py base/ candidate/
```

Write human and machine-readable output:

```bash
./compare_fio_perf_runs.py base/ candidate/ \
  --output report.md \
  --json-out report.json
```

Options:

```text
--top N          Rows per perf section              (default: 20)
--focus REGEX    Regex for the focused NVMesh section
--strict         Fail if flamegraph.svg or perf.data are missing
--perf-timeout   Timeout for perf header parsing    (default: 5s)
```

### Input directory shape

```text
base/
  run_1/
    fio.json
    perf.data          <- optional; used for capture metadata only
    flamegraph.svg     <- primary perf source; required unless --no-perf was used
  run_2/
    ...
candidate/
  run_1/
    ...
```

### Report sections

1. **Workload Compatibility** — flags differences in operation, block size,
   iodepth, numjobs, ioengine, etc.
2. **Fio Regression Summary** — IOPS, bandwidth, latency, CPU, context
   switches.
3. **Perf Capture Summary** — event, host, kernel, CPU count, sample duration,
   flamegraph frame counts.
4. **Top Exclusive Function Regressions** — leaf frame delta sorted by
   `cycles/IO`.
5. **Top Focused NVMesh/NVMe Regressions** — same table filtered by `--focus`
   regex.
6. **Top Inclusive Call-Path Regressions** — full call path delta; useful for
   attributing generic leaf costs such as spinlocks to specific code paths.
7. **New Hotspots / Disappeared Hotspots** — symbols that crossed the
   `0.005%`/`0.05%` threshold between base and candidate.
8. **Warnings And Assumptions** — missing files, mismatched workloads,
   aggregation caveats.

### Practical reading order

1. **Workload Compatibility** — if `DIFF`, decide whether the comparison is
   still meaningful.
2. **Fio Regression Summary** — confirm direction and size of regression.
3. **Perf Capture Summary** — confirm event, host, kernel, CPU count.
4. **Top Focused NVMesh/NVMe Regressions** — fastest path to actionable
   symbols.
5. **Top Inclusive Call-Path Regressions** — explain generic leaf costs.
6. **New Hotspots** — code paths that appeared only in the candidate.
7. **Warnings And Assumptions** — check for missing files or caveats.

---

## Troubleshooting

### perf shows `[unknown]` symbols — fio must be recompiled

If the flamegraph or `perf report` output shows many `[unknown]` frames
belonging to fio, fio was built without frame pointers. Rebuild it with:

```bash
CFLAGS="-fno-omit-frame-pointer" make -j$(nproc)
```

Linux perf's default stack unwinding relies on frame pointers. When they are
omitted (the compiler default for `-O2`), perf cannot walk the user-space call
chain and collapses fio frames to `[unknown]`. The flag disables that
optimization so perf can unwind correctly.

After recompiling, re-run `workload.sh` to collect new `perf.data` and
flamegraphs, then re-run `compare_fio_perf_runs.py`.

### FlameGraph scripts not found

`workload.sh` expects `~/FlameGraph/stackcollapse-perf.pl` and
`~/FlameGraph/flamegraph.pl`. Run `setup.sh` to clone the repo, or:

```bash
git clone https://github.com/brendangregg/FlameGraph ~/FlameGraph
```

### perf.data symbolization is incomplete for kernel symbols

The `compare_fio_perf_runs.py` script uses `flamegraph.svg` (not raw
`perf.data`) as the authoritative source for symbol names because local
symbolization of kernel frames can be poor when the matching `build-id` files
are unavailable. `perf.data` is read only for capture metadata
(`perf report --header-only`).

---

## Design Notes for compare_fio_perf_runs.py

### Why fio.json, flamegraph.svg, and perf.data are used

**`fio.json`** is the source of measured regression. It contains IOPS,
bandwidth, total IO count, runtime, submission/completion/total latency,
percentiles, fio CPU, context switches, and disk utilization.

**`flamegraph.svg`** is the source of symbolized perf cost. It already
contains symbolized frames in machine-readable `<title>` elements:

```xml
<title>function_name (123,456 samples, 0.12%)</title>
```

This avoids the local symbolization problems seen with raw `perf.data` in the
original `NVMESH-9235` environment.

**`perf.data`** is used only for capture metadata: event name, hostname,
kernel release, perf version, CPU count, sample duration, and the original
`perf record` command line.

### Cycles/IO normalization

`cycles/IO = leaf_frame_samples / fio_total_ios`

This matters because runs can have slightly different durations or IO counts. A
raw percent delta alone is misleading when one run did more work or ran longer.

- `Delta pp` — how CPU share changed.
- `Delta cycles/IO` — how much more sampled work is associated with each IO.

Prioritize functions with both a positive `Delta cycles/IO` and a plausible
relation to the fio regression.

### Median aggregation

When comparing version directories, each `run_*` child is parsed independently
and then aggregated. Medians are used because fio/perf runs can have outliers.
If a symbol is missing from a run its cost is treated as zero. The `Seen B/C`
column shows how many base and candidate runs contained the symbol.

### Data flow

```text
main()
  parse_args()
  analyze_group(base)
    collect_run_paths()
    analyze_run() for each run
      parse_fio()
      parse_flamegraph()
      parse_perf_header()
    aggregate_fio()
    aggregate_costs()
  analyze_group(candidate)
  build_report()
    workload_rows()
    fio_metric_rows()
    perf_capture_rows()
    delta_rows()
    hotspot_rows()
  print Markdown
  optionally write Markdown and JSON
```

### Key dataclasses

```text
FioSummary      — parsed fio workload fields, metrics, percentiles
Frame           — one flamegraph frame: name, samples, percent, geometry, leaf flag
Cost            — normalized cost: percent, samples, cycles/IO, seen count
FlameSummary    — flamegraph totals plus leaf and inclusive cost maps
PerfHeader      — metadata from perf report --header-only
RunAnalysis     — one run directory: fio + flame + perf metadata + warnings
GroupAnalysis   — one input argument: single run or aggregated version directory
```

### How to extend the script

**Add a fio metric:** `parse_fio()` → `FIO_METRIC_ORDER` → `metric_label()` →
`format_metric()`.

**Add a workload compatibility field:** `WORKLOAD_KEYS` and (if needed)
`FioSummary.workload_value()`.

**Change the default focus regex:** `DEFAULT_FOCUS_RE`. Keep it broad enough
for first-pass triage but avoid accidental matches (e.g. `lock` also matches
`clock`).

**Change hotspot thresholds:** `NEW_HOTSPOT_MIN_PCT` and
`ABSENT_HOTSPOT_MAX_PCT`.

**Add a report section:** `build_report()`. If the section should appear in
JSON, update the `json_report` dictionary.

### Known limitations

- Flamegraph parser assumes standard FlameGraph SVG structure.
- Parent/child reconstruction is geometry-based, not from original folded stack
  text.
- The script does not regenerate flamegraphs from `perf.data`.
- `cycles/IO` assumes the flamegraph sample count is comparable between runs.
- Version aggregation is median-based, not matched-pair based.
- Multiple-job fio percentiles are medianed, not statistically merged.
- The report is diagnostic; it does not prove causality by itself.
