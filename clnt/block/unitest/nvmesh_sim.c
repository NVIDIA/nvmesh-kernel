// For documentation, see Header in H file
/*****************************************************************************/
#include "nvmesh_sim.h"
#include "server/nvmeibs_main_sim.h"
#include "../nvmeibc_block_common.h"
#include "../datapath_utils_generic/nvmeibc_block_dp_dbg_tools.h"
#include "server/nvmeibs_serjio_sim_access.h"
#include "module/instance/nvmeibc_cinst_params.h"
#include "nvmeib_jdr.h"
#include "nvmeibs_memmgr_metrics.h"

#define MAX_GEN_CMD_BOUND_BUFFER 4096*128
// 16 bytes is size of single free list entry, and minumum size required for system to operate.
// Can be enabled to check corner case freening jentries one by one, but is very slow.
// #define MAX_GEN_CMD_BOUND_BUFFER 16

void NVMeshSystem__printUncompletedIOs(struct NVMeshSystem *sys){
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int i, rv = 0, n_ios = 0;
	n_ios = osSimulator_allert_pending_ios(&client->OS, 0);			// Let the IO enough time to finish
	if (n_ios) {
		if (n_ios < 10) {											// Print operations that havent finished. Don't print too much to not clog the log
			bool printk_status = printk_is_enabled();
			printk_enable(true);									// Other threads may print to loog too here. Thats a shame :-(
			__debug_topo_print_uncompleted_op(&client->p->blok, true);
			printk_enable(printk_status);
		} else {
			unitest_print("num ops=%d, Not printing to avoid log clog\n",  n_ios);
		}
		unitest_print("****- Printing taken locks\n");
		for (i=0; i<sys->nServers; i++)
			rv += ramDiskSimulator_printTakenLocks(&sys->servers[i].ramDisk);
	}
}

/*************************** Entire NVMEsh system ****************************/
void NVMeshSystem_wipe_all_disks(struct NVMeshSystem *sys){
	int i;
	for (i=0; i<sys->nServers; i++)
		ramDiskSimulator_wipe(&sys->servers[i].ramDisk, 0xFF);
}

// Different from below function, sets all MD on drives as if cleared by writing zeros
// With 0's in MD (also counts as never written to), allows injecting values into MD
void NVMeshSystem_wipe_all_md_of_disks(struct NVMeshSystem *sys) {
	int i, j;
	for (i=0; i<sys->nServers; i++) {
		struct serverSimulator *S = &sys->servers[i];
		ramDiskSimulator_wipeMD(     &S->ramDisk, nvmeib_disk_init_md_max);
		ramDiskSimulator_wipeMD_jour(&S->ramDisk, nvmeib_jmd_unused_entry_md_max());
		//ramDiskSimulator_wipeMD_serjioDB(&S->ramDisk, zeroed_md);
		//nvmeibs_simu_print_jmdc(S);
		if (S->jmdc) {
			for (j = 0; j < NVMEIB_EC_TOTAL_JOURNAL_BLKS; j++) {
				S->jmdc[j] = nvmeib_jmd_unused_entry_val; // When wiping JMDD wipe JMDC as well
			}
		}
	}
}


// Different from above function, sets all MD on drives with initialized value (all 1's)
void NVMeshSystem_precondition_all_disks_for_EC(struct NVMeshSystem *sys) {
	int i;
	for (i=0; i<sys->nServers; i++) {
		struct ramDiskSimulator* disk = &sys->servers[i].ramDisk;
		ramDiskSimulator_format_metadata(disk, true);
		ramDiskSimulator_wipe(      disk, 0x0);
		ramDiskSimulator_reset_txid(disk);
		ramDiskSimulator_wipeMD(     disk, nvmeib_disk_init_md_max);
		ramDiskSimulator_wipeMD_jour(disk, nvmeib_jmd_unused_entry_md_max());
		//ramDiskSimulator_wipeMD_serjioDB(disk, zeroed_md);
		ramDiskSimulator_mark_ec(disk, true);
	}
}

void NVMeshSystem_precondition_all_disks_for_R1(struct NVMeshSystem *sys) {
	int i;
	for (i=0; i<sys->nServers; i++) {
		struct ramDiskSimulator* disk = &sys->servers[i].ramDisk;
		ramDiskSimulator_format_metadata(disk, true);
		ramDiskSimulator_mark_ec(disk, false);
	}
	NVMeshSystem_wipe_all_md_of_disks(sys);
}

void NVMeshSystem_volume_memset(struct NVMeshSystem *sys, int v, u8 val) {
	int	si;
	struct volumeDescriptor	*vol = &sys->mdb.vols[v];
	for (si = 0; si < vol->nSegments; si++) {
		struct disk_range *seg = vol->segs + si;
		struct ramDiskSimulator *ramDisk = &sys->servers[seg->node_id].ramDisk;
		ramDiskSimulator_wipeRange(ramDisk, __from4K(seg->dlba_start), __from4K(seg->length), val);
	}
}

void NVMeshSystem_volume_wipe_MD(struct NVMeshSystem *sys, int v) {
	int	si;
	struct volumeDescriptor	*vol = &sys->mdb.vols[v];

	for (si = 0; si < vol->nSegments; si++) {
		struct disk_range *seg = vol->segs + si;
		struct ramDiskSimulator *ramDisk = &sys->servers[seg->node_id].ramDisk;
		ramDiskSimulator_wipeMDRange(ramDisk, __from4K(seg->dlba_start), __from4K(seg->length), nvmeib_disk_init_md_max);
	}
}

void NVMeshSystem_set_all_txid_to_1(struct NVMeshSystem *sys) {
	int i, j;
	for (i=0; i<sys->nServers; i++) {
		for (j=0; j<RAMDISK_DATA_LOCK_SIZE; ++j) {
			sys->servers[i].ramDisk.TxIDs[j] = 1;
		}
	}
}

void NVMeshSystem_wipe_all_dirty_bits(struct NVMeshSystem *sys){
	int i;
	_ND(trace_nvmesh_sim_NVMeshSystem_wipe_all_dirty_bits, "Wiping dirty bits on all disks");
	for (i=0; i<sys->nServers; i++)
		ramDiskSimulator_wipe_dirty_bits(&sys->servers[i].ramDisk, 0x0);
}

int NVMeshSystem_wipe_all_stale_locks(struct NVMeshSystem *sys) {
	int i, c = 0;
	for (i=0; i<sys->nServers; i++)
		c += ramDiskSimulator_CleanSta(&sys->servers[i].ramDisk);
	return c;
}

int NVMeshSystem_wipe_all_read_only_locks(struct NVMeshSystem *sys) {
	int i, c = 0;
	for (i=0; i<sys->nServers; i++)
		c += ramDiskSimulator_CleanROL(&sys->servers[i].ramDisk);
	return c;
}

void NVMeshSystem_verify_no_dirty_bits(struct NVMeshSystem *sys){
	int i;
	_ND(trace_nvmesh_sim_NVMeshSystem_verify_no_dirty_bits, "Verify no dirty bits on all disks");
	for (i=0; i<sys->nServers; i++)
		ramDiskSimulator_verify_no_dirty_bits(&sys->servers[i].ramDisk);
}

struct nvmeibc_topology * ___get_tail_topo_of_device(struct NVMeshSystem *sys, int v){
	return list_last_entry(&sys->clients[0].devs[v]->topologies.topologies, struct nvmeibc_topology, list_n);
}

void NVMeshSystem_detectStuckIOs(struct NVMeshSystem *sys, bool long_wait) {
	const int loop_term = long_wait ? 200 : 100;							// In valgrind, wait more, coz everything is slower
	extern void nvmeibc_pd_dump_transfers(struct nvmeibc_disk *disk);
	struct clientSimulator *client = &sys->clients[0];					// Test via the first client
	int n_prev_ios = 0x7FFFFFFF, n_ios, wait_msec, v;
	clientSimulator_wait_for_all_io_resubmittion(client, -1);			// Wait for IO resubmit threads to finish
	n_ios = osSimulator_allert_pending_ios(&client->OS, 0);				// Test right now
	// start with 0 wait time so we exit loop ASAP with single IO test cases.
	for (wait_msec = 0; n_ios && (wait_msec <= loop_term); wait_msec += 1) {	// If IO stuck don't wait too long: 10 + 11 + ...45[msec] ~ 1[sec]. Otherwise (IO progressing) extend this period
		//nvmeibc_pd_dump_transfers(&sys->clients[0].physDiscs[0]);				// test dump (when enabled) while waiting
		n_prev_ios = n_ios;
		msleep(wait_msec);
		n_ios = osSimulator_allert_pending_ios(&client->OS, 0);
		if (wait_msec>=19){
			unitest_print("****- stuck IO's=%6d, prev=%6d, kernel_timers=%6d wait=%3d\n", n_ios, n_prev_ios, get_kernel_num_active_timers(), wait_msec); // Print IOs amount to see if it decreases
			if (n_ios<n_prev_ios)
				wait_msec--;										// Taking a long time but IO is being processed. Give it more time.
		}
	}
	if (!n_ios)
		return;
	for (v=0; v<client->nBdevs; v++) {
		if (!nvmeibc_topo_is_io_ok(&client->devs[v]->topologies)) {
			struct nvmeibc_topology *t = ___get_tail_topo_of_device(sys, v);
			const int offset_l = (u64)(&((struct nvmeibc_cmd_lock*)NULL)->dbg_topo.list);
			const int offset_o = (u64)(&((struct operation       *)NULL)->dbg_topo.list);
			struct nvmeibc_cmd_lock* l = (struct nvmeibc_cmd_lock*)(((char*)(t->dbg_tcntrs.next))-offset_l);
			struct operation       * o = (struct operation       *)(((char*)(t->dbg_tcntrs.next))-offset_o);
			_NE_dmesg(error_nvmesh_sim_NVMeshSystem_detectStuckIOs, "@TOPOLOGY @LOCKSETS,@OPERATION", t, l, o);
			if (t->newer != NULL)
				_Emerg("BUG: Topology stuck with IO's on it!\n");
			else if (t->percpu->t_users==0)
				_Emerg("BUG: Probably: Toma thinks seg is registered while Block thinks it is not!\n");
			else
				_Emerg("BUG: What the hell????\n");
			BUG_ON(true);                                               // This topology got stuck and prevents releasing topologies tail
		}

	}
	NVMeshSystem__printUncompletedIOs(sys);
	osSimulator_allert_pending_ios(&client->OS, -1);				// OS detects and allerts on lost (pending IO's) and issues a BUG
}

static inline void __connected_server_to_clnt(struct nvmeibc_disk* D, struct serverSimulator* srv, int inst_id) {
	D->local_server = (void*)srv; // Connect clients reflection to the server
	if (srv) {
		srv->client_disks[inst_id] = D;
		if (!inst_id) srv->disk = D; /* Yuri: Ugly hack for serjio. @TODO: remove, format hard drive, write random numbers above and put into microwave for 5 minutes. But first add MC support for serjio. */
	}
}

extern int  insmod_nvmeiba_all_os_apis_init(void);				// Alliasing to .ko driver's init and destroy method
extern void rm_mod_nvmeiba_all_os_apis_exit(void);

/**
 * Yuri: It is a hack for simulator, but it is the bes way I can see it done
 * When creating disk in simu_disk, we need to return a pointer to a pre allocated
 * physical disk. Problem is - it has no idea on what is nvmesh simulator system structure
 * So it will extern this function and call it to do the job
 * Here is the second problem: how to find the root nvmesh system this disk belongs to?
 * So the hack is to include nvmesh system pointer inside the name.
 *
 * @TODO: Can be done in a prettier way. nvmesh_create_disk should allocate a new disk on heap
 * and destroy it in the end.
 * In simulator, to access phyDisk pointes, special getters should be used, like with io_traits
 * This requires major refactoring, however.
 *
 */
struct nvmeibc_disk* NVMeshSystem_get_phys_disk_from_name_and_client_name(const char *name, const char *client_name) {
	struct NVMeshSystem *sys = (struct NVMeshSystem *)simple_strtoul(strstr(name, "_0x")+3, NULL, 16); /* Find the pointer encoded in the string, skip the _0x header, extract number use it as address */
	int inst_id, i;
	/* Find the client */
	for (inst_id = 0; inst_id < NVMESH_N_MAX_CLIENTS; ++inst_id)
		if (!strcmp(client_name, sys->clients[inst_id].name)) break;
	BUG_ON(inst_id >= NVMESH_N_MAX_CLIENTS);
	/* Find the disk */
	for (i = 0; i < sys->clients[inst_id].nPhysDisks; i++){
		if (!strcmp(name, sys->clients[inst_id].physDiscs[i].name))
			return &sys->clients[inst_id].physDiscs[i];
	}
	BUG();
	return NULL;
}

static void __update_clients_reflection_srv_disks(struct NVMeshSystem *sys, struct clientSimulator* client) {
	client->nPhysDisks = sys->nServers; /* Client may access each of the disks */
	for (int i=0; i<client->nPhysDisks; i++){
		struct nvmeibc_disk *D = &client->physDiscs[i];
		/* @TODO: This is pretty stupid but will work. Disk name should be generated on server.
		   Instead we do it on client simply in order not to allocate memory space for it.
		   We also encode NVMeshSystem pointer in the disk name, see NVMeshSystem_get_phys_disk_from_name_and_client_name
		   */
		sprintf(D->name,"%s_%d_0x%llx", "phDisk", i, (u64)sys);
		sys->mdb.srvrs[i].disk_name = D->name;
		D->max_gen_cmd_bb = MAX_GEN_CMD_BOUND_BUFFER;
		D->min_gen_cmd_bb = MAX_GEN_CMD_BOUND_BUFFER;
	}
}

static void __connect_client_disks_to_servers(struct NVMeshSystem *sys, struct clientSimulator* client) {
	for (int i=0; i<client->nPhysDisks; i++){
		struct nvmeibc_disk *D = &client->physDiscs[i];
		__connected_server_to_clnt(D, &sys->servers[i], client->inst_id);
	}
}

static void __destroy_clients_reflection_srv_disks(struct clientSimulator* client) {
	int i;
	for (i=0; i<client->nPhysDisks; i++){
		struct nvmeibc_disk *D = &client->physDiscs[i];
		if (D->local.jrnl.jmdc) {
			sim_kfree(D->local.jrnl.jmdc);
			D->local.jrnl.jmdc = NULL;
		}
		__connected_server_to_clnt(D, NULL, client->inst_id);
	}
	client->nPhysDisks = 0;
}

int NVMeshSystem_new_client_add(struct NVMeshSystem *sys) {
	struct clientSimulator *client1 = clientSimulator_create_instance(sys->clients);
	__update_clients_reflection_srv_disks(sys, client1);
	__connect_client_disks_to_servers(sys, client1);
	NVMeshSystem_service_nvmeshclient_start(  sys, client1->inst_id); /* Activate it within NVMeshSystem */
	NVMeshSystem_attach_client_to_all_volumes(sys, client1->inst_id); /* Attach the new client to all our volumes */
	return client1->inst_id;
}

void NVMeshSystem_service_nvmeshclient_start(struct NVMeshSystem *sys, int inst_id) {
	struct clientSimulator* client = &sys->clients[inst_id];
	struct cli_status_verification *cs = &client->cli_scripts;
	void *handle;
	struct mcs_simu *mcs = &sys->mgmt.mcs[client->inst_id];

	mgmt_init_expected_counters(mcs);

	clientSimulator_ismod(client); // Load the driver into client 0
	/* Attach the simulators to listen to clients msg-loops */
	handle = msgloop_proc_inject_reader_cb(client->name, "mcs", mcs , mgmt_incoming_msg_from_clnt_cb);		// Daniel Listens to: cc_api->mcs.msg_loop == /proc/nvmeibc/mcs/mcs
	mcs->mq.mcs = container_of(handle, struct c_api_proc, handle);
	mcs->mq.mcs->dir->fops->open(NULL, (void*)mcs->mq.mcs->msg_loop);
	handle = msgloop_proc_inject_reader_cb(client->name, "cli", client, clientSimulator_incoming_cli_msg_cb);// Daniel Listens to: cc_api->cli.msg_loop == /proc/nvmeibc/cli/cli
	cs->cli = container_of(handle, struct c_api_proc, handle);
	mcs->s.msg_counter = 0;			// Zero debug counters of msgs received from the client
}

void NVMeshSystem_attach_client_to_all_volumes(struct NVMeshSystem *sys, int inst_id) {
	struct clientSimulator* client = &sys->clients[inst_id];
	struct osSimulator *osBak;
	client->nBdevs = MAX_NORMAL_VOLUMES_IN_NVMESH; /* Client connects to all the volumes. */
	client->vols = sys->mdb.vols; /* Client reference to vol (read only) */

	client->OS.di_tracker = &sys->di_tracker; /* Client's OS reference to vol DI trackers */
	osBak = osSimulator_getCurrent();
	osSimulator_setCurrent(&client->OS); /* Select the OS to handle block device registration */
	send_command_to_all(sys, inst_id, volCmds_New);
	NVMeshSystem_serialize(sys);
	for(int v_idx = 0; v_idx < client->nBdevs; ++v_idx){
		clientSimulator_wait_for_io_enabled_for_vol(client, v_idx, false);
	}
	BUG_ON(!NVMeshSystem_is_stable(sys)); /* System must start in stable state */
	osSimulator_setCurrent(osBak);
}

void NVMeshSystem_new_client_rmv(struct NVMeshSystem *sys, int inst_id) {
	struct clientSimulator *client1 = &sys->clients[inst_id];
	NVMeshSystem_detach_client_from_all_volumes(sys, client1->inst_id);
	clientSimulator_destroy_instance(sys->clients, inst_id);
	__destroy_clients_reflection_srv_disks(client1);

	clientSimulator_rmmod(client1); // Load the driver into client 0
	client1->instance_is_active = false;
}

void NVMeshSystem_detach_client_from_all_volumes(struct NVMeshSystem *sys, int inst_id) {
	struct clientSimulator* client = &sys->clients[inst_id];
	struct osSimulator *osBak = osSimulator_getCurrent();

	osSimulator_setCurrent(&client->OS);
	send_command_to_all(sys, inst_id, volCmds_Detach);
	NVMeshSystem_serialize(sys);

	/* Poison all data */
	client->nBdevs = 0;
	client->vols = (void*)0xdeadbeef;
	client->OS.di_tracker = (void*)0xdeadbeef;
	BUG_ON(!NVMeshSystem_is_stable(sys));
	osSimulator_setCurrent(osBak);
}

extern struct volume_di_tracker_conf vdt_confs[2];

int NVMeshSystem_init(struct NVMeshSystem *sys) {
	int i;
	const u32 protocol_versions[2] = {NVMEIBT_CLIENT_PROTO_VERSION, NVMEIBT_CLIENT_PROTO_VERSION};
	struct clientSimulator* client;	// Current client

	_NI_dmesg(trace_nvmesh_sim_NVMeshSystem_init, "Starting NVMesh. CommitID:@COMMIT_ID", COMMIT_ID);
	memset(sys,0,sizeof(*sys));

	// Initialize storage servers, do not launch toma yet

	sys->nClients = NVMESH_N_MAX_CLIENTS;
	sys->nServers = NVMESH_N_PHYS_DISKS;					// Total 6 physical disks (0,1,2,3,4,5).

	// Initialize Clients OS & reflection of physical disks
	{
		int c = 0;
		for_each_client(sys->clients, client){
			client->inst_id = c++; /* This initialization here gurantees correctness in the future as we rely on inst_id = index */
			if (client->inst_id == 0) /* Initialize client names. They will not change. @TODO: maybe add an option to change sometime, but carefully. */
				scnprintf(client->name, ARRAY_SIZE(client->name), "nvmeibc");
			else
				scnprintf(client->name, ARRAY_SIZE(client->name), "mc%04d", (unsigned char)client->inst_id);
			__update_clients_reflection_srv_disks(sys, client);
		}
	}
	sys->clients[0].instance_is_active = true; /* Client instance 0 always exists */

	for (i=0; i<sys->nServers; i++){
		serverSimulator_init_hardware(&sys->servers[i], i, &sys->mdb.srvrs[i]);
		NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
	}

	// Initialize Clients OS & reflection of physical disks
	for_each_client(sys->clients, client){
		osSimulator_init(&client->OS);
		__connect_client_disks_to_servers(sys, client);
		cli_status_ver_init(&client->cli_scripts);
		client->is_nvmeiba_ko_up = 1;	// Load nvmeiba.ko
		client->is_nvmeibp_ko_up = 1;	// Load common/public .ko before the client
	}

	insmod_nvmeiba_all_os_apis_init();
	nvmeib_public_init();

	mgmt_simu_init(&sys->mgmt, &sys->mdb); 					// Initialize the mgmt

	/* Sys-admin creates and allocates the needed volumes */
	mongo_db_simu_alloc_volumes(       &sys->mdb, sys->clients[0].physDiscs);
	mongo_db_simu_cnv_to_toma_topo_all(&sys->mdb, &sys->tcf);
	mongo_db_simu_reconf_fictious_deprec_seg(&sys->mdb, &sys->tcf, 3, true);	// Optional: Add unneeded depricated segments to the configuration. Test that the system can load with them as well

	// Start tomas with the above configuration
	{
		pthread_mutexattr_t lock_attr;
		pthread_mutexattr_init(&lock_attr);
		pthread_mutexattr_settype(&lock_attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&sys->tcf.lock, &lock_attr);
	}

	for (i=0; i<sys->nServers; i++) {
		tomaSimulator_init(&sys->servers[i].simToma, &sys->mdb, &sys->tcf, protocol_versions[i%2]);
	}

	for (i=0; i<sys->nServers; i++){ // This must be after all TOMA's initialization
		serverSimulator_init_serjio(&sys->servers[i]);
	}

	NVMeshSystem_service_nvmeshclient_start(sys, 0);

	// Optional: simulate the case when client got configuration but the version is not up to date since toma's already sent switch_topology messages in the past.
	tTopoOfNVMesh_incVer(&sys->tcf);								// All volumes are outdated.

	di_tracker_init(&sys->di_tracker, &vdt_confs[false /* (not) is_ec */], MAX_NORMAL_VOLUMES_IN_NVMESH);

	NVMeshSystem_async_io_gate_init(sys);

	// Connect clients to volumes. Todo: change loops for sparse connenctivity. Must be done when toma is already up, or else toma will not let the clients register to disks
	osSimulator_setCurrent(&sys->clients->OS); /*Set the first OS simulator. Important that it is before first NVMeshSystem_attach_client_to_all_volumes*/
	for_each_active_client(&sys->clients[0], client) {
		NVMeshSystem_attach_client_to_all_volumes(sys, client->inst_id);
	}
	NVMeshSystem_serialize(sys);
	mongo_db_simu_reconf_fictious_deprec_seg(&sys->mdb, &sys->tcf, 3, false);	// Disable the above fictious segments
	send_command_to_all(sys, -1, volCmds_New);								// If deliberate errors were inserted, try fixing the volumes
	BUG_ON(!NVMeshSystem_is_stable(sys));								// System must start in stable state

	_NI_dmesg(trace_1_nvmesh_sim_NVMeshSystem_init, "******************** NVMesh created ************");
	_NI_dmesg(trace_2_nvmesh_sim_NVMeshSystem_init, "Kernel MEMORY allocations: @NUM_ALLOCS", kget_num_allocs());
	return 0;
}

int NVMeshSystem_destroy(struct NVMeshSystem *sys){
	int v,i;
	struct tTopoOfNVMesh* cf = &sys->tcf;
	struct clientSimulator* client;
	NVMeshSystem_serialize(sys);
	if (nvmeibc_get_state() != NVMEIBC_MOD_STATE_READY) {							// Verify all volumes are detached
		for_each_active_client(sys->clients, client) {
			for (v=0; v<client->nBdevs; v++)
				BUG_ON(client->devs[v]);						// Block devices should be attached anymore
		}
	} else {
		BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	}

	// Destroy data integrity tracker
	di_tracker_reset(&sys->di_tracker);
	di_tracker_fini(&sys->di_tracker);

	// Destroy the block devices of the clients
	for_each_active_client(sys->clients, client) {
		clientSimulator_rmmod(client);
		client->is_nvmeibp_ko_up = 0;	// public.ko is removed after the client
		client->is_nvmeiba_ko_up = 0;	// rmmod nvmeiba.ko
		if (0) {	// Todo: Properly close mcs/cli proc. Currently no need because /proc files were already closed by kernel at this point
			struct mcs_simu *mcs = &sys->mgmt.mcs[client->inst_id];
			msgloop_proc_inject_reader_cb(client->name, "mcs", NULL, NULL);
			msgloop_proc_inject_reader_cb(client->name, "cli", NULL, NULL);
			mcs->mq.mcs->dir->fops->release(NULL, (void*)mcs->mq.mcs->msg_loop);
		}
	}
	nvmeib_public_module_exit();
	rm_mod_nvmeiba_all_os_apis_exit();
	NVMeshSystem_serialize(sys);							// Wait until the driver was fully unloaded

	NVMeshSystem_async_io_gate_destroy(sys);

	// Stop tomas
	for (i=0; i<sys->nServers; i++)
		tomaSimulator_destroy(&sys->servers[i].simToma);

	// Destroy the mgmt (clean mongodb, mcs and mgmt)
	for (v=0; v<cf->nVolumes; v++)
		tTopoOfVolume_destroy(&cf->vols[v]);

	pthread_mutex_destroy(&cf->lock);

	mongo_db_simu_delet_volumes(&sys->mdb);					// Undo: Simulate sys-admins work to destroy volumes
	mgmt_simu_destroy(&sys->mgmt);							// Destroy mgmt (Unset the mcs completion object and mcs message queue)

	// Destroy Clients reflection of physical disks
	for_each_active_client(sys->clients, client){
		__destroy_clients_reflection_srv_disks(client);
		osSimulator_destroy(&client->OS);					//
	}
	sys->nClients = 0;
	// Destroy storage servers, do not launch toma yet
	for (i=0; i<sys->nServers; i++){
		serverSimulator_destroy(&sys->servers[i]);
	}
	sys->nServers = 0;

	_NI_dmesg(trace_nvmesh_sim_NVMeshSystem_destroy, "******************** NVMesh destroyed ************");
	return 0;
}

void NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(struct NVMeshSystem *sys, bool value){
	for (int i=0; i<sys->nServers; i++) {
		sys->servers[i].simToma.ignore_jour_gc_launch_request = value;
	}
}

static void NVMeshSystem_wait_for_serjios_wq(struct NVMeshSystem *sys) {
	int i;
	extern struct workq_struct *serjios_wq;
	(void)sys;
	drain_workqueue(serjios_wq); // in the simulator We have only one serjio wq so it's not mandatory to drain all serjios.
	for (i=0; i<sys->nServers; i++){ // This must be after all TOMA's initialization
		nvmeibs_serjio_wait_serjio_ready(&sys->servers[i].ramDisk.server_disk.di);
	}
}

void NVMeshSystem_serialize(struct NVMeshSystem *sys) {

	NVMeshSystem_async_io_gate_get(sys);

	//Actually we have 4 queues: toma, system, per-device resubmition queue & per serjio queueu
	//The first 2 may be empty, but the third one is not.
	//For example, reconfiguration topology free & registration triggering happens from there
	for (int i=0; i<2; i++) {						// Daniel: 2 iterations because system_wq can put work (unsubscribe segment) on toma worqueue
		tomaSimulator_waitProtoEnd(NULL);			// wait for all toma msgs to be processed, completing the client <--> toma updates
		kernel_work_queues_drain_all();				// do we need to wait for spawned work-items ? Daniel: Todo: Not sure, I suspect we are still missing something
		NVMeshSystem_wait_for_serjios_wq(sys);
		// Here we dont wait for resubmition queue
		// clientSimulator_wait_for_all_recoveries_done(sys->clients);	// Testing for valgrind to drain running recoveries before checking isStable
		// clientSimulator_wait_for_all_sync_ops(sys->clients);
	}
	if (0) { // Daniel: For debug, In tests where new tasks are added async, this might never be true. Use to debug tests where you know that no new tasks are added and only waiting for the old to finish
		extern struct workqueue_struct *toma_rcv_wq;
		int toma_jobs = toma_rcv_wq->num_pending_works;	// Daniel: Intentionally reading var without holding lock!
		int syst_jobs =   system_wq->num_pending_works;
		if ((toma_jobs + syst_jobs) != 0){
			_Emerg("Unitest race BUG, toma_jobs=%d sys tasks=%d\n", toma_jobs, syst_jobs);
		}
	}

	NVMeshSystem_async_io_gate_put(sys);
}

bool NVMeshSystem_is_stable(struct NVMeshSystem *sys){
	struct tTopoOfNVMesh* cf = &sys->tcf;
	struct clientSimulator *client;
	int i,v,c,s;
	bool rv = true;
	for_each_active_client(sys->clients, client)				// Verify that clients is ok
		clientSimulator_is_stable(client);

	// Verify that toma has identical lock id's to both segments of all raid1's
	for (v=0; v<cf->nVolumes; v++) {
		struct tTopoOfVolume *tv = &cf->vols[v];
		for (c=0; c<tv->nChunks; c++) {
			struct tTopoOfRaid0Chunk* tc = &tv->chunks[c];
			for (i=0; tc && (i < tc->stripeWidth); i++){
				const struct tTopoOfPraid *pr = &tc->raids[i];
				rv &= tTopoOfPraid_verify_identical_locks(pr);
				for (s=0; s<pr->header.n_segments; s++) {
					rv &= (pr->s[s].access_mode == NVMEIBTC_DS_MODE_RW);
				}
			}
		}
	}

	// Verify no stale locks on any of the disks
	for (i=0; i<sys->nServers; i++){
		ramDiskSimulator_verify_no_locks(      &sys->servers[i].ramDisk);
		ramDiskSimulator_verify_no_dirty_bits( &sys->servers[i].ramDisk);
		ramDiskSimulator_verify_no_bad_sectors(&sys->servers[i].ramDisk);
		tomaSimulator_verify_no_locks(         &sys->servers[i].simToma);
	}
	tomaNetwork_verifyNoSwitchTopoWait();
	for_each_active_client(sys->clients, client)
		osSimulator_rv_of_last_io_clean_all(&client->OS);
	return rv;
}

void NVMeshSystem_async_io_gate_init(struct NVMeshSystem *sys)
{
	pthread_mutex_init(&sys->async_io_gate.mutex, NULL);
	pthread_cond_init(&sys->async_io_gate.cond, NULL);
	sys->async_io_gate.n_requestors = 0;
}

void NVMeshSystem_async_io_gate_destroy(struct NVMeshSystem *sys)
{
	pthread_cond_destroy(&sys->async_io_gate.cond);
	pthread_mutex_destroy(&sys->async_io_gate.mutex);
}

void NVMeshSystem_async_io_gate_get(struct NVMeshSystem *sys)
{
	pthread_mutex_lock(&sys->async_io_gate.mutex);
	sys->async_io_gate.n_requestors++;
	pthread_mutex_unlock(&sys->async_io_gate.mutex);
}

void NVMeshSystem_async_io_gate_put(struct NVMeshSystem *sys)
{
	pthread_mutex_lock(&sys->async_io_gate.mutex);
	BUG_ON(sys->async_io_gate.n_requestors == 0);
	if (!(--sys->async_io_gate.n_requestors))
		pthread_cond_broadcast(&sys->async_io_gate.cond);
	pthread_mutex_unlock(&sys->async_io_gate.mutex);
}

void NVMeshSystem_async_io_gate_wait(struct NVMeshSystem *sys)
{
	pthread_mutex_lock(&sys->async_io_gate.mutex);
	while (sys->async_io_gate.n_requestors)
		pthread_cond_wait(&sys->async_io_gate.cond, &sys->async_io_gate.mutex);
	pthread_mutex_unlock(&sys->async_io_gate.mutex);
}

/*********************** NVMesh Configuration changes *************************/
/* Swaps all segments that on node n1 with segs on node n2 */
static void __segment_node_id_swap(struct NVMeshSystem *sys, u64 n1, u64 n2) {
	int v, s;
	for (v = 0; v<sys->mdb.nVols; v++) {
		struct volumeDescriptor *vol = &sys->mdb.vols[v];
		for (s = 0; s<vol->nSegments; s++) {
			struct disk_range *seg = &vol->segs[s];
			if (seg->node_id == n1) {
				_ND(trace_nvmesh_sim_segment_node_id_swap, "Swapping segment @SEG node id from @NODE_ID to @NODE_ID", seg->ruuid, (int)n1, (int)n2);
				seg->node_id = n2;
			} else if (seg->node_id == n2) {
				_ND(trace_1_nvmesh_sim_segment_node_id_swap, "Swapping segment @SEG node id from @NODE_ID to @NODE_ID", seg->ruuid, (int)n2, (int)n1);
				seg->node_id = n1;
			}
		}
	}
}

void NVMeshSystem_swap_disk_between_servers(struct NVMeshSystem *sys, int n1, int n2) {
	struct serverSimulator *t1 = &sys->servers[n1], *t2 = &sys->servers[n2];
	BUG_ON(n1==n2);
	_ND(trace_nvmesh_sim_NVMeshSystem_swap_disk_between_servers, "server @NODE_ID_STR disk @DISK_NAME <--> server @NODE_ID_STR disk @DISK_NAME", t1->hardware->node_id, t1->hardware->disk_name, t2->hardware->node_id, t2->hardware->disk_name);
	swap(sys->mdb.srvrs[n1].disk_name, sys->mdb.srvrs[n2].disk_name);
	tomaNetwork_swap_2tomas(&t1->simToma, &t2->simToma);
	__segment_node_id_swap(sys, n1, n2);
}

// This scenario swaps two drives between two servers (n1, n2) by pausing them, swapping the memory of the
// connections of the toma simulator, setting the drives servers accordingly, increasing all
// volumes versions generating a disk reappear event and cont'ing both drives, runs unitest_IO for each
// volume to ensure it works, should be called twice to "return" the original drives back to the servers (use revert)
void NVMeshSystem_DiskReappearEvent(struct NVMeshSystem *sys, int n1, int n2, bool revert) {// Todo: move to mongo db
	struct clientSimulator *client = &sys->clients[0]; // Yuri: Use client 0. @TODO: Add multiclient? Do we care?
	struct nvmeibc_disk *disks = client->physDiscs;
	int v;
	BUG_ON((n1==n2) || (n1 >= NVMESH_N_PHYS_DISKS) || (n2 >= NVMESH_N_PHYS_DISKS));
	_ND(trace_nvmesh_sim_NVMeshSystem_DiskReappearEvent, "Swaping drives between servers @NODE_ID and @NODE_ID", n1, n2);
	NVMeshSystem__invoke_pause_on_disk(sys, n1);
	NVMeshSystem__invoke_pause_on_disk(sys, n2);
	NVMeshSystem_swap_disk_between_servers(sys, n1, n2);
	if (!revert) {
		__connected_server_to_clnt(&disks[n1], &sys->servers[n2], client->inst_id);
		__connected_server_to_clnt(&disks[n2], &sys->servers[n1], client->inst_id);
	} else {
		__connected_server_to_clnt(&disks[n1], &sys->servers[n1], client->inst_id);
		__connected_server_to_clnt(&disks[n2], &sys->servers[n2], client->inst_id);
	}
	for (v=0; v<client->nBdevs; v++)
		__unitest_volume_config_version_inc(&sys->mdb.vols[v], &sys->tcf.vols[v]);

	generate_mcs_invalid_operation(&sys->mgmt.mcs[client->inst_id]);	// Used to generate a message with opcode==MCS_INVALID_MCS_OPCODE_MSG which is ignored and causes a full configuration request

	NVMeshSystem_serialize(sys);
	NVMeshSystem__invoke_cont_on_disk(sys, n1, false);
	NVMeshSystem__invoke_cont_on_disk(sys, n2, false);
}

void NVMeshSystem_notify_new_disk_sgmnts(struct NVMeshSystem *sys){
	for (s32 i = 0; i < sys->nServers; ++i){
		serverSimulator_notify_new_disk_sgmnts(&sys->servers[i]);
	}
}

/*********************** Config-to-client Propagator **************************/
int NVMeshSystem_send_volumes_config_to_clients_with_param(struct NVMeshSystem *sys, int inst_id, int vur, bool preempt, u64 reservation_version, bool allow_sub_block_io)
{
	int rv = 0;
	if (inst_id < 0) {
		struct clientSimulator *client;
		for_each_active_client(sys->clients, client)
			if ((rv = clientSimulator_get_volumes_config(client, &sys->mgmt, vur, preempt, reservation_version, allow_sub_block_io))) return rv;
	} else {
		return clientSimulator_get_volumes_config(&sys->clients[inst_id], &sys->mgmt, vur, preempt, reservation_version, allow_sub_block_io);
	}
	return rv;
}

int NVMeshSystem_send_volumes_config_to_clients(struct NVMeshSystem *sys, int inst_id) {
	return NVMeshSystem_send_volumes_config_to_clients_with_param(sys, inst_id, 'r', false, RESERVATION_MODE_IRRELEVANT, false);
}

static void __reset_predefined_command(struct NVMeshSystem* sys){
	for (int v=0; v < sys->mdb.nVols; ++v)
		sys->mdb.vols[v].nextCmd = volCmds_Illegal;
}

void send_command_predefined_to_all(struct NVMeshSystem *sys, int inst_id) {
	struct clientSimulator *client = &sys->clients[0];			// The single client in the system
	BUG_ON(NVMeshSystem_send_volumes_config_to_clients(sys, inst_id));
	NVMeshSystem_serialize(sys);
	clientSimulator_wait_for_detach_drain(client);				// If cmd was detach - Wait until all detach consequences are settled so we dont overwrite the client members
}

void send_command_to_all(struct NVMeshSystem *sys, int inst_id, enum volumeCommands cmd) {
	int v;
	struct clientSimulator *client = &sys->clients[0];			//the single client in the system
	for (v=0; v<client->nBdevs; v++)
		sys->mdb.vols[v].nextCmd = cmd;
	send_command_predefined_to_all(sys, inst_id);
	__reset_predefined_command(sys);
}

void send_command_to_vols(struct NVMeshSystem *sys, int inst_id, enum volumeCommands cmd, u8 n_volumes, struct volumeDescriptor *volumes){
	__reset_predefined_command(sys);
	for (int volume=0; volume < n_volumes; volume++)
		volumes[volume].nextCmd = cmd; 				// setup the command

	send_command_predefined_to_all(sys, inst_id);			// execute it
	__reset_predefined_command(sys);
}

void send_command_to_vol_safe_detach_no_wait(struct NVMeshSystem *sys, int inst_id, int v) {
	sys->mdb.vols[v].nextCmd = volCmds_Detach;
	NVMeshSystem_send_volumes_config_to_clients(sys, inst_id);
	NVMeshSystem_serialize(sys);
	sys->mdb.vols[v].nextCmd = volCmds_Illegal;
}

void send_command_to_vol(struct NVMeshSystem *sys, int inst_id, int v, enum volumeCommands cmd) {
	struct clientSimulator *client = &sys->clients[0];	//the single client in the system
	sys->mdb.vols[v].nextCmd = cmd;
	NVMeshSystem_send_volumes_config_to_clients(sys, inst_id);
	NVMeshSystem_serialize(sys);
	if (cmd == volCmds_Detach)
		clientSimulator_wait_for_detach_drain(client);
	sys->mdb.vols[v].nextCmd = volCmds_Illegal;
}

int send_command_to_vol_attach_or_update(struct NVMeshSystem *sys, int inst_id, int v) {
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client
	struct volumeDescriptor   *vol 		 = &sys->mdb.vols[v];		// Move segment of third volume
	int rv = 0;
	for(int i = 0; i < vol->nSegments; ++i){
		BUG_ON(vol->segs[i].dlba_start < 24ull*(1ull << 28));
	}
	vol->nextCmd = (client->devs[v] == NULL) ? volCmds_New : volCmds_Update;
	rv = NVMeshSystem_send_volumes_config_to_clients(sys, inst_id);
	vol->nextCmd = volCmds_Illegal;
	NVMeshSystem_serialize(sys);
	return rv;
}

/**************************** Low layer events API ****************************/
void NVMeshSystem__invoke_pause_on_disk(struct NVMeshSystem *sys, int disk_id){
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	struct nvmeibc_disk    *curDisk= &client->physDiscs[disk_id];
	struct serverSimulator *curServer= serverOf(curDisk);
	if (curDisk->volumes.next == NULL)
		return;													// This disk does not exists from clients perspective. Client never created it (it exists in NVMesh system but was not initialized)
	mark_disk_wait_for_admin_channel(curDisk);					// Can be sent only when admin channel is operational
	_NI(trace_nvmesh_sim_NVMeshSystem__invoke_pause_on_disk, "NVMEsh: PAUSE on disk @DISK_ID", disk_id);
	serverSimulator_disconnect(curServer);
	curDisk->detached = true;
	nvmeibc_disk_pause(curDisk);								// In real system: Arrives as interrupt
	nvmeibc_disk_start_release(curDisk, NVMEIBC_DISK_RELEASE_DISK_PAUSED);						// In real system: Arrives later on the work queue
	NVMeshSystem_serialize(sys);
}

void NVMeshSystem__invoke_cont_on_disk(struct NVMeshSystem *sys, int disk_id, bool leave_toma_not_ready){
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	struct nvmeibc_disk    *curDisk= &client->physDiscs[disk_id];
	struct serverSimulator *curServer= serverOf(curDisk);
	if (curDisk->volumes.next == NULL)
		return;													// This disk does not exists from clients perspective. Client never created it (it exists in NVMesh system but was not initialized)
	mark_disk_wait_for_admin_channel(curDisk);					// Can be sent only when admin channel is operational
	_NI(trace_nvmesh_sim_NVMeshSystem__invoke_cont_on_disk, "NVMEsh: CONT on disk @DISK_ID", disk_id);
	serverSimulator_re_connect(curServer);
	if (leave_toma_not_ready)
		curServer->simToma.state = tomaState_not_ready;			// Unitest will have to activate the toma later
	rediscovery(curDisk);
	NVMeshSystem_serialize(sys);								// Wait to complete
}

void NVMeshSystem__invoke_pause_cont_on_disk(struct NVMeshSystem *sys, int disk_id){
	struct tTopoOfNVMesh   *cf = &sys->tcf;
	NVMeshSystem__invoke_pause_on_disk(sys, disk_id);
	if ((disk_id&0x1) != 0) {
		tTopoOfNVMesh_incVer(cf);					// Test cases where all volumes become outdated, or not.
	}
	NVMeshSystem__invoke_cont_on_disk(sys, disk_id, false);
}

struct TstPRaid NVMeshSystem_TstPRaid_init_abs(struct NVMeshSystem *sys, u8 volume_index, u8 abs_praid_index){
	struct TstPRaid result = {0};
	const s32 disk_range_index = translate_praid2disk_range_index(&sys->mdb.vols[volume_index], abs_praid_index, &result.vsi);
	BUG_ON(disk_range_index < 0);
	result.vsi.volume = volume_index;
	result.cpr = &sys->mdb.vols[volume_index].segs[disk_range_index];
	result.tpr = tTopoOfVolume_getRaid1(&sys->tcf.vols[volume_index], abs_praid_index);
	return result;
}

struct TstPRaid NVMeshSystem_TstPRaid_init_rel(struct NVMeshSystem *sys, const struct volume_segment_index vsi){
	const int seg_ind = translate_segment2disk_range_index(&sys->mdb.vols[vsi.volume], vsi);
	const int s0_ind  = (seg_ind >= 0) ? (seg_ind - vsi.segment) : -1 /* not found */;
	BUG_ON(s0_ind < 0);
	return (struct TstPRaid){
		  .cpr = &sys->mdb.vols[vsi.volume].segs[s0_ind]
		, .tpr = &sys->tcf.vols[vsi.volume].chunks[vsi.chunk].raids[vsi.raid]
		, .vsi = vsi
	};
}

struct TstPRaid NVMeshSystem_pick_other_volume_disk_range(struct NVMeshSystem *sys, const struct TstPRaid *not_me_raid){
	// obviously, the combined iterator is missing
	const struct disk_range* dr_spec = &not_me_raid->cpr[not_me_raid->vsi.segment];
	for (int volume = 0; volume < sys->mdb.nVols; ++volume){
		if (volume != not_me_raid->vsi.volume){
			int disk_range_index = 0;
			struct tTopoOfVolume* tv = &sys->tcf.vols[volume];
			struct volumeDescriptor* mv = &sys->mdb.vols[volume];
			for (int chunk = 0; chunk < tv->nChunks; ++chunk) {
				struct tTopoOfRaid0Chunk* tc = &tv->chunks[chunk];
				for (int stripe = 0; stripe < tc->stripeWidth; ++stripe){
					struct tTopoOfPraid* traid1 = &tc->raids[stripe];
					for (int segment = 0; segment < traid1->header.n_segments; ++segment){
						struct disk_range* curr_dr = 0;
						++disk_range_index;
						curr_dr = &mv->segs[disk_range_index];
						if (dr_spec->length <= curr_dr->length){
							return (struct TstPRaid){
								.cpr = curr_dr, .tpr = traid1, .vsi = {.volume = volume, .chunk=chunk, .raid=stripe, .segment = segment}
							};
						}
					}
				}
			}
		}
	}
	BUG();
	return (struct TstPRaid){.cpr=0, .tpr=0, .vsi={.volume=-1, .chunk=-1, .raid=-1, .segment=-1}};
}

/* EC-5585 - Taken from dp_ec.c - needs to be modified for snake - or replaced entirely*/
#include "block/controlpath/nvmeibc_b_cp_blkset_topo.h"
struct io_phys_addr {		// EC-5585: Remove this struct
	s8 start_first_slice;	// num segs to skip from beggining of slice to start io
	s8 n_fs_cmds;// Exact Amount of commands in the first slice [1..r->slice_size].
	s8 n_lps_cmds; // How many blocks are in the last partial slice: 0 if does not exist (single slice or last slice is full) [0..r->slice_size-1]
	u16 n_slices; // Maximal length of a command (in blocks). Used for parity. Also the number of slices the IO targets
	s8 slc_strt_sgmnt_id; //slice starts at segment index
	u16 non_rw_raid_roles_map; //represents non rw roles in the raid
};

/* Struct which encapsulates phys addr calcs and segments of ec protection raid*/
struct io_phys_addr_prpr_ctx {
	u64 base_off; // Physical offset from segment start due to raid1 offset
	s16 start_first_slice;	// num segs to skip from beggining of slice to start io
	s16 n_fs_cmds;// Exact Amount of commands in the first slice [1..r->slice_size].
	s16 n_lps_cmds; // How many blocks are in the last partial slice: 0 if does not exist (single slice or last slice is full) [0..r->slice_size-1]
	s16 n_slices; // Maximal length of a command (in blocks). Used for parity. Also the number of slices the IO targets
	s16 role2sgmnt[N_MAX_RAID_SLICE_LEN];	// First seg ment is slice start, last P segments are parities

	u8 n_segs_involved; //number of segments involved in transaction
	u8 n_pari; //number of parities in the raid
	u16 n_non_rw_roles;  //number of non RW roles
	struct nvmeibc_roles_bmps bmps;
};

static inline void convert_io_phys_addr_prpr_ctx_to_ldr(struct io_phys_addr_prpr_ctx const * const from, struct io_phys_addr* to){
	to->start_first_slice = from->start_first_slice;
	to->n_fs_cmds = from->n_fs_cmds;
	to->n_lps_cmds = from->n_lps_cmds;
	to->n_slices = from->n_slices;
	to->slc_strt_sgmnt_id = from->role2sgmnt[0];
	to->non_rw_raid_roles_map = nvmeibc_get_nonrw_roles(from->bmps);
}

/* Iterator pointing to a io within lock. Calculate slices, shared with Sync */
static void io_phys_addr_calc(const struct dp_io_topo_iterator_res* it,
							  struct io_phys_addr_prpr_ctx *res)
{
	const struct nvmeibc_raid1 *r = it->r;
	const int d_own_seg = get_owner_seg_slice_start(it->r, it->rlba);
	//const int p_own_seg = ((d_own_seg + r->slice_size)%r->replicas);
	int i;
	res->start_first_slice = (it->rlba % r->slice_size);
	res->base_off = 		 (it->rlba / r->slice_size);
	res->n_fs_cmds =  min(r->slice_size - res->start_first_slice, (int)it->nlbas);
	res->n_slices =   ((it->rlba + it->nlbas - 1) / r->slice_size) - res->base_off + 1;
	res->n_lps_cmds = (res->n_slices > 1) ? (it->rlba + it->nlbas) % r->slice_size : 0;

	res->bmps = nvmeibc_roles_bmps_get_by_slba(r, res->base_off);
	for (i = 0; i < r->replicas; i++) {
		res->role2sgmnt[i] = nvmeibc_raid1_role2seg(r, d_own_seg, i);
	}

	res->n_segs_involved = min((int)it->nlbas, r->slice_size);
	res->n_pari = (u8)nvmeibc_raid1_count_bmp(r, raid.pari);
	res->n_non_rw_roles = hweight32(nvmeibc_get_nonrw_roles(res->bmps));
}

static struct io_phys_addr TEST_ONLY_get_io_physical_address(const struct dp_io_topo_iterator_res* res)
{
	struct io_phys_addr_prpr_ctx tmp;
	struct io_phys_addr result;
	io_phys_addr_calc(res, &tmp);
	convert_io_phys_addr_prpr_ctx_to_ldr(&tmp, &result);
	return result;
}

static inline s32 __get_io_n_slices(struct test_context ctx, struct nvmeibc_topology *topo, u64 vlba, u32 length)
{
	s32 n_slices = 0;
	struct dp_io_topo_iterator topo_iter;
	dp_io_topo_iterator_init(&topo_iter, vlba, length, topo, ctx.sraid.vsi.chunk);
	while( dp_io_topo_iterator_next(&topo_iter, 'l') ){
		const struct io_phys_addr bsinfo = TEST_ONLY_get_io_physical_address(&topo_iter.res);
		// n_slices: EC-5585: Todo Doron
		n_slices += bsinfo.n_slices;
	}
	return n_slices;
}

struct slice_traits get_io_slice_traits(struct test_context ctx, u64 vlba, u32 length){
	struct slice_traits traits = {0};
	struct dp_io_topo_iterator topo_iter;
	struct nvmeibc_topology *topo = nvmeibc_topology_get(&ctx.dev->topologies);
	const s32 n_max_slices = ctx.dev->dp.p.binje;
	BUG_ON( n_max_slices < __get_io_n_slices(ctx, topo, vlba, length));

	dp_io_topo_iterator_init(&topo_iter, vlba, length, topo, ctx.sraid.vsi.chunk);
	dp_io_topo_iterator_next(&topo_iter, 'l');
	{
		const struct io_phys_addr bsinfo = TEST_ONLY_get_io_physical_address(&topo_iter.res);
		// nslices // owner_seg, EC-5585: Todo Doron
		const u16 d0_sgmnt_id = bsinfo.slc_strt_sgmnt_id;
		const struct nvmeibc_raid1 *praid = topo_iter.res.r;

		traits.chunk_idx = praid->segments->toma_reg->ch;
		traits.raid_idx = praid->segments->toma_reg->r1;

		traits.replicas = praid->replicas;
		traits.slice_size = praid->slice_size;
		traits.slba = topo_iter.res.rlba / praid->slice_size;
		traits.rlba = traits.slba * praid->slice_size;
		// tx_bm EC-5585: Todo Doron
		traits.tx_bm = GENMASK(bsinfo.start_first_slice + bsinfo.n_fs_cmds -1, bsinfo.start_first_slice);

		for (u16 role = 0; role < praid->replicas; ++role){
			const u16 sgmnt_id = (d0_sgmnt_id + role) % praid->replicas;

			traits.sgmnt2role[sgmnt_id] = role;
			traits.role2sgmnt[role] = sgmnt_id;
			traits.sgmnt2dlba[sgmnt_id] = praid->segments[sgmnt_id].first_lba + traits.slba;
		}

		traits.roles_bmps = praid->calculated_data.roles_bmps[d0_sgmnt_id];
		lock_ownership_build_raid_map(praid, traits.rlba, NVMEIB_BLOCK_IO_OP_WRITE, &traits.rlmap);
	}

	nvmeibc_topology_put(topo);
	return traits;
}

extern void jdr_write_nvmeibc_roles_bmps(struct jdr* jdr, char const* name, struct nvmeibc_roles_bmps const* roles_bmp);
void jdr_write_slice_traits(struct jdr* jdr, char const* name, struct slice_traits* st)
{
	jdr_object_scope(jdr, name);

	jdr_write(jdr, st, chunk_idx);
	jdr_write(jdr, st, raid_idx);
	jdr_write_bitfield(jdr, st, rlba);
	jdr_write_bitfield(jdr, st, slba);
	jdr_write_bitfield(jdr, st, replicas);
	jdr_write_bitfield(jdr, st, slice_size);
	jdr_write_bitmap(jdr, st, tx_bm);

	jdr_write_fundamental_s_array(jdr, "role2sgmnt", st->role2sgmnt);
	jdr_write_fundamental_s_array(jdr, "sgmnt2role", st->sgmnt2role);
	jdr_write_fundamental_s_array(jdr, "sgmnt2dlba", st->sgmnt2dlba);

	jdr_write_nvmeibc_roles_bmps(jdr, "roles_bmps", &st->roles_bmps);

}

struct io_traits __alloc_io_traits(struct test_context ctx, struct nvmeibc_topology *topo, u64 vlba, u32 length){
	const s32 n_slices = __get_io_n_slices(ctx, topo, vlba, length);
	struct slice_traits* slices = sim_kmalloc(sizeof(struct slice_traits) * n_slices, GFP_KERNEL);
	BUG_ON(length == 0);
	return (struct io_traits){   .vlba = vlba
							   , .n_blocks = length
							   , .n_slices=n_slices
							   , .start_first_slice=(u16)~0
							   , .slices=slices
							   , .last_slice=&slices[n_slices-1]};
}

void free_io_traits(struct io_traits* io_traits){
	sim_kfree(io_traits->slices);
	memset(io_traits, 0, sizeof(*io_traits));
}

struct io_traits get_io_traits(struct test_context ctx, u64 vlba, u32 length)
{
	struct dp_io_topo_iterator topo_iter;
	struct nvmeibc_topology *topo = nvmeibc_topology_get(&ctx.dev->topologies);
	struct io_traits info = __alloc_io_traits(ctx, topo, vlba, length);
	const struct nvmeibc_raid1 *praid1 = NULL;

	s32 io_slice_id = -1;
	dp_io_topo_iterator_init(&topo_iter, vlba, length, topo, ctx.sraid.vsi.chunk);
	while(dp_io_topo_iterator_next(&topo_iter, 'l')){
		const struct io_phys_addr bsinfo = TEST_ONLY_get_io_physical_address(&topo_iter.res);
		const struct nvmeibc_raid1 *praid = topo_iter.res.r;
		const u64 slba = topo_iter.res.rlba / praid->slice_size;
		// ownerseg EC-5585: Todo Doron
		const u16 d0_sgmnt_id = bsinfo.slc_strt_sgmnt_id;
		u64 tail = topo_iter.res.nlbas;

		if (!praid1)
			praid1 = praid;
		else
			BUG_ON(praid1 != praid); //all I/O is comming to the same praid
		// nslices EC-5585: Todo Doron
		for (s8 slice_id = 0; slice_id < bsinfo.n_slices; ++slice_id){
			struct slice_traits* sinfo = &info.slices[++io_slice_id];
			u8 start_io = 0, end_io = 0;
			const u16 max_io_sgmnts_bmp = GENMASK(praid->slice_size-1,0);

			if (io_slice_id == 0){ // start first slice EC-5585: Todo Doron
				info.start_first_slice = bsinfo.start_first_slice;
			}
			// Fix to snake EC-5585: Todo Doron
			sinfo->slba = slba + slice_id;
			sinfo->rlba = sinfo->slba * praid->slice_size;
			sinfo->replicas = praid->replicas;
			sinfo->slice_size = praid->slice_size;

			start_io = max(topo_iter.res.rlba, (u64)sinfo->rlba) - sinfo->rlba;

			if (io_slice_id == 0 && bsinfo.start_first_slice){
				start_io = bsinfo.start_first_slice;
				end_io = bsinfo.start_first_slice + bsinfo.n_fs_cmds; // last slice EC-5585: Todo Doron
			} else if ((slice_id == bsinfo.n_slices - 1) && bsinfo.n_lps_cmds){
				end_io = (topo_iter.res.rlba + topo_iter.res.nlbas) % praid->slice_size;
			} else {
				end_io = min((u64)praid->slice_size, tail);
			}

			tail -= (end_io - start_io);

			sinfo->tx_bm = GENMASK(end_io-1, start_io);
			BUG_ON(max_io_sgmnts_bmp < sinfo->tx_bm);

			for (u16 role = 0; role < praid->replicas; ++role){
				const u16 sgmnt_id = (d0_sgmnt_id + role) % praid->replicas;

				sinfo->sgmnt2role[sgmnt_id] = role;
				sinfo->role2sgmnt[role] = sgmnt_id;
				sinfo->sgmnt2dlba[sgmnt_id] = praid->segments[sgmnt_id].first_lba + sinfo->slba;
			}

			sinfo->roles_bmps = praid->calculated_data.roles_bmps[d0_sgmnt_id];
			lock_ownership_build_raid_map(praid, sinfo->rlba, NVMEIB_BLOCK_IO_OP_WRITE, &sinfo->rlmap);
		}
		BUG_ON(tail != 0);
	}

	nvmeibc_topology_put(topo);
	return info;
}

/* ------------------ Error injection ------------------*/

u32 NVMeshSystem_get_n_trans_err_injected(struct NVMeshSystem *sys) {
	u32 n_err = 0;
	int i;
	struct clientSimulator *client;
	for_each_client(sys->clients, client)
		for (i=0; i<sys->nServers; i++) {
			struct nvmeibc_disk_hooks *disk_hooks = sys->servers[i].client_disks[client->inst_id]->disk_hooks;
			if (disk_hooks) {
				n_err += disk_hooks->args.trerr.n_errs_inj;
			}
		}

	return n_err;
}

void NVMeshSystem_gen_cmd_hooks_setup_single_disk(struct nvmeibc_disk* disk, struct nvmeibc_disk_hooks *hooks) {
	disk->disk_hooks = hooks;
}

void NVMeshSystem_gen_cmd_hooks_setup_all_disks(struct NVMeshSystem *sys, struct nvmeibc_disk_hooks *hooks) {
	int i;
	struct clientSimulator *client;
	for_each_client(sys->clients, client) {
		for (i=0; i<sys->nServers; i++) {
			struct nvmeibc_disk *disk = sys->servers[i].client_disks[client->inst_id];
			if (disk)
				NVMeshSystem_gen_cmd_hooks_setup_single_disk(disk, hooks);
		}
	}
}

void NVMeshSystem_gen_cmd_hooks_clean_all_disks(struct NVMeshSystem *sys) {
	NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, NULL);
}

/* ------------------end Error injection ------------------- */

/*****************************************************************************/
// EOF.
