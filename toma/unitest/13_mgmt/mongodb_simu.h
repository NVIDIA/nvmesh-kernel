/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements mongo-db which store volumes / cluster configuration and reported states, by Tomas,
    Primarily used by management simulator and unit-test code */
#include "../sandbox_util.h"
#include "../../../autogen/clnt/nvmeibc_mcs_stub.h"	// Client simulator - report to mgmt simulator
#define SB_CLUSTER_CONF_N_NODES_TOTAL (3)			// Cluster of 3 machines, 1 live followed by 2 simulated other tomas, presented as nodes n37, n38, n39
#define SB_CLUSTER_CONF_MAX_VOLS      (4)			// Maximum number of volumes in the cluster configuration
#define SB_CLUSTER_CONF_MAX_CHUNKS    (2)			// Maximum number of chunks in a volume, For now, 2 chunks only, Support for volume extend once

/* Sandbox UUID encoding -- all uuids are 32-bit integers built from 0-based
 * array indices. First 3 nibbles = node {f37 (liveToma), 2 other Tomas: f38,
 * f39}. Volumes (block devices) have first 4 nibbles as bdXX where XX is
 * volume index (up to 256 vols), last 4 nibbles are 0CRS where C, R, S are
 * chunk, raid and seg indices respectively. Counting in the encoded UUID
 * starts from 1.
 *   node:   0xf3{N+7}000c{N}              N = node index in sb_cluster_conf.nodes[]
 *   disk:   0xf3{N+7}000d{D}              D = disk index in sb_node_conf.disks[]
 *   vol:    0xbd{V+1}0000                 V = volume index in sb_cluster_conf.vols[]
 *   praid:  0xbd{V+1}{C+1}{R+1}0          C, R = chunk and praid indices
 *   seg:    0xbd{V+1}{C+1}{R+1}{S+1}      S = segment index in sb_praid_conf.segs[]
 *
 * Tests should refer to objects via these helpers instead of dealing with the
 * bit positions, and instead of dereferencing sb_cluster_conf for the UUID.
 * Each component is a single hex nibble after the +1 shift, so any index
 * >= 15 would corrupt the encoding -- the BUG_ONs catch that.
 */
uint32_t sb_node_uuid (int n);
uint32_t sb_disk_uuid (int n, int d);
uint32_t sb_vol_uuid  (int v);
uint32_t sb_praid_uuid(int v, int c, int r);
uint32_t sb_seg_uuid  (int v, int c, int r, int s);

struct sb_cluster_conf {
	struct sb_node_conf {
		char hostname[64];		// Easily recognizable host name
		uint32_t uuid;			// uuid of this node
		struct sb_nics_conf {
			const char *protocol;
			uint32_t uuid;
		} nics[2];				// Each node has exactly 2 nics
		struct sb_disk_conf {	// Each node has up to 3 local disks, represents what management knows
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
		} disks[3];
		struct peer_toma_simu *peer;				// Relevant for other node only (not the live toma). Pointer to peer Toma
		struct clnt_simu      *clnt;				// Client running on this node. on 'live' node local client is mandatory for recoveries. On other nodes those are remote clients simulating attach/io's
	} nodes[SB_CLUSTER_CONF_N_NODES_TOTAL], *live, *other;
	int n_nodes;
	struct sb_volume_conf {							// All volumes configuration
		// --------------- Volume config
		const char* name;
		uint32_t uuid;								// For simplicity all uuids are u32
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
				} segs[4];							// Up to 3+1 EC, for now
			} raids[1];								// For now, each chunk has only 1 praid. Dont support Raid-0
		} chunks[SB_CLUSTER_CONF_MAX_CHUNKS];
		// --------------- Client reports
		struct sb_attachment_info {					// Each client can be attached to each volume
			uint32_t attachment_version;			// 0 if not attached.
			struct nvmeibc_reservation reserv;		// Todo: Use this to test enforcing reservation version attached by Toma
			bool ioEnabled;							// Client reports that its IO is enabled (after conversation with Toma).
			bool is_recovery_attach;
		} clnts[SB_CLUSTER_CONF_N_NODES_TOTAL];		//
		// --------------- Toma reports
		struct sb_chunk_topo {
			struct sb_praid_topo {
				const struct sb_praid_conf *cfg;	// Pointer to config of praid
				uint16_t version_major;
				uint16_t version_minor;
				struct sb_seg_topo {
					enum seg_topo_state { mdb_seg_UNK = 0, mdb_seg_BOOT = 'B', mdb_seg_ZERO = '0', mdb_seg_INIT = 'I', mdb_seg_CORRUPTED = '!', mdb_seg_RW = 'R', mdb_DEAD = 'D', mdb_WRITE = 'W', mdb_seg_dep='v', mdb_seg_rep='^',  } status;
					bool vitality;					// True = reports to leader, false = node not conencted to leader
				} segs[4];							// Up to 3+1 EC, for now
			} raids[1];								// For now, each chunk has only 1 praid. Dont support Raid-0
		} topo_chunks[SB_CLUSTER_CONF_MAX_CHUNKS];
	} vols[SB_CLUSTER_CONF_MAX_VOLS];				// For now, up to 4 volumes
	int n_vols;
	int zone_idx;									// All those volume exist in a specific zone
};

void sb_cluster_conf_create( struct sb_cluster_conf *);
void sb_cluster_conf_destroy(struct sb_cluster_conf *);
int  sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *, const char *host_name);
const struct sb_cluster_conf *sb_cluster_get_const_conf(void);
/* Mutable accessor -- used by scenarios that need to flip simulator-only state
 * such as sb_disk_conf.is_out_of_service before sending a HW config update. */
struct sb_cluster_conf       *sb_cluster_get_conf(void);
int  sb_cluster_get_disk_idx_from_disk_name(const struct sb_cluster_conf *, const char *disk_name);
int  sb_cluster_get_disk_idx_from_disk_uuid(const struct sb_cluster_conf *, const char *disk_uuid);
int  sb_cluster_get_node_idx_from_disk_uuid(const struct sb_cluster_conf *, uint32_t    disk_uuid);
bool sb_cluster_update_disk_namespace_from_name(     struct sb_disk_conf *, const char *disk_name);
void sb_cluster_update_disk_vendor_and_verify(       struct sb_disk_conf *, const char *vendor);

const struct sb_seg_conf*   sb_cluster_get_seg_ptr_from_uuid(const struct sb_cluster_conf *, uint32_t  seg_uuid);
      struct sb_praid_topo* sb_cluster_get_topo_prd_ptr_from_uuid( struct sb_cluster_conf *, const char *raid_uuid);
      struct sb_seg_topo*   sb_cluster_get_topo_seg_ptr_from_uuid( struct sb_cluster_conf *, const char *seg_uuid);

bool sb_cluster_topo_prd_is_ioable(const struct sb_praid_topo*);
bool sb_cluster_vol_has_any_live_toma_local_segs(const struct sb_cluster_conf *sb, uint32_t vol_idx);

// Todo: Add functions here to dynamically create and remove volumes in mongo-db instead of static during init creation
