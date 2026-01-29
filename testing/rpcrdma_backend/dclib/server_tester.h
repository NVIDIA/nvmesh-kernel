/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef SERVER_TESTER_H
#define SERVER_TESTER_H

struct server_tester;
struct manager;
struct server_tester * server_tester_create(struct manager* o);
void server_tester_free(struct server_tester* st);

#endif
