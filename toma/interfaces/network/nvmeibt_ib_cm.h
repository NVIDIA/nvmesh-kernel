#ifdef TOMA_IB_ROCE
#ifndef NVMEIBT_IB_CM_H
#define NVMEIBT_IB_CM_H

#include "cm.h"

int nvmeibt_ib_cm_get_event(struct ib_cm_device *device, struct ib_cm_event **event);
int nvmeibt_ib_cm_ack_event(struct ib_cm_event *event);
struct ib_cm_device* nvmeibt_ib_cm_open_device(struct ibv_context *device_context);
void nvmeibt_ib_cm_close_device(struct ib_cm_device *device);
int nvmeibt_ib_cm_create_id(struct ib_cm_device *device,
		    struct ib_cm_id **cm_id, void *context);
int nvmeibt_ib_cm_destroy_id(struct ib_cm_id *cm_id);
int nvmeibt_ib_cm_attr_id(struct ib_cm_id *cm_id,
		  struct ib_cm_attr_param *param);
int nvmeibt_ib_cm_listen(struct ib_cm_id *cm_id,
		 __be64 service_id,
		 __be64 service_mask);
int nvmeibt_ib_cm_send_req(struct ib_cm_id *cm_id,
		   struct ib_cm_req_param *param);
int nvmeibt_ib_cm_send_rep(struct ib_cm_id *cm_id,
		   struct ib_cm_rep_param *param);
int nvmeibt_ib_cm_send_rtu(struct ib_cm_id *cm_id,
		   void *private_data,
		   uint8_t private_data_len);
int nvmeibt_ib_cm_send_dreq(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len);
int nvmeibt_ib_cm_send_drep(struct ib_cm_id *cm_id,
		    void *private_data,
		    uint8_t private_data_len);
int nvmeibt_ib_cm_notify(struct ib_cm_id *cm_id, enum ibv_event_type event);
int nvmeibt_ib_cm_send_rej(struct ib_cm_id *cm_id,
		   enum ib_cm_rej_reason reason,
		   void *ari,
		   uint8_t ari_length,
		   void *private_data,
		   uint8_t private_data_len);
int nvmeibt_ib_cm_init_qp_attr(struct ib_cm_id *cm_id,
		       struct ibv_qp_attr *qp_attr,
		       int *qp_attr_mask);
int nvmeibt_ib_cm_send_sidr_req(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_req_param *param);
int nvmeibt_ib_cm_send_sidr_rep(struct ib_cm_id *cm_id,
			struct ib_cm_sidr_rep_param *param);

int nvmeibt_ib_cm_init(void);
void nvmeibt_ib_cm_free(void);

#endif
#endif /* TOMA_IB_ROCE */
