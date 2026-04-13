/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MSGS_H
#define NVMEIBC_MSGS_H

#include "nvmeib_consts_shared.h"
#include "nvmeib_msgs_shared.h"
#include "nvmeib_version_shared.h"

#define C_MSGS_MAX(a,b) ((a) > (b) ? (a) : (b))

// Helps detect between NVMesh v1.2.X and newer versions which support EC
#define JOURNAL_INFO_MARKER 0xdadbdcdd

/* client login */
enum nvmeibc_login_ops {
	NVMEIBC_ADMIN_CHANNEL 		= 0x01,
	NVMEIBC_IO_CHANNEL 			= 0x02,
	NVMEIBC_LOCK_CHANNEL 		= 0x03,
	NVMEIBC_LOCAL_LOCK_CHANNEL 	= 0x04,
	NVMEIBC_NORDDA_CHANNEL 		= 0x05,
	NVMEIBC_SECONDARY_LOCK_CH	= 0x06,
};

static inline const char *nvmeibc_login_ops_to_str(enum nvmeibc_login_ops op)
{
	switch (op) {
	case NVMEIBC_ADMIN_CHANNEL     : return "ADMIN";
	case NVMEIBC_IO_CHANNEL        : return "IO";
	case NVMEIBC_LOCK_CHANNEL      : return "LOCK";
	case NVMEIBC_LOCAL_LOCK_CHANNEL: return "LOCAL-LOCK";
	case NVMEIBC_NORDDA_CHANNEL    : return "NORDDA";
	case NVMEIBC_SECONDARY_LOCK_CH : return "2ND-LOCK";
	default: return "???";
	}
}

struct nvmeibc_login_new_client {
	__be64 c_version;
	__be32 max_iu_len;
	uuid_be client_uuid;
} __attribute__((packed));

struct nvmeibc_login_new_lock {
} __attribute__((packed));

struct nvmeibc_io_channel_def {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u8 sgid[16];
	u8 dgid[16];
	__be16 qp_num;
} __attribute__((packed));

struct nvmeibc_io_channel_login_def {
	__be64 s_subnet_id;
	__be64 s_interface_id;
	__be16 qp_num;
} __attribute__((packed));

enum nvmeibc_login_io_ch_flags {
	NVMEIBC_LOGIN_IOCH_USE_IO_KA = 0x1,
};

struct nvmeibc_login_new_io_channel {
	struct nvmeibc_io_channel_login_def def;
	u8 flags;
} __attribute__((packed));

struct nvmeibc_login_new_io_channel_v1 {
	__be64 s_subnet_id;
	__be64 s_interface_id;
	__be64 d_subnet_id;
	__be64 d_interface_id;
	__be16 qp_num;
	u8 flags;
	u8 pad;
} __attribute__((packed));

enum nvmeibc_login_msg_hdr_flags {
	NVMEIBC_LOGIN_MSG_HDR_NOT_SUPPORTED = 0,
	NVMEIBC_LOGIN_MSG_HDR_SUPPORTED = 1,
	NVMEIBC_LOGIN_MSG_HDR_REQUIRED = 2,
};

struct nvmeibc_login_request {
	struct nvmeib_login_wire_op_cid op_cid;
	union {
		struct {
			__be64 d_subnet_id;
			__be64 d_interface_id;
			union {
				struct nvmeibc_login_new_client creq;
				struct nvmeibc_login_new_lock lreq;
				struct nvmeibc_login_new_io_channel dreq;
			} __attribute__((packed));
			u8 local;
		} __attribute__((packed)) v0;
		struct {
			struct nvmeib_msg_hdr_login_data msg_hdr;
			union {
				struct nvmeibc_login_new_client creq;
				struct nvmeibc_login_new_lock lreq;
				struct nvmeibc_login_new_io_channel_v1 dreq;
			} __attribute__((packed));
		} __attribute__((packed)) v1;
	};
} __attribute__((packed));

static const union nvmeib_version nvmeibc_login_req_v1_version = NVMEIB_2p8_VERSION_INIT;

static inline void nvmeibc_login_req_init(struct nvmeibc_login_request *req,
					  union nvmeib_version tgt_link_ver,
					  u8 opcode, u32 cid, bool local, __be64 d_subnet_id, __be64 d_interface_id)
{
	memset(req, 0, sizeof(*req));
	if (link_version_match(tgt_link_ver, nvmeibc_login_req_v1_version)) {
		nvmeib_wire_op_cid_set_req(&req->op_cid, opcode, cid, 
				       1 /* version */, local);
	} else {
		nvmeib_wire_op_cid_set_req(&req->op_cid, opcode, cid, 
				       0, 0);
		req->v0.local = local;
		req->v0.d_subnet_id = d_subnet_id;
		req->v0.d_interface_id = d_interface_id;
	}
}

static inline bool nvmeibc_login_req_get_local(struct nvmeibc_login_request *req)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		return !!req->v0.local;
	} else {
		return nvmeib_wire_op_cid_get_req_local(&req->op_cid);
	}
}

static inline bool nvmeibc_login_req_get_dgid(struct nvmeibc_login_request *req,
					      __be64 *dgid_subnet_id,
					      __be64 *dgid_interface_id)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		*dgid_subnet_id = req->v0.d_subnet_id;
		*dgid_interface_id = req->v0.d_interface_id;
		return true;
	}
	return false;
}

static inline u8 nvmeibc_login_req_get_version(struct nvmeibc_login_request *req)
{
	return nvmeib_wire_op_cid_get_req_version(&req->op_cid);
}

static inline void nvmeibc_login_req_set_local(struct nvmeibc_login_request *req,
					       bool local)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		req->v0.local = (u8)local;
	} else {
		nvmeib_wire_op_cid_set_req_local(&req->op_cid, (u8)local);
	}
}

static inline void nvmeibc_login_req_get_msg_hdr(struct nvmeibc_login_request *req,
						u8 *out_flags,
						u8 *in_flags,
						__be32 *index,
						__be64 *sid)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		if (out_flags)
			*out_flags = 0;
		if (in_flags)
			*in_flags = 0;
		if (index)
			*index = 0;
		if (sid)
			*sid = 0;
	} else {
		if (out_flags)
			*out_flags = req->v1.msg_hdr.out_flags;
		if (in_flags)
			*in_flags = req->v1.msg_hdr.in_flags;
		if (index)
			*index = req->v1.msg_hdr.index;
		if (sid)
			*sid = req->v1.msg_hdr.sid;
	}
}

static inline void nvmeibc_login_req_set_msg_hdr(struct nvmeibc_login_request *req,
						 u8 out_flags,
						 u8 in_flags,
						 __be32 index,
						 __be64 sid)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 1) {
		req->v1.msg_hdr.out_flags = out_flags;
		req->v1.msg_hdr.in_flags = in_flags;
		req->v1.msg_hdr.index = index;
		req->v1.msg_hdr.sid = sid;
	}
}

static inline void nvmeibc_login_req_set_ach(struct nvmeibc_login_request *req,
					     union nvmeib_version client_version,
					     u32 max_iu_len,
					     const uuid_be *client_uuid)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		req->v0.creq.c_version = cpu_to_be64(client_version.all);
		req->v0.creq.max_iu_len = cpu_to_be32(max_iu_len);
		req->v0.creq.client_uuid = *client_uuid;
	} else {
		req->v1.creq.c_version = cpu_to_be64(client_version.all);
		req->v1.creq.max_iu_len = cpu_to_be32(max_iu_len);
		req->v1.creq.client_uuid = *client_uuid;
	}
}

static inline void nvmeibc_login_req_get_ach(struct nvmeibc_login_request *req,
					     u64 *c_version,
					     u32 *max_iu_len,
					     uuid_be *client_uuid)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		if (c_version)
			*c_version = be64_to_cpu(req->v0.creq.c_version);
		if (max_iu_len)
			*max_iu_len = be32_to_cpu(req->v0.creq.max_iu_len);
		if (client_uuid)
			*client_uuid = req->v0.creq.client_uuid;
	} else {
		if (c_version)
			*c_version = be64_to_cpu(req->v1.creq.c_version);
		if (max_iu_len)
			*max_iu_len = be32_to_cpu(req->v1.creq.max_iu_len);
		if (client_uuid)
			*client_uuid = req->v1.creq.client_uuid;
	}
}

static inline void nvmeibc_login_req_set_ioch(struct nvmeibc_login_request *req,
					      __be64 sgid_subnet_id,
					      __be64 sgid_interface_id,
					      __be64 dgid_subnet_id,
					      __be64 dgid_interface_id,
					 u16 qp_num,
					 u8 flags)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		req->v0.dreq.def.s_subnet_id = sgid_subnet_id;
		req->v0.dreq.def.s_interface_id = sgid_interface_id;
		req->v0.d_subnet_id = dgid_subnet_id;
		req->v0.d_interface_id = dgid_interface_id;
		req->v0.dreq.def.qp_num = cpu_to_be16(qp_num);
		req->v0.dreq.flags = flags;
	} else {
		req->v1.dreq.s_subnet_id = sgid_subnet_id;
		req->v1.dreq.s_interface_id = sgid_interface_id;
		req->v1.dreq.d_subnet_id = dgid_subnet_id;
		req->v1.dreq.d_interface_id = dgid_interface_id;
		req->v1.dreq.qp_num = cpu_to_be16(qp_num);
		req->v1.dreq.flags = flags;
	}
}

static inline void nvmeibc_login_req_get_ioch(const struct nvmeibc_login_request *req,
					      __be64 *sgid_subnet_id,
					      __be64 *sgid_interface_id,
					      __be64 *dgid_subnet_id,
					      __be64 *dgid_interface_id,
					      u16 *qp_num, u8 *flags)
{
	if (nvmeib_wire_op_cid_get_req_version(&req->op_cid) == 0) {
		if (sgid_subnet_id)
			*sgid_subnet_id = req->v0.dreq.def.s_subnet_id;
		if (sgid_interface_id)
			*sgid_interface_id = req->v0.dreq.def.s_interface_id;
		if (dgid_subnet_id)
			*dgid_subnet_id = req->v0.d_subnet_id;
		if (dgid_interface_id)
			*dgid_interface_id = req->v0.d_interface_id;
		if (qp_num)
			*qp_num = be16_to_cpu(req->v0.dreq.def.qp_num);
		if (flags)
			*flags = req->v0.dreq.flags;
	} else {
		if (sgid_subnet_id)
			*sgid_subnet_id = req->v1.dreq.s_subnet_id;
		if (sgid_interface_id)
			*sgid_interface_id = req->v1.dreq.s_interface_id;
		if (dgid_subnet_id)
			*dgid_subnet_id = req->v1.dreq.d_subnet_id;
		if (dgid_interface_id)
			*dgid_interface_id = req->v1.dreq.d_interface_id;
		if (qp_num)
			*qp_num = be16_to_cpu(req->v1.dreq.qp_num);
		if (flags)
			*flags = req->v1.dreq.flags;
	}
}

/* client request to server */
enum nvmeibc_config_ops {
	NVMEIBC_MA_SHARE_CONFIG  = 0x00,
	NVMEIBC_MA_GET_IO        = 0x01,
	NVMEIBC_MA_GET_ACCESS    = 0x02,
	NVMEIBC_MA_ALLOC_IO_NET  = 0x03,
	NVMEIBC_MA_LOCATE_RSC    = 0x04,
	NVMEIBC_MA_RESET_RSC     = 0x05,
	//NVMEIBC_MA_DETACH_DISK   = 0x06, //dont shift-upwards, consider versioning
	NVMEIBC_MA_GET_DISK_MEMS = 0x07,
	NVMEIBC_MA_ALLOC_NR_NET  = 0x08,
	NVMEIBC_MA_GET_LOCK_GIDS = 0x09,
	NVMEIBC_MA_GET_JRANGE		= 0x10,

	/*
	 * CDV_extent allocation / deallocation — sent by a client TPV.allocator
	 * to the CDV.allocator TOMA via the per-disk ADMIN channel (§2.8).
	 */
	NVMEIBC_MA_CDV_ALLOC_EXTENT	= 0x20,
	NVMEIBC_MA_CDV_FREE_EXTENT	= 0x21,
	NVMEIBC_MA_CDV_LIST_EXTENTS	= 0x22,	/* recovery: list all extents owned by a TPV */
};

/* ── CDV_extent allocation protocol (§2.8) ──────────────────────────────────
 *
 * Client TPV.allocator → CDV.allocator TOMA via ADMIN channel.
 *
 * These are application-level C structs passed between the TPV allocator and
 * the IB admin channel layer.  The IB admin channel layer is responsible for
 * encoding them into the actual wire format (VEX / volume_client_req payload).
 *
 * UUIDs are NUL-terminated ASCII strings of length NVMEIBC_BD_UUID_LEN,
 * consistent with the rest of the NVMesh codebase.
 */

/* NVMEIBC_MA_CDV_ALLOC_EXTENT request */
struct nvmeibc_cdv_alloc_req {
	char tpv_uuid[NVMEIBC_BD_UUID_LEN];	/* owning TPV UUID (ASCII string) */
	char cdv_uuid[NVMEIBC_BD_UUID_LEN];	/* parent CDV UUID (ASCII string) */
	u64  req_id;				/* monotonically increasing per-TPV; for idempotency */
	u64  client_generation;			/* allocator_generation client believes is current */
};

/* NVMEIBC_MA_CDV_ALLOC_EXTENT response status codes */
enum nvmeibc_cdv_alloc_status {
	NVMEIBC_CDV_ALLOC_OK		= 0,	/* extent_index is valid */
	NVMEIBC_CDV_ALLOC_CDV_FULL	= 1,	/* no free extents; CDVCapacityWarning sent to mgmt */
	NVMEIBC_CDV_ALLOC_WRONG_GEN	= 2,	/* client_generation stale; re-fetch CDV topology */
	NVMEIBC_CDV_ALLOC_ERROR		= 3,	/* generic TOMA-side error */
};

/* NVMEIBC_MA_CDV_ALLOC_EXTENT response */
struct nvmeibc_cdv_alloc_resp {
	u64  req_id;			/* echoes request req_id */
	u64  extent_index;		/* data CDV_extent index i; 0 on failure */
	u64  allocator_generation;	/* current allocator generation on TOMA side */
	u8   status;			/* enum nvmeibc_cdv_alloc_status */
};

/* NVMEIBC_MA_CDV_FREE_EXTENT request (no dedicated response; fire-and-forget) */
struct nvmeibc_cdv_free_req {
	char tpv_uuid[NVMEIBC_BD_UUID_LEN];	/* owning TPV UUID */
	char cdv_uuid[NVMEIBC_BD_UUID_LEN];	/* parent CDV UUID */
	u64  extent_index;			/* data CDV_extent index to return to the pool */
};

/* NVMEIBC_MA_CDV_LIST_EXTENTS request — sent by the recovery path */
struct nvmeibc_cdv_list_req {
	char tpv_uuid[NVMEIBC_BD_UUID_LEN];	/* whose extents to list */
	char cdv_uuid[NVMEIBC_BD_UUID_LEN];	/* which CDV to query */
};

/*
 * NVMEIBC_MA_CDV_LIST_EXTENTS response — variable-length.
 *
 * Followed immediately by n_extents × u64 extent indices.
 * The response is reassembled in the client's admin-channel receive path
 * (nvmeibc_ib_admin_channel.c §2.8) and returned as a vmalloc'd array.
 */
struct nvmeibc_cdv_list_resp {
	u64  n_extents;		/* number of u64 extent indices that follow */
	u8   status;		/* 0 = OK, non-zero = error */
};

/* ── end CDV_extent allocation protocol ──────────────────────────────────── */

enum {
	NVMEIBC_CFG_SHARE_MAX_CONST = 32,
};

struct volume_client_config_share_const_elem {
	char const_name[64];
	u64 const_val;
} __attribute__((packed));

struct volume_client_config_share_base {
	__be64 version;
	struct nvmeib_container share_const_ctnr;
	struct volume_client_config_share_const_elem share_consts[];
} __attribute__((packed));

struct volume_client_config_share {
	struct volume_client_config_share_base base;
} __attribute__((packed));

#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#endif
struct volume_client_config_share_max_DONT_USE {
	struct volume_client_config_share_base base;
	struct volume_client_config_share_const_elem share_consts[NVMEIBC_CFG_SHARE_MAX_CONST];
} __attribute__((packed));
#ifdef __clang__
#	pragma clang diagnostic pop
#endif

struct volume_client_config_rdma_info {
	__be64 msg_raddr;
	__be32 msg_size;
	__be32 msg_rkey;
};

struct volume_client_config_ma_get_io_base {
	char client_name[NVMEIB_BASE_CLIENT_NAME_SIZE];
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct volume_client_config_rdma_info rdma;
	__be64 keep_alive_raddr;
	__be32 keep_alive_size;
	__be32 keep_alive_rkey;
} __attribute__((packed));

struct volume_client_config_ma_get_io {
	struct volume_client_config_ma_get_io_base base;
} __attribute__((packed));

struct volume_client_config_get_disk_locks {
	char client_name[NVMEIB_BASE_CLIENT_NAME_SIZE];
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
} __attribute__((packed));


struct volume_client_config_get_lock_gids {
	char client_name[NVMEIB_BASE_CLIENT_NAME_SIZE];
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct volume_client_config_rdma_info rdma;
} __attribute__((packed));

struct volume_client_config_jrange_cache {
	__be32 rng_id;
	__be64 rng_genid;
	__be32 free_bitmap[DIV_ROUND_UP(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, 32)];
	struct nvmeib_jrnl_ent_md ent_md[NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE];
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
}  __attribute__((packed));

struct volume_client_config_get_jrange_base {
	char client_name[NVMEIB_BASE_CLIENT_NAME_SIZE];
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct volume_client_config_rdma_info rdma;
	uuid_be client_uuid;
	__be32 prev_rng; /* 0xffffffff for no previous range */
}  __attribute__((packed));

struct volume_client_config_get_jrange_ext1 {
	struct volume_client_config_jrange_cache clnt_jrc;
}  __attribute__((packed));

struct volume_client_config_get_jrange_ext2 {
	__be_binje_t binje_req;
	__be_binje_t binje_jrc;
}  __attribute__((packed));

struct volume_client_config_get_jrange_ext3 {
	struct nvmeib_remote_access_info jmdc_rai;
}  __attribute__((packed));

struct volume_client_config_get_jrange {
	struct volume_client_config_get_jrange_base base;
	struct volume_client_config_get_jrange_ext1 ext1;
	struct volume_client_config_get_jrange_ext2 ext2;
	struct volume_client_config_get_jrange_ext3 ext3;
};

struct volume_client_jmdc_req  {
	char client_name[NVMEIB_BASE_CLIENT_NAME_SIZE];
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	char seg_uuid[NVMEIB_GID_STR_MAX];
	struct volume_client_config_rdma_info rng_data_rdma;
	struct volume_client_config_rdma_info ents_md_rdma;
	struct volume_client_config_rdma_info jmdc_ents_rdma;
	__be16 start_rng;
	__be16 num_rng;
	u8 get_len_only;
	u8 get_dirty_only;
	u8 get_client_uuid_only;
	u8 recov_src; /* enum nvmeib_recov_src */
	u8 dummy[3];
	/* When get_by_client_uuid instead */
	uuid_be client_uuid;
} __attribute__((packed));

/* the volume_client_config_ma_access rdma response format
		r remote admin guid
		for i = 0 to r - 1
			remote main guid (16 bytes)
	    	u remote disks on that admin gid that need to be accessed
	    	for ii = 0 to u -1
	    		disk_name [NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE]
	    		s remote io guid (4 bytes)
	    		for iii = 0 to s -1
	    			remote io guid (16 bytes)
	    			t local io guid (4 bytes)
	    			for iiii = 0 to t - 1
	    				local io guid (16 bytes)
	    			end
	    		end
	    	end
	    end
	*/
struct volume_client_config_ma_access_base {
	struct volume_client_config_rdma_info rdma;
} __attribute__((packed));

struct volume_client_config_ma_access_ext1 {
	__be32 req_nrch_per_path;
} __attribute__((packed));

struct volume_client_config_ma_access {
	struct volume_client_config_ma_access_base base;
	struct volume_client_config_ma_access_ext1 ext1;
} __attribute__((packed));

struct volume_client_config_init_req {
} __attribute__((packed));

struct volume_client_config_alloc_net_req {
	struct nvmeibc_io_channel_def def;
	__be64 cs_gid; //[IOCH-DRAINED TBD]: move this into nvmeibc_io_channel_def?
} __attribute__((packed));

struct volume_client_config_ma_reset_io {
	struct nvmeibc_io_channel_def def;
	__be64 rsc_id;
	__be64 msix_table_addr;
	__be64 msix_raddr;
	__be32 msix_payload;
} __attribute__((packed));

struct volume_client_config_ma_locate {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct volume_client_config_rdma_info rdma;
	bool no_ofed;
	char ofed_ver[NVMEIB_MAX_OFED_VER_STRLEN + 1];
	char kern_ver[NVMEIB_MAX_KERN_VER_STRLEN + 1];
} __attribute__((packed));

struct volume_client_config_ma_detach {
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
} __attribute__((packed));

struct volume_client_config_req_13 {
	u8 opcode;
	struct {
		union {
			u8 payload[0];
			struct volume_client_config_ma_get_io get_io_req;
			struct volume_client_config_ma_access acs_req;
			struct volume_client_config_alloc_net_req a_net_req;
			struct volume_client_config_ma_reset_io reset_io_req;
			struct volume_client_config_ma_locate loc_req;
			struct volume_client_config_ma_detach detach_req;
			struct volume_client_config_get_disk_locks disk_locks;
			struct volume_client_config_get_lock_gids get_lock_gids;
			struct volume_client_config_get_jrange get_jrange;
		};
		/* Take offset to get maximum size */
		u8 payload_max[0];
	};
};

#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#endif
struct volume_client_config_req {
	u8 opcode;
	struct {
		union {
			u8 payload[0];
			/* 1.3 protocol */
			struct volume_client_config_ma_get_io get_io_req;
			struct volume_client_config_ma_access acs_req;
			struct volume_client_config_alloc_net_req a_net_req;
			struct volume_client_config_ma_reset_io reset_io_req;
			struct volume_client_config_ma_locate loc_req;
			struct volume_client_config_ma_detach detach_req;
			struct volume_client_config_get_disk_locks disk_locks;
			struct volume_client_config_get_lock_gids get_lock_gids;
			struct volume_client_config_get_jrange get_jrange;
			/* 2.0 protocol */
			struct volume_client_config_share config_share;
			/* Members used to calculate maximum size */
			struct volume_client_config_share_max_DONT_USE config_share_max_DONT_USE;
		};
		/* Take offset to get maximum size */
		u8 payload_max[0];
	};
} __attribute__((packed));
#ifdef __clang__
#	pragma clang diagnostic pop
#endif

enum nvmeibc_dbg_cmd_ops {
	NVMEIBC_DBG_PLEASE_KILL_YOURSELF,
};

struct volume_client_dbg_req_base {
	__be64 version;
	/* Techincally I should have bumped version for this, but these commands
	   are internal, so I don't see a problem. */
	struct {
		__be32 has_death_wish;
		__be64 rsc_id;
		__be64 dlba;
	} death_wish;
} __attribute__((packed));

struct volume_client_dbg_please_kill_yourself {
	struct volume_client_dbg_req_base base;
} __attribute__((packed));

struct volume_client_dbg_req {
	u8 opcode;
	struct {
		union {
			u8 payload[0];
			/* 2.1 protocol */
			struct volume_client_dbg_please_kill_yourself pky_req;
		};
		/* Take offset to get maximum size */
		u8 payload_max[0];
	};
} __attribute__((packed));

enum __attribute__ ((__packed__)) nvmeibc_indirect_op {
    NVMEIB_IND_OP_UNUSED    		= 0,

    NVMEIB_IND_OP_IO_READ   		= 1,
    NVMEIB_IND_OP_IO_WRITE  		= 2,
    NVMEIB_IND_OP_IO_DSM    		= 3,

    NVMEIB_IND_OP_MD_READ   		= 4,
	NVMEIB_IND_OP_MD_RD_MOD_WR 		= 5, 	// Generic read modify write
	NVMEIB_IND_OP_IO_WRITE_UNCOR	= 6,	// uncorrectable
};

enum nvmeibc_indirect_piggyback_op {
	NVMEIB_IND_PG_NONE = 0,
	NVMEIB_IND_PG_READ_LOCK = 1,
	//NVMEIB_IND_PG_SET_DB = 2, //not removing so not to break VEX
	NVMEIB_IND_PG_WRITE_BLKSET_INFO = 3
};

struct volume_client_io_req_base {
	/* the disk name */
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	/* start LBA on disk, in units of software lbas */
	__be64 sw_slba;
	/* how many data bytes.
	   for trim cmd, X * sizeof(struct nvmeib_dsm_range) */
	__be64 data_len;
	/* piggyback read lock */
	u8 ind_pg_op;
	u8 pad[3];	/* enum nvmeibc_indirect_piggyback_op */
	__be64 lmi;
	__be64 offset;
	u8 dirty_bit_set_OBSOLETE; //not removing so not to break VEX (size wise)
	__be32 blkset_info;
	u8 ind_op; /* enum nvmeibc_indirect_op */
	/* remote access info to the io's md buffer.
	 *
	 * if @md_desc.size != 0 then if @ind_op is:
	 *  READ or MD-READ	: srv read md from disk and rdma-write to md-desc
	 *                    only here, other members are valid.
	 *  WRITE          	: clt rdma-write md-desc and srv write it to disk
	 *  MD-TRIM			: srv reads data and md, write data and zeros to md
	 *
	 *  We send the md size and not only a flag to check client had mapped
	 *  (and send or will recv) the same size the nvme-driver will access.
	 */
	struct nvmeib_md_desc md_desc;

	/* journal MD piggyback
	   Write op with MD, can piggyback a write to jmd-cache[i] where
	   i is the offset in journal blks units from cl's journal range.
	   =0 : No piggyback
	   >0 : Piggyback to jmd-tbl entry #'@jmd_pb - 1' */
	u8 jmd_pb[2];
	u8 dummy[2];

	/* io-ka piggy-back */
	__be64 io_ka_val;

	/* table_count has been moved to nvmeib_indirect_buf.
	 * In the case of nvmeib_indirect_buf the vex_dref contains 
	 * NVMEIB_NRCH_INDIRECT_BUF_MAGIC otherwise
	 * NVMEIB_NRCH_DIRECT_BUF_MAGIC */
	struct vex_dref buf_dref;

 	/*
	 * base_dref points to IO IP maps - direct or indirect
	 */
} __attribute__((packed));

struct volume_client_io_req_ext1 {
	u8 is_sub_block; /* Is this command directed to a sub block */
	u8 sub_block_idx; /* If yes, what index */
} __attribute__((packed));

struct volume_client_io_req_ext2 {
	u8 is_recovery; /* This is IO for recovery */
} __attribute__((packed));

struct volume_client_io_req {
	struct volume_client_io_req_base base;
	struct volume_client_io_req_ext1 ext1;
	struct volume_client_io_req_ext2 ext2;
} __attribute__((packed));

/* Used to calculate the maximum size of an IO Request */
struct volume_client_io_req_max_DONT_USE {
	struct volume_client_io_req_base base;
	/* Padding to align to 8-bytes for DREF */
	u8 padding[ALIGN(sizeof(struct volume_client_io_req_base), sizeof(u64)) - sizeof(struct volume_client_io_req_base)];
	/* Max size is indirect with NVMEIBS_MAX_IO_CHANNEL_MSGS entries */
	struct nvmeib_indirect_buf indirect_hdr;
	struct nvmeib_direct_buf indirect_ents[NVMEIBS_MAX_IO_CHANNEL_MSGS];
	/* ------------------ ALL MEMBERS ABOVE THIS LINE -----------------*/
	/* Use offset of payload_max to calculate maximum size */
	u8 payload_max[0];
} __attribute__((packed));

enum nvmeibc_client_req_ops {
	NVMEIBC_CR_IO = 0x01,
	NVMEIBC_CR_LOCK = 0x02,
};

struct volume_client_gen_req_uuid_jour_base {
	uuid_be client_uuid;
	char sgmnt_uuid[NVMEIB_GID_STR_MAX];
	struct wire_remote_access_info jmdc_rai;
	struct wire_remote_access_info ent_md_rai;
} __attribute__((packed));

struct volume_client_gen_req_uuid_jour_ext1 {
	__be32 binje;
} __attribute__((packed));

struct volume_client_gen_req_uuid_jour {
	struct volume_client_gen_req_uuid_jour_base base;
	struct volume_client_gen_req_uuid_jour_ext1 ext1;
} __attribute__((packed));;

struct volume_client_gen_req_blkset_recovered_base {
	uuid_be uuid;
	char ds_uuid[NVMEIB_GID_STR_MAX];
	__be64 lock_ent;
	/* blkset number in pRAID */
	__be64 blkset_num;
	/* first LBA in the blkset on *this* disk ;
	   used when @rng_id is not valid, to flush
	   any JMDC entry that has j2d in the range
	   [@blkset_slba, @blkset_slba+LOCKSET_4KS) */
	__be64 blkset_slba;
	__be32 rng_id;
	__be32 ent_id;
	u8 pass2toma;
	u8 from_jgc; 	/* From the Journal Garbage Collection (As opposed to HTR) */
	__be64 gen_id; 	/* JGC Generation ID */
} __attribute__((packed));

struct volume_client_gen_req_blkset_recovered {
	struct volume_client_gen_req_blkset_recovered_base base;
};

struct disk_req_get_ec_dirty_bits_base {
	__be64 lba;
	__be32 sectors;
	char get_dbits;		// True -> bitmap of dbits.
	char get_stales;	// True -> bitmap of stale locks. Can combine both
	char get_full_val;	// If true, Get full value otherwise (!!val)
	char reserved;		// Removed in V-1.3.1

	struct wire_remote_access_info rai; //TODO: move this to base struct
} __attribute__((packed));

struct disk_req_get_ec_dirty_bits {
	struct disk_req_get_ec_dirty_bits_base base;
};

struct volume_client_toma_req_base {
	u8 cmd_type;
	__be64 handle;
	struct nvmeib_container data_ctnr;
}__attribute__((packed));

struct volume_client_toma_req_ext1 {
	struct nvmeib_container srv_pb_ctnr;
}__attribute__((packed));

struct volume_client_toma_req {
	struct volume_client_toma_req_base base;
	struct volume_client_toma_req_ext1 ext1;
}__attribute__((packed));

/* Used to calculate the maximum size of a TOMA Request (1.3) */
struct volume_client_toma_req_13_max_DONT_USE {
	struct volume_client_toma_req_base base;
	u8 toma_req_data[NVMEIB_TOMA_REQ_MAX_LEN_NON_EC];
	/* ------------------ ALL MEMBERS ABOVE THIS LINE -----------------*/
	/* Use offset of payload_max to calculate maximum size */
	u8 payload_max[0];
}__attribute__((packed));

/* Used to calculate the maximum size of a TOMA Request */
struct volume_client_toma_req_max_DONT_USE {
	struct volume_client_toma_req_base base;
	u8 toma_req_data[NVMEIB_TOMA_REQ_MAX_LEN];
	struct volume_client_toma_req_ext1 ext1;
	struct nvmeibs_lost_srv_resource_payload pb_payload;
	/* ------------------ ALL MEMBERS ABOVE THIS LINE -----------------*/
	/* Use offset of payload_max to calculate maximum size */
	u8 payload_max[0];
}__attribute__((packed));

struct volume_client_gen_req_free_ents_base {
	char seg_uuid[NVMEIB_GID_STR_MAX];
	u8 src; /* enum nvmeib_recov_src */
	u8 pad[3];
	__be16 num_ents;
	/* Used for HTR */
	u8 pass2toma;
	__be64 blkset_num;
	__be64 blkset_slba;
	__be64 lock_ent;
	/* SERJIO Boot ID */
	char serjio_boot_id[NVMEIB_GID_STR_MAX];
} __attribute__((packed));

struct volume_client_gen_req_free_ents {
	struct volume_client_gen_req_free_ents_base base;
};

struct wire_free_ents_entry_base {
	/* Range Generation ID */
	__be64 rng_gen_id;
	/* journal-range index */
	__be16 rng_idx;
	/* Entry Index */
	__be16 ent_idx;
	/* Entry MD (Generation ID) */
	u8 ent_md_gen_id;
	u8 padding[3];
} __attribute__((packed));

struct wire_free_ents_entry_ext1 {
	/* Range N */
	__be_binje_t binje;
} __attribute__((packed));

struct wire_free_ents_entry {
	struct wire_free_ents_entry_base base;
	struct wire_free_ents_entry_ext1 ext1;
} __attribute__((packed));

struct volume_client_gen_req_jentry_erase_base {
	__be64 rng_gen_id;
	__be32 rng_id;
	__be32 ent_id;
	__be64 sw_jlba;
	struct nvmeib_jrnl_ent_md ent_md;
} __attribute__((packed));

struct volume_client_gen_req_jentry_erase_ext1 {
	__be_binje_t rng_binje;
} __attribute__((packed));

struct volume_client_gen_req_jentry_erase {
	struct volume_client_gen_req_jentry_erase_base base;
	struct volume_client_gen_req_jentry_erase_ext1 ext1;
};

struct volume_client_gen_req {
	u8 op;
	struct {
		union {
			u8 payload[0];
			struct volume_client_gen_req_uuid_jour uuid_jour_req;
			struct volume_client_gen_req_blkset_recovered blkset_recovered_req;
			struct disk_req_get_ec_dirty_bits get_ec_db_req;
			struct volume_client_gen_req_free_ents free_ents_req;
			struct volume_client_gen_req_jentry_erase jentry_erase_req;
		};
		/* Take offset to get maximum size */
		u8 payload_max[0];
	};
} __attribute__((packed));

struct volume_client_lock_req_base {
	u64 lmi;
	__be64	offset;
	struct {
		__be64 compare_add;
		__be64 swap;
		__be64 compare_add_mask;
		__be64 swap_mask;
	} atomic;
	u8 op;
} __attribute__((packed));

struct volume_client_lock_req_ext1 {
	__be64 seg_id;
	struct {
		u8 table_type; /* enum nvmeib_lock_table */
		u8 pad[3];
		__be32 len;
		u8 data[NVMEIB_LOCK_DATA_SIZE];
	} rdma;
} __attribute__((packed));

struct volume_client_lock_req {
	struct volume_client_lock_req_base base;
	struct volume_client_lock_req_ext1 ext1;
} __attribute__((packed));

/*
 * Gregory - type members up to the union are not extendable
 */
#ifdef __clang__
#	pragma clang diagnostic push
#	pragma clang diagnostic ignored "-Wgnu-variable-sized-type-not-at-end"
#endif
struct volume_client_req {
	struct nvmeib_hdr hdr;
	u8 client_op;
	__be16 version_tag;
	u8 dummy[4];
	union {
		u8 payload[0];
		/* admin-ch */
		struct {
			union {
				struct volume_client_dbg_req dbg_req;
				struct volume_client_config_req config_req;
				struct volume_client_toma_req toma_req;
				struct volume_client_jmdc_req jmdc_req;
				/* Members used to calculate maximum size */
				struct volume_client_toma_req_max_DONT_USE toma_req_max_DONT_USE;
			} __attribute__((packed));
			/* Take offset to get maximum size for admin channel */
			u8 ach_payload_max[0];
		} __attribute__((packed));
		struct {
			union {
				struct volume_client_config_req_13 config_13_req;
				struct volume_client_toma_req toma_13_req;
				struct volume_client_jmdc_req jmdc_13_req;
				/* Members used to calculate maximum size */
				struct volume_client_toma_req_13_max_DONT_USE toma_13_req_max_DONT_USE;
			} __attribute__((packed));
			/* Take offset to get maximum size for admin channel (1.3) */
			u8 ach_13_payload_max[0];
		} __attribute__((packed));
		/* nordda-ch */
		struct {
			union {
				struct volume_client_io_req io_req;
				struct volume_client_gen_req gen_req;
				struct volume_client_lock_req lock_req;
				/* Members used to calculate maximum size */
				struct volume_client_io_req_max_DONT_USE io_req_max_DONT_USE;
			} __attribute__((packed));
			/* Take offset to get maximum size for nordda channel */
			u8 nrch_payload_max[0];
		} __attribute__((packed));
	};
} __attribute__((packed));
#ifdef __clang__
#	pragma clang diagnostic pop
#endif

#define NVMEIBC_VOLUME_CLIENT_DBG_REQ_HDR_SIZE() \
	(offsetof(struct volume_client_req, payload) + offsetof(struct volume_client_dbg_req, payload))

#define NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_HDR_SIZE() \
	(offsetof(struct volume_client_req, payload) + offsetof(struct volume_client_config_req, payload))
	
#define NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_PAYLOAD(creq) \
	((void *)creq->config_req.payload)

#define NVMEIBC_VOLUME_CLIENT_DBG_REQ_PAYLOAD(creq) \
	((void *)creq->dbg_req.payload)
	
#define NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_CONST_PAYLOAD(creq) \
	((const void *)creq->config_req.payload)

#define NVMEIBC_VOLUME_CLIENT_DBG_REQ_CONST_PAYLOAD(creq) \
	((const void *)creq->dbg_req.payload)

#define NVMEIBC_VOLUME_CLIENT_13_CONFIG_REQ_SIZE(cfg_req_field) \
	(offsetof(struct volume_client_req, config_13_req.payload_max))

#define NVMEIBC_VOLUME_CLIENT_20_CONFIG_REQ_SIZE(cfg_req_field) \
	(offsetof(struct volume_client_req, config_req.payload_max))

#define NVMEIBC_VOLUME_CLIENT_REQ_HDR_SIZE() \
	(offsetof(struct volume_client_req, payload))

#define NVMEIBC_VOLUME_CLIENT_ADMIN_REQ_SIZE(req_field) \
	(offsetof(struct volume_client_req, payload) + sizeof_field(struct volume_client_req, req_field))

#define NVMEIBC_VOLUME_CLIENT_REQ_PAYLOAD(creq) \
	((void *)creq->payload)

#define NVMEIBC_VOLUME_CLIENT_GEN_REQ_PAYLOAD(creq) \
	((void *)creq->gen_req.payload)

/* client response to server request */
enum {
	NVMEIBC_RSP_OPCODE_BASE		= 0x00,
	NVMEIBC_RSP_OPCODE_OK		= 0xa0,
	NVMEIBC_RSP_OPCODE_ERR		= 0xa1,
	NVMEIBC_RSP_TOMA_OPCODE_OK	= 0xa2,
	NVMEIBC_RSP_TOMA_OPCODE_ERR	= 0xa3,
	NVMEIBC_RSP_LOGOUT_OK		= 0xa4,
	NVMEIBC_RSP_LOGOUT_LAST		= 0xff,
};

struct volume_client_put_rsp {
} __attribute__((packed));

struct volume_client_get_rsp {
	__be64 n;
	__be64 ids[NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT];
} __attribute__((packed));

/*
 * Gregory - type members up to the union are not extendable
 */
struct volume_client_rsp {
	struct nvmeib_hdr hdr;
	u8 opcode;
	__be16 version_tag;
	u8 dummy[4];
	u8 bytes[0];
	union {
		struct volume_client_put_rsp p_rsp;
		struct volume_client_get_rsp g_rsp;
	};
} __attribute__((packed));

enum {
	NVMEIBC_DEFAULT_MAX_INDIRECT_IO = sizeof(struct nvmeib_indirect_buf) +
		NVMEIBS_MAX_IO_CHANNEL_MSGS * sizeof(struct nvmeib_direct_buf),

	NVMEIBC_IO_REQ_SIZE = offsetof(struct volume_client_req, io_req_max_DONT_USE.payload_max),

	NVMEIBC_RSP_SIZE = sizeof(struct volume_client_rsp),

	NVMEIBC_LOGIN_SIZE = sizeof(struct nvmeibc_login_request),

	NVMEIBC_DIRTY_BITS_PAGES = 128,

	NVMEIBC_13_MAX_ADMIN_CLIENT_REQ_SIZE =
		offsetof(struct volume_client_req, ach_13_payload_max),
		
	NVMEIBC_13_MAX_ADMIN_CLIENT_MSG_SIZE =
		C_MSGS_MAX(NVMEIBC_13_MAX_ADMIN_CLIENT_REQ_SIZE, NVMEIBC_RSP_SIZE),

	NVMEIBC_MAX_ADMIN_CLIENT_REQ_SIZE =
		offsetof(struct volume_client_req, ach_payload_max),

	NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE =
		C_MSGS_MAX(NVMEIBC_MAX_ADMIN_CLIENT_REQ_SIZE, NVMEIBC_RSP_SIZE),

	NVMEIBC_NORDDA_CLIENT_IO_REQ_SIZE = NVMEIBC_IO_REQ_SIZE,

	NVMEIBC_NORDDA_CLIENT_IO_REQ_MIN_SIZE =
		offsetof(struct volume_client_req, payload) +
		sizeof(struct volume_client_io_req),

	NVMEIBC_NORDDA_CLIENT_GEN_REQ_SIZE =
		offsetof(struct volume_client_req, gen_req.payload_max),

	NVMEIBC_NORDDA_CLIENT_MSG_SIZE = 
		offsetof(struct volume_client_req, nrch_payload_max),
		
	NVMEIBC_MAX_CLIENT_MSG_SIZE =
		C_MSGS_MAX(NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE, NVMEIBC_NORDDA_CLIENT_MSG_SIZE),
};

struct wire_lock_gid_base {
	union nvmeib_wire_gid gid;
	__be32 atomic_ops;	// obsolete // not anymore - Now used to pass the max number of outstanding atomic ops the device supports
} __attribute__((packed));

struct wire_lock_gid_ext1 {
	u8 link_layer;
	u8 transport_type;
	__be32 priority;
} __attribute__((packed));

struct wire_lock_gid_ext2 {
	__be16 tcp_base_port;
	__be16 tcp_num_ports;
} __attribute__((packed));

struct wire_lock_gid {
	struct wire_lock_gid_base base;
	struct wire_lock_gid_ext1 ext1;
	struct wire_lock_gid_ext2 ext2;
} __attribute__((packed));

struct wire_get_io_disks_info_base {
	char id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	__be32 prefered;
	__be32 numa_dist; // (0 - numas are not connected, 10 - same numa (chip), 20, ...)
	__be32 exising_conn_num; //number of connections the srv already has on the port
} __attribute__((packed));

struct wire_get_io_disks_info_ext1 {
	__be32 nrch_ioreq_num; // max num NR channel IO requests
} __attribute__((packed));

struct wire_get_io_disks_info {
	struct wire_get_io_disks_info_base base;
	struct wire_get_io_disks_info_ext1 ext1;
} __attribute__((packed));

struct wire_get_io_ports_info_base {
	char	hw_gid[16];
	u8	may_access;
	char	ib_gid[16];
	__be32	pkey;
	__be32	hw_type;
	__be32	layer;
	__be32	n_msgs;
	__be32	nr_prefered;
} __attribute__((packed));

struct wire_get_io_ports_info_ext1 {
	u8 transport_type; /* rdma_transport_type */
	u8 trans_prio;
	u8 numa_dist;
	u8 bw_prio;
	u8 lat_prio;
} __attribute__((packed));

struct wire_get_io_ports_info_ext2 {
	__be64 node_guid; /* Uniquely identifies the NIC */
} __attribute__((packed));

struct wire_get_io_ports_info_ext3 {
	__be16 tcp_base_port;
	__be16 tcp_num_ports;
} __attribute__((packed));

struct wire_get_io_ports_info {
	struct wire_get_io_ports_info_base base;
	struct wire_get_io_ports_info_ext1 ext1;
	struct wire_get_io_ports_info_ext2 ext2;
	struct wire_get_io_ports_info_ext3 ext3;
} __attribute__((packed));

/* access map lnics & rnics names are relative to server */

struct wire_acs_map_clnt_ionics_base {
	char	gid[16];
	u8	may_access;
} __attribute__((packed));

struct wire_acs_map_clnt_ionics {
	struct wire_acs_map_clnt_ionics_base base;
};

struct wire_acs_map_srv_ionics_base {
	char gid[16];
	struct nvmeib_container clnt_ionics_map;
} __attribute__((packed));

struct wire_acs_map_srv_ionics {
	struct wire_acs_map_srv_ionics_base base;
};

struct wire_acs_map_disks_base {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	struct nvmeib_container srv_ionics_map;
} __attribute__((packed));

struct wire_acs_map_disks {
	struct wire_acs_map_disks_base base;
};

struct wire_acs_map_arnic_base {
	char	gid[16];
	__be32	section_len;
	struct nvmeib_container disks_map;
} __attribute__((packed));

struct wire_acs_map_arnic {
	struct wire_acs_map_arnic_base base;
};



#endif

