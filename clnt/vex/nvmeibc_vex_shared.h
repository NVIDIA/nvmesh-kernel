#ifndef NVMEIBC_VEX_H
#define NVMEIBC_VEX_H

/* Uncomment to print VEX_OPS macro logging */
//#define VEX_OPS_V 2

#include "nvmeib_version_shared.h"
#include "nvmeib_vex.h"

/* VEX: Admin Channel Client Functions and OPs */

#ifndef C_IB_ADMIN_CHANNEL_C
#	define VEX_C_IB_ACH_DECL_OR_DEF_STRUCT 		DECLARE_STRUCT
#	define VEX_C_IB_ACH_DECL_OR_DEF_FNS 		NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	include "nvmeibs_msgs_shared.h"
#	define VEX_C_IB_ACH_DECL_OR_DEF_STRUCT 		DEFINE_STRUCT
#	define VEX_C_IB_ACH_DECL_OR_DEF_FNS 		DECLARE_FNS
#endif
	
#ifndef C_TOMA_C
#	define VEX_C_TOMA_C_DECL_OR_DEF_STRUCT		DECLARE_STRUCT
#	define VEX_C_TOMA_C_DECL_OR_DEF_FNS			NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	include "nvmeibs_msgs_shared.h"
#	define VEX_C_TOMA_C_DECL_OR_DEF_STRUCT		DEFINE_STRUCT
#	define VEX_C_TOMA_C_DECL_OR_DEF_FNS			DECLARE_FNS
#endif

#ifndef C_JAM_C
#	define VEX_C_JAM_C_DECL_OR_DEF_STRUCT 		DECLARE_STRUCT
#	define VEX_C_JAM_C_DECL_OR_DEF_FNS	 		NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	include "nvmeibs_msgs_shared.h"
#	define VEX_C_JAM_C_DECL_OR_DEF_STRUCT 		DEFINE_STRUCT
#	define VEX_C_JAM_C_DECL_OR_DEF_FNS 		DECLARE_FNS
#endif

/* VEX: vex_ach_shared_cfg */
VEX_OPS(vex_ach_shared_cfg, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_shared_cfg_clnt_ops, "ShareCfg",
				base, (NVMEIB_20_VERSION_INIT), volume_client_config_share_base, var_sz,
					encode, static, vex_ach_shared_cfg_clnt_base_encode);

/* VEX: vex_ach_get_io_alloc_disk */
VEX_OPS(vex_ach_get_io_alloc_disk, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_ach_get_io_alloc_disk_clnt_ops, "DiskInfo",
				base, (NVMEIB_BASE_VERSION_INIT), wire_get_io_disks_info_base, const_sz,
					decode, static, vex_ach_get_io_alloc_disk_clnt_base_decode,
				ext1, (NVMEIB_20_VERSION_INIT), wire_get_io_disks_info_ext1, const_sz,
					decode, static, vex_ach_get_io_alloc_disk_clnt_ext1_decode);

/* VEX: vex_ach_get_io_port_info */
VEX_OPS(vex_ach_get_io_port_info, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, THREE_EXT, ONE_OP,
			vex_ach_get_io_port_info_clnt_ops, "PortInfo",
				base, (NVMEIB_BASE_VERSION_INIT), wire_get_io_ports_info_base, const_sz,
					decode, static, vex_ach_get_io_port_info_clnt_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), wire_get_io_ports_info_ext1, const_sz,
					decode, static, vex_ach_get_io_port_info_clnt_ext1_decode,
				ext2, (NVMEIB_2p5_VERSION_INIT), wire_get_io_ports_info_ext2, const_sz,
					decode, static, vex_ach_get_io_port_info_clnt_ext2_decode,
				ext3, (NVMEIB_2p7_VERSION_INIT), wire_get_io_ports_info_ext3, const_sz,
					decode, static, vex_ach_get_io_port_info_clnt_ext3_decode);

/* VEX: vex_ach_acs_map_clnt_ionics */
VEX_OPS(vex_ach_acs_map_clnt_ionics, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_clnt_ionics_clnt_ops, "CltIoNic",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_clnt_ionics_base, const_sz,
					encode, static, vex_ach_acs_map_clnt_ionics_clnt_base_encode);

/* VEX: vex_ach_acs_map_srv_ionics */
VEX_OPS(vex_ach_acs_map_srv_ionics, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_srv_ionics_clnt_ops, "SrvIoNic",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_srv_ionics_base, var_sz,
					encode, static, vex_ach_acs_map_srv_ionics_clnt_base_encode);

/* VEX: vex_ach_acs_map_disks */
VEX_OPS(vex_ach_acs_map_disks, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_disks_clnt_ops, "AcsDisks",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_disks_base, var_sz,
					encode, static, vex_ach_acs_map_disks_clnt_base_encode);

/* VEX: vex_ach_acs_map_arnics */
VEX_OPS(vex_ach_acs_map_arnics, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_acs_map_arnics_clnt_ops, "AcsArnic",
				base, (NVMEIB_BASE_VERSION_INIT), wire_acs_map_arnic_base, var_sz,
					encode, static, vex_ach_acs_map_arnics_clnt_base_encode);

/* VEX: vex_ach_acs_map_disk_info */
VEX_OPS(vex_ach_acs_map_disk_info, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
	vex_ach_acs_map_disk_info_clnt_ops, "AcsDInfo",
	base, (NVMEIB_BASE_VERSION_INIT), volume_server_config_access_map_per_disk_rsp_base, const_sz,
		decode, static, vex_ach_acs_map_disk_info_clnt_base_decode,
	ext1, (NVMEIB_2p5_VERSION_INIT), volume_server_config_access_map_per_disk_rsp_ext1, var_sz,
		decode, static, vex_ach_acs_map_disk_info_clnt_ext1_decode);

/* VEX: vex_ach_acs_map_di_rsrc_set */
VEX_OPS(vex_ach_acs_map_di_rsrc_set, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
	vex_ach_acs_map_di_rsrc_set_clnt_ops, "DiRscSet",
	base, (NVMEIB_BASE_VERSION_INIT), wire_disk_rsc_set_base, const_sz,
		decode, static, vex_ach_acs_map_di_rsrc_set_clnt_base_decode);

/* VEX: vex_ach_lock_mem_seg_info */
VEX_OPS(vex_ach_lock_mem_seg_info, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, TWO_EXT, ONE_OP,
	vex_ach_lock_mem_seg_info_clnt_ops, "LkMemSeg",
	base, (NVMEIB_BASE_VERSION_INIT), volume_server_config_per_segment_lock_info_base, const_sz,
	decode, static, vex_ach_lock_mem_seg_info_clnt_base_decode,
	ext1, (NVMEIB_2p5_VERSION_INIT), volume_server_config_per_segment_lock_info_ext1, const_sz,
	decode, static, vex_ach_lock_mem_seg_info_clnt_ext1_decode,
	ext2, (NVMEIB_3p1_VERSION_INIT), volume_server_config_per_segment_lock_info_ext2, const_sz,
	decode, static, vex_ach_lock_mem_seg_info_clnt_ext2_decode);

/* VEX: vex_ach_get_acs */
VEX_OPS(vex_ach_get_acs, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
		vex_ach_get_acs_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
			base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_ma_access_base, const_sz, 
				encode, static, vex_ach_get_acs_clnt_base_encode,
			ext1, (NVMEIB_3p0_VERSION_INIT), volume_client_config_ma_access_ext1, const_sz,
				encode, static, vex_ach_get_acs_clnt_ext1_encode);

/* VEX: vex_ach_get_io */
VEX_OPS(vex_ach_get_io, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_get_io_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_ma_get_io_base, const_sz, 
					encode, static, vex_ach_get_io_clnt_base_encode);

/* VEX: vex_ach_dbg_cmd */
VEX_OPS(vex_ach_dbg_cmd, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_dbg_cmd_clnt_ops, "DebugCmd",
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_dbg_req_base, const_sz, 
					encode, static, vex_ach_dbg_cmd_base_clnt_encode);

/* VEX: vex_ach_get_jrange_req */
VEX_OPS(vex_ach_get_jrange_req, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, THREE_EXT, ONE_OP,
			vex_ach_get_jrange_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_config_get_jrange_base, const_sz,
					encode, static, vex_ach_get_jrange_req_clnt_base_encode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_client_config_get_jrange_ext1, const_sz,
					encode, static, vex_ach_get_jrange_req_clnt_ext1_encode,
				ext2, (NVMEIB_2p3_VERSION_INIT), volume_client_config_get_jrange_ext2, const_sz,
					encode, static, vex_ach_get_jrange_req_clnt_ext2_encode,
				ext3, (NVMEIB_3p0_VERSION_INIT), volume_client_config_get_jrange_ext3, const_sz,
					encode, static, vex_ach_get_jrange_req_clnt_ext3_encode);

/* VEX: vex_ach_get_jrange_rsp */
VEX_OPS(vex_ach_get_jrange_rsp, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, THREE_EXT, ONE_OP,
			vex_ach_get_jrange_rsp_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_get_jrange_rsp_base, const_sz,
					decode, static, vex_ach_get_jrange_rsp_clnt_base_decode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_server_get_jrange_rsp_ext1, const_sz,
					decode, static, vex_ach_get_jrange_rsp_clnt_ext1_decode,
				ext2, (NVMEIB_2p3_VERSION_INIT), volume_server_get_jrange_rsp_ext2, const_sz,
					decode, static, vex_ach_get_jrange_rsp_clnt_ext2_decode,
				ext3, (NVMEIB_3p0_VERSION_INIT), volume_server_get_jrange_rsp_ext3, const_sz,
					decode, static, vex_ach_get_jrange_rsp_clnt_ext3_decode);

/* VEX: vex_ach_get_jmdc_rsp */
VEX_OPS(vex_ach_get_jmdc_rsp, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_ach_get_jmdc_rsp_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_get_jmdc_rsp_base, const_sz,
					decode, static, vex_ach_get_jmdc_rsp_clnt_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_get_jmdc_rsp_ext1, const_sz,
					decode, static, vex_ach_get_jmdc_rsp_clnt_ext1_decode);

/* VEX: vex_ach_get_jmdc_rng_hdr */
VEX_OPS(vex_ach_get_jmdc_rng_hdr, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_ach_get_jmdc_rng_hdr_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_get_jmdc_rng_data_base, const_sz,
					decode, static, vex_ach_get_jmdc_rng_hdr_clnt_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_get_jmdc_rng_data_ext1, const_sz,
					decode, static, vex_ach_get_jmdc_rng_hdr_clnt_ext1_decode);

/* VEX: vex_ach_get_lock_gids_rsp */
VEX_OPS(vex_ach_get_lock_gids_rsp, VEX_C_IB_ACH_DECL_OR_DEF_STRUCT, VEX_C_IB_ACH_DECL_OR_DEF_FNS, TWO_EXT, ONE_OP,
			vex_ach_get_lock_gids_rsp_clnt_ops, "LockGids",
				base, (NVMEIB_BASE_VERSION_INIT), wire_lock_gid_base, const_sz,
					decode, static, vex_ach_get_lock_gids_rsp_clnt_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), wire_lock_gid_ext1, const_sz,
					decode, static, vex_ach_get_lock_gids_rsp_clnt_ext1_decode,
				ext2, (NVMEIB_2p7_VERSION_INIT), wire_lock_gid_ext2, const_sz,
					decode, static, vex_ach_get_lock_gids_rsp_clnt_ext2_decode);

/* VEX: vex_ach_clnt_toma_req */
VEX_OPS(vex_ach_clnt_toma_req, VEX_C_TOMA_C_DECL_OR_DEF_STRUCT, VEX_C_TOMA_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_ach_clnt_toma_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_toma_req_base, var_sz,
					encode, static, vex_ach_clnt_toma_req_clnt_base_encode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_client_toma_req_ext1, var_sz,
					encode, static, vex_ach_clnt_toma_req_clnt_ext1_encode);

/* VEX: vex_ach_toma_clnt_req */
VEX_OPS(vex_ach_toma_clnt_req, VEX_C_TOMA_C_DECL_OR_DEF_STRUCT, VEX_C_TOMA_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_ach_toma_clnt_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_toma_req_base, var_sz,
					decode, static, vex_ach_toma_clnt_req_clnt_base_decode);

/* VEX: vex_ach_abnd_free */
VEX_OPS(vex_ach_abnd_free, VEX_C_JAM_C_DECL_OR_DEF_STRUCT,
		VEX_C_JAM_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
		vex_ach_abnd_free_clnt_ops, "AbndFree",
		base, (NVMEIB_BASE_VERSION_INIT), volume_server_cmd_jmd_free_abnd_base, const_sz,
			decode, static, vex_ach_abnd_free_base_decode,
		ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_cmd_jmd_free_abnd_ext1, const_sz,
			decode, static, vex_ach_abnd_free_ext1_decode);

extern const struct vex_ops *const vex_ach_clnt_ops_collection[vex_ach_ops_num];

/* VEX: NoRDDA Channel Client Functions and OPs */

#ifndef C_IB_NET_NR_C
#	define VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT		DECLARE_STRUCT
#	define VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS		NO_DECLARE_FNS
#else
#	include "nvmeibc_msgs_shared.h"
#	include "nvmeibs_msgs_shared.h"
#	define VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT		DEFINE_STRUCT
#	define VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS		DECLARE_FNS
#endif

/* VEX: vex_nrch_io_read_clnt_ops */
VEX_OPS(vex_nrch_io_read, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, TWO_EXT, TWO_OPS,
			vex_nrch_io_read_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_io_req_base, const_sz,
					encode, static, vex_nrch_io_read_clnt_base_encode,
					fini, static, vex_nrch_io_read_clnt_base_fini,
				ext1, (NVMEIB_2p2_VERSION_INIT), volume_client_io_req_ext1, const_sz,
					encode, static, vex_nrch_io_read_clnt_ext1_encode,
					fini, static, vex_nrch_io_read_clnt_ext1_fini,
				ext2, (NVMEIB_3p3_VERSION_INIT), volume_client_io_req_ext2, const_sz,
					encode, static, vex_nrch_io_read_clnt_ext2_encode,
					fini, static, vex_nrch_io_read_clnt_ext2_fini);

/* VEX: vex_nrch_io_other_clnt_ops */
VEX_OPS(vex_nrch_io_other, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, TWO_EXT, THREE_OPS,
			vex_nrch_io_other_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_io_req_base, const_sz, 
					init, static, vex_nrch_io_other_clnt_base_init,
					encode, static, vex_nrch_io_other_clnt_base_encode,
					fini, static, vex_nrch_io_other_clnt_base_fini,
				ext1, (NVMEIB_2p2_VERSION_INIT), volume_client_io_req_ext1, const_sz, 
					init, static, vex_nrch_io_other_clnt_ext1_init,
					encode, static, vex_nrch_io_other_clnt_ext1_encode,
					fini, static, vex_nrch_io_other_clnt_ext1_fini,
				ext2, (NVMEIB_3p3_VERSION_INIT), volume_client_io_req_ext2, const_sz, 
					init, static, vex_nrch_io_other_clnt_ext2_init,
					encode, static, vex_nrch_io_other_clnt_ext2_encode,
					fini, static, vex_nrch_io_other_clnt_ext2_fini);

/* VEX: vex_io_srv_lock_clnt_ops */
VEX_OPS(vex_nrch_io_srv_lock_req, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_io_srv_lock_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_lock_req_base, const_sz,
					encode, static, vex_nrch_io_srv_lock_req_clnt_base_encode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_client_lock_req_ext1, const_sz,
					encode, static, vex_nrch_io_srv_lock_req_clnt_ext1_encode);

VEX_OPS(vex_nrch_io_srv_lock_rsp, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_io_srv_lock_rsp_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_lock_rsp_base, const_sz,
					decode, static, vex_nrch_io_srv_lock_rsp_clnt_base_decode,
				ext1, (NVMEIB_20_VERSION_INIT), volume_server_lock_rsp_ext1, const_sz,
					decode, static, vex_nrch_io_srv_lock_rsp_clnt_ext1_decode);

/* VEX: vex_nrch_gen_br_req_clnt_ops */
VEX_OPS(vex_nrch_gen_br_req, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_br_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_blkset_recovered_base, const_sz,
					encode, static, vex_nrch_gen_br_req_clnt_base_encode);

/* VEX: vex_nrch_gen_uj_req_clnt_ops */
VEX_OPS(vex_nrch_gen_uj_req, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_gen_uj_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_uuid_jour_base, const_sz,
					encode, static, vex_nrch_gen_uj_req_clnt_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_client_gen_req_uuid_jour_ext1, const_sz,
					encode, static, vex_nrch_gen_uj_req_clnt_ext1_encode);

/* VEX: vex_nrch_gen_uj_req_clnt_ops */
VEX_OPS(vex_nrch_gen_uj_rsp, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, TWO_EXT, ONE_OP,
			vex_nrch_gen_uj_rsp_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_server_gen_rsp_uuid_jour_base, const_sz,
					decode, static, vex_nrch_gen_uj_rsp_clnt_base_decode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_server_gen_rsp_uuid_jour_ext1, const_sz,
					decode, static, vex_nrch_gen_uj_rsp_clnt_ext1_decode,
				ext2, (NVMEIB_3p0_VERSION_INIT), volume_server_gen_rsp_uuid_jour_ext2, const_sz,
					decode, static, vex_nrch_gen_uj_rsp_clnt_ext2_decode);

/* VEX: vex_nrch_gen_db_req_clnt_ops */
VEX_OPS(vex_nrch_gen_db_req, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_db_req_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), disk_req_get_ec_dirty_bits_base, const_sz,
					encode, static, vex_nrch_gen_db_req_clnt_base_encode);

/* VEX: vex_nrch_gen_fje_clnt_ops */
VEX_OPS(vex_nrch_gen_fje, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, BASE_ONLY, ONE_OP,
			vex_nrch_gen_fje_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_free_ents_base, const_sz,
					encode, static, vex_nrch_gen_fje_clnt_base_encode);


/* VEX: vex_nrch_gen_fje_ent_clnt_ops */
VEX_OPS(vex_nrch_gen_fje_ent, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_gen_fje_ent_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), wire_free_ents_entry_base, const_sz,
					encode, static, vex_nrch_gen_fje_ent_clnt_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), wire_free_ents_entry_ext1, const_sz,
					encode, static, vex_nrch_gen_fje_ent_clnt_ext1_encode);

/* VEX: vex_nrch_gen_je_clnt_ops */
VEX_OPS(vex_nrch_gen_je, VEX_C_IB_NET_NR_C_DECL_OR_DEF_STRUCT, VEX_C_IB_NET_NR_C_DECL_OR_DEF_FNS, ONE_EXT, ONE_OP,
			vex_nrch_gen_je_clnt_ops, VEX_OPS_ZERO_MAGIC_STR,
				base, (NVMEIB_BASE_VERSION_INIT), volume_client_gen_req_jentry_erase_base, const_sz,
					encode, static, vex_nrch_gen_je_clnt_base_encode,
				ext1, (NVMEIB_2p3_VERSION_INIT), volume_client_gen_req_jentry_erase_ext1, const_sz,
					encode, static, vex_nrch_gen_je_clnt_ext1_encode);

extern const struct vex_ops *const vex_nrch_clnt_ops_collection[vex_nrch_ops_num];

#endif
