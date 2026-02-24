#ifndef TOMA_SANDBOX_NVME_H_INCLUDED
#define TOMA_SANDBOX_NVME_H_INCLUDED

#include "sandbox_util.h"

void sandbox_nvme_init(void);

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
const struct sandbox_nvme_lbaf *sandbox_nvme_get_lbaf(int /*enum SANDBOX_NVME_FMT_e*/ fmt_idx);

struct sandbox_nvme_device {
	int vendor_id;
	const char *serial_number;
	const char *model_number;
	const char *device_name;			// Short name for the device
	const char *device_path;			// The path we actually use for the device, e.g. `_root/dev/nvme0n1`.
	bool stock_disk;
	uint64_t size_in_blocks;
	enum SANDBOX_NVME_FMT_e current_format_idx;		// Mutable, format operations can update it.
};

//const struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path /* == /dev/nvme...n1 */);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_index(int index);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id(const char *disk_id /* e.g. NVMD_SN_002.1 */);
struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id_mut(  const char *disk_id /* e.g. NVMD_SN_002.1 */);
int sandbox_nvme_get_device_count(void);
int sandbox_nvme_open(const struct sandbox_nvme_device *dev);
int sandbox_nvme_format_disk(struct sandbox_nvme_device *dev, enum SANDBOX_NVME_FMT_e fmt_idx);	// Format a device: update LBA format and erase disk content. return 0 on success, -1 if format index invalid or I/O error

#include <stdarg.h>				// va_list
int nvme_ioctl_admin_cmd(const char *path, int fd, va_list ap);
int nvme_ioctl_get_size( const char *path,         va_list ap);
#endif // TOMA_SANDBOX_NVME_H_INCLUDED
