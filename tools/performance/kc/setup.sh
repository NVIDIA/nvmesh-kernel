#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# setup.sh — install FlameGraph tooling and set nvmeibc module params on client hosts

set -euo pipefail

HOSTS=(nvme171 nvme172 nvme173 nvme180)

DEFAULT_MODPROBE_OPTS='options nvmeibc lock_ch_get_method=0 lock_shard_type=0 disk_locks_use_max_rd_atomic_limit=Y nvmeibc_io_pet_disable=1'
MODPROBE_OPTS="${1:-${DEFAULT_MODPROBE_OPTS}}"

MODPROBE_CONF="/etc/modprobe.d/nvmesh_lock_ch.conf"

FLAMEGRAPH_DIR="${HOME}/FlameGraph"

# Install FlameGraph if not already present
if [[ ! -d "${FLAMEGRAPH_DIR}" ]]; then
  echo "=== Installing FlameGraph to ${FLAMEGRAPH_DIR} ==="
  git clone https://github.com/brendangregg/FlameGraph "${FLAMEGRAPH_DIR}"
else
  echo "=== FlameGraph already installed at ${FLAMEGRAPH_DIR} ==="
fi

for h in "${HOSTS[@]}"; do
  echo "=== $h: configure ==="
  ssh "root@$h" "cat > ${MODPROBE_CONF} << 'EOF'
${MODPROBE_OPTS}
EOF
cat ${MODPROBE_CONF}"
done

for h in "${HOSTS[@]}"; do
  echo "=== $h: restart nvmeshclient ==="
  ssh "root@$h" "systemctl restart nvmeshclient"
  sleep 3
done

# Build per-parameter sysfs read commands from MODPROBE_OPTS
PARAM_VERIFY_CMDS=""
while IFS= read -r param; do
  PARAM_VERIFY_CMDS+="echo '${param}='\$(cat /sys/module/nvmeibc/parameters/${param} 2>/dev/null || echo N/A)"$'\n'
done < <(echo "${MODPROBE_OPTS}" | grep -oE '[^= ]+=[^ ]+' | cut -d= -f1)

# Build grep pattern for startup log
PARAM_GREP_PATTERN=$(echo "${MODPROBE_OPTS}" | grep -oE '[^= ]+=[^ ]+' | cut -d= -f1 | paste -sd'|')

for h in "${HOSTS[@]}"; do
  echo "=== $h: verify ==="
  ssh "root@$h" "
    echo 'service='\$(systemctl is-active nvmeshclient)
    ${PARAM_VERIFY_CMDS}
    grep -E '${PARAM_GREP_PATTERN}' /var/log/nvmesh/on_startup_module_params.client
  "
  echo
done
