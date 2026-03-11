/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "sandbox_util.h"
#include "unit_test_main.h"
#include "mgmt_sim.h"
#ifdef __cplusplus
	#ifdef NDEBUG
		#undef _FORTIFY_SOURCE			// https://github.com/sagemath/cysignals/issues/73#issuecomment-371909263, otherwise false positive detection of stack corruption on longjump
	#endif
#endif
#include <ucontext.h>
#include "nvmeibt_debug.h"				// Binary traces
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

/********************************************************************/
#define WAIT_UNTIL(cond) do { while (!(cond)) yield(); } while (0)

static void all_test_scenarios(void) {
	N_SANDBOX(__AUTOID__, "unit test thread: waiting for both disks ready for format");
	WAIT_UNTIL(mgmt_sim_both_disks_ready_for_format());

	N_SANDBOX(__AUTOID__, "unit test thread: sending format drives");
	mgmt_sim_send_format_drives();

	N_SANDBOX(__AUTOID__, "unit test thread: waiting for both disks formatted ok");
	WAIT_UNTIL(mgmt_sim_both_disks_formatted_ok());

	N_SANDBOX(__AUTOID__, "unit test thread: waiting for drive zeroing to complete");
	WAIT_UNTIL(mgmt_sim_both_disks_zeroing_done());

	N_SANDBOX(__AUTOID__, "unit test thread: sending addVolume V_REMOTE1");
	mgmt_sim_send_add_volume_remote1();

	N_SANDBOX(__AUTOID__, "unit test thread: waiting for reportTarget after V_REMOTE1");
	WAIT_UNTIL(mgmt_sim_consume_got_report_target());

	N_SANDBOX(__AUTOID__, "unit test thread: sending addVolume V_R1");
	mgmt_sim_send_add_volume_r1();

	N_SANDBOX(__AUTOID__, "unit test thread: waiting for V_R1 pRaid report");
	WAIT_UNTIL(mgmt_sim_v_r1_praid_reported());

	N_SANDBOX(__AUTOID__, "unit test thread: test scenario complete");
	scheduler.is_unit_test_done = true;
	yield();
	BUG_ON(true);														// Cannot reach here or will get stuck
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
	BUG_ON(!scheduler.is_unit_test_done);
	if (scheduler.ctx_thread_uni.uc_stack.ss_sp)
		__free_stack(scheduler.ctx_thread_uni.uc_stack.ss_sp);
	memset(&scheduler, 0, sizeof(scheduler));
}
