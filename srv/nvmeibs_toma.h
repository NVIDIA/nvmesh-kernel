#ifndef NVMEIBS_TOMA_H
#define NVMEIBS_TOMA_H

#include "nvmeibs_client.h"
#include "nvmeib_msgloop.h"
#include "nvmeib_shared.h"

/* Toma proc interface constructor and destructor */
int  nvmeibs_toma_create(struct proc_dir_entry *dir, void *arg);
void nvmeibs_toma_remove(void);

/* Client:TOMA */
int  nvmeibs_toma_client_proc_recv(void *arg, char *buf, size_t len,
	bool *posted);
void nvmeibs_toma_client_proc_send(struct nvmeibs_client *cl,
								   struct nvmeib_iu *recv_ioctx,
								   struct nvmeib_iu *send_ioctx);
void nvmeibs_toma_remove_client(struct nvmeibs_client *cl);

/* Server:TOMA */
int nvmeibs_toma_server_proc_recv(void *arg, char *buf, size_t len,
	bool *posted);
void nvmeibs_toma_server_proc_open(void *arg);
void nvmeibs_toma_server_proc_close(void *arg);
/* force toma to shut down */
void nvmeibs_toma_shut_down(void);
/* return true if toma is connected */
bool nvmeibs_toma_is_connected(void);
int nvmeibs_toma_server_proc_send(struct nvmeibs_toma_server_proc_buf *buf);
int nvmeibs_toma_server_proc_send_vec(struct msg_vec *vec, int cnt);

int nvmeibs_toma_server_proc_send_event(struct nvmeibs_toma_server_proc_buf *buf);
int nvmeibs_toma_server_proc_send_vec_event(struct msg_vec *vec, int cnt);

/* Server to Toma event reporting */
int nvmeibs_toma_report_event_client_disconnect(struct nvmeibs_client *cl);
int nvmeibs_toma_report_event_client_connect(struct nvmeibs_client *cl);
int nvmeibs_toma_report_event_disk_change(char *disk_id, unsigned long long n_blocks,
										  unsigned int block_size, unsigned int max_request_size,
										  unsigned int seq, unsigned int nsid,
										  const char *dev_name, unsigned int metadata,
										  const char *status, unsigned int vendor, char op, char *model_str, char * native_serial_str);
int nvmeibs_toma_report_event_port_gid_change(struct nvmeibs_ib_port *ib_port);
int nvmeibs_toma_report_event_nic_change(struct nvmeibs_dev *nis_dev, bool add);
int nvmeibs_toma_report_event_blkset_recovered(struct nvmeibs_disk_info *di, const char *ds_uuid, u64 blkset_num,
	u64 blkset_slba, u64 pre_recov_lock_val, struct nvmeibs_async_cookie_params *cookie_params);
int nvmeibs_toma_report_event_serjio_disk_range_cleaned(void *arg, const char *seg_id);
int nvmeibs_toma_report_event_serjio_request_jgc(const char *seg_id, char *disk_id_str);

int nvmeibs_toma_send_work_iu_alloc(struct nvmeibs_client *cl);
void nvmeibs_toma_send_work_iu_free(struct nvmeibs_client *cl);

#endif /* NVMEIBS_TOMA_H */

