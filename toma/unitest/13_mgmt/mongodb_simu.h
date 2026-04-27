/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements mongo-db which store volumes / cluster configuration and reported states, by Tomas */
#include "../sandbox_util.h"
#include "../../../autogen/clnt/nvmeibc_mcs_stub.h"	// Client simulator - report to mgmt simulator
#define SB_CLUSTER_CONF_N_NODES_TOTAL (3)			// Cluster of 3 machines, 1 live followed by 2 simulated other tomas, presented as nodes n37, n38, n39
#define SB_CLUSTER_CONF_MAX_VOLS      (4)			// Maximum number of volumes in the cluster configuration
#define SB_CLUSTER_CONF_MAX_CHUNKS    (2)			// Maximum number of chunks in a volume, For now, 2 chunks only, Support for volume extend once
#define SB_CLUSTER_CONF_MAX_PR_IN_CH  (1)			// Maximum number of Protection raids in chunk, For now, each chunk has only 1 praid. Dont support Raid-0
#define SB_CLUSTER_CONF_MAX_PR_SEGS   (4)			// Maximum number of segments in protection raid. Up to R1-3Mirror+1seg for replacement, for now
#define SB_CLUSTER_CONF_N_CLNTS_TOTAL (3)			// Each Node can be a client. Either local to live toma or remote
struct sb_cluster_conf {
	struct sb_node_conf {
		char hostname[64];		// Easily recognizable host name
		uint32_t uuid;			// uuid of this node
		struct sb_nics_conf {
			const char *protocol;
			uint32_t uuid;
		} nics[2];				// Each node has exactly 2 nics
		struct sb_disk_conf {	// Each node has up to 3 local disks, represents what management knows
			// --------------- disk config
			char serial[16];							// Unique for each disk
			uint32_t uuid;
			struct sandbox_nvme_device *local_nvme;		// Direct pointer to local nvme configuration, for verification that Toma reported correctly the disk to mgmt
			uint32_t size_bytes;
			u16 num_blocks;								// In blocks
			u16 block_size;								// In bytes
			u16 metadata_size;							// In bytes
			u16 vendor;
			u16 name_space_id;							// When formatted to NVMesh namespace id will change (nvmesh namespace is 1)
			bool is_out_of_service;
			// --------------- disk per client usage
			struct sb_disk_subscription {				// Each client can be attached to multiple volumes/segments on this disk (subscribed to many segments)
				int n_ref;								// ref-count
			} clnts[SB_CLUSTER_CONF_N_CLNTS_TOTAL];
		} disks[3];
		struct peer_toma_simu *peer;				// Relevant for other node only (not the live toma). Pointer to peer Toma
		struct clnt_simu      *clnt;				// Client running on this node. on 'live' node local client is mandatory for recoveries. On other nodes those are remote clients simulating attach/io's
	} nodes[SB_CLUSTER_CONF_N_NODES_TOTAL], *live, *other;	// nodes[0] is live toma, followed by 2 others
	int n_nodes;
	struct sb_volume_conf {							// All volumes configuration
		// --------------- Volume config
		const char* name;
		uint32_t uuid;								// For simplicity all uuids are u32
		uint32_t conf_version;						// Ever increasing number
		unsigned num_blocks;						// Volume size (num of 4KB blocks)
		unsigned num_chunks;						// Created with ==1
		struct sb_chunk_conf {
			uint32_t uuid;							// My chunk uuid
			unsigned vlba_start;					// Implemented volume blocks range via this chunk
			unsigned vlba_end;
			unsigned n_raids;						// Striping size (Raid-0 implementation)
			struct sb_praid_conf {
				uint32_t uuid;						// My praid uuid
				unsigned D, P;						// (data+parity) protection. R1 = (1+{1..2}), EC = ({2..8}+{1..2})
				struct sb_seg_conf {
					uint32_t disk_uuid;				// Physical disk uuid where disk segment resides, Todo consider changing to u32
					uint32_t uuid;					// My disk segment uuid
					unsigned block_start;			// Disk block address of segment start
					unsigned block_end;				// All disk segments in chunk have identical length
				} segs[SB_CLUSTER_CONF_MAX_PR_SEGS];
			} raids[SB_CLUSTER_CONF_MAX_PR_IN_CH];
		} chunks[SB_CLUSTER_CONF_MAX_CHUNKS];
		// --------------- Client reports
		struct sb_attachment_info {					// Each client can be attached to each volume
			uint32_t attachment_version;			// 0 if not attached.
			struct nvmeibc_reservation reserv;		// Todo: Use this to test enforcing reservation version attached by Toma
			bool ioEnabled;							// Client reports that its IO is enabled (after conversation with Toma).
			bool is_recovery_attach;
			struct sb_chunk_reg {							// Registration vs Live Toma on disk segments
				struct clnt_praid_reg_ctx {					// The praid client is registering due to volume attach
					struct sb_praid_topo *topo;				// Direct link to Toma topology for verification.
					uint16_t version_major;					// Major praid version given by Toma, 0 if unknown
					uint16_t is_seg_registered_bmp;			// Bitmap of registered segments, Need to register only vs live Toma, no need to talk to Simulated Toma
					uint16_t conversation_id;				// Taken from the real client, goes up on each unregister, Not mandatory, Just for easy debugging
					// Todo: Save minimal info about Toma topology to know which dbits to write
					u32 lock_id;							// Lock id given by Toma, 0 if unregistered
					u32 n_ios;								// N simulated ios to praid blocksets that were done
				} raids[  SB_CLUSTER_CONF_MAX_PR_IN_CH];
			} chunks[     SB_CLUSTER_CONF_MAX_CHUNKS];
		} clnts[          SB_CLUSTER_CONF_N_CLNTS_TOTAL];	// All possible clients (shared-RW mode), though in exclusive mode only 1 client is attached
		// --------------- Toma reports
		struct sb_chunk_topo {
			struct sb_praid_topo {
				const struct sb_praid_conf *cfg;			// Pointer to config of praid
				uint16_t version_major;
				uint16_t version_minor;
				struct sb_seg_topo {
					enum seg_topo_state { mdb_seg_UNK = 0, mdb_seg_BOOT = 'B', mdb_seg_ZERO = '0', mdb_seg_INIT = 'I', mdb_seg_CORRUPTED = '!', mdb_seg_RW = 'R', mdb_DEAD = 'D', mdb_WRITE = 'W', mdb_seg_dep='v', mdb_seg_rep='^',  } status;
					bool vitality;							// Deprecated: True = reports to leader, false = node not conencted to leader
				} segs[SB_CLUSTER_CONF_MAX_PR_SEGS];
			} raids[   SB_CLUSTER_CONF_MAX_PR_IN_CH];
		} topo_chunks[ SB_CLUSTER_CONF_MAX_CHUNKS];
	} vols[            SB_CLUSTER_CONF_MAX_VOLS];
	int n_vols;
	int zone_idx;											// All those volume exist in a specific zone
	// --------------- Toma reports
	struct sb_live_toma_reports {
		struct sb_live_toma_reports_leader {
			int expected_token;
			int reported_token;
			int reported_majority_sw_ver;
			uint32_t raftTerm;
			int n_keep_alives;								// Number of received keep alives
		} ldr;
		struct sb_live_toma_reports_follower {
			int expected_token;
			int reported_token;
			int expected_sw_ver;
			int reported_sw_ver;
			int n_keep_alives;								// Number of received keep alives
		} fol;
		struct sb_live_target_report {
			int64_t boot_time;								// reportTarget: payload.node.bootTime, A way to distinguish that Toma was restarted
			int last_reportId;
			int n_reports;									// Number of received reports
		} target;
	} rep;
};

#define UUID_from_U32 			 "%8x-0000-0000-0000-000000000000"		// All the UUID's have unique first u32 so we dont use the rest of 12[b]

void sb_cluster_conf_create( struct sb_cluster_conf *);
void sb_cluster_conf_destroy(struct sb_cluster_conf *);
int  sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *, const char *host_name);
const struct sb_cluster_conf *sb_cluster_get_const_conf(void);
      struct sb_cluster_conf *sb_cluster_get_conf(void);
int  sb_cluster_get_disk_idx_from_disk_name(const struct sb_cluster_conf *, const char *disk_name);
int  sb_cluster_get_disk_idx_from_disk_uuid_s(const struct sb_cluster_conf *, const char *disk_uuid);
int  sb_cluster_get_node_idx_from_disk_uuid(                                  uint32_t    disk_uuid);
int  sb_cluster_get_disk_idx_from_disk_uuid_n(                                uint32_t    disk_uuid);
bool sb_cluster_update_disk_namespace_from_name(     struct sb_disk_conf *, const char *disk_name);
void sb_cluster_update_disk_vendor_and_verify(       struct sb_disk_conf *, const char *vendor);
void sb_cluster_praid_alloc_replacement_seg(      struct sb_cluster_conf *, struct sb_praid_conf* pr /*, Todo: give destination disk here */ );

const struct sb_seg_conf*   sb_cluster_get_seg_ptr_from_uuid(const struct sb_cluster_conf *, uint32_t  seg_uuid);
void                        sb_cluster_get_seg_idx_from_uuid_n(uint32_t seg_uuid, unsigned *vi, unsigned *ci, unsigned *ri, unsigned *si);
      struct sb_praid_topo* sb_cluster_get_topo_prd_ptr_from_uuid( struct sb_cluster_conf *, const char *raid_uuid);
      struct sb_seg_topo*   sb_cluster_get_topo_seg_ptr_from_uuid_n( struct sb_cluster_conf *, uint32_t    seg_uuid);
      struct sb_seg_topo*   sb_cluster_get_topo_seg_ptr_from_uuid_s( struct sb_cluster_conf *, const char *seg_uuid);

bool sb_cluster_topo_prd_is_ioable(const struct sb_praid_topo*);
bool sb_cluster_vol_has_any_live_toma_local_segs(const struct sb_cluster_conf *sb, uint32_t vol_idx);
static inline bool sb_cluster_node_is_live_toma(unsigned node_idx) { return node_idx == 0; }

// Iterators over configuration. Below for loops emulate the same clients defines
#define topo_for_each_chunk(  vol, chunk, ci)	for (ci = 0, chunk = vol->chunks;    ci < vol->num_chunks;     ++ci, ++chunk)
#define chunk_for_each_raid1(chunk, raid, ri)	for (ri = 0, raid = chunk->raids;    ri < chunk->n_raids;      ++ri, ++raid)
#define raid1_for_each_seg(  raid,  seg,  si)	for (si = 0, seg  = raid->segs;      si < (raid->D + raid->P); ++si, ++seg)
#define chunk_for_each_seg(chunk, raid, ri, seg, si)	chunk_for_each_raid1(chunk, raid, ri)	 raid1_for_each_seg(  raid,  seg,  si)
#define topo_for_each_raid1(vol, chunk, ci, raid, ri)	topo_for_each_chunk(  vol, chunk, ci)	 chunk_for_each_raid1(chunk, raid, ri)
#define topo_for_each_seg(vol, c, ci, r1, ri, seg, si)	topo_for_each_raid1(  vol, c, ci, r1, ri) raid1_for_each_seg(    r1,  seg,  si)
#define topo_for_each_live_toma_seg(vol, c, ci, r1, ri, seg, si)	topo_for_each_seg(vol, c, ci, r1, ri, seg, si) if (sb_cluster_node_is_live_toma(sb_cluster_get_node_idx_from_disk_uuid(seg->disk_uuid)))
#define topo_declare_iterator(c, r, seg, ci, ri, si)	const struct sb_chunk_conf *c;	const struct sb_praid_conf *r;	const struct sb_seg_conf *seg;	unsigned ci, ri, si;

// Todo: Add functions here to dynamically create and remove volumes in mongo-db instead of static during init creation
