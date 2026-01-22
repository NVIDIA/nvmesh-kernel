/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef MMAP_MANAGER_H
#define MMAP_MANAGER_H

struct mmap_manager;
typedef struct mmap_manager mmap_manager_t;

mmap_manager_t* init_mmap_manager(const char* proc_name);
void destroy_mmap_manager(mmap_manager_t* self);

void *get_buf_addr(mmap_manager_t* self, unsigned long buf_idx);

int get_control_proc(mmap_manager_t* self);

unsigned long get_trace_buf_size(mmap_manager_t* self);

#endif /* MMAP_MANAGER_H */
