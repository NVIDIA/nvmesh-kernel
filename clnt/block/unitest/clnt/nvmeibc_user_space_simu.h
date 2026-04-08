/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_USER_SPACE_SIMU_H
#define NVMEIBC_USER_SPACE_SIMU_H
/* Implementation of various client side user space apps and utils.
   1. Defines the client side simulator */

#include "./clnt/nvmeibc_cli_scripts_simu.h"
#include "nvmeibc_block.h"									// struct nvmeibc_disk
#include "block/nvmeibc_topology.h"							// struct nvmeibc_roles_bmps

struct clientSimulator {
	int nPhysDisks;											// Amount of physical disks in the system (regardless of how they are used by block devices)
	struct nvmeibc_disk   physDiscs[NVMESH_N_PHYS_DISKS];	// Array of physical disks as clients sees them (reflection of servers disks)
	int nBdevs;												// Number of thick volumes that client uses. He may not want to use all the volumes in the system.
	struct nvmeibc_block_device   *devs[MAX_VOLUMES_IN_NVMESH];// Pointers to a Block devices which mgmt knows this client is attached to (also appear under /dev/nvmesh/ directory). Can be QLC and MTV
	const struct volumeDescriptor     *vols;				// Reference to NVMeshSystem->vols. All clients point to the same array (thus saving memory) - Can be QLC (however cannot attach no as part of MTV for POC)
	struct osSimulator OS;									// Operating system of client which issues IO.
	struct cli_status_verification cli_scripts;				// Simulator of the pack of cli scripts (read/write)
	u32 is_nvmeiba_ko_up : 1;								// is nvmeiba.ko insmodded
	u32 is_nvmeibp_ko_up : 1;								// is nvmeib_common.ko, nvmeib_public.ko insmodded
	u32 is_nvmeibc_ko_up : 1;								// service nvemshclient start/stop (insmod/rmmod) is depicted by this boolean
	const struct nvmeibc_cinst_params* p;					// Todo: Support multi-instance client
	char name[10];											// Client string name - in simulator it is an important key as it is used to connect between simulator structures and cinst
	int inst_id;											// Client instance ID = index in simulator client array, NOT necessarily same as cinst.p.index
	bool instance_is_active;								// True if this client instance was added to the system false otherwise
};

/**
 * Iterate over all clients that are added to the system
 * @param client0 Pointer to the clients array first element of type struct clientSimulator *
 * @param iter Iterator variable of type struct clientSimulator *
 */
#define for_each_active_client(client0, iter)                                      \
	for_each_client(client0, iter)                                                 \
		if ((iter)->instance_is_active)

#define for_each_non_active_client(client0, iter)                                  \
	for_each_client(client0, iter)                                                 \
		if (!(iter)->instance_is_active)

/**
 * Iterate over all clients possible
 * @param client0 Pointer to the clients array first element of type struct clientSimulator *
 * @param iter Iterator variable of type struct clientSimulator *
 */
#define for_each_client(client0, iter)                                             \
	for ((iter) = (client0); ((iter) - (client0)) < NVMESH_N_MAX_CLIENTS; ++(iter)) \

struct clientSimulator *clientSimulator_create_instance(struct clientSimulator *client0);
void clientSimulator_destroy_instance(struct clientSimulator *client_0, int inst_id);

void clientSimulator_ismod(			 struct clientSimulator *client);
void clientSimulator_rmmod(			 struct clientSimulator *client);
void clientSimulator_print_proc_dir( struct clientSimulator *client, bool verbose); /* cat /proc/nvmesh/. */
void clientSimulator_dump_procfs_to_disk(struct clientSimulator *client, const char *rootPath); /* Dump entire /proc tree to disk */
void clientSimulator_print_proc_files_of_vol(struct clientSimulator *client, bool verbose, int volInd); /* cat/proc/nvmeibc/<volume name>/<wildcard> on Linux */
void clientSimulator_print_proc_file_by_path(struct clientSimulator *client, const char *path); /* cat path on Linux */
struct proc_dir_entry* clientSimulator_find_proc_file_by_path(struct clientSimulator *client, const char *path);
struct proc_dir_entry* clientSimulator_find_vol_proc_file_by_path(struct clientSimulator *client, int volInd, const char *path);
int clientSimulator_get_num_executed_ioctls(struct clientSimulator *client);

bool    clientSimulator_is_vol_recoverer_attached(struct clientSimulator *client, int volInd);
bool    clientSimulator_does_vol_allow_512B_IO(struct clientSimulator *client, int volInd);
#define clientSimulator_sizeof_bdev(         client, vol_i)	(((client)->OS.disks[vol_i]->nr_sects) >> KERNEL_SECTOR_TO_SECTOR_SHIFT) // Alternative can use: client->devs[vol_i]->size


static inline void  __attribute__((format (printf, 2, 3)))
clientSimulator_send_to_cli_va(struct clientSimulator* client, const char* cmd, ...)
{
	int rc = 0;
	char buf[512] = {};
	extern void clientSimulator_wait_for_mainwq(struct clientSimulator *client);

	va_list args;
	va_start(args, cmd);
	rc = vsnprintf(buf, ARRAY_SIZE(buf)-1, cmd, args);
	va_end(args);
	BUG_ON(rc < 0);

	cli_send_command_to_clnt(&(client)->cli_scripts, buf);
	clientSimulator_wait_for_mainwq( client);
}

#define clientSimulator_send_to_cli(         client, cmd) ({ cli_send_command_to_clnt(&(client)->cli_scripts, cmd); clientSimulator_wait_for_mainwq( client); })
#define clientSimulator_send_to_cli_and_wait(client, cmd) ({ clientSimulator_send_to_cli(client, cmd); wait_for_cli_status_verification(client); })

int clientSimulator_incoming_cli_msg_cb(void *_clnt, const char *buf, size_t len); 	// Accepts all strings sent to cli by client's production kernel code to clients user space scripts.

struct mgmt_simu;
int clientSimulator_get_volumes_config(struct clientSimulator *client, struct mgmt_simu *mgmt, int vur /* v-name, u-uuid, r-random */, bool preempt, u64 reservation_version, bool allow_sub_block_io);	// Propagate volume status (attachd/etach/delete/update/...) to client
void prepare_cli_status_verification_for_shutdown(struct clientSimulator *client, const bool is_upgrade); // Prepare expectors for upgrade messages or shutdown messages
int clientSimulator_get_volume_version(struct clientSimulator *client, int vol_index);

/*************************** Waiting For Event ********************************/
void    clientSimulator_wait_for_mainwq(struct clientSimulator *client);
#define clientSimulator_wait_for_all_ecpus_to_finish(client) ecpu_set_drain(&(client)->OS.kernel->ecpu_set);
void    clientSimulator_wait_for_detach_drain(struct clientSimulator *client);

/* All IO's might finished (returned results to OS), but are now in process of
   free() and returning from deep stack of calling functions. Wait for that */
struct nvmeibc_topologies;
void clientSimulator_wait_for_all_topo_users_to_finish(struct nvmeibc_topologies *nt);

/* Wait until client registers with toma on volume 'v' IO becomes enabled and
   client is in idle state (no-ios).
   NOTE: If "Self leaking Tomatos" msg appear - probably the unitest
   scenario is wrong (you don't understand to which condition to wait). Do not
   supress the warning, unless you absolutely understand what you are doing and
   can justify it */
void clientSimulator_wait_for_io_enabled_for_vol(struct clientSimulator *client, int v, const bool supress_warning);
void clientSimulator_wait_for_io_enabled_for_vol_non_idle(struct clientSimulator *client, int v);	// Todo: Remove: Same as above but used when IO's are being issued to client and go to resubmition thread when IO is disabled
void clientSimulator_wait_for_io_toggle_init(struct clientSimulator *client, int v);				// Idle wait for completion of a single io toggle, Todo, extend it to 'N' toggles when needed
void clientSimulator_wait_for_io_toggle_wait(struct clientSimulator *client, int v);

/* Not needed. Just wait for termination of all io, who cares about resubmittion */
void clientSimulator_wait_for_all_io_resubmittion(struct clientSimulator *client, int volInd);

/* Assuming There is no IO in air. Wait until all volumes of have a single
   topology. IE: Wait for all leftovers (releasing lockss, abandon, sync, etc.)
   to finish. They are using topology so waiting for ref count drop to zero is enough */
void clientSimulator_wait_for_single_topo_no_io(struct clientSimulator *client);

/* Assuming test env does NOT issue any IO's, their number can only decrease.
 * 1. First wait for all kernel bio's to end
 * 2. Do: clientSimulator_wait_for_single_topo_no_io() */
void clientSimulator_wait_for_all_bio_ops(struct clientSimulator *client);

/* Assuming no new sync operations are issued, Wait until all syncs finish */
void clientSimulator_wait_for_all_sync_ops(struct clientSimulator *client);

/* Assuming no new recovery operations are issued*/
void clientSimulator_wait_for_all_recoveries_done(struct clientSimulator *client);

/* Verify that client is stable (volumes are attached, all io finished, etc) */
bool clientSimulator_is_stable(const struct clientSimulator *client);

/******************************************************************************/
/* Todo: Reorganize the stuff below of cli commands*/
static inline void reset_cli_status_verification(struct clientSimulator *client){
	const struct c_api_proc *cli = client->cli_scripts.cli;
	 cli_status_ver_reset(&client->cli_scripts);
	 cli->dir->fops->open(NULL, (void*)cli->msg_loop);		 // Before sending the cmds
}

static inline void set_cli_status_verification_expector(struct clientSimulator *client, const char *status, const int vol_ind){
	cli_status_ver_set_expector(&client->cli_scripts, status, vol_ind, false);
}

static inline void set_cli_status_verification_sensetive_expector(struct clientSimulator *client, const char *status, const int vol_ind){
	cli_status_ver_set_expector(&client->cli_scripts, status, vol_ind, true);
}

static inline void set_cli_error_expector(struct clientSimulator *client, const enum NVMEIBC_CLI_ERROR_TYPES err){
	reset_cli_status_verification(client);
	cli_status_ver_set_err(&client->cli_scripts, err);
}

static inline void wait_for_cli_status_verification(struct clientSimulator *client){
	const struct c_api_proc *cli = client->cli_scripts.cli;
	cli_status_ver_match_expector(&client->cli_scripts, NULL);
	cli_status_ver_wait_for(&client->cli_scripts);
	cli->dir->fops->release(NULL, (void*)cli->msg_loop);	// After completing expectation
}

#endif // NVMEIBC_USER_SPACE_SIMU_H
