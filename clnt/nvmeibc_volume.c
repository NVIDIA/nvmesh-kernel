/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_volume.h"
#include "nvmeib_event.h"
#include "nvmeibc_defs.h"
#include "nvmeibc_msgs_shared.h"
#include "nvmeibc_channel.h"
#include "nvmeibc_ib_admin_channel.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_main.h"
#include "nvmeibc_nvme.h"
#include "nvmeibs_msgs_shared.h"
#include "nvmeibs_main.h"
#include "nvmeib_stats.h"
#include "nvmeib_nvme.h"
#include "nvmeibc_targets.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "block/nvmeibc_block_api_os.h"					// Get/Put os_api during force detach
//#define CONFIG_NVMEIB_DEBUG 1
//#define DEBUG 1
#include "nvmeib_utils.h"
#include "management_utils_common/nvmeibc_management_volume_conf_checks.h"
#include "block/controlpath/nvmeibc_b_cp_cpu_masks.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"
#include "utils/nvmeib_jdr/nvmeib_jdr.h"

#define __NFIN NFINS(volume->hdr.devname)
#define __NFOUT NFOUTS(volume->hdr.devname)

struct volume_workq {
	struct workqe_struct work;
	struct nvmeibc_volume *volume;
};

static struct nvmeibc_disk_id *find_disk_from_block(const struct nvmeibc_cinst_params_main *p,
	struct nvmeibc_disk *disk_inp, struct nvmeibc_block_device *block_dev)
{
	struct list_head *disks;
	struct nvmeibc_disk_id *rv = NULL;
	struct list_head *volumes;
	struct nvmeibc_volume *vol;
	struct nvmeibc_disk_id *disk;
	bool found = false;

	NFIN;
	volumes = nvmeibc_get_volumes(p);
	list_for_each_entry(vol, volumes, link) {
		if (vol->block_dev == block_dev) {
			found = true;
			break;
		}
	}

	if (!found) {
		_NT(trace_volume_find_disk_from_block, "Volume of block not found");
		rv = NULL;
		goto out;
	}

	disks = &vol->info.disks;
	found = false;
	list_for_each_entry(disk, disks, link)
		if (disk->disk == disk_inp) {
			found = true;
			break;
		}

	if (!found) {
		_NT(error_volume_find_disk_from_block, "@DISK_NAME not found in volume", disk_inp->name);
		rv = NULL;
		goto out;
	}

	rv = disk;

out:
	NFOUT;
	return rv;
}

static struct nvmeibc_disk_id *find_disk(struct nvmeibc_volume *volume,
	const char *disk_name)
{
	struct list_head *disks = &volume->info.disks;
	struct nvmeibc_disk_id *disk;
	bool found = false;

	__NFIN;
	_ND(trace_volume_find_disk, "Looking for disk @DISK_NAME", disk_name);
	list_for_each_entry(disk, disks, link)
		if (!memcmp(disk->name, disk_name, sizeof(disk->name))) {
			found = true;
			break;
		}
	__NFOUT;
	return found ? disk : NULL;
}

static struct nvmeibc_disk_id *disk_id_create(struct nvmeibc_volume *volume,
					      const char *disk_name,
					      struct nvmeibc_target *target,
					      int attachment_ver)
{
	struct nvmeibc_disk_id *disk_id;
	ssize_t disk_id_size = sizeof(*disk_id);
	__NFIN;

	_ND(trace_volume_disk_id_create, "Creating disk @DISK_NAME", disk_name);
	if (!(disk_id = kzalloc(disk_id_size, GFP_KERNEL))) {
		_NE(error_volume_disk_id_create, DMESG_PREFIX("@DEV_NAME") ": @DISK_NAME: Fail to allocate admin remote nic", volume->hdr.devname, disk_name);
		goto out;
	}
	strlcpy(disk_id->name, disk_name, sizeof(disk_id->name));
	disk_id->volume = volume;
	disk_id->attachment_version = attachment_ver;
	disk_id->target = target;
	list_add_tail(&disk_id->link, &volume->info.disks);
	list_add(&disk_id->dlink, &target->disks);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	disk_id->v_disk_stats = nvmeib_io_stats_create_traced(volume->hdr.devname, VERB_RW_T_BITMASK, NVMEIBC_SECTOR_SIZE);
#endif
	disk_id->create_jiff = jiffies;
out:
	__NFOUT;
	return disk_id;
}

void nvmeibc_volume_disks_stats_clear(const struct nvmeibc_volume *volume, const int which)
{
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	struct nvmeibc_disk_id *d;
	list_for_each_entry(d, &volume->info.disks, link)
		nvmeib_io_stats_clear(d->v_disk_stats, which);
#else
	(void)volume;
	(void)which;
#endif
}

void nvmeibc_volume_trace_stats(const struct nvmeibc_volume *volume)
{
	nvmeibc_block_trace_stats(volume->block_dev, true /* diff_only */);
}

static bool handle_disk_reappear(struct nvmeibc_disk_id *disk_id,
				 struct nvmeibc_target *new_target,
				 int attachment_ver)
{	// Remove from old target and add to new target update disk_reappeared flag
	if (disk_id->attachment_version > attachment_ver) {
		_ND(war_old_disk_reapper_msg,
		    "received old disk  @DISK_NAME to target connection @NODE_ID_STR to @NODE_ID_STR",
		    disk_id->name, disk_id->target->node_id,
		    new_target->node_id);
		return false;
	}

	if (!disk_id->target){
		_ND(trace_volume_handle_disk_reappear, "new disk @DISK_NAME setting target to @NODE_ID_STR", disk_id->name, new_target->node_id);
	} else if (disk_id->target != new_target) {
		_ND(trace_1_volume_handle_disk_reappear, "disk @DISK_NAME reappear from @NODE_ID_STR to @NODE_ID_STR", disk_id->name, disk_id->target->node_id, new_target->node_id);
	} else {
		return false;
	}


	list_del(&disk_id->dlink);
	disk_id->target = new_target;
	list_add(&disk_id->dlink, &new_target->disks);
	disk_id->attachment_version = attachment_ver;
	return true;
}

#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
static void nvmeibc_volume_disk_stats_destroy(struct nvmeibc_disk_id *disk, struct proc_dir_entry *disks_dir);
#endif

static void disk_id_destroy(struct nvmeibc_disk_id *disk_id)
{
	_ND(trace_volume_disk_id_destroy, "destroy disk_id: name=@DISK_NAME, @HDR_UUID, num_ranges=@NUM_RANGES", disk_id->name, disk_id->volume->hdr.uuid, disk_id->num_ranges);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	if (disk_id->proc_dir) {	// Destroy disk volume stats proc
		nvmeibc_volume_disk_stats_destroy(disk_id, disk_id->volume->disks_dir);
	}

	nvmeib_io_stats_free(disk_id->v_disk_stats);

#endif
	kfree(disk_id);
}

static int get_real_disk(const struct nvmeibc_cinst_params_main *p, struct nvmeibc_disk_id *disk_id, struct list_head *arnics, int num_ranges, const char *node_id)
{
	const struct nvmeibc_cinst_params_core *pcore = nvmeibc_isnt_params_main2core(p);
	struct list_head *disks = nvmeibc_get_disks(p);
	struct nvmeibc_disk *disk = NULL;
	bool found = false;
	int rv = 0;

	NFIN;
	list_for_each_entry(disk, disks, link) {
		if (!strncmp(disk_id->name, disk->name, sizeof(disk->name))) {
			_NT(t_01_vol_get_real_disk, "@DEV_NAME_FULL: Add volume to disk @DISK_NAME(@DISK)", disk_id->volume->full_name, disk_id->name, disk);
			nvmeibc_disk_add_volume(disk, disk_id, num_ranges);
			nvmeibc_disk_set_next_config(disk_id, node_id);
			found = true;
			goto out;
		}
	}
	if (!found) {
		disk_id->new_disk = 1;
		_NT(t_02_vol_get_real_disk, "@DEV_NAME_FULL: Add volume to disk @DISK_NAME (new)", disk_id->volume->full_name, disk_id->name);
		if (!(rv = nvmeibc_disk_create(pcore, disk_id, arnics, num_ranges, node_id))) {
			rv = 1; /*That is, the caller must wait for end of discovery*/
			//list_sort(NULL, arnics, arnic_score_cmp);
		}
	}

out:
	NFOUT;
	return rv;
}

static struct list_head *__get_vols_list(const struct nvmeibc_cinst_params_main *p, enum nvmeibc_config_volume_type _type)
{
	if (__is_bmp_included_in(UNKNOWN_ILLEGAL, _type)) return NULL; // Return NULL to look at all lists
	return nvmeibc_get_volumes(p);
}

/* Returns a nvmeibc_volume object by the volume devname */
struct nvmeibc_volume *nvmeibc_volume_get_by_name(const struct nvmeibc_cinst_params_main *p,
				const char *name, enum nvmeibc_config_volume_type type)
{
	struct nvmeibc_volume *s = NULL;
	struct list_head *volumes = __get_vols_list(p, type);
	if (unlikely(!volumes)) {	/* If unknown, search all lists */
		if ((s=nvmeibc_volume_get_by_name(p, name, NORMAL_VOLUME ))!=NULL) return s;
	} else {
		list_for_each_entry(s, volumes, link)
			if (!strncmp(name, s->hdr.devname, sizeof(s->hdr.devname)))
				return s;
	}
	return NULL;
}

/* Returns a nvmeibc_volume object by the volume uuid */
struct nvmeibc_volume *nvmeibc_volume_get_by_uuid(const struct nvmeibc_cinst_params_main *p,
				const char *uuid, enum nvmeibc_config_volume_type type)
{
	struct nvmeibc_volume *s = NULL;
	struct list_head *volumes = __get_vols_list(p, type);
	if (unlikely(!volumes)) {	/* If unknown, search all lists */
		if ((s=nvmeibc_volume_get_by_uuid(p, uuid, NORMAL_VOLUME ))!=NULL) return s;
	} else {
		list_for_each_entry(s, volumes, link)
			if (!strncmp(uuid, s->hdr.uuid, sizeof(s->hdr.uuid)))
				return s;
	}
	return NULL;
}

static int __disconnect_disk_from_volume(struct nvmeibc_disk_id *disk_id,
	const char *devname, bool block)
{
	bool need_block;
	const struct nvmeibc_disk *disk = disk_id->disk;

	NFIN;
	nvmeibc_target_remove_disk_id(disk_id);
	if (disk) {
		_NI(trace_volume_disconnect_disk_from_volume,
		    "@EVENT_TAG TRC Disconnection disk=@DISK_NAME[#rng=@RNG], bdev=@DEV_NAME[#rng=@RNG]",
		    EV_DISK_DISCONNECT_VOLUME(), disk->name, disk->n_ranges, devname, disk_id->num_ranges);
		list_del(&disk_id->link);
		need_block = nvmeibc_disk_remove(disk_id, block);
		if (!need_block) {
			disk_id_destroy(disk_id);
		}
	} else { // Daniel: Not sure this can happen
		_NT(error_volume_disconnect_disk_from_volume, "nvmeibc bug?=@DISK_NAME, #rng=@RNG", disk_id->name, disk_id->num_ranges);
		list_del(&disk_id->link);
		disk_id_destroy(disk_id);
		need_block = false;
	}
	NFOUT;
	return need_block;
}

static inline void __set_status(struct nvmeibc_volume *volume, enum nvmeibc_volume_status stat)
{
	unsigned long flags;
	nvmeibc_assert_on_main_wq(volume->p);	// Set status is always done from main-wq. Accessing the status can be from disk-wq as well
	_NT(t_03_vol_get_real_disk, "@EVENT_TAG @DEV_NAME: set status @INT->@INT (@STR->@STR); attachment_version=@INT",
	    EV_VOL_STATUS(), volume->hdr.devname, volume->status, stat, nvmeibc_volume_status_str(volume->status),
	    nvmeibc_volume_status_str(stat), volume->hdr.attachment_version);
	nvmeibc_volume_get(volume, &flags);
	volume->status = stat;
	nvmeibc_volume_put(volume, &flags);
};

/* Inverse of attach. set in_failure==true if trying to detach due to attach failure */
static void nvmeibc_volume_detach(struct nvmeibc_volume *volume, const struct nvmeibc_vol_detach_cmd how, struct nvmeibc_multi_completion *on_finish);

/* Return the volume's busy status */
int nvmeibc_volume_try_detach(struct nvmeibc_volume *volume, struct nvmeibc_vol_detach_cmd how, struct nvmeibc_multi_completion *on_finish)
{
	int rv;
	_NI(trace_volume_nvmeibc_volume_try_detach, "@NDU " DMESG_PREFIX("@DEV_NAME") ": Detach started. Flags = {F=@BOOL_YN, R=@BOOL_YN, A=@BOOL_YN}", 0, volume->hdr.devname, how.force, how.recov, how.abandon);
	rv = nvmeibc_block_try_detach(volume->block_dev, how);
	if (rv == 0) {
		how.err_attach = false;
		nvmeibc_volume_detach(volume, how, on_finish); /* 'volume' gets kfree() */
	}
	return rv;
}

/* Returns the volume's OS api */
static struct nvmeibc_os_api *volume_get_os_api(struct nvmeibc_volume *volume)
{
	return nvmeibc_block_get_os_api(volume->block_dev);
}

/* Create or update a volume according to incomming MCS message with new config
 * Also backwards compatibility for old config-fs configuration
 * is_update : indicate whether this is the creation of a new volume or an
 *   update to existing one.
 */
static int __volume_create_from_configuration(
	struct nvmeibc_volume *volume,
	const struct nvmeib_mgmt_to_client_volume_configuration *conf,
	bool is_update)
{
	const struct nvmeibc_cinst_params_main *p = volume->p;
	struct nvmeibc_disk_id *current_disk = NULL;
	int rv = 0, i, j, n_wait_disks;
	struct list_head disks_wait_list;
	struct nvmeibc_target *target = NULL;
	int attachment_ver = conf->attachmentsVersion;

	__NFIN;
	INIT_LIST_HEAD(&disks_wait_list);
	__set_status(volume, (is_update ? NVS_UPDATING : NVS_ATTACHING_STARTED));

	// For each target find or create it's local image (nvmeibc_target)
	for (i = 0, n_wait_disks = 0; i < conf->n_targets; i++) {
		const struct nvmeibc_target_conf *target_conf = &conf->targets[i];
		const char *disk_node_id = target_conf->node_id;
		// Target by target -> update all nics, then update all disks and
		// ranges at once (existing drives for this target will be updated within)
		nvmeibc_target_update_or_create(p, target_conf, &target);
		if (!target){
			_NE(t01vcfcs, DMESG_PREFIX() ": Out of memory, for target @NODE_ID_STR", disk_node_id);
			rv = -ENOMEM;
			goto no_mem;
		}

		for (j = 0; j < target_conf->n_disks; j++) {
			bool disk_reappeared;
			const struct nvmeibc_disk_conf *cur_disk = &target_conf->disks[j];
			const char *disk_id = cur_disk->diskID;
			_ND(t02vcfcs, "disk=@DISK_INT disk_id='@DISK_ID_STR'.", i, disk_id);
			if (!(current_disk = find_disk(volume, disk_id))) {
				current_disk = disk_id_create(volume, disk_id, target, attachment_ver);
				if (!current_disk) {
					_NE(t03vcfcs, DMESG_PREFIX() ": Out of memory, for disk @DISK_ID_STR", disk_id);
					rv = -ENOMEM;
					goto no_mem;
				}
			}
			_ND(t04vcfcs, "Disk @DISK_NAME. node_id=@NODE_ID_STR", current_disk->name, disk_node_id);
			disk_reappeared = handle_disk_reappear(current_disk, target, attachment_ver);
			if (!current_disk->disk) {
				_NT(t05vcfcs, "Trying to connect/use to disk @DISK_ID_STR", disk_id);
				rv = get_real_disk(p, current_disk, &target->arnics,
								   current_disk->num_ranges, disk_node_id);
				if (rv == -ENOMEM) {
					goto no_mem;
				} else if (rv < 0) {
					_NT(t06vcfcs, "Fail to create new disk (@DISK_ID_STR), the disk is created paused and rediscover will be started shortly ",disk_id);
				} else if (rv > 0) {
					WARN_ON(current_disk->disk == NULL);
					n_wait_disks++;
					list_add(&current_disk->disk_wl_link, &disks_wait_list);
					rv = 0;
					continue;
				}
			} else if (disk_reappeared) { // Only if this disk moved to a new node do we need to update the disk_id (if the target updated it's arnics we already updated this disk)
				nvmeibc_disk_set_next_config(current_disk, disk_node_id);
			}
			if (unlikely(!is_update)) {
				BUG_ON(current_disk->num_ranges != 0);
			}
		}
	}
	_ND(t07vcfcs, "n_wait_disks=@N_WAIT_DISKS", n_wait_disks);
	while ((current_disk = list_first_entry_or_null(&disks_wait_list,
									struct nvmeibc_disk_id, disk_wl_link))) {
		list_del(&current_disk->disk_wl_link);
		if ((rv = nvmeibc_disk_wait_for_discover(current_disk)) < 0) {
			_NT(t08vcfcs, "Fail to discover disk @DISK_NAME, the disk is created paused and rediscover will start shortly", current_disk->disk->name);
		}
	}
	rv = __check_striping_length_and_chunk(nvmeibc_isnt_params_main2blk(p)->binje, conf->volumes);

	/* No need to clean stack 'disks_wait_list' & current_disk->disk_wl_link */
no_mem:
	if (likely(is_update)) {
		if (rv<0) {
			/* Update failed, fallback to previous attached state*/
			__set_status(volume, NVS_ATTACHED);
		}
	}
	__NFOUT;
	return rv;
}

static int count_disks(struct nvmeibc_volume *volume)
{
	struct nvmeibc_disk_id *d;
	int n_disks = 0;
	list_for_each_entry(d, &volume->info.disks, link)
		++n_disks;
	return n_disks;
}

#ifndef get_disk_uptime
#define get_disk_uptime(disk) (jiffies - (disk)->create_jiff)
#endif

/* PER VOLUME DISK STATS */
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
static ssize_t v_disk_stats_fill_buf_json(void *priv, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibc_disk_id *disk = priv;
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	ssize_t count  = 0, indent = 0;
	NFIN;
	count += jops->start_obj(buf + count, len - count, NULL, indent++);
	count += jops->data_str(buf + count, len - count, "uuid", disk->name, !JSON_LAST_ELEM, indent);
	count += nvmeib_io_stats_to_json(disk->v_disk_stats, buf+count, len-count, get_disk_uptime(disk), jops, indent, true);
	count += jops->end_obj(buf + count, len - count, JSON_LAST_ELEM, --indent);
	NFOUT;
	return count;
#undef BUF_ADD
}

static void txt_append_dot1(struct nvmeib_txt *txt, const char *name, s64 value)
{
	nvmeib_txt_append(txt, "%s=%lld.%d\n", name, value/10, (int)(value%10));
}

static ssize_t v_disk_stats_fill_buf(void *priv, char *buf, size_t len)
{
	struct nvmeib_txt txt = nvmeib_txt_make((struct charvec){.base=buf,.len=len});
	const struct nvmeibc_disk_id *disk = priv;
	struct nvmeib_io_stats *stats = disk->v_disk_stats;
	struct nvmeib_io_counters c;
	const ulong cur_time = get_disk_uptime(disk);
	int factor = nvmeib_public_tsc_khz() / 10000;
	int factor2 = factor*factor >> 8;
	/* Print in units of micro-seconds. Latency/factor is in units of 1/10^7 of
	   a second and we also do /10 when printing. so total of 1/10^6 sec */
	NFIN;
	nvmeib_txt_append(&txt, "uptime=%ld.%03ld\n", cur_time/HZ, 1000*(cur_time%HZ)/HZ);
	memset(&c, 0, sizeof c);
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_READ, -1 /* All sizes */, &c);
	nvmeib_txt_append(&txt, "read_ops=%lld\n", c.total_ops);
	nvmeib_txt_append(&txt, "read_sub_block_ops=%lld\n", c.total_sub_block);
	nvmeib_txt_append(&txt, "read_size=%lld\n", c.total_size);
	txt_append_dot1(&txt, "read_latency", c.total_latency/factor);
	txt_append_dot1(&txt, "read_latency^2", c.total_latency_sqr/factor2);
	txt_append_dot1(&txt, "read_worst_latency", c.worst_latency/factor);

	memset(&c, 0, sizeof c);
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_WRITE, -1 /* All sizes */, &c);
	nvmeib_txt_append(&txt, "write_ops=%lld\n", c.total_ops);
	nvmeib_txt_append(&txt, "write_sub_block_ops=%lld\n", c.total_sub_block);
	nvmeib_txt_append(&txt, "write_size=%lld\n", c.total_size);
	txt_append_dot1(&txt, "write_latency", c.total_latency/factor);
	txt_append_dot1(&txt, "write_latency^2", c.total_latency_sqr/factor2);
	txt_append_dot1(&txt, "write_worst_latency", c.worst_latency/factor);

	memset(&c, 0, sizeof c);
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_DISCARD, -1 /* All sizes */, &c);
	nvmeib_txt_append(&txt, "trim_ops=%lld\n", c.total_ops);
	nvmeib_txt_append(&txt, "trim_sub_block_ops=%lld\n", c.total_sub_block);
	nvmeib_txt_append(&txt, "trim_size=%lld\n", c.total_size);
	txt_append_dot1(&txt, "trim_latency", c.total_latency/factor);
	txt_append_dot1(&txt, "trim_latency^2", c.total_latency_sqr/factor2);
	txt_append_dot1(&txt, "trim_worst_latency", c.worst_latency/factor);

	NFOUT;
	return txt.impl.total;
}

static void nvmeibc_volume_disk_stats_destroy(struct nvmeibc_disk_id *disk, struct proc_dir_entry *disks_dir)
{
	if (disk->proc_ent_stats_json) {
		nvmeib_public_proc_remove(disk->proc_ent_stats_json);
		disk->proc_ent_stats_json = NULL;
	}
	if (disk->proc_ent_stats) {
		nvmeib_public_proc_remove(disk->proc_ent_stats);
		disk->proc_ent_stats = NULL;
	}
	if (disk->proc_dir) {
		remove_proc_entry(disk->name, disks_dir);
		disk->proc_dir = NULL;
	}
}

void nvmeibc_volume_disks_stats_destroy(struct nvmeibc_volume *volume)
{
	struct proc_dir_entry *disks_dir = volume->disks_dir;
	if (disks_dir) {	// If root isn't created no need to delete per disk
		struct nvmeibc_disk_id *d;
		list_for_each_entry(d, &volume->info.disks, link) {
			if (d->proc_dir)	// Destroy
				nvmeibc_volume_disk_stats_destroy(d, disks_dir);
		}
		nvmeibc_block_device_disk_stats_remove(volume->block_dev);
	}
	volume->disks_dir = NULL;
}

static int nvmeibc_volume_create_disk_proc(struct proc_dir_entry *vol_dir, struct nvmeibc_volume *volume)
{
	if (volume->disks_dir) {	// Folder already exists
		return 0;
	}
	volume->disks_dir = proc_mkdir(PROCFS_DISKS_STR, vol_dir);
	return (int)(volume->disks_dir == NULL);
}

int nvmeibc_volume_disk_stats_create(struct proc_dir_entry *vol_dir, struct nvmeibc_volume *volume)
{
	int rv = 0;
	struct nvmeibc_disk_id *d;
	struct proc_dir_entry *disks_dir;
	if (nvmeibc_volume_create_disk_proc(vol_dir, volume) != 0) {	// Volume disks folder not created
		return -ENOMEM;
	}
	disks_dir = volume->disks_dir;
	list_for_each_entry(d, &volume->info.disks, link) {
		if (d->proc_dir != NULL) {	// Disk folder exists
			if (d->proc_ent_stats != NULL) {	// Disk file exists
				continue;
			}
			BUG();
		}
		if (!(d->proc_dir = proc_mkdir(d->name, disks_dir))) {
			_NE(error_volume_disk_stats_create, "Fail to create volume @DEV_NAME disks directory", volume->hdr.devname);
			rv = -ENOMEM;
			break;
		}
		if (!(d->proc_ent_stats = nvmeib_public_proc_create("iostats", d->proc_dir, v_disk_stats_fill_buf, NULL, d))) {
			_NE(error_1_volume_disk_stats_create, "Fail to create volume @DEV_NAME disk @DISK_NAME stats proc entry", volume->hdr.devname, d->name);
			rv = -ENOMEM;
			break;
		}
		if (!(d->proc_ent_stats_json = nvmeib_public_proc_create("iostats.json", d->proc_dir, v_disk_stats_fill_buf_json, NULL, d))) {
			_NE(error_2_volume_disk_stats_create, "Fail to create volume @DEV_NAME disk @DISK_NAME stats.json proc entry", volume->hdr.devname, d->name);
			rv = -ENOMEM;
			break;
		}

	}
	if (rv) {	// UNDO the above, call destroy
		nvmeibc_volume_disks_stats_destroy(volume);
	}
	return rv;
}
#endif // defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)

static int __setup_block_device_from_volume(struct nvmeibc_volume *volume, const struct nvmeib_mgmt_to_client_volume_configuration *msg, bool modify)
{
	struct nvmeibc_volume_conf* conf = msg->volumes;
	const struct nvmeibc_volume_header *hdr = &volume->hdr;
	int rv = 0;

	if (nvmeibc_managment_does_vol_need_disks(hdr)) {
		/*
		const int has_rt_disks = (count_disks(volume) != 0);
		const int has_cfg_disks = nvmeibc_management_does_vol_have_disks(msg);
		BUG_ON(has_rt_disks != has_cfg_disks);

		It looks like management(simulator) may send configuration with 0 disks in case of update, but it is forbidden in case of attach.
		There is a need to verify this with Daniel S.
		*/
		if ((rv = count_disks(volume)) <= 0) {
			_NE_to_user(t_1s_volattach, DMESG_PREFIX("@DEV_NAME"), "Volume got Illegal configuration from management (version=@C_VOL_VER) with @RV disks. Attach will fail. Error code: 1053.", hdr->devname, hdr->version, rv);
			rv = -EINVAL;
			goto out;
		}
	}

	if (modify) rv = nvmeibc_block_reconf(conf, volume);
	else 		rv = nvmeibc_block_init(  conf, volume);
	/* unfreeze_disks(volume); */

	if (!rv) {
		struct nvmeibc_disk_id *disk_id;
		struct nvmeibc_disk *dd;
		/* After we have successfully manage to create the volume's block device we scan all new disks that the volume created and if any of them is offline we call its rediscovery process. Previous disks that the volume needs and are offline are running their rediscovery processes independently. */
		list_for_each_entry(disk_id, &volume->info.disks, link) {
			if (!disk_id->new_disk)
				continue;
			dd = disk_id->disk;
			if (!dd)
				continue; /* Disks with zero ranges are legal */
			if (atomic_read(&dd->paused)) {
				_NT(t_1q_volattach, "Disk @DISK_NAME is offline therefore disk release is called", dd->name);
				if ((rv = nvmeibc_disk_start_release(dd, NVMEIBC_DISK_RELEASE_STARTED_OFFLINE)) < 0)
					goto out;
			}
		}
	} else {
		if (modify) {
			_NE(t_1r_volattach, DMESG_PREFIX("@DEV_NAME") ": reconfiguration failed=@RV, rolling back to prev config", hdr->devname, rv);
		} else {
			_NE_to_user(t_1t_volattach, DMESG_PREFIX("@DEV_NAME"), "Volume attach failed with result @RV, IO will not be possible. Error code: 1054.", hdr->devname, rv);
		}
	}
out:
	return rv;
}

static int __reply_to_sub_volume_request(struct nvmeibc_volume *volume, const bool add, int rv)
{
	u32 reply_status;
	if (add) {
		if (rv) reply_status = NVMEIB_C_TO_M_VOLUME_ALIAS_CREATE_FAILED;
		else    reply_status = NVMEIB_C_TO_M_VOLUME_ALIAS_CREATED;
	} else {
		if (!rv) reply_status = NVMEIB_C_TO_M_VOLUME_ALIAS_DELETED;
		else {
			if (rv == -EBUSY) reply_status = NVMEIB_C_TO_M_VOLUME_ACK_BUSY;
			else              reply_status = NVMEIB_C_TO_M_VOLUME_ALIAS_DELETE_FAILED;
		}
	}
	return nvmeibc_cc_api_sub_vol_notification(volume, reply_status);
}

int nvmeibc_volume_ioctl_config(const struct nvmeibc_cinst_params_main *p, const char* cmd /*, int len*/)
{
	struct nvmeibc_volume *volume = NULL;
	char dev_name[NVMEIBC_BD_NAME_LEN];
	const char *end;
	int len, rv = -ENOENT, sub_vol_prefix_size = (sizeof(SUB_VOL_CMD)-1);
	bool is_sub_vol_request = false;

	/* Split cmd into name of the volume and command */
	end = my_strchrnul(cmd, '|');
	len = end-cmd;
	if ((!end)||(!*end)||((len+1)>NVMEIBC_BD_NAME_LEN)) {
		_NI(t_01vioctlconf, QA_BLOCK_PREFIX "Unrecognized command, len=@INT", len);
		goto _out;
	}
	memcpy(dev_name, cmd, len);
	dev_name[len] = 0;
	cmd = end+1;
	is_sub_vol_request = (!strncmp(cmd, SUB_VOL_CMD, sub_vol_prefix_size));			// Todo: Eventually clean this hack, not urgent

	if ((len==0) || ((len==1) && (dev_name[0]=='*'))) {							// All volumes - if name is empty or '*'
		rv = nvmeibc_block_qa_config(nvmeibc_isnt_params_main2blk(p), NULL, cmd); /* To all bdevs */
		goto _out;
	}
	volume = nvmeibc_volume_get_by_name(p, dev_name, NORMAL_VOLUME);
	if (!volume) {
		_NI(t_02vioctlconf, QA_BLOCK_PREFIX "@DEV_NAME: Volume does not exist", dev_name);
		if (is_sub_vol_request)
			rv = nvmeibc_cc_api_sub_vol_unknown(p, dev_name, false);
		goto _out;
	}
	nvmeibc_assert_on_main_wq(volume->p);	// Ioclts are handled on main-wq. So no need to lock volume as its status cannot change
	if ((volume->status != NVS_ATTACHED)||(!volume->block_dev)) {
		_NI(t_03vioctlconf, QA_BLOCK_PREFIX "@DEV_NAME: Volume is not attached yet", dev_name);
	} else {
		rv = nvmeibc_block_qa_config(nvmeibc_isnt_params_main2blk(p), volume->block_dev, cmd);
		if (is_sub_vol_request) {
			const bool add = (cmd[sub_vol_prefix_size] == 'a');				// Todo: Not urgent, encode this in 'rv'
			rv = __reply_to_sub_volume_request(volume, add, rv);
		}
	}
_out:
	return rv;
}

int nvmeibc_volume_is_ready_for_pause_cont(const struct nvmeibc_volume* v)
{
	switch (v->status) {						/* Note: Using case fallbacks*/
	case NVS_ATTACHING_STARTED:		return 0;	/* Just ignore it / as topology may not exist yet */
	case NVS_UPDATING:							/* In all cases below, bdev exists and can respond to pause/cont */
	case NVS_ATTACHING_HAVE_BDEV:
	case NVS_ATTACHED:
	case NVS_DETACHING: 			return 1;	/* Ready, Even when detaching, be able to receive disk pause. As long as bdev exists */
	case NVS_DETACHING_IO_DRAINED:
	default:						return -1;	/* Do nothing */
	}
}

int nvmeibc_volume_get_cpu_masks(struct nvmeibc_volume *volume, struct nvmeib_cpu_mask_info *mask_infos, int max_masks)
{
	if (!volume->block_dev)
		return 0;	// No masks

	return nvmeibc_block_get_cpu_masks(volume->block_dev, mask_infos, max_masks);
}

static void __ref_ids_verify_and_copy(struct nvmeibc_volume_header *hdr, const struct nvmeibc_volume_conf *conf)
{
	const int n_bytes = nvmeibc_volume_ext_blob_size(conf->attachment.n_ref_ids);
	int i = 0;
	for (i = 0; i < conf->attachment.n_ref_ids; i++) {
		_NT(t0envlu, "ref_id[@INT]=@STR", i, &conf->attachment.referenceIDs[i].val[0]);
	}
	memcpy(hdr->ext_blob.referenceIDs, conf->attachment.referenceIDs, n_bytes);	// Just copy the strings, Could have remove refID and add a differnt one and update messages were aggregated
	hdr->ext_blob.n_ref_ids = conf->attachment.n_ref_ids;
}

static int __update_only_volume_ref_ids(struct nvmeibc_volume_header *hdr, const struct nvmeibc_volume_conf *conf, int attachment_version, bool verbose)
{
	int rv = 0;

	unsigned long flags = 0;
	spin_lock_irqsave(&hdr->ext_blob_modify_guard, flags);

	hdr->attachment_version = attachment_version;
	hdr->attachment_version_per_volume = conf->attachment.version;
	if (verbose) {
		_NT(t0dnvlu, "@DEV_NAME: n_ref_ids @INT->@INT attach_ver{global=@INT, avpv=@INT64}", hdr->devname, hdr->ext_blob.n_ref_ids, conf->attachment.n_ref_ids, attachment_version, hdr->attachment_version_per_volume);
	}
	WARN((hdr->ext_blob.n_ref_ids < 0)||(conf->attachment.n_ref_ids < 0), "nvmeibc bug: n_refs %d->%d\n", hdr->ext_blob.n_ref_ids, conf->attachment.n_ref_ids);
	if (hdr->ext_blob.n_ref_ids == conf->attachment.n_ref_ids) {							// Identical amount of ref_ids
		if (conf->attachment.n_ref_ids > 0) {
			__ref_ids_verify_and_copy(hdr, conf);
		} else {																// Both empty, nothing to do
			BUG_ON(hdr->ext_blob.referenceIDs != NULL);							// Sanity Trap
		}
	} else if (hdr->ext_blob.n_ref_ids > conf->attachment.n_ref_ids) {			// Blob shrinks
		if (conf->attachment.n_ref_ids > 0) {
			__ref_ids_verify_and_copy(hdr, conf);
		} else {
			nvmeibc_volume_header_destroy(hdr, NVMEIBC_VOLUME_HEADER_DESTROY_REF_IDS);									// Clean all ref strings
		}
	} else {																	// Blob becomes bigger
		const int n_bytes = nvmeibc_volume_ext_blob_size(conf->attachment.n_ref_ids);
		BUG_ON(conf->attachment.n_ref_ids <= 0);
		nvmeibc_volume_header_destroy(hdr, NVMEIBC_VOLUME_HEADER_DESTROY_REF_IDS);
		hdr->ext_blob.referenceIDs = (struct nvmeibc_reference_id *)kmalloc(n_bytes, GFP_KERNEL);
		if (hdr->ext_blob.referenceIDs) {
			__ref_ids_verify_and_copy(hdr, conf);
		} else {
			rv = -ENOMEM;
		}
	}
	spin_unlock_irqrestore(&hdr->ext_blob_modify_guard, flags);
	return rv;
}

// Used also in setup_block_device
void nvmeibc_volume_header_create_from_msg(struct nvmeibc_volume_header *hdr,
										const struct nvmeibc_volume_conf *conf,
										int attachment_version, bool verbose)
{
	strlcpy(hdr->devname, conf->name, sizeof(hdr->devname));
	hdr->type = conf->type;
	strlcpy(hdr->uuid, conf->uuid, sizeof(hdr->uuid));
	hdr->version = conf->version;
	if (verbose) {
		_NT(t1dnvlu, "@DEV_NAME: n_ref_ids @INT->0 drop, avpv=@INT64", hdr->devname, hdr->ext_blob.n_ref_ids, conf->attachment.version);
	}
	nvmeibc_volume_header_init_0(hdr);								// We dont allow update, but clean allocation because volume update may fail and we need to revert this
	__update_only_volume_ref_ids(hdr, conf, attachment_version, verbose);
	hdr->last_sent_io_perm = NVMEIB_C_TO_M_IO_TYPE_PERMIT_NEVER;	// Worst possible
	nvmeibc_volume_attach_t_init_from_conf(&hdr->vat, conf);
}

static int nvmeibc_volume_update(struct nvmeibc_volume *volume,
				const struct nvmeib_mgmt_to_client_volume_configuration *msg)
{
	const struct nvmeibc_volume_conf *hdr = &msg->volumes[0];
	const char *devname = hdr->name;
	struct nvmeibc_volume_header prev_hdr = volume->hdr;
	const struct nvmeibc_volume *temp_volume = nvmeibc_volume_get_by_name(volume->p, devname, hdr->type);
	// const bool should_update_ref_ids = (msg->attachmentsVersion > volume->hdr.attachment_version);
	const bool should_update_ref_ids = (hdr->attachment.version > volume->hdr.attachment_version_per_volume);
	int rv = 0;
	NFIN;

	/* Note: devname != volume->devname.devname is legal. Volume renaming */
	if (temp_volume && temp_volume != volume) {
		_NE(t00nvlu, DMESG_PREFIX("@DEV_NAME") ": Volume rename to @DEV_NAME failed! Name already exits, @HDR_UUID", volume->hdr.devname, devname, temp_volume->hdr.uuid);
		rv = -EINVAL;
		goto out;
	}
	_NI(i01nvlu, "Volume @DEV_NAME is already attached, volume will update, take_ref_ids=@BOOL_YN", devname, should_update_ref_ids);
	if (volume->status >= NVS_DETACHING) {
		_NE(t02nvlu, DMESG_PREFIX("@DEV_NAME") ": cannot attach while it is being detached", devname);
		rv = -EINVAL;
		goto out;
	}
	if (prev_hdr.version > (int)hdr->version) {
		_NT(t04nvlu, "@DEV_NAME: rejecting old configuration @C_VOL_VER", devname, hdr->version);
		rv = -EPERM; /* Illegal to go back to an earlier configuration */
		goto out;
	}
	if ((prev_hdr.version == (int)hdr->version) && ((int)prev_hdr.type == hdr->type)) {
		_NT(t05nvlu, "@DEV_NAME: ignore existing configuration @C_VOL_VER, new_@RES_MOD_VER", devname, hdr->version, hdr->reservation.version);
		rv = -ERROR_VOL_UPDATE_ALREADY_LATEST;
		if (should_update_ref_ids) {
			if (__update_only_volume_ref_ids(&volume->hdr, hdr, msg->attachmentsVersion, true) < 0)
				rv = -ENOMEM;
		} else {
			_NT(t09nvlu, "@DEV_NAME: n_ref_ids @INT also ignorring, attachment version unchanged @INT", devname, volume->hdr.ext_blob.n_ref_ids, msg->attachmentsVersion);
		}
		goto out;
	}
	if (__volume_create_from_configuration(volume, msg, true)) {
		rv = -EINVAL;
		goto out;
	}
	nvmeibc_volume_header_create_from_msg(&volume->hdr, hdr, msg->attachmentsVersion, true);

	// Mistakenly overwrite the entire header. Rollback back the needed parts
	volume->hdr.last_sent_io_perm = prev_hdr.last_sent_io_perm;
	if (should_update_ref_ids) {
		_NT(t3dnvlu, "@DEV_NAME: n_ref_ids @INT->@INT apply, avpv=@INT64", volume->hdr.devname, prev_hdr.ext_blob.n_ref_ids, volume->hdr.ext_blob.n_ref_ids, volume->hdr.attachment_version_per_volume);
		nvmeibc_volume_header_destroy(&prev_hdr, NVMEIBC_VOLUME_HEADER_DESTROY_REF_IDS);		// Free previous ref_ids, not needed anymore
		prev_hdr.ext_blob = volume->hdr.ext_blob;
	} else {
		unsigned long flags = 0;

		//no need to lock here - we are on the main wq
		_NT(t9dnvlu, "@DEV_NAME: n_ref_ids @INT->@INT rollback, avpv=@INT64", volume->hdr.devname, volume->hdr.ext_blob.n_ref_ids, prev_hdr.ext_blob.n_ref_ids, prev_hdr.attachment_version_per_volume);

		spin_lock_irqsave(&volume->hdr.ext_blob_modify_guard, flags);
		{
			nvmeibc_volume_header_destroy(&volume->hdr, NVMEIBC_VOLUME_HEADER_DESTROY_REF_IDS);	// Roll back ref_ids we mistakenly took from 'msg'
			volume->hdr.attachment_version_per_volume = prev_hdr.attachment_version_per_volume;
			volume->hdr.ext_blob = prev_hdr.ext_blob;
		}
		spin_unlock_irqrestore(&volume->hdr.ext_blob_modify_guard, flags);
	}	// Here: all volume attachment (not config) is identical volume->hdr.attachment == prev_hdr.attachment;

	if (prev_hdr.type != volume->hdr.type)
		_NT(t06nvlu, "@DEV_NAME: type updated @HDR_TYPE-->@HDR_TYPE", devname, prev_hdr.type, volume->hdr.type);
	if ((rv = __setup_block_device_from_volume(volume, msg, true)) < 0) {
		_NE(t07nvlu, DMESG_PREFIX("@DEV_NAME") ": failed reconfigure from ver @C_VOL_VER to @C_VOL_VER. rv=@RV - rolling back", devname, prev_hdr.version, volume->hdr.version, rv);
		/* goto detach_volume - No!!!! this volume is attach and has IO detach attemp will lead to crash!!! */
		_NT(t2dnvlu, "@DEV_NAME: n_ref_ids @INT->@INT roll_back, update_fail", volume->hdr.devname, volume->hdr.ext_blob.n_ref_ids, prev_hdr.ext_blob.n_ref_ids);
		volume->hdr = prev_hdr;		// This is a rollback of configuration only, attachment stuff is already identical see above
	} else {
		BUG_ON(rv > 0);	// Code assumes all erros < 0, success : rv == 0
	}
	/* Important: The underlying block device might be rebooting into the
	   current configuration version, and will finish this asyncronously.
	   Anyways, we consider the attach completed. So attach is always syncronous */
	__set_status(volume, NVS_ATTACHED);
	if (!rv) {	// Sanity, Crash if updated to wrong version to prevent data corruption
		const int version_diff = (volume->hdr.version - prev_hdr.version);
		const bool invalid_update = (version_diff < 0) ||
									((version_diff == 0) && (prev_hdr.type == volume->hdr.type));	// Accept same configuration only to change attachment type (like recoverer -> visible volume)
		if (invalid_update) {
			_NE(t08nvlu, DMESG_PREFIX("@DEV_NAME") ": used wrong configuration @C_VOL_VER, prev=@C_VOL_VER, @HDR_TYPE", devname, volume->hdr.version, prev_hdr.version, volume->hdr.type);
			BUG_ON(invalid_update);
		}
	}
out:
	NFOUT;
	return rv;
}

int nvmeibc_volume_attach(const struct nvmeibc_cinst_params_main *p,
	const struct nvmeib_mgmt_to_client_volume_configuration *msg,
	struct nvmeib_pet_base_controller* io_pet_controller)
{
	struct nvmeibc_volume *volume;
	const struct nvmeibc_volume_conf *hdr = &msg->volumes[0];
	int rv = 0;
	const char *devname = hdr->name;
	const char *uuid = hdr->uuid;

	NFINS(devname);

	if ((volume = nvmeibc_volume_get_by_uuid(p, uuid, hdr->type))) {
		rv = nvmeibc_volume_update(volume, msg);
		goto out;
	} else if (nvmeibc_volume_get_by_name(p, devname, hdr->type)) {
		_NE(t_0b_vol_attach, DMESG_PREFIX("@DEV_NAME") ": Volume with that name already exists (different uuid)", devname);
		rv = -EINVAL;
		goto out;
	} else if (!(volume = kzalloc(sizeof(*volume), GFP_KERNEL))) {
		_NE(t_0c_vol_attach, DMESG_PREFIX("@DEV_NAME") ": Fail to allocate volume", devname);
		rv = -ENOMEM;
		goto out;
	}
	volume->p = p;
	volume->io_pet_controller = io_pet_controller;
	nvmeibc_volume_header_create_from_msg(&volume->hdr, hdr, msg->attachmentsVersion, true);

	spin_lock_init(&volume->spinlock);
	snprintf(volume->full_name, sizeof(volume->full_name), "%.*s-%.*s",
		(int)NVMEIB_HOST_NAME_LEN, nvmeibc_get_utsname_nodename(p),
		(int)NVMEIBC_BD_NAME_LEN, volume->hdr.devname);
	_NI(t_01_vol_attach, "Volume @DEV_NAME (@DEV_NAME_FULL) @C_VOL_VER, nchunks=@N_CHUNKS, @HDR_TYPE, cnt_@RES_MOD_VER, reservation_mode=@INT, preempt=@INT", volume->hdr.devname,
		volume->full_name, volume->hdr.version, msg->volumes->n_chunks,
		volume->hdr.type,
		volume->hdr.vat.res.version, volume->hdr.vat.res.mode, volume->hdr.vat.res.preempt);
	__set_status(volume, NVS_ATTACHING_STARTED);
	INIT_LIST_HEAD(&volume->info.disks);				// Exactly the list of volumes which use transport layer c_disks
	volume->info.retain_disks = false;

	/* Type specific initializations */
	if (nvmeibc_managment_does_vol_need_disks(   &volume->hdr)) { /*Todo*/ }
	/* Add volume to the driver internal lists */
	if ((rv = nvmeibc_add_volume(volume)) < 0) {
		_NE(t_03_vol_attach, DMESG_PREFIX("@DEV_NAME") ": nvmeibc is going down. attach canceled", devname);
		rv = -1;
		goto _free_volume_no_attach;
	}

	/* Now create the volume */
	{
		rv = __volume_create_from_configuration(volume, msg, false);
		switch (rv) {
		case 0: // success
			break;
		case -ENOMEM:
			_NT(t_04_vol_attach, "@DEV_NAME: __volume_create() failed - aborting", devname);
			goto _detach_newly_created_volume;
		case -ENODEV:
			_NT(t_05_vol_attach, "@DEV_NAME: __volume_create() failed - entering reconnect", devname);
			break;
		case -EINVAL:
			_NT(t_06_vol_attach, "Invalid configuration identified - aborting");
			goto _detach_newly_created_volume;
		default:
			WARN(true, "nvmeibc bug, unhandled error\n");
			goto _detach_newly_created_volume;
		}
		if ((rv = __setup_block_device_from_volume(volume, msg, false)) < 0) {
			_NE(t_07_vol_attach, DMESG_PREFIX("@DEV_NAME") ": creation failed rv=@RV - cancel attach", devname, rv);
			goto _detach_newly_created_volume;
		}
	}
	_NI_to_user(t_09_vol_attach, "@NDU " DMESG_PREFIX("@DEV_NAME"), "Attach finished successfully", 0, devname); 	//. Error code: 0
	__set_status(volume, NVS_ATTACHED);
	goto out;

_detach_newly_created_volume:
	{ /* Syncronous detach (cleanup of failed attach) does not update completion*/
		nvmeibc_volume_detach(volume, nvmeibc_vol_detach_cmd_error(), NULL);
		volume = NULL;				/* Was already kfree () */
		goto out;
	}
_free_volume_no_attach:
	kfree(volume);
	goto out;

out:
	NFOUTS(devname);
	return rv;

}

void nvmeibc_volume_update_volume_single_segment(void *context, const struct nvmeibc_cinst_params_main *p)
{
	//unsigned long flags;
	struct nvmeibc_volume *volume;
	struct nvmeibc_disk_id *disk_id;
	struct nvmeibc_disk_id_update_params *params = (struct nvmeibc_disk_id_update_params*)context;
	struct nvmeibc_disk *disk = nvmeibc_disk_from_base(params->disk);
	struct nvmeibc_block_device *block_dev = params->block_dev;
	const bool is_attach = params->is_attach;
	NFIN;
	BUG_ON(disk == NULL);
	BUG_ON(block_dev == NULL);
	disk_id = find_disk_from_block(p, disk, block_dev);
	if (!disk_id) {
		const char *action = (is_attach ? "++":"--");
		/* We already detached the volume using __disconnect_disk_from_volume()
		   but and execution of this func, was scheduled after detach */
		_ND(trace_volume_nvmeibc_volume_update_volume_single_segment, "TRC disk=@DISK_NAME[#rng=@RNG], bdev=@BDEV. Not found! ignoring vol@ACTION_PTR disk!",
		   disk->name, disk->n_ranges, block_dev, action);
		/* Note: is_attach can be true. If Volume is instructed to attach and
		   immediately detach then ++ tasks are scheduled after attach*/
		goto out;
	}
	volume = disk_id->volume;
	BUG_ON(volume == NULL);
	BUG_ON(disk_id->disk != disk);
	if (is_attach) {
		//spin_lock_irqsave(&disk->volume_spinlock, flags);
		++disk_id->num_ranges;
		++disk->n_ranges;
		//_ND(nvmeibc_volume_update_volume_single_segment_d1,
		//	"TRC disk=@STR[#rng=@INT++], bdev=@PTR[#rng=@INT++] ptr=@PTR",
		//	disk->name, disk->n_ranges, block_dev,
		//	disk_id->num_ranges, block_dev);
		//spin_unlock_irqrestore(&disk->volume_spinlock, flags);
	} else {
		//spin_lock_irqsave(&disk->volume_spinlock, flags);
		BUG_ON(disk->n_ranges < disk_id->num_ranges);
		--disk_id->num_ranges;
		--disk->n_ranges;
		//_ND(nvmeibc_volume_update_volume_single_segment_d2,
		//	"TRC disk=@STR[#rng=@INT--], bdev=@STR[#rng=@INT--] ptr=@PTR",
		//	disk->name, disk->n_ranges, block_dev->name,
		//	disk_id->num_ranges, block_dev);
		//spin_unlock_irqrestore(&disk->volume_spinlock, flags);
		if (disk_id->num_ranges == 0 && !volume->info.retain_disks)
			__disconnect_disk_from_volume(disk_id, volume->full_name, true);
	}
out:
	NFOUT;
}

/* Handle the counters of volumes on disks.
   Danie: Todo, document and move to section of transport code.*/
static void __free_transport_resources_of(struct nvmeibc_volume *volume)
{
	struct list_head disks_to_block;
	struct nvmeibc_disk_id *disk;
	struct list_head * disks  = &volume->info.disks;
	INIT_LIST_HEAD(&disks_to_block);
	while ((disk = list_first_entry_or_null(disks, struct nvmeibc_disk_id, link))) {
		_ND(__free_transport_resources_of_d1, "remove from volume disk @STR", disk->disk->name);
		if (__disconnect_disk_from_volume(disk, volume->hdr.devname, false)) {
			_ND(__free_transport_resources_of_d2, "Postponing the remove");
			list_add(&disk->link, &disks_to_block);
		} else
			_ND(__free_transport_resources_of_d3, "No need to postposne");
	}

	while ((disk = list_first_entry_or_null(&disks_to_block, struct nvmeibc_disk_id, link))) {
		_ND(__free_transport_resources_of_d4, "Blocking for disk @STR", disk->disk->name);
		nvmeibc_disk_block_remove(disk);
		_ND(__free_transport_resources_of_d5, "Done");
		list_del(&disk->link);
		disk_id_destroy(disk);
	}
}

static void __retain_transport_resources_of(struct nvmeibc_volume *volume)
{
	volume->info.retain_disks = true;
}


/* A state machine handler that implements asyncronous detach,
   Fully runs on main work queue! Though io/recoveries draining is async and use additional state machines which upon completion schedule this state machine on main-wq */
static void __nvmeibc_volume_detach_retry(struct workqe_struct * _w);
static void nvmeibc_volume_detach_o(void *ctx)
{
	struct nvmeibc_volume *volume = (struct nvmeibc_volume *)ctx;
	struct nvmeibc_volume_detach_t *detach = &volume->detach;
	int err;

_func_start:
	if (detach->error != 0) {
		if (detach->state < volume_detach_state_error_retry)
			detach->state = volume_detach_state_error_retry;
	}
	switch (detach->state) {
	case volume_detach_state_start:
		_NI(trace_volume_nvmeibc_volume_detach_o, DMESG_PREFIX("@DEV_NAME") ": Launched detach, attempt=@INT", volume->hdr.devname, detach->num_retry_attempts);
		if (!detach->cmd.err_attach) {
			if (detach->num_retry_attempts == 0)
				WARN_ON(volume->status != NVS_ATTACHED);
			else
				WARN_ON(volume->status < NVS_DETACHING);
		} else {
			WARN_ON(volume->status == NVS_ATTACHED);
		}
		__set_status(volume, NVS_DETACHING);
		{
			if (!detach->cmd.err_attach) {
				struct nvmeibc_os_api *os = volume_get_os_api(volume);
				if (block_api_os_get(os, "NVMeshClntDetach") == 0)
					detach->os = os;
			}
			detach->state = volume_detach_state_no_io;
			if (volume->block_dev) { /* Wait for IO's to drain */
				__retain_transport_resources_of(volume);	// do not release disks one by one with each destroyed segment, let __free_transport_resources_of() do it in parallel
				err = nvmeibc_wait_for_io_drain(volume->block_dev, detach->handler, volume);
				if (!err)
					return;	/* Will come back as callback*/
				detach->error = err;
			}
		}
		goto _func_start;


	case volume_detach_state_no_io: {
		__set_status(volume, NVS_DETACHING_IO_DRAINED);		// Only when IO is drained, will blockdevice start to ignore disk_pause
		/* Close blkdev for new IO requests. */
		nvmeibc_assert_on_main_wq(volume->p);
		if (volume->block_dev) {
			nvmeibc_block_trace_stats(volume->block_dev, false /* diff_only */);
			nvmeibc_del_blkdev(volume->block_dev);
		}
		detach->state = volume_detach_state_cleanup;
		goto _func_start;
	}

	case volume_detach_state_cleanup:
		nvmeibc_assert_on_main_wq(volume->p);
		nvmeibc_del_volume(volume);
		__free_transport_resources_of(volume);
		//need to make sure that the disk is released before freeing the volume
		if (volume->block_dev) {
			nvmeibc_block_exit(volume->block_dev);
			volume->block_dev = NULL;
		}
		detach->state = volume_detach_state_success_done;
		goto _func_start;

	case volume_detach_state_error_retry:
		_NW(warn_volume_nvmeibc_volume_detach_o,
		    "@EVENT_TAG" DMESG_PREFIX("@DEV_NAME") ": detach attempt=@INT failed, err=@RV, will retry",
		    EV_DETACH_RETRY(), volume->hdr.devname, detach->num_retry_attempts, detach->error);
		if (detach->num_retry_attempts < 10){
			if (!detach->cmd.err_attach) {
				block_api_os_put(detach->os);
			}
			WQ_INIT_WORK(&detach->retry_work, __nvmeibc_volume_detach_retry);
			err = nvmeibc_add_work(volume->p, &detach->retry_work);
			if (!err)
				return; /* Will come via detach retry */
			/* Else: Bliad! cannot even schedule detach. Aborting */
			detach->num_retry_attempts = 0x1DEAD1;
		}
		detach->state = volume_detach_state_error_done;
		goto _func_start;

	case volume_detach_state_error_done: /* Detach failed, reschedule.*/
		_NE(error_volume_nvmeibc_volume_detach_o,
		    "@NDU @EVENT_TAG" DMESG_PREFIX("@DEV_NAME") ": Force Detach failed. System is unstable!",
		    0, EV_DETACH_FAILED(), volume->hdr.devname);
		nvmeibc_cc_api_notify_detach_completion(volume, NVMEIB_C_TO_M_VOLUME_ACK_DETACH_FAILED);
		nvmeibc_multi_completion_done(volume->job_comp);
		volume->job_comp = NULL;
		WARN_ON(true);
		return;

	case volume_detach_state_success_done:
		BUG_ON(detach->error != 0);	/* Bug: had to pass in error state*/
		// close the open handle we held while we executed detach.
		if (!detach->cmd.err_attach) {
			const enum_vol_status how = (detach->cmd.abandon) ? NVMEIB_C_TO_M_VOLUME_ACK_UPDATE_READY : ((detach->cmd.shutdown) ? NVMEIB_C_TO_M_VOLUME_ACK_SHUTDOWN : NVMEIB_C_TO_M_VOLUME_ACK_DETACHED);
			block_api_os_put(detach->os);
			nvmeibc_cc_api_notify_detach_completion(volume, how);
			_NI_to_user(trace_1_volume_nvmeibc_volume_detach_o, "@NDU " DMESG_PREFIX("@DEV_NAME"), "Detach finished successfully", 0, volume->hdr.devname);	//. Error code: 0
			nvmeibc_multi_completion_done(volume->job_comp);
		} else {
			BUG_ON(volume->job_comp);	// Not a real job
		}
		volume->job_comp = NULL;
		_NT(t4dnvlu, "@EVENT_TAG @DEV_NAME: n_ref_ids @INT->0 detach, avpv=@INT64", EV_DETACH_FINISHED(), volume->hdr.devname, volume->hdr.ext_blob.n_ref_ids, volume->hdr.attachment_version_per_volume);
		nvmeibc_volume_header_destroy(&volume->hdr, NVMEIBC_VOLUME_HEADER_DESTROY_TOTAL);
		kfree(volume);
		return;
	default:;
 	}
	WARN(true, DMESG_PREFIX("%s: ") "Unknown detach state=%d", volume->hdr.devname, detach->state);
}

static void nvmeibc_volume_detach(struct nvmeibc_volume *volume, const struct nvmeibc_vol_detach_cmd cmd, struct nvmeibc_multi_completion *on_finish)
{
	struct nvmeibc_volume_detach_t *d = &volume->detach;
	BUG_ON(volume->job_comp);		// Did not finish previous job. How?
	volume->job_comp = on_finish;
	d->cmd = cmd;
	d->num_retry_attempts = 0;
	d->handler = nvmeibc_volume_detach_o;
	d->state = volume_detach_state_start;
	d->error = 0;
	d->handler(volume);
	return; /* Danger! it might already got kfree() here */
}

static void __nvmeibc_volume_detach_retry(struct workqe_struct *_w)
{
	struct nvmeibc_volume_detach_t *d = container_of(_w, struct nvmeibc_volume_detach_t, retry_work);
	struct nvmeibc_volume *volume =     container_of( d, struct nvmeibc_volume         , detach);
	++d->num_retry_attempts;
	d->state = volume_detach_state_start;
	d->error = 0;
	d->handler(volume);
	return; /* Danger! it might already got kfree() here */
}

void nvmeibc_volume_get(struct nvmeibc_volume *volume, unsigned long *flags)
{
	if (flags) {
		spin_lock_irqsave(&volume->spinlock, *flags);
	} else {
		spin_lock(&volume->spinlock);
	}
}

void nvmeibc_volume_put(struct nvmeibc_volume *volume, unsigned long *flags)
{
	if (flags) {
		spin_unlock_irqrestore(&volume->spinlock, *flags);
	} else {
		spin_unlock(&volume->spinlock);
	}
}

void nvmeibc_volume_set_block_device(struct nvmeibc_volume *volume, struct nvmeibc_block_device *dev)
{
	unsigned long flags;

	NFIN;
	nvmeibc_assert_on_main_wq(volume->p);	// Called from main-wq but volume->block_dev is accessed from disk-wq for pause-cont
	nvmeibc_volume_get(volume, &flags);
	volume->block_dev = dev;
	volume->status = NVS_ATTACHING_HAVE_BDEV;
	nvmeibc_volume_put(volume, &flags);
	NFOUT;
}

// Iterate over disk_id list of the given volume & dump all disks it is using
void nvmeibc_volume_dump_disk_ids(struct nvmeibc_volume *vol)
{
	struct list_head        *list_disk_ids = &vol->info.disks;
	struct nvmeibc_disk_id  *disk_id_iter;
	unsigned long flags;

	_NI_to_user(t_b9_dp_dbg_tools, DMESG_PREFIX("@DEV_NAME_FULL"), "Volume is using disks:", vol->full_name);
	spin_lock_irqsave(&vol->spinlock, flags);
	list_for_each_entry(disk_id_iter, list_disk_ids, link) {
		_NI_to_user(t_ba_dp_dbg_tools, DMESG_PREFIX(), "\tname=@NAME, @DEV_NAME, count=@COUNT", disk_id_iter->name, disk_id_iter->volume->hdr.devname, disk_id_iter->num_ranges);
	}
	spin_unlock_irqrestore(&vol->spinlock, flags);
}

static const char * __vol_type_2_string(const struct nvmeibc_volume *vol)
{
	const bool is_recoverer = nvmeibc_block_is_recoverer(&vol->hdr);		// == (dev->os->is_io_api_disabled)
	const bool is_shadow = nvmeibc_block_is_shadow(&vol->hdr);			// == (dev->os->is_io_api_disabled)
	if (is_recoverer) {
		return "recoverer";
	}
	if (is_shadow) {
		return "shadow";
	}
	return "visible";
}

void nvmeibc_volume_to_text(const struct nvmeibc_volume *volume, struct nvmeib_txt* txt)
{
	const struct nvmeibc_volume_header *h = &volume->hdr;
	nvmeib_txt_append(txt, "Mgmt Report: {fioe_cli=%d, last_io_perm=%d, attachment_version=%d, type=%s}", h->first_io_enabled_was_sent_to_cli, h->last_sent_io_perm, h->attachment_version, __vol_type_2_string(volume));
}

void nvmeibc_volume_to_json(const struct nvmeibc_volume *volume, struct jdr *jdr)
{
	const struct nvmeibc_volume_header *h = &volume->hdr;
	jdr_object_scope(jdr, "Reports status");
	jdr_write_var(jdr, fioe_cli, h->first_io_enabled_was_sent_to_cli);
	jdr_write_var(jdr, last_mgmt_io_perm, h->last_sent_io_perm);
	jdr_write_var(jdr, attachment_version, h->attachment_version);
}

int nvmeibc_volume_call_for_all_vol_disks(const struct nvmeibc_volume *volume,
					  int (*call_fn)(struct nvmeibc_idisk *disk, void *ctx), void *ctx)
{
	struct nvmeibc_disk_id  *disk_id_iter;
	int rv, n_calls = 0;
	unsigned long flags;

	spin_lock_irqsave((spinlock_t *)&volume->spinlock, flags);
	list_for_each_entry(disk_id_iter, &volume->info.disks, link) {
		if ((rv = (*call_fn)(&(disk_id_iter->disk->base), ctx)) < 0)
			goto unlock;
		n_calls++;
	}
	rv = n_calls;
unlock:
	spin_unlock_irqrestore((spinlock_t *)&volume->spinlock, flags);
	return rv;
}
