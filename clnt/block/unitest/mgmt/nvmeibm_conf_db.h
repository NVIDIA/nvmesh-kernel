/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBM_MGMT_CONF_DB_H
#define NVMEIBM_MGMT_CONF_DB_H
/* Implementation of management Mongo-DB simulator */
#include "./nvmeib_common_all.h"						// Must be first include
#include "../uni_framework/unitest_defs.h"

/******************************************************************************/
#define NVMESH_N_PHYS_DISKS 				(24)			// Amount of physical disks / Tomas in the mesh
#define NVMESH_N_PHYS_DISKS_REGULAR_USE 	(6)				// Amount of physical disks used for regular testing, not including exotic segment relocation to a new disk, etc
#define NVMESH_MAX_SEG_PER_VOLUME           (24)            // the number of segments in the array pointed from the volume
#define MAX_NORMAL_VOLUMES_IN_NVMESH		(4) 			// Just an arbitrary decison to store the volumes in preallocated array.
#define MAX_VOLUMES_IN_NVMESH				(8)				// All volumes that have a bdev (MAX_NORMAL_VOLUMES_IN_NVMESH+MAX_QLC_VOLUMES_IN_NVMESH+MAX_MTV_VOLUMES_IN_NVMESH)
#define NVMESH_N_MAX_CLIENTS				(3) 			// Maximum client instances allowed.
// Mongo-db Raid Level Definitions, 4 drives
// JBOD   : stripe segments across many drives.
// Raid 0 : stripe segments on drives {0,1,2,..K}*
// Raid 1 : a.k.a mirror - paired segments contain exact copy of one another
// Raid 1+0 : stripe chunks across many drives & each one is mirrored
//
// Simulate sys-admins work to create volumes. Don't touch!!! Unitests depend on that exact structure
// user provides volume size, stripe width, segment size (multiple of 128KB)
// Note: we have a limitation that a volume must be on continuous disks: Di,Di+1,...Di+j
//       the numbers within the volume indicate the segment No.
//       each drive is 1MB size (i.e. 8*128KB)
//       volumes are allocated in the order : vol0, vol1, vol2, vol3.
//         each one is allocated from whatever is left on the drives
//       in each drive, the column is the order of locks on the drive LBA range
//
// Disks	0		1		2		3		4		5		6 (Unused)
// ====================================================================
// 	Vol0 /----------------------------------------------\ Raid 1+0	, 1 chunk	, S-Width of 3, Mirroring 2: {{0,1}, {2,3}, {4,5}},
// 		 | {0 		1}	   {2		3}	   {4		5}	| 				S-size 128KB (32 blocks). S-length is 4 (segment takes 0.5[MB]).
// 		 | {0		1}	   {2		3}	   {4		5}	|				Address space arrangment: [{0,1}..{2,3}..{4,5}..{1,0}..{3,2}..{5,4}..]
// 		 | {0		1}	   {2		3}	   {4		5}	|
// 		 | {0		1}	   {2		3}	   {4		5}	|
// 		 -----------------------------------------------/				Total size 1.5[MB]
//
// 	Vol1 /----------------------------------------------\ JBOD		, 5 chunks, each having 1 segment
// 		 | 0		1									|				Address space arrangment: [000..11..2..3..4]
// 		 | 0		1									|
// 		 | 0		3									|
// 		 | 2		4									|
// 		 \----------------------------------------------/				Total size 1.0[MB]
//
// 	Vol2 /----------------------------------------------\ Raid 0	, 2 chunks, each having S-Width of 2,
// 		 |  		 		0		1		2		3	|				S-size 128KB (32 blocks). S-length is 2 (segment takes 0.25[MB])
// 		 |  		 		0		1		2		3	|				Address space arrangment: [0..1..0..1..2..3..2..3]
// 		 \----------------------------------------------/				Total size 1.0[MB]
//
//  Vol3 /----------------------------------------------\ Raid 1	, 2 chunks, each having 2 segments (Mirroring 2: {{2,3}, {4,5}})
// 		 |  		 	   {0		1}	   {2		3}	|				segment takes 0.25[MB]
// 		 |  		 	   {0		1}	   {2		3}	|				Address space arrangment: [{00,11}..{22,33}]
// 		 \----------------------------------------------/				Total size 0.5[MB]
//
/******************************************************************************/
// When Upgrading volume vol0 to first chunk 4 mirrored, second 3 mirrored the
// Configuration becomes:
// Raid 1+0	, chunk 0, S-Width of 3, Mirroring 4 S-size 128KB. S-length is 4 (segment takes 0.5[MB]).
//            chunk 1, S-Width of 3, Mirroring 3 S-size 128KB. S-length is 8 (segment takes   1[MB]).
// Total size 3*0.5 + 3*1.0 = 4.5[MB]
// Disks	0	1	2	3	4	5	6	7	8	9	10	11	   12 13 14   15 16 17   18 19 20
// 	Vol0 /--------------------------------------------------+---------------------------------
//	0x00 | {0			 } {	5		 } { 		10	  } | {12 13 14} {15 16 17} {18 19 20}
// 	0x20 | {0	1		 } {	5	6	 } { 		10	11} | {12 13 14} {15 16 17} {18 19 20}
// 	0x40 | {0	1	2	 } {	5	6	7} { 		10	11} | {12 13 14} {15 16 17} {18 19 20}
// 	0x60 | {0	1	2	3} {	5	6	7} {8		10	11} | {12 13 14} {15 16 17} {18 19 20}
// 	0x80 | { 	1	2	3} {4		6	7} {8	9		11} | {12 13 14} {15 16 17} {18 19 20}
// 	0xA0 | {		2	3} {4			7} {8	9		  } | {12 13 14} {15 16 17} {18 19 20}
// 	0xC0 | { 		 	3} {4			 } {8	9		  } | {12 13 14} {15 16 17} {18 19 20}
// 	0xE0 | { 		 	 } {4			 } { 	9		  } | {12 13 14} {15 16 17} {18 19 20}
// 	0xFF --LLLLLL-----------LLLLLL----------LLLLLL----------+--LLLLL------LLLLL------LLLLL----   Locking segments

/******************************* EC *******************************************/
// When Upgrading volume vol0 to Raid6: 8 + 2, single chunk
// Configuration becomes:
// Total size 8*0.5 = 4[MB] (1[MB] of parities). 1024x8[b] metadata
// Disks	0	1	2	3	4	5	6	7	8	9
// 	Vol0 /----------------------------------------
//	0x00 | {0			    	5		     	 }
// 	0x20 | {0	1		    	5	6	     	 }
// 	0x40 | {0	1	2	    	5	6	7    	 }
// 	0x60 | {0	1	2	3   	5	6	7   8	 }
// 	0x80 | { 	1	2	3   4		6	7   8	9}
// 	0xA0 | {		2	3   4			7   8	9}
// 	0xC0 | { 		 	3   4			    8	9}
// 	0xE0 | { 		 	    4			     	9}

/************************* Segment: Range on disk *****************************/
struct disk_range {   				                    // Define a segment (tange of blocks on a physical disk), which is used to implement a block device.
	//segment fields
	//struct disk_range_info {
		u64 dlba_start;                                     // Offset of the range from the beginning of the physical disk in units of blocks (range mapped from those adresses)
		u32 node_id;										// On which node this segment resides. Todo: remove.
	//};
	u32 stripe_index;                                   // the index of the "current" segment within the chunk stripe width (K). mirror:([0..(2K-1)] ,[0..(2k-1)] non-mirrored
	char ruuid[40];										// For MCS we will require to save the ruuid since the message that points to the string is released
	//praid fields; they are copied between all segments
	u32 slice_size;										// 1 for non erasure coded (mirror on jbod). 2 or more number of data segments which are protected by 'p' parities
	u32 replicas;                                       // 1 for non mirrored. 2 or more for mirrored
	//chunk fields; also copied
	u32 bd_start;                                       // Offset of the range from the beginning of the block device  in units of blocks (range mapped to   those adresses). all stripe members share the value of the first member.
	u32 length;                                         // Length of the range in units of blocks
	u32 stripe_width;                                   // The amount of disks on which which data is interlaced. (RAID 0)
	u32 stripe_size;                                    // Amount of sequential data in each stripe.
	//fields used for praid uuid generation, in addition to stripe_index above
	u32 chunk_index;									// index of this segment's chunk in volume
	u32 volume_index;
};

#define __disk_range_get_num_parities(R) ((R)->replicas - (R)->slice_size)
#define __disk_range_segs_in_chunk(   R) ((R)->replicas * (R)->stripe_width)

//struct disk_range represents a disk range in use by some raid (volume), segment relocation tests have a need to move segment from one location to another one since our simulator universe maps 1:1 between disk and server it is enough to specify the node_id and offset as the segment destination
struct disk_range_info {
	u64 dlba_start;                                      // Offset of the range from the beginning of the physical disk in units of blocks (range mapped from those adresses)
	u32 node_id;										// On which node this segment resides. Todo: remove.
};

struct mongo_db_simu;
void disk_range_init(struct mongo_db_simu *mdb, struct disk_range*R, u64 bd_start, int disk_i, int stripe_i, int chunk_i, int vol_i, int rep, int wid, int stripe_size, int len, int slice_size, bool allow_invalid_cfg);
void disk_range_copy(   struct disk_range*R, const struct disk_range* src);
u32  disk_range_get_disk_by_uuid(const char* ruuid, u32 *optional_seg_ind_r);
void disk_range_destroy(struct disk_range*R);
void disk_range_format_praid_uuid(struct disk_range *seg, char *praid_uuid);

#define disk_range_get_start_addr(s) \
	(s->bd_start + (u64)s->stripe_size * (s->stripe_index/s->replicas))

struct nvmeibc_block_disk {
    int n_ranges;                         // The number of ranges of disk
    struct disk_range *ranges;            // Array of ranges
};
void nvmeibc_block_disk_init(   struct nvmeibc_block_disk*D);
void nvmeibc_block_disk_destroy(struct nvmeibc_block_disk*D);

#include "main/cc_api/nvmeibc_main_capi_manipulate_vols.h"  // For nvmeibc_config_volume
#include "autogen/clnt/nvmeibc_mcs_stub.h"
enum volumeCommands{										// Commands which can be given to the .ko driver regarding each volumes
	volCmds_Illegal 		= 0,							// Artificial illegal command
	volCmds_New				,								// Default action is attach when everything is memset() to zero
	volCmds_RecoveryAttach	,								// recovery attach, speacial case where toma wishes to attach volume for recovery
	volCmds_HiddenAttach	,								// hidden attach, no OS registration
	volCmds_ShadowAttach	,								// shadow attach, no report
	volCmds_Update			,								// Update already attached volume
	volCmds_RecoveryUpdate	,								// Update already attached recovery volume
	volCmds_Detach			,
	volCmds_Delete			,								// Delete volume
	volCmds_ForceDetach		,								// Force detach (unsafe detach)
	volCmds_ForceRecoveryDetach,							// Force recovery detach (Used by toma)
	volCmds_ForceHiddenDetach,								// Force hidden detach
	volCmds_RecoveryDetach	,								// Detach only recovery volumes
	volCmds_HiddenDetach	,								// Detach only hidden volumes
	volCmds_DetachUpgrade	,								// Detach hidden / recovery volumes, abandon non hidden / recovery volumes
	volCmds_DetachUpgradeHiddenRecoveryForce,				// Detach hidden + recovery + force + upgrade.
	volCmds_DetachUpgradeRecoveryForce,						// Detach recovery + force + upgrade.
	volCmds_DetachUpgradeHiddenForce,						// Detach hidden + force + upgrade.
	volCmds_AttachReadOnly	,								// Read only reservation
	volCmds_AttachExclusive									// Exclusive mode reservation
};

struct volumeDescriptor { 									// A single volume/block device as defined by the sys-admin through the management and given to client through configfs
	struct nvmeibc_volume_header info;						// General info created by the management (Name, ID, etc).
	struct nvmeibc_volume_attach_t vat;						// Stores current reservation version and mode used by "any" client populates the same fields in the message
	enum volumeCommands nextCmd;							// The action .ko driver has to take when dealing with this volume
	int nVols;												// The number of underlying volumes, Always 1 unless we are part of an MTV
	struct volumeDescriptor **links;						// Any linked volumes requied to attach along with this volume

	// Below: special fields of regular volume
	struct nvmeibc_locks_scheme_conf locks_scheme;			// Why locking type to use to syncronize all clients
	int nSegments;											// Amount of segments on the disks (each physical disk may contribute 0,1, or more segments)
	struct disk_range *segs;								// Array of segments (defined by the sys-admin). Define the reservation of addresses on disks, type of mirroring, etc
	int nDepreSegments;										// Amount of segments witch are being relocated to a newer location. Each deprecated segment has a replacement in the list of 'segs'
	struct disk_range *depre_segs;							// Used when hot swapping a disk (relocating segment of volume from one disk to another)
	int nChunks;											// Number of chunks of the volume
	int snake_size;											// Mgmt configuration value for EC and QLC, snake size cannot be changed - for EC must coinside with n_slice_j
	bool enable_crc_check;									// Mgmt configuration value for EC and Mirror (default EC is true, Mirror is false)
	bool use_debug_di;										// For QA to allow debug di to be set upon creation
	bool enable_local_read_optimization;					// Mgmt configuration value for Mirror only

	// referenceID's (optional)
	int attachmentVersion;
	int n_ref_ids;
	struct nvmeibc_reference_id referenceIDs[3];			// Up to 3 ids
	bool expect_clnt_ref_ids_to_not_match_mongodb;			// A hack, should be removed and always be false;
};
void volumeDescriptor_free(struct volumeDescriptor* vol);	// Destructor
void volumeDescriptor_ref_ids_generate(struct volumeDescriptor* vol);

#define MTVolumeDescriptor volumeDescriptor

struct mdb_target_conf { 		 		                   	// Describes the full configuration of target/server nodes networking and storage
	char rnic_guid[GUID_SIZE];								// Connection identifier (hexa humbers). Todo: Support array of nics
	char node_id[NVMEIB_HOST_NAME_LEN];						// Unique node id
	const char*	disk_name;									// Name of a single physical disk (externally allocated to save memory). Todo: Support array of disks per target
	u64	 disk_alloc_end;									// Segments on this disk are allocated sequentially. Range [0..disk_alloc_end-1] was already allocated to volumes. Units of 4k's
	// Important: network/disks are separated to allow disk/nic migration between targets. Do not unite them!
};
void mdb_target_conf_init(struct mdb_target_conf* c, int i, const char* disk_name);		// Constructor of configuration of i'th target

/******************************************************************************/
struct mongo_db_simu {										// Full mongo database with many tables (semgnets, disks, rtarget nics etc) arranged as arrays
	int nVols, nMTVolumes;									// Number of volumes as defined by the sys-admin.
	struct volumeDescriptor   vols[MAX_VOLUMES_IN_NVMESH];	// Array  of volumes as defined by the sys-admin.
	struct MTVolumeDescriptor *mtvols; 						// Todo: Remove, jsut for simplicity pointer into the 'mtvols' array at correct offset
	struct nvmeibc_block_disk discs[MAX_VOLUMES_IN_NVMESH][NVMESH_N_PHYS_DISKS]; // Array of disk descriptors used by each volume for initialization of block devices(meta-data and pointer to a physical disk)
	struct mdb_target_conf srvrs[NVMESH_N_PHYS_DISKS];		// Array of all servers
	uint disk_range_uniqueID;								// unique ID generator for each segment
	bool vol_is_attached[NVMESH_N_MAX_CLIENTS][MAX_VOLUMES_IN_NVMESH];	// Management sees attachment of clients to volumes (can be different from what clients think, due to network problems and races). In stable situation should be identical to the point of view of the client
};

/* Simulate as if sys-admin allocates all the needed volumes for the testing environment, and stores the allocations in mongo db */
struct nvmeibc_disk;
void mongo_db_simu_alloc_volumes(struct mongo_db_simu *mdb, struct nvmeibc_disk *physDiscs);
void mongo_db_simu_delet_volumes(struct mongo_db_simu *mdb);	// Destructor of the configuration
enum nvmeibc_config_volume_type mongo_db_simu_is_thick_vol(const char* vol_uuid);	// Is this volume thick or thin
int                        mongo_db_simu_get_thick_vol_ind(const char* vol_uuid);
int                        mongo_db_simu_get_mt_vol_ind(   const char* vol_uuid);
int  mongo_db_simu_get_num_vol_on_disk(       struct mongo_db_simu *mdb, int disk);
void mongo_db_simu_update_clnt_vol_attachment(struct mongo_db_simu *mdb, int inst_id, const struct nvmeibc_volume_status_payload *msg, bool is_attached);

/* Convert sys-admin mongo-db definition of volume 'v' to Toma configuration, Daniel: Todo, this is incorrect place. Toma leader should do this conversion, not mongo-db! */
struct tTopoOfVolume; struct tTopoOfPraid; struct tTopoOfNVMesh;
void mongo_db_simu_cnv_to_toma_topo_vol(const struct volumeDescriptor *vol, struct tTopoOfVolume *tv, int v);
void mongo_db_simu_cnv_to_toma_topo_all(const struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tTopo);

// returns client volume segment index to disk_range
s32 translate_segment2disk_range_index(const struct volumeDescriptor *vol, struct volume_segment_index vsi);
s32 translate_praid2disk_range_index(  const struct volumeDescriptor *vol, u8 praid_index, struct volume_segment_index* resolved_vsi);

/********************** Configuration change mechanism ************************/
/* Move a single segment 'seg' in volume 'v' to 'dstDisk' at offset 'offset',
   Updates toma topology, pointed by 'r1' (Todo: Auto calculate it from tcf)*/
void mongo_db_simu_move_disk_range(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, struct TstPRaid* from, int dstDisk, u64 physical_offset);

/* Same as above but moving a few segments / adding segs / removing segs:
   Separated to 3 steps:
   1.  update mongo-db with deprecated segs (not active in the current conf but
       was active in the previous one)
   2. Update the disks with new segments
   3. Propagate this reconf to Toma*/
void mongo_db_simu_reconf_set_depricate(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, int v,
			int n_segs , struct disk_range *deprec_seg[/*n_segs*/],
			int n_disks,            u32 deprecDisk_ids[/*n_disks*/]);
void mongo_db_simu_reconf_set_new(struct nvmeibc_block_disk *disks, int srcDisk, int dstDisk, struct disk_range *seg, u64 dlba_start);

/* add_del= true means upgrading/downgrading the volume. Otherwise segment is just relocating */
void mongo_db_simu_reconf_tell_toma(struct tTopoOfPraid* r1, struct disk_range *seg, bool add_del, const struct nvmeibc_locks_scheme_conf *ls);

/* Clear remaining data from previous reconfigurations */
void mongo_db_simu_reconf_cleanup(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, int v);

/* Add/remove unrealistic unreal deprecated segment to test that client can initialize from such configuration*/
void mongo_db_simu_reconf_fictious_deprec_seg(struct mongo_db_simu *mdb, struct tTopoOfNVMesh *tcf, int v, bool should_add);

void __unitest_volume_config_version_inc(struct volumeDescriptor *vol, struct tTopoOfVolume *tv);
void __unitest_volume_config_version_dec(struct volumeDescriptor *vol, struct tTopoOfVolume *tv);

/************************** Reservation *************************************/
#define is_exclusive(mode) (mode == NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_EX)
void __unitest_volume_reservation_reset(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf);
void __unitest_volume_reservation_inc(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf);
void __unitest_volume_reservation_dec(struct volumeDescriptor *vol, struct tTopoOfVolume *vol_cf);

int nvmeibc_volume_attach_request_update_reservation_info(struct volumeDescriptor *vol, unsigned int *preempt_var, enum_reservation_mode mode, u64 reservation_version, bool is_512B_IO_allowed);
void nvmeibc_volume_detach_request_update_reservation_info(struct volumeDescriptor *vol);
// Todo: move NVMeshSystem_DiskReappearEvent to here

enum { MDV_INDEX = 0, QLC_INDEX, WCV_INDEX, MTV_INDEX, MAX_N_VOLUME_NAMES_FOR_MTV};	// Order of links is important

#endif // NVMEIBM_MGMT_CONF_DB_H
