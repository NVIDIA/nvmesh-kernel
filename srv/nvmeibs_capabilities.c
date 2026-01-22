/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibs_capabilities.h"
#include "common/nvmeib_macro_magic.h"
#include "common/proc_epilog.h"

#define PROC_NVMEIBS_DISKS_CSV_VER 1
#define PROC_NVMEIBS_NIC_GIDS_CSV_VER 1
#define PROC_NVMEIBS_NICS_CSV_VER 1
#define PROC_NVMEIBS_PARTITIONS_CSV_VER 1
#define PROC_NVMEIBS_SERJIO_CLIENTS_CSV_VER 1
#define PROC_NVMEIBS_SERJIO_JENTRIES_CSV_VER 1
#define PROC_NVMEIBS_SERJIO_JMDC_CSV_VER 1
#define PROC_NVMEIBS_SERJIO_JMDC_MAPPING_CSV_VER 1
#define PROC_NVMEIBS_SERJIO_PARTITIONS_CSV_VER 1
#define PROC_NVMEIBS_SERJIO_RANGES_CSV_VER 1
#define PROC_NVMEIBS_SERJIOS_CSV_VER 1

#define NVMEIBS_CAPABILITIES_STRING \
"\nformat:\n" \
"  version: 1\n" \
"build:\n" \
"  is_production: " STRINGIFY(NVMESH_IS_PRODUCTION_COMPILATION) "\n" \
"  take_stats: " STRINGIFY(TAKE_STATS) "\n" \
"CSV encoding:\n" \
"  /proc/nvmeibs/disks.csv: " STRINGIFY(PROC_NVMEIBS_DISKS_CSV_VER) "\n" \
"  /proc/nvmeibs/nic_gids/*.csv: " STRINGIFY(PROC_NVMEIBS_NIC_GIDS_CSV_VER) "\n" \
"  /proc/nvmeibs/nics.csv: " STRINGIFY(PROC_NVMEIBS_NICS_CSV_VER) "\n" \
"  /proc/nvmeibs/partitions.csv: " STRINGIFY(PROC_NVMEIBS_PARTITIONS_CSV_VER) "\n" \
"  /proc/nvmeibs/serjio/*/clients.csv: " STRINGIFY(PROC_NVMEIBS_SERJIO_CLIENTS_CSV_VER) "\n" \
"  /proc/nvmeibs/serjio/*/jentries/*.csv: " STRINGIFY(PROC_NVMEIBS_SERJIO_JENTRIES_CSV_VER) "\n" \
"  /proc/nvmeibs/serjio/*/jmdc/*.csv: " STRINGIFY(PROC_NVMEIBS_SERJIO_JMDC_CSV_VER) "\n" \
"  /proc/nvmeibs/serjio/*/jmdc_mapping.csv: " STRINGIFY(PROC_NVMEIBS_SERJIO_JMDC_MAPPING_CSV_VER) "\n" \
"  /proc/nvmeibs/serjio/*/partitions.csv: " STRINGIFY(PROC_NVMEIBS_SERJIO_PARTITIONS_CSV_VER) "\n" \
"  /proc/nvmeibs/serjio/*/ranges.csv: " STRINGIFY(PROC_NVMEIBS_SERJIO_RANGES_CSV_VER) "\n" \
"  /proc/nvmeibs/serjios.csv: " STRINGIFY(PROC_NVMEIBS_SERJIOS_CSV_VER) "\n"

const char capabilities[] = NVMEIBS_CAPABILITIES_STRING;

MODULE_INFO(nvmeibs_capabilities, NVMEIBS_CAPABILITIES_STRING);

const char* nvmeibs_get_capabilities(void)
{
	return capabilities;
}
