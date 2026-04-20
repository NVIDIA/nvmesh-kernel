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
#include "nvmeibt_disk_segment_basics.h"	// NVMEIBT_SEG_DIRTY_BITS_STATE_* used by eviction Phase 5
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
	const struct sb_praid_conf *pr_c = &vol->chunks[0].raids[0];
	const struct sb_praid_topo *pr_t = &vol->topo_chunks[0].raids[0];
	SCENARIO_PRINT(__AUTOID__, "Waiting for volume @DEV_NAME {@INT+@INT} to be ioable", vol->name, pr_c->D, pr_c->P);
	/*for (unsigned s = 0; s < (pr_c->D + pr_c->P); s++ ) {
		int node_idx = sb_cluster_get_node_idx_from_disk_uuid(cfg, pr_c->segs[s].disk_uuid);
		peer_toma_simu_set_seg_inject(cfg->nodes[node_idx].peer, {pr_c->segs[s].uuid, });	// Inject degraded mode
	}*/
	WAIT_UNTIL(sb_cluster_topo_prd_is_ioable(pr_t));
	if (sb_cluster_vol_has_any_live_toma_local_segs(cfg, v)) {		// Otherwise no work will be done by the real toma.
		SCENARIO_PRINT(__AUTOID__, "Attaching clients, todo...");
	}
}

/* Well-known V_R1 UUIDs, named by their *role* in the eviction scenario.
 * V_R1 lives at sb_cluster_conf.vols[1] with one chunk + one praid; the SB_*
 * macros (mongodb_simu.h) encode that into the sandbox's u32 UUID scheme. */
#define V_R1_SEG_UUID(seg_idx)     sb_seg_uuid(/*vol*/1, /*chunk*/0, /*raid*/0, (seg_idx))
#define V_R1_PRAID_UUID            sb_praid_uuid(1, 0, 0)
#define V_R1_EVICTED_SEG_UUID      V_R1_SEG_UUID(0)   /* node 0 disk 1 */
#define V_R1_SURVIVOR1_SEG_UUID    V_R1_SEG_UUID(1)   /* node 1 disk 0 */
#define V_R1_SURVIVOR2_SEG_UUID    V_R1_SEG_UUID(2)   /* node 1 disk 1 */
#define V_R1_REPLACEMENT_SEG_UUID  V_R1_SEG_UUID(3)   /* node 2 disk 0 (dormant fixture, promoted in Phase 1) */

/* Locate the segment in the V_R1 snapshot by u32 uuid, or -1 if absent. */
static int __rpt_find_seg(const struct mgmt_sim_praid_report_snapshot *r, u32 uuid) {
	for (int i = 0; i < r->n_segments; i++)
		if (r->segs[i].uuid == uuid)
			return i;
	return -1;
}

/* Phase-2 verification: toma has promoted seg[3] and reports the evicted slot
 * as deprecated + the replacement as "replacement" in the latest V_R1 report. */
static bool evict_replacement_reported(void) {
	const struct mgmt_sim_praid_report_snapshot *r = mgmt_sim_get_v_r1_report();
	int old_i = __rpt_find_seg(r, V_R1_EVICTED_SEG_UUID);
	int rep_i = __rpt_find_seg(r, V_R1_REPLACEMENT_SEG_UUID);
	return r->n_segments == 4
		&& old_i >= 0 && (r->segs[old_i].status1 == mdb_seg_dep)
		&& rep_i >= 0 && (r->segs[rep_i].status1 == mdb_seg_rep);
}

/* Phase-2 verification: additionally, the replacement's
 * (peer node 2 has surfaced it in its ACT_TOPO reply). */
static bool evict_replacement_up(void) {
	const struct mgmt_sim_praid_report_snapshot *r = mgmt_sim_get_v_r1_report();
	int rep_i = __rpt_find_seg(r, V_R1_REPLACEMENT_SEG_UUID);
	return evict_replacement_reported() && (rep_i >= 0);
}

/* Phase-4 verification: toma reported at least one segment as "under_recovery"
 * at some point since the last reset -- proves the praid reached SWITCH_TOPO_U. */
static bool evict_under_recovery(void) {
	return mgmt_sim_get_v_r1_report()->was_under_recovery_witnessed;
}

/* Phase-6 verification: recovery actually happened AND the praid converged back
 * to 3 normal segments, with the replacement present. */
static bool evict_rebuild_complete(void) {
	const struct mgmt_sim_praid_report_snapshot *r = mgmt_sim_get_v_r1_report();
	if (!r->was_under_recovery_witnessed || r->n_segments != 3) return false;
	if (__rpt_find_seg(r, V_R1_REPLACEMENT_SEG_UUID) < 0) return false;
	for (int i = 0; i < r->n_segments; i++)
		if (r->segs[i].status1 != mdb_seg_RW)
			return false;
	return true;
}

/*
 * Disk eviction / segment replacement scenario for V_R1 (NVMESH-8156).
 *
 * Reproduces the production drive-eviction + rebuild procedure captured in the
 * lab. The phase order is the contract; per-phase sandbox adapters land in
 * subsequent commits as each phase goes live.
 *
 * V_R1 is a 3-way mirror (D=1, P=2). The eviction moves data off seg[0]
 * (on the live toma's disk[1]) to a replacement seg[3] on peer node 2's
 * disk[0]. seg[1] and seg[2] on peer node 1 provide the surviving mirror.
 *
 * Conventions in the comments below:
 *   [REAL]    behavior of the code under test (live toma leader on node 0)
 *   [SANDBOX] behavior the test harness drives (mgmt_sim / peer_toma_simu)
 *   [VERIFY]  what the scenario waits for (expressed on the pRaidReport snapshot)
 */
static void scenario_evict_rebuild_r1(void) {
	SCENARIO_PRINT(__AUTOID__, "start (inert outline -- phases go live incrementally)");

	/* ====================================================================
	 * PHASE 1 -- Management triggers eviction + rebuild
	 *
	 * Production context: user clicks "Evict Drive"; mgmt marks the disk
	 * isOutOfService, allocates a replacement segment, sends two messages
	 * + a keep-alive.
	 *
	 * [SANDBOX] mgmt_sim sends, in order:
	 *   (a) updateVolume v2 with 4 segments:
	 *         seg[0] "markedForRebuild_old"  (evicted, flag 'R')
	 *         seg[1] "normal"                (surviving mirror)
	 *         seg[2] "normal"                (surviving mirror)
	 *         seg[3] "markedForRebuild"      (replacement, flag 'S')
	 *         vol action="markedForRebuild", status="online"
	 *   (b) hardwareConfiguration with the evicted disk isOutOfService=true
	 *   (c) leader keep-alive
	 * [REAL]    toma leader receives (effects observed in Phase 2).
	 * ==================================================================== */
	mgmt_sim_reset_v_r1_report_state();
	SCENARIO_PRINT(__AUTOID__, "Phase 1: updateVolume v2 with replacement segment");
	{
		static const struct mgmt_sim_vol_seg_update evict_segs[] = {
			{ .seg_idx = 0, .praid_idx = 0, .status = "markedForRebuild_old" },
			{ .seg_idx = 1, .praid_idx = 1, .status = "normal" },
			{ .seg_idx = 2, .praid_idx = 2, .status = "normal" },
			{ .seg_idx = 3, .praid_idx = 0, .status = "markedForRebuild" },
		};
		mgmt_sim_send_volume_update(1, "online", "markedForRebuild", evict_segs, (int)ARRAY_SIZE(evict_segs));
	}
	yield();

	SCENARIO_PRINT(__AUTOID__, "Phase 1: HW cfg marking seg[0] disk OOS");
	sb_cluster_get_conf()->live->disks[1].is_out_of_service = true;
	mgmt_sim_send_msg_latest_hw_config();
	yield();
	mgmt_sim_send_leader_keep_alive();

	/* ====================================================================
	 * PHASE 2 -- Toma reports replacement topology; verify
	 *
	 * [REAL]    toma leader:
	 *   - Parses updateVolume v2; maps flags to internal status:
	 *       seg[0] -> deprecated, seg[3] -> replacement,
	 *       seg[1]/seg[2] -> normal.
	 *   - Sees disk isOutOfService -> seg[0] skips zeroing -> X_DONE.
	 *   - leader_switch_to_replacement_seg() promotes seg[3] to active.
	 *   - seg[3] is on remote node 2 -> no local GPT write.
	 *   - Emits updatePRaidReport with seg[0]=deprecated,
	 *     seg[3]=replacement vitality=up, seg[1]/seg[2]=normal.
	 * [SANDBOX] node 2's peer_toma_simu must surface seg[3] in its ACT_TOPO
	 *           reply even though the leader's BIN_TOPO didn't include it
	 *           before updateVolume v2.
	 * [VERIFY]  WAIT_UNTIL snapshot shows 4 segments with seg[0]=deprecated,
	 *           seg[3]=replacement, seg[3] vitality=up.
	 * ==================================================================== */
	WAIT_UNTIL(evict_replacement_reported());
	WAIT_UNTIL(evict_replacement_up());

	/* ====================================================================
	 * PHASE 3 -- Management removes the deprecated segment
	 *
	 * Production context: mgmt saw seg[0]=deprecated in Phase 2's report
	 * and now drops seg[0] from the volume.
	 *
	 * [SANDBOX] mgmt_sim sends updateVolume v3 with 3 segments:
	 *             seg[1] "normal", seg[2] "normal",
	 *             seg[3] "markedForRebuild"
	 *             vol action="markedForRebuild", status="degraded"
	 *           followed by a leader keep-alive.
	 * [REAL]    toma leader receives (effects observed in Phase 4).
	 * ==================================================================== */
	SCENARIO_PRINT(__AUTOID__, "Phase 3: updateVolume v3 removing the deprecated segment");
	{
		static const struct mgmt_sim_vol_seg_update post_evict_segs[] = {
			{ .seg_idx = 1, .praid_idx = 1, .status = "normal" },
			{ .seg_idx = 2, .praid_idx = 2, .status = "normal" },
			{ .seg_idx = 3, .praid_idx = 0, .status = "markedForRebuild" },
		};
		mgmt_sim_send_volume_update(1, "degraded", "markedForRebuild", post_evict_segs, (int)ARRAY_SIZE(post_evict_segs));
	}
	yield();
	mgmt_sim_send_leader_keep_alive();

	/* ====================================================================
	 * PHASE 4 -- Toma enters under_recovery; verify
	 *
	 * [REAL]    toma advances praid sync_cmd:
	 *             STABLE -> RESET_REGISTRANTS -> SWITCH_TOPO_I
	 *                    -> SWITCH_TOPO_W     -> SWITCH_TOPO_U
	 *           In SWITCH_TOPO_U:
	 *             - One of seg[1]/seg[2] (peer node 1) -> OWNER_RECOVERER
	 *             - seg[3] (peer node 2) -> UNDER_RECOVERY_R
	 *           Emits updatePRaidReport with seg[3] status="under_recovery".
	 * [SANDBOX] mgmt_sim parser latches was_under_recovery_witnessed=true on
	 *           first "under_recovery" observation so the test can see it
	 *           after subsequent reports overwrite the per-seg field.
	 * [VERIFY]  WAIT_UNTIL was_under_recovery_witnessed on V_R1 snapshot.
	 * ==================================================================== */
	WAIT_UNTIL(evict_under_recovery());

	/* ====================================================================
	 * PHASE 5 -- Fake recovery completion in the sandbox
	 *
	 * Production context: a hidden client on node 2 copies data from
	 * OWNER_RECOVERER on node 1 to seg[3]. When done, the peer toma on
	 * node 1 transitions OWNER_RECOVERER -> OWNER_RECOVERER_DONE. The
	 * leader sees that via APPEND_ENTRIES_REP and finalizes rebuild.
	 * The sandbox has no hidden client -- we force the peer's reported
	 * state directly.
	 *
	 * [SANDBOX] Set dirty_bits override on node 1's peer simulator for
	 *           BOTH seg[1] and seg[2] to OWNER_RECOVERER_DONE (leader
	 *           picks one as OWNER_RECOVERER; we can't know which, so
	 *           cover both). Mechanism: peer_toma_simu_set_seg_inject().
	 * [REAL]    no action this phase (Phase 6 observes the effects once
	 *           the next ACT_TOPO reply is processed).
	 * ==================================================================== */
	SCENARIO_PRINT(__AUTOID__, "Phase 5: forcing OWNER_RECOVERER_DONE on node 1's surviving mirrors");
	{
		struct peer_toma_simu *p1 = sb_cluster_get_conf()->nodes[1].peer;
		peer_toma_simu_set_seg_inject(p1, &(struct toma_simu_inject_seg_state_t){
			.uuid = V_R1_SURVIVOR1_SEG_UUID,
			.dbits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE,
		});
		peer_toma_simu_set_seg_inject(p1, &(struct toma_simu_inject_seg_state_t){
			.uuid = V_R1_SURVIVOR2_SEG_UUID,
			.dbits_state = NVMEIBT_SEG_DIRTY_BITS_STATE_OWNER_RECOVERER_DONE,
		});
	}
	mgmt_sim_send_leader_keep_alive();

	/* ====================================================================
	 * PHASE 6 -- Rebuild complete; verify
	 *
	 * [REAL]    toma leader on seeing OWNER_RECOVERER_DONE from the peer:
	 *             - Surviving seg: OWNER_RECOVERER  -> OWNER_IDLE
	 *             - Replacement:   UNDER_RECOVERY_R -> OWNER_IDLE
	 *             - Praid:         SWITCH_TOPO_U    -> STABLE
	 *             - Emits updatePRaidReport with all 3 segs "normal".
	 * [SANDBOX] mgmt_sim parses the final report; latch remains set.
	 * [VERIFY]  WAIT_UNTIL snapshot has was_under_recovery_witnessed
	 *           AND n_segments==3 AND seg[3] present AND all "normal".
	 * ==================================================================== */
	WAIT_UNTIL(evict_rebuild_complete());
	SCENARIO_PRINT(__AUTOID__, "Phase 6: V_R1 segment replacement rebuild complete");
	/* Drop the Phase 5 overrides now that rebuild is verified; otherwise the
	 * peer would keep pinning seg[1]/seg[2] at OWNER_RECOVERER_DONE and block
	 * any later topology transitions (e.g. the delete-driven X_ZERO->X_DONE). */
	peer_toma_simu_clear_seg_injects(sb_cluster_get_conf()->nodes[1].peer);
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

	SCENARIO_PRINT(__AUTOID__, "sending addVolume @DEV_NAME, waiting for report target", cfg->vols[0].name);
	mgmt_sim_send_add_volume(0);
	WAIT_UNTIL(mgmt_sim_consume_got_report_target());
	if (1) {		// Simulate as if kafka resent an old message again
		WAIT_UNTIL(sim_broker_topic_is_empty(kb_vol));
		SCENARIO_PRINT(__AUTOID__, "sending old(-1) add volume msg, Will be ignored by Toma");
		sim_broker_topic_msg_inject_next_msg_offset(kb_vol, -1);
		mgmt_sim_send_add_volume(1);		// Will be ignored by Toma
	}

	mgmt_sim_send_leader_keep_alive();
	SCENARIO_PRINT(__AUTOID__, "sending addVolume @DEV_NAME, waiting for pRaid report", cfg->vols[1].name);
	mgmt_sim_send_add_volume(1);
	WAIT_UNTIL(mgmt_sim_v_r1_praid_reported());
	mgmt_sim_send_leader_keep_alive();
	mgmt_sim_send_praid_report_req(cfg->vols[0].chunks[0].raids[0].uuid);		// Todo: Send a real value and verify it

	if (1) {		// Simulate as if kafka resent a very old message again
		SCENARIO_PRINT(__AUTOID__, "sending old(-2) add volume msg, Will be ignored by Toma");
		sim_broker_topic_msg_inject_next_msg_offset(kb_vol, -2);
		mgmt_sim_send_add_volume(1);		// Will be ignored by Toma
	}

	scenario_user_rpcs_generic();
	scenario_user_rpcs_praid();
	scenario_attach_good_path_io_detach_on_volume(0);
	scenario_attach_good_path_io_detach_on_volume(1);

	scenario_evict_rebuild_r1();

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
