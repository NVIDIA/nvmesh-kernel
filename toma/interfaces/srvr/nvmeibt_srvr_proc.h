#ifndef NVMEIBT_SRVR_PROC_H
#define NVMEIBT_SRVR_PROC_H

/* This file encapsulates communication channel Toma<-->LocalServer,
   based on api: 'struct nvmeibs_toma_server_proc_buf',
                 'enum nvmeibs_toma_server_msg_type' messages.*/

/***************************** Generic API Toma->Server ***********************/
int nvmeibt_open_fd_clnt_and_local_srvr(void); 		// Toma->Srvr, Srvr->Toma, Toma->Clnt.  No need to close, when Toma dies
int nvmeibt_toma_get_local_server_fd(void);			// Todo: obscure, Get already opened file descriptor Toma->Srvr,.
int nvmeibt_toma_get_local_server_fd_events(void);	// Todo: obscure, Get already opened file descriptor Srvr->Toma.
int nvmeibt_toma_announce_ready(int is_on);			// Login/Logout into local server

/******************************** Specific msgs *******************************/
/* cid - identifier of client's connection with server per specific disk */

/* Command server to force disconnect clinet (cid) from disk */
int nvmeibt_client_disconnect_force_cmd(int cid);

/* Notify server about the location of partitions a disk's (journal/serjio-db)*/
int nvmeibr_proc_notify_journal_info(const char* ldisk_id /*Name of NVMe disk */,
		uint64_t journal_pba,   uint64_t journal_length,
		uint64_t serjio_db_pba, uint64_t serjio_db_length);

//int nvmeibt_seg_active_notify_serjio_clean_range(struct nvmeibt_seg_active *seg_active, bool seg_deleted)

/***************************** Generic messages *******************************/
int nvmeibt_toma_send_msg_to_local_server(const struct nvmeibs_toma_server_proc_buf *msg);
int nvmeibt_toma_get_msg_from_local_server(struct nvmeibs_toma_server_proc_buf *msg, int max_len, bool *is_server_event);
int nvmeibt_toma_send_buf_to_client(      const char *buf, int buf_len, const struct nvmeibt_host_name *dst);

/***************************** Probe Local Hardware *******************************/
#define DISKS_INFO_FILE    TOMA_ROOT_DIR "proc/nvmeibs/disks.csv"
#define NICS__INFO_FILE    TOMA_ROOT_DIR "proc/nvmeibs/nics.csv"
#define LOCKS_INFO_FILE    TOMA_ROOT_DIR "proc/nvmeibs/locks.%.*s"          	// uuid of disk

#define PCI_DISK_FILE_DO_BIND TOMA_ROOT_DIR "sys/bus/pci/drivers/%s/bind"   	// "nvme" / "nvmeibs"
#define PCI_DISK_FILE_UN_BIND TOMA_ROOT_DIR "sys/bus/pci/drivers/%s/unbind" 	// "nvme" / "nvmeibs"

// Other server procs "/proc/nvmeibs"
// TOMA_STATUS_PROC_PATH - directory for toa output

#endif //NVMEIBT_SRVR_PROC_H

