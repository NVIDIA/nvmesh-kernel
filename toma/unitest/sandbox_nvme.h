#ifndef TOMA_SANDBOX_NVME_H_INCLUDED
#define TOMA_SANDBOX_NVME_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

void sandbox_nvme_init(void);

struct sandbox_nvme_device {
	int vendor_id;
	const char *serial_number;
	const char *model_number;
	const char *device_name;			// Short name for the device
	const char *device_path;			// The path we actually use for the device, e.g. `_root/dev/nvme0n1`.
	bool stock_disk;
	uint64_t size_in_blocks;
};

const struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path /* == /dev/nvme...n1 */);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_index(int index);
const struct sandbox_nvme_device *sandbox_nvme_get_device_by_disk_id(const char *disk_id /* e.g. NVMD_SN_002.1 */);
int sandbox_nvme_get_device_count(void);
int sandbox_nvme_open(const struct sandbox_nvme_device *dev);

#define SANDBOX_NVME_BLOCK_SIZE_EXPONENT 12 // logical sector size as exponent of 2 (2**12 = 4096 bytes).

#endif // TOMA_SANDBOX_NVME_H_INCLUDED
