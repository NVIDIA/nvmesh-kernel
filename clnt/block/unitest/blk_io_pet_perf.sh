#!/usr/bin/env bash

set -euo pipefail

UNITEST_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PERF_DATA="$UNITEST_DIR/perf.data"
PERF_BIN="/usr/bin/perf"
TEST_NAME="unitest_MirrorMultiThreadedIO"
IO_PET_BUFFER_SIZE="${IO_PET_BUFFER_SIZE:-4096}"
IO_PET_VERBOSE="${IO_PET_VERBOSE:-1}"

usage()
{
	cat <<EOF
Usage: $0 [options]

Options:
  --nvmeibc_io_pet_buffer_size N  Set simulator IO PET buffer size in bytes.
  --nvmeibc_io_pet_verbose 0|1    Enable or disable verbose IO PET records.
  --io-pet-buffer-size N          Alias for --nvmeibc_io_pet_buffer_size.
  --io-pet-verbose 0|1            Alias for --nvmeibc_io_pet_verbose.
  -h, --help                      Show this help.

Defaults:
  nvmeibc_io_pet_buffer_size=$IO_PET_BUFFER_SIZE
  nvmeibc_io_pet_verbose=$IO_PET_VERBOSE
EOF
}

while [ $# -gt 0 ]; do
	case "$1" in
		--nvmeibc_io_pet_buffer_size|--io-pet-buffer-size)
			if [ $# -lt 2 ]; then
				echo "Error: $1 requires a value"
				exit 1
			fi
			IO_PET_BUFFER_SIZE="$2"
			shift 2
			;;
		--nvmeibc_io_pet_verbose|--io-pet-verbose)
			if [ $# -lt 2 ]; then
				echo "Error: $1 requires a value"
				exit 1
			fi
			IO_PET_VERBOSE="$2"
			shift 2
			;;
		-h|--help)
			usage
			exit 0
			;;
		*)
			echo "Error: unknown argument: $1"
			usage
			exit 1
			;;
	esac
done

case "$IO_PET_BUFFER_SIZE" in
	''|*[!0-9]*)
		echo "Error: nvmeibc_io_pet_buffer_size must be a non-negative integer"
		exit 1
		;;
esac

case "$IO_PET_VERBOSE" in
	0|1) ;;
	*)
		echo "Error: nvmeibc_io_pet_verbose must be 0 or 1"
		exit 1
		;;
esac

if [ ! -x "$PERF_BIN" ]; then
	echo "Error: $PERF_BIN is not executable"
	exit 1
fi

if ! command -v rg >/dev/null 2>&1; then
	echo "Error: rg is required to filter perf report output"
	exit 1
fi

JOBS="${JOBS:-}"
if [ -z "$JOBS" ]; then
	JOBS="$(nproc 2>/dev/null || echo 10)"
fi

CONF_FILE="$(mktemp "${TMPDIR:-/tmp}/blk_io_pet_perf.XXXXXX.cfg")"
trap 'rm -f "$CONF_FILE"' EXIT

cat >"$CONF_FILE" <<EOF
[include]
$TEST_NAME
[exclude]
*
EOF

echo "Building blk_unitest in release mode..."
make -C "$UNITEST_DIR" -j "$JOBS" write_compilation_cmd all USE_RELEASE=1 BUILD_RELEASE=1 USE_SANITIZERS=0

echo "Recording perf data for $TEST_NAME..."
echo "IO PET config: nvmeibc_io_pet_buffer_size=$IO_PET_BUFFER_SIZE nvmeibc_io_pet_verbose=$IO_PET_VERBOSE"

cd "$UNITEST_DIR"
"$PERF_BIN" record -F 999 -g --call-graph dwarf -o "$PERF_DATA" -- ./blk_unitest \
	-tracedbg 0 -nRep 1 \
	-conf "$CONF_FILE" \
	-nvmeibc_io_pet_buffer_size "$IO_PET_BUFFER_SIZE" \
	-nvmeibc_io_pet_verbose "$IO_PET_VERBOSE" \
	-noTestEras -noTestNrep -noTestRestart

echo "Top PET symbols from perf report:"
"$PERF_BIN" report -i "$PERF_DATA" --stdio --sort=symbol --percentage=absolute -g none 2>&1 | rg -o "^\s+[0-9].*pet[^\s]*" | head -40
