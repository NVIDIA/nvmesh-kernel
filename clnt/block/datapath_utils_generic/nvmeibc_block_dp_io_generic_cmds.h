/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_IO_GENERIC_CMDS_H
#define NVMEIBC_DP_IO_GENERIC_CMDS_H
/* Methods of generic IO commands. Each command is read/write/trim to a segment
   on disk. It uses transport layer command and request structures with sg list.
   IO to protection raid includes a set of commands.
   Array of commands (possible concatenated commands of few raids/chunks are   .
   used by operation. */
#include "../../nvmeibc_block.h"		/* external API of the block */
#include "../datapath_ec/nvmeibc_block_dp_ec_rldr_info.h" /* Todo: obscure rldr data/info */
#include "nvmeibc_block_dp_io_generic_stages.h"
#include "nvmeibc_block_dp_buffers.h"


static inline bool e_cmds_stage_is_rdma_appendix(const enum e_cmds_stage stage){
	return (stage == E_CMDS_STAGE_POST_JR_RDMA) || (stage == E_CMDS_STAGE_POST_IO_RDMA);
}

static inline bool e_cmds_stage_is_reed_solomon(const enum e_cmds_stage stage){
	return (stage == E_CMDS_STAGE_CALC_DEG_DATA) || (stage == E_CMDS_STAGE_CALC_PARITIES);
}

/* TODO:
 * Split the command into a leader cmd that holds the per BIO info and
 * an array of the actual per disk element data that should be much smaller.
 * Each operation translates into one or more disk accesses or "commands"
 * (nvmeibc_block_command) and perhaps one or more lock accesses
 * (nvmeibc_cmd_lock). The commands and locks are stored in an array per
 * operation. The first entry of the array (named leader) is special.
 * It has information (BIO) that is not copied to the rest of array elements
 * (saving mallocs). All commands hold a reference to the leader and all locks
 * hold a reference to the first lock. This makes working on them in callbacks
 * easier. The first command also has a reference to the locks and vice versa.
 * TODO: It may make sense to move all the specific data held only in the first
 * lock and first command into the operation structure:
 * cmds have 3 modes of operation:
   1. Single state execution plan. Used in Jbod.
      Operation has atomic counter of sub state machines in o->n_uncomp_raids,
	  which is initialized to 1. cmds[i].use_stages is false
      All cmds are executed in parallel, leader (cmd[0]) counts them atomically
      in n_uncompleted_cmds and when all finish it decs o->n_uncomp_raids
      (which triggers kfree(o) and also frees the command. Optimization:
      o->n_uncomp_raids always goes from 1 to 0 so it can be optimized out.

   2. Single state execution plan, for Raid1-Trims, Vio-lock/unlock ops.
      Almost identical to above
      This scheme is used for Mirrored trim because trim do not write dirty
      bits so it always have only one stage. Also its commands are split
      to sub commands and that makes tracking of sibling commands and stages
      very complicated.
      A slight difference from the first execution mode: trim command can be
	  split and freed before operation completes and that is prone to races. So
      cmds[0].n_uncompleted_cmds gets additional ref count, which

   3. Multi-stage per raid execution plan. Used in erasure codding R5, R6 which
      typically have many stages. Raid-1 has typically 1 stage + rare additional
	  stage of turn-off dirty bits:
      o->atomic counter is initialized to the amount of raids on which bio is
      spanned. In raid 10/50/60 the amount of raids can be large.
	  cmds[i].use_stages is true
      Commands are grouped in per-raid groups an each the first command in
      raid is the raid leader (functions as the cmd[0] is the previous case).
      It uses same field 'n_uncompleted_cmds' to control the execution plan
      of the raid cmds: when this atomic drops to zero raid leader does
      raid_cur_stage++, and starts the next stage (i.e. executing all raid
	  commands which belong to this stage). When raid_last_stage is
      reached raid leader decs o->n_uncomp_raids.

   Clarification: Case 1,2 is a special case of 3 where all the commands are
      treated as if they are in a single raid, all belonging to first the stage,
      raid_cur_stage == raid_last_stage == 0.
      leader uses 'ncmds' as raid leader uses 'nraid_siblings' to control loop
      over all its commands. 'my_leader' of each cmd is 0
   */
struct nvmeibc_block_command {					// IO Command to disk to R/W/T data or metadata or command to Server to take action.
	struct operation *o;                        // Reference back to the operation that lead to this command (internal structures)
    struct nvmeibc_disk_segment *ds;            // Every command must be on a single segment. When we write to raid (mirrored) each IO will generate few commands
	struct nvmeibc_disk_io_command *iocmd;		// Transport layer IO command
	struct nvmeibc_disk_gen_cmd    *gen_cmd;	// Transport layer instruction to server command, Typically used iocmd or gen_cmd
	u64 nlbas;                                  // Todo: Remove, use sg info! Number of logical block addresses in the current command (in units of blocks)
	u64 first_rlba;								// The first RLBA this command repristents used as Salt for CRC calculations
	struct nvmeibc_block_command *cmdarr;       // Pointer to the first command of the operation (first element in array of commands with length = ncmds)
	struct nvmeibc_cmd_lock    *locksets;       // Array of locks ('nlocks' of which are used by this command). In C++ facion: this->locksets>cmds == this->cmdarr
	unsigned long jam_alloc_jif;
	nvmeibc_atomic_t nlocks;                    // Number of locks needed this command. Always >= 1 for non JBOD protection raid
	s16 ncmds;                                  // Total amount of commands in this operation: cmdarr[0]..cmdarr[ncmds-1]
	size_t mem_allocated_size;                  // set for the first lock command for non placement allocation, used by memmgr metrics on-free accounting
	s16 nlocks_take_before_cmd;                 // == nlocks unless special read ops where locks are not taken, but just verified. Also used as assist variable for calculating 'nlocks' in a loop (Avoid expensive atomic operations).
	int o_rv;                                   // Return value of the operation. Negative is error. 0 is OK
	struct /* Fields of raid leader */ { 		// Leader of sibling commands in protection raid (commands affecting same slice). Support (per-raid) multi stage state machine of commands
		struct nvmeibc_raid_leader_cmd_ctx rld;	// Data of raid leader. Todo: move the stuff below into it
		nvmeibc_atomic_t n_uncompleted_cmds;    // Amount of commands remaining in the current raid stage.
		enum e_cmds_stage raid_first_stage: 4;	// Execution plan of this raid starts from this stage.
		enum e_cmds_stage raid_cur_stage  : 4;	// Current stage in the execution plan.
		enum e_cmds_stage raid_last_stage : 4;	// Terminal stage in execution plan. Upon reaching it, notify the operation that this raid terminated and possibly release some locks
		bool use_stages                   : 1;	// Do we use multistage commands execution (raid5, raid1) or single stage (jbod)
		bool use_jr_apend_stage_data_lock : 1;	// Does the execution plan of this raid involves journal appendix stages (stage which writes TxID to a data lock)
		bool use_io_apend_stages          : 1;	// Does the execution plan of this raid involves data    appendix stages: View read lock / stage which sends dirty bits and not commands
		bool use_io_apend_stages_data_lock: 1;	// Bool: Relevant Only if use_io_apend_stages is true. If true - write blockset-info to data lock
		bool use_io_apend_stages_dbit_off : 1;  // Bool: Relevant Only if use_io_apend_stages is true. If true - Send dirty bit turn off as a separate stage
		bool use_read_fail_fix_blockset   : 1;	// Does the execution plan of this raid involves fixup of a bad sector, ecnountered by read command (pre read for R/W-IO or READ-IO)
		bool should_check_view_lock       : 1;	// Flag which marks that lock view was explicitly launched (not done as piggyback)
		bool all_cmds_sm_done             : 1;	// Becomes true when cmds state machine is ready to transition to unlock state machine
		u32  was_journ_success            : 1;	// Bool: Write-IO, true if journal write was successfull or was not used at all (double_dead-parities == true), if at least 1 IO succeeds, and transaction aborts, must abandon journal+locks coz sync will roll-forward
		u32  was_transaction_abandoned    : 1;	// Bool: was_journ_success == true, but write of IO failed. Trasnaction (journal+locks) should be abandoned to allow 'sync' to roll the transaction forward
		u32  is_roll_fwd_guaranteed       : 1;	// Bool: If transaction was abandoned == true, check whether roll-fwd-guaranteed
		u32  was_abandoned_jr_recovered   : 1;  // Bool: Colliding abandoned jentries were encountered and successfully freed - block on JAM allocation until it gets A2F from Serjio
		u32  nraid_siblings               : 8;	// Amount of commands which represent transaction to a single blockset (In R1: serires of Wr/Re/Trim, in EC Reads+Writes+Journals....)
	};
	struct /* Stage info of each command*/ { 	// Support (per-raid) multi stage state machine of commands
		enum e_cmds_stage my_stage        : 4;	// When the IO raid state machine reaches this stage - this command should be executed
		bool is_not_ndb_owner             : 1;	// Erasure coding only. Journal cmds share blocks with the data commands so they don't even have sglist
		bool do_not_send                  : 1;	// Command which is not send to remote disk but. Degraded read - data is calculated from parity. In degraded write, cmd might not be sent to disk but is needed for calculation of parity.
		bool is_parity                    : 1;	// Is this command R/W parity (crucial coz, parity metadata is different from data md).
		bool do_not_realease_md			  : 1;  // When sync ops share read/write MD we cannot free both
		bool is_valid_for_reuse			  : 1; 	// After reading blocks from drive correctly (EDIC checked if possible), re/calculating blocks marked for reuse, especially for syncs
		bool used_placement_alloc         : 1;	// cmds array was never allocated explicitly but trails on another allocation (locks)
		bool used_placement_alloc_sbis    : 1;	// sbis array was never allocated explicitly but trails on another allocation (locks or cmds)
		bool used_placement_sgl           : 1;	// SGLs allocated with cmds
		u32  reserved2                    : 4;
		u32  my_leader                    : 16; // Points to raid leader command: cmd->cmdarr[cmd->my_leader] is the leader command of raid.
	};
};

#define nvmeibc_block_command_get_sgl(cmd) (*(cmd)->iocmd->reqs1.ndb->table.sgl)
/* Allocates and returns a buffer typically used for an SGL  */
struct nvmeib_data_buffer *nvmeib_get_ndb(struct nvmeibc_block_command *cmd, int nentries, gfp_t gfp);
void                     __nvmeib_put_ndb(struct nvmeib_data_buffer *ndb);	// EXC-3361 should be static and used via __nvmeibc_disk_io_command_free(). Hack of HTR
int nvmeib_make_discard_ndb(struct nvmeibc_block_command *cmd);

enum nvmeib_dsm_range_encoding {
	NVMEIB_DSM_RANGE_ENCODING_NATIVE,
	NVMEIB_DSM_RANGE_ENCODING_LITTLE_ENDIAN,
};

struct nvmeib_dsm_range nvmeib_get_ndb_discard_range(struct nvmeibc_block_command const* cmd, enum nvmeib_dsm_range_encoding encoding);

/* Does this command require journal manager actions */
bool dp_cmds_does_require_jam(const struct nvmeibc_block_command *cmd);

static inline struct nvmeibc_block_command * dp_cmds_get_cmd_from_comp(const struct nvmeibc_d_iocmd_comp *comp)
{
	return comp->cmd;	// Todo use: container_of(comp, struct nvmeibc_disk_io_command, comp)->disk_cmd.owner
}

/************************* API of array of commands ***************************/
/* Alloc array of commands, use dp_cmds_free_all() to free it*/
struct nvmeibc_block_command *dp_cmds_kvzalloc(     u32 n_cmds);									// Allocate array
struct nvmeibc_block_command *dp_cmds_placement_new(u32 ncmds, void* locks_suffix, bool has_sgls);	// Use placement allocation after array of locks
size_t                        dp_cmds_calc_size(    u32 ncmds);										// Calculate size needed for this amount of cmds
void dp_cmds_free_all(  struct nvmeibc_block_command *cmds);
void dp_cmds_free_split(struct nvmeibc_block_command *cmds);	// After TRIM split, free previous unneeded commands

/* PET for sync commands: after a parallel disk batch finishes; PET read response+ndb per leg, write response only. */
void nvmeibc_sync_cmd_response_pet_describe(struct nvmeibc_block_command *trigger_cmd);
/* PET for disk IO commands: after a command finishes; PET response+content. */
void nvmeibc_cmd_disk_io_complete_response_pet_describe(struct operation *o, struct nvmeibc_block_command *cmd);

u64  dp_cmds_req_alloc_unique_id(void);

/* Fill request to transport layer of n'th command in the array*/
void dp_cmds_req_fill(struct operation* o, int n, struct nvmeibc_disk_segment *ds);

/* Try to execute a single cmds[n] or force execute it directly (use only when
   you are sure that it is safe). If you have multi-stage cmds then execute only
   the commands of the first stage and they will trigger the rest*/
int dp_cmds_tryexec_cmd(struct nvmeibc_block_command *cmds, int n, const int error_on_no_execution);
int dp_cmds_execute_cmd(struct nvmeibc_block_command *cmds, int n);

/* After command cmds[n] was done (sent, aborted, failed, etc..) complete it
   and allow transition to next stage / state machine */
void dp_cmds_complete_cmd(struct nvmeibc_block_command *cmds, int n, struct nvmeibc_block_command *this_cmd /* DEBUG_TRANSFERS */);

/* Did the command fail, but not corrupted the slice? */
#define dp_cmds_rv_failed_ACID(cmd) (cmd->o_rv == -EAGAIN || cmd->o_rv == -ENXIO || cmd->o_rv == -ENOMEM) /* Not sent*/
#define dp_cmds_rv_failed_non_ACID(cmd) (cmd->o_rv == -EIO)   /* Slice might be corrupted */
static inline bool dp_cmds_was_cmd_not_sent(int o_rv)		/* Judge by rv of the command if it was sent or failed locally*/
{
	return ((o_rv == -EAGAIN) /* Could not reach transport layer */ ||
			  (o_rv == -ENXIO) || (o_rv == -ENOMEM) /* Could/should not send */ ||
			  (o_rv == -ENOEXEC)/* Could/should not sent AND dont retry */);
}

#define __cmd_set_comp_err(c, err) ({ (c)->iocmd->comp.comp_code = (c)->o_rv = (err);})
#define __cmd_clean_comp_val(c) ({__cmd_set_comp_err(c, 0);})

/************************ Multi-stage execution plan **************************/
/* After blockset acquissition state machine finished, notify raid-leader to start his first stage.
   locks_rv is the 'rv' of locks state machine. If != 0, locks were broken and commands will auto-fail */
void dp_cmds_execute_first_stage(        struct nvmeibc_block_command *cmds, int ci,   int locks_rv);
void dp_cmds_execute_stageless_trim_cmds(struct nvmeibc_block_command *cmds, int ow_i, int locks_rv);	// Stageless trim commands start here

/* When cmds[ci] raid leader finished current stage, those methods advance to next stage execution. */
void dp_cmds_done_stage_overcome_failure(struct nvmeibc_block_command *cmds, int ci);
int  dp_cmds_prev_stage_analyze_rv(      struct nvmeibc_block_command *cmds, int ci);
void dp_cmds_prepare_next_stage(         struct nvmeibc_block_command *cmds, int ci, int *last, int *ncmds, /* cmds of the todo-stage */ int *prev_stage_rv);
void dp_cmds_next_stage_execute(         struct nvmeibc_block_command *cmds, int ci);

/* Fiber, single stack execution state machine */
void dp_cmds_fiber_execute_1_blockset_state_machine(struct operation *o, const bool acquired_locks);

/********************** API of raid leaders commands **************************/
#define rv_storage_of_binfo_write(rldr)  ((rldr)->o_rv)			// RDMA appendix/Reed-solomon calculations commands preserve their return values in the rldr. It could be done, since the next stage will not be executed (I don't mean fast forward on error). if the previous stage finished with "hard" error
#define rv_storage_of_sync_error( rldr)  ((rldr)->o_rv)			// Same as above, but for when IO calls blockset syncs
/* Find first command of requeste stage of raid leader. Returning its index,
   -1 if no command of that stage exists */
static inline int dp_cmds_get_first_cmd_of_stage(const struct nvmeibc_block_command *rldr, enum e_cmds_stage stage)
{
	int	i, n_siblings = rldr->nraid_siblings;
	for (i=0; i< n_siblings; i++) {
		if (rldr[i].my_stage == stage)
			return i;
	}
	return -1;
}

/* Get number of commands in the stage pointed by the first cmd in stage */
// TODO(EBA): 'stage' argument isnt required now but when we have the per-stage command {count , index} attached to the operation, that will be more powerfull.
static inline int
dp_cmds_get_stage_count(const struct nvmeibc_block_command *first_cmds,
						int n_cmds, enum e_cmds_stage stage)
{
	int	i;
	BUG_ON(first_cmds->my_stage != stage);
	for (i=0; (i< n_cmds) && (first_cmds[i].my_stage == stage); i++);
	return i;
}

#define dp_cmds_get_next_raid_leader(rldr) 	(((rldr)->use_stages) ? (rldr)->nraid_siblings : 1) // returns the number of cmds to skip from given raid leader to the next. when using stages (EC, Mirror Rd/Wr) we'll skip non leaders (most cmds) when not using stages (TRIM cmds), we consider all cmds as leaders.

// retuns the raid leader cmd of the given cmd.
static inline struct nvmeibc_block_command *
dp_cmd_get_raid_leader(struct nvmeibc_block_command *cmd)
{
	return &cmd->cmdarr[cmd->my_leader];
}

static inline bool dp_cmd_is_raid_leader(const struct nvmeibc_block_command *cmd)
{
	return (cmd == dp_cmd_get_raid_leader((void*)cmd));
}

/* Find IO cmd which is described by the given journal command, or itself cmd */
struct nvmeibc_block_command *dp_cmd_jour_to_data(struct nvmeibc_block_command *);

// set write journal cmds to share ndb of DATA
void dp_rldr_set_wr_journal_ndb_from_data(struct nvmeibc_block_command *rldr, const int n_jcmds, const int journal_start);
void dp_rldr_set_wr_journal_cookies_to_data(struct nvmeibc_block_command *rldr);

/*************** Piggybacking of RDMA operations on data commands *************/
struct nvmeibc_dbits_tx;
void dp_cmds_piggyback_info_on_data_write(struct nvmeibc_block_command *cmd, union nvmeib_blkset_info v);
void dp_cmds_piggyback_info_on_journal_write(struct nvmeibc_block_command *jcmd, struct nvmeibc_block_command *dcmd, union nvmeib_blkset_info v);
void dp_cmds_fake_piggyback_info_on_data_write(struct nvmeibc_block_command *cmd, union nvmeib_blkset_info v);
void dp_cmds_add_readlock_to_rldr(   struct nvmeibc_block_command *rldr);

#define dp_cmds_get_piggyback_val(pcmd) (dp_cmds_get_pigbck_comp_dc((pcmd)->iocmd)->lock.bi)

// Find a lock by segment in a lock array. Will return NULL if not found
struct nvmeibc_cmd_lock * __get_lock_by_seg(const struct nvmeibc_disk_segment *seg,
					   struct nvmeibc_cmd_lock *lock_start);
// Find lock which does not reside on parity segments. Only on slice-start (D0) segment.
struct nvmeibc_cmd_lock * __get_data_lock(struct nvmeibc_block_command *rldr);

#define should_notify_toma(code)  ((code != EPERM_READ_FAIL_NO_RETRY) && (code != EPERM_READ_FAIL) && \
								   (code != -EIO) && (code != -ENOENT) && !dp_cmds_was_cmd_not_sent(code))

/**************************** API of gen commands *****************************/
/* Add/Del gen_cmd to block command */
int  dp_cmds_gencmd_add(struct nvmeibc_block_command *cmd);
void dp_cmds_gencmd_del(struct nvmeibc_block_command *cmd);

/**************************** PET APIs  *****************************/
void nvmeibc_blkset_info_write_pet_describe(struct operation* o, u8 sgmnt, u64 addr, struct nvmeibc_d_rdma_comp *dc);


/* PET trace declarations */
void pet_trace_binfo_lock_write_err(const struct nvmeibc_block_command *rldr);

#endif  // H beginning

