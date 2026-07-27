#ifndef NVMEIBS_NORDDA_SIM_SHARED_H
#define NVMEIBS_NORDDA_SIM_SHARED_H

struct volume_server_gen_rsp_uuid_jour;

struct handle_get_uuid_jour_cb_param {
	const struct nvmeib_gen_cmd_param *gen_param;
	union nvmeib_gen_cmd_rsp *gen_rsp;
	size_t jmdc_sz;
	size_t ent_md_sz;
};

int nvmeibs_gen_cmd_handle_get_uuid_jour(struct nvmeibs_disk_info *di, struct handle_get_uuid_jour_cb_param *param);

#endif /*NVMEIBS_NORDDA_SIM_SHARED_H*/
