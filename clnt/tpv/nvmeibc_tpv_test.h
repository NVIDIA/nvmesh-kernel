/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_TPV_TEST_H
#define NVMEIBC_TPV_TEST_H

/*
 * nvmeibc_tpv_test.h - TPV kernel self-test declarations.
 *
 * This header is included by nvmeibc_tpv_proc.c to register the selftest
 * proc entry.  The actual test implementations live in nvmeibc_tpv_test.c.
 *
 * nvmeibc_tpv_test.c also provides the stub implementations of the CDV
 * transport externs declared in nvmeibc_tpv_persist.c, nvmeibc_tpv_io.c,
 * nvmeibc_tpv_allocator.c, and nvmeibc_tpv_recovery.c that have no
 * production implementation yet (pending CDV block-layer integration and
 * IB admin channel additions in later milestones).
 *
 * Proc entry:
 *   /proc/nvmeibc/tpv/<name>/selftest  (read-only)
 *   Reading the file runs all five kernel self-tests against the first TPV
 *   that has this proc entry registered and reports a summary.
 */

#ifdef __KERNEL__

/*
 * nvmeibc_tpv_run_selftests - proc fill function for the "selftest" entry.
 *
 * Signature matches proc_fill_t: (void *arg, char *buf, size_t len).
 * arg is the struct nvmeibc_tpv * passed at registration.
 */
ssize_t nvmeibc_tpv_run_selftests(void *arg, char *buf, size_t len);

#endif /* __KERNEL__ */

#endif /* NVMEIBC_TPV_TEST_H */
