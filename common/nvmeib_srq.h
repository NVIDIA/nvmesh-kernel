/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_SRQ_H
#define NVMEIB_SRQ_H

#include "nvmeib.h"

#define NVMEIB_MAX_NIC_SRQS 16
struct nvmeib_srq_params {
	int q_size;
	int msg_size;
	u8 buf_pattern;
	u32 srq_limit;
};

enum nvmeib_srq_type {
	NVMEIB_SRQ_TYPE_PRIMARY		= 0,
	NVMEIB_SRQ_TYPE_SECONDARY	= 1,
	NVMEIB_SRQ_TYPE_INVALID		= 2
};

/* SRQ pool APIs */
int nvmeib_srq_pool_create(struct nvmeib_dev *dev,
	int pool_size,
	struct nvmeib_srq_params *prim_q_params,
	struct nvmeib_srq_params *sec_qs_params,
	void *memmgr_metrics_ctx);
void nvmeib_srq_pool_free(struct nvmeib_dev *dev);
int nvmeib_srq_pool_query(struct nvmeib_dev *dev,
	struct nvmeib_srq_params *prim, struct nvmeib_srq_params *sec);

/* Primary/Secondary SRQ APIs */
struct nvmeib_srq_info *nvmeib_srq_info_get(struct nvmeib_dev *dev,
	enum nvmeib_srq_type type);
void nvmeib_srq_info_put(struct nvmeib_srq_info *srq_info, void *owner);

/* Private SRQ APIs */
struct nvmeib_srq_info *nvmeib_srq_info_create(struct nvmeib_dev *dev,
	struct nvmeib_srq_params *params, void *ctx, void *memmgr_metrics_ctx);
int nvmeib_srq_info_free(struct nvmeib_srq_info *srq_info);

/* Common APIs */
struct ib_srq *nvmeib_srq_info_ib_srq(struct nvmeib_srq_info *srq_info);
struct nvmeib_iu *nvmeib_srq_rtrv_recv(struct nvmeib_srq_info *srq_info,
	int index, void *owner);
int nvmeib_srq_post_recv(struct nvmeib_srq_info *srq_info,
	struct nvmeib_iu *iu);

struct nvmeib_iu *nvmeib_srq_find_next_owner(struct nvmeib_srq_info *srq_info,
		void *owner, struct nvmeib_iu *curr);

bool nvmeib_srq_is_pcpu_cq(struct nvmeib_srq_info *srq_info);


#if defined(DEBUG_SCQ_IU_OWNER) && (DEBUG_SCQ_IU_OWNER==1)

#if defined(DEBUG_SCQ_IU_OWNER_BT) && !defined(NVMEIB_COMMON)
#include "nvmeib_public.h"

#define nvmeib_iu_sw2hw_bt(_iu) do { \
	struct stack_trace st = { .max_entries = ARRAY_SIZE((_iu)->sw2hw_bt), .nr_entries = 0, .skip = 1};\
	memcpy((_iu)->old_sw2hw_bt, (_iu)->sw2hw_bt, sizeof((_iu)->old_sw2hw_bt));\
	nvmeib_public_save_stack_trace(&st);\
} while(0)
#else
#define nvmeib_iu_sw2hw_bt(_iu)
#endif

/* iu (recv-buf) ownership for shared (per-cpu, per-dev) CQs mode */
#define nvmeib_iu_owner_switch(_iu, _old, _new, _srq_info, _trace_) \
do {\
	if (nvmeib_srq_is_pcpu_cq(_srq_info)) {\
		enum nvmeib_iu_owner old;\
		BUILD_BUG_ON(_old != NVMEIB_IU_OWNER_SW && _old != NVMEIB_IU_OWNER_HW);\
		BUILD_BUG_ON(_new != NVMEIB_IU_OWNER_SW && _new != NVMEIB_IU_OWNER_HW);\
		old = atomic_cmpxchg(&_iu->owner, _old, _new);\
		if (old != _old) {\
			_NE(_trace_, "Invalid iu owner, exp @INT32_HEX but @INT32_HEX, iu=@PTR, srq_info=@PTR",\
			_old, old, _iu, _srq_info);\
			BUG();\
		}\
		if (_new == NVMEIB_IU_OWNER_HW)\
			nvmeib_iu_sw2hw_bt(_iu);\
	}\
} while(0)

#define nvmeib_iu_owner_sw2hw(_iu, _srq_info, _trace_) \
nvmeib_iu_owner_switch(_iu, NVMEIB_IU_OWNER_SW, NVMEIB_IU_OWNER_HW, _srq_info, _trace_)

#define nvmeib_iu_owner_hw2sw(_iu, _srq_info, _trace_) \
nvmeib_iu_owner_switch(_iu, NVMEIB_IU_OWNER_HW, NVMEIB_IU_OWNER_SW, _srq_info, _trace_)

#else /* !DEBUG_SCQ_IU_OWNER */

#define nvmeib_iu_owner_switch(_iu, _old, _new, _srq_info, _trace_)
#define nvmeib_iu_owner_sw2hw(_iu, _srq_info, _trace_)
#define nvmeib_iu_owner_hw2sw(_iu, _srq_info, _trace_)

#endif

#ifdef TRACE_SRQ
void nvmeib_srq_inc(struct nvmeib_srq_info *srq_info, int n);
void nvmeib_srq_dec(struct nvmeib_srq_info *srq_info, int n);
#else
#define nvmeib_srq_inc(srq_info, n)
#define nvmeib_srq_dec(srq_info, n)
#endif

#endif /* NVMEIB_SRQ_H */

