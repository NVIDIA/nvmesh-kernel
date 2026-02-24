#ifndef NVMEIBC_IDISK_H
#define NVMEIBC_IDISK_H

#include "kr_incs.h"
#include "common/nvmeib_shared.h"

enum nvmeibc_disk_status {
	d_online,
	d_offline
};

struct nvmeibc_disk_client_journal;

struct nvmeibc_idisk { /* shared interface/contract between block & core */
	struct {
		__attribute__((nonnull(1)))
		char const *(*get_name)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		char const *(*get_full_name)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		char const *(*get_host_name)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		bool (*should_pause)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		bool (*is_cont_preventors_waited_too_long)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		int (*get_sector_shift)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		int (*get_md_size)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		int (*get_max_request_size_bytes)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		bool (*is_access_local)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		size_t (*get_min_gen_cmd_bb)(struct nvmeibc_idisk const *self);
		__attribute__((nonnull(1)))
		struct nvmeibc_disk_client_journal const *(*get_journal)(struct nvmeibc_idisk const *self);
		/* for tests, will be removed in future */
		__attribute__((nonnull(1)))
		struct nvmeibc_disk_client_journal *(*get_journal_mut)(struct nvmeibc_idisk *self);
		__attribute__((nonnull(1)))
		int (*read_cont_preventors)(struct nvmeibc_idisk *self);
		__attribute__((nonnull(1)))
		int (*inc_cont_preventors)(struct nvmeibc_idisk *self);
		__attribute__((nonnull(1)))
		int (*dec_cont_preventors)(struct nvmeibc_idisk *self);
		__attribute__((nonnull(1)))
		enum nvmeibc_disk_status (*get_status)(struct nvmeibc_idisk const * self);
		__attribute__((nonnull(1)))	
		void (*call_discover)(struct nvmeibc_idisk *self);
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

/* Segment sw_md_size shall be taken from disk.
   There is a corner case though, if the disk is dead, md may be 0
   even if it is not.
   For EC praids, it is a problem as we later rely on metadata non 0.
   This is why, in case md_size is 0, and it is ec, we will put md = 8
   anyway. The assumption is that as soon as disk goes up, and discovery
   runs, new topology size will arive, and we will set correct md_size
   there.

	BOTTOM LINE: in most cases you should query the topology(segment) for the sw_md_size
	It will stay unchaged as long as topology leaving;
*/
static inline int nvmeibc_idisk_get_sw_md_size(struct nvmeibc_idisk *disk)
{
	return disk->ops.get_md_size(disk) << (NVMEIBC_SECTOR_SHIFT - disk->ops.get_sector_shift(disk));
}

#endif /* NVMEIBC_IDISK_H */
