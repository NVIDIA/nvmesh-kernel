/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef UNI_SCENARIO_GF_H
#define UNI_SCENARIO_GF_H
/* Sets of unitest scenarios for GF math testing */
#include "../bunitest.h"
#include "nvmesh_sim.h"

TEST_FUNC int gf_simple(void);
TEST_FUNC int gf_extensive(void);
TEST_FUNC int gf_opt_check(void);
TEST_FUNC int gf_perf(void);

#endif

