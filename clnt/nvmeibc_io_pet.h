/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_IO_PET_H_INCLUDED
#define NVMEIBC_IO_PET_H_INCLUDED

#include "common/pet/nvmeib_pet_specification.h"

extern struct nvmeib_pet_message_description const __start_nvmeib_pet_messages[];
extern struct nvmeib_pet_message_description const __stop_nvmeib_pet_messages[];

#define NVMEIBC_IO_PET_MSG(pet_journal, msg, severity, ...) NVMEIB_IO_PET_MSG(pet_journal, msg, severity, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_NORM(pet_journal, msg, ...) NVMEIB_IO_PET_MSG_NORM(pet_journal, msg, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_WARN(pet_journal, msg, ...) NVMEIB_IO_PET_MSG_WARN(pet_journal, msg, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_ERROR(pet_journal, msg, ...) NVMEIB_IO_PET_MSG_ERROR(pet_journal, msg, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_CRIT(pet_journal, msg, ...) NVMEIB_IO_PET_MSG_CRIT(pet_journal, msg, __VA_ARGS__)


struct nvmeib_pet_base_controller* nvmeibc_io_pet_controller_create(void);

__attribute__((nonnull (1)))
void nvmeibc_io_pet_controller_free(struct nvmeib_pet_base_controller* self);

struct nvmeib_pet_journal nvmeibc_io_pet_journal_make(struct nvmeib_pet_base_controller*);

#endif //NVMEIBC_IO_PET_H_INCLUDED
