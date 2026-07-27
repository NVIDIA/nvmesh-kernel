#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Benchmark raid-1 randwrite-4K with optional system-wide perf flamegraph collection.
# Usage: ./run_raid1_perf.sh OUTPUT_DIR [--vol-name NAME] [--capacity SIZE] [--runs N] [--no-perf]
set -euo pipefail

FLAMEGRAPH_DIR="${HOME}/FlameGraph"
NUM_RUNS=5
VOL_NAME="perf-test-raid1"
CAPACITY="60G"
FIO_SIZE="30G"
FIO_IO_SIZE="60G"
NO_PERF=false

usage() {
    echo "Usage: $0 OUTPUT_DIR [--vol-name NAME] [--capacity SIZE] [--runs N] [--no-perf]"
    echo ""
    echo "  OUTPUT_DIR   Directory to write per-run results (created if absent)"
    echo "  --vol-name   NVMesh volume name  (default: ${VOL_NAME})"
    echo "  --capacity   Volume capacity     (default: ${CAPACITY})"
    echo "  --runs       Number of fio runs  (default: ${NUM_RUNS})"
    echo "  --size       fio --size per job  (default: ${FIO_SIZE})"
    echo "  --io-size    fio --io_size       (default: ${FIO_IO_SIZE})"
    echo "  --no-perf    Skip perf recording and flamegraph generation"
    exit 1
}

[[ $# -lt 1 ]] && usage
OUTPUT_DIR="$1"; shift

while [[ $# -gt 0 ]]; do
    case "$1" in
        --vol-name=*)  VOL_NAME="${1#*=}";    shift ;;
        --vol-name)
            [[ $# -ge 2 ]] || { echo "ERROR: --vol-name requires a value" >&2; exit 1; }
            VOL_NAME="$2"; shift 2 ;;
        --capacity=*)  CAPACITY="${1#*=}";    shift ;;
        --capacity)
            [[ $# -ge 2 ]] || { echo "ERROR: --capacity requires a value" >&2; exit 1; }
            CAPACITY="$2"; shift 2 ;;
        --runs=*)      NUM_RUNS="${1#*=}";    shift ;;
        --runs)
            [[ $# -ge 2 ]] || { echo "ERROR: --runs requires a value" >&2; exit 1; }
            NUM_RUNS="$2"; shift 2 ;;
        --size=*)      FIO_SIZE="${1#*=}";    shift ;;
        --size)
            [[ $# -ge 2 ]] || { echo "ERROR: --size requires a value" >&2; exit 1; }
            FIO_SIZE="$2"; shift 2 ;;
        --io-size=*)   FIO_IO_SIZE="${1#*=}"; shift ;;
        --io-size)
            [[ $# -ge 2 ]] || { echo "ERROR: --io-size requires a value" >&2; exit 1; }
            FIO_IO_SIZE="$2"; shift 2 ;;
        --no-perf)     NO_PERF=true; shift ;;
        *) echo "ERROR: Unknown argument: $1" >&2; usage ;;
    esac
done

# Validate NUM_RUNS as positive integer
[[ "$NUM_RUNS" =~ ^[1-9][0-9]*$ ]] || { echo "ERROR: --runs must be a positive integer (got: ${NUM_RUNS})" >&2; exit 1; }

# Early OUTPUT_DIR creation
mkdir -p "${OUTPUT_DIR}" || { echo "ERROR: Cannot create OUTPUT_DIR '${OUTPUT_DIR}'" >&2; exit 1; }

# Dependency checks
for cmd in fio nvmesh python3; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: '$cmd' not found in PATH" >&2; exit 1; }
done
if [[ "${NO_PERF}" == false ]]; then
    command -v perf >/dev/null 2>&1 || { echo "ERROR: 'perf' not found in PATH" >&2; exit 1; }
    for fg_script in stackcollapse-perf.pl flamegraph.pl; do
        [[ -f "${FLAMEGRAPH_DIR}/${fg_script}" ]] || {
            echo "ERROR: FlameGraph script '${fg_script}' not found in ${FLAMEGRAPH_DIR}" >&2
            echo "       Clone https://github.com/brendangregg/FlameGraph to ~/FlameGraph" >&2
            exit 1
        }
    done
fi

# ---------------------------------------------------------------------------
# Volume setup — create and attach the raid-1 volume if not already present
# ---------------------------------------------------------------------------
DEVICE="/dev/nvmesh/${VOL_NAME}"
CLIENT_ID="$(hostname -f)"

setup_volume() {
    if [[ -b "${DEVICE}" ]]; then
        echo "Volume device ${DEVICE} already exists — skipping creation."
        return 0
    fi

    echo "Creating NVMesh raid-1 volume '${VOL_NAME}' (capacity=${CAPACITY}) ..."
    nvmesh -y volume create \
        -n "${VOL_NAME}" \
        -c "${CAPACITY}" \
        --raid-level 1 \
        --number-of-mirrors 1 \
        --no-crc-enabled \
        --wait --timeout 300

    echo "Attaching volume '${VOL_NAME}' to client '${CLIENT_ID}' ..."
    nvmesh -y client attach \
        --id "${CLIENT_ID}" \
        --volume "${VOL_NAME}" \
        --mode SHARED_READ_WRITE \
        --wait --timeout 120

    [[ -b "${DEVICE}" ]] || { echo "ERROR: ${DEVICE} did not appear after attach" >&2; exit 1; }
    echo "Volume ready: ${DEVICE}"
}

teardown_volume() {
    echo "Detaching volume '${VOL_NAME}' from client '${CLIENT_ID}' ..."
    nvmesh -y client detach \
        --id "${CLIENT_ID}" \
        --volume "${VOL_NAME}" \
        --wait --timeout 120 \
        || echo "  WARNING: detach failed or volume was not attached" >&2

    echo "Deleting volume '${VOL_NAME}' ..."
    nvmesh -y volume delete \
        -n "${VOL_NAME}" \
        --wait --timeout 120 \
        || echo "  WARNING: volume delete failed or volume did not exist" >&2

    echo "Teardown complete."
}

setup_volume

# ---------------------------------------------------------------------------
# Dump system, module, environment and volume/client diagnostics before runs
# ---------------------------------------------------------------------------
dump_diagnostics() {
    local D="${OUTPUT_DIR}"
    local SI="${D}/sysinfo"
    mkdir -p "${SI}"
    echo "Saving diagnostics to ${D} ..."

    # --- host / kernel / NVMesh version ---
    hostname -f                                     > "${SI}/hostname.txt"       2>&1 || true
    uname -a                                        > "${SI}/uname.txt"          2>&1 || true
    nvmesh --version                                > "${SI}/nvmesh_version.txt" 2>&1 || true

    # --- loaded nvmei* modules ---
    lsmod | grep -E '^nvmei'                        > "${SI}/lsmod_nvmei.txt"    2>&1 || true

    # --- runtime module parameters (one file per module) ---
    for _mod in nvmeiba nvmeibc nvmeibs \
                nvmeib_common nvmeib_common_public \
                nvmeib_common_mlx4_public nvmeib_common_mlx5_public; do
        local _pdir="/sys/module/${_mod}/parameters"
        [[ -d "${_pdir}" ]] || continue
        {
            for _p in "${_pdir}"/*; do
                [[ -f "${_p}" ]] || continue
                printf "%-44s = %s\n" "$(basename "${_p}")" "$(cat "${_p}" 2>/dev/null)"
            done
        } > "${SI}/module_params_${_mod}.txt"
        echo "  module params: ${_mod}"
    done

    # --- modinfo ---
    modinfo nvmeibc                                 > "${SI}/modinfo_nvmeibc.txt" 2>&1 || true

    # --- startup module-params snapshot written by nvmeshclient service ---
    local _startup="/var/log/nvmesh/on_startup_module_params.client"
    if [[ -r "${_startup}" ]]; then
        cp "${_startup}" "${SI}/on_startup_module_params.txt"
        echo "  startup params: ${_startup}"
    fi

    # --- NVMesh config files ---
    for _cf in \
        /etc/nvmesh/nvmesh.conf \
        /etc/nvmesh/.nvmesh.conf \
        /etc/nvmesh/configs/nvmeibc/.nvmesh.conf \
        /etc/nvmesh/configs/nvmeibc/.mgmt.nvmesh.conf \
        /etc/modprobe.d/nvmesh.conf \
        /etc/modprobe.d/nvmesh_for_nvmeshum.conf \
        /etc/sysctl.d/98-nvmesh.conf \
        /etc/depmod.d/zz02-nvmesh.conf \
        /etc/udev/rules.d/60-nvmesh.rules
    do
        [[ -r "${_cf}" ]] || continue
        local _fname
        _fname="$(printf '%s' "${_cf}" | sed 's|/|_|g; s|^_||')"
        cp "${_cf}" "${SI}/${_fname}"
        echo "  config: ${_cf}"
    done

    # --- sysctl values set by NVMesh ---
    sysctl -a 2>/dev/null \
        | grep -E '(rp_filter|arp_announce|arp_ignore|min_free_kbytes)' \
        > "${SI}/sysctl_nvmesh.txt" || true

    # --- nvmeibc proc interface ---
    if [[ -r /proc/nvmeibc/version ]]; then
        cp /proc/nvmeibc/version "${D}/nvmeibc_version.txt"
        echo "  /proc/nvmeibc/version"
    else
        echo "  WARNING: /proc/nvmeibc/version not readable" >&2
    fi

    local _vsf=""
    if [[ -d /proc/nvmeibc/volumes ]]; then
        for _f in /proc/nvmeibc/volumes/*/status; do
            [[ -f "${_f}" ]] || continue
            grep -q "${VOL_NAME}" "${_f}" 2>/dev/null || continue
            _vsf="${_f}"
            break
        done
    fi
    if [[ -n "${_vsf}" ]]; then
        cp "${_vsf}" "${D}/nvmeibc_volume_status.txt"
        echo "  ${_vsf}"
    else
        echo "  WARNING: /proc/nvmeibc/volumes/<id>/status not found for '${VOL_NAME}'" >&2
    fi

    # --- disk lock diagnostics (baseline, pre-runs) ---
    save_disk_lock_diag "${SI}/disk_lock_diag"

    # --- nvmesh management-plane snapshots ---
    nvmesh volume show -n "${VOL_NAME}"  > "${D}/nvmesh_volume_show.txt" 2>&1 \
        && echo "  nvmesh volume show" \
        || echo "  WARNING: nvmesh volume show failed" >&2
    nvmesh client show -n "${CLIENT_ID}" > "${D}/nvmesh_client_show.txt" 2>&1 \
        && echo "  nvmesh client show" \
        || echo "  WARNING: nvmesh client show failed" >&2

    echo "  Done."
}

# ---------------------------------------------------------------------------
# Capture lock diagnostics for all nvmeibc disks
# save_disk_lock_diag DEST_DIR [PROC_ROOT]
# ---------------------------------------------------------------------------
save_disk_lock_diag() {
    local dest="$1"
    local proc_root="${2:-/proc/nvmeibc}"
    local disks_dir="${proc_root}/disks"
    mkdir -p "${dest}"
    if [[ ! -d "${disks_dir}" ]]; then
        echo "  WARNING: ${disks_dir} not found — skipping lock diag" >&2
        return 0
    fi
    local found=0
    for disk_dir in "${disks_dir}"/*/; do
        [[ -d "${disk_dir}" ]] || continue
        local disk_name
        disk_name="$(basename "${disk_dir}")"
        if [[ -r "${disk_dir}/status" ]]; then
            cp "${disk_dir}/status"      "${dest}/${disk_name}_status.txt"
            found=$((found + 1))
        fi
        if [[ -r "${disk_dir}/status.json" ]]; then
            cp "${disk_dir}/status.json" "${dest}/${disk_name}_status.json"
        fi
        if [[ -r "${disk_dir}/lock_channels" ]]; then
            cp "${disk_dir}/lock_channels" "${dest}/${disk_name}_lock_channels.json"
        fi
    done
    if [[ "${found}" -eq 0 ]]; then
        echo "  WARNING: no disk status files found under ${disks_dir}" >&2
    else
        # Consolidated view: "Lock diag" from text status (older format) and
        # lock_diag_* fields from lock_channels JSON (3.5.0-r7+ format)
        {
            grep -h "Lock diag"  "${dest}"/*_status.txt        2>/dev/null || true
            grep -h "lock_diag_" "${dest}"/*_lock_channels.json 2>/dev/null || true
        } > "${dest}/lock_diag_summary.txt"
        echo "  lock diag: ${found} disk(s) → ${dest}"
    fi
}

dump_diagnostics

# ---------------------------------------------------------------------------
# Benchmark loop
# ---------------------------------------------------------------------------
mkdir -p "${OUTPUT_DIR}"

PERF_PID=""

for run in $(seq 1 "${NUM_RUNS}"); do
    RUN_DIR="${OUTPUT_DIR}/run_${run}"
    mkdir -p "${RUN_DIR}"

    echo ""
    echo "===== Run ${run}/${NUM_RUNS}  dir=${RUN_DIR} ====="

    # 0. Snapshot lock diag before the run
    save_disk_lock_diag "${RUN_DIR}/lock_diag_pre"

    # 1. Start system-wide perf recording in the background.
    #    The script runs as root, so perf is launched directly — no sudo wrapper
    #    needed, and $! is perf's actual PID with no signal-forwarding indirection.
    if [[ "${NO_PERF}" == false ]]; then
        echo "  [perf] starting system-wide recording ..."
        perf record -F 99 -a -g -o "${RUN_DIR}/perf.data" &
        PERF_PID=$!
        # Ensure perf is stopped if fio fails or the script is interrupted
        trap "kill -INT '${PERF_PID}' 2>/dev/null || true; wait '${PERF_PID}' 2>/dev/null || true" EXIT ERR INT TERM
        sleep 1   # give perf a moment to initialize before fio starts
    fi

    # 2. Run fio — JSON to fio.json, human-readable stdout to fio_output.txt
    echo "  [fio] running randwrite 4K iodepth=32 numjobs=24 ..."
    # Save parameters before running so they're present even if fio fails
    cat > "${RUN_DIR}/fio_params.txt" <<EOF
filename=${DEVICE}
rw=randwrite
bs=4K
iodepth=32
numjobs=24
size=${FIO_SIZE}
io_size=${FIO_IO_SIZE}
ioengine=libaio
output-format=json  # JSON written to fio.json; human-readable summary in fio_summary.txt
group_reporting=1
exitall_on_error=1
direct=1
ramp_time=15
name=randwrite_4K
EOF
    sudo fio \
        --filename="${DEVICE}" \
        --rw=randwrite \
        --bs=4K \
        --iodepth=32 \
        --numjobs=24 \
        --size="${FIO_SIZE}" \
        --io_size="${FIO_IO_SIZE}" \
        --ioengine=libaio \
        --output-format=json \
        --output="${RUN_DIR}/fio.json" \
        --group_reporting \
        --exitall_on_error=1 \
        --direct=1 \
        --ramp_time=15 \
        --name=randwrite_4K

    # 3. Disarm trap then stop perf gracefully (SIGINT causes it to finalize perf.data)
    if [[ "${NO_PERF}" == false ]]; then
        trap - EXIT ERR INT TERM
        echo "  [perf] stopping recording (pid=${PERF_PID}) ..."
        kill -INT "${PERF_PID}" 2>/dev/null || true
        wait "${PERF_PID}" 2>/dev/null || true
        echo "  [perf] perf.data written to ${RUN_DIR}/perf.data"
    fi

    # 4. Snapshot lock diag after the run
    save_disk_lock_diag "${RUN_DIR}/lock_diag_post"

    # 6. Generate flamegraph
    if [[ "${NO_PERF}" == false ]]; then
        echo "  [flamegraph] generating ${RUN_DIR}/flamegraph.svg ..."
        if sudo perf script -i "${RUN_DIR}/perf.data" \
                | "${FLAMEGRAPH_DIR}/stackcollapse-perf.pl" \
                | "${FLAMEGRAPH_DIR}/flamegraph.pl" \
                > "${RUN_DIR}/flamegraph.svg"; then
            echo "  [flamegraph] done: ${RUN_DIR}/flamegraph.svg"
        else
            echo "  [flamegraph] WARNING: flamegraph generation failed for run ${run}" >&2
        fi
    fi

    # 7. Summarize fio results from JSON
    IOPS=$(python3 -c "
import json, sys
try:
    with open('${RUN_DIR}/fio.json') as f:
        d = json.load(f)
    w = d['jobs'][0]['write']
    clat = w.get('clat_ns', w.get('lat_ns', {}))
    pcts = clat.get('percentile', {})
    lines = [
        f\"IOPS:        {w['iops']:>12,.0f}\",
        f\"BW (KiB/s): {w['bw']:>12,.0f}\",
        f\"lat_mean_us:{clat.get('mean', float('nan'))/1000:>12.1f}\",
        f\"lat_stdev_us:{clat.get('stddev', float('nan'))/1000:>11.1f}\",
    ]
    if pcts:
        for k in ('99.000000', '99.900000', '99.990000'):
            if k in pcts:
                lines.append(f\"lat_p{k.rstrip('0').rstrip('.')}_us:{pcts[k]/1000:>10.1f}\")
    with open('${RUN_DIR}/fio_summary.txt', 'w') as sf:
        sf.write('\n'.join(lines) + '\n')
    print(f\"{w['iops']:,.0f}\")
except Exception as e:
    with open('${RUN_DIR}/fio_summary.txt', 'w') as sf:
        sf.write(f'parse error: {e}\n')
    print('parse-error')
" 2>&1)
    echo "  [result] Run ${run} IOPS: ${IOPS}"
done

echo ""
echo "===== Summary ====="
python3 - "${OUTPUT_DIR}" "${NUM_RUNS}" <<'PYEOF'
import json, sys, os

out_dir, num_runs = sys.argv[1], int(sys.argv[2])
iops_list = []
for n in range(1, num_runs + 1):
    path = os.path.join(out_dir, f"run_{n}", "fio.json")
    try:
        with open(path) as f:
            d = json.load(f)
        iops = d["jobs"][0]["write"]["iops"]
        iops_list.append(iops)
        print(f"  run {n:2d}: {iops:>12,.0f} IOPS")
    except Exception as e:
        print(f"  run {n:2d}: error ({e})")

if iops_list:
    avg = sum(iops_list) / len(iops_list)
    print(f"  {'avg':>6}: {avg:>12,.0f} IOPS  (across {len(iops_list)} runs)")
PYEOF
echo ""
echo "Results in: ${OUTPUT_DIR}"

teardown_volume
