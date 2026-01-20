#!/bin/sh
set -eu

# -------- helpers --------
log() { printf '%s\n' "$*"; }
log_no_newline() { printf '%s' "$*"; }

is_debian_like() {
  # /etc/os-release: ID_LIKE can be "debian ubuntu" etc.
  case " ${ID_LIKE-} " in
    *" debian "*) return 0 ;;
    *) return 1 ;;
  esac
}

initrd_path_for_kver() {
  # Reasonable defaults
  if is_debian_like; then
    printf '%s\n' "/boot/initrd.img-$1"
  else
    # RHEL/CentOS/Fedora family
    printf '%s\n' "/boot/initramfs-$1.img"
  fi
}

list_initrd() {
  # args: <kver> <outfile>
  kver=$1
  out=$2
  img=$(initrd_path_for_kver "$kver")

  # If the image doesn't exist, just produce an empty list
  if [ ! -r "$img" ]; then
    : >"$out"
    return 0
  fi

  if is_debian_like; then
    # Ubuntu/Debian
    if command -v lsinitramfs >/dev/null 2>&1; then
      lsinitramfs "$img" 2>/dev/null >"$out" || : >"$out"
    else
      : >"$out"
    fi
  else
    # RHEL-like
    if command -v lsinitrd >/dev/null 2>&1; then
      lsinitrd "$img" 2>/dev/null >"$out" || : >"$out"
    else
      : >"$out"
    fi
  fi
}

rebuild_initrd_if_needed() {
  # args: <kver> <initrd_list_file> <module_paths...>
  kver=$1
  initrd_ls_file=$2
  shift 2

  need_rebuild=0
  for ko in "$@"; do
    [ -f "$ko" ] || continue
    b=$(basename "$ko")
    if grep -Fq "$b" "$initrd_ls_file"; then
      log "Module $b present in initramfs for kernel $kver"
      need_rebuild=1
      break
    fi
  done

  [ "$need_rebuild" -eq 1 ] || return 0

  if is_debian_like; then
    if command -v update-initramfs >/dev/null 2>&1; then
      log_no_newline "Rebuilding initramfs for kernel $kver (update-initramfs)..."
      update-initramfs -k "$kver" -u >/dev/null 2>&1 || (log "Failed to rebuild initramfs!" && exit 1)
      log "DONE"
    fi
  else
    if command -v dracut >/dev/null 2>&1; then
      img=$(initrd_path_for_kver "$kver")
      log_no_newline "Rebuilding initramfs for kernel $kver (dracut)..."
      dracut -f "$img" "$kver" >/dev/null 2>&1 || (log "Failed to rebuild initramfs!" && exit 1)
      log "DONE"
    fi
  fi
}

# -------- main --------
# shellcheck disable=SC1091  # (if you run shellcheck elsewhere)
if [ -r /etc/os-release ]; then
  . /etc/os-release
fi

log "Checking for updated IB Core modules..."

REPO_BASE=/opt/nvmesh/common-repo
MODULES_BASE=/lib/modules
RUN_KVER=$(uname -r)
OFED_VER_STRING=$(ofed_info -s 2>/dev/null || echo "none")
OFED_VER_STRING=${OFED_VER_STRING%:*}
OFED_FULL_VER=$(echo "$OFED_VER_STRING" | grep -Eo "[0-9.]+[0-9.-]+")
OFED_VER=$(echo "$OFED_VER_STRING" | grep -Eo "[0-9.]+" | head -1)
reboot_needed=0

[ -d "$REPO_BASE" ] || (log "NVMesh common-repo not found" && exit 0)

# So far only non-OFED is supported
for f in $REPO_BASE/*/ib_core_modules.tar.gz; do
  [ -f "$f" ] || continue

  # Extract repo name from the path
  repo=${f#$REPO_BASE/}
  repo=${repo%/ib_core_modules.tar.gz}

  # Extract kernal and OFED version from the repo name
  ofed_ver=$(echo "$repo" | cut -d_ -f2)
  kver=$(echo "$repo" | cut -d_ -f3-)

  # Check if ofed_ver matches installed OFED version
  case "$ofed_ver" in
    "$OFED_VER_STRING"|"OFED-internal-$OFED_FULL_VER"|OFED-internal-$OFED_VER-*|MLNX_OFED_LINUX-$OFED_FULL_VER-*MLNX_OFED_LINUX-$OFED_VER-*)
      # Version matches, continue processing
      ;;
    *)
      log "Skipping modules for not-installed OFED version $ofed_ver"
      continue
      ;;
  esac

  moddir="$MODULES_BASE/$kver"
  if [ ! -d "$moddir" ]; then 
    log "Skipping modules for not-installed kernel $kver"
    continue
  fi

  install_dir=$MODULES_BASE/$kver/extra/nvmesh/ib_core

  if [ -d "$install_dir" ]; then
    log "Removing existing install directory $install_dir"
    rm -rf "$install_dir"
  fi

  log_no_newline "Extracting modules for kernel: $kver and OFED: $ofed_ver..."
  mkdir -p $install_dir
  tar -xzf "$f" -C "$install_dir" || (log "Failed to extract modules!" && continue)
  log "DONE"
  log_no_newline "Running depmod..."
  depmod -a "$kver" || (log "Failed to run depmod!" && continue)
  log "DONE"

  # Collect module files safely (no ls; handles empty matches)
  set -- "$install_dir"/*.ko*
  [ -e "$1" ] || (log "No modules found in $install_dir" && continue)
  # Now "$@" are the matched module paths

  # Print basenames of modules
  mods=""
  for ko in "$@"; do
    b=$(basename "$ko")
    if [ -z "$mods" ]; then
      mods=$b
    else
      mods="$mods $b"
    fi
  done
  log "Found modules: $mods for kernel: $kver and OFED: $ofed_ver."
  
  initrd_ls_file=$(mktemp)
  # Ensure temp file is removed even if we hit 'set -e' paths later
  trap 'rm -f "$initrd_ls_file"' EXIT HUP INT TERM

  list_initrd "$kver" "$initrd_ls_file"

  # Only attempt rebuild if we successfully listed something
  if [ -s "$initrd_ls_file" ]; then
    rebuild_initrd_if_needed "$kver" "$initrd_ls_file" "$@"
  fi

  # Clean temp for this iteration; keep trap for safety
  rm -f "$initrd_ls_file"
  trap - EXIT HUP INT TERM

  # ---- Reboot decision: ONLY based on the currently running kernel ----
  [ "$kver" = "$RUN_KVER" ] || continue

  log "Checking running kernel $kver if reboot needed..."

  for ko in "$@"; do
    [ -f "$ko" ] || continue

    name=$(/sbin/modinfo -F name "$ko" 2>/dev/null || :)
    inst_ver=$(/sbin/modinfo -F version "$ko" 2>/dev/null || :)

    [ -n "$name" ] || continue
    [ -d "/sys/module/$name" ] || continue

    run_ver=$(cat "/sys/module/$name/version" 2>/dev/null || :)

    if [ -z "$run_ver" ] || [ "$run_ver" != "$inst_ver" ]; then
      log "Updated module $name does not match running version -> reboot needed"
      reboot_needed=1
      touch /var/run/nvmesh/reboot_needed
      break
    fi
  done
done

if [ "$reboot_needed" -eq 1 ]; then
  log "Updated IB Core modules installed; reboot required!"
else
  log "Reboot not needed"
fi

exit 0
