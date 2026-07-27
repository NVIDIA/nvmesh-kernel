// For documentation, see Header in H file
/*****************************************************************************/
#include "nvmeibc_block_dp_cmd_lock_link.h"
#include "nvmeib_error_report.h"
#include "nvmeib_utils_bin_traces.h"
#include "nvmeibc_block_dp_common.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_block_dp_dbg_tools.h"
#include "nvmeibc_memmgr_metrics.h"

NVMEIBC_MEMMGR_METRIC(dp_commands_locks_matrix, "component=raid.io.locking");

int nvmeibc_clmat_allocate(struct operation *o, bool use_kv_alloc, int n_locks, int n_cmds, int n_additional_bytes)
{
	BUILD_BUG_ON((((sizeof(*o->locks)/8)*8)) != sizeof(*o->locks));		// Verify each lock is of multiple of 64 bits
	if (0 == n_locks) {
		return 0;														// Nothing allocated, not even additional bytes
	} else {
		const int n_padded_clmat_bits = round_up((n_locks*n_cmds), BITS_PER_LONG);
		const int alloc_size = (n_locks * sizeof(*o->locks)) + (n_padded_clmat_bits >> 3) + n_additional_bytes;
		void *in_op = nvmeibc_operation_alloc_from_sufix(o, (u32)alloc_size);
		if (in_op) {
			memset(in_op, 0, alloc_size);
			o->locks = in_op;
			o->flags.is_clmat_embedded = 1;
		} else if (unlikely(use_kv_alloc))
			o->locks = my_kvzalloc(alloc_size, GFP_KERNEL);
		else
			o->locks = my_kzalloc( alloc_size, GFP_NOFS);

		if (!o->flags.is_clmat_embedded) {
			o->locks->mem_allocated_size = alloc_size;
			nvmesh_memmgr_metric_on_alloc_update(dp_commands_locks_matrix, alloc_size, o->locks);
		}

		if (o->locks) {
			o->CLmat = (void*)&o->locks[n_locks];
			o->CLmat_elems = n_padded_clmat_bits;
			return alloc_size;
		}
		return -ENOMEM;
	}
}

void nvmeibc_clmat_free(struct operation *o)
{
	if (o->CLmat) {			// Freed by nvmeibc_clmat_free_dangling_locks(), todo, consolidate this
		o->CLmat = NULL;
		o->CLmat_elems = 0;
	}
}

void nvmeibc_clmat_free_dangling_locks(struct nvmeibc_cmd_lock *locks)
{
	if (locks) {
		if (locks->resubmit_operation_dont_free) {
			locks->resubmit_operation_dont_free = false;			// Just for debug, not mandatory, will memset upon retry
			nvmeibc_io_resubmitter_retry_op((struct operation *)locks->pg);
		} else if (locks->pg) {
			nvmeibc_operation_free_bio_part(((struct operation *)locks->pg)->bios[0]);
		} else { /* operation and locks don't share memory */
			nvmesh_memmgr_metric_on_free_update(dp_commands_locks_matrix, locks->mem_allocated_size);
			my_kvfree(locks);
		}
	}
}

bool nvmeibc_clmat_is_linked(const ulong *CLmat, int ci, int lsi, const struct nvmeibc_cmd_lock *locksets)
{
	const int ind = ci*locksets->nlocks+lsi;
	return 1UL & (CLmat[BIT_WORD(ind)] >> (ind & (BITS_PER_LONG-1)));
}

void nvmeibc_clmat_set_link(ulong *CLmat, int ci, int lsi, const struct nvmeibc_cmd_lock *locksets)
{
	const int ind = ci*locksets->nlocks+lsi;
	const unsigned long mask = BIT_MASK(ind);
	unsigned long *p = ((unsigned long *)CLmat) + BIT_WORD(ind);
	*p  |= mask;
}

static void __clmat_copy_cmd_link_to_cmd(ulong *CLmat, int ci1, int ci2,
								 const struct nvmeibc_cmd_lock *locksets)
{
	int lsi;
	for (lsi = 0; lsi < locksets->nlocks; lsi++) {
		if (nvmeibc_clmat_is_linked(CLmat, ci1, lsi, locksets))
			nvmeibc_clmat_set_link( CLmat, ci2, lsi, locksets);
	}
}

static inline void __clmat_clean_links(struct operation *o)
{
	memset(&o->CLmat[0], 0, (o->CLmat_elems >> 3));
}

void nvmeibc_clmat_to_string(const struct nvmeibc_block_command *cmds)
{
	static const char col_bible[] = "Locks";
	static const char row_bible[] = "Commands";
	int i, j, nlocks, bible_index;
	if (cmds && cmds->o && cmds->o->CLmat && cmds->locksets) {
		const int MAT_SIZE = 30;	// Print up to 30x30
		#define CLMAT_LINE_SIZE (1+2+1+MAT_SIZE*2+2)	// 1:{prefix bible}, 2:{cmd index}, 1:{}, separator, 2:{each cmd has state,seperator}, 2:{'@',0x0}
		char line[CLMAT_LINE_SIZE], *cur = &line[0];
		nlocks = cmds->locksets->nlocks;
		_NI_dmesg(t_a1_dp_dbg_tools, "   |***** @STR *****", col_bible);
		_NI_dmesg(t_a2_dp_dbg_tools, "---+-------------------");
		for (j = 0, bible_index = 0; j < min((int)cmds->ncmds,MAT_SIZE); j++, cur = &line[0]) {
			if (bible_index < (int)sizeof(row_bible)-1) {
				*cur++ = row_bible[bible_index];
				bible_index++;
			} else {
				*cur++ = ' ';
			}
			cur += scnprintf(cur, 3, "%*d", 2, j);	// the command index
			*cur++ = '|';
			for (i = 0; i < min(nlocks, MAT_SIZE); i++) {
				*cur++ = (nvmeibc_clmat_is_linked(cmds->o->CLmat, j, i,
											cmds->locksets) ? 'X' : '.');
				*cur++ = '|';
			}
			if (dp_cmd_is_raid_leader(&cmds[j]))
				*cur++ = '@'; // For readability, tag all raid leaders
			*cur++ = '\0';
			BUG_ON((size_t)(cur-line) > sizeof(line));
			_NI_dmesg(t_a3_dp_dbg_tools, "@LINE", line);
		}
	}
}

struct nvmeibc_block_command *nvmeibc_cllink_find_cmd_by_lock(
	struct nvmeibc_cmd_lock *locksets, int lsi)
{
	struct nvmeibc_block_command *cmds_arr = locksets->cmds;
	int c;
	lsi = locksets[lsi].owner_id;	// Search by owner
	for (c = 0; c < cmds_arr->ncmds; c+=dp_cmds_get_next_raid_leader(cmds_arr+c)) {
		if (nvmeibc_clmat_is_linked(cmds_arr->o->CLmat, c, lsi, locksets))
			return &cmds_arr[c];
	}
	WARN_ON(true);
	return NULL;
}

int nvmeibc_cllink_find_lock_by_cmd(struct nvmeibc_block_command *cmd)
{
	const struct nvmeibc_cmd_lock *locksets = cmd->cmdarr->locksets;
	int lsi;
	for (lsi = 0; lsi < locksets->nlocks; lsi++) {
		if (nvmeibc_clmat_is_linked(cmd->o->CLmat, cmd->my_leader, lsi, locksets))
			return lsi;
	}
	NVMESH_BUG(1, __dump_operation_report, cmd->cmdarr->o, "nvmeibc bug, cmd=%d, going to crash...\n",
		   (int)(cmd - cmd->cmdarr));
	return -1;
}

static inline bool __ranges_intersect(u64 cs, u64 ce, u64 ls, u64 le)
{   /* Inverse of no intersect condition ((cs >= le) || (ls >= ce)) */
	return (cs < le) && (ls < ce);
}

#define are_different_raids(ds1, ds2) \
	(nvmeibc_get_raid1_of_seg(ds1) != nvmeibc_get_raid1_of_seg(ds2))

static int __link_next_locks_raid_to_cmd(struct nvmeibc_cmd_lock *locksets, int own_i,
		u64 cmd_start, u64 cmd_end, struct nvmeibc_block_command *cmds, int ci)
{
	const int nsibs = locksets[own_i].n_siblings, next_blkset = own_i + nsibs;
	int lsi, has_link = false;
	if (are_different_raids(cmds[ci].ds, locksets[own_i].ds))
		goto _out; /* Irrelevant raid, no need to check sibling locks */
	for (lsi = own_i; lsi < next_blkset; lsi++) {
		const u64 lock_offset = __offset_from_seg(locksets[lsi]);
		const u64 ls_start =    __lock_start(locksets[lsi]) - lock_offset;
		const u64 ls_end =      __lock_end(  locksets[lsi]) - lock_offset;
		if (__ranges_intersect(cmd_start, cmd_end, ls_start, ls_end)) {
			has_link = true;	// All siblings are needed for cmd
			goto _out;
		}
	}
_out:
	if (has_link) { // All raid siblings need the lock
		const int n_refs = dp_cmds_get_next_raid_leader(cmds+ci);
		for (lsi = own_i; lsi < next_blkset; lsi++)
			nvmeibc_clmat_set_link(cmds->o->CLmat, ci, lsi, locksets);
		locksets[own_i].ncmds_non_atomic += nsibs * n_refs; // locks are counted on owner
		cmds[ci].nlocks_take_before_cmd += nsibs;
	}
	return next_blkset;
}

static void __link_cmd_vs_all_locks(struct nvmeibc_cmd_lock *locksets,
								struct nvmeibc_block_command *cmds, int ci)
{
	const struct nvmeibc_block_command *c = dp_cmd_jour_to_data(&cmds[ci]);
	const u64 offset = __offset_from_seg(*c); 		// In units of blocks
	const u64 start =  __cmd_start(*c) - offset;
	const u64 end =    __cmd_end(  *c) - offset;
	int lsi;
	cmds[ci].nlocks_take_before_cmd = 0;
	for (lsi = 0; lsi < locksets->nlocks; )
		lsi = __link_next_locks_raid_to_cmd(locksets, lsi, start, end, cmds, ci);
	nvmeibc_atomic_set(&cmds[ci].nlocks, cmds[ci].nlocks_take_before_cmd);
}

static void __copy_link_of_rldr_to_sibs(struct nvmeibc_cmd_lock *locksets,
					struct nvmeibc_block_command *cmds, int ci)
{
	const struct nvmeibc_block_command *rldr = &cmds[ci];
	int i;
	for (i = 1; i < rldr->nraid_siblings; i++) {
		__clmat_copy_cmd_link_to_cmd(cmds->o->CLmat, ci, ci+i, locksets);
		cmds[ci+i].nlocks_take_before_cmd = rldr->nlocks_take_before_cmd;
		nvmeibc_atomic_set(&cmds[ci+i].nlocks,      rldr->nlocks_take_before_cmd);
	}
}

void nvmeibc_cllink_cmds_locksets(struct nvmeibc_block_command *cmds,
	struct nvmeibc_cmd_lock *locksets, struct nvmeibc_block_command *new_cmds)
{
	int ci, lsi;
	__clmat_clean_links(cmds->o);
	for_each_primary_owner(lsi, locksets)	// Only owners have link: Calculation on the non-atomic var. At the end, copy to atomic.
		locksets[lsi].ncmds_non_atomic = 0;

	if (cmds->use_stages) { /* Link the leader and copy linkage to siblings*/
		for (ci = 0; ci < cmds->ncmds; ci+= cmds[ci].nraid_siblings) {
			__link_cmd_vs_all_locks(    locksets, cmds, ci);
			__copy_link_of_rldr_to_sibs(locksets, cmds, ci);
		}
	} else {
		for (ci = 0; ci < cmds->ncmds; ci++) /* Link each and every command*/
			__link_cmd_vs_all_locks(    locksets, cmds, ci);
	}

	for_each_primary_owner(lsi, locksets)
		nvmeibc_atomic_set(&locksets[lsi].ncmds, locksets[lsi].ncmds_non_atomic);

	cmds->locksets = locksets;
	locksets->cmds = cmds;
	locksets->new_cmds = new_cmds;  /* NULL if is not DISCARD */
	// __dump_operation(cmds->o);	// Daniel: Uncomment to debug via simulator
}

/*****************************************************************************/
// EOF.

