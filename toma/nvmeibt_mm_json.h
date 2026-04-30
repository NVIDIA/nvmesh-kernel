/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*
 * nvmeibt_mm_json.h
 *
 *  Created on: Aug 9, 2020
 *      Author: yair
 */

#ifndef TOMA_NVMEIBT_MM_JSON_H_
#define TOMA_NVMEIBT_MM_JSON_H_

static const char EYECATCHER_CNF_END[] = "CNFEnd";	// Duplicated, but same and can use sizeof()

#include "nvmeibt_json_base.h"
#include "../common/nvmeib_shared.h"

/******************************************************************************/
// Management and Configuration Specific Structures
/******************************************************************************/

struct mm_segment_conf {
	char eyecatcher[4];					// 4
	int8_t pRaidIndex;					// 5
	uint8_t pRaidTypeIndex;				// 6
	uint8_t type;						// 7
	char action;						// 8
	uint64_t lbs;						// 16
	uint64_t lbe;						// 24
	char		filler_1[8];			// 32
	union nvmeib_uuid uuid;				// 48
	union nvmeib_uuid diskUUID;			// 64
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));	// Pack for memcmp

struct mm_praid_conf {
	char eyecatcher[4];					// 4
	uint32_t version;					// 8
	int8_t num_segments;				// 9
	uint8_t activated;					// 10
	uint8_t stripeIndex;				// 11
	char	filler_1[5];				// 16
	union nvmeib_uuid uuid;				// 32
	struct mm_segment_conf *segments;	// 40
	int64_t	topo_config_idx_updated;	// 48
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct mm_chunk_conf {
	char eyecatcher[4];					// 4
	uint8_t num_praids;					// 5
	uint8_t num_praids_to_send;			// 6	// Used only for leader to calculate incremental topo_config
	char	filler_1[2];				// 8
	uint64_t vlbs;						// 16
	uint64_t vlbe;						// 24
	struct mm_praid_conf *praids;		// 32
	union nvmeib_uuid uuid;				// 48
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct nvmeibt_raid0_config {			// Attributes of Striping Raid-0, implemented on top of protection raid D+P
	uint8_t stripe_size_encoded;		// Toma encoded values to save space. Decoded(u32) - Amount of sequential data blocks (vlba) in each praid before moving to the next praid in R0.
	uint8_t stripe_width;				// The amount of disks on which which data is interlaced. (RAID 0) == Amount of praids in chunk, Integer >= 1
} __attribute__((__packed__));
u32  nvmeibt_raid0_config_decode_ssize(const struct nvmeibt_raid0_config*);
void nvmeibt_raid0_config_encode(            struct nvmeibt_raid0_config*, u32 stripe_size_in_units_of_blocks, u32 stripe_width);
#define nvmeibt_raid0_config_constructor()		(struct nvmeibt_raid0_config){32, 1}

struct mm_vol_conf {
	char eyecatcher[4];					// 4
	uint8_t raidType;					// 5
	uint8_t num_chunks;					// 6
	uint16_t blockSize;					// 8
	uint32_t version;					// 12
	char filler_0;						// 13
	char name[26];						// 39
	char action;						// 40
	char res_type;						// 41	// Obsolete Elect
	uint8_t relativeRebuildPriority;	// 42
	struct nvmeibt_raid0_config r0;		// 43
	uint8_t lockServer_type;			// 45
	uint8_t lockServer_maxNOwners;		// 46
	int8_t lockServer_locksetShift;		// 47
	int8_t		enableCrcCheck;			// 48
	int8_t		use_debug_di;			// 49
	char	filler_1[15];				// 64
	union nvmeib_uuid uuid;				// 80
	uint64_t blocks;					// 88
	int64_t		kafka_offset_or_idx;	// 96	// For mgmt_config it is the offset, for topo_config it is a generated idx
	struct mm_chunk_conf *chunks;		// 104
	char	filler_2[8];				// 112
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct mm_nic_conf {	// Never serialized to wire format. No need to validate its size & alignment
	char eyecatcher[4];					// 4
	uint32_t pkey;						// 8
	uint16_t version;					// 10
	uint8_t protocol;					// 11
	char sw_gid_str[36];				// 47
	char hw_gid_str[36];  				// 83
	char filler[13];					// 96
	union nvmeib_uuid uuid;				// 112
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct mm_disk_conf {	// Never serialized to wire format. No need to validate its size & alignment
	char eyecatcher[4];						// 4
	uint32_t activeFormatRequestCounter;	// 8
	uint64_t n_pblks;						// 16
	union nvmeib_uuid uuid;					// 32
	union nvmeib_uuid origNodeUuid;			// 48
	char diskID[42];						// 90
	uint16_t vendorID;						// 92
	uint16_t version;						// 94
	uint16_t block_size;					// 96
	uint8_t isOutOfService;					// 97
	char	filler_2[15];					// 112
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct mm_node_conf {	// Never serialized to wire format. No need to validate its size & alignment
	char eyecatcher[4];					// 4
	uint16_t version;					// 6
	uint8_t num_nics;					// 7
	char	filler_1[1];				// 8
	struct mm_nic_conf *nics;			// 16
	char node_id[256];					// 272
	union nvmeib_uuid uuid;				// 288
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct mm_raft_member_conf {	// Both wire-packed and serialized
	char		eyecatcher[4];						// 4
	int64_t		kafka_offset;						// 12
	char		hostname[NVMEIB_HOST_NAME_LEN];		// 76
	char		filler_1[12];						// 88
	int64_t		raft_members_seq_no_updated;		// 96, very small ~2[b] unique id of raft quorum member. Start from 1, each raft add/rmv member increases by 1
	union		nvmeib_uuid uuid;					// 112
	char	align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct HW_mgmt_conf {
	char eyecatcher[4];								// 4
	uint16_t structVersion;							// 6
	uint16_t protocolVersion;						// 8
	int64_t						configurationVersion;	// 16
	int64_t						idx;				// 24 The original conf from MGMT does not contain a kafka_offset. An auxiliary value becomes sticky
	int64_t						messageTypeVersion;	// 32
	union nvmeib_uuid			dbUUID;				// 48
	char						messageType[64];	// 112
	uint16_t					num_disks;			// 118
	uint16_t					num_nodes;			// 120
	struct mm_disk_conf			*disks;				// 136
	struct mm_node_conf			*nodes;				// 144
	int64_t						leaderToken;		// 152
	int64_t						kafkaMessageSequence;	// 160
	int64_t						raftTerm;			// 168
	uint8_t						stopSendingKeepaliveToken;	// 169
	char						filler[55];			// 224
	char						align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct mm_mgmt_conf {
	char eyecatcher[4];								// 4
	uint16_t structVersion;							// 6
	uint16_t protocolVersion;						// 8
	int64_t						configurationVersion;	// 16
	int64_t						idx;				// 24 The original conf from MGMT does not contain a kafka_offset. An auxiliary value becomes sticky
	int64_t						messageTypeVersion;	// 32
	union nvmeib_uuid			dbUUID;				// 48
	char						messageType[64];	// 112
	int							num_vols;			// 116
	uint16_t					num_disks;			// 118
	uint16_t					num_nodes;			// 120
	struct mm_vol_conf			*volumes;			// 128
	struct mm_disk_conf			*disks;				// 136
	struct mm_node_conf			*nodes;				// 144
	int64_t						leaderToken;		// 152
	int64_t						kafkaMessageSequence;	// 160
	int64_t						raftTerm;			// 168
	uint8_t						stopSendingKeepaliveToken;	// 169
	char						filler[55];			// 224
	char						align[0] __attribute__((aligned(16)));
} __attribute__((__packed__, aligned(16)));

struct _packed_mm_mgmt_conf {
	char						eyecatcher[4];			// 4
	uint16_t					structVersion;			// 6
	uint16_t					protocolVersion;		// 8
	int64_t						configurationVersion;	// 16
	int64_t						idx;					// 24
	char						filler_1[24];			// 48
	union nvmeib_uuid			dbUUID;					// 64
	char						messageType[64];		// 128
	int64_t						messageTypeVersion;		// 136
	uint32_t					num_vols;				// 140
	uint16_t					num_disks;				// 142
	uint16_t					num_nodes;				// 144
	char						align[0] __attribute__((aligned(16)));
}__attribute__((__packed__, aligned(16)));

void nvmeibt_mm_json_free_kv_tree(struct mm_json_elem *root);
struct mm_json_elem *parse_json_txt_into_kv_tree(const char *in, int buff_len);

uint16_t nvmeibt_raft_member_conf_convert_le_be(struct mm_raft_member_conf *dst, const struct mm_raft_member_conf *src);
int nvmeibt_mgmt_msg_json_tree_to_mgmt_conf(struct mm_mgmt_conf *conf, struct mm_json_elem *root, int64_t kafka_offset, bool is_new_or_upd, bool is_deleteVolumeCompleted);
int nvmeibt_mm_json_tree_to_HW_mgmt_conf(struct HW_mgmt_conf *conf, struct mm_json_elem *elem, int64_t kafka_offset);
void mm_conf_free_tree(struct mm_mgmt_conf *conf);
void HW_conf_free_tree(struct HW_mgmt_conf *conf);
void mm_print_conf(struct mm_mgmt_conf *conf, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
void HW_print_conf(struct HW_mgmt_conf *conf, int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
void mm_set_vol_num(void *dst, uint32_t n_vols);
struct nvmeibt_Str;
void serialize_end_of_array_obj_to_JSON(int i, int n_objs_in_arr, bool is_end_of_obj_right_after_end_of_array, struct nvmeibt_Str *JSON_output);
struct mm_mgmt_conf *mm_wire_buf_to_mm_mgmt_conf(const void *in_wire_conf_buf, bool is_topo_config, struct nvmeibt_Str *JSON_output);
struct nvmeibt_praid;
void nvmeibt_mm_jason_leader_topo_config_mm_praid_conf_and_mm_segs_conf_to_wire_buf(struct nvmeibt_praid *praid);
void nvmeibt_mm_json_leader_serialize_baseline_topo_config_to_wire(uint64_t topo_config_version);
void nvmeibt_mm_json_leader_serialize_kafka_mgmt_config_to_wire(void);
int nvmeibt_mm_json_mark_deleted_in_kafka_mgmt_config_vol_chunks_praids_segs_wire_conf_buf(void *data, size_t len);
int mm_conf_get_packed_size(struct mm_mgmt_conf *conf);
struct nvmeibt_block_device;
void nvmeibt_mm_json_serialize_vol_and_chunks_and_praids_and_segs_kafka_mgmt_config_to_wire(struct nvmeibt_block_device *blkdev, struct mm_vol_conf *vol);
uint16_t nvmeibt_seg_convert_config_le_be(void *p, struct mm_segment_conf *src, BOOL is_out);
uint16_t nvmeibt_praid_convert_config_le_be(void *p, struct mm_praid_conf *src, BOOL is_out);
uint16_t nvmeibt_chunk_convert_config_le_be(void *p, struct mm_chunk_conf *src, BOOL is_out);
uint16_t nvmeibt_vol_convert_config_le_be(void *p, struct mm_vol_conf *src, BOOL is_out);
uint16_t nvmeibt_mm_mgmt_convert_config_le_be(void *p, struct mm_mgmt_conf *src, BOOL is_out);
uint16_t nvmeibt_packed_seg_config_size(void);
uint16_t nvmeibt_packed_praid_config_size(void);
uint16_t nvmeibt_packed_chunk_config_size(void);
uint16_t nvmeibt_packed_vol_config_size(void);
uint16_t nvmeibt_packed_mm_mgmt_config_size(void);
// Wrappers that convert to wire format via aligned temporary buffers; Use these when dst may not be 16-byte aligned
uint16_t nvmeibt_seg_convert_to_wire_via_aligned_tmp(void *dst, struct mm_segment_conf *src);
uint16_t nvmeibt_praid_convert_to_wire_via_aligned_tmp(void *dst, struct mm_praid_conf *src);
uint16_t nvmeibt_chunk_convert_to_wire_via_aligned_tmp(void *dst, struct mm_chunk_conf *src);
uint16_t nvmeibt_vol_convert_to_wire_via_aligned_tmp(void *dst, struct mm_vol_conf *src);
uint16_t nvmeibt_mm_mgmt_convert_to_wire_via_aligned_tmp(void *dst, struct mm_mgmt_conf *src);
struct nvmeibt_persist_and_wire_buf;
int nvmeibt_mm_json_read_JSON_and_generate_persist_and_wire(char *JSON_file_name);

#endif /* TOMA_NVMEIBT_MM_JSON_H_ */
