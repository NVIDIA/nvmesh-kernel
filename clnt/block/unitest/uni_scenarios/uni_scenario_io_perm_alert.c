/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "uni_scenario_io_perm_alert.h"
#include "../nvmeibc_block_common.h"
#include "./uni_framework/bunitest_conf.h"
#include "../uni_framework/unitest_defs.h"
#include "../uni_framework/range_algorithms.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_perm_alert.h"
#include "nvmeibc_simu_disk.h"

//PAY ATTENTION - the test here are timing dependent; if you debug with GDB they will fail

struct mgmt_message_t{
	char msg[4096];
	unsigned long id;
	bool is_update;
	u32 received;
	const void *dev;
};

static struct mgmt_message_t mgmt_message = {{0},0,0,0,0};

static int __store_mgmt_message(const struct nvmeibc_block_device* dev, char *msg) //, unsigned long msg_id, bool is_update)
{
	BUG_ON(!msg);
	BUG_ON(sizeof(mgmt_message.msg) <= strlen(msg));
	strlcpy(mgmt_message.msg, msg, sizeof(mgmt_message.msg));
	mgmt_message.dev = dev;
	//mgmt_message.id = msg_id;
	//mgmt_message.is_update = is_update;
	mgmt_message.received += 1;
	kfree(msg); /* msg was allocated by nvmesh, not simulator*/
	return 0;
}

struct mgmt_io_changed_notification_t{
	enum nvmeibc_io_perm_arm prev;
	enum nvmeibc_io_perm_arm curr;
};

static struct mgmt_io_changed_notification_t mgmt_io_changed_notice = { NVMEIBC_IO_PERM_ARM_DEV_INIT, NVMEIBC_IO_PERM_ARM_DEV_INIT};

static void __notify_io_changed(struct nvmeibc_block_device *dev, enum nvmeibc_io_perm_arm curr){
	(void)dev;
	mgmt_io_changed_notice.prev = mgmt_io_changed_notice.curr;
	mgmt_io_changed_notice.curr = curr;
}

static inline bool __io_perm_alert_is_armed(struct nvmeibc_io_perm_alert *iod){
	return iod->arm != iod->arm_stable || iod->arm == NVMEIBC_IO_PERM_ARM_DETACHING;
}


enum expected_no_protection_mode{NO_PROTECTION_MODE_IRRELEVANT, NO_PROTECTION_MODE_READ_ONLY, NO_PROTECTION_MODE_WRITABLE};

static void __verify_armed(struct nvmeibc_io_perm_alert *iod, nvmeibc_jiffies_t now, enum nvmeibc_io_perm_arm expected, enum expected_no_protection_mode mode){
	BUG_ON(iod->arm != expected);

	if (!__io_perm_alert_is_armed(iod))
		return;

	BUG_ON(iod->last_armed_at == 0); //we are armed
	BUG_ON(now < iod->last_armed_at); //last arm in the past
	if (iod->arm == iod->arm_stable){
		//arm & arm_stable should be different; only in case of detaching they may be the same
		BUG_ON(iod->arm != NVMEIBC_IO_PERM_ARM_DETACHING);
	}

	if (nvmeibc_io_perm_arm_has_bio(iod->arm)){
		if ( nvmeibc_io_perm_arm_has_bio_with_protection(iod->arm) == false){
			if (mode == NO_PROTECTION_MODE_READ_ONLY){
				BUG_ON(!nvmeibc_io_perm_alert_should_create_readonly_topo(iod, now));
			} else {
				BUG_ON(nvmeibc_io_perm_alert_should_create_readonly_topo(iod, now));
			}
		}
	}
}

static void __test_create_destroy(struct nvmeibc_block_device *dev){
	nvmeibc_jiffies_t now = jiffies;
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;
	bzero(iod, sizeof(*iod));
	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	__verify_armed(iod, now+1, NVMEIBC_IO_PERM_ARM_DEV_INIT, NO_PROTECTION_MODE_IRRELEVANT);
	nvmeibc_io_perm_alert_destroy(iod);
	BUG_ON(iod->arm != NVMEIBC_IO_PERM_ARM_DESTROYED);
}

static void __test_switch_none(struct nvmeibc_block_device *dev){
	nvmeibc_jiffies_t now = jiffies;
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;

	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	BUG_ON(iod->arm != NVMEIBC_IO_PERM_ARM_DEV_INIT);

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_FIRST);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_FIRST, NO_PROTECTION_MODE_WRITABLE);

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_OK);
	BUG_ON(iod->arm != NVMEIBC_IO_PERM_ARM_BIO_OK);

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC, NO_PROTECTION_MODE_IRRELEVANT);

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_OK);
	BUG_ON(iod->arm != NVMEIBC_IO_PERM_ARM_BIO_OK);

	nvmeibc_io_perm_alert_destroy(iod);
}


static void __test_switch_io_disabled(struct nvmeibc_block_device *dev){
	nvmeibc_jiffies_t now = jiffies;
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;
	enum expected_no_protection_mode mode;
	//in this scenario we don't pass through "protected" mode, so the volume will stay read only.

	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC);
	mode = iod->config.unprotected_write_period ? NO_PROTECTION_MODE_WRITABLE : NO_PROTECTION_MODE_READ_ONLY;

	for (u8 i = 0; i < 3; ++i){
		now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
		__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION, mode);

		now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC);
		__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC, NO_PROTECTION_MODE_IRRELEVANT);
	}

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_DETACHING);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_DETACHING, NO_PROTECTION_MODE_IRRELEVANT);

	nvmeibc_io_perm_alert_destroy(iod);
}


static void __test_switch_no_protection(struct nvmeibc_block_device *dev){
	nvmeibc_jiffies_t now = jiffies;
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;

	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	iod->config.unprotected_write_period = 10;
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST);

	for (u8 i = 0; i < 3; ++i){
		now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
		__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION, NO_PROTECTION_MODE_WRITABLE);
		now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_OK);
	}

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION, NO_PROTECTION_MODE_WRITABLE);

	iod->last_armed_at = 1;
	iod->unprotected_write_to_read_only_at = iod->last_armed_at + iod->config.unprotected_write_period;
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION, NO_PROTECTION_MODE_READ_ONLY);

	for (u8 j = 0; j < 3; ++j){
		now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC);
		__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC, NO_PROTECTION_MODE_IRRELEVANT);

		now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
		__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION, NO_PROTECTION_MODE_READ_ONLY);
	}

	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_DETACHING);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_DETACHING, NO_PROTECTION_MODE_IRRELEVANT);

	nvmeibc_io_perm_alert_destroy(iod);
}

static void __test_switch_detaching(struct nvmeibc_block_device *dev){
	nvmeibc_jiffies_t now = jiffies;
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;

	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_DETACHING);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_DETACHING, NO_PROTECTION_MODE_IRRELEVANT);

	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_OK);
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_DETACHING);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_DETACHING, NO_PROTECTION_MODE_IRRELEVANT);

	now = nvmeibc_io_perm_alert_create(iod, (u64)dev);
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
	now = nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_DETACHING);
	__verify_armed(iod, now, NVMEIBC_IO_PERM_ARM_DETACHING, NO_PROTECTION_MODE_IRRELEVANT);
}


static void __test_to_string(struct nvmeibc_block_device *dev){
#if 0
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;
	int written = 0;
	char buf[4096] = { [0 ... 4095] = 0};

	nvmeibc_io_perm_alert_create(iod, (u64)dev);
	nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST);
	nvmeibc_io_perm_alert_switch(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);

	written = nvmeibc_io_perm_alert_tostring(iod, buf, sizeof(buf));
	BUG_ON(written < 180 /* Daniel: What is this constant? */);
#else
	(void)dev; // Daniel: Disabled the test, dont understant what it tests. 2string method is already tested by clientSimulator_print_proc_file_by_path() clientSimulator_print_proc_dir() etc.
#endif
}


nvmeibc_jiffies_t __switch_and_wait(struct nvmeibc_io_perm_alert *iod, enum nvmeibc_io_perm_arm arm){
	nvmeibc_jiffies_t sleep_period = max(iod->config.stabilization_period, iod->config.unprotected_write_period);
	sleep_period = max(sleep_period, iod->config.stucked_detach_alert_freq);
	nvmeibc_io_perm_alert_switch(iod, arm);
	usleep(sleep_period);
	return jiffies;
}

static void __test_send_and_detaching(struct nvmeibc_block_device *dev){
	nvmeibc_jiffies_t now = jiffies;
	struct nvmeibc_io_perm_alert *iod = &dev->dp.io_perm_alert;
	const u32 *n_alerts = &iod->stats.n_stucked_detach_alert_sent;

	nvmeibc_io_perm_alert_create(iod, (u64)dev);
	iod->config.alert_sender = __store_mgmt_message;
	iod->config.notify_io_changed = __notify_io_changed;
	iod->config.stabilization_period = 1;
	iod->config.unprotected_write_period = 0; //turn on read only immediately
	iod->config.stucked_detach_alert_freq = 1;

	bzero(&mgmt_message, sizeof(mgmt_message));
	bzero(&mgmt_io_changed_notice, sizeof(mgmt_io_changed_notice));

	now = __switch_and_wait(iod, NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST);
	nvmeibc_io_perm_alert_periodic_wakeup(dev, now);
	BUG_ON(mgmt_message.received || *n_alerts);
	BUG_ON(mgmt_io_changed_notice.prev != NVMEIBC_IO_PERM_ARM_DEV_INIT && mgmt_io_changed_notice.curr != NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST);

	//switch to read only
	now = __switch_and_wait(iod, NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
	nvmeibc_io_perm_alert_periodic_wakeup(dev, now);
	BUG_ON(mgmt_message.received || *n_alerts);
	BUG_ON(mgmt_io_changed_notice.prev != NVMEIBC_IO_PERM_ARM_BIO_OK_FIRST && mgmt_io_changed_notice.curr != NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_FIRST);

	now = __switch_and_wait(iod, NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC);
	nvmeibc_io_perm_alert_periodic_wakeup(dev, now);
	BUG_ON(mgmt_message.received || *n_alerts);
	BUG_ON(mgmt_io_changed_notice.prev != NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION && mgmt_io_changed_notice.curr != NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC);

	now = __switch_and_wait(iod, NVMEIBC_IO_PERM_ARM_DETACHING);
	nvmeibc_io_perm_alert_periodic_wakeup(dev, now);
	BUG_ON(mgmt_message.received != 1 || *n_alerts != 1);
	BUG_ON(mgmt_io_changed_notice.prev != NVMEIBC_IO_PERM_ARM_NO_BIO_NO_SYNC && mgmt_io_changed_notice.curr != NVMEIBC_IO_PERM_ARM_DETACHING);

	usleep(2);
	nvmeibc_io_perm_alert_periodic_wakeup(dev, jiffies);
	BUG_ON(mgmt_message.received != 2 || *n_alerts != 2);
	BUG_ON(mgmt_io_changed_notice.prev != NVMEIBC_IO_PERM_ARM_DETACHING && mgmt_io_changed_notice.curr != NVMEIBC_IO_PERM_ARM_DETACHING);

	usleep(2);
	nvmeibc_io_perm_alert_periodic_wakeup(dev, jiffies);
	BUG_ON(mgmt_message.received != 3 || *n_alerts != 3);
	BUG_ON(mgmt_io_changed_notice.prev != NVMEIBC_IO_PERM_ARM_DETACHING && mgmt_io_changed_notice.curr != NVMEIBC_IO_PERM_ARM_DETACHING);

	iod->config.alert_sender = NULL;
	iod->config.notify_io_changed = NULL;
}


static void __verify_topo_io_perm(struct test_context env, enum nvmeib_io_type_permission expected){
	struct nvmeibc_topology *topo = nvmeibc_topology_get(&env.dev->topologies);
	BUG_ON(topo->io_perm != expected);
	nvmeibc_topology_put(topo);
}

static void __test_watchdog(struct NVMeshSystem *sys){
	const struct volume_segment_index vsi = {0,0,0,0};

	struct test_context env = {
		.sys = sys,
		.client = sys->clients,
		.dev = sys->clients->devs[vsi.volume],
		.sraid = NVMeshSystem_TstPRaid_init_rel(sys, vsi)
	};

	const u16 protection = env.sraid.cpr->replicas - env.sraid.cpr->slice_size;

	const enum NVMEIBTC_DS_MODE raid_is_perfect[N_MAX_RAID_SLICE_LEN] = {
		[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};

	enum NVMEIBTC_DS_MODE raid_is_no_protection[N_MAX_RAID_SLICE_LEN];
	for (u16 si = 0; si < ARRAY_SIZE(raid_is_no_protection); ++si){
		if (si < protection)
			raid_is_no_protection[si] = NVMEIBTC_DS_MODE_DEAD;
		else
			raid_is_no_protection[si] = NVMEIBTC_DS_MODE_RW;
	}

	nvmeibc_io_perm_alert_set_unprotect_period(&env.dev->dp.io_perm_alert, 10*60);
	//switch to double degraded mode and verify the volume is writable
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, raid_is_no_protection, SW_TOPO__WAIT_ACK_DR, NULL);
	BUG_ON(env.dev->dp.io_perm_alert.arm != NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION);
	__verify_topo_io_perm(env, NVMEIB_IO_TYPE_PERMIT_ALL_NO_PROTECTION);

	//now, lets simulate as double dgrd write period ended
	env.dev->dp.io_perm_alert.unprotected_write_to_read_only_at = jiffies;
	nvmeibc_block_watchdog_run_once((*env.client->p->blok._private));
	BUG_ON(env.dev->dp.io_perm_alert.arm != NVMEIBC_IO_PERM_ARM_BIO_NO_PROTECTION_RO);
	__verify_topo_io_perm(env, NVMEIB_IO_TYPE_PERMIT_RDONLY);

	//back to normal
	tomaSimulator_switchTopoEC(env.sraid.tpr->header.uuid, raid_is_perfect, SW_TOPO__WAIT_ACK_DR, NULL);
	BUG_ON(env.dev->dp.io_perm_alert.arm != NVMEIBC_IO_PERM_ARM_BIO_OK);
	__verify_topo_io_perm(env, NVMEIB_IO_TYPE_PERMIT_ALL);
	nvmeibc_io_perm_alert_set_unprotect_period(&env.dev->dp.io_perm_alert, -1);
}

TEST_FUNC int unitest_io_perm_alert(struct NVMeshSystem *sys){
	struct nvmeibc_block_device dev;
	bzero(&dev, sizeof(dev));
	strlcpy(dev.name, "abcd", 5);

	(void)sys;
	__test_create_destroy(&dev);
	__test_switch_none(&dev);
	__test_switch_io_disabled(&dev);
	__test_switch_no_protection(&dev);
	__test_switch_detaching(&dev);
	__test_to_string(&dev);
	__test_send_and_detaching(&dev);
	__test_watchdog(sys);

	unitest_print("*** %s - done\n", __FUNCTION__);
	return 0;
}
