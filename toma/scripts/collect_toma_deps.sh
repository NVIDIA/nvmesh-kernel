#!/usr/bin/env bash
# DanielHSH, 2025/Nov, Used if we need to copy Toma to other machine (kubernetes container) with all its .so, without installing NVMesh properly. Gil Vitsinger was the last one who used
TOMA_BIN_PATH=${TOMA_BIN_PATH:-"toma/bin/release/nvmeibt_toma"}
OUTPUT_LIBS=${OUTPUT_LIBS:-toma_libs}

set -e
ldd_output=$(ldd $TOMA_BIN_PATH)
deps_paths=$(ldd $TOMA_BIN_PATH | awk '{print $3}' | sed 's/\.*//')

echo "Output for ldd running on toma is $ldd_output\n"

mkdir -p $OUTPUT_LIBS
echo "Copy lib files into $OUTPUT_LIBS"

for f in $deps_paths
do
    if [[ $f =~ libc.so || $f =~ libpthread.so ]]; then
        echo "Skipping $f - glibc specifc"
        continue
    elif [[ $f =~ libudev.so ]]; then
        echo "Skipping $f - for now we trust OS to already have it"
        continue
    fi

    [ -f $f ] && echo "Copying $f" && cp $f $OUTPUT_LIBS || echo "Skipping unknown"
    if [[ $f =~ libibverbs ]]; then
        echo "Copying all verbs"
        cp -v $(dirname $f)/libibverbs/* $OUTPUT_LIBS/
    fi
done
