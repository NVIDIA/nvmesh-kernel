/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_TREND_H
#define NVMEIB_TREND_H

#include "kr_incs.h"

//Optional improvements:
//Use only nvmeib_trend_md's @head, no need for @tail
struct nvmeib_trend_md {
	int head;
	int tail;
	int max;
};

#define NVMEIB_TREND_MAX_SIZE 5
#define NVMEIB_TREND_MD_INIT(max_size) {.head = -1, .tail = 0, .max = max_size}
#define NVMEIB_TREND_HEAD_IDX(t) (t).md.head
#define NVMEIB_TREND_TAIL_IDX(t) (t).md.tail
#define NVMEIB_TREND_MAX(t) (t).md.max
#define NVMEIB_TREND_IS_INIT(t) (NVMEIB_TREND_HEAD_IDX(t) >= -1)
#define NVMEIB_TREND_IS_EMPTY(t) (NVMEIB_TREND_HEAD_IDX(t) == -1)
#define NVMEIB_TREND_HEAD(t) ((t).data[NVMEIB_TREND_HEAD_IDX(t)])
#define NVMEIB_TREND_TAIL(t) (t).data[NVMEIB_TREND_TAIL_IDX(t)]
#define EUCMOD(a, b)  ((a) < 0 ? ((((a) % (b)) + (b)) % (b)) : ((a) % (b)))

struct nvmeib_single_trend_t {
    int data;
    struct timeval tv;
};

struct nvmeib_trend {
	struct nvmeib_trend_md md;
	struct nvmeib_single_trend_t data[NVMEIB_TREND_MAX_SIZE];
};

typedef int (*nvmeib_trend_call_fn)(struct nvmeib_single_trend_t *data, void *arg);

#define NVMEIB_TREND_INIT {NVMEIB_TREND_MD_INIT(NVMEIB_TREND_MAX_SIZE),}
#define NVMEIB_TREND_IS_FULL(arr) (!NVMEIB_TREND_IS_EMPTY(arr)) && \
	((NVMEIB_TREND_HEAD_IDX(arr) + 1) % NVMEIB_TREND_MAX(arr) == NVMEIB_TREND_TAIL_IDX(arr))

static inline void nvmeib_trend_init(struct nvmeib_trend *arr) {
	arr->md.head = -1;
	arr->md.max = NVMEIB_TREND_MAX_SIZE;
}

static inline void nvmeib_trend_update_head(struct nvmeib_trend *arr, int insert) {
	NVMEIB_TREND_HEAD(*arr).data = insert;
	do_gettimeofday(&NVMEIB_TREND_HEAD(*arr).tv);
}

static inline void nvmeib_trend_insert(struct nvmeib_trend *arr, int insert) {
	int next = (NVMEIB_TREND_HEAD_IDX(*arr) + 1) % NVMEIB_TREND_MAX(*arr);
	if (NVMEIB_TREND_IS_INIT(*arr) && NVMEIB_TREND_IS_FULL(*arr)) {
	    NVMEIB_TREND_TAIL_IDX(*arr) = (next + 1) % NVMEIB_TREND_MAX(*arr);
	}

	NVMEIB_TREND_HEAD_IDX(*arr) = next;
	nvmeib_trend_update_head(arr, insert);
}

int nvmeib_trend_foreach(struct nvmeib_trend *arr, nvmeib_trend_call_fn fn, void *arg);

#endif /*NVMEIB_TREND_H */