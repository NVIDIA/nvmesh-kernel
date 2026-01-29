/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "main/nvmeibc_main_ioctls.h"
#include "module/instance/nvmeibc_cinst_params.h"
//#include "main/nvmeibc_main_common.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_module_ioctls_inc_c

struct module_work_param {	// Struct for scheduling module cli msg handling on mainwq
	struct workqe_struct work;
	struct nvmeibc_multi_completion on_finish;
	enum module_work_types type;
	union {
		struct {
			char *buf;						// string of the command (null terminated)
			size_t len;						// == strlen(buf)
		} ioctl;
		struct {
			const struct nvmeibc_cinst_params *p;
			bool is_upgrade;
		} direct;
		u64 payload;
	};
	int rv;
};
#define module_work_is_blocking(mw) (((mw)->type & mw_is_blocking_flag) != 0)

#define module_work_param_init_empty \
	{workqe_struct_init, nvmeibc_multi_completion_init_empty(), mw_illegal, {.payload = 0ULL}, 0}

static void module_work_param_to_string(const struct module_work_param *mw)
{
	if ((mw->type == mw_inst_ioctl)||(mw->type == mw_inst_ioctl_blocking)) {
		_NT(handle_proc_io_e4, "nvmeibc work: @NVMEIBC_WORK_TYPE, ioctl=|@STR|", mw->type, mw->ioctl.buf);
	} else {
		const struct nvmeibc_cinst_params *p = mw->direct.p;
		_NT(t_03_main_davw, "nvmeibc work: @NVMEIBC_WORK_TYPE, inst=@INT", mw->type, (p ? p->index : -1));
	}
}

static int __send_task_wait_for_completion(struct module_work_param *mw)
{
	struct t_main_clnt_sched *s = nvmeibc_get_module_sched(); // Todo: change to &((struct t_main_module_single_instance_globals *)args)->sched;
	const bool should_wait = module_work_is_blocking(mw);
	int rv = 0;
	if (t_main_clnt_sched_add_work(s, &mw->work) < 0) {
		_NE(handle_proc_io_e7, MAIN_IOCTL_PREFIX ": Fail to add @NVMEIBC_WORK_TYPE to module work_queue", mw->type);
		rv = -1;
	} else if (should_wait) {
		nvmeibc_multi_completion_wait_for(&mw->on_finish);
		rv = mw->rv;
	} else { /* Launched successfully */ }
	return rv;
}

static void handle_module_generic_work(struct workqe_struct *_w)
{
	struct module_work_param *mw = container_of(_w, struct module_work_param, work);
	const bool should_wait = module_work_is_blocking(mw);

	nvmeibc_assert_on_module_wq();
	module_work_param_to_string(mw);
	switch (mw->type) {
	case mw_inst_ioctl:
	case mw_inst_ioctl_blocking: {
		if (mw->ioctl.buf[0] == MAIN_IOCTL_MARKER) {
			nvmeibc_module_ioctl(mw->ioctl.buf + 1);
		}
		kfree(mw->ioctl.buf);
		mw->ioctl.buf = NULL;
		break;
	}
	case mw_inst_add_blocking: {
		if (nvmeibc_get_state() <= NVMEIBC_MOD_STATE_READY)
			mw->rv = nvmeibc_instance_create_on_modwq(mw->direct.p);
		else
			mw->rv = -ENODEV;
		break;
	}
	case mw_inst_del_blocking: {
		mw->rv = nvmeibc_instance_destroy_on_modwq(mw->direct.p);
		break;
	}

	case mw_clean_all_vols_of_all_inst: {
		const enum nvmeibc_mod_state state = nvmeibc_get_state();
		//int n_instance = nvmeibc_cinst_array_get_num_instances();
		if (state != NVMEIBC_MOD_STATE_RM_RDY) {
			const struct nvmeibc_cinst_params *p = NULL;
			nvmeibc_state_promote(NVMEIBC_MOD_STATE_PREP_RM);
			//nvmeibc_multi_completion_add_aux_jobs(&mw->on_finish, n_instance);
			for_each_cinst(p) {
				shut_down_detach_all_remainig_volumes_of_inst(&p->main, mw->direct.is_upgrade);
				// Todo: Use nvmeibc_add_work(&p->main, NULL); To make all detaches parallel
			}
			nvmeibc_state_promote(NVMEIBC_MOD_STATE_RM_RDY);
		} else {
			_NT(handle_module_work_w2, "Skipping @NVMEIBC_WORK_TYPE. It was already done", mw->type);
		}
		break;
	}

	case mw_inst_del_all_blocking: {
		const struct nvmeibc_cinst_params *p = NULL;
		nvmeibc_state_promote(NVMEIBC_MOD_STATE_EXITING);
		for_each_cinst(p) {
			nvmeibc_instance_destroy_on_modwq(p);
		}
		break;
	}
	default:
		_NT(handle_module_work_w1, "Unknown module @NVMEIBC_WORK_TYPE", mw->type);
		break;
	}
	if (should_wait)
		nvmeibc_multi_completion_done(&mw->on_finish);
	else
		kfree(mw);	// No one is waiting for this struct so free it
}

int nvmeibc_instance_do_blocking(const struct nvmeibc_cinst_params *p, enum module_work_types type, bool is_upgrade)
{
	struct module_work_param mw = module_work_param_init_empty;
	int rv = 0;
	mw.direct.p = p;
	mw.direct.is_upgrade = is_upgrade;
	mw.type = type;
	WQ_INIT_WORK(&mw.work, handle_module_generic_work);
	nvmeibc_multi_completion_init(&mw.on_finish);
	if (type == mw_inst_del_all_blocking) {
		const enum nvmeibc_mod_state state = nvmeibc_get_state();
		if (state != NVMEIBC_MOD_STATE_RM_RDY) {			// All instances must clean from volumes, either init fail or cli command to shutdown already cleaned the volumes
			if (state != NVMEIBC_MOD_STATE_INITIALIZING)
				_NT(warn_main_nvmeibc_exit, "nvmeibc unloading in an un-ordered fashion. Recovering @STATE", state);
			mw.type = mw_clean_all_vols_of_all_inst;        // Try recoveing the situation: detach all volumes on all instances
			rv =  __send_task_wait_for_completion(&mw);
			mw.type = type;									// Now remove all instances
			nvmeibc_multi_completion_init(&mw.on_finish);
		}
	}
	rv |=  __send_task_wait_for_completion(&mw);
	return rv;
}

static int handle_module_cli_input(void *args, char *buf, size_t len, bool *posted)
{
	int rv = -EINVAL;
	int n_lines = 0;
	size_t cur_len, orig_len = len;
	char *next_buf, *next_line;
	struct module_work_param *mw;

	NFIN;
	(void)args; (void)posted;
	if (len < 3) {
		rv = -EINVAL;
		goto out;
	}
	// Todo: Unite below code with function __handle_multiple_cli_lines()
	if (buf[len] != 0) { // Daniel: Todo, remove in the final product
		char *l = &buf[len - 1];
		_NE(handle_proc_io_w1, DMESG_MOD_PREFIX ": corrupted string {@CHAR,@CHAR}",l[0], l[1]);
		WARN_ON(true);
		goto out;
	}
	do { // Single line CLI handling
		next_line = strchr(buf, '\n');
		cur_len = next_line ? (size_t)(next_line - buf) : len;
		mw = NULL;
		next_buf = NULL;
		if (!((mw = kzalloc(sizeof(*mw), GFP_KERNEL)) &&
			  (next_buf = kzalloc(cur_len + 1, GFP_KERNEL)))) {
			_NE(handle_proc_io_e1, DMESG_MOD_PREFIX ": Fail to allocate for inst_ctl work");
			goto err;
		} else {
			const bool default_blocking_ioctl = true;
			mw->type = (default_blocking_ioctl ? mw_inst_ioctl_blocking : mw_inst_ioctl);
			mw->ioctl.buf = next_buf;
			mw->ioctl.len = cur_len + 1;
			memcpy(mw->ioctl.buf, buf, cur_len);
			WQ_INIT_WORK(&mw->work, handle_module_generic_work);
			// skip cur_line and ('\n' or '\0')
			buf += mw->ioctl.len;
			len -= mw->ioctl.len;
		}

		mw->ioctl.len = nvmeib_remove_unsafe_symbols(mw->ioctl.buf, mw->ioctl.buf);
		if (module_work_is_blocking(mw))
			nvmeibc_multi_completion_init(&mw->on_finish);
		if (__send_task_wait_for_completion(mw) < 0)
			goto err;
		if (module_work_is_blocking(mw))
			kfree(mw);
		n_lines++;
	} while (next_line);
	rv = orig_len;
	goto out;

err:
	kfree(mw);
	kfree(next_buf);

out:
	_NT(handle_proc_io_e3, "Processed: n_lines=@INT, rv=@RV, buf_len=@INT", n_lines, rv, (int)orig_len);
	NFOUT;
	return rv;
}

#pragma pop_macro("__FILE_LITERAL__")
