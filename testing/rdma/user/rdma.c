/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <netdb.h>

#include <infiniband/verbs.h>

// if x is NON-ZERO, error is printed
#define TEST_NZ(x, y) do { if ((x)) die(y); } while (0)

// if x is ZERO, error is printed
#define TEST_Z(x, y) do { if (!(x)) die(y); } while (0)

// if x is NEGATIVE, error is printed
#define TEST_N(x, y) do { if ((x) < 0) die(y); } while (0)

static int page_size;
static int sl = 1;
static pid_t pid;

#define WR_LIST_SIZE        3
#define SINGLE_WRITE_SIZE   256

#define RUN_TIME 5000000ULL

struct ib_connection {
    int             	lid;
    int            	 	qpn;
    int             	psn;
	unsigned 			rkey;
	unsigned long long 	vaddr;
};

struct app_data {
	int							port;
	int							ib_port;
	unsigned            		size;
	int                 		tx_depth;
	int 		    			sockfd;
	char						*servername;
	struct ib_connection		local_connection;
	struct ib_connection 		*remote_connection;
	struct ibv_device			*ib_dev;
};

struct app_context{
	struct ibv_context 		*context;
	struct ibv_pd      		*pd;
	struct ibv_mr      		*mr;
	struct ibv_cq      		*rcq;
	struct ibv_cq      		*scq;
	struct ibv_qp      		*qp;
	struct ibv_comp_channel *ch;
	void               		*buf;
	unsigned            	size;
	int                 	tx_depth;
	struct ibv_sge      	sge_list;
	struct ibv_send_wr  	wr[WR_LIST_SIZE];
};

/*
 *  kill app
 * ********************
 *	Kill client or server app 
 */
static int die(const char *reason) {
	fprintf(stderr, "Err: %s - %s\n ", strerror(errno), reason);
	exit(EXIT_FAILURE);
	return -1;
}

/*
 *  tcp_client_connect
 * ********************
 *	Creates a connection to a TCP server 
 */
static int tcp_client_connect(struct app_data *data) {
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family		= AF_UNSPEC,
		.ai_socktype	= SOCK_STREAM
	};

	char *service;
	int n;
	int sockfd = -1;
	struct sockaddr_in sin;

	TEST_N(asprintf(&service, "%d", data->port), "Error writing port-number to port-string");
	TEST_N(getaddrinfo(data->servername, service, &hints, &res), "getaddrinfo threw error");
	for (t = res; t; t = t->ai_next) {
		TEST_N(sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol), "Could not create client socket");
		TEST_N(connect(sockfd,t->ai_addr, t->ai_addrlen), "Could not connect to server");	
	}
	freeaddrinfo(res);
	return sockfd;
}

/*
 *  tcp_server_listen
 * *******************
 *  Creates a TCP server socket  which listens for incoming connections 
 */
static int tcp_server_listen(struct app_data *data) {
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags		= AI_PASSIVE,
		.ai_family		= AF_UNSPEC,
		.ai_socktype	= SOCK_STREAM	
	};

	char *service;
	int sockfd = -1;
	int n, connfd;
	struct sockaddr_in sin;

	TEST_N(asprintf(&service, "%d", data->port), "Error writing port-number to port-string");
    TEST_N(n = getaddrinfo(NULL, service, &hints, &res), "getaddrinfo threw error");
	TEST_N(sockfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol), "Could not create server socket");
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);
    TEST_N(bind(sockfd,res->ai_addr, res->ai_addrlen), "Could not bind addr to socket"); 	
	listen(sockfd, 1);
	TEST_N(connfd = accept(sockfd, NULL, 0), "server accept failed");
	freeaddrinfo(res);
	close(sockfd);
	return connfd;
}

/*
 *  tcp_exch_ib_connection_info
 * *****************************
 *  Client an server exchanges info 
 */
static int tcp_exch_ib_connection_info(struct app_data *data) {

	char msg[sizeof "0000:000000:000000:00000000:0000000000000000"];
	int parsed;

	struct ib_connection *local = &data->local_connection;

	sprintf(msg, "%04x:%06x:%06x:%08x:%016Lx", local->lid, local->qpn, local->psn, local->rkey, local->vaddr);

	if (write(data->sockfd, msg, sizeof msg) != sizeof msg) {
		perror("Could not send connection_details to peer");
		return -1;
	}	

	if (read(data->sockfd, msg, sizeof msg) != sizeof msg) {
		perror("Could not receive connection_details to peer");
		return -1;
	}

	free(data->remote_connection);
	TEST_Z(data->remote_connection = malloc(sizeof(struct ib_connection)), "Could not allocate memory for remote_connection connection");

	struct ib_connection *remote = data->remote_connection;

	parsed = sscanf(msg, "%x:%x:%x:%x:%Lx", &remote->lid, &remote->qpn, &remote->psn, &remote->rkey, &remote->vaddr);
	
	if (parsed != 5) {
		fprintf(stderr, "Could not parse message from peer");
		free(data->remote_connection);
		return -1;
	}
	
	return 0;
}

/*
 *  qp_change_state_init
 * **********************
 *	Changes Queue Pair status to INIT
 */
static int qp_change_state_init(struct ibv_qp *qp, struct app_data *data) {
	
	struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state        	= IBV_QPS_INIT;
    attr->pkey_index      	= 0;
    attr->port_num        	= data->ib_port;
    attr->qp_access_flags	= IBV_ACCESS_REMOTE_WRITE;

    TEST_NZ(ibv_modify_qp(qp, attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS), "Could not modify QP to INIT, ibv_modify_qp");

	return 0;
}

/*
 * 	init_ctx
 * **********
 *	This method initializes the Infiniband Context
 *  It creates structures for:
 *  ProtectionDomain, MemoryRegion, CompletionChannel, Completion Queues, Queue Pair
 */
static struct app_context *init_ctx(struct app_data *data)
{
	struct app_context *ctx;
	struct ibv_device *ib_dev;

	ctx = malloc(sizeof *ctx);
	memset(ctx, 0, sizeof *ctx);
	
	ctx->size = data->size;
	ctx->tx_depth = data->tx_depth;
	
	TEST_NZ(posix_memalign(&ctx->buf, page_size, ctx->size * 2), "could not allocate working buffer ctx->buf");

	memset(ctx->buf, 0, ctx->size * 2);

	struct ibv_device **dev_list;
	TEST_Z(dev_list = ibv_get_device_list(NULL), "No IB-device available. get_device_list returned NULL");
    TEST_Z(data->ib_dev = dev_list[0], "IB-device could not be assigned. Maybe dev_list array is empty");
	TEST_Z(ctx->context = ibv_open_device(data->ib_dev), "Could not create context, ibv_open_device");
	TEST_Z(ctx->pd = ibv_alloc_pd(ctx->context), "Could not allocate protection domain, ibv_alloc_pd");

	/* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
     * The Consumer is not allowed to assign Remote Write or Remote Atomic to
     * a Memory Region that has not been assigned Local Write. 
	 */

	TEST_Z(ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size * 2, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE), "Could not allocate mr, ibv_reg_mr. Do you have root access?");
	TEST_Z(ctx->ch = ibv_create_comp_channel(ctx->context), "Could not create completion channel, ibv_create_comp_channel");
	TEST_Z(ctx->rcq = ibv_create_cq(ctx->context, 1, NULL, NULL, 0), "Could not create receive completion queue, ibv_create_cq");	
	TEST_Z(ctx->scq = ibv_create_cq(ctx->context, ctx->tx_depth, ctx, ctx->ch, 0), "Could not create send completion queue, ibv_create_cq");

	struct ibv_qp_init_attr qp_init_attr = {
		.send_cq = ctx->scq,
		.recv_cq = ctx->rcq,
		.qp_type = IBV_QPT_RC,
		.cap = {
			.max_send_wr = ctx->tx_depth,
			.max_recv_wr = 1,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_inline_data = 0
		}
	};

	TEST_Z(ctx->qp = ibv_create_qp(ctx->pd, &qp_init_attr), "Could not create queue pair, ibv_create_qp");	
	qp_change_state_init(ctx->qp, data);
	
	return ctx;
}

/*
 * 	destroy_ctx
 * **********
 *	Destroy all the crap that was created in the init_ctx
 */
static void destroy_ctx(struct app_context *ctx) {
	TEST_NZ(ibv_destroy_qp(ctx->qp), "Could not destroy queue pair, ibv_destroy_qp");
	TEST_NZ(ibv_destroy_cq(ctx->scq), "Could not destroy send completion queue, ibv_destroy_cq");
	TEST_NZ(ibv_destroy_cq(ctx->rcq), "Coud not destroy receive completion queue, ibv_destroy_cq");
	TEST_NZ(ibv_destroy_comp_channel(ctx->ch), "Could not destory completion channel, ibv_destroy_comp_channel");
	TEST_NZ(ibv_dereg_mr(ctx->mr), "Could not de-register memory region, ibv_dereg_mr");
	TEST_NZ(ibv_dealloc_pd(ctx->pd), "Could not deallocate protection domain, ibv_dealloc_pd");	
	free(ctx->buf);
	free(ctx);
}

/*
 *  set_local_ib_connection
 * *************************
 *  Sets all relevant attributes needed for an IB connection. Those are then sent to the peer via TCP
 * 	Information needed to exchange data over IB are: 
 *	  lid - Local Identifier, 16 bit address assigned to end node by subnet manager 
 *	  qpn - Queue Pair Number, identifies qpn within channel adapter (HCA)
 *	  psn - Packet Sequence Number, used to verify correct delivery sequence of packages (similar to ACK)
 *	  rkey - Remote Key, together with 'vaddr' identifies and grants access to memory region
 *	  vaddr - Virtual Address, memory address that peer can later write to
 */
static void set_local_ib_connection(struct app_context *ctx, struct app_data *data) {

	// First get local lid
	struct ibv_port_attr attr;
	TEST_NZ(ibv_query_port(ctx->context, data->ib_port, &attr), "Could not get port attributes, ibv_query_port");

	data->local_connection.lid = attr.lid;
	data->local_connection.qpn = ctx->qp->qp_num;
	data->local_connection.psn = lrand48() & 0xffffff;
	data->local_connection.rkey = ctx->mr->rkey;
	data->local_connection.vaddr = (uintptr_t)ctx->buf + ctx->size;
}

static void print_ib_connection(char *conn_name, struct ib_connection *conn) {
	printf("%s: LID %#04x, QPN %#06x, PSN %#06x RKey %#08x VAddr %#016Lx\n", conn_name, conn->lid, conn->qpn, conn->psn, conn->rkey, conn->vaddr);

}

/*
 *  qp_change_state_rtr
 * **********************
 *  Changes Queue Pair status to RTR (Ready to receive)
 */
static int qp_change_state_rtr(struct ibv_qp *qp, struct app_data *data) {
	
	struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

    attr->qp_state              = IBV_QPS_RTR;
    attr->path_mtu              = IBV_MTU_2048;
    attr->dest_qp_num           = data->remote_connection->qpn;
    attr->rq_psn                = data->remote_connection->psn;
    attr->max_dest_rd_atomic    = 1;
    attr->min_rnr_timer         = 12;
    attr->ah_attr.is_global     = 0;
    attr->ah_attr.dlid          = data->remote_connection->lid;
    attr->ah_attr.sl            = sl;
    attr->ah_attr.src_path_bits = 0;
    attr->ah_attr.port_num      = data->ib_port;

    TEST_NZ(ibv_modify_qp(qp, attr,
                IBV_QP_STATE                |
                IBV_QP_AV                   |
                IBV_QP_PATH_MTU             |
                IBV_QP_DEST_QPN             |
                IBV_QP_RQ_PSN               |
                IBV_QP_MAX_DEST_RD_ATOMIC   |
                IBV_QP_MIN_RNR_TIMER),
        "Could not modify QP to RTR state");

	free(attr);
	
	return 0;
}

/*
 *  qp_change_state_rts
 * **********************
 *  Changes Queue Pair status to RTS (Ready to send)
 *	QP status has to be RTR before changing it to RTS
 */
static int qp_change_state_rts(struct ibv_qp *qp, struct app_data *data){

	// first the qp state has to be changed to rtr
	qp_change_state_rtr(qp, data);
	
	struct ibv_qp_attr *attr;

    attr =  malloc(sizeof *attr);
    memset(attr, 0, sizeof *attr);

	attr->qp_state              = IBV_QPS_RTS;
    attr->timeout               = 14;
    attr->retry_cnt             = 7;
    attr->rnr_retry             = 7;    /* infinite retry */
    attr->sq_psn                = data->local_connection.psn;
    attr->max_rd_atomic         = 1;

    TEST_NZ(ibv_modify_qp(qp, attr,
                IBV_QP_STATE            |
                IBV_QP_TIMEOUT          |
                IBV_QP_RETRY_CNT        |
                IBV_QP_RNR_RETRY        |
                IBV_QP_SQ_PSN           |
                IBV_QP_MAX_QP_RD_ATOMIC),
        "Could not modify QP to RTS state");

    free(attr);

    return 0;
}

#define GET_TIME ({struct timeval tv; TEST_NZ(gettimeofday(&tv, NULL), "fail to read current time"); (unsigned long)tv.tv_sec * 1000000 + tv.tv_usec; })

/*
 *  rdma_write
 * **********************
 *	Writes into buffer of peer
 */
static void rdma_write(struct app_context *ctx, struct app_data *data) {
	int i;
	ctx->sge_list.addr      = (uintptr_t)ctx->buf;
   	ctx->sge_list.length    = SINGLE_WRITE_SIZE;
   	ctx->sge_list.lkey      = ctx->mr->lkey;

	for (i = 0; i < WR_LIST_SIZE; ++i) {
		ctx->wr[i].wr.rdma.remote_addr = data->remote_connection->vaddr;
		ctx->wr[i].wr.rdma.rkey        = data->remote_connection->rkey;
		ctx->wr[i].wr_id       = i;
		ctx->wr[i].sg_list     = &ctx->sge_list;
		ctx->wr[i].num_sge     = 1;
		ctx->wr[i].opcode      = IBV_WR_RDMA_WRITE;
		ctx->wr[i].send_flags  = 0/*IBV_SEND_SIGNALED*/;
		ctx->wr[i].next        = &ctx->wr[i + 1];
	}
	ctx->wr[i - 1].send_flags  = IBV_SEND_SIGNALED;
	ctx->wr[i - 1].next        = NULL;

    struct ibv_send_wr *bad_wr;
	struct timeval tv;
	unsigned long start_time = GET_TIME;
	unsigned long end_time;
	struct ibv_wc wc;
	int ne;
	unsigned long msg_counter = 0;
	while ((end_time = GET_TIME) < start_time + RUN_TIME) {
		TEST_NZ(ibv_post_send(ctx->qp, ctx->wr, &bad_wr), "ibv_post_send failed. This is bad mkay");
		while ((ne = ibv_poll_cq(ctx->scq, 1, &wc)) == 0) {
		}
		if (ne < 0 || wc.status != IBV_WC_SUCCESS) {
			die("poll_cq() returned with error");
		}
		msg_counter += WR_LIST_SIZE;
	}
	printf("messages=%lld, time_in_us=%lld, mps=%lf\n", msg_counter, end_time - start_time, ((double)msg_counter * 1000000) / ((double)(end_time - start_time))); 
}

int main(int argc, char *argv[]) {
	struct app_context 		*ctx = NULL;
	char                    *ib_devname = NULL;
	int                   	iters = 1000;
	int                  	send_count, completion_count;
	int                   	duplex = 0;
	struct ibv_qp			*qp;

	struct app_data	 	 data = {
		.port	    		= 18515,
		.ib_port    		= 1,
		.size       		= 65536,
		.tx_depth   		= 100,
		.servername 		= NULL,
		.remote_connection 	= NULL,
		.ib_dev     		= NULL
		
	};

	if (argc == 2) {
		data.servername = argv[1];
	}
	if (argc > 2) {
		die("*Error* Usage: rdma <server>\n");
	}

	pid = getpid();

	if (!data.servername) {
		// Print app parameters. This is basically from rdma_bw app. Most of them are not used atm (at_the_moment)
		printf("PID=%d | port=%d | ib_port=%d | size=%d | tx_depth=%d | sl=%d |\n", pid, data.port, data.ib_port, data.size, data.tx_depth, sl);
	}

	// Start random is later needed to create random number for psn
	srand48(pid * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);
	TEST_Z(ctx = init_ctx(&data), "Could not create ctx, init_ctx");
	set_local_ib_connection(ctx, &data);
	
	if (data.servername) {
		data.sockfd = tcp_client_connect(&data);
	} else {
		TEST_N(data.sockfd = tcp_server_listen(&data), "tcp_server_listen (TCP) failed");
	}
	TEST_NZ(tcp_exch_ib_connection_info(&data), "Could not exchange connection, tcp_exch_ib_connection");

	// Print IB-connection details
	print_ib_connection("Local  Connection", &data.local_connection);
	print_ib_connection("Remote Connection", data.remote_connection);	

	if (data.servername) {
		qp_change_state_rts(ctx->qp, &data);
	} else {
		qp_change_state_rtr(ctx->qp, &data);
	}	

	char byte;
	if (data.servername) {
		/* Client - RDMA WRITE */
		printf("Client. Writing (RDMA-WRITE) to Server-Buffer (Buffer that was registered with MR)\n");
		rdma_write(ctx, &data);
		write(data.sockfd, &byte, sizeof(byte));
	} else {
		/* Server - Wait for read on socket to end the session */
		printf("Server. Waiting for client to finish session\n");
		read(data.sockfd, &byte, sizeof(byte));
	}

	printf("Destroying IB context\n");
	destroy_ctx(ctx);

	printf("Closing socket\n");	
	close(data.sockfd);

	return 0; 
}

