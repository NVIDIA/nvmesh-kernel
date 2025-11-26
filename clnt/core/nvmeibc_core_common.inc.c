#include "module/instance/nvmeibc_cinst_params.h"
#include "core/nvmeibc_core_ibdev.inc.c"			// Todo replace by .h

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__  nvmeibc_core_common_inc_c

struct t_core_clnt_globals * __get_from_params_core_globals_container(
				const struct nvmeibc_cinst_params_core *p)
{
	return ((struct t_core_clnt_globals *)(*(p)->_private));
}

void t_core_clnt_globals_params_free(struct nvmeibc_cinst_params_core *p);

static void __unload_keeper(void)
{
	unsigned long start_unload_jif, end_unload_jif;

	start_unload_jif = jiffies;
	nvmeib_public_unload_keeper();
	end_unload_jif = jiffies;

	_NI(clnt_keeper_unload, "Keeper unload took @JIFFIES jif", end_unload_jif - start_unload_jif);
}

int t_core_clnt_globals_create(const struct nvmeibc_cinst_params_core *p, const char *clnt_inst_name)
{
	int rv = -ENOMEM, cpu;
	extern void* nvmeibc_jam_init(const struct nvmeibc_cinst_params_core *p);
	struct t_core_clnt_globals *cg = kzalloc(sizeof(*cg), GFP_KERNEL);
	_NI(t_00_coreg, "@NDU client globals create (core) - start", 0);
	if (!cg)
		goto _out;
	(*p->_private) = cg;
	cg->clnt_inst_name = clnt_inst_name;
	t_core_clnt_globals_init_ibdev(p);
	if (!(cg->c_jam = nvmeibc_jam_init(p)))
		goto _out;
	if (!(cg->dg = nvmeibc_disk_create_globals(p)))
		goto _out;

	if (nvmeib_set_used_dev_list(
		p->filter_ports, p->max_p_len, &cg->devs_lists.used) < 0 ||
		nvmeib_set_used_pots_guids(
		p->filter_guids, p->max_g_len, &cg->devs_lists.used) < 0) {
		_NE_to_user(t_01_coreg, DMESG_MOD_PREFIX, "There is a problem with the networking devices list, which may be a result of an incorrect definition in the conf file or an internal bug, try to restart the services after adjusting in the conf file. Error code: 1039.");
		goto _out;
	}
	if (!(cg->wd_commands = nvmeib_wd_create(NVMEIBC_IO_TIMEOUT, 0 ,0))) {
		_NE(t_02_coreg, "Failed to init watch dog");
		goto _out;
	}
	if (!(cg->intr_shaper = nvmeib_intr_shaper_create(p->shaper_fs, p->shaper_burst, p->shaper_max_pct_cpu))) {
		_NE(t_03_coreg, "Failed to allocate interrupts shaper");
		goto _out;
	}
	if (!(cg->pcpu_wds = kcalloc(nr_cpu_ids, sizeof(*cg->pcpu_wds), GFP_KERNEL))) {
		_NE(t_05_coreg, "Failed to allocate per-cpu wds");
		goto _out;
	}
	for_each_possible_cpu(cpu) {
		if (!(cg->pcpu_wds[cpu] = nvmeib_wd_create_on_cpu(NVMEIBC_IO_TIMEOUT, 0, 0, cpu))) {
			_NE(t_06_coreg, "Failed to allocate per-cpu wds");
			goto _out;
		}
	}

	nvmeibc_cg_ib_sa_register_client(cg);  // Defer ib_register_client() on main-wq
	if (nvmeibc_run_on_main_wq(nvmeibc_isnt_params_core2main(p), clnt_start_ib_work_fn, (void*)p, true, true, NULL)) {
		_NE(t_04_coreg, "Failed to register to an RDMA device");
		goto _out;
	}

	/* Finished with keeper module. Try and unload it */
	__unload_keeper();

	nvmeibc_core_set_local_server_notification(p, true);
	rv = 0;
_out:
	_NI(t_07_coreg, "@NDU client globals create (core) - done", 0);
	return rv;
}

#define get_core_cints(p) (*(p)->_private)
void t_core_clnt_globals_destroy(const struct nvmeibc_cinst_params_core *p)
{
	extern void nvmeibc_jam_exit(const struct nvmeibc_cinst_params_core *p);
	struct t_core_clnt_globals *cg = get_core_cints(p);
	int cpu;
	_NI(trace_1_c_core_destroy, "@NDU client globals destroy (core) - start", 0);
	if (cg) {
		remove_ib(p);
		/* [NVMESH-4452]: Check for NULL cg->pcpu_wds before deref */
		if (cg->pcpu_wds) {
			for_each_possible_cpu(cpu) {
				/* nvmeib_wd_remove() checks for NULL ptr internally */
				nvmeib_wd_remove(cg->pcpu_wds[cpu]);
			}
			kfree(cg->pcpu_wds);
		}
		/* nvmeib_intr_shaper_destroy() checks for NULL ptr internally */
		nvmeib_intr_shaper_destroy(cg->intr_shaper); cg->intr_shaper = NULL;
		/* nvmeib_wd_remove() checks for NULL ptr internally */
		nvmeib_wd_remove(cg->wd_commands);           cg->wd_commands = NULL;
		nvmeib_free_used_dev_list(&cg->devs_lists.used);
		/* nvmeibc_jam_exit() checks for NULL ptr internally */
		nvmeibc_jam_exit(p);
		cg->c_jam = NULL;
		nvmeibc_disk_delete_globals(p);
		cg->dg = NULL;
		cg->clnt_inst_name = NULL;
		kfree(cg);
		get_core_cints(p) = NULL;
	}
	t_core_clnt_globals_params_free((struct nvmeibc_cinst_params_core *)p);
	_NI(trace_2_c_core_destroy, "@NDU client globals destroy (core) - end", 0);
}

/************************* Free/Update Params IOctls **************************/
static void __free_ports_filter(struct nvmeibc_cinst_params_core *p)
{
	if (p && p->filter_ports) {
		if (p->filter_ports != nvmeibc_filter_ports) {
			kfree(p->filter_ports);
		}
		p->filter_ports = NULL;
	}
}

static void __free_guids_filter(struct nvmeibc_cinst_params_core *p)
{
	if (p && p->filter_guids) {
		if (p->filter_guids != nvmeibc_filter_guids) {
			kfree(p->filter_guids);
		}
		p->filter_guids = NULL;
	}
}

void t_core_clnt_globals_params_free(struct nvmeibc_cinst_params_core *p)
{
	__free_ports_filter(p);
	__free_guids_filter(p);
}

typedef int (*t_update_params_fn)(struct nvmeibc_cinst_params_core *p, const char *ioctl);

static int __update_ports_filter(struct nvmeibc_cinst_params_core *p, const char *ioctl)
{
	const char *new_filter_ports = kstrdup(ioctl, GFP_KERNEL);
	_NT(t0__update_ports_filter,
		"update ports: @PORTS(@PTR) -> @PORTS(@PTR) (ioctl=@PORTS(@PTR))",
		p->filter_ports ? p->filter_ports : "null", p->filter_ports,
		new_filter_ports ? new_filter_ports : "null", new_filter_ports,
		ioctl ? ioctl : "null", ioctl);

	__free_ports_filter(p);
	p->filter_ports = new_filter_ports;
	return p->filter_ports ? 0 : -ENOMEM;
}

static int __update_guids_filter(struct nvmeibc_cinst_params_core *p, const char *ioctl)
{
	const char *new_filter_guids = kstrdup(ioctl, GFP_KERNEL);

	_NT(t0__update_guids_filter,
		"update guids: @GUIDS(@PTR) -> @GUIDS(@PTR) (ioctl=@GUIDS(@PTR))",
		p->filter_guids ? p->filter_guids : "null", p->filter_guids,
		new_filter_guids ? new_filter_guids : "null", new_filter_guids,
		ioctl ? ioctl : "null", ioctl);

	__free_guids_filter(p);
	p->filter_guids = new_filter_guids;
	return p->filter_guids ? 0 : -ENOMEM;
}

static int __update_use_rdda(struct nvmeibc_cinst_params_core *p, const char *ioctl)
{
	int rv = 0;

	if (ioctl[0] == 'y')
		p->use_rdda = true;
	else if (ioctl[0] == 'n')
		p->use_rdda = false;
	else
		rv = -EINVAL;
	return rv;
}

static int __update_tcp_mode(struct nvmeibc_cinst_params_core *p, const char *ioctl)
{
	/* assumes we get 1 char only that represent a number */
	p->tcp_mode = (unsigned int)(ioctl[0] - '0');
	_NT(_update_tcp_mode, "TCP mode was set to @UINT", p->tcp_mode);
	return 0;
}

struct t_param_update_ioctl {
	const char *prefix;
	int prefix_len;
	t_update_params_fn fn;
};

static struct t_param_update_ioctl _param_update_tbl[] = {
	{"ports=" , 6 /*strlen("ports=")*/, __update_ports_filter},
	{"guids=" , 6 /*strlen("guids=")*/, __update_guids_filter},
	{"use_rdda=", 9 /*strlen("use_rdda=")*/, __update_use_rdda},
	{"tcp_mode=", 9 /*strlen("tcp_mode=")*/, __update_tcp_mode}
};

int t_core_clnt_globals_params_update(struct nvmeibc_cinst_params_core *p, const char *ioctl)
{
	int i, rv, n_funcs = ARRAY_SIZE(_param_update_tbl);
	for (i = 0; i < n_funcs; i++) {
		const struct t_param_update_ioctl *upd = &_param_update_tbl[i];
		if (strncmp(upd->prefix, ioctl, upd->prefix_len) == 0) {
			rv = upd->fn(p, &ioctl[upd->prefix_len]);
			goto _out;
		}
	}
	rv = -EINVAL;			// Unknown parameter update
_out:
	return rv;
}

void nvmeibc_cinst_prep_upgrade_shutdown(const struct nvmeibc_cinst_params_core *p)
{
	if (nvmeibc_cinst_is_first_core_instance(p)) {
		/* Load the keeper to hold the FRs during restart */
		nvmeib_public_load_keeper();
	}
}

#pragma pop_macro("__FILE_LITERAL__")
