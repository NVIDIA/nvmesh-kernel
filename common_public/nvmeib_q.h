/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_Q_H
#define NVMEIB_Q_H	1

#include <linux/list.h>

#define NVMEIB_Q_LOG_LEVEL_DEFAULT 2 /* WARN */
#define NVMEIB_Q_LOG_LEVEL_VERBOSE 4 /* TRACE */

struct nvmeib_qent;
typedef void (*nvmeib_qfunc)(struct nvmeib_qent *work);

struct nvmeib_qent {
	struct list_head link;
	void (*f)(struct nvmeib_qent *);
#ifdef DEBUG_NVMEIB_Q
	void *add_fn;
	void *add_fn_prev;
	void *add_fn_prev2;
	long idx;
#endif
};

struct nvmeib_q;

struct nvmeib_q *nvmeib_startq(const char *name, unsigned int cpu);
void nvmeib_endq(struct nvmeib_q *q);
bool nvmeib_addq(struct nvmeib_q *q, struct nvmeib_qent *entry
#ifdef DEBUG_NVMEIB_Q
		, void *add_fn, void *add_fn_prev, void *add_fn_prev2
#endif
		);
bool nvmeib_delq(struct nvmeib_q *q, struct nvmeib_qent *entry);
bool nvmeib_cancel_qent(struct nvmeib_q *q, struct nvmeib_qent *entry);
bool nvmeib_flush_qent(struct nvmeib_q *q, struct nvmeib_qent *entry);
void nvmeib_flushq(struct nvmeib_q *q);
void nvmeib_drainq(struct nvmeib_q *q);
int nvmeib_qpid(struct nvmeib_q *q);
void nvmeib_q_set_log_level(struct nvmeib_q *q, unsigned int level);

#endif
