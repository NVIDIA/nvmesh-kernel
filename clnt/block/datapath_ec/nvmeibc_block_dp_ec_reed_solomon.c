#include "nvmeibc_block_dp_ec_reed_solomon.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"

/* Includes All the math, algorithms and NIC optimizations of erasure coding
   calcualtions */
#include "nvmeibc_block_dp_ec.h" // for E_CMDS_STAGE
#include "nvmeibc_block_dp_ec_gf.h"
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"

// TODO: Review documentation and update if required

/* XOR parity calculation is done in the following way:
   We iterate slice by slice and calculate parities by the strategy for the current slice
   We have 3 strategies:
   1. Update
   2. Complement AKA Full-slice
   3. Restore

   Update:
		Update strategy is called when there are fewer reads to parities and data affected
		then for slice size - affected data
	Complement:
		We have all data blocks of the slice and calculate parities once
	Restore:
		We only write to partial memebers of the block but cannot update due
		to degraded data and cannot complement due to a different member
		we first restore the non-written to member and calculate a full slice
		from the new blocks and all the old ones

	When required we calculate CRC as well as parities at the same time.
	CRC is dependant of the rlba of the block and is used as salt (unique input) for the
	CRC calculation.
	The rlba is dependent on the slice size and snake size of the volume
	For snake size == 1 each slice member is the next rlba and the next slice starts
	at previous rlba + slice size.
	For snake size > 1 the next data member rlba is current rlba + snake size
	Note: when snake size == 1 it is the same as above
	Each slice start rlba is either the previous slice start rlba + 1 or a full snake
	rlba value: slice size * snake size, again when snake size == 1 this is the same as above

	The input to all functions is the operation MSSA:
	We use the MSSA to access both the data blocks and the metadata
	We iterate over all slices and act according to the current slice stratgey using the struct t_strat_changer
	For each slice (row) we set the data/parity into the input/output vectors of the reed solomon calculation
	We prepare the in, out and crc bitmaps and store the CRC into the metadata of the relevent block

	Example for slice size = 3, snake size = 1 and 1 parity

   We have a 3+1 Raid on 4 segments. Data blocks are arranged as
   Segments - S0 | S1 | S2 | S3         S0 | S1 | S2 | S3
              ---+----+----+----        ---+----+----+----
   Rlba addr:(D0) (D1)  D2   P0                   N0   NP0
			  D3   D4   D5   P1         N1   N2   N3   NP1
			  D6   D7   D8   P2         N4   N5   N6   NP2
			  D9  (DA) (DB)  P3         N7             NP3
    IO is write of 8 blocks: [2..9] generates new data N[0..7] (see above)
	We need to calculate new parity NP[0..3]:
		NP0 = D2^N0^P0	|| D0^D1^N0 -> read either D0 and D1 or N2 and P0 (same read overhead both strategies are valid we choose complement in this case)
		NP1 = D3^N1^D4^N2^D5^N3^P1 -> == N1^N2^N3 - Complement
		NP2 = D6^N4^D7^N5^D8^N6^P2 -> == N4^N5^N6 - Complement
		NP3 = D9^N7^P3  || N7^DA^DB -> same as partial first slice



    Currently Parity calculation is done with GF arithmetics.
    The api is nvmeibc_reed_solomon_update_parities, nvmeibc_reed_solomon_fill_missing,
    All of these functions get the properties of the slice (N data segs and K parity segs, S snake size).
    They get the in, out and crc bitmaps and the slice rlba
    They get an array of buffers (vec) for each block in the slice ordered (D0-DN-1, P0-PK-1), with a NULL pointer
    when data is missing, the update function gets Old Vec and New Vec with all P and D required.
    Decode can calculate up to K missing data blocks.
*/

#define get_mssa_page(sg) ((sg) ? sg_virt(sg) : NULL)

#include "../datapath_utils_generic/nvmeibc_block_dp_block_md.h"
// For a full blockset read (sync) we have a single SG but we can only calculate
// Crc one block at a time, so we need to increment the pointer within the SG
// And continue with the Parity/CRC calculations

// Consider using this for full lockset sync if we have a single sgl element
// TODO might be able to use with mssa for sync (all syncs are same length and same bitmaps, just call this function and repeat (increase row variable)
static void __attribute__((__unused__)) __increment_to_next_block(u8 *data[N_MAX_RAID_SLICE_LEN], u8 *coding[], const int slice_size, const int n_pari_w)
{
	int i;
	for (i=0;i<slice_size;i++) { // Increase each valid pointer
		if (i < n_pari_w) {
			if (coding[i] != NULL)
				coding[i] += NVMEIBC_SECTOR_SIZE;
		}
		if (data[i] != NULL)
			data[i] += NVMEIBC_SECTOR_SIZE;
	}
}
// Increment each buffer to the next block, Todo handle CRC values
// Unused
#define __prepare_next_sync_block __increment_to_next_block

/************************** Generic functions *********************************/
static void __set_reed_solo_rv_on_cmds(struct nvmeibc_block_command *rldr, int rv)
{
	// Set on rldr as dp_cmds_prev_stage_analyze_rv will check rldr
	// Read solomon, even though mathematically fixes a problem it never cleans the rv of the problem because the fix will be commited & applied by someone else. The original failure remains. So error is compond, input error or calcualtion error
	if (rv && !rv_storage_of_binfo_write(rldr))
		__cmd_set_comp_err(rldr, rv);
}

static void __return_execution_to_cmd_stages(struct nvmeibc_block_command *cmd, int rv)
{
	cmd->iocmd->comp.comp_code = rv;
	dp_dbgdi_do_add_restore_info(cmd, false);
	dp_ec_block_completion(&cmd->iocmd->comp, nvmeibc_d_iocmd_comp_tag_make());
}

static void set_final_edic_value_from_crc_res(void *_md, const u32 crc, const bool is_parity)
{
	BUG_ON(_md == NULL); // Must not be NULL, if it is should have ben unset in crc_bm used to set EDIC
	nvmeibc_block_dp_ec_md_set_edic(_md, crc, is_parity);
}

static bool scrub_compare_edic_with_crc(void *_md, const u32 crc)
{
	const u32 edic = crc & NVMEIBC_DP_EC_MD_EDIC_MASK(true);
	union nvmeibc_block_dp_ec_data_block_md *md = _md;
	if (nbdpec_md_was_data_never_written(md)) { // Nothing to compare to
		return false;
	}
	return (md->P.edic != edic);
}

/************************** Edic calculation functions ************************/
static void __mark_edic_only_complete(struct nvmeibc_block_command *rldr, int rv)
{
	__set_reed_solo_rv_on_cmds(rldr, rv);
	// Expected single completion when double degraded parities
	BUG_ON(nvmeibc_atomic_read(&rldr->n_uncompleted_cmds) != 1);
	dp_ec_block_completion(&rldr->iocmd->comp, nvmeibc_d_iocmd_comp_tag_make());
}

// Write first command is always to the first snake
// Pre-reads for write might not be in the first snake (unaligned degraded multi snake write)
// Read first command (pre/read) is in first snake always.
static inline u64 get_snake_start_rlba(const struct operation *o, const struct multi_snake_slice_analyzer *mssa)
{
	const u64 full_snake = (mssa->slice_size * mssa->snake_size);
	const int first_snake_cmd = (mssa->n_writes) ? mssa->n_reads : 0;
	return (o->cmds[first_snake_cmd].first_rlba / full_snake) * full_snake; // Align to nearest snake start
}

static inline int copy_bio_block_into_new_page(struct scatterlist *sg, struct nps_block_iter *nbi, u8 **blk)
{
	u8 *dst;
	if (unlikely(nps_block_iter_nblocks(nbi) < 1)) {
		WARN(true, "nvmeibc bug! not enough blocks in allocated pages");
		return -ENOMEM; // Not really...
	}
	sg_set_page(sg, nps_block_iter_page(nbi), NVMEIBC_SECTOR_SIZE, nps_block_iter_offset(nbi));
	dst = sg_virt(sg);
	memcpy(dst, *blk, NVMEIBC_SECTOR_SIZE);
	*blk = dst;
	nps_block_iter_advance(nbi, 1);
	return 0;
}

static void fill_edic_only(const struct operation *o, int prev_rv)
{
	const struct multi_snake_slice_analyzer *mssa = o->mssa;
	const int snake_size = mssa->snake_size, replicas = mssa->replicas, slice_size = mssa->slice_size;
	const u64 snake_start_rlba = get_snake_start_rlba(o, mssa);
	struct nvmeibc_datapath *dp = &o->nd->dp;
	const bool copy_required = nvmeibc_operation_is_bio_copy_needed_for_write(o);
	struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(o->pages);
	int map_i, col, row, ci = 0;
	BUG_ON(!((nvmeib_block_io_op_is_write(o->op)) && (mssa->no_jour) && mssa->no_rw_p));		// Only in journal-less writes
	BUG_ON(mssa->n_reads && !(__is_bio_wrapper_for_bio(o->bios[0]->bio) && get_valid_wrapper_block_maps(o->bios[0])));	// Pre-reads allowed only for read-modify-write flow
	if (prev_rv)
		goto _out; // Skip calculation

	for_each_column_for_each_snake_while(mssa, mssa->bio_map, map_i, col, row, (ci++ < mssa->n_writes)) {
		int n_slices = hweight32(nvmeibc_mssa_get_column_map(mssa, col, true));
		u8 *blk_data = sg_virt(mssa->gf_blocks.bio[map_i]);		// Original BIO block
		u64 rlba = snake_start_rlba + ((col < slice_size) ? map_rlba_offset(col, snake_size, row, slice_size) : map_rlba_offset_parity(snake_size, row, slice_size));
		if (copy_required) {	// Copy before edic
			// Set new destination page into command sgl - copy BIO page into buffer, swap pointers
			copy_bio_block_into_new_page(mssa->gf_blocks.bio[map_i], &nbi, &blk_data);
		}
		{
			u32 edic = nvmeibc_calculate_edic_from_data_and_rlba(rlba, blk_data, dp->enable_di_debug_mode);
			int row_index_for_column = map_i;
			set_final_edic_value_from_crc_res(mssa->blocks_md[map_i], edic, (col >= slice_size));
			while (--n_slices) {
				// Get next map index use replicas
				advance_rlba_to_next_slice(row_index_for_column, snake_size, replicas);
				// Advnace rlba use slice size
				advance_rlba_to_next_slice(rlba, snake_size, slice_size);
				blk_data = sg_virt(mssa->gf_blocks.bio[row_index_for_column]);
				if (copy_required) {
					copy_bio_block_into_new_page(mssa->gf_blocks.bio[row_index_for_column], &nbi, &blk_data);
				}
				edic = nvmeibc_calculate_edic_from_data_and_rlba(rlba, blk_data, dp->enable_di_debug_mode);
				set_final_edic_value_from_crc_res(mssa->blocks_md[row_index_for_column], edic, (col >= slice_size));
			}
		}
	for_each_column_for_each_snake_while_end }
_out:
	return __mark_edic_only_complete(o->cmds, prev_rv);
}

/************************** Parity calculation functions **********************/
// Mark completion code correctly - will happen with HW offload
static void __mark_parity_complete(struct nvmeibc_block_command *rldr, int rv)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	const int replicas = mssa->replicas, slice_size = mssa->slice_size, n_parities = replicas - slice_size;
	int i;
	ulong target_bmp = mssa->target_bmp;
	__set_reed_solo_rv_on_cmds(rldr, rv);
	BUG_ON(target_bmp == 0);
	BUG_ON((int)hweight16(target_bmp) > n_parities);
	for_each_set_bit(i, &target_bmp, replicas) {
		const int ci = mssa->column_to_cmd.write[i];
		BUG_ON(ci < 0); // Must be valid
		BUG_ON(i < slice_size);
		__return_execution_to_cmd_stages(&rldr[ci], rv);		// Give completions, once last completion is given everything is freed
	}
}

// For each degraded segment that we restored we complete the command of the read (must have do not send set to skip edic check)
// For syncs we don't discern between readfail and degraded (nothing marked in the command since we already wiped the o_rv so we don't skip checking EDIC)
// We cannot complete on read-failed blocks since MD has yet to be set correctly (we will fail on versioning for example) and we don't skip checking MD
// Before MSSA we would complete the sync commands that are write, now it is explicit
static void __mark_restore_data_complete(struct nvmeibc_block_command *rldr, int rv)
{
	const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
	const int slice_size = mssa->slice_size, n_parities = mssa->replicas - slice_size;
	int i;
	const bool is_sync_op = (rldr->o->op != NVMEIB_BLOCK_IO_OP_READ);
	ulong target_bmp = mssa->target_bmp;
	__set_reed_solo_rv_on_cmds(rldr, rv);
	BUG_ON(target_bmp == 0);
	BUG_ON((int)hweight16(target_bmp) > n_parities);
	for_each_set_bit(i, &target_bmp, slice_size) {
		int ci;
		if (is_sync_op) { // Complete for write will skip EDIC check
			ci = mssa->column_to_cmd.write[i];
		} else {		  // EDIC check is skiped due to do_not_send
			ci = mssa->column_to_cmd.read[i];
			BUG_ON(rldr[ci].do_not_send == false);
		}
		BUG_ON(ci < 0); // Must be valid
		__return_execution_to_cmd_stages(&rldr[ci], rv);		// Give completions, once last completion is given everything is freed
	}
}

// Decide if specific sync knowledge can reduce repetition or all single strat
#if 0
static void apply_gf_calculation_for_sync_operation(struct recovery_sync_op *so)
{

}
#endif

struct t_strat_changer {
	int cur_i;					// Index of current strategy
	int change_row;				// cur_i will ++ when reaching this row.
	union {
		enum slice_strategy strat;  // Current strategy
		s8 n_parities;				// Number of blocks to restore
	};
	bool is_single_strat;		// No changes are needed
};

// Used to calculate parities and crc for writes/restores,  Can be separated into update/complement, when single strategy is used we can reduce the looping
u32 apply_gf_calculation_for_operation(const struct operation *o, u32 dgrd_sgmnts_bmp, int prev_rv)
{
	const struct multi_snake_slice_analyzer *mssa = o->mssa;
	const bool debug_di = o->nd->dp.enable_di_debug_mode;
	int column, row, map_index;
	u32 rv = 0;
	const bool is_sync_op = (!nvmeib_block_io_op_is_write(o->op));
	const bool is_scrub = (o->op == NVMEIB_BLOCK_IO_OP_RECOVER_SCRUBBING);
	const int snake_size = mssa->snake_size, slice_size = mssa->slice_size, replicas = mssa->replicas;
	struct t_strat_changer s = {.cur_i = 0, .change_row = mssa->slice_change_strategy[0], .strat = mssa->strategies[0], .is_single_strat = mssa->single_strat};
	const u64 snake_start_rlba = get_snake_start_rlba(o,mssa);
	u64 preread_512_maps = (__is_bio_wrapper_for_bio(o->bios[0]->bio)) ? get_valid_wrapper_block_maps(o->bios[0]) : 0;
	int calc_rv = 0;

	if (mssa->no_rw_p) {
		fill_edic_only(o, prev_rv);
	} else {
		const bool copy_required = nvmeibc_operation_is_bio_copy_needed_for_write(o);
		struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(o->pages);
		if (copy_required) {	// We use pages for parites and pre-reads, the remainder should be nlbas and available
			BUG_ON(o->pages.nused_blks == 0);
			nps_block_iter_advance(&nbi, o->pages.nused_blks);
		}
		for (row = 0; row < mssa->map_size / replicas; row++) {
			u32 i;
			unsigned char *vec[N_MAX_RAID_SLICE_LEN] = {0};
			unsigned char *new_vec[N_MAX_RAID_SLICE_LEN] = {0};
			u32 *crc[N_MAX_RAID_SLICE_LEN] = {0};
			u32 _crc[N_MAX_RAID_SLICE_LEN] = {0};
			u32 in_bm, out_bm, crc_bm = 0;
			const u64 slice_start_rlba = snake_start_rlba + map_rlba_offset_parity(snake_size, row, slice_size);
			unsigned long set_crc = 0;
			unsigned long row_map = nvmeibc_mssa_get_row_map(mssa, row, true);
			if (!s.is_single_strat && (s.change_row == row)) {			// Change strategy, if needed
				s.cur_i++;
				s.change_row = mssa->slice_change_strategy[s.cur_i];
				s.strat = mssa->strategies[s.cur_i];
			}
			if (!is_sync_op && row_map == 0) {
				continue;
			}
			switch (s.strat) {
			case UPDATE_STRATEGY:{
				unsigned char *old_vec[N_MAX_RAID_SLICE_LEN] = {0};
				u32 old_bm = row_map;
				for_each_set_bit(column, &row_map, replicas) { // Includes parities
					map_index = mssa_map_index();
					old_vec[column] = sg_virt(mssa->gf_blocks.prd[map_index]);
					vec[column] = sg_virt(mssa->gf_blocks.bio[map_index]);
					crc[column] = &_crc[column];
					BUG_ON(old_vec[column] == NULL);
					BUG_ON(vec[column] == NULL);
					if (copy_required && (column < slice_size)) {	// Override current command bio page with allocated one pass to update
						sg_set_page(mssa->gf_blocks.bio[map_index], nps_block_iter_page(&nbi), NVMEIBC_SECTOR_SIZE, nps_block_iter_offset(&nbi));
						nps_block_iter_advance(&nbi, 1);
						new_vec[column] = sg_virt(mssa->gf_blocks.bio[map_index]);
					}
				}
				for (column = slice_size; column < replicas; column++) {
					if (likely(old_bm & (1 << column))) {
					} else { // Update strategy when one parity is not writable, need to add a dummy buffer set in it's pre-read
						map_index = mssa_map_index();
						old_bm |= (1 << column);
						old_vec[column] = vec[column] = sg_virt(mssa->gf_blocks.prd[map_index]);	// Same buffer for input and output. We will ruin the information of parity in pre-read. Who cares
						BUG_ON(vec[column] == NULL);
					}
				}
				crc_bm = row_map;
				if (nvmeibc_reed_solomon_update_parities(replicas, slice_size, snake_size, old_vec, vec, new_vec, &crc[0], old_bm, crc_bm, slice_start_rlba, debug_di))
					calc_rv = -EIO;
				// Set edic into MD
				break;
			}
			case RESTORE_STRATEGY:
				out_bm = dgrd_sgmnts_bmp;
				in_bm = ((1 << replicas) - 1)^out_bm;
				for (i = 1, column = 0; column < replicas; column++, i <<= 1) {
					const int gf_index = mssa_map_index(); // Read index All old available D+P
					vec[column] = get_mssa_page(mssa->gf_blocks.prd[gf_index]); // Already set to buff or NULL
				}
				if (nvmeibc_reed_solomon_fill_missing(replicas, slice_size, snake_size, vec, NULL, NULL, in_bm, out_bm, crc_bm, slice_start_rlba, debug_di))
					calc_rv = -EIO;
				// If used for recovery store CRC now - currently recovery doesn't call with RESTORE_STRATEGY
				// Check if 512B Write requires Read-Modify-Write for a restored block in this slice
				if (unlikely(preread_512_maps)) { // 512B operation
					if (__extract_rmw_first_block_map(preread_512_maps)) {	// First block is a subblock op
						map_index = find_first_bit(mssa->bio_map, mssa->map_size);
						if (mssa_map_index_to_row(map_index, snake_size, replicas) == row) {	// The block is in this row (slice)
							__copy_sub_block_by_map(sg_virt(mssa->gf_blocks.bio[map_index]), sg_virt(mssa->gf_blocks.prd[map_index]), __extract_rmw_first_block_map(preread_512_maps));
							preread_512_maps &= ((1UL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR) - 1) << KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
						}
					}
					if (__extract_rmw_last_block_map(preread_512_maps)) {	// Last block is a subblock op
						map_index = find_last_bit(mssa->bio_map, (mssa->map_size - ((replicas-slice_size)*snake_size)));
						if (mssa_map_index_to_row(map_index, snake_size, replicas) == row) {	// The block is in this row (slice)
							__copy_sub_block_by_map(sg_virt(mssa->gf_blocks.bio[map_index]), sg_virt(mssa->gf_blocks.prd[map_index]), __extract_rmw_last_block_map(preread_512_maps));
							preread_512_maps &= (1UL << KERNEL_SECTORS_IN_NVMEIBC_SECTOR) - 1;
						}
					}
				}
				if (1) { // Prepare full slice vector from pre-reads/restored data or new bio
					out_bm = 0;
					for (i = 1, column = 0; column < replicas; column++, i <<= 1) {
						const int gf_index = mssa_map_index(); // Prepare full slice from old and new
						if (test_bit(gf_index, mssa->bio_map)) { // Use write index
							crc_bm |= i;
							crc[column] = &_crc[column];
							vec[column] = sg_virt(mssa->gf_blocks.bio[gf_index]);
							BUG_ON(vec[column] == NULL);
							if (column < slice_size) {
							} else {
								out_bm |= i;
							}
							if (copy_required && (column < slice_size)) {
								sg_set_page(mssa->gf_blocks.bio[gf_index], nps_block_iter_page(&nbi), NVMEIBC_SECTOR_SIZE, nps_block_iter_offset(&nbi));
								nps_block_iter_advance(&nbi, 1);
								new_vec[column] = sg_virt(mssa->gf_blocks.bio[gf_index]);
							}
						} else { // Restored data from previous step or read from preread
							vec[column] = get_mssa_page(mssa->gf_blocks.prd[gf_index]);
							if (column >= slice_size && vec[column] != NULL) { // Writable parity must be part of bio map
								out_bm |= i;
								BUG();
							}
						}
					}
				} else {
				FALLTHRU; // full slice can be used now skip it's init
			case COMPLEMENT_STRATEGY:
					out_bm = 0;
					for (i = 1, column = 0; column < replicas; column++, i <<= 1) {
						const int gf_index = mssa_map_index(); // Prepare full slice
						vec[column] = get_mssa_page(mssa->gf_blocks.bio[gf_index]); // Already set to buff or NULL (Ps can be NULL)
						if (test_bit(gf_index, mssa->bio_map)) {
							crc_bm |= i;
							crc[column] = &_crc[column];
						}
						if (column < slice_size) { // Parity can be NULL if dead and not required
							if (vec[column] == NULL) { // Might be a pre-read block in prd map
								BUG_ON(mssa->gf_blocks.prd == NULL);
								vec[column] = sg_virt(mssa->gf_blocks.prd[gf_index]);
							} else if (copy_required && (row_map & i)) { // Skip blocks we pre-read but are not part of BIO
								sg_set_page(mssa->gf_blocks.bio[gf_index], nps_block_iter_page(&nbi), NVMEIBC_SECTOR_SIZE, nps_block_iter_offset(&nbi));
								nps_block_iter_advance(&nbi, 1);
								new_vec[column] = sg_virt(mssa->gf_blocks.bio[gf_index]);
							}
							BUG_ON(vec[column] == NULL);
						} else {
							if (vec[column] != NULL) {
								out_bm |= i;
							} else BUG_ON(!nvmeib_block_io_op_is_write(o->op));
						}
					}
				}

				in_bm = (1 << slice_size) - 1;
				if (nvmeibc_reed_solomon_fill_missing(replicas, slice_size, snake_size, vec, new_vec, &crc[0], in_bm, out_bm, crc_bm, slice_start_rlba, debug_di))
					calc_rv = -EIO;
				// Store CRC into MD

				break;
			default:
				BUG();
			}	// End of switch
			if (is_scrub) {	// Only check Parities EDIC
				set_crc = nvmeibc_raid1_get_parities_bmp(o->rso->r1);
			} else {
				set_crc = crc_bm;
			}
			for_each_set_bit(column, &set_crc, replicas) {
				const int gf_index = mssa_map_index();
				if (is_scrub) { // Scrub -> Calculate full slice Parities, and compare EDIC values (for parities only, EDIC was checked for each data block when read), if calc EDIC differs from read EDIC we assume P/Q are invalid and re-write them
					BUG_ON(column < slice_size);
					BUG_ON(dgrd_sgmnts_bmp);
					BUG_ON(!is_sync_op);
					BUG_ON(s.strat != COMPLEMENT_STRATEGY);
					BUG_ON(!s.is_single_strat);
					BUG_ON(s.change_row);
					if (scrub_compare_edic_with_crc(mssa->blocks_md[gf_index], *crc[column]))
						rv |= (1u << row);
				}
				set_final_edic_value_from_crc_res(mssa->blocks_md[gf_index], *crc[column], (column >= slice_size));
			}
		}
		if (copy_required) WARN(!nps_block_iter_empty(&nbi), "nvmeibc bug! unused extra blocks\n");
		__mark_parity_complete(o->cmds, calc_rv? calc_rv : prev_rv);
	}
	return rv;
}

/************************** Data restoration functions ************************/
// for each slice in mssa, if parity is read for that slice we will restore all data from the dgrd_segment_bm
// reads will set NULL for unrequired data or unavailable parity
// Sync will not have any NULL pointers so we rebuild all data from non-dgrd_segment_bmp parities
void restore_degraded_data_for_read(const struct operation *o, u32 dgrd_segment_bmp, int prev_rv, const bool is_crc_required) {
	const bool di_debug_mode = o->nd->dp.enable_di_debug_mode;
	const bool is_sync_op = (o->op != NVMEIB_BLOCK_IO_OP_READ);
	int calc_rv = 0;
	BUG_ON(is_sync_op && !is_crc_required);
	if (prev_rv) goto _out;

	{
		int column, row;
		struct multi_snake_slice_analyzer *mssa = o->mssa;
		const int snake_size = mssa->snake_size, slice_size = mssa->slice_size, replicas = mssa->replicas;
		const u64 snake_start_rlba = get_snake_start_rlba(o, mssa);
		struct t_strat_changer s = {.cur_i = 0, .change_row = mssa->slice_change_strategy[0], .n_parities = mssa->n_parities[0], .is_single_strat = mssa->single_strat};
		for (row = 0; row < mssa->map_size / replicas; row++) {
			u32 curr_index = 1;
			unsigned char *vec[N_MAX_RAID_SLICE_LEN] = {0};
			u32 *crc[N_MAX_RAID_SLICE_LEN] = {0};
			u32 _crc[N_MAX_RAID_SLICE_LEN] = {0};
			u32 out_bm = dgrd_segment_bmp & ((1 << slice_size) - 1); // Only data is required, maybe not all of it
			u32 in_bm = (~dgrd_segment_bmp) & ((1 << slice_size) - 1); // All available data we add parities later
			u32 crc_bm = (is_crc_required) ? out_bm : 0;
			const u64 slice_start_rlba = snake_start_rlba + map_rlba_offset_parity(snake_size, row, slice_size);
			unsigned long set_crc = crc_bm;
			if (!is_sync_op && (!nvmeibc_mssa_get_row_map(mssa, row, false))) {
				// Skip until relevent slice (for sync all slices are relevent and we don't set pre-read map with bio)
				// For reads we only set pre-read row map bits if we need a pre-read for that slice
				continue;
			}
			if (!s.is_single_strat && (s.change_row == row)) {			// Change strategy, if needed
				s.cur_i++;
				s.change_row = mssa->slice_change_strategy[s.cur_i];
				s.n_parities = mssa->n_parities[s.cur_i];
			}
			BUG_ON(s.n_parities == 0);
			for (curr_index = 1, column = 0; column < replicas; column++, curr_index <<= 1) {
				const int gf_index = mssa_map_index(); // Set all available D+P
				vec[column] = get_mssa_page(mssa->gf_blocks.bio[gf_index]);
				if (column >= slice_size) {
					if (vec[column] != NULL && !(dgrd_segment_bmp & curr_index)) { // We read the parity
						in_bm |= curr_index;
					}
				} else {
					if (vec[column] == NULL) { // block that is not required remove from out_bm and crc_bm
						BUG_ON(is_crc_required); // Only sync requires CRC (only sync writes it to disk)
						if (is_crc_required) {
							crc_bm &= ~curr_index;
						}
						out_bm &= ~curr_index;
						BUG_ON(in_bm & curr_index); // Must not be input
					}
				}
				if (curr_index & crc_bm) {
					crc[column] = &_crc[column];
				}
			}
			if (nvmeibc_reed_solomon_fill_missing(replicas, slice_size, snake_size, vec, NULL, crc, in_bm, out_bm, crc_bm, slice_start_rlba, di_debug_mode))
				calc_rv = -EIO;
			if (is_crc_required) {
				for_each_set_bit(column, &set_crc, replicas) { // Data restoration for sync
					const int gf_index = mssa_map_index(); // Metadata index in 2D array
					set_final_edic_value_from_crc_res(mssa->blocks_md[gf_index], _crc[column], !(column < slice_size));
				}
			}
		}
	}
_out:
	__mark_restore_data_complete(o->cmds, calc_rv? calc_rv : prev_rv);
}
