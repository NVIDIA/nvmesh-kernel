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

/* Macros for filling JSON with a struct holding: buf, len, count, ntabs, jops */
#define CALL_JSON_START_OBJ(data, name)\
	((data)->count += (*(data)->jops->start_obj)((data)->buf + (data)->count, (data)->len - (data)->count, name, (data)->ntabs++))

#define CALL_JSON_END_OBJ(data, is_last)\
	((data)->count += (*(data)->jops->end_obj)((data)->buf + (data)->count, (data)->len - (data)->count, is_last, --(data)->ntabs))

#define CALL_JSON_START_ARRAY(data, name)\
	((data)->count += (*(data)->jops->start_array)((data)->buf + (data)->count, (data)->len - (data)->count, name, (data)->ntabs++))

#define CALL_JSON_END_ARRAY(data, is_last)\
	((data)->count += (*(data)->jops->end_array)((data)->buf + (data)->count, (data)->len - (data)->count, is_last, --(data)->ntabs))

#define CALL_JSON_DATA_UVAL(data, is_last, name, val)\
	((data)->count += (*(data)->jops->data_uval)((data)->buf + (data)->count, (data)->len - (data)->count, name, val, is_last, (data)->ntabs))

#define CALL_JSON_DATA_SVAL(data, is_last, name, val)\
	((data)->count += (*(data)->jops->data_sval)((data)->buf + (data)->count, (data)->len - (data)->count, name, val, is_last, (data)->ntabs))

#define CALL_JSON_DATA_STR(data, is_last, name, str)\
	((data)->count += (*(data)->jops->data_str)((data)->buf + (data)->count, (data)->len - (data)->count, name, str, is_last, (data)->ntabs))

#define CALL_JSON_DATA_UVAL_FLOAT(data, is_last, name, num, denom, precision)\
	((data)->count += (*(data)->jops->data_uval_float)((data)->buf + (data)->count, (data)->len - (data)->count, name, num, denom, precision, is_last, (data)->ntabs))

#endif /* _NVMEIB_JSON_H_ */ 
