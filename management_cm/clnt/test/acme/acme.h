/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*This header was generated automaticaly from scheme, do change it*/

#ifndef NVMEIBC_MCS_STUB
#define NVMEIBC_MCS_STUB

/* single employee record */
struct employee {
    char first_name[16];
    char last_name[32];
    unsigned long long id;
    short salary;
} __attribute__((packed));

/* talbles of employees */
struct workers {
    int num_employees;
    unsigned long long employees_offset; /*offset to array. each elemen of type employee*/
} __attribute__((packed));

struct mcs_message {
    /* Golobal message header with fields commot to all messages */
    struct header {
        struct {
            unsigned short msg_len;
            unsigned short var_offset;
        } __attribute__((packed)) msg_len;
        unsigned int opcode;
        unsigned long long timestamp;
        unsigned int software_version;
        unsigned int reserved_future_use;
        char client_generated_uuid[40];
    } __attribute__((packed)) header;
    union {
        struct workers workers;
    } msg;
    char var_data[0];
} __attribute__((packed));

static const int MCS_WORKERS_MSG = 1;

#endif
