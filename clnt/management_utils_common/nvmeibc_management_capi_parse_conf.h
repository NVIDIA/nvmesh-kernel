#ifndef NVMEIBC_MANAGEMENT_CAPI_PARSE_CONF_H_
#define NVMEIBC_MANAGEMENT_CAPI_PARSE_CONF_H_

#include "nvmeibc_mcs_stub.h"


/* Methods for receiving vol configuration from mgmt */
int __copy_nvmeib_mgmt_to_client_volume_config(
	      struct nvmeib_mgmt_to_client_volume_configuration *dst,   // 1 result volume
	const struct nvmeib_mgmt_to_client_volume_configuration  src[], // array from which 'vol_i' is extracted
	const int vol_i);


void nvmeibc_cc_api_free_config_msg(void *msg);


struct nvmeib_mgmt_to_client_update_targets_nics* 
nvmeib_mgmt_to_client_update_targets_nics_clone(const struct nvmeib_mgmt_to_client_update_targets_nics *src);

void nvmeibc_cc_api_free_update_targets_nics(struct nvmeib_mgmt_to_client_update_targets_nics *msg);

#endif


