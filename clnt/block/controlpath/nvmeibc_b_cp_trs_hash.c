/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block.h" // Must be first for simulator
#include "block/nvmeibc_block_common.h"
#include "nvmeibc_b_cp_trs_hash.h"
#include "nvmeibc_icore_ops.h"

#define TOPO_HASH_STARTING_VAL (0x1234) 	  /* unique key generator */
static DEFINE_SPINLOCK(topologies_rb_l);	  /* Lock on access to hash table */
static struct rb_root topologies_rb = {NULL,}; /* RB_ROOT; might not compile */
static u64 topologies_id = TOPO_HASH_STARTING_VAL;
static int topologies_hash_size = 0;

#ifdef DEBUG_TOMA_REG_LEAKS
static atomic_t n_toma_regs = ATOMIC_INIT(0);
#endif

/* Print the hashtable of tr's, recursive implementation (can print sub trees)*/
static int nvmeibc_trs_hash_tostring_rec(char *buf, int len, struct rb_node *rb)
{
	#define BUF_ADD(...) pos += scnprintf(buf + pos, len - pos, __VA_ARGS__)
	int pos = 0;
	if (!rb)
		return pos;
	pos += nvmeibc_trs_hash_tostring_rec(buf + pos, len - pos, rb->rb_left);
	{	/* Print in order: coz tr->handle is always accending */
		const struct nvmeibc_subscription_ctx *tr = rb_entry(rb, struct nvmeibc_subscription_ctx, rb);
		const char *dev_name = (!is_toma_reg_already_dead(tr)) ?
			tr->nt->device_name : "Zombie";
		BUF_ADD("0x%-14llx- %s(%d,%d,%d), disk %s [%llu..%llu]\n",
			tr->handle, dev_name, tr->ch, tr->r1, tr->seg,
			((tr->disk)->base.ops.get_name(&((tr->disk))->base)), tr->first_lba, tr->first_lba+tr->length-1);
	}
	pos += nvmeibc_trs_hash_tostring_rec(buf + pos, len - pos, rb->rb_right);
	return pos;
}

int nvmeibc_block_layer_all_tostring(char *buf, int len)
{
	unsigned long flags;
	int pos = 0;
	spin_lock_irqsave(&topologies_rb_l, flags);
	BUF_ADD("Subscribed segs: current=%d, total=%lld\n",
			topologies_hash_size, (topologies_id-TOPO_HASH_STARTING_VAL));
	pos += nvmeibc_trs_hash_tostring_rec(buf + pos, len - pos,
										 topologies_rb.rb_node);
	spin_unlock_irqrestore(&topologies_rb_l, flags);
	return pos;
	#undef BUF_ADD
}

#define nvmeibc_trs_hash_is_inserted(tr)  ((tr)->handle != 0)
#define nvmeibc_trs_hash_was_removed(tr)  (RB_EMPTY_NODE(&tr->rb))

void nvmeibc_trs_hash_insert(struct nvmeibc_subscription_ctx *tr)
{
	struct rb_node **link, *parent;
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&topologies_rb_l, flags);
	tr->handle = topologies_id++;
	topologies_hash_size++;
	link = &topologies_rb.rb_node;
	parent = NULL;
	while (*link) {
		parent = *link;
		link = &parent->rb_right;	// handle always grows so go only right
	}
	rb_link_node(&tr->rb, parent, link);
	rb_insert_color(&tr->rb, &topologies_rb);
	spin_unlock_irqrestore(&topologies_rb_l, flags);
	#ifdef DEBUG_TOPO_CNTRS
		atomic_set(&tr->n_registers, 0);
	#endif
	NFOUT;
}

void nvmeibc_trs_hash_remove(struct nvmeibc_subscription_ctx *tr)
{
	unsigned long flags;

	NFIN;
	spin_lock_irqsave(&topologies_rb_l, flags);
	if (nvmeibc_trs_hash_is_inserted(tr) && !nvmeibc_trs_hash_was_removed(tr)) {
		rb_erase(&tr->rb, &topologies_rb);
		topologies_hash_size--;
		RB_CLEAR_NODE(&tr->rb); /* Mark that tr was removed */
	} else {
		const char *dev_name = (!is_toma_reg_already_dead(tr)) ?
			tr->nt->device_name : "Zombie";
		WARN(true,"nvmeibc double remove of tr handle=0x%llx, %s(%d,%d,%d)\n",
				tr->handle, dev_name, tr->ch, tr->r1, tr->seg);
	}
	spin_unlock_irqrestore(&topologies_rb_l, flags);
 	NFOUT;
}

/* Does range [cs..ce) intersects with [ls..le) ? */
static inline bool __ranges_intersect(u64 cs, u64 ce, u64 ls, u64 le)
{
	return (cs < le) && (ls < ce); /* Inverse of no intersect condition ((cs >= le) || (ls >= ce)) */
}

int nvmeibc_trs_detect_config_corruption(const struct nvmeibc_cinst_params_blk *p)
{
	unsigned long flags;
	const struct rb_node  *n1,  *n2;
	const struct nvmeibc_subscription_ctx *tr1, *tr2;
	int rv = 0, cv;

	(void)p;	// Mechanism below is unified for all instances
	/* Stop after first intersection:*/
	spin_lock_irqsave(&topologies_rb_l, flags);
	for (n1 = rb_first(&topologies_rb); n1 && !rv; n1 = rb_next(n1)) {
		tr1 = rb_entry(n1, struct nvmeibc_subscription_ctx, rb);
		for (n2 = rb_next(n1);          n2 && !rv; n2 = rb_next(n2)) {
			tr2 = rb_entry(n2, struct nvmeibc_subscription_ctx, rb);
			if (tr1->disk != tr2->disk)
				continue;
			cv=	__ranges_intersect(tr1->first_lba, tr1->first_lba+tr1->length,
								   tr2->first_lba, tr2->first_lba+tr2->length);
			if (cv) { /* Print under spinlock or copy tr1/tr2 to stack */
				WARN(1, "nvmeibc corruption: segments intersect: disk=%s "
				   "lba1=%llu, len1=%llu, lba2=%llu, len2=%llu "
				   "s1=(%d,%d,%d), s2=(%d,%d,%d)\n",
				   ((tr1->disk)->base.ops.get_name(&((tr1->disk))->base)),
				   tr1->first_lba, tr1->length,
				   tr2->first_lba, tr2->length,
				   tr1->ch, tr1->r1, tr1->seg,
				   tr2->ch, tr2->r1, tr2->seg);
				rv++;
				// Todo: allert mgmt with error and invoke system crash BUG();
			}
		}
	}
	spin_unlock_irqrestore(&topologies_rb_l, flags);
	return rv;
}

void nvmeibc_trs_hash_verify_empty_unsafe(void)
{
	#ifdef DEBUG_TOMA_REG_LEAKS
		int n_regs = atomic_read(&n_toma_regs);
		if (n_regs != 0)
			_NE(error_trs_hash_verify_empty_leak, DMESG_PREFIX() ": Going to leak @INT TRs", n_regs);
		WARN_ON(n_regs);
	#endif
	if (topologies_rb.rb_node) {
		char *buf = kmalloc(PAGE_SIZE/2, GFP_KERNEL);
		if (buf) {
			nvmeibc_block_layer_all_tostring(buf, PAGE_SIZE/2);
			_NE(error_trs_hash_verify_empty, DMESG_PREFIX() ": Going to leak TRs, \n@STR", buf);
		}
		kfree(buf);
		WARN_ON(true);
	}
}

static struct nvmeibc_subscription_ctx *__find_tr_assume_have_lock(u64 handle)
{
	struct nvmeibc_subscription_ctx *tr = 0; /* Avoid compilation warning */
	struct rb_node *n;

	NFIN;
	BUG_ON(!spin_is_locked(&topologies_rb_l));
	n = topologies_rb.rb_node;
	while (n) {
		tr = rb_entry(n, struct nvmeibc_subscription_ctx, rb);
		if (handle < tr->handle) {
			n = n->rb_left;
		} else if (handle > tr->handle) {
			n = n->rb_right;
		} else {
			break;
		}
	}
	if (!n) {
		tr = 0;
	}
	if (tr) { /* atomic_inc with spinlock because dec() is without spinlock */
		kref_get(&tr->use_count);
	}
	NFOUT;
	return tr;
}

struct nvmeibc_subscription_ctx *__get_tr(u64 handle)
{
	unsigned long flags;
	struct nvmeibc_subscription_ctx *tr;
	spin_lock_irqsave(&topologies_rb_l, flags);
	tr = __find_tr_assume_have_lock(handle);
	spin_unlock_irqrestore(&topologies_rb_l, flags);
	return tr;
}

struct nvmeibc_subscription_ctx *tr_create(void)
{
	struct nvmeibc_subscription_ctx *tr = kzalloc(sizeof(struct nvmeibc_subscription_ctx), GFP_ATOMIC);
	if (tr) {
		kref_init(&tr->use_count);
		spin_lock_init(&tr->death_lock);
		#ifdef DEBUG_TOMA_REG_LEAKS
			atomic_inc(&n_toma_regs);
		#endif
	}
	return tr;
}

void tr_destroy_unused(struct nvmeibc_subscription_ctx *tr)
{
	#ifdef DEBUG_TOMA_REG_LEAKS
		atomic_dec(&n_toma_regs);
	#endif
	kfree(tr);
}

/* Destroy tr, when it was already disconencted from its segment. Assume 'tr' is valid and in state DEAD */
static void __tr_destroy(struct nvmeibc_subscription_ctx *tr)
{
	int rv;
	const char *dev_name = "Zombie";
	struct nvmeibc_icore_ops const* icore_ops = nvmeibc_core_ops_get();
	tr->nt = NULL; /* Do not use it, device might already kfree */
	_NT(trace_b_cp_trs_hash_tr_destroy, "@DEV_NAME" SEGMENT_FMT " TOMA unsubscribe: handle=@HANDLE disk=@DISK", dev_name, tr->ch, tr->r1, tr->seg, tr->handle, tr->disk);
	WARN(tr->status != NVMEIBC_SUBSCRIPTION_STATUS_DEAD, "tr->status=%d\n", tr->status);
	WARN_ON(!nvmeibc_trs_hash_was_removed(tr));
	rv = icore_ops->toma_unsubscribe(icore_ops, &tr->disk->base, (u64)tr->handle);
	if (unlikely(rv < 0)) {
		_NT(warn_b_cp_trs_hash_tr_destroy, "@DEV_NAME" SEGMENT_FMT " TOMA unsubscribe failed(@RV): disk=@DISK", dev_name, tr->ch, tr->r1, tr->seg, rv, tr->disk);
	}
	if (tr->mem_handle) { /* NO IO, so no one uses disk locks */
		nvmeibc_disk_locks_free_mem_info(tr->mem_handle);
		tr->mem_handle = NULL;
	}
	tr->handle = 0;
	tr_destroy_unused(tr);
}

int nvmeibc_trs_hash_subscribe(struct nvmeibc_subscription_ctx *tr,
							   struct nvmeibc_disk_subscription_params *params)
{
	int rv;
	params->arg = tr->handle;
	rv = nvmeibc_disk_subscribe_toma_service(tr->disk, params->arg, params);
	if (unlikely((rv < 0) && (rv != -EAGAIN))) {
		rv = -ENODEV;
		nvmeibc_trs_hash_remove(tr);
		nvmeibc_disk_locks_free_mem_info(tr->mem_handle);
		tr->mem_handle = NULL;
	} else {
		rv = 0;   // No err, or tranport layer will retry and solve it
	}
	return rv;
}

static void __tr_last_ref_release(struct kref *ref)
{
	struct nvmeibc_subscription_ctx *tr = container_of(ref, struct nvmeibc_subscription_ctx, use_count);
	__tr_destroy(tr);
}

void __put_tr(struct nvmeibc_subscription_ctx *tr)
{
	kref_put(&tr->use_count, __tr_last_ref_release);
}

void __add_ref_to_tr(struct nvmeibc_subscription_ctx *tr)
{
	kref_get(&tr->use_count);
}
