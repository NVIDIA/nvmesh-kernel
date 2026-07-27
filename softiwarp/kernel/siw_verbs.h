/*
 * Software iWARP device driver for Linux
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

#ifndef _SIW_VERBS_H
#define _SIW_VERBS_H

#include <linux/errno.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>
#include <linux/version.h>

#include "siw.h"
#include "siw_cm.h"

#if KS_IB_DEVICE_ALLOC_USES_UCONTEXT
extern int siw_alloc_ucontext(struct ib_ucontext *, struct ib_udata *);
extern void siw_dealloc_ucontext(struct ib_ucontext *);
#else
extern struct ib_ucontext *siw_alloc_ucontext(struct ib_device *,
					      struct ib_udata *);
extern int siw_dealloc_ucontext(struct ib_ucontext *);
#endif
extern int siw_query_port(struct ib_device *, t_ib_port, struct ib_port_attr *);
#if KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE
extern int siw_get_port_immutable(struct ib_device *, t_ib_port,
				  struct ib_port_immutable *);
#endif
#if KS_IB_QUERY_DEVICE_HAS_UDATA
extern int siw_query_device(struct ib_device *, struct ib_device_attr *,
			    struct ib_udata *);
#else
extern int siw_query_device(struct ib_device *, struct ib_device_attr *);
#endif

#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
#if !KS_IB_CREATE_CQ_HAS_ATTR_BUNDLE
extern int siw_create_cq(struct ib_cq *base_cq, const struct ib_cq_init_attr *attr,
		  struct ib_udata *udata);
#else
extern int siw_create_cq(struct ib_cq *cq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs);
#endif
#elif KS_CREATE_CQ_HAS_IB_CQ_INIT_ATTR
extern struct ib_cq *siw_create_cq(struct ib_device *,
				   const struct ib_cq_init_attr *,
				   struct ib_ucontext *, struct ib_udata *);
#else
extern struct ib_cq *siw_create_cq(struct ib_device *, int, int,
				   struct ib_ucontext *, struct ib_udata *);
#endif

#if !KS_PROCESS_MAD_HAS_OUT_MAD_PKEY_INDEX
int siw_no_mad(struct ib_device *, int, t_ib_port, struct ib_wc *, struct ib_grh *,
	       struct ib_mad *, struct ib_mad *);
#elif KS_PROCESS_MAD_HAS_IB_MAD_HDR
int siw_no_mad(struct ib_device *, int, t_ib_port, const struct ib_wc *,
	       const struct ib_grh *,const struct ib_mad_hdr *, size_t,
	       struct ib_mad_hdr *, size_t *, u16 *);
#else
int siw_no_mad(struct ib_device *, int, t_ib_port, const struct ib_wc *,
		   const struct ib_grh *, const struct ib_mad *, struct ib_mad *,
		   size_t *, u16 *);
#endif

extern int siw_query_port(struct ib_device *, t_ib_port, struct ib_port_attr *);
extern int siw_query_pkey(struct ib_device *, t_ib_port, u16, u16 *);
extern int siw_query_gid(struct ib_device *, t_ib_port, int, union ib_gid *);

extern struct net_device *siw_get_netdev(struct ib_device *, t_ib_port);

#if !KS_IB_DEVICE_PD_USES_UCONTEXT
extern int siw_alloc_pd(struct ib_pd *pd, struct ib_udata *udata);
#if KS_IB_DEALLOC_PD_INT_RETURN
extern int siw_dealloc_pd(struct ib_pd *pd, struct ib_udata *udata);
#else
extern void siw_dealloc_pd(struct ib_pd *pd, struct ib_udata *udata);
#endif
#else
extern struct ib_pd *siw_alloc_pd(struct ib_device *, struct ib_ucontext *,
				  struct ib_udata *);
extern int siw_dealloc_pd(struct ib_pd *);
#endif

#if (KS_IB_CREATE_AH_HAS_UDATA && KS_IB_CREATE_AH_HAS_FLAGS) // KS_IB_CREATE_AH_HAS_FLAGS_AND_UDATA
struct rdma_ah_attr;
#	if KS_IB_CREATE_AH_HAS_PD
#	define DECLARE_SIW_CREATE_AH(func) struct ib_ah *func(struct ib_pd *pd, struct rdma_ah_attr *ah_attr, \
							u32 flags, struct ib_udata *udata)
#	elif KS_IB_CREATE_AH_HAS_AH
#	define DECLARE_SIW_CREATE_AH(func) int func(struct ib_ah *ib_ah, struct rdma_ah_attr *ah_attr, \
							u32 flags, struct ib_udata *udata)
# 	else
#		error "Invalid create_ah signature"
#	endif
#elif KS_IB_CREATE_AH_HAS_UDATA
#	if KS_IB_CREATE_AH_HAS_PD
struct rdma_ah_attr;
#	define DECLARE_SIW_CREATE_AH(func) struct ib_ah *func(struct ib_pd *pd, struct rdma_ah_attr *ah_attr, \
								struct ib_udata *udata)
#	elif KS_IB_CREATE_AH_HAS_AH
#		if KS_IB_CREATE_AH_HAS_AH_INIT_ATTR
struct rdma_ah_init_attr;
#	define DECLARE_SIW_CREATE_AH(func) int func(struct ib_ah *ib_ah, struct rdma_ah_init_attr *ah_attr, struct ib_udata *udata)
#		else
struct rdma_ah_attr;
#	define DECLARE_SIW_CREATE_AH(func) int func(struct ib_ah *ib_ah, struct rdma_ah_attr *ah_attr, struct ib_udata *udata)
#		endif
# 	else
#		error "Invalid create_ah signature"
#	endif
#else
#	define DECLARE_SIW_CREATE_AH(func) struct ib_ah *func(struct ib_pd *, struct ib_ah_attr *)
#endif

extern DECLARE_SIW_CREATE_AH(siw_create_ah);

#if KS_IB_DESTROY_AH_HAS_FLAGS
#	if KS_IB_DESTROY_AH_RETURNS_INT
#define DECLARE_SIW_DESTROY_AH(func) int func(struct ib_ah *ah, u32 flags)
#	elif KS_IB_DESTROY_AH_RETURNS_VOID
#define DECLARE_SIW_DESTROY_AH(func) void func(struct ib_ah *ah, u32 flags)
#	else
#		error "Invalid destroy_ah signature"
#	endif
#else
#	if KS_IB_DESTROY_AH_RETURNS_INT
#define DECLARE_SIW_DESTROY_AH(func) int func(struct ib_ah *ah)
#	elif KS_IB_DESTROY_AH_RETURNS_VOID
#define DECLARE_SIW_DESTROY_AH(func) void func(struct ib_ah *ah);
#	else
#		error "Invalid destroy_ah signature"
#	endif
#endif

extern DECLARE_SIW_DESTROY_AH(siw_destroy_ah);

#if KS_IB_CREATE_QP_INT_RV
static inline struct siw_qp *to_siw_qp(struct ib_qp *ofa_qp)
{
	return container_of(ofa_qp, struct siw_qp, ofa_qp);
}
extern int siw_create_qp(struct ib_qp *, struct ib_qp_init_attr *,
				   struct ib_udata *);
#else
extern struct ib_qp *siw_create_qp(struct ib_pd *, struct ib_qp_init_attr *,
				   struct ib_udata *);

#endif
extern int siw_query_qp(struct ib_qp *, struct ib_qp_attr *, int,
			struct ib_qp_init_attr *);
extern int siw_ofed_modify_qp(struct ib_qp *, struct ib_qp_attr *, int,
			      struct ib_udata *);
#if KS_DESTROY_QP_HAS_UDATA
extern int siw_destroy_qp(struct ib_qp *, struct ib_udata *);
#else
extern int siw_destroy_qp(struct ib_qp *);
#endif
#if KS_POST_SEND_HAS_CONST
extern int siw_post_send(struct ib_qp *,
			const struct ib_send_wr *,
			const struct ib_send_wr **);
extern int siw_post_receive(struct ib_qp *,
			const struct ib_recv_wr *,
			const struct ib_recv_wr **);
#else
extern int siw_post_send(struct ib_qp *, struct ib_send_wr *,
			 struct ib_send_wr **);
extern int siw_post_receive(struct ib_qp *, struct ib_recv_wr *,
			    struct ib_recv_wr **);
#endif
#if KS_IB_DESTROY_CQ_HAS_IB_UDATA
#if KS_IB_DESTROY_CQ_INT_RETURN
extern int siw_destroy_cq(struct ib_cq *, struct ib_udata *);
#else
extern void siw_destroy_cq(struct ib_cq *, struct ib_udata *);
#endif
#else
extern int siw_destroy_cq(struct ib_cq *);
#endif
extern int siw_poll_cq(struct ib_cq *, int num_entries, struct ib_wc *);
extern int siw_req_notify_cq(struct ib_cq *, enum ib_cq_notify_flags);
#if KS_IB_REG_USER_MR_HAS_ATTR
extern struct ib_mr *siw_reg_user_mr(struct ib_pd *, struct ib_mr_init_attr *, struct ib_udata *);
#else
extern struct ib_mr *siw_reg_user_mr(struct ib_pd *, u64, u64, u64, int,
				     struct ib_udata *);
#endif
#if KS_IB_ALLOC_MR_HAS_IB_UDATA
extern struct ib_mr *siw_alloc_mr(struct ib_pd *, enum ib_mr_type, u32, struct ib_udata *);
#else
extern struct ib_mr *siw_alloc_mr(struct ib_pd *, enum ib_mr_type, u32);
#endif
extern struct ib_mr *siw_get_dma_mr(struct ib_pd *, int);
extern int siw_map_mr_sg(struct ib_mr *, struct scatterlist *, int,
			 unsigned int *);
#if KS_IB_DEREG_MR_HAS_IB_UDATA
extern int siw_dereg_mr(struct ib_mr *, struct ib_udata *);
#else
extern int siw_dereg_mr(struct ib_mr *);
#endif
#if !KS_IB_CREATE_SQR_HAS_IB_PD
extern int siw_create_srq(struct ib_srq *, struct ib_srq_init_attr *, struct ib_udata *);
#else
extern struct ib_srq *siw_create_srq(struct ib_pd *, struct ib_srq_init_attr *,
				     struct ib_udata *);
#endif
extern int siw_modify_srq(struct ib_srq *, struct ib_srq_attr *,
			  enum ib_srq_attr_mask, struct ib_udata *);
extern int siw_query_srq(struct ib_srq *, struct ib_srq_attr *);
#if KS_IB_DESTROY_SQR_HAS_IB_UDATA
#if KS_IB_DESTROY_CQ_INT_RETURN
extern int siw_destroy_srq(struct ib_srq *ofa_srq, struct ib_udata *udata);
#else
extern void siw_destroy_srq(struct ib_srq *ofa_srq, struct ib_udata *udata);
#endif
#else
extern int siw_destroy_srq(struct ib_srq *);
#endif
#if KS_POST_SRQ_RECV_HAS_CONST
extern int siw_post_srq_recv(struct ib_srq *,
				const struct ib_recv_wr *,
				const struct ib_recv_wr **);
#else
extern int siw_post_srq_recv(struct ib_srq *, struct ib_recv_wr *,
			     struct ib_recv_wr **);
#endif
extern int siw_mmap(struct ib_ucontext *, struct vm_area_struct *);

extern struct dma_map_ops siw_dma_generic_ops;
#if KS_IB_VERBS_HAS_DMA_MAPPING_OPS
extern struct ib_dma_mapping_ops siw_dma_mapping_ops;
#endif

#if KS_IB_HAS_FMR
struct ib_fmr *siw_fmr_alloc(struct ib_pd *pd, int acc,
			struct ib_fmr_attr *fmr_attr);
int siw_map_phys_fmr(struct ib_fmr *ibfmr, u64 *page_list,
		      int npages, u64 iova);
#endif
#endif
