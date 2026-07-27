// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "nvmeibt_topology_simu.h"
#include "uni_framework/range_algorithms.h"

void tTopoOfPraid_init(struct tTopoOfPraid* T, int praid_version, int nMirroring, const char *praidUUID, const char*segsUUIDs[/*nMirroring*/], const int segsNodeIDs[/*nMirroring*/], lock_server_type_e type, int max_n_owners){
	int i, sizeofUUID = sizeof(T->header.uuid);
	memset(T, 0, sizeof(*T));
	memcpy(T->header.uuid, praidUUID ,sizeofUUID);
	T->header.praid_version 			= praid_version;				// Configurations start from arbitrary 13 number in our simulator.
	T->header.io_perms					= 0xFF;							// Everything is permitted
	T->header.blkset_sync_safety		= NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO;	// By default sync operations (recovery) is allowed
	T->header.n_segments 				= nMirroring;
	for (i = 0; i < nMirroring; i++) {	// Set all segs as RW (affects locking scheme)
		T->s[i].access_mode = NVMEIBTC_DS_MODE_RW;	// Default, each disk is read/write.
		memcpy(&T->s[i].uuid, segsUUIDs[i], sizeofUUID);
	}

	for (i = 0; i < nMirroring; i++) {	// Initialize the n'th segment (first segment always exists)
		struct nvmeibt_client_topo_disk_segment *curSeg = &T->s[i];
		curSeg->unused23 = (char)segsNodeIDs[i];	// Unique ID of the node (disk)
		nvmeibt_client_topo_disk_segment_upd_owners(T->s, nMirroring, curSeg->uuid, "", type, max_n_owners, i);	// Init lock server
	}
}

void tTopoOfPraid_getdisks_by_order(const struct tTopoOfPraid* r1, int disk_ids[N_MAX_RAID_SLICE_LEN]) {
	int i;
	for (i=0; (i<N_MAX_RAID_SLICE_LEN)&&(r1->s[i].uuid[0] != 0); i++) {// DONT do: i<r1->header.n_segments. This will prevent us from sending switch topos into degraded mode...
		disk_ids[i] = r1->s[i].unused23;
	}
}

void tTopoOfPraid_getdisks_by_status(const struct tTopoOfPraid* r1, int liveSegInd[N_MAX_RAID_SLICE_LEN+1], int deadSegInd[N_MAX_RAID_SLICE_LEN+1],
																    int liveDskInd[N_MAX_RAID_SLICE_LEN+1], int deadDskInd[N_MAX_RAID_SLICE_LEN+1]){
	const struct nvmeibt_client_topo_disk_segment *curSeg = &r1->s[0];
	int _diskInds[N_MAX_RAID_SLICE_LEN]/* = {[0 ... N_MAX_RAID_SLICE_LEN-1] = -1}*/, i, l = 0, d = 0;
	array_fill(_diskInds, -1);
	tTopoOfPraid_getdisks_by_order(r1, _diskInds);						// Get disk id of each segment.
	for (i=0; i<=N_MAX_RAID_SLICE_LEN; i++)
		liveSegInd[i] = liveDskInd[i] = deadSegInd[i] = deadDskInd[i] = -1;		// Illegal indices

	if (r1->header.n_segments == 1) {									// Backwards compatibility for jbods in v1.1.0
		liveSegInd[l++] = 0;
		deadSegInd[d++] = 1;
		goto _set_disks_of_segs;
	}
	for (i=0; i<r1->header.n_segments; i++, curSeg++){					// Sort segments to live/dead bins
		if (curSeg->access_mode == NVMEIBTC_DS_MODE_DEAD) deadSegInd[d++] = i;
		else											  liveSegInd[l++] = i;
	}
	for (i=0; i<l; i++){												// Backwards compatibility: From the live segments make sure the first one is RW (and W segs come later). in v1.1.0
		if (r1->s[liveSegInd[i]].access_mode == NVMEIBTC_DS_MODE_RW){
			swap(liveSegInd[0], liveSegInd[i]);
			break;
		}
	}
	// if (i==l)   --> Raid 1 with no RW segs
_set_disks_of_segs:
	for (i=0; i<l; i++)	liveDskInd[i] = _diskInds[liveSegInd[i]];
	for (i=0; i<d; i++)	deadDskInd[i] = _diskInds[deadSegInd[i]];
}

int tTopoOfPraid_gen_num_non_readble_segs(const struct tTopoOfPraid* R) {
	int i, n_segs = R->header.n_segments, n_non_rw = 0;
	for (i = 0; i < n_segs; i++)
		if ((R->s[i].access_mode != NVMEIBTC_DS_MODE_RW) && (R->s[i].access_mode != NVMEIBTC_DS_MODE_W_NO_DIRTY))
			n_non_rw++;
	return n_non_rw;
}

void tTopoOfPraid_incVer(struct tTopoOfPraid* r1){
	r1->header.praid_version++;											// Monotonically increase Raid version if it is relevant
}

int tTopoOfPraid_get_size(const struct tTopoOfPraid* R){
	return (sizeof(struct nvmeibt_client_topo_praid) + R->header.n_segments*sizeof(struct nvmeibt_client_topo_disk_segment));
}

void tTopoOfPraid_force_lock_on_read(struct tTopoOfPraid* R, bool should_lock){
	R->header.blkset_sync_safety = (should_lock ? NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_YES : NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO);
}

/* Find the segment which can bear the lock owner for non readable segment 'si' */
static int __calc_owner_lock_seg_of(const struct nvmeibc_locks_scheme_conf *ls, struct tTopoOfPraid* r1, int si) {
	const int n_segs = r1->header.n_segments;
	int i, j, rv = -1;
	const lock_server_type_e e = (lock_server_type_e)ls->type;
	BUG_ON(e != OWNER_SCHEME_SL_START_DEC_C);
	switch (e) {
		case OWNER_SCHEME_FIRST_L_INC_A:
			for (i = 0; (i < n_segs) && (rv == -1); i++) {
				if (r1->s[i].access_mode == NVMEIBTC_DS_MODE_RW)
					rv = i;
			}
			if (rv >= (int)ls->maxNOwners)
				_ND(trace_topology_simu_calc_owner_lock_seg_of, "Non IOable Topo of n-mirrored raid1, coz all lock segments are down");
			break;
		case OWNER_SCHEME_SL_START_INC_A:
			for (j = 0; (j < n_segs) && (rv == -1); j++) {
				i = (si + j) % n_segs;
				if (r1->s[i].access_mode == NVMEIBTC_DS_MODE_RW)
					rv = i;
			}
			break;
		case OWNER_SCHEME_SL_START_DEC_A:
		case OWNER_SCHEME_SL_START_DEC_C:
			for (j = 0; (j < n_segs) && (rv == -1); j++) {
				i = (n_segs + si - j) % n_segs;
				if (r1->s[i].access_mode == NVMEIBTC_DS_MODE_RW)
					rv = i;
			}
			break;
		default: BUG_NOT_IMPLEMENTED_YET;
	} // switch (ls->type)
	BUG_ON(rv < 0);				// Nowhere to read from. Illegal topology. Probably wrong unitest
	return rv;
}

void tTopoOfPraid_update_segs_by_access_mode(struct tTopoOfPraid* r1, const struct nvmeibc_locks_scheme_conf *locks_scheme) {
	int i;
	if (r1->header.n_segments<2) {										// Not applicable for non mirrored volumes
		BUG_ON(r1->s[0].access_mode != NVMEIBTC_DS_MODE_RW);
		memset(r1->s->owners, 0 , sizeof(r1->s->owners));
		nvmeibt_client_topo_disk_segment_upd_owners(r1->s, r1->header.n_segments, r1->s->uuid, "", locks_scheme->type, locks_scheme->maxNOwners, 0);
		return;
	}

	for (i = 0; i < r1->header.n_segments; i++) {				// Set owner segment
		struct nvmeibt_client_topo_disk_segment *s = &r1->s[i];
		const char* owner_uuid     =((s->access_mode == NVMEIBTC_DS_MODE_RW) ?         s->uuid : r1->s[__calc_owner_lock_seg_of(locks_scheme, r1, i)].uuid); // Each segment is its own owner or point to the nearest non dead segment
		const char* sec_owner_uuid = (s->access_mode == NVMEIBTC_DS_MODE_W_NO_DIRTY) ? s->uuid : "";
		memset(s->owners, 0 , sizeof(s->owners));
		nvmeibt_client_topo_disk_segment_upd_owners(r1->s, r1->header.n_segments, owner_uuid, sec_owner_uuid, locks_scheme->type, locks_scheme->maxNOwners, i);
	}
	// Set sync-state. Todo: insert here correct logic, when sync is allowed. copy from toma/nvmeibt_raid1.h/nvmeibt_praid_client_sync_blkset_sync_safety()
	//r1->header.blkset_sync_safety = NVMEIBT_PRAID_IS_PRIMARY_OWNER_LOCK_MOVING_NO;
}

void tTopoOfVolume_destroy(struct tTopoOfVolume *tv){
	int c;
	for (c=0; c<tv->nChunks; c++){
		struct tTopoOfRaid0Chunk* tc = &tv->chunks[c];
		sim_kfree(tc->raids);
		tc->raids = NULL;
	}
	sim_kfree(tv->chunks);
	tv->chunks = NULL;
}

void tTopoOfVolume_incVer(struct tTopoOfVolume* tv){
	int c,i;
	for (c=0; c<tv->nChunks; c++) {
		struct tTopoOfRaid0Chunk* tc = &tv->chunks[c];
		for (i=0; i<tc->stripeWidth; i++)
			tTopoOfPraid_incVer(&tc->raids[i]);
	}
}

bool tTopoOfVolume_isMirrored(const struct tTopoOfVolume* V){
	return (V->chunks->raids->header.n_segments>1);
}

bool tTopoOfVolume_isStriped(const struct tTopoOfVolume* V){
	return (V->segs->stripe_width>1);
}

struct tTopoOfPraid* tTopoOfVolume_getRaid1(struct tTopoOfVolume* tv, int j){
	int c;
	for (c=0; c<tv->nChunks; c++) {
		struct tTopoOfRaid0Chunk* tc = &tv->chunks[c];
		if (j<tc->stripeWidth)
			return &tc->raids[j];
		else
			j -= tc->stripeWidth;
	}
	return NULL;
}

void tTopoOfNVMesh_incVer(struct tTopoOfNVMesh* gTopo) {
	int const err = pthread_mutex_lock(&gTopo->lock);
	BUG_ON(err);
	for (int v=0; v<gTopo->nVolumes; v++)
		tTopoOfVolume_incVer(&gTopo->vols[v]);
	pthread_mutex_unlock(&gTopo->lock);
}

const struct tTopoOfPraid* tTopoOfNVMesh_find_r1_by_seg_uuid(const struct tTopoOfNVMesh *gTopos, const char* uuid_str, const struct tTopoOfVolume **tv_ptr, int*segInd) {
	const int sizeofUUID = 16 + 0*NVMEIB_GID_STR_MAX;			// No need to compare the entire uuid, first 16 bytes is enough. More-over sometimes only the first 16 bytes are given
	int v,c,r,s;
	int err = 0;
	pthread_mutex_t* gTopos_lock = (void*)(&gTopos->lock);

	if (tv_ptr) *tv_ptr = 0;						// Return volume only if needed
	if (segInd) *segInd = 0;
	err = pthread_mutex_lock(gTopos_lock);
	BUG_ON(err);

	for (v = 0; v < gTopos->nVolumes; v++) {
		const struct tTopoOfVolume *tv = &gTopos->vols[v];
		for (c=0; c<tv->nChunks; c++){
			const struct tTopoOfRaid0Chunk*tc = &tv->chunks[c];
			for (r=0; r<tc->stripeWidth; r++){
				const struct tTopoOfPraid *tr = &tc->raids[r];
				for (s=0; s<tr->header.n_segments; s++) {
					if (!strncmp(uuid_str, tr->s[s].uuid, sizeofUUID))
						break;
				}
				if (s<tr->header.n_segments) {
					if (tv_ptr) *tv_ptr = tv;
					if (segInd) *segInd = s;
					pthread_mutex_unlock(gTopos_lock);
					return tr;
				}
			}
		}
	}

	pthread_mutex_unlock(gTopos_lock);
	return NULL;
}

const struct tTopoOfPraid* tTopoOfNVMesh_find_r1_by_praid_uuid(const struct tTopoOfNVMesh *gTopos, const char* uuid_str, const struct tTopoOfVolume **tv_ptr) {
	const int sizeofUUID = 16 + 0*NVMEIB_GID_STR_MAX;			// No need to compare the entire uuid, first 16 bytes is enough. More-over sometimes only the first 16 bytes are given
	int err = 0;
	int v,c,r;
	pthread_mutex_t* gTopos_lock = (void*)(&gTopos->lock);
	if (tv_ptr) *tv_ptr = 0;						// Return volume only if needed

	err = pthread_mutex_lock(gTopos_lock);
	BUG_ON(err);

	for (v = 0; v < gTopos->nVolumes; v++) {
		const struct tTopoOfVolume *tv = &gTopos->vols[v];
		for (c=0; c<tv->nChunks; c++){
			const struct tTopoOfRaid0Chunk*tc = &tv->chunks[c];
			for (r=0; r<tc->stripeWidth; r++){
				const struct tTopoOfPraid *tr = &tc->raids[r];
				if (!strncmp(uuid_str, tr->header.uuid, sizeofUUID)) {
					if (tv_ptr) *tv_ptr = tv;
					pthread_mutex_unlock(gTopos_lock);
					return tr;
				}
			}
		}
	}
	pthread_mutex_unlock(gTopos_lock);
	return NULL;
}

const struct tTopoOfVolume *tTopoOfNVMesh_find_vol_by_praid(const struct tTopoOfNVMesh *gTopos, const struct tTopoOfPraid *pr) {
	int v,c,r;
	pthread_mutex_t* gTopos_lock = (void*)(&gTopos->lock);
	int const err = pthread_mutex_lock(gTopos_lock);
	BUG_ON(err);

	for (v = 0; v < gTopos->nVolumes; v++) {
		const struct tTopoOfVolume *tv = &gTopos->vols[v];
		for (c=0; c<tv->nChunks; c++){
			const struct tTopoOfRaid0Chunk*tc = &tv->chunks[c];
			for (r=0; r<tc->stripeWidth; r++){
				if (&tc->raids[r] == pr) {
					pthread_mutex_unlock(gTopos_lock);
					return tv;
				}
			}
		}
	}
	pthread_mutex_unlock(gTopos_lock);
	return NULL;
}

struct tTopoOfVolume* tTopoOfNVMesh_find_carrier_by_uuid(const struct tTopoOfNVMesh *gTopos, const char* uuid) {
	int sizeofUUID = sizeof(gTopos->vols->chunks->raids->header.uuid), v;

	pthread_mutex_t* gTopos_lock = (void*)(&gTopos->lock);
	int const err = pthread_mutex_lock(gTopos_lock);
	BUG_ON(err);

	for (v = 0; v < gTopos->nVolumes; v++) {
		const struct tTopoOfVolume *tv = &gTopos->vols[v];
		if (strncmp(uuid, tv->uuid, sizeofUUID) == 0){
			pthread_mutex_unlock(gTopos_lock);
			return (struct tTopoOfVolume *)tv;
		}
	}

	pthread_mutex_unlock(gTopos_lock);
	return NULL;

}

struct disk_sgmnts tTopoOfNVMesh_list_disk_sgmnts(const struct tTopoOfNVMesh* gtopo, u32 disk_id, bool ec_sgmnts_only) {
	int last_insert_pos = -1;
	struct disk_sgmnts result = {0};
	pthread_mutex_t* gtopo_lock = (void*)(&gtopo->lock);
	int const err = pthread_mutex_lock(gtopo_lock);
	BUG_ON(err);

	for (int v_idx = 0; v_idx < gtopo->nVolumes; ++v_idx){
		const struct tTopoOfVolume *volume = &gtopo->vols[v_idx];
		for (int sgmnt_idx = 0; sgmnt_idx < volume->n_segs; ++sgmnt_idx){
			const struct disk_range *sgmnt = &volume->segs[sgmnt_idx];
			if (sgmnt->node_id == disk_id){
				//we have 1:1 mapping between node_id and disk_id
				if (ec_sgmnts_only && (sgmnt->slice_size == 1)){
					continue;
				}
				result.sgmnts[++last_insert_pos] = sgmnt;
				BUG_ON(last_insert_pos == ARRAY_SIZE(result.sgmnts));
			}
		}
	}
	pthread_mutex_unlock(gtopo_lock);
	return result;
}

/*****************************************************************************/
// EOF.

