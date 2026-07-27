#ifndef NVMEIBC_DP_EC_RLDR_INFO_H
#define NVMEIBC_DP_EC_RLDR_INFO_H
/* Describes the data of raid leader command */
#include "../toma/clnt/nvmeibt_client_protocol.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_io_generic_stages.h"

/* Example: We have a 3+1 Raid on 4 segments. Data blocks are arranged as
   Segments - S0 | S1 | S2 | S3         S0 | S1 | S2 | S3
              ---+----+----+----        ---+----+----+----
   Vlba blks: D0   D1   D2   P                    C0
    		  D3   D4   D5   P          C1   C2   C0
    		  D6   D7   D8   P          C1   C2   C0
    		  D9   DA   DB   P          C1
    IO is read of blocks 8 blocks: [2..9] generates 3 commands C0, C1, C2.
    C0 to seg S2 has phys start 0 and length of 3
    C1 to seg S0 has phys start 1 and length of 3
    C2 to seg S1 has phys start 1 and length of 2
    We will initalize the commands by working per slice.
	Loop on slice will do 4 iterations
      iteration 1: partial slice, inits Block1 of C0
      iteration 2: full    slice, runs on C1, C2, C0 adding 1 block to each
      iteration 2: full    slice, runs on C1, C2, C0 adding 1 block to each
      iteration 4: partial slice, runs on C1
    io_phys_addr.start_first_slice == 2: C0 starts on segment S0[2] == S2
    io_phys_addr.base_off == 0 - C0 starts on offset 0 on seg S2. If the read
      was to blocks [8..15] instead of [2..9] then offset would be 8/3 == 2.
    io_phys_addr.n_fs_cmds == 1: First slice is partial using 1 command
      the rest of the commands {C1,C2} get +1 to their physical starting addr.
    io_phys_addr.blk1_end == 2: First 2 commands {C0,C1} got a boost of +1 to
      their length. We had 2 full slices but those commands have length of 3
      coz of partial slices (beggining or end)
    io_phys_addr.blks_end == 1: The last partial slice has a single block in it

    Suppose operation would be write instead of read, we have to add parity
    Parity command is added after each iteration over the slice, parity has
    smallest address of all commands and maximal length
    io_phys_addr.n_slices == 4: length of the parity command C3 on seg S3
*/

/* information attached to a raid leader */
struct nvmeibc_raid_leader_cmd_ctx {
	union nvmeib_blkset_info pre;		// lock status before IO started
	union nvmeib_blkset_info post;		// lock status (changed) by IO
};

static inline union nvmeib_blkset_info
nvmeibc_rldr_get_post_stage_rdma_piggyback(const struct nvmeibc_raid_leader_cmd_ctx *rld, const enum e_cmds_stage stage) {
	union nvmeib_blkset_info rv = {.all = 0};
	if (stage == E_CMDS_STAGE_POST_JR_RDMA) {
		rv.bits.txid  = rld->post.bits.txid;
		rv.bits.dirty = rld->pre.bits.dirty;  // pre.bits.dirty contains dbits after turnof in EC and in QLC it contains the MD block preliminary dbits.
		return rv;
	} else if (stage == E_CMDS_STAGE_POST_IO_RDMA) {
		return rld->post;  // post.bits.dirty contains the dbits after turnon and in QLC also after turnoff (If we can turn off, we must turn off).
	} else {
		WARN(true, "nvmeibc bug! stage=%d\n", stage);
		return rv;
	}
}

bool nvmeibc_rldr_are_parity_sgmnts_dead(const struct nvmeibc_block_command* rldr);

#endif  // H beginning

