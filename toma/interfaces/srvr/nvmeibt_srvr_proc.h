#ifndef NVMEIBT_SRVR_PROC_H
#define NVMEIBT_SRVR_PROC_H

/* This file encapsulates communication channel Toma<-->LocalServer */
#include "srv/nvmeibs_srv_toma_messages.h"		// Global nvmesh dir: ../../../

/***************************** Generic API Toma->Server ***********************/
int nvmeibt_open_fd_clnt_and_local_srvr(void); 		// Toma->Srvr, Srvr->Toma, Toma->Clnt.  No need to close, when Toma dies
int nvmeibt_toma_get_local_server_fd(void);			// Todo: obscure, Get already opened file descriptor Toma->Srvr,.
int nvmeibt_toma_get_local_server_fd_events(void);	// Used to epoll on this fd
int nvmeibt_toma_announce_ready(int is_on);			// Login/Logout into local server

/***************************** Generic messages *******************************/
int nvmeibt_toma_send_msg_to_local_server(const struct nvmeibs_toma_server_proc_buf *msg);
int nvmeibt_toma_get_msg_from_local_server(     struct nvmeibs_toma_server_proc_buf *msg, int max_len, bool *is_server_event);
int nvmeibt_toma_send_buf_to_client(      const /* struct nvmeibs_toma_client_proc_buf */ char *buf, int buf_len, const char *clnt_host);

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
#define DISKS_INFO_FILE    TOMA_ROOT_DIR "proc/nvmeibs/disks.csv"
#define NICS__INFO_FILE    TOMA_ROOT_DIR "proc/nvmeibs/nics.csv"
#define LOCKS_INFO_FILE    TOMA_ROOT_DIR "proc/nvmeibs/locks.%.*s"          	// uuid of disk

#define PCI_DISK_FILE_DO_BIND TOMA_ROOT_DIR "sys/bus/pci/drivers/%s/bind"   	// "nvme" / "nvmeibs"
#define PCI_DISK_FILE_UN_BIND TOMA_ROOT_DIR "sys/bus/pci/drivers/%s/unbind" 	// "nvme" / "nvmeibs"

// Other server procs "/proc/nvmeibs"
// TOMA_STATUS_PROC_PATH - directory for toma output

#endif //NVMEIBT_SRVR_PROC_H

