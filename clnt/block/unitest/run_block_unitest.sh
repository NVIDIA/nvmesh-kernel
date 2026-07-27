#!/bin/bash

pstderr() {
  (>&2 echo $@)
}

strindex() {
  x="${1%%$2*}"
  [[ "$x" = "$1" ]] && echo -1 || echo "${#x}"
}

print_help()
{
  echo -e "Wrapper for block unitest with binary tracing enabled"
  echo -e "Usage: $(basename $0) [options] [blk_unitest args]\n"
  echo -e "options:"
  echo -e "\t[-h|--help] Print this message and exit"
  echo -e "\t[-d] Debug level: {f, d, t, i, w, e, 0}, default = 0"
  echo -e "\t[-n] No postprocess"
}

parse_args()
{
  FIRST_ARG=1
  DEBUG_LEVEL="0"
  DEBUG_LEVELS="0ewitdf"
  NO_POSTPROCESS="$(grep -c _NVMEIB_TRACE_BACKEND_DMESG gen_events.h)"

  declare DEBUG_LEVEL_DICT

  i="1"
  while [ $i -le "$#" ]
  do
    if [ "${!i}" = "-h" ] || [ "${!i}" = "--help" ]
    then
      print_help
      exit 1
    fi

    if [ "${!i}" = "-d" ]
    then
      i=$[$i+1]
      DEBUG_LEVEL=$(strindex "$DEBUG_LEVELS" "${!i}")
      i=$[$i+1]
      continue
    fi

    if [ "${!i}" = "-n" ]
    then
      i=$[$i+1]
      NO_POSTPROCESS=1
      continue
    fi

    break
  done
  UNITEST_ARGS=${@:$i}
}

# MAIN
parse_args $@

PROJECT_ROOT="../../.."
POSTPROCESS_TOOL_DIR="$PROJECT_ROOT/tools/traces_post_processor"
POSTPROCESS_TOOL_TARGET="pager"
POSTPROCESS_TOOL="./pager"

if [ "$NO_POSTPROCESS" == "0" ]
then
  make -C $POSTPROCESS_TOOL_DIR $POSTPROCESS_TOOL_TARGET || { pstderr "Could not build post_lttng, aborting"; exit "$?"; }
fi

# If user aborts simulator, we will dump traces we got
trap ctrl_c INT
ctrl_c()
{
  pstderr "Interrupted, flushing traces"
}

pstderr "Running unitest"
./blk_unitest -tracedbg $DEBUG_LEVEL $UNITEST_ARGS
EXIT_CODE=$?

trap - INT

if [ "$NO_POSTPROCESS" == "0" ]
then
  pstderr "Runnig postprocessing"
  $POSTPROCESS_TOOL 2>/dev/null
fi

exit $EXIT_CODE
