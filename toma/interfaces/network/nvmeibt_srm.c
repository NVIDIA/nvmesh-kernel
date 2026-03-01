#include <pthread.h>
#include <endian.h>
#include <byteswap.h>
#include <netinet/in.h>
#include <time.h>
#include <sys/timerfd.h>
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_srm.h"
#include "nvmeibt_ds.h"
#include "../common/nvmeib_hash.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_raft.h"
#include "interfaces/network/network_incs.h"
#include "../common/nvmeib_shared.h"

#if defined(COMPILE_DEBUG)
#   define SRM_ENABLE_FAULTS 1
#endif

#define USE_ABORT 1
#define TERMINATE(x) if (USE_ABORT) nvmeibt_abort(ES_FATAL); else goto x

#define CRITICAL_BUG() nvmeibt_abort(ES_FATAL)


#define N__D(__name__, fmt, ...) N_Df(__name__, "@SRM_NAME-" fmt, srm ? srm->name : "?", ## __VA_ARGS__)
#define N__T(__name__, fmt, ...) N_Tf(__name__, "@SRM_NAME-" fmt, srm ? srm->name : "?", ## __VA_ARGS__)
#define N__I(__name__, fmt, ...) N_If(__name__, "@SRM_NAME-" fmt, srm ? srm->name : "?", ## __VA_ARGS__)
#define N__W(__name__, fmt, ...) N_Wf(__name__, "@SRM_NAME-" fmt, srm ? srm->name : "?", ## __VA_ARGS__)
#define N__E(__name__, fmt, ...) N_Ef(__name__, "@SRM_NAME-" fmt, srm ? srm->name : "?", ## __VA_ARGS__)

struct _frame_info {
	uint16_t frame_id;
	uint16_t send_count;
	uint16_t send_cycle;
	uint16_t msg_index;
};

struct rsrm_send_window {
	wnd_mask_t send_mask; /* posted frames set after post send, cleared on slide only*/
	wnd_mask_t ack_mask   /* set on ACK arrival*/;
	wnd_mask_t nack_mask;
	uint8_t head_pos;  /* head index in frame[] */
	uint8_t wnd_size;
	uint16_t head_frame_id;
	uint16_t acked_frames;
	struct _frame_info frame[RSRM_SWND_SIZE];
};

struct rsrm_recv_window {
	uint16_t msg_id;
	uint16_t head_frame_id;
	uint32_t ack_id;
	wnd_mask_t recv_mask;
	uint8_t msg_frames_received;
	uint8_t poll_frames_received;
	uint8_t acks_to_send;
};

enum wire_owner {
	WIRE_OWNER_HOST,
	WIRE_OWNER_PORT,
	WIRE_OWNER_NUM
};

struct connection_work;
struct srm_recv_context;
struct connection_msg {
	struct nvmeibt_wire_msg wire;
	union {
		struct connection_work *work;
		struct srm_recv_context *recv_ctx;
	};
	struct connection_msg *next;
	uint16_t send_cnt;
	struct xdlist link;
};
struct nvmeibt_srm;
struct connection_work {
	struct nvmeibt_srm *srm;
	struct nvmeibt_msg_request *req;
	bool in_use;
	union {
		char *pos;
		struct {
			uint16_t total_num;
			uint16_t next_to_send;
			uint32_t data_offset;
		} frame;
		#define frames_num frame.total_num
		#define frame_to_send frame.next_to_send
		#define frame_data_offset frame.data_offset
	};
	// RAFT messages issued each 200ms
	// 16 bit msg_id will wrap around after 3.64 hrs
	uint16_t msg_id;
	int counter;
	int on_wire;
	int msg_count;
	void *user;
	bool canceled;
	bool error;
	uint64_t queue_tm;
	struct connection_msg *msg_head;
	struct rsrm_send_window swnd;
	uint32_t ack_id;
	struct timespec work_time;
	struct timespec window_time;
	struct xdlist link;
	struct xdlist tm_link;
};

#define SRM_RESERVED_MSG_ID (0)

struct srm_user {
	struct nvmeibt_srm_user user;
	struct xdlist link;
};

#define MAX_MSG_LEN 1024

struct msg_request_wrapper {
	struct nvmeibt_msg_request req;
	char msg_inline[MAX_MSG_LEN];
	int cmpl_status;
	struct xdlist link;
	struct xdlist cmpl_link;

};

struct lock_region {
	void *addr;
	unsigned len;
	uint32_t rkey;
	struct xdlist link;
	int refcnt;
};

struct srm_recv_context {
	void *rcv_buffer;
	void *rcv_pos;
	struct connection_msg *ack_msg;
	int rcv_buffer_len;
	int rcv_size;
	int acks_num;
	uint32_t recv_srm_id;
	uint64_t start_recv;
	struct rsrm_recv_window rwnd;
	struct timespec start_time;
	struct xdlist link;
	struct xdlist recv_completed_link;
};

#define REPORT_INITIAL_SIZE (100UL * 1024 * 1024)
#define REPORT_STEP_SIZE REPORT_INITIAL_SIZE

/*Jared: May not be needed - checking with atomics in ib layer first
#define SEND_COMPLETION_TIMEOUT_SECS 30
static struct timeval send_completion_timeout = {SEND_COMPLETION_TIMEOUT_SECS, 0};
static struct timeval next_send_completion_timeout = {INT_MAX, 0};
static int num_send_completion_waiters = 0;
*/
TODO(move SRM to node context);
struct nvmeibt_srm {
	const struct carrier carrier;
	char name[256];
	/* send part */
	struct connection_msg *msgs;

	XDLIST_DECLARE(, struct connection_msg, link) msg_pool_q;
	XDLIST_DECLARE(, struct connection_msg, link) ack_pool_q;
	XDLIST_DECLARE(, struct connection_work, link) work_pool_q;
	XDLIST_DECLARE(, struct connection_work, link) work_q;
	XDLIST_DECLARE(, struct msg_request_wrapper, link) wreq_gc_q;
	pthread_mutex_t guard;
	bool guard_locked;
	pthread_t guard_tid;
	uint32_t send_srm_id;
	uint16_t uid;
	/* register senders */
	int qp_send_count;
	XDLIST_DECLARE(, struct srm_user, link) user_pool_q;
	struct nvmeib_hash_table		*senders_hash_by_ptr_to_user;
	bool allow_send;
	/* receive part */
	uint32_t recv_srm_id;
	char *rcv_buffer;
	int rcv_buffer_len;
	char *rcv_pos;
	int rcv_size;
	uint64_t rcv_id;
	int rcv_counter;
	int dispatch;
	XDLIST_DECLARE(, struct srm_recv_context, link) recv_ctx_q;
	/* register receivers */
	struct nvmeib_hash_table		*receivers_hash_by_ptr_to_user;
	bool allow_receive;
	/* backport compatability stuff */
	XDLIST_DECLARE(, struct msg_request_wrapper, link) req_pool_q;
	/* server lock memory regions */
	XDLIST_DECLARE(, struct lock_region, link) lock_regions;
	int lock_owner;
	struct xdlist resend_link;
	struct xdlist resend_ack_link;

#if defined(SRM_ENABLE_FAULTS)
	uint32_t __fault_stash_frame;
	uint32_t __fault_stash_ack;
	uint32_t __fault_fetch_msg;
#endif
};

static inline void *srm_carrier(struct nvmeibt_srm *srm)
{
	return srm->carrier.carrier_obj(&srm->carrier);
}

bool srm_faults_active = false;

static void rsrm_cancel_srm_timers(struct nvmeibt_srm *srm);
static void do_sender_gc(struct nvmeibt_srm *srm);
static void rsrm_dequeue_send_work(struct nvmeibt_srm *srm);
static void rsrm_queue_send_work(struct nvmeibt_srm *srm);
static void terminate_expired_work(struct nvmeibt_srm *srm, struct connection_work *work, bool use_cb);
static inline bool msg_recv_completed(struct srm_recv_context *recv_ctx);
static struct connection_msg *fetch_msg(struct nvmeibt_srm *srm, struct connection_work *work,
			uint16_t msg_id);

#define ROCE_RESEND_TMOUT_NSEC	MSEC_TO_NSEC(8)	// 8 ms

#ifndef LLVM
static inline bool random_drop(void)
{
#if 1
	return false;
#else
	return drand48() <= 0.0001;
#endif
}
#endif

static int lock(struct nvmeibt_srm *srm)
{
	int rv;

//	FIN_9;
	if ((rv = pthread_mutex_lock(&srm->guard)) != 0) {
		N__E(error_srm_lock, "Failed to lock send/receive manager guard for context @SRM_CARRIER (@RV)",
			srm_carrier(srm), rv);
		TERMINATE(out);
	} else {
		srm->guard_locked = true;
		srm->guard_tid = pthread_self();
	}

out:
//	FOUT_9;
	return rv;
}

static int unlock(struct nvmeibt_srm *srm)
{
	int rv;

//	FIN_9;
	srm->guard_locked = false;
	srm->guard_tid = 0xdeadbeef;
	if ((rv = pthread_mutex_unlock(&srm->guard)) != 0) {
		N__E(error_srm_unlock, "Failed to unlock send/receive manager guard for context @SRM_CARRIER (@RV)",
			srm_carrier(srm), rv);
		TERMINATE(out);
	}

out:
//	FOUT_9;
	return rv;
}

static BOOL __attribute__((unused)) is_already_locked(struct nvmeibt_srm *srm)
{
	pthread_t my_tid = pthread_self();
	return srm->guard_locked && pthread_equal(my_tid, srm->guard_tid);
}

struct nvmeibt_srm *nvmeibt_srm_create(const struct carrier *car)
{
	void *conn = car->conn;
	struct nvmeibt_srm *srm = car->srm_obj(car);
	struct connection_work *work;
	void *msgs_buffer;

	struct srm_recv_context *recv_ctx;

	NFIN;
	if (srm) {
		N__T(trace_srm_nvmeibt_srm_create_0,
			 "conn=@CONN reuse existing srm=@SRM", conn, srm);
		goto out;
	}

	srm = NNVMEIBT_TOMA_CALLOC(trace_srm_nvmeibt_srm_create, 1, sizeof(*srm));

	memcpy((void *)&srm->carrier, car, sizeof(*car));

	if (!(srm->msgs = NNVMEIBT_TOMA_CALLOC(trace_1_srm_nvmeibt_srm_create, srm->carrier.max_messages, sizeof(*srm->msgs)))) {
		N__E(error_srm_nvmeibt_srm_create, "Failed to allocate send/receive manager msgs array "
			"for context @CONN", conn);
		goto free_srm;
	}

	msgs_buffer = car->alloc_msgs_buffer(car);
	if (!msgs_buffer) {
		goto free_msgs;
	}

	do {
		srm->uid = (typeof(srm->uid))random();
	} while (srm->uid == SRM_RESERVED_MSG_ID);

	srm->qp_send_count = 1;

	XDLIST_HEAD_INIT(&srm->ack_pool_q);
	/* init the message queue */
	XDLIST_HEAD_INIT(&srm->msg_pool_q);
	/* fill the queue */
	for (int i = 0; i < srm->carrier.max_messages; ++i) {
		srm->msgs[i].wire.send_ctx_idx = i;
		srm->msgs[i].wire.buffer = msgs_buffer + i * (car->max_chunk_size + car->srm_data_offset);
		srm->msgs[i].wire.len = 0;
		XDLIST_ADD_TAIL(&srm->msg_pool_q, &srm->msgs[i]);
		if (false) {
			N__D(trace_msg_srm_nvmeibt_srm_create, "add msg=@MSG", &srm->msgs[i]);
		}
	}
	/* init the work pool queue */
	XDLIST_HEAD_INIT(&srm->work_pool_q);
	for (int i = 0; i < srm->carrier.max_messages; ++i) {
		work = NNVMEIBT_TOMA_CALLOC(trace_2_srm_nvmeibt_srm_create, 1, sizeof(*work));
		work->srm = srm;
		XDLIST_ADD_TAIL(&srm->work_pool_q, work);
	}
	/* init the work queue */
	XDLIST_HEAD_INIT(&srm->work_q);
	/* init queue guard */
	if (pthread_mutex_init(&srm->guard, NULL) < 0) {
		N__E(error_1_srm_nvmeibt_srm_create, "Failed to create send/receive manager guard for context @CONN",
			conn);
		goto free_works;
	}

	recv_ctx = NNVMEIBT_TOMA_CALLOC(trace_3_srm_nvmeibt_srm_create, 1, sizeof(*recv_ctx));
	lock(srm);
	recv_ctx->ack_msg = fetch_msg(srm, NULL, 0);
	unlock(srm);
	XDLIST_HEAD_INIT(&srm->recv_ctx_q);
	XDLIST_ADD_TAIL(&srm->recv_ctx_q, recv_ctx);

	/* init senders and receivers */
	XDLIST_HEAD_INIT(&srm->user_pool_q);
	srm->senders_hash_by_ptr_to_user = NVMEIB_HASH_CREATE(vysqj29, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "srm->senders", 8, 1);
	srm->receivers_hash_by_ptr_to_user = NVMEIB_HASH_CREATE(fg6wejh, HASH_MIN_LOG2_OF_N_ARR_ENTRIES, "srm->receivers", 8, 1);
	XDLIST_HEAD_INIT(&srm->req_pool_q);
	XDLIST_HEAD_INIT(&srm->lock_regions);
	XDLIST_HEAD_INIT(&srm->wreq_gc_q);

	XDLIST_INIT_LINK(&srm->resend_link, NULL);
	XDLIST_INIT_LINK(&srm->resend_ack_link, NULL);

#if defined(SRM_ENABLE_FAULTS)
	srm->__fault_stash_frame = 0;
	srm->__fault_stash_ack = 0;
	srm->__fault_fetch_msg = 0;
#endif

	snprintf(srm->name, sizeof(srm->name), "%s", car->conn_name(car));
	N__D(trace_7_srm_nvmeibt_srm_create, "Created srm (@SRM) over conn @CONN", srm, conn);
	goto out;

free_works:
	NNVMEIBT_FREE_LIST(trace_4_srm_nvmeibt_srm_create, &srm->work_pool_q);
	XDLIST_HEAD_INIT(&srm->work_pool_q);
	XDLIST_HEAD_INIT(&srm->work_q);
	XDLIST_HEAD_INIT(&srm->msg_pool_q);

free_msgs:
	NNVMEIBT_TOMA_FREE(trace_5_srm_nvmeibt_srm_create, srm->msgs);

free_srm:
	NNVMEIBT_TOMA_FREE(trace_6_srm_nvmeibt_srm_create, srm);

out:
	NFOUT;
	return srm;
}

static void return_req_to_pool_(struct nvmeibt_srm *srm,
	struct nvmeibt_msg_request *req)
{
	struct msg_request_wrapper *reqw = container_of(req, typeof(*reqw), req);
	NTOMA_ASSERT(error_srm_return_req_to_pool, is_already_locked(srm), "srm @SRM is not locked", srm);
	NXDLIST_ADD_TAIL_CHECK(error_1_srm_return_req_to_pool, &srm->req_pool_q, reqw);
}

/*Jared: May not be needed - checking with atomics in ib layer first
static void update_send_completions_on_send_(struct nvmeibt_srm *srm)
{
	struct timeval now;

	FIN;
	TOMA_ASSERT(is_already_locked(srm), "srm %p is not locked\n", srm);
	srm->send_completions_in_the_air++;
	if (srm->send_completions_in_the_air == 1) {
		gettimeofday(&now, NULL);
		timeradd(&send_completion_timeout, &now, &srm->send_completion_timeout);
		//_Tf("Setting send completion timeout on srm %p (conn %p) to %d secs\n",
		    srm, carrier_obj(srm), srm->send_completion_timeout.tv_sec);

		num_send_completion_waiters++;
		if (num_send_completion_waiters == 1) {
			next_send_completion_timeout = srm->send_completion_timeout;

			//_Tf("Updating global send completion timeout to %d secs\n", send_completion_timeout.tv_sec);
		}
	}
}
*/

static void free_req_data(struct nvmeibt_msg_request *req)
{
	NFIN;
	if (req->msg_type == NVMEIBT_IB_PROTOCOL_SIGNATURE_TOMA_RECOVERY) {
		NNVMEIBT_BM_FREE(trace_srm_free_req_data, req->data);
	}
	NFOUT;
}

static void reinit_work(struct nvmeibt_srm *srm, struct connection_work *work)
{
	memset(work, 0, sizeof(*work));
	work->srm = srm;
	XDLIST_INIT_LINK(&work->link, NULL);
	XDLIST_INIT_LINK(&work->tm_link, NULL);
}

static void release_msg(struct nvmeibt_srm *srm, struct connection_msg *msg,
						uint16_t msg_id)
{
	if (true) {
		N__D(trace_srm_release_msg, "msg_id=@MSG_ID_INT release msg=@MSG srm=@SRM send_cnt=@SEND_CNT",
			msg_id, msg, srm, msg->send_cnt);
	}
	XDLIST_ADD_TAIL(&srm->msg_pool_q, msg);
}

static void release_work_msgs(struct nvmeibt_srm *srm,
							  struct connection_work *work)
{
	typeof(work->swnd) *swnd = &work->swnd;
	struct connection_msg *msg;

	for (uint8_t bit = 0; bit < RSRM_SWND_SIZE; bit++) {
		if (swnd->frame[bit].frame_id != NO_FRAME) {
			msg = srm->msgs + swnd->frame[bit].msg_index;
			release_msg(srm, msg, work->msg_id);
			swnd->frame[bit].frame_id = NO_FRAME;
		}
	}
}

static void finish_work_(
	struct nvmeibt_srm *srm, struct connection_work *work)
{
	NFIN;
	NTOMA_ASSERT(error_srm_finish_work, is_already_locked(srm), "srm @SRM is not locked", srm);
	release_work_msgs(srm, work);
	XDLIST_DEL(&work->link);
	XDLIST_DEL(&work->tm_link);
	reinit_work(srm, work);
	XDLIST_ADD_TAIL(&srm->work_pool_q, work);
	rsrm_dequeue_send_work(srm);
	NFOUT;
}

static void call_cb(struct nvmeibt_srm *srm __attribute__((unused)), struct nvmeibt_msg_request *req,
	int status, uint64_t v)
{
	if (req) {
		if (req->cbs.send_c) {
			req->cbs.send_c(req->arg, status);
		}
		else if (req->cbs.lock_c) {
			req->cbs.lock_c(req->arg, status, v);
		}
	}
}

static int
rsrm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req);

int
nvmeibt_srm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req)
{
	return srm ? rsrm_queue_req(srm, req) : -1;
}

void
nvmeibt_srm_cancel_req(struct nvmeibt_srm *srm)
{
	if (srm)
		srm_terminate_all_works(srm);
}

static void
rsrm_send_completion(struct nvmeibt_srm *srm, struct connection_msg *msg);
void nvmeibt_srm_send_completion(struct nvmeibt_srm *srm, uint16_t send_ctx_idx)
{
	struct connection_msg *msg;
	if (send_ctx_idx >= srm->carrier.max_messages) {
		N_Ef(error_srm_nvmeibt_srm_send_completion, "Invalid srm @SRM send completion index @SEND_CTX_IDX", srm, send_ctx_idx);
		return;
	}
	msg = &srm->msgs[send_ctx_idx];
	rsrm_send_completion(srm, msg);
}

static void clear_receive(struct nvmeibt_srm *srm)
{
	NFIN;
	srm->rcv_buffer = NULL;
	srm->rcv_buffer_len = 0;
	srm->rcv_id = 0;
	srm->rcv_counter = 0;
	srm->rcv_pos = NULL;
	srm->rcv_size = 0;
	srm->dispatch = 0;
	NFOUT;
}

static int allocate_rcv_buffer(struct nvmeibt_srm *srm, int len) {
	srm->rcv_buffer = (char *)NNVMEIBT_BM_CALLOC(trace_srm_allocate_rcv_buffer, len);
	srm->rcv_buffer_len = len;
	return srm->rcv_buffer ? 0 : -1;
}

static int rsrm_receive_completion(struct nvmeibt_srm *srm, void *buffer);
int nvmeibt_srm_receive_completion(struct nvmeibt_srm *srm, void *buffer)
{
	return rsrm_receive_completion(srm, buffer);
}

static int register_receive_user(struct nvmeibt_srm *srm,
	struct nvmeibt_srm_user *user, bool is_sender)
{
	struct srm_user *new_user;
	int rv = 0;

	NFIN;
	lock(srm);

	if ((is_sender && !srm->allow_send) ||
		(!is_sender && !srm->allow_receive)) {
		N_Ef(error_srm_register_receive_user, "is_sender=@IS_SENDER allow_send=@ALLOW_SEND allow_receive=@ALLOW_RECEIVE",
			is_sender, srm->allow_send, srm->allow_receive);
		rv = -1;
		goto unlock_l;
	}

	new_user = NNVMEIBT_ALLOC_ELEM(trace_srm_register_receive_user, &srm->user_pool_q, srm_carrier(srm));
	new_user->user = *user;
	if (is_sender) {
		nvmeib_hash_add_uint64_t(srm->senders_hash_by_ptr_to_user, (uint64_t)new_user->user.user, new_user);
	}
	else {
		nvmeib_hash_add_uint64_t(srm->receivers_hash_by_ptr_to_user, (uint64_t)new_user->user.user, new_user);
	}

unlock_l:
	unlock(srm);
	NFOUT;
	return rv;
}

int nvmeibt_srm_register_send_user(struct nvmeibt_srm *srm,
	struct nvmeibt_srm_user *user)
{
	int rv;
	NFIN;
	rv = register_receive_user(srm, user, true);
	NFOUT;
	return rv;
}

int nvmeibt_srm_register_receive_user(struct nvmeibt_srm *srm,
	struct nvmeibt_srm_user *user)
{
	int rv;
	NFIN;
	rv = register_receive_user(srm, user, false);
	NFOUT;
	return rv;
}

int nvmeibt_srm_allow_send(struct nvmeibt_srm *srm, uint32_t send_srm_id)
{
	NFIN;
	lock(srm);
	if (!srm->allow_send) {
		srm->allow_send = true;
		srm->send_srm_id = send_srm_id;
	}
	unlock(srm);
	NFOUT;
	return 0;
}

int nvmeibt_srm_stop_sender(struct nvmeibt_srm *srm, void *user)
{
	struct srm_user *p;
	struct connection_work *work;
	void (*user_stop_c)(void *arg, int reason) = NULL;
	void *arg = NULL;

	NFIN;
	lock(srm);
	if (srm->allow_send) {
		/* clear from sender hash */
		p = nvmeib_hash_search_uint64_t(srm->senders_hash_by_ptr_to_user, (uint64_t)user);
		if (p) {
			if (p->user.cbs.stop_c) {
				user_stop_c = p->user.cbs.stop_c;
				arg = p->user.user;
			}
			nvmeib_hash_delete_uint64_t(srm->senders_hash_by_ptr_to_user, (uint64_t)user);
			XDLIST_ADD_TAIL(&srm->user_pool_q, p);
		}

		XDLIST_FOREACH(work, &srm->work_q) {
			if (work->user == user) {
				work->canceled = true;
			}
		}
	}
	unlock(srm);

	/* call user's stop cb not under lock */
	if (user_stop_c) {
		user_stop_c(arg, u_stop_all);
	}

	NFOUT;
	return 0;
}

static void stop_all_senders_no_lock(struct nvmeibt_srm *srm)
{
	struct srm_user *p;
	struct connection_work *work;

	NFIN;
	XDLIST_FOREACH_SAFE(work, &srm->work_q) {
		work->canceled = true;
		if (work->req && work->req->cbs.cancel_c) {
			work->req->cbs.cancel_c(work->req->arg, work->req);
		}
		else {
			free_req_data(work->req);
		}
	}
	/* clear from sender hash */
	NVMEIB_HASH_FOREACH(p, srm->senders_hash_by_ptr_to_user) {
		if (p->user.cbs.stop_c) {
			p->user.cbs.stop_c(p->user.user, u_stop_all);
		}
		nvmeib_hash_delete_uint64_t(srm->senders_hash_by_ptr_to_user, (int64_t)p->user.user);
		XDLIST_ADD_TAIL(&srm->user_pool_q, p);
	}
	NFOUT;
}

int nvmeibt_srm_allow_receive(struct nvmeibt_srm *srm, uint32_t recv_srm_id)
{
	struct srm_recv_context *recv_ctx;

	NFIN;
	lock(srm);
	if (!srm->allow_receive) {
		srm->allow_receive = true;
		srm->recv_srm_id = recv_srm_id;
		XDLIST_FOREACH_SAFE(recv_ctx, &srm->recv_ctx_q) {
			recv_ctx->rwnd.msg_id = SRM_RESERVED_MSG_ID;
			recv_ctx->rwnd.poll_frames_received = 0;
			NNVMEIBT_BM_FREE(trace_srm_nvmeibt_srm_allow_receive, recv_ctx->rcv_buffer);
			recv_ctx->rcv_pos = NULL;
		}
	}
	unlock(srm);
	NFOUT;
	return 0;
}

int nvmeibt_srm_stop_receiver(struct nvmeibt_srm *srm, void *user)
{
	struct srm_user *p;
	void (*user_stop_c)(void *arg, int reason) = NULL;
	void *arg = NULL;

	NFIN;
	lock(srm);
	if (srm->allow_receive) {
		/* clear from sender hash */
		p = nvmeib_hash_search_uint64_t(srm->receivers_hash_by_ptr_to_user, (uint64_t)user);
		if (p) {
			if (p->user.cbs.stop_c) {
				user_stop_c = p->user.cbs.stop_c;
				arg = p->user.user;
			}
			nvmeib_hash_delete_uint64_t(srm->receivers_hash_by_ptr_to_user, (uint64_t)user);
			XDLIST_ADD_TAIL(&srm->user_pool_q, p);
		}
	}
	unlock(srm);

	/* call user's stop cb not under lock */
	if (user_stop_c) {
		user_stop_c(arg, u_stop_all);
	}

	NFOUT;
	return 0;
}

static void stop_all_receivers_nolock(struct nvmeibt_srm *srm)
{
	struct srm_user *p;

	NFIN;
	/* clear from sender hash */
	NVMEIB_HASH_FOREACH(p, srm->receivers_hash_by_ptr_to_user) {
		if (p->user.cbs.stop_c) {
			p->user.cbs.stop_c(p->user.user, u_stop_all);
		}
		nvmeib_hash_delete_uint64_t(srm->receivers_hash_by_ptr_to_user, (uint64_t)p->user.user);
		XDLIST_ADD_TAIL(&srm->user_pool_q, p);
	}
	clear_receive(srm);
	NFOUT;
}

int nvmeibt_srm_stop_all_receivers(struct nvmeibt_srm *srm)
{
	int ok_r = 0;

	NFIN;
	lock(srm);
	if (srm->allow_receive) {
		srm->allow_receive = false;
		ok_r = 1;
	}
	unlock(srm);

	if (ok_r) {
		stop_all_receivers_nolock(srm);
	}
	NFOUT;
	return 0;
}

int nvmeibt_srm_stop_all(struct nvmeibt_srm *srm)
{
	int ok_r = 0;
	int ok_s = 0;

	NFIN;
	lock(srm);
	XDLIST_DEL(&srm->resend_ack_link);
	rsrm_cancel_srm_timers(srm);
	do_sender_gc(srm);
	if (srm->allow_receive) {
		srm->allow_receive = false;
		ok_r = 1;
	}
	if (srm->allow_send) {
		srm->allow_send = false;
		ok_s = 1;
	}
	unlock(srm);
	if (ok_r) {
		stop_all_receivers_nolock(srm);
	}
	if (ok_s) {
		stop_all_senders_no_lock(srm);
	}

	NFOUT;
	return 0;
}

void nvmeibt_srm_free(struct nvmeibt_srm *srm)
{
	struct srm_recv_context *recv_ctx;

	//NFIN;
	srm_terminate_all_works(srm);
	nvmeibt_srm_stop_all(srm);
	NNVMEIBT_FREE_LIST(trace_1_srm_free, &srm->work_pool_q);
	NNVMEIBT_FREE_LIST(trace_11_srm_free, &srm->req_pool_q);
	NNVMEIBT_FREE_LIST(trace_2_srm_free, &srm->user_pool_q);
	XDLIST_FOREACH_SAFE(recv_ctx, &srm->recv_ctx_q) {
		if (recv_ctx) {
			NNVMEIBT_BM_FREE(trace_3_srm_free, recv_ctx->rcv_buffer);
			recv_ctx->rcv_pos = NULL;
		}
	}
	NNVMEIBT_FREE_LIST(trace_4_srm_free, &srm->recv_ctx_q);
	NNVMEIBT_TOMA_FREE(trace_5_srm_free, srm->msgs);
	srm->carrier.release_msgs_buffer(&srm->carrier);
	NNVMEIBT_TOMA_FREE(trace_6_srm_free, srm);
	//NFOUT;
}

void nvmeibt_srm_set_lock_owner(struct nvmeibt_srm *srm, int f)
{
	NFIN;
	srm->lock_owner = f;
	NFOUT;
}

/**
 * =============================================
 * =============================================
 * ============= Reliable SRM
 * =============================================
 * =============================================
 */

/**
 * with 1ms tick __cycle value wraps after 65.5 seconds
 */
static uint16_t __cycle = 0;

static inline uint16_t current_cycle(void)
{
	return __cycle;
}

static inline bool bit_set(wnd_mask_t bitmap, uint8_t bit)
{
	return (bitmap & (1UL << bit));
}

static inline void init_send_window(struct rsrm_send_window *swnd)
{
	memset(swnd, 0, sizeof(*swnd));
	swnd->wnd_size = RSRM_SWND_SIZE;
	for (unsigned i = 0; i < RSRM_SWND_SIZE; i++) {
		swnd->frame[i].frame_id = NO_FRAME;
	}
}

static inline bool can_add_send_frame(struct rsrm_send_window *swnd, uint16_t frame)
{
	int bit = frame - swnd->head_frame_id;
	int pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;
	bool verdict =
		frame >= swnd->head_frame_id 			&&
		frame < swnd->head_frame_id + RSRM_SWND_SIZE 	&&
		!(swnd->send_mask & (1UL << bit)) 		&&
		!(swnd->ack_mask & (1UL << bit)) 			&&
		swnd->frame[pos].frame_id == NO_FRAME		&&
		!swnd->frame[pos].send_count;
	N_Df(trace_srm_can_add_send_frame, "swnd=@SWND:@ACK_MASK:@HEAD_FRAME_ID frame=@FRAME pos=@INT bit=@BIT",
		swnd->send_mask, swnd->ack_mask,
		swnd->head_frame_id, frame, pos, bit);
	return verdict;
}

static void send_window_add_frame(struct connection_work *work,
					struct connection_msg *msg,
					uint16_t frame_id)
{
	typeof(work->srm) __attribute__ ((unused)) srm = work->srm;
	struct rsrm_send_window *swnd = &work->swnd;
	int bit = frame_id - swnd->head_frame_id;
	int pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;

	msg->send_cnt++;
	swnd->wnd_size--;
	swnd->send_mask |= (1UL << bit);
	swnd->frame[pos].frame_id = frame_id;
	swnd->frame[pos].send_count = 1;
	swnd->frame[pos].msg_index = msg->wire.send_ctx_idx;
	swnd->frame[pos].send_cycle = current_cycle();
	if (false) {
		N__D(trace_srm_send_window_add_frame, "msg_id=@MSG_ID_INT frame=@FRAME pos=@INT msg_index=@MSG_INDEX",
			work->msg_id, frame_id, pos, msg->wire.send_ctx_idx);
	}
}

static int slide_recv_window(struct nvmeibt_srm *srm, struct srm_recv_context *recv_ctx)
{
	struct rsrm_recv_window *rwnd = &recv_ctx->rwnd;
	int slide = rwnd->recv_mask == RSRM_SWND_BITFLD_MASK ?
		(int)RSRM_SWND_SIZE : __builtin_ctzl(~rwnd->recv_mask);
	if (slide) {
		wnd_mask_t rm = rwnd->recv_mask;
		uint16_t h = rwnd->head_frame_id;
		rwnd->head_frame_id += slide;
		if (slide < RSRM_SWND_SIZE) {
			rwnd->recv_mask >>= slide;
		} else {
			rwnd->recv_mask = 0;
		}
		if (false) {
			//Trace args:[srm_name,msg_id,window,slide,recv_mask,head_frame_id,recv_mask,head_frame_id]
			N__D(trace_srm_slide_recv_window, "msg_id=@MSG_ID_INT RECV window=@WINDOW slide=@SLIDE prev=@RECV_MASK:@HEAD_FRAME_ID now=@RECV_MASK:@HEAD_FRAME_ID",
				rwnd->msg_id, rwnd->ack_id, slide,
				rm, h, rwnd->recv_mask, rwnd->head_frame_id);
		}
	}
	return slide;
}

static int slide_send_window(struct connection_work *work, struct rsrm_send_window *swnd)
{
	int slide = 0;
	struct nvmeibt_srm __attribute__ ((unused)) *srm = work->srm;

	wnd_mask_t sm = swnd->send_mask;
	wnd_mask_t am = swnd->ack_mask;
	wnd_mask_t nm = swnd->nack_mask;
	uint16_t h = swnd->head_frame_id;
	uint8_t hp = swnd->head_pos;

	if (!swnd->ack_mask) goto out;

	NFIN;

	slide = (swnd->ack_mask == RSRM_SWND_BITFLD_MASK) ?
		(int)RSRM_SWND_SIZE : __builtin_ctzl(~swnd->ack_mask);

	if (slide) {
		for (int i = 0; i < slide; i++) {
			uint8_t pos = (swnd->head_pos + i) & RSRM_SWND_POS_MASK;
			swnd->frame[pos].frame_id = NO_FRAME;
			swnd->frame[pos].msg_index = NO_FRAME;
			swnd->frame[pos].send_count = 0;
		}

		swnd->wnd_size = slide;
		if (slide < RSRM_SWND_SIZE) {
			swnd->send_mask >>= slide;
			swnd->ack_mask  >>= slide;
			swnd->nack_mask >>= slide;
		} else {
			swnd->send_mask = 0;
			swnd->ack_mask  = 0;
			swnd->nack_mask = 0;
		}
		swnd->head_frame_id += slide;
		swnd->head_pos = (swnd->head_pos + slide) & RSRM_SWND_POS_MASK;
		if (false) {
			//Trace args:[srm_name,msg_id,slide,send_mask,ack_mask,nack_mask,head_frame_id,head_pos,send_mask,ack_mask,nack_mask,head_frame_id,head_pos]
			N__D(trace_srm_slide_send_window, "msg_id=@MSG_ID_INT swnd slide=@SLIDE "
				"@SEND_MASK:@ACK_MASK:@NACK_MASK @HEAD_FRAME_ID:@HEAD_POS "
				"@SEND_MASK:@ACK_MASK:@NACK_MASK @HEAD_FRAME_ID:@HEAD_POS",
				work->msg_id, slide, sm,am,nm, h, hp,
				swnd->send_mask, swnd->ack_mask, swnd->nack_mask,
				swnd->head_frame_id, swnd->head_pos);
		}
	}

	NFOUT;
out:
	return slide;
}

static inline int
msg_frames_num(struct connection_work *work)
{
	typeof(work->srm) __attribute__ ((unused)) srm = work->srm;
	int frame_size = work->srm->carrier.max_chunk_size - sizeof(struct rsrm_frame_header);
	int _frames_num =
		msg_transfer_len(work->req) ?
		(msg_transfer_len(work->req) + frame_size - 1) / frame_size : 1;
	N__D(trace_srm_msg_frames_num, "msg_id=@MSG_ID_INT type=@TYPE_STR transfer=@TRANSFER frame_size=@FRAME_SIZE frames_num=@FRAMES_NUM",
		work->msg_id, nvmeibt_ib_protocol_signature_to_str(work->req->msg_type),
		msg_transfer_len(work->req),
		frame_size, _frames_num);
	return _frames_num;
}

static inline bool
srm_basic_req_validation(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req)
{
	bool verdict = false;
	if (!srm->allow_send) verdict = false;
	else {
		verdict = msg_transfer_len(req) > 0		&&
				req->msg_len <= MAX_MSG_LEN	&&
				(req->msg_len +
					sizeof(struct rsrm_frame_header)) <=
					(uint32_t)srm->carrier.max_chunk_size;
	}
	return verdict;
}

static struct connection_work *
rsrm_init_new_work(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req)
{
	static struct connection_work *work;
	struct msg_request_wrapper *reqw;
	int total_len, data_len;

	reqw = NNVMEIBT_ALLOC_ELEM(trace_srm_rsrm_init_new_work, &srm->req_pool_q, srm_carrier(srm));
	reqw->req = *req;
	XDLIST_INIT_LINK(&reqw->link, NULL);
	XDLIST_INIT_LINK(&reqw->cmpl_link, NULL);

	/* BIG DATA DEBUG */
	total_len = msg_transfer_len(req) +  EXT_DATA_CHUNK;
	data_len = total_len - req->msg_len;

	reqw->req.data_len = data_len;

	N__D(trace_d_srm_rsrm_init_new_work, "orig transfer=@TRANSFER dbg transfer=@TRANSFER",
		msg_transfer_len(req), msg_transfer_len(&reqw->req));

	work = NNVMEIBT_ALLOC_ELEM(trace_1_srm_rsrm_init_new_work, &srm->work_pool_q, srm_carrier(srm));
	work->srm = srm;
	do {
		work->msg_id = ++srm->uid;
	} while (work->msg_id == SRM_RESERVED_MSG_ID);
	work->req = &reqw->req;
	work->user = req->user;
	work->msg_count = 0;
	work->ack_id = 0;

	work->frames_num = msg_frames_num(work);
	work->frame_to_send = 0;
	work->frame_data_offset = 0;
	init_send_window(&work->swnd);

	reqw->req.can_back_to_pool = 1;
	reqw->req.msg = reqw->msg_inline;
	memcpy(reqw->req.msg, req->msg, req->msg_len);

	XDLIST_INIT_LINK(&work->tm_link, NULL);
	XDLIST_ADD_TAIL(&srm->work_q, work);
	NFOUT;
	return work;
}

static struct connection_msg *
fetch_msg(struct nvmeibt_srm *srm, struct connection_work *work,
			uint16_t msg_id)
{
	struct connection_msg *msg = XDLIST_FIRST(&srm->msg_pool_q);
	// FIN;
#if defined(SRM_ENABLE_FAULTS)
	if (srm_faults_active && !(++srm->__fault_fetch_msg % 101)) {
		N__I(trace_srm_fetch_msg_a, "FAULT:msg_id=@MSG_ID_INT simulate missing msg", msg_id);
		msg = NULL;
	}
#endif
	NTOMA_ASSERT(error_srm_fetch_msg, is_already_locked(srm), "srm @SRM is not locked", srm);
	if (msg) {
		XDLIST_DEL(&msg->link);
		msg->wire.len = 0;
		msg->work = work;
		msg->next = NULL;
		msg->send_cnt = 0;
		if (true) {
			N__D(trace_srm_fetch_msg, "msg_id=@MSG_ID_INT fetched msg=@MSG next=@NEXT srm=@SRM",
				msg_id, msg, XDLIST_FIRST(&srm->msg_pool_q), srm);
		}
	} else {
		/* Send window model cannot run out of messages */
		N__E(error_1_srm_fetch_msg, "msg_id=@MSG_ID_INT no free message srm=@SRM", msg_id, srm);
	}

	if (work) {
		work->msg_count++;
	}

	// FOUT;
	return msg;
}

static int
build_frame(struct nvmeibt_srm *srm,
		struct connection_work *work, struct connection_msg *msg)
{
	struct rsrm_frame_header *h = srm_data_buffer(&msg->wire, &srm->carrier);
	void *dst = h + 1;
	void *src = work->req->data + work->frame_data_offset;
	int frame_space_used = sizeof(*h);
	int frame_space_left = srm->carrier.max_chunk_size - frame_space_used, frame_data_size;
	int data_left = work->req->data_len - work->frame_data_offset;

	NTOMA_ASSERT(error_srm_build_frame, (uint64_t)h == (uint64_t)srm_data_buffer(&msg->wire, &srm->carrier),  "ptr verification 1");
	NTOMA_ASSERT(error_1_srm_build_frame, (uint64_t)dst - (uint64_t)h == sizeof(*h), "ptr verification 2");

	if (work->msg_id == SRM_RESERVED_MSG_ID) {
		N__E(error_2_srm_build_frame, "msg_id=@MSG_ID_INT is invalid", SRM_RESERVED_MSG_ID);
		CRITICAL_BUG();
	}

	h->srm_id = nvmeib_htonl(srm->send_srm_id);
	h->msg_id = nvmeib_htons((uint16_t)work->msg_id);
	h->msg_length = nvmeib_htonl(msg_transfer_len(work->req));
	h->msg_type = nvmeib_htonl(work->req->msg_type);
	h->frame_id = nvmeib_htons((uint16_t)(work->frame_to_send));

	if (!work->frame_to_send) {
		memcpy(dst, work->req->msg, work->req->msg_len); // :)
		frame_space_used += work->req->msg_len;
		frame_space_left -= work->req->msg_len;
		dst += work->req->msg_len;
		h->data_offset = nvmeib_htonl(0);
	} else {
		h->data_offset = nvmeib_htonl(work->frame_data_offset + work->req->msg_len);
	}

	frame_data_size = min(frame_space_left, data_left);
	memcpy(dst, src, frame_data_size);
	frame_space_used += frame_data_size;
	h->frame_len = nvmeib_htons((uint16_t)(frame_space_used - sizeof(*h)));
	msg->wire.len = frame_space_used;
	if (false) {
		N__D(trace_srm_build_frame, "msg_id=@MSG_ID_INT frame=@FRAME "
			"frame_space_used=@FRAME_SPACE_USED frame_data_size=@FRAME_DATA_SIZE "
			"data offset=@INT",
			work->msg_id, work->frame_to_send,
			frame_space_used, frame_data_size,
			work->frame_data_offset);
	}
	return frame_data_size;
}


static inline bool can_submit_frame(struct connection_work *work)
{
	struct nvmeibt_srm __attribute__ ((unused)) *srm = work->srm;
	if (false) {
		N__D(trace_srm_can_submit_frame, "msg_id=@MSG_ID_INT next_frame=@NEXT_FRAME/@TOTAL_NUM swnd size=@SIZE", work->msg_id,
		work->frame_to_send,work->frames_num, work->swnd.wnd_size); }
	return (work->frame_to_send < work->frames_num) && work->swnd.wnd_size;
}

static inline bool should_send_signal(struct nvmeibt_srm *srm)
{
	return !(srm->qp_send_count % RSRM_QP_MSG_COUNT);
}

static int
transfer_raft_msg(struct connection_work *work)
{
	int rv = 0, frame_data_size;
	struct connection_msg *msg;
	struct nvmeibt_srm *srm = work->srm;
	struct rsrm_send_window *swnd = &work->swnd;
	bool signal_send_cmpl = should_send_signal(srm);

	NFIN;
	NTOMA_ASSERT(error_srm_transfer_raft_msg, is_already_locked(srm), "@SRM_NAME msg_id=@MSG_ID_INT srm is not locked",
			srm->name, work->msg_id);

	while (can_submit_frame(work)) {
		if (!can_add_send_frame(&work->swnd, work->frame_to_send)) {
			int bit = work->frame_to_send - swnd->head_frame_id;
			int pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;
			N__E(error_1_srm_transfer_raft_msg,
				 "msg_id=@MSG_ID_INT frame=@FRAME swnd verification failed "
				 "swnd=@SWND:@ACK_MASK:@HEAD_FRAME_ID "
				 "frame id=@FRAME_ID send count=@COUNT ",
				 work->msg_id, work->frame_to_send,
				 swnd->send_mask, swnd->ack_mask,
				 swnd->head_frame_id,
				 swnd->frame[pos].frame_id,
				 swnd->frame[pos].send_count);

			rv = -1;
			goto out;
		}
		msg = fetch_msg(srm, work, work->msg_id);
		if (!msg) {
			rv = -1;
			goto out;
		}
		frame_data_size = build_frame(srm, work, msg);
		rv = srm->carrier.send_msg(&srm->carrier, &msg->wire,
			work->msg_id, signal_send_cmpl);

		if (rv == 0) {
			send_window_add_frame(work, msg, work->frame_to_send);
			++work->frame_to_send;
			++srm->qp_send_count;
			signal_send_cmpl = should_send_signal(srm);
			work->frame_data_offset += frame_data_size;
		} else {
			N__W(warn_srm_transfer_raft_msg,
				 "msg_id=@MSG_ID_INT failed to post frame=@FRAME err=@ERR",
				 work->msg_id, work->frame_to_send, rv);
			release_msg(srm, msg, work->msg_id);
			goto out;
		}
	}
out:
	if (rv == 0) {
		N__D(trace_srm_transfer_raft_msg, "msg_id=@MSG_ID_INT "
			"swnd:={head=@HEAD send_mask=@SEND_MASK ack_mask=@ACK_MASK wnd_size=@WND_SIZE}",
			work->msg_id, swnd->head_frame_id, swnd->send_mask,
			swnd->ack_mask, swnd->wnd_size);
	}
	NFOUT;
	return rv;
}

static int process_nack(struct connection_work *work)
{
	typeof(work->srm) srm = work->srm;
	typeof(&work->swnd) swnd = &work->swnd;
	bool signal_send_cmpl = should_send_signal(srm);
	int rv = 0;

	NFIN;
	for (uint8_t bit = 0; bit < RSRM_SWND_SIZE; bit++) {
		int pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;
		typeof(&swnd->frame[0]) fr_info = swnd->frame + pos;
		struct connection_msg *msg;

		if (!bit_set(swnd->nack_mask, bit)) continue;

		if (fr_info->frame_id != swnd->head_frame_id + bit) {
			N__E(error_srm_process_nack, "msg_id=@MSG_ID_INT no match info_frame=@INFO_FRAME wind_frame=@WIND_FRAME",
					work->msg_id, fr_info->frame_id, swnd->head_frame_id + bit);
			CRITICAL_BUG();
		}

		N__D(trace_srm_process_nack, "msg_id=@MSG_ID_INT nack resend frame=@FRAME send cnt=@CNT",
				work->msg_id, fr_info->frame_id,
				fr_info->send_count);

		msg = srm->msgs + fr_info->msg_index;
		rv = srm->carrier.send_msg(&srm->carrier, &msg->wire,
								   work->msg_id, signal_send_cmpl);
		if (rv == 0) {
			srm->qp_send_count++;
			signal_send_cmpl = should_send_signal(srm);
			msg->send_cnt++;
			fr_info->send_count++;
			swnd->nack_mask &= ~(1UL << bit);
		} else {
			work->error = true;
			break;
		}
	}

	if (swnd->nack_mask) {
		N_Wf(error_1_srm_process_nack, "msg_id=@MSG_ID_INT nack mask not empty @NACK_MASK",
			work->msg_id, swnd->nack_mask);
	}
	NFOUT;
	return rv;
}

static int rsrm_resend_window(struct connection_work *work)
{
	typeof(work->srm) srm = work->srm;
	typeof(&work->swnd) swnd = &work->swnd;
	bool signal_send_cmpl = should_send_signal(srm);
	int rv = 0;

	NFIN;
	for (uint8_t bit = 0; bit < RSRM_SWND_SIZE; bit++) {
		int pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;
		typeof(&swnd->frame[0]) fr_info = swnd->frame + pos;
		struct connection_msg *msg;

		if (!bit_set(swnd->send_mask, bit)) continue;
		if ( bit_set(swnd->ack_mask,  bit)) continue;

		N__D(trace_srm_rsrm_resend_window, "msg_id=@MSG_ID_INT resend frame=@FRAME send cnt=@CNT",
			work->msg_id, fr_info->frame_id, fr_info->send_count);

		msg = srm->msgs + fr_info->msg_index;
		rv = srm->carrier.send_msg(&srm->carrier, &msg->wire,
										work->msg_id, signal_send_cmpl);
		if (rv == 0) {
			srm->qp_send_count++;
			signal_send_cmpl = should_send_signal(srm);
			msg->send_cnt++;
			fr_info->send_count++;
			getnstimeofday_boot(&work->window_time);
		} else {
			work->error = true;
			terminate_expired_work(srm, work, 1);
			break;
		}
	}
	NFOUT;
	return rv;
}

static void gc_add_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req)
{
	struct msg_request_wrapper *wreq = container_of(req, typeof(*wreq), req);
	NTOMA_ASSERT(error_srm_gc_add_req, is_already_locked(srm), "srm @SRM is not locked", srm);
	XDLIST_ADD_TAIL(&srm->wreq_gc_q, wreq);
}

/**
 * Prefer to run GC tasks in RAFT sender
 * This is sync context that does not waiste reactor (select) CPU clocks
 */
static void do_sender_gc(struct nvmeibt_srm *srm)
{
	struct msg_request_wrapper *wreq;

	NTOMA_ASSERT(error_srm_do_sender_gc, is_already_locked(srm), "srm @SRM is not locked", srm);

	XDLIST_FOREACH_SAFE(wreq, &srm->wreq_gc_q) {
		XDLIST_DEL(&wreq->link);
		XDLIST_ADD_TAIL(&srm->req_pool_q, wreq);
	}
}

static int
rsrm_queue_req(struct nvmeibt_srm *srm, struct nvmeibt_msg_request *req)
{
	int rv = -1;
	struct connection_work *work;
	NFIN;

	// receiver side handles a single job
	srm_terminate_all_works(srm);

	if (!srm_basic_req_validation(srm, req)) {
		N__D(trace_srm_rsrm_queue_req, "=== Basic Validation Failure =====");
		goto out;
	}

	lock(srm);

	do_sender_gc(srm);

	work = rsrm_init_new_work(srm, req);
	if (!work) goto out;
	rsrm_queue_send_work(work->srm);

	rv = transfer_raft_msg(work);

	if (rv) {
		work->error = true;
		return_req_to_pool_(srm, work->req);
		finish_work_(srm, work);
	} else {
		getnstimeofday_boot(&work->window_time);
		work->work_time = work->window_time;
	}

	unlock(srm);

#if 0
	if (rv < 0) {
		call_cb(srm, req, src_send_error, 0);
	}
#endif
out:
	NFOUT;
	return rv;
}

static inline bool work_completed(struct connection_work *work)
{
	bool verdict = false;

	if (work->error || work->canceled) {
		verdict = true;
	} else {
		verdict = work->frames_num == work->swnd.acked_frames;
	}
	return verdict;
}

static void
rsrm_send_completion(struct nvmeibt_srm *srm, struct connection_msg *msg)
{
	struct rsrm_frame_header *h = srm_data_buffer(&msg->wire, &srm->carrier);
	uint16_t msg_id = nvmeib_ntohs(h->msg_id);
	struct connection_msg *m;

	NFIN;
	lock(srm);

	N__D(trace_srm_rsrm_send_completion, "acks to release @SIZE", XDLIST_N_ELEMNTS(&srm->ack_pool_q));
	XDLIST_FOREACH_SAFE(m, &srm->ack_pool_q) {
		h = srm_data_buffer(&m->wire, &srm->carrier);
		msg_id = nvmeib_ntohs(h->msg_id);
		XDLIST_DEL(&m->link);
		release_msg(srm, m, msg_id);
	}

	unlock(srm);
	NFOUT;
}

static int init_recv_context(struct nvmeibt_srm *srm,
								struct srm_recv_context *recv_ctx,
								uint16_t msg_id, uint32_t msg_len,
								uint32_t msg_type)
{
	int rv = 0;
	struct rsrm_recv_window *rwnd;
	struct nvmeibt_big_msg *bmsg;
	uint32_t allocated_len = msg_len + sizeof(*bmsg);
	uint32_t ack_id = (uint32_t)nvmeib_public_rdtsc();

	NTOMA_ASSERT(error_srm_init_recv_context, is_already_locked(srm), "srm @SRM is not locked", srm);
	rwnd = &recv_ctx->rwnd;
	rwnd->msg_id = msg_id;
	rwnd->head_frame_id = 0;
	rwnd->ack_id = nvmeib_htonl(ack_id);
	rwnd->recv_mask = 0;
	rwnd->acks_to_send = 0;
	rwnd->msg_frames_received = 0;
	rwnd->poll_frames_received = 0;
	recv_ctx->rcv_size = 0;

	rv = allocate_rcv_buffer(srm, allocated_len);
	if (rv < 0) {
		N__E(error_1_srm_init_recv_context, "msg_id=@MSG_ID_INT cannot allocate recv buffer len=@LEN", msg_id, allocated_len);
		goto out;
	}
	recv_ctx->rcv_buffer = srm->rcv_buffer;
	recv_ctx->rcv_buffer_len = srm->rcv_buffer_len - sizeof(*bmsg);
	getnstimeofday_boot(&recv_ctx->start_time);

	bmsg = recv_ctx->rcv_buffer;
	bmsg->big_msg_id = msg_id;
	bmsg->msg_type = msg_type;
	bmsg->data_len = msg_len - EXT_DATA_CHUNK;

	recv_ctx->rcv_pos = bmsg + 1;
out:
	return rv;
}

static struct srm_recv_context *
fetch_recv_context(struct nvmeibt_srm *srm, uint16_t msg_id,
					uint32_t msg_len, uint32_t msg_type)
{
	struct srm_recv_context *recv_ctx;
	NTOMA_ASSERT(error_srm_fetch_recv_context, is_already_locked(srm), "srm @SRM is not locked", srm);
	XDLIST_FOREACH_SAFE(recv_ctx, &srm->recv_ctx_q) {
		typeof (msg_id) prev_msg_id, prev_prev_msg_id;

		// deal with msg_id wrap around
		prev_msg_id = recv_ctx->rwnd.msg_id - 1;
		if (prev_msg_id == SRM_RESERVED_MSG_ID) prev_msg_id--;
		prev_prev_msg_id = prev_msg_id--;
		if (prev_prev_msg_id == SRM_RESERVED_MSG_ID) prev_prev_msg_id--;

		if (msg_id == recv_ctx->rwnd.msg_id) {
			if (false) {
				N__D(trace_3_srm_fetch_recv_context, "msg_id=@MSG_ID_INT contunue with active recv_ctx", msg_id);
			}
			return recv_ctx;
		} else if ((msg_id == prev_msg_id) || (msg_id == prev_prev_msg_id)) {
			N__T(trace_2_srm_fetch_recv_context, "incoming msg_id=@MSG_ID_INT is too old have=@HAVE",
				 msg_id, recv_ctx->rwnd.msg_id);
			return NULL;
		} else {
			int rv;
			long diff_ns;
			struct timespec now;
			getnstimeofday_boot(&now);
			diff_ns = timespec_diff_ns(now, recv_ctx->start_time);
			if (false) {
				 N__D(trace_1_srm_fetch_recv_context, "incoming msg_id=@MSG_ID_INT claims recv_ctx after @DIFF nsec existing msg_id=@MSG_ID_INT dispatched=@DISPATCHED",
				msg_id, diff_ns, recv_ctx->rwnd.msg_id, recv_ctx->rcv_buffer == NULL);
			}
			NNVMEIBT_BM_FREE(trace_srm_fetch_recv_context, recv_ctx->rcv_buffer);
			XDLIST_DEL(&srm->resend_ack_link);
			rv = init_recv_context(srm, recv_ctx, msg_id, msg_len, msg_type);
			return !rv ? recv_ctx : NULL;
		}
	}
	return NULL;
}

void srm_post_send_cmpl(struct nvmeibt_srm *srm)
{
	struct connection_work *work;
	struct msg_request_wrapper *reqw;
	XDLIST_DECLARE(, struct msg_request_wrapper, cmpl_link) completed_req_q;
	XDLIST_HEAD_INIT(&completed_req_q);

	lock(srm);
	XDLIST_FOREACH_SAFE(work, &srm->work_q) {
		bool canceled = work->canceled;
		bool error = work->error;
		int slide;

		reqw = container_of(work->req, struct msg_request_wrapper, req);
		if (canceled || error) {
			reqw->cmpl_status = src_send_error;
			XDLIST_ADD_TAIL(&completed_req_q, reqw);
			gc_add_req(srm, work->req);
			finish_work_(srm, work);
			continue;
		}

		if (work_completed(work)) {
			reqw->cmpl_status = !error ? src_send_comp_ok : src_send_error;
			XDLIST_ADD_TAIL(&completed_req_q, reqw);
			N__D(trace_srm_srm_post_send_cmpl, "msg_id=@MSG_ID_INT completed reqw=@REQW status=@STATUS",
				work->msg_id, reqw, reqw->cmpl_status);
			gc_add_req(srm, work->req);
			finish_work_(srm, work);
			continue;
		}

		if (work->swnd.nack_mask) {
			process_nack(work);
		}

		slide = slide_send_window(work, &work->swnd);
		if (slide) {
			int rv = transfer_raft_msg(work);
			if (rv) {
				work->error = error = true;
				reqw->cmpl_status = src_send_error;
				XDLIST_ADD_TAIL(&completed_req_q, reqw);
				gc_add_req(srm, work->req);
				finish_work_(srm, work);
			}
		}
		getnstimeofday_boot(&work->window_time);
	}

	unlock(srm);

	while ((reqw = XDLIST_FIRST(&completed_req_q))) {
		N__D(trace_srm_srm_post_send_cmpl_2, "finalize reqw=@PPP status=@INT", reqw, reqw->cmpl_status);
		XDLIST_DEL(&reqw->cmpl_link);
		call_cb(srm, &reqw->req, reqw->cmpl_status, 0);
	}

}

static inline struct connection_work *
work_from_msg_id(struct nvmeibt_srm *srm, uint16_t msg_id)
{
	struct connection_work *work = NULL, *w;
	XDLIST_FOREACH(w, &srm->work_q) {
		if (w->msg_id == msg_id) {
			work = w;
			break;
		}
	}
	return work;
}

static inline bool
validate_recv_ack_id(struct connection_work *work, struct rsrm_recv_window *rwnd)
{
	uint32_t ack_id = nvmeib_ntohl(rwnd->ack_id);
	bool verdict;
	struct nvmeibt_srm __attribute__ ((unused)) *srm = work->srm;

	if (!work->ack_id) {
		work->ack_id = ack_id;
	}

	verdict = (ack_id == work->ack_id);

	if (!verdict) {
		N__T(trace_srm_validate_recv_ack_id,
			"msg_id=@MSG_ID_INT ACK invalid id expect @ACK_ID got @ACK_ID",
			work->msg_id, work->ack_id, ack_id);
	}

	return verdict;
}

int normalize_recv_ack(
	struct connection_work *work,
	struct rsrm_recv_window *rwnd, wnd_mask_t *mask)
{
	int wnd_size = 0;
	typeof (&work->swnd) swnd = &work->swnd;
	typeof (work->srm) __attribute__ ((unused)) srm = work->srm;
	typeof (rwnd->head_frame_id) rtail = rwnd->head_frame_id + RSRM_SWND_SIZE - 1;
	typeof (swnd->head_frame_id) htail = swnd->head_frame_id + RSRM_SWND_SIZE - 1;

	if (rwnd->head_frame_id == swnd->head_frame_id) {
		wnd_size = RSRM_SWND_SIZE;
		*mask = rwnd->recv_mask;
	} else if (rwnd->head_frame_id < swnd->head_frame_id) {
		if (rtail < swnd->head_frame_id) {
			N__D(trace_srm_normalize_recv_ack, "ACK id=@MSG_ID_INT too old", rwnd->msg_id);
			goto out;
		}
		wnd_size = 1 + rtail - swnd->head_frame_id;
		*mask = rwnd->recv_mask >> (wnd_size - 1);
	} else {
		if (rwnd->head_frame_id > htail + 1) {
					N__E(error_srm_normalize_recv_ack, "ACK id=@MSG_ID_INT future ack", rwnd->msg_id);
					goto out;
		}
		if (rwnd->head_frame_id == htail + 1) {
			*mask = RSRM_SWND_BITFLD_MASK;
		} else {
			int prefix = rwnd->head_frame_id - swnd->head_frame_id;
			*mask = (rwnd->recv_mask << prefix) | ((1UL << prefix) - 1);
		}
		wnd_size = RSRM_SWND_SIZE;
	}
out:
	return wnd_size;
}

enum frame_recv_state {
	FRAME_LOST = -1,
	FRAME_NOT_RECEIVED,
	FRAME_RECEIVED
};

static struct connection_msg *swnd_msg(struct connection_work *work, uint8_t bit)
{
	typeof(work->srm) srm = work->srm;
	typeof(work->swnd) *swnd = &work->swnd;
	struct connection_msg *msg = NULL;
	struct rsrm_frame_header *msg_hdr;
	uint16_t frame_id;
	uint8_t pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;
	msg = srm->msgs + swnd->frame[pos].msg_index;
	msg_hdr = srm_data_buffer(&msg->wire, &srm->carrier);
	frame_id = nvmeib_ntohs(msg_hdr->frame_id);
	if (frame_id != swnd->head_frame_id + bit) {
		N__E(error_srm_swnd_msg, "msg_id=@MSG_ID_INT wire does not match "
			"bit=@BIT swnd frame=@FRAME wire frame=@FRAME", work->msg_id, bit,
			swnd->head_frame_id + bit, frame_id);
		CRITICAL_BUG();
	}
	return msg;
}

/**
 * poll_cq() should pick a single ACK.
 * If additional ACK arrive during poll_cq look ignore previous nACK data
 */
void rsrm_recv_ack(struct nvmeibt_srm *srm, struct rsrm_frame_header *h)
{
	uint16_t msg_id = nvmeib_ntohs(h->msg_id);
	struct rsrm_recv_window *rwnd = (typeof(rwnd))(h + 1);
	struct connection_work *work = NULL;
	struct rsrm_send_window *swnd = NULL;
	uint8_t rwnd_size = RSRM_SWND_SIZE, msb;
	wnd_mask_t rwnd_bitmap = rwnd->recv_mask;
	enum frame_recv_state frs = FRAME_NOT_RECEIVED;

	NTOMA_ASSERT(error_srm_rsrm_recv_ack, is_already_locked(srm), "@SRM_NAME msg_id=@MSG_ID_INT srm is not locked",
	            srm->name, work->msg_id);
	work = work_from_msg_id(srm, msg_id);
	if (!work) {
		N__D(trace_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT ACK cannot find work", msg_id);
		goto fin;
	} else if (work->canceled || work->error) {
		N__T(trace_srm_rsrm_recv_ack_0, "msg_id=@MSG_ID_INT ACK work is not valid", msg_id);
		goto fin;
	}

	swnd = &work->swnd;

	N__D(trace_1_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT ack received swnd=@SWND_INT:@SEND_MASK:@ACK_MASK:@NACK_MASK rwnd=@RWND_INT:@RECV_MASK", work->msg_id,
		swnd->head_frame_id, swnd->send_mask,
		swnd->ack_mask, swnd->nack_mask,
		rwnd->head_frame_id, rwnd->recv_mask);

	if (!validate_recv_ack_id(work, rwnd)) {
		terminate_expired_work(srm, work, 1);
		goto fin;
	}

	if (swnd->nack_mask) {
		swnd->nack_mask = 0;
		N__D(trace_2_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT clear nack info", work->msg_id);
	}

	rwnd_size = normalize_recv_ack(work, rwnd, &rwnd_bitmap);
	if (!rwnd_bitmap) {
		N__D(trace_3_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT normalized ack is zero", work->msg_id);
		goto out;
	}

	msb = RSRM_SWND_SIZE - __builtin_clzl(rwnd_bitmap) - 1;
	N__D(trace_4_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT normilized ack swnd=@SWND_INT:@SEND_MASK rwnd=@RWND_INT:@RWND_BITMAP size=@SIZE msb=@MSB",
		work->msg_id, swnd->head_frame_id, swnd->send_mask,
		swnd->head_frame_id, rwnd_bitmap, rwnd_size, msb);

	for (int8_t bit = msb; bit > -1; bit--) {
		/* ACK bits verified from MSB down for better lost frames detection
		 * Frame X in send window considered lost if it dit not receive ACK while
		 * any frame X+a, a>0 in the same send window did receive ACK
		 */

		if (!bit_set(swnd->send_mask, bit)) {
			if (false) {
				N__D(trace_5_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT send bit not set "
					"bit=@BIT swnd=@SWND rwnd=@RWND",
					work->msg_id, bit, swnd->send_mask, rwnd_bitmap);
			}
			continue;
		}

		if (!bit_set(rwnd_bitmap, bit)) {
			if (frs == FRAME_RECEIVED) {
				frs = FRAME_LOST;
			}

			if (frs == FRAME_LOST) {
				swnd->nack_mask |= (1UL << bit);
				N__D(trace_6_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT frame=@FRAME was lost - nack",
					msg_id, swnd->head_frame_id + bit);
			}
			continue;
		}

		if (!bit_set(swnd->ack_mask, bit)) {
			struct connection_msg *msg;
			uint8_t pos = (swnd->head_pos + bit) & RSRM_SWND_POS_MASK;

			swnd->ack_mask |= (1UL << bit);
			swnd->acked_frames++;
			if (frs == FRAME_NOT_RECEIVED) {
				frs = FRAME_RECEIVED;
			}
			msg = swnd_msg(work, bit);
			if (false) {
				N__D(trace_7_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT release frame=@FRAME bit=@BIT pos=@INT", work->msg_id,
					swnd->head_frame_id + bit, bit, pos);
			}
			release_msg(srm, msg, work->msg_id);
			swnd->frame[pos].frame_id = NO_FRAME;
			swnd->frame[pos].msg_index = NO_FRAME;
		}
	}

out:
	N__D(trace_8_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT acked frames=@FRAMES total frames=@FRAMES", work->msg_id,
			swnd->acked_frames, work->frames_num);

	if (swnd->acked_frames > work->frames_num) {
		N__E(error_1_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT acked frames=@FRAMES total frames=@FRAMES", work->msg_id,
				swnd->acked_frames, work->frames_num);
		CRITICAL_BUG();
	}

	if (swnd->ack_mask & swnd->nack_mask) {
		N__W(error_2_srm_rsrm_recv_ack, "msg_id=@MSG_ID_INT ACKs messed up", work->msg_id);
		swnd->nack_mask &= ~swnd->ack_mask;
	}
fin:
	;
}

static void srm_process_incoming_frame(
	struct nvmeibt_srm *srm,
	struct srm_recv_context *recv_ctx,
	struct rsrm_frame_header *h)
{
	int bit;
	uint16_t msg_id = nvmeib_ntohs(h->msg_id);
	uint16_t frame_id = nvmeib_ntohs(h->frame_id);
	uint16_t frame_len = nvmeib_ntohs(h->frame_len);
	uint32_t data_offset = nvmeib_ntohl(h->data_offset);
	struct rsrm_recv_window *rwnd = &recv_ctx->rwnd;
	void *data = h + 1;

	// FIN;

	if (msg_recv_completed(recv_ctx)) {
		N__I(trace_srm_srm_process_incoming_frame_1,
			 "msg_id=@MSG_ID_INT got frames on completed msg", msg_id);
		rwnd->poll_frames_received++; // force return ACK
		goto out;
	}
	if (frame_id < rwnd->head_frame_id) {
		N__T(trace_srm_srm_process_incoming_frame_2,
			"msg_id=@MSG_ID_INT got resend frame=@FRAME "
			"rwnd=@RWND:@HEAD_FRAME_ID",
			msg_id, frame_id, rwnd->recv_mask, rwnd->head_frame_id);
		rwnd->poll_frames_received++; // force return ACK
		goto out;
	}
	if (frame_id > rwnd->head_frame_id + RSRM_SWND_SIZE) {
		N__W(warn_srm_srm_process_incoming_frame, "msg_id=@MSG_ID_INT discard invalid frame=@FRAME rwnd=@RWND:@HEAD_FRAME_ID",
			msg_id, frame_id, rwnd->recv_mask, rwnd->head_frame_id);
		goto out;
	}

	if (false) {
		N__D(trace_3_srm_srm_process_incoming_frame, "msg_id=@MSG_ID_INT incoming frame=@FRAME frame len=@LEN data offset=@UINT rwnd=@RWND:@HEAD_FRAME_ID",
			msg_id, frame_id, frame_len, data_offset,
			rwnd->recv_mask, rwnd->head_frame_id);
	}

	bit = frame_id - rwnd->head_frame_id;
	if (bit_set(rwnd->recv_mask, bit)) {
		if (false) {
			N__D(trace_1_srm_srm_process_incoming_frame, "msg_id=@MSG_ID_INT skip frame=@FRAME retransmition", msg_id, frame_id);
		}
		goto out;
	}

	rwnd->recv_mask |= (1UL << bit);
	rwnd->msg_frames_received++;
	rwnd->poll_frames_received++;
	recv_ctx->rcv_size += frame_len;

	memcpy(recv_ctx->rcv_pos + data_offset, data, frame_len);

	if (false) {
		N__D(trace_2_srm_srm_process_incoming_frame, "msg_id=@MSG_ID_INT RCV frame=@FRAME rcv_size=@RCV_SIZE:@RCV_BUFFER_LEN rwnd=@RWND:@HEAD_FRAME_ID "
			"rcv_ctx=@RCV_CTX polled frames=@FRAMES",
			rwnd->msg_id, frame_id,
			recv_ctx->rcv_size, recv_ctx->rcv_buffer_len,
			rwnd->recv_mask, rwnd->head_frame_id,
			recv_ctx, rwnd->poll_frames_received);
	}

	if (recv_ctx->rcv_size > recv_ctx->rcv_buffer_len) {
		N__E(error_srm_srm_process_incoming_frame, "msg_id=@MSG_ID_INT recv_size=@RECV_SIZE recv_buffer_len=@RECV_BUFFER_LEN",
			rwnd->msg_id, recv_ctx->rcv_size, recv_ctx->rcv_buffer_len);
		CRITICAL_BUG();
	}
out:
	if (0) {
		NFOUT;
	}
}

static inline bool msg_recv_completed(struct srm_recv_context *recv_ctx)
{
	return recv_ctx->rcv_size && recv_ctx->rcv_size == recv_ctx->rcv_buffer_len;
}

static void build_frame_ack(struct nvmeibt_srm *srm,
							struct connection_msg *msg,
							struct srm_recv_context *recv_ctx)
{
	struct rsrm_frame_header *h = srm_data_buffer(&msg->wire, &srm->carrier);
	struct rsrm_recv_window *rwnd = &recv_ctx->rwnd;
	void *data = h + 1;

	NTOMA_ASSERT(error_srm_build_frame_ack, (uint64_t)h == (uint64_t)srm_data_buffer(&msg->wire, &srm->carrier),  "ptr verification 1");
	NTOMA_ASSERT(error_1_srm_build_frame_ack, (uint64_t)data - (uint64_t)h == sizeof(*h), "ptr verification 2");

	if (rwnd->msg_id == SRM_RESERVED_MSG_ID) {
		N__E(error_2_srm_build_frame_ack, "msg_id=@MSG_ID_INT is invalid", SRM_RESERVED_MSG_ID);
		CRITICAL_BUG();
	}

	h->msg_type = nvmeib_htonl(NVMEIBT_PROTOCOL_SIGNATURE_RAFT_FRAME_ACK);
	h->srm_id = nvmeib_htonl(srm->send_srm_id);
	h->msg_id = nvmeib_htons(rwnd->msg_id);
	h->msg_length = 0;
	h->frame_id = 0;
	memcpy(data, rwnd, sizeof(*rwnd));
	msg->wire.len = sizeof(*h) + sizeof(*rwnd);
	msg->recv_ctx = recv_ctx;
	recv_ctx->acks_num = 0;

	h->frame_len = nvmeib_htons((uint16_t)sizeof(*rwnd));
	if (false) {
		N__D(trace_srm_build_frame_ack, "ACK msg_id=@MSG_ID_INT wnd_id=@WND_ID rwnd=@RWND:@HEAD_FRAME_ID",
			rwnd->msg_id, rwnd->ack_id,
			rwnd->recv_mask, rwnd->head_frame_id);
	}
}

static inline bool srm_fault__stash_ack(
		__attribute__((unused)) struct nvmeibt_srm *srm,
		__attribute__((unused)) uint16_t msg_id)
{
#if defined(SRM_ENABLE_FAULTS)
	if (srm_faults_active && (++srm->__fault_stash_ack % 17) == 0) {
		N__I(trace_srm_srm_fault__stash_ack_a, "FAULT:msg_id=@MSG_ID_INT simulate lost ACK", msg_id);
		return true;
	}
	return false;
#else
	return false;
#endif
}

static XDLIST_DECLARE(, struct nvmeibt_srm, resend_ack_link) rsrm_resend_ack_q = XDLIST_INIT(rsrm_resend_ack_q);

void srm_post_recv_cmpl(struct nvmeibt_srm *srm)
{
	int rv;
	struct connection_msg *ack_msg;
	struct srm_recv_context *recv_ctx;
	bool signal_send_cmpl = should_send_signal(srm);
	XDLIST_DECLARE(, struct srm_recv_context, recv_completed_link) completed_msg;
	XDLIST_HEAD_INIT(&completed_msg);

	lock(srm);
	if (!srm->allow_send) {
		N__T(trace_srm_srm_post_recv_cmpl_0,
			 "conn=@CONN srm=@SRM down cannot transfer ACK",
			 srm_carrier(srm), srm);
		XDLIST_FOREACH_SAFE(recv_ctx, &srm->recv_ctx_q) {
			struct rsrm_recv_window *rwnd = &recv_ctx->rwnd;
			rwnd->poll_frames_received = 0;
		}
		goto out;
	}

	XDLIST_FOREACH_SAFE(recv_ctx, &srm->recv_ctx_q) {
		struct rsrm_recv_window *rwnd = &recv_ctx->rwnd;

		// keep receive window active after full message was received
		if (rwnd->poll_frames_received) {
			rwnd->poll_frames_received = 0;
			slide_recv_window(srm, recv_ctx);
			if (!srm_fault__stash_ack(srm, rwnd->msg_id)) {
				N__D(trace_srm_srm_post_recv_cmpl, "ACK msg_id=@MSG_ID_INT "
					"rwnd={head=@HEAD recv_mask=@RECV_MASK}", rwnd->msg_id,
					rwnd->head_frame_id, rwnd->recv_mask);
				ack_msg = recv_ctx->ack_msg;
				build_frame_ack(srm, ack_msg, recv_ctx);
				XDLIST_DEL(&srm->resend_ack_link);
				XDLIST_ADD_TAIL(&rsrm_resend_ack_q, srm);
				rv = srm->carrier.send_msg(&srm->carrier, &ack_msg->wire,
												rwnd->msg_id, signal_send_cmpl);
				if (!rv) {
					srm->qp_send_count++;
					signal_send_cmpl = should_send_signal(srm);
				}
			}
		} else {
			// no new frames after CQ poll.
			// re-transmit prevousr ACK if needed
			if (false) {
				N__D(trace_1_srm_srm_post_recv_cmpl, "msg_id=@MSG_ID_INT recv_ctx=@RECV_CTX no poll frames",
					rwnd->msg_id, recv_ctx);
			}
			continue;
		}

		if (msg_recv_completed(recv_ctx) && recv_ctx->rcv_buffer) {
			N__D(trace_2_srm_srm_post_recv_cmpl, "msg_id=@MSG_ID_INT data received len=@LEN",
				rwnd->msg_id, recv_ctx->rcv_size);
			XDLIST_ADD_TAIL(&completed_msg, recv_ctx);
			/* do not zero receive window here - ACKs have to be sent back */
		}
	}
out:
	unlock(srm);
	XDLIST_FOREACH_SAFE(recv_ctx, &completed_msg) {
		N__D(trace_3_srm_srm_post_recv_cmpl, "msg_id=@MSG_ID_INT recv completed", recv_ctx->rwnd.msg_id);
		if (nvmeibt_get_nw_node())
			nvmeibt_nm_add_wire_recv(nvmeibt_get_nw_node(),
					recv_ctx->rcv_buffer);
		recv_ctx->rcv_buffer = NULL;
		recv_ctx->rcv_pos = NULL;
	}
}

static inline bool srm_fault__stash_incoming_frame(
		__attribute__((unused)) struct nvmeibt_srm *srm,
		__attribute__((unused)) struct rsrm_frame_header *h)
{
#if defined(SRM_ENABLE_FAULTS)
	uint16_t msg_id   = nvmeib_ntohs(h->msg_id);
	uint16_t frame_id = nvmeib_ntohs(h->frame_id);
	if (srm_faults_active && (++srm->__fault_stash_frame % 7) == 0) {
		N__I(trace_srm_srm_fault__stash_incoming_frame_a, "FAULT msg_id=@MSG_ID_INT drop incoming frame=@FRAME", msg_id, frame_id);
		return true;
	}
	return false;
#else
	return false;
#endif
}

static int rsrm_receive_completion(struct nvmeibt_srm *srm, void *buffer)
{
	int rv = -1;
	struct rsrm_frame_header *h = buffer;
	uint32_t msg_srm_id = nvmeib_ntohl(h->srm_id);
	uint16_t msg_id = nvmeib_ntohs(h->msg_id);
	uint32_t msg_type = nvmeib_ntohl(h->msg_type);
	uint32_t msg_len = nvmeib_ntohl(h->msg_length);
	struct srm_recv_context *recv_ctx;

	if (msg_id == SRM_RESERVED_MSG_ID) {
		N__E(error_srm_rsrm_receive_completion, "msg_id=@MSG_ID_INT is invalid", SRM_RESERVED_MSG_ID);
		CRITICAL_BUG();
	}

	lock(srm);

	if (!srm->allow_receive || !srm->allow_send) {
		N__T(trace_srm_rsrm_receive_completion_0,
			 "manager is closed: allow_receive=@ALLOW_RECEIVE "
			 "allow_send=@ALLOW_SEND",
			 srm->allow_receive, srm->allow_send);
		goto out;
	} else if (msg_srm_id != srm->recv_srm_id) {
		N__E(error_1_srm_rsrm_receive_completion,
			 "incoming message from wrong SRM=@SRM_INT me=@RECV_SRM_ID",
			 msg_srm_id, srm->recv_srm_id);
		goto out;
	}

	if (is_raft_frame_ack(msg_type)) {
		rsrm_recv_ack(srm, h);
		rv = 0;
		goto out;
	}

	recv_ctx = fetch_recv_context(srm, msg_id, msg_len, msg_type);
	if (!recv_ctx) {
		N__T(trace_1_srm_rsrm_receive_completion_0,
			"msg_id=@MSG_ID_INT ignore incoming frame", msg_id);
		goto out;
	}

	if (msg_id != recv_ctx->rwnd.msg_id) {
		N__E(error_2_srm_rsrm_receive_completion, "msg_id do not match @MSG_ID_INT:@MSG_ID_INT", msg_id, recv_ctx->rwnd.msg_id);
		CRITICAL_BUG();
	}

	if (srm_fault__stash_incoming_frame(srm, h))
		goto out;

	srm_process_incoming_frame(srm, recv_ctx, h);

out:
	unlock(srm);

	return rv;
}

void nvmeibt_srm_post_cmpl(struct nvmeibt_srm *srm)
{
	srm_post_recv_cmpl(srm);
	srm_post_send_cmpl(srm);
}

static XDLIST_DECLARE(, struct nvmeibt_srm, resend_link) rsrm_resend_q = XDLIST_INIT(rsrm_resend_q);

static void rsrm_cancel_srm_timers(struct nvmeibt_srm *srm)
{
	struct connection_work *work;
	NTOMA_ASSERT(error_srm_rsrm_cancel_srm_timers, is_already_locked(srm), "srm @SRM is not locked", srm);

	NFIN;
	XDLIST_FOREACH_SAFE(work, &srm->work_q) {
		N__D(trace_srm_rsrm_cancel_srm_timers, "msg_id=@MSG_ID_INT cancel work timer", work->msg_id);
		XDLIST_DEL(&work->tm_link);
	}
	NFOUT;
}

static int srm_resend_fd = -1;
int rsrm_init_work_tmq(void)
{
	int rv = -ENODEV;
	XDLIST_HEAD_INIT(&rsrm_resend_q);
	XDLIST_HEAD_INIT(&rsrm_resend_ack_q);

	srm_resend_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
	if (srm_resend_fd == -1) {
		N_Ef(error_toma_nvmeibt_toma_init, "srm_resend_fd: timerfd failure error @AUTO_ERRNO");
		goto out;
	} else {
		struct itimerspec tmspec = {TIMESPEC_ZERO, TIMESPEC_ZERO};
		rv = timerfd_settime(srm_resend_fd, 0, &tmspec, NULL);
		if (rv == -1) {
			N_Ef(trace_21_toma_nvmeibt_toma_init, "srm_resend_fd: failed to init @AUTO_ERRNO");
			goto out;
		}
	}
	rv = 0;
out:
	return rv;
}

int rsrm_get_fd_timer(void)
{
	return srm_resend_fd;
}

static void rsrm_queue_send_work(struct nvmeibt_srm *srm)
{
	if (XDLIST_NULL(&srm->resend_link)) {
		N__D(trace_srm_rsrm_queue_send_work, "add to resend queue srm=@SRM", srm);
		XDLIST_ADD_TAIL(&rsrm_resend_q, srm);
		if (XDLIST_N_ELEMNTS(&rsrm_resend_q) == 1) {
			struct itimerspec	tmspec;
			tmspec.it_interval = timespec_from_nsec(nvmeibt_raft_get_effective_heartbeat_timeout_ns() / 20);
			tmspec.it_value    = tmspec.it_interval;
			if (timerfd_settime(srm_resend_fd, 0, &tmspec, NULL) == -1) {
				int err = errno;
				N__E(error_srm_rsrm_queue_send_work, "failed to start resend timer err=@ERR", err);
				nvmeibt_abort(ES_FATAL);
			}
			N__D(trace_1_srm_rsrm_queue_send_work, "start resend timer");
		}
	} else {
		N__D(trace_2_srm_rsrm_queue_send_work, "on resend queue srm=@SRM", srm);
	}
}

static void rsrm_dequeue_send_work(struct nvmeibt_srm *srm)
{
	if (XDLIST_NULL(&srm->resend_link)) {
		N__E(error_srm_rsrm_dequeue_send_work, "not on resend queue srm=@SRM", srm);
		CRITICAL_BUG();
	}

	if (!XDLIST_N_ELEMNTS(&srm->work_q)) {
		XDLIST_DEL(&srm->resend_link);
		N__D(trace_srm_rsrm_dequeue_send_work, "remove from resend queue srm=@SRM", srm);

		if (!XDLIST_N_ELEMNTS(&rsrm_resend_q)) {
			struct itimerspec tmspec = {
				.it_interval = TIMESPEC_ZERO,
				.it_value    = TIMESPEC_ZERO
			};
			int rv = timerfd_settime(srm_resend_fd, 0, &tmspec, NULL);
			if (rv == -1) {
				int err = errno;
				N__E(error_1_srm_rsrm_dequeue_send_work, "failed to stop resend timer err=@ERR", err);
				nvmeibt_abort(ES_FATAL);
			}
			N__D(trace_1_srm_rsrm_dequeue_send_work, "stop resend timer");
		}
	}

}

static void terminate_expired_work(struct nvmeibt_srm *srm, struct connection_work *work, bool use_cb)
{
	struct nvmeibt_msg_request *req;

	NFIN;
	N__T(warn_srm_terminate_expired_work, "msg_id=@MSG_ID_INT terminate expired work", work->msg_id);
	work->error = true;
	req = work->req;
	gc_add_req(srm, req);
	finish_work_(srm, work);
	if (use_cb)
		call_cb(srm, req, src_send_error, 0);
	NFOUT;
}

void srm_terminate_all_works(struct nvmeibt_srm *srm)
{
	struct connection_work *work;
	struct msg_request_wrapper *reqw;
	XDLIST_DECLARE(, struct msg_request_wrapper, cmpl_link) completed_req_q;
	XDLIST_HEAD_INIT(&completed_req_q);

	NFIN;
	lock(srm);
	XDLIST_FOREACH_SAFE(work, &srm->work_q) {
		reqw = container_of(work->req, typeof(*reqw), req);
		XDLIST_ADD_TAIL(&completed_req_q, reqw);
		terminate_expired_work(srm, work, 0);
	}
	unlock(srm);

	while ((reqw = XDLIST_FIRST(&completed_req_q))) {
		N__D(trace_srm_terminate_all_works,
			 "finalize reqw=@PPP status=@INT", reqw, src_send_error);
		XDLIST_DEL(&reqw->cmpl_link);
		call_cb(srm, &reqw->req, src_send_error, 0);
	}

	NFOUT;
}

void rsrm_resend_acks(void)
{
	struct nvmeibt_srm *srm;

	//NFIN;
	XDLIST_FOREACH_SAFE(srm, &rsrm_resend_ack_q) {
		struct srm_recv_context *recv_ctx;
		lock(srm);
		if (!srm->allow_send) goto nlk;

		XDLIST_FOREACH_SAFE(recv_ctx, &srm->recv_ctx_q) {
			struct rsrm_recv_window *rwnd = &recv_ctx->rwnd;
			struct connection_msg *ack = recv_ctx->ack_msg;
			if (!ack) continue;
			if (++recv_ctx->acks_num > 4) {
				XDLIST_DEL(&srm->resend_ack_link);
				goto nlk;
			}
			srm->carrier.send_msg(&srm->carrier, &ack->wire,
									rwnd->msg_id, should_send_signal(srm));
			N__D(trace_srm_rsrm_resend_acks, "ACK msg_id=@MSG_ID_INT resend @ACKS_NUM "
				"rwnd={head=@HEAD recv_mask=@RECV_MASK}", rwnd->msg_id, recv_ctx->acks_num,
				rwnd->head_frame_id, rwnd->recv_mask);
		}
nlk:
		unlock(srm);
	}
	//NFOUT;
}

int rsrm_resend_timer(void)
{
	struct nvmeibt_srm *srm;
	struct connection_work *work;
	struct timespec now;
	long window_tm;
	struct msg_request_wrapper *reqw;
	uint64_t rsrm_tm_buffer;
	int rv;
	XDLIST_DECLARE(, struct msg_request_wrapper, cmpl_link) completed_req_q;

	XDLIST_HEAD_INIT(&completed_req_q);

	NFIN;
	rv = read(srm_resend_fd, &rsrm_tm_buffer, sizeof(rsrm_tm_buffer));	// Dummy read
	getnstimeofday_boot(&now);
	XDLIST_FOREACH_SAFE(srm, &rsrm_resend_q) {
		lock(srm);
		XDLIST_FOREACH_SAFE(work, &srm->work_q) {
			if (!work->req) {
				N__E(error_srm_rsrm_resend_timer, "SRM with no work on resend queue");
				CRITICAL_BUG();
			}

			window_tm = timespec_diff_ns(now, work->window_time);
			if (window_tm >= ROCE_RESEND_TMOUT_NSEC) {
				if (true) {
					N__D(trace_srm_rsrm_resend_timer, "msg_id=@MSG_ID_INT resend window after @WINDOW_TM nsec",
						work->msg_id, window_tm);
					rsrm_resend_window(work);
				} else {
					break;
				}
			}
		}
		unlock(srm);
		while ((reqw = XDLIST_FIRST(&completed_req_q))) {
			N__D(srm_rsrm_resend_timer_trace, "finalize reqw=@PPP status=@INT", reqw, src_send_error);
			XDLIST_DEL(&reqw->cmpl_link);
			call_cb(srm, &reqw->req, src_send_error, 0);
		}
	}

	NFOUT;
	return rv;
}

/********************************* SRM faults *********************************/
#include <sys/stat.h>
// Daniel: Consider moving to separate file
#define FIFO_PATH TOMA_DIR_RUN_NVMESH "/fifo_comm"
#define FIFO_COMM_BUFFER_LEN 64
#define FIFO_COMM_MAX_TOKENS 5

typedef void (*token_handler_t)(char *tokens[FIFO_COMM_MAX_TOKENS]);

struct fifo_comm_cmd_entry {
	const char *command;
	token_handler_t handler;
};

static int fifo_comm_rd = -1;
int rsrm_faults_get_fd(void)
{
	return fifo_comm_rd;
}

void rsrm_destroy_after_run(void)
{
	NNVMEIBT_CLOSE(tonecfd1, fifo_comm_rd);
}

static void open_fifo_comm(void)
{
	fifo_comm_rd = NNVMEIBT_OPEN(trace_toma_open_fifo_comm, FIFO_PATH, O_RDONLY | O_NONBLOCK, 0600);
	if (fifo_comm_rd < 0) {
		N_Wf(trace_1_toma_open_fifo_comm, "failed to open FIFO comm @AUTO_ERRNO");
	} else {
		N_Tf(info_toma_open_fifo_comm, "FIFO comm ready");
	}
}

int rsrm_faults_init_fifo_comm(void)
{
	int rv = 0;

	rv = mkfifo(FIFO_PATH, 0600);
	if (rv < 0) {
		if (errno == EEXIST) {
			N_Tf(trace_toma_init_fifo_comm, "FIFO comm already exists");
		} else {
			N_Wf(warn_toma_init_fifo_comm, "failed to create FIFO comm at string=@STR (@AUTO_ERRNO)", FIFO_PATH);
			rv = -1;
			goto out;
		}
	}

	open_fifo_comm();
out:
	return rv;
}

static void fifo_comm_test(char *tokens[FIFO_COMM_MAX_TOKENS] __attribute__ ((unused)))
{
	N_Tf(trace_toma_fifo_comm_test, "FIFO comm test");
}

static void fifo_comm_srm(char *tokens[FIFO_COMM_MAX_TOKENS])
{
	if (!strncmp(tokens[1], "faults", sizeof("faults") - 1)) {
		srm_faults_active = !strncmp(tokens[2], "on", sizeof("on") - 1);
		N_Tf(trace_toma_fifo_comm_srm, "FIFO comm set srm faults @STR", srm_faults_active ? "on" : "off");
	}
}

struct fifo_comm_cmd_entry fifo_comm_cmds[] = {
	{ "srm",		fifo_comm_srm		},
	{ "test",		fifo_comm_test 		},
	{ NULL, NULL}
};

void rsrm_faults_handle_fifo_comm(void)
{
	int rv, t = 0;
	char buffer[FIFO_COMM_BUFFER_LEN + 1], *bufptr = buffer;
	char *tokens[FIFO_COMM_MAX_TOKENS] = { 0, };
	rv = read(fifo_comm_rd, buffer, FIFO_COMM_BUFFER_LEN);
	if (rv < 0) {
		N_Ef(err_toma_handle_fifo_comm, "failed to read FIFO comm @AUTO_ERRNO");
		NNVMEIBT_CLOSE(trace_1_toma_handle_fifo_comm, fifo_comm_rd);
		open_fifo_comm();
		goto out;
	} else if (rv < FIFO_COMM_BUFFER_LEN) {
		buffer[rv] = '\0';
	} else {
		N_Tf(trace_toma_handle_fifo_comm, "FIFO input too long");
		NNVMEIBT_CLOSE(trace_2_toma_handle_fifo_comm, fifo_comm_rd);
		open_fifo_comm();
		goto out;
	}

	do {
		tokens[t] = strtok(bufptr, " ");
		if (tokens[t++]) {
			bufptr = NULL;
		} else {
			break;
		}
	} while (t < FIFO_COMM_MAX_TOKENS);

	if (!tokens[0]) goto out;

	for (typeof(fifo_comm_cmds[0]) *e = fifo_comm_cmds; e->command; e++) {
		if (!strncmp(e->command, tokens[0], sizeof(*tokens[0]) - 1)) {
			e->handler(tokens);
		}
	}
out:
	return;
}

