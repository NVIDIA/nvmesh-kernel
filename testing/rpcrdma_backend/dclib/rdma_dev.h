/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef RDMA_DEV_H_INCLUDED
#define RDMA_DEV_H_INCLUDED

#include "xkr_incs.h"
#include "xib_incs.h"
#include "utils.h"

#define NVMESH_SERVICE_ID 6988676976697999ULL
#define NVMESH_SERVICE_ID_MASK (~RDMA_IB_IP_PS_MASK)
#define NVMESH_PKEY 0xffff

struct manager;
struct ib_device;
struct rdma_dev_info {
	struct manager *owner;
	struct ib_device *ib_dev;
	int max_receive_msg_size;
};

struct rdma_dev {
	struct ib_device *ib_dev;
	struct completion *remove_done;
	void *priv;
	struct list_head link;
};

struct per_core_rdma {
	spinlock_t guard;
	struct list_head link;
};

struct rdma_service {
	struct server_service service;
	struct x_ref in_use;
	u64 service_id;
};

struct rdma_dev * rdma_dev_create(struct rdma_dev_info *p);
void rdma_dev_stop(struct rdma_dev *d);
void rdma_dev_free(struct rdma_dev *d);
bool rdma_dev_push_request(struct rdma_dev *d, struct request_base *r);
void rdma_dev_per_core_rdma_free(struct per_core_rdma *rdma);

struct sockaddr_storage;
struct server_info;
int rdma_dev_register_server(struct rdma_dev *d,
	struct server_info *server, struct sockaddr_storage *src);
int rdma_dev_unregister_server(struct rdma_dev *d,
	void *server, struct sockaddr_storage *src);
struct client_info;
int rdma_dev_connect(struct rdma_dev *d, struct client_info *client,
	struct sockaddr_storage *src, struct sockaddr_storage *dst);
int rdma_dev_disconnect(struct rdma_dev *d, void *client, void *path);

int rdma_dev_get_src_addr(void *path, struct sockaddr_storage *a); 
int rdma_dev_get_dst_addr(void *path, struct sockaddr_storage *a);
struct _path;
bool rdma_dev_is_path_port(struct _path *path, struct per_core_rdma *d);
struct local_address_info;
int rdma_dev_register_address(struct rdma_dev *d,
	void (*f)(void *ctx, struct local_address_info *a), void *ctx);
struct post_send_info;
int rdma_dev_post_send(struct post_send_info *ps, void *path,
	struct per_core_rdma *d, int *index_remote_dct);
int rdma_dev_put_send_resource(void *rsc);
int rdma_dev_put_recv_resource(void *rsc, int index);
int rdma_dev_unregister_client(void *client, void *path);
struct memory_reg;
struct memory_reg * rdma_dev_reg_mem(
	struct rdma_dev *d, void *addr, unsigned len);
int rdma_dev_unreg_mem(struct memory_reg *mem);
void rdma_dev_sync_mem_for_cpu(
	struct memory_reg *mem, enum dma_data_direction dir);
void rdma_dev_sync_mem_for_dev(
	struct memory_reg *mem, enum dma_data_direction dir);

#endif
