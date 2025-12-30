#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "nvmeibc_disk.h"
#include "common/proc_epilog.h"
#include "common_public/nvmeib_public_keeper.h"
#include "nvmeibc_memmgr_metrics.h"
#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__  nvmeibc_core_ibdev_inc_c

/* option to filter nics and ports
   if empty, no filter is used
   otherwise the format is either
   <hca_id> - use this nic and all its ports
   <hca_id>:port id
   for example:
   mlx4_0:1,mlx_4:2,mlx4_1:1 - will use three ports of two nics
   */
#define MAX_FP 1024
static char nvmeibc_filter_ports[MAX_FP] = "";
module_param_string(ports, nvmeibc_filter_ports, MAX_FP, 0644);
MODULE_PARM_DESC(ports, "Option to filter nics and ports\n"
   "\t\t If empty, no filter is used otherwise the format is either:\n"
   "\t\t    <hca_id> - use this nic and all its ports\n"
   "\t\t    <hca_id>:port id\n"
   "\t\t For example:\n"
   "\t\t    mlx4_0:1,mlx_4:2,mlx4_1:1 - will use three ports of two nics");

static char nvmeibc_filter_guids[MAX_FP] = "";
module_param_string(guids, nvmeibc_filter_guids, MAX_FP, 0644);
MODULE_PARM_DESC(guids, "option to filter ports according to port\'s hardware guids");

unsigned nvmeibc_max_nic_srqs = NVMEIB_MAX_NIC_SRQS;
module_param_named(max_nic_srqs, nvmeibc_max_nic_srqs, int, 0444);
MODULE_PARM_DESC(max_nic_srqs, "Maximum SRQs per nic");

unsigned sm_th = 32;
module_param(sm_th, uint, 0644);
MODULE_PARM_DESC(sm_th, "Maximum concurrent sm-requests per client");

bool nvmeibc_use_pcpu_cq = false; /* Disabled by service script for TCP */
module_param_named(use_pcpu_cq, nvmeibc_use_pcpu_cq, bool, 0444);
MODULE_PARM_DESC(use_pcpu_cq, "Use a per CPU shared completion queue (SCQ) and shared receive queue (SRQ)");

bool nvmeibc_pcpu_cq_poll_proc = false;
module_param_named(pcpu_cq_poll_proc, nvmeibc_pcpu_cq_poll_proc, bool, 0444);
MODULE_PARM_DESC(pcpu_cq_poll_proc, "Create proc files for polling the nvmeibc shared completion queues from SPDK (Requires pcpu_cq_all_cpus=Y for nvmeib_common)");

atomic_t nvmeibc_num_ioch_rm_works = ATOMIC_INIT(0);  // num of ioch the ioch remove works currently run
unsigned nvmeibc_max_ioch_rm_works = 0;
module_param_named(max_ioch_rm_works, nvmeibc_max_ioch_rm_works, int, 0644);
MODULE_PARM_DESC(max_ioch_rm_works, "Max concurrent IO communication channel removal operations");

bool nvmeibc_use_rdda = false; // Disable prior 2.5.0 due to rare DI [EC-8005]
module_param_named(use_rdda, nvmeibc_use_rdda, bool, 0444);
MODULE_PARM_DESC(use_rdda, "Allow using RDDA for client");

unsigned nvmeibc_nic_io_stats_block_size = 1 << NVMEIBC_SECTOR_SHIFT;
module_param_named(nic_io_stats_block_size, nvmeibc_nic_io_stats_block_size, uint, 0444);
MODULE_PARM_DESC(nic_io_stats_block_size, "Block-size to use for NIC iostats.json");

typedef int (*disk_pre_update_fn_type)(  /*const struct nvmeibc_cinst_params_core *p,*/ enum nvmeibc_disk_update_type, void *);
typedef void (*disk_post_update_fn_type)(  const struct nvmeibc_cinst_params_core *p, enum nvmeibc_disk_update_type, void *);
typedef int (*do_disk_update_fn_type)(   /*const struct nvmeibc_cinst_params_core *p,*/ struct nvmeibc_disk *disk, enum nvmeibc_disk_update_type, void *);

struct disk_update_workqe {
	struct workqe_struct work;
	const struct nvmeibc_cinst_params_core *p;
	struct completion *comp;
	enum nvmeibc_disk_update_type update_type;
	void *update_data;
	do_disk_update_fn_type do_fn;
	disk_pre_update_fn_type pre_fn;
	disk_post_update_fn_type post_fn;
};

NVMEIBC_MEMMGR_METRIC(c_dev_srq, "component=client.dev.srq");
NVMEIBC_MEMMGR_METRIC(c_dev_fr_pool, "component=client.dev.fr_pool");

static int update_disks_config(const struct nvmeibc_cinst_params_core *p,
							   enum nvmeibc_disk_update_type update_type, void *update_data,
			       do_disk_update_fn_type do_fn, disk_pre_update_fn_type pre_fn,
			       disk_post_update_fn_type post_fn, bool can_sleep);

unsigned int nvmeibc_tcp_mode = 0;
module_param_named(tcp_mode, nvmeibc_tcp_mode, uint, 0444);
MODULE_PARM_DESC(tcp_mode, "TCP transport mode, 0 = RoCE only, 1 = TCP Only, 2 or greater = TCP and RoCE");

#define nvmeibc_cg_ib_register_client(cg)                                      \
	({                                                                         \
		int ___rv;                                                             \
		BUG_ON((cg)->ib.registered);                                           \
		___rv = ib_register_client(&(cg)->ib.cli);                             \
		if (!___rv) (cg)->ib.registered = true;                                \
		___rv;                                                                 \
	})
#define nvmeibc_cg_ib_unregister_client(cg)                                    \
	({                                                                         \
		if ((cg)->ib.registered) {                                             \
			ib_unregister_client(&(cg)->ib.cli);                               \
			(cg)->ib.registered = false;                                       \
		}                                                                      \
	})

#define nvmeibc_cg_ib_sa_register_client(cg)                                   \
	({                                                                         \
		BUG_ON((cg)->ib.sa_registered);                                        \
		ib_sa_register_client(&(cg)->ib.sa_cli);                               \
		(cg)->ib.sa_registered = true;                                        \
	})
#define nvmeibc_cg_ib_sa_unregister_client(cg)                                 \
	({                                                                         \
		if ((cg)->ib.sa_registered) {                                          \
			ib_sa_unregister_client(&(cg)->ib.sa_cli);                         \
			(cg)->ib.sa_registered = false;                                    \
		}                                                                      \
	})


extern uint nvmeibc_jentry_num_blocks;

static void nvmeibc_core_ibdev_fill_cinst_params_from_module_params(struct nvmeibc_cinst_params_core* p)
{
	p->filter_guids = nvmeibc_filter_guids;
	p->max_g_len = MAX_FP;
	p->filter_ports = nvmeibc_filter_ports;
	p->max_p_len = MAX_FP;
	p->max_nic_srqs = nvmeibc_max_nic_srqs;
	p->sm_th = sm_th;
	p->use_pcpu_cq = nvmeibc_use_pcpu_cq;
	p->use_rdda = nvmeibc_use_rdda;
	p->binje = nvmeibc_jentry_num_blocks;
	p->tcp_mode = nvmeibc_tcp_mode;
	nvmeibc_max_ioch_rm_works = num_online_cpus();
}

static int fill_dot_debug_fn(void *_ctx)
{
	struct t_main_clnt_globals *_mg = _ctx;
	struct nvmeibc_dev *nic_dev;
	struct list_head *devs;
	int rv = 0;
	NFIN;
	if (_mg->priv_sched.state != NVMEIBC_INST_STATE_READY)
		goto _out;							// Unsafe to report other non main layers coz they might not have been created yet or already destroyed

	devs = nvmeibc_get_all_devices(nvmeibc_isnt_params_main2core(_mg->p));
	list_for_each_entry(nic_dev, devs, dev_list_n) {
		if (nic_dev->dev->fr_pool) {
			_NT(t_90_dp_dbg_tools, "NIC @IB_DEV_NAME (@NIC_DEV)", nic_dev->dev->ib_dev->name, nic_dev);
			nvmeib_fast_reg_pool_trace(nic_dev->dev->fr_pool);
		} else
			_NT(t_91_dp_dbg_tools, "NIC @IB_DEV_NAME (@NIC_DEV) has no FR pool", nic_dev->dev->ib_dev->name, nic_dev);
	}
	nvmeibc_targets_list_to_string(_mg->p);
	NFOUT;
_out:
	return rv;
}

static int fill_shared_cq_fn(void *_ctx)
{
	struct nvmeibc_shared_cq_info *p = _ctx;
	struct t_main_clnt_globals *_mg = p->_mg;
	char *buffer = p->buffer;
	int len = p->len;
	int count = 0;
	struct nvmeibc_dev *nic_dev;
	struct list_head *devs;
	int rv = 0;

	NFIN;
	if (_mg->priv_sched.state != NVMEIBC_INST_STATE_READY)
		goto _out;							// Unsafe to report other non main layers coz they might not have been created yet or already destroyed

#define BUF_ADD(...) count += scnprintf(buffer+count, len-count, __VA_ARGS__)
	count += nvmeib_dev_cq_stat_hdr(buffer, len);
	devs = nvmeibc_get_all_devices(nvmeibc_isnt_params_main2core(_mg->p));
	list_for_each_entry(nic_dev, devs, dev_list_n)
		count += nvmeib_dev_cq_stat(nic_dev->dev, buffer + count, len - count);
#undef BUF_ADD

_out:
	p->count = count;
	complete(p->comp);
	NFOUT;
	return rv;
}

static int reset_shared_cq_fn(void *_ctx)
{
	struct nvmeibc_shared_cq_info *p = _ctx;
	struct t_main_clnt_globals *_mg = p->_mg;
	struct nvmeibc_dev *nic_dev;
	struct list_head *devs;
	int rv = -EINVAL;
	NFIN;

	_NT(trace_reset_shared_cq_fn, "reset pcpu-cq stats work");

	if (_mg->priv_sched.state != NVMEIBC_INST_STATE_READY)
		goto out;

	devs = nvmeibc_get_all_devices(nvmeibc_isnt_params_main2core(_mg->p));
	list_for_each_entry(nic_dev, devs, dev_list_n)
		nvmeib_dev_cq_stat_reset(nic_dev->dev);
	rv = 0;

out:
	complete(p->comp);

	NFOUT;
	return rv;
}

#define CORE_CLIENT_RSRC_INFO_PROC_FRMT_VER 1
static ssize_t fill_rsrc_info(void *_ctx, char *buffer, size_t len)
{
	const struct t_main_clnt_globals *_mg = _ctx;
	int count = 0;
	struct nvmeibc_disk *d;
	struct list_head *disks;
	bool first = true;
	count += scnprintf(buffer + count, len - count, "{ \"c_disks\": [\n");
	disks = nvmeibc_get_disks(_mg->p);
	list_for_each_entry(d, disks, link) {
		if (!first)
			count += scnprintf(buffer + count, len - count,",");
		first = false;
		count +=  nvmeibc_disk_print_info(d, buffer + count, len - count);
	}
	count += scnprintf(buffer + count, len - count, "]\n");
	count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_RSRC_INFO_PROC_FRMT_VER, buffer + count, len - count);
	count += scnprintf(buffer + count, len - count, "}\n");
	return count;
}

const char *nvmeibc_device_name(struct nvmeibc_dev *dev)
{
	return nvmeib_device_name(dev->dev);
}

/******************************* Mlx Hack: ************************************/
// Unfortunately registering ib device does not have context (Sub optimal API
// of mellanox. So as a solution we use unique function ptr for each client
// instance.
#if IB_REMOVE_EXTRA_ARG
	#define IB_REM_EXTRA_ARG_DECLARE ,void *v
	#define IB_REM_EXTRA_ARG_UNUSE   (void)v;
#else
	#define IB_REM_EXTRA_ARG_DECLARE
	#define IB_REM_EXTRA_ARG_UNUSE
#endif
#if KS_IB_CLIENT_ADD_RV_IS_INT
typedef int (*_f_add_t)(struct ib_device *);
#else
typedef void (*_f_add_t)(struct ib_device *);
#endif
typedef void (*_f_del_t)(struct ib_device * IB_REM_EXTRA_ARG_DECLARE);
static void add_one(   const struct nvmeibc_cinst_params_core *pc, struct ib_device *device);
static void remove_one(const struct nvmeibc_cinst_params_core *pc, struct ib_device *device);

#define __cp(i) &nvmeibc_cinst_array_get_itr_i_unsafe(i)->core

#include "module/instance/nvmeibc_cinst_max.h"
#if 0
static void __f_add_0(struct ib_device *device){                          add_one(   __cp(0), device); }
static void __f_add_1(struct ib_device *device){                          add_one(   __cp(1), device); }
static void __f_add_2(struct ib_device *device){                          add_one(   __cp(2), device); }
static void __f_add_3(struct ib_device *device){                          add_one(   __cp(3), device); }
static void __f_del_0(struct ib_device *device IB_REM_EXTRA_ARG_DECLARE) {remove_one(__cp(0), device); IB_REM_EXTRA_ARG_UNUSE }
static void __f_del_1(struct ib_device *device IB_REM_EXTRA_ARG_DECLARE) {remove_one(__cp(1), device); IB_REM_EXTRA_ARG_UNUSE }
static void __f_del_2(struct ib_device *device IB_REM_EXTRA_ARG_DECLARE) {remove_one(__cp(2), device); IB_REM_EXTRA_ARG_UNUSE }
static void __f_del_3(struct ib_device *device IB_REM_EXTRA_ARG_DECLARE) {remove_one(__cp(3), device); IB_REM_EXTRA_ARG_UNUSE }

static _f_add_t _f_add_array_[NVMEIBC_MAX_CINSTS] = {
		__f_add_0, __f_add_1, __f_add_2, __f_add_3
};
static _f_del_t _f_del_array_[NVMEIBC_MAX_CINSTS] = {
		__f_del_0, __f_del_1, __f_del_2, __f_del_3
};

#else
#include "nvmeib_macro_magic.h"
#define REG_ADD_FUNC_PRE __f_add

#if KS_IB_CLIENT_ADD_RV_IS_INT
#define GEN_ADD_REG_FUNC(i, id) \
	static int id ## i (struct ib_device *device) \
	{add_one(__cp(i), device); return 0;}
#else
#define GEN_ADD_REG_FUNC(i, id) \
	static void id ## i (struct ib_device *device) \
	{add_one(__cp(i), device);}
#endif

EVAL(REPEAT(NVMEIBC_MAX_CINSTS, GEN_ADD_REG_FUNC, REG_ADD_FUNC_PRE))

#define REG_REMOVE_FUNC_PRE __f_del
#define GEN_REMOVE_REG_FUNC(i, id) \
	static void id ## i (struct ib_device *device IB_REM_EXTRA_ARG_DECLARE) \
	{remove_one(__cp(i), device); IB_REM_EXTRA_ARG_UNUSE }
EVAL(REPEAT(NVMEIBC_MAX_CINSTS, GEN_REMOVE_REG_FUNC, REG_REMOVE_FUNC_PRE))

#define REG_ADD_REG_FUNC(i, id) COMMA_IF(i) [i] = id ## i
static _f_add_t _f_add_array_[NVMEIBC_MAX_CINSTS] = {
	EVAL(REPEAT(NVMEIBC_MAX_CINSTS, REG_ADD_REG_FUNC, REG_ADD_FUNC_PRE))
};

#define REG_REMOVE_REG_FUNC(i, id) COMMA_IF(i) [i] = id ## i
static _f_del_t _f_del_array_[NVMEIBC_MAX_CINSTS] = {
	EVAL(REPEAT(NVMEIBC_MAX_CINSTS, REG_REMOVE_REG_FUNC, REG_REMOVE_FUNC_PRE))
};
#endif

/****************************** Mlx Hack End **********************************/
static void t_core_clnt_globals_init_ibdev(const struct nvmeibc_cinst_params_core *pc)
{
	const int index = nvmeibc_cinst_get_core_inst_num(pc);
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(pc);
	cg->ib.cli.name =   (char*)cg->clnt_inst_name;
	cg->ib.cli.add =    _f_add_array_[index];
	cg->ib.cli.remove = _f_del_array_[index];
	cg->ib.selected_link_layer = IB_LINK_LAYER_UNSPECIFIED;
	INIT_LIST_HEAD(&cg->devs_lists.all);
	INIT_LIST_HEAD(&cg->devs_lists.used);
	INIT_LIST_HEAD(&cg->devs_lists.unused);
	//_NT(t_00_cibdev, "default_layer=@LAYER, extra_arg=@INT", cg->ib.selected_link_layer, IB_REMOVE_EXTRA_ARG);
}

struct ib_sa_client *nvmeibc_sa_client(const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	return &cg->ib.sa_cli;
}

struct list_head *nvmeibc_get_all_devices(const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_core2main(p));	/* Enforce that this is only accessed by the main WQ */
	return &cg->devs_lists.all;
}

struct list_head *nvmeibc_get_unused_devices(const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_core2main(p));	/* Enforce that this is only accessed by the main WQ */
	return &cg->devs_lists.unused;
}

static void free_port_gid_list(struct nvmeibc_ib_port *ib_port)
{
	struct nvmeib_rdma_ib_port_gid *gid_iter;
	while ((gid_iter = list_first_entry_or_null(&ib_port->gid_list,
		struct nvmeib_rdma_ib_port_gid, link))) {
		list_del(&gid_iter->link);
		kfree(gid_iter);
	}
}

static void replace_port_gid_list(struct nvmeibc_ib_port *ib_port, struct list_head *new_gid_list)
{
	struct nvmeib_rdma_ib_port_gid *gid_iter;

	_NT(t0_replace_port_gid_list,
		"@DEV_NAME:@PORT_NUM, replace gid-list",
		nvmeibc_device_name(ib_port->nic_dev), ib_port->port);

	free_port_gid_list(ib_port);
	list_splice(new_gid_list, &ib_port->gid_list);
	ib_port->n_gids = 0;
	ib_port->gid.valid = false;
	memset(&ib_port->gid.gid, 0, sizeof(ib_port->gid.gid));
	list_for_each_entry(gid_iter, &ib_port->gid_list, link) {
		_NT(t1_replace_port_gid_list,
			"[@INT] gid=@GUID_RAW, v=@BOOL, p=@BOOL",
			ib_port->n_gids, &gid_iter->gid,
			ib_port->gid.valid, gid_iter->preferred);

		if (!ib_port->gid.valid || gid_iter->preferred) {
			ib_port->gid = (*gid_iter);
			INIT_LIST_HEAD(&ib_port->gid.link);
		}
		ib_port->n_gids++;
	}
}

static struct nvmeibc_ib_port* add_port(struct nvmeibc_dev *device, u8 port)
{
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(device);
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_ib_port *ib_port;
	struct ib_port_attr a;
	struct list_head new_gid_list = LIST_HEAD_INIT(new_gid_list);
	int rv = 0;

	NFIN;
	(void)p;
	if (!(ib_port = kzalloc(sizeof *ib_port, GFP_KERNEL)))
		goto out;

	ib_port->nic_dev = device;
	ib_port->port = port;
	ib_port->layer = rdma_port_get_link_layer(P2IB(ib_port), port);
	ib_port->transport_type = rdma_node_get_transport(P2IB(ib_port)->node_type);

	if (ib_query_port(P2IB(ib_port), ib_port->port, &a)) {
		_NE(error_0_main_add_port, "ib_query_port() failed.");
		goto free_port;
	}
	if ((rv = ib_query_pkey(P2IB(ib_port), ib_port->port, 0, &ib_port->pkey))) {
		_NE(error_1_main_add_port, "ib_query_pkey() failed.");
		goto free_port;
	}
	INIT_LIST_HEAD(&ib_port->gid_list);
	ib_port->port_active = a.state == IB_PORT_ACTIVE;
	ib_port->port_used = true;

	if (!nvmeib_use_dev(&cg->devs_lists.used, P2IB(ib_port), (u8)port, &new_gid_list)) {
		_NT(trace_1_main_add_port, "Device @DEV_NAME port @PORT filtered", P2IB(ib_port)->name, port);
		ib_port->port_used = false;
	}
	replace_port_gid_list(ib_port, &new_gid_list);
	if (!ib_port->gid.valid) {
		_NT(trace_2_main_add_port, "Device @DEVICE_NAME port @PORT has no valid gid", P2IB(ib_port)->name, port);
		ib_port->port_used = false;
	}
	else if (cg->ib.selected_link_layer != IB_LINK_LAYER_UNSPECIFIED &&
		ib_port->gid.link_layer != cg->ib.selected_link_layer) {
		/* Link layer has already been chosen and this port doesn't match */
		_NT(trace_3_main_add_port, "Device @DEV_NAME port @PORT filtered out due to link layer",
		    P2IB(ib_port)->name, port);
		ib_port->port_used = false;
	}
	_NT(trace_main_add_port, "gid=@GID port_used=@PORT_USED state=@STATE valid=@VALID",
			ib_port->gid.gid_str, ib_port->port_used, a.state, ib_port->gid.valid);
	/* what should be do here if now dma device? */
	//ib_port->dev.parent = IBDEV2DMADEV(device->dev->ib_dev);
	goto out;

free_port:
	kfree(ib_port);
	ib_port = NULL;

out:
	NFOUT;
	return ib_port;
}

static struct nvmeibc_ib_port *find_dev_port(struct nvmeibc_dev *dev, u8 port)
{
	struct nvmeibc_ib_port *ib_port;
	struct nvmeibc_ib_port *ret = NULL;

	NFIN;
	list_for_each_entry(ib_port, &dev->port_list, port_list_n) {
		if (ib_port->port == port) {
			ret = ib_port;
			break;
		}
	}
	list_for_each_entry(ib_port, &dev->unused_port_list, port_list_n) {
		if (ib_port->port == port) {
			ret = ib_port;
			break;
		}
	}
	NFOUT;
	return ret;
}

static int activate_device(const struct nvmeibc_cinst_params_core *p,
			   struct nvmeibc_dev *nic_dev);

struct nvmeibc_port_update_data {
	struct ib_device *ib_dev;
	u8 port_num;
	enum ib_event_type event;
	const struct nvmeibc_cinst_params_core *cips;
};

static int update_port_work_fn(void *param)
{
	struct nvmeibc_port_update_data *update_port = param;
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(update_port);
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_dev *nic_dev = NULL;
	u8 port_num = update_port->port_num;
	struct nvmeibc_ib_port *ib_port = NULL;
	struct ib_port_attr a;
	int rv = 0;
	enum nvmeibc_disk_update_type port_update_type = DISK_NO_UPDATE;

	NFIN;
	if (!(nic_dev = ib_get_client_data(update_port->ib_dev, &cg->ib.cli))) {
		_NT(trace_main_update_port_work_fn, "IB device @IB_DEV_NAME has no nic_dev. Must be going down", update_port->ib_dev->name);
		rv = -ENODEV;
		goto out;
	}

	if (!(ib_port = find_dev_port(nic_dev, port_num))) {
		_NE(error_main_update_port_work_fn, "@DEV_NAME: Unknown Port @PORT_NUM of Device", nvmeibc_device_name(nic_dev), port_num);
		rv = -ENODEV;
		goto out;
	}

	/* Update the port */
	if ((rv = ib_query_port(P2IB(ib_port), ib_port->port, &a))) {
		_NE(error_1_main_update_port_work_fn, "ib_query_port() failed with error @RV.", rv);
		goto port_err;
	}

	if ((rv = ib_query_pkey(P2IB(ib_port), ib_port->port, 0, &ib_port->pkey))) {
		_NE(error_2_main_update_port_work_fn, "ib_query_pkey() failed with error @RV.", rv);
		goto port_err;
	}

	if (update_port->event == IB_EVENT_GID_CHANGE ||
		update_port->event == IB_EVENT_CLIENT_REREGISTER || /* update port on subnet-prefix change */
		update_port->event == IB_EVENT_SM_CHANGE) {
		union ib_gid curr_gid = ib_port->gid.gid;
		struct list_head new_gid_list = LIST_HEAD_INIT(new_gid_list);
		bool port_used;
		/* Check to see if the port/dev is still enabled (new GID may be filtered out) */
		port_used = nvmeib_use_dev(&cg->devs_lists.used, P2IB(ib_port), ib_port->port, &new_gid_list);
		replace_port_gid_list(ib_port, &new_gid_list);
		if (!port_used ||
				(cg->ib.selected_link_layer != IB_LINK_LAYER_UNSPECIFIED &&
				ib_port->gid.link_layer != cg->ib.selected_link_layer)) {
			/* If port was previously used, set it as unused and remove from disks */
			if (ib_port->port_used) {
				_NT(trace_1_main_update_port_work_fn, "Device @IB_DEV_NAME port @PORT is now unused due to GID / link-layer change",
					P2IB(ib_port)->name, ib_port->port);
				ib_port->port_used = false;
				list_del(&ib_port->port_list_n);
				list_add_tail(&ib_port->port_list_n, &nic_dev->unused_port_list);
				port_update_type = DISK_UPDATE_REMOVE_PORT;
			}
		} else {
			/* If port was previously unused, initialize it (and maybe its device) for use */
			if (!ib_port->port_used) {
				_NT(trace_2_main_update_port_work_fn, "Device @IB_DEV_NAME port @PORT is now used due to GID / link-layer change",
					P2IB(ib_port)->name, ib_port->port);
				ib_port->port_used = true;
				list_del(&ib_port->port_list_n);
				list_add_tail(&ib_port->port_list_n, &nic_dev->port_list);
				if (cg->ib.selected_link_layer == IB_LINK_LAYER_UNSPECIFIED)
					cg->ib.selected_link_layer = ib_port->gid.link_layer;
				if (!nic_dev->device_used) {
					/* Device has now become used */
					if ((rv = activate_device(p, nic_dev)) < 0) {
						_NT(trace_3_main_update_port_work_fn, "Failed to activate device @IB_DEV_NAME (@RV)", nic_dev->dev->ib_dev->name, rv);
					} else {
						/* Move nic to from unused to device list */
						list_del(&nic_dev->dev_list_n);
						list_add_tail(&nic_dev->dev_list_n, nvmeibc_get_all_devices(p));
						/* Add nic to disks */
						port_update_type = DISK_UPDATE_ADD_NIC;
					}
				} else
					port_update_type = DISK_UPDATE_ADD_PORT;
			} else {
				/* GID change but still port is NOT filtered out */
				port_update_type = DISK_UPDATE_PORT_UPDATE;
			}
		}

		if (curr_gid.global.subnet_prefix != ib_port->gid.gid.global.subnet_prefix ||
			curr_gid.global.interface_id != ib_port->gid.gid.global.interface_id)
			_NT(trace_4_main_update_port_work_fn, "Device @IB_DEV_NAME port @PORT selected GID Changed from @RAW_IPV6 to @RAW_IPV6",
			   P2IB(ib_port)->name, ib_port->port, &curr_gid.raw, &ib_port->gid.gid.raw);
	}
	else if (ib_port->port_used) {
		/* For other events just inform disk that a used port has updated */
		port_update_type = DISK_UPDATE_PORT_UPDATE;
	}

	ib_port->layer = ib_port->gid.link_layer;
	ib_port->port_active = a.state == IB_PORT_ACTIVE;
	_NI(trace_5_main_update_port_work_fn, DMESG_MOD_PREFIX ": @IB_DEV_NAME: device port @PORT GID @RAW_IPV6 now @DEV_STATE",
	   P2IB(ib_port)->name, ib_port->port, &ib_port->gid.gid.raw,
	    (nvmeibc_ib_port_enabled(ib_port) ? "Enabled" : "Disabled"));

	/* Update port on all disks */
	if (port_update_type != DISK_NO_UPDATE) {
		void *update_data = port_update_type == DISK_UPDATE_ADD_NIC ? (void *)nic_dev : (void *)ib_port;
		if ((rv = update_disks_config(p, port_update_type, update_data, NULL, NULL, NULL, true))) {
			_NE(error_3_main_update_port_work_fn, "Error @RV adding port/nic using update_disks_config", rv);
		}
	}

out:
	/* Free the parameter */
	kfree(param);
	// nvmeib_set_roce_lossy_mode_on();
	NFOUT;
	return rv;

port_err:
	if (ib_port->port_used) {
		ib_port->port_used = false;
		list_del(&ib_port->port_list_n);
		list_add_tail(&ib_port->port_list_n, &nic_dev->unused_port_list);
	}
	goto out;
}

bool nvmeibc_is_arnic_local(struct nvmeib_rdma_ib_port_gid *port_gids, struct nvmeibc_admin_rnic *arnic)
{
	bool found = false;

	NFIN;
	if (arnic->link_layer != port_gids->link_layer)
		goto out;
	if (arnic->transport_type != port_gids->transport_type)
		goto out;
	if (arnic->link_layer == IB_LINK_LAYER_INFINIBAND) {
		/* For IB, we ignore the subnet mask because it might be out of date */
		found = port_gids->gid.global.interface_id == arnic->ib_gid.global.interface_id;
		goto out;
	} else
		found = memcmp(&arnic->ib_gid, &port_gids->gid, 16) == 0;

out:
	NFOUT;
	return found;
}

static void remove_port(struct nvmeibc_ib_port *ib_port)
{
	NFIN;
	free_port_gid_list(ib_port);
	kfree(ib_port);
	NFOUT;
}

static int allocate_fmr(struct nvmeibc_dev *nic_dev)
{
	struct ib_fmr_pool *fmr_pool = NULL;
	struct nvmeib_fr_pool *fr_pool = NULL;
	int rv = 0;
	NFIN;

	if (nic_dev->dev->fmr_pool || nic_dev->dev->fr_pool) {
		_NE(error_0_main_allocate_fmr, "MRs leak");
		WARN_ON(1);
	}

	/* allocate fast memory registration pool */
	if (!nvmeib_alloc_fast_reg_pool(nic_dev->dev, &fmr_pool, &fr_pool, -1, c_dev_fr_pool)) {
		if (fmr_pool)
			nic_dev->dev->fmr_pool = fmr_pool;
		else if (fr_pool)
			nic_dev->dev->fr_pool = fr_pool;
	}
	if (!fmr_pool && !fr_pool) {
		_NE(error_main_allocate_fmr, "Cannot allocate port MR.");
		rv = -1;
	}

	NFOUT;
	return rv;
}

static void free_fmr(struct nvmeibc_dev *nic_dev)
{
	struct nvmeib_dev *dev = nic_dev->dev;
	NFIN;
	/* destroy fast registration pool */
	if (nic_dev->dev->use_fast_reg) {
		if (nic_dev->dev->fr_pool) {
			struct nvmeib_keeper_frs_info *frs_info = &dev->keeper_frs_info;
			struct nvmeib_keeper_ops *keeper_ops = NULL;
			/* [NVMESH-6668]: Look for keeper module that will hold the FR MRs for the next run */
			if (nvmeib_dev_use_keeper(dev) && (keeper_ops = nvmeib_public_get_keeper())) {
				frs_info->version = NVMEIB_KEEPER_FRS_INFO_VERSION;
				frs_info->mr_arr = krealloc(
					frs_info->mr_arr, 
					sizeof(*frs_info->mr_arr) * dev->fr_pool->size, 
					GFP_KERNEL);

				if (frs_info->mr_arr) {
					frs_info->n_mr = dev->fr_pool->size;
					frs_info->mr_page_mask = dev->mr_page_mask;
					frs_info->mr_page_size = dev->mr_page_size;
					frs_info->mr_max_size = dev->mr_max_size;
					frs_info->max_pages_per_mr = dev->max_pages_per_mr;
				} else {
					_NW(warn_free_fmr_oom, "OOM");
					frs_info->n_mr = 0;
				}
			} else {
				/* Keeper not found - free array */
				kfree(frs_info->mr_arr);
				frs_info->mr_arr = NULL;
				frs_info->n_mr = 0;
			}
			nvmeib_destroy_fast_reg_pool(nic_dev->dev->fr_pool, frs_info->mr_arr, &frs_info->n_mr, c_dev_fr_pool, nic_dev->dev->fr_pool->size);
			nic_dev->dev->fr_pool = NULL;
			
			if (keeper_ops) {
				if (frs_info->n_mr) {
					frs_info->dma_mr = dev->mr;
					frs_info->pd = dev->pd;
					dev->save_to_keeper = true;
				}
				nvmeib_public_put_keeper();
			}
		}
	} else if (nic_dev->dev->fmr_pool) {
#if KS_IB_VERBS_SUPPORTS_FMR
		ib_destroy_fmr_pool(nic_dev->dev->fmr_pool);
		nic_dev->dev->fmr_pool = NULL;
#else
	BUG();
#endif
	}
	NFOUT;
}


static void remove_nic(struct nvmeibc_dev *nic_dev)
{
	NFIN;

	if (nic_dev->device_used) {
		/* free the fmr pool */
		free_fmr(nic_dev);
		nvmeib_public_proc_remove(nic_dev->procfs_status);
	}

	nvmeib_free(nic_dev->dev);
	kfree(nic_dev);
	NFOUT;
}

struct start_ib_work {
	struct workqe_struct work;
	struct completion *comp;
	int rv;
};

/* ensure that all devices have the same layer type
   in general if no filter is used and we are in mixed system
   the infiniband cards will be used.
   if filters are imposed on the system, and the selected devices are mixed,
   an error will be returned*/
static int ensure_devs_uniformity(const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	int rv = 0;
	struct nvmeibc_dev *nic_dev, *tmp_nic;
	struct nvmeibc_ib_port *ib_port, *tmp;
	enum rdma_link_layer selected_layer = IB_LINK_LAYER_UNSPECIFIED;
	bool mixed = false;
	struct list_head *dev_list;

	NFIN;
	dev_list = nvmeibc_get_all_devices(p);
	list_for_each_entry(nic_dev, dev_list, dev_list_n) {
		list_for_each_entry(ib_port, &nic_dev->port_list, port_list_n) {
			switch (selected_layer) {
			case IB_LINK_LAYER_UNSPECIFIED:
				if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
					selected_layer = IB_LINK_LAYER_INFINIBAND;
				else if (ib_port->layer == IB_LINK_LAYER_ETHERNET)
					selected_layer = IB_LINK_LAYER_ETHERNET;
				break;
			case IB_LINK_LAYER_INFINIBAND:
				if (ib_port->layer == IB_LINK_LAYER_ETHERNET)
					mixed = true;
				break;
			case IB_LINK_LAYER_ETHERNET:
				if (ib_port->layer == IB_LINK_LAYER_INFINIBAND) {
					selected_layer = IB_LINK_LAYER_INFINIBAND;
					mixed = true;
				}
				break;
			default:
				_NE(error_main_ensure_devs_uniformity, "Unknown layer type @SELECTED_LAYER", selected_layer);
				BUG();
			}
		}
	}

	_NT(trace_main_ensure_devs_uniformity, "Selected link layer is @SELECTED_LAYER mixed=@MIXED", selected_layer, mixed ? "true" : "false");

	if (mixed) {
		if (!list_empty(&cg->devs_lists.used))
			_NE_to_user(error_1_main_ensure_devs_uniformity, DMESG_MOD_PREFIX, "Configured network devices list contains both Infiniband and Ethernet devices, which is not supported, fix list in the configuration file and restart services. Error code: 1044. Internal filter ports: @NVMEIBC_FILTER_PORTS.", nvmeibc_filter_ports);
		else
			_NE_to_user(error_2_main_ensure_devs_uniformity, DMESG_MOD_PREFIX, "No network device limitations were configured and there are both Infiniband and Ethernet devices, which is not supported, fix list in the configuration file and restart services. Error code: 1045. Internal filter ports: @NVMEIBC_FILTER_PORTS.", nvmeibc_filter_ports);
		rv = -EINVAL;
		goto out;
	}

	/* Set the global selected link layer */
	cg->ib.selected_link_layer = selected_layer;

	if (!mixed)
		goto out;

	list_for_each_entry_safe(nic_dev, tmp_nic, dev_list, dev_list_n) {
		list_for_each_entry_safe(ib_port, tmp, &nic_dev->port_list, port_list_n) {
			if (ib_port->layer != selected_layer) {
				_NW(warn_main_ensure_devs_uniformity, "port @GID_STR is removed", ib_port->gid.gid_str);
				list_del(&ib_port->port_list_n);
				remove_port(ib_port);
			}
		}
		if (list_empty(&nic_dev->port_list)) {
			_NW(warn_1_main_ensure_devs_uniformity, "Nic removed");
			ib_set_client_data(nic_dev->dev->ib_dev, &cg->ib.cli, NULL);
			nvmeib_rdma_unregister_event_handler(nic_dev->event_handler);
			list_del(&nic_dev->dev_list_n);
			remove_nic(nic_dev);
		}
	}

	if (list_empty(dev_list))
		rv = -ENODEV;

out:
	NFOUT;
	return rv;
}

static int clnt_start_ib_work_fn(void *param)
{
	const struct nvmeibc_cinst_params_core *p = param;
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	int rv = 0;
	NFIN;
	if ((rv = nvmeibc_cg_ib_register_client(cg))) {
		goto out;
	}

	if ((rv = ensure_devs_uniformity(p))) {
		_NE(error_main_clnt_start_ib_work_fn,
			"ensure_devs_uniformity failed (@RV)", rv);
		goto err_unreg;
	}

	goto out;

out:
	NFOUT;
	return rv;

err_unreg:
	nvmeibc_cg_ib_unregister_client(cg);
	goto out;
}

/**
 * nvmeibc_ib_event_handler() - Asynchronous IB event
 * callback function.
 *
 * Callback function called by the InfiniBand core when an asynchronous IB
 * event occurs. This callback may occur in interrupt context. See also
 * section 11.5.2, Set Asynchronous Event Handler in the InfiniBand
 * Architecture Specification.
 */
static void nvmeibc_ib_event_handler(struct nvmeib_rdma_event_handler *event_handler,
			   struct ib_event *event)
{
	const struct nvmeibc_cinst_params_core *p = event_handler->ctx;
	struct ib_device *ib_dev = event_handler->ib_dev;
	static const char *event_desc[] = {
		"CQ Error",
		"QP Fatal",
		"QP Request Error",
		"QP Access Error",
		"Comm Established",
		"SQ Drained",
		"Path Migration",
		"Path Migration Error",
		"Device Fatal",
		"Port Active",
		"Port Error",
		"LID Change",
		"P-Key Change",
		"SM Change",
		"SRQ Error",
		"SRQ Limit Reached",
		"Last WQE Reached",
		"Client Reregister",
		"GID Change"
	};
	NFIN;

	if (event->event >= ARRAY_SIZE(event_desc)) {
		_NE(t_a5_ibeh, DMESG_MOD_PREFIX ": @DEV_NAME: Invalid ASYNC event @EVENT for device ", event->device->name, event->event);
		goto out;
	}

	_NI(t_a6_ibeh, DMESG_MOD_PREFIX ": @IB_DEV_NAME: Got ASYNC Event event_desc=@EVENT_DESC on device", ib_dev->name, event_desc[event->event]);

	switch (event->event) {
	case IB_EVENT_PORT_ERR:
	case IB_EVENT_PKEY_CHANGE:
	case IB_EVENT_PORT_ACTIVE:
	case IB_EVENT_GID_CHANGE:
	case IB_EVENT_CLIENT_REREGISTER:
	case IB_EVENT_SM_CHANGE:
	{
		struct nvmeibc_port_update_data *param = kzalloc(sizeof(*param), GFP_ATOMIC);
		int sts;
		int rv = 0;

		if (!param) {
			_NE(t_a7_ibeh, "Memory allocation error");
			goto out;
		}

		param->ib_dev = ib_dev;
		param->port_num = event->element.port_num;
		param->event = event->event;
		nvmeibc_cinst_get_core_p(param) = p;

		_NI(t_a8_ibeh, "ASYNC Event event_desc=@EVENT_DESC is on Port @PORT_NUM of Device @IB_DEV_NAME. Running update on Main WQ",
			event_desc[event->event], event->element.port_num, ib_dev->name);
		if ((rv = nvmeibc_run_on_main_wq(nvmeibc_isnt_params_core2main(p), update_port_work_fn, param, false, false,&sts))) {
			if (rv == -ENOSYS) {
				// client is going down. main wq was stopped
				_NT(t_a9_ibeh, "ignore async event - client is shutting down");
				kfree(param);

			} else {
				_NE(t_aa_ibeh, "Error @RV updating port (port_update_work_fn)", rv);
				if (sts < 0) {
					_NE(t_ab_ibeh, "Fail to add work (nor was it ran inplace), free params");
					kfree(param);
				}
			}
		}
	} break;
	case IB_EVENT_DEVICE_FATAL:
	{
		_NE(client_ib_event_fatal, "IB_EVENT_DEVICE_FATAL received in client");
	} break;
	default:
		break;
	}

out:
	NFOUT;
}

static void nvmeib_nic_create_proc_file(const struct nvmeibc_cinst_params_main *p, 
					struct nvmeibc_dev *nic_dev);

static int activate_device(const struct nvmeibc_cinst_params_core *p,
			   struct nvmeibc_dev *nic_dev)
{
	int rv = 0;
	NFIN;

	if (nic_dev->dev->dev_type != DT_uknown) {
		if ((rv = allocate_fmr(nic_dev)) < 0) {
			_NE(error_main_activate_device, "@IB_DEV_NAME: allocate_fmr() failed.", nic_dev->dev->ib_dev->name);
			goto out;
		}
	}
	if (!(nic_dev->stats = nvmeib_io_stats_create(nic_dev->dev->ib_dev->name, VERB_RW_T_RECOV_GEN_BITMASK, nvmeibc_nic_io_stats_block_size))) {
		_NE(error_2_main_activate_device, "@IB_DEV_NAME: stats create failed.", nic_dev->dev->ib_dev->name);
		rv = -ENOMEM;
		goto out;
	}
	nvmeib_nic_create_proc_file(nvmeibc_isnt_params_core2main(p), nic_dev);
	nic_dev->device_used = true;
	nic_dev->add_jif = jiffies;
	NFOUT;
out:
	return rv;
}

static int create_srq_pool(const struct nvmeibc_cinst_params_core *p, struct nvmeib_dev *dev)
{
	struct nvmeib_srq_params prim;
	struct nvmeib_srq_params sec, *psec = NULL;
	int rv = -1;
	NFIN;

	prim.q_size = NVMEIBC_MAX_VOLUME_SRQ; //TODO: reduce if using secondary SRQs
	prim.msg_size = roundup_pow_of_two(NVMEIBS_MAX_ADMIN_MSG_SIZE);
	prim.srq_limit = 1;
	if (p->max_nic_srqs > 1) {
		sec.q_size = NVMEIB_NORDDA_SRQ_MAX_SIZE;
		sec.msg_size = roundup_pow_of_two(NVMEIBS_NORDDA_SERVER_MSG_SIZE);
		sec.srq_limit = 1;
		psec = &sec;
	}
	rv = nvmeib_srq_pool_create(dev, p->max_nic_srqs, &prim, psec, c_dev_srq);

	NFOUT;
	return rv;
}

static int create_cq_srq(struct nvmeib_dev *dev)
{
	int rv = -1;
	NFIN;

	rv = nvmeib_create_cq_srq(dev,
		MAX(NVMEIBS_MAX_ADMIN_MSG_SIZE, NVMEIBS_NORDDA_SERVER_MSG_SIZE),
		c_dev_srq);

	NFOUT;
	return rv;
}

static int nvmeibc_srq_create(const struct nvmeibc_cinst_params_core *p, struct nvmeib_dev *dev)
{
	int rv;
	NFIN;

	rv = !nvmeibc_use_pcpu_cq ?
		create_srq_pool(p, dev) :	/* populates dev->srqs */
		create_cq_srq(dev);		/* populates dev->cqs[].srq_info */

	NFOUT;

	return rv;
}

#define CORE_CLIENT_NETSTAT_PROC_FRMT_VER 2 /* Bumped to 2 due to fix for [NVMESH-6726] */
static ssize_t procfs_netstat_fill(void *arg, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibc_dev *nis_dev = arg;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	ssize_t count  = 0, indent = 0;
	unsigned long nic_uptime = jiffies - nis_dev->add_jif;
	NFIN;
	count += jops->start_obj(buf + count, len - count, NULL, indent++);
	count += nvmeib_io_stats_to_json(nis_dev->stats, buf+count, len-count, nic_uptime, jops, indent, true);
	count += nvmeib_proc_add_json_proc_epilog(CORE_CLIENT_NETSTAT_PROC_FRMT_VER, buf + count, len - count);
	count += jops->end_obj(buf + count, len - count, JSON_LAST_ELEM, --indent);
	NFOUT;
	return count;
#undef BUF_ADD
}

static ssize_t procfs_netstat_clear(void *arg, char *buf, size_t len)
{
       struct nvmeibc_dev *nis_dev = arg;
       int reset;
       ssize_t rv;

       if (sscanf(buf, "%d", &reset) != 1 || reset != 0) {
               rv = -EINVAL;
               goto out;
       }

       nvmeib_io_stats_clear(nis_dev->stats, 'A');

       rv = len;

out:
       return rv;
}

static void nvmeib_nic_create_proc_file(const struct nvmeibc_cinst_params_main *p, struct nvmeibc_dev *nic_dev)
{
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(p);
	char dir_name[IB_DEVICE_NAME_MAX + 1];

	snprintf(dir_name, sizeof(dir_name) - 1, "%s", nic_dev->dev->ib_dev->name);
	nic_dev->proc_dir = proc_mkdir(dir_name, _mg->proc_dir.net);
	if (!nic_dev->proc_dir) {
		_NE(error_main_nvmeib_nic_create_proc_file, "@DIR_NAME: failed to create proc net dir", dir_name);
		return;
	}

	nic_dev->procfs_status = nvmeib_public_proc_create("iostats.json",nic_dev->proc_dir,procfs_netstat_fill,procfs_netstat_clear,nic_dev);
	if (!nic_dev->procfs_status) {
		_NE(error_1_main_nvmeib_nic_create_proc_file, "failed to create @DIR_NAME/iostats.json proc file", dir_name);
		remove_proc_entry(dir_name, _mg->proc_dir.net);
		nic_dev->proc_dir = NULL;
	}
}

static void nvmeib_nic_remove_proc_file(const struct nvmeibc_cinst_params_main *p, struct nvmeibc_dev *nic_dev)
{
	struct t_main_clnt_globals *_mg = __get_from_params_main_globals_container(p);
	char dir_name[IB_DEVICE_NAME_MAX + 1];

	if (nic_dev->procfs_status) {
		nvmeib_public_proc_remove(nic_dev->procfs_status);
		nic_dev->procfs_status = NULL;
	}
	if (nic_dev->proc_dir) {
		snprintf(dir_name, sizeof(dir_name) - 1, "%s", nic_dev->dev->ib_dev->name);
		remove_proc_entry(dir_name, _mg->proc_dir.net);
		nic_dev->proc_dir = NULL;
	}
}

bool nvmeibc_support_srq(struct nvmeib_dev *dev)
{
	return (nvmeibc_max_nic_srqs > 0) && nvmeib_support_srq(dev);		// Daniel: Todo use p->max_nic_srqs
}

struct add_one_params {
	const struct nvmeibc_cinst_params_core *cips;
	struct ib_device *device;
};

static int add_one_work_fn(void *_param)
{
	struct add_one_params *work = _param;
	struct ib_device *device = work->device;
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(work);
	const struct nvmeibc_cinst_params_blk *blk = nvmeibc_isnt_params_core2blk(p);
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_dev *nic_dev;
	struct nvmeib_dev *dev;
	struct nvmeibc_ib_port *ib_port;
	int start_port, end_port, port;
	int rv = 0;
	struct list_head gid_list = LIST_HEAD_INIT(gid_list);

	NFIN;
	kfree(_param);

	if (!(dev = nvmeib_init(device, blk->dev_name.str, nvmeibc_use_pcpu_cq, nvmeibc_pcpu_cq_poll_proc))) {
		_NE(error_main_add_one_work_fn, "@DEV_NAME: Failed to init device", device->name);
		goto err;
	}

	if (nvmeib_init_fast_reg(dev) < 0) //omril: called in nvmeib_init() too
		goto free_dev;

	nic_dev = kzalloc(sizeof(*nic_dev), GFP_KERNEL);
	if (!nic_dev)
		goto free_dev;

	nic_dev->dev = dev;
	nvmeibc_cinst_get_core_p(nic_dev) = p;

	if (nvmeibc_support_srq(dev)) {
		if (nvmeibc_srq_create(p, dev) < 0) {
			_NE(error_1_main_add_one_work_fn, "@DEV_NAME: Fail to create shared request queue", device->name);
			goto free_nic_dev;
		}
	} else
		_NI(trace_main_add_one_work_fn, "@DEV_NAME: Working without SRQ", device->name);

	INIT_LIST_HEAD(&nic_dev->port_list);
	INIT_LIST_HEAD(&nic_dev->unused_port_list);
	INIT_LIST_HEAD(&nic_dev->dev_list_n);

	if (device->node_type == RDMA_NODE_IB_SWITCH) {
		start_port = 0;
		end_port = 0;
	} else {
		start_port = 1;
		end_port = nic_dev->dev->phys_port_cnt;
	}

	ib_set_client_data(device, &cg->ib.cli, nic_dev);

	if ((rv = nvmeib_rdma_register_event_handler(device, nvmeibc_ib_event_handler,
		(void*)p, &nic_dev->event_handler))) {
		_NE(error_2_main_add_one_work_fn, "@DEV_NAME nvmeib_rdma_register_event_handler() failed(@RV).", device->name, rv);
		goto free_nic_dev;
	}

	_NT(trace_1_main_add_one_work_fn, "ULL: dev @DEV_NAME: @DEV, s=@START_PORT, e=@END_PORT", device->name, device, start_port, end_port);
	for (port = start_port; port <= end_port; ++port) {
		ib_port = add_port(nic_dev, port);
		if (ib_port) {
			if (ib_port->port_used) {
				list_add_tail(&ib_port->port_list_n, &nic_dev->port_list);
				nic_dev->device_used = true;
			} else {
				list_add_tail(&ib_port->port_list_n, &nic_dev->unused_port_list);
			}
		} else {
			_NT(trace_2_main_add_one_work_fn, "Failed to add port - device @DEV_NAME port @PORT",
			    device->name, port);
		}
	}

	if (nic_dev->device_used) {
		if ((rv = activate_device(p, nic_dev)) < 0) {
			_NE(error_3_main_add_one_work_fn, "@DEV_NAME: Error activating nic. rv=@RV", device->name, rv);
			goto unregister_event_handler;
		}

		list_add_tail(&nic_dev->dev_list_n, nvmeibc_get_all_devices(p));

		/* Use disk update function to add nic to the disks. */
		/* On module init, the disk list will be empty so this will just run the pre fn. */
		if ((rv = update_disks_config(p, DISK_UPDATE_ADD_NIC, nic_dev, NULL, NULL, NULL, true))) {
			_NE(error_4_main_add_one_work_fn, "@DEV_NAME: Error @RV adding new NIC using update_disks_config", device->name, rv);
			remove_nic(nic_dev);
			goto out;
		}

		_NI(trace_4_main_add_one_work_fn, DMESG_MOD_PREFIX ": @DEV_NAME: Device is OK to use", device->name);
	}
	else {
		list_add_tail(&nic_dev->dev_list_n, &cg->devs_lists.unused);
		_NW(warn_main_add_one_work_fn, DMESG_MOD_PREFIX ": @DEV_NAME: Device is not currently used due to filter / link-layer", device->name);
	}

	goto out;

unregister_event_handler:
	/* Remove NIC event handler */
	nvmeib_rdma_unregister_event_handler(nic_dev->event_handler);

	/* Clear IB client NIC pointer */
	ib_set_client_data(nic_dev->dev->ib_dev, &cg->ib.cli, NULL);


free_nic_dev:
	nvmeib_nic_remove_proc_file(nvmeibc_isnt_params_core2main(p), nic_dev);
	nvmeib_io_stats_free(nic_dev->stats);
	nvmeib_free(nic_dev->dev);
	kfree(nic_dev);
	goto out; //dev is already free, remember that nic_dev->dev = dev

free_dev:
	nvmeib_free(dev);

err:
	_NE(error_out_main_add_one_work_fn, DMESG_MOD_PREFIX ": @DEV_NAME: Fail to add device", device->name);

out:
	NFOUT;
	return rv;
}

static void add_one(const struct nvmeibc_cinst_params_core *p, struct ib_device *device)
{
	struct add_one_params *params;
	const enum nvmeib_dev_type dev_type = nvmeib_get_device_type(device);
	int rv;

	_NI(trace_main_add_one, "add_one called for IB Device @DEV_NAME dev_type=@NUM ptr=@PTR. Running add_one_work_fn on Main WQ", device->name, dev_type, device);

	if (dev_type == DT_siw && p->tcp_mode == 0) {
		_NI(add_one_trc_1041, DMESG_MOD_PREFIX ": @DEV_NAME: Skipping TCP transport due to TCP Mode == 0", device->name);
		goto out;
	}
	else if (dev_type != DT_siw && p->tcp_mode == 1) {
		_NI(add_one_trc_1048, DMESG_MOD_PREFIX ": @DEV_NAME: Skipping RDMA transport due to TCP Mode == 1", device->name);
		goto out;
	}
	else if (nvmeib_is_dev_in_blacklist(device)) {
		_NI(trace_add_one_dev_bl, "@DEV_NAME: Device is blacklisted, not using", device->name);
		goto out;
	}

	if ((params = kmalloc(sizeof(*params), GFP_KERNEL))) {
		nvmeibc_cinst_get_core_p(params) = p;
		params->device = device;
		if ((rv = nvmeibc_run_on_main_wq(nvmeibc_isnt_params_core2main(p), add_one_work_fn, params, true, false, NULL)))
			_NE(error_main_add_one, "Error @RV running add_one_work_fn", rv);
	} else {
		rv = -ENOMEM;
	}

out:
	return;
}

static void remove_nic_post_fn(const struct nvmeibc_cinst_params_core *p, enum nvmeibc_disk_update_type update_type, void *update_data)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_dev *nic_dev = update_data;
	NFIN;
	(void)update_type;
	_NI(trace_main_remove_nic_post_fn, "@DEV_NAME: Finished removing NIC from disks. Bringing it down", nvmeibc_device_name(nic_dev));

	/* Remove NIC event handler */
	nvmeib_rdma_unregister_event_handler(nic_dev->event_handler);

	/* Clear IB client NIC pointer */
	ib_set_client_data(nic_dev->dev->ib_dev, &cg->ib.cli, NULL);

	/* Remove NIC from device list */
	list_del(&nic_dev->dev_list_n);
	_NI(trace_1_main_remove_nic_post_fn, "@IB_DEV_NAME: Hot remove of NIC completed", nic_dev->dev->ib_dev->name);
	NFOUT;
}

static void remove_one(const struct nvmeibc_cinst_params_core *p, struct ib_device *device)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_dev *nic_dev;
	struct nvmeibc_ib_port *ib_port, *tmp_ib_port;
	int rv;

	NFIN;
	_NT(trace_main_remove_one, "Remove one called");
	nic_dev = ib_get_client_data(device, &cg->ib.cli);
	if (!nic_dev) {
		NFOUT;
		return;
	}

	/* Call update_disks_config to schedule a remove nic task and wait for completion.*/
	if ((rv = update_disks_config(p, DISK_UPDATE_REMOVE_NIC, nic_dev, NULL, NULL, remove_nic_post_fn, true))) {
		if (rv == -ENOSYS) {
			/* Main WQ is not up so just call the post fn directly */
			remove_nic_post_fn(p, DISK_UPDATE_REMOVE_NIC, nic_dev);
		} else {
			/* Unknown error - Crash */
			BUG();
		}
	}

	list_for_each_entry_safe(ib_port, tmp_ib_port, &nic_dev->port_list,
		port_list_n)
		remove_port(ib_port);
	list_for_each_entry_safe(ib_port, tmp_ib_port, &nic_dev->unused_port_list,
		port_list_n)
		remove_port(ib_port);

	nvmeib_nic_remove_proc_file(nvmeibc_isnt_params_core2main(p), nic_dev);
	nvmeib_io_stats_free(nic_dev->stats);
	remove_nic(nic_dev);
	NFOUT;
}

static void remove_ib(const struct nvmeibc_cinst_params_core *p)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);

	NFIN;

	nvmeibc_cg_ib_unregister_client(cg);
	nvmeibc_cg_ib_sa_unregister_client(cg);

	if (cg->local_server) {
		kfree(cg->local_server);
		cg->local_server = NULL;
	}
	NFOUT;
}

struct nvmeib_local_server *nvmeibc_get_local_server(const struct nvmeibc_cinst_params_core *p)
{
	nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_core2main(p));
	return __get_from_params_core_globals_container(p)->local_server;
}

/* Called when all disks have been updated with the new local server.
 * Can now update the main local server - we are in the context of the main wq so don't need to lock */
static void update_local_server_post_fn(const struct nvmeibc_cinst_params_core *p, enum nvmeibc_disk_update_type update_type, void *update_data)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeib_local_server *old_local_server = cg->local_server;
	(void)update_type;
	cg->local_server = update_data;
	kfree(old_local_server);
}

static void set_local_server(struct nvmeib_local_server *s, void *arg)
{
	struct nvmeibc_local_server *new_local_server = NULL;
	const struct nvmeibc_cinst_params_core *p = arg;
	if (s)
		new_local_server = kmemdup(s, sizeof(*s), GFP_KERNEL);
	else
		_NT(trace_main_set_local_server, "Removing local server");

	if (update_disks_config(p, DISK_UPDATE_LOCAL_SRV, new_local_server,
		NULL, NULL, update_local_server_post_fn, true)) {
		/* Update failed - free new_local_server */
		kfree(new_local_server);
	}
}

static void remove_local_server(void *arg)
{
	const struct nvmeibc_cinst_params_core *p = arg;
	update_disks_config(p, DISK_UPDATE_LOCAL_SRV, NULL, NULL, NULL,
			    update_local_server_post_fn, true);
}

static void nvmeibc_core_set_local_server_notification(const struct nvmeibc_cinst_params_core *p, bool is_on)
{
	if (is_on) {
		struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
		if (!cg->ib.was_notification_set) {
			nvmeib_set_local_server_notification_calbacks(set_local_server, (void*)p, remove_local_server);
			cg->ib.was_notification_set = true;
		}
	} else {
		struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
		if (cg->ib.was_notification_set) {
			nvmeib_set_local_server_notification_calbacks(NULL, (void*)p, NULL);
			cg->ib.was_notification_set = false;
		}
	}
}

static int lnic_alloc_n_add_ports(const struct nvmeibc_cinst_params_core *p,
								  struct nvmeibc_local_nic *ln,
								  struct list_head *port_list)
{
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_local_nic_port *lnp;
	struct nvmeibc_ib_port *port;
	int rv;
	NFIN;

	list_for_each_entry(port, port_list, port_list_n) {
		if (cg->ib.selected_link_layer != IB_LINK_LAYER_UNSPECIFIED &&
			port->layer != cg->ib.selected_link_layer) {
			_NI(trace_main_lnic_alloc_n_add_ports, DMESG_MOD_PREFIX ": Skip hw-gid=@GID_RAW with link-layer=@LAYER (selected-layer=@LAYER)", port->gid.hw_gid.raw, port->layer, cg->ib.selected_link_layer);
			continue;
		}

		if (!(lnp = kzalloc(sizeof(*lnp), GFP_KERNEL))) {
			_NE(error_main_lnic_alloc_n_add_ports, "Fail to alloc lnic port");
			rv = -ENOMEM;
			goto out;
		}
		lnp->ib_port = port;
		list_add_tail(&lnp->link, &ln->ports);
		ln->n_ports++;
	}
	rv = 0;

out:
	NFOUT;
	return rv;
}

static struct nvmeibc_local_nic *create_local_nic_from_nic_dev(struct nvmeibc_dev *nic_dev)
{
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(nic_dev);
	struct nvmeibc_local_nic *ln = NULL;
	struct nvmeibc_local_nic_port *lnp, *tmp_lnp;

	NFIN;
	if (list_empty(&nic_dev->port_list)) {
		_NT(trace_main_create_local_nic_from_nic_dev, "NIC @DEV_NAME - add although no used ports",
		   nvmeibc_device_name(nic_dev));
	}

	if (!(ln = kzalloc(sizeof(*ln), GFP_KERNEL))) {
		_NE(error_main_create_local_nic_from_nic_dev, "Fail to allocate local nic cache entry");
		goto out;
	}
	ln->nic_dev = nic_dev;
	INIT_LIST_HEAD(&ln->ports);
	if (lnic_alloc_n_add_ports(p, ln, &nic_dev->port_list) < 0)
		goto err;
	if (lnic_alloc_n_add_ports(p, ln, &nic_dev->unused_port_list) < 0)
		goto err;

	if (list_empty(&ln->ports)) {
		_NT(trace_1_main_create_local_nic_from_nic_dev, "No ports added, free ln");
		goto free_ln;
	}

	ln->cold_add = false;

	goto out;

err:
	list_for_each_entry_safe(lnp, tmp_lnp, &ln->ports, link) {
		list_del(&lnp->link);
		kfree(lnp);
	}

free_ln:
	kfree(ln);
	ln = NULL;

out:
	NFOUT;
	return ln;
}

/* Add nic to disk's local nic list.*/
void nvmeibc_add_nic_disk_local_nics(struct nvmeibc_disk *disk, struct nvmeibc_dev *nic_dev)
{
	struct nvmeibc_local_nic *ln;

	NFIN;

	if ((ln = create_local_nic_from_nic_dev(nic_dev))) {
		_NT(trace_main_nvmeibc_add_nic_disk_local_nics, "Added local nic @DEV_NAME with @N_PORTS ports to disk @DISK_NAME",
		   nvmeibc_device_name(nic_dev), ln->n_ports, disk->name);
		prio_list_add_tail(&ln->link, &disk->local_nics, local_nic_prio_cmp_fn, NULL);
		disk->num_lnics++;
	}
	NFOUT;
}

/* Populate disk's local nic list.
 * Not guarded, must be called in context of Main WQ */
int nvmeibc_populate_disk_local_nics(struct nvmeibc_disk *disk)
{
	const struct nvmeibc_cinst_params_core *p = nvmeibc_cinst_get_core_p(disk);
	struct t_core_clnt_globals *cg = __get_from_params_core_globals_container(p);
	struct nvmeibc_dev *nic_dev;
	int rv = 0;

	NFIN;
	nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_core2main(p));

	list_for_each_entry(nic_dev, &cg->devs_lists.all, dev_list_n) {
		nvmeibc_add_nic_disk_local_nics(disk, nic_dev);
	}
	list_for_each_entry(nic_dev, &cg->devs_lists.unused, dev_list_n) {
		nvmeibc_add_nic_disk_local_nics(disk, nic_dev);
	}

	if (!disk->num_lnics)
		rv = -ENODEV;

	NFOUT;
	return rv;
}

#pragma pop_macro("__FILE_LITERAL__")
