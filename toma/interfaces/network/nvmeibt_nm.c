#include "nvmeibt_global.h"
#include "nvmeibt_nm.h"
#include "nvmeibt_nm_hw_iface.h"
//#include "nvmeibt_nm_ibud.h"
#include "nvmeibt_ib_common.h"
#include "nvmeibt_debug.h"
#include "common_public/nvmeib_uuid_be.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"
#define STATUS_STR_INIT_SIZE 4096
#define MAX_FDS 1024
#define LOCAL_NODE_PERIODIC_TIMER_NS SEC_TO_NSEC(1)

#define XDLIST_INIT_ELEM(field) XDLIST_INIT_LINK(field, NULL)

static const int wait_first_ping = 0;
int64_t ibud_enable_periodic_traces = ENABLE_NETWORKING_PERIODIC_TRACES_DEFAULT;
int64_t udp_max_header_length = MAX_HEADER_LENGTH_DEFAULT;

static void print_status(struct nvmeibt_nm_local_node *ln);

#define CREATE_RSC_RV(name, r, ok, f, ...) \
	({ \
		int __rv__; \
		if ((__rv__ = f(__VA_ARGS__)) == ok) \
			NNVMEIBT_TOMA_REGISTER_RSC(name, r, sizeof(*r)); \
		__rv__; \
	})

#define DESTROY_RSC(name, r, f) \
	do { \
		NNVMEIBT_TOMA_UNREGISTER_RSC(name, r, sizeof(*(r))); \
		f(r); \
		(r) = NULL; \
	} while (0)

#ifndef AF_IB
#define AF_IB 27
#endif
char * __attribute__ ((unused)) nvmeibt_nm_tss(void *_a, char b[], int len)
{
	struct sockaddr_storage *a = _a;
	int family;
	const char *str = ((struct sockaddr *)a)->sa_family == 0? NULL : nvmeibt_sockaddr_get_inet_addr(a);
	if (str) {
		family = ((struct sockaddr *)a)->sa_family;
		if (family == AF_IB) {
			family = AF_INET6;
		}
		if (!inet_ntop(family, str, b, len)) {
			N_ETf(nm_tss_e1,
				"Failed to convert socket_addr to string @AUTO_ERRNO");
			snprintf(b, len, "%s", "UNKNOWN");
		}
	}
	else {
		N_Df(nm_tss_e2, "Failed to get socket_addr as a string, sa_family=0");
		snprintf(b, len, "%s", "UNKNOWN");
	}
	return b;
}

void nvmeibt_nm_mutex_lock(pthread_mutex_t *p)
{
    if (pthread_mutex_lock(p) != 0) {
		N_ETf(nm_mutex_lock_e1, "Failed to lock - @AUTO_ERRNO");
		abort();
    }
}

void nvmeibt_nm_mutex_release(pthread_mutex_t *p)
{
    if (pthread_mutex_unlock(p) != 0) {
		N_ETf(nm_mutex_unlock_e1, "Failed to unlock - @AUTO_ERRNO");
		abort();
    }
}

void nvmeibt_nm_free_linkable(struct nvmeibt_nm_linkable *l)
{
#if USE_PRINTF
	_Z("l=%p", l);
#else
	N_Df(nm_free_linkable_d1, "l @PTR", l);
#endif
}

static inline struct nvmeibt_nm_req * l2r(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_req, base);
}

static void put_request(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_req *r)
{
	//NFIN;
	nvmeibt_nm_mutex_lock(&ln->guard);
	if (r->base.type == nvmeibt_nm_request_rsrm_resend_acks)
		--ln->n_rsrm_resend_acks;
	else if (r->base.type == nvmeibt_nm_request_rsrm_send_timer)
		--ln->n_rsrm_send_timer;
	memset(r, 0, sizeof(struct nvmeibt_nm_req));
	XDLIST_ADD_TAIL(&ln->el_pool, &r->base);
	nvmeibt_nm_mutex_release(&ln->guard);
	//NFOUT;
}

struct nvmeibt_nm_remote_node * nvmeibt_nm_l2rn(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_remote_node, base);
}

static inline struct nvmeibt_nm_hash_key_type * __attribute__ ((unused)) l2kt(
	struct nvmeibt_nm_linkable *l)
{
	return (l ? container_of(l, struct nvmeibt_nm_hash_key_type, base) : NULL);
}

static inline struct nvmeibt_nm_hash_wrid_key_type * kt2wrid( struct nvmeibt_nm_hash_key_type *kt)
{
	return container_of(kt, struct nvmeibt_nm_hash_wrid_key_type, base);
}

struct nvmeibt_nm_remote_node * nvmeibt_nm_find_remote_node(struct nvmeibt_nm_local_node *ln, const union nvmeib_uuid *id)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_remote_node *rn = NULL;
	int found = false;

	NFIN;
	XDLIST_FOREACH(l, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(l);
		if (ARE_UUID_EQ(&rn->id, id)) {
			found = true;
			break;
		}
	}
	NFOUT;
	return found ? rn : NULL;
}

enum remote_node_event {
	REMOTE_NODE_ACCESSIBLE = 0,
	REMOTE_NODE_INACCESSIBLE,
};

static const char * remote_node_event_to_str(int event)
{
	switch (event) {
	case REMOTE_NODE_ACCESSIBLE: return "accessible";
	case REMOTE_NODE_INACCESSIBLE: return "inaccessible";
	default: return "unknown";
	}
}

static struct nvmeibt_nm_remote_node * get_remote_node(struct nvmeibt_nm_local_node *ln,
	struct nvmeibt_nm_add_nic_req *e)
{
	struct nvmeibt_nm_remote_node *rn = NULL;
	int found;

	NFIN;
	if (!(rn = nvmeibt_nm_find_remote_node(ln, &e->node_id))) {
		N_Tf(nm_grn_t10, "Allocating remote node @STR", e->remote_node);
		if ((rn = NNVMEIBT_TOMA_CALLOC(nm_grn_t1, 1, sizeof(*rn)))) {
			XDLIST_HEAD_INIT(&rn->addresses);
			XDLIST_HEAD_INIT(&rn->best_ready);
			XDLIST_INIT_ELEM(&rn->base.link);
			if (!nvmeibt_event_tracker_init(&rn->event_tracker, 10, remote_node_event_to_str)) {
				N_ETf(nm_grn_e2_xxx, "Failed to initialize event tracker");
				NNVMEIBT_TOMA_FREE(nm_grn_t1_xxx, rn);
				rn = NULL;
			}
		}
		found = 0;
	}
	else {
		N_Tf(nm_grn_t11, "Already have remote node @STR", rn->name);
		found = 1;
	}
	if (!rn) {
		N_ETf(nm_grn_e1, "Failed to get a remote node");
		goto out;
	}
	if (!found)
		snprintf(rn->name, sizeof(rn->name), "%s", e->remote_node);
	if (!rn->ln) {
		rn->ln = ln;
		rn->node = e->node;
		rn->id = e->node_id;
		rn->me = e->me;
		if (rn->me) {
			N_Tf(nm_grn_t1000, "Remote node @STR is me", rn->name);
			ln->local_node_id = e->node_id;
		}
		else {
			N_Tf(nm_grn_t1001, "Remote node @STR is NOT me (remote_node_id @UUID_LE, my_node_id @UUID_LE)",
				rn->name, &rn->id, &ln->local_node_id);
		}
		N_Tf(nm_test_me213, "Adding Remote @STR to Local node @UUID_LE(@PTR)", rn->name, &ln->local_node_id, ln);
		nvmeibt_nm_mutex_lock(&ln->guard);
		XDLIST_ADD_TAIL(&ln->remotes, &rn->base);
		nvmeibt_nm_mutex_release(&ln->guard);
	}

out:
	NFOUT;
	return rn;
}

struct nvmeibt_nm_remote_addr * nvmeibt_nm_l2ra(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_remote_addr, base);
}

struct nvmeibt_nm_remote_addr * nvmeibt_nm_find_remote_address_by_gid(
	struct nvmeibt_nm_remote_node *rn, union ibv_gid *gid)
{
	struct nvmeibt_nm_linkable *l;
	char buf[IB_GID_STR_SIZE];
	int found = false;

	format_gid(gid, buf);
	N_Df(nm_frabg_t1000, "Looking for gid @STR that belongs to node @STR",
		buf, rn->name);
	XDLIST_FOREACH(l, &rn->addresses) {
		format_gid(&nvmeibt_nm_l2ra(l)->gid, buf);
		N_Df(nm_frabg_d1000, "Comparing @STR", buf);
		if (!memcmp(nvmeibt_nm_l2ra(l)->gid.raw, gid->raw, sizeof(gid->raw))) {
			found = true;
			break;
		}
	}

	return found ? nvmeibt_nm_l2ra(l) : NULL;
}

struct nvmeibt_nm_remote_addr * nvmeibt_nm_find_node_remote_address_by_gid(
	struct nvmeibt_nm_per_nic *pn, union ibv_gid *gid)
{
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_remote_addr *ra = NULL;
	char gid_str[IB_GID_STR_SIZE];
	char b[TOMA_SOCKADDR_STRING_LEN];
	int found;

	memset(gid_str, 0, sizeof(gid_str));
	found = false;
	XDLIST_FOREACH(lrn, &pn->local_node->remotes) {
		if ((ra = nvmeibt_nm_find_remote_address_by_gid(nvmeibt_nm_l2rn(lrn), gid))) {
			found = true;
			break;
		}
	}
	if (!found) {
		format_gid_raw(gid->raw, gid_str);
		N_Tf(nm_frabg_e1, "Failed to find remote_address for gid @STR", gid_str);
		ra = NULL;
	}
	else
		N_Df(nm_frabg_d1, "Found remote_address @STR from node @STR",
			nvmeibt_nm_tss(&ra->a, b, sizeof(b)), ra->rn->name);

	return ra;
}

struct nvmeibt_nm_remote_addr * nvmeibt_nm_find_remote_address_by_uuid(
	struct nvmeibt_nm_remote_node *rn, const union nvmeib_uuid *id)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_remote_addr *ra = NULL;
	int found = false;

	NFIN;
	N_Tf(nm_frabu_t1000, "Looking for uuid @UUID_LE that belongs to node @STR", id, rn->name);
	XDLIST_FOREACH(l, &rn->addresses) {
		ra = nvmeibt_nm_l2ra(l);
		N_Df(nm_frabu_d1000, "Comparing @UUID_LE", &ra->id);
		if (ARE_UUID_EQ(&ra->id, id)) {
			found = true;
			break;
		}
	}
	NFOUT;
	return found ? ra : NULL;
}


static struct nvmeibt_nm_remote_addr * find_remote_address(
	struct nvmeibt_nm_remote_node *rn, const char *addr, const union nvmeib_uuid *id)
{
	struct nvmeibt_nm_remote_addr *ra;
	union ibv_gid gid;

	NFIN;
	ra = !nvmeibt_ib_common_device_uuid_str_to_raw(&gid, addr) ?
		nvmeibt_nm_find_remote_address_by_gid(rn, &gid) : NULL;
	/* We are called from the add_remote_nic and we did not find the
	 * remote_addresss by the GUID however add_remote_nic is also called when
	 * the GUID is modified so we must check the uuid of the NIC and if it
	 * exists we must delete it.
	 */
	if (!ra) {
		if ((ra = nvmeibt_nm_find_remote_address_by_uuid(rn, id))) {
			/* Modify NIC GUID */
			//del_remote_address(ra);
			ra = NULL;
		}
	}
	if (ra && id)
		ra->id = *id;
	NFOUT;
	return ra;
}

struct nvmeibt_nm_path * nvmeibt_nm_l2p(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_path, base);
}

static void announce_node_disconnected(struct nvmeibt_nm_path *path)
{
	int was_connected;

	PFIN;
	was_connected = path->state < nvmeibt_nm_ps_connected ? 0 : 1;
	nvmeibt_nm_set_path_state(path, nvmeibt_nm_ps_wait_start);
	if (was_connected) {
		--path->ra->rn->connected;
		if (path->ra->rn->connected == 0) {
			N_IMf(nm_and_t1, "Node @STR is NOT accessible", path->ra->rn->name);
			nvmeibt_event_tracker_add(&path->ra->rn->event_tracker, REMOTE_NODE_INACCESSIBLE);
		}
	}
	PFOUT;
}

void nvmeibt_nm_del_key(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_linkable *l, int free_mem)
{
	//NFIN_;
	N_Df(nm_del_key_t1, "@STR link @STR",
		free_mem ? "Freeing" : "Unlinking", l->name);
	XHASHTABLE_DEL(&ln->key_val, &l->link);
	if (free_mem) {
		if (l->free)
			l->free(l);
		else
			NNVMEIBT_TOMA_FREE(nm_del_key_val_t2, l);
	}
	//NFOUT_;
}

int create_timer(void)
{
	int fd;

	NFIN;
	if ((fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) < 0) {
		N_ETf(cm_ct_e1, "timerfd_create failed @AUTO_ERRNO");
		fd = -1;
	}
	NFOUT;
	return fd;
}

void free_timer(int *fd)
{
	NFIN;
	if (*fd > -1) {
		NNVMEIBT_CLOSE(free_timer_close_fd, *fd);
		*fd = -1;
	}
	NFOUT;
}

int cancel_timer(int *fd, struct itimerspec *its)
{
	int rv = 0;
	struct itimerspec ts;

	NFIN;
	if (!its ||
		(its->it_value.tv_sec ||
		 its->it_value.tv_nsec ||
		 its->it_interval.tv_sec ||
		 its->it_interval.tv_nsec)) {
		memset(&ts, 0, sizeof(ts));
		if ((rv = timerfd_settime(*fd, 0, &ts, NULL)) < 0) {
			N_ETf(cm_cancel_timer_e1, "Failed to  disarm timer_fd @AUTO_ERRNO");
			free_timer(fd);
		}
	}
	NFOUT;
	return rv;
}

static int set_timer(int *fd, struct itimerspec *its,
	int64_t first_ns, int64_t interval_ns)
{
	struct itimerspec it;
	int rv;

	NFIN;
	memset(&it, 0, sizeof(it));
	if (!cancel_timer(fd, its)) {
		memset(its, 0, sizeof(*its));
		its->it_value = timespec_from_nsec(first_ns);
		if (interval_ns) {
			its->it_interval = timespec_from_nsec(interval_ns);
		}
		N_Df(cm_st_d1,
			"Setting timer for first-shot of @LONG.@TIMESPEC_NS seconds and "
			"interval of @LONG.@TIMESPEC_NS",
			its->it_value.tv_sec, its->it_value.tv_nsec,
			its->it_interval.tv_sec, its->it_interval.tv_nsec);
		if ((rv = timerfd_settime(*fd, 0, its, NULL)) < 0) {
			N_ETf(cm_st_e1, "Failed to  arm timer_fd - @AUTO_ERRNO");
			NNVMEIBT_CLOSE(set_timer_close_fd, *fd);
		}
		else {
			timerfd_gettime(*fd, &it);
			N_Df(nm_st_d2, "next: @LONG.@TIMESPEC_NS, interval @LONG.@TIMESPEC_NS",
				it.it_value.tv_sec, it.it_value.tv_nsec,
				it.it_interval.tv_sec, it.it_interval.tv_nsec);
			rv = 0;
		}
	}
	else
		rv = -1;
	NFOUT;
	return rv;
}

int set_periodic(int *fd, struct itimerspec *its, int64_t ns)
{
	return set_timer(fd, its, ns, ns);
}

static int set_one_shot(
	int *fd, struct itimerspec *its, int64_t ns_from_now)
{
	return set_timer(fd, its, ns_from_now, 0);
}

int clear_timer_fd(int fd, int is_read, int is_write)
{
	uint64_t v;
	int rv = -1;

	if (!is_read || is_write) {
		N_ETf(nm_clear_timer_fd_e1,
			"OOPS: is_read @INT. is_write @INT", is_read, is_write);
		goto out;
	}
	do {
		rv = read(fd, &v, sizeof(v));
	} while (rv > 0 || (rv == -1 && errno == EINTR));
	if (rv != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
		N_ETf(nm_clear_timer_fd_e2, "Failed to read local_node_timer, "
			"rv @INT @AUTO_ERRNO", rv);
		goto out;
	}
	rv = 0;

out:
	return rv;
}

int nvmeibt_nm_del_fd(struct nvmeibt_nm_local_node *ln, int fd)
{
	int rv = 0;
	struct epoll_event ev;

	NFIN;
	memset(&ev, 0, sizeof(ev));
	if (fd > -1) {
		ev.data.fd = fd;
		rv = epoll_ctl(ln->epoll_fd, EPOLL_CTL_DEL, fd, &ev);
		memset(&ln->fds[fd], 0, sizeof(ln->fds[fd]));
	}
	NFOUT;
	return rv;
}

static void path_del_fd(struct nvmeibt_nm_path *path)
{
	PFIN;
	nvmeibt_nm_del_fd(path->ra->ln, path->timer_fd);
	memset(&path->its, 0, sizeof(path->its));
	PFOUT;
}


static void free_path(struct nvmeibt_nm_path *path)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_hash_wrid_key_type *wrid;

	PFIN;
	N_Tf(nm_free_path_t1000, "Freeing path @STR", path->name);
	announce_node_disconnected(path);
	if (!XDLIST_NULL(&path->base.link))
		XDLIST_DEL(&path->base.link);
	if (!XDLIST_NULL(&path->best_ready_link.link))
		XDLIST_DEL(&path->best_ready_link.link);
	path_del_fd(path);
	free_timer(&path->timer_fd);

	path->pp->pn->local_node->hw_func_tbl.free_path(path);
	if (path->login_fd > -1) {
		nvmeibt_nm_del_fd(path->pp->pn->local_node, path->login_fd);
		cancel_timer(&path->login_fd, &path->login_its);
		free_timer(&path->login_fd);
	}

	if (path->hashed) {
		nvmeibt_nm_del_key(path->ra->ln, &path->cmt.base.base, 1);
		path->hashed = 0;
	}
	while (!(XDLIST_EMPTY(&path->wrids))) {
		l = XDLIST_FIRST(&path->wrids);
		XDLIST_DEL(&l->link);
		wrid = kt2wrid(l2kt(l));
		NNVMEIBT_TOMA_FREE(nm_free_path_t1, wrid);
	}
	if (path->srm) {
		nvmeibt_srm_free(path->srm);
		path->srm = NULL;
	}
	if (path->payload) {
		NNVMEIBT_TOMA_FREE(nm_free_path_t3, path->payload);
		path->payload = NULL;
	}
	DESTROY_RSC(nm_free_path_t89, path->name, free);
	path->pp->pn->local_node->renew_status = 1;
	NNVMEIBT_TOMA_FREE(nm_free_path_t2, path);
	PFOUT;
}

#define LOCAL_ADDR_RESTART_TO MSEC_TO_NSEC(500)
int path_start_timer(struct nvmeibt_nm_path *path, int is_start)
{
	int64_t val_ns;
	int rv = -1;

	PFIN;
	cancel_timer(&path->timer_fd, NULL);
#if 0	// from here the code is legacy and kept as a reminder
	val_ns = ++path->version == 1 ? 0 : LOCAL_ADDR_RESTART_TO;
	if (!is_start)
		val_ns *= 2;
	val_ns += _rand_ms(1000);
#endif	// #if 0	// from here the code is legacy and kept as a reminder
	if (is_start) {
		val_ns = max(1LL, (int64_t)nvmeibt_raft_get_effective_heartbeat_timeout_ns() / N_PINGS_PER_RAFT_HEARTBEAT);
		if (set_one_shot(&path->timer_fd, &path->its, val_ns))
			goto out;
	}
	else {
		/* per Alexander request we will try that value */
		val_ns = max(1LL, (int64_t)nvmeibt_raft_get_effective_heartbeat_timeout_ns() * (N_PINGS_PER_RAFT_HEARTBEAT - 1) / N_PINGS_PER_RAFT_HEARTBEAT);
		if (set_periodic(&path->timer_fd, &path->its, val_ns))
			goto out;
	}
	rv = 0;
	goto out;

out:
	PFOUT;
	return rv;
}

struct nvmeibt_nm_path * nvmeibt_nm_restart_path(struct nvmeibt_nm_path *path)
{
	PFIN;
	NVMEIBT_PATH_COUNTER_INC(path, path_restarts);
	if (!path->not_ready_traced) {
		path->not_ready_traced = 1;
		N_Tf(nm_restart_path_t200, "Path @STR is NOT READY", path->name);
	}
	announce_node_disconnected(path);
	path->is_rtr = 0;
	path->is_rts = 0;
	nvmeibt_nm_set_path_state(path, nvmeibt_nm_ps_wait_start);
	/* remove frrom the best_ready list that is maintained
	*  in the remote node
	* */
	if (!XDLIST_NULL(&path->best_ready_link.link))
		XDLIST_DEL(&path->best_ready_link.link);
	path->ping_retry_counter = 0;
	path->ping_id = 0;
	path->is_sender = 0;
	path->last_received_ping_ns_UNUSED = 0;

	path->pp->pn->local_node->hw_func_tbl.restart_path(path);
	if (path->login_fd > -1) {
		cancel_timer(&path->login_fd, &path->login_its);
		free_timer(&path->login_fd);
	}


	if (path->hashed) {
		nvmeibt_nm_del_key(path->ra->ln, &path->cmt.base.base, 1);
		path->hashed = 0;
	}
	if (path->srm) {
		srm_terminate_all_works(path->srm);
		nvmeibt_srm_stop_all(path->srm);
	}
	if (!XDLIST_NULL(&path->base.link))
		XDLIST_DEL(&path->base.link);
	if (path_start_timer(path, 1)) {
		free_path(path);
		path = NULL;
	}
	else {
		XDLIST_ADD_TAIL(&path->ra->connecting, &path->base);
		path->cmt.base.guid = ++path->pp->pn->local_node->guid;
		path->cmt.base.base.type = kt_connect_cm;

		path->payload->sgid = path->pp->gid;
		pathtoln(path)->hw_func_tbl.set_conneting_path(path);
		if (path->renew_srm_id) {
			path->renew_srm_id = 0;
			if (++path->srm_id == 0)
                ++path->srm_id;
            path->remote_srm_id = 0;
		}
	}
    N_Tf(nm_restart_path_t2000, "Path @STR: srm @UINT", path->name, path->srm_id);
	PFOUT;
	return path;
}

void nvmeibt_nm_restart_port(struct nvmeibt_nm_per_port *pp);
static int handle_path(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct nvmeibt_nm_path *path = ctx;
	int rv = 0;
	(void) dry_tries;

	PFIN;
	//N_Tf(kckckck, "handle path @NAME - state = @INT", path->name, path->state);
	if (clear_timer_fd(path->timer_fd, is_read, is_write))
		goto out;

	if (path->pp->restart_needed) {
		nvmeibt_nm_restart_port(path->pp);
		path = NULL;
		goto out;
	}

	switch (path->state) {
	case nvmeibt_nm_ps_wait_start:
		cancel_timer(&path->timer_fd, NULL);
		if ((rv = pathtoln(path)->hw_func_tbl.resolve_path(path))) {
			N_Ef(handle_path_failed_resolved, "Path @STR failed resolved", path->name);
		}
		break;
	case nvmeibt_nm_ps_connected:
	case nvmeibt_nm_ps_wait_ping_ack:
		if (path->ping_retry_counter < QP_PING_UD_NUM_RETRIES) {
			if (path->ping_retry_counter) {
				NVMEIBT_PATH_COUNTER_INC(path, ping_retries);
			}
			if (pathtoln(path)->hw_func_tbl.send_ping(path, 0, path->ping_id, path->ping_retry_counter)) {
				nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_ping);
				nvmeibt_nm_restart_path(path);
			}
			else {
				getnstimeofday_boot(&(path->ping_send_timespec));
				nvmeibt_nm_set_path_state(path, nvmeibt_nm_ps_wait_ping_ack);
				++path->ping_retry_counter;
			}
		}
		else {
			N_Tf(nm_handle_path_e3, "Path @STR ping failed for @INT times",
				path->name, QP_PING_UD_NUM_RETRIES);
			nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_ping_retry);
			nvmeibt_nm_restart_path(path);
		}
		break;
	default:
		break;
	}

out:
	PFOUT;
	return rv;
}

static void * path_carrier_obj(const struct carrier *car)
{
	void *obj;
	switch (car->type) {
	/* TODO:NM Legacy?? not see why needed */
	case carrier_rdma:
	case carrier_udp:
		obj = car->ctx;
		break;
	default:
		obj = NULL;
		break;
	}
	return obj;
}

static const char * path_name(const struct carrier *car)
{
	struct nvmeibt_nm_path *path = path_carrier_obj(car);
	return path ? path->name : "";
}

static struct nvmeibt_srm * path_srm_obj(const struct carrier *car)
{
	struct nvmeibt_nm_path *path = path_carrier_obj(car);
	return path ? path->srm : NULL;
}

static void * path_alloc_msgs_buffer(const struct carrier *car)
{
	struct nvmeibt_nm_path *path = path_carrier_obj(car);
	return pathtoln(path)->hw_func_tbl.alloc_msgs_buffer(path);
}

static void path_release_send_buffer(const struct carrier *car)
{
	struct nvmeibt_nm_path *path = path_carrier_obj(car);
	pathtoln(path)->hw_func_tbl.path_release_send_buffer(path_carrier_obj(car));
}

static int path_send_msg(const struct carrier *car,
	struct nvmeibt_wire_msg *wire, uint16_t msg_id, bool signal)
{
	struct nvmeibt_nm_path *path = path_carrier_obj(car);
	int rv;

	if (!path) {
		N_ETf(nm_lsm_e1, "Carrier has no path context");
		rv = -1;
		goto out;
	}
	if (path->ra->ln->thr != pthread_self()) {
		N_ETf(nm_lsm_e3001, "Path @STR tries to send a message from "
			"thread @PTHREAD which is not the main networking thread @PTHREAD",
			path->name, pthread_self(), path->ra->ln->thr);
		nvmeibt_abort(ES_FATAL);
	}
	rv = pathtoln(path)->hw_func_tbl.path_send_msg(path, wire, msg_id, signal);

out:
	return rv;
}

static int create_path_srm(struct nvmeibt_nm_path *path)
{
	const struct carrier car = {
		.type = carrier_rdma,
		.max_chunk_size = pathtoln(path)->hw_func_tbl.get_max_chunk_size(path),
		.max_messages = SRM_MAX_MSG_NUM,
		.srm_data_offset = pathtoln(path)->hw_func_tbl.srm_get_data_offset_size(),
		{
			.ctx = path,
		},
		.conn_name = path_name,
		.carrier_obj = path_carrier_obj,
		.srm_obj = path_srm_obj,
		.alloc_msgs_buffer = path_alloc_msgs_buffer,
		.release_msgs_buffer = path_release_send_buffer,
		.send_msg = path_send_msg,
	};

	path->max_chunk_size = car.max_chunk_size;
	path->max_messages = car.max_messages;
	path->srm = nvmeibt_srm_create(&car);
	return path->srm ? 0 : -1;
	return 0;
}

static struct nvmeibt_nm_path * add_path(struct nvmeibt_nm_remote_addr *ra, struct nvmeibt_nm_per_port *pp)
{
	struct nvmeibt_nm_path *path = NULL;
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_path,
	};
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];

	PFIN;
	pp->pn->local_node->renew_status = 1;
	if (pp->transport == rtr_ib && (pp->broadcast_id != ra->broadcast_id)) {
		N_ETf(nm_add_path_e1003, "IB path with local pkey @PKEY and "
			"remote pkey @PKEY", pp->broadcast_id, ra->broadcast_id);
		goto out;
	}
	if (!(path = pp->pn->local_node->hw_func_tbl.allocate_path())) {
		N_ETf(nm_add_path_e1, "Failed to allocate path");
		goto out;
	}

	if (!(path->payload = pp->pn->local_node->hw_func_tbl.allocate_login_data())) {
		N_ETf(nm_add_path_e_loging_data, "Failed to allocate login data");
		goto free_path;
	}

	path->login_fd = -1;
	path->ra = ra;
	path->pp = pp;
	path->loopback = memcmp(
		pp->gid.raw, ra->gid.raw, sizeof(pp->gid.raw)) == 0;
	path->cmt.base.base.free = nvmeibt_nm_free_linkable;
	XDLIST_INIT_ELEM(&path->base.link);
	XDLIST_INIT_ELEM(&path->best_ready_link.link);
	nvmeibt_ib_common_rdma_gid2ip( &a, &path->ra->gid, pp->pn->local_node->hw_func_tbl.get_port(),
		path->ra->transport == rtr_ib, pp->broadcast_id);
	if (!CREATE_RSC_RV(nm_add_path_t88, path->name, 0, asprintf,
		&path->name, "%s->%s:%s",
		pp->name, path->ra->rn->name, nvmeibt_nm_tss(&a, b, sizeof(b)))) {
		N_ETf(nm_add_path_e11, "Failed to allocate path_name - @AUTO_ERRNO");
		goto free_login_data;
	}
	N_Tf(nm_add_path_t1000, "Allocating path @STR", path->name);
	if (create_path_srm(path))
		goto free_name;
	XDLIST_HEAD_INIT(&path->wrids);
	path->timer_fd = create_timer();
	if (path->timer_fd < 0)
		goto free_srm;
	pfd.ctx = path;
	pfd.fd = path->timer_fd;
	if (nvmeibt_nm_add_fd(path->ra->ln, &pfd))
		goto free_fd;
	path->srm_id = (uint32_t)lrand48();
	path->payload->node_id = path->ra->ln->local_node_id;

	/* Initialize path counters */
	nvmeibt_nm_path_counters_init(path);

	path = nvmeibt_nm_restart_path(path);
	goto out;

free_fd:
	free_timer(&path->timer_fd);

free_srm:
	nvmeibt_srm_free(path->srm);
	path->srm = NULL;

free_name:
	DESTROY_RSC(nm_add_path_t89, path->name, free);

free_login_data:
	NNVMEIBT_TOMA_FREE(nm_add_path_t13, path->payload);

free_path:
	NNVMEIBT_TOMA_FREE(nm_add_path_t12, path);
	path = NULL;

out:
	PFOUT;
	return path;
}

struct nvmeibt_nm_path * nvmeibt_nm_get_path(struct nvmeibt_nm_remote_addr *ra,
 struct nvmeibt_nm_per_port *pp, int force_restart, uint32_t remote_srm_id)
{
	struct nvmeibt_nm_path *path = NULL;
	struct nvmeibt_nm_linkable *l;
	int found = 0;

	NFIN;
	XDLIST_FOREACH(l, &ra->ready) {
		path = nvmeibt_nm_l2p(l);
		if (path->pp == pp) {
			if (force_restart && path->remote_srm_id != remote_srm_id) {
				nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_different_srm_id);
				nvmeibt_nm_restart_path(path);
			}
			found = 1;
			break;
		}
	}
	if (!found) {
		XDLIST_FOREACH(l, &ra->connecting) {
			path = nvmeibt_nm_l2p(l);
			if (path->pp == pp) {
				found = 1;
				break;
			}
		}
	}
	if (!found)
		path = add_path(ra, pp);
	NFOUT;
	return path;
}

static bool can_communicate(enum nvmeib_rdma_transport a, enum nvmeib_rdma_transport b) {
	if (a == rtr_ib)
		return a == b;
	if (a == rtr_multi)
		return a == b || b == rtr_tcp || b == rtr_roce;
	if (a == rtr_roce)
		return a == b || b == rtr_multi;
	if (a == rtr_tcp)
		return a == b || b == rtr_multi;
	return false;
}

static void add_port_path(struct nvmeibt_nm_remote_addr *ra, struct nvmeibt_nm_per_port *pp)
{
	bool ra_roce = nvmeibt_nic_is_roce(ra->transport);
	bool pp_roce = nvmeibt_nic_is_roce(pp->transport);

	NFIN;
	if (pp->is_dead || !pp->is_valid ||
		!(can_communicate(ra->transport, pp->transport)) ||
		(!ra_roce &&
			(ra->gid.global.subnet_prefix !=
			pp->gid.global.subnet_prefix))) {
		N_Ef(nm_app_d1, "pp->is_dead @STR, pp->is_roce @STR, ra->is_roce @STR pp->is_valid @STR",
			pp->is_dead ? "T" : "F", pp_roce ? "T" : "F",
			ra_roce ? "T" : "F", pp->is_valid? "T" : "F");
		if (!ra_roce)
			return;
			//N_Df(nm_app_d2, "ra->subnet @_X, pp->subnet @_X",
			//	ra->gid.global.subnet_prefix, pp->gid.global.subnet_prefix);
		/* TODO:NM check */
		// /remove_path(ra, pp);
	}
	else
		nvmeibt_nm_get_path(ra, pp, 0, 0);
	NFOUT;
}


static void add_paths(struct nvmeibt_nm_remote_addr *ra)
{
	struct nvmeibt_nm_local_node *ln = ra->ln;
	struct nvmeibt_nm_per_nic *pn;
	int i, j;

	NFIN;
	for (i = 0; i < ln->n_nics; ++i) {
		pn = ln->nics[i];
		if (!pn->allowed || pn->is_dead)
			continue;
		for (j = 0; j < pn->n_ports; ++j) {
			add_port_path(ra, pn->ports[j]);
		}
	}
	NFOUT;
}


static void del_remote_address(struct nvmeibt_nm_remote_addr *ra);
static struct nvmeibt_nm_remote_addr * add_remote_address(struct nvmeibt_nm_remote_node *rn,
	struct nvmeibt_nic *nic, const union nvmeib_uuid *id, const char *addr,
	union ibv_gid *gid, uint16_t broadcast_id)
{
	struct nvmeibt_nm_local_node *ln = rn->ln;
	struct nvmeibt_nm_remote_addr *ra, *iter_ra;
	int is_ib = nic->transport == rtr_ib;
	struct nvmeibt_nm_linkable *l;


	NFIN;
	if (!(ra = ln->hw_func_tbl.allocate_remote_addr())) {
		N_ETf(nm_ara_e1, "Failed to allocate remote_address object");
		goto out;
	}
	XDLIST_HEAD_INIT(&ra->connecting);
	XDLIST_HEAD_INIT(&ra->ready);
	XDLIST_INIT_ELEM(&ra->base.link);
	ra->nic = nic;
	if (id)
		ra->id = *id;
	if (addr) {
		snprintf(ra->guid, sizeof(ra->guid), "%s", addr);
		if (nvmeibt_ib_common_device_uuid_str_to_raw(&ra->gid, addr))
			goto free_ra;
	}
	else if (gid) {
		ra->gid = *gid;
		format_gid_raw(gid->raw, ra->guid);
	}
	else {
		N_ETf(nm_ara_e22, "remote address has no address or gid");
		goto free_ra;
	}
	nvmeibt_ib_common_rdma_gid2ip(
		&ra->a, &ra->gid, ln->hw_func_tbl.get_port(), is_ib, broadcast_id);
	ra->transport = nic->transport;
	ra->ln = ln;
	ra->rn = rn;
	ra->broadcast_id = broadcast_id;

	/* TODO:NM It's a bit hacky but Toma can't easily ask us to delete remote addr so
	   This way we ensure remove the old paths */
	XDLIST_FOREACH_SAFE(l, &rn->addresses) {
		iter_ra = nvmeibt_nm_l2ra(l);
		if (ARE_UUID_EQ(&iter_ra->id, &ra->id)) {
			del_remote_address(iter_ra);
			break;
		}
	}

	add_paths(ra);
	XDLIST_ADD_TAIL(&rn->addresses, &ra->base);
	goto out;

free_ra:
	NNVMEIBT_TOMA_FREE(nm_ara_t2, ra);

out:
	NFOUT;
	return ra;
}

static void add_port_paths(struct nvmeibt_nm_per_port *pp);
static void nvmeibt_nm_free_port(struct nvmeibt_nm_per_port *pp);

#define siw_iface_prefix "siw_"
void nvmeibt_nm_attach_port(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic) {
	struct nvmeibt_nm_per_port *pp;
	int nic_idx;
	char *lnic_name;
	struct nvmeibt_local_nic *lnic;
	union ibv_gid conf_gid;

	NFIN;
	lnic = nvmeib_hash_search_ascii_str(nvmeibt_global_get_global()->local_nics_hash_by_sw_gid_str, nic->from_config.sw_gid_str);
	if (!lnic) {
		N_Ef(attach_port_lnic_not_found, "Couldn't find lnic");
		goto out;
	}
	lnic_name = lnic->from_config.device_type;

	if (strncmp(lnic_name, siw_iface_prefix, sizeof(siw_iface_prefix) - 1) == 0)
		lnic_name += strlen(siw_iface_prefix);

	for (nic_idx=0; nic_idx<ln->n_nics; nic_idx++) {
		if (!strncmp(lnic_name, ln->nics[nic_idx]->dev_name, sizeof(ln->nics[nic_idx]->dev_name) - 1) ||
			!strncmp(lnic->from_config.device_network_name, ln->nics[nic_idx]->dev_name, sizeof(ln->nics[nic_idx]->dev_name) - 1))
			break;
	}

	if (nic_idx == ln->n_nics) {
		N_Ef(attach_port_no_lnic, "Couldn't find nic with name @STR", lnic_name);
		goto out;
	}
	if (ln->nics[nic_idx]->is_dead) {
		N_Tf(cthwk44, "ln->nics[@INT]=@STR is_dead, skipping", nic_idx, lnic_name);
		goto out;
	}

	/* Attach the nic if wasn't attached yet */
	if (!ln->nics[nic_idx]->allowed) {
		ln->hw_func_tbl.attach_nic(ln, nic_idx);
	}
	/* Now attach the port */
	pp = ln->nics[nic_idx]->ports[lnic->from_config.port -1];
	nvmeibt_ib_common_device_uuid_str_to_raw(&conf_gid, nic->from_config.sw_gid_str);
	if (pp->is_valid) {
		/* port is already attached, check if config was changed as toma may call us even if not */
		if (pp->mtu != lnic->from_config.mtu || memcmp(&pp->conf_gid, &conf_gid, sizeof(union ibv_gid)) ||
			pp->broadcast_id != nic->from_config.partition_key) {
				nvmeibt_nm_free_port(pp);
				pp->conf_gid = conf_gid;
				pp->broadcast_id = nic->from_config.partition_key;
			} else {
				N_Tf(attach_port_port_exists, "Port @STR already exsists with same configuration - skip", lnic_name);
				goto out;
			}
	} else {
		pp->conf_gid = conf_gid;
		pp->broadcast_id = nic->from_config.partition_key;
	}
	nvmeibt_ib_common_read_local_nics(&ln->lnd);
	pp->port_id = nic->from_config.id;
	ln->hw_func_tbl.create_port(pp);
	add_port_paths(pp);
	ln->renew_status = 1;

out:
	NFOUT;
}

static void add_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_add_nic_req *e)
{
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_remote_addr *ra;
	union ibv_gid gid;
	char b[TOMA_SOCKADDR_STRING_LEN];
	struct sockaddr_storage a;
	int is_ib = e->nic->transport == rtr_ib;

	NFIN;

	if (e->nic->transport < 0) {
		/* TODO:NM Toma bug?? */
		N_Ef(nm_ignore_no_transport, "Trying to add NIC @STR on node @STR with negative transport @INT",
			e->remote_nic, e->remote_node, e->nic->transport);
		return;
	}

	if (nvmeibt_ib_common_device_uuid_str_to_raw(&gid, e->remote_nic))
		N_Tf(nm_add_remote_nic_t1234, "Trying to add NIC @STR on node @STR",
			e->remote_nic, e->remote_node);
	else {
		nvmeibt_ib_common_rdma_gid2ip(
			&a, &gid, ln->hw_func_tbl.get_port(), is_ib, e->brodcast_id);
		N_Tf(nm_add_remote_nic_t1235, "Trying to add NIC @STR on node @STR is_ib = @INT, transport = @INT",
			 nvmeibt_nm_tss(&a, b, sizeof(b)), e->remote_node, is_ib, e->nic->transport);
	}
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Wf(nm_add_remote_w1, "No valid topology contaning my node");
		goto out;
	}

	if (!(rn = get_remote_node(ln, e))) {
		N_ETf(nm_arn_e1, "Failed to get a remote node");
		goto out;
	}

	if (e->me)
			nvmeibt_nm_attach_port(ln, e->nic);
	ra = find_remote_address(rn, e->remote_nic, &e->nic_id);
	if (!ra) {
		add_remote_address(rn, e->nic, &e->nic_id, e->remote_nic,
			NULL, e->brodcast_id);
	}
	else {
		N_Tf(nm_arn_tt21, "Address @STR already exists in remote node @STR "
			"so try adding missing paths",
			e->remote_nic, e->remote_node);
		add_paths(ra);
	}


out:
	NFOUT;
}

static inline struct nvmeibt_nm_path * b2p(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_path, best_ready_link);
}

static void handle_send_req(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_send_msg_req *e)
{
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_path *path;
	int found = 0;
	int srv_rv;

	NFIN;

	XDLIST_FOREACH(lrn, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(lrn);
		if (ARE_UUID_EQ(&rn->id, &e->node_id)) {
			found = 1;
			break;
		}
	}
	if (!found) {
		N_Tf(nm_hsr_t101, "No remote node that match node_id @UUID_LE", &e->node_id);
		goto call_err;
	}
	if (!XDLIST_EMPTY(&rn->best_ready)) {
		path = b2p(XDLIST_FIRST(&rn->best_ready));
		N_Tf(nm_hsr_t9876, "Best path to send message is @STR", path->name);
		if ((srv_rv = nvmeibt_srm_queue_req(path->srm, &e->req))) {
			N_Ef(handle_send_req_srm_err, "Failed to send queue srm request, running callback with an error @INT", srv_rv);
			goto call_err;
		}
		goto out;
	}
	else {
		N_Tf(nm_hsr_t3642, "remote_node @STR has no @STR", rn->name,
			XDLIST_EMPTY(&rn->addresses) ?
				"remote addresses" : "ready paths");
		goto call_err;
	}

call_err:
	if (e->req.cbs.send_c)
		e->req.cbs.send_c(e->req.arg, -1);

out:
	NFOUT;
}

static void del_remote_address(struct nvmeibt_nm_remote_addr *ra)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_path *path;
	char b[TOMA_SOCKADDR_STRING_LEN];

	NFIN;
	N_Tf(nm_dra_t1000, "Freeing remote address @STR of @STR",
		nvmeibt_nm_tss(&ra->a, b, sizeof(b)), ra->rn->name);
	ra->ln->renew_status = 1;
	if (!XDLIST_NULL(&ra->base.link))
		XDLIST_DEL(&ra->base.link);
	while (!XDLIST_EMPTY(&ra->connecting)) {
		l = XDLIST_FIRST(&ra->connecting);
		XDLIST_DEL(&l->link);
		path = nvmeibt_nm_l2p(l);
		free_path(path);
	}
	while (!XDLIST_EMPTY(&ra->ready)) {
		l = XDLIST_FIRST(&ra->ready);
		XDLIST_DEL(&l->link);
		path = nvmeibt_nm_l2p(l);
		free_path(path);
	}
	NNVMEIBT_TOMA_FREE(nm_dra_t1, ra);
	NFOUT;
}

static void del_remote_addresses(struct nvmeibt_nm_remote_node *rn)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_remote_addr *ra;

	NFIN;
	while (!XDLIST_EMPTY(&rn->addresses)) {
		l = XDLIST_FIRST(&rn->addresses);
		XDLIST_DEL(&l->link);
		ra = nvmeibt_nm_l2ra(l);
		del_remote_address(ra);
	}
	NFOUT;
}

static void free_remote_node(struct nvmeibt_nm_remote_node *rn)
{
	NFIN;
	N_Tf(nm_frn_t1000, "Freeing remote node @STR", rn->name);
	nvmeibt_nm_mutex_lock(&rn->ln->guard);
	if (!XDLIST_NULL(&rn->base.link))
		XDLIST_DEL(&rn->base.link);
	nvmeibt_nm_mutex_release(&rn->ln->guard);
	del_remote_addresses(rn);
	nvmeibt_event_tracker_free(&rn->event_tracker);
	NNVMEIBT_TOMA_FREE(nm_frn_t1, rn);
	NFOUT;
}


static void handle_del_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_del_nic_req *e)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_remote_addr *ra;
	int found = 0;
	union ibv_gid gid;
	char b[TOMA_SOCKADDR_STRING_LEN];
	struct sockaddr_storage a;
	int i,j;

	NFIN;

	N_Tf(nm_del_remote_nic_t12345, "Trying to del NIC @STR on node @STR(me=@BOOL)",
			e->remote_nic, e->remote_node, e->me);
	if (nvmeibt_ib_common_device_uuid_str_to_raw(&gid, e->remote_nic))
		N_Tf(nm_del_remote_nic_t1234, "Trying to del NIC @STR on node @STR",
			e->remote_nic, e->remote_node);
	else {
		nvmeibt_ib_common_rdma_gid2ip(
			&a, &gid, ln->hw_func_tbl.get_port(), e->transport == rtr_ib, e->broadcast_id);
		N_Tf(nm_del_remote_nic_t1235, "Trying to del NIC @STR on node @STR",
			nvmeibt_nm_tss(&a, b, sizeof(b)), e->remote_node);
	}
	N_Tf(nm_del_fsdfsd, "start comparing @UUID_LE @PTR", &ln->local_node_id, ln);
	XDLIST_FOREACH(l, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(l);
		N_Tf(nm_del_remote_nic_t12356, "Comparing @UUID_LE and @UUID_LE", &rn->id, &e->node_id);
		if (ARE_UUID_EQ(&rn->id, &e->node_id)) {
			found = 1;
			break;
		}
	}
	if (!found) {
		N_Tf(nm_drn_t1, "Failed to find remote node @UUID_LE", &e->node_id);
		goto out;
	}
	found = 0;
	XDLIST_FOREACH(l, &rn->addresses) {
		ra = nvmeibt_nm_l2ra(l);
		if (ARE_UUID_EQ(&ra->id, &e->nic_id)) {
			found = 1;
			break;
		}
	}
	if (found) {
		del_remote_address(ra);
		if (XDLIST_EMPTY(&rn->addresses))
			free_remote_node(rn);
	}

	if (e->me) {
		for (i=0; i<ln->n_nics; i++) {
			for (j=0; j<ln->nics[i]->n_ports; j++) {
				if (ARE_UUID_EQ(&(ln->nics[i]->ports[j]->port_id), &e->nic_id)) {
					nvmeibt_nm_free_port(ln->nics[i]->ports[j]);
					break;
				}
			}
		}
	}
	ln->renew_status = 1;

out:
	NFOUT;
}

static int handle_del_remote_node(
	struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_del_remote_node *e)
{
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_linkable *l;

	NFIN;
	XDLIST_FOREACH(l, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(l);
		if (ARE_UUID_EQ(&rn->id, &e->node_id)) {
			free_remote_node(rn);
			break;
		}
	}
	NFOUT;
	return 0;
}

static int handle_local_nic_changed(
	struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_local_nic_change_req *e)
{
	int allowed;
	int found;
	int i;
	int rv = 0;
	struct nvmeibt_nm_per_nic *pn;

	NFIN;
	ln->lnd.n_ib_nics = 0;
	ln->lnd.n_nics = 0;
	nvmeibt_ib_common_read_local_nics(&ln->lnd);
	/* on remove nics list may become empty */
	if (ln->lnd.n_nics <= 0 && e->add) {
		N_ETf(nm_hrnc_e1, "No configured RDMA NICs");
		rv = -1;
		goto out;
	};
	found = 0;
	for (i = 0; i < ln->n_nics; ++i) {
		if (!strncmp(ln->nics[i]->dev_name, e->dev_name,
				sizeof(ln->nics[i]->dev_name))) {
			pn = ln->nics[i];
			found = 1;
			break;
		}
	}
	if (!found) {
		N_ETf(nm_hrnc_e2, "No such ib_device @STR", e->dev_name);
		goto out;
	}
	allowed = nvmeibt_ib_common_ib_is_dev_allowed(&ln->lnd, pn->dev_name);
	if (pn->allowed) {
		if (!e->add)
			nvmeibt_nm_free_nic(pn);
		else if (allowed && pn->is_dead)
			ln->hw_func_tbl.attach_nic(ln, i);
	}
	else {
		if (allowed && e->add)
			ln->hw_func_tbl.attach_nic(ln, i);
	}

out:
	NFOUT;
	return rv;
}

static void remove_path(struct nvmeibt_nm_remote_addr *ra, struct nvmeibt_nm_per_port *pp)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_path *path = NULL;
	char b[TOMA_SOCKADDR_STRING_LEN];

	NFIN;
	N_Df(nm_remove_path_d2, "Remote address @STR", nvmeibt_nm_tss(&ra->a, b, sizeof(b)));
	pp->pn->local_node->renew_status = 1;
	XDLIST_FOREACH(l, &ra->connecting)
		if (nvmeibt_nm_l2p(l)->pp == pp) {
			path = nvmeibt_nm_l2p(l);
			break;
		}
	if (!path)
		XDLIST_FOREACH(l, &ra->ready)
			if (nvmeibt_nm_l2p(l)->pp == pp) {
				path = nvmeibt_nm_l2p(l);
				break;
			}
	if (path) {
		free_path(path);
	}
	NFOUT;
}


static void free_port_paths(struct nvmeibt_nm_per_port *pp)
{
	struct nvmeibt_nm_local_node *ln = pp->pn->local_node;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_linkable *lra;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_remote_addr *ra;

	NFIN;
	XDLIST_FOREACH_SAFE(lrn, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(lrn);
		XDLIST_FOREACH(lra, &rn->addresses) {
			ra = nvmeibt_nm_l2ra(lra);
			remove_path(ra, pp);
		}
	}
	NFOUT;
}

static void nvmeibt_nm_free_port(struct nvmeibt_nm_per_port *pp)
{

	NFIN;

	/* first we release the port from all nm internals so we can ignore */
	free_port_paths(pp);
	pp->pn->local_node->hw_func_tbl.free_port(pp);
	pp->is_valid = 0;
	pp->restart_needed = false;
	pp->pn->local_node->renew_status = 1;

	NFOUT;
}

static void add_port_paths(struct nvmeibt_nm_per_port *pp)
{
	struct nvmeibt_nm_local_node *ln = pp->pn->local_node;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_linkable *lra;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_remote_addr *ra;

	NFIN;
	XDLIST_FOREACH_SAFE(lrn, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(lrn);
		XDLIST_FOREACH(lra, &rn->addresses) {
			ra = nvmeibt_nm_l2ra(lra);
			add_port_path(ra, pp);
		}
	}
	NFOUT;
}

void nvmeibt_nm_restart_port(struct nvmeibt_nm_per_port *pp) {
	NFIN;
	nvmeibt_nm_free_port(pp);
	if (!pp->pn->local_node->hw_func_tbl.create_port(pp))
		add_port_paths(pp);

	NFOUT;
}

static void free_ports(struct nvmeibt_nm_per_nic *pn)
{
	struct nvmeibt_nm_per_port *pp;
	int i;

	NFIN;
	for (i = 0; i < pn->n_ports; ++i) {
		pp = pn->ports[i];
		if (!pp->is_dead)
			nvmeibt_nm_free_port(pp);
		NNVMEIBT_TOMA_FREE(nm_free_port_t2, pp);
	}
	NNVMEIBT_TOMA_FREE(nm_free_port_t1, pn->ports);
	pn->ports = NULL;
	pn->n_ports = 0;
	NFOUT;
}

void nvmeibt_nm_free_nic(struct nvmeibt_nm_per_nic *pn)
{
	NFIN;
	pn->local_node->renew_status = 1;

	free_ports(pn);

	pn->local_node->hw_func_tbl.free_nic(pn);

	pn->allowed = 0;
	pn->is_dead = 0;
	NFOUT;
}

static int handle_port_gid_changed(
	struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_gid_changed_req *e)
{
	//struct ibv_device *dev;
	struct nvmeibt_nm_per_nic *pn;
	struct nvmeibt_nm_per_port *pp;
	int allowed;
	int found;
	int i;
	int rv = 0;

	NFIN;
	ln->lnd.n_ib_nics = 0;
	ln->lnd.n_nics = 0;
	nvmeibt_ib_common_read_local_nics(&ln->lnd);
	if (ln->lnd.n_nics <= 0) {
		N_ETf(nm_hpgc_e1, "No configured RDMA NICs");
		rv = -1;
		goto out;
	};
	found = 0;
	/* TODO:NM Extract this as used by other functions as well */
	for (i = 0; i < ln->n_nics; ++i) {
		if (!strncmp(ln->nics[i]->dev_name, e->dev_name,
				sizeof(ln->nics[i]->dev_name))) {
			pn = ln->nics[i];
			found = 1;
			break;
		}
	}
	if (!found) {
		N_ETf(nm_hpgc_e2, "No such ib_device @STR", e->dev_name);
		goto out;
	}
	allowed = nvmeibt_ib_common_ib_is_dev_allowed(&ln->lnd, pn->dev_name);
	if (pn->allowed) {
		if (!allowed)
			nvmeibt_nm_free_nic(pn);
		else {
			for (i = 0; i < pn->n_ports; ++i) {
				pp = pn->ports[i];
				if (pp->port_num == e->port) {
					nvmeibt_nm_restart_port(pp);
					break;
				}
			}
		}
	}
	else {
		if (allowed)
			ln->hw_func_tbl.attach_nic(ln, i);
	}

out:
	NFOUT;
	return rv;
}

static int handle_cancel_remote_node_req(struct nvmeibt_nm_local_node *ln,
	struct nvmeibt_nm_cencel_remote_node_req *e)
{
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_remote_addr *ra;
	struct nvmeibt_nm_linkable *lra;
	struct nvmeibt_nm_path *path;
	struct nvmeibt_nm_linkable *lp;

	NFIN;
	XDLIST_FOREACH(lrn, &ln->remotes) {
		rn = nvmeibt_nm_l2rn(lrn);
		if (ARE_UUID_EQ(&rn->id, &e->node_id)) {
			XDLIST_FOREACH(lra, &rn->addresses) {
				ra = nvmeibt_nm_l2ra(lra);
				if (XDLIST_EMPTY(&ra->ready))
					continue;
				XDLIST_FOREACH(lp, &ra->ready) {
					path = nvmeibt_nm_l2p(lp);
					/* TODO:NM is_sender always 0 */
					if (path->is_sender) {
						nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_cancel_node_req);
						nvmeibt_nm_restart_path(path);
						goto out;
					}
				}
			}
			break;
		}
	}

out:
	NFOUT;
	return 0;
}

static int handle_ext_events(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct nvmeibt_nm_local_node *ln = ctx;
	nvmeibt_nm_l_list_t events;
	struct nvmeibt_nm_linkable *e;
	uint64_t v;
	int del;
	int rv;
	void *vv;
	(void) dry_tries;

	//NFIN;
	if (!is_read) {
		N_ETf(nm_handle_ext_events_e0, "Woke up but not for read, is_write @INT",
			is_write);
		ln->run = 0;
		goto out;
	}
	do {
		rv = read(ln->event_fd, &v, sizeof(v));
	} while (rv > 0 || (rv == -1 && errno == EINTR));
	if (rv != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
		N_ETf(nm_handle_ext_events_e1, "Failed to read external events");
		ln->run = 0;
		goto out;
	}
	XDLIST_HEAD_INIT(&events);
	nvmeibt_nm_mutex_lock(&ln->guard);
	XDLIST_SPLICE(&ln->el, &events);
	nvmeibt_nm_mutex_release(&ln->guard);
	while (!XDLIST_EMPTY(&events)) {
		e = XDLIST_FIRST(&events);
		XDLIST_DEL(&e->link);
		del = 1;
		if (e->type != nvmeibt_nm_request_rsrm_resend_acks &&
			e->type != nvmeibt_nm_request_rsrm_send_timer)
			N_Tf(nm_handle_ext_events_d1, "Ext event type @STR", nvmeibt_nm_r2str(e->type));
		switch (e->type) {
		case nvmeibt_nm_request_add_nic:
			add_remote_nic(ln, &l2r(e)->add_nic_req);
			break;
		case nvmeibt_nm_request_send_msg:
			handle_send_req(ln, &l2r(e)->send_msg_req);
			if (l2r(e)->send_msg_req.req.msg)
				NNVMEIBT_TOMA_FREE(nm_handle_ext_events_t1001, l2r(e)->send_msg_req.req.msg);
			break;
		case nvmeibt_nm_request_rsrm_resend_acks:
			rsrm_resend_acks();
			break;
		case nvmeibt_nm_request_rsrm_send_timer:
			rsrm_resend_timer();
			break;
		case nvmeibt_nm_request_del_nic:
			handle_del_nic(ln, &l2r(e)->del_nic_req);
			break;
		case nvmeibt_nm_request_del_remote_node:
			handle_del_remote_node(ln, &l2r(e)->del_remote_node);
			break;
		case nvmeibt_nm_request_local_nic_changed:
			handle_local_nic_changed(ln, &l2r(e)->local_nic_change_req);
			vv = l2r(e)->local_nic_change_req.dev_name;
			NNVMEIBT_TOMA_FREE(nm_handle_ext_events_t73, vv);
			break;
		case nvmeibt_nm_request_port_gid_changed:
			handle_port_gid_changed(ln, &l2r(e)->gid_change_req);
			vv = l2r(e)->gid_change_req.dev_name;
			NNVMEIBT_TOMA_FREE(nm_handle_ext_events_t71, vv);
			vv = l2r(e)->gid_change_req.gid_str;
			NNVMEIBT_TOMA_FREE(nm_handle_ext_events_t72, vv);
			break;
		case nvmeibt_nm_request_rsrm_faults:
			rsrm_faults_handle_fifo_comm();
			break;
		case nvmeibt_nm_request_cancel_req:
			handle_cancel_remote_node_req(ln, &l2r(e)->cacnel_remote_node_req);
			break;
		case nvmeibt_nm_request_stop:
			ln->run = 0;
		default:
			break;
		}
		if (del)
			put_request(ln, l2r(e));
	}

out:
	rv = ln->run ? 0 : -1;
	//NFOUT;
	return rv;
}

static int create_poll(struct nvmeibt_nm_local_node *ln)
{
	int rv;

	NFIN;
	if ((rv = ln->epoll_fd = epoll_create1(0)) < 0) {
		N_ETf(cm_create_poll_e1, "Failed to create epolli - @AUTO_ERRNO");
		rv = -1;
		goto out;
    }
	else {
		rv = sysconf(_SC_OPEN_MAX);
		if (rv < 0)
			rv = MAX_FDS;
		if (!(ln->fds = NNVMEIBT_TOMA_CALLOC(
			nm_create_poll_t1, rv, sizeof(*ln->fds)))) {
			N_ETf(cm_create_poll_e2, "Failed to allocate FD array");
			rv = -1;
			NNVMEIBT_CLOSE(create_poll_free_epoll_fd_a, ln->epoll_fd);
			goto out;
		}
		if (!(ln->events = NNVMEIBT_TOMA_CALLOC(
			cm_create_poll_t2, rv, sizeof(*ln->events)))) {
			N_ETf(cm_create_poll_e3, "Failed to allocate eventts array");
			rv = -1;
			NNVMEIBT_TOMA_FREE(cm_create_poll_t88, ln->fds);
			ln->fds = NULL;
			NNVMEIBT_CLOSE(create_poll_free_epoll_fd_b, ln->epoll_fd);
			goto out;
		}
	}
	ln->n_fds = rv;
	rv = 0;

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_add_fd(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_per_fd *pfd)
{
	int rv = 0;
	struct epoll_event ev;

	NFIN;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP | EPOLLET;
	if (pfd->wait_write)
		ev.events |= EPOLLOUT;
	ev.data.fd = pfd->fd;
	rv = epoll_ctl(ln->epoll_fd, EPOLL_CTL_ADD, pfd->fd, &ev);
	if (!rv) {
		ln->fds[pfd->fd] = *pfd;
		ln->fds[pfd->fd].wait_read = 1;
	}
	NFOUT;
	return rv;
}

int nvmeibt_nm_mod_fd(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_per_fd *pfd)
{
	int rv = 0;
	struct epoll_event ev;

	NFIN;
	memset(&ev, 0, sizeof(ev));
	ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP | EPOLLET;
	if (pfd->wait_write)
		ev.events |= EPOLLOUT;
	ev.data.fd = pfd->fd;
	rv = epoll_ctl(ln->epoll_fd, EPOLL_CTL_MOD, pfd->fd, &ev);
	if (!rv) {
		ln->fds[pfd->fd] = *pfd;
	}
	NFOUT;
	return rv;
}

static void revive_dead_ports(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_per_nic *pn;
	struct nvmeibt_nm_per_port *pp;
	int i, j;

	for (i = 0; i < ln->n_nics; ++i) {
		pn = ln->nics[i];
		if (pn->allowed && pn->ports) {
			for (j = 1; j <= pn->n_ports; ++j) {
				pp = pn->ports[j - 1];
				if (pp->is_dead && ln->hw_func_tbl.should_revive_port(pp)) {
					N_Tf(nm_revive, "Reviving port @INT on @STR",
						pp->port_num, pp->pn->dev_name);
					nvmeibt_nm_restart_port(pp);
				}
			}
		}
	}
}

static void print_status_jdr(struct nvmeibt_nm_local_node *ln);
static int handle_ln_timer(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct nvmeibt_nm_local_node *ln = ctx;
	int  rv = 0;
	(void) dry_tries;

	NFIN;
	if (clear_timer_fd(ln->timer_fd, is_read, is_write))
		goto out;

	revive_dead_ports(ln);
	print_status(ln);
	print_status_jdr(ln);
	ln->renew_status = 0;

out:
	NFOUT;
	return rv;
}

static int create_ln_timer(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_ln_timer,
	};
	int64_t val;
	int rv = -1;

	NFIN;
	if ((ln->timer_fd = create_timer()) < 0) {
		N_ETf(cm_create_ln_timer_e1, "Failed to create timer");
		goto out;
	}
	val = LOCAL_NODE_PERIODIC_TIMER_NS;
	if (set_periodic(&ln->timer_fd, &ln->its, val))
		goto free_fd;
	pfd.ctx = ln;
	pfd.fd = ln->timer_fd;
	if (nvmeibt_nm_add_fd(ln, &pfd)) {
		rv = -1;
		goto free_fd;
	}
	rv = 0;
	goto out;

free_fd:
	NNVMEIBT_CLOSE(create_ln_timer_free_timer_fd, ln->timer_fd);

out:
	NFOUT;
	return rv;
}

static int handle_srm_timer(void *ctx, int is_read, int is_write, int dry_tries)
{
	(void) dry_tries;
#if USE_PRINTF
	_Z("ctx=%p, is_read=%d, is_write=%d", ctx, is_read, is_write);
#else
	N_Df(cm_handle_srm_timer_d1, "ctx @PTR, is_read @INT, is_write @INT",
		ctx, is_read, is_write);
#endif
	rsrm_resend_timer();
	return 0;
}

typedef enum {
	io_empty = 0,
	io_read = ((unsigned)EPOLLIN | (unsigned)EPOLLPRI),
	io_write = EPOLLOUT,
	io_close = ((unsigned)EPOLLHUP | (unsigned)EPOLLRDHUP),
	io_exception = EPOLLERR
} io_event_type;

#define MAX_FRAMES_TO_PROCESS 30
static int process_fds(struct nvmeibt_nm_local_node *ln, int n)
{
	int i, rv = 0, ret_g = 0;
	int is_read = 0;
	int is_write = 0;
	int is_close = 0;
	int fd;
	struct nvmeibt_nm_per_fd *pfd;

	/*NFIN;*/
	for (i = 0; i < n; ++i) {
		fd = ln->events[i].data.fd;
		//N_Df(process_fds_d1, "fd @INT, events @INT32_HEX",
		//	fd, ln->events[i].events);
		is_close = 0;
		is_write = 0;
		is_read = 0;
		if (ln->events[i].events & io_write) {
			//N_Df(process_fds_d2, "event is write fd @INT", fd);
			if (ln->fds[fd].wait_write)
				is_write = 1;
			else
				is_close = 1;
		}
		if (ln->events[i].events & io_read) {
			if (ln->fds[fd].wait_read)
				is_read = 1;
			else
				is_close = 1;
		}
		if ((ln->events[i].events & io_close) ||
			(ln->events[i].events & io_exception)) {
			N_Df(nm_process_fds_d4, "eevnt is @STR",
				(ln->events[i].events & io_close) ? "close" : "exception");
			is_close = 1;
			is_read = 0;
			is_write = 0;
		}
		pfd = &ln->fds[fd];
		if (pfd->f) {
			pfd->need_read = pfd->need_read || !!is_read;
			pfd->need_write = pfd->need_write || !!is_write;
			if (is_close) {
				nvmeibt_nm_del_fd(ln, fd);
				pfd->need_close = 1;
				pfd->need_read = 0;
				pfd->need_write = 0;
			}
		}
		else
			N_Tf(nm_process_fds_t1001, "Got an FD event on @INT which was "
				"already closed", fd);
	}

	/* Now run cb for ready fds */
	for (i = 0; i < ln->n_fds; ++i) {
		pfd = &ln->fds[i];
		if (pfd->f && (pfd->need_read || pfd->need_write || pfd->need_close)) {
			rv = pfd->f(pfd->ctx, pfd->need_read, pfd->need_write, MAX_FRAMES_TO_PROCESS);
			N_Df(nm_process_fds_rv, "FD @FD rv @INT need_read @INT need_write @INT", pfd->fd, rv, pfd->need_read, pfd->need_write);
			if (!rv) {
				pfd->need_write = pfd->need_read = 0;
				N_Df(nm_process_fds_drained, "FD: @FD drained", pfd->fd);
			}
			else if (rv == ETIMEDOUT) {
				ret_g = ETIMEDOUT;
				N_Tf(nm_process_fds_not_drain, "FD: @FD not drain yet - stay in poll", pfd->fd);
			}
			else {
				N_Df(nm_process_fds_drained_rv, "FD @FD rv @INT", pfd->fd, rv);
				ret_g = rv;
				break;
			}
		}
	}

	/*NFOUT;*/
	return ret_g;
}

#define POLL_SLEEP_MS 100
static int wait_events(struct nvmeibt_nm_local_node *ln)
{
	int cont = 1;
	int n, rv = 0, last_loop_timedout = false;

	NFIN;
	while (cont) {
		n = epoll_wait(ln->epoll_fd, &ln->events[0], ln->n_fds,
			 last_loop_timedout? 0 : POLL_SLEEP_MS);
		if (n >= 0) {
			rv = process_fds(ln, n);
			last_loop_timedout = (rv == ETIMEDOUT);
			if (rv && !last_loop_timedout)
				goto leave;
			else {
				cont = 1;
				ln->hw_func_tbl.wait_events(ln);
			}
			rsrm_resend_acks();
		}
		else {
leave:
			cont = 0;
			rv = -1;
		}
	}
	NFOUT;
	return rv;
}

static inline struct nvmeibt_nm_toma_req * e2tr(struct nvmeibt_nm_linkable *l)
{
	return container_of(l, struct nvmeibt_nm_toma_req, base);
}

static void free_pools(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_toma_req *m;
	struct nvmeibt_nm_req *n;

	NFIN;
	while (!(XDLIST_EMPTY(&ln->toma_request_pool))) {
		l = XDLIST_FIRST(&ln->toma_request_pool);
		XDLIST_DEL(&l->link);
		m = e2tr(l);
		NNVMEIBT_TOMA_FREE(nm_free_pool_t1, m);
	}
	while (!(XDLIST_EMPTY(&ln->toma_requests))) {
		l = XDLIST_FIRST(&ln->toma_requests);
		XDLIST_DEL(&l->link);
		m = e2tr(l);
		if (l->type == nvmeibt_nm_tr_process_recv)
			NNVMEIBT_BM_FREE(nm_free_pool_t2, m->msg);
		NNVMEIBT_TOMA_FREE(nm_free_pool_t3, m);
	}
	while (!(XDLIST_EMPTY(&ln->el_pool))) {
		l = XDLIST_FIRST(&ln->el_pool);
		XDLIST_DEL(&l->link);
		n = l2r(l);
		NNVMEIBT_TOMA_FREE(nm_free_pool_t10, n);
	}
	while (!(XDLIST_EMPTY(&ln->el))) {
		l = XDLIST_FIRST(&ln->el);
		XDLIST_DEL(&l->link);
		n = l2r(l);
		NNVMEIBT_TOMA_FREE(nm_free_pool_t11, n);
	}
	NFOUT;
}

static void free_remote_nodes(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_linkable *l;

	NFIN;
	while (!XDLIST_EMPTY(&ln->remotes)) {
		l = XDLIST_FIRST(&ln->remotes);
		XDLIST_DEL(&l->link);
		free_remote_node(nvmeibt_nm_l2rn(l));
	}
	NFOUT;
}

static void free_poll(struct nvmeibt_nm_local_node *ln)
{
	NFIN;
	if (ln->fds) {
		NNVMEIBT_TOMA_FREE(nm_free_poll_t1, ln->fds);
		ln->fds = NULL;
		ln->n_fds = 0;
	}
	if (ln->events) {
		NNVMEIBT_TOMA_FREE(nm_free_poll_t2, ln->events);
		ln->events = NULL;
	}
	NNVMEIBT_CLOSE(free_pool_close_epoll_fd, ln->epoll_fd);
	NFOUT;
}


static void free_key_val(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_linkable *l;

	NFIN;
	XHASHTABLE_FOR_EACH_SAFE(l, &ln->key_val) {
		N_Df(nm_free_key_val_d1, "l @PTR", l);
		nvmeibt_nm_del_key(ln, l, 1);
	}
	NFOUT;
}

static void free_nics(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_per_nic *pn;
	int i;

	NFIN;
	for (i = 0; i < ln->n_nics; ++i) {
		pn = ln->nics[i];
		if (pn->allowed && !pn->is_dead)
			nvmeibt_nm_free_nic(pn);
	}
	NFOUT;
}

static void free_local_node(struct nvmeibt_nm_local_node *ln)
{
	int rsrm_fd_timer;
	NFIN;

	free_pools(ln);

	if (ln->nics) {
		free_nics(ln);
		/* We allocate 1 buffer per all nics */
		NNVMEIBT_TOMA_FREE(nm_free_rdma_t1, ln->nics[0]);
		NNVMEIBT_TOMA_FREE(nm_free_rdma_t2, ln->nics);
		ln->nics = NULL;
	}

	ln->hw_func_tbl.free_local_node(ln);

	free_remote_nodes(ln);
	free_poll(ln);
	free_key_val(ln);

	NNVMEIBT_CLOSE(free_local_node_close_event_fd, ln->event_fd);
	NNVMEIBT_CLOSE(free_local_node_close_timer_fd, ln->timer_fd);
	rsrm_fd_timer = rsrm_get_fd_timer();
	NNVMEIBT_CLOSE(free_local_node_close_srm_fd, rsrm_fd_timer);
	pthread_mutex_destroy(&ln->guard);
	NNVMEIBT_TOMA_FREE(nm_free_local_node_t1, ln->status_str1);
	NNVMEIBT_TOMA_FREE(nm_free_local_node_t1, ln->status_str2);
	NNVMEIBT_TOMA_FREE(nm_free_local_node_json_t1, ln->status_json_str1);
	NNVMEIBT_TOMA_FREE(nm_free_local_node_json_t1, ln->status_json_str2);
	NNVMEIBT_TOMA_FREE(nm_free_local_node_t3, ln);
	NFOUT;
}

static void * run(void *v)
{
	struct nvmeibt_nm_local_node *ln = v;

	NFIN;
	ln->run = 1;
	while (ln->run) {
		/* Start the event loop */
		wait_events(ln);
	}
	free_local_node(ln);
	NFOUT;
	return NULL;
}

static int start_thread(struct nvmeibt_nm_local_node *ln)
{
	pthread_attr_t attr;
	int rv;

	NFIN;
	if ((rv = pthread_attr_init(&attr)) != 0 ||
		(rv = pthread_create(&ln->thr, &attr, run, ln)) != 0) {
		N_ETf(nm_start_thread_e1, "Fail to create ibud thread - @AUTO_ERRNO");
		rv = -1;
	}
	else {
		pthread_setname_np(ln->thr, "ibud");
		N_Tf(nm_start_thread_good, "created a nw thread @PTHREAD", ln->thr);
		rv = 0;
	}
	N_Tf(nm_start_thread_good2, "created a nw thread @PTHREAD", ln->thr);
	NFOUT;
	return rv;
}

static struct nvmeibt_nm_local_node * create_local_node(struct nvmeibt_nm_hw_function_table *tbl)
{
	struct nvmeibt_nm_local_node *ln;
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_ext_events,
	};
	int i;

	NFIN;
	if (!(ln = tbl->allocate_local_node())) {
		N_ETf(nm_create_local_node_e1,
			"Failed to allocate local_node - @AUTO_ERRNO");
		goto out;
	}
	ln->hw_func_tbl = *tbl;
	ln->guid = 1024;
	if (!(ln->status_str1 =
		NNVMEIBT_TOMA_CALLOC(nm_cln_t1, 1, STATUS_STR_INIT_SIZE)) ||
		!(ln->status_str2 =
		NNVMEIBT_TOMA_CALLOC(nm_cln_t1, 1, STATUS_STR_INIT_SIZE)) ||
		!(ln->status_json_str1 =
		NNVMEIBT_TOMA_CALLOC(nm_cln_json_t1, 1, STATUS_STR_INIT_SIZE)) ||
		!(ln->status_json_str2 =
		NNVMEIBT_TOMA_CALLOC(nm_cln_json_t1, 1, STATUS_STR_INIT_SIZE))) {
		N_ETf(nm_create_local_node_e111,
			"Failed to allocate local_node status strs - @AUTO_ERRNO");
		goto free_ln;
	}
	ln->status_str1_size = ln->status_str2_size = STATUS_STR_INIT_SIZE;
	ln->status_json_str1_size = ln->status_json_str2_size = STATUS_STR_INIT_SIZE;
	ln->status_str = ln->status_str1;
	ln->status_json_str = ln->status_json_str1;
	if (pthread_mutex_init(&ln->guard, NULL) < 0) {
		N_ETf(nm_create_local_node_e2, "Failed to create guard - @AUTO_ERRNO");
		goto free_ln;
	}

	if (tbl->init_local_node(ln)) {
		N_ETf(nm_create_local_node_e5, "Failed to start hw init local node");
		goto free_hw_ln;
	}

	if (create_poll(ln))
		goto free_m;
	if ((ln->event_fd = eventfd(0, EFD_NONBLOCK)) < 0) {
		N_ETf(nm_create_local_node_e3,
			"Failed to create request_event_fd - @AUTO_ERRNO");
		goto free_fd;
	}
	pfd.ctx = ln;
	pfd.fd = ln->event_fd;
	if (nvmeibt_nm_add_fd(ln, &pfd)) {
		N_ETf(nm_create_local_node_e21,
			"Failed to register a watch external events");
		goto free_e;
	}
	if ((create_ln_timer(ln)) < 0) {
		N_ETf(nm_create_local_node_e211, "Failed to create timer");
		goto free_e;
	}
	pfd.f = handle_srm_timer;
	pfd.fd = rsrm_get_fd_timer();
	if (nvmeibt_nm_add_fd(ln, &pfd)) {
		N_ETf(nm_create_local_node_e23,
			"Failed to register a watch for srm timer");
		goto free_e;
	}
	if ((ln->wire_event_fd = eventfd(0, EFD_NONBLOCK)) < 0) {
		N_ETf(nm_create_local_node_e31,
			"Failed to create wire_event_fd - @AUTO_ERRNO");
		goto free_e;
	}

	XDLIST_HEAD_INIT(&ln->el);
	XDLIST_HEAD_INIT(&ln->el_pool);
	XDLIST_HEAD_INIT(&ln->remotes);
	XHASHTABLE_INIT(&ln->key_val);
	XDLIST_HEAD_INIT(&ln->toma_request_pool);
	XDLIST_HEAD_INIT(&ln->toma_requests);

	nvmeibt_ib_common_read_local_nics(&ln->lnd);
	tbl->hw_init(ln);
	tbl->init_nics(ln);
	for (i = 0; i < ln->n_nics; ++i) {
		N_Tf(cm_init_rdma_t1000, "Hdandling device @STR", ln->nics[i]->dev_name);
		ln->nics[i]->allowed = 0;
	}
	ln->renew_status = 1;

	if (start_thread(ln)) {
		N_ETf(nm_create_local_node_e4, "Failed to start nm thread");
		goto free_wire_fd;
	}

	goto out;

free_wire_fd:
	NNVMEIBT_CLOSE(create_local_node_close_wire_fd, ln->wire_event_fd);

free_e:
	NNVMEIBT_CLOSE(create_local_node_close_event_fd, ln->event_fd);

free_fd:
	free_poll(ln);

free_m:
	pthread_mutex_destroy(&ln->guard);

free_hw_ln:
	tbl->free_local_node(ln);

free_ln:
	if (ln->status_json_str2)
		NNVMEIBT_TOMA_FREE(cm_create_local_node_json_221, ln->status_json_str2);
	if (ln->status_json_str1)
		NNVMEIBT_TOMA_FREE(cm_create_local_node_json_222, ln->status_json_str1);
	if (ln->status_str2)
		NNVMEIBT_TOMA_FREE(cm_create_local_node_221, ln->status_str2);
	if (ln->status_str1)
		NNVMEIBT_TOMA_FREE(cm_create_local_node_222, ln->status_str1);
	NNVMEIBT_TOMA_FREE(cm_create_local_node_t223, ln);

out:
	NFOUT;
	return ln;
}

#include <dlfcn.h>
#include <linux/limits.h>
#include <libgen.h>
void *nvmeibt_nm_tracer_init(const char *lib_path)
{
	void *handle;
	struct _tracer *so_tracer_start;
	struct _tracer *so_tracer_end;

	N_Tf(j93k4908, "");
	handle = dlopen(lib_path, RTLD_LAZY);
	if (handle) {
		// Register tracer section from the loaded library
		so_tracer_start = (struct _tracer *)dlsym(handle, "__tracer_start");
		so_tracer_end = (struct _tracer *)dlsym(handle, "__tracer_end");

		if (so_tracer_start && so_tracer_end) {
			if (nvmeibt_debug_register_tracer_section(so_tracer_start, so_tracer_end) == 0) {
				N_Tf(nm_register_so_tracer_success, "Successfully registered tracer section from @STR", lib_path);
			} else {
				N_Wf(nm_register_so_tracer_failed, "Failed to register tracer section from @STR", lib_path);
			}
		} else {
			N_Ef(nm_no_tracer_symbols, "No tracer section symbols found in @STR (this is normal if the library has no traces)", lib_path);
		}
	} else {
		N_ETf(nm_no_hw_no_handle, "couldn't find handle to @STR (@AUTO_ERRNO) - @STR", lib_path, dlerror());
		nvmeibt_abort(ES_FATAL);
	}

	return handle;
}

struct nvmeibt_nm_local_node *nvmeibt_nm_init(void *handle)
{
	struct nvmeibt_nm_local_node *ln;
	struct nvmeibt_nm_hw_function_table hw_func_table = {0};
	void (*hw_fill_func) (struct nvmeibt_nm_hw_function_table *);

	*(void**)(&hw_fill_func) = dlsym(handle, "nvmeibt_nm_hw_fill_hw_function_table");
	if (hw_fill_func) {
		hw_fill_func(&hw_func_table);
	} else {
		N_ETf(nm_no_hw_fill_symbol, "couldn't find nvmeibt_nm_hw_fill_hw_function_table in lib");
		nvmeibt_abort(ES_FATAL);
	}

	ln = create_local_node(&hw_func_table);
	return ln;
}

struct nvmeibt_nm_req * nvmeibt_nm_get_req(void) {
	return NNVMEIBT_TOMA_CALLOC(nm_get_req, 1, sizeof(struct nvmeibt_nm_req));
}

static struct nvmeibt_nm_req * get_request_ex(
	struct nvmeibt_nm_local_node *ln, enum nvmeibt_nm_periodic_msg_type t, int *n)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_req *r = NULL;
	int alloc = 1;

	//NFIN;
	nvmeibt_nm_mutex_lock(&ln->guard);
	if (n) {
		if (t == nvmeibt_nm_pst_rsrm_resend_acks) {
			*n = ln->n_rsrm_resend_acks;
			if (*n < 3)
				++ln->n_rsrm_resend_acks;
			else
				alloc = 0;
		}
		else if (t == nvmeibt_nm_pst_rsrm_send_timer) {
			*n = ln->n_rsrm_send_timer;
			if (*n < 3)
				++ln->n_rsrm_send_timer;
			else
				alloc = 0;
		}
	}
	if (alloc && !XDLIST_EMPTY(&ln->el_pool)) {
		l = XDLIST_FIRST(&ln->el_pool);
		XDLIST_DEL(&l->link);
		r = l2r(l);
	}
	nvmeibt_nm_mutex_release(&ln->guard);
	if (alloc) {
		if (!r) {
			if (!(r = nvmeibt_nm_get_req()))
				N_ETf(nm_sgibudr_e1, "Failed to allocate request");
		}
	}
	//NFOUT;
	return r;
}

static int add_event(
	struct nvmeibt_nm_local_node *ln, int fd, nvmeibt_nm_l_list_t *l, struct nvmeibt_nm_linkable *e)
{
	uint64_t v = 1;
	int rv = 0;

	//NFIN;
	nvmeibt_nm_mutex_lock(&ln->guard);
	XDLIST_ADD_TAIL(l, e);
	rv = write(fd, &v, sizeof(v)) == sizeof(v) ? 0 : -1;
	nvmeibt_nm_mutex_release(&ln->guard);
	//NFOUT;
	return rv;
}


int nvmeibt_nm_add_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_add_nic_req *e;
	struct nvmeibt_topology *cur_topo;
	int me;
	int rv = 0;

	NFIN;
	if (!ln)
		goto out;
	if (!nvmeibt_topology_is_HW_config_functional()) {
		N_Wf(nm_add_remote_nic_w11, "No valid topology contaning nic");
		goto out;
	}
	if (!nic->its_node) {
		N_ETf(nm_add_remote_nic_e23, "Nic @UUID_LE has node node owner", &nic->from_config.id);
		rv = -1;
		goto out;
	}
	cur_topo = nvmeibt_global_get_global();
	me = ARE_UUID_EQ(&(nic->its_node->from_config.id), &(cur_topo->my_node->from_config.id));

	if (ARE_UUID_EQ(&ln->local_node_id, &nvmeib_uuid_null_val))
		ln->local_node_id = cur_topo->my_node->from_config.id;

	if ((r = get_request_ex(ln, 0, NULL))) {
		e = &r->add_nic_req;
		e->node = nic->its_node;
		e->node_id = nic->its_node->from_config.id;
		snprintf(e->remote_node, sizeof(e->remote_node), "%s",
			nic->its_node->from_config.name);
		e->me = me;
		e->nic = nic;
		e->nic_id = nic->from_config.id;
		snprintf(e->remote_nic, sizeof(e->remote_nic), "%s",
			nic->from_config.sw_gid_str);
		if (!me)
			e->me_id = cur_topo->my_node->from_config.id;
		e->transport = nic->transport;
		e->brodcast_id = nic->from_config.partition_key;
		r->base.type = nvmeibt_nm_request_add_nic;
		add_event(ln, ln->event_fd, &ln->el, &r->base);
	}
	else {
		N_ETf(nm_add_remote_node_e2, "Failed to allocate add_nic_request");
		rv = -1;
	}

out:
	NFOUT;
	return rv;
}

struct nvmeibt_nm_hash_key_type * find_key(struct nvmeibt_nm_local_node *ln, uint64_t key)
{
	struct nvmeibt_nm_linkable *l;
	int found = 0;

	//NFIN;
	XHASHTABLE_FOR_EACH_POSSIBLE(l, &ln->key_val, key)
		if (l2kt(l)->guid == key) {
			found = 1;
			break;
		}
	//NFOUT;
	return found ? l2kt(l) : NULL;
}

void nvmeibt_nm_add_key(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_linkable *l, uint64_t key,
	const char namefmt[], ...)
{
	va_list args;

	//NFIN;
	N_Df(nm_add_key_d1, "l @PTR", l);
	va_start(args, namefmt);
	vsnprintf(l->name, sizeof(l->name), namefmt, args);
	va_end(args);
	//N_Tf(nm_add_key_t1, "Adding link @STR", l->name);
	if (!l->free) {
		N_Tf(nm_add_key_t2, "link @PTR has no free callback", l);
	}
	XHASHTABLE_ADD(&ln->key_val, l, key);
	//NFOUT;
}

/* Adding the path to keyval so when connection is established we can verify path exists */
void nvmeibt_nm_path_resolve_wait(struct nvmeibt_nm_path *path) {
	nvmeibt_nm_set_path_state(path, nvmeibt_nm_ps_wait_addr);
	nvmeibt_nm_add_key(path->pp->pn->local_node, &path->cmt.base.base, path->cmt.base.guid,
		"Path %p - waiting for resolved", path);
	path->hashed = 1;
}

void nvmeibt_nm_path_modify_to_rtr(struct nvmeibt_nm_path *path)
{
	PFIN;
	path->not_ready_traced = 0;
	if (!path->is_rtr) {
		path->is_rtr = 1;
		nvmeibt_srm_allow_receive(path->srm, path->srm_id);
		N_Tf(nm_hee_t1001, "Path @STR is RTR", path->name);
	}
	path->pp->pn->local_node->renew_status = 1;
	PFOUT;
}

struct nvmeibt_nm_toma_req * get_toma_request(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_toma_req *r = NULL;

	//NFIN;
	nvmeibt_nm_mutex_lock(&ln->guard);
	if (!XDLIST_EMPTY(&ln->toma_request_pool)) {
		l = XDLIST_FIRST(&ln->toma_request_pool);
		XDLIST_DEL(&l->link);
		r = e2tr(l);
	}
	nvmeibt_nm_mutex_release(&ln->guard);
	if (!r) {
		if (!(r = NNVMEIBT_TOMA_CALLOC(nm_grm_t1, 1, sizeof(*r))))
			N_ETf(nm_grm_e1, "Failed to allocate toma_request");
	}
	//NFOUT;
	return r;
}

static void put_toma_request(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nm_toma_req *r)
{
	//NFIN;
	nvmeibt_nm_mutex_lock(&ln->guard);
	XDLIST_ADD_TAIL(&ln->toma_request_pool, &r->base);
	nvmeibt_nm_mutex_release(&ln->guard);
	//NFOUT;
}

static void make_path_node_default(
	struct nvmeibt_nm_remote_node *rn, struct nvmeibt_nm_path *path, int force)
{
	struct nvmeibt_nm_local_node *ln = rn->ln;
	struct nvmeibt_nm_toma_req *m;

	NFIN;
	if ((m = get_toma_request(ln))) {
		m->node_id = rn->id;
		m->key = path ? path->cmt.base.guid : 1;
		m->force = force;
		m->base.type = nvmeibt_nm_tr_node_key;
		add_event(ln, ln->wire_event_fd, &ln->toma_requests, &m->base);
	}
	NFOUT;
}

int nvmeibt_nm_process_toma_requests(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_linkable *l;
	struct nvmeibt_nm_toma_req *m;
	nvmeibt_nm_l_list_t ll;
	struct nvmeibt_node *node;
	uint64_t v;
	int rv = 0;

	NFIN;
	if (!ln)
		goto out;
	do {
		rv = read(ln->wire_event_fd, &v, sizeof(v));
	} while (rv > 0 || (rv == -1 && errno == EINTR));
	if (rv != -1 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
		N_ETf(nm_nrptr_e1356, "Failed to read TOMA events");
	}
	XDLIST_HEAD_INIT(&ll);
	nvmeibt_nm_mutex_lock(&ln->guard);
	XDLIST_SPLICE(&ln->toma_requests, &ll);
	nvmeibt_nm_mutex_release(&ln->guard);
	while (!XDLIST_EMPTY(&ll)) {
		l = XDLIST_FIRST(&ll);
		XDLIST_DEL(&l->link);
		m = e2tr(l);
		if (l->type == nvmeibt_nm_tr_process_recv)
			nvmeibt_toma_dispatch_received_msg(m->msg);
		else if (l->type == nvmeibt_nm_tr_node_key) {
			if (nvmeibt_topology_is_HW_config_functional()) {
				if ((node = nvmeibt_node_get_node_by_id(&m->node_id))) {
					/* if the node conn_ctx was nopt initialize yet set it now
					 */
					if (m->force || (unsigned long)node->conn_ctx <= 1)
						node->conn_ctx = (void *)m->key;
				}
			}
		}
		put_toma_request(ln, m);
	}

out:
	NFOUT;
	return rv;
}

static void path_modify_to_RTS(struct nvmeibt_nm_path *path)
{
	path->not_ready_traced = 0;
	if (!path->is_rts) {
		path->is_rts = 1;
		if (!XDLIST_NULL(&path->base.link))
			XDLIST_DEL(&path->base.link);
		XDLIST_ADD_TAIL(&path->ra->ready, &path->base);
		if (!XDLIST_NULL(&path->best_ready_link.link))
			XDLIST_DEL(&path->best_ready_link.link);
		/* add the best_ready list that the remote node maintains */
		XDLIST_ADD_TAIL(&path->ra->rn->best_ready, &path->best_ready_link);
		nvmeibt_srm_allow_send(path->srm, path->remote_srm_id);
		/* use NULL in the following to make the node ready for send.
		 * however it still does not set the default path.  the default path
		 * will be set on the first send from TOMA
		 */
		make_path_node_default(path->ra->rn, NULL, 0);
		N_Tf(nm_pmtrts, "Path @STR is RTS", path->name);
	}
}

static void announce_node_connected(struct nvmeibt_nm_path *path)
{
	int was_connected;

	PFIN;
	was_connected = path->state < nvmeibt_nm_ps_connected ? 0 : 1;
	nvmeibt_nm_set_path_state(path, nvmeibt_nm_ps_connected);
	if (!was_connected) {
		++path->ra->rn->connected;
		if (path->ra->rn->connected == 1) {
			N_IMf(nm_anc_t1, "Node @STR is accessible", path->ra->rn->name);
			nvmeibt_event_tracker_add(&path->ra->rn->event_tracker, REMOTE_NODE_ACCESSIBLE);
		}
	}
	PFOUT;
}

int nvmeibt_nm_connect_path(struct nvmeibt_nm_path *path) {
	int rv = -1;

	PFIN;
	cancel_timer(&path->login_fd, &path->login_its);
	path->renew_srm_id = 1;
	if (path_start_timer(path, 0)) {
		N_ETf(nm_hee_e6, "Failed to start ping timer");
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_ping_timer);
		nvmeibt_nm_restart_path(path);
		goto out;
	}
	path->ping_retry_counter = 0;
	nvmeibt_nm_path_modify_to_rtr(path);
	if (!wait_first_ping || path->loopback)
		path_modify_to_RTS(path);
	else
		path->is_rts = 0;
	nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_success);
	announce_node_connected(path);
	path->pp->pn->local_node->renew_status = 1;
	rv = 0;

	PFOUT;
out:
	return rv;
}

int64_t nvmeibt_raft_get_roof_leader_heartbeat_timeout_ns(void);

#define PING_EXCEPTIONAL_TO_STD_RATIO 10
void nvmeibt_nm_on_recv_ping(struct nvmeibt_nm_path *path, union nvmeibt_nm_ping_imm_data *v) {
	int64_t			ns_since_ping;
	struct nvmeib_iir	*all_ping_IIR = &(path->ra->rn->node->peer_statistics.ping_response_time_IIR);
	struct nvmeib_iir	*exceptional_ping_IIR = &(path->ra->rn->node->peer_statistics.ping_response_time_exceptional_IIR);

	if (v->fields.resp) {
		if (	(path->state == nvmeibt_nm_ps_wait_ping_ack &&
				 v->fields.srm_id_lsb == (uint16_t)path->srm_id &&
				 v->fields.ping_id == path->ping_id)) {
			getnstimeofday_boot(&(path->ra->rn->node->peer_statistics.last_ping_response_timespec));
			path->ping_retry_counter = 0;
			++path->ping_id;
			//path->pp->pn->ln->renew_status = 1;
			N_Df(nm_hprc_d1, "Path @STR Received valid ping reply",
				path->name);
			path_modify_to_RTS(path);
			ns_since_ping = timespec_diff_ns(path->ra->rn->node->peer_statistics.last_ping_response_timespec, path->ping_send_timespec);
			if (ns_since_ping < nvmeibt_raft_get_roof_leader_heartbeat_timeout_ns()) {
				nvmeib_iir_add_sample(all_ping_IIR, ns_since_ping);
				if (ns_since_ping > (nvmeib_iir_get_val(*all_ping_IIR) * PING_EXCEPTIONAL_TO_STD_RATIO +
									 nvmeib_iir_get_standard_deviation(*all_ping_IIR) * PING_EXCEPTIONAL_TO_STD_RATIO)) {
					if (nvmeib_iir_get_saturation_level(*all_ping_IIR) > 0.5) {
						nvmeib_iir_add_sample(exceptional_ping_IIR, ns_since_ping);
						NVMEIB_IIR_DUMP(vgsi85k, *exceptional_ping_IIR, ns_since_ping);
					}
				}
				_DILUTED_CMD(12000, NVMEIB_IIR_DUMP(d83knd4, *all_ping_IIR, ns_since_ping));
			} else {
				N_Df(unhwx2v, "Ignoring IIR @STR ns_since_ping=@INT64_TD", path->name, ns_since_ping);
			}
		}
		else {
			N_Ef(nm_hprc_t1, "Path @STR recived invalid ping reply: "
				"ping_id @INT (mine @INT), srm_id @INT (mine @INT)",
				path->name,
				(unsigned)v->fields.ping_id,
				(unsigned)path->ping_id,
				(unsigned)v->fields.srm_id_lsb,
				(unsigned)(uint16_t)path->srm_id);
			NVMEIBT_PATH_COUNTER_INC(path, invalid_ping_response);
		}
	}
	else {
		if (v->fields.srm_id_lsb == (uint16_t)path->srm_id) {
			N_Df(nm_hprc_d2, "Path @STR recived valid ping request: "
				"ping_id @INT, retry_count @INT", path->name,
				(unsigned)v->fields.ping_id,
				(unsigned)v->fields.retry_count);
			path->last_received_ping_ns_UNUSED = timespec_to_nsec(path->ra->rn->node->peer_statistics.last_ping_response_timespec);
			/* we are already in the best_ready list so we put
			 * ourself in the head
			 **/
			if (!XDLIST_NULL(&path->best_ready_link.link)) {
				XDLIST_DEL(&path->best_ready_link.link);
				XDLIST_ADD_HEAD(&path->ra->rn->best_ready,
					&path->best_ready_link);
			}
			if (pathtoln(path)->hw_func_tbl.send_ping(path, 1, v->fields.ping_id, v->fields.retry_count))
				N_ETf(nm_hprc_e1, "Path @STR failed to send ping response: "
					"ping_id @INT, retry_count @INT",
					path->name,
					(unsigned)v->fields.ping_id,
					(unsigned)v->fields.retry_count);
			getnstimeofday_boot(&(path->ping_send_timespec));
		}
	}
	//PFOUT_;
}


int nvmeibt_nm_add_wire_recv(struct nvmeibt_nm_local_node *ln, void *buf)
{
	struct nvmeibt_nm_toma_req *m;
	int rv = 0;

	NFIN;
	if (!ln)
		goto out;
	if (!(m = get_toma_request(ln))) {
		rv = -1;
		goto free_buf;
	}
	m->base.type = nvmeibt_nm_tr_process_recv;
	m->msg = buf;
	rv = add_event(ln, ln->wire_event_fd, &ln->toma_requests, &m->base);
	goto out;

free_buf:
	NNVMEIBT_BM_FREE(nm_nrawr_t1, buf);

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_queue_srm_req(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *remote_node,
	struct nvmeibt_msg_request *req)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_send_msg_req *e;
	void *msg;
	int rv;

	NFIN;
	if (!ln) {
		rv = 0;
		goto out;
	}

	if ((r = get_request_ex(ln, 0, NULL))) {
		e = &r->send_msg_req;
		if (req->msg_len) {
			if (!(msg = NNVMEIBT_TOMA_CALLOC(nm_queue_req_t2, 1, req->msg_len))) {
				N_ETf(nm_queue_request_e11, "Failed to allocate inline msg");
				rv = -1;
				goto free_e;
			}
			/* TODO:NM why we copy? */
			memcpy(msg, req->msg, req->msg_len);
		}
		else
			msg = NULL;
		e->node_id = remote_node->from_config.id;
		e->req = *req;
		e->key = (uint64_t)remote_node->conn_ctx;
		e->req.msg = msg;
		r->base.type = nvmeibt_nm_request_send_msg;
		add_event(ln, ln->event_fd, &ln->el, &r->base);
	}
	else {
		N_ETf(nm_queue_req_e2, "Failed to allocate send_msg_request");
		rv = -1;
	}
	rv = 0;
	goto out;

free_e:
	put_request(ln, r);

out:
	NFOUT;
	return rv;
}

void nvmeibt_nm_rsrm_resend_acks(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_req *r;
	int n = 0;

	//NFIN;
	if (ln) {
		if ((r = get_request_ex(ln, nvmeibt_nm_pst_rsrm_resend_acks, &n))) {
			r->base.type = nvmeibt_nm_request_rsrm_resend_acks;
			add_event(ln, ln->event_fd, &ln->el, &r->base);
		}
		else if (n < 3) {
			/* n < 3 means that we actually needed a message but failed */
			N_ETf(nm_nrard_e1, "Failed to allocate rsrm_resend_acks_request");
		}
	}
	//NFOUT;
}

int nvmeibt_nm_rsrm_send_timer(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_req *r;
	int n = 0;
	int rv;

	//NFIN;
	if (ln) {
		if ((r = get_request_ex(ln, nvmeibt_nm_pst_rsrm_send_timer, &n))) {
			r->base.type = nvmeibt_nm_request_rsrm_send_timer;
			rv = add_event(ln, ln->event_fd, &ln->el, &r->base);
		}
		else if (n < 3) {
			/* n < 3 means that we actually needed a message but failed */
			N_ETf(nm_nrtrd_e1, "Failed to allocate rsrm_send_timer_request");
			rv = -1;
		}
		else
			rv = 0;
	}
	else
		rv = 0;
	//NFOUT;
	return rv;
}


int nvmeibt_nm_get_fd(struct nvmeibt_nm_local_node *ln)
{
	return ln->wire_event_fd;
}

static int handle_reconnect(void *ctx, int is_read, int is_write, int dry_tries)
{
	struct nvmeibt_nm_path *path = ctx;
	int rv = 0;
	(void) dry_tries;

	PFIN;

	clear_timer_fd(path->login_fd, is_read, is_write);
	nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_conenct);
	nvmeibt_nm_restart_path(path);

	PFOUT;
	return rv;
}


int nvmeibt_nm_try_connect_path(struct nvmeibt_nm_path *path) {
	int rv = -1;
	char gid_str[IB_GID_STR_SIZE];
	struct nvmeibt_nm_per_fd pfd = {
		.f = handle_reconnect,
		.ctx = path,
	};

	path->payload->srm_id = htobe32(path->srm_id);
	path->payload->version = (union version)NVMEIBT_TN_PROTOCOL_VERSION;
	path->payload->sgid = path->pp->gid;
	format_gid_raw(path->pp->gid.raw, gid_str);
	path->payload->node_id = path->pp->pn->local_node->local_node_id;

	N_Tf(checker, "Trying to connect to connect gid @STR", gid_str);
	rv = pathtoln(path)->hw_func_tbl.try_connect_path(path, true);
	if (rv) {
					N_ETf(nm_herr_e2, "Failed to call connect "
			"(rv= @INT)- @AUTO_ERRNO", rv);
		nvmeibt_nm_path_set_last_error(path, nvmeibt_nm_ple_failed_conenct);
		nvmeibt_nm_restart_path(path);
			goto out;
	}
	else {
		nvmeibt_nm_set_path_state(path, nvmeibt_nm_ps_wait_connect);
		path->login_fd = create_timer();
		pfd.fd = path->login_fd;
		set_periodic(&path->login_fd, &path->login_its, 1000000000);
		nvmeibt_nm_add_fd(path->pp->pn->local_node, &pfd);
	}

out:
	return rv;
}

struct hash_listener_key_type * nvmeibt_nm_kt2cml(struct nvmeibt_nm_hash_key_type *kt)
{
	return container_of(kt, struct hash_listener_key_type, base);
}

struct nvmeibt_nm_per_port * nvmeibt_nm_cmt2pp(struct hash_listener_key_type *cmt)
{
	return container_of(cmt, struct nvmeibt_nm_per_port, cmt);
}

static int check_version(union version l, union version r)
{
	/* to avoid endianess issues we compare byte_by_bye */
	if (l.major > r.major)
		return 1;
	if (l.major < r.major)
		return -1;
	if (l.minor > r.minor)
		return 1;
	if (l.minor < r.minor)
		return -1;
	if (l.subminor > r.subminor)
		return 1;
	if (l.subminor < r.subminor)
		return -1;
	return 0;
}

static struct nvmeibt_nm_path * get_path_connect_req(
	struct nvmeibt_nm_per_port *pp, struct nvmeibt_nm_login_data *login_data)
{
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_remote_addr *ra = NULL;
	struct nvmeibt_nm_path *path = NULL;

	PFIN;
	if (!(rn = nvmeibt_nm_find_remote_node(pp->pn->local_node, &login_data->node_id))) {
		N_Ef(abrf, "could not find remote node @UUID_LE", &login_data->node_id);
		goto out;
	}
	if (!(ra = nvmeibt_nm_find_remote_address_by_gid(rn, &login_data->sgid)))
		N_Df(fdfsd, "...");
		/*TODO:NM Enable if need
		if (allow_connect_before_register_path)
			ra = add_remote_address(
				rn, NULL, NULL, NULL, &p->sgid, pp->is_roce, pp->pkey);
		*/
	if (!ra)
		goto out;
	/* we call get_path with 1 to tell the get_path that if it finds
	 * a path in the ready queue with different remote_srm_id it must restart
	 * the path because the other peer just reconnected.
	 * */
	if (!(path = nvmeibt_nm_get_path(ra, pp, 1, be32toh(login_data->srm_id))))
		goto out;

out:
	PFOUT;
	return path;
}

int nvmeibt_nm_on_connection_request(struct nvmeibt_nm_login_data *login, struct nvmeibt_nm_per_port *pp, void *data) {
	uint32_t reject_reason = REJECT_FAILED_ACCEPT;
	struct sockaddr_storage a;
	char b[TOMA_SOCKADDR_STRING_LEN];
	int rv;
	struct nvmeibt_nm_path *path;

	if (check_version(login->version, s_min_ver) < 0) {
		N_ETf(nm_hecr_e1001, "Version mismatch: "
			"peer version is @INT.@INT.@INT, "
			"self version is @INT.@INT.@INT, "
			"min required version is @INT.@INT.@INT",
			login->version.major, login->version.minor, login->version.subminor,
			s_self_ver.major, s_self_ver.minor, s_self_ver.subminor,
			s_min_ver.major, s_min_ver.minor, s_min_ver.subminor);
		reject_reason = REJECT_VERSION;
		rv = -1;
		goto send_reject;
	}

	nvmeibt_ib_common_rdma_gid2ip(
		&a, &login->sgid, pp->pn->local_node->hw_func_tbl.get_port(), pp->transport == rtr_ib, pp->broadcast_id);
	N_Tf(nm_he4cr_t1, "Received connect request from addr @STR on host @UUID_LE, "
		"srm_id @INT", nvmeibt_nm_tss(&a, b, sizeof(b)), &login->node_id,
		be32toh(login->srm_id));

	if ((path = get_path_connect_req(pp, login))) {
		path->remote_srm_id = be32toh(login->srm_id);
		path->payload->srm_id = htobe32(path->srm_id);
		path->payload->sgid = pp->gid;
        N_Tf(nm_hecr_t3001, "Path @STR (srm @UINT, remote_srm @UINT) - "
            "received connect request",
            path->name, path->srm_id, path->remote_srm_id);
		if ((rv = pp->pn->local_node->hw_func_tbl.accept_connection(path, pp, login, data)))
			N_ETf(nm_hecr_e104, "Failed to accept - @AUTO_ERRNO");
		else {
			nvmeibt_nm_path_modify_to_rtr(path);
		}
	}
	else {
		N_Tf(nm_hecr_t2345, "Remote addr @STR on host @UUID_LE is not known "
			"at the moment", nvmeibt_nm_tss(&a, b, sizeof(b)), &login->node_id);
		rv = -1;
		goto send_reject;
	}
	goto out;

send_reject:
	if (pp->pn->local_node->hw_func_tbl.reject_connection(data, reject_reason, login))
		N_ETf(nm_hecr_e2, "Fail to reject");

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_del_remote_nic(struct nvmeibt_nm_local_node *ln, struct nvmeibt_nic *nic)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_del_nic_req *e;
	int rv = 0;

	NFIN;
	if (!ln)
		goto out;

	if (nic->transport < 0) {
		return 0;
	}

	if ((r = get_request_ex(ln, 0, NULL))) {
		e = &r->del_nic_req;
		e->node_id = nic->from_config.its_node_id;
		snprintf(e->remote_node, sizeof(e->remote_node), "%s",
			nic->its_node ? nic->its_node->from_config.name : "???");
		e->nic_id = nic->from_config.id;
		snprintf(e->remote_nic, sizeof(e->remote_nic), "%s",
			nic->from_config.sw_gid_str);
		e->transport = nic->transport;
		e->broadcast_id = nic->from_config.partition_key;
		r->base.type = nvmeibt_nm_request_del_nic;
		if (nic->its_node)
			e->me = nic->its_node->is_my_node;
		add_event(ln, ln->event_fd, &ln->el, &r->base);
	}
	else {
		N_ETf(nm_del_remote_nic_e1, "Failed to allocate del_nic_request");
		rv = -1;
	}

out:
	NFOUT;
	return rv;
}

int nvmeibt_nm_del_remote_node(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_del_remote_node *e;
	int rv;

	NFIN;
	if (ln) {
		if ((r = get_request_ex(ln, 0, NULL))) {
			e = &r->del_remote_node;
			e->node_id = node->from_config.id;
			r->base.type = nvmeibt_nm_request_del_remote_node;
			add_event(ln, ln->event_fd, &ln->el, &r->base);
			rv = 0;
		}
		else {
			N_ETf(nm_nrdrn_e1, "Failed to allocate del_remote_node_request");
			rv = -1;
		}
	}
	else
		rv = 0;
	NFOUT;
	return rv;
}

static void trace_time(char *buf, int buf_size)
{
	struct timespec ts;
    time_t nowtime;
    struct tm nowtm;
    char tmbuf[64];
    getnstimeofday_real(&ts);
    nowtime = ts.tv_sec;
	localtime_r(&nowtime, &nowtm);
    strftime(tmbuf, sizeof(tmbuf), "%Y.%m.%d-%H:%M:%S", &nowtm);
    snprintf(buf, buf_size, "%.*s.%09ld", 56, tmbuf, ts.tv_nsec);
}

static const char * print_path_state(enum nvmeibt_nm_path_state state)
{
	switch (state) {
	case nvmeibt_nm_ps_wait_start: return "wait_restart";
	case nvmeibt_nm_ps_wait_addr: return "wait_address_resolve";
	case nvmeibt_nm_ps_wait_route: return "wait_route_resolve";
	case nvmeibt_nm_ps_wait_connect: return "wait_accpet";
	case nvmeibt_nm_ps_connected: return "connected";
	case nvmeibt_nm_ps_wait_ping_ack: return "connected";
	//case ps_wait_ping_ack: return "connected_wait_ping_ack";
	default: return "???";
	}
}

/* local_node API */
static void print_status(struct nvmeibt_nm_local_node *ln)
{
	char **ln_str;
	int *ln_size;
	char *str;
	int len;
	int count;
	int i, j, n;
	struct nvmeibt_nm_per_nic *pn;
	struct nvmeibt_nm_per_port *pp;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_linkable *lra;
	struct nvmeibt_nm_remote_addr *ra;
	struct nvmeibt_nm_linkable *lp;
	struct nvmeibt_nm_path *path;
	char buf[64];

#define add_print(x, ...) \
do { \
	n = scnprintf(str + count, len - count, x, ## __VA_ARGS__); \
	if (n) count += n; else goto inc; \
} while (0)

#if TRACE_NODE_UUID
#define print_path_status(path) \
do { \
	if ((path)->state == nvmeibt_nm_ps_wait_ping_ack) \
		add_print("\t\t\t- ~~path %s, remote_node_id %s, version: %lu, " \
			"state: %s, last_error: %s, ping_retries: %d\n", path->name, \
			nvmeibt_union_uuid_to_urn_uuid(&rn->id).str, (path)->version, print_path_state((path->state), \
			nvmeibt_nm_ple_str((path)->last_error), (path)->ping_retry_counter); \
	else \
		add_print("\t\t\t- ~~path %s, remote_node_id %s, version: %lu, " \
			"state: %s, last_error: %s\n", (path)->name, nvmeibt_union_uuid_to_urn_uuid(&rn->id).str, \
			(path)->version, \
			print_path_state((path->state), nvmeibt_nm_ple_str((path->state)->last_error)); \
} while (0)
#else
#define print_path_status(path) \
do { \
	if ((path)->state == nvmeibt_nm_ps_wait_ping_ack) \
		add_print("\t\t\t- ~~path %s(%p), version: %lu, " \
			"state: %s, last_error: %s, ping_retries: %d\n", (path)->name, (path), \
			(path)->version, print_path_state((path->state)), \
			nvmeibt_nm_ple_str((path)->last_error), (path)->ping_retry_counter); \
	else \
		add_print("\t\t\t- ~~path %s(%p), version: %lu, " \
			"state: %s, last_error: %s\n", (path)->name, (path), \
			(path)->version, \
			print_path_state(path->state), nvmeibt_nm_ple_str((path)->last_error)); \
} while (0)
#endif

	NFIN;

	if (!ln->renew_status)
		goto out;

	trace_time(buf, sizeof(buf));
	if (!ln->status_str || ln->status_str == ln->status_str1) {
		ln_str = &ln->status_str2;
		ln_size = &ln->status_str2_size;
	}
	else {
		ln_str = &ln->status_str1;
		ln_size = &ln->status_str1_size;
	}
	goto cont;

inc:
	len = *ln_size * 2;
	if (!(str = NNVMEIBT_TOMA_CALLOC(nm_print_status_t1, 1, len)))
		goto out;
	NNVMEIBT_TOMA_FREE(nm_print_status_t21, *ln_str);
	*ln_str = str;
	*ln_size = len;

cont:
	len = *ln_size;
	str = *ln_str;
	count = 0;
	add_print("~~IB CONFIG @ %s\n", buf);
	add_print("~~IB STATUS\n");
	for (i = 0; i < ln->n_nics; ++i) {
		pn = ln->nics[i];
		add_print("\t- ~~listener %s, allowed %c, alive %c\n",
			pn->dev_name,
			pn->allowed ? 'Y' : 'N',
			pn->allowed ? (pn->is_dead ? 'N' : 'Y') : '?');
		if (!pn->allowed || pn->is_dead)
			continue;
		for (j = 0; j < pn->n_ports; ++j) {
			pp = pn->ports[j];
			add_print("\t\t- ~~port %d, gid %s, usable=%c, alive=%c, "
				"state=%s, type=%s\n",
				pp->port_num, pp->name,
				pp->is_valid ? 'Y' : 'N', pp->is_dead ? 'N' : 'Y',
				pp->pn->local_node->hw_func_tbl.get_pp_state(pp),
				rdma_trasport_to_string(pp->transport));
			if (pp->is_dead)
				continue;
			XDLIST_FOREACH(lrn, &ln->remotes) {
				rn = nvmeibt_nm_l2rn(lrn);
				XDLIST_FOREACH(lra, &rn->addresses) {
					ra = nvmeibt_nm_l2ra(lra);
					XDLIST_FOREACH(lp, &ra->ready) {
						path = nvmeibt_nm_l2p(lp);
						if (path->pp != pp)
							continue;
						print_path_status(path);
					}
					XDLIST_FOREACH(lp, &ra->connecting) {
						path = nvmeibt_nm_l2p(lp);
						if (path->pp != pp)
							continue;
						print_path_status(path);
					}
				}
			}
		}
	}

	nvmeibt_nm_mutex_lock(&ln->guard);
	ln->status_str = str;
	nvmeibt_nm_mutex_release(&ln->guard);

out:
	return;
}

/* Path counters functions */
void nvmeibt_nm_path_counters_init(struct nvmeibt_nm_path *path)
{
	NFIN;
	memset(&path->counters, 0, sizeof(path->counters));
	path->pp->pn->local_node->renew_status = 1;
	NFOUT;
}

void nvmeibt_nm_path_counters_reset(struct nvmeibt_nm_path *path)
{
	NFIN;
	memset(&path->counters, 0, sizeof(path->counters));
	path->pp->pn->local_node->renew_status = 1;
	NFOUT;
}

void nvmeibt_nm_path_counter_inc(struct nvmeibt_nm_path *path, nvmeibt_nm_path_counter_t *counter)
{
	if (path && counter) {
		(*counter)++;
	}
	path->pp->pn->local_node->renew_status = 1;
}

void nvmeibt_nm_path_counter_add(struct nvmeibt_nm_path *path, nvmeibt_nm_path_counter_t *counter, uint64_t value)
{
	if (path && counter) {
		(*counter) += value;
	}
	path->pp->pn->local_node->renew_status = 1;
}

void nvmeibt_nm_path_set_last_error(struct nvmeibt_nm_path *path, enum nvmeibt_nm_path_last_error error)
{
	if (!path)
		return;

	path->last_error = error;

	if (error >= 0 && error < nvmeibt_nm_ple_max) {
		nvmeibt_nm_path_counter_inc(path, &path->counters.error_counts[error]);
	} else {
		nvmeibt_nm_path_counter_inc(path, &path->counters.error_counts[nvmeibt_nm_ple_other]);
	}
}

/* JDR-based status printing functions */
static void print_path_jdr(struct jdr *jdr, struct nvmeibt_nm_path *path, const char *list_type)
{
	jdr_object_scope(jdr, NULL);
	jdr_write_var(jdr, name, (char const *)path->name);
	jdr_write_var(jdr, version, path->version);
	jdr_write_var(jdr, state, print_path_state(path->state));
	jdr_write_var(jdr, last_error, nvmeibt_nm_ple_str(path->last_error));
	jdr_write_var(jdr, list_type, (char const *)list_type);
	if (path->state == nvmeibt_nm_ps_wait_ping_ack) {
		jdr_write_var(jdr, ping_retries, path->ping_retry_counter);
	}

	/* Add counters information */
	{ /* counters scope */
		jdr_object_scope(jdr, "counters");

		/* Path restart counter */
		jdr_write_var(jdr, path_restarts, path->counters.path_restarts);
		jdr_write_var(jdr, invalid_ping_response, path->counters.invalid_ping_response);
		jdr_write_var(jdr, ping_retries, path->counters.ping_retries);
		{ /* path_restarts_counters scope */
			int error_type;

			jdr_object_scope(jdr, "path_restarts_counters");

			for (error_type = 0; error_type < nvmeibt_nm_ple_max; error_type++) {
				const char *error_description = nvmeibt_nm_ple_str(error_type);
				jdr->ops.u64(jdr, error_description, path->counters.error_counts[error_type]);
			}
		} /* path_restarts_counters scope */
	} /* counters scope */
}

static void print_status_jdr(struct nvmeibt_nm_local_node *ln)
{
	char **ln_json_str;
	int *ln_json_size;
	char *json_str;
	struct charvec buffer;
	struct charvec json = {0};
	struct jdr jdr;
	int i, j;
	struct nvmeibt_nm_per_nic *pn;
	struct nvmeibt_nm_per_port *pp;
	struct nvmeibt_nm_linkable *lrn;
	struct nvmeibt_nm_remote_node *rn;
	struct nvmeibt_nm_linkable *lra;
	struct nvmeibt_nm_remote_addr *ra;
	struct nvmeibt_nm_linkable *lp;
	struct nvmeibt_nm_path *path;
	char timestamp_buf[64];
	int len;

	NFIN;

	if (!ln->renew_status)
		goto out;

	trace_time(timestamp_buf, sizeof(timestamp_buf));

	// we have 2 strs so we always have 1 valid str to use for printing the status has it happens from another thread without locking
	if (!ln->status_json_str || ln->status_json_str == ln->status_json_str1) {
		ln_json_str = &ln->status_json_str2;
		ln_json_size = &ln->status_json_str2_size;
	}
	else {
		ln_json_str = &ln->status_json_str1;
		ln_json_size = &ln->status_json_str1_size;
	}
	goto cont;

inc:
	len = json.len * 2;
	if (!(json_str = NNVMEIBT_TOMA_REALLOC(nm_print_status_jdr_t1, *ln_json_str, len)))
		goto out;
	*ln_json_str = json_str;
	*ln_json_size = len;

cont:
	len = *ln_json_size;
	json_str = *ln_json_str;
	buffer.base = json_str;
	buffer.len = len;

	jdr = jdr_make(buffer);

	{ /* nw_status scope */
		jdr_object_scope(&jdr, "nw_status");
		jdr_write_var(&jdr, timestamp, (char const *)timestamp_buf);
		{ /* state scope */
			jdr_object_scope(&jdr, "state");
			{
				jdr_array_scope(&jdr, "remote_nodes");
				XDLIST_FOREACH(lrn, &ln->remotes) {
					rn = nvmeibt_nm_l2rn(lrn);
					{
						jdr_object_scope(&jdr, NULL);
						jdr_write_var(&jdr, name, (char const *)rn->name);
						jdr_write_var(&jdr, connected, rn->connected);
						jdr_write_var(&jdr, is_me, rn->me);

						/* Print event tracker for this remote node */
						nvmeibt_event_tracker_print_jdr(&rn->event_tracker, &jdr);
					}
				}
			}
			{ /* nics scope */
				jdr_array_scope(&jdr, "nics");
				for (i = 0; i < ln->n_nics; ++i) {
					jdr_object_scope(&jdr, NULL);
					pn = ln->nics[i];
					jdr_write_var(&jdr, name, (char const *)pn->dev_name);
					jdr_write_var(&jdr, allowed, pn->allowed);
					jdr_write_var(&jdr, alive, pn->allowed ? !pn->is_dead : false);

					if (pn->allowed && !pn->is_dead) {
						jdr_array_scope(&jdr, "ports");
						for (j = 0; j < pn->n_ports; ++j) {
							jdr_object_scope(&jdr, NULL);
							pp = pn->ports[j];
							jdr_write_var(&jdr, port_num, pp->port_num);
							jdr_write_var(&jdr, gid, (char const *)pp->name);
							jdr_write_var(&jdr, usable, pp->is_valid);
							jdr_write_var(&jdr, alive, !pp->is_dead);
							if (!pp->is_dead) {
								jdr_write_var(&jdr, state, pp->pn->local_node->hw_func_tbl.get_pp_state(pp));
								jdr_write_var(&jdr, type, rdma_trasport_to_string(pp->transport));
							}

							if (!pp->is_dead) {
								jdr_array_scope(&jdr, "paths");
								XDLIST_FOREACH(lrn, &ln->remotes) {
									rn = nvmeibt_nm_l2rn(lrn);
									XDLIST_FOREACH(lra, &rn->addresses) {
										ra = nvmeibt_nm_l2ra(lra);

										/* Ready paths */
										XDLIST_FOREACH(lp, &ra->ready) {
											path = nvmeibt_nm_l2p(lp);
											if (path->pp != pp)
												continue;
											print_path_jdr(&jdr, path, "ready");
										}

										/* Connecting paths */
										XDLIST_FOREACH(lp, &ra->connecting) {
											path = nvmeibt_nm_l2p(lp);
											if (path->pp != pp)
												continue;
											print_path_jdr(&jdr, path, "connecting");
										}
									}
								}
							}
						}
					}
				}
			} /* nics scope */
		} /* state scope */
	} /* nw_status scope */

	json = jdr_finalize(&jdr);

	/* Check if buffer was too small */
	if (json.base == NULL && json.len > buffer.len) {
		/* Buffer was too small, need larger buffer */
		goto inc;
	}

	nvmeibt_nm_mutex_lock(&ln->guard);
	ln->status_json_str = json_str;
	nvmeibt_nm_mutex_release(&ln->guard);

out:
	NFOUT;
	return;
}

int nvmeibt_nm_print_status_json(void *ctx,
	int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_nm_local_node *ln;
	if (ctx) {
		ln = ctx;
		(*printf_fn)(printf_ctx, "%s\n", ln->status_json_str ? ln->status_json_str : "{}");
	}
	return 0;
}

int nvmeibt_nm_print_status(void *ctx,
	int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_nm_local_node *ln;
	if (ctx) {
		ln = ctx;
		(*printf_fn)(printf_ctx, "%s\n", ln->status_str);
	}
	return 0;
}

void nvmeibt_nm_handle_local_nic_change(struct nvmeibt_nm_local_node *ln, const char *dev_name, int add)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_local_nic_change_req *e;

	NFIN;
	if (!ln)
		goto out;
	if ((r = get_request_ex(ln, 0, NULL))) {
		e = &r->local_nic_change_req;

		if (strncmp(dev_name, siw_iface_prefix, sizeof(siw_iface_prefix) - 1) == 0)
			dev_name += strlen(siw_iface_prefix);
		e->dev_name = NNVMEIBT_TOMA_STRDUP(nm_nrhncr_t1001, dev_name);
		e->add = add;
		if (!e->dev_name) {
			N_ETf(nm_nrhncr_e1, "Failed to allocate nic_change request");
			put_request(ln, r);
			goto out;
		}
		r->base.type = nvmeibt_nm_request_local_nic_changed;
		add_event(ln, ln->event_fd, &ln->el, &r->base);
	}
	else
		N_ETf(nm_nrhnc_e1, "Failed to allocate nic_change_request");

out:
	NFOUT;
}


void nvmeibt_nm_handle_local_port_gid_change(
	struct nvmeibt_nm_local_node *ln, const char *dev_name, int port, const char *gid_str)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_gid_changed_req *e;

	NFIN;
	if (!ln)
		goto out;
	if ((r = get_request_ex(ln, 0, NULL))) {
		e = &r->gid_change_req;

		if (strncmp(dev_name, siw_iface_prefix, sizeof(siw_iface_prefix) - 1) == 0)
			dev_name += strlen(siw_iface_prefix);
		e->dev_name = NNVMEIBT_TOMA_STRDUP(nm_nrhpgc_t1001, dev_name);
		e->gid_str = NNVMEIBT_TOMA_STRDUP(nm_nrhpgc_t1002, gid_str);
		if (!(e->dev_name && e->gid_str)) {
			N_ETf(nm_nrhpgc_e1, "Failed to allocate gid_changed request");
			NNVMEIBT_TOMA_FREE(nm_nrhpgc_t1003, e->dev_name);
			NNVMEIBT_TOMA_FREE(nm_nrhpgc_t1004, e->gid_str);
			put_request(ln, r);
			goto out;
		}
		e->port = port;
		r->base.type = nvmeibt_nm_request_port_gid_changed;
		add_event(ln, ln->event_fd, &ln->el, &r->base);
	}
	else
		N_ETf(nm_nrhpgc_e2, "Failed to allocate port_gid_changed_request");

out:
	NFOUT;
}

void nvmeibt_nm_rsrm_faults_handle_fifo_com(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_req *r;

	//NFIN;
	if (ln) {
		if ((r = get_request_ex(ln, 0, NULL))) {
			r->base.type = nvmeibt_nm_request_rsrm_faults;
			add_event(ln, ln->event_fd, &ln->el, &r->base);
		}
		else
			N_ETf(nm_nrfrd_e1, "Failed to allocate rsrm_faults_request");
	}
	//NFOUT;
}

int nvmeibt_nm_cancel_req_node(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *node)
{
	struct nvmeibt_nm_req *r;
	struct nvmeibt_nm_cencel_remote_node_req *e;
	int rv;

	NFIN;
	if (ln) {
		if ((r = get_request_ex(ln, 0, NULL))) {
			e = &r->cacnel_remote_node_req;
			e->node_id = node->from_config.id;
			r->base.type = nvmeibt_nm_request_cancel_req;
			add_event(ln, ln->event_fd, &ln->el, &r->base);
			rv = 0;
		}
		else {
			N_ETf(nm_nrcrn_e1, "Failed to allocate del_remote_node_request");
			rv = -1;
		}
	}
	else
		rv = 0;
	NFOUT;
	return rv;
}

void nvmeibt_nm_done(struct nvmeibt_nm_local_node *ln)
{
	struct nvmeibt_nm_req *r;
	pthread_t t;


	NFIN;
	if (!ln)
		goto out;
	if ((r = get_request_ex(ln, 0, NULL))) {
		t = ln->thr;
		r->base.type = nvmeibt_nm_request_stop;
		add_event(ln, ln->event_fd, &ln->el, &r->base);
		pthread_join(t, NULL);
	}
	else
		N_ETf(nm_nrd_e1, "Failed to allocate stop_request");

out:
	NFOUT;
}

void nvmeibt_nm_set_path_state(struct nvmeibt_nm_path *path, enum nvmeibt_nm_path_state new_state) {
	enum nvmeibt_nm_path_state old_state = path->state;

	path->state = new_state;
	if ((new_state >= nvmeibt_nm_ps_wait_connect && old_state < nvmeibt_nm_ps_connected) || (old_state >= nvmeibt_nm_ps_connected &&
		 new_state < nvmeibt_nm_ps_connected))
			N_Tf(t_nm_set_path_state, "@STR changed state from @STR -> @STR", path->name,
										print_path_state(old_state), print_path_state(new_state));
}

bool nvmeibt_nm_is_remote_node_connected(struct nvmeibt_nm_local_node *ln, struct nvmeibt_node *remote_node)
{
	struct nvmeibt_nm_remote_node 		*rn;
	bool								is_connected = 0;

	nvmeibt_nm_mutex_lock(&ln->guard);
	rn = nvmeibt_nm_find_remote_node(ln, &remote_node->from_config.id);
	nvmeibt_nm_mutex_release(&ln->guard);

	if (rn)
		is_connected = !!rn->connected;
	else
		N_Wf(uru84ns, "No remote node that match node_id=@UUID_LE", &remote_node->from_config.id);

	return is_connected;
}

