/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/ib_incs.h"

int ib_register_client(struct ib_client *client){
	struct ib_device *ibd = kzalloc(sizeof(*ibd), GFP_KERNEL);
	sprintf(ibd->name,"mlx_%s", client->name);
	ibd->node_type = 17;
	client->dev = ibd;
	client->add(ibd);
	return 0;
}

void ib_unregister_client(struct ib_client *client) {
	struct ib_device *ibd = client->dev;
	client->remove(ibd);
	kfree(ibd);
}

void ib_set_client_data(struct ib_device *device, struct ib_client *client, void *data) {
	WARN_ON(client->dev != device);
	client->data = data;
}

void *ib_get_client_data(struct ib_device *device, struct ib_client *client){
	WARN_ON(client->dev != device);
	return client->data;
}

enum rdma_link_layer rdma_port_get_link_layer(struct ib_device *device, u8 port_num){
	(void)device;
	(void)port_num;
	return IB_LINK_LAYER_INFINIBAND;
}

__attribute_const__ enum rdma_transport_type rdma_node_get_transport(enum rdma_node_type node_type) {
	if (node_type == RDMA_NODE_USNIC) return RDMA_TRANSPORT_USNIC;
	if (node_type == RDMA_NODE_USNIC_UDP) return RDMA_TRANSPORT_USNIC_UDP;
	if (node_type == RDMA_NODE_RNIC) return RDMA_TRANSPORT_IWARP;
	return RDMA_TRANSPORT_IB;
}
