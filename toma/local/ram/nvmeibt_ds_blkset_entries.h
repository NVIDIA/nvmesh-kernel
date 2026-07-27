#ifndef NVMEIBT_DS_BLKSET_ENTRIES_H
#define NVMEIBT_DS_BLKSET_ENTRIES_H

/* Manage RAM blockset entries table for segment */
#include "nvmeibt_params.h"
#include "nvmeibt_common.h"

struct nvmeibt_seg_active;

bool nvmeibt_ds_metadata_init_EC_locks_table(    struct nvmeibt_seg_active *);
bool nvmeibt_ds_metadata_init_non_EC_locks_table(struct nvmeibt_seg_active *);

// Pack/Unpack the full RAM of the segment into/from buffer (to save on disk).
//	- Returns the number of bytes used
uint64_t nvmeibt_ds_blkset_entries___pack(union nvmeib_lock_blkset_entry *src, char *dst, struct nvmeibt_disk_segment *ds);
void     nvmeibt_ds_blkset_entries_sanitize_packed(BOOL is_EC, char *disk_blk_buf, uint64_t n_blksets, uint64_t *n_stale_locks, uint64_t *n_dirty_bits);
uint64_t nvmeibt_ds_blkset_entries_unpack(union nvmeib_lock_blkset_entry *dst, char *src, struct nvmeibt_disk_segment *ds);

/* Given a 'querry' {true/false} for each segment, test it for specific blockset
	This function encapsulates the logic of stride and which parts of blockset
	entries are used*/
static inline bool nvmeibt_ds_blkset_entries_check_condition(uint64_t blkset_no, int n_topo_segs, const bool querry[NVMEIBT_MAX_N_SEGMENTS_IN_PRAID])
{
	const int n_blksets_in_stride = (1 << LOCK_CHANGE_STRIDE_SHIFT);
	int	natural_owner_seg_idx = (blkset_no / n_blksets_in_stride) % n_topo_segs;	// Identical to client get_owner_seg_slice_start() function
	return querry[natural_owner_seg_idx];
}


#endif
