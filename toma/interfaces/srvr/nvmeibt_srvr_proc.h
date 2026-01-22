/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_SRVR_PROC_H
#define NVMEIBT_SRVR_PROC_H

/* This file encapsulates communication channel Toma<-->LocalServer */
#include "srv/nvmeibs_srv_toma_messages.h"		// Global nvmesh dir: ../../../

struct km_comm_msg_hdr {													// Will be converted internally upon send to struct nvmeib_nl_uk_comm_msg.
	int len; 																// size of data[0], actually can be calculated from opcode field directly.
	enum uk_comm_opcode opcode;
	void (*on_done)(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *rep);	// Optional callback to execute when reply from server arrives.
	void *ctx;																// Optional private context for on done
	char  data[0] __attribute((aligned(8))); // union nvmeib_nl_msg_to_srvr_payload;
};

struct nvmeibt_km_comm_params {
	int (*on_add_disk   )(const struct nvmeib_disk_info *);		// Called when new disk is added, Toma registers its callbacks to server notifications about disks
	int (*on_remove_disk)(const struct nvmeib_remove_disk *);	// Called when existing disk is removed
	int (*process_extend_msg)(const struct nvmeib_push_extended_msg *);
	void (*process_local_srvr_msg)(struct nvmeibs_toma_server_proc_buf *m, int n_bytes);	// Callback to handle local server message and free it
	int (*process_disk_info)(const char* ldisk_id, u16 vendor_id, const char *model_str, enum nvmeibs_serjio_status serjio_status);
	void (*print_status_fn)(enum nvmeibs_toma_status_type, int (*printf_fn)(void *ctx, const char *fmt, ...), void *ctx);
	bool use_user_space_api;									// ServerLib will connect to Kernel/User-Space nvmeib server.
	bool use_only_passive_util_mode;							// Not for Toma but other utilities which only passively query server for various things but do not communicate nor issue instructions.
};

int nvmeib_srvr_api_lib_create(const struct nvmeibt_km_comm_params *);		// Singleton, Login into local server, Now can receive messages from server. Initialize your queues/mutexes/etc before calling this function
void nvmeib_srvr_api_lib_server__detach(          void);	// Stop send/recv msgs to server and receive callbacks. Can still use the library calls unrelated to server messaging, like unmapping locks
void nvmeib_srvr_api_lib_destroy(                 void);	// Do not use 'p' after calling this function. It is freed
int	 nvmeib_srvr_api_lib_send_async_msg_to_server(const struct km_comm_msg_hdr *);	// Send message with callback (async api)
int	 nvmeib_srvr_api_lib_send_block_msg_to_server(const struct nvmeibs_toma_server_proc_buf *);	// Blocking: server reply returned directly
int  nvmeib_srvr_api_lib_send_block_msg_to_client(const struct nvmeibs_toma_client_proc_buf *, int buf_len);	// Blocking: ask server to forward msg to a client. Server send rv returned directly
int  nvmeibt_km_comm_get_disk_info(               const char *disk_name, struct nvmeib_disk_info *di);	// On success returns 0, negative on error
int  nvmeib_srvr_api_lib_send_block_status_reply( const struct nvmeibs_msg_s2t_toma_status_req *req);	// Todo: Remove. Library should auto call this. Function below is used to fill kernel server proc files under TOMA_STATUS_PROC_DIR directory

/***************************** Probe Local Hardware *******************************/
struct nvmeibt_Str;
int nvmeib_srvr_api_lib_get_csv_disks(      struct nvmeibt_Str *str); // Appends the csv to already allocated (possibly empty) string. Upon error return negative rv, 0 on success
int nvmeib_srvr_api_lib_get_csv_nics(       struct nvmeibt_Str *str); // Same as above
int nvmeib_srvr_api_lib_get_disk_smart_info(int seq, struct nvmeibt_Str *str);

struct mmap_tbl {
	void *addr;					// Address of locks table memory map. On alloc error returned NULL
	size_t length;				// Actual length[bytes] of allocated memory. On error == 0. Might be slightly bigger than requested, due to padding
};
struct mmap_tbl nvmeib_srvr_api_lib_locks_map_get(const char* disk_name, uint64_t n_blksets, uint64_t offset /*=0*/, bool allow_write /*= true*/);
int             nvmeib_srvr_api_lib_locks_map_put(const char *disk_name, struct mmap_tbl memory_returned_by_valid_get);		// Upon error returns negative

int nvmeib_srvr_api_lib_disk_dobind(          const char *disk_bdf, bool is_nvmesh);	//   Bind to   nvmesh/nvme driver
int nvmeib_srvr_api_lib_disk_unbind(          const char *disk_bdf, bool is_nvmesh);	// UnBind from nvmesh/nvme driver
int nvmeib_srvr_api_lib_disk_nvmeof_sata_bind(const char *dev_file_name, const char*model, const char*serial, u16 vendor, const bool is_stock_to_nvmeibs);

#endif //NVMEIBT_SRVR_PROC_H

