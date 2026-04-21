/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/*
 * nvmeibc_tpv_ib_admin.c - Real IB admin channel for CDV extent operations.
 *
 * Implements the three CDV admin functions declared as externs in
 * nvmeibc_tpv_allocator.c and nvmeibc_tpv_recovery.c:
 *
 *   nvmeibc_ib_admin_cdv_alloc_extent()  - request one CDV_extent from TOMA
 *   nvmeibc_ib_admin_cdv_free_extent()   - return one CDV_extent to TOMA
 *   nvmeibc_ib_admin_cdv_list_extents()  - list CDV_extents owned by a TPV
 *
 * Send path:
 *   Finds the CDV disk segment whose TOMA hostname matches the elected
 *   allocator toma_id (falls back to first active segment if none match),
 *   builds a nvmeibt_client_msg with the CDV request as thick.data[], and
 *   sends via icore_ops->toma_send().
 *
 * Receive path (ALLOC and LIST only):
 *   The TOMA response arrives in nvmeibc_topology.c's message dispatch, which
 *   calls nvmeibc_cdv_dispatch_alloc_response() /
 *   nvmeibc_cdv_dispatch_list_response().  These match the response to a
 *   pending request by req_id and complete the waiter.
 *
 * Test hooks:
 *   nvmeibc_tpv_test_cdv_alloc_fn, _free_fn, _list_fn are NULL in production.
 *   nvmeibc_tpv_test.c sets them before self-tests and clears them afterward,
 *   mirroring the pattern used by nvmeibc_tpv_cdv_test_sync_read_fn.
 */

#include "common/kr_incs.h"
#include "nvmeibc_tpv.h"
#include "clnt/nvmeibc_volume.h"
#include "clnt/nvmeibc_msgs_shared.h"
#include "block/nvmeibc_block_common.h"
#include "block/nvmeibc_topology.h"
#include "nvmeibc_icore_ops.h"

/* -- Forward declarations (prototypes required by -Wmissing-prototypes) -- */

int nvmeibc_ib_admin_cdv_alloc_extent(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_alloc_req *req,
	struct nvmeibc_cdv_alloc_resp *resp);

int nvmeibc_ib_admin_cdv_free_extent(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_free_req *req);

int nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
				       const char *toma_id,
				       const char *tpv_uuid,
				       u64 **out_indices,
				       u64 *out_count);

/* -- Test hook pointers ---------------------------------------------------- */

int (*nvmeibc_tpv_test_cdv_alloc_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_alloc_req *req,
	struct nvmeibc_cdv_alloc_resp *resp);
EXPORT_SYMBOL(nvmeibc_tpv_test_cdv_alloc_fn);

int (*nvmeibc_tpv_test_cdv_free_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const struct nvmeibc_cdv_free_req *req);
EXPORT_SYMBOL(nvmeibc_tpv_test_cdv_free_fn);

int (*nvmeibc_tpv_test_cdv_list_fn)(
	struct nvmeibc_volume *cdv, const char *toma_id,
	const char *tpv_uuid, u64 **out_indices, u64 *out_count);
EXPORT_SYMBOL(nvmeibc_tpv_test_cdv_list_fn);

/* -- Pending-request tracking -------------------------------------------- */

#define CDV_ADMIN_TIMEOUT_SECS 30

struct nvmeibc_cdv_pending_req {
	u64                req_id;
	struct completion  done;
	/* Response data filled by dispatch functions */
	u8                 resp_status;
	u64                resp_extent_index;
	u64                resp_allocator_generation;
	/* For LIST: index array + count */
	u64               *list_indices;
	u64                list_count;
	struct list_head   node;
};

static DEFINE_SPINLOCK(cdv_pending_lock);
static LIST_HEAD(cdv_pending_list);
static atomic64_t nvmeibc_tpv_req_id_counter = ATOMIC64_INIT(0);

/* -- Segment lookup ------------------------------------------------------
 *
 * Find the CDV volume's active disk segment for the elected allocator TOMA.
 * CDV is JBOD: single chunk, single RAID-1, one or two segments.
 *
 * When toma_id is non-empty, prefer a segment whose disk hostname matches.
 * This ensures CDV_ALLOC_EXTENT/FREE/LIST go to the elected allocator TOMA
 * rather than whichever segment happens to be first - on a RAID-1 CDV the
 * two segments live on different TOMA nodes, and routing to the wrong one
 * causes the not-allocator Path 4 WRONG_GEN loop.
 *
 * Falls back to the first active segment if none match toma_id (e.g. during
 * initial attach before the topology push has arrived).
 */
static struct nvmeibc_disk_segment *cdv_find_segment_for_toma(
	struct nvmeibc_volume *cdv, const char *toma_id)
{
	struct nvmeibc_block_device *bdev;
	struct nvmeibc_topologies *nt;
	struct nvmeibc_topology *t;
	struct nvmeibc_disk_segment *seg, *fallback = NULL;
	int si;

	if (!cdv || !cdv->block_dev)
		return NULL;

	bdev = cdv->block_dev;
	nt = &bdev->topologies;

	if (list_empty(&nt->topologies))
		return NULL;

	t = list_first_entry(&nt->topologies, struct nvmeibc_topology, list_n);
	if (!t || t->nchunks < 1 || !t->chunks || !t->chunks[0].raid1s)
		return NULL;

	raid1_for_each_seg(&t->chunks[0].raid1s[0], seg, si) {
		if (!is_seg_active(*seg) || !seg->toma_reg ||
		    !is_toma_reg_valid(seg->toma_reg))
			continue;
		if (!fallback)
			fallback = seg;
		if (toma_id && toma_id[0] && seg->disk) {
			const char *host = seg->disk->disk_host;

			if (host && strncmp(host, toma_id, NVMEIB_HOST_NAME_LEN) == 0)
				return seg;
		}
	}

	if (fallback && toma_id && toma_id[0])
		_NW(cdv_seg_toma_mismatch,
		    "CDV: no segment for elected allocator toma=@STR; using fallback",
		    toma_id);
	return fallback;
}

/* -- TOMA send with CDV payload ------------------------------------------
 *
 * Builds a nvmeibt_client_msg with msg_type and the CDV request struct
 * as thick.data[], then sends via icore_ops->toma_send().
 *
 * Unlike nvmeibc_toma_send_direct_msg which hardcodes sizeof(client_msg_pl)
 * as the payload, this passes the raw CDV struct so TOMA receives it directly
 * in msg->msg_data.
 */
static int cdv_toma_send(struct nvmeibc_disk_segment *seg,
			 enum NVMEIBT_CLIENT_MSG_TYPES msg_type,
			 const void *payload, u32 payload_size)
{
	struct nvmeibc_icore_ops const *ops = nvmeibc_core_ops_get();
	struct nvmeibc_disk_toma_send_params env = {0};
	struct nvmeibc_raid1 *r1;
	struct nvmeibc_topology *t;
	struct nvmeibt_client_msg *msg;
	u32 total_size;
	int rv;

	if (!seg || !seg->toma_reg || !is_toma_reg_valid(seg->toma_reg))
		return -ENODEV;

	r1 = nvmeibc_get_raid1_of_seg(seg);
	t = seg->chunk->topology;

	total_size = sizeof(*msg) + payload_size;
	msg = kzalloc(total_size, GFP_NOIO);
	if (!msg)
		return -ENOMEM;

	nvmeibt_client_thick_msg_write(msg, msg_type,
		NVMEIBT_CLIENT_RT_REASON_DIRECT,
		nvmeib_get_utsname_nodename(),
		seg->toma_reg->protocol_version,
		0x35003500, /* config_version placeholder */
		t->configuration_version,
		t->topology_version,
		r1->version,
		seg->uuid, r1->lid.all,
		t->nt->reservation_version_max_seen,
		r1->toma.conversation_ind,
		false, /* never_reged_on_seg */
		nvmeibc_block_is_hidden(t->nt->nd),
		payload_size, (void *)payload,
		nvmeib_get_guid());

	env.buf = (u8 *)msg;
	env.len_toma = total_size;
	env.len_srvr = total_size;
	env.send_comp_cb = NULL;
	env.recv_rsp_cb = NULL;
	env.arg = NULL;

	rv = ops->toma_send(ops, seg->disk, seg->toma_reg->handle, &env);

	kfree(msg);
	return rv;
}

/* -- Response dispatch (called from nvmeibc_topology.c) ---------------- */

void nvmeibc_cdv_dispatch_alloc_response(const struct nvmeibc_cdv_alloc_resp *resp)
{
	struct nvmeibc_cdv_pending_req *pending;
	unsigned long flags;

	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_for_each_entry(pending, &cdv_pending_list, node) {
		if (pending->req_id == resp->req_id) {
			pending->resp_status = resp->status;
			pending->resp_extent_index = resp->extent_index;
			pending->resp_allocator_generation = resp->allocator_generation;
			/*
			 * complete() must be called inside the lock.  If we
			 * unlock first, a concurrent timeout in the waiter can
			 * run list_del() and return (freeing the stack frame)
			 * before complete() touches pending->done - UAF.
			 */
			complete(&pending->done);
			spin_unlock_irqrestore(&cdv_pending_lock, flags);
			return;
		}
	}
	spin_unlock_irqrestore(&cdv_pending_lock, flags);

	_NW(cdv_alloc_rsp_orphan,
	    "CDV: ALLOC_RSP req_id=@LLU has no pending waiter; dropped",
	    resp->req_id);
}
EXPORT_SYMBOL(nvmeibc_cdv_dispatch_alloc_response);

void nvmeibc_cdv_dispatch_list_response(const struct nvmeibc_cdv_list_resp *resp,
					const u64 *indices, u64 n_indices)
{
	struct nvmeibc_cdv_pending_req *pending;
	unsigned long flags;

	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_for_each_entry(pending, &cdv_pending_list, node) {
		if (pending->req_id == resp->req_id) {
			pending->resp_status = resp->status;
			pending->list_count = n_indices;
			if (n_indices > 0 && indices) {
				pending->list_indices = kvmalloc_array(
					n_indices, sizeof(u64), GFP_ATOMIC);
				if (pending->list_indices)
					memcpy(pending->list_indices, indices,
					       n_indices * sizeof(u64));
				else
					pending->list_count = 0;
			}
			/* complete() inside the lock - same reason as in
			 * nvmeibc_cdv_dispatch_alloc_response(). */
			complete(&pending->done);
			spin_unlock_irqrestore(&cdv_pending_lock, flags);
			return;
		}
	}
	spin_unlock_irqrestore(&cdv_pending_lock, flags);

	_NW(cdv_list_rsp_orphan,
	    "CDV: LIST_RSP req_id=@LLU has no pending waiter; dropped",
	    resp->req_id);
}
EXPORT_SYMBOL(nvmeibc_cdv_dispatch_list_response);

/* -- CDV_ALLOC_EXTENT ---------------------------------------------------- */

int nvmeibc_ib_admin_cdv_alloc_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_alloc_req   *req,
	struct nvmeibc_cdv_alloc_resp        *resp)
{
	struct nvmeibc_cdv_pending_req pending;
	struct nvmeibc_disk_segment *seg;
	unsigned long flags, remaining;
	int rv;

	/* Test hook */
	if (unlikely(nvmeibc_tpv_test_cdv_alloc_fn))
		return nvmeibc_tpv_test_cdv_alloc_fn(cdv, toma_id, req, resp);

	seg = cdv_find_segment_for_toma(cdv, toma_id);
	if (!seg) {
		_NE(cdv_alloc_no_seg,
		    "CDV: ALLOC no active segment for CDV uuid=@STR",
		    cdv->hdr.uuid);
		return -ENODEV;
	}

	/* Set up pending request */
	memset(&pending, 0, sizeof(pending));
	pending.req_id = req->req_id;
	init_completion(&pending.done);
	INIT_LIST_HEAD(&pending.node);

	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_add_tail(&pending.node, &cdv_pending_list);
	spin_unlock_irqrestore(&cdv_pending_lock, flags);

	/* Send the request to TOMA */
	rv = cdv_toma_send(seg, NVMEIBT_CLIENT_MSG_RT_CDV_ALLOC_EXTENT,
			   req, sizeof(*req));
	if (rv) {
		_NE(cdv_alloc_send_fail,
		    "CDV: ALLOC send failed rv=@INT", rv);
		goto out_remove;
	}

	/* Wait for TOMA response */
	remaining = wait_for_completion_timeout(
		&pending.done, CDV_ADMIN_TIMEOUT_SECS * HZ);

	/*
	 * Always remove from the pending list before returning.
	 *
	 * The success path must also call list_del: the pending entry lives
	 * on our stack, and leaving it in the list after we return causes
	 * dispatch_alloc_response to walk stale memory the next time a
	 * response arrives - UAF and spinlock corruption.
	 *
	 * complete() is called inside cdv_pending_lock in the dispatch
	 * functions, so by the time wait_for_completion_timeout() returns
	 * the dispatch is done touching pending.done.  It is safe to
	 * list_del here without waiting further.
	 */
	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_del(&pending.node);
	spin_unlock_irqrestore(&cdv_pending_lock, flags);

	if (!remaining) {
		_NE(cdv_alloc_timeout,
		    "CDV: ALLOC timeout after @INT seconds for req_id=@LLU",
		    CDV_ADMIN_TIMEOUT_SECS, req->req_id);
		return -ETIMEDOUT;
	}

	/* Copy response */
	memset(resp, 0, sizeof(*resp));
	resp->req_id = req->req_id;
	resp->status = pending.resp_status;
	resp->extent_index = pending.resp_extent_index;
	resp->allocator_generation = pending.resp_allocator_generation;
	return 0;

out_remove:
	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_del(&pending.node);
	spin_unlock_irqrestore(&cdv_pending_lock, flags);
	return rv;
}
EXPORT_SYMBOL(nvmeibc_ib_admin_cdv_alloc_extent);

/* -- CDV_FREE_EXTENT (fire-and-forget) ----------------------------------- */

int nvmeibc_ib_admin_cdv_free_extent(
	struct nvmeibc_volume                *cdv,
	const char                           *toma_id,
	const struct nvmeibc_cdv_free_req    *req)
{
	struct nvmeibc_disk_segment *seg;
	int rv;

	/* Test hook */
	if (unlikely(nvmeibc_tpv_test_cdv_free_fn))
		return nvmeibc_tpv_test_cdv_free_fn(cdv, toma_id, req);

	seg = cdv_find_segment_for_toma(cdv, toma_id);
	if (!seg) {
		_NE(cdv_free_no_seg,
		    "CDV: FREE no active segment for CDV uuid=@STR",
		    cdv->hdr.uuid);
		return -ENODEV;
	}

	rv = cdv_toma_send(seg, NVMEIBT_CLIENT_MSG_RT_CDV_FREE_EXTENT,
			   req, sizeof(*req));
	if (rv)
		_NE(cdv_free_send_fail,
		    "CDV: FREE send failed rv=@INT", rv);

	return rv;
}
EXPORT_SYMBOL(nvmeibc_ib_admin_cdv_free_extent);

/* -- CDV_LIST_EXTENTS ---------------------------------------------------- */

/*
 * Matches the existing recovery caller signature:
 *   nvmeibc_ib_admin_cdv_list_extents(cdv, toma_id, tpv_uuid,
 *                                     &out_indices, &out_count)
 *
 * On success, *out_indices is set to a vmalloc'd array of *out_count u64
 * extent indices; the caller must vfree(*out_indices).
 */
int nvmeibc_ib_admin_cdv_list_extents(struct nvmeibc_volume *cdv,
				       const char *toma_id,
				       const char *tpv_uuid,
				       u64 **out_indices,
				       u64 *out_count)
{
	struct nvmeibc_cdv_pending_req pending;
	struct nvmeibc_cdv_list_req req;
	struct nvmeibc_disk_segment *seg;
	unsigned long flags, remaining;
	int rv;

	*out_indices = NULL;
	*out_count   = 0;

	if (unlikely(nvmeibc_tpv_test_cdv_list_fn))
		return nvmeibc_tpv_test_cdv_list_fn(cdv, toma_id, tpv_uuid,
						    out_indices, out_count);

	seg = cdv_find_segment_for_toma(cdv, toma_id);
	if (!seg)
		return -ENODEV;

	memset(&req, 0, sizeof(req));
	strncpy(req.tpv_uuid, tpv_uuid, sizeof(req.tpv_uuid) - 1);
	strncpy(req.cdv_uuid, cdv->hdr.uuid, sizeof(req.cdv_uuid) - 1);
	req.req_id = (u64)atomic64_inc_return(&nvmeibc_tpv_req_id_counter);

	memset(&pending, 0, sizeof(pending));
	pending.req_id = req.req_id;
	init_completion(&pending.done);
	INIT_LIST_HEAD(&pending.node);

	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_add_tail(&pending.node, &cdv_pending_list);
	spin_unlock_irqrestore(&cdv_pending_lock, flags);

	rv = cdv_toma_send(seg, NVMEIBT_CLIENT_MSG_RT_CDV_LIST_EXTENTS,
			   &req, sizeof(req));
	if (rv)
		goto out_remove;

	remaining = wait_for_completion_timeout(
		&pending.done, CDV_ADMIN_TIMEOUT_SECS * HZ);

	/* Always remove from list before returning - see alloc_extent for the
	 * full explanation of why the success path also needs list_del. */
	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_del(&pending.node);
	spin_unlock_irqrestore(&cdv_pending_lock, flags);

	if (!remaining) {
		kvfree(pending.list_indices);
		return -ETIMEDOUT;
	}

	/* Check TOMA response status (non-zero = error / not ready). */
	if (pending.resp_status != 0) {
		kvfree(pending.list_indices);
		return -EAGAIN;
	}

	/* Transfer ownership of the vmalloc'd index array to the caller. */
	*out_indices = pending.list_indices;
	*out_count   = pending.list_count;
	return 0;

out_remove:
	spin_lock_irqsave(&cdv_pending_lock, flags);
	list_del(&pending.node);
	spin_unlock_irqrestore(&cdv_pending_lock, flags);
	kvfree(pending.list_indices);
	return rv;
}
EXPORT_SYMBOL(nvmeibc_ib_admin_cdv_list_extents);
