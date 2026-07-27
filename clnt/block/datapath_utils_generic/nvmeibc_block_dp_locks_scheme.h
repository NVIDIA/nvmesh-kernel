#ifndef NVMEIBC_BLOCK_DP_LOCKS_SCHEME_H_
#define NVMEIBC_BLOCK_DP_LOCKS_SCHEME_H_

/**************** Lock Scheme Types (Defined by configuration) ****************/

/* Definitions: (D=No. of Data segments, L=No. of locks).
   1. There are 2 possible ways to select an owner lock (position of the first
      lock within the raid slice).
   2. Further locks (if any) have 2 options: they are placed on members with
      increasing or decreasing index & wrap around cyclically.
   3. Owner primary lock (P) is first, followed by Copy locks (A) for a total of L locks.
  Example (D=4, L=2) for enum lock_server_type_e:
    OWNER_SCHEME_FIRST_L_INC => P A X X , A P X X , <repeat>...
	OWNER_SCHEME_SL_START_INC=> P A X X , X P A X , X X P A , A X X P, <repeat>....
    OWNER_SCHEME_SL_START_DEC=> P X X A , A P X X , X A P X , X X A P, <repeat>....

  The lock-mode of a raid has 4 supported modes (for L locks):
     in the future, a raid config will contain this parameter so different
	 volumes could use different mode. for now this is global.
   1. OWNER_SCHEME_SL_START_INC: (D > L) => Owner is in slice start. At most L-1 Copy locks on segments after  it
   2. OWNER_SCHEME_SL_START_DEC: (D > L) => Owner is in slice start. At most L-1 Copt locks on segments before it
   3. OWNER_SCHEME_FIRST_L_INC : (D > L) => Same as OWNER_SCHEME_SL_START_INC but only inside [0..L-1] first segs. Owner != slice start
   4. SYMMETRIC                : (D<= L) => All data members have a lock. All the above 3 methods become identical
*/

struct nvmeibc_locks_scheme {					// 32bits, Same as nvmeibc_locks_scheme_conf but packed
	lock_server_type_e type;
	int max_n_owners   : 8;							// # Owners for a raid slice (Primary + Dual + Copy-Owner(s)), Clnt will use min(max_n_owners, n_raid_segments)
	int lockset_shift : 16;							// Owner lock is zigzagging every 2^lockset_shift blocks. Typically {1 or 2 or 4} x num_slices_in_lock
};

struct nvmeibc_locks_scheme_conf;

void nvmeibc_locks_scheme_build(struct nvmeibc_locks_scheme *dst, const struct nvmeibc_locks_scheme_conf *src);

/**************** Initial owner API ****************/

int nvmeibc_locks_scheme_find_initial_owner_seg(const struct nvmeibc_locks_scheme *ls, const u64 offset_in_blks, const int replicas);
int nvmeibc_locks_scheme_find_slice_start_seg(const u64 slba, const int replicas);

#endif /* NVMEIBC_BLOCK_DP_LOCKS_SCHEME_H_ */
