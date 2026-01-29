/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __FLOG_H__
#define __FLOG_H__

#include <linux/kernel.h>

#ifdef UM_APP
#define nflog(name, fmt, ...) ({ NVMEIB_LOG_GOODPATH(fmt, _DBG, tracer_nvmeshum_dp, name, ##__VA_ARGS__); })
#else
#define nflog(name, fmt, ...) ({ NVMEIB_LOG_GOODPATH(fmt, _DBG, /*Deafult*/, name, ##__VA_ARGS__); })
#endif

#endif /* __FLOG_H__ */
