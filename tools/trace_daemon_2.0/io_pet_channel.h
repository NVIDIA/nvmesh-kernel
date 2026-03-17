/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef IO_PET_CHANNEL_H
#define IO_PET_CHANNEL_H

#include "unlink_list.h"

#define IO_PET_PROC_PATH "/proc/nvmeib/io.pet/io.pet"
#define IO_PET_CHANNEL_NAME "nvmeibc_io_pet"
#define IO_PET_CPU_ID 0
//IO_PET_CPU_ID was introduced to reuse unlink_list.h functionality for io.pet channel

typedef struct io_pet_channel_meta
{
	char* dir;
	char* name;
	int max_logs;
	size_t max_file_size; /* Maximum file size before rotation */
} io_pet_channel_meta_t;

struct io_pet_channel;
typedef struct io_pet_channel io_pet_channel_t;

const io_pet_channel_meta_t* get_io_pet_ch_meta(io_pet_channel_t* self);
io_pet_channel_t* init_io_pet_channel(const char* dir, const char* name);
void destroy_io_pet_channel(io_pet_channel_t* self);
int start_io_pet_channel(io_pet_channel_t* self);
void join_io_pet_channel(io_pet_channel_t* self);
void reconf_io_pet_channel(io_pet_channel_t* self);
char* get_io_pet_channel_name(io_pet_channel_t* self);

#endif /* IO_PET_CHANNEL_H */

