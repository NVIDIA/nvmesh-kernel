#ifndef NVMEIBC_BLOCK_DP_LOCK_SERVER_H
#define NVMEIBC_BLOCK_DP_LOCK_SERVER_H
/*
 * Defines the logic of how locks are taken in protection raid
 * The lock server completely separates the location of locking from the
 * location of the IO. it takes all members of a raid-stripe & returns the
 * segments that need to be locked Owner, Secondary & (possibly multiple) Copy-Ow
 * the client takes the address of the IO, finds the segment on which the IO
 * needs to be issued & then uses the lock-server to translate the segment to
 * the locking server.
 */
#include "kr_incs.h"
#include "nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_block_dp_dbits.h"
#include "nvmeibc_block_dp_locks_scheme.h"


/******************* Single Segment Lock Info Update **************************/
#include "../toma/clnt/nvmeibt_client_protocol.h"
struct lock_ownership_map {		// Describes which locks must be taken before issuing IO to the slice of which thid segment is an owner
	enum NVMEIBTC_DS_OWNER_MODE type[N_MAX_RAID_LOCKS];
	s8                            si[N_MAX_RAID_LOCKS];	// -1: Invalid, otherwise index of segment
	int n_locks   : 8;			// For faster datapath 2 arrays above are sorted to hold {owner, secow_si, sorted copy-owners }
}  __attribute__ ((packed));
static inline void lock_ownership_map_poison(struct lock_ownership_map* lm) {
	memset(lm, 0, sizeof(*lm));
};

#define LOCK_OWNERSHIP_MAP_STRING_LEN	(24)	// including terminating NULL. Used for printing the lock map to proc files and debug
void lock_ownership_map_to_string(const struct lock_ownership_map *lm, char res[LOCK_OWNERSHIP_MAP_STRING_LEN]);

/* Given a msg from Toma, update the ownership map of the raid's segment 'me' */
struct nvmeibc_raid1;
void lock_ownership_update_seg_map(struct nvmeibc_raid1* r1, int me,
					const struct nvmeibt_client_topo_disk_segment *t_si);

/* Update ownership map due to slice length change ('u'-upgrade / 'd'-downgrade
   action on segment 'si') */
void lock_ownership_update_raid_map(const struct nvmeibc_raid1*r1, int si,
									char action);

/*********************** Datapath Locks calculations **************************/
/* Important: the order in which IO requests locks vaires between R/W/Trim so
   it cannot be described by lock server */

/* Given an IO rlba[blocks] (offset from the beginning of protection raid),
   Find the segment which is:
   1. Owner for locking (owner lock) - For Raid1(N-mirroring) / Raid5/6.
   2. Owner for reading (For: N-mirroring with N>2)
   3. Slice start - For JBOD, Raid1(2-mirroring) / Raid5/6.
   Note: For Symetric N-mirroring read-owner == lock-owner. For generic locking
   server this is not correct. Note: Owner always exists! */
int get_owner_seg_of_lock(    const struct nvmeibc_raid1 *r1, u64 rlba);
int get_owner_seg_of_read(    const struct nvmeibc_raid1 *r1, u64 rlba);
int get_owner_seg_slice_start(const struct nvmeibc_raid1 *r1, u64 rlba);

/* Given an IO rlba[blocks] (offset from the beginning of protection raid),
   calc full map of which locks to take on each segment. Returns the map + index
   of owner lock segment with owner lock. Owner always exists */
void lock_ownership_build_raid_map(const struct nvmeibc_raid1*r1, u64 rlba,
	enum nvmeib_block_io_op op, /*out*/ struct lock_ownership_map *raid_l);

sgmnts_bmp_t nvmeibc_calc_db_on_parities_segs(const struct nvmeibc_block_command *rldr);
roles_bmp_t nvmeibc_calc_db_on_parities_roles(const struct nvmeibc_block_command *rldr);

/* Check if it is possible perform an action of turning dbits off (seg with
   TURN_OFF topology + turned on dbit */
bool dp_ec_can_fix_dbits(struct nvmeibc_block_command *rldr);

#endif  // H beginning

