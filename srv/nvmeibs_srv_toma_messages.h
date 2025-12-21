#ifndef NVMEIBS_SRV_TOMA_MESSAGES_H
#define NVMEIBS_SRV_TOMA_MESSAGES_H

#include "../common/nvmeib_types.h"
#include "../toma/clnt/nvmeibt_client_protocol.h"

/******************************** Old proc API ****************************/
enum nvmeibs_toma_status_type {		// Server requests Toma for the current status specific of s specific component
	NVMEIBS_TOMA_STATUS_ALL = 0,
	NVMEIBS_TOMA_STATUS_RAFT,
	NVMEIBS_TOMA_STATUS_DSEG,
	NVMEIBS_TOMA_STATUS_BDEV,
	NVMEIBS_TOMA_STATUS_DISK,
	NVMEIBS_TOMA_STATUS_IB,
	NVMEIBS_TOMA_STATUS_RECOVER,
	NVMEIBS_TOMA_STATUS_CFG,
	NVMEIBS_TOMA_STATUS_TOPO,
	NVMEIBS_TOMA_STATUS_LEADER,
	NVMEIBS_TOMA_STATUS_LOCAL_DISKS,
	NVMEIBS_TOMA_STATUS_MEM_ALLOC,
	NVMEIBS_TOMA_STATUS_ALL_JSON,
	NVMEIBS_TOMA_STATUS_NM_JSON,
	NVMEIBS_TOMA_STATUS_KAFKA_INFO,
	NVMEIBS_TOMA_STATUS_AUTO_MAX, // The statuses after this will not be generated automatically by the server
	NVMEIBS_TOMA_STATUS_ZEROING,
};

struct nvmeibs_msg_s2t_toma_status_req {	// s2t means Server to Toma, t2s is Toma to Server
	enum nvmeibs_toma_status_type type;
	int handle;								// Cookie passed from srvr to Toma and returned in the reply
	size_t max_length;						// Max byte length of the reply inlcuding terminating \0
	char fname[16];							// The proc file name that Toma should fill
	int handle_req; /* Cookie passed from srvr to Toma and returned in the reply. When we have more than one caller, we need to differentiate between them */
};

struct nvmeibs_msg_t2s_toma_status_resp {
	int handle;								// Copied from the request
	size_t length;							// Actual byte length of the reply
	int is_overflow;						// was req->max_length too short for the reply buffer and it was truncated
	int handle_req; 						// Copied from the request
};

struct nvmeibs_msg_t2s_journal {				// Toma notifies server about the location of partitions a disk's (journal/serjio-db)
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u64 lba;
	u64 length;
	u64 serjio_db_lba;
	u64 serjio_db_length;
};

struct nvmeibs_msg_t2s_clean_journal_for_range {
	char	disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	u16		vendor_id;
	char    seg_uuid[URN_UUID_STR_LENGTH + 1];
	u64		start_4Klba;
	u64		end_4Klba;
	u8		seg_deleted;
};

struct nvmeibs_msg_s2t_blkset_recovered {		// Server, pass client notification about recovered blockset to toma
	char	disk_segment_urn_uuid_str[NVMEIB_GID_STR_MAX];
	u64		blkset_no;			// Blockset number in praid, used by Toma
	u64		blkset_slba;		// Disk lba of first block in blockset (used by serjio)
	u64		pre_recov_lock_val; /* used for both lock-id and dbits */
	u64		cookie; /* used to get ack on a specific message */
};

struct nvmeibs_msg_t2s_blkset_recovered_ack {
	u64 cookie; /* must be a copy of the cookie from the above request */
	u32 toma_rv; /* In case the operation was a failure will contain an error code */
};

struct nvmeibs_msg_s2t_launch_JGC {			// Server instructs Toma to start journal garbage collection recovery
	char	disk_segment_urn_uuid_str[NVMEIB_GID_STR_MAX];
	char	disk_id_str[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
};

struct nvmeibs_toma_subscriber_change_msg {
	union nvmeib_uuid	client_uuid;
	u32					cid __attribute__ ((packed));	// client id
	char				disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	char				host_name[NVMEIB_HOST_NAME_LEN];
	bool				is_subscribe;
	u64					toma_conn_proc_handle;		// Highest 32bits is client id, Lower 32 is identifier of segment. Together they identify conenction of client per segment. Higher 32 bits may be use to disconnect client from all segment on disk
}__attribute__((packed));

struct nvmeibs_msg_s2t_serjio_range_cleaned {
	char seg_id[URN_UUID_STR_LENGTH + 1];
};

struct nvmeibs_t2s_msg_client_disconnect_force_cmd {
	u32 cid;
}__attribute__((packed));

struct nvmeibs_msg_s2t_port_gid_change {
	char ib_dev[NVMEIB_IB_DEVICE_NAME_MAX];
	u8 port;
	char gid_str[NVMEIB_GID_STR_MAX];
}__attribute__((packed));

struct nvmeibs_msg_s2t_nic_change {
	char ib_dev[NVMEIB_IB_DEVICE_NAME_MAX];
	bool add;
}__attribute__((packed));

struct nvmeibs_msg_s2t_disk_change {
	u64 n_blocks;
	u64 n_hw_blocks;
	u64 vendor_id;
	char model_str[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	char dev_name[DISK_NAME_LEN*4];				// EC-3152: *2 is very fishy
	char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
	char status[LOCAL_DISK_STATUS_STR_LEN];
	u32 block_size;
	u32 max_request_size;
	u32 seq;
	u32 nsid;
	u32 metadata;
	char op;									// 'a' = add, 'r' = remove, 'c', 's' ???
	char native_serial_str[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
}__attribute__((packed));

/* Toma <--> Server message format. Both directions */
struct nvmeibs_toma_server_proc_buf {
	union {
		struct {		// The first u64 decides between server event or client message to TOMA
			u32 type;	// For server event, the zero member must be 0, and then the type member indicates the server event type.
			u32 zero;	// For client messages, the client-uid (cid) - which occupies the higher half of the handle- may not be zero.
		};
		u64 handle;
	};
	union {
		struct nvmeibs_toma_subscriber_change_msg subscriber_change_msg;
		struct nvmeibs_toma_client_disconnect_msg_hdr client_disconnect_msg_hdr;
		struct nvmeibs_msg_s2t_disk_change disk_change_msg;
		struct nvmeibs_t2s_msg_client_disconnect_force_cmd client_disconnect_force_cmd;
		struct nvmeibs_msg_s2t_port_gid_change port_gid_change_msg;
		struct nvmeibs_msg_s2t_nic_change nic_change_msg;
		struct nvmeibs_msg_s2t_toma_status_req status_req_msg;
		struct nvmeibs_msg_t2s_toma_status_resp status_resp_msg;
		struct nvmeibs_msg_t2s_journal journal_msg;
		struct nvmeibs_msg_t2s_clean_journal_for_range clean_journal_msg;
		struct nvmeibs_msg_s2t_blkset_recovered blkset_recovered_msg;
		struct nvmeibs_msg_t2s_blkset_recovered_ack blkset_recovered_ack_msg;
		struct nvmeibs_msg_s2t_launch_JGC trigger_JGC_cmd;
		struct nvmeibs_msg_s2t_serjio_range_cleaned serjio_range_cleaned_msg;
		u8 buf[0];
	};
}__attribute__((packed));

/* Server:Toma msg type */
enum nvmeibs_toma_server_msg_type {
	/* Toma requests to Server and corresponding responses */
	// NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_REQ		= 0x00,  Deprecated
	// NVMEIBS_TOMA_DISK_SEGMENT_LOCK_GID_RSP		= 0x01,

	/*Toma login*/
	NVMEIBS_TOMA_LOGIN							= 0x02,
	NVMEIBS_TOMA_LOGOUT							= 0x03,

	NVMEIBS_TOMA_CLIENT_DISCONNECT_FORCE_CMD	= 0x08,

	NVMEIBS_TOMA_JOURNAL_INFO					= 0x09,

	NVMEIBS_TOMA_CLEAN_JOURNAL_FOR_DISK_RANGE	= 0x16,

	/* Server event notifications to Toma */
	NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE 			= 0x10,
	NVMEIBS_TOMA_REPORT_EVENT_SUBSCRIBER_CHANGE		= 0x11,
	NVMEIBS_TOMA_REPORT_EVENT_CLIENT_DISCONNECT		= 0x12,
	NVMEIBS_TOMA_REPORT_EVENT_PORT_GID_CHANGE		= 0x13,
	NVMEIBS_TOMA_REPORT_EVENT_NIC_CHANGE			= 0x14,
	/*NVMEIBS_TOMA_REPORT_EVENT_SERJIO_STATE_CHANGE	= 0x15, - Moved to nelink */
	NVMEIBS_TOMA_REPORT_EVENT_SERJIO_RANGE_CLEANED	= 0x16,

	/* Server requests to TOMA and corresponding responses */
	NVMEIBS_TOMA_WRITE_STATUS_REQ					= 0x20,
	NVMEIBS_TOMA_WRITE_STATUS_RESP					= 0x21,
	NVMEIBS_TOMA_TRIGGER_JGC						= 0x22,		// Serjio triggers a JournalGarbageCollection on a seg
	NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED		= 0x23,
	NVMEIBS_TOMA_REPORT_EVENT_BLKSET_RECOVERED_ACK	= 0x24,
};

/******************************** New netlink API ****************************/
#define NETLINK_SRV_COMM 31
#define NETLINK_SRV_COMM_MAX_PAYLOAD 1024
#define NVMESH_NL_MSG_TYPE (NLMSG_MIN_TYPE + 1)
#define TOMA_CALLER 'T'
#define INFRA_CALLER 'I'
#define LOCAL_CLNT_CALLER 'C'
#define TOMA_SILENCE_MAX_PERIOD_SECS (3600)

struct nvmeib_nl_uk_comm_msg {
	/* header and data */
    int len;
    int opcode;
	/* the name of the caller */
	char caller_type;
    unsigned long id __attribute__((aligned(8)));
    char data[0];
};

struct nvmeib_nl_uk_comm_rep {
    int opcode;
	int error;
	long long latency_ns;
};

typedef void (*on_uk_comm_done_func)(void *ctx, int ok,
									 struct nvmeib_nl_uk_comm_rep *rep);
enum uk_comm_opcode {
	csc_start = 0,
	csc_get_disk_names,					// Version 1.3+
	csc_zero_disk,
	csc_test_zero_disk,

	csc_register_disk_events,			// Version 2.0+
	csc_get_disks,
	csc_remove_disk,
	csc_format_disk,
	csc_io_to_disk,
	csc_keep_alive,
	csc_identify_disk,

	csc_local_client,					// Version 2.3+
	csc_local_clnt_msg_to_toma_unused,
	csc_toma_msg_to_local_clnt,		// Example: recovery attach
	csc_msg_to_process,

	/* should be the last just before the end */
#if defined(UK_ZERO_TEST) && UK_ZERO_TEST
	csc_contaminate_disk,
#endif
	csc_end
};

static inline const char * uk_comm_opcode_str(int opcode)
{
	switch (opcode) {
	case csc_get_disk_names: return "csc_get_disk_names";
	case csc_zero_disk: return "csc_zero_disk";
	case csc_test_zero_disk: return "csc_test_zero_disk";

	case csc_register_disk_events: return "csc_register_disk_events";
	case csc_get_disks: return "csc_get_disks";
	case csc_remove_disk: return "csc_remove_disk";
	case csc_format_disk: return "csc_format_disk";
	case csc_io_to_disk: return "csc_io_to_disk";
	case csc_keep_alive: return "csc_keep_alive";
	case csc_identify_disk: return "csc_identify_disk";

	case csc_local_client: return "csc_local_client";
	case csc_msg_to_process: return "csc_msg_to_process";

#if defined(UK_ZERO_TEST) && UK_ZERO_TEST
	case csc_contaminate_disk: return "csc_contaminate_disk";
#endif
	default: return "csc_unknown";
	}
}

enum uk_comm_err_opcode {
	csce_ok = 0,
	csce_failed,
	csce_bad_zero_params,
	csce_format_in_progress,
	csce_format_oom,
	csce_format_thread,
	csce_format_failed,
	csce_io_failed,
	csce_identify_failed,
	csce_identify_thread,
	csce_disk_stopped,
	csce_unsupported_opcode,
	csce_end,
};

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

struct nvmeib_nl_toma_msg_hdr {		// Use the same header msg_to_toma and msg_from_toma. No real reason
	int							opcode;
	char						caller_type;
	pid_t						toma_pid;
};

struct nvmeib_nl_msg_to_toma {
	struct nvmeib_nl_toma_msg_hdr	hdr;
	union srvr2toma_payload_t {
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

static inline int nvmeibs_max_nl_reply(void)
{
	return sizeof(struct nvmeib_nl_uk_comm_msg) + sizeof(union srvr2toma_payload_t);
}

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
