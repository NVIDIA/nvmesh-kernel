/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_MSGS_H
#define NVMEIB_MSGS_H

/***************************************************************************
 * Protocol data structures used by both client and server messages
 **************************************************************************/

struct nvmeib_container_elem {
	union {
		__be64 magic;
		char magic_str[8];
	};
	u8 pad[2];
	__be16 idx;
	__be32 size;
	u8 payload[0];
} __attribute__((packed));

struct nvmeib_container {
	union {
		__be64 magic;
		char magic_str[8];
	};
	union {
		__be64 contents_magic;
		char contents_magic_str[8];
	};
	__be16 version_tag;
	__be16 n_elem;
	__be32 elem_stride;
	__be32 size;
	union {
		struct nvmeib_container_elem elements[0];
		u8 payload[0];
	} __attribute__((packed));
} __attribute__((packed));

struct nvmeib_direct_buf {
	u64 va;
	u32 key;
	u32 len;
	/* other direction key */
	u32 okey;
};

struct wire_remote_access_info {
	__be64 raddr;
	__be32 len;
	__be32 rkey;
};

struct nvmeib_local_access_info {
	u64 laddr;
	u32 len;
	u32 lkey;
};

struct nvmeib_indirect_buf {
	struct nvmeib_direct_buf table_desc;
	u16 table_count;
	u32 len;
	struct nvmeib_direct_buf desc_list[0];
} __attribute__((packed));

union nvmeib_msg_hdr_svc_id {
	uint8_t raw[16];
	struct {
		__be64 sid;
		__be32 index;
		__be32 dct;
	} global;
};

struct nvmeib_msg_hdr {
	__be32 version;
	__be32 flags;
	union nvmeib_msg_hdr_svc_id src;
	union nvmeib_msg_hdr_svc_id dst;
};

struct nvmeib_msg_hdr_login_data {
	u8 out_flags;
	u8 in_flags;
	u8 pad[2];
	__be32 index;
	__be64 sid;
} __attribute__((packed));

struct nvmeib_hdr {
	u64 tag;
	u8 opcode;
} __attribute__((packed));

/* client server type of messages
   (nvmeib_hdr.opcode) */
enum e_NVMEIB_CMD{
	NVMEIB_LOGIN_REQ 		= 0x00,
	NVMEIB_CONFIG	  		= 0x01,
	NVMEIB_CMD		  		= 0x03,	// IO cmd R/W/Trim (Blocks + optional metadata)
	NVMEIB_GEN_CMD		  	= 0x04,
	NVMEIB_DBG_CMD		  	= 0x05,
	NVMEIB_C_LOGOUT	  		= 0x10,
	NVMEIB_LOGIN_RSP  		= 0xc0,
	NVMEIB_RSP		  		= 0xc1,
	NVMEIB_LOGIN_REJ  		= 0xc2,
	NVMEIB_SIW_ACK_TGT_SEND = 0xc3,
	NVMEIB_H_LOGOUT	  		= 0x80,
	NVMEIB_TOMA_REQ	  		= 0xa0, /* toma rsps are NVMEIB_RSP type */
	NVMEIB_KEEP_ALIVE 		= 0xb0,
	NVMEIB_GET_JMDC  		= 0xe0, /* get journal md cache */
};

static inline const char *msg_op_to_str(int op)
{
	switch (op) {
	case NVMEIB_LOGIN_REQ: return "LOGIN";
	case NVMEIB_CONFIG: return "CONFIG";
	case NVMEIB_CMD: return "CMD_IO_RWT";
	case NVMEIB_C_LOGOUT: return "CLIENT LOGOUT";
	case NVMEIB_LOGIN_RSP: return "LOGIN RSP";
	case NVMEIB_RSP: return "RSP";
	case NVMEIB_LOGIN_REJ: return "LOGIN REJ";
	case NVMEIB_H_LOGOUT: return "CONTROLLER LOGOUT";
	case NVMEIB_TOMA_REQ: return "TOMA REQ";

	default: return "???";
	}
}

/* Access info to remote metadata buffer
 * Server: md buffer per iocmd - sent once when connecting channel
 * Client: md buffer of block-io-cmd - sent per IO */
struct nvmeib_md_desc {
	__be64 raddr;
	__be32 size;
	__be32 rkey;
	__be32 sw_sector_shift;
	u8 pad[4];
	/* Same read-modify-write action to apply
	 *  on metadata of every LBA on the cmd */
	__be64 rmw_action;
} __attribute__((packed));

union nvmeib_wire_gid {
	u8	raw[16];
	struct {
		__be64	subnet_prefix;
		__be64	interface_id;
	} global;
} __attribute__((packed));

struct nvmeib_login_wire_op_cid {
	union {
		struct {
#ifdef __LITTLE_ENDIAN
			u8 opcode:4;
			u8 local:1;
			u8 version:3;
#else
			/* BIG_ENDIAN */
			u8 version:3;
			u8 local:1;
			u8 opcode:4;
#endif
		}  __attribute__((packed)) req_opcode;
		u8 rsp_opcode;
	};
	u8 cid_lo;
	u8 cid_mid;
	u8 cid_hi;
} __attribute__((packed));

static inline u32 nvmeib_wire_op_cid_get_cid(const struct nvmeib_login_wire_op_cid *op_cid)
{
	return (((u32)op_cid->cid_hi << 16) |
		((u32)op_cid->cid_mid << 8) |
		((u32)op_cid->cid_lo << 0));
}

static inline u8 nvmeib_wire_op_cid_get_rsp_opcode(const struct nvmeib_login_wire_op_cid *op_cid)
{
	return op_cid->rsp_opcode;
}

static inline u8 nvmeib_wire_op_cid_get_req_opcode(const struct nvmeib_login_wire_op_cid *op_cid)
{
	return op_cid->req_opcode.opcode;
}

static inline u8 nvmeib_wire_op_cid_get_req_version(const struct nvmeib_login_wire_op_cid *op_cid)
{
	return op_cid->req_opcode.version;
}

static inline bool nvmeib_wire_op_cid_get_req_local(const struct nvmeib_login_wire_op_cid *op_cid)
{
	return op_cid->req_opcode.local;
}

static inline void nvmeib_wire_op_cid_set_req_local(struct nvmeib_login_wire_op_cid *op_cid,
						    u8 local)
{
	op_cid->req_opcode.local = local;
}

static inline void nvmeib_wire_op_cid_set_cid(struct nvmeib_login_wire_op_cid *op_cid, u32 cid)
{
	op_cid->cid_hi = (cid >> 16) & 0xff;
	op_cid->cid_mid = (cid >> 8) & 0xff;
	op_cid->cid_lo = (cid >> 0) & 0xff;
}

static inline void nvmeib_wire_op_cid_set_req(struct nvmeib_login_wire_op_cid *op_cid, u8 opcode, u32 cid, u8 version, u8 local)
{
	op_cid->req_opcode.opcode = opcode;
	op_cid->req_opcode.version = version;
	op_cid->req_opcode.local = local;

	nvmeib_wire_op_cid_set_cid(op_cid, cid);
}

static inline void nvmeib_wire_op_cid_set_rsp(struct nvmeib_login_wire_op_cid *op_cid, u8 opcode, u32 cid)
{
	op_cid->rsp_opcode = opcode;

	nvmeib_wire_op_cid_set_cid(op_cid, cid);
}

#define TOMA_CONN_HASH_TABLE_SIZE (512)
#define NVMEIB_TOMA_BUF_MAX_LEN	(2*1024*1024)	/* toma --> server */

/* Toma command type sent from disk */
enum nvmeib_toma_cmd_type
{
	NVMEIB_TOMA_CMD_REG,
	NVMEIB_TOMA_CMD_UNREG,
	NVMEIB_TOMA_CMD_SEND,
};

static inline const char *nvmeib_toma_cmd_str(enum nvmeib_toma_cmd_type cmd)
{
	switch (cmd) {
	case NVMEIB_TOMA_CMD_REG: 	return "NVMEIB_TOMA_CMD_REG";
	case NVMEIB_TOMA_CMD_UNREG: return "NVMEIB_TOMA_CMD_UNREG";
	case NVMEIB_TOMA_CMD_SEND: 	return "NVMEIB_TOMA_CMD_SEND";

	default: return "???";
	}
}

enum toma_req_position{
	NVMEIB_TOMA_REQ_FIRST		= (1 << 0),
	NVMEIB_TOMA_REQ_MIDDLE		= (1 << 1),
	NVMEIB_TOMA_REQ_LAST		= (1 << 2),
	NVMEIB_TOMA_REQ_STANDALONE 	= (NVMEIB_TOMA_REQ_FIRST | NVMEIB_TOMA_REQ_LAST),
};

static inline const char *nvmeib_toma_pos_str(enum toma_req_position pos)
{
	switch (pos) {
	case NVMEIB_TOMA_REQ_FIRST: 		return "NVMEIB_TOMA_REQ_FIRST";
	case NVMEIB_TOMA_REQ_MIDDLE: 		return "NVMEIB_TOMA_REQ_MIDDLE";
	case NVMEIB_TOMA_REQ_LAST:			return "NVMEIB_TOMA_REQ_LAST";
	case NVMEIB_TOMA_REQ_STANDALONE:	return "NVMEIB_TOMA_REQ_STANDALONE";

	default: return "???";
	}
}

#define NVMEIB_TAG_VERSION_RESERVED 13 /* Because why not 13? */

union nordda_tag {
	struct {
		u16 index; /* Important for backward compatibility - index is LSB */
		u16 version;
		u32 ch_version;
	}__attribute__((packed));
	u64 all;
}__attribute__((packed));

#define NVMEIB_INC_TAG_VERSION(ver_)                                           \
	({                                                                         \
		++ver_;                                                                \
		if (!ver_) ++ver_;                                                      \
		if (ver_ == NVMEIB_TAG_VERSION_RESERVED) ++ver_;                       \
		ver_;                                                                  \
	})

/* TODO: get rid of inlines inside h files, they are evil, must be either
 macro or non-inline or inline inside c file */
#define nordda_tag_encode(remote_ver_, ch_version_, version_, index_)          \
	({                                                                         \
		u64 ____result;                                                        \
		if (nvmeib_version_protocol_lt(remote_ver_, &nvmeib_2p2_version)) {     \
			____result = (u64)index_;                                          \
		} else {                                                               \
			union nordda_tag tag = {.ch_version = ch_version_,                 \
			                        .version = version_,                       \
			                        .index = index_};                          \
			____result = tag.all;                                              \
		}                                                                      \
		____result;                                                            \
	})

/* For decoding, version check is not important due to the fields order.
 * Index is always on low bits, so even when sent from the older versions.
 */
static inline u16 nordda_tag_decode_index(u64 tag)
{
	union nordda_tag nr_tag = { .all = tag };
	return nr_tag.index;
}

static inline u16 nordda_tag_decode_version(u64 tag)
{
	union nordda_tag nr_tag = { . all = tag };
	return nr_tag.version ? nr_tag.version : NVMEIB_TAG_VERSION_RESERVED;
}

static inline bool nordda_tag_decode_version_decode_verify_reserved(u16 cmp, u64 tag) {
	u16 ver = nordda_tag_decode_version(tag);
	return ver != NVMEIB_TAG_VERSION_RESERVED && ver != cmp;
}

static inline u32 nordda_tag_decode_ch_version(u64 tag)
{
	union nordda_tag nr_tag = { . all = tag };
	return nr_tag.ch_version ? nr_tag.ch_version : NVMEIB_TAG_VERSION_RESERVED;
}

static inline bool nordda_tag_decode_ch_version_decode_verify_reserved(u16 cmp, u64 tag) {
	u16 ver = nordda_tag_decode_ch_version(tag);
	return ver != NVMEIB_TAG_VERSION_RESERVED && ver != cmp;
}

/* after calling nordda_tag_decode_version or nordda_tag_decode_ch_version,
   verify returned version (u16, i.e. truncating in ch-ver's case) considering
   NVMEIB_TAG_VERSION_RESERVED.
   This allows caller to print the (semi) decoded version value */
static inline bool nordda_tag_decode_verify_any_ver_consider_reserved(u16 cmp, u16 ver) {
	return ver != NVMEIB_TAG_VERSION_RESERVED && ver != cmp;
}


#endif //NVMEIB_MSGS_H
