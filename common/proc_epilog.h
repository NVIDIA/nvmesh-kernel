/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef _PROC_EPILOG
#define _PROC_EPILOG
// Todo: Unify with nvmeib_add_proc_status_footer, remove the .h and .c file
ssize_t nvmeib_proc_add_json_proc_epilog(int version, char *buf, size_t buf_len);
ssize_t nvmeib_proc_add_txt_proc_epilog( int version, char *buf, size_t buf_len);
ssize_t nvmeib_proc_add_yaml_proc_epilog(int version, char *buf, size_t buf_len);
ssize_t nvmeib_proc_add_smart_proc_epilog(int version, char *buf, size_t buf_len);
struct jdr;
void nvmeib_proc_add_jdr_proc_epilog(int version, struct jdr *jdr);
// serializing to json using jdr
void nvmeib_proc_add_json_proc_epilog_jdr(int version, struct jdr *jdr);
// serializing to txt using txt
struct nvmeib_txt;
void nvmeib_proc_add_txt_proc_epilog_txt( int version, struct nvmeib_txt *txt);

#endif //_PROC_EPILOG
