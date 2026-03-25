/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
/* Implements mongo-db which store volumes / cluster configuration and reported states, by Tomas,
    Primarily used by management simulator and unit-test code */
#include "../sandbox_util.h"

struct sb_cluster_conf {
	struct sb_node_conf {
		char hostname[64];		// Easily recognizable host name
		uint32_t uuid;			// uuid of this node
		struct sb_nics_conf {
			const char *protocol;
			uint32_t uuid;
		} nics[2];				// Each node has exactly 2 nics
		struct sb_disk_conf {	// Each node has up to 3 local disks
			char name[16];
			uint32_t uuid;
			u16 vendor;
			bool is_out_of_service;
		} disks[3];
		bool ignore_append_entries;			// Emulates infinitely slow local disk response time, does not commit raft leaders topo
	} nodes[3], *live, *other;	// Cluster of 3 machines, 1 live followed by 2 simulated other tomas, presented as nodes n37, n38, n49
	int n_nodes;
	struct sb_volume_conf {							// All volumes configuration
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
		} chunks[2];								// For now, 2 chunks only, Support for volume extend once
	} vols[4];										// For now, up to 4 volumes
	int n_vols;
	int zone_idx;									// All those volume exist in a specific zone
};

void sb_cluster_conf_create( struct sb_cluster_conf *);
void sb_cluster_conf_destroy(struct sb_cluster_conf *);
int  sb_cluster_conf_find_node_idx_by_name(const struct sb_cluster_conf *, const char *host_name);
const struct sb_cluster_conf *sb_cluster_get_const_conf(void);
int  sb_cluster_get_disk_idx_from_disk_name(const struct sb_cluster_conf *, const char *disk_name);
int  sb_cluster_get_disk_idx_from_disk_uuid(const struct sb_cluster_conf *, const char *disk_uuid);

// Todo: Add functions here to dynamically create and remove volumes in mongo-db instead of static during init creation
void sb_cluster_ignore_append_entries_by_node(int node_idx);
