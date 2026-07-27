#pragma once
#include "../bunitest.h"
#include "nvmesh_sim.h"
#include "block/recovery/nvmeibc_raid_recovery.h"

struct sim_recovery_hooks{
	struct nvmeibc_recovery_hooks base;
	useconds_t on_finish_one_sync_sleep_period;
	struct nvmeibc_multi_completion launched;
	struct nvmeibc_multi_completion cleaned;
	struct nvmeibc_multi_completion synced;
};

struct sim_recovery_hooks sim_recovery_hooks_create(useconds_t on_finish_one_sync_sleep_period, u32 follow_x_launches, u32 follow_x_cleanups, u32 on_finish_x_syncs);
void    sim_recovery_setup_hooks(struct sim_recovery_hooks* hooks);
#define sim_recovery_clean_hooks() { sim_recovery_setup_hooks(NULL); }


