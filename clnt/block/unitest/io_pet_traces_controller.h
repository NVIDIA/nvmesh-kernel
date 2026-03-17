/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once

struct nvmeib_pet_base_controller;
struct nvmeib_pet_base_controller* sim_get_io_pet_controller(void);

void sim_io_pet_controller_rotate(char const* test_id);

