/**
 * serjio public data structures
 *
 *  */

#ifndef NVMEIBS_SERJIO_DEFS_H
#define NVMEIBS_SERJIO_DEFS_H
#include "common/kr_incs.h"

struct nvmeibs_serjio_lba_range {
	u64 lba;				// Units of physical disk block (not block devices 4K)
	u64 len_nlbas;
	int part_idx;				// Partition Index in GPT
	efi_guid_t part_uuid;			// Partition Unique UUID
};

struct nvmeibs_serjio_disk_ranges{
	struct nvmeibs_serjio_lba_range journal;
	struct nvmeibs_serjio_lba_range db;
};

#endif //NVMEIBS_SERJIO_DEFS_H
