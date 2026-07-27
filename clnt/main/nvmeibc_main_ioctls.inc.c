#include "module/instance/nvmeibc_cinst_params.h"
#include "main/nvmeibc_main_common.h"
#include "core/nvmeibc_core_common.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeibc_main_ioctls_inc_c

/* Todo: Make ./+.h with correct API and add this c file to make */
/******************************* Main ioctls ********************************/
static void __dump_volume_status(struct nvmeibc_control_api *cc_api, const struct nvmeibc_volume *volume, const int ordinal)
{
	struct nvmeib_client_to_mgmt_vol_info *msg = kzalloc(sizeof(*msg) +
											sizeof(*msg->attachments), GFP_KERNEL);
	char cli_reply[MAX_VOL_INFO_STRING];
	msg->attachments = (void*)(&msg[1]);
	__fill_generic_message_with_basic_info(msg, cc_api);
	__fill_msg_vol_info(msg->attachments, &volume->hdr, NVMEIB_C_TO_M_VOLUME_ACK_ATTACHED, nvmeibc_get_io_perm_for_reporting(volume->block_dev));
	__vol_info_to_string(msg->attachments, cli_reply);
	_NI_dmesg(t_1h_dp_dbg_tools, "@INT) @STR", ordinal, cli_reply);
	kfree(msg);
}
static int __dump_vols(struct nvmeibc_control_api *cc_api, const char *dump_type /* Todo: check type */)
{
	const struct nvmeibc_cinst_params_main *p = __get_cinst_params_from_cc_api(cc_api);
	struct nvmeibc_volume *volume;
	int i = 0;
	 nvmeibc_assert_on_main_wq(p);
	_NI_dmesg(t_1g_dp_dbg_tools, "Starting volume status dump(@PTR) for @RV volumes", dump_type, nvmeibc_get_all_volumes_num(p));
	list_for_each_entry(volume, nvmeibc_get_volumes(p), link) {
		__dump_volume_status(cc_api, volume, ++i);
	}
	list_for_each_entry(volume, nvmeibc_get_mt_volumes(p), link) {
		__dump_volume_status(cc_api, volume, ++i);
	}
	_NI_dmesg(t_1i_dp_dbg_tools, "Volume status dump completed");
	return 0;
}

static int __get_req_conf(struct nvmeibc_control_api *cc_api, const char *cmd_unused)
{
	_NI_dmesg(t_1j_dp_dbg_tools, "Got a request to ask for complete configuration");
	__schedule_request_config(cc_api, REQUEST_ALL_VOLUMES, "CLI request");
	(void)cmd_unused;
	return 0;
}

static int _nvmeibc_main_clnt_instance_set_param(const struct nvmeibc_cinst_params_main *pm, const char *str);
static int __parse_clnt_inst_params(struct nvmeibc_cinst_params *p, const char *str_params)
{
	char buf[256], *param_end;
	int len, rv = 0;

	if (str_params[0] == '\0')
		goto _out;

	while (1) {
		if (!(param_end = strchr(str_params, '|'))) {
			rv = _nvmeibc_main_clnt_instance_set_param(&p->main, str_params + 5);
			goto _out;
		}
		len = param_end - str_params + 1;
		strlcpy(buf, str_params, min(len, 256));
		if ((rv = _nvmeibc_main_clnt_instance_set_param(&p->main, buf + 5)) < 0)
			goto _out;
		str_params = param_end + 1;
	}
_out:
	return rv;
}

static int __sscanf_strings_back_compat(struct nvmeibc_cinst_params *p, const char *str_config)
{	// Parse: {%8[^,],%8[^}]}. Old kernels does not support this option in sscanf, what a shame...
	#define CINST_NAME_MIN_LEN (1)			// Allow names of 1 letter, like "a", not inlcuding \'0'
#if 0
	const char *fmt = "{%"__stringify(CINST_NAME_LEN)"[^,],%"__stringify(CINST_NAME_LEN)"[^}]}";
	int rv = sscanf(str_config, fmt, p->blok.dev_name.str, p->blok.dir_lsblk.str);
#else
	const char *s, *end = str_config;			// Todo: Generalize for CSV parsing
	int rv, len;					// len = including '\0'
	if (str_config[0] != '{')
		return -EINVAL;				// No heading '{'
	s = &end[1];
	end = strchr(s, ',');
	len = ((int)(end-s+1));
	if ((!end) || (len > CINST_NAME_LEN) || (len <= CINST_NAME_MIN_LEN))
		return -EINVAL;				// Incorrect first arg
	strlcpy(p->blok.dev_name.str, s, (size_t)len);
	s = &end[1];					// Skip ','
	end = strchr(s, '}');
	len = ((int)(end-s+1));
	if ((!end) || (len > CINST_NAME_LEN) || ((s != end) && (len <= CINST_NAME_MIN_LEN)))
		return -EINVAL;				// Incorrect second arg
	if (s == end)
		p->blok.dir_lsblk.str[0] = '\0';
	else
		strlcpy(p->blok.dir_lsblk.str, s, (size_t)len);
	if ((rv = __parse_clnt_inst_params(p, end + 1)) >= 0)
		rv = 2;
#endif
	return rv;
}

static int __parse_clnt_inst_conf(struct nvmeibc_cinst_params *p, const char *str_config)
{
	int rv = __sscanf_strings_back_compat(p, str_config);
	if (rv != 2) {
		_NT(t_17_vol_ioctl, "incorrect isntance params=@STR, rv=@RV", str_config, rv);
		rv = -EINVAL;
	} else {
		rv = 0;
	}
	return rv;
}

static int __verify_permissions_for_instance_ioctls(const struct nvmeibc_control_api *cc_api)
{	// This ioctls can be given only to primary instance
	if (nvmeibc_cinst_is_first_main_instance(__get_cinst_params_from_cc_api(cc_api))) {
		return 0;
	} else {
		return -EPERM;
	}
}

#include "module/nvmeibc_module_main.h"		// Todo Move function below from main layer to module layer
#include "module/instance/nvmeibc_cinst.h"
static int nvmeibc_main_clnt_instance_add_ioctl(struct nvmeibc_control_api *cc_api, const char *str_config)
{
	struct nvmeibc_cinst_params *p = (void*)nvmeibc_cinst_params_get_default();
	int rv;

	if (cc_api && (rv = __verify_permissions_for_instance_ioctls(cc_api)) < 0)
		goto _out;

	if (!p) {
		rv = -ENODEV;
		goto _out;
	}

	nvmeibc_instance_init_module_params(p);
	if ((rv = __parse_clnt_inst_conf(p, str_config)) < 0) {
		rv = -EINVAL;
		goto _out;
	}
	if (nvmeibc_cinst_get_by_name(p->blok.dev_name.str)) {
		rv = -EALREADY;
		goto _out;
	}
	if (cc_api) {				// Old version: Ioctl to add instance given to first instance, will block the main-wq of first instance
		rv = nvmeibc_instance_do_blocking(p, mw_inst_add_blocking, false);
	} else {
		rv = nvmeibc_instance_create_on_modwq(p);	// Ioctl given to module, on module-wq
	}
_out:
	if (rv)
		nvmeibc_instance_free_module_params(p);

	return rv;
}

static int nvmeibc_main_clnt_instance_del_ioctl(struct nvmeibc_control_api *cc_api, const char *str_config)
{	// Note! ioctl is received on instance (0) different the deleted one.
	struct nvmeibc_cinst_params tmp;
	const struct nvmeibc_cinst_params *p;
	int rv = 0;

	if (cc_api && (rv = __verify_permissions_for_instance_ioctls(cc_api)) < 0)
		goto _out;

	if ((rv = __parse_clnt_inst_conf(&tmp, str_config)) < 0) {
		goto _out;
	}
	p = nvmeibc_cinst_get_by_name(tmp.blok.dev_name.str);
	if (!p) {
		rv = -ENODEV;
		goto _out;
	}
	if (nvmeibc_get_all_volumes_num(&p->main)) {		// Cant delete instance before detaching volumes
		rv = -EBUSY;
		goto _out;
	}
	if (nvmeibc_cinst_is_first_instance(p)) {	// Instance 0 can never be deleted
		rv = -EPERM;
		goto _out;
	}
	if (cc_api) {				// Old version: Ioctl to add instance given to first instance, will block the main-wq of first instance
		rv = nvmeibc_instance_do_blocking(p, mw_inst_del_blocking, false);
	} else {
		rv = nvmeibc_instance_destroy_on_modwq(p);	// Ioctl given to module, on module-wq
	}
	rv = 0;
_out:
	if (rv) {
		_NI(t_20_vol_ioctl, MAIN_IOCTL_PREFIX ": nvmeibc ioctl failure. rv=@RV", rv);
	}
	return rv;
}

static int __attribute__((unused)) nvmeibc_main_clnt_instance_dump(struct nvmeibc_control_api *cc_api, const char *str_config)
{
	int rv = 0;
	if (cc_api && (rv = __verify_permissions_for_instance_ioctls(cc_api)) < 0)
		goto _out;
	(void)str_config;
	// nvmeibc_cinst_array_debug_print(); Deprecated. Use /proc file to read the data.
_out:
	if (rv) {
		_NI(t_21_vol_ioctl, MAIN_IOCTL_PREFIX ": nvmeibc ioctl failure. rv=@RV", rv);
	}
	return rv;
}

static int _nvmeibc_main_clnt_instance_set_param(const struct nvmeibc_cinst_params_main *pm, const char *str)
{
	const struct nvmeibc_cinst_params_blk  *pb = nvmeibc_isnt_params_main2blk(pm);
	const struct nvmeibc_cinst_params_core *pc = nvmeibc_isnt_params_main2core(pm);
	int rv = 0;

	// Dispatch to layers, Daniel: Explicit removal of constness because we are going to edit params
	switch (str[0]) {
	case 'b': rv = t_blok_clnt_globals_params_update((void*)pb, &str[2]); break;
	case 'c': rv = t_core_clnt_globals_params_update((void*)pc, &str[2]); break;
	case 'm': rv = t_main_clnt_globals_params_update((void*)pm, &str[2]); break;
	default : rv = -EINVAL;
	}
	if (rv) {
		_NI(t_22_vol_ioctl, MAIN_IOCTL_PREFIX ": nvmeibc ioctl failure. rv=@RV", rv);
	}
	return rv;
}

static int nvmeibc_main_clnt_instance_set_param(struct nvmeibc_control_api *cc_api, const char *str)
{
	return _nvmeibc_main_clnt_instance_set_param(__get_cinst_params_from_cc_api(cc_api), str);
}

static int __help(struct nvmeibc_control_api *cc_api, const char *cmd);

/********************** Main function *****************************/
typedef int (*_vioctl_func)(struct nvmeibc_control_api* cc_api, const char*cmd);		// Prototype of callback of each ioctl
struct t_vioctl {
	const char *name;
	short       len;
	_vioctl_func func;
	const char *help_args;
	const char *help_hint;
	bool need_inst;			// does this ioctl need client instance to be executed?
};

static struct t_vioctl vioctls[] = {
	{"dump_vol_status" , 15, &__dump_vols       , "", "Print attached vol status", true},
	{"get_full_conf"   , 13, &__get_req_conf    , "", "Request full config from mgmt", true},
	{"clnt++"          ,  6, &nvmeibc_main_clnt_instance_add_ioctl, "", "Add new clnt instance", false},
	{"clnt--"          ,  6, &nvmeibc_main_clnt_instance_del_ioctl, "", "Remove existing clnt isntance", false},
	{"param"           ,  5, &nvmeibc_main_clnt_instance_set_param, "", "Set param for specific instance", true},
	{"help"            ,  4, &__help            , "", "print help", false}
};

static int __help(struct nvmeibc_control_api *ctx_unused, const char *cmd)
{
	int i, n_ioctls = ARRAY_SIZE(vioctls);
	for (i = 0; i < n_ioctls; i++) {
		_NI_dmesg(t_bq_dp_dbg_tools, "@IOCTL_IDX), @IOCTL_NAME@IOCTL_HELP_ARGS/* @IOCTL_HELP_HINT */", i, vioctls[i].name, vioctls[i].help_args, vioctls[i].help_hint);
	}
	(void)ctx_unused; (void)cmd;
	return 0;
}

int nvmeibc_main_ioctl(struct nvmeibc_control_api *cc_api, const char *cmd)
{
	int rv = -EINVAL, i, n_ioctls = ARRAY_SIZE(vioctls);
	nvmeibc_assert_on_main_wq(__get_cinst_params_from_cc_api(cc_api));
	_NT(t_01_vol_ioctl, MAIN_IOCTL_PREFIX "@PTR Will @STR", cc_api, cmd);
	for (i = 0; i < n_ioctls; i++) {
		const struct t_vioctl *vioctl = &vioctls[i];
		if (!strncmp(cmd, vioctl->name, vioctl->len) &&
			((vioctl->need_inst && cc_api) || (!vioctl->need_inst /*&& !cc_api*/))) {
			rv = vioctl->func(cc_api, cmd + vioctl->len);
			_NI(t_03_vol_ioctl_, "@EVENT_TAG " MAIN_IOCTL_PREFIX " executed main ioctl: @STR rv=@RV", EV_IOCTL(), cmd, rv);
			goto _out;
		}
	}
	_NI(t_02_vol_ioctl, MAIN_IOCTL_PREFIX ": @PTR Unrecognized cmd: @STR! use 'help'", cc_api, cmd);
_out:
	return rv;
}

int nvmeibc_module_ioctl(const char *cmd)
{
	int rv = -EINVAL, i, n_ioctls = ARRAY_SIZE(vioctls);

	nvmeibc_assert_on_module_wq();
	_NT(t_03_vol_ioctl, MAIN_IOCTL_PREFIX "nvmeibc mod: Will @STR", cmd);
	for (i = 0; i < n_ioctls; i++) {
		const struct t_vioctl *vioctl = &vioctls[i];
		if ((!vioctl->need_inst) && (!strncmp(cmd, vioctl->name, vioctl->len))) {
			rv = vioctl->func(NULL, cmd + vioctl->len);
			goto _out;
		}
	}
	_NI(t_04_vol_ioctl, MAIN_IOCTL_PREFIX ": nvmeibc mod: Unrecognized cmd: @STR! use 'help'", cmd);
_out:
	return rv;
}

#pragma pop_macro("__FILE_LITERAL__")
