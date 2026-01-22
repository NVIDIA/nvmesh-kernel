/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/* nvmeibc_main.c - NVMe IB attached block driver */
#include "nvmeibc_block.h"					// Must be first for simulator
#include "nvmeib_public.h"
#include "nvmeibc_main.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_types.h"
#include "nvmeib_wd.h"
#include "nvmeib_utils.h"
#include "nvmeib_public_procfs.h"
#include "nvmeibc_cc_api.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeibc_error_tags.h"
#include "nvmeib_srq.h"
#include "flog.h"
#include "nvmeibc_targets.h"
#include "nvmeibc_jam.h"
#include "main/nvmeibc_main_common.h"
#include "core/nvmeibc_core_common.h"
#include "nvmeib_ib_driver.h"
#include "management_utils_common/nvmeibc_management_capi_parse_conf.h"
#include "nvmeibc_ib_nordda_channel.h"
#include "nvmeibc_locks_channel.h"
/* Must be last to override module_{init/exit} */
#include "kr_undef.h"
#include "nvmeib_public.h"
#include "main/nvmeibc_main_common.inc.c"			// Todo: Remove me
#include "core/nvmeibc_core_common.inc.c"			// Todo: Remove me
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"
#include "common/proc_epilog.h"
#include "nvmeibc_memmgr_metrics.h"

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("NVMe block device over Infiniband");
MODULE_LICENSE("GPL and additional rights");

#define PROCFS_VOLUMES_STR "volumes"
#define PROCFS_DISKS_STR "disks"
#define PROCFS_NET_STR	"net"
#define PROCFS_JAM_STR	"jam"

int nvmeibc_debug_level = 1;

#if defined(NVMESH_IS_PRODUCTION_COMPILATION) && (NVMESH_IS_PRODUCTION_COMPILATION==1)
#define DEFAULT_TRACER_DEBUG_LEVEL 3
#define DEFAULT_GOODPATH_DEBUG_LEVEL 0
#define DEFAULT_GOODPATH_SYNCS_DEBUG_LEVEL 0
#define DEFAULT_GOODPATH_LOCKS_DEBUG_LEVEL 0
#define DEFAULT_TOPOLOGY_DEBUG_LEVEL 3
#define DEFAULT_RECOVERY_DEBUG_LEVEL 3
#else
#define DEFAULT_TRACER_DEBUG_LEVEL 4
#define DEFAULT_GOODPATH_DEBUG_LEVEL 2
#define DEFAULT_GOODPATH_SYNCS_DEBUG_LEVEL 4
#define DEFAULT_GOODPATH_LOCKS_DEBUG_LEVEL 3
#define DEFAULT_TOPOLOGY_DEBUG_LEVEL 4
#define DEFAULT_RECOVERY_DEBUG_LEVEL 4
#endif
#define DEFAULT_GOODPATH_TRANSPORT_DEBUG_LEVEL 1

int tracer_nvmeibc_debug_level = DEFAULT_TRACER_DEBUG_LEVEL;
int goodpath_nvmeibc_debug_level = DEFAULT_GOODPATH_DEBUG_LEVEL;
int goodpath_nvmeibc_syncs_debug_level = DEFAULT_GOODPATH_SYNCS_DEBUG_LEVEL;
int goodpath_nvmeibc_locks_debug_level = DEFAULT_GOODPATH_LOCKS_DEBUG_LEVEL;
int goodpath_nvmeibc_transport_debug_level = DEFAULT_GOODPATH_TRANSPORT_DEBUG_LEVEL;
int topology_debug_level = DEFAULT_TOPOLOGY_DEBUG_LEVEL;
int recovery_debug_level = DEFAULT_RECOVERY_DEBUG_LEVEL;

int nvmeibc_nr_cb_ulp_batch_nolock = 1;
bool nvmeibc_local_bypass_enabled = true;
bool nvmeibc_iommu_enabled = false;

module_param_named(debug_level, nvmeibc_debug_level, int, 0644);
MODULE_PARM_DESC(debug_level, "Deprecated Debug tracing level [0..2]");

module_param_named(tracer_debug_level, tracer_nvmeibc_debug_level, int, 0644);
MODULE_PARM_DESC(tracer_debug_level, "Control path tracing debug level [0..4]");

module_param_named(goodpath_debug_level, goodpath_nvmeibc_debug_level, int, 0644);
MODULE_PARM_DESC(goodpath_debug_level, "Data path tracing debug level");

module_param_named(goodpath_syncs_debug_level, goodpath_nvmeibc_syncs_debug_level, int, 0644);
MODULE_PARM_DESC(goodpath_syncs_debug_level, "Data path syncs tracing debug level");

module_param_named(goodpath_locks_debug_level, goodpath_nvmeibc_locks_debug_level, int, 0644);
MODULE_PARM_DESC(goodpath_locks_debug_level, "Data path locks tracing debug level");

module_param_named(goodpath_transport_debug_level, goodpath_nvmeibc_transport_debug_level, int, 0644);
MODULE_PARM_DESC(goodpath_transport_debug_level, "Data path transport layer tracing debug level");

module_param_named(topology_debug_level, topology_debug_level, int, 0644);
MODULE_PARM_DESC(topology_debug_level, "Topology path tracing debug level");

module_param_named(recovery_debug_level, recovery_debug_level, int, 0644);
MODULE_PARM_DESC(recovery_debug_level, "Recovery tracing debug level");

module_param_named(iommu_enabled, nvmeibc_iommu_enabled, bool, 0444);
MODULE_PARM_DESC(iommu_enabled, "Used to tell client that IOMMU is enabled");
EXPORT_SYMBOL(nvmeibc_iommu_enabled);

NVMEIB_DECLARE_KERNEL_WARNINGS_TRAP;

const char *nvmeibc_get_utsname_nodename(const struct nvmeibc_cinst_params_main *p)
{
	struct t_main_clnt_globals * _mg = __get_from_params_main_globals_container(p);
	return &_mg->com.full_name[0];
}

int nvmeib_debug_level(void)
{
	return nvmeibc_debug_level;
}

bool nvmeib_serial_console(void) { return false; }

bool profiling_enabled = ~true;
module_param(profiling_enabled, bool, 0644);
MODULE_PARM_DESC(profiling_enabled, "Enable statistics gathering, should be 0 if clocksource != tsc");

static const char* nvmeibc_mod_state_to_string(enum nvmeibc_mod_state state)
{
	switch (state) {
	case NVMEIBC_MOD_STATE_INITIALIZING: return "Initializing";
	case NVMEIBC_MOD_STATE_READY:        return "Ready";
	case NVMEIBC_MOD_STATE_PREP_RM:      return "Prepearing for removal";
	case NVMEIBC_MOD_STATE_RM_RDY:       return "Zombie, ready to removal";
	case NVMEIBC_MOD_STATE_EXITING:      return "Exiting";
	default:                             return "BUG";
	}
};

#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.inc.c"	// Todo: Remove
#include "core/nvmeibc_core_ibdev.h"
#include "block/nvmeibc_block_api_os.h"					// Print number of running reread partitions
#include "nvmeib_mcs.h"

MODULE_INFO(mcs_scheme_version, MCS_SCHEME_VERSION_STR);

#define STATUS_PROC_FRMT_VER 1
static ssize_t __fill_status_fmt(void *_ctx, char *buffer, size_t len, const char fmt)
{
	const struct t_main_clnt_globals *_mg = _ctx;
	#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	int count = 0, is_json = (fmt == 'J');
	enum nvmeibc_mod_state state = nvmeibc_get_state();
	// ---------------- Global Module info ------------------------------
	if (is_json)
		BUF_ADD("{\"module\": {\"state\":%d, \"state_str\":\"%s\",\n\"blk_size_bytes\":%d,\n\"n_cpus\":%d}\n", state, nvmeibc_mod_state_to_string(state), (1<<NVMEIBC_SECTOR_SHIFT), MAX_NUM_ACTIVE_CPUS);
	else
		BUF_ADD("module state=%d %s\nvolumes block size=%d[bytes], n_cpus=%d\n"                                      , state, nvmeibc_mod_state_to_string(state), (1<<NVMEIBC_SECTOR_SHIFT), MAX_NUM_ACTIVE_CPUS);
	if (_mg->priv_sched.state >= NVMEIBC_INST_STATE_EXITING)
		goto _out;							// Unsafe to report main layer which can get kfree (coz we dont hold any lock here)

	{// ------------------ Main layer info ------------------------------
	if (is_json)
		BUF_ADD(",\"main\": {");
	if (is_json) {
		BUF_ADD("\"uuid\":\"%pUB\", \"state\":%d,\n", &_mg->com.uuid, _mg->priv_sched.state);
		BUF_ADD("\"MCS\" : {\"shceme_version\": %u, \"report_id\": %llu, \"cached_messages\": %d,"
			" \"cache_tot_count\": %d, \"processing_multi_vol_cmd\": %d},\n",
			MCS_SCHEME_VERSION, _mg->cc_api.clnt_2_mgmt_report_id,
			nvmeib_mcs_cache_size(_mg->cc_api.mcs.handle),
			nvmeib_mcs_cache_count(_mg->cc_api.mcs.handle),
			_mg->cc_api.processing_multi_vol_cmd);
	} else {
		BUF_ADD("UUID: %pUB,\t\t<state=%d>\n", &_mg->com.uuid, _mg->priv_sched.state);
		BUF_ADD("MCS scheme ver=0x%x=%u reportID=%llu cached_messages=%d cache_tot_count=%d processing_multi_vol_cmd=%d\n",
			MCS_SCHEME_VERSION, MCS_SCHEME_VERSION,
			_mg->cc_api.clnt_2_mgmt_report_id,
			nvmeib_mcs_cache_size(_mg->cc_api.mcs.handle),
			nvmeib_mcs_cache_count(_mg->cc_api.mcs.handle),
			_mg->cc_api.processing_multi_vol_cmd);
	}
	if (1) {		// Dump msg loops stats
		int n_mcs_krnl_to_usr_msgs, n_cli_krnl_to_usr_msgs, n_main_wq_items;
		const struct nvmeibc_control_api* cc_api = &_mg->cc_api;
		n_main_wq_items = atomic_read(&_mg->sched.main_wq_submit_use_count);
		nvmeibc_cc_api_get_n_msgs(cc_api, &n_mcs_krnl_to_usr_msgs, &n_cli_krnl_to_usr_msgs);
		if (is_json) {
			BUF_ADD("\"n_messages\": {\"mcs\":%d, \"cli\":%d, \"wq\":%d}\n", n_mcs_krnl_to_usr_msgs, n_cli_krnl_to_usr_msgs, n_main_wq_items);
		} else {
			BUF_ADD("krnl_2_usr_msgs: n_mcs=%d, n_cli=%d, n_wq=%d\n", n_mcs_krnl_to_usr_msgs, n_cli_krnl_to_usr_msgs, n_main_wq_items);
		}
	}
	if (is_json)
		BUF_ADD("}\n");						// Close the main object
	}
	if (_mg->priv_sched.state != NVMEIBC_INST_STATE_READY)
		goto _out;							// Unsafe to report other non main layers coz they might not have been created yet or already destroyed

	// ------------------ Core layer info ------------------------------
	if (!is_json) {							// Todo: Add json support
		const struct nvmeibc_cinst_params_core *pcore = nvmeibc_isnt_params_main2core(_mg->p);
		count += nvmeibc_jam_fill_status(pcore, buffer+count, len-count);
	}

	// ------------------ Block layer info ------------------------------
	if (!is_json) {		// Block Layer to string: Todo, encapsulate as function and remove from here to block layer
		const struct nvmeibc_cinst_params_blk *pblk = nvmeibc_isnt_params_main2blk(_mg->p);
		extern enum nvmeibc_gf_optimization_type __gf_choose_functions(enum nvmeibc_gf_optimization_type index);
		extern const char *nvmeibc_gf_optimization_to_string(int);
		extern int gf_asm_count;
		const int gf_val = __gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT);
		count += nvmeibc_block_layer_all_tostring(buffer+count, len-count);
		BUF_ADD("GF algorithm:%d (%s), asm code version:%d\n", gf_val, nvmeibc_gf_optimization_to_string(gf_val), gf_asm_count);
		{
			bool is_read_part_on = false;
			const int n_read_part = nvmeibc_os_api_layer_get_num_read_part(pblk, &is_read_part_on);
			BUF_ADD("n_read_part=%d, is_on=%d\n", n_read_part, is_read_part_on);
		}
	}

_out:
	if (is_json) {
		count += nvmeib_proc_add_json_proc_epilog(STATUS_PROC_FRMT_VER, buffer+count, len-count);
		BUF_ADD("}\n");
	} else {
		count += nvmeib_proc_add_txt_proc_epilog(STATUS_PROC_FRMT_VER, buffer+count, len-count);
	}
	return count;
	#undef BUF_ADD
}

static ssize_t fill_status(void *_ctx, char *buffer, size_t len)
{
	return __fill_status_fmt(_ctx, buffer, len, 'H');
}

static ssize_t fill_status_json(void *_ctx, char *buffer, size_t len)
{
	return __fill_status_fmt(_ctx, buffer, len, 'J');
}

static ssize_t fill_cpu_masks_json(void *_ctx, char *buffer, size_t len)
{
	const struct t_main_clnt_globals *mg = _ctx;
	const struct nvmeibc_cinst_params_blk *pblk = nvmeibc_isnt_params_main2blk(mg->p);
	return nvmeibc_block_cpu_masks_to_json(pblk, buffer, len);
}

struct __fill_nics_json_wq_ctx {
	struct completion *alldone;
	const struct nvmeibc_cinst_params_main *p;
	char *buf;
	size_t len;
	int count;
	int rv;
};

static void __fill_single_nic_port_json(struct __fill_nics_json_wq_ctx *ctx, struct nvmeibc_dev *nic, struct nvmeibc_ib_port *port, bool used, bool *first, const char *indent) {
	if (port->gid.valid) {
		enum rdma_link_layer layer;
		struct ib_port_attr a;
		int rv;
		if (!*first) {
			ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
			              ",\n");
		}
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s{\n",
		              indent);


		layer = rdma_port_get_link_layer(nic->dev->ib_dev, port->port);
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"used\": %s,\n",
		              indent, used ? "true" : "false");
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"dev_name\": \"%s\",\n",
		              indent, nic->dev->ib_dev->name);
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"ndev_name\": \"%s\",\n",
		              indent, port->gid.ndev_name);
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"gid\": \"0x%016llx%016llx\",\n",
		              indent, be64_to_cpu(port->gid.gid.global.subnet_prefix), be64_to_cpu(port->gid.gid.global.interface_id));
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"port\": %d,\n",
		              indent, port->port);
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"pkey\":\"%#x\",\n",
		              indent, port->pkey);
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"transport\": \"%s\",\n",
		              indent, nvmeib_rdma_transport_to_s(layer, nic->dev->dev_type));
		if ((rv = ib_query_port(nic->dev->ib_dev, port->port, &a)) < 0) {
			_NE(error_during_ib_query_port, DMESG_PREFIX() "ib_query_port() failed, rv=@RV", rv);
		} else {
			ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"state\": \"%s\",\n",
		              indent, nvmeib_ib_port_state_t_to_s(a.state));
			ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"mtu\": %d,\n",
		              indent, ib_mtu_enum_to_int(a.active_mtu));
			ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"max_mtu\": %d,\n",
		              indent, ib_mtu_enum_to_int(a.max_mtu));
		}
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"gid_index\": %d,\n",
		              indent, port->gid.gid_index);
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"rove_v2\": %s,\n",
		              indent, port->gid.gid_type == NVMEIB_GID_TYPE_ROCE_V2 ? "true" : "false");
		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s\t\"ip_v6\": %s,\n",
		              indent, port->gid.net_type == NVMEIB_NETWORK_IPV6 ? "true" : "false");


		ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count,
		              "%s}",
		              indent);
		*first = false;
	}
}

#define CORE_CLIENT_NICS_PROC_FRMT_VER 1
static int __fill_nics_json_on_main_wq(void *_ctx) {
	struct __fill_nics_json_wq_ctx *ctx = _ctx;
	const struct nvmeibc_cinst_params *p = container_of(ctx->p, struct nvmeibc_cinst_params, main);
	struct nvmeibc_dev *nic;
	struct nvmeibc_ib_port *port;
	bool first = true;

	nvmeibc_assert_on_main_wq(ctx->p);

	ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count, "[\n");

	list_for_each_entry(nic, nvmeibc_get_all_devices(&p->core), dev_list_n) {
		list_for_each_entry(port, &nic->port_list, port_list_n) {
			__fill_single_nic_port_json(ctx, nic, port, true, &first, "\t");
		}
	}

	list_for_each_entry(nic, nvmeibc_get_unused_devices(&p->core), dev_list_n) {
		list_for_each_entry(port, &nic->port_list, port_list_n) {
			__fill_single_nic_port_json(ctx, nic, port, false, &first, "\t");
		}
	}

	ctx->count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_NICS_PROC_FRMT_VER, ctx->buf + ctx->count, ctx->len - ctx->count);

	ctx->count += scnprintf(ctx->buf + ctx->count, ctx->len - ctx->count, "\n]\n");

	ctx->rv = 0;

	complete(ctx->alldone);

	return 0;
}

static inline ssize_t fill_nics_json(void *arg, char *buf, size_t len) {
	DECLARE_COMPLETION_ONSTACK(alldone);
	struct t_main_clnt_globals *_mg = arg;
	struct __fill_nics_json_wq_ctx ctx = {
	    .alldone = &alldone,
	    .p = _mg->p,
	    .buf = buf,
	    .len = len,
	    .count = 0,
	    .rv = 0,
	};

	if (!ctx.p)
		return -ENOENT;

	if (nvmeibc_run_on_main_wq(ctx.p, __fill_nics_json_on_main_wq, &ctx, true, false, NULL)) {
		_NE(fill_nics_json_wq_err, DMESG_PREFIX() "Failed to run a task on the main wq");
		/* Will just return an empty buffer */
	} else {
		wait_for_completion(ctx.alldone);
	}

	if (!ctx.rv) {
		return ctx.count;
	} else {
		return ctx.rv;
	}
}

/* Adds work to main-WQ to collect debug information which
   shall preferably be lock-less operation */
#define CORE_CLIENT_DOT_DEBUG_PROC_FRMT_VER 1
static ssize_t fill_dot_debug(void *_ctx, char *buffer, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	struct t_main_clnt_globals *_mg = _ctx;
	int count = 0;
	if (nvmeib_debug_level() < MIN_TRACE) {
		BUF_ADD("module debug level insufficient\n");
		goto out;
	}

	_NT(t_5n_dp_dbg_tools, "Collecting data ...");
	BUF_ADD("Additional info: see dmesg...\n");
	if (nvmeibc_run_on_main_wq(_mg->p, fill_dot_debug_fn, (void*)_mg, true, true, NULL))
		_NT(t_5o_dp_dbg_tools, "Fail fill_dot_debug_fn");
	_NT(t_5p_dp_dbg_tools, "Done");

out:
	count += nvmeib_proc_add_txt_proc_epilog(CORE_CLIENT_DOT_DEBUG_PROC_FRMT_VER, buffer + count, len - count);
	return count;
#undef BUF_ADD
}

static ssize_t fill_shared_cq(
		void *_ctx, char *buffer, size_t len)
{
	struct t_main_clnt_globals *_mg = _ctx;
	int count = 0;
	struct nvmeibc_shared_cq_info *p = NULL;
	DECLARE_COMPLETION_ONSTACK(comp);

#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	if ((p = kmalloc(sizeof(*p), GFP_KERNEL))) {
		p->_mg = _mg;
		p->buffer = buffer;
		p->len = len;
		p->count = 0;
		p->comp = &comp;
		if (nvmeibc_run_on_main_wq(
					_mg->p, fill_shared_cq_fn, (void*)p, true, true, NULL))
			_NT(t_6q_dp_dbg_tools, "Fail fill_shared_cq");
		else {
			wait_for_completion(&comp);
			count = p->count;
		}
	}
#undef BUF_ADD

	kfree(p);
	return count;
}

//SCQ-TODO: reuse fill_shared_cq...
static ssize_t reset_shared_cq(
		void *_ctx, char *buffer, size_t len)
{
	struct t_main_clnt_globals *_mg = _ctx;
	struct nvmeibc_shared_cq_info *p = NULL;
	DECLARE_COMPLETION_ONSTACK(comp);
	int rv = -1;

	_NT(t0_reset_shared_cq, "reset pcpu-cq stats");

	if ((p = kmalloc(sizeof(*p), GFP_KERNEL))) {
		p->_mg = _mg;
		p->buffer = buffer;
		p->len = len;
		p->count = 0;
		p->comp = &comp;
		if (nvmeibc_run_on_main_wq(
					_mg->p, reset_shared_cq_fn, (void*)p, true, true, NULL))
			_NT(t1_reset_shared_cq, "Fail reset_shared_cq");
		else {
			wait_for_completion(&comp);
			rv = len;
		}
	}

	kfree(p);
	return rv;
}

#define PROC_FILE_CREATE(_mg, file_ptr, name, func) ({ \
	if (!rv) { \
		file_ptr = nvmeib_public_proc_create(name, (_mg)->proc_dir.root, &func ,NULL, _mg); \
		rv = ((file_ptr) ? 0 : -1); \
	}})

#define PROC_FILE_CREATE_WRITABLE(_mg, file_ptr, name, func) ({ \
	if (!rv) { \
		file_ptr = nvmeib_public_proc_create(name, (_mg)->proc_dir.root, NULL ,&func, _mg); \
		rv = ((file_ptr) ? 0 : -1); \
	}})

#define PROC_FILE_CREATE_RW(_mg, file_ptr, name, rd_func, wr_func) ({ \
	if (!rv) { \
		file_ptr = nvmeib_public_proc_create(name, (_mg)->proc_dir.root, &rd_func ,&wr_func, _mg); \
		rv = ((file_ptr) ? 0 : -1); \
	}})

#define PROC_FILE_REMOVE(_mg, file_ptr) ({ \
	if (file_ptr) { \
		nvmeib_public_proc_remove(file_ptr); \
		file_ptr = NULL; \
	}})

#include "nvmeib_msgloop.h"
#include "module/nvmeibc_module_main.h"
#include "module/nvmeibc_module_main.inc.c"				// Todo: Remove
									//
static int per_clnt_inst_proc_files_create(struct t_main_clnt_globals *_mg)
{
	int rv = 0;
	NFIN;
	rv = nvmeibc_cc_api_create(&_mg->cc_api, _mg->proc_dir.root);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.rsrc_info  , "rsrc_info.json", fill_rsrc_info);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.status     , "status", fill_status);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.statusjson , "status.json", fill_status_json);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.cpu_masks_json, "cpu_masks.json", fill_cpu_masks_json);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.dot_debug  , ".debug", fill_dot_debug);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.nics_json  , "nics.json", fill_nics_json);
	if (nvmeibc_use_pcpu_cq)
		PROC_FILE_CREATE_RW(_mg, _mg->proc_dir.files.shared_cq, "shared_cq", fill_shared_cq, reset_shared_cq);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.memmgr_info , "memmgr_info", nvmeibc_memmgr_metrics_info);
	PROC_FILE_CREATE(_mg, _mg->proc_dir.files.error_tags_info , "error_tags_info", nvmeibc_error_tags_info);
	NFOUT;
	return rv;
}

static void per_clnt_inst_proc_files_remove(struct t_main_clnt_globals *_mg)
{
	NFIN;
	nvmeibc_cc_api_destroy(&_mg->cc_api);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.status);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.statusjson);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.cpu_masks_json);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.rsrc_info);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.dot_debug);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.nics_json);
	if (nvmeibc_use_pcpu_cq)
		PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.shared_cq);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.memmgr_info);
	PROC_FILE_REMOVE(_mg, _mg->proc_dir.files.error_tags_info);
	NFOUT;
}

#define PROC_SUB_DIR_CREATE(_mg, dir_ptr, name) ({ \
	if (!(dir_ptr = proc_mkdir(name, _mg->proc_dir.root))) { \
		goto destroy; \
	}})

#define PROC_SUB_DIR_REMOVE(_mg, dir_ptr, name) ({ \
	if (dir_ptr) { \
		remove_proc_entry(name, _mg->proc_dir.root); \
		dir_ptr = NULL; \
	}})

static void nvmeibc_instance_procs_destroy(const struct nvmeibc_cinst_params *p)
{
	bool is_main_inst = nvmeibc_cinst_is_first_main_instance(&p->main);
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(&p->main);
	NFIN;
	if (_mg->proc_dir.root) {
		per_clnt_inst_proc_files_remove(_mg);
		PROC_SUB_DIR_REMOVE(_mg, _mg->proc_dir.disks  , PROCFS_DISKS_STR);
		PROC_SUB_DIR_REMOVE(_mg, _mg->proc_dir.volumes, PROCFS_VOLUMES_STR);
		PROC_SUB_DIR_REMOVE(_mg, _mg->proc_dir.net    , PROCFS_NET_STR);
		PROC_SUB_DIR_REMOVE(_mg, _mg->proc_dir.jam    , PROCFS_JAM_STR);
		if (!is_main_inst)
			remove_proc_entry(_mg->proc_dir.root_name, NULL);
		_mg->proc_dir.root_name = NULL;
	}
	NFOUT;
}

static int nvmeibc_instance_procs_create(const struct nvmeibc_cinst_params *p)
{
	bool is_main_inst = nvmeibc_cinst_is_first_main_instance(&p->main);
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(&p->main);
	int rv = -1;
	NFIN;

	_mg->proc_dir.root = is_main_inst ? nvmeibc_get_module_proc_dir_entry() : proc_mkdir(_mg->proc_dir.root_name, NULL);	// First instance currently shares drectory of the module (back-compatible). Todo, change this
	if (!_mg->proc_dir.root)
		goto destroy;
	PROC_SUB_DIR_CREATE(_mg, _mg->proc_dir.disks  , PROCFS_DISKS_STR);
	PROC_SUB_DIR_CREATE(_mg, _mg->proc_dir.volumes, PROCFS_VOLUMES_STR);
	PROC_SUB_DIR_CREATE(_mg, _mg->proc_dir.net    , PROCFS_NET_STR);
	PROC_SUB_DIR_CREATE(_mg, _mg->proc_dir.jam    , PROCFS_JAM_STR);
	if (per_clnt_inst_proc_files_create(_mg) < 0)
		goto destroy;
	rv = 0;
	goto out;

destroy:
	_NT(error_main_nvmeibc_procs_create, "Fail to create proc dir");
	nvmeibc_instance_procs_destroy(p);

out:
	NFOUT;
	return rv;
}

uuid_be *nvmeibc_get_uuid(const struct nvmeibc_cinst_params_core *pc)
{
	const struct nvmeibc_cinst_params_main *p = nvmeibc_isnt_params_core2main(pc);
	return &__get_from_params_main_globals_container(p)->com.uuid;
}

struct wd_obj *nvmeibc_get_watchdog(const struct nvmeibc_cinst_params_core *p)
{
	return __get_from_params_core_globals_container(p)->wd_commands;
}

struct wd_obj *nvmeibc_get_watchdog_pcpu(const struct nvmeibc_cinst_params_core *p, int cpu)
{
	if (cpu < 0 || cpu >= NR_CPUS)
		return NULL;
	return __get_from_params_core_globals_container(p)->pcpu_wds[cpu];
}

struct nvmeib_intr_shaper *nvmeibc_get_shaper(const struct nvmeibc_cinst_params_core *p)
{
	return __get_from_params_core_globals_container(p)->intr_shaper;
}

struct proc_dir_entry *nvmeibc_get_proc_dir_disks(const struct nvmeibc_cinst_params_core *pc)
{
	const struct nvmeibc_cinst_params_main *p = nvmeibc_isnt_params_core2main(pc);
	return __get_from_params_main_globals_container(p)->proc_dir.disks;
}
struct proc_dir_entry *nvmeibc_get_proc_dir_volumes(const struct nvmeibc_cinst_params_main *p)
{
	return __get_from_params_main_globals_container(p)->proc_dir.volumes;
}
struct proc_dir_entry *nvmeibc_get_proc_dir_net(const struct nvmeibc_cinst_params_core *pc)
{
	const struct nvmeibc_cinst_params_main *p = nvmeibc_isnt_params_core2main(pc);
	return __get_from_params_main_globals_container(p)->proc_dir.net;
}
struct proc_dir_entry *nvmeibc_get_proc_dir_jam(const struct nvmeibc_cinst_params_core *pc)
{
	const struct nvmeibc_cinst_params_main *p = nvmeibc_isnt_params_core2main(pc);
	return __get_from_params_main_globals_container(p)->proc_dir.jam;
}

struct list_head *nvmeibc_get_volumes(const struct nvmeibc_cinst_params_main *p)
{
	return &__get_from_params_main_globals_container(p)->vols.volumes;
}

struct list_head *nvmeibc_get_mt_volumes(const struct nvmeibc_cinst_params_main *p)
{
	return &__get_from_params_main_globals_container(p)->vols.mtvolumes;
}

int nvmeibc_get_thick_volumes_num(const struct nvmeibc_cinst_params_main *p)
{
	return __get_from_params_main_globals_container(p)->vols.num_thik_volumes;
}

int nvmeibc_get_all_volumes_num(const struct nvmeibc_cinst_params_main *p)
{
	struct t_main_clnt_globals *gm = __get_from_params_main_globals_container(p);
	return gm->vols.num_thik_volumes + gm->vols.num_mult_volumes;
}

int nvmeibc_get_volumes_num(const struct nvmeibc_cinst_params_main *p, enum nvmeibc_config_volume_type type)
{
	int n_vols = 0;
	struct nvmeibc_volume *curr_volume = NULL;

	list_for_each_entry(curr_volume, nvmeibc_get_volumes(p), link) {
		if (curr_volume->hdr.type == type){
			n_vols += 1;
		}
	}

	return n_vols;
}

int nvmeibc_get_masked_volumes_num(const struct nvmeibc_cinst_params_main *p, enum nvmeibc_config_volume_type type)
{
	int n_vols = 0;
	struct nvmeibc_volume *curr_volume = NULL;

	list_for_each_entry(curr_volume, nvmeibc_get_volumes(p), link) {
		if (curr_volume->hdr.type & type){
			n_vols += 1;
		}
	}

	return n_vols;
}

struct list_head *nvmeibc_get_disks(const struct nvmeibc_cinst_params_main *p)
{
	return &__get_from_params_main_globals_container(p)->disks.list;
}

static void __nvmeibc_disk_dump_disk_ids(struct nvmeibc_disk *disk)
{
    struct list_head        *list_disk_ids = &disk->volumes;
	struct nvmeibc_disk_id  *disk_id_iter;
	int n_total_ranges = 0;
	unsigned long flags;

	_NT(t_b7_dp_dbg_tools, "Disk @DISK_NAME is using disks:", disk->name);
	spin_lock_irqsave(&disk->volume_spinlock, flags);
	list_for_each_entry(disk_id_iter, list_disk_ids, slink) {
		_NT(t_b8_dp_dbg_tools, "\tname=@NAME, @DEV_NAME, count=@COUNT", disk_id_iter->name, disk_id_iter->volume->hdr.devname, disk_id_iter->num_ranges);
		n_total_ranges += disk_id_iter->num_ranges;
	}
	BUG_ON(disk->n_ranges != n_total_ranges);
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
}

void nvmeibc_dump_volumes_disk_ids(const struct nvmeibc_cinst_params_main *p)
{
	struct list_head         *volumes = nvmeibc_get_volumes(p);
	struct nvmeibc_volume    *vol_iter;

	list_for_each_entry(vol_iter, volumes, link) {
		nvmeibc_volume_dump_disk_ids(vol_iter);
	}
}

void nvmeibc_dump_disks_ids(const struct nvmeibc_cinst_params_main *p)
{
	struct list_head      *disks = nvmeibc_get_disks(p);
	struct nvmeibc_disk   *disk_iter;

	list_for_each_entry(disk_iter, disks, link) {
		__nvmeibc_disk_dump_disk_ids(disk_iter);
	}
}

int nvmeibc_add_volume(struct nvmeibc_volume *v)
{
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(v->p);
	_mg->vols.num_thik_volumes++;
	list_add_tail(&v->link, nvmeibc_get_volumes(v->p));
	return 0;
}

int nvmeibc_del_volume(struct nvmeibc_volume *v)
{
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(v->p);
	struct list_head *list = nvmeibc_get_volumes(v->p);
	const int rv = list_empty(list) ? -1 : 0;
	if (!rv){
		list_del(&v->link);
		--_mg->vols.num_thik_volumes;
	}
	return rv;
}

int nvmeibc_add_disk(struct nvmeibc_disk *disk)
{
	struct list_head *disks = nvmeibc_get_disks(nvmeibc_isnt_params_core2main(nvmeibc_cinst_get_core_p(disk)));
	list_add_tail(&disk->link, disks);
	return 0;
}

int nvmeibc_del_disk(struct nvmeibc_disk *disk)
{
	struct list_head *disks = nvmeibc_get_disks(nvmeibc_isnt_params_core2main(nvmeibc_cinst_get_core_p(disk)));
	const int rv = list_empty(disks) ? -1 : 0;
	if (!rv)
		list_del(&disk->link);
	return rv;
}

bool nvmeibc_find_disk(struct nvmeibc_disk *disk)
{
	struct list_head *disks = nvmeibc_get_disks(nvmeibc_isnt_params_core2main(nvmeibc_cinst_get_core_p(disk)));
	struct nvmeibc_disk *d = NULL;
	list_for_each_entry(d, disks, link)
		if (d == disk) {
			return true;
		}
	return false;
}

/*********************** Attach/Detach/Update Volumes ************************/
#include "main/cc_api/nvmeibc_main_capi_manipulate_vols.h"
#include "main/cc_api/nvmeibc_main_capi_manipulate_vols.inc.c"			// Todo: Remove me

static void update_disks_work_done_cb(void *cb_ctx)
{
	struct nvmeib_ref *wait_ref = cb_ctx;
	nvmeib_ref_put(wait_ref);
}

static void update_disks_work(struct workqe_struct *work)
{
	struct disk_update_workqe *rdwork =
		container_of(work, struct disk_update_workqe, work);
	struct nvmeibc_disk *disk;
	struct completion *caller_comp = rdwork->comp;
	enum nvmeibc_disk_update_type update_type = rdwork->update_type;
	void *update_data = rdwork->update_data;
	do_disk_update_fn_type do_fn = rdwork->do_fn;
	disk_pre_update_fn_type pre_fn = rdwork->pre_fn;
	disk_post_update_fn_type post_fn = rdwork->post_fn;
	const struct nvmeibc_cinst_params_core *p = rdwork->p;
	struct nvmeib_ref wait_ref;

	struct nvmeibc_disk_update_data disk_update_data = {
		.update_type = rdwork->update_type,
		.update_data = rdwork->update_data,
		.done_cb = update_disks_work_done_cb,
		.done_cb_ctx = &wait_ref,
	};

	NFIN;
	kfree(rdwork);

	nvmeib_ref_init(&wait_ref);

	if (pre_fn) {
		int rv = 0;
		_NT(t_31_cmain, "Calling function '@PRE_FN' before disk update: @DISK_UPDATE_TYPE_STR", pre_fn,
		   nvmeibc_disk_update_type_str(update_type));
		rv = (*pre_fn)(update_type, update_data);

		if (rv) {
			_NT(t_32_cmain, "Failed disk update: @DISK_UPDATE_TYPE_STR because pre_fn '@PRE_FN' returned @RV", nvmeibc_disk_update_type_str(update_type), pre_fn, rv);
			goto complete;
		}
	}

	/* We update the local server of every disk, because it is used
	*  in discover to determine if the disk is local or not. */
	list_for_each_entry(disk, nvmeibc_get_disks(nvmeibc_isnt_params_core2main(p)), link) {
		if (!do_fn || (*do_fn)(disk, update_type, update_data)) {
			_NT(t_33_cmain, "Scheduling update: @DISK_UPDATE_TYPE_STR on disk: @DISK_NAME",
			   nvmeibc_disk_update_type_str(update_type), disk->name);
			if (!nvmeib_ref_get(&wait_ref)) {
				BUG();
			}
			if (nvmeibc_disk_update_config(disk, &disk_update_data, false) < 0)
				nvmeib_ref_put(&wait_ref);
		}
	}

	nvmeib_ref_release_start(&wait_ref);
	nvmeib_ref_release_wait(&wait_ref);

	_NT(t_34_cmain, "All disks finished update: @DISK_UPDATE_TYPE_STR", nvmeibc_disk_update_type_str(update_type));

	if (post_fn) {
		_NT(t_35_cmain, "Calling function '@POST_FN' after disk update: @DISK_UPDATE_TYPE_STR", post_fn,
		   nvmeibc_disk_update_type_str(update_type));
		(*post_fn)(p, update_type, update_data);
	}

complete:
	if (caller_comp)
		complete(caller_comp);

	NFOUT;
}

static int update_disks_config(const struct nvmeibc_cinst_params_core *p,
							   enum nvmeibc_disk_update_type update_type, void *update_data,
			       do_disk_update_fn_type do_fn, disk_pre_update_fn_type pre_fn,
			       disk_post_update_fn_type post_fn, bool can_sleep)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	struct disk_update_workqe *work;
	int rv = 0;

	NFIN;
	if ((work = kzalloc(sizeof(*work), can_sleep ? GFP_KERNEL : GFP_ATOMIC))) {
		work->comp = can_sleep ? &comp : NULL;
		work->update_type = update_type;
		work->update_data = update_data;
		work->do_fn = do_fn;
		work->pre_fn = pre_fn;
		work->post_fn = post_fn;
		work->p = p;
		BUG_ON(p == NULL);				// Crash the code that making the problem rather then the code that suffering from it
		if (nvmeibc_is_on_main_wq(nvmeibc_isnt_params_core2main(p), false)) {
			/* On main wq so just call work fn directly */
			update_disks_work(&work->work);
		}
		else {
			WQ_INIT_WORK(&work->work, update_disks_work);
			if ((rv = nvmeibc_add_work(nvmeibc_isnt_params_core2main(p), &work->work)) < 0) {
				if (rv == -ENOSYS)
					_NT(t_39_cmain, "Main WQ is not running");
				else
					_NE(t_3A_cmain, DMESG_MOD_PREFIX ": Fail to add work: @RV", rv);
				kfree(work);
				goto out;
			}
			if (can_sleep)
				wait_for_completion(&comp);
		}
	}
	else {
		_NE(t_3B_cmain, DMESG_MOD_PREFIX ": fail to allocate remove_local work");
		rv = -ENOMEM;
	}

out:
	NFOUT;
	return rv;
}


static void __print_hooray(bool is_start, const char* mod_name)
{	/* Hooray */
	const char *ur = ((is_start) ? "registered" : "unregistered");		// Consider up/down
	struct timeval time;
	unsigned long local_time;
	struct rtc_time tm;
	do_gettimeofday(&time);
	local_time = (u32)(time.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);

	_NI_to_user(nvmeibc_hooray,
				"module", "Module @STR. Module: @STR. "
						  "Timestamp: @TM_YEAR-@TM_MON-@TM_MDAY @TM_HOUR:@TM_MIN:@TM_SEC. "
						  "Internal version for support cases: @COMMIT_ID_LONG",
				ur, mod_name,
				tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
				(unsigned long)COMMIT_ID); // Hooray //. Error code: 0
}

#include "module/instance/nvmeibc_cinst_params.h"

int nvmeibc_instance_destroy_on_modwq(const struct nvmeibc_cinst_params *p)
{
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(&p->main);

	nvmeibc_assert_on_module_wq();										// Add-remove isntance, can be done only from module main-wq
	_NT(t_21_main_davw, "Instance @STR", p->main.proc_dir_root_name);
	if ((!_mg)||(_mg->sched.main_wq == NULL))
		goto _only_main_remains;										// Main layer of isntance was not created correctly. Other layers need no cleaning

	nvmeibc_core_set_local_server_notification(&p->core, false);			// Stop notifications as fast as possible to remove stress from main-wq
	shut_down_detach_all_remainig_volumes_of_inst(&p->main, false); // We use multicompletion in detach_all_volumes to ensure we do not stop the main wq before all volumes are detached BEWARE: we leave this even though this action isnt safe (assuming a race is rare & code only runs if we have a bug in prepare phase or module is removed unsafely. otherwise, it'll leak a lot of memory.
	t_main_clnt_priv_sched_set(_mg, NVMEIBC_INST_STATE_EXITING);
	t_main_clnt_sched_disable(&_mg->sched);							// Disable main-wq, to prevent new tasks from arriving that might need block/core layers
	t_blok_clnt_globals_destroy(&p->blok);
	t_core_clnt_globals_destroy(&p->core);
	// Unlikely: mcs msgs keep arriving here, through proc files. Let them drain (auto-fail) here
	nvmeibc_instance_procs_destroy(p);								// Must be without lock, to let msgs drain. All msgs will fail coz there is no main-wq

_only_main_remains:
	t_main_clnt_sched_destroy(&_mg->sched);
	t_main_clnt_globals_destroy(&p->main);
	__print_hooray(false, p->main.proc_dir_root_name);
	nvmeibc_cinst_array_del(p);
	return 0;
}

void nvmeibc_instance_init_module_params(const struct nvmeibc_cinst_params *p) {
	struct nvmeibc_cinst_params *_p = (struct nvmeibc_cinst_params *)((void *)p);

	nvmeibc_block_dp_fill_cinst_params_from_module_params(&_p->blok);
	nvmeibc_core_ibdev_fill_cinst_params_from_module_params(&_p->core);
	nvmeibc_main_capi_fill_cinst_params_from_module_params(&_p->main);
}

void nvmeibc_instance_free_module_params(const struct nvmeibc_cinst_params *p)
{
	struct nvmeibc_cinst_params *_p = (struct nvmeibc_cinst_params *)((void *)p);

	t_core_clnt_globals_params_free(&_p->core);
	t_main_clnt_globals_params_free(&_p->main);
}

int nvmeibc_instance_create_on_modwq(const struct nvmeibc_cinst_params *p)
{
	struct t_main_clnt_globals *_mg;
	int rv = -EINVAL;

	nvmeibc_assert_on_module_wq();														// Add-remove isntance, can be done only from module main-wq
	_NT(t_23_main_davw, "Instance @STR", p->main.proc_dir_root_name);
	nvmeibc_cinst_array_add(p);
	if (t_main_clnt_globals_create(&p->main) < 0)
		goto _err;

	_mg = __get_from_params_main_globals_container(&p->main);
	nvmeib_public_uuid_gen(&_mg->com.uuid);
	_NT(t_24_main_davw, "Instance UUID: @CLIENT_UUID", &_mg->com.uuid);
	if (nvmeibc_instance_procs_create(p) < 0)
		goto _err;

	if (t_core_clnt_globals_create(&p->core, _mg->com.full_name) < 0)
		goto _err;

	if (unlikely(t_blok_clnt_globals_init(&p->blok) < 0)) {
		goto _err;
	}
	__print_hooray(true, p->main.proc_dir_root_name);
	t_main_clnt_priv_sched_set(_mg, NVMEIBC_INST_STATE_READY);
	rv = 0;
_err:
	if (unlikely(rv != 0)) {
		nvmeibc_instance_destroy_on_modwq(p);
	} else {
	}
	return rv;
}

#ifdef CORE_UNITEST
int init_corecomm(void);
void destroy_corecomm(void);
#else
#define init_corecomm(...)
#define destroy_corecomm(...)
#endif

bool nvmeibc_prof_evt_registered = false;

#if KS_HAS_PROFILE_EVENT_REGISTER
static int task_exit_notify(__attribute__((__unused__))struct notifier_block *self, __attribute__((__unused__))unsigned long val, void *data)
{
	extern void nvmeibc_dp_operation_mi_clear_plug(struct task_struct *tsk);
	nvmeibc_dp_operation_mi_clear_plug((struct task_struct *)data);
	return 0;
}

static struct notifier_block task_exit_nb = {
	.notifier_call = task_exit_notify,
};
#else
#include <linux/kprobes.h>

/* Use kprobes instead */
static int handler_on_do_exit(struct kprobe *p, struct pt_regs *regs) {
	extern void nvmeibc_dp_operation_mi_clear_plug(struct task_struct *tsk);
	nvmeibc_dp_operation_mi_clear_plug((struct task_struct *)current);
	return 0;
}

static struct kprobe kp_on_do_exit = {
	.symbol_name = "do_exit",
	.pre_handler = handler_on_do_exit
};

#endif

static void nvmeibc_profile_event_register(void)
{
	int rv;
#if KS_HAS_PROFILE_EVENT_REGISTER
	rv = profile_event_register(PROFILE_TASK_EXIT, &task_exit_nb);
#else
	rv = register_kprobe(&kp_on_do_exit);
#endif
	if (rv) {
		_NI(t_00_cper, DMESG_MOD_PREFIX ": Failed to register to profile-events, mini-elevator will not be usable (rv=@INT)", rv);
	} else {
		nvmeibc_prof_evt_registered = true;
	}
}

static void nvmeibc_profile_event_unregister(void)
{
	if (nvmeibc_prof_evt_registered) {
#if KS_HAS_PROFILE_EVENT_REGISTER
		profile_event_unregister(PROFILE_TASK_EXIT, &task_exit_nb);
#else
		unregister_kprobe(&kp_on_do_exit);
#endif
		nvmeibc_prof_evt_registered = false;
	}
}

static void __nvmeibc_exit(void)
{
	NFIN;
	nvmeib_local_client_close_server();
	set_toma_local_clnt_globals(NULL);
	destroy_corecomm();
	nvmeibc_profile_event_unregister();
	nvmeibc_instance_do_blocking(NULL, mw_inst_del_all_blocking, false);
	nvmeib_public_set_debug_level(NULL);
	nvmeibc_nordda_channel_wq_destroy();
	nvmeibc_locks_channel_wq_destroy();
	main_module_single_instance_globals_destroy();

#if !defined(BLKDEV_SIMULATOR) || (BLKDEV_SIMULATOR != 1)
	nvmesh_memmgr_metrics_free_pcpu(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
#endif
	NFOUT;
}

static void init_toma_local_client(struct t_main_clnt_globals *main_clnt_globals)
{
	struct nvmeib_local_client		toma_local_clnt;

	NFIN;
	toma_local_clnt.toma_request_f = nvmeibc_cc_api_handle_toma_to_local_clnt_msg;
	set_toma_local_clnt_globals(main_clnt_globals);
	nvmeib_register_local_client(&toma_local_clnt);
	_NT(4cak92m, "toma_request_f=@PTR main_clnt_globals=@PTR", toma_local_clnt.toma_request_f, main_clnt_globals);
	NFOUT;
}

static int __init nvmeibc_init(void) /* Constructor */
{
	const struct nvmeibc_cinst_params *p;
	int rv = -EINVAL;

	BUILD_BUG_ON(sizeof(struct nvmeibc_login_request) > NVMEIB_MAX_CM_REQ_PAYLOAD_SIZE);
	_NI(trace_1_nvmeibc_init, DMESG_MOD_PREFIX ": Load --> Version: commit_id=@COMMIT_ID_LONG, ports=@PORTS, guids=@GUIDS", (ulong)COMMIT_ID, nvmeibc_filter_ports, nvmeibc_filter_guids);
	main_module_single_instance_globals_init();
	nvmeib_set_debug_level(nvmeib_debug_level);
	nvmeib_public_set_debug_level(nvmeib_debug_level);

	if (nvmeibc_nordda_channel_wq_init() < 0) {
		_NE(nvmeibc_init_nordda_wq, "Failed to initialize nordda channel workqueue");
		goto out;
	}

	if (nvmeibc_locks_channel_wq_init() < 0) {
		_NE(nvmeibc_init_locks_wq, "Failed to initialize locks channel SCQ workqueue");
		goto out;
	}

#if !defined(BLKDEV_SIMULATOR) || (BLKDEV_SIMULATOR != 1)
	rv = nvmesh_memmgr_metrics_alloc_pcpu(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	if (rv < 0) {
		_NE(nvmeibc_init_pcpu_alloc, "Failed to initialize memmgr metrics");
		goto out;
	}
#endif

	if (!nvmeibc_use_pcpu_cq && !nvmeibc_gf_calc_in_irq_ctx()) {
		_NE_dmesg(nvmeibc_init_pcpu_must_be_on, "Invalid configuration: EC parity calculations on ARM is done via kernel primitive which require irqs enabled, for that nvmeibc module must use CQ pollers i.e. use_pcpu_cq=1");
		goto out;
	}

	if (nvmeibc_tcp_mode && nvmeibc_use_pcpu_cq) {
		_NE_dmesg(nvmeibc_init_pcpu_tcp_err, "Invalid configuration: pcpu-cqs is not supported over TCP");
		goto out;
	}

	if (nvmeibc_disk_prefix_priority_masks_validate_module_params()) {
		goto out;
	}

	if (!(p = nvmeibc_cinst_params_get_default()))
		goto out;
	nvmeibc_instance_init_module_params(p);
	if (nvmeibc_instance_do_blocking(p, mw_inst_add_blocking, false) < 0) /* calls nvmeibc_instance_create_on_modwq */
		goto err;
	nvmeibc_state_promote(NVMEIBC_MOD_STATE_READY);
	nvmeibc_profile_event_register();
	init_corecomm();
	init_toma_local_client(__get_from_params_main_globals_container(&p->main));

	rv = 0;
	goto out;

err:
	_NI(trace_2_nvmeibc_init, DMESG_MOD_PREFIX ": Failed nvmeshclient-init (@INT), rollback...", rv);
	__nvmeibc_exit();

out:
	_NI(trace_3_nvmeibc_init, DMESG_MOD_PREFIX ": Load <--");

	return rv;
}

static void __exit nvmeibc_exit(void) /* Destructor */
{
	_NI(trace_0_nvmeibc_exit, DMESG_MOD_PREFIX ": Unload -->");
	__nvmeibc_exit();
	_NI(trace_1_nvmeibc_exit, DMESG_MOD_PREFIX ": Unload <--");
}

module_init(nvmeibc_init);
module_exit(nvmeibc_exit);
