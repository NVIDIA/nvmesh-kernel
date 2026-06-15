/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <linux/kref.h>
#include <linux/module.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>

/* NVMesh includes */
/* clang-format off */
#include "nvmeib.h"
#include "nvmeibc_disk.h"
#include "nvmeibc_volume.h"
#include "nvmeibc_main.h"
#include "nvmeibc_pausable.h"
#include "nvmeibc_cc_api.h"
#include "nvmeibc_jam.h"
#include "main/nvmeibc_main_common.h"
#include "module/instance/nvmeibc_cinst_params.h"
#include "main/utils/nvmeibc_main_block_gen_work_sched.h"
#include "block/nvmeibc_block_common.h"
#include "block/recovery/nvmeibc_block_dp_sync_common.h"
#include "nvmeibs_disk.h"
#include "nvmeibs_disk_locks.h"
#include "nvmeib_public.h"
/* clang-format on */

/* Defines netlink messages */
#include "corecomm_netlink_if.h"
#include "corecomm_netlink_rpc_srv.h"
#include "corecomm_injections.h"

/**
 * This is the corecomm kernel side. In the future make this a separate driver.
 * Curerntly it is part of nvmeibc.
 */

#define DISK_SW_MD_SIZE(disk)                                                  \
	((disk)->md_size << (NVMEIBC_SECTOR_SHIFT - (disk)->sector_shift))

static atomic_t ___running_handle = ATOMIC_INIT(1);
#define corecomm_next_handle atomic_inc_return(&___running_handle)

/*****************************************************/
/********************** UTILS ************************/
/*****************************************************/

static int __corecomm_uuid_parse(uuid_be *out, const char *uuid, bool big_endian) {
	if (big_endian)  return -EINVAL;
	return uuid_parse(uuid, &out.b[0]);
}

/*****************************************************/
/********************** CONFIG ***********************/
/*****************************************************/

/**
 * Target arnics
 * This is a map from target ID to arnics, information used later for discovery
 * It emulates real sustem nvmeibc_target data structure, can roughtly be
 * seen as a replacement for MCS in corecomm layer.
 * Correctness checks are minimal to none, be warned.
 */
static struct corecomm_target_cfg {
	char node_id[40];
	struct list_head /*of struct nvmeibc_admin_rnic*/ arnics;
	size_t size;
} global_targets[32];

static int __corecomm_add_target_arnic(const char *node_id, const char *gid,
                                       unsigned short pkey,
                                       enum rdma_type type) {
	int rv                           = 0;
	struct nvmeibc_admin_rnic *arnic = NULL;
	union ib_gid _gid;
	int i;

	/* Why I need this hack: It appears that sscanf in kernel does not
	   behave the same as user. Sepcifically it will not supprot
	   sscanf(str, "%16lx%16lx", &x, &y) - it sees them as one number.
	   this is a fix. */
	char gidlow[17] = {0}, gidhi[17] = {0};
	memcpy(gidhi, gid+2, 16);
	memcpy(gidlow, gid+18, 16);

	if (sscanf(gidhi, "%lx", (unsigned long *)&_gid.global.subnet_prefix) != 1 ||
	    sscanf(gidlow, "%lx", (unsigned long *)&_gid.global.interface_id) != 1) {
		printk(KERN_ALERT "Bad gid format %s\n", gid);
		rv = -EINVAL;
		goto err;
	}

	for (i = 0; i < sizeof(global_targets) / sizeof(global_targets[0]); ++i)
		if (!global_targets[i].node_id[0] ||
		    !strncmp(global_targets[i].node_id, node_id,
		             sizeof(global_targets[i].node_id)))
			break;

	if (i >= sizeof(global_targets) / sizeof(global_targets[0])) {
		rv = -ENOMEM;
		goto err;
	}

	if (!global_targets[i].node_id[0]) {
		strncpy(global_targets[i].node_id, node_id,
		        sizeof(global_targets[i].node_id));
		INIT_LIST_HEAD(&global_targets[i].arnics);
		global_targets[i].size = 0;
	}

	list_for_each_entry(arnic, &global_targets[i].arnics, link) {
		if (arnic && !memcmp(&arnic->ib_gid, &_gid, sizeof(union ib_gid))) {
			/* Already have it. Skip. */
			goto err;
		}
	}

	if (!(arnic = kzalloc(sizeof(struct nvmeibc_admin_rnic), GFP_KERNEL))) {
		rv = -ENOMEM;
		goto err;
	}

	if (!(arnic->score =
	          kzalloc(sizeof(struct nvmeibc_arnic_score), GFP_KERNEL))) {
		rv = -ENOMEM;
		goto err;
	}

	global_targets[i].size++;

	arnic->ib_gid = _gid;

	arnic->ib_gid.global.subnet_prefix =
	    NVMEIB_HTONLL(arnic->ib_gid.global.subnet_prefix);
	arnic->ib_gid.global.interface_id =
	    NVMEIB_HTONLL(arnic->ib_gid.global.interface_id);

	arnic->pkey = pkey;

	arnic->priority  = 0;
	arnic->num_conns = 0; /* Start fresh */

	arnic->order = global_targets[i].size - 1;

	strlcpy(arnic->node_id, node_id, sizeof(arnic->node_id));

	arnic->score->gid = arnic->ib_gid;

	/* Other values */
	if (type == CORECOMM_RDMA_IB) {
		arnic->service_id   = NVMEIB_SERVICE_ID;
		arnic->service_port = 0;
	} else if (type == CORECOMM_RDMA_ROCE) {
		arnic->service_id   = 0;
		arnic->service_port = NVMEIB_PORT_ID;
	} else if (type == CORECOMM_RDMA_IWARP) {
		arnic->service_id   = 0;
		arnic->service_port = nvmeib_get_tcp_base_port_id();
	}

	/* There are more fileds in arnic, those are not set here */

	list_add(&arnic->link, &global_targets[i].arnics);

err:
	printk(KERN_INFO "Adding arnic %s type %d result=%d", gid, type, rv);
	if (rv && arnic) {
		if (arnic->score) kfree(arnic->score);
		kfree(arnic);
	}

	return rv;
}

static struct corecomm_target_cfg *
__corecomm_find_target_cfg(const char *node_id) {
	int i;

	for (i = 0; i < sizeof(global_targets) / sizeof(global_targets[0]); ++i)
		if (!strncmp(global_targets[i].node_id, node_id,
		             sizeof(global_targets[i].node_id)))
			return &global_targets[i];
	return NULL;
}

static void __corecomm_destroy_target_cfg_store(void) {
	int i;

	for (i = 0; i < sizeof(global_targets) / sizeof(global_targets[0]); ++i)
		if (global_targets[i].node_id[0]) {
			struct corecomm_target_cfg *target = &global_targets[i];
			while (!list_empty(&target->arnics)) {
				struct nvmeibc_admin_rnic *arnic = list_first_entry(
				    &target->arnics, struct nvmeibc_admin_rnic, link);
				list_del(&arnic->link);
				if (arnic->score) kfree(arnic->score);
				kfree(arnic);
			}
		}
}

/*****************************************************/
/***************** NVMEIBS INTERFACE *****************/
/*****************************************************/
/**
 * I don't want to introduce dependecy on nvmeibs.
 * There is nothing wrong in running corecomm with nvmeibc only (the otehr
 * direction is not correct by the way). Problem is, sometimes nvmeibs is needed
 * (example - read RAM directly on server side to validate some operation
 * correctness). For this, I introduce this set of virtual nvmeibs functions
 * that are resolved on demand.
 */
struct nvmeibs_if {
	bool is_initialized;
	struct list_head *(*disk_get_disks)(int *);
	void (*disk_put_disks)(void);
	int (*nvme_format_disk)(const char *, struct nvmeib_format_disk *,
	                        struct nvmeib_new_format_info *);
	struct nvmeibs_disk_info *(*disk_freeze)(const char *);
	void (*disk_unfreeze)(const char *);
};

#define __GET_NVMEIBS_FUNC(dest, name)                                         \
	if (!((dest) = (void *)nvmeib_kallsyms_lookup_name(#name))) {              \
		printk(KERN_ALERT "Error getting function %s from server", #name);     \
		return false;                                                          \
	}

static bool __get_nvmeibs_if(struct nvmeibs_if *sif) {
	sif->is_initialized = false;
#if KS_KALLSYMS_LOOKUP
	__GET_NVMEIBS_FUNC(sif->disk_get_disks, nvmeibs_disk_get_disks);
	__GET_NVMEIBS_FUNC(sif->disk_put_disks, nvmeibs_disk_put_disks);
	__GET_NVMEIBS_FUNC(sif->nvme_format_disk, nvmeibs_nvme_format_disk);
	__GET_NVMEIBS_FUNC(sif->disk_freeze, nvmeib_disk_freeze);
	__GET_NVMEIBS_FUNC(sif->disk_unfreeze, nvmeib_disk_unfreeze);
	sif->is_initialized = true;
	return true;
#else // !KS_KALLSYMS_LOOKUP
	return false;
#endif
}

/**
 * This one is called on connection close, allows to release resources allocated
 * for server interface
 * @TODO: Currenly a placeholder, drop the use of kallsyms and introduce cross
 * registration from server
 */
static void __put_nvmeibs_if(struct nvmeibs_if *sif) {}

/*****************************************************/
/********************** CDISK ************************/
/*****************************************************/

/**
 * Cdisk store is the etry point. It maps between cdisk kernel structure and its
 * handle known to user space. At this stage implemented in dumbest way using an
 * array, where handle is the index of the array.
 * Two basic operations are -
 * Discover
 * 1. Does some magic to get cdisk pointer given cdisk name
 * 2. Adds cdisk to store (if needed) and returns the handle
 * Remove
 * 1. Remove cdisk from store
 * 2. Free resources (if can)
 * Get by handle
 * 1. Given a handle from prev step, return cdisk object
 *
 * This is implementation detail and should not be known outside this file.
 */
#define CORECOMM_MAX_CDISKS 1024
struct corecomm_cdisk_info {
	/**
	 * Volume and disk_id structs are stubs.
	 * For the sake of simulation, we have 1 disk_id and 1 volume for each disk.
	 * The sole purpose here is to be able to use real functions for discovery,
	 * pause, cont etc.
	 */
	struct nvmeibc_volume *volume;
	struct nvmeibc_disk_id *disk_id;
	struct nvmeibc_disk *disk;
	void *mem_info;
};
static struct cdisk_store {
	struct corecomm_cdisk_info arr[CORECOMM_MAX_CDISKS];
} global_cdisk_store = {{{0}}}; /* Global vars initialized to 0 */

static struct nvmeib_lock_entry_constants global_lock_entry_constants = {
    .w_blkset_info          = true,
    .blkset_info_txid_shift = NVMEIB_BLKSET_INFO_TXID_SHIFT,
    .blkset_info_txid_mask  = NVMEIB_BLKSET_INFO_TXID_MASK,
    .blkset_info_dbits_shift =
        NVMEIB_EC_JMDC_BITS_TX_ID + NVMEIB_BLKSET_INFO_TXID_SHIFT,
    .blkset_info_dbits_mask = NVMEIB_BLKSET_INFO_DIRTY_MASK,
    .stale_bit_mask         = 0 /*Don't care*/
};

struct nvmeibc_disk_create_on_main_wq_param {
	const struct nvmeibc_cinst_params_core *p;
	struct nvmeibc_disk_id *disk_id;
	struct list_head *arnics;
	int num_ranges;
	const char *node_id;
};

static int nvmeibc_disk_create_on_main_wq(void *_param) {
	struct nvmeibc_disk_create_on_main_wq_param *param = _param;
	return nvmeibc_disk_create(param->p, param->disk_id, param->arnics,
	                           param->num_ranges, param->node_id);
}

static cdisk_handle cdisk_store_discover(const char *disk_name,
                                         const char *node_id) {
	cdisk_handle i;
	/* Do we already havbe it in our store? */
	/* @TODO: Add node_id as a part of the store key */
	for (i = 0; i < CORECOMM_MAX_CDISKS; ++i)
		if (global_cdisk_store.arr[i].disk &&
		    !strncmp(global_cdisk_store.arr[i].disk->name, disk_name,
		             sizeof(global_cdisk_store.arr[i].disk->name)))
			return i + 1;
	/* Ok, so we don't have it. find an empty spot then. */
	for (i = 0; i < CORECOMM_MAX_CDISKS; ++i)
		if (!global_cdisk_store.arr[i].disk) break;
	if (i >= CORECOMM_MAX_CDISKS) return -ENOBUFS; /* No empty spot? :((( */
	/* Find cdisk */
	{
		/* Client instance - always use cinst 0. Maybe change in the future */
		int rv;
		struct corecomm_target_cfg *target =
		    __corecomm_find_target_cfg(node_id);
		const struct nvmeibc_cinst_params *cips =
		    nvmeibc_cinst_get_by_name("nvmeibc");
		struct nvmeibc_volume *volume   = NULL;
		struct nvmeibc_disk_id *disk_id = NULL;
		if (!target) return -EINVAL;
		if (!cips) {
			WARN(true, "Could not find cinst 0");
			return -ENOENT;
		}
		/* Allocate memory for stub structures */
		if (!(volume = global_cdisk_store.arr[i].volume =
		          kzalloc(sizeof(struct nvmeibc_volume), GFP_KERNEL))) {
			return -ENOMEM;
		}
		if (!(disk_id = global_cdisk_store.arr[i].disk_id =
		          kzalloc(sizeof(struct nvmeibc_disk_id), GFP_KERNEL))) {
			kfree(volume);
			return -ENOMEM;
		}
		/* Important fields to initialize:
		 * volume.full_name, volume.spinlock
		 * disk_id.name, disk_id.volume
		 */
		scnprintf(volume->full_name, sizeof(volume->full_name),
		          "CoreDummyVolume_%s", disk_name);
		spin_lock_init(&volume->spinlock);
		volume->p = &cips->main;
		strncpy(disk_id->name, disk_name, sizeof(disk_id->name));
		disk_id->volume = volume;

		/* Disk crate must be run from the main wq. Because. */
		rv = nvmeibc_run_on_main_wq(
		    &cips->main, nvmeibc_disk_create_on_main_wq,
		    &(struct nvmeibc_disk_create_on_main_wq_param){
		        &cips->core, disk_id, &target->arnics,
		        /*num_ranges - not used*/ 0xdead, node_id},
		    /*drain*/ true, /*sleep*/ true, /*bullshit*/ NULL);
		if (rv) {
			kfree(volume);
			kfree(disk_id);
			return rv;
		}

		/* @TODO: Maybe waiting here is not the best option, should be done from
		 * a separate call? Just consider, for the future Yuri that will be
		 * smarter than me. */
		WARN_ON(!disk_id->disk);
		global_cdisk_store.arr[i].disk = disk_id->disk;
		wait_for_completion(disk_id->disk->discover_comp);

		/* Mem info. @TODO: what if not found? Should not happen but... */
		global_cdisk_store.arr[i].mem_info =
		    nvmeibc_disk_locks_seg_locks_mem_info(disk_id->disk, -1);

		return i + 1; /*return handle*/
	}

	/* We should not get here */
	WARN_ON(true);
	return -ENODEV;
}

static void cdisk_store_remove(cdisk_handle handle) {
	handle = handle - 1;
	if (handle >= 0 && handle < CORECOMM_MAX_CDISKS &&
	    global_cdisk_store.arr[handle].disk) {

		nvmeibc_disk_remove(global_cdisk_store.arr[handle].disk_id, true);
		kfree(global_cdisk_store.arr[handle].disk_id);
		kfree(global_cdisk_store.arr[handle].volume);
		kfree(global_cdisk_store.arr[handle].mem_info);

		global_cdisk_store.arr[handle].disk = NULL;
	}
}

static void cdisk_store_clean(void) {
	cdisk_handle i;
	for (i = 0; i < CORECOMM_MAX_CDISKS; ++i) {
		cdisk_store_remove(i + 1);
	}
}

static struct corecomm_cdisk_info *cdisk_store_get(cdisk_handle handle) {
	handle = handle - 1;
	if (handle >= 0 && handle < CORECOMM_MAX_CDISKS &&
	    global_cdisk_store.arr[handle].disk) {
		return &global_cdisk_store.arr[handle];
	}
	return NULL;
}

/*****************************************************/
/************************** NDB **********************/
/*****************************************************/

/**
 * Corecomm IO path does not pass userspace buffers to kernel and back.
 * Instead it pre-allocates NDBs (struct nvmeib_data_buffer) with ready to
 * use sgl of a desired size, inside the *kernel* memory adress space.
 * Userspace can reference these buffers by handle.
 *
 * Then, data can be set/read using utility functions.
 *
 * @TODO: Allow mmap these buffers to user space.
 * @TODO: Add locks. Do we need thread safety here? For now I don't care.
 */

struct corecomm_ndb_info {
	struct list_head
	    link; /* Keep it first so it is easier to parse with crash */
	struct nvmeib_data_buffer ndb;
	void *md;
	unsigned long soft_md[PAGE_SIZE];
	ndb_handle handle;
};

static struct corecomm_ndb_info *ndb_store_add(struct list_head *store,
                                               unsigned int n_pages) {
	struct corecomm_ndb_info *entry = NULL;
	struct scatterlist *sg;
	int i;                     /* I just love some C features. Sigh... */
	if (!n_pages) return NULL; /*Sanity*/
	if (!(entry = kzalloc(sizeof(*entry), GFP_KERNEL))) return NULL;
	if (!(entry->md = (void *)get_zeroed_page(GFP_KERNEL))) {
		kfree(entry);
		return NULL;
	}
	entry->handle = corecomm_next_handle;
	{
		/* This is the key logic - init a valid ndb entry */
		struct nvmeib_data_buffer *ndb = &entry->ndb;
		ndb->length                    = NVMEIBC_SECTOR_SIZE * n_pages;
		ndb->table.nents                  = n_pages;

		if ((sg_alloc_table(&ndb->table, n_pages, GFP_KERNEL) < 0)) {
			goto err;
		}
		for_each_sg(ndb->table.sgl, sg, ndb->table.nents, i) {
			struct page *page = alloc_pages(GFP_KERNEL | __GFP_ZERO, 0);
			if (!page) { goto err; }
			sg_set_page(sg, page, PAGE_SIZE, 0);
		}
		/* @TODO: Allocate metadata here. This will affect the maimum IO size.
		 * For now lets just ignore it. */
	}
	/* Finally, if all good */
	list_add_tail(&entry->link, store);
	return entry;
err:
	if (entry) {
		for_each_sg(entry->ndb.table.sgl, sg, entry->ndb.table.nents, i) {
			struct page *page = sg_page(sg);
			if (page) __free_pages(page, 0);
		}
		sg_free_table(&entry->ndb.table);
		free_page((unsigned long)entry->md);
		kfree(entry);
	}
	return NULL;
}

static struct corecomm_ndb_info *ndb_store_get(struct list_head *store,
                                               ndb_handle handle) {
	struct corecomm_ndb_info *entry = NULL;
	list_for_each_entry(entry, store, link) {
		if (entry->handle == handle) return entry;
	}
	return NULL;
}

static void ndb_store_delete(struct list_head *store, ndb_handle handle) {
	struct corecomm_ndb_info *entry = ndb_store_get(store, handle);
	struct scatterlist *sg;
	int i;
	if (!entry) return;
	list_del(&entry->link);
	for_each_sg(entry->ndb.table.sgl, sg, entry->ndb.table.nents, i) {
		struct page *page = sg_page(sg);
		if (page) __free_pages(page, 0);
	}
	sg_free_table(&entry->ndb.table);
	free_page((unsigned long)entry->md);
	kfree(entry);
}

static void ndb_store_clean(struct list_head *store) {
	while (!list_empty(store)) {
		ndb_store_delete(
		    store,
		    list_first_entry(store, struct corecomm_ndb_info, link)->handle);
	}
}

/*****************************************************/
/*************** CONNECTION MANAGEMENT ***************/
/*****************************************************/

/* This is the netlink socket we are using to speak with userspace */
static struct sock *global_nl_sk = NULL;

/**
 * Object representing a single session of communication between user process
 * and the kerrnel via netlink
 */
struct corecomm_connection_ctx {
	struct list_head link; /* Link to global store */
	struct kref ref;       /* Each connection is refcounted object */
	int pid;               /* Port ID */
	struct sock *sk;       /* Socket to send responses on */

	struct nvmeibs_if sif; /* Server direct interface */
	struct list_head ndbs; /* elem type struct corecomm_ndb_info */
};

/**
 * Represents the store of all active connections
 */
struct corecomm_connection_store {
	spinlock_t guard;
	struct list_head list; /* element type struct corecomm_connection_ctx */
	struct proc_dir_entry *proc;
	wait_queue_head_t wq; /* Wait queue - to await disconnection on */
	bool is_terminating;
} global_connections_store;

/**
 * Create new connection object, raising its refcount
 * @param pid Port id it is bound to
 * @param gfp_flags - GFP flags to use for allocation
 * @return New connection object or NULL on error
 */
static struct corecomm_connection_ctx *
corecomm_create_connection(int pid, gfp_t gfp_flags) {
	struct corecomm_connection_ctx *ctx;
	ulong flags;
	if (!(ctx = kzalloc(sizeof(*ctx), gfp_flags))) return NULL;
	ctx->pid = pid;
	kref_init(&ctx->ref);
	ctx->sk = global_nl_sk;

	/* Initialization */
	__get_nvmeibs_if(&ctx->sif);
	INIT_LIST_HEAD(&ctx->ndbs);

	spin_lock_irqsave(&global_connections_store.guard, flags);
	list_add_tail(&ctx->link, &global_connections_store.list);
	spin_unlock_irqrestore(&global_connections_store.guard, flags);
	return ctx;
}

/**
 * Get connection objected, raising its refcount
 * @return Connection object or NULL if not found
 */
static struct corecomm_connection_ctx *corecomm_get_connection(int pid) {
	ulong flags;
	struct corecomm_connection_ctx *ctx;
	spin_lock_irqsave(&global_connections_store.guard, flags);
	list_for_each_entry(ctx, &global_connections_store.list, link) {
		if (pid == ctx->pid) {
			kref_get(&ctx->ref);
			spin_unlock_irqrestore(&global_connections_store.guard, flags);
			return ctx;
		}
	}
	spin_unlock_irqrestore(&global_connections_store.guard, flags);
	return NULL;
}

static void corecomm_put_connection_last_ref(struct kref *ref) {
	ulong flags;
	struct corecomm_connection_ctx *ctx =
	    container_of(ref, struct corecomm_connection_ctx, ref);

	spin_lock_irqsave(&global_connections_store.guard, flags);
	list_del(&ctx->link);
	spin_unlock_irqrestore(&global_connections_store.guard, flags);

	/* Destruction */
	__put_nvmeibs_if(&ctx->sif);
	ndb_store_clean(&ctx->ndbs);
	kfree(ctx);
}

/**
 * Put connection object, decrease refcount
 */
static void corecomm_put_connection(struct corecomm_connection_ctx *ctx) {
	if (ctx) kref_put(&ctx->ref, corecomm_put_connection_last_ref);
}

static ssize_t corecomm_control_proc_read(struct file *filp, char __user *user,
                                          size_t len, loff_t *off) {
	int rv = wait_event_interruptible_timeout(
	    global_connections_store.wq, global_connections_store.is_terminating,
	    2 * HZ);
	if (!rv) return 0; /* Timeout */
	printk(KERN_INFO "Control proc has been terminated\n");
	(void)user;
	(void)off;
	(void)len;
	return -1; /* Terminate */
}

static int corecomm_control_proc_open(struct inode *inode, struct file *filp) {
	struct corecomm_connection_ctx *ctx =
	    corecomm_create_connection(get_current()->tgid, GFP_KERNEL);
	if (!ctx) return -ENOMEM;
	filp->private_data = ctx;
	printk(KERN_INFO "Connection created %d\n", get_current()->tgid);
	return 0;
}

static int corecomm_control_proc_release(struct inode *inode,
                                         struct file *filp) {
	corecomm_put_connection(
	    (struct corecomm_connection_ctx *)filp->private_data);
	printk(KERN_INFO "Connection closed %d\n",
	       ((struct corecomm_connection_ctx *)filp->private_data)->pid);
	return 0;
}

static struct file_operations fops = {.read    = corecomm_control_proc_read,
                                      .open    = corecomm_control_proc_open,
                                      .release = corecomm_control_proc_release};

/**
 * Must be called first - init the global store
 * Will also open the control proc
 * Netlink must be ready by the time of this fcuntion call
 * @return 0 on success, error code otherwise
 */
static int corecomm_connection_store_init(void) {
	INIT_LIST_HEAD(&global_connections_store.list);
	spin_lock_init(&global_connections_store.guard);
	init_waitqueue_head(&global_connections_store.wq);
	global_connections_store.proc           = NULL;
	global_connections_store.is_terminating = false;
	{
		const struct nvmeibc_cinst_params *cips =
		    nvmeibc_cinst_get_by_name("nvmeibc");
		struct t_main_clnt_globals *_mg =
		    __get_from_params_main_globals_container(&cips->main);
		if (!_mg->proc_dir.root) return -EINVAL;

		/* Not using nvmeib_public_proc_create because I need .release and .open fops
		 */
		global_connections_store.proc =
		    proc_create("corecomm", 0444, _mg->proc_dir.root, &fops);
		if (!global_connections_store.proc) return -1;
	}
	return 0;
}

/**
 * Relase all corecomm connections and resources, called before close
 */
static void corecomm_connection_store_destroy(void) {
	if (global_connections_store.proc) {
		printk(KERN_INFO "Destroying connection store");
		global_connections_store.is_terminating = true;
		wake_up_all(&global_connections_store.wq);
		proc_remove(global_connections_store.proc);
		global_connections_store.proc = NULL;
	}
}

/*****************************************************/
/***************** SPECIFIC HANDLERS *****************/
/*****************************************************/
/* Here will be all the specific handlers per message. This is the actual body
 * of of the layer. */

NLRPC_SRV_SYNC(corecomm_format_local_disk_, struct corecomm_new_format_info,
               rsp, ctx, (struct corecomm_format_disk, fd)) {
	BUILD_BUG_ON(sizeof(*rsp) != sizeof(struct nvmeib_new_format_info));
	BUILD_BUG_ON(sizeof(fd) != sizeof(struct nvmeib_format_disk));
	if (!ctx->sif.is_initialized) {
		printk(KERN_ALERT
		       "Could not resolve nvmeibs_if, perhaps nvmeibs is down?\n");
		return -EAFNOSUPPORT;
	}
	return ctx->sif.nvme_format_disk(fd.disk_id, (void *)&fd, (void *)rsp);
}

NLRPC_SRV_SYNC(corecomm_freeze_, int, rsp, ctx, (name_t, disk_name)) {
	if (!ctx->sif.is_initialized) {
		printk(KERN_ALERT
		       "Could not resolve nvmeibs_if, perhaps nvmeibs is down?\n");
		return -EAFNOSUPPORT;
	}
	if (!ctx->sif.disk_freeze(disk_name)) return -EFAULT;
	return 0;
}

NLRPC_SRV_SYNC(corecomm_unfreeze_, int, rsp, ctx, (name_t, disk_name)) {
	if (!ctx->sif.is_initialized) {
		printk(KERN_ALERT
		       "Could not resolve nvmeibs_if, perhaps nvmeibs is down?\n");
		return -EAFNOSUPPORT;
	}
	ctx->sif.disk_unfreeze(disk_name);
	return 0;
}

NLRPC_SRV_SYNC_BYREF(corecomm_set_symbol_, int, rsp, ctx,
                     (long_name_t, sym_name), (int, size),
                     (struct page_container, value), (int, deref)) {
	void *addr;
	(void)ctx; /* Unused */
	if (msg->size < 1 || msg->size > sizeof(struct page_container))
		return -EINVAL;
#if KS_KALLSYMS_LOOKUP
	if (!(addr = (void *)nvmeib_kallsyms_lookup_name(msg->sym_name))) {
		extern struct corecomm_inj_sym_store *corecomm_inj_default_store;
		BUG_ON(!corecomm_inj_default_store);
		addr = corecomm_inj_get_sym(corecomm_inj_default_store, msg->sym_name,
		                            msg->size);
		BUG_ON(!addr);
	}
	if (msg->deref) {
		memcpy(*(void **)addr, msg->value.data, msg->size);
	} else {
		memcpy(addr, msg->value.data, msg->size);
	}
	return 0;
#else // !KS_KALLSYMS_LOOKUP
	return -ENXIO;
#endif
}

NLRPC_SRV_SYNC_BYREF(corecomm_read_symbol_, struct page_container, rsp, ctx,
                     (long_name_t, sym_name), (int, size),
                     (struct page_container, value), (int, deref)) {
	void *addr;
	(void)ctx; /* Unused */
	if (msg->size < 1 || msg->size > sizeof(struct page_container))
		return -EINVAL;
#if KS_KALLSYMS_LOOKUP
	if (!(addr = (void *)nvmeib_kallsyms_lookup_name(msg->sym_name)))
		return -ENOENT;
	if (msg->deref) {
		memcpy(rsp, *(void **)addr, msg->size);
	} else {
		memcpy(rsp, addr, msg->size);
	}
	return 0;
#else // !KS_KALLSYMS_LOOKUP
	return -ENXIO;
#endif
}

NLRPC_SRV_SYNC(corecomm_register_arnic_, int, rsp, ctx, (name_t, node_id),
               (name_t, gid), (unsigned short, pkey), (enum rdma_type, type)) {
	(void)ctx; /* Unused */
	return __corecomm_add_target_arnic(node_id, gid, pkey, type);
}

NLRPC_SRV_SYNC(corecomm_discover_, cdisk_handle, rsp, ctx, (name_t, disk_name),
               (name_t, node_id)) {
	(void)ctx;
	*rsp = cdisk_store_discover(disk_name, node_id);
	if (rsp < 0) return *rsp;
	return 0;
}

NLRPC_SRV_SYNC(corecomm_disk_remove_, int, rsp, ctx, (cdisk_handle, handle)) {
	(void)ctx; /* Unused */
	cdisk_store_remove(handle);
	return 0;
}

/** Data structure used to bea able to decode disk and connection from rdma
 * response
 */
struct corecomm_pd_op_ctx {
	struct corecomm_connection_ctx *ctx;
	struct corecomm_cdisk_info *dinfo;

	union {
		struct nvmeibc_d_rdma_comp dc;
		struct {
			struct nvmeibc_disk_free_jrnl_ents_comp free_ents_comp;
			struct nvmeibc_disk_gen_cmd gen_cmd;
		};
	};
};

static struct corecomm_pd_op_ctx *
__init_corecomm_pd_op_ctx(gfp_t gfp_flags, struct corecomm_connection_ctx *ctx,
                          struct corecomm_cdisk_info *dinfo) {
	struct corecomm_pd_op_ctx *op_ctx =
	    kzalloc(sizeof(struct corecomm_pd_op_ctx), gfp_flags);
	if (!op_ctx) return NULL;
	op_ctx->dinfo         = dinfo;
	op_ctx->ctx           = ctx;
	op_ctx->dc.lock_cnsts = &global_lock_entry_constants;
	return op_ctx;
}

void corecomm_pd_please_kill_yourself_cb_(void *_ctx) {
	struct corecomm_pd_op_ctx *op_ctx   = _ctx;
	struct corecomm_connection_ctx *ctx = op_ctx->ctx;
	int rv = 0;
	nvmeibc_pd_cb_called_comp(op_ctx->dinfo->disk, &op_ctx->dc);
	kfree(op_ctx);
	nlrpc_reply(rv, ctx, GFP_KERNEL);
}

NLRPC_SRV_ASYNC(corecomm_pd_please_kill_yourself_, ctx, (cdisk_handle, cdisk),
                (unsigned int, rsc_id), (unsigned long long, dlba)) {
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	int rv;
	struct corecomm_pd_op_ctx *op_ctx;
	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	op_ctx = __init_corecomm_pd_op_ctx(GFP_KERNEL, ctx, dinfo);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	if ((rv = nvmeibc_pd_dbg_please_kill_yourself(
	         dinfo->disk, corecomm_pd_please_kill_yourself_cb_, op_ctx, rsc_id,
	         dlba))) {
		printk(KERN_ALERT
		       "Error, calling nvmeibc_pd_dbg_please_kill_yourself %d\n",
		       rv);
		return rv;
	}
	return 0;
}

static int corecomm_pd_lock_cb(struct nvmeibc_d_rdma_comp *dc) {
	struct corecomm_pd_op_ctx *op_ctx =
	    container_of(dc, struct corecomm_pd_op_ctx, dc);
	struct corecomm_connection_ctx *ctx = op_ctx->ctx;
	struct lock_data ld;

	printk(KERN_INFO "Inside corecomm_pd_lock_cb\n");

	/* Call this function directly, need for block logic validity checks like in
	 * real code */
	nvmeibc_pd_cb_called_comp(op_ctx->dinfo->disk, dc);

	ld.status = dc->lock_status;
	ld.value  = dc->lock.id | (dc->lock.bi << 32);
	kfree(op_ctx);

	return nlrpc_reply(ld, ctx, GFP_ATOMIC);
}

NLRPC_SRV_ASYNC(corecomm_pd_cmpxchg_, ctx, (cdisk_handle, handle),
                (unsigned long long, addr), (unsigned long long, compare),
                (unsigned long long, exchange)) {
	int rv;
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(handle);
	struct nvmeibc_d_rdma_comp *dc    = NULL;
	struct corecomm_pd_op_ctx *op_ctx = NULL;

	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	/*All good, lets prepare the stuff*/
	op_ctx = __init_corecomm_pd_op_ctx(GFP_KERNEL, ctx, dinfo);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	dc           = &op_ctx->dc;
	dc->callback = corecomm_pd_lock_cb;
	dc->compare  = compare;
	dc->exchange = exchange;
	if ((rv = nvmeibc_pd_cmpxchg(dinfo->disk, dinfo->mem_info, addr, dc))) {
		printk(KERN_ALERT
		       "Call to nvmeibc_pd_cmpxchg failed synchronously %d\n",
		       rv);
		kfree(op_ctx);
		return rv;
	}
	return 0;
}

NLRPC_SRV_ASYNC(corecomm_pd_read_lock_, ctx, (cdisk_handle, handle),
                (unsigned long long, addr)) {
	int rv;
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(handle);
	struct nvmeibc_d_rdma_comp *dc    = NULL;
	struct corecomm_pd_op_ctx *op_ctx = NULL;

	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	/*All good, lets prepare the stuff*/
	op_ctx = __init_corecomm_pd_op_ctx(GFP_KERNEL, ctx, dinfo);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	dc           = &op_ctx->dc;
	dc->callback = corecomm_pd_lock_cb;
	if ((rv = nvmeibc_pd_read_lock(dinfo->disk, dinfo->mem_info, addr, dc))) {
		printk(KERN_ALERT
		       "Call to nvmeibc_pd_read_lock failed synchronously %d\n",
		       rv);
		kfree(op_ctx);
		return rv;
	}
	return 0;
}

NLRPC_SRV_ASYNC(corecomm_pd_write_blkset_info_, ctx, (cdisk_handle, handle),
                (unsigned long long, addr), (unsigned long long, bi)) {
	int rv;
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(handle);
	struct nvmeibc_d_rdma_comp *dc    = NULL;
	struct corecomm_pd_op_ctx *op_ctx = NULL;

	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	/*All good, lets prepare the stuff*/
	op_ctx = __init_corecomm_pd_op_ctx(GFP_KERNEL, ctx, dinfo);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	dc           = &op_ctx->dc;
	dc->lock.bi  = bi;
	dc->callback = corecomm_pd_lock_cb;
	if ((rv = nvmeibc_pd_write_blkset_info(dinfo->disk, dinfo->mem_info, addr,
	                                       dc))) {
		printk(KERN_ALERT
		       "Call to nvmeibc_pd_write_blkset_info failed synchronously %d\n",
		       rv);
		kfree(op_ctx);
		return rv;
	}
	return 0;
}

NLRPC_SRV_SYNC(corecomm_alloc_ndb_, ndb_handle, rsp, ctx,
               (unsigned int, n_pages)) {
	struct corecomm_ndb_info *info = ndb_store_add(&ctx->ndbs, n_pages);
	if (info) {
		*rsp = info->handle;
		return 0;
	}
	return -ENOMEM;
}

NLRPC_SRV_SYNC(corecomm_free_ndb_, int, rsp, ctx, (ndb_handle, ndb)) {
	ndb_store_delete(&ctx->ndbs, ndb);
	return 0;
}

NLRPC_SRV_SYNC(corecomm_stamp_ndb_, int, rsp, ctx, (ndb_handle, ndb),
               (data_stamps_arr_t, stamps_arr), (unsigned int, n_stamps),
               (unsigned int, offset_page), (unsigned int, offset_bytes)) {
	struct corecomm_ndb_info *info;
	if (!(info = ndb_store_get(&ctx->ndbs, ndb))) {
		printk(KERN_ALERT "Bad handle\n");
		return -EINVAL;
	}
	if (info && n_stamps + offset_page <= info->ndb.table.nents) {
		struct scatterlist *sg;
		unsigned int i;
		for_each_sg(info->ndb.table.sgl, sg, info->ndb.table.nents, i) {
			if (i > offset_page + n_stamps) break;
			if (i >= offset_page) {
				struct page *page = sg_page(sg);
				if (page) {
					unsigned long *virt =
					    ((void *)page_address(page) + offset_bytes);
					*virt               = stamps_arr[i - offset_page];
				} else {
					WARN(true, "WTF? Not page?\n");
				}
			}
		}
		return 0;
	} else {
		printk(KERN_ALERT "NDB outbound\n");
		return -EINVAL;
	}
}

NLRPC_SRV_SYNC(corecomm_stamp_ndb_md_, int, rsp, ctx, (ndb_handle, ndb),
               (data_stamps_arr_t, stamps_arr), (unsigned int, n_stamps),
               (unsigned int, offset_page)) {
	struct corecomm_ndb_info *info;
	if (!(info = ndb_store_get(&ctx->ndbs, ndb))) {
		printk(KERN_ALERT "Bad handle\n");
		return -EINVAL;
	}
	if (sizeof(unsigned long) * (n_stamps + offset_page) >= PAGE_SIZE) {
		printk(KERN_ALERT "She said it is too big... %d %d\n", n_stamps,
		       offset_page);
		return -EINVAL;
	}
	memcpy(info->soft_md + offset_page, stamps_arr,
	       sizeof(unsigned long) * n_stamps);
	return 0;
}

NLRPC_SRV_SYNC(corecomm_read_stamp_ndb_, struct data_stamps_arr_container, rsp,
               ctx, (ndb_handle, ndb), (unsigned int, n_stamps),
               (unsigned int, offset_page), (unsigned int, offset_bytes)) {
	struct corecomm_ndb_info *info;
	if (!(info = ndb_store_get(&ctx->ndbs, ndb))) {
		printk(KERN_ALERT "Bad handle\n");
		return -EINVAL;
	}
	if (info && n_stamps + offset_page <= info->ndb.table.nents &&
	    n_stamps <= CORECOMM_MAX_DATA_STAMPS_ONE_OP) {
		struct scatterlist *sg;
		unsigned int i;

		for_each_sg(info->ndb.table.sgl, sg, info->ndb.table.nents, i) {
			if (i > offset_page + n_stamps) break;
			if (i >= offset_page) {
				struct page *page = sg_page(sg);
				if (page) {
					unsigned long *virt =
					    ((void *)page_address(page) + offset_bytes);
					rsp->data[i - offset_page] = *virt;
				} else {
					WARN(true, "WTF? No page?\n");
				}
			}
		}
		return 0;
	} else {
		printk(KERN_ALERT "NDB outbound\n");
		return -EINVAL;
	}
}

NLRPC_SRV_SYNC(corecomm_read_stamp_ndb_md_, struct data_stamps_arr_container,
               rsp, ctx, (ndb_handle, ndb), (unsigned int, n_stamps),
               (unsigned int, offset_page)) {
	struct corecomm_ndb_info *info;
	if (!(info = ndb_store_get(&ctx->ndbs, ndb))) {
		printk(KERN_ALERT "Bad handle\n");
		return -EINVAL;
	}
	if (info && sizeof(unsigned long) * (n_stamps + offset_page) <= PAGE_SIZE &&
	    n_stamps <= CORECOMM_MAX_DATA_STAMPS_ONE_OP) {
		memcpy(rsp->data, info->soft_md, sizeof(unsigned long) * n_stamps);
		return 0;
	} else {
		printk(KERN_ALERT "NDB outbound\n");
		return -EINVAL;
	}
}

NLRPC_SRV_SYNC(corecomm_direct_read_lock_, struct lock_data, ld, ctx,
               (name_t, disk_name), (unsigned long long, addr)) {
	struct nvmeibs_disk_info *di = NULL;
	struct list_head *disk_info_list;
	ld->status = -1;
	ld->value  = -1;

	if (!ctx->sif.is_initialized) {
		printk(KERN_ALERT
		       "Could not resolve nvmeibs_if, perhaps nvmeibs is down?\n");
		return -EAFNOSUPPORT;
	}

	disk_info_list = ctx->sif.disk_get_disks(NULL);

	list_for_each_entry(di, disk_info_list, link) {
		if (!strncmp(di->disk_id, disk_name, sizeof(di->disk_id))) {
			/*Found the di*/
			struct nvmeibs_disk_lock_mem_info *disk_mem_info;
			struct nvmeibs_disk_private_data *pd = di->priv;
			if (!pd) {
				WARN(true, "No disk private data, WTF? %s\n", disk_name);
				ctx->sif.disk_put_disks();
				return -ENOENT;
			}
			list_for_each_entry(disk_mem_info, &pd->lock_mems, link) {
				if (disk_mem_info->seg_id == NVMEIBS_DEFAULT_DISK_SEG_ID) {
					/* For now always read via defult segment later idk */
					const unsigned long long locks_per_page =
					    PAGE_SIZE / NVMEIB_LOCK_BLKSET_ENTRY_SIZE;
					const unsigned long long lock_addr =
					    addr / disk_mem_info->lock_set_size;
					if (addr < disk_mem_info->start_addr ||
					    addr >= disk_mem_info->len * NVMEIBC_SECTOR_SIZE) {
						return -EINVAL;
					}
					ld->status = 0; /* Direct read */
					ld->value =
					    ((unsigned long long **)
					         disk_mem_info->virt)[lock_addr / locks_per_page]
					                             [(lock_addr % locks_per_page)];
					ctx->sif.disk_put_disks();
					return 0;
				}
			}
			/* Should not be here */
			WARN(true, "Could not find the default segment for disk %s\n",
			     disk_name);
			ctx->sif.disk_put_disks();
			return -ENOENT;
		}
	}
	ctx->sif.disk_put_disks();
	return -ENOENT;
}

#define n_bits_blkset_problem (sizeof(union nvmeib_blkset_problem_report) * 8)
#define n_bits_blkset_stale_s (sizeof(union nvmeib_blkset_sparse_report) * 8)

static int corecomm_pd_get_blkset_problems_cb(struct nvmeibc_d_rdma_comp *dc) {
	struct corecomm_pd_op_ctx *op_ctx =
	    container_of(dc, struct corecomm_pd_op_ctx, dc);
	struct corecomm_connection_ctx *ctx = op_ctx->ctx;
	void *arr                           = dc->dbits_arr.arr;
	const u64 n_elem                    = dc->dbits_arr.size;
	const int elem_size = dc->dbits_arr_req.reserved ? n_bits_blkset_stale_s
	                                                 : n_bits_blkset_problem;
	const u64 num_bytes = DIV_ROUND_UP((n_elem * elem_size), 8);

	printk(KERN_INFO "Inside corecomm_pd_get_blkset_problems_cb\n");

	/* Goodbye pausable layer, we are done */
	nvmeibc_pd_cb_called_comp(op_ctx->dinfo->disk, dc);

	nlrpc_reply_buf(arr, num_bytes, ctx, GFP_ATOMIC);

	kfree(op_ctx);

	return 0;
}

NLRPC_SRV_ASYNC(corecomm_pd_get_blkset_problems_, ctx, (cdisk_handle, cdisk),
                (unsigned long long, start), (unsigned long long, len),
                (int, get_dbits), (int, get_stales)) {
	int rv;
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	struct nvmeibc_d_rdma_comp *dc    = NULL;
	struct corecomm_pd_op_ctx *op_ctx = NULL;

	if (len > CORECOMM_MAX_DATA_STAMPS_ONE_OP) {
		printk(KERN_ALERT "Too much, lower your ambitions\n");
		return -EINVAL;
	}

	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	op_ctx = __init_corecomm_pd_op_ctx(GFP_KERNEL, ctx, dinfo);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	dc           = &op_ctx->dc;
	dc->callback = corecomm_pd_get_blkset_problems_cb;

	/* Emulate block beahaviour here. We use reserved field here to save report
	 * element size that may be either 16 or 64 bits. In real code, reserved is
	 * not used, instead this data is saved in block command that we don't have
	 * here. */
	if (!get_dbits && get_stales) { dc->dbits_arr_req.reserved = 1; }
	dc->dbits_arr_req.get_dbits    = get_dbits;
	dc->dbits_arr_req.get_stales   = get_stales;
	dc->dbits_arr_req.get_full_val = 1;

	if ((rv = nvmeibc_pd_get_blkset_problems(dinfo->disk, dinfo->mem_info,
	                                         start, len, dc))) {
		printk(
		    KERN_ALERT
		    "Call to nvmeibc_pd_get_blkset_problems failed synchronously %d\n",
		    rv);
		kfree(op_ctx);
		return rv;
	}
	return 0;
}

static void
corecomm_pd_free_jrnl_ents_cb_(struct nvmeibc_disk_free_jrnl_ents_comp *comp) {
	struct corecomm_pd_op_ctx *op_ctx =
	    container_of(comp, struct corecomm_pd_op_ctx, free_ents_comp);
	struct corecomm_connection_ctx *ctx = op_ctx->ctx;
	int rsp = comp->status == NCL_STATUS_TAKEN ? 0 : comp->status;

	printk(KERN_INFO "Inside corecomm_pd_get_blkset_problems_cb\n");

	/* Goodbye pausable layer, we are done */
	nvmeibc_pd_cb_called_free_jrnl_ents(op_ctx->dinfo->disk, comp);

	nlrpc_reply(rsp, ctx, GFP_ATOMIC);

	nvmeib_release(&comp->ents_ai, comp->ents, NULL);
	kfree(op_ctx);
}

NLRPC_SRV_ASYNC(corecomm_pd_free_jrnl_ents_, ctx, (cdisk_handle, cdisk),
                (name_t, seg_uuid), (unsigned long long, start_blkset_lba),
                (unsigned long long, len_blksets), (int, pass2toma),
                (unsigned long long, lock_entry_raw), (name_t, serjio_boot_id),
                (unsigned int, jrange), (unsigned int, jentry),
                (unsigned char, jentry_gen_id), (int, num_ents)) {
	/* Currently, this API supports only num_ents = 1, same as real code
	   However, server side api is not limited. TODO: maybe add this
	   functionality if we need it at all.
	 */
	int rv;
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	struct nvmeibc_disk_free_jrnl_ents_comp *free_ents_comp = NULL;
	struct corecomm_pd_op_ctx *op_ctx                       = NULL;

	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	op_ctx = __init_corecomm_pd_op_ctx(GFP_KERNEL, ctx, dinfo);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	free_ents_comp          = &op_ctx->free_ents_comp;
	free_ents_comp->gen_cmd = &op_ctx->gen_cmd;

	free_ents_comp->disk        = dinfo->disk;
	free_ents_comp->recov_src   = NVMEIB_RECOV_SRC_HTR;
	free_ents_comp->blkset_slba = start_blkset_lba;
	free_ents_comp->pass2toma   = pass2toma;
	free_ents_comp->blkset_num  = len_blksets;
	free_ents_comp->lock_ent    = lock_entry_raw;
	free_ents_comp->num_ents    = num_ents;
	memcpy(free_ents_comp->seg_uuid, seg_uuid, NVMEIB_GID_STR_MAX);
	memcpy(free_ents_comp->serjio_boot_id, serjio_boot_id, NVMEIB_GID_STR_MAX);

	free_ents_comp->ents = nvmeib_alloc(
	    &free_ents_comp->ents_ai,
	    free_ents_comp->num_ents * sizeof(struct nvmeib_free_ents_data), NULL);
	if (!free_ents_comp->ents) {
		kfree(op_ctx);
		return -ENOMEM;
	}
	free_ents_comp->ents[0].rng_idx    = jrange;
	free_ents_comp->ents[0].rng_gen_id = dinfo->disk->jour.rng_gen_id;
	free_ents_comp->ents[0].ent_idx    = jentry;
	free_ents_comp->ents[0].ent_md = (struct nvmeib_jrnl_ent_md){jentry_gen_id};
	free_ents_comp->ents_enc_buf = nvmeib_alloc(&free_ents_comp->ents_enc_ai,
						    free_ents_comp->num_ents * sizeof(struct wire_free_ents_entry), NULL);
	if (!free_ents_comp->ents_enc_buf) {
		nvmeib_release(&free_ents_comp->ents_ai, free_ents_comp->ents, NULL);
		kfree(op_ctx);
		return -ENOMEM;
	}
	free_ents_comp->ents_enc_buf_sz = free_ents_comp->ents_enc_ai.n << PAGE_SHIFT;
	free_ents_comp->callback       = corecomm_pd_free_jrnl_ents_cb_;

	if ((rv = nvmeibc_pd_free_jrnl_ents(dinfo->disk, free_ents_comp))) {
		printk(KERN_ALERT
		       "Call to nvmeibc_pd_free_jrnl_ents failed synchronously %d\n",
		       rv);
		if (op_ctx && free_ents_comp->ents)
			nvmeib_release(&free_ents_comp->ents_ai,
			               free_ents_comp->ents, NULL);
		kfree(op_ctx);
		return rv;
	}
	return 0;
}

/**
 * Context of a jmdc related gen command (cold recovery/jgc originated)
 */
struct corecomm_pd_jmdc_op_ctx {
	struct corecomm_connection_ctx *ctx;
	struct corecomm_jmdc_container
	    *scratch; /* Very inefficient, preallocate buffer for response. @TODO:
	                 Do something about it.*/
	corecomm_userspace_ptr output;
	struct corecomm_cdisk_info *dinfo;

	struct { /* Based on struct nvmeibc_disk_jcmd from cold recovery */
		struct nvmeib_jmdc_read_jrnl_data jrnl_desc;
		struct {
			struct nvmeib_get_jmdc_rng_data *arr;
			struct nvmeib_alloc_info _ai;
			size_t len;
		} rng;
		struct {
			struct nvmeib_jrnl_ent_md *arr;
			struct nvmeib_alloc_info _ai;
			size_t len;
		} ent_md;
		struct {
			union jblock_md *arr;
			struct nvmeib_alloc_info _ai;
			size_t len;
		} md;
		struct nvmeibc_disk_jmdc_read_comp comp;
	} cmd;
};

static void
__free_corecomm_pd_jmdc_op_ctx(struct corecomm_pd_jmdc_op_ctx *op_ctx) {
	if (op_ctx) {
		if (op_ctx->cmd.rng.arr)
			nvmeib_release(&op_ctx->cmd.rng._ai, op_ctx->cmd.rng.arr, NULL);
		if (op_ctx->cmd.md.arr)
			nvmeib_release(&op_ctx->cmd.md._ai, op_ctx->cmd.md.arr, NULL);
		if (op_ctx->cmd.ent_md.arr)
			nvmeib_release(&op_ctx->cmd.ent_md._ai,
			               op_ctx->cmd.ent_md.arr), NULL;
		kvfree(op_ctx->scratch);
		kfree(op_ctx);
	}
}

static struct corecomm_pd_jmdc_op_ctx *__init_corecomm_pd_jmdc_op_ctx(
    gfp_t gfp_flags, struct corecomm_connection_ctx *ctx,
    struct corecomm_cdisk_info *dinfo, unsigned int num_rng,
    corecomm_userspace_ptr output) {
	struct corecomm_pd_jmdc_op_ctx *op_ctx =
	    kzalloc(sizeof(struct corecomm_pd_jmdc_op_ctx), gfp_flags);
	if (!op_ctx) return NULL;
	op_ctx->scratch = vmalloc(sizeof(struct corecomm_jmdc_container));
	if (!op_ctx->scratch) { kfree(op_ctx); return NULL; }
	op_ctx->dinfo         = dinfo;
	op_ctx->ctx           = ctx;
	op_ctx->output        = output;
	op_ctx->cmd.comp.disk = dinfo->disk;
	/* How can one not love such code? */
	op_ctx->cmd.rng.len = sizeof(*op_ctx->cmd.rng.arr) * num_rng;
	op_ctx->cmd.rng.arr =
	    nvmeib_alloc(&op_ctx->cmd.rng._ai, op_ctx->cmd.rng.len, NULL);
	op_ctx->cmd.ent_md.len = sizeof(*op_ctx->cmd.ent_md.arr) * num_rng *
	                         NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
	op_ctx->cmd.ent_md.arr =
	    nvmeib_alloc(&op_ctx->cmd.ent_md._ai, op_ctx->cmd.ent_md.len, NULL);
	op_ctx->cmd.md.len = sizeof(*op_ctx->cmd.md.arr) * num_rng *
	                     NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE;
	op_ctx->cmd.md.arr =
	    nvmeib_alloc(&op_ctx->cmd.md._ai, op_ctx->cmd.md.len, NULL);

	op_ctx->cmd.comp.rsp.read_jrnl_data = &op_ctx->cmd.jrnl_desc;
	op_ctx->cmd.comp.rng_data_ai        = &op_ctx->cmd.rng._ai;
	op_ctx->cmd.comp.rsp.rng_data       = op_ctx->cmd.rng.arr;
	op_ctx->cmd.comp.rsp.rng_data_len   = &op_ctx->cmd.rng.len;
	op_ctx->cmd.comp.ent_md_ai          = &op_ctx->cmd.ent_md._ai;
	op_ctx->cmd.comp.rsp.ent_md         = op_ctx->cmd.ent_md.arr;
	op_ctx->cmd.comp.rsp.ent_md_len     = &op_ctx->cmd.ent_md.len;
	op_ctx->cmd.comp.jmdc_ent_ai        = &op_ctx->cmd.md._ai;
	op_ctx->cmd.comp.rsp.jmdc_ent       = op_ctx->cmd.md.arr;
	op_ctx->cmd.comp.rsp.jmdc_ent_len   = &op_ctx->cmd.md.len;
	return op_ctx;
}

static void
corecomm_pd_jmdc_read_cb_(struct nvmeibc_disk_jmdc_read_comp *comp) {
	struct corecomm_pd_jmdc_op_ctx *op_ctx =
	    container_of(comp, struct corecomm_pd_jmdc_op_ctx, cmd.comp);
	struct corecomm_jmdc_container *dst =
	    (struct corecomm_jmdc_container *)op_ctx->scratch;
	int dummy = 777, i, j;

	BUILD_BUG_ON(sizeof(struct nvmeib_get_jmdc_rng_data) !=
	             sizeof(*dst->rng.arr));

	printk(KERN_INFO "Inside corecomm_pd_jmdc_read_cb\n");

	/* Goodbye pausable layer, we are done */
	nvmeibc_pd_cb_called_jmdc(op_ctx->dinfo->disk, comp);

	memcpy(dst->ent_md.arr, op_ctx->cmd.ent_md.arr,
	       min((int)sizeof(dst->ent_md.arr),
	           (int)(op_ctx->cmd.ent_md._ai.n * PAGE_SIZE)));
	memcpy(dst->rng.arr, op_ctx->cmd.rng.arr,
	       min((int)sizeof(dst->rng.arr),
	           (int)(op_ctx->cmd.rng._ai.n * PAGE_SIZE)));

	dst->ent_md.len = op_ctx->cmd.ent_md.len;
	dst->md.len = op_ctx->cmd.md.len;
	dst->rng.len = op_ctx->cmd.rng.len;

	for (i = 0;
	     i < op_ctx->cmd.ent_md.len / NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE;
	     ++i) {
		for (j = 0; j < NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE; ++j) {
			*(struct jblock_md_decompressed *)(&dst->md.arr[j]) =
			    nvmeibc_block_dp_ec_jmd_decode(
			        &op_ctx->cmd.md
			             .arr[i * NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE + j]);
		}
	}

	nvmeib_public_copy_user_pages(op_ctx->scratch, op_ctx->ctx->pid,
	                              (void *)op_ctx->output,
	                              sizeof(struct corecomm_jmdc_container), 1);

	nlrpc_reply(dummy, op_ctx->ctx, GFP_ATOMIC);

	__free_corecomm_pd_jmdc_op_ctx(op_ctx);
}

NLRPC_SRV_ASYNC(corecomm_pd_jmdc_read_, ctx, (cdisk_handle, cdisk),
                (unsigned int, start_rng), (unsigned int, num_rng),
                (int, dirty_only), (corecomm_userspace_ptr, output)) {
	int rv;
	struct corecomm_cdisk_info *dinfo      = cdisk_store_get(cdisk);
	struct corecomm_pd_jmdc_op_ctx *op_ctx = NULL;

	/* @TODO: Input corectness check, to avoid buffers overflow */

	if (!dinfo) {
		printk(KERN_ALERT "Error, bad handle\n");
		return -ESTALE;
	}
	op_ctx = __init_corecomm_pd_jmdc_op_ctx(GFP_KERNEL, ctx, dinfo, num_rng, output);
	if (!op_ctx) {
		printk(KERN_ALERT "Error, out of memory, can't allocate ctx\n");
		return -ENOMEM;
	}
	op_ctx->cmd.comp.callback  = corecomm_pd_jmdc_read_cb_;
	op_ctx->cmd.comp.start_rng = start_rng;
	op_ctx->cmd.comp.num_rng   = num_rng;
	op_ctx->cmd.comp.recov_src =
	    NVMEIB_RECOV_SRC_COLD; /*@TODO: Is there any poin in testing other
	                              values?*/
	op_ctx->cmd.comp.dirty_only = dirty_only;

	if ((rv = nvmeibc_pd_jmdc_read(dinfo->disk, &op_ctx->cmd.comp))) {
		printk(
		    KERN_ALERT
		    "Call to nvmeibc_pd_get_blkset_problems failed synchronously %d\n",
		    rv);
		kfree(op_ctx);
		return rv;
	}
	return 0;
}

/* IO relies on the existance of a HUGE amount of data structures. For
 * corecomm hey are all dummies. This struct holds them all in one place,
 * serving 2 purpuses:
 * 1. Set IO callback
 * 2. Prevent some rogue NULL pointer dereference from some third degree
 * debug print.
 */
struct corecomm_io_ctx {
	struct corecomm_connection_ctx *ctx;
	struct nvmeibc_disk_io_command dcmd;
	struct nvmeibc_block_command bcmd;
	struct operation o;
	struct nvmeibc_block_device nd;
	struct nvmeibc_disk_segment ds;

	struct corecomm_ndb_info *ninfo;
	struct nlmsghdr *nlh;
	int pid;
};

/** @note: Can only fail if ENOMEM */
static struct corecomm_io_ctx *
__init_corecomm_io_ctx(gfp_t gfp, struct nvmeibc_disk *disk,
                       void (*cb)(struct nvmeibc_d_iocmd_comp *),
                       struct corecomm_connection_ctx *conn_ctx) {
	struct corecomm_io_ctx *ctx = kzalloc(sizeof(*ctx), gfp);
	if (ctx) {
		ctx->dcmd.reqs           = &ctx->dcmd.reqs1;
		ctx->dcmd.comp.cmd       = &ctx->bcmd;
		ctx->dcmd.disk_cmd.owner = &ctx->bcmd;
		ctx->bcmd.o              = &ctx->o;
		ctx->bcmd.cmdarr         = &ctx->bcmd;
		ctx->bcmd.iocmd          = &ctx->dcmd;
		ctx->bcmd.ds             = &ctx->ds;
		ctx->bcmd.ncmds          = 1;
		ctx->o.nd                = &ctx->nd;

		ctx->ctx               = conn_ctx;
		ctx->nd.dp.cmd_comp_cb = cb;
		ctx->ds.disk           = disk;
		ctx->nd.dp.sync_execute_op =
		    (void *)ctx; /* Hack, save self pointer here - this field is unused
		                    anyway */

		atomic_set(&ctx->bcmd.n_uncompleted_cmds, 1);
	}
	return ctx;
}

static void corecomm_nvmeibc_pd_io_cb_(struct nvmeibc_d_iocmd_comp *comp) {
	struct corecomm_io_ctx *io_ctx =
	    (void *)comp->cmd->o->nd->dp.sync_execute_op;
	struct lock_data rsp;
	printk(KERN_INFO "IO completion: %d\n", comp->comp_code);
	if (io_ctx->ninfo) {
		const unsigned int sw_md = DISK_SW_MD_SIZE(io_ctx->ds.disk);
		if (sw_md) {
			unsigned int i = 0;
			/*Convert physical md to soft md*/
			for (i = 0; i < PAGE_SIZE / sw_md; ++i)
				io_ctx->ninfo->soft_md[i] =
				    *((unsigned long *)(io_ctx->ninfo->md + i * sw_md));
		}
	}

	rsp.io_status = comp->comp_code;
	rsp.status =    comp->pigbck_comp.lock_status;
	rsp.value =
	    comp->pigbck_comp.lock.id | (comp->pigbck_comp.lock.bi << 32);

	printk(KERN_INFO "DEBUG status %d v 0x%llx\n", rsp.status, rsp.value);

	/* This may be an interrupt context */
	nlrpc_reply(rsp, io_ctx->ctx, GFP_ATOMIC);

	kfree(io_ctx);
}

/* Return type: struct lock_data, asynchronously from callback */
NLRPC_SRV_ASYNC(corecomm_nvmeibc_pd_io_, ctx, (cdisk_handle, cdisk),
                (ndb_handle, ndb), (unsigned long long, addr),
                (unsigned long long, len), (enum io_type, io_type),
                (int, block /*block = 1, journal = 0*/), (int, has_piggy_back),
                (unsigned long long, pb_addr), (int, sub_block)) {
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	struct corecomm_ndb_info *ninfo   = ndb_store_get(&ctx->ndbs, ndb);
	struct corecomm_io_ctx *io_ctx;
	int rv;

	if (!dinfo) {
		printk(KERN_ALERT "Bad disk\n");
		return -EINVAL;
	}

	if ((!ninfo && io_type != CORECOMM_IO_DISCARD)) {
		printk(KERN_ALERT "Bad NDB\n");
		return -EINVAL;
	}

	if (sub_block > 7) {
		printk(KERN_ALERT "Bad subblock\n");
		return -EINVAL;
	}

	if (!(io_ctx = __init_corecomm_io_ctx(GFP_KERNEL, dinfo->disk,
	                                      corecomm_nvmeibc_pd_io_cb_, ctx))) {
		return -ENOMEM;
	}

	io_ctx->ctx = ctx;

	io_ctx->dcmd.reqs->op           = io_type;
	io_ctx->dcmd.reqs->disk_address = addr;
	io_ctx->dcmd.reqs->ndb          = ninfo ? &ninfo->ndb : NULL;
	io_ctx->ninfo                   = ninfo;

	if (ninfo) {
		const unsigned int sw_md = DISK_SW_MD_SIZE(dinfo->disk);
		/* Prepare NDB for reuse */
		struct nvmeib_data_buffer bak = ninfo->ndb;
		memset(&ninfo->ndb, 0, sizeof(ninfo->ndb));
		ninfo->ndb.length   = bak.length;
		ninfo->ndb.table    = bak.table;
		ninfo->ndb.table.nents = bak.table.nents;
		if (sw_md) {
			unsigned int i = 0;
			/*Convert soft md to physical md*/
			for (i = 0; i < PAGE_SIZE / sw_md; ++i)
				*((unsigned long *)(ninfo->md + i * sw_md)) = ninfo->soft_md[i];
			io_ctx->dcmd.reqs->md = ninfo->md;
		}
		if (sub_block >= 0) {
			ninfo->ndb.length = ninfo->ndb.table.sgl->length = 512;
			ninfo->ndb.table.sgl->offset += sub_block*512; /*TODO: Not needed*/
			io_ctx->dcmd.reqs->do_512b_sub_block_x = sub_block ^ 0x20;
		}
	}

	if (has_piggy_back && io_type == CORECOMM_IO_READ) {
		struct nvmeibc_d_rdma_comp *dc = &io_ctx->dcmd.comp.pigbck_comp;
		printk(KERN_INFO "Request includes pb\n");

		dp_cmds_add_generic_piggyback(io_ctx->dcmd);
		io_ctx->dcmd.lpb.handle = dinfo->mem_info;
		io_ctx->dcmd.lpb.addr   = pb_addr;
		dc->lock_cnsts          = &global_lock_entry_constants;
		dc->opr                 = NVMEIBC_LOCK_READ;
	}

	if (block) {
		if ((rv = nvmeibc_pd_execute_io_blocks(dinfo->disk, &io_ctx->dcmd))) {
			/* There will be no callback, return synchronously */
			kfree(io_ctx);
			return rv;
		}
	} else {
		if ((rv = nvmeibc_pd_execute_io_jour_blocks(dinfo->disk, &io_ctx->dcmd))) {
			/* There will be no callback, return synchronously */
			kfree(io_ctx);
			return rv;
		}
	}

	printk(KERN_INFO "IO request sent\n");
	return 0;
}

/** Context for gen cmds */
struct corecomm_gen_ctx {
	struct corecomm_io_ctx *io_ctx;
	struct recovery_sync_op so;
	struct nvmeibc_raid1 r1;
	struct nvmeibc_disk_gen_cmd gen_cmd;
};

void __free_corecomm_gen_ctx(struct corecomm_gen_ctx *ctx) {
	if (ctx) {
		kfree(ctx->io_ctx);
		kfree(ctx);
	}
}

/** @note: Can only fail if ENOMEM
 */
static struct corecomm_gen_ctx *
__init_corecomm_gen_ctx(gfp_t gfp, struct nvmeibc_disk *disk,
                        void (*cb)(struct nvmeibc_d_iocmd_comp *),
                        struct corecomm_connection_ctx *conn_ctx) {
	struct corecomm_gen_ctx *ctx = kzalloc(sizeof(*ctx), gfp);
	if (!ctx) goto err;
	if (!(ctx->io_ctx = __init_corecomm_io_ctx(gfp, disk, cb, conn_ctx)))
		goto err;
	ctx->gen_cmd.disk_cmd.cmd_type     = NVMEIBC_DISK_CMD_GEN;
	ctx->gen_cmd.disk                  = disk;
	ctx->gen_cmd.disk_cmd.owner        = &ctx->io_ctx->dcmd;
	ctx->io_ctx->bcmd.gen_cmd          = &ctx->gen_cmd;
	ctx->gen_cmd.jiffies_start         = jiffies;
	ctx->gen_cmd.timeout               = HZ;
	ctx->so.r1                         = &ctx->r1;
	ctx->io_ctx->nd.dp.sync_execute_op = (void *)ctx;
	return ctx;
err:
	__free_corecomm_gen_ctx(ctx);
	return NULL;
}

static void
corecomm_gen_blkset_recovered_cb_(struct nvmeibc_d_iocmd_comp *comp) {
	struct corecomm_gen_ctx *gen_ctx =
	    (void *)comp->cmd->o->nd->dp.sync_execute_op;
	const int rsp = gen_ctx->gen_cmd.comp_code;
	printk(KERN_INFO "In corecomm_gen_blkset_recovered_cb_, comp_code=%d\n",
	       rsp);
	if (!rsp)
		nlrpc_reply(rsp, gen_ctx->io_ctx->ctx, GFP_ATOMIC);
	else
		nlrpc_error(rsp, gen_ctx->io_ctx->ctx, GFP_ATOMIC);

	__free_corecomm_gen_ctx(gen_ctx);
}

NLRPC_SRV_ASYNC(corecomm_gen_blkset_recovered_, ctx, (cdisk_handle, cdisk),
                (name_t, client_uuid), (name_t, sgmnt_uuid),
                (unsigned int, slice_size), (unsigned long, jrange),
                (unsigned long, jentry), (int, pass2toma)) {
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	struct corecomm_gen_ctx *gen_ctx;

	int rv = 0;

	if (!dinfo) { return -EINVAL; }

	if (!(gen_ctx = __init_corecomm_gen_ctx(GFP_KERNEL, dinfo->disk,
	                                        corecomm_gen_blkset_recovered_cb_,
	                                        ctx))) {
		printk(KERN_ALERT "Out of memory while initializing gen_ctx\n");
		return -ENOMEM;
	}

	/*Ughhh....*/
	gen_ctx->r1.slice_size = slice_size;
	__corecomm_uuid_parse(&gen_ctx->so.recoveree_cuuid, client_uuid, true);
	BUILD_BUG_ON(ARRAY_MEM_SIZE(gen_ctx->io_ctx->ds.uuid) != sizeof(name_t));
	memcpy(gen_ctx->io_ctx->ds.uuid, sgmnt_uuid, sizeof(name_t));

	nvmeibcbdpec_fill_blockset_recovered_info(&gen_ctx->io_ctx->bcmd, HZ,
	                                          gen_ctx, jentry, &gen_ctx->so,
	                                          jrange, jentry, pass2toma);

	if ((rv = nvmeibc_pd_execute_gen(dinfo->disk, &gen_ctx->gen_cmd))) {
		printk(KERN_ALERT "Error calling gen command %d\n", rv);
		__free_corecomm_gen_ctx(gen_ctx);
	}

	return rv;
}

/* Here is the problem with cmds by jam - they have no callback.
   So, can't destinguish between real jam and corecomm issued command.
   So this trick solves it - if req-id == magic => it is corecomm.
*/
#define CORECOMM_JAM_CMD_MAGIC 0x440a350f

void corecomm_gen_jentry_erase_cb_(struct nvmeibc_disk_gen_cmd *gen_cmd) {
	/* We could arrive here from a command sent by jam or from a command by
	   unitest. In case it was jam - have to make sure not to handle it but
	   forward to jam. Else - handle. */
	if (gen_cmd->req_id == CORECOMM_JAM_CMD_MAGIC) {
		struct corecomm_gen_ctx *gen_ctx =
		    container_of(gen_cmd, struct corecomm_gen_ctx, gen_cmd);
		int rsp = gen_cmd->comp_code;
		printk(KERN_INFO "In corecomm_gen_jentry_erase_cb_, comp_code=%d\n",
		       rsp);
		nvmeibc_pd_cb_called_cmd(gen_ctx->gen_cmd.disk,
		                         &gen_cmd->disk_cmd); /* Complete pd */
		if (!rsp)
			nlrpc_reply(rsp, gen_ctx->io_ctx->ctx, GFP_ATOMIC);
		else
			nlrpc_error(rsp, gen_ctx->io_ctx->ctx, GFP_ATOMIC);

		__free_corecomm_gen_ctx(gen_ctx);
	} else {
		/* Let the jam handle */
		nvmeibc_jam_jentry_erase_comp(gen_cmd);
	}
}

NLRPC_SRV_ASYNC(corecomm_gen_jentry_erase_, ctx, (cdisk_handle, cdisk),
                (unsigned short, jentry), (unsigned char, jentry_gen_id)) {
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	struct corecomm_gen_ctx *gen_ctx;

	int rv = 0;

	if (!dinfo) { return -EINVAL; }

	if (!(gen_ctx = __init_corecomm_gen_ctx(
	          GFP_KERNEL, dinfo->disk, (void *)corecomm_gen_jentry_erase_cb_,
	          ctx))) {
		printk(KERN_ALERT "Out of memory while initializing gen_ctx\n");
		return -ENOMEM;
	}

	/* Copied from JAM */
	gen_ctx->gen_cmd.disk_cmd.cmd_type          = NVMEIBC_DISK_CMD_GEN;
	gen_ctx->gen_cmd.disk_cmd.server_side_only  = true;
	gen_ctx->gen_cmd.opcode                     = NVMEIB_GEN_OP_JENTRY_ERASE;
	gen_ctx->gen_cmd.cpu_mask                   = (struct nvmeib_cpu_mask){0};
	gen_ctx->gen_cmd.param.je.rng_gen_id        = dinfo->disk->jour.rng_gen_id;
	gen_ctx->gen_cmd.param.je.rng_idx           = dinfo->disk->jour.rng_id;
	gen_ctx->gen_cmd.param.je.ent_erase.ent_idx = jentry;
	gen_ctx->gen_cmd.param.je.ent_erase.ent_md.ent_gen_id = jentry_gen_id;
	gen_ctx->gen_cmd.param.je.ent_erase.ent_swlba =
	    dinfo->disk->jour.rng_slba + jentry;
	gen_ctx->gen_cmd.req_id = CORECOMM_JAM_CMD_MAGIC;

	if ((rv = nvmeibc_pd_execute_gen(dinfo->disk, &gen_ctx->gen_cmd))) {
		printk(KERN_ALERT "Error calling gen command %d\n", rv);
		__free_corecomm_gen_ctx(gen_ctx);
	}

	return rv;
}

void __free_corecomm_htr_jmdc_ctx(struct corecomm_gen_ctx *ctx) {
	if (ctx) {
		kfree(ctx->gen_cmd.param.uj.jmdc_dest.local.ptr);
		kfree(ctx->gen_cmd.param.uj.ent_md_dest.local.ptr);
		__free_corecomm_gen_ctx(ctx);
	}
}

static struct corecomm_gen_ctx *
__init_corecomm_htr_jmdc_ctx(gfp_t gfp, struct nvmeibc_disk *disk,
                             void (*cb)(struct nvmeibc_d_iocmd_comp *),
                             struct corecomm_connection_ctx *conn_ctx) {
	struct corecomm_gen_ctx *ctx =
	    __init_corecomm_gen_ctx(gfp, disk, cb, conn_ctx);
	const int jmdc_sz =
	    CORECOMM_JOURNAL_ENTRIES_PER_RANGE * sizeof(union jblock_md);
	const int ent_md_sz =
	    CORECOMM_JOURNAL_ENTRIES_PER_RANGE * sizeof(struct nvmeib_jrnl_ent_md);
	void *md, *ent_md;
	if (!ctx) goto err;

	md     = kzalloc(jmdc_sz, gfp);
	ent_md = kzalloc(ent_md_sz, gfp);

	if (!md || !ent_md) goto err;

	nvmeib_buffer_init_one(&ctx->gen_cmd.param.uj.jmdc_dest.local, md, jmdc_sz);
	nvmeib_buffer_init_one(&ctx->gen_cmd.param.uj.ent_md_dest.local, ent_md, ent_md_sz);

	ctx->gen_cmd.n_data_sink  = 2;
	ctx->gen_cmd.data_sink[0] = &ctx->gen_cmd.param.uj.jmdc_dest;
	ctx->gen_cmd.data_sink[1] = &ctx->gen_cmd.param.uj.ent_md_dest;

	return ctx;
err:
	__free_corecomm_htr_jmdc_ctx(ctx);
	return NULL;
}

static void __cp_get_uuid_jour_rsp(void *_dst, void *_src, size_t size) {
	const union jblock_md *src = _src;
	struct jblock_md_decompressed *dst = _dst;
	int i;

	BUILD_BUG_ON(sizeof(struct jblock_md_decompressed) != sizeof(struct corecomm_jblock_md_decompressed));
	BUILD_BUG_ON(sizeof(struct corecomm_jmdc_range) > CORECOMM_SAFE_DATA_SIZE_ONE_OP);

	for (i = 0; i < CORECOMM_JOURNAL_ENTRIES_PER_RANGE; ++i) {
		dst[i] = nvmeibc_block_dp_ec_jmd_decode(&src[i]);
	}
}

static void corecomm_gen_get_uuid_jour_cb_(struct nvmeibc_d_iocmd_comp *comp) {
	struct corecomm_gen_ctx *jmdc_ctx =
	    (void *)comp->cmd->o->nd->dp.sync_execute_op;
	const int comp_code = jmdc_ctx->gen_cmd.comp_code;

	printk(KERN_INFO "In corecomm_gen_get_uuid_jour_cb_, comp_code=%d\n",
	       comp_code);

	if (!comp_code)
		__nlrpc_reply(NLRPC_REPLY,
						jmdc_ctx->gen_cmd.param.uj.jmdc_dest.local.ptr,
		                sizeof(struct corecomm_jmdc_range),
		                jmdc_ctx->io_ctx->ctx, __cp_get_uuid_jour_rsp, GFP_ATOMIC);
	else
		nlrpc_error(comp_code, jmdc_ctx->io_ctx->ctx, GFP_ATOMIC);

	__free_corecomm_htr_jmdc_ctx(jmdc_ctx);
}

NLRPC_SRV_ASYNC(corecomm_gen_get_uuid_jour_, ctx, (cdisk_handle, cdisk),
                (name_t, client_uuid), (name_t, sgmnt_uuid)) {
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);
	struct corecomm_gen_ctx *jmdc_ctx;
	int rv;

	if (!dinfo) { return -EINVAL; }

	if (!(jmdc_ctx = __init_corecomm_htr_jmdc_ctx(
	          GFP_KERNEL, dinfo->disk, corecomm_gen_get_uuid_jour_cb_, ctx))) {
		return -ENOMEM;
	}

	/* Segment UUID is string while client UUID is uuid_be. Love it! :)) */
	__corecomm_uuid_parse(&jmdc_ctx->gen_cmd.param.uj.client_uuid, client_uuid,
	                      true);
	BUILD_BUG_ON(ARRAY_MEM_SIZE(jmdc_ctx->gen_cmd.param.uj.sgmnt_uuid) !=
	             sizeof(name_t));
	memcpy(jmdc_ctx->gen_cmd.param.uj.sgmnt_uuid, sgmnt_uuid, sizeof(name_t));

	/* Opcode - who would gues this opcode means `read jmdc`? */
	jmdc_ctx->gen_cmd.opcode = NVMEIB_GEN_OP_GET_UUID_JOUR;
	jmdc_ctx->gen_cmd.cpu_mask = (struct nvmeib_cpu_mask){0};

	if ((rv = nvmeibc_pd_execute_gen(dinfo->disk, &jmdc_ctx->gen_cmd))) {
		printk(KERN_ALERT "Error calling gen command %d\n", rv);
		__free_corecomm_htr_jmdc_ctx(jmdc_ctx);
	}

	return rv; /*Not implemented yet*/
}

/*** JAM interface ***/

NLRPC_SRV_SYNC(corecomm_alloc_jrnls_, struct corecomm_lbas_set, rsp, ctx,
               (int, n_disks), (struct corecomm_disks_set, disks), (int, txid),
               (struct corecomm_lbas_set, dlbas)) {
	struct nvmeibc_disk *cdisks[ARRAY_SIZE(disks.disks)];
	static unsigned long priority = 1;
	int i;
	if (n_disks > ARRAY_SIZE(disks.disks)) {
		printk(KERN_ALERT "Error, not enough space for n_disks=%d\n", n_disks);
		return -ENOBUFS;
	}
	for (i = 0; i < n_disks; ++i) {
		struct corecomm_cdisk_info *dinfo = cdisk_store_get(disks.disks[i]);
		if (!dinfo || !dinfo->disk) {
			printk(KERN_ALERT "Error resolving disk handle #%d (handle = 0x%llu)\n", i, disks.disks[i]);
			return -EINVAL;
		}
		cdisks[i] = dinfo->disk;
	}
	return nvmeibc_jam_lbas_alloc(n_disks, cdisks, txid, dlbas.lbas, rsp->lbas,
	                              true, NULL, NULL, jiffies + HZ, priority++, NULL);
}

NLRPC_SRV_SYNC(corecomm_free_jrnls_, int, dummy, ctx, (int, n_disks),
               (struct corecomm_disks_set, disks),
               (struct corecomm_lbas_set, jlbas), (unsigned int, wr_sts_bm)) {
	struct nvmeibc_disk *cdisks[ARRAY_SIZE(disks.disks)];
	int i;
	if (n_disks > ARRAY_SIZE(disks.disks)) {
		printk(KERN_ALERT "Error, not enough space for n_disks=%d\n", n_disks);
		return -ENOBUFS;
	}
	for (i = 0; i < n_disks; ++i) {
		struct corecomm_cdisk_info *dinfo = cdisk_store_get(disks.disks[i]);
		if (!dinfo || !dinfo->disk) {
			printk(KERN_ALERT "Error resolving disk handle #%d (handle = 0x%llu)\n", i, disks.disks[i]);
			return -EINVAL;
		}
		cdisks[i] = dinfo->disk;
	}
	nvmeibc_jam_lbas_free(n_disks, cdisks, jlbas.lbas, wr_sts_bm);
	return 0;
}

NLRPC_SRV_SYNC(corecomm_jam_lba_2_idx_, int, output, ctx, (cdisk_handle, cdisk),
               (unsigned long long, lba)) {
	struct corecomm_cdisk_info *dinfo = cdisk_store_get(cdisk);

	if (!dinfo) { return -EINVAL; }

	*output = nvmeibc_jam_lba_2_idx(dinfo->disk, lba);

	return 0;
}

/* This is the main message handler - it receives a message from userspace,
 * and redirects it to an appropriate handler*/
NLRPC_SRV_DECLARE_MSG_HANDLER(nlrpc_input_handler, NLRPC_API_LIST);

/**
 * Init shall be called from module init
 */
int init_corecomm(void) {
	extern struct corecomm_inj_sym_store *corecomm_inj_default_store;

	struct netlink_kernel_cfg cfg = {
	    .input = nlrpc_input_handler,
	};

	printk(KERN_INFO "Corecomm init\n");

	global_nl_sk = netlink_kernel_create(&init_net, NETLINK_CORECOMM, &cfg);
	if (!global_nl_sk) {
		printk(KERN_ERR "Error creating socket.\n");
		return -1;
	}

	if (corecomm_connection_store_init()) {
		printk(KERN_ERR "Error initializing connection store.\n");
		netlink_kernel_release(global_nl_sk);
		return -1;
	}

	corecomm_inj_default_store = corecomm_inj_sym_store_create();

	return 0;
}

/**
 * Destroy shalle be called from module exit
 */
void destroy_corecomm(void) {
	/* @TODO: Add some locking here */
	struct sock *sk = global_nl_sk;
	if (sk) {
		extern struct corecomm_inj_sym_store *corecomm_inj_default_store;
		global_nl_sk = NULL;
		printk(KERN_INFO "Destroying corecomm\n");
		corecomm_connection_store_destroy();
		netlink_kernel_release(sk);
		cdisk_store_clean();
		__corecomm_destroy_target_cfg_store();
		corecomm_inj_sym_store_destroy(corecomm_inj_default_store);
	}
}
