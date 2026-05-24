/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef IO_PET_TRACES_CONTROLLER_H
#define IO_PET_TRACES_CONTROLLER_H

#include <stdbool.h>
#include <stddef.h>

struct nvmeib_pet_base_controller;
struct nvmeib_pet_base_controller* sim_get_io_pet_controller(void);

void sim_io_pet_controller_rotate(char const* test_id);
void sim_io_pet_controller_set_buffer_size(size_t buffer_size);
size_t sim_io_pet_controller_get_buffer_size(void);
void sim_io_pet_controller_set_verbose(bool verbose);
bool sim_io_pet_controller_get_verbose(void);

#endif /* IO_PET_TRACES_CONTROLLER_H */
