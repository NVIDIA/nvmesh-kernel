/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#define S_CLIENT_C

#include "kr_incs.h"

#include "nvmeibs_defs.h"
#include "nvmeibs_client.h"
#include "nvmeibs_nordda.h"
#include "nvmeibs_main.h"
#include "nvmeibs_nvme.h"
#include "nvmeibs_disk.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeibs_types.h"
#include "nvmeib_ib_driver.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeib_public.h"
#include "nvmeibs_test.h"
#include "nvmeibs_toma.h"
#include "nvmeib_utils.h"
#include "nvmeib_srq.h"
#include "nvmeib_shared.h"
#include "nvmeib_utils.h"
#include "nvmeibs_client_db.h"
#include "nvmeibs_trace.h"
#include "vex/nvmeibs_vex.h"
#include "nvmeib_public.h"
#include "../core_unitest/corecomm_injections.h"
#include "../softiwarp/common/siw_user.h"
#include "common/proc_epilog.h"
#include "kr_undef.h"
#include "nvmeibs_memmgr_metrics.h"

#define __NFIN NFINS(cl->name)
#define __NFOUT NFOUTS(cl->name)
#define is_logout_rsp(_hdr, _recv_ioctx) (_hdr->opcode == NVMEIB_RSP && \
	 		((struct volume_client_rsp *)(_recv_ioctx->buf))->opcode == NVMEIBC_RSP_LOGOUT_OK)

static unsigned int nvmeibs_nr_max_channels_per_path = 0;
module_param_named(nr_max_channels_per_path, nvmeibs_nr_max_channels_per_path, uint, 0644);
MODULE_PARM_DESC(nr_max_channels_per_path, "The maximum number of RDMA IO channels per network path.");

static unsigned int nvmeibs_nr_max_channels_per_path_tcp = NVMEIB_MAX_NR_TCP_CHANNELS_PER_PATH;
module_param_named(nr_max_channels_per_path_tcp, nvmeibs_nr_max_channels_per_path_tcp, uint, 0644);
MODULE_PARM_DESC(nr_max_channels_per_path_tcp, "The maximum number of SIW IO channels per network path.");

static unsigned int nvmeibs_ioka_timeout_sec = NVMEIB_IOCH_KA_TIMEOUT_SEC;
module_param_named(ioka_timeout_sec, nvmeibs_ioka_timeout_sec, int, 0644);
MODULE_PARM_DESC(ioka_timeout_sec, "Keepalive timeout failure for an IO channel, in seconds.");

NVMEIBS_MEMMGR_METRIC(s_clients_msg_area, "component=target.clients.msg_area");


#define SCLIENTS_PROC_DIRNAME "clients"
static struct proc_dir_entry *sclients_dir = NULL;

static void cl_send_comp_h(void *ctx, struct ib_wc *wcs);
static void cl_recv_comp_h(void *ctx, struct ib_wc *wcs);
static void l_send_comp_h(void *ctx, struct ib_wc *wcs);
static void l_recv_comp_h(void *ctx, struct ib_wc *wcs);
static void l_2nd_send_comp_h(void *ctx, struct ib_wc *wcs);
static void l_2nd_recv_comp_h(void *ctx, struct ib_wc *wcs);

static void send_resource(struct nvmeibs_client *cl,
						  struct nvmeibs_cmd_info *info,
						  enum vex_ext_enum vex_ext, struct nvmeib_iu *send_ioctx);

static void free_rionic_ka(struct nvmeibs_rionic *rionic, bool already_locked);

void cookie_store_remove_local_ch(struct nvmeibs_client *cl);
static int single_sclient_proc_mkdir(struct nvmeibs_client *cl);
static void single_sclient_proc_mkdir_work(struct workqe_struct *work);
static void single_sclient_proc_umkdir(struct nvmeibs_client *cl);

static void cl_set_name(struct nvmeibs_client *cl,
	const char *host_name, const char *disk_name)
{
	NFIN;
	snprintf(cl->host_name, sizeof(cl->host_name), "%s", host_name);
	snprintf(cl->disk_name, sizeof(cl->disk_name), "%s", disk_name);
	snprintf(cl->name, sizeof(cl->name), "%.*s_%.*s_%06d",
		NVMEIB_HOST_NAME_LEN, cl->host_name,
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE, cl->disk_name,
		(int)(cl->cid % 1000000));
	NFOUT;
}

static int cl_disk_get_(struct list_head *disks, struct nvmeibs_client *cl,
	const char *host_name, const char *disk_name)
{
	//struct list_head *disks;
	struct nvmeibs_disk_info *di;
	int n;
	int rv = -1;
	NFIN;

	_NT(trace_client_cl_disk_get_, "disks=@DISKS_PTR", disks);
	if (cl->di) {
		_NT(trace_1_client_cl_disk_get_, "Already linked to disk @DI", cl->di);
		goto out;
	}
	if (!nvmeibs_cdb_exists(cl)) {
		_NT(trace_2_client_cl_disk_get_, "cl not hashed");
		/* if unhashed cl manage to get disk and release-disk-clients runs
		   before it is hashed, it will miss it (wait till it disconnects) */
		goto out;
	}

	//disks = nvmeibs_disk_get_disks(NULL);
	list_for_each_entry(di, disks, link) {
		_NT(trace_3_client_cl_disk_get_, "di->disk_id=@DISK_ID_STR, disk_name=@DISK_NAME", di->disk_id, disk_name);
		if (!strncmp(di->disk_id, disk_name, sizeof(di->disk_id))) {
			if (di->dying) {
				_NT(trace_4_client_cl_disk_get_, "disk @DISK_ID_STR is dying", di->disk_id);
				break;
			}
			if ((n = nvmeib_ref_get(&di->nref)) == 0) {
				_NW(warn_client_cl_disk_get_, "Fail to inc alive disk's refcnt ???");
				break;
			}
			_NT(trace_5_client_cl_disk_get, "Found disk @DISK_ID_STR, inc to @REFCNT", di->disk_id, n);
			cl_set_name(cl, host_name, disk_name);
			cl->di = di;

			rv = 0;
			break;
		}
	}
	//nvmeibs_disk_put_disks();

out:
	NFOUT;
	return rv;
}

static int cl_disk_get(struct nvmeibs_client *cl,
	const char *host_name, const char *disk_name)
{
	struct list_head *disks;
	int rv;

	disks = nvmeibs_disk_get_disks(NULL);
	rv = cl_disk_get_(disks, cl, host_name, disk_name);
	nvmeibs_disk_put_disks();

	NFOUT;
	return rv;
}

static void cl_disk_put(struct nvmeibs_client *cl)
{
	int n;
	NFIN;

	if (cl->di) {
		if ((n = nvmeib_ref_put(&cl->di->nref)) < 0)
			_NW(warn_client_cl_disk_put, "negative ref cnt???");
		_NT(trace_client_cl_disk_put, "dec to @REFCNT", n);
		cl->di = NULL;
	}
	else
		_NW(warn_1_client_cl_disk_put, "Not linked");

	NFOUT;
}

/**
 * Client's local disk (ldisk) register to server's corresponding
 * local s_client to protect shared resources namely (deprecated). NVMESH-4574 active-locks-deprecation
 * Unlike remote-client, local-client may still access s_client's
 * resources after breaking QP due to shorcuts from nvmeib_local_server.
 * This API shall ensure s_client object is not freed before
 * client module (nvmeibc-disk-release) is done with this instance.
 *
 * API:
 * _register()   : called after connecting admin-channel (s_client created)
 * _unregister() : called after disconnecting disk.
 * _wait()       : called after removing s_client from hash.
 *
 */
/* List of srv's local-clients registered by their corresponding clnt's ldisk.
   Protected by nvmeibs_client_lock */
static LIST_HEAD(nvmeibs_client_ldisk_list);
/* Mutex just for making nvmeibs_client_ldisk-wait reentrant */
static DEFINE_MUTEX(ldisk_done_guard);

#define NVMEIB_CLIENT_LDISK_WAIT (5 * HZ)

/* This function may only be used if cl is safe, either:
   under clients lock, from cl-wq or after removing it from hash */
static int ldisk_is_registered(struct nvmeibs_client *cl)
{
	NFIN;
	NFOUT;
	return !list_empty(&cl->ldisk_link);
}

struct ldisk_register_params {
	const char *disk_name;
	struct nvmeib_local_disk *ldisk;
	struct list_head *disks;
};

static int ldisk_single_sclient_proc_mkdir_work_add(struct nvmeibs_client *cl)
{
	struct cl_external_workq *ew;
	int rv = 1;

	if (!(ew = kzalloc(sizeof(*ew), GFP_ATOMIC))) {
		_NE(error_ldisk_register_fc_ew_kzalloc, "Fail to proc mkdir work");
		goto out;
	}

	WQ_INIT_WORK(&ew->work, single_sclient_proc_mkdir_work);
	ew->cl = cl;
	/* we are already holding the cid lock */
	if (nvmeibs_client_add_work(cl, &ew->work)) {
		_NE(error_ldisk_register_add_e_work, "Fail to add external work");
		goto out;
	}
	/* pass ownership to the queue */
	ew = NULL;
	rv = 0;

out:
	if (unlikely(ew))
		kfree(ew);
	return rv;
}

static int ldisk_register_fc(struct nvmeibs_client *cl, void *arg)
{
	struct ldisk_register_params *p = arg;
	const char *disk_name = p->disk_name;
	struct nvmeib_local_disk *ldisk = p->ldisk;
	int rv = -1;
	NFIN;

	if (!cl->is_local) {
		_NE(error_client_ldisk_register_fc, "cl is not local");
		goto err;
	}
	if (!cl->net) {
		_NE(error_1_client_ldisk_register_fc, "invalid main-net");
		goto err;
	}
	if (cl->net->state != QP_LIVE &&
		cl->net->state != QP_CONNECTING) {
		_NE(error_2_client_ldisk_register_fc, "invalid main-net state");
		goto err;
	}
	if (ldisk_is_registered(cl)) {
		_NE(error_3_client_ldisk_register_fc, "ldsik already registered");
		goto err;
	}

	if (!nvmeibs_async_cookie_store_add_ch(&cl->cookie_store, NVMEIBS_ASYNC_LOCAL_CHANNEL, GFP_ATOMIC)) {
		_NE(error_5_client_ldisk_register_fc, "Fail to add async cookie channel for local bypass");
		goto err;
	}

	/* find disk and set cl->di */
	if (cl_disk_get_(p->disks, cl, nvmeib_get_utsname_nodename(), disk_name) < 0) {
		_NE(error_4_client_ldisk_register_fc, "Fail to get disk");
		goto err;
	}

	if ((rv = ldisk_single_sclient_proc_mkdir_work_add(cl))) {
		goto err;
	}

	_NT(trace_client_ldisk_register_fc, "register ldisk to cl @CL (@CL_NAME)", cl, cl->name);
	list_add_tail(&cl->ldisk_link, &nvmeibs_client_ldisk_list);
	ldisk->p = cl->di;
	ldisk->sector_shift = cl->di->block_shift;
	ldisk->md_size = cl->di->metadata;
	ldisk->md_extd = cl->di->mtdt_extd;
	ldisk->max_request_size = cl->di->max_request_size;
	ldisk->external = !!cl->di->external;
	rv = 0;

	goto out;

err:
	cookie_store_remove_local_ch(cl);

out:
	NFOUT;
	return rv;
}

int nvmeibs_client_ldisk_register(u64 cid, const char *disk_name,
	struct nvmeib_local_disk *ldisk)
{
	struct ldisk_register_params p = {
		.disk_name = disk_name,
		.ldisk     = ldisk,
		.disks     = NULL};
	int rv;
	NFIN;

	p.disks = nvmeibs_disk_get_disks(NULL);
	rv = nvmeibs_cdb_cid_fast_call(cid, ldisk_register_fc, &p);
	nvmeibs_disk_put_disks();

	NFOUT;
	return rv;
}

static int ldisk_unregister_(struct nvmeibs_client *cl)
{
	int rv;
	NFIN;

	cookie_store_remove_local_ch(cl);

	if (!completion_done(&cl->ldisk_done)) {
		_NT(trace_client_ldisk_unregister, "unregister ldisk from cl @CL (@CL_NAME)", cl, cl->name);
		complete(&cl->ldisk_done);
		rv = 0;
	}
	else {
		_NW(warn_client_ldisk_unregister, "cl @CL_NAME, ldisk already unregistered", cl->name);
		rv = -1;
	}

	NFOUT;
	return rv;
}

int nvmeibs_client_ldisk_unregister(u64 cid, struct nvmeib_local_disk *ldisk)
{
	struct nvmeibs_client *cl;
	unsigned long flags;
	bool found = false;
	int rv = -1;
	NFIN;

	_NT(trace_s_client_ldisk_unregister,
		"Putting Disk @DISK_NAME jrange handle @JRANGE_HANDLE for range @JRNL_RNG_IDX",
		((struct nvmeibs_disk_info *)ldisk->p)->disk_id, ldisk->jrange_handle,
		(u16)ldisk->jrnl.rng_idx);
	nvmeibs_serjio_put_jrange_handle(ldisk->jrange_handle, NULL);
	ldisk->jrange_handle = NULL;

	flags = nvmeibs_cdb_lock();
	list_for_each_entry(cl, &nvmeibs_client_ldisk_list, ldisk_link) {
		if (cl->cid == cid) {
			found = true;
			rv = ldisk_unregister_(cl);
			break;
		}
	}
	nvmeibs_cdb_unlock(flags);

	if (!found)
		_NW(warn_client_nvmeibs_client_ldisk_unregister, "cl @CL_NAME, ldisk try to unregister but not registered", cl->name);


	NFOUT;
	return rv;
}

/* This function is called after removing cl from hash
   to prevent concurrent ldisk's registeration to cl */
static int nvmeibs_client_ldisk_wait(struct nvmeibs_client *cl)
{
	unsigned long flags;
	int a = 0, rvw;
	int rv = -1;
	NFIN;

	/* sanity */
	if (nvmeibs_cdb_find_cid(cl->cid)) {
		_NE(error_client_nvmeibs_client_ldisk_wait, "cl @CL_NAME, must first be removed from cdb", cl->name);
		goto out;
	}

	if (!ldisk_is_registered(cl)) {
		_NT(trace_client_nvmeibs_client_ldisk_wait, "cl @CL_NAME, ldisk not registered, not waiting", cl->name);
		rv = 0;
		goto out;
	}

	//mutex_lock(&ldisk_done_guard);
	while ((rvw = wait_for_completion_interruptible_timeout(
		&cl->ldisk_done, NVMEIB_CLIENT_LDISK_WAIT)) <= 0) {
		_NW(nvmeibs_client_ldisk_wait_w1, "cl @STR: wait for ldisk, attempt @INT (rvw=@INT)",
			cl->name, a, rvw);
		a++;
	}
	_NT(trace_1_client_nvmeibs_client_ldisk_wait, "cl @CL_NAME, done wait to ldisk unregister (del from list)", cl->name);

	/* remove from nvmeibs_client_ldisk_list */
	flags = nvmeibs_cdb_lock();
	list_del_init(&cl->ldisk_link);
	nvmeibs_cdb_unlock(flags);
	rv = 0;
	//mutex_unlock(&ldisk_done_guard);

out:
	NFOUT;
	return rv;
}

static int client_add_work_external_fc(struct nvmeibs_client *cl, void *arg)
{
	struct cl_external_workq *ew = arg;
	int rv;
	NFIN;

	if (!cl->is_local || ldisk_is_registered(cl)) {
		ew->cl = cl;
		if ((rv = nvmeibs_client_add_work(cl, &ew->work)) < 0)
			_NE(error_client_client_add_work_external_fc, "Fail to add work (rv @RV)", rv);
	}
	else {
		_NT(trace_client_client_add_work_external_fc, "cl @CL_NAME, ldisk not registered, not adding work", cl->name);
		rv = -1;
	}

	NFOUT;
	return rv;
}

/* under clients-hash lock, if cid found in hash add work to cid's cl-wq */
int nvmeibs_client_add_work_external(u64 cid, struct cl_external_workq *ew)
{
	int rv;
	NFIN;

	rv = nvmeibs_cdb_cid_fast_call(cid, client_add_work_external_fc, ew);

	NFOUT;
	return rv;
}

struct ldisk_locks_workq {
	struct cl_external_workq ew;
	struct nvmeib_local_disk *ldisk;
	struct completion *done;
	int rv;
};

static bool is_cl_lock_net_dying(struct nvmeibs_client *cl)
{
	NFIN;
	NFOUT;
	/* must run on cl-wq */
	return (!cl->lock_net || atomic_read(&cl->lock_net->dying));
}

//work-function to do lock-channel stuff for ldisk
static void ldisk_locks_work(struct workqe_struct *work)
{
	struct cl_external_workq *ew =
		container_of(work, struct cl_external_workq, work);
	struct nvmeibs_client *cl = ew->cl;
	struct ldisk_locks_workq *w =
		container_of(ew, struct ldisk_locks_workq, ew);
	struct nvmeib_local_disk *ldisk = w->ldisk;
	int rv = -1;
	__NFIN;

	if (!cl->di) {
		_NE(error_client_ldisk_locks_work, "cl @CL_NAME, no di", cl->name);
		goto out;
	}
	if (is_cl_lock_net_dying(cl)) {
		_NE(error_1_client_ldisk_locks_work, "cl @CL_NAME, no lock-net, not holding lock-dev refcnt, bail", cl->name);
		goto out;
	}
	rv = nvmeibs_disk_locks_ldisk_alloc(cl, ldisk);

out:
	w->rv = rv;
	complete(w->done);
	__NFOUT;
}

int nvmeibs_client_ldisk_alloc_locks(u64 cid, struct nvmeib_local_disk *ldisk)
{
	struct ldisk_locks_workq w;
	DECLARE_COMPLETION_ONSTACK(done);
	int rv;
	NFIN;

	WQ_INIT_WORK(&w.ew.work, ldisk_locks_work);
	w.ew.cl = NULL;
	w.done = &done;
	w.ldisk = ldisk;
	if ((rv = nvmeibs_client_add_work_external(cid, &w.ew)) < 0)
		_NE(error_client_nvmeibs_client_ldisk_alloc_locks, "Fail to add work cid=@CID_LLONG, (rv @RV)", cid, rv);
	else {
		_NT(trace_client_nvmeibs_client_ldisk_alloc_locks, "wait for work on cl-wq cid=@CID_LLONG", cid);
		wait_for_completion(&done);
		rv = w.rv;
	}

	NFOUT;
	return rv;
}

struct ldisk_jrnl_workq {
	struct cl_external_workq ew;
	struct nvmeib_local_disk *ldisk;
	const struct nvmeib_jrange_cache *jrc;
	binje_t binje_req;
	struct completion *done;
	int rv;
	void *jrange_handle;
};

//work-function to do lock-channel stuff for ldisk
static void ldisk_jrnl_work(struct workqe_struct *work)
{
	struct cl_external_workq *ew =
		container_of(work, struct cl_external_workq, work);
	struct nvmeibs_client *cl = ew->cl;
	struct ldisk_jrnl_workq *w =
		container_of(ew, struct ldisk_jrnl_workq, ew);
	struct nvmeib_local_disk *ldisk = w->ldisk;
	const struct nvmeib_jrange_cache *jrc = w->jrc;
	int rv = 0;
	__NFIN;

	if (!cl->di) {
		_NE(error_client_ldisk_jrnl_work, "cl @CL_NAME, no di", cl->name);
		rv = -ENODATA;
		goto out;
	}

	if (!cl->di->metadata) {
		_NE(error_1_client_ldisk_jrnl_work, "cl @CL_NAME, disk @DISK_ID_STR has no metadata", cl->name, cl->di->disk_id);
		rv = -EINVAL;
		goto out;
	}

	/* Disk has metadata, get a journal-range from SERJIO */
	if ((rv = nvmeibs_serjio_alloc_journal_range(cl->di, cl->cid,
		cl->client_uuid, cl->host_name, w->binje_req, jrc, &ldisk->jrnl)) < 0) {
		if (rv == -EBUSY)
			_NT(trace_client_ldisk_jrnl_work, "Client @CL_NAME (@CLIENT_UUID) - Journal range @JRNL_RNG_IDX is still releasing. Try again",
			   cl->name, &cl->client_uuid, jrc->rng_id);
		else
			_NE(error_2_client_ldisk_jrnl_work, "Error @RV allocating journal chunk", rv);
		cl->jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
		cl->jrnl_rng_binje = NVMEIB_EC_INVALID_JOURNAL_BINJE;
		cl->jrnl_rng_n_ent = 0;
		goto out;
	} else {
		cl->jrnl_rng = rv;
		cl->jrnl_rng_binje = ldisk->jrnl.rng_binje;
		cl->jrnl_rng_n_ent = ldisk->jrnl.n_ents;
		if (IS_ERR(w->jrange_handle = nvmeibs_serjio_get_jrange_handle(
				cl->di, cl->cid, cl->jrnl_rng, NULL, NULL))) {
			nvmeibs_serjio_return_journal_range(cl->di, cl->jrnl_rng);
			cl->jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
			cl->jrnl_rng_binje = NVMEIB_EC_INVALID_JOURNAL_BINJE;
			rv = PTR_ERR(w->jrange_handle);
			_NE(error_3_client_ldisk_jrnl_work, "Error @RV getting journal range handle", rv);
			goto out;
		}
		rv = 0;
		_NT(trace_2_s_client_ldisk_jrnl_work, "Client @CL_NAME (@CLIENT_UUID) - Journal range @JRNL_RNG_IDX got jrange handle @JRANGE_HANDLE",
			cl->name, &cl->client_uuid, jrc->rng_id, w->jrange_handle);
	}

out:

	w->rv = rv;
	complete(w->done);
	__NFOUT;
}

int nvmeibs_client_ldisk_alloc_jrnl_rng(u64 cid, struct nvmeib_local_disk *ldisk,
	const struct nvmeib_jrange_cache *jrc, binje_t binje_req) {
	struct ldisk_jrnl_workq w;
	DECLARE_COMPLETION_ONSTACK(done);
	int rv;
	NFIN;

	WQ_INIT_WORK(&w.ew.work, ldisk_jrnl_work);
	w.ew.cl = NULL;
	w.done = &done;
	w.ldisk = ldisk;
	w.jrc = jrc;
	w.binje_req = binje_req;
	if ((rv = nvmeibs_client_add_work_external(cid, &w.ew)) < 0)
		_NE(error_client_nvmeibs_client_ldisk_alloc_jrnl_rng, "Fail to add work cid=@CID_LLONG, (rv @RV)", cid, rv);
	else {
		_NT(trace_client_nvmeibs_client_ldisk_alloc_jrnl_rng, "wait for work on cl-wq cid=@CID_LLONG", cid);
		wait_for_completion(&done);
		rv = w.rv;
		if (!rv)
			ldisk->jrange_handle = w.jrange_handle;
	}

	NFOUT;
	return rv;
}

/* controller get_put resource command work */
struct get_put_rsc_workq {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	struct nvmeibs_get_put_cmd *cmd;
};

static int post_recv_iu(struct nvmeibs_client *cl, struct nvmeib_iu *iu);

int nvmeibs_client_send_msg(struct nvmeibs_client *cl, struct nvmeibs_net *net,
	struct nvmeib_iu *iu, int len, int wr_opcode, u16 wr_version)
{
	struct ib_sge list[2], *sge = list;
	struct ib_send_wr wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;

	__NFIN;

	if (atomic_read(&net->dying)) {
		_NE(error_1_client_nvmeibs_client_send_msg, "net @NET is dying", net);
		rv = -1;
		goto out;
	}

	if (!net->qp) {
		_NE(error_2_client_nvmeibs_client_send_msg, "net @NET, qp is NULL", net);
		rv = -1;
		goto out;
	}

	if (net->msg_hdr) {
		sge->addr = net->msg_hdr_dma_addr;
		sge->length = sizeof(*net->msg_hdr);
		sge->lkey = nvmeib_get_lkey(P2NV(net->params.port));
		sge++;
	}

	sge->addr = iu->dma;
	sge->length = len;
	sge->lkey = nvmeib_get_lkey(P2NV(net->params.port));

	memset(&wr, 0, sizeof(wr));
	wr.opcode = IB_WR_SEND;
	wr.wr_id = nordda_wr_id_encode(wr_version, wr_opcode, iu->index);
	wr.sg_list = list;
	wr.num_sge = 1 + (sge - list);
	wr.send_flags = IB_SEND_SIGNALED;
#if ENABLE_SIW
	if (P2NV(net->params.port)->dev_type == DT_siw) {
		struct volume_server_rsp *rsp = iu->buf;
		wr.send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
		if (rsp->hdr.opcode == NVMEIB_RSP && rsp->opcode == NVMEIBS_RSP_IO_OPCODE_OK)
			wr.send_flags |= SIW_IB_SEND_TX_TIMESTAMP;
	}
#endif
	_ND(trace_client_nvmeibs_client_send_msg,
		"Sending cl=@CL_NAME net=@NET iu->index=@INDEX, wr_id=@WR_ID,"
		"qp_num=@QP_NUM, remote_qpn=@QP_NUM", cl->name, net,
		iu->index, wr.wr_id, net->qp->qp_num, net->remote_qpn);
	wmb();
	/* JH IOMMU: DMA_TO_DEVICE is correct. Source for Local RDMA_SEND */
	ib_dma_sync_single_for_device(P2IB(net->params.port), iu->dma,
		len, DMA_TO_DEVICE);

	if (!net->qp_stats) {
		_NE(v7yedri, "net @NET, qp_stats=NULL", net);
		rv = -1;
		goto out;
	}

	iu->send_size += len;
	iu->send_time = ktime_get();
	rv = nvmeibs_ib_post_send(net, &wr, &bad_wr);

out:
	__NFOUT;
	return rv;
}

static void prepare_rsp_msg(struct nvmeibs_client *cl, struct nvmeibs_net *net,
	u64 tag, __be16 version_tag, struct nvmeib_iu *send_ioctx, int payload_len)
{
	struct volume_server_rsp *rsp;
	int r_msg_size = net->params.s_msg_size;

	__NFIN;
	_ND(trace_client_prepare_rsp_msg, "r_msg_size @R_MSG_SIZE", r_msg_size);
	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is source for local RDMA_SEND */
	ib_dma_sync_single_for_cpu(P2IB(net->params.port), send_ioctx->dma,
		r_msg_size, DMA_TO_DEVICE);
	rsp = send_ioctx->buf;
	memset(rsp, 0, offsetof(struct volume_server_rsp, payload) + payload_len);
	rsp->hdr.opcode = NVMEIB_RSP;
	rsp->hdr.tag = tag;
	rsp->version_tag = version_tag;
	__NFOUT;
}

static int send_rsp_msg(struct nvmeibs_client *cl, struct nvmeibs_net *net,
						u8 opcode, struct nvmeib_iu *send_ioctx, void *p, int len, int wr_opcode, u16 wr_version)
{
    struct volume_server_rsp *rsp = send_ioctx->buf;
    int rv;

    __NFIN;
    rsp->opcode = opcode;
    /* copy extra bytes - if needed */
    if ((opcode == NVMEIBS_RSP_MGMT_OPCODE_OK ||
         opcode == NVMEIBS_RSP_IO_OPCODE_OK ||
         opcode == NVMEIBS_RSP_IO_OPCODE_ERR ||
         opcode == NVMEIBS_RSP_GEN_OPCODE_OK ||
         opcode == NVMEIBS_RSP_GEN_OPCODE_ERR) && p && len)
        vex_memcpy(rsp->payload, p, len);
    /* we use offsetof and sizeof since the struct includes a last member
       of type[0] member and in that case
       sizeof(struct) != offsetof(struct, [0])
    */
    if ((rv = nvmeibs_client_send_msg(
        cl, net, send_ioctx, offsetof(struct volume_server_rsp, payload) + len,
        wr_opcode, wr_version)) < 0) {
        _NE(error_client_send_rsp_msg, "Send response to client failed @RV", rv);
		if (nvmeibs_use_pcpu_cq) {
			WARN_ON_ONCE(1);
			nvmeibs_net_release(net, NVMEIBS_LOGOUT_REASON_SEND_RSP_FAILED);
		}
	}
    __NFOUT;
    return rv;
}

int nvmeibs_client_send_rsp(struct nvmeibs_client *cl, struct nvmeibs_net *net,
	u8 opcode, u64 tag, __be16 version_tag, struct nvmeib_iu *send_ioctx, void *p, int len,
	int wr_opcode, u16 wr_version)
{
	int rv;

	__NFIN;
	prepare_rsp_msg(cl, net, tag, version_tag, send_ioctx, len);
	rv = send_rsp_msg(cl, net, opcode, send_ioctx, p, len, wr_opcode, wr_version);
	__NFOUT;
	return rv;
}

struct nvmeibs_rionic *nvmeibs_client_get_rionic(struct nvmeibs_client *cl,
	struct nvmeibc_io_channel_def *def, enum nvmeibs_client_ioch_type ioch_type)
{
	struct list_head *disks = &cl->disks;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_rionic *rionic = NULL;
	int qp = be16_to_cpu(def->qp_num);
	int i, j;

	__NFIN;

	if (!list_is_singular(disks)) {
		_NT(trace_client_nvmeibs_client_get_rionic, "cl's disks list not singular");
		goto out;
	}

	list_for_each_entry(cdisk, disks, link) {
			for (i = 0; i < cdisk->n_lionics; ++i) {
				_NT(nvmeibs_client_get_rionic_t1, "cdisk->lionics[i].gid.raw=@GID_RAW, def->dgid=@DGID",
					cdisk->lionics[i].gid.raw, def->dgid);
				if (!memcmp(cdisk->lionics[i].gid.raw, def->dgid, 16))
					for (j = 0; j < cdisk->lionics[i].n_rionics; ++j) {
						_NT(nvmeibs_client_get_rionic_t2, "cdisk->lionics[i].rionics[j].gid.raw=@GID_RAW,"
							"def->sgid=@SGID",
							cdisk->lionics[i].rionics[j].gid.raw, def->sgid);
						if (!memcmp(cdisk->lionics[i].rionics[j].gid.raw,
							def->sgid, 16)) {
							if (ioch_type == NVMEIBS_IOCH_RDDA) {
								/* RDDA removed */
							}
							else if (ioch_type == NVMEIBS_IOCH_NORDDA) {
								if (qp < cdisk->lionics[i].rionics[j].\
									n_nr_channels) {
									_ND(nvmeibs_client_get_rionic_d2, "Found requested nordda ioch triplet");
									rionic = &cdisk->lionics[i].rionics[j];
									goto out;
								}
							}
							else {
								_NT(nvmeibs_client_get_rionic_t3, "Unknown ioch-type @INT", ioch_type);
								goto out;
							}
						}
					}
			}
	}
out:
	__NFOUT;
	return rionic;
}

static int send_rdma_msg(struct nvmeibs_client *cl,
	struct volume_client_config_rdma_info *rdma, size_t rdma_len)
{
	struct ib_sge list;
	struct nvmeib_send_wr wr;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;

	__NFIN;
	BUG_ON(rdma_len > NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
	if (!cl->net->qp) {
		_NE(error_client_send_rdma_msg, "cl @CL_NAME net->qp is NULL", cl->name);
		rv = -1;
		goto out;
	}
	nvmeib_mem_sync_map_for_device(&cl->out_msg_area_map, 0, rdma_len);
	list.addr = cl->out_msg_area_map.ioaddr;
	list.length = rdma_len;
	list.lkey = cl->out_msg_area_map.lkey;
#if 0
	/* Jared: Why is this an error? */
	if (list.length != be32_to_cpu(rdma->msg_size)) {
		_NE(error_1_client_send_rdma_msg, "Err: list.length=@LENGTH_INT != rdma->msg_size=@MSG_SIZE",
			list.length, be32_to_cpu(rdma->msg_size));
		dump_stack();
		rv = -1;
		goto out;
	}
#endif
	memset(&wr, 0, sizeof(wr));
	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_rdma(wr).remote_addr = be64_to_cpu(rdma->msg_raddr);
	nvmeib_send_wr_rdma(wr).rkey = be32_to_cpu(rdma->msg_rkey);
	nvmeib_send_wr_common(wr).sg_list = &list;
	nvmeib_send_wr_common(wr).num_sge = 1;

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_MORE_WQES | SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
	}
#endif

	rv = nvmeibs_ib_post_send(cl->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);
	if (rv < 0)
		_NT(error_2_client_send_rdma_msg_post_send,
				"cl @CL_NAME net->qp @QP ib_post_send failed (@RV)",
				cl->name, cl->net->qp, rv);

out:
	__NFOUT;
	return rv;
}

struct nvmeibs_config_get_io_ctx {
	struct nvmeibs_client *cl;
	struct nvmeibs_ib_port *port;
	struct volume_client_req *req;
	struct volume_client_config_rdma_info rdma;
	char client_name[NVMEIB_HOST_NAME_LEN];
	char disk_name[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_alloc_disk_srv_base_encode)
{
	struct nvmeibs_client *cl = arg;
	struct nvmeibs_disk_info *disk = cl->di;
	struct wire_get_io_disks_info_base *di = wire_buf;
	int rv;
	int is_prefered, numa_dist;

	BUG_ON(!wire_buf);
	BUG_ON((const void *)(di + 1) > wire_buf_end);

	memcpy(di->id, disk->disk_id, sizeof(di->id));

	is_prefered = nvmeibs_disk_is_prefered_port(
		disk, cl->net->params.port);
	di->prefered = cpu_to_be32(is_prefered);

	numa_dist = nvmeibs_disk_nic_numa_dist(disk, cl->net->qp->device);
	di->numa_dist = cpu_to_be32(numa_dist);

	di->exising_conn_num = cpu_to_be32(nvmeib_ref_read(&cl->net->params.port->n_port_conns));

	rv = sizeof(*di);
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_alloc_disk_srv_ext1_encode)
{
	struct nvmeibs_client *cl = arg;
	struct wire_get_io_disks_info_ext1 *di = wire_buf;
	int rv;

	BUG_ON(!wire_buf);
	BUG_ON((const void *)(di + 1) > wire_buf_end);

	di->nrch_ioreq_num = cpu_to_be32(cl->nrch_ioreq_num);

	rv = sizeof(*di);
	return rv;
}

static ssize_t add_disks_info(struct nvmeibs_client *cl, void *head, const void *lim)
{
	struct nvmeibs_disk_info *disk = cl->di;
	int ndisks = disk ? 1 : 0;
	struct nvmeib_container *c;
	int i;
	ssize_t rv;

	rv = vex_encode_container_hdr(&c, cl->vex_ach_ops[vex_ach_get_io_alloc_disk], head, lim);
	if (rv < 0) {
		goto out;
	}

	if (!ndisks)
		nvmeib_container_empty_init(c);
	else {
		for (i = 0; i < ndisks; i++, disk++) {
			rv = CALL_VEX_OP(encode_container_elem,
						vex_ach_get_io_alloc_disk_srv_ops, ONE_EXT,
							base, vex_ach_get_io_alloc_disk_srv_base_encode,
							ext1, vex_ach_get_io_alloc_disk_srv_ext1_encode,
								cl->vex_ach_ops[vex_ach_get_io_alloc_disk], c, lim, cl);
			if (rv < 0)
				goto out;
		}
	}

	rv = nvmeib_container_size(c);
	_NT(add_disks_info_t1, "add @INT disks info to container size=@ZU\n", ndisks, rv);
out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_port_info_srv_base_encode)
{
	struct nvmeibs_config_get_io_ctx *ctx = arg;
	struct nvmeibs_client *cl = ctx->cl;
	struct nvmeibs_ib_port *port = ctx->port;
	struct wire_get_io_ports_info_base *pi = wire_buf;
	struct nvmeibs_disk_info *disk = cl->di;
	int n_msgs = cl->n_msgs;
	int nr_prefered;
	char gid_buf[GUID_SIZE] = {0};
	int rv = -1;
	NFIN;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*pi) > wire_buf_end);

	if (NVMEIB_UPDATE_NW_PATHS) {
		/* write the port HW gid  */
		_NT(trace_client_add_port_info_base, "Sending lnic: hw-gid=@GID_IPV6 (may_access=@MAY_ACCESS)",
			port->gid.hw_gid.raw, !!nvmeibs_ib_port_enabled(port));

		memcpy(pi->hw_gid, port->gid.hw_gid.raw, 16);
		/* write the port active flag  */
		pi->may_access = !!nvmeibs_ib_port_enabled(port);
	}

	memcpy(pi->ib_gid, port->gid.gid.raw, 16);

	/* write the pkey */
	pi->pkey = cpu_to_be32(((u32)port->pkey));
	/* write the hw type of the nic */
	pi->hw_type =
		cpu_to_be32(((u32)nvmeib_get_device_type(P2IB(port))));
	/* save the layer */
	pi->layer = cpu_to_be32(port->layer);
	/* write max messages returned on a data channel -
	   this will limit the size of the S/G list that
	   the client can use
	*/
	pi->n_msgs = cpu_to_be32(n_msgs);
	/* is nic prefered for no-rdda io */
	nr_prefered =
		nvmeibs_disk_is_prefered_port(disk, port) ||
		(nvmeibs_disk_nic_numa_dist(disk, P2IB(port)) ==
		 NVMEIBS_DISK_NIC_NUMA_SAME);
	pi->nr_prefered = cpu_to_be32(nr_prefered);

	format_gid_raw(port->gid.gid.raw, gid_buf);
	_NT(trace_1_client_add_port_info_base, "Sending lnic: @IB_DEV_NAME:@PORT, hw-gid=@GID_IPV6, dev-used=@USED, port-used=@USED, gid=@GID, "
	   "pkey=@PKEY, hw_type=@HW_TYPE, layer=@LAYER_CHR, max_msgs=@MAX_MSGS, nr_prefered=@NR_PREFERED",
	   port->nis_dev->dev->ib_dev->name, port->port,
		port->gid.hw_gid.raw, port->nis_dev->device_used, port->port_used,
	   gid_buf, port->pkey, ((u32)nvmeib_get_device_type(P2IB(port))),
		port->layer == IB_LINK_LAYER_INFINIBAND ? 'I' : 'E',
		n_msgs, nr_prefered);
	rv = sizeof(*pi);

	NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_port_info_srv_ext1_encode)
{
	struct nvmeibs_config_get_io_ctx *ctx = arg;
	struct nvmeibs_ib_port *port = ctx->port;
	struct wire_get_io_ports_info_ext1 *ext1 = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	ext1->transport_type = port->transport;
	ext1->trans_prio = port->transport_priority;
	ext1->numa_dist = nvmeibs_disk_nic_numa_dist(ctx->cl->di, port->nis_dev->dev->ib_dev);
	ext1->bw_prio = 0; /* TBD */
	ext1->lat_prio = 0; /* TBD */

	return sizeof(*ext1);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_port_info_srv_ext2_encode)
{
	struct nvmeibs_config_get_io_ctx *ctx = arg;
	struct nvmeibs_ib_port *port = ctx->port;
	struct wire_get_io_ports_info_ext2 *ext2 = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext2) > wire_buf_end);

	ext2->node_guid = port->nis_dev->dev->ib_dev->node_guid;

	return sizeof(*ext2);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_io_port_info_srv_ext3_encode)
{
	struct wire_get_io_ports_info_ext3 *ext3 = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext3) > wire_buf_end);

	ext3->tcp_base_port = cpu_to_be16(nvmeib_get_tcp_base_port_id());
	ext3->tcp_num_ports = cpu_to_be16(nvmeib_get_tcp_num_ports());

	return sizeof(*ext3);
}

static ssize_t add_port_info(struct nvmeibs_client *cl, struct nvmeib_container *c,
						 const void *e, struct nvmeibs_ib_port *port)
{
	struct nvmeibs_config_get_io_ctx ctx = {
		.cl = cl,
		.port = port,
	};
	int n_msgs = cl->n_msgs;
	char gid_buf[GUID_SIZE] = {0};
	ssize_t rv = -1;
	const struct vex_ops *vctx = cl->vex_ach_ops[vex_ach_get_io_port_info];
	NFIN;

	rv = CALL_VEX_OP(encode_container_elem,
				vex_ach_get_io_port_info_srv_ops, THREE_EXT,
					base, vex_ach_get_io_port_info_srv_base_encode,
					ext1, vex_ach_get_io_port_info_srv_ext1_encode,
					ext2, vex_ach_get_io_port_info_srv_ext2_encode,
					ext3, vex_ach_get_io_port_info_srv_ext3_encode,
						vctx, c, e, &ctx);

	if (rv < 0)
		goto out;

	format_gid_raw(port->gid.gid.raw, gid_buf);
	_NI(add_port_info_i1, "Sending lnic: @STR:@INT, hw-gid=@HW_GID, dev-used=@INT, port-used=@INT, gid=@STR, "
	   "pkey=@INT32_HEX, hw_type=@INT, layer=@CHAR, max_msgs=@INT",
	   port->nis_dev->dev->ib_dev->name, port->port,
		port->gid.hw_gid.raw, port->nis_dev->device_used, port->port_used,
	   gid_buf, port->pkey, ((u32)nvmeib_get_device_type(P2IB(port))),
		port->layer == IB_LINK_LAYER_INFINIBAND ? 'I' : 'E',
		n_msgs);

out:
	NFOUT;
	return rv;
}

static ssize_t add_ports_info(struct nvmeibs_client *cl, struct nvmeib_container *c, const void *e,
						  struct nvmeibs_dev *nic, struct list_head *port_list)
{
	struct ib_device *ib_dev = nic->dev->ib_dev;
	struct nvmeibs_ib_port *port;
	struct ib_port_attr a;
	ssize_t rv;
	NFIN;

	list_for_each_entry(port, port_list, port_list_n) {
		if (port->layer != nvmeibs_selected_layer) {
			/* assume nvmeibs_client_registered is true,
			   o/w how could have the client connected */
			_NI(add_ports_info_i1, "Skip hw-gid=@HW_GID of link-layer=@INT (selected-layer=@INT)",
			   port->gid.hw_gid.raw, port->layer, nvmeibs_selected_layer);
			continue;
		}

		if ((rv = ib_query_port(ib_dev, port->port, &a)) < 0) {
			if (port_list == &nic->port_list) {
				_NE(add_ports_info_e1, "ib_query_port() of used port failed.");
				goto out;
			}
			//EC-1921 - skip unresponsive 'unused' port, failover to it will
			//only be possible if (re)discover runs after it's back to life.
			_NT(add_ports_info_t1, "Failed ib-query non-used port, continue without it");
			continue;
		}

		if ((rv = add_port_info(cl, c, e, port)) < 0) {
			_NE(add_ports_info_e2, "Failed to add port info");
			goto out;
		}
	}
	list_rotate_left(port_list);
	rv = 0;

out:

	NFOUT;
	return rv;
}

static ssize_t add_nics_info(struct nvmeibs_client *cl, void *ps, const void *e)
{
	struct list_head *nics;
	struct nvmeibs_dev *nic;
	int n_nics;
	struct nvmeib_container *ports_ctr;
	ssize_t rv;

	NFIN;
	rv = vex_encode_container_hdr(&ports_ctr, cl->vex_ach_ops[vex_ach_get_io_port_info], ps, e);
	if (rv < 0) {
		goto out;
	}

	/* used devices */
	nics = nvmeibs_get_devices(&n_nics);
	list_for_each_entry(nic, nics, nvmeibs_dev_list_n) {
		/* used ports */
		if ((rv = add_ports_info(cl, ports_ctr, e, nic,
								 &nic->port_list)) < 0) {
			goto err;
		}
		/* unused ports */
		if ((rv = add_ports_info(cl, ports_ctr, e, nic,
								 &nic->unused_port_list)) < 0) {
			goto err;
		}
	}
	list_rotate_left(nics);

	/* unused devices */
	nics = nvmeibs_get_unused_devices_no_lock(&n_nics);
	list_for_each_entry(nic, nics, nvmeibs_dev_list_n) {
		if (!list_empty(&nic->port_list)) {
			_NE(error_1_client_add_nics_info, "Unused dev has used ports");
			goto err;
		}

		/* unused ports */
		if ((rv = add_ports_info(cl, ports_ctr, e, nic,
								 &nic->unused_port_list)) < 0) {
			goto err;
		}
	}
	list_rotate_left(nics);

	_NI(add_nics_info_i1, "Sending total # of ports @UINT (container size @ZU)",
	   nvmeib_container_n_elem(ports_ctr), nvmeib_container_size(ports_ctr));
	rv = nvmeib_container_size(ports_ctr);
	goto unlock;

err:
	rv = -1;

unlock:
	nvmeibs_put_devices();

out:
	NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_shared_cfg_srv_base_decode)
{
	const struct volume_client_config_share_base *conf = wire_buf;
	const struct volume_client_config_share_const_elem *const_elem;
	int i;
	ssize_t rv;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*conf) > wire_buf_end);

	if (!nvmeib_container_validate(&conf->share_const_ctnr, ops->magic_val, ops->vex_ext)) {
		rv = -EPROTO;
		goto out;
	}

#define XCMP(_id, _cmp, _const) \
	if (strncmp(const_elem->const_name, #_const, sizeof(const_elem->const_name)) == 0) { \
		if (!(const_elem->const_val _cmp cpu_to_be64(_const))) { \
			_NE(XCMP_e ##_id, #_const " (@INT_ULLONG) " #_cmp " @INT_ULLONG match failed on version @_X", \
				be64_to_cpu(const_elem->const_val), (u64)_const, be64_to_cpu(conf->version)); \
			rv = -EINVAL; \
			goto out; \
		} else { \
			_NT(XCMP_t ##_id, #_const " (@INT_ULLONG) " #_cmp " @INT_ULLONG match - version @_X", \
				be64_to_cpu(const_elem->const_val), (u64)_const, be64_to_cpu(conf->version)); \
			continue; \
		} \
	}

	for_each_nvmeib_container_elem_cnst_stride(&conf->share_const_ctnr, i, const_elem) {
		/*
		* https://docs.google.com/document/d/1dMSGjiEBT2QUB8dlCaFLlMvitEhg_pcKElLnQZsa0Ns/edit?usp=sharing
		*/
		XCMP(XCMP_1, <=, NVMEIB_COMPAT_MAX_NR_CHANNELS_PER_PATH);
		XCMP(XCMP_2, <=, NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT);
		XCMP(XCMP_3, <=, NVMEIB_MAX_NORDDA_IO_REQ);
		XCMP(XCMP_4, ==, VOLUME_SERVER_MAX_ARRAY_SIZE);
		XCMP(XCMP_5, <=, NVMEIBS_MAX_ADMIN_MSG_SIZE);
		XCMP(XCMP_6, ==, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
		XCMP(XCMP_7, ==, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
		XCMP(XCMP_8, ==, LOCAL_DISK_STATUS_STR_LEN);
		XCMP(XCMP_9, <=, MAX_PORTS_FOR_LOCKS_GIDS);
		XCMP(XCMP_10, ==, NVMEIB_IB_DEVICE_NAME_MAX);
		XCMP(XCMP_11, ==, NVMEIB_GID_STR_MAX);
		XCMP(XCMP_12, ==, NVMEIB_HOST_NAME_LEN);
		XCMP(XCMP_13, ==, NVMEIB_MAX_KERN_VER_STRLEN);
		XCMP(XCMP_14, ==, NVMEIB_MAX_OFED_VER_STRLEN);
		XCMP(XCMP_15, ==, NVMEIBS_LOST_SRV_RESOURCE_PAYLOAD_SIZE);
		XCMP(XCMP_16, <=, NVMEIBS_NORDDA_SERVER_MSG_SIZE);
		XCMP(XCMP_17, <=, NVMEIBS_CONFIG_MSG_RDMA_PAGES);
		XCMP(XCMP_18, ==, NVMEIB_IOCH_KA_WRITE_LEN);
		XCMP(XCMP_19, >=, NVMEIBC_MAX_ADMIN_CLIENT_MSG_SIZE);
		XCMP(XCMP_20, >=, NVMEIBC_NORDDA_CLIENT_MSG_SIZE);
	}
	rv = 0;

out:
	return rv;
#undef XCMP
}

static void verify_link_config(struct nvmeibs_client *cl, struct nvmeib_iu *recv_ioctx,
	struct nvmeib_iu *send_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_shared_cfg];
	void *payload = NULL;
	int rv, payload_len = 0;
	u8 opcode;

	if (be16_to_cpu(req->version_tag) != vex_ops->vex_ext) {
		_NT(verify_link_config_t1, "Unexpected version @UINT in shared_cfg request", be16_to_cpu(req->version_tag));
		opcode = NVMEIBS_RSP_MGMT_OPCODE_ERR;
		rv = -EPROTO;
		goto send_rsp;
	}
	rv = CALL_VEX_OP(decode,
				vex_ach_shared_cfg_srv_ops, BASE_ONLY,
					base, vex_ach_shared_cfg_srv_base_decode,
						vex_ops, NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_PAYLOAD(req),
					recv_ioctx->buf + recv_ioctx->size, NULL);
	opcode = (!rv ? NVMEIBS_RSP_MGMT_OPCODE_OK : NVMEIBS_RSP_MGMT_OPCODE_ERR);

send_rsp:
	_NT(trace_client_verify_link_config, "NVMEIBC_MA_SHARE_CONFIG host @CL_NAME config @TRUE_FALSE_STR", cl->name, !rv ? "OK":"ERR");
	nvmeibs_client_send_rsp(cl, cl->net, opcode,
							req->hdr.tag, req->version_tag, send_ioctx, payload, payload_len, NVMEIB_SEND_CFG, NON_NR_VERSION);

}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_io_srv_base_decode)
{
	struct nvmeibs_config_get_io_ctx *cmd_ctx = arg;
	const struct volume_client_config_ma_get_io_base *io_req = wire_buf;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*io_req) > wire_buf_end);

	cmd_ctx->rdma = io_req->rdma;
	memcpy(cmd_ctx->client_name, io_req->client_name, sizeof(io_req->client_name));
	memcpy(cmd_ctx->disk_name, io_req->disk_name, sizeof(cmd_ctx->disk_name));

	return sizeof(*io_req);
}

/* nvmeibs_get_io_info(): Get controller disk and nic info */
static void get_io_info(struct nvmeibs_client *cl, struct nvmeib_iu *recv_ioctx,
	struct nvmeib_iu *send_ioctx)
{
	struct nvmeibs_config_get_io_ctx cmd_ctx = {};
	struct volume_client_req *req = recv_ioctx->buf;
	void *payload = NULL;
	int payload_len = 0;
	int rv;
	void *p = cl->out_msg_area;
	const void *e = cl->out_msg_area_end;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_get_io];
	DECLARE_COMPLETION_ONSTACK(send_done);

	cmd_ctx.cl = cl;
	cmd_ctx.req = req;

	__NFIN;
	_NT(trace_client_get_io_info, "--- Start handling NVMEIBC_MA_GET_IO message from host @CL_NAME",
		cl->name);

	/* clear the outgoing message area */
	memset(cl->out_msg_area, 0xcc, NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);

	if (be16_to_cpu(req->version_tag) != vex_ops->vex_ext) {
		_NT(get_io_info_t1, "Unexpected version @UINT in shared_cfg request", be16_to_cpu(req->version_tag));
		rv = NVMEIBS_RSP_MGMT_OPCODE_ERR;
		goto send_rsp;
	}

	CALL_VEX_OP(decode,
			vex_ach_get_io_srv_ops, BASE_ONLY,
				base, vex_ach_get_io_srv_base_decode,
					vex_ops,
					NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_CONST_PAYLOAD(req),
					recv_ioctx->buf + recv_ioctx->size, &cmd_ctx);

	/* find disk and set cl->di */
	if (cl_disk_get(cl, cmd_ctx.client_name, cmd_ctx.disk_name) < 0)
		_NE(error_client_get_io_info, "Fail to get disk"); /* prepare rsp with 0 disks */

	/* write the disks */
	rv = add_disks_info(cl, p, e);

	if (rv < 0)
		goto send_rsp;

	p += rv;

	if (cl->di) {
		rv = add_nics_info(cl, p, e);
		if (rv < 0)
			goto send_rsp;
		p += rv;
	}

	rv = send_rdma_msg(cl, &cmd_ctx.rdma, p - cl->out_msg_area);
	if (!rv)
		send_ioctx->io_done = &send_done;

send_rsp:
	nvmeibs_client_send_rsp(cl, cl->net,
		rv == 0 ? NVMEIBS_RSP_MGMT_OPCODE_OK : NVMEIBS_RSP_MGMT_OPCODE_ERR,
		req->hdr.tag, req->version_tag, send_ioctx, payload, payload_len, NVMEIB_SEND_CFG, NON_NR_VERSION);
	if (send_ioctx->io_done && (rv = wait_for_completion_timeout(&send_done, NVMEIB_WAIT_FOR_ADMIN_SEND_COMP)) <= 0) {
		_NT(trace_client_get_io_info_time_out, "Timed out (@RV) waiting for send comp from host @CL_NAME",
			rv, cl->name);
		rv = -ETIMEDOUT;
	} else {
		rv = 0;
	}
	send_ioctx->io_done = NULL;

	_NT(trace_1_client_get_io_info, "--- Finish handling NVMEIBC_MA_GET_IO message from host @CL_NAME (rv @RV)",
		cl->name, rv);

	/* Now that client's name is set, create it procfs dir */
	if (cl->di && !rv) {
		if (single_sclient_proc_mkdir(cl))
			nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_PROCFS_ERR);
	}

	__NFOUT;
}

/* Called with nvmeibs_dev_guard lock taken */
static bool find_anic(struct list_head *anics, const void *p)
{
	bool found = false;
	struct nvmeibs_dev *nic;
	struct nvmeibs_ib_port *port;

	NFIN;
	list_for_each_entry(nic, anics, nvmeibs_dev_list_n) {
		list_for_each_entry(port, &nic->port_list, port_list_n) {
			if (!memcmp(p, port->gid.gid.raw, 16)) {
				found = true;
				goto out;
			}
		}
	}

out:
	NFOUT;
	return found;
}

/* Called with nvmeibs_dev_guard lock already taken */
static struct  nvmeibs_ib_port *find_lnic(struct list_head *nics, const char *gid)
{
	struct nvmeibs_dev *nic;
	struct nvmeibs_ib_port *port, *rv = NULL;

	NFIN;
	list_for_each_entry(nic, nics, nvmeibs_dev_list_n) {
		list_for_each_entry(port, &nic->port_list, port_list_n) {
			if (!memcmp(gid, port->gid.gid.raw, 16)) {
				rv = port;
				goto out;
			}
		}
	}

out:
	NFOUT;
	return rv;
}

static struct  nvmeibs_ib_port *find_lnic_by_hw_gid(struct list_head *nics, const char *hw_gid)
{
	struct nvmeibs_dev *nic;
	struct nvmeibs_ib_port *port, *rv = NULL;
	int size;

	NFIN;
	list_for_each_entry(nic, nics, nvmeibs_dev_list_n) {
		list_for_each_entry(port, &nic->port_list, port_list_n) {
			if (!memcmp(hw_gid, port->gid.hw_gid.raw, 16)) {
				rv = port;
				goto out;
			}
		}
		list_for_each_entry(port, &nic->unused_port_list, port_list_n) {
			if (!memcmp(hw_gid, port->gid.hw_gid.raw, 16)) {
				rv = port;
				goto out;
			}
		}
	}

	nics = nvmeibs_get_unused_devices_no_lock(&size);
	list_for_each_entry(nic, nics, nvmeibs_dev_list_n) {
		list_for_each_entry(port, &nic->unused_port_list, port_list_n) {
			if (!memcmp(hw_gid, port->gid.hw_gid.raw, 16)) {
				rv = port;
				goto out;
			}
		}
	}


out:
	NFOUT;
	return rv;
}

struct nvmeibs_access_map_decode_ctx {
	struct nvmeibs_client *cl;
	struct list_head *anics;
	struct list_head *disks;
	struct nvmeibs_anic *anic;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_lionic *lionic;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_clnt_ionics_srv_base_decode)
{
	struct nvmeibs_access_map_decode_ctx *ctx = arg;
	struct nvmeibs_lionic *lionic = ctx->lionic;
	const struct wire_acs_map_clnt_ionics_base *rnic = wire_buf;
	struct nvmeibs_rionic *rionic;
	int rv;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(*rnic) > wire_buf_end);

	if (elem_idx == 0) {
		lionic->n_rionics = n_elem;
		_NT(vex_ach_acs_map_clnt_ionics_srv_base_decode_t1, "# of remote io nics to access the controller nic @UINT", lionic->n_rionics);
		if (!(lionic->rionics = kcalloc(n_elem, sizeof(*lionic->rionics), GFP_KERNEL))) {
			_NE(vex_ach_acs_map_clnt_ionics_srv_base_decode_e1, "Fail to allocate client remote nic array");
			lionic->n_rionics = 0;
			rv = -ENOMEM;
			goto out;
		}
	}

	_NT(vex_ach_acs_map_clnt_ionics_srv_base_decode_t2, "Adding remote io nic [@INT/@INT] - @GID",
	 elem_idx, n_elem, &rnic->gid);

	rionic = &lionic->rionics[elem_idx];
	rionic->lionic = lionic;
	memcpy(rionic->gid.raw, rnic->gid, 16);
	mutex_init(&rionic->keep_alive.lock);

	if (NVMEIB_UPDATE_NW_PATHS) {
		rionic->may_access = rnic->may_access;
	}

	rv = sizeof(*rnic);

out:
	return rv;
}

/* Called with nvmeibs_dev_guard lock already taken */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_srv_ionics_srv_base_decode)
{
	struct nvmeibs_access_map_decode_ctx *ctx = arg;
	struct nvmeibs_client *cl = ctx->cl;
	struct nvmeibs_client_disk *cdisk = ctx->cdisk;
	struct nvmeibs_ib_port *port;
	struct list_head *nics;
	int rv = -1, n_nics;
	const struct wire_acs_map_srv_ionics_base *lnic = wire_buf;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_acs_map_clnt_ionics];

	NFIN;
	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(lnic + 1) > wire_buf_end);

	if (elem_idx == 0) {
		cdisk->n_lionics = n_elem;
		_NT(trace_client_load_lnics_base, "# of controller io nics the client asks to use @N_LIONICS", cdisk->n_lionics);

		if (cdisk->n_lionics <= 0) {
			_NE(error_client_load_lnics_base, "Invalid number of controller nics: @N_LIONICS", cdisk->n_lionics);
			rv = -EINVAL;
			goto out;
		}

		if (!(cdisk->lionics = kcalloc(n_elem, sizeof(*cdisk->lionics), GFP_KERNEL))) {
		_NE(error_1_client_load_lnics_base, "Fail to allocate client local nic array");
			cdisk->n_lionics = 0;
			rv = -ENOMEM;
			goto out;
		}
	}

	_NT(vex_ach_acs_map_srv_ionics_srv_base_decode_t1,
	 "Handling local io nic [@INT/@INT] @GID", elem_idx, n_elem, &lnic->gid);

	nics = nvmeibs_get_devices_no_lock(&n_nics);

	if (!NVMEIB_UPDATE_NW_PATHS) {
		if (!(port = find_lnic(nics, lnic->gid))) {
			_NE(vex_ach_acs_map_srv_ionics_srv_base_decode_e1, "Did not find local nic @GID", &lnic->gid);
			rv = -ENOENT;
			goto free_lionics;
		} else
			_NT(vex_ach_acs_map_srv_ionics_srv_base_decode_t2, "Found local nic @GID type @INT", &lnic->gid, port->hw_type);
	}
	else {
		if (!(port = find_lnic_by_hw_gid(nics, lnic->gid))) {
			_NE(vex_ach_acs_map_srv_ionics_srv_base_decode_e2, "Did not find local nic @GID", &lnic->gid);
			rv = -ENOENT;
			goto free_lionics;
		} else
			_NT(vex_ach_acs_map_srv_ionics_srv_base_decode_t3, "Found local nic @GID type @INT", &lnic->gid, port->hw_type);
	}

	ctx->lionic = &cdisk->lionics[elem_idx];
	ctx->lionic->disk = cdisk;
	ctx->lionic->hw_type = port->hw_type;
	ctx->lionic->may_access = nvmeibs_ib_port_enabled(port); /* ib-port-state active && not filtered-out */
	ctx->lionic->port = port;

	memcpy(ctx->lionic->gid.raw, lnic->gid, 16);

	rv = CALL_VEX_OP(decode_container,
				vex_ach_acs_map_clnt_ionics_srv_ops, BASE_ONLY,
					base, vex_ach_acs_map_clnt_ionics_srv_base_decode,
						vex_ops, &lnic->clnt_ionics_map, wire_buf_end, ctx);

	if (rv < 0)
		goto free_lionics;

	rv += (const void *)&lnic->clnt_ionics_map - wire_buf;
	BUG_ON(wire_buf + rv > wire_buf_end);
	goto out;

free_lionics:
	kfree(cdisk->lionics);
	cdisk->lionics = NULL;

out:
	/* Finished processing this lionic */
	ctx->lionic = NULL;
	NFOUT;
	return rv;
}

/* Called with nvmeibs_dev_guard lock already taken */
VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_disks_srv_base_decode)
{
	struct nvmeibs_access_map_decode_ctx *ctx = arg;
	const struct wire_acs_map_disks_base *di = wire_buf;
	struct nvmeibs_client *cl = ctx->cl;
	struct nvmeibs_anic *anic = ctx->anic;
	struct list_head *disks = ctx->disks;
	int rv = -1;
	struct nvmeibs_disk_info *disk;
	bool found = false;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_acs_map_srv_ionics];

	NFIN;
	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(di + 1) > wire_buf_end);

	if (elem_idx == 0) {
		anic->n_disks = n_elem;
		if (!(anic->disks = kcalloc(n_elem, sizeof(*anic->disks), GFP_KERNEL))) {
			_NE(vex_ach_acs_map_disks_srv_base_decode_e1, "Failed to allocate anic disk array");
			rv = -ENOMEM;
			goto out;
		}
	}

	list_for_each_entry(disk, disks, link) {
		if (!strncmp(di->disk_id, disk->disk_id,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			_ND(vex_ach_acs_map_disks_srv_base_decode_d1, "Found a match");
			anic->disks[elem_idx].di = disk;
			found = true;
			break;
		}
	}

	if (!found) {
		_NT(vex_ach_acs_map_disks_srv_base_decode_t1, "Client ask for disk @STR that we do not have",
			di->disk_id);
		rv = -ENOENT;
		goto free_disks;
	}

	ctx->cdisk = &anic->disks[elem_idx];

	rv = CALL_VEX_OP(decode_container,
				vex_ach_acs_map_srv_ionics_srv_ops, BASE_ONLY,
					base, vex_ach_acs_map_srv_ionics_srv_base_decode,
						vex_ops, &di->srv_ionics_map, wire_buf_end, ctx);
	if (rv < 0)
		goto free_disks;

	rv += (const void *)&di->srv_ionics_map - wire_buf;
	BUG_ON(wire_buf + rv > wire_buf_end);
	goto out;

free_disks:
	kfree(anic->disks);
	anic->disks = NULL;

out:
	/* Finished processing this disk */
	ctx->cdisk = NULL;
	NFOUT;
	return rv;
}

static void nr_channels_free(struct nvmeibs_nr_channel *nr_channels, int n)
{
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	void *val;
#endif
#if 0
	int i;
#endif
	NFIN;

	if (nr_channels) {
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
		if ((val = nvmeib_s_tree_lookup((unsigned long)nr_channels)) != nr_channels) {
			_NE(nr_channels_free_e1, "OOPS, nr_channels=@PTR (val=@PTR) was deleted",
				nr_channels, val);
			goto out;
		}
#endif
#if 0
		for (i = 0; i < n; i++)
			vfree(nr_channels[i].io_cmds);
#endif
		vfree(nr_channels[0].io_cmds);
#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
		nvmeib_s_tree_del((unsigned long)nr_channels);
#endif
		kfree(nr_channels);
	}

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
out:
#endif
	NFOUT;
}

static struct nvmeibs_nr_channel *nr_channels_alloc(u32 nrch_ioreq_num, int n)
{
	struct nvmeibs_nr_channel *nr_channels;
	u64 mem_size;
	int i;
	NFIN;

	if (!(nr_channels = kzalloc(sizeof(*nr_channels) * n, GFP_KERNEL))) {
		_NE(error_client_nr_channels_alloc, "Fail to allocate nr_channels");
		goto out;
	}

	mem_size = sizeof(*(nr_channels->io_cmds)) * nrch_ioreq_num;

#if defined(DEBUG_USING_RADIX) && DEBUG_USING_RADIX
	nvmeib_s_tree_add((unsigned long)nr_channels, nr_channels);
#endif
#if 0
	for (i = 0; i < n; i++) {
		if (!(nr_channels[i].io_cmds = vzalloc(mem_size))) {
			_NE(error_1_client_nr_channels_alloc, "Fail to allocate io_cmds @NCMDS", i);
			nr_channels_free(nr_channels, i);
			nr_channels = NULL;
			goto out;
		}
	}
#endif
	if (!(nr_channels[0].io_cmds = vzalloc(n * mem_size))) {
		_NE(error_0_client_nr_channels_alloc, "Fail to allocate io_cmds\n");
		nr_channels_free(nr_channels, 0);
		nr_channels = NULL;
		goto out;
	}
	for (i = 1; i < n; i++)
		nr_channels[i].io_cmds = nr_channels[0].io_cmds + i * nrch_ioreq_num;

out:
	NFOUT;
	return nr_channels;
}

static void free_lnics(struct nvmeibs_client_disk *cdisk)
{
	int i, j, k;

	NFIN;
	for (i = 0; i < cdisk->n_lionics; ++i) {
		for (j = 0; j < cdisk->lionics[i].n_rionics; ++j) {
			/*
			 * No-RDDA
			 */
			for (k = 0; k < cdisk->lionics[i].rionics[j].n_nr_channels; ++k) {
				_ND(free_lnics_d1, "Free nrch net @PTR",
					cdisk->lionics[i].rionics[j].nr_channels[k].net);
				kfree(cdisk->lionics[i].rionics[j].nr_channels[k].net);
			}
			_ND(free_lnics_d2, "Free nr_channels array (l,r=@INT32_02,@INT32_02)", i, j);
			nr_channels_free(
				cdisk->lionics[i].rionics[j].nr_channels,
				cdisk->lionics[i].rionics[j].n_nr_channels);

			free_rionic_ka(&cdisk->lionics[i].rionics[j], false);
		}
		kfree(cdisk->lionics[i].rionics);
	}
	kfree(cdisk->lionics);
	cdisk->lionics = NULL;
	cdisk->n_lionics = 0;
	NFOUT;
}

static void free_disks(struct nvmeibs_anic *anic)
{
	int i;

	NFIN;
	for (i = 0; i < anic->n_disks; ++i)
		free_lnics(&anic->disks[i]);
	kfree(anic->disks);
	anic->disks = NULL;
	anic->n_disks = 0;
	NFOUT;
}

static void free_anics(struct nvmeibs_client *cl)
{
	struct nvmeibs_anic *anic, *tmp_anic;

	__NFIN;
	list_for_each_entry_safe(anic, tmp_anic, &cl->anics, link) {
		list_del(&anic->link);
		free_disks(anic);
		kfree(anic);
	}
	__NFOUT;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_acs_map_arnics_srv_base_decode)
{
	int rv;
	struct nvmeibs_access_map_decode_ctx *ctx = arg;
	const struct wire_acs_map_arnic_base *arnic = wire_buf;
	struct nvmeibs_anic *anic = NULL;
	struct nvmeibs_client *cl = ctx->cl;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_acs_map_disks];

	_ND(trace_client_load_anic_base, "Looking for admin gid @GID_IPV6", &arnic->gid);
	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON((const void *)(arnic + 1) > wire_buf_end);

	if (!(find_anic(ctx->anics, &arnic->gid))) {
		_NE(error_client_load_anic_base, "Fail to find admin nic gid @GID_IPV6", &arnic->gid);
		rv = -ENOENT;
		goto out;
	}

	_ND(trace_1_client_load_anic_base, "We found an admin gid the client is looking for");
	if (!(anic = kzalloc(sizeof(*anic), GFP_KERNEL))) {
		_NE(error_1_client_load_anic_base, "Fail to allocate admin nic");
		rv = -ENOMEM;
		goto out;
	}

	/* save the gid */
	memcpy(anic->gid.raw, arnic->gid, 16);

	ctx->anic = anic;

	rv = CALL_VEX_OP(decode_container,
		vex_ach_acs_map_disks_srv_ops, BASE_ONLY,
			base, vex_ach_acs_map_disks_srv_base_decode,
				vex_ops, &arnic->disks_map, wire_buf_end, ctx);
	if (rv < 0)
		goto free_anic;

	list_add_tail(&anic->link, &cl->anics);
	rv += (const void *)&arnic->disks_map - wire_buf;
	BUG_ON(wire_buf + rv > wire_buf_end);
	goto out;

free_anic:
	if (anic && anic->disks)
		kfree(anic->disks);
	kfree(anic);

out:
	/* Finished processing this anic */
	ctx->anic = NULL;
	return rv;
}

static int parse_maps(struct nvmeibs_client *cl, struct volume_client_req *req, const void *ps, const void *e)
{
	int rv = 0;
	struct nvmeibs_access_map_decode_ctx arnic_ctx = {
		.cl = cl,
	};
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_acs_map_arnics];

	__NFIN;
	if (be16_to_cpu(req->version_tag) != vex_ops->vex_ext) {
		_NT(parse_maps_t1, "Invalid NVMEIBC_MA_GET_ACCESS version @UINT", be16_to_cpu(req->version_tag));
		rv = -EPROTO;
		goto out;
	}

	/* To preserve the convention of system rscs lock order,
	   lock the s_disk in advance, before locking the nics */
	arnic_ctx.disks = nvmeibs_disk_get_disks(NULL);
	arnic_ctx.anics = nvmeibs_get_devices(NULL);

	rv = CALL_VEX_OP(decode_container,
					vex_ach_acs_map_arnics_srv_ops, BASE_ONLY,
						base, vex_ach_acs_map_arnics_srv_base_decode,
							vex_ops, ps, e, &arnic_ctx);
	if (rv < 0)
		goto free_anics;

	BUG_ON(ps + rv > e);

	rv = 0;
	goto unlock;

free_anics:
	free_anics(cl);

unlock:
	nvmeibs_put_devices();
	nvmeibs_disk_put_disks();

out:
	__NFOUT;
	return rv;
}

static int build_disks_list(struct nvmeibs_client *cl)
{
	struct list_head *anics = &cl->anics;
	struct list_head *disks = &cl->disks;
	struct nvmeibs_anic *anic;
	struct nvmeibs_client_disk *cdisk;
	int n_disks = 0;
	bool found;
	int i;
	u64 disk_id = 0;
	int rv = 0;

	__NFIN;
	cl->client_io_cmd_n_pages = nvmeibs_get_max_pages_in_fmr();
	/* scan all admin nics */
	list_for_each_entry(anic, anics, link)
		/* for all the disks that this admin needs to access */
		for (i = 0; i < anic->n_disks; ++i) {
			_ND(build_disks_list_d1, "Trying to add disk @STR", anic->disks[i].di->disk_id);
			found = false;
			/* check if we already aware of the that disk */
			list_for_each_entry(cdisk, disks, link)
				if (cdisk->di == anic->disks[i].di) {
					found = true;
					break;
				}
			if (!found) {
				/* a new disk so add it to our list */
				cdisk = &anic->disks[i];
				list_add_tail(&cdisk->link, disks);
				cdisk->id = disk_id++;
				++n_disks;
				cl->client_io_cmd_n_pages = nvmeibs_nvme_max_io_bb(
					cdisk->di ? cdisk->di->dev : NULL);
				_ND(build_disks_list_d2, "Adding client disk @STR - @LLD",
					cdisk->di->disk_id, cdisk->id);
			}
		}

	if (n_disks) {
		cl->n_disks = n_disks;
		if (cl->client_io_cmd_n_pages <= 0) {
			_NE(build_disks_list_e1, "client @UINT (@CLIENT_UUID) has invalid client_io_cmd_n_pages value: @INT",
			   cl->cid, &cl->client_uuid, cl->client_io_cmd_n_pages);
			rv = -1;
		} else {
			_NT(trace_client_build_disks_list, "The smallest BB has @CLIENT_IO_CMD_N_PAGES pages", cl->client_io_cmd_n_pages);
		}
	}
	else {
		_NE(error_client_build_disks_list, "OOPS no disks were found - how come at this point?");
		rv = -1;
	}

	__NFOUT;
	return rv;
}


static int send_n_wait(struct nvmeibs_client *cl, u64 tag, __be16 version_tag, struct nvmeib_iu *iu,
	void *payload, int payload_len)
{
	int rv;

	__NFIN;

	init_completion(iu->io_done);

	iu->io_status = -1;
	if ((rv = nvmeibs_client_send_rsp(cl, cl->net, NVMEIBS_RSP_MGMT_OPCODE_OK,
		tag, version_tag, iu, payload, payload_len, NVMEIB_SEND_CFG, NON_NR_VERSION)) < 0) {
		_NT(trace_client_send_n_wait, "Failed to send the P part of the access");
		goto out;
	}
	if ((rv = wait_for_completion_interruptible_timeout(iu->io_done,
		NVMEIB_WAIT_FOR_ADMIN_SEND_COMP)) <= 0 || iu->io_status != IB_WC_SUCCESS) {
		_NE(trace_1_client_send_n_wait, "Failed to wait for the P part to finish rv=@RV "
				"io_status=@IO_STATUS send_ioctx @PTR client @CL net @NET qp @QP",
			rv, iu->io_status, iu, cl, cl->net, cl->net->qp);
		rv = -1;
		goto out;
	}
	rv = 0;

out:
	nvmeib_reinit_completion(iu->io_done);
	__NFOUT;
	return rv;
}

static int client_disk_resource_sets_count(struct nvmeibs_client_disk *cdisk,
					   struct list_head **first_ent,
					   struct list_head **end_ent)
{

	struct list_head *mems = &cdisk->di->mem_priv_list;
	struct disk_mapped_mem_info *mem;
	int n = 0;

	NFIN;

	list_for_each_entry(mem, mems, link) {
		++n;
	}
	if (first_ent)
		*first_ent = mems->next;
	if (end_ent)
		*end_ent = mems;

	NFOUT;
	return n;
}

//detemine the mac amount of io channel per localnic - remotenic.
// the client will not create more channles that this number
static int distribute_lionic_resources(struct nvmeibs_client *cl, struct nvmeibs_client_disk *cdisk, unsigned req_nrch_per_path)
{
	int total_rionics = 0, n_rionics = 0;
	int per_client, per_rionic = 0, extra __attribute__((unused));
	int i, j, k, n __attribute__((unused));
	bool first __attribute__((unused)) = true;
	char guid[GUID_SIZE];

	NFIN;
	per_client = min(NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT, cdisk->di->n_qs);
	for (i = 0; i < cdisk->n_lionics; ++i) {
		format_gid_raw(cdisk->lionics[i].gid.raw, guid);
		n_rionics += cdisk->lionics[i].n_rionics;
		if (!cdisk->lionics[i].may_access)
			_NT(distribute_lionic_resources_t2, "lnic @STR, no access, skip", guid);
		else {
			_NT(distribute_lionic_resources_t3, "Using lnic @STR", guid);
			for (j = 0; j < cdisk->lionics[i].n_rionics; ++j) {
				total_rionics += !!(cdisk->lionics[i].rionics[j].may_access);
			}
		}
	}
	if (total_rionics > 0) {
		per_rionic = DIV_ROUND_UP(per_client, total_rionics);
		_NT(trace_client_distribute_lionic_resources,
			"per_client=@INT, per_rionic=@PER_RIONIC --> "
			"total_rionics=@TOTAL_RIONICS", per_client, per_rionic, total_rionics);
	} else {
		_NT(trace_1_client_distribute_lionic_resources, "No available rdda lnics");
		goto no_rdda;
	}
	extra = per_rionic ? min(per_rionic * total_rionics - per_client,
		per_rionic - 1) : 0;
	n = 0;
	for (i = 0; i < cdisk->n_lionics; ++i) {
		if (!cdisk->lionics[i].may_access) {
			format_gid_raw(cdisk->lionics[i].gid.raw, guid);
			_NT(distribute_lionic_resources_t4, "Not using lnic @STR", guid);
			continue;
		}
		for (j = 0; j < cdisk->lionics[i].n_rionics; ++j) {
			if (!cdisk->lionics[i].rionics[j].may_access) {
				_NT(distribute_lionic_resources_t5, "Not using rnic @GID",
					&cdisk->lionics[i].rionics[j].gid);
				continue;
			}
			first = false;
		}
	}

no_rdda:
	/*
	 * No-RDDA
	 */
	for (i = 0; i < cdisk->n_lionics; ++i) {
		for (j = 0; j < cdisk->lionics[i].n_rionics; ++j) {
			if (cdisk->lionics[i].hw_type == DT_siw)
				cdisk->lionics[i].rionics[j].n_nr_channels = min_t(int, req_nrch_per_path, cl->max_nrchs_per_path_tcp);
			else
				cdisk->lionics[i].rionics[j].n_nr_channels = min_t(int, req_nrch_per_path, cl->max_nrchs_per_path_rdma);
			_ND(distribute_lionic_resources_d2, "disk @STR: lionic=@INT, rionic=@INT, n_qps=@INT",
				cdisk->di->disk_id, i,j, per_rionic);
			if (cdisk->lionics[i].rionics[j].n_nr_channels &&
				!(cdisk->lionics[i].rionics[j].nr_channels =
				  nr_channels_alloc(cl->nrch_ioreq_num, cdisk->lionics[i].rionics[j].n_nr_channels))) {
				_NE(distribute_lionic_resources_e2, "OOM: cannot allocate nr channels for "
					"disk @STR (l=@INT, r=@INT, n=@INT)", cdisk->di->disk_id, i, j,
					cdisk->lionics[i].rionics[j].n_nr_channels);
				cdisk->lionics[i].rionics[j].n_nr_channels = 0;
				n_rionics = -1;
				goto out;
			}
			else {
				/* init the nr_channels */
				for (k = 0;
					  k < cdisk->lionics[i].rionics[j].n_nr_channels;
					  ++k) {
					_ND(distribute_lionic_resources_d3, "init nr_channel @INT, @PTR (l,r=@INT32_02,@INT32_02)",
						k, &cdisk->lionics[i].rionics[j].nr_channels[k], i, j);
					cdisk->lionics[i].rionics[j].nr_channels[k].id = -1;
					cdisk->lionics[i].rionics[j].nr_channels[k].rionic =
						&cdisk->lionics[i].rionics[j];
				}
			}
		}
	}

out:
	NFOUT;
	return n_rionics;
}

struct disk_info_enc_ctx {
	struct nvmeibs_client *cl;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_disk_private_data *dpd;
	int tot_rionics;
	int n_rsc_sets;
	struct list_head *curr_mem_link;
	struct list_head *end_mem_head;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_disk_info_srv_base_encode)
{
	struct volume_server_config_access_map_per_disk_rsp_base *per_disk_rsp = wire_buf;
	struct disk_info_enc_ctx *ctx = arg;
	int tot_rionics = ctx->tot_rionics;
	int n_rsc_sets = ctx->n_rsc_sets;
	struct nvmeibs_client_disk *cdisk = ctx->cdisk;
	struct nvmeibs_disk_private_data *dpd = ctx->dpd;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*per_disk_rsp) > wire_buf_end);

	/* write the data */
	/* the disk name */
	memcpy(per_disk_rsp->disk_name, cdisk->di->disk_id,
	       NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	/* add the nsid */
	per_disk_rsp->nsid = cpu_to_be32((u32)cdisk->di->nsid);
	/* add the block shift and metadata info (ns format) */
	per_disk_rsp->sector_shift = cpu_to_be32((u32)cdisk->di->block_shift);
	per_disk_rsp->md.md_size = cpu_to_be32(cdisk->di->metadata);
	per_disk_rsp->md.md_extd = cdisk->di->mtdt_extd;
	/* add the max dma/request size */
	per_disk_rsp->max_request_size = cpu_to_be32((u32)cdisk->di->max_request_size);
	/* add the n qps */
	per_disk_rsp->triplets = cpu_to_be32(tot_rionics);
	per_disk_rsp->disk_rscs_n_sets = cpu_to_be32(n_rsc_sets);
	per_disk_rsp->disk_rscs = cpu_to_be32(cdisk->n_disk_rsrc);
	per_disk_rsp->client_rscs = cpu_to_be32(max_client_rsrc);
	/* disk pd */
	per_disk_rsp->disk_lock_counter =
		cpu_to_be64(atomic64_inc_return(&dpd->disk_lock_counter) + 1);
	per_disk_rsp->disk_lock_segments =
		cpu_to_be32(dpd->n_memsegs);

	rv = sizeof(*per_disk_rsp);

	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_di_rsrc_set_srv_base_encode)
{
	struct wire_disk_rsc_set_base *base = wire_buf;
	struct disk_info_enc_ctx *ctx = arg;
	struct disk_mapped_mem_info *mem;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*wire_buf) > wire_buf_end);

	if (ctx->curr_mem_link == ctx->end_mem_head) {
		_NT(trace_vex_ach_acs_map_di_rsrc_set_srv_base_encode_no_ent,
		    "mem list ended prematurely");
		rv = -ENOENT;
		goto out;
	}

	mem = list_entry(ctx->curr_mem_link, typeof(*mem), link);

	base->node_guid = mem->node_guid;

	ctx->curr_mem_link = ctx->curr_mem_link->next;

	rv = sizeof(*base);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_acs_map_disk_info_srv_ext1_encode)
{
	struct volume_server_config_access_map_per_disk_rsp_ext1 *ext1 = wire_buf;
	struct disk_info_enc_ctx *ctx = arg;
	struct nvmeibs_client *cl = ctx->cl;
	const struct vex_ops *rsrc_set_vex_ops = cl->vex_ach_ops[vex_ach_acs_map_di_rsrc_set];
	struct nvmeib_container *ctnr;
	int i;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	if (ctx->n_rsc_sets == 0) {
		nvmeib_container_empty_init(&ext1->disk_rsc_set_ctnr);
		rv = sizeof(*ext1);
	} else {
		if ((rv = vex_encode_container_hdr(&ctnr, rsrc_set_vex_ops, &ext1->disk_rsc_set_ctnr, wire_buf_end)) < 0)
			goto out;
		for (i = 0; i < ctx->n_rsc_sets; i++) {
			if ((rv = CALL_VEX_OP(encode_container_elem,
				vex_ach_acs_map_di_rsrc_set_srv_ops, BASE_ONLY,
					base, vex_ach_acs_map_di_rsrc_set_srv_base_encode,
						rsrc_set_vex_ops, ctnr, wire_buf_end, ctx)) < 0)
				goto out;
		}
		rv = offsetof(typeof(*ext1), disk_rsc_set_ctnr) + nvmeib_container_size(ctnr);
	}

out:
	return rv;
}

struct vex_get_acs_ctx {
	struct nvmeibs_client *cl;
	struct volume_client_config_rdma_info rdma;
	unsigned req_nrch_per_path;
};

static int send_disk_info(struct nvmeibs_client *cl, struct nvmeibs_client_disk *cdisk,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx, struct vex_get_acs_ctx *get_acs_ctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	struct volume_server_rsp *rsp = &cl->rsp;
	void *payload = rsp->payload;
	void *payload_end = rsp->ach_payload_max;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_acs_map_disk_info];
	struct disk_info_enc_ctx enc_ctx = {
		.cl = cl,
		.cdisk = cdisk,
		.dpd = cdisk->di->priv,
	};
	ssize_t rv = 0;
	size_t payload_len;

	__NFIN;
	//determine the max io channels per rionic, allocate the io channel
	if ((enc_ctx.tot_rionics = distribute_lionic_resources(cl, cdisk, get_acs_ctx->req_nrch_per_path)) < 0) {
		_NT(trace_client_send_disk_info, "Cannot distribute lionic resources on disk @DISK_ID_STR tot_res=@TOT_RES",
		    cdisk->di->disk_id, enc_ctx.tot_rionics);
		rv = -1;
		goto out;
	}
	_NT(trace_1_client_send_disk_info, "sent tot_r=@TOT_R", enc_ctx.tot_rionics);

	if (!atomic_read(&cdisk->di->register_done)) {
		// YR: this might happen if mem_priv ain't set yet

		/* [Jared]: Clarification. This could happen if the following conditions occur:
		 * 1) When add_disk is called, nvmeibs_disk_scan_finished() returns false
		 * 2) ib_register_client has initialised at least one NIC, (add_one) which has allowed the client to connect
		 * 3) ib_register_client has not finished initialising all NICs
		 *
		 * In such a case, not all resources may be available to send to the client. Seeing we don't as yet have a mechanism to
		 * add unregistered resources to an already "discovered" client, then we want to wait until the ib_register_client
		 * has finished initialising all NICs.
		 *
		 * Further note, this is a bad flow because the client is expecting a response here (wait_pending_iu) and
		 * will have to timeout before continuing. Needs to be fixed.
		 */
		_NT(trace_2_client_send_disk_info, "Skipping disk @DISK_ID_STR, as it is still in setup", cdisk->di->disk_id);
		rv = -1;
		goto out;
	}
	enc_ctx.n_rsc_sets = client_disk_resource_sets_count(cdisk, &enc_ctx.curr_mem_link, &enc_ctx.end_mem_head);

	_NT(trace_3_client_send_disk_info, "per_disk_rsp->disk_rscs_n_sets=@DISK_RSCS_N_SETS", enc_ctx.n_rsc_sets);

	/* set n_disk_rsrc */
	cdisk->n_disk_rsrc = (u32)cdisk->di->n_qs;
	if (P2NV(cl->net->params.port)->dev_type == DT_siw) {
		/* SIW does not support RDDA, no there's no need to send the nvme qs info */
		cdisk->n_disk_rsrc = 0;
	}

	if ((rv = CALL_VEX_OP(encode,
		vex_ach_acs_map_disk_info_srv_ops, ONE_EXT,
			base, vex_ach_acs_map_disk_info_srv_base_encode,
			ext1, vex_ach_acs_map_disk_info_srv_ext1_encode,
		       vex_ops, payload, payload_end, &enc_ctx)) < 0)
		goto out;
	payload_len = rv;

	if ((rv = send_n_wait(cl, req->hdr.tag, vex_ops->vex_ext, send_ioctx, payload,
		payload_len)) < 0) {
		_NT(trace_4_client_send_disk_info, "Failed to send disk P part");
	}

	_NT(trace_5_client_send_disk_info, "Disk @DISK_ID_STR, nsid=@NSID, block_shift=@BLOCK_SHIFT_INT, n_triplets=@N_TRIPLETS, disk_n_qps=@DISK_N_QPS "
	   "n_lock_segments=dpd->n_memsegs=@N_MEMSEGS",
		cdisk->di->disk_id, cdisk->di->nsid,
		cdisk->di->block_shift, enc_ctx.tot_rionics, cdisk->n_disk_rsrc, enc_ctx.dpd->n_memsegs);

out:
	__NFOUT;
	return rv;
}

static int send_triplets(struct nvmeibs_client *cl, struct nvmeibs_client_disk *cdisk,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	struct volume_server_rsp *rsp = &cl->rsp;
	struct volume_server_config_access_map_per_disk_n_rsp *per_disk_n_rsp;
	struct nvmeibs_lionic *lionic;
	struct nvmeibs_rionic *rionic;
	int i, j = 0, k, rv = 0;
	char rgid_buf[GUID_SIZE] = {0};
	char lgid_buf[GUID_SIZE] = {0};

	__NFIN;
	per_disk_n_rsp = &rsp->per_disk_n_rsp;
	k = 0;
	for (i = 0; i < cdisk->n_lionics; ++i) {
		lionic = &cdisk->lionics[i];
		_NT(send_triplets_t1, "lionic->n_rionics=@INT", lionic->n_rionics);
		for (j = 0; j < lionic->n_rionics; ++j) {
			rionic = &lionic->rionics[j];
			memcpy(per_disk_n_rsp->a[k].lnic, lionic->gid.raw, 16);
			memcpy(per_disk_n_rsp->a[k].rnic, rionic->gid.raw, 16);
			format_gid_raw(per_disk_n_rsp->a[k].lnic, lgid_buf);
			format_gid_raw(per_disk_n_rsp->a[k].rnic, rgid_buf);
			_ND(send_triplets_d1, "Triplet: s=@STR, d=@STR", lgid_buf, rgid_buf);
			per_disk_n_rsp->a[k].n_qps = cpu_to_be32(0); /* RDDA removed */
			if (++k == VOLUME_SERVER_MAX_ARRAY_SIZE) {
				/* message buffer is full so send it */
				_ND(send_triplets_d2, "Disk @STR: mid - lionic=@INT, rionic=@INT, k=@INT",
					cdisk->di->disk_id, i, j, k);
				if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx,
					per_disk_n_rsp, sizeof(*per_disk_n_rsp))) < 0) {
					_NT(send_triplets_t2, "Failed to send disk N part");
					goto out;
				}
				k = 0;
			}
		}
	}
	_NT(trace_client_send_triplets, "k=@INT", k);
	if (k) {
		_ND(trace_1_client_send_triplets, "Disk @DISK_ID_STR: last - lionic=@LIONIC_INT, rionic=@RIONIC_INT, k=@INT",
			cdisk->di->disk_id, i, j, k);
		if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, per_disk_n_rsp,
			sizeof(*per_disk_n_rsp))) < 0) {
			_NT(trace_2_client_send_triplets, "Failed to send disk N part");
			goto out;
		}
	}

out:
	__NFOUT;
	return rv;
}

struct get_lock_gids_rsp_ctx {
	union ib_gid gid;
	bool max_tgt_atomic_ops;
	int max_rd_atom_on_wire;
	enum rdma_link_layer link_layer;
	enum rdma_transport_type transport_type;
	unsigned int priority;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_lock_gids_rsp_clnt_base_encode)
{
	struct wire_lock_gid_base *lock_gid = wire_buf;
	struct get_lock_gids_rsp_ctx *ctx = arg;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*lock_gid) > wire_buf_end);

	lock_gid->gid.global.subnet_prefix = ctx->gid.global.subnet_prefix;
	lock_gid->gid.global.interface_id = ctx->gid.global.interface_id;
	lock_gid->atomic_ops = (ctx->max_tgt_atomic_ops && ctx->max_rd_atom_on_wire > 0 ?
							cpu_to_be32(ctx->max_rd_atom_on_wire) : 0);

	_NT(vex_ach_get_lock_gids_rsp_clnt_base_encode_t1, "lock_n [@INT] gid @GID atomic_ops @ATOMIC_CAP",
		elem_idx, &lock_gid->gid, be32_to_cpu(lock_gid->atomic_ops));

	rv = sizeof(*lock_gid);

	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_lock_gids_rsp_clnt_ext1_encode)
{
	struct wire_lock_gid_ext1 *ext1 = wire_buf;
	struct get_lock_gids_rsp_ctx *ctx = arg;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	ext1->link_layer = ctx->link_layer;
	ext1->transport_type = ctx->transport_type;
	ext1->priority = cpu_to_be32(ctx->priority);

	_NT(vex_ach_get_lock_gids_rsp_clnt_ext1_encode_t1, "lock_n [@INT] gid @GID layer @LAYER transport @TRANSPORT_TYPE",
		elem_idx, &ctx->gid, ctx->link_layer, ctx->transport_type);

	rv = sizeof(*ext1);

	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_lock_gids_rsp_clnt_ext2_encode)
{
	struct wire_lock_gid_ext2 *ext2 = wire_buf;
	struct get_lock_gids_rsp_ctx *ctx = arg;
	ssize_t rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext2) > wire_buf_end);

	ext2->tcp_base_port = cpu_to_be16(nvmeib_get_tcp_base_port_id());
	ext2->tcp_num_ports = cpu_to_be16(nvmeib_get_tcp_num_ports());

	_NT(vex_ach_get_lock_gids_rsp_clnt_ext2_encode_t1, "lock_n [@INT] gid @GID TCP Ports [@START_PORT, @END_PORT]",
	    elem_idx, &ctx->gid, ext2->tcp_base_port, ext2->tcp_base_port + ext2->tcp_num_ports - 1);

	rv = sizeof(*ext2);

	return rv;
}

static int send_lock_devices(struct nvmeibs_client *cl,
	struct nvmeibs_disk_private_data *disk_private_data,
	struct nvmeibs_disk_info *di,
	struct volume_client_req *req, struct volume_client_config_rdma_info *rdma,
	struct nvmeib_iu *send_ioctx)
{
	ssize_t rv = 0;
	struct nvmeibs_dev *dev, *admin_dev;
	struct nvmeibs_ib_port *port;
	void *p = cl->out_msg_area;
	void *e = cl->out_msg_area_end;
	struct list_head *devs;
	bool found_admin = false;
	struct nvmeibs_dev *tdev;
	struct nvmeib_container *lc;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_get_lock_gids_rsp];
	DECLARE_COMPLETION_ONSTACK(send_done);

	__NFIN;

	/* Lock GIDs request is still at base and is not using VEX yet */
	if (be16_to_cpu(req->version_tag) != vex_base) {
		_NT(send_lock_devices_t1, "Invalid version tag @UINT for NVMEIBC_MA_GET_LOCK_GIDS", be16_to_cpu(req->version_tag));
		rv = -EPROTO;
		goto out;
	}

	memset(cl->out_msg_area, 0xcc, e - p);
	if ((rv = vex_encode_container_hdr(&lc, vex_ops, p, e)) < 0)
		goto out;
	devs = nvmeibs_get_devices(NULL);
	nvmeibs_disk_locks_guard();
	dev = disk_private_data->lock_dev;
	if (dev) {
		list_for_each_entry(port, &dev->port_list, port_list_n) {
			struct get_lock_gids_rsp_ctx ctx = {
				.gid = port->gid.gid,
				.max_tgt_atomic_ops = dev->atomic_ops,
				.max_rd_atom_on_wire = nvmeib_device_get_max_rd_atom_on_wire(dev->dev->dev_type),
				.link_layer = port->layer,
				.transport_type = port->transport,
				.priority = port->transport_priority,
			};
			if ((rv = CALL_VEX_OP(encode_container_elem,
					vex_ach_get_lock_gids_rsp_srv_ops, TWO_EXT,
						base, vex_ach_get_lock_gids_rsp_clnt_base_encode,
						ext1, vex_ach_get_lock_gids_rsp_clnt_ext1_encode,
						ext2, vex_ach_get_lock_gids_rsp_clnt_ext2_encode,
							vex_ops, lc, e, &ctx)) < 0)
				goto no_space;
		}

		list_for_each_entry(tdev, devs, nvmeibs_dev_list_n) {
			if (tdev != dev && nvmeib_same_physical_dev(tdev->dev, dev->dev)) {
				list_for_each_entry(port, &tdev->port_list, port_list_n) {
					struct get_lock_gids_rsp_ctx ctx = {
						.gid = port->gid.gid,
						.max_tgt_atomic_ops = dev->atomic_ops,
						.max_rd_atom_on_wire = nvmeib_device_get_max_rd_atom_on_wire(tdev->dev->dev_type),
						.link_layer = port->layer,
						.transport_type = port->transport,
						.priority = port->transport_priority,
					};
					if ((rv = CALL_VEX_OP(encode_container_elem,
							vex_ach_get_lock_gids_rsp_srv_ops, TWO_EXT,
								base, vex_ach_get_lock_gids_rsp_clnt_base_encode,
								ext1, vex_ach_get_lock_gids_rsp_clnt_ext1_encode,
								ext2, vex_ach_get_lock_gids_rsp_clnt_ext2_encode,
									vex_ops, lc, e, &ctx)) < 0)
						goto no_space;
				}
			}
		}
	}
	else {
		admin_dev = cl->ib_port->nis_dev;

		list_for_each_entry(dev, devs, nvmeibs_dev_list_n)  {
			if (dev == admin_dev) {
				found_admin = true;
				continue;
			}
			list_for_each_entry(port, &dev->port_list, port_list_n) {
				struct get_lock_gids_rsp_ctx ctx = {
					.gid = port->gid.gid,
					/* we & these to members and send - client uses this
					   value to sent conn_params.initiator_depth on-connect
					   TBD: merge these two... */
					.max_tgt_atomic_ops = nvmeibs_dev_do_atomics(dev) ? 1 : 0,
					.max_rd_atom_on_wire = nvmeib_device_get_max_rd_atom_on_wire(dev->dev->dev_type),
					.link_layer = port->layer,
					.transport_type = port->transport,
					.priority = port->transport_priority,
				};
				if ((rv = CALL_VEX_OP(encode_container_elem,
						vex_ach_get_lock_gids_rsp_srv_ops, TWO_EXT,
							base, vex_ach_get_lock_gids_rsp_clnt_base_encode,
							ext1, vex_ach_get_lock_gids_rsp_clnt_ext1_encode,
							ext2, vex_ach_get_lock_gids_rsp_clnt_ext2_encode,
								vex_ops, lc, e, &ctx)) < 0)
					goto no_space;
			}
		}
		//admin device ports are sent last
		BUG_ON(found_admin == false);
		list_for_each_entry(port, &admin_dev->port_list, port_list_n) {
			struct get_lock_gids_rsp_ctx ctx = {
				.gid = port->gid.gid,
				.max_tgt_atomic_ops = nvmeibs_dev_do_atomics(admin_dev) ? 1 : 0,
				.max_rd_atom_on_wire = nvmeib_device_get_max_rd_atom_on_wire(admin_dev->dev->dev_type),
				.link_layer = port->layer,
				.transport_type = port->transport,
				.priority = port->transport_priority,
			};
			if ((rv = CALL_VEX_OP(encode_container_elem,
					vex_ach_get_lock_gids_rsp_srv_ops, TWO_EXT,
						base, vex_ach_get_lock_gids_rsp_clnt_base_encode,
						ext1, vex_ach_get_lock_gids_rsp_clnt_ext1_encode,
						ext2, vex_ach_get_lock_gids_rsp_clnt_ext2_encode,
							vex_ops, lc, e, &ctx)) < 0)
				goto no_space;
		}
	}

	nvmeibs_disk_lock_unguard();
	nvmeibs_put_devices();

	_NT(send_lock_devices_t2, "Sending @INT lock gids in container (size @ZU)",
	   nvmeib_container_n_elem(lc), nvmeib_container_size(lc));

	rv = send_rdma_msg(cl, rdma, nvmeib_container_size(lc));
	if (!rv)
		send_ioctx->io_done = &send_done;
	else
		_NT(send_lock_devices_t4, "send_rdma_msg failed (@RV)", rv);
	// always attempt to return a reply (even if previous stage failed)
	nvmeibs_client_send_rsp(cl, cl->net,
		rv != -1 ? NVMEIBS_RSP_MGMT_OPCODE_OK : NVMEIBS_RSP_MGMT_OPCODE_ERR,
		req->hdr.tag, cpu_to_be16(vex_ops->vex_ext), send_ioctx, NULL, 0, NVMEIB_SEND_CFG, NON_NR_VERSION);
	if (send_ioctx->io_done) {
		if ((rv = wait_for_completion_timeout(&send_done, NVMEIB_WAIT_FOR_ADMIN_SEND_COMP)) <= 0) {
			_NT(trace_client_send_lock_devices_time_out, "Timed out (@RV) waiting for send comp from host @CL_NAME",
				rv, cl->name);
			rv = -ETIMEDOUT;
		} else {
			rv = 0; /* Wait for completion returns positive on success, not what we want*/
		}
	}
	send_ioctx->io_done = NULL;
#if 0
	if ((rv = send_n_wait(cl, req->hdr.tag, send_ioctx,
			cl->out_msg_area, 0)) < 0) {
		_NT(send_lock_devices_t3, "Failed to send device lock information");
		goto out;
	}
#endif
	goto out;

no_space:
	_NE(error_client_send_lock_devices, "No space in message area");
	nvmeibs_disk_lock_unguard();
	nvmeibs_put_devices();

out:
	__NFOUT;
	return rv;
}


bool nvmeibs_client_check_lock( struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port)
{
	bool rv = false;
	struct nvmeibs_client_disk *cdisk;
	struct list_head *disks = &cl->disks;
	NFIN;

	if (!cl->is_local) {
		//assuming single disk that belongs to the client
		list_for_each_entry(cdisk, disks, link) {
			_NT(nvmeibs_client_check_lock_t1, "Disk = @STR", cdisk->di->disk_id);
			BUG_ON(!list_is_last(&cdisk->link, disks));
			rv = nvmeibs_disk_locks_is_selected_device(cdisk->di, ib_port, cl);
			_NT(nvmeibs_client_check_lock_t2, "rv = @INT", rv);
		}
	}
	else {
		rv = nvmeibs_disk_locks_is_selected_device(cl->di, ib_port, cl);
	}

	NFOUT;
	return rv;
}

struct lock_mem_seg_info_ctx {
	struct nvmeibs_disk_lock_mem_info *lmi;
	struct nvmeibs_disk_lock_mem_info_for_device *lock_dev;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_lock_mem_seg_info_clnt_base_encode)
{
	struct volume_server_config_per_segment_lock_info_base *base = wire_buf;
	struct lock_mem_seg_info_ctx *ctx = arg;
	struct nvmeibs_disk_lock_mem_info *lmi = ctx->lmi;
	struct nvmeibs_disk_lock_mem_info_for_device *lock_dev = ctx->lock_dev;
	int rv;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);

	base->seg_id = cpu_to_be32(lmi->seg_id);
	if (lmi->len > (uint64_t)UINT_MAX) {
		if (link_ext < vex_ext2) {
			_NE(err_vex_ach_lock_mem_seg_info_clnt_base_encode_len,
			    "LOCKS: segment len @LENGTH_LONG is too large for protocol version", lmi->len);
			rv = -EPROTO;
			goto out;
		}
		base->len = 0;
	} else {
		base->len = cpu_to_be32(lmi->len);
	}
	base->lock_set_size = cpu_to_be64(lmi->lock_set_size);
	base->rkey = cpu_to_be32(lock_dev->rkey);
	base->lkey = cpu_to_be32(lock_dev->lkey);
	base->addr = cpu_to_be64((u64)(lock_dev->mem.ioaddr));
	base->start_addr = cpu_to_be64(lmi->start_addr);

	_ND(trace_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: sending single disk lock information");
	_ND(trace_2_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: seg_id @SEG_IDX",
	    be32_to_cpu(base->seg_id));
	_ND(trace_3_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: len = @LENGTH_INT", be32_to_cpu(base->len));
	_ND(trace_4_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: lock_set_size @LOCK_SET_SIZE",
	    be64_to_cpu(base->lock_set_size));
	_ND(trace_5_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: start addr @START_ADDR",
	    be64_to_cpu(base->start_addr));
	_ND(trace_6_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: addr = @IOADDR", lock_dev->mem.ioaddr);
	_ND(trace_7_vex_ach_lock_mem_seg_info_clnt_base_encode, "LOCKS: rkey @RKEY", lock_dev->rkey);

	rv = sizeof(*base);
out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_lock_mem_seg_info_clnt_ext1_encode)
{
	struct volume_server_config_per_segment_lock_info_ext1 *ext1 = wire_buf;
	struct lock_mem_seg_info_ctx *ctx = arg;
	struct nvmeibs_disk_lock_mem_info *lmi = ctx->lmi;

	BUG_ON(!ext1);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);

	ext1->lmi = cpu_to_be64((u64)lmi);

	_ND(trace_vex_ach_lock_mem_seg_info_clnt_ext1_encode, "LOCKS: lmi @LMI", lmi);

	return sizeof(*ext1);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_lock_mem_seg_info_clnt_ext2_encode)
{
	struct volume_server_config_per_segment_lock_info_ext2 *ext2 = wire_buf;
	struct lock_mem_seg_info_ctx *ctx = arg;
	struct nvmeibs_disk_lock_mem_info *lmi = ctx->lmi;

	BUG_ON(!ext2);
	BUG_ON(wire_buf + sizeof(*ext2) > wire_buf_end);

	ext2->len64 = cpu_to_be64(lmi->len);

	_ND(trace_vex_ach_lock_mem_seg_info_clnt_ext2_encode,
	    "LOCKS: len64 @LENGTH_LONG", lmi->len);

	return sizeof(*ext2);
}

static int send_disk_lock_mems(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	int rv = 0;
	struct nvmeibs_client_disk *cdisk;
	struct volume_client_req *req = recv_ioctx->buf;
	struct nvmeibs_disk_private_data *disk_private_data;
	struct volume_server_config_per_segment_lock_info *per_segment_lock_info;
	struct nvmeibs_disk_lock_mem_info *lmi;
	struct nvmeibs_disk_lock_mem_info_for_device *selected_dev = NULL;
	struct nvmeibs_disk_lock_mem_info_for_device *main_dev = NULL;
	struct completion *send_done = NULL;
	struct lock_mem_seg_info_ctx seg_info_ctx = {};

	__NFIN;
	BUG_ON(cl->n_disks > 1);
	BUG_ON(cl->n_disks <= 0);
	cdisk = list_first_entry_or_null(&cl->disks, struct nvmeibs_client_disk, link);
	BUG_ON(cdisk == NULL);

	if (is_cl_lock_net_dying(cl)) {
		_NE(error_client_send_disk_lock_mems, "cl @CL_NAME, no lock-net, not holding lock-dev refcnt, bail", cl->name);
		rv = -EINVAL;
		goto out;
	}

	disk_private_data = (struct nvmeibs_disk_private_data *)cdisk->di->priv;
	if (!disk_private_data) {
		rv = -EINVAL;
		_NE(error_1_client_send_disk_lock_mems, "got null private data");
		goto out;
	}
	_NT(trace_client_send_disk_lock_mems, "--- Start handling NVMEIBC_MA_GET_DISK_MEMS message from host @CL_NAME "
	   "disk @DISK_ID_STR", cl->name, cdisk->di->disk_id);
	per_segment_lock_info = &(cl->rsp.per_segment_lock_info);
	send_done = kzalloc(sizeof(struct completion), GFP_ATOMIC);
	if (!send_done) {
		_NE(error_2_client_send_disk_lock_mems, "memory allocation problem when trying to allcate completion");
		rv = -ENOMEM;
		goto out;
	}
	init_completion(send_done);
	BUG_ON(send_ioctx->io_done);
	send_ioctx->io_done = send_done;
	_ND(trace_1_client_send_disk_lock_mems, "LOCKS: iteration over disk locks. number of memsegs: @N_MEMSEGS",
		disk_private_data->n_memsegs);
	if (disk_private_data->n_memsegs > 0) {
		list_for_each_entry(lmi, &disk_private_data->lock_mems, link) {
			selected_dev = lmi->selected;
			main_dev = nvmeibs_disk_locks_lmi_get_device(
				lmi, cl->lock_net->params.port->gid.gid.raw, cl->lock_net->params.port->layer, cl->lock_net->params.port->transport);
			if (selected_dev == main_dev)
				_ND(trace_2_client_send_disk_lock_mems, "Lock device is the main locking device");
			else
				_ND(trace_3_client_send_disk_lock_mems, "Lock device is NOT the main locking device");
			if (main_dev == NULL) {
				_NW_dmesg(warn_client_send_disk_lock_mems, "Lock device is NULL");
				rv = -1;
				goto out;
			}

			seg_info_ctx.lmi = lmi;
			seg_info_ctx.lock_dev = main_dev;

			if ((rv = CALL_VEX_OP(encode,
				vex_ach_lock_mem_seg_info_srv_ops, TWO_EXT,
					base, vex_ach_lock_mem_seg_info_clnt_base_encode,
					ext1, vex_ach_lock_mem_seg_info_clnt_ext1_encode,
					ext2, vex_ach_lock_mem_seg_info_clnt_ext2_encode,
						cl->vex_ach_ops[vex_ach_lock_mem_seg_info],
						cl->rsp.payload, cl->rsp.ach_payload_max,
						&seg_info_ctx)) < 0)
			{
				_NE(error_4_client_send_disk_lock_mems, "Failed (@RV) encoding lock mem seg info", rv);
				rv = -1;
				goto out;
			}

			if ((rv = send_n_wait(cl, req->hdr.tag, cl->vex_ach_ops[vex_ach_lock_mem_seg_info]->vex_ext, send_ioctx,
				per_segment_lock_info, sizeof(*per_segment_lock_info))) < 0) {
				_NT(trace_4_client_send_disk_lock_mems, "Failed to send segment lock info");
				goto out;
			}
		}
		BUG_ON(!nvmeibs_use_tcp_locks && !selected_dev);
		BUG_ON(nvmeibs_use_tcp_locks && selected_dev);
		BUG_ON(main_dev == NULL);
		//struct volume_server_config_active_lock_table_info { __be32 rkey; __be64 addr; __be32 allocated; __be32 used; } x = {0,0,0,0}; // NVMESH-4574 24[b], active-locks-deprecation
		if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, NULL /* &x */, 0 /* sizeof(x) */)) < 0) {
			_NT(trace_13_client_send_disk_lock_mems, "Failed to send deprecated active table connection information");
			goto out;
		}
	}

	kfree(send_done);
	send_ioctx->io_done = NULL;
	_NT(trace_18_client_send_disk_lock_mems, "--- Finish handling NVMEIBC_MA_GET_DISK_MEMS message from host @CL_NAME",
	 	cl->name);

out:
	__NFOUT;
	return rv;
}

static int send_disk_lock_info(struct nvmeibs_client *cl,
	struct nvmeibs_client_disk *cdisk, struct nvmeib_iu *recv_ioctx,
	struct nvmeib_iu *send_ioctx, struct vex_get_acs_ctx *get_acs_ctx)
{
	int rv = 0;
	struct volume_client_req *req = recv_ioctx->buf;
	struct nvmeibs_disk_private_data *disk_private_data;
	struct volume_server_config_per_disk_locks_rsp *per_disk_lock_rsp;

	__NFIN;
	disk_private_data = (struct nvmeibs_disk_private_data *)cdisk->di->priv;
	if (!disk_private_data) {
		rv = -EINVAL;
		_NE(error_client_send_disk_lock_info, "got null private data");
		goto out;
	}
	per_disk_lock_rsp = &(cl->rsp.per_disk_lock_rsp);
	_ND(trace_client_send_disk_lock_info, "sending disk lock information of disk @DISK_ID_STR", cdisk->di->disk_id);
	strncpy(per_disk_lock_rsp->disk_name, cdisk->di->disk_id,
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);

	per_disk_lock_rsp->num_of_memsegs =
		cpu_to_be32(disk_private_data->n_memsegs);

	if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, per_disk_lock_rsp,
			sizeof(*per_disk_lock_rsp))) < 0) {
			_NT(trace_1_client_send_disk_lock_info, "Failed to send disk num of segments");
			goto out;
		}
	//send the list of lock devices
	send_ioctx->io_done = NULL;	// ensure complete() will not be called by
					// process_send_completion()
	if ((rv = send_lock_devices(cl, disk_private_data, cdisk->di, req,
		&get_acs_ctx->rdma, send_ioctx))) {
		_NT(trace_2_client_send_disk_lock_info, "Failed to send disk lock devices");
	}

out:
	__NFOUT;
	return rv;
}

static int send_disk_hca_rscs(struct nvmeibs_client *cl,
	struct nvmeibs_client_disk *cdisk, struct nvmeib_iu *recv_ioctx,
	struct nvmeib_iu *send_ioctx, struct disk_mapped_mem_info *mem)
{
	struct volume_client_req *req = recv_ioctx->buf;
	struct volume_server_rsp *rsp = &cl->rsp;
	struct volume_server_config_access_map_per_disk_d_rsp *per_disk_d_rsp;
	struct nvmeibs_disk_description *d;
	struct nvmeibs_q_info *qs;
	struct nvmeib_alloc_n_map *bb_map;
	int i, j, ii;
	char gid[GUID_SIZE] = {0};
	int rv = 0;
	struct nvmeibs_disk_private_data *dpd;
	struct nvmeibs_disk_seg_lock_info *e;
	struct nvmeibs_disk_lock_mem_info *lmi;
	struct nvmeibs_disk_lock_mem_info_for_device *lock_dev;
	bool found_dev;

	__NFIN;
	per_disk_d_rsp = &rsp->per_disk_d_rsp;
	/* copy the ports only with the first entry */
	memcpy(per_disk_d_rsp->ports, mem->ports,
		sizeof(per_disk_d_rsp->ports));
	_NT(trace_client_send_disk_hca_rscs, "Sending mem_>n_ports=@N_PORTS", mem->n_ports);
	per_disk_d_rsp->n_ports = cpu_to_be32(mem->n_ports);
	for (ii = 0; ii < mem->n_ports; ++ii) {
		format_gid_raw(mem->ports[ii].raw, gid);
		_ND(trace_1_client_send_disk_hca_rscs, "port @II @GID", ii, gid);
	}
	if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, per_disk_d_rsp,
		sizeof(*per_disk_d_rsp))) < 0) {
		_NT(trace_2_client_send_disk_hca_rscs, "Failed to send disk P part");
		goto out;
	}
	for (i = 0; i < cdisk->n_disk_rsrc; ) {
		for (j = 0; j < VOLUME_SERVER_MAX_ARRAY_SIZE && i < cdisk->n_disk_rsrc;
			 ++i, ++j) {
			struct nvme_dma_info *dma_info = &mem->ib_dma[i];

			qs = &cdisk->di->qs[i];
			bb_map = &mem->bb_maps[i];
			d = &per_disk_d_rsp->a[j];
			d->id = cpu_to_be64((u64)qs->qid);
			d->bounce_buffer_raddr = cpu_to_be64(bb_map->ioaddr);
			d->bounce_buffer_size = cpu_to_be32(qs->bb_npages * PAGE_SIZE);
			d->bounce_buffer_lkey = cpu_to_be32(bb_map->lkey);
			d->bounce_buffer_rkey = cpu_to_be32(bb_map->rkey);
			d->bounce_buffer_n_pages = cpu_to_be32(qs->bb_npages);
			if (qs->bb_mtdt_nvme_len && dma_info) {
				d->md_raddr = cpu_to_be64(dma_info->md);
				d->md_size = cpu_to_be32(qs->bb_mtdt_nvme_len);
				d->md_lkey = cpu_to_be32(nvmeib_get_lkey(N2NV(mem->dev)));
				d->md_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			}
			d->prp1_raddr = cpu_to_be64(qs->prpl_phys);
			d->prp1_size = 0;
			d->prp1_rkey = 0;
			d->prpl_raddr = cpu_to_be64(dma_info ? dma_info->prpl : qs->prpl_phys);
			d->prpl_size = cpu_to_be32(PAGE_SIZE);
			d->prpl_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			d->prpl_n_pages = cpu_to_be32((int)1);
			d->sq_raddr = cpu_to_be64(dma_info ? dma_info->sq : qs->sq_phys);
			d->sq_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			d->sq_entries = cpu_to_be32(qs->sq_len);
			d->cq_raddr = cpu_to_be64(dma_info ? dma_info->cq : qs->cq_phys);
			d->cq_lkey = cpu_to_be32(nvmeib_get_lkey(N2NV(mem->dev)));
			d->cq_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			d->cq_entries = cpu_to_be32(qs->cq_len);
			d->sq_db_raddr = cpu_to_be64(dma_info ? dma_info->sq_db : qs->sq_db_phys);
			d->sq_db_size = cpu_to_be32(4);
			d->sq_db_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			d->cq_db_raddr = cpu_to_be64(dma_info ? dma_info->cq_db : qs->cq_db_phys);
			d->cq_db_size = cpu_to_be32(4);
			d->cq_db_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			d->msix_raddr = cpu_to_be64(mem->nvme_msix_addr_iommu + qs->qid*PCI_MSIX_ENTRY_SIZE);
			d->msix_rkey = cpu_to_be32(nvmeib_get_rkey(N2NV(mem->dev)));
			d->bb_raddr_nvme[0] = cpu_to_be64(qs->bb_addr_nvme[0]);
			d->bb_raddr_nvme[1] = cpu_to_be64(qs->bb_addr_nvme[1]);
			d->bb_raddr_nvme[2] = cpu_to_be64(qs->bb_mtdt_nvme);
			_ND(send_disk_hca_rscs_d1, "resource # (@INT, @INT)", j, qs->qid);
			_ND(send_disk_hca_rscs_d2, "bb_raddr= @_X", bb_map->ioaddr);
			_ND(send_disk_hca_rscs_d3, "bb_size=@LONG", qs->bb_npages * PAGE_SIZE);
			_ND(send_disk_hca_rscs_d4, "bb_lkey=@INT32_HEX", bb_map->lkey);
			_ND(send_disk_hca_rscs_d5, "bb_rkey=@INT32_HEX", bb_map->rkey);
			_ND(send_disk_hca_rscs_d6, "bb_n_pages=@INT", qs->bb_npages);
			_ND(send_disk_hca_rscs_d7, "md_raddr= @_X", dma_info->md);
			_ND(send_disk_hca_rscs_d8, "md_size=@INT", qs->bb_mtdt_nvme_len);
			_ND(send_disk_hca_rscs_d9, "md_lkey=@INT32_HEX", nvmeib_get_lkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d10, "md_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d11, "prp1_raddr=0");
			_ND(send_disk_hca_rscs_d12, "prp1_size=0");
			_ND(send_disk_hca_rscs_d13, "prp1_rkey=0");
			_ND(send_disk_hca_rscs_d14, "prpl_raddr=@_X", qs->prpl_phys);
			_ND(send_disk_hca_rscs_d15, "prpl_size=@LONG", PAGE_SIZE);
			_ND(send_disk_hca_rscs_d16, "prpl_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d17, "prpl_n_pages=@INT", 1);
			_ND(send_disk_hca_rscs_d18, "sq_raddr=@_X", qs->sq_phys);
			_ND(send_disk_hca_rscs_d19, "sq_size=@LONG", PAGE_SIZE);
			_ND(send_disk_hca_rscs_d20, "sq_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d21, "sq_entries=@INT", qs->sq_len);
			_ND(send_disk_hca_rscs_d22, "cq_raddr=@_X", qs->cq_phys);
			_ND(send_disk_hca_rscs_d23, "cq_size=@LONG", PAGE_SIZE);
			_ND(send_disk_hca_rscs_d24, "cq_lkey=@INT32_HEX", nvmeib_get_lkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d25, "cq_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d26, "cq_entries=@INT", qs->cq_len);
			_ND(send_disk_hca_rscs_d27, "sq_db_raddr=@_X", qs->sq_db_phys);
			_ND(send_disk_hca_rscs_d28, "sq_db_size=@INT", 4);
			_ND(send_disk_hca_rscs_d29, "sq_db_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d30, "cq_db_raddr=@_X", qs->cq_db_phys);
			_ND(send_disk_hca_rscs_d31, "cq_db_size=@INT", 4);
			_ND(send_disk_hca_rscs_d32, "cq_db_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d33, "msix_raddr=@_X", be64_to_cpu(d->msix_raddr));
			_ND(send_disk_hca_rscs_d34, "msix_rkey=@INT32_HEX", nvmeib_get_rkey(N2NV(mem->dev)));
			_ND(send_disk_hca_rscs_d35, "bb_raddr_nvme[0..2]=(@_X, @_X, @_X), ",
				qs->bb_addr_nvme[0], qs->bb_addr_nvme[1], qs->bb_addr_nvme[1]);
		}
		if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, per_disk_d_rsp,
			sizeof(*per_disk_d_rsp))) < 0) {
			_NT(send_disk_hca_rscs_t36, "Failed to send disk D part");
			goto out;
		}
	}

	/* send the per disk segment lock info */
	dpd = (struct nvmeibs_disk_private_data *)cdisk->di->priv;
	lmi = list_first_entry_or_null(&dpd->lock_mems,
		struct nvmeibs_disk_lock_mem_info, link);
	for (i = 0; i < dpd->n_memsegs; ) {
		for (j = 0; j < VOLUME_SERVER_MAX_ARRAY_SIZE && i < dpd->n_memsegs;
			  ++i, ++j) {
			e = &per_disk_d_rsp->b[j];
			found_dev = false;
			list_for_each_entry(lock_dev, &lmi->per_device_locks, entry)
				if (lock_dev->nic_dev == mem->dev) {
					found_dev = true;
					break;
				}
			if (!found_dev) {
				_NE(error_client_send_disk_hca_rscs, "Fail to find lock rscs: disk @DISK_ID_STR, dev @IB_DEV_NAME (i=@IDX, j=@IDX)",
					cdisk->di->disk_id, mem->dev->dev->ib_dev->name, i, j);
//				_NE(send_disk_hca_rscs_e1, "Fail to find lock rscs: disk @STR, dev @STR (i=@INT, j=@INT)",
//					cdisk->di->disk_id, mem->dev->dev->ib_dev->name, i, j);
				nvmeibs_client_send_rsp(cl, cl->net, NVMEIBS_RSP_MGMT_OPCODE_ERR,
										req->hdr.tag, req->version_tag, send_ioctx, NULL, 0, NVMEIB_SEND_CFG, NON_NR_VERSION);
				rv = -1;
				goto out;
			}
			e->seg_id = cpu_to_be64((u64)(lmi->seg_id));
			e->start_disk_address = cpu_to_be64(lmi->start_addr);
			e->len = cpu_to_be64((u64)(lmi->len));
			e->ioaddr = cpu_to_be64(lock_dev->mem.ioaddr);
			e->lockset_size = cpu_to_be32((u32)(lmi->lock_set_size));
			e->lkey = cpu_to_be32(lock_dev->mem.lkey);
			e->rkey = cpu_to_be32(lock_dev->mem.rkey);
			e->lmi = (u64)lmi;
			_ND(trace_3_client_send_disk_hca_rscs, "seg_id=@SEG_ID_INT", lmi->seg_id);
			_ND(trace_4_client_send_disk_hca_rscs, "start_disk_address=@START_DISK_ADDRESS", lmi->start_addr);
			_ND(trace_5_client_send_disk_hca_rscs, "len=@LEN", lmi->len);
			_ND(trace_6_client_send_disk_hca_rscs, "ioaddr=@IOADDR", lock_dev->mem.ioaddr);
			_ND(trace_7_client_send_disk_hca_rscs, "lockset_size=@LOCKSET_SIZE_LLONG", lmi->lock_set_size);
			_ND(trace_8_client_send_disk_hca_rscs, "lkey=@LKEY", lock_dev->mem.lkey);
			_ND(trace_9_client_send_disk_hca_rscs, "rkey=@RKEY", lock_dev->mem.rkey);
			_ND(trace_10_client_send_disk_hca_rscs, "lmi=@LMI", (void*)(lmi));
			lmi = list_next_entry(lmi, link);
		}
		if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, per_disk_d_rsp,
			sizeof(*per_disk_d_rsp))) < 0) {
			_NT(trace_11_client_send_disk_hca_rscs, "Failed to send disk L part");
			goto out;
		}
	}

out:
	__NFOUT;
	return rv;
}

static int send_disk_rscs(struct nvmeibs_client *cl, struct nvmeibs_client_disk *cdisk,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	struct list_head *mems = &cdisk->di->mem_priv_list;
	struct disk_mapped_mem_info *mem;
	int rv = 0;

	NFIN;
	list_for_each_entry(mem, mems, link) {
		_NT(trace_client_send_disk_rscs, "Sending resources of HCA @IB_DEV_NAME for disk @DISK_ID_STR",
			mem->dev->dev->ib_dev->name, cdisk->di->disk_id);
		if ((rv = send_disk_hca_rscs(
			cl, cdisk, recv_ioctx, send_ioctx, mem)) < 0) {
			_NT(trace_1_client_send_disk_rscs, "Failed to send disk @DISK_ID_STR resources for device @IB_DEV_NAME",
				cdisk->di->disk_id, mem->dev->dev->ib_dev->name);
			break;
		}
		_NT(trace_2_client_send_disk_rscs, "Done for HCA @IB_DEV_NAME for disk @DISK_ID_STR",
			mem->dev->dev->ib_dev->name, cdisk->di->disk_id);
	}
	NFOUT;
	return rv;
}


static int build_access_map_disk(struct nvmeibs_client *cl,
	struct nvmeibs_client_disk *cdisk, struct nvmeib_iu *recv_ioctx,
	struct nvmeib_iu *send_ioctx, struct vex_get_acs_ctx *get_acs_ctx)
{
	int rv;

	__NFIN;
	if ((rv = send_disk_info(cl, cdisk, recv_ioctx, send_ioctx, get_acs_ctx)) < 0 ||
		(rv = send_triplets(cl, cdisk, recv_ioctx, send_ioctx)) < 0 ||
		(rv = send_disk_rscs(cl, cdisk, recv_ioctx, send_ioctx)) < 0  ||
		(rv = send_disk_lock_info(cl, cdisk, recv_ioctx, send_ioctx, get_acs_ctx)) < 0)
		_NT(trace_client_build_access_map_disk, "Failed to send client's access map");

	__NFOUT;
	return rv;
}

static int send_controller_info(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	struct volume_server_rsp *rsp = &cl->rsp;
	struct volume_server_config_access_map_gen_rsp *access_g_rsp;
	int rv = 0;

	__NFIN;
	access_g_rsp = &rsp->access_g_rsp;
	access_g_rsp->page_size = cpu_to_be32(PAGE_SIZE);
	access_g_rsp->n_disks = cpu_to_be32(cl->n_disks);

	if ((rv = send_n_wait(cl, req->hdr.tag, req->version_tag, send_ioctx, rsp->payload,
		sizeof(*access_g_rsp))) < 0) {
		_NT(trace_client_send_controller_info, "Failed to send disk P part");
	}
	__NFOUT;
	return rv;
}

static int build_access_map_reply(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx, struct vex_get_acs_ctx *get_acs_ctx)
{
	struct list_head *disks = &cl->disks;
	struct nvmeibs_client_disk *cdisk;
	struct completion *send_done;
	int rv;

	__NFIN;
	send_done = kzalloc(sizeof(struct completion), GFP_KERNEL);
	if (!send_done) {
		_NE(error_client_build_access_map_reply, "Memory allocation problem when trying to allocate completion");
		rv = -ENOMEM;
		goto out;
	}
	init_completion(send_done);
	BUG_ON(send_ioctx->io_done);
	send_ioctx->io_done = send_done;
	/* the general part of the response */
	if ((rv = send_controller_info(cl, recv_ioctx, send_ioctx)) < 0) {
		_NT(trace_client_build_access_map_reply, "Failed to send disk G part");
		goto out;
	}
	list_for_each_entry(cdisk, disks, link) {
		if ((rv = build_access_map_disk(cl, cdisk, recv_ioctx,
			send_ioctx, get_acs_ctx)) < 0) {
			_NT(build_access_map_reply_t1, "Out with error @INT", rv);
			goto out;
		}
	}
	kfree(send_done);
	send_ioctx->io_done = NULL;
out:
	__NFOUT;
	return rv;
}

static int build_access_map(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx, struct vex_get_acs_ctx *get_acs_ctx)
{
	int rv = 0;

	__NFIN;
	/* build disk list */
	if ((rv = build_disks_list(cl)) < 0)
		goto out;
	/* build the reply for the client */
	rv = build_access_map_reply(cl, recv_ioctx, send_ioctx, get_acs_ctx);

out:
	__NFOUT;
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_acs_srv_base_decode)
{
	struct vex_get_acs_ctx *get_acs_ctx = arg;
	const struct volume_client_config_ma_access_base *base = wire_buf;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) >= wire_buf_end);

	get_acs_ctx->rdma = base->rdma;

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_acs_srv_ext1_decode)
{
	struct vex_get_acs_ctx *get_acs_ctx = arg;
	struct nvmeibs_client *cl = get_acs_ctx->cl;
	const struct volume_client_config_ma_access_ext1 *ext1 = wire_buf;

	if (!wire_buf) {
		/* Extension not present, set defaults */
		get_acs_ctx->req_nrch_per_path = max_t(unsigned, cl->max_nrchs_per_path_tcp, cl->max_nrchs_per_path_rdma);
		return 0;
	}

	BUG_ON(wire_buf + sizeof(*ext1) >= wire_buf_end);

	get_acs_ctx->req_nrch_per_path = be32_to_cpu(ext1->req_nrch_per_path);

	return sizeof(*ext1);
}

/* nvmeibs_get_access_info(): Parse and configure client access map */
static void get_access_info(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	const void *p = cl->in_msg_area;
	const void *e = p + (NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
	struct volume_client_req *req = recv_ioctx->buf;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_get_acs];
	struct vex_get_acs_ctx get_acs_ctx = {
		.cl = cl,
	};
	int rv;

	__NFIN;
	_NT(trace_client_get_access_info, "--- Start handling NVMEIBC_MA_GET_ACCESS message from host @CL_NAME",
		cl->name);

	CALL_VEX_OP(decode,
			vex_ach_get_acs_srv_ops, ONE_EXT,
				base, vex_ach_get_acs_srv_base_decode,
				ext1, vex_ach_get_acs_srv_ext1_decode,
					vex_ops,
					NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_CONST_PAYLOAD(req),
					recv_ioctx->buf + recv_ioctx->size, &get_acs_ctx);

	_NT(trace_1_client_get_access_info, "TPE: p=@MSG_DATA,e=@MSG_DATA,ovfl=@OVFL", p, e, p >= e);
	nvmeib_mem_sync_map_for_cpu(&cl->in_msg_area_map, 0, NVMEIB_MEM_SYNC_ENTIRE_MAP_LEN);
	if ((rv = parse_maps(cl, req, p, e)) < 0 ||
		(rv = build_access_map(cl, recv_ioctx, send_ioctx, &get_acs_ctx)) < 0) {
		_NE(error_client_get_access_info, "Fail to parse and create client access map");
	}
	_NT(trace_2_client_get_access_info, "TPE: p=@MSG_DATA,e=@MSG_DATA,ovfl=@OVFL", p, e, p >= e);
	_NT(trace_3_client_get_access_info, "--- Finish handling NVMEIBC_MA_GET_ACCESS message from host @CL_NAME",
		cl->name);
	__NFOUT;
}

//test if io channel is alive

static void get_lock_gids(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	struct nvmeibs_disk_private_data *disk_pd = cl->di->priv;
	__NFIN;

	_NT(trace_client_get_lock_gids, "--- Start handling NVMEIBC_MA_GET_LOCK_GIDS message from host @CL_NAME",
		cl->name);
	if (send_lock_devices(cl, disk_pd, cl->di, req,
		&req->config_req.get_lock_gids.rdma, send_ioctx) < 0)
		_NE(error_client_get_lock_gids, "Failed to send disk lock devices");
	_NT(trace_1_client_get_lock_gids, "--- Finish handling NVMEIBC_MA_GET_LOCK_GIDS message from host @CL_NAME",
		cl->name);

	__NFOUT;
}

struct vex_ach_get_jrange_ctx {
	/* encoded request (wire format) */
	struct volume_client_config_get_jrange *creq_jrange;
	struct volume_client_config_rdma_info rdma;
	struct nvmeib_remote_access_info jmdc_rai;
	/* decoded request and encoded output/response */
	uuid_be client_uuid;
	binje_t binje_req;
	struct nvmeib_jrange_cache *jrc;
	struct volume_server_get_jrange_rsp *net_rsp;
	struct nvmeib_jrange_rsp *rsp;
	size_t jmdc_rsp_len;
	dma_addr_t jmdc_rsp_dma;
	size_t rsp_size;
};

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_req_srv_base_decode)
{
	struct vex_ach_get_jrange_ctx *ctx = arg;
	const struct volume_client_config_get_jrange_base *get_jrange = wire_buf;
	struct nvmeib_jrange_cache *jrc = ctx->jrc;

	BUG_ON(!wire_buf); /* Should not be NULL for base fn */
	BUG_ON((const void *)(get_jrange + 1) > wire_buf_end);
	ctx->client_uuid = get_jrange->client_uuid;
	ctx->rdma = get_jrange->rdma;
	jrc->rng_id = be32_to_cpu(get_jrange->prev_rng);

	return sizeof(*get_jrange);
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_req_srv_ext1_decode)
{
	struct vex_ach_get_jrange_ctx *ctx = arg;
	const struct volume_client_config_get_jrange_ext1 *get_jrange = wire_buf;
	struct nvmeib_jrange_cache *jrc = ctx->jrc;
	int i;
	ssize_t rv;

	if (!wire_buf) {
		/* Fill Defaults */
		jrc->gen_id = NVMEIB_EC_INVALID_JOURNAL_GEN_ID;
		memset(jrc->ent_md, NVMEIB_EC_INVALID_JOURNAL_ENT_GEN_ID, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
		strcpy(jrc->serjio_boot_id, UUID_ZERO_STRING);
		rv = 0;
		goto out;
	}
	BUG_ON((const void *)(get_jrange + 1) > wire_buf_end);
	jrc->rng_id = be32_to_cpu(get_jrange->clnt_jrc.rng_id);
	jrc->gen_id = be64_to_cpu(get_jrange->clnt_jrc.rng_genid);
	nvmeib_bitmap_from_be32(jrc->free_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE,
							get_jrange->clnt_jrc.free_bitmap);
	for (i = 0; i < NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE; i++)
		jrc->ent_md[i] = get_jrange->clnt_jrc.ent_md[i];
	memcpy(jrc->serjio_boot_id, get_jrange->clnt_jrc.serjio_boot_id, NVMEIB_GID_STR_MAX);
	rv = sizeof(*get_jrange);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_req_srv_ext2_decode)
{
	struct vex_ach_get_jrange_ctx *ctx = arg;
	const struct volume_client_config_get_jrange_ext2 *get_jrange = wire_buf;
	struct nvmeib_jrange_cache *jrc = ctx->jrc;
	ssize_t rv;

	if (!wire_buf) {
		/* fill defaults */
		ctx->binje_req = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		jrc->rng_binje = NVMEIB_EC_JOURNAL_DEFAULT_BLOCKS_PER_ENTRY;
		rv = 0;
		goto out;
	}

	BUG_ON((const void *)(get_jrange + 1) > wire_buf_end);
	ctx->binje_req = be32_to_cpu(get_jrange->binje_req);
	jrc->rng_binje = be32_to_cpu(get_jrange->binje_jrc);
	rv = sizeof(*get_jrange);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_get_jrange_req_srv_ext3_decode)
{
	struct vex_ach_get_jrange_ctx *ctx = arg;
	const struct volume_client_config_get_jrange_ext3 *get_jrange = wire_buf;
	ssize_t rv;

	if (!wire_buf) {
		/* fill defaults */
		memset(&ctx->jmdc_rai, 0, sizeof(ctx->jmdc_rai));
		rv = 0;
		goto out;
	}

	BUG_ON((const void *)(get_jrange + 1) > wire_buf_end);
	ctx->jmdc_rai.rkey = be32_to_cpu(get_jrange->jmdc_rai.rkey);
	ctx->jmdc_rai.raddr = be64_to_cpu(get_jrange->jmdc_rai.raddr);
	ctx->jmdc_rai.len = be32_to_cpu(get_jrange->jmdc_rai.len);

	rv = sizeof(*get_jrange);

out:
	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_rsp_srv_base_encode)
{
	struct volume_server_get_jrange_rsp_base *get_jrange = wire_buf;
	struct vex_ach_get_jrange_ctx *ctx = arg;
	struct nvmeib_jrange_rsp *rsp = ctx->rsp;

	BUG_ON(!wire_buf); /* Should not be NULL for base fn */
	BUG_ON(wire_buf + sizeof(*get_jrange) > wire_buf_end);

	get_jrange->rng_idx = cpu_to_be32(rsp->rng_idx);
	get_jrange->jrnl_lba = cpu_to_be64(rsp->rng_slba);
	get_jrange->rng_nlba = cpu_to_be32(rsp->rng_nlba);
	nvmeib_bitmap_to_be32_ext(get_jrange->non_free_ents_bmp,
							  rsp->free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, true);
	_NI(debug_s_client_2860, "free_ents_bmp: @BITMAP512 (@HEX64), non_free_ents_bmp (BE32): @BITMAP512 (@HEX64)\n",
		rsp->free_ents_bmp, rsp->free_ents_bmp, get_jrange->non_free_ents_bmp, get_jrange->non_free_ents_bmp);
	memcpy(get_jrange->jmdc_ents, rsp->jmdc, sizeof(get_jrange->jmdc_ents));

	return sizeof(*get_jrange);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_rsp_srv_ext1_encode)
{
	struct volume_server_get_jrange_rsp_ext1 *get_jrange = wire_buf;
	struct vex_ach_get_jrange_ctx *ctx = arg;
	struct nvmeib_jrange_rsp *rsp = ctx->rsp;
	int n_free_ents;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*get_jrange) > wire_buf_end);
	get_jrange->gen_id = cpu_to_be64(rsp->gen_id);
	get_jrange->n_ents = cpu_to_be32(rsp->n_ents);
	n_free_ents = bitmap_weight(rsp->free_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	get_jrange->n_free_ents = cpu_to_be32(n_free_ents);
	memcpy(get_jrange->ents_md, rsp->ent_md, sizeof(get_jrange->ents_md));
	memcpy(get_jrange->serjio_boot_id, rsp->serjio_boot_id, NVMEIB_GID_STR_MAX);

	return sizeof(*get_jrange);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_rsp_srv_ext2_encode)
{
	struct volume_server_get_jrange_rsp_ext2 *get_jrange = wire_buf;
	struct vex_ach_get_jrange_ctx *ctx = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*get_jrange) > wire_buf_end);

	get_jrange->binje_rsp = cpu_to_be32(ctx->rsp->rng_binje);

	return sizeof(*get_jrange);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jrange_rsp_srv_ext3_encode)
{
	struct volume_server_get_jrange_rsp_ext3 *get_jrange = wire_buf;
	struct vex_ach_get_jrange_ctx *ctx = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*get_jrange) > wire_buf_end);

	get_jrange->rng_nblk = cpu_to_be32(ctx->rsp->rng_nblk);
	get_jrange->max_rng_blk = cpu_to_be32(ctx->rsp->max_rng_blk);
	get_jrange->tot_n_rng = cpu_to_be32(ctx->rsp->tot_n_rng);

	return sizeof(*get_jrange);
}

struct vex_ach_get_jmdc_rng_hdr_srv_base_encode_ctx {
	struct nvmeib_get_jmdc_rng_data *rng_data;
	const unsigned long *dirty_ents_bmp, *abnd_ents_bmp;
};

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jmdc_rng_hdr_srv_base_encode)
{
	struct volume_server_get_jmdc_rng_data_base *res = wire_buf;
	struct nvmeib_get_jmdc_rng_data *rng_data = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*res) > wire_buf_end);

	res->client_uuid = rng_data->client_uuid;
	res->rng_idx = cpu_to_be16(rng_data->rng_idx);
	res->rng_start_lba = cpu_to_be64(rng_data->rng_start_lba);
	res->rng_size_lba = cpu_to_be32(rng_data->rng_size_lba);
	res->rng_gen_id = cpu_to_be64(rng_data->rng_gen_id);
	res->rng_ent_offset = cpu_to_be32(rng_data->rng_ent_offset);
	res->only_dirty_ents = rng_data->only_dirty_ents;

	nvmeib_bitmap_to_be32(
		res->dirty_ents_bmp, rng_data->dirty_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	nvmeib_bitmap_to_be32(
		res->abnd_ents_bmp, rng_data->abnd_ents_bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);

	return sizeof(*res);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jmdc_rng_hdr_srv_ext1_encode) {
	struct volume_server_get_jmdc_rng_data_ext1 *res = wire_buf;
	struct nvmeib_get_jmdc_rng_data *rng_data = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*res) > wire_buf_end);

	res->rng_ent_block_offset = cpu_to_be32(rng_data->rng_ent_block_offset);
	res->binje = binje_to_be(rng_data->binje);
	res->num_ents = cpu_to_be16(rng_data->num_ents);
	res->num_dirty_ents = cpu_to_be16(rng_data->num_dirty_ents);

	return sizeof(*res);
}

static int get_jrange_rdma_ext_jmdc(struct nvmeibs_client *cl, struct vex_ach_get_jrange_ctx *ctx)
{
	struct nvmeib_send_wr wr;
	struct ib_sge sge;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;

	NFIN;
	if (ctx->jmdc_rsp_len > ctx->jmdc_rai.len) {
		_NE(error_client_get_jrange_rdma_ext_jmdc_len, "NVMEIBC_MA_GET_JRANGE - jmdc remote data buffer is too small @REMOTE_LEN < @LENGTH_INT",
			ctx->jmdc_rai.len, ctx->jmdc_rsp_len);
		rv = -ENOMEM;
		goto out;
	}

	sge.addr = ctx->jmdc_rsp_dma;
	sge.length = ctx->jmdc_rsp_len;
	sge.lkey = nvmeib_get_lkey(P2NV(cl->ib_port));

	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr).num_sge = 1;
	nvmeib_send_wr_common(wr).sg_list = &sge;
	nvmeib_send_wr_common(wr).send_flags = 0;
	nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_RDMA_GET_JRANGE_EXT_JMDC, 0);
	nvmeib_send_wr_clear_next(wr);

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |=
			SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT | SIW_IB_SEND_MORE_WQES;
	}
#endif

	nvmeib_send_wr_rdma(wr).rkey = ctx->jmdc_rai.rkey;
	nvmeib_send_wr_rdma(wr).remote_addr = ctx->jmdc_rai.raddr;

	rv = nvmeibs_ib_post_send(cl->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);

	_NT(trace_client_get_jrange_rdma_ext_jmdc, "NVMEIBC_MA_GET_JRANGE - RDMA Write - @LENGTH_INT bytes from (@ADDR@@LKEY) -> (@REMOTE_ADDR_LLONG@@RKEY)",
		sge.length, sge.addr, sge.lkey, nvmeib_send_wr_rdma(wr).remote_addr,
		nvmeib_send_wr_rdma(wr).rkey);

out:
	NFOUT;
	return rv;
}

static void get_jrange(struct nvmeibs_client *cl, struct nvmeib_iu *recv_ioctx,
					   struct nvmeib_iu *send_ioctx)
{
	int rv;
	struct volume_client_req *req = recv_ioctx->buf;
	const void *recv_buf_end = recv_ioctx->buf + recv_ioctx->size;
	struct vex_ach_get_jrange_ctx ctx = {};
	const struct vex_ops *req_vex_ops = cl->vex_ach_ops[vex_ach_get_jrange_req];
	const struct vex_ops *rsp_vex_ops = cl->vex_ach_ops[vex_ach_get_jrange_rsp];
	DECLARE_COMPLETION_ONSTACK(send_done);

	__NFIN;
	ctx.jmdc_rsp_len = NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * sizeof(*ctx.rsp->jmdc);
	if (!(ctx.jrc = kzalloc(sizeof(*ctx.jrc), GFP_KERNEL)) ||
		!(ctx.rsp = kzalloc(sizeof(*ctx.rsp), GFP_KERNEL)) ||
		!(ctx.rsp->jmdc = nvmeib_public_ib_dma_alloc_coherent(P2IB(cl->net->params.port),
							ctx.jmdc_rsp_len,
							&ctx.jmdc_rsp_dma,
							DMA_TO_DEVICE))) {
		_NE(get_jrange_e1, "OOM Error");
		rv = -ENOMEM;
		goto out;
	}

	if (be16_to_cpu(req->version_tag) != req_vex_ops->vex_ext) {
		_NT(get_jrange_t1, "Invalid version @UINT in NVMEIBC_MA_GET_JRANGE", be16_to_cpu(req->version_tag));
		rv = -EPROTO;
		goto out;
	}

	/* Build cache input from message */
	rv = CALL_VEX_OP(decode,
				vex_ach_get_jrange_req_srv_ops, THREE_EXT,
					base, vex_ach_get_jrange_req_srv_base_decode,
					ext1, vex_ach_get_jrange_req_srv_ext1_decode,
					ext2, vex_ach_get_jrange_req_srv_ext2_decode,
					ext3, vex_ach_get_jrange_req_srv_ext3_decode,
						req_vex_ops, NVMEIBC_VOLUME_CLIENT_CONFIG_REQ_CONST_PAYLOAD(req),
						recv_buf_end, &ctx);
	if (rv < 0)
		goto out;

	_NT(trace_client_get_jrange,
		"--- Start handling NVMEIBC_MA_GET_JRANGE message from host "
		"@CL_NAME (UUID @CLIENT_UUID), prev rng-id: @JRNL_RNG_IDX), "
		"binje-cache=@BINJE, binje-req=@BINJE",
		cl->name, &ctx.client_uuid, ctx.jrc->rng_id,
		ctx.binje_req, ctx.jrc->rng_binje);

	if ((rv = nvmeibs_serjio_alloc_journal_range(cl->di, cl->cid,
		ctx.client_uuid, cl->name, ctx.binje_req, ctx.jrc, ctx.rsp)) < 0) {
		if (rv == -EBUSY) {
			_NT(trace_1_client_get_jrange, "Client @CL_NAME (@CLIENT_UUID) - Journal range @JRNL_RNG_IDX is still releasing. Try again",
			   cl->name, &ctx.client_uuid, cl->jrnl_rng);
		} else
			_NE(error_client_get_jrange, "Error @RV allocating journal chunk", rv);
		cl->jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
		cl->jrnl_rng_binje = NVMEIB_EC_INVALID_JOURNAL_BINJE;
		cl->jrnl_rng_n_ent = 0;
	} else {
		cl->jrnl_rng = rv;
		cl->jrnl_rng_binje = ctx.rsp->rng_binje;
		cl->jrnl_rng_n_ent = ctx.rsp->n_ents;

		if ((rv = CALL_VEX_OP(encode,
						vex_ach_get_jrange_rsp_srv_ops, THREE_EXT,
							base, vex_ach_get_jrange_rsp_srv_base_encode,
							ext1, vex_ach_get_jrange_rsp_srv_ext1_encode,
							ext2, vex_ach_get_jrange_rsp_srv_ext2_encode,
							ext3, vex_ach_get_jrange_rsp_srv_ext3_encode,
								rsp_vex_ops, cl->out_msg_area, cl->out_msg_area_end, &ctx)) > 0) {
			if (!(rv = send_rdma_msg(cl, &ctx.rdma, rv)) &&
				(!ctx.jmdc_rai.len || !(rv = get_jrange_rdma_ext_jmdc(cl, &ctx))))
				send_ioctx->io_done = &send_done;
		} else {
			_NT(trace_client_get_jrange_enc_failed,
			    "Failed (@RV) encoding GET_JRANGE rsp", rv);
			rv = -1;
		}
	}
out:
	// always attempt to return a reply (even if previous stage failed)
	nvmeibs_client_send_rsp(cl, cl->net,
				rv == 0 ? NVMEIBS_RSP_MGMT_OPCODE_OK : NVMEIBS_RSP_MGMT_OPCODE_ERR,
				req->hdr.tag, cpu_to_be16(rsp_vex_ops->vex_ext), send_ioctx, NULL, 0, NVMEIB_SEND_CFG, NON_NR_VERSION);
	if (send_ioctx->io_done && (rv = wait_for_completion_timeout(&send_done, NVMEIB_WAIT_FOR_ADMIN_SEND_COMP)) <= 0) {
		_NT(trace_client_get_jrange_time_out, "Timed out (@RV) waiting for send comp from host @CL_NAME",
			rv, cl->name);
		rv = -ETIMEDOUT;
	}
	send_ioctx->io_done = NULL;
	_NT(trace_2_client_get_jrange, "--- Finish handling (@RV) NVMEIBC_MA_GET_JRANGE message from host @CL_NAME (UUID @CLIENT_UUID)",
	   rv, cl->name, &ctx.client_uuid);
	if (ctx.rsp && ctx.rsp->jmdc) {
		nvmeib_public_ib_dma_free_coherent(P2IB(cl->net->params.port),
						ctx.jmdc_rsp_len,
						ctx.rsp->jmdc,
						ctx.jmdc_rsp_dma);
	}
	kfree(ctx.rsp);
	kfree(ctx.jrc);
	__NFOUT;
}

struct write_get_jmdc_params
{
	struct nvmeibs_client *cl;
	struct nvmeib_alloc_n_map src_map;
	void *src;
	off_t src_rng_data_off;
	size_t src_rng_data_len;
	off_t src_jmdc_off;
	size_t src_jmdc_len;
	off_t src_ent_md_off;
	size_t src_ent_md_len;
	struct volume_client_jmdc_req *jmdc_req;
	unsigned last_rng_idx;
	int src_hdr_idx;
	int dst_hdr_idx;
	int src_ent_idx;
	int dst_ent_idx;
	int src_ent_block_idx;
	int dst_ent_block_idx;
	int dirty_rng_cnt;
	int rng_cnt;
	int ent_cnt;
	int ent_block_cnt;
	struct nvmeib_send_wr *ents_wrs;
	struct ib_seg *ents_sges;
	int n_ents_wrs;
	int n_ents_sges;
	size_t rng_data_serialized_sz;
	u64 jrnl_start_clsect;
	u64 jrnl_len_clsect;
	struct completion *rdma_comp;
};

#ifdef LOW_MEM
#define NUM_GET_JMDC_SRC_RNG	2
#else
#define NUM_GET_JMDC_SRC_RNG	4
#endif

/* We want enough pages to send at least NUM_SRC_RNG is one go */
#define NUM_GET_JMDC_RNG_DATA_SRC_PAGES(n_rng, sz_rng_data) \
	DIV_ROUND_UP((n_rng) * NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE * \
		sz_rng_data, PAGE_SIZE)
#define NUM_GET_JMDC_MD_SRC_PAGES(n_rng) \
	DIV_ROUND_UP((n_rng) * NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE * \
		sizeof(union jblock_md), PAGE_SIZE)
#define NUM_GET_JMDC_ENT_MD_SRC_PAGES(n_rng) \
	DIV_ROUND_UP((n_rng) * NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE * \
		sizeof(struct nvmeib_jrnl_ent_md), PAGE_SIZE)

/* Journal Ranges (struct volume_server_get_jmdc_rng_data) */
static int rdma_write_get_jmdc_rng_data(struct write_get_jmdc_params *params, bool signal)
{
	struct nvmeibs_client *cl = params->cl;
	struct nvmeib_send_wr wr;
	struct ib_sge sge;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;
	u32 write_len = params->src_hdr_idx * params->rng_data_serialized_sz;
	u32 remote_offset = params->dst_hdr_idx * params->rng_data_serialized_sz;
	u32 remote_len = be32_to_cpu(params->jmdc_req->rng_data_rdma.msg_size);
	u32 index = atomic_inc_return(&cl->write_get_jmdc_cnt);

	NFIN;
	if (remote_offset + write_len > remote_len) {
		_NE(error_client_rdma_write_get_jmdc_rng_data, "NVMEIB_GET_JMDC - range data buffer is too small @REMOTE_LEN < @REMOTE_LEN",
			remote_len, remote_offset + write_len);
		rv = -ENOMEM;
		goto out;
	}

	sge.addr = params->src_map.ioaddr + params->src_rng_data_off;
	sge.length = write_len;
	sge.lkey = params->src_map.lkey;

	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr).num_sge = 1;
	nvmeib_send_wr_common(wr).sg_list = &sge;
	nvmeib_send_wr_common(wr).send_flags = signal ? IB_SEND_SIGNALED : 0;
	nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_RDMA_GET_JMDC, index);
	nvmeib_send_wr_clear_next(wr);

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |=
			SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT |
			(!signal ? SIW_IB_SEND_MORE_WQES : 0);
	}
#endif

	nvmeib_send_wr_rdma(wr).rkey = be32_to_cpu(params->jmdc_req->rng_data_rdma.msg_rkey);
	nvmeib_send_wr_rdma(wr).remote_addr =
		be64_to_cpu(params->jmdc_req->rng_data_rdma.msg_raddr) + remote_offset;

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw)
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	rv = nvmeibs_ib_post_send(cl->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);

	_ND(trace_client_rdma_write_get_jmdc_rng_data, "NVMEIB_GET_JMDC - RDMA Write - @LENGTH_INT bytes from (@ADDR@@LKEY) -> (@REMOTE_ADDR_LLONG@@RKEY)",
	   sge.length, sge.addr, sge.lkey, nvmeib_send_wr_rdma(wr).remote_addr,
	   nvmeib_send_wr_rdma(wr).rkey);

out:
	NFOUT;
	return rv;
}

/* JMDC entries (union jblock_md) */
static int rdma_write_get_jmdc_ent(struct write_get_jmdc_params *params, bool signal)
{
	struct nvmeibs_client *cl = params->cl;
	struct nvmeib_send_wr wr;
	struct ib_sge sge;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;
	u32 write_len = params->src_ent_block_idx * sizeof(union jblock_md);
	u32 remote_offset = params->dst_ent_block_idx * sizeof(union jblock_md);
	u32 remote_len = be32_to_cpu(params->jmdc_req->jmdc_ents_rdma.msg_size);
	u32 index = atomic_inc_return(&cl->write_get_jmdc_cnt);

	NFIN;
	if (remote_offset + write_len > remote_len) {
		_NE(error_client_rdma_write_get_jmdc_ent, "NVMEIB_GET_JMDC - jmdc_ents data buffer is too small @REMOTE_LEN < @REMOTE_LEN",
			remote_len, remote_offset + write_len);
		rv = -ENOMEM;
		goto out;
	}

	sge.addr = params->src_map.ioaddr + params->src_jmdc_off;
	sge.length = write_len;
	sge.lkey = params->src_map.lkey;

	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr).num_sge = 1;
	nvmeib_send_wr_common(wr).sg_list = &sge;
	nvmeib_send_wr_common(wr).send_flags = signal ? IB_SEND_SIGNALED : 0;
	nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_RDMA_GET_JMDC, index);
	nvmeib_send_wr_clear_next(wr);

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |=
			SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT |
			(!signal ? SIW_IB_SEND_MORE_WQES : 0);
	}
#endif

	nvmeib_send_wr_rdma(wr).rkey = be32_to_cpu(params->jmdc_req->jmdc_ents_rdma.msg_rkey);
	nvmeib_send_wr_rdma(wr).remote_addr =
		be64_to_cpu(params->jmdc_req->jmdc_ents_rdma.msg_raddr) + remote_offset;

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw)
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	rv = nvmeibs_ib_post_send(cl->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);

	_ND(trace_client_rdma_write_get_jmdc_ent_md, "NVMEIB_GET_JMDC - RDMA Write - @LENGTH_INT bytes from (@ADDR@@LKEY) -> (@REMOTE_ADDR_LLONG@@RKEY)",
		sge.length, sge.addr, sge.lkey, nvmeib_send_wr_rdma(wr).remote_addr,
		nvmeib_send_wr_rdma(wr).rkey);

out:
	NFOUT;
	return rv;
}

/* JMDC entries' MD (struct nvmeib_jrnl_ent_md) */
static int rdma_write_get_jmdc_ent_md(struct write_get_jmdc_params *params, bool signal)
{
	struct nvmeibs_client *cl = params->cl;
	struct nvmeib_send_wr wr;
	struct ib_sge sge;
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	int rv;
	u32 write_len = params->src_ent_idx * sizeof(struct nvmeib_jrnl_ent_md);
	u32 remote_offset = params->dst_ent_idx * sizeof(struct nvmeib_jrnl_ent_md);
	u32 remote_len = be32_to_cpu(params->jmdc_req->ents_md_rdma.msg_size);
	u32 index = atomic_inc_return(&cl->write_get_jmdc_cnt);

	NFIN;
	if (remote_offset + write_len > remote_len) {
		_NE(error_client_rdma_write_get_jmdc_ent_md, "NVMEIB_GET_JMDC - end_md data buffer is too small @REMOTE_LEN < @REMOTE_LEN",
			remote_len, remote_offset + write_len);
		rv = -ENOMEM;
		goto out;
	}

	sge.addr = params->src_map.ioaddr + params->src_ent_md_off;
	sge.length = write_len;
	sge.lkey = params->src_map.lkey;

	nvmeib_send_wr_common(wr).opcode = IB_WR_RDMA_WRITE;
	nvmeib_send_wr_common(wr).num_sge = 1;
	nvmeib_send_wr_common(wr).sg_list = &sge;
	nvmeib_send_wr_common(wr).send_flags = signal ? IB_SEND_SIGNALED : 0;
	nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_RDMA_GET_JMDC, (u16)index);
	nvmeib_send_wr_clear_next(wr);

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw) {
		nvmeib_send_wr_common(wr).send_flags |=
		SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT |
		(!signal ? SIW_IB_SEND_MORE_WQES : 0);
	}
#endif

	nvmeib_send_wr_rdma(wr).rkey = be32_to_cpu(params->jmdc_req->ents_md_rdma.msg_rkey);
	nvmeib_send_wr_rdma(wr).remote_addr =
		be64_to_cpu(params->jmdc_req->ents_md_rdma.msg_raddr) + remote_offset;

#if ENABLE_SIW
	if (P2NV(cl->net->params.port)->dev_type == DT_siw)
		nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

	rv = nvmeibs_ib_post_send(cl->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr);

	_ND(trace_client_rdma_write_get_jmdc_ent, "NVMEIB_GET_JMDC - RDMA Write - @LENGTH_INT bytes from (@ADDR@@LKEY) -> (@REMOTE_ADDR_LLONG@@RKEY)",
	   sge.length, sge.addr, sge.lkey, nvmeib_send_wr_rdma(wr).remote_addr,
	   nvmeib_send_wr_rdma(wr).rkey);

out:
	NFOUT;
	return rv;
}

/* For this @range_idx, add the following to proper offsets in @cl->out_msg_area:
   - Journal Ranges (struct volume_server_get_jmdc_rng_data)
   - JMDC entries (union jblock_md)
   - JMDC entries' MD (struct nvmeib_jrnl_ent_md)

   Assume this current pieces can fit in @cl->out_msg_area but if next wont fit,
   RDMA write it to client.
*/
static DECLARE_SERJIO_RNG_CB_FN(write_get_jmdc)
{
	struct write_get_jmdc_params *params = ctx;
	struct nvmeib_get_jmdc_rng_data rng_data;
	struct volume_server_get_jmdc_rng_data *dest_rng_data = params->src + params->src_rng_data_off + params->src_hdr_idx * params->rng_data_serialized_sz;
	union jblock_md *dest_jmdc = params->src + params->src_jmdc_off + params->src_ent_block_idx * (sizeof(*dest_jmdc));
	struct nvmeib_jrnl_ent_md *dest_ent_md = params->src + params->src_ent_md_off + params->src_ent_idx * sizeof(*ent_md);
	int rv = 0, i;
	bool only_dirty_ents;
	int n_ents;

	_NT(t0_s_client_write_get_jmdc, "range range_idx: @JRNL_RNG_IDX N: @BINJE num_entries: @NUM_ENTS client_uuid: @CLIENT_UUID dirty_ents: "
	NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE_TRACE
	" start_lba: @START_CLSECT size_lba: @SIZE_CLSECT",
	   range_idx, range_binje, num_entries, &client_uuid, dirty_ents_in_seg_bmp, start_clsect, size_clsect);

	if (n_dirty_ents_in_seg > 0)
		params->dirty_rng_cnt++;
	else if (params->jmdc_req->get_dirty_only)
		goto out;
	else if (params->jmdc_req->get_client_uuid_only &&
		nvmeib_uuid_cmp(params->jmdc_req->client_uuid, client_uuid) != 0)
		goto out;

	params->rng_cnt++;
	params->last_rng_idx = range_idx;

	if (params->jmdc_req->get_dirty_only) {
		only_dirty_ents = true;
		n_ents = n_dirty_ents_in_seg;
	}
	else {
		only_dirty_ents = false;
		n_ents = num_entries;
	}

	if (params->jmdc_req->get_len_only) {
		_ND(trace_1_client_write_get_jmdc, "get_len_only");
		params->ent_cnt += n_ents;
		params->ent_block_cnt += num_entries * range_binje;
		rv = 0;
		goto out;
	}

	/* Fill in range data in header source buffer */
	rng_data.client_uuid = client_uuid;
	rng_data.rng_idx = range_idx;
	rng_data.rng_start_lba = start_clsect;
	rng_data.rng_size_lba = size_clsect;
	rng_data.rng_gen_id = gen_id;
	rng_data.rng_ent_offset = params->ent_cnt;
	rng_data.only_dirty_ents = only_dirty_ents;
	rng_data.rng_ent_block_offset = params->ent_block_cnt;
	rng_data.binje = range_binje;
	rng_data.num_ents = num_entries;
	rng_data.num_dirty_ents = 0;
	bitmap_copy(rng_data.abnd_ents_bmp, abnd_ents_in_seg_bmp, num_entries);
	bitmap_copy(rng_data.dirty_ents_bmp, dirty_ents_in_seg_bmp, num_entries);

	if (only_dirty_ents) {
		for_each_set_bit(i, dirty_ents_in_seg_bmp, num_entries) {
			union jblock_md *jmdc_ent = (*get_jmdc_ent_fn)(rng_handle, i);
			BUG_ON(!nvmeib_is_jmd_io_entry(jmdc_ent[0]));
			rng_data.num_dirty_ents++;
			memcpy(dest_jmdc, jmdc_ent, (sizeof(*dest_jmdc) * range_binje));
			dest_jmdc += range_binje;
			params->src_ent_block_idx += range_binje;
			params->ent_block_cnt += range_binje;
			*dest_ent_md = ent_md[i];
			dest_ent_md++;
			params->src_ent_idx++;
			params->ent_cnt++;
		}
	}
	else {
		/* @note: jmdc_ent has data per block, so its size is always MAX */
		for (i = 0 ; i < num_entries; i++) {
			union jblock_md *jmdc_ent = (*get_jmdc_ent_fn)(rng_handle, i);
			memcpy(dest_jmdc, jmdc_ent, sizeof(*dest_jmdc) * range_binje);
			dest_jmdc += range_binje * num_entries;
		}
		params->src_ent_block_idx += range_binje * num_entries;
		params->ent_block_cnt += range_binje * num_entries;

		memcpy(dest_ent_md, ent_md, sizeof(*ent_md) * num_entries);
		dest_ent_md += num_entries;
		params->src_ent_idx += num_entries;
		params->ent_cnt += num_entries;
		rng_data.num_dirty_ents = bitmap_weight(dirty_ents_in_seg_bmp, num_entries);
	}

	for_each_set_bit(i, dirty_ents_in_seg_bmp, num_entries) {
		union jblock_md *jmdc_ent = (*get_jmdc_ent_fn)(rng_handle, i);
		const u64 j2d_start = nvmeibs_serjio_jmd_decode_j2d_start(jmdc_ent, range_binje);
		const u64 j2d_end = nvmeibs_serjio_jmd_decode_j2d_end(jmdc_ent, range_binje);
		_NT(nvmeibs_client_c_jmdc_bit_set, "GET_JMDC for Disk @DISK_ID_STR to @CL_NAME: "
		"Range: @JRNL_RNG_IDX N: @BINJE Entry: @JRNL_RNG_ENT_IDX GenID: "
		"@JRNL_RNG_GEN_ID.@JRNL_ENT_GEN_ID J2D: [@J2D_START,@J2D_END] TxID: @TXID TxBmp_compres: @TXBM_COMPRESSED",
		params->cl->di->disk_id, params->cl->name, range_idx, 1 << range_binje, i, gen_id, ent_md[i].ent_gen_id,
		j2d_start, j2d_end, jmdc_ent[0].tx_id, jmdc_ent[0].tx_bmp);
	}

	rv = CALL_VEX_OP(encode,
					vex_ach_get_jmdc_rng_hdr_srv_ops, ONE_EXT,
						base, vex_ach_get_jmdc_rng_hdr_srv_base_encode,
						ext1, vex_ach_get_jmdc_rng_hdr_srv_ext1_encode,
							params->cl->vex_ach_ops[vex_ach_get_jmdc_rng_hdr],
							dest_rng_data, dest_rng_data + 1, &rng_data);
	BUG_ON(rv != params->rng_data_serialized_sz);

	params->src_hdr_idx++;

	_ND(trace_2_client_write_get_jmdc, "params - src_hdr_idx: @SRC_HDR_IDX dst_hdr_idx: @DST_HDR_IDX src_ent_idx: @IDX dst_ent_idx: @IDX dirty_rng_cnt: @DIRTY_RNG_CNT rng_cnt: @RNG_CNT",
			params->src_hdr_idx, params->dst_hdr_idx, params->src_ent_idx, params->dst_ent_idx, params->dirty_rng_cnt, params->rng_cnt);

	/* Check to see if we have enough buffer space for the next range */
	if ((params->src_ent_idx + NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE) * sizeof(*ent_md) >	params->src_ent_md_len ||
		(params->src_ent_block_idx + NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE) * sizeof(union jblock_md) > params->src_jmdc_len ||
		(params->src_hdr_idx + 1) * params->rng_data_serialized_sz > params->src_rng_data_len) {
		/* Not enough space in source buffer - RDMA Write JMDC, MD and Range Headers to dest */
		if ((rv = rdma_write_get_jmdc_ent(params, false))) {
			_NE(error_client_write_get_jmdc_ent, "NVMEIB_GET_JMDC - rdma_write_get_jmdc_ent failed (@RV)", rv);
			goto out;
		}

		if ((rv = rdma_write_get_jmdc_ent_md(params, false))) {
			_NE(error_client_write_get_jmdc_ent_md, "NVMEIB_GET_JMDC - rdma_write_get_jmdc_ent_md failed (@RV)", rv);
			goto out;
		}
		/* Source header buffer is full - RDMA Write it */
		if ((rv = rdma_write_get_jmdc_rng_data(params, true))) {
			_NE(error_1_client_write_get_jmdc, "NVMEIB_GET_JMDC - rdma_write_get_jmdc_rng_data failed (@RV)", rv);
			goto out;
		}

		params->dst_ent_block_idx += params->src_ent_block_idx;
		params->src_ent_block_idx = 0;
		params->dst_ent_idx += params->src_ent_idx;
		params->src_ent_idx = 0;
		params->dst_hdr_idx += params->src_hdr_idx;
		params->src_hdr_idx = 0;

		_NT(t1_s_client_write_get_jmdc,
			"GET_JMDC: No buffer space for next range, do RDMA write");
		rv = -ENOBUFS;
		goto out;
	}

	rv = 0;

out:
	_NT(t2_s_client_write_get_jmdc, "range range_idx: @JRNL_RNG_IDX, rv=@RV",
		range_idx, rv);

	return rv;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jmdc_rsp_srv_base_encode)
{
	struct volume_server_get_jmdc_rsp_base *base = wire_buf;
	struct write_get_jmdc_params *params = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(base) > wire_buf_end);

	memcpy(base->serjio_boot_id, nvmeibs_serjio_get_boot_id(params->cl->di), NVMEIB_GID_STR_MAX);
	base->jrnl_start_lba = cpu_to_be64(params->jrnl_start_clsect);
	base->jrnl_len_lba = cpu_to_be64(params->jrnl_len_clsect);
	base->num_ents_rng = cpu_to_be16(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	base->num_rng = cpu_to_be16((u16)params->rng_cnt);
	base->num_dirty_rng = cpu_to_be16((u16)params->dirty_rng_cnt);
	base->hdr_len = cpu_to_be32(params->rng_cnt * params->rng_data_serialized_sz);
	base->ents_len = cpu_to_be32(params->ent_block_cnt * sizeof(union jblock_md));
	base->num_ents = cpu_to_be32(params->ent_cnt);
	base->ents_md_len = cpu_to_be32(params->ent_cnt * sizeof(struct nvmeib_jrnl_ent_md));

	return sizeof(*base);
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_ach_get_jmdc_rsp_srv_ext1_encode)
{
	struct volume_server_get_jmdc_rsp_ext1 *ext1 = wire_buf;
	struct write_get_jmdc_params *params = arg;

	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(ext1) > wire_buf_end);

	ext1->hdr_version = cpu_to_be16(params->cl->vex_ach_ops[vex_ach_get_jmdc_rng_hdr]->vex_ext);
	ext1->hdr_stride = cpu_to_be16(params->rng_data_serialized_sz);
	ext1->num_ent_blocks = cpu_to_be32(params->ent_block_cnt);

	return sizeof(*ext1);
}

static void get_jmdc(struct nvmeibs_client *cl, struct nvmeib_iu *recv_ioctx,
		     struct nvmeib_iu *send_ioctx)
{
	struct volume_client_req *req = recv_ioctx->buf;
	void *payload = NULL;
	int payload_len = 0;
	int rv;
	struct write_get_jmdc_params params = {};
	unsigned start_rng = be16_to_cpu(req->jmdc_req.start_rng);
	unsigned num_rng_rem = be16_to_cpu(req->jmdc_req.num_rng);
	ssize_t rng_data_serialized_sz;
	struct volume_server_get_jmdc_rsp rsp;
	int n_comps = 0;
	int n_src_rng;
	DECLARE_COMPLETION_ONSTACK(rdma_comp);
	unsigned long flags;
	int wait_rv;

	__NFIN;



	if (be16_to_cpu(req->version_tag) != vex_base) {
		_NT(get_jmdc_t1, "Invalid version tag @UINT for NVMEIB_GET_JMDC", be16_to_cpu(req->version_tag));
		rv = -EPROTO;
		goto out;
	}

	_NT(trace_client_get_jmdc, "--- Start handling NVMEIB_GET_JMDC message "
		"(client @CLIENT_NAME, disk @DISK_NAME, seg_uuid: @SEG_UUID, start: @START_RNG, num: @NUM_RNG) start from host @CL_NAME",
	   req->jmdc_req.client_name, req->jmdc_req.disk_name, req->jmdc_req.seg_uuid,
	   start_rng, num_rng_rem, cl->name);

	if (!cl->net->qp) {
		_NE(error_client_get_jmdc, "cl @CL_NAME net->qp is NULL", cl->name);
		rv = -1;
		goto out;
	}

	params.cl = cl;
	params.jmdc_req = &req->jmdc_req;

	if ((rng_data_serialized_sz = vex_size(params.cl->vex_ach_ops[vex_ach_get_jmdc_rng_hdr], NULL, true)) < 0) {
		_NE(err_client_get_jmdc_inv_rng_hdr_sz, "NVMEIB_GET_JMDC - Failed (@RV) getting Range Data Header Size", rng_data_serialized_sz);
		rv = rng_data_serialized_sz;
		goto send_rsp;
	}
	params.rng_data_serialized_sz = (size_t)rng_data_serialized_sz;

	if ((rv = nvmeibs_serjio_get_jrnl_clsects(
		cl->di, &params.jrnl_start_clsect, &params.jrnl_len_clsect))) {
		_NE(error_4_client_get_jmdc, "NVMEIB_GET_JMDC - nvmeibs_serjio_get_disk_ranges failed (@RV)", rv);
		goto send_rsp;
	}

	/* Allocate and map src data memory */
	n_src_rng = req->jmdc_req.get_client_uuid_only ? 1 : NUM_GET_JMDC_SRC_RNG;
	params.src_map.pd = P2NV(cl->net->params.port)->pd;
	params.src_map.n_pages =
		NUM_GET_JMDC_RNG_DATA_SRC_PAGES(
			n_src_rng, params.rng_data_serialized_sz) +
		NUM_GET_JMDC_MD_SRC_PAGES(n_src_rng) +
		NUM_GET_JMDC_ENT_MD_SRC_PAGES(n_src_rng);
	if (!(params.src = nvmeib_mem_alloc_n_vmap(&params.src_map))) {
		_NE(error_client_get_jmdc_alloc_n_map_fail,
			"nvmeib_mem_alloc_n_vmap failed");
		rv = -ENOMEM;
		goto send_rsp;
	}
	/* The order here is completely arbitrary, the data is sent to 3 different MRs on the client */
	params.src_rng_data_off = 0;
	params.src_rng_data_len = NUM_GET_JMDC_RNG_DATA_SRC_PAGES(
		n_src_rng, params.rng_data_serialized_sz) << PAGE_SHIFT;
	params.src_jmdc_off = params.src_rng_data_off + params.src_rng_data_len;
	params.src_jmdc_len = NUM_GET_JMDC_MD_SRC_PAGES(n_src_rng) << PAGE_SHIFT;
	params.src_ent_md_off = params.src_jmdc_off + params.src_jmdc_len;
	params.src_ent_md_len = NUM_GET_JMDC_ENT_MD_SRC_PAGES(n_src_rng) << PAGE_SHIFT;

	BUG_ON(params.src_ent_md_off + params.src_ent_md_len > (params.src_map.n_pages << PAGE_SHIFT));

	/* Set completion for RDMA WRITE */
	spin_lock_irqsave(&cl->get_jmdc_comp_lock, flags);
	BUG_ON(cl->get_jmdc_comp);
	cl->get_jmdc_comp = &rdma_comp;
	spin_unlock_irqrestore(&cl->get_jmdc_comp_lock, flags);

	if (req->jmdc_req.get_client_uuid_only) {
		if ((rv = nvmeibs_serjio_call_for_client_range(cl->di, req->jmdc_req.client_uuid,
				NVMEIB_EC_INVALID_JOURNAL_BINJE, /* Match any N */
				req->jmdc_req.seg_uuid, write_get_jmdc, &params)) < 0) {
			goto send_rsp;
		}
	} else {
		while ((rv = nvmeibs_serjio_call_for_each_assigned_range(cl->di, start_rng, num_rng_rem,
			req->jmdc_req.seg_uuid, write_get_jmdc, &params)) == -ENOBUFS) {
			_NT(t0_s_cl_get_jmdc_enobufs, "Wait for send-comp #@INT", n_comps);
			if ((wait_rv = wait_for_completion_timeout(&rdma_comp, 15 * HZ)) <= 0) {
				_NE(error_1_client_rdma_write_get_jmdc_rng_data, "NVMEIB_GET_JMDC - Timed-out waiting for RDMA Write");
				rv = -ETIMEDOUT;
				goto send_rsp;
			}
			n_comps++;
			nvmeib_reinit_completion(&rdma_comp);
			num_rng_rem -= (params.last_rng_idx + 1) - start_rng;
			start_rng = params.last_rng_idx + 1;

			_NT(t1_s_cl_get_jmdc_enobufs,
				"--- Cont handling NVMEIB_GET_JMDC message "
				"last_rng_idx=@INT, start_rng=@START_RNG, num_rng_rem=@NUM_RNG",
			   params.last_rng_idx, start_rng, num_rng_rem);
		}

		if (rv < 0) {
			_NE(error_1_client_get_jmdc, "Failed (@RV) to write NVMEIB_GET_JMDC data to host @CL_NAME", rv, cl->name);
			goto send_rsp;
		}
	}

	if (params.src_ent_idx > 0) {
		/* Write the last of the entries JMDC and MD */
		if ((rv = rdma_write_get_jmdc_ent(&params, false))) {
			_NE(error_client_write_get_jmdc, "NVMEIB_GET_JMDC - rdma_write_get_jmdc_ent failed (@RV)", rv);
			goto send_rsp;
		}
	}

	if (params.src_ent_block_idx > 0) {
		if ((rv = rdma_write_get_jmdc_ent_md(&params, false))) {
			_NE(error_2_client_get_jmdc, "NVMEIB_GET_JMDC - rdma_write_get_jmdc_ent_md failed (@RV)", rv);
			goto send_rsp;
		}
	}

	if (params.src_hdr_idx > 0) {
		/* Write the last of the range data */
		if ((rv = rdma_write_get_jmdc_rng_data(&params, false))) {
			_NE(error_3_client_get_jmdc, "NVMEIB_GET_JMDC - rdma_write_get_jmdc_rng_data failed (@RV)", rv);
			goto send_rsp;
		}
	}

	payload = &rsp;
	rv = CALL_VEX_OP(encode,
					vex_ach_get_jmdc_rsp_srv_ops, ONE_EXT,
						base, vex_ach_get_jmdc_rsp_srv_base_encode,
						ext1, vex_ach_get_jmdc_rsp_srv_ext1_encode,
							cl->vex_ach_ops[vex_ach_get_jmdc_rsp],
							payload, payload + sizeof(rsp), &params);
	BUG_ON(rv <= 0);
	payload_len = rv;
	rv = 0;

	_NT(trace_2_client_get_jmdc, "NVMEIB_GET_JMDC to @CL_NAME - boot_id: @SERJIO_BOOT_ID jrnl_start_lba: @START_LBA jrnl_len_lba: @END_LBA num_ents_rng: @RNG num_rng: @RNG"
	" num_dirty_rng: @RNG hdr_len: @LENGTH_INT ents_len: @LENGTH_INT ents_md_len: @ENT_MD_LEN", cl->name, rsp.base.serjio_boot_id,
	be64_to_cpu(rsp.base.jrnl_start_lba), be64_to_cpu(rsp.base.jrnl_len_lba), be16_to_cpu(rsp.base.num_ents_rng),
	   be16_to_cpu(rsp.base.num_rng), be16_to_cpu(rsp.base.num_dirty_rng), be32_to_cpu(rsp.base.hdr_len),
	   be32_to_cpu(rsp.base.ents_len), be32_to_cpu(rsp.base.ents_md_len));

send_rsp:
	nvmeibs_client_send_rsp(cl, cl->net,
			rv == 0 ? NVMEIBS_RSP_MGMT_OPCODE_OK : NVMEIBS_RSP_MGMT_OPCODE_ERR,
			req->hdr.tag, req->version_tag, send_ioctx, payload, payload_len, NVMEIB_RDMA_GET_JMDC_RSP, NON_NR_VERSION);

	if (!rv) {
		_NT(t0_s_cl_get_jmdc_wait_final, "Wait for send-comp #@INT", n_comps);
		if ((wait_rv = wait_for_completion_timeout(&rdma_comp, 15 * HZ)) <= 0) {
			_NE(error_cl_get_jmdc_wait_final_timeout, "NVMEIB_GET_JMDC - Timed-out waiting for RDMA Write");
			rv = -ETIMEDOUT;
		}
	}

	spin_lock_irqsave(&cl->get_jmdc_comp_lock, flags);
	cl->get_jmdc_comp = NULL;
	spin_unlock_irqrestore(&cl->get_jmdc_comp_lock, flags);

	if (params.src)
		nvmeib_mem_vunmap_n_free(&params.src_map, params.src);


out:
	_NT(trace_3_client_get_jmdc, "--- Finish handling NVMEIB_GET_JMDC message from host @CL_NAME (rv @RV)",
	   cl->name, rv);
	__NFOUT;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_ach_dbg_cmd_base_srv_decode)
{
	struct volume_client_dbg_req_base *res = arg;
	const struct volume_client_dbg_req_base *req = wire_buf;
	(void)arg;

	BUG_ON(!wire_buf); /* Should never be NULL for base decode */
	BUG_ON(wire_buf + sizeof(req) > wire_buf_end);

	res->death_wish.has_death_wish = be32_to_cpu(req->death_wish.has_death_wish);
	res->death_wish.rsc_id = be64_to_cpu(req->death_wish.rsc_id);
	res->death_wish.dlba = be64_to_cpu(req->death_wish.dlba);

	return sizeof(struct volume_client_dbg_req_base);
}

#define SELF_CRASH_STR "nvmeibs module self crash to prevent data corruption"
static void handle_please_kill_yourself_cb(void *arg, int status, u32 result) {
	_NE_dmesg(dbg_please_kill_yourself_death_note_after_read, SELF_CRASH_STR "Done. Error code: 1064. SG=@PTR.", arg);
	BUG();
}

static bool handle_please_kill_yourself(struct nvmeibs_client *cl, struct volume_client_dbg_please_kill_yourself *args) {
	NFIN;
	_NE_dmesg(dbg_please_kill_yourself_death_note, SELF_CRASH_STR "Start. Error code: 1065. Additional info: rsc_id=@RSC_ID, dlba=@DLBA, cl=@PTR, q_info=@PTR",
	    (u32)args->base.death_wish.rsc_id, (u64)args->base.death_wish.dlba, cl,
	    nvmeibs_q_info_get_by_rsc_id(cl->di, args->base.death_wish.rsc_id));
	if (!args->base.death_wish.has_death_wish) BUG();
	else {
		struct nvmeibs_nvme_req *nvme_req;
		dma_addr_t phys_dma;
		dma_addr_t*phys_virt;
		size_t ndatapages = 1;
		size_t nmdpages = cl->di->metadata ? 1 : 0;
		size_t npages = ndatapages + nmdpages;
		dma_addr_t mdpage_phys = 0;
		void *mdpage_virt = NULL;
		int block_shift_delta = NVMEIBC_SECTOR_SHIFT - cl->di->block_shift;
		int i, rv;

		BUG_ON(!(nvme_req = kzalloc(sizeof(*nvme_req), GFP_KERNEL)));

		BUG_ON(!(phys_virt = dma_alloc_coherent(get_nvme_dma_device(cl->di->dev),
		                                        sizeof(*phys_virt) * npages,
		                                        &phys_dma, GFP_KERNEL)));

		for (i = 0; i < npages; i++) {
			BUG_ON(!(mdpage_virt = dma_alloc_coherent(
			             get_nvme_dma_device(cl->di->dev), PAGE_SIZE,
			             &phys_virt[i], GFP_KERNEL)));
			if (i == npages - 1) mdpage_phys = phys_virt[i];
		}
		nvme_req->use_sg = false;
		nvme_req->buf_addrs = phys_virt;
		nvme_req->prpl_phys = phys_dma;

		nvme_req->mtdt_dma_ptr = mdpage_phys;
		nvme_req->metadata = nmdpages ? mdpage_virt : NULL;
		nvme_req->mtdt_size = nmdpages * cl->di->metadata;

		nvme_req->nvme_op = nvme_cmd_read;
		nvme_req->use_hw_blocks = 1;
		nvme_req->disk_block = args->base.death_wish.dlba >> block_shift_delta;
		nvme_req->data_len = PAGE_SIZE;
		nvme_req->cb = handle_please_kill_yourself_cb;
		nvme_req->arg = phys_virt;
		nvme_req->disk_info = cl->di;

		if ((rv = submit_local_cmd(cl->di, nvme_req))) {
			_NE_dmesg(suice_death_wish_failed, SELF_CRASH_STR "Abort. Error code: 1066. rv=@RV", rv);
			BUG();
		}
	}
	NFOUT;

	return false; /* This operation never returns */
}

static bool handle_dbg_cmd(struct nvmeibs_client *cl,
                           struct nvmeib_iu *recv_ioctx,
                           struct nvmeib_iu *send_ioctx) {
	struct volume_client_req *req = recv_ioctx->buf;
	const struct vex_ops *vex_ops = cl->vex_ach_ops[vex_ach_dbg_cmd];
	int rv;
	bool is_done;

	__NFIN;
	if (be16_to_cpu(req->version_tag) != vex_base) {
		_NT(handle_dbg_cmd_t1, "Invalid version tag @UINT for NVMEIB_DBG_CMD",
		    be16_to_cpu(req->version_tag));
		rv = -EPROTO;
		is_done = true;
		goto out;
	}

	CALL_VEX_OP(decode, vex_ach_dbg_cmd_srv_ops, BASE_ONLY, base,
	            vex_ach_dbg_cmd_base_srv_decode, vex_ops,
	            NVMEIBC_VOLUME_CLIENT_DBG_REQ_CONST_PAYLOAD(req),
	            recv_ioctx->buf + recv_ioctx->size, &req->dbg_req.pky_req.base);

	switch ((enum nvmeibc_dbg_cmd_ops)req->dbg_req.opcode) {
		case NVMEIBC_DBG_PLEASE_KILL_YOURSELF:
			is_done = handle_please_kill_yourself(cl, &req->dbg_req.pky_req);
			goto done; /* Yeah, sure */

		/* No default. Handle everything. Don't be lazy. */
	}
	/* Being here means corrupted enum */
	is_done = true;
	WARN(true, "Invalid op %d for NVMEIB_DBG_CMD\n", req->dbg_req.opcode);
	rv = -EPROTO;
	goto out;
done:
	rv = 0;
out:
	_NT(handle_dbg_cmd_t2, "handle_dbg_cmd rv:@RV", rv);
	__NFOUT;
	return is_done;
}


#if 0 /* DEBUG ONLY */
static void dump_send_buffer(struct nvmeibs_client *cl)
{
	void *ioch = cl->_ioch_;

	__NFIN;
	if (false && ioch) {
		_ND(fill_config_alloc_net_req_d1, "Sene Q buffer");
		nvmeib_dump_page(ioch->qp_rsc.sq.vaddr);
		_ND(fill_config_alloc_net_req_d2, "DB descriptor=@INT32_HEX",
			*(u32 *)ioch->qp_rsc.sq.doorbell_descr_address_virt);
	}
	__NFOUT;
}
#endif

static void alloc_nr_nets(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	struct volume_client_req *req;
	struct volume_client_config_alloc_net_req *creq;
	struct volume_server_config_alloc_nr_net_rsp *rsp = NULL;
	struct nvmeibs_rionic *rionic;
	struct nvmeibs_nr_channel *nrch;
	void *payload = NULL;
	u16 channel_id;
	int payload_len = 0;
	int rv;
	unsigned long ts;
	__NFIN;

	req = recv_ioctx->buf;

	if (be16_to_cpu(req->version_tag) != vex_base) {
		_NT(__AUTOID__, "Invalid version tag @VERSION in NVMEIBC_MA_ALLOC_IO_NET", be16_to_cpu(req->version_tag));
		rv = -EPROTO;
		goto send_rsp;
	}

	creq = &req->config_req.a_net_req;
	channel_id = be16_to_cpu(creq->def.qp_num);

	if (!(rsp = kzalloc(sizeof(*rsp), GFP_KERNEL))) {
		_NE(error_client_alloc_nr_nets, "OOM: Fail to allcoate alloc_nr_net rsp, host @CL_NAME, qpn=@QPN",
			cl->name,  be16_to_cpu(creq->def.qp_num));
		rv = -ENOMEM;
		goto send_rsp;
	}

	_NT(trace_client_alloc_nr_nets, "--- Start handling NVMEIBC_MA_ALLOC_NR_NET message from host @CL_NAME "
	   "qpn=@QPN", cl->name,  be16_to_cpu(creq->def.qp_num));
	if (!(rionic = nvmeibs_client_get_rionic(cl, &creq->def,
		NVMEIBS_IOCH_NORDDA))) {
		_NE(error_1_client_alloc_nr_nets, "Invalid client parameters: src nic=@NIC_STR, dst nic=@NIC_STR, channel=@CHANNEL",
			creq->def.sgid, creq->def.dgid, channel_id);
		rv = -EINVAL;
		goto send_rsp;
	}

	nrch = &rionic->nr_channels[channel_id];
	if (nrch->id == -1) {
		_NI(trace_1_client_alloc_nr_nets, "NR-channel is not connected");
		rv = -ENOTCONN;
		goto send_rsp;
	} else {
		union nvmeib_version link_ver = nrch->net->params.cl->link_version;
		nrch->io_srv_lock_req_ops = vex_select_ops(link_ver, vex_nrch_io_srv_lock_req_srv_ops);
		nrch->io_srv_lock_rsp_ops = vex_select_ops(link_ver, vex_nrch_io_srv_lock_rsp_srv_ops);
		nrch->gen_br_ops = vex_select_ops(link_ver, vex_nrch_gen_br_req_srv_ops);
		nrch->gen_uj_ops = vex_select_ops(link_ver, vex_nrch_gen_uj_req_srv_ops);
		nrch->gen_db_ops = vex_select_ops(link_ver, vex_nrch_gen_db_req_srv_ops);
		nrch->gen_fje_ops = vex_select_ops(link_ver, vex_nrch_gen_fje_srv_ops);
		nrch->gen_fje_ent_ops = vex_select_ops(link_ver, vex_nrch_gen_fje_ent_srv_ops);
		nrch->gen_je_ops = vex_select_ops(link_ver, vex_nrch_gen_je_srv_ops);
	}

	if (nvmeib_version_protocol_lt(&cl->link_version, &nvmeib_2p1_version)) {
		_NT(trace_5_client_alloc_nr_nets, "omit check cs-gid for older client");
	}
	else {
		u64 cs_gid = be64_to_cpu(creq->cs_gid);
		if (cs_gid <= nrch->cs_gid) {
			_NW(trace_4_client_alloc_nr_nets,
				"NR-channel recv cs-gid @LLU exp. > @LLU", cs_gid, nrch->cs_gid);
			rv = -EINVAL;
			goto send_rsp;
		}
		_NT(trace_6_client_alloc_nr_nets,
			"nrch @NRCH_NAME, cs-gid: @LLU --> @LLU", nrch->name, nrch->cs_gid, cs_gid);
		nrch->cs_gid = cs_gid;
	}

	_NT(trace_2_client_alloc_nr_nets, "nrch @NRCH_NAME, alloc & map iocmds and fill rsp", nrch->name);
	ts = jiffies;
	if (!(rv = nvmeibs_nordda_io_cmds_alloc(nrch)) &&
		!(rv = nvmeibs_nordda_fill_config_alloc_nr_net_rsp(nrch, rsp))) {
		payload = rsp;
		payload_len = sizeof(*rsp);
	}
	if (jiffies - ts > HZ * 3) {
		_NW(trace_2_client_alloc_nr_nets_x, "nrch @NRCH_NAME, took @LLU to connect", nrch->name, jiffies - ts);
	}

send_rsp:
	nvmeibs_client_send_rsp(cl, cl->net,
		rv == 0 ? NVMEIBS_RSP_MGMT_OPCODE_OK : NVMEIBS_RSP_MGMT_OPCODE_ERR,
		req->hdr.tag, req->version_tag, send_ioctx, payload, payload_len, NVMEIB_SEND_CFG, NON_NR_VERSION);
	_NT(trace_3_client_alloc_nr_nets, "--- Finish handling NVMEIBC_MA_ALLOC_NR_NET message from host @CL_NAME, rv=@RV",
		cl->name, rv);
	kfree(rsp);
	__NFOUT;
}

/**
 * nvmeibs_handle_fmr_reg(): The FMR registration return from
 * the send_q. Once we manage to register the send_q buffer for
 * remote access we need to initiate it (e.g. mlx5)
 *
 */
static void handle_fmr_reg(struct nvmeib_iu *send_ioctx)
{
	struct nvmeibs_net *net;

	NFIN;
	if (send_ioctx->io_done)
		complete(send_ioctx->io_done);
	else {
		_NT(trace_client_handle_fmr_reg, "fmr registration @STATUS_STR",
			send_ioctx->io_status == IB_WC_SUCCESS ? "was OK" : "failed");
		/* return send context to free_msg list */
		net = send_ioctx->priv;
		nvmeibs_net_put_ioctx(net, send_ioctx);
	}
	NFOUT;
}

static void handle_fmr_inv(struct nvmeib_iu *send_ioctx)
{
	struct nvmeibs_net *net;

	NFIN;
	/* wakeup waiter for the fmr invalidation completion */
	if (send_ioctx->io_done)
		complete(send_ioctx->io_done);
	else {
		_NT(trace_client_handle_fmr_inv, "fmr invalidation @STATUS_STR",
			send_ioctx->io_status == IB_WC_SUCCESS ? "was OK" : "failed");
		/* return send context to free_msg list */
		net = send_ioctx->priv;
		nvmeibs_net_put_ioctx(net, send_ioctx);
	}
	NFOUT;
}


/**
 * nvmeibs_handle_client_config_req() - Client configuration
 * request. The following options are supported:
 * NVMEIBC_MA_GET_IO: Get the attached disks and nics.
 * NVMEIBC_MA_GET_ACCESS: Build acces plan for the client.
 * NVMEIBC_MA_ALLOC_IO_NET: Send the io channel info to allow
 * NVMEIBC_MA_LOCATE_RSC: Send disk resoureces the client can
 * use.
 *
 */
static void handle_config_req(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	const struct volume_client_req *creq = recv_ioctx->buf;
	__NFIN;
	switch (creq->config_req.opcode) {
	case NVMEIBC_MA_SHARE_CONFIG:
		verify_link_config(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIBC_MA_GET_IO:
		get_io_info(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIBC_MA_GET_ACCESS:
		get_access_info(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIBC_MA_ALLOC_IO_NET:
		BUG();
		break;
	case NVMEIBC_MA_RESET_RSC:
		BUG();
		break;
	case NVMEIBC_MA_GET_DISK_MEMS:
		send_disk_lock_mems(cl,recv_ioctx,send_ioctx);
		break;
	case NVMEIBC_MA_ALLOC_NR_NET:
		alloc_nr_nets(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIBC_MA_GET_LOCK_GIDS:
		get_lock_gids(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIBC_MA_GET_JRANGE:
		get_jrange(cl, recv_ioctx, send_ioctx);
		break;
	default:
		_NW(warn_client_handle_config_req, "Unknown client config message op");
		break;
	}
	__NFOUT;
}

/**
 * handle_rsp() - Client response for a controller
 * request.
 */
static void handle_rsp(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx)
{
	struct volume_client_rsp *rsp = recv_ioctx->buf;
	struct nvmeibs_cmd_info *info;
	bool found = false;
	int i = 0;

	__NFIN;
	if ((rsp->opcode <= NVMEIBC_RSP_OPCODE_BASE) ||
		(rsp->opcode >= NVMEIBC_RSP_LOGOUT_LAST)) {
		_NE(error_client_handle_rsp, "Invalid response opcode @OPCODE", rsp->opcode);
	}
	if ((rsp->opcode == NVMEIBC_RSP_TOMA_OPCODE_OK) ||
		(rsp->opcode == NVMEIBC_RSP_TOMA_OPCODE_ERR)) {
		_ND(trace_client_handle_rsp, "Received TOMA response (opcode = @OPCODE_STR)",
		   rsp->opcode == NVMEIBC_RSP_TOMA_OPCODE_OK ? "OK" : "ERR");

		goto out;
	}

	if (be16_to_cpu(rsp->version_tag) != vex_base) {
		_NT(handle_rsp_t1, "Invalid version tag @UINT in NVMEIB_RSP", be16_to_cpu(rsp->version_tag));
		goto out;
	}

	list_for_each_entry(info, &cl->wire_cmd, link) {
		if (info == (struct nvmeibs_cmd_info *)rsp->hdr.tag) {
			found = true;
			break;
		}
		i++;
	}

	if (unlikely(!found)) {
		_NE(error_1_client_handle_rsp,
			"OOPS, @CL (@CL_NAME) received rsp with uknown tag=@LLU, "
			"opcode: {@OPCODE, @OPCODE}), i=@INT",
			cl, cl->name, rsp->hdr.tag,  rsp->hdr.opcode, rsp->opcode, i);
		WARN_ON_ONCE(1);
	}
	else if (unlikely(!info->has_rsp)) {
		/* NVMEIBS_JAM_ABND2FREE, NVMEIBS_RGID_CHANGE and NVMEIBS_IOCH_DRAINED
		   don't have rsp */
		_NE(error_2_client_handle_rsp, "OOPS, info for which rsp is not exp was left in list, op @OP",
		   info->op);
	}
	else if (rsp->opcode == NVMEIBC_RSP_OPCODE_OK) {
		list_del(&info->link);
		if (info->op == NVMEIBS_PUT_RSC) {
			/* nothing to do in this case */
			kfree(get_put_info(info));
		} else if (info->op == NVMEIBS_GET_RSC) {
			/* give the disk the resources */
			nvmeibs_disk_put_resources(
				cl, get_pg_info(info)->disk_name, &rsp->g_rsp);
			kfree(get_get_info(info));
		}
		 else {
			_NE(error_3_client_handle_rsp, "OOPS: unknown controller command op code @OP", info->op);
			kfree(info);
		}
	}
	else {
		_NE(error_4_client_handle_rsp, "Received rsp error tag=@TAG_PTR", info);
	}

out:
	__NFOUT;
}


/*
* Logout related functions
*/

inline static void logout_rsp_recv_comp(struct nvmeibs_client *cl)
{
	if(cl->net->logout_sent_comp) {
		_NT(trace_logout_rsp_recv_comp,
		 "LOGOUT: got logout response, calling complete on @NET", cl->net);
		complete(cl->net->logout_sent_comp);
	}
}

int nvmeibs_client_logout(struct nvmeibs_net const *net)
{
	struct nvmeibs_client_logout_msg *cmd;
	struct volume_server_req *req;
	struct nvmeib_iu * iu;
	int rv;

	if (nvmeib_version_protocol_lt(&net->params.cl->link_version, &nvmeib_2p4_version)) {
		_ND(trace_nvmeibs_client_logout_skip, "omit logout send to older client");
		rv = 0;
		goto out;
	}

	if (!(cmd = kzalloc(sizeof(*cmd), GFP_KERNEL))) {
		_NE(trace_nvmeibs_client_logout_error_alloc, "Fail to alloc cmd");
		rv = -ENOMEM;
		goto out;
	}

	if (!(iu = nvmeibs_client_get_ioctx(net->params.cl))) {
		_NW(trace_nvmeibs_client_logout_no_iu, "No iu available");
		kfree(cmd);
		rv = -EBUSY;
		goto out;
	}

	req = iu->buf;
	req->logout_req.reason = net->logout_reason;
	cmd->cmd.info.has_rsp = true;
	cmd->cmd.info.op = NVMEIBS_LOGOUT;
	memcpy(cmd->cmd.disk_name, net->params.cl->disk_name,
			NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	_NT(trace_0_nvmeibs_client_logout,
		 "LOGOUT: sending logout to net @NET, OPCODE: @LOGOUT_REASON", net,
		 net->logout_reason);
	/* pass ownership of req and cmd to send_resource */
	send_resource(net->params.cl, &cmd->cmd.info, vex_base, iu);
	rv = 0;

out:
	return rv;
}

/**
 * handle_logout() - Client is done.
 */
static void handle_logout(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	__NFIN;
	/* nothing to do for now */
	if (send_ioctx)
		nvmeibs_net_put_ioctx(cl->net, send_ioctx);
	__NFOUT;
}

static void handle_msg(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	struct nvmeib_hdr *hdr;
	bool post_recv = true;

	__NFIN;
	hdr = recv_ioctx->buf;
	switch (hdr->opcode) {
	case NVMEIB_CONFIG:
		handle_config_req(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIB_RSP:
		if (send_ioctx)
			nvmeibs_net_put_ioctx(cl->net, send_ioctx);
		handle_rsp(cl, recv_ioctx);
		break;
	case NVMEIB_C_LOGOUT:
		handle_logout(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIB_TOMA_REQ:
		_ND(trace_client_handle_msg, "Received TOMA request");
		nvmeibs_toma_client_proc_send(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIB_GET_JMDC:
		_ND(trace_1_client_handle_msg, "Received Get JMDC Request");
		get_jmdc(cl, recv_ioctx, send_ioctx);
		break;
	case NVMEIB_DBG_CMD:
		_ND(trace_2_client_handle_msg, "Received a DBG Command");
		post_recv = handle_dbg_cmd(cl, recv_ioctx, send_ioctx);
		break;
	default:
		_NW(warn_client_handle_msg, "received Client message with opcode @OPCODE", hdr->opcode);
		break;
	}
	/* return the received context to the shared received queue */
	if (post_recv)
		post_recv_iu(cl, recv_ioctx);
	__NFOUT;
}

static void handle_msg_work(struct nvmeib_iu *recv_ioctx)
{
	struct nvmeib_iu *send_ioctx = recv_ioctx->work_payload;
	struct nvmeibs_net *net = recv_ioctx->owner_ptr;
	struct nvmeibs_client *cl = net->priv;
	u32 recv_idx;
	__NFIN;

	BUG_ON(!cl);
	BUG_ON(!cl->net);

	/* prep for post-back to srq */
	recv_idx = recv_ioctx->recv_idx;
	NVMEIB_IU_RESET_RCV_IDX(recv_ioctx);

	/* sanity */
	if (unlikely(nvmeibs_net_get_qp_state(cl->net) != QP_LIVE)) {
		_ND(dbg_0_s_client_handle_msg_work,
			"@CL_NAME, @NET not live", cl->name, cl->net);
		goto err;
	}

	if (unlikely(atomic_read(&cl->net->dying))) {
		_ND(dbg_1_s_client_handle_msg_work,
			"@CL_NAME, @NET is dying", cl->name, cl->net);
		goto err;
	}

	/* check re-order */
	if (unlikely(recv_idx == 0 || recv_idx != cl->recv_idx_head)) {
		_NE(err_0_s_client_handle_msg_work,
			"@CL_NAME: detected recv reorder, idx @INT, exp. @INT",
			cl->name, recv_idx, cl->recv_idx_head);
		goto err;
	}

	/* update recv-idx head (only here on wq --> no lock) */
	if (++cl->recv_idx_head == 0)
		++cl->recv_idx_head;

	handle_msg(cl, recv_ioctx, send_ioctx);
	goto out;

err:
	post_recv_iu(cl, recv_ioctx);
	nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_MSG_HANDLE_FAILED);

out:
	__NFOUT;
}

static int cl_recv_msg_work_add_(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx, struct nvmeib_iu *send_ioctx)
{
	int rv;
	__NFIN;

	if (!send_ioctx) {
		_NE(err_0_cl_recv_msg_work_add_, "No send-ioctx");
		rv = -EINVAL;
		goto err;
	}

	recv_ioctx->work_payload = send_ioctx;
	recv_ioctx->work_handler = handle_msg_work;

	rv = nvmeibs_client_add_work(cl, &recv_ioctx->work);
	if (!rv)
		goto out;

err:
	NVMEIB_IU_RESET_RCV_IDX(recv_ioctx);
	post_recv_iu(cl, recv_ioctx);
	if (send_ioctx)
		nvmeibs_net_put_ioctx(cl->net, send_ioctx);
	nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_RCV_ADD_WORK_FAILED);

out:
	__NFOUT;
	return rv;
}

static int handle_new_msg(struct nvmeibs_client *cl,
	struct nvmeib_iu *recv_ioctx)
{
	int r_msg_size = cl->net->params.r_msg_size;
	struct nvmeib_hdr *hdr = recv_ioctx->buf;
	struct nvmeib_iu *send_ioctx = NULL;
	unsigned long flags;
	int rv;

	BUG_ON(!cl);
	BUG_ON(!recv_ioctx);

	/* JH IOMMU: DMA_FROM_DEVICE is correct. Used as a sink for Remote RDMA SEND */
	ib_dma_sync_single_for_cpu(P2IB(cl->ib_port), recv_ioctx->dma,
		r_msg_size, DMA_FROM_DEVICE);

	/* sanity */
	if (recv_ioctx->recv_idx != 0) {
		_NE_dmesg(err_0_s_client_handle_new_msg, "@CL_NAME: unexp. recv-idx @INT p=@PTR", cl->name, recv_ioctx->recv_idx, recv_ioctx);
		BUG_NON_PRODUCTION(975);
		/* reset it to be on the safe side */
		NVMEIB_IU_RESET_RCV_IDX(recv_ioctx);
		post_recv_iu(cl, recv_ioctx);
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_RCV_UNEXPECTED_IDX);
		rv = -1;
		goto out;
	}

	/* update ka-timeout for any recv-msg */
	cl->ka_timeout = jiffies + NVMEIB_KEEP_ALIVE_TO;

	/* if msg does not require processing, we are done */
	if (hdr->opcode == NVMEIB_KEEP_ALIVE) {
		post_recv_iu(cl, recv_ioctx);
		rv = 0;
		goto out;
	}

	if (is_logout_rsp(hdr, recv_ioctx)) {
		logout_rsp_recv_comp(cl);
		post_recv_iu(cl, recv_ioctx);
		rv = 0;
		goto out;
	}

	/* [NVMESH-1153]: send_toma_msg (on s_client wq) now waits for this response to protect the client accumulation buffer.
	 * Therefore, we must process it here and not on s_client wq.
	 * Ideally it should be a different opcode, but we don't want to change the protocol */
	if (hdr->opcode == NVMEIB_RSP)
		_ND(trace_s_client_handle_new_msg, "@CL (@CL_NAME), Rsp with tag @TAG",
		    cl, cl->name, hdr->tag);
	if (hdr->opcode == NVMEIB_RSP &&
		hdr->tag == cl->clnt_toma_rsp.tag_cntr) {
		struct volume_client_rsp *rsp = recv_ioctx->buf;
		unsigned long flags;

		BUG_ON(rsp->opcode != NVMEIBC_RSP_TOMA_OPCODE_OK &&
			rsp->opcode != NVMEIBC_RSP_TOMA_OPCODE_ERR);

		spin_lock_irqsave(&cl->clnt_toma_rsp.comp_lock, flags);
		cl->clnt_toma_rsp.opcode = rsp->opcode;
		if (cl->clnt_toma_rsp.comp)
			complete(cl->clnt_toma_rsp.comp);
		spin_unlock_irqrestore(&cl->clnt_toma_rsp.comp_lock, flags);
		post_recv_iu(cl, recv_ioctx);
		rv = 0;
		goto out;
	}

	/* update recv-idx tail (only here on rx-comp --> no lock) */
	recv_ioctx->recv_idx = cl->recv_idx_tail;
	if (++cl->recv_idx_tail == 0)
		++cl->recv_idx_tail;

	/* defer recv-msg to pending or wq */
	spin_lock_irqsave(&cl->spinlock, flags);
	if (cl->fwd2_pending_received_msgs ||
		!(send_ioctx = nvmeibs_net_get_ioctx(cl->net))) {
		list_add_tail(&recv_ioctx->free_tx_n, &cl->net->pending_received_msgs);
		cl->fwd2_pending_received_msgs = true;
		rv = 0;
	}
	else {
		/* we can avoid kzalloc-and-add work after unlock spinlock (because
		   pending is empty i.e. no other ctx can add-work and cause re-order)
		   but irqs are disabled anyhow && this is ctrl channel */
		rv = cl_recv_msg_work_add_(cl, recv_ioctx, send_ioctx);
	}
	spin_unlock_irqrestore(&cl->spinlock, flags);

out:
	return rv;
}

static struct nvmeib_iu *get_recv_iu(struct nvmeibs_client *cl, u32 index)
{
	if (N2SI(cl->net))
		return nvmeib_srq_rtrv_recv(N2SI(cl->net), index, cl->net);
	else if (cl->recv_q)
		return nvmeib_rq_rtrv_recv(cl->recv_q, index);
	else
		return NULL;
}

static int post_recv_iu(struct nvmeibs_client *cl, struct nvmeib_iu *iu)
{
	int rv;

#if !NVMESH_IS_PRODUCTION_COMPILATION
	BUG_ON(iu->recv_idx);
#endif

	if (N2SI(cl->net))
		rv = nvmeib_srq_post_recv(N2SI(cl->net), iu);
	else if (cl->recv_q)
		rv = nvmeibs_net_post_recvq(cl->net, cl->recv_q, iu);
	else {
		_NE(error_client_post_recv_iu, "No SRQ or RQ available");
		rv = -EINVAL;
	}

	return rv;
}

static void process_rcv_completion(struct nvmeibs_client *cl, struct ib_wc *wc)
{
	struct nvmeib_iu *recv_ioctx;
	u32 index;

	index = nvmeib_idx_from_wc(wc);
	if (!(recv_ioctx = get_recv_iu(cl, index))) {
		_NE(error_client_process_rcv_completion, "Got null recv_ioctx");
		return;
	}

	if (wc->status == IB_WC_SUCCESS)
		handle_new_msg(cl, recv_ioctx);
	else {
		/* return the received context to the shared received queue */
		if (wc->status != IB_WC_WR_FLUSH_ERR || !atomic_read(&cl->net->dying))
			_NT(trace_client_process_rcv_completion, "receiving failed for idx @INDEX with status @STATUS", index, wc->status);
		post_recv_iu(cl, recv_ioctx);
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_RCV_COMPLETION_FAILED);
	}
}

static void send_resource(struct nvmeibs_client *cl,
						  struct nvmeibs_cmd_info *info,
						  enum vex_ext_enum vex_ext, struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req;
	int rv;

	__NFIN;
	req = send_ioctx->buf;
	req->hdr.opcode = NVMEIB_CMD;
	req->hdr.tag = (u64)info;
	req->version_tag = vex_ext;
	req->opcode = info->op;
	_NT(trace_0_send_resource,
		"@CL (@CL_NAME), wire_cmd: add and send info=@PTR, op=@INT, has_rsp=@BOOL",
		cl, cl->name, info, info->op, info->has_rsp);
	list_add_tail(&info->link, &cl->wire_cmd);
	/* JH IOMMU: Correct. syncs data from cpu to device as source for Local RDMA_SEND */
	ib_dma_sync_single_for_device(P2IB(cl->ib_port), send_ioctx->dma,
		cl->net->params.s_msg_size, DMA_TO_DEVICE);
	if ((rv = nvmeibs_client_send_msg(cl, cl->net, send_ioctx,
		cl->net->params.s_msg_size, NVMEIB_SEND_CFG, NON_NR_VERSION)) < 0 || !info->has_rsp) {
		_NT(trace_1_send_resource,
			"@CL (@CL_NAME), wire_cmd: del info=@PTR", cl, cl->name, info);
		list_del(&info->link);
		if (info->op == NVMEIBS_PUT_RSC) {
			kfree(get_put_info(info));
		} else if (info->op == NVMEIBS_GET_RSC) {
			kfree(get_get_info(info));
		} else if (info->op == NVMEIBS_JAM_ABND2FREE) {
			kfree(get_abnd_free_info(info));
		} else if (info->op == NVMEIBS_RGID_CHANGE) {
			kfree(get_gid_change_info(info));
		} else if (info->op == NVMEIBS_IOCH_DRAINED) {
			kfree(get_ioch_drained_info(info));
		} else {
			kfree(info);
		}

		/* Only put if failed to send, otherwise the send-comp handler will do the put */
		if (rv < 0) {
			_NT(trace_client_send_resource, "Send request to client @CL_NAME failed (@RV)", cl->name, rv);
			/* No need to complete(send_ioctx->done) as caller
			always resumes, if at all, from rsp (recv-comp) */
			nvmeibs_client_put_ioctx(cl, send_ioctx);
		}
	}
	__NFOUT;
}

static void send_put_resource(struct nvmeibs_client *cl,
	struct nvmeibs_put_cmd *cmd, struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req;

	__NFIN;
	req = send_ioctx->buf;
	memcpy(req->p_req.g_req.disk_name, cmd->cmd.disk_name,
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	req->p_req.g_req.n = cpu_to_be64(cmd->n_ids);
	memcpy(req->p_req.ids, cmd->ids, sizeof(cmd->ids));
	send_resource(cl, &cmd->cmd.info, vex_base, send_ioctx);
	__NFOUT;
}

static void send_put_resource_work(struct workqe_struct *work)
{
	struct get_put_rsc_workq *w =
		container_of(work, struct get_put_rsc_workq, work);
	struct nvmeibs_put_cmd *cmd = get_put_info(&w->cmd->info);
	struct nvmeibs_client *cl = w->cl;
	int dying = atomic_read(&cl->net->dying);
	struct nvmeib_iu *send_ioctx;

	__NFIN;
	if (!dying) {
		if ((send_ioctx = nvmeibs_client_get_ioctx(cl)))
			send_put_resource(cl, cmd, send_ioctx);
		else {
			unsigned long flags;

			spin_lock_irqsave(&cl->spinlock, flags);
			list_add_tail(&cmd->cmd.info.link, &cl->pending_cmd);
			spin_unlock_irqrestore(&cl->spinlock, flags);
		}
	} else {
		kfree(cmd);
	}
	kfree(w);
	__NFOUT;
}

static void send_get_resource(struct nvmeibs_client *cl,
	struct nvmeibs_get_cmd *cmd, struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req;

	__NFIN;
	req = send_ioctx->buf;
	memcpy(req->g_req.disk_name, cmd->cmd.disk_name,
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	req->g_req.n = cpu_to_be64(cmd->n);
	send_resource(cl, &cmd->cmd.info, vex_base, send_ioctx);
	__NFOUT;
}

static void send_get_resource_work(struct workqe_struct *work)
{
	struct get_put_rsc_workq *w =
		container_of(work, struct get_put_rsc_workq, work);
	struct nvmeibs_get_cmd *cmd = get_get_info(&w->cmd->info);
	struct nvmeibs_client *cl = w->cl;
	int dying = atomic_read(&cl->net->dying);
	struct nvmeib_iu *send_ioctx;

	__NFIN;
	if (!dying) {
		if ((send_ioctx = nvmeibs_client_get_ioctx(cl)))
			send_get_resource(cl, cmd, send_ioctx);
		else {
			unsigned long flags;

			spin_lock_irqsave(&cl->spinlock, flags);
			list_add_tail(&cmd->cmd.info.link, &cl->pending_cmd);
			spin_unlock_irqrestore(&cl->spinlock, flags);
		}
	} else {
		kfree(cmd);
	}
	kfree(w);
	__NFOUT;
}

static bool is_resource_cmd(struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req = send_ioctx->buf;

	return (req->hdr.opcode == NVMEIB_CMD) &&
		(req->opcode == NVMEIBS_PUT_RSC || req->opcode == NVMEIBS_GET_RSC);
}


static void process_send_completion_no_iu(struct nvmeibs_client *cl,
				enum ib_wc_status wc_status, u32 opcode, u32 index)
{
	if (wc_status != IB_WC_SUCCESS) {
		if (wc_status != IB_WC_WR_FLUSH_ERR || !atomic_read(&cl->net->dying)) {
			_NT(trace_client_process_send_completion_no_iu, "Sending opcode @NVMEIB_WR_OPCODE_STR (@OPCODE) for idx @INDEX was failed with status @WC_STATUS",
				nvmeib_wr_opcode_str(opcode), opcode, index, wc_status);
		}
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_SND_COMPLETION_FAILED);
	}
}

static void process_send_completion_put_iu(struct nvmeibs_client *cl,
				enum ib_wc_status wc_status, u32 opcode, u32 index,
				struct nvmeib_iu *send_ioctx)
{
	bool leave = false;

	__NFIN;

	_ND(trace_client_process_send_completion_put_iu, "send completion of @NVMEIB_WR_OPCODE_STR", nvmeib_wr_opcode_str(opcode));

	if (wc_status == IB_WC_SUCCESS) {
		if (opcode == NVMEIB_SEND_CFG)
			_ND(trace_1_client_process_send_completion_put_iu, "sending response (controller request) for idx @INDEX was OK",
				index);
		else
			_ND(trace_2_client_process_send_completion_put_iu, "sending opcode @OPCODE for idx @INDEX was OK", opcode, index);
	}
	else {
		if (opcode == NVMEIB_SEND_CFG) {
			_NT(trace_3_client_process_send_completion_put_iu, "Sending response (controller request) for idx @INDEX failed "
				"with status @WC_STATUS", index, wc_status);
			leave = true;
		}
		else {
			_ND(trace_4_client_process_send_completion_put_iu, "sending opcode @OPCODE for idx @INDEX was failed with status @WC_STATUS",
				opcode, index, wc_status);
			leave = !is_resource_cmd(send_ioctx);
		}
	}

	/* return send context to free_msg list */
	nvmeibs_net_put_ioctx(cl->net, send_ioctx);
	if (leave){
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_SND_COMPLETION_FAILED);
	}

	__NFOUT;
}

static void send_abnd_free(struct nvmeibs_client *cl,
	struct nvmeibs_abnd_free_cmd *cmd, struct nvmeib_iu *send_ioctx);
static void send_gid_change(struct nvmeibs_client *cl,
	struct nvmeibs_gid_change_cmd *cmd, struct nvmeib_iu *send_ioctx);
static void send_ioch_drained(struct nvmeibs_client *cl,
	struct nvmeibs_ioch_drained_cmd *cmd, struct nvmeib_iu *send_ioctx);

static void attempt_handle_pending_send_recv(struct nvmeibs_client *cl)
{
	struct nvmeib_iu *send_ioctx;
	struct nvmeib_iu *recv_ioctx;
	struct nvmeibs_cmd_info *info = NULL;
	unsigned long flags;

	__NFIN;

	/* try to handle pending received messages */
	spin_lock_irqsave(&cl->spinlock, flags);
	while (unlikely(
			(!list_empty(&cl->net->pending_received_msgs) ||
			 !list_empty(&cl->pending_cmd)) &&
			cl->net->state == QP_LIVE &&
			(send_ioctx = nvmeibs_net_get_ioctx(cl->net)) != NULL)) {
		if ((recv_ioctx = list_first_entry_or_null(
			&cl->net->pending_received_msgs, struct nvmeib_iu, free_tx_n)))
			list_del_init(&recv_ioctx->free_tx_n);
		else if ((info = list_first_entry_or_null(&cl->pending_cmd,
				struct nvmeibs_cmd_info, link)))
				list_del(&info->link);
		if (recv_ioctx) {
			cl_recv_msg_work_add_(cl, recv_ioctx, send_ioctx);
		}
		spin_unlock_irqrestore(&cl->spinlock, flags);
		if (info) {
			if (info->op == NVMEIBS_PUT_RSC)
				send_put_resource(cl, get_put_info(info), send_ioctx);
			else if (info->op == NVMEIBS_GET_RSC)
				send_get_resource(cl, get_get_info(info), send_ioctx);
			else if (info->op == NVMEIBS_JAM_ABND2FREE)
				send_abnd_free(cl, get_abnd_free_info(info), send_ioctx);
			else if (info->op == NVMEIBS_RGID_CHANGE)
				send_gid_change(cl, get_gid_change_info(info), send_ioctx);
			else if (info->op == NVMEIBS_IOCH_DRAINED)
				send_ioch_drained(cl, get_ioch_drained_info(info), send_ioctx);
			else {
				_NE(attempt_handle_pending_send_recv_e1, "Unknown controller command @INT", info->op);
				kfree(info);
			}
		}
		spin_lock_irqsave(&cl->spinlock, flags);
	}
	if (list_empty(&cl->net->pending_received_msgs)) {
		cl->fwd2_pending_received_msgs = false;
	}
	spin_unlock_irqrestore(&cl->spinlock, flags);

	__NFOUT;
}

/**
 * nvmeibs_process_send_completion() - Process an IB send
 * completion.
 *
 */
static void process_send_completion(struct nvmeibs_client *cl, struct ib_wc *wc)
{
	struct nvmeib_iu *send_ioctx;
	u32 index;
	u32 opcode;
	unsigned long flags;

	index = nvmeib_idx_from_wc(wc);
	opcode = nvmeib_opcode_from_wc(wc);

	if (opcode == NVMEIB_KEEP_ALIVE_REQ) {
		if (atomic_dec_return(&cl->ka_sent) < 0){
			//we got the completion after server disconnection
			atomic_set(&cl->ka_sent, 0);
		}
	}

	if (unlikely(opcode == NVMEIB_DRAIN_QUEUE)) {
		nvmeibs_net_on_drain_sq(cl->net);
		goto out;
	}

	if ((opcode != NVMEIB_SEND_CFG) &&
		(opcode != NVMEIB_TOMA_SEND_REQ) &&
		(opcode != NVMEIB_TOMA_SEND_RSP) &&
		(opcode != NVMEIB_RDMA_GET_JMDC_RSP) &&
		(opcode != NVMEIB_WR_DBG_CMD)) {
		/* rdma completion, no iu to release */
		process_send_completion_no_iu(cl, wc->status, opcode, index);
		if (opcode == NVMEIB_RDMA_GET_JMDC) {
			_NT(t0_client_process_send_completion,
				"cl=@CL, GET_JMDC index=@INT (comp=@PTR)",
				cl, index, cl->get_jmdc_comp);
			spin_lock_irqsave(&cl->get_jmdc_comp_lock, flags);
			if (cl->get_jmdc_comp)
				complete(cl->get_jmdc_comp);
			spin_unlock_irqrestore(&cl->get_jmdc_comp_lock, flags);
		}
		goto out;
	}

	/*we should not be here in case of keep alive.
	  keep alive have its own message buffer*/
	BUG_ON(opcode == NVMEIB_KEEP_ALIVE_REQ);

	BUG_ON(index >= cl->net->params.sendq_size);
	send_ioctx = cl->net->ioctx_ring[index];
	send_ioctx->io_status = wc->status;

#if IB_NEW_FR
	if (wc->opcode == IB_WC_REG_MR && opcode == NVMEIB_FAST_REG_WR_ID)
#else
	if (wc->opcode == IB_WC_FAST_REG_MR && opcode == NVMEIB_FAST_REG_WR_ID)
#endif
		handle_fmr_reg(send_ioctx);
	else if (wc->opcode == IB_WC_LOCAL_INV && opcode == NVMEIB_LOCAL_INV_WR_ID)
		handle_fmr_inv(send_ioctx);
	else if (send_ioctx->io_done)
		complete(send_ioctx->io_done);
	else {
		if (opcode == NVMEIB_TOMA_SEND_REQ) {
			_NT(trace_client_process_send_completion, "TOMA req send_ioctx->io_done = NULL, release net");
			nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_TOMA_SND_COMPLETION_FAILED);
			goto out;
		}
		else if (opcode == NVMEIB_RDMA_GET_JMDC_RSP) {
			_NT(t1_client_process_send_completion,
			    "cl=@CL, GET_JMDC index=@INT (comp=@PTR, wc_opcode=@WC_OPCODE, wc_status=@WC_STATUS)",
			    cl, index, cl->get_jmdc_comp, wc->opcode, wc->status);
			spin_lock_irqsave(&cl->get_jmdc_comp_lock, flags);
			if (cl->get_jmdc_comp)
				complete(cl->get_jmdc_comp);
			spin_unlock_irqrestore(&cl->get_jmdc_comp_lock, flags);
		}

		process_send_completion_put_iu(cl,
			wc->status, opcode, index, send_ioctx);

		/* we've just returned iu to pool,
		   try to handle pending received messages */
		attempt_handle_pending_send_recv(cl);
	}

out:;
}

static void process_scq_completion_imp(
	struct ib_cq *cq, struct nvmeibs_client *cl, struct ib_wc *wcs)
{
	int n, i, rv;

	NFIN;

	BUG_ON(nvmeibs_use_pcpu_cq);

	while ((n = ib_poll_cq(cq, NVMEIBS_POLL_SIZE, wcs)) >= 0) {
		if (cl->dismissed)
			_NT(trace_client_process_scq_completion_imp, "Allow processing of drain-sq wr");
		for (i = 0; i < n; ++i)
			process_send_completion(cl, &wcs[i]);
		if ((rv = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP |
			IB_CQ_REPORT_MISSED_EVENTS)) <= 0) {
			if (rv < 0)
				_NE(error_client_process_scq_completion_imp, "ib_req_notify_cq failed (@RV) for cl @CL", rv, cl);
			break;
		}
	}
	if (n < 0) {
		_NT(error_1_client_process_scq_completion_imp, "cl @CL_NAME failed to poll - error code @CODE", cl->name, n);
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_SCQ_SND_COMPLETION_FAILED);
	}
	NFOUT;
}

static void process_rcq_completion_imp(
	struct ib_cq *cq, struct nvmeibs_client *cl, struct ib_wc *wcs)
{
	int i, n, rv;

	NFIN;

	BUG_ON(nvmeibs_use_pcpu_cq);

	while ((n = ib_poll_cq(cq, NVMEIBS_POLL_SIZE, wcs)) >= 0) {
		struct nvmeibs_net *net = cl->net;
		nvmeibs_net_dec_recv(net, n);
		for (i = 0; i < n; ++i) {
			if (nvmeib_opcode_from_wc(&wcs[i]) == NVMEIB_DRAIN_QUEUE) {
				if (i < n - 1) {
					_NW(warn_client_process_rcq_completion_imp, "cl @CL net @NET: Drain WR is not last recv completion", cl, cl->net);
					WARN_ON(1);
				}
				nvmeibs_rq_drain_comp(net, &wcs[i]);
			}
			else if (!cl->dismissed)
				process_rcv_completion(cl, &wcs[i]);
			else {
				u32 index = nvmeib_idx_from_wc(&wcs[i]);
				struct nvmeib_iu *recv_ioctx = get_recv_iu(cl, index);
				struct nvmeib_hdr *hdr = recv_ioctx->buf;
				if (is_logout_rsp(hdr, recv_ioctx)) {
						logout_rsp_recv_comp(cl);
					}
				if (recv_ioctx) {
					post_recv_iu(cl, recv_ioctx);
				}
				else
					_NE(error_client_process_rcq_completion_imp, "Got null recv_ioctx");
			}
		}
		if ((rv = ib_req_notify_cq(cq, IB_CQ_NEXT_COMP |
			IB_CQ_REPORT_MISSED_EVENTS)) <= 0) {
			if (rv < 0)
				_NE(error_1_client_prosess_rcq_completion_imp, "ib_req_notify_cq failed (@RV) for cl @CL", rv, cl);
			break;
		}
	}
	NFOUT;
}

static void process_scq_completion(struct ib_cq *cq, void *ctx)
{
	struct nvmeibs_client *cl = ctx;
	NFIN;
	process_scq_completion_imp(cq, cl, cl->scq_wcs);
	NFOUT;
}
static void process_rcq_completion(struct ib_cq *cq, void *ctx)
{
	struct nvmeibs_client *cl = ctx;
	NFIN;
	process_rcq_completion_imp(cq, cl, cl->rcq_wcs);
	if (cl->net && cl->net->scq)
		process_scq_completion_imp(cl->net->scq, cl, cl->rcq_wcs);
	else {
		_NT(trace_client_process_rcq_completion, "Oops: cl @CL, net @NET", cl, cl->net);
		WARN_ON(true);
	}
	NFOUT;
}

static void lock_channel_send_completion(struct ib_cq *cq, void *vcl)
{
	struct nvmeibs_client *cl = vcl;
	struct nvmeibs_net *net = cl->lock_net;
	DECLARE_IB_WC_ONSTACK(wc);

	NFIN;

	BUG_ON(nvmeibs_use_pcpu_cq);

	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	while (ib_poll_cq(cq, 1, &wc) > 0) {
		/* check if someone waits for the send completion */
		if (unlikely(nvmeib_opcode_from_wc(&wc) == NVMEIB_DRAIN_QUEUE)) {
			nvmeibs_net_on_drain_sq(net);
			break; //catch send after drain-sq
		} else
			_NW(warn_client_lock_channel_send_completion, "Unexpected wr_id @WR_ID_LLONG (opcode: @OPCODE, status: @STATUS) on client @CL lock channel",
			   wc.wr_id, wc.opcode, wc.status, cl);
	}
	NFOUT;
}


static void ka_disable(void *context)
{
	struct nvmeibs_client *cl = context;
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&cl->ka_spinlock, flags);
	_NT(trace_client_ka_disable, "cl @CL, net @NET: Disable keep-alive (curr state @KA_STATE)",
	   cl, cl->net, cl->ka_state);
	cl->ka_state = KA_DISABLED;
	spin_unlock_irqrestore(&cl->ka_spinlock, flags);

	NFOUT;
}

static void ka_enable(struct nvmeibs_client *cl)
{
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&cl->ka_spinlock, flags);
	if (cl->ka_state == KA_INIT) {
		_NT(trace_client_ka_enable, "cl @CL, net @NET: Enable keep-alive (curr state @KA_STATE)",
		   cl, cl->net, cl->ka_state);
		cl->ka_state = KA_ENABLED;
	}
	else
		_NT(trace_1_client_ka_enable, "cl @CL, net @NET: cannot Enable keep-alive, curr state @KA_STATE",
		   cl, cl->net, cl->ka_state);
	spin_unlock_irqrestore(&cl->ka_spinlock, flags);

	NFOUT;
}

static void ready_to_send(void *context)
{
	struct nvmeibs_client *cl = context;
	__NFIN;
	_NT(trace_client_ready_to_send, "Handle ready to send context (cl) @CL", cl);

	/* enable keep-alive here to avoid recv-comp on
	   client side before it completes connect-qp */
	if (cl->ka_state != KA_NOT_USED)
		ka_enable(cl);

	attempt_handle_pending_send_recv(cl);

	__NFOUT;
}

static int disconnect_rionic_iochs(struct nvmeibs_rionic *rionic);
static void set_rionic_ka_dying(struct nvmeibs_rionic *rionic, bool dying);
static void disconnect_lionic(struct nvmeibs_lionic *lionic)
{
	int i;
	NFIN;

	if (lionic->rionics) {
		for (i = 0; i < lionic->n_rionics; ++i) {
			set_rionic_ka_dying(&lionic->rionics[i], true);
			disconnect_rionic_iochs(&lionic->rionics[i]);
		}
	}

	NFOUT;
}

static void stop_io_channels_on_lgid(struct nvmeibs_client *cl,
									 union ib_gid *lgid)
{
	struct nvmeibs_client_disk *cdisk;
	int i;
	__NFIN;

	list_for_each_entry(cdisk, &cl->disks, link) {
		if (cdisk->lionics) {
			for (i = 0; i < cdisk->n_lionics; ++i) {
				if (!memcmp(lgid, &cdisk->lionics[i].gid, sizeof(*lgid))) {
					_NT(stop_io_channels_on_lgid_t1, "cl @STR, found lionic w/ lgid @GID", cl->name, lgid);
					if (cdisk->lionics[i].rionics)
						disconnect_lionic(&cdisk->lionics[i]);
					break;
				}
			}
		}
	}

	__NFOUT;
}

static void stop_io_channels(struct nvmeibs_client *cl)
{
	struct nvmeibs_client_disk *cdisk;
	int i;
	__NFIN;

	list_for_each_entry(cdisk, &cl->disks, link) {
		if (cdisk->lionics) {
			for (i = 0; i < cdisk->n_lionics; ++i) {
				if (cdisk->lionics[i].rionics)
					disconnect_lionic(&cdisk->lionics[i]);
			}
		}
	}

	__NFOUT;
}

static void stop_lock_channel(struct nvmeibs_client *cl)
{
	__NFIN;

	_NT(trace_client_stop_lock_channel, "CID: @CID - Add release-work for lock channel (@LOCK_NET)", cl->cid, cl->lock_net);
	if (cl->lock_net) {
		nvmeibs_net_release(cl->lock_net, NVMEIBS_LOGOUT_REASON_LOCK_CH_STOP);
	}

	__NFOUT;
}

static void stop_main_channel(struct nvmeibs_client *cl, enum nvmeibs_logout_reason reason)
{
	__NFIN;

	_NT(trace_client_stop_main_channel, "CID: @CID - Add release-work for main (admin) channel (@NET)", cl->cid, cl->net);
	if (cl->net) {
		nvmeibs_net_release(cl->net, reason);
	}

	__NFOUT;
}

/* this one is called when the client peer disconnect */
static void start_release_work(void *context)
{
	struct nvmeibs_client *cl = context;

	__NFIN;
	_NT(trace_client_start_release_work, "Will call free client @CL @CID", cl, cl->cid);
	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid,
	 NVMEIBS_LOGOUT_REASON_CLIENT_DISCONNECT);
	__NFOUT;
}

void nvmeibs_client_keep_alive(struct nvmeibs_client *cl)
{
	struct nvmeib_send_wr wr = {};
	IB_DECLARE_BAD_SEND_WR(bad_wr);
	unsigned long flags;
	struct ib_sge sg_list[2], *sge = sg_list;
	unsigned long t;
	bool release = false;
	int rv;

	/* Expired cookies, doning on the way, no need for spin lock*/
	nvmeibs_async_cookie_store_bail_expired_store(&cl->cookie_store);

	spin_lock_irqsave(&cl->ka_spinlock, flags);
	if (cl->ka_state != KA_ENABLED) {
		cl->ka_timeout = jiffies + NVMEIB_KEEP_ALIVE_TO;
		goto out;
	}

	t = jiffies;
	//check that the last keep alive was answered in time
	if (cl->ka_timeout == 0) {
		cl->ka_timeout = jiffies + NVMEIB_KEEP_ALIVE_TO;
		t = jiffies;
	}

	if (time_after(t, cl->ka_timeout)) {
		_NT(trace_client_nvmeibs_client_keep_alive, "Keep alive was triggered for client @CL_NAME - now @TOPOLOGY_LONG,"
		   " timout tick @KA_TIMEOUT la_id = @KA_ID",
			cl->name, t, cl->ka_timeout, cl->ka_id);
		release = true;
		goto out;
	}
	else if(atomic_read(&cl->ka_sent) == 0){
		++cl->ka_id;
		/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is source for local RDMA_SEND */
		ib_dma_sync_single_for_cpu(P2IB(cl->ib_port), cl->ka_msg_dma_addr,
			sizeof(*cl->ka_msg_area), DMA_TO_DEVICE);
		cl->ka_msg_area->opcode = NVMEIB_KEEP_ALIVE;
		cl->ka_msg_area->tag = cl->ka_id;

		if (cl->net->msg_hdr) {
			sge->addr = cl->net->msg_hdr_dma_addr;
			sge->length = sizeof(*cl->net->msg_hdr);
			sge->lkey = nvmeib_get_lkey(P2NV(cl->ib_port));
			sge++;
		}
		sge->addr = cl->ka_msg_dma_addr;
		sge->length = sizeof(*cl->ka_msg_area);
		sge->lkey = nvmeib_get_lkey(P2NV(cl->ib_port));
		/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is source for local RDMA_SEND */
		ib_dma_sync_single_for_device(P2IB(cl->ib_port), cl->ka_msg_dma_addr,
			sizeof(*cl->ka_msg_area), DMA_TO_DEVICE);

		nvmeib_send_wr_common(wr).opcode = IB_WR_SEND;
		nvmeib_send_wr_common(wr).wr_id = nvmeib_encode_wr_id(NVMEIB_KEEP_ALIVE_REQ, (u16)(cl->ka_id));
		nvmeib_send_wr_common(wr).send_flags = IB_SEND_SIGNALED;
		nvmeib_send_wr_common(wr).sg_list = sg_list;
		nvmeib_send_wr_common(wr).num_sge = 1 + (sge - sg_list);
		nvmeib_send_wr_clear_next(wr);

#if ENABLE_SIW
		if (P2NV(cl->net->params.port)->dev_type == DT_siw)
			nvmeib_send_wr_common(wr).send_flags |= SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT;
#endif

		if (cl && cl->net && cl->net->qp) {
			atomic_inc(&cl->ka_sent);
			if ((rv = nvmeibs_ib_post_send(cl->net, nvmeib_send_wr_to_ib_ptr(wr), &bad_wr)) < 0) {
				_NT(trace_1_client_nvmeibs_client_keep_alive, "Failed to send keep_alive @RV", rv);
				atomic_dec(&cl->ka_sent);
				release = true;
				goto out;
			}
		}

		/* Check keep alive on all io paths */
		nvmeibs_client_check_all_io_paths_ka(cl);
	}

out:
	spin_unlock_irqrestore(&cl->ka_spinlock, flags);
	if (release) {
		cl->ka_timeout = 0;
		atomic_set(&cl->ka_sent,0);
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_ADMIN_CH_KA_SND_FAILED);
	}
}


#if 0 /* DEBUG ONLY */
void nvmeibs_client_check_trigger(struct nvmeibs_client *cl)
{
	void *ioch = cl->_ioch_;
#if 0
	struct nvmeib_dbg_mlx mm = {0};
	struct ib_send_wr wr[2] = {{0}};
	struct ib_send_wr *bad_wr;
	struct ib_sge sg[2] = {{0}};
#endif

	__NFIN;
	if (ioch) {
		_NI(nvmeibs_client_check_trigger_i1, "Send Q buffer");
#if 0
		nvmeib_dump_page(ioch->qp_rsc.sq.vaddr);
		nvmeib_ibdr_parse_qp_sq(ioch->net->params->port->nis_dev->dev->ib_dev, ioch->net->qp, &mm);
		nvmeib_dump_page(ioch->qp_rsc.sq.vaddr);
		_NI(nvmeibs_client_check_trigger_i2, "db_payload=@INT32_HEX", ioch->qp_rsc.sq.doorbell_payload);
		sg[0].addr = mm.laddr1;
		sg[0].lkey = mm.lkey1;
		sg[0].length = mm.lsize1;

		wr[0].opcode = IB_WR_RDMA_WRITE;
		wr[0].wr.rdma.remote_addr = mm.raddr1;
		wr[0].wr.rdma.rkey = mm.rkey1;
		wr[0].num_sge = 1;
		wr[0].sg_list = &sg[0];
		wr[0].next = &wr[1];
		sg[0].addr = mm.laddr1;
		sg[0].lkey = mm.lkey1;
		sg[0].length = mm.lsize1;

		sg[1].addr = mm.laddr2;
		sg[1].lkey = mm.lkey2;
		sg[1].length = mm.lsize2;
		wr[1].opcode = IB_WR_RDMA_WRITE_WITH_IMM;
		wr[1].wr.rdma.remote_addr = mm.raddr2;
		wr[1].wr.rdma.rkey = mm.rkey2;
		wr[1].num_sge = 1;
		wr[1].sg_list = &sg[1];
		wr[1].next = NULL;

		BUG_ON(ib_post_send(ioch->net->qp, wr, &bad_wr) < 0);
		nvmeib_dump_page(ioch->qp_rsc.sq.vaddr);
		_NI(nvmeibs_client_check_trigger_i3, "db_payload=@INT32_HEX", ioch->qp_rsc.sq.doorbell_payload);
#else
		_NI(nvmeibs_client_check_trigger_i4, "DB descriptor=@INT",
			be32_to_cpu(*(u32 *)ioch->qp_rsc.sq.doorbell_descr_address_virt));
		_NI(nvmeibs_client_check_trigger_i5, "db_payload=@INT32_HEX, db_addr=@_X",
			ioch->qp_rsc.sq.doorbell_payload,
			ioch->qp_rsc.sq.doorbell_address_virt);
		__raw_writel(ioch->qp_rsc.sq.doorbell_payload,
			(void *)ioch->qp_rsc.sq.doorbell_address_virt);
#endif
	}
	__NFOUT;
}
#endif

static void nvmeibs_set_link_ops(struct nvmeibs_client *cl)
{
	union nvmeib_version link_version = cl->link_version;
	vex_link_ops(link_version, vex_ach_srv_ops_collection, cl->vex_ach_ops, vex_ach_names, vex_ach_ops_num);
	cl->vex_io_req_ops = vex_select_ops(link_version, vex_nrch_io_req_srv_ops);
	cl->vex_io_rsp_ops = vex_select_ops(link_version, vex_nrch_io_rsp_srv_ops);
}

static void nvmeibs_set_link_version(struct nvmeibs_client *cl)
{
	union nvmeib_version s_version = nvmeib_version_get();

	_NT(trace_client_nvmeibs_set_link_version, "client version: " NVMEIB_VERSION_TRACE_FMT()
	   " server version  " NVMEIB_VERSION_TRACE_FMT() "",
	   NVMEIB_VERSION_PRINT_ARG(&cl->c_version),
	   NVMEIB_VERSION_PRINT_ARG(&s_version));

	if (cl->c_version.protocol.all <= s_version.protocol.all) {
		cl->link_version.all = cl->c_version.all;
		_NT(trace_1_client_nvmeibs_set_link_version, "link version: client");
	} else {
		cl->link_version.all = s_version.all;
		_NT(trace_2_client_nvmeibs_set_link_version, "link version: server");
	}
}

static inline size_t calc_msg_area_sz(struct nvmeibs_client *cl)
{
	return (sizeof(*cl->in_msg_area_map.pages) * NVMEIBS_CONFIG_MSG_RDMA_PAGES) + (NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT) +
		(sizeof(*cl->out_msg_area_map.pages) * NVMEIBS_CONFIG_MSG_RDMA_PAGES) + (NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
}

int nvmeibs_client_allocate(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client **new_cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej)
{
	struct nvmeibs_client *cl = NULL;
	int rv = 0, i;
	size_t alloc_size = 0;

	NFIN;
	_NT(trace_client_nvmeibs_client_allocate,
	    "Allocate nvmeibs_client obj for cid = @CID",
		nvmeib_wire_op_cid_get_cid(&req->op_cid));
	cl = kzalloc(sizeof(*cl), GFP_KERNEL);
	if (cl) {
		cl->wq = wq_create(proc_name_format("S", "WQ", "client"));
		cl->remove_wq = wq_create_verbose(proc_name_format("S", "WQ", "cl_rm"));
	}
	if (!(cl && /* rsp && params && */ cl->wq && cl->remove_wq)) {
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		rv = -ENOMEM;
		_NE(error_client_nvmeibs_client_allocate, "rejected NVMEIB_LOGIN_REQ because no memory.");
		goto free_client;
	}
	_NI(trace_1_client_nvmeibs_client_allocate, "Allocated initial resources for cid @CID : "
	   "cl=@CL, wq-pid=@PID, remove-wq-pid=@PID",
	   nvmeib_wire_op_cid_get_cid(&req->op_cid), cl, wq_pid(cl->wq), wq_pid(cl->remove_wq));

	nvmeibc_login_req_get_ach(req, &cl->c_version.all, NULL, NULL);
	nvmeibs_set_link_version(cl);
	nvmeibs_set_link_ops(cl);
	cl->is_local = nvmeibc_login_req_get_local(req);

	/* allocate client rdma message */
	atomic_set(&cl->msg_area_refcount.dying, 1); /* Just a precaution */

	if (!(cl->in_msg_area = alloc_pages_exact(NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT, GFP_KERNEL)) ||
		!(cl->out_msg_area = alloc_pages_exact(NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT, GFP_KERNEL)) ||
		!(cl->in_msg_area_map.pages = kzalloc(sizeof(*cl->in_msg_area_map.pages) * NVMEIBS_CONFIG_MSG_RDMA_PAGES, GFP_KERNEL)) ||
		!(cl->out_msg_area_map.pages = kzalloc(sizeof(*cl->out_msg_area_map.pages) * NVMEIBS_CONFIG_MSG_RDMA_PAGES, GFP_KERNEL)))
	{
		_NE(error_1_client_nvmeibs_client_allocate, "Fail to allocate client message area for rdma");
		rv = -ENOMEM;
		nvmesh_memmgr_metric_on_alloc_update(s_clients_msg_area, alloc_size, false /* success */);
		goto free_msg_area;
	}

	alloc_size = calc_msg_area_sz(cl);
	nvmesh_memmgr_metric_on_alloc_update(s_clients_msg_area, alloc_size, true /* success */);

	cl->in_msg_area_end = cl->in_msg_area + (NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
	cl->out_msg_area_end = cl->out_msg_area + (NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);

	for (i = 0; i < NVMEIBS_CONFIG_MSG_RDMA_PAGES; i++) {
		cl->in_msg_area_map.pages[i] = virt_to_page(cl->in_msg_area + (i << PAGE_SHIFT));
		cl->out_msg_area_map.pages[i] = virt_to_page(cl->out_msg_area + (i << PAGE_SHIFT));
	}
	
	cl->in_msg_area_map.pd = P2NV(ib_port)->pd;
	cl->in_msg_area_map.n_pages = NVMEIBS_CONFIG_MSG_RDMA_PAGES;
	cl->in_msg_area_map.access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE;
	cl->in_msg_area_map.dma_dir = DMA_FROM_DEVICE;
	
	if ((rv = nvmeib_mem_alloc_n_map(&cl->in_msg_area_map))) {
		_NE(error_9_client_nvmeibs_client_allocate, "nvmeib_alloc_n_map failed (@RV) for in msg area", rv);
		goto free_msg_area;
	}
	
	cl->out_msg_area_map.pd = P2NV(ib_port)->pd;
	cl->out_msg_area_map.n_pages = NVMEIBS_CONFIG_MSG_RDMA_PAGES;
	cl->out_msg_area_map.access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE;
	cl->out_msg_area_map.dma_dir = DMA_TO_DEVICE;
	
	if ((rv = nvmeib_mem_alloc_n_map(&cl->out_msg_area_map))) {
		_NE(error_12_client_nvmeibs_client_allocate, "nvmeib_alloc_n_map failed (@RV) for out msg area", rv);
		goto unmap_msg_area;
	}

	/*allocate keep alive message area*/
	/* allocate client rdma message */
	if (!(cl->ka_msg_area =
		  kzalloc(sizeof(*cl->ka_msg_area), GFP_KERNEL))) {
		_NE(error_3_client_nvmeibs_client_allocate, "Fail to allocate client keep alive area for rdma");
		rv = -ENOMEM;
		goto unmap_msg_area;
	}
	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is source for local RDMA_SEND */
	cl->ka_msg_dma_addr = ib_dma_map_single(P2IB(ib_port), cl->ka_msg_area,
		sizeof(*cl->ka_msg_area), DMA_TO_DEVICE);
	if (ib_dma_mapping_error(P2IB(ib_port), cl->ka_msg_dma_addr)) {
		_NE(error_4_client_nvmeibs_client_allocate, "Fail to DMA map client keep alive area");
		rv = -ENOMEM;
		goto free_ka_msg_area;
	}

	BUG_ON(atomic_read(&cl->ka_sent) != 0);

	/* init client */
	nvmeibc_login_req_get_ach(req, NULL, NULL, &cl->client_uuid);
	cl->host_name[0] = '?';
	spin_lock_init(&cl->spinlock);
	spin_lock_init(&cl->ka_spinlock);
	cl->ib_port = ib_port;
	cl->cid = nvmeib_wire_op_cid_get_cid(&req->op_cid);
	cl->is_local = nvmeibc_login_req_get_local(req);

	/* we need at least twice the number of messages since anything
	   tha ca be SG entry here is a unique WQE
	*/
	cl->n_msgs = 2 * NVMEIBS_MAX_IO_CHANNEL_MSGS;
	cl->ka_state = KA_INIT;
	cl->ka_timeout = 0;
	cl->ka_id = 0;
	cl->nrch_ioreq_num = nvmeibs_get_nordda_io_req_num();
	cl->max_wrs_per_req = nvmeibs_get_nordda_max_wrs_per_req();
	INIT_LIST_HEAD(&cl->anics);
	INIT_LIST_HEAD(&cl->disks);
	INIT_LIST_HEAD(&cl->lnics);
	INIT_LIST_HEAD(&cl->pending_cmd);
	INIT_LIST_HEAD(&cl->wire_cmd);
	INIT_LIST_HEAD(&cl->ldisk_link);
	init_completion(&cl->ldisk_done);
	cl->jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	cl->fwd2_pending_received_msgs = true;
	cl->recv_idx_head = 1;
	cl->recv_idx_tail = 1;
	nvmeibs_async_cookie_store_init(&cl->cookie_store);
	spin_lock_init(&cl->get_jmdc_comp_lock);
	spin_lock_init(&cl->clnt_toma_rsp.comp_lock);
	while (cl->clnt_toma_rsp.tag_cntr == 0)
		get_random_bytes(&cl->clnt_toma_rsp.tag_cntr, sizeof(cl->clnt_toma_rsp.tag_cntr));
	cl->max_nrchs_per_path_rdma = cl->c_version.protocol.all > nvmeib_13_protocol_version.all ?
					NVMEIB_NR_GET_MAX_CHANNELS_PER_PATH(nvmeibs_nr_max_channels_per_path) : NVMEIB_COMPAT_MAX_NR_CHANNELS_PER_PATH;
	cl->max_nrchs_per_path_tcp = nvmeibs_nr_max_channels_per_path_tcp;

	/* add the client to client hash for the case that the client machine
	   tries to allocate lock or io channels before we register the client
	*/
	if (!nvmeibs_cdb_add(cl)) {
		_NE(error_5_client_nvmeibs_client_allocate, "Server is currently removing all clients. Probably TOMA is down");
		rv = -1;
		goto unmap_ka_msg_area;
	}

	if (nvmeib_ref_get(&cl->ib_port->n_port_conns) == 0) {
		_NE(error_8_client_nvmeibs_client_allocate,
			"Failed ref-get port - Hot remove to NIC and port-wq is being drained ?!");
		rv = -1;
		goto cdb_remove_cl;
	}

	*new_cl = cl;
	_ND(trace_2_client_nvmeibs_client_allocate,
	    "New connection request local = @LOCAL_INT",
		nvmeibc_login_req_get_local(req));
	goto out;

cdb_remove_cl:
	nvmeibs_cdb_del(cl, false);

unmap_ka_msg_area:
	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is source for local RDMA_SEND */
	ib_dma_unmap_single(P2IB(ib_port), cl->ka_msg_dma_addr,
		16, DMA_TO_DEVICE);

free_ka_msg_area:
	kfree(cl->ka_msg_area);

unmap_msg_area:
	if (cl->out_msg_area_map.mr)
		nvmeib_mem_unmapn_n_free(&cl->out_msg_area_map);
	if (cl->in_msg_area_map.mr)
		nvmeib_mem_unmapn_n_free(&cl->in_msg_area_map);

free_msg_area:
	kfree(cl->out_msg_area_map.pages);
	kfree(cl->in_msg_area_map.pages);
	
	if (cl->out_msg_area)
		free_pages_exact(cl->out_msg_area, NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
	if (cl->in_msg_area)
		free_pages_exact(cl->in_msg_area, NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
	nvmesh_memmgr_metric_on_free_update(s_clients_msg_area, alloc_size);

free_client:
	if (cl) {
		 if (cl->wq) {
			wq_destroy(cl->wq);
			cl->wq = NULL;
		 }
		 if (cl->remove_wq) {
			wq_destroy(cl->remove_wq);
			cl->remove_wq = NULL;
		 }
	}
	kfree(cl);

out:
	__NFOUT;
	return rv;
}

static void cl_ad_handler(void *context)
{
	struct nvmeibs_client *cl = context;
	__NFIN;

	_NT(trace_nordda_cl_ad_handler, "cl @CL", cl);
	nvmeibs_toma_send_work_iu_free(cl);
	__NFOUT;
}

#define num_or_maxu8(_num) min(_num, 255)

static void connect_admin_channel_work(struct workqe_struct *work)
{
	struct alloc_work *w = container_of(work, struct alloc_work, work);
	struct nvmeibs_ib_port *ib_port = w->port;
	struct nvmeib_rdma_cm *cm_id = w->cm_id;
	struct nvmeibs_client *cl = w->cl;
	struct nvmeibc_login_request *req = &w->req;
	struct nvmeibs_login_reject lrej = {{0}};
	struct nvmeibs_login_reject *rej = &lrej;
	struct nvmeibs_login_response *rsp = NULL;
	struct nvmeibs_net_init *params = NULL;
	struct nvmeibs_net_init_target target;
	struct nvmeib_dev *dev = P2NV(ib_port);
	struct nvmeib_srq_info *srq_info;
	enum nvmeibs_logout_reason reason;
	int cpus;

	__NFIN;

	rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!(rsp && params)) {
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_client_connect_admin_channel_work, "rejected NVMEIB_LOGIN_REQ because no memory.");
		reason = NVMEIBS_LOGOUT_REASON_ADMIN_REJECT_LOGIN_ALLOC_FAILED;
		goto reject;
	}

/* VEX TODO: init version connection params here */

	/* init connection params */
	snprintf(params->name, sizeof(params->name), "%s", "A");
	params->net_type = S_NET_ADMIN;
	params->cl = cl;
	WARN_ON(ib_port == NULL);
	params->port = ib_port;
	params->cm_id = cm_id;
	params->s_msg_size = NVMEIBS_MAX_ADMIN_MSG_SIZE;
	params->max_send_sge = NVMEIBS_SERVER_DEFAULT_MAX_SGES;
	params->sendq_size = NVMEIBS_SERVER_DEFAULT_SQ_SIZE;
	/* for each io request we also may have rdma request */
	params->sendq_size *= 2;
	params->scq_size = params->sendq_size;
	params->r_msg_size = ib_port->port_attrib.max_req_size;
	params->rcq_size = params->sendq_size;
	if (!nvmeibs_use_pcpu_cq) {
		srq_info = nvmeib_srq_info_get(dev, NVMEIB_SRQ_TYPE_PRIMARY); //TODO: Remove this ?
		params->recvq_size = srq_info != NULL ? 0 : NVMEIBS_ADMIN_RQ_SIZE;
		params->use_srq = srq_info != NULL;
		params->scq_handler = process_scq_completion;
		params->rcq_handler = process_rcq_completion;
	}
	else {
		params->use_srq = nvmeibs_support_srq(dev);
		params->recvq_size = params->use_srq ? 0 : NVMEIBS_ADMIN_RQ_SIZE;
		params->send_comp_h = cl_send_comp_h;
		params->recv_comp_h = cl_recv_comp_h;
	}
	params->srq_priv = NULL;
	params->srq_type = NVMEIB_SRQ_TYPE_PRIMARY;
	params->cm_handler = NULL;
	params->scq_context = cl;
	params->rcq_context = cl;
	params->rts_handler = ready_to_send;
	params->rts_context = cl;
	params->rw_handler = start_release_work;
	params->rw_context = cl;
	params->bn_handler = NULL;
	params->bn_context = NULL;
	params->ad_handler = cl_ad_handler;
	params->ad_context = cl;
	params->ka_handler = ka_disable;
	params->ka_context = cl;

	if (!params->use_srq) {
		if (!(cl->recv_q = kzalloc(sizeof(*cl->recv_q), GFP_KERNEL))) {
			_NE(error_1_client_connect_admin_channel_work, "Memory allocation error for receive queue");
			reason = NVMEIBS_LOGOUT_REASON_ADMIN_REJECT_LOGIN_ALLOC_FAILED;
			goto reject;
		}
		if (nvmeib_init_recvq(cl->recv_q, dev,
			params->recvq_size, params->r_msg_size)) {
			_NE(error_2_client_connect_admin_channel_work, "Error initializing receive queue");
			kfree(cl->recv_q);
			reason = NVMEIBS_LOGOUT_REASON_ADMIN_REJECT_RCQ_INIT_FAILED;
			goto reject;
		}
		params->recv_q = cl->recv_q;
		params->max_recv_sge = 1;
	}

	/* create nvmeib_login_response */
	nvmeib_wire_op_cid_set_rsp(&rsp->base.op_cid, NVMEIB_LOGIN_RSP, cl->cid);
	rsp->base.opcode = NVMEIBS_ADMIN_CHANNEL;
	snprintf(rsp->base.creq.host_name, NVMEIB_HOST_NAME_LEN, "%s",
		nvmeibs_node_name);
	rsp->base.creq.s_link_version = cpu_to_be64(cl->link_version.all);
	rsp->base.creq.max_iu_len = nvmeibc_login_req_get_local(req) ? 0 : cpu_to_be32(params->s_msg_size);
	rsp->base.creq.msg_buffer_pages = cpu_to_be32(NVMEIBS_CONFIG_MSG_RDMA_PAGES);
	rsp->base.creq.msg_buffer_raddr = cpu_to_be64(cl->in_msg_area_map.ioaddr);
	rsp->base.creq.msg_buffer_rkey = cpu_to_be32(cl->in_msg_area_map.rkey);

	rsp->ext1.n_ext = 6;
	rsp->ext1.ach.s_version = cpu_to_be64(nvmeib_version_get().all);

	cpus = num_active_cpus();
	/* trying to be safe for older clients */
	rsp->ext2.ach.tgt_num_cpus = num_or_maxu8(cpus);
	rsp->ext2.ach.tgt_max_nrchs_per_path = num_or_maxu8(cl->max_nrchs_per_path_rdma);
	rsp->ext3.ach.tgt_max_nrchs_per_path_tcp = num_or_maxu8(cl->max_nrchs_per_path_tcp);

	rsp->ext4.ach.tgt_num_cpus = cpu_to_be32(cpus);
	rsp->ext4.ach.tgt_max_nrchs_per_path = cpu_to_be32(cl->max_nrchs_per_path_rdma);
	rsp->ext4.ach.tgt_max_nrchs_per_path_tcp = cpu_to_be32(cl->max_nrchs_per_path_tcp);

	rsp->ext6.ach.tgt_pg_sz_shift = PAGE_SHIFT;

	/* set target */
	target.common = params;
	target.req = req;
	target.rsp = rsp;

	if (!nvmeibs_net_allocate_target(&cl->net, &target, rej)) {
		_NE(error_3_client_connect_admin_channel_work, "Fail to create target Admin QP");
		reason = NVMEIBS_LOGOUT_REASON_ADMIN_REJECT_TARGET_ADMIN_QP_CREATE_FAILED;
		goto reject;
	}

	if (nvmeibs_toma_send_work_iu_alloc(cl)) {
		_NE(error_4_client_connect_admin_channel_work, "Fail to alloc toma-send-work-iu");
		nvmeibs_net_release(cl->net, NVMEIBS_LOGOUT_REASON_TOMA_WORK_ALLOC_FAILED);
		goto out;
	}

	cl->net->priv = cl;
	goto out;

reject:
	/* here iff nvmeibs-net_allocate_target did NOT call net-release */
	if (cl->recv_q) {
		nvmeib_free_recvq(cl->recv_q);
		kfree(cl->recv_q);
		cl->recv_q = NULL;
	}

	if (rej->reason) {
		_NT(trace_client_connect_admin_channel_work, "Reject new connection");
		nvmeibs_send_login_reject(cm_id, rej, req);
	}
	/* we must remember to close the cm_id */
	nvmeib_rdma_destroy_cm(cm_id);

	_NT(trace_1_client_connect_admin_channel_work, "Will call free client @CL @CID", cl, cl->cid);
	nvmeibs_ib_port_free_client(cl->ib_port, cl->cid, reason);

	nvmeib_ref_put(&ib_port->n_port_conns);

out:
	kfree(params);
	kfree(rsp);
	kfree(w);

	__NFOUT;
}

static noinline void cl_lock_dev_put(struct nvmeibs_client *cl)
{
	struct list_head *disks;
	struct nvmeibs_client_disk *cdisk;

	NFIN;
	nvmeibs_disk_locks_guard();
	if (!cl->is_local) {
		disks = &cl->disks;
		list_for_each_entry(cdisk, disks, link) {
			BUG_ON(!list_is_last(&cdisk->link, disks));
			nvmeibs_disk_lock_dev_put_(cdisk->di, false, 1);
		}
	}
	else
		nvmeibs_disk_lock_dev_put_(cl->di, false, 1);
	cl->lock_validated = false;
	nvmeibs_disk_lock_unguard();
	NFOUT;
}

static void lock_client_end(void *context)
{
	struct nvmeibs_client *cl = context;
	NFIN;

	if (cl->atomic_test_zone_map.allocated) {
		nvmeib_mem_unmapn_n_free(&cl->atomic_test_zone_map);
		cl->atomic_test_zone_map.allocated = false;
	}

	cl_lock_dev_put(cl);

	NFOUT;
}

static void lock_ch_pre_rw(void *context)
{
	struct nvmeibs_client *cl = context;
	int i;
	NFIN;

	/* Ensure this is serialized with secondary-lock-ch connect work */
	if (!on_wq(cl->wq)) {
		_NW(warn_client_lock_ch_pre_rw, "Oops, unexpected ctx, exp wq @PID but pid=@PID",
		   wq_pid(cl->wq), current->pid);
		WARN_ON_ONCE(1);
	}
	else {

	/* Primary lock channel is going down. Disconnect any secondary lock nets */
	_NT(trace_client_lock_ch_pre_rw, "CID: @CID - Stopping secondary lock channels @CL_NAME", cl->cid, cl->name);
	for (i = 0 ; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
		if (cl->_2nd_lock_ch[i].net) {
			nvmeibs_net_release(cl->_2nd_lock_ch[i].net, NVMEIBS_LOGOUT_REASON_LOCK_CH_STOP);
		}
	}

	}

	NFOUT;
}

static void connect_lock_channel_work(struct workqe_struct *work)
{
	struct alloc_work *w = container_of(work, struct alloc_work, work);
	struct nvmeibs_ib_port *ib_port = w->port;
	struct nvmeib_rdma_cm *cm_id = w->cm_id;
	struct nvmeibs_client *cl = w->cl;
	struct nvmeibc_login_request *req = &w->req;
	struct nvmeibs_login_reject lrej = {{0}};
	struct nvmeibs_login_reject *rej = &lrej;
	struct nvmeibs_login_response *rsp = NULL;
	struct nvmeibs_net_init *params = NULL;
	struct nvmeibs_net_init_target target;
	struct list_head *disks;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_disk_private_data *disk_private_data;

	__NFIN;
	/*allocate atomic test zone*/
	memset(&cl->atomic_test_zone_map, 0, sizeof(cl->atomic_test_zone_map));
	cl->atomic_test_zone_map.pd = P2NV(ib_port)->pd;
	cl->atomic_test_zone_map.n_pages = DIV_ROUND_UP((1 + NVMEIB_N_2ND_LOCK_CHS) * sizeof(u64), PAGE_SIZE);
	cl->atomic_test_zone_map.access_flags = IB_ACCESS_LOCAL_WRITE |
					IB_ACCESS_REMOTE_READ |
					IB_ACCESS_REMOTE_WRITE|
					IB_ACCESS_REMOTE_ATOMIC;
	cl->atomic_test_zone_map.dma_dir = DMA_BIDIRECTIONAL;
	if (nvmeib_mem_alloc_n_map(&cl->atomic_test_zone_map)) {
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_client_connect_lock_channel_work, "rejected NVMEIB_LOGIN_REQ because failed to allocate lock-ch atomic test zone.");
		cl_lock_dev_put(cl);
		goto reject;
	}

	rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!(rsp && params)) {
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_1_client_connect_lock_channel_work, "rejected NVMEIB_LOGIN_REQ because no memory.");
		cl_lock_dev_put(cl);
		goto reject;
	}
	cl->dismissed = false;
	snprintf(params->name, sizeof(params->name), "%s", "L");
	params->net_type = S_NET_LOCK;
	params->cl = cl;
	WARN_ON(ib_port == NULL);
	params->port = ib_port;
	params->cm_id = cm_id;
	params->s_msg_size = NVMEIBS_DEFAULT_IO_MSG_SIZE;
	params->max_send_sge = 1;
	params->r_msg_size = NVMEIBS_DEFAULT_IO_MSG_SIZE;
	params->sendq_size = cl->n_msgs;
	params->scq_size = params->sendq_size;
	/* Locks Channel does not receive anything */
	params->rcq_size = 0;
	if (!nvmeibs_use_pcpu_cq) {
		params->use_srq = false;
		params->scq_handler = lock_channel_send_completion;
	}
	else {
		params->use_srq = nvmeibs_support_srq(P2NV(ib_port));
		params->send_comp_h = l_send_comp_h;
		params->recv_comp_h = l_recv_comp_h;
	}
	params->cm_handler = NULL;
	params->scq_context = cl;
	params->rcq_handler = NULL;
	params->rcq_context = NULL;
	params->bn_handler = lock_client_end;
	params->bn_context = cl;
	params->use_atomic = true;
	params->pre_rw_handler = lock_ch_pre_rw;
	params->pre_rw_context = cl;
	params->ch_index = 0;

	/* create nvmeib_login_response */
	nvmeib_wire_op_cid_set_rsp(&rsp->base.op_cid, NVMEIB_LOGIN_RSP, 0);
	rsp->base.opcode = NVMEIBS_LOCK_CHANNEL;

	/* when not using (per disk) lock-dev, allow PCIe atomic capable devices
	   (SIW) to use wire atomic ops */
	if (likely(!nvmeibs_use_tcp_locks ||
			   nvmeib_device_sup_cap(P2NV(ib_port)->dev_type,
									 NVMEIB_DEVCAP_PCIE_ATOMICS))) {
		rsp->base.lrsp.atomic_cap = nvmeib_device_sup_cap(
			P2NV(ib_port)->dev_type, NVMEIB_DEVCAP_ATOMICS_RESP) ?
			IB_ATOMIC_HCA : IB_ATOMIC_NONE;
		rsp->base.lrsp.masked_atomic_cap = nvmeib_device_sup_cap(
			P2NV(ib_port)->dev_type,  NVMEIB_DEVCAP_MASKED_ATOMICS_RESP) ?
			IB_ATOMIC_HCA : IB_ATOMIC_NONE;
	} else {
		rsp->base.lrsp.atomic_cap = IB_ATOMIC_NONE;
		rsp->base.lrsp.masked_atomic_cap = IB_ATOMIC_NONE;
	}

	rsp->base.lrsp.atomic_cap = rsp->base.lrsp.atomic_cap;
	rsp->base.lrsp.masked_atomic_cap = rsp->base.lrsp.masked_atomic_cap;
	rsp->base.lrsp.atomic_test_zone_rkey = cpu_to_be32(cl->atomic_test_zone_map.rkey);
	rsp->base.lrsp.atomic_test_zone_raddr = cpu_to_be64(
		cl->atomic_test_zone_map.ioaddr + (params->ch_index * sizeof(u64)));

	/* set target */
	target.common = params;
	target.req = req;
	target.rsp = rsp;
	rej->reason = 0;
	if (!nvmeibs_net_allocate_target(&cl->lock_net, &target, rej)) {
		/* This was done under locks_guard
		params->bn_context = NULL;
		params->bn_handler = NULL;
		*/
		cl_lock_dev_put(cl);
		_NE(error_2_client_connect_lock_channel_work, "Fail to create target Lock QP");
		goto reject;
	}

	disks = &cl->disks;
	nvmeibs_disk_locks_guard();
	list_for_each_entry(cdisk, disks, link) {
		BUG_ON(!list_is_last(&cdisk->link, disks));
		disk_private_data = (struct nvmeibs_disk_private_data *)cdisk->di->priv;
		BUG_ON(!nvmeibs_use_tcp_locks && !disk_private_data->lock_dev);
		BUG_ON(nvmeibs_use_tcp_locks && disk_private_data->lock_dev);
	}
	nvmeibs_disk_lock_unguard();
	goto out;

reject:
	if (cl->atomic_test_zone_map.allocated) {
		nvmeib_mem_unmapn_n_free(&cl->atomic_test_zone_map);
	}
	if (rej->reason) {
		_NT(trace_client_connect_lock_channel_work, "Reject new connection");
		nvmeibs_send_login_reject(cm_id, rej, req);
	}
	/* we must remember to close the cm_id */
	nvmeib_rdma_destroy_cm(cm_id);

	nvmeib_ref_put(&ib_port->n_port_conns);

out:
	kfree(params);
	kfree(rsp);
	kfree(w);
	__NFOUT;
}

static void _2nd_ch_send_comp(struct ib_cq *cq, void *ctx)
{
	struct nvmeibs_2nd_lock_ch *_2nd_ch_ptr = ctx;
	struct nvmeibs_client *cl = _2nd_ch_ptr->cl;
	struct nvmeibs_net *net = _2nd_ch_ptr->net;
	DECLARE_IB_WC_ONSTACK(wc);

	NFIN;

	BUG_ON(nvmeibs_use_pcpu_cq);

	ib_req_notify_cq(cq, IB_CQ_NEXT_COMP);
	while (ib_poll_cq(cq, 1, &wc) > 0) {
		/* check if someone waits for the send completion */
		if (nvmeib_opcode_from_wc(&wc) == NVMEIB_DRAIN_QUEUE) {
			nvmeibs_net_on_drain_sq(net);
			break;
		} else
			_NW(warn_client_2nd_ch_send_comp, "Unexpected wr_id @WR_ID_LLONG (opcode: @OPCODE, status: @STATUS) on client @CL secondary lock channel @IDX",
			   wc.wr_id, wc.opcode, wc.status, cl, _2nd_ch_ptr->idx);
	}
	NFOUT;
}

#if 0
static void _2nd_lock_ch_pre_rw(void *ctx)
{
	struct nvmeibs_2nd_lock_ch *_2nd_ch_ptr = ctx;
	struct nvmeibs_client *cl = _2nd_ch_ptr->cl;

	__NFIN;

	/* Disconnect the main lock net and it will disconnect the rest */
	nvmeibs_net_release(cl->lock_net);

	__NFOUT;
}
#endif

static void connect_2nd_lock_ch_work(struct workqe_struct *work)
{
	struct alloc_work *w = container_of(work, struct alloc_work, work);
	struct nvmeibs_ib_port *ib_port = w->port;
	struct nvmeib_rdma_cm *cm_id = w->cm_id;
	struct nvmeibs_client *cl = w->cl;
	struct nvmeibc_login_request *req = &w->req;
	struct nvmeibs_login_reject lrej = {{0}};
	struct nvmeibs_login_reject *rej = &lrej;
	struct nvmeibs_login_response *rsp = NULL;
	struct nvmeibs_net_init *params = NULL;
	struct nvmeibs_net_init_target target;
	int _2nd_ch_idx;

	__NFIN;
	if (!cl->lock_net) {
		_NE(error_client_connect_2nd_lock_ch_work, "rejected NVMEIB_LOGIN_REQ for 2nd Lock Channel of client @CL because primary net is NULL", cl);
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_LOCK_2ND_NO_PRIMARY);
		goto reject;
	}
	if (atomic_read(&cl->lock_net->dying)) {
		_NE(error_1_client_connect_2nd_lock_ch_work, "rejected NVMEIB_LOGIN_REQ for client @CL because primary net is dying", cl);
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_LOCK_2ND_NO_PRIMARY);
		goto reject;
	}
	if ((_2nd_ch_idx = atomic_inc_return(&cl->n_2nd_lock_ch) - 1) >= NVMEIB_N_2ND_LOCK_CHS) {
		_NE(error_2_client_connect_2nd_lock_ch_work, "rejected NVMEIB_LOGIN_REQ for client @CL because the maximum Secondary Lock Channels are logged in.", cl);
		goto reject;
	}

	rsp = kzalloc(sizeof(*rsp), GFP_KERNEL);
	params = kzalloc(sizeof(*params), GFP_KERNEL);
	if (!(rsp && params)) {
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);
		_NE(error_3_client_connect_2nd_lock_ch_work, "rejected NVMEIB_LOGIN_REQ for client @CL because no memory.", cl);
		goto reject;
	}
	cl->dismissed = false;
	snprintf(params->name, sizeof(params->name), "%s.%02d", "L", _2nd_ch_idx);
	params->net_type = S_NET_LOCK_2ND;
	params->cl = cl;
	WARN_ON(ib_port == NULL);
	params->port = ib_port;
	params->cm_id = cm_id;
	/* Secondary Lock Net does not receive or send anything */
	params->s_msg_size = NVMEIBS_DEFAULT_IO_MSG_SIZE;
	params->max_send_sge = 1;
	params->r_msg_size = 0;
	params->sendq_size = 1;
	params->scq_size = 1;
	params->rcq_size = 0;
	if (!nvmeibs_use_pcpu_cq) {
		params->use_srq = false;
		params->scq_handler = _2nd_ch_send_comp;
	}
	else {
		params->use_srq = nvmeibs_support_srq(P2NV(ib_port));
		params->send_comp_h = l_2nd_send_comp_h;
		params->recv_comp_h = l_2nd_recv_comp_h;
	}
	params->cm_handler = NULL;
	params->scq_context = &cl->_2nd_lock_ch[_2nd_ch_idx];
	params->rcq_handler = NULL;
	params->rcq_context = NULL;
	params->use_atomic = true;
	params->ch_index = _2nd_ch_idx + 1;

	/* init values for _2nd_lock_ch_pre_rw() */
	cl->_2nd_lock_ch[_2nd_ch_idx].cl = cl;
	cl->_2nd_lock_ch[_2nd_ch_idx].idx = _2nd_ch_idx;
	#if 0
	params->pre_rw_handler = _2nd_lock_ch_pre_rw;
	params->pre_rw_context = &cl->_2nd_lock_ch[_2nd_ch_idx];
	#endif

	/* create nvmeib_login_response */
	nvmeib_wire_op_cid_set_rsp(&rsp->base.op_cid, NVMEIB_LOGIN_RSP, 0);
	rsp->base.opcode = NVMEIBS_2ND_LOCK_NET;

	/* when not using (per disk) lock-dev, allow PCIe atomic capable devices
	   (SIW) to use wire atomic ops */
	if (likely(!nvmeibs_use_tcp_locks ||
			nvmeib_device_sup_cap(P2NV(ib_port)->dev_type,
			NVMEIB_DEVCAP_PCIE_ATOMICS)))
	{
		rsp->base.lrsp.atomic_cap = nvmeib_device_sup_cap(
			P2NV(ib_port)->dev_type, NVMEIB_DEVCAP_ATOMICS_RESP) ?
			IB_ATOMIC_HCA : IB_ATOMIC_NONE;
		rsp->base.lrsp.masked_atomic_cap = nvmeib_device_sup_cap(
			P2NV(ib_port)->dev_type,  NVMEIB_DEVCAP_MASKED_ATOMICS_RESP) ?
			IB_ATOMIC_HCA : IB_ATOMIC_NONE;
	} else {
		rsp->base.lrsp.atomic_cap = IB_ATOMIC_NONE;
		rsp->base.lrsp.masked_atomic_cap = IB_ATOMIC_NONE;
	}

	rsp->base.lrsp.atomic_test_zone_rkey = cpu_to_be32(cl->atomic_test_zone_map.rkey);
	rsp->base.lrsp.atomic_test_zone_raddr = cpu_to_be64(
		cl->atomic_test_zone_map.ioaddr + (params->ch_index * sizeof(u64)));

	/* set target */
	target.common = params;
	target.req = req;
	target.rsp = rsp;
	rej->reason = 0;
	if (!nvmeibs_net_allocate_target(&cl->_2nd_lock_ch[_2nd_ch_idx].net, &target, rej)) {
		_NE(error_4_client_connect_2nd_lock_ch_work, "Fail to create target Secondary Lock QP");
		goto reject;
	}
	/* Moved upwards... Invalidating is done only @cl-release
	cl->_2nd_lock_ch[_2nd_ch_idx].cl = cl;
	cl->_2nd_lock_ch[_2nd_ch_idx].idx = _2nd_ch_idx;
	*/
	goto out;

reject:
	atomic_dec(&cl->n_2nd_lock_ch);
	if (rej->reason) {
		_NT(trace_client_connect_2nd_lock_ch_work, "Reject new connection");
		nvmeibs_send_login_reject(cm_id, rej, req);
	}
	/* we must remember to close the cm_id */
	nvmeib_rdma_destroy_cm(cm_id);

	nvmeib_ref_put(&ib_port->n_port_conns);

out:
	kfree(params);
	kfree(rsp);
	kfree(w);
	__NFOUT;
}

int nvmeibs_client_connect_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej,
	workq_func_t connect_channel_work_f)
{
	struct alloc_work *w = NULL;
	int rv;
	__NFIN;

	/* We count NON-admin channels too as they may use port's resources
	   (e.g. io-ka) and thus when port is hot-removed (e.g.  siw or mlx
	   driver detect health issue), it must wait for them to do ref-put */
	if (nvmeib_ref_get(&ib_port->n_port_conns) == 0) {
		_NE(error_0_client_nvmeibs_client_connect_channel,
			"Failed ref-get port cl @CL_NAME)", cl->name);
		rv = -1;
		goto err_reject;
	}

	if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		_NE(error_1_client_nvmeibs_client_connect_channel,
			"Fail to allocate work (cl @CL_NAME)", cl->name);
		rv = -1;
		goto err_refput;
	}

	WQ_INIT_WORK(&w->work, connect_channel_work_f);
	w->cl = cl;
	w->port = ib_port;
	w->cm_id = cm_id;
	w->req = *req;
	if ((rv = nvmeibs_client_add_work(cl, &w->work)) < 0) {
		_NT(trace_client_nvmeibs_client_connect_channel,
			"Fail to add work (cl @CL_NAME)", cl->name);
		rv = -1;
		goto err_work;
	}

	rv = 0;
	goto out;

err_work:
	kfree(w);

err_refput:
	nvmeib_ref_put(&ib_port->n_port_conns);

err_reject:
	rej->reason = __constant_cpu_to_be32(
		NVMEIBS_LOGIN_REJ_INSUFFICIENT_RESOURCES);

out:
	__NFOUT;
	return rv;
}

int nvmeibs_client_connect_admin_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej)
{
	int rv;

	__NFIN;
	rv = nvmeibs_client_connect_channel(
		ib_port, cm_id, cl, req, rej, connect_admin_channel_work);
	_NT(trace_client_nvmeibs_client_connect_admin_channel, "cl @CL_NAME: @STATUS_STR connect-admin-channel work to cl-wq",
		cl->name, !rv ? "Added" : "Failed to add");
	__NFOUT;

	return rv;
}

int nvmeibs_client_connect_lock_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej)
{
	int rv;

	__NFIN;
	rv = nvmeibs_client_connect_channel(
		ib_port, cm_id, cl, req, rej, connect_lock_channel_work);
	_NT(trace_client_nvmeibs_client_connect_lock_channel, "cl @CL_NAME: @STATUS_STR connect-lock-channel work to cl-wq",
		cl->name, !rv ? "Added" : "Failed to add");
	__NFOUT;

	return rv;
}

int nvmeibs_client_connect_2nd_lock_ch(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej)
{
	int rv;

	__NFIN;
	rv = nvmeibs_client_connect_channel(
		ib_port, cm_id, cl, req, rej, connect_2nd_lock_ch_work);
	_NT(trace_client_nvmeibs_client_connect_2nd_lock_ch, "cl @CL_NAME: @STATUS_STR connect_2nd_lock_net_work to cl-wq",
		cl->name, !rv ? "Added" : "Failed to add");
	if (rv)
		atomic_dec(&cl->n_2nd_lock_ch);
	__NFOUT;

	return rv;
}

/* stop ib-post-send triggered by disk completion */

int nvmeibs_client_connect_io_channel(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_login_reject *rej)
{
	int rv;
	__NFIN;
	/* RDDA removed - stubbed */
	rv = -ENOTSUPP;
	(void)ib_port;
	(void)cm_id;
	(void)cl;
	(void)req;
	(void)rej;
	__NFOUT;
	return rv;
}

static void free_cmds(struct list_head *l)
{
	struct nvmeibs_cmd_info *i, *ti;

	NFIN;
	list_for_each_entry_safe(i, ti, l, link) {
		_NT(free_cmds, "list-del info=@PTR, op=@INT", i, i->op);
		list_del(&i->link);
		if (i->op == NVMEIBS_PUT_RSC) {
			kfree(get_put_info(i));
		} else if (i->op == NVMEIBS_GET_RSC) {
			kfree(get_get_info(i));
		} else if (i->op == NVMEIBS_JAM_ABND2FREE) {
			kfree(get_abnd_free_info(i));
		} else if (i->op == NVMEIBS_RGID_CHANGE) {
			kfree(get_gid_change_info(i));
		} else if (i->op == NVMEIBS_IOCH_DRAINED) {
			kfree(get_ioch_drained_info(i));
		} else {
			kfree(i);
		}
	}
	NFOUT;
}

void nvmeibs_client_prp_io_channel_def(struct nvmeibs_client *cl,
	struct nvmeibc_login_request *req, struct nvmeibs_ib_port *ib_port,
	struct nvmeibc_io_channel_def *def)
{
	union ib_gid sgid, dgid;
	u16 qp_num;
	const int d_name_size = min(sizeof(cl->disk_name), sizeof(def->disk_name));

	NFIN;
	memset(def->disk_name, 0, sizeof(def->disk_name));
	memcpy(def->disk_name, cl->disk_name, d_name_size);
	nvmeibc_login_req_get_ioch(req,
				&sgid.global.subnet_prefix, &sgid.global.interface_id,
				&dgid.global.subnet_prefix, &dgid.global.interface_id,
				&qp_num, NULL);
	memcpy(def->sgid, sgid.raw, sizeof(def->sgid));
	memcpy(def->dgid, dgid.raw, sizeof(def->dgid));
	def->qp_num = cpu_to_be16(qp_num);
	_ND(trace_client_nvmeibs_client_prp_io_channel_def, "s=@SGID --> d=@DGID", def->sgid, def->dgid);

	NFOUT;
}

static void free_client_cmds(struct nvmeibs_client *cl)
{
	__NFIN;
	_NT(t0_free_client_cmds, "@CL (@CL_NAME), free pending-cmd", cl, cl->name);
	free_cmds(&cl->pending_cmd);

	_NT(t1_free_client_cmds, "@CL (@CL_NAME), free wire-cmd", cl, cl->name);
	free_cmds(&cl->wire_cmd);
	__NFOUT;
}

static void free_msg_area(struct nvmeibs_client *cl)
{
	__NFIN;

	if (cl->out_msg_area_map.mr)
		nvmeib_mem_unmapn_n_free(&cl->out_msg_area_map);
	if (cl->in_msg_area_map.mr)
		nvmeib_mem_unmapn_n_free(&cl->in_msg_area_map);

	kfree(cl->out_msg_area_map.pages);
	kfree(cl->in_msg_area_map.pages);

	if (cl->out_msg_area)
		free_pages_exact(cl->out_msg_area, NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);
	if (cl->in_msg_area)
		free_pages_exact(cl->in_msg_area, NVMEIBS_CONFIG_MSG_RDMA_PAGES << PAGE_SHIFT);

	/* JH IOMMU: DMA_TO_DEVICE is correct. Buffer is source for local RDMA_SEND */
	ib_dma_unmap_single(P2IB(cl->ib_port), cl->ka_msg_dma_addr,
		sizeof(*cl->ka_msg_area), DMA_TO_DEVICE);

	nvmesh_memmgr_metric_on_free_update(s_clients_msg_area, calc_msg_area_sz(cl));
	__NFOUT;
}

static void free_recv_q(struct nvmeibs_client *cl)
{
	__NFIN;
	if (cl->recv_q) {
		nvmeib_free_recvq(cl->recv_q);
		kfree(cl->recv_q);
		cl->recv_q = NULL;
	}
	__NFOUT;
}

static void free_client(struct workqe_struct *work)
{
	struct nvmeibs_free_cl_work *fr_cl =
		container_of(work, struct nvmeibs_free_cl_work, work);
	struct nvmeibs_client *cl = fr_cl->cl;
	struct nvmeibs_ib_port *ib_port = cl->ib_port;

	NFIN;
	_NT(trace_client_free_client, "cl '@CL_NAME' (@CL), release (step 3/3)", cl->name, cl);

	if (fr_cl->free_cl_cb) /* remove_single_client */
		fr_cl->free_cl_cb(cl);
	kfree(fr_cl);

	nvmeib_ref_put(&ib_port->n_port_conns);

	NFOUT;
}

static void finish_client_work(struct workqe_struct *work)
{

	struct nvmeibs_free_cl_work *fr_cl =
		container_of(work, struct nvmeibs_free_cl_work, work);
	struct nvmeibs_client *cl = fr_cl->cl;
	enum nvmeibs_logout_reason reason = fr_cl->reason;
	int i;

	NFIN;

	_NT(trace_client_finish_client_work, "cl '@CL_NAME' (@CL), release (step 2/3)", cl->name, cl);

	if (!on_wq(cl->remove_wq)) {
		_NW(warn_1_client_finish_client_work, "Oops, unexpected ctx, exp wq @PID but pid=@PID",
		   wq_pid(cl->remove_wq), current->pid);
		WARN_ON_ONCE(1);
	}

	cookie_store_remove_local_ch(cl);

	/* flush works already pending e.g. net-allocate */
	wq_flush(cl->wq);
	/* Now we ensure that if NVMEIBC_MA_GET_IO was arrived before we called wq_flush we already parsed it and proc was added
	   In addition if a NVMEIBC_MA_GET_IO will arrive from this point the parsing function will see sclient as dying
	   so the proc won't be added */
	single_sclient_proc_umkdir(cl);


	_NT(trace_1_client_finish_client_work, "Add release-work for (connected) channels");
	nvmeibs_disk_get_disks(NULL);
	stop_main_channel(cl, reason);
	stop_io_channels(cl);
	stop_lock_channel(cl);
//	stop_main_channel(cl);
	nvmeibs_disk_put_disks();

	_NT(trace_2_client_finish_client_work, "flush cl wq...");
	/* flush works added above and works added before net-release-work of
	   main-wq had disabled recv-comps of main-ch (e.g. alloc-nrq-nets).
	   This can happen if client-release was NOT triggered from net-layer
	   (e.g. by disk-removal, toma-disconnet, etc.) in which case net-release
	   work of main-wq was only added here, above */
	wq_flush(cl->wq);

	if (nvmeib_ref_get(&cl->msg_area_refcount)) {
		BUG_ON(!cl->release_done);
		complete(cl->release_done);
		nvmeib_ref_put(&cl->msg_area_refcount);
	}

	nvmeibs_disk_remove_client_all(cl);

	/* free client memory */
	free_anics(cl);
	/* free pending and wire commands */
	free_client_cmds(cl);
	/* free rdma message area */
	free_msg_area(cl);
	/* free receive queue */
	free_recv_q(cl);

	kfree(cl->disk_cache);
	kfree(cl->ka_msg_area);
	kfree(cl->lock_net);
	for (i = 0; i < NVMEIB_N_2ND_LOCK_CHS; i++) {
		if (cl->_2nd_lock_ch[i].net &&
			!atomic_read(&cl->_2nd_lock_ch[i].net->dying)) {
			_NW(warn_2_client_finish_client_work,
				"cl @CL_NAME, zombie secondary net #@INDEX (@NET)",
			   cl->name, i, cl->_2nd_lock_ch[i].net);
			WARN_ON_ONCE(1);
		}
		kfree(cl->_2nd_lock_ch[i].net);
		cl->_2nd_lock_ch[i].cl = NULL;
		cl->_2nd_lock_ch[i].net = NULL;
	}

	/* before freeing cl's shared resources (deprecated)*/
	if (cl->is_local) {
		_NT(trace_3_client_finish_client_work, "ldisk-wait...");
		nvmeibs_client_ldisk_wait(cl);
	}

	/* Return journal range to SERJIO */
	if (cl->jrnl_rng != NVMEIB_EC_INVALID_JOURNAL_RANGE) {
		_NT(trace_4_client_finish_client_work, "Return journal chunk @JRNL_RNG_IDX", cl->jrnl_rng);
		nvmeibs_serjio_return_journal_range(cl->di, cl->jrnl_rng);
		cl->jrnl_rng = NVMEIB_EC_INVALID_JOURNAL_RANGE;
	}

	/* report client-disconnect event to Toma */
	if (cl->connected_to_toma > 0)
		nvmeibs_toma_report_event_client_disconnect(cl);
	/* remove self from toma connections hash table */
	nvmeibs_toma_remove_client(cl);

	/* Before freeing client - validate */
	BUG_ON(!nvmeibs_async_cookie_store_is_empty(&cl->cookie_store));

	/* allow disk removal (del from disks-list and returning to nvme-layer) */
	cl_disk_put(cl);

	kfree(cl->net);

	WQ_INIT_WORK(work, free_client);
	nvmeibs_ib_port_add_work(cl->ib_port, work);

	NFOUT;
}

/**
 * nvmeibs_client_release() - Close an client by closing its QP.
 *
 * Reset the QP and make sure all resources associated with the
 * client will be deallocated at an appropriate time.
 *
 * Done in 3 phases: port WQ, clinet-WQ and port-WQ again.
 */
int nvmeibs_client_release(struct nvmeibs_client *cl,
	void (*free_cl_cb)(struct nvmeibs_client *),
	enum nvmeibs_logout_reason reason)
{
	int dying = atomic_inc_return(&cl->dying);
	int rv = 1;
	struct nvmeibs_free_cl_work *free_cl_work;
	unsigned long flags;

	__NFIN;
	_NT(trace_nvmeibs_client_release, "LOGOUT: dismissing client @CL", cl);
	cl->dismissed = true;
	nvmeib_q_set_log_level(cl->wq, NVMEIB_Q_LOG_LEVEL_VERBOSE);

	if (dying != 1) {
		_ND(trace_client_nvmeibs_client_release, "We are already scheduled for death @DYING times", dying);
		rv = 0;
		goto out;
	}

	cl->dying_start_time = jiffies;
	_NT(trace_1_client_nvmeibs_client_release, "cl '@CL_NAME' (@CL), release (step 1/3)", cl->name, cl);

	if (cl->clnt_toma_rsp.comp) {
		spin_lock_irqsave(&cl->clnt_toma_rsp.comp_lock, flags);
		if (cl->clnt_toma_rsp.comp) {
			_NT(trace_client_nvmeibs_client_release_toma_rsp, "cl '@CL_NAME' (@CL) - complete toma rsp with error as we are dying", cl->name, cl);
			cl->clnt_toma_rsp.opcode = NVMEIBC_RSP_TOMA_OPCODE_ERR;
			complete(cl->clnt_toma_rsp.comp);
		}
		spin_unlock_irqrestore(&cl->clnt_toma_rsp.comp_lock, flags);
	}

	/* wait for pending work before we start the shut down process */
	if (nvmeib_ref_get(&cl->msg_area_refcount)) {
		BUG_ON(!cl->release_done);
		complete(cl->release_done);
		nvmeib_ref_put(&cl->msg_area_refcount);
	}

	free_cl_work = kzalloc(sizeof(*free_cl_work), GFP_KERNEL);
	if (free_cl_work) {
		WQ_INIT_WORK(&free_cl_work->work, finish_client_work);
		free_cl_work->cl = cl;
		free_cl_work->free_cl_cb = free_cl_cb;
		free_cl_work->reason = reason;
		wq_add_work(cl->remove_wq, &free_cl_work->work);
	} else
		_NE(error_client_nvmeibs_client_release, "Cannot allocate memory for client @CL_NAME release", cl->name);

out:
	NFOUT;
	return rv;
}

void cookie_store_remove_local_ch(struct nvmeibs_client *cl)
{
	NFIN;

	_NT(t0_cookie_store_remove_local_ch,
		"@CL (@CL_NAME), attempt remove local-ch from cookie-store, "
		"called from '@__BUILTIN_RETURN_ADDRESS_FUNC'",
		cl, cl->name, __builtin_return_address(0));

	nvmeibs_async_cookie_store_bail_all_chid(&cl->cookie_store, NVMEIBS_ASYNC_LOCAL_CHANNEL);
	nvmeibs_async_cookie_store_wait_chid(&cl->cookie_store, NVMEIBS_ASYNC_LOCAL_CHANNEL);
	nvmeibs_async_cookie_store_remove_chid(&cl->cookie_store, NVMEIBS_ASYNC_LOCAL_CHANNEL);

	NFOUT;
}

struct nvmeib_iu *nvmeibs_client_get_ioctx(struct nvmeibs_client *cl)
{
	struct nvmeib_iu *iu;

	__NFIN;
	iu = nvmeibs_net_get_ioctx(cl->net);
	__NFOUT;
	return iu;
}

void nvmeibs_client_put_ioctx(struct nvmeibs_client *cl,
	struct nvmeib_iu *ioctx)
{
	__NFIN;
	nvmeibs_net_put_ioctx(cl->net, ioctx);
	__NFOUT;
}

int nvmeibs_client_add_work(struct nvmeibs_client *cl,
	struct workqe_struct *work)
{
	return wq_add_work(cl->wq, work) ? 0 : -1;
}

void nvmeibs_client_put_resource(struct nvmeibs_client *cl,
	const char *disk_name, __be64 *rscs, u64 n)
{
	struct nvmeibs_put_cmd *cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	struct get_put_rsc_workq *work = kzalloc(sizeof(*work), GFP_KERNEL);

	__NFIN;
	BUG_ON(cl->is_local); //don't send resources to local client
	if (!cmd || !work) {
		kfree(cmd);
		kfree(work);
		_NE(error_client_nvmeibs_client_put_resource, "OOM: fail to allocate command");
		goto out;
	}
	cmd->cmd.info.op = NVMEIBS_PUT_RSC;
	cmd->cmd.info.has_rsp = true;
	memcpy(cmd->cmd.disk_name, disk_name, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	memcpy(cmd->ids, rscs, sizeof(cmd->ids));
	cmd->n_ids = n;
	_NT(trace_client_nvmeibs_client_put_resource, "Sending disk @DISK_NAME n_rsc @RSC_NUM to client @CL_NAME", disk_name, n, cl->name);
	WQ_INIT_WORK(&work->work, send_put_resource_work);
	work->cl = cl;
	work->cmd = &cmd->cmd;
	if (nvmeibs_client_add_work(cl, &work->work) < 0) {
		_NT(trace_1_client_nvmeibs_client_put_resource, "Fail to add work");
		kfree(cmd);
		kfree(work);
	}

out:
	__NFOUT;
}

void nvmeibs_client_get_resource(struct nvmeibs_client *cl,
	const char *disk_name, u64 n)
{
	struct nvmeibs_get_cmd *cmd = kzalloc(sizeof(*cmd), GFP_KERNEL);
	struct get_put_rsc_workq *work = kzalloc(sizeof(*work), GFP_KERNEL);

	__NFIN;
	if (!cmd || !work) {
		kfree(cmd);
		kfree(work);
		_NE(error_client_nvmeibs_client_get_resource, "OOM: fail to allocate command");
		goto out;
	}
	cmd->cmd.info.op = NVMEIBS_GET_RSC;
	cmd->cmd.info.has_rsp = true;
	memcpy(cmd->cmd.disk_name, disk_name, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	cmd->n = n;
	_NT(trace_client_nvmeibs_client_get_resource, "request @RSC_NUM rscs from @CL_NAME", n, cl->name);
	WQ_INIT_WORK(&work->work, send_get_resource_work);
	work->cl = cl;
	work->cmd = &cmd->cmd;
	nvmeibs_client_add_work(cl, &work->work);
out:
	__NFOUT;
}

int nvmeibs_client_create_disk_cache(struct nvmeibs_client *cl,
	struct list_head *disks, int n)
{
	struct nvmeibs_disk_info *disk;
	int i, rv = 0;

	__NFIN;
	_ND(trace_client_nvmeibs_client_create_disk_cache, "n=@N_RSCS", n);
	if (cl->disk_cache) {
		_NE(error_client_nvmeibs_client_create_disk_cache, "cl @CL already has disk names arr", cl);
		rv = -1;
		goto out;
	}
	if (!(cl->disk_cache = kzalloc(sizeof(*cl->disk_cache) * n,
		GFP_KERNEL))) {
		_NE(error_1_client_nvmeibs_client_create_disk_cache, "OOM: fail to allocate client disk names array");
		rv = -1;
		goto out;
	}
	i = 0;
	list_for_each_entry(disk, disks, link)
		cl->disk_cache[i++] = disk;
	cl->n_caches = n;

out:
	__NFOUT;
	return rv;
}

static ssize_t print_cl_rionic_info(char *buffer, int len,
	struct nvmeibs_rionic *rionic)
{
	int count = 0;
	char gid[GUID_SIZE];

	NFIN;
	format_gid_raw(rionic->gid.raw, gid);
	count += scnprintf(buffer + count, len - count,
		"{\"gid\":\"%s\",\n"
		"\"n_io_channels\":%d,\n"
		"\"n_nr_channels\":%d"
		"}\n",
		gid, 0 /* RDDA removed */, rionic->n_nr_channels);

	NFOUT;
	return count;
}

static ssize_t print_cl_lionic_info(char *buffer, int len,
	struct nvmeibs_lionic *lionic)
{
	int count = 0, i;
	char gid[GUID_SIZE];
	bool first = true;


	NFIN;
	format_gid_raw(lionic->gid.raw, gid);
	count += scnprintf(buffer + count, len - count,
		"{\"gid\":\"%s\",\n"
		"\"n_rionics\":%d,\n"
		"\"hw_type\":%d,"
		"\"hw_type_name\":\"%s\",\n"
		"\"rionics\":[\n",
		gid, lionic->n_rionics, lionic->hw_type,
		nvmeib_ib_driver_dev_type(lionic->hw_type));

	for (i = 0; i < lionic->n_rionics; ++i) {
		if (!first)
			count += scnprintf(buffer + count, len - count,",\n");
		first = false;
		count += print_cl_rionic_info(buffer + count, len - count,
			&lionic->rionics[i]);
	}
	count += scnprintf(buffer + count, len - count, "]}\n");

	NFOUT;
	return count;
}

static ssize_t print_cl_disk_info(char *buffer, int len,
	struct nvmeibs_client_disk *cdisk)
{
	int count = 0,i;
	bool first = true;

	NFIN;
	count += scnprintf(buffer + count, len - count,
		"{\"name\":\"%s\"\n,\"id\":%lld,\n\"n_lionics\":%d,\n\"s_lionics\":[\n",
		cdisk->di->disk_id, cdisk->id, cdisk->n_lionics);
	for (i = 0; i < cdisk->n_lionics; ++i) {
		if (!first)
			count += scnprintf(buffer + count, len - count,",\n");
		first = false;
		count += print_cl_lionic_info(buffer + count, len - count,
			&cdisk->lionics[i]);
	}
	count += scnprintf(buffer + count, len - count, "]}\n");

	NFOUT;
	return count;
}

static ssize_t print_cl_anic_info(char *buffer, int len,
	struct nvmeibs_anic *anic)
{
	int count = 0, i;
	bool first = true;
	char gid[GUID_SIZE];

	NFIN;
	format_gid_raw(anic->gid.raw, gid);
	count += scnprintf(buffer + count, len - count,
		"{\"gid\":\"%s\",\n\"n_disks\":%d\n,\"disks\":[\n",
		gid, anic->n_disks);
	for (i = 0; i < anic->n_disks; ++i) {
		if (!first)
			count += scnprintf(buffer + count, len - count,",\n");
		first = false;
		count += print_cl_disk_info(buffer + count, len - count,
			&anic->disks[i]);
	}
	count += scnprintf(buffer + count, len - count, "]}\n");

	NFOUT;
	return count;
}

/*print client information in json format*/
ssize_t nvmeibs_client_print_client_info(struct nvmeibs_client *cl,
	char *buffer, int len)
{
	int count = 0;
	char gid[GUID_SIZE];
	bool first = true;
	struct nvmeibs_anic *anic;

	__NFIN;
	format_gid_raw(cl->ib_port->gid.gid.raw, gid);
	count += scnprintf(buffer + count, len - count,
		"{\"name\":\"%s\",\n"
		"\"main_admin_lnic\":\"%s\",\n"
		"\"n_lnic\":%d\n,\"admin_nics\":[\n",
		cl->name, gid, cl->n_lnics);


	list_for_each_entry(anic, &cl->anics, link) {
		if (!first)
			count += scnprintf(buffer + count, len - count,",\n");
		first = false;
		count += print_cl_anic_info(buffer + count, len - count, anic);
	}

	count += scnprintf(buffer + count, len - count, "]}\n");

	__NFOUT;
	return count;
}

VEX_OPS_ENCODE_FN_SIG(vex_ach_abnd_free_srv_base_encode)
{
	struct volume_server_cmd_jmd_free_abnd_base *base = wire_buf;
	struct nvmeibs_abnd_free_cmd *cmd = arg;
	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*base) > wire_buf_end);
	memcpy(base->disk_name, cmd->cmd.disk_name,
		   NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	base->rng_num = cpu_to_be32(cmd->rng_num);
	base->rng_gen_id = cpu_to_be64(cmd->rng_gen_id);
	nvmeib_bitmap_to_be32(base->abnd_free_bitmap,
						  cmd->abnd_free_bitmap, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	memcpy(base->free_ent_md, cmd->ent_md, sizeof(base->free_ent_md));
	return sizeof(*base);
}

VEX_OPS_ENCODE_FN_SIG(vex_ach_abnd_free_srv_ext1_encode)
{
	struct volume_server_cmd_jmd_free_abnd_ext1 *ext1 = wire_buf;
	struct nvmeibs_abnd_free_cmd *cmd = arg;
	BUG_ON(!wire_buf);
	BUG_ON(wire_buf + sizeof(*ext1) > wire_buf_end);
	ext1->binje = binje_to_be(cmd->binje);
	return sizeof(*ext1);
}

static void send_abnd_free(struct nvmeibs_client *cl,
			   struct nvmeibs_abnd_free_cmd *cmd, struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req;
	int rv;

	__NFIN;
	req = send_ioctx->buf;
	rv = CALL_VEX_OP(encode,
			vex_ach_abnd_free_srv_ops, ONE_EXT,
				base, vex_ach_abnd_free_srv_base_encode,
				ext1, vex_ach_abnd_free_srv_ext1_encode,
				cl->vex_ach_ops[vex_ach_abnd_free], req->payload, send_ioctx->buf + send_ioctx->size, cmd);
	BUG_ON(rv < 0);

	send_resource(cl, &cmd->cmd.info,
				  cl->vex_ach_ops[vex_ach_abnd_free]->vex_ext, send_ioctx);
	__NFOUT;
}

static void send_abnd_free_work(struct workqe_struct *work)
{
	struct get_put_rsc_workq *w =
	container_of(work, struct get_put_rsc_workq, work);
	struct nvmeibs_abnd_free_cmd *cmd = get_abnd_free_info(&w->cmd->info);
	struct nvmeibs_client *cl = w->cl;
	int dying = atomic_read(&cl->net->dying);
	struct nvmeib_iu *send_ioctx;
	unsigned long flags;

	__NFIN;
	if (!dying) {
		if ((send_ioctx = nvmeibs_client_get_ioctx(cl)))
			send_abnd_free(cl, cmd, send_ioctx);
		else {
			spin_lock_irqsave(&cl->spinlock, flags);
			list_add_tail(&cmd->cmd.info.link, &cl->pending_cmd);
			spin_unlock_irqrestore(&cl->spinlock, flags);
		}
	} else {
		kfree(cmd);
	}
	kfree(w);
	__NFOUT;
}

struct send_jrn_abnd_free_params
{
	const char *disk_name;
	u32 rng_num;
	u64 rng_gen_id;
	unsigned long *abnd_free_bitmap;
	struct nvmeib_jrnl_ent_md *ent_md;
	binje_t binje;
};

static int send_journal_abnd_free_fc(struct nvmeibs_client *cl, void *arg)
{
	struct send_jrn_abnd_free_params *params = arg;
	const char *disk_name = params->disk_name;
	u32 rng_num = params->rng_num;
	unsigned long *abnd_free_bitmap = params->abnd_free_bitmap;
	struct nvmeibs_abnd_free_cmd *cmd = kzalloc(sizeof(*cmd), GFP_ATOMIC);
	struct get_put_rsc_workq *work = kzalloc(sizeof(*work), GFP_ATOMIC);
	int rv = 0;

	__NFIN;
	if (!cmd || !work) {
		kfree(cmd);
		kfree(work);
		_NE(error_client_send_journal_abnd_free_fc, "OOM: fail to allocate command");
		rv = -ENOMEM;
		goto out;
	}

	cmd->cmd.info.op = NVMEIBS_JAM_ABND2FREE;
	cmd->cmd.info.has_rsp = false;
	memcpy(cmd->cmd.disk_name, disk_name, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	cmd->rng_num = rng_num;
	cmd->rng_gen_id = params->rng_gen_id;
	memcpy(cmd->abnd_free_bitmap, abnd_free_bitmap, sizeof(unsigned long) * BITS_TO_LONGS(NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE));
	memcpy(cmd->ent_md, params->ent_md,
		   sizeof(*params->ent_md) * NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE);
	cmd->binje = params->binje;
	_ND(trace_client_send_journal_abnd_free_fc, "Sending jrange @RNG_NUM abnd->free to client @CL_NAME", rng_num, cl->name);
	WQ_INIT_WORK(&work->work, send_abnd_free_work);
	work->cl = cl;
	work->cmd = &cmd->cmd;
	if (nvmeibs_client_add_work(cl, &work->work) < 0) {
		_NT(trace_1_client_send_journal_abnd_free_fc, "Fail to add work");
		kfree(cmd);
		kfree(work);
		rv = -EFAULT;
		goto out;
	}

out:
	__NFOUT;
	return rv;
}

int nvmeibs_client_send_journal_abnd_free(u64 client_id, const char *disk_name,
					   u32 rng_num, u64 rng_gen_id,
					   unsigned long *abnd_free_bitmap,
					   struct nvmeib_jrnl_ent_md *ent_md, binje_t binje)
{
	struct send_jrn_abnd_free_params params;
	int rv;

	params.disk_name = disk_name;
	params.rng_num = rng_num;
	params.rng_gen_id = rng_gen_id;
	params.abnd_free_bitmap = abnd_free_bitmap;
	params.ent_md = ent_md;
	params.binje = binje;

	if ((rv = nvmeibs_cdb_cid_fast_call(client_id, send_journal_abnd_free_fc, &params))) {
		_NW(warn_client_nvmeibs_client_send_journal_abnd_free, "send_journal_abnd_free for client @CLIENT_ID failed", client_id);
	}
	return rv;
}

static void send_gid_change(struct nvmeibs_client *cl,
	struct nvmeibs_gid_change_cmd *cmd, struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req;

	__NFIN;
	req = send_ioctx->buf;
	memcpy(req->r_req.disk_name, cmd->cmd.disk_name,
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	memcpy(req->r_req.hw_gid, cmd->hw_gid.raw, sizeof(req->r_req.hw_gid));
	memcpy(req->r_req.gid, cmd->gid.raw, sizeof(req->r_req.gid));
	req->r_req.layer = cmd->layer;
	req->r_req.may_access = !!cmd->may_access;
	send_resource(cl, &cmd->cmd.info, vex_base, send_ioctx);
	__NFOUT;
}

static void nvmeibs_client_enable_lionic(struct nvmeibs_client *cl, union ib_gid *lgid)
{
	struct nvmeibs_client_disk *cdisk;
	int i, j;
	__NFIN;

	list_for_each_entry(cdisk, &cl->disks, link) {
		if (cdisk->lionics) {
			for (i = 0; i < cdisk->n_lionics; ++i) {
				if (!memcmp(lgid, &cdisk->lionics[i].gid, sizeof(*lgid))) {
					_NT(nvmeibs_client_enable_lionic_t1, "cl @CL_NAME, enabling lionic w/ lgid @GID", cl->name, lgid);
					for (j = 0; j < cdisk->lionics[i].n_rionics; j++) {
						set_rionic_ka_dying(&cdisk->lionics[i].rionics[j], false);
					}
					break;
				}
			}
		}
	}

	__NFOUT;
}

static void cl_gid_change_work(struct workqe_struct *work)
{
	struct get_put_rsc_workq *w =
		container_of(work, struct get_put_rsc_workq, work);
	struct nvmeibs_client *cl = w->cl;
	struct nvmeibs_gid_change_cmd *cmd = get_gid_change_info(&w->cmd->info);
	bool net_alive = (cl->net && !atomic_read(&cl->net->dying));
	struct nvmeib_iu *send_ioctx;
	NFIN;

	if (cmd->may_access) {
		nvmeibs_client_enable_lionic(cl, &cmd->gid);
	}

	if (net_alive) {
		if ((send_ioctx = nvmeibs_client_get_ioctx(cl))) {
			send_gid_change(cl, cmd, send_ioctx);
		}
		else {
			spin_lock_irq(&cl->spinlock);
			list_add_tail(&cmd->cmd.info.link, &cl->pending_cmd);
			spin_unlock_irq(&cl->spinlock);
		}
	} else {
		kfree(cmd);
	}
	kfree(w);

	NFOUT;
}

/* Called under clients-hash lock, while cl in hash */
int nvmeibs_client_update_gid_change(struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port)
{
	struct get_put_rsc_workq *w = kzalloc(sizeof(*w), GFP_ATOMIC);
	struct nvmeibs_gid_change_cmd *cmd = kzalloc(sizeof(*cmd), GFP_ATOMIC);
	int rv;
	NFIN;

	if (!w || !cmd) {
		_NE(error_client_nvmeibs_client_update_gid_change, "Fail to alloc work/cmd");
		kfree(cmd);
		kfree(w);
		rv = -ENOMEM;
		goto out;
	}

	cmd->cmd.info.op = NVMEIBS_RGID_CHANGE;
	cmd->cmd.info.has_rsp = false;
	memcpy(cmd->cmd.disk_name, cl->disk_name,
		   NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	cmd->hw_gid = ib_port->gid.hw_gid;
	cmd->gid = ib_port->gid.gid;
	cmd->layer = ib_port->layer;
	cmd->may_access = nvmeibs_ib_port_enabled(ib_port);

	WQ_INIT_WORK(&w->work, cl_gid_change_work);
	w->cl = cl;
	w->cmd = &cmd->cmd;
	rv = nvmeibs_client_add_work(cl, &w->work);
	if (rv) {
		_NT(trace_client_nvmeibs_client_update_gid_change, "Fail to add work");
		kfree(cmd);
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

struct cl_stop_io_on_lgid_workq {
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	union ib_gid lgid;
};

static void cl_stop_io_on_lgid_work(struct workqe_struct *work)
{
	struct cl_stop_io_on_lgid_workq *w =
		container_of(work, struct cl_stop_io_on_lgid_workq, work);
	struct nvmeibs_client *cl = w->cl;
	int dying = !cl->net || atomic_read(&cl->net->dying);
	NFIN;

	if (!dying) {
		_NT(trace_client_cl_stop_io_on_lgid_work, "cl @CL_NAME, stop io channels using lgid @LGID_IPV6", cl->name, &w->lgid);
		stop_io_channels_on_lgid(cl, &w->lgid);
	}
	kfree(w);

	NFOUT;
}

int nvmeibs_client_stop_io_on_lgid(struct nvmeibs_client *cl,
	struct nvmeibs_ib_port *ib_port)
{
	struct cl_stop_io_on_lgid_workq *w;
	int rv;
	NFIN;

	if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		_NE(error_client_nvmeibs_client_stop_io_on_lgid, "Fail to alloc work");
		rv = -ENOMEM;
		goto out;
	}

	WQ_INIT_WORK(&w->work, cl_stop_io_on_lgid_work);
	w->cl = cl;
	w->lgid = ib_port->gid.gid;


	rv = nvmeibs_client_add_work(cl, &w->work);
	if (rv) {
		_NT(trace_client_nvmeibs_client_stop_io_on_lgid, "Fail to add work");
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

static int rionic_check_iopath_ka(struct nvmeibs_rionic *rionic)
{
	struct nvmeibs_rionic_ka *rionic_ka = &rionic->keep_alive;
	u64 curr_ka;
	int rv = 0;
	unsigned long ka_int_jif;

	NFIN;
	mutex_lock(&rionic_ka->lock);
	if (rionic_ka->n_ioch == 0) {
		/* No channels connected - dec and return */
		rv = -ENOENT;
		goto out;
	}

	/* JH IOMMU: DMA_FROM_DEVICE is correct, used as a sync for Remote RDMA_WRITE */
	ib_dma_sync_single_for_cpu(P2IB(rionic_ka->ib_port),
							   rionic_ka->ka_ctr_phys_addr, sizeof(*rionic_ka->ka_ctr), DMA_FROM_DEVICE);

	curr_ka = *rionic_ka->ka_ctr;
	if (curr_ka != rionic_ka->last_ka_ctr) {
		/* Was updated, reset interval */
		_ND(trace_client_rionic_check_iopath_ka, "IO keep-alive ctr @LAST_KA_CTR -> @CURR_KA on path @GID_IPV6 -> @GID_IPV6",
		   rionic_ka->last_ka_ctr, curr_ka, &rionic->lionic->gid, &rionic->gid);
		rionic_ka->last_ka_ctr = curr_ka;
		rionic_ka->last_ka_update_jif = jiffies;
		goto out;
	}

	/* Not updated, check interval */
	ka_int_jif = jiffies - rionic_ka->last_ka_update_jif;
	if (ka_int_jif > nvmeibs_ioka_timeout_sec * HZ) {
		_NT(trace_1_client_rionic_check_iopath_ka, "Detected keep-alive timeout of @KA_TIMEOUT ms on path @GID_IPV6 -> @GID_IPV6, "
												   "reinit last-update-jif (next check @INT sec from now)",
		   1000 * ka_int_jif / HZ, &rionic->lionic->gid, &rionic->gid,
			nvmeibs_ioka_timeout_sec);
		/* reinit timeout, otherwise....
		   in case client reconnects new channels on this path such that
		   rionic_ka->n_ioch doesn't manage to reach 0, if we reach here
		   again in 1sec from now before client managed to send io-ka on
		   these new channels --> we will falsely detect a io-ka-timeout
		   again and disconnect these new channels --> loop */
		rionic_ka->last_ka_update_jif = jiffies;
		rv = -ETIMEDOUT;
	}

out:
	mutex_unlock(&rionic_ka->lock);
	NFOUT;
	return rv;
}

static int disconnect_rionic_iochs(struct nvmeibs_rionic *rionic)
{
	struct nvmeibs_nr_channel *nrch;
	struct nvmeibs_lionic *lionic = rionic->lionic;
	int i, rv = 0;

	_NT(trace_client_disconnect_rionic_iochs, "disconnecting all io channels on path @GID_IPV6 -> @GID_IPV6",
		&lionic->gid, &rionic->gid);

	/* No-RDDA */
	if (rionic->nr_channels) {
		for (i = 0; i < rionic->n_nr_channels; i++) {
			nrch = &rionic->nr_channels[i];
			if (nrch && nrch->id != -1) {
				nvmeibs_net_release(nrch->net, NVMEIBS_LOGOUT_REASON_IO_CH_DISCONNECTION);
				rv++;
			}
		}
	}
	return rv;
}

struct check_all_io_paths_workqe
{
	struct workqe_struct work;
	struct nvmeibs_client *cl;
};

/* check keep-alive on all io-paths */
static void check_all_io_paths_ka_work(struct workqe_struct *work)
{
	struct check_all_io_paths_workqe *w = container_of(
		work, struct check_all_io_paths_workqe, work);
	struct nvmeibs_client *cl = w->cl;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_lionic *lionic;
	struct nvmeibs_rionic *rionic;
	int i, j, rv = 0;
	__NFIN;

	if (atomic_read(&cl->dying)) {
		_NT(trace_client_check_all_io_paths_ka_work, "client @CL_NAME is dying", cl->name);
		goto out;
	}
	list_for_each_entry(cdisk, &cl->disks, link) {
		if (cdisk->lionics) {
			for (i = 0; i < cdisk->n_lionics; ++i) {
				lionic = &cdisk->lionics[i];
				if (!lionic->may_access)
					continue;
				if (!lionic->rionics)
					continue;
				for (j = 0; j < lionic->n_rionics; j++) {
					rionic = &lionic->rionics[j];
					if (!rionic->may_access)
						continue;
					if ((rv = rionic_check_iopath_ka(rionic)) == -ETIMEDOUT)
						disconnect_rionic_iochs(rionic);
				}
			}
		}
	}
out:
	kfree(w);
	__NFOUT;
}

int nvmeibs_client_check_all_io_paths_ka(struct nvmeibs_client *cl)
{
	struct check_all_io_paths_workqe *w;
	int rv;
	NFIN;

	if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		_NE(error_client_nvmeibs_client_check_all_io_paths_ka, "Fail to alloc work");
		rv = -ENOMEM;
		goto out;
	}

	WQ_INIT_WORK(&w->work, check_all_io_paths_ka_work);
	w->cl = cl;

	rv = nvmeibs_client_add_work(cl, &w->work);
	if (rv) {
		_NT(trace_client_nvmeibs_client_check_all_io_paths_ka, "Fail to add work");
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

struct cl_disconnect_io_path_workq
{
	struct workqe_struct work;
	struct nvmeibs_client *cl;
	struct nvmeibs_rionic *rionic;
};

static void cl_disconnect_io_path_work(struct workqe_struct *work)
{
	struct cl_disconnect_io_path_workq *io_work =
		container_of(work, struct cl_disconnect_io_path_workq, work);
	struct nvmeibs_client *cl = io_work->cl;
	struct nvmeibs_rionic *srch_rionic = io_work->rionic, *rionic;
	struct nvmeibs_lionic *lionic;
	struct nvmeibs_client_disk *cdisk;
	int i, j;

	__NFIN;
	list_for_each_entry(cdisk, &cl->disks, link) {
		if (!cdisk->lionics)
			continue;
		for (i = 0; i < cdisk->n_lionics; ++i) {
			lionic = &cdisk->lionics[i];
			if (!lionic->rionics)
				continue;
			for (j = 0; j < lionic->n_rionics; j++) {
				rionic = &lionic->rionics[j];
				if (rionic == srch_rionic) {
					/* when will care more about io channel logout we can take it as param */
					disconnect_rionic_iochs(rionic);
					goto out;
				}
			}
		}
	}
	goto out;

out:
	__NFOUT;
	kfree(io_work);
}

int nvmeibs_client_disconnect_io_path(struct nvmeibs_client *cl,
									  struct nvmeibs_rionic *rionic)
{
	struct cl_disconnect_io_path_workq *w;
	int rv;
	NFIN;

	if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		_NE(error_client_nvmeibs_client_disconnect_io_path, "Fail to alloc work");
		rv = -ENOMEM;
		goto out;
	}

	WQ_INIT_WORK(&w->work, cl_disconnect_io_path_work);
	w->cl = cl;
	w->rionic = rionic;

	rv = nvmeibs_client_add_work(cl, &w->work);
	if (rv) {
		_NT(trace_client_nvmeibs_client_disconnect_io_path, "Fail to add work");
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

static int alloc_rionic_ka(struct nvmeibs_rionic *rionic,
						  struct nvmeibs_ib_port *ib_port)
{
	struct nvmeibs_rionic_ka *rionic_ka = &rionic->keep_alive;
	int rv = 0;

	NFIN;
	if (NVMEIB_USE_MR_FOR_IO_KA) {
		rionic_ka->ka_map.pd = P2NV(ib_port)->pd;
		rionic_ka->ka_map.n_pages = 1;
		rionic_ka->ka_map.ioaddr = 0;
		rionic_ka->ka_map.access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE;
		rionic_ka->ka_map.pages = NULL;
		/* JH IOMMU: DMA_FROM_DEVICE is correct. IO KA is sink for Remote RDMA Write */
		rionic_ka->ka_map.dma_dir = DMA_FROM_DEVICE;
		if ((rv = nvmeib_mem_alloc_n_map(&rionic_ka->ka_map)) < 0) {
			_NW(warn_client_alloc_rionic_ka, "Failed (@RV) to allocate memory for io keep-alive", rv);
			rv = -ENOMEM;
			goto out;
		}
		rionic_ka->ka_ctr = sg_virt(&rionic_ka->ka_map.mem_table.sgl[0]);
		rionic_ka->ka_ctr_phys_addr = sg_dma_address(&rionic_ka->ka_map.mem_table.sgl[0]);
		rionic_ka->ka_ctr_ioaddr = rionic_ka->ka_map.ioaddr;
		rionic_ka->ka_ctr_rkey = rionic_ka->ka_map.rkey;
	} else {
		rionic_ka->ka_ctr = nvmeib_public_ib_dma_alloc_coherent(
				P2IB(ib_port), sizeof(rionic_ka->ka_ctr), &rionic_ka->ka_ctr_phys_addr, GFP_KERNEL);
		if (ib_dma_mapping_error(P2IB(ib_port), rionic_ka->ka_ctr_phys_addr)) {
			_NW(warn_client_alloc_rionic_ka_dma, "Failed to allocate memory for io keep-alive");
			rv = -ENOMEM;
			goto out;
		}
		rionic_ka->ka_ctr_rkey = nvmeib_get_rkey(P2NV(ib_port));
		rionic_ka->ka_ctr_ioaddr = rionic_ka->ka_ctr_phys_addr;
	}

	rionic_ka->ib_port = ib_port;
	*rionic_ka->ka_ctr = (u64)-1;
	rionic_ka->last_ka_ctr = (u64)-1;
	rionic_ka->last_ka_update_jif = jiffies;

out:
	NFOUT;
	return rv;
}

static void free_rionic_ka(struct nvmeibs_rionic *rionic, bool already_locked)
{
	struct nvmeibs_rionic_ka *rionic_ka = &rionic->keep_alive;

	if (!already_locked)
		mutex_lock(&rionic_ka->lock);
	WARN_ON(rionic_ka->n_ioch != 0);
	if (rionic_ka->ka_ctr) {
		if (NVMEIB_USE_MR_FOR_IO_KA) {
			nvmeib_mem_unmapn_n_free(&rionic_ka->ka_map);
		} else {
			nvmeib_public_ib_dma_free_coherent(P2IB(rionic_ka->ib_port), sizeof(rionic_ka->ka_ctr),
				rionic_ka->ka_ctr, rionic_ka->ka_ctr_phys_addr);
			rionic_ka->ka_ctr_phys_addr = 0;
			rionic_ka->ka_ctr_ioaddr = 0;
		}
		rionic_ka->ka_ctr = NULL;
	}
	if (!already_locked)
		mutex_unlock(&rionic_ka->lock);
}

static void set_rionic_ka_dying(struct nvmeibs_rionic *rionic, bool dying)
{
	struct nvmeibs_rionic_ka *rionic_ka = &rionic->keep_alive;
	mutex_lock(&rionic_ka->lock);
	rionic_ka->dying = dying;
	mutex_unlock(&rionic_ka->lock);
}

int nvmeibs_client_rionic_fill_rsp_io_ka(struct nvmeibs_rionic *rionic,
										 struct nvmeibs_ib_port *ib_port,
										 struct nvmeibs_login_response *rsp)
{
	struct nvmeibs_rionic_ka *rionic_ka = &rionic->keep_alive;
	int rv;

	NFIN;
	if (rsp->base.opcode != NVMEIBS_IO_CHANNEL && rsp->base.opcode != NVMEIBS_NORDDA_CHANNEL) {
		_NW(warn_client_nvmeibs_client_rionic_fill_rsp_io_ka, "Invalid response opcode @OPCODE", (int)rsp->base.opcode);
		rv = -EINVAL;
		goto out;
	}

	mutex_lock(&rionic_ka->lock);
	if (rionic_ka->dying) {
		rv = -EBUSY;
		goto unlock;
	}

	if (!rionic_ka->ka_ctr) {
		if ((rv = alloc_rionic_ka(rionic, ib_port)) < 0) {
			goto unlock;
		}
	}

	if (rionic_ka->n_ioch == 0) {
		_NT(trace_0_nvmeibs_client_rionic_fill_rsp_io_ka,
			"IO-KA: path @GID_IPV6 -> @GID_IPV6, reinit last-update-jif "
			"(@JIFFIES --> @JIFFIES)",
			&rionic->lionic->gid, &rionic->gid,
			rionic_ka->last_ka_update_jif, jiffies);
		rionic_ka->last_ka_update_jif = jiffies;
	}
	rionic_ka->n_ioch++;
	if (rsp->base.opcode == NVMEIBS_NORDDA_CHANNEL) {
		rsp->base.nr_rsp.io_ka_rkey = cpu_to_be32(rionic_ka->ka_ctr_rkey);
		rsp->base.nr_rsp.io_ka_raddr = cpu_to_be64(rionic_ka->ka_ctr_ioaddr);
	} else if (rsp->base.opcode == NVMEIBS_IO_CHANNEL) {
		rsp->base.io_rsp.io_ka_rkey = cpu_to_be32(rionic_ka->ka_ctr_rkey);
		rsp->base.io_rsp.io_ka_raddr = cpu_to_be64(rionic_ka->ka_ctr_ioaddr);
	} else {
		BUG_ON(1);
	}

	rv = 0;

unlock:
	mutex_unlock(&rionic_ka->lock);

out:
	NFOUT;
	return rv;
}

void nvmeibs_client_rionic_disconnect_ioch(struct nvmeibs_rionic *rionic, bool io_ka_enabled)
{
	struct nvmeibs_rionic_ka *rionic_ka = &rionic->keep_alive;

	if (io_ka_enabled) {
		mutex_lock(&rionic_ka->lock);
		rionic_ka->n_ioch--;
		BUG_ON(rionic_ka->n_ioch < 0);
		if (rionic_ka->dying && rionic_ka->n_ioch == 0) {
			free_rionic_ka(rionic, true);
		}
		mutex_unlock(&rionic_ka->lock);
	}
}

static void send_ioch_drained(struct nvmeibs_client *cl,
	struct nvmeibs_ioch_drained_cmd *cmd, struct nvmeib_iu *send_ioctx)
{
	struct volume_server_req *req;

	__NFIN;
	req = send_ioctx->buf;
	memcpy(req->i_req.disk_name, cmd->cmd.disk_name,
		NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	memcpy(req->i_req.s_hw_gid, cmd->s_hw_gid.raw, sizeof(req->i_req.s_hw_gid));
	memcpy(req->i_req.c_hw_gid, cmd->c_hw_gid.raw, sizeof(req->i_req.c_hw_gid));
	req->i_req.is_rdda = !!cmd->is_rdda;
	req->i_req.ch_num = cpu_to_be16(cmd->ch_num);
	req->i_req.cs_gid = cpu_to_be64(cmd->cs_gid);
	_NT(trace_0_send_ioch_drained,
		"send IOCH-DRAINED: @CL (@CL_NAME),s=@HW_GID, c=@HW_GID, ch_num=@INT, "
		"rdda=@BOOL, cs_gid=@LLU",
		cl, cl->name, cmd->s_hw_gid.raw, cmd->c_hw_gid.raw, (int)cmd->ch_num,
		cmd->is_rdda, cmd->cs_gid);
	send_resource(cl, &cmd->cmd.info, vex_base, send_ioctx);
	__NFOUT;
}

void nvmeibs_client_send_ioch_drained(struct nvmeibs_client *cl, bool is_rdda,
	struct nvmeibs_rionic *rionic, int ch_num, u64 cs_gid)
{
	bool net_alive = (cl->net && !atomic_read(&cl->net->dying));
	struct nvmeibs_ioch_drained_cmd *cmd = NULL;
	struct nvmeib_iu *send_ioctx;
	NFIN;

	if (!on_wq(cl->wq)) {
		_NE(error_0_nvmeibs_ioch_drained_cmd, "Wrong wq");
		goto out;
	}
	if (!net_alive) {
		_NT(trace_1_nvmeibs_ioch_drained_cmd, "net is dead");
		goto out;
	}
	if (nvmeib_version_protocol_lt(&cl->link_version, &nvmeib_2p1_version)) {
		_NT(trace_3_nvmeibs_ioch_drained_cmd, "omit send to older client");
		goto out;
	}

	if (!(cmd = kzalloc(sizeof(*cmd), GFP_KERNEL))) {
		_NE(error_2_nvmeibs_ioch_drained_cmd, "Fail to alloc cmd");
		goto out;
	}

	cmd->cmd.info.op = NVMEIBS_IOCH_DRAINED;
	cmd->cmd.info.has_rsp = false;
	memcpy(cmd->cmd.disk_name, cl->disk_name,
	       NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE);
	cmd->s_hw_gid = rionic->lionic->gid;
	cmd->c_hw_gid = rionic->gid;
	cmd->is_rdda = is_rdda;
	cmd->ch_num = ch_num;
	cmd->cs_gid = cs_gid;

	if ((send_ioctx = nvmeibs_client_get_ioctx(cl))) {
		send_ioch_drained(cl, cmd, send_ioctx);
	}
	else {
		_NT(trace_4_nvmeibs_ioch_drained_cmd,
		    "pending cmd @PTR, info @PTR", cmd, &cmd->cmd.info);
		spin_lock_irq(&cl->spinlock);
		list_add_tail(&cmd->cmd.info.link, &cl->pending_cmd);
		spin_unlock_irq(&cl->spinlock);
	}

out:
	NFOUT;
}

/*
 * Per device (shared) CQs
 */

static void cl_send_comp_h(void *ctx, struct ib_wc *wcs)
{
	struct nvmeibs_client *cl = ctx;
	NFIN;

	if (cl->dismissed)
		_NT(cl_send_comp_h_t1, "Allow processing of drain-sq wr");
	process_send_completion(cl, wcs);

	NFOUT;
}

static void cl_recv_comp_h(void *ctx, struct ib_wc *wcs)
{
	struct nvmeibs_client *cl = ctx;
	struct nvmeibs_net *net = cl->net;

	NFIN;
	nvmeibs_net_dec_recv(net, 1);
	if (nvmeib_opcode_from_wc(wcs) == NVMEIB_DRAIN_QUEUE) { //TODO: Remove this
		nvmeibs_rq_drain_comp(net, wcs);
	}
	else if (!cl->dismissed)
		process_rcv_completion(cl, wcs);
	else {
		u32 index = nvmeib_idx_from_wc(wcs);
		struct nvmeib_iu *recv_ioctx = get_recv_iu(cl, index);
		if (recv_ioctx)
			post_recv_iu(cl, recv_ioctx);
		else
			_NE(cl_recv_comp_h_e1, "Got null recv_ioctx");
	}
	NFOUT;
}

static void l_send_comp_h(void *ctx, struct ib_wc *wcs)
{
	struct nvmeibs_client *cl = ctx;
	struct nvmeibs_net *net = cl->lock_net;

	NFIN;
	/* check if someone waits for the send completion */
	if (unlikely(nvmeib_opcode_from_wc(wcs) == NVMEIB_DRAIN_QUEUE))
		nvmeibs_net_on_drain_sq(net);
	else
		_NW(l_send_comp_h_w1, "Unexpected wr_id @_X (opcode: @INT, status: @INT) on client @PTR lock channel",
		   wcs->wr_id, wcs->opcode, wcs->status, cl);
	NFOUT;
}

static void l_recv_comp_h(void *ctx, struct ib_wc *wcs)
{
	NFIN;
	WARN_ON(true);
	NFOUT;
}

static void l_2nd_send_comp_h(void *ctx, struct ib_wc *wcs)
{
	struct nvmeibs_2nd_lock_ch *_2nd_ch_ptr = ctx;
	struct nvmeibs_client *cl = _2nd_ch_ptr->cl;
	struct nvmeibs_net *net = _2nd_ch_ptr->net;

	NFIN;
	/* check if someone waits for the send completion */
	if (unlikely(nvmeib_opcode_from_wc(wcs) == NVMEIB_DRAIN_QUEUE))
		nvmeibs_net_on_drain_sq(net);
	else
		_NW(l_2nd_send_comp_h_w1, "Unexpected wr_id @_X (opcode: @INT, status: @INT) on client @PTR lock channel",
		   wcs->wr_id, wcs->opcode, wcs->status, cl);
	NFOUT;
}

static void l_2nd_recv_comp_h(void *ctx, struct ib_wc *wcs)
{
	NFIN;
	WARN_ON(true);
	NFOUT;
}

/* /proc/nvmeibs/sclients/.. related functions */
struct proc_dir_entry *nvmeibs_client_proc_mkdir(struct proc_dir_entry *parent) {
	if (!(sclients_dir = proc_mkdir(SCLIENTS_PROC_DIRNAME, parent))) {
		_NE(nvmeibs_client_proc_mkdir_e_create, "Could not create sclients proc dir");
	}
	return sclients_dir;
}

void nvmeibs_client_proc_umkdir(struct proc_dir_entry *parent) {
	/* safe to call */
	proc_remove(sclients_dir);
	sclients_dir = NULL;
}

static void foreach_nr_chan(struct nvmeibs_client *cl, void (*cb) (struct nvmeibs_nr_channel *, void *), void *arg) {
	int i, j, k, nr_channels_len;
	struct list_head *disks = &cl->disks;
	struct nvmeibs_client_disk *cdisk;
	struct nvmeibs_nr_channel *ch;

	list_for_each_entry(cdisk, disks, link) {
		for (i = 0; i < cdisk->n_lionics; i++) {
			for (j = 0; j < cdisk->lionics[i].n_rionics; j++) {
				nr_channels_len = cdisk->lionics[i].rionics[j].n_nr_channels;
				for (k = 0; k < nr_channels_len; k++ ) {
					ch = &cdisk->lionics[i].rionics[j].nr_channels[k];
					cb(ch, arg);
				}
			}
		}
	}
}

static void fill_qp_stats_per_chan(struct nvmeibs_nr_channel *ch, void *arg) {
	struct nvmeibs_client_proc_work *stats_work = (struct nvmeibs_client_proc_work *) arg;
	int count = *stats_work->count;
	char *buffer = stats_work->buffer;
	size_t len = stats_work->len;
	unsigned long flags;

	count += scnprintf(buffer + count, len - count,
	"%-*s: ", QPS_STATS_PAD_BLANKS_LEN_LINE_NUM + QPS_STATS_PAD_BLANKS_LEN_NAME, ch->name);

	/* net ptr is modified on same ctx as this func ie s-cl-wq */
	if (!ch->net) {
		goto out;
	}
	nvmeibs_net_spin_lock_irqsave(ch->net, &flags);
	if (ch->net->state == QP_LIVE) {
		count += nvmeib_qp_stats_fill(ch->net->qp_stats,
												buffer + count,
												len - count);
	}
	nvmeibs_net_spin_unlock_irqrestore(ch->net, flags);

out:
	count += scnprintf(buffer + count, len - count, "\n");
	*stats_work->count = count;
}

static void fill_qp_stats_work(struct workqe_struct *work)
{
	struct nvmeibs_client_proc_work *stats_work = (struct nvmeibs_client_proc_work *) work;
	struct nvmeibs_client *cl = stats_work->cl;

	foreach_nr_chan(cl, fill_qp_stats_per_chan, stats_work);

	complete(stats_work->comp);
}
#define CORE_SERVER_QP_STATS_PROC_FRMT_VER 1
static ssize_t proc_queue_and_wait(struct nvmeibs_client *cl, char *buffer, size_t len,
	void (*work_func) (struct workqe_struct *))
{
	struct nvmeibs_client_proc_work work;
	DECLARE_COMPLETION_ONSTACK(done);
	int count = 0;

	WQ_INIT_WORK(&work.work, work_func);
	work.cl = cl;
	work.buffer = buffer;
	work.len  = len;
	work.comp = &done;
	work.count = &count;

	if (!nvmeibs_client_add_work(cl, &work.work))
		wait_for_completion(&done);

	count += nvmeib_proc_add_txt_proc_epilog(CORE_SERVER_QP_STATS_PROC_FRMT_VER, buffer + count, len - count);

	return count;
}

static ssize_t fill_qp_stats(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_client *cl = (struct nvmeibs_client *)arg;

	return proc_queue_and_wait(cl, buffer, len, fill_qp_stats_work);	
}

static void reset_qp_stats_per_chan(struct nvmeibs_nr_channel *ch, void *arg) {
	unsigned long flags;

	/* net ptr is modified on same ctx as this func ie s-cl-wq */
	if (!ch->net) {
		return;
	}
	nvmeibs_net_spin_lock_irqsave(ch->net, &flags);
	if (ch->net->state == QP_LIVE)
		nvmeib_qp_stats_reset(ch->net->qp_stats);
	nvmeibs_net_spin_unlock_irqrestore(ch->net, flags);
}

static void reset_qp_stats_work(struct workqe_struct *work)
{
	struct nvmeibs_client_proc_work *stats_work = (struct nvmeibs_client_proc_work *) work;
	struct nvmeibs_client *cl = stats_work->cl;

	foreach_nr_chan(cl, reset_qp_stats_per_chan, NULL);

	*stats_work->count = stats_work->len;
	complete(stats_work->comp);
}

static ssize_t reset_qp_stats(void *arg, char *buf, size_t len) {
	struct nvmeibs_client *cl = (struct nvmeibs_client *)arg;
	int reset;

	if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
		return -EINVAL;
	}

	return proc_queue_and_wait(cl, buf, len, reset_qp_stats_work);
}


/* should be call only after cl->name was set */
static int single_sclient_proc_mkdir(struct nvmeibs_client *cl)
{
	int rv;

	if (atomic_read(&cl->dying)) {
		_NT(single_sclient_proc_mkdir_dying, "Client @CL(@STR) already dying - not adding procs", cl, cl->name);
		rv = 0;
		goto out;
	}

	if (!(cl->proc_dir = proc_mkdir(cl->name, sclients_dir))) {
		_NE(cl_disk_get_failed_proc_dir, "could not create sclient proc for @STR", cl->name);
		rv = -ENOMEM;
		goto out;
	}

	if (!(cl->qp_stats = nvmeib_public_proc_create(
		"qp_stats", cl->proc_dir, &fill_qp_stats, &reset_qp_stats, cl))) {
		_NE(cl_disk_get_failed_proc_qp_stats, "could not create qp_stats proc for @STR", cl->name);
		rv = -ENOMEM;
		goto out;
	}
	rv = 0;

out:
	return rv;
}


static void single_sclient_proc_mkdir_work(struct workqe_struct *work)
{
	struct cl_external_workq *ew =
		container_of(work, struct cl_external_workq, work);

	if (single_sclient_proc_mkdir(ew->cl))
		nvmeibs_net_release(ew->cl->net, NVMEIBS_LOGOUT_REASON_PROCFS_ERR);

	kfree(work);
}

static void single_sclient_proc_umkdir(struct nvmeibs_client *cl)
{
	if (cl->qp_stats) {
		nvmeib_public_proc_remove(cl->qp_stats);
	}
	proc_remove(cl->proc_dir);
	cl->proc_dir = NULL;
}
