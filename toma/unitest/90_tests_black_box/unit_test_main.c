/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "../sandbox_util.h"
#include "unit_test_main.h"
#include "nvmeibt_debug.h"				// Binary traces
#include "../mgmt_sim.h"
#include "../11_os/os_internal.h"
#include "../11_os/nvmeibt_udev_simu_internal.h"
#include "../12_user/user_rpc_simu.h"
#include "../15_server/sandbox_nvmeibs_toma.h"
#include "../16_otherToma/peer_toma_simu.h"
#include "../kafka/sandbox_kafka_internal.h"
#ifdef __cplusplus
	#ifdef NDEBUG
		#undef _FORTIFY_SOURCE			// https://github.com/sagemath/cysignals/issues/73#issuecomment-371909263, otherwise false positive detection of stack corruption on longjump
	#endif
#endif
#include <ucontext.h>
#include <sys/mman.h>

/********************************************************************/
#define PAGE_SIZE (1UL << PAGE_SHIFT)	/* Guard pages before and after stack to detect overflow */
struct stack_first_page {
	char magic[16];
	size_t stack_size;
};

static inline void __init_stack_first_page(struct stack_first_page *me, size_t stack_size) {
	me->stack_size = stack_size;
	strncpy(me->magic, "|__uni_stack__|", sizeof(me->magic));
}

static void *__allocate_stack(size_t length) {
	const size_t padded_length = length + 2 * PAGE_SIZE;
	void *stack = mmap(NULL, padded_length, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);	// Allocating 2 pages more than length requested with no read/write access
	void *rv;
	BUG_ON(stack == MAP_FAILED);
	BUG_ON(mprotect(stack, PAGE_SIZE, PROT_WRITE) < 0);				// Modify permissions for the first guard page only
	__init_stack_first_page(stack, padded_length);					// Writing to the first page details about the allocation and setting it back to no access permissions
	BUG_ON(mprotect(stack, PAGE_SIZE, PROT_NONE) < 0);				// Now first and last pages have zero access permissions
	rv = mmap(stack + PAGE_SIZE, length, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
	BUG_ON(rv == MAP_FAILED);
	return rv;
}

static void __free_stack(void *addr) {
	void *stack = ((char *)addr - PAGE_SIZE);
	const struct stack_first_page *me = stack;
	BUG_ON((((uintptr_t)addr % PAGE_SIZE) != 0) || (uintptr_t)addr < 2*PAGE_SIZE);
	mprotect(stack, PAGE_SIZE, PROT_READ);
	BUG_ON(strcmp(me->magic, "|__uni_stack__|") != 0);
	BUG_ON(munmap(stack, me->stack_size) != 0);
}

static struct t_uni_thread_ctx {
	ucontext_t ctx_main, ctx_thread_uni;
	bool is_unit_test_done;
} scheduler;

static void yield(void) { swapcontext(&scheduler.ctx_thread_uni, &scheduler.ctx_main); }	// Yield unitest thread and let Toma main thread to continue

static void do_on_unitests_done(void) {
	const bool ok = scheduler.is_unit_test_done;
	const char *pass = COL_GREEN "passed" COL_RESET ", \t\tShutting down Toma app";
	const char *fail = COL_RED   "Did not finish!" COL_RESET ", Crashing... See Toma Bin logs for more info";
	SANDBOX_PRINT("All unit-tests: %s\n", (ok ? pass : fail));
	if (ok)
		yield();					// Last yield back to toma main thread, this fiber will not be continued
	BUG_ON(true);					// ok==true: Scheduler bug. ok=False: Unit-test did not finish properly
}

/********************************************************************/
#define WAIT_UNTIL_N(cond, max_yields) ({ \
	for (int __wu_i = 0; !(cond); ++__wu_i) { \
		BUG_ON(__wu_i >= (max_yields)); \
		yield(); \
	} \
})

#define WAIT_UNTIL(cond)  WAIT_UNTIL_N(cond, 200)		// For now, Just arbitrary amount of epoll events.
#define SCENARIO_PRINT(id, fmt, ...) _NMIRROR_LOGLEVEL(IMf, LOG_EMERG, id, NVMEIB_LOG_ETERNAL, "<> ", fmt,  ## __VA_ARGS__)

static void scenario_test_signals(void) {
	SCENARIO_PRINT(__AUTOID__, "start");
	os_sim_send_signal_to_toma(SIGUSR2);	yield();
	os_sim_send_signal_to_toma(SIGCHLD);	yield();
	os_sim_send_signal_to_toma(SIGUSR2);	yield();
	SCENARIO_PRINT(__AUTOID__, "end");
}

static void scenario_user_rpcs_disk_models(void) {
	SCENARIO_PRINT(__AUTOID__, "start");
	user_rpc_send_to_toma("disk-models set dummy1_disk_model ignore_metadata on");
	user_rpc_send_to_toma("disk-models set dummy1_disk_model is_zeroing_using_test_and_write off");
	user_rpc_send_to_toma("disk-models set dummy2_disk_model force_512b off");
	user_rpc_send_to_toma_and_set_expected_reply_size("disk-models list", (1 << 12));
	user_rpc_send_to_toma("disk-models remove dummy1_disk_model");
	user_rpc_send_to_toma("disk-models remove dummy2_disk_model");
	user_rpc_send_to_toma_and_set_expected_reply_size("disk-models list", (1 << 11));
	WAIT_UNTIL(user_rpc_did_toma_reply_to_all_rpcs());
	SCENARIO_PRINT(__AUTOID__, "done");
}

static void scenario_user_rpcs_generic(void) {
	SCENARIO_PRINT(__AUTOID__, "start");
	user_rpc_send_to_toma("simulate dump-clnt-hash 20");
	user_rpc_send_to_toma("simulate bm-garbage-collect 1");
	user_rpc_send_to_toma("status server_csvs");
	user_rpc_send_to_toma("status nm_json");
	user_rpc_send_to_toma("status errors");
	WAIT_UNTIL(user_rpc_did_toma_reply_to_all_rpcs());
	SCENARIO_PRINT(__AUTOID__, "done");
	scenario_user_rpcs_disk_models();
}
static void scenario_user_rpcs_praid(void) {
	SCENARIO_PRINT(__AUTOID__, "start");
	user_rpc_send_to_toma("simulate dump_status");
	user_rpc_send_to_toma("simulate reread_conf");
	user_rpc_send_to_toma("simulate resend-praids-report vol1");
	WAIT_UNTIL(user_rpc_did_toma_reply_to_all_rpcs());
	SCENARIO_PRINT(__AUTOID__, "done");
}

static void scenario_udev_events(void) {
	SCENARIO_PRINT(__AUTOID__, "start");
	nvmeibt_udev_simu_send_disk_event_to_toma(0, true);			yield();
	nvmeibt_udev_simu_send_disk_event_to_toma(1, false);		yield();
	nvmeibt_udev_simu_send_disk_event_to_toma(2, true);			yield();
  //nvmeibt_udev_simu_send_sata_event_to_toma(   true);			yield();
	nvmeibt_udev_simu_send_sata_event_to_toma(   false);		yield();
	WAIT_UNTIL(nvmeibt_udev_simu_did_toma_consume_all_events());
	SCENARIO_PRINT(__AUTOID__, "done");
}

void scenario_nvmeibs_messages(void) {
	SCENARIO_PRINT(__AUTOID__, "start");
	nvmeibs_simu_send_msg(NVMEIBS_TOMA_TRIGGER_JGC);						yield();
	nvmeibs_simu_send_msg(NVMEIBS_TOMA_WRITE_STATUS_REQ);					yield();
	nvmeibs_simu_send_msg(NVMEIBS_TOMA_REPORT_EVENT_DISK_CHANGE);			yield();
	nvmeibs_simu_send_msg(NVMEIBS_TOMA_REPORT_EVENT_PORT_GID_CHANGE);		yield();
	nvmeibs_simu_send_msg(NVMEIBS_TOMA_REPORT_EVENT_NIC_CHANGE);			yield();
	nvmeibs_simu_send_msg(NVMEIBS_TOMA_REPORT_EVENT_SERJIO_RANGE_CLEANED);	yield();
	nvmeibs_simu_send_extended_msg("HelloFromClnt");		// Send once an extended message to test the flow
	SCENARIO_PRINT(__AUTOID__, "sent");										yield();
}

void scenario_attach_good_path_io_detach_on_volume(int v) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	const struct sb_volume_conf *vol = &cfg->vols[v];
	//const struct sb_praid_conf *pr_c = &vol->chunks[0].raids[0];
	const struct sb_praid_topo *pr_t = &vol->topo_chunks[0].raids[0];
	SCENARIO_PRINT(__AUTOID__, "Waiting for volume @DEV_NAME to be ioable", vol->name);
	/*for (unsigned s = 0; s < (pr_c->D + pr_c->P); s++ ) {
		int node_idx = sb_cluster_get_node_idx_from_disk_uuid(cfg, pr_c->segs[s].disk_uuid);
		peer_toma_simu_set_seg_inject(cfg->nodes[node_idx].peer, {pr_c->segs[s].uuid, });
	}*/
	WAIT_UNTIL(sb_cluster_topo_prd_is_ioable(pr_t));
	SCENARIO_PRINT(__AUTOID__, "Attaching clients, todo...");
}

static void scenario_create_remove_r1(void) {
	const struct sb_cluster_conf *cfg = sb_cluster_get_const_conf();
	struct sim_broker_topic *kb_vol = sim_broker_topic_find_by(KTOPIC_TYPE_M2T_VOLUMES);
	mgmt_sim_send_msg_latest_hw_config(); yield();				// Send unrelated occasional HW config change
	SCENARIO_PRINT(__AUTOID__, "waiting for both disks ready for format");
	WAIT_UNTIL(mgmt_sim_both_disks_ready_for_format());

	SCENARIO_PRINT(__AUTOID__, "sending format 2 drives request: {@STR, @STR}", cfg->live->disks[0].serial, cfg->live->disks[1].serial);
	mgmt_sim_send_format_drive(0);
	mgmt_sim_send_format_drive(1);
	scenario_nvmeibs_messages();								// While drives are formatting test server messages
	scenario_udev_events();

	SCENARIO_PRINT(__AUTOID__, "waiting for both disks format+zeroing done");
	WAIT_UNTIL(mgmt_sim_drive_format_is_done(0) && mgmt_sim_drive_format_is_done(1));

	mgmt_sim_send_msg_latest_hw_config(); yield();				// Send unrelated occasional HW config change
	mgmt_sim_send_disk_report_req(0); yield();
	SCENARIO_PRINT(__AUTOID__, "waiting for leader to exists");
	WAIT_UNTIL(mgmt_sim_get_n_leader_keep_alives_received() > 0);
	mgmt_sim_send_leader_keep_alive();

	SCENARIO_PRINT(__AUTOID__, "sending addVolume V_REMOTE1, waiting for report target");
	mgmt_sim_send_add_volume_remote1();
	WAIT_UNTIL(mgmt_sim_consume_got_report_target());
	if (1) {		// Simulate as if kafka resent an old message again
		WAIT_UNTIL(sim_broker_topic_is_empty(kb_vol));
		SCENARIO_PRINT(__AUTOID__, "sending old(-1) add volume msg, Will be ignored by Toma");
		sim_broker_topic_msg_inject_next_msg_offset(kb_vol, -1);
		mgmt_sim_send_add_volume_r1();		// Will be ignored by Toma
	}

	mgmt_sim_send_leader_keep_alive();
	SCENARIO_PRINT(__AUTOID__, "sending addVolume V_R1, waiting for V_R1 pRaid report");
	mgmt_sim_send_add_volume_r1();
	WAIT_UNTIL(mgmt_sim_v_r1_praid_reported());
	mgmt_sim_send_leader_keep_alive();
	mgmt_sim_send_praid_report_req(cfg->vols[0].chunks[0].raids[0].uuid);		// Todo: Send a real value and verify it

	if (1) {		// Simulate as if kafka resent a very old message again
		SCENARIO_PRINT(__AUTOID__, "sending old(-2) add volume msg, Will be ignored by Toma");
		sim_broker_topic_msg_inject_next_msg_offset(kb_vol, -2);
		mgmt_sim_send_add_volume_r1();		// Will be ignored by Toma
	}

	scenario_user_rpcs_generic();
	scenario_user_rpcs_praid();
	scenario_attach_good_path_io_detach_on_volume(0);

	SCENARIO_PRINT(__AUTOID__, "Simulate degraded mode of V_R1");
	peer_toma_simu_ignore_append_entries_by_node(2);
	mgmt_sim_send_volume_exclusive_attach_notify(cfg->vols[0].uuid);
	WAIT_UNTIL(mgmt_sim_v_r1_praid_reported());

	mgmt_sim_send_msg_latest_hw_config(); yield();				// Send unrelated occasional HW config change
	SCENARIO_PRINT(__AUTOID__, "sending deleteVolume V_R1, waiting for V_R1 praid deprecated in report");
	mgmt_sim_send_delete_volume_r1();
	mgmt_sim_send_leader_keep_alive();
	/* Zeroing is skipped for FIRST_USE_EVER segments (never activated) — segments go directly to X_DONE */
	WAIT_UNTIL(mgmt_sim_v_r1_praid_deprecated());

	SCENARIO_PRINT(__AUTOID__, "sending deleteVolumeCompleted V_R1");
	mgmt_sim_send_leader_keep_alive();							// Just additional unrelated keepalive to keep more pressure on toma
	mgmt_sim_send_delete_volume_completed_r1();

	SCENARIO_PRINT(__AUTOID__, "waiting for reportTarget after deleteVolumeCompleted (gc)");
	WAIT_UNTIL(mgmt_sim_consume_got_report_target());
	SCENARIO_PRINT(__AUTOID__, "waiting for kafka commit on deleteVolumeCompleted");
	WAIT_UNTIL(sim_broker_topic_is_empty(kb_vol));		// Verify Toma finished with volume deletion by committing offsets of all volume instructions
}

static void all_test_scenarios(void) {
	scenario_create_remove_r1();
	scenario_test_signals();
	SCENARIO_PRINT(__AUTOID__, "test scenario complete");
	os_sim_send_signal_to_toma(SIGKILL);		// Issue shutdown instruction
	scheduler.is_unit_test_done = true;
	do_on_unitests_done();
}

void toma_unit_test_thread_create(void) {
	scheduler.is_unit_test_done = false;
	getcontext(&scheduler.ctx_thread_uni);
	scheduler.ctx_thread_uni.uc_stack.ss_size = 16 * PAGE_SIZE;
	scheduler.ctx_thread_uni.uc_stack.ss_sp = __allocate_stack(scheduler.ctx_thread_uni.uc_stack.ss_size);
	scheduler.ctx_thread_uni.uc_link = &scheduler.ctx_main;
	makecontext(&scheduler.ctx_thread_uni, all_test_scenarios, 0 /* no arguments*/);
}

int toma_unit_test_thread_switch_to(void) {
	if (!scheduler.is_unit_test_done) {
		swapcontext(&scheduler.ctx_main, &scheduler.ctx_thread_uni);
		return 1;
	}
	return 0;
}

void toma_unit_test_thread_destroy(void) {
	if (!scheduler.is_unit_test_done)
		do_on_unitests_done();					// Force stop unit-tests
	if (scheduler.ctx_thread_uni.uc_stack.ss_sp)
		__free_stack(scheduler.ctx_thread_uni.uc_stack.ss_sp);
	memset(&scheduler, 0, sizeof(scheduler));
}
