/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBT_RAFT_MSG_FMT
#define NVMEIBT_RAFT_MSG_FMT

enum nvmeibt_raft_msg_type {
	RAFT_MSG_REQ_VOTE =           (0x1 << 0),
	RAFT_MSG_REQ_VOTE_REP =       (0x1 << 1),
	RAFT_MSG_APPEND_ENTRIES =     (0x1 << 2),
	RAFT_MSG_APPEND_ENTRIES_REP = (0x1 << 3),
};

struct raft_msg {
	uint32_t					sw_ver;										// 4	// TOMA_SW_VER: binary capability version of the sender
	char						build_version[24];							// 28
	char						reserved_3[20];								// 48
	enum nvmeibt_raft_msg_type	msg_type:32;								// 52
	union nvmeib_uuid			src_node_id;								// 68
	int							src_node_idx;								// 72
	union nvmeib_uuid			dst_node_id;								// 88
	int							dst_node_idx;								// 92
	unsigned long long			current_term;								// 100
	unsigned long long			shutdown_term;								// 108
	int64_t						applied_TOPO_idx;							// 116	// Leader's decision, propagated to all
	int64_t						applied_TOPO_CONFIG_idx;					// 124	// Leader the config of the TOPO
	int64_t						applied_raft_members_offset;				// 132	// Leader
	//
	unsigned long long			local_serialization_version;				// 140
	uint32_t					append_entries_msg_num;						// 144
	int							msg_data_len;								// 148
	uint32_t					raft_hdr_crc;								// 152
	int							reserved_2;									// 156																		//
	char						is_vote_granted;							// 157
	char						is_with_raft_log;							// 158
	char						flags;										// 159
	char						reserved;									// 160
	struct nvmeibt_persist_and_wire_buf		persist_and_wire_buf;			// 160 + 224 = 384	// Must be the last field. Immediately followed by the actual data (topo, config, ...)
} __attribute__ ((packed, aligned(8)));

#endif // #ifndef NVMEIBT_RAFT_MSG_FMT