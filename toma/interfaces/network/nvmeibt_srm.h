#ifndef NVMEIBT_SRM_H
#define NVMEIBT_SRM_H

#include <stdint.h>

#define SRM_EMPTY_USER ((void *)(~(0ULL)))
#define NO_FRAME ((uint16_t)-1)

typedef uint64_t wnd_mask_t;
#define RSRM_SWND_SIZE (64) // CPP does not do sizeof()
#define RSRM_SWND_POS_MASK (RSRM_SWND_SIZE - 1)
#define RSRM_SWND_BITFLD_MASK ((wnd_mask_t)-1)
#define SRM_MAX_MSG_NUM (2 * RSRM_SWND_SIZE)

#define RSRM_QP_MSG_COUNT	8

//#define RSRM_DEBUG_IB_TRANSPORT

#if defined(RSRM_DEBUG_ROCE_TRANSPORT)
#define EXT_DATA_CHUNK (15<<10)
#elif defined(RSRM_DEBUG_IB_TRANSPORT)
#define EXT_DATA_CHUNK (4096 * 15)
#else
#define EXT_DATA_CHUNK 0
#endif

struct nvmeibt_srm;
struct connection_context;

struct rsrm_frame_header {
	uint16_t msg_id;
	uint16_t frame_id; // up to 90MB
	uint32_t srm_id;
	uint32_t msg_length;
	uint32_t msg_type;
	uint32_t data_offset;
	uint16_t frame_len;
} __attribute__ ((packed));

struct nvmeibt_msg_header {
	uint32_t srm_id;
	uint64_t msg_id;
	uint32_t msg_length;
	uint32_t chunk_length;
	uint32_t msg_type;
	uint32_t chunk_counter;
};

struct nvmeibt_chunk_header {
	uint32_t srm_id;
	uint64_t msg_id;
	uint32_t chunk_length;
	uint32_t chunk_counter;
	uint32_t more;
	uint32_t pad_u64;
};

struct nvmeibt_wire_msg {
	uint16_t send_ctx_idx;
	void *buffer;
	int len;
};

typedef enum {
	srs_send_ok_unused = 0,
	src_send_error,
	src_send_comp_ok,
} nvmeibt_sr_status_t;

struct nvmeibt_msg_request;
struct nvmeibt_sr_cb_table {
	/* data send-completion callback */
	void (*send_c)(void *arg, int status);
	/* lock send-completion callback */
	void (*lock_c)(void *arg, int status, uint64_t compare);
	void (*cancel_c)(void *arg, struct nvmeibt_msg_request *req);
};

struct nvmeibt_msg_request {
	unsigned int msg_type;
	unsigned int reason;
	union {
		struct {
			union {
				void *msg;
				const void *cnst_msg;
			};
			int msg_len;
			union {
				char *data;
				const char *cnst_data;
			};
			int data_len;
		};
	};
	struct nvmeibt_sr_cb_table cbs;
	void *arg;
	void *user;
	int can_back_to_pool;
};

static inline int msg_transfer_len(struct nvmeibt_msg_request *req)
{
	return req->msg_len + req->data_len;
}

typedef enum {
	u_stop,
	u_stop_all,
} nvmeibt_u_stop_t;

struct nvmeibt_user_cb_table {
	/* get srm-user identifier
	 * from received buffer */
	void *(*get_user_c)(void *buf);

	/* stop srm-user.
	 * upon return, user is done processing all
	 * msgs that were already forwarded to it */
	void (*stop_c)(void *arg, int reason);
};

struct nvmeibt_srm_user {
	void *user;
	struct nvmeibt_user_cb_table cbs;
};

enum carrier_type {
	carrier_rdma,
	carrier_udp,
	carrier_num
};

struct carrier {
	enum carrier_type type;

	int max_chunk_size;
	int max_messages;
	int srm_data_offset;

	union {
		struct connection_context *conn;
		struct udp_peer *udp_peer;
		void *ctx;
	};

	const char *(*conn_name)(const struct carrier *);
	void *(*carrier_obj)(const struct carrier *);
	struct nvmeibt_srm *(*srm_obj)(const struct carrier *);
	void *(*alloc_msgs_buffer)(const struct carrier *);
	void  (*release_msgs_buffer)(const struct carrier *);
	int (*send_msg)(const struct carrier *, struct nvmeibt_wire_msg *, uint16_t msg_id, bool);
	int (*get_timer_fd)(const struct carrier *);
};

static inline void *srm_data_buffer(struct nvmeibt_wire_msg *wire,
					const struct carrier *car)
{
	return wire->buffer + car->srm_data_offset;
}

struct nvmeibt_srm *nvmeibt_srm_create(const struct carrier *);
void nvmeibt_srm_free(struct nvmeibt_srm *srm);
int nvmeibt_srm_queue_req(struct nvmeibt_srm *srm,
	struct nvmeibt_msg_request *req);
void nvmeibt_srm_cancel_req(struct nvmeibt_srm *srm);
void nvmeibt_srm_send_completion(struct nvmeibt_srm *srm,
	uint16_t send_ctx_idx);
int nvmeibt_srm_receive_completion(struct nvmeibt_srm *srm, void *buffer);
void srm_post_recv_cmpl(struct nvmeibt_srm *srm);
void srm_post_send_cmpl(struct nvmeibt_srm *srm);
void nvmeibt_srm_post_cmpl(struct nvmeibt_srm *srm);
int nvmeibt_srm_register_send_user(struct nvmeibt_srm *srm,
	struct nvmeibt_srm_user *user);
int nvmeibt_srm_register_receive_user(struct nvmeibt_srm *srm,
	struct nvmeibt_srm_user *user);
int nvmeibt_srm_allow_send(struct nvmeibt_srm *srm, uint32_t send_srm_id);
int nvmeibt_srm_stop_sender(struct nvmeibt_srm *srm, void *user);
int nvmeibt_srm_stop_all_senders(struct nvmeibt_srm *srm);
int nvmeibt_srm_allow_receive(struct nvmeibt_srm *srm, uint32_t recv_srm_id);
int nvmeibt_srm_stop_receiver(struct nvmeibt_srm *srm, void *user);
int nvmeibt_srm_stop_all_receivers(struct nvmeibt_srm *srm);
int nvmeibt_srm_stop_all(struct nvmeibt_srm *srm);

void nvmeibt_srm_set_lock_owner(struct nvmeibt_srm *srm, int f);
uint32_t nvmeibt_srm_lock_region_lkey(struct nvmeibt_srm *srm);
uint32_t nvmeibt_srm_lock_region_rkey(struct nvmeibt_srm *srm);
void srm_terminate_all_works(struct nvmeibt_srm *srm);

typedef void *(*extract_user_t)(void *buf);

// API SRM<-->Toma
int  rsrm_init_work_tmq(void);			// Create SRM
void rsrm_resend_acks(void);			// Periodic high priority job: Send acks for recieved packets
int  rsrm_get_fd_timer(void);			// Get Timer file descriptor for periodic timer based resent packets
int  rsrm_resend_timer(void);			// Callback for timer

// API SRM faults<-->Toma
int  rsrm_faults_init_fifo_comm(void);	// Create Fifo queue (cli file)
int  rsrm_faults_get_fd(void);			// Get descriptor of fifo to select
void rsrm_faults_handle_fifo_comm(void);// Handle fault after wakeup from select
void rsrm_destroy_after_run(void);
#endif	// NVMEIBT_SRM_H
