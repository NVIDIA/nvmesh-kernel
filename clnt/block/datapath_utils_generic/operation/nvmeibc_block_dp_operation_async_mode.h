/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_DP_OPERATION_ASYNC_CALLBACK_MODE_H
#define NVMEIBC_DP_OPERATION_ASYNC_CALLBACK_MODE_H

/*********************** Async Transition ********************************/
/* This file defines the asyncronous transition between stages / state machines:
Recovery Operations (Syncs):
		Kernel:    completions driven, in interrupt context.
		User mode: fiber context with same stack. Completion wakes up request sender
   Async transitions happen when:
		1. 1 or more disk commands are sent (disk IO / gen commands). Transport layer
		2. 1 or more rdma requests are sent (cmpxcng lock, write binfo). Transport layer
		3. Statem-machine is completed, and execution is passed back to caller. Internal
		4. Occasionally: Move to next stage of current state machine but must change context interrupt->thread, etc
IO:
   Async transitions happen like above and also, when:
		4. Calling a sync to solve a problem (internal). Like unknown txid, journal garbage collection, etc
		5. Allocation journals state machine with JAM

2 transition modes are supported:
	1. Stack preserving, like python async await(). After async function completes continue on the same stack.
		Used in user space cooperative fiber modes. Also called fiber mode.
	2. Completion context. Execution flow (state machines) continues in completion context of async function
		Used in kernel (possibly) interrupt completion mode.
*/
#ifndef BLKCMP_SO_COMPLETION_PRESERVE_STACK
	#define BLKCMP_SO_DEFINE_BLOCKING_CONTEXT 								// No blocking context, empty struct
	#define BLKCMP_SO_BLOCKING_CONTEXT_ALLOC(ctx) ({ })						// Nothing to do
	#define BLKCMP_SO_ASYNC_AWAIT_RV(  state_machine) state_machine			// Send async request (disk/gen commands, async state machine, lock/unlock, etc)
	#define BLKCMP_SO_ASYNC_AWAIT(     state_machine) state_machine			// Send async request (Send function cant fail). Just call a state machine
	#define BLKCMP_SO_ASYNC_RESUME_CMP(state_machine) state_machine			// CMP = Completion context, Call next state machine from completion context
	#define BLKCMP_SO_ASYNC_RESUME_SND(state_machine) 						// SND = Async-op-sender context, Already called next state machine in completion context, so do nothing
	#define BLKCMP_SO_ASYNC_RESUME_CAL(state_machine) state_machine			// CAL = Caller (parent) context, Just transition back to caller state machine
	#define BLKCMP_SO_ASYNC_RESUME_CUR(...)           return __VA_ARGS__	// CUR = Resume next stage of current state machine. State machine function will be restarted as callback in completion context

#else
	#define BLKCMP_SO_DEFINE_BLOCKING_CONTEXT struct { struct completion c; } wait
	#define BLKCMP_SO_BLOCKING_CONTEXT_ALLOC(ctx) ({ init_completion(  &(ctx)->wait.c); })	// First initialization of blocking context
	#define SO_CMP_INIT(ctx) ({ reinit_completion(  &(ctx)->wait.c); })		// Arbitrary object  which have a sleeping context (completion).
	#define SO_CMP_WAIT(ctx) ({ wait_for_completion(&(ctx)->wait.c); })
	#define SO_CMP_COMP(ctx) ({ complete(           &(ctx)->wait.c); })
	#define BLKCMP_SO_ASYNC_AWAIT_RV(state_machine) ({ \
		int send_rv; \
		SO_CMP_INIT(so->o); \
		send_rv = state_machine; \
		if (send_rv == 0) { SO_CMP_WAIT(so->o); } else { SO_CMP_COMP(so->o); } \
		send_rv; \
	})
	#define BLKCMP_SO_ASYNC_AWAIT(     state_machine) ({ SO_CMP_INIT(so->o); state_machine; SO_CMP_WAIT(so->o); })
	#define BLKCMP_SO_ASYNC_RESUME_CMP(state_machine) SO_CMP_COMP(so->o)	// CMP = Completion context, Wakeup fiber which will resume state machine, dont call state machine directly
	#define BLKCMP_SO_ASYNC_RESUME_SND(state_machine) state_machine			// SND = Async-op-sender context, Resume state machine by calling its function directly, inasted of in completion context
	#define BLKCMP_SO_ASYNC_RESUME_CAL(state_machine) ({ if (0) {state_machine;}; })	// CAL = Caller context, Do nothing, caller code is up in the stack just finish current function and its state machine will continue, so no need to launch it explicitly.
	#define BLKCMP_SO_ASYNC_RESUME_CUR(...)           goto _func_start		// Continue run within the same sate machine function, jump to state machine function start

#endif
	#define ASYNC_AWAIT_AND_RESUME(expression, ...) ({ expression; BLKCMP_SO_ASYNC_RESUME_CUR(__VA_ARGS__); })

/****************** IO Operations: Same logic as above ***********************/
#ifndef BLKCMP_IO_COMPLETION_PRESERVE_STACK
	#define BLKCMP_IO_ASYNC_AWAIT(     state_machine) state_machine
	#define BLKCMP_IO_ASYNC_RESUME_CMP(state_machine) state_machine
	#define BLKCMP_IO_ASYNC_RESUME_SND(state_machine)
	#define BLKCMP_IO_ASYNC_RESUME_CAL(state_machine) state_machine
#else
	#define BLKCMP_IO_ASYNC_AWAIT(     state_machine) ({ SO_CMP_INIT(o); state_machine; SO_CMP_WAIT(o); })
	#define BLKCMP_IO_ASYNC_RESUME_CMP(state_machine)	 SO_CMP_COMP(o)
	#define BLKCMP_IO_ASYNC_RESUME_SND(state_machine) state_machine
	#define BLKCMP_IO_ASYNC_RESUME_CAL(state_machine) ({ if (0) {state_machine;}; })
	#define BLKCMP_IO_COMPLETION_NO_LOCKS_TIMER_RETRY (1)			/* Caller will retry the io */
	#define BLKCMP_SO_LAUNCH_ON_CALLER_STACK          (1)			/* Syncs (Blockset-fixup) is launch on caller stack, instead of resubmition or other thread. IO's / Recovery launch syncs */
#endif
#define BLKCMP_IO_ONLY_IF_PRESERVE_STACK(expression) BLKCMP_IO_ASYNC_RESUME_SND(expression)			// Executed only when fibers mode

/********************* Recovery algorithm: Same logic as above ***************/
#ifndef BLKCMP_RC_COMPLETION_PRESERVE_STACK
	#define BLKCMP_RC_ASYNC_AWAIT(     sm, r)		sm					// Send async request (Send function cant fail). Just call a state machine
	#define BLKCMP_RC_ASYNC_RESUME_CMP(sm, r, d)	sm					// CMP = Completion context, Call next state machine from completion context
	#define BLKCMP_RC_ASYNC_RESUME_SND(sm)								// SND = Async-op-sender context, Already called next state machine in completion context, so do nothing
	#define BLKCMP_RC_ASYNC_RESUME_SYN(sm)			sm					/* SYN = Syncronous completion, same as CMP */
#else
	#define BLKCMP_RC_ASYNC_AWAIT(     sm, r)		({ SO_CMP_INIT(r); sm; SO_CMP_WAIT(r); })	// Same transition mechanism as syncs
	#define BLKCMP_RC_ASYNC_RESUME_CMP(sm, r, d) 	({ if (d) {sm; }; SO_CMP_COMP(r); })		// CMP = Completion context, Wakeup fiber which will resume state machine, dont call state machine directly
	#define BLKCMP_RC_ASYNC_RESUME_SND(sm)			sm					// SND = Async-op-sender context, Resume state machine by calling its function directly, inasted of in completion context
	#define BLKCMP_RC_ASYNC_RESUME_SYN(sm) 								/* SYN = Like CMP but no need to wakup, because it is a syncronous function */
#endif

/********************* Remove unnecessary rescheduling ***************/
#ifndef BLKCMP_SO_COMPLETION_PRESERVE_STACK
	/* Regular kernel async scheduling */
	#define BLKCMP_ANY_schedule_work 					schedule_work
	#define BLKCMP_ANY_schedule_delayed_work			schedule_delayed_work
	#define BLKCMP_ANY_schedule_work_on_sys_wq_rand_cpu schedule_work_on_sys_wq_rand_cpu
	#define BLKCMP_ANY_dp_block_schedule_operation_work	dp_block_schedule_operation_work
#else
	/* Syncronous direct call */
	#define BLKCMP_ANY_schedule_work(w)                     ({ (  w)->func(w);             true; })	// static inline bool schedule_work(struct work_struct *work) { work->func(work); return true; }
	#define BLKCMP_ANY_schedule_delayed_work(d, ...)        ({ (d)->work.func(&(d)->work); true; }) //static inline bool schedule_delayed_work(struct delayed_work *dwork, unsigned long delay) { return schedule_work(&dwork->work); }
	#define BLKCMP_ANY_schedule_work_on_sys_wq_rand_cpu(w)  ({ (  w)->func(w);             true; })
	#define BLKCMP_ANY_dp_block_schedule_operation_work(o, w)	({ (void)(o); (w)->func(w); true; }) // inline static bool dp_block_schedule_operation_work(const struct operation *o, struct work_struct *work)
#endif

#endif
