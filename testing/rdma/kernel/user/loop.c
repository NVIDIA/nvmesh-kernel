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
#include <ctype.h>

#include <infiniband/verbs.h>
#include <infiniband/cm.h>
#include <infiniband/sa.h>

#define MIN(A,B) ((A) > (B) ? (A) : (B))

#define DEBUG 1

#define _If(fmt, ...) fprintf(stdout, fmt, ## __VA_ARGS__)
#define _Ef(fmt, ...) fprintf(stderr, fmt, ## __VA_ARGS__)

#if DEBUG
#define _Df(fmt, ...) fprintf(stdout, fmt, ## __VA_ARGS__)
#else
#define _Df(fmt, ...)
#endif

// if x is NON-ZERO, error is printed
#define TEST_NZ(x, y) do { if ((x)) die(y); } while (0)

// if x is ZERO, error is printed
#define TEST_Z(x, y) do { if (!(x)) die(y); } while (0)

// if x is NEGATIVE, error is printed
#define TEST_N(x, y) do { if ((x) < 0) die(y); } while (0)

static int page_size;
static pid_t pid;

#define MAX_MSGS        256

#define MAX_RUN_SECS	600

#define MAX_PORT	255

#define RUN_TIME 5000000ULL

static const char usage_str[] = "Usage: cltusr [--dev=<devname>] [--port=<portnum>] [--gid=<GUID>] [--msg-size=<msg_size>] [--msgs=<n_msgs>] [--run-time=<secs>]";

struct app_data {
	char			dev_name[IBV_SYSFS_NAME_MAX];
	int			port;
	uint8_t			port_link_layer;
	uint16_t 		port_lid;
	struct ibv_device	*ib_dev;
	union ibv_gid 		gid;
	int			gid_set;
	int 			msg_size;
	int 			n_msgs;
	int			run_time;
	long long int		tot_sent_msgs;
	long long int		tot_sent_bytes;
	long long int		tot_recv_msgs;
	long long int		tot_recv_bytes;
};

struct app_context{
	struct ibv_context 		*context;
	struct ibv_pd      		*pd;
	struct ibv_mr      		*mr_send, *mr_recv;
	struct ibv_cq      		*cq;
	struct ibv_qp      		*qp_send, *qp_recv;
	struct ibv_comp_channel 	*ch;
	void               		*buf_send, *buf_recv;
	unsigned            		buf_size;
	struct ibv_sge      		sge[MAX_MSGS];
	struct ibv_send_wr  		wr[MAX_MSGS];
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
 *  qp_change_state_init
 * **********************
 *	Changes Queue Pair status to INIT
 */
static int qp_change_state_init(struct app_context *ctx, struct app_data *data) {
	
	struct ibv_qp_attr attr = {0};
	
	attr.qp_state        	= IBV_QPS_INIT;
	attr.port_num        	= data->port;
	attr.pkey_index		= 0;
	attr.qp_access_flags	= IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;

	TEST_NZ(ibv_modify_qp(ctx->qp_send, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS), "Could not modify QP to INIT, ibv_modify_qp");
	TEST_NZ(ibv_modify_qp(ctx->qp_recv, &attr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS), "Could not modify QP to INIT, ibv_modify_qp");

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
	int n_devs = 0, i;

	ctx = malloc(sizeof *ctx);
	memset(ctx, 0, sizeof *ctx);
	
	ctx->buf_size = MIN(page_size, data->n_msgs * data->msg_size);
	
	TEST_NZ(posix_memalign(&ctx->buf_send, page_size, ctx->buf_size), "could not allocate working buffer ctx->buf");
	TEST_NZ(posix_memalign(&ctx->buf_recv, page_size, ctx->buf_size), "could not allocate working buffer ctx->buf");

	memset(ctx->buf_send, 0, ctx->buf_size);
	memset(ctx->buf_recv, 0, ctx->buf_size);

	struct ibv_device **dev_list;
	TEST_Z(dev_list = ibv_get_device_list(&n_devs), "No IB-device available. get_device_list returned NULL");
	TEST_Z(n_devs, "No IB-device available. get_device_list returned zero devices");
	
	if (strlen(data->dev_name) > 0) {
		for (i = 0; i < n_devs; i++) {
			if (strncmp(data->dev_name, dev_list[i]->name, IBV_SYSFS_NAME_MAX) == 0) {
				data->ib_dev = dev_list[i];
				break;
			}
		}
	} else
		data->ib_dev = dev_list[0];

	TEST_Z(data->ib_dev, "IB-device could not be assigned. Maybe dev_list array is empty");
	TEST_Z(ctx->context = ibv_open_device(data->ib_dev), "Could not create context, ibv_open_device");
	TEST_Z(ctx->pd = ibv_alloc_pd(ctx->context), "Could not allocate protection domain, ibv_alloc_pd");

	/* We dont really want IBV_ACCESS_LOCAL_WRITE, but IB spec says:
     * The Consumer is not allowed to assign Remote Write or Remote Atomic to
     * a Memory Region that has not been assigned Local Write. 
	 */

	TEST_Z(ctx->mr_send = ibv_reg_mr(ctx->pd, ctx->buf_send, ctx->buf_size, 
					 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE), 
					"Could not allocate mr, ibv_reg_mr.");
	TEST_Z(ctx->mr_recv = ibv_reg_mr(ctx->pd, ctx->buf_recv, ctx->buf_size, 
					 IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE), 
					"Could not allocate mr, ibv_reg_mr.");
	TEST_Z(ctx->ch = ibv_create_comp_channel(ctx->context), "Could not create completion channel, ibv_create_comp_channel");
	TEST_Z(ctx->cq = ibv_create_cq(ctx->context, MAX_MSGS + 1, ctx, ctx->ch, 0), "Could not create send completion queue, ibv_create_cq");

	struct ibv_qp_init_attr qp_init_attr = {
		.send_cq = ctx->cq,
		.recv_cq = ctx->cq,
		.qp_type = IBV_QPT_RC,
		.cap = {
			.max_send_wr = MAX_MSGS,
			.max_recv_wr = MAX_MSGS,
			.max_send_sge = 1,
			.max_recv_sge = 1,
			.max_inline_data = 0
		}
	};

	TEST_Z(ctx->qp_send = ibv_create_qp(ctx->pd, &qp_init_attr), "Could not create queue pair, ibv_create_qp");
	TEST_Z(ctx->qp_recv = ibv_create_qp(ctx->pd, &qp_init_attr), "Could not create queue pair, ibv_create_qp");		
	qp_change_state_init(ctx, data);

	return ctx;
}

/*
 * 	destroy_ctx
 * **********
 *	Destroy all the crap that was created in the init_ctx
 */
static void destroy_ctx(struct app_context *ctx) {
	TEST_NZ(ibv_destroy_qp(ctx->qp_send), "Could not destroy queue pair, ibv_destroy_qp");
	TEST_NZ(ibv_destroy_qp(ctx->qp_recv), "Could not destroy queue pair, ibv_destroy_qp");
	TEST_NZ(ibv_destroy_cq(ctx->cq), "Could not destroy send completion queue, ibv_destroy_cq");
	TEST_NZ(ibv_destroy_comp_channel(ctx->ch), "Could not destory completion channel, ibv_destroy_comp_channel");
	TEST_NZ(ibv_dereg_mr(ctx->mr_send), "Could not de-register memory region, ibv_dereg_mr");
	TEST_NZ(ibv_dereg_mr(ctx->mr_recv), "Could not de-register memory region, ibv_dereg_mr");
	TEST_NZ(ibv_dealloc_pd(ctx->pd), "Could not deallocate protection domain, ibv_dealloc_pd");	
	free(ctx->buf_send);
	free(ctx->buf_recv);
	free(ctx);
}

static void get_port_info(struct app_context *ctx, struct app_data *data)
{
	struct ibv_port_attr port_attr = {0};

	TEST_NZ(ibv_query_port(ctx->context, data->port, &port_attr), "Error with ibv_query_port");
	data->port_link_layer = port_attr.link_layer;
	data->port_lid = port_attr.lid;
}


/*
 * 	connect_qps
 * **********
 *	Connect the Loopback QPs
 */
static void connect_qps(struct app_context *ctx, struct app_data *data)
{
	uint32_t send_qp_psn	= lrand48() & 0xffffff;
	uint32_t recv_qp_psn	= lrand48() & 0xffffff;
	struct ibv_qp_attr qp_connect_attr = {0};
	int gid_selected = 0;

	qp_connect_attr.qp_state		= IBV_QPS_RTR;
	qp_connect_attr.path_mtu		= IBV_MTU_512;
	qp_connect_attr.rq_psn          	= recv_qp_psn;	
	qp_connect_attr.dest_qp_num     	= ctx->qp_recv->qp_num;
	qp_connect_attr.ah_attr.dlid    	= data->port_lid;
	qp_connect_attr.ah_attr.port_num 	= data->port;
	qp_connect_attr.max_rd_atomic    	= 1;
	qp_connect_attr.max_dest_rd_atomic	= 1;
	qp_connect_attr.min_rnr_timer		= 12,
	qp_connect_attr.port_num		= data->port;
	qp_connect_attr.timeout			= 14;
	qp_connect_attr.retry_cnt		= 7;
	qp_connect_attr.rnr_retry		= 7;

	if (data->port_link_layer == IBV_LINK_LAYER_ETHERNET) {
		qp_connect_attr.ah_attr.is_global = 1;
		for (qp_connect_attr.ah_attr.grh.sgid_index = 0;
			!ibv_query_gid(ctx->context, data->port, qp_connect_attr.ah_attr.grh.sgid_index, 
					&qp_connect_attr.ah_attr.grh.dgid);
			qp_connect_attr.ah_attr.grh.sgid_index++) {
			
			if (!data->gid_set || qp_connect_attr.ah_attr.grh.dgid.global.interface_id == data->gid.global.interface_id) {
				gid_selected = 1;
				break;
			}
		}
	}

	if (!gid_selected) {
		errno = EINVAL;
		die("Chosen GID not found in Port GID Table\n");
	}

	TEST_NZ(ibv_modify_qp(ctx->qp_send, &qp_connect_attr,
			IBV_QP_STATE              |
			IBV_QP_AV                 |
			IBV_QP_PATH_MTU           |
			IBV_QP_DEST_QPN           |
			IBV_QP_RQ_PSN             |
			IBV_QP_MAX_DEST_RD_ATOMIC |
			IBV_QP_MIN_RNR_TIMER), "Error modifying QP to RTR");

	qp_connect_attr.dest_qp_num 	= ctx->qp_send->qp_num;
	qp_connect_attr.rq_psn 		= send_qp_psn;

	TEST_NZ(ibv_modify_qp(ctx->qp_recv, &qp_connect_attr,
			IBV_QP_STATE              |
			IBV_QP_AV                 |
			IBV_QP_PATH_MTU           |
			IBV_QP_DEST_QPN           |
			IBV_QP_RQ_PSN             |
			IBV_QP_MAX_DEST_RD_ATOMIC |
			IBV_QP_MIN_RNR_TIMER), "Error modifying QP to RTR");

	memset(&qp_connect_attr, 0, sizeof(qp_connect_attr));
	
        qp_connect_attr.qp_state		= IBV_QPS_RTS;
	qp_connect_attr.sq_psn			= send_qp_psn;
	qp_connect_attr.timeout			= 14;
	qp_connect_attr.retry_cnt		= 7;
	qp_connect_attr.rnr_retry		= 7;
	qp_connect_attr.max_dest_rd_atomic	= 1;

	TEST_NZ(ibv_modify_qp(ctx->qp_send, &qp_connect_attr,
			      IBV_QP_STATE              |
			      IBV_QP_TIMEOUT            |
			      IBV_QP_RETRY_CNT          |
			      IBV_QP_RNR_RETRY          |
			      IBV_QP_SQ_PSN             |
			      IBV_QP_MAX_QP_RD_ATOMIC), "Error modifying QP to RTS");
	
	qp_connect_attr.sq_psn		= recv_qp_psn;

	TEST_NZ(ibv_modify_qp(ctx->qp_recv, &qp_connect_attr,
			      IBV_QP_STATE              |
			      IBV_QP_TIMEOUT            |
			      IBV_QP_RETRY_CNT          |
			      IBV_QP_RNR_RETRY          |
			      IBV_QP_SQ_PSN             |
			      IBV_QP_MAX_QP_RD_ATOMIC), "Error modifying QP to RTS");
}

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

static void post_wr_recv(struct app_context *ctx, struct app_data *data, int idx)
{
	struct ibv_recv_wr wr, *bad_wr;
	struct ibv_sge list;
	
	list.addr   = (uintptr_t)ctx->buf_recv + idx * data->msg_size;
	list.length = data->msg_size;
	list.lkey   = ctx->mr_recv->lkey;

	wr.next     = NULL;
	wr.wr_id    = idx;
	wr.sg_list  = &list;
	wr.num_sge  = 1;
	TEST_NZ(ibv_post_recv(ctx->qp_recv, &wr, &bad_wr), "Error with ib_post_recv");
}

static void run_test(struct app_context *ctx, struct app_data *data)
{
	struct ibv_send_wr *bad_wr;
	struct ibv_wc wc = {0};
	

	int rv = 0;
	int i;
	struct timeval start_time, end_time, time = {0};
	struct timezone tz;
	
	_If("------> Test prep \n");

	/* Post WQEs to RQ */
	for (i = 0; i < data->n_msgs; ++i) {
		post_wr_recv(ctx, data, i);
	}

	_Df("n_messages=%d, message_len=%d\n", data->n_msgs, data->msg_size);
	for (i = 0; i < data->n_msgs; ++i) {
		ctx->wr[i].opcode = IBV_WR_SEND;
		ctx->wr[i].wr_id = i;
		ctx->wr[i].num_sge = 1;
		ctx->wr[i].sg_list = &ctx->sge[i];
		ctx->wr[i].next = &ctx->wr[i + 1];
		ctx->sge[i].addr = (uintptr_t)ctx->buf_send + i * data->msg_size;
		ctx->sge[i].lkey = ctx->mr_send->lkey;
		ctx->sge[i].length = data->msg_size;
	}
	ctx->wr[i - 1].next = NULL;
	ctx->wr[i - 1].send_flags = IBV_SEND_SIGNALED;
	
	_If("-----> Test starts\n");
	data->tot_sent_msgs = 0;
	data->tot_sent_bytes = 0;
	data->tot_recv_msgs = 0;
	data->tot_recv_bytes = 0;
	
	gettimeofday(&start_time, &tz);
	time.tv_sec = data->run_time;
	timeradd(&start_time, &time, &end_time);
	
	if ((rv = ibv_post_send(ctx->qp_send, ctx->wr, &bad_wr)) < 0) {
		_Ef("ib_post_send() returned %d\n", rv);
	} else {
		do {
			/* Poll CQ */
			if ((rv = ibv_poll_cq(ctx->cq, 1, &wc)) < 0) {
				_Ef("ibv_poll_cq() returned %d\n", rv);
				break;
			} else if (rv) {
				if (wc.opcode == IBV_WC_SEND) {
					data->tot_sent_msgs += data->n_msgs;
					data->tot_sent_bytes += wc.byte_len;
					if ((rv = ibv_post_send(ctx->qp_send, ctx->wr, &bad_wr)) < 0) {
						_Ef("ib_post_send() returned %d\n", rv);
						break;
					}
					
				} else if (wc.opcode & IBV_WC_RECV) {
					data->tot_recv_msgs++;
					data->tot_recv_bytes += wc.byte_len;
					post_wr_recv(ctx, data, (int)wc.wr_id);
				} else {
					_Ef("Invalid WC Opcode %d\n", wc.opcode);
					break;
				}
			}
			gettimeofday(&time, &tz);
		} while (timercmp(&time, &end_time, <));
	}
	_If("-----> Test is over\n");
	if (!rv) {
		_If("total sent messages = %lld sent bytes = %lld recvd messages = %lld recvd bytes = %lld\n",  
		    data->tot_sent_msgs, data->tot_sent_bytes, data->tot_recv_msgs, data->tot_recv_bytes);
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
	char gid_buff[64];
	
	static const char short_options[] = "d:p:g:z:n:t:";
	static struct option long_options[] =
	{
		{"dev",		required_argument,	0,	'd'},
		{"port", 	required_argument,	0,	'p'},
		{"gid",		required_argument,	0,	'g'},
		{"msg-size",	required_argument,	0,	'z'},
		{"msgs",	required_argument,	0,	'n'},
		{"run-time",	required_argument,	0,	't'},

		{0, 0, 0, 0}
	};

	struct app_data	 	 data = {
		.port	    		= 1,
		.ib_dev     		= NULL,
		.msg_size		= 256,
		.n_msgs			= 1,
		.run_time		= 5
	};
	
	data.dev_name[0] = 0;
	
	while ((op = getopt_long(argc, argv, short_options, long_options, &long_idx)) != -1) {
		fprintf(stdout, "optarg=%s\n", optarg);
		if (optarg) {
			optarg = trim_whitespace(optarg);
		}
		switch (op) {
		case 'd':
			strncpy(&data.dev_name[0], optarg, IBV_SYSFS_NAME_MAX);
			break;
		case 'p':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > MAX_PORT) {
				errno = EINVAL;
				die(usage_str);
			}

			data.port = optarg_n;
			break;
		case 'g':
			if (inet_pton(AF_INET6, optarg, &data.gid) != 1) {
				errno = EINVAL;				
				die(usage_str);
			}
			else
				data.gid_set = 1;
			break;
		case 'z':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > page_size) {
				errno = EINVAL;
				die(usage_str);
			}

			data.msg_size = optarg_n;
			break;
		case 'n':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > MAX_MSGS) {
				errno = EINVAL;
				die(usage_str);
			}

			data.n_msgs = optarg_n;
			break;
		case 't':
			optarg_n = strtol(optarg, NULL, 10);

			if (optarg_n < 1 || optarg_n > MAX_RUN_SECS) {
				errno = EINVAL;
				die(usage_str);
			}

			data.run_time = optarg_n;
			break;

		default:
			errno = EINVAL;
			die(usage_str);
			break;
		}
	}
	
	pid = getpid();

	// Print app parameters.
	fprintf(stdout, "PID=%d | Server GID=%s | port=%d | msg-size=%d | n_msgs=%d | run-time=%d\n", pid, inet_ntop(AF_INET6, &data.gid, gid_buff, sizeof(gid_buff)), 
		data.port, data.msg_size, data.n_msgs, data.run_time);
	
	// Start random is later needed to create random number for psn
	srand48(pid * time(NULL));
	page_size = sysconf(_SC_PAGESIZE);
	
	/* Create context */
	TEST_Z(ctx = init_ctx(&data), "Could not create ctx, init_ctx");
	get_port_info(ctx, &data);
	
	fprintf(stdout, "Device %s | Port LID: %d | Port Type %s | Send QPn: %x | Recv QPn: %x\n",
		data.ib_dev->name, data.port_lid, (data.port_link_layer == IBV_LINK_LAYER_ETHERNET ? "RoCE" : "IB"),
		ctx->qp_send->qp_num, ctx->qp_recv->qp_num);
	
	connect_qps(ctx, &data);
	
	//Run the test
	run_test(ctx, &data);
	
	printf("Destroying IB context\n");
	destroy_ctx(ctx);

	return 0; 
}

