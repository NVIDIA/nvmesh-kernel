/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CLIENT_TESTER_H
#define CLIENT_TESTER_H

struct client_tester;
struct manager;
struct sockaddr_storage;
struct client_tester * client_tester_create(struct manager* o,
	struct sockaddr_storage *dst);
void client_tester_free(struct client_tester* ct);

#endif

