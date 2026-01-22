/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef _NVMEIB_JSON_H_
#define _NVMEIB_JSON_H_

#include "kr_incs.h"
#include <linux/types.h>

#define JSON_LAST_ELEM 1

struct nvmeib_json_ops {
    ssize_t (*indent)(char *buf, size_t len, size_t indent);
    ssize_t (*start_obj)(char *buf, size_t len, const char *name, size_t indent);
    ssize_t (*end_obj)(char *buf, size_t len, int is_last, size_t indent);
    ssize_t (*start_array)(char *buf, size_t len, const char *name, size_t indent);
    ssize_t (*end_array)(char *buf, size_t len, int is_last, size_t indent);
    ssize_t (*data_str)(char *buf, size_t len, const char *name, const char *val, int is_last, size_t indent);
    ssize_t (*data_uval)(char *buf, size_t len, const char *name, u64 val, int is_last, size_t indent);
    /* Value printed is float of numerator / denominator with up to precision decimal points */
    ssize_t (*data_uval_float)(char *buf, size_t len, const char *name, const u64 numerator, const u64 denominator, const int precision, const int is_last, size_t indent);
    ssize_t (*data_sval)(char *buf, size_t len, const char *name, s64 val, int is_last, size_t indent);
    ssize_t (*right_padd)(char *buf, size_t len, ssize_t fixed);
    ssize_t (*data_bool)(char *buf, size_t len, const char *name, bool val, int is_last, size_t indent);
    ssize_t (*data_sprintf)(char *buf, size_t len, int is_last, size_t indent, const char *name, const char *fmt, ...);
};

extern const struct nvmeib_json_ops nvmeib_json_ops;

#endif /* _NVMEIB_JSON_H_ */ 
