/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_UNDEF_H
#define KR_UNDEF_H

/* always call the function and possible wrapper functions */
#ifdef schedule_work
#	undef schedule_work
#endif

#ifdef schedule_delayed_work
#	undef schedule_delayed_work
#endif

#include "compat/kr_incs_module.h"

#endif
