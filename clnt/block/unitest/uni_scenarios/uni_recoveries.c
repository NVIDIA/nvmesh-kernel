/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_recoveries.h"
#include "../nvmeibc_block_common.h"
#include "./uni_framework/bunitest_conf.h"
#include "../uni_framework/unitest_defs.h"
#include "../uni_framework/range_algorithms.h"
#include "block/recovery/nvmeibc_raid_recovery.h"


void sim_recovery_setup_hooks(struct sim_recovery_hooks* hooks){
	extern struct nvmeibc_recovery_hooks* rcvr_hooks;
	rcvr_hooks = hooks? &hooks->base : NULL;
}

static void sim_recovery_on_finish_one_sync_only_sleep(struct nvmeibc_recovery_hooks* self, struct nvmeibc_recovery* recov){
	struct sim_recovery_hooks* derived = container_of(self, struct sim_recovery_hooks, base);
	BUG_ON(!recov);
	if (derived->on_finish_one_sync_sleep_period)
		usleep(derived->on_finish_one_sync_sleep_period);
}

static void sim_recovery_on_finish_sync(struct nvmeibc_recovery_hooks* self, struct nvmeibc_recovery* recov){
	struct sim_recovery_hooks* derived = container_of(self, struct sim_recovery_hooks, base);
	BUG_ON(!recov);
	nvmeibc_multi_completion_done(&derived->synced);
	if (derived->on_finish_one_sync_sleep_period) {
		usleep(derived->on_finish_one_sync_sleep_period);
	}
}

static void sim_recovery_on_launch(struct nvmeibc_recovery_hooks* self, struct nvmeibc_recovery* recov){
	struct sim_recovery_hooks* derived = container_of(self, struct sim_recovery_hooks, base);
	BUG_ON(!recov);
	nvmeibc_multi_completion_done(&derived->launched);
}

static void sim_recovery_on_cleanup_locked(struct nvmeibc_recovery_hooks* self, struct nvmeibc_recovery* recov){
	struct sim_recovery_hooks* derived = container_of(self, struct sim_recovery_hooks, base);
	BUG_ON(!recov);
	nvmeibc_multi_completion_done(&derived->cleaned);
}

static void sim_recovery_on_cleanup_locked_verify_once(struct nvmeibc_recovery_hooks* self, struct nvmeibc_recovery* recov) {
	memset(self, 0xDD, sizeof(*self));					// Now if second completion will arrive it will seg_fault on calling function at address 0xDDDDD so we can easily detect the problem
	sim_recovery_on_cleanup_locked(self, recov);
}

struct sim_recovery_hooks sim_recovery_hooks_create(useconds_t on_finish_one_sync_sleep_period, u32 follow_x_launches, u32 follow_x_cleanups, u32 on_finish_x_syncs){
	struct sim_recovery_hooks hooks;
	memset(&hooks, 0, sizeof(hooks));
	if (on_finish_one_sync_sleep_period){
		hooks.base.on_finish_one_sync = sim_recovery_on_finish_one_sync_only_sleep;
		hooks.on_finish_one_sync_sleep_period = on_finish_one_sync_sleep_period;
	}
	if (follow_x_launches){
		hooks.base.on_launch = sim_recovery_on_launch;
		nvmeibc_multi_completion_init(&hooks.launched);
		nvmeibc_multi_completion_add_aux_jobs(&hooks.launched, follow_x_launches-1);
	}
	if (follow_x_cleanups){
		hooks.base.on_finish = (follow_x_cleanups > 1) ? sim_recovery_on_cleanup_locked : sim_recovery_on_cleanup_locked_verify_once;
		nvmeibc_multi_completion_init(&hooks.cleaned);
		nvmeibc_multi_completion_add_aux_jobs(&hooks.cleaned, follow_x_cleanups-1);
	}
	if (on_finish_x_syncs) { // Can be done with or without sleep - will overide the cb, but will check the sleep period value
		hooks.base.on_finish_one_sync = sim_recovery_on_finish_sync;
		nvmeibc_multi_completion_init(&hooks.synced);
		nvmeibc_multi_completion_add_aux_jobs(&hooks.synced, on_finish_x_syncs-1);
	}
	return hooks;
}
