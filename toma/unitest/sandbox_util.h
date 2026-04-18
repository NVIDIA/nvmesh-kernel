/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#pragma once
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <signal.h>		// BUG_ON uses raise(SIGABRT)

#define SANDBOX_PRINT(fmt, ...)      fprintf(stderr, "SANDBOX: " fmt, __VA_ARGS__)      // Todo: Remove me, use binary tracing
#define SANDBOX_PRINT_TMP(fmt, ...)  fprintf(stderr, "SANDBOX: " COL_PURPL fmt COL_RESET, __VA_ARGS__)
#define N_SANDBOX(name, fmt, ...) _NMIRROR_LOGLEVEL(IMf, LOG_DEBUG, name, NVMEIB_LOG_ETERNAL, "SANDBOX: ", fmt, ## __VA_ARGS__)

#undef BUG_ON
#define BUG_ON(condition)	({ const int hit__ = !!(condition); if (hit__) { \
	fprintf(stderr, "************************** BUG!!!! at %s:%d - %s(), val=%d, condition=%s\n", __FILE__, __LINE__, __FUNCTION__, hit__, #condition); \
	nvmeibt_flush_all_and_terminate(); \
	raise(SIGABRT);} \
})
//#define WARN(condition, fmt, ...) 	do { const int hit = !!(condition); if (hit) {/*dump_stack(); */SANDBOX_PRINT("************************** BUG!!!! at %s() line %d, val=%d, condition=%s\n", __FUNCTION__, __LINE__, hit, #condition); raise(SIGABRT);} } while(0)
