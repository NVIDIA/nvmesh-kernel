#ifndef NVMEIBC_DP_EC_RECOV_ALL_STATS_H
#define NVMEIBC_DP_EC_RECOV_ALL_STATS_H

/******************** Journal recoveries stats counter ************************/
struct nvmeibc_cold_stats {
	atomic_t n_syncs;			// Number of blocksets with candidates
	atomic_t n_htr_call;		// Number of times executed htr
	atomic_t n_no_htr_call;		// All candidates were worthless
	atomic_t n_jgc_blksets;		// Number of blocksets that jgc cleaned
	atomic_t n_jgc_freed;		// Number of jentries, jgc freed,		Optimization: not filled by each sync() doing++, but by recovery doing add()
	atomic_t n_resets;			// Number of times this structure was reset. Without this field one cannot know of counters are 0 because they were cleaned or nothing happened in the past
};
int  nvmeibc_cold_stats_to_string(char* buf, int len);
void nvmeibc_cold_stats_reset(void);
void nvmeibc_cold_stats_get(struct nvmeibc_cold_stats *rv);

/******************************* HTR stats counter ****************************/
struct htr_stats {
	atomic_t n_calls;   			// number of calls.
	atomic_t n_comps;				// number of completed (not necessarily successfully) calls.
	atomic_t n_comps_err;			// number of calls completed unsuccessfully.
	atomic_t n_uuid_no_jour;     	// client which left the stale locks has no jmdc(jri). Client failed without getting allocated journal or it already released the journal entries.
	atomic_t n_jour_cmtd;           // number of HTR calls which encountered journal fully committed, so it needs to continue to check if data committed.
	atomic_t n_data_cmtd;   		// number of HTR calls which encountered journal committed + data committed on at least one of the data blocks. If true than a roll-fwd or regen-fwd will be executed.
	atomic_t n_roll_fwd;			// The number of HTR calls which roll-fwd at least one block from the journal.
	atomic_t n_roll_fwd_by_dbits_turnon; 	// The number of HTR calls which roll-fwd a TX using a dbits turn-on on at least one dead block.
	atomic_t n_roll_bkw_by_dbits_turnon;  	// The number of HTR calls which roll-back a TX using a dbits turn-on on at least one dead block.
	atomic_t n_regen_fwd;					// The number of HTR calls which roll-forward a TX using restoration from the data seg non-readable but writable.
	atomic_t n_regen_bkw;                   // The number of HTR calls which roll-bkw a TX using restoration from the rest of the data segs non-readable but writable.
	atomic_t n_update_parity_dbits;         // The number of HTR calls which changed the dbits(turn-on/turn-off) in the parities of the slice. The only case when the dbits are changed in slice is due to: n_regen_bkw/n_roll_bkw_by_dbits_turnon/n_roll_fwd_by_dbits_turnon/n_regen_fwd.
	atomic_t n_send_recovered;				// The number of HTR calls which successfully sent blockset recovered on the blockset/slice making the relevant journals unallocated and notifies TOMA.
	atomic_t n_regen_bkw_no_pari;			// The number of HTR calls which regened all the paritie's mds in the blockset by calling rollback (which finished successfully). Happens when all parities are degraded(non readable).
	atomic_t n_dbits_turnon_no_pari;    	// The number of HTR calls which turned on dbits on all the paritie's mds in the blockset by calling rollback (which finished successfully). Happens when all parities are degraded(non readable).
	atomic_t n_colds; 						// The number of times HTR called by Cold reacovery.
	atomic_t n_dbits_rebuild;  				// The number of HTR calls which called no_whole state machine in order to turnoff dbits.
	atomic_t n_ext_sm;						// The number of HTR calls which called external sm.
	atomic_t n_resets;						// The number of times the stats been cleaned(reseted).
};
int  nvmeibc_htr_fill_status(char* buf, int len);
void nvmeibc_htr_stats_reset(void);
void nvmeibc_htr_stats_get(struct htr_stats *rv);

/********************* Maintanance syncs stats counter ************************/
struct nvmeibc_maintain_sync_stats {
	atomic_t n_txid_wrap;
	atomic_t n_txid_resolve;			// Resolve unknown
	atomic_t n_dbits_resolve;			// Resolve unknown
	atomic_t n_commit_binfo;
	atomic_t n_dconvict_turnon;
	atomic_t n_resets;					// Number of times this structure was reset. Without this field one cannot know of counters are 0 because they were cleaned or nothing happened in the past
};
int nvmeibc_maintain_sync_stats_to_string(char* buf, int len);
void nvmeibc_maintain_sync_stats_reset(void);
void nvmeibc_maintain_sync_stats_get(struct nvmeibc_maintain_sync_stats *rv);

#endif  // H beginning
