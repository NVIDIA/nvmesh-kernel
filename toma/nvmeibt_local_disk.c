/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/* Local disks driverflow documentation:

   Local disks in the system can be bound to one of 2 drivers:
   1. nvmeibs driver.
   2. stock linux nvme driver.

   In order for a disk to be usable by nvmesh, it must be:
   1. bound to nvmeibs driver.
   2. formatted and preconditioned by TOMA.

   NORMAL DRIVER FLOW:

   When TOMA starts (or gets an event about disk plug) it will:
   1. If the disk is bound to stock nvme driver:
		If the disk is excluded - do nothing.

		If the disk is formatted without metadata:
			Read the disk GPT and decide whether the disk needs to be moved to
			nvmeibs driver, according to the contents of the GPT.
		Else: (the disk is formatted with metadata):
			Bind the disk to nvmeibs driver.
    2. Else: (disk is bound to nvmeibs driver)
		If the disk is excluded:
			Bind it back to the stock nvme driver
		Else:
			Read the contents of the drive and either resume format
			if it was in the middle of a format, or completely read
			the GPT and report it to management.

   REPORT TO MGMT
   All the drives, including their smart info and their GPT, and whether
    excluded, are reported to the MGMT.

   FORMAT FLOW:
   A format command for a given disk arrives from the management, and includes
   the parameters with which the disk should be formatted. (those parameters
   were previously reported to the MGMT, by TOMA)

   Formatting a drive:
   The format flow recieves a format command from the MGMT, which includes
   the disk parameters, format parameters and a format counter.
   The format counter is used to distinguish between different format commands
   from the management, from network hicups and messages coming out of order.

   If a disk is during a format already, and a newer format command arrives, it
   will be queued, and the disk is marked with "abort format", when the format
   aborts the new format will start. (if multiple format commands arrive while
   a disk is formatting only the newest command will be stored and executed)

   Formatting a disk does the following:
   1. Checks that the disk is bound to nvmeibs driver, and if not attempts to bind it.
   2. Issues a freeze command on the disk

      The freeze command is received by TOMA as a server event, which signals a disk as "Frozen",
      this requires TOMA to stop using the disk, and unmap all it's mapped memory. When all
      the memory is unmapped the server "freeze" operation returns and the python script
      continues the format.
    3. Issues a nvme-format command, according to the format parameters.
    4. Write a signature to block 0 of the disk indicating it was formatted, and the format
       parameters.
    5. Unfreezes the disk.


    When the disk is unfrozen TOMA reads the contents of the disk, and finds the format
    signature there which tells it that the disk was formatter and that it can continue
    the disk preconditioning process.

    At this point TOMA starts zeroing the disk from the 2nd block and up to a little before
    the end of the disk (a little is enough to write a GPT table at the end of the disk).

    The zeroing is done in iterations, each around 0.1% of the total disk size. Once TOMA zeroes
    at least 0.5% of the disk it allocates the GPT on the disk, and the disk can be used
    to allocate segments on it. (if a segment is allocated on space which was not yet zeroed
    it won't become activated until it's entire range is zeroed).

    Zeroing can fail in the middle, and TOMA will resume it, up to from where it left
    before the crash. (it stores checkpoints after every 0.1% it zeroes)


    Disk Statuses as reported by TOMA to MGMT:

    *   Status         			Meaning
    *
    *	Not Initialized			The disk is not formatted, but can be formatted to be used by nvmesh
    *	Ingesting 				The disk is in the process of being moved from the stock nvme driver
								to the nvmeibs driver. (won't appear almost never since this is very fast)
    *	Frozen 					The disk is to disallow TOMA any access to it (and clients) and is
								in initial format stages. (This might go away eventually)
    *	Formatting 				Disk is during formatting stage, and before the GPT was created on it.
								This will be reported before 0.5% of the disk was zeroed.
    *	Initializing 			Disk completed formatting and is now during zeroing stage, the disk can
								be used, but some segments may take a while to become online (only when
								their blocks will be zeroed). This will be repoted after more than 0.5%
								of the disk were zeroed.
	*	Ok 						Disk is completely formatted and zeroed.
	*	Error 					Some error occurred during the initialization of the disk. (i.e. reading GPT/MBR)
    *	Format_Error 			An error occurred during disk format, either in the disk, or in our
								nvmeibs driver.
    *	Excluded 				This disk is excluded and cannot be formatted.
    *
   */

#include <sys/types.h>
#include <linux/types.h>
#include <sys/types.h>
#include <unistd.h>

#include "nvmeibt_read_config.h"
#include <mntent.h>
#include <sys/ioctl.h>
#include <linux/fs.h>
#include <sys/stat.h>
#include "interfaces/nvme/nvmeibt_nvme_defines.h"
#include "nvmeibt_local_disk.h"
#include "nvmeibt_disk.h"
#include "nvmeibt_disk_segment.h"
#include "nvmeibt_local_disk_util.h"
#include "nvmeibt_ds_metadata.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_wq.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_global.h"
#include "nvmeibt_read_config.h"
#include "nvmeibt_persistency_info.h"
#include "nvmeibt_kafka.h"

#define LOCAL_DISK_SMART_PROBE_UPDATE_THRESHOLD_SEC		300

struct fill_local_disk_from_stock_driver_wq_entry {
	struct nvmeibt_wq_entry 		wq_entry;
	struct nvmeibt_udev_event_info  *udev_event_info;
	struct nvmeibt_local_disk		*new_local_disk; /* Both input + output */
	int								rv;
};

struct periodic_reread_smart_counters_wq_entry {
	struct nvmeibt_wq_entry 					wq_entry;
	int											fd;
	BOOL										is_stock_disk;
    struct nvmeibt_ascii_uuid					ldisk_id;
	char										ld_display[100];
	u32 										vendor_id;
    struct nvme_smart_log 						smart_log;
	int 										rv;
};

struct local_disk_format_wq_entry {
	struct nvmeibt_wq_entry 		wq_entry;
	int 							fd;
	char							ld_display[100];
	int								seq;
	struct format_details			format_details;
	int 							rv;
	char 							*bdf;
	unsigned int					old_blocksize;
	BOOL							rescan_after_format;
};

struct local_disk_bind_wq_entry {
	struct nvmeibt_wq_entry 		wq_entry;
	struct nvmeibt_ascii_uuid		ldisk_id;
	struct nvmeibt_udev_event_info  *udev_event_info;
	char 							bdf[NVMEIBS_DISKS_CSV_STATUS_LEN];
	char							dev_file_name[PATH_MAX];
	char							serial[DISK_NAME_LEN];
	char							ld_display[100];
	char							model[NVMEIB_DISK_MAX_MODEL_STR_SIZE];
	u16								vendor;
	int 							is_stock_to_nvmeibs;
	int 							rv;
	bool							is_auto_takeover;
	int								dev_file_fd;
	int								seq;
	struct nvmeibt_local_disk_controller	*controller_for_sanity_checks;
};

/******************************     Controller     ****************************/
// A controller_id it native_serial. It has a list of local_disk objects (deleted when empty)
// It can change state NVMEIBS<-->STOCK

nvmeibt_local_disk_controllers_list_t		all_controllers_list;

char *controller_state_str(enum NVMEIBT_LOCAL_DISK_CONTROLLER_STATE controller_state) {
	switch (controller_state) {
	case NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNDEFINED:						return "UNDEFINED";
	case NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK:							return "STOCK";
	case NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNBOUND_STOCK_TO_NVMEIBS:		return "UNBOUND_STOCK_TO_NVMEIBS";
	case NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS:						return "NVMEIBS";
	case NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNBOUND_NVMEIBS_TO_STOCK:		return "UNBOUND_NVMEIBS_TO_STOCK";
	default:																return "Unknown";
	}
}

static struct nvmeibt_local_disk_controller *controller_get_by_native_serial(char *native_serial_str)
{
	struct nvmeibt_local_disk_controller	*controller;
	struct nvmeibt_local_disk_controller	*out_controller = NULL;

	XDLIST_FOREACH(controller, &all_controllers_list) {
		if (strncmp(controller->native_serial_str, native_serial_str, sizeof(controller->native_serial_str)) == 0) {
			out_controller = controller;
			break;
		}
	}
	if (out_controller) {
		N_Tf(vwy738w, "controller=@STR state=@STR", out_controller->native_serial_str, controller_state_str(out_controller->controller_state));
	} else {
		N_Tf(cvajwjk, "Not found");
	}
	return out_controller;
}

static void controller_mark_is_bind_to_nvmeibs_needed(struct nvmeibt_local_disk_controller *controller)
{
	if (controller) {
		controller->is_bind_to_nvmeibs_needed = 1;
	}
}

static void controller_mark_is_bind_back_to_stock_needed(struct nvmeibt_local_disk_controller *controller)
{
	if (controller) {
		controller->is_bind_back_to_stock_needed = 1;
	}
}

void nvmeibt_local_disk_propagate_needed_bind_and_excluded_and_takeover_to_controller_local_disks(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_local_disk_controller	*controller;
	struct nvmeibt_local_disk 				*tmp_local_disk;
	bool									is_controller_modified;

	NFIN;
	// Any local_disk. nvmeibs/stock
	controller = local_disk->controller;
	if (!controller) {
		N_Ef(y7u8iqq, "local_disk=@STR controller=NULL", nvmeibt_local_disk_display(local_disk));
		sprintf(local_disk->from_config.status, "Error");
		goto out;
	}
	if (local_disk->is_excluded) {
		if (!(controller->is_excluded)) {
			controller->is_excluded = 1;
			is_controller_modified = 1;
		}
	} else {
		local_disk->is_excluded = controller->is_excluded;
	}
	if (local_disk->is_auto_takeover) {
		if (!(controller->is_auto_takeover)) {
			controller->is_auto_takeover = 1;
			is_controller_modified = 1;
		}
	} else {
		local_disk->is_auto_takeover = controller->is_auto_takeover;
	}
	if (nvmeibt_local_disk_is_bind_to_nvmeibs_needed(local_disk)) {
		if (!(controller->is_bind_to_nvmeibs_needed)) {
			controller_mark_is_bind_to_nvmeibs_needed(controller);
			is_controller_modified = 1;
		}
	} else {
		local_disk->is_bind_to_nvmeibs_needed = (local_disk->is_binding_to_nvmeibs ? 0 : controller->is_bind_to_nvmeibs_needed);
	}
	if (nvmeibt_local_disk_is_bind_back_to_stock_needed(local_disk)) {
		if (!(controller->is_bind_back_to_stock_needed)) {
			controller_mark_is_bind_back_to_stock_needed(controller);
			is_controller_modified = 1;
		}
	} else {
		local_disk->is_bind_back_to_stock_needed = (local_disk->is_binding_back_to_stock ? 0 : controller->is_bind_back_to_stock_needed);
	}
	if (!is_controller_modified) {
		goto out;
	}
	// is_controller_modified.
	// validate
	TODO(validate more, also depending on local_disk->is_owned_by_nvmeibs_driver);
	if (	((controller->is_excluded && controller->is_bind_to_nvmeibs_needed) ||
			 (controller->is_bind_to_nvmeibs_needed && controller->is_bind_back_to_stock_needed))) {
		N_Ef(du5vnis, "controller=@STR is_excluded=@BOOL is_bind_to_nvmeibs_needed=@BOOL is_bind_back_to_stock_needed=@BOOL",
			 controller->native_serial_str, controller->is_excluded, controller->is_bind_to_nvmeibs_needed, controller->is_bind_back_to_stock_needed);
	}
	// Propagate to all its local_disks
	XDLIST_FOREACH_SAFE(tmp_local_disk, &(controller->local_disks_list)) {
		if (controller->is_excluded) {
			tmp_local_disk->is_excluded = 1;
		}
		if (	(controller->is_bind_to_nvmeibs_needed &&
				 !nvmeibt_local_disk_is_binding_to_nvmeibs(tmp_local_disk) &&
				 !(controller->is_excluded))) {
			if (!(tmp_local_disk->is_owned_by_nvmeibs_driver)) {
				nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(tmp_local_disk);
			}
		}
		if (	(controller->is_bind_back_to_stock_needed &&
				  !nvmeibt_local_disk_is_binding_back_to_stock(tmp_local_disk)) ||
				 controller->is_excluded) {
			if (tmp_local_disk->is_owned_by_nvmeibs_driver) {
				nvmeibt_local_disk_mark_is_bind_back_to_stock_needed(tmp_local_disk);
			}
		}
	}
out:
	NFOUT;
}

static void controller_del_local_disk(char *native_serial_str, struct nvmeibt_local_disk *local_disk, bool is_from_nvmeibs)
{
	struct nvmeibt_local_disk_controller	*controller;

	NFIN;
	if (!local_disk) {
		N_Wf(cjhja32, "local_disk=NULL native_serial=@STR", native_serial_str);
		goto out;
	}
	if (!(local_disk->controller)) {
		N_Tf(i39mn2k, "local_disk=@STR controller=NULL. Skipping", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	controller = controller_get_by_native_serial(native_serial_str);
	if (!controller) {
		N_Ef(ntiu2vl, "controller=NULL");
		goto out;
	}
	if (local_disk->controller != controller) {
		N_Ef(vvghsjw, "local_disk=@STR->controller=@PTR", nvmeibt_local_disk_display(local_disk), controller);
		goto out;
	}
	N_Tf(72bxwyo, "controller=@STR state=@STR", controller->native_serial_str, controller_state_str(controller->controller_state));
	if (	((is_from_nvmeibs && (controller->controller_state == NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS)) ||
			 (!is_from_nvmeibs && (controller->controller_state == NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK)))) {
		N_Tf(df4mxwi, "is_from_nvmeibs=@BOOL state=@STR. Probably not unbinded earlier", is_from_nvmeibs, controller_state_str(controller->controller_state));
	}
	XDLIST_DEL(&(local_disk->controller_local_disks_list_link));
	if (XDLIST_N_ELEMNTS(&(controller->local_disks_list)) <= 0) {
		XDLIST_DEL(&(controller->all_controllers_list_link));
		NNVMEIBT_TOMA_FREE(6znhetc, controller);
	}
	local_disk->controller = NULL;
out:
	NFOUT;
}

static struct nvmeibt_local_disk_controller *controller_add_local_disk(char *native_serial_str, struct nvmeibt_local_disk *local_disk, enum NVMEIBT_LOCAL_DISK_CONTROLLER_STATE req_controller_state)
{
	struct nvmeibt_local_disk_controller	*controller;

	NFIN;
	controller = controller_get_by_native_serial(native_serial_str);
	if (controller) {
		if (req_controller_state != controller->controller_state) {
			N_Wf(chauipu, "controller=@STR req_state=@STR != old_state=@STR",
				 controller->native_serial_str, controller_state_str(req_controller_state), controller_state_str(controller->controller_state));
		}
		if (local_disk->controller == controller) {
			N_Tf(duan4cx, "local_disk=@STR already controller=@STR", nvmeibt_local_disk_display(local_disk), controller->native_serial_str);
			controller->controller_state = req_controller_state;
			goto out;
		} else if (!(local_disk->controller)) {
			N_Tf(sfomrb5, "local_disk=@STR controller=NULL", nvmeibt_local_disk_display(local_disk));
		} else {
			N_Ef(c4gja0k, "local_disk=@STR local_disk->controller=@PTR != controller=@PTR", nvmeibt_local_disk_display(local_disk), local_disk->controller, controller);
			XDLIST_DEL(&(local_disk->controller_local_disks_list_link));	// Should never happen, but the old one is definetely wrong
		}
	} else {
		controller = NNVMEIBT_TOMA_CALLOC(vajqjk3, 1, sizeof(*controller));
		nvmeibt_strlcpy(controller->native_serial_str, native_serial_str, sizeof(controller->native_serial_str));
		XDLIST_INIT_LINK(&(controller->all_controllers_list_link), NULL);
		XDLIST_HEAD_INIT(&(controller->local_disks_list));
		XDLIST_ADD_TAIL(&all_controllers_list, controller);
	}
	local_disk->controller = controller;
	XDLIST_ADD_TAIL(&(controller->local_disks_list), local_disk);
	nvmeibt_local_disk_propagate_needed_bind_and_excluded_and_takeover_to_controller_local_disks(local_disk);
	controller->controller_state = req_controller_state;
	N_Tf(vraj8o1, "local_disk=@STR controller=@STR state=@STR", nvmeibt_local_disk_display(local_disk), controller->native_serial_str, controller_state_str(controller->controller_state));
out:
	NFOUT;
	return controller;
}

/******************************************************************************/

static bool disk_read_cnt_test = false;
bool is_periodic_smart_polling_enabled = DISABLE_PERIODIC_SMART_POLLING_DEFAULT;

void nvmeibt_local_disk_set_is_periodic_smart_polling_enabled(bool val, bool print_me)
{
	is_periodic_smart_polling_enabled = val;
	if (print_me)
		N_Tf(rvghsds, "is_periodic_smart_polling_enabled=@BOOL", is_periodic_smart_polling_enabled);
}
void nvmeibt_local_disk_set_disable_periodic_smart_polling(int64_t val)
{
	nvmeibt_local_disk_set_is_periodic_smart_polling_enabled(!val, true);	// The API is unfortunately "disable"
}

int nvmeibt_local_disk_one_time_init(void) {
	const char *str = nvmeibt_global_nvmesh_conf_get_val_by_key("TOMA_CLOUD_MODE");
	if (str) {
		const bool not_cloud = strcasecmp(str, "Yes") && strcasecmp(str, "True") && strcmp(str, "1");
		nvmeibt_local_disk_set_is_periodic_smart_polling_enabled(not_cloud, true);
	}
	XDLIST_HEAD_INIT(&all_controllers_list);
	return 0;
}

u16 nvmeibt_local_disk_vendor_id(const struct nvmeibt_local_disk *local_disk)
{
	return (!local_disk ? -1 : (u16)(local_disk->from_config.vendor));
}

int nvmeibt_local_disk_dev_file_fd(const struct nvmeibt_local_disk *local_disk)
{
		return (local_disk ? local_disk->dev_file_fd : -1);
}

struct netlink_io_context *nvmeibt_local_disk_dev_nl_ctx(const struct nvmeibt_local_disk *local_disk)
{
		return (local_disk ? local_disk->dev_nl_ctx : NULL);
}

const struct nvmeibt_ascii_uuid *nvmeibt_local_disk_UUID(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? &local_disk->from_config.ldisk_id : NULL);
}

static inline void generate_local_disk_config_display(struct nvmeibt_local_disk_config *config)
{
	if (likely(config)) {
		if (unlikely(config->ld_display[0] == '\0')) {
			snprintf(config->ld_display, sizeof(config->ld_display), "%.36s(%.24s.%d)", config->ldisk_id.str, config->native_serial.str, config->nsid);
			N_Tf(cbsjkl2, "@STR", config->ld_display);
		}
	}
}

const char *nvmeibt_local_disk_config_display(const struct nvmeibt_local_disk_config *config)
{
	generate_local_disk_config_display((struct nvmeibt_local_disk_config *)config);
	return (config ? config->ld_display : "???");
}

const char *nvmeibt_local_disk_display(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? nvmeibt_local_disk_config_display(&(local_disk->from_config)) : "???");
}

const char* nvmeibt_local_disk_file_name(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk ? local_disk->from_config.dev_file_name : "???");
}

void nvmeibt_local_disk_dump(__attribute__((__unused__)) const struct nvmeibt_local_disk *local_disk)
{
#ifdef TOMA_DEBUG
	const struct nvmeibt_local_disk_config *f = &local_disk->from_config;
	N_Tf(7bstrg1, "Config data: disk=@STR vendor=@VENDOR n_pblk=@LLU pblk_size=@PBLK_SIZE max_request_size=@INT, seq=@INT, dev_name=@DEV_NAME metadata=@INT status=@STATUS_STR",
		 nvmeibt_local_disk_config_display(f), f->vendor, (unsigned long long)f->n_pblk, f->pblk_size, f->max_request_size, f->seq, f->dev_file_name,
		 f->metadata_n_bytes, f->status);
#endif
}

BOOL nvmeibt_local_disk_is_md_supported(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk && (local_disk->from_config.metadata_n_bytes != 0));
}

static int mmap_locks_table(struct nvmeibt_local_disk *local_disk)
{
	const char *disk_name = nvmeibt_local_disk_display(local_disk);
	const uint64_t n_4k_blocks = DIV_ROUND_UP(local_disk->from_config.n_pblk * local_disk->from_config.pblk_size, 4096);
	const uint64_t n_blksets = DIV_ROUND_UP(n_4k_blocks, NUM_4KBLKS_IN_BLKSET);
	off_t offset = 0;				// For simplicity: Toma maps the entire disk locks table, even if it includes jbods segments or unsed areas.
	if (local_disk->mmap_disk_locks_tbl.addr) {
		N_Tf(sh3ifu6, "disk=@STR locks table is already mmaped", disk_name);
		return 0;
	} else {
		struct mmap_tbl mtbl;
		N_Tf(sh3ifu7, "disk=@STR mmap at offset=@OFFSET_INT n_blksets=@UINT64_TX n_4k_blocks=@UINT64_TX", disk_name, offset, n_blksets, n_4k_blocks);
		mtbl = nvmeib_srvr_api_lib_locks_map_get(nvmeibt_local_disk_UUID_str(local_disk), n_blksets, offset, true);
		if (mtbl.addr) {
			local_disk->mmap_disk_locks_tbl = mtbl;
			N_Tf(ca8ak20, "mmap locks table of disk=@STR: addr @ADDR_PTR, length @LENGTH_LONG ", disk_name, mtbl.addr, mtbl.length);
			return 0;
		} // else, dont touch local_disk->mmap_disk_locks_tbl.
		return -1;
	}
}

static int nvmeibt_local_disk_munmap_mem_tbls(struct nvmeibt_local_disk *local_disk)
{
	int					rv = 0;
	int					i;
	struct nvmeibt_disk	*disk;

	NFIN;

	disk = NNVMEIBT_LOCAL_DISK_GET_DISK(trace_local_disk_nvmeibt_local_disk_munmap_mem_tbls, local_disk);
	if (disk) {
		for (i = 0; i < disk->n_segments; i++) {
			nvmeibt_seg_active_del_ptr_to_locks_tbl(nvmeibt_disk_segment_get_seg_active(disk->disk_segments[i]));
		}
	}

	N_Tf(cg672k9, "munmap locks table of disk=@STR", nvmeibt_local_disk_display(local_disk));
	if (!local_disk->mmap_disk_locks_tbl.addr) {
		N_Tf(xvq7i29, "disk=@STR locks table is NOT mmaped", nvmeibt_local_disk_display(local_disk));
	} else {
		rv = nvmeib_srvr_api_lib_locks_map_put(nvmeibt_local_disk_UUID_str(local_disk), local_disk->mmap_disk_locks_tbl);
	}
	local_disk->mmap_disk_locks_tbl.addr = NULL;
	local_disk->mmap_disk_locks_tbl.length = -1;
	NFOUT;
	return rv;
}

static int nvmeibt_local_disk_destroy_and_free(struct nvmeibt_local_disk *local_disk)
{
	int rv = 0;
	if (nvmeibt_local_disk_munmap_mem_tbls(local_disk) < 0) {
		N_Tf(vzyhq03, "failed to munmap mem tbls for disk=@STR (@AUTO_ERRNO)", nvmeibt_local_disk_display(local_disk));
		rv = -1;
	}
	NNVMEIBT_BM_FREE(hu86aw4,   local_disk->dev_nl_ctx);
	NNVMEIBT_CLOSE(hu86aw5,     local_disk->dev_file_fd);
	NNVMEIBT_TOMA_FREE(hu86aw6, local_disk);
	return rv;
}

// disk_remove() is called when no thread/registrant is accessing the disk
static void local_disk_remove_from_nvmeibs(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_disk				*disk;
	int 							fd = -1;
	struct nvmeibt_topology			*cur_topo = nvmeibt_global_get_global();

	NFIN;

	N_IMf(idwoam4, "Removing local_disk=@STR", nvmeibt_local_disk_display(local_disk));

	nvmeibt_local_disk_stop_wq(local_disk);

	// remove the workqueue from the hash, as we are guaranteed that no one will be using it

	NNVMEIBT_HASH_DEL_OBJ_ASCII_new(hu86aw3, cur_topo->nvmesh_local_disks_hash_by_ldisk_id_str, local_disk, local_disk);
	nvmeibt_local_disk_munmap_mem_tbls(local_disk);
	NNVMEIBT_BM_FREE(hu86aw9, local_disk->dev_nl_ctx);
	nvmeibt_topology_active_mark_reserialization_required();
	disk = NNVMEIBT_LOCAL_DISK_GET_DISK(ji8u723, local_disk);
	// local_disk->its_disk = NULL;
	if (disk) {
		disk->its_local_disk = NULL;
	}

	if (!nvmeibt_local_disk_is_bind_back_to_stock_needed(local_disk)) {
		controller_del_local_disk(local_disk->from_config.native_serial.str, local_disk, 1);
	}

	fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	NNVMEIBT_CLOSE(i989we8, fd);
	if (strcmp(local_disk->from_config.status, "Formatting") == 0) {
		struct nvmeibt_local_disk  *local_disk_tmp = NULL;

		N_Tf(cjfur81, "disk=@STR is formatting, move to side list", nvmeibt_local_disk_display(local_disk));
		NNVMEIBT_HASH_ADD_OBJ_ASCII_new(fkko035, cur_topo->formatting_local_disks_hash_by_ldisk_id_str,
				local_disk, 0,
				NVMEIBT_MAX_N_DISKS_PER_NODE, local_disk_tmp, local_disk);
	}
	else {
		NNVMEIBT_TOMA_FREE(alfo934, local_disk);
	}

	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(tsvaau3);
	NFOUT;
}

void nvmeibt_local_disk_remove_by_ldisk_id_str(const char *ldisk_id_str)
{
	struct nvmeibt_local_disk		*local_disk;
	struct nvmeibt_disk				*disk;

	NFIN;
	local_disk = nvmeib_hash_search_ascii_str(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str, ldisk_id_str);
	if (!local_disk) {
		N_Tf(uu55fr4, "disk_id=@STR is not in local_disks, probably already removed", ldisk_id_str);
		goto out;
	}

	nvmeibt_local_disk_mark_should_be_removed(local_disk);
	disk = NNVMEIBT_LOCAL_DISK_GET_DISK(nn33eii, local_disk);
	if (disk)
		nvmeibt_topology_remove_disk_from_its_current_node(disk);
	else
		nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(local_disk);

out:
	NFOUT;
}

int nvmeibt_local_disk_free_all_resources(void)
{
	struct nvmeibt_local_disk *local_disk;
	int rv = 0;

	NFIN;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		NNVMEIBT_HASH_DEL_OBJ_ASCII_new(hu86aw7, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str, local_disk, local_disk);
		rv |= nvmeibt_local_disk_destroy_and_free(local_disk);
	}
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->formatting_local_disks_hash_by_ldisk_id_str) {	// Should be empty
		NNVMEIBT_HASH_DEL_OBJ_ASCII_new(hu86aw8, nvmeibt_global_get_global()->formatting_local_disks_hash_by_ldisk_id_str, local_disk, local_disk);
		rv |= nvmeibt_local_disk_destroy_and_free(local_disk);
	}
	NFOUT;
	return rv;
}

void nvmeibt_local_disk_free_stock_fds(void)
{
	struct nvmeibt_local_disk *local_disk;
	NFIN;

	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str) {
		NNVMEIBT_CLOSE(ol98nt5, local_disk->dev_file_fd);
	}
	NFOUT;
}

struct nvmeibt_local_disk *nvmeibt_local_disk_get_local_disk_by_ldisk_id(const struct nvmeibt_ascii_uuid *ldisk_id, struct nvmeib_hash_table *local_disks_hash)
{
	return nvmeib_hash_search_ascii_str(local_disks_hash, ldisk_id->str);
}

static void stock_local_disk_forget_that_a_stock_local_disk(struct nvmeibt_local_disk *stock_local_disk)
{
	N_IMf(raomtqu, "Removing stock_local_disk=@STR", nvmeibt_local_disk_display(stock_local_disk));
	if (!stock_local_disk) {
		goto out;
	}
	if (!(stock_local_disk->is_bind_to_nvmeibs_needed)) {
		controller_del_local_disk(stock_local_disk->from_config.native_serial.str, stock_local_disk, 0);
	}
	nvmeibt_local_disk_stop_wq(stock_local_disk);
	NNVMEIBT_HASH_DEL_OBJ_ASCII_new(vdgh2q7, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str, stock_local_disk, local_disk);
	NNVMEIBT_CLOSE(t3_stock_local_disk_terminate, stock_local_disk->dev_file_fd);
	// NNVMEIBT_TOMA_FREE(t4_stock_local_disk_terminate, stock_local_disk);	// This very same object will transform into local_disk
out:
	;
}

void inspect_smart_changes(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_local_disk_util_smart_info	*smart_info = &(local_disk->from_config.smart_info);
	struct nvmeibt_local_disk_util_smart_info	*prev_smart_info = &(local_disk->from_config.prev_smart_info);

	NFIN;
	if (	smart_info->Critical_Warning						!= prev_smart_info->Critical_Warning ||
			smart_info->Available_Spare							!= prev_smart_info->Available_Spare ||
			smart_info->Available_Spare_Threshold				!= prev_smart_info->Available_Spare_Threshold ||
			smart_info->Power_On_Hours							!= prev_smart_info->Power_On_Hours ||
			smart_info->Media_Errors							!= prev_smart_info->Media_Errors ||
			smart_info->Number_of_Error_Information_Log_Entries	!= prev_smart_info->Number_of_Error_Information_Log_Entries) {
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(5jkhsd8);
		*prev_smart_info = *smart_info;
	}
	NFOUT;
}

void nvmeibt_local_disk_mark_periodic_reread_smart_counters_just_finished(struct nvmeibt_local_disk *local_disk, BOOL is_successful, BOOL is_periodic_reread_needed)
{
	N_Tf(v95kwo3, "disk=@STR is_successful=@BOOL is_periodic_reread_needed=@BOOL", nvmeibt_local_disk_display(local_disk), is_successful, is_periodic_reread_needed);
	if (!local_disk) {
		goto out;
	}
	if (!is_successful && is_supported_not_nvme_disk(local_disk))
		is_successful = true; // fake for external and virtual disks

	local_disk->is_periodic_reread_smart_counters_in_the_air = 0;

	if (disk_read_cnt_test) {
		N_Tf(t_xx_64, "Read smart counters fail simulation");
		is_successful = false;
	}

	if (is_successful != local_disk->was_last_read_of_smart_counters_successful) {
		local_disk->was_last_read_of_smart_counters_successful = is_successful;
		if (!strcmp(local_disk->from_config.status, "Ok")) {
			update_liveliness_of_seg_actives_of_specific_local_disk(local_disk);
			NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(643gsah);
		}
	}

	if (is_successful) {
		local_disk->is_smart_log_valid = 1;
		if (is_periodic_reread_needed && is_periodic_smart_polling_enabled) {
			getnstimeofday_boot(&(local_disk->last_periodic_reread_smart_counters_time));
		} else {
			local_disk->last_periodic_reread_smart_counters_time = TIMESPEC_MAX_C99;	// Never reread
		}
		inspect_smart_changes(local_disk);
	}
	else {
		if (is_periodic_reread_needed && is_periodic_smart_polling_enabled) {
			// Not sure whether a failure to read the smart should void the disk (conf_corrupted),
			//  or shall we treat it as smart_not_yet_ready, and retry soon after.
			// The code prior to adding this function was the latter, so keeping it this way
			local_disk->last_periodic_reread_smart_counters_time = TIMESPEC_ZERO;	// Immediately need to reread
		} else {
			N_Wf(wrsnd4n, "Tried to read the SMART once, failed. Will not retry");
			local_disk->last_periodic_reread_smart_counters_time = TIMESPEC_MAX_C99;	// Never reread
		}
	}
out:
	return;	// Avoid compilation error
}

struct nvmeibt_wq *nvmeibt_local_disk_create_wq(const char *ldisk_id_str)
{
	struct nvmeibt_wq		*wq;

	NFIN;
	wq = nvmeib_hash_search_ascii_str(nvmeibt_global_get_global()->ldisks_wq_hash_by_ldisk_id_str, ldisk_id_str);
	if (wq) {
		N_Tf(vgshgwe, "ldisk=@STR wq already exists", ldisk_id_str);
		goto out;
	}
	wq = nvmeibt_wq_create(ldisk_id_str);
	if (!wq) {
		N_Ef(sj74lsx, "Failed to create wq for ldisk=@STR", ldisk_id_str);
		goto out;
	}
	nvmeib_hash_add_ascii_str(nvmeibt_global_get_global()->ldisks_wq_hash_by_ldisk_id_str, nvmeibt_wq_get_name(wq), wq);
out:
	NFOUT;
	return wq;
}

enum nvmeibt_add_rv nvmeibt_local_disk_add_from_config(char *config_str, int config_tag)
{
	enum nvmeibt_add_rv					rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_local_disk			*new_local_disk = NULL, *local_disk_tmp = NULL;
	struct nvmeibt_local_disk			*stock_local_disk = NULL;
	struct nvmeibt_local_disk			*local_disk = NULL;
	struct nvmeibt_local_disk_config	*f = NULL;
	int									r;
	BOOL 								is_frozen = false, is_formatting = false;
	BOOL								should_reread_disk = false;
	struct nvmeibt_topology				*cur_topo = nvmeibt_global_get_global();

	NFIN;

	new_local_disk = NNVMEIBT_TOMA_CALLOC(trace_local_disk_nvmeibt_local_disk_add_from_config, 1, sizeof(*new_local_disk)); // Read into it, maybe use it.
	XDLIST_INIT_LINK(&new_local_disk->controller_local_disks_list_link, NULL);
	new_local_disk->seg_active_hash_by_uuid = NVMEIB_HASH_CREATE(4vghs8d, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 3), "seg_active_hash", 16, 0);

	new_local_disk->dev_file_fd = -1;
	new_local_disk->are_partitions_setup_in_mem = false;
	new_local_disk->is_PMBR_saved_on_disk = false;

	// id,blocks,block_size,max_request_size,seq,nsid,dev_name,metadata,status,vendor,model,native_serial
	f = &(new_local_disk->from_config);
	r = nvmeibt_sscanf_csv_line(config_str,
								's', sizeof(f->ldisk_id.str), &f->ldisk_id.str,
								'D', &f->n_pblk,
								'D', &f->n_hw_pblk,
								'd', &f->pblk_size,
								'd', &f->max_request_size,
								'd', &f->seq,
								'd', &new_local_disk->from_config.nsid,
								's', sizeof(f->dev_file_name), &f->dev_file_name,
								'd', &f->metadata_n_bytes,
								's', sizeof(f->status), &f->status,
								'd', &f->vendor,
								's', sizeof(f->smart_info.Model), &f->smart_info.Model,
								's', sizeof(new_local_disk->from_config.native_serial.str), &new_local_disk->from_config.native_serial.str,
								'\0');
	if (r <= 0) {
		rv = NVMEIBT_ADD_FAILED;
		goto out;
	}

	f->disk_type = nvmeibt_local_disk_get_stock_disk_type_by_dev_file_name(f->dev_file_name, NULL);
	nvmeibt_local_disk_util_extract_stripped_dev_name_from_dev_file_name(f->stripped_dev_file_name, f->dev_file_name);

	// On every hotplug / new-local_disk, we start everything from scratch as if
	//  the disk is good and functional, until proven otherwise
	new_local_disk->main_gpt.is_valid = false;
	new_local_disk->metadata_gpt.is_valid = false;
	new_local_disk->is_mbr_a_valid_pmbr = false;
	new_local_disk->is_being_formatted = false;
	new_local_disk->is_smart_log_valid = false;
	new_local_disk->last_periodic_reread_smart_counters_time = TIMESPEC_ZERO;
	new_local_disk->last_add_time = nvmeibt_global_get_cur_event_start_time();
	new_local_disk->is_periodic_reread_smart_counters_in_the_air = 0;
	new_local_disk->was_last_read_of_smart_counters_successful = true;
	new_local_disk->active_format_request_counter = 0;
	new_local_disk->reappearing_counter = 0;
	new_local_disk->is_being_deleted = false;
	new_local_disk->should_be_removed = false;
	new_local_disk->gpt_change_no = 0;
	new_local_disk->gpt_submitted_change_no = 0;
	new_local_disk->is_owned_by_nvmeibs_driver = 1;

	new_local_disk->CHANGE_EVENT_counters.active_zeroing_CHANGE_no = 0;
	new_local_disk->CHANGE_EVENT_counters.last_CHANGE_no = 0;

	is_frozen =     strstr(f->status, "Frozen") ? true : false;
	is_formatting = strstr(f->status, "Format") ? true : false; /*Capture both: Formatting and Format_Error*/

	f->smart_info.block_size = f->pblk_size;
	f->smart_info.metadata_size = f->metadata_n_bytes;
	f->smart_info.blocks = f->n_pblk;
	nvmeibt_strlcpy(f->smart_info.diskID, f->ldisk_id.str, sizeof(f->smart_info.diskID));
	nvmeibt_strlcpy(f->smart_info.format_options, "[]", sizeof(f->smart_info.format_options));

	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_local_disk_UUID(new_local_disk), nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);

	if (is_frozen || is_formatting) {
		if (local_disk) {
			f->smart_info = local_disk->from_config.smart_info;
			f->disk_metadata = local_disk->from_config.disk_metadata;
		}
	}

	nvmeibt_local_disk_clear_is_bind_to_nvmeibs_needed(new_local_disk);  // Disk came from nvmeibs, so no need to try and rebind it again.

	/*
	 * NOTE: it is safe to add @new_local_disk to @cur_topo->local_disks_hash
	 * without further checks because it was just freshly allocated and surely
	 * is not in @cur_topo->stock_local_disks_hash.
	 */
	rv = NNVMEIBT_HASH_ADD_OBJ_ASCII_new(dki98c6,
					cur_topo->nvmesh_local_disks_hash_by_ldisk_id_str,
					new_local_disk,
					config_tag,
					NVMEIBT_MAX_N_DISKS_PER_NODE, local_disk_tmp, local_disk);

	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL)
		goto out;

	// Clear formatting disk entry, if one existed
	local_disk_tmp = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_local_disk_UUID(new_local_disk), cur_topo->formatting_local_disks_hash_by_ldisk_id_str);
	if (local_disk_tmp) {
		N_Tf(dgy777w, "disk=@STR finished formatting", nvmeibt_local_disk_display(new_local_disk));
		NNVMEIBT_HASH_DEL_OBJ_ASCII_new(aki9e84, cur_topo->formatting_local_disks_hash_by_ldisk_id_str, local_disk_tmp, local_disk);	//sdfsdfsd/
		new_local_disk->reappearing_counter = local_disk_tmp->reappearing_counter;	// Got it from MGMT, and we need it in the next reportTarget
		NNVMEIBT_TOMA_FREE(dko089e, local_disk_tmp);
	}

	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_local_disk_UUID(new_local_disk), nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);

	if (nvmeibt_topology_is_disk_explicitly_excluded(&new_local_disk->from_config.native_serial, new_local_disk->from_config.vendor, new_local_disk->from_config.smart_info.Model, new_local_disk->from_config.nsid)) {
		local_disk->is_excluded = true;
		local_disk->is_explicitly_excluded = true;
		N_Tf(dlo049e, "disk=@STR is explicitly excluded", nvmeibt_local_disk_display(new_local_disk));
	}

	if ((nvmeibt_local_disk_UUID_str(local_disk)[0] == '\0') || strstr("UNKNOWN", nvmeibt_local_disk_UUID_str(local_disk))) {
		N_Ef(ski94ie, "disk=@STR dev_file_name=@STR is auto evicted because ldisk_id is UNKNOWN or null.",
			 nvmeibt_local_disk_display(local_disk), nvmeibt_local_disk_file_name(local_disk));
		sprintf(local_disk->from_config.status, "Error");
		goto out;
	}

	if (local_disk->is_excluded) { // Drive is excluded we need to bind it back to the stock nvme driver.
		N_Tf(ck093od, "local_disk=@STR vendor=@VENDOR marking that it should bind back to stock nvme driver",
			nvmeibt_local_disk_display(new_local_disk), nvmeibt_local_disk_vendor_id(local_disk));
		if (!nvmeibt_local_disk_is_bind_back_to_stock_needed(local_disk)) {
			nvmeibt_local_disk_mark_is_bind_back_to_stock_needed(local_disk);
			local_disk->its_udev_event_info = NULL;
		}
	}

	if (nvmeibt_topology_is_disk_explicitly_auto_takeover(&new_local_disk->from_config.native_serial, new_local_disk->from_config.vendor, new_local_disk->from_config.smart_info.Model, new_local_disk->from_config.nsid)) {
		new_local_disk->is_auto_takeover = true;
		N_Tf(bsykw92, "disk=@STR just marking is_auto_takeover. No action items", nvmeibt_local_disk_display(new_local_disk));
	}

	// In any case, update the following config-driven fields
	local_disk->is_being_deleted = 0;	// If is_being_deleted (hotunplug), and quickly hotplugged (MODIFIED)

	if (is_frozen || is_formatting) {
		N_Tf(fhy76r4, "disk=@STR ptr=@PTR is_frozen=@IS_FROZEN is_formatting=@IS_FORMATTING",
			nvmeibt_local_disk_display(local_disk), local_disk, is_frozen, is_formatting);
		goto out;
	}

	if (rv == NVMEIBT_ADD_NEW) {
		NTOMA_ASSERT(dji87r4, !local_disk->mmap_disk_locks_tbl.addr, "disk=@STR is just added, but already has mmap_tbl address=@ADDRESS_PTR",
					nvmeibt_local_disk_display(new_local_disk), local_disk->mmap_disk_locks_tbl.addr);
		NTOMA_ASSERT(asjue45, nvmeibt_local_disk_dev_file_fd(local_disk) < 0, "disk=@STR is just added, but already open fd=@LOCAL_DISK_DEV_FILE_FD",
					nvmeibt_local_disk_display(new_local_disk), nvmeibt_local_disk_dev_file_fd(local_disk));

		local_disk->dev_nl_ctx = nvmeibt_make_netlink_context_from_config(&local_disk->from_config);
		if (!local_disk->dev_nl_ctx) {
			N_Ef(73nsaoi, "disk=@STR cannot allocate netlink context!",  nvmeibt_local_disk_file_name(local_disk));
			rv = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;
			goto out;
		}

		local_disk->dev_file_fd = NNVMEIBT_OPEN_LOCAL_DISK_WRITE(gjit85r, nvmeibt_local_disk_file_name(local_disk));
		if (nvmeibt_local_disk_dev_file_fd(local_disk) < 0) {
			N_Ef(cmkigu5, "unable to open dev_file for disk=@STR dev_file=@DEV_FILE file_type=\"@FILE_TYPE_STR\" err=@AUTO_ERRNO",
				nvmeibt_local_disk_file_name(new_local_disk),
				nvmeibt_local_disk_file_name(local_disk),
				get_file_type_str(nvmeibt_local_disk_file_name(local_disk)));
			nvmeibt_local_disk_mark_is_being_deleted(local_disk);
			rv = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;
			goto out;
		}
		N_Tf(djut823, "local_disk=@STR opened fd=@INT", nvmeibt_local_disk_display(new_local_disk), nvmeibt_local_disk_dev_file_fd(local_disk));

		// Init the topology-related fields
		if (mmap_locks_table(local_disk) < 0) {
			N_Wf(shirn7v, "Skipping local disk=@STR, because failed to mmap_locks_table", nvmeibt_local_disk_display(local_disk));
			rv = NVMEIBT_ADD_SKIPPED;
			nvmeibt_local_disk_mark_is_being_deleted(local_disk);
			goto out;
		}

		N_Tf(gjity96, "adding disk=@STR, ptr=@PTR will read config from disk", nvmeibt_local_disk_display(local_disk), local_disk);
		should_reread_disk = true;
		controller_add_local_disk(local_disk->from_config.native_serial.str, local_disk, NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS);
		local_disk->wq = nvmeibt_local_disk_create_wq(nvmeibt_local_disk_UUID_str(local_disk));
	}

	if (rv == NVMEIBT_ADD_MODIFIED) {
		should_reread_disk = true;
		N_Tf(dkit95r, "disk=@STR modified, will reread config from disk", nvmeibt_local_disk_display(local_disk));
	}

	if (should_reread_disk) {
		// Try to restore the new disk gpt(s) and config.
		// Read persistent configuration of current disk
		nvmeibt_local_disk_clear_is_mem_in_sync_with_disk_metadata_gpt_entry_and_ctrl_of_segs(local_disk);
		if (launch_read_of_local_disk_gpt_and_segs_metadata_and_persist(local_disk) < 0) {
			// We failed reading persisted config from this disk, we ignore it and effectively evict it. Since it is
			// either unuseable due to a hardware problem or might already have users' data on it.
			char header[MGMT_LOG_MSG_HEADER_LEN];
			char msg[MGMT_LOG_MSG_MSG_LEN];
			snprintf(header, sizeof(header), "disk %.77s is unusable", nvmeibt_local_disk_display(local_disk));
			snprintf(msg, sizeof(msg), "Unable to read persistency from new disk, dev=%.32s on node=%.32s",
				 nvmeibt_local_disk_file_name(local_disk), nvmeibt_node_name(cur_topo->my_node));
			nvmeibt_kafka_generic_log_msg_to_mgmt_send(NULL, header, msg, NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_HIGH);
		}

		// Remove the local_disk from the stock_local_disks array since it is now part of the nvmeibs and not part of the stock driver.
		// At the point of removal the device might be already deleted from the list since we might have gotten a udev event about
		// it already, before we got the add event from nvmeibs.
		N_Tf(gjit8u6, "Trying to remove disk=@STR from stock_local_disks inventory", nvmeibt_local_disk_display(new_local_disk));
		stock_local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(nvmeibt_local_disk_UUID(new_local_disk), cur_topo->stock_local_disks_hash_by_ldisk_id_str);
		if (stock_local_disk) {
			N_Tf(aji8456, "Erasing disk=@STR from stock_local_disks, as it was added to nvmeibs, format_request_counter=@FORMAT_REQUEST_COUNTER",
				nvmeibt_local_disk_display(stock_local_disk), local_disk->active_format_request_counter);
			NTOMA_ASSERT(dmvjit6, rv == NVMEIBT_ADD_NEW, "remove from stock_local_disks of non-new disk=@STR by disk=@STR (rv=@RV)",
						nvmeibt_local_disk_display(stock_local_disk), nvmeibt_local_disk_display(local_disk), rv);
			if (!(stock_local_disk->is_being_formatted)) {
				N_Tf(rbnd04l, "disk=@STR stock_local_disk->is_being_formatted=0. Probably just hot-plugged", nvmeibt_local_disk_display(local_disk));
			} else {
				TODO(Launch format, and not wait for mgmt to resend the request);
			}
			local_disk->is_being_formatted = false; // stock_local_disk->is_being_formatted was probably true, but we want management to re-send the format
			local_disk->active_format_request_counter = stock_local_disk->active_format_request_counter;
			// Just update some flags. Some are not mandatory.
			local_disk->is_auto_takeover = stock_local_disk->is_auto_takeover;
			local_disk->is_bind_back_to_stock_needed = 0;
			local_disk->is_bind_to_nvmeibs_needed = 0;
			local_disk->is_excluded = stock_local_disk->is_excluded;
			//
			stock_local_disk_forget_that_a_stock_local_disk(stock_local_disk);
		}
	}

out:
	nvmeibt_local_disk_propagate_needed_bind_and_excluded_and_takeover_to_controller_local_disks(local_disk);
	if (is_frozen) {
		N_Tf(fkit954, "disk=@STR is frozen, skipping original rv=@RV", nvmeibt_local_disk_display(local_disk), rv);
		nvmeibt_local_disk_munmap_mem_tbls(local_disk);
	}

	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		// This triggers disk removal from the local_disks array, it is triggered in case the disk
		// is frozen OR in case we added a new local_disk and there is a problem with the locks
		// file of the disk.
		N_Tf(fkirt42, "Removing disk=@STR", nvmeibt_local_disk_display(local_disk));
		nvmeibt_local_disk_mark_should_be_removed(local_disk);
		nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(local_disk);
	} else if (rv == NVMEIBT_ADD_NEW) {
		/* nothing */ ;
	} else {
		N_Tf(akit954, "Freeing new_local_disk=@NEW_LOCAL_DISK rv=@RV new_local_disk=@NEW_LOCAL_DISK_PTR",
			nvmeibt_local_disk_display(new_local_disk), rv, new_local_disk);
		NNVMEIBT_TOMA_FREE(llo033c, new_local_disk);
	}

	NFOUT;
	return rv;
}

int nvmeibt_local_disk_recover_missing_ldisk_id_in_upgraded_disk_metadata(struct nvmeibt_local_disk_config *f)
{
	int			rv;

	NFIN;
	if (f->ldisk_id.str[0] == '\0' || f->native_serial.str[0] == '\0' || f->nsid == -1) {
		goto out_err;
	}
	if (strncmp(f->disk_metadata.ldisk_id_str, f->ldisk_id.str, sizeof(f->disk_metadata.ldisk_id_str)) != 0) {
		if (f->disk_metadata.ldisk_id_str[0] != '\0') {
			goto out_err;
		}
		nvmeibt_strlcpy(f->disk_metadata.ldisk_id_str, f->ldisk_id.str, sizeof(f->disk_metadata.ldisk_id_str));
	}
	if (strncmp(f->disk_metadata.native_serial_str, f->native_serial.str, sizeof(f->disk_metadata.native_serial_str)) != 0) {
		if (f->disk_metadata.native_serial_str[0] != '\0') {
			goto out_err;
		}
		nvmeibt_strlcpy(f->disk_metadata.native_serial_str, f->native_serial.str, sizeof(f->disk_metadata.native_serial_str));
	}
	if (f->disk_metadata.nsid != f->nsid) {
		if (f->disk_metadata.nsid != 0) {
			goto out_err;
		}
		f->disk_metadata.nsid = f->nsid;
	}
	rv = 0;
	goto out;
out_err:
	N_Ef(dciim1l, "ldisk_id=@STR(@STR) native_serial=@STR(@STR) nsid=@INT(@INT)", f->ldisk_id.str, f->disk_metadata.ldisk_id_str, f->native_serial.str, f->disk_metadata.native_serial_str, f->nsid, f->disk_metadata.nsid);
	rv = -1;
out:
	NFOUT;
	return rv;
}

/** [NVMESH-7967] EMULATE_4KPI: If EMULATE_4KPI is Yes/True/1, check for GPT or magic at fake-4k offset; if found, mark disk for nvmeibs bind.
 *  Returns true if found (caller should goto out_OK), false otherwise. */
static bool try_fake4kpi_gpt_or_magic(struct nvmeibt_local_disk *local_disk)
{
	bool rv = false;
	const char *emulate_val;
	int f4k_pblk_size;
	uint64_t fake4kpi_gpt_offset;
	uint64_t fake4kpi_magic_offset;
	char *sector_buf;
	bool found = false;
	ssize_t rd;

	NFIN;
	emulate_val = nvmeibt_global_nvmesh_conf_get_val_by_key("EMULATE_4KPI");
	if (!emulate_val ||
	    (strcasecmp(emulate_val, "Yes") != 0 &&
	     strcasecmp(emulate_val, "True") != 0 &&
	     strcmp(emulate_val, "1") != 0))
		goto out;

	f4k_pblk_size = local_disk->from_config.pblk_size;
	fake4kpi_gpt_offset = (uint64_t)FAKE4KPI_SECTORS_PER_LBA * f4k_pblk_size;
	fake4kpi_magic_offset = (uint64_t)FAKE4KPI_DATA_SECTORS * f4k_pblk_size;
	sector_buf = NNVMEIBT_BM_ALIGNED_CALLOC(f4kpi_gpt_alloc, PAGE_SIZE, f4k_pblk_size);
	rd = NNVMEIBT_PREAD(f4kpi_gpt, nvmeibt_local_disk_dev_file_fd(local_disk),
						sector_buf, f4k_pblk_size, fake4kpi_gpt_offset, 1);
	if (rd == f4k_pblk_size) {
		uint64_t gpt_sig;
		memcpy(&gpt_sig, sector_buf, sizeof(gpt_sig));
		if (gpt_sig == GPT_SIGNATURE)
			found = true;
	}

	if (!found) {
		rd = NNVMEIBT_PREAD(f4kpi_magic_rd, nvmeibt_local_disk_dev_file_fd(local_disk),
							sector_buf, f4k_pblk_size, fake4kpi_magic_offset, 1);
		if (rd == f4k_pblk_size &&
		    memcmp(sector_buf, FAKE4KPI_MAGIC, FAKE4KPI_MAGIC_LEN) == 0)
			found = true;
	}

	if (found) {
		NNVMEIBT_BM_FREE(f4kpi_gpt_free, sector_buf);
		nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(local_disk);
		N_Tf(f4kpi_gpt_found, "disk=@STR has GPT/magic at fake_4kpi offset, will be moved to nvmesh driver.",
			nvmeibt_local_disk_display(local_disk));
		rv = true;
		goto out;
	}
	NNVMEIBT_BM_FREE(f4kpi_gpt_free2, sector_buf);
out:
	NFOUT;
	return rv;
}

static void fill_disk_from_stock_driver_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct fill_local_disk_from_stock_driver_wq_entry			*entry;
	struct nvmeibt_local_disk									*new_local_disk;
	struct nvmeibt_disk_mbr										mbr;
	BOOL														is_formatted_with_md;
	const struct nvmeibt_disk_gpt_partition_entry				*disk_metadata_entry;
	const struct nvmeibt_disk_gpt_partition_entry				*mbr_gpt_metadata_entry;
	enum nvmeibt_disk_type										disk_type;
	BOOL														is_fill_devinfo_successful;

	NFIN;
	// A major task in this function is to decide which disks mark_is_bind_to_nvmeibs_needed()

	entry = container_of(wq_entry, struct fill_local_disk_from_stock_driver_wq_entry, wq_entry);

	new_local_disk = entry->new_local_disk;
	entry->rv = -1;

	disk_type = new_local_disk->from_config.disk_type;
	switch (disk_type) {
	case NVMEIBT_NVMESH_DISK_TYPE:
		N_Wf(vt6549a, "disk=@STR already binded to nvmesh", nvmeibt_local_disk_display(new_local_disk));
		is_fill_devinfo_successful = false;
		break;
	case NVMEIBT_NVME_DISK_TYPE:
		is_fill_devinfo_successful =
			nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_nvme_driver(&new_local_disk->from_config,
																					   entry->udev_event_info->dev_file_name,
																					   nvmeibt_local_disk_dev_file_fd(new_local_disk));
		break;
	case NVMEIBT_EXTERNAL_DISK_TYPE:
		is_fill_devinfo_successful =
			nvmeibt_local_disk_util_fill_local_disk_devinfo_and_smart_from_udev(&new_local_disk->from_config,
																				entry->udev_event_info->dev_file_name,
																				nvmeibt_local_disk_dev_file_fd(new_local_disk));
		break;
	case NVMEIBT_VIRTUAL_DISK_TYPE:
		is_fill_devinfo_successful =
			nvmeibt_local_disk_util_fill_devinfo_for_vdisk(&new_local_disk->from_config,
														   entry->udev_event_info->dev_file_name,
														   nvmeibt_local_disk_dev_file_fd(new_local_disk));
		break;
	default:
		is_fill_devinfo_successful = false;
	}
	if (!is_fill_devinfo_successful) {
		new_local_disk->is_mbr_a_valid_pmbr = 0;
		goto out;
	}
	// Now the new_local_disk->from_config.native_serial is filled

	nvmeibt_local_disk_mark_periodic_reread_smart_counters_just_finished(new_local_disk, 1, 0);

	// It is safe to test this inside the thread, since the relevant part of cur_topo is loaded on startup once.
	if (nvmeibt_topology_is_disk_explicitly_excluded(&new_local_disk->from_config.native_serial, new_local_disk->from_config.vendor, new_local_disk->from_config.smart_info.Model, new_local_disk->from_config.nsid)) {
		new_local_disk->is_excluded = true;
		new_local_disk->is_explicitly_excluded = true;
		N_Tf(omfua72, "disk=@STR is explicitly excluded", nvmeibt_local_disk_display(new_local_disk));
		goto out_OK;
	}

	if (new_local_disk->is_excluded) {
		N_Tf(cvsthw1, "stock disk=@STR is excluded. Probably failed to open(O_EXCL)", nvmeibt_local_disk_display(new_local_disk));
		goto out_OK;
	}

	is_formatted_with_md = (new_local_disk->from_config.smart_info.metadata_size != 0);

	new_local_disk->main_gpt.is_valid = false;
	new_local_disk->metadata_gpt.is_valid = false;
	new_local_disk->is_excluded = false;
	new_local_disk->is_explicitly_excluded = false;
	new_local_disk->is_auto_takeover = false;
	new_local_disk->should_relaunch_format_on_the_next_zeroing_finalize = false;
	new_local_disk->is_being_formatted = false;
	new_local_disk->active_format_request_counter = 0;
	new_local_disk->reappearing_counter = 0;
	new_local_disk->is_owned_by_nvmeibs_driver = 0;

	nvmeibt_strlcpy(new_local_disk->main_gpt.main_or_metadata, MAIN_GPT_NAME, sizeof(new_local_disk->main_gpt.main_or_metadata));
	nvmeibt_strlcpy(new_local_disk->metadata_gpt.main_or_metadata, METADATA_GPT_NAME, sizeof(new_local_disk->metadata_gpt.main_or_metadata));
	new_local_disk->main_gpt.ldisk_id = *nvmeibt_local_disk_UUID(new_local_disk);
	new_local_disk->metadata_gpt.ldisk_id = *nvmeibt_local_disk_UUID(new_local_disk);

	if (nvmeibt_topology_is_disk_explicitly_auto_takeover(&new_local_disk->from_config.native_serial, new_local_disk->from_config.vendor, new_local_disk->from_config.smart_info.Model, new_local_disk->from_config.nsid)) {
		new_local_disk->is_auto_takeover = true;
		nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(new_local_disk);
		N_Tf(2b8alk3, "disk=@STR is_auto_takeover", nvmeibt_local_disk_display(new_local_disk));
	}

	/* Decide whether this disk needs to be "moved" from stock nvme driver to nvmeibs driver.
	   This will happen if the disk is readable and has our GPT (and metadata partition) on it.
	*/
	N_Tf(trace_1_local_disk_fill_disk_from_stock_driver_wrapper, "new disk=@STR ptr=@PTR is_formatted_with_md=@IS_FORMATTED_WITH_MD",
		nvmeibt_local_disk_display(new_local_disk), new_local_disk, is_formatted_with_md);

	/*We automatically bind all disks that are formatted with MD to nvmesh, as previous stock drivers have
	  bugs when reading from such disks and cause system crashes upon read. We must therefore move these disks  															 .
	  to our driver prior to trying to read from them (even the main_GPT).
	  This isn't such a bad choice since someone who uses the stock driver cannot use these disks anyway															 .
	  once they are formatted with MD   																																	 .
	  For disks that are not formatted with MD we check if they are ours or not.*/
	/* DHSH: Related to BUG NVMESH-7436 The above remark is shit! During boot nvmesh does not exist and stock driver will read the MBR and GPT and get crash.
	   So if there is a problem - Solving it here is meaningless! The current solution we have is that GPT crc is corrupted */
	if (is_formatted_with_md) {
		nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(new_local_disk);
		N_Tf(trace_2_local_disk_fill_disk_from_stock_driver_wrapper, "disk=@STR is formatted with metadata, will be moved to nvmesh driver.", nvmeibt_local_disk_display(new_local_disk));
		goto out_OK;
	}
	if (nvmeibt_disk_metadata_read_mbr_blk(nvmeibt_local_disk_dev_nl_ctx(new_local_disk),
										   nvmeibt_local_disk_dev_file_fd(new_local_disk),
										   new_local_disk->from_config.pblk_size, &mbr,
										   nvmeibt_local_disk_display(new_local_disk), NULL) < 0) {
		N_ETf(5vw7j3i, "disk=@STR unable to read MBR blk.", nvmeibt_local_disk_display(new_local_disk));
		new_local_disk->is_PMBR_saved_on_disk = 0;
		goto out;
	}

	if (!nvmeibt_disk_metadata_is_protective_mbr(&mbr)) {
		N_Tf(cgqw7k2, "disk=@STR does not have valid pmbr, skipping.", nvmeibt_local_disk_display(new_local_disk));
		goto out_OK;
	}
	new_local_disk->is_mbr_a_valid_pmbr = 1;
	N_Tf(8xjk2lg, "disk=@STR, has protective MBR trying to restore Main-GPT", nvmeibt_local_disk_display(new_local_disk));
	if (nvmeibt_disk_metadata_restore_gpt(NULL, nvmeibt_local_disk_dev_file_fd(new_local_disk),
										  new_local_disk->from_config.pblk_size,
										  &new_local_disk->main_gpt,
										  1,
										  new_local_disk->from_config.n_pblk - 1,
										  true) < 0) {
		if (try_fake4kpi_gpt_or_magic(new_local_disk))
			goto out_OK;
		new_local_disk->is_done_reading_gpt_existing_or_not = 0;
		goto out;
	}
	mbr_gpt_metadata_entry = nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(&new_local_disk->main_gpt);
	if (!mbr_gpt_metadata_entry) {
		N_Tf(uxnxjrh, "disk=@STR does not have the correct metadata entry on it, skipping.", nvmeibt_local_disk_display(new_local_disk));
		goto out_OK;
	}

	// Try to find disk_metadata partition and read it.
	disk_metadata_entry = nvmeibt_disk_metadata_get_disk_metadata_entry(&new_local_disk->metadata_gpt);
	if (disk_metadata_entry) {
		// We should restore the disk metadata partition as well for this disk if it exists, so that formats can continue.
		if (nvmeibt_disk_metadata_read_disk_metadata(nvmeibt_local_disk_dev_nl_ctx(new_local_disk),
													 nvmeibt_local_disk_dev_file_fd(new_local_disk),
													 new_local_disk->from_config.pblk_size,
													 disk_metadata_entry->pba_s * new_local_disk->from_config.pblk_size,
													 &new_local_disk->from_config.disk_metadata) < 0) {
			N_Ef(eb8dl3i, "Unable to restore disk metadata partition on disk=@STR, that was read from nvme driver, partition is corrupt", nvmeibt_local_disk_display(new_local_disk));
			new_local_disk->is_done_reading_gpt_existing_or_not = 0;
			goto out;
		}

		N_Tf(cvya9ol, "Restored disk_metadata partition on disk=@STR md_supported=@BOOL", nvmeibt_local_disk_display(new_local_disk), new_local_disk->from_config.disk_metadata.is_md_supported);
	}
	nvmeibt_local_disk_recover_missing_ldisk_id_in_upgraded_disk_metadata(&(new_local_disk->from_config));
	//
	nvmeibt_strlcpy(new_local_disk->from_config.ldisk_id.str, new_local_disk->from_config.disk_metadata.ldisk_id_str, sizeof(new_local_disk->from_config.ldisk_id.str));
	//
	N_Tf(rqgqruk, "local_disk=@STR native_serial=@STR smart_serial=@STR", nvmeibt_local_disk_display(new_local_disk),
		 new_local_disk->from_config.native_serial.str, new_local_disk->from_config.smart_info.Serial_Number);
	if (new_local_disk->from_config.native_serial.str[0] == '\0') {
		if (new_local_disk->from_config.smart_info.Serial_Number[0]) {
			nvmeibt_strlcpy(new_local_disk->from_config.native_serial.str, new_local_disk->from_config.smart_info.Serial_Number, sizeof(new_local_disk->from_config.native_serial.str));
		} else {
			N_Ef(bshj02l, "No sorce for native_serial for local_disk=@STR", nvmeibt_local_disk_display(new_local_disk));
		}
	}
	//
	// This seems an nvmesh disk, we will try to move it to nvmesh.
	nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(new_local_disk);
	N_Tf(gxuwsho, "disk=@STR seems to have nvmesh Main-GPT, will be moved to nvmesh driver.", nvmeibt_local_disk_display(new_local_disk));
out_OK:
	new_local_disk->is_done_reading_gpt_existing_or_not = 1;
	entry->rv = 0;
out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}


static void fill_disk_from_stock_driver_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct fill_local_disk_from_stock_driver_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct fill_local_disk_from_stock_driver_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_local_disk_fill_disk_from_stock_driver_freer, entry);

	NFOUT;
}

static void fill_disk_from_stock_driver_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct fill_local_disk_from_stock_driver_wq_entry	*entry;
	enum nvmeibt_add_rv 								rv = NVMEIBT_ADD_UNINITIALIZED;
	struct nvmeibt_local_disk							*local_disk_tmp = NULL;

	NFIN;
	entry = container_of(wq_entry, struct fill_local_disk_from_stock_driver_wq_entry, wq_entry);

	/* if wq_entry was canceled, set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(zyj49o2, "canceled fill disk=@STR", nvmeibt_local_disk_display(entry->new_local_disk));
		entry->rv = -1;
	}

	if (entry->rv < 0) {
		N_Ef(fki98r4, "Error filling disk from stock driver disk=@STR", nvmeibt_local_disk_display(entry->new_local_disk));
		goto out;
	}

	N_Tf(aski93e, "Adding stock_local_disk=@STR",  nvmeibt_local_disk_display(entry->new_local_disk));


	rv = NNVMEIBT_HASH_ADD_OBJ_ASCII_new(fy7223c,
					nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str,
					entry->new_local_disk,
					0, /* Don't care. The config_tag is useless for local_disks that are updated per event */
					NVMEIBT_MAX_N_DISKS_PER_NODE, local_disk_tmp, local_disk);
	if (rv == NVMEIBT_ADD_FAILED || rv == NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL) {
		N_Wf(q4nbs8z, "Failed adding stock_local_disk=@STR", nvmeibt_local_disk_display(entry->new_local_disk));
		entry->rv = -1;
		goto out;
	}
	if (rv == NVMEIBT_ADD_NEW) {
		controller_add_local_disk(entry->new_local_disk->from_config.native_serial.str, entry->new_local_disk, NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK);
		entry->new_local_disk->wq = nvmeibt_local_disk_create_wq(nvmeibt_local_disk_UUID_str(entry->new_local_disk));
	}
	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(4jd8bjp);
	if (entry->new_local_disk->is_excluded) {
		NNVMEIBT_CLOSE(trace_5_local_disk_fill_disk_from_stock_driver_finalize, entry->new_local_disk->dev_file_fd);
	}

out:
	if (nvmeibt_local_disk_is_bind_to_nvmeibs_needed(entry->new_local_disk) && local_disk_tmp)
		local_disk_tmp->its_udev_event_info = entry->udev_event_info;
	else
		nvmeibt_toma_udev_event_processing_end(entry->udev_event_info);

	if ((entry->rv < 0 || rv != NVMEIBT_ADD_NEW) && entry->new_local_disk) {
		NNVMEIBT_CLOSE(trace_3_local_disk_fill_disk_from_stock_driver_finalize, entry->new_local_disk->dev_file_fd);
		N_Tf(cvwh3uk, "Freeing local disk=@STR rv=@RV new_local_disk=@NEW_LOCAL_DISK_PTR", nvmeibt_local_disk_display(entry->new_local_disk), rv, entry->new_local_disk);
		NNVMEIBT_TOMA_FREE(trace_4_local_disk_read_config_finalize, entry->new_local_disk);
	}
}

BOOL nvme_local_disk_is_smart_valid(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk && local_disk->is_smart_log_valid);
}

BOOL nvmeibt_local_disk_is_ready_for_segments(const struct nvmeibt_local_disk *local_disk)
{
	bool	is_ready;
	is_ready = (local_disk && local_disk->metadata_gpt.is_valid &&
				!nvmeibt_local_disk_is_being_deleted(local_disk) &&
				!(local_disk->is_excluded) &&
				local_disk->is_PMBR_saved_on_disk);
	if (!is_ready) {
		if (local_disk) {
			N_Tf(cvs623k, "metadata_gpt.is_valid=@BOOL is_being_deleted=@BOOL is_excluded=@BOOL its_disk=@PTR is_PMBR_saved_on_disk=@BOOL",
				 local_disk->metadata_gpt.is_valid, nvmeibt_local_disk_is_being_deleted(local_disk), local_disk->is_excluded, local_disk->its_disk, local_disk->is_PMBR_saved_on_disk);
		} else {
			N_Tf(ccv7h3m, "local_disk=NULL");
		}
	}
	return is_ready;
}

BOOL nvmeibt_local_disk_is_done_initial_reading_of_local_disk(const struct nvmeibt_local_disk *local_disk)
{
	return (local_disk && local_disk->is_smart_log_valid && local_disk->is_done_reading_gpt_existing_or_not);
}

int nvmeibt_local_disk_add_from_stock_driver(struct nvmeibt_udev_event_info *udev_event_info)
{
	int													rv = -1;
	struct fill_local_disk_from_stock_driver_wq_entry 	*wqe;
	const char											*dev_file_name = udev_event_info->dev_file_name;

	NFIN;

	wqe = NNVMEIBT_BM_CALLOC(t_h1_tomaldisk, sizeof(*wqe));

	wqe->wq_entry.type = "FILL_DISK_FROM_STOCK_DRIVER";
	wqe->wq_entry.execute = fill_disk_from_stock_driver_wrapper;
	wqe->wq_entry.finalize = fill_disk_from_stock_driver_finalize;
	wqe->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	wqe->wq_entry.free = fill_disk_from_stock_driver_freer;

	wqe->udev_event_info = udev_event_info;
	wqe->new_local_disk = NNVMEIBT_TOMA_CALLOC(t_h2_tomaldisk, 1, sizeof(*(wqe->new_local_disk)));   // Read into it, maybe use it.) {
	sprintf(wqe->new_local_disk->from_config.status, "Not_Initialized");
	wqe->new_local_disk->from_config.disk_type = udev_event_info->disk_type;
	XDLIST_INIT_LINK(&wqe->new_local_disk->controller_local_disks_list_link, NULL);
	wqe->new_local_disk->seg_active_hash_by_uuid = NVMEIB_HASH_CREATE(5vgd7j0, (HASH_MIN_LOG2_OF_N_ARR_ENTRIES + 3), "seg_active_hash", 16, 0);

	wqe->new_local_disk->are_partitions_setup_in_mem = false;
	if ((wqe->new_local_disk->dev_file_fd = NNVMEIBT_OPEN_READ_EXCL(rcgdh2k, dev_file_name, 0)) < 0) {
		N_Tf(tvcsghe, "open(@OPEN, O_EXCL) failed. err='@AUTO_ERRNO'. marking is_excluded and retrying non-O_EXCL", dev_file_name);
		wqe->new_local_disk->is_excluded = true;
		if ((wqe->new_local_disk->dev_file_fd = NNVMEIBT_OPEN_READ(t_h5_tomaldisk, dev_file_name, 1)) < 0) {
			N_WTf(t_h6_tomaldisk, "Unable to open=@OPEN for reading at all err=@AUTO_ERRNO", dev_file_name);
			goto err;
		}
	}
	else {
		NNVMEIBT_CLOSE(t_h6_1_tomaldisk,   wqe->new_local_disk->dev_file_fd);
		wqe->new_local_disk->dev_file_fd = NNVMEIBT_OPEN_LOCAL_DISK_WRITE(t_h5_1_tomaldisk, dev_file_name);
	}

	if (nvmeibt_toma_read_disk_from_stock_driver_add_work(&(wqe->wq_entry)) != 0) {
		N_Ef(t_h7_tomaldisk, "Unable to add fill disk from nvme driver offload task to WQ!");
		goto err;
	}

	rv = 0;
	goto out;

err:
	NNVMEIBT_CLOSE(t_h8_tomaldisk,   wqe->new_local_disk->dev_file_fd);
	NNVMEIBT_TOMA_FREE(t_h9_tomaldisk, wqe->new_local_disk);
	NNVMEIBT_BM_FREE(t_ha_tomaldisk, wqe);

out:
	NFOUT;
	return rv;
}

BOOL nvmeibt_local_disk_is_being_deleted(const struct nvmeibt_local_disk *local_disk)
{
	return (!local_disk || local_disk->is_being_deleted);
}

void nvmeibt_local_disk_mark_is_being_deleted(struct nvmeibt_local_disk *local_disk)
{
	if (local_disk) {
		local_disk->is_being_deleted = 1;
		N_Tf(c5vw83k, "local_disk=@STR", nvmeibt_local_disk_display(local_disk));
	}
}

static BOOL nvmeibt_local_disk_should_be_removed(struct nvmeibt_local_disk *local_disk)
{
	return (!local_disk || local_disk->should_be_removed);
}

void nvmeibt_local_disk_mark_should_be_removed(struct nvmeibt_local_disk *local_disk)
{
	if (local_disk) {
		local_disk->should_be_removed = true;
	}
}

void nvmeibt_local_disk_munmap_and_rm_if_should_be_removed_and_unused(struct nvmeibt_local_disk *local_disk)
{
	bool	is_any_active_recovery_on_local_disk;
	struct nvmeibt_seg_active		*seg_active;

	if (!local_disk) {
		goto out;
	}
	N_Tf(hur7822, "disk=@STR is_being_deleted=@IS_BEING_DELETED", nvmeibt_local_disk_display(local_disk), nvmeibt_local_disk_is_being_deleted(local_disk));
	if (nvmeibt_local_disk_should_be_removed(local_disk)) {
		is_any_active_recovery_on_local_disk = nvmeibt_recovery_launch_abort_all_recoveries_on_local_disk(local_disk);	// Also catch pre-registrant recoveries
		// Make sure that no recovery registrants exist, by making sure that none exist.
		//  Recovery registrants might access the memory using the recovery rx thread
		if (is_any_active_recovery_on_local_disk) {
			N_Tf(vwrhgvq, "Awaiting recoveries disk=@STR", nvmeibt_local_disk_display(local_disk));
			goto out;
		}
		if (nvmeibt_register_is_any_registered_on_local_disk(local_disk)) {
			N_Tf(fhyru11, "disk=@STR has_registrants", nvmeibt_local_disk_display(local_disk));
			goto out;
		}
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			NVMEIBT_SEG_ACTIVE_FREE_MEM_AND_PROCESSES(seg_active);
		}
		local_disk_remove_from_nvmeibs(local_disk);
	}
out:
	return;
}

void nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(struct nvmeibt_local_disk *local_disk)
{
	N_Tf(4gvsghj, "local_disk=@STR is_excluded=@INT is_explicitly_excluded=@INT", nvmeibt_local_disk_display(local_disk), local_disk->is_excluded, local_disk->is_explicitly_excluded);
	if (!(local_disk->is_excluded) && !(local_disk->is_explicitly_excluded)) {
		local_disk->is_bind_to_nvmeibs_needed = true;
	}
}

void nvmeibt_local_disk_clear_is_bind_to_nvmeibs_needed(struct nvmeibt_local_disk *local_disk)
{

	local_disk->is_bind_to_nvmeibs_needed = false;
}

BOOL nvmeibt_local_disk_is_bind_to_nvmeibs_needed(const struct nvmeibt_local_disk *local_disk)
{
	return local_disk->is_bind_to_nvmeibs_needed;
}

BOOL nvmeibt_local_disk_is_binding_to_nvmeibs(struct nvmeibt_local_disk *local_disk)
{
	return local_disk->is_binding_to_nvmeibs;
}

void nvmeibt_local_disk_mark_is_binding_to_nvmeibs(struct nvmeibt_local_disk *local_disk)
{
	local_disk->is_binding_to_nvmeibs = true;
}

void nvmeibt_local_disk_clear_is_binding_to_nvmeibs(struct nvmeibt_local_disk *local_disk)
{
	local_disk->is_binding_to_nvmeibs = false;
}

void nvmeibt_local_disk_mark_is_bind_back_to_stock_needed(struct nvmeibt_local_disk *local_disk)
{
	local_disk->is_bind_back_to_stock_needed = true;
}

void nvmeibt_local_disk_clear_is_bind_back_to_stock_needed(struct nvmeibt_local_disk *local_disk)
{

	local_disk->is_bind_back_to_stock_needed = false;
}

BOOL nvmeibt_local_disk_is_bind_back_to_stock_needed(const struct nvmeibt_local_disk *local_disk)
{
	return local_disk->is_bind_back_to_stock_needed;
}

BOOL nvmeibt_local_disk_is_binding_back_to_stock(struct nvmeibt_local_disk *local_disk)
{
	return local_disk->is_binding_back_to_stock;
}

void nvmeibt_local_disk_mark_is_binding_back_to_stock(struct nvmeibt_local_disk *local_disk)
{
	local_disk->is_binding_back_to_stock = true;
}

void nvmeibt_local_disk_clear_is_binding_back_to_stock(struct nvmeibt_local_disk *local_disk)
{
	local_disk->is_binding_back_to_stock = false;
}

static BOOL nvmeibt_local_disk_is_periodic_reread_smart_counters_needed(const struct nvmeibt_local_disk *local_disk)
{
	struct timespec	now;
	struct timespec	diff_timeout;
	BOOL			is_update_needed;

	is_update_needed = (!local_disk->is_periodic_reread_smart_counters_in_the_air &&
						is_periodic_smart_polling_enabled &&
						!nvmeibt_local_disk_is_being_deleted(local_disk));
	if (is_update_needed) {
		getnstimeofday_boot(&now);
		diff_timeout = timespec_sub(now, local_disk->last_periodic_reread_smart_counters_time);

		is_update_needed = (diff_timeout.tv_sec > LOCAL_DISK_SMART_PROBE_UPDATE_THRESHOLD_SEC);
	}
	return is_update_needed;
}

BOOL nvmeibt_local_disk_is_serjio_ready(const struct nvmeibt_local_disk *local_disk)
{
	return (!nvmeibt_local_disk_is_md_supported(local_disk) || (local_disk->serjio_status == NVMEIBS_SERJIO_STATUS_READY));
}

struct nvmeibt_local_disk *nvmeibt_stock_local_disk_get_by_dev_file_name(const char *dev_name, bool is_mandatory)
{
	struct nvmeibt_local_disk *stock_local_disk;

	NFIN;
	NVMEIB_HASH_FOREACH(stock_local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str) {
		if (!strcmp(dev_name, nvmeibt_local_disk_file_name(stock_local_disk))) {
			goto out;
		}
	}
	if (is_mandatory) {
		N_Wf(warn_local_disk_nvmeibt_stock_local_disk_get_by_dev_name, "stock_local_disk @DEV_NAME not found in inventory", dev_name);
	} else {
		N_Tf(dhcxik3, "stock_local_disk @DEV_NAME not found in inventory", dev_name);
	}
	stock_local_disk = NULL;

out:
	NFOUT;
	return stock_local_disk;
}

struct nvmeibt_local_disk* nvmeibt_local_disk_get_by_dev_file_name(const char *dev_file_name)
{
	struct nvmeibt_local_disk *local_disk;

	NFIN;
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str) {
		if (!strcmp(dev_file_name, nvmeibt_local_disk_file_name(local_disk))) {
			goto out;
		}
	}
	N_Wf(vcrhql1, "local_disk @DEV_NAME not found in inventory", dev_file_name);
	local_disk = NULL;

out:
	NFOUT;
	return local_disk;
}

void nvmeibt_local_disk_remove_stock_local_disk_by_dev_file_name(const char *dev_file_name)
{
	struct nvmeibt_local_disk	*stock_local_disk;

	NFIN;
	N_Tf(4vhqj8q, "Trying to remove stock_local_disk with dev_file_name=@STR n_stock_local_disks=@N_STOCK_LOCAL_DISKS",
		dev_file_name, nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str));

	NVMEIB_HASH_FOREACH(stock_local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str) {
		if (!strcmp(dev_file_name, nvmeibt_local_disk_file_name(stock_local_disk))) {
			if (stock_local_disk->is_being_formatted) {
				// The stock disk will be removed later on, when the new drive is bound to nvmesh.
				// This allows us to keep the format state of the disk alive
				N_Tf(ns0yjwl, "Not erasing disk=@STR from stock_local_disks (being formatted)", nvmeibt_local_disk_display(stock_local_disk));
				goto out;
			}
			N_Tf(5gw8k2o, "Erasing disk=@STR from stock_local_disks", nvmeibt_local_disk_display(stock_local_disk));
			stock_local_disk_forget_that_a_stock_local_disk(stock_local_disk);
			goto out;
		}
	}

	N_Tf(1vyompz, "Could not find @STR in stock_local_disks, it was probably already added to local_disks", dev_file_name);

out:
	NFOUT;
}

void nvmeibt_local_disk_mark_segs_post_update_actions_required(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_seg_active			*seg_active;

	NFIN;
	NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
		NVMEIBT_SEG_ACTIVE_MARK_ARE_POST_UPDATE_ACTIONS_REQUIRED(ui87yt6, seg_active);
	}
	NFOUT;
}

int nvmeibt_local_disk_update_serjio_state(const char *ldisk_id_str, const char *native_serial_str, int nsid, enum nvmeibs_serjio_status serjio_status)
{
	int		rv = 0;
	struct nvmeibt_local_disk	*local_disk;

	NFIN;
	local_disk = nvmeib_hash_search_ascii_str(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str, ldisk_id_str);
	if (local_disk) {
		if (serjio_status == NVMEIBS_SERJIO_STATUS_ERROR)
			N_Wf(o7w8wd7, "disk=@STR serjio_status=SERJIO_ERROR", nvmeibt_local_disk_display(local_disk));
		else
			N_Tf(oo08wd7, "disk=@STR setting serjio_STATUS=@SERJIO_STATUS", nvmeibt_local_disk_display(local_disk), serjio_status);
		if (local_disk->serjio_status != serjio_status) {
			if ((local_disk->serjio_status == NVMEIBS_SERJIO_STATUS_READY) ||
				(serjio_status == NVMEIBS_SERJIO_STATUS_READY)) {
				nvmeibt_local_disk_mark_segs_post_update_actions_required(local_disk);
			}
			local_disk->serjio_status = serjio_status;
		}
	}
	else {
		N_Tf(fhjur83, LOCAL_DISK_LOG_FMT " not found", ldisk_id_str, native_serial_str, nsid);
		rv = -1;
	}
	NFOUT;
	return rv;
}

BOOL nvmeibt_local_disk_is_formatted(const struct nvmeibt_local_disk *local_disk)
{
	const struct nvmeibt_local_disk_config *f = &local_disk->from_config;
	BOOL rv = false;

	if (strcmp(f->status, "Not_Initialized") != 0 &&
		strcmp(f->status, "Formatting") != 0 &&
		strcmp(f->status, "Error") != 0 &&
		strcmp(f->status, "Format_Error") != 0 &&
		strcmp(f->status, "Ingesting") != 0) {
		rv = true;
	}

	return rv;
}

void print_one_local_disk_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx, struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_local_disk_config *f = &local_disk->from_config;

	(*printf_fn)(printf_ctx, "- Disk=%s pblk_size=%d metadata_size=%d n_pblks=%llu n_hw_pblk=%llu dev=%s status=%s%s%s%s\n",
				 nvmeibt_local_disk_display(local_disk), f->pblk_size, f->metadata_n_bytes, f->n_pblk, f->n_hw_pblk,
				 f->dev_file_name, f->status,
				 local_disk->is_excluded ? " EXCLUDED" : "",
				 local_disk->is_explicitly_excluded ? "(explicit)" : "",
				 local_disk->is_drive_write_error ? " DRIVE_WRITE_ERROR" : "");
	if (strstr(f->status, "Initializing") != NULL) {
		(*printf_fn)(printf_ctx, "last_pba_zeroed=%zu\n", f->disk_metadata.last_pba_zeroed);
	}

	if (nvmeibt_local_disk_is_formatted(local_disk)) {
		struct nvmeibt_Str *gpt_ctx = NNVMEIBT_STR_ALLOC(trace_local_disk_print_one_local_disk_status);
		struct nvmeibt_Str *metadata_gpt_ctx = NNVMEIBT_STR_ALLOC(trace_1_local_disk_print_one_local_disk_status);

		nvmeibt_disk_metadata_fill_gpt_header_str(&local_disk->main_gpt.header, gpt_ctx, "Main", "Mem");
		nvmeibt_disk_metadata_fill_all_gpt_entries_str(local_disk->main_gpt.entries, local_disk->main_gpt.max_n_entries, true, gpt_ctx, "Main", "Mem");
		(*printf_fn)(printf_ctx, "%s", nvmeibt_Str_str(gpt_ctx));
		nvmeibt_disk_metadata_fill_gpt_header_str(&local_disk->metadata_gpt.header,  metadata_gpt_ctx, "Metadata", "Mem");
		nvmeibt_disk_metadata_fill_all_gpt_entries_str(local_disk->metadata_gpt.entries, local_disk->metadata_gpt.max_n_entries, true, metadata_gpt_ctx, "Metadata", "Mem");
		(*printf_fn)(printf_ctx, "%s", nvmeibt_Str_str(metadata_gpt_ctx));

		NNVMEIBT_STR_FREE(trace_2_local_disk_print_one_local_disk_status, gpt_ctx);
		NNVMEIBT_STR_FREE(trace_3_local_disk_print_one_local_disk_status, metadata_gpt_ctx);
	}
	(*printf_fn)(printf_ctx, "\n");
}

void nvmeibt_local_disk_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx)
{
	struct nvmeibt_local_disk	*local_disk;
	struct nvmeibt_local_disk	*stock_local_disk;

	(*printf_fn)(printf_ctx, "\n- - - - -   LOCAL DISKS   - - - - -\n");
	NVMEIB_HASH_FOREACH(local_disk, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str)
		print_one_local_disk_status(printf_fn, printf_ctx, local_disk);
	(*printf_fn)(printf_ctx, "\n- - - - -   STOCK LOCAL DISKS   - - - - -\n");
	NVMEIB_HASH_FOREACH(stock_local_disk, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str)
		print_one_local_disk_status(printf_fn, printf_ctx, stock_local_disk);
	(*printf_fn)(printf_ctx, "\n");
}

static void periodic_reread_smart_counters_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct periodic_reread_smart_counters_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct periodic_reread_smart_counters_wq_entry, wq_entry);

	entry->rv = -1;
	N_Tf(trace_local_disk_update_smart_counters_wrapper, "Updating smart counters for disk=@STR", entry->ld_display);

	if (nvmeibt_local_disk_util_get_nvme_smart_log(entry->fd, &entry->smart_log) < 0) {
		N_Wf(warn_local_disk_update_smart_counters_wrapper, "Unable to get_nvme_smart_log for disk=@STR from fd=@FD, possibly removed", entry->ld_display, entry->fd);
		goto out;
	}

	entry->rv = 0;

out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void periodic_reread_smart_counters_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct periodic_reread_smart_counters_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct periodic_reread_smart_counters_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_local_disk_update_smart_counters_freer, entry);

	NFOUT;
}

void periodic_reread_smart_counters_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct periodic_reread_smart_counters_wq_entry *entry;
	struct nvmeibt_local_disk *local_disk = NULL;

	NFIN;

	entry = container_of(wq_entry, struct periodic_reread_smart_counters_wq_entry, wq_entry);

	if (entry->is_stock_disk) {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&entry->ldisk_id, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str);
	}
	else {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&entry->ldisk_id, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	}

	if (!local_disk) {
		N_Tf(hwtnxqa, "disk=@STR not found... cannot update smart_log", entry->ld_display);
		goto out;
	}

	/* if wq_entry was canceled, set wq_entry->rv = -1 to be treated like error */
	if (wq_entry->is_canceled) {
		N_Tf(anvix7w, "canceled updating smart_log for disk=@STR", entry->ld_display);
		entry->rv = -1;
	}

	if (entry->rv < 0) {
		N_Wf(vhms9kp, "Error updating smart_log for disk=@STR", entry->ld_display);
		goto out;
	}

	// Update the values in the given local disk.
	local_disk->from_config.smart_info.Critical_Warning = entry->smart_log.critical_warning;
	local_disk->from_config.smart_info.Available_Spare = entry->smart_log.avail_spare;
	local_disk->from_config.smart_info.Available_Spare_Threshold = entry->smart_log.spare_thresh;
	local_disk->from_config.smart_info.Percentage_Used = entry->smart_log.percent_used;
	local_disk->from_config.smart_info.Host_Write_Commands = nvmeibt_host_writes_int128_to_uint64(entry->smart_log.host_writes);
	local_disk->from_config.smart_info.Power_Cycles = nvmeibt_host_writes_int128_to_uint64(entry->smart_log.power_cycles);
	local_disk->from_config.smart_info.Power_On_Hours = nvmeibt_host_writes_int128_to_uint64(entry->smart_log.power_on_hours);
	local_disk->from_config.smart_info.Unsafe_Shutdowns = nvmeibt_host_writes_int128_to_uint64(entry->smart_log.unsafe_shutdowns);
	local_disk->from_config.smart_info.Media_Errors = nvmeibt_host_writes_int128_to_uint64(entry->smart_log.media_errors);
	local_disk->from_config.smart_info.Number_of_Error_Information_Log_Entries = nvmeibt_host_writes_int128_to_uint64(entry->smart_log.num_err_log_entries);

out:
	nvmeibt_local_disk_mark_periodic_reread_smart_counters_just_finished(local_disk, (entry->rv == 0), 1);
	NFOUT;
}

void nvmeibt_local_disk_launch_local_disk_periodic_reread_smart_counters_if_needed(struct nvmeibt_local_disk *local_disk, bool is_stock_disk)
{
	struct periodic_reread_smart_counters_wq_entry *periodic_reread_smart_counters_task = NULL;
	int			rv;

	// Usually, the first SMART read is from restore_disk_structures_finalize(),
	//  which is a different code path,
	//  and then periodically (if needed/enabled)
	// Some disks (Azure), are slow when reading the smart, so the following
	//  periodic rereads are disabled after the first call.

    // NFIN;
	if (!nvmeibt_local_disk_is_periodic_reread_smart_counters_needed(local_disk)) {
		goto out;
	}

	periodic_reread_smart_counters_task = NNVMEIBT_BM_CALLOC(49ksl4r, sizeof (*periodic_reread_smart_counters_task));

	periodic_reread_smart_counters_task->wq_entry.type = "UPDATE_SMART_LOG";
	periodic_reread_smart_counters_task->wq_entry.execute = periodic_reread_smart_counters_wrapper;
	periodic_reread_smart_counters_task->wq_entry.finalize = periodic_reread_smart_counters_finalize;
	periodic_reread_smart_counters_task->wq_entry.abort = nvmeibt_toma_wakeup_wq_abort_func;
	periodic_reread_smart_counters_task->wq_entry.free = periodic_reread_smart_counters_freer;
	periodic_reread_smart_counters_task->ldisk_id = *nvmeibt_local_disk_UUID(local_disk);
	nvmeibt_strlcpy(periodic_reread_smart_counters_task->ld_display, nvmeibt_local_disk_display(local_disk), sizeof(periodic_reread_smart_counters_task->ld_display));
	periodic_reread_smart_counters_task->vendor_id = local_disk->from_config.vendor;
	periodic_reread_smart_counters_task->fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	periodic_reread_smart_counters_task->rv = -1;
	periodic_reread_smart_counters_task->is_stock_disk = is_stock_disk;

	rv = nvmeibt_local_disk_add_work_with_ldisk_last_CHANGE_no(local_disk, &(periodic_reread_smart_counters_task->wq_entry));
	if (rv != 0) {
		N_Ef(usnej2n, "Unable to add update log task to WQ @STR disk=@STR", (is_stock_disk ? "stock" : ""), nvmeibt_local_disk_display(local_disk));
		goto free_resources;
	}
	local_disk->is_periodic_reread_smart_counters_in_the_air = 1;
	goto out;

free_resources:
	NNVMEIBT_BM_FREE(n2mis6x, periodic_reread_smart_counters_task);

out:
	;//NFOUT;
}

void nvmeibt_local_disk_fill_ld_info(struct local_disk_info	*ld_info, struct nvmeibt_local_disk *local_disk)
{
	ld_info->from_config = local_disk->from_config;	// Holds many parameters including the disk_metadata record
	ld_info->nl_ctx = nvmeibt_local_disk_dev_nl_ctx(local_disk);
	ld_info->fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	ld_info->gpt_change_no = local_disk->gpt_change_no;
	ld_info->mbr = local_disk->mbr;
	ld_info->main_gpt = local_disk->main_gpt;
	ld_info->metadata_gpt = local_disk->metadata_gpt;
	ld_info->is_gpt_written = 0;
	ld_info->is_PMBR_saved_on_disk = local_disk->is_PMBR_saved_on_disk;
	ld_info->local_disk_in_info = local_disk;
}

int nvmeibt_local_disk_write_PMBR_and_GPTs(struct local_disk_info *ld_info)
{
	int									rv;

	NFIN;
	if (!(ld_info->is_PMBR_saved_on_disk)) { // Write the PMBR only from here, during zeroing. No race.
		rv = nvmeibt_write_GPTS_of_a_local_disk(ld_info);
		if (rv < 0) {
			goto out;
		}
		rv = nvmeibt_write_PMBR_of_a_local_disk(ld_info->nl_ctx,
												local_disk_info_fd(ld_info),
												local_disk_info_pblk_size(ld_info),
												&(ld_info->mbr),
												local_disk_info_display(ld_info));
		if (rv < 0) {
			goto out;
		}
	}
	rv = 0;
out:
	NFOUT;
	return rv;
}

static void bind_not_nvme_disk_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_bind_wq_entry *e = container_of(wq_entry, struct local_disk_bind_wq_entry, wq_entry);
	int rc;

	NFIN;
	rc = nvmeib_srvr_api_lib_disk_nvmeof_sata_bind(e->dev_file_name, e->model, e->serial, e->vendor, e->is_stock_to_nvmeibs);
	if (rc == 0)
		e->rv = 0;
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *)wq_entry);
	NFOUT;
}

static void bind_not_nvme_disk_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_bind_wq_entry		*entry;

	entry = container_of(wq_entry, struct local_disk_bind_wq_entry, wq_entry);
	N_Tf(uju76v2, "disk=@STR is_stock_to_nvmeibs=@INT rv=@INT", entry->ld_display, entry->is_stock_to_nvmeibs, entry->rv);
	if (wq_entry->is_canceled) {
		entry->rv = -1;
	}
	if (entry->rv < 0) {
		N_Wf(opnj84x, "Error during bind_disk for disk=@STR", entry->ld_display);
	}
	if (entry->udev_event_info)
		nvmeibt_toma_udev_event_processing_end(entry->udev_event_info);
}

bool is_bdf_and_serial_match(char *bdf_str, char *native_serial_str, int seq, int dev_file_fd, bool is_stock_to_nvmeibs, bool is_auto_takeover)
{
	char										path[100];
	struct stat									st;
	int											fd = -1;
	char										read_serial[24];
//	ssize_t										n_bytes;
	int											rc;
	bool										rv = 0;
	struct nvmeibt_local_disk_util_smart_info	smart_info;

	NFIN;
	snprintf(path, sizeof(path), "/sys/bus/pci/drivers/%s/%s", is_stock_to_nvmeibs ? "nvme" : "nvmeibs", bdf_str);
	rc = stat(path, &st);
	if (rc<0) {
		N_Wf(bhh29ka, "Cannot find PCI BDF=@STR in path=@STR serial=@STR rc=@INT @AUTO_ERRNO", bdf_str, path, native_serial_str, rc);
		goto out;
	}
	if (is_stock_to_nvmeibs) {
		// Read the nvme details
TODO(make the following work);
#if 0	// verify the serial
		fd = NNVMEIBT_OPEN(rvhsji3, path, O_RDONLY);
		if (fd < 0) {
			N_Ef(vxbaj1l, "Cannot open '@STR''! is_auto_takeover=@BOOL @AUTO_ERRNO", path, is_auto_takeover);
			goto out;
		}
		n_bytes = NNVMEIBT_PREAD_ATOMIC(vhwk37d, fd, read_serial, sizeof(read_serial), 0, 0);
		if (n_bytes <= 0) {
			// The warning was printed from pread
			goto out;
		}
		if (strncmp(read_serial, native_serial_str, sizeof(read_serial)) != 0) {
			N_Wf(f8dbvu1, "BDF=@STR expected_serial=@STR actual_serial=@STR)", bdf_str, native_serial_str, read_serial);
			goto out;
		}
#else	// #if 0	// verify the serial
(void)is_auto_takeover;
#endif	// #if 0	// verify the serial
	} else {
		// Read the nvmeibs smart details
		if (nvmeibt_local_disk_util_read_smart_info(seq, &smart_info, dev_file_fd) < 0) {
			N_Wf(7cnwtjo, "Error reading smart_info for fd=@INT native_serial=@STR", dev_file_fd, native_serial_str);
			goto out;
		}
		if (strncmp(smart_info.Serial_Number, native_serial_str, sizeof(smart_info.Serial_Number)) != 0) {
			N_Wf(iv7ct4m, "BDF=@STR read_serial=@STR expected_serial=@STR)", bdf_str, read_serial, native_serial_str);
			goto out;
		}
	}
	rv = 1;		// We have a match
out:
	NNVMEIBT_CLOSE(icnu2kd, fd);
	NFOUT_rv;
	return rv;
}

static void bind_disk_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_bind_wq_entry *entry;
	int op_rv;
	char *path;
	char *bdf_str;
	struct nvmeibt_local_disk_controller	*controller;
	char									*native_serial_str;

	NFIN;

	entry = container_of(wq_entry, struct local_disk_bind_wq_entry, wq_entry);
	native_serial_str = entry->serial;
	bdf_str = entry->bdf;
	if (!is_bdf_and_serial_match(bdf_str, native_serial_str, entry->seq, entry->dev_file_fd, entry->is_stock_to_nvmeibs, entry->is_auto_takeover)) {
		N_Tf(1vy5uwb, "Skipping BDF=@STR serial=@STR", bdf_str, native_serial_str);
		entry->rv = 1;
		goto out;
	}

	controller = entry->controller_for_sanity_checks;
	NTOMA_ASSERT(gyqpb5x, controller, "controller(@STR)=NULL", native_serial_str);
	if (	(entry->is_stock_to_nvmeibs && (controller->controller_state != NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK)) ||
			(!(entry->is_stock_to_nvmeibs) && (controller->controller_state != NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS))) {
		N_Tf(bbsj90l, "Skip serial=@STR is_stock_to_nvmeibs=@BOOL BDF=@STR state=@STR", controller->native_serial_str, entry->is_stock_to_nvmeibs, bdf_str, controller_state_str(controller->controller_state));
		entry->rv = 1;
		goto out;
	}
	N_Tf(8sh3bao, "dev_file_name=@STR serial=@STR is_auto_takeover=@BOOL is_stock_to_nvmeibs=@BOOL", entry->dev_file_name, entry->serial, entry->is_auto_takeover, entry->is_stock_to_nvmeibs);
	path = entry->is_stock_to_nvmeibs ? "nvme" : "nvmeibs";
	op_rv = nvmeib_srvr_api_lib_disk_unbind(bdf_str, !entry->is_stock_to_nvmeibs);
	if ((op_rv != 0) && (op_rv != -EIO)) {
		N_Ef(usnfsd4, "Cannot open path='@STR' serial=@STR is_auto_takeover=@BOOL rv=@RV, @AUTO_ERRNO", path, entry->serial, entry->is_auto_takeover, op_rv);
		entry->rv = -1;
		goto out;

	} else if (op_rv == 0) {
		controller->controller_state = (entry->is_stock_to_nvmeibs ? NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNBOUND_STOCK_TO_NVMEIBS : NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNBOUND_NVMEIBS_TO_STOCK);
		N_Tf(vhqj3kw, "Unbind write SUCCESS BDF=@STR serial=@STR to path=@STR @AUTO_ERRNO", bdf_str, entry->serial, path);
	} else {
		controller->controller_state = NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNDEFINED;
		if (entry->is_auto_takeover) {
			N_Tf(vh2j9l2, "Unbind write FAILED BDF=@STR to path=@STR serial=@STR @AUTO_ERRNO", bdf_str, path, entry->serial);
			entry->rv = 1;
		} else {
			N_Wf(b5j46ne, "Unbind write FAILED BDF=@STR to path=@STR serial=@STR @AUTO_ERRNO", bdf_str, path, entry->serial);
			entry->rv = -1;
		}
		goto out;
	}
	path = entry->is_stock_to_nvmeibs ? "nvmeibs" : "nvme";
	op_rv = nvmeib_srvr_api_lib_disk_dobind(bdf_str,  entry->is_stock_to_nvmeibs);
	if ((op_rv != 0) && (op_rv != -EIO)) {
		N_Ef(7shsdkl, "Cannot open path='@STR' serial=@STR is_auto_takeover=@BOOL rv=@RV, @AUTO_ERRNO", path, entry->serial, entry->is_auto_takeover, op_rv);
		entry->rv = -1;
		goto out;
	} else if (op_rv == 0) {
		controller->controller_state = (entry->is_stock_to_nvmeibs ? NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS : NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK);
		entry->rv = 0;
		N_Tf(7rghdkw, "Bind write SUCCESS BDF=@STR path=@STR", bdf_str, path);
	} else {
		if (	(XDLIST_N_ELEMNTS(&(controller->local_disks_list)) > 0 &&	// Already has a "disk", probably my siebling namespace
				 controller->controller_state == (entry->is_stock_to_nvmeibs ? NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_NVMEIBS : NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_STOCK))) {
			N_Tf(rcgsyjw, "Bind failed, but controller is in good state. Possibly due to a bind on a siebling name-space");
			entry->rv = 0;
		} else {
			controller->controller_state = NVMEIBT_LOCAL_DISK_CONTROLLER_STATE_UNDEFINED;
			entry->rv = -1;
			N_Ef(03mskxg, "Bind Failed BDF=@STR to path=@STR serial=@STR @AUTO_ERRNO", bdf_str, path, entry->serial);
		}
	}
out:
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void bind_disk_finalize(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_bind_wq_entry		*entry;
	struct nvmeibt_local_disk			*old_local_disk;

	NFIN;
	entry = container_of(wq_entry, struct local_disk_bind_wq_entry, wq_entry);
	N_Tf(yridp3x, "disk=@STR is_stock_to_nvmeibs=@INT rv=@INT", entry->ld_display, entry->is_stock_to_nvmeibs, entry->rv);
	if (wq_entry->is_canceled) {
		entry->rv = -1;
	}
	if (entry->rv < 0) {
		N_Wf(cbuekwl, "Error during bind_disk for disk=@STR", entry->ld_display);
		goto out;
	}
	old_local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&(entry->ldisk_id),
																   (entry->is_stock_to_nvmeibs ? nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str :
																	nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str));
	controller_del_local_disk(entry->serial, old_local_disk, !(entry->is_stock_to_nvmeibs));
	if (entry->is_stock_to_nvmeibs == 0) {
		goto out;	// bind_to_stock. No hurry
	}
	// Succedded with unbind from stock and bind to nvmeibs
	// Leaving the stock_local_disk alive for its is_being_formatted flag, that will be used when the nvmeibs device is added
	//
	// Bind was successful, Not controller_add since the nvmeibs local_disk is generated only as part of the DISK_CHANGE_EVENT (probably not yet).
out:
	if (entry->udev_event_info)
		nvmeibt_toma_udev_event_processing_end(entry->udev_event_info);
	NFOUT;
}

static void bind_disk_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_bind_wq_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct local_disk_bind_wq_entry, wq_entry);
	NNVMEIBT_BM_FREE(trace_local_disk_bind_disk_freer, entry);

	NFOUT;
}

void nvmeibt_local_disk_launch_bind_to_nvmeibs(struct nvmeibt_local_disk *stock_local_disk)
{
	struct local_disk_bind_wq_entry 	*bind_wq_entry;

	NFIN;
	N_Tf(owjseum, "stock_local_disk=@STR", nvmeibt_local_disk_display(stock_local_disk));
	if (!is_supported_not_nvme_disk(stock_local_disk) && stock_local_disk->from_config.pcie_bdf[0] == '\0') {
		N_Ef(4gs7jhs, "Unsupported disk_type disk=@STR BDF=@STR",nvmeibt_local_disk_display(stock_local_disk), stock_local_disk->from_config.pcie_bdf);
		goto out;
	}
	if (nvmeibt_local_disk_is_binding_to_nvmeibs(stock_local_disk)) {
		goto out;
	}
	bind_wq_entry = NNVMEIBT_BM_CALLOC(t1_local_disk_bind_to_nvmeibs, sizeof(*bind_wq_entry));

	bind_wq_entry->wq_entry.type = "LOCAL_DISK_BIND";
	bind_wq_entry->wq_entry.free = bind_disk_freer;
	bind_wq_entry->rv = -1;
	bind_wq_entry->is_stock_to_nvmeibs = 1;		// unbind from stock, bind to nvmesh
	bind_wq_entry->is_auto_takeover = stock_local_disk->is_auto_takeover;
	nvmeibt_strlcpy(bind_wq_entry->dev_file_name, nvmeibt_local_disk_file_name(stock_local_disk), sizeof(bind_wq_entry->dev_file_name));
	nvmeibt_strlcpy(bind_wq_entry->ld_display, nvmeibt_local_disk_display(stock_local_disk), sizeof(bind_wq_entry->ld_display));
	nvmeibt_strlcpy(bind_wq_entry->serial, stock_local_disk->from_config.native_serial.str, sizeof(bind_wq_entry->serial));
	nvmeibt_strlcpy(bind_wq_entry->model, stock_local_disk->from_config.smart_info.Model, sizeof(bind_wq_entry->model));
	bind_wq_entry->vendor = nvmeibt_local_disk_vendor_id(stock_local_disk);
	memcpy(bind_wq_entry->bdf, stock_local_disk->from_config.pcie_bdf, sizeof(bind_wq_entry->bdf));
	bind_wq_entry->ldisk_id = stock_local_disk->from_config.ldisk_id;

	if (is_supported_not_nvme_disk(stock_local_disk)) {
		bind_wq_entry->wq_entry.execute = bind_not_nvme_disk_wrapper;
		bind_wq_entry->wq_entry.finalize = bind_not_nvme_disk_finalize;
	}
	else {
		bind_wq_entry->wq_entry.execute = bind_disk_wrapper;
		bind_wq_entry->wq_entry.finalize = bind_disk_finalize;
	}
	bind_wq_entry->udev_event_info = stock_local_disk->its_udev_event_info;
	bind_wq_entry->controller_for_sanity_checks = controller_get_by_native_serial(bind_wq_entry->serial);

	if (nvmeibt_wq_run_once(&bind_wq_entry->wq_entry) == NULL) {
		N_Ef(rha9o4p, "Unable to add disk bind offload task to WQ! stock_local_disk=@STR", nvmeibt_local_disk_display(stock_local_disk));
		NNVMEIBT_BM_FREE(t2_local_disk_bind_to_nvmeibs, bind_wq_entry);
		goto out;
	}

	nvmeibt_local_disk_clear_is_bind_to_nvmeibs_needed(stock_local_disk);
	nvmeibt_local_disk_mark_is_binding_to_nvmeibs(stock_local_disk);
	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(xyaiw2m);
	if (stock_local_disk->is_being_formatted == false)
		sprintf(stock_local_disk->from_config.status, "Ingesting");

out:
	NFOUT;
}


void nvmeibt_local_disk_launch_bind_back_to_stock(struct nvmeibt_local_disk *local_disk)
{
	struct local_disk_bind_wq_entry 	*bind_wq_entry;

	NFIN;
	if (nvmeibt_local_disk_is_binding_back_to_stock(local_disk)) {
		goto out;
	}
	if (!nvmeibt_local_disk_is_done_initial_reading_of_local_disk(local_disk)) {
		N_Tf(cusjzi2, "Skipping @STR !is_done_initial_reading_of_local_disk", nvmeibt_local_disk_display(local_disk));
		goto out;
	}
	if (!is_supported_not_nvme_disk(local_disk) && local_disk->from_config.pcie_bdf[0] == '\0') {
		N_Ef(ssrimf0, "Error @STR BDF='@STR'",nvmeibt_local_disk_display(local_disk), local_disk->from_config.pcie_bdf);
		goto out;
	}
	N_Tf(3vasjhw, "@STR", nvmeibt_local_disk_display(local_disk));
	bind_wq_entry = NNVMEIBT_BM_CALLOC(t1_local_disk_bind_back_to_stock, sizeof(*bind_wq_entry));

	bind_wq_entry->wq_entry.type = "LOCAL_DISK_UNBIND";
	bind_wq_entry->wq_entry.free = bind_disk_freer;
	bind_wq_entry->rv = -1;
	bind_wq_entry->is_stock_to_nvmeibs = 0;		// unbind from nvmesh, bind to stock
	bind_wq_entry->dev_file_fd = local_disk->dev_file_fd;
	bind_wq_entry->seq = local_disk->from_config.seq;
	nvmeibt_strlcpy(bind_wq_entry->dev_file_name, nvmeibt_local_disk_file_name(local_disk), sizeof(bind_wq_entry->dev_file_name));
	nvmeibt_strlcpy(bind_wq_entry->ld_display, nvmeibt_local_disk_display(local_disk), sizeof(bind_wq_entry->ld_display));
	nvmeibt_strlcpy(bind_wq_entry->serial, local_disk->from_config.native_serial.str, sizeof(bind_wq_entry->serial));
	bind_wq_entry->vendor = nvmeibt_local_disk_vendor_id(local_disk);
	nvmeibt_strlcpy(bind_wq_entry->bdf, local_disk->from_config.pcie_bdf, sizeof(bind_wq_entry->bdf));
	bind_wq_entry->ldisk_id = local_disk->from_config.ldisk_id;

	if (is_supported_not_nvme_disk(local_disk)) {
		bind_wq_entry->wq_entry.execute = bind_not_nvme_disk_wrapper;
		bind_wq_entry->wq_entry.finalize = bind_not_nvme_disk_finalize;
	}
	else {
		bind_wq_entry->wq_entry.execute = bind_disk_wrapper;
		bind_wq_entry->wq_entry.finalize = bind_disk_finalize;
	}
	bind_wq_entry->udev_event_info = local_disk->its_udev_event_info;
	bind_wq_entry->controller_for_sanity_checks = controller_get_by_native_serial(bind_wq_entry->serial);

	if (nvmeibt_wq_run_once(&bind_wq_entry->wq_entry) == NULL) {
		N_Ef(vgsh482, "Unable to add disk bind offload task to WQ! disk=@STR", nvmeibt_local_disk_display(local_disk));
		NNVMEIBT_BM_FREE(t2_local_disk_bind_back_to_stock, bind_wq_entry);
		goto out;
	}

	nvmeibt_local_disk_clear_is_bind_back_to_stock_needed(local_disk);
	nvmeibt_local_disk_mark_is_binding_back_to_stock(local_disk);
	NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(5snsjq9);
	sprintf(local_disk->from_config.status, "Exgesting");

out:
	NFOUT;
}

static int nvme_submit_admin_passthru(int fd, struct nvme_passthru_cmd *cmd)
{
	return ioctl(fd, NVME_IOCTL_ADMIN_CMD, cmd);
}

static int nvme_identify(int fd, __u32 nsid, __u32 cdw10, void *data)
{
	struct nvme_admin_cmd cmd = {
		.opcode		= nvme_admin_identify,
		.nsid		= nsid,
		.addr		= (__u64)(uintptr_t) data,
		.data_len	= NVME_IDENTIFY_DATA_SIZE,
		.cdw10		= cdw10,
	};

	return nvme_submit_admin_passthru(fd, &cmd);
}

struct format_ctx_data {
	pthread_mutex_t guard_mutex;
	pthread_cond_t 	completion_signal;
	int 			rv;
};

static void format_disk_on_done(void *ctx, int is_ok, struct nvmeib_nl_uk_comm_rep *msg)
{
	struct format_ctx_data *format_ctx = ctx;
	int pt_err;

	NFIN;
	NTOMA_ASSERT(format_disk_on_done_0, (!msg && !is_ok) || msg->opcode == csc_format_disk,
				"format done, on non format msg of type=@TYPE", msg->opcode);

	if (!format_ctx) {
		 N_Ef(trace_format_disk_on_done_1, "Cannot wakeup caller thread as format ctx is NULL");
		 nvmeibt_abort(ES_FATAL);
	}

	pt_err = pthread_mutex_lock(&format_ctx->guard_mutex);
	if (pt_err != 0) {
		N_Ef(trace_format_disk_on_done_2, "Cannot wakeup caller thread, cannot lock_mutex=@LOCK_MUTEX error: @INT (@STR)", &format_ctx->guard_mutex, pt_err, strerror(pt_err));
		nvmeibt_abort(ES_FATAL);
	}

	format_ctx->rv = is_ok ? 0 : 1;

	// Wake the thread that called this zeroing operation.
	if ((pt_err = pthread_cond_signal(&format_ctx->completion_signal)) != 0) {
		N_Ef(trace_format_disk_on_done_3, "Cannot wakeup caller thread with cond_var=@COND_VAR pthread signal error: @INT (@STR)", &format_ctx->completion_signal, pt_err, strerror(pt_err));
		nvmeibt_abort(ES_FATAL);
	}

	if ((pt_err = pthread_mutex_unlock(&format_ctx->guard_mutex)) != 0) {
		N_Ef(trace_format_disk_on_done_4, "Cannot wakeup caller thread, cannot unlock_mutex=@UNLOCK_MUTEX error: @INT (@STR)", &format_ctx->guard_mutex, pt_err, strerror(pt_err));
		nvmeibt_abort(ES_FATAL);
	}

	NFOUT;
}

static void format_disk_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_format_wq_entry *entry;
	int err;
	int pt_err;
	int nsid;
	struct nvme_id_ns ns;
	int i;
	int lbaf = -1;
	int md_type = 0;
	char *dma_buffer = NULL;
	struct km_comm_msg_hdr *format_msg = NULL;
	struct nvmeib_format_disk *format_msg_payload = NULL;
	pthread_condattr_t attr;
	struct format_ctx_data *format_ctx = NULL;
	const struct nvmeibt_disk_flow_params_t *params = NULL;
	int		cur_pblk_size;
	uint64_t last_block_addr;
	char bdf[32]="";

	NFIN;

	entry = container_of(wq_entry, struct local_disk_format_wq_entry, wq_entry);
	entry->rv = -1;

	if (entry->bdf)
		nvmeibt_strlcpy(bdf, entry->bdf, sizeof(bdf));

	entry->fd = dup(entry->fd);
	params = nvmeibt_disk_flow_params_get(entry->format_details.model, true);

	if (entry->format_details.is_nvme) {
		nsid = ioctl(entry->fd, NVME_IOCTL_ID);
		err = nvme_identify(entry->fd, nsid, NVME_ID_CNS_NS, &ns);
		if (err) {
			N_Ef(vnjxlw0, "id-ns err=@FD disk=@STR", err, entry->ld_display);
			goto out;
		}

		for (i = 0; i <= ns.nlbaf; i++) {
			unsigned int metadata_size = ns.lbaf[i].ms;	// note: little-endian
			unsigned int block_size = 1 << ns.lbaf[i].ds;
			if (entry->format_details.block_size == block_size && entry->format_details.metadata_size == metadata_size) {
				lbaf = i;
				break;
			}
		}
		if (params && params->force_metadata && entry->format_details.metadata_size)
			md_type = 0;
		else
			md_type = (ns.mc > 1) ? 0 : 1;

		if (lbaf < 0) {
			N_Ef(csmikk4, "id-ns results: lbaf=@FD md_type=@FD disk=@STR", lbaf, md_type, entry->ld_display);
			goto out;
		}

		cur_pblk_size = 1 << ns.lbaf[ns.flbas & 0xf].ds;
		last_block_addr = ns.nsze * cur_pblk_size;
	}
	else {
		last_block_addr = entry->format_details.n_pblk * entry->format_details.block_size;
	}

	// Clear out the MBR
	dma_buffer = NNVMEIBT_BM_ALIGNED_CALLOC(trace_local_disk_format_disk_wrapper, PAGE_SIZE, PAGE_SIZE*2);

	if (NNVMEIBT_PWRITE(warn_1_local_disk_format_disk_wrapper, entry->fd, dma_buffer, PAGE_SIZE*2, 0, 0) < 0) {
		N_Wf(humedy4, "Failed to write size @SIZEOF at offset 0x0 fd=@FD disk=@STR buff=@BUFFER (@AUTO_ERRNO)", PAGE_SIZE*2, entry->fd, entry->ld_display, (void *) dma_buffer);
	}
	// Clear out last block (backup GPT)
	if (NNVMEIBT_PWRITE(warn_2_local_disk_format_disk_wrapper, entry->fd, dma_buffer, PAGE_SIZE, last_block_addr - PAGE_SIZE, PAGE_SIZE) < 0) {
		N_Wf(xncjisk, "Failed to write size @SIZEOF at offset @OFFSET fd=@FD disk=@STR buff=@BUFFER (@AUTO_ERRNO)", PAGE_SIZE, (ns.nsze-1)*PAGE_SIZE, entry->fd, entry->ld_display, (void *) dma_buffer);
	}

	if (params && params->skip_reformat && entry->old_blocksize == entry->format_details.block_size) {
		N_Tf(iymw9gh, "Skipping reformat as blocksize is already set disk=@STR", entry->ld_display);
		entry->rescan_after_format = true;
		goto mark_disk_as_nvmesh_formatted;
	}

	if (entry->format_details.is_nvme) {
		format_msg = NNVMEIBT_BM_CALLOC(trace_format_disk_wrapper_nl_1, sizeof(*format_msg) + sizeof(*format_msg_payload));
		format_msg_payload = (struct nvmeib_format_disk *)(format_msg->data);
		format_ctx = NNVMEIBT_BM_CALLOC(trace_format_disk_wrapper_nl_2, sizeof(*format_ctx));

		pt_err = pthread_mutex_init(&format_ctx->guard_mutex, NULL);
		if (pt_err != 0) {
			N_Ef(trace_format_disk_wrapper_nl_2_5, "Failed to create format context guard err=@INT (@STR)", pt_err, strerror(pt_err));
			goto out;
		}
		if ((pt_err = pthread_condattr_init(&attr)) != 0) {
			N_Ef(trace_format_disk_wrapper_nl_3, "Failed to create cond var attr err=@INT (@STR)", pt_err, strerror(pt_err));
			goto out;
		}
		if ((pt_err = pthread_cond_init(&format_ctx->completion_signal, &attr)) != 0) {
			N_Ef(trace_format_disk_wrapper_nl_4, "Failed to create format context cond var err=@INT (@STR)", pt_err, strerror(pt_err));
			goto out;
		}
		if ((pt_err = pthread_mutex_lock(&format_ctx->guard_mutex)) != 0) {
			N_Ef(trace_format_disk_wrapper_nl_5, "Cannot wakeup caller thread, cannot lock_mutex=@LOCK_MUTEX error: @INT (@STR)",
					&format_ctx->guard_mutex, pt_err, strerror(pt_err));
			goto out;
		}

		/* Send a message to nvmeibs to format the disk.
		   Wait for the callback of the completion to wake us up.*/

		snprintf(format_msg_payload->disk_id, sizeof(format_msg_payload->disk_id), "%.*s",
				 (int)(sizeof(format_msg_payload->disk_id) - 1), entry->format_details.ldisk_id.str);
		format_msg_payload->vendor_id = entry->format_details.vendor_id;
		format_msg_payload->format_id.val = lbaf | md_type << 4;
		format_msg_payload->flag_nvme_format = 1;
		format_msg_payload->flag_delete_create_ns = 0;
		if (params && params->delete_ns_when_formatting) {
			format_msg_payload->flag_nvme_format = 0;
			format_msg_payload->flag_delete_create_ns = 1;
		}
		if (params && params->reset_after_format) {
			format_msg_payload->flag_reset_ctrlr = 1;
		}
		if (params) {
			N_Tf(trace_format_disk_wrapper_nl_5_1, "model=@STR, format @INT delete-ns/create-ns @INT",
				entry->format_details.model,
				format_msg_payload->flag_nvme_format,
				format_msg_payload->flag_delete_create_ns);
		}
		else {
			N_Tf(trace_format_disk_wrapper_nl_5_2, "model=@STR, can't get params", entry->format_details.model);
		}

		format_msg->opcode = csc_format_disk;
		format_msg->on_done = format_disk_on_done;
		format_msg->ctx = (void *)format_ctx;
		format_msg->len = sizeof(*format_msg_payload);

		if (nvmeibt_send_msg_to_srv(format_msg) != 0) {
			N_Ef(cga8q9k, "Unable to send csc_format_disk msg to srv. disk=@STR!", entry->ld_display);
			goto out;
		}

		// Wait for zeroing to finish
		if ((pt_err = pthread_cond_wait(&format_ctx->completion_signal, &format_ctx->guard_mutex)) != 0) {
			N_Ef(trace_format_disk_wrapper_nl_7, "Cannot wait for format to finish, cond_var=@COND_VAR error: @INT (@STR)", &format_ctx->completion_signal, pt_err, strerror(pt_err));
			format_ctx = NULL;	// leak is better than a use-after-free
			goto out;
		}

		N_Tf(trace_format_disk_wrapper_nl_8, "Request to format disk completed disk=@STR rv=@INT", entry->ld_display, format_ctx->rv);
		if (format_ctx->rv) {
			entry->rv = format_ctx->rv;
			goto out;
		}
	}
	else {
		// SATA/SAS drives must be re-probed
		entry->rescan_after_format = true;
	}

mark_disk_as_nvmesh_formatted:
	snprintf(dma_buffer, PAGE_SIZE, NVMESH_FORMATTED_HDR ", block_size=%d, metadata_size=%d, is_md_supported=%u, ldisk_id=%s,  disk_obj_uuid=%s, format_request_counter=%d",
			 entry->format_details.block_size, entry->format_details.metadata_size,
			 (entry->format_details.metadata_size>0)?1:0,
			 entry->format_details.ldisk_id.str, nvmeibt_union_uuid_to_urn_uuid(&(entry->format_details.disk_obj_uuid)).str, entry->format_details.format_request_counter);
	if (NNVMEIBT_PWRITE(warn_3_local_disk_format_disk_wrapper, entry->fd, dma_buffer, PAGE_SIZE, 0, 0) < 0) {
		N_Ef(error_5_local_disk_format_disk_wrapper, "Failed to write size @SIZEOF at offset 0x0 fd=@FD buff=@BUFFER (@AUTO_ERRNO)", PAGE_SIZE, entry->fd, (void *) dma_buffer);
		if (params && params->reset_after_format && bdf[0]) {
			N_Tf(trace_format_disk_wrapper_nl_9, "Rebinding disk due to write error");
			close(entry->fd);
			entry->fd = -1;
			(void)nvmeib_srvr_api_lib_disk_unbind(bdf, true);
			(void)nvmeib_srvr_api_lib_disk_dobind(bdf, true);
		}
	} else {
		entry->rv = 0;
	}
out:
	if (format_ctx) {
		pt_err = pthread_mutex_unlock(&format_ctx->guard_mutex);
		if (pt_err != 0) {
			N_Ef(xx_35, "pthread_mutex_unlock failed err=@INT (@STR)", pt_err, strerror(pt_err));
		}
		if ((pt_err = pthread_cond_destroy(&format_ctx->completion_signal))) {
			N_Ef(xx_36, "pthread_cond_destroy failed err=@INT (@STR)", pt_err, strerror(pt_err));
		}
		if ((pt_err = pthread_mutex_destroy(&format_ctx->guard_mutex))) {
			N_Ef(xx_37, "pthread_mutex_destroy failed err=@INT (@STR)", pt_err, strerror(pt_err));
		}
		NNVMEIBT_BM_FREE(trace_format_disk_wrapper_nl_free_1, format_ctx);
	}
	if (format_msg)
		NNVMEIBT_BM_FREE(trace_format_disk_wrapper_nl_free_2, format_msg);
	if (dma_buffer)
		NNVMEIBT_BM_FREE(hhy441a, dma_buffer);

	if (entry->fd>=0)
		close(entry->fd);
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void format_disk_freer(struct nvmeibt_wq_entry *wq_entry) {
	struct local_disk_format_wq_entry *entry;
	NFIN;

	entry = container_of(wq_entry, struct local_disk_format_wq_entry, wq_entry);
	// It is a pitty that the use-case that led to the following (very rare?) use case is not documented
	if (entry->rescan_after_format) {
		struct nvmeibt_Str			*new_config = NNVMEIBT_STR_ALLOC(trace_format_disk_freer_2);
		struct nvmeibt_Str			*edited_new_config = NNVMEIBT_STR_ALLOC(trace_format_disk_freer_4);
		if (nvmeibt_read_config_file(new_config, NVMEIBT_CSV_TYPE_LOCAL_DISKS) < 0) {
			N_Ef(trace_format_disk_freer_1, "Failed reading disks csv after format");
		} else {
			// read first 2 lines
			const char *start = nvmeibt_Str_str(new_config);
			const char *newline = strchr(start, '\n');
			if (newline) {
				newline = strchr(newline+1, '\n');
			}
			if (newline) {
				nvmeibt_Str_strncpy(edited_new_config, start, (newline-start)+1);
				do {
					start = newline+1;
					newline = strchr(start, '\n');
					if (*start && !newline)
						newline = start+strnlen(start, (1<<12))-1;
					if (strncmp(start, entry->format_details.ldisk_id.str, sizeof(entry->format_details.ldisk_id.str)) == 0) {
						// If the line from disks csv matches the formatted disk then generate a config with only this new line for immediate parsing
						nvmeibt_Str_strncat(edited_new_config, start, (newline-start)+1);
					}
				} while(*start);
			}
		}
		NNVMEIBT_STR_FREE(trace_format_disk_freer_3, new_config);

		nvmeibt_parse_csv_buf(edited_new_config, 0, NVMEIBT_CSV_TYPE_LOCAL_DISKS);
		NNVMEIBT_STR_FREE(trace_format_disk_freer_5, edited_new_config);
	}
	NNVMEIBT_BM_FREE(trace_local_disk_format_disk_freer, entry);
	NFOUT;
}

int nvmeibt_local_disk_launch_disk_format(struct nvmeibt_local_disk *in_local_disk)
{
	struct local_disk_format_wq_entry 	*disk_format_wq_entry;
	struct nvmeibt_local_disk			*local_disk;
	int rv = -1;

	NFIN;

	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&(in_local_disk->pending_format.ldisk_id), nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	if (!local_disk) {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&(in_local_disk->pending_format.ldisk_id), nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str);
		if (!local_disk) {
			N_Ef(fhur875, "disk=@STR not found, cannot launch format", nvmeibt_local_disk_display(in_local_disk));
		}
		else {
			nvmeibt_local_disk_mark_is_bind_to_nvmeibs_needed(local_disk);
			local_disk->its_udev_event_info = NULL;
			nvmeibt_local_disk_propagate_needed_bind_and_excluded_and_takeover_to_controller_local_disks(local_disk);
			local_disk->is_being_formatted = true;
			rv = -2;
			N_Tf(5cs98wk, "stock_disk=@STR binding to nvmeibs first", nvmeibt_local_disk_display(in_local_disk));
		}
		goto out;
	}

	local_disk->main_gpt.header.disk_obj_uuid = in_local_disk->pending_format.disk_obj_uuid;

	// Just to be on the safe side, invalidate a selected set of the local_disk's flags
	local_disk->are_partitions_setup_in_mem = 0;
	local_disk->is_PMBR_saved_on_disk = 0;
	local_disk->is_mbr_a_valid_pmbr = 0;
	local_disk->serjio_status = NVMEIBS_SERJIO_STATUS_NOT_FOUND;
	local_disk->main_gpt.is_valid = 0;
	local_disk->metadata_gpt.is_valid = 0;

	local_disk->active_format_request_counter = local_disk->pending_format.format_request_counter;
	local_disk->is_being_formatted = true;

	disk_format_wq_entry = NNVMEIBT_BM_CALLOC(t1_local_disk_launch_disk_format, sizeof(*disk_format_wq_entry));

	disk_format_wq_entry->wq_entry.type = "LOCAL_DISK_FORMAT";
	disk_format_wq_entry->wq_entry.execute = format_disk_wrapper;
	disk_format_wq_entry->wq_entry.free = format_disk_freer;
	disk_format_wq_entry->rv = -1;
	disk_format_wq_entry->fd = nvmeibt_local_disk_dev_file_fd(local_disk);
	disk_format_wq_entry->seq = local_disk->from_config.seq;
	disk_format_wq_entry->bdf = local_disk->from_config.pcie_bdf;
	disk_format_wq_entry->old_blocksize = local_disk->from_config.pblk_size;
	disk_format_wq_entry->rescan_after_format = false;
	disk_format_wq_entry->format_details.ldisk_id = local_disk->pending_format.ldisk_id;
	disk_format_wq_entry->format_details.vendor_id = local_disk->pending_format.vendor_id;
	disk_format_wq_entry->format_details.block_size = local_disk->pending_format.block_size;
	disk_format_wq_entry->format_details.n_pblk = local_disk->from_config.n_hw_pblk;
	disk_format_wq_entry->format_details.is_nvme = is_supported_nvme_disk(local_disk);
	disk_format_wq_entry->format_details.metadata_size = local_disk->pending_format.metadata_size;
	disk_format_wq_entry->format_details.disk_obj_uuid = local_disk->pending_format.disk_obj_uuid;
	disk_format_wq_entry->format_details.format_request_counter = local_disk->pending_format.format_request_counter;
	nvmeibt_strlcpy(disk_format_wq_entry->format_details.model, local_disk->from_config.smart_info.Model, sizeof(disk_format_wq_entry->format_details.model));
	nvmeibt_strlcpy(disk_format_wq_entry->ld_display, nvmeibt_local_disk_display(local_disk), sizeof(disk_format_wq_entry->ld_display));

	if (nvmeibt_wq_run_once(&disk_format_wq_entry->wq_entry) == NULL) {
		N_Ef(aji98w3, "Unable to add disk format offload task to WQ!");
		NNVMEIBT_BM_FREE(dkit984, disk_format_wq_entry);
		goto out;
	}
	// Mark disk as formatting, in case we will send a report target for now.
	sprintf(local_disk->from_config.status, "Formatting");
	rv = 0;

out:
	NFOUT;
	return rv;
}

void nvmeibt_local_disk_set_read_cnt_test(bool read_cnt_test)
{
	if (disk_read_cnt_test != read_cnt_test) {
		if (read_cnt_test)
			N_Tf(t_xx_65, "Start fail read disk's smart counters");
		else
			N_Tf(t_xx_66, "Stop fail read disk's smart counters");
		disk_read_cnt_test = read_cnt_test;
	}
}

/******************************* Disk format params ***************************/

static struct nvmeibt_disks_models_flow_params_t disk_model_flow_params = {
	.n_models = 0,
	.dm = {{{0},0,0,0,0,0,0,0,0,0,0},},				// All zero
	.dflt = {
		.model = {'!','D','e','f','a','u','l','t'},
		.is_zeroing_using_test_and_write = true,
		.is_using_nvme_trim_before_zero = true,
		.is_secure_erase_after_disk_format = true,
		.is_zeroing_mandatory = false,
		.delete_ns_when_formatting = false,
		.reset_after_format = false,
		.skip_reformat = false,
		.ignore_metadata = false,
		.force_metadata = false,
		.force_512b = false
	},
};

#define __try_parse_disk_param_v1_3_2(_trace, _name, dst_ptr) do { \
	cur_param_name = _name; \
	cur_prefix_len = sizeof(_name) - 1; \
	if (!strncmp(config, cur_param_name, cur_prefix_len)) { \
		if (sscanf(config + cur_prefix_len + 1, "%d", dst_ptr) > 0) { \
            N_Tf(_trace, "@STR", config); \
			(*n_matches)++; \
			goto _out; \
		} else { \
			goto _err; \
		} \
	} \
} while (0)

static void __disk_flow_params_print(const char *prefix, const struct nvmeibt_disk_flow_params_t *p, int index)
{
	if (p) {
		N_Df(t_12_nvmeibt_dfprm_read,
			  "@STR@RV)model=@STR,"
			  "is_zeroing_using_test_and_write=@RV,"
			  "is_zeroing_mandatory=@RV,"
			  "is_secure_erase_after_disk_format=@RV,"
			  "is_using_nvme_trim_before_zero=@RV,"
			  "delete_ns_when_formatting=@RV,"
			  "reset_after_format=@RV,"
			  "skip_reformat=@RV,"
			  "ignore_metadata=@RV,"
			  "force_metadata=@RV,"
			  "force_512b=@RV,",
			prefix,
			index,
			p->model,
			p->is_zeroing_using_test_and_write,
			p->is_zeroing_mandatory,
			p->is_secure_erase_after_disk_format,
			p->is_using_nvme_trim_before_zero,
			p->delete_ns_when_formatting,
			p->reset_after_format,
			p->skip_reformat,
			p->ignore_metadata,
			p->force_metadata,
			p->force_512b);
	}
	else {
		N_Df(t_12_nvmeibt_dfprm_read_2, "Model not found");
	}
}

static void __disks_model_flow_params_print(const struct nvmeibt_disks_models_flow_params_t *all)
{
	int i;
	N_Tf(t_10_nvmeibt_dfprm_read, "Disk models flow params: n_models=@RV", all->n_models);
	__disk_flow_params_print("", &all->dflt, -1);
	for (i = 0; i < all->n_models; i++) {
		__disk_flow_params_print("", &all->dm[i], i);
	}
}

int nvmeibt_disk_flow_params_try_read_from_config_line(const char*config, int *n_matches)
{
	struct nvmeibt_disks_models_flow_params_t *all = &disk_model_flow_params;
	struct nvmeibt_disk_flow_params_t *p = &all->dflt;
	const char *cur_param_name;
	int cur_prefix_len;

	// Old generic params, regardless of disk model. Default values
	__try_parse_disk_param_v1_3_2(t_01_nvmeibt_dfprm_read, "is_zeroing_using_test_and_write"   , &p->is_zeroing_using_test_and_write);
	__try_parse_disk_param_v1_3_2(t_02_nvmeibt_dfprm_read, "is_using_nvme_trim_before_zero"    , &p->is_using_nvme_trim_before_zero);
	__try_parse_disk_param_v1_3_2(t_03_nvmeibt_dfprm_read, "is_secure_erase_after_disk_format" , &p->is_secure_erase_after_disk_format);
	__try_parse_disk_param_v1_3_2(t_04_nvmeibt_dfprm_read, "is_zeroing_mandatory"              , &p->is_zeroing_mandatory);
	__try_parse_disk_param_v1_3_2(t_05_nvmeibt_dfprm_read, "delete_ns_when_formatting"         , &p->delete_ns_when_formatting);
	__try_parse_disk_param_v1_3_2(t_06_nvmeibt_dfprm_read, "reset_after_format"                , &p->reset_after_format);
	__try_parse_disk_param_v1_3_2(t_07_xvmeibt_dfprm_read, "skip_reformat"		               , &p->skip_reformat);
	__try_parse_disk_param_v1_3_2(t_08_xvmeibt_dfprm_read, "ignore_metadata"	               , &p->ignore_metadata);
	__try_parse_disk_param_v1_3_2(t_09_xvmeibt_dfprm_read, "force_metadata"	                   , &p->force_metadata);
	__try_parse_disk_param_v1_3_2(t_10_xvmeibt_dfprm_read, "force_512b"		                   , &p->force_512b);

	if (1) {			// New, per disk model params
		// DISK_MODELS_PARAMS_V1_3_3:model,is_zeroing_using_test_and_write,is_zeroing_mandatory,is_secure_erase_after_disk_format
		// DISK_MODELS_PARAMS_V1_3_3=ABC,1,1,1,1
		cur_param_name = "DISK_MODELS_PARAMS_V1_3_3";
		cur_prefix_len = sizeof("DISK_MODELS_PARAMS_V1_3_3") - 1;
		if (!strncmp(config, cur_param_name, cur_prefix_len)) {
			if (config[cur_prefix_len] == ':') {
				N_Tf(t_15_nvmeibt_dfprm_read, "@STR header consumed", cur_param_name);
				(*n_matches)++;
				goto _out;
			} else /*if (config[cur_prefix_len] == '=')*/ {		// Will try to add disk model params
				if (all->n_models >= (int)(sizeof(all->dm) / sizeof(all->dm[0]))) {
					N_Wf(t_16_nvmeibt_dfprm_read, "Maximum limit of disk models reached. Ignorring");
					goto _out;
				}
				p = &all->dm[all->n_models];
			}
			p->delete_ns_when_formatting = 0;
			p->reset_after_format = 0;
			if (sscanf(config + cur_prefix_len + 1, "%40[^,],%d,%d,%d,%d",
					   p->model,
					   &p->is_zeroing_using_test_and_write,
					   &p->is_zeroing_mandatory,
					   &p->is_secure_erase_after_disk_format,
					   &p->is_using_nvme_trim_before_zero) == 5) {
				N_Tf(t_17_nvmeibt_dfprm_read, "Added disk model @RV: @STR", all->n_models, config);
				(*n_matches)+= 5;
				all->n_models++;
				goto _out;
			} else {
				goto _err;
			}
		}
		// DISK_MODELS_PARAMS_V1_3_4:model,is_zeroing_using_test_and_write,is_zeroing_mandatory,is_secure_erase_after_disk_format,is_using_nvme_trim_before_zero
		// DISK_MODELS_PARAMS_V1_3_4=ABC,1,1,1,1
		cur_param_name = "DISK_MODELS_PARAMS_V1_3_4";
		cur_prefix_len = sizeof("DISK_MODELS_PARAMS_V1_3_4") - 1;
		if (!strncmp(config, cur_param_name, cur_prefix_len)) {
			if (config[cur_prefix_len] == ':') {
				N_Tf(t_18_nvmeibt_dfprm_read, "@STR header consumed", cur_param_name);
				(*n_matches)++;
				goto _out;
			} else /*if (config[cur_prefix_len] == '=')*/ {		// Will try to add disk model params
				if (all->n_models >= (int)(sizeof(all->dm) / sizeof(all->dm[0]))) {
					N_Wf(t_19_nvmeibt_dfprm_read, "Maximum limit of disk models reached. Ignorring");
					goto _out;
				}
				p = &all->dm[all->n_models];
			}
			p->reset_after_format = 0;
			if (sscanf(config + cur_prefix_len + 1, "%40[^,],%d,%d,%d,%d,%d",
					   p->model,
					   &p->is_zeroing_using_test_and_write,
					   &p->is_zeroing_mandatory,
					   &p->is_secure_erase_after_disk_format,
					   &p->is_using_nvme_trim_before_zero,
					   &p->delete_ns_when_formatting) == 6) {
				N_Tf(t_20_nvmeibt_dfprm_read, "Added disk model @RV: @STR", all->n_models, config);
				(*n_matches)+= 6;
				all->n_models++;
				goto _out;
			} else {
				goto _err;
			}
		}
		// DISK_MODELS_PARAMS_V2_1:model,is_zeroing_using_test_and_write,is_zeroing_mandatory,is_secure_erase_after_disk_format,is_using_nvme_trim_before_zero,delete_ns_when_formatting,reset_after_format
		// DISK_MODELS_PARAMS_V2_1=ABC,1,1,1,1,1,1
		cur_param_name = "DISK_MODELS_PARAMS_V2_1";
		cur_prefix_len = sizeof("DISK_MODELS_PARAMS_V2_1") - 1;
		if (!strncmp(config, cur_param_name, cur_prefix_len)) {
			if (config[cur_prefix_len] == ':') {
				N_Tf(t_21_nvmeibt_dfprm_read, "@STR header consumed", cur_param_name);
				(*n_matches)++;
				goto _out;
			} else /*if (config[cur_prefix_len] == '=')*/ {		// Will try to add disk model params
				if (all->n_models >= (int)(sizeof(all->dm) / sizeof(all->dm[0]))) {
					N_Wf(t_22_nvmeibt_dfprm_read, "Maximum limit of disk models reached. Ignorring");
					goto _out;
				}
				p = &all->dm[all->n_models];
			}
			p->reset_after_format = 0;
			if (sscanf(config + cur_prefix_len + 1, "%40[^,],%d,%d,%d,%d,%d,%d,%d,%d,%d,%d",
					   p->model,
					   &p->is_zeroing_using_test_and_write,
					   &p->is_zeroing_mandatory,
					   &p->is_secure_erase_after_disk_format,
					   &p->is_using_nvme_trim_before_zero,
					   &p->delete_ns_when_formatting,
					   &p->reset_after_format,
					   &p->skip_reformat,
					   &p->ignore_metadata,
					   &p->force_metadata,
					   &p->force_512b) > 5) {
				N_Tf(t_23_nvmeibt_dfprm_read, "Added disk model @RV: @STR", all->n_models, config);
				(*n_matches)++;
				all->n_models++;
				goto _out;
			} else {
				goto _err;
			}
		}
	}

	return false;		/* Config line was not consumed by any of the vars */
_err:
	N_Wf(t_24_nvmeibt_dfprm_read, "cannot scan @STR", config);
_out:
	if (0) { __disks_model_flow_params_print(all); }		// Just for debug
	return true;		/* Config line was consumed by this function */
}

const struct nvmeibt_disk_flow_params_t *nvmeibt_disk_flow_params_get(const char *model, bool use_defaults)
{
	const struct nvmeibt_disks_models_flow_params_t *all = &disk_model_flow_params;
	const struct nvmeibt_disk_flow_params_t *rv = &all->dflt;
	int i = -1;

	if (!use_defaults)
		rv = NULL;

	N_Df(t_30_nvmeibt_dfprm_read, "searching model @STR", model);
	if (model == NULL)
		goto _out;

	for (i = 0; i < all->n_models; i++) {
		if (!strncmp(all->dm[i].model, model, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)) {
			rv = &all->dm[i];
			goto _out;
		}
	}
	i = -1;
	if (!use_defaults)
		goto _out;

	// Hack for Kioxia CM6, Micron 7300
	if (strncmp(model, "KCM6", 4)==0) {
		static struct nvmeibt_disk_flow_params_t kioxia_cm6;
		memcpy(&kioxia_cm6, &all->dflt, sizeof(kioxia_cm6));
		kioxia_cm6.force_metadata = 1;
		rv = &kioxia_cm6;
	}
	else if (strncmp(model, "Micron_7300", 11)==0 || strncmp(model, "Micron_9300", 11)==0) {
		static struct nvmeibt_disk_flow_params_t micron;
		memcpy(&micron, &all->dflt, sizeof(micron));
		micron.reset_after_format = 1;
		rv = &micron;
	}
	else if (strncmp(model, "INTEL SSDPF21Q", 14)==0 || strncmp(model, "INTEL SSDPE21K", 14)==0) {
		static struct nvmeibt_disk_flow_params_t optane2;
		memcpy(&optane2, &all->dflt, sizeof(optane2));
		optane2.force_512b = 1;
		rv = &optane2;
	}

_out:
	__disk_flow_params_print("selected", rv, i);
	return rv;
}

const struct nvmeibt_disk_flow_params_t *nvmeibt_disk_flow_params_get_next_model(const struct nvmeibt_disk_flow_params_t *arg)
{
	const struct nvmeibt_disks_models_flow_params_t *all = &disk_model_flow_params;
	int i = -1;

	if (!arg) {
		if (all->n_models > 0)
			return &all->dm[0];
		return NULL;
	}

	for (i = 0; i < (all->n_models - 1); i++) {
		if (arg == &all->dm[i]) {
			return &all->dm[i+1];
		}
	}
	return NULL;
}

int nvmeibt_disk_flow_params_set_model_params(const struct nvmeibt_disk_flow_params_t *arg)
{
	struct nvmeibt_disks_models_flow_params_t *all = &disk_model_flow_params;
	struct nvmeibt_disk_flow_params_t *p = NULL;
	int i = -1;

	if (strncmp(arg->model, all->dflt.model, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)==0) {
		p = &all->dflt;
	}
	for (i=0; !p && i<all->n_models; i++) {
		if (strncmp(arg->model, all->dm[i].model, NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE)==0) {
			p = &all->dm[i];
		}
	}

	if (!p) {	// Add new model
		if (all->n_models < (ARRAY_SIZE(all->dm)-1)) {
			p = &all->dm[all->n_models++];
		} else {
			return -2;
		}
	}

	N_Tf(t_1_set_model_params, "Set new flow params for disk model @STR", arg->model);
	memcpy(p, arg, sizeof(*p));
	return 0;
}

void nvmeibt_disk_flow_params_remove_model(const struct nvmeibt_disk_flow_params_t *arg)
{
	struct nvmeibt_disks_models_flow_params_t *all = &disk_model_flow_params;
	struct nvmeibt_disk_flow_params_t *p = (struct nvmeibt_disk_flow_params_t *)arg, *last = &all->dm[all->n_models-1];
	N_Tf(t_1_remove_model_params, "Remove flow params for disk model @STR", p->model);
	if (p != last)
		memcpy(p, last, sizeof(*p));
	memset(last, 0, sizeof(*last));		// Just for debug, clean the old data
	--all->n_models;
}

void nvmeibt_disk_flow_params_print(struct nvmeibt_Str *s)
{
	const struct nvmeibt_disks_models_flow_params_t *all = &disk_model_flow_params;
	const struct nvmeibt_disk_flow_params_t *p = &all->dflt;
	int i = -1;

	nvmeibt_Str_sprintf(s, "+ is_zeroing_using_test_and_write %d\n", p->is_zeroing_using_test_and_write);
	nvmeibt_Str_sprintf(s, "+ is_zeroing_mandatory %d\n", p->is_zeroing_mandatory);
	nvmeibt_Str_sprintf(s, "+ is_secure_erase_after_disk_format %d\n", p->is_secure_erase_after_disk_format);
	nvmeibt_Str_sprintf(s, "+ is_using_nvme_trim_before_zero %d\n", p->is_using_nvme_trim_before_zero);
	nvmeibt_Str_sprintf(s, "+ delete_ns_when_formatting %d\n", p->delete_ns_when_formatting);
	nvmeibt_Str_sprintf(s, "+ reset_after_format %d\n", p->reset_after_format);
	nvmeibt_Str_sprintf(s, "+ skip_reformat %d\n", p->skip_reformat);
	nvmeibt_Str_sprintf(s, "+ ignore_metadata %d\n", p->ignore_metadata);
	nvmeibt_Str_sprintf(s, "+ force_metadata %d\n", p->force_metadata);
	nvmeibt_Str_sprintf(s, "+ force_512b %d\n", p->force_512b);

	if (all->n_models > 0) {
		nvmeibt_Str_sprintf(s, "+ DISK_MODELS_PARAMS_V2_1:\n");

		for (i=0; i<all->n_models; i++) {
			p = &all->dm[i];
			nvmeibt_Str_sprintf(s, "+ DISK_MODELS_PARAMS_V2_1 %s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
			   p->model,
			   p->is_zeroing_using_test_and_write,
			   p->is_zeroing_mandatory,
			   p->is_secure_erase_after_disk_format,
			   p->is_using_nvme_trim_before_zero,
			   p->delete_ns_when_formatting,
			   p->reset_after_format,
			   p->skip_reformat,
			   p->ignore_metadata,
			   p->force_metadata,
			   p->force_512b);

		}
	}
}

void nvmeibt_disk_flow_params_reset_models_before_new_scan(void)
{
	disk_model_flow_params.n_models = 0;
}

/******************************************************************************/

struct nvmeibt_ldisk_id_for_srvr_cmd nvmeibt_local_disk_config_to_srvr_cmd_disk(const struct nvmeibt_local_disk_config *f)
{
	struct nvmeibt_ldisk_id_for_srvr_cmd ldisk;
	ldisk.ldisk_id = f->ldisk_id;							// Identical to nvmeibt_local_disk_uuid(ld)
	ldisk.vendor_id = (u16)(f->vendor);			// Identical to nvmeibt_local_disk_vendor_id(ld)
	strlcpy(ldisk.Model, f->smart_info.Model, sizeof(ldisk.Model));
	return ldisk;
}

void nvmeibt_local_disk_mark_is_specific_disk_report_req(const char *ldisk_id_str, unsigned int reappearing_counter)
{
	struct nvmeibt_local_disk				*local_disk = NULL;
	struct nvmeibt_ascii_uuid				ldisk_id;

	NFIN;
	nvmeibt_strlcpy(ldisk_id.str, ldisk_id_str, sizeof(ldisk_id.str));
	local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	if (!local_disk) {
		local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, nvmeibt_global_get_global()->stock_local_disks_hash_by_ldisk_id_str);
		if (!local_disk) {
			local_disk = nvmeibt_local_disk_get_local_disk_by_ldisk_id(&ldisk_id, nvmeibt_global_get_global()->formatting_local_disks_hash_by_ldisk_id_str);
			if (!local_disk) {
				N_Tf(65gvbsd, "ldisk_id=@STR not found... cannot update reappearing_counter", ldisk_id_str);
				goto out;
			}
		}
	}
	if (reappearing_counter > local_disk->reappearing_counter) {
		local_disk->reappearing_counter = reappearing_counter;	// Will hopefully trigger a new report
		N_Tf(ms94jd8, "disk=@STR update reappearing_counter to:@REAPPEARING_COUNTER", nvmeibt_local_disk_display(local_disk), reappearing_counter);
		NVMEIBT_GLOBAL_MARK_REPORT_TARGET_HAS_NEW_DATA(cbhsjs8);
	} else {
		N_Tf(ms947aq, "disk=@STR ignore new reappearing_counter (@REAPPEARING_COUNTER<=@REAPPEARING_COUNTER)", nvmeibt_local_disk_display(local_disk), reappearing_counter, local_disk->reappearing_counter);
	}
out:
	NFOUT;
}

void nvmeibt_local_disk_stop_all_activities_for_removed_local_disk(struct nvmeibt_local_disk *local_disk)
{
	struct nvmeibt_seg_active		*seg_active;
	int								n_local_disks;

	NFIN;
	n_local_disks = nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str);
	if (local_disk && local_disk->is_owned_by_nvmeibs_driver) {
		NVMEIB_HASH_FOREACH(seg_active, local_disk->seg_active_hash_by_uuid) {
			nvmeibt_seg_active_stop_all_recoveries_and_registrations(seg_active, 1, 0); // registrants I/O is irrelevant
			// The previous func can remove the local disk
			if (n_local_disks != nvmeib_hash_get_n_elements(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str)) {
				N_Tf(nh112aa, "local_disk=@STR was removed", nvmeibt_local_disk_display(local_disk));
				break;
			}
		}
	}
	NFOUT;
}

struct local_disk_wq_entry_wrapper_entry {
	struct nvmeibt_wq_entry 	wq_entry;
	struct nvmeibt_wq_entry 	*wrapped_entry;
	unsigned int 				*last_CHANGE_no;
};

static void local_disk_wq_entry_wrapper(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_wq_entry_wrapper_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct local_disk_wq_entry_wrapper_entry, wq_entry);

	N_Tf(trace_1_toma_local_disk_wq_entry_wrapper, "last_CHANGE_no=@INT", *entry->last_CHANGE_no);

	// Execute original entry;
	entry->wrapped_entry->wq = entry->wq_entry.wq;
	entry->wrapped_entry->execute(entry->wrapped_entry);

	// Modify entry to signal that inner entry "free will run separately - via completion of the original entry"
	entry->wrapped_entry = NULL;
	// Signal completion of entry wrapper.
	nvmeibt_toma_trigger_wakeup(NVMEIBT_TOMA_WAKEUP_TYPE_WQ, (void *) wq_entry);
	NFOUT;
}

static void local_disk_wq_entry_freer(struct nvmeibt_wq_entry *wq_entry)
{
	struct local_disk_wq_entry_wrapper_entry *entry;

	NFIN;

	entry = container_of(wq_entry, struct local_disk_wq_entry_wrapper_entry, wq_entry);

	// Check if inner WQ still exists (in case it did NOT execute), if so free it.
	if (entry->wrapped_entry) {
		entry->wrapped_entry->free(entry->wrapped_entry);
	}

	NNVMEIBT_BM_FREE(trace_toma_local_disk_wq_entry_freer, entry);

	NFOUT;
}

int nvmeibt_local_disk_specific_add_work(struct nvmeibt_wq *local_disk_wq, struct nvmeibt_wq_entry *e)
{
	return (local_disk_wq ? nvmeibt_wq_addw(local_disk_wq , e) : -1);
}

/**
 * Stop & drain the wq for the specific local_disk, if it exists, and returns
 * when the WQ is drained.
 *
 * @author max (11/8/18)
 *
 * @param ldisk_id
 * @param vendor_id
 *
 */
void nvmeibt_local_disk_stop_wq(struct nvmeibt_local_disk *local_disk)
{
	NFIN;
	if (local_disk) {
		if (local_disk->wq) {
			N_Tf(4cf8sk2, "ldisk=@STR wq=@PTR", nvmeibt_local_disk_UUID_str(local_disk), local_disk->wq);
#if 0	// Do not delete ldisk_wq. It is used by stock and non-stock
			// drain the wq of this disk, to avoid anything from attempting execution on it.
			nvmeibt_wq_drain(local_disk->wq);
			nvmeibt_wq_destroy(local_disk->wq);
			local_disk->wq = NULL;
			nvmeib_hash_delete_ascii_str(nvmeibt_global_get_global()->ldisks_wq_hash_by_ldisk_id_str, nvmeibt_local_disk_UUID_str(local_disk));
#endif	// #if 0	// Do not delete ldisk_wq. It is used by stock and non-stock
		} else {
			N_Wf(82njkaow, "ldisk=@STR wq=NULL", nvmeibt_local_disk_UUID_str(local_disk));
		}
	}
	NFOUT;
}

static struct local_disk_wq_entry_wrapper_entry* prepare_entry_with_ldisk_last_CHANGE_no(struct nvmeibt_local_disk *local_disk, struct nvmeibt_wq_entry *e)
{
	struct local_disk_wq_entry_wrapper_entry	*entry_wrapper = NULL;
	uint32_t									local_disk_last_CHANGE_no = local_disk->CHANGE_EVENT_counters.last_CHANGE_no;	// Freeze the value

	if (!e->last_CHANGE_no) {
		// We need to generate the version with which this execution starts, this is the first (and possibly only) link in a disk
		// specific WQ execution
		e->last_CHANGE_no = local_disk_last_CHANGE_no;
		N_Tf(s8l30l5, "Initializing execution of wq for disk=@STR with last_CHANGE_no=@INT",
			nvmeibt_local_disk_display(local_disk), e->last_CHANGE_no);
	}

	// We check the version with which the WQ is submitted, if the disk version is different, we can't execute it,
	// since this means that probably the WQ is a part of a chain that needs to be cut-off.
	if (e->last_CHANGE_no != local_disk_last_CHANGE_no) {
		N_Wf(bi4n6sg, "Unable to add work for specific disk=@STR because wq_last_CHANGE_no=@INT != last_CHANGE_no=@INT",
			 nvmeibt_local_disk_display(local_disk), e->last_CHANGE_no, local_disk_last_CHANGE_no);
		goto free_resources;
	}

	N_Tf(d84k50j, "Starting execution of wq for disk=@STR with last_CHANGE_no=@INT", nvmeibt_local_disk_display(local_disk), e->last_CHANGE_no);

	// Create wrapper entry that performs all the synchronization of local_disk object with other WQ's and
	// if possible executes the original wq_entry
	entry_wrapper = NNVMEIBT_BM_CALLOC(tvfhjwe, sizeof(*entry_wrapper));
	entry_wrapper->wrapped_entry = e;
	/* The active version is snapshotted - at THIS moment in time, while the current
	   can change, as a result of DISK_CHANGE events, hence if the current changes,
	   it will cause any entries that are inserted to the queue with the old version
	   to be drained, regardless if they were inserted before or after the DISK_CHANGE
	   event, as long as the event was not completed, when the event is completed -
	   only then new entries will be allowed to execute as again active will match
	   current, but new entries will be inserted only from the main thread hence
	   exactly after the DISK_CHANGE event completes.*/
	entry_wrapper->last_CHANGE_no = &local_disk->CHANGE_EVENT_counters.last_CHANGE_no;
	entry_wrapper->wq_entry.type = "LOCAL_DISK_WRAPPER";
	entry_wrapper->wq_entry.execute = local_disk_wq_entry_wrapper;
	entry_wrapper->wq_entry.free = local_disk_wq_entry_freer;

	goto out;

free_resources:
	NNVMEIBT_BM_FREE(vbus93o, entry_wrapper);

out:
	return entry_wrapper;
}

static void free_work_with_ldisk_last_CHANGE_no_entry(struct local_disk_wq_entry_wrapper_entry *entry_wrapper)
{
	NNVMEIBT_BM_FREE(cvvgs82, entry_wrapper);
}

int nvmeibt_local_disk_add_work_with_ldisk_last_CHANGE_no(struct nvmeibt_local_disk *local_disk, struct nvmeibt_wq_entry *e)
{
	int rv = 0;
	struct local_disk_wq_entry_wrapper_entry *entry_wrapper = NULL;

	NFIN;
	if (!local_disk) {
		N_Wf(d4nk1sp, "local_disk=NULL");
		rv = -1;
		goto out;
	}
	N_Tf(whcia9g, "Adding work for disk=@STR", nvmeibt_local_disk_display(local_disk));
	if (nvmeibt_local_disk_is_being_deleted(local_disk)) {
		N_Tf(4u2m9ak, "disk=@STR is_being_deleted. Skipping", nvmeibt_local_disk_display(local_disk));
		rv = -1;
		goto out;
	}
	if ((entry_wrapper = prepare_entry_with_ldisk_last_CHANGE_no(local_disk, e)) == NULL) {
		rv = -1;
		goto out;
	}
	rv = nvmeibt_local_disk_specific_add_work(local_disk->wq, &entry_wrapper->wq_entry);
	if (rv < 0)
		free_work_with_ldisk_last_CHANGE_no_entry(entry_wrapper);
out:
	NFOUT;
	return rv;
}

/******************** TEST helpers ****************************************/
#if defined(TOMA_SIMULATOR_SANDBOX)

void TEST_add_local_disk_to_hash(const char *ldisk_id_str, bool is_excluded, bool is_drive_write_error)
{
	struct nvmeibt_local_disk	*ld = calloc(1, sizeof(*ld));

	snprintf(ld->from_config.ldisk_id.str, sizeof(ld->from_config.ldisk_id.str), "%s", ldisk_id_str);
	ld->is_excluded = is_excluded;
	ld->is_drive_write_error = is_drive_write_error;
	ld->config_tag = 1;
	nvmeib_hash_add_ascii_str(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str,
							  ld->from_config.ldisk_id.str, ld);
}

void TEST_add_not_ready_local_disk_to_hash(const char *ldisk_id_str, int n_segments)
{
	struct nvmeibt_local_disk	*ld = calloc(1, sizeof(*ld));
	struct nvmeibt_disk			*disk = calloc(1, sizeof(*disk));

	snprintf(ld->from_config.ldisk_id.str, sizeof(ld->from_config.ldisk_id.str), "%s", ldisk_id_str);
	ld->config_tag = 1;
	// Not ready: metadata_gpt.is_valid defaults to 0 from calloc
	disk->n_segments = n_segments;
	ld->its_disk = disk;
	nvmeib_hash_add_ascii_str(nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str,
							  ld->from_config.ldisk_id.str, ld);
}

void TEST_remove_local_disk_from_hash(const char *ldisk_id_str)
{
	struct nvmeibt_local_disk	*ld = nvmeib_hash_delete_ascii_str(
			nvmeibt_global_get_global()->nvmesh_local_disks_hash_by_ldisk_id_str, ldisk_id_str);

	if (ld) {
		free(ld->its_disk);
		free(ld);
	}
}
#endif // #if defined(TOMA_SIMULATOR_SANDBOX)

