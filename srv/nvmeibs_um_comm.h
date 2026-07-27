#ifndef NVMEIBS_UM_COMM_H
#define NVMEIBS_UM_COMM_H

/* user mode - kernel communication */
struct nvmeibs_um_comm;
struct nvmeibs_um_comm * nvmeibs_um_comm_start(void);
void nvmeibs_um_comm_stop(struct nvmeibs_um_comm *p);
void nvmeibs_um_comm_add_disk(
	struct nvmeibs_um_comm *p, struct nvmeibs_disk_info *di);
void nvmeibs_um_comm_remove_disk(
	struct nvmeibs_um_comm *p, struct nvmeibs_disk_info *di);
void nvmeibs_um_comm_update_toma_state(struct nvmeibs_um_comm *p, bool is_up);
void nvmeibs_um_comm_add_dummy_disk(
	struct nvmeibs_um_comm *p, const char *name, int len);
void nvmeibs_um_comm_remove_dummy_disk(
	struct nvmeibs_um_comm *p, const char *name, int len);
void nvmeibs_um_comm_serjio_state_changed(
    struct nvmeibs_um_comm *p, const char *disk_id, u16 vendor_id,
    char *model_str, enum nvmeibs_serjio_status serjio_status);
struct nvmeib_msg_to_process;
int nvmeibs_send_msg_to_user_porcess(struct nvmeib_msg_to_process *msg);

#endif

