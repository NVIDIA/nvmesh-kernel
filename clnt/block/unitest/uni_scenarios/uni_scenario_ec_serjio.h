/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef UNI_SCENARIO_EC_SERJIO_H
#define UNI_SCENARIO_EC_SERJIO_H

/**
 * Serjio unitest
 */

#include "../bunitest.h"

/**
 * Run all JAM tests one after another. Return the accumulated rv.
 */
TEST_FUNC int unitest_serjio_AllTests(struct NVMeshSystem *sys);

#endif /*UNI_SCENARIO_EC_SERJIO_H*/
