#include "nvmeibc_pausable.h"
#include "nvmeibc_block_dp_ec_recovery_common.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeibc_block_dp_ec_recov_cold.h"
#include "nvmeibc_block_dp_ec_recov_hot.h"
#include "../../controlpath/nvmeibc_b_cp_blkset_topo.h"
#include "../nvmeibc_block_dp_ec.h"
#include "nvmeibc_memmgr_metrics.h"

NVMEIBC_MEMMGR_METRIC(dp_recovery_cold, "component=raid.io_ctrl.cold");

/********************************************************************
* Copied December 31st from Design Document
* https://docs.google.com/document/d/1Kd87He07uE4HIxwO6kf0KnnfCUHOMPwfWXODK1g6SsY
*
* Client-Side Cold Recovery
*
* Cold recovery has two phases: the first is done behind gates-closed -
* no I/O allowed, while the second is more like hot recovery.
*
* Cold Recovery - Analysis Phase (crac)
* ------------------------------
* This phase is performed when no I/O is allowed and should take just a few
* seconds.
*
* 1. TOMA reads topology and journal chunk allocation information from
*    drive metadata
* 2. Server Jr Component to read all allocated journal chunks for all drives
*    in that server and to store the metadata information in RAM.
*    a. We have 128*8B=1KB metadata per chunk, overall up to 4MB per drive
*    b. Pay attention that this formulation automatically takes into account
*       whether a drive is EC-ready, have EC-segment allocated on it, etc.
* 3. Each Server Jr Component primes the J2D Map in-RAM data structure with
*    illegal value (-1) for each journal entry
* 4. On apply topology event, each TOMA starts a Cold Recovery Analysis
*    Client (CRAC) per pRAID and assigns it the blockset ranges for which this
*    TOMA is the owner. We may relax the ‘per RAID’ and allow a client to be
*    responsible for several pRAIDs
* 5. Each CRAC asks Server Jr Components for journal metadata areas for the
*    drives comprising the pRAID from all servers relevant for this pRAID and
*    reads all of them to the client’s RAM
* 6. Each CRAC builds a list of viable transaction candidates according to
*    TxID, TxBM and J2D but only for J2Ds falling into the blockset ranges
*    assigned to this client and skips all other entries.
*    a. For a candidate to be viable it should have all readable (according to
*       the current topology) entries from the same journal chunk agreeing on
*       TxID, TxBM and J2D
* 7. When a journal chunk is processed, each entry with J2D pointing to the
*    region under responsibility of this CRAC falls into 2 categories
*    a. Belongs to a viable candidate for blockset B so the corresponding J2D
*       Map on the server side should be changed: (-1) → B
*    b. Can be discarded and the J2D Map entry is zeroed: (-1) → 0
*    c. These updates can be done in batches to save RDMA Write operations
* 8. Upon finishing this operation CRAC writes to 3 server RAMs
*    (owner & 2 backups):
*    a. A map:
*       Lock Index →
*       {(Slice Index, TxBM, TxID, {JCI, Offset | ∀d in TxBM}) | ∀candidates}
*    b. Lock Entry: all zeros (including TxID) but ‘Stale’ bit is set for
*       any blockset for which it has at least one viable candidate
* 9. CRAC reports to all Server Jr Components about the completion of its work.
*    This report causes the servers to walk through J2D Map and check if there
*    are journal chunks that can be released to the pool (no J2D Map entry is
*    non-zero). Each chunk that can be released is zeroed before returning it
*    to the pool.
* 10.In the end CRAC reports about completion to TOMA
* 11.TOMA then goes through all the Lock Entries it owns and if the entry does
*    not have ‘Stale’ bit set TOMA sets its TxID to 1
* 12.In parallel with CRAC operation (or at any point in run-time) TOMA passes
*    to the local Server Jr Component the list of unallocated ranges on the
*    local drives so the Jr Component can walk through journal metadata and
*    clean all J2D Map entries (change (-1) → 0, as if a client-recoverer
*    released them) which somehow point to unallocated ranges. This operation
*    combined with the CRAC updates of J2D Map should free the journal very
*    quickly leaving only the information required for second phase of cold
*    recovery.
*
* Cold Recovery - Execution Phase (sync)
* -------------------------------
* The actions described below can be performed by a special recovery client
* started by TOMA or by any client that stumbles upon an entry with TxID = 0
* and ‘Stale’ bit set.
* 1. Lock it with the Recoverer LockID and ‘use-journal’ bit is reset
* 2. Ask the owner TOMA for the list of candidates for this blockset
*    a. If we want for TOMA to be abstracted from the internals of the
*       candidate map - it can provide the address and the information will
*       be brought by RDMA Read
* 3. For each candidate:
*    a. Read the metadata of the slice pointed by J2D in all drives mentioned
*       in TxBM
*    b. Look for data block with
*       i. JCI field points to the chunk the candidate was found in
*       ii.TxID matches the candidate’s TxID
*    c. If no such data block found - discard the candidate
* 4. If no candidates remain - release the lock with ‘Stale’ bit reset and
*    TxID = 1
* 5.Among the remaining candidates choose the one with the highest TxID
*    a. We can’t get a TxID from before wrap-around since on wrap-around the
*       TxIDs in the data blocks’ metadata are zeroed and therefore we can’t
*       have a match above
*    b. We can’t have same TxIDs in different candidates at this point since
*       the data metadata will point to a specific chunk via JCI field so only
*       one will remain
*    c. We can’t have same TxID in different candidates in the same journal
*       chunk for the same reasons we already mentioned in hot recovery
*    6. Roll it forward and remember its TxID
* 7. Release the lock with ‘Stale’ bit reset and with TxID set according to
*    the rolled forward TxID
* 8. Update J2D Maps on all servers that the entries used by candidates are
*    now free (change (-1) → 0)
*    a. There is no concern here to release journal entries after releasing
*        the lock since the journal chunks are under server responsibility
*        anyway
*/

/* This cmd is used to retrive the entire jmdc (in a packed way) & send cleaned bmp */
struct nvmeibc_disk_jcmd {							// Journal related command to disk
struct jrecovery *jrecov;						// Reference to main struct
	struct nvmeib_jmdc_read_jrnl_data jrnl_desc;	// Header of the entire journal area
	struct {										// Array of headers of journal range allocations, 1 for each non empty JRI.
		struct nvmeib_get_jmdc_rng_data *arr;		// Block layer accessor to the array.
		struct nvmeib_alloc_info _ai;				// Transport layer allocation (accessor) to the array
		size_t len;									// Length of the array
	} rng;
	struct {										// Array of headers of journal range allocations, 1 for each non empty JRI.
		struct nvmeib_jrnl_ent_md *arr;			// Block layer accessor to the array.
		struct nvmeib_alloc_info _ai;				// Transport layer allocation (accessor) to the array
		size_t len;									// Length of the array
	} ent_md;
	struct {										// Packed jmdc entries of entire journal area
		union jblock_md *arr;// Block layer accessor to the array.
		struct nvmeib_alloc_info _ai;				// Transport layer allocation info (accessor)
		size_t len;									// Length of the JMDC array
	} md;
	struct nvmeibc_disk_jmdc_read_comp comp;		// Read jmdc completion
	struct nvmeibc_disk_free_jrnl_ents_comp free_ents;// Send to Serjio which jentries can cleaned upon recovery finish
};
#define __get_ent_of( djcmd, ent_offset) (&(djcmd)->md.arr[    ent_offset])
#define jentry_gen_id(djcmd, ent_offset) (&(djcmd)->ent_md.arr[ent_offset])

struct jrecovery {					// Cold recovery struct, inherits from regular recovery
	struct nvmeibc_recovery *recovery;
	struct nvmeibc_disk_jcmd jcmds[N_MAX_RAID_SLICE_LEN];
	ulong bmp;						// Bitmap of segs which jmdc is analyzed. 'RW/W+' for cold recovery but RW/W+/W/D for JGC
	int n_segs;						// Num segs in praid == num elements in 'jcmds' array

	int max_candidates;				// Max possible blocksets to be fixed (allocation size)
	int num_candidates;				// Actual amount of blocksets <= max_candidates <= Min(Total Journal entries, Num blockset in segment)
	struct nvmibc_blockset_candidates *cands;	// Array of candidates (used by the iterator)
	struct rb_root root;			// just a helper, to build correctly the iterator array.
	struct list_head rb_list;		// For fast delete of the rb tree.
	atomic_t reads;					// Counter for amount of in air requests
	struct work_struct work;		// Move from interurpt to thread context
	BLKCMP_SO_DEFINE_BLOCKING_CONTEXT;
	union {
		struct {					// Extention for Garbage collection recovery
			u32 num_garbage_collected;	// amount of total garbage collected
			enum NVMEIBTC_DS_MODE acm;	// Type of garbage collection, LKJ: Todo, change to boolan (Auto garbage D/W/W- and must eximne-W+/RW) this will simplify code
		};
	};
};

#define jrecov_is_default_cand_garbage(jrec) \
	((jrec->recovery->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC) && \
	(jrec->acm != NVMEIBTC_DS_MODE_RW)) // Note: Here RW is RW/W+


static void *cldr_kzalloc(size_t size, gfp_t flags)
{
	void *ptr = kzalloc(size, flags);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_cold, ptr? ksize(ptr):size, ptr);
	return ptr;
}

static void *cldr_kmalloc(size_t size, gfp_t flags)
{
	void *ptr = kmalloc(size, flags);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_cold, ptr? ksize(ptr):size, ptr);
	return ptr;
}

void cldr_kfree(void *ptr);
void cldr_kfree(void *ptr)
{
	if (ptr)
		nvmesh_memmgr_metric_on_free_update(dp_recovery_cold, ksize(ptr));
	kfree(ptr);
}

static unsigned long cldr__get_free_pages(gfp_t gfp_mask, unsigned int order)
{
	unsigned long addr = __get_free_pages(gfp_mask, order);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_cold, (1 << order) * PAGE_SIZE, addr);
	return addr;
}

static void cldr_free_pages(unsigned long addr, unsigned int order)
{
	if (addr)
		nvmesh_memmgr_metric_on_free_update(dp_recovery_cold, (1 << order) * PAGE_SIZE);
	free_pages(addr, order);
}

/******************** Hash map for candidates building ************************/
struct bs_node {					// Element in hash map (RB-tree)
	struct rb_node node;
	struct list_head list;			// List of nodes for fast delition of hash
	u32 blkst_lba;					// Index of blockset on the disk. u32 -> enough for 4G*128KB = 512[TB] disk
	int cand_ind;			// Index of corresponding element in candidates arr
};

static bool bs_node_tree_insert(struct rb_root *root, struct bs_node *data,
				struct list_head *head)
{
	struct rb_node **node = &(root->rb_node), *parent = NULL;
	while (*node) {				// Figure out where to put new node
		struct bs_node *cur = container_of(*node, struct bs_node, node);
		parent = *node;
		if (data->blkst_lba < cur->blkst_lba)
			node = &((*node)->rb_left);
		else if (data->blkst_lba > cur->blkst_lba)
			node = &((*node)->rb_right);
		else
			return false;		// Should never happen, coz we verified that
	}

	list_add(       &data->list, head);	// Add new node and rebalance tree
	rb_link_node(   &data->node, parent, node);
	rb_insert_color(&data->node, root);
	return true;
}

static void bs_node_free_tree(struct list_head *head)
{
	struct bs_node *bsn, *tmp;
	list_for_each_entry_safe(bsn, tmp, head, list) {
		list_del(&bsn->list);
		cldr_kfree(bsn);
	}
}

static struct bs_node *bs_node_tree_lookup(struct rb_root *root, u32 key)
{
	struct rb_node *node = root->rb_node;
	while (node) {
		struct bs_node *cur = container_of(node, struct bs_node, node);
		if (     key < cur->blkst_lba)
			node = node->rb_left;
		else if (key > cur->blkst_lba)
			node = node->rb_right;
		else
			return cur;
	}
	return NULL;
}

struct tx_client_indices {			 // Candidate: Array of values for each segment
	uuid_be cuuid;                   // client uuid which has dirty jri somewhere in the volume
	int jris[ N_MAX_RAID_SLICE_LEN]; // -1 if not found, otherwise jri
	int rng_arr_idx[ N_MAX_RAID_SLICE_LEN]; // -1 if not found, otherwise index into rng_data array
};

struct tx_cand_indices {			  // Used to construct a candidate. Has arrays of values for each segment, Tmp variable used in a loop
	const struct tx_client_indices *client; // inherit from client
	struct jent_md_decompressed jent_mds[N_MAX_RAID_SLICE_LEN];
	int jents[N_MAX_RAID_SLICE_LEN];  // -1 if not found, otherwise jentry index
	int jents_offset[N_MAX_RAID_SLICE_LEN]; // -1 if not found, otherwise offset into MD buffer
	int jents_blk_offset[N_MAX_RAID_SLICE_LEN]; // -1 if not found, otherwise offset into JMD buffer
};

#define clean_Tx(field) 	memset(field, -1, sizeof(field));	// Clean jris/jents
#define clean_zero_Tx(field) memset(field, 0, sizeof(field));

static int __add_jour_candidate_to_blockset(struct jrecovery *jrecov, struct tx_cand_indices *tx, const int pivot_seg)
{
	const struct jent_md_decompressed *jent_pivot = &tx->jent_mds[pivot_seg];
	struct nvmeibc_recovery *recov = jrecov->recovery;
	const u64 slba = jent_pivot->md_arr[0].j2slba;
	const u32 blockset = (u32)(slba/LOCKSET_SLICES);								// We reduce by 5 bits so effectivly represents slba of 37[bits]
	const u32 tx_id = jent_pivot->md_arr[0].tx_id;
	struct bs_node *bsn = NULL;
	struct nvmibc_blockset_candidates *candidates = jrecov->cands, *bc;
	struct nvmibc_tx_candidate *c;
	int i, rv = 0;
	NFIN;
	if (!(c = cldr_kzalloc(sizeof(*c), GFP_KERNEL))) {
		rv = -ENOMEM;
		goto out_free;
	}
	if (!(bsn = bs_node_tree_lookup(&jrecov->root, blockset))) {		// Find blockset or create a new struct for blockset
		if (unlikely(jrecov->num_candidates >= jrecov->max_candidates)) {
			WARN((jrecov->num_candidates >= jrecov->max_candidates), "nvmeibc bug: too much candidate blocksets: %d >= %d", jrecov->num_candidates, jrecov->max_candidates);
			rv = -ENOMEM;
			goto out_free;
		}

		if (!(bsn = cldr_kzalloc(sizeof(*bsn), GFP_KERNEL))) {
			rv = -ENOMEM;
			goto out_free;
		}
		bsn->cand_ind = jrecov->num_candidates;
		bsn->blkst_lba = blockset;
		if (!bs_node_tree_insert(&jrecov->root, bsn, &jrecov->rb_list)) {
			rv = -EINVAL;
			goto out_free;
		}
		bc = &candidates[jrecov->num_candidates++];
		bc->blkset_lba = blockset;
		INIT_LIST_HEAD(&bc->can_list);
		_NTRR(t_01_crajc, "@SLBA_BLKSETS: Created, n_blocksets=@INT", blockset, jrecov->num_candidates);
	}
	list_add(&c->next, &candidates[bsn->cand_ind].can_list);
	c->cuuid = tx->client->cuuid;
	c->b.j2slba = slba;
	c->b.len = jent_pivot->len;
	for (i=0; i < c->b.len; ++i)
		c->b.tx_bmp[i] = jent_pivot->md_arr[i].tx_bmp;
	c->b.tx_id = tx_id;

	//c->b.version_unused = jent_pivot->md_arr[0].version;
	_NTRR(t_02_crajc, "@SLBA_BLKSETS: Jcand++: slba=@J2D, txid=@TXID, pivot_len=@HEX_1B, clnt=@UUID_4B_BE, ver=@HEX_1B, first_slice_txbm=@TXBM", blockset, slba, tx_id, c->b.len, &c->cuuid, jent_pivot->md_arr[0].version, c->b.tx_bmp[0]); 	// Todo, consider unify with CAND_PRINT_FMT
	c->locations[0].is_garbage = jrecov_is_default_cand_garbage(jrecov);	// Mark first loc as garbage (if needed)
	for_each_set_bit(i, &jrecov->bmp, jrecov->n_segs) {
		struct nvmeibc_disk_jcmd *djcmd = &jrecov->jcmds[i];
		struct candidate_location *loc = &c->locations[i];
		if (tx->jents[i] < 0 || (!tx->jent_mds[i].is_valid)) {
			if (tx->jents[i] >= 0) { /* optimization: invalidate candidate in jmdc buffer so we won't find it while searching for next candidates */
				union jblock_md *jmd = __get_ent_of(djcmd, tx->jents_blk_offset[i]);
				jmd->raw = nvmeib_jmd_unused_entry_val.raw;
			}
			continue;
		}
		loc->jri =    tx->client->jris[i];
		loc->jentry = tx->jents[i];
		_NTRR(t_03_crajc, "@SLBA_BLKSETS: slba=@J2D, seg=@SI, jri=@JRI, jent=@JENT, jri_gen=@JRNL_RNG_GEN_ID, ent_gen=@JRNL_ENT_GEN_ID", blockset, slba, i, loc->jri, loc->jentry, loc->rng_gen_id, loc->jentry_gen_id);
		loc->jentry_gen_id = jentry_gen_id(djcmd, tx->jents_offset[i])->ent_gen_id;
		loc->rng_gen_id =   djcmd->rng.arr[tx->client->rng_arr_idx[i]].rng_gen_id;
		loc->rng_binje =    djcmd->rng.arr[tx->client->rng_arr_idx[i]].binje;
		loc->rng_start_lba = djcmd->rng.arr[tx->client->rng_arr_idx[i]].rng_start_lba;
		loc->rng_size_lba = djcmd->rng.arr[tx->client->rng_arr_idx[i]].rng_size_lba;
		loc->rng_num_ents = djcmd->rng.arr[tx->client->rng_arr_idx[i]].num_ents;
		loc->binje_offset = (tx->jent_mds[i].md_arr[0].j2slba - jent_pivot->md_arr[0].j2slba);
		loc->binje_len = (u8)tx->jent_mds[i].len;
		loc->is_jour_commited = true;	// Else, kzalloc makes it false
		if (1) {	/* optimization: invalidate candidate in jmdc buffer so we won't find it while searching for next candidates */
			union jblock_md *jmd = __get_ent_of(djcmd, tx->jents_blk_offset[i]);
			jmd->raw = nvmeib_jmd_unused_entry_val.raw;				// Todo: use nvmeib_shared_set_jentry_md_unused()
		}
	}
out_free:
	if (unlikely(rv)) {
		_NTRR(trace_2_dp_ec_recov_cold_add_candidate, "@SLBA_BLKSETS: err=@ERR", blockset, rv);
		cldr_kfree(bsn);
	}
	NFOUT;
	return rv;

}

static inline int get_pivot_si_from_slba(struct nvmeibc_raid1 *r1, u64 slba) {
	const int slice_start = get_owner_seg_slice_start(r1, slba * (u64)r1->slice_size);
	sgmnts_bmp_t rw_parities = nvmeibc_raid1_get_sgmnts_bmp(r1, readable) & nvmeibc_raid1_get_roles_bmp(r1, slice_start, pari_sgmnts);
	ulong rw_parities_bit_arr = rw_parities;
	WARN(!rw_parities, "nvmeibc bug!\n");
	return find_first_bit(&rw_parities_bit_arr, r1->replicas);
}

static inline int get_pivot_si(struct nvmeibc_raid1 *r1, struct jent_md_decompressed jent_mds[]) {
	int i = 0, replicas = r1->replicas;
	while (!jent_mds[i].is_valid && (i < replicas)) {
		i++;
	}
	return get_pivot_si_from_slba(r1, jent_mds[i].md_arr[0].j2slba);
}

#define __md_equal(j1, j2) ((j1->tx_id == j2->tx_id) && (j1->tx_bmp == j2->tx_bmp))
static inline u32 __get_n_jblks_in_jentry_from_disk_cmd(const struct nvmeibc_disk_jcmd *js, int i) {
	u32 rv = js->rng.arr[i].binje;
	WARN(rv==0 || rv > NVMEIB_EC_JOURNAL_MAX_BLOCKS_PER_ENTRY, "nvmeibc bug!, invalid binje=%u\n", rv);
	return rv;
}

static int __calc_candidates_for_clnt_jris(struct jrecovery *jrecov, struct tx_cand_indices *tx)
{
	struct nvmeibc_raid1 *r1 = recovery_topo_pr_ptr(jrecov->recovery);
	struct nvmeibc_disk_jcmd *js1, *js2;
	int i, j, err = 0;
	unsigned jc1, jc2;
	union jblock_md *md1, *md2;
	struct jentry_md ent1, ent2;
	u64 slba, slba2;							// slice lba of the candidate
	int rng_ent_offset1, rng_ent_offset2;
	int rng_ent_block_offset1, rng_ent_block_offset2;
	int offset1, offset2, non_dirty_offset1, non_dirty_offset2;
	unsigned num_ents1, num_ents2;
	const int dirty_offset = 1;
	u32 binje1, binje2;

	NFIN;

	/* Guaranteed: ∀i | (SEG(i)=RW && (tx->jris[i] is dirty)) -> (tx->jris[i] != -1) */
	for_each_set_bit(i, &jrecov->bmp, jrecov->n_segs) { // Find Tx on seg i, and subset of segs [i+1...last_seg]
		if (tx->client->jris[i] == -1)  // non dirty range can't be first block of a candidate's txbm
			continue;

		js1 = jrecov->jcmds + i;
		binje1 = __get_n_jblks_in_jentry_from_disk_cmd(js1, tx->client->rng_arr_idx[i]);
		rng_ent_offset1 =       js1->rng.arr[tx->client->rng_arr_idx[i]].rng_ent_offset;
		rng_ent_block_offset1 = js1->rng.arr[tx->client->rng_arr_idx[i]].rng_ent_block_offset;
		non_dirty_offset1 =     js1->rng.arr[tx->client->rng_arr_idx[i]].only_dirty_ents ? 0 : 1;
		num_ents1 =             js1->rng.arr[tx->client->rng_arr_idx[i]].num_ents;
		for (jc1 = 0; jc1 < num_ents1; jc1++, rng_ent_offset1 += offset1, rng_ent_block_offset1 += (offset1*binje1)) {
			if (!test_bit(jc1, (unsigned long *)js1->rng.arr[tx->client->rng_arr_idx[i]].dirty_ents_bmp)) {
				offset1 = non_dirty_offset1;
				continue;
			} else
				offset1 = dirty_offset;
			ent1.md_arr = __get_ent_of(js1, rng_ent_block_offset1);
			md1 = &ent1.md_arr[0];
			if (!nvmeib_is_jmd_io_entry(*md1))
				continue;
			if ((!nvmeib_jentry_md_is_valid(&ent1, binje1)) && (jrecov->recovery->type == NVMEIBT_RECOVERY_TYPE_EC_COLD)) {
				nvmeib_shared_set_jentry_md_unused(md1, binje1);	// Just invalidate the jentry inorder not to pass it again somehow.
				continue;
			}

			clean_Tx(tx->jents);
			clean_Tx(tx->jents_offset);
			clean_Tx(tx->jents_blk_offset);
			clean_zero_Tx(tx->jent_mds);

			nvmeibc_block_dp_ec_md_decode_jentry(r1->segments[i].first_lba, &tx->jent_mds[i], &ent1);
			tx->jent_mds[i].is_valid = true;
			tx->jents[i] =            jc1;
			tx->jents_offset[i] =     rng_ent_offset1;
			tx->jents_blk_offset[i] = rng_ent_block_offset1;
			slba = tx->jent_mds[i].md_arr->j2slba;

			for (j = i + 1; j < jrecov->n_segs; ++j) {
				int n_found_in_jri = 0;
				if (tx->client->rng_arr_idx[j] == -1)
					continue;

				WARN(jrecov->recovery->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC, "nvmeibc bug!\n");
				js2 = jrecov->jcmds + j;
				binje2 = __get_n_jblks_in_jentry_from_disk_cmd(js2, tx->client->rng_arr_idx[j]);
				rng_ent_offset2 =       js2->rng.arr[tx->client->rng_arr_idx[j]].rng_ent_offset;
				rng_ent_block_offset2 = js2->rng.arr[tx->client->rng_arr_idx[j]].rng_ent_block_offset;
				non_dirty_offset2 =     js2->rng.arr[tx->client->rng_arr_idx[j]].only_dirty_ents ? 0 : 1;
				num_ents2 =             js2->rng.arr[tx->client->rng_arr_idx[j]].num_ents;
				for (jc2 = 0; jc2 < num_ents2; jc2++, rng_ent_offset2 += offset2, rng_ent_block_offset2 += (offset2*binje2)) {
					if (!test_bit(jc2, (unsigned long *)js2->rng.arr[tx->client->rng_arr_idx[j]].dirty_ents_bmp)) {
						offset2 = non_dirty_offset2;
						continue;
					} else
						offset2 = dirty_offset;
					if (binje1 != binje2) {
						/*Important when one of the jri without abandoned they might have different binje in the stage of transistion. this is why we locate the warn here. */
						_NW_dmesg(t_00_cc4cj, "module nvmeibc bug!, same client with different binje on different segs, both contain abandoned entries. Crashing the system to prevent data corruption. Error code: 1065."
								"jrecov=@PTR tx=@PTR client1=@CLIENT_UUID, jri1=@JRNL_RNG_IDX, seg1[@INDEX]=@SEG_UUID_STR, N=@BINJE, client2=@CLIENT_UUID, jri2=@JRNL_RNG_IDX, seg2[@INDEX]=@SEG_UUID_STR, N=@BINJE",
								jrecov, tx,
								&js1->rng.arr[tx->client->rng_arr_idx[i]].client_uuid,
								tx->client->jris[i], i, r1->segments[i].uuid, binje1,
								&js2->rng.arr[tx->client->rng_arr_idx[j]].client_uuid,
								tx->client->jris[j], j, r1->segments[j].uuid, binje2);
						BUG();
					}
					ent2.md_arr = __get_ent_of(js2, rng_ent_block_offset2);
					md2 = &ent2.md_arr[0];
					if (!nvmeib_is_jmd_io_entry(*md2))	// TxID 0 and empty txbm are invalid
						continue;
					if ( !nvmeib_jentry_md_is_valid(&ent2, binje2)) {
						nvmeib_shared_set_jentry_md_unused(md2, binje2);	// Just invalidate the jentry inorder not to pass it again somehow.
						continue;
					}

					slba2 = nvmeibc_block_dp_ec_jmd_decode_j2d_only(md2) - r1->segments[j].first_lba;
					if (((slba2>>LOCKSET_SLICES_SHIFT) == (slba>>LOCKSET_SLICES_SHIFT)) && (md1->tx_id == md2->tx_id)) {  // both entries are on the same blockset and have the same txid.
						// important: Since JAM ensures that for each blockset and txid there could be only one jentry with this data
						n_found_in_jri++;
						if (unlikely(n_found_in_jri > 1)) {
							WARN(true, "nvmeibc bug!, more than one entry with the same txid on the same blockset and jri: seg=%d jri=%u, txid=%u, slba2=%llu ent2=%d, ent1=%d\n", j, tx->client->jris[i], md2->tx_id, slba2, jc2, tx->jents[j]);
							continue;
						}
						nvmeibc_block_dp_ec_md_decode_jentry(r1->segments[j].first_lba, &tx->jent_mds[j], &ent2);
						tx->jent_mds[j].is_valid = true;
						tx->jents[j] = jc2;	// slba's can differ only after TxID wraparound
						tx->jents_offset[j] = rng_ent_offset2;
						tx->jents_blk_offset[j] = rng_ent_block_offset2;
						break;
					}
				}
			}

			/* Guaranteed: ∀i | tx.jris[i]==-1 -> tx.jents[i]==-1 */
			if (jrecov->recovery->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC)			// On jgc there is no point to check if journal committed since we only analyze 1 seg
				err = __add_jour_candidate_to_blockset(jrecov, tx, i);
			else if (__is_journal_committed(r1, tx->jent_mds, jrecov->bmp)) {		// Cold recovery
				/* Guaranteed: ∀i | rw_txbm[i]==1 -> tx->jents[i]!=-1 */
				const int pivot_seg = get_pivot_si(r1, tx->jent_mds);
				err = __add_jour_candidate_to_blockset(jrecov, tx, pivot_seg);
			}
			if (err)
				goto out;
		}
	}
out:
	NFOUT;
	return err;
}

// Prune out any slices that are not in the recovered area in pRaid
static void __reduce_mds_by_seg_range(struct jrecovery *jrecov, int disk_i)
{
	struct nvmeibc_disk_jcmd *js = &jrecov->jcmds[disk_i];
	struct nvmeibc_disk_free_jrnl_ents_comp *fcmd = &js->free_ents;
	struct nvmeibc_recovery *recovery = jrecov->recovery;
	struct nvmeibc_subscription_ctx *tr = recovery->args.tr;
	bool do_only_owners = recovery->args.do_only_owners;
	struct nvmeibc_raid1 *r1 = recovery_topo_pr_ptr(recovery);
	u32 i, jc;
	int offset;

	/* calculate the range of blocksets (slices) received from TOMA */
	const u64 first_slba = r1->segments[disk_i].first_lba +                                  recovery->args.r_start * LOCKSET_SLICES;
	const u64 last_slba =  r1->segments[disk_i].first_lba + min(r1->segments[disk_i].length, recovery->args.r_end   * LOCKSET_SLICES);
	for (jc = 0; jc < js->jrnl_desc.num_dirty_rng; ++jc) {
		u32 rng_ent_offset =       js->rng.arr[jc].rng_ent_offset;
		u32 rng_ent_block_offset = js->rng.arr[jc].rng_ent_block_offset;
		const u32 binje = __get_n_jblks_in_jentry_from_disk_cmd(js, jc);
		const unsigned num_ents = js->rng.arr[jc].num_ents;
		const int non_dirty_offset = js->rng.arr[jc].only_dirty_ents ? 0 : 1;
		const int dirty_offset = 1;
		for (i = 0; i < num_ents; i++, rng_ent_offset += offset, rng_ent_block_offset += (offset*binje)) {	TODO(multi_slice, Simplify the amount of indices changed in a loop)
			if (!test_bit(i, (unsigned long *)js->rng.arr[jc].dirty_ents_bmp)) {
				offset = non_dirty_offset;
			} else {
				struct nvmeib_jrnl_ent_md *ent_md = jentry_gen_id(js, rng_ent_offset);
				union jblock_md *md =                __get_ent_of(js, rng_ent_block_offset);
				const u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(md);
				const u64 rlba = (j2d - first_slba) * (u64)r1->slice_size;	// rlba of slice
				if (j2d < first_slba || j2d >= last_slba)
					md->raw = nvmeib_jmd_unused_entry_val.raw;		// Dont touch, journal belongs to a tx in different protection raid/volume
				else if (do_only_owners && (get_owner_seg_of_lock(r1, rlba)) != tr->seg)
					md->raw = nvmeib_jmd_unused_entry_val.raw;		// Dont touch, cold recovery on other client will fix it
				else if (!nvmeib_is_jmd_io_entry(*md)) {
					WARN(js->rng.arr[jc].only_dirty_ents, "nvmeibc bug! serjio supposed to send only dirty entries.\n");
					// Do nothing, no journal in this entry
				} else if (recovery->type == NVMEIBT_RECOVERY_TYPE_EC_COLD) {
					// We will analyze and release this jour entry
					fcmd->ents[fcmd->num_ents].rng_idx =    js->rng.arr[jc].rng_idx;
					fcmd->ents[fcmd->num_ents].rng_binje =  js->rng.arr[jc].binje;
					fcmd->ents[fcmd->num_ents].rng_gen_id = js->rng.arr[jc].rng_gen_id;
					fcmd->ents[fcmd->num_ents].ent_md.ent_gen_id = ent_md->ent_gen_id;
					fcmd->ents[fcmd->num_ents].ent_idx = i;
					fcmd->num_ents++;
				} else {
					/* In garbage collection dont auto free all journals, do this after determining who is garbage */
				}
				offset = dirty_offset;
			}
		}
	}
}

static struct tx_client_indices* __find_client_index(struct tx_client_indices clients[NVMEIB_EC_MAX_JOURNAL_RANGES], u32 n_clients, const uuid_be *cuuid)
{
	u32 i;
	for (i = 0; i < n_clients; ++i) {
		if (!nvmeib_uuid_cmp(clients[i].cuuid, *cuuid)) {
			return &(clients[i]);
		}
	}
	return NULL;
}

/* For each client which has any dirty jri on at least one seg, Find it's JRIs (same UUID) on all RW segs.
   Returns the number of relevant clients found */
static u32 __init_clients_indices(struct jrecovery *jrecov, struct tx_client_indices clients[NVMEIB_EC_MAX_JOURNAL_RANGES])
{
	u32 jc, n_clients = 0;
	int seg = 0;
	struct nvmeibc_recovery *recov = jrecov->recovery;

	for_each_set_bit_from(seg, &jrecov->bmp, jrecov->n_segs) {
		struct nvmeibc_disk_jcmd *jcmd = &jrecov->jcmds[seg];
		struct nvmeib_jmdc_read_jrnl_data *J = &jcmd->jrnl_desc;
		_NTRR(t_03_crici, "jcmd: seg=@SI, num_ranges=@NUM_RANGES strt=@STRT, len=@LEN_LLONG, num_entries=@INT", seg, (int)J->num_dirty_rng, J->jrnl_start_lba, J->jrnl_len_lba, J->num_ents);
		for (jc = 0; jc < J->num_dirty_rng; ++jc) {
			const struct nvmeib_get_jmdc_rng_data *rng_data = &jcmd->rng.arr[jc];
			const uuid_be *cuuid = &rng_data->client_uuid;
			struct tx_client_indices* cur_clnt = __find_client_index(clients, n_clients, cuuid);
			if (!cur_clnt) {					// First JRI of this client, add it to list
				WARN_RR(n_clients > NVMEIB_EC_MAX_JOURNAL_RANGES, "nvmeibc bug, too many clients in praid impossible state. Max possible jris in praid: %d, num clients: %d\n", NVMEIB_EC_MAX_JOURNAL_RANGES, n_clients);
				cur_clnt = &clients[n_clients++];
				cur_clnt->cuuid = *cuuid;
				clean_Tx(cur_clnt->jris);
				clean_Tx(cur_clnt->rng_arr_idx);
			}
			cur_clnt->jris[       seg] = rng_data->rng_idx;
			cur_clnt->rng_arr_idx[seg] = jc;
		}
	}
	_NTRR(t_04_crici, "jcmd: going to handle @INT clients", n_clients);
	return n_clients;
}

static int __calculate_all_candidates(struct jrecovery *jrecov)
{
	struct nvmeibc_recovery *recov = jrecov->recovery;
	int rv = 0;
	struct tx_client_indices *clients_ind_arr = cldr_kmalloc(NVMEIB_EC_MAX_JOURNAL_RANGES*sizeof(struct tx_client_indices), GFP_KERNEL);
	u32 i, n_clients;
	struct tx_cand_indices *tx = cldr_kzalloc(sizeof(*tx), GFP_KERNEL);	// Too large to fit on stack

	if ((!tx)||(!clients_ind_arr)) {
		rv = -ENOMEM;
		goto out;
	}
	n_clients = __init_clients_indices(jrecov, clients_ind_arr);

	for (i = 0; i < n_clients; ++i) {
		tx->client = &clients_ind_arr[i];
		_NTRR(t_05_crici, "cuuid=@CLIENT_UUID, analyze", &tx->client->cuuid);
		rv = __calc_candidates_for_clnt_jris(jrecov, tx);
		if (rv)
			goto out;
	}
out:
	cldr_kfree(tx);
	cldr_kfree(clients_ind_arr);
	return rv;
}

static int __crac_analyze(struct jrecovery *jrecov)
{
	int rv, i, n_cand = 0;
	for_each_set_bit(i, &jrecov->bmp, jrecov->n_segs) {
		struct nvmeib_jmdc_read_jrnl_data *jdesc = &jrecov->jcmds[i].jrnl_desc;
		n_cand += jdesc->num_ents; // Total upper bound is 'jdesc->num_dirty_rng * jdesc->num_ents_rng' which is typically much higher in case of 'jgc'
	}
	_NT(t_01_crcrac, "upper bound on dirty blockset=@INT", n_cand);
	jrecov->max_candidates = min((jrecov->recovery->args.r_end - jrecov->recovery->args.r_start), (u64)n_cand);
	if (!jrecov->max_candidates)
		return 0;					// Nothing to analyze

	jrecov->cands = my_kvzalloc(sizeof(*jrecov->cands) * jrecov->max_candidates, GFP_KERNEL);
	if (!jrecov->cands) {
		_NE(t_02_crcrac, DMESG_PREFIX() ": Error allocating memory for candidates");
		return -ENOMEM;
	}

	rv = __calculate_all_candidates(jrecov);
	bs_node_free_tree(&jrecov->rb_list);
	return rv;
}

/* Initialize members of recovery struct for execution of EC cold recovery */
static void __priv_destructor(void* priv);
static struct jrecovery *jmdc_recovery_init(struct nvmeibc_recovery *recov)
{
	struct jrecovery *jrecov = NULL;
	jrecov = cldr_kzalloc(sizeof(*jrecov), GFP_KERNEL);
	if (!jrecov)
		return NULL;

	recov->priv = jrecov;
	recov->priv_destructor = __priv_destructor;
	jrecov->recovery = recov;
	jrecov->root = RB_ROOT;
	INIT_LIST_HEAD(&jrecov->rb_list);
	BLKCMP_SO_BLOCKING_CONTEXT_ALLOC(jrecov);
	return jrecov;
}

static void __jmdc_read_bufs_free(struct jrecovery *jrecov)
{
	int i;
	for (i = 0; i < jrecov->n_segs; ++i) {
		struct nvmeibc_disk_jcmd *djr = &jrecov->jcmds[i];
		if (djr->rng.arr)
			nvmeib_release(&djr->rng._ai, djr->rng.arr, dp_recovery_cold);
		if (djr->ent_md.arr)
			nvmeib_release(&djr->ent_md._ai, djr->ent_md.arr, dp_recovery_cold);
		if (djr->md.arr)
			nvmeib_release(&djr->md._ai, djr->md.arr, dp_recovery_cold);
	}
}

static int allocate_serjio_jfree_cmd(struct nvmeibc_disk_jcmd *djr)
{
	//RRRR: add test that checks cold recovery right after format, without any IO
	struct nvmeibc_disk *disk = djr->comp.disk;
	struct nvmeibc_disk_free_jrnl_ents_comp *fcmd = &djr->free_ents;
	int rv;
	fcmd->ents = NULL;
	BUG_ON(fcmd->gen_cmd); /* Must not be allocated at this point */

	if (!(fcmd->gen_cmd = cldr_kzalloc(sizeof(*fcmd->gen_cmd), GFP_KERNEL))) {
		rv = -ENOMEM;
		goto out;
	}
	if (djr->jrnl_desc.num_ents){
		fcmd->ents_sz = djr->jrnl_desc.num_ents * sizeof(*fcmd->ents);
		fcmd->ents = (void *)cldr__get_free_pages(GFP_KERNEL, get_order(fcmd->ents_sz));
		if (!fcmd->ents) {
			rv = -ENOMEM;
			goto out;
		}
		if (disk->access_local)
			fcmd->ents_enc_buf = NULL;
		else {
			size_t ents_enc_buf_sz = disk->min_gen_cmd_bb;
			if (!ents_enc_buf_sz) {
				ents_enc_buf_sz = NVMEIBC_SECTOR_SIZE;
				_NT(trace_allocate_serjio_jfree_cmd_inv_min_bb,
					"disk @DISK_NAME has min_gen_cmd_bb not set, probably no NRCHs are connected. Command will be sent to pending with minimum size buffer (@SIZE_T)",
					disk->name, ents_enc_buf_sz);
			}
			if (!(fcmd->ents_enc_buf = nvmeib_alloc(&fcmd->ents_enc_ai, ents_enc_buf_sz, dp_recovery_cold))) {
				_NE(err_allocate_serjio_jfree_cmd_oom, DMESG_PREFIX() "OOM allocating @SIZE_T buffer", fcmd->ents_enc_buf_sz);
				rv = -ENOMEM;
				goto out;
			}
			fcmd->ents_enc_buf_sz = fcmd->ents_enc_ai.n << PAGE_SHIFT;
		}
	}
	rv = 0;
out:
	if (rv) { /* Error */
		if (fcmd->gen_cmd) {
			cldr_kfree(fcmd->gen_cmd);
			fcmd->gen_cmd = NULL;
		}
		if (fcmd->ents) {
			cldr_free_pages((unsigned long)fcmd->ents, get_order(fcmd->ents_sz));
			fcmd->ents = NULL;
		}
		if (fcmd->ents_enc_buf) {
			nvmeib_release(&fcmd->ents_enc_ai, fcmd->ents_enc_buf, dp_recovery_cold);
			fcmd->ents_enc_buf = NULL;
		}
	}
	return rv;
}

static void free_serjio_jfree_cmd(struct jrecovery *jrecov)
{
	int i;
	for (i = 0; i < jrecov->n_segs; ++i) {
		struct nvmeibc_disk_free_jrnl_ents_comp *fcmd = &jrecov->jcmds[i].free_ents;
		if (fcmd->ents) {
			cldr_free_pages((unsigned long)fcmd->ents, get_order(fcmd->ents_sz));
			fcmd->ents = NULL;
		}
		if (fcmd->ents_enc_buf) {
			nvmeib_release(&fcmd->ents_enc_ai, fcmd->ents_enc_buf, dp_recovery_cold);
			fcmd->ents_enc_buf = NULL;
		}
		if (fcmd->gen_cmd) {
			cldr_kfree(fcmd->gen_cmd);
			fcmd->gen_cmd = NULL;
		}
	}
}

static void free_jrecov(struct work_struct *w)
{
	struct jrecovery *jrecov = container_of(w, struct jrecovery, work);
	free_serjio_jfree_cmd(jrecov);
	cldr_kfree(jrecov);
}

static void __priv_destructor(void* priv)
{
	struct jrecovery *jrecov = priv;
	jrecov->recovery->priv = NULL;
	jrecov->recovery->priv_destructor = NULL;
	jrecov->recovery = NULL;		// Dont access, Might already be free when work executes
	INIT_WORK(&jrecov->work, free_jrecov);	// In interrupt context or holding spinlock, so no need for async
	BLKCMP_ANY_schedule_work(&jrecov->work);
}

static void __post_crac_launch_iterator(struct jrecovery *jrecov, int err)
{
	struct nvmeibc_recovery *recov = jrecov->recovery;
	recovery_put_topo(recov);
	__jmdc_read_bufs_free(jrecov);
	if (err || !jrecov->max_candidates) {
		_NTRR(trace_dp_ec_recov_cold_post_crac_launch_iterator, "recovery finish with err, @ERR", err);
		BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_finish(recov, err), recov, true);
	} else if (unlikely(jrecov_is_default_cand_garbage(jrecov))) {
		_NTRR(trace_1_dp_ec_recov_cold_post_crac_launch_iterator, "JGC: marks all entries as garbage"); // jrecov->acm
		recov->itr.skip_all(&recov->itr);
		BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_finish(recov, 0), recov, true);	// As if iterator completed a full run and all candiadtes were garbage
	} else {
		BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_start(recov), recov, false);
	}
}

static void __free_jrnl_ents_cb(struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	struct nvmeibc_disk_jcmd *djcmd = container_of(comp, struct nvmeibc_disk_jcmd, free_ents);
	struct jrecovery *jrecov = djcmd->jrecov;
	struct nvmeibc_recovery *recov = jrecov->recovery;
	int i, err = 0;

	if (NCL_had_acquire_callback(comp->status))
		nvmeibc_pd_cb_called_free_jrnl_ents(comp->disk, comp);
	if (!atomic_dec_and_test(&jrecov->reads))
		return;
	_NTRR(trace_dp_ec_recov_cold_free_jrnl_ents_cb, "All free jrnl msg to Serjios returned");
	for_each_set_bit(i, &jrecov->bmp, jrecov->n_segs) {
		if (!NCL_do_i_have_lock(jrecov->jcmds[i].free_ents.status)) {
			err = -10048;
			_NTRR(error_dp_ec_recov_cold_free_jrnl_ents_cb, "Failed on Serjio_idx=@RV, error=@ERR", i, err);
		}
	}
	recovery_on_batch_finish(recov, err);
}

static void __mark_garbage_entries_as_free(struct nvmeibc_recovery *recov)
{
	struct jrecovery *jrecov = recov->priv;
	//struct nvmeibc_subscription_ctx *tr = recov->init.tr;		// For prints
	int c, l;

	jrecov->num_garbage_collected = 0;
	for (c = 0; c < jrecov->num_candidates; c++) {
		struct nvmibc_blockset_candidates *cand = &jrecov->cands[c];
		struct nvmibc_tx_candidate *cur;
		list_for_each_entry(cur , &cand->can_list, next) {			// Todo, optimize: Decision of garbage can be tested on first element only
			struct candidate_location *loc = &cur->locations[0];	// Enough to test first location
			nvmibc_tx_candidate_invalidate(cur);
			if (!loc->is_garbage)
				continue;
			for_each_set_bit(l, &jrecov->bmp, jrecov->n_segs) {	// Mark all disks of all journal entries to the curernt blockset as free
				if (loc[l].is_jour_commited) {						// Daniel: This is true only in subset of jrecov->bmp bits
					struct nvmeibc_disk_jcmd *js = &jrecov->jcmds[l];
					struct nvmeibc_disk_free_jrnl_ents_comp *fcmd = &js->free_ents;
					struct nvmeib_free_ents_data *fd = &fcmd->ents[fcmd->num_ents]; // We copied those fields from  struct nvmeib_get_jmdc_rng_data -> fd
					fd->rng_gen_id =        loc[l].rng_gen_id;
					fd->rng_idx =           loc[l].jri;
					fd->rng_binje =         loc[l].rng_binje;
					fd->ent_idx =           loc[l].jentry;
					fd->ent_md.ent_gen_id = loc[l].jentry_gen_id;
					fcmd->num_ents++;
					//_NT(__mark_garbage_entries_as_free_t1, "@INT) Disk @INT, dlba=@INT32_HEX/j2d=@INT32_HEX txbmp=@INT32_HEX jri=@INT, jent=@INT", jrecov->num_garbage_collected, l, cand->blba, cur->b.j2slba, cur->b.tx_bmp, loc[l].jri, loc[l].jentry);
					jrecov->num_garbage_collected++;
				}
			}
		}
		nvmibc_blockset_candidates_kfree_deleted(cand);							// Candidates are not needed anymore, much like cold sync does
	}
}

static void __send_msg_free_jrnl_ents(struct nvmeibc_recovery *recov)
{
	struct nvmeibc_disk_free_jrnl_ents_comp *fcmd;
	struct jrecovery *jrecov = recov->priv;
	int rv, i;
	/* Crucial to cache on stack, coz everythings gets kfree in the last iteration */
	const ulong bmp = jrecov->bmp;
	const int n_segs = jrecov->n_segs;
	// recovery_on_batch_request(recov);	// Technically we should call it, but we dont want to mark the batch as complete
	if (recov->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC) {
		__mark_garbage_entries_as_free(recov);
		_NTRR(trace_dp_ec_recov_cold_send_msg_free_jrnl_ents, "JGC collected @NUM_GARBAGE_COLLECTED entries", jrecov->num_garbage_collected);
		atomic_add(jrecov->num_garbage_collected, &nvmeibc_flow_counters_ref()->jour.n_jgc_freed);
	} /* Else Cold recovery already marked all as free */

	_NTRR(trace_1_dp_ec_recov_cold_send_msg_free_jrnl_ents, "Sending to @RV Serjios: bmp=@BITMAP, n_segs=@RV", atomic_read(&jrecov->reads), (u32)bmp, n_segs);
	for_each_set_bit(i, &bmp, n_segs) {
		fcmd = &jrecov->jcmds[i].free_ents;
		if (fcmd->num_ents) {
			rv = nvmeibc_pd_free_jrnl_ents(fcmd->disk, fcmd);
			if (rv) {
				_NTRR(trace_2_dp_ec_recov_cold_send_msg_free_jrnl_ents, "Failed on Serjio_idx=@RV, rv=@RV", i, rv);
				fcmd->status = NCL_STATUS_DISKDEAD;	// Same as NCL_STATUS_FAIL_COMP
				fcmd->callback(fcmd);
			} else { /* Beware, Entire recovery kfree() here */ }
		} else {
			fcmd->status = NCL_STATUS_TRANSFERRED;
			fcmd->callback(fcmd);
		}
	}
}

static void __prepare_jrnl_free_cmds(struct jrecovery *jrecov, int disk_i)
{
	struct nvmeibc_disk_free_jrnl_ents_comp *fcmd;

	atomic_set(&jrecov->reads, hweight32(jrecov->bmp));
	fcmd = &jrecov->jcmds[disk_i].free_ents;
	fcmd->callback = __free_jrnl_ents_cb;
	switch (jrecov->recovery->type) {
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
		fcmd->recov_src = NVMEIB_RECOV_SRC_COLD;
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
		fcmd->recov_src = NVMEIB_RECOV_SRC_JGC;
		break;
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
		fcmd->recov_src = NVMEIB_RECOV_SRC_HTR;			// RRRR: Daniel, WTF? Why this case. What about stale locks recovery, HTR uses this function but not recovery!!!!!
		break;
	default:
		BUG_ON(1);
	}
	fcmd->start_ent = 0;
	fcmd->num_ents = 0;
	fcmd->disk = jrecov->jcmds[disk_i].comp.disk;        // copy from read_jmdc command
	memcpy(fcmd->seg_uuid, jrecov->jcmds[disk_i].comp.seg_uuid, NVMEIB_GID_STR_MAX);
	memcpy(fcmd->serjio_boot_id,
		   jrecov->jcmds[disk_i].comp.rsp.read_jrnl_data->serjio_boot_id, NVMEIB_GID_STR_MAX);
}

static void __do_crac_analysis(struct work_struct *w)
{
	struct jrecovery *jrecov = container_of(w, struct jrecovery, work);
	struct nvmeibc_recovery *recov = jrecov->recovery;
	u64 b_length_ub;				// Upper bound on batch length
	int err = 0;
	int i;

	WARN_RR(recovery_topo_pr_ptr(recov) == NULL, "nvmeibc bug, topo=NULL\n");				// Incorrect flow
	for_each_set_bit(i, &jrecov->bmp, jrecov->n_segs) {
		if (!NCL_do_i_have_lock(jrecov->jcmds[i].comp.rsp.status)) {
			err = -ENOEXEC;
			_NTRR(trace_1_do_crac_analysis, "failed to receive jmdc from seg=@SI, err=@ERR\n", i, err);
			goto _out;
		}
		err = allocate_serjio_jfree_cmd(&jrecov->jcmds[i]);
		if (err) {
			_NTRR(trace_2_do_crac_analysis, "failed to allocate Serjio journal free cmd, err=@ERR", err);
			goto _out;
		}
		/* Prepare each free cmd */
		__prepare_jrnl_free_cmds(jrecov, i);
		/* For Cold: Also fills in the entries */
		__reduce_mds_by_seg_range(jrecov, i);
	}
	err = __crac_analyze(jrecov);
	if (unlikely(err)) {
		_NTRR(trace_3_do_crac_analysis, "failed to calaculate candidate, err=@ERR", err);
		/* Todo: here cleanup partial allocations of crac, to not leak them */
		goto _out;
	}

	/* cold "next batch" is to inform Serjio about free journal entries.
	 * If Serjio reports zero dirty ranges, we don't need to free journal entries */
	if (jrecov->max_candidates) {
		recov->has_appendix_task = true;
		recov->do_next_work_batch_cb = __send_msg_free_jrnl_ents;
	}
	b_length_ub = (recov->args.r_end - recov->b_start);	// Exactly the remaining (entire) range coz we have only 1 batch.
	recovery_on_batch_req_comp_calc_upper_bound(recov, jrecov->num_candidates, b_length_ub); // Also works for num_candidates == 0
	recov->itr.reinit(&recov->itr, 0, 0, jrecov->num_candidates, 64, (void*)jrecov->cands);
_out:
	__post_crac_launch_iterator(jrecov, err);
}

static void __read_jcmd_cb(struct nvmeibc_disk_jmdc_read_comp *comp)
{
	struct nvmeibc_disk_jcmd *djr = container_of(comp, struct nvmeibc_disk_jcmd, comp);
	struct jrecovery *jrecov = djr->jrecov;

	if (NCL_had_acquire_callback(comp->rsp.status))
		nvmeibc_pd_cb_called_jmdc(comp->disk, comp);
	if (atomic_dec_and_test(&jrecov->reads)) {				// Last read returned
		struct nvmeibc_recovery *recov = jrecov->recovery;
		struct work_struct *work = &jrecov->work;			// interrupt context, schedule the analysis to thread context
		_NTRR(trace_dp_ec_recov_cold_read_jcmd_cb, "All jmdc read returned, scheduling crac");
		BLKCMP_RC_ASYNC_RESUME_CMP(INIT_WORK(work, __do_crac_analysis);	BLKCMP_ANY_schedule_work(work), jrecov, false);
	}
}

static int __jmdc_read_bufs_alloc(struct jrecovery *jrecov, struct nvmeibc_raid1 *r1)
{
	int i;
	for_each_set_bit(i, &jrecov->bmp, jrecov->n_segs) {
		struct nvmeibc_disk_jcmd *djr = &jrecov->jcmds[i];
		struct nvmeibc_disk *disk = r1->segments[i].disk;
		djr->rng.len = sizeof(*djr->rng.arr) * disk->jour.tot_n_rng;
		djr->rng.arr = nvmeib_alloc(&djr->rng._ai, djr->rng.len, dp_recovery_cold);
		djr->ent_md.len = sizeof(*djr->ent_md.arr) * disk->jour.tot_n_rng * NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
		djr->ent_md.arr = nvmeib_alloc(&djr->ent_md._ai, djr->ent_md.len, dp_recovery_cold);
		djr->md.len =  sizeof(*djr->md.arr) * disk->jour.tot_n_rng * disk->jour.max_rng_blk;
		djr->md.arr =  nvmeib_alloc(&djr->md._ai, djr->md.len, dp_recovery_cold);
		if (!djr->rng.arr || !djr->ent_md.arr || !djr->md.arr) {
			_NE(error_dp_ec_recov_cold_jmdc_read_bufs_alloc, DMESG_PREFIX() ": Out of memory");
			return -ENOMEM;
		}
	}
	return 0;
}

static int __jmdc_req_alloc(struct jrecovery *jrecov, struct nvmeibc_raid1 *r1)
{
	struct nvmeibc_recovery *recov = jrecov->recovery;
	struct nvmeibc_subscription_ctx *tr = recov->args.tr;
	u32 min_segs_needed;

	WARN_RR(in_interrupt(), DMESG_PREFIX(": ") "illegal call, can stuck\n");
	jrecov->n_segs = r1->replicas;
	if (recov->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC) {		// Solve GC only in callers segment
		jrecov->acm = r1->segments[tr->seg].toma_acm;
		 _NTRR(trace_dp_ec_recov_cold_jmdc_req_alloc, "JGC @ACM seg", nvmeibt_client_topo_seg_access_mode_to_str(jrecov->acm));
		switch (jrecov->acm) {
			case NVMEIBTC_DS_MODE_RW  : /* Regular JGC */; break;
			case NVMEIBTC_DS_MODE_W_NO_DIRTY: jrecov->acm = NVMEIBTC_DS_MODE_RW; break;	/* Treat as RW */
			case NVMEIBTC_DS_MODE_DEAD: /* All garbage by definition */; break;
			default                   : /* All garbage according to current design */; break;
		}
		jrecov->bmp = (1 << tr->seg);	// Important, use =, not &=
		min_segs_needed = 1U;
	} else {
		jrecov->bmp = nvmeibc_raid1_get_sgmnts_bmp(r1, readable);
		min_segs_needed = (u32)r1->slice_size;					// Cold recovery cant operate with less
	}
	if (unlikely(hweight32(jrecov->bmp) < min_segs_needed)) {
		WARN_RR(1, "nvmeibc bug, invalid topo: bmp=0x%x\n", (u32)jrecov->bmp);        // Incorrect flow
		return -EINVAL;
	}
	if (__jmdc_read_bufs_alloc(jrecov, r1))
		return -ENOMEM;
	return 0;
}

static void __jmdc_req_send(struct jrecovery *jrecov, struct nvmeibc_raid1 *r1)
{
	struct nvmeibc_recovery *recov = jrecov->recovery;
	int i, rv = 0, should_auto_fail = false;
	const ulong req_bmp = jrecov->bmp;
	const int n_segs = jrecov->n_segs;	// Important cache on stack!
	enum nvmeib_recov_src recov_src =
		(jrecov->recovery->type == NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC ?
		NVMEIB_RECOV_SRC_JGC : NVMEIB_RECOV_SRC_COLD);

	_NTRR(trace_1_dp_ec_recov_cold_jmdc_req_send, "req_bmp=@BITMAP, n_segs=@RV", (u32)req_bmp, n_segs);

	atomic_set(&jrecov->reads, hweight32(jrecov->bmp));
	for_each_set_bit(i, &req_bmp, n_segs) {						// Iterate using stack values
		struct nvmeibc_disk_jcmd *drj = &jrecov->jcmds[i];
		drj->jrecov = jrecov;
		drj->comp.callback = __read_jcmd_cb;
		drj->comp.disk = r1->segments[i].disk;
		drj->comp.start_rng = 0;
		drj->comp.num_rng = drj->comp.disk->jour.tot_n_rng;
		drj->comp.rsp.read_jrnl_data = &drj->jrnl_desc;
		drj->comp.rng_data_ai =        &drj->rng._ai;
		drj->comp.rsp.rng_data =        drj->rng.arr;
		drj->comp.rsp.rng_data_len =   &drj->rng.len;
		drj->comp.ent_md_ai =			&drj->ent_md._ai;
		drj->comp.rsp.ent_md =			drj->ent_md.arr;
		drj->comp.rsp.ent_md_len =		&drj->ent_md.len;
		drj->comp.jmdc_ent_ai =        &drj->md._ai;
		drj->comp.rsp.jmdc_ent =        drj->md.arr;
		drj->comp.rsp.jmdc_ent_len =   &drj->md.len;
		drj->comp.recov_src = recov_src;
		drj->comp.dirty_only = true;
		memcpy(drj->comp.seg_uuid, r1->segments[i].uuid, NVMEIB_GID_STR_MAX);
		if (!should_auto_fail) {
			rv = nvmeibc_pd_jmdc_read(drj->comp.disk, &drj->comp);
			if (rv == 0)
				continue;
		}
		should_auto_fail = true;
		_NTRR(trace_dp_ec_recov_cold_jmdc_req_send, "Failed for seg=@SI, rv=@RV auto_fail=@RV", i, rv, should_auto_fail);
		drj->comp.rsp.status = NCL_STATUS_DISKDEAD;	// Same as NCL_STATUS_FAIL_COMP
		drj->comp.callback(&drj->comp);
	}	// Warning: Here recov, jrecov, t, r1, all do not exist anymore
}

static void __recovery_read_jcmd(struct work_struct *work)
{
	struct jrecovery *jrecov = container_of(work, struct jrecovery, work);
	struct nvmeibc_recovery *recov = jrecov->recovery;
	struct nvmeibc_raid1 *r1 = recovery_topo_pr_ptr(recov);
	int rv;

	if ((rv = __jmdc_req_alloc(jrecov, r1)) < 0)
		return __post_crac_launch_iterator(jrecov, rv);
	BLKCMP_RC_ASYNC_AWAIT(__jmdc_req_send(jrecov, r1), jrecov);
	BLKCMP_RC_ASYNC_RESUME_SND(__do_crac_analysis(&jrecov->work));
}

void nvmeibc_block_dp_ec_recov_cold_start(struct nvmeibc_recovery *recov)
{
	struct jrecovery *jrecov = NULL;

	if ((recovery_on_batch_request(recov)) != 0)
		goto err;

	_NTRR(trace_1_ec_recov_cold_start, "surviving_bmp=@BITMAP", (u32)recov->args.cold.surviving_ram_bmp);
	if (0) {			/* Todo: Here add a check which executes cold ram recovery instead of full disk+journal cold recovery */
	}
	jrecov = jmdc_recovery_init(recov);
	if (!jrecov) {
		_NTRR(tr_1_recov_cold_start, "nvmeibc, no memory");
		goto err;
	}

	if (recovery_get_topo(recov)) {
		_NTRR(tr_2_recov_cold_start, "nvmeibc, no topology");
		goto err;
	}
	INIT_WORK(&jrecov->work, __recovery_read_jcmd);
	if (!BLKCMP_ANY_schedule_work(&jrecov->work))
		goto err;
	return;

err:
	BLKCMP_RC_ASYNC_RESUME_CMP(recovery_on_batch_finish(recov, -ENOEXEC), recov, true);
}

/********************** DP iterator cold (could be virtual) functions ************************/
void cold_iterator_free(struct nvmibc_blockset_candidates *candidates, u64 num_items)
{
	struct nvmibc_tx_candidate *can, *tmp;
	u64 i;

	if (!candidates)
		return;

	for (i = 0; i < num_items; i++) {
		list_for_each_entry_safe(can, tmp, &candidates[i].can_list, next) {
			list_del(&can->next);
			cldr_kfree(can);
		}
	}
	my_kvfree(candidates);
}
