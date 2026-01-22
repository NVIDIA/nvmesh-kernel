/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MSC
#define NVMEIBC_MSC

#include "kr_incs.h"

struct nvmeib_mcs_msg_cache;

struct mcs_info {
	//header version from common h file
	int mcs_header_version;
	//scheme checksum from stub file
	int mcs_scheme_version;
	//number of messages items from stub file
	int mcs_num_msg;
	//maximal message id
	int mcs_max_msg_id;
	//maximal number of offsets in stub fule
	int mcs_num_offs;
	//points to c array of arrys of the pointers offsets from the message strat
	const int *mcs_incr_offs;
	//for each array, the type of the array
	const int *mcs_incr_types;
	//maps between opcodes and upstream message id.
	const int *mcs_opcodes_to_incr_id_upstr;
	//maps between opcodes and downstream message id.
	const int *mcs_opcodes_to_incr_id_downstream;
	//the size of messages
	const size_t *mcs_sizeof_items;
	//hold messages that wait for send acknowledge from upstream
	struct nvmeib_mcs_msg_cache *cache;
};

//initialize new mcs handle to be used later
//parameters are taken from the stub h file
#define NVMEIB_MCS_INIT nvmeib_mcs_allocate_and_init(MCS_SCHEME_VERSION, NUM_MSGS, NUM_OFFSETS,\
	(incr_offst[0]), (incr_types[0]),\
	opcodes_to_incr_id_upstr, opcodes_to_incr_id_downstr, sizeof_items, \
	sizeof(opcodes_to_incr_id_upstr) / sizeof(opcodes_to_incr_id_upstr[0]))

//initialize new mcs handle to be used later.
void *nvmeib_mcs_allocate_and_init(unsigned int scheme_version,
	int num_msg, int num_offs, const int *incr_offs, const int *incr_types,
	const int *opcodes_to_incr_id_upstr, const int *opcodes_to_incr_id_downstr,
	const size_t *sizeof_items, size_t max_msg_id);

int nvmeib_mcs_init(struct mcs_info *mcs_info, unsigned int scheme_version,
	int num_msg, int num_offs, const int *incr_offs, const int *incr_types,
	const int *opcodes_to_incr_id_upstr, const int *opcodes_to_incr_id_downstr,
	const size_t *sizeof_items, size_t max_msg_id);

//Terminate message loop
void nvmeib_mcs_remove(void *info);

/*prepares the message for use.*/
void *nvmeib_mcs_get_msg(void *info, char *buf, size_t len);

//return message opcode
int nvmeib_mcs_get_opcode(void *msg);

//return a pointer to the buffer to send
void *nvmeib_mcs_get_msg_data(void *msg);

//return the total length of the buffer to send
int nvmeib_mcs_get_msg_len(void *msg);

//preapares a message to send.
//the input message is changed so the mcs will be able to unpack it
int nvmeib_mcs_prepare_to_send(void *info, void *message);

//a callback for sending message
typedef int (*send_cb)(void *context, void *buf, int len);
typedef int (*build_cb)(void *context, void *buf, int len);

//sends a message
int nvmeib_mcs_send(struct mcs_info *info, int opcode, void *buf,
		    const unsigned char token[16], send_cb send_tool,
		    void *context);
//build a message
int nvmeib_mcs_build_msg(struct mcs_info *info, int opcode, void *msg_to_send,
			 unsigned char token[16], build_cb bcb, void *context);

/*
    The layer that uses mcs may choose to work with mcs allocation functions
    or to to work with nvmeib_mcs_send. In that case the upper level will
    supply a callback for send.
*/

//allocate message with payload
void *nvmeib_mcs_alloc(void *info, int opcode, int payload_len);

//realloc message, will alloc only if the current size is too small
void *nvmeib_mcs_realloc(void *msg, void *info, int opcode);

//return chunk of memory from payload area
void *nvmeib_mcs_get_chunck(void *info, void *msg,
    size_t block_size, int num_blocks);

//free message allocated with nvmeibc_mcs_alloc
void nvmeib_mcs_free(void *msg);

void *replace_upstream_with_downstream(struct mcs_info* orig);

#ifndef USER_SPACE

int nvmeib_mcs_ack_msg(void *info, unsigned char token[16]);

int nvmeib_mcs_cache_size(void *_info);

int nvmeib_mcs_cache_count(void *_info);

struct msgloop_procfs_ent;

int nvmeib_mcs_send_cached_msg(struct mcs_info *info,
			       struct msgloop_procfs_ent *ent);

int nvmeib_mcs_send_proc(struct mcs_info *info, void *msg,
			 const char (*unique_uuid)[64], int opcode,
			 struct msgloop_procfs_ent *ent);
#endif

#endif
