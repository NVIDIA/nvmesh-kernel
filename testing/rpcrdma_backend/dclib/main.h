/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef MAIN_H_INCLUDED
#define MAIN_H_INCLUDED

#include "xkr_incs.h"
#include "xkr_version.h"
#include "xib_incs.h"

struct manager;
struct manager * rpcrdma_backend_create(void);
void rpcrdma_backend_free(struct manager *o);
struct manager * rpcrdma_backend_get_obj(void);

#endif

