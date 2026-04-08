/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBA_API_H
#define NVMEIBA_API_H

#include "nvmeib_common_os_block_api.h"

/* Must not include anything from the NVMesh nor ATOM codebase!
   nvmeiba API towards rest of NVMesh (mainly client) */

/* Used for NVMEIBC_ATOM_MIGHT_NOT_SUPPORT_DETACHING, to be removed eventually */
#define NVMEIBA_HACK_DETACHING_DYNAMIC_EXPORT 1

/* Status of nvmeiba_atom which acts as bitfield:
   bit 0(1) - is initialized,      bit 1(2) - is used by kernel (open/close/io),
   bit 2(4) - has nvmeibc support, bit 3(8) - is detaching */
enum nvmeiba_status {
	nvmeiba_status_illegal =   0x0,		// kzalloc() illegal, uninitialized status
	nvmeiba_status_hidden =    0x1,		// 1       - Object used as dummy without connecting to kernel (example: hidden attach). No IO possible, gendisk == NULL, was never initialized
	nvmeiba_status_orphan =    0x3,		// 1|2     - Nvmesh is upgrading and went down, nvmeiba remained orphan. IO is saved internally for future execution, gendisk != NULL
	nvmeiba_status_live =      0x7,		// 1|2|4   - Object connected to kernel, receiving io, passing it to block device. gendisk != NULL. Does not mean that io is enabled, just that BIO is received
	nvmeiba_status_detaching = 0xB,		// 1|2| |8 - IO is autofailed, block device and its os_api are detaching (freeing resources). gendisk becoming NULL.
};

/* Every block device which wants to communicate with OS must inherit (include)
   this class. */
struct nvmeiba_atom_os_api {
	enum nvmeiba_status status;
	//struct {
		struct request_queue *queue;		// IO request queue. Atom does not execute IO on queue and is oblivious of queue->queuedata. Inherited nvmeibc does
		struct gendisk *disk;   			// This is kernel's representation of our block device as disk device
		spinlock_t disk_lock;				// For exclusive access to gendisk. Daniel: Todo copy some used fields from gen_disk to atom and then can get rid of this lock
		atomic_t gendisk_status;			// Values: 1 = Gendisk was not added yet, 2 = adding, 3 = added (io possibly running)
		// If NVMEIBA_HACK_DETACHING_DYNAMIC_EXPORT is defined, this is interpreted like:
		// int (*set_detaching_fn)(struct nvmeiba_atom_os_api *atom); // nvmeiba_os_api_set_detaching
		// When this workaround is no longer needed, the field can become reserved (unused) once again
		u64 reserved[1];
	//} io_resources;
	ulong attach_jiff;						// When this atom was first created (units of jiffies)
	struct nvmeiba_bio_pending_list {		// Struct that manages pending IO list (gathered bio for future execution, while atom is abandoned)
		spinlock_t lock;					// Not needed: Can use global atom lock but that for clean understanding, easier to use another one
		struct bio_list bio_list;			// List of all pending IO's gathered by nvmeiba while nvmeibc upgrading
		u32 n_bios;							// Not needed: Just a cache for fast access without traversing the list == bio_list_size(bio_list)
		u64 reserved[1];
	} pender;
	struct nvmeiba_users {					// Stores information about which processes use the struct right now
		spinlock_t lock;					// Protects users list. open()/close() can happen simultaneously on different cores
		atomic_t n_opens; 					// Amount of higher level (OS) acceses to this device (mounts). 0 means volume is not used. If used, do not kill the block device (this will crash the OS)
		struct list_head pids;				// A list of users (pid's) who hold an open handle to the module
		bool readonly;  					// Reflects attach mode (not open mode). Is read only
		u64 reserved[1];
	} users;
	struct list_head list_all_os_apis;		// Connect to list of all OS api's.
	char dev_name[32 /*NVMEIBC_BD_NAME_LEN*/];// Name of the volume (If device name is very long, the kernel disk name will be truncated). Not NULL !
	struct nvmeiba_part {					// Legacy partition metadata, currently unused
		ulong offset;						// Offset in vlba from parent block device. unit of [bytes]. Length is written inside gen_disk
		struct nvmeiba_atom_os_api *parent;	// Direct ptr to parents. Self reference for non sub atoms
		struct list_head part_list;			// Link list of sub bdevs of parent
		spinlock_t list_lock_unused;
		union nvmeiba_part_flags {
			struct {
				u32 is_sub_atom : 1;		// By default false. Is atom a sub class (partition) of another atom
				u32 is_sub_auto_resize : 1;	// By default false. For sub atom - when carrier is resized, should sub atom be resized as well. For carrier - true if at lease 1 sub atom needs auto resizing
				u32 is_sub_unique_name : 1;
				u32 is_owner_of_reqctx : 1;	// By default false. If true, each sub atom will have non shared request queue data (bio execution/elevator/etc). If false, request data of carrier atom is shared to all sub atoms
				u32 is_sub_share_reqq  : 1;	// By default false. Deprecated, If True sub atom shares request queue with carrier. Support was removed for kernel 4+. Obviously: is_owner_of_reqctx=1 -> is_sub_share_reqq=0.
			};
			u32 all;
		} flags __attribute__((packed));
		u64 reserved[1];
	} sub;
	struct nvmeiba_config {
		bool enforce_readonly;				// Deprecated field
		u64 reserved[1];
	} conf;
	u16 alloc_size;							// Memory in bytes which stores the inherited nvmeibc_os_api in which atom resides. Relevant for managing orphans memory
};

/****************************** API for ATOM **********************************/
/* Daniel: Note, deliberately constructor/destructor moved from nvmeiba
   to nvmeibc, for better forward compatibility. ATOM has a basic
   con/des-structors which take care only for resources that are encapsulated in
   nvmeiba. Every resource which is shared with nvmeibc is created/destroyed
   by it and ATOM does not even know if resource state is correct. It assumes
   nvmeibc disconnects and reconnects to ATOM in a decent way */

/* This is NOT a virtual constructor! nvmeibc_os_api already created itself and
   ATOM. Only registers ATOMS internal resources unrelated to nvmeibc */
void nvmeiba_os_api_constructor(struct nvmeiba_atom_os_api *atom);

/* This is NOT a virtual destructor! nvmeibc_os_api already destroyed itself,
   but has not kfree. This destructor frees only resources nvmeiba_os_api & actually frees
   memory of 'atom + inheritted os_api. */
void nvmeiba_os_api_destructor(struct nvmeiba_atom_os_api *atom);

/* block device (nvmeibc_os_api) abandons nvmeiba_atom upon upgrade and adopts
   it back upon booting with a newer version.
   Step 1: Abandon request queue
   Step 2: When all IO's drains, abandon the atom (set status to abandoned)
   Optional Step3: nvmeibc instance can probe which atoms are abandoned, to decide how/whom to adopt
   Step 4: Adopt back. For single nvmeibc instance, adoption by name is enough
           For multi-instance dir/name is required */
int  nvmeiba_os_api_orphan_abandon(struct nvmeiba_atom_os_api *atom);
bool nvmeiba_os_api_is_queue_orphan(const struct nvmeiba_atom_os_api *atom);
int  nvmeiba_os_api_set_detaching(struct nvmeiba_atom_os_api *atom);
void nvmeiba_os_api_exec_for_each_atom(const char* dev_dir, void (*fn)(const struct nvmeiba_atom_os_api *atom, void *ctx), void* ctx);
struct nvmeiba_atom_os_api *nvmeiba_os_api_orphan_adopt(const char* dev_dir, const char *dev_name);

ssize_t nvmeiba_atom_users_to_string(void *_atom, char *buf, size_t len);	// const atom. The prototype of function written as callback for proc files

int  nvmeiba_atom_open( struct BLK_MODE_OPEN_OBJ_T *bdev, const char *name);
void nvmeiba_atom_close(struct gendisk *disk);

/*********************** API for List of all ATOMS ****************************/
/* Get the git commit version of nvmeiba module (for future compatibility)*/
u64 nvmeiba_os_apis_get_version(void);

/* Get the amount of atoms (bdevs) as seen by kernel (attached, unsafely detached, orphans, etc)
   req_type = 'A' = all, 'O' - num orphans only. 'C' - number of nvmeibc instances connected to atom. Other values for future support */
int nvmeiba_os_apis_get_num(unsigned char req_type);

#define NVMEIBA_2_C_PROTO_VERSION_V_2_0				(2)
#define NVMEIBA_2_C_PROTO_VERSION_V_2_1				(3)
struct nvmeiba_to_c_handover {						// Upon connection, handover of nvmeiba to nvmeibc
	const struct block_device_operations *fops;		// To be overwritten by nvmeibc
	u32 n_orphan_osapi;								// Amount of orphan atoms that must be taken over by nvmeibc
	u8 protocol_version;							// Version of nvmeiba with respect to nvmeibc
};

struct nvmeiba_to_c_handover nvmeiba_os_do_on_nvmeibc_up(void);
void nvmeiba_os_do_on_nvmeibc_down(void);

#endif
