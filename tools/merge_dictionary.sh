#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

function print_usage {
cat << __USAGE__
Usage: $0 nvmeib?.traces.json dictionary.json [--simulator]
    Calculate checksum (currently crc32) on a simple concatination of
    dictionary.json and nvmeib?.traces.json.
    Output the result into the original nvmeib?.traces.json, which is now
    a json dictionary with the following fields:
    * dictionary - Content of dictionary.json
    * traces     - Content of the original nvmeib?.traces.json
    * cksum      - Checksum
__USAGE__
exit 1
}

function calc_cksum {
    cat "$1" "$2" | grep -v '"line"' | cksum | cut -d' ' -f1
}

function die {
    >&2 echo "$*"
    exit 1
}

if [ "$#" -ne 2 ] && [ "$#" -ne 4 ] ; then
    print_usage
fi

if [ ! -f "$1" ] ; then
    die "'$1' not found"
fi

if [ ! -f "$2" ] ; then
    die "'$2' not found"
fi

if [ "$3" == "--cksum" ]; then
    CKSUM=$4 # In simulator we do not use checksum to optimize number of rebuilds
else
    CKSUM=$(calc_cksum "$1" "$2")
    if [ "$CKSUM" == "0" ]; then
        # 0 is a reserved value
        CKSUM="1"
    fi
fi

CKSUMFILE="$1.cksum"

# Temporary working file
TMPFILE=$(mktemp)
trap "rm -f $TMPFILE" EXIT
chmod 644 $TMPFILE

cat > "$TMPFILE" << __MERGED_TRACES_FILE__
{

"cksum": $CKSUM,

"commit_id": "$COMMIT_ID",

"traces":
$(cat $1)

,

"dictionary":
$(cat $2)

}
__MERGED_TRACES_FILE__

echo $CKSUM > $CKSUMFILE

# Only bump $1's mtime if content actually changed. Preserves mtime so
# downstream rebuilds (object files that transitively depend on this
# dictionary) can be skipped on no-op re-builds.
if cmp -s "$TMPFILE" "$1"; then
    rm -f "$TMPFILE"
else
    mv -f "$TMPFILE" "$1"
    touch "$1" -r "$CKSUMFILE"
fi

# Same guard for the cksum-named copy. If the cksum is unchanged the
# filename is identical and the content is identical; skip the cp.
DICTFILE=$(dirname "$1")/dict.$CKSUM.json
if ! cmp -s "$1" "$DICTFILE" 2>/dev/null; then
    cp -f "$1" "$DICTFILE"
fi

exit 0
