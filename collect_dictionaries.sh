#/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

set -e #exit on first error
echo "Collection dictionaries for binary tracing..."

TMPDIR=$(mktemp -d)
find . \
	-name 'dict.*.json' \
	-o -name 'libfmtrs.so' \
	| xargs -I{} cp -f {} ${TMPDIR}
trap "rm -rf ${TMPDIR}" EXIT
# EPOCH used by compilator to have determinstic timestamps in tar.  It's in seconds for GCC, so, if present, has to be converted
tar "${SOURCE_DATE_EPOCH:+--mtime=$(date -d "@$SOURCE_DATE_EPOCH" '+%F %T')}" -cvzf dictionaries.tar.gz -C ${TMPDIR} $(cd ${TMPDIR} && ls)

echo "Collection dictionaries done"
