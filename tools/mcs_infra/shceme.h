/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

struct message_type_1 {
    char gid[32];
    char disk[20];
} __attribute__((packed));
struct mcs_message {
    struct header {
        struct msg_len {
            unsigned short msg_len;
            unsigned short var_offset;
        } __attribute__((packed));
        unsigned int opcode;
        unsigned long long timestamp;
    } __attribute__((packed));
    union {
        struct message_type_1;
    };
    char var_data[0];
} __attribute__((packed));
static const int MSC_MESSAGE_TYPE_1_MSG = 1;
