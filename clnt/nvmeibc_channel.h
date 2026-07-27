#ifndef NVMEIBC_CHANNEL_H
#define NVMEIBC_CHANNEL_H

#include "kr_incs.h"
#include "nvmeibc_types.h"
#include "nvmeibs_types.h"
#include "nvmeibc_core_dbgdi_shared.h"

struct nvmeibc_volume;
struct nvmeibc_disk_command;
struct nvmeibc_disk;
struct wd_obj;
struct nvmeibc_cinst_params_core;

struct nvmeibc_channel {
	char rhost_name[NVMEIB_HOST_NAME_LEN];
	char name[NVMEIB_HOST_NAME_LEN + 1 /* '-' */ +
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE + 5 /* '~NR.L' */  +
		4 /* lionic_index */ + 1 /* 'R' */ + 4 /* 'rionic_index' */ +
		1 + /* 'C' */ + 4 /* 'qpn' */ + 1 /* NULL */];
	/* the disk that holds the channel */
	struct nvmeibc_disk *disk;
	/* disk-version:
	   set on channel create and used when constructing rcookie to avoid
	   if we were to construct cookie using disk->version, either before
	   llp-post or on llp-comp, we might endup using updated version i.e.
	   while disk is releasing and ch (still not) --> EC-5597
	*/
	u64 disk_version;
	/* well one always needs a lock... */
	spinlock_t spinlock;
	/* and we would like to know which cpu locked it */
	int locking_pid;
	/* 1 if we are in the process of disappearing */
	atomic_t dying;
	/* wait queue to wait for controller replies */
	wait_queue_head_t wqh;
	/* atomic unique counter per channel */
	atomic_t tag;
	/* our watchdog */
	struct wd_obj *wd;
	/* used by io-channel for lists:
	   disk->info->available_channels and disk->ioch_kill_list lists */
	struct list_head link;
	/* the type of the derived channel */
	enum channel_type ct;
	/* the index of the channel */
	int index;
	/* the channel version */
	u64 version;
	bool version_valid;
	/* SMP sync abstraction for channel version */
	seqcount_t version_seq;
	/* number of reqs taken by user (new-IO path)
	   and still not issued to the net */
	atomic_t n_user_reqs;
	struct completion *reqs_comp;
	/* virtual functions */
	int (*execute_io)(struct nvmeibc_channel *ch, struct nvmeibc_disk *disk,
		struct nvmeibc_disk_command *disk_cmd, void *context);
	int (*execute_pending_io)(struct nvmeibc_disk *disk,
		struct nvmeibc_channel *ch, void *context, bool sp_locked, u64 version);
	int (*execute_ka)(struct nvmeibc_channel *ch);

	/* io counters */
	u64 cnt_io_ok;
	u64 cnt_io_fail;
	u64 cnt_read_ok;
	u64 cnt_read_fail;
	u64 cnt_write_ok;
	u64 cnt_write_fail;
	u64 cnt_trim_ok;
	u64 cnt_trim_fail;

	/* Used to ensure the channel is only freed once */
	atomic_t n_freed;
	const struct nvmeibc_cinst_params_core *cips;

//#if	defined(DEBUG_REQ_REUSED_BB_STATE) && (DEBUG_REQ_REUSED_BB_STATE == 1)
	/* list volume-reqs owned by ulp (aka rcookie) - lru first */
	struct list_head reused_bb_list;
	spinlock_t reused_bb_list_guard;
	u32 reused_bb_cnt;
	/* time when lru rcookie was given to ulp
	   i.e. @req->reused_bb_lru_jif of the head of the reused_bb_list, if any */
	/* this is for displying on /proc files and hence lockless,
	   we can add support later to display the entire list */
	unsigned long reused_bb_lru_jif;
//#endif

	//[IOCH-DRAINED TBD]: hide this from nvmeib_channel class
	// [*] it is used directly in nvmeibc_channel_init(), see ioch_drained_init
	// [*] make this priv-info of c_disk to be alloced on channel-init by it;
	//     cant alloc this on-demand, in bad-path, what to do if alloc fails?!
	struct ioch_bailed_cmds {
		/* client/server generation-ID
		   inc'ed after receiving NVMEIBS_IOCH_DRAINED from srv meaning
		   all ioch's NVMe cmds were drained from disk-queue. only then
		   is client allowed to reconnect this ioch */
		u64 cs_gid;
		struct list_head link;
		struct list_head list;
		unsigned long arm_jif;
		int srv_drained;
	} bailed_cmds;

	/* cpu on which comps of this channel will arrive on.
	 * set during connect before connecting net and
	 * cleared on disconnect or connect-rollback.
	 * -1 - Not set,
	 * >= 0 - CPU to run completions on.
	 */
	int comp_cpu;

	/**************************************************************************************************
	 * Make the completion processing lockless:
	 * i.e. Instead of acquiring the channel lock, req-info lock (NRCH), lock-ch lock (LOCK) or wd-lock,
	 * simply disable preemption and interrupts.
	 * Requires that all access to the channel (iopath submission, iopath completion, error path, bring-up and tear-down) is performed by the same cpu.
	 *
	 * This is done for completions by either:
	 * - Using dev-cq on all cpus (nvmeib_pcpu_cq_all_cpus=Y) OR
	 * - Using an rcq/scq offload thread bound to the correct cpu OR
	 * - Using shared cq and the defer_recv_intr_wq bound to the correct cpu
	 *
	 * The following locking macros check this value and instead of locking/unlocking the spin-lock,
	 * instead do get_cpu/put_cpu and local_irq_save/local_irq_restore:
	 * - nvmeibc_channel_spin_[un]lock*,
	 * - nrch_guard_spin_[un]lock_*,
	 * - ri_spin_[un]lock*,
	 * - nvmeibc_locks_channel_spin_un[lock]*,
	 *
	 * Relevant functions include:
	 * - nvmeibc_ib_net_nordda_execute_io (NRCH)
	 * - get_req_info (NRCH)
	 * - put_req_info (NRCH)
	 * - send_completion_has_rsp (NRCH)
	 * - nordda_send_completion (NRCH)
	 * - process_rsp (NRCH)
	 * - nvmeibc_ib_nordda_channel_reused_context (NRCH)
	 * - on_start_wd_event (NRCH)
	 * - on_end_wd_event (NRCH)
	 * - process_send_cq
	 * - polling_process_send_cq_
	 * - scq_kthread_func
	 * - intr_process_send_cq_
	 * - polling_process_recv_cq_
	 * - rcq_kthread_func
	 * - intr_process_recv_cq_
	 * - poll_cq_and_process
	 * - recv_completion_intr
	 * - nvmeibc_ib_net_disconnect
	 * - nvmeibc_ib_nordda_channel_try_disconnect
	 * - lock_send_completion (LOCK)
	 * - try_connect_2nd_ch (LOCK)
	 * - free_premature_lock_ch (LOCK)
	 * - nvmeibc_locks_channel_lock_cmd_completion (LOCK)
	 * - drain_defered_queue (LOCK)
	 * - lock_ch_pause (LOCK)
	 * - abort_in_progress_ops (LOCK)
	 * - execute_opr (LOCK)
	 * - nvmeibc_disk_locks_on_completion (LOCK)
	 * - nvmeibc_disk_locks_process_deferred (LOCK)
	 * - locks_rw_disconnect_ch (LOCK)
	 * - on_locks_start_wd_event (LOCK)
	 * - on_locks_end_wd_event (LOCK)
	 ***************************************************************************************************/
	bool comp_ll;
	
	/* If this is a coremask channel, the coremask cookie */
	void *coremask_cookie;

#ifdef DBGDI_REMOVED_IN_PRODUCTION
};

	#define CCHANNEL_DBGDI_set_over_eager(x, val)
#else
	/* Limitation: currently if the last operation spans multiple
	 * lbas, there is one magic for all. It can possibly false negative
	 * the case when there is a small write => long read, and
	 * the corruption is found in last read lbas, but it is normally OK
	 * as mostly IOs are the same size. */
	struct {
		bool was_overeager;
		u64 io_id; /* ever growing IO identifier */
		u64 ch_ptr; /* channel pointer, can be used to identify IO uniquely */

		/* updated by data_blk_fill_for_core_[rd,wr]_[pre,post] funcs */
		struct nvmeibc_channel_dbg_di_magic_data magic_data;
		struct {
			struct {
				u8 opcode;
				u8 ch_type;
				u64 jif;
				u64 dlba;
				u64 len;
			} cur, last;
		} dbg_of_dbg; /*Used to debug the debug*/
	} dbg_di;
};

	#define CCHANNEL_DBGDI_set_over_eager(x, val)  (x).dbg_di.was_overeager = (val)
#endif // DBGDI_REMOVED_IN_PRODUCTION

bool nvmeibc_channel_rm_work_get(void);
void nvmeibc_channel_rm_work_put(void);

int nvmeibc_channel_init(struct nvmeibc_channel *ch, const struct nvmeibc_cinst_params_core *p);
bool nvmeibc_channel_alive(struct nvmeibc_channel *ch);

void nvmeibc_channel_reset(struct nvmeibc_channel *ch);
void nvmeibc_channel_iocmd_cnt(
	struct nvmeibc_channel *ch, enum nvmeib_block_io_op op, int comp_code);

void nvmeibc_channel_pcpu_ch_set_cpu(struct nvmeibc_channel *ch, int cpu, bool comp_ll);
void nvmeibc_channel_pcpu_ch_clear_cpu(struct nvmeibc_channel *ch);

static inline bool nvmeibc_channel_is_ll_pcpu_ch(struct nvmeibc_channel* ch)
{
	if (ch->comp_ll) {
		BUG_ON(ch->comp_cpu < 0 || ch->comp_cpu >= NVMEIB_DFLT_MAX_CPUS);
		return true;
	}
	return false;
}

static inline bool nvmeibc_channel_is_pcpu_ch(struct nvmeibc_channel* ch) {
	return (ch->comp_cpu >= 0 && ch->comp_cpu < NVMEIB_DFLT_MAX_CPUS && ch->comp_cpu != NVMEIB_CPU_INVALID);
}

static inline int nvmeibc_channel_pcpu_ch_get_cpu(struct nvmeibc_channel *ch)
{
	return ch->comp_cpu;
}

static inline bool nvmeibc_channel_is_coremask_ch(struct nvmeibc_channel *ch) {
	return (nvmeibc_channel_is_pcpu_ch(ch) && !nvmeibc_channel_is_ll_pcpu_ch(ch) && ch->coremask_cookie);
}

static inline int nvmeibc_channel_get_coremask_ch_cpu(struct nvmeibc_channel *ch) {
	return nvmeibc_channel_is_coremask_ch(ch) ? ch->comp_cpu : NVMEIB_CPU_INVALID;
}

static inline void *nvmeibc_channel_get_coremask_ch_cookie(struct nvmeibc_channel *ch) {
	return ch->coremask_cookie;
}

static inline void nvmeibc_channel_set_coremask_ch_cookie(struct nvmeibc_channel *ch, void *cookie) {
	BUG_ON(nvmeibc_channel_is_ll_pcpu_ch(ch));
	ch->coremask_cookie = cookie;
}

#define NVMEIBC_CHANNEL_DEBUG_PCPU_LOCK	0
#ifdef NVMEIBC_CHANNEL_DEBUG_PCPU_LOCK
struct nvmeibc_channel_pcpu_lock_info {
	struct nvmeibc_channel *ch;
	int pid;
	const char *file;
	const char *fn;
	int line;
};

extern struct nvmeibc_channel_pcpu_lock_info nvmeibc_channel_pcpu_lock_info[NR_CPUS];

static inline void nvmeibc_channel_pcpu_set_lock_info(struct nvmeibc_channel *ch, const char *file, const char *fn, int line)
{
	int cpu = smp_processor_id();\
	nvmeibc_channel_pcpu_lock_info[cpu].ch = ch;\
	nvmeibc_channel_pcpu_lock_info[cpu].file = file;\
	nvmeibc_channel_pcpu_lock_info[cpu].fn = fn;
	nvmeibc_channel_pcpu_lock_info[cpu].line = line;
	nvmeibc_channel_pcpu_lock_info[cpu].pid = current->pid;\
}

static inline void nvmeibc_channel_pcpu_reset_lock_info(void)
{
	int cpu = smp_processor_id();
	nvmeibc_channel_pcpu_lock_info[cpu].ch = NULL;
	nvmeibc_channel_pcpu_lock_info[cpu].file = NULL;
	nvmeibc_channel_pcpu_lock_info[cpu].fn = NULL;
	nvmeibc_channel_pcpu_lock_info[cpu].line = -1;
	nvmeibc_channel_pcpu_lock_info[cpu].pid = -1;
}

#else
#define nvmeibc_channel_pcpu_set_lock_info(ch, file, fn, line) { (void)ch; (void)file; (void)fn; (void)line; }
#define nvmeibc_channel_pcpu_reset_lock_info()
#endif

static inline void __nvmeibc_channel_spin_lock(struct nvmeibc_channel *ch, const char *file, const char *fn, int line)
{
	if (nvmeibc_channel_is_ll_pcpu_ch(ch)) {
		BUG_ON(get_cpu() != nvmeibc_channel_pcpu_ch_get_cpu(ch));
		nvmeibc_channel_pcpu_set_lock_info(ch, file, fn, line);
	} else {
		spin_lock(&ch->spinlock);
	}
	ch->locking_pid = current->pid;
}

#define nvmeibc_channel_spin_lock(ch) __nvmeibc_channel_spin_lock(ch, __FILE__, __FUNCTION__, __LINE__)

static inline void nvmeibc_channel_spin_unlock(struct nvmeibc_channel *ch)
{
	if (nvmeibc_channel_is_ll_pcpu_ch(ch)) {
		BUG_ON(smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(ch));
		ch->locking_pid = -1;
		nvmeibc_channel_pcpu_reset_lock_info();
		put_cpu();
	} else {
		ch->locking_pid = -1;
		spin_unlock(&ch->spinlock);
	}
}

static inline void __nvmeibc_channel_spin_lock_irqsave(
	struct nvmeibc_channel *ch, unsigned long *pflags, const char *file, const char *fn, int line)
{
	if (nvmeibc_channel_is_ll_pcpu_ch(ch)) {
		BUG_ON(get_cpu() != nvmeibc_channel_pcpu_ch_get_cpu(ch));
		local_irq_save(*pflags);
		nvmeibc_channel_pcpu_set_lock_info(ch, file, fn, line);
	} else {
		spin_lock_irqsave(&ch->spinlock, *pflags);
	}
	ch->locking_pid = current->pid;
}

#define nvmeibc_channel_spin_lock_irqsave(ch, pflags) __nvmeibc_channel_spin_lock_irqsave(ch, pflags, __FILE__, __FUNCTION__, __LINE__)

static inline void nvmeibc_channel_spin_unlock_irqrestore(
	struct nvmeibc_channel *ch, unsigned long flags)
{
	if (nvmeibc_channel_is_ll_pcpu_ch(ch)) {
		BUG_ON(smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(ch));
		ch->locking_pid = -1;
		nvmeibc_channel_pcpu_reset_lock_info();
		local_irq_restore(flags);
		put_cpu();
	} else {
		ch->locking_pid = -1;
		spin_unlock_irqrestore(&ch->spinlock, flags);
	}
}

static inline bool nvmeibc_channel_already_locked(struct nvmeibc_channel *ch)
{
	if (nvmeibc_channel_is_ll_pcpu_ch(ch)) {
		if (preemptible())
			return false;
		BUG_ON(smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(ch));
		return ch->locking_pid == current->pid;
	}
	/* work assumption: ch is always locked with irqsave */
	return irqs_disabled() && ch->locking_pid == current->pid;
}

void nvmeibc_channel_version_update_nolock(struct nvmeibc_channel *ch);
void nvmeibc_channel_version_invalidate_nolock(struct nvmeibc_channel *ch);

bool nvmeibc_channel_version_get_tracked(struct nvmeibc_channel *ch, u64 *version, unsigned *change_cookie);
#define nvmeibc_channel_version_get(_ch, _ver) nvmeibc_channel_version_get_tracked(_ch, _ver, NULL)

bool nvmeibc_channel_version_tracked_changed(struct nvmeibc_channel *ch, unsigned change_cookie);

bool nvmeibc_channel_try_use_req_info(struct nvmeibc_channel *ch);
void nvmeibc_channel_end_use_req_info(struct nvmeibc_channel *ch);
void nvmeibc_channel_wait_used_req_infos(struct nvmeibc_channel *ch);

void nvmeibc_channel_init_dbgdi_uniq(struct nvmeibc_channel *ch,
                                     const struct nvmeibc_cinst_params_core *p,
                                     int lionic_index, int rionic_index,
                                     int qpn);

#endif
