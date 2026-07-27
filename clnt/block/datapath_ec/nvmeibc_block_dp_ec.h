#ifndef NVMEIBC_DP_EC_H
#define NVMEIBC_DP_EC_H
/* API of Erasure coded datapath Raid5/6 + optional Raid0 datapath:
   Includes Components:
    1. Internal API for ec
    2. External API (virtual functions of data path)
 */
#include "kr_incs.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_common.h"

/***************************** Internal EC API ********************************/
#include "block/nvmeibc_block_common.h"

/* Append commands of a given single lock in raid to o->cmds. Returns the total
   amount of commands. Updates nlbas and bio iterator */
int dp_ec_cmds_add_for_raid(struct operation *o,
			const struct dp_io_topo_iterator_res* it, const int first_raid_cmd);

/************************** DP virtual functions ******************************/
int  dp_ec_should_ignore_op(const struct operation *o); /* Ignore trim ops */
int  dp_ec_prepare_op(            struct operation *o);
int  dp_ec_execute_op(            struct operation *o);
void dp_ec_block_completion(struct nvmeibc_d_iocmd_comp *comp);
int  dp_ec_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int rv);
void dp_ec_exec_func_on_stage_end(struct nvmeibc_block_command *rldr, int *rv);
void dp_ec_calc_should_abandon(struct nvmeibc_block_command *cmds, int lsi);
void dp_ec_calc_comp_state(const struct nvmeibc_block_command *cmds, int *rv, bool *retry);

int dp_ec_translate_addr(struct dp_block_translation_unit *tu);


/************************ DP Sync virtual functions ***************************/
int  dp_ec_sync_prepare_op(struct recovery_sync_op *so);
void dp_ec_sync_execute_op(struct recovery_sync_op *so);	// When locks are taken
void dp_ec_sync_cmd_cb(struct nvmeibc_block_command *cmd);	// Callback on disk command / Async Raid6 calculations
void dp_ec_sync_resume_op( struct recovery_sync_op *so);	// Resume the execution of state machine after async generic cb. Called from Interrupt context, when locks are already taken. Dispatches the work to other types of syncs

/**************** Internal API: Move to different file ************************/

// MSSA and all of its functions, todo place in local where required
#define N_MAX_MULTI_SLICE_LEN (LOCKSET_SLICES) // 16? or 8? for QLC must be 32
#define N_MAX_STRATEGIES (5)				   // Unaligned snakes can have up to 5 different strategies
enum slice_strategy {
	INVALID_STRATEGY = 0, // Rename to no_action on slice
	COMPLEMENT_STRATEGY,// Also known as full slice, pre-read will only read unwritten slice blocks (can be 0), Parities are only written
	UPDATE_STRATEGY,  // pre-read map and bio_map for this slice must be the same (includes parity
	RESTORE_STRATEGY, // For read: restore required in this slice for the bio_map entires on unreadable segs, for write: an unreadable (not written to) block needs to be restored before we use COMPLEMENT_STRATEGY, all available segs are read for this slice
	NO_ACTION_STRATEGY,	// Either non-degraded read slice, or write hole slice
};

// SET NOT JOUR IN MSSA CORROECTLY AND NO_RW_P (fix no_jour to be no_rw_p where necessary)

struct multi_snake_slice_analyzer { // Bitmap of consecutive lbas (snake size for each segment) with Parity columns at the end
	int slice_size;
	int snake_size;
	int replicas;
	int owner_seg;
	int map_size;							// Amount of elements in the arrays below that are used. Always multiple of full slice
	int n_reads;
	int n_writes;
	DECLARE_BITMAP(pre_read, N_MAX_RAID_SLICE_LEN * N_MAX_MULTI_SLICE_LEN);
	DECLARE_BITMAP(bio_map,  N_MAX_RAID_SLICE_LEN * N_MAX_MULTI_SLICE_LEN);
	struct {								// 2D array of pointers into data of block - if we use update or restore strategy for write we will allocate a second map for the reads
		struct scatterlist **bio;
		struct scatterlist **prd;
	} gf_blocks;
	void **blocks_md; 						// 2D array of pointers into MD of write blocks (or EDIC location of QLC)
	struct {								// See if possible to remove some or most of these
		s8 read[N_MAX_MULTI_SLICE_LEN]; 	// If we have a split for DX we can have C(dx) = y and C(dx+1) = y+2
		s8 write[N_MAX_MULTI_SLICE_LEN]; 	// If we have a split for DX we can have C(dx) = y and C(dx+1) = y+2
		s8 jour[N_MAX_MULTI_SLICE_LEN]; 	// If we have a split for DX we can have C(dx) = y and C(dx+1) = y+2
	} column_to_cmd;
	bool single_strat;						  // Repeat all actions for all slices
	bool no_jour;							  // Used to decern between regular write and recovery (also if no journal required will albe be set)
	bool no_rw_p;							  // When no parities are writeable we don't seend journals
	union {
		enum slice_strategy strategies[N_MAX_STRATEGIES];		// If cur_tx_bm != prev_tx_bm increase strat value
		s8 n_parities[N_MAX_STRATEGIES];					 // Degraded reads: strategy is unused, I store n_parities as MD for this slice (I want to use it for optimization in restoring data), some slices need 2 parities some 1 and some none
	};
	int slice_change_strategy[N_MAX_STRATEGIES-1];			// Slice 0 is strat 1 if slice_change_strat[0] != -1 that slice we change start to strat [1] and on slice of value [1] change to strat [2]
	roles_bmp_t tx_bms[N_MAX_STRATEGIES];				    // Up to 3 different tx_bms not including parity (0 means empty slices in snake)

	struct t_mssa_1d_col_masks {					// CR temp members (remove if not worth it)
		u32 prd[N_MAX_RAID_SLICE_LEN]; 		// One for pre_read and one for bio (can be allocated instead). At most 32 slices in blockset so u32 is enough
		u32 bio[N_MAX_RAID_SLICE_LEN];
	} col_masks;

	struct t_mssa_1d_row_masks {				// CR temp members (remove if not worth it)
		u32 prd[N_MAX_MULTI_SLICE_LEN]; 		// One for pre_read and one for bio (can be allocated instead). At most 32 slices in blockset can be log2(N_MAX_RAID_SLICE_LEN)
		u32 bio[N_MAX_MULTI_SLICE_LEN];
	} row_masks;

	u16 target_bmp;								// Target bmp for GF calculations (which segments require restore, or which parites are calculated)
};

#define mssa_get_data_start_index(mssa) ((mssa)->n_reads + (((mssa)->no_jour) ? 0 : ((mssa)->n_writes / 2)))
#define mssa_get_jour_start_index(mssa) ((mssa)->n_reads)
#define mssa_get_write_count(mssa) (((mssa)->no_jour) ? (mssa)->n_writes : ((mssa)->n_writes / 2))
static inline int column_to_cmd(const struct multi_snake_slice_analyzer *mssa, const int column, const bool is_read) {
	int res = ((is_read) ? (mssa)->column_to_cmd.read[column] : (mssa)->column_to_cmd.write[column]);
	BUG_ON(res < 0);
	return res;
}
#define mssa_map_index_to_column(mi, ss, rep) ((mi / ss) % rep)
#define mssa_map_index_to_row(   mi, ss, rep) ((mi % ss) + (mi / (ss * rep)) * ss)
static inline int map_rlba_offset(const int col, const int snk_sz, const int row, const int slc_sz)
{
	if (snk_sz == 1) {
		return (row * slc_sz) + col;
	} else
		return (((((row / snk_sz) * slc_sz) + col) * snk_sz) + (row % snk_sz));
}
#define map_rlba_offset(  col, snk_sz, row, slc_sz) ((((row)/(snk_sz))*(slc_sz) + col) * (snk_sz) + (row)%(snk_sz))
#define map_rlba_offset_parity(snk_sz, row, slc_sz) (map_rlba_offset(0, snk_sz, row, slc_sz))
// Must have column/snake_size/row/replicas all defined already
#define mssa_map_index() (map_rlba_offset(column, snake_size, row, replicas))
#define nvmeibc_mssa_get_row_map(   mssa, row,    is_bio) (nvmeibc_mssa_get_row_map_or_set(   mssa, row,    is_bio, false))
#define nvmeibc_mssa_get_column_map(mssa, column, is_bio) (nvmeibc_mssa_get_column_map_or_set(mssa, column, is_bio, false))
#define nvmeibc_mssa_get_row_tx_bm(mssa, row) (GENMASK((mssa)->slice_size - 1, 0) & nvmeibc_mssa_get_row_map((mssa),row,true))
// Translate rlba to index in map without parities, then return the row of that map
#define rlba_to_row(rlba, snk_sz, slc_sz) (mssa_map_index_to_row((rlba % (LOCKSET_SLICES*slc_sz)), snk_sz, slc_sz))
// MSSA init will set the row masks if needed and __mssa_count_number_of_cmds will fill the column masks
u32         nvmeibc_mssa_get_column_map_or_set(const struct multi_snake_slice_analyzer *mssa, const int column, const bool is_bio, const bool set);
roles_bmp_t nvmeibc_mssa_get_row_map_or_set(   const struct multi_snake_slice_analyzer *mssa, const int row   , const bool is_bio, const bool set);

sgmnts_bmp_t nvmeibc_mssa_calc_write_bmp(const struct multi_snake_slice_analyzer *mssa);
sgmnts_bmp_t nvmeibc_mssa_calc_full_blockset_write_bmp(const struct multi_snake_slice_analyzer *mssa);

// In mssa times, this can variy between mid snake and edge of snake (or no snake)
#define advance_rlba_to_next_slice(rlba, snake_size, slice_size) ({\
	if (snake_size == 1) rlba += slice_size; \
	else { rlba++; if ((rlba % snake_size) == 0) rlba += (slice_size - 1) * snake_size; }\
})

// Allocated as part of the operation, needs to be set after
void dp_ec_set_allocated_mssa_into_operation(struct operation *o);

// For printing into dmesg
void nvmeibc_dump_mssa(const struct multi_snake_slice_analyzer *mssa, enum nvmeib_block_io_op op,
		       const struct nvmeibc_raid1 *pr, nvmeib_trace_level trace_level);


struct mssa_dump_vars{
	const struct multi_snake_slice_analyzer *mssa;
	enum nvmeib_block_io_op op;
	const struct nvmeibc_raid1 *pr;
};

void __dump_mssa(struct mssa_dump_vars *vars, nvmeib_trace_level trace_level);

// renamed, added while_clause to break when while_clause is false
// for each bit set in a map find the first row of a column and do an operation for the whole column
// can repeat some columns in next snake - clause is how many commands have been handled
#define for_each_column_for_each_snake_while(mssa, bitmap, map_i, col, row, while_clause) \
	for (map_i = find_first_bit((bitmap), (mssa)->map_size); \
		 ((map_i < (mssa)->map_size) && \
		 (while_clause)); \
	     map_i += ((mssa)->snake_size - ((row) % (mssa)->snake_size)), \
		 map_i = find_next_bit((bitmap), (mssa)->map_size, map_i)) { \
		col = mssa_map_index_to_column(map_i, (mssa)->snake_size, (mssa)->replicas); \
		row = mssa_map_index_to_row(   map_i, (mssa)->snake_size, (mssa)->replicas);
#define for_each_column_for_each_snake_while_end }			// Mainly for visual simplicity

void transition_from_journal_to_write_sm(struct nvmeibc_block_command *rldr, int err);
int __calc_ncmds_calc_parity(const struct nvmeibc_block_command *rldr, int *ncmds);

/**************** No Write Hole Virtual Funcs *********************************/
u32                                  dp_ec_calc_scrub_writes(           struct recovery_sync_op *so);  // Returns a bitmap of which slices are incorrect and should to be rewritten
enum NO_WRITE_HOLE_NEXT_STAGE_CHOICE dp_ec_no_write_hole_fix(           struct recovery_sync_op *so);
void                                 dp_ec_no_write_hole_destroy(       struct recovery_sync_op *so);
int                                  dp_ec_no_write_hole_get_restore_rv(struct recovery_sync_op *so);
#endif  // H beginning

