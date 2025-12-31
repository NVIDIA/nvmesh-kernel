#ifndef TOMA_SANDBOX_NVME_H_INCLUDED
#define TOMA_SANDBOX_NVME_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

void sandbox_nvme_init(void);

struct sandbox_nvme_device {
	int vendor_id;
	const char *serial_number;
	const char *model_number;
	// Short name for the device
	const char *device_name;
	// The path we actually use for the device, e.g. `_root/dev/nvme0n1`.
	const char *device_path;
	bool stock_disk;
	uint64_t size_in_blocks;
};

struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path);
int sandbox_nvme_get_device_count(void);
struct sandbox_nvme_device *sandbox_nvme_get_device_by_index(int index);

#define SANDBOX_NVME_BLOCK_SIZE_EXPONENT 12 // logical sector size as exponent of 2 (2**12 = 4096 bytes).

#endif // TOMA_SANDBOX_NVME_H_INCLUDED
