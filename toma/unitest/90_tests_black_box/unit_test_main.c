/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "../sandbox_util.h"
#include "unit_test_main.h"
#include "nvmeibt_debug.h"				// Binary traces
#include "../mgmt_sim.h"
#include "../os/os_internal.h"
#include "../server/sandbox_nvmeibs_toma.h"
#include "../12_user/user_rpc_simu.h"
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
	void *stack = mmap(NULL, padded_length, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);	// Allocating 2 pages more than length requested with no read/write access
	void *rv;
	BUG_ON(stack == MAP_FAILED);
	BUG_ON(mprotect(stack, PAGE_SIZE, PROT_WRITE) < 0);				// Modify permissions for the first guard page only
	__init_stack_first_page(stack, padded_length);					// Writing to the first page details about the allocation and setting it back to no access permissions
	BUG_ON(mprotect(stack, PAGE_SIZE, PROT_NONE) < 0);				// Now first and last pages have zero access permissions
	rv = mmap(stack + PAGE_SIZE, length, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0, 0);
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
	os_sim_send_signal_to_toma(SIGUSR2);	yield();
	os_sim_send_signal_to_toma(SIGCHLD);	yield();
	os_sim_send_signal_to_toma(SIGUSR2);	yield();
}

static void scenario_user_rpcs(void) {
	SCENARIO_PRINT(__AUTOID__, "Testing RPCs start");
	user_rpc_send_to_toma("simulate dump_status");
	user_rpc_send_to_toma("simulate reread_conf");
	user_rpc_send_to_toma("simulate dump-clnt-hash 20");
	user_rpc_send_to_toma("simulate bm-garbage-collect 1");
	user_rpc_send_to_toma("simulate resend-praids-report vol1");
	user_rpc_send_to_toma("status server_csvs");
	user_rpc_send_to_toma("status errors");
	user_rpc_send_to_toma("disk-models list");
	WAIT_UNTIL(user_rpc_did_toma_reply_to_all_rpcs());
	SCENARIO_PRINT(__AUTOID__, "Testing RPCs done");
}

static void scenario_create_remove_r1(void) {
	mgmt_sim_send_msg_latest_hw_config(); yield();				// Send unrelated occasional HW config change
	SCENARIO_PRINT(__AUTOID__, "waiting for both disks ready for format");
	WAIT_UNTIL(mgmt_sim_both_disks_ready_for_format());

	SCENARIO_PRINT(__AUTOID__, "sending format drives");
	mgmt_sim_send_format_drives();
	nvmeibs_simu_send_extended_msg("HelloFromClnt");		// Send once an extended message to test the flow

	SCENARIO_PRINT(__AUTOID__, "waiting for both disks formatted ok");
	WAIT_UNTIL(mgmt_sim_both_disks_formatted_ok());

	SCENARIO_PRINT(__AUTOID__, "waiting for drive zeroing to complete");
	WAIT_UNTIL(mgmt_sim_both_disks_zeroing_done());

	mgmt_sim_send_msg_latest_hw_config(); yield();				// Send unrelated occasional HW config change
	SCENARIO_PRINT(__AUTOID__, "waiting for leader to exists");
	WAIT_UNTIL(mgmt_sim_get_n_leader_keep_alives_received() > 0);
	mgmt_sim_send_leader_keep_alive();

	SCENARIO_PRINT(__AUTOID__, "sending addVolume V_REMOTE1");
	mgmt_sim_send_add_volume_remote1();

	SCENARIO_PRINT(__AUTOID__, "waiting for reportTarget after V_REMOTE1");
	WAIT_UNTIL(mgmt_sim_consume_got_report_target());

	mgmt_sim_send_leader_keep_alive();
	SCENARIO_PRINT(__AUTOID__, "sending addVolume V_R1");
	mgmt_sim_send_add_volume_r1();

	SCENARIO_PRINT(__AUTOID__, "waiting for V_R1 pRaid report");
	WAIT_UNTIL(mgmt_sim_v_r1_praid_reported());
	mgmt_sim_send_leader_keep_alive();

	mgmt_sim_send_msg_latest_hw_config(); yield();				// Send unrelated occasional HW config change
	SCENARIO_PRINT(__AUTOID__, "sending deleteVolume V_R1");
	mgmt_sim_send_delete_volume_r1();
	mgmt_sim_send_leader_keep_alive();

	/* Zeroing is skipped for FIRST_USE_EVER segments (never activated) — segments go directly to X_DONE */

	SCENARIO_PRINT(__AUTOID__, "waiting for V_R1 praid deprecated in report");
	WAIT_UNTIL(mgmt_sim_v_r1_praid_deprecated());

	SCENARIO_PRINT(__AUTOID__, "sending deleteVolumeCompleted V_R1");
	mgmt_sim_send_delete_volume_completed_r1();

	SCENARIO_PRINT(__AUTOID__, "waiting for reportTarget after deleteVolumeCompleted (gc)");
	WAIT_UNTIL(mgmt_sim_consume_got_report_target());
	mgmt_sim_send_leader_keep_alive();							// Just additional unrelated keepalive to keep more pressure on toma
}

static void all_test_scenarios(void) {
	scenario_user_rpcs();
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
