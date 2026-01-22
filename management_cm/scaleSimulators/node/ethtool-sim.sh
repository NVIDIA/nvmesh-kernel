#!/usr/bin/env bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0



if [ "$1" == "-S" ]; then
  cat /tmp/statistics/node/nics/$2/data_for_ethtool_sim
elif [ "$1" == "-p" ]; then
  echo "Pause parameters for $2:\nAutonegotiate:	off\nRX:		on\nTX:		on\n"
else
  echo "ethtool Simulator For NVMesh statistics Simulator"
  echo "Only -S and -a flags are supported"
  exit 1
fi
