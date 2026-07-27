#ifndef NVMEIB_VEX_H
#define NVMEIB_VEX_H

enum nvmeib_vex_ach_ops {
	vex_ach_shared_cfg = 0,
	vex_ach_get_io_alloc_disk,
	vex_ach_get_io_port_info,
	vex_ach_acs_map_clnt_ionics,
	vex_ach_acs_map_srv_ionics,
	vex_ach_acs_map_disks,
	vex_ach_acs_map_arnics,
	vex_ach_acs_map_disk_info,
	vex_ach_acs_map_di_rsrc_set,
	vex_ach_lock_mem_seg_info,
	vex_ach_clnt_toma_req,
	vex_ach_toma_clnt_req,
	vex_ach_get_io,
	vex_ach_get_jrange_req,
	vex_ach_get_jrange_rsp,
	vex_ach_get_lock_gids_rsp,
	vex_ach_dbg_cmd,
	vex_ach_abnd_free,
	vex_ach_get_jmdc_rsp,
	vex_ach_get_jmdc_rng_hdr,
	vex_ach_get_acs,
	vex_ach_ops_num // LAST element;
};

static inline const char *vex_ach_names(int n)
{
	enum nvmeib_vex_ach_ops op = n;
	switch (op) {
	case vex_ach_shared_cfg: return "ach_shared_cfg";
	case vex_ach_get_io_alloc_disk: return "ach_get_io_alloc_disk";
	case vex_ach_get_io_port_info: return "ach_get_io_port_info";
	case vex_ach_acs_map_clnt_ionics: return "ach_acs_map_clnt_ionics";
	case vex_ach_acs_map_srv_ionics: return "ach_acs_map_srv_ionics";
	case vex_ach_acs_map_disks: return "ach_acs_map_disks";
	case vex_ach_acs_map_arnics: return "ach_acs_map_arnics";
	case vex_ach_acs_map_disk_info: return "ach_acs_map_disk_info";
	case vex_ach_acs_map_di_rsrc_set: return "ach_acs_map_di_rsrc_set";
	case vex_ach_lock_mem_seg_info: return "ach_lock_mem_seg_info";
	case vex_ach_clnt_toma_req: return "ach_clnt_toma_req";
	case vex_ach_toma_clnt_req: return "ach_toma_clnt_req";
	case vex_ach_get_io: return "ach_get_io";
	case vex_ach_get_jrange_req: return "ach_get_jrange_req";
	case vex_ach_get_jrange_rsp: return "ach_get_jrange_rsp";
	case vex_ach_get_lock_gids_rsp: return "ach_get_lock_gids_rsp";
	case vex_ach_dbg_cmd: return "ach_dbg_cmd";
	case vex_ach_abnd_free: return "ach_abnd_free";
	case vex_ach_get_jmdc_rsp: return "ach_get_jmdc_rsp";
	case vex_ach_get_jmdc_rng_hdr: return "ach_get_jmdc_rng_hdr";
	case vex_ach_get_acs: return "ach_get_acs";
	default: break;
	}
	return "invalid";
}

enum nvmeibc_vex_nrch_ops {
	vex_nrch_io_read = 0,
	vex_nrch_io_other,
	vex_nrch_io_srv_lock_req,
	vex_nrch_io_srv_lock_rsp,
	vex_nrch_io_req,
	vex_nrch_io_rsp,
	vex_nrch_gen_br_req,
	vex_nrch_gen_uj_req,
	vex_nrch_gen_db_req,
	vex_nrch_gen_fje,
	vex_nrch_gen_je,
	vex_nrch_gen_uj_rsp,
	vex_nrch_gen_br_rsp,
	vex_nrch_gen_db_rsp,
	vex_nrch_gen_fje_ent,
	vex_nrch_ops_num // LAST element;
};

static inline const char *vex_nrch_names(int n)
{
	enum nvmeibc_vex_nrch_ops op = n;
	switch (op) {
	case vex_nrch_io_read: 				return "nrch_io_read";
	case vex_nrch_io_other: 			return "nrch_io_other";
	case vex_nrch_io_srv_lock_req: 		return "nrch_io_srv_lock_req";
	case vex_nrch_io_srv_lock_rsp: 		return "nrch_io_srv_lock_rsp";
	case vex_nrch_io_req: 				return "nrch_io_req";
	case vex_nrch_io_rsp: 				return "nrch_io_rsp";
	case vex_nrch_gen_br_req: 				return "nrch_gen_br_req";
	case vex_nrch_gen_uj_req: 				return "nrch_gen_uj_req";
	case vex_nrch_gen_db_req: 				return "nrch_gen_db_req";
	case vex_nrch_gen_fje: 				return "nrch_gen_fje_req";
	case vex_nrch_gen_je: 				return "nrch_gen_je_req";
	case vex_nrch_gen_uj_rsp:			return "nrch_gen_uj_rsp";
	case vex_nrch_gen_br_rsp:			return "nrch_gen_br_rsp";
	case vex_nrch_gen_db_rsp:			return "nrch_gen_db_rsp";
	case vex_nrch_gen_fje_ent:			return "nrch_gen_fje_ent";
	
	default: break;
	}
	return "invalid";
}

#endif
