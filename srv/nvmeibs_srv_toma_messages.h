/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBS_SRV_TOMA_MESSAGES_H
#define NVMEIBS_SRV_TOMA_MESSAGES_H

#include "../common/nvmeib_types.h"
#include "../toma/clnt/nvmeibt_client_protocol.h"

/* Request structures between TOMA and nvmeibs */

struct nvmeib_zero_disk {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int vendor_id;
	unsigned long start_hw_sector;
	unsigned long n_hw_sectors;
	struct {
		u8 is_hw : 1;
		u8 is_zeroing_using_test_and_write : 1;
		u8 is_using_nvme_trim_before_zero : 1;
		u8 is_secure_erase_after_disk_format : 1;
		u8 is_zeroing_mandatory : 1;
	};
};

#if defined(UK_ZERO_TEST) && UK_ZERO_TEST
struct nvmeib_contaminate_disk {
	struct nvmeib_zero_disk base;
	int percentage_zero;
};
#endif

union nvme_format_id {
	u32 val;
	struct {
		u32 id : 4;
		u32 is_inline : 1;
	};
};

struct nvmeib_format_disk {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int vendor_id;
	union nvme_format_id format_id;
	union {
		u32 flags;
		struct {
			u32 flag_nvme_format : 1;
			u32 flag_delete_create_ns : 1;
			u32 flag_reset_ctrlr : 1;
		};
	};
};

enum nvmeib_main_gpt_update_flags {
	NO_MAIN_GPT_UPDATE 				=	0x0,
	
	MAIN_GPT_UPDATE_STAGE_MASK			=	0x3,
	MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE 	= 	0x1,
	MAIN_GPT_UPDATE_STAGE_ENTRIES 			=	0x2,
	MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE 	= 	0x3,
	
	MAIN_GPT_UPDATE_SERJIO_MASK			=	0x4,
	MAIN_GPT_UPDATE_SERJIO_INIT			=	0x4,
};

static inline const char *nvmeib_gpt_update_str(enum nvmeib_main_gpt_update_flags gpt_update, bool is_primary)
{
	switch (gpt_update & MAIN_GPT_UPDATE_STAGE_MASK) {
	case NO_MAIN_GPT_UPDATE:
		return "No Main GPT Update";
	case MAIN_GPT_UPDATE_STAGE_HEADER_PRE_UPDATE:
		if (is_primary)
			return "Main Primary GPT Header Pre-Update";
		else
			return "Alternate Primary GPT Header Pre-Update";
	case MAIN_GPT_UPDATE_STAGE_ENTRIES:
		if (is_primary)
			return "Main Primary GPT Entries";
		else
			return "Alternate Primary GPT Entries";
	case MAIN_GPT_UPDATE_STAGE_HEADER_POST_UPDATE:
		if (is_primary)
			return "Main Primary GPT Header Post-Update";
		else
			return "Alternate Primary GPT Header Post-Update";
	default:
		return "Unknown Main GPT Update";
	}
}

struct nvmeib_io_to_disk {
	char 			disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int 	vendor_id;
	int				pid;
	unsigned long	start_sector;
	char			*data;
	union {
		char			*md;
		const void		*gpt_entries;
	};
	unsigned int	data_len;
	unsigned int	md_len;
	union {
		struct {
			unsigned is_read : 1;
			unsigned is_hw : 1;
			unsigned gpt_update_flags : 3;
		};
		unsigned long v;
	};
};

static inline void nvmeib_clear_io_to_disk(struct nvmeib_io_to_disk *io_to_disk)
{
	io_to_disk->start_sector = 0;
	io_to_disk->data = NULL;
	io_to_disk->md = NULL;
	io_to_disk->data_len = 0;
	io_to_disk->md_len = 0;
	io_to_disk->v = 0;
}

enum nvmeib_io_is_read {
	NVMEIB_IO_IS_WRITE = 0,
	NVMEIB_IO_IS_READ,
};

static inline void nvmeib_init_io_to_disk(struct nvmeib_io_to_disk *io_to_disk,
								   unsigned long start_sector,
								   char *data, unsigned int data_len,
								   char *md, unsigned int md_len,
								   enum nvmeib_io_is_read is_read, enum nvmeib_main_gpt_update_flags main_gpt_update_flags)
{
	io_to_disk->start_sector = start_sector;
	io_to_disk->data = data;
	io_to_disk->data_len = data_len;
	io_to_disk->md = md;
	io_to_disk->md_len = md_len;
	io_to_disk->is_read = is_read;
	io_to_disk->gpt_update_flags = main_gpt_update_flags;
}


struct nvmeib_identify_disk {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	int pid;
	void *data;
	int data_len;
};

enum NVMEIB_DISK_EVENT {
	NVMEIB_DISK_EVENT_PLUG			= 0x0,
	NVMEIB_DISK_EVENT_UNPLUG		= (0x1 << 0)
};

struct nvmeib_ack_disk_event {
	struct nvmeib_nl_uk_comm_rep base;
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int vendor_id;
	enum NVMEIB_DISK_EVENT acked_event;
};

struct nvmeib_remove_disk {
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u64 vendor_id;
	unsigned long ack_id;
};

/* Response structures*/
struct nvmeib_zero_disk_reply {
	struct nvmeib_nl_uk_comm_rep base;
};

#if defined(UK_ZERO_TEST) && UK_ZERO_TEST
struct nvmeib_contaminate_disk_reply {
	struct nvmeib_nl_uk_comm_rep base;
};
#endif

struct nvmeib_io_to_disk_reply {
	struct nvmeib_nl_uk_comm_rep base;
	char 			disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int 	vendor_id;
	unsigned int	n_data_io;
	unsigned int	n_md_io;
};

struct nvmeib_identify_disk_reply {
	struct nvmeib_nl_uk_comm_rep base;
	char 			disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	unsigned int	data_len;
};

struct nvmeib_copied_rscs_reply {
	struct nvmeib_nl_uk_comm_rep base;
	void *rsc;
};

struct nvmeib_short_disk_info {
	bool is_dummy;
	union {
		struct {
			char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
			u32 hw_block_size;
			u32 hw_md_size;
			u64 n_blocks;
			u64 n_hw_blocks;
			u32 sw_block_size;
			u32 sw_md_size;
			u32 md_inline;
			u32 max_request_size;
		};
		char dummy_name[256];
	};
};

struct nvmeib_get_disk_names_reply {
	struct nvmeib_nl_uk_comm_rep base;
	int n_disks;
	char dummy[4];
	struct nvmeib_short_disk_info info[0];
};

struct nvmeib_new_format_info {
	char new_dev_file_name[256];
	uint64_t new_n_pblk;
	int	new_seq;
};

struct nvmeib_format_disk_reply {
	struct nvmeib_nl_uk_comm_rep base;
	struct nvmeib_new_format_info info;
};

struct nvmeib_test_zero_reply {
	struct nvmeib_nl_uk_comm_rep base;
	unsigned long failed_sw_lba;
};

struct nvmeib_disk_info {
	u64 n_blocks;
	u64 n_hw_blocks;
	u64 vendor_id;
	char dev_name[DISK_NAME_LEN*2];
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	char status[LOCAL_DISK_STATUS_STR_LEN];
	char model_str[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	char native_serial_str[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u32 block_size;
	u32 max_request_size;
	u32 seq;
	u32 nsid;
	u32 metadata;
	u32 max_n_hw_sectors;
	u32 max_n_sw_sectors;
};

enum nvmeib_disk_info_reply_selector {
	nvmeib_disk_info_reply_dinfo,
	nvmeib_disk_info_reply_serjio_state,
	nvmeib_disk_info_reply_dummy,
};

struct nvmeib_disk_info_reply {
	struct nvmeib_nl_uk_comm_rep base;
	enum nvmeib_disk_info_reply_selector selector;
	union {
		struct {
			struct nvmeib_disk_info disk;
			enum nvmeibs_serjio_status serjio_status;
		} dinfo;
		struct {
			char dummy_name[256];
			bool remove_disk;
		};
		struct {
			char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
			u16 vendor_id;
			char model_str[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
			enum nvmeibs_serjio_status serjio_status;
		} serjio_state_change;
	};
};

struct nvmeib_register_change_disk {
	int (*on_add_disk)(void *ctx, struct nvmeib_disk_info *);
	void *add_ctx;
	int (*on_remove_disk)(void *ctx, struct nvmeib_remove_disk *);
	void *remove_ctx;
};

struct nvmeib_push_msg_process {
	struct nvmeib_nl_uk_comm_rep base;
	char start[0];
};

static inline int nvmeibs_max_nl_reply(void)
{
	return
		sizeof(struct nvmeib_nl_uk_comm_msg) +
		sizeof(
				union {
						char _1[sizeof(struct nvmeib_nl_uk_comm_rep)];
						char _2[sizeof(struct nvmeib_test_zero_reply)];
						char _3[sizeof(struct nvmeib_ack_disk_event)];
						char _4[sizeof(struct nvmeib_format_disk_reply)];
						char _5[sizeof(struct nvmeib_io_to_disk_reply)];
						char _6[sizeof(struct nvmeib_get_disk_names_reply)];
						char _7[sizeof(struct nvmeib_disk_info_reply)];
						char _8[sizeof(struct nvmeib_identify_disk_reply)];
						char _9[sizeof(struct nvmeib_push_msg_process)];
						char _a[sizeof(struct nvmeib_copied_rscs_reply)];
				}
		);
}

struct nvmeib_nl_toma_msg_hdr {		// Use the same header msg_to_toma and msg_from_toma. No real reason
	int							opcode;
	char						caller_type;
	pid_t						toma_pid;
};

struct nvmeib_nl_msg_to_toma {
	struct nvmeib_nl_toma_msg_hdr	hdr;
	union {
		struct nvmeib_nl_uk_comm_rep			nl_uk_comm_rep;
		struct nvmeib_test_zero_reply			test_zero_reply;
		struct nvmeib_ack_disk_event			ack_disk_event;
		struct nvmeib_format_disk_reply			format_disk_reply;
		struct nvmeib_io_to_disk_reply			io_to_disk_reply;
		struct nvmeib_get_disk_names_reply		get_disk_names_reply;
		struct nvmeib_disk_info_reply			disk_info_reply;
		struct nvmeib_identify_disk_reply		identify_disk_reply;
		struct nvmeib_copied_rscs_reply			copied_rscs_reply;
	} payload;
};

/* message from toma to ... */
struct nvmeib_nl_msg_from_toma {
	struct nvmeib_nl_toma_msg_hdr hdr;
	union {
		/* message from toma to local client.
		   if the data is copied, toma will receive a message
		   at teh end of a successfult copy and error otherwise...
		*/
		struct nvmeib_toma_client {
			enum RECOVERY_ATTACH_CMD		attach_cmd;
			/* if true the message data in the message must be copied before
			   calling the client API
			*/
			int copy;
			/* numebr of pages that are needed to be copied */
			unsigned n_pages;
			/* the data to be transfered to the client.  in the case where the
			   message is needed to be copied the message pointer must be
			   page aligned
			*/
			void *data;
		} toma_client;
	} payload;
};

#endif // NVMEIBS_SRV_TOMA_MESSAGES_H
