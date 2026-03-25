/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_nm_ibud.h"
#include <rdma/rdma_cma.h>
#include <infiniband/ib.h>

static const char * const nvmeibt_ib_qkey_string = "TOMA";

#define NVMEIBT_IB_QKEY (be32toh(*(uint32_t *)nvmeibt_ib_qkey_string))
/* Utility macros */
#define IBUD_CREATE_RSC(name, r, f, ...) \
	({ \
	 	if ((r = f(__VA_ARGS__))) \
			NNVMEIBT_TOMA_REGISTER_RSC(name, r, sizeof(*r)); \
		else \
			N_Ef(name##_ibud_failed_creation, "Failed to call " #f " got errno=@AUTO_ERRNO"); \
		r; \
	})

#define IBUD_DESTROY_RSC(name, r, f) \
	do { \
		NNVMEIBT_TOMA_UNREGISTER_RSC(name, r, sizeof(*(r))); \
		f(r); \
		(r) = NULL; \
	} while (0)

#define IBUD_CREATE_RSC_RV(name, r, ok, f, ...) \
	({ \
		int __rv__; \
		if ((__rv__ = f(__VA_ARGS__)) == ok) \
			NNVMEIBT_TOMA_REGISTER_RSC(name, r, sizeof(*r)); \
		else \
			N_Ef(name##_ibud_failed_creation_rv, "Failed to call " #f " got errno=@AUTO_ERRNO"); \
		__rv__; \
	})

static void *offload_thread(void *arg);
static void add_defer_cm(struct ibud_local_node *ln, struct rdma_cm_id *cm_id);

struct nvmeibt_nm_hw_function_table ibud_func_table = {
	.allocate_local_node = nvmeibt_nm_hw_allocate_local_node,
	.init_local_node = nvmeibt_nm_hw_init_local_node,
	.free_local_node = nvmeibt_nm_hw_free_local_node,
	.hw_init = nvmeibt_nm_hw_init,
	.init_nics = nvmeibt_nm_hw_init_nics,
	.attach_nic = nvmeibt_nm_hw_attach_nic,
	.allocate_path = nvmeibt_nm_hw_allocate_path,
	.restart_path = nvmeibt_nm_hw_restart_path,
	.set_conneting_path = nvmeibt_nm_hw_set_conneting_path,
	.free_path = nvmeibt_nm_hw_free_path,
	.resolve_path = nvmeibt_nm_hw_resolve_path,
	.allocate_remote_addr = nvmeibt_nm_hw_allocate_remote_addr,
	.get_max_chunk_size = nvmeibt_nm_hw_get_max_chunk_size,
	.alloc_msgs_buffer = nvmeibt_nm_hw_alloc_msgs_buffer,
	.path_release_send_buffer = nvmeibt_nm_hw_path_release_send_buffer,
	.path_send_msg = nvmeibt_nm_hw_path_send_msg,
	.srm_get_data_offset_size = nvmeibt_nm_hw_srm_get_data_offset_size,
	.try_connect_path = nvmeibt_nm_hw_try_connect_path,
	.send_ping = nvmeibt_nm_hw_send_ping,
	.reject_connection = nvmeibt_nm_hw_reject_connection,
	.accept_connection = nvmeibt_nm_hw_accept_connection,
	.allocate_login_data = nvmeibt_nm_hw_allocate_login_data,
	.get_pp_state = nvmeibt_nm_hw_get_pp_state,
	.create_port = nvmeibt_nm_hw_create_port,
	.free_port = nvmeibt_nm_hw_free_port, 
	.free_nic = nvmeibt_nm_hw_free_nic,
	.should_revive_port = nvmeibt_nm_hw_should_revive_port,
	.wait_events = nvmeibt_nm_hw_wait_events,
	.get_port = nvmeibt_nm_hw_get_port
};

void nvmeibt_nm_hw_fill_hw_function_table(struct nvmeibt_nm_hw_function_table *in) {
	*in = ibud_func_table;
};

struct nvmeibt_nm_local_node * nvmeibt_nm_hw_allocate_local_node(void) {
	return NNVMEIBT_TOMA_CALLOC(ibud_nm_hw_allocate_local_node, 1, sizeof(struct ibud_local_node));
}

static int rdmacm_cm_ack_event(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	int rv;

	NFIN;
#if USE_PRINTF
	_Z("ln=%p", ln);
#else
	N_Df(nm_rdma_cm_ack_event_d1, "local_node @PTR", ln);
#endif
	N_Tf(nm_rdma_cm_ack_event_t1, "ack event @PTR, cm_id @PTR", event, event->id);

    if (rdma_ack_cm_event(event)) {
    	N_ETf(nm_rdmacm_cm_ack_event_e1,
			"rdma_ack_cm_event failed on event @STR: @AUTO_ERRNO",
			rdma_event_str(event->event));
    	rv = -1;
	} else
		rv = 0;
	NFOUT;
	return rv;
}

static struct nvmeibt_nm_hash_cm_connect_key_type * __attribute__ ((unused)) kt2ccl(
	struct nvmeibt_nm_hash_key_type *kt)
{
	return container_of(kt, struct nvmeibt_nm_hash_cm_connect_key_type, base);
}

static inline struct nvmeibt_nm_path *cmt2p(struct nvmeibt_nm_hash_cm_connect_key_type *cmt)
{
	return container_of(cmt, struct nvmeibt_nm_path, cmt);
}

static inline struct nvmeibt_nm_hash_key_type * __attribute__ ((unused)) l2kt(
	struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_hash_key_type, base);
}

static int rdmacm_cm_handle_event_addr_resolved(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	uint64_t id = (uint64_t)event->id->context;
	struct nvmeibt_nm_path *path;
	struct ibud_path *ibud_path;
	struct nvmeibt_nm_hash_key_type *kt;

	NFIN;
#if USE_PRINTF
	_Z("ln=%p, event=%p", ln, event);
#else
	N_Df(nm_hear_d1, "local_node @PTR, event @PTR", ln, event);
#endif
	if (!(kt = find_key(ln, id))) {
		N_ETf(nm_hear_e1, "No such client for cm_id @PTR", event->id);
		goto out;
	}
	if (kt->base.type == kt_connect_cm) {
		path = cmt2p(kt2ccl(kt));
		ibud_path = (struct ibud_path *)path;
		N_Tf(nm_hear_t1001, "Path @PATH (srm @UINT)- address resolved",
            path->name, path->srm_id);
		path->err_addr_resolved = false;
		if (rdma_resolve_route(ibud_path->cmid, 1000)) {
			N_Ef(nm_hear_etf1, "Path @PATH failed to resolve route for cm_id @PTR, errno=@AUTO_ERRNO",
				path->name, ibud_path->cmid);
			if (path->err_route_resolved) {
				N_Ef(nm_hear_et2, "Path @PATH failed to resolve route for cm_id @PTR",
					path->name, ibud_path->cmid);
					} else {
			N_ETf(nm_hear_etf2, "Path @PATH failed to resolve route for cm_id @PTR",
				path->name, ibud_path->cmid);
			path->err_route_resolved = true;
		}
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_route_resolved);
			nvmeibt_nm_restart_path(cmt2p(kt2ccl(kt)));
			goto out;
		}
		else
			nvmeibt_nm_set_path_state(cmt2p(kt2ccl(kt)), nvmeibt_nm_ps_wait_route);
	}

out:
	NFOUT;
	return 0;
}

static inline void fill_conn_params(struct rdma_conn_param *conn_param)
{
	memset(conn_param, 0, sizeof(*conn_param));
	conn_param->responder_resources = 1;
	conn_param->initiator_depth = 1;
	conn_param->retry_count = 7;
	conn_param->rnr_retry_count = 7;
}

static int rdmacm_cm_handle_event_route_resolved(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	uint64_t id = (uint64_t)event->id->context;
	struct nvmeibt_nm_hash_key_type *kt;
	struct nvmeibt_nm_path *path;

	NFIN;

	N_Df(nm_herr_d1, "local_node @PTR, event @PTR", ln, event);

	if (!(kt = find_key(ln, id))) {
		N_ETf(nm_herr_e1, "No such client cm_id @PTR", event->id);
		goto out;
	}
	if (kt->base.type == kt_connect_cm) {
		path = cmt2p(kt2ccl(kt));
		N_Tf(nm_herr_t3001, "Path @PATH (SRM @UINT)- route resolved",
            path->name, path->srm_id);
		path->err_route_resolved = false;
		nvmeibt_nm_try_connect_path(path);
	}

out:
	NFOUT;
	return 0;
}

static int rdmacm_cm_handle_event_established(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	uint64_t id = (uint64_t)event->id->context;
	struct nvmeibt_nm_hash_key_type *kt;
	struct nvmeibt_nm_path *path;
	struct ibud_path *ibud_path;
	struct ibud_login_data *p;
	struct nvmeibt_nm_login_data *login;
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];

	NFIN;
#if USE_PRINTF
	_Z("ln=%p, event=%p", ln, event);
#else
	N_Df(nm_hee_d1,
		"local_node @PTR, event @PTR", ln, event);
#endif
	if (!(kt = find_key(ln, id))) {
		N_ETf(nm_hee_e1, "No such client cm_id @PTR", event->id);
		goto out;
	}
	if (kt->base.type == kt_connect_cm) {
		path = cmt2p(kt2ccl(kt));
		ibud_path = (struct ibud_path *)path;
		p = (void *)event->param.ud.private_data;
		login = (struct nvmeibt_nm_login_data *)p;
		nvmeibt_ib_common_rdma_gid2ip(&a, &login->sgid, RDMA_CM_SERVICE_PORT,
			!nvmeibt_nic_is_roce(path->pp->transport), path->pp->broadcast_id);
		ibud_path->remote_qp_num = be32toh(p->qpn);
		ibud_path->remote_qkey = NVMEIBT_IB_QKEY;
		path->remote_srm_id = be32toh(login->srm_id);
#if USE_PRINTF
		_Z("%s", b);
#else
		N_Df(nm_hee_d11, "Received connect response from host @UUID_LE at addr @STR, "
			"srm_id @INT, qp_num @INT",
			&login->node_id, nvmeibt_nm_tss(&a, b, sizeof(b)),
			be32toh(login->srm_id), be32toh(p->qpn));
#endif
		N_Tf(nm_hee_t3001, "Path @PATH (srm @UINT, remote_srm @UINT) - connected",
            path->name, path->srm_id, path->remote_srm_id);
		/* in the case the ah was create during a previous accpet of the peer we
		 * need to destory the possible stale ah
		 */
		if (ibud_path->ah) {
			IBUD_DESTROY_RSC(nm_hee_t88, ibud_path->ah, ibv_destroy_ah);
		}
		ibud_path->ah = IBUD_CREATE_RSC(nm_hee_t89, ibud_path->ah, ibv_create_ah,
			((struct ibud_per_nic *)(path->pp->pn))->pd, &event->param.ud.ah_attr);
			if (!ibud_path->ah) {
		N_ETf(nm_hee_e5, "Failed to create address_handler - @AUTO_ERRNO");
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_ah);
			nvmeibt_nm_restart_path(path);
			goto out;
		}
		else {
			/* we have the address handler so we can terminate the cm_id */
			add_defer_cm((struct ibud_local_node *)(path->pp->pn->local_node), ibud_path->cmid);
			ibud_path->cmid = NULL;
			/* set the remote_address dlid so we can reply
			 * pings on IB setups
			 */
			if (!nvmeibt_nic_is_roce(path->pp->transport)) {
				((struct ibud_remote_addr *)path->ra)->dlid = event->param.ud.ah_attr.dlid;
					N_Df(nm_hee_d2134, "Path @PATH has dlid @UINT",
		path->name, ((struct ibud_remote_addr *)path->ra)->dlid);
			}
			if (nvmeibt_nm_connect_path(path)) {
				N_Ef(nm_hee_d2135, "path @PATH failed to connect", path->name);
				goto out;
			}
		}
	}

out:
	NFOUT;
	return 0;
}

int nvmeibt_nm_hw_accept_connection(struct nvmeibt_nm_path *path, struct nvmeibt_nm_per_port *port,
									struct nvmeibt_nm_login_data *login_data, void *data) {
		struct ibud_path *ibud_path = (struct ibud_path *)path;
		struct rdma_conn_param conn_param;
		struct ibud_login_data *ibud_login_data = (struct ibud_login_data *)login_data;
		int rv;

		fill_conn_params(&conn_param);
		/* we neeed the following three for the case we need to reply to ping
		 * requests for the peer we just accepted.
		 */
		ibud_path->remote_qp_num = be32toh(ibud_login_data->qpn);
		ibud_path->remote_qkey = NVMEIBT_IB_QKEY;

		((struct ibud_login_data *)(path->payload))->qpn = htobe32(((struct ibud_per_port *)port)->qp->qp_num);

		if (!nvmeibt_nic_is_roce(path->pp->transport)) {
			((struct ibud_remote_addr *)(path->ra))->dlid = be16toh(ibud_login_data->slid);
					N_Df(nm_hecr_d2134, "Path @PATH has dlid @UINT",
			path->name, ((struct ibud_remote_addr *)(path->ra))->dlid);
		}

		conn_param.private_data = path->payload;
		conn_param.private_data_len = sizeof(struct ibud_login_data);
		if ((rv = rdma_accept((struct rdma_cm_id *)data, &conn_param)))
			N_ETf(nm_hecr_e10, "Failed to call rdma_accept - @AUTO_ERRNO");
		
		return rv;
}

int nvmeibt_nm_hw_reject_connection(void *data, uint32_t reject_reason, struct nvmeibt_nm_login_data *login_data) {
	(void) login_data;
	
	return rdma_reject((struct rdma_cm_id *)data, &reject_reason, sizeof(reject_reason));
}

static int rdmacm_cm_handle_event_connect_request(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	uint64_t id = (uint64_t)event->listen_id->context;
	struct nvmeibt_nm_hash_key_type *kt;
	struct hash_listener_key_type *cmt;
	struct nvmeibt_nm_per_port *pp;
	struct ibud_login_data *p;
	struct nvmeibt_nm_login_data *login_data = NULL;
	struct rdma_cm_id *cmid = event->id;
	int rv;

	NFIN;
	/* regiseter the allocated CM for TOMA memory tracking */
	NNVMEIBT_TOMA_REGISTER_RSC(nm_hece_t1001, cmid, sizeof(*cmid));
	if (!(kt = find_key(ln, id))) {
		N_ETf(nm_hecr_e1, "No such listener cm_id @PTR", event->listen_id);
		rv = -1;
		goto send_reject;
	}
	cmt = nvmeibt_nm_kt2cml(kt);
	pp = nvmeibt_nm_cmt2pp(cmt);
	p = (void *)event->param.conn.private_data;
	login_data = (typeof(login_data))p;
	rv = nvmeibt_nm_on_connection_request(login_data, pp, cmid);
	goto out;

send_reject:
	nvmeibt_nm_hw_reject_connection(cmid, REJECT_FAILED_ACCEPT, NULL);
out:
	/* we do not need that id anymore */
	add_defer_cm((struct ibud_local_node *)ln, cmid);
	NFOUT;
	return rv;
}

static int rdmacm_cm_handle_error_event(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	uint64_t id = (uint64_t)event->id->context;
	struct nvmeibt_nm_hash_key_type *kt;
	struct nvmeibt_nm_path *path;

	NFIN;
#if USE_PRINTF
	_Z("ln=%p, event=%p", ln, event);
#else
	N_Df(nm_heerr_d1, "local_node @PTR, event @PTR", ln, event);
#endif
	if (!(kt = find_key(ln, id))) {
		N_ETf(nm_heerr_e1, "No such client cm_id @PTR", event->id);
		goto out;
	}
	if (kt->base.type == kt_connect_cm) {
		path = cmt2p(kt2ccl(kt));
		if (path->last_logged_event_type != event->event) {
			N_Wf(nm_heerr_e2, "path @PATH, got cm event @STR", path->name, rdma_event_str(event->event));
			path->last_logged_event_type = event->event;
		}
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_ERROR:
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_address_resolved);
		break;
	case RDMA_CM_EVENT_ROUTE_ERROR:
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_route_resolved);
		break;
	case RDMA_CM_EVENT_CONNECT_ERROR:
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_conenct_error);
		break;
	case RDMA_CM_EVENT_UNREACHABLE:
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_peer);
		break;
	case RDMA_CM_EVENT_REJECTED:
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_conenct_reject);
		break;
	default:
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_cm_error);
		break;
	}
		nvmeibt_nm_restart_path(path);
	}

out:
	NFOUT;
	return 0;
}

static int rdma_cm_handle_device_removal(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	uint64_t id = (uint64_t)event->id->context;
	struct nvmeibt_nm_hash_key_type *kt;
	struct nvmeibt_nm_per_nic *pn;
	int rv = 1;

	NFIN;
	if (!(kt = find_key(ln, id))) {
		N_ETf(nm_hedr_e1, "No such client cm_id @PTR", event->id);
		goto out;
	}
	if (kt->base.type == kt_connect_cm)
		pn = cmt2p(kt2ccl(kt))->pp->pn;
	else if (kt->base.type == kt_listener_cm)
		pn = nvmeibt_nm_cmt2pp(nvmeibt_nm_kt2cml(kt))->pn;
	else
		pn = NULL;
	if (pn) {
		rdmacm_cm_ack_event(ln, event);
		nvmeibt_nm_free_nic(pn);
		rv = 0;
	}

out:
	NFOUT;
	return rv;
}

static int process_cm_events(
	struct nvmeibt_nm_local_node *ln, struct rdma_cm_event *event)
{
	char b1[TOMA_SOCKADDR_STRING_LEN];
	char b2[TOMA_SOCKADDR_STRING_LEN];
	int ack_event = 1;
	int rv = 0;
	struct ibud_local_node *ibud_ln = (struct ibud_local_node *)ln;

	NFIN;
#if USE_PRINTF
	_Z("b1=%s", b1);
	_Z("b1=%s", b2);
#else
	N_Tf(nm_process_cm_events_t1,
		"rdmacm event(fd=@INT, cm_id=@PTR, event_status=@STATUS): "
		"event=@STR, @STR->@STR",
		ibud_ln->ch->fd, event->id, event->status,
		rdma_event_str(event->event),
		nvmeibt_nm_tss(rdma_get_local_addr(event->id), b2, sizeof(b2)),
		nvmeibt_nm_tss(rdma_get_peer_addr(event->id), b1, sizeof(b1)));
#endif
    /*
	 * Using https://linux.die.net/man/3/rdma_get_cm_event to distinguish
     * between client and server events
	 */
	/* The following applies for rdma_cm_id of type RDMA_PS_TCP only */
	assert(event->id->ps == RDMA_PS_IB || event->id->ps == RDMA_PS_UDP);

    /* Using https://linux.die.net/man/3/rdma_get_cm_event to distinguish
     * between client and server events */
  	switch (event->event) {
   	case RDMA_CM_EVENT_ADDR_RESOLVED:
    	/* client side event */
    	rdmacm_cm_handle_event_addr_resolved(ln, event);
    	break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
    	rdmacm_cm_handle_event_route_resolved(ln, event);
    	break;
    case RDMA_CM_EVENT_CONNECT_REQUEST:
    	rdmacm_cm_handle_event_connect_request(ln, event);
    	break;
    // case RDMA_CM_EVENT_CONNECT_RESPONSE:
    	//rdmacm_cm_handle_event_connect_response(ln, event);
    	//break;
    case RDMA_CM_EVENT_ESTABLISHED:
    	rdmacm_cm_handle_event_established(ln, event);
    	break;
    //case RDMA_CM_EVENT_DISCONNECTED:
    //	rdmacm_cm_handle_event_disconnected(ln, event);
    //	break;
    //case RDMA_CM_EVENT_TIMEWAIT_EXIT:
    	/* This event is generated when the QP associated with the connection
    	 * has exited its timewait state and is now ready to be re-used.
    	 * after a QP has been disconnected, it is maintained in a timewait
    	 * state to allow any in flight packets to exit the network.
    	 * after the timewait state has completed, the rdma_cm will
		 * report this event.
		 * */
    //	break;
    	/* client error events */
    case RDMA_CM_EVENT_ADDR_ERROR:
    case RDMA_CM_EVENT_ROUTE_ERROR:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
    case RDMA_CM_EVENT_REJECTED:
    	rdmacm_cm_handle_error_event(ln, event);
    	break;
    	/* client and server error events */
    //case RDMA_CM_EVENT_ADDR_CHANGE:
	//	ack_event = rdma_cm_handle_addr_change(ln, event);
	//	break;
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
		ack_event = rdma_cm_handle_device_removal(ln, event);
		break;
    default:
    	N_ETf(nm_process_cm_events_e5,
			"unexpected RDMACM event: @STR", rdma_event_str(event->event));
    	break;
    }

    if (ack_event)
		rdmacm_cm_ack_event(ln, event);

	NFOUT;
	return rv;
}

static int handle_cm_events(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct nvmeibt_nm_local_node *ln = ctx;
	struct rdma_cm_event *event;
	int cont;
	int rv;
	(void) dry_tries;

	NFIN;
#if USE_PRINTF
	_Z("is_write=%d", is_write);
#endif
	if (!is_read) {
		N_ETf(nm_handle_cm_eveents_e1,
			"Waiting for read but received something else, is_write @INT",
			is_write);
		rv = -1;
		goto out;
	}
	cont = 1;
	ln->renew_status = 1;
	while (cont) {
		rv = rdma_get_cm_event(((struct ibud_local_node *)(ln))->ch, &event);
		if (!rv)
			rv = process_cm_events(ln, event);
		if (rv < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				cont = 0;
				rv = 0;
			}
			else if (errno != EINTR)
				cont = 0;
		}
		else if (rv > 0) {
			/* OOPS: this is really weird --> ret > 0  */
			cont  = 0;
			rv = -1;
		}
	}

out:
	NFOUT;
	return rv;
}

static struct rdma_event_channel * create_event_channel(struct ibud_local_node *p)
{

	NFIN;
    p->ch = IBUD_CREATE_RSC(nm_cec_t88, p->ch, rdma_create_event_channel);
	if (!p->ch)
		N_ETf(cm_create_event_channel_e1,
			"Faioed to create event channel - @AUTO_ERRNO");
	NFOUT;
	return p->ch;
}

static inline struct nvmeibt_nm_hash_wrid_key_type * kt2wrid( struct nvmeibt_nm_hash_key_type *kt)
{
	return container_of(kt, struct nvmeibt_nm_hash_wrid_key_type, base);
}

static int handle_wc_send_comp(
	struct nvmeibt_nm_per_nic *pn, struct ibv_wc *wc, struct timespec now)
{
	struct nvmeibt_nm_hash_key_type *kt;
	struct nvmeibt_nm_hash_wrid_key_type *wrid;
	uint64_t id = wc->wr_id;
	struct nvmeibt_nm_path *path;
	uint16_t sender_index;
	int type;
	int signal_comp;
	int rv;

	//NFIN;
	if (!(kt = find_key(pn->local_node, id))) {
		N_Tf(nm_hwsc_e1, "No send_request for comp id @LU", id);
		rv = -EINVAL;
		goto out;
	}
	if (kt->base.type != kt_wrid) {
		N_ETf(nm_hwsc_e2, "id @LU does not point to a wr_id", id);
		rv = -EINVAL;
		goto out;
	}
	nvmeibt_nm_del_key(pn->local_node, &kt->base, 0);
	wrid = kt2wrid(kt);
	/* look for the local address */
	if (!(kt = find_key(pn->local_node, wrid->path_guid))) {
		N_Tf(nm_hwsc_e3, "No path for path_guid @LU",
			wrid->path_guid);
		rv = -EINVAL;
		goto free_wr;
	}
	if (kt->base.type != kt_connect_cm) {
		N_ETf(nm_hwsc_e4, "id @LU does not point to a path", wrid->path_guid);
		rv = -EINVAL;
		goto free_wr;
	}
	path = cmt2p(kt2ccl(kt));
	sender_index = wrid->sender_index;
	type = wrid->type;
	signal_comp = wrid->signaled;
	XDLIST_ADD_TAIL(&path->wrids, &wrid->base.base);
	if (signal_comp) {
		if (type == IBT_WR_SEND_PING)
					N_Df(nm_hwsc_e5, "Device @STR: Path @PATH ping send_comp for @STR",
			((struct ibud_per_nic *)pn)->device->name, path->name, wrid->response ? "reply" : "request");
		else
					N_Df(nm_hwsc_e6, "Path @PATH SRM send comp - id @INT",
			path->name, sender_index);
		path->last_send_comp_time_UNUSED = now;
		if (wc->status == IBV_WC_SUCCESS) {
			if (type != IBT_WR_SEND_PING)
				nvmeibt_srm_send_completion(path->srm, sender_index);
		}
			else {
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_send_comp);
		nvmeibt_nm_restart_path(path);
		}
	}
	rv = 0;
	goto out;

free_wr:
	NNVMEIBT_TOMA_FREE(nm_hwsc_t1, wrid);

out:
	//NFOUT;
	return rv;
}

static struct nvmeibt_nm_remote_addr * find_node_remote_address_by_dlid(
	struct nvmeibt_nm_per_port *pp, uint16_t dlid)
{
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_remote_addr *ra = NULL;
	struct nvmeibt_nm_linkable *lra;
	char b[TOMA_SOCKADDR_STRING_LEN];
	int found;

	//NFIN_;
	found = false;
	BLOCK(look_ra) XDLIST_FOREACH(lrn, &pp->pn->local_node->remotes) {
		rn = nvmeibt_nm_l2rn(lrn);
		XDLIST_FOREACH(lra, &rn->addresses) {
			ra = nvmeibt_nm_l2ra(lra);
			if (((struct ibud_remote_addr *)(ra))->dlid == dlid && ra->gid.global.subnet_prefix == pp->gid.global.subnet_prefix) {
				found = true;
				BREAK(look_ra);
			}
		}
	}
	if (!found) {
		N_Tf(nm_frabd_e1, "Failed to find remote_address for dlid @UINT", dlid);
		ra = NULL;
	}
	else
		N_Df(nm_frabd_d1, "Found remote_address @STR from node @STR",
			nvmeibt_nm_tss(&ra->a, b, sizeof(b)), ra->rn->name);
	//NFOUT_;
	return ra;
}

struct nvmeibt_nm_remote_addr * get_remote_address(
	struct nvmeibt_nm_per_nic *pn, int port_num, struct ibv_wc *wc)
{
	struct nvmeibt_nm_remote_addr *ra = NULL;
	union srq_context v;
	struct ibv_grh *grh;
	struct ibv_ah_attr ah_attr;
	struct ibud_per_nic * ibud_nic = (struct ibud_per_nic *)pn;

	N_Df(nm_gre_d2124, "wc->wc_flags & IBV_WC_GRH = @STR",
		(wc->wc_flags & IBV_WC_GRH) ? "On" : "Off");
	v.val = wc->wr_id;
	grh = ibud_nic->gmapped_buffer + v.bits.buf_idx * sizeof(*grh);
	if (ibv_init_ah_from_wc(ibud_nic->context, port_num, wc, grh, &ah_attr)) {
		N_ETf(nm_gra_e1, "Failed to initiate ah_attr - @AUTO_ERRNO");
		goto out;
	}
	ra = (wc->wc_flags & IBV_WC_GRH) ?
		nvmeibt_nm_find_node_remote_address_by_gid(pn, &ah_attr.grh.dgid) :
		find_node_remote_address_by_dlid(pn->ports[port_num - 1], ah_attr.dlid);

out:
	return ra;
}

static void trace_remote_address_paths(struct nvmeibt_nm_remote_addr *ra)
{
	char b[TOMA_SOCKADDR_STRING_LEN];
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_path *path;

	N_Tf(nm_trap_t1, "@STR on @STR", nvmeibt_nm_tss(&ra->a, b, sizeof(b)), ra->rn->name);
	XDLIST_FOREACH(l, &ra->ready) {
		path = nvmeibt_nm_l2p(l);
		N_Tf(nm_trap_t2, "Ready: path @PATH", path->name);
	}
	XDLIST_FOREACH(l, &ra->connecting) {
		path = nvmeibt_nm_l2p(l);
		N_Tf(nm_trap_t3, "Connecting: path @PATH", path->name);
	}
}


static struct nvmeibt_nm_path * find_path_by_wc(struct nvmeibt_nm_per_nic *pn, struct ibv_wc *wc)
{
	union srq_context v;
	struct nvmeibt_nm_per_port *pp = NULL;
	struct nvmeibt_nm_remote_addr *ra;
	struct nvmeibt_nm_path *path;
	struct ibud_path *ibud_path;
	struct nvmeibt_nm_linkable *l;
	struct ibv_grh *grh;
	int found;
	int i;
	struct ibud_per_nic *ibud_nic = (struct ibud_per_nic *)pn;

	//NFIN_;
	v.val = wc->wr_id;
	N_Df(nm_find_path_d1900, "Device @STR, recv_buf @INT",
		ibud_nic->device->name, v.bits.buf_idx);
	if (v.bits.buf_idx > MAX_SRQ_SIZE) {
		N_ETf(nm_find_path_e1, "Invalid receive index @INT", v.bits.buf_idx);
		goto err;
	}
	found = false;

	/* Firstly we find the dst port */
	for (i = 0; i < pn->n_ports; ++i) {
		pp = pn->ports[i];
		if (!pp->is_dead && ((struct ibud_per_port *)(pp))->qp) {
			if (((struct ibud_per_port *)(pp))->qp->qp_num == wc->qp_num) {
				found = true;
				break;
			}
		}
	}
	if (!found) {
		N_ETf(nm_find_path_e2, "No port to match the receive completion");
		goto err;
	}
	if (!(ra = get_remote_address(pn, pp->port_num, wc))) {
		N_ETf(nm_find_path_e3,
			"No remote_address to match the receive completion");
		goto err;
	}
	found = false;
	XDLIST_FOREACH(l, &ra->ready) {
		path = nvmeibt_nm_l2p(l);
		if (path->pp == pp) {
			N_Df(nm_find_path_d2000, "Found ready path @PATH", path->name);
			found = true;
			break;
		}
	}
	if (found)
		goto on_found;
	/* if there is no ready path to handle the message try to find a
	 * path that is still connecting but is the one that accepted the
	 * connection from the peer.  there should be one because a message is sent
	 * only after a connection request was accepted.
	*/
	XDLIST_FOREACH(l, &ra->connecting) {
		path = nvmeibt_nm_l2p(l);
		if (path->pp == pp) {
			N_Df(nm_find_path_d2001, "Found connecting path @PATH", path->name);
			found = true;
			break;
		}
	}
	if (found) {
		/* the path has to be in RTR which means that it is the one that
		 * accepted the connection from the peer.
		 */
		ibud_path = (struct ibud_path *)path;
		if (!path->is_rtr) {
			N_Tf(nm_find_path_73333, "Path @PATH is NOT RTR", path->name);
			found = false;
		}
		else if (!ibud_path->ah) {
					N_Df(nm_find_path_d1000, "Path @PATH is RTR but without ah so "
			"going to try and create one for it to (possibly) reply "
			"a ping request", path->name);
			grh = ((struct ibud_per_nic *)pn)->gmapped_buffer + v.bits.buf_idx * sizeof(*grh);
			if (!(ibud_path->ah = IBUD_CREATE_RSC(nm_find_path_t88, ibud_path->ah,
				ibv_create_ah_from_wc, ((struct ibud_per_nic *)pn)->pd, wc, grh, pp->port_num))) {
				found = false;
							N_ETf(nm_find_path_e300, "Path @PATH failed to create ah - "
				"@AUTO_ERRNO", path->name);
			}
		}
	}
	else
		path = NULL;
	if (!found) {
		if (!path) {
			trace_remote_address_paths(ra);
			N_ETf(nm_find_path_e4, "Failed to find path to handle the request");
		}
		goto err;
	}

on_found:
	path->renew_srm_id = 1;
	goto out;

err:
	path = NULL;

out:
	//NFOUT_;
	return path;
}

static void handle_ping_recv_comp(struct nvmeibt_nm_path *path, struct ibv_wc *wc)
{	
	union nvmeibt_nm_ping_imm_data v;

	v.raw = be32toh(wc->imm_data);
	nvmeibt_nm_on_recv_ping(path, &v);
}

static int post_recv_buf(struct nvmeibt_nm_per_nic *pn, int index)
{
	struct ibv_sge sge[2];
	struct ibv_recv_wr wr, *bad_wr;
	union srq_context v;
	int rv;
	struct ibud_per_nic *ibud_nic = (struct ibud_per_nic *)pn;

	//NFIN_;
	if (index < MAX_SRQ_SIZE) {
		sge[0].length = sizeof(struct ibv_grh);
		sge[0].lkey = ibud_nic->srq_gmr->lkey;
		sge[1].length = ibud_nic->recv_msg_size;
		sge[1].lkey = ibud_nic->srq_mmr->lkey;
		memset(&wr, 0, sizeof(wr));
		wr.sg_list = sge;
		wr.num_sge = 2;
		sge[0].addr = (unsigned long)(ibud_nic->gmapped_buffer +
			index * sizeof(struct ibv_grh));
		sge[1].addr = (unsigned long)(ibud_nic->mapped_buffer +
			index * ibud_nic->recv_msg_size);
		v.val = 0;
		v.bits.buf_idx = index;
		wr.wr_id = v.val;
		if (ibv_post_srq_recv(ibud_nic->srq, &wr, &bad_wr)) {
			N_ETf(nm_post_receivge_buffer_e1, "Failed to post to SRQ for device @STR",
				ibud_nic->device->name);
			rv = -1;
		}
		else
			rv = 0;
	}
	else {
		N_ETf(nm_post_receivge_buffer_e2, "Device @STR - index @INT is out of "
			"range @INT", ibud_nic->device->name, index, MAX_SRQ_SIZE);
		rv = -1;
	}
	//NFOUT_;
	return rv;
}


static int handle_wc_recv_comp(
	struct nvmeibt_nm_per_nic *pn, struct ibv_wc *wc, struct timespec now, nvmeibt_nm_l_list_t *pl)
{
	struct nvmeibt_nm_path *path;
	union srq_context v;
	struct ibud_per_nic *ibud_nic = (struct ibud_per_nic *)pn;

	if (wc->status == IBV_WC_SUCCESS) {
		v.val = wc->wr_id;
		if (!(path = find_path_by_wc(pn, wc)))
			goto put_back;
		N_Df(nm_hwrc_d1, "Received message on path @PATH", path->name);
		path->last_recv_comp_time_UNUSED = now;
		if (wc->wc_flags & IBV_WC_WITH_IMM)
			handle_ping_recv_comp(path, wc);
		else {
			nvmeibt_srm_receive_completion(path->srm,
				ibud_nic->mapped_buffer + v.bits.buf_idx * ibud_nic->recv_msg_size);
			if (!path->poll_linked) {
				path->poll_linked = 1;
				XDLIST_ADD_TAIL(pl, &path->poll_link);
			}
		}
	}
	else {
		N_ETf(nm_hwrc_e5, "Completion with error @STR",
			ibv_wc_status_str(wc->status));
		goto out;
	}

put_back:
	post_recv_buf(pn, v.bits.buf_idx);

out:
	return 0;
}

static inline struct nvmeibt_nm_path *pl2p(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_path, poll_link);
}

static int handle_wc_unknown_comp(struct nvmeibt_nm_per_nic *pn, struct ibv_wc *wc)
{
	struct nvmeibt_nm_hash_key_type *kt;
	struct nvmeibt_nm_hash_wrid_key_type *wrid;
	uint64_t id = wc->wr_id;
	struct nvmeibt_nm_path *path = NULL;
	int rv;

	NFIN;
	N_Ef(nm_handle_wc_unknown_comp_no_send_s, "Got opcode=@INT - unknown, trying to restart path", wc->opcode);
	/* firstly we have to clean the wrid from the hash */ 
	if (!(kt = find_key(pn->local_node, id))) {
		N_Tf(nm_handle_wc_unknown_comp_no_send_req, "No send_request for comp id @LU", id);
		rv = -EINVAL;
		goto out;
	}
	if (kt->base.type != kt_wrid) {
		N_ETf(nm_handle_wc_unknown_comp_not_wrid, "id @LU does not point to a wr_id", id);
		rv = -EINVAL;
		goto out;
	}
	nvmeibt_nm_del_key(pn->local_node, &kt->base, 0);
	wrid = kt2wrid(kt);
	/* look for the local address */
	if (!(kt = find_key(pn->local_node, wrid->path_guid))) {
		N_Tf(nm_handle_wc_unknown_comp_no_path_guid, "No path for path_guid @LU",
			wrid->path_guid);
		rv = -EINVAL;
		goto free_wr;
	}
	if (kt->base.type != kt_connect_cm) {
		N_ETf(nm_handle_wc_unknown_comp_no_path, "id @LU does not point to a path", wrid->path_guid);
		rv = -EINVAL;
		goto free_wr;
	}

	/* add the wrid back to the path struct */
	path = cmt2p(kt2ccl(kt));
	XDLIST_ADD_TAIL(&path->wrids, &wrid->base.base);
	N_Tf(nm_handle_wc_unknown_comp_restart_path, "resetart path @PATH", path->name);
	nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_unknown_wr_opcode);

	/* now we may restart the path */
	nvmeibt_nm_restart_path(path);
	rv = 0;
	goto out;

free_wr:
	NNVMEIBT_TOMA_FREE(nm_handle_wc_unknown_comp_free_wrid, wrid);

out:
	NFOUT;
	return rv;
}

/* calc IBV_WC_DRIVER2 as not in all rdma-core versions */
#define NVMEIBT_IBV_WC_DRIVER2 IBV_WC_RECV + 8
static int poll_cq(struct ibud_per_nic *pn, int *dry_tries)
{
	struct ibv_wc wc[16];
	struct timespec now;
	nvmeibt_nm_l_list_t poll_q;
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_path *path;
	int i, n, org_dry_tries = 0;

	//NFIN;
	XDLIST_HEAD_INIT(&poll_q);
	while ((n = ibv_poll_cq(pn->cq, min(ARRAY_SIZE(wc), *dry_tries), wc)) > 0) {
		org_dry_tries = *dry_tries;
		N_Df(nm_poll_cq_d1000, "n @INT", n);
		getnstimeofday_boot(&now);
		for (i = 0; i < n; ++i) {
			*dry_tries = *dry_tries - 1;
			if (wc[i].opcode == IBV_WC_RECV) {
				handle_wc_recv_comp(&pn->base, &wc[i], now, &poll_q);
			}
			else if (wc[i].opcode == IBV_WC_SEND) {
				handle_wc_send_comp(&pn->base, &wc[i], now);
			}
			else {
				if (wc[i].opcode == NVMEIBT_IBV_WC_DRIVER2) {
					N_Ef(nm_poll_cq_driver2, "Got opcode IBV_WC_DRIVER2 - known rdma-core bug: \
						https://github.com/linux-rdma/rdma-core/commit/4c905646de3e75bdccada4abe9f0d273d76eaf50");
				}
				handle_wc_unknown_comp(&pn->base, &wc[i]);
			}
		}
		if (n < min(ARRAY_SIZE(wc), org_dry_tries) || !(*dry_tries))
			break;
	}
	while (!XDLIST_EMPTY(&poll_q)) {
		l = XDLIST_FIRST(&poll_q);
		XDLIST_DEL(&l->link);
		path = pl2p(l);
		path->poll_linked = 0;
		N_Df(nm_poll_cq_d2312, "Path @PATH", path->name);
		if (path->is_rtr)
			srm_post_recv_cmpl(path->srm);
		if (path->is_rts)
			srm_post_send_cmpl(path->srm);
	}

	if (!n)
		return 0;
	else if (n < min(ARRAY_SIZE(wc), org_dry_tries))
		return 0;
	return ETIMEDOUT;
	//NFOUT;
}

static int handle_nic_comps(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct ibud_per_nic *pn = ctx;
	struct ibv_cq *ev_cq;
	void *ev_ctx;
	struct nvmeibt_nm_hash_key_type *kt;
	int rv = 0;

	//NFIN;
#if USE_PRINTF
	_Z("ctx=%p, is_read=%d, is-write=%d", ctx, is_read, is_write);
#else
	N_Df(nm_handle_nic_async_d1, "ctx @PTR, is_read @INT, is_write @INT",
		ctx, is_read, is_write);
#endif
	if (!is_read || is_write) {
		N_ETf(nm_handle_nic_comps_e1,
			"OOPS: is_read @INT. is_write @INT", is_read, is_write);
		goto out;
	}
	while (dry_tries) {
		if ((rv = ibv_get_cq_event(pn->comp_ch, &ev_cq, &ev_ctx)) && !pn->need_dry) {
			if (errno != EAGAIN && errno != EWOULDBLOCK)
				N_ETf(nm_handle_nic_comps_e2, "Failed to get completion queue "
					"rv @INT - @AUTO_ERRNO", rv);
			else {
				rv = 0;
				pn->need_dry = false;
			}
			goto out;
		}
		if (!rv) {
			ibv_ack_cq_events(ev_cq, 1);
			if (!(kt = find_key(pn->base.local_node, (uint64_t)ev_ctx))) {
				N_ETf(nm_handle_nic_comp_e3, "No CQ for id @LU",
					(uint64_t)ev_ctx);
				continue;
			}
			if (kt->base.type != kt_cq) {
				N_ETf(nm_handle_nic_comp_e4, "CQ id @LU does not point to CQ",
					(uint64_t)ev_ctx);
				continue;
			}
			if (ibv_req_notify_cq(ev_cq, 0)) {
				N_ETf(nm_handle_nic_comp_e5, "Failed to request CQ for device @STR",
					pn->device->name);
				continue;
			}
		} else {
			/* pn->need_dry */
			ev_cq = pn->cq;
			/* Rearam to be on the safe side - doesn't seems needed */
			ibv_req_notify_cq(ev_cq, 0);
		}
		pn->need_dry = true;
		if (ev_cq == pn->cq) {
			rv = poll_cq(pn, &dry_tries);
			if (!rv) {
				pn->need_dry = false;
			}
		}
	}

out:
	//NFOUT;
	return rv;
}

static int handle_nic_async(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct ibud_per_nic *pn = ctx;
	struct nvmeibt_nm_local_node *ln = pn->base.local_node;
	struct ibv_async_event async_event;
	int cont;
	int rv = 0;

	NFIN;
#if USE_PRINTF
	_Z("is_write=%d, event_str=%s",
		is_write, ibv_async_event_str[async_event.event_type]);
#endif
	if (!is_read) {
		N_ETf(nm_handle_nic_async_e1, "Waiting for read but received "
			"something else, is_write @INT", is_write);
		rv = -1;
		goto out;
	}
	cont = 1;
	while (cont && dry_tries-- > 0) {
		rv = !pn->base.is_dead? ibv_get_async_event(pn->context, &async_event) : -1;
		if (!rv) {
    		ibv_ack_async_event(&async_event);
			if (!pn->base.allowed || pn->base.is_dead)
				N_Tf(nm_handle_nic_async_t1000, "Nic @STR got async_event @STR - "
					"allowed @STR, alive @STR", pn->device->name,
					ibv_event_type_str(async_event.event_type),
					pn->base.allowed ? "Y" : "N", pn->base.is_dead ? "N" : "Y");
			else {
				N_Tf(nm_handle_nic_async_t1,
					"async_events @STR on device @STR on port @INT",
					ibv_event_type_str(async_event.event_type),
					pn->device->name, async_event.element.port_num);
				switch (async_event.event_type) {
				case IBV_EVENT_CQ_ERR:
					break;
				case IBV_EVENT_QP_FATAL:
				case IBV_EVENT_QP_REQ_ERR:
				case IBV_EVENT_QP_ACCESS_ERR:
				case IBV_EVENT_COMM_EST:
				case IBV_EVENT_SQ_DRAINED:
				case IBV_EVENT_PATH_MIG:
				case IBV_EVENT_PATH_MIG_ERR:
				case IBV_EVENT_QP_LAST_WQE_REACHED:
					nvmeibt_nm_free_nic(&pn->base);
					nvmeibt_nm_hw_attach_nic(ln,
									 (struct ibud_per_nic *)(&pn->base) - (struct ibud_per_nic *)(ln->nics[0]));
					break;
				case IBV_EVENT_SRQ_ERR:
				case IBV_EVENT_SRQ_LIMIT_REACHED:
				case IBV_EVENT_DEVICE_FATAL:
					nvmeibt_nm_free_nic(&pn->base);
					nvmeibt_nm_hw_attach_nic(ln,
									 (struct ibud_per_nic *)(&pn->base) - (struct ibud_per_nic *)(ln->nics[0]));
					break;
				/* TODO:NM these would be either report by configuration or not critical */
				case IBV_EVENT_PORT_ERR:
				case IBV_EVENT_PORT_ACTIVE:
				case IBV_EVENT_GID_CHANGE:
				case IBV_EVENT_PKEY_CHANGE:
					/* just update print info */
					ln->renew_status = 1;
					break;
				case IBV_EVENT_LID_CHANGE:
				case IBV_EVENT_SM_CHANGE:
				case IBV_EVENT_CLIENT_REREGISTER:
					pn->base.ports[async_event.element.port_num - 1]->restart_needed = true;
					break;
				default:
					break;
				}
			}
		}
		if (rv < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				cont = 0;
				rv = 0;
			}
			else if (errno != EINTR)
				cont = 0;
		}
		else if (rv > 0) {
			/* OOPS: this is really weird --> ret > 0  */
			cont  = 0;
			rv = -1;
		}
	}

out:
	NFOUT;
	return dry_tries > 0 ? rv : ETIMEDOUT;
}

static int create_srq(struct ibud_per_nic *pn)
{
	struct ibv_srq_init_attr srq_attr;
	int page_size = sysconf(_SC_PAGESIZE);
	void *mbuf;
	int mlen;
	struct ibv_srq *srq;
	struct ibv_mr *mmr;
	struct ibv_sge sge[2];
	struct ibv_recv_wr wr, *bad_wr;
	union srq_context v;
	void *gbuf;
	int glen;
	struct ibv_mr *gmr;
	int i, rv = -1;

	NFIN;
	memset(&srq_attr, 0, sizeof(srq_attr));
	mlen = MAX_SRQ_SIZE * page_size;
	if (!(mbuf = NNVMEIBT_BM_ALIGNED_ALLOC(nm_create_srq_t1, page_size, mlen))) {
		N_ETf(nm_create_srq_e1,
			"Failed to allocated RDMA buffer for device @STR",
			pn->device->name);
		goto out;
	}
	glen = MAX_SRQ_SIZE * sizeof(struct ibv_grh);
	if (!(gbuf = NNVMEIBT_BM_ALIGNED_ALLOC(
		nm_create_srq_t2, page_size, glen))) {
		N_ETf(nm_create_srq_e2,
			"Failed to allocated GRH buffer for device @STR",
			pn->device->name);
		goto free_b;
	}
	if (!(mmr = IBUD_CREATE_RSC(nm_create_srq_t88, mmr, ibv_reg_mr, pn->pd, mbuf,
		mlen, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE))) {
		N_ETf(nm_create_srq_e21, "Failed to register RDMA buffer for device @STR",
			pn->device->name);
		goto free_g;
	}
	if (!(gmr = IBUD_CREATE_RSC(nm_create_srq_t89, gmr, ibv_reg_mr, pn->pd, gbuf,
		glen, IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE))) {
		N_ETf(nm_create_srq_e22, "Failed to register GRH buffer for device @STR",
			pn->device->name);
		goto free_m;
	}
	srq_attr.attr.max_wr = MAX_SRQ_SIZE;
	srq_attr.attr.max_sge = 2;
	if (!(srq = IBUD_CREATE_RSC(nm_create_srq_t90, srq, ibv_create_srq,
		pn->pd, &srq_attr))) {
		N_ETf(nm_create_srq_e3, "Failed to create SRQ for device @STR",
			pn->device->name);
		goto free_gm;
	}
	sge[0].length = sizeof(struct ibv_grh);
	sge[0].lkey = gmr->lkey;
	sge[1].length = page_size;
	sge[1].lkey = mmr->lkey;
	memset(&wr, 0, sizeof(wr));
	wr.sg_list = sge;
	wr.num_sge = 2;
	for (i = 0; i < MAX_SRQ_SIZE; ++i) {
		sge[0].addr = (unsigned long)(gbuf + i * sizeof(struct ibv_grh));
		sge[1].addr = (unsigned long)(mbuf + i * page_size);
		v.val = 0;
		v.bits.buf_idx = i;
		wr.wr_id = v.val;
		if (ibv_post_srq_recv(srq, &wr, &bad_wr)) {
			N_ETf(nm_create_srq_e4, "Failed to post to SRQ for device @STR",
				pn->device->name);
			goto free_s;
		}
	}
	pn->srq = srq;
	pn->srq_mmr = mmr;
	pn->mapped_buffer = mbuf;
	pn->mapped_buffer_size = mlen;
	pn->srq_gmr = gmr;
	pn->gmapped_buffer = gbuf;
	pn->gmapped_buffer_size = glen;
	pn->recv_msg_size = page_size;
	rv = 0;
	goto out;

free_s:
	IBUD_DESTROY_RSC(nm_create_srq_t91, srq, ibv_destroy_srq);

free_gm:
	IBUD_DESTROY_RSC(nm_create_srq_t92, gmr, ibv_dereg_mr);

free_m:
	IBUD_DESTROY_RSC(nm_create_srq_t93, mmr, ibv_dereg_mr);

free_g:
	NNVMEIBT_BM_FREE(nm_create_srq_t20, gbuf);

free_b:
	NNVMEIBT_BM_FREE(nm_create_srq_t21, mbuf);

out:
	NFOUT;
	return rv;
}

static const char * __attribute__ ((unused)) print_port_state(
	enum ibv_port_state state)
{
	switch (state) {
	case IBV_PORT_NOP: return "IBV_PORT_NOP";
	case IBV_PORT_DOWN: return "IBV_PORT_DOWN";
	case IBV_PORT_INIT: return "IBV_PORT_INIT";
	case IBV_PORT_ARMED: return "IBV_PORT_ARMED";
	case IBV_PORT_ACTIVE: return "IBV_PORT_ACTIVE";
	case IBV_PORT_ACTIVE_DEFER: return "IBV_PORT_ACTIVE_DEFER";
	default: return "???";
	}
}

static struct local_nic_port_data * get_port_info(struct ibud_per_port *pp)
{
	struct local_nic_data *nic;
	struct local_nic_port_data *d = NULL;
	int i;
	struct local_nics_data *lnd = &pp->base.pn->local_node->lnd;

	NFIN;
	for (i = 0; i < lnd->n_nics; i++) {
		nic = &lnd->nics[i];
		if (strncmp(((struct ibud_per_nic *)(pp->base.pn))->device->name, nic->ibv_devname,
		NVMEIB_IB_DEVICE_NAME_MAX) == 0 && nic->ports[pp->base.port_num].valid) {
			d = &nic->ports[pp->base.port_num];
			break;
		}
	}
	NFOUT;
	return d;
}

static int handle_ib_port(struct ibud_per_port *pp, struct local_nic_port_data *d)
{
	struct sockaddr_ib *pib;
	char gid_str[IB_GID_STR_SIZE + 1] = {0};
	int rv = -1;

	NFIN;
	if (ibv_query_gid(ibudpp2ibudpn(pp)->context, ibudpp2pp(pp)->port_num, 0, &pp->gid)) {
		N_ETf(nm_handle_ib_port_e1,
			"Failed to query gid for port @INT for device @STR @AUTO_ERRNO",
			ibudpp2pp(pp)->port_num, ibudpp2ibudpn(pp)->device->name);
		goto out;
	}
	format_gid(&pp->gid, gid_str);
	pib = (struct sockaddr_ib *)&pp->bind_sin;
	pib->sib_family = PF_IB;
	pib->sib_sid = NVMEIB_HTONLL(RDMA_CM_IB_UD_SERVICE_ID);
	pib->sib_sid_mask = ~(uint64_t)0;
	pib->sib_pkey = nvmeib_htons(d->pkey);
	memcpy(pib->sib_addr.sib_raw, pp->gid.raw, sizeof(pib->sib_addr.sib_raw));
	pp->gid_index = 0;
	pp->global = 0;
	pp->base.broadcast_id = pp->pkey = d->pkey;
	N_Df(nm_handle_ib_port_d1, "Device @STR, port @INT pkey @PKEY - is IB @STR",
		ibudpp2ibudpn(pp)->device->name, ibudpp2pp(pp)->port_num, pp->pkey, gid_str);
	rv = 0;
	goto out;

out:
	NFOUT;
	return rv;
}

static int handle_roce_port(struct ibud_per_port *pp, struct local_nic_port_data *d)
{
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];
	char c[TOMA_SOCKADDR_STRING_LEN];
	int rv = -2;

	NFIN;
	nvmeibt_ib_common_rdma_gid2ip(&a, &d->sw_gid, RDMA_CM_SERVICE_PORT, 0, 0);
	N_Tf(nm_handle_roce_port_t100, "Now working on port @STR",
		 nvmeibt_nm_tss(&a, b, sizeof(b)));
	if (ibv_query_gid(ibudpp2ibudpn(pp)->context, ibudpp2pp(pp)->port_num, d->gid_index, &pp->gid)) {
		N_ETf(nm_handle_roce_port_e2,
			"Failed to query gid for port @INT for device @STR @AUTO_ERRNO",
			ibudpp2pp(pp)->port_num, ibudpp2ibudpn(pp)->device->name);
		goto out;
	}
	nvmeibt_ib_common_rdma_gid2ip(&a, &pp->gid, RDMA_CM_SERVICE_PORT, 0, 0);
	nvmeibt_nm_tss(&a, c, sizeof(c));
	if (memcmp(pp->gid.raw, d->sw_gid.raw, sizeof(d->sw_gid.raw))) {
		N_ETf(nm_handle_roce_port_e3,
			"Query gid for port @INT index @INT for device @STR is @STR, "
			"configured GID is @STR",
			ibudpp2pp(pp)->port_num, d->gid_index, ibudpp2ibudpn(pp)->device->name, c, b);
		goto out;
	}
	else
		pp->gid_index = d->gid_index;
	ibudpp2pp(pp)->is_ipv4 = ipv6_addr_v4mapped((struct in6_addr *)&pp->gid);
	if ((ibudpp2pp(pp)->is_ipv4 && !d->roce_ipv6) || (!ibudpp2pp(pp)->is_ipv4 && d->roce_ipv6)) {
		nvmeibt_ib_common_rdma_gid2ip(
			&pp->bind_sin, &pp->gid, RDMA_CM_SERVICE_PORT, 0, 0);
		pp->global = 1;
	}
	else {
		N_ETf(nm_handle_roce_port_e4, "Device @STR, port @INT - is RoCE @STR "
			"and it is @STR but configure says it is @STR",
			ibudpp2ibudpn(pp)->device->name, ibudpp2pp(pp)->port_num, c,
			ibudpp2pp(pp)->is_ipv4 ? "IPv4" : "IPv6",
			d->roce_ipv6 ? "IPv6" : "IPv4");
		goto out;
	}
	N_Df(nm_handle_roce_port_d1, "Device @STR, port @INT - is RoCE @STR",
		ibudpp2ibudpn(pp)->device->name, ibudpp2pp(pp)->port_num, b);
	rv = 0;
	goto out;

out:
	NFOUT;
	return rv;
}

static int find_gid(struct ibud_per_port *pp)
{
	struct local_nic_port_data *d;
	int rv = -1;

	NFIN;
	d = get_port_info(pp);
	if (d) {
		pp->base.mtu = d->mtu;
		pp->base.transport = pp->attr.link_layer == IBV_LINK_LAYER_ETHERNET? rtr_roce : rtr_ib;
		rv = nvmeibt_nic_is_roce(pp->base.transport) ? handle_roce_port(pp, d) : handle_ib_port(pp, d);
	}
	else {
		N_ETf(nm_find_gid_e1, "Port @INT on device @STR is not valid",
			pp->base.port_num, ibudpp2ibudpn(pp)->device->name);
		rv = -1;
	}
	NFOUT;
	return rv;
}

static int start_listener(struct ibud_per_port *pp)
{
	struct ibud_per_nic *pn = ibudpp2ibudpn(pp);
	struct ibud_local_node *ln = (struct ibud_local_node *)pn->base.local_node;
	struct rdma_cm_id *cm_id;
	struct hash_listener_key_type *cmt = &pp->base.cmt;
	int rv = -1;

	NFIN;
	cmt->base.guid = ++ln->base.guid;
	rv = IBUD_CREATE_RSC_RV(nm_start_listener_t88, cm_id, 0, rdma_create_id,
		ln->ch, &cm_id, (void *)cmt->base.guid, RDMA_PS_UDP);
	if (rv) {
#if USE_PRINTF
		_Z("pn=%p", pn);
#else
		N_ETf(nm_start_listener_e1, "Device @STR, port @INT - "
			"failed to create cm_id", pn->device->name, pp->base.port_num);
#endif
		pp->base.is_dead = 1;
		goto out;
	}
	else
		nvmeibt_nm_add_key(&ln->base, &cmt->base.base, cmt->base.guid,
			"listner %p - port %p", cmt, pp);
	rv = rdma_bind_addr(cm_id, (struct sockaddr *)&pp->bind_sin);
	if (rv) {
		N_ETf(nm_start_listener_e2, "Device @STR, port @INT - "
			"failed to bind cm_id - @AUTO_ERRNO",
			pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		rv = -1;
		goto free_cm_id;
	}
	if (!cm_id->verbs) {
		N_ETf(nm_start_listener_no_verbs, "No verbs found for Device @STR, port @INT after bind - check if node_guid is set for this device", pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		rv = -1;
		goto free_cm_id;
	}
	if (strncmp(cm_id->verbs->device->name, pn->context->device->name,
			sizeof(cm_id->verbs->device->name)) ||
		cm_id->port_num != pp->base.port_num) {
		N_ETf(nm_start_listener_e3, "Device @STR, port @INT - "
			"binded device is different from this device: "
			"cm_id (@STR, @PTR, @INT) pp (@STR, @PTR, @INT)",
			pn->device->name, pp->base.port_num,
			cm_id->verbs->device->name, cm_id->verbs, cm_id->port_num,
			pn->context->device->name, pn->context, pp->base.port_num);
		pp->base.is_dead = 1;
		rv = -1;
		goto free_cm_id;
	}
	rv = rdma_listen(cm_id, 3);
	if (rv) {
		N_ETf(nm_start_listener_e4, "Device @STR, port @INT - "
			"failed to listen", pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		goto free_cm_id;
	}
	pp->cm_id = cm_id;
	rv = 0;
	goto out;

free_cm_id:
	nvmeibt_nm_del_key(&ln->base, &cmt->base.base, 1);
	IBUD_DESTROY_RSC(nm_start_listener_t89, cm_id, rdma_destroy_id);

out:
	NFOUT;
	return rv;
}

static int start_qp(struct ibud_per_port *pp)
{
	struct ibv_qp_init_attr iattr;
	struct ibv_qp_attr qattr;
	struct ibv_qp *qp;
	struct ibud_per_nic *pn = ibudpp2ibudpn(pp);
	int rv = -1;

	NFIN;
	memset(&iattr, 0, sizeof(iattr));
	memset(&qattr, 0, sizeof(qattr));
	iattr.qp_type = IBV_QPT_UD;
	iattr.send_cq = pn->cq;
	iattr.recv_cq = pn->cq;
	iattr.cap.max_send_wr = MAX_RAFT_CONN_SEND_WR;
	iattr.cap.max_send_sge = 1;
	iattr.srq = pn->srq;
	if (!(qp = IBUD_CREATE_RSC(nm_startt_qp_t88, qp,
		ibv_create_qp, pn->pd, &iattr))) {
		N_ETf(nm_start_qp_e1, "Device @STR, port @INT - failed to create qp",
			pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		goto out;
	}
	/* init */
	qattr.qp_state = IBV_QPS_INIT;
	qattr.pkey_index = 0;
	qattr.port_num = pp->base.port_num;
	qattr.qkey = NVMEIBT_IB_QKEY;
	if (ibv_modify_qp(qp, &qattr,
			IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY)) {
		N_ETf(nm_start_qp_e2, "Device @STR, port @INT - failed to init qp",
			pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		goto free_qp;
	}
	/* RTR */
	memset(&qattr, 0, sizeof(qattr));
	qattr.qp_state = IBV_QPS_RTR;
	if (ibv_modify_qp(qp, &qattr, IBV_QP_STATE)) {
		N_ETf(nm_start_qp_e3, "Device @STR, port @INT - failed to RTR qp",
			pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		goto free_qp;
	}
	/* RTS */
	memset(&qattr, 0, sizeof(qattr));
	qattr.qp_state = IBV_QPS_RTS;
	qattr.sq_psn = 0;
	if (ibv_modify_qp(qp, &qattr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
		N_ETf(nm_start_qp_e4, "Device @STR, port @INT - failed to RTR qp",
			pn->device->name, pp->base.port_num);
		pp->base.is_dead = 1;
		goto free_qp;
	}
	pp->qp = qp;
	N_Tf(nm_start_qp_t1000, "Port @STR - qp num @UINT, qkey @UINT",
		pp->base.name, pp->qp->qp_num, NVMEIBT_IB_QKEY);
	rv = 0;
	goto out;

free_qp:
	IBUD_DESTROY_RSC(nm_start_qp_t89, qp, ibv_destroy_qp);

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_hw_create_port(struct nvmeibt_nm_per_port *pp)
{
	struct ibud_per_port *ibud_port = (struct ibud_per_port *)pp;
	struct ibud_per_nic *pn = ((struct ibud_per_nic *)pp->pn);
	struct hash_listener_key_type *cmt = &pp->cmt;
	struct sockaddr_storage a;
	int rv = -1;

	/* TODO:NM Try to exract as much as possible from here */
	NFIN;
	pn->base.local_node->renew_status = 1;
	pp->is_dead = 0;
	pp->is_valid = 0;
	cmt->base.base.type = kt_listener_cm;
	cmt->base.base.free = nvmeibt_nm_free_linkable;
	if (ibv_query_port(pn->context, pp->port_num, &ibud_port->attr)) {
		N_ETf(nm_create_ports_e2, "failed to query port @INT for device @STR",
			pp->port_num, pn->device->name);
		pp->is_dead = 1;
		goto out;
	}
	if (ibud_port->attr.state < IBV_PORT_ACTIVE) {
		N_ETf(nm_create_ports_e3,
			"port @INT on device @STR is not active - port's state @STR",
			pp->port_num, pn->device->name,
			print_port_state(ibud_port->attr.phys_state));
		pp->is_dead = 1;
		goto out;
	}
	if (find_gid(ibud_port)) {
		N_ETf(nm_create_ports_e4,
			"port @INT on device @STR failed to find gid",
			pp->port_num, pn->device->name);
		pp->is_dead = 1;
		goto out;
	}
	pp->gid = ibud_port->gid;

	if (memcmp(&pp->gid, &pp->conf_gid, sizeof(pp->conf_gid))) {
		N_Tf(ibud_create_port, "Conf gid doesn't match current gid on @STR", pp->name);
		pp->is_dead = 1;
		goto out;
	}

	pp->is_valid = 1;
	if (start_listener(ibud_port)) {
		N_ETf(nm_create_ports_e6,
			"port @INT on device @STR failed to start listening",
			pp->port_num, pn->device->name);
		pp->is_dead = 1;
		goto out;
	}
	nvmeibt_ib_common_rdma_gid2ip(
		&a, &pp->gid, RDMA_CM_SERVICE_PORT, !nvmeibt_nic_is_roce(pp->transport), pp->broadcast_id);
	nvmeibt_nm_tss(&a, pp->name, sizeof(pp->name));
	if (start_qp(ibud_port)) {
		N_ETf(nm_create_ports_e5,
			"port @INT on device @STR failed to allocate qp",
			pp->port_num, pn->device->name);
		pp->is_dead = 1;
		goto free_cm;
	}
	N_Df(nm_create_port_d2000, "Max message size on port @INT",
		(1 << (ibud_port->attr.active_mtu + 7)));
	rv = 0;
	goto out;

free_cm:
	nvmeibt_nm_del_key(pn->base.local_node, &pp->cmt.base.base, 1);
	/* must free the port cm_id so we recreate the listener */
	IBUD_DESTROY_RSC(nm_create_port_t89, ibud_port->cm_id, rdma_destroy_id);

out:
	NFOUT;
	return rv;
}

static int create_ports(struct ibud_per_nic *pn)
{
	struct nvmeibt_nm_per_port **ports;
	struct nvmeibt_nm_per_port *alloc;
	struct nvmeibt_nm_per_port *pp;
	int n_ports = pn->attr_ex.orig_attr.phys_port_cnt;
	int i, rv = -1;

	NFIN;
	if (!(ports = NNVMEIBT_TOMA_CALLOC(
					nm_create_ports_t1, n_ports, sizeof(struct ibud_per_port *)))) {
		N_ETf(nm_create_ports_e1, "Failed to create ports for device @STR",
			pn->device->name);
		goto out;
	}
	else {
		pn->base.ports = ports;
		pn->base.n_ports = n_ports;
	}

	if (!(alloc = NNVMEIBT_TOMA_CALLOC(
					nm_create_ports_t2, n_ports, sizeof(struct ibud_per_port)))) {
		N_ETf(nm_create_ports_p_e, "Failed to create ports for device @STR",
			pn->device->name);
		goto free_ports_p;
	}

	for (i = 1; i <= n_ports; ++i) {
		ports[i-1] = &alloc[i-1];
		pp = ports[i - 1];
		pp->pn = (typeof(pp->pn))(pn);
		pp->port_num = i;
	}
	rv = 0;
	goto out;

free_ports_p:
	NNVMEIBT_TOMA_FREE(nm_free_ports, ports);

out:
	NFOUT;
	return rv;
}

static void free_srq(struct ibud_per_nic *pn)
{
	NFIN;
	if (pn->srq) {
		IBUD_DESTROY_RSC(nm_free_srq_t88, pn->srq, ibv_destroy_srq);
	}
	if (pn->srq_mmr) {
		IBUD_DESTROY_RSC(nm_free_srq_t89, pn->srq_mmr, ibv_dereg_mr);
	}
	if (pn->srq_gmr) {
		IBUD_DESTROY_RSC(nm_free_srq_t90, pn->srq_gmr, ibv_dereg_mr);
	}
	NNVMEIBT_BM_FREE(nm_free_srq_t1, pn->mapped_buffer);
	pn->mapped_buffer = NULL;
	pn->mapped_buffer_size = 0;
	NNVMEIBT_BM_FREE(nm_free_srq_t2, pn->gmapped_buffer);
	pn->gmapped_buffer = NULL;
	pn->gmapped_buffer_size = 0;
	NFOUT;
}

static int add_nic(struct ibud_local_node *ln, int nic_index)
{
	struct ibv_device *d = ln->dev_list[nic_index];
	struct ibud_per_nic *pn = (struct ibud_per_nic *)ln->base.nics[nic_index];
	/*
	struct ibv_cq_init_attr_ex cq_attr_ex;
	*/
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_nic_async,
		.ctx = pn,
	};
	struct hash_cq_key_type *cqt = NULL;
	int rv = -1;

	NFIN;
	N_Tf(nm_add_nic_t1000, "Adding device @STR", d->name);
	ln->base.renew_status = 1;

	cqt = &pn->cqt;
	pn->base.local_node = &ln->base;

	pn->device = d;
	pn->base.allowed = 1;
	if (!(pn->context = ibv_open_device(pn->device))) {
		N_ETf(nm_add_nic_e1, "Failed to get context for device @STR",
			pn->device->name);
		goto dead;
	}
	if ((rv = nvmeibt_nonblock_fd(pn->context->async_fd)) < 0) {
		N_ETf(nm_add_nic_e11, "Fail to make async_event_fd non-blocking");
		goto dead;
	}
	pfd.fd = pn->context->async_fd;
	if (nvmeibt_nm_add_fd(&ln->base, &pfd))
		goto dead;
	if (ibv_query_device_ex(pn->context, NULL, &pn->attr_ex)) {
		N_ETf(nm_add_nic_e2, "Failed to query device @STR", pn->device->name);
		goto delfd;
	}
	if (!(pn->pd = IBUD_CREATE_RSC(
		nm_add_nic_t88, pn->pd, ibv_alloc_pd, pn->context))) {
		N_ETf(nm_add_nic_e3, "Failed to create PD for device @STR",
			pn->device->name);
		goto delfd;
	}
	if (!(pn->comp_ch = IBUD_CREATE_RSC(nm_add_nic_t89, pn->comp_ch,
		ibv_create_comp_channel, pn->context))) {
		N_ETf(nm_add_nic_e31, "Failed to create COMP CH for device @STR",
			pn->device->name);
		goto free_pd;
	}
	if ((rv = nvmeibt_nonblock_fd(pn->comp_ch->fd)) < 0) {
		N_ETf(nm_add_nix_e311, "Fail to make comp_ch_fd non-blocking");
		goto free_comp;
	}
	pfd.fd = pn->comp_ch->fd;
	pfd.f = handle_nic_comps;
	if (nvmeibt_nm_add_fd(&ln->base, &pfd))
		goto free_comp;
	cqt->base.base.type = kt_cq;
	cqt->base.base.free = nvmeibt_nm_free_linkable;
	cqt->pn = pn;
	cqt->base.guid = ++ln->base.guid;
	/*
	cq_attr_ex.channel = pn->comp_ch;
	cq_attr_ex.cq_context = (void *)cqt->base.guid;
	cq_attr_ex.cqe =
		min(MAX_RAFT_CONN_SEND_WR, pn->attr_ex.orig_attr.max_qp_wr);
	cq_attr_ex.comp_vector = 0;
	cq_attr_ex.wc_flags = IBV_WC_EX_WITH_COMPLETION_TIMESTAMP;
	*/
	if (!(pn->cq = IBUD_CREATE_RSC(nm_add_nic_t90, pn->cq, ibv_create_cq,
		pn->context, min(MAX_RAFT_CONN_SEND_WR,
		pn->attr_ex.orig_attr.max_qp_wr),
		(void *)cqt->base.guid, pn->comp_ch, 0))) {
		N_ETf(nm_add_nic_e4, "Failed to create CQ for device @STR",
			pn->device->name);
		NNVMEIBT_TOMA_FREE(nm_add_nic_t12, cqt);
		goto free_compfd;
	}
	else {
		cqt->cq = pn->cq;
		nvmeibt_nm_add_key(&ln->base, &cqt->base.base, cqt->base.guid, "CQ %p - nic %p", cqt, pn);
		if (ibv_req_notify_cq(pn->cq, 0)) {
			N_ETf(nm_add_nic_e41, "Failed to request CQ for device @STR",
				pn->device->name);
			goto free_cq;
		}
	}
	if (create_srq(pn)) {
		N_ETf(nm_add_nic_e5, "Failed to create SRQ for device @STR",
			pn->device->name);
		goto free_cq;
	}
	if (create_ports(pn)) {
		N_ETf(nm_add_nic_e6, "Failed to create ports for device @STR",
			pn->device->name);
		goto free_s;
	}

	rv = 0;
	goto out;

free_s:
	free_srq(pn);

free_cq:
	nvmeibt_nm_del_key(&ln->base, &pn->cqt.base.base, 1);
	IBUD_DESTROY_RSC(nm_add_nic_t91, pn->cq, ibv_destroy_cq);

free_compfd:
	nvmeibt_nm_del_fd(&ln->base, pn->comp_ch->fd);

free_comp:
	IBUD_DESTROY_RSC(nm_add_nic_t92, pn->comp_ch, ibv_destroy_comp_channel);

free_pd:
	IBUD_DESTROY_RSC(nm_add_nic_t93, pn->pd, ibv_dealloc_pd);

delfd:
	nvmeibt_nm_del_fd(&ln->base, pn->context->async_fd);

dead:
	pn->base.is_dead = 1;

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_hw_init(struct nvmeibt_nm_local_node *ln) {
    	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_cm_events,
		.ctx = ln,
	};
	int rv = -1;
	struct ibud_local_node *ibud_ln = (struct ibud_local_node *)ln;

	NFIN;
	
	ibud_ln->dev_list = ibv_get_device_list(&ln->n_nics);
	if (!ln->n_nics) {
		N_ETf(nm_init_rdma_e2, "No HW RDMA devices");
		rv = -1;
		goto out;
	}
	if (!create_event_channel(ibud_ln)) {
		rv = -1;
		goto out;
	}
	if ((rv = nvmeibt_nonblock_fd(ibud_ln->ch->fd)) < 0) {
		N_ETf(nm_init_rdma_e31, "Fail to make RDMA-CM fd non-blocking");
		goto free_ch;
	}
	pfd.fd = ibud_ln->ch->fd;
	if (nvmeibt_nm_add_fd(ln, &pfd)) {
		N_ETf(cm_init_rdma_e21,
			"Failed to register a watch for CM events");
		goto free_ch;
	}
	rv = 0;
	goto out;

free_ch:
	IBUD_DESTROY_RSC(cm_init_rdma_t89, ibud_ln->ch, rdma_destroy_event_channel);

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_hw_init_nics(struct nvmeibt_nm_local_node *ln) {
	int rv = 0;
	int i;
	struct ibud_per_nic *nics = NULL;

	if (!(ln->nics = NNVMEIBT_TOMA_CALLOC(
		nm_init_nics_calloc_err, ln->n_nics, sizeof(struct ibud_per_nic *)))) {
		N_ETf(cm_init_rdma_e4, "Failed to allocate NICs");
		rv = -1;
		goto out;
	}

	if (!(nics = NNVMEIBT_TOMA_CALLOC(
		nm_init_nics_p_calloc_err, ln->n_nics, sizeof(struct ibud_per_nic)))) {
		N_ETf(cm_init_rdma_e6, "Failed to allocate NICs");
		rv = -1;
		goto free_nics;
	}
	
	for (i=0; i < ln->n_nics; i++) {
		ln->nics[i] = (struct nvmeibt_nm_per_nic *)(&nics[i]);
		strlcpy(ln->nics[i]->dev_name,
				((struct ibud_local_node *)(ln))->dev_list[i]->name,
					 sizeof(ln->nics[i]->dev_name));
	}
	goto out;

free_nics:
	NNVMEIBT_TOMA_FREE(t_ibud_hw_free_nics, ln->nics);

out:
	return rv;
}

int nvmeibt_nm_hw_attach_nic(struct nvmeibt_nm_local_node *ln, int idx) {
	return add_nic((struct ibud_local_node *)ln, idx);
}

struct nvmeibt_nm_req * nvmeibt_nm_hw_get_req(void) {
	return NNVMEIBT_TOMA_CALLOC(nm_ibud_get_req, 1, sizeof(struct ibud_req));
}

void nvmeibt_nm_hw_clear_req(struct nvmeibt_nm_req *req) {
	memset(req, 0, sizeof(struct ibud_req));
}

void nvmeibt_nm_hw_set_remote_nic_event(struct nvmeibt_nm_req *req, struct nvmeibt_nic *nic) {
	struct ibud_req *ibud_req = (struct ibud_req *)req;

	ibud_req->add_nic_req.is_roce = nvmeibt_nic_is_roce(nic->transport);
	ibud_req->add_nic_req.pkey = nic->from_config.partition_key;
}

struct ibud_network_offload {
	pthread_t t;
	nvmeibt_nm_l_list_t offload_request_pool;
	nvmeibt_nm_l_list_t exec_list;
	pthread_mutex_t pool_guard;
	pthread_mutex_t exec_guard;
	pthread_cond_t wakeup;
};

static int create_offloader(struct ibud_local_node *ln)
{
	struct ibud_network_offload *w;
	pthread_condattr_t attr;
	pthread_attr_t tattr;
	int rv = -1;
	int pt_err;

	NFIN;
	if (!(w = NNVMEIBT_TOMA_CALLOC(nm_create_offloader_t1, 1, sizeof(*w)))) {
		N_ETf(nm_create_offloader_e1, "Failed to allocate offloader");
		goto out;
	}
	pt_err = pthread_mutex_init(&w->pool_guard, NULL);
	if (pt_err != 0) {
		errno = pt_err;
		N_ETf(nm_create_offloader_e2,
			"Failed to create pool_guard - @AUTO_ERRNO");
		goto free_w;
	}
	if ((pt_err = pthread_mutex_init(&w->exec_guard, NULL)) != 0) {
		errno = pt_err;
		N_ETf(nm_create_offloader_e21,
			"Failed to create exec_guard - @AUTO_ERRNO");
		goto free_pg;
	}
	if ((pt_err = pthread_condattr_init(&attr)) != 0) {
		errno = pt_err;
		N_ETf(nm_create_offloader_e22,
			"Failed to create cond var attr - @AUTO_ERRNO");
		goto free_eg;
	}
	if ((pt_err = pthread_cond_init(&w->wakeup, &attr)) != 0) {
		errno = pt_err;
		N_ETf(nm_create_offloader_e3,
			"Failed to create wakeup - @AUTO_ERRNO");
		goto free_eg;
	}
	XDLIST_HEAD_INIT(&w->offload_request_pool);
	XDLIST_HEAD_INIT(&w->exec_list);
	if ((rv = pthread_attr_init(&tattr)) != 0 ||
		(rv = pthread_create(&w->t, &tattr, offload_thread, w)) != 0) {
		errno = rv;
		N_ETf(nm_create_offloader_e23,
			"Failed to start offload thread - @AUTO_ERRNO");
		goto free_cv;
	}
	pthread_setname_np(w->t, "ibud_offload");

	rv = 0;
	goto out;

free_cv:
	pthread_cond_destroy(&w->wakeup);

free_eg:
	pthread_mutex_destroy(&w->exec_guard);

free_pg:
	pthread_mutex_destroy(&w->pool_guard);

free_w:
	NNVMEIBT_TOMA_FREE(nm_create_offloader_t223, w);
	w = NULL;

out:
	NFOUT;
	ln->network_offload = w;
	return rv;
}

enum defer_work_types {
	dwt_cmid = 1,
	dwt_stop = 2,
};

struct cm_id_work {
	struct rdma_cm_id *cm_id;
};

struct defer_work {
	struct nvmeibt_nm_linkable base;
	union {
		struct cm_id_work cmidw;
	};
};

static inline struct defer_work * l2dw( struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct defer_work, base);
}

static void free_offload_list(nvmeibt_nm_l_list_t *l)
{
	struct nvmeibt_nm_linkable *i;
	struct defer_work *d;

	NFIN;
	while (!XDLIST_EMPTY(l)) {
		i = XDLIST_FIRST(l);
		XDLIST_DEL(&i->link);
		d = l2dw(i);
		NNVMEIBT_TOMA_FREE(nm_free_offload_list_t1, d);
	}
	NFOUT;
}

static int wakeup(pthread_cond_t *cond)
{
	int rv;

	if ((rv = pthread_cond_signal(cond)) != 0) {
		errno = rv;
		N_ETf(nm_wakeup_e1, "Failed to wakeup offloader cond - @AUTO_ERRNO");
		nvmeibt_abort(ES_FATAL);
	}
	return rv;
}

static void push_defer_work(struct ibud_network_offload *w, struct defer_work *d)
{
	NFIN;
	nvmeibt_nm_mutex_lock(&w->exec_guard);
	XDLIST_ADD_TAIL(&w->exec_list, &d->base);
	nvmeibt_nm_mutex_release(&w->exec_guard);
	wakeup(&w->wakeup);
	NFOUT;
}

static void process_defer(struct ibud_local_node *ln)
{
	struct nvmeibt_nm_linkable *l;

	/*NFIN;*/
	while (!XDLIST_EMPTY(&ln->defer_work)) {
		l = XDLIST_FIRST(&ln->defer_work);
		XDLIST_DEL(&l->link);
		push_defer_work(ln->network_offload, l2dw(l));
	}
	/*NFOUT;*/
}

static struct defer_work * get_defer_work(struct ibud_network_offload *w)
{
	struct defer_work *d = NULL;
	struct nvmeibt_nm_linkable *l;

	NFIN;
	nvmeibt_nm_mutex_lock(&w->pool_guard);
	if (!XDLIST_EMPTY(&w->offload_request_pool)) {
		l = XDLIST_FIRST(&w->offload_request_pool);
		XDLIST_DEL(&l->link);
	}
	else
		l = NULL;
	nvmeibt_nm_mutex_release(&w->pool_guard);
	if (l)
		d = l2dw(l);
	else
		d = NNVMEIBT_TOMA_CALLOC(nm_get_defer_work_t1, 1, sizeof(*d));
	if (!d)
		N_ETf(nm_get_defer_work_e1, "Failed to allocate defer_work");
	else
		memset(d, 0, sizeof(*d));
	NFOUT;
	return d;
}

static void free_offloader(struct ibud_network_offload *w)
{
	struct defer_work *d;
	int pt_err;

	NFIN;
	if ((d = get_defer_work(w))) {
		d->base.type = dwt_stop;
		push_defer_work(w, d);
		if ((pt_err = pthread_join(w->t, NULL))) {
			errno = pt_err;
			N_ETf(nm_free_offloader_e1, "Failed to join offloader - @AUTO_ERRNO");
		}
	}
	pthread_cond_destroy(&w->wakeup);
	pthread_mutex_destroy(&w->exec_guard);
	pthread_mutex_destroy(&w->pool_guard);
	NNVMEIBT_TOMA_FREE(nm_free_offloader_t1, w);
	NFOUT;
}

#define WAIT_WAKEUP_TIMEOUT 5
static int timed_wait(pthread_cond_t *cond, pthread_mutex_t *m, int sec)
{
	struct timespec ts;
	int rv;

	if (sec) {
		getnstimeofday_real(&ts);
		ts.tv_sec += sec;
		if ((rv = pthread_cond_timedwait(cond, m, &ts)) != 0 &&
			rv != ETIMEDOUT) {
			errno = rv;
			N_ETf(nm_timed_wait_e1, "Failed to timed_wait for offloader cond "
				"(rv @INT) - @AUTO_ERRNO", rv);
			rv = -1;
		}
		else {
			rv = 0;
		}
	}
	else {
		if ((rv = pthread_cond_wait(cond, m)) != 0) {
			errno = rv;
			N_ETf(nm_timed_wait_e2,
				"Failed to wait for offloader cond - @AUTO_ERRNO");
			rv = -1;
		}
	}
	return rv;
}

static void *offload_thread(void *arg)
{
	struct ibud_network_offload *w = arg;
	nvmeibt_nm_l_list_t l;
	struct nvmeibt_nm_linkable *i;
	int cont = 1;
	int rv;

	NFIN;
	while (cont) {
		nvmeibt_nm_mutex_lock(&w->exec_guard);
		while (XDLIST_EMPTY(&w->exec_list)) {
			if ((rv = timed_wait(&w->wakeup, &w->exec_guard,
				WAIT_WAKEUP_TIMEOUT)) < 0) {
				nvmeibt_nm_mutex_release(&w->exec_guard);
				N_Ef(cruq94b, "Err");
				nvmeibt_abort(ES_FATAL);
				goto out;
			}
		}
		XDLIST_HEAD_INIT(&l);
		XDLIST_SPLICE(&w->exec_list, &l);
		nvmeibt_nm_mutex_release(&w->exec_guard);
		while (!XDLIST_EMPTY(&l)) {
			i = XDLIST_FIRST(&l);
			XDLIST_DEL(&i->link);
			if (i->type == dwt_cmid)
				IBUD_DESTROY_RSC(
					nm_destroy_cm, l2dw(i)->cmidw.cm_id, rdma_destroy_id);
			else if (i->type == dwt_stop)
				cont = 0;
			else
				N_Tf(nm_offlaod_thread_t1, "Unknown op @INT", (int)i->type);
			nvmeibt_nm_mutex_lock(&w->pool_guard);
			XDLIST_ADD_TAIL(&w->offload_request_pool, i);
			nvmeibt_nm_mutex_release(&w->pool_guard);
		}
	}
	free_offload_list(&l);
	nvmeibt_nm_mutex_lock(&w->exec_guard);
	free_offload_list(&w->exec_list);
	nvmeibt_nm_mutex_release(&w->exec_guard);
	nvmeibt_nm_mutex_lock(&w->pool_guard);
	free_offload_list(&w->offload_request_pool);
	nvmeibt_nm_mutex_release(&w->pool_guard);

out:
	NFOUT;
	return NULL;
}

int nvmeibt_nm_hw_init_local_node(struct nvmeibt_nm_local_node *ln) {
	XDLIST_HEAD_INIT(&((struct ibud_local_node *)(ln))->defer_work);
	return create_offloader((struct ibud_local_node *)ln);
}

static void add_defer_cm(struct ibud_local_node *ln, struct rdma_cm_id *cm_id)
{
	struct defer_work *d;

	NFIN;
	N_Df(nm_add_defer_cm_d1, "cm_id @PTR", cm_id);
	d = get_defer_work(ln->network_offload);
	if (d) {
		d->base.type = dwt_cmid;
		d->cmidw.cm_id = cm_id;
		XDLIST_ADD_TAIL(&ln->defer_work, &d->base);
	}
	else
		N_ETf(nm_add_defer_cm_e1, "Failed to allocate work to delete cm_id");
	NFOUT;
}

void nvmeibt_nm_hw_restart_path(struct nvmeibt_nm_path *path) {
	struct ibud_path *ibud_p = (struct ibud_path *)path;

	if (ibud_p->cmid) {
		add_defer_cm((struct ibud_local_node *)(path->ra->ln), ibud_p->cmid);
		ibud_p->cmid = NULL;
	}
	if (ibud_p->ah) {
		IBUD_DESTROY_RSC(nm_restart_path_t88, ibud_p->ah, ibv_destroy_ah);
	}
}

void nvmeibt_nm_hw_set_conneting_path(struct nvmeibt_nm_path *path) {
	struct ibud_per_port *ibud_pp = (struct ibud_per_port *)(path->pp);

	((struct ibud_login_data *)(path->payload))->qpn = htobe32(ibud_pp->qp->qp_num);
}


struct nvmeibt_nm_path * nvmeibt_nm_hw_allocate_path(void) {
	return NNVMEIBT_TOMA_CALLOC(ibud_nm_hw_allocate_path, 1, sizeof(struct ibud_path));
}

void nvmeibt_nm_hw_free_path(struct nvmeibt_nm_path *path) {
	struct ibud_path *ibud_p = (struct ibud_path *)path;
	
	if (ibud_p->cmid) {
		add_defer_cm(((struct ibud_local_node *)(path->ra->ln)), ibud_p->cmid);
		ibud_p->cmid = NULL;
	}
	if (ibud_p->ah) {
		IBUD_DESTROY_RSC(nm_free_path_t88, ibud_p->ah, ibv_destroy_ah);
	}
}

struct nvmeibt_nm_login_data * nvmeibt_nm_hw_allocate_login_data(void) {
	return NNVMEIBT_TOMA_CALLOC(ibud_nm_hw_allocate_login_data, 1, sizeof(struct ibud_login_data));
}

static void trace_pkeys(struct nvmeibt_nm_per_port *pp)
{
	__be16 pkey;
	int i;
	struct ibud_per_port *ibud_pp = (struct ibud_per_port *)pp;

	for (i = 0; i < ibud_pp->attr.pkey_tbl_len; ++i) {
		if (!ibv_query_pkey(((struct ibud_per_nic *)(pp->pn))->context, pp->port_num, i, &pkey))
			N_Df(nm_trace_pkey_d1, "Port @STR, pkey[@INT]=@INT32_HEX",
				pp->name, i, be16toh(pkey));
		else
			break;
	}
}

static int check_path_pkey(struct nvmeibt_nm_path *path, __be16 addr_pkey)
{
	struct nvmeibt_nm_per_port *pp = path->pp;
	__be16 pkey;
	int rv;

	PFIN;
	if ((rv = ibv_query_pkey(((struct ibud_per_nic *)(pp->pn))->context, pp->port_num, 0, &pkey)))
		N_ETf(nm_check_path_pkey_e1,
			"Failed to query pkey on path @PATH - @AUTO_ERRNO", path->name);
	else if (pkey != addr_pkey) {
			N_ETf(nm_check_path_pkey_e2, "Path @PATH - address pkey @INT32_HEX is "
		"different from port pkey @INT32_HEX", path->name,
		be16toh(addr_pkey), be16toh(pkey));
		rv = -1;
		trace_pkeys(pp);
	}
	PFOUT;
	return rv;
}

int nvmeibt_nm_hw_resolve_path(struct nvmeibt_nm_path *path) {
	struct nvmeibt_nm_hash_cm_connect_key_type *cmt;
	struct rdma_cm_id *cm_id;
	struct sockaddr_storage a;
	__be16 pkey_be;
	int rv;

	PFIN;
	cmt = &path->cmt;
	cmt->base.base.free = nvmeibt_nm_free_linkable;
	rv = IBUD_CREATE_RSC_RV(nm_hpss_t88, cm_id, 0, rdma_create_id,
		((struct ibud_local_node *)(path->pp->pn->local_node))->ch, &cm_id,
		 (void *)cmt->base.guid, RDMA_PS_UDP);
	if (rv) {
		N_ETf(nm_hpss_e2, "Path @PATH - failed to create cm_id", path->name);

		goto restart;
	}
	/* we must set the bing port of the conenct to any port since TOMA
	 * is already binded to the listener
	 */
	a = ((struct ibud_per_port *)(path->pp))->bind_sin;
	/* any port for the bing address */
	if (nvmeibt_nic_is_roce(path->pp->transport))
		((struct sockaddr_in *)&a)->sin_port = 0;
	else {
		((struct sockaddr_ib *)&a)->sib_sid =
			NVMEIB_HTONLL(RDMA_IB_IP_PS_UDP);
		pkey_be = ((struct sockaddr_ib *)&a)->sib_pkey;
		if (check_path_pkey(path, pkey_be))
			goto free_cmid;
	}
	N_Tf(nm_hpss_t1001, "Path @PATH (srm @UINT) - start address resolution",
        path->name, path->srm_id);
	if (rdma_resolve_addr(cm_id,
		(struct sockaddr *)&a, (struct sockaddr *)&path->ra->a, 1000)) {
		if (path->err_addr_resolved) {
			N_Ef(nm_hpss_e3, "Path @PATH failed to resolve address - @AUTO_ERRNO", path->name);
		} else {
			N_ETf(nm_hpss_ef3, "Path @PATH failed to resolve address - @AUTO_ERRNO", path->name);
			path->err_addr_resolved = true;
		}
		
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_address);
		goto free_cmid;
	}
	((struct ibud_path *)(path))->cmid = cm_id;
	nvmeibt_nm_path_resolve_wait(path);

	goto out;

free_cmid:
	add_defer_cm((struct ibud_local_node *)path->ra->ln, cm_id);

restart:
	nvmeibt_nm_restart_path(path);

out:
	PFOUT;
	return rv;
}

struct nvmeibt_nm_remote_addr * nvmeibt_nm_hw_allocate_remote_addr(void) {
	return NNVMEIBT_TOMA_CALLOC(ibud_nm_hw_allocate_remote_addr, 1, sizeof(struct ibud_remote_addr));
}

int nvmeibt_nm_hw_get_max_chunk_size(struct nvmeibt_nm_path *path) {
	return path->pp->mtu - sizeof(struct ibv_grh);
}

void * nvmeibt_nm_hw_alloc_msgs_buffer(struct nvmeibt_nm_path *path)
{
	struct nvmeibt_nm_per_port *pp;
	int page_size;
	void *buf = NULL;
	struct ibv_mr *mr;
	long len;

	PFIN;
	if (!path) {
		N_ETf(nm_path_sb_e1, "Carrier has no path context");
		goto out;
	}
	pp = path->pp;
	page_size = sysconf(_SC_PAGESIZE);
	len = (long)path->max_messages * path->max_chunk_size;
	len = round_up_to_page_size(len, page_size);
	buf = nvmeibt_bm_allocate_dma_buffer(page_size, len);
	if (!buf) {
		N_ETf(nm_path_sb_e2, "Failed to allocate send buffer for port @STR",
			pp->name);
		goto out;
	}
	memset(buf, 0xdd, len);
	mr = IBUD_CREATE_RSC(nm_path_sb_t88, mr, ibv_reg_mr, ((struct ibud_per_nic *)(pp->pn))->pd, buf, len,
		IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE);
	if (!mr) {
		N_ETf(nm_path_sb_e3, "Failed to register send buffer for port @STR",
			pp->name);
		goto free_b;
	}
	path->send_buf = buf;
	path->send_buf_len = len;
	((struct ibud_path *)path)->sbmr = mr;
	goto out;

free_b:
	nvmeibt_bm_free_buffer(buf);

out:
	PFOUT;
	return buf;
}

void nvmeibt_nm_hw_path_release_send_buffer(struct nvmeibt_nm_path *path)
{

	PFIN;
	if (path && path->send_buf) {
		nvmeibt_bm_free_buffer(path->send_buf);
		path->send_buf = NULL;
		path->send_buf_len = 0;
		if (((struct ibud_path *)path)->sbmr) {
			IBUD_DESTROY_RSC(nm_dprsb_t88, ((struct ibud_path *)path)->sbmr, ibv_dereg_mr);
		}
	}
	PFOUT;
}

static void free_wridkt(struct nvmeibt_nm_linkable *l)
{
	void *v = kt2wrid(l2kt(l));
	NNVMEIBT_TOMA_FREE(nm_free_wridkt_t1, v);
}

/* TODO:NM Extract to nvmeib_nm ??? */
static struct nvmeibt_nm_hash_wrid_key_type * get_wrid(struct nvmeibt_nm_path *path)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_hash_wrid_key_type *wrid;

	if (!XDLIST_EMPTY(&path->wrids)) {
		l = XDLIST_FIRST(&path->wrids);
		XDLIST_DEL(&l->link);
	}
	else
		l = NULL;
	if (l)
		wrid = kt2wrid(l2kt(l));
	else
		wrid = NNVMEIBT_TOMA_MALLOC(nm_get_wrid_t1, sizeof(*wrid));
	if (!wrid) {
		N_ETf(nm_get_wrid_e1, "Failed to allocate wrid for send");
		goto out;
	}
	wrid->base.base.free = free_wridkt;
	wrid->base.base.type = kt_wrid;
	wrid->base.guid = ++path->pp->pn->local_node->guid;
	wrid->path_guid = path->cmt.base.guid;
	wrid->signaled = 1;

out:
	return wrid;
}


int nvmeibt_nm_hw_path_send_msg(struct nvmeibt_nm_path *path, struct nvmeibt_wire_msg *wire, uint16_t msg_id, bool signal) {
	struct ibud_path *ibud_path = (struct ibud_path *)path;
	struct nvmeibt_nm_per_port *pp = path->pp;
	struct ibud_per_port *ibud_port = (struct ibud_per_port *)pp;
	struct nvmeibt_nm_hash_wrid_key_type *wrid;
	struct ibv_sge sge;
	struct ibv_send_wr wr;
	struct ibv_send_wr *bad_wr;
	int rv = -1;

	if (!(wrid = get_wrid(path))) {
		N_Tf(nm_lsm_e3, "Path @PATH, failed to allocate wrid object", path->name);
		rv = -EINVAL;
		goto out;
	}
	else
		wrid->signaled = signal;
	if (wire->len > pp->mtu) {
			N_ETf(nm_lsm_e33, "Path @PATH, invalid msg size @LEN for "
		"UD QP (> MTU @MTU)", path->name, wire->len, pp->mtu);
		rv = -EMSGSIZE;
		goto out;
	}
	sge.addr = (uint64_t)wire->buffer;
	sge.length = wire->len;
	sge.lkey = ibud_path->sbmr->lkey;
	wrid->type = IBT_WR_SEND_MSG;
	wrid->sender_index = wire->send_ctx_idx;
	wr.wr_id = wrid->base.guid;
	wr.sg_list = &sge;
	wr.num_sge = 1;
	wr.opcode = IBV_WR_SEND;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.next = NULL;

	wr.wr.ud.ah = ibud_path->ah;
	wr.wr.ud.remote_qpn = ibud_path->remote_qp_num;
	wr.wr.ud.remote_qkey = ibud_path->remote_qkey;
	if ((rv = ibv_post_send(((struct ibud_per_port *)pp)->qp, &wr, &bad_wr))) {
			N_Tf(nm_lsm_e4, "Path @PATH, ail to send msg_id=@MSG_ID to listener, "
		"qp=@QP_NUM, ibv_post_send_rv=@IBV_POST_SEND_RV, "
		"wr=@WR, bad_wd=@BAD_WD, @STATE, RTS=@RTS", path->name,
		msg_id, ibud_port->qp->qp_num, rv, &wr, bad_wr, ibud_port->qp->state,
		ibud_port->qp->state == IBV_QPS_RTS);
		XDLIST_ADD_TAIL(&path->wrids, &wrid->base.base);
		rv = -1;
		goto out;
	}
	else
			N_Df(nm_lsm_d1, "Path @PATH, ent msg_id=@MSG_ID to "
		"@REMOTE_GUID_STR@@NODE_NAME, qp=@QP_NUM (remote qpn=@QPN), "
		"addr=@ADDR_PTR length @LENGTH_INT wr.wr_id=@WR_ID signal=@SIGNAL",
		path->name, msg_id, path->ra->guid, path->ra->rn->name,
		ibud_port->qp->qp_num, ibud_path->remote_qp_num, (void *)sge.addr, sge.length,
		(uint64_t)wr.wr_id, signal);
	nvmeibt_nm_add_key(path->ra->ln, &wrid->base.base, wrid->base.guid,
		"wrid @PTR - send_msg", wrid);
	
	rv = 0;
	goto out;

out:
	return rv;
}

int nvmeibt_nm_hw_send_ping(struct nvmeibt_nm_path *path, int is_response, uint8_t ping_id, int retry_count)
{
	struct ibv_send_wr wr = {0}, *bad_wr;
	struct nvmeibt_nm_hash_wrid_key_type *wrid;
	union nvmeibt_nm_ping_imm_data v;
	struct nvmeibt_nm_per_port *pp __attribute__((unused)) = path->pp;
	struct ibud_per_port * ibud_port = (struct ibud_per_port *)pp;
	int rv = -1;
	struct ibud_path *ibud_path = (struct ibud_path *)path;

	/* TODO:NM First check here if need to create cm ... per Alex req */
	//NFIN;
	if (!(wrid = get_wrid(path))) {
		N_Tf(nm_lsm_e31, "Failed to allocate wrid object");
		rv = -EINVAL;
		goto out;
	}
	v.raw = 0;
	v.fields.srm_id_lsb = (uint16_t)path->remote_srm_id;
	v.fields.ping_id = ping_id;
	v.fields.retry_count = retry_count;
	v.fields.resp = is_response ? 1 : 0;
	wr.imm_data = htobe32(v.raw);
	wr.wr_id = wrid->base.guid;
	wr.opcode = IBV_WR_SEND_WITH_IMM;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.ud.ah = ibud_path->ah;
	wr.wr.ud.remote_qpn = ibud_path->remote_qp_num;
	wr.wr.ud.remote_qkey = ibud_path->remote_qkey;
	if ((rv = ibv_post_send(ibud_port->qp, &wr, &bad_wr))) {
		N_Tf(nm_sp_e1, "Fail to send ping_msg=@MSG_ID, qp=@QP_NUM, "
			"ibv_post_send_rv=@IBV_POST_SEND_RV, "
			"wr=@WR, bad_wd=@BAD_WD, state=@STATE, RTS=@RTS",
			(unsigned long)path->ping_id, ibud_port->qp->qp_num, rv, &wr, bad_wr,
			ibud_port->qp->state, ibud_port->qp->state == IBV_QPS_RTS);
		XDLIST_ADD_TAIL(&path->wrids, &wrid->base.base);
		rv = -1;
		goto out;
	}
	else {
			N_Df(nm_sp_d1, "Path @PATH sent ping @STR: ping_id=@MSG_ID, "
		"qp=@QP_NUM (remote qpn=@QPN), remote_qkey @UINT",
		path->name, is_response ? "reply" : "request",
		(unsigned long)path->ping_id,
		ibud_port->qp->qp_num, ibud_path->remote_qp_num, wr.wr.ud.remote_qkey);
	}

	getnstimeofday_boot(&(path->ping_send_timespec));
	wrid->type = IBT_WR_SEND_PING;
	wrid->sender_index = 0;
	wrid->response = is_response;
	nvmeibt_nm_add_key(path->ra->ln, &wrid->base.base, wrid->base.guid,
		"wrid @PTR - send_ping", wrid);
	//path->pp->pn->ln->renew_status = 1;
	rv = 0;
	goto out;

out:
	return rv;

}

int nvmeibt_nm_hw_try_connect_path(struct nvmeibt_nm_path *path, bool first) {
	struct ibud_path *ibud_path = (struct ibud_path *)path;
	struct rdma_conn_param conn_param;
	int rv;
	(void) first;

	NFIN;
	if (!nvmeibt_nic_is_roce(path->pp->transport)) {
		((struct ibud_login_data *) path->payload)->slid = htobe16(((struct ibud_per_port *)(path->pp))->attr.lid);
		N_Df(nm_herr_d2136, "Path @PATH has slid @UINT ",
			path->name, ((struct ibud_per_port *)(path->pp))->attr.lid);
	}

	fill_conn_params(&conn_param);
	conn_param.private_data = path->payload;
	conn_param.private_data_len = sizeof(struct ibud_login_data);

	if ((rv = rdma_connect(ibud_path->cmid, &conn_param)))
		N_Ef(nm_herr_d2135, "rdma connect failed - @PATH, errno=@AUTO_ERRNO", path->name);
	else
		N_Tf(nm_herr_d2134, "rdma connect done - @PATH", path->name);

	NFOUT;
	return rv;
}

const char * nvmeibt_nm_hw_get_pp_state(struct nvmeibt_nm_per_port *pp) {
	return ibv_port_state_str(((struct ibud_per_port *)pp)->attr.state);
}

void nvmeibt_nm_hw_free_port(struct nvmeibt_nm_per_port *pp) {
	struct ibud_per_port *ibud_port = (struct ibud_per_port *)pp;
	if (ibud_port->qp) {
		IBUD_DESTROY_RSC(nm_free_port_t88, ibud_port->qp, ibv_destroy_qp);
	}
	if (ibud_port->cm_id) {
		nvmeibt_nm_del_key(pp->pn->local_node, &pp->cmt.base.base, 1);
		/* must free the port cm_id so we recreate the listener 
		   we don't use the offloader here as we want to ensure cm deleted 
		   before we will call rdma_listen once again */
		IBUD_DESTROY_RSC(nm_free_port_t89, ibud_port->cm_id, rdma_destroy_id);
	}
}

void nvmeibt_nm_hw_free_nic(struct nvmeibt_nm_per_nic *pn) {
	struct ibud_per_nic *ibud_nic = (struct ibud_per_nic *)pn;

	if (ibud_nic->context)
		nvmeibt_nm_del_fd(pn->local_node, ibud_nic->context->async_fd);
	free_srq(ibud_nic);
	if (ibud_nic->cq) {
		nvmeibt_nm_del_key(pn->local_node, &ibud_nic->cqt.base.base, 1);
		IBUD_DESTROY_RSC(nm_free_nic_t88, ibud_nic->cq, ibv_destroy_cq);
	}
	if (ibud_nic->comp_ch) {
		nvmeibt_nm_del_fd(pn->local_node, ibud_nic->comp_ch->fd);
		IBUD_DESTROY_RSC(nm_free_nic_t89, ibud_nic->comp_ch, ibv_destroy_comp_channel);
	}
	if (ibud_nic->pd) {
		IBUD_DESTROY_RSC(nm_free_nic_t90, ibud_nic->pd, ibv_dealloc_pd);
	}
}

bool nvmeibt_nm_hw_should_revive_port(struct nvmeibt_nm_per_port *pp) {
	struct ibud_per_port *ibud_port = (struct ibud_per_port *)pp;
	struct ibud_per_nic *ibud_nic = (struct ibud_per_nic *)(pp->pn);
	bool rv = false;

	if (!ibud_nic) {
		N_ETf(nm_ibud_revive, "no nic set for port");
		goto out;
	}

	if (!ibud_nic->context || pp->pn->is_dead) {
		N_ETf(nm_ibud_revive2, "nic @STR is dead or doesn't have context", pp->pn->dev_name);
		goto out;
	}

	if (ibv_query_port(ibud_nic->context, pp->port_num, &ibud_port->attr)) {
		N_ETf(nm_revive3, "failed to query port @INT for device @STR",
			pp->port_num, ibud_nic->device->name);
		pp->is_dead = 1;
		goto out;
	}

	if (ibud_port->attr.state < IBV_PORT_ACTIVE) {
		N_ETf(nm_revive4,
			"port @INT on device @STR is not active - port's state @STR",
			pp->port_num, ibud_nic->device->name,
			print_port_state(ibud_port->attr.state));
		pp->is_dead = 1;
		goto out;
	}
	rv = true;

out:
	return rv;
}

void nvmeibt_nm_hw_free_local_node(struct nvmeibt_nm_local_node *ln) {
	struct ibud_local_node *ibud_ln = (struct ibud_local_node *)ln;

	process_defer(ibud_ln);
	free_offloader(ibud_ln->network_offload);

	if (ibud_ln->ch) {
		nvmeibt_nm_del_fd(ln, ibud_ln->ch->fd);
		IBUD_DESTROY_RSC(nm_free_rdma_t88, ibud_ln->ch, rdma_destroy_event_channel);
	}
}

void nvmeibt_nm_hw_wait_events(struct nvmeibt_nm_local_node *ln) {
	process_defer((struct ibud_local_node *)ln);
}

unsigned short nvmeibt_nm_hw_get_port(void) {
	return RDMA_CM_SERVICE_PORT;
}

int nvmeibt_nm_hw_srm_get_data_offset_size(void) {
	return 0;
}
