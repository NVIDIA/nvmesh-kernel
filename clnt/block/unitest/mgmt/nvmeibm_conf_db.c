/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibm_conf_db.h"
#include "./toma/nvmeibt_topology_simu.h"
#include "../uni_framework/unitest_defs.h"
#include "nvmesh_sim.h"

/******************************************************************************/
/**
 * Segment UUID explained:
 * Each segment UUID in simulator is real valid UUID of the following form:
 * a004d006-0034-0000-0000-00000000000X
 * a004 - Means semgment #4 (zero based index of array)
 * d006 - Means located on disk #6 (zero based index of array)
 * 0034 - Ever growing number. Must change on each segment relocation to ensure UUID uniqueness.
 * X    - Status: 0 - normal, 1 - destroyed, 2 - deprecated
 *
 * The remaining 20 bytes are currently unused.
 * Note: if changing format, make sure to keep it compliant with UUID standard,
 * i.e it must be a value of format 8-4-4-4-12, where each character is a hexadecimal
 * digit [0 .. f]
 */

struct ruuid_info{
	u32 segment; //disk range unique id in the management database. When seg is deleted and recreated in the same place (disk, dlba), its id is changed
	u32 disk; //every node has exactly one disk; so the simulator refer to node id and disk id as synonyms
			  //adding more more disk to the node will blow up
	u32 uniq; // Unique id, ever growing on each reconf. This forces segment uuid to change on segment relocation.
}; //0, 1, ∞ principle

#define RUUID_DATA_LENGTH (36)
#define SEG_UUID_PATTERN "a%03ud%03u-%04u-0000-0000-000000000000"
#define RUUID_MAX_SEG_DISK (999)		// Printed via %03u
static inline void ruuid_format(char ruuid[40], struct ruuid_info info){
	int printed = 0;
	BUG_ON((RUUID_MAX_SEG_DISK < info.segment) || (RUUID_MAX_SEG_DISK < info.disk));
	printed = snprintf(ruuid, 40, SEG_UUID_PATTERN, info.segment, info.disk, info.uniq);
	BUG_ON(RUUID_DATA_LENGTH != printed);
	memset(ruuid+printed, 0, 40-printed);
}

static inline struct ruuid_info ruuid_parse(const char ruuid[40]){
	struct ruuid_info info = {0};
	const int error = sscanf(ruuid, SEG_UUID_PATTERN, &info.segment, &info.disk, &info.uniq);
	BUG_ON(error == EOF);
	return info;
}

static inline void ruuid_destroy(char ruuid[40]){
	strcpy(&ruuid[RUUID_DATA_LENGTH-1], "1");
}


static inline void ruuid_deprecate(char ruuid[40]){
	strcpy(&ruuid[RUUID_DATA_LENGTH-1], "2");
}

static inline void ruuid_move_mangled(char ruuid[40], u32 to_disk){
	struct ruuid_info tmp = ruuid_parse(ruuid);
	BUG_ON(RUUID_MAX_SEG_DISK < to_disk);
	tmp.disk = to_disk;
	++tmp.uniq; // Make sure sgmnt ID changes after move
	ruuid_format(ruuid, tmp);
}
/******************************************************************************/
void disk_range_init(struct mongo_db_simu *mdb, struct disk_range*R, u64 bd_start, int node_i, int stripe_i, int chunk_i, int vol_i, int rep, int wid, int stripe_size, int seg_len, int slice_size, bool allow_invalid_cfg){
	struct mdb_target_conf *targ = &mdb->srvrs[node_i];
	extern struct NVMeshSystem* g_sys;
	struct ramDiskSimulator *ramDisk = &g_sys->servers[node_i].ramDisk;
	stripe_size *= LOCKSET_SLICES;						// Counts in units of locks (of 4Kblocks)
	R->node_id = node_i;
	R->bd_start = bd_start;                             // first block within the north-bound volume (block device address)
	R->dlba_start = (u64)targ->disk_alloc_end; 			// first blocks within the underlying device (physical disk address)
	R->length = stripe_size*seg_len;					// Assign memory from this disk
	R->replicas	= rep;
	R->stripe_width	= wid;
	R->stripe_size = stripe_size;
	R->stripe_index	= stripe_i;
	R->chunk_index = chunk_i;
	R->volume_index = vol_i;
	R->slice_size = slice_size;
	ruuid_format(R->ruuid, (struct ruuid_info){.segment=mdb->disk_range_uniqueID, .disk=R->node_id, .uniq=0});
	if (slice_size > 1) { // Set EC RAM
		ramDiskSimulator_reset_txid_range(ramDisk, R->dlba_start, seg_len);
	}
	mdb->disk_range_uniqueID++;                                 // Keep the id unique
	targ->disk_alloc_end += R->length;					// Mark the physical area on the disk as allocated
	if (allow_invalid_cfg == false) {
		WARN_ON((u64)targ->disk_alloc_end > (ramDisk->committed_addr.block + __bytesTo4K(ramDisk->data_seg_size)));
	}
}

#define PRAID_UUID_PATTERN "f%03uc%03u-b%03u-0000-0000-000000000000"

void disk_range_format_praid_uuid(struct disk_range *seg, char *praid_uuid)
{
	u32 praid_in_chunk_index =  seg->stripe_index / seg->replicas;
	int printed = snprintf(praid_uuid, 40, PRAID_UUID_PATTERN, seg->volume_index, seg->chunk_index, praid_in_chunk_index);
	BUG_ON(RUUID_DATA_LENGTH != printed);
	memset(praid_uuid + printed, 0, 40-printed);
}

void disk_range_copy(struct disk_range *R, const struct disk_range *src){
	*R	= *src;
}

u32 disk_range_get_disk_by_uuid(const char* ruuid, u32 *optional_seg_ind_rv) {
	struct ruuid_info ri = ruuid_parse(ruuid);
	if (optional_seg_ind_rv)
		*optional_seg_ind_rv = ri.segment;
	return ri.disk;
}

void disk_range_destroy(struct disk_range*R){
	ruuid_destroy(R->ruuid);
}

void nvmeibc_block_disk_destroy(struct nvmeibc_block_disk*D){
	D->ranges   			= NULL;				// Should already be freed by previous code
}

void nvmeibc_block_disk_init(struct nvmeibc_block_disk*D){
	memset(D,0,sizeof(*D));
}

void volumeDescriptor_free(struct volumeDescriptor* vol){
	int i;
	for (i=0; i<vol->nSegments; i++)
		disk_range_destroy(&vol->segs[i]);
	sim_kfree(vol->segs);
	vol->segs = NULL;
}

void mdb_target_conf_init(struct mdb_target_conf* c, int i, const char* disk_name) {
	extern struct NVMeshSystem* g_sys;
	struct ramDiskSimulator *ramDisk = &g_sys->servers[i].ramDisk;
	sprintf(c->rnic_guid, "0xface%02dacac", i);		// Inialize unique GUID    (hex number) ArniC
	sprintf(c->node_id  , "0xface%02ddede", i);		// Inialize unique node id (hex number) noDE
	c->disk_name = disk_name;						// Pointer only, keep disk name allocated in 1 place only
	c->disk_alloc_end = ramDisk->committed_addr.block;	// Disk is completely empty and not used
}

/******************************************************************************/
#include "nvmeibc_simu_disk.h"

static void __vol_info_init(struct nvmeibc_volume_header *info, int v, enum nvmeibc_config_volume_type type) {
	if (type == NORMAL_VOLUME) {
		sprintf(info->devname, "Thik_NAME_%02d", v);
		sprintf(info->uuid,    "Thik_UUID_%02d", v); // Number at the end is crucial. Don't touch! gendisk is found using this number
		info->version = v + 30;						 // Volumes start from arbitrary different versions
	}
	info->type = type;
}

#define nvmeibc_locks_scheme_create(ls) ({ \
	(ls)->type =         OWNER_SCHEME_SL_START_DEC_C; \
	(ls)->maxNOwners =   N_MAX_RAID_LOCKS; \
	(ls)->locksetShift = -1;     /* unused */ \
})

enum nvmeibc_config_volume_type mongo_db_simu_is_thick_vol(const char* vol_uuid) {
	if (vol_uuid[3] == 'k') return NORMAL_VOLUME;
	BUG();                  return UNKNOWN_ILLEGAL;
}

int mongo_db_simu_get_thick_vol_ind(const char* vol_uuid) {
	BUG_ON(!isdigit(vol_uuid[11]));
	return (int)(vol_uuid[11]-'0');									// Faster than do strcmp in a loop
}

int mongo_db_simu_get_mt_vol_ind(const char* vol_uuid) {
	return mongo_db_simu_get_thick_vol_ind(vol_uuid);
}

static inline void __init_mgmt_vat(struct nvmeibc_volume_attach_t *vat) {
	nvmeibc_volume_attach_t_init(vat);
	vat->res.version = 1; // MGMT will always hold 1, 0 is recoverer
	vat->res.mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RW;	// New resrevation logic default RW mode, sent to client and stored in mgmt DB
}

void volumeDescriptor_ref_ids_generate(struct volumeDescriptor* vol) {
	const int max_refs = ARRAY_SIZE(vol->referenceIDs);
	const int ref_id_name_len = (int)sizeof(vol->referenceIDs->val);
	const char *debug_suffix = &vol->info.uuid[8]; 		// Just a part of uuid (without a prefix) so developer can easily realte ref_id to volume
	int i;
	vol->attachmentVersion++;
	vol->n_ref_ids = vol->attachmentVersion % (max_refs+1);	// Change the amount of n_refs, sometimes grow, sometimes decrease
	//pr_emerg("%s: ++attach version=%u\n", vol->info.devname, vol->attachmentVersion);
	for (i = 0; i < vol->n_ref_ids; i++) {
		snprintf(&vol->referenceIDs[0].val[0], ref_id_name_len, "ref%02u_ver%02u_%.*sXXXXYYYYY123456789", i, vol->attachmentVersion, 10, debug_suffix);
	}
}

void volumeDescriptor_ref_ids_verify(const struct volumeDescriptor* vol, const struct nvmeibc_volume_status_payload *msg) {
	int i;
	if (vol->expect_clnt_ref_ids_to_not_match_mongodb)
		return;
	BUG_ON(vol->n_ref_ids != msg->n_ref_ids);
	for (i = 0; i < vol->n_ref_ids; i++) {
		BUG_ON(strcmp(vol->referenceIDs[i].val, msg->referenceIDs[i].val));
	}
}

void mongo_db_simu_alloc_volumes(struct mongo_db_simu *mdb, struct nvmeibc_disk *physDiscs) {
	int c, v, i, s;
	const int n_servers = NVMESH_N_PHYS_DISKS;
	// index I describes volI
	const int __nChunks[MAX_VOLUMES_IN_NVMESH] = {1,5,2,2,/*QLC*/ 1,1,/*MTV*/0,0};	// Amount of chunks
	const int __nSegs[  MAX_VOLUMES_IN_NVMESH] = {6,5,4,4,/*QLC*/10,6,/*MTV*/0,0};	// Amount of segments
	const int __nMirror[MAX_VOLUMES_IN_NVMESH] = {2,1,1,2,/*QLC*/10,6,/*MTV*/0,0};	// Mirroring level
	const int __nStrWid[MAX_VOLUMES_IN_NVMESH] = {3,1,2,1,/*QLC*/ 1,1,/*MTV*/0,0};	// Stripe width
	const int __nSlices[MAX_VOLUMES_IN_NVMESH] = {1,1,1,1,/*QLC*/ 8,4,/*MTV*/0,0};	// Slice size
	const int __nSnakes[MAX_VOLUMES_IN_NVMESH] = {1,1,1,1,/*QLC*/ 4,2,/*MTV*/0,0};	// Snake size (8 is not viable for 8+2 only 6+2) can test 1/2/4 (1 was thoroughly tested previously)

	for (i=0; i<n_servers; i++){
		mdb_target_conf_init(&mdb->srvrs[i], i, &physDiscs[i].name[0]);	// Pointer only, keep disk name allocated in 1 place only
		for (v=0; v<mdb->nVols; v++)  								// Map physical disks to block devices. For simplicity map even disks which are not used
			nvmeibc_block_disk_init(&mdb->discs[v][i]);
	}
	mdb->disk_range_uniqueID = 0;									// Restart enunmeration of the segments
	mdb->nVols = MAX_NORMAL_VOLUMES_IN_NVMESH;
	for (v=0; v<mdb->nVols; v++) {
		struct volumeDescriptor *vol = &mdb->vols[v];
		enum nvmeibc_config_volume_type type = NORMAL_VOLUME;
		vol->links = NULL;
		vol->nVols = 1;
		__vol_info_init(&vol->info, v, type);	// Normal or QLC configuration
		nvmeibc_locks_scheme_create(&vol->locks_scheme);
		vol->nChunks = __nChunks[v];
		vol->nSegments = __nSegs[v];
		vol->snake_size = __nSnakes[v];
		__init_mgmt_vat(&vol->vat);
		BUG_ON(vol->nSegments > NVMESH_MAX_SEG_PER_VOLUME);
		vol->segs = (struct disk_range*)sim_kmalloc(sizeof(*vol->segs) * NVMESH_MAX_SEG_PER_VOLUME, GFP_KERNEL);
		switch (v) {
			case 0:
				for (i=0; i<vol->nSegments; i++)
					disk_range_init(mdb, &vol->segs[i], 0, i, i, 0, v, __nMirror[v], __nStrWid[v], 1, RAMDISK_DATA_LOCK_SIZE/2, __nSlices[v], false);
				// Segments are already sorted by disks since each disk has only 1 segment
				break;
			case 1: {
				int _segLens[5] = {3,2,1,RAMDISK_DATA_LOCK_SIZE/8,1};
				int _disks[  5] = {0,1,0,1                       ,1};
				u64 bd_start = 0;
				for (i=0; i<vol->nSegments; i++){
					disk_range_init(mdb, &vol->segs[i], bd_start, _disks[i], 0, i, v, __nMirror[v], __nStrWid[v], 1, _segLens[i], __nSlices[v], false);
					bd_start += vol->segs[i].length;
				}
				swap(vol->segs[1], vol->segs[2]); // Sort segments by disks. Artificial ugly hack to simplify configfs emulation. Switch segment 1 and 2 places.
				swap(vol->segs[1].chunk_index, vol->segs[2].chunk_index);
				break;
			}
			case 2:
			case 3:{
				u64 bd_start = 0, bdStartOfCurChunk = 0;
				for (i=0; i<vol->nSegments; i++){
					const bool newChunk = (i+1)%2;							// Each chunk has 2 segments
					const int chunkIndex = i / 2;
					if (newChunk)
						bdStartOfCurChunk = bd_start/__nMirror[v];			// Mirroring segments back up each other without advancing the address space
					disk_range_init(mdb, &vol->segs[i], bdStartOfCurChunk, i+2, (newChunk?0:1), chunkIndex, v, __nMirror[v], __nStrWid[v], 1, RAMDISK_DATA_LOCK_SIZE/4, __nSlices[v], false);
					bd_start += vol->segs[i].length;
				}
				// Segments are already sorted by disks since each disk has only 1 segment
				break;
			}
			case 4:{	// First QLC
				for (i=0; i<vol->nSegments; i++)										// Will resize according to MDV later
					disk_range_init(mdb, &vol->segs[i], 0, i+8, i, 0, v, __nMirror[v], __nStrWid[v], 1, RAMDISK_DATA_LOCK_SIZE, __nSlices[v], false);
				vol->enable_crc_check = true;
				break;
			}
			case 5:{	// Second QLC
				for (i=0; i<vol->nSegments; i++)										// Will resize according to MDV later
					disk_range_init(mdb, &vol->segs[i], 0, i+18, i, 0, v, __nMirror[v], __nStrWid[v], 1, RAMDISK_DATA_LOCK_SIZE, __nSlices[v], false);
				vol->enable_crc_check = true;
				break;
			}
			default:
				BUG();
		}
		vol->use_debug_di = false; // Default is false for all volumes, Each unitest can set it to true at its will
	}

	for (v=0; v<mdb->nVols; v++){ 								// Map volume segments to physical discs
		struct volumeDescriptor *vol = &mdb->vols[v];
		//_ND_dmesg(t_simuav12, "Name=@STR, uuid=@STR", vol->info.devname, vol->info.uuid);
		for (s=0; s<vol->nSegments; s++){
			//_ND_dmesg(t_simuav13, "\t[@INT]uuid=@STR", i, vol->segs[s].ruuid);
			const int ni = vol->segs[s].node_id;
				mdb->discs[v][ni].n_ranges++;				// Increase the amount of segments for this disk
			if (mdb->discs[v][ni].ranges == NULL)
				mdb->discs[v][ni].ranges = &vol->segs[s];	// Start of the array of segments for this disk
		}
		if (v < MAX_NORMAL_VOLUMES_IN_NVMESH)
			vol->nextCmd = volCmds_New;                     // All thick volumes should be auto attached.
		else
			vol->nextCmd = volCmds_Illegal;                 // All QLC volumes should not attach yet
	}

	// Arbitrary decision, volumes 0,3 will have ref-ids, others dont
	for (v=0; v<mdb->nVols; v++) {
		mdb->vols[v].attachmentVersion = 1;
		for (c = 0; c < NVMESH_N_MAX_CLIENTS; c++) {
			mdb->vol_is_attached[c][v] = false;
			mdb->vol_attached_type[c][v] = mdb->vols[v].info.type;
		}
	}
	volumeDescriptor_ref_ids_generate(&mdb->vols[0]);
	volumeDescriptor_ref_ids_generate(&mdb->vols[3]);
}

void mongo_db_simu_delet_volumes(struct mongo_db_simu *mdb) {
	int i, v;
	const int n_servers = NVMESH_N_PHYS_DISKS;
	mdb->disk_range_uniqueID = 0;									// Restart enunmeration of the segments
	for (i=0; i<n_servers; i++)
	for (v=0; v<mdb->nVols; v++)
		nvmeibc_block_disk_destroy(&mdb->discs[v][i]);			// Clear vol-disk connectivity matrix.

	for (v=0; v<mdb->nVols; v++) {
		int c;
		for (c=0; c<NVMESH_N_MAX_CLIENTS; c++)
			BUG_ON(mdb->vol_is_attached[c][v]);						// Cannot delete volume while client is still attached
		volumeDescriptor_free(&mdb->vols[v]);					// Undo: Simulate sys-admins work to destroy volumes
	}

	for (v=0; v<mdb->nMTVolumes; v++) {
		sim_kfree(mdb->mtvols[v].links);
	}
}

int mongo_db_simu_get_num_vol_on_disk(struct mongo_db_simu *mdb, int disk) { // Daniel Todo: Insert a correct implementation with real search
	(void)mdb;
	switch (disk) {
	case 0: case 1:	                    return  2;
	case 2: case 3: case 4:	case 5:	    return  3;
	default:  BUG_NOT_IMPLEMENTED_YET;	return -1;
	}
}

void mongo_db_simu_update_clnt_vol_attachment(struct mongo_db_simu *mdb, int inst_id, const struct nvmeibc_volume_status_payload *msg, bool is_attached) {
	const int v = mongo_db_simu_get_thick_vol_ind(&msg->uuid[0]);
	struct volumeDescriptor *vol = &mdb->vols[v];
	mdb->vol_is_attached[inst_id][v] = is_attached;
	if (is_attached) {
		enum nvmeibc_config_volume_type attached_type = vol->info.type;

		/* Upstream status does not carry a raw volume type, so recover it from
		 * the attached-state semantics that the real product preserves.
		 */
		if (msg->is_hidden) {
			attached_type = (msg->reservation.version == RESERVATION_MODE_IRRELEVANT) ?
				RECOVERER_VOLUME : SHADOW_VOLUME;
		}
		mdb->vol_attached_type[inst_id][v] = attached_type;
		volumeDescriptor_ref_ids_verify(vol, msg);
	} else { // Reset mode to 0, increase RV
		mdb->vol_attached_type[inst_id][v] = vol->info.type;
		nvmeibc_volume_detach_request_update_reservation_info(&mdb->vols[v]);
	}
}

void mongo_db_simu_reconf_set_depricate(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, int v, int n_segs, struct disk_range *dp_seg[/*n_segs*/], int n_disks, u32 dp_diskids[/*n_disks*/]) {
	struct volumeDescriptor   *vol    = &mdb->vols[v];
	struct tTopoOfVolume      *vol_cf = &tcf->vols[v];
	struct nvmeibc_block_disk *discs  = mdb->discs[v];
	int i;
	for (i=0; i<NVMESH_N_PHYS_DISKS; i++) { 			/* Undo all previous deprecated disks */
		if (discs[i].n_ranges < 0)
			discs[i].n_ranges = 0;
	}
	if (dp_diskids){
		for (i=0; i<n_disks; i++)
			discs[dp_diskids[i]].n_ranges = -1;	/* Mark requested deprecated disks */
	}

	if (vol->nDepreSegments){ 						/* Undo previous deprecated segments */
		for (i=0; i<vol->nDepreSegments; i++)
			disk_range_destroy(&vol->depre_segs[i]);
		vol->nDepreSegments=0;
		sim_kfree(vol->depre_segs);
		vol->depre_segs = NULL;
	}
	if (dp_seg) {								/* Mark requested deprecated segments */
		vol->depre_segs = (struct disk_range*)sim_kmalloc(sizeof(*vol->depre_segs)*n_segs, GFP_KERNEL);
		for (i=0; i<n_segs; i++)
			disk_range_copy(&vol->depre_segs[i], dp_seg[i]);
		vol->nDepreSegments+=n_segs;
	}

	__unitest_volume_config_version_inc(vol, vol_cf);/* Auto increase the version of configuration */
}

void mongo_db_simu_reconf_cleanup(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, int v) {
	mongo_db_simu_reconf_set_depricate(mdb, tcf, v, 0, NULL, 0, NULL);
	__unitest_volume_config_version_dec(&mdb->vols[v], &tcf->vols[v]); /* Decrease version, because it was increased by mistake in the previous line */
}

void mongo_db_simu_reconf_set_new(struct nvmeibc_block_disk *disks, int srcNode, int dstNode, struct disk_range *seg, u64 dlba){
	if (srcNode>=0) {											// Segment has origin. If not, this is volume upgrade and not segment relocation.
		disks[srcNode].n_ranges--;
		disks[srcNode].ranges = NULL;							// Move the array of segments for this disk (a single segment)
	}
	if (dstNode>=0) {											// Segment has a replacement. If not, this is volume downgrade.
		disks[dstNode].n_ranges++;
		disks[dstNode].ranges = seg;
		seg->node_id = dstNode;									// Relocate to destination disk
	}															// Else. No destination. This segment is removed
	if (seg){
		BUG_ON(dlba < 24ull*(1ull << 28));
		seg->dlba_start = dlba;
	}
}

void mongo_db_simu_move_disk_range(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, struct TstPRaid* from, int dstDisk, u64 physical_offset){
//	mongo_db_simu_move_1seg(mdb, tcf, from->volume, dstDisk, &from->cpr[from->si], offset, from->tpr);
	struct disk_range* seg = &from->cpr[from->vsi.segment];
	struct NVMeshSystem *sys = container_of(tcf, struct NVMeshSystem, tcf);
	if (seg->node_id == (u32)dstDisk) {
		dstDisk = seg->node_id;
		mongo_db_simu_reconf_set_depricate(mdb, tcf, from->vsi.volume, 0, NULL, 0, NULL);
	} else {
		mongo_db_simu_reconf_set_depricate(mdb, tcf, from->vsi.volume, 1, &seg, 1, &seg->node_id);
	}
	mongo_db_simu_reconf_set_new(mdb->discs[from->vsi.volume], (int)seg->node_id, dstDisk, seg, physical_offset);
	mongo_db_simu_reconf_tell_toma(from->tpr, seg, false, tcf->vols[from->vsi.volume].locks_scheme);
	// Notify serjio
	serverSimulator_notify_new_disk_sgmnts(&sys->servers[dstDisk]);
	if (seg->node_id != (u32)dstDisk)
		serverSimulator_notify_new_disk_sgmnts(&sys->servers[seg->node_id]);

}


void __unitest_volume_config_version_inc(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf){
	vol_cf->version = ++vol->info.version;
}

void __unitest_volume_config_version_dec(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf){
	vol_cf->version = --vol->info.version;
}

void mongo_db_simu_reconf_fictious_deprec_seg(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, int v, bool should_add) {
	const struct volumeDescriptor *vol = &mdb->vols[v];
	struct disk_range *seg = &vol->segs[0];
	if (should_add) {
		u32 dep_disc = 0;
		mongo_db_simu_reconf_set_depricate(mdb, tcf, v, 1, &seg, 1, &dep_disc);
		vol->depre_segs->node_id = dep_disc;
		ruuid_deprecate(vol->depre_segs->ruuid);
	} else {
		mongo_db_simu_reconf_cleanup(mdb, tcf, v);
	}
}

s32 translate_praid2disk_range_index(const struct volumeDescriptor *vol, u8 praid_index, struct volume_segment_index* resolved_vsi){
	const struct disk_range *segment_cur = vol->segs;

	const u32 n_chunks = vol->nChunks;
	for (u32 ci = 0; ci < n_chunks; ++ci){
		u32 ri = 0;
		const u32 n_raids = min(praid_index, segment_cur->stripe_width);
		praid_index -= n_raids;
		for (; ri < n_raids; ++ri){
			const u32 n_segments = segment_cur->replicas;
			segment_cur += n_segments; //jumping to next raid
		}
		if (!praid_index){
			if (resolved_vsi){
				//resolved_vsi->volume should be initialized from the context
				resolved_vsi->chunk = ci;
				resolved_vsi->raid = ri;
				resolved_vsi->segment = 0;
			}
			return (s32)(segment_cur - vol->segs);
		}
	}
	if (resolved_vsi){
		//resolved_vsi->volume should be initialized from the context
		resolved_vsi->chunk = -1;
		resolved_vsi->raid = -1;
		resolved_vsi->segment = -1;
	}
	return -1;
}

s32 translate_segment2disk_range_index(const struct volumeDescriptor *vol, struct volume_segment_index vsi){
	s32 position = -1; // not found
	const struct disk_range *segment_cur = vol->segs;
	const struct disk_range *segment_end = vol->segs + vol->nSegments;

	{	//skip chunks
		const u32 n_chunks = vol->nChunks;
		for (u32 ci = 0; ci < n_chunks && ci != (u32)vsi.chunk; ++ci){
			const u32 n_raids = segment_cur->stripe_width;
			for (u32 ri = 0; ri < n_raids; ++ri){
				const u32 n_segments = segment_cur->replicas;
				segment_cur += n_segments; //jumping to next raid
			}
		}
		BUG_ON(segment_end <= segment_cur);
	}
	{	//skip raids within the chunk
		const u32 n_raids = segment_cur->stripe_width;
		for (u32 ri = 0; ri < n_raids && ri != (u32)vsi.raid; ++ri){
			const u32 n_segments = segment_cur->replicas;
			segment_cur += n_segments; //jumping to next raid
		}
		BUG_ON(segment_end <= segment_cur);
	}

	if ((u32)vsi.segment < segment_cur->replicas){
		position = (segment_cur + vsi.segment) - vol->segs;
	}
	return position;
}

void mongo_db_simu_cnv_to_toma_topo_vol(const struct volumeDescriptor *vol, struct tTopoOfVolume *tv, int v){
	int c, r, s;
	struct disk_range *segs = vol->segs;
	const char* segsUUIDs[N_MAX_RAID_SLICE_LEN] = {NULL};	// UUID's of all the segments in a slice
	int         segsNids[ N_MAX_RAID_SLICE_LEN] = {0};		// Disks (server nodes) where the segments reside
	tv->version		  = vol->info.version;
	tv->nChunks 	  = vol->nChunks;
	tv->locks_scheme  = &vol->locks_scheme;
	tv->segs		  = (const struct disk_range *)vol->segs;
	tv->chunks   	  = (struct tTopoOfRaid0Chunk*)sim_kmalloc(sizeof(struct tTopoOfRaid0Chunk)*tv->nChunks, GFP_KERNEL);
	for (c=0; c<tv->nChunks; c++) {
		const int uniqueValue = 100000 +(10*v+c)*100 + 1;						// Configurations start from arbitrary number in our simulator for each volume for each chunk
		const int praid_version = uniqueValue;									// starting raid1 version.
		struct tTopoOfRaid0Chunk* tc = &tv->chunks[c];
		const int nMirror = (int)segs->replicas;
		BUG_ON(N_MAX_RAID_SLICE_LEN<nMirror);									// Todo: make the define larger
		tc->stripeWidth = segs->stripe_width;
		tc->raids       = (struct tTopoOfPraid*)sim_kmalloc(sizeof(struct tTopoOfPraid)*tc->stripeWidth, GFP_KERNEL);
		for (r=0; r<tc->stripeWidth; r++, segs += nMirror){
			char praidUUID[40];
			disk_range_format_praid_uuid(segs, praidUUID);
			for (s=0; s<nMirror; s++) {
				segsUUIDs[s] =      segs[s].ruuid;
				segsNids[ s] = (int)segs[s].node_id;
			}
			tTopoOfPraid_init(&tc->raids[r], praid_version, nMirror, praidUUID, segsUUIDs, segsNids, vol->locks_scheme.type, vol->locks_scheme.maxNOwners);
		}
	}
	*((int*)&tv->n_segs) = (segs-tv->segs);										// Total amount of segments in the volume
	tv->uuid = &vol->info.uuid[0];
}

void mongo_db_simu_cnv_to_toma_topo_all(const struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf) {
	int v;
	tcf->nVolumes = mdb->nVols;
	for (v=0; v<tcf->nVolumes; v++)
		mongo_db_simu_cnv_to_toma_topo_vol(&mdb->vols[v], &tcf->vols[v], v);
}

void mongo_db_simu_reconf_tell_toma(struct tTopoOfPraid* r1, struct disk_range *seg, bool add_del, const struct nvmeibc_locks_scheme_conf *ls){
	const int stripe_index = (seg->stripe_index&0x1);			// Don't care about Raid0, only about the order in Raid1
	struct nvmeibt_client_topo_disk_segment* conf_seg = &r1->s[stripe_index];
	if (!add_del) {												// Relocating segment
		const bool is_self_owner = !strcmp(conf_seg->owners[0].seg_uuid, conf_seg->uuid);
		ruuid_move_mangled(seg->ruuid, seg->node_id);
		strcpy(conf_seg->uuid				  , seg->ruuid);	// Update raid1
		if (is_self_owner)
			strcpy(conf_seg->owners[0].seg_uuid, seg->ruuid);

		conf_seg->unused23 = seg->node_id;						//  Update the toma node id
	} else if (r1->header.n_segments == 2){ 					// Downgrading from Raid1 to non mirrored. Deleting 'seg'
		r1->header.n_segments = 1;
		if (stripe_index == 0) { 								// Replace seg[0] by seg[1] (swap them to not loose the first segment when we upgrade in future)
			swap(r1->s[0], r1->s[1]);
		}														// Else, nothing to do, seg[1] just dessapeared.
	} else {													// Upgrading non mirrored to Raid1.
		r1->header.n_segments = 2;
		if (stripe_index == 0) { 								// Replace seg[1] by seg[0]
			swap(r1->s[0], r1->s[1]);
		}
		// Convert config seg to toma topology seg
		strcpy(conf_seg->uuid, seg->ruuid);						// Update raid1. As long as we first downgrade and then upgrade the strings are equal coz we did not delete the string during downgrade
		strlcpy(conf_seg->owners[0].seg_uuid, conf_seg->uuid, sizeof(conf_seg->owners[0].seg_uuid));
		conf_seg->owners[0].mode = NVMEIBTC_DS_OWNER_MODE_PRIMARY; // Self owner
	}
	tTopoOfPraid_update_segs_by_access_mode(r1, ls);
}

// Reset the reservation information for this volume
void __unitest_volume_reservation_reset(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf) {
   nvmeibc_volume_attach_t_init(&vol->vat);
   vol->vat.res.version = 1; // MGMT always hold 1
   vol_cf->reservation_version = 0;
}

void __unitest_volume_reservation_inc(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf) {
	vol->vat.res.version++;
	vol_cf->reservation_version++;
}

void __unitest_volume_reservation_dec(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf) {
	vol->vat.res.version--;
	vol_cf->reservation_version--;
}

static int __attach_request_update_reservation_info(unsigned int *preempt_var, const enum_reservation_mode mode, u64 reservation_version, const bool is_512B_IO_allowed, struct nvmeibc_volume_attach_t *vat) {
	const bool is_preempt = (*preempt_var == NVMEIB_C_TO_M_VOLUME_PREEMPT);
	const bool is_recovery = (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC);
	if (!is_recovery && (reservation_version == 0)) { // Real attach request without reservation version -> reply with latest RV
		// Since the flow of client asks for attach with reservation_version == 0 and gets a response from mgmt with reservation_version != 0, then asks for attach again with the given reservation_version value is no longer relevant
		// This shortcut sets the volumes reservation version directly into the request and no longer has a flow of 2 messages.
		// Additionally if an EX mode is asked without a preempt flag, set the weak preempt flag, this is required for test verification and can be removed if no longer relevant.
		if (is_exclusive(mode) && !is_preempt)
			*preempt_var = NVMEIB_C_TO_M_VOLUME_WEAK_PREEMPT;
		reservation_version = vat->res.version;
		BUG_ON(reservation_version == 0);		// Wrong managment version counter. Starts from 1
	}
	if ((reservation_version > 0) && (reservation_version != vat->res.version)) {  // Version is too old (behind)
		return -EPERM; // Note: if (is_preempt) --> race caused RV++ in mgmt before we got back here - fail request
	} else if (is_preempt) { // Increase res_ver++ if necessary, set mode
		if (mode != vat->res.mode || is_exclusive(vat->res.mode))  // No need to disconnect other clients if the mode is the same
			vat->res.version++;
	} else if (is_recovery) {
		BUG_ON(reservation_version != 0);
	} else if (vat->res.mode && (is_exclusive(vat->res.mode) || (mode != vat->res.mode))) { // If held exclusively or the requested mode is incorrect return -EACCES
		return -EACCES;
	}
	// Allowed request update mode: !is_recovery. No logic required, but all configurations will send the updated value from last approved attach request
	vat->res.mode = mode;
	vat->res.is_512B_IO_allowed = is_512B_IO_allowed;
	return 0;
}

// Update the reservation info based on the request from client
// Return -ECACCES if the mode is invalid
// Retrun -EPERM if the version is older
// Otherwise update the mode and bump the reservation if we changed it or preempted it
int nvmeibc_volume_attach_request_update_reservation_info(struct volumeDescriptor *vol, unsigned int *preempt_var, enum_reservation_mode mode, u64 reservation_version, bool is_512B_IO_allowed) {
	return __attach_request_update_reservation_info(preempt_var, mode, reservation_version, is_512B_IO_allowed, &vol->vat);
}

// Detaching while holding exclusive access resets the mode and increases the version
// (TODO for simulator) However, if a different client detaches while in exclusive it should not change unless it's the same client
void nvmeibc_volume_detach_request_update_reservation_info(struct volumeDescriptor *vol) {
	unsigned int preempt_var = NVMEIB_C_TO_M_VOLUME_PREEMPT;
	#define is_same_client() (true)
	if (is_exclusive(vol->vat.res.mode) && is_same_client()) {
		__attach_request_update_reservation_info(&preempt_var, NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC, 0, false, &vol->vat);
	}
}
