/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "main/utils/nvmeibc_main_block_gen_work_sched.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_block_gen_work_sched_inc_c

struct generic_task_workq {					// Task of volume/block device
	struct workqe_struct work;				// Conenct to wq
	char owner_uuid[NVMEIBC_BD_UUID_LEN];	// Owner volume found, verify uuid
	enum nvmeibc_config_volume_type type;	// For faster search of volumes
	const struct nvmeibc_cinst_params *p;	// Which client instance performs this work
	union {
		blk2main_gen_work_t fnm;			// Execute the job on owner
		blk2blk_gen_work_t  fnb;
	};
	void *context;							// Param to the job (may include pointer to client instance as well).
	bool is_blk2blk_task;
	bool should_free_context;				// Should kfree() the params?
};

static void __do_generic_work_of_volume(struct workqe_struct *work)
{
	struct generic_task_workq *g = container_of(work, struct generic_task_workq, work);
	const struct nvmeibc_cinst_params_main *mp = &g->p->main;
	if (nvmeibc_volume_get_by_uuid(mp, g->owner_uuid, g->type)) {
		if (g->is_blk2blk_task)
			g->fnb(g->context);
		else
			g->fnm(g->context, mp);
	} else {
		_NT(trace_main_do_generic_work_of_volume, "Skipping work of detached @HDR_UUID", g->owner_uuid);
	}
	if (g->should_free_context)
		kfree(g->context);
	kfree(work);
}

static void __schedule_gen_blok_work(const struct nvmeibc_cinst_params_blk *p, const char *owner_uuid,
		void *handler, void *context, bool should_free_context, bool is_blk2blk)
{
	struct generic_task_workq *g;
	int rv;

	BUG_ON(handler == NULL);
	g = kzalloc(sizeof(*g), GFP_ATOMIC);
	if (!g) {
		_NE(error_main_nvmeibc_schedule_generic_work, DMESG_PREFIX() ": out of memory");
		rv = -ENOMEM;
		goto out;
	}
	BUG_ON(p == NULL);
	WQ_INIT_WORK(&g->work, __do_generic_work_of_volume);
	strlcpy(g->owner_uuid, owner_uuid, sizeof(g->owner_uuid));
	g->is_blk2blk_task = is_blk2blk;
	if (is_blk2blk) {
		g->fnb = handler;
	} else {
		g->fnm = handler;
	}
	g->type    = UNKNOWN_ILLEGAL; /* Todo: Add this as parameter */
	g->context = context;
	g->should_free_context = should_free_context;
	g->p = container_of(p, struct nvmeibc_cinst_params, blok);
	rv = nvmeibc_add_work(&g->p->main, &g->work);
out:
	if (rv<0){
		if (should_free_context)
			kfree(context);
		kfree(g);
	}
}

void nvmeibc_block_set_generic_work_to_main(const struct nvmeibc_cinst_params_blk *p,
		const char *block_uuid, /* Unique identifier of the block device for a given instance */
		blk2main_gen_work_t fn, void *context, bool should_free_context)
{
	__schedule_gen_blok_work(p, block_uuid, fn, context, should_free_context, false);
}

void nvmeibc_block_set_generic_work_to_self(const struct nvmeibc_cinst_params_blk *p,
		const char *block_uuid, /* Unique identifier of the block device for a given instance */
		blk2blk_gen_work_t fn,  void *context, bool should_free_context)
{
	__schedule_gen_blok_work(p, block_uuid, fn, context, should_free_context, true);
}

const struct nvmeibc_cinst_params_blk *nvmeibc_isnt_params_main2blk(const struct nvmeibc_cinst_params_main* p_main)
{
	struct nvmeibc_cinst_params *p = container_of(p_main, struct nvmeibc_cinst_params, main);
	return &p->blok;
}

const struct nvmeibc_cinst_params_main *nvmeibc_isnt_params_blk2main(const struct nvmeibc_cinst_params_blk* p_blk)
{
	struct nvmeibc_cinst_params *p = container_of(p_blk, struct nvmeibc_cinst_params, blok);
	return &p->main;
}

const struct nvmeibc_cinst_params_core *nvmeibc_isnt_params_main2core(const struct nvmeibc_cinst_params_main* p_main)
{
	struct nvmeibc_cinst_params *p = container_of(p_main, struct nvmeibc_cinst_params, main);
	return &p->core;
}

const struct nvmeibc_cinst_params_main *nvmeibc_isnt_params_core2main(const struct nvmeibc_cinst_params_core* p_core)
{
	struct nvmeibc_cinst_params *p = container_of(p_core, struct nvmeibc_cinst_params, core);
	return &p->main;
}

const struct nvmeibc_cinst_params_core *nvmeibc_isnt_params_blk2core(const struct nvmeibc_cinst_params_blk* p_blk)
{
	struct nvmeibc_cinst_params *p = container_of(p_blk, struct nvmeibc_cinst_params, blok);
	return &p->core;
}

const struct nvmeibc_cinst_params_blk* nvmeibc_isnt_params_core2blk(const struct nvmeibc_cinst_params_core* p_core)
{
	struct nvmeibc_cinst_params *p = container_of(p_core, struct nvmeibc_cinst_params, core);
	return &p->blok;
}
/******************************************************************************/

int nvmeibc_cinst_params_blk_get_cinst_params_core_tcp_mode(const struct nvmeibc_cinst_params_blk *p) {
	return nvmeibc_isnt_params_blk2core(p)->tcp_mode;
}

/******************************************************************************/
bool nvmeibc_is_on_main_wq(const struct nvmeibc_cinst_params_main *p, bool do_assert)
{
	struct t_main_clnt_globals *mg = __get_from_params_main_globals_container(p);
	return t_main_clnt_sched_on_main_wq(&mg->sched, do_assert);
}

int nvmeibc_add_work(const struct nvmeibc_cinst_params_main *p, struct workqe_struct *work)
{
	struct t_main_clnt_globals *mg = __get_from_params_main_globals_container(p);
	return t_main_clnt_sched_add_work(&mg->sched, work);
}

bool nvmeibc_cancel_work(const struct nvmeibc_cinst_params_main *p, struct workqe_struct *work)
{
	struct t_main_clnt_globals *mg = __get_from_params_main_globals_container(p);
	return t_main_clnt_sched_cancel_work(&mg->sched, work);
}

struct run_mainwq_workqe {
	struct workqe_struct work;
	nvmeibc_main_wq_fn_type fn;
	void *param;
	int rv;
	bool free_work;
};

static void nvmeibc_run_on_main_wq_workfn(struct workqe_struct *work_qe)
{
	struct run_mainwq_workqe *r = container_of(work_qe, struct run_mainwq_workqe, work);
	r->rv = (*r->fn)(r->param);
	if (r->free_work)
		kfree(r);
}

int nvmeibc_run_on_main_wq(const struct nvmeibc_cinst_params_main *p, nvmeibc_main_wq_fn_type fn, void *param, bool drain_wq, bool can_sleep, int *p_sts)
{
	struct t_main_clnt_globals *mg = __get_from_params_main_globals_container(p);
	struct t_main_clnt_sched *s = &mg->sched;
	struct run_mainwq_workqe stack_work = {
		.fn = fn,
		.param = param,
		.rv = 0,
		.free_work = false
	}, *work = NULL;
	int sts = 0;
	int rv = 0;

	NFIN;
	if (drain_wq && t_main_clnt_sched_on_main_wq(s, false)) {
		/* Already on Main WQ and caller is waiting => run the fn directly */
		rv = (*fn)(param);
		goto out;
	}

	if (drain_wq) {
		/* Waiting for it to finish so we can use the stack work and comp */
		work = &stack_work;
	} else {
		if (!(work = kzalloc(sizeof(*work), (!can_sleep ? GFP_ATOMIC : GFP_KERNEL)))) {
			rv = -ENOMEM;
			sts = -1;
			goto out;
		}
		work->fn = fn;
		work->param = param;
		work->free_work = true;
	}
	WQ_INIT_WORK(&work->work, nvmeibc_run_on_main_wq_workfn);

	if ((rv = t_main_clnt_sched_add_work(s, &work->work))) {
		if (work->free_work) {
			kfree(work);
		}
		sts = -1;
		goto out;
	}

	if (drain_wq) {
		wq_drain(s->main_wq);
		rv = stack_work.rv;
	}
out:
	if (p_sts)
		*p_sts = sts;

	NFOUT;
	return rv;
}

#pragma pop_macro("__FILE_LITERAL__")
