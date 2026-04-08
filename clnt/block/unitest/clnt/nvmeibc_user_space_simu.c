/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "nvmeibc_user_space_simu.h"
#include "nvmeibc_volume.h"
#include "../nvmeibc_block_common.h"
#include "module/instance/nvmeibc_cinst_params.h"							// To test states of client instance, access lists of volumes, etc
#include "block/unitest/nvmeibc_simu_disk.h"
#include <sys/stat.h>
#include <errno.h>
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

#define cli_vol_status_is_detached(s) (!strcmp(s, CLI_DETACHED) || !strcmp(s,CLI_ATTACH_FAILED) || !strcmp(s,CLI_SHUTDOWN) || !strcmp(s,CLI_UPDATE_READY))
#define cli_vol_status_is_attached(s) (!strcmp(s, CLI_ATTACHED))

int clientSimulator_incoming_cli_msg_cb(void *_ctx, const char *buf, size_t len){
	struct clientSimulator* client = _ctx;
	const struct nvmeibc_cinst_params_main* c_inst = &client->p->main;		// In fact the context should be the current client instace
	struct nvmeibc_volume_header info;
	char str_status[32] = {0}; // see __vol_cmd_enum_to_result(enum_vol_status status)
	char str_blocked[8] = {0};
	u64 reservation_version;

	cli_status_ver_incoming_msg_from_clnt_cb(&client->cli_scripts, buf, len);
	if ((buf[0] == ':')||(!strncmp(buf, "ERR:", 4))||(!strncmp(buf, "RETRY", 5)))
		return 0;				// Not interested in Token cancel and errors
	BUG_ON(strncmp(buf, CLI_MSG_FORMAT, 7));	// Incorrect cli message
	if (sscanf(buf, CLI_MSG_FORMAT, str_status, &info.version, str_blocked, info.uuid, info.devname, &reservation_version) != 6) {
		BUG_ON(info.version != -1); // or strcmp(str_status, CLI_UNKNOWN);  unknown volume
		return 0;
	}
	info.type = mongo_db_simu_is_thick_vol(info.uuid);
	{
		const int v = mongo_db_simu_get_thick_vol_ind(info.uuid);
		if (cli_vol_status_is_detached(str_status)) {
			client->devs[v] = NULL;
		} else if (cli_vol_status_is_attached(str_status)) {
			if (!client->devs[v])
				client->devs[v] = nvmeibc_volume_get_by_uuid(c_inst, info.uuid, info.type)->block_dev; // does not work if blockedv does not register with OS: (struct nvmeibc_block_device*)client->OS.disks[v]->queue->queuedata
			if (client->devs[v]->os->is_io_api_disabled)
				osSimulator_nullify(&client->OS, v);				// Just for debug for recovery volumes
		}
	}
	return 0;
}

int clientSimulator_get_num_executed_ioctls(struct clientSimulator *clnt) {
	const struct nvmeibc_control_api *cc_api = container_of(clnt->cli_scripts.cli, struct nvmeibc_control_api, cli);
	// Alternatively can use cc_api = __get_from_params_main_globals_container(&clnt->p->main)->cc_api;
	return cc_api->ioctls.num_executed_ioctls;	// Only works for first client instance
}

bool clientSimulator_is_vol_recoverer_attached(struct clientSimulator *client, int v) {
	return client->devs[v] && client->devs[v]->os->is_io_api_disabled;
}

bool clientSimulator_does_vol_allow_512B_IO(struct clientSimulator *client, int v) {
	return ((client->devs[v]->os->atom.queue->limits.logical_block_size ==  (1 << KERNEL_SECTOR_SHIFT)) &&
			(client->devs[v]->os->atom.queue->limits.physical_block_size == (1 << KERNEL_SECTOR_SHIFT)));
}

/******************************************************************************/
extern int  insmod_nvmeibc_init(void);							// Alliasing to .ko driver's init and destroy method
extern void rm_mod_nvmeibc_exit(void);

struct clientSimulator *clientSimulator_create_instance(struct clientSimulator *client0) {
	char cmd[100];
	struct clientSimulator *new_client = NULL;
	for_each_non_active_client(client0, new_client) break; /* Just grab the first unused and be off with it */
	BUG_ON(new_client >= client0 + NVMESH_N_MAX_CLIENTS); /* Must be at least one left */
	scnprintf(cmd, ARRAY_SIZE(cmd), "%%clnt++{%s,%s}", new_client->name, new_client->name);
	clientSimulator_send_to_cli(client0, cmd); /* Send the cmd - always to client 0 */
	new_client->instance_is_active = true;
	return new_client;
}

void clientSimulator_destroy_instance(struct clientSimulator *client0, int inst_id) {
	char cmd[100];
	struct clientSimulator * client = &client0[inst_id];
	scnprintf(cmd, ARRAY_SIZE(cmd), "%%clnt--{%s,%s}", client->name, client->name);
	clientSimulator_send_to_cli(client0, cmd); /* Send the cmd - always to client 0 */
}

void clientSimulator_ismod(struct clientSimulator *client) {
	BUG_ON(client->is_nvmeibc_ko_up || (client->inst_id == 0 && insmod_nvmeibc_init())); /* Load driver on instance 0 only */
	client->p = nvmeibc_cinst_get_by_name(client->name);
	client->is_nvmeibc_ko_up = true;
}

#include "nvmeibc_volume.h"	/* Verify nothing left behind in clients .ko */
static void __verify_client_ko_fully_unloded(struct clientSimulator *client) {
	int i, was_used;
	struct nvmeibc_disk *D;
	struct list_head *volumes;
	for (i = 0; i < client->nPhysDisks; i++){			// Verify no disk is attached to volume from the simulator side
		D = &client->physDiscs[i];
		volumes = &D->volumes;
		was_used = (((u64)volumes->next|(u64)volumes->prev) != 0);	// Disk was used by at least one volume. If not, then it is not initialized at all
		if (was_used && !list_empty(volumes)) {
			struct nvmeibc_disk_id *disk_id;
			list_for_each_entry(disk_id, volumes, slink)
				_Emerg("Disk %d is still used by volume disk_id %p!\n", i, disk_id);
			BUG();
		}
	}
}

void clientSimulator_rmmod(struct clientSimulator *client) {
	int v;
	if (client->inst_id == 0) {
		osSimulator_setCurrent(&client->OS);				// Select the OS to handle block device destruction
		rm_mod_nvmeibc_exit();								// Free data and unload the .ko driver
		__verify_client_ko_fully_unloded(client);
	}
	client->is_nvmeibc_ko_up = false;
	for (v=0; v<client->nBdevs; v++)
		client->devs[v] = NULL;							// Block devices do not exist anymore
}

/******************************************************************************/
struct proc_path_params {
	const char* path;				// Remaining path to match (consumed during traversal)
	struct proc_dir_entry* e;		// For find: the found entry
};

static int __stream_proc_file(struct proc_dir_entry* e, void* _ctx) {
	char buf[4096];
	loff_t offset = 0;
	int cnt;
	(void)_ctx;

	if (!e->data) {
		return true;
	}

	unitest_print("\n" KERN_COL_WHITE_BOLD "File:%s\n" KERN_COL_RESET, e->name);
	for (;;) {
		cnt = e->fops->read((void *)e->data, buf, sizeof(buf) - 1, &offset);
		if (cnt <= 0) {
			break;
		}
		buf[cnt] = '\0';
		unitest_print("%s", buf);
	}
	return true; 		// Always enter sub directories
}

static char __does_proc_path_match(struct proc_dir_entry* e, struct proc_path_params *p) {
	if (p->path) {
		const int name_len = strlen(e->name);
		if (!strncmp(e->name, p->path, name_len)) {
			if (!e->data) {
				p->path += (name_len + 1);	// skip the directory + '/'
				return 'S';					// File not found but we found a parent directory so go into sub dirs
			} else if (p->path[name_len] != 0) {
				// Wrong match, example: path = "abc/file.txt", matched to file = "a"
			} else {
				p->path = NULL;				// File was already found. Consumed the entire patch, Skip all the rest
				return 'F';					// File found
			}
		}
	}
	return 0;									// File not found and sub dirs are irrelevant
}

static int __stream_proc_file_if_in_path(struct proc_dir_entry* e, void* _p) {
	const int rv = __does_proc_path_match(e, _p);
	if (rv == 'F') {
		__stream_proc_file(e, NULL);
	}
	return (rv == 'S');
}

static int __find_proc_file_by_path(struct proc_dir_entry* e, void* _p) {
	struct proc_path_params *p = _p;
	const int rv = __does_proc_path_match(e, p);
	if (rv == 'F') {
		p->e = e;
	}
	return (rv == 'S');
}

static int __stream_dir_tree(struct proc_dir_entry* e, void* _ctx) {
	int indent;
	(void)_ctx;

	indent = (int)((!e->data) ? e->depth : (e->depth-1))*2;
	unitest_print("%*s%s/%s\n", indent, "", (e->data ? "|    " : "|_"), e->name);
	return true; 		// Always enter sub directories
}

/* Read NVMesh info from /proc */
void clientSimulator_print_proc_dir(struct clientSimulator *client, bool verbose) {
	if (!verbose) {
		return;
	}
	procfs_traverse_tree_dfs(&client->OS.kernel->procfs, NULL, __stream_dir_tree);
	procfs_traverse_tree_dfs(&client->OS.kernel->procfs, NULL, __stream_proc_file);
}

/******************************************************************************/
/* Context for dumping proc files to filesystem */
struct proc_dump_to_fs_ctx {
	char current_path[PATH_MAX];	// Current accumulated path
	int depth;						// Current depth in directory tree
};

/* Helper: Create directory recursively (like mkdir -p) */
static int __mkdir_recursive(const char *path) {
	char tmp[PATH_MAX];
	char *p = NULL;
	size_t len;

	snprintf(tmp, sizeof(tmp), "%s", path);
	len = strlen(tmp);
	if (tmp[len - 1] == '/')
		tmp[len - 1] = 0;

	for (p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
				unitest_print("Error: Failed to create directory %s: %s\n", tmp, strerror(errno));
				return -1;
			}
			*p = '/';
		}
	}
	if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
		unitest_print("Error: Failed to create directory %s: %s\n", tmp, strerror(errno));
		return -1;
	}
	return 0;
}

/* Helper: Pop N subdirectories from current_path */
static void __pop_path_components(char *path, int count) {
	char *p;
	int i;

	for (i = 0; i < count; i++) {
		p = strrchr(path, '/');
		if (p && p != path) {
			*p = '\0';
		} else {
			/* Reached root or invalid state */
			path[0] = '\0';
			break;
		}
	}
}

/* Helper: Write proc file content to filesystem */
static bool __write_proc_file_to_disk(struct proc_dir_entry *e, const char *current_path) {
	char file_path[PATH_MAX];
	char buff[4096*64];
	loff_t offset = 0;
	int bytes_read, rv;

	/* Build file path */
	if ((size_t)scnprintf(file_path, sizeof(file_path), "%s/%s", current_path, e->name) >= sizeof(file_path)) {
		unitest_print("Error: Path too long, skipping file: %s/%s\n", current_path, e->name);
		return true; /* Continue with other files */
	}

	/* Read proc file content in chunks and write to disk */
	bytes_read = e->fops->read((void *)e->data, buff, sizeof(buff), &offset);
	if (bytes_read > 0) {
		rv = nvmeib_write_file(file_path, buff, bytes_read);
		if (rv < 0) {
			unitest_print("Error: Failed to write to %s: %s\n", file_path, strerror(errno));
			return true; /* Continue with other files */
		}
	}

	return true; /* Continue traversal */
}

/* Helper: Handle directory entry - adjust path and create directory */
static bool __handle_directory_entry(struct proc_dir_entry *e, struct proc_dump_to_fs_ctx *ctx) {
	size_t path_len, remaining;
	int n, pop_count;

	/* Adjust path based on depth changes */
	if (e->depth <= ctx->depth) {
		/* Same level or going back up - pop directories and append new name */
		pop_count = ctx->depth - e->depth + 1;
		__pop_path_components(ctx->current_path, pop_count);
	}

	/* Append directory name to current path */
	path_len = strlen(ctx->current_path);
	remaining = sizeof(ctx->current_path) - path_len;
	n = scnprintf(ctx->current_path + path_len, remaining, "/%s", e->name);

	if (n <= 0 || (size_t)n >= remaining) {
		unitest_print("Error: Path too long: %s/%s\n", ctx->current_path, e->name);
		return false; /* Stop traversal on error */
	}

	/* Update context depth */
	ctx->depth = e->depth;

	/* Create directory */
	if (__mkdir_recursive(ctx->current_path) != 0) {
		return false; /* Stop traversal on error */
	}

	unitest_print("Created directory: %s (depth=%d)\n", ctx->current_path, e->depth);
	return true; /* Enter subdirectories */
}

/* Callback to dump each proc file to filesystem */
static int __dump_proc_file_to_fs(struct proc_dir_entry* e, void* _ctx) {
	struct proc_dump_to_fs_ctx *ctx = _ctx;

	/* Directory: handle path and create it */
	if (!e->data) {
		return __handle_directory_entry(e, ctx);
	}

	/* File: write proc content to filesystem */
	return __write_proc_file_to_disk(e, ctx->current_path);
}

void clientSimulator_dump_procfs_to_disk(struct clientSimulator *client, const char *rootPath) {
	int n;
	struct proc_dump_to_fs_ctx ctx;
	if (!rootPath || strlen(rootPath) == 0 || rootPath[0] == '/') {
		unitest_print("Error: Invalid procfs dump root path %s\n",
		              rootPath ? rootPath : "<null>");
		return;
	}
	/* Initialize context */
	n = scnprintf(ctx.current_path, sizeof(ctx.current_path), "%s/%lld", rootPath, (s64)ktime_get());
	if (n <= 0 || (size_t)n >= sizeof(ctx.current_path)) {
		unitest_print("Error: Path too long: %s\n", rootPath);
		return;
	}
	ctx.depth = -1;

	/* Create root directory */
	if (__mkdir_recursive(ctx.current_path) != 0) {
		unitest_print("Error: Failed to create root directory %s\n", ctx.current_path);
		return;
	}

	/* Traverse entire proc tree starting from root and dump all files */
	procfs_traverse_tree_dfs(&client->OS.kernel->procfs, &ctx, &__dump_proc_file_to_fs);
}

void clientSimulator_print_proc_files_of_vol(struct clientSimulator *client, bool verbose, int volInd) {
	struct nvmeibc_block_device	*bdev = client->devs[volInd];
	BUG_ON(volInd >= client->nBdevs);
	if (!verbose) {
		return;
	}
	procfs_traverse_tree_dfs(bdev->os->procfs->dir, NULL, __stream_proc_file);
}

void clientSimulator_print_proc_file_by_path(struct clientSimulator *client, const char *path) {
	struct proc_path_params p = {.path = (path + 1), .e = NULL};	// Skip leading '/'
	procfs_traverse_tree_dfs(&client->OS.kernel->procfs, &p, __stream_proc_file_if_in_path);
}

struct proc_dir_entry* clientSimulator_find_proc_file_by_path(struct clientSimulator *client, const char *path) {
	struct proc_path_params p = {.path = (path + 1), .e = NULL};	// Skip leading '/'
	procfs_traverse_tree_dfs(&client->OS.kernel->procfs, &p, &__find_proc_file_by_path);
	return p.e;
}

struct proc_dir_entry* clientSimulator_find_vol_proc_file_by_path(struct clientSimulator *client, int volInd, const char *path) {
	struct nvmeibc_block_device	*bdev = client->devs[volInd];
	char path_including_vol[PATH_MAX];
	struct proc_path_params p = {.path = path_including_vol, .e = NULL};

	BUG_ON(volInd >= client->nBdevs);
	snprintf(path_including_vol, PATH_MAX, "%s/%s", bdev->name, path);
	procfs_traverse_tree_dfs(bdev->os->procfs->dir, &p, &__find_proc_file_by_path);
	return p.e;
}

#include "./mgmt/nvmeibm_mcs_simu.h"
int clientSimulator_get_volumes_config(struct clientSimulator *client, struct mgmt_simu *mgmt, int vur, bool preempt, u64 reservation_version, bool allow_sub_block_io)
{
	int res = 0, v;
	int len = NVMEIBC_BD_UUID_LEN + 8 + 17 + 36 + 7 /* strlen("attachu ") + " token --XX rv --preempt --512" */;
	char cli_command[len];
	static u64 token_generator = 0x14;
	static int exec_counter = 0;									// Counts number of this function calls
	const struct volumeDescriptor* vols = client->vols;
	struct nvmeibc_block_device** devs = client->devs;
	char token[16];
	//TODO - should be possible to replace all non recovery attaches with a message to the mgmt simulator to send the attach message
	//1. weak preempt might require some work around if required and we can remove that verification if it's not going to happen from mgmt anymore,
	//	the reason was debugability to see that we requested EX access (and got it) without preempt request.
	reset_cli_status_verification(client);
	for (v=0; v<client->nBdevs; v++){
		const struct nvmeibc_volume_header *info = &vols[v].info;
		const char *vol_id;
		const bool is_attached = (devs[v] != NULL);
		const bool is_recoverer_attached = is_attached && nvmeibc_block_is_recoverer(devs[v]);
		const bool is_shadow_attached = is_attached && nvmeibc_block_is_shadow(devs[v]);
		BUG_ON(info->devname[0] == 0);
		if (vur == 'r')
			vur = (exec_counter%2 ? 'u' : 'v');						// Random decision: give command by uuid or by name
		vol_id = ((vur == 'v') ? info->devname : info->uuid);
		memset(&cli_command, 0, len);								// Destroy all leftovers for previous command

		if (vols[v].nextCmd == volCmds_Detach ) {					// Detach command is sent via CLI
			set_cli_status_verification_expector(client, detach_string(&vols[v], (vur == 'u'), (&(*devs[v]) != NULL)), v);
			snprintf(cli_command, len, "detach%c %s", vur, vol_id);
		} else if (vols[v].nextCmd == volCmds_ForceDetach) {
			set_cli_status_verification_expector(client, detach_string(&vols[v], (vur == 'u'), (&(*devs[v]) != NULL)), v);
			snprintf(cli_command, len, "detach%c %s --force", vur, vol_id);
		} else if (vols[v].nextCmd == volCmds_DetachUpgrade) {
			set_cli_status_verification_expector(client, update_ready_string(&vols[v], (vur == 'u'), (&(*devs[v]) != NULL), devs[v]->os->is_io_api_disabled), v);
			snprintf(cli_command, len, "detach%c %s --upgrade", vur, vol_id);
		} else if (vols[v].nextCmd == volCmds_Delete) {				// Delete command is sent by an MCS volumeRemovedEvent message
			set_cli_status_verification_expector(client, detach_string(&vols[v], true        , (&(*devs[v]) != NULL)), v);
			generate_mcs_delete_message_for_volume(&mgmt->mcs[client->inst_id], &vols[v], MCS_VOLUME_DELETION_MESSAGE_MSG);
		} else if (vols[v].nextCmd == volCmds_New) {				// Attach command is sent via CLI
			snprintf(&token[0], sizeof(token), "%015llu", ++token_generator);
			set_cli_status_verification_expector(client,
				(is_recoverer_attached || is_shadow_attached) ? failed_attach_string(&vols[v], CLI_UPDATE_FAILED) : attach_string(&vols[v]),
				v);
			snprintf(cli_command, len, "attach%c %s %s --RW %llu%s%s", vur, vol_id, token, reservation_version, (preempt) ? " --preempt" : "", (allow_sub_block_io) ? " --512" : "");
		} else if (vols[v].nextCmd == volCmds_ShadowAttach) {				// Attach command is sent via CLI
			set_cli_status_verification_expector(client,
				(is_attached && !is_shadow_attached) ? failed_attach_string(&vols[v], CLI_UPDATE_FAILED) : attach_string(&vols[v]),
				v);
			snprintf(cli_command, len, "attach%c %s %s --RW %llu%s%s", vur, vol_id, MAGIC_CONFIG_SHADOW_TOKEN, reservation_version, (preempt) ? " --preempt" : "", (allow_sub_block_io) ? " --512" : "");
		} else if (vols[v].nextCmd == volCmds_Update) {				// Update command is generated via MCS ATTACH volume message (TODO(Doron): once we have more specific MCS commands such as volumeExtendedEvent use them)
			set_cli_status_verification_expector(client, update_string(&vols[v]), v); // Update can no longer arrive from CLI instead generate an MCS attach message directly
			generate_mcs_attach_message_for_volume(&mgmt->mcs[client->inst_id], &vols[v], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
		} else if (vols[v].nextCmd == volCmds_RecoveryUpdate) {				// Same as above
			set_cli_status_verification_expector(client, cli_generic_string(&vols[v], CLI_ATTACHED, true), v); // Update can no longer arrive from CLI instead generate an MCS attach message directly
			generate_mcs_attach_message_for_volume(&mgmt->mcs[client->inst_id], &vols[v], MCS_ATTACH_VOLUMES_MESSAGE_MSG, false, MAGIC_CONFIG_UPDATE_TOKEN, NVMEIB_MCS_MSG_WITH_NO_ERROR);
		} else if (vols[v].nextCmd == volCmds_RecoveryAttach) {
			set_cli_status_verification_expector(client,
				(is_attached && !is_recoverer_attached) ? failed_attach_string(&vols[v], CLI_UPDATE_FAILED) : recovery_attach_string(&vols[v], is_attached),
				v);
			snprintf(cli_command, len, "attach%c %s %s", vur, vol_id, MAGIC_RECOVR_ATTACH_TOKEN); // MAGIC_RECOVR_ATTACH_TOKEN
		} else if (vols[v].nextCmd == volCmds_ForceRecoveryDetach) {
			set_cli_status_verification_expector(client, (devs[v] != NULL) ? detach_recov_string(&vols[v], is_recoverer_attached) : cli_unknown_string(vol_id, (vur == 'u')), v);
			snprintf(cli_command, len, "detach%c %s --recov --force", vur, vol_id);
		} else if (vols[v].nextCmd == volCmds_RecoveryDetach) {
			set_cli_status_verification_expector(client, detach_recov_string(&vols[v], is_recoverer_attached), v);
			snprintf(cli_command, len, "detach%c %s --recov", vur, vol_id);
		} else if (vols[v].nextCmd == volCmds_DetachUpgradeRecoveryForce) {
			set_cli_status_verification_expector(client, detach_recov_string(&vols[v], is_recoverer_attached), v);
			snprintf(cli_command, len, "detach%c %s --recov --force --upgrade", vur, vol_id);
		} else if (vols[v].nextCmd == volCmds_AttachReadOnly) {
			snprintf(&token[0], sizeof(token), "%015llu", ++token_generator);
			set_cli_status_verification_expector(client,
				(is_recoverer_attached || is_shadow_attached) ? failed_attach_string(&vols[v], CLI_UPDATE_FAILED) : attach_string(&vols[v]),
				v);
			snprintf(cli_command, len, "attach%c %s %s --RO %llu%s%s", vur, vol_id, token, reservation_version, (preempt) ? " --preempt" : "", (allow_sub_block_io) ? " --512" : "");
		} else if (vols[v].nextCmd == volCmds_AttachExclusive) {
			snprintf(&token[0], sizeof(token), "%015llu", ++token_generator);
			set_cli_status_verification_expector(client,
				(is_recoverer_attached || is_shadow_attached) ? failed_attach_string(&vols[v], CLI_UPDATE_FAILED) : attach_string(&vols[v]),
				v);
			snprintf(cli_command, len, "attach%c %s %s --EX %llu%s%s", vur, vol_id, token, reservation_version,
				(preempt) ? " --preempt" : "",
				(allow_sub_block_io) ? " --512" : "");
		} else if (vols[v].nextCmd == volCmds_Illegal) { // Remaining is Illegal command
			_ND(trace_user_space_simu_clientSimulator_get_volumes_config, "Volume @DEV_NAME has no new command", vols[v].info.devname);
			continue;
		} else { BUG();		/* Unsupported command in unitests */}

		if (cli_command[0]) {
			_ND(trace_1_user_space_simu_clientSimulator_get_volumes_config, "Sending @CLI_COMMAND command to CLI", cli_command);
			BUG_ON((strlen(cli_command)+1) > (size_t)len);
			cli_send_command_to_clnt(&client->cli_scripts, cli_command);
		}
		res++;
	}
	exec_counter++;
	_ND(trace_2_user_space_simu_clientSimulator_get_volumes_config, "Sent CLI commands for @RES volumes via @VUR", res, vur);
	clientSimulator_wait_for_mainwq(client);
	wait_for_cli_status_verification(client);
	return 0;
}

// Must call wait_for_cli_status_verification after shutdown/upgrade command is sent to cli
void prepare_cli_status_verification_for_shutdown(struct clientSimulator *client, const bool is_upgrade){
	int v;
	reset_cli_status_verification(client);
	for (v=0; v<client->nBdevs; v++) {
		if (client->devs[v] != NULL) { // No need to set expector, if not attached client will not send shutdown or upgrade status
			if (is_upgrade) set_cli_status_verification_expector(client, update_ready_string(&client->vols[v], true, true, client->devs[v]->os->is_io_api_disabled), v);
			else            set_cli_status_verification_expector(client,     shutdown_string(&client->vols[v], true, true, client->devs[v]->os->is_io_api_disabled), v);
		}
	}
}

/*************************** Waiting For Event ********************************/
#include "main/nvmeibc_main_common.h"
void clientSimulator_wait_for_mainwq(struct clientSimulator *client) {
	if (client->is_nvmeibc_ko_up) {	// Else, client is rmmoded. No main-wq
		struct t_main_clnt_globals *mg = __get_from_params_main_globals_container(&client->p->main);
		wq_drain(mg->sched.main_wq);
	}
}

/* wait until all detach consequences are settled (i.e.: block devices are freed)
 * when detach complets, the block_device/request_queue/gendisk might still be alive due to IO's.
 * wait until they are deleted bcz our "struct block_device" is not allocated upon attach but rather reused within the ClientSimulator
 * BEWARE:This means we cannot test the case where the volume is attached & handles IO's while an older incarnation is still alive (we can do that on the Linux kernel)*/
void clientSimulator_wait_for_detach_drain(struct clientSimulator *client) {
	int		v;
	unsigned long 	start_time = jiffies, elapsed;
	for (v=0; v<client->nBdevs; v++) {
		if (client->devs[v] != NULL)
			continue;											// Irrelevant for detach
		if (client->vols[v].nextCmd == volCmds_DetachUpgrade)
			continue;											// By definition kernel is not aware of the detach!
		while (osSimulator_diskIsAttached(&client->OS, v)) {    // volume was detached. wait for ts gendisk & block_device to be freed
			sched_yield();
			rmb();
			elapsed = jiffies_to_msecs(jiffies - start_time);
			WARN(elapsed > 1000, "Detach vol %d, drain stuck for %lu msec\n", v, elapsed);
		}
	}
}

void clientSimulator_wait_for_all_topo_users_to_finish(struct nvmeibc_topologies *nt){
	int sum, i, n_iters;
	struct nvmeibc_topology *t = nvmeibc_topology_get(nt);
	for (n_iters=100000; n_iters>0; n_iters--) {					// Wait for up to 1[sec]
		sum = 0;
		for_each_possible_cpu(i)
			sum += t->percpu[i].t_users;
		if (sum != 1) {												// +1 ref we took a few lines ago
			udelay(10);
		} else
			break;
	}
	nvmeibc_topology_put(t);
	if (n_iters==0){
		struct nvmeibc_topology *stuck_topo = list_last_entry(&nt->topologies, struct nvmeibc_topology,list_n);
		pr_emerg("Error: %s(), did not converge on %p!!! System unpredicted behavior\n", __FUNCTION__, stuck_topo);
	}
}

static bool __are_syncs_still_running(struct nvmeibc_block_device* bdev, const bool do_assert) {
	int is_list_full, num_running, rv = false;
	spin_lock(&bdev->dp.resub.lock);
	is_list_full = !list_empty(&bdev->dp.sync_rsrcs.request_list);
	num_running = bdev->dp.sync_rsrcs.stats.num_running;
	if (is_list_full) {									// There are syncs that havent been launched yet. Very bad
		const int list_size = list_calc_size(&bdev->dp.sync_rsrcs.request_list);
		WARN(do_assert, "Error: %s: %d==%d syncs did not launch. %d running\n", bdev->name, list_size, bdev->dp.sync_rsrcs.stats.num_pending, num_running);
		rv = true;
	} else if (num_running) {
		if (do_assert) {
			pr_emerg("Error: %s: unitest did not drain all syncs. %d running\n", bdev->name, num_running);	// Dont crash, probably bug in unitest of not waiting properly
			WARN(do_assert, "Asserting to prevent sync from completing\n");
		}
		rv = true;
	}
	spin_unlock(&bdev->dp.resub.lock);
	return rv;
}

bool clientSimulator_is_stable(const struct clientSimulator *client) {
	int v;
	for (v=0; v<client->nBdevs; v++) {					// Verify no sync operations are executing
		struct nvmeibc_block_device* bdev = client->devs[v];
		struct nvmeibc_topologies* nt = &bdev->topologies;
		BUG_ON(!bdev || __are_syncs_still_running(bdev, true));
		if (!nvmeibc_block_is_recoverer(bdev))			// Non recoverer volumes must be known by the os
			BUG_ON(!client->OS.bds[v].bd_disk);

		clientSimulator_wait_for_all_topo_users_to_finish(nt);	// Verify topology is stable, Wait for the last changed, if any
		nvmeibc_topology_is_stable_noio(nt, true);
	}
	return true;	// If was not stable we would hit a BUG_ON()
}

/* Wait until client registers with toma on volume 'v' IO becomes enabled and
   client is in idel state.
   NOTE: If "Self leaking Tomatos" msg appear - probably the unitest
   scenario is wrong (you don't understand to which condition to wait). Do not
   supress the warning, unless you absolutely understand what you are doing and
   can justify it */
void clientSimulator_wait_for_io_enabled_for_vol(struct clientSimulator *client, int v, const bool supress_warning) {
	int i;
	const struct nvmeibc_topologies* nt = &client->devs[v]->topologies;
	for (i=1; i<=10; i++) {
		if (!nvmeibc_topology_is_stable_noio(nt, false)) {
			schedule(); // Should never happen, unless some timing bug in unitest...
			tomaSimulator_waitProtoEnd(NULL);
			if (!supress_warning) {
				pr_emerg("Self leaking %d Tomatos\n", i);
			} else
				_NT(t_00_simu_wait_io, "Self leaking @INT Tomatos", i);
		} else
			break;
	}	// Waited for ~ 110[msecs]
	nvmeibc_topology_is_stable_noio(nt, true); // Invoke bug!
}

void clientSimulator_wait_for_io_enabled_for_vol_non_idle(struct clientSimulator *client, int v) {
	const struct nvmeibc_topologies* nt = &client->devs[v]->topologies;
	int i;
	for (i = 200000; (!nvmeibc_topo_is_io_ok(nt))&&(i == 0); i--) {	// ~2[sec]
		schedule();
	}
	BUG_ON(i <= 0);
}

static void __unitest_on_io_enabled_cb(struct nvmeibc_topologies *nt, void *_ctx) {
	struct completion *cmp = _ctx;
	complete(cmp);
	_NT(t_a2_simu_wait_io, "vdio2: @DBG_NUM_ENABLING_IO_TOGGLES @IO_PERM", nt->dbg_num_enabling_io_toggles, nt->io_perm);
	nt->debug_on_io_enabled.cb = NULL;
}

void clientSimulator_wait_for_io_toggle_init(struct clientSimulator *client, int v) {
	struct nvmeibc_topologies* nt = &client->devs[v]->topologies;
	struct completion *cmp = sim_kmalloc(sizeof(*cmp), GFP_KERNEL);
	init_completion(cmp);
	_NT(t_a1_simu_wait_io, "vdio1: @DBG_NUM_ENABLING_IO_TOGGLES @IO_PERM", nt->dbg_num_enabling_io_toggles, nt->io_perm);
	nt->debug_on_io_enabled.cb =  __unitest_on_io_enabled_cb;
	nt->debug_on_io_enabled.ctx = cmp;
}
void clientSimulator_wait_for_io_toggle_wait(struct clientSimulator *client, int v){
	struct nvmeibc_topologies* nt = &client->devs[v]->topologies;
	struct completion *cmp = nt->debug_on_io_enabled.ctx;
	_NT(t_a3_simu_wait_io, "vdio3: @DBG_NUM_ENABLING_IO_TOGGLES @IO_PERM", nt->dbg_num_enabling_io_toggles, nt->io_perm);
	BUG_ON(wait_for_completion_interruptible_timeout(cmp, 3*HZ) <= 0);					// DHS: Arbitrary 3 seconds wait
	_NT(t_a4_simu_wait_io, "vdio4: @DBG_NUM_ENABLING_IO_TOGGLES @IO_PERM", nt->dbg_num_enabling_io_toggles, nt->io_perm);
	nt->debug_on_io_enabled.ctx = NULL;
	sim_kfree(cmp);
}

void clientSimulator_wait_for_single_topo_no_io(struct clientSimulator *client) {
	int v;
	for (v=0; v < client->nBdevs; v++) {
		struct nvmeibc_block_device	*bdev = client->devs[v];
		if (bdev)
			nvmeibc_topo_wait_for_single_topo_no_io(&bdev->topologies, 1);
	}
}
void clientSimulator_wait_for_all_sync_ops(struct clientSimulator *client) {
	int v, rounds = 10*client->nBdevs;								// 10 [msec] for each volume is way too much...
	for (v=0; (v<client->nBdevs)&&(rounds > 0); v++, rounds--) {
		struct nvmeibc_block_device* bdev = client->devs[v];
		if ((bdev)&&(__are_syncs_still_running(bdev, false))) {
			msleep(1);
			v--;						// Stay in the same volume
		}
	}
	WARN((rounds==0), "%s: Timed out, on volume %d\n", __FUNCTION__, v);
}

void clientSimulator_wait_for_all_recoveries_done(struct clientSimulator *client){
	int v, rounds = 1000*client->nBdevs;								// 10 [msec] for each volume is way too much...
	for (v=0; (v<client->nBdevs)&&(rounds > 0); v++, rounds--) {
		struct nvmeibc_block_device* bdev = client->devs[v];
		if ((bdev == NULL) || (0 == nvmeibc_recovs_drainer_get_num(&bdev->dp.running_recovs, NULL, NULL, NULL)))
			continue;
		usleep(10);
		schedule();
		v--;						// Stay in the same volume
	}
	WARN((rounds==0), "%s: Timed out, on volume %d\n", __FUNCTION__, v);
	_ND(tr_1_wait_for_all_recovs, "waiting for all recoveries done - done");
}

void clientSimulator_wait_for_all_io_resubmittion(struct clientSimulator *client, int volInd) {
	// extern const char* nvmeibc_resubmitter_name;
	int v, n_devs = volInd+1;
	if (volInd<0) {
		n_devs = client->nBdevs;
		volInd = 0;
	}
	for (v=volInd; v<n_devs; v++) {
		/*int i, max_wait = 1000;											// 1 secs is way too much enough
		for (i=0; (i<max_wait)&&(!RB_EMPTY_ROOT(&client->devs[v]->rb_paused_ops)); i++)
			msleep(1);
		if (!RB_EMPTY_ROOT(&client->devs[v]->rb_paused_ops)) {
			_Emerg("Probably bug in kernel simulator, No one woke-up resubmit thread of %s, but it has IO's awaiting resubmition\n", client->devs[v]->name);
			BUG_ON(true);
		}*/
	}
	//kthread_wait_for_paused(nvmeibc_resubmitter_name);
	// Todo: Think of better solution. This is not enough. Retry timer can resubmit operation and wake-up resubmition thread
}

void clientSimulator_wait_for_all_bio_ops(struct clientSimulator *client) {
	int n_ios, v;
	do { // Step 1: wait for all bio's to end. Todo: Change to non busy wait.
		n_ios = osSimulator_allert_pending_ios(&client->OS, 0);
	} while (n_ios);
	// Step 2: now wait for all objects (locks, sync, etc...) to end
	clientSimulator_wait_for_single_topo_no_io(client);
	for (v=0; v < client->nBdevs; v++) {	// Verify no syncs in the system (Why would there be syncs if there is no IO?)
		struct nvmeibc_block_device	*bdev = client->devs[v];
		if (bdev)
			__are_syncs_still_running(bdev, false);
	}
	// we now have a single topology with no reference to it so no IO objects alive in the system
}

int clientSimulator_get_volume_version(struct clientSimulator *client, int vol_index)
{
	return client->devs[vol_index]->volume->hdr.version;
}

/*****************************************************************************/
// EOF.
