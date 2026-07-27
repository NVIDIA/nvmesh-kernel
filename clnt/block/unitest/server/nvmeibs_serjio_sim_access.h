#ifndef SERJIO_SIM_ACCESS_H
#define SERJIO_SIM_ACCESS_H
/* Additional API for serjio to allow simulators tot est it. */

struct nvmeibs_serjio_full_gen_id_info {
    char boot_id[NVMEIB_GID_STR_MAX];
    u64 jri_gen_id;
    u8 ent_gen_id;
    bool wrapped;
};

void nvmeibs_serjio_wait_serjio_ready(struct nvmeibs_disk_info *di);
void nvmeibs_serjio_drain_wq(struct nvmeibs_disk_info *di);

int nvmeibs_serjio_find_jrange_index_by_client_uuid(struct nvmeibs_disk_info* di, uuid_be client_uuid);
union jblock_md* nvmeibs_serjio_get_jmdc_ptr(struct nvmeibs_disk_info *di);

void nvmeibs_serjio_entry_do(struct nvmeibs_disk_info* di, u32 jri, u32 ei, const char *action);
u32  nvmeibs_serjio_get_abandoned_bmp_by_jri(struct nvmeibs_disk_info* di, u32 jri);
void nvmeibs_serjio_verify_no_abandoned_entries(struct nvmeibs_disk_info* di);
void nvmeibs_serjio_get_full_gen_id_info(struct nvmeibs_disk_info* di, u32 jri, u32 ei, struct nvmeibs_serjio_full_gen_id_info *out);

#endif // #ifndef KR_INCS_H

