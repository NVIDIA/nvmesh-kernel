/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_admin_channel.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_types.h"
#include "nvmeibs_types.h"
#include "nvmeib.h"
#include "nvmeib_utils.h"
#include "module/instance/nvmeibc_cinst_params.h"
#include "nvmeibc_main.h"

#define __FIN FINS(ch->base.name)
#define __FOUT FOUTS(ch->base.name)

#define __NFIN NFINS(ch->base.name)
#define __NFOUT NFOUTS(ch->base.name)

int nvmeibc_admin_channel_init(
	const struct nvmeibc_cinst_params_core *p, struct nvmeibc_admin_channel *ch)
{
	int rv = 0;
	proc_name_t pname;

	__NFIN;
	ch->base.ct = ct_admin;
	if (!(rv = nvmeibc_channel_init(&ch->base, p))) {
		INIT_LIST_HEAD(&ch->rionics);
		INIT_LIST_HEAD(&ch->periodics);
		spin_lock_init(&ch->periodics_guard);
		atomic_set(&ch->pending_start_ioch_wq, 0);
		init_rwsem(&ch->segments_locks_remote.guard);
	}
	else {
		_NE(error_admin_channel_nvmeibc_admin_channel_init, "nvmeibc_channel_init() failed @RV", rv);
		goto out;
	}

	clnt_proc_name_format(pname, 'C', "WQ", "adminCh", nvmeibc_cinst_get_core_inst_num(p));
	if (!(ch->remove_wq = wq_create_verbose(pname))) {
		_NE(error_1_admin_channel_nvmeibc_admin_channel_init, "Fail to allocate admin channel removal queue");
		rv = -ENOMEM;
		goto out;
	}

out:
	__NFOUT;
	return rv;
}

static void free_iornic(struct nvmeibc_io_rnic *rionic)
{
	struct list_head *lionics = &rionic->lionics;
	struct nvmeibc_io_lnic *lionic, *tmp_lionic;
	int i;
	unsigned long flags;

	NFIN;
	if (rionic->disk) {
		spin_lock_irqsave(&rionic->disk->spinlock, flags);
		/* Unlink rionic from the disk */
		if (!list_empty(&rionic->disk_link))
			list_del_init(&rionic->disk_link);
		if (!list_empty(&rionic->disk_nrlink))
			list_del_init(&rionic->disk_nrlink);
		spin_unlock_irqrestore(&rionic->disk->spinlock, flags);
	}

	list_for_each_entry_safe(lionic, tmp_lionic, lionics, rionic_link) {
		list_del(&lionic->rionic_link);
		/*
		 * Free No-RDDA channels
		 */
		for (i = 0; i < lionic->n_nr_qps; ++i)
			nvmeibc_ib_nordda_channel_free(lionic->nr_channels + i);
		kfree(lionic->nr_channels);
		nvmeib_public_free_percpu(lionic->last_io_ka_jif);
		nvmeib_public_free_percpu(lionic->last_send_success_jif);
		nvmeib_public_free_percpu(lionic->last_recv_success_jif);
		/* Unmap dummy-MD from local NIC */
		if (lionic->dummy_md_read_addr)
			ib_dma_unmap_single(P2IB(lionic->port), lionic->dummy_md_read_addr, PAGE_SIZE, DMA_FROM_DEVICE);
		if (lionic->dummy_md_write_addr)
			ib_dma_unmap_single(P2IB(lionic->port), lionic->dummy_md_write_addr, PAGE_SIZE, DMA_TO_DEVICE);
		if (lionic->dummy_md_write_ptr)
			free_page((unsigned long)lionic->dummy_md_write_ptr);
		if (lionic->dummy_md_read_ptr)
			free_page((unsigned long)lionic->dummy_md_read_ptr);
		kfree(lionic);
	}

	kfree(rionic);
	NFOUT;
}

void nvmeibc_admin_channel_free(struct nvmeibc_admin_channel *ch)
{
	struct nvmeibc_io_rnic *rionic;

	__NFIN;
	while ((rionic = list_first_entry_or_null(
		&ch->rionics, struct nvmeibc_io_rnic, admin_link))) {
		list_del(&rionic->admin_link);
		free_iornic(rionic);
	}
	wq_destroy(ch->remove_wq);
	ch->remove_wq = NULL;
	__NFOUT;
}

struct admin_ch_workqe {
	struct workqe_struct work;
	struct nvmeibc_admin_channel *ch;
	struct workqe_struct *wrapped_work;
	bool high_pri;
};

static void admin_ch_work_fn(struct workqe_struct *work)
{
	struct admin_ch_workqe *admin_ch_workqe = container_of(work, struct admin_ch_workqe, work);
	struct workqe_struct *wrapped_work = admin_ch_workqe->wrapped_work;
	struct nvmeibc_admin_channel *ch = admin_ch_workqe->ch;
	bool high_pri = admin_ch_workqe->high_pri;
	u64 start_jif;
	workq_func_t wrapped_work_fn = wrapped_work->f;

	kfree(admin_ch_workqe);

	if (high_pri)
		atomic_dec(&ch->wq_high_pri_cnt);
	_ND(trace_admin_channel_admin_ch_work_fn, "Running fn @WRAPPED_WORK_FN on admin ch @CH_PTR wq @MAIN_WQ_PID",
	   wrapped_work_fn, ch, wq_pid(ch->remove_wq));
	start_jif = jiffies;
	(*wrapped_work_fn)(wrapped_work);
	_ND(trace_1_admin_channel_admin_ch_work_fn, "fn @WRAPPED_WORK_FN on admin ch @CH_PTR wq @MAIN_WQ_PID took @DIFF_JIFFIES ms",
	   wrapped_work_fn, ch, wq_pid(ch->remove_wq), jiffies - start_jif);
}

static int _admin_ch_add_work(struct nvmeibc_admin_channel *ch,
	struct workqe_struct *work, bool high_pri_work)
{
	struct admin_ch_workqe *work_wrap = NULL;
	int rv = 0, high_pri_cnt = atomic_read(&ch->wq_high_pri_cnt);

	if (!high_pri_work && high_pri_cnt > 0) {
		_NT(trace_admin_channel_admin_ch_add_work, "not scheduling work fn @WRAPPED_WORK_FN due to @HIGH_PRI_CNT high priority tasks on wq",
		   work->f, high_pri_cnt);
		rv = -EAGAIN;
		goto out;
	}

	if (!(work_wrap = kzalloc(sizeof(*work_wrap), GFP_ATOMIC))) {
		_NT(error_admin_channel_admin_ch_add_work, "OOM Error");
		rv = -ENOMEM;
		goto out;
	}

	WQ_INIT_WORK(&work_wrap->work, admin_ch_work_fn);
	work_wrap->high_pri = high_pri_work;
	work_wrap->ch = ch;
	work_wrap->wrapped_work = work;

	if (!wq_add_work(ch->remove_wq, &work_wrap->work))
		rv = -1;
	else if (high_pri_work)
		atomic_inc(&ch->wq_high_pri_cnt);
		
out:
	if (rv < 0)
		kfree(work_wrap);
	return rv;
}

int nvmeibc_admin_channel_add_work(struct nvmeibc_admin_channel *ch,
	struct workqe_struct *work)
{
	return _admin_ch_add_work(ch, work, true);
}

int nvmeibc_admin_channel_add_low_pri_work(struct nvmeibc_admin_channel *ch,
	struct workqe_struct *work)
{
	return _admin_ch_add_work(ch, work, false);
}

void nvmeibc_admin_channel_add_periodic(struct nvmeibc_admin_channel *ch,
	struct admin_periodic *p)
{
	unsigned long flags;

	__NFIN;
	spin_lock_irqsave(&ch->periodics_guard, flags);
	list_add_tail(&p->link, &ch->periodics);
	p->remove_periodic = nvmeibc_admin_channel_remove_periodic;
	p->ch = ch;
	spin_unlock_irqrestore(&ch->periodics_guard, flags);
	__NFOUT;
}

void nvmeibc_admin_channel_remove_periodic(struct nvmeibc_admin_channel *ch,
	struct admin_periodic *p)
{
	unsigned long flags;

	__NFIN;
	spin_lock_irqsave(&ch->periodics_guard, flags);
	list_del(&p->link);
	spin_unlock_irqrestore(&ch->periodics_guard, flags);
	__NFOUT;
}

void nvmeibc_admin_channel_exec_periodic(struct nvmeibc_admin_channel *ch,
	unsigned long t)
{
	struct admin_periodic *p, *tmp_p;
	int rv;
	unsigned long flags;

	spin_lock_irqsave(&ch->periodics_guard, flags);
	list_for_each_entry_safe(p, tmp_p, &ch->periodics, link) {
		if ((rv = p->on_periodic(p->on_periodic_arg, t))) {
			list_del(&p->link);
			if (rv < 0) {
				kfree(p);
			}
		}
	}
	spin_unlock_irqrestore(&ch->periodics_guard, flags);
}


