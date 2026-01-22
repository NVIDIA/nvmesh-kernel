/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/* Doronl See change-ID Icbdbe7f6e069acc6d1754a8ba0530f13af49d56e for removed
   legacy MCS to configfs functions */

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_main_capi_parse_conf_inc_c

unsigned int cfg_version = 0;
char *cfg_name = "DefaultName";
char *cfg_id = "DefaultID";

module_param(cfg_version, uint, 0644);
MODULE_PARM_DESC(cfg_version, "Client configuration profile version");

module_param(cfg_name, charp, 0644);
MODULE_PARM_DESC(cfg_name, "Client configuration profile name"); // according to Tom, max len URN_UUID_STR_LENGTH+1

module_param(cfg_id, charp, 0644);
MODULE_PARM_DESC(cfg_id, "Client configuration profile ID");	// according to Tom, max len URN_UUID_STR_LENGTH+1

void nvmeibc_main_capi_fill_cinst_params_from_module_params(struct nvmeibc_cinst_params_main *p)
{
	strlcpy(&p->cfg_profile.name[0], cfg_name, URN_UUID_STR_LENGTH);
	strlcpy(&p->cfg_profile.id[0]  , cfg_id  , URN_UUID_STR_LENGTH+1);
	p->cfg_profile.version =         cfg_version;
}

#pragma pop_macro("__FILE_LITERAL__")
