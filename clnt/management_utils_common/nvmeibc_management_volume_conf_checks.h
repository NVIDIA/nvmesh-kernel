#ifndef NVMEIBC_MANAGEMENT_VOLUME_CONF_CHECK_H_
#define NVMEIBC_MANAGEMENT_VOLUME_CONF_CHECK_H_

#include "nvmeibc_mcs_stub.h"

//actually, after an innocent macro there is a hidden powerfull check that ensures
//that all praids in a chunk have exactly the same number of segments
//nsegs
//	- represents the number of segments in praid.
//	- usually it is calculated taking 3 variable into account:
//		- dataBlocks - number of data segments
//		- numberOfMirrors - number of time the data segment is mirrored; in case of EC or JBOD it should contain 0
//		- parityBlocks - number of segments dedicated to parity; in case of mirror or JBOD should contain 0
//p - praid, stripeIndex - index of raid within the chunk
//s - segment, pRaidTypeIndex - index of segment within the raid
//I (Roman) could be wrong, but it looks like segment type (s->type) is always set to TYPE_DATA(==0)
//marking at least one segment as TYPE_PARITY breaks the check
#define get_allocation_index(nsegs, p, s) \
	(nsegs * p->stripeIndex + s->pRaidTypeIndex + ((s->type == TYPE_PARITY) ? p->dataBlocks : 0))

#define calc_segment_length(seg) ((seg)->lbe + 1 - (seg)->lbs)

int __get_valid_stripe_size(const struct nvmeibc_chunk_conf *cur_chunk, const struct nvmeibc_praid_conf *cur_praid, int chunk_idx);

void nvmeibc_management_try_setup_stripe_size_safe(struct nvmeibc_chunk_conf *cur_chunk, const struct nvmeibc_praid_conf *cur_praid, int chunk_idx);

// This function used to be in update_ranges, but is no longer relevant
// Now it will fail if any segments striping index is out of bounds
// Or if any segment in a chunk has a different length, or if a chunk is missing
// Segment with index 0
int __check_striping_length_and_chunk(u32 binje, const struct nvmeibc_volume_conf *conf);

#define nvmeibc_managment_does_vol_need_disks(conf)  (true)

int nvmeibc_management_does_vol_have_disks(const struct nvmeib_mgmt_to_client_volume_configuration* conf);

int nvmeibc_management_um_checks(const struct nvmeibc_volume_conf* vol_conf);

void nvmeibc_management_init_defaults_safe(struct nvmeibc_volume_conf* vol_conf);
#endif
