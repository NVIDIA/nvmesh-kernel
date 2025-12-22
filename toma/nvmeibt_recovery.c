#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#include "nvmeibt_toma.h"
#include "nvmeibt_common.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_recovery.h"
#include "nvmeibt_register.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_kafka.h"
#include "../common/nvmeib_volume_type.h"
#include "../autogen/clnt/nvmeibc_mcs_stub.h"

/*
 * State machines of recovery tasks:
 *
 *    state         meaning and expected action(s)
 *  ------------  ------------------------------------------------------------
 *  INIT          task created: select client, notify client (to attach)
 *  WAIT_CLIENT   client notified: wait for client to register
 *  IN_PROGRESS   task started: expect progress/finish/aborted reports
 *  RETRY_TASK    task experienced an error but not ended by TOMA: retry task
 *  FINISHED      task finished: success (terminal: TOMA will remove the task)
 *  CANCELED      task canceled: success (terminal: TOMA will remove the task)
 *  ERROR         task aborted: failure (terminal: TOMA will remove the task)
 *
 * Life-cycle of recovery tasks:
 *
 *  Recovery tasks are started by TOMA, and must be explicitly ended by TOMA.
 *
 *  Usually a task reaches a terminal state and then notify TOMA (through the
 *  nvmeibt_seg_active_recovery_done() callback), and TOMA will end the task.
 *
 *  TOMA may also request to end a task earlier, in which case the task will
 *  be marked (with task->end_ordered) and will not notify TOMA when reaching
 *  a terminal state.
 *
 *  TOMA calls tid = nvmeibt_recovery_start_rebuild() to start a task. Tasks
 *  start in INIT state and make progress via recovery_task_state_machine()
 *  and handling of incoming messages from the client.
 *
 *  TOMA calls nvmeibt_recovery_launch_abort_rebuild(tid) to end tasks. A task in a
 *  terminal state (FINISHED/CANCELED/ERROR) is marked for deletion. A task in
 *  non-terminal state (e.g IN_PROGRESS) first notifies the client by sending
 *  abort message and then waits for client's ack. Deletion will be deferred
 *  until it reaches a terminal state.
 *
 *  If a task ends without success (i.e. canceled or due to error), it is the
 *  responsibility of TOMA to relaunch the task at a later time.
 *
 * Aborts and errors:
 *
 *  When TOMA ends a task in non-terminal state, we say the task is aborted.
 *  We send abort message to the client and wait for ack (up to timeout).
 *
 *  When an error occurs we switch to RETRY_TASK state (not terminal) and set
 *  a timeout. When the timeout expires we will automatically retry the task
 *  from scratch, perpetually, until TOMA ends the task. The retry timeout is
 *  calculated with linear backoff (with min/max values).
 *
 *  Errors can occur due to:
 *   - failure to setup client (select client, notify client)
 *   - timeout waiting for client to register after notify
 *   - failure to send msg to registrant (start, ping, abort)
 *   - timeout waiting for status report from client
 *   - received error indication from client in status report
 *   - received malformed status report from client
 *   - client unregistered/disconnected before task ends
 *
 * Timeouts:
 *
 *  In addition to task retry timeouts, for tasks in state IN_PROGRESS we use
 *  pings and a separate retry count before giving up on unresponsive clients.
 *  The rationale is that we already speak to the client, so try harder before
 *  tearing the connection.
 *
 *  If a timeout occurs without receiving status message, then we will send a
 *  ping message(s) (and reset the timer) to allow client to respond to us.
 *
 *  If a timeout still occurs, then we consider it an error, and send an abort
 *  message to abort the task. Eventually client will respond with status or
 *  another timeout will occur which we will consider an error.
 *
 * Things to do:
 *
 *  - Currently we always select the local client to do the work (also means
 *    we do not need ANNOUNCE messages). Should extend the selection logic and
 *    protocol to allow other clients as well.
 *
 *  - Currently every disk-segment recovery becomes one (possibly giant) task.
 *    Should split large tasks into smaller pieces, and be able to manage both
 *    multiple pieces and multiple clients per disk_segment->seg_active.
 *
 *  - Abort message to clients should use the "reason" argument: if triggered
 *    by recovery logic then internal reason, otherwise TOMA should provide
 *    the reason as argument to nvmeibt_recovery_launch_abort_rebuild().
 *
 */

enum RECOVERY_STATE {
	RECOVERY_STATE_NONE				= 0,
	RECOVERY_STATE_INIT				= 1,
	RECOVERY_STATE_WAIT_CLIENT		= 2,
	RECOVERY_STATE_IN_PROGRESS		= 3,
	RECOVERY_STATE_RETRY_TASK		= 4,
	RECOVERY_STATE_FINISHED			= 5,
	RECOVERY_STATE_CANCELED			= 6,
	RECOVERY_STATE_ERROR			= 7,
};

enum RECOVERY_ERROR {
	RECOVERY_ERROR_SELECT_CLIENT  = (1<<0),
	RECOVERY_ERROR_TIMEOUT_STATE  = (1<<2),
	RECOVERY_ERROR_MSG_TO_CLIENT  = (1<<3),
	RECOVERY_ERROR_CLIENT_ERROR   = (1<<4),
	RECOVERY_ERROR_CLIENT_GONE    = (1<<5),
};

#define	EXEC_ERR_EXTERNAL			(-1)
#define	EXEC_ERR_TIMEOUT			(-2)

struct recovery_task {
	struct {
		u64 tid;
		enum NVMEIBT_RECOVERY_TYPE type;	/* task description */
		u32 max_batch_size;
		u8  effort_percents;
	}; TODO(refactor: unify with struct nvmeibt_client_recovery_generic_header clnt)

	/* task properties */
	struct nvmeibt_seg_active *seg_active;
	int praid_version;

	/* client properties */
	struct nvmeibt_registrant_ctx *registrant;

	/* task state */
	enum RECOVERY_STATE state;
	struct timespec timeout_time;
	BOOL was_aborted;
	BOOL end_ordered;

	int n_pings;  /* see rationale in "Timeouts" section above */

	int task_retry;
	enum RECOVERY_STATE prev_state;  /* last state where error occurred */

	int error_bits;  /* indicate errors already reported */

	/* task status */
	int ret_code;

	struct xdlist link;
};

/* list of all active recovery tasks */
static XDLIST_DECLARE(, struct recovery_task, link)
		recovery_task_list = XDLIST_INIT(recovery_task_list);

static struct timespec next_wait_for_recovery_timeout = TIMESPEC_MAX_C99;

int64_t recovery_timeout_wait_client_sec = ATTACH_TIMEOUT_SEC_DEFAULT;

static bool is_rebuild_endless = false;
enum ENCRYPT_DELAY_E nvmeibt_encrypt_delay = ENCRYPT_DELAY_NONE;

/* timeout constants */
#define RECOVERY_TIMEOUT_INIT_CLIENT_SEC   1  /* for client module to be loaded */

#define RECOVERY_TIMEOUT_IN_PROGRESS_PING_SEC  5  /* for client to report progress (until ping) */
#define RECOVERY_TIMEOUT_IN_PROGRESS_MAX_SEC   30 /* for client to report progress (until give up) */

#define RECOVERY_TIMEOUT_RETRY_TASK_MSEC	1500   /* timeout for task retry */

/* skip report to mgmt of client errors if short lived (avoid spam) */
#define RECOVERY_DELAY_TO_REPORT_CLIENT_ERR_ATTEMPTS  3

/* max unanswered ping before giving up */
#define RECOVERY_PING_MAX  \
	(RECOVERY_TIMEOUT_IN_PROGRESS_MAX_SEC / RECOVERY_TIMEOUT_IN_PROGRESS_PING_SEC)

/* retries policy for failed recovery-detach */
#define RECOVERY_DETACH_N_RETRY_BASE		5		/* total retries count for regular (recovery) detach */
#define RECOVERY_DETACH_N_RETRY_FORCE		8		/* total retries count for --force (recovery) detach */
#define RECOVERY_DETACH_N_RETRY_MAX			INT_MAX	/* max retries count (meaning: don't retry further */

/* default priority (effort_percentage) */
#define RECOVERY_SCRUBBING_PRIORITY			1		/* default priority: 1% */

/* forward declarations */
static void recovery_task_state_machine(struct recovery_task *task, struct nvmeibt_registrant_ctx *new_local_reg_ctx);
static void recovery_task_set_state(
				struct recovery_task *task,
				enum RECOVERY_STATE state);
static void __recovery_task_fill_effort_params(struct recovery_task *task);

static int recovery_max_n_simultaneous_dirty_rebuild = MAX_N_SIMULTANEOUS_DIRTY_REBUILD_DEFAULT;
void nvmeibt_recovery_set_max_n_simultaneous_dirty_rebuild(int64_t max_n_simultaneous_dirty_rebuild)
{
	recovery_max_n_simultaneous_dirty_rebuild = max_n_simultaneous_dirty_rebuild;
	N_Tf(jii98hd, "recovery_max_n_simultaneous_dirty_rebuild=@INT", recovery_max_n_simultaneous_dirty_rebuild);
	if (recovery_max_n_simultaneous_dirty_rebuild <= 0) {
		N_Wf(ssio9ic, "recovery_max_n_simultaneous_dirty_rebuild=@INT", recovery_max_n_simultaneous_dirty_rebuild);
	}
	nvmeibt_recovery_execute_dirty_rebuilds_as_needed();
}

int64_t nvmeibt_recovery_get_max_n_simultaneous_dirty_rebuild(void)
{
	return recovery_max_n_simultaneous_dirty_rebuild;
}

static int recovery_max_n_simultaneous_stale_and_txid_rebuild = MAX_N_SIMULTANEOUS_STALE_AND_TXID_REBUILD_DEFAULT;
void nvmeibt_recovery_set_max_n_simultaneous_stale_and_txid_rebuild(int64_t max_n_simultaneous_rebuild)
{
	recovery_max_n_simultaneous_stale_and_txid_rebuild = max_n_simultaneous_rebuild;
	N_Tf(jiit6hd, "recovery_max_n_simultaneous_stale_and_txid_rebuild=@INT", max_n_simultaneous_rebuild);
	if (recovery_max_n_simultaneous_stale_and_txid_rebuild <= 0) {
		N_Wf(ssq29ic, "recovery_max_n_simultaneous_stale_and_txid_rebuild=@INT", max_n_simultaneous_rebuild);
	}
	nvmeibt_recovery_execute_stale_and_txid_rebuilds_as_needed();
}

int64_t nvmeibt_recovery_get_max_n_simultaneous_stale_and_txid_rebuild(void)
{
	return recovery_max_n_simultaneous_stale_and_txid_rebuild;
}

static int recovery_max_n_simultaneous_scrubbing = MAX_N_SIMULTANEOUS_SCRUBBING_DEFAULT;
void nvmeibt_recovery_set_max_n_simultaneous_scrubbing(int64_t max_n_simultaneous_scrubbing)
{
	recovery_max_n_simultaneous_scrubbing = max_n_simultaneous_scrubbing;
	N_Tf(4cfa983, "recovery_max_n_simultaneous_scrubbing=@INT", recovery_max_n_simultaneous_scrubbing);
	if (recovery_max_n_simultaneous_scrubbing <= 0) {
		N_Wf(hd785lb, "recovery_max_n_simultaneous_scrubbing=@INT", recovery_max_n_simultaneous_scrubbing);
	}
}

int64_t nvmeibt_recovery_get_max_n_simultaneous_scrubbing(void)
{
	return recovery_max_n_simultaneous_scrubbing;
}

static const char *recovery_state_to_str(enum RECOVERY_STATE state)
{
	switch (state) {
	case RECOVERY_STATE_NONE:			return "N/A";
	case RECOVERY_STATE_INIT:			return "INIT";
	case RECOVERY_STATE_WAIT_CLIENT:	return "WAIT_CLIENT";
	case RECOVERY_STATE_IN_PROGRESS:	return "IN_PROGRESS";
	case RECOVERY_STATE_RETRY_TASK:		return "RETRY_TASK";
	case RECOVERY_STATE_FINISHED:		return "FINISHED";
	case RECOVERY_STATE_CANCELED:		return "CANCELED";
	case RECOVERY_STATE_ERROR:			return "ERROR";
	default:
		N_Ef(error_recovery_recovery_state_to_str, "Unknown state=@STATE", state);
		return "???";
	}
}

static BOOL recovery_task_is_terminal_state(struct recovery_task *task)
{
	switch (task->state) {
	case RECOVERY_STATE_FINISHED:
	case RECOVERY_STATE_CANCELED:
	case RECOVERY_STATE_ERROR:
		return 1;
	default:
		return 0;
	}
}

BOOL nvmeibt_recovery_is_any_recovery_active(void)
{
	return !XDLIST_EMPTY(&recovery_task_list);
}

BOOL nvmeibt_recovery_launch_abort_all_recoveries_on_local_disk(struct nvmeibt_local_disk *local_disk)
{
	struct recovery_task	*task;
	BOOL					is_any = 0;

	NFIN;
	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {	// Expected to have up to 2 running
		if (task->end_ordered) {
			continue;
		}
		if (nvmeibt_seg_active_get_local_disk(task->seg_active) == local_disk) {
			N_Tf(vbs03l0, "disk=@STR tid=@TID", nvmeibt_local_disk_display(local_disk), task->tid);
			nvmeibt_recovery_launch_abort_rebuild(task->tid);
			is_any = 1;
		}
	}
	NFOUT;
	return is_any;
}

static void calc_next_wait_for_recovery_timeout(void)
{
	struct recovery_task	*task;
	struct timespec			now;
	struct timespec			ts_diff;

	NFIN;
	next_wait_for_recovery_timeout = TIMESPEC_MAX_C99;
	getnstimeofday(&now);
	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {
		next_wait_for_recovery_timeout = timespec_min(next_wait_for_recovery_timeout, task->timeout_time);
		ts_diff = timespec_sub(task->timeout_time, now);
		N_Tf(ects0l3, "tid=@TID timeout in @LLD.@TV_MSEC", task->tid, ts_diff.tv_sec, NSEC_TO_MSEC(ts_diff.tv_nsec));
	}
	ts_diff = timespec_sub(next_wait_for_recovery_timeout, now);
	N_Tf(omtv6fu, "next recovery timeout in @LLD.@TV_MSEC", ts_diff.tv_sec, NSEC_TO_MSEC(ts_diff.tv_nsec));
	NFOUT;
}

static void recovery_task_upd_timeout_time(struct recovery_task *task)
{
	struct timespec delta = TIMESPEC_ZERO;

	NFIN;

	getnstimeofday(&(task->timeout_time));

	switch (task->state) {
	case RECOVERY_STATE_INIT:
		delta.tv_sec += RECOVERY_TIMEOUT_INIT_CLIENT_SEC;
		break;
	case RECOVERY_STATE_WAIT_CLIENT:
		delta.tv_sec += recovery_timeout_wait_client_sec;
		break;
	case RECOVERY_STATE_IN_PROGRESS:
		delta.tv_sec += RECOVERY_TIMEOUT_IN_PROGRESS_PING_SEC;
		break;
	case RECOVERY_STATE_RETRY_TASK:
		delta.tv_sec = RECOVERY_TIMEOUT_RETRY_TASK_MSEC / 1000;
		delta.tv_nsec = MSEC_TO_NSEC(RECOVERY_TIMEOUT_RETRY_TASK_MSEC % 1000);
		break;
	case RECOVERY_STATE_FINISHED:
	case RECOVERY_STATE_CANCELED:
	case RECOVERY_STATE_ERROR:
		/*
		 * Task in terminal state: set timeout to 'now' to force a recovery
		 * timeout, which will handle the task removal and deletion.
		 */
		delta.tv_sec += 0;
		break;
	default:
		N_Ef(dju87gt, "Invalid task state tid=@TID state=@RECOVERY_STATE_TO_STR",
			task->tid, recovery_state_to_str(task->state));
		nvmeibt_abort(ES_FATAL);
	}

	N_Tf(vmkoi98, "next timeout for task tid=@TID state=@RECOVERY_STATE_TO_STR in @LLD.@TIMESPEC_NS",
		task->tid, recovery_state_to_str(task->state),
		delta.tv_sec, delta.tv_nsec);

	task->timeout_time = timespec_add(delta, task->timeout_time);
	calc_next_wait_for_recovery_timeout();

	NFOUT;
}

static BOOL recovery_task_is_timeout_expired(struct recovery_task *task)
{
	struct timespec now;

	getnstimeofday(&now);
	return timespec_ge(now, task->timeout_time);
}

/*
 * NOTE: resetting task->task_retry affects the task's next timeout.
 * The caller is responsible to call recovery_task_upd_timeout_time().
 */
static void recovery_reset_task_retry(struct recovery_task *task)
{
	NFIN;

	task->task_retry = 0;

	NFOUT;
}

#define __recovery_get_host_name(clnt)	({							\
				const struct nvmeibt_host_name *__clnt = &(clnt);	\
				__clnt->host_name[0] ? __clnt->host_name : "n/a";	\
			})

static void recovery_write_log_msg_to_mgmt(struct recovery_task *task,
										  enum RECOVERY_ERROR error, int clnt_err,
										  BOOL is_retry)

{
	char hdr[MGMT_LOG_MSG_HEADER_LEN];
	char msg[MGMT_LOG_MSG_MSG_LEN];
	const char *log;
	const char *volume;
	BOOL skip_msg;

	NFIN;

	volume = task->seg_active ?
			nvmeibt_seg_active_blkdev_name(task->seg_active) : "n/a";

	sprintf(hdr, "Volume rebuild failure");

	/*
	 * We were called to report an error to the mgmt system. But some errors
	 * are ephemeral and we should not bother the mgmt about them unless they
	 * persist. We set skip_msg=true in such case to skip the report.
	 */
	skip_msg = false;

	switch (error) {
	case RECOVERY_ERROR_SELECT_CLIENT:
		/* if failed to select a client to do recovery */
		log = "select client";
		sprintf(msg, "Volume %s: rebuild status: no client running on target node, will retry", volume);
		break;
	case RECOVERY_ERROR_TIMEOUT_STATE:
		/* if an unexpected timeout occurs in any state */
		log = "timeout occurred";
		sprintf(msg, "Volume %s: rebuild status: unexpected timeout, will retry", volume);
		break;
	case RECOVERY_ERROR_MSG_TO_CLIENT:
		/* if failed to send a msg to registered client */
		log = "message client";
		sprintf(msg, "Volume %s: rebuild status: failed to send a message to client running rebuild, will retry", volume);
		break;
	case RECOVERY_ERROR_CLIENT_ERROR:
		/* if client replied and reported error */
		log = "error by client";
		sprintf(msg, "Volume %s: rebuild status: client running rebuild reported an error, will retry", volume);
		break;
	case RECOVERY_ERROR_CLIENT_GONE:
		log = "client unregistered";
		sprintf(msg, "Volume %s: rebuild status: client running rebuild unregistered from volume, will retry", volume);
		break;
	default:
		log = NULL;  // appease compiler
		N_Ef(error_recovery_recovery_write_log_msg_to_mgmt, "invalid recovery task error @ERROR for tid=@TID", error, task->tid);
		nvmeibt_abort(ES_FATAL);
		break;
	}

	/*
	 * do not complain to mgmt about client errors before some retry:
	 * e.g. client may register with us but not yet with other peer, so not
	 * ready to start recovery and therefore will reject us; we expected it
	 * to resolve quickly so wait a short while before snitching to mgmt.
	 */
	if (is_retry && (task->task_retry <= RECOVERY_DELAY_TO_REPORT_CLIENT_ERR_ATTEMPTS))
		skip_msg = true;
	if (	(skip_msg ||
			 nvmeibt_register_is_client_delete_in_the_air(task->registrant) ||
			 (clnt_err == -8 /* topo, other_segs */) ||
			 (clnt_err == -10001 /* No topo, need to Reg */) ||
			 (clnt_err == -10040 /* DISK_DEAD */) ||
			 (clnt_err == -10051 /* Cannot start recov. before previous finished */))) {
		N_Tf(trace_1_recovery_recovery_write_log_msg_to_mgmt,
			"rebuild non fatal error: @STR: tid=@TID vol=@VOL seg=@UUID_8 error=@NUM clnt_err=@INT (skip)",
			log, task->tid, volume, nvmeibt_seg_active_UUID_8(task->seg_active), task->ret_code, clnt_err);
	} else {
		N_Wf(warn_recovery_recovery_write_log_msg_to_mgmt,
			"rebuild error: @STR: tid=@TID vol=@VOL seg=@UUID_8 error=@NUM clnt_err=@INT state=@RECOVERY_STATE_TO_STR",
			log, task->tid, volume, nvmeibt_seg_active_UUID_8(task->seg_active), error, clnt_err, recovery_state_to_str(task->state));
	}

	/* do not bother the mgmt with premature or unneeded warning */
	if (skip_msg)
		goto out;

	/* do not issue same warning repeatedly (until errors clean) */
	if (task->error_bits & error)
		goto out;

	/* finally committed to report to mgmt: mark this error as sent */
	task->error_bits |= error;

#if 1  /* change to "#if 0" to disable rebuild error to mgmt */
	nvmeibt_kafka_generic_log_msg_to_mgmt_send(NULL, hdr, msg, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
#endif

out:
	NFOUT;
}

static struct recovery_task *recovery_find_task_by_tid(u64 tid)
{
	struct recovery_task *task;
	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {
		if (task->tid == tid)
			return task;
	}
	return NULL;
}

/*****************           NetLink to local_clnt           ******************/

typedef void (*attach_detach_end_cb_t)(int, struct nvmeibt_block_device *, enum RECOVERY_ATTACH_CMD);

struct attach_detach_wq_entry {
	struct nvmeibt_wq_entry 			wq_entry;
	struct nvmeibt_block_device			*vol;
	attach_detach_end_cb_t				finalize_specific;
	void								*serialized_blkdev_for_clnt_4k_aligned;
	size_t								serialized_blkdev_for_clnt_len;
	enum RECOVERY_ATTACH_CMD			attach_cmd;
	enum nvmeibc_config_volume_type		vol_type;
	char								attach_token[16];
	struct xdlist						attach_detach_list_link;
	bool								fail_attach_due_to_missing_disk;
	int									rv;				/* Output */
};

void *serialize_seg_to_nvmeibc(struct nvmeibc_segment_conf *out_p, struct nvmeibt_disk_segment *seg, int8_t actual_idx)
{
	nvmeibt_strlcpy(out_p->uuid, nvmeibt_union_uuid_to_urn_uuid(&(seg->from_config.id)).str, min(sizeof(out_p->uuid), sizeof(struct nvmeibt_urn_uuid)));
	out_p->lbs = seg->seg_mgmt.lb_s;
	out_p->lbe = seg->seg_mgmt.lb_e;
	nvmeibt_strlcpy(out_p->diskUUID, nvmeibt_union_uuid_to_urn_uuid(&(seg->seg_mgmt.disk_id)).str, min(sizeof(out_p->diskUUID), sizeof(struct nvmeibt_urn_uuid)));
	nvmeibt_strlcpy(out_p->diskID, nvmeibt_disk_segment_get_disk_ldisk_id_str(seg), sizeof(out_p->diskID));
	nvmeibt_strlcpy(out_p->nodeUUID, nvmeibt_disk_segment_get_node_name(seg), sizeof(out_p->nodeUUID));
	out_p->type = TYPE_BOTH;
	out_p->pRaidIndex = actual_idx;
	out_p->pRaidTypeIndex = actual_idx;
	if (actual_idx != seg->from_config.idx_in_praid) {
		N_Ef(cvst3k3, "Surprise actual_idx=@INT idx_in_praid=@INT", actual_idx, seg->from_config.idx_in_praid);
	}
	out_p->allocationIndex = -1;
	nvmeibt_strlcpy(out_p->status, "normal", sizeof(out_p->status));
	return (out_p + sizeof(*out_p));
}

void* serialize_praid_to_nvmeibc(struct nvmeibc_praid_conf *out_p, struct nvmeibt_praid *praid, int praid_idx, int offset_of_segs)
{
	nvmeibt_strlcpy(out_p->uuid, nvmeibt_union_uuid_to_urn_uuid(&(praid->from_config.id)).str, min(sizeof(out_p->uuid), sizeof(struct nvmeibt_urn_uuid)));
	out_p->activated = 0;	// Probably unused
	out_p->numberOfMirrors = praid->from_config.redundancy;
	out_p->stripeIndex = praid_idx;			// praid->praid_mgmt.stripe_idx;
	out_p->dataBlocks = (praid->praid_mgmt.n_topo_segs - praid->from_config.redundancy);
	out_p->parityBlocks = 0;				// Maybe praid->from_config.redundancy;
	out_p->n_both_DP_segs = 0;
	//
	out_p->lockServer.type = praid->from_config.lock_scheme_type;
	out_p->lockServer.maxNOwners = (praid->from_config.redundancy + 1);
	out_p->lockServer.locksetShift = praid->praid_mgmt.lockset_shift;
	//
	out_p->n_segments = (uint32_t)praid->praid_mgmt.n_topo_segs;
	out_p->segments = (void *)(0LL + offset_of_segs);
	return (out_p + sizeof(*out_p));
}

void* serialize_chunk_to_nvmeibc(struct nvmeibc_chunk_conf *out_p, struct nvmeibt_chunk *chunk, int offset_of_praids)
{
	nvmeibt_strlcpy(out_p->uuid, chunk->urn_uuid.str, sizeof(out_p->uuid));
	out_p->vlbs = chunk->from_config.vlb_s;
	out_p->vlbe = chunk->from_config.vlb_e;
	out_p->stripeSize = (chunk->from_config.stripe_size * (chunk->praids[0]->praid_mgmt.n_topo_segs - chunk->praids[0]->from_config.redundancy ));	// Crazy, but be it
	out_p->stripeWidth = chunk->from_config.stripe_width;
	out_p->n_praids = chunk->n_praids;
	out_p->praids = (void *)(0LL + offset_of_praids);
	return (out_p + sizeof(*out_p));
}

void* serialize_blkdev_to_nvmeibc(struct nvmeibc_volume_conf *out_p, struct attach_detach_wq_entry *attach_detach_task,
								  char *vol_name, const union nvmeib_uuid *vol_uuid, int offset_of_chunks)
{
	struct nvmeibt_block_device 						*blkdev = attach_detach_task->vol;

	nvmeibt_strlcpy(out_p->uuid, nvmeibt_union_uuid_to_urn_uuid(vol_uuid).str, min(sizeof(out_p->uuid), sizeof(struct nvmeibt_urn_uuid)));
	nvmeibt_strlcpy(out_p->name, vol_name, sizeof(out_p->name));
	out_p->stripeWidth = blkdev->from_config.stripe_width;
//	out_p-> = blkdev->from_config.attr;
	out_p->blockSize = blkdev->from_config.blk_size_bytes;
	out_p->RAIDLevel = -1;	// (blkdev->chunks[0]->praids[0]->praid_mgmt.type ?) The client can calculate it based on other info
	out_p->type = attach_detach_task->vol_type;
	out_p->version = blkdev->from_config.version;
	out_p->blocks = blkdev->from_config.size_lblks;
//	out_p-> = blkdev->from_config.is_deprecated;
//	out_p-> = blkdev->from_config.attr.relative_rebuild_priority;
	out_p->version = blkdev->from_config.attr.version;
	out_p->numberOfMirrors = blkdev->chunks[0]->praids[0]->from_config.redundancy;
	out_p->stripeSize = blkdev->from_config.stripe_size;
	//
	// Reservation_mode is don't care for toma attach
	out_p->reservation.version = RESERVATION_MODE_IRRELEVANT;
	out_p->reservation.mode = NVMEIB_C_TO_M_VOLUME_ACTION_REQUEST_RC;
	out_p->reservation.preempt = NVMEIB_C_TO_M_VOLUME_PREEMPT_UNKNOWN;
	out_p->reservation.is_512B_IO_allowed = 0;
	//
	out_p->lockServer.type = blkdev->chunks[0]->praids[0]->from_config.lock_scheme_type;
	out_p->lockServer.maxNOwners = (blkdev->chunks[0]->praids[0]->from_config.redundancy + 1);
	out_p->lockServer.locksetShift = blkdev->chunks[0]->praids[0]->praid_mgmt.lockset_shift;
	//
	out_p->dataBlocks = (blkdev->chunks[0]->praids[0]->praid_mgmt.n_topo_segs - blkdev->chunks[0]->praids[0]->from_config.redundancy);
	out_p->parityBlocks = blkdev->chunks[0]->praids[0]->from_config.redundancy;
	out_p->sliceWidth = 1;													// GET FROM MGMT
	out_p->enableCrcCheck = blkdev->from_config.enableCrcCheck;
	out_p->use_debug_di = blkdev->from_config.use_debug_di;
	out_p->enableLocalReadOptimization = 0;									// GET FROM MGMT
	out_p->n_chunks = blkdev->n_chunks;
	out_p->chunks = (void *)(0LL + offset_of_chunks);
	return (out_p + sizeof(*out_p));
}

//#include "../clnt/nvmeibc_cc_api.h"
void *serialize_hdr_to_client_vol_config(struct nvmeib_mgmt_to_client_volume_configuration *out_p,
										 size_t offset_of_vol, int n_targets, size_t offset_of_targets, char *attach_token) {
	out_p->messageTypeVersion = -1; // meaningless for toma, will be filled with SUPPORTED_MCS_PROTOCOL_VERSION in the client code
	strlcpy(out_p->cli_unique_id, attach_token, sizeof(out_p->cli_unique_id));
	//
	out_p->attachmentsVersion = 0;
	//
	out_p->updateType = UPDATETYPE_TOMA_VOL_CONFIG_TO_LOCAL_CLNT;
	out_p->n_volumes = 1;
	out_p->volumes = (void *)offset_of_vol;
	out_p->n_targets = n_targets;
	out_p->targets = (void *)(0LL + offset_of_targets);
	return (out_p + sizeof(*out_p));
}

void *serialize_target_to_nvmeibc(struct nvmeibc_target_conf *out_p, struct nvmeibt_node *node, int offset_of_nics, int offset_of_disks, int n_disks)
{
	const struct nvmeibt_urn_uuid node_uuid = nvmeibt_union_uuid_to_urn_uuid(&node->from_config.id);

	strlcpy(out_p->node_id, nvmeibt_node_name(node), sizeof(out_p->node_id));
	strlcpy(out_p->uuid, node_uuid.str, min(sizeof(out_p->uuid), sizeof(node_uuid.str)));
	out_p->n_nics = node->n_nics;
	out_p->n_disks = n_disks;
	out_p->nics = (void *)(0LL + offset_of_nics);
	out_p->disks = (void *)(0LL + offset_of_disks);
	return (out_p + sizeof(*out_p));
}

void *serialize_nic_to_nvmeibc(struct nvmeibc_nic_conf *out_p, struct nvmeibt_nic *nic)
{
	size_t		sz = sizeof(nic->from_config.protocol);

	nvmeibt_strlcpy(out_p->nicID, nic->from_config.nicID, sizeof(out_p->nicID));
	snprintf(out_p->guid, sizeof(out_p->guid), "0x%s", nic->from_config.guid_str);
	out_p->pkey = (unsigned short)(nic->from_config.partition_key);
	out_p->protocol = (!strncmp(nic->from_config.protocol, "ROCE", sz)		?	PROTOCOL_ROCE :
					   !strncmp(nic->from_config.protocol, "IB", sz)		?	PROTOCOL_INFINIBAND :
					   !strncmp(nic->from_config.protocol, "TCP", sz)		?	PROTOCOL_TCP :
					   !strncmp(nic->from_config.protocol, "MULTI", sz)		?	PROTOCOL_MULTI :
																				PROTOCOL_UNKNOWN);
	return (out_p + sizeof(*out_p));
}

void *serialize_disk_to_nvmeibc(struct nvmeibc_disk_conf *out_p, struct nvmeibt_disk *disk)
{
	strlcpy(out_p->uuid, nvmeibt_disk_id_str(disk), sizeof(out_p->uuid));
	strlcpy(out_p->diskID, nvmeibt_disk_get_ldisk_id_str(disk), sizeof(out_p->diskID));
	out_p->blocks = -1ULL;	// disk->from_config.n_pblks;
	return (out_p + sizeof(*out_p));
}

size_t nvmeibt_recovery_serialize_nvmeibc_attach_config(struct attach_detach_wq_entry *attach_detach_task, void **serialized_buf_4k_aligned)
{
	struct nvmeibt_toma_to_local_client_msg 			*msg;
	struct nvmeibt_toma_to_local_client_attach_params	*attach_params;
	struct nvmeibt_block_device 						*vol = attach_detach_task->vol;
	void					*config_s;
	size_t					size;
	int						n_targets;
	struct nvmeibt_chunk	*chunk;
	struct nvmeibt_praid 	*praid;
	int						i, j, k;
	struct nvmeibt_node		*node;
	struct nvmeibt_disk		*disk;
	int						toma_to_local_client_msg_size;
	int						hdr_size;
	int						vol_size;
	int						chunks_size;
	int						praids_size = 0;
	int						segs_size = 0;
	int						targets_size = 0;
	int						nics_size = 0;
	int						disks_size = 0;
	int						offset_of_hdr;
	int						offset_of_vol;
	int						offset_of_this_chunk;
	int						offset_of_this_praid;
	int						offset_of_this_seg;
	int						offset_of_this_target;
	int						offset_of_this_nic;
	int						offset_of_this_disk;

	NFIN;
	// Calc size
	toma_to_local_client_msg_size = sizeof(*msg);
	hdr_size = sizeof(struct nvmeib_mgmt_to_client_volume_configuration);
	vol_size = sizeof(struct nvmeibc_volume_conf);
	chunks_size = sizeof(struct nvmeibc_chunk_conf) * vol->n_chunks;
	for (i = 0; i < vol->n_chunks; i++) {
		chunk = vol->chunks[i];
		praids_size += chunk->n_praids *sizeof(struct nvmeibc_praid_conf);
		for (j = 0; j < chunk->n_praids; j++) {
			praid = chunk->praids[j];
			segs_size += praid->praid_mgmt.n_topo_segs * sizeof(struct nvmeibc_segment_conf);
		}
	}
	// Targets, disks and NICS, we send all
	n_targets = NVMEIBT_HASH_N_OBJS(&(nvmeibt_global_get_global()->nodes_hash));
	targets_size = n_targets * sizeof(struct nvmeibc_target_conf);
	XHASHTABLE_FOR_EACH_SAFE(node, &(nvmeibt_global_get_global()->nodes_hash)) {
		nics_size += (node->n_nics * sizeof(struct nvmeibc_nic_conf));
		disks_size += (node->n_disks_config * sizeof(struct nvmeibc_disk_conf));
	}
	size = toma_to_local_client_msg_size + hdr_size + vol_size + chunks_size + praids_size + segs_size + targets_size + nics_size + disks_size;
	*serialized_buf_4k_aligned = NNVMEIBT_BM_ALIGNED_ALLOC(inwnk2l, PAGE_SIZE, size);
	//
	offset_of_hdr = 0;
	offset_of_vol = offset_of_hdr + hdr_size;
	offset_of_this_chunk = offset_of_vol + vol_size;
	offset_of_this_praid = offset_of_this_chunk + chunks_size;
	offset_of_this_seg = offset_of_this_praid + praids_size;
	offset_of_this_target = offset_of_this_seg + segs_size;
	offset_of_this_nic = offset_of_this_target + targets_size;
	offset_of_this_disk = offset_of_this_nic + nics_size;
	//
	msg = (struct nvmeibt_toma_to_local_client_msg *)(*serialized_buf_4k_aligned);
	attach_params =&(msg->payload.attach_params);
	nvmeibt_strlcpy(attach_params->vol_name, vol->from_config.client_blkdev_name, sizeof(attach_params->vol_name));
	attach_params->vol_uuid = vol->from_config.id;
	attach_params->msg_type = TOMA_TO_LOCAL_CLNT_MSG_TYPE_ATTACH;
	attach_params->serialized_config_len = size;
	//
	config_s = msg->data;
	N_Tf(b6fmubs, "local_cl msg=@PTR attach_params=@PTR data=@PTR attach_params->msg_type=@INT attach_params->vol_name=@STR",
		msg, attach_params, config_s, attach_params->msg_type, attach_params->vol_name);
	serialize_hdr_to_client_vol_config(config_s + offset_of_hdr, offset_of_vol, n_targets, offset_of_this_target, attach_detach_task->attach_token);

	// Serialize the vol
	serialize_blkdev_to_nvmeibc(config_s + offset_of_vol, attach_detach_task, attach_params->vol_name, &(attach_params->vol_uuid), offset_of_this_chunk);
	for (i = 0; i < vol->n_chunks; i++) {
		chunk = vol->chunks[i];
		serialize_chunk_to_nvmeibc(config_s + offset_of_this_chunk, chunk, offset_of_this_praid);
		offset_of_this_chunk += sizeof(struct nvmeibc_chunk_conf);
		for (j = 0; j < chunk->n_praids; j++) {
			praid = chunk->praids[j];
			serialize_praid_to_nvmeibc(config_s + offset_of_this_praid, praid, j, offset_of_this_seg);
			offset_of_this_praid += sizeof(struct nvmeibc_praid_conf);
			for (k = 0; k < praid->praid_mgmt.n_topo_segs; k++) {
				struct nvmeibt_disk_segment *seg = praid->praid_mgmt.topo_segs[k];
				serialize_seg_to_nvmeibc(config_s + offset_of_this_seg, seg, seg->from_config.idx_in_praid);
				disk = seg->seg_mgmt.its_disk;
				if (!disk) {
					N_Wf(y778o2s, "seg=@UUID_8 has no disk yet, don't attach the volume", nvmeibt_seg_UUID_8(seg));
					attach_detach_task->fail_attach_due_to_missing_disk = 1;
					goto out;
				}
				if (!disk->is_needed_for_vol) {
					disk->is_needed_for_vol = 1;
					disk->its_node_config->n_disks_needed_for_vol++;
				}
				offset_of_this_seg += sizeof(struct nvmeibc_segment_conf);
			}
		}
	}

	// Serialize the HW
	XHASHTABLE_FOR_EACH_SAFE(node, &(nvmeibt_global_get_global()->nodes_hash)) {
		serialize_target_to_nvmeibc(config_s + offset_of_this_target, node, offset_of_this_nic, offset_of_this_disk, node->n_disks_needed_for_vol);
		offset_of_this_target += sizeof(struct nvmeibc_target_conf);
		for (i = 0; i < node->n_nics; i++) {
			serialize_nic_to_nvmeibc(config_s + offset_of_this_nic, node->nics[i]);
			offset_of_this_nic += sizeof(struct nvmeibc_nic_conf);
		}
		if (node->n_disks_needed_for_vol) {
			for (i = 0; i < node->n_disks_config; i++) {
				disk = node->disks_config[i];
				if (disk->is_needed_for_vol) {
					serialize_disk_to_nvmeibc(config_s + offset_of_this_disk, disk);
					offset_of_this_disk += sizeof(struct nvmeibc_disk_conf);
					disk->is_needed_for_vol = 0;
				}
			}
			node->n_disks_needed_for_vol = 0;
		}
	}

out:
	NFOUT;
	return size;
}

size_t nvmeibt_recovery_serialize_detach_msg(struct attach_detach_wq_entry *attach_detach_task, void **serialized_buf_4k_aligned)
{
	struct nvmeibt_toma_to_local_client_msg 			*msg;
	struct nvmeibt_toma_to_local_client_attach_params	*attach_params;
	struct nvmeibt_block_device 						*vol = attach_detach_task->vol;
	struct detach_message		*config_s;
	size_t						size;

	size = sizeof(*msg) + sizeof(*config_s) + sizeof(config_s->volumes[0]);
	*serialized_buf_4k_aligned = NNVMEIBT_BM_ALIGNED_ALLOC(txbwu4h, PAGE_SIZE, size);
	msg = (struct nvmeibt_toma_to_local_client_msg *)(*serialized_buf_4k_aligned);
	attach_params = &(msg->payload.attach_params);
	nvmeibt_strlcpy(attach_params->vol_name, vol->from_config.client_blkdev_name, sizeof(attach_params->vol_name));
	attach_params->vol_uuid = vol->from_config.id;
	attach_params->msg_type = TOMA_TO_LOCAL_CLNT_MSG_TYPE_DETACH;
	attach_params->serialized_config_len = size;

	config_s = (struct detach_message *)(msg->data);
	config_s->messageTypeVersion = -1;
	config_s->attachmentsVersion = -1;
	config_s->n_volumes = 1;

	config_s->volumes = (struct volume_detach_payload *)(config_s + 1);
	config_s->volumes[0].force = (attach_detach_task->vol_type == RECOVERER_VOLUME) ? 0 : 1;
	config_s->volumes[0].upgrade = 0;
	nvmeibt_strlcpy(config_s->volumes[0].name, nvmeibt_blkdev_name(vol), sizeof(config_s->volumes[0].name));
	nvmeibt_strlcpy(config_s->volumes[0].uuid, nvmeibt_union_uuid_to_urn_uuid(&attach_params->vol_uuid).str, min(sizeof(config_s->volumes[0].uuid), sizeof(struct nvmeibt_urn_uuid)));
	config_s->volumes = (struct volume_detach_payload *)(sizeof(*config_s)); // offset
	return size;
}

/*************************  Attach / Detach WQ     ****************************/

// Upon starting a recovery we add an attach wq entry
// Usually, it will happen quickly, and we will get a REGISTER req
// When done, we add a detach entry
// If n_recoveries_needing_hidden_attach(praid) we do not send a detach req

static XDLIST_DECLARE(, struct attach_detach_wq_entry, attach_detach_list_link) attach_detach_list = XDLIST_INIT(attach_detach_list);
struct nvmeibt_wq *attach_detach_wq;

static void attach_detach_wq_init(void) {
	NFIN;
	attach_detach_wq = nvmeibt_wq_create("attach_detach");
	NFOUT;
}

static void attach_detach_wq_exit(void)
{
	NFIN;
	if (attach_detach_wq) {
		nvmeibt_wq_flush(attach_detach_wq);
		nvmeibt_wq_destroy(attach_detach_wq);
		attach_detach_wq = NULL;
	}
	NFOUT;
}

static void attach_detach_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct attach_detach_wq_entry		*attach_detach_task;

	NFIN;
	attach_detach_task = container_of(wq_entry, struct attach_detach_wq_entry, wq_entry);
	N_Tf(hqilaq0, "vol=@STR cmd=@STR", attach_detach_task->vol->from_config.client_blkdev_name, (attach_detach_task->attach_cmd == RECOVERY_ATTACH_CMD_ATTACH ? "Attach" : "Detach"));
	if (attach_detach_task->finalize_specific)
		attach_detach_task->finalize_specific(attach_detach_task->rv, attach_detach_task->vol, attach_detach_task->attach_cmd);
	NFOUT;
}

static void attach_detach_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct attach_detach_wq_entry		*attach_detach_task;

	NFIN;
	attach_detach_task = container_of(wq_entry, struct attach_detach_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(by74j2i, attach_detach_task);
	NFOUT;
}

void nvmeibt_recovery_buf_to_local_clnt_was_copied_and_can_be_freed(void *ctx, int ok, struct nvmeib_nl_uk_comm_rep *rep)
{
	void	*serialized_blkdev_for_clnt_4k_aligned = ctx;

	(void)ok;
	(void)rep;
	NNVMEIBT_BM_FREE(7sg3zmu, serialized_blkdev_for_clnt_4k_aligned);
}

static int nvmeibt_toma_send_recovery_attach_msg_to_local_clnt(struct attach_detach_wq_entry *attach_detach_task)
{
	struct km_comm_msg_hdr			*nl_msg;
	struct nvmeib_nl_msg_from_toma	*toma_msg;
	int								rv = 0;

	NFIN;
	nl_msg = NNVMEIBT_BM_CALLOC(84j2kua, sizeof(*nl_msg) + sizeof(*toma_msg));	// Copy into it
	nl_msg->opcode = csc_local_client;
	nl_msg->ctx = attach_detach_task->serialized_blkdev_for_clnt_4k_aligned;
	nl_msg->len = sizeof(*toma_msg);
	nl_msg->on_done = nvmeibt_recovery_buf_to_local_clnt_was_copied_and_can_be_freed;
	toma_msg = (typeof(toma_msg))nl_msg->data;
	//
	toma_msg->payload.toma_client.data = attach_detach_task->serialized_blkdev_for_clnt_4k_aligned;
	toma_msg->payload.toma_client.n_pages = (unsigned)divroundup(attach_detach_task->serialized_blkdev_for_clnt_len, PAGE_SIZE);
	toma_msg->payload.toma_client.copy = 1;
	N_Tf(56wjhgqkgyed, "opcode=@INT nl_msg->len=@INT n_pages=@UINT data=@PTR pid=@LLU",
		 nl_msg->opcode, nl_msg->len, toma_msg->payload.toma_client.n_pages, toma_msg->payload.toma_client.data, getpid());
	if (nvmeibt_send_msg_to_srv(nl_msg) != 0) {
		N_Ef(03ndjzb, "Unable to send attach msg to local_clnt using netlink vol=@STR", attach_detach_task->vol->from_config.client_blkdev_name);
		rv = -1;
	}
	NNVMEIBT_BM_FREE(heu4h3y, nl_msg);
	NFOUT;
	return rv;
}

static void detach_shadow_vol_for_encryption_finalize(int attach_detact_rv, struct nvmeibt_block_device *shadow_vol,
													  __attribute__((__unused__)) enum RECOVERY_ATTACH_CMD attach_cmd);

static void encrypt_delay(bool can_use_bin_traces)
{
	if (can_use_bin_traces) N_Tf(bhqw833, "ENCRYPT_DELAY_START");
	sleep(20);
	if (can_use_bin_traces) N_Tf(bhqw834, "ENCRYPT_DELAY_END");
}

static void attach_detach_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct attach_detach_wq_entry		*attach_detach_task;
	int									i;
	char								blkdev_path[PATH_MAX];
	struct stat							blkdev_stat;

	NFIN;
	attach_detach_task = container_of(wq_entry, struct attach_detach_wq_entry, wq_entry);
	if (attach_detach_task->fail_attach_due_to_missing_disk) {
		attach_detach_task->rv = -1;
		goto out;
	}
	attach_detach_task->rv = nvmeibt_toma_send_recovery_attach_msg_to_local_clnt(attach_detach_task);
	if (attach_detach_task->finalize_specific == detach_shadow_vol_for_encryption_finalize) {	// detach for encrypt
		if (nvmeibt_encrypt_delay == ENCRYPT_DELAY_BEFORE_DETACH)
			encrypt_delay(true);
		snprintf(blkdev_path, sizeof(blkdev_path), "/dev/nvmesh/%s", nvmeibt_blkdev_name(attach_detach_task->vol));
		for (i = 0; i < 100; i++) { // Wait maximum 10 sec
			if (stat(blkdev_path, &blkdev_stat) != 0)
				break;
			N_Tf(idw8wkr, "Awaiting @STR to detach", blkdev_path);
			nanosleep(&(struct timespec){ 0, 100 * 1000 * 1000 }, NULL);	// 100ms
		}
		if (i == 100)
			N_ETf(idwu6kr, "failed to detach @STR", blkdev_path);
	}

out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
	NFOUT;
}

static struct nvmeibt_block_device *create_vol_and_chunk_for_single_praid(struct nvmeibt_praid *praid)
{
	struct nvmeibt_block_device						*new_vol = NULL;
	struct nvmeibt_block_device						*origin_vol;
	struct nvmeibt_chunk							*new_chunk;
	struct nvmeibt_chunk							*origin_chunk;
	struct nvmeibt_seg_mgmt							*seg_mgmt;
	long long										blkdev_n_4kblk;

	origin_chunk = nvmeibt_praid_get_chunk(praid);
	origin_vol = nvmeibt_chunk_get_blkdev(origin_chunk);
	if (NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(origin_vol)) {
		goto out;
	}

	new_chunk = NNVMEIBT_TOMA_CALLOC(i7880nw, 1, sizeof(struct nvmeibt_chunk));
	//new_chunk->from_config = origin_chunk->from_config;
	new_chunk->from_config.id = praid->from_config.id;
	seg_mgmt = &(praid->praid_follower.applied_praid_lot.topo_seg_lots[0]->my_seg->seg_mgmt);
	new_chunk->from_config.vlb_s = 0;
	blkdev_n_4kblk = (praid->praid_mgmt.n_topo_segs - praid->from_config.redundancy) *
		(seg_mgmt->lb_e - seg_mgmt->lb_s + 1);
	new_chunk->from_config.vlb_e = blkdev_n_4kblk - 1;
	new_chunk->from_config.stripe_width = 1;
	new_chunk->from_config.stripe_size = origin_chunk->from_config.stripe_size;
	new_chunk->praids[0] = praid;
	new_chunk->n_praids = 1;

	new_vol = NNVMEIBT_TOMA_CALLOC(i9990nw, 1, sizeof(struct nvmeibt_block_device));
	new_vol->from_config = origin_vol->from_config;
	nvmeibt_strlcpy(new_vol->from_config.client_blkdev_name,
					praid->vol_name_for_recovery,
					sizeof(new_vol->from_config.client_blkdev_name));
	new_vol->from_config.id = praid->vol_uuid_for_recovery;
	new_vol->from_config.stripe_width = 1;
	new_vol->from_config.size_lblks = blkdev_n_4kblk;
	new_vol->chunks[0] = new_chunk;
	new_vol->n_chunks = 1;

out:
	return new_vol;
}

static struct nvmeibt_block_device *create_shadow_vol(struct nvmeibt_block_device *origin_vol, char *shadow_vol_name)
{
	struct nvmeibt_block_device						*new_vol = NULL;
	int												i;

	new_vol = NNVMEIBT_TOMA_CALLOC(i98q0nw, 1, sizeof(struct nvmeibt_block_device));
	new_vol->from_config = origin_vol->from_config;
	memcpy(new_vol->from_config.client_blkdev_name, shadow_vol_name, sizeof(new_vol->from_config.client_blkdev_name));
	new_vol->from_config.id = origin_vol->chunks[0]->from_config.id;
	for (i = 0; i < origin_vol->n_chunks; i++) {
		new_vol->chunks[i] = origin_vol->chunks[i];
	}
	new_vol->n_chunks = origin_vol->n_chunks;

	return new_vol;
}

static void launch_attach_detach_task(struct nvmeibt_block_device *vol,
									  attach_detach_end_cb_t attach_detach_finalize_specific,
									  char *attach_token,
									  enum nvmeibc_config_volume_type vol_type,
									  enum RECOVERY_ATTACH_CMD attach_cmd)
{
	struct attach_detach_wq_entry		*attach_detach_task;

	NFIN;
	attach_detach_task = NNVMEIBT_BM_CALLOC(2irgyxo, sizeof(*attach_detach_task));
	attach_detach_task->wq_entry.type = "ATTACH_DETACH";
	attach_detach_task->wq_entry.execute = attach_detach_wrapper;
	attach_detach_task->wq_entry.finalize = attach_detach_finalize;
	attach_detach_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	attach_detach_task->wq_entry.free = attach_detach_freer;
	attach_detach_task->vol = vol;
	attach_detach_task->finalize_specific = attach_detach_finalize_specific;
	attach_detach_task->attach_cmd = attach_cmd;
	attach_detach_task->vol_type = vol_type;
	strlcpy(attach_detach_task->attach_token, attach_token, sizeof(attach_detach_task->attach_token));
	if (attach_cmd == RECOVERY_ATTACH_CMD_ATTACH) {
		attach_detach_task->serialized_blkdev_for_clnt_len =
			nvmeibt_recovery_serialize_nvmeibc_attach_config(attach_detach_task, &attach_detach_task->serialized_blkdev_for_clnt_4k_aligned);
	} else {
		attach_detach_task->serialized_blkdev_for_clnt_len =
			nvmeibt_recovery_serialize_detach_msg(attach_detach_task, &attach_detach_task->serialized_blkdev_for_clnt_4k_aligned);
	}
	nvmeibt_wq_addw(attach_detach_wq, &(attach_detach_task->wq_entry));
	NFOUT;
}

static void attach_detach_praid_for_recovery_finalize(int attach_detact_rv, struct nvmeibt_block_device *vol,
													  __attribute__((__unused__)) enum RECOVERY_ATTACH_CMD attach_cmd)
{
	if (attach_detact_rv)
		N_Wf(soo0636, "vol=@STR attach/detach failed", vol->from_config.client_blkdev_name);

	NNVMEIBT_TOMA_FREE(hu98i22, vol->chunks[0]);
	NNVMEIBT_TOMA_FREE(hu98i23, vol);
}

static void encrypt_action_end_process(struct nvmeibt_block_device *shadow_vol)
{
	struct nvmeibt_encrypt_params					*encrypt_params = shadow_vol->encrypt_params;
	struct nvmeibt_block_device						*origin_vol = encrypt_params->origin_vol;

	if (origin_vol->encrypt_params == NULL) { // The vol was deleted
		NNVMEIBT_TOMA_FREE(u8223nd, origin_vol);
	} else {
		origin_vol->encrypt_params = NULL;
	}

	nvmeibt_kafka_mark_CMD_k_msg_for_kafka_commit_by_toma(encrypt_params->kafka_offset);

	NNVMEIBT_TOMA_FREE(u8872bf, encrypt_params);
	NNVMEIBT_TOMA_FREE(u88721s, shadow_vol);
}

static void attach_shadow_vol_for_encryption_finalize(int attach_detact_rv, struct nvmeibt_block_device *shadow_vol,
													  __attribute__((__unused__)) enum RECOVERY_ATTACH_CMD attach_cmd) {
	struct nvmeibt_encrypt_params					*ep = shadow_vol->encrypt_params;
	struct run_exec_on_blkdev_ctx					*exec_ctx = &ep->exec_ctx;
	NFIN;
	if (attach_detact_rv) {
		const struct nvmeibt_block_device *vol = ep->origin_vol;
		N_Wf(soo01x6, "vol=@STR attach failed", shadow_vol->from_config.client_blkdev_name);
		nvmeibt_kafka_send_encrypt_cmd_response(vol->from_config.client_blkdev_name, &vol->urn_uuid, ep->encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 1, "TOMA vol attach error");
		encrypt_action_end_process(shadow_vol);
		goto out;
	}
	exec_ctx->run_exec_on_blkdev_cb_func = nvmeibt_detach_vol_for_encryption;
	exec_ctx->child_stdout_buf = NNVMEIBT_STR_ALLOC(vcdsghf);
	exec_ctx->child_stderr_buf = NNVMEIBT_STR_ALLOC(ik50eln);
	exec_ctx->timeout_ms = 100000;
#if 1
	nvmeibt_run_exec_on_blkdev(exec_ctx);
#else
	exec_ctx->toma_rv = 0;
	exec_ctx->exec_rv = 0;
	nvmeibt_detach_vol_for_encryption(exec_ctx);
#endif
out:
	NFOUT;
}

static void attach_detach_shadow_vol_finalize(int attach_detact_rv, struct nvmeibt_block_device *shadow_vol,
											  enum RECOVERY_ATTACH_CMD attach_cmd)
{
	char					attach_str[] = "attach", detach_str[] = "detach", *out_str;

	out_str = (attach_cmd == RECOVERY_ATTACH_CMD_ATTACH) ? attach_str : detach_str;

	if (attach_detact_rv) {
		N_Wf(soei3x6, "vol=@STR @STR failed", shadow_vol->from_config.client_blkdev_name, out_str);
		// fprintf(stderr, "%s %s failed\n", shadow_vol->from_config.client_blkdev_name, out_str);
	} else {
		N_Tf(s1971x6, "vol=@STR @STR completed", shadow_vol->from_config.client_blkdev_name, out_str);
		// fprintf(stderr, "%s %s completed\n", shadow_vol->from_config.client_blkdev_name, out_str);
	}
	NNVMEIBT_TOMA_FREE(u94d21s, shadow_vol);
}

static void sanitize_str(struct nvmeibt_Str **old_buf_p)
{
	int									old_idx, new_idx, old_str_len;
	struct nvmeibt_Str					*new_buf;
	char								*old_str;
	char								*new_str;

	// In the new_str all control charecters must be replaced by ' ' and LineFeed by '\n'
	new_buf = NNVMEIBT_STR_ALLOC(b3b3h91);
	old_str_len = nvmeibt_Str_strlen(*old_buf_p);
	old_str = (char *)nvmeibt_Str_str(*old_buf_p);
	NNVMEIBT_STR_RESIZE_BUF(vs29sje, new_buf, old_str_len * 2 + 1);
	new_str = (char *)nvmeibt_Str_str(new_buf);
	new_idx = 0;

	for (old_idx = 0; old_idx < old_str_len; old_idx++) {
		if (old_str[old_idx] >= ' ') {
			new_str[new_idx++] = old_str[old_idx];
		} else if (old_str[old_idx] == '\n') {
			new_str[new_idx++] = '\\';
			new_str[new_idx++] = 'n';
		} else {
			new_str[new_idx++] = ' ';
		}
	}
	// Remove the "\n" at the end of string if any
	if ((new_idx >= 2) && (new_str[new_idx - 2] == '\\') && (new_str[new_idx - 1] == 'n'))
		new_idx -= 2;

	new_str[new_idx] = '\0';
	new_buf->str_len = new_idx;

	NNVMEIBT_STR_FREE(u73d812, *old_buf_p);
	*old_buf_p = new_buf;
}

	// Cryptsetup error codes according to Chat GPT
#define CRYPTSETUP_ERR_SUCCESS						0
#define CRYPTSETUP_ERR_WRONG_PARAMS					1
#define CRYPTSETUP_ERR_NO_PERMISSION				2
#define CRYPTSETUP_ERR_OUT_OF_MEMORY				3
#define CRYPTSETUP_ERR_WRONG_DEVICE					4
#define CRYPTSETUP_ERR_DEVICE_ALREADY_MAPPED		5
#define CRYPTSETUP_ERR_NO_KEY_AVAILABLE				6
#define CRYPTSETUP_ERR_WRONG_KEY					7
#define CRYPTSETUP_ERR_DEVICE_IS_BUSY				8
#define CRYPTSETUP_ERR_WRONG_CRYPTO_PARAMS			9
#define CRYPTSETUP_ERR_IO_ERROR						10
#define CRYPTSETUP_ERR_DEVICE_ALREADY_EXISTS		11
#define CRYPTSETUP_ERR_TIMEOUT						12
#define CRYPTSETUP_ERR_NOT_SUPPORTED				13
#define CRYPTSETUP_ERR_UNKNOWN						14

static bool is_cryptsetup_error_retryable(int err)
{
	return ((err == CRYPTSETUP_ERR_OUT_OF_MEMORY) ||
			(err == CRYPTSETUP_ERR_DEVICE_ALREADY_MAPPED) ||
			(err == CRYPTSETUP_ERR_DEVICE_IS_BUSY) ||
			(err == CRYPTSETUP_ERR_IO_ERROR) ||
			(err == CRYPTSETUP_ERR_TIMEOUT));
}

static void detach_shadow_vol_for_encryption_finalize(int attach_detact_rv, struct nvmeibt_block_device *shadow_vol,
													  __attribute__((__unused__)) enum RECOVERY_ATTACH_CMD attach_cmd) {
	struct nvmeibt_encrypt_params					*ep = shadow_vol->encrypt_params;
	const struct nvmeibt_block_device				*vol = ep->origin_vol;
	struct run_exec_on_blkdev_ctx					*exec_ctx = &ep->exec_ctx;
	const char *bdev_name = vol->from_config.client_blkdev_name;
	if (attach_detact_rv)
		N_Wf(su801x6, "vol=@STR detach failed", shadow_vol->from_config.client_blkdev_name);

	NVMEIBT_LONG_TRACE_WRAPPER(socngw9, "STDOUT", nvmeibt_Str_str(exec_ctx->child_stdout_buf), nvmeibt_Str_strlen(exec_ctx->child_stdout_buf));
	NVMEIBT_LONG_TRACE_WRAPPER(1c7sjir, "STDERR", nvmeibt_Str_str(exec_ctx->child_stderr_buf), nvmeibt_Str_strlen(exec_ctx->child_stderr_buf));
	sanitize_str(&exec_ctx->child_stdout_buf);
	sanitize_str(&exec_ctx->child_stderr_buf);
	if (exec_ctx->toma_rv)
		nvmeibt_kafka_send_encrypt_cmd_response(bdev_name, &vol->urn_uuid, ep->encrypt_idx, ENCRYPT_CMD_RESPONSE_TOMA_ERR, 1, nvmeibt_Str_str(exec_ctx->child_stderr_buf));
	else if (exec_ctx->exec_rv)
		nvmeibt_kafka_send_encrypt_cmd_response(bdev_name, &vol->urn_uuid, ep->encrypt_idx, ENCRYPT_CMD_RESPONSE_CMD_ERR, is_cryptsetup_error_retryable(exec_ctx->exec_rv), nvmeibt_Str_str(exec_ctx->child_stderr_buf));
	else
		nvmeibt_kafka_send_encrypt_cmd_response(bdev_name, &vol->urn_uuid, ep->encrypt_idx, ENCRYPT_CMD_RESPONSE_SUCCESS, 0, "");

	NNVMEIBT_STR_FREE(vwi43k3, exec_ctx->child_stdout_buf);
	NNVMEIBT_STR_FREE(w9ak20m, exec_ctx->child_stderr_buf);
	if (nvmeibt_encrypt_delay == ENCRYPT_DELAY_BEFORE_COMMIT) {
		sleep(1);
		N_Ef(wu8334n, "ENCRYPT DEBUG");
		nvmeibt_abort(ES_FATAL);
	}
	encrypt_action_end_process(shadow_vol);
}

static void attach_detach_praid_for_recovery(struct nvmeibt_praid *praid, enum RECOVERY_ATTACH_CMD attach_cmd)
{
	struct nvmeibt_block_device						*vol;

	NFIN;
	if (attach_cmd == RECOVERY_ATTACH_CMD_DETACH) {
		if (nvmeibt_praid_get_n_recoveries_needing_hidden_attach(praid)) {
			N_Tf(vs92ol0, "Skipping DETACH vol=@STR n_recoveries_needing_hidden_attach=@INT",
				 praid->vol_name_for_recovery, nvmeibt_praid_get_n_recoveries_needing_hidden_attach(praid));
			goto out;
		} else {
			N_Tf(rvushoq, "Here we could add the detach cmd delayed, in case we encounter a new attach soon. Probably not worth the effort");
		}
	}
	vol = create_vol_and_chunk_for_single_praid(praid);
	if (vol)
		launch_attach_detach_task(vol, attach_detach_praid_for_recovery_finalize,
								  MAGIC_RECOVR_ATTACH_TOKEN, RECOVERER_VOLUME, attach_cmd);
out:
	NFOUT;
}

void nvmeibt_attach_vol_for_encryption(struct nvmeibt_block_device *vol, char *shadow_vol_name, struct nvmeibt_encrypt_params *encrypt_params)
{
	struct nvmeibt_block_device						*shadow_vol;

	N_Tf(jure821w,"");
	shadow_vol = create_shadow_vol(vol, shadow_vol_name);
	encrypt_params->exec_ctx.blkdev = shadow_vol;
	encrypt_params->origin_vol = vol;
	shadow_vol->encrypt_params = encrypt_params;
	launch_attach_detach_task(shadow_vol, attach_shadow_vol_for_encryption_finalize,
							  MAGIC_CONFIG_SHADOW_TOKEN, NORMAL_VOLUME, RECOVERY_ATTACH_CMD_ATTACH);
}

void nvmeibt_detach_vol_for_encryption(struct run_exec_on_blkdev_ctx *exec_ctx)
{
	// struct nvmeibt_encrypt_params					*encrypt_params;

	// encrypt_params = container_of(exec_ctx, struct nvmeibt_encrypt_params, exec_ctx);
	N_Tf(ja7e821w,"");
	launch_attach_detach_task(exec_ctx->blkdev, detach_shadow_vol_for_encryption_finalize,
							  MAGIC_CONFIG_SHADOW_TOKEN, NORMAL_VOLUME, RECOVERY_ATTACH_CMD_DETACH);
}

void nvmeibt_attach_detach_shadow_vol(char *origin_vol_name, bool is_attach, struct nvmeibt_Str *out)
{
	struct nvmeibt_block_device							*shadow_vol = NULL;
	struct nvmeibt_block_device							*origin_vol = NULL;
	struct nvmeibt_block_device							*vol;
	char												shadow_vol_name[32];
	enum RECOVERY_ATTACH_CMD							attach_cmd;

	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(vol, &nvmeibt_global_get_global()->block_devices_hash) {
		if(!strcmp(vol->from_config.client_blkdev_name, origin_vol_name)) {
			origin_vol = vol;
			break;
		}
	}
	if (!origin_vol) {
		nvmeibt_Str_sprintf(out, "Volume %s not found\n", origin_vol_name);
	} else {
		attach_cmd = is_attach ? RECOVERY_ATTACH_CMD_ATTACH : RECOVERY_ATTACH_CMD_DETACH;
		shadow_vol_name[0] = 'd';
		shadow_vol_name[1] = '_';
		nvmeibt_strlcpy(shadow_vol_name + 2, origin_vol_name, sizeof(shadow_vol_name) - 2);
		nvmeibt_Str_sprintf(out, "Shadow volume %s %s", shadow_vol_name, is_attach ? "attached" : "detached");
		shadow_vol = create_shadow_vol(origin_vol, shadow_vol_name);
		launch_attach_detach_task(shadow_vol, attach_detach_shadow_vol_finalize,
								  MAGIC_CONFIG_SHADOW_TOKEN, NORMAL_VOLUME, attach_cmd);
	}
	NFOUT;
}

/********************   End Attach / Detach WQ     ****************************/
/****************        Begin run_exec_on_blkdev WQ       ********************/

struct run_exec_on_blkdev_wq_entry {
	struct nvmeibt_wq_entry 		wq_entry;
	struct run_exec_on_blkdev_ctx	*run_exec_on_blkdev_ctx;
};

static int waitpid_with_timeout(pid_t pid, int timeout_ms, char *exec_str)
{
	pid_t				returned_pid;
	int					rv = EXEC_ERR_EXTERNAL;
	int					waitpid_status;
	struct timespec		end_timespec;
	struct timespec		now;

	NFIN;
	if (pid == 0) {
		N_Ef(5nwieks, "pid=0");
		rv = 0;		// Do not kill this pid
		goto out;
	}

	getnstimeofday(&end_timespec);
	timespec_update_by_a_few_nsec(&end_timespec, MSEC_TO_NSEC(timeout_ms));
	while (1) {
		returned_pid = waitpid(pid, &waitpid_status, WNOHANG);
		if (returned_pid == pid)
			break;

		if (returned_pid == 0) {
			getnstimeofday(&now);
			if (timespec_lt(now, end_timespec)) {
				N_Tf(vauk39s, "0=waitpid(), Retrying");
				nanosleep(&(struct timespec){0, MSEC_TO_NSEC(100)}, NULL); // 100ms
				continue;
			}
			N_Wf(wncmr44, "'@STR' Timeout occurred", exec_str);
			rv = EXEC_ERR_TIMEOUT;
			goto out;
		}
		N_Ef(3xdnrto, "'@STR' @INT=waitpid(@INT) @AUTO_ERRNO", exec_str, (int)returned_pid, (int)pid);
		goto out;
	}
	if (WIFEXITED(waitpid_status)) {
		N_Tf(cbwu59o, "'@STR' exited, status=@INT", exec_str, (int8_t)WEXITSTATUS(waitpid_status));	// The exit(xxx) of the child
		rv = WEXITSTATUS(waitpid_status);
	} else if (WIFSIGNALED(waitpid_status)) {
		N_Ef(kskt4j4, "'@STR' killed by signal @INT", exec_str, WTERMSIG(waitpid_status));
	} else if (WIFSTOPPED(waitpid_status)) {
		N_Ef(0b1vday, "'@STR' stopped by signal @INT", exec_str, WSTOPSIG(waitpid_status));
	} else if (WIFCONTINUED(waitpid_status)) {
		N_Ef(xmfhvpw, "'@STR' continued", exec_str);
	}
out:
	if (rv < 0) {
		N_Tf(5us8iiw, "kill -9 @INT (@STR)", (int)pid, exec_str);
		kill(-pid, SIGKILL);		// That's what we can do, // -pid means pgid=pid. In our case, it means include its descendants
		nanosleep(&(struct timespec){1, MSEC_TO_NSEC(200)}, NULL);	// 1.2 sec
		returned_pid = waitpid(pid, &waitpid_status, WNOHANG);	// collect it
		N_Tf(vyejd94, "'@STR' @INT=waitpid(@INT) @AUTO_ERRNO", exec_str, (int)returned_pid, (int)pid);
	}
	NFOUT;
	return rv;
}

static void cleanup_passphrase_dir_leftover_files(void)
{
	if (strlen(PASSPHRASE_DIR_NAME) > 10 && strstr(PASSPHRASE_DIR_NAME, "toma")) {		// Avoid accidents
		system("rm -rf " PASSPHRASE_DIR_NAME);
	} else {
		N_Ef(92n4jwi, "PASSPHRASE_DIR_NAME=@STR", PASSPHRASE_DIR_NAME);
	}
}

static int write_passphrase_to_file(char *passphrase, char *file_name)
{
	int 		fd = -1;
	int			rv = -1;

	NFIN;
	if (!passphrase[0]) {
		rv = 0;
		goto out;
	}
	if (unlink(file_name) && errno != ENOENT) {
		N_ETf(fgvhakw, "Error unlink(@STR) @AUTO_ERRNO", file_name);
		goto out;
	}
	fd = NNVMEIBT_OPEN(unrgsk5, file_name, O_CREAT | O_WRONLY | O_TRUNC, 0700);
	if (fd < 0) {
		N_ETf(vshg982, "Error open(@STR) @AUTO_ERRNO", file_name);
		goto out;
	}
	if (NNVMEIBT_PWRITE(xcfahw8, fd, passphrase, strnlen(passphrase, PASSPHRASE_MAX_LEN), 0, 0) < 0) {
		goto out;
	}
	rv = 0;
out:
	NNVMEIBT_CLOSE(usnqjt0, fd);
	NFOUT;
	return rv;
}

static uint64_t get_nvmesh_vol_size(char *vol_path)	// From DHS
{
	int				fd = -1;
	uint64_t		size = 0;
	int				rv0;
	int				rv1;
	struct stat		sb;

	fd = open(vol_path, O_RDONLY | O_LARGEFILE);
	if (fd < 0) {
		N_Wf(ctgycf4, "Failed to open @STR @AUTO_ERRNO", vol_path);
		goto out;
	}
	rv0 = fstat(fd, &sb);
	if (rv0 < 0) {
		N_Wf(0cm4hyw, "fstat(@STR)=@INT @AUTO_ERRNO", vol_path, rv0);
		goto out;
	}
	if (sb.st_size) {
		size = sb.st_size;
	} else { // Note: sometimes (sb.st_size == 0), so we need the ioctl
		rv1 = ioctl(fd, BLKGETSIZE64, &size);
		if (rv1 < 0) {
			N_Wf(bvg9365, "ioctl(@STR, BLKGETSIZE64)=@INT @AUTO_ERRNO", vol_path, rv0);
			size = 0;
			goto out;
		}
	}
out:
	if (fd >= 0) {
		close(fd);
	}
	return size;
}

static void run_exec_on_blkdev_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct run_exec_on_blkdev_wq_entry	*entry;
	pid_t								child_pid = -1;
	int									child_fds_stdout[2] = {-1, -1};
	int									child_fds_stderr[2] = {-1, -1};
	int									child_fds_stdin[2] = {-1, -1};
	int									i;
	char								blkdev_path[PATH_MAX];
	struct stat							blkdev_stat;
	struct timespec						start_timestamp;
	struct timespec						now;
	struct nvmeibt_encrypt_params 		*encrypt_params;
	struct nvmeibt_block_device			*blkdev;

	NFIN;
	entry = container_of(wq_entry, struct run_exec_on_blkdev_wq_entry, wq_entry);
	blkdev = entry->run_exec_on_blkdev_ctx->blkdev;
	N_Tf(tskoawm, "blkdev=@STR", nvmeibt_blkdev_name(blkdev));
	encrypt_params = blkdev->encrypt_params;
	// Wait for the blkdev to show up
	snprintf(blkdev_path, sizeof(blkdev_path), "/dev/nvmesh/%s", nvmeibt_blkdev_name(blkdev));
	getnstimeofday(&start_timestamp);
    do {
		if (stat(blkdev_path, &blkdev_stat) == 0) {
			if (!is_block_device_stat(blkdev_stat)) {
				N_Ef(7shj20s, "@STR exists but not a block_device. type=@X", nvmeibt_blkdev_name(blkdev), blkdev_stat.st_mode);
				nvmeibt_Str_sprintf(entry->run_exec_on_blkdev_ctx->child_stderr_buf, "TOMA internal error");
				goto out;
			}
			if (get_nvmesh_vol_size(blkdev_path) != 0) {
				break;
			}
		}
		N_Tf(idhswkr, "Awaiting @STR to attach", blkdev_path);
		getnstimeofday(&now);
		if (timespec_diff_ns(now, start_timestamp) > MSEC_TO_NSEC(entry->run_exec_on_blkdev_ctx->timeout_ms)) {
			N_Ef(0ajdowb, "Timeout awaiting attach of @STR", nvmeibt_blkdev_name(blkdev));
			goto out;
		}
		nanosleep(&(struct timespec){ 0, MSEC_TO_NSEC(100) }, NULL);	// 100ms
	} while (1);
	/* prepare pipes to capture helper process's output */
	if (NNVMEIBT_PIPE(s7hdvka, child_fds_stdout) < 0 || NNVMEIBT_PIPE(6vss93k, child_fds_stderr) < 0 || NNVMEIBT_PIPE(vbdhdk8, child_fds_stdin) < 0) {
		N_Ef(4vghak4, "pipe() failed (@AUTO_ERRNO)");
		goto out;
	}
	/* make the read/write side(s) non-blocking to avoid lockups */
	if (nvmeibt_nonblock_fd(child_fds_stdout[0]) < 0 || nvmeibt_nonblock_fd(child_fds_stderr[0]) < 0 || nvmeibt_nonblock_fd(child_fds_stdin[1]) < 0) {
		N_Ef(7h3klso, "nvmeibt_nonblock_fd() failed (@AUTO_ERRNO)");
		goto out;
	}
	//
	nvmeibt_recursive_mkdir(PASSPHRASE_DIR_NAME, S_IRWXU);  // 0700
	chmod(PASSPHRASE_DIR_NAME, S_IRWXU);  // 0700 - In case it already existed.
	if (	(write_passphrase_to_file(encrypt_params->old_passphrase, encrypt_params->old_passphrase_file_name) ||
			 write_passphrase_to_file(encrypt_params->new_passphrase, encrypt_params->new_passphrase_file_name))) {
		goto out;
	}
	//
	child_pid = fork();
	if (child_pid < 0) {	// Err
		/* error */
		N_Ef(tvjks03, "vfork() failed: (@AUTO_ERRNO)");
		goto out;
	}
	if (child_pid == 0) {	// Child (Note: do not use binary tracing logger in this code)
		int			rv;

		if (setpgid(0, 0) < 0) {	// Set my_pgid = my_pid. All my children should inherit this pgid
		}
		// redirect the chils's stdout and stderr to the pipe that the father captures
		if (dup2(child_fds_stderr[1], STDERR_FILENO) < 0 || dup2(child_fds_stdout[1], STDOUT_FILENO) < 0 || dup2(child_fds_stdin[0], STDIN_FILENO) < 0) {
			fprintf(stderr, "dup2() failed (%m)");
			_exit(103);		// From child
		}
		// After the dup2() close the pre-dup
		if (child_fds_stdout[1] != STDOUT_FILENO)
			close(child_fds_stdout[1]);
		if (child_fds_stderr[1] != STDERR_FILENO)
			close(child_fds_stderr[1]);
		if (child_fds_stdin[0] != STDIN_FILENO)
			close(child_fds_stdin[0]);
		nvmeibt_close_all_nonstd_fds(0);		// Dhsh: I dont understand why this line is here
		//
		if (nvmeibt_encrypt_delay == ENCRYPT_DELAY_BEFORE_EXECUTION)
			encrypt_delay(false);
		syslog(LOG_NOTICE, "%s", entry->run_exec_on_blkdev_ctx->executable_str);
		rv = execlp("bash", "bash", "-c", entry->run_exec_on_blkdev_ctx->executable_str, NULL);
		/* NOT REACHED, unless execlp fails*/
		fprintf(stderr, "execlp failed, %m");
		_exit(rv);		// From child - ONLY if exec() ITSELF failed
	}
	N_Tf(idpw53n, "forked child_pid=@PID", (int)child_pid);
	// Parent goodpath. Wait for child (with timeout), and collect its outputs
	// BTW, we already ignore SIGPIPE (using signal(SIGPIPE, SIG_IGN);)
#if 0	// Use STDIN to feed the passphrase
	n_written = nvmeibt_write(child_fds_stdin[1], stdin_passphrase, strlen(stdin_passphrase));
	N_Tf(rsbwuia, "passphrase_stdin_str n_written=@SIZE_T", n_written);
#endif	// #if 0	// Use STDIN to feed the passphrase
	NNVMEIBT_CLOSE(uqoqmrq, child_fds_stdin[1]);	// EOF at the child
	entry->run_exec_on_blkdev_ctx->exec_rv = waitpid_with_timeout(child_pid, entry->run_exec_on_blkdev_ctx->timeout_ms,
																  entry->run_exec_on_blkdev_ctx->executable_str);
out:
	N_IMf(ecgsuyfjkha, "exec_rv=@INT", entry->run_exec_on_blkdev_ctx->exec_rv);
	// Read the child's stdout & stderr
	if (child_fds_stdout[0] >= 0) {
		nvmeibt_str_read_from_pipe_fd(entry->run_exec_on_blkdev_ctx->child_stdout_buf, child_fds_stdout[0], "out");
	}
	if (entry->run_exec_on_blkdev_ctx->exec_rv == EXEC_ERR_TIMEOUT) {
		nvmeibt_Str_sprintf(entry->run_exec_on_blkdev_ctx->child_stderr_buf, "Timeout occured\n");
	}
	if (child_fds_stderr[0] >= 0) {
		nvmeibt_str_read_from_pipe_fd(entry->run_exec_on_blkdev_ctx->child_stderr_buf, child_fds_stderr[0], "err");
	}
	for (i = 0; i < 2; i++) {
		if (child_fds_stdout[i] >= 0)
			NNVMEIBT_CLOSE(4cgak23, child_fds_stdout[i]);
		if (child_fds_stderr[i] >= 0)
			NNVMEIBT_CLOSE(0suwbtc, child_fds_stderr[i]);
		if (child_fds_stdin[i] >= 0)
			NNVMEIBT_CLOSE(ftbhsj2, child_fds_stdin[i]);
	}
	unlink(encrypt_params->old_passphrase_file_name);
	unlink(encrypt_params->new_passphrase_file_name);
	//
	if (nvmeibt_encrypt_delay == ENCRYPT_DELAY_AFTER_EXECUTION)
		encrypt_delay(true);
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
	NFOUT;
}

static void run_exec_on_blkdev_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct run_exec_on_blkdev_wq_entry		*entry;

	NFIN;
	entry = container_of(wq_entry, struct run_exec_on_blkdev_wq_entry, wq_entry);
	N_Tf(tbjhe93, "blkdev=@STR toma_rv=@INT exec_rv=@INT",
		 nvmeibt_blkdev_name(entry->run_exec_on_blkdev_ctx->blkdev), entry->run_exec_on_blkdev_ctx->toma_rv, entry->run_exec_on_blkdev_ctx->exec_rv);
	if (wq_entry->is_canceled) {
		entry->run_exec_on_blkdev_ctx->toma_rv = -1;
	}
	if (entry->run_exec_on_blkdev_ctx->toma_rv < 0 || entry->run_exec_on_blkdev_ctx->exec_rv < 0) {
		N_Wf(o9kslwp, "Error run_exec_on_blkdev blkdev=@STR toma_rv=@INT exec_rv=@INT",
			 nvmeibt_blkdev_name(entry->run_exec_on_blkdev_ctx->blkdev), entry->run_exec_on_blkdev_ctx->toma_rv, entry->run_exec_on_blkdev_ctx->exec_rv);
	}
	if (entry->run_exec_on_blkdev_ctx->run_exec_on_blkdev_cb_func) {
		entry->run_exec_on_blkdev_ctx->run_exec_on_blkdev_cb_func(entry->run_exec_on_blkdev_ctx);
	}
	NFOUT;
}

static void run_exec_on_blkdev_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct run_exec_on_blkdev_wq_entry		*entry;

	NFIN;
	entry = container_of(wq_entry, struct run_exec_on_blkdev_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(uvna4h2, entry);
	NFOUT;
}

void nvmeibt_run_exec_on_blkdev(struct run_exec_on_blkdev_ctx *ctx)
{
	struct run_exec_on_blkdev_wq_entry 	*run_exec_wq_entry;

	NFIN;
	N_Tf(cvghhsi, "blkdev=@STR", nvmeibt_blkdev_name(ctx->blkdev));
	//
	run_exec_wq_entry = NNVMEIBT_BM_CALLOC(47j5klw, sizeof(*run_exec_wq_entry));
	run_exec_wq_entry->wq_entry.execute = run_exec_on_blkdev_wrapper;
	run_exec_wq_entry->wq_entry.finalize = run_exec_on_blkdev_finalize;
	//
	run_exec_wq_entry->wq_entry.type = "RUN_EXEC_ON_BLKDEV";
	run_exec_wq_entry->wq_entry.free = run_exec_on_blkdev_freer;
	//
	run_exec_wq_entry->run_exec_on_blkdev_ctx = ctx;
	run_exec_wq_entry->run_exec_on_blkdev_ctx->toma_rv = 0;
	run_exec_wq_entry->run_exec_on_blkdev_ctx->exec_rv = EXEC_ERR_EXTERNAL;
	//
	if (nvmeibt_wq_run_once(&run_exec_wq_entry->wq_entry) == NULL) {
		N_Ef(dbb49kt, "Unable to add run_exec_on_blkdev task to WQ!");
		nvmeibt_Str_sprintf(run_exec_wq_entry->run_exec_on_blkdev_ctx->child_stderr_buf, "TOMA internal error");
		run_exec_on_blkdev_finalize(&(run_exec_wq_entry->wq_entry));
		run_exec_on_blkdev_freer(&(run_exec_wq_entry->wq_entry));
		goto out;
	}
out:
	NFOUT;
}

/******************        End run_exec_on_blkdev WQ        *******************/

static void recovery_delete_task(struct recovery_task *recovery_task)
{
	struct nvmeibt_local_disk		*local_disk;

	NFIN;
	if (recovery_task) {
		local_disk = nvmeibt_seg_active_get_local_disk(recovery_task->seg_active);	// Before we delete the task
		N_Tf(trace_recovery_recovery_delete_task, "delete recovery task tid=@TID with state=@RECOVERY_STATE_TO_STR",
			recovery_task->tid, recovery_state_to_str(recovery_task->state));
		if (!recovery_task->end_ordered) {
			N_Ef(error_recovery_recovery_delete_task, "tid=@TID !task->end_ordered", recovery_task->tid);
			nvmeibt_abort(ES_FATAL);
		}
		recovery_task->seg_active = NULL;
		recovery_task->registrant = NULL;
		XDLIST_DEL(&recovery_task->link);
		NNVMEIBT_TOMA_FREE(x5wgw02, recovery_task);
		nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(local_disk);
	}
	NFOUT;
}

static bool recovery_task_delete_if_ended(struct recovery_task *task)
{
	bool	is_deleted = 0;
	//FIN;
	if (task->end_ordered && recovery_task_is_terminal_state(task)) {
		recovery_delete_task(task);
		is_deleted = 1;
	}
	//FOUT;
	return is_deleted;
}

/*
 * State machine transitions:
 *
 *  START        END                                  END             END
 *  STATE      ORDERED   EVENT/CONDITION             STATE          ORDERED
 *  --------------------------------------------------------------------------
 *  INIT              (initial)
 *
 *  INIT              -> (notify client)          -> WAIT_CLIENT
 *  INIT              -> (notify fail, timeout)   -> RETRY_TASK
 *  INIT              -> (end rebuild)            -> CANCELED          Y
 *
 *  WAIT_CLIENT       -> (client register)        -> IN_PROGRESS
 *  WAIT_CLIENT       -> (send fail, timeout)     -> RETRY_TASK
 *  WAIT_CLIENT       -> (end rebuild)            -> CANCELED          Y
 *
 *  IN_PROGRESS       -> (timeout-ping)           -> IN_PROGRESS
 *  IN_PROGRESS       -> (timeout, send fail)     -> RETRY_TASK
 *  IN_PROGRESS       -> (client unregister)      -> RETRY_TASK
 *  IN_PROGRESS       -> (end rebuild)            -> IN_PROGRESS       Y  (1)
 *  IN_PROGRESS       -> (finished)               -> FINISHED          Y
 *  IN_PROGRESS   Y   -> (timeout, send fail)     -> CANCELED          Y  (2)
 *  IN_PROGRESS   Y   -> (finished)               -> CANCELED          Y  (2)
 *
 *  RETRY_TASK        -> (timeout)                -> INIT
 *  RETRY_TASK        -> (end rebuild)            -> ERROR             Y
 *
 *  FINISHED          -> (end rebuild)            -> FINISHED          Y
 *  CANCELED          -> (end rebuild)            -> CANCELED          Y
 *  ERROR             -> (end rebuild)            -> ERROR             Y
 *
 *  CANCELED      Y   (terminal)                  -> delete task
 *  CANCELED      Y   (terminal)                  -> delete task
 *  ERROR         Y   (terminal)                  -> delete task
 *
 * Comments:
 *  (1) if TOMA calls nvmeibt_recovery_launch_abort_rebuild() on task IN_PROGRESS then
 *      we first try to gracious abort.
 *  (2) once aborted, we always switch to CANCELED, whether succeeded or not.
 *
 */

static void recovery_task_set_state(struct recovery_task *task, enum RECOVERY_STATE state)
{
	struct nvmeibt_praid	*praid;
	enum NVMEIBT_RECOVERY_STATUS status;

	NFIN;
	praid = nvmeibt_seg_active_get_praid(task->seg_active);

	N_Tf(trace_recovery_recovery_task_set_state, "tid=@TID state=@RECOVERY_STATE_TO_STR --> state=@RECOVERY_STATE_TO_STR", task->tid,
		recovery_state_to_str(task->state), recovery_state_to_str(state));

	/*
	 * For state RETRY_TASK, if TOMA already canceled the task before via
	 * nvmeibt_recovery_launch_abort_rebuild(), just switch instead to CANCELED.
	 * See also coment in nvmeibt_recovery_launch_abort_rebuild().
	 */
	if ((state == RECOVERY_STATE_RETRY_TASK) && task->end_ordered) {
		state = RECOVERY_STATE_CANCELED;
	}

	if (task->state == state)
		goto out;

	/*
	 * For state RETRY_TASK we compare progress to last attempt: if different,
	 * then reset the backoff counter (more likely not due to same problem as
	 * before). Only if failed in same place/way as before we bump task_retry.
	 */
	if (state == RECOVERY_STATE_RETRY_TASK) {
		if (task->prev_state != task->state)
			recovery_reset_task_retry(task);
		task->prev_state = task->state;
	}

	/* only allow transition if not already in terminal state */
	if (recovery_task_is_terminal_state(task)) {
		N_Tf(trace_1_recovery_recovery_task_set_state, "task tid=@TID cannot change from terminal state=@RECOVERY_STATE_TO_STR to state=@RECOVERY_STATE_TO_STR",
			task->tid, recovery_state_to_str(task->state), recovery_state_to_str(state));
		/*
		 * already in terminal state so we must have reported to TOMA when
		 * switched to terminal state before: skip reporting now.
		 */
		goto out;
	} else {
		task->state = state;
	}

	/* in final state notify TOMA (TOMA will end the task if not already) */
	switch (task->state) {
	case RECOVERY_STATE_FINISHED:
		status = NVMEIBT_RECOVERY_STATUS_SUCCESS;
		break;
	case RECOVERY_STATE_CANCELED:
		status = NVMEIBT_RECOVERY_STATUS_ABORT;
		break;
	case RECOVERY_STATE_ERROR:
		status = NVMEIBT_RECOVERY_STATUS_ERROR;
		break;
	default:
		/* nothing to notify about */
		goto out;
	}
	// We get here only for recoveries that ended their life-cycle, and only once per tid.
	nvmeibt_praid_dec_n_recoveries_needing_hidden_attach(praid);
	N_IMf(dgyy743, "<<<<<----- Done @RECOVERY_TYPE_TO_STR tid=@TID effort=@EFFORT_PERCENTS batch_size=@UINT vol=@VOL praid=@UUID_LE, owner-seg=@UUID_8 status=@STATUS_STR status=@STATUS_STR",
		 nvmeibt_recovery_type_to_str(task->type),
		 (unsigned long long) task->tid, (u32)task->effort_percents, task->max_batch_size,
		 nvmeibt_seg_active_blkdev_name(task->seg_active),
		 nvmeibt_praid_UUID(nvmeibt_seg_active_get_praid(task->seg_active)),
		 nvmeibt_seg_active_UUID_8(task->seg_active),
		 recovery_state_to_str(state),
		 nvmeibt_recovery_status_to_str(status));

	/* notify TOMA that task ended */
	nvmeibt_seg_active_recovery_done(task->tid, task->seg_active, task->type, status);

	/*
	 * We may have recovery-attached the client before; if we no longer use
	 * this client by other tasks, then we shall cleanly recovery-detach it.
	 *
	 * (Use _deferred to delay the operation a bit, in case a new recovery
	 * task with same volume/client pair comes in (avoid detach-then-attach).
	 */
	attach_detach_praid_for_recovery(praid, RECOVERY_ATTACH_CMD_DETACH);

	/* from this point onward we may not trust task->seg_active anymore */
	task->seg_active = NULL;

	/*
	 * By now the task is marked for deletion since the TOMA callback would
	 * have call nvmeibt_recovery_launch_abort_rebuild().
	 */

	NTOMA_ASSERT(error_recovery_recovery_task_set_state, task->end_ordered, "task tid=@TID end_ordered unset", task->tid);

out:
	/*
	 * Even if task is in terminal state we _must not_ delete the task now
	 * because caller(s) up the stack may use the pointer. See also comment
	 * in nvmeibt_recovery_launch_abort_rebuild().
	 *
	 * Instead, the task is marked for deletion (via task->end_ordered) to
	 * cause the timeout calculation for this task to trigger an immediate
	 * timeout, so the subsequent timeout handler will delete it safely.
	 */

	// DONT CALL: recovery_delete_task(task);

	recovery_task_upd_timeout_time(task);
	NFOUT;
}

static BOOL recovery_task_is_able_to_send_abort_to_local_registrant(struct recovery_task *task)
{
	struct nvmeibt_local_disk		*local_disk;
	NFIN;

	if (!!(task->registrant) ^ !!(task->state == RECOVERY_STATE_IN_PROGRESS)) {
		N_Ef(error_recovery_recovery_task_may_abort, "IN_PROGRESS without registrant or vice versa: tid=@TID state=@RECOVERY_STATE_TO_STR",
			task->tid, recovery_state_to_str(task->state));
	}

	local_disk = nvmeibt_seg_active_get_local_disk(task->seg_active);
	NFOUT;

	/* cannot send "abort" message if no registrant to receive... */
	// Cannot send abort to the local client (registrant) if the local_disk is gone
	return (!nvmeibt_local_disk_is_being_deleted(local_disk) && task->registrant);
}

static int recovery_task_look_for_local_active_registrant(struct recovery_task *task, struct nvmeibt_registrant_ctx *new_local_reg_ctx)
{
	struct nvmeibt_registrant_ctx	*reg_ctx;
	const char						*reg_ctx_hostname;

	NFIN;
	if (task->registrant != NULL) {
		goto out;
	}
	if (new_local_reg_ctx && new_local_reg_ctx->is_recoverer) {
		reg_ctx = new_local_reg_ctx;
		goto found_local_clnt;
	}
	XHASHTABLE_FOR_EACH_SAFE(reg_ctx, &task->seg_active->active_registrants) {
		reg_ctx_hostname = __recovery_get_host_name(reg_ctx->client->net);
		// N_Tf(y776zs, "consider registrant=@HOSTNAME reg_lock_id=@C_LID seg=@UUID_8", reg_ctx_hostname, reg_ctx->reg_lock_id.all, nvmeibt_seg_active_UUID_8(task->seg_active));
		if (reg_ctx->is_recoverer && (strncmp(nvmeibt_get_my_hostname(), reg_ctx_hostname, NVMEIB_HOST_NAME_LEN) == 0)) {
			if (!nvmeibt_register_is_processing_registrant_removal(reg_ctx)) {
				goto found_local_clnt;
			}
		}
	}
	goto out;
found_local_clnt:
	task->registrant = reg_ctx;
	N_Tf(buw88xh, "tid=@TID seg=@UUID_8 found local registrant reg_lock_id=@C_LID", task->tid, nvmeibt_seg_active_UUID_8(reg_ctx->seg_active), reg_ctx->reg_lock_id.all);
out:
	NFOUT;
	return !!(task->registrant);
}

static BOOL recovery_is_mandatory(struct recovery_task *task)
{
	BOOL is_mandatory = 0;

	switch (task->type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
	case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON:
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
		is_mandatory = 1;
		break;
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:
		is_mandatory = 0;
		break;
	default:
		N_Ef(error_recovery_recovery_is_mandatory, "invalid task type=@RECOVERY_TYPE_TO_STR for tid=@TID",
			nvmeibt_recovery_type_to_str(task->type), task->tid);
		nvmeibt_abort(ES_FATAL);
	}

	return is_mandatory;
}

static void __fill_recovery_task_header(struct nvmeibt_client_recovery_generic_header *hdr, const struct recovery_task *task)
{
	hdr->id =   task->tid;
	hdr->type = task->type;
	hdr->effort_percents = task->effort_percents;
	hdr->max_batch_size =  task->max_batch_size;
}

static void recovery_send_start(struct recovery_task *task)
{
	struct nvmeibt_client_recovery_start_pl pl;
	int rv;
	struct nvmeibt_praid					*praid;
	struct nvmeibt_praid_lot				*praid_lot;
	struct nvmeibt_seg_lot					*peer_seg_lot;

	NFIN;
	praid = nvmeibt_seg_active_get_praid(task->seg_active);
	__recovery_task_fill_effort_params(task);					// Recalculate, in case it took a long time to start rebuild, and meanwhile user changed the effort

	memset(&pl, 0, sizeof(pl));
	__fill_recovery_task_header(&pl.task, task);
	nvmeibt_strlcpy(pl.praid_id, nvmeibt_praid_id_str(praid), sizeof(pl.praid_id));
	pl.is_mandatory = recovery_is_mandatory(task);
	pl.do_only_owners = pl.task.type != NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC;
	if (pl.task.type == NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON) {
		/* Should not be executed directly. Auto launched by client*/
	} else if (pl.task.type == NVMEIBT_RECOVERY_TYPE_EC_COLD) {
		int i;
		pl.cold.surviving_ram_bmp = 0;
		praid_lot = &praid->praid_follower.applied_praid_lot;
		for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
			peer_seg_lot = praid_lot->topo_seg_lots[i];
			if (peer_seg_lot->seg_topo.leader_seg_flags.has_ram_survived) {
				pl.cold.surviving_ram_bmp |= (1 << i);
			}
		}
	}
	nvmeibt_seg_active_get_recovery_blkset_range(task->type, task->seg_active, &pl.start_lock, &pl.num_locks);
	N_Tf(mmss472, "seg=@UUID_8 task_type=@TASK_TYPE start_lock=@UINT64_TX", nvmeibt_seg_active_UUID_8(task->seg_active), nvmeibt_recovery_type_to_str(pl.task.type), pl.start_lock);

	rv = nvmeibt_register_send_msg_to_registrant(task->registrant,
												 NVMEIBT_CLIENT_MSG_TR_RECOVER_START,
												 NVMEIBT_CLIENT_TR_REASON_NONE,
												 sizeof(pl),
												 &pl);
	if (rv < 0) {
		recovery_write_log_msg_to_mgmt(task, RECOVERY_ERROR_MSG_TO_CLIENT, 0, true);
		recovery_task_set_state(task, RECOVERY_STATE_RETRY_TASK);
		goto out;
	}

out:
	NFOUT;
}

static void recovery_send_abort(struct recovery_task *task)
{
	struct nvmeibt_client_recovery_taskid_pl pl;
	int rv;

	NFIN;
	__fill_recovery_task_header(&pl.task, task);

	/*
	 * Never reach here for tasks not in state IN_PROGRESS (task->registrant
	 * may only be valid for tasks IN_PROGRESS or terminal state.
	 */
	if (task->state != RECOVERY_STATE_IN_PROGRESS) {
		N_Ef(error_recovery_recovery_send_abort, "recovery: abort msg to task not IN_PROGRESS (tid=@TID state=@RECOVERY_STATE_TO_STR)",
			task->tid, recovery_state_to_str(task->state));
		recovery_task_set_state(task, RECOVERY_STATE_RETRY_TASK);
		goto out;
	}

	rv = nvmeibt_register_send_msg_to_registrant(task->registrant,
												 NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT,
												 NVMEIBT_CLIENT_TR_REASON_NONE,
												 sizeof(pl),
												 &pl);

	if (rv < 0) {
		N_Wf(warn_recovery_recovery_send_abort, "recovery: failed to send NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT (@AUTO_ERRNO)");
		recovery_task_set_state(task, RECOVERY_STATE_RETRY_TASK);
		goto out;
	}

	/*
	 * After ABORT messages to client, wait for progress report that confirms
	 * the task was aborted.
	 */
	recovery_task_set_state(task, RECOVERY_STATE_IN_PROGRESS);

	task->n_pings = RECOVERY_PING_MAX; /* avoid PINGs after abort */
	task->was_aborted = 1;

out:
	NFOUT;
}

static void recovery_send_ping(struct recovery_task *task)
{
	struct nvmeibt_client_recovery_taskid_pl pl;
	int rv;

	NFIN;
	__fill_recovery_task_header(&pl.task, task);

	rv = nvmeibt_register_send_msg_to_registrant(task->registrant,
												 NVMEIBT_CLIENT_MSG_TR_RECOVER_PING,
												 NVMEIBT_CLIENT_TR_REASON_NONE,
												 sizeof(pl),
												 &pl);

	if (rv < 0) {
		recovery_write_log_msg_to_mgmt(task, RECOVERY_ERROR_MSG_TO_CLIENT, 0, true);
		recovery_task_set_state(task, RECOVERY_STATE_RETRY_TASK);
		goto out;
	}

	/* only actually send the first PING (the rest are redundant) */
	if (task->n_pings == 0)
		recovery_task_upd_timeout_time(task);

	task->n_pings++;

out:
	NFOUT;
}

/*
 * recovery_state_init(): handle state INIT
 * Initialize state, optionally notify client, and switch to next state.
 */
static void recovery_state_init(struct recovery_task *task)
{
	struct nvmeibt_praid	*praid;

	NFIN;
	praid = nvmeibt_seg_active_get_praid(task->seg_active);
	attach_detach_praid_for_recovery(praid, RECOVERY_ATTACH_CMD_ATTACH);

	if (task->registrant != NULL) {
		goto done;
	}
	if (recovery_task_look_for_local_active_registrant(task, NULL)) {
		goto done;
	}
done:
	recovery_task_set_state(task, RECOVERY_STATE_WAIT_CLIENT);
	NFOUT;
}

/*
 * recovery_state_wait_client(): handle state WAIT_CLIENT
 * If selected client already registered, skip to IN_PROGRESS, else nothing.
 */
static void recovery_state_wait_client(struct recovery_task *task, struct nvmeibt_registrant_ctx *new_local_reg_ctx)
{
	NFIN;
	if (recovery_task_look_for_local_active_registrant(task, new_local_reg_ctx)) {
		recovery_task_set_state(task, RECOVERY_STATE_IN_PROGRESS);	// Might "fail". E.g., when already in terminal stat
		if (task->state == RECOVERY_STATE_IN_PROGRESS)
			recovery_send_start(task);
	}
	NFOUT;
}

/*
 * recovery_state_in_progress(): handle state IN_PROGRESS
 * If timeout has passed then send PING message to client.
 */
static void recovery_state_in_progress(struct recovery_task *task)
{
	NFIN;
	/*
	 * Allow up to RECOVERY_PING_MAX pings before timeout causes errors: the
	 * call to recovery_send_ping() will reset the timer and defer the timeout.
	 */
	if (task->n_pings < RECOVERY_PING_MAX && recovery_task_is_timeout_expired(task))
		recovery_send_ping(task);
	NFOUT;
}

static void recovery_state_retry_task(struct recovery_task *task)
{
	NFIN;
	N_Tf(trace_recovery_recovery_state_retry_task, "retry tid=@TID state=@RECOVERY_STATE_TO_STR (attempt @TASK_RETRY)", task->tid,
		recovery_state_to_str(task->state), task->task_retry);
	NTOMA_ASSERT(error_recovery_recovery_state_retry_task, task->state == RECOVERY_STATE_RETRY_TASK && !task->end_ordered,
				"retry not in RETRY_TASK or with end_ordered tid=@TID state=@RECOVERY_STATE_TO_STR",
				task->tid, recovery_state_to_str(task->state));
	task->registrant = NULL;
	task->was_aborted = 0;
	task->n_pings = 0;
	task->task_retry++;
	task->ret_code = 0;
	recovery_task_set_state(task, RECOVERY_STATE_INIT);
	NFOUT;
}

static struct recovery_task *recovery_alloc_task(
		enum NVMEIBT_RECOVERY_TYPE task_type,
		struct nvmeibt_seg_active *seg_active)
{
	struct recovery_task *task;

	NFIN;
	task = NNVMEIBT_TOMA_CALLOC(trace_recovery_recovery_alloc_task, 1, sizeof(*task));
	task->type = task_type;
	task->seg_active = seg_active;
	NFOUT;
	return task;
}

static u64 recovery_alloc_tid(void)
{
	static u64 tid = 0xaa000000;

	/* won't overflow anytime soon (...) so safe from reuse of tid */
	return ++tid;
}

static void __recovery_task_fill_effort_params(struct recovery_task *task)
{
	const struct nvmeibt_block_device *bdev = nvmeibt_disk_segment_get_blkdev(nvmeibt_seg_active_get_disk_segment(task->seg_active));
	int	relative_rebuild_priority = bdev->from_config.attr.relative_rebuild_priority;
	NFIN;

	if (task->type == NVMEIBT_RECOVERY_TYPE_SCRUBBING) {
		/* scrubbing is done in the background, low priority */
		task->effort_percents = RECOVERY_SCRUBBING_PRIORITY;
	} else if ((relative_rebuild_priority >= 1)&&(relative_rebuild_priority <= 10)) {	// Valid range
		task->effort_percents = relative_rebuild_priority*10;					// convert to percents
		TODO(Here insert logic which takes into account other rebuilds)
		/* Ronen Hod remark: The simplest is to always run the one top-priority seg.
		   Also stop the already running tasks with low priority
		   In the future, take into account other factors (e.g. the amount of work left), and use those as weights.
		 */
	} else {
		task->effort_percents = NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE;		// Default clients value
	}
	task->max_batch_size = nvmeibt_recovery_client_batch_n_blksets_get();
	NFOUT;
}

static u64 recovery_new_task(enum NVMEIBT_RECOVERY_TYPE task_type, struct nvmeibt_seg_active *seg_active, int praid_version)
{
	struct recovery_task *task;

	NFIN;

	task = recovery_alloc_task(task_type, seg_active);

	task->tid = recovery_alloc_tid();
	task->praid_version = praid_version;
	__recovery_task_fill_effort_params(task);				// Calculate here, to be able to sort by priority in future

	XDLIST_ADD_TAIL(&recovery_task_list, task);
	N_Tf(es83k4o, "recovery_task_list size=@SIZE", XDLIST_N_ELEMNTS(&recovery_task_list));
	nvmeibt_praid_inc_n_recoveries_needing_hidden_attach(nvmeibt_seg_active_get_praid(task->seg_active));

	N_IMf(yy7ww38, "----->>>>> Start @RECOVERY_TYPE_TO_STR tid=@TID vol=@VOL praid=@UUID_LE owner-seg=@UUID_8 size=@SIZEOF(blksets)",
		 nvmeibt_recovery_type_to_str(task->type), task->tid,
		 nvmeibt_seg_active_blkdev_name(task->seg_active),
		 nvmeibt_praid_UUID(nvmeibt_seg_active_get_praid(task->seg_active)),
		 nvmeibt_seg_active_UUID_8(task->seg_active),
		 num_blksets_in_disk_segment(nvmeibt_seg_active_get_disk_segment(task->seg_active)));

	recovery_task_set_state(task, RECOVERY_STATE_INIT);		// The state machine will trigger the attach
	recovery_task_state_machine(task, NULL);

	NFOUT;
	return task->tid;
}

static void recovery_task_state_machine(struct recovery_task *task, struct nvmeibt_registrant_ctx *new_local_reg_ctx)
{
	BOOL check_timeout = 1;

	NFIN;

again:
	N_Tf(trace_recovery_recovery_task_state_machine, "tid=@TID state=@STATE_STR", task->tid, recovery_state_to_str(task->state));

	switch (task->state) {
	case RECOVERY_STATE_INIT:
		recovery_state_init(task);
		/* if state advanced to WAIT_CLIENT, then re-process now */
		if (task->state == RECOVERY_STATE_WAIT_CLIENT)
			goto again;
		break;
	case RECOVERY_STATE_WAIT_CLIENT:
		recovery_state_wait_client(task, new_local_reg_ctx);
		break;
	case RECOVERY_STATE_IN_PROGRESS:
		recovery_state_in_progress(task);
		break;
	case RECOVERY_STATE_RETRY_TASK:
		recovery_state_retry_task(task);
		break;
	case RECOVERY_STATE_FINISHED:
	case RECOVERY_STATE_CANCELED:
	case RECOVERY_STATE_ERROR:
		check_timeout = 0;
		break;
	default:
		N_Ef(error_recovery_recovery_task_state_machine, "Unknown state=@STATE", task->state);
		nvmeibt_abort(ES_FATAL);
	}

	if (check_timeout && recovery_task_is_timeout_expired(task)) {
		/*
		 * If a timeout occurs (and not aborted already) we first try to
		 * abort, otherwise we skip straight to error state.
		 */
		if (!task->was_aborted && recovery_task_is_able_to_send_abort_to_local_registrant(task)) {
			N_Tf(warn_0_recovery_recovery_task_state_machine, "recovery task tid=@TID state=@RECOVERY_STATE_TO_STR abort due to timeout",
				 task->tid, recovery_state_to_str(task->state));
			recovery_send_abort(task);
		} else if (task->state == RECOVERY_STATE_RETRY_TASK) {
			/*
			 * Timeouts in state RETRY_TASK are anything but expected, and
			 * should have been handled in recovery_timeout_occurred().
			 * If we do see one here, it must be subtle timing issue, so
			 * log it and ignore.
			 */
			N_Wf(warn_recovery_recovery_task_state_machine, "recovery: tid=@TID state timeout in state RETRY_TASK", task->tid);
		} else {
			N_Tf(warn_1_recovery_recovery_task_state_machine, "recovery: tid=@TID state=@RECOVERY_STATE_TO_STR retry due to timeout",
				 task->tid, recovery_state_to_str(task->state));
			recovery_write_log_msg_to_mgmt(task, RECOVERY_ERROR_TIMEOUT_STATE, 0, true);
			recovery_task_set_state(task, RECOVERY_STATE_RETRY_TASK);
		}
	}

	NFOUT;
}

static void __recovery_on_bdev_attr_change_notify_task(struct recovery_task *toma_task, const struct nvmeibt_client_recovery_generic_header *clnt)
{
	bool need_update_effort = false, need_update_batch_size = false;
	if ((toma_task->state == RECOVERY_STATE_IN_PROGRESS) && !(toma_task->end_ordered)) {	// Just to be safe
		__recovery_task_fill_effort_params(toma_task);
		need_update_effort = (toma_task->effort_percents != NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE) && clnt && (toma_task->effort_percents != clnt->effort_percents);
		need_update_batch_size = !clnt || (toma_task->max_batch_size  != clnt->max_batch_size);
		if (need_update_effort || need_update_batch_size) {
			N_Tf(tr_01_attr_chng_notify_task, "tid=@TID, type: @RECOVERY_TYPE_TO_STR, effort:@EFFORT_PERCENTS, clnt_effort=@EFFORT_PERCENTS, batch_size=@UINT, clnt_batch_size=@UINT", toma_task->tid, nvmeibt_recovery_type_to_str(toma_task->type), (u32)toma_task->effort_percents, (u32)clnt->effort_percents, toma_task->max_batch_size, clnt->max_batch_size);
			recovery_send_ping(toma_task);				// Now send ping: Daniel: If client does not answer pings, a change in rebuild priority cannot be propagated to client
			// Dont use: recovery_task_state_machine(task); because if client progresses nicely pings would not be sent
		}
	}
}

void nvmeibt_recovery_notify_all_dirty_rebuilds_of_params_change(void)
{
	struct recovery_task	*task;

	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {	// Expected to have up to 2 running
		if ((task->type == NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD) || (task->type == NVMEIBT_RECOVERY_TYPE_SCRUBBING)) {
			__recovery_on_bdev_attr_change_notify_task(task, NULL);
		}
	}
}

static void __recovery_handle_msg_status(struct nvmeibt_client_recovery_status_pl *client_recovery_status, BOOL finished)
{
	struct recovery_task							*toma_task;
	struct nvmeibt_client_recovery_generic_header	*clnt_task_hdr = &client_recovery_status->task;
	NFIN;

	toma_task = recovery_find_task_by_tid(clnt_task_hdr->id);
	N_Tf(trace_01_recov_handle_msg, "status msg: tid=@TID, type=@TR_RECOV_TYPE effort=@EFFORT_PERCENTS batch_size=@UINT num left @NUM_LOCKS_LEFT @C_PRV ret_code @RV",
		clnt_task_hdr->id, clnt_task_hdr->type, (u32)clnt_task_hdr->effort_percents, clnt_task_hdr->max_batch_size, client_recovery_status->num_locks_left, client_recovery_status->praid_version, client_recovery_status->ret_code);

	if (toma_task == NULL) {
		if (finished) {
			N_Tf(cvail50, "Unknown tid=@TID finished. rv=@RV. Possibly a deleted task", clnt_task_hdr->id, client_recovery_status->ret_code);
		} else
			N_Tf(trace_03_recov_handle_msg, "No recovery in progress for this segment");
		goto out;
	}

	/* ignore "stray" messages, e.g. that arrive after we are finished */
	if (toma_task->state != RECOVERY_STATE_IN_PROGRESS) {
		N_Tf(trace_04_recov_handle_msg, "Recovery wrong state tid=@TID, state=@RECOVERY_STATE_TO_STR",
			toma_task->tid, recovery_state_to_str(toma_task->state));
		goto out;
	}

	if (toma_task->was_aborted) { // client's ack on abort? meaning the toma_task canceled successfully. Todo: break into sub cases
		N_Tf(trace_05_recov_handle_msg, "status msg for aborted tid=@TID, rv=@RV (new @RV), expected",
			toma_task->tid, toma_task->ret_code, client_recovery_status->ret_code);
		recovery_task_set_state(toma_task, RECOVERY_STATE_CANCELED);
		goto out;
	}

	toma_task->n_pings = 0;

	if ((client_recovery_status->ret_code < 0) || is_rebuild_endless) {
		if (is_rebuild_endless) {
			N_Tf(t_xx_60, "Endless rebuild simulation");
		}
		toma_task->ret_code = client_recovery_status->ret_code; // if client reported error, then it won't expect further communications
		goto _retry_recov;
	} else if (toma_task->type != clnt_task_hdr->type) {
		N_Wf(trace_3_recov_handle_msg, "Recovery error: tid=@TID, toma_type=@TR_RECOV_TYPE clnt_type=@TR_RECOV_TYPE", toma_task->tid, toma_task->type, clnt_task_hdr->type);
		goto _retry_recov;
	}

	switch (toma_task->type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:
		nvmeibt_seg_active_set_n_dirty_bits_remaining(toma_task->seg_active, client_recovery_status->num_locks_left);
		toma_task->seg_active->dirty_next_unfixed_lock_blkset_no = client_recovery_status->next_unfixed_lock;
		N_Tf(3cgan2j, "seg=@UUID_8 dirty_next_unfixed_lock_blkset_no=@UINT64_TX", nvmeibt_seg_active_UUID_8(toma_task->seg_active), toma_task->seg_active->dirty_next_unfixed_lock_blkset_no);
		break;
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
		nvmeibt_seg_active_set_n_stale_locks_remaining(toma_task->seg_active, client_recovery_status->num_locks_left);
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:
		nvmeibt_seg_active_set_n_txid_remaining(toma_task->seg_active, client_recovery_status->num_locks_left);
		break;
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
		N_Tf(error_1_recovery_recovery_handle_msg_status, "Useless JGC progress");
		break;
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:
		/* scrubbing doesn't care about partial updates */
		break;
	default:
		N_Ef(error_2_recovery_recovery_handle_msg_status, "Unknown toma_task_type=@TR_RECOV_TYPE", toma_task->type);
		nvmeibt_abort(ES_FATAL);
	}

	/* so far so good ... reset the toma_task retry count (backoff calculation) */
	recovery_reset_task_retry(toma_task);

	if (finished) {
		recovery_task_set_state(toma_task, RECOVERY_STATE_FINISHED);
		goto out;
	}

	/* recovery still onoing (RECOVERY_STATE_IN_PROGRESS) not finished yet, check if we need to update client with new priority? */
	__recovery_on_bdev_attr_change_notify_task(toma_task, clnt_task_hdr);

	/* all good, toma_task state didn't change: update (reset) toma_task's timeout */
	recovery_task_upd_timeout_time(toma_task);

out:
	NFOUT;
	return;

_retry_recov:
	recovery_write_log_msg_to_mgmt(toma_task, RECOVERY_ERROR_CLIENT_ERROR, client_recovery_status->ret_code, true);
	recovery_task_set_state(       toma_task, RECOVERY_STATE_RETRY_TASK);
	goto out;
}

struct timespec nvmeibt_recovery_get_next_timeout_timespec(void)
{
	return next_wait_for_recovery_timeout;
}

u64 nvmeibt_recovery_start_rebuild(enum NVMEIBT_RECOVERY_TYPE task_type, struct nvmeibt_seg_active *seg_active)
{
	int		praid_version = 0;
	u64		tid = -1ULL;

	NFIN;

	if (!nvmeibt_seg_active_get_disk_segment(seg_active)) {
		N_Tf(vss7812, "seg_active=@UUID_8 without disk_segment, skipping", nvmeibt_seg_active_UUID_8(seg_active));
		goto out;
	}
	if (!nvmeibt_register_is_seg_active_accepting_registrations(seg_active, NULL)) {
		goto out;	// Since we have a limit on n_rebuilds, do not launch one that is unable to do its job
	}

	/*
	 * Get the praid_version from the relevant recovery_ctx (based on task_type).
	 * Note, however, that praid_version is insignificant for: JGC, scrubbing.
	 */
	nvmeibt_seg_active_get_recovery_praid_version(task_type, seg_active, &praid_version);

	tid = recovery_new_task(task_type, seg_active, praid_version);
out:
	NFOUT;
	return tid;
}

bool nvmeibt_recovery_is_aborting_rebuild(u64 tid)
{
	struct recovery_task	*task = recovery_find_task_by_tid(tid);
	return (task ? task->end_ordered : 1);
}

void nvmeibt_recovery_launch_abort_rebuild(u64 tid)
{
	struct recovery_task *task;

	NFIN;

	task = recovery_find_task_by_tid(tid);
	if (task == NULL) {
		N_Wf(7zbaqko, "failed to delete recovery task tid=@TID: not found", tid);
		goto out;
	}

	task->end_ordered = 1;

	/*
	 * We _must not_ delete the task now because caller(s) up the stack may
	 * use the pointer. Consider for example:
	 *   ......() ->
	 *     recovery_......() ->
	 *       recovery_task_set_state() ->
	 *         nvmeibt_disk_segment_applied_recovery_done() ->
	 *           nvmeibt_recovery_launch_abort_rebuild() ->
	 *
	 * Instead, the task is marked for deletion (via task->end_ordered) to
	 * cause the timeout calculation for this task to trigger an immediate
	 * timeout, so the subsequent timeout handler will delete it safely.
	 */

	if (recovery_task_is_terminal_state(task)) {
		// DONT CALL: recovery_delete_task(task);
		goto out;
	}

	/* If task is waiting to retry, might as well kill it now as error */
	if (task->state == RECOVERY_STATE_RETRY_TASK) {
		recovery_task_set_state(task, RECOVERY_STATE_ERROR);
		goto out;
	}

	/*
	 * If task is not in terminal state, then we want to allow it to orderly
	 * send "abort" and receive ack from the registrant.
	 * If the task may be aborted (i.e. if it has a valid registrant to speak
	 * to), then abort now.
	 * If not -and we already handled terminal states above- we simply switch
	 * straight to CANCELED state.
	 */

	if (recovery_task_is_able_to_send_abort_to_local_registrant(task)) {
		recovery_send_abort(task);  /* will set state to IN_PROGRESS */
	} else {
		recovery_task_set_state(task, RECOVERY_STATE_CANCELED);
	}

	/*
	 * TOMA will only do its cleanup after we have invoked the callback
	 * nvmeibt_seg_active_recovery_done(): task->seg_active
	 * is needed until then.
	 */
out:
	NFOUT;
}

/*
 * nvmeibt_recovery_handle_client_registered(): callback from TOMA when a new
 * client registers to a disk segment.
 */
void nvmeibt_recovery_handle_client_registered(struct nvmeibt_registrant_ctx *reg_ctx)
{
	struct recovery_task	*task;

	NFIN;
	N_Tf(jdhheu3, "new registrant=@HOSTNAME reg_lock_id=@C_LID seg=@UUID_8",
		__recovery_get_host_name(reg_ctx->client->net), reg_ctx->reg_lock_id.all,
		nvmeibt_seg_active_UUID_8(reg_ctx->seg_active));

	XDLIST_FOREACH_SAFE(task, &recovery_task_list) { 		// Todo: Daniel, This is very inn-eficient. Should have list per segment, not global list of many recoveries
		if (reg_ctx->seg_active != task->seg_active)
			continue;
		if (strncmp(nvmeibt_get_my_hostname(), reg_ctx->client->net.host_name, NVMEIB_HOST_NAME_LEN))
			continue;
		// Local registration on the right seg
		if (!recovery_task_is_terminal_state(task))
			recovery_task_set_state(task, RECOVERY_STATE_WAIT_CLIENT);	// Somehow we have a registrant - use it
		if (task->state != RECOVERY_STATE_WAIT_CLIENT)
			continue;
		N_Tf(rnn55md, "task tid=@TID found local registrant seg=@UUID_8 reg_lock_id=@C_LID", task->tid, nvmeibt_seg_active_UUID_8(reg_ctx->seg_active), reg_ctx->reg_lock_id.all);
		recovery_task_state_machine(task, reg_ctx);
	}
	NFOUT;
}

/*
 * nvmeibt_recovery_handle_client_unregistered(): callback from TOMA when some
 * existing client un-registers from a disk segment.
 */
void nvmeibt_recovery_handle_client_unregistered(struct nvmeibt_registrant_ctx *reg_ctx)
{
	struct recovery_task *task;

	NFIN;
	N_Tf(ppwwmn3, "unregistering registrant=@HOSTNAME reg_lock_id=@C_LID seg=@UUID_8",
		__recovery_get_host_name(reg_ctx->client->net), reg_ctx->reg_lock_id.all,
		nvmeibt_seg_active_UUID_8(reg_ctx->seg_active));

	NTOMA_ASSERT(wu83nsq, reg_ctx->reg_lock_id.all != 0, "registrant=@HOSTNAME reg_lock_id=@C_LID seg=@UUID_8: no lockid!",
				__recovery_get_host_name(reg_ctx->client->net), reg_ctx->reg_lock_id.all,
				nvmeibt_seg_active_UUID_8(reg_ctx->seg_active));

	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {		// Todo: Daniel, This is very inn-eficient. Should have list per segment, not global list of many recoveries
		if (reg_ctx->seg_active != task->seg_active)
			continue;
		if (!nvmeibt_register_is_same_registrant(task->registrant, reg_ctx))
			continue;										// This test isn ot strong enough. It assumes same segment

		N_Tf(ccu839h, "task tid=@TID drop registrant=@HOSTNAME reg_lock_id=@C_LID seg=@UUID_8",
			task->tid, __recovery_get_host_name(reg_ctx->client->net), reg_ctx->reg_lock_id.all,
			nvmeibt_seg_active_UUID_8(reg_ctx->seg_active));

		task->registrant = NULL;

		/* simply ignore if task in terminal state already */
		if (recovery_task_is_terminal_state(task))
			continue;

		recovery_write_log_msg_to_mgmt(task, RECOVERY_ERROR_CLIENT_GONE, 0, true);
		recovery_task_set_state(task, RECOVERY_STATE_RETRY_TASK);
	}
	NFOUT;
}

/*
 * nvmeibt_recovery_handle_incoming_msg(): callback from TOMA to process
 * an incoming message from client registrant.
 */
int nvmeibt_recovery_handle_incoming_msg(struct nvmeibt_register_msg *msg)
{
	struct nvmeibt_client_msg_pl* payload;

	NFIN;
	payload = (struct nvmeibt_client_msg_pl *) msg->msg_data;
	NREGISTER_MSG_DUMP(trace_recovery_nvmeibt_recovery_handle_incoming_msg, msg->msg_type, msg->reason, &(msg->registrant_ctx), msg->data_length, msg->cookie);

	switch (msg->msg_type) {
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS:
		N_Tf(trace_1_recovery_nvmeibt_recovery_handle_incoming_msg, "Handling NVMEIBT_CLIENT_MSG_RT_RECOVER_PROGRESS left @NUM_LOCKS_LEFT rv=@RV",
			payload->recov_status.num_locks_left, payload->recov_status.ret_code);
		__recovery_handle_msg_status(&payload->recov_status, 0);  // 2nd arg "0" to indicate not finished
		break;
	case NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH:
		N_Tf(trace_2_recovery_nvmeibt_recovery_handle_incoming_msg, "Handling NVMEIBT_CLIENT_MSG_RT_RECOVER_FINISH left @NUM_LOCKS_LEFT rv=@RV",
			payload->recov_status.num_locks_left, payload->recov_status.ret_code);
		__recovery_handle_msg_status(&payload->recov_status, 1);  // 2nd arg "1" to indicate task finished
		break;
	default:
		N_Wf(warn_recovery_nvmeibt_recovery_handle_incoming_msg, "Unexpected msg type @MSG_TYPE", msg->msg_type);
		break;
	}
	NFOUT;
	return 0;
}

/*
 * nvmeibt_recovery_timeout_occurred(): callback from TOMA to tell
 * us that a timeout (may have) occurred.
 */
int nvmeibt_recovery_timeout_occurred(void)
{
	struct recovery_task *task;

	NFIN;
	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {
		if (recovery_task_delete_if_ended(task)) {
			continue;	// Deleted
		}
		if (!recovery_task_is_timeout_expired(task))
			continue;

		N_Tf(trace_recovery_nvmeibt_recovery_timeout_occurred_2, "task tid=@TID state=@RECOVERY_STATE_TO_STR handling timeout",
			task->tid, recovery_state_to_str(task->state));

		if (!nvmeibt_register_is_seg_active_accepting_registrations(task->seg_active, NULL)) {
			nvmeibt_recovery_launch_abort_rebuild(task->tid);
		}
		recovery_task_state_machine(task, NULL);
	}

	calc_next_wait_for_recovery_timeout();
	NFOUT;
	return 0;
}

/* helpers to print out debugging information */

static uint64_t task_get_n_remaining_blksets(struct recovery_task *task)
{
	switch (task->type) {
	case NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD:		return nvmeibt_seg_active_get_n_dirty_bits_remaining(task->seg_active);
	case NVMEIBT_RECOVERY_TYPE_STALE_REBUILD:		return nvmeibt_seg_active_get_n_stale_locks_remaining(task->seg_active);
	case NVMEIBT_RECOVERY_TYPE_EC_FIX_UNK_BINFO:	return nvmeibt_seg_active_get_n_txid_remaining(task->seg_active);
	case NVMEIBT_RECOVERY_TYPE_SCRUBBING:			return nvmeibt_seg_active_get_n_scrubbing_remaining(task->seg_active);
	//
	case NVMEIBT_RECOVERY_TYPE_EC_COLD:
	case NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE:
	case NVMEIBT_RECOVERY_TYPE_EC_DCONVICT_TURNON:
	case NVMEIBT_RECOVERY_TYPE_EC_JOUR_GC:
	case NVMEIBT_RECOVERY_TYPE_VOID_DUMMY:
		return -1ULL;
	case NVMEIBT_RECOVERY_TYPE_INVALID:
	default:
		N_Ef(fy237jw, "Unknown task->type=@INT", task->type);
		return -1ULL;
	}
}

static int recovery_task_print_status(
				struct recovery_task *task,
				int (*printf_fn)(void *ctx, const char *fmt, ...),
				void *printf_ctx)
{
	struct nvmeibt_praid					*praid;
	struct nvmeibt_praid_lot				*praid_lot;
	struct nvmeibt_seg_lot					*seg_lot;
	char									*vol_name = "";
	int										i;

	NFIN;
	praid = nvmeibt_seg_active_get_praid(task->seg_active);
	/* task state/status info */

	(*printf_fn)(printf_ctx,
				 "\t - tid=%#lx seg=%08x type=%s effort=%d%% batch_size=%u state=%s pings=%d flags=%s%s n_blksets_left=%llu\n",
				 task->tid,
				 nvmeibt_seg_active_UUID_8(task->seg_active),
				 nvmeibt_recovery_type_to_str(task->type),
				 (u32)task->effort_percents,
				 task->max_batch_size,
				 recovery_state_to_str(task->state),
				 task->n_pings,
				 task->was_aborted ? "abort " : "",
				 task->end_ordered ? "ended " : "",
				 task_get_n_remaining_blksets(task));

	/* volume info */

	if (task->seg_active == NULL) {
		/* task->seg_active may be NULL e.g. if terminated/aborted */
		(*printf_fn)(printf_ctx, "\t\t - volume=N/A (praid):\n");
		(*printf_fn)(printf_ctx, "\t\t\t - N/A\n");
		goto skip;
	}

	if (praid->praid_mgmt.its_chunk &&
		praid->praid_mgmt.its_chunk->its_block_device)
		vol_name = praid->praid_mgmt.its_chunk->its_block_device->from_config.client_blkdev_name;
	(*printf_fn)(printf_ctx, "\t\t - volume='%s' (praid):\n", vol_name);


	praid_lot = &praid->praid_follower.applied_praid_lot;
	for (i = 0; i < praid_lot->n_topo_seg_lots; i++) {
		seg_lot = praid_lot->topo_seg_lots[i];
		(*printf_fn)(printf_ctx, "\t\t\t - [%d] : seg %.8s state=%s node=%s\n",
					 i, nvmeibt_disk_segment_id_str(seg_lot->my_seg), nvmeibt_seg_topo_dirty_bits_state_str(&(seg_lot->seg_topo)),
					 nvmeibt_node_name(seg_lot->my_seg->seg_mgmt.its_disk->its_node_config));
	}

skip:
	NFOUT;
	return 0;
}

int nvmeibt_recovery_print_status(
				int (*printf_fn)(void *ctx, const char *fmt, ...),
				void *printf_ctx)
{
	struct recovery_task *task;

	NFIN;
	(*printf_fn)(printf_ctx, "RECOVERY STATUS\n");

	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {
		recovery_task_print_status(task, printf_fn, printf_ctx);
	}
	NFOUT;
	return 0;
}

void nvmeibt_recovery_exit(void)
{
	struct recovery_task *task;

	NFIN;
	N_Tf(trace_recovery_nvmeibt_recovery_exit, "clean up recovery tasks");
	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {
		N_Tf(trace_1_recovery_nvmeibt_recovery_exit, "delete recovery task tid=@TID status=@RECOVERY_STATE_TO_STR",
			task->tid, recovery_state_to_str(task->state));
		/* delete the task */
		if (task->seg_active) // task is not finished
			attach_detach_praid_for_recovery(nvmeibt_seg_active_get_praid(task->seg_active), RECOVERY_ATTACH_CMD_DETACH);  /* better exit clean */
		recovery_delete_task(task);
	}
	nanosleep(&(struct timespec){0, MSEC_TO_NSEC(10)}, NULL);	// Let the detach run
	attach_detach_wq_exit();
	NFOUT;
}

int nvmeibt_recovery_init(void)
{
	NFIN;
	N_Tf(trace_recovery_nvmeibt_recovery_init, "initialize recovery tasks");
	calc_next_wait_for_recovery_timeout();
	attach_detach_wq_init();
	cleanup_passphrase_dir_leftover_files();
	NFOUT;
	return 0;
}

void nvmeibt_recovery_set_endless_rebuild_state(bool is_endless)
{
	if (is_rebuild_endless != is_endless) {
		if (is_endless)
			N_Tf(t_xx_61, "REBUILD set to endless");
		else
			N_Tf(t_xx_62, "REBUILD set to normal");
		is_rebuild_endless = is_endless;
	}
}

void nvmeibt_recovery_report_rebuild_progress_to_mgmt(void)
{
	struct nvmeibt_Str						*json_payload = NULL;
	static unsigned int						max_json_payload_len = 1024;
	struct recovery_task					*task;

	NFIN;
	if (	!nvmeibt_global_is_any_rebuild_progress_report_to_mgmt_due() ||
			(timespec_diff_ns(nvmeibt_global_get_cur_event_start_time(), nvmeibt_global_get_global()->last_progress_report_timestamp) < MIN_NSEC_BETWEEN_PROGRESS_REPORTS)) {
		goto out;
	}

	json_payload = NNVMEIBT_STR_ALLOC(7dgsnc5);
	NNVMEIBT_STR_RESIZE_BUF(ji983e6, json_payload, max_json_payload_len);

	XDLIST_FOREACH_SAFE(task, &recovery_task_list) {	// Expected to have up to 2 running
		nvmeibt_seg_active_send_one_seg_rebuild_progress_report_to_mgmt(task->seg_active, json_payload);
		max_json_payload_len = max(max_json_payload_len, nvmeibt_Str_strlen(json_payload) + 10);
	}
	nvmeibt_global_get_global()->last_progress_report_timestamp = nvmeibt_global_get_cur_event_start_time();
	nvmeibt_global_clear_is_any_rebuild_progress_report_to_mgmt_due();
out:
	NNVMEIBT_STR_FREE(bvhfpqz, json_payload);
	NFOUT;
}

