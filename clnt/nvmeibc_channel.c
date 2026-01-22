/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_channel.h"
#include "nvmeibc_main.h"
#include "nvmeib_wd.h"
#include "nvmeib_utils.h"
#include "nvmeibc_trace.h"
#include "core/nvmeibc_core_common.h"
#include "nvmeibc_ib_nordda_channel.h"

#ifdef NVMEIBC_CHANNEL_DEBUG_PCPU_LOCK
struct nvmeibc_channel_pcpu_lock_info nvmeibc_channel_pcpu_lock_info[NR_CPUS];
#endif

static void ioch_drained_init(struct nvmeibc_channel *ch)
{
	ch->bailed_cmds.cs_gid = 1;
	INIT_LIST_HEAD(&ch->bailed_cmds.link);
	INIT_LIST_HEAD(&ch->bailed_cmds.list);
}

int nvmeibc_channel_init(struct nvmeibc_channel *ch, const struct nvmeibc_cinst_params_core *p)
{
	int rv = 0;

	NFIN;
	nvmeibc_cinst_get_core_p(ch) = p;
	ch->name[0] = '?';
	spin_lock_init(&ch->spinlock);
	init_waitqueue_head(&ch->wqh);
	atomic_set(&ch->tag, 0);
	atomic_set(&ch->dying, 0);
	ch->wd = nvmeibc_get_watchdog(nvmeibc_cinst_get_core_p(ch));
	ch->locking_pid = -1;
	WRITE_ONCE(ch->version, 0);
	WRITE_ONCE(ch->version_valid, false);
	seqcount_init(&ch->version_seq);
	ch->disk_version = 0;
	atomic_set(&ch->n_freed, 0);
	ioch_drained_init(ch);
	spin_lock_init(&ch->reused_bb_list_guard);
	INIT_LIST_HEAD(&ch->reused_bb_list);
	ch->reused_bb_cnt = 0;
	ch->reused_bb_lru_jif = 0;
	ch->comp_cpu = NVMEIB_CPU_INVALID;
	
	NFOUT;
	return rv;
}

void nvmeibc_channel_pcpu_ch_set_cpu(struct nvmeibc_channel *ch, int cpu, bool comp_ll)
{
	ch->comp_cpu = cpu;
	if (cpu != NVMEIB_CPU_INVALID && comp_ll) {
		ch->comp_ll = true;
		ch->wd = nvmeibc_get_watchdog_pcpu(nvmeibc_cinst_get_core_p(ch), cpu);
	}
}
EXPORT_SYMBOL(nvmeibc_channel_pcpu_ch_set_cpu);

void nvmeibc_channel_pcpu_ch_clear_cpu(struct nvmeibc_channel *ch)
{
	ch->wd = nvmeibc_get_watchdog(nvmeibc_cinst_get_core_p(ch));
	ch->comp_cpu = NVMEIB_CPU_INVALID;
	ch->comp_ll = false;
}
EXPORT_SYMBOL(nvmeibc_channel_pcpu_ch_clear_cpu);

bool nvmeibc_channel_alive(struct nvmeibc_channel *ch)
{
	return atomic_read(&ch->dying) == 0;
}

void nvmeibc_channel_version_update_nolock(struct nvmeibc_channel *ch)
{
	u64 ch_ver;
	bool ver_valid = nvmeibc_channel_version_get(ch, &ch_ver);

	_NT(trace_nvmeibc_channel_version_update,
	    "ch @CH_PTR @CH_NAME (type @CH_TYPE_TO_STR), inc from version=@LLU",
	    ch, ch->name, ch_type_to_str(ch->ct), ch_ver);
	WARN_ONCE(ver_valid, "ch=%p", ch);

	/* Use seq-lock abstraction to handle SMP acquire/release semantics */
	write_seqcount_begin(&ch->version_seq);
	ch_ver = READ_ONCE(ch->version);
	NVMEIB_INC_TAG_VERSION(ch_ver);
	WRITE_ONCE(ch->version, ch_ver);
	WRITE_ONCE(ch->version_valid, true);
	write_seqcount_end(&ch->version_seq);
}

void nvmeibc_channel_version_invalidate_nolock(struct nvmeibc_channel *ch)
{
	u64 ch_ver;
	bool ver_valid = nvmeibc_channel_version_get(ch, &ch_ver);

	_NT(trace_nvmeibc_channel_version_invalidate,
	    "ch @CH_PTR @CH_NAME (type @CH_TYPE_TO_STR), version=@LLU, valid=@BOOL",
	    ch, ch->name, ch_type_to_str(ch->ct), ch_ver, ver_valid);

	/* Use seq-lock abstraction to handle SMP acquire/release semantics */
	write_seqcount_begin(&ch->version_seq);
	WRITE_ONCE(ch->version_valid, false);
	write_seqcount_end(&ch->version_seq);
}

bool nvmeibc_channel_version_get_tracked(struct nvmeibc_channel *ch, u64 *version, unsigned *change_cookie)
{
	unsigned seq;
	bool valid;

	/* Use seq-lock abstraction to handle SMP acquire/release semantics */
	do {
		seq = read_seqcount_begin(&ch->version_seq);
		if ((valid = READ_ONCE(ch->version_valid))) {
			*version = READ_ONCE(ch->version);
		}
	} while (read_seqcount_retry(&ch->version_seq, seq));

	if (change_cookie) {
		/* Return the seq value for a future call to nvmeibc_channel_version_changed() */
		*change_cookie = seq;
	}

	return valid;
}

/* Checks if the version has changed since the call to nvmeibc_channel_version_get_tracked
 * Uses the seqcount value from read_seqcount_begin */
bool nvmeibc_channel_version_tracked_changed(struct nvmeibc_channel* ch, unsigned change_cookie)
{
	return raw_read_seqcount(&ch->version_seq) != change_cookie;
}

/* Try inc channel's reqs ref-cnt.
 *
 * Called by (or why req info is valid):
 * 1. get-req-info            : Disk lock taken and after we've got req-info
 *  	<-- start-io-nordda   : remove-work cant run, wait for ch init to comp
 *  	<-- execute-io-remote : ---
 * 2. send/recv completion	  : channel (and net) still valid, no LAST WQE yet
 */
bool nvmeibc_channel_try_use_req_info(struct nvmeibc_channel *ch)
{
	int rv;

	NFIN;
	if (!(rv = atomic_inc_not_zero_hint(&ch->n_user_reqs, 1)))
		_ND(trace_channel_nvmeibc_channel_try_use_req_info, "ch @CH_NAME (type @CH_TYPE_TO_STR), cant inc", ch->name, ch_type_to_str(ch->ct));

	NFOUT;
	return !!rv;
}

/* Dec channel's reqs ref-cnt and if last, complete()
 *
 * Called by:
 * 1. put-req-info : no pending block-cmd or execute pending-io err
 * 2. execute-io   : successful execution (post-send)
 */
void nvmeibc_channel_end_use_req_info(struct nvmeibc_channel *ch)
{
	int n;

	NFIN;
	if (!(n = atomic_dec_return(&ch->n_user_reqs))) {
		BUG_ON(!ch->reqs_comp);
		complete(ch->reqs_comp);
	}
	_ND(trace_channel_nvmeibc_channel_end_use_req_info, "ch @CH_NAME (type @CH_TYPE_TO_STR), remained @N_USER_REQS n_user_reqs",
		ch->name, ch_type_to_str(ch->ct), n);
	NFOUT;
}


#define NVMEIBC_NRCH_WAIT_REQS (2 * HZ)
void nvmeibc_channel_wait_used_req_infos(struct nvmeibc_channel *ch)
{
	DECLARE_COMPLETION_ONSTACK(comp);

	int n, a = 0, rvw;

	NFIN;
	if (atomic_read(&ch->n_user_reqs) > 0) {
		ch->reqs_comp = &comp;
		if ((n = atomic_dec_return(&ch->n_user_reqs))) {
			_NT(trace_channel_nvmeibc_channel_wait_used_req_infos, "ch @CH_NAME (type @CH_TYPE_TO_STR): wait for @N_USER_REQS req-info to finish",
			   ch->name, ch_type_to_str(ch->ct), n);
#if 0
			wait_for_completion(ch->reqs_comp);
#else
			while ((rvw = wait_for_completion_interruptible_timeout(
				ch->reqs_comp, NVMEIBC_NRCH_WAIT_REQS)) <= 0) {
				_NW(warn_channel_nvmeibc_channel_wait_used_req_infos, "ch @CH_NAME (type @CH_TYPE_TO_STR): wait for @N_USER_REQS req to finish, attempt @START_IOCH_ATTEMPT_CTR",
					ch->name, ch_type_to_str(ch->ct), n, a);
				a++;
			}
#endif
		}
		ch->reqs_comp = NULL;
	}
	else
		_ND(trace_1_channel_nvmeibc_channel_wait_used_req_infos, "ch @CH_NAME (type @CH_TYPE_TO_STR) release prior init",
			ch->name, ch_type_to_str(ch->ct));
	NFOUT;
}

void nvmeibc_channel_reset(struct nvmeibc_channel *ch)
{
	/* clear counters */
	ch->cnt_io_ok = 0;
	ch->cnt_io_fail = 0;
	ch->cnt_read_ok = 0;
	ch->cnt_read_fail = 0;
	ch->cnt_write_ok = 0;
	ch->cnt_write_fail = 0;
	ch->cnt_trim_ok = 0;
	ch->cnt_trim_fail = 0;
}

void nvmeibc_channel_iocmd_cnt(
	struct nvmeibc_channel *ch, enum nvmeib_block_io_op op, int comp_code)
{
	if (comp_code == 0) {
		ch->cnt_io_ok++;
		if (op == NVMEIB_BLOCK_IO_OP_READ)
			ch->cnt_read_ok++;
		else if (nvmeib_block_io_op_is_write(op))
			ch->cnt_write_ok++;
		else if (op == NVMEIB_BLOCK_IO_OP_DISCARD)
			ch->cnt_trim_ok++;
	} else {
		ch->cnt_io_fail++;
		if (op == NVMEIB_BLOCK_IO_OP_READ)
			ch->cnt_read_fail++;
		else if (nvmeib_block_io_op_is_write(op))
			ch->cnt_write_fail++;
		else if (op == NVMEIB_BLOCK_IO_OP_DISCARD)
			ch->cnt_trim_fail++;
	}
}

extern unsigned nvmeibc_max_ioch_rm_works;
extern atomic_t nvmeibc_num_ioch_rm_works;
inline bool nvmeibc_channel_rm_work_get(void)
{
	return atomic_add_unless(&nvmeibc_num_ioch_rm_works,
							 1, nvmeibc_max_ioch_rm_works);
}
inline void nvmeibc_channel_rm_work_put(void)
{
	int n;
	n = atomic_dec_return(&nvmeibc_num_ioch_rm_works);
	BUG_ON(n < 0);
}

void nvmeibc_channel_init_dbgdi_uniq(struct nvmeibc_channel *ch,
                                     const struct nvmeibc_cinst_params_core *p,
                                     int lionic_index, int rionic_index,
                                     int qpn) {
#ifdef DBGDI_REMOVED_IN_PRODUCTION
	(void)ch; (void)p; (void)lionic_index; (void)rionic_index; (void)qpn;
#else
	static atomic_t running_uniq            = ATOMIC_INIT(1);
	ch->dbg_di.magic_data.uniq.lionic_index = lionic_index;
	ch->dbg_di.magic_data.uniq.rionic_index = rionic_index;
	ch->dbg_di.magic_data.uniq.qpn          = qpn;
	ch->dbg_di.magic_data.uniq.running_uniq = atomic_inc_return(&running_uniq);
	ch->dbg_di.magic_data.uniq.client_uuid  = *nvmeibc_get_uuid(p);
#endif // DBGDI_REMOVED_IN_PRODUCTION
}

