/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_PUBLIC_POLL_H
#define NVMEIB_PUBLIC_POLL_H

#include "kr_incs.h"

/* register a polling context */
struct nvmeib_public_poll_ctx {
	void (*poll_f)(void *);
	void *poll_ctx;
	void (*done_f)(void *);
	void *done_ctx;
	volatile bool done;
	struct list_head link;
};

struct nvmeib_public_per_core;
struct nvmeib_public_per_node {
	int n_cores;
	int max_cores;
	struct nvmeib_public_per_core **cores;
};

struct nvmeib_public_poller {
	int n_nodes;
	struct nvmeib_public_per_node *nodes;
	void (*add_ctx)(struct nvmeib_public_per_core *core,
					struct nvmeib_public_poll_ctx *ctx);
};

struct nvmeib_public_poller * nvmeib_public_poller_create(void);
void nvmeib_public_poller_free(struct nvmeib_public_poller *poller);
void nvmeib_public_poller_add_ctx(
	struct nvmeib_public_per_core *core, struct nvmeib_public_poll_ctx *ctx);

#endif

