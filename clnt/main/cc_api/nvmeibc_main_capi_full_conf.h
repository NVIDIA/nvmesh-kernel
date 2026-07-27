#ifndef NVMEIBC_MAIN_CAPI_FULL_CONF_H
#define NVMEIBC_MAIN_CAPI_FULL_CONF_H

/* Management of full configuration request tasks */
void __full_conf_create( struct c_api_perrep* _this);
void __full_conf_destroy(struct c_api_perrep* _this);
void __full_conf_mark_received(struct nvmeibc_control_api* cc_api);
void __full_conf_request_on_err(struct nvmeibc_control_api* cc_api);

#endif
