/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_TRACE_STRESS_TEST_H
#define NVMEIB_TRACE_STRESS_TEST_H

#include "kr_incs.h"

struct nvmeib_trace_stress_prog {
	struct list_head link;
	void (*action)(struct nvmeib_trace_stress_prog *prog);
	void (*dtor)(struct nvmeib_trace_stress_prog *prog);
	void (*print)(struct nvmeib_trace_stress_prog *prog);
	struct nvmeib_trace_stress_prog *(*copy)(
	    struct nvmeib_trace_stress_prog *prog);
};

struct nvmeib_trace_stress_prog *
nvmeib_trace_stress_cpu_and_prog_from_string(const char *str, size_t len,
                                             size_t *off, size_t *cpu_start,
                                             size_t *cpu_end);

#endif /* NVMEIB_TRACE_STRESS_TEST_H */