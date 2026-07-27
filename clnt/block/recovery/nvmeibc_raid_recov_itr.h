#ifndef NVMEIBC_RAID_RECOV_ITERATORS
#define NVMEIBC_RAID_RECOV_ITERATORS

#include "../../nvmeibc_block.h"		/* external API of the block */
#include "block/recovery/nvmeibc_block_dp_sync_api.h"

/********************** Iterator API for generic recovery *********************/
/* Usage: Recovery type must have 3 virtual components:
   1. Func() - Get then first/next batch of work
   2. Iterator to process the batch of work
   3. '_sync_cb' method to fix a blockset.

   Iterator is a (peoducer/consumer) data structure :
    1. Consumers: Workers which do syncs and fix blocksets
    2. Producers:
    	Request for next batch of work
		failed syncs which need retry
    3. REsponsibilites of the iterator:
		Hold multiple jobs (blocksets) to be fixed
		Returning next job to do to worker
		Receiving 'rv' for the job from worker.

  2 Iterator on entries implemented:
     1. Full   array of entries. Used for: R1/R6 Dbits
	 2. Sparse array of entries: R6-Stale locks, R6 cold/JGC */
struct recovery_itr_ctx;

struct nvmeibc_recovery_itr_job {	// The job returned by iterator (blockset to be fixed)
	u64 cookie;						// Marker of the task for error reporting. If only 1 worker exists, cookie == iterrator head
	u64 blkset_lba;					// Which blkset to fix. Like slba but units of blksets
	
	// Aggregates additional info for how to fix the blockset.
	// Can be one of the values below:
	// In case of journal recoveries (cold/jgc), it is blckset_candidates_ptr.
	// In case of stale lock recovery (only) it is stale_lock.
	// For other types of recoveries, it is blckset_problem.
	union {
		struct nvmibc_blockset_candidates *blckset_candidates_ptr; // Information held for journal recoveries (64 bit pointer)
		u32 stale_lock; // Stale lock, held for stale lock recoveries (32 bit)
		union nvmeib_blkset_problem_report blckset_problem; // Info of this blockset problem (16 bit)
	} aux;
	s32 err;						// When worker finishes processing gives rv to iterator. When iterator allocates job to worker err==0
	u64 jif_delay_before_work; /* Min delay time (jif) after which a new work is allowed to start */
};
void nvmeibc_recovery_itr_job_init(   struct nvmeibc_recovery_itr_job *j);
bool nvmeibc_recovery_itr_job_has_job(struct nvmeibc_recovery_itr_job *j);

struct nvmeibc_recovery_itr_t { /* Iterator of blocksets */
	struct recovery_itr_ctx*  ctx;			/* Iterator private state - don't touch*/
	void (*set_upper_bound)(   struct nvmeibc_recovery_itr_t* , u64 num_blksets);	/* Before iterator reinits to batch of work, set it to hold an upper bound of it, so statistics will always advance monotonically from 0..100% */
	int  (*reinit)(            struct nvmeibc_recovery_itr_t* , u64 slba_start, u64 rlba_start, u64 num_blksets, int elem_size, const u8 *data); /* Init/reinit the iterator to a batch of work (range of blocksets). Data can be bitmap or any payload, slba, rlba in units of blocksets*/
	void (*begin_cb)(          struct nvmeibc_recovery_itr_t* , struct nvmeibc_recovery_itr_job *res); /* Set iterator to the first rlba to process. Returns the job */
	void (*next)(              struct nvmeibc_recovery_itr_t* , struct nvmeibc_recovery_itr_job *res); /* ++: Get 'rv' of completed job and issue a new job (into the same ptr). Iterator moves to next blockset to recover, if prev blockset failed and it is mandatory, will set it to retry */
	void (*skip_all)(          struct nvmeibc_recovery_itr_t* ); 			  /* Allows iterator to reach terminal state without ever running. As if called begin_cb() and next() until the end */
	void (*on_batch_done)(     struct nvmeibc_recovery_itr_t* );		/* Clear prev batch info & prepare the iterator for reinit */
	void (*get_progress)(const struct nvmeibc_recovery_itr_t* , u64* done, u64* total);	// Get the progress of the iterator (Completed X out of Y blocksets). Async call
	void (*destroy)(           struct nvmeibc_recovery_itr_t* );		/* destructor */
};

/*********************** Generic iterator contexts ****************************/
#define nvmeibc_recovery_itr_t_init_null(itr) memset(itr, 0, sizeof(*(itr)))
int nvmeibc_recovery_itr_t_init_blocksets(    struct nvmeibc_recovery_itr_t *, bool is_mandatory);
int nvmeibc_recovery_itr_init_cold_candidates(struct nvmeibc_recovery_itr_t *, bool is_mandatory);
void nvmeibc_recovery_itr_t_destroy(          struct nvmeibc_recovery_itr_t *);

#endif // NVMEIBC_RAID_RECOV_ITERATORS
