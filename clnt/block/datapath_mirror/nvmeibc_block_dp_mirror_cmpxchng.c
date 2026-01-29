/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_mirror.h"
#include "nvmeibc_block_dp_mirror_cmpxchng.h"

static inline bool __is_valid_cmpxchng_op(const struct operation *o)
{
	const u64 nlbas = get_op_nlbas(o);
	return ((nlbas == 1) && (o->num_bios == 1) /* Post Elevator bio*/);
}

bool nvmeibc_operation_is_valid_carrier_op(struct operation *o)
{
	const struct bio *bio = o->bios[0]->bio;
	if (__is_bio_from_carrier(bio)) {
		o->md_op.bx = rider_bio_get_bio_extention_from(bio);
	} else if (!o->nd->allow_external_io_on_carrier) {
		return false;
	} else { /* backdoor is enabled, every carrier acts as regular R1 */}
	return true;
}

int dp_dc_mirror_should_ignore_op(const struct operation *o)
{
	if (!nvmeibc_operation_is_valid_carrier_op((void*)o))			// Daniel: Ugly that we remove const. Todo, fix this
		return -EPERM;
	return dp_mirror_should_ignore_op(o);
}

int dp_wcv_mirror_should_ignore_op(const struct operation *o)
{
	if (!nvmeibc_operation_is_valid_carrier_op((void*)o))			// Daniel: Ugly that we remove const. Todo, fix this
		return -EPERM;
	if (nvmeibc_operation_has_bio_extention(o)) {
		const struct bio_extention *bx = o->md_op.bx;
		if (bx->exec.force_read_b4_write && !__is_valid_cmpxchng_op(o))
			return -EINVAL;
		if (bx->exec.give_1st_md_blk_on_endio)
			if (o->op != NVMEIB_BLOCK_IO_OP_READ)	// Todo, verify here that this is a full qlc blockset read of mtv cold recovery
				return -EINVAL;

		if (bx->cb.give.after_locks_taken) {
			if (!nvmeib_block_io_op_is_write(o->op))	// Write operation can use barrier to update wcv2mdv pointer
				return -EINVAL;
		}
	}
	return dp_mirror_should_ignore_op(o);
}

int dp_md_mirror_should_ignore_op(const struct operation *o)
{
	if (!nvmeibc_operation_is_valid_carrier_op((void*)o))			// Daniel: Ugly that we remove const. Todo, fix this
		return -EPERM;
	if (!__is_valid_cmpxchng_op(o))
		return -EINVAL;
	if (nvmeibc_operation_has_bio_extention(o)) {
		#if defined(BLKDEV_SIMULATOR)
			// Simulator does wierd and illegal stuff with MDV volume, that deliberately causes corruption of MTV data
		#else
			if (o->op != NVMEIB_BLOCK_IO_OP_READ) {
				WARN_ON(!o->md_op.bx->exec.force_read_b4_write);		// All metadata Write operations must be cmpxchng, otherwise we are risking data corruption
			}
		#endif
	}
	return dp_mirror_should_ignore_op(o);
}

/******************* dp_md_mirror functions. Move to a different file *********/
#define __get_rider_error_on_locks_taken(o, bx) (((bx->exec.rider_rv_on_callback == 0)&&(!o->topo->phased_out)) ? 0 : -ENXIO) 			// Important: Not executed so locks will not be abandoned if raider wants to abort operation
static void __dp_md_resume_execution_by_rider_on_locks_taken(void *carrier_ctx)
{
	struct nvmeibc_block_command *rldr = carrier_ctx;
	const struct operation *o = rldr->o;
	struct bio_extention *bx = o->md_op.bx;
	const int err = __get_rider_error_on_locks_taken(o, bx);
	bx->cb.get.carrier_ctx = NULL;
	bx->cb.get.fn = NULL;				// Daniel: Just for debug, protect against double callback from rider
	if ((bx->cb.give.before_first_write_cmd)&&(!bx->exec.force_read_b4_write)&&(err == 0))
		bx_carrier_give_cb_to_rider(bx, err, CAR_BX_CB_REASON_BEFORE_WRITE_CB);		// This is just for debug, can be removed in the future
	dp_mirror_exec_func_on_locks_tkn(rldr, 0);	// Locks taken successfully
	dp_cmds_execute_first_stage(rldr->cmdarr, (rldr-rldr->cmdarr), err);
}

int dp_md_mirror_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int err) {
	const struct operation *o = rldr->o;
	if (nvmeibc_operation_has_bio_extention(o) && (err == 0)) {		// Almost always true. False only when MDV accessed through backdoor
		struct bio_extention *bx = o->md_op.bx;
		if (bx->cb.give.after_locks_taken) {
			const bool wait_for_async_callback_to_resume_execution = (o->op != NVMEIB_BLOCK_IO_OP_READ);
			if (wait_for_async_callback_to_resume_execution) {
				bx->cb.get.fn = &__dp_md_resume_execution_by_rider_on_locks_taken;
				bx->cb.get.carrier_ctx = rldr;
				bx_carrier_give_cb_to_rider(bx, err, CAR_BX_CB_REASON_ON_LOCKS_TAKEN_CB);
				return 1;											// Wait for async callback
			} else {
				bx_carrier_give_cb_to_rider(bx, err, CAR_BX_CB_REASON_ON_LOCKS_TAKEN_CB);	// Syncronous branch. Exists for debug only. No real customer usage
				err = __get_rider_error_on_locks_taken(o, bx);
			}
		}
		if ((bx->cb.give.before_first_write_cmd)&&(!bx->exec.force_read_b4_write))
			bx_carrier_give_cb_to_rider(bx, err, CAR_BX_CB_REASON_BEFORE_WRITE_CB);		// This is just for debug, can be removed in the future
	} // Upon error we dont give callback, either retry io or quit with error
	return dp_mirror_exec_func_on_locks_tkn(rldr, err);
}

void dp_md_mirror_exec_func_on_stage_end(struct nvmeibc_block_command *rldr, int *rv)
{
	int i;
	if (rldr->raid_cur_stage == E_CMDS_STAGE_READ_PRE_DATA) {
		rldr->raid_cur_stage = E_CMDS_STAGE_DO_IO_AND_PAR;
		if (*rv == 0) {
			const struct bio_extention *bx = rldr->o->md_op.bx;
			WARN_ON(!bx->exec.force_read_b4_write);					// Impossible
			bx_carrier_give_cb_to_rider(bx, *rv, CAR_BX_CB_REASON_AFTER_READ_CB);
			if (bx->exec.rider_rv_on_callback != 0) {
				*rv = -EXDEV;
				return;
			}

			if (nvmeibc_is_mirror_md_enabled(rldr)) {
				for (i = 1; i < rldr->nraid_siblings; i++)
					__metadata_for_cmd_fill_vals(&rldr[i], (i-1));		// Now we have the correct data to write. Calcualte edic. md already allocated
			}

			if (bx->cb.give.before_first_write_cmd)
				bx_carrier_give_cb_to_rider(bx, *rv, CAR_BX_CB_REASON_BEFORE_WRITE_CB);
		}
	} else {
		dp_mirror_exec_func_on_stage_end(rldr, rv);
	}
}

void dp_md_mirror_calc_should_abandon(struct nvmeibc_block_command *rldr, __attribute__ ((unused)) int lsi)
{
	struct nvmeibc_block_command *c = rldr, *end = &c[c->ncmds];
	const struct bio_extention *bx = rldr->o->md_op.bx;
	struct nvmeibc_cmd_lock *lo = rldr->locksets;
	struct t_abandon aban = {0};
	if (unlikely(rldr->o->op == NVMEIB_BLOCK_IO_OP_READ))
		return; // Special topology where read takes locks, never abandoned

	for (; c < end; c++) {
		if (c->iocmd->reqs->op != NVMEIB_BLOCK_IO_OP_READ)
			__calc_should_abandon_add_cmd(&aban, c);				// In cmpxchng block, it is ok for read to succeed and write to fail
		else
			WARN_ON(!bx->exec.force_read_b4_write);				// Just sanity, todo, remove in production
	}
	__calc_should_abandon_set_unlock_value(lo, &aban);
}

void dp_md_calc_comp_state(const struct nvmeibc_block_command *cmds, int *rv, bool *retry)
{
	const struct bio_extention *bx = cmds->o->md_op.bx;
	dp_mirror_calc_comp_state(cmds, rv, retry);
	if (bx) {
		if (bx->exec.rider_rv_on_callback != 0) {
			*rv = -EXDEV;											// rider told us to stop
		} else if (*rv != 0) {
			// Severe error without retry
		} else if (*retry == true) {								// Regular IO err, that would retried
			*rv = -EIO;												// but carriers dont retry
		}
		*retry = false;												// Daniel G.L asked not to retry QLC op when Rider (MTV) sent the io. Do retry if backdoor sent the io
	}
}

/******************* dp_wcv_mirror functions. Move to a different file *********/
u64 dp_wcv_mirror_get_wcv2mdv_ptr_from_locks_binfo(const struct operation *o)
{
	(void)o; //return rldr->rld.pre.bits.txid;
	return 0x7ULL;
}

int dp_wcv_mirror_exec_func_on_locks_tkn(struct nvmeibc_block_command *rldr, int err)
{
	const struct operation *o = rldr->o;
	if (nvmeibc_operation_has_bio_extention(o) && (err == 0)) {			// Special IO
		struct bio_extention *bx = o->md_op.bx;
		if (bx->cb.give.after_locks_taken) {					// Special Cold-Recovery Read, with J2D resconstruction
			WARN(!nvmeib_block_io_op_is_write(o->op), "nvmeibc bug, op=%d\n", o->op);
			bx->cb.payload.wcv2mdv_ptr = dp_wcv_mirror_get_wcv2mdv_ptr_from_locks_binfo(o);
			bx_carrier_give_cb_to_rider(bx, 0, CAR_BX_CB_REASON_ON_LOCKS_TAKEN_CB);					// Syncronous branch. Unlike in MDV, which has async callback
		} else if (bx->exec.force_read_b4_write) {				// Caiser special cmpxchng IO
			// Do nothing special,
		} else {
			// Regular MTV IO without any barriers
			//WARN(true, "nvmeibc bug, wrong operation, o=%p, crashing the system to prevent data corruption\n", o);BUG();
		}
	}
	return dp_mirror_exec_func_on_locks_tkn(rldr, err);
}

static void __store_md_ptr_for_rider(struct operation *o, void *md_ptr)
{
	struct bio_extention *bx = o->md_op.bx;
	if ((bx)&&(bx->exec.give_1st_md_blk_on_endio)) {
		WARN(o->op != NVMEIB_BLOCK_IO_OP_READ, "nvmebc incorrect flow, op=%d\n", o->op);
		bx->cb.payload.md_ptr = md_ptr;
		//bx_carrier_give_cb_to_rider(bx, 0, CAR_BX_CB_REASON_MD_ENDIO); // Daniel G.L asked to unify callback with end_rider_to_carrier_bio
	}
}

#if 0
void __operation_extended_rider_io_end_bio(struct operation *o, int rv)
{
	if (nvmeibc_operation_is_qlc(o)) {
		struct qlc_operation *o_qlc = container_of(o, struct qlc_operation, o);
		BUG_ON(o->flags.was_bio_part_split);						// Split will cause memory and data corruption because md/o_qlc gets free
		if (!rv || rv == -ENOENT) __store_md_ptr_for_rider(o, qlc_operation_get_md_ptr(o_qlc));
		__complete_all_bparts(o, rv);
		qlc_operation_destroy(o_qlc);
		return;
	} else if (nvmeibc_operation_has_bio_extention(o) && (rv == 0)) {
		BUG_ON(o->flags.was_bio_part_split);						// Split will cause memory and data corruption because md/cmds gets free
		if (nvmeibc_block_is_wcv(o->nd)) {
			extern u64 dp_wcv_mirror_get_wcv2mdv_ptr_from_locks_binfo(const struct operation *o);	// Ugly, Will be solved when this function becomes real
			__store_md_ptr_for_rider(o, (void*)dp_wcv_mirror_get_wcv2mdv_ptr_from_locks_binfo(o));
		} else if (nvmeibc_block_is_mdv(o->nd)) {
			__store_md_ptr_for_rider(o, o->cmds[0].iocmd->reqs->md);			// First cmd of read
		} else { /* No other volume support bio extention */}
	}
}

void __operation_extended_rider_io_resubmit(struct operation *o) {
	if (nvmeibc_operation_is_qlc(o))
		qlc_operation_destroy(container_of(o, struct qlc_operation, o));
}
#endif