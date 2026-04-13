#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0

# compute_backports.sh - Detect kernel API backports and output -D flags
#
# Includes file-existence and grep-based checks moved from the top-level Makefile
# (kernel paths use KSRC1; RDMA UAPI uses INC_RDMA = inbox or OFED per backports.mk).
#
# Called from backports.mk via $(shell ...) during the COMPILE_MODULES step.
# Runs as a single process with file-content caching, replacing ~100+ separate
# Make $(shell ...) invocations.
#
# Usage: compute_backports.sh KSRC1 INC_RDMA INC_RDMA_DRV KERN_SYMVERS RDMA_SYMVERS ARCH [GREP_DEBUG GREP_DEBUG_LOGFILE]
# INC_RDMA: OFED tree or KSRC1 (UAPI). INC_RDMA_DRV: OFED tree or KERN_FILES_PATH (OFED_SRC_DIR or kernels/ copy per backports.mk).
# Output: Space-separated -DFLAG=value flags on stdout

set -f  # disable globbing

KSRC1="$1"
INC_RDMA="$2"
INC_RDMA_DRV="$3"
KERN_SYMVERS="$4"
RDMA_SYMVERS="$5"
ARCH="$6"
GREP_DEBUG="${7:-0}"
GREP_DEBUG_LOGFILE="$8"

if [[ "$GREP_DEBUG" == "1" && -n "$GREP_DEBUG_LOGFILE" ]]; then
    exec 19>"$GREP_DEBUG_LOGFILE"
    BASH_XTRACEFD=19
    set -x
fi

CFLAGS=""

# ---- File content cache ----
CACHE_DIR=$(mktemp -d)
trap 'rm -rf "$CACHE_DIR"' EXIT

# Strip C comments and cache per unique set of input files
strip_and_grep() {
    local outer_pattern="$1"
    shift
    # Cache key: hash of paths (joined path strings can exceed NAME_MAX as a filename)
    local cache_key
    cache_key=$(printf '%s\0' "$@" | sha256sum | awk '{print $1}')
    local cache_file="$CACHE_DIR/$cache_key"
    if [[ ! -f "$cache_file" ]]; then
        sed -e '/\/\*/,/\*\//{s:/\*.*\*/::g; t; :a; /\/\*/,/\*\//{s:/\*.*\*/::g; t; N; b a}; s:/\*.*\*/::g}' "$@" 2>/dev/null > "$cache_file"
    fi
    grep -Poz "$outer_pattern" < "$cache_file"
}

# ---- Core check function ----
# Args: define outer_pattern inner_pattern file_paths basepath [found_val] [notfound_val]
grep_check() {
    local define="$1"
    local outer_pattern="$2"
    local inner_pattern="$3"
    local file_paths="$4"
    local basepath="$5"
    local found_val="${6:-1}"
    local notfound_val="${7:-0}"

    local files=()
    for f in $file_paths; do
        files+=("$basepath/$f")
    done

    if strip_and_grep "$outer_pattern" "${files[@]}" | grep -Pq "$inner_pattern" 2>/dev/null; then
        CFLAGS="$CFLAGS -D${define}=${found_val}"
    else
        CFLAGS="$CFLAGS -D${define}=${notfound_val}"
    fi
}

# ---- Pattern-specific wrappers ----
# Argument order matches the original Make functions for easy comparison.

# grep_struct_member: define struct_name member file_paths basepath [found notfound]
grep_struct_member() {
    grep_check "$1" "(?s)${2}\s+\{.*?(?=\n\};)\n\};\n" "$3" "$4" "$5" "$6" "$7"
}

# grep_func_var: define func_name var_name file_paths basepath [found notfound]
grep_func_var() {
    grep_check "$1" "(?s)${2}\s*\(.*?\);\n" "$3" "$4" "$5" "$6" "$7"
}

# grep_func_rv: define func_name return_type file_paths basepath [found notfound]
grep_func_rv() {
    grep_check "$1" "(?s)[^\n]*${2}\s*\(.*?\);\n" "(?s)${3}\s*${2}" "$4" "$5" "$6" "$7"
}

# grep_macro_param: define macro_name param_name file_paths basepath [found notfound]
grep_macro_param() {
    grep_check "$1" "(?s)#define\s+${2}\s*\(.*?\)" "$3" "$4" "$5" "$6" "$7"
}

# grep_func_ptr_var: define ptr_name var_name file_paths basepath [found notfound]
grep_func_ptr_var() {
    grep_func_var "$1" "\\(\\*\\s*${2}\\)" "$3" "$4" "$5" "$6" "$7"
}

# grep_func_ptr_rv: define ptr_name return_type file_paths basepath [found notfound]
grep_func_ptr_rv() {
    grep_func_rv "$1" "\\(\\*\\s*${2}\\)" "$3" "$4" "$5" "$6" "$7"
}

# grep_typedef: define type_name file_paths basepath [found notfound]
grep_typedef() {
    grep_check "$1" "typedef\s+.*?\s+${2}" "" "$3" "$4" "$5" "$6"
}

# ---- Kernel source (KSRC1) convenience wrappers ----
grep_ksrc_struct_member() { grep_struct_member "$1" "$2" "$3" "$4" "$KSRC1" "$5" "$6"; }
grep_ksrc_func_var()      { grep_func_var      "$1" "$2" "$3" "$4" "$KSRC1" "$5" "$6"; }
grep_ksrc_func_ptr_var()  { grep_func_ptr_var  "$1" "$2" "$3" "$4" "$KSRC1" "$5" "$6"; }
grep_ksrc_macro_param()   { grep_macro_param   "$1" "$2" "$3" "$4" "$KSRC1" "$5" "$6"; }
grep_ksrc_typedef()       { grep_typedef       "$1" "$2" "$3" "$KSRC1" "$4" "$5"; }
grep_ksrc_func_ptr_rv()   { grep_func_ptr_rv   "$1" "$2" "$3" "$4" "$KSRC1" "$5" "$6"; }
grep_ksrc_func_rv()       { grep_func_rv       "$1" "$2" "$3" "$4" "$KSRC1"; }

# ---- RDMA (INC_RDMA) convenience wrappers ----
grep_rdma_struct_member() { grep_struct_member "$1" "$2" "$3" "$4" "$INC_RDMA" "$5" "$6"; }
grep_rdma_func_var()      { grep_func_var      "$1" "$2" "$3" "$4" "$INC_RDMA" "$5" "$6"; }
grep_rdma_func_rv()       { grep_func_rv       "$1" "$2" "$3" "$4" "$INC_RDMA"; }
grep_rdma_func_ptr_var()  { grep_func_ptr_var  "$1" "$2" "$3" "$4" "$INC_RDMA" "$5" "$6"; }
grep_rdma_func_ptr_rv()   { grep_func_ptr_rv   "$1" "$2" "$3" "$4" "$INC_RDMA"; }
grep_rdma_macro_param()   { grep_macro_param   "$1" "$2" "$3" "$4" "$INC_RDMA" "$5" "$6"; }
grep_rdma_typedef()       { grep_typedef       "$1" "$2" "$3" "$INC_RDMA" "$4" "$5"; }

# ---- RDMA drivers (INC_RDMA_DRV) convenience wrappers ----
grep_rdma_drv_struct_member() { grep_struct_member "$1" "$2" "$3" "$4" "$INC_RDMA_DRV" "$5" "$6"; }
grep_rdma_drv_func_var()      { grep_func_var      "$1" "$2" "$3" "$4" "$INC_RDMA_DRV" "$5" "$6"; }
grep_rdma_drv_func_rv()       { grep_func_rv       "$1" "$2" "$3" "$4" "$INC_RDMA_DRV"; }
grep_rdma_drv_func_ptr_var()  { grep_func_ptr_var  "$1" "$2" "$3" "$4" "$INC_RDMA_DRV" "$5" "$6"; }
grep_rdma_drv_func_ptr_rv()   { grep_func_ptr_rv   "$1" "$2" "$3" "$4" "$INC_RDMA_DRV"; }
grep_rdma_drv_macro_param()   { grep_macro_param   "$1" "$2" "$3" "$4" "$INC_RDMA_DRV" "$5" "$6"; }
grep_rdma_drv_typedef()       { grep_typedef       "$1" "$2" "$3" "$INC_RDMA_DRV" "$4" "$5"; }

# grep_symvers: Base function to check if a symbol is exported
grep_symvers() {
    local define="$1"
    local symbol="$2"
    local symvers_path="$3"
    local found_val="${4:-1}"
    local notfound_val="${5:-0}"
    if grep -qw "$symbol" "$symvers_path" 2>/dev/null; then
        CFLAGS="$CFLAGS -D${define}=${found_val}"
    else
        CFLAGS="$CFLAGS -D${define}=${notfound_val}"
    fi
}

# grep_kern_symvers: Check if a symbol is exported by the kernel Module.symvers
grep_kern_symvers() {
    grep_symvers "$1" "$2" "$KERN_SYMVERS" "$3" "$4"
}

# grep_rdma_symvers: Check if a symbol is exported by the kernel Module.symvers (INBOX) or the OFED Module.symvers
grep_rdma_symvers() {
    grep_symvers "$1" "$2" "$RDMA_SYMVERS" "$3" "$4"
}

# file_exists_define: probe relative path under one or more base directories.
# Usage: file_exists_define <cpp_define> <relative_path> <base_dirs> <val_if_found> <val_if_not_found> [any|all]
#   <base_dirs>: whitespace-separated absolute or relative directory paths (e.g. "$INC_RDMA $KSRC1")
#   [any|all]: optional match mode (default: any)
#     any: success if the file exists in at least one base directory.
#     all: success only if the file exists in every non-empty base directory.
#   Appends -D<cpp_define>=<val> to CFLAGS.
#   On success sets LAST_EXISTING_FILE_PATH to first matching path (any) or first checked path (all);
#   clears it on failure.
# Exit: 0 on success per match mode, 1 otherwise — suitable for: if file_exists_define ...; then ...; fi
file_exists_define() {
    local define="$1"
    local rel="${2#./}"
    local bases="$3"
    local found_val="${4:-1}"
    local notfound_val="${5:-0}"
    local match_mode="${6:-any}"
    local base path first_path=""
    local checked_any=0
    LAST_EXISTING_FILE_PATH=
    case "$match_mode" in
        any)
            for base in $bases; do
                [[ -n "$base" ]] || continue
                path="$base/$rel"
                if [[ -f "$path" ]]; then
                    LAST_EXISTING_FILE_PATH="$path"
                    CFLAGS="$CFLAGS -D${define}=${found_val}"
                    return 0
                fi
            done
            ;;
        all)
            for base in $bases; do
                [[ -n "$base" ]] || continue
                checked_any=1
                path="$base/$rel"
                [[ -n "$first_path" ]] || first_path="$path"
                if [[ ! -f "$path" ]]; then
                    CFLAGS="$CFLAGS -D${define}=${notfound_val}"
                    return 1
                fi
            done
            if [[ "$checked_any" -eq 1 ]]; then
                LAST_EXISTING_FILE_PATH="$first_path"
                CFLAGS="$CFLAGS -D${define}=${found_val}"
                return 0
            fi
            ;;
        *)
            echo "file_exists_define: invalid match mode '$match_mode' (use any or all)" >&2
            CFLAGS="$CFLAGS -D${define}=${notfound_val}"
            return 1
            ;;
    esac
    CFLAGS="$CFLAGS -D${define}=${notfound_val}"
    return 1
}

# hashtable: Makefile logic — 1 only if hashtable.h exists and the five-argument
# hash_for_each_possible variant is absent (uses two grep_check calls).
grep_ksrc_hashtable_from_makefile() {
    grep_check "KS_HASHTABLE_FILE" "(?s)." "" "include/linux/hashtable.h" "$KSRC1" "1" "0"
    grep_check "KS_HASHTABLE_5ARG" "(?s)#define\s+hash_for_each_possible[^\n]*name, obj, node, member, key" "" "include/linux/hashtable.h" "$KSRC1" "1" "0"
    local hf h5
    hf=$(echo "$CFLAGS" | sed -n 's/.*-DKS_HASHTABLE_FILE=\([01]\).*/\1/p')
    h5=$(echo "$CFLAGS" | sed -n 's/.*-DKS_HASHTABLE_5ARG=\([01]\).*/\1/p')
    CFLAGS=$(echo "$CFLAGS" | sed 's/ -DKS_HASHTABLE_FILE=[01]//g; s/ -DKS_HASHTABLE_5ARG=[01]//g')
    if [[ "$hf" == "1" && "$h5" == "0" ]]; then
        CFLAGS="$CFLAGS -DKS_HASHTABLE=1"
    else
        CFLAGS="$CFLAGS -DKS_HASHTABLE=0"
    fi
}


###############################################################################
# All backport checks
###############################################################################

# ---------------------------------------------------------------------------- #
# Moved from top-level Makefile (kernel tree = KSRC1; RDMA UAPI = INC_RDMA)
# Minimum supported kernel is 4.18 — many probes are constant on that baseline.
# ---------------------------------------------------------------------------- #

# --- Fixed for >= 4.18 mainline (see kr_version.h K_CHECK_VER fallbacks) ---
CFLAGS="$CFLAGS -DKSRC_INCLUDE_SCHED_MM=1 -DLINUX_SCHED_MM=1"
CFLAGS="$CFLAGS -DKS_HAS_I387_HEADER=0"
CFLAGS="$CFLAGS -DKS_REINIT_COMPLETION=1"
CFLAGS="$CFLAGS -DKS_HAS_SCHED_SIGNAL_HEADER=1 -DKS_HAS_SCHED_TASK_HEADER=1"
CFLAGS="$CFLAGS -DKS_HAS_BITMAP_SCNPRINTF=1"
CFLAGS="$CFLAGS -DKS_NEW_TIMER_API=1"
CFLAGS="$CFLAGS -DKS_HAS_IRQ_POLL=1"
CFLAGS="$CFLAGS -DKS_HAS_VM_FAULT_T=1"
CFLAGS="$CFLAGS -DKS_HAS_SO_INCOMING_CPU=1"
CFLAGS="$CFLAGS -DKS_DRIVERFS_DEV=0"
# 4.14+ genhd prototypes use request_queue; min 4.18. Paths that use these only run when
# KS_HAS_PART_INC_DEC_IN_FLIGHT (kernel < 5.8), which implies genhd.h — so 1 is always right
# when it matters; when genhd.h is gone (5.15+) that block is compiled out — value unused.
CFLAGS="$CFLAGS -DKS_PART_INC_IN_FLIGHT_USES_Q=1 -DKS_PART_DEC_IN_FLIGHT_USES_Q=1"

grep_ksrc_hashtable_from_makefile

grep_check "KS_HAVE_REGISTER_NETDEVICE_NOTIFIER_RH" "register_netdevice_notifier_rh" "" "include/linux/netdevice.h" "$KSRC1"
grep_check "KS_FAULT_EXPECTS_VM_AREA" "\*fault.*struct vm_area_struct" "" "include/linux/mm.h" "$KSRC1"

grep_check "KS_NO_BIO_IS_RW" "bio_is_rw" "" "include/linux/bio.h" "$KSRC1" "0" "1"

grep_check "KS_HAS_SA_PATH_REC" "struct sa_path_rec" "" "include/rdma/ib_sa.h" "$INC_RDMA"

grep_check "KS_HAS_GENHD_H" "(?s)." "" "include/linux/genhd.h" "$KSRC1" "1" "0"

grep_check "KS_HAS_SCSCI_REQUEST_H" "(?s)." "" "include/scsi/scsi_request.h" "$KSRC1" "1" "0"

grep_check "KS_HAS_UUID_BE_GEN" "uuid_be_gen" "" "include/linux/uuid.h" "$KSRC1"

grep_check "KS_HAS_MUTEX_OWNER" "__mutex_owner" "" "include/linux/mutex.h" "$KSRC1"
grep_ksrc_func_var "KS_HAS_ATOMIC_INC_NOT_ZERO_HINT" "int atomic_inc_not_zero_hint" "" "include/linux/atomic.h"

grep_check "KS_HAS_GPL_SME_ACTIVE" "sme_active.*EXPORT_SYMBOL_GPL" "" "Module.symvers" "$KSRC1"

grep_rdma_func_var "KS_IB_SA_PATH_REC_GET_HAS_RETRIES" "int ib_sa_path_rec_get" "retries" "include/rdma/ib_sa.h"

grep_check "KS_HAS_MMIOWB" "#define\s+mmiowb\s*\(\)" "" "arch/x86/include/asm/io.h" "$KSRC1"
grep_check "KS_HAS_KERNEL_SOCKPTR" "(?s)." "" "include/linux/sockptr.h" "$KSRC1" "1" "0"

grep_ksrc_func_var "KS_HAS_DO_GETTIMEOFDAY" "void do_gettimeofday" "" "include/linux/time.h include/linux/timekeeping.h include/linux/timekeeping32.h"
grep_ksrc_func_var "KS_HAS_GETNSTIMEOFDAY" "void getnstimeofday" "" "include/linux/time.h include/linux/timekeeping.h include/linux/timekeeping32.h"
file_exists_define "HAVE_TIMECOUNTER_H" "include/linux/timecounter.h" "$KSRC1" "1" "0"
file_exists_define "HAVE_CGROUP_RDMA_H" "include/linux/cgroup_rdma.h" "$INC_RDMA $KSRC1" "1" "0" "all"

# ---------------------------------------------------------------------------- #
# Moved from top-level Makefile (kmod / tcp / mmap_lock / genhd / blkdev probes)
# ---------------------------------------------------------------------------- #
grep_check "KS_HAS_CALL_USERMODEHELPER_SETFNS" "call_usermodehelper_setfns" "" "include/linux/kmod.h" "$KSRC1"
grep_check "KS_HAS___TCP_SEND_ACK" "__tcp_send_ack" "" "include/net/tcp.h" "$KSRC1"
grep_check "KS_HAS_TCP_RENO_UNDO_CWND" "tcp_reno_undo_cwnd" "" "include/net/tcp.h" "$KSRC1"
grep_check "KS_HAS_MMAP_LOCK_FUNCTIONS" "mmap_read_lock" "" "include/linux/mmap_lock.h" "$KSRC1"
grep_check "KS_HAS_MMAP_WRITE_TRYLOCK" "mmap_write_trylock" "" "include/linux/mmap_lock.h" "$KSRC1"
grep_check "KS_HAS_REVALIDATE_DISK_SIZE" "revalidate_disk_size" "" "include/linux/genhd.h" "$KSRC1"
grep_check "KS_HAS_BIO_START_IO_ACCT" "bio_start_io_acct" "" "include/linux/blkdev.h" "$KSRC1"

# kref_read: always probe inbox kernel tree (KSRC1), not INC_RDMA / OFED linux headers
grep_check "KS_HAS_KREF_READ" "kref_read" "" "include/linux/kref.h" "$KSRC1"

# ---------------------------------------------------------------------------- #
# Moved from top-level Makefile: __ib_alloc_pd, ib_verbs.h, mlx5_ib.h
# UAPI: $INC_RDMA (OFED_SRC_DIR or KSRC1 per backports.mk). Driver tree: $INC_RDMA_DRV
# (OFED or KERN_FILES_PATH for NO_OFED).
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_HAS_IB_ALLOC_MACRO" "__ib_alloc_pd" "" "include/rdma/ib_verbs.h"
grep_rdma_func_var "KS_IB_ALLOC_HAS_SKIP_TRACKING" "__ib_alloc_pd" "skip_tracking" "include/rdma/ib_verbs.h"

grep_check "HAS_IB_GET_DMA_MR" "ib_get_dma_mr" "" "include/rdma/ib_verbs.h" "$INC_RDMA"
grep_check "KS_HAS_VIRT_DMA_SUPPORT" "ib_uses_virt_dma" "" "include/rdma/ib_verbs.h" "$INC_RDMA"
grep_check "HAS_IB_QUERY_GID" "ib_query_gid" "" "include/rdma/ib_verbs.h" "$INC_RDMA"
grep_check "KS_IB_DEVICE_HAS_GET_NETDEV" "get_netdev" "" "include/rdma/ib_verbs.h" "$INC_RDMA"
grep_check "KS_IB_HAS_RDMA_AH_ATTR_TYPE" "rdma_ah_attr_type" "" "include/rdma/ib_verbs.h" "$INC_RDMA"

# Check if mlx5_ib.h exists in the RDMA tree
if file_exists_define KS_MLX5 "drivers/infiniband/hw/mlx5/mlx5_ib.h" "$INC_RDMA_DRV" 1 0; then
    grep_rdma_drv_struct_member "MLX5_IB_QP_FRAG_BUF" "struct mlx5_ib_qp" "struct mlx5_frag_buf" "drivers/infiniband/hw/mlx5/mlx5_ib.h"
    # MLX5_IB_WQ_FRAG_BUF_CTRL is independent of MLX5_IB_QP_FRAG_BUF in the code we compile (guarded only by WQ_FRAG_BUF_CTRL).
    grep_rdma_drv_struct_member "MLX5_IB_WQ_FRAG_BUF_CTRL" "struct mlx5_ib_wq" "struct mlx5_frag_buf_ctrl" "drivers/infiniband/hw/mlx5/mlx5_ib.h"
    grep_rdma_drv_struct_member "MLX5_IB_CQ_FRAG_BUF_CTRL" "struct mlx5_ib_cq_buf" "struct mlx5_frag_buf_ctrl" "drivers/infiniband/hw/mlx5/mlx5_ib.h"
    grep_rdma_drv_struct_member "IB_MLX5_NEW_BF" "struct mlx5_ib_qp" "struct mlx5_bf" "drivers/infiniband/hw/mlx5/mlx5_ib.h"
    grep_rdma_drv_struct_member "MLX5_IB_FBC_HAS_FRAG_BUF" "mlx5_frag_buf_ctrl" "frag_buf\;" "include/linux/mlx5/driver.h"
fi

# cma_priv.h under driver tree (OFED or KERN_FILES_PATH per INC_RDMA_DRV in backports.mk)
file_exists_define IB_HAS_CMA_PRIV_H "drivers/infiniband/core/cma_priv.h" "$INC_RDMA_DRV" 1 0

# ---------------------------------------------------------------------------- #
# Kernel 5.10
# ---------------------------------------------------------------------------- #
grep_rdma_func_ptr_rv "KS_IB_DESTROY_CQ_INT_RETURN" "destroy_cq" "int" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_rv "KS_IB_DESTROY_SRQ_INT_RETURN" "destroy_srq" "int" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_rv "KS_IB_DEALLOC_PD_INT_RETURN" "dealloc_pd" "int" "include/rdma/ib_verbs.h"
grep_rdma_func_rv "KS_IB_DESTROY_SRQ_RETURN_VOID" "ib_destroy_srq" "void" "include/rdma/ib_verbs.h"
grep_ksrc_struct_member "KS_REQUEST_QUEUE_HAS_REQUEST_FN" "struct request_queue" "make_request_fn" "include/linux/blkdev.h"
grep_rdma_func_var "KS_IB_REGISTER_DEVICE_HAS_DMA_DEVICE" "int ib_register_device" "struct device \*" "include/rdma/ib_verbs.h"

grep_ksrc_struct_member "KS_MLX5_ACCESS_MODE_1_0" "mlx5_ifc_mkc_bits" "access_mode_1_0 " "include/linux/mlx5/mlx5_ifc.h"

grep_rdma_func_var "KS_IB_HAS_RDMA_GET_GID_ATTR" "struct ib_gid_attr \*rdma_get_gid_attr" "int index" "include/rdma/ib_cache.h"
grep_rdma_struct_member "KS_IB_RDMA_PORT_SPACE" "enum rdma_port_space" "" "include/rdma/rdma_cm.h"
grep_ksrc_struct_member "KS_TCP_SOCK_HAS_XMIT_SIZE_GOAL_SEGS" "struct tcp_sock" "xmit_size_goal_segs" "include/linux/tcp.h"
grep_ksrc_struct_member "KS_DEV_ARCHDATA_HAS_DMA_OPS" "struct dev_archdata" "dma_ops" "arch/${ARCH}/include/asm/device.h"
grep_rdma_func_var "KS_IB_REGISTER_DEVICE_HAS_NAME" "int ib_register_device" "const char \*name" "include/rdma/ib_verbs.h"
grep_ksrc_func_var "KS_GET_USER_PAGES_HAS_TASK_STRUCT" "long get_user_pages" "struct task_struct \*tsk" "include/linux/mm.h"
grep_ksrc_func_var "KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT" "long get_user_pages_remote" "struct task_struct \*tsk" "include/linux/mm.h"
grep_ksrc_func_var "KS_GET_USER_PAGES_HAS_VMAS" "long get_user_pages" "struct vm_area_struct \*\*vmas" "include/linux/mm.h"
grep_ksrc_func_var "KS_GET_USER_PAGES_REMOTE_HAS_VMAS" "long get_user_pages_remote" "struct vm_area_struct \*\*vmas" "include/linux/mm.h"
grep_rdma_func_ptr_var "KS_IB_REG_USER_MR_HAS_ATTR" "reg_user_mr" "struct ib_mr_init_attr" "include/rdma/ib_verbs.h"
grep_rdma_func_var "KS_IB_REGISTER_DEVICE_HAS_KOBJECT" "int ib_register_device" "struct kobject" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_QUERY_DEVICE_HAS_UDATA" "query_device" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_CREATE_CQ_HAS_IB_CQ_INIT_ATTR" "create_cq" "const struct ib_cq_init_attr" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_CREATE_AH_HAS_UDATA" "create_ah" "struct ib_udata" "include/rdma/ib_verbs.h"

grep_rdma_func_ptr_var "KS_PROCESS_MAD_HAS_OUT_MAD_PKEY_INDEX" "process_mad" "u16 \*out_mad_pkey_index" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_PROCESS_MAD_HAS_IB_MAD_HDR" "process_mad" "struct ib_mad_hdr" "include/rdma/ib_verbs.h"

grep_rdma_func_ptr_var "KS_POST_SRQ_RECV_HAS_CONST" "post_srq_recv" "const struct ib_recv_wr" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_POST_SEND_HAS_CONST" "post_send" "const struct ib_send_wr" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_ATTR_HAS_MAX_SEND_SGE" "struct ib_device_attr" "max_send_sge" "include/rdma/ib_verbs.h"

grep_rdma_struct_member "KS_IB_DEVICE_HAS_OPS" "struct ib_device" "ib_device_ops" "include/rdma/ib_verbs.h"

# KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE depends on KS_IB_DEVICE_HAS_OPS
if echo "$CFLAGS" | grep -q "KS_IB_DEVICE_HAS_OPS=1"; then
    grep_rdma_struct_member "KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE" "struct ib_device_ops" "get_port_immutable" "include/rdma/ib_verbs.h"
else
    grep_rdma_struct_member "KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE" "struct ib_device" "get_port_immutable" "include/rdma/ib_verbs.h"
fi

grep_ksrc_struct_member "KS_DMA_MAP_OPS_HAS_DMA_ATTR" "struct dma_map_ops" "struct dma_attrs" "include/linux/dma-mapping.h"
grep_ksrc_func_var "KS_HAS_SMP_STORE_MB" "define smp_store_mb" "value" "include/asm-generic/barrier.h"

# ---------------------------------------------------------------------------- #
# RH8
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_SOCK_OPT_GETNAME_HAS_UADDR_LEN" "inet_getname" "uaddr_len" "include/net/inet_common.h"
grep_ksrc_struct_member "KS_THREAD_INFO_HAS_CPU" "struct thread_info" "cpu" "arch/${ARCH}/include/asm/thread_info.h"
grep_rdma_struct_member "KS_IB_DEVICE_HAS_DEVICE_OPS" "struct ib_device_ops" "ops" "include/rdma/ib_verbs.h"
grep_ksrc_struct_member "KS_DEVICE_HAS_DEVICE_RH" "struct device" "device_rh" "include/linux/device.h"
grep_rdma_struct_member "KS_IW_CM_HAS_IFNAME" "struct iw_cm_verbs" "ifname" "include/rdma/iw_cm.h"
grep_rdma_func_ptr_var "KS_IB_CREATE_AH_HAS_FLAGS" "create_ah" "u32" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-[147, 193] (CentOS 8.1.1911, 8.2.2004)
# ---------------------------------------------------------------------------- #
grep_rdma_func_ptr_var "IB_DEREG_MR_HAS_UDATA" "dereg_mr" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_OPS_HAS_MODULE_OWNER" "struct ib_device_ops" "owner" "include/rdma/ib_verbs.h"
grep_rdma_func_var "KS_IB_MLX5_WRITE64_HAS_DB_LOCK" "static inline void mlx5_write64" "spinlock_t \*doorbell_lock" "include/linux/mlx5/doorbell.h"

grep_check "KS_HAS___LLIST_ADD_BATCH" "__llist_add_batch" "" "include/linux/llist.h" "$KSRC1"
grep_check "KS_HAS___LLIST_ADD" "__llist_add" "" "include/linux/llist.h" "$KSRC1"
grep_check "KS_HAS___LLIST_DEL_ALL" "__llist_del_all" "" "include/linux/llist.h" "$KSRC1"

# ---------------------------------------------------------------------------- #
# OFED 5.1
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_RDMA_REJECT_HAS_REASON" "int rdma_reject" "u8 reason" "include/rdma/rdma_cm.h"

# ---------------------------------------------------------------------------- #
# RH8.3
# ---------------------------------------------------------------------------- #
grep_rdma_drv_struct_member "IB_MLX5_MR_HAS_LIVE" "mlx5_ib_mr" "live" "drivers/infiniband/hw/mlx5/mlx5_ib.h"

# ---------------------------------------------------------------------------- #
# RH8.4
# ---------------------------------------------------------------------------- #
grep_rdma_drv_struct_member "IB_MLX5_MR_HAS_DEV" "mlx5_ib_mr" "dev" "drivers/infiniband/hw/mlx5/mlx5_ib.h"
grep_rdma_drv_struct_member "IB_MLX5_MR_HAS_NPAGES" "mlx5_ib_mr" "npages" "drivers/infiniband/hw/mlx5/mlx5_ib.h"
grep_rdma_drv_struct_member "IB_MLX5_QP_HAS_WQ_SIG" "mlx5_ib_qp" "wq_sig" "drivers/infiniband/hw/mlx5/mlx5_ib.h"

# ---------------------------------------------------------------------------- #
# Kernel 5.4.0-[1031, 1035] (Ubuntu 18.04 @ Azure)
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_IB_REGISTER_DEVICE_HAS_IB_DEVICE" "int ib_register_device" "struct ib_device" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_HAS_OWNER" "struct ib_device" "owner" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_HAS_IWCM" "struct ib_device" "iwcm" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_DEVICE_ALLOC_USES_UCONTEXT" "alloc_ucontext" "struct ib_ucontext" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_DESTROY_QP_HAS_UDATA" "destroy_qp" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_DEVICE_PD_USES_UCONTEXT" "alloc_pd" "struct ib_ucontext" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_CREATE_CQ_HAS_IB_DEVICE" "create_cq" "struct ib_device" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_CREATE_CQ_HAS_IB_UCONTEXT" "create_cq" "struct ib_ucontext" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_DESTROY_CQ_HAS_IB_UDATA" "destroy_cq" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_ALLOC_MR_HAS_IB_UDATA" "alloc_mr" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_DEREG_MR_HAS_IB_UDATA" "dereg_mr" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_CREATE_SQR_HAS_IB_PD" "create_srq" "struct ib_pd" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_DESTROY_SQR_HAS_IB_UDATA" "destroy_srq" "struct ib_udata" "include/rdma/ib_verbs.h"
grep_rdma_func_var "KS_IB_SET_NETDEV_HAS_IB_DEVICE" "int ib_device_set_netdev" "struct ib_device" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_OPS_HAS_SIZES" "struct ib_device_ops" "DECLARE_RDMA_OBJ_SIZE" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# OFED 5.2 (Kernel 5.10)
# ---------------------------------------------------------------------------- #
grep_rdma_func_ptr_rv "KS_IB_CLIENT_ADD_RV_IS_INT" "add" "int" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_CLIENT_REMOVE_HAS_CLIENT_DATA" "remove" "client_data" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_VERBS_SUPPORTS_FMR" "struct ib_device_ops" "alloc_fmr" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Remove the need for the RHEL_BKPORT_MLX5 switch
# ---------------------------------------------------------------------------- #
if [[ -f "$INC_RDMA/include/linux/mlx4/qp.h" ]]; then
    grep_rdma_struct_member "KS_MLX_FENCE_VLAN" "struct mlx4_wqe_ctrl_seg" "qpn_vlan" "include/linux/mlx4/qp.h"
fi

# Set to 0 if struct mlx5_create_mkey_mbox_in exists
grep_rdma_struct_member "KS_MLX5_IFC" "struct mlx5_create_mkey_mbox_in" "" "include/linux/mlx5/device.h" "0" "1"

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-193
# ---------------------------------------------------------------------------- #
grep_rdma_func_ptr_var "KS_IB_CREATE_AH_HAS_PD" "create_ah" "struct ib_pd" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_CREATE_AH_HAS_AH" "create_ah" "struct ib_ah" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_rv "KS_IB_DESTROY_AH_RETURNS_INT" "destroy_ah" "int" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_rv "KS_IB_DESTROY_AH_RETURNS_VOID" "destroy_ah" "void" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-305
# ---------------------------------------------------------------------------- #
grep_rdma_func_ptr_var "KS_IB_CREATE_AH_HAS_AH_INIT_ATTR" "create_ah" "struct rdma_ah_init_attr" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_var "KS_IB_DESTROY_AH_HAS_FLAGS" "destroy_ah" "u32" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_HAS_FMR" "struct ib_fmr" "" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Kernel 4.18.0-305.10.2.el8_4 (for RH8.4 and vanilla 5.8)
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_TCP_SOCK_SET_NODELAY" "tcp_sock_set_nodelay" "" "include/linux/tcp.h"
grep_ksrc_func_var "KS_HAS_TCP_SOCK_SET_QUICKACK" "tcp_sock_set_quickack" "" "include/linux/tcp.h"
grep_ksrc_func_var "KS_HAS_TCP_SETSOCKOPT" "tcp_setsockopt" "" "include/net/tcp.h"
grep_ksrc_func_var "KS_TCP_SETSOCKOPT_TAKES_SOCKPTR_T" "tcp_setsockopt" "sockptr_t" "include/net/tcp.h"
grep_ksrc_func_var "KS_HAS_TCP_GETSOCKOPT" "tcp_getsockopt" "" "include/net/tcp.h"
grep_ksrc_func_var "KS_HAS_SOCK_SETSOCKOPT" "sock_setsockopt" "" "include/net/sock.h"
grep_ksrc_func_var "KS_SOCK_SETSOCKOPT_TAKES_SOCKPTR_T" "sock_setsockopt" "sockptr_t" "include/net/sock.h"

# ---------------------------------------------------------------------------- #
# Used for SIW panic_remote_on_rx_err 4.14 and above
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_KERNEL_SENDMSG_LOCKED" "kernel_sendmsg_locked" "" "include/linux/net.h"

# ---------------------------------------------------------------------------- #
# Used for SIW IB_EVENT_GID_CHANGED 5.4 and above
# ---------------------------------------------------------------------------- #
grep_rdma_struct_member "KS_IB_DEVICE_HAS_EVENT_HADNLER_RWSEM" "struct ib_device" "event_handler_rwsem" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Used for nonexists ib_dma_alloc_coherent 5.11 and above
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_HAS_IB_DMA_ALLOC_COHERENT" "ib_dma_alloc_coherent" "" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# SIW - INET_MATCH sdif
# ---------------------------------------------------------------------------- #
grep_ksrc_macro_param "KS_INET_MATCH_HAS_SDIF" "INET_MATCH" "__sdif" "include/net/inet_hashtables.h"

# ---------------------------------------------------------------------------- #
# SIW - Moved from Makefile
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_SOCK_SET_REUSEADDR" "sock_set_reuseaddr" "" "include/net/sock.h"

# ---------------------------------------------------------------------------- #
# Kernel 4.15
# ---------------------------------------------------------------------------- #
grep_rdma_struct_member "KS_IB_DEVICE_OPS_HAS_DRIVER_ID" "struct ib_device_ops" "driver_id" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_HAS_DRIVER_ID" "struct ib_device" "driver_id" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# 4.18.0-240.el8.x86_64
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_IDR_INIT" "idr_init" "" "include/linux/idr.h"

# ---------------------------------------------------------------------------- #
# 4.18.0-513.24.1.el8_9.x86_64
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_IB_SA_PATH_REC_GET_CB_HAS_NUM_PRS" "int ib_sa_path_rec_get" "num_prs" "include/rdma/ib_sa.h"
grep_rdma_func_var "KS_IB_SA_PATH_REC_GET_CB_HAS_NUM_PRS_UINT" "int ib_sa_path_rec_get" "unsigned int num_prs" "include/rdma/ib_sa.h"

# ---------------------------------------------------------------------------- #
# 5.12 Kernel
# ---------------------------------------------------------------------------- #
grep_ksrc_struct_member "KS_HAS_BLOCK_DEVICE_STRUCT" "block_device" "" "include/linux/blk_types.h"
grep_check "KS_HAS_MODULE_MUTEX" "struct\s+mutex\s+module_mutex" "" "include/linux/module.h" "$KSRC1" "1" "0"
grep_ksrc_func_var "KS_HAS_REVALIDATE_DISK_FN" "revalidate_disk" "" "include/linux/fs.h"
grep_ksrc_func_var "KS_HAS_SET_CAPACITY_AND_MODIFY_FN_GENDISK" "set_capacity_and_notify" "" "include/linux/genhd.h"
grep_ksrc_func_var "KS_HAS_DISK_PART_ITER" "disk_part_iter_init" "" "include/linux/genhd.h"
grep_ksrc_struct_member "KS_BIO_HAS_BI_BDEV_PTR" "struct bio" "struct block_device.*\*" "include/linux/blk_types.h"
grep_ksrc_struct_member "KS_BIO_HAS_BI_GENDISK_PTR" "struct bio" "struct gendisk.*\*" "include/linux/blk_types.h"
grep_ksrc_struct_member "KS_BIO_HAS_BI_OPF" "struct bio" "bi_opf" "include/linux/blk_types.h"
grep_ksrc_struct_member "KS_BIO_HAS_BI_WRITE_HINT" "struct bio" "bi_write_hint" "include/linux/blk_types.h"

# ---------------------------------------------------------------------------- #
# 5.13 Kernel
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_LIST_SORT_CMP_FUNC_T" "list_sort" "list_cmp_func_t" "include/linux/list_sort.h"
grep_rdma_func_var "IB_VERBS_PORT_NUM_IS_U32" "ib_query_port" "u32\s+port_num" "include/rdma/ib_verbs.h"
grep_ksrc_struct_member "KS_BLOCK_DEV_OPS_HAS_REVALIDATE_DISK" "block_device_operations" "revalidate_disk" "include/linux/blkdev.h"

# ---------------------------------------------------------------------------- #
# 5.15 Kernel
# ---------------------------------------------------------------------------- #
grep_ksrc_struct_member "KS_BDI_PTR_IN_QUEUE" "request_queue" "struct backing_dev_info.*\*" "include/linux/blkdev.h"
grep_ksrc_struct_member "KS_BDI_IN_QUEUE" "request_queue" "struct backing_dev_info" "include/linux/blkdev.h"

grep_rdma_func_ptr_rv "KS_IB_CREATE_QP_INT_RV" "create_qp" "int" "include/rdma/ib_verbs.h"
grep_rdma_func_ptr_rv "KS_IB_CREATE_QP_INT_RV" "create_qp" "int" "include/rdma/ib_verbs.h"
grep_rdma_struct_member "KS_IB_DEVICE_OPS_HAS_QP_SIZE" "struct ib_device_ops" "DECLARE_RDMA_OBJ_SIZE\(ib_qp\);" "include/rdma/ib_verbs.h"

grep_ksrc_func_var "KS_HAS_REVALIDATE_DISK_FN" "revalidate_disk" "" "include/linux/fs.h"

# ---------------------------------------------------------------------------- #
# SIW - 4.15.0-136-generic
# ---------------------------------------------------------------------------- #
grep_rdma_struct_member "KS_IB_VERBS_HAS_DMA_MAPPING_OPS" "struct ib_dma_mapping_ops" "" "include/rdma/ib_verbs.h"
grep_ksrc_struct_member "KS_DMA_MAP_OPS_HAS_MAPPING_ERROR" "struct dma_map_ops" "mapping_error" "include/linux/dma-mapping.h"
grep_ksrc_struct_member "KS_DMA_MAP_OPS_HAS_SET_DMA_MASK" "struct dma_map_ops" "set_dma_mask" "include/linux/dma-mapping.h"
grep_ksrc_struct_member "KS_DMA_MAP_OPS_HAS_IS_PHYS" "struct dma_map_ops" "is_phys" "include/linux/dma-mapping.h"
grep_rdma_struct_member "KS_IB_DEVICE_HAS_DMA_OPS" "struct ib_device_ops" "ops" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# MLNX_OFED_LINUX-5.7-1.0.0.0 / 4.18.0-477.10.1.el8_8.x86_64
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_RDMA_MLX5_CORE_CREATE_MKEY_MKEY_U32" "int mlx5_core_create_mkey" "u32 \*mkey" "include/linux/mlx5/driver.h"
grep_rdma_drv_struct_member "KS_RDMA_MLX5_IB_MKEY_HAS_NDESCS" "struct mlx5_ib_mkey" "unsigned int ndescs" "drivers/infiniband/hw/mlx5/mlx5_ib.h"

# ---------------------------------------------------------------------------- #
# Lustre 2.15.2 / 4.18.0-425.3.1.el8_lustre.x86_64
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_RDMA_MLX5_MISSING_MLX5_BUF_OFFSET" "mlx5_buf_offset" "" "include/linux/mlx5/driver.h" "0" "1"

# ---------------------------------------------------------------------------- #
# Compat for smp_call_function_single_async
# ---------------------------------------------------------------------------- #
grep_ksrc_typedef "KS_SMP_CALL_SINGLE_DATA_T" "call_single_data_t" "include/linux/smp.h"

# ---------------------------------------------------------------------------- #
# SIW - For CQ notification as tasklet
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_TASKLET_SETUP" "tasklet_setup" "" "include/linux/interrupt.h"

# ---------------------------------------------------------------------------- #
# Missing scatterlist functionality in 3.10 kernels
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_SGL_ALLOC_ORDER" "sgl_alloc_order" "" "include/linux/scatterlist.h"
grep_ksrc_struct_member "KS_HAS_SG_DMA_PAGE_ITER" "sg_dma_page_iter" "" "include/linux/scatterlist.h"

# ---------------------------------------------------------------------------- #
# OFED 23.07
# ---------------------------------------------------------------------------- #
grep_rdma_func_var "KS_RDMA_IB_CM_LISTEN_HAS_SERVICE_MASK" "int ib_cm_listen" "__be64 service_mask" "include/rdma/ib_cm.h"

# INET_MATCH - now an inline function on newer kernels
grep_ksrc_func_var "KS_HAS_NEW_INET_MATCH_LOWER" "inet_match" "" "include/net/inet_hashtables.h"
grep_ksrc_func_var "KS_HAS_NEW_INET_MATCH_CAPS" "INET_MATCH" "" "include/net/inet_hashtables.h"

# ---------------------------------------------------------------------------- #
# Kernel 5.17
# ---------------------------------------------------------------------------- #
grep_ksrc_func_rv "KS_SUBMIT_BIO_VOID_RV" "submit_bio" "void" "include/linux/bio.h include/linux/fs.h"
grep_ksrc_func_rv "KS_ADD_DISK_INT_RV" "add_disk" "int\s*__must_check" "include/linux/blkdev.h"

# ---------------------------------------------------------------------------- #
# Kernel 5.18
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_BIO_INIT_HAS_BDEV_N_OPF" "bio_init" "opf" "include/linux/bio.h"
grep_ksrc_func_var "KS_HAS_SET_CAPACITY_AND_MODIFY_FN_BDEV" "set_capacity_and_notify" "" "include/linux/blkdev.h"

# ---------------------------------------------------------------------------- #
# Kernel 5.19
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_BLK_MQ_DESTROY_QUEUE" "blk_mq_destroy_queue" "" "include/linux/blk-mq.h"
grep_ksrc_func_var "KS_HAS_SET_FS" "set_fs" "" "include/asm-generic/uaccess.h"
grep_ksrc_func_var "KS_BIO_ALLOC_HAS_BLOCK_DEVICE" "bio_alloc" "struct block_device" "include/linux/bio.h"
grep_ksrc_func_var "KS_BLKDEV_ISSUE_DISCARD_HAS_FLAGS" "blkdev_issue_discard" "unsigned long flags" "include/linux/blkdev.h"

grep_ksrc_func_rv "KS_BLOCK_DEV_MAKE_REQUEST_VOID" "submit_bio" "void" "include/linux/bio.h include/linux/fs.h"
grep_ksrc_func_rv "KS_BLK_QC_T" "submit_bio" "blk_qc_t" "include/linux/bio.h include/linux/fs.h"

grep_ksrc_func_var "KS_HAS_PROFILE_EVENT_REGISTER" "profile_event_register" "" "include/linux/profile.h"
grep_ksrc_func_var "KS_PDE_DATA_IS_LOWER" "pde_data" "" "include/linux/proc_fs.h"
grep_ksrc_func_var "KS_HAS_BLKDEV_IOCTL" "blkdev_ioctl" "" "include/linux/fs.h"
grep_rdma_func_var "KS_IB_REGISTER_DEVICE_HAS_DEVICE" "int ib_register_device" "struct device" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Kernel 6.5
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_BLKDEV_GET_BY_PATH_HAS_HOLDERS" "blkdev_get_by_path" "blk_holder_ops" "include/linux/blkdev.h"

# ---------------------------------------------------------------------------- #
# Kernel 6.8
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_VM_FLAGS_SET" "vm_flags_set" "" "include/linux/mm.h"
grep_ksrc_func_var "KS_HAS_BIO_SET_OP_ATTRS" "bio_set_op_attrs" "" "include/linux/blktypes.h"
grep_ksrc_func_var "KS_HAS_BDEV_OPEN_BY_PATH" "bdev_open_by_path" "" "include/linux/blkdev.h"
grep_ksrc_func_var "KS_HAS_BLK_CLEANUP_DISK" "blk_cleanup_disk" "" "include/linux/genhd.h"
grep_ksrc_func_var "KS_HAS_TCP_SENDPAGE" "tcp_sendpage" "" "include/net/tcp.h"
grep_ksrc_func_var "KS_HAS_RDMA_FOR_EACH_PORT" "rdma_for_each_port" "" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# Kernel 6.9
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_BDEV_FILE_OPEN_BY_PATH" "bdev_file_open_by_path" "" "include/linux/blkdev.h"
grep_ksrc_func_var "KS_HAS_QUEUE_LIMITS_START_UPDATE" "queue_limits_start_update" "" "include/linux/blkdev.h"
grep_ksrc_macro_param "KS_HAS_BLK_ALLOC_DISK" "blk_alloc_disk" "" "include/linux/blkdev.h include/linux/genhd.h"
grep_ksrc_macro_param "KS_BLK_ALLOC_DISK_2PARAMS" "blk_alloc_disk" "lim" "include/linux/blkdev.h include/linux/genhd.h"

# ---------------------------------------------------------------------------- #
# DOCA OFED 24.10
# ---------------------------------------------------------------------------- #
grep_rdma_func_ptr_var "KS_IB_CREATE_CQ_HAS_ATTR_BUNDLE" "create_cq" "struct uverbs_attr_bundle" "include/rdma/ib_verbs.h"

# ---------------------------------------------------------------------------- #
# [NVMESH-5532]
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_SOCK_NOT_OWNED_BY_ME" "sock_not_owned_by_me" "" "include/net/sock.h"
grep_ksrc_func_var "KS_HAS_FIND_NTH_BIT" "find_nth_bit" "" "include/linux/find.h"

# ---------------------------------------------------------------------------- #
# Kernel 6.14
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_BDEV_PARTNO" "bdev_partno" "" "include/linux/blkdev.h"

# ---------------------------------------------------------------------------- #
# Used for keeper
# ---------------------------------------------------------------------------- #
grep_rdma_struct_member "KS_RDMA_HAS_RESTRACK" "struct rdma_restrack_entry" "" "include/rdma/restrack.h"
grep_ksrc_struct_member "KS_HAS_PROC_FS" "struct proc_ops" "" "include/linux/proc_fs.h"

# ---------------------------------------------------------------------------- #
# Kernel 6.17
# ---------------------------------------------------------------------------- #
grep_ksrc_func_var "KS_HAS_DEL_TIMER_SYNC" "del_timer_sync" "" "include/linux/timer.h"
grep_ksrc_func_var "KS_HAS___INIT_TIMER" "__init_timer" "" "include/linux/timer.h"
grep_ksrc_func_var "KS_CRC32C_USES_SIZE_T" "crc32c" "size_t" "include/linux/crc32.h"
grep_ksrc_struct_member "KS_HAS_SKB_CHECKSUM_OPS" "struct skb_checksum_ops" "" "include/linux/skbuff.h include/net/checksum.h"
grep_ksrc_func_var "KS_HAS___CRC32C_LE_COMBINE" "__crc32c_le_combine" "" "include/linux/crc32.h"
grep_ksrc_func_var "KS_HAS_HRTIMER_INIT" "hrtimer_init" "" "include/linux/hrtimer.h"
grep_rdma_func_ptr_var "KS_IB_REG_USER_MR_HAS_DMAH" "reg_user_mr" "ib_dmah" "include/rdma/ib_verbs.h"
grep_rdma_func_var "KS_HAS_MLX5_GET_UARS_PAGE" "mlx5_get_uars_page" "" "include/linux/mlx5/driver.h"
grep_ksrc_macro_param "KS_HAS_TIMER_CONTAINER_OF" "timer_container_of" "" "include/linux/timer.h"

# Verify tcp_setsockopt is actually exported (declared but unexported in 6.12+)
grep_kern_symvers "KS_TCP_SETSOCKOPT_EXPORTED" "tcp_setsockopt"


###############################################################################
# Output
###############################################################################
echo "$CFLAGS"
