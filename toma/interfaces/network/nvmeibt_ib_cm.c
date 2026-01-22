/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#if TOMA_IB_ROCE
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_ib_cm.h"

#include <dlfcn.h>

#define LIBIBCM_FNAME "libibcm.so.1"

/* libibcm function pointers */
static int (*ib_cm_get_event_fn)(struct ib_cm_device *device, struct ib_cm_event **event) = NULL;
static int (*ib_cm_ack_event_fn)(struct ib_cm_event *event) = NULL;
static struct ib_cm_device* (*ib_cm_open_device_fn)(struct ibv_context *device_context) = NULL;
static void (*ib_cm_close_device_fn)(struct ib_cm_device *device) = NULL;
static int (*ib_cm_create_id_fn)(struct ib_cm_device *device,
		    struct ib_cm_id **cm_id, void *context) = NULL;
static int (*ib_cm_destroy_id_fn)(struct ib_cm_id *cm_id) = NULL;
static int (*ib_cm_attr_id_fn)(struct ib_cm_id *cm_id,
		  struct ib_cm_attr_param *param) = NULL;
static int (*ib_cm_listen_fn)(struct ib_cm_id *cm_id,
		 __be64 service_id,
		 __be64 service_mask) = NULL;
static int (*ib_cm_send_req_fn)(struct ib_cm_id *cm_id,
		   struct ib_cm_req_param *param) = NULL;
static int (*ib_cm_send_rep_fn)(struct ib_cm_id *cm_id,
		   struct ib_cm_rep_param *param) = NULL;
static int (*ib_cm_send_rtu_fn)(struct ib_cm_id *cm_id,
		   void *private_data,
		   uint8_t private_data_len) = NULL;
static int (*ib_cm_send_dreq_fn)(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len) = NULL;
static int (*ib_cm_send_drep_fn)(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len) = NULL;
static int (*ib_cm_notify_fn)(struct ib_cm_id *cm_id, enum ibv_event_type event) = NULL;
static int (*ib_cm_send_rej_fn)(struct ib_cm_id *cm_id,
		   enum ib_cm_rej_reason reason,
		   void *ari,
		   uint8_t ari_length,
		   void *private_data,
		   uint8_t private_data_len) = NULL;
static int (*ib_cm_init_qp_attr_fn)(struct ib_cm_id *cm_id,
		       struct ibv_qp_attr *qp_attr,
		       int *qp_attr_mask) = NULL;
static int (*ib_cm_send_sidr_req_fn)(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_req_param *param) = NULL;
static int (*ib_cm_send_sidr_rep_fn)(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_rep_param *param) = NULL;
static void *libibcm_handle = NULL;

int nvmeibt_ib_cm_get_event(struct ib_cm_device *device, struct ib_cm_event **event)
{
	if (ib_cm_get_event_fn)
		return (*ib_cm_get_event_fn)(device, event);
	else
		return -1;
}

int nvmeibt_ib_cm_ack_event(struct ib_cm_event *event)
{
	if (ib_cm_ack_event_fn)
		return (*ib_cm_ack_event_fn)(event);
	else
		return -1;
}

struct ib_cm_device* nvmeibt_ib_cm_open_device(struct ibv_context *device_context)
{
	if (ib_cm_open_device_fn)
		return (*ib_cm_open_device_fn)(device_context);
	else
		return NULL;
}

void nvmeibt_ib_cm_close_device(struct ib_cm_device *device)
{
	if (ib_cm_close_device_fn)
		return (*ib_cm_close_device_fn)(device);
}

int nvmeibt_ib_cm_create_id(struct ib_cm_device *device,
		    struct ib_cm_id **cm_id, void *context)
{
	if (ib_cm_create_id_fn)
		return (*ib_cm_create_id_fn)(device, cm_id, context);
	else
		return -1;
}

int nvmeibt_ib_cm_destroy_id(struct ib_cm_id *cm_id)
{
	if (ib_cm_destroy_id_fn)
		return (*ib_cm_destroy_id_fn)(cm_id);
	else
		return -1;
}

int nvmeibt_ib_cm_attr_id(struct ib_cm_id *cm_id,
		  struct ib_cm_attr_param *param)
{
	if (ib_cm_attr_id_fn)
		return (*ib_cm_attr_id_fn)(cm_id, param);
	else
		return -1;
}

int nvmeibt_ib_cm_listen(struct ib_cm_id *cm_id,
		 __be64 service_id,
		 __be64 service_mask)
{
	if (ib_cm_listen_fn)
		return (*ib_cm_listen_fn)(cm_id, service_id, service_mask);
	else
		return -1;
}

int nvmeibt_ib_cm_send_req(struct ib_cm_id *cm_id,
		   struct ib_cm_req_param *param)
{
	if (ib_cm_send_req_fn)
		return (*ib_cm_send_req_fn)(cm_id, param);
	else
		return -1;
}

int nvmeibt_ib_cm_send_rep(struct ib_cm_id *cm_id,
		   struct ib_cm_rep_param *param)
{
	if (ib_cm_send_rep_fn)
		return (*ib_cm_send_rep_fn)(cm_id, param);
	else
		return -1;
}

int nvmeibt_ib_cm_send_rtu(struct ib_cm_id *cm_id,
		   void *private_data,
		   uint8_t private_data_len)
{
	if (ib_cm_send_rtu_fn)
		return (*ib_cm_send_rtu_fn)(cm_id, private_data, private_data_len);
	else
		return -1;
}

int nvmeibt_ib_cm_send_dreq(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len)
{
	if (ib_cm_send_dreq_fn)
		return (*ib_cm_send_dreq_fn)(cm_id, private_data, private_data_len);
	else
		return -1;
}

int nvmeibt_ib_cm_send_drep(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len)
{
	if (ib_cm_send_drep_fn)
		return (*ib_cm_send_drep_fn)(cm_id, private_data, private_data_len);
	else
		return -1;
}

int nvmeibt_ib_cm_notify(struct ib_cm_id *cm_id, enum ibv_event_type event)
{
	if (ib_cm_notify_fn)
		return (*ib_cm_notify_fn)(cm_id, event);
	else
		return -1;
}

int nvmeibt_ib_cm_send_rej(struct ib_cm_id *cm_id,
		   enum ib_cm_rej_reason reason,
		   void *ari,
		   uint8_t ari_length,
		   void *private_data,
		   uint8_t private_data_len)
{
	if (ib_cm_send_rej_fn)
		return (*ib_cm_send_rej_fn)(cm_id, reason, ari, ari_length,
									private_data, private_data_len);
	else
		return -1;
}

int nvmeibt_ib_cm_init_qp_attr(struct ib_cm_id *cm_id,
		       struct ibv_qp_attr *qp_attr,
		       int *qp_attr_mask)
{
	if (ib_cm_init_qp_attr_fn)
		return (*ib_cm_init_qp_attr_fn)(cm_id, qp_attr, qp_attr_mask);
	else
		return -1;
}

int nvmeibt_ib_cm_send_sidr_req(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_req_param *param)
{
	if (ib_cm_send_sidr_req_fn)
		return (*ib_cm_send_sidr_req_fn)(cm_id, param);
	else
		return -1;
}

int nvmeibt_ib_cm_send_sidr_rep(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_rep_param *param)
{
	if (ib_cm_send_sidr_rep_fn)
		return (*ib_cm_send_sidr_rep_fn)(cm_id, param);
	else
		return -1;
}

int nvmeibt_ib_cm_init(void)
{
	int rv = 0;
	const char *error;

	libibcm_handle = dlopen(LIBIBCM_FNAME, RTLD_LAZY);
	if (!libibcm_handle) {
		N_Tf(trace_ib_cm_nvmeibt_ib_cm_init, "Failed to dlopen: " LIBIBCM_FNAME "");
		rv = -1;
		goto out;
	}

	dlerror();
	ib_cm_get_event_fn = dlsym(libibcm_handle, "ib_cm_get_event");
	ib_cm_ack_event_fn = dlsym(libibcm_handle, "ib_cm_ack_event");
	ib_cm_open_device_fn = dlsym(libibcm_handle, "ib_cm_open_device");
	ib_cm_close_device_fn = dlsym(libibcm_handle, "ib_cm_close_device");
	ib_cm_create_id_fn = dlsym(libibcm_handle, "ib_cm_create_id");
	ib_cm_destroy_id_fn = dlsym(libibcm_handle, "ib_cm_destroy_id");
	ib_cm_attr_id_fn = dlsym(libibcm_handle, "ib_cm_attr_id");
	ib_cm_listen_fn = dlsym(libibcm_handle, "ib_cm_listen");
	ib_cm_send_req_fn = dlsym(libibcm_handle, "ib_cm_send_req");
	ib_cm_send_rep_fn = dlsym(libibcm_handle, "ib_cm_send_rep");
	ib_cm_send_rtu_fn = dlsym(libibcm_handle, "ib_cm_send_rtu");
	ib_cm_send_dreq_fn = dlsym(libibcm_handle, "ib_cm_send_dreq");
	ib_cm_send_drep_fn = dlsym(libibcm_handle, "ib_cm_send_drep");
	ib_cm_notify_fn = dlsym(libibcm_handle, "ib_cm_notify");
	ib_cm_send_rej_fn = dlsym(libibcm_handle, "ib_cm_send_rej");
	ib_cm_init_qp_attr_fn = dlsym(libibcm_handle, "ib_cm_init_qp_attr");
	ib_cm_send_sidr_req_fn = dlsym(libibcm_handle, "ib_cm_send_sidr_req");
	ib_cm_send_sidr_rep_fn = dlsym(libibcm_handle, "ib_cm_send_sidr_rep");

	if ((error = dlerror())) {
		N_Tf(trace_1_ib_cm_nvmeibt_ib_cm_init, "Failed (@ERROR_STR) to dynamic bind: " LIBIBCM_FNAME "", error);
		dlclose(libibcm_handle);
		rv = -1;
	}
out:
	return rv;
}

void nvmeibt_ib_cm_free(void)
{
	if (libibcm_handle)
		dlclose(libibcm_handle);
}
#endif /* TOMA_IB_ROCE */
