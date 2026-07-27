#ifndef NVMEIBS_MSGS_H
#define NVMEIBS_MSGS_H

#include "nvmeib_consts_shared.h"
#include "nvmeib_msgs_shared.h"
#include "nvmeib_version_shared.h"

#define S_MSGS_MAX(a,b) ((a) > (b) ? (a) : (b))

/* max number of nr-channels per path to the srv's nic
   keeping the default for backword-compat (VEX) */
#define NVMEIB_COMPAT_MAX_NR_CHANNELS_PER_PATH (4)

/* server login response */
enum nvmeibs_login_ops {
	NVMEIBS_ADMIN_CHANNEL 	= 0x01,
	NVMEIBS_IO_CHANNEL 		= 0x02,
	NVMEIBS_LOCK_CHANNEL 	= 0x03,
	NVMEIBS_NORDDA_CHANNEL	= 0x04,
	NVMEIBS_2ND_LOCK_NET	= 0x05,
};

struct nvmeibs_login_new_admin_channel_base {
	char host_name[NVMEIB_HOST_NAME_LEN];
	__be64 s_link_version;
	__be16 max_iu_len;
	__be16 req_limit;
	__be64 msg_buffer_raddr;
	__be32 msg_buffer_pages;
	__be32 msg_buffer_rkey;
} __attribute__((packed));

struct nvmeibs_login_new_io_channel_base {
	__be64 jmdc_raddr;
	__be32 jmdc_len;
	__be32 jmdc_rkey;
	__be64 io_ka_raddr;
	__be32 io_ka_rkey;
} __attribute__((packed));

struct nvmeibs_login_new_nr_channel_base {
	__be64 io_ka_raddr;
	__be32 io_ka_rkey;
} __attribute__((packed));

struct nvmeibs_login_rsp_lock_channel_base {
	u8	atomic_cap;  		/* enum ib_atomic_cap */
	u8	pad[3];
	u8	masked_atomic_cap;	/* enum ib_atomic_cap */
	u8	pad2[3];
	__be32 atomic_test_zone_rkey;
	__be64 atomic_test_zone_raddr;
} __attribute__((packed));

struct nvmeibs_login_new_admin_channel_ext1 {
	__be64 s_version;
} __attribute__((packed));

struct nvmeibs_login_new_admin_channel_ext2 {
	u8 tgt_num_cpus;
	u8 tgt_max_nrchs_per_path;
} __attribute__((packed));

struct nvmeibs_login_new_admin_channel_ext3 {
	u8 tgt_max_nrchs_per_path_tcp;
} __attribute__((packed));

struct nvmeibs_login_new_admin_channel_ext4 {
	__be32 tgt_num_cpus;
	__be32 tgt_max_nrchs_per_path;
	__be32 tgt_max_nrchs_per_path_tcp;
} __attribute__((packed));

struct nvmeibs_login_new_admin_channel_ext6 {
	u8 tgt_pg_sz_shift;
} __attribute__((packed));

struct nvmeibs_login_new_io_channel_ext1 {
} __attribute__((packed));

struct nvmeibs_login_new_nr_channel_ext1 {
} __attribute__((packed));

struct nvmeibs_login_rsp_lock_channel_ext1 {
} __attribute__((packed));

/* client login handling */
struct nvmeibs_login_response_base {
	struct nvmeib_login_wire_op_cid op_cid;
	union {
		struct nvmeibs_login_new_admin_channel_base creq;
		struct nvmeibs_login_new_io_channel_base io_rsp;
		struct nvmeibs_login_rsp_lock_channel_base lrsp;
		struct nvmeibs_login_new_nr_channel_base nr_rsp;
	};
	u8 opcode;
} __attribute__((packed));

struct nvmeibs_login_response_ext1 {
	u8 n_ext;
	union {
		struct nvmeibs_login_new_admin_channel_ext1 ach;
		struct nvmeibs_login_new_io_channel_ext1 io_rsp;
		struct nvmeibs_login_rsp_lock_channel_ext1 lrsp;
		struct nvmeibs_login_new_nr_channel_ext1 nr_rsp;
		/* In previous versions, the type of io_rsp was nvmeibs_login_new_io_channel_base by mistake
		 * resulting in the size of this extension to be padded to 29 bytes. This space can be used in future */
		char pad[28];
	};
} __attribute__((packed));

struct nvmeibs_login_response_ext2 {
	struct nvmeibs_login_new_admin_channel_ext2 ach;
} __attribute__((packed));

struct nvmeibs_login_response_ext3 {
	struct nvmeibs_login_new_admin_channel_ext3 ach;
} __attribute__((packed));

struct nvmeibs_login_response_ext4 {
	struct nvmeibs_login_new_admin_channel_ext4 ach;
} __attribute__((packed));

enum nvmeibs_login_msg_hdr_out_flags {
	NVMEIBS_LOGIN_WITHOUT_MSG_HDR = 0x00,
	NVMEIBS_LOGIN_WITH_MSG_HDR = 0x01,
};

struct nvmeibs_login_response_ext5 {
	struct nvmeib_msg_hdr_login_data msg_hdr;
} __attribute__((packed));


struct nvmeibs_login_response_ext6 {
	struct nvmeibs_login_new_admin_channel_ext6 ach;
} __attribute__((packed));

struct nvmeibs_login_response {
	struct nvmeibs_login_response_base base;
	struct nvmeibs_login_response_ext1 ext1;
	struct nvmeibs_login_response_ext2 ext2;
	struct nvmeibs_login_response_ext3 ext3;
	struct nvmeibs_login_response_ext4 ext4;
	struct nvmeibs_login_response_ext5 ext5;
	struct nvmeibs_login_response_ext6 ext6;
} __attribute__((packed));

enum nvmeib_login_reject_reason {
	NVMEIBS_LOGIN_REJ_UNABLE_ESTABLISH_CONNECTION	= 0x00,
	NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES		= 0x01,
	NVMEIBS_LOGIN_REJ_REQ_IT_IU_INSUFF_CRED			= 0x02,
	NVMEIBS_LOGIN_REJ_CLIENT_IS_ALREADY_CONNECTED	= 0x03,
	NVMEIBS_LOGIN_REJ_CLIENT_MSG_TOO_SMALL			= 0x04,
	NVMEIBS_LOGIN_REJ_NO_SUCH_CLIENT				= 0x05,
	NVMEIBS_LOGIN_REJ_NO_SUCH_DISK					= 0x06,
	NVMEIBS_LOGIN_REJ_INVALID_QPN					= 0x07,
	NVMEIBS_LOGIN_REJ_QPN_EXISTS					= 0x08,
	NVMEIBS_LOGIN_REJ_NO_DISKS_FOUND_ON_CONTROLLER	= 0x09,
	NVMEIBS_LOGIN_REJ_TOMA_NOT_CONNECTED            = 0x0a,
	NVMEIBS_LOGIN_REJ_INVALID_LOCK_PORT          	= 0x0b,
	NVMEIBS_LOGIN_REJ_IO_CHANNEL_SRC_DST_MATCH 		= 0x0c,
	NVMEIBS_LOGIN_REJ_IO_CHANNEL_ALREADY_CONNECTED 	= 0x0d,
	NVMEIBS_LOGIN_REJ_ADMIN_INVALID_GID			 	= 0x0e,
	NVMEIBS_LOGIN_REJ_LOCK_INVALID_GID			 	= 0x1e,
	NVMEIBS_LOGIN_REJ_RDDA_INVALID_GID			 	= 0x2e,
	NVMEIBS_LOGIN_REJ_NRDDA_INVALID_GID			 	= 0x3e,
	NVMEIBS_LOGIN_REJ_LOCK_2ND_INVALID_GID			= 0x4e,
	NVMEIBS_LOGIN_REJ_MODULE_EXIT			= 0x0f,
	NVMEIBS_LOGIN_REJ_PORT_DISABLED			= 0x10,
	NVMEIBS_LOGIN_REJ_LOCAL_DISK_DYING		= 0x11,
	NVMEIBS_LOGIN_REJ_LOCAL_DISK_NOT_FOUND		= 0x12,
	NVMEIBS_LOGIN_REJ_LOCAL_DISK_INVALID_LOCKS	= 0x13,
	NVMEIBS_LOGIN_REJ_NO_PRIMARY_LOCK_CHANNEL		= 0x14,
	NVMEIBS_LOGIN_REJ_MAX_SECONDARY_LOCK_NETS		= 0x15,
	NVMEIBS_LOGIN_REJ_UNSUPPORTED_VERSION			= 0x16,
	NVMEIBS_LOGIN_REJ_LOCK_2ND_NO_PRIMARY 			= 0x17,
	NVMEIBS_LOGIN_REJ_MSG_HDR_NOT_SUPPORTED			= 0x18,
	NVMEIBS_LOGIN_REJ_INVALID_CMD					= 0xff
};

struct nvmeibs_login_reject {
	struct nvmeib_hdr hdr;
	u8 dummy[7];
	__be64 s_version;
	__be32 reason;
	__be32 val;
	u64	tag;
} __attribute__((packed));

/* server requests to client */
enum nvmeibs_cmd_ops {
	NVMEIBS_PUT_RSC = 0x00,
	NVMEIBS_GET_RSC = 0x01,
	NVMEIBS_JAM_ABND2FREE = 0x02,
	NVMEIBS_RGID_CHANGE = 0x03,
	NVMEIBS_IOCH_DRAINED = 0x04,
	NVMEIBS_LOGOUT = 0x05,
};

static inline const char *nvmeibs_cmd_ops_to_str(int op)
{
	switch (op) {
	case NVMEIBS_PUT_RSC: return "PUT";
	case NVMEIBS_GET_RSC: return "GET";
	case NVMEIBS_JAM_ABND2FREE: return "JAM_ABND2FREE";
	case NVMEIBS_RGID_CHANGE: return "RGID_CHANGE";
	case NVMEIBS_IOCH_DRAINED: return "IOCH_DRAINED";
	case NVMEIBS_LOGOUT: return "LOGOUT";
	default: return "???";
	}
}

struct volume_server_cmd_get_req {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	__be64 n;
} __attribute__((packed));

struct volume_server_cmd_put_req {
	struct volume_server_cmd_get_req g_req;
	__be64 ids[NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT];
} __attribute__((packed));

struct volume_server_cmd_jmd_free_abnd_base {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	__be32 rng_num;
	__be64 rng_gen_id;
	__be32 abnd_free_bitmap[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
	struct nvmeib_jrnl_ent_md free_ent_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
} __attribute__((packed));

struct volume_server_cmd_jmd_free_abnd_ext1 {
	__be_binje_t binje;
} __attribute__((packed));

struct volume_server_cmd_jmd_free_abnd {
	struct volume_server_cmd_jmd_free_abnd_base base;
	struct volume_server_cmd_jmd_free_abnd_ext1 ext1;
} __attribute__((packed));

struct volume_server_cmd_rgid_change_req {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u8 hw_gid[16];
	u8 gid[16];
	u8 layer;
	u8 may_access;
} __attribute__((packed));

struct volume_server_cmd_ioch_drained_req {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u8 s_hw_gid[16];
	u8 c_hw_gid[16];
	u8 is_rdda;
	__be16 ch_num;		/* same as use to connect */
	__be64 cs_gid;		/* monotonically-inc session-id of the ch */
} __attribute__((packed));

struct volume_server_toma_req_base {
	u8 position;
	__be64 handle;
	struct nvmeib_container data_ctnr;
} __attribute__((packed));


struct volume_server_toma_req {
	struct volume_server_toma_req_base base;
} __attribute__((packed));

struct volume_server_cmd_logout_reason {
	int reason; /* nvmeibs_logout_reason */
} __attribute__((packed));

struct volume_server_toma_req_max_DONT_USE {
	struct volume_server_toma_req_base base;
	u8 toma_req_data[NVMEIB_TOMA_REQ_MAX_LEN];
} __attribute__((packed));

/*
 * Gregory - type members up to the union are not extendable
 */
struct volume_server_req {
	struct nvmeib_hdr hdr;
	u8 opcode;
	__be16 version_tag;
	u8 dummy[4];
	u8 bytes[0];
	struct {
		union {
			u8 payload[0];
			struct volume_server_cmd_get_req g_req;
			struct volume_server_cmd_put_req p_req;
			struct volume_server_toma_req toma_req;
			struct volume_server_cmd_ioch_drained_req i_req;
			//TBD: move to nrch
			struct volume_server_cmd_jmd_free_abnd j_req;
			struct volume_server_cmd_rgid_change_req r_req;
			/* should be enum */
			struct volume_server_cmd_logout_reason logout_req;
			/* Used to calculate maximum size - DONT USE */
			struct volume_server_toma_req_max_DONT_USE toma_req_max_DONT_USE;
		};
		/* Take offset to get maximum payload size */
		u8 payload_max[0];
	};
} __attribute__((packed));

/* server responses to client request */
enum {
	NVMEIBS_RSP_MGMT_OPCODE_OK = 0x00,
	NVMEIBS_RSP_MGMT_OPCODE_ERR = 0x01,
	NVMEIBS_RSP_IO_OPCODE_OK = 0x02,
	NVMEIBS_RSP_IO_OPCODE_ERR = 0x03,
	NVMEIBS_RSP_TOMA_OPCODE_OK = 0x04,
	NVMEIBS_RSP_TOMA_OPCODE_ERR = 0x05,
	NVMEIBS_RSP_JMD_FREE_ABND_OK	= 0x0a,
	NVMEIBS_RSP_JMD_FREE_ABND_ERR	= 0x0b,
	NVMEIBS_RSP_GEN_OPCODE_OK = 0x0c,
	NVMEIBS_RSP_GEN_OPCODE_ERR = 0x0d,
};

/* server IB HW type */
enum {
	MLNX_mlx4	= 0x04,
	MLNX_mlx5	= 0x05,

	HW_unknown	= 0xff
};

struct volume_server_config_alloc_net_rsp {
	__be64 sendq_buffer_raddr;
	__be32 sendq_buffer_size;
	__be32 sendq_buffer_rkey;
	__be32 sq_wqe_shift;
	__be32 sq_wqe_cnt;
	__be32 sq_spare_wqes;
	__be32 sq_max_wqes_per_wr;
	__be16 sq_last_pos;
	__be16 sq_full_delta;
	__be16 sq_max_sge;
	__be16 qp_mtu;
	__be32 sq_head;
	__be32 db_size_in_db;
	__be64 cq_ci_db_raddr;
	__be32 cq_rkey;
	__be32 cq_cons_index;
	__be32 cq_entries;
	__be64 sqdb_raddr;
	__be32 sqdb_size;
	__be32 sqdb_rkey;
	__be32 sqdb_payload;
	__be64 sqdb_descr_raddr;
	__be32 sqdb_descr_size;
	__be32 sqdb_descr_rkey;
	__be32 qp_id;
	__be32 sq_psn;
} __attribute__((packed));

struct nvmeibs_disk_description {
	__be64 id;
	__be64 bounce_buffer_raddr;
	__be32 bounce_buffer_size;
	__be32 bounce_buffer_lkey;
	__be32 bounce_buffer_rkey;
	__be32 bounce_buffer_n_pages;
	__be64 md_raddr;
	__be32 md_size;
	__be32 md_lkey;
	__be32 md_rkey;
	__be64 prp1_raddr;
	__be32 prp1_size;
	__be32 prp1_rkey;
	__be64 prpl_raddr;
	__be32 prpl_size;
	__be32 prpl_rkey;
	__be32 prpl_n_pages;
	__be32 dummy0;
	__be64 sq_raddr;
	__be32 sq_rkey;
	__be32 sq_entries;
	__be32 dummy1;
	__be64 cq_raddr;
	__be32 cq_lkey;
	__be32 cq_rkey;
	__be32 cq_entries;
	__be64 sq_db_raddr;
	__be32 sq_db_size;
	__be32 sq_db_rkey;
	__be64 cq_db_raddr;
	__be32 cq_db_size;
	__be32 cq_db_rkey;
	__be64 msix_raddr;
	__be32 msix_rkey;
	__be32 dummy2;
	__be64 bb_raddr_nvme[3];
} __attribute__((packed));

struct nvmeibs_disk_seg_lock_info {
	__be64 seg_id;
	__be64 start_disk_address;
	__be64 len;
	__be64 ioaddr;
	__be32 lockset_size;
	__be32 lkey;
	__be32 rkey;
	__be64 lmi;
} __attribute__((packed));

struct volume_server_config_io_info_rsp {
	/* the rdma message format
		m disks infos (4 bytes)
	    for i = 0 to m - 1
	    	(NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE bytes each)
	    end
	    n  pairs of remote io guid + pkeys (4 bytes) + type (4 bytes)
	    for i = 0 to n - 1
			(qid, info) (12 bytes + pkey 4 bytes each +
	    				 type 4 bytes + layer + max_messages returned 4 bytes)
	    end
	*/
} __attribute__((packed));


struct volume_server_client_req_io_area {
	__be64 raddr;
	__be32 rn_pages;
	__be32 rkey;
	struct nvmeib_md_desc md_desc;
} __attribute__((packed));

struct volume_server_config_access_map_gen_rsp {
	__be32 page_size;
	__be32 n_disks;
} __attribute__((packed));

struct volume_server_config_nr_jmdc_desc {
	__be64 raddr;
	__be32 len;
	__be32 rkey;
};

struct volume_server_config_alloc_nr_net_rsp {
	struct volume_server_client_req_io_area a[NVMEIB_MAX_NORDDA_IO_REQ];
	struct volume_server_config_nr_jmdc_desc jmdc_desc;
} __attribute__((packed));

struct volume_server_config_per_segment_lock_info_base {
	__be32 seg_id;
	__be64 start_addr;
	__be64 lock_set_size;
	__be32 len;
	__be32 rkey;
	__be32 lkey; /*the lkey for shadow read*/
	__be64 addr; /*virtual address at remote*/
} __attribute__((packed));

struct volume_server_config_per_segment_lock_info_ext1 {
	__be64 lmi;
} __attribute__((packed));

struct volume_server_config_per_segment_lock_info_ext2 {
	__be64 len64; /* 64-bit length, needed for large disks */
} __attribute__((packed));

struct volume_server_config_per_segment_lock_info {
	struct volume_server_config_per_segment_lock_info_base base;
	struct volume_server_config_per_segment_lock_info_ext1 ext1;
	struct volume_server_config_per_segment_lock_info_ext2 ext2;
} __attribute__((packed));

struct volume_server_config_per_disk_locks_rsp {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	__be32 num_of_memsegs;
} __attribute__((packed));

struct volume_server_get_jmdc_rng_data_base {
	/* Client UUID */
	uuid_be client_uuid;
	/* journal-range index */
	__be16 rng_idx;
	/* Size of journal-range (in LBA), in units of clnt volume blocks (software lba) */
	__be32 rng_size_lba;
	/* Start of Journal Range (in LBA), in units of clnt volume blocks (software lba) */
	__be64 rng_start_lba;
	/* Range Generation ID (used to keep track of recoveries) */
	__be64 rng_gen_id;
	/* If set, only the dirty entries were sent (JMDC and MD), otherwise all entries were sent */
	u8 only_dirty_ents;
	u8 rsvd0[7];
	/* First Entry Offset in (JMDC (only for N = 1) and MD) array */
	__be32 rng_ent_offset;
	/* Dirty Entries Bitmap */
	__be32 dirty_ents_bmp[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
	/* Abnd Entries Bitmap */
	__be32 abnd_ents_bmp[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
} __attribute__((packed));

struct volume_server_get_jmdc_rng_data_ext1 {
	/* First Entry Block Offset in JMDC array */
	__be32 rng_ent_block_offset;
	/* Blocks in journal entry */
	__be32 binje;
	/* Number of journal entries in range */
	__be16 num_ents;
	/* Number of dirty journal entries in range */
	__be16 num_dirty_ents;
} __attribute__((packed));

/* EC-MS TDOO: hide VEX from ulp */
struct volume_server_get_jmdc_rng_data {
	struct volume_server_get_jmdc_rng_data_base base;
	struct volume_server_get_jmdc_rng_data_ext1 ext1;
} __attribute__((packed));

struct volume_server_get_jmdc_rsp_base {
	/* Number of entries in range (when N = 1) */
	__be16 num_ents_rng;
	/* Number of dirty ranges */
	__be16 num_dirty_rng;
	/* Number of ranges */
	__be16 num_rng;
	/* Start Offset of Journal (in nvmeibc_sectors) */
	__be64 jrnl_start_lba;
	/* Length of Journal (in nvmeibc_sectors) */
	__be64 jrnl_len_lba;
	/* Length of header data */
	__be32 hdr_len;
	/* Length of entries */
	__be32 ents_len;
	/* Total entries */
	__be32 num_ents;
	/* Length of entries MD */
	__be32 ents_md_len;
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
} __attribute__((packed));

struct volume_server_get_jmdc_rsp_ext1 {
	/* Version of header data */
	__be16 hdr_version;
	/* Number of bytes between the header of range N and range N + 1 */
	__be16 hdr_stride;
	/* Number of entry blocks */
	__be32 num_ent_blocks;
} __attribute__((packed));

struct volume_server_get_jmdc_rsp {
	struct volume_server_get_jmdc_rsp_base base;
	struct volume_server_get_jmdc_rsp_ext1 ext1;
} __attribute__((packed));

struct volume_server_config_disk_metadata_info {
	__be32 md_size;
	u8 md_extd;
	u8 padd[3];
};

struct volume_server_config_access_map_per_disk_rsp_base {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	__be32 nsid;
	__be32 sector_shift;
	__be32 max_request_size;
	__be32 triplets;
	__be32 disk_rscs_n_sets;
	__be32 disk_rscs;
	__be32 client_rscs;
	__be64 disk_lock_counter;
	__be32 disk_lock_segments;
	struct volume_server_config_disk_metadata_info md;
} __attribute__((packed));

struct wire_disk_rsc_set_base {
	__be64 node_guid;
} __attribute__((packed));

struct wire_disk_rsc_set {
	struct wire_disk_rsc_set_base base;
} __attribute__((packed));

struct volume_server_config_access_map_per_disk_rsp_ext1 {
	struct nvmeib_container disk_rsc_set_ctnr; /* struct wire_disk_rsc_set_data */
} __attribute__((packed));

struct volume_server_config_access_map_per_disk_rsp {
	struct volume_server_config_access_map_per_disk_rsp_base base;
	struct volume_server_config_access_map_per_disk_rsp_ext1 ext1;
} __attribute__((packed));

struct volume_server_config_triplet {
	u8 lnic[16];
	u8 rnic[16];
	__be32 n_qps;
};

#define VOLUME_SERVER_MAX_ARRAY_SIZE 8

struct volume_server_config_access_map_per_disk_n_rsp {
	struct volume_server_config_triplet a[VOLUME_SERVER_MAX_ARRAY_SIZE];
} __attribute__((packed));

struct volume_server_config_access_map_per_disk_d_rsp {
	union nvmeib_wire_gid ports[MAX_HCA_PORTS];
	__be32 n_ports;
	union {
		struct nvmeibs_disk_description a[VOLUME_SERVER_MAX_ARRAY_SIZE];
		struct nvmeibs_disk_seg_lock_info b[VOLUME_SERVER_MAX_ARRAY_SIZE];
	};
} __attribute__((packed));

struct volume_server_config_reset_rsp {
} __attribute__((packed));

struct volume_server_config_locate_rsp {
	/* the rdma message format
	   n located resources (8 bytes)
	   1..n resource id (8 bytes each)
	*/
	u8 ofed_kern_mismatch;
	u8 no_ofed;
	char ofed_ver[NVMEIB_MAX_OFED_VER_STRLEN + 1];
	char kern_ver[NVMEIB_MAX_KERN_VER_STRLEN + 1];
} __attribute__((packed));

#define NVMEIBS_IO_RSP_ERR (0xFADA0000U)
enum volume_server_io_rsp_comp_code { //omril: same for gen-cmd
	/* =0 Success
	   >0 NVMe errors
	   <0 NVMesh error */
	NVMEIBS_IO_RSP_ERR_NO_DISK	= NVMEIBS_IO_RSP_ERR | 0x1,
	NVMEIBS_IO_RSP_ERR_INV_TAG	= NVMEIBS_IO_RSP_ERR | 0x2,
	NVMEIBS_IO_RSP_ERR_INV_OP	= NVMEIBS_IO_RSP_ERR | 0x3,
	NVMEIBS_IO_RSP_ERR_SUBMIT	= NVMEIBS_IO_RSP_ERR | 0x4,
	NVMEIBS_IO_RSP_ERR_MAP_SG	= NVMEIBS_IO_RSP_ERR | 0x5,
	NVMEIBS_IO_RSP_ERR_RDMA		= NVMEIBS_IO_RSP_ERR | 0x6,
	NVMEIBS_IO_RSP_ERR_MD_TRIM	= NVMEIBS_IO_RSP_ERR | 0x7,
	NVMEIBS_IO_RSP_ERR_JMD_PB	= NVMEIBS_IO_RSP_ERR | 0x8,
	NVMEIBS_IO_RSP_ERR_TXID_TRIM= NVMEIBS_IO_RSP_ERR | 0x9,
	NVMEIBS_IO_RSP_ERR_UUID_JOUR= NVMEIBS_IO_RSP_ERR | 0xa,
	NVMEIBS_IO_RSP_ERR_GET_EC_DB= NVMEIBS_IO_RSP_ERR | 0xb,
	NVMEIBS_IO_RSP_ERR_SERJIO	= NVMEIBS_IO_RSP_ERR | 0xc,
	NVMEIBS_IO_RSP_ERR_TOMA_EVT	= NVMEIBS_IO_RSP_ERR | 0xd,
	NVMEIBS_IO_RSP_ERR_INV_VER_TAG = NVMEIBS_IO_RSP_ERR | 0xe,
	NVMEIBS_IO_RSP_ERR_INV_JRNG	= NVMEIBS_IO_RSP_ERR | 0xf,
	NVMEIBS_IO_RSP_ERR_INV_JENT	= NVMEIBS_IO_RSP_ERR | 0x10,
	NVMEIBS_IO_RSP_ERR_INSUF_MD	= NVMEIBS_IO_RSP_ERR | 0x11,
	NVMEIBS_IO_RSP_ERR_UUID_SEG = NVMEIBS_IO_RSP_ERR | 0x12,
	NVMEIBS_IO_RSP_ERR_PB_LOCK = NVMEIBS_IO_RSP_ERR | 0x13,

	NVMEIBS_IO_RSP_ERR_INV_JRNG_GENID = NVMEIBS_IO_RSP_ERR | 0x14,
	NVMEIBS_IO_RSP_ERR_INV_JENT_GENID = NVMEIBS_IO_RSP_ERR | 0x15,
	NVMEIBS_IO_RSP_ERR_INV_JENT_SWLBA = NVMEIBS_IO_RSP_ERR | 0x16,
	NVMEIBS_IO_RSP_ERR_INV_JENT_STATE = NVMEIBS_IO_RSP_ERR | 0x17,
	NVMEIBS_IO_RSP_ERR_INV_JRNG_BINJE = NVMEIBS_IO_RSP_ERR | 0x20,

	NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED = NVMEIBS_IO_RSP_ERR | 0x18,
	NVMEIBS_IO_RSP_ERR_ASYNC_OP_TIMEDOUT = NVMEIBS_IO_RSP_ERR | 0x19,

	NVMEIBS_IO_RSP_ERR_NO_MEM_VADDR = NVMEIBS_IO_RSP_ERR | 0x20,

	/* do not abuse */
	NVMEIBS_IO_RSP_ERR_GENERIC = NVMEIBS_IO_RSP_ERR | 0xff,
	/* Special value, indication for the server that reply will arrive asynchronously */
	NVMEIBS_IO_RSP_EXPECT_ASYNC_REPLY = NVMEIBS_IO_RSP_ERR | 0x1001,
};

static inline const char *io_rsp_comp_code_to_str(
	enum volume_server_io_rsp_comp_code c)
{
	switch (c) {
	case NVMEIBS_IO_RSP_ERR_NO_DISK	: return "NO_DISK   ";
	case NVMEIBS_IO_RSP_ERR_INV_TAG	: return "INV_TAG   ";
	case NVMEIBS_IO_RSP_ERR_INV_OP 	: return "INV_OP    ";
	case NVMEIBS_IO_RSP_ERR_SUBMIT 	: return "SUBMIT    ";
	case NVMEIBS_IO_RSP_ERR_MAP_SG	: return "MAP_SG    ";
	case NVMEIBS_IO_RSP_ERR_RDMA   	: return "RDMA      ";
	case NVMEIBS_IO_RSP_ERR_MD_TRIM	: return "MD_TRIM   ";
	case NVMEIBS_IO_RSP_ERR_JMD_PB	: return "JMD_PB    ";
	case NVMEIBS_IO_RSP_ERR_TXID_TRIM: return  "TXID_TRIM";
	case NVMEIBS_IO_RSP_ERR_UUID_JOUR: return "UUID_JOUR";
	case NVMEIBS_IO_RSP_ERR_GET_EC_DB: return "GET_EC_DB";
	case NVMEIBS_IO_RSP_ERR_SERJIO	: return "SERJIO";
	case NVMEIBS_IO_RSP_ERR_TOMA_EVT: return "TOMA_EVT";
	case NVMEIBS_IO_RSP_ERR_INV_VER_TAG: return "INVALID_VERSION";
	case NVMEIBS_IO_RSP_ERR_INV_JRNG: return "INV_JRNG";
	case NVMEIBS_IO_RSP_ERR_INV_JENT: return "INV_JENT";
	case NVMEIBS_IO_RSP_ERR_INSUF_MD: return "INSUF_MD";
	case NVMEIBS_IO_RSP_ERR_UUID_SEG: return "UUID_SEG";
	case NVMEIBS_IO_RSP_ERR_PB_LOCK: return "PB_LOCK";
	case NVMEIBS_IO_RSP_ERR_INV_JRNG_GENID: return "INV_JRNG_GENID";
	case NVMEIBS_IO_RSP_ERR_INV_JENT_GENID: return "INV_JENT_GENID";
	case NVMEIBS_IO_RSP_ERR_INV_JENT_SWLBA: return "INV_JENT_SWLBA";
	case NVMEIBS_IO_RSP_ERR_INV_JENT_STATE: return "INV_JENT_STATE";
	case NVMEIBS_IO_RSP_ERR_GENERIC: return "GENERIC";
	default: return "???";
	}
}

struct volume_server_lock_rsp_base {
	__be64 cmp_swap_val;
	__be32 comp_code;
} __attribute__((packed));

struct volume_server_lock_rsp_ext1 {
	__be32 read_len;
	u8 read_data[NVMEIB_LOCK_DATA_SIZE];
} __attribute__((packed));

struct volume_server_lock_rsp {
	struct volume_server_lock_rsp_base base;
	struct volume_server_lock_rsp_ext1 ext1;
} __attribute__((packed));

struct volume_server_io_rsp_base {
	__be64 piggyback_read;
	__be32 comp_code;
} __attribute__((packed));

struct volume_server_io_rsp {
	struct volume_server_io_rsp_base base;
} __attribute__((packed));

/* 1.3: Response to GET_JRANGE */
struct volume_server_get_jrange_rsp_base {
	__be32	rng_idx;
	__be64	jrnl_lba;
	__be32	rng_nlba;
	__be32	non_free_ents_bmp[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
	union jblock_md jmdc_ents[NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_V1_3];
} __attribute__((packed));

struct volume_server_get_jrange_rsp_ext1 {
	__be32 n_ents;
	__be32 n_free_ents;
	__be64 gen_id;
	struct nvmeib_jrnl_ent_md ents_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
} __attribute__((packed));

struct volume_server_get_jrange_rsp_ext2 {
	__be_binje_t binje_rsp;
} __attribute__((packed));

struct volume_server_get_jrange_rsp_ext3 {
	__be32 rng_nblk;
	__be32 max_rng_blk;
	__be32 tot_n_rng;
} __attribute__((packed));

struct volume_server_get_jrange_rsp {
	struct volume_server_get_jrange_rsp_base base;
	struct volume_server_get_jrange_rsp_ext1 ext1;
	struct volume_server_get_jrange_rsp_ext2 ext2;
	struct volume_server_get_jrange_rsp_ext3 ext3;
};

struct volume_server_gen_rsp_uuid_jour_base {
	uuid_be uuid;
	__be32 rng_id;
	__be64 rng_slba;
	__be32 rng_nlba;
	__be64 rng_gen_id;
	__be32 jmdc_len;
	__be32 ent_md_len;
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
	__be32 dirty_ents_bitmap[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
	__be32 abnd_ents_bitmap[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
} __attribute__((packed));

struct volume_server_gen_rsp_uuid_jour_ext1 {
	__be32 binje;
} __attribute__((packed));

struct volume_server_gen_rsp_uuid_jour_ext2 {
	__be32 rng_blk; /* Number of journal blocks in range (binjr?) */
	__be32 n_ents; /* Number of entries */
} __attribute__((packed));

struct volume_server_gen_rsp_uuid_jour {
	struct volume_server_gen_rsp_uuid_jour_base base;
	struct volume_server_gen_rsp_uuid_jour_ext1 ext1;
	struct volume_server_gen_rsp_uuid_jour_ext2 ext2;
} __attribute__((packed));

struct volume_server_gen_rsp_get_dirty_bits_base {
	__be32 nlbas;
} __attribute__((packed));

struct volume_server_gen_rsp_blkset_recovered_base {
	__be32 status;
	uuid_be uuid;
	__be32 rng_id;
	__be32 ent_id;
};

struct volume_server_gen_rsp {
	__be32 comp_code;
	union {
		u8 payload[0];
		struct volume_server_gen_rsp_uuid_jour uuid_jour_rsp;
		struct volume_server_gen_rsp_blkset_recovered_base blkset_recovered_rsp;
		struct volume_server_gen_rsp_get_dirty_bits_base get_db_rsp;
	};
} __attribute__((packed));

/*
 * Gregory - type members up to the union are not extendable
 */
struct volume_server_rsp {
	struct nvmeib_hdr hdr;
	u8 opcode;
	__be16 version_tag;
	u8 dummy[4];
	u8 payload[0];
	union {
		/* admin-ch */
		struct {
			union {
				struct volume_server_config_io_info_rsp io_info_rsp;
				struct volume_server_config_access_map_gen_rsp access_g_rsp;
				struct volume_server_config_access_map_per_disk_rsp per_disk_rsp;
				struct volume_server_config_access_map_per_disk_n_rsp per_disk_n_rsp;
				struct volume_server_config_access_map_per_disk_d_rsp per_disk_d_rsp;
				struct volume_server_config_alloc_net_rsp a_net_rsp;
				struct volume_server_config_alloc_nr_net_rsp a_nr_net_rsp;
				struct volume_server_config_locate_rsp locate_rsp;
				struct volume_server_config_reset_rsp reset_rsp;
				struct volume_server_config_per_disk_locks_rsp per_disk_lock_rsp;
				struct volume_server_config_per_segment_lock_info per_segment_lock_info;
				struct volume_server_get_jmdc_rsp get_jmdc_rsp;
			} __attribute__((packed));
			/* Take offset to get maximum size for admin channel */
			u8 ach_payload_max[0];
		} __attribute__((packed));
		/* nordda-ch */
		struct {
			union {
				struct volume_server_lock_rsp lock_rsp;
				struct volume_server_io_rsp io_rsp;
				struct volume_server_gen_rsp gen_rsp;
			} __attribute__((packed));
			/* Take offset to get maximum size for nordda channel */
			u8 nrch_payload_max[0];
		} __attribute__((packed));
	} __attribute__((packed));
} __attribute__((packed));

enum {
	/* this is the size of a management message send by server */
	NVMEIBS_MAX_ADMIN_RSP_SIZE = offsetof(struct volume_server_rsp, ach_payload_max),

	NVMEIBS_MAX_ADMIN_MSG_SIZE =
		S_MSGS_MAX(sizeof(struct volume_server_req), NVMEIBS_MAX_ADMIN_RSP_SIZE),

	NVMEIBS_NORDDA_SERVER_MSG_SIZE = offsetof(struct volume_server_rsp, nrch_payload_max),
};

#endif

