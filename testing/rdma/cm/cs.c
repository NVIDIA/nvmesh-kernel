/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <netdb.h>
#include <byteswap.h>
#include <getopt.h>

#include <rdma/rdma_cma.h>
#include <rdma/rdma_verbs.h>
#include <infiniband/ib.h>

#define trace(fmt, ...) fprintf(stderr, "%lX:%s:%s[%d]" fmt, pthread_self(), __FILE__, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define FIN trace("-->\n")
#define FOUT trace("<--\n")
#define LINE trace("---\n")

//static char *server = "fe80:0000:0000:0000:0002:c903:00f1:7432";
static char *port = "7471";
static int message_size = 100;
static int message_count = 10;
static struct rdma_addrinfo hints;
static struct rdma_addrinfo	*rai;
static struct rdma_event_channel *channel;
static char *dst_addr;
static char *src_addr;

enum CQ_INDEX {
	SEND_CQ_INDEX,
	RECV_CQ_INDEX
};

struct node {
	struct rdma_cm_id *cma_id;
	int connected;
	struct ibv_pd *pd;
	struct ibv_cq *cq[2];
	struct ibv_mr *mr;
	void *mem;
} *node;

struct test {
} test;

static int alloc_node(void)
{
	int ret;

	node = calloc(1, sizeof *node);
	if (!node) {
		printf("unable to allocate memory for test node\n");
		return -ENOMEM;
	}

	if (dst_addr) {
		ret = rdma_create_id(channel, &node->cma_id, node, hints.ai_port_space);
		if (ret)
			goto err;
	}
	return 0;
	
err:
	rdma_destroy_id(node->cma_id);
	free(node);
	return ret;
}

static void destroy_node(struct node *node)
{
	if (!node->cma_id)
		return;

	if (node->cma_id->qp)
		rdma_destroy_qp(node->cma_id);

	if (node->cq[SEND_CQ_INDEX])
		ibv_destroy_cq(node->cq[SEND_CQ_INDEX]);

	if (node->cq[RECV_CQ_INDEX])
		ibv_destroy_cq(node->cq[RECV_CQ_INDEX]);

	if (node->mem) {
		ibv_dereg_mr(node->mr);
		free(node->mem);
	}

	if (node->pd)
		ibv_dealloc_pd(node->pd);

	/* Destroy the RDMA ID after all device resources */
	rdma_destroy_id(node->cma_id);
	free(node);
}

static int get_rdma_addr(char *src, char *dst, char *port, struct rdma_addrinfo *hints, struct rdma_addrinfo **rai)
{
	struct rdma_addrinfo rai_hints, *res;
	int ret;

	if (hints->ai_flags & RAI_PASSIVE)
		return rdma_getaddrinfo(src, port, hints, rai);

	rai_hints = *hints;
	if (src) {
		rai_hints.ai_flags |= RAI_PASSIVE;
		ret = rdma_getaddrinfo(src, NULL, &rai_hints, &res);
		if (ret)
			return ret;

		rai_hints.ai_src_addr = res->ai_src_addr;
		rai_hints.ai_src_len = res->ai_src_len;
		rai_hints.ai_flags &= ~RAI_PASSIVE;
	}

	ret = rdma_getaddrinfo(dst, port, &rai_hints, rai);
	if (src)
		rdma_freeaddrinfo(res);
	return ret;
}

static int addr_handler(struct node *node)
{
	int ret;

	ret = rdma_resolve_route(node->cma_id, 2000);
	if (ret)
		perror("resolve route failed");
	return ret;
}

static int create_message(struct node *node)
{
	if (!message_size)
		message_count = 0;

	if (!message_count)
		return 0;

	node->mem = malloc(message_size);
	if (!node->mem) {
		printf("failed message allocation\n");
		return -1;
	}
	node->mr = ibv_reg_mr(node->pd, node->mem, message_size, IBV_ACCESS_LOCAL_WRITE);
	if (!node->mr) {
		printf("failed to reg MR\n");
		goto err;
	}
	return 0;
	
err:
	free(node->mem);
	return -1;
}

static int init_node(struct node *node)
{
	struct ibv_qp_init_attr init_qp_attr;
	int cqe, ret;

	node->pd = ibv_alloc_pd(node->cma_id->verbs);
	if (!node->pd) {
		ret = -ENOMEM;
		printf("unable to allocate PD\n");
		goto out;
	}

	cqe = message_count ? message_count : 1;
	node->cq[SEND_CQ_INDEX] = ibv_create_cq(node->cma_id->verbs, cqe, node, 0, 0);
	node->cq[RECV_CQ_INDEX] = ibv_create_cq(node->cma_id->verbs, cqe, node, 0, 0);
	if (!node->cq[SEND_CQ_INDEX] || !node->cq[RECV_CQ_INDEX]) {
		ret = -ENOMEM;
		printf("unable to create CQ\n");
		goto out;
	}

	memset(&init_qp_attr, 0, sizeof init_qp_attr);
	init_qp_attr.cap.max_send_wr = cqe;
	init_qp_attr.cap.max_recv_wr = cqe;
	init_qp_attr.cap.max_send_sge = 1;
	init_qp_attr.cap.max_recv_sge = 1;
	init_qp_attr.qp_context = node;
	init_qp_attr.sq_sig_all = 1;
	init_qp_attr.qp_type = IBV_QPT_RC;
	init_qp_attr.send_cq = node->cq[SEND_CQ_INDEX];
	init_qp_attr.recv_cq = node->cq[RECV_CQ_INDEX];
	ret = rdma_create_qp(node->cma_id, node->pd, &init_qp_attr);
	if (ret) {
		perror("unable to create QP");
		goto out;
	}

	ret = create_message(node);
	if (ret) {
		printf("failed to create messages: %d\n", ret);
		goto out;
	}
	
out:
	return ret;
}

static int post_recvs(struct node *node)
{
	struct ibv_recv_wr recv_wr, *recv_failure;
	struct ibv_sge sge;
	int i, ret = 0;

	if (!message_count)
		return 0;

	recv_wr.next = NULL;
	recv_wr.sg_list = &sge;
	recv_wr.num_sge = 1;
	recv_wr.wr_id = (uintptr_t)node;

	sge.length = message_size;
	sge.lkey = node->mr->lkey;
	sge.addr = (uintptr_t)node->mem;

	for (i = 0; i < message_count && !ret; i++) {
		ret = ibv_post_recv(node->cma_id->qp, &recv_wr, &recv_failure);
		if (ret) {
			printf("failed to post receives: %d\n", ret);
			break;
		}
	}
	return ret;
}

static int post_sends(struct node *node)
{
	struct ibv_send_wr send_wr, *bad_send_wr;
	struct ibv_sge sge;
	int i, ret = 0;

	if (!node->connected || !message_count)
		return 0;

	send_wr.next = NULL;
	send_wr.sg_list = &sge;
	send_wr.num_sge = 1;
	send_wr.opcode = IBV_WR_SEND;
	send_wr.send_flags = 0;
	send_wr.wr_id = (unsigned long)node;

	sge.length = message_size;
	sge.lkey = node->mr->lkey;
	sge.addr = (uintptr_t) node->mem;

	for (i = 0; i < message_count && !ret; i++) {
		ret = ibv_post_send(node->cma_id->qp, &send_wr, &bad_send_wr);
		if (ret) 
			printf("failed to post sends: %d\n", ret);
	}
	return ret;
}

static int poll_cqs(enum CQ_INDEX index)
{
	struct ibv_wc wc[8];
	int done, ret;
	if (node->connected)
		for (done = 0; done < message_count; done += ret) {
			ret = ibv_poll_cq(node->cq[index], 8, wc);
			if (ret < 0) {
				printf("failed polling CQ: %d\n", ret);
				return ret;
			}
		}
	return 0;
}

static int route_handler(struct node *node)
{
	struct rdma_conn_param conn_param;
	int ret;

	ret = init_node(node);
	if (ret)
		goto err;

	ret = post_recvs(node);
	if (ret)
		goto err;

	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 5;
	conn_param.private_data = rai->ai_connect;
	conn_param.private_data_len = rai->ai_connect_len;
	ret = rdma_connect(node->cma_id, &conn_param);
	if (ret) {
		perror("failure connecting");
		goto err;
	}
	return 0;
	
err:
	return ret;
}

static int connect_handler(struct rdma_cm_id *cma_id)
{
	int ret;

	node->cma_id = cma_id;
	cma_id->context = node;
	ret = init_node(node);
	if (ret)
		goto err2;

	ret = post_recvs(node);
	if (ret)
		goto err2;

	ret = rdma_accept(node->cma_id, NULL);
	if (ret) {
		perror("failure accepting");
		goto err2;
	}
	return 0;

err2:
	node->cma_id = NULL;

err1:
	printf("failing connection request\n");
	rdma_reject(cma_id, NULL, 0);
	return ret;
}

static int cma_handler(struct rdma_cm_id *cma_id, struct rdma_cm_event *event)
{
	int ret = 0;

	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		ret = addr_handler(cma_id->context);
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		ret = route_handler(cma_id->context);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		ret = connect_handler(cma_id);
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		((struct node *)cma_id->context)->connected = 1;
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		printf("event: %s, error: %d\n", rdma_event_str(event->event), event->status);
		ret = event->status;
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		rdma_disconnect(cma_id);
		((struct node *)cma_id->context)->connected = 0;
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		/* Cleanup will occur after test completes. */
		break;
	default:
		break;
	}
	return ret;
}

static int connect_events(void)
{
	struct rdma_cm_event *event;
	int ret = 0;

	while (!node->connected && !ret) {
		ret = rdma_get_cm_event(channel, &event);
		if (!ret) {
			ret = cma_handler(event->id, event);
			rdma_ack_cm_event(event);
		} else {
			perror("failure in rdma_get_cm_event in connect events");
			ret = errno;
		}
	}
	return ret;
}

static int disconnect_events(void)
{
	struct rdma_cm_event *event;
	int ret = 0;

	while (node->connected && !ret) {
		ret = rdma_get_cm_event(channel, &event);
		if (!ret) {
			ret = cma_handler(event->id, event);
			rdma_ack_cm_event(event);
		} else {
			perror("failure in rdma_get_cm_event in disconnect events");
			ret = errno;
		}
	}
	return ret;
}

static int run_server(void)
{
	struct rdma_cm_id *listen_id;
	int ret;

	printf("starting server\n");
	ret = rdma_create_id(channel, &listen_id, &test, hints.ai_port_space);
	if (ret) {
		perror("listen request failed");
		return ret;
	}

	ret = get_rdma_addr(src_addr, dst_addr, port, &hints, &rai);
	if (ret) {
		perror("getrdmaaddr error");
		goto out;
	}
	ret = rdma_bind_addr(listen_id, rai->ai_src_addr);
	if (ret) {
		perror("bind address failed");
		goto out;
	}
	ret = rdma_listen(listen_id, 0);
	if (ret) {
		perror("failure trying to listen");
		goto out;
	}
	ret = connect_events();
	if (ret)
		goto out;
	if (message_count) {
		printf("initiating data transfers\n");
		ret = post_sends(node);
		if (ret)
			goto out;
		printf("completing sends\n");
		ret = poll_cqs(SEND_CQ_INDEX);
		if (ret)
			goto out;

		printf("receiving data transfers\n");
		ret = poll_cqs(RECV_CQ_INDEX);
		if (ret)
			goto out;
		printf("data transfers complete\n");

	}

	printf("disconnecting\n");
	if (node->connected) {
		node->connected = 0;
		rdma_disconnect(node->cma_id);
	}

	ret = disconnect_events();

 	printf("disconnected\n");

out:
	rdma_destroy_id(listen_id);
	return ret;
}

static int run_client(void)
{
	int i, ret, ret2;

	printf("starting client\n");

	ret = get_rdma_addr(src_addr, dst_addr, port, &hints, &rai);
	if (ret) {
		perror("getaddrinfo error");
		return ret;
	}

	printf("connecting\n");
	ret = rdma_resolve_addr(node->cma_id, rai->ai_src_addr, rai->ai_dst_addr, 2000);
	if (ret) {
		perror("failure getting addr");
		return ret;
	}

	ret = connect_events();
	if (ret)
		goto disc;

	if (message_count) {
		printf("receiving data transfers\n");
		ret = poll_cqs(RECV_CQ_INDEX);
		if (ret)
			goto disc;

		printf("sending replies\n");
		ret = post_sends(node);
		if (ret)
			goto disc;

		printf("data transfers complete\n");
	}

disc:
	ret2 = disconnect_events();
	if (ret2)
		ret = ret2;
out:
	return ret;
}

int main(int argc, char **argv)
{
	int op, ret;

	while ((op = getopt(argc, argv, "s:b:p")) != -1) {
		switch (op) {
		case 's':
			dst_addr = optarg;
			break;
		case 'b':
			src_addr = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		default:
			printf("usage: %s\n", argv[0]);
			printf("\t[-s server_address]\n");
			printf("\t[-b bind_address]\n");
			printf("\t[-p port_number]\n");
			exit(1);
		}
	}
	
	//hints.ai_port_space = RDMA_PS_IB;
	//hints.ai_flags = RAI_NUMERICHOST | RAI_FAMILY;
	//hints.ai_family = AF_IB;
	hints.ai_port_space = RDMA_PS_TCP;
	//hints.ai_flags = RAI_NUMERICHOST;
	channel = rdma_create_event_channel();
	if (!channel) {
		printf("failed to create event channel\n");
		exit(1);
	}

	if (alloc_node())
		exit(1);

	if (dst_addr) {
		ret = run_client();
	} else {
		hints.ai_flags |= RAI_PASSIVE;
		ret = run_server();
	}

	printf("test complete\n");
	destroy_node(node);
	rdma_destroy_event_channel(channel);
	if (rai)
		rdma_freeaddrinfo(rai);

	printf("return status %d\n", ret);
	return ret;
}

