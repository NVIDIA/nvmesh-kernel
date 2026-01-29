/*
 * Software iWARP library for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#if HAVE_CONFIG_H
#  include <siw_config.h>
#endif	/* HAVE_CONFIG_H */

/* From rdma-core for IBVERBS_PABI_VERSION/PROVIDER_DRIVER() macro */
#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <net/if.h>
#include <pthread.h>

#include "siw.h"
#include "siw_abi.h"
#include "../../common/siw_user.h"


int rdma_db_nr = -1;
extern const int siw_debug;

static void siw_free_context(struct ibv_context *ibv_ctx);

static struct verbs_context_ops siw_context_ops = {
#if !QUERY_DEVICE_IN_CTX_OPS
	.query_device_ex = siw_query_device_ex,
#else
	.query_device	= siw_query_device,
#endif
#if FREE_CONTEXT_IN_CTX_OPS
	.free_context = siw_free_context,
#endif
	.query_port	= siw_query_port,
	.query_qp       = siw_query_qp,
	.alloc_pd	= siw_alloc_pd,
	.dealloc_pd	= siw_free_pd,
	.reg_mr		= siw_reg_mr,
	.dereg_mr	= siw_dereg_mr,
	.create_cq	= siw_create_cq,
	.resize_cq	= siw_resize_cq,
	.destroy_cq	= siw_destroy_cq,
	.create_srq	= siw_create_srq,
	.modify_srq	= siw_modify_srq,
	.destroy_srq	= siw_destroy_srq,
	.create_qp	= siw_create_qp,
	.modify_qp	= siw_modify_qp,
	.destroy_qp	= siw_destroy_qp,
	.create_ah	= siw_create_ah,
	.destroy_ah	= siw_destroy_ah,
	.attach_mcast	= NULL,
	.detach_mcast	= NULL,
	.req_notify_cq	= siw_notify_cq,
	.async_event = siw_async_event,
	.post_send = siw_post_send_ofed,
	.post_recv = siw_post_recv_ofed,
	.post_srq_recv = siw_post_srq_recv_ofed,
	.poll_cq = siw_poll_cq_ofed,
};

static struct verbs_context_ops siw_context_ops_mapped = {
#if !QUERY_DEVICE_IN_CTX_OPS
	.query_device_ex = siw_query_device_ex,
#else
	.query_device	= siw_query_device,
#endif
#if FREE_CONTEXT_IN_CTX_OPS
	.free_context = siw_free_context,
#endif
	.query_port	= siw_query_port,
	.query_qp       = siw_query_qp,
	.alloc_pd	= siw_alloc_pd,
	.dealloc_pd	= siw_free_pd,
	.reg_mr		= siw_reg_mr,
	.dereg_mr	= siw_dereg_mr,
	.create_cq	= siw_create_cq,
	.resize_cq	= siw_resize_cq,
	.destroy_cq	= siw_destroy_cq,
	.create_srq	= siw_create_srq,
	.modify_srq	= siw_modify_srq,
	.destroy_srq	= siw_destroy_srq,
	.create_qp	= siw_create_qp,
	.modify_qp	= siw_modify_qp,
	.destroy_qp	= siw_destroy_qp,
	.create_ah	= siw_create_ah,
	.destroy_ah	= siw_destroy_ah,
	.attach_mcast	= NULL,
	.detach_mcast	= NULL,
	.req_notify_cq	= siw_notify_cq,
	.async_event = siw_async_event,
	.post_send = siw_post_send_mapped,
	.post_recv = siw_post_recv_mapped,
	.post_srq_recv = siw_post_srq_recv_mapped,
	.poll_cq = siw_poll_cq_mapped,
};

static struct verbs_context *siw_alloc_context(struct ibv_device *ofa_dev, int fd, void *private_data)
{
	struct siw_context *context;
	struct ibv_get_context cmd;
	struct siw_alloc_ucontext_resp resp;
	struct siw_device *siw_dev = dev_ofa2siw(ofa_dev);
	
	context = verbs_init_and_alloc_context(ofa_dev, fd, context, ofa_ctx,
					       RDMA_DRIVER_SIW_XLRO);
	if (!context)
		return NULL;

	if (ibv_cmd_get_context(&context->ofa_ctx, &cmd, sizeof cmd,
				&resp.ibv_resp, sizeof resp)) {
		verbs_uninit_context(&context->ofa_ctx);
		free(context);
		return NULL;
	}

	/*
	 * here we take the chance to put in two versions of fast path
	 * operations: private or via OFED 
	 */
	switch (siw_dev->if_type) {
		case SIW_IF_OFED:
			verbs_set_ops(&context->ofa_ctx, &siw_context_ops);
			break;
			
		case SIW_IF_MAPPED:
			verbs_set_ops(&context->ofa_ctx, &siw_context_ops_mapped);
			break;
		default:
			printf("SIW IF type %d not supported\n", siw_dev->if_type);
			verbs_uninit_context(&context->ofa_ctx);
			free(context);
			return NULL;
	}
	context->dev_id = resp.drv_payload.dev_id;
	rdma_db_nr = resp.drv_payload.rdma_db_nr;
	return &context->ofa_ctx;
}

static void siw_free_context(struct ibv_context *ibv_ctx)
{
	struct siw_context *ctx = ctx_ofa2siw(ibv_ctx);
	
	verbs_uninit_context(&ctx->ofa_ctx);
	free(ctx);
}

static struct verbs_device *siw_device_alloc(struct verbs_sysfs_dev *sysfs_dev)
{
	struct siw_device *dev;
	char value[32];
	int if_type;

	if (ibv_read_sysfs_file(sysfs_dev->ibdev_path, "if_type", value,
		sizeof(value)) < 0)
		return NULL;

	sscanf(value, "%i", &if_type);

	if (if_type != SIW_IF_OFED && if_type != SIW_IF_MAPPED)
		return NULL;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	pthread_spin_init(&dev->lock, PTHREAD_PROCESS_PRIVATE);
	dev->if_type = if_type;

	return &dev->ofa_dev;
}

static void siw_device_free(struct verbs_device *vdev)
{
	struct siw_device *dev =
		container_of(vdev, struct siw_device, ofa_dev);
	free(dev);
}

static const struct verbs_match_ent rnic_table[] = {
	/* FIXME: rxe needs a more reliable way to detect the rxe device */
	VERBS_NAME_MATCH("siw", NULL),
	{},
};

static const struct verbs_device_ops siw_dev_ops = {
	.name = "siw",
	.match_min_abi_version = VERSION_ID_SOFTIWARP,
	.match_max_abi_version = VERSION_ID_SOFTIWARP,
	.match_table = rnic_table,
	.alloc_device = siw_device_alloc,
	.uninit_device = siw_device_free,
	.alloc_context = siw_alloc_context,
#if !FREE_CONTEXT_IN_CTX_OPS
	.free_context = siw_free_context,
#endif
};

PROVIDER_DRIVER(siw, siw_dev_ops);

