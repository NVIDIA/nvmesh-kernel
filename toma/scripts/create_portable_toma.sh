#!/usr/bin/env bash
# DanielHSH, 2025/Nov, Used if we need to copy Toma to other machine (kubernetes container) with all its .so, without installing NVMesh properly. Gil Vitsinger was the last one who used

TOMA_BIN_PATH=${TOMA_BIN_PATH:-"toma/bin/release/nvmeibt_toma"}
OUTPUT_LIBS=${OUTPUT_LIBS:-toma_libs}

set -e
ldd_output=$(ldd $TOMA_BIN_PATH)
deps_paths=$(ldd $TOMA_BIN_PATH | awk '{print $3}' | sed 's/\.*//')
ibud_ldd_output=$(ldd $(dirname $TOMA_BIN_PATH)/nw_ibud.so)
ibud_deps_paths=$(ldd $(dirname $TOMA_BIN_PATH)/nw_ibud.so | awk '{print $3}' | sed 's/\.*//')


echo "Output for ldd running on toma is $ldd_output\n"
echo "Output for ldd running on ibud is $ibud_ldd_output\n"

mkdir -p $OUTPUT_LIBS
echo "Copy lib files into $OUTPUT_LIBS"

function collect_dep() {
    dep=$1

    [ -f $dep ] && echo "Copying $dep" && cp -f $dep $OUTPUT_LIBS || echo "Skipping unknown"
}

function collect_deps() {
    collect_libibverbs=$1
    shift
    deps=$*

    echo "collecting $deps"
    for d in $deps
    do
        collect_dep $d
        # libibverbs use dlopen on some driver providers..so we need to collect them as well
        if [[ $d =~ libibverbs && "$collect_libibverbs" == "true" ]]; then

                providers=$(ls $(dirname $d)/libibverbs/*)
                for p in $providers
                do
                        echo collecting provider $p
                        cp -f $p $OUTPUT_LIBS
                        deps_paths=$(ldd $p | awk '{print $3}' | sed 's/\.*//')
                        collect_deps false $deps_paths
                done
        fi
    done
}

function collect_dynamic_linker() {
        cp $1 $OUTPUT_LIBS
}

collect_deps false $deps_paths
collect_deps true $ibud_deps_paths

linker_path=$(readelf -l $TOMA_BIN_PATH | grep 'Requesting' | cut -d':' -f2 | tr -d ' ]';)
collect_dynamic_linker $linker_path
cp $TOMA_BIN_PATH $OUTPUT_LIBS/__$(basename $TOMA_BIN_PATH)

# wrap toma bin with script that runs it with the collected dynamic linker
cat > $OUTPUT_LIBS/$(basename $TOMA_BIN_PATH) << EOF
#!/usr/bin/env bash

RDMAV_DRIVERS="mlx4;mlx5" LD_LIBRARY_PATH=\$(dirname \$0) \$(dirname \$0)/$(basename $linker_path) \$(dirname \$0)/__$(basename $TOMA_BIN_PATH) \$*
EOF
chmod +x $OUTPUT_LIBS/$(basename $TOMA_BIN_PATH)