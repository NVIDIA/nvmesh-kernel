#ifndef NVMEIBT_KM_COMM_H
#define NVMEIBT_KM_COMM_H

struct nvmeib_nl_uk_comm_rep;
struct km_comm_msg_hdr {
	/* the len of data[0] */
	int len;
	int opcode;
	void (*on_done)(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *rep);
	void *ctx;
	char data[0] __attribute((aligned(8)));
};

struct nvmeibt_km_comm;
struct nvmeibt_km_comm * nvmeibt_km_comm_create(void);
void nvmeibt_km_comm_delete(struct nvmeibt_km_comm *p);
int nvmeibt_km_comm_send(
	struct nvmeibt_km_comm *p, struct km_comm_msg_hdr *hdr);
struct nvmeib_register_change_disk;
int nvmeibt_km_comm_register_disk_events(struct nvmeibt_km_comm *p,
	struct nvmeib_register_change_disk *cbs);
void nvmeibt_km_comm_ack_disk_remove(struct nvmeibt_km_comm *p,
	unsigned long ack_id);
struct nvmeib_disk_info;
int nvmeibt_km_comm_get_disk_info(struct nvmeibt_km_comm *p,
	const char *disk_name, struct nvmeib_disk_info *di);
void nvmeibt_km_comm_test(void);

#endif