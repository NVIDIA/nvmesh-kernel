#ifndef TOMA_SANDBOX_NVME_H_INCLUDED
#define TOMA_SANDBOX_NVME_H_INCLUDED

struct sandbox_nvme_device {
	int vendor_id;
	const char *serial_number;
	const char *model_number;
	// Short name for the device
	const char *device_name;
	// The path we actually use for the device, e.g. `_root/dev/nvme0n1`.
	const char *device_path;
	bool stock_disk;
};

struct sandbox_nvme_device *sandbox_nvme_get_device_by_path(const char *path);

#endif // TOMA_SANDBOX_NVME_H_INCLUDED
