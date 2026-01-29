/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include <linux/kmod.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/atomic.h>

#include "nvmeibs_trace.h"
#include "nvmeib_utils.h"
#include "nvmeibs_distribute_intrs.h"


/* This module implements an API running a userspace program that
 * evenly distributes interrupts among NVMe disks. */


/* path for affinity script and it dependencies e.g. nvmesh_set_irq_affinity_common */
#define DIST_INTRS_UPCALL_PATH "/opt/nvmesh/common-repo/scripts/"

#define DIST_INTRS_UPCALL_PATHLEN 256


static char distribute_interrupts_prog[DIST_INTRS_UPCALL_PATHLEN] =
				DIST_INTRS_UPCALL_PATH "/nvmesh_set_irq_affinity";

module_param_string(distr_intr_program, distribute_interrupts_prog,
                sizeof(distribute_interrupts_prog), 0600);
MODULE_PARM_DESC(distr_intr_program, "Path to the interrupt distribution program");


static int do_userspace_upcall(unsigned long handled_disk_events);
static int distribute_interrupts_upcall(unsigned long handled_disk_events);


static unsigned long disk_events;
static int currently_running;
static int shutting_down;
static DEFINE_SPINLOCK(disk_events_spinlock);
static DECLARE_COMPLETION(interrupt_distribution_done);



static void cleanup_subprcs_info(struct subprocess_info *info)
{
	unsigned long handled_disk_events = (unsigned long)info->data;
	unsigned long flags = 0;
	unsigned long current_disk_events;
	int run_distribute_prog = 0;


	spin_lock_irqsave(&disk_events_spinlock, flags);
	if (!shutting_down) {
		current_disk_events = disk_events;
		if (current_disk_events != handled_disk_events)
			run_distribute_prog = 1;
		else
			currently_running--;
	} else {
		complete(&interrupt_distribution_done);
	}
	spin_unlock_irqrestore(&disk_events_spinlock, flags);

	if (run_distribute_prog) {
		distribute_interrupts_upcall(current_disk_events);
	}
}



int trigger_distribute_nvme_interrupts(void)
{
	int rv = -1;
	unsigned long current_disk_events;
	unsigned long flags = 0;
	int run_distribute_prog = 0;
	int is_shutting_down;

	_NW(warn_distribute_intrs_trigger_distribute_nvme_interrupts, "shutting_down=@SHUTTING_DOWN   disk_events=@DISK_EVENTS", shutting_down, disk_events);
	spin_lock_irqsave(&disk_events_spinlock, flags);
	is_shutting_down = shutting_down;
	if (!is_shutting_down) {
		disk_events++;
		current_disk_events = disk_events;
		run_distribute_prog = (currently_running == 0);
		if (run_distribute_prog)
			currently_running++;
	}
	spin_unlock_irqrestore(&disk_events_spinlock, flags);

	if (run_distribute_prog) {
		rv = distribute_interrupts_upcall(current_disk_events);
	} else {
		rv = 0;
		if (is_shutting_down)
			_NT(trace_1_distribute_intrs_trigger_distribute_nvme_interrupts, "shutting down the module so not performing interrupt distribution");
		else
			_NT(trace_distribute_intrs_trigger_distribute_nvme_interrupts, "Already running");
	}

	return rv;
}


void wait_until_distribute_interrupts_done(void)
{
	unsigned long flags = 0;
	int need_to_wait = 0;

	NFIN;

	spin_lock_irqsave(&disk_events_spinlock, flags);
	shutting_down = 1;
	need_to_wait = (currently_running != 0);
	spin_unlock_irqrestore(&disk_events_spinlock, flags);

	if (need_to_wait) {
		_NT(trace_distribute_intrs_wait_until_distribute_interrupts_done, "Waiting for interrupt distribution process to exit");
		wait_for_completion(&interrupt_distribution_done);
	}

	NFOUT;
}



static int distribute_interrupts_upcall(unsigned long handled_disk_events)
{
	int rv;
	unsigned long flags = 0;

	rv = do_userspace_upcall(handled_disk_events);
	if (rv) {
		_NE(error_distribute_intrs_distribute_interrupts_upcall, "Failed running NVMe interrupt distribution process @RV",
			rv);
		spin_lock_irqsave(&disk_events_spinlock, flags);
		currently_running--;
		if ((currently_running == 0) && shutting_down) {
			complete(&interrupt_distribution_done);
		}
		spin_unlock_irqrestore(&disk_events_spinlock, flags);
	}
	return rv;
}


#if KS_HAS_CALL_USERMODEHELPER_SETFNS
#define CALL_USERMODEHELPER_SETUP(path, argv, envp, gfp_mask, init, cleanup, data) ({	\
	void* info = call_usermodehelper_setup(path, argv, envp, gfp_mask);		\
	call_usermodehelper_setfns(info, init, cleanup, data);				\
	info;	})
#else
#define CALL_USERMODEHELPER_SETUP(path, argv, envp, gfp_mask, init, cleanup, data) call_usermodehelper_setup(path, argv, envp, gfp_mask, init, cleanup, data)
#endif

static int do_userspace_upcall(unsigned long handled_disk_events)
{
	static char *envp[] = { "HOME=/",
				"TERM=linux",
				"PATH=/sbin:/usr/sbin:/bin:/usr/bin" ":" DIST_INTRS_UPCALL_PATH,
				NULL
				};

	static char *argv[] = { "/bin/sh", "-c", (char *)distribute_interrupts_prog, NULL };

	int ret = -EACCES;
	void* data;
	struct subprocess_info *subpr_info;

	if (distribute_interrupts_prog[0] == '\0')
		goto out;
	_NI(trace_distribute_intrs_do_userspace_upcall, "calling user mode script: @DISTRIBUTE_INTERRUPTS_PROG   handled_disk_events=@HANDLED_DISK_EVENTS",
		distribute_interrupts_prog, handled_disk_events);

	data = (void*)handled_disk_events;
	subpr_info = CALL_USERMODEHELPER_SETUP(argv[0],
		argv, envp, GFP_ATOMIC, NULL, cleanup_subprcs_info, data);
	if (!subpr_info) {
		_NE(error_distribute_intrs_do_userspace_upcall, "failed forking process");
		goto out;
	}
	ret = call_usermodehelper_exec(subpr_info, UMH_NO_WAIT);

	_NI(trace_3_distribute_intrs_do_userspace_upcall, "user script ended with code: @RV", ret);

out:
	/* block further calls because script execution fails */
	if (ret == -ENOENT || ret == -EACCES)
		distribute_interrupts_prog[0] = '\0';

	return ret > 0 ? 0 : ret;
}
