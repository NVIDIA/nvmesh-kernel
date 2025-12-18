/* Implements nvmesh internal ioctls to block device layer */
#include "datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "nvmeib_event.h"
#include "nvmeibc_block_common.h"
#include "nvmeibc_block.h"
#include "recovery/nvmeibc_raid_recovery.h"
#include "./datapath_ec/nvmeibc_block_dp_ec_gf.h"
#include "block/controlpath/nvmeibc_b_cp_topo_common.h"
#include "datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeibc_memmgr_metrics.h"

#ifdef KR_UNDEF_H
#	undef KR_UNDEF_H
#endif
#include "kr_undef.h"

/* dev == NULL means run callback for all bdevs */
#define get_dev_name(dev) (dev ? dev->name : "all bdevs")

#define __auto_detect_base(txt) (((txt[0] == '0')&&(txt[1] == 'x')) ? 16 : 10)
u64 __read_ull_addr(const char** str)
{
	u64 rv = (~0ULL);								// Illegall out of volume address
	while (((**str) == ',')||((**str) == '=')||((**str) == ' '))		// Skip separators
		(*str)++;
	if ((*str)[0]) {
		const char *txt = (*str);
		const int base = __auto_detect_base(txt);
		rv = (u64)simple_strtoull(txt, (void*)str, base);	// Daniel: Why not use base '0'? consider replacing with kstrtoull()
		while ((**str) == ',')						// Skip separators
			(*str)++;
	}
	return rv;
}

/* Since we are on main-wq, no need to lock the list b->block_devices_sl, as bdevs are added/removed on main-wq only */
#define __do_for_each_bdev(p, dev, expression) ({ \
	struct t_block_clnt_globals *b = __get_from_params_blok_globals_container(p); \
	nvmeibc_assert_on_main_wq(nvmeibc_isnt_params_blk2main(p)); \
	list_for_each_entry(dev, &b->block_devices, list_n) { \
		expression; \
	}})

#define do_for_bdev(p, dev, expression) do {\
	if (dev) {                            expression;  } \
	else     { __do_for_each_bdev(p, dev, expression); } \
} while (0)

struct t_topo_pr {			// Structure used to operate on protection raid
	struct nvmeibc_topology *t;		// Acquired topology
	struct nvmeibc_raid1 *pr;		// Protection raid of valid topo
	u16 ci, ri;						// Indices of the raid
	u16 action;						// Action code to perform on pr
};

//if ci,ri & si are valid - increments reference and returns the topology
//else informs about the error (_I) and returns NULL
static struct nvmeibc_topology* __ioctl_get_topology(struct nvmeibc_block_device *dev, u32 ci, u32 ri, s32 si){
	struct nvmeibc_topology *t = nvmeibc_topology_get(&dev->topologies);
	struct nvmeibc_raid1 *r = NULL;
	if (((u32)__get_topo_num_chunks(t) <= ci) || ((u32)nvmeibc_chunk_get_num_raids(t, ci) <= ri)) {
		_NI_to_user(t_y0_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME illegal raid (@CI,@PRAID_IDX)", dev->name, ci, ri);
		goto _err;
	}
	r = __get_r1_by_t(t, ci, ri);
	if (0 <= si){
		if (r->replicas <= si) {
			_NI_to_user(t_y1_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME illegal raid (@CI,@PRAID_IDX, @SI)", dev->name, ci, ri, si);
			goto _err;
		}
	}
	return t;
_err:
	nvmeibc_topology_put(t);
	return NULL;
}

static void __ioctl_put_topology(struct nvmeibc_topology* t){
	if (t)
		nvmeibc_topology_put(t);
}

static bool __pr_for_action_get(struct nvmeibc_block_device *dev /* != NULL */,
			struct t_topo_pr *rv, const char *cmd, int len)
{
	u32 ci = 0, ri = 0;
	rv->t = nvmeibc_topology_get(&dev->topologies);
	if ((len < 3)||(!rv->t))
		goto _out;
	sscanf(cmd, "%u, %u", &ci, &ri);
	if (((u32)__get_topo_num_chunks(      rv->t    ) <= ci ) ||
		((u32)nvmeibc_chunk_get_num_raids(rv->t, ci) <= ri)) {
		goto _out;
	}
	rv->ci = ci;
	rv->ri = ri;
	rv->pr = __get_r1_by_t(rv->t, ci, ri);
_out:;
	_NT(trace_01_ioctl_pr_get, QA_BLOCK_PREFIX "@DEV_NAME, raid (@CI,@PRAID_IDX) Do @ACTION_INT, rv=@RV", dev->name, rv->ci, rv->ri, rv->action, (!rv->pr));
	return (rv->pr != NULL);
}

static void __pr_for_action_put(struct t_topo_pr *pra)
{
	nvmeibc_topology_put(pra->t);
}

/********************** IOctls callback functions *****************************/
static int __stop_mgmt_allerts(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	const char* name = get_dev_name(dev);
	u32 new_freq = U32_MAX;
	(void)cmd;
	_NT(trace_0_mgmt_allerts, "@DEV_NAME: no-io allert freq disabled (@NEW_FREQ)", name, new_freq);
	do_for_bdev(p, dev, nvmeibc_io_perm_alert_set_stucked_detach_alert_freq(&dev->dp.io_perm_alert, new_freq));
	return 0;
}

static int __set_mgmt_allerts_freq(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	const char* name = get_dev_name(dev);
	u32 new_freq = 0;
	int rv = 0, len = strlen(cmd);
	if (len > 1) {	/* Skip '=' or ' ' */
		sscanf(cmd + 1, "%u", &new_freq);
		if ((new_freq < 1)) {
			_NI_to_user(t_y2_dp_dbg_tools, QA_BLOCK_PREFIX, "minimum alert frequency is 1 second");
			rv = -EINVAL;
		}
		else {
			_NT(trace_1_mgmt_allerts, "@DEV_NAME:new io disabled freq @NEW_FREQ", name, new_freq);
			do_for_bdev(p, dev, nvmeibc_io_perm_alert_set_stucked_detach_alert_freq(&dev->dp.io_perm_alert, new_freq));
		}
	}
	return rv;
}

static int __clear_stats_io(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)cmd;
	do_for_bdev(p, dev, nvmeibc_block_clean_debug_counters(dev, true, true, true, false, false));
	return 0;
}

static int __clear_stats_worst_io(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)cmd;
	do_for_bdev(p, dev, nvmeibc_block_clean_debug_counters(dev, false, true, false, false, false));
	return 0;
}

static int __clear_stats_all(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)cmd;
	do_for_bdev(p, dev, nvmeibc_block_clean_debug_counters(dev, true, true, true, true, true));
	return 0;
}

static int __clear_profilers(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)cmd;
	do_for_bdev(p, dev, nvmeibc_block_clean_debug_counters(dev, false, false, false, false, true));
	return 0;
}

static int __clear_memmgr_metrics(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)cmd;
	(void)dev;
	(void)p;
	nvmesh_memmgr_metrics_clear(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	return 0;
}

static int __enforce_readonly(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	const char* name = get_dev_name(dev);
	int rv = 0, len = strlen(cmd);
	if (len > 1) {	/* Skip '=' or ' ' */
		const bool do_enforce = (cmd[1] != '0');
		do_for_bdev(p, dev, dev->os->atom.conf.enforce_readonly = do_enforce);
	} else {
		_NI_to_user(t_y3_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME missing arg 0/1 for @CMD_STR!", name, cmd);
		rv =  -EINVAL;
	}
	return rv;
}

static int __max_retry_secs(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = 0, len = strlen(cmd);
	if (len > 1) {	/* Skip '=' or ' ' */
		unsigned long new_retry;
		sscanf(cmd + 1, "%ld", &new_retry);
		if ((new_retry < 1) || (new_retry > (1 << 20))) {
			_NT(trace_api_nvmeshioctl_max_retry_secs, QA_BLOCK_PREFIX "max_retry_sec @NEW_RETRY out of range", new_retry);
			#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
				rv = 0;
			#else
				rv =  -EINVAL;
			#endif
		}
		if (rv == 0) {
			new_retry *= HZ;	// Convert to jiffies
			do_for_bdev(p, dev, { dev->max_retry_jiffies = new_retry; nvmeibc_io_resubmitter_wakeup(&dev->dp.resub); });
		}
	}
	return rv;
}

static int __atom_upg_fail_io(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int n_bios = -1, rv = -EPERM, len = strlen(cmd);
	if (dev) {
		_NI_to_user(t_y5_dp_dbg_tools, QA_BLOCK_PREFIX, "cmd must be executed on specific bdev via pointer!");
	} else if (len > 16) {
		u64 ptr = 0;
		sscanf(cmd + 1, "%d", &n_bios);		// cmd = "=-1, ptr=0xffff9ba59c7be600
		cmd = strstr(cmd+3, "ptr=0x");
		if (cmd) {
			sscanf(cmd, "ptr=0x%llx", &ptr);
			if (ptr) {
				rv = block_api_os_autofail_upgrade_io((struct nvmeibc_os_api *)ptr, n_bios);
				rv = (rv == n_bios) ? 0 /*good*/ : -ENOEXEC;
			}
		}
	}
	(void)p;
	return rv;
}

static int __atom_read_part_toggle(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = -EPERM, len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	if (dev) {
		_NI_to_user(t_y6_dp_dbg_tools, QA_BLOCK_PREFIX, "cmd must be executed on all block devices!");
	} else {
		nvmeibc_os_api_layer_toggle_read_part(p, do_enable);
		rv = 0;
	}
	return rv;
}

static int __atom_prepare_for_upg(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	_NI_to_user(t_y7_dp_dbg_tools, QA_BLOCK_PREFIX, "Stop all self inflicted bio!");
	(void)cmd; (void)dev; dev = NULL;
	// Here: Do loop for each client instance and stop all self issuing bio's, otherwise client will not be able to rmmod
	nvmeibc_os_api_layer_toggle_read_part(p, false);
	return 0;
}

static int __osapi_revalidate(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	_NI_to_user(t_y7a_dp_dbg_tools, QA_BLOCK_PREFIX, "Launched with cmd=@STR", cmd);
	do_for_bdev(p, dev, block_api_os_async_revalidate(dev));
	return 0;
}

static int __dump_tcntrs(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	bool do_unsafe __attribute__((unused)) = false;
	if ((len > 1)&&(cmd[1] == 'U')) {	/* Skip '=' or ' ' or '_' */
		do_unsafe = true;
	}
	if (dev) {
		_NT(trace_api_nvmeshioctl_dump_tcntrs, QA_BLOCK_PREFIX "ignorring dev @DEV_NAME, doing to all devs", dev->name);
	}
	__debug_topo_print_uncompleted_op(p, do_unsafe);
	return 0;
}

static int __dump_uncompleted(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)dev; (void)cmd; (void)p;
	__uncompleted_cmds_list_dump();
	return 0;
}

extern void nvmeibc_pd_dump_transfers(struct nvmeibc_disk *disk); //#include "nvmeibc_pausable.h"
static int __dump_transfers(const struct nvmeibc_cinst_params_blk *unused_p, struct nvmeibc_block_device *dev, const char *cmd)
{

	struct t_topo_pr p = {NULL, NULL, 0, 0, 0};
	int rv = -EINVAL, len = strlen(cmd);

	(void)unused_p;
	#ifdef DEBUG_TRANSFERS
		if (!dev) {
			_NI_to_user(t_y8_dp_dbg_tools, QA_BLOCK_PREFIX, "cmd must be executed on specific bdev!");
			goto _out;
		}
	#else
		_NI_to_user(t_y9_dp_dbg_tools, QA_BLOCK_PREFIX, "Unsupported command @CMD_STR! Recompile", cmd);
		if (dev)
			rv = 0;		// command is valid but not unsupported, Else cmd invalid
		goto _out;
	#endif
	if (!strncmp(cmd, "_raid", 5)) {
		int i;
		p.action = 'r'; /* Print transfers of all disks of raid */
		len -= 6;  /* Skip '=' or ' ' */
		cmd += 6;
		if (!__pr_for_action_get(dev, &p, cmd, len))
			goto _out;
		for (i = 0; i < p.pr->replicas; i++) {
			nvmeibc_pd_dump_transfers(p.pr->segments[i].disk);
		}
	} else if (!strncmp(cmd, " ", 1)) { /* Deprecated unsafe v1.2.1 ioctl */
		u64 diskp = 0;
		p.action = 'd'; /* Print of single disk, deprecated v1.2.1 ioctls */
		sscanf(cmd + 1, "%llx", &diskp); /* Skip ' ' */
		nvmeibc_pd_dump_transfers((struct nvmeibc_disk *)diskp);
	} else {
		_NI_to_user(t_ya_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME unknown cmd @CMD_STR", dev->name, cmd);
		goto _out;
	}
	rv = 0;
_out:
	__pr_for_action_put(&p);
	return rv;
}

static int __toggle_edic(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	do_for_bdev(p, dev, dev->dp.enable_edic_check = do_enable);
	return 0;
}

static int __get_read_has_mutable_bio_buffers_from_cmd(const char *cmd)
{
	const int len = strlen(cmd);
	if (len < 2){
		return 0;
	}
	if (cmd[1] == '0' || cmd[1] == 'n' || cmd[1] == 'N'){
		return 0;
	}

	if (cmd[1] == '1' || cmd[1] == 'y' || cmd[1] == 'Y'){
		return 1;
	}

	if (cmd[1] == '2'){
		return 2;
	}
	return 0;
}

static int __toggle_read_has_mutable_bio_buffers(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	const int read_has_mutable_bio_buffers = __get_read_has_mutable_bio_buffers_from_cmd(cmd);
	do_for_bdev(p, dev, dev->dp.read_has_mutable_bio_buffers = read_has_mutable_bio_buffers);
	return 0;
}

static int __toggle_lclread(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	do_for_bdev(p, dev, dev->dp.enable_local_read_optimization = do_enable);
	return 0;
}

static int __toggle_ext_car_io(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	do_for_bdev(p, dev, dev->allow_external_io_on_carrier = do_enable);
	return 0;
}

static int __show_struct_size(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	(void)p; (void)dev; (void)cmd;
	#define _SZ(S) ((int)sizeof(struct S))
	_NI_to_user(t_yb_dp_dbg_tools, QA_BLOCK_PREFIX, "{u32=@INT, u64=@INT, page=@X}, cmd={bcmd=@INT, dcmd=@INT, dcmd_comp=@INT, ndb=@INT}, lock=@INT{rdma_cmp=@INT} o=@INT{bpart=@INT}, t=@INT {percpu=@INT}, os_api=@INT{atom=@INT}",
		(int)sizeof(int), (int)sizeof(u64), (u32)PAGE_SIZE,
		_SZ(nvmeibc_block_command), _SZ(nvmeibc_disk_io_command), _SZ(nvmeibc_d_iocmd_comp), _SZ(nvmeib_data_buffer),
		_SZ(nvmeibc_cmd_lock), _SZ(nvmeibc_d_rdma_comp),
		_SZ(operation), _SZ(bio_part),
		_SZ(nvmeibc_topology), _SZ(nvmeibc_topo_percpu), _SZ(nvmeibc_os_api), _SZ(nvmeiba_atom_os_api));

	return 0;
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
static int __toggle_di_debug_mode(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	do_for_bdev(p, dev, dev->dp.enable_di_debug_mode = do_enable);
	return 0;
}
#endif

#define t_vlba_trans_init(tv, _dev, _addr, _nlbas) ({\
	(tv)->output.n_cmds = (tv)->output.n_locks = 0;	\
	(tv)->output.d_carriers.n_nds = 0;	\
	(tv)->output.md_carrier.nd = NULL;	\
	(tv)->input.nd = _dev;		\
	(tv)->input.vlba = _addr;	\
	(tv)->input.nlbas = _nlbas;	\
	(tv)->input.translate_locks = false;	\
	(tv)->input.op = NVMEIB_BLOCK_IO_OP_WRITE;  })

#define __host_of(disk) \
	((disk)->disk_host[0] == '?' ? "Unknown" : (disk)->disk_host)

//TODO: this function can be used to serve information about "prepare" stage.
static void __trans_vlba_of_bdev(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, u64 addr, u32 nlbas, char op, u64 cookie, const int recusive_depth)
{
	struct dp_block_translation_unit *tv = kzalloc(sizeof(*tv), GFP_ATOMIC);
	struct t_dp_block_trans_output *res = &tv->output;
	char tabs[32];
	int i, rv;
	(void)p;
	if (!tv) {
		_NW_to_user(t_yc_dp_dbg_tools, DMESG_PREFIX("@DEV_NAME"), "No memory for address translation cookie=@COOKIE", dev->name, cookie);
		goto _out;
	}
	memset(tabs, ' ', sizeof(tabs));
	tabs[recusive_depth*4] = '\0';			// Depth in range of [0..2] so no out of buffer here
	t_vlba_trans_init(tv, dev, addr, nlbas);
	if ((op == 'r')||(op == 'R'))
		tv->input.op = NVMEIB_BLOCK_IO_OP_READ;	// Else 'wWlL' = write
	if ((op == 'l')||(op == 'L'))
		tv->input.translate_locks = true;

	tv->input.translate_by_cfg = ((op == 'c')||(op == 'C'));

	if (!dev->dp.dbg_trans_addr) {
		_NT(t_a1_ncioctl, "@STR @DEV_NAME: Address translation not supported cookie=@COOKIE", tabs, dev->name, cookie);
		goto _out;
	}
	rv = dev->dp.dbg_trans_addr(tv);
	if (rv || (res->n_cmds == 0 && res->d_carriers.n_nds == 0)) {
		_NT(t_a2_ncioctl, "@STR @DEV_NAME: Address translation failed! [@VLBA..@VLBA] rv=@RV cookie=@COOKIE", tabs, dev->name, addr, (addr+(u64)(nlbas-1)), rv, cookie);
		goto _out;
	} else if (nlbas == 1) {
		_NI_to_user(t_yd_dp_dbg_tools, QA_BLOCK_PREFIX, "@STR @DEV_NAME: @OP_CHR Translating address @VLBA to @NCMDS dlbas cookie=@COOKIE", tabs, dev->name, op, addr, res->n_cmds, cookie);
	} else {
		_NI_to_user(t_ye_dp_dbg_tools, QA_BLOCK_PREFIX, "@STR @DEV_NAME: @OP_CHR Translating range [@VLBA..@VLBA] to @NCMDS dlbas cookie=@COOKIE", tabs, dev->name,  op, addr, (addr+(u64)(nlbas-1)), res->n_cmds, cookie);
	}
	for (i = 0; i < res->n_cmds; i++)
		_NI_to_user(t_yf_dp_dbg_tools, QA_BLOCK_PREFIX, "@STR @INDEX) @DEV_NAME:@VLBA ==> @DESCR:@DISK_HOST:@DISK_NAME:@DLBA cookie=@COOKIE",  tabs, i, dev->name, addr, res->descr[i], __host_of(res->disks[i]), res->disks[i]->name, res->offs[i], cookie);
	for (i = 0; i < res->n_locks; i++)
		_NI_to_user(t_yg_dp_dbg_tools, QA_BLOCK_PREFIX, "@STR @INDEX) @DEV_NAME:@VLBA ==> Lock:@DESCR:@DISK_HOST:@DISK_NAME cookie=@COOKIE",   tabs, i, dev->name, addr, res->ldescr[i], __host_of(res->ldisks[i]), res->ldisks[i]->name, cookie);
	for (i = 0; i < res->d_carriers.n_nds; i++) {
		_NI_to_user(t_yh_dp_dbg_tools, QA_BLOCK_PREFIX, "@STR @INDEX) @DEV_NAME:@VLBA ==> @DESCR:@DEV_NAME:@VLBA cookie=@COOKIE",              tabs, i, dev->name, addr, res->d_carriers.descr[i], res->d_carriers.nds[i]->name, res->d_carriers.lbas[i], cookie);
		if (res->d_carriers.lbas[i] != (~0ULL))
			__trans_vlba_of_bdev(p, res->d_carriers.nds[i], res->d_carriers.lbas[i], nlbas, op, cookie, recusive_depth+1);	// Recursive call
	}
	if (res->md_carrier.nd) {
		_NI_to_user(t_yi_dp_dbg_tools, QA_BLOCK_PREFIX, "@STR @INDEX) @DEV_NAME:@VLBA ==> @DESCR:@DEV_NAME:@VLBA cookie=@COOKIE",         tabs, i, dev->name, addr, "MDV", res->md_carrier.nd->name, res->md_carrier.lba, cookie);
		__trans_vlba_of_bdev(p, res->md_carrier.nd, res->md_carrier.lba, nlbas, op, cookie, recusive_depth+1);				// Recursive call
	}
	if (res->mssa_output) { // Prints all MSSA MAPS
		char *line = res->mssa_output;
		_NI_to_user(t_z0_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME: MSSA OUTPUT MAPS cookie=@COOKIE", dev->name, cookie);
		while (line) { // Mutiple lines are printed
			char *next_line = strchr(line, '\n');
			if (next_line) { // Terminate line (replace \n with \0), or line is already terminated and last
				*next_line = '\0';
			}
			_NI_to_user(t_z1_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME: @STR cookie=@COOKIE", dev->name, line, cookie);
			if (next_line) { // Skip the \0 char
				line = next_line + 1;
			} else {		 // Done
				line = NULL;
			}
		}
	}
_out:
	if (res->mssa_output) {
		kfree(res->mssa_output);
	}
	kfree(tv);
}
static int __translate_vlba(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	u64 addr = __read_ull_addr(&cmd), cookie = 0;
	u32 nlbas = 1;								// Default: a single block
	char op = 'W';								// Default: translate write
	sscanf(cmd, "%u,%c,%llu", &nlbas, &op, &cookie);
	if (nlbas == 0)
		nlbas = 1;								// Definitely no one meant to translate empty IO. Minimal length is 1 block
	do_for_bdev(p, dev, __trans_vlba_of_bdev(p, dev, addr, nlbas, op, cookie, 0));
	return 0;
}

#define __d2_v_lba_fmt "sgmnt=(%u,%u,%u) dlba=%llu"
#define __d2_v_lba_help "Translate DLBA->[V/C/R]LBA segment(chunk, raid, sgmnt)"
#define __goto_out_if(condition) if (unlikely(condition)) { rv = -1*(__LINE__); goto _out; }
static int __translate_dlba(struct nvmeibc_block_device *dev, const char *cmd, char dst)
{
	u64 dlba = 0, cookie = 0, lba = ~0ULL;
	struct t_topo_pr p = {NULL, NULL, 0, 0, 0};
	u32 ci, ri, si = 0;
	int rv = -EINVAL, scan_rv;
	if (!dev) {
		_NI_to_user(t_yj_dp_dbg_tools, QA_BLOCK_PREFIX, "cmd must be executed on specific bdev!");
		goto _out;
	}
	while (*cmd == ' ') cmd++;
	__goto_out_if(4 != (scan_rv = sscanf(cmd, __d2_v_lba_fmt, &ci, &ri, &si, &dlba)));
	if ((p.t = __ioctl_get_topology(dev, ci, ri, si)) != NULL) {
		p.pr = __get_r1_by_t(p.t, ci, ri);
		if (!dev->dp.dbg_trans_dlba_to_clba) {
			_NT(t_21_ncioctl, "@DEV_NAME: Address translation not supported cookie=@COOKIE", dev->name, cookie);
			goto _out;
		}
		if (dst == 'v')
			lba = nvmeibc_datapath_dlba_to_vlba( &dev->dp, p.pr, si, dlba);
		else if (dst == 'c')
			lba = dev->dp.dbg_trans_dlba_to_clba(&dev->dp, p.pr, si, dlba);
		else if (dst == 'r')
			lba = nvmeibc_datapath_dlba_to_rlba( &dev->dp, p.pr, si, dlba);
		_NI_to_user(t_yk_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME:" SEGMENT_FMT ", Translating address @DLBA(@LONG) -> @LLX(@LONG)", dev->name, ci, ri, si, dlba, (long)dlba, lba, (long)lba);
		rv = 0;
	}
_out:
	__ioctl_put_topology(p.t);
	if (rv)
		_NE_to_user(t_yl_dp_dbg_tools, QA_BLOCK_PREFIX, "Format: @STR", __d2_v_lba_fmt);
	return rv;
}

static int __translate_d_2_vlba_deprecated(__attribute__ ((unused)) const struct nvmeibc_cinst_params_blk *unused_p, struct nvmeibc_block_device *dev, const char *cmd)
{
	return __translate_dlba(dev, cmd, 'v');
}

static int __translate_dlba_new(__attribute__ ((unused)) const struct nvmeibc_cinst_params_blk *unused_p, struct nvmeibc_block_device *dev, const char *cmd)
{
	char dst = cmd[0];
	switch (cmd[0]) {
	case 'v': case 'V': dst = 'v'; break;
	case 'c': case 'C': dst = 'c'; break;
	case 'r': case 'R': dst = 'r'; break;
	default:
		_NI_to_user(t_ym_dp_dbg_tools, QA_BLOCK_PREFIX, "Unkown translation destination |@CMD_STR|, see help", cmd);
		return -EINVAL;
	}
	return __translate_dlba(dev, &cmd[1], dst);
}

static int __vol_elev_flush(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, __attribute__ ((unused)) const char *cmd)
{
	extern void __mini_elevator_force_flush_all_ioctl(struct nvmeibc_block_device *dev);
	do_for_bdev(p, dev, __mini_elevator_force_flush_all_ioctl(dev));
	return 0;
}

static int __translate_txbm(__attribute__ ((unused)) const struct nvmeibc_cinst_params_blk *unused_p, __attribute__ ((unused)) struct nvmeibc_block_device *dev, const char *cmd)
{
	u32 txbm_in = __read_ull_addr(&cmd), txbm_out;
	const char op = cmd[0];
	if ((op == 'z')||(op == 'Z')||(op == 'c')||(op == 'C'))	{			// zip / compress
		txbm_out = nvmeibc_block_dp_ec_md_txbm_compress(txbm_in);		// Cutoff to 16 bits
		_NI_to_user(t_yo_dp_dbg_tools, QA_BLOCK_PREFIX, "TxBMcompress: @TXBM -> @TXBM_COMPRESSED", txbm_in, txbm_out);
	} else if ((op == 'd')||(op == 'D')||(op == 'u')||(op == 'U')) {
		txbm_out = nvmeibc_block_dp_ec_md_txbm_decompress(txbm_in);		// unzip / decompress,  Cutoff to 8 bits
		_NI_to_user(t_yp_dp_dbg_tools, QA_BLOCK_PREFIX, "TxBMdecomprs: @TXBM_COMPRESSED -> @TXBM", txbm_in, txbm_out);
	} else if (op == (char)0) {
		_NI_to_user(t_yq_dp_dbg_tools, QA_BLOCK_PREFIX, "Missing translation direction, see help");
	} else {
		_NI_to_user(t_yr_dp_dbg_tools, QA_BLOCK_PREFIX, "Unkown translation direction |@CMD_STR|, see help", cmd);
	}
	return 0;
}

static int __translate_qlc2mdv(__attribute__ ((unused)) const struct nvmeibc_cinst_params_blk *unused_p, __attribute__ ((unused)) struct nvmeibc_block_device *dev, const char *cmd)
{
	// TODO: Implement with QLC volume argument, support per-chunk round-robin translation
#if 0
	u64 QLC_vlba_blckset = __read_ull_addr(&cmd), mdv_vlba;
	const char op = cmd[0];
	if ((op == 'b')||(op == 'V'))	{			// VLBA[blocks]
	} else /*if ((op == 'l')||(op == 'B'))*/ {     	// VLBA[blocksets]
	}
	mdv_vlba = nvmeibc_convert_vlba_blkset_to_mdv_vlba(QLC_vlba_blckset, 1);
	_NI_to_user(t_ys_dp_dbg_tools, QA_BLOCK_PREFIX, "QLC[blockset] to MDV vlba @VLBA -> @VLBA", QLC_vlba_blckset, mdv_vlba);
#else
	(void)cmd;
#endif
	return 0;
}

static void __send_di_alert_on_bdev(struct nvmeibc_block_device *dev, u64 addr)
{
	#define ALRT_LEN (192)
	static const char *fmt = "E%s Volume %s DI BUG@ address %llu";
	const char *clnt_name = nvmeib_get_utsname_nodename();
	char *msg;

	NFIN;
	if ((msg = kmalloc(ALRT_LEN, GFP_ATOMIC)) == NULL)
		goto _out;
	snprintf(msg, ALRT_LEN, fmt, /*Hdr:*/ clnt_name, dev->name, /*Body:*/ addr);
	nvmeibc_block_send_mgmt_allert(dev, msg);//, 0, false);
_out:
	NFOUT;
}

static int __stop_bdev(struct nvmeibc_block_device *dev)
{
	int rv = nvmeibc_block_suspend(dev, NULL, NULL); // Daniel: use callbacks
	dev->max_retry_jiffies =  HZ / 10;       /* Autofail all IO */
	nvmeibc_io_resubmitter_wakeup(&dev->dp.resub);
	return rv;
}

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-larger-than"
#endif
static void __report_di_bug_on_addr_on_bdev(struct nvmeibc_block_device *dev, u64 addr)
{
	struct dp_block_translation_unit tv;
	struct t_dp_block_trans_output *res = &tv.output;
	int rv = 0;
	_NE_to_user(t_yt_dp_dbg_tools, DMESG_PREFIX("@DEV_NAME"), "vlba=@VLBA - DATA CORRUPTION. Stopping IO", dev->name, addr);
	if (addr == (~0ULL)) {
		rv |= __stop_bdev(dev);
		rv |= nvmeibc_topologies_inform_di_bug_in_raid(&dev->topologies, NULL, 0, 0);	// Default, first raid
	} else {
		t_vlba_trans_init(&tv, dev, addr, 1);
		tv.input.translate_by_cfg = true;
		if (dev->dp.dbg_trans_addr && (dev->dp.dbg_trans_addr(&tv) == 0)) {
			int i;
			rv |= __stop_bdev(dev);
			if (res->n_cmds > 0) {
				rv |= nvmeibc_topologies_inform_di_bug_in_raid(&dev->topologies, res->offs, res->ci, res->ri);
			}

			// Report recursively on MD volumes and carriers, if any
			if (res->md_carrier.nd)
				__report_di_bug_on_addr_on_bdev(res->md_carrier.nd, res->md_carrier.lba);

			for (i = 0; i < (int)res->d_carriers.n_nds; i++)
				__report_di_bug_on_addr_on_bdev(res->d_carriers.nds[i], res->d_carriers.lbas[i]);

		} else { /*Address is wrong or translation not supported*/ }

		__send_di_alert_on_bdev(dev, addr);
	}
	(void)rv;
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif

static int __report_di_bug_on_addr(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	u64 addr = __read_ull_addr(&cmd);
	do_for_bdev(p, dev, __report_di_bug_on_addr_on_bdev(dev, addr));
	return 0;
}

static int __manage_suspend(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = 0, len = strlen(cmd);
	const bool do_suspend = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	if (do_suspend) { // Daniel: use callbacks
		do_for_bdev(p, dev, rv |= nvmeibc_block_suspend(dev, NULL, NULL));
	} else {										// Revive
		nvmeibcb_dp_io_fail_mgr_reset_limit(NULL);	// Todo: Currently support action on all block devices
		do_for_bdev(p, dev, rv |= nvmeibc_block_revive( dev));
	}
	return rv;
}

static int __manage_iofail(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = 0, len = strlen(cmd);
	(void)p; (void)dev;
	if (len <= 1) {
		_NT(t_mif_00, QA_BLOCK_PREFIX "wrong arguments");
		rv =  -EINVAL;
	} else if ((cmd[1] == 'D')||(cmd[1] == 'd')) {
		nvmeibcb_dp_io_fail_mgr_disable(NULL); // Todo: Currently support action on all block devices
	} else if ((cmd[1] == 'R')||(cmd[1] == 'r')) {
		nvmeibcb_dp_io_fail_mgr_reset_limit(NULL); // Todo: Currently support action on all block devices
	} else {
		int new_io_limit = 0;
		sscanf(cmd + 1, "%d", &new_io_limit);
		nvmeibcb_dp_io_fail_mgr_set_limit(NULL, new_io_limit); // Todo: Currently support action on all block devices
	}
	return rv;
}

static int __ignore_toma_msg(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	do_for_bdev(p, dev, dev->ignore_all_toma_msgs = do_enable);
	return 0;
}

static int __ignore_toma_recovs(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int len = strlen(cmd);
	const bool do_enable = ((len > 1)&&(cmd[1] == '1')); /* Skip '=' or ' ' */
	do_for_bdev(p, dev, dev->ignore_all_recov_requests = do_enable);
	return 0;
}

static int __os_ptr(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	const char* name = get_dev_name(dev);
	int rv = 0, len = strlen(cmd);
	if (len != 2) {
		_NI_to_user(t_yu_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME missing arg ++/--!", name);
		rv = -EINVAL;
	} else if (!dev) {
		_NI_to_user(t_yv_dp_dbg_tools, QA_BLOCK_PREFIX, "Must be executed for specific volume!"); // We take spinlock to iterate over all devs and it cannot be called with interrupt disabled
		rv =  -EINVAL;
	} else if (cmd[0] == '+') {
		do_for_bdev(p, dev, block_api_os_get(dev->os, QA_BLOCK_PREFIX));
	} else if (cmd[0] == '-') {
		do_for_bdev(p, dev, block_api_os_put(dev->os));
	} else {
		WARN(true, QA_BLOCK_PREFIX "%s, wrong command %s, attempting to crush the system?", name, cmd);
	}
	return rv;
}

static void __dup_many_topos_concurrently(void* _dev)
{
	struct nvmeibc_block_device *dev = _dev;
	int i;
	for (i = 0; i < 64; i++) {
		nvmeibc_topologies_duplicate(&dev->topologies, NULL);
	}
}

static int __topo_action(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = 0, action = cmd[0];
	if (action == 'd') {		// dup
		do_for_bdev(p, dev, nvmeibc_topologies_duplicate(&dev->topologies, NULL));
	} else if (action == 'D') {	// Multi-Dup on all cpu's. Check race conditions of topology phased out mechanism
		do_for_bdev(p, dev, on_each_cpu(__dup_many_topos_concurrently, dev, true));
	} else if (action == 'f') {	// Flush
		do_for_bdev(p, dev, nvmeibc_warm_apply_conf_diffs(&dev->topologies));
	} else if (action == 'm') {	// Manual-recovery
		do_for_bdev(p, dev, nvmeibc_topology_manual_attempt_recover_list(&dev->topologies));
	} else if (action == 'c') {	// Check
		  int rv1 = 0, rv2 = 0;
		  if (dev)
			  _NI_to_user(t_yw_dp_dbg_tools, QA_BLOCK_PREFIX, "specific bdev is ignorred, testing all of then!");
		  rv1 = nvmeibc_trs_detect_config_corruption(p);
		  do_for_bdev(p, dev,
				rv2 += nvmeibc_topologies_detect_illegal_raid_conf(&dev->topologies));
		  _NI_to_user(t_yx_dp_dbg_tools, QA_BLOCK_PREFIX, "Config corruption=@CHAR, Praid Illegal nodes=@CHAR", (rv1?'Y':'N'), (rv2?'Y':'N'));
		  // Todo: Add here other topo checks ... Note: Ioctl succeeded regardless of the config corruption
	} else {
		_NI_to_user(t_yy_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME unknown cmd @CMD_STR", dev->name, cmd);
		rv = -EINVAL;
	}
	return rv;
}

#define __raid_rcvrs_handle_msg_frmt " sgmnt=(%u,%u,%u) type=%d task=%d msg=%u"
#define __raid_rcvrs_handle_msg_help "sends msg to a single or all recoveries running under (chunk, raid, sgmnt). To send message to all segments recoveries, set type=-1 & task=-1"
static int  __raid_rcvrs_handle_msg(__attribute__((__unused__)) const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = -EINVAL;
	u32 ci = 0, ri = 0, si = 0, msg = 0;
	s32 type = -1, task_id = -1;
	struct nvmeibc_topology *t = NULL;

	__goto_out_if(6 != sscanf(cmd, __raid_rcvrs_handle_msg_frmt, &ci, &ri, &si, &type, &task_id, &msg))
	if ((t = __ioctl_get_topology(dev, ci, ri, si))){
		struct nvmeibc_raid1 *r = __get_r1_by_t(t, ci, ri);
		struct nvmeibc_subscription_ctx *tr = r->segments[si].toma_reg;
		//small cheap forward compatible convinience: user may specify the concrete msg or just offset
		enum NVMEIBT_CLIENT_MSG_TYPES msg_type = msg < NVMEIBT_CLIENT_MSG_RT_RECOVER_ANNOUNCE ? NVMEIBT_CLIENT_MSG_RT_RECOVER_ANNOUNCE + msg : msg;
		if (type == -1 && task_id == -1){
			nvmeibc_recoveries_handle_ioctl_request(tr, msg_type, NULL);
		} else {
			struct nvmeibt_client_recovery_taskid_pl mission = {.task = {.id = task_id, .type = type, .max_batch_size = NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE, .effort_percents = NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE }};
			nvmeibc_recoveries_handle_ioctl_request(tr, msg_type, &mission);
		}
		rv = 0;
	}
_out:
	__ioctl_put_topology(t);
	if (rv)
		_NE_to_user(t_yz_dp_dbg_tools, QA_BLOCK_PREFIX, "Format: @STR", __raid_rcvrs_handle_msg_frmt);
	return rv;
}

#define __raid_rcvrs_start_frmt " sgmnt=(%u,%u,%u) type=%u is_mandatory=%u do_only_owners=%u blocksets=[%llu, %lld)"
#define __raid_rcvrs_start_help "starts recovery (type) on segment(chunk, raid, sgmnt); blocksets end range could be set to -1 - whole segment;"
static int __raid_rcvrs_start(__attribute__((__unused__)) const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = -EINVAL, scan_rv;
	u32 ci = 0, ri = 0, si = 0, type = 0, is_mandatory = 0, do_only_owners = 0;
	u64 bs_start = -1;
	s64 bs_end = -1;

	struct nvmeibc_topology *t = NULL;
	_NT(trace_api_nvmeshioctl_raid_rcvrs_start, "@FUNCTION called dev=@DEV_NAME cmd=@STR", __FUNCTION__, dev->name, cmd);
	__goto_out_if(8 != (scan_rv = sscanf(cmd, __raid_rcvrs_start_frmt, &ci, &ri, &si, &type, &is_mandatory, &do_only_owners, &bs_start, &bs_end)));
	if ((t = __ioctl_get_topology(dev, ci, ri, si))) {
		struct nvmeibc_raid1 *r = __get_r1_by_t(t, ci, ri);
		struct nvmeibc_subscription_ctx *tr = r->segments[si].toma_reg;
		struct nvmeibt_client_recovery_start_pl  __attribute__((aligned(8))) rpl = {
			.task = {.id = (jiffies * 100) + 187, .type = type, .max_batch_size = NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE, .effort_percents = NVMEIBT_CLIENT_PROTOCOL_EFFORT_PERCENTS_DONT_CARE}
			, .is_mandatory = is_mandatory
			, .do_only_owners = do_only_owners
			, .praid_id = {0}
			, .start_lock = bs_start
			, .num_locks = ((bs_end < 0) ? (tr->length / LOCKSET_SLICES) : (bs_end - bs_start))	//the validation left in purpose to the impl function
			, .reserved = {0},
		};
		const u32 uuid_size = min(sizeof(rpl.praid_id), sizeof(r->segments[si].uuid));
		memcpy(&rpl.praid_id, &r->segments[si].uuid, uuid_size);
		rv = nvmeibc_recovery_start_ioctl(tr, r, &rpl);
	}
_out:
	__ioctl_put_topology(t);
	if (rv == -EINVAL)
		_NE_to_user(t_yA_dp_dbg_tools, QA_BLOCK_PREFIX, "Format: @STR", __raid_rcvrs_start_frmt);
	return rv;
}

static int __raid_rcvrs_set_n_sw(__attribute__((__unused__)) const struct nvmeibc_cinst_params_blk *pp, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = -EINVAL, len = strlen(cmd), act_len = 3, n_sw_to_use = -1;
	struct t_topo_pr p = {NULL, NULL, 0, 0, .action = 's'};
	__goto_out_if((!dev)||(len < 6));
	sscanf(cmd, "=%d:", &n_sw_to_use);				// 1 or 2 digit number
	__goto_out_if((n_sw_to_use < 0)||((n_sw_to_use > 99)));
	if (n_sw_to_use > 9)
		act_len++;
	len -= act_len;  /* Skip '=N:' */
	cmd += act_len;
	__goto_out_if(!__pr_for_action_get(dev, &p, cmd, len));
	switch (p.action) {
		case 's': { int i;
			dev->ignore_all_recov_toma_speed_req = (n_sw_to_use != 0);	// When manually setting non default value, prevent toma from override it
			for (i = 0; i < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES; i++)
				nvmeibc_recovery_set_num_sw(p.pr->hdr->recoveries[i], n_sw_to_use);
		}
	}
	rv = 0;
_out:
	if (rv)
		_NI_to_user(t_yB_dp_dbg_tools, QA_BLOCK_PREFIX, "nvmeibc error: @BDEV, n_sw=@RV!", dev, n_sw_to_use);
	__pr_for_action_put(&p);
	return rv;
}

static int __raid_rcvrs_set_n_mb(__attribute__((__unused__)) const struct nvmeibc_cinst_params_blk *pp, struct nvmeibc_block_device *dev, const char *cmd)
{
	int rv = -EINVAL, len = strlen(cmd), b_size = -1;
	struct t_topo_pr p = {NULL, NULL, 0, 0, .action = 's'};
	__goto_out_if((!dev)||(len < 6));
	sscanf(cmd, "=%u:", &b_size);
	__goto_out_if(b_size < 0);
	while ((*cmd != ':') && (*cmd != 0)) { // Skip '=N'
		cmd++; len--;
	}
	cmd++; len--; // Skip ':'
	__goto_out_if(!__pr_for_action_get(dev, &p, cmd, len));
	switch (p.action) {
		case 's': { int i;
			dev->ignore_all_recov_toma_speed_req = (b_size != 0);	// When manually setting non default value, prevent toma from override it
			for (i = 0; i < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES; i++)
				nvmeibc_recovery_set_max_batch_size(p.pr->hdr->recoveries[i], b_size);
		}
	}
	rv = 0;
_out:
	if (rv)
		_NI_to_user(t_yL_dp_dbg_tools, QA_BLOCK_PREFIX, "nvmeibc error: @BDEV, max_batch_size=@RV!", dev, b_size);
	__pr_for_action_put(&p);
	return rv;
}

static int __stale_lock_resolver(const struct nvmeibc_cinst_params_blk *unused_p, struct nvmeibc_block_device *dev, const char *cmd)
{
	struct t_topo_pr p = {NULL, NULL, 0, 0, 0};
	struct stale_lock_resolver_t *slr;
	int rv = -EINVAL, len = strlen(cmd), act_len = 5;
	(void)unused_p;
	if (!dev) {
		_NI_to_user(t_yC_dp_dbg_tools, QA_BLOCK_PREFIX, "cmd must be executed on specific bdev!");
		goto _out;
	}
	if        (!strncmp(cmd, "print", act_len)) {
		p.action = 'p'; /* Print */
	} else if (!strncmp(cmd, "clear", act_len)) {
		p.action = 'c'; /* clear */
	} else {
		_NI_to_user(t_yD_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME unknown cmd @CMD_STR", dev->name, cmd);
		goto _out;
	}
	len -= (act_len+1);  /* Skip '=' or ' ' */
	cmd += (act_len+1);
	__goto_out_if(!__pr_for_action_get(dev, &p, cmd, len));
	slr = &p.pr->hdr->slr;
	switch (p.action) {
		case 'c': stale_lock_resolver_clear_all(slr); /*break;*/
		FALLTHRU;
		case 'p': stale_lock_resolver_to_log(slr); break;
	}
	rv = 0;
_out:
	__pr_for_action_put(&p);
	return rv;
}

#define __sub_vol_fmt_scanf "name=%63s start=%llu len=%llu"
#define __sub_vol_fmt "{add/del} " __sub_vol_fmt_scanf
#define __sub_vol_help ""

static int __sub_vol_do(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	u64 lba = 0, nlba = 0;
	char name[64], uuid[32], action;
	int rv = -EINVAL, len = strlen(cmd), n_args = 0, act_len = 3;

	if (!dev) {
		_NI_to_user(t_yE_dp_dbg_tools, QA_BLOCK_PREFIX, "cmd must be executed on specific bdev!");
		goto _out;
	}
	(void)p;
	memset(name, 0, sizeof(name));
	memset(uuid, 0, sizeof(uuid));			// Todo: Sub vols get empty ("") uuid. Not the best practice....
	action = cmd[0];
	if (len <= act_len+1) {
		_NI_to_user(t_yF_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME cmd @CMD_STR without args", dev->name, cmd);
		goto _out;
	} else if (!strncmp(cmd, "add", act_len)) {
	} else if (!strncmp(cmd, "del", act_len)) {
	} else {
		_NI_to_user(t_yG_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME unknown cmd @CMD_STR", dev->name, cmd);
		goto _out;
	}
	len -= (act_len+1);  /* Skip '=' or ' ' */
	cmd += (act_len+1);
	n_args = sscanf(cmd, __sub_vol_fmt_scanf, name, &lba, &nlba);

	switch (action) {
		case 'a': {
			if (n_args < 1) {		/* Mising Name, ... */
				goto _out;
			} else if (n_args == 1) {
				rv = block_api_os_sub_vol_name(dev->os, name, uuid);
			} else if (n_args < 3) {	/* Missing Name, start, len, ... */
				goto _out;
			} else {
				rv = block_api_os_sub_vol_attach(dev->os, lba, nlba, name, uuid);
			}
			break;
		}
		case 'd': {
			if (n_args < 1) {		/* Delete all subvols is not supported yet ... */
				goto _out;
			} else {
				rv = block_api_os_sub_vol_detach(dev->os, name);
			}
			break;
		}
		default:;
	}
_out:
	return rv;
}

static int __change_gf_func(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int index = nvmeibc_gf_optimization_from_string(&cmd[1]), rv = 0;
	(void)p;	// gf functions are unified for all instances
	if (dev)
		_NI_to_user(t_yH_dp_dbg_tools, QA_BLOCK_PREFIX, "specific bdev is ignorred, changing the GF for all of them!");
	if (index < 0) {
		_NT(trace_1_api_nvmeshioctl_change_gf_func, QA_BLOCK_PREFIX "Unknown gf_functions request: @CMD_STR", cmd);
		return -EINVAL;
	}
	rv = __gf_choose_functions(index);
	return ((rv == index) ? 0 : -EINVAL);
}

static int __help(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd);

/********************** Main function *****************************/
/* Prototype of callback of each ioctl */
typedef int (*_ioctl_func)(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char*cmd);

struct t_ioctl {
	const char *name;
	short       len;
	_ioctl_func func;
	const char *help_args;
	const char *help_hint;
};

static struct t_ioctl ioctls[] = {
	{"mgmt_alert_stop" , 15, &__stop_mgmt_allerts    , "", "Mgmt allerts freq=0"},
	{"mgmt_alert_freq" , 15, &__set_mgmt_allerts_freq, "=<N>", "in [secs]"},
	{"clear_io_stats"  , 14, &__clear_stats_io       , "", ""},
	{"clear_worst_cntr", 16, &__clear_stats_worst_io , "", ""},
	{"clear_all_cntrs" , 15, &__clear_stats_all      , "", ""},
	{"clear_profilers" , 15, &__clear_profilers      , "", "Clear profiler counters"},
	{"clear_memmgr_metrics" , 20, &__clear_memmgr_metrics , "", "Clear memmgr metrics"},
	{"enforce_readonly", 16, &__enforce_readonly     , "=<1 or 0>", ""},
	{"max_retry_secs"  , 14, &__max_retry_secs       , "=<N>", "in [secs]"},
	{"dump_tcntrs"     , 11, &__dump_tcntrs          , "=Unsafe or =Safe", ""},
	{"dump_uncompleted", 16, &__dump_uncompleted     , "", "Uncompleted cmds"},
	{"dump_transfers"  , 14, &__dump_transfers       , "=<disk ptr addr>", " or _raid=N"},
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	{"di_debug_mode"   , 13, &__toggle_di_debug_mode , "=<1 or 0>", "Enable debug di mode, Never use in production!"},
#endif
	{"set_read_edic"   , 13, &__toggle_edic          , "=<1 or 0>", "Enable Edic Calc on Write, Check on Read"},
	{"read_has_mutable_bio_buffers", 28, &__toggle_read_has_mutable_bio_buffers, "=<2 or 1 or 0>", "Use private buffers for read (2 always, 1 during degraded topo, 0 don't use"},
	{"set_lclread_opt" , 15, &__toggle_lclread       , "=<1 or 0>", "Enable local read optimization on R1"},
	{"set_car_ext_io"  , 14, &__toggle_ext_car_io    , "=<1 or 0>", "Allow external kernel io to carrier volumes"},
	{"show_struct_size", 16, &__show_struct_size     , "", "Show sizes of structs in datapath"},
	{"translate_addr"  , 14, &__translate_vlba       , "=<addr>,<?len>,<?R/W/C/L>,<?cookie?", "VLBA->DLBA, len=1, W, cookie=0"},
	{"translate_dlba2" , 15, &__translate_dlba_new   , __d2_v_lba_fmt, __d2_v_lba_help},
	{"translate_dlba"  , 14, &__translate_d_2_vlba_deprecated, __d2_v_lba_fmt, __d2_v_lba_help},		// Deprecated: 2.0.3 and before. Needed for automation / CI / logs collector
	{"translate_txbm"  , 14, &__translate_txbm       , "=<TxBM><z/u>", "zip/unzip TxBM. Example:0x2z"},
	{"translate_qlcmdv", 16, &__translate_qlc2mdv    , "=<QLC vlba_blcksets>", "Translates QLC blockset to MDV VLBA"},
	{"di_bug_on_addr"  , 14, &__report_di_bug_on_addr, "=<addr>", "Allert DI"},
	{"volume_suspend"  , 14, &__manage_suspend       , "=<1 or 0>", ""},
	{"volume_elevflush", 16, &__vol_elev_flush       , "", "unsafe action, use with care, when plug is deactivated"},
	{"volume_io_fail"  , 14, &__manage_iofail        , "=<Number or X/D>", "# of io attempts before suspention. R - Reset to default, D to disable auto suspention"},
	{"ignore_toma_msg" , 15, &__ignore_toma_msg      , "=<1 or 0>", "debug: toggle ignore all msgs from toma"},
	{"ignore_toma_rcv" , 15, &__ignore_toma_recovs   , "=<1 or 0>", "debug: toggle ignore all recovery requests from toma"},
	{"os_ptr"          ,  6, &__os_ptr               , "++/--", "Blk_get/put"},
	{SUB_VOL_CMD       ,  8, &__sub_vol_do           , __sub_vol_fmt, __sub_vol_help},
	{"topo_"           ,  5, &__topo_action          , "{dup/Dup/manual/flush/check}", "flush=restart clnt-toma protocol, dup=is_stuck?, Dup=multi-dup, manual=???, check=if config/topo for bugs"},
	{"STLR_"           ,  5, &__stale_lock_resolver  , "{print/clear}=<ci,ri>", ""},
	{"recov_set_num_sw", 16, &__raid_rcvrs_set_n_sw  , "=N:<ci,ri>", "Set number of sync workers"},
	{"recov_set_nbatch", 16, &__raid_rcvrs_set_n_mb  , "=N:<ci,ri>", "Set max size of batch, units=locks"},
	{"recov_launch"    , 12, &__raid_rcvrs_start     , __raid_rcvrs_start_frmt, __raid_rcvrs_start_help},
	{"recov_handle"    , 12, &__raid_rcvrs_handle_msg, __raid_rcvrs_handle_msg_frmt, __raid_rcvrs_handle_msg_help},
	{"change_gf_func"  , 14, &__change_gf_func       , "=<avx2|u64|sse2|unoptimized>", "Toggles between GF functions"},
	{"atom_upg_fail_io", 16, &__atom_upg_fail_io     , "=N, ptr=0xffff9ba59c7be600", "Autofail N io's of upgrading atom by its pointer, -1 for all"},
	{"atom_read_part"  , 14, &__atom_read_part_toggle, "=<1 or 0>", "Enable re-read partition of volumes"},
	{"atom_b4upgrade"  , 14, &__atom_prepare_for_upg , "", "Must be issued prior to hot upgrade"},
	{"atom_revalidate" , 15, &__osapi_revalidate     , "", "Revalidate and reread partition"},
	{"help"            ,  4, &__help                 , "", "print help"}
};

static int __help(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char *cmd)
{
	int i, n_ioctls = ARRAY_SIZE(ioctls);
	for (i = 0; i < n_ioctls; i++) {
		_NI_dmesg(t_br_dp_dbg_tools, "@IOCTL_IDX), @IOCTL_NAME@IOCTL_HELP_ARGS/* @IOCTL_HELP_HINT */", i, ioctls[i].name, ioctls[i].help_args, ioctls[i].help_hint);
	}
	(void)dev; (void)cmd; (void)p;
	return 0;
}

int nvmeibc_block_qa_config(const struct nvmeibc_cinst_params_blk *p, struct nvmeibc_block_device *dev, const char* cmd)
{
	const char* name = get_dev_name(dev);
	int rv = -EINVAL, i, n_ioctls = ARRAY_SIZE(ioctls);

	_NW(trace_api_nvmeshioctl_nvmeibc_block_qa_config, QA_BLOCK_PREFIX "@DEV_NAME Will @STR", name, cmd);
	for (i = 0; i < n_ioctls; i++) {
		if (!strncmp(cmd, ioctls[i].name, ioctls[i].len)) {
			rv = ioctls[i].func(p, dev, cmd + ioctls[i].len);
			_NI_to_user(t_yK_dp_dbg_tools, "@EVENT_TAG" QA_BLOCK_PREFIX, "@DEV_NAME @STR, rv=@RV", EV_IOCTL(),
				    name, cmd, rv);
			goto _out;
		}
	}
	_NI_to_user(t_yI_dp_dbg_tools, QA_BLOCK_PREFIX, "@DEV_NAME Unrecognized cmd: @STR! use '|help'", name, cmd);
_out:
	return rv;
}
