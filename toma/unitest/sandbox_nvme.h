#ifndef TOMA_SANDBOX_NVME_H_INCLUDED
#define TOMA_SANDBOX_NVME_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

void sandbox_nvme_init(void);

/**
 * NVMe LBA format descriptor.
 * Matches the structure used in NVMe Identify NS response.
 */
struct sandbox_nvme_lbaf {
	uint8_t block_size_exp;         // Block size as exponent of 2 (9=512, 12=4096)
	uint16_t metadata_size;         // Metadata size in bytes (0 or 8)
};

/**
 * Number of supported LBA formats.
 */
#define SANDBOX_NVME_LBAF_COUNT 4

/**
 * Get LBA format descriptor by index.
 * Aborts with BUG_ON if index is out of range [0, SANDBOX_NVME_LBAF_COUNT).
 *
 * @param fmt_idx Format index (0-3)
 * @return Pointer to the LBA format descriptor (never NULL)
 */
const struct sandbox_nvme_lbaf *sandbox_nvme_get_lbaf(int fmt_idx);

/* Format index constants for clarity */
#define SANDBOX_NVME_FMT_512_0   0   /* 512 bytes, no metadata */
#define SANDBOX_NVME_FMT_512_8   1   /* 512 bytes, 8 bytes metadata */
#define SANDBOX_NVME_FMT_4096_0  2   /* 4096 bytes, no metadata */
#define SANDBOX_NVME_FMT_4096_8  3   /* 4096 bytes, 8 bytes metadata */

struct sandbox_nvme_device {
	int vendor_id;
	const char *serial_number;
	const char *model_number;
	const char *device_name;			// Short name for the device
	const char *device_path;			// The path we actually use for the device, e.g. `_root/dev/nvme0n1`.
	bool stock_disk;
	uint64_t size_in_blocks;
	uint8_t current_format_idx;         // Index into sandbox_nvme_lbaf_table
};

const struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path /* == /dev/nvme...n1 */);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_index(int index);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id(const char *disk_id /* e.g. NVMD_SN_002.1 */);
struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id_mut(const char *disk_id /* e.g. NVMD_SN_002.1 */);
int sandbox_nvme_get_device_count(void);
int sandbox_nvme_open(const struct sandbox_nvme_device *dev);

/**
 * Format a device: update LBA format and erase disk content.
 * Simulates what a real NVMe format operation does.
 *
 * @param dev The device to format (must not be NULL)
 * @param fmt_idx The new format index (0-3)
 * @return 0 on success, -1 if format index invalid or I/O error
 */
int sandbox_nvme_format_disk(struct sandbox_nvme_device *dev, int fmt_idx);

#endif // TOMA_SANDBOX_NVME_H_INCLUDED
