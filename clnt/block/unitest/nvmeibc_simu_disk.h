#ifndef NVMEIBC_SIMU_DISK_H
#define NVMEIBC_SIMU_DISK_H
/*
 * This is a simulator designed to test block device IO requests and data services in a local (stand alone compilation).
 * It simulates all remote components, allowing the source to run on a single PC (possiblly even step by step in debugger)
 *  	1. Given API to linux kernel (kernel simulator), simulates all the software layers below block device (layers of disk.c, disk_locks.c etc)
 *  	2. Allows nvmeibc_block.c to be run in standalone mode.
 *  	3. Simulates also all the layers below volume.c and main.c, thus enabling the entire driver to run in standalone
 * System architecture:
 *  Each storage server is represented by
 *  	1. A single RAM/physical disk simulator (as if server has 1 disk only). Disks may vary in their sizes. Disk is simulated by RAM memory
 *  	2. Toma Simulator.
 *  Each Volume
 *  	1. Uses segments in one or more physical disks. A single disk can contribute a few segments to volume
 *  	2. Each disk can contribute segments to a few volumes. Example: Volumes v1,v2 use disks d1,d2. Where v1 consumes 1/3 of d1 and 1/3 of d2, v2 consumes 2/3 of d1 and 1/2 of d2.
 *  	3. The organization of segments & disks is called 'volume configuration'
 *  Block device is client's point of view on volume. A single volume v0 is accessed by Client C1 through block device B1, and by client C2 through B2. B1 and B2 are almost identical except for client specific info (like lock id)
 *  	1. Each block device is a production code witch uses underlying simulated nvmeibc_disk. This simulator passes block device requests to the server simulator
 *  	2. nvmeibc_disk emulates every need of block device (communication with Toma, locks, hardware IO, etc)
 *  Each client consists of
 *  	1. A single OS simulator (representing local clients PC).
 *  	2. A few block devices (few volumes).
 *  	3. Simulated application running on top of OS simulator, which makes IO requests. Example of application is unitest (write array to disk, read it and see that the result is identical to the original)
 *  Management Service Simulator
 *  	1. Simulates sys-admin's definition of volumes (how many, on which disk, which raid level etc)
 *  	2. Sends this information to all block devices of all the clients and to the servers (tomas)
 *  NVMeshSystem Simulator:
 *  	1. Defines multiple storage servers
 *  	2. Defines Management service which generates volumes
 *  	3. Defines multiple clients (each with sub group of the existing volumes, represented as block devices)
 *  Unitesting:
 *  	1. Should use the NVMeshSystem Simulator.
 *  	2. Defines apps running on clients, can disconnect servers, kill tomas, etc...
 */

#include "nvmeib_common_all.h"

#include "block/datapath_utils_generic/nvmeibc_block_dp_defs.h"
/******************** Simulator of Disk locks functionality ******************/
#define  LOW_MEM								// Reduce the size of lock channels by 90%, by removing the large array, unused by the simulator.
#include "nvmeibc_locks_channel.h"
#undef  LOW_MEM
#include "nvmeibc_disk_locks.h"					// Emulation of this file functions which are called through the pausable api

//static inline struct nvmeibc_disk_segments_locks *nvmeibc_disk_get_segs_locks(struct nvmeibc_disk *disk){(void)disk; return NULL;}
/******************************* Disk Simulator ******************************/
#undef TOMA_CONN_HASH_TABLE_SIZE
#define TOMA_CONN_HASH_TABLE_SIZE (1) 		// Reduce the size of c_disk by 80-90%, by removing the large array, unused by the simulator.
#include "nvmeibc_disk.h"						// Emulation of this .c file.
#include "nvmeibc_disk_hooks.h"
#include "nvmeibs_disk_locks.h"

/* struct nvmeibc_disk - is built as a simulator of disk towards block device (identical fields to the real disk structure)
 * In struct nvmeibc_disk we override an unused the field 'local_server' as a
 * pointer to simulator of server side computer with the physical disk. (Avoiding the need to emulate IB network connection to the server)
 * The actual type of this field is 'struct serverSimulator*' and not 'struct nvmeib_local_server *' as written in the above .h file
 * Todo: Add unitest variables by overriding also the unused admin channel??
 * - We also override unised field to mark whether admin channel is up (can receive pause/cont) or down
 */
#define serverOf(pDisk)	((struct serverSimulator*)(((struct nvmeibc_disk*)(pDisk))->local_server))			// Macro to get access to the server side through the clients disk. Read the explanation above
#define mark_disk_admin_channel_is_down( pDisk)	atomic_inc(&pDisk->num_contended);
#define mark_disk_admin_channel_is_up(   pDisk)	atomic_set(&pDisk->num_contended,0);
#define mark_disk_wait_for_admin_channel(pDisk)	while (atomic_read(&curDisk->num_contended)>0) {usleep(1);}

// Simulating nvmeibc_disk.h API used by block.c
void *nvmeibc_disk_locks_seg_locks_mem_info(	struct nvmeibc_disk *disk, int seg_id);
void  nvmeibc_disk_call_discover(				struct nvmeibc_disk *disk);
int   nvmeibc_disk_execute_io(					struct nvmeibc_disk *disk, struct nvmeibc_disk_io_command *block_cmd);
int nvmeibc_disk_execute_gen(struct nvmeibc_disk *disk, struct nvmeibc_disk_gen_cmd *gen_cmd);

// Simulating nvmeibc_disk.h API used by volume.c
/*int  nvmeibc_disk_create(		struct nvmeibc_disk_id *disk_id, struct list_head *arnics, int num_ranges); // init 'nvmeibc_disk', activated by volume.
void nvmeibc_disk_remove(		struct nvmeibc_disk_id *disk_id);										// de-init
void nvmeibc_disk_add_volume(	struct nvmeibc_disk *disk, struct nvmeibc_disk_id *disk_id);			// call this func when a new volume is attached which uses this disk
int  nvmeibc_disk_start_release(struct nvmeibc_disk *disk, void (*done)(void), bool send_disconnect);	// Stop using disc. When it disconnected from network or we don't use it anymore
*/

// Additional API used by unitest environment
void rediscovery(				struct nvmeibc_disk *disk);	// Continue using disk which was paused by nvmeibc_disk_start_release()

/****************** Simulator of client-toma communication *******************/
#include "../toma/clnt/nvmeibt_client_protocol.h"

// Simulating nvmeibc_disk.h clients API with Toma (Simulates the API used by the higher level). According to the protocol: https://docs.google.com/document/d/1juZValp1d_BnD_m2x97OnxcIsz4xP1XzMRpVquoMMMg/edit
int  nvmeibc_disk_subscribe_toma_service(	struct nvmeibc_disk *disk, u64 handle, struct nvmeibc_disk_subscription_params  *params);
int  nvmeibc_disk_unsubscribe_toma_service(	struct nvmeibc_disk *disk, u64 handle);
int  nvmeibc_disk_toma_send(             	struct nvmeibc_disk *disk, u64 handle, struct nvmeibc_disk_toma_send_params *params);

#include "./toma/nvmeibt_toma_simu.h"
enum e_special_lock_ids{ 											// Special lock ID's used for simulating unitest environment other client
	SIMULATOR_OTHER_CLIENT_LOCK_ID  = 42,	/*0x2A*/				// Simulate external client's lock_id (not one of the tested clients),
	SIMULATOR_USE_PREV_LOCK_ID  	= 47,	/*0x2F*/				// Special marker for Toma Simulator during unregister
	SIMULATOR_OLD_FORGOTTEN_LOCK_ID = 53,	/*0x35*/				// Special lock ID, used to simulate unknown lock ID to Tomas (long forgetten unregistered client)
	SIMULATOR_CLNT_ALLIEN_LOCK_NO_J = 54,	/*0x36*/				// Special lock ID, Toma knows this ficticious client but it did not leave any trace in serjio.
	SIMULATOR_LAST_RESERVED_LOCK_ID = 0x1000,						// All owner lock id's of clients are higher than this number. Numbers below are reserved for the simulator. Must be pow2
	SIMULATOR_USE_CURRENT_LOCK_ID	= 5,							// Special code for Toma sending messages to use the current lock id with which the client is registered
};

// Ficticious simulated client (used by unitest environment to inject stale transactions and locks into the system and let real client solve them
const uuid_be *SIMULATOR_OTHER_CLIENT__get_uuid(void);				// Other simulated client holding SIMULATOR_OTHER_CLIENT_LOCK_ID
const char *SIMULATOR_OTHER_CLIENT__get_urn_uuid(void);
const uuid_be *SIMULATOR_ALIEN_CLIENT__get_uuid(void);				// Ficticious client from the past that the system has no trace of: relevant for SIMULATOR_OLD_FORGOTTEN_LOCK_ID, SIMULATOR_CLNT_ALLIEN_LOCK_NO_J

/****************** Simulator of server side physical disk *******************/
#define RAMDISK_DATA_LOCK_SIZE (16)		// size of ramdisk used for volume segments, in locksets

//ramDiskSimulator memory layout/geometry
// ...[prefix][data][journal][db][postfix]
// where:
//    prefix & postfix are memory protection areas.
//    ... is allocated, but unused memory. data, journal & db partions
//        should be aligned by NVMEIBC_SECTOR_SHIFT(assumed==12 (4KB))
//        so, we pay this for alignment
//    data - data partion, data_seg_size describes the size in bytes
//    journali & db - serjio splits this partition to ranges and allocates
//        a range per client; the db area (privated to serjio) keeps allocation records.

enum ramDiskState {								// 0 - OK, bit field of error codes
	ramDisk_running =   0,						// Disk is in normal state.
	ramDisk_down =      1,						// Ram disk disconnected, will not answer to client
	ramDisk_broken =    2,						// NVMe Drive fails all IO types with NVME_SC_DNR
	ramDisk_no_rdma =   4,						// Disk has no lock channel in ram. Cannot RDMA it's ram (no locks, no dirty bits, no TxID's etc)
	ramDisk_fail_data = 8,						// For EC: autofails writes to 'data area', allow writes only to journal area: Used to test stale lock sync
};

enum lba_unit{
	LBA_UNIT_BYTE   = 0,
	LBA_UNIT_SECTOR = 1,
	LBA_UNIT_4KB    = 2,
	LBA_UNIT_LOCK   = 3
};

struct ramDiskCommitedAddress{
	union{
		struct {
			u64 byte;
			u64 sector;
			u64 block; //4KB
			u64 lock;
		};
 		u64 addresses[4];
	};
};

struct ramDiskSimulator {
	atomic_t io_cnt, sub_blk_io_cnt;					// Todo, make those non atomic, for faster execution
	enum ramDiskState state;
	int 		uniqueID;								// Unique ID. For each range on this disc: disk_range.disk_id == uniqueID
	bool		f_allowIOwithoutLocks;					// Ram disk will not alert when client issues write/trim without holding proper lock. Usefull for debugging client crashes, without ram disk interfering with its own bugs
	bool		use_ec_stale_locks;						// Todo: Remove this field. Move it to param to ramDiskSimulator_lockStale() functions
	u32			sector_shift;							// Disk is formatted to size of physical sector. Typically != NVMEIBC_SECTOR_SIZE
	u32 		md_size;								// Amount of metadata bytes for each NVMEIBC_SECTOR_SIZE. Disk is formatted with metadata of varying size.
	u32 		max_dma_size;                           // Simulate dma property of disk. In units of bytes. Must be a multiple of block size
	u64 		committed_disk_size;					// size of whole disk (after user data we have the journal). In bytes. Num of metadata blocks is Disk_size/4K
	struct ramDiskCommitedAddress committed_addr, committed_addr_end;
	//struct {											// Server side RAM allocated for datapath to this nvme disk
		spinlock_t  cmpxchg_lock;							// NIC: atomic cmpxchng on RAM through the bus. Theoretically we could take locks using atomic cmpexchg.
		u32 /*nvmeib_lock_id*/    locks[RAMDISK_DATA_LOCK_SIZE];	// Not atomic_t since we dont comare_exchange atomic operations on it
		union nvmeibc_dbits_entry dbits[RAMDISK_DATA_LOCK_SIZE];	// Dirty bits (implemented as dirty bytes like in real system
		u32 		TxIDs[RAMDISK_DATA_LOCK_SIZE];		// Used for erasure coding: Latest Transaction ID of slices in the lock
	//}
	struct nvmeibs_clnt_state{							// Server side state which is related to how a client operates this nvme disk
		u32        id_gen;								// Unique generator of client id's. Grows on each discovery of this disk by client
		union nvmeib_blkset_problem_report *bi_inj;		// When client requests blockset info map for recovery, this ptr can be used to inject incorrect data
	} c;
	//struct {											// Partitions on physical NVME disk, to be used in IO
		u8* 		_mem;								// Entire NVME disk with extra bytes before/after the drive to allow setting signatures and traps to write outside of a disk
		u8* 		mem;								// Partiotion 1: For data. Each disk has 1[MB] of memory. 8 stripes/locks each of 32 blocks. 8*32*4K
		spinlock_t  mem_lock;							// Spin lock protecting the memcpy() to and from the memory, emulating as if there is only N queues for IO
		short		error_code;							// The NVME error code returned in disk is in broken state
		u64 		data_seg_size;						// Size of partition 1 (data) in bytes
		struct t_disk_simu_serjio_partitions {
			u8* jranges_start;							// Partition 2: for journal entries (2GB in real system). Only pointer into correct offset in mem, 4K * NVMEIB_EC_TOTAL_JOURNAL_BLKS (The actual place on disk where journals are written (2GB in real system at the end of each disk))
			u8* db_start;								// Partition 3: for serjio DB: In real serjio this holds (mapping of UUID->JRI + statistics)
		} serjio;
		void* _blk_md;									// Per-4K meta-data for all the partitions above. Private! Do not access directly (deliberatly void*)
	//}
	struct {											// Emulates respective data structures of server side disk, various server side emulating components
		struct nvmeibs_disk_info di;					// Server disk info, used for server side gen_cmds handling and serjio
		struct nvmeibs_disk_private_data pd;			// Disk private data
	} server_disk;
};


static inline u64 ramDiskSimulator_translate_to_byte_addr(u64 addr, enum lba_unit unit) {
	switch(unit){
		case LBA_UNIT_BYTE:{
			return addr;
		}
		case LBA_UNIT_SECTOR:{
			return NVMEIBC_SECTOR2BYTE(addr);
		}
		case LBA_UNIT_4KB:{
			return addr << 12;
		}
		case LBA_UNIT_LOCK:{
			return addr << BYTES_IN_LOCKSET_SHIFT;
		}
	};
	BUG();
	return U64_MAX;
}

static inline u64 ramDiskSimulator_translate_to_unit_addr(u64 byte_addr, enum lba_unit target_unit) {
	switch(target_unit){
		case LBA_UNIT_BYTE:{
			return byte_addr;
		}
		case LBA_UNIT_SECTOR:{
			BUG_ON(NVMEIBC_SECTOR2BYTE(NVMEIBC_BYTE2SECTOR(byte_addr)) != byte_addr);
			return NVMEIBC_BYTE2SECTOR(byte_addr);
		}
		case LBA_UNIT_4KB:{
			BUG_ON(byte_addr - (byte_addr / 4096) * 4096);
			return byte_addr >> 12;
		}
		case LBA_UNIT_LOCK:{
			//BUG_ON(byte_addr - (byte_addr / BYTES_IN_LOCKSET) * BYTES_IN_LOCKSET);
			//somoetimes we want to get lock address for a slice
			return byte_addr >> BYTES_IN_LOCKSET_SHIFT;
		}
	};
	BUG();
	return U64_MAX;
}

//translate absolute disk address to a committed relative one
static inline u64 ramDiskSimulator_get_committed_addr_as(struct ramDiskSimulator const *ramDisk, u64 addr, enum lba_unit in_unit, enum lba_unit out_unit) {
	u64 const committed_addr_bgn = ramDisk->committed_addr.addresses[in_unit];
	u64 const committed_addr_end = ramDisk->committed_addr_end.addresses[in_unit];
	u64 const committed = addr - committed_addr_bgn;
	BUG_ON(addr < committed_addr_bgn);
	BUG_ON(committed_addr_end <= addr);
	if (in_unit == out_unit){
		return committed;
	} else {
		u64 const byte_addr_committed = ramDiskSimulator_translate_to_byte_addr(committed, in_unit);
		u64 const unit_addr_committed = ramDiskSimulator_translate_to_unit_addr(byte_addr_committed, out_unit);
		return unit_addr_committed;
	}
}

static inline u64 ramDiskSimulator_get_committed_addr(struct ramDiskSimulator const *ramDisk, u64 addr, enum lba_unit unit) {
	return ramDiskSimulator_get_committed_addr_as(ramDisk, addr, unit, unit);
}

#define COMMITTED_ADDR(ramDisk, addr, unit) ramDiskSimulator_get_committed_addr(ramDisk, addr, LBA_UNIT_##unit)
#define COMMITTED_ADDR_AS(ramDisk, addr, in_unit, out_unit) ramDiskSimulator_get_committed_addr_as(ramDisk, addr, LBA_UNIT_##in_unit, LBA_UNIT_##out_unit)

#define as_ramDisk(_di) container_of(_di, struct ramDiskSimulator, server_disk.di)
#define __ptr_to_ith_md(D, dlba) (&(((u8*)D->_blk_md)[D->md_size*(dlba)]))
#define ramDiskSimulator_n_metadatas(D)  ((D)->committed_disk_size>>NVMEIBC_SECTOR_SHIFT)
#define ramDiskSimulator_TRIMVAL	(0)					// When memory address is trimmed it is overwritten (memset) with this value (have all bits set to 0)
#define ramDiskSimulator_MD_TRIMVAL	((u8)0xff)				// When memory address is trimmed metadata is overwritten (memset) with this value (have all bits set to -1)
int  	ramDiskSimulator_init(                 struct ramDiskSimulator*, int uniqueID);
int  	ramDiskSimulator_init_server_side(     struct ramDiskSimulator*);
void 	ramDiskSimulator_destroy(              struct ramDiskSimulator*);
void 	ramDiskSimulator_disconnect(           struct ramDiskSimulator*);	// Like physical unplug of the network cable
void 	ramDiskSimulator_reconnect(	           struct ramDiskSimulator*);
void	ramDiskSimulator_break(                struct ramDiskSimulator*, short err_code);  // Cause a ramDisk to return a specific error or generic NVME_SC_DNR if error not defined
void	ramDiskSimulator_fix(                  struct ramDiskSimulator*);    // Fix the above condition
void    ramDiskSimulator_fail_data(            struct ramDiskSimulator*); // Fail EC data commands (journal will succeed)
void    ramDiskSimulator_fix_data(             struct ramDiskSimulator*); // Fix  EC data commands (journal will succeed)
void 	ramDiskSimulator_setrdma(              struct ramDiskSimulator*, bool enable);// Enable/Disable rdma of locks
void 	ramDiskSimulator_verify_no_locks(      struct ramDiskSimulator*);		 // Verify that no one holds any lock on the disk (no client locks, no stales). Typically run between tests to verify initial condition
int 	ramDiskSimulator_printTakenLocks(      struct ramDiskSimulator*);		 // Print information about all the taken locks on this disk. Returnes the amount of taken locks
void 	ramDiskSimulator_get_serjio_partions_sectors_ranges(struct ramDiskSimulator *self, u64* jrnl_start, u64* jrnl_length, u64* db_start, u64* db_length); //returns serjio partions ranges in units of disk sector

void 	ramDiskSimulator_wipe(                 struct ramDiskSimulator*, u8); 		 // Overwrite (memset) the content of the disk with a byte argument (don't touch blocks metadata)
void 	ramDiskSimulator_wipeRange(            struct ramDiskSimulator*, u64 dlba, u64 size, u64 val);   // Range is given in units of NVMEIBC_SECTOR_SIZE
void 	ramDiskSimulator_wipe_dirty_bits(      struct ramDiskSimulator*, u8); 		 // Overwrite (memset) the content of the disk with a byte argument

void 	ramDiskSimulator_verify_no_dirty_bits( struct ramDiskSimulator*);	 // Useful function to verify a test didn't leave any dirty bits
void    ramDiskSimulator_verify_no_other_dirty_bits(struct ramDiskSimulator*, const union nvmeibc_dbits_entry dbits); // Allow only specifc dbits due to topology
void	ramDiskSimulator_setDirty(             struct ramDiskSimulator*, u64 dlba, u32 val);
void	ramDiskSimulator_DirtyVerifyAndClean(  struct ramDiskSimulator*, u64 dlba, u32 val);	// Verify this dbit exists and clean it
void 	ramDiskSimulator_lockStale(            struct ramDiskSimulator*, u64 dlba);  // Simulate as if another client left a stale lock in this address (in units of blocks)
void    ramDiskSimulator_lockStaleRO_EC(       struct ramDiskSimulator*, u64 dlba);  // Same as above but for read only lock
void 	ramDiskSimulator_lockUnSta(            struct ramDiskSimulator*, u64 dlba);  // Inverse of the above, remove the stale-special lock. If it was not stale-special, invokes a bug
bool    ramDiskSimulator_lockIsSta(            struct ramDiskSimulator*, u64 dlba);
int     ramDiskSimulator_CleanSta(             struct ramDiskSimulator*);      		 // Clean all stale locks on the disk (including stale special) and return their count
int 	ramDiskSimulator_CleanROL(             struct ramDiskSimulator*);			 // Clean all read only locks
void    ramDiskSimulator_set_lock(             struct ramDiskSimulator*, u64 dlba, u32 lock_id);	// put a lock_id within the specified disk lock
bool    ramDiskSimulator_is_locked(            struct ramDiskSimulator*, u64 dlba);	// returns TRUE when lock entry is locked, FALSE otherwise.
bool    ramDiskSimulator_is_locked_by(         struct ramDiskSimulator*, u64 dlba, u32 lock_id);	// returns TRUE when lock entry is locked by the given lock_id, FALSE otherwise (unlocked or locked with another value)
void    ramDiskSimulator_set_unlock(           struct ramDiskSimulator*, u64 dlba);// set lock table entry as unlocked, regardless of whatever is its value
int     ramDiskSimulator_clean_lock(           struct ramDiskSimulator*, u32 lock_id);	// clean a given lock_id & return the number of locks that were removed
void 	ramDiskSimulator_lockDo(               struct ramDiskSimulator*, u64 dlba);  // Simulate as if another client took the lock, preventing from all the others from taking it
void 	ramDiskSimulator_lockUn(               struct ramDiskSimulator*, u64 dlba);  // Simulate as if another client unlocked his lock. Inverse of ramDiskSimulator_lockDo()
void 	ramDiskSimulator_AllowUnlockedIO(      struct ramDiskSimulator*, bool permit);	// Ram disk f_allowIOwithoutLocks=permit
void    ramDiskSimulator_do_bad_sector_with_ptr(u64 *sec_start, u64 *md, short error_code); // Set block at a given index (address in block units) as bad sector with specific error_code
void 	ramDiskSimulator_do_bad_sector(        struct ramDiskSimulator*, u64 dlba, short error_code); // Set block at a given index (address in block units) as bad sector with specific error_code
void 	ramDiskSimulator_un_bad_sector(        struct ramDiskSimulator*, u64 dlba); // Inverse of the above
short   ramDiskSimulator_is_block_bad_sector(  const u64 *sec_start);               // Test ib block is bad sector, returns the error_code that was set
short 	ramDiskSimulator_is_bad_sector(        struct ramDiskSimulator*, u64 dlba); // Test ib block is bad sector, returns the error_code that was set
short   ramDiskSimulator_is_bad_byte_addr(     struct ramDiskSimulator*, u64 dlba_byte, int lengthPage);	// Same as above but for address in bytes and not in units of blocks
void	ramDiskSimulator_verify_no_bad_sectors(struct ramDiskSimulator*); // Verify all bad sectors were cleaned

// Internal API for server simulator components: Todo, move to a dedicated file
int ramDisk_execute_io(struct ramDiskSimulator* ram, struct nvmeibc_disk_io_command *cmd);

/************************** EC stuff ******************************************/
static const u8 nvmeib_disk_init_md_max[DISK_MAX_MD_SIZE_BYTE] = {[0 ... DISK_MAX_MD_SIZE_BYTE - 1] = DISK_MD_INIT_BYTE,};	// Entire MD is initialized to (~0)
const void *nvmeib_jmd_unused_entry_md_max(void);
void 	ramDiskSimulator_format_metadata(struct ramDiskSimulator *ram, bool enable_metadata);		// Format the disk to enable metadata or disable it
bool 	ramDiskSimulator_has_metadata(   const struct ramDiskSimulator *ram);						// Is the disk formatted with metadata
void 	ramDiskSimulator_mark_ec(struct ramDiskSimulator *ram, bool is_ec);							// Mark whether or not we are now running in EC configuration
void* 	ramDiskSimulator_get_metadataptr(       const struct ramDiskSimulator *ram, u64 block_ind);	// Get raw pointer to metadata of i'th data block.
void* 	ramDiskSimulator_get_metadataptr_jblk(  const struct ramDiskSimulator *ram, u64 j_ind);	    // Get raw pointer to metadata of i'th journal block.
void* 	ramDiskSimulator_get_metadataptr_unsafe(const struct ramDiskSimulator *ram, u64 block_ind);	// Same as above, regardless of formatting type (even if disk formatted without metadata), use with care!
void 	ramDiskSimulator_wipeMD(         struct ramDiskSimulator* _this, const void *one_md_entry); 		 	// Overwrite the MD of all blocks with the sample metadata
void 	ramDiskSimulator_wipeMD_jour(    struct ramDiskSimulator* _this, const void *one_md_entry);			// Overwrite the MD of all journal blocks with the sample metadata
void 	ramDiskSimulator_wipeMD_serjioDB(struct ramDiskSimulator* _this, const void *one_md_entry);			// Overwrite the MD of all Serjio DB with the sample metadata
void 	ramDiskSimulator_wipeMDRange(struct ramDiskSimulator* _this, u64 dlba, u64 nlbas, const void *md); // Range is given in units of NVMEIBC_SECTOR_SIZE
void 	ramDiskSimulator_MD_read(    struct ramDiskSimulator* _this, u64 dlba, u64 nlbas,       void *md); // Read a range of metadatas into 'md' array
void 	ramDiskSimulator_MD_write(   struct ramDiskSimulator* _this, u64 dlba, u64 nlbas, const void *md); // Write a range of metadatas from 'md' array
void 	ramDiskSimulator_reset_txid(      struct ramDiskSimulator *ram);								// reset TxID of all LOCKSET on disk. they become consistent for any EC setup
void 	ramDiskSimulator_reset_txid_range(struct ramDiskSimulator *ram, const u32 start, const u32 end);		// reset TxID of specifc LOCKSETS on disk. they become consistent for any EC setup
void 	ramDiskSimulator_read_txid( const struct ramDiskSimulator *ram, u32 tx_id[], u64 dlba, u32 size);	// read the tx_id of a set of locksets

// transport simulator conf object
struct ut_conf_transport {
	bool 		is_disk_callback_sync;
};


#endif  // H beginning

