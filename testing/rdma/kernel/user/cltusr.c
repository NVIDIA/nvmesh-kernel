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
#include <getopt.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <endian.h>

#include <infiniband/verbs.h>
#include <infiniband/cm.h>
#include <infiniband/sa.h>

#include "csa.h"

#define u64 uint64_t
#define u32 uint32_t
#define u16 uint16_t
#define u8 uint8_t

#define DEBUG 1

#define _If(fmt, ...) fprintf(stdout, fmt, ## __VA_ARGS__)
#define _Ef(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)

#if DEBUG
#define _Df(fmt, ...) fprintf(stdout, fmt, ## __VA_ARGS__)
#else
#define _Df(fmt, ...)
#endif



struct c_login_request {
	
};

struct s_login_response {
	u64 raddr;
	u32 rkey;
};

// if x is NON-ZERO, error is printed
#define TEST_NZ(x, y) do { if ((x)) die(y); } while (0)

// if x is ZERO, error is printed
#define TEST_Z(x, y) do { if (!(x)) die(y); } while (0)

// if x is NEGATIVE, error is printed
#define TEST_N(x, y) do { if ((x) < 0) die(y); } while (0)

static int page_size;
static pid_t pid;

#define MAX_MSGS        256
#define SINGLE_WRITE_SIZE   256

#define MAX_RUN_SECS	600

#define RUN_TIME 5000000ULL

static const char usage_str[] = "Usage: cltusr --gid=<GUID> [--pkey=<pkey>] [--service-id=<service_id>] [--msg-size=<msg_size>] [--msgs=<n_msgs>] [--run-time=<secs>]";

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
	int 		    			sockfd;
	char						*servername;
	struct ib_connection		local_connection;
	struct ib_connection 		remote_connection;
	struct ibv_device			*ib_dev;
	union ibv_gid			clt_gid;
	union ibv_gid 			srv_gid;
	u16				pkey;
	__be64				service_id;
	int 				msg_size;
	int 				n_msgs;
	int				run_time;
	struct ibv_sa_path_rec 		path_rec;
	long long int			tot_msgs;
};

struct app_context{
	struct ibv_context 		*context;
	struct ibv_pd      		*pd;
	struct ibv_mr      		*mr;
	struct ibv_cq      		*rcq;
	struct ibv_cq      		*scq;
	struct ibv_qp      		*qp;
	struct ib_cm_device 		*cm_dev;
	struct ib_cm_id			*cm_id;
	struct ibv_comp_channel *ch;
	void               		*buf;
	unsigned            	size;
	struct ibv_sge      	sge[1];
	struct ibv_send_wr  	wr[MAX_MSGS];
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

static int find_pkey(struct app_context *ctx, struct app_data *data, u16 *pkey_index)
{
	int i;
	int rv = 0;
	struct ibv_device_attr device_attr = {{0}};
	u16 tmp_pkey;
	int idx = -1;
	int partial_idx = -1;
	
	if ((rv = ibv_query_device(ctx->context, &device_attr))) {
		fprintf(stderr, "Error %d with ibv_query_device\n", rv);
		goto out;
	}
	
	for (i = 0; i < device_attr.max_pkeys; ++i) {
		if ((rv = ibv_query_pkey(ctx->context, data->ib_port, i, &tmp_pkey))) {
			fprintf(stderr, "Error %d querying pkey table at index %i\n",
				rv, i);
			goto out;
		}
		
		if ((tmp_pkey & 0x7fff) == (data->pkey & 0x7fff)) {
			/* Partial Match - Check for Full Match */
			if ((tmp_pkey & 0x8000)) {
				/* Full Match - Done searching */
				idx = i;
				break;
			}
			else if (partial_idx < 0) {
				/* Set index for partial match */
				partial_idx = i;
			}
		}
	}
	
out:
	if (idx >= 0) {
		*pkey_index = (u16)idx;
		return 0;
	}
	else if (partial_idx >= 0) {
		*pkey_index = (u16)idx;
		return 0;
	}
	else {
		errno = -ENOENT;
		return -1;
	}
}

/*
 *  qp_change_state_init
 * **********************
 *	Changes Queue Pair status to INIT
 */
static int qp_change_state_init(struct app_context *ctx, struct app_data *data) {
	
	struct ibv_qp_attr attr = {0};
	
	TEST_NZ(find_pkey(ctx, data, &attr.pkey_index), "Error finding pkey index");

	attr.qp_state        	= IBV_QPS_INIT;
	attr.port_num        	= data->ib_port;
	attr.qp_access_flags	= IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

	TEST_NZ(ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS), "Could not modify QP to INIT, ibv_modify_qp");

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

	ctx = malloc(sizeof *ctx);
	memset(ctx, 0, sizeof *ctx);
	
	ctx->size = data->size;
	
	TEST_NZ(posix_memalign(&ctx->buf, page_size, ctx->size * 2), "could not allocate working buffer ctx->buf");

	memset(ctx->buf, 0, ctx->size * 2);

	struct ibv_device **dev_list;
	TEST_Z(dev_list = ibv_get_device_list(NULL), "No IB-device available. get_device_list returned NULL");
	TEST_Z(data->ib_dev = dev_list[0], "IB-device could not be assigned. Maybe dev_list array is empty");
	TEST_Z(ctx->context = ibv_open_device(data->ib_dev), "Could not create context, ibv_open_device");
	TEST_Z(ctx->pd = ibv_alloc_pd(ctx->context), "Could not allocate protection domain, ibv_alloc_pd");
	
	TEST_Z(ctx->cm_dev = ib_cm_open_device(ctx->context), "Could not open CM device, ib_cm_open_device");
	TEST_NZ(ib_cm_create_id(ctx->cm_dev, &ctx->cm_id, ctx), "Could not create CM ID, ib_cm_create_id");
	
	/* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
     * The Consumer is not allowed to assign Remote Write or Remote Atomic to
     * a Memory Region that has not been assigned Local Write. 
	 */

	TEST_Z(ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, ctx->size * 2, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE), "Could not allocate mr, ibv_reg_mr. Do you have root access?");
	TEST_Z(ctx->ch = ibv_create_comp_channel(ctx->context), "Could not create completion channel, ibv_create_comp_channel");
	TEST_Z(ctx->rcq = ibv_create_cq(ctx->context, 32, NULL, NULL, 0), "Could not create receive completion queue, ibv_create_cq");	
	TEST_Z(ctx->scq = ibv_create_cq(ctx->context, 32, ctx, ctx->ch, 0), "Could not create send completion queue, ibv_create_cq");

	struct ibv_qp_init_attr qp_init_attr = {
		.send_cq = ctx->scq,
		.recv_cq = ctx->rcq,
		.qp_type = IBV_QPT_RC,
		.cap = {
			.max_send_wr = 32,
			.max_recv_wr = 32,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_inline_data = 0
		}
	};

	TEST_Z(ctx->qp = ibv_create_qp(ctx->pd, &qp_init_attr), "Could not create queue pair, ibv_create_qp");	
	qp_change_state_init(ctx, data);
	
	return ctx;
}

/*
 * 	destroy_ctx
 * **********
 *	Destroy all the crap that was created in the init_ctx
 */
static void destroy_ctx(struct app_context *ctx) {
	TEST_NZ(ib_cm_destroy_id(ctx->cm_id), "Could not destroy CM ID, ib_cm_destroy_id");
	ib_cm_close_device(ctx->cm_dev);
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
static int qp_change_state_rtr(struct app_context *ctx, struct app_data *data) 
{    
	struct ibv_qp_attr qp_attr = {0};
	int attr_mask = 0;
	
	qp_attr.qp_state = IBV_QPS_RTR;
	TEST_NZ(ib_cm_init_qp_attr(ctx->cm_id, &qp_attr, &attr_mask), 
		"Error setting QP to RTR, ib_cm_init_qp_attr");
	qp_attr.path_mtu = IBV_MTU_512;
	qp_attr.max_dest_rd_atomic = 4;
	TEST_NZ(ibv_modify_qp(ctx->qp, &qp_attr, attr_mask),
		"Could not modify QP to RTR state");
	
	return 0;
}

/*
 *  qp_change_state_rts
 * **********************
 *  Changes Queue Pair status to RTS (Ready to send)
 *	QP status has to be RTR before changing it to RTS
 */
static int qp_change_state_rts(struct app_context *ctx, struct app_data *data)
{
	struct ibv_qp_attr qp_attr = {0};
	int attr_mask = 0;

	// first the qp state has to be changed to rtr
	qp_change_state_rtr(ctx, data);
	
	qp_attr.qp_state = IBV_QPS_RTS;
	TEST_NZ(ib_cm_init_qp_attr(ctx->cm_id, &qp_attr, &attr_mask), 
		"Error setting QP to RTS, ib_cm_init_qp_attr");
	qp_attr.path_mtu = IBV_MTU_512;
	qp_attr.max_dest_rd_atomic = 4;
	TEST_NZ(ibv_modify_qp(ctx->qp, &qp_attr, attr_mask),
		"Could not modify QP to RTS state");
	
	return 0;
}

static void query_res_cb(osmv_query_res_t *res) {
	struct client_sa_info *sa_info = (struct client_sa_info *)res->query_context;
	
	sa_info->result = *res;
}

int lookup_path(struct app_context *ctx, struct app_data *data)
{
	struct client_sa_info sa_info = {0};
	char gid_buff0[64];
	char gid_buff1[64];
	
	sa_info.query_res_cb = query_res_cb;
	TEST_NZ(init_sa(&sa_info), "Failed to init SA, init_sa");

	fprintf(stdout, "Looking up path %s -> %s\n", 
		inet_ntop(AF_INET6, &data->clt_gid, gid_buff0, sizeof(gid_buff0)), 
		inet_ntop(AF_INET6, &data->srv_gid, gid_buff1, sizeof(gid_buff1)));
	
	TEST_NZ(lookup_path_rec_by_gid(&sa_info, &data->clt_gid, &data->srv_gid, data->service_id), 
		"Error looking up path to srv GID");
	
	memcpy(&data->path_rec, &sa_info.dst_path_record, sizeof(data->path_rec));
	
	free_sa(&sa_info);
	
	return 0;
}

static int clt_send_login_req(struct app_context *ctx, struct app_data *data)
{
	struct {
		struct ib_cm_req_param param;
		struct c_login_request priv;
	} req;

	memset(&req, 0, sizeof(req));
	req.param.primary_path = &data->path_rec;
	req.param.alternate_path = NULL;
	req.param.service_id = data->service_id;
	req.param.qp_num = data->local_connection.qpn;
	req.param.qp_type = ctx->qp->qp_type;
	req.param.private_data = &req.priv;
	req.param.private_data_len = sizeof(&req.priv);
	req.param.flow_control = 1;

	req.param.starting_psn = data->local_connection.psn;

	/*
	 * Pick some arbitrary defaults here; we could make these
	 * module parameters if anyone cared about setting them.
	 */
	req.param.responder_resources = 4;
	req.param.remote_cm_response_timeout = 20;
	req.param.local_cm_response_timeout = 20;
	req.param.retry_count = 20;
	req.param.rnr_retry_count = 20;
	req.param.max_cm_retries = 15;
	/* try to connect */
	TEST_NZ(ib_cm_send_req(ctx->cm_id, &req.param), "Error sending CM Request, ib_send_cm_req");
	return 0;
}

int clt_handle_cm_rep(struct app_context *ctx, struct app_data *data, struct ib_cm_event *cm_event)
{
	struct s_login_response *lrsp = cm_event->private_data;
	int i;
	struct ibv_recv_wr wr, *bad_wr;
	struct ibv_sge list;
	
	/* init remote connection data from cm response */
	data->remote_connection.lid = be16toh(data->path_rec.dlid);
	data->remote_connection.qpn = cm_event->param.rep_rcvd.remote_qpn;
	data->remote_connection.psn = cm_event->param.rep_rcvd.starting_psn;
	data->remote_connection.vaddr = lrsp->raddr;
	data->remote_connection.rkey = lrsp->rkey;
	
	/* set qp state to rts */
	qp_change_state_rts(ctx, data);
	
	/* Post WQEs to RQ */
	for (i = 0; i < data->size / page_size; ++i) {
		list.addr   = (uintptr_t)ctx->buf + i * page_size;
		list.length = page_size;
		list.lkey   = ctx->mr->lkey;

		wr.next     = NULL;
		wr.wr_id    = i;
		wr.sg_list  = &list;
		wr.num_sge  = 1;
		TEST_NZ(ibv_post_recv(ctx->qp, &wr, &bad_wr), "Error with ib_post_recv");
	}
	
	/* Send CM RTU */
	TEST_NZ(ib_cm_send_rtu(ctx->cm_id, NULL, 0), "Error sending CM RTU, ib_send_cm_rtu");
	return 0;
}

#define GET_TIME ({struct timeval tv; TEST_NZ(gettimeofday(&tv, NULL), "fail to read current time"); (unsigned long)tv.tv_sec * 1000000 + tv.tv_usec; })

static char *trim_whitespace(char *str)
{
	char *end;

	/* trim leading space */
	while(isspace(*str))
		str++;
	/* all spaces? */
	if (*str == 0) {
		return str;
	}
	/* trim trailing space */
	end = str + strlen(str) - 1;
	while(end > str && isspace(*end)) {
		end--;
	}
	/* write new null terminator */
	*(end + 1) = 0;

	return str;
}

static void clt_run_test(struct app_context *ctx, struct app_data *data)
{
	struct ibv_send_wr *bad_wr;
	struct ibv_wc wc = {0};
	int rv = 0;
	int i;
	struct timeval start_time, end_time, time = {0};
	struct timezone tz;
	
	_If("------> Test prep \n");
	
	ctx->sge[0].addr = (uintptr_t)ctx->buf;
	ctx->sge[0].lkey = ctx->mr->lkey;
	ctx->sge[0].length = data->msg_size;
	_Df("n_messages=%d, message_len=%d\n", data->n_msgs, data->msg_size);
	for (i = 0; i < data->n_msgs; ++i) {
		ctx->wr[i].opcode = IBV_WR_RDMA_WRITE;
		ctx->wr[i].wr_id = i;
		ctx->wr[i].wr.rdma.remote_addr = data->remote_connection.vaddr;
		ctx->wr[i].wr.rdma.rkey = data->remote_connection.rkey;
		ctx->wr[i].num_sge = 1;
		ctx->wr[i].sg_list = &ctx->sge[0];
		ctx->wr[i].next = &ctx->wr[i + 1];
	}
	ctx->wr[i - 1].next = NULL;
	ctx->wr[i - 1].send_flags = IBV_SEND_SIGNALED;
	
	_If("-----> Test starts\n");
	data->tot_msgs = 0;
	
	gettimeofday(&start_time, &tz);
	time.tv_sec = data->run_time;
	timeradd(&start_time, &time, &end_time);
	do {
		/* Poll CQ until empty */
		while ((rv = ibv_poll_cq(ctx->scq, 1, &wc)) != 0) {
			if (rv < 0) {
				_Ef("ibv_poll_cq() returned %d\n", rv);
				break;
			}
		}
		if ((rv = ibv_post_send(ctx->qp, ctx->wr, &bad_wr)) < 0) {
			_Ef("ib_post_send() returned %d\n", rv);
			break;
		}
		else {
			while ((rv = ibv_poll_cq(ctx->scq, 1, &wc)) == 0);
			if (rv <= 0 || wc.status != IBV_WC_SUCCESS) {
				_Ef("CQE: rv=%d, wc.status=%d\n", rv, (int)wc.status);
				break;
			}
			else
				rv = 0;
		}
		data->tot_msgs += data->n_msgs;
		gettimeofday(&time, &tz);
	} while (timercmp(&time, &end_time, <));
	_If("-----> Test is over\n");
	if (!rv) {
		_If("total messages = %lld\n",  data->tot_msgs);
	}
	else {
		_Ef("test ended with error %d\n", rv);
	}
}

int main(int argc, char *argv[]) {
	struct app_context 		*ctx = NULL;
	int op;
	int long_idx = -1;
	long optarg_n;
	long long optarg_ll;
	int gid_set = 0;
	struct ib_cm_event *cm_event;
	enum ib_cm_event_type  cm_event_type = IB_CM_REQ_ERROR;
	char gid_buff[64];
	
	static const char short_options[] = "g:p:s:z:n:t";
	static struct option long_options[] =
	{
		{"gid",		required_argument,	0,	'g'},
		{"pkey",	required_argument,	0,	'p'},
		{"service-id",	required_argument,	0,	's'},
		{"msg-size",	required_argument,	0,	'z'},
		{"msgs",	required_argument,	0,	'n'},
		{"run-time",	required_argument,	0,	't'},

		{0, 0, 0, 0}
	};

	struct app_data	 	 data = {
		.port	    		= 18515,
		.ib_port    		= 1,
		.size       		= 65536,
		.servername 		= NULL,
		.ib_dev     		= NULL,
		.pkey			= 0x7fff,
		.service_id		= 0,
		.msg_size		= 256,
		.n_msgs			= 1,
		.run_time		= 5
	};
	
	while ((op = getopt_long(argc, argv, short_options, long_options, &long_idx)) != -1) {
		fprintf(stdout, "optarg=%s\n", optarg);
		if (optarg) {
			optarg = trim_whitespace(optarg);
		}
		switch (op) {
		case 'g':
			if (inet_pton(AF_INET6, optarg, &data.srv_gid) != 1)
				die(usage_str);
			else
				gid_set = 1;
			break;
		case 'p':
			optarg_n = strtol(optarg, NULL, 16);

			if (optarg_n < 0 || optarg_n > 0xffff)
				die(usage_str);

			data.pkey = (u16)optarg_n;
			break;
		case 's':
			optarg_ll = strtoll(optarg, NULL, 16);
			if (optarg_ll < 0)
				die(usage_str);
			
			data.service_id = htobe64(optarg_ll);
			break;
		case 'z':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > SINGLE_WRITE_SIZE)
				die(usage_str);

			data.msg_size = optarg_n;
			break;
		case 'n':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > MAX_MSGS)
				die(usage_str);

			data.n_msgs = optarg_n;
			break;
		case 't':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > MAX_RUN_SECS)
				die(usage_str);

			data.run_time = optarg_n;
			break;

		default:
			die(usage_str);
			break;
		}
	}
	
	if (!gid_set)
		die(usage_str);
	
	pid = getpid();

	// Print app parameters.
	fprintf(stdout, "PID=%d | Server GID=%s | pkey=0x%4x | service-id=0x%llx | msg-size=%d | n_msgs=%d | run-time=%d\n", pid, inet_ntop(AF_INET6, &data.srv_gid, gid_buff, sizeof(gid_buff)), data.pkey, data.service_id, data.msg_size, data.n_msgs, data.run_time);
	
	// Start random is later needed to create random number for psn
	srand48(pid * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);
	
	/* Create context */
	TEST_Z(ctx = init_ctx(&data), "Could not create ctx, init_ctx");
	
	TEST_NZ(ibv_query_gid(ctx->context, data.ib_port, 0, &data.clt_gid), "Could not lookup local GID, ibv_query_gid");
	TEST_NZ(lookup_path(ctx, &data), "Could not lookup path to server");
	
	set_local_ib_connection(ctx, &data);
	clt_send_login_req(ctx, &data);
	
	do {
		TEST_NZ(ib_cm_get_event(ctx->cm_dev, &cm_event), "Error getting CM event, ib_cm_get_event");
		switch ((cm_event_type = cm_event->event)) {
		case IB_CM_REQ_ERROR:
			die("Error sending CM Request to server");
			break;
		case IB_CM_REJ_RECEIVED:
			die("Got CM Reject from server");
			break;
		case IB_CM_REP_RECEIVED:
			fprintf(stdout, "Got CM Response from server.\n");
			clt_handle_cm_rep(ctx, &data, cm_event);
			break;
		default:
			fprintf(stderr, "Received unexpected CM Event %d\n", cm_event_type);
		}
		TEST_NZ(ib_cm_ack_event(cm_event), "Error ack'ing CM event, ib_cm_ack_event");
	} while (cm_event_type != IB_CM_REP_RECEIVED);

	// Print IB-connection details
	print_ib_connection("Local  Connection", &data.local_connection);
	print_ib_connection("Remote Connection", &data.remote_connection);
	
	//Run the test
	clt_run_test(ctx, &data);
	
	printf("Destroying IB context\n");
	destroy_ctx(ctx);

	return 0; 
}

