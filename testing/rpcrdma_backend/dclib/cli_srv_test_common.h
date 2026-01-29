/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CLI_SRV_TEST_COMMON_H_INCLUDED
#define CLI_SRV_TEST_COMMON_H_INCLUDED

#include "manager.h"

enum cli_srv_test {
	cst_type_dummy = 1,
	cst_type_client_req1,
	cst_type_server_rep1,
	cst_type_client_req2,
	cst_type_server_rep2,
};

struct cli_srv_payload {
	enum cli_srv_test type;
	union service_id sid;
};

struct cli_srv_tester_header {
	enum cli_srv_test type;
};

struct cli_srv_tester_req1_msg {
	struct cli_srv_tester_header hdr;
	char buf[64];
};

struct ser_cli_tester_rep1_msg {
	struct cli_srv_tester_header hdr;
	char buf[64];
	__be64 raddr;
	__be32 rkey;
};

struct cli_srv_tester_req2_msg {
	struct cli_srv_tester_header hdr;
	char buf[64];
	__be32 table_size;
};

struct ser_cli_tester_rep2_msg {
	struct cli_srv_tester_header hdr;
	char buf[64];
};

struct local_address {
	struct sockaddr_storage a;
	void *local_rdma_dev;
	struct list_head link;
};

struct add_local_addr_request {
	struct request_base r;
	struct local_address_info a;
};

static inline void add_local_addr_request_free(struct request_base *r)
{
	kfree(container_of(r, struct add_local_addr_request, r));
}

#endif
