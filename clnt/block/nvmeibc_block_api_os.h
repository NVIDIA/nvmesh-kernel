/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_BLOCK_API_OS_H
#define NVMEIBC_BLOCK_API_OS_H
/* Inherits from nvmeiba_atom:
 * Interface of volume with the OS kernel, used by regular block devices
 */
#include "atom/nvmeiba_nvmesh_api.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_jdr_proc.h"

struct nvmeibc_cinst_params_blk;
struct nvmeibc_os_apis_container * nvmeibc_os_api_layer_init(const struct nvmeibc_cinst_params_blk *p);	// When module goes up. Returns positive major number or 0,negative on error
void nvmeibc_os_api_layer_destroy(const struct nvmeibc_cinst_params_blk *p);	// When module goes down
int  nvmeibc_os_api_layer_get_num_read_part(const struct nvmeibc_cinst_params_blk *p, bool *is_on);
void nvmeibc_os_api_layer_toggle_read_part( const struct nvmeibc_cinst_params_blk *p, bool  is_on);

enum nvmeibc_os_ap_revalidation_job_type {
	NVMEIBC_OS_DISK_REVALIDATION_EMPTY =  0,		// Nothing to do. Must be 0
	NVMEIBC_OS_DISK_ONLY_REVAL =          1,		// Only revalidate using bdev mutex
	NVMEIBC_OS_DISK_REVAL_AND_READ_PART = 3,		// Revalidate and read partition
} __attribute__ ((packed));
/******************************************************************************/
/* Represent all the API of block device with the kernel. Including
   1. Registration of IO method, Mount/UnMount, IOctl
   2. Configuration of IO queue parameters
   3. Outputs to Proc files */
struct nvmeibc_os_api {
	struct nvmeiba_atom_os_api atom;				// Inheritance from atom core. Must be first !!!!!
	const struct nvmeibc_os_apis_container *driver_context;	// The driver in which this OS API resides
	struct nvmeibc_os_api_self_ref {				// Mechanism to acquire/release self reference. Much like in kref. last user which does close() on os_api will free it
#if KS_HAS_BDEV_FILE_OPEN_BY_PATH
		struct file	*bdev_during_detach;	// Safe version of the above. Most of the time NULL. Stores a real pointer when ref is taken
#elif KS_HAS_BDEV_OPEN_BY_PATH
		struct bdev_handle	*bdev_during_detach;	// Safe version of the above. Most of the time NULL. Stores a real pointer when ref is taken
#else // KS_HAS_BDEV_OPEN_BY_PATH
		struct block_device	*bdev_during_detach;	// Safe version of the above. Most of the time NULL. Stores a real pointer when ref is taken
#endif // KS_HAS_BDEV_OPEN_BY_PATH
	} unsafe_self_ref;								// Daniel: consider simplifying this struct and its functionality, Warning - unsafe pointer to kernel underlying block device (without ++ it's refcount). Use with great care!

	struct nvmeibc_procfs {
		struct proc_dir_entry *dir;					// The volumes /proc/.../vol_name/ directory, where proc files will reside
		struct nvmeib_public_procfs_ent *io_st_sum;		// Summerized IO stats written as table
		struct nvmeib_public_procfs_ent *status;			// Realtme block device status is written to a file <dav_name>/status
		struct nvmeib_public_procfs_ent *opens; 			// client processes holding an open handle of the volume.
		struct nvmeib_public_procfs_ent *throttle;			// proc file providing io throttling related information
		struct nvmeib_public_procfs_ent *io_status;			// proc file providing status of running ios per cpu
		struct nvmeib_public_procfs_ent *stalocks;			// proc file providing the history of stale locks the client encountered
		struct nvmeib_public_procfs_ent *profiling;		// proc file providing the current profiling information for this volume
		//--------- CSV formats for reading profiler
		struct nvmeib_public_procfs_ent *profcsv;			// proc file providing the current profiling information for this volume in CSV format
		//--------- Json formats for mgmt/NVCK
		struct nvmeib_public_procfs_ent *j_status;			// Realtme block device status
		struct nvmeib_public_procfs_ent *j_io_st;			// Detailed   IO stats written as json format
		//--------- Extra blob info about volume
		struct nvmeib_public_procfs_ent *ext_blob;
		//--------- IO throttle metrics (count + latency, per-CPU, JSON, read+reset)
		struct nvmeib_jdr_procfs_ent *io_throttle_metrics;
		//--------- Volume's CPU masks
		struct {
			struct proc_dir_entry *dir;
			struct nvmeib_public_procfs_ent *j_show;
			struct nvmeib_public_procfs_ent *add;
			struct nvmeib_public_procfs_ent *del;
		} cpu_masks;
	} *procfs;

	char dev_uuid[NVMEIBC_BD_UUID_LEN];		// NVMesh unique identifier of volume(block device) using this api
	void* dev;								// Private pointer to your device. Not used in OS api.

	u8   slice_size;						// Cache this value for optimizing io sizes of file system
	enum nvmeibc_os_ap_revalidation_job_type disk_reval_task;		// Which revalidation task should be done
	bool is_io_api_disabled;				// By default this API is active. Disable to prevent kernel from issuing IO
	bool is_trim_disabled;					// By default this API is active. If IO API is active, kernel can issue TRIM's. Turn Trims off via this flag
	bool is_io_config_api_disabled;			// By default this API is active. If IO API is active, you can tune IO params depending on volume topology.
	bool is_proc_api_disabled;				// By default this API is active. Disable to prevent /proc entries for the volume
	bool is_init_error;						// Creation/Initialization/Adoption of OS_API. Calling Destroy upon error
	//bool 8th byte in u64 of flags.		// Todo make bit field
	u64  ro_header_sectors;
	u64 reserved[15];						// Reserve some bytes for future versions
};

#define get_nvmeibc_os_api_uptime(os) (jiffies - (os)->atom.attach_jiff)

/* Signature of method used to execute bio request.*/
typedef int (bio_exec_fn)(struct bio *bio, unsigned long jiffies_start_time);

/* Struct with callbacks for proc files of blockdevice. Each user of os api with
   is_proc_api_disabled == false, must implement at least those callbacks */
struct nvmeibc_procfs_cb {
	proc_fill_t *dev_status_to_txt;		// Print status of block device
	proc_fill_t *dev_status_to_json;		// Print status of block device
	proc_fill_t *dev_recovs_to_txt;		// Print status of recoveries
	proc_fill_t *profiling_to_string;		// Print profiling information
	proc_fill_t *profiling_to_csv;			// Print profiling information in CSV format
	proc_fill_t *ext_blob_to_txt;			// Print exteternal blob informat
	struct {
		proc_fill_t *to_json;			// Print volume's CPU masks
		proc_chng_t *add;				// Add the CPU mask to the volume
		proc_chng_t *del;				// Delete the CPU mask from the volume
	} cpu_masks;
};

/* First of 3 steps of OS API. Does allocations, /proc , IO stats, etc..
   On success returns 0.
   is_hidden - if TRUE user space apps will not see this device -> no IO
   proc_func - set of function which describes volume in /proc. If /proc
		api is enabled all functions must not be NULL.
   Does not enable IO yet! */
struct nvmeibc_os_api * block_api_os_create(const struct nvmeibc_cinst_params_blk *p,
		bool is_hidden, const char* dev_name, const char* dev_uuid, void* dev,
		 const struct nvmeibc_procfs_cb cb);

/* fn - your method which performs the IO request, might be async, but
 * must not be NULL. IO will not start
 * On error returns negative code. 0 on success.
 * size - size of the device (in blocks of 4KB).
 * slice_size - required to limit the max IO size by transport constraignt * slice_size
 * header_size - encryption header size
 */
int block_api_os_init(struct nvmeibc_os_api *os, bio_exec_fn *fn, ulong size,
		      bool is_read_only, const int slice_size, u64 ro_header_sectors);

/* OS can start issuing IO before this function terminates. If IO is still not
   enabled, the requests will accumulate in resubmition queue. Call after
   block_api_os_init() in either sync or async form */
int  block_api_os_start_accepting_kernel_io(struct nvmeibc_os_api *os);

/* All new IO requests will be immediately Rejected or Accumulated
    1. Rejected - Usefull for unsafe detach.
    2. Accumulated in a side list - Usefull for upgrade.
   Reason: 'D' for detach, 'U' for Upgrade. Todo, in future make enum
   No new requests will enter the make_request() function of block device/
   Requests which are being processed, will still reach block device, But their
   amount will monotonically decrease over time */
void block_api_os_stop_accepting_kernel_io(struct nvmeibc_os_api *os, u32 reason);
/* Must be called after previous method! Wait until all IO's from request_queue
   are returned. OS api and block device may destroy themselves after call
   to this function.
   Note: User space might continue sending IO's but they are auto rejected or
    	 accumulated - tasks that do not require any variables/structs of
    	 nvmeibc. They do however require the code segment with assembly
    	 instructions of the function which autofails IO's so rmmod might not
		 work if user space holds reference to block device.*/
void block_api_os_drain_io(                struct nvmeibc_os_api *os);

/* Aux function: In case hot upgrade fails, autofail up to 'n_autofails' orphan IO to be able to revert to warm upgrade.
   n_autofails == -1, means all ios */
int block_api_os_autofail_upgrade_io(struct nvmeibc_os_api *os, int n_autofails);

/* Does OS use this volume right now. If it does IO is possible */
static inline int block_api_os_is_mounted(const struct nvmeibc_os_api *os){
	return (atomic_read(&os->atom.users.n_opens) /*> 0*/);
}

/* Dump OS users that use this volume at the moment. */
ssize_t block_api_os_dump_users(struct nvmeibc_os_api *os, char *buf, size_t len);

/* For debug only, zero all IO statistics counters */
void block_api_os_clear_io_stats(struct nvmeibc_os_api *os, const int which);

/* Inverse of _create(). Call only after IO is not flowing into os api / bdev
   anymore (either No IO or IO is redirected to a different entity due to unsafe
   detach or upgrade). Cleanup everything.
   Destroy - cleans all fields of nvmeibc_os_api, but NOT of nvmeiba_os_api, so
   it can exist without nvmeibc.
   Must be called after block_api_os_drain_io()
   If os->is_io_api_disabled --> No one uses bdev so syncronously also calls
     destructor of nvmeiba atom, and frees 'os'.
   Else 'caller' (detach) must hold ref to 'os' to prevent it from kfree()
     during the call to this function. os will be freed when reference counter
     reaches zero
   Else 'caller' (upgrade) .... Todo... Elaborate here */
void block_api_os_destroy(struct nvmeibc_os_api *os);

/* Methods to ++/-- refcount to the os_api. It will not be freed unless ref
   count goes to zero. Warning Do not call with irq disabled! */
int  block_api_os_get(struct nvmeibc_os_api *os, const char *owner_name);
void block_api_os_put(struct nvmeibc_os_api *os);

static inline bool block_api_os_is_io_api_enabled(const struct nvmeibc_os_api *os)
{
	return !os->is_io_api_disabled;
}

/*********************** IO executing methods ********************************/
/* In your bio_exec_fn extract your device from the kernels bio */
const struct nvmeibc_os_api* block_api_os_get_os(const struct bio *bio);
struct nvmeibc_block_device* block_api_os_get_base_bdev(const struct nvmeibc_os_api* os, ulong *sub_offset, ulong *sub_len);
int block_api_os_verify_bio_geometry(const struct bio *bio);

#include "block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_part.h"

u64 block_api_os_get_max_supported_trim_blks(struct nvmeibc_os_api *os);

/*************** Realtime reconfiguration of IO params ***********************/
/* Update OS of size change (initalization or volume shrink/grow) and tune
   parameters for better IO performace when volume mirroring changes.
   force_revalidation - set to true to force revalidation and reread of
   partitions. Typically forced, when first IO is enabled */
void block_api_os_change_size(     struct nvmeibc_block_device *dev,
											bool force_revalidation);
void block_api_os_change_mirorring(struct nvmeibc_block_device *dev);
void block_api_os_async_revalidate(struct nvmeibc_block_device *dev);

/********************************** Sub Volume *******************************/
int block_api_os_sub_vol_attach(struct nvmeibc_os_api *carrier,
						ulong start_lba, ulong size,
						const char* dev_name, const char* dev_uuid);
int block_api_os_sub_vol_detach(struct nvmeibc_os_api *carrier, const char* dev_name);

// Creating sub volume spanning on the entire carrier (effectively alliasing carrier by different name)
#define block_api_os_sub_vol_name(carrier, dev_name, dev_uuid) \
			block_api_os_sub_vol_attach(carrier, 0UL, 0UL, dev_name, dev_uuid);

/*************************** Sub-block operations***** ***********************/
static inline unsigned get_logical_block_size_from_os(const struct nvmeibc_os_api* os) {
	return (os->atom.queue) ? os->atom.queue->limits.logical_block_size : NVMEIBC_SECTOR_SIZE;
}

static inline unsigned get_dev_logical_block_size_from_bio(const struct bio *bio) {
	return get_logical_block_size_from_os(block_api_os_get_os(bio));
}

// Returns the nvmeibc block of kernel_block in NVMEIBC_SECTOR_SIZE blocks
// e.g, in case the device's logical block size is 512 (2^9):
// passing any blocks between 8-15 to the function, returns block 1
// passing any blocks between 0-7 to the function, returns block 0
static inline ulong kernel_block_to_nvmeibc_block(const ulong kernel_block) {
	return (kernel_block >> KERNEL_SECTOR_TO_SECTOR_SHIFT);
}

// Returns nvmeibc_block in logical block units
static inline ulong nvmeibc_block_to_kernel_block(const ulong nvmeibc_block)
{
	return (nvmeibc_block << KERNEL_SECTOR_TO_SECTOR_SHIFT);
}

// Returns the NVMEIBC_SECTOR_SIZE aligned logical block containing the input 'kernel_block_offset'
static inline ulong kernel_block_offset_in_nvmeibc_sector_size_alignment(const ulong kernel_block_offset)
{
	return nvmeibc_block_to_kernel_block(kernel_block_to_nvmeibc_block(kernel_block_offset));
}

// Return false iff the entire range between the start offset and length are aligned to NVMEIBC_SECTOR_SIZE
static inline bool is_sub_block_op(const ulong op_start_block_offset, const ulong op_byte_len)
{
	return ((((op_start_block_offset << KERNEL_SECTOR_SHIFT) % NVMEIBC_SECTOR_SIZE) != 0) ||	//	either the start offset, in bytes, is not aligned to NVMEIBC_SECTOR_SIZE
			((op_byte_len % NVMEIBC_SECTOR_SIZE) != 0));										//	or the length of IO in bytes is not aligned to NVMEIBC_SECTOR_SIZE
}

static inline ulong bytes_to_kernel_blocks(const ulong bytes) {
	return bytes >> KERNEL_SECTOR_SHIFT;
}

struct nvmeibc_block_device* get_bdev_or_parent_bdev_of_bio(const struct bio* bio);

static inline bool is_bio_on_device_attached_as_512B(const struct bio *bio)
{
	return ((1 << KERNEL_SECTOR_SHIFT) == get_dev_logical_block_size_from_bio(bio));
}

// Return true iff the sub-block operation is valid
static inline bool is_valid_512B_op(const enum nvmeib_block_io_op op, const ulong op_start_block_offset, const ulong op_byte_len)
{
	return (nvmeib_block_io_op_is_read(op) ||	// Either a read operation
		   ((nvmeib_block_io_op_is_write(op)) &&	// Or a 4k aligned write operation (TODO - remove this valid check after allowing sub block writes)
			!is_sub_block_op(op_start_block_offset, op_byte_len)));
}

static inline uint get_numer_of_required_4k_blocks_from_512B_IO(const ulong offset_in_kernel_sectors, const long total_size_b, u64 *start_4k_block, u64 *last_4k_block) {
	*start_4k_block = kernel_block_to_nvmeibc_block(offset_in_kernel_sectors);
	*last_4k_block 	= kernel_block_to_nvmeibc_block(offset_in_kernel_sectors + bytes_to_kernel_blocks(total_size_b - 1));
	return *last_4k_block - *start_4k_block + 1;
}

#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	#define CALL_SUBMIT_BIO_FN(q, disk, bio)	(q)->make_request_fn(q, bio)
	#define CALL_BIO_Q_CONTEXT(atom)			&(atom)->queue
#else
	#define CALL_SUBMIT_BIO_FN(q, disk, bio)	(disk)->fops->submit_bio(bio)
	#define CALL_BIO_Q_CONTEXT(atom)			&(atom)->disk->fops
#endif

#endif  // H beginning
