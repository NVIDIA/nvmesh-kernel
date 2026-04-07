/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_VOLUME_H
#define NVMEIBC_VOLUME_H

#include "nvmeibc_block.h"
#include "main/cc_api/nvmeibc_main_capi_manipulate_vols.h"  // For nvmeibc_config_volume
#include "common/pet/nvmeib_pet_specification.h"

/* Realtime state of the volume. Typical flow:
   kmalloc -> NVS_ATTACHING -> NVS_ATTACHED -> NVS_UPDATING ->
   NVS_ATTACHED -> NVS_DETACHING -> NVS_DETACHING_IO_DRAINED -> kfree() */
enum nvmeibc_volume_status {					// Todo: Evntually remove this struct, it is not really needed. Encoded in detach statemahcine below
	NVS_ATTACHING_STARTED 	= 0, 				// Just now allocated a new volume. And in the process of creating it
	NVS_ATTACHING_HAVE_BDEV = 1, 				// In process of attaching volume, already have block device allocated which can respond to callbacks
	NVS_ATTACHED  			= 2, 				// Normal state of the volume. Ready to serve IO and to be dettached
	NVS_UPDATING			= 3,				// Changing the configuration of the volume (that was previously attached
	NVS_DETACHING			= 4,				// During the fraction of a second we are disconnecting the volume from the system. After this status the volume memory will be freed
	NVS_DETACHING_IO_DRAINED= 5,				// During the fraction of a second we are disconnecting the volume from the system. After this status the volume memory will be freed
};

static inline const char* nvmeibc_volume_status_str(const enum nvmeibc_volume_status s)
{
	switch (s) {
	case NVS_ATTACHING_STARTED:
		return "ATTACHING_STARTED";
	case NVS_ATTACHING_HAVE_BDEV:
		return "ATTACHING_HAVE_BDEV";
	case NVS_ATTACHED:
		return "ATTACHED";
	case NVS_UPDATING:
		return "UPDATING";
	case NVS_DETACHING:
		return "DETACHING";
	case NVS_DETACHING_IO_DRAINED:
		return "DETACHING_IO_DRAINED";
	default:
		return "UNKNOWN";
	}
}

struct nvmeibc_volume_detach_t {				// unsafe detach assist struct
	enum volume_detach_state_e {
		volume_detach_state_start,
		volume_detach_state_no_io,
		volume_detach_state_cleanup,
		volume_detach_state_error_retry,
		volume_detach_state_error_done,
		volume_detach_state_success_done,
	} state;									// State machine state
	int error;									// Error code of the last stage in state machine
	int num_retry_attempts;						// For debug: If unsafe detach fails, counts the number of attempts
	struct workqe_struct retry_work;			// Reschedule itself to retry unsafe detach
	struct nvmeibc_vol_detach_cmd cmd;			// Detach parameters from request (shutdown/force/err/hidden/abandon(upgrade))
	void (*handler)(void *ctx);					// Async state machine method
	struct nvmeibc_os_api *os;
};

/* nvme_over_ib volume. Much like  (C++ polymorphism), supports different volume
   types (controlled by type). */
struct nvmeibc_cinst_params_main;
struct nvmeibc_volume {
	/* -------------- volume base class code -------------- */
	// TODO: remove dependency on this old structure - or rename it to reflect it's current use as a a holder of various descriptors
	struct nvmeibc_volume_header hdr; /* Configuration header */

	/*name of the client concatenated with the name of the device*/
	char full_name[NVMEIB_HOST_NAME_LEN + NVMEIBC_BD_NAME_LEN + 2];		// Example: "n118.acme.com_mc0003-vol_ec"  +2 for ('-','\0'). Dont confuse with hdr.dev_name
	const struct nvmeibc_cinst_params_main *p;						// To whihc client instance this volume belongs

	/* volume guard and status to handle properly volume creation and offline
	   disks. Methods (disk PAUSE/CONT) must prevents status change so they
	   hold the spinlock, through entire execution. Each status update must
	   acquire spinlock as well. 'status', 'block_dev', 'info' are accessed via disk workqueue as well for pasue/cont */
	spinlock_t spinlock;
	enum nvmeibc_volume_status status;
	struct nvmeibc_multi_completion *job_comp;	// When asked to detach all volumes mark here when the job finishes
	struct nvmeibc_volume_detach_t detach;		// Assist struct for performing force detach (like unsafe usb removal)
	struct list_head link;						// Link List of volumes
	struct nvmeibc_volume_info info;		// a volume configuration info
	/* -------------- Polymorphic code -------------- */
	struct nvmeibc_block_device  *block_dev;	// The default block device interface
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	struct proc_dir_entry *disks_dir;			// The volumes /proc/.../vol_name/disks/ directory, where disk proc folders and files will reside
#endif
	struct nvmeib_pet_base_controller* io_pet_controller;
};

/********************** Generic API of All Volumes ***************************/
/* Attach a volume, or update an existing volume. Receives MCS message that gave
   this command and for backwards compatibility config-fs entry.
   To be used by main.c, only in main workqueue! */

struct nvmeib_pet_base_controller;
int nvmeibc_volume_attach(const struct nvmeibc_cinst_params_main *p,
	const struct nvmeib_mgmt_to_client_volume_configuration *msg,
	struct nvmeib_pet_base_controller* io_pet_controller);

/* Polymorphic for any type of volume, can be called during attach.
   returns 0 - asyncronous detach started (not busy), or negative error code if
   detach could not be started (busy or any othe problem).
   if 'force' flag is set - starts unsafe detach immediately regardless of
   business */
int nvmeibc_volume_try_detach(struct nvmeibc_volume *volume,
							  const struct nvmeibc_vol_detach_cmd how,
							  struct nvmeibc_multi_completion *on_finish);

/* Mainly for debug. Find a volume by its name/uuid and type, Setting type to
   UNKNOWN will result in longer search*/
struct nvmeibc_volume *nvmeibc_volume_get_by_name(const struct nvmeibc_cinst_params_main *p,
		const char *name, enum nvmeibc_config_volume_type type);
struct nvmeibc_volume *nvmeibc_volume_get_by_uuid(const struct nvmeibc_cinst_params_main *p,
		const char *uuid, enum nvmeibc_config_volume_type type);

/* locks the spinlock to external exclusive operations on the volume */
void nvmeibc_volume_get(struct nvmeibc_volume *volume, unsigned long *flags);
void nvmeibc_volume_put(struct nvmeibc_volume *volume, unsigned long *flags);
/* Pass realtime ioctl commands to the volume */
int  nvmeibc_volume_ioctl_config(const struct nvmeibc_cinst_params_main *p, const char* cmd /*, int len*/);
int  nvmeibc_volume_get_max_global_attach_version_ever_seen(const struct nvmeibc_cinst_params_main *p);

/* Update/Create volume header from MCS message */
void nvmeibc_volume_header_create_from_msg(struct nvmeibc_volume_header *hdr,
										const struct nvmeibc_volume_conf *conf,
										int attachment_version, bool verbose);

static inline const struct nvmeibc_volume_attach_t *nvmeibc_volume_get_attach_t(const struct nvmeibc_volume* v) {
	return &v->hdr.vat;
}

ulong nvmeibc_volume_get_size(const struct nvmeibc_volume* v);

/* Returns 1 if ready, 0 if not but will be soon, -1 if will never be ready */
int nvmeibc_volume_is_ready_for_pause_cont(const struct nvmeibc_volume* v);

/* Insert to volume the dev which implements it. */
void nvmeibc_volume_set_block_device(struct nvmeibc_volume *volume, struct nvmeibc_block_device *dev);

/* When block device starts/stops using a segment on disk it calls
   this method. context contains nvmeibc_disk_id_update_params uses
   is_attach - means we started to use segment, otherwise stopped*/
void nvmeibc_volume_update_volume_single_segment(void *context, const struct nvmeibc_cinst_params_main *p);

int nvmeibc_volume_get_cpu_masks(struct nvmeibc_volume *volume, struct nvmeib_cpu_mask_info *mask_infos, int max_masks);

#define PROCFS_DISKS_STR "disks"
int  nvmeibc_volume_disk_stats_create(struct proc_dir_entry *vol_dir, struct nvmeibc_volume *volume);
void nvmeibc_volume_disks_stats_destroy(struct nvmeibc_volume *volume);
void nvmeibc_volume_disks_stats_clear(const struct nvmeibc_volume *volume, const int which);
/*
 * For debug: dump (to log) all disks that are being used by the volume
 */
void nvmeibc_volume_dump_disk_ids(struct nvmeibc_volume *vol);

void nvmeibc_volume_trace_stats(const struct nvmeibc_volume *volume);

struct nvmeib_txt;
void nvmeibc_volume_to_text(const struct nvmeibc_volume *volume, struct nvmeib_txt* txt);

struct jdr;
void nvmeibc_volume_to_json(const struct nvmeibc_volume *volume, struct jdr* jdr);

/* Calls call_fn for all volume disks. Stops if call_fn returns < 0
 * NOTE: volume spinlock is held while calling call_fn.
 * Returns: call_fn return code if stopped otherwise number of disks called.
 */
int nvmeibc_volume_call_for_all_vol_disks(const struct nvmeibc_volume *volume, int (*call_fn)(struct nvmeibc_idisk *disk, void *ctx), void *ctx);

#endif
