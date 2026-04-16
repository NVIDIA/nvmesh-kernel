/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_error_report.h"
#include "nvmeibc_block_common.h"
#include "nvmeibc_block.h"		/* external API of the block */
#include "nvmeibc_block_api_os.h"
#include "nvmeibc_nvmeiba_kapi.h"
#include "nvmeib_json.h"
#include "nvmeib_io_stats.h"
#include "nvmeib_utils.h"
#include "os_api/nvmeibc_block_api_os_common.h"
#include "os_api/nvmeibc_block_api_os_scsi_ioctls.inc.c"
#include "os_api/nvmeibc_block_api_os_sub_vols_common.h"
#include "common/proc_epilog.h"
#ifndef KR_VERSION_H
#	include <kr_version.h>
#endif
#if KS_HAS_PART_STAT_H
#	include <linux/part_stat.h>
#endif

/* Note: The params below have to be translated to units of 512[b]*/
#define to_kenrel_sects(l) ((l)*(LOCKSET_SLICES << KERNEL_SECTOR_TO_SECTOR_SHIFT))

unsigned max_trim_size_mirrored = 128;		 // 128 locks, 16[megabytes]
module_param(max_trim_size_mirrored, uint, 0644);
MODULE_PARM_DESC(max_trim_size_mirrored, "Maximum size of a single NVMesh internal TRIM operation for mirrored volumes. The value is for multiples of 128 KB.");

unsigned max_trim_size_non_mirrored = 16384;	 // 256[locks]=32[Mb], 16K[locks]=2[Gb]
module_param(max_trim_size_non_mirrored, uint, 0644);
MODULE_PARM_DESC(max_trim_size_non_mirrored, "Maximum size of a single NVMesh internal TRIM operation for non-mirrored volumes.");

/************************ Minor ID allocator for volume ***********************/
bool nvmeibc_use_block_external_major = false;	// Use internal allocator of minors instead of kernel built in
module_param_named(use_block_external_major, nvmeibc_use_block_external_major, bool, 0444);	// Can be set only when module is going up
MODULE_PARM_DESC(use_block_external_major, "Determines whether to use a dedicated block external major for the NVMesh block devices. This is rarely required.");

uint nvmeibc_max_num_partitions_on_vol = DISK_MAX_PARTS;	// Used in internal allocator of minors
module_param_named(max_num_partitions_on_vol, nvmeibc_max_num_partitions_on_vol, uint, 0444);	// Can be set only when module is going up
MODULE_PARM_DESC(max_num_partitions_on_vol, "Defines the maximum number of external partitions reserved as minors range.");

void disk_id_allocator_init(struct disk_id_allocator_t* al)
{
	const int n_max_minors = BITS_PER_BYTE * sizeof(al->bmp);
	memset(al, 0, sizeof(*al));
	al->bmp[0] = 0x1;	// For debug, dont allocate minor 0, as it is used to mark auto allocation
	_NT(t_dia5, "DIA: Max amount of minors supported by kernel = 2^@INT > n_vols=@INT * n_partitions=@INT", MINORBITS, n_max_minors, DISK_MAX_PARTS);
	BUILD_BUG_ON((BITS_PER_BYTE* sizeof(al->bmp) * DISK_MAX_PARTS) >= (1 << MINORBITS)); // Support up to 1K gendisks, 1 bit per gendisk, each with 256 partitions. Our minors occupy up to 18 bits, which is less than MINORBITS
}

void disk_id_allocator_alloc(struct disk_id_allocator_t* al, struct gendisk *disk, const char* vol_name)
{
	if (nvmeibc_use_block_external_major) {
		disk->minors = disk->first_minor = 0;
		if (disk->minors == 0)// This will cause the gendisk to be minor of  'cat /proc/devices | grep blkext' driver
			disk->flags	|= GENHD_FL_EXT_DEVT;	// minors == 0 indicates to use ext devt from part0
	} else {
		const int n_max_minors = BITS_PER_BYTE * sizeof(al->bmp);
		const int new_minor = (int)find_first_zero_bit(al->bmp, n_max_minors);
		// Note: disk->disk_name may be not initialized yet
		_NT(t_dia0, "DIA: +@INT, bmp=@LX, vol=@DEV_NAME", new_minor, al->bmp[0], vol_name);			// pr_emerg("DIA: + %u, 0x%lx disk=%p, vol=%s\n", new_minor, al->bmp[0], disk, vol_name);
		WARN(new_minor >= n_max_minors, "nvmeibc: bug too much volumes... going to crush: volumes=%s, minor=%d\n", vol_name, disk->first_minor);
		set_bit(new_minor, al->bmp);
		disk->minors = DISK_MAX_PARTS;		// This will cause the gendisk to be minor of 'nvmeibc' driver
		disk->first_minor = new_minor * nvmeibc_max_num_partitions_on_vol;	// All partitions will use integer: disk->first_minor + [1 ... disk->minors-1]
		// _NI_to_user(t_dia6, DMESG_PREFIX("@DEV_NAME"), "assigning minor versions for partitions [@INT..@INT]", vol_name, disk->first_minor, disk->first_minor + disk->minors-1);
	}
}

void disk_id_allocator_free(struct disk_id_allocator_t* al, struct gendisk *disk)
{
	if (!nvmeibc_use_block_external_major) {
		const int new_minor = disk->first_minor / nvmeibc_max_num_partitions_on_vol;
		_NT(t_dia1, "DIA: -@INT, bmp=@LX, vol=@DEV_NAME", new_minor, al->bmp[0], disk->disk_name);		// pr_emerg("DIA: - %u, 0x%lx disk=%p, vol=%s\n", new_minor, al->bmp[0], disk, disk->disk_name);
		WARN(!test_bit(new_minor, al->bmp), "nvmeibc: Unexpected Internal error during detach, %d minor of disk %s not registered for removal!\n", new_minor, disk->disk_name);
		clear_bit(new_minor, al->bmp);
	}
}

void disk_id_allocator_mark(struct disk_id_allocator_t* al, struct gendisk *disk)
{
	if (!nvmeibc_use_block_external_major) {
		const int new_minor = disk->first_minor / nvmeibc_max_num_partitions_on_vol;
		const int is_already_allocated = test_bit(new_minor, al->bmp);
		_NT(t_dia2, "DIA: u@INT, bmp=@LX, vol=@DEV_NAME, is_a=@BOOL_YN", new_minor, al->bmp[0], disk->disk_name, is_already_allocated);		// pr_emerg("DIA: u %u, 0x%lx disk=%p, vol=%s\n", new_minor, al->bmp[0], disk, disk->disk_name);
		set_bit(new_minor, al->bmp);
		WARN(is_already_allocated, "nvmeibc: Unexpected Internal error during hot upgrade, %d minor of disk %s already registered!\n", new_minor, disk->disk_name);
	}
}

#define get_dia(atom)  ((struct disk_id_allocator_t *)&(((struct nvmeibc_os_api *)(atom))->driver_context)->dia)

/************************ Self rereference counters ++/-- *********************/
#define block_path_to_string(dst, len, os) \
	scnprintf(dst, len, "/dev/%s", os->atom.disk->disk_name)

#define __is_kernel_dev_err(bdev) 	((bdev == NULL) || IS_ERR(bdev))

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
static struct file * __get_safe_kern_dev(const char *path, BLK_MODE_T mode)
{
	struct file *f = bdev_file_open_by_path(path, mode, NULL /* holder */, NULL);
	struct block_device *bdev = IS_ERR(f) ? (struct block_device *)f : file_bdev(f);
	if (__is_kernel_dev_err(bdev)) {
		_NT(t_gskvae_00, "path lookup for @STR found @PTR and it is@YES_NO_STATUS err, @YES_NO_STATUS null",
			path, bdev, IS_ERR(bdev) ? "" : " not", (bdev == NULL) ? "is" : "not");
		f = 0;
	}
	return f;
}

#elif KS_HAS_BDEV_OPEN_BY_PATH

// Daniel: Do not use bdev = bdget_disk(atom->disk, 0); Gendisk might already be free()
// Note: This function does open() on our os.
static struct bdev_handle * __get_safe_kern_dev(const char *path, BLK_MODE_T mode)
{
	struct bdev_handle *bdh = bdev_open_by_path( path, mode, NULL /* holder */, NULL);
	struct block_device *bdev = IS_ERR(bdh) ? (struct block_device *)bdh : bdh->bdev;
	if (__is_kernel_dev_err(bdev)) {
		_NT(t_gskvae_00, "path lookup for @STR found @PTR and it is@YES_NO_STATUS err, @YES_NO_STATUS null",
			path, bdev, IS_ERR(bdev) ? "" : " not", (bdev == NULL) ? "is" : "not");
		bdh = 0;
	}
	return bdh;
}

#else // KS_HAS_BDEV_OPEN_BY_PATH

static int bdev_holder = 1;

static struct block_device * __get_safe_kern_dev(const char *path, BLK_MODE_T mode)
{
#if KS_BLKDEV_GET_BY_PATH_HAS_HOLDERS
	struct block_device *bdev = blkdev_get_by_path( path, mode, &bdev_holder, NULL);
#else
	struct block_device *bdev = blkdev_get_by_path( path, mode, &bdev_holder);
#endif
	if (__is_kernel_dev_err(bdev)) {
		_NT(t_gskvae_00, "path lookup for @STR found @PTR and it is@YES_NO_STATUS err, @YES_NO_STATUS null",
			path, bdev, IS_ERR(bdev) ? "" : " not", (bdev == NULL) ? "is" : "not");
	}
	return bdev;
}

#endif // KS_HAS_BDEV_OPEN_BY_PATH

#if KS_BLKDEV_GET_BY_PATH_HAS_HOLDERS
#define BLKDEV_PUT(bdev) blkdev_put((bdev), NULL)
#else
#define BLKDEV_PUT(bdev) blkdev_put((bdev), SELF_REF_MODE);
#endif

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
static bool block_api_os_check_if_bdev_already_exists(const struct nvmeibc_os_api *os)
{
	char path[DISK_NAME_LEN+10];
	dev_t dev;

	block_path_to_string(path, sizeof(path), os);
	return lookup_bdev(path, &dev) == 0;
}
#else // KS_HAS_BDEV_FILE_OPEN_BY_PATH
static bool block_api_os_check_if_bdev_already_exists(const struct nvmeibc_os_api *os)
{
	char path[DISK_NAME_LEN+10];
	struct block_device *bdev = NULL;

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	struct file *bdev_file = NULL;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	struct bdev_handle *bdh = NULL;
#endif

	int rv;
	block_path_to_string(path, sizeof(path), os);
	_NT(t_gskvae_01, "Checking if bdev already exists...");
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	bdev_file = __get_safe_kern_dev(path, SELF_REF_MODE);
	bdev = file_bdev(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
	bdh = __get_safe_kern_dev(path, SELF_REF_MODE);
	bdev = bdh ? bdh->bdev : 0;
#else //  KS_HAS_BDEV_OPEN_BY_PATH
	bdev = __get_safe_kern_dev(path, SELF_REF_MODE);
#endif //  KS_HAS_BDEV_OPEN_BY_PATH
	if (unlikely(bdev == ERR_PTR(-ENOTBLK))) { // Not possible to create bdev
		rv = true;
	} else if (__is_kernel_dev_err(bdev)) {
		rv = false;						// Does not exist
	} else {
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		bdev_fput(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
		bdev_release(bdh);
#else //  KS_HAS_BDEV_OPEN_BY_PATH
		BLKDEV_PUT(bdev);
#endif //  KS_HAS_BDEV_OPEN_BY_PATH
		rv = true;
	}
	return rv;
}
#endif

/* Do self open: +2 ref to gendisk/bdev (+1 by __get_safe_kern_dev, +1 self) */
int block_api_os_get(struct nvmeibc_os_api *os, const char *owner_name)
{
	char path[DISK_NAME_LEN+10];
	struct block_device *bdev = NULL;

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	struct file *bdev_file;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	struct bdev_handle *bdh = NULL;
#endif

	const char *reason = "unknown";
	if (!os || os->is_io_api_disabled)
		return -EPERM;

	block_path_to_string(path, sizeof(path), os);
	if (os->unsafe_self_ref.bdev_during_detach) {
		reason = "get() twice";
		goto _critical_error;
	}

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	bdev_file = __get_safe_kern_dev(path, SELF_REF_MODE);
	bdev = bdev_file ? file_bdev(bdev_file) : 0;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	bdh = __get_safe_kern_dev(path, SELF_REF_MODE);
	bdev = bdh ? bdh->bdev : 0;
#else //  KS_HAS_BDEV_OPEN_BY_PATH
	bdev = __get_safe_kern_dev(path, SELF_REF_MODE);
#endif //  KS_HAS_BDEV_OPEN_BY_PATH
	if (__is_kernel_dev_err(bdev)) {
		reason = "Cannot get()";		// Probably user played manually with /dev/... directory
		goto _critical_error;
	}
#if KS_HAS_BLKMODE
	if (nvmeiba_kapi.atom_open(bdev->bd_disk, owner_name) != 0) {
#else // KS_HAS_BLKMODE
	if (nvmeiba_kapi.atom_open(bdev, owner_name) != 0) {
#endif // KS_HAS_BLKMODE
		reason = "Cannot open()";
		goto _critical_error;
	}
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	os->unsafe_self_ref.bdev_during_detach = bdev_file; // Safe, coz we know, refcount is held by os
#elif KS_HAS_BDEV_OPEN_BY_PATH
	os->unsafe_self_ref.bdev_during_detach = bdh; // Safe, coz we know, refcount is held by os
#else //  KS_HAS_BDEV_OPEN_BY_PATH
	os->unsafe_self_ref.bdev_during_detach = bdev; // Safe, coz we know, refcount is held by os
#endif //  KS_HAS_BDEV_OPEN_BY_PATH
	_NT(tr_0_os_get, "@STR -> bdev{@BDEV @GENDISK}++", owner_name, bdev, bdev->bd_disk);
	return 0;

_critical_error:
	_NE_to_user(tr_1_os_get, DMESG_PREFIX("@PATH"), "Unexpected error opening kernel block device for an attached volume, try to detach and attach volume to relieve. Error code: 1021. Block device: @BDEV @GENDISK. Reason: @STR.", path, bdev, os->atom.disk, reason);
	if (bdev && !__is_kernel_dev_err(bdev))

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		bdev_fput(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
		bdev_release(bdh);
#else //  KS_HAS_BDEV_OPEN_BY_PATH
		BLKDEV_PUT(bdev);
#endif //  KS_HAS_BDEV_OPEN_BY_PATH

#if !defined(BLKDEV_SIMULATOR)
	WARN_ON(true);					// Simulator intentionally causes some errors to occur to test behavior
#endif
	return -ENODEV;
}

/* Do self close: -2 ref to gendisk/bdev */
void block_api_os_put(struct nvmeibc_os_api *os)
{
	struct block_device		*bdev;

#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	struct file *bdev_file = NULL;
#elif KS_HAS_BDEV_OPEN_BY_PATH
	struct bdev_handle *bdh = NULL;
#endif

	struct gendisk			*disk;
	if (!os || os->is_io_api_disabled)
		return;
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	bdev_file = os->unsafe_self_ref.bdev_during_detach;	// Safe because we hold ref count
	bdev = file_bdev(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
	bdh = os->unsafe_self_ref.bdev_during_detach;	// Safe because we hold ref count
	bdev = bdh->bdev;
#else // KS_HAS_BDEV_OPEN_BY_PATH
	bdev = os->unsafe_self_ref.bdev_during_detach;	// Safe because we hold ref count
#endif // KS_HAS_BDEV_OPEN_BY_PATH
	if (!bdev) {
		_NE_to_user(tr_2_os_get, DMESG_PREFIX("@DEV_NAME"), "Unexpected error that may make it impossible to detach a volume, which may require a reboot to cleanup. Error code: 1022.", os->atom.dev_name);
		return;
	}
	disk = bdev->bd_disk;           // atom->disk may already be NULL
	os->unsafe_self_ref.bdev_during_detach = NULL;// Will do -1 on bdev below. Ptr is unsafe
	nvmeiba_kapi.atom_close(disk);
	os = NULL;					// BEWARE: os_api object might be freed now.
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		bdev_fput(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
		bdev_release(bdh);
#else //  KS_HAS_BDEV_OPEN_BY_PATH
		BLKDEV_PUT(bdev);
#endif //  KS_HAS_BDEV_OPEN_BY_PATH
	_NT(tr_3_os_get, "bdev{@BDEV @GENDISK}--", bdev, bdev->bd_disk);
}

#define get_os_api_cints(p) __get_from_params_blok_globals_container(p)->osc
/********************** Partitions revalidation mechanism *********************/
/* Disk revalidation is a relatively short task which is shceduled on the main
   wq. However, it invokes a reread partition task which is a blocking IO and
   if blockdevice has IO disabled this will block for a long long time. So this
   cannot be done on main_wq. Warning: This function must be safe to call even
   after volume was detached, coz it is called asyncronously!!! */
struct _read_part_t {						// Async work for reading partitions
	struct work_struct work;				// !!! Must be first
	struct nvmeibc_os_apis_container *p; 	// Which client instance is doing the job
	char path[DISK_NAME_LEN+10];			// Find block device by name
	// BEWARE: we send the gendisk reference but dnot increment the refcount - so DONT dereference it !!!
	struct gendisk *osapi_disk;				// For debug, gendisk os_api held
};

bool no_part_scan = false;
module_param(no_part_scan, bool, 0644);
MODULE_PARM_DESC(no_part_scan, "Disable partition scan on nvmesh block devices.");	// Disable both internally triggered and externally triggered
#define N_READ_PART_DISABLED  100000				// When disabled counter becomes negative with this factor
#define __is_self_read_part_enabled(n)  ((n) >= 0)	// False means internally cannot trigger read partition but externally can do that
static void __read_part_t_destroy(struct _read_part_t *rp)
{
	int n_inflight = atomic_dec_return(&rp->p->num_read_part_in_flight);
	_NT(trace_01_read_partitions_work, "Volume @DEV_NAME read partitions done. in_flights=@RV", rp->path, n_inflight);
	kfree(rp);
}

#if !defined(BLKDEV_SIMULATOR)
	#define RUN_READ_PART_ON_THREAD (1)			/* We dont want to block system/main wq on IO */
#endif

#ifdef RUN_READ_PART_ON_THREAD
	// This runs on dedicated thread
	#define RP_RETURN_VAL  (0)
	#define RP_RETURN_TYPE int
	#define RP_ARG_TYPE void
#else
	// This runs on the system_wq, see EC-2904
	#define RP_RETURN_VAL
	#define RP_RETURN_TYPE void
	#define RP_ARG_TYPE struct work_struct
#endif

#if KS_HAS_BLKDEV_IOCTL
#include <asm/uaccess.h>
static int __ioctl_by_bdev(struct block_device *bdev, unsigned cmd, unsigned long arg)
{

	int res;
#ifdef CONFIG_SET_FS
	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);
	res = blkdev_ioctl(bdev, 0, cmd, arg);
	set_fs(old_fs);
#else
	res = blkdev_ioctl(bdev, 0, cmd, arg);
#endif
	return res;
}
#endif

static RP_RETURN_TYPE __read_partitions_work(RP_ARG_TYPE *param)
{
	struct work_struct *_work = (struct work_struct *)param;
	struct _read_part_t *rp = container_of(_work, struct _read_part_t, work);
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
	const BLK_MODE_T mode = BLK_OPEN_READ | BLK_OPEN_WRITE;
	struct file *bdev_file = __get_safe_kern_dev(rp->path, mode);
	struct block_device *bdev = __is_kernel_dev_err(bdev_file) ? (void *)bdev_file : file_bdev(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
	const BLK_MODE_T mode = BLK_OPEN_READ | BLK_OPEN_WRITE;
	struct bdev_handle *bdh = __get_safe_kern_dev(rp->path, mode);
	struct block_device *bdev = __is_kernel_dev_err(bdh) ? (void *)bdh : bdh->bdev;
#else // KS_HAS_BDEV_OPEN_BY_PATH
	const BLK_MODE_T mode = FMODE_READ|FMODE_LSEEK|FMODE_PREAD|FMODE_PWRITE; // 0x1d //FMODE_NDELAY| FMODE_WRITE;
	struct block_device *bdev = __get_safe_kern_dev(rp->path, mode);
#endif // KS_HAS_BDEV_OPEN_BY_PATH
	if (__is_kernel_dev_err(bdev)) { /* Maybe volume was detached by main_wq? */
		_NW_to_user(t_01_osapi_rpw,     DMESG_PREFIX("@DEV_NAME"), "Failed to read partitions, to read manually use partprobe. Error code: 1023. Mode=@FMODE.", rp->path, (unsigned)mode);
	} else {
		if (!bdev->bd_disk) { /* Daniel: ioctl accesses it without testing*/
			_NE_to_user(t_02_osapi_rpw, DMESG_PREFIX("@DEV_NAME"), "Failed to read partitions, to read manually use partprobe. Error code: 1024. Mode=@FMODE.", rp->path, (unsigned)mode); // What ??? a bug???
		} else { // This is a blocking IO (possibly for a long time...)
#if KS_HAS_BLKDEV_IOCTL
			_NT(trace_api_os_read_partitions_work, DMESG_PREFIX("@DEV_NAME: ") "going to read partitions: @BDEV|@GENDISK m=@FMODE", rp->path, bdev, rp->osapi_disk, (unsigned)mode);
			__ioctl_by_bdev(bdev, BLKRRPART, 0);		// Invokes blkdev_reread_part()
#elif !defined(BLKDEV_SIMULATOR)
			/* Kernel does not have blkdev_ioctl. Call partprobe or blockdev instead, to force blkdev_reread_part() */
			do {
				/* TBD: Check in script and pass via modparam */
				const char *part_probe = "blockdev --rereadpt"; // "/usr/sbin/partprobe";
				char cmd_line[64];
				int rv;
				snprintf(cmd_line, sizeof(cmd_line), "%s %s", part_probe, rp->path);
				_NT(trace_api_os_read_partitions_work_pp, DMESG_PREFIX("@DEV_NAME: ") "Running |@STR| to read partitions: @BDEV|@GENDISK m=@FMODE", rp->path, part_probe, bdev, rp->osapi_disk, (unsigned)mode);
				rv = nvmeib_run_usermode_script(cmd_line);
				if (rv < 0) {
					_NE_dmesg(trace_api_os_read_partitions_work_pp_fail, DMESG_PREFIX("@DEV_NAME: ") "partprobe Failed (@RV) for cmd-line |@STR|. Volume size might be incorrect!", rp->path, rv, cmd_line);
				}
			} while(0);
#endif
		}
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		bdev_fput(bdev_file);
#elif KS_HAS_BDEV_OPEN_BY_PATH
		bdev_release(bdh);
#else //  KS_HAS_BDEV_OPEN_BY_PATH
		BLKDEV_PUT(bdev);
#endif //  KS_HAS_BDEV_OPEN_BY_PATH
	}
	__read_part_t_destroy(rp);
	// msleep(30000)				// to dest detach while read partition thread is still alive
	return RP_RETURN_VAL;
}

static void __launch_async_read_part_blocking_io(struct _read_part_t *rp)
{
	int n_inflight = atomic_inc_return(&rp->p->num_read_part_in_flight);
	if (!__is_self_read_part_enabled(n_inflight)) {
		_NT(t_04_read_part, "@DEV_NAME: read partitions abort due to hot upgrade. in_flights=@RV", rp->path, n_inflight);
		goto __destroy_on_error;
	}
	_NT(t_00_read_part, "@DEV_NAME: read partitions launch. in_flights=@RV", rp->path, n_inflight);

#ifdef RUN_READ_PART_ON_THREAD
	{
		struct task_struct *rp_thread;
		proc_name_t pname;
		clnt_proc_name_format(pname, 'C', "RP", "ccb_rp", nvmeibc_cinst_get_blok_inst_num(nvmeibc_cinst_get_blok_p(rp->p)));
		rp_thread = kthread_run(__read_partitions_work, rp, "%s", pname);
		if (IS_ERR(rp_thread)) {
			_NE_to_user(t_01_read_part, DMESG_PREFIX("@PATH"), "Failed to read partitions due to thread creation error, to read manually use partprobe. Error code: 1025. Return code: @PTR_ERR.", rp->path, PTR_ERR(rp_thread));
			goto __destroy_on_error;
		}
	}
#else
	INIT_WORK(&rp->work, __read_partitions_work);
	schedule_work(&rp->work);
#endif
	return;
__destroy_on_error:
	__read_part_t_destroy(rp);
}

#define is_gendisk_ready_for_io(atom) (atomic_read(&(atom)->gendisk_status) > 2)
#define __verify_on_main_wq_osapi(os)    nvmeibc_assert_on_main_wq(nvmeibc_cinst_get_blok_m((os)->driver_context))
#define __verify_on_main_wq_atom(at)  __verify_on_main_wq_osapi(container_of(at, struct nvmeibc_os_api, atom))

#if KS_BLOCK_DEV_OPS_HAS_REVALIDATE_DISK
	static int __disk_revalidation_internal(struct gendisk *gd)
	{
		_NT(t_01_osapi_dr, "@GENDISK", gd); // For debug only, Todo, can add counter here
		return 0;
	}

	static void __disk_revalidation_internal_call(struct gendisk *gd)
	{
		if (gd->fops->revalidate_disk)
			gd->fops->revalidate_disk(gd);
	}
#else
	static void __disk_revalidation_internal_call(struct gendisk *gd) { (void)gd; }
#endif

static void __revalidate_and_reread_work(void *_os) {
	unsigned long flags;
	struct gendisk *os_disk;
	struct nvmeibc_os_api *os = (struct nvmeibc_os_api*)_os;
	struct nvmeiba_atom_os_api *atom = &os->atom;
	struct nvmeibc_block_device *bdev = os->dev;

	ulong n_blocks_size = 0;
	if ((os->is_io_api_disabled)||(!os->disk_reval_task))
		return;

	__verify_on_main_wq_osapi(os);									// Must be on main work queue to serialize with sub volume add/del, Alternatively must acquire sub.list_lock_unused
	spin_lock_irqsave(&atom->disk_lock, flags);
	os_disk = atom->disk;
	n_blocks_size = (ulong)(get_capacity(os_disk)>>KERNEL_SECTOR_TO_SECTOR_SHIFT);
	spin_unlock_irqrestore(&atom->disk_lock, flags);
	_NT(t_01_ospairrrw, "@DEV_NAME: revalidation start, @GENDISK, task_type=@INT, @DEV_SIZE[blks]", atom->dev_name, os_disk, os->disk_reval_task, n_blocks_size); // For debug
	if (unlikely(!os_disk))  			/* NULL if bdev is dettaching right now */
		goto _scheduling_done;
#if KS_HAS_REVALIDATE_DISK_FN /* this will call our macro or orignial func, if it will also fail to compile it's safer cause at least we catch it */
	revalidate_disk(os_disk); // Must not be within spin_lock, Potential dead lock here: EC-6596
	// Kernel 4: revalidate_disk() calls check_disk_size_change() which does i_size_write() writing file system inode and thus it is extended as well.
	if (0) __disk_revalidation_internal_call(os_disk); // revalidate_disk() already called the content of this function directly
	_NT(t_07_ospairrrw, "@DEV_NAME: revalidate_disk() called", atom->dev_name); 	// set_capacity() actually calls i_size_write(os_disk->part0->bd_inode)
#elif (KS_HAS_SET_CAPACITY_AND_MODIFY_FN_BDEV || KS_HAS_SET_CAPACITY_AND_MODIFY_FN_GENDISK)
	// In kernel 5.12+
	_NT(t_06_ospairrrw, "@DEV_NAME: revalidation already happened in set_capacity()", atom->dev_name); 	// set_capacity() actually calls i_size_write(os_disk->part0->bd_inode)
	__disk_revalidation_internal_call(os_disk);
	#if 0	// Daniel, not sure needed, took the idea from loop device
		if (!set_capacity_and_notify(os_disk, get_capacity(os_disk)))
			kobject_uevent(&disk_to_dev(os_disk)->kobj, KOBJ_CHANGE);
	#endif
#elif KS_HAS_REVALIDATE_DISK_SIZE
	revalidate_disk_size(os_disk, true);	// Alternatively can call: set_capacity_revalidate_and_notify()
	if (0) __disk_revalidation_internal_call(os_disk); // revalidate_disk_size() already called the content of this function directly
	_NT(t_08_ospairrrw, "@DEV_NAME: revalidate_disk_size()", atom->dev_name);
#else
	__disk_revalidation_internal_call(os_disk);
	_NT(t_05_ospairrrw, "@DEV_NAME: revalidation not implemented, resize may not be visible in fs", atom->dev_name);
#endif
	__revalidate_sub_vols_of_carrier(atom);
	if ((no_part_scan)||(os->disk_reval_task == NVMEIBC_OS_DISK_ONLY_REVAL) || nvmeibc_block_dev_is_shadow(bdev)) {
		_NT(t_02_ospairrrw, "@DEV_NAME: read partition is skipped", atom->dev_name);
		goto _scheduling_done;
	}
	if (is_gendisk_ready_for_io(atom)) { /* Default: reread partitions */
		struct _read_part_t *rp = kzalloc(sizeof(*rp), GFP_ATOMIC);
		if (!rp) {
			_NE_to_user(t_03_ospairrrw, DMESG_PREFIX("@DEV_NAME"), "Failed to read partitions due to out of memory, to read manually use partprobe. Error code: 1026.", atom->dev_name);
			goto _out;
		}
		block_path_to_string(rp->path, sizeof(rp->path), os);
		rp->osapi_disk = atom->disk;
		rp->p = (void*)os->driver_context;
		__launch_async_read_part_blocking_io(rp);
	} /* Else: block_api_os_start_accepting_kernel_io() not finished yet */

_scheduling_done:
	spin_lock_irqsave(&atom->disk_lock, flags);
	os->disk_reval_task = NVMEIBC_OS_DISK_REVALIDATION_EMPTY;		// Read partitions was scheduled, good enough, even if it fails
	spin_unlock_irqrestore(&atom->disk_lock, flags);
_out:
	_NT(t_04_ospairrrw, "@DEV_NAME: revalidation end, @GENDISK", atom->dev_name, os_disk);
}

/* Needed: On volumes capacity change & First IO enabling. Optionally via external ioctl */
static void __schedule_revalidation_must_hold_spinlock(struct nvmeibc_block_device *dev) {
	nvmeibc_io_resubmitter_wakeup(&dev->dp.resub); 	/* Revalidation needed + Resend pending IO */
	nvmeibc_block_set_generic_work_to_self(nvmeibc_cinst_get_blok_p(dev), dev->uuid, __revalidate_and_reread_work, dev->os, false);
}

void block_api_os_async_revalidate(struct nvmeibc_block_device *dev) {
	struct nvmeibc_os_api *os = dev->os;
	struct nvmeiba_atom_os_api *atom = &os->atom;
	unsigned long flags;
	spin_lock_irqsave(&atom->disk_lock, flags);
	os->disk_reval_task = NVMEIBC_OS_DISK_REVAL_AND_READ_PART;		// Full job
	__schedule_revalidation_must_hold_spinlock(dev);
	spin_unlock_irqrestore(&atom->disk_lock, flags);
}

#include "module/instance/nvmeibc_cinst_params.h"
/************************** Driver version ************************************/
static void nvmeibc_driver_version_clear(struct nvmeibc_driver_version* dv)
{
	dv->nvmeibc_major = 0;					// Default val to request alocation from kernel
	dv->registered_blkdev = false;
}

static void nvmeibc_driver_version_init(struct nvmeibc_driver_version* dv, const struct nvmeibc_cinst_params_blk *p)
{
	nvmeibc_driver_version_clear(dv);
	dv->name_proc_dev = p->dev_name.str;		// Save pointers because strings are held in iside param of instance
	dv->dir_lsblk =     p->dir_lsblk.str;
}

static int nvmeibc_driver_version_register(struct nvmeibc_driver_version* dv)
{
	int rv = register_blkdev(dv->nvmeibc_major, dv->name_proc_dev);
	if (rv <= 0) {
		_NE_to_user(error_api_os_nvmeibc_driver_version_register, DMESG_PREFIX("@STR"), "Failed to register volume as a block device. Error code: 1027. Return code: @RV.", dv->name_proc_dev, rv);
	} else {
		dv->nvmeibc_major = rv;
		dv->registered_blkdev = true;
		_NI(trace_api_os_nvmeibc_driver_version_register, DMESG_PREFIX("@STR") ": blkdev driver registered major=@MAJOR, /dev/@STR", dv->name_proc_dev, rv, dv->dir_lsblk);
	}
	return rv;
}

static void nvmeibc_driver_version_unreg(struct nvmeibc_driver_version* dv)
{
	if (dv->registered_blkdev) {
		unregister_blkdev(dv->nvmeibc_major, dv->name_proc_dev);
		nvmeibc_driver_version_clear(dv);
	}
}

/******************** nvmeibc fops which override nvmeiba *********************/
#define gendisk_get_api_os(disk)	((const struct nvmeibc_os_api*)((disk)->private_data))
static int __device_ioctl(struct block_device *bdev, fmode_t mode, unsigned cmd, unsigned long arg)
{
	int rv = -ENOTTY;
	const struct nvmeibc_os_api *os = gendisk_get_api_os(bdev->bd_disk);
	_NT(t_dioctl00, "@DEV_NAME: ioctl cmd=@X, arg=@ARG, mode=@X", os->atom.dev_name, cmd, (u64)arg, mode);
	/* During upgrade/force-detach, fail all IOCTLs with -ENOTTY.
	 * If we want to enable IOCTLs during upgrade in the future, we will need to add support for it in nvmeiba anyway. */
	if (unlikely(os->atom.status != nvmeiba_status_live)) {
		_NT(t_dioctl02, "Cannot execute ioctl cmd=@X, arg=@ARG, device is not live, status=@RV", cmd, (u64)arg, os->atom.status);
		return -ENOTTY;
	}
	switch (cmd) {
	case SG_IO:
		rv = nvmeibc_block_sg_io(os, (void __user *)arg);
		break;
	case CDROM_GET_CAPABILITY: // 0x5331
		break; // deliberately ignore this command
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)			// Currently not used in real system
	case NVME_IOCTL_ID:	// 0x4e40
		rv = nvmeibc_nvme_ioctl_id(os, (void __user *)arg);
		break;
#endif
	case 0x1261: // BLKFLSBUF
		rv = 0;		// Request to flush all cached IO, we dont cache anything.
		break;

	//case 0x1272: // BLKGETSIZE64
	//case 0x125f: // BLKRRPART
	default:
		_NT(t_dioctl01, "Unsupported ioctl cmd=@X, arg=@ARG", cmd, (u64)arg);
		break;
	}
	return rv;
}

/******************************************************************************/
static REQ_RET nvmeibc_b_req_make_no_q(  struct bio *bio);
#if NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING
static REQ_RET nvmeibc_b_req_reject_no_q(struct bio *bio);
#endif

static void __nvmeibc_os_api_layer_destroy(struct nvmeibc_os_apis_container *c)
{
	nvmeiba_kapi.os_do_on_nvmeibc_down();			// Disconenct from nvmeiba
	nvmeibc_driver_version_unreg(&c->drv_ver);		// Todo: Share this with ATOM
	// disk_id_allocator_init(&c->dia);			// Todo: Here, verify that bitmap is empty if none of the volumes were abandoned
	kfree(c);
}

static void nvmeibc_block_device_operations_init(const struct block_device_operations *src, struct block_device_operations *dst)
{
	memset(dst, 0, sizeof(*dst));
	dst->owner = src->owner;			// This line is crucial. Do not put 'THIS_MODULE' here. We want each open()/close()/mount()/... to take reference on nvmeiba, not nvmeibc to be able to hot upgrade
	dst->open = src->open;
	dst->release = src->release;
	dst->ioctl = __device_ioctl;
	if (0) {
		#if KS_BLOCK_DEV_OPS_HAS_REVALIDATE_DISK
			dst->revalidate_disk = __disk_revalidation_internal;	// Daniel: Enable for debug
		#endif
	}
}

static void __probe_my_atoms_from_nvmeiba(const struct nvmeiba_atom_os_api *atom, void* ctx) {
	struct nvmeibc_os_apis_container *c = ctx;
	if (atom->status == nvmeiba_status_detaching)
		return; // Dont care here, it does not have any allocated minor!
	WARN(atom->status != nvmeiba_status_orphan, "nvmeibc bug: Wrong usage of function. volume %s, status=%d!\n", atom->dev_name, atom->status);
	disk_id_allocator_mark(&c->dia, atom->disk);
}

extern struct proc_dir_entry *nvmeibc_get_proc_dir_volumes(const struct nvmeibc_cinst_params_main *p);	// Dont include entire nvmeibc_main.h api
struct nvmeibc_os_apis_container * nvmeibc_os_api_layer_init(const struct nvmeibc_cinst_params_blk *p)
{
	struct nvmeiba_to_c_handover H;
	struct nvmeibc_os_apis_container *c = (void*)kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c) {
		_NE(t_01_oslinit, DMESG_PREFIX() ": Out of memory");
		goto _out;
	}
	nvmeibc_cinst_get_blok_p(c) = p;
	H = nvmeiba_kapi.os_do_on_nvmeibc_up();			// Connect to nvmeiba
	nvmeibc_block_device_operations_init(H.fops, &c->bdev_fops_io);
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	#if NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING
		nvmeibc_block_device_operations_init(H.fops, &c->bdev_fops_de);
		c->bdev_fops_de.submit_bio = nvmeibc_b_req_reject_no_q; // This pointer will replace IO when detaching
	#endif
	c->bdev_fops_io.submit_bio = nvmeibc_b_req_make_no_q;	// This pointer will be used to issue IOs
#endif
	nvmeibc_driver_version_init(&c->drv_ver, p);
	nvmeibc_driver_version_clear(&c->drv_ver);
	c->atom_protocol_version = H.protocol_version;
	c->proc_root = nvmeibc_get_proc_dir_volumes(nvmeibc_isnt_params_blk2main(p));
	atomic_set(&c->num_read_part_in_flight, 0);
	if (nvmeibc_driver_version_register(&c->drv_ver) <= 0) {
		__nvmeibc_os_api_layer_destroy(c);
		c = NULL;
	} else {
		disk_id_allocator_init(&c->dia);	// Occupy the minors of atoms in nvmeiba
		if (H.n_orphan_osapi > 0)
			nvmeiba_kapi.os_api_exec_for_each_atom(c->drv_ver.dir_lsblk, __probe_my_atoms_from_nvmeiba, c);
	}
_out:
	return c;
}

void nvmeibc_os_api_layer_destroy(const struct nvmeibc_cinst_params_blk *p)
{
	__nvmeibc_os_api_layer_destroy(get_os_api_cints(p));
}

int nvmeibc_os_api_layer_get_num_read_part(const struct nvmeibc_cinst_params_blk *p, bool *is_on)
{
	struct nvmeibc_os_apis_container *c = get_os_api_cints(p);
	const int n = atomic_read(&c->num_read_part_in_flight);
	if (is_on)
		*is_on = __is_self_read_part_enabled(n);
	return __is_self_read_part_enabled(n) ? n : (n + N_READ_PART_DISABLED);
}

void nvmeibc_os_api_layer_toggle_read_part(const struct nvmeibc_cinst_params_blk *p, bool turn_on)
{
	struct nvmeibc_os_apis_container *c = get_os_api_cints(p);
	int n = atomic_read(&c->num_read_part_in_flight);
	const bool was_on = __is_self_read_part_enabled(n);
	if ((was_on) && (!turn_on))
		n = atomic_sub_return(N_READ_PART_DISABLED, &c->num_read_part_in_flight);
	else if ((!was_on) && (turn_on))
		n = atomic_add_return(N_READ_PART_DISABLED, &c->num_read_part_in_flight);
	_NT(t_03_read_part, "@CLNT_INSTANCE_ID, inst_name=@STR, read_part=@BOOL->@BOOL, post_n=@INT", nvmeibc_cinst_get_blok_inst_num(p), &p->dev_name.str[0], was_on, turn_on, n);
}

#define __get_gendisk_of_kernel_bio(bio) (bio_gendisk(bio))				// Can be used only on 'bio' originated by the kernel

/* Start gatherithg statistics about the IO operation.Call prior to IO execution */
static void start_stats(struct bio *bio)
{
	struct gendisk *disk = __get_gendisk_of_kernel_bio(bio);

	if (blk_queue_io_stat(disk->queue)) {
		#if KS_HAS_BIO_START_IO_ACCT
			bio_start_io_acct(bio);
		#elif KS_GENERIC_IO_ACCT
			#if KS_GENERIC_IO_ACCT_REQ_Q
				generic_start_io_acct(disk->queue, bio_data_dir(bio), bio_sectors(bio), &disk->part0);
			#else
				generic_start_io_acct(             bio_data_dir(bio), bio_sectors(bio), &disk->part0);
			#endif
		#else
			const int rw = bio_data_dir(bio);
			#if KS_HAS_PART_STAT_LOCK_CPU
				const int cpu = part_stat_lock();
			#else
				int cpu;
				part_stat_lock();
				cpu = get_cpu();
			#endif

			/* part_round_stats(cpu, &disk->part0); */
			#if KS_HAS_PART_STAT_CPU
				part_stat_inc(cpu, &disk->part0, ios[rw]);
				part_stat_add(cpu, &disk->part0, sectors[rw], bio_sectors(bio));
			#else
				part_stat_inc(     &disk->part0, ios[rw]);
				part_stat_add(     &disk->part0, sectors[rw], bio_sectors(bio));
			#endif
			#if KS_HAS_PART_INC_DEC_IN_FLIGHT
				#if KS_PART_INC_IN_FLIGHT_USES_Q
					part_inc_in_flight(disk->queue, &disk->part0, rw);
				#else
					part_inc_in_flight(             &disk->part0, rw);
				#endif
			#else
				 part_stat_local_inc(&disk->part0, in_flight[op_is_write(bio_op(bio))]);
			#endif
			part_stat_unlock();
		#endif // KS_GENERIC_IO_ACCT
	}
}

/* End gatherithg statistics about the IO operation. Call after IO execution */
static void end_stats(struct bio *bio, struct gendisk *disk, unsigned long start_time)
{
	if (blk_queue_io_stat(disk->queue)) {
		#if KS_HAS_BIO_START_IO_ACCT
			bio_end_io_acct(bio, start_time);
		#else
			#if KS_GENERIC_IO_ACCT
				#if KS_GENERIC_IO_ACCT_REQ_Q
					generic_end_io_acct(disk->queue, bio_data_dir(bio), &disk->part0, start_time);
				#else
					generic_end_io_acct(             bio_data_dir(bio), &disk->part0, start_time);
				#endif
			#else
				unsigned long duration = jiffies - start_time;
				const int rw = bio_data_dir(bio);
				#if KS_HAS_PART_STAT_LOCK_CPU
					int cpu = part_stat_lock();
				#else
					int cpu;  part_stat_lock(); cpu = get_cpu();
				#endif
				#if KS_HAS_PART_STAT_CPU
					part_stat_add(cpu, &disk->part0, ticks[rw], duration);
				#else
					part_stat_add(     &disk->part0, ticks[rw], duration);
				#endif
				/* part_round_stats(cpu, &disk->part0); */
				#if KS_HAS_PART_INC_DEC_IN_FLIGHT
					#if KS_PART_DEC_IN_FLIGHT_USES_Q
						part_dec_in_flight(disk->queue, &disk->part0, rw);
					#else
						part_dec_in_flight(             &disk->part0, rw);
					#endif
				#else
					part_stat_local_dec(&disk->part0, in_flight[op_is_write(bio_op(bio))]);
				#endif
				part_stat_unlock();
			#endif // KS_GENERIC_IO_ACCT
		#endif // KS_HAS_BIO_START_IO_ACCT
	}
}

/************* In air IO fast per_cpu counter. Used for unsafe detach ********/
#include "../../common/nvmeib_scalabale_refcount.h"
struct reqq_data {								// Saved in kernel's "struct request_queue"->queuedata
	struct scalabale_refcount	io_refcount;	// Counter of IO's currently active on the volume. also indicates the need to fail requests when it refuses the provide a reference.
	/*Daniel: Todo, here put NVMesh's elevator*/
	bio_exec_fn *bio_executor;
	u16 vol_refcount;							// Debug only field. How many vols use this ref count
};

static void* reqq_data_create(scalabale_refcount_condition _cb, bio_exec_fn *bio_fn)
{
	struct reqq_data *reqq = kmalloc(sizeof(*reqq), GFP_KERNEL);
	if (reqq) {
		scalabale_refcount_init(&reqq->io_refcount, _cb, NULL);
		reqq->bio_executor = bio_fn;
		reqq->vol_refcount = 1;
	}
	return reqq;
}

static void *reqq_data_copy_constructor(struct reqq_data *src, bool by_val)
{
	struct reqq_data *reqq;
	if (!by_val) {			// Pass by reference. Sub volume points to reqq data of volume
		reqq = src;
		reqq->vol_refcount++;
	} else {
		reqq = reqq_data_create(src->io_refcount.cond_cb , src->bio_executor);
	}
	return reqq;
}

#define reqq_data_get(q) ((struct reqq_data*)(q->queuedata))

// TODO: for Kernel 5.10+ consider spliting into two declaraitons (but we have a lot of callers that will also need refactoring)
static void reqq_data_connect_to_q(struct reqq_data *reqq, struct request_queue *q, const struct block_device_operations **fops)
{
	(void)fops;
	if (!reqq->io_refcount.ctx)
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
		reqq->io_refcount.ctx = q;			// reqdata is connected only to the owner q
#else
		reqq->io_refcount.ctx = (void*)fops;// reqdata is connected only to the owner fops
#endif
	q->queuedata = reqq;					// Other non owner q's may point to reqdata
}

static void reqq_data_destroy(struct reqq_data *reqq) {
	if (reqq) {		// If by reference then just remove ref count
		reqq->vol_refcount--;
		if (reqq->vol_refcount == 0) {
			scalabale_refcount_destroy(&reqq->io_refcount);
			kfree(reqq);
		}
	}
}

/* When new IO arrives, add ref or indicate that IO's should immediately fail
   in fact, maintains a count of IO's that started but not completed yet. */
static inline bool reqq_data_start_io(struct request_queue *q)
{
	return scalabale_refcount_get_ref(&(reqq_data_get(q)->io_refcount));
}

static inline void reqq_data_end_io(struct request_queue *q)
{
	       scalabale_refcount_put_ref(&(reqq_data_get(q)->io_refcount));
}

/* This function handles the common tasks when ending a bio
   End stats*/
static void __end_bio_common(struct gendisk *disk, struct bio *bio, int rv){
	struct request_queue *q = disk->queue;
	const struct nvmeiba_atom_os_api *atom = &block_api_os_get_os(bio)->atom;

	reqq_data_end_io(q);
	if (unlikely((rv) && nvmeiba_kapi.os_api_is_queue_orphan(atom))) {
		CALL_SUBMIT_BIO_FN(q, disk, bio);		// Last chance to save bio from error by upgrading to new nvmeibc
	} else {
		bio_endio(bio, rv);
	}
}

/* Terminate the bio */
static void __end_kernel_bio(struct bio *bio, ulong start_time, int rv)
{
	struct gendisk *disk = __get_gendisk_of_kernel_bio(bio);
#if !KS_BVEC_ITER
	if (!rv)
		bio->bi_size = 0;
#endif
	if (unlikely(disk == NULL)) {				// BUG - EC-6978:
		_NE_to_user(tr_0_osekbo, DMESG_PREFIX(""), "Unexpected FS error that may make it impossible to detach a volume, which may require a reboot to cleanup. Block device was closed while having bio=@BIO in air IOs, Error code: 1067.", bio);
		bio_endio(bio, rv);
		return;
	}
	end_stats(bio, disk, start_time);
	__end_bio_common(disk, bio, rv);
}

static void __end_512B_wrapper_replacement_bio(struct bio *wrapper_bio, unsigned long start_time, int rv)
{
	extern void sub_block_op_bio_endio(struct bio *wrapper_bio, int rv);
	struct bio *bio = get_original_bio_from_wrapper(wrapper_bio);	// The original sub-block read bio
	sub_block_op_bio_endio(wrapper_bio, rv);	// Copy the data to the original bio and do cleanup
	__end_kernel_bio(bio, start_time, rv);		// Complete the original bio
}

static void __end_rider_to_carrier_bio(struct bio *bio, ulong start_time, int rv)
{
	// For carrier BIO there is no disk use OS api and get the queue from ATOM
	const struct nvmeiba_atom_os_api *atom = &block_api_os_get_os(bio)->atom;
	struct request_queue *q = atom->queue;
	(void)start_time;
	reqq_data_end_io(q);
	if (unlikely((rv) && nvmeiba_kapi.os_api_is_queue_orphan(atom))) {
		CALL_SUBMIT_BIO_FN(q, atom->disk, bio);		// Last chance to save bio from error by upgrading to new nvmeibc
	} else {
		extern void rider_bio_endio(struct bio *bio, int rv);	// Carrier responds with this function
		rider_bio_endio(bio, rv);
	}
}

const struct nvmeibc_os_api* block_api_os_get_os(const struct bio *bio)
{
	if (!__is_bio_from_carrier(bio))
		return gendisk_get_api_os(__get_gendisk_of_kernel_bio(bio));
	else
		return (const struct nvmeibc_os_api*)((u64)bio->bi_end_io & (~1ULL)); // CARRIER_BIO
}

int block_api_os_verify_bio_geometry(const struct bio *bio)
{
	ulong sub_offset = 0, sub_len = ~0UL;							// len - Irrelevant for testing of size (volume can be auto extandable)
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	const struct nvmeibc_block_device *nd = block_api_os_get_base_bdev(os, &sub_offset, &sub_len);	// Important: os != nd->os
	const ulong lba_bio_start_s = (ulong)__GET_BI_SECTOR(bio);
	const long total_size_b = __GET_BI_SIZE(bio);
	const enum nvmeib_block_io_op op = __get_bio_op(os, bio);						//Even if sub-read is done, there will be no change to the operation

	(void)nd;

	if ((int)op < 0){// enum is uint so convert to signed error code
		return -EINVAL;
	}

	/* Subset of out of bound tests taken from operation code. Needed to avoid wrong split */
	if (unlikely((lba_bio_start_s > (1ULL << 63)) || (total_size_b == 0) ||
				 (lba_bio_start_s + (total_size_b>>KERNEL_SECTOR_SHIFT) > sub_len))){
		return -EINVAL;
	}
	return 0;
}

static const struct gendisk* block_api_os_get_gendisk(const struct bio *bio)
{
	struct gendisk *disk = __get_gendisk_of_kernel_bio(bio);	// For carrier BIO there is no disk use OS api and get the disk from ATOM, otherwise os_api might be invalid use disk
	return disk ? disk : block_api_os_get_os(bio)->atom.disk;
}

#if NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING
/* request_queue callback, Autofails all IO's. Replaces the real cb when volume is unsafe-detaching */
static REQ_RET nvmeibc_b_req_reject(struct request_queue *q, struct bio *bio)
{
	nflog(flog_api_os_nvmeibc_b_req_reject, "IO reject: bio=@BIO, off=@OFF_LLONG[s] q=@QUEUE", bio, (u64)__GET_BI_SECTOR(bio), q);
	if (!__is_bio_from_carrier(bio)) {
		bio_io_error(bio);
	} else {
		rider_bio_endio(bio, -EIO);
	}
	return REQ_RET_ZERO;
}
static __attribute__((unused)) REQ_RET nvmeibc_b_req_reject_no_q(struct bio *bio)
{
	return nvmeibc_b_req_reject(block_api_os_get_gendisk(bio)->queue, bio);
}
#endif

bool nvmeibc_bio_noexec = 0;
module_param_named(bio_noexec, nvmeibc_bio_noexec, bool, 0644);
MODULE_PARM_DESC(bio_noexec, "This is for debugging. When set to true, BIOs (kernel block IOs) are ignored instead of being executed.");

/* request_queue callback, Using this method OS reads/write/trims the disk */
static REQ_RET nvmeibc_b_req_make(struct request_queue *q, struct bio *bio)
{
	const ulong now_jiffies = jiffies;
	int rv;

	if (unlikely(nvmeibc_bio_noexec)) {
		bio_endio(bio, 0); /* Return the BIO without executing it Allows us to benchmark the user-kernel bio interface */
		return REQ_RET_ZERO;
	}

	BUG_ON(__is_bio_wrapper_for_bio(bio));	// Wrapper BIOs are issued in the bio_executor function
	/* Note: Here q->make_request_fn() / fops->submit_bio can change during upgrade/detach */
	if (reqq_data_start_io(q)) {
		if (unlikely(bio->bi_next)) {
			WARN(1, "nvmeibc: elevetor io is not supported! rejecting io");
			bio_io_error(bio);
		} else {
			nflog(flog_api_os_nvmeibc_b_req_make, "IO begin: b=@BIO, now=@NOW, off=@OFF_LLONG[s], size=@SIZE", bio, now_jiffies, (u64)__GET_BI_SECTOR(bio), (u32)__GET_BI_SIZE(bio));
			if (!__is_bio_from_carrier(bio)) { // Carrier bio does not take stats
				start_stats(bio);
				if ((rv = (reqq_data_get(q)->bio_executor)(bio, now_jiffies))){
					__end_kernel_bio(bio, now_jiffies, rv);
				}
			} else {
				if ((rv = (reqq_data_get(q)->bio_executor)(bio, now_jiffies)))
					__end_rider_to_carrier_bio(bio, now_jiffies, rv);
			}
		}
	} else {
		CALL_SUBMIT_BIO_FN(q, block_api_os_get_gendisk(bio), bio);	// submit_bio()/make_request_fn() changed, call the correct function
	}
	return REQ_RET_ZERO;
}

static __attribute__((unused)) REQ_RET nvmeibc_b_req_make_no_q(struct bio *bio)
{
	return nvmeibc_b_req_make(block_api_os_get_gendisk(bio)->queue, bio);
}

#define __dirver_ctx_of(atom) container_of(atom, struct nvmeibc_os_api, atom)->driver_context

#if NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING
static int __set_make_req_to_reject(struct nvmeiba_atom_os_api *atom)
{
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	atom->queue->make_request_fn = nvmeibc_b_req_reject;
#else
	atom->disk->fops = (const struct block_device_operations*)&__dirver_ctx_of(atom)->bdev_fops_de;
#endif
	return 0;
}
#endif

void block_api_os_stop_accepting_kernel_io(struct nvmeibc_os_api *os, u32 reason)
{
	struct nvmeiba_atom_os_api *atom = &os->atom;
	if (!os->is_io_api_disabled) {
		if (reason == 'D') {
#if NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING
			int (*set_detaching_fn)(struct nvmeiba_atom_os_api *atom) = (void *)os->atom.reserved[0];

			if (os->driver_context->atom_protocol_version > NVMEIBA_2_C_PROTO_VERSION_V_2_0) {
				BUG_ON(!set_detaching_fn);
			}

			if (set_detaching_fn) {
				__exec_for_carrier_and_sub_vols(atom, set_detaching_fn);
			} else {
				__exec_for_carrier_and_sub_vols(atom, __set_make_req_to_reject);
			}
#else
			__exec_for_carrier_and_sub_vols(atom, nvmeiba_kapi.os_api_set_detaching);
#endif
		} else {	// If carrier, abandones queue for itself and all sub volumes
			__exec_for_carrier_and_sub_vols(atom, nvmeiba_kapi.os_api_orphan_abandon);
		}
		wmb();	// make sure all cores see this ASAP
	}
}

static void __adopt_atom_redirect_new_bio(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeiba_bio_pending_list *p = &atom->pender;
	ulong flags;
	spin_lock_irqsave(&p->lock, flags);		// Prevent p->bio_list from increasing
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	atom->queue->make_request_fn = nvmeibc_b_req_make;
#else
	atom->disk->fops = (const struct block_device_operations*)&__dirver_ctx_of(atom)->bdev_fops_io;
#endif
	wmb();	// make sure all cores see this ASAP
	spin_unlock_irqrestore(&p->lock, flags);
	// New IO's redirected for execution. No one touching p->bio_list
}

static void __adopt_atom_submit_pending_list(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeiba_bio_pending_list *p = &atom->pender;
	struct bio *bio;
	const union nvmeiba_part_flags *f = &atom->sub.flags;
	struct nvmeiba_atom_os_api *fops_atom = ((f->is_sub_atom && f->is_sub_share_reqq) ? atom->sub.parent : atom);	// Send through real volume fops
	_NT(t_01_submit_plist, "@DEV_NAME: adoption completed atom=@ATOM, nios=@NIOS", atom->dev_name, atom, p->n_bios);
	// Note: Here p->bio_list cannot grow. no need to hold atom->pender->lock because new IO's do not enter the list, and ioctls of drain cannot run on main-wq
	while ((bio = bio_list_pop(&p->bio_list)) != NULL) {
		CALL_SUBMIT_BIO_FN(fops_atom->queue, fops_atom->disk, bio);
		p->n_bios--;
	}
}

int block_api_os_autofail_upgrade_io(struct nvmeibc_os_api *os, int n_autofails_orig)
{
	struct nvmeiba_atom_os_api *atom = &os->atom;
	struct nvmeiba_bio_pending_list *p = &atom->pender;
	ulong flags;
	struct bio *bio;
	int n_autofails = n_autofails_orig;
	if (atom->status != nvmeiba_status_orphan) {
		_NT(t_02_submit_plist, "@DEV_NAME: reject autofail on atom=@ATOM, status=@RV", atom->dev_name, atom, atom->status);
		return -EPERM;
	}
	_NT(t_03_submit_plist, "@DEV_NAME: autofailing orphan io atom=@ATOM, nios=@NIOS, n_autofail=@NIOS", atom->dev_name, atom, p->n_bios, n_autofails_orig);
	spin_lock_irqsave(&p->lock, flags);		// Note: Here p->bio_list can grow. So qcuire lock
	while ((n_autofails!=0) && ((bio = bio_list_pop(&p->bio_list)) != NULL)) {
		p->n_bios--;
		spin_unlock_irqrestore(&p->lock, flags);
		bio_io_error(bio);
		n_autofails--;
		spin_lock_irqsave(&p->lock, flags);
	}
	spin_unlock_irqrestore(&p->lock, flags);
	return (n_autofails_orig-n_autofails);				// Number of autofailed io's
}

static void __sub_vol_inherit_carrier_fields(struct nvmeibc_os_api *sub, const struct nvmeibc_os_api *car)
{
	sub->slice_size = car->slice_size;			// Currently this is the only important field, other than that all fields in os_api except for atom itself are zero
	WARN_ON(sub->slice_size == 0);				// Illegal configuration
}

static int __atom_adoption_start_accepting_io(struct nvmeiba_atom_os_api *atom)		// Adoption completed
{
	if (atom->sub.flags.is_sub_atom)
		__sub_vol_inherit_carrier_fields((void*)atom, (void*)atom->sub.parent);
	__adopt_atom_redirect_new_bio(atom);		// Sub atoms use carriers request queue, so new IO's always go there
	__adopt_atom_submit_pending_list(atom);		// Daniel: Todo, if a lot of bio, maybe delay to external context?
	atom->status = nvmeiba_status_live;			// orphan --> live
	_NT(t_11_api_os_start_bio, "@DEV_NAME: adoption completed, atom=@ATOM", atom->dev_name, atom);
	return 0;
}

/* IO is being drained, because either due to detach or due to upgrade */
static bool __is_nvmeibc_bdev_destroying(struct scalabale_refcount *s, void *ctx)
{
	(void)s;
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	return ((volatile struct request_queue*)ctx)->make_request_fn != nvmeibc_b_req_make;
#else
	return (*(volatile struct block_device_operations**)ctx)->submit_bio != nvmeibc_b_req_make_no_q;
#endif
	return true;
}

static int nvmeiba_atom_drain_io(struct nvmeiba_atom_os_api *atom)
{
	struct request_queue *q = atom->queue; 		// Same as atom->disk->queue;
	const union nvmeiba_part_flags *f = &atom->sub.flags;
	struct scalabale_refcount *cnt = &(reqq_data_get(q)->io_refcount);
	const bool should_skip_draining = (f->is_sub_atom && (!f->is_owner_of_reqctx));	// Impossible to drain incomming IO's via queue context because they are flowing to the carrier (directly or via other sub volumes). No need to drain because Carrier will serve the IOs anyway
	int	i = 1, n_ios;

	if (unlikely(should_skip_draining)) {
		return -EPERM;
	}
	for (; (n_ios = scalabale_refcount_get_count(cnt)) > 0; i++) {
		if ((i&0xFF) == 0) {		// Print once every 256[msecs]
			_NT(trace_api_os_nvmeiba_atom_drain_io, "@DEV_NAME: detach iter=@ITER, nios=@NIOS", atom->dev_name, i, n_ios);
			schedule();
		}
		msleep(1);		// Not interrupt context
	}
	return 0;
}

#define ASSERT_ATOM_STATUS(a, st) \
	WARN((a)->status != st, "nvmeibc bug, atom=%s status=%d\n", (a)->dev_name, (a)->status)

static int __set_atom_status_orphan(struct nvmeiba_atom_os_api *atom)		// Abandoning completed
{
	atom->status = nvmeiba_status_orphan;
	return 0;
}

static int __set_atom_status_detaching(struct nvmeiba_atom_os_api *atom)
{
	atom->status = nvmeiba_status_detaching;
	return 0;
}

void block_api_os_drain_io(struct nvmeibc_os_api *os)
{
	struct nvmeiba_atom_os_api *atom = &os->atom;
	bool is_abandoning = false;							// Default, not abandining, just detaching
	__verify_on_main_wq_osapi(os);							// Draining should be done not in interrupt context
	if (os->is_io_api_disabled) {
		ASSERT_ATOM_STATUS(atom, nvmeiba_status_hidden);
		_NT(trace_api_os_block_api_os_drain_io, "@DEV_NAME: does NOT require IO quiesce", atom->dev_name);
	} else {
		int rv;
		ASSERT_ATOM_STATUS(atom, nvmeiba_status_live);
		WARN(!__is_nvmeibc_bdev_destroying(NULL, CALL_BIO_Q_CONTEXT(atom)), "nvmeibc bug\n");	// Wrong draining sequence. First call block_api_os_stop_accepting_kernel_io(), then drain the rest
		rv = __exec_for_carrier_and_sub_vols(atom, nvmeiba_atom_drain_io);
		is_abandoning = nvmeiba_kapi.os_api_is_queue_orphan(atom);
		_NT(trace_1_api_os_block_api_os_drain_io, "@DEV_NAME: Draining ios=@RV. q=@QUEUE", atom->dev_name, rv, atom->queue);
	}
	if (is_abandoning) {
		__exec_for_carrier_and_sub_vols(atom, __set_atom_status_orphan);
	} else {
		__exec_for_carrier_and_sub_vols(atom, __set_atom_status_detaching);
	}
}

/******************************************************************************/
static void __set_max_trim(struct request_queue *q, u32 m, const char* name)
{
	if (q->limits.max_discard_sectors != m) {
#if !KS_BLOCK_DEV_DEVICE_TRIM_LIMIT || KS_BLK_ALLOC_DISK_2PARAMS
		q->limits.max_discard_sectors = m;
#else
		blk_queue_max_discard_sectors(q, m);
#endif
		_NT(trace_api_os_set_max_trim, "@DEV_NAME: max trim size updated to @SIZE[Kb]", name, (m>>1));	// Can see and reduce through /sys/block/nvmesh/name/queue/discard_max_bytes
	}
}

static void __set_max_rw_io(struct request_queue *q, const char* name, const int slice_size)
{
	int m = NVMEIBS_MAX_IO_CHANNEL_MSGS*slice_size;	// Transport sg constraint per slice member
	m <<= KERNEL_SECTOR_TO_SECTOR_SHIFT;	// Kernel units
	_NT(trace_api_os_set_max_rw_io, "@DEV_NAME: max rw kernel size updated to @SIZE[Kb]", name, (m>>1));	// Can see and reduce through /sys/block/nvmesh/name/queue/max_sectors_kb
#if KS_BLK_ALLOC_DISK_2PARAMS
	q->limits.max_hw_sectors = m;
#else
	blk_queue_max_hw_sectors(q, m);
#endif
}

#if KS_LEGACY_API_blk_queue_flag
	#define blk_queue_flag_set   queue_flag_set_unlocked
	#define blk_queue_flag_clear queue_flag_clear_unlocked
#endif

typedef struct __request_queue_params
{
	bool is_trim_disabled;
	const int slice_size;
	bool is_512B_IO_allowed;
} request_queue_params;

static inline request_queue_params __init_request_queue_params(struct nvmeibc_os_api *os)
{
	const request_queue_params rv = { .is_trim_disabled   = os->is_trim_disabled, .slice_size = os->slice_size,
									  .is_512B_IO_allowed = nvmeibc_block_is_kernel_sector_io_allowed(os->dev)};
	return rv;
}

static void __request_queue_set_default_params(struct request_queue *q, const char *dev_name, const request_queue_params params)
{
	// Queue should either be 512B or 4KB, no other options
	const unsigned int logical_block_size = (params.is_512B_IO_allowed) ? (1 << KERNEL_SECTOR_SHIFT) : NVMEIBC_SECTOR_SIZE;
	_ND(t_request_queue_set_default_params_1, "Setting request queue block size to (@INT64)", logical_block_size);

#if KS_BLK_ALLOC_DISK_2PARAMS
	q->queue_flags = NVMESH_QUEUE_FLAG_DEFAULT | QUEUE_FLAG_NOMERGES;
#else
	q->queue_flags = NVMESH_QUEUE_FLAG_DEFAULT;
	blk_queue_flag_set(QUEUE_FLAG_NOMERGES, q);			// Danie: Not good!!! Revisit this
	blk_queue_flag_set(QUEUE_FLAG_NONROT  , q);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, q);
#endif
	/* Register the method through which OS reads and writes to the disk */
#if !KS_HAS_NEW_BLK_ALLOC_QUEUE && KS_REQUEST_QUEUE_HAS_REQUEST_FN
	blk_queue_make_request(q, nvmeibc_b_req_make);	// make_request_fn = nvmeibc_b_req_make, + set default queue limits
#endif
#if KS_BLK_ALLOC_DISK_2PARAMS
	q->limits.logical_block_size = logical_block_size;
	q->limits.physical_block_size = logical_block_size;
	q->limits.io_min = logical_block_size;
	q->limits.io_opt = BYTES_IN_LOCKSET * params.slice_size;
#else
	blk_queue_logical_block_size( q, logical_block_size);
	blk_queue_physical_block_size(q, logical_block_size);
	blk_queue_io_min(   		  q, logical_block_size);
	blk_queue_io_opt(   		  q, (BYTES_IN_LOCKSET * params.slice_size));
#endif
	__set_max_rw_io(              q, dev_name, params.slice_size);

	/* Set configuration for trims */
#if KS_Q_LIMITS_HAS_DISCARD_ZEROS
	q->limits.discard_zeroes_data	= 0;
#endif
	q->limits.discard_alignment =   NVMEIBC_SECTOR_SIZE;
	q->limits.discard_granularity = NVMEIBC_SECTOR_SIZE;
	__set_max_trim(q, to_kenrel_sects(max_trim_size_non_mirrored), dev_name);

/* only set on kerrnels where QUEUE_FLAG_DISCARD defined - otherwise blk_queue_max_discard_sectors def should be enough */
#ifdef QUEUE_FLAG_DISCARD
	if (params.is_trim_disabled) blk_queue_flag_clear(QUEUE_FLAG_DISCARD, q);
	else						 blk_queue_flag_set(  QUEUE_FLAG_DISCARD, q);
#endif
}

static inline void ALLERT_NAME_TRUNCATION(bool cond, const char *src, const char *dst)
{
	if (cond)
		_NE_to_user(trace_36_api_os, DMESG_PREFIX("@DEV_NAME"), "The block device path for a volume is longer than allowed by the operating system, so truncating the volume name. Error code: 1028. Truncated Volume: @DEV_NAME.", src, dst);
}

static int __gendisk_set_name(struct nvmeiba_atom_os_api *atom, const char *dir_lsblk)
{
	struct gendisk *disk = atom->disk;
	int n;
	if (likely(dir_lsblk) && dir_lsblk[0] != '\0')
		n = snprintf(disk->disk_name, DISK_NAME_LEN, "%s/%.30s", dir_lsblk, atom->dev_name);
	else
		n = snprintf(disk->disk_name, DISK_NAME_LEN, "%.30s", atom->dev_name);
	disk->disk_name[DISK_NAME_LEN-1] = 0;
	ALLERT_NAME_TRUNCATION((n > DISK_NAME_LEN), atom->dev_name, disk->disk_name);
	{		// Safety measurement. Make sure that gen_disk name is still unique, otherwise we are going to get a crash in sys-fs
		const struct nvmeibc_os_api *os = container_of(atom, struct nvmeibc_os_api, atom);
		if (block_api_os_check_if_bdev_already_exists(os)) {
			_NE_to_user(trace_26_api_os, DMESG_PREFIX("@DEV_NAME"), "The block device path for a volume already exists under /dev/ for another volume, so aborting generation of the block device. Error code: 1029.", disk->disk_name);
			return -EALREADY;
		}
	}
	return 0;
}

static int __adopt_queue_and_disk_of_sub_vol(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeiba_atom_os_api *car = atom->sub.parent;
	struct request_queue *q = atom->queue;
	void *q_data = reqq_data_copy_constructor(car->queue->queuedata, atom->sub.flags.is_owner_of_reqctx);
	atom->disk->fops = car->disk->fops;
	if ((q_data)&&(q)) {
		reqq_data_connect_to_q(q_data, q, &atom->disk->fops);
		return 0;
	} else {
		return -ENOMEM;
	}
}

static int __adopt_queue_and_disk_carrier_with_sub_vol(struct nvmeiba_atom_os_api *atom, void *q_data, const struct nvmeibc_os_apis_container *c)
{
	int rv = -ENOMEM;
	// Conenct to existing queue
	reqq_data_connect_to_q(q_data, atom->queue, &atom->disk->fops);
	atom->disk->fops = &c->bdev_fops_io;
	rv = __exec_for_each_sub_vol(atom, __adopt_queue_and_disk_of_sub_vol);
	return rv;
}

static inline void block_api_os_destroy_on_init_error_io_never_started(struct nvmeibc_os_api *os, int rv, const char *dev_name, const char *fn_name)
{
	_NE(t5_sub_vol_create, DMESG_PREFIX("@DEV_NAME") ": error=@RV in function @STR", dev_name, rv, fn_name);
	if (os) {
		os->is_init_error = true;						// In most cases this is already 'true' except from when upgrading hidden volume to visible (in this case init succeeded but we reinit it
		if (!os->atom.sub.flags.is_sub_atom)			// Much like Virtual destructor
			block_api_os_destroy(os);
		else
			block_api_os_destroy_sub_vol(&os->atom);
	}
}

static struct request_queue * __alloc_disk_and_maybe_queue(struct nvmeiba_atom_os_api *atom, bool should_add_q)
{
	#if KS_HAS_BLK_ALLOC_DISK
		#if KS_BLK_ALLOC_DISK_2PARAMS
			atom->disk = blk_alloc_disk(NULL, NUMA_NO_NODE);
		#else
			atom->disk = blk_alloc_disk(NUMA_NO_NODE);
		#endif
		BUG_ON(!should_add_q);					// Todo: disk is added with queue, free the queue and only let disk remain
		atom->queue = atom->disk->queue;
		atom->disk->queue = NULL;				// We will use atom->queue. For sub volumes this may be the queue of parent
	#else
		/* Yoav Cohen: on newer kernel versions will just use BLOCK_EXT_MAJOR, actually
		this what eventully happens for other kernels due to GENHD_FL_EXT_DEVT
		We may really support our own major but doesn't seems urgent */
		atom->disk = alloc_disk(0);				// We have only 1 minor, all OS api's share it. Sub vols act identically as carrier
		if (should_add_q) {
			#if KS_HAS_NEW_BLK_ALLOC_QUEUE
				atom->queue = blk_alloc_queue(nvmeibc_b_req_make, NUMA_NO_NODE);
			#else
				atom->queue = blk_alloc_queue(                    GFP_KERNEL);
			#endif
		}
	#endif
	return atom->queue;
}

int block_api_os_init(struct nvmeibc_os_api *os, bio_exec_fn *fn, ulong size,
		      bool is_read_only, const int slice_size,
		      u64 ro_header_sectors)
{
	const struct nvmeibc_os_apis_container *c = os->driver_context;
	struct nvmeiba_atom_os_api *atom = &os->atom;
	struct request_queue *q = NULL;
	void *q_data = NULL;
	struct gendisk *disk = NULL;
	int rv = 0;

	os->slice_size = slice_size;						// v2.0.2 and before this field did not exist. Save even for hidden volumes
	WARN_ON(os->slice_size == 0);						// Illegal configuration
	/* If IO API is disabled, all its sub API's are disabled as well */
	if (os->is_io_api_disabled)
		goto _out;

	os->ro_header_sectors = ro_header_sectors;

	if (!(os->stats = nvmeib_io_stats_create_traced(atom->dev_name, VERB_RW_T_BITMASK, NVMEIBC_SECTOR_SIZE))) {
		rv = -ENOMEM | 0x1000;
		goto _out;
	}

	q_data = reqq_data_create(__is_nvmeibc_bdev_destroying, fn);
	if (!q_data) {
		rv = -ENOMEM | 0x2000;
		goto _out;
	}

	if (atom->status == nvmeiba_status_orphan) {
		rv = __adopt_queue_and_disk_carrier_with_sub_vol(atom, q_data, c);
		if (rv < 0) {
			goto _out;
		}
		if ((u32)atom->disk->major != c->drv_ver.nvmeibc_major) {
			_NT(t_baoi_03, "@DEV_NAME major=@DEV_MAJOR change to @DEV_MAJOR", atom->disk->disk_name, atom->disk->major, c->drv_ver.nvmeibc_major);
			// atom->disk->major = c->drv_ver.nvmeibc_major;
		}
		goto __success_init;
	}

	/************** Below, initialization of atom ************/
	q = __alloc_disk_and_maybe_queue(atom, true);
	if (unlikely(!q || !atom->disk)) {
		rv = -ENOMEM | 0x3000;
		goto _out;
	}

	__request_queue_set_default_params(q, atom->dev_name, __init_request_queue_params(os));
	atom->conf.enforce_readonly = true;		// Default is true
	disk = atom->disk;			/* Just for short writing */
	disk->major = nvmeibc_use_block_external_major ? 0 : c->drv_ver.nvmeibc_major;	// In kernel 5.15+ setting major without minors is illegal: https://elixir.bootlin.com/linux/v5.15.165/source/block/genhd.c#L416
	disk_id_allocator_alloc(get_dia(atom), atom->disk, atom->dev_name);
	if (no_part_scan || IS_PATH_VDISK(atom->dev_name)) {
		/* Avoid udevd reading our volumes as it not handle io disable well and may stuck */
		disk->flags |= GENHD_FL_NO_PART;
	}
	disk->fops = &c->bdev_fops_io;	/* Since IO can now be enabled use IO-able fops */
	disk->private_data = atom;
	disk->queue = q;
	if ((rv = __gendisk_set_name(atom, c->drv_ver.dir_lsblk)) < 0)
		goto _out;

	reqq_data_connect_to_q(q_data, q, &disk->fops);	// The address of disk->fops remains the same, and we init it right after

	set_capacity(disk, (size<<KERNEL_SECTOR_TO_SECTOR_SHIFT));
	os->atom.users.readonly = is_read_only;

	if (!os->is_io_config_api_disabled)
		block_api_os_change_mirorring(os->dev);

	atom->status = nvmeiba_status_live;
__success_init:
	disk = atom->disk;
	_NI(trace_api_os_block_api_os_init, DMESG_PREFIX("@DEV_NAME") ": ver=(@DEV_MAJOR:@VOL_ID)/@GENDISK_MAJOR dev=@DEV, osapi=@PTR, registering with OS...", disk->disk_name, MAJOR(disk_devt(disk)), MINOR(disk_devt(disk)), disk->major, os->dev, os);
_out:
	if (unlikely(rv))
		block_api_os_destroy_on_init_error_io_never_started(os, rv, atom->dev_name, __FUNCTION__);
	else
		os->is_init_error = false;
	return rv;
}

static int __add_disk_io_starts_b4_func_ends(struct nvmeibc_os_api *os)
{
	struct gendisk *disk = os->atom.disk;
	const sector_t size = get_capacity(disk);
	int rv;

	/* Calling add_disk() with non zero capacity will start blocking IO
	   (possibly deadlock). So we add it with zero capacity. More info here:
	   http://stackoverflow.com/questions/13518404/add-disk-hangs-on-insmod */
	set_capacity(disk, 0);
	atomic_set(&os->atom.gendisk_status, 2);	// Daniel: For debug
	// Call block_api_os_check_if_bdev_already_exists once more and abort is final opt
	// return negative rv in this case and call block_api_os_destroy_on_init_error_io_never_started
	// after setting dev->os = NULL;
#if KS_ADD_DISK_INT_RV
	if ((rv = add_disk(disk))) {
		_NE_dmesg(err_nvmeibc_block_api_add_disk_failed,
			  DMESG_PREFIX("@DEV_NAME") ": add_disk failed (@RV)", disk->disk_name, rv);
		goto out;
	}
#else
	add_disk(disk);
#endif
	set_capacity(disk, size);			// Kernel may send IO now
	ASSERT_ATOM_STATUS(&os->atom, nvmeiba_status_live);	// live from the init function
	atomic_set(&os->atom.gendisk_status, 3);
	rv = 0;

#if KS_ADD_DISK_INT_RV
out:
#endif
	return rv;
}

int block_api_os_start_accepting_kernel_io(struct nvmeibc_os_api *os)
{
	struct nvmeibc_block_device *dev = os->dev;
	struct nvmeiba_atom_os_api *atom = &os->atom;
	sector_t size;
	int rv = 0;

	if (os->is_io_api_disabled)
		return 0;

	size = get_capacity(atom->disk);
	_NT(t_01_api_os_start_bio, "@DEV_NAME: start bio @GENDISK sz=@SZ[sectors], @DEV_SIZE[blks]", atom->dev_name, atom->disk, (ulong)size, (ulong)(size>>KERNEL_SECTOR_TO_SECTOR_SHIFT));
	if (atom->status == nvmeiba_status_orphan) {
		__exec_for_carrier_and_sub_vols(atom, __atom_adoption_start_accepting_io);
	} else {
		if (__add_disk_io_starts_b4_func_ends(os)) {
			rv = -1;
			goto _out;
		}
	}

	_NI(t_02_api_os_start_bio, DMESG_PREFIX("@DEV_NAME") ": started accepting kernel_io, ver=(@DEV_MAJOR:@VOL_ID)", atom->disk->disk_name, MAJOR(disk_devt(atom->disk)), MINOR(disk_devt(atom->disk)));

	if (atom->sub.flags.is_sub_atom)			// Daniel: currently equals to (!dev)
		goto _out;			// Sub volumes, skip revalidation. Daniel: Todo, maybe enable?

	if (nvmeibc_topologies_are_reads_enabled(&dev->topologies)) {
		block_api_os_async_revalidate(dev); /* In rare cases Toma's
		   were quick to give register ACKs and IO was enabled and
		   __revalidate_and_reread_work() probably failed because gen disk
		   was not ready - We must Revalidate and reread partitions here
		   Daniel: There is a race condition where, disk will be revalidated
		   twice (once when IO is enabled, second time here). So what... */
	} /* else, Will be revalidated when IO is enabled */
_out:
	return rv;
}

const struct nvmeibc_block_device* get_bdev_of_bio(const struct bio *bio)
{
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	return (const struct nvmeibc_block_device*)(os->dev);
}

//TODO: very bad interface; should be split to two functions
struct nvmeibc_block_device * block_api_os_get_base_bdev(const struct nvmeibc_os_api* os, ulong *sub_offset, ulong *sub_len)
{
	const bool is_sub = (os->atom.sub.flags.is_sub_atom);
	if (likely(!is_sub))
		return (struct nvmeibc_block_device *)os->dev;
	if (sub_offset){
		*sub_offset = (os->atom.sub.offset >> KERNEL_SECTOR_SHIFT);		// Always zero for non sub volume
	}
	if (sub_len){
		*sub_len = get_capacity(os->atom.disk);
	}
	return (struct nvmeibc_block_device *)((struct nvmeibc_os_api*)os->atom.sub.parent)->dev;
}

struct nvmeibc_block_device* get_bdev_or_parent_bdev_of_bio(const struct bio* bio)
{
	const struct nvmeibc_os_api *os = block_api_os_get_os(bio);
	struct nvmeibc_block_device* bdev = block_api_os_get_base_bdev(os, NULL, NULL);
	BUG_ON(bdev == NULL);
	return bdev;
}

static void __kill_gen_disk(struct nvmeiba_atom_os_api *atom)
{
	struct gendisk *disk;
	unsigned long flags;
	spin_lock_irqsave(&atom->disk_lock, flags);
	disk = atom->disk;
	atom->disk = NULL;	// the nvmeibc_block_device object on which we act is freed right after
	spin_unlock_irqrestore(&atom->disk_lock, flags);
	if (likely(disk)){ 		// partially constructed atom object
		if (is_gendisk_ready_for_io(atom)) {	// Disk was fully added, must remove it normally
			del_gendisk(disk);	// mark the device is being shutdown, so no new references to it can be taken
			disk_id_allocator_free(get_dia(atom), disk);
#if KS_HAS_BLK_CLEANUP_DISK
			/* This function does the put and the kobject release fn
			 * will destroy the queue as well as the disk */
			blk_cleanup_disk(disk);
#else
			put_disk(disk);		// eliminate our reference to gendisk, should result deletion of the gendisk unless IO's are in-flight
#endif
		} else {
			kfree(disk);		// Cleanup on creation problem
		}
	}
}

/* Destructor of ATOMs resources shared with nvmeibc */
static void nvmeiba_atom_io_resources_destructor(struct nvmeiba_atom_os_api *atom)
{
	__kill_gen_disk(atom);
	if (atom->queue) {
#if !KS_HAS_BLK_ALLOC_DISK && !KS_HAS_BLK_CLEANUP_DISK
		blk_cleanup_queue(atom->queue);
#else
		/* Queue is cleaned up by blk_cleanup_disk or by the newer (kernel 6.5+) implementation of put_disk */
#endif
		atom->queue = NULL;
	} else { /* Hidden attach / Sub volume with shared queue / destroy upon creation failure */}
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR == 1)
#define ASSERT_COUNTERS(...) NVMESH_BUG(__VA_ARGS__)
#else
#define ASSERT_COUNTERS(...) NVMESH_WARN(__VA_ARGS__)
#endif

static void __assert_no_inflight_io(struct nvmeibc_os_api *os)
{
	int i;

	if (!os->stats) {
		_NT(trace_api_os_assert_no_inflight_io, "@DEV_NAME: stats not initialized", os->atom.dev_name);
		return;
	}

	for (i = 0; i < IO_STAT_VERB_DISCARD; i++) {
		struct nvmeib_io_counters counters = {0};
		nvmeib_io_stats_readc(os->stats, i, 0 /* All sizes */, &counters);
		ASSERT_COUNTERS(counters.inflight_ops > 0, NO_REPORT, NULL,
				"nvmeibc bug: %s: inflight IO's found in stats verb=%d, inflight=%d\n",
				os->atom.dev_name, i, counters.inflight_ops);
	}

}

static void __proc_destroy(struct nvmeibc_os_api *os);
void block_api_os_destroy(struct nvmeibc_os_api *os)
{
	struct nvmeiba_atom_os_api *atom = &os->atom;
	struct reqq_data *rq_ctx = (atom->queue) ? atom->queue->queuedata : NULL;	// Store pointer on stack coz we may loose reference to it
	const bool is_io_api_disabled = os->is_io_api_disabled;
	const bool can_other_threads_open_atom = (atom->disk && is_gendisk_ready_for_io(atom));

	// Just asserts for flow correctness
	const bool expect_invisible_atom = ((is_io_api_disabled) || (atom->status == nvmeiba_status_hidden));		// Intended to be hidden or failed to reach the state of initialized gen_disk
	WARN((expect_invisible_atom == can_other_threads_open_atom), "nvmeiba error e=%d, c=%d", expect_invisible_atom, can_other_threads_open_atom);
	if (can_other_threads_open_atom) {
		if (os->is_init_error) {
			ASSERT_ATOM_STATUS(atom, nvmeiba_status_orphan);	// The only possibility is error while adopting atom after live upgrade
			// Daniel: Todo, think how to handle this case. It is very tricky, In fact attach of live upgrade causes force-detach.
		}
		if (atom->status != nvmeiba_status_orphan)
			WARN(os->unsafe_self_ref.bdev_during_detach == NULL, "nvmeibc flow error\n");		// 'os' will get kfree() on last close(). So must hold self-reference, or else os can get kfree() via a close() operation from other context and we crash
	}

	/* Here atom is starting to loose support of nvmeibc_os_api as it is destroyed */
	if (is_io_api_disabled) {									// IO Resources never allocated, Atom cannot be orphan/live
		if (os->is_init_error) {
			ASSERT_ATOM_STATUS(atom, nvmeiba_status_hidden);	// Sanity assertion. Uppon hidden attach error
		} else {
			ASSERT_ATOM_STATUS(atom, nvmeiba_status_detaching);	// Sanity assertion. Uppon hidden detach.
		}
	} else if ((atom->status == nvmeiba_status_orphan)&&(!os->is_init_error)) {		// Resources should not be freed, left for upgrade
		atom->queue->queuedata = NULL;							// For debug: Queue always exists. Disconnect nvmeibc context from nvmeiba queue
		__exec_for_each_sub_vol(atom, block_api_os_destroy_sub_vol);
	} else {													// Regular detach / init error / adopt error
		if (!os->is_init_error)
			ASSERT_ATOM_STATUS(atom, nvmeiba_status_detaching);	// Sanity assertion. If no init error and os_api enabled then atom was live and became detaching
		__exec_for_each_sub_vol(atom, block_api_os_destroy_sub_vol);			// Recursively call destructor on each sub volume
		nvmeiba_atom_io_resources_destructor(atom);
	}
	_NT(trace_api_os_block_api_os_destroy, "@DEV_NAME request_queue destroyed: refcount=@REFCOUNT", atom->dev_name, rq_ctx ?
	   scalabale_refcount_get_count(&rq_ctx->io_refcount) : -ENODEV);
	if (!os->is_proc_api_disabled)
		__proc_destroy(os);
	if (os->stats) {			// No more IO, nor /proc files so stats are not used regardless of ref-count.
		__assert_no_inflight_io(os);	// Assert that no IO is inflight, so stats are not used anymore
		nvmeib_io_stats_free(os->stats);
		os->stats = NULL;
	}
	reqq_data_destroy(rq_ctx);	// OK to free q context because atom either autofails or enqueues IOs and does not aware of the context
	os = NULL; 					// Dont use 'OS' anymore. It does not exist, and might kfree thorugh atom destructor asyncronously!

	if (!can_other_threads_open_atom)	{	// Free atom inline. LKJ: Replace the condition and destructor to ref_put()
		nvmeiba_kapi.os_api_destructor(atom); /* Kernel does not use us. No unsafe detach */
		atom = NULL;		 /* Both were kfree(), dont use them */
	} else if (atom->status == nvmeiba_status_orphan) {
		/* Dont call __exec_for_carrier_and_sub_vols(atom,destructor). Each atom holds resources for future adoption or will free itelf on adoption error */
	} else {
		atom = NULL; /* Destructor, is auto called upon last close() on atom. os/atom, might already be kfree() */
	}
}

void block_api_os_end_io(struct bio_part *cur, unsigned long start_time, int rv)
{
	if (unlikely(__is_bio_from_carrier(cur->bio)))
		__end_rider_to_carrier_bio(cur->bio, start_time, rv);
	else if (unlikely(__is_bio_wrapper_for_bio(cur->bio)))
		__end_512B_wrapper_replacement_bio(cur->bio, start_time, rv);
	else
		__end_kernel_bio(cur->bio, start_time, rv);
}

#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_part.inc.c"

enum nvmeib_block_io_op __get_bio_op(const struct nvmeibc_os_api* os, const struct bio *bio)
{
	const bool ro_vol = os->atom.users.readonly;
	int op = -EPERM;

	if (bio_data_dir(bio) == READ) {
		op = NVMEIB_BLOCK_IO_OP_READ;
	} else if (bio->bi_iter.bi_sector < os->ro_header_sectors) {
			_NW(warn_get_bio_op_write_on_header,
			    DMESG_PREFIX( "@DEV_NAME") ": attempt to write on block encryption header",
			    os->atom.dev_name);
			op = -EROFS;
	} else if (ro_vol) {
		_NW(warn_api_os_get_bio_op, DMESG_PREFIX("@DEV_NAME") ": Attempt to write/trim on read-only volume", os->atom.dev_name);
		op = -EROFS;
#ifdef REQ_OP_BITS
	} else if (unlikely(bio_op(bio) == REQ_OP_DISCARD)) {
#elif defined(BIO_DISCARD)
	} else if (unlikely(bio->bi_rw & BIO_DISCARD)) {
#else
	} else if (unlikely(bio->bi_rw & REQ_DISCARD)) {
#endif
		op = NVMEIB_BLOCK_IO_OP_DISCARD;
	} else {
		op = NVMEIB_BLOCK_IO_OP_WRITE;
	}
	return op;
}


u64 block_api_os_get_max_supported_trim_blks(struct nvmeibc_os_api *os)
{
	return ((u64)os->atom.queue->limits.max_discard_sectors >>
			KERNEL_SECTOR_TO_SECTOR_SHIFT);
}

/***************** IO Usage Statistics to send to mcs  ***********************/
#define IOSTATS_PROC_FRMT_VER 2 /* Bumped to 2 due to fix for [NVMESH-6726] */
/* Callback - dumps statistics (generated by a terminated IO) into a buffer.
   One callback for all types of volumes. */
static ssize_t iostats_to_non_json(void *_os, char *buf, size_t len)
{
	const struct nvmeibc_os_api *os = _os;
	const u64 io_prob = ((u64)((struct nvmeibc_block_device *)os->dev)->dp.io_perm_alert.stats.longest_io_problem_duration_msec);	// Daniel: This is very ugly, think of a better solution
	const ulong cur_time = get_nvmeibc_os_api_uptime(os);
	ssize_t count = 0;

	count = nvmeib_iostats_sum_to_string(os->stats, cur_time, io_prob, buf, len);

	count += nvmeib_proc_add_txt_proc_epilog(IOSTATS_PROC_FRMT_VER, buf + count, len - count);

	return count;
}

static ssize_t iostats_detailed_to_json(void *_os, char *buf, size_t len)
{
	#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	const struct nvmeibc_os_api *os	= _os;
	ssize_t count  = 0, indent = 0;	//, max_padd = (ssize_t)3600; 	// Padd to 3.6[KB], coz why not?
	const struct nvmeib_json_ops *jops = &nvmeib_json_ops;
	const u32 hex_type = ((struct nvmeibc_block_device *)os->dev)->type;	// Daniel: This is very ugly, think of a better solution
	if (unlikely(!os->stats)) {
		BUF_ADD("{}\n");	/* Empty JSON */
		goto _out;
	}
	count += jops->start_obj( buf + count, len - count, NULL,                                   indent++);
	count += nvmeib_io_stats_to_json(os->stats, buf+count, len-count, get_nvmeibc_os_api_uptime(os), jops, indent,
										false);
	count += jops->data_uval( buf + count, len - count, "type", hex_type    , false         ,   indent);
	count += jops->data_str(  buf + count, len - count, "uuid", os->dev_uuid, JSON_LAST_ELEM,   indent);
	count += nvmeib_proc_add_json_proc_epilog(IOSTATS_PROC_FRMT_VER, buf + count, len - count);
	count += jops->end_obj(   buf + count, len - count,                       JSON_LAST_ELEM, --indent);
_out:
	//WARN(count > max_padd, "nvmeibc wrong padding: %lld > %lld\n", (s64)count, (s64)max_padd);
	//count += jops->right_padd(buf + count, len - count, max_padd - count);
	return count;
	#undef BUF_ADD
}

#define IO_THROTTLE_PROC_FRMT_VER 1
static ssize_t io_throttle_to_string(void *_dev, char *buf, size_t len)
{
#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	struct nvmeibc_block_device *dev = _dev;
	ssize_t	count = 0;
	ulong flags, now = jiffies;
	int	cpu_id;

	if (unlikely(nvmeibc_block_status_is_detaching(dev->status))) {
		BUF_ADD("Detaching...\n");
		goto _out;
	}

	// dump number of IO's in per-cpu waiting list & the first IO waiting in queue.
	BUF_ADD("per-cpu io-wait-list: %d\n", (int)MAX_NUM_ACTIVE_CPUS);
	for_each_allocated_cpu(cpu_id) {
		struct topo_percore_shared *tps = &dev->topologies.percore_shared[cpu_id];
		spin_lock_irqsave(&tps->list_access, flags);
		if (tps->n_wait_list > 0) {
			const struct operation *o = list_first_entry(&tps->io_wait_list, struct operation, per_cpu_wait_list);
			BUF_ADD("cpu %3d: n_io=%d, o=%p, op=%d, %lu[msec]\n", cpu_id, tps->n_wait_list, o, o->op, ((ulong)(now - o->jiffies1) * 1000)/HZ);
		} else {
			//BUF_ADD("cpu %3d: empty\n", cpu_id);
		}
		spin_unlock_irqrestore(&tps->list_access, flags);
	}
	count += nvmeib_proc_add_txt_proc_epilog(IO_THROTTLE_PROC_FRMT_VER,
						  buf + count, len - count);
_out:
	return count;
#undef BUF_ADD
}

static ssize_t __empty_tostring(void *_context, char *buf, size_t len)
{
	(void)_context; (void)buf; (void)len;
	return 0;
}
#define __set_default_if_null(func) if (!(func)) func = __empty_tostring

/* Create access to proc directory and its read only files */
#define RO_proc_open(name, p, cb, param) /* Create read only file */ \
               nvmeib_public_proc_create(name, (p)->dir, cb, NULL /* No write */, param)

static int __proc_create(struct nvmeibc_os_api *os, struct nvmeibc_procfs_cb cb)
{
	/* Create files named after the volumes. When user reads it, the
	   file displays the content of buffer filled by cb.
	   Warning: Don't touch file names!!! they are used in auto scripts*/
	struct nvmeibc_procfs *p = &os->procfs;

	__set_default_if_null(cb.dev_status_to_txt);
	__set_default_if_null(cb.dev_status_to_json);
	__set_default_if_null(cb.dev_recovs_to_txt);
	__set_default_if_null(cb.flows_cntr_to_json);
	__set_default_if_null(cb.profiling_to_string);
	__set_default_if_null(cb.profiling_to_csv);
	__set_default_if_null(cb.ext_blob_to_txt);
	p->dir = proc_mkdir(os->atom.dev_name, os->driver_context->proc_root);
	if (!p->dir)
		goto _out;

	p->io_st_sum= RO_proc_open("iostats"    ,      p, iostats_to_non_json         , os);
	p->j_io_st  = RO_proc_open("iostats.json",     p, iostats_detailed_to_json , os);
	p->opens    = RO_proc_open("client_processes", p, nvmeiba_kapi.atom_users_to_string, os);
	p->throttle = RO_proc_open("io_throttle",      p, io_throttle_to_string    , os->dev);
	p->status   = RO_proc_open("status"     ,      p, cb.dev_status_to_txt     , os->dev);
	p->stalocks = RO_proc_open("recov_stats",      p, cb.dev_recovs_to_txt     , os->dev);
	p->profiling= RO_proc_open("profiling",        p, cb.profiling_to_string   , os->dev);
	p->profcsv  = RO_proc_open("profiling.csv",    p, cb.profiling_to_csv      , os->dev);
	p->j_status = RO_proc_open("status.json" ,     p, cb.dev_status_to_json    , os->dev);
	p->j_flow_c = RO_proc_open("flow_cntr.json",   p, cb.flows_cntr_to_json    , os->dev);
	p->ext_blob = RO_proc_open("blob.txt",         p, cb.ext_blob_to_txt       , os->dev);

	if (cb.cpu_masks.to_json) {
		p->cpu_masks.dir = proc_mkdir("cpu_masks", p->dir);
		if (!p->cpu_masks.dir)
			goto _out;
		p->cpu_masks.j_show = nvmeib_public_proc_create("cpu_masks.json", p->cpu_masks.dir, cb.cpu_masks.to_json, NULL, os->dev);
		p->cpu_masks.add = nvmeib_public_proc_create("cpu_mask_add", p->cpu_masks.dir, NULL, cb.cpu_masks.add, os->dev);
		p->cpu_masks.del = nvmeib_public_proc_create("cpu_mask_del", p->cpu_masks.dir, NULL, cb.cpu_masks.del, os->dev);
	}

_out:
	return (p->dir && p->io_st_sum && p->status && p->opens && p->throttle &&
			p->stalocks && p->profiling && p->j_status && p->j_io_st && p->j_flow_c && p->ext_blob &&
			(!cb.cpu_masks.to_json || (p->cpu_masks.dir && p->cpu_masks.j_show && p->cpu_masks.add && p->cpu_masks.del)));
}

static void __proc_destroy(struct nvmeibc_os_api *os)
{
#define RM_PROC_FILE(procfs_entry) ({		\
	if (procfs_entry) {						\
		nvmeib_public_proc_remove(procfs_entry);	\
		(procfs_entry) = NULL;				\
	}										\
})
	struct nvmeibc_procfs *p = &os->procfs;
	RM_PROC_FILE(p->cpu_masks.j_show);
	RM_PROC_FILE(p->cpu_masks.add);
	RM_PROC_FILE(p->cpu_masks.del);
	RM_PROC_FILE(p->throttle);
	RM_PROC_FILE(p->opens);
	RM_PROC_FILE(p->io_st_sum);
	RM_PROC_FILE(p->j_io_st);
	RM_PROC_FILE(p->status);
	RM_PROC_FILE(p->stalocks);
	RM_PROC_FILE(p->profiling);
	RM_PROC_FILE(p->profcsv);
	RM_PROC_FILE(p->j_status);
	RM_PROC_FILE(p->j_flow_c);
	RM_PROC_FILE(p->ext_blob);
	if (p->dir) {
		if (p->cpu_masks.dir) {
			remove_proc_entry("cpu_masks", p->dir);
			p->cpu_masks.dir = NULL;
		}
		remove_proc_entry(os->atom.dev_name, os->driver_context->proc_root);
		p->dir = NULL;
	}
}

static struct nvmeibc_os_api *__kzalloc_os_api(void)
{
	struct nvmeibc_os_api *os = kzalloc(sizeof(*os), GFP_ATOMIC);
	if (os) {
		os->atom.alloc_size = sizeof(*os);
		ASSERT_ATOM_STATUS(&os->atom, nvmeiba_status_illegal);
		os->is_init_error = true;		// Will be true until _init() succeeds.
	}
	return os;
}

static struct nvmeibc_os_api *__get_mem_for_os_api(const char* dev_dir, const char* dev_name)
{
	struct nvmeiba_atom_os_api *orphan = nvmeiba_kapi.os_api_orphan_adopt(dev_dir, dev_name);
	struct nvmeibc_os_api *os = NULL;
	BUILD_BUG_ON(&((struct nvmeibc_os_api *)0)->atom != NULL); 			// Incorrect inheritance
	BUILD_BUG_ON(sizeof(os->atom.dev_name) != NVMEIBC_BD_NAME_LEN); 	// Incorrect inheritance
	if (likely(!orphan)) {
		os = __kzalloc_os_api();	// No one to adopt, create from scratch
	} else {
		ASSERT_ATOM_STATUS(orphan, nvmeiba_status_orphan);
		BUG_ON(sizeof(*os) > orphan->alloc_size);				// Cannot reuse orphans memory. Should never happen! Daniel: I want to avoid copying orphan to current to avoid race conditions
		os = (void*)orphan;
		memset(&orphan[1], 0, (sizeof(*os)-sizeof(*orphan)));	// As if os kzalloced() and only 'atom' completely initialized
	}
	return os;
}

#define __atom_set_self_parent(atom)   (atom)->sub.parent = atom
static void nvmeibc_atom_constructor(struct nvmeiba_atom_os_api *atom, const char* dev_name, bool is_sub_atom)
{
	strlcpy(atom->dev_name, dev_name, sizeof(atom->dev_name));
	spin_lock_init(&atom->disk_lock);
	atomic_set(&atom->gendisk_status, 1);
	INIT_LIST_HEAD(&atom->users.pids);
	spin_lock_init(&atom->users.lock);
	spin_lock_init(&atom->pender.lock);
	bio_list_init(&atom->pender.bio_list);
	atom->sub.flags.is_sub_atom = is_sub_atom;
	INIT_LIST_HEAD(&atom->sub.part_list);
	spin_lock_init(&atom->sub.list_lock_unused);
	__atom_set_self_parent(atom);
	atom->status = nvmeiba_status_hidden;	// Atom is hidden until registration with OS
	atom->attach_jiff = jiffies;
	nvmeiba_kapi.os_api_constructor(atom);
}

static void __set_dev_and_uuid(struct nvmeibc_os_api *os, void* dev, const char* dev_uuid, const struct nvmeibc_os_apis_container *c)
{
	os->driver_context = c;						// Connect os_api to multi-client instance
	os->dev	= dev;
	UUID_COPY(os->dev_uuid, dev_uuid);
	if (!dev)
		os->is_proc_api_disabled = true;		// Sub volumes dont have /proc entries. They use carriers /proc
}

static int __get_mem_for_os_api_sub_vols(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeibc_os_api *carrier = container_of(atom->sub.parent, struct nvmeibc_os_api, atom);
	const struct nvmeibc_os_apis_container *c = carrier->driver_context;
	struct nvmeibc_os_api *os = __get_mem_for_os_api(c->drv_ver.dir_lsblk, atom->dev_name);
	BUG_ON(&os->atom != atom);
	__set_dev_and_uuid(os, NULL, atom->dev_name, c);
	_NT(t_baoc_10, "@DEV_NAME: adopted sub_atom=@ATOM", atom->dev_name, &os->atom);
	return 0;
}

struct nvmeibc_os_api *block_api_os_create(const struct nvmeibc_cinst_params_blk *p,
	bool is_hidden, const char* dev_name, const char* dev_uuid, void* dev,
	const struct nvmeibc_procfs_cb cb)
{
	const struct nvmeibc_os_apis_container *c = get_os_api_cints(p);
	struct nvmeibc_os_api *os = __get_mem_for_os_api(c->drv_ver.dir_lsblk, dev_name);
	int rv = 0;
	if (unlikely(!os)) {
		rv = -ENOMEM;
		goto _out;
	}
	os->is_io_api_disabled = is_hidden;
	__set_dev_and_uuid(os, dev, dev_uuid, c);

	/* Atom initialization */
	if (likely(os->atom.status != nvmeiba_status_orphan)) {	// If not adopting, call constructor
		nvmeibc_atom_constructor(&os->atom, dev_name, false);
		_NT(t_baoc_00, "@DEV_NAME: will @YES_NO_STATUS accept kernel IO", dev_name, ((os->is_io_api_disabled) ? "not" : "do"));
	} else {									// If adopting
		if (unlikely(os->is_io_api_disabled)) {
			/* Unlikely scenario, after upgrade we are adopting atom upon attach,
			   but hidden attach happened before regular attach. Fail hidden attach, or else we risk DI due to wron reservation version */
			_NT(t_baoc_01, "@DEV_NAME: Failing hidden attach. Volume during upgrade! Reabandoning", dev_name);
			os->atom.status = nvmeiba_status_live;		// We mistakenly adopted the atom, so abandon it again. Sorry bro...
			nvmeiba_kapi.os_api_orphan_abandon(&os->atom);	// Note: No need to call '__exec_for_carrier_and_sub_vols' because we havent adopted the sub volumes yet
			__set_atom_status_orphan(&os->atom);
			rv = -EACCES;
			os = NULL;
			goto _out;
		}
		_NT(t_baoc_02, "@DEV_NAME: adopted atom=@ATOM", dev_name, &os->atom);
		__exec_for_each_sub_vol(&os->atom, __get_mem_for_os_api_sub_vols); // Auto adopting all sub-vols of this carrier
	}

	/* Proc API */
	_NT(t_baoc_03, "@DEV_NAME: will @YES_NO_STATUS have a /proc entries", dev_name, ((os->is_proc_api_disabled) ? "not" : "do"));
	if (!os->is_proc_api_disabled) {
		if (!__proc_create(os, cb)) {
			rv = -EBUSY;
			goto _out;
		}
	}

_out:
	if (unlikely(rv)) {
		block_api_os_destroy_on_init_error_io_never_started(os, rv, dev_name, __FUNCTION__);
		os = NULL;
	}
	return os;
}

ssize_t block_api_os_dump_users(struct nvmeibc_os_api *os, char *buf, size_t len)
{
	return nvmeiba_kapi.atom_users_to_string(&os->atom, buf, len);
}

void block_api_os_clear_io_stats(struct nvmeibc_os_api *os, const int which)
{
	_NT(trace_api_os_block_api_os_clear_io_stats, "volume @DEV_NAME clearing IO stats: @INT", os->atom.dev_name, (char)which);
	if ((!os->is_io_api_disabled)&&(os->stats))
		nvmeib_io_stats_clear(os->stats, which);
}

/*************** Realtime reconfiguration of IO params ***********************/
static void __mark_carrier_should_update_rider_on_reconf(struct nvmeibc_block_device *dev, int n_segs, bool was_size_changed)
{
	union nvmeibc_reconf_msg_car2rider *msg = NULL;
	if (nvmeibc_block_is_d_carrier(dev)) {
		msg = &dev->c_d_api.reconf_msg;
	}
	if (msg) {											// Merge with previously unhandled msg -> Update message only with non empty fields
		if (n_segs)
			msg->n_segs = n_segs;
		if (was_size_changed)
			msg->was_size_changed = was_size_changed;
	}
	nvmeibc_io_resubmitter_wakeup(&dev->dp.resub);	/* Reconf notification passed via resub thread */
}

/* Configure the OS request queue for TRIM IO operations. Trim's should be
   short for mirrored vols to avoid taking large amount of locks */
void block_api_os_change_mirorring(struct nvmeibc_block_device *dev)
{
	struct nvmeibc_topology *t = nvmeibc_topology_get(&dev->topologies);
	struct nvmeibc_os_api *os = dev->os;
	const bool cannot_reconf_q = (os->is_io_config_api_disabled || os->is_io_api_disabled);
	int n_replicas = nvmeibc_topology_get_max_num_replicas(t);
	nvmeibc_topology_put(t);
	if (!cannot_reconf_q){
		const unsigned int m = to_kenrel_sects(max_trim_size_mirrored);
		const unsigned int n = to_kenrel_sects(max_trim_size_non_mirrored);
		const unsigned int r = ((n_replicas>1) ? m : n);
		__set_max_trim(os->atom.queue, r, dev->name);
	}
	__mark_carrier_should_update_rider_on_reconf(dev, n_replicas, 0);
}

/* Allways called when IO is enabled for block device */
void block_api_os_change_size(struct nvmeibc_block_device *dev, bool force_revalidation)
{
	struct nvmeibc_os_api *os = dev->os;
	struct nvmeiba_atom_os_api *atom = &os->atom;
	const sector_t bdev_size = (dev->size<<KERNEL_SECTOR_TO_SECTOR_SHIFT);
	bool resize_occured = false;
	unsigned long flags;
	if (!os->is_io_config_api_disabled) {
		spin_lock_irqsave(&atom->disk_lock, flags);
		if (!atom->disk)
			goto _out;		/* volume is dettaching */
		if (get_capacity(atom->disk) != bdev_size) {
			set_capacity(atom->disk,    bdev_size);
			resize_occured = true;
			os->disk_reval_task = NVMEIBC_OS_DISK_ONLY_REVAL;			// No need to reread partitions upon volume resize, only revalidate
		}
		if (force_revalidation)
			os->disk_reval_task = NVMEIBC_OS_DISK_REVAL_AND_READ_PART;	//
		if (os->disk_reval_task)
			__schedule_revalidation_must_hold_spinlock(dev);
	_out:
		spin_unlock_irqrestore(&atom->disk_lock, flags);
	}
	if (resize_occured) {
		_NT(t_06_api_os_resize, "@DEV_NAME: resized to @SZ[blks]", dev->name, dev->size);
		__mark_carrier_should_update_rider_on_reconf(dev, 0, 1);
	}
}

/********************************** Sub Volume *******************************/
#include "os_api/nvmeibc_block_api_os_sub_vols_common.inc.c"	// Todo: Remove

static void __copy_gendisk(struct gendisk *dst, const struct gendisk *src)
{
	dst->major = nvmeibc_use_block_external_major ? 0 : src->major;
	dst->flags = src->flags;
	dst->fops =  src->fops;
	dst->queue = src->queue;
}

// Find sub volume of carrier, assuming carrier has sub.list_lock_unused locked for addition/deletion (or serialized onon main_wq)
static struct nvmeibc_os_api * __find_sub_atom_carrier_locked(struct nvmeiba_atom_os_api *car, const char* dev_name)
{
	const u32 car_name_len = (u32)strlen(car->dev_name) + 1;	// Skip prefix of carrier
	struct nvmeiba_part *part;
	struct nvmeibc_os_api *rv = NULL;
	const int max_len = sizeof(car->dev_name);
	list_for_each_entry(part, &car->sub.part_list, part_list) {		// LKJ: take spinlock of sub-volume here
		struct nvmeiba_atom_os_api *atom = container_of(part, struct nvmeiba_atom_os_api, sub);
		const u32 skip_prefix = (atom->sub.flags.is_sub_unique_name ? 0 : car_name_len);
		if (!strncmp(&atom->dev_name[skip_prefix], dev_name, (max_len-skip_prefix-1))) {
			rv = (struct nvmeibc_os_api*)atom;
			goto _out;
		}
	}
_out:
	return rv;
}

static int nvmeibc_atom_part_add(struct nvmeiba_atom_os_api *atom, struct nvmeiba_atom_os_api *car, ulong start_lba, const char *dir_lsblk, const char* dev_name, union nvmeiba_part_flags f)
{
	int rv = 0;
	__verify_on_main_wq_atom(atom);							// Must be on main work queue to serialize with sub volume add/del, Alternatively must acquire sub.list_lock_unused
	atom->sub.offset = (start_lba << NVMEIBC_SECTOR_SHIFT);		// Bytes
	f.is_sub_atom = true;										// Regardless of what the caller passed
	atom->sub.flags.all = f.all;
	disk_id_allocator_alloc(get_dia(atom), atom->disk, atom->dev_name);
	__copy_gendisk(atom->disk, car->disk);
	atom->users.readonly = car->users.readonly;					// Inherit the same reservation flags from carrier
	if (!f.is_sub_share_reqq) {
		atom->disk->queue = atom->queue;
	}
	atom->disk->private_data = atom;

	if (1) {
		int n;
		if (f.is_sub_unique_name) {
			n = snprintf(&atom->dev_name[0], DISK_NAME_LEN, "%s",                             dev_name);
		} else {
			#define SUB_VOL_NAME_FMT	"%s_%s"				// <carrier>_<sub>
			n = snprintf(&atom->dev_name[0], DISK_NAME_LEN, SUB_VOL_NAME_FMT, car->dev_name, dev_name);
		}
		ALLERT_NAME_TRUNCATION((n > DISK_NAME_LEN), dev_name, atom->dev_name);
		if ((rv = __gendisk_set_name(atom, dir_lsblk)) < 0)
			goto _out;
	}

	// Sub volume creation succeeded, proceeed to add it to the carrier
	//car->sub.flags.is_sub_auto_resize |= f.is_sub_auto_resize;	// True if at least one of them is auto resizable. Todo add this line when upon removing nickname this flag is updated
	atom->sub.parent = car;
	list_add_tail(&atom->sub.part_list, &car->sub.part_list);
	nvmeiba_kapi.atom_part_add(car);
_out:
	return rv;
}

static void nvmeibc_atom_part_del(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeiba_atom_os_api *car = atom->sub.parent;
	__verify_on_main_wq_atom(atom);				// Must be on main work queue to serialize with sub volume add/del, Alternatively must acquire sub.list_lock_unused
	if (atom->sub.flags.is_sub_atom) {			// Protect against incorrect call on non sub volume
		if (car && (car != atom)) {
			list_del(&atom->sub.part_list);     // Delete form carrier so take lock on carrier
			__atom_set_self_parent(atom);
			nvmeiba_kapi.atom_part_del(car);
		} else {}								// Cleanup of failed constructor: Rare case when creation of sub volume failed before it could connect to carrier
	}
}

int block_api_os_sub_vol_attach(struct nvmeibc_os_api *carrier,			// Equivalent of carrier attach
						ulong start_lba, ulong size,
						const char* dev_name, const char* dev_uuid)
{
	int rv = -ENOMEM;
	struct nvmeibc_os_api *os = NULL;
	struct nvmeiba_atom_os_api *atom = NULL;
	struct gendisk *disk = NULL;
	const char* dir_lsblk;
	union nvmeiba_part_flags f = {.all = 0};
	if (carrier->atom.sub.flags.is_sub_atom) {
		_NT(t0_sub_vol_create, "@DEV_NAME: cannot be created on sub @DEV_NAME", dev_name, carrier->atom.dev_name);
		rv = -ENODEV;
		goto _out;
	}
	if (carrier->is_io_api_disabled) {
		_NT(t1_sub_vol_create, "@DEV_NAME: cannot be created on hidden @DEV_NAME", dev_name, carrier->atom.dev_name);
		rv = -ENODEV;
		goto _out;
	}
	if (unlikely(__find_sub_atom_carrier_locked(&carrier->atom, dev_name))) {
		rv = -EEXIST;
		goto _out;
	}
	if (1) {		// Capacity settings
		const ulong carrier_cap_sectors = (ulong)get_capacity(carrier->atom.disk);
		const ulong carrier_cap_blocks = (carrier_cap_sectors >> KERNEL_SECTOR_TO_SECTOR_SHIFT);
		if ((start_lba + size) > carrier_cap_blocks) {
			_NE(t2_sub_vol_create, DMESG_PREFIX("@DEV_NAME") ": cannot be created with lba > last lba of @DEV_NAME", dev_name, carrier->atom.dev_name);
			rv = -ENODEV;
			goto _out;
		}
		if ((start_lba == 0) && (size == 0)) {	// Just sub volume spanning on all the entire carrier (named attach)
			size = carrier_cap_blocks;
			f.is_sub_auto_resize = true;
			f.is_sub_unique_name = true;		// 2019.11.29 - Yanivs request for POC, aliases are not prefixed by volume name
		}
	}
	// ******** Default flags for sub volumes, ******
	f.is_owner_of_reqctx = false;				// Daniel: Make module params? Sub vols requse context of carrier
	if (f.is_owner_of_reqctx)
		f.is_sub_share_reqq = false;			// If sub volus use different context then they must use different request queues as well

	// ******** Equivalent to block_api_os_create() of regular volume ******
	if ((os = __kzalloc_os_api()) == NULL) {
		goto _out;
	}
	__set_dev_and_uuid(os, NULL /*No device*/, dev_uuid, carrier->driver_context);
	atom = &os->atom;
	nvmeibc_atom_constructor(atom, dev_name, true);

	// ******** Equivalent to block_api_os_init() of regular volume ******

	__sub_vol_inherit_carrier_fields(os, carrier);
	__alloc_disk_and_maybe_queue(atom, !f.is_sub_share_reqq);
	disk = atom->disk;
	if (!f.is_sub_share_reqq) {
		struct request_queue *q = atom->queue;
		void *q_data = reqq_data_copy_constructor(carrier->atom.queue->queuedata, f.is_owner_of_reqctx);
		if (!q_data) {
			rv = -ENOMEM | 0x2000;
			goto _out;
		}
		if (unlikely(!q)) {
			rv = -ENOMEM | 0x3000;
			goto _out;
		}
		reqq_data_connect_to_q(q_data, q, &carrier->atom.disk->fops);
		__request_queue_set_default_params(q, dev_name, __init_request_queue_params(carrier));
	}
	if (!os->atom.disk) {
		rv = -ENOMEM | 0x4000;
		goto _out;
	}

	dir_lsblk = carrier->driver_context->drv_ver.dir_lsblk;
	rv = nvmeibc_atom_part_add(atom, &carrier->atom, start_lba, dir_lsblk, dev_name, f);
	if (unlikely(rv < 0)) {
		_NE(t_03_nvmeibc_atom_prt_add, DMESG_PREFIX("@DEV_NAME") ": sb_vol @DEV_NAME error rv=@RV", carrier->atom.dev_name, dev_name, rv);
		goto _out;
	}

	set_capacity(disk, (size<<KERNEL_SECTOR_TO_SECTOR_SHIFT));
	atom->status = nvmeiba_status_live;
	block_api_os_start_accepting_kernel_io(os);
	_NI(t3_sub_vol_create, DMESG_PREFIX("@DEV_NAME") ": ver=(@DEV_MAJOR:@VOL_ID)/@GENDISK_MAJOR dev=@DEV, osapi=@PTR, registering with OS...", disk->disk_name, MAJOR(disk_devt(disk)), MINOR(disk_devt(disk)), disk->major, os->dev, os);
	rv = 0;
_out:
	if (unlikely(rv))
		block_api_os_destroy_on_init_error_io_never_started(os, rv, dev_name, __FUNCTION__);
	else
		os->is_init_error = false;
	return rv;
}

static int block_api_os_destroy_sub_vol(struct nvmeiba_atom_os_api *atom)
{
	struct nvmeibc_os_api *os = container_of(atom, struct nvmeibc_os_api, atom);
	const bool can_other_threads_open_atom = (atom->disk && is_gendisk_ready_for_io(atom));
	const bool should_take_self_ref = (can_other_threads_open_atom && (atom->status != nvmeiba_status_orphan));

	int rv;
	if (os->is_init_error) {
		ASSERT_ATOM_STATUS(atom, nvmeiba_status_hidden);
		nvmeibc_atom_part_del(&os->atom);						// Remove possibly half initialized connection of sub voluem to carrier
	} else if (atom->status == nvmeiba_status_detaching) {
		nvmeibc_atom_part_del(&os->atom);						// Remove connection of sub voluem to carrier
	} else {													// Detach for upgrade
		ASSERT_ATOM_STATUS(atom, nvmeiba_status_orphan);
	}
	if (should_take_self_ref) {
		if (block_api_os_get(os, "Detach_SubVol") < 0) {		// Carrier/Regular os_apis have the volume layer to take external ref. Sub volumes dont have that
			rv = -ENODEV;
			goto _out;
		}
	}
	block_api_os_destroy(os);
	if (should_take_self_ref) {
		block_api_os_put(os);
	}
	rv = 0;
_out:
	return rv;
}

int block_api_os_sub_vol_detach(struct nvmeibc_os_api *carrier, const char* dev_name)		// Equivalent of carrier detach wihtout --force
{
	struct nvmeibc_os_api *os = __find_sub_atom_carrier_locked(&carrier->atom, dev_name);
	const char *carrier_name = &carrier->atom.dev_name[0];
	int rv = 0;
	__verify_on_main_wq_osapi(carrier);				// Must be on main work queue to serialize with sub volume add/del, Alternatively must acquire sub.list_lock_unused
	if (!os) {
		rv = -ENODEV;
		goto _out;
	}
	if (block_api_os_is_mounted(os)) {
		rv = -EBUSY;
		goto _out;
	}
	if (!os->atom.sub.flags.is_sub_share_reqq) {
		block_api_os_stop_accepting_kernel_io(os, 'D');
		block_api_os_drain_io(os);
	}
	rv = block_api_os_destroy_sub_vol(&os->atom);
_out:
	if (rv) {
		_NE(t_00_sub_vol_del, DMESG_PREFIX("@DEV_NAME") ": cannot delete sub vol of @DEV_NAME, rv=@RV", dev_name, carrier_name, rv);
	}
	return rv;
}

static int __atom_mimic_carrier_properties(struct nvmeiba_atom_os_api *atom)		// Adoption completed
{
	struct nvmeiba_atom_os_api *car = atom->sub.parent;
	__verify_on_main_wq_atom(atom);				// Must be on main work queue to serialize with sub volume add/del, Alternatively must acquire sub.list_lock_unused
	if (atom->sub.flags.is_sub_auto_resize) {
		set_capacity(atom->disk, get_capacity(car->disk));
		// Daniel: Not sure if we need to do block_api_os_async_revalidate(car), Figure that out
		_NT(t_07_api_os_resize, "@DEV_NAME: mimicked resize of carrier", atom->dev_name);
	}
	return 0;
}

static int __revalidate_sub_vols_of_carrier(struct nvmeiba_atom_os_api *atom)
{
	__exec_for_each_sub_vol(atom, __atom_mimic_carrier_properties);
	return 0;
}

/*************************** Riders on Top of Carriers ***********************/
void block_api_os_carrier_ref_add(struct nvmeibc_os_api *car, const struct nvmeibc_os_api *rider)
{
	struct nvmeibc_os_api_self_ref *ref = &car->unsafe_self_ref;
	__verify_on_main_wq_osapi(car);
	_NT(t_q0_capios, "@DEV_NAME: mounting carrier @DEV_NAME", rider->atom.dev_name, car->atom.dev_name);
	nvmeiba_kapi.atom_part_add(&car->atom);	// Same refcount mechanism as sub-vols/aliases/partitions. As if rider creates an alias for carrier, thus holding a reference
	WARN(ref->bdev_during_detach != NULL, "nvmeibc flow error\n");		// Detach could not be started because carrier will be used
}

void block_api_os_carrier_ref_del(struct nvmeibc_os_api *car, const struct nvmeibc_os_api *rider)
{
	struct nvmeibc_os_api_self_ref *ref = &car->unsafe_self_ref;
	__verify_on_main_wq_osapi(car);
	_NT(t_q1_capios, "@DEV_NAME: umounting carrier @DEV_NAME", rider->atom.dev_name, car->atom.dev_name);
	WARN(ref->bdev_during_detach != NULL, "nvmeibc flow error\n");		// Detach could not be started because carrier is in use
	nvmeiba_kapi.atom_part_del(&car->atom);
}

