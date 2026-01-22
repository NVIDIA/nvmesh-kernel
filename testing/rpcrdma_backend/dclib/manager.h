/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef MANAGER_H_INCLUDED
#define MANAGER_H_INCLUDED

#include <linux/dma-direction.h>
#include "requests.h"

#define CONNECT_PAYLOAD_SIZE 128

struct manager;
struct manager_init_params {

};

struct manager * manager_create(struct manager_init_params *p);
void manager_free(struct manager *o);
struct rdma_dev;
int manager_rdma_dev_exit(struct manager *o, struct rdma_dev *dev);

struct per_core_rdma;
int manager_pcpu_add_dev(
	struct manager *o, int cpu, struct per_core_rdma *d, bool preferred);
int manager_pcpu_rem_dev(
	struct manager *o, int cpu, struct per_core_rdma *d);
void * manager_link_new_path(struct manager *o, void *path);
void manager_unlink_path(struct manager *o, void *path);
void manager_free_path(struct manager *o, void *path);

struct sockaddr_storage;
struct local_address_info {
	struct sockaddr_storage *a;
	/* the local rdma device */
	void *local_rdma_dev;
};

/* register a callback on a new local address - port becomes active */
int manager_register_new_address(struct manager *o,
	void (*f)(void *ctx, struct local_address_info *a), void *ctx);
int manager_unregister_new_address(struct manager *o, void *ctx);

struct client_ft {
	void (*connect)(void *ctx, void *path, void *payload, int payload_len);
	void (*disconnect)(void *ctx, void *path);
};

enum listen_status {
	l_ok = 0,
	l_no_such_addr,
	l_not_ready,
	l_already_exist,
	l_start_fail,
};

struct client_info;
struct server_ft {
	void (*listen)(void *ctx, struct sockaddr_storage *s, int status);
	struct client_info * (*accept)(void *ctx, void *path,
				   struct sockaddr_storage *s,
				   struct sockaddr_storage *d,
				   void *spayload, int spayload_len,
				   void *dpayload, int dpayload_len);
};

struct server_info {
	void *server;
	struct server_ft ft;
};

int manager_register_server(struct manager *o, struct server_info *server,
	void *local_rdma_dev, struct sockaddr_storage *src);
int manager_unregister_server(struct manager *o, void *server,
	void *local_rdma_dev, struct sockaddr_storage *src);

struct client_info {
	void *client;
	struct client_ft ft;
	char payload[CONNECT_PAYLOAD_SIZE];
};

int manager_connect(struct manager *o, struct client_info *client,
	void *local_rdma_dev, struct sockaddr_storage *src,
	struct sockaddr_storage *dst);
int manager_disconnect(struct manager *o, void *local_rdma_dev,
	void *client, void *path);

struct dst_paths {
	void *path;
	void *next_paths;
};

union service_id {
	u8 raw[16];
	struct {
		__be64 sid;
		__be32 index;
		__be32 dct;
	} global;
};

struct ib_wc;
typedef void (*recv_comp_t)(void *ctx, struct ib_wc *wc, void *msg,
							void *rsc, int index, union service_id *sid);
struct server_service {
	recv_comp_t recv_comp;
	void *ctx;
};

enum dc_wr_opcode {
	__IB_WR_SEND = 1,
	__IB_WR_SEND_WITH_IMM = (1 << 1),
	__IB_WR_RDMA_WRITE = (1 << 2),
	__IB_WR_RDMA_WRITE_WITH_IMM = (1 << 3),
	__IB_WR_RDMA_READ = (1 << 4),
	__IB_WR_ATOMIC_CMP_AND_SWP = (1 << 5),
	__IB_WR_ATOMIC_FETCH_AND_ADD = (1 << 6),
	__IB_WR_MASKED_ATOMIC_CMP_AND_SWP = (1 << 7),
	__IB_WR_MASKED_ATOMIC_FETCH_AND_ADD = (1 << 8),
	__IB_WR_REG_MR = (1 << 9),
	__IB_WR_LOCAL_INV = (1 << 10),
	__IB_WR_RDMA_READ_WITH_INV = (1 << 11),
};

struct post_send_info {
	union service_id *src;
	union service_id *dst;
	struct scatterlist *sg;
	int nents;
	struct dst_paths *paths;
	enum dc_wr_opcode opcode;
	u64 raddr;
	u32 rkey;
	int (*prepare_send)(void *ctx, void *buffer, int buf_size, int n_rdma,
						int *size);
	void (*send_comp)(void *ctx, struct ib_wc *wc);
	void *ctx;
	void *rsc;
};

int manager_post_send(struct post_send_info *ps);

struct server_service;
int manager_register_service(struct manager *o,
	struct server_service *service, union service_id *sid);
int manager_unregister_service(struct manager *o, union service_id *sid);

struct rdma_service;
struct manager_per_cpu {
	spinlock_t guard;
	struct rdma_service *srvs;
	int len;
};
struct manager_per_cpu * manager_get_services_cpu(
	struct manager *o, int cpu);

struct memory_reg {
	u64 addr;
	u32 rkey;
	u32 lkey;
};
struct memory_reg * manager_reg_mem(struct manager *o, void *local_rdma_dev,
	void *addr, unsigned len);
int manager_ureg_mem(struct memory_reg *mem);
void manager_sync_mem_for_cpu(
	struct memory_reg *mem, enum dma_data_direction dir);
void manager_sync_mem_for_dev(
	struct memory_reg *mem, enum dma_data_direction dir);

#endif

