#ifndef NVMEIBS_VEX_H
#define NVMEIBS_VEX_H

/* Uncomment to print VEX_OPS macro logging */
// #define VEX_OPS_V 2

#include "nvmeib_version_shared.h"
#include "nvmeib_vex.h"

#ifndef S_CLIENT_C
#	define VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT	DECLARE_STRUCT
#	define VEX_S_CLIENT_C_DECL_OR_DEF_FN		NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	define VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT	DEFINE_STRUCT
#	define VEX_S_CLIENT_C_DECL_OR_DEF_FN		DECLARE_FNS
#endif

#ifndef S_TOMA_C
#	define VEX_S_TOMA_C_DECL_OR_DEF_STRUCT		DECLARE_STRUCT
#	define VEX_S_TOMA_C_DECL_OR_DEF_FNS			NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	define VEX_S_TOMA_C_DECL_OR_DEF_STRUCT		DEFINE_STRUCT
#	define VEX_S_TOMA_C_DECL_OR_DEF_FNS			DECLARE_FNS
#endif

/* VEX: Admin Channel Server Functions and OPs */

/* VEX: vex_ach_shared_cfg */
VEX_OPS(vex_ach_shared_cfg, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_shared_cfg_srv_ops, "ShareCfg",
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_share_base, var_sz,
					decode, static, vex_ach_shared_cfg_srv_base_decode);

/* VEX: vex_ach_get_io_alloc_disk */
VEX_OPS(vex_ach_get_io_alloc_disk, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, ONE_EXT, ONE_OP,
			vex_ach_get_io_alloc_disk_srv_ops, "DiskInfo",
				base, (NVMEIB_BASE_VERSION_INIT), wire_get_io_disks_info_base, const_sz,
					encode, static, vex_ach_get_io_alloc_disk_srv_base_encode,
				ext1, (NVMEIB_20_VERSION_INIT), wire_get_io_disks_info_ext1, const_sz,
					encode, static, vex_ach_get_io_alloc_disk_srv_ext1_encode);

/* VEX: vex_ach_get_io_port_info */
VEX_OPS(vex_ach_get_io_port_info, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, THREE_EXT, ONE_OP,
			vex_ach_get_io_port_info_srv_ops, "PortInfo",
				base, (NVMEIB_BASE_VERSION_INIT), wire_get_io_ports_info_base, const_sz,
					encode, static, vex_ach_get_io_port_info_srv_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), wire_get_io_ports_info_ext1, const_sz,
					encode, static, vex_ach_get_io_port_info_srv_ext1_encode,
				ext2, (NVMEIB_2p5_VERSION_INIT), wire_get_io_ports_info_ext2, const_sz,
					encode, static, vex_ach_get_io_port_info_srv_ext2_encode,
				ext3, (NVMEIB_2p7_VERSION_INIT), wire_get_io_ports_info_ext3, const_sz,
					encode, static, vex_ach_get_io_port_info_srv_ext3_encode);

/* VEX: vex_ach_acs_map_clnt_ionics */
VEX_OPS(vex_ach_acs_map_clnt_ionics, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_clnt_ionics_srv_ops, "CltIoNic",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_clnt_ionics_base, const_sz,
					decode, static, vex_ach_acs_map_clnt_ionics_srv_base_decode);

/* VEX: vex_ach_acs_map_srv_ionics */
VEX_OPS(vex_ach_acs_map_srv_ionics, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_srv_ionics_srv_ops, "SrvIoNic",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_srv_ionics_base, var_sz,
					decode, static, vex_ach_acs_map_srv_ionics_srv_base_decode);

/* VEX: vex_ach_acs_map_disks */
VEX_OPS(vex_ach_acs_map_disks, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_disks_srv_ops, "AcsDisks",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_disks_base, var_sz,
					decode, static, vex_ach_acs_map_disks_srv_base_decode);

/* VEX: vex_ach_acs_map_arnics */
VEX_OPS(vex_ach_acs_map_arnics, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_arnics_srv_ops, "AcsArnic",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_arnic_base, var_sz,
					decode, static, vex_ach_acs_map_arnics_srv_base_decode);

/* VEX: vex_ach_acs_map_disk_info */
VEX_OPS(vex_ach_acs_map_disk_info, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, ONE_EXT, ONE_OP,
	vex_ach_acs_map_disk_info_srv_ops, "AcsDInfo",
	base, (NVMEIB_BASE_VERSION_INIT), volume_server_config_access_map_per_disk_rsp_base, const_sz,
		encode, static, vex_ach_acs_map_disk_info_srv_base_encode,
	ext1, (NVMEIB_2p5_VERSION_INIT), volume_server_config_access_map_per_disk_rsp_ext1, var_sz,
		encode, static, vex_ach_acs_map_disk_info_srv_ext1_encode);

/* VEX: vex_ach_acs_map_di_rsrc_set */
VEX_OPS(vex_ach_acs_map_di_rsrc_set, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
	vex_ach_acs_map_di_rsrc_set_srv_ops, "DiRscSet",
	base, (NVMEIB_BASE_VERSION_INIT), wire_disk_rsc_set_base, const_sz,
		encode, static, vex_ach_acs_map_di_rsrc_set_srv_base_encode);

/* VEX: vex_ach_lock_mem_seg_info */
VEX_OPS(vex_ach_lock_mem_seg_info, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, TWO_EXT, ONE_OP,
	vex_ach_lock_mem_seg_info_srv_ops, "LkMemSeg",
	base, (NVMEIB_BASE_VERSION_INIT), volume_server_config_per_segment_lock_info_base, const_sz,
	encode, static, vex_ach_lock_mem_seg_info_clnt_base_encode,
	ext1, (NVMEIB_2p5_VERSION_INIT), volume_server_config_per_segment_lock_info_ext1, const_sz,
	encode, static, vex_ach_lock_mem_seg_info_clnt_ext1_encode,
	ext2, (NVMEIB_3p1_VERSION_INIT), volume_server_config_per_segment_lock_info_ext2, const_sz,
	encode, static, vex_ach_lock_mem_seg_info_clnt_ext2_encode);

/* VEX: vex_ach_get_io */
VEX_OPS(vex_ach_get_io, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_get_io_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_ma_get_io_base, const_sz,
					decode, static, vex_ach_get_io_srv_base_decode);

/* VEX: vex_ach_get_acs */
VEX_OPS(vex_ach_get_acs, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, ONE_EXT, ONE_OP,
		vex_ach_get_acs_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
			base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_ma_access_base, const_sz, 
				decode, static, vex_ach_get_acs_srv_base_decode,
			ext1, (NVMEIB_3p0_VERSION_INIT), volume_client_config_ma_access_ext1, const_sz,
				decode, static, vex_ach_get_acs_srv_ext1_decode);

/* VEX: vex_ach_dbg_cmd */
VEX_OPS(vex_ach_dbg_cmd, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, BASE_ONLY, ONE_OP,
			vex_ach_dbg_cmd_srv_ops, "DebugCmd",
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_dbg_req_base, const_sz, 
					decode, static, vex_ach_dbg_cmd_base_srv_decode);

/* VEX: vex_ach_get_jrange_req */
VEX_OPS(vex_ach_get_jrange_req, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, THREE_EXT, ONE_OP,
			vex_ach_get_jrange_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_get_jrange_base, const_sz,
					decode, static, vex_ach_get_jrange_req_srv_base_decode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_client_config_get_jrange_ext1, const_sz,
					decode, static, vex_ach_get_jrange_req_srv_ext1_decode,
				ext2, (NVMEIB_2p3_VERSION_INIT), volume_client_config_get_jrange_ext2, const_sz,
					decode, static, vex_ach_get_jrange_req_srv_ext2_decode,
				ext3, (NVMEIB_3p0_VERSION_INIT), volume_client_config_get_jrange_ext3, const_sz,
					decode, static, vex_ach_get_jrange_req_srv_ext3_decode);

/* VEX: vex_ach_get_jrange_rsp */
VEX_OPS(vex_ach_get_jrange_rsp, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, THREE_EXT, ONE_OP,
			vex_ach_get_jrange_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_get_jrange_rsp_base, const_sz,
					encode, static, vex_ach_get_jrange_rsp_srv_base_encode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_server_get_jrange_rsp_ext1, const_sz,
					encode, static, vex_ach_get_jrange_rsp_srv_ext1_encode,
				ext2, (NVMEIB_2p3_VERSION_INIT), volume_server_get_jrange_rsp_ext2, const_sz,
					encode, static, vex_ach_get_jrange_rsp_srv_ext2_encode,
				ext3, (NVMEIB_3p0_VERSION_INIT), volume_server_get_jrange_rsp_ext3, const_sz,
					encode, static, vex_ach_get_jrange_rsp_srv_ext3_encode);

/* VEX: vex_ach_get_jmdc_rsp */
VEX_OPS(vex_ach_get_jmdc_rsp, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, ONE_EXT, ONE_OP,
			vex_ach_get_jmdc_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_get_jmdc_rsp_base, const_sz,
					encode, static, vex_ach_get_jmdc_rsp_srv_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_get_jmdc_rsp_ext1, const_sz,
					encode, static, vex_ach_get_jmdc_rsp_srv_ext1_encode);
	
/* VEX: vex_ach_get_jmdc_rng_hdr */
VEX_OPS(vex_ach_get_jmdc_rng_hdr, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, ONE_EXT, ONE_OP,
			vex_ach_get_jmdc_rng_hdr_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_get_jmdc_rng_data_base, const_sz,
					encode, static, vex_ach_get_jmdc_rng_hdr_srv_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_get_jmdc_rng_data_ext1, const_sz,
					encode, static, vex_ach_get_jmdc_rng_hdr_srv_ext1_encode);

/* VEX: vex_ach_get_lock_gids_rsp */
VEX_OPS(vex_ach_get_lock_gids_rsp, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT, VEX_S_CLIENT_C_DECL_OR_DEF_FN, TWO_EXT, ONE_OP,
			vex_ach_get_lock_gids_rsp_srv_ops, "LockGids",
				base, (NVMEIB_BASE_VERSION_INIT), wire_lock_gid_base, const_sz,
					encode, static, vex_ach_get_lock_gids_rsp_clnt_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), wire_lock_gid_ext1, const_sz,
					encode, static, vex_ach_get_lock_gids_rsp_clnt_ext1_encode,
				ext2, (NVMEIB_2p7_VERSION_INIT), wire_lock_gid_ext2, const_sz,
					encode, static, vex_ach_get_lock_gids_rsp_clnt_ext2_encode);

/* VEX: vex_ach_abnd_free */
VEX_OPS(vex_ach_abnd_free, VEX_S_CLIENT_C_DECL_OR_DEF_STRUCT,
		VEX_S_CLIENT_C_DECL_OR_DEF_FN, ONE_EXT, ONE_OP,
		vex_ach_abnd_free_srv_ops, "AbndFree",
		base, (NVMEIB_BASE_VERSION_INIT), volume_server_cmd_jmd_free_abnd_base, const_sz,
			encode, static, vex_ach_abnd_free_srv_base_encode,
		ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_cmd_jmd_free_abnd_ext1, const_sz,
			encode, static, vex_ach_abnd_free_srv_ext1_encode);

/* VEX: vex_ach_clnt_toma_req */
VEX_OPS(vex_ach_clnt_toma_req, VEX_S_TOMA_C_DECL_OR_DEF_STRUCT, VEX_S_TOMA_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_ach_clnt_toma_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_toma_req_base, var_sz,
					decode, static, vex_ach_clnt_toma_req_srv_base_decode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_client_toma_req_ext1, var_sz,
					decode, static, vex_ach_clnt_toma_req_srv_ext1_decode);

/* VEX: vex_ach_toma_clnt_req */
VEX_OPS(vex_ach_toma_clnt_req, VEX_S_TOMA_C_DECL_OR_DEF_STRUCT, VEX_S_TOMA_C_DECL_OR_DEF_FNS, BASE_ONLY, NO_OPS,
			vex_ach_toma_clnt_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_toma_req_base, var_sz);

extern const struct vex_ops * const vex_ach_srv_ops_collection[vex_ach_ops_num];

/* VEX: NoRDDA Channel Server Functions and OPs */
#ifndef S_NORDDA_C
#	define VEX_S_NRCH_C_DECL_OR_DEF_STRUCT		DECLARE_STRUCT
#	define VEX_S_NRCH_C_DECL_OR_DEF_FNS			NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	define VEX_S_NRCH_C_DECL_OR_DEF_STRUCT		DEFINE_STRUCT
#	define VEX_S_NRCH_C_DECL_OR_DEF_FNS			DECLARE_FNS
#endif

/* VEX: vex_io_srv_lock_srv_ops */
VEX_OPS(vex_nrch_io_srv_lock_req, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_io_srv_lock_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_lock_req_base, const_sz,
					decode, static, vex_nrch_io_srv_lock_req_base_decode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_client_lock_req_ext1, const_sz,
					decode, static, vex_nrch_io_srv_lock_req_ext1_decode);

VEX_OPS(vex_nrch_io_srv_lock_rsp, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_io_srv_lock_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_lock_rsp_base, const_sz,
					encode, static, vex_nrch_io_srv_lock_rsp_base_encode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_server_lock_rsp_ext1, const_sz,
					encode, static, vex_nrch_io_srv_lock_rsp_ext1_encode);

/* VEX: vex_nrch_gen_br_req_srv_ops */
VEX_OPS(vex_nrch_gen_br_req, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_br_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_blkset_recovered_base, const_sz,
					decode, static, vex_nrch_gen_br_req_srv_base_decode);

VEX_OPS(vex_nrch_gen_br_rsp, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_br_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_gen_rsp_blkset_recovered_base, const_sz,
					encode, static, vex_nrch_gen_br_rsp_srv_base_encode);

/* VEX: vex_nrch_gen_uj_req_srv_ops */
VEX_OPS(vex_nrch_gen_uj_req, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_gen_uj_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_uuid_jour_base, const_sz,
					decode, static, vex_nrch_gen_uj_req_srv_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_client_gen_req_uuid_jour_ext1, const_sz,
					decode, static, vex_nrch_gen_uj_req_srv_ext1_decode);

VEX_OPS(vex_nrch_gen_uj_rsp, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, TWO_EXT, ONE_OP,
			vex_nrch_gen_uj_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_gen_rsp_uuid_jour_base, const_sz,
					encode, static, vex_nrch_gen_uj_rsp_srv_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_gen_rsp_uuid_jour_ext1, const_sz,
					encode, static, vex_nrch_gen_uj_rsp_srv_ext1_encode,
				ext2, (NVMEIB_3p0_VERSION_INIT), volume_server_gen_rsp_uuid_jour_ext2, const_sz,
					encode, static, vex_nrch_gen_uj_rsp_srv_ext2_encode);

/* VEX: vex_nrch_gen_db_req_srv_ops */
VEX_OPS(vex_nrch_gen_db_req, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_db_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), disk_req_get_ec_dirty_bits_base, const_sz,
					decode, static, vex_nrch_gen_db_req_srv_base_decode);

VEX_OPS(vex_nrch_gen_db_rsp, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_db_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_gen_rsp_get_dirty_bits_base, const_sz,
					encode, static, vex_nrch_gen_db_rsp_srv_base_encode);

/* VEX: vex_nrch_gen_fje_srv_ops */
VEX_OPS(vex_nrch_gen_fje, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_fje_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_free_ents, const_sz,
					decode, static, vex_nrch_gen_fje_srv_base_decode);

/* VEX: vex_nrch_gen_fje_ent_clnt_ops */
VEX_OPS(vex_nrch_gen_fje_ent, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_gen_fje_ent_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), wire_free_ents_entry_base, const_sz,
					decode, static, vex_nrch_gen_fje_ent_srv_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), wire_free_ents_entry_ext1, const_sz,
					decode, static, vex_nrch_gen_fje_ent_srv_ext1_decode);

/* VEX: vex_nrch_gen_je_srv_ops */
VEX_OPS(vex_nrch_gen_je, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_gen_je_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_jentry_erase_base, const_sz,
					decode, static, vex_nrch_gen_je_srv_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_client_gen_req_jentry_erase_ext1, const_sz,
					decode, static, vex_nrch_gen_je_srv_ext1_decode);

/* VEX: vex_nrch_io_req_srv_ops */
VEX_OPS(vex_nrch_io_req, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, TWO_EXT, ONE_OP,
			vex_nrch_io_req_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_io_req_base, const_sz,
					decode, static, vex_nrch_io_req_srv_base_decode,
				ext1, (NVMEIB_2p2_VERSION_INIT), volume_client_io_req_ext1, const_sz,
					decode, static, vex_nrch_io_req_srv_ext1_decode,
				ext2, (NVMEIB_3p3_VERSION_INIT), volume_client_io_req_ext2, const_sz,
					decode, static, vex_nrch_io_req_srv_ext2_decode);

/* VEX: vex_nrch_io_rsp_srv_ops */
VEX_OPS(vex_nrch_io_rsp, VEX_S_NRCH_C_DECL_OR_DEF_STRUCT, VEX_S_NRCH_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_io_rsp_srv_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_io_rsp, const_sz,
					encode, static, vex_nrch_io_rsp_srv_base_encode);

extern const struct vex_ops *const vex_nrch_srv_ops_collection[vex_nrch_ops_num];

#endif
