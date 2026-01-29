/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MAIN_COMMON_H
#define NVMEIBC_MAIN_COMMON_H

/************************* Scheduling mechanism of main ***********************/
struct t_main_clnt_sched {							// Mechanism of module/main layer for scheduling tasks
	struct workq_struct *main_wq;					// the main workqueue - everything starts here
	char main_wq_name[24 /*WQ_NAME_LEN*/];			// Must be very short to fin in privetly defined WQ_NAME_LEN
	int main_wq_pid;
	atomic_t main_wq_submit_use_count;				// if 0 means that main_wq is going to (or has been) terminate
	struct completion main_wq_stop_use_completed;	// Completes when indeed no new tasks can arrive to wq. Only a single drain is required
};
int  t_main_clnt_sched_create( struct t_main_clnt_sched *, const char *name, u16 cinst_index);
void t_main_clnt_sched_disable(struct t_main_clnt_sched *);	// Prevent new jobs from arriving tom main-wq and drain existing jobs, object itself remains to auto-fail incomming tasks
void t_main_clnt_sched_destroy(struct t_main_clnt_sched *);	// Remove the sched object. Beforre calling this function must disable all object which may attempt to schedule tasks
bool t_main_clnt_sched_on_main_wq(const struct t_main_clnt_sched *s, bool do_assert);
int  t_main_clnt_sched_add_work(struct t_main_clnt_sched *s, struct workqe_struct *work);
bool t_main_clnt_sched_cancel_work(struct t_main_clnt_sched *s, struct workqe_struct *work);

/*************** Internal Variables of main layer per instance ****************/
struct nvmeibc_cinst_params_main;
struct t_main_clnt_globals {
	struct t_main_clnt_communication {	// Communication with server/mcs/mgmt/etc
		char full_name[__NEW_UTS_LEN + 8 /*CINST_NAME_LEN*/ + 2];	// Daniel: Todo, define this more precisely
		uuid_be uuid;					// Daniel: Probably need to move out of here into core params. Not persistent accross system restart. Needed for discovery and journal per client allocation
	} com;

	struct t_main_clnt_proc_dir {	/* globals: our root @ the procfs */
		const char *root_name;		// Root directory of this client instance (Daniel: Not allocated to force it to be identical to what appears in /proc/devices)
		struct proc_dir_entry *root;
		struct proc_dir_entry *disks;
		struct proc_dir_entry *volumes;
		struct proc_dir_entry *net;
		struct proc_dir_entry *jam;
		struct t_main_clnt_proc_files {		// Files in root directory of each instance
			struct nvmeib_public_procfs_ent *rsrc_info;
			struct nvmeib_public_procfs_ent *status;
			struct nvmeib_public_procfs_ent *statusjson;
			struct nvmeib_public_procfs_ent *dot_debug;
			struct nvmeib_public_procfs_ent *shared_cq;
			struct nvmeib_public_procfs_ent *nics_json;
			struct nvmeib_public_procfs_ent *memmgr_info;
			struct nvmeib_public_procfs_ent *error_tags_info;
			struct nvmeib_public_procfs_ent *cpu_masks_json;
		} files;
	} proc_dir;
	struct t_main_clnt_vols {
		/* List of volume (volumes) that are currently attached. The list is only
		   accessed in the context of the main work queue so no spinlocks needed.*/
		struct list_head volumes;
		struct list_head mtvolumes;
		int num_thik_volumes;				// Num elements in the lists. For debug
		int num_mult_volumes;
	} vols;
	struct t_main_clnt_disks {
		struct list_head list;				//  List of disksthat are used by currently attached volumes. The list is only accessed in the context of the main work queue.
	} disks;
	struct nvmeibc_control_api cc_api;		// MCS/CLI api's
	struct targets_global *targets;
	const struct nvmeibc_cinst_params_main *p;	// Reference to the params of client instance.
	struct t_main_clnt_sched sched;
	struct t_main_clnt_priv_sched {						// Mechanisms for each instance to schedule its tasks
		enum nvmeibc_inst_state {						// Identical values and meanings to enum nvmeibc_mod_state
			NVMEIBC_INST_STATE_INITIALIZING = 0,		// Durining creation state is now initializing
			NVMEIBC_INST_STATE_READY,					// Instance completed initialization of all layers (main/block/core) & is operational
			NVMEIBC_INST_STATE_PREP_RM,					// Instance is preparing for removal, cannot attach new volumes.
			NVMEIBC_INST_STATE_RM_RDY,					// No volumes attached. Empty instance remains, waiting for destruction
			NVMEIBC_INST_STATE_EXITING,					// Instance does not have any more volumes. It is being destroyed
		} state;
		struct mutex is_ready_for_attaches;				// Serializes init module / destroy module / attach. Starts as locked and becomes unlock when scheduler can perform volume manipulations
	} priv_sched;
};

struct nvmeibc_shared_cq_info {
	struct t_main_clnt_globals *_mg;
	char *buffer;
	size_t len;
	size_t count;
	struct completion *comp;
};

int  t_main_clnt_globals_create( const struct nvmeibc_cinst_params_main *p);
void t_main_clnt_globals_destroy(const struct nvmeibc_cinst_params_main *p);
int  t_main_clnt_globals_params_update(struct nvmeibc_cinst_params_main *p, const char *ioctl);	// Respond to ioctl which updates params
void t_main_clnt_globals_params_free(struct nvmeibc_cinst_params_main *p);

struct t_main_clnt_globals * __get_from_params_main_globals_container(const struct nvmeibc_cinst_params_main *p);
#define __get_cinst_params_from_cc_api(c) \
	((const struct nvmeibc_cinst_params_main *)(container_of(c, struct t_main_clnt_globals, cc_api)->p))

#endif
