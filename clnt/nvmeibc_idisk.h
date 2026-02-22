#ifndef NVMEIBC_IDISK_H
#define NVMEIBC_IDISK_H

#include "kr_incs.h"

enum nvmeibc_disk_status {
	d_online,
	d_offline
};

struct nvmeibc_disk_client_journal;

struct nvmeibc_idisk { /* shared interface/contract between block & core */
	struct {
		char const *(*get_name)(struct nvmeibc_idisk const *self);
		char const *(*get_full_name)(struct nvmeibc_idisk const *self);
		char const *(*get_host_name)(struct nvmeibc_idisk const *self);
		bool (*should_pause)(struct nvmeibc_idisk const *self);
		bool (*is_cont_preventors_waited_too_long)(struct nvmeibc_idisk const *self);
		int (*get_sector_shift)(struct nvmeibc_idisk const *self);
		int (*get_md_size)(struct nvmeibc_idisk const *self);
		int (*get_max_request_size_bytes)(struct nvmeibc_idisk const *self);
		bool (*is_access_local)(struct nvmeibc_idisk const *self);
		size_t (*get_min_gen_cmd_bb)(struct nvmeibc_idisk const *self);
		struct nvmeibc_disk_client_journal const *(*get_journal)(struct nvmeibc_idisk const *self);
		/* for tests, will be removed in future */
		struct nvmeibc_disk_client_journal *(*get_journal_mut)(struct nvmeibc_idisk *self);
		int (*read_cont_preventors)(struct nvmeibc_idisk *self);
		int (*inc_cont_preventors)(struct nvmeibc_idisk *self);
		int (*dec_cont_preventors)(struct nvmeibc_idisk *self);
		enum nvmeibc_disk_status (*get_status)(struct nvmeibc_idisk const * self);
	} ops;
};

static inline const char* nvmeibc_idisk_get_host_name_for_logging(struct nvmeibc_idisk const* disk)
{
	const char* name = disk->ops.get_host_name(disk);
	if (!name || name[0] == '?')
		return "Unknown";
	else
		return name;
}

static inline bool nvmeibc_idisk_is_512b_sub_block_x_supported(const struct nvmeibc_idisk* disk)
{	//TODO: in near future, the KC simulator will probably have it's own implementation, so this code will move there
	#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
		(void)disk;
		return true;
	#else
		return disk->ops.get_sector_shift(disk) == 9 && disk->ops.get_md_size(disk) == 0;
	#endif
}


#endif /* NVMEIBC_IDISK_H */
