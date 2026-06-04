/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_NONSLEEPABLE_H
#define NVMEIB_NONSLEEPABLE_H

/*
 * Mark / unmark a "non-sleepable" region (analogous to a Linux atomic context:
 * spinlock held, IRQs disabled, softirq/timer callback).
 *
 * In the KC simulator build, calls increment a per-thread depth counter. The
 * kmalloc/vmalloc shims read that counter and BUG() when a sleepable allocation
 * (GFP_KERNEL etc.) is attempted while depth > 0 — mirroring the kernel's
 * "scheduling while atomic" BUG.
 *
 * In real kernel builds these are no-ops; the kernel already enforces this via
 * might_sleep() and lockdep.
 *
 * Pair every nvmesh_enter_nonsleepable() with an nvmesh_exit_nonsleepable() on
 * every return path of the bracketed function.
 */

#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR == 1
extern __thread int __nonsleepable_depth;
#endif

static inline void nvmesh_enter_nonsleepable(void) {
#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR == 1
    __nonsleepable_depth++;
#endif
}
static inline void nvmesh_exit_nonsleepable(void)  {
#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR == 1
    BUG_ON(__nonsleepable_depth <= 0);
    __nonsleepable_depth--;
#endif
}

/* Suspend/resume bracket the in-process simulator boundary into srv/ code.
 * srv/ runs sleepable in real production (separate process); the simulator
 * collapses it into a synchronous call while a client spinlock is still held.
 * Without the bracket, srv/-side GFP_KERNEL allocations falsely trip the
 * non-sleepable-context trap. */
static inline int  nvmesh_nonsleepable_suspend(void) {
#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR == 1
    int s = __nonsleepable_depth;
    __nonsleepable_depth = 0;
    return s;
#else
	return 0;
#endif
}
static inline void nvmesh_nonsleepable_resume(int saved) {
#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR == 1
    __nonsleepable_depth = saved;
#else
    (void)saved;
#endif
}

#endif /* NVMEIB_NONSLEEPABLE_H */
