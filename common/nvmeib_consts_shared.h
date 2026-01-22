#ifndef NVMEIB_CONSTS_H
#define NVMEIB_CONSTS_H

/******************************************************
 * Shared NVMesh Constants
 *****************************************************/

#define DEFAULT_MANAGEMENT_REPORT_FREQUENCY 5

#define NVMEIBC_IO_PERM_ARM_DEFAULT_STABILIZATION_PERIOD_SEC 5
// the overall state of the nvmeibc module
enum nvmeibc_mod_state {
	NVMEIBC_MOD_STATE_INITIALIZING = 0,	// Module is initializing, possibly first instance is created
	NVMEIBC_MOD_STATE_READY,			// Module completed initialization & is operational. Has 1 instance
	NVMEIBC_MOD_STATE_PREP_RM,			// Module is preparing for removal, cannot attach new volumes, cannot create new instances. Still need to detach existing volumes and instances
	NVMEIBC_MOD_STATE_RM_RDY,			// Module is ready to be removed, no volumes attached. Idle (zombie) instances remain
	NVMEIBC_MOD_STATE_EXITING,			// Module is now exiting, /proc files being removed, memory is getting kfree()
};

/* Constants moved from nvmeib.h */
enum {
	MAX_HCA_PORTS = 4,

	NVMEIB_IOCH_KA_WRITE_LEN = sizeof(u64),

	NVMEIB_UPDATE_NW_PATHS = 1,
};

/* Constants moved from nvmeibs_types.h */


/* If inline metadata, limit transfer size to 4KB */
#define DEBUG_MD_EXTD 0
#if DEBUG_MD_EXTD
#define MAX_IO_CHANNEL_MSGS 64
#else
#define MAX_IO_CHANNEL_MSGS 64 /* --> 128KB and MD */
#endif

#ifndef LOW_MEM
/* the maximum number of messages a remote client may put on
 a *controller io qp.  this is usually between 1 for write and 2 for read
 	   assuming the client S/G list is collapsed.  however we set a much higher
 	   	   mark because we wish to reduce the cleanup messages
 	   	   	*/
#if NVMEIBC_SECTOR_SHIFT >= PAGE_SHIFT
#define NVMEIBS_MAX_IO_CHANNEL_MSGS				MAX_IO_CHANNEL_MSGS
#else
#define NVMEIBS_MAX_IO_CHANNEL_MSGS				(MAX_IO_CHANNEL_MSGS << (PAGE_SHIFT - NVMEIBC_SECTOR_SHIFT))
#endif
#else /* LOW_MEM */
#define NVMEIBS_MAX_IO_CHANNEL_MSGS				(MAX_IO_CHANNEL_MSGS << (PAGE_SHIFT - NVMEIBC_SECTOR_SHIFT))
#endif

#define NVMEIB_LOCK_DATA_BUFFERS 2
#define NVMEIB_LOCK_DATA_SIZE	((NVMEIB_LOCK_DATA_BUFFERS) * sizeof(u64))

enum {
	/* the max number of completion that can extracted from a cq poll */
	NVMEIBS_POLL_SIZE =
		(NVMEIBS_MAX_IO_CHANNEL_MSGS > 8 ? NVMEIBS_MAX_IO_CHANNEL_MSGS : 8) * 2,

	/* the size of a message a remote client may put on a controller io qp */
	NVMEIBS_DEFAULT_IO_MSG_SIZE = 16,
	/* the max rdma message that can be sent over a port */
	NVMEIBS_DEFAULT_MAX_RDMA_SIZE = (1 << 20),

	/* disk name max size: 32+1 for '\0' */
	NVMEIBS_DISK_MAX_NVMEXPRESS_ID_SIZE_LOGIN_MSG = 32, /* Send without '\0' */

	/* the size of the admin channel */
	NVMEIBS_SERVER_DEFAULT_SQ_SIZE = 32,

	NVMEIBS_SERVER_DEFAULT_MAX_SGES = 4, // SIW QPs are limited with 6 SGEs

	/* number of pages to allocate for configuration messages */
#ifndef LOW_MEM
	NVMEIBS_CONFIG_MSG_RDMA_PAGES = 16,
#else
	NVMEIBS_CONFIG_MSG_RDMA_PAGES = 8,
#endif

	/* max bounce buffer pages */
	NVMEIBS_MAX_BOUNCE_BUFFER_PAGES = 128,

	/* the maximum number of resources a client may get from a disk */

	NVMEIBS_MAX_DISK_RESOURCES_PER_CLIENT = NVMEIB_MAX_DISK_RESOURCES_PER_CLIENT,

	NVMEIBS_MAX_MASK_CMP_XCHG_ATTEMPTS = 10,
	NVMEIBS_UNMAP_REQUEST_WARN_SECONDS = 2,
};

enum {
	/*                                 e x c e l e r o       */
	NVMEIB_SERVICE_ID =      6988676976697999ULL,			/* 18446744073709551615 */
	NVMEIB_PORT_ID = 7914,
	NVMEIB_IB_PORT_PRIORITY 	= 0,
	NVMEIB_ROCE_PORT_PRIORITY	= 10,
	NVMEIB_TCP_PORT_PRIORITY 	= 20,
};

#endif //NVMEIB_CONSTS_H
