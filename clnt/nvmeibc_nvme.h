#ifndef NVMEIBC_NVME_H
#define NVMEIBC_NVME_H

#include "kr_incs.h"
#include "nvmeib_nvme.h"

struct nvmeibc_disk_channel_rsc;
int nvmeibc_fill_rwsq(u8 nvme_op, struct nvmeibc_disk_channel_rsc *info,
	u32 command_id, int entry, u64 disk_sector, int len_bytes, int *prpl_len,
	u8 do_512b_sub_block_x);

static inline int nvmeibc_fill_rsq(struct nvmeibc_disk_channel_rsc *info,
	u32 command_id, int entry, u64 disk_sector, int len_bytes, int *prpl_len,
	u8 do_512b_sub_block_x) {
	return nvmeibc_fill_rwsq(nvme_cmd_read, info, command_id, entry, disk_sector,
		len_bytes, prpl_len, do_512b_sub_block_x);
}
static inline int nvmeibc_fill_wsq(struct nvmeibc_disk_channel_rsc *info,
	u32 command_id, int entry, u64 disk_sector, int len_bytes, int *prpl_len,
	u8 do_512b_sub_block_x) {
	return nvmeibc_fill_rwsq(nvme_cmd_write, info, command_id, entry, disk_sector,
		len_bytes, prpl_len, do_512b_sub_block_x);
}
int nvmeibc_fill_dsq(struct nvmeibc_disk_channel_rsc *info,
	u32 command_id, int entry, u64 disk_sector, int len_bytes, int *prpl_len);

#endif
