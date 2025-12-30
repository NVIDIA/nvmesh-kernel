#ifndef NVMEIBT_SRVR_PROC_H
#define NVMEIBT_SRVR_PROC_H

/* This file encapsulates communication channel Toma<-->LocalServer */
#include "srv/nvmeibs_srv_toma_messages.h"		// Global nvmesh dir: ../../../

int nvmeib_srvr_api_lib_create(void);
int nvmeib_srvr_api_lib_handshake_server(void);	// Login into local server, Todo: Unify with create
int nvmeib_srvr_api_lib_destroy(void);
int nvmeib_srvr_api_lib_get_fd_for_epoll(void);

int nvmeib_srvr_api_lib_send_msg_to_server(const struct nvmeibs_toma_server_proc_buf *msg);
int nvmeib_srvr_api_lib_recv_msg_from_server(    struct nvmeibs_toma_server_proc_buf *msg, int max_len, bool *is_server_event);
int nvmeibt_toma_send_buf_to_client(       const struct nvmeibs_toma_client_proc_buf *msg, int buf_len, const char *clnt_host);

// Function below is used to fill kernel server proc files under TOMA_STATUS_PROC_DIR directory
int nvmeib_srvr_api_lib_fill_and_send_status_reply(const struct nvmeibs_msg_s2t_toma_status_req *req,
	void (*your_print_status_fn)(enum nvmeibs_toma_status_type, int (*printf_fn)(void *ctx, const char *fmt, ...), void *ctx));

/***************************** Netlink: New Toma-API vs kernel server, used for disk related communication */
struct km_comm_msg_hdr {
	int len; /* the len of data[0] */
	int opcode;
	void (*on_done)(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *rep);
	void *ctx;
	char  data[0] __attribute((aligned(8)));
};

struct nvmeibt_km_comm;
struct nvmeibt_km_comm *nvmeibt_km_comm_create(void);
void					nvmeibt_km_comm_delete(struct nvmeibt_km_comm *p);
int						nvmeibt_km_comm_send(  struct nvmeibt_km_comm *p, struct km_comm_msg_hdr *hdr);
int	 nvmeibt_km_comm_register_disk_events(     struct nvmeibt_km_comm *p, struct nvmeib_register_change_disk *cbs);
void nvmeibt_km_comm_ack_disk_remove(          struct nvmeibt_km_comm *p, unsigned long ack_id);
int  nvmeibt_km_comm_get_disk_info(            struct nvmeibt_km_comm *p, const char *disk_name, struct nvmeib_disk_info *di);

/***************************** Probe Local Hardware *******************************/
struct nvmeibt_Str;
int nvmeib_srvr_api_lib_get_csv_disks(struct nvmeibt_Str *str); // Appends the csv to already allocated (possibly empty) string. Upon error return negative rv, 0 on success
int nvmeib_srvr_api_lib_get_csv_nics( struct nvmeibt_Str *str); // Same as above

struct mmap_tbl {
	void *addr;					// Address of locks table memory map. On alloc error returned NULL
	size_t length;				// Actual length[bytes] of allocated memory. On error == 0. Might be slightly bigger than requested, due to padding
};
struct mmap_tbl nvmeib_srvr_api_lib_locks_map_get(const char* disk_name, uint64_t n_blksets, uint64_t offset /*=0*/, bool allow_write /*= true*/);
int             nvmeib_srvr_api_lib_locks_map_put(const char *disk_name, struct mmap_tbl memory_returned_by_valid_get);		// Upon error returns negative

int nvmeib_srvr_api_lib_disk_dobind(const char *disk_bdf, bool is_nvmesh);	//   Bind to   nvmesh/nvme driver
int nvmeib_srvr_api_lib_disk_unbind(const char *disk_bdf, bool is_nvmesh);	// UnBind from nvmesh/nvme driver

#endif //NVMEIBT_SRVR_PROC_H

