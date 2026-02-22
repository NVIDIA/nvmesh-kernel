/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "block/nvmeibc_block_common.h"
#include "nvmeibc_b_dp_iostats.h"

// The counter's LSB bit being set indicates that the counter has been modified.
#define COUNTER_TO_VAL_SHIFT  (1)
#define COUNTER_CHANGED_MASK (1ULL << (COUNTER_TO_VAL_SHIFT - 1))

/************************** nvmeibcb_dp_io_fail_mgr ***************************/
static inline const char* __critcial_cmd_error_to_string(int cmd_rv)
{
	switch (cmd_rv) {
	case 128                     :  return "LBA Out of Range";
	case EPERM_READ_FAIL_NO_RETRY:  return "Multi-bad sector?";
	case EPERM_WRITE_FAIL_NO_RETRY: return "Final Write filure";
	case -ENOEXEC:                  return "Unable to acquire locks";
	default:                        return "No extra info";
	}
}

#define DEFAULT_ALLOWED_IO_FAILS (7)	// Gives bdev grace for N failed IO's. When cntr reaches zero suspends the blockdevice
#define DISABLED_MARKER       (-100)	// If counter is set to -100 or below then mgr is disabled

atomic_t warn_on_io_err_cntr = ATOMIC_INIT(DEFAULT_ALLOWED_IO_FAILS);
bool nvmeibcb_dp_io_fail_mgr_inspect(const struct nvmeibc_block_command *cmds, int i)
{
	struct nvmeibc_block_device *nd = cmds->o->nd;
	const struct nvmeibc_block_command *c = &cmds[i];
	const char *vname = nd->name, *dname = c->ds->disk->ops.get_name(c->ds->disk);
	const u64 dlba = c->iocmd->reqs1.disk_address;
	const bool is_detaching = nvmeibc_block_status_is_detaching(nd->status);
	const int n_warns = (is_detaching ?
							atomic_read(&warn_on_io_err_cntr) :						// Do not take those ios into account, because counter is global (by design) for all volumes
							atomic_dec_if_positive(&warn_on_io_err_cntr));			// Note: result is allways decreased even if atomic dec did not succeed;
	// WARN_ON(c->o_rv == 0);			// This sanity check is disabled because syncs abuse this function to execute not on a specific disk command failure.
	_NE(t_a1_failmgr, DMESG_PREFIX("@DEV_NAME") ": {@O_DBG_ID}, op=@BLOCK_IO_OP error=@O_RV, disk=@DISK_NAME [@DLBA..@DLBA) detaching=@BOOL_YN, n_warns=@INT, info=@STR", vname, cmds->o->dbg_id, cmds->o->op, c->o_rv, dname, dlba, (dlba+c->nlbas), is_detaching, n_warns, __critcial_cmd_error_to_string(c->o_rv));
	if (is_detaching)
		return false;	// Volume is detaching do not retry. Any IO failures are accepted including (timout, bad sector coz sync was not launched, etc)

	if (n_warns < DISABLED_MARKER) {			// Mgr is disabled: '<' not '<=' coz dec affects result but not atomic itself
		_NE(t_a2_failmgr, DMESG_PREFIX("@DEV_NAME") ": returning IO error to user-space", vname);
		return false;				// Dont retry operation. BIO failure
	} else if (n_warns >= 0) {		// After enough decs, becomes -1
		WARN(true, DMESG_PREFIX("%s: ") "op=%d failed rv=%d. %d more attempts allowed\n", vname, cmds->o->op, c->o_rv, n_warns);	// Just WARN and retry
	} else {						// n_warns == -1, atomic == 0
		_NE_to_user(t_a3_failmgr, DMESG_PREFIX("@DEV_NAME"), "Volume experienced numerous io errors. Suspending it to avoid further degradation and possible data corruption. Contact Nvidia support, to manually revive it. Error code: 1033.", vname);
		nvmeibc_block_suspend(nd, NULL, NULL);
	}
	return true;					// Do retry after manual revive
}

void nvmeibcb_dp_io_fail_mgr_init(struct nvmeibcb_dp_io_fail_mgr *m)
{
	memset(m, 0, sizeof(*m));
}

void nvmeibcb_dp_io_fail_mgr_disable(struct nvmeibcb_dp_io_fail_mgr *m)
{
	nvmeibcb_dp_io_fail_mgr_set_limit(m, DISABLED_MARKER);
}

int nvmeibcb_dp_io_fail_mgr_set_limit(struct nvmeibcb_dp_io_fail_mgr *m, int n_ios)
{
	const int rv = atomic_read(&warn_on_io_err_cntr);
	atomic_set(&warn_on_io_err_cntr, n_ios);
	_NI(t_a4_failmgr, "io_err_cntr @INT -> @INT", rv, n_ios);
	(void)m;
	return rv;
}

void nvmeibcb_dp_io_fail_mgr_reset_limit(struct nvmeibcb_dp_io_fail_mgr *m)
{
	if (atomic_read(&warn_on_io_err_cntr) > DISABLED_MARKER) {	// If not disabled reset to default
		// Daniel: here asuming only 1 technitian works with block device so no one will disable the mgr here. Else this is a race condition
		atomic_set(&warn_on_io_err_cntr, DEFAULT_ALLOWED_IO_FAILS);
	}
	(void)m;
}

static void nvmeibcb_dp_io_fail_mgr_tostring(const struct nvmeibcb_dp_io_fail_mgr *m, struct nvmeib_txt *txt)
{
	const int warns = atomic_read(&warn_on_io_err_cntr); // Todo: Store counter per block device per disk and not globally
	nvmeib_txt_append(txt, "{sus_thresh=%d, n_binfo=%d/%d, n_htr0=%d, EC4571=%d}\n",
		warns, m->n_binfo_errors, m->n_htr_null_uuids, m->n_binfo_copy_owner_error, m->n_blocked_cont_EC_4571);
}

static void nvmeibcb_dp_io_fail_mgr_tojson(const struct nvmeibcb_dp_io_fail_mgr *m, struct jdr *jdr)
{
	const int warns = atomic_read(&warn_on_io_err_cntr); // Todo: Store counter per block device per disk and not globally
	jdr_object_scope(jdr, "internal_err");
	jdr_write_var(jdr, sus_thresh, warns);
	jdr_write_var(jdr, n_binfo_err, m->n_binfo_errors);
	jdr_write_var(jdr, n_cpbinfo_fix, m->n_htr_null_uuids);
	jdr_write_var(jdr, n_htr_null_uuids, m->n_binfo_copy_owner_error);
	jdr_write_var(jdr, n_EC4571, m->n_blocked_cont_EC_4571);
}

void nvmeibcb_dp_io_fail_mgr_binfo_err(struct nvmeibcb_dp_io_fail_mgr *m)
{
	m->n_binfo_errors++;			// Todo: Make this atomic via lock
}

void nvmeibcb_dp_io_fail_mgr_binfo_copy_errfix(struct nvmeibcb_dp_io_fail_mgr *m)
{
	m->n_binfo_copy_owner_error++;	// Todo: Make this atomic via lock
}

void nvmeibcb_dp_io_fail_mgr_htr_null_uuids_err(struct nvmeibcb_dp_io_fail_mgr *m, struct nvmeibc_block_device *dev)
{
	m->n_htr_null_uuids++;			// Todo: Make this atomic via lock
	if (m->n_htr_null_uuids > 50) {
		nvmeibc_block_suspend(dev, NULL, NULL);
	}
}

void nvmeibcb_dp_io_fail_mgr_blocked_cont(struct nvmeibcb_dp_io_fail_mgr *m)
{
	m->n_blocked_cont_EC_4571++;			// Todo: Make this atomic via lock
}

/***************************** dp_io_stats *****************************/
void dp_io_stats_init(struct dp_io_stats *t)
{
	spin_lock_init(&t->lock);
	dp_io_stats_clear(t);
	nvmeibcb_dp_io_fail_mgr_init(&t->mgr);
}

void dp_io_stats_clear(struct dp_io_stats *t)
{
	unsigned long flags;
	int indx;
	spin_lock_irqsave(&t->lock, flags);
	for (indx = 0; indx <= DP_IO_STATS_OTHER; indx++) {
		t->n.countrs[indx] = 0ULL | COUNTER_CHANGED_MASK; // setting the counter value to COUNTER_CHANGED_MASK will trigger counter reporting on the next report loop
	}
	for (indx = 0; indx < NVMEIB_BLOCK_IO_OP_MAX_SYNC_TYPES; indx++) {
		t->n.sync_counters[indx] = 0ULL | COUNTER_CHANGED_MASK; // Set the LSB to 1 to indicate that the counter was changed
	}
	nvmeibcb_dp_io_fail_mgr_init(&t->mgr);
	spin_unlock_irqrestore(&t->lock, flags);
}

static const char *__dp_io_stats_names(enum dp_iostats_names name)
{
	switch (name) {
	case DP_IO_STATS_CRITICAL_FAIL: return "critical_fail";
	case DP_IO_STATS_TIMED_OUT: return "timed_out";
	case DP_IO_STATS_SUSPED_FAIL: return "suspended_fail";
	case DP_IO_STATS_IGNORED_ERR: return "ignored_err";
	case DP_IO_STATS_CANCELED_BY_RIDER: return "canceled_by_rider";
	case DP_IO_STATS_ILLEGAL_TRIMS: return "illegal_trims";
	case DP_IO_STATS_RESUBMITTED: return "resubmitted";
	case DP_IO_STATS_RESUBMITTED_STARTED: return "started resubmission";
	case DP_IO_STATS_DNR_BAD_SECTORS: return "dnr_bad_sectors";
	case DP_IO_STATS_SOFTWARE_READ_FAIL_COUNT: return "software_read_fail_count";
	case DP_IO_STATS_MD_MARKED_INVALID_ERRORS: return "md_errors";
        case DP_IO_STATS_MD_EDIC_CHECK_ERRORS: return "md_edic_check_errors";
	case DP_IO_STATS_EDIC_ERRORS: return "edic_errors";
	case DP_IO_STATS_LOCK_OP_FAILED: return "lock_op_failed";
	case DP_IO_STATS_LOCKSET_FAILED: return "lockset_failed";
	case DP_IO_STATS_LOCK_TRANSFER_ACCEPTED_COUNT: return "lock_transfer_accepted_count";
	case DP_IO_STATS_LOCK_TRANSFER_REJECTED_COUNT: return "lock_transfer_rejected_count";
	case DP_IO_STATS_LOCK_CONTENDED_COUNT: return "lock_contended_count";
	case DP_IO_STATS_LOCK_STALE_COUNT: return "lock_stale_count";
	case DP_IO_STATS_WAIT_FOR_LOCKSET_CANCELED_COUNT: return "wait_for_lockset_canceled_count";
	case DP_IO_STATS_OTHER: return "other";
	default:
		return "???";
	}
}

/* The LSB is for indicating that the counter was changed since it was last accessed for read */
void dp_io_stats_add(struct dp_io_stats * t, u64 *counter, int count)
{
	unsigned long flags;
	if (likely(count != 0)) {
		spin_lock_irqsave(&t->lock, flags); /* Frequently called from interrupt */
		*counter += (count << 1);
		*counter |= COUNTER_CHANGED_MASK; // Indicate that the counter was changed by setting the LSB to 1
		spin_unlock_irqrestore(&t->lock, flags);
	}
}

static u64 __get_cnt_val(const struct dp_io_stats_cntrs *t, enum dp_iostats_names name)
{
	return t->countrs[name] >> COUNTER_TO_VAL_SHIFT;
}

u64 dp_io_stats_get_counter(const struct dp_io_stats *t, enum dp_iostats_names name)
{
	unsigned long flags;
	u64 val;
	spin_lock_irqsave((spinlock_t *)&t->lock, flags);
	val = __get_cnt_val(&t->n, name);
	spin_unlock_irqrestore((spinlock_t *)&t->lock, flags);
	return val;
}

u64 dp_io_stats_get_sync_counter(const struct dp_io_stats *t, enum nvmeib_block_io_op sync_type)
{
	int sync_id = SYNC_TYPE_ID(sync_type);
	unsigned long flags;
	u64 val;
	if (sync_id >= NVMEIB_BLOCK_IO_OP_MAX_SYNC_TYPES) {
		return 0;
	}
	spin_lock_irqsave((spinlock_t *)&t->lock, flags);
	val = t->n.sync_counters[sync_id] >> 1;
	spin_unlock_irqrestore((spinlock_t *)&t->lock, flags);
	return val;
}


void dp_io_stats_clear_counter(struct dp_io_stats *t, enum dp_iostats_names name)
{
	unsigned long flags;
	spin_lock_irqsave((spinlock_t *)&t->lock, flags);
	if (t->n.countrs[name] >=  0x2) // we only want to report counters clear that the prev value was not 0
		t->n.countrs[name] = 0ULL | COUNTER_CHANGED_MASK; // set the LSB to 1 to indicate that the counter was changed
	spin_unlock_irqrestore((spinlock_t *)&t->lock, flags);
}

#define IO_ERRS_VALS                                                                                           \
	__get_cnt_val(t, DP_IO_STATS_CRITICAL_FAIL), __get_cnt_val(t, DP_IO_STATS_TIMED_OUT),                  \
	__get_cnt_val(t, DP_IO_STATS_SUSPED_FAIL), __get_cnt_val(t, DP_IO_STATS_IGNORED_ERR),                  \
	__get_cnt_val(t, DP_IO_STATS_CANCELED_BY_RIDER), __get_cnt_val(t, DP_IO_STATS_ILLEGAL_TRIMS),          \
	__get_cnt_val(t, DP_IO_STATS_DNR_BAD_SECTORS), __get_cnt_val(t, DP_IO_STATS_MD_MARKED_INVALID_ERRORS), \
	__get_cnt_val(t, DP_IO_STATS_MD_EDIC_CHECK_ERRORS), __get_cnt_val(t, DP_IO_STATS_LOCK_OP_FAILED),      \
	__get_cnt_val(t, DP_IO_STATS_LOCKSET_FAILED), __get_cnt_val(t, DP_IO_STATS_RESUBMITTED_STARTED),       \
	__get_cnt_val(t, DP_IO_STATS_RESUBMITTED), __get_cnt_val(t, DP_IO_STATS_TIMED_OUT)

void dp_io_stats_tostring(const struct dp_io_stats *_t, struct nvmeib_txt *txt)
{
	const struct dp_io_stats_cntrs *t = &_t->n; // Cast to non-const for easier access

	nvmeib_txt_append(txt, "Failed IO: crit=%llu, detach=%llu, ignore=%llu, rider=%llu, trim=%llu, other=%llu,"
		" bad_sectors=%llu, metadata_marked_invalid_err=%llu, edic_discrepencies=%llu, "
		"lock_cmds_failed=%llu, lockset_failed=%llu, (resub: in=%llu, out=%llu, tout=%llu)",
		IO_ERRS_VALS);
	nvmeibcb_dp_io_fail_mgr_tostring(&_t->mgr, txt);
}

void dp_io_stats_tojson(const struct dp_io_stats *_t, struct jdr *jdr)
{
	const struct dp_io_stats_cntrs *t = &_t->n; // Cast to non-const for easier access

	{
		jdr_object_scope(jdr, "failed_io");
		jdr_write_var(jdr, critical, __get_cnt_val(t, DP_IO_STATS_CRITICAL_FAIL));
		jdr_write_var(jdr, detach, __get_cnt_val(t, DP_IO_STATS_SUSPED_FAIL));
		jdr_write_var(jdr, ignore, __get_cnt_val(t, DP_IO_STATS_IGNORED_ERR));
		jdr_write_var(jdr, rider_cancel, __get_cnt_val(t, DP_IO_STATS_CANCELED_BY_RIDER));
		jdr_write_var(jdr, illegal_trims, __get_cnt_val(t, DP_IO_STATS_ILLEGAL_TRIMS));
		jdr_write_var(jdr, other, __get_cnt_val(t, DP_IO_STATS_OTHER));
	}
	jdr_write_var(jdr, bad_sectors, __get_cnt_val(t, DP_IO_STATS_DNR_BAD_SECTORS));
	jdr_write_var(jdr, metadata_mark_invalid_errors, __get_cnt_val(t, DP_IO_STATS_MD_MARKED_INVALID_ERRORS));
	jdr_write_var(jdr, edic_discrepencies, __get_cnt_val(t, DP_IO_STATS_MD_EDIC_CHECK_ERRORS));
	jdr_write_var(jdr, lock_cmds_failed, __get_cnt_val(t, DP_IO_STATS_LOCK_OP_FAILED));
	jdr_write_var(jdr, lockset_failed, __get_cnt_val(t, DP_IO_STATS_LOCKSET_FAILED));
	{
		jdr_object_scope(jdr, "resubmittion");
		jdr_write_var(jdr, in, __get_cnt_val(t, DP_IO_STATS_RESUBMITTED_STARTED));
		jdr_write_var(jdr, out, __get_cnt_val(t, DP_IO_STATS_RESUBMITTED));
		jdr_write_var(jdr, timed_out, __get_cnt_val(t, DP_IO_STATS_TIMED_OUT));
	}
	nvmeibcb_dp_io_fail_mgr_tojson(&_t->mgr, jdr);
}

static bool __is_counter_in_use(const char *name)
{
	return (name && name[0] && name[0] != '?'); // Check if the name is valid and not a placeholder
}

void dp_io_stats_trace(u32 blk_dev_id, struct dp_io_stats *t, bool changed_only)
{
	unsigned long flags;
	struct dp_io_stats_cntrs *n = &t->n;
	int indx;
	spin_lock_irqsave(&t->lock, flags);
	for (indx = 0; indx <= DP_IO_STATS_OTHER; indx++) {
		if (!changed_only || n->countrs[indx] & COUNTER_CHANGED_MASK) { // If LSB is not set, then no change since last read
			u64 cntr = n->countrs[indx] >> COUNTER_TO_VAL_SHIFT; // Get the value without the LSB
			NVMEIB_LOG_METRICS("blockid=@UINT @STR=@BLK_FLOW_COUNTER", _I, /*Default Scope*/, dp_info_stats,
					   blk_dev_id, __dp_io_stats_names(indx), cntr);

			n->countrs[indx] &= ~COUNTER_CHANGED_MASK; // Clear the LSB after logging
		}
	}

	for (indx = 0; indx < NVMEIB_BLOCK_IO_OP_MAX_SYNC_TYPES; indx++) {
		enum nvmeib_block_io_op sync_type = SYNC_IO_OP(indx);
		const char *counter_name = nvmeib_block_io_op_str(sync_type);
		if (__is_counter_in_use(counter_name) && (!changed_only || n->sync_counters[indx] & COUNTER_CHANGED_MASK)) { // If LSB is not set, then no change since last read
			u64 cntr = n->sync_counters[indx] >> COUNTER_TO_VAL_SHIFT; // Get the value without LSB
			NVMEIB_LOG_METRICS("blockid=@UINT @STR counter=@BLK_FLOW_COUNTER", _I, /*Default Scope*/,
					   dp_info_sync_stats, blk_dev_id, nvmeib_block_io_op_str(sync_type), cntr);
			n->sync_counters[indx] &= ~COUNTER_CHANGED_MASK; // Clear the LSB after logging
		}
	}
	spin_unlock_irqrestore(&t->lock, flags);
}
