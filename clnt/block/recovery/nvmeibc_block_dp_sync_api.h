#ifndef NVMEIBC_SYNC_OP_EXTERNAL_API_H
#define NVMEIBC_SYNC_OP_EXTERNAL_API_H
/* API towards datapath/recoveries to launch syncs on blocksets*/

#include "../../nvmeibc_block.h"		/* external API of the block */
/* Scheme of full raid recovery and single lock sync. It has 3 API's:
    1. Decentralize unregister for stale locks resolving
    2. Sync     - Fixing another clients broken lock
    3. Recovery - Full Rebuild of protection raid

     {BIO needs lock}         {Toma requested Rebuild/Recovery}
       STALE   STALE_SAFE_TO_USE             |
          |          \-------+               |
          |                  |               |
          V                  V               V
/---- decentr-unreg.h --- sync_api.h --- raid_recovery.h -----\
|      * Ask All Tomas      |                       |         |
|           ^               |   /------------\      |         |
|           |               |   |            |      V         |
|           |               V   V           DirtyB rebuild    |
|           |             sync_common.h     Raid Stale locks  |
|           |               |               Cold, Convict, etc|
|           |               |                                 |
|           +---------------+-----> Generic Locks SM          |
|                           |        * Take, Release          |
|                           |        * Piggyback dirtyB       |
|                           |                                 |
|       +-------------------+-----> DP Virtual cmd funcs:     |
|       * Maintenace: TxID,DB        * dp_mirror_sync.c       |
|                                    * dp_ec_sync.c           |
\-------------------------------------------------------------/*/
/************************ Recoverying single blockset ************************/
/* Func prototype to be called when sync operation completes.
   err - the status of sync operation (0 - success, negative, error) */
typedef int (*nvmeibc_sync_cb_t)(void* ctx, int err);

/* All methods below:
   1. Schedule sync op on a blockset, into resubmit thread.
   2. Each sync fixes a different problem in the blockset.
   3. All methods receive a 'lock' (problematic blockset), cb above and context
      to call upon completion.
   4. All Return non zero value on error (could not schedule sync), 0 = success.
   5. If returned 0, upon async completion will call when_done_cb(context) with
      status of the sync. 0 = success, non zero is error */

/* Fix all types of problems in blockset which can be caused by a stale lock.
   Success means: commit data consistently for all slices & removes the stale lock.
   For raid1 will do 2 reads, compare and write the owners data to recovered
   segment. */
int nvmeibc_sync_fix_stale(struct nvmeibc_cmd_lock *lock,
									  nvmeibc_sync_cb_t done_cb, void* ctx);

/* Same as above but partial. Fixes only a range of slices. Thus unable to
   remove the stale lock even upon success.  Used during recovery mode
   (volume rebuild) to speed it up */
int nvmeibc_sync_fix_some_slices_in_stale(struct nvmeibc_cmd_lock *lock,
		u16 start_slice, u16 n_slices,nvmeibc_sync_cb_t done_cb, void* ctx);

/* Used only in EC. Garbage collect journal entries colliding with IO.
   Like fix stale lock, but with locks already taken. */
int nvmeibc_sync_jour_hot_gc(struct nvmeibc_cmd_lock *lock,
		nvmeibc_sync_cb_t when_done_cb, void* context);

/* Used only in R1. Unlike: nvmeibc_sync_fix_stale() does not fix any problem in
   a blockset but rather commits the stale lock to the other copy (copy lock but
   never dual lock!). Thus recovery can delay the fix of
   stale locks to later stage, (the stale lock itself is backed up in 2 rams)*/
int nvmeibc_sync_commit_stale_lock(struct nvmeibc_cmd_lock *lock,
									  nvmeibc_sync_cb_t done_cb, void* ctx);

/* Used only in R6. There is no problem with the data on disks, just missing
   backup of RAM (blockset info). Copy the blockset info from primary owner to
   all problematic copies. */
int nvmeibc_sync_commit_binfo(struct nvmeibc_cmd_lock *lock,
									  nvmeibc_sync_cb_t done_cb, void* ctx);

/* During cold recovery fixup all problems that are encountered in a blockset
   and restore the RAM of the blockset (lock-id + blockset info) */
int nvmeibc_sync_fix_cold(struct nvmeibc_cmd_lock *lock,
									nvmeibc_sync_cb_t done_cb, void* ctx);

/* Raid 6: Probe if owner lock is unlocked. If it does, then garbage collect
   the journals pointing to this blockset (they are old) */
int nvmeibc_sync_gc_unlocked_lock(struct nvmeibc_cmd_lock *lock,
									  nvmeibc_sync_cb_t done_cb, void* ctx);

/* Used only in EC. Doing txid_wrap around for a blockset. (called when a write operation sees a txid=max_possible in binfo). */
int nvmeibc_sync_txid_wrap(struct nvmeibc_cmd_lock *l,
	nvmeibc_sync_cb_t done_cb, void* ctx);

/* Scrubbing the blockset also fix other problems and resolve unkowns if exist.*/
int nvmeibc_sync_scrubbing(struct nvmeibc_cmd_lock *lock,
	nvmeibc_sync_cb_t done_cb, void* ctx);

/* Resolve unknown binfo (Txid/Dbits). Send by recovery when not holding locks, or IO when holding locks */
int nvmeibc_sync_unknown_binfo_recov(struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void* ctx);
int nvmeibc_sync_unknown_binfo_by_io(struct nvmeibc_cmd_lock *lock, nvmeibc_sync_cb_t done_cb, void* ctx);

/* Rebuild dirty bits, Analyze the blockset, find valid sources of data in
   blockset (say segments in RW mode) and restore/commit data to all other
   blocks. Executed for all slices. The dirty bit is changed to the value which
   defined by toma in current topology. Used during recovery mode
   (volume rebuild) */
int nvmeibc_sync_recover_dirty(struct nvmeibc_cmd_lock *lock,
	nvmeibc_sync_cb_t done_cb, void* ctx);

/* Overcome permanent read failure, in normal mode. Almost identical to dbits
   in sence that analyze all valid sources of data and commit to invalid, but
   here a invalid source might be seg in RW (with bad sector)
    */
int nvmeibc_sync_read_failure( struct nvmeibc_cmd_lock *lock,
				nvmeibc_sync_cb_t when_done_cb, void* ctx);

/* Turn on specila dirty convict marker prior to turning of dirtybits. Called by Recovery only */
int nvmeibc_sync_turn_on_dirty_convict(struct nvmeibc_cmd_lock *lock,
	nvmeibc_sync_cb_t done_cb, void* ctx);

/* Launch any sync from above, by 'op', caller must carefully select the correct op */
int nvmeibc_sync_generic_by_op(struct nvmeibc_cmd_lock *lock, u16 start_block, u16 n_slices,
	enum nvmeib_block_io_op op, nvmeibc_sync_cb_t when_done_cb, void* context);

/* Resubmit thread launches the next pending sync operation using this func */
int nvmeibc_sync_submit(struct nvmeibc_block_device *nd);

bool nvmeibc_sync_mem_resources_reuse_should_free_unsafe(const struct nvmeibc_block_device *nd);
void nvmeibc_sync_mem_resources_reuse_free_all(struct nvmeibc_block_device *nd);

/*****************************************************************************/
/* When Write/Trim IO operations are long (cover the entire 128K lock) they
   become, unsafe because they do sync implicitly */
bool nvmeibc_sync_is_io_implicit_sync(const struct nvmeibc_cmd_lock *lock,
									 const struct nvmeibc_block_command *c);

void block_api_sync_clear_stats(struct nvmeibc_block_device *nd);

// Sync notifies caller that it attempts to inject sgl data of the caller with latest data
void nvmeibc_mark_sync_wants_to_inject_caller_sgl(const struct nvmeibc_block_command *cmd);

/************************** Module Params *************************************/
/* A module parameter which is a multiplier of probability (factor/100).
 * This allows tuning probability of minimal vs. LOCKSET size sync.
 * usage: value == 0     => forces minimal sync
 * 		  value [1..99]  => reduces probability.
 *    	  value == 100   => no factor
 *  	  value [101.. ] => increase probability
 *        value > 100*LOCKSET_SLICES => forces full LOCKSET sync*/
extern uint nvmeibc_sync_full_lockset_probability_factor;
#define NVMEIBC_SYNC_PROB_FORCE_MIN				(0)
#define NVMEIBC_SYNC_PROB_DEFAULT  				100
#define NVMEIBC_SYNC_PROB_FORCE_LOCKSET			(100*LOCKSET_SLICES)

/* probabilistic decision whether to go for full BLKSET sync or minimal sync
 * Note: (cmd_size == BLKSET_BLKS) => always go for full sync */
static inline bool nvmeibc_sync_is_trigger_full_blkset_sync(int cmd_size)
{
	int     p = (get_random_u32() % LOCKSET_SLICES) + 1;       // random value in range [1..BLKSET_BLKS]
	cmd_size *= (nvmeibc_sync_full_lockset_probability_factor/100);
	return (p <= cmd_size);
}

#define NVMEIBC_MAX_ALLOWED_SYNC_OPS_DEFAULT	(384)											// Default max limit for sync operations per volume. Todo: function of QOS / RAM size and num cpu's
#define NVMEIBC_MAX_ALLOWED_SYNC_OPS_LIMIT		(NVMEIBC_MAX_ALLOWED_SYNC_OPS_DEFAULT*16)		// Upper limit on the max number of sync ops that can be set
extern uint nvmeibc_sync_max_operations_per_dev;
static inline uint nvmeibc_sync_get_max_dev_sync_ops(void)
{
	const uint max_sync = nvmeibc_sync_max_operations_per_dev;	// copy so we never return accidental value now being updated as we verify.
	if (unlikely(max_sync > NVMEIBC_MAX_ALLOWED_SYNC_OPS_LIMIT)) {
		nvmeibc_sync_max_operations_per_dev = NVMEIBC_MAX_ALLOWED_SYNC_OPS_LIMIT;
		return NVMEIBC_MAX_ALLOWED_SYNC_OPS_LIMIT;
	}
	return max_sync;
}

#endif // NVMEIBC_SYNC_OP_EXTERNAL_API_H
