/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef TOMA_SANDBOX_NVME_H_INCLUDED
#define TOMA_SANDBOX_NVME_H_INCLUDED

#include "os/os_internal.h"

// API towards server simulator
struct sandbox_nvme_lbaf {			// NVMe LBA format descriptor. Matches the structure used in NVMe Identify NS response.
	uint8_t block_size_exp;			// Block size as exponent of 2 (9=512, 12=4096) bytes
	uint16_t metadata_size;			// Metadata size in bytes (0 or 8) bytes
};

enum SANDBOX_NVME_FMT_e {			// Supported LBA formats for sandbox NVMe devices. This table is indexed by format ID (0-3).
	SANDBOX_NVME_FMT_512_0 = 0,		// 512 bytes, no metadata
	SANDBOX_NVME_FMT_512_8 = 1,		// 512 bytes, 8 bytes metadata
	SANDBOX_NVME_FMT_4096_0 = 2,	// 4096 bytes, no metadata
	SANDBOX_NVME_FMT_4096_8 = 3,	// 4096 bytes, 8 bytes metadata
	SANDBOX_NVME_LBAF_COUNT = 4,	// Number of supported LBA formats.
};
const struct sandbox_nvme_lbaf *sandbox_nvme_get_lbaf(enum SANDBOX_NVME_FMT_e fmt_idx);

struct sandbox_nvme_device {
	const struct sb_disk_conf *conf;	// Pointer to this disk configuration as seen by mgmt for comparison;
	struct TSB_os_mmap_impl ram;		// RAM allocated for this nvme device by the nvmeibs server to hold locks and binfo
	const int vendor_id;
	const char *serial_number;
	const char *model_number;
	const char *device_name;			// Short name for the device
	const char *device_path;			// The path we actually use for the device, e.g. `_root/dev/nvme0n1`.
	const bool stock_disk;
	uint64_t size_in_bytes;				// Physical capacity in bytes. Block count is derived dynamically: size_in_bytes / block_size. Stock disks derive this from the image file at init time.
	enum SANDBOX_NVME_FMT_e current_format_idx;		// Mutable, format operations can update it.
};

uint64_t sandbox_nvme_get_n_blocks(const struct sandbox_nvme_device *dev);
void sandbox_nvme_init(void);
unsigned  sandbox_nvme_get_device_count(void);
const struct sandbox_nvme_device *sandbox_nvme_get_device_arr(void);
      struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id(  const char *disk_name);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_full_path(const char *path);
      struct sandbox_nvme_device *sandbox_nvme_get_device_by_ram_mmap( const void *addr);
int  sandbox_nvme_format_disk(   const char* disk_id, enum SANDBOX_NVME_FMT_e fmt_idx);	// Format a device: update LBA format and erase disk content. return 0 on success, -1 if format index invalid or I/O error
int  sandbox_nvme_zero_disk_area(const char* disk_id, size_t start_block, size_t num_blocks);
int  sandbox_nvme_io_to_disk(    const char* disk_id, size_t start_block, size_t num_bytes, void *data, bool is_read);

// API towards operating system
#include <stdarg.h>				// va_list
int nvme_ioctl_admin_cmd(const char *path, int fd, va_list ap);
int nvme_ioctl_get_size( const char *path,         va_list ap);
#endif // TOMA_SANDBOX_NVME_H_INCLUDED
