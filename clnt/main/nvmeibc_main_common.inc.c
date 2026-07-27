#include "module/instance/nvmeibc_cinst_params.h"

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__  nvmeibc_main_common_inc_c

void t_main_clnt_globals_params_free(struct nvmeibc_cinst_params_main *p);

/************************* Scheduling mechanism of main ***********************/
static int __start_main_wq(struct t_main_clnt_sched *s, const char *name, u16 cinst_index)
{
	int rv = 0;
	proc_name_t pname;

	snprintf(s->main_wq_name, sizeof(s->main_wq_name), "c_mwq_%s", name);
	if (strncmp(name, "mod", 8) == 0)
		snprintf(pname, sizeof(pname), proc_name_format("C", "WQ", "mwq_mod"));
	else
		clnt_proc_name_format(pname, 'C', "WQ", "mwq", cinst_index);

	if (!(s->main_wq = wq_create_verbose(pname))) {
		_NT(trace_01_main_wq, "Fail to allocate main work queue");
		rv = -1;
		goto out;
	}
	s->main_wq_pid = wq_pid(s->main_wq);
	atomic_set(&s->main_wq_submit_use_count, 1);
	_NT(trace_02_main_wq, "main work queue created name=@STR pid=@MAIN_WQ_PID", s->main_wq_name, s->main_wq_pid);
out:
	return rv;
}

void t_main_clnt_sched_disable(struct t_main_clnt_sched *s)
{
	if (s->main_wq) {
		wq_drain(s->main_wq);						// Let tasks finish with their sub tasks on main wq.
		init_completion(&s->main_wq_stop_use_completed);
		if (!atomic_dec_and_test(&s->main_wq_submit_use_count))
			wait_for_completion(&s->main_wq_stop_use_completed);
		wq_drain(s->main_wq);
		// Worqueue is completely idle. No works exist, none can start
	}
	_NT(trace_03_main_wq, "main work queue drained name=@STR pid=@MAIN_WQ_PID", s->main_wq_name, s->main_wq_pid);
}

int t_main_clnt_sched_create(struct t_main_clnt_sched *s, const char *name, u16 cinst_index)
{
	int rv;
	memset(s, 0, (sizeof(*s)));
	if ((rv = __start_main_wq(s, name, cinst_index)) < 0)
		t_main_clnt_sched_destroy(s);
	return rv;
}

void t_main_clnt_sched_destroy(struct t_main_clnt_sched *s)
{
	if (s->main_wq) {
		wq_destroy(s->main_wq);
		_NT(trace_04_main_wq, "main work queue destroyed name=@STR pid=@MAIN_WQ_PID", s->main_wq_name, s->main_wq_pid);
		s->main_wq_pid = 0;
		s->main_wq = NULL;
	}
}

int t_main_clnt_sched_add_work(struct t_main_clnt_sched *s, struct workqe_struct *work)
{
	int rv;
	if (atomic_inc_not_zero(&s->main_wq_submit_use_count)) {
		rv = wq_add_work(s->main_wq, work) ? 0 : -EINVAL;
		if (atomic_dec_and_test(&s->main_wq_submit_use_count))
			complete(&s->main_wq_stop_use_completed);
	} else {
		rv = -ENOSYS;  // during shutdown of instance it is ok for mainwq to be down
	}
	return rv;
}

bool t_main_clnt_sched_cancel_work(struct t_main_clnt_sched *s, struct workqe_struct *work)
{
	return wq_cancel_work(s->main_wq, work);
}

bool t_main_clnt_sched_on_main_wq(const struct t_main_clnt_sched *s, bool do_assert)
{
	const bool rv = on_wq_pid(s->main_wq_pid);
	WARN((do_assert)&&(!rv),"nvmebc bug: thread=\"%s\" pid=%d instead of wq=\"%s\"", current->comm, current->pid, s->main_wq_name);
	return rv;
}

/****************** Mechanics of preventing volume manipulation and instance creation ********************/
static void t_main_clnt_priv_sched_set(struct t_main_clnt_globals *mg, enum nvmeibc_inst_state st) {
	struct t_main_clnt_priv_sched *s = &mg->priv_sched;
	const enum nvmeibc_inst_state prev_state = s->state;
	int rv = 0;
	switch (st) {
	case NVMEIBC_INST_STATE_INITIALIZING: {
		mutex_init(&s->is_ready_for_attaches);
		s->state = st;
		mutex_lock(&s->is_ready_for_attaches);
		break;
	}
	case NVMEIBC_INST_STATE_READY: {
		rv = (!mutex_is_locked(&s->is_ready_for_attaches));	// Mutex should be held until everything is ready to prevent attaches
		rv |= (prev_state != (st-1));
		s->state = st;
		mutex_unlock(&s->is_ready_for_attaches);
		break;
	}
	case NVMEIBC_INST_STATE_PREP_RM: {
		const bool is_init_error = (prev_state != NVMEIBC_INST_STATE_READY);	//  Destrucion of partially initialized object (Creation failed).
		if (is_init_error) {
			rv = (!mutex_is_locked(&s->is_ready_for_attaches));				// mutex should be held until destroy to prevent attaches
		} else {
			mutex_lock(&s->is_ready_for_attaches);							// Wait for no running attach/detach requests
		}
		s->state = st;
		mutex_unlock(&s->is_ready_for_attaches);							// Now incomming attach detach requests will autofail
		break;
	}
	case NVMEIBC_INST_STATE_RM_RDY:
	case NVMEIBC_INST_STATE_EXITING: {
		rv = (prev_state != (st-1));
		s->state = st;
		break;
	}
	default:
		rv = -EINVAL;
	}
	WARN(rv, "nvmeibc bug, wrong flow, inst=%s, state=%d->%d, rv=%d", mg->proc_dir.root_name, prev_state, s->state, rv);
	_NT(t_01_main_priv_sched, "@STR: @STATE->@STATE", mg->proc_dir.root_name, prev_state, s->state);
}

static int t_main_clnt_priv_sched_bdev_manipulation_start(struct t_main_clnt_priv_sched *s) {
	int rv = 0;
	mutex_lock(&s->is_ready_for_attaches);
	if (s->state >= NVMEIBC_INST_STATE_PREP_RM) {
		_NT(t_50_cmain, "Isntance is in state=@STATE - bailing out", s->state);
		rv = -ENODEV;
	} else {
		WARN((s->state != NVMEIBC_INST_STATE_READY), "nvmeibc bug, wrong state=%d", s->state);	// While initializing, this function must be delayed (prevented to run)
	}
	return rv;
}

static void t_main_clnt_priv_sched_bdev_manipulation_end(struct t_main_clnt_priv_sched *s) {
	WARN(!mutex_is_locked(&s->is_ready_for_attaches), "nvmeibc bug, wrong flow");
	mutex_unlock(&s->is_ready_for_attaches);
}

/*********************** Internal Varsiables of main layer ********************/
#define get_main_cints(p) (*(p)->_private)
int t_main_clnt_globals_create(const struct nvmeibc_cinst_params_main *p)
{
	struct t_main_clnt_globals *mg = kzalloc(sizeof(*mg), GFP_KERNEL);
	if (!mg)
		return -ENOMEM;

	get_main_cints(p) = mg;
	mg->p = p;

	// --------------- init ->proc_dir
	mg->proc_dir.root_name = p->proc_dir_root_name;

	// --------------- init ->priv_sched
	t_main_clnt_priv_sched_set(mg, NVMEIBC_INST_STATE_INITIALIZING);
	t_main_clnt_sched_create(&mg->sched, p->proc_dir_root_name, nvmeibc_cinst_get_main_inst_num(p));

	// --------------- init ->com
	/* Backwards compatibility*/
	if (nvmeibc_cinst_is_first_main_instance(p)) {	// instace 0 uses hostname.           Example: n111.excelero.com
		strlcpy(mg->com.full_name, nvmeib_get_utsname_nodename(), sizeof(mg->com.full_name));
		// When required add module param here to store MANAGEMENT_SERVERS and MANAGEMENT_PROTOCOL given from nvmeshclient (nvmesh.conf)
		// mg->com.use_https = nvmeibc_clnt_to_mgmt_use_https;
		// mg->com.management_cluster = kstrdup(nvmeibc_clnt_to_mgmt_cluster, GFP_KERNEL);
	} else{				// instace 0 uses hostname.           Example: n111.excelero.com_mc9
		snprintf(mg->com.full_name, sizeof(mg->com.full_name), "%.*s_%.*s",
				 NVMEIB_HOST_NAME_LEN, nvmeib_get_utsname_nodename(),
				 CINST_NAME_LEN,       mg->proc_dir.root_name);
	}

	// --------------- init ->vols
	INIT_LIST_HEAD(&mg->vols.volumes);
	INIT_LIST_HEAD(&mg->vols.mtvolumes);
	INIT_LIST_HEAD(&mg->disks.list);

	if (nvmeibc_target_init_arnics_league(p) < 0)
		return -ENOMEM;

	return 0;
}

void t_main_clnt_globals_destroy(const struct nvmeibc_cinst_params_main *p)
{
	struct t_main_clnt_globals *mg = get_main_cints(p);
	if (mg) {
		nvmeibc_target_free_arnic_league(p);
		WARN_ON(!list_empty(&mg->vols.volumes));
		WARN_ON(!list_empty(&mg->vols.mtvolumes));
		WARN_ON(!list_empty(&mg->disks.list));
		kfree(mg);
		get_main_cints(p) = NULL;
	}

	// Allocated on init instance
	t_main_clnt_globals_params_free((struct nvmeibc_cinst_params_main *)p);
}

/************************* Free/Update Params IOctls **************************/
void t_main_clnt_globals_params_free(struct nvmeibc_cinst_params_main *p)
{
	if (p && p->mgmt.cluster) {
		kfree(p->mgmt.cluster);
		p->mgmt.cluster = NULL;
	}
}

typedef int (*t_update_main_params_fn)(struct nvmeibc_cinst_params_main *p, const char *ioctl);

struct t_param_main_update_ioctl {
	const char *prefix;
	int prefix_len;						// Length of the string above
	t_update_main_params_fn fn;
};

static int __update_mgmt_protocol(struct nvmeibc_cinst_params_main *p, const char *protocol)
{
	if ((strncmp("https", protocol, 5/*strlen("https")*/)) == 0) {
		p->mgmt.use_https = true;
	} else if ((strncmp("http", protocol, 4/*strlen("http")*/)) == 0) {
		p->mgmt.use_https = false;
	} else
		return -EINVAL;
	return 0;
}

static int __update_mgmt_autogen(struct nvmeibc_cinst_params_main *p, const char *str)
{
	p->mgmt.is_auto_generated = ((str[0] == '1')||(str[0] == 'T')||(str[0] == 't'));		// True/true/1
	return 0;
}

static int __update_mgmt_dbuuid(struct nvmeibc_cinst_params_main *p, const char *str)
{
	ulong flags;
	spin_lock_irqsave(&p->param_change_lock, flags);
	strlcpy(p->mgmt.db_uuid, str, sizeof(p->mgmt.db_uuid));
	spin_unlock_irqrestore(&p->param_change_lock, flags);
	return 0;
}

static int __update_mgmt_cluster(struct nvmeibc_cinst_params_main *p, const char *cluster)
{
	const char *new_cluster = kstrdup(cluster, GFP_KERNEL);
	ulong flags;
	spin_lock_irqsave(&p->param_change_lock, flags);		// Prevent other function to access the string
	kfree(p->mgmt.cluster);									// Can be NULL
	p->mgmt.cluster = new_cluster;
	spin_unlock_irqrestore(&p->param_change_lock, flags);
	return 0;
}

static int __update_cfg_version(struct nvmeibc_cinst_params_main *p, const char *cfg_version)
{
	return kstrtol(cfg_version, 10, (long *)(&p->cfg_profile.version));
}

static int __update_cfg_name(struct nvmeibc_cinst_params_main *p, const char *cfg_name)
{
	strlcpy(p->cfg_profile.name, cfg_name, sizeof(p->cfg_profile.name));
	return 0;
}

static int __update_cfg_id(struct nvmeibc_cinst_params_main *p, const char *cfg_id)
{
	strlcpy(p->cfg_profile.id, cfg_id, sizeof(p->cfg_profile.id));
	return 0;
}

static struct t_param_main_update_ioctl _param_main_update_tbl[] = {
	{"cluster="  , 8 , __update_mgmt_cluster},
	{"protocol=" , 9 , __update_mgmt_protocol},
	{"auto_gen=" , 9 , __update_mgmt_autogen},
	{"db_uuid="  , 8 , __update_mgmt_dbuuid},
	{"cfg_version=", 12, __update_cfg_version},
	{"cfg_name=", 9, __update_cfg_name},
	{"cfg_id=", 7, __update_cfg_id},
};

int t_main_clnt_globals_params_update(struct nvmeibc_cinst_params_main *p, const char *ioctl)
{
	int i, rv, n_funcs = ARRAY_SIZE(_param_main_update_tbl);
	for (i = 0; i < n_funcs; i++) {
		const struct t_param_main_update_ioctl *upd = &_param_main_update_tbl[i];
		if (strncmp(upd->prefix, ioctl, upd->prefix_len) == 0) {
			rv = upd->fn(p, &ioctl[upd->prefix_len]);
			goto _out;
		}
	}
	rv = -EINVAL;			// Unknown parameter update
_out:
	return rv;
}

struct t_main_clnt_globals * __get_from_params_main_globals_container(const struct nvmeibc_cinst_params_main *p)
{
	return ((struct t_main_clnt_globals *)get_main_cints(p));
}

#pragma pop_macro("__FILE_LITERAL__")
