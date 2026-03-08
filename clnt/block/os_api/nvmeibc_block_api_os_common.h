#ifndef NVMEIBC_BLOCK_API_OS_COMMON_H
#define NVMEIBC_BLOCK_API_OS_COMMON_H
/* Internal API for all interfaces of block device to the kernel (io, ioctls,
   file operations, read partitions, revalidations, drive registration,...)
 */

// Can share struct below with nvmeiba, allow reconenction to existing driver version. Better not do that to keep nvmeiba as simple as possible
struct nvmeibc_driver_version {
	u32 nvmeibc_major;						// Major version of the driver
	bool registered_blkdev;					// For debug: Is it registered with Kernel. == (nvmeibc_major>0)
	const char *name_proc_dev;				// Directory under which volumes will be seen in cat /proc/devices | grep -e nvmeibc
	const char *dir_lsblk;					// Directory under which volumes will be seen in /dev/, /sys/block/, also via lsblk | grep nvmesh
};

/************************ Minor ID allocator for volume ***********************/
// Note: actual major:minor version is written here: ls -l /dev/nvmesh, also cat /sys/block/nvmesh/<vol name>/dev and cat /sys/block/nvmesh/<vol name>/uevent
//       MAJ - Major version for client instance nvmeibc00 can be seen here: cat /proc/devices | grep nvmeibc00
//       Bdi access via: ls /sys/class/bdi/MAJ:MINOR or /sys/devices/virtual/bdi/MAJ:MINOR
struct disk_id_allocator_t {		// Allocator of minor disk version (unique for each attached volume within client instance)
	unsigned long bmp[16];			// Support up to 1K gendisks, 1 bit per gendisk
};
void disk_id_allocator_init( struct disk_id_allocator_t* al);
void disk_id_allocator_alloc(struct disk_id_allocator_t* al, struct gendisk *disk, const char* vol_name); // Allocate new unique minor for gendisk
void disk_id_allocator_free( struct disk_id_allocator_t* al, struct gendisk *disk);				// Deallocate minor
void disk_id_allocator_mark( struct disk_id_allocator_t* al, struct gendisk *disk);	// Mark that minor is allocated after hot upgrade

struct nvmeibc_os_apis_container {
	struct block_device_operations bdev_fops_io;// nvmeibc our block device methods with OS with IO enabled (submit_bio)
	#if NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING && !KS_REQUEST_QUEUE_HAS_REQUEST_FN
		struct block_device_operations bdev_fops_de;// nvmeibc our block device methods with OS while detaching (reject bio), when request queue exists, q->function is changed so no need for different fops
	#endif
	struct nvmeibc_driver_version  drv_ver;		// nvmeibc driver
	atomic_t num_read_part_in_flight;			// Debug counter, number of running read partitions
	struct proc_dir_entry *proc_root;		// Root directory in /proc where os api's will create directory for each volume. Example: /proc/nvmeibc/volumes
	const struct nvmeibc_cinst_params_blk *cips;// Ptr to params of current client instance
	struct disk_id_allocator_t dia;
};

/******************************************************************************/
#endif  // H beginning

