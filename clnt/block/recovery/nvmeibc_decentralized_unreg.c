/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_decentralized_unreg.h"
#include "block/nvmeibc_block_common.h"
#define N_MAX_LOCKS_IN_CACHE (64)	// Daniel: Todo, make larger / param ??
extern bool qa_ec_stress_debug;
#define is_cache_full(c) ((c)->size >= (qa_ec_stress_debug ? 2 : N_MAX_LOCKS_IN_CACHE))		// Reduce LRU cache size to be only a few entires

static const char* _resolve_status_to_string(enum stale_lock_resolve_status st)
{		// Steps of resolving stale lock
	switch (st) {
		case stale_lock_resolve_broken		: return "brk";
		case stale_lock_resolve_unkown      : return "unk";
		case stale_lock_resolve_asked_toma  : return "ask";
		case stale_lock_resolve_partial_ans : return "prt";
		case stale_lock_resolve_safe_to_use : return "OK ";
		default:							  return "???";
	}
};

/**************************** Cache element ***********************************/
typedef struct _lock_cache_elem_t {
	struct list_head 		link;	// List of newest to oldest
	struct 		rb_node     rb;		// Link to hash map to find element by key
	u32			lock_id;			// Key of the search
	enum stale_lock_resolve_status	status;		// Value: The status of the lock
	u32			awaiting_answers;	// Bit field which marks which toma's answers we still need. Supports raid of up to 32 length
	uuid_be 	cuuid;				// UUID of the client which left stale lock, on each of the Toma's. Must be identical to all Tomas, or Zero on Tomas that do not hold any remnance of the stale lock
	ulong 		jif_init;			// For debug: Jiffies when element was created
} lock_elem;

#if (N_MAX_RAID_SLICE_LEN > 32)
	#error lock cache does not support raids of more than 32 segments
#endif

#define INIT_AWATING_ANSWERS ((u32)-1)				// All toma's needed
static inline void lock_elem_init(lock_elem* el, u32 lock_id)
{
	INIT_LIST_HEAD(&el->link);
	el->status = stale_lock_resolve_unkown;
	el->lock_id = lock_id;
	el->awaiting_answers = INIT_AWATING_ANSWERS;
	el->cuuid = NULL_UUID_BE;
	el->jif_init = jiffies;
	_ND(trace_decentralized_unreg_lock_elem_init, "Added lock to resolve cache @LOCK_ENT", el->lock_id);
}

static void __update_uuid(const uuid_be *toma, uuid_be *rv)
{
	if (    nvmeib_uuid_cmp(*toma, NULL_UUID_BE)) { 					// Toma knows the UUID (it is not 0)
		if (nvmeib_uuid_cmp(*rv  , NULL_UUID_BE)) {						// If client already knows the UUID verify that they are identical (Just for debug)
			const int diff = nvmeib_uuid_cmp(*toma, *rv);
			WARN(diff, "Bug: toma %pUB != clnt %pUB\n", toma, rv);
		}
		*rv = *toma;					// Update uuid
	}
}

static inline void lock_elem_update(lock_elem* el, u32 answers_i, ulong* stat,
			enum stale_lock_resolve_status new_status, const uuid_be *cuuid)
{	// Must hold spinlock so 2 threads do not update element. Update is atomic
	const char *old_str = _resolve_status_to_string(el->status);
	const char *new_str = _resolve_status_to_string(new_status);
	const char *res_str;
	bool verbose = true;

	switch (new_status) {
	case stale_lock_resolve_unkown      :							// Some problem with the lock. Rollback the status.
		el->status = stale_lock_resolve_unkown;						// Example: Sending msg to toma failed, element was removed and re-added in LRU
		el->awaiting_answers = INIT_AWATING_ANSWERS;
		break;

	case stale_lock_resolve_asked_toma  :							// IO is asking about the lock and if it is unknown wants to send messages to Toma
		if (el->status < new_status) {								// Illegal to set status, only try to increase it
			el->status = new_status;								// Update the status to next state
			el->awaiting_answers = (u32)answers_i;					// Generate a mask of waiting for answers_i tomas
		} else { /* Other IO already asked toma so nothing to do */
			verbose = false; /* It is in data path, and will clog dmesg! */
		}
		break;

	case stale_lock_resolve_partial_ans :							// Arrived Toma message
		if (el->status < stale_lock_resolve_asked_toma) {
			/* Toma message arrived but Lock was removed from cach and re-added
			   or broken. Ignore message & need to restart sending complains */
			BUG_ON(el->awaiting_answers != INIT_AWATING_ANSWERS);
		} else if (el->status < stale_lock_resolve_safe_to_use) {	// Upgrade status to partial answer
			__update_uuid(cuuid, &el->cuuid);
			el->awaiting_answers &= (~(1<<answers_i));				// Turn-off answers_i's bit
			if (el->awaiting_answers != 0)
				el->status = stale_lock_resolve_partial_ans;
			else {
				const ulong diff = (jiffies - el->jif_init);
				el->status = stale_lock_resolve_safe_to_use;
				if (diff > *stat)
					*stat = diff;									// Store the worst reply time
			}
		}
		break;
	case stale_lock_resolve_broken		:							// Illegal to set it, only auto-calculate it
	case stale_lock_resolve_safe_to_use :							// Illegal to set it, only auto-calculate it
	default:
		BUG(); break;
	}

	if (unlikely(verbose)) {
		res_str = _resolve_status_to_string(el->status);
		_NT(t_01_decunr, "@LOCK_ENT, cuuid=@CLIENT_UUID, (mask=@X, ans=@X|@STR->@STR=@STR)", el->lock_id,
			   &el->cuuid, el->awaiting_answers, answers_i, old_str, new_str, res_str);
	}
}

/****************************** Cache LRU *************************************/
static lock_elem* __remove_oldest_entry(struct stale_lock_resolver_cache_t *c)
{	// Must hold spinlock!
	lock_elem *el = list_last_entry(&c->head, lock_elem, link);
	rb_erase(&el->rb, &c->cache_root);
	list_del(&el->link);
	c->size--;
	return el;
}

static lock_elem* __alloc_new_entry(struct stale_lock_resolver_cache_t *c)
{	// Must hold spinlock!
	lock_elem *el;
	if (is_cache_full(c))
		el = __remove_oldest_entry(c);
	else
		el = &c->elems_arr[c->size]; // Just filling the cache
	c->size++;
	return el;
}

int stale_lock_resolver_cache_t_create(struct stale_lock_resolver_cache_t *c)
{
	int rv = 0;
	INIT_LIST_HEAD(&c->head);
	c->cache_root = RB_ROOT;
	c->size = 0;
	spin_lock_init(&c->lock);
	c->elems_arr = kzalloc(sizeof(lock_elem)*N_MAX_LOCKS_IN_CACHE, GFP_KERNEL);
	if (!c->elems_arr)
		rv = -ENOMEM;
	return rv;
}

#define JIFF_2_MILSEC(J) ((int)(((J) * 1000) / HZ))
void stale_lock_resolver_cache_t_clear(struct stale_lock_resolver_cache_t *c)
{
	unsigned long flags;
	spin_lock_irqsave(&c->lock, flags);
	if (c->size)
		_NT(trace_1_cache_clear, "@SIZE entries, longest=@LONGEST[msec]", c->size, JIFF_2_MILSEC(c->longest_resolve));
	INIT_LIST_HEAD(&c->head);
	c->cache_root = RB_ROOT;
	c->size = 0;
	c->longest_resolve = 0UL;
	spin_unlock_irqrestore(&c->lock, flags);
}

void stale_lock_resolver_cache_t_to_log(const struct stale_lock_resolver_cache_t *c)
{
	unsigned long flags;
	lock_elem *el;
	int index = 0;
	spin_lock_irqsave((spinlock_t*)&c->lock, flags);
	_NI_dmesg(t_70_dp_dbg_tools, "@SIZE entries:", c->size);
	list_for_each_entry(el, &c->head, link) {
		_NI_dmesg(t_71_dp_dbg_tools, "\t@INDEX) @LOCK_ENT @_RESOLVE_STATUS_TO_STRING, wait_mask=@WAIT_MASK", index++, el->lock_id, _resolve_status_to_string(el->status), el->awaiting_answers);
	}
	spin_unlock_irqrestore((spinlock_t*)&c->lock, flags);
}

static int stale_lock_resolver_cache_t_to_str(const struct stale_lock_resolver_cache_t *c, char *buf, int len)
{
	#define BUF_ADD(...) pos += scnprintf(buf + pos, len - pos, __VA_ARGS__)
	unsigned long flags;
	const lock_elem *el;
	struct rb_node *n;
	const ulong now = jiffies;
	int pos = 0, i = 0;

	spin_lock_irqsave((spinlock_t*)&c->lock, flags);
	BUF_ADD("%d stale locks\n", c->size);
	if (c->size == 0)
		goto _out;
	for (n = rb_first(&c->cache_root); n; n = rb_next(n)) { // Sort by locks
		el = container_of(n, lock_elem, rb);
		BUF_ADD("\t%3d) 0x%x %s, wait_mask=0x%x uuid=%pUB %d[msec]\n", i++, el->lock_id,
			  _resolve_status_to_string(el->status), el->awaiting_answers, &el->cuuid,
				JIFF_2_MILSEC(now - el->jif_init));
	}
	BUF_ADD("\t-------- LRU: \n");
	i = 0;
	list_for_each_entry(el, &c->head, link) {		// Sort by LRU
		BUF_ADD("\t%3d) 0x%x %s, wait_mask=0x%x\n", i++, el->lock_id, _resolve_status_to_string(el->status), el->awaiting_answers);
	}
_out:
	spin_unlock_irqrestore((spinlock_t*)&c->lock, flags);
	return pos;
	#undef BUF_ADD
}

void stale_lock_resolver_cache_t_destroy(struct stale_lock_resolver_cache_t *c)
{
	stale_lock_resolver_cache_t_clear(c);
	if (c->elems_arr) {						// Preallocated version
		kfree(c->elems_arr);
		c->elems_arr = NULL;
	}
}

typedef struct t_insert {	// Find where to insert a new element in rbtree
	lock_elem* el;				// Result of search
	struct rb_node *parent;		// Parent in rb-tree
	struct rb_node **_new;		// Place where to insert the new element
} t_insert;

static lock_elem* __find_by_key(struct rb_root *r, u32 lock_id, t_insert *rv)
{
	s32 diff;
	rv->_new =   &(r->rb_node);
	rv->parent = NULL;
  	while (*rv->_new) { 		// Figure out where to put new node
  		rv->el = container_of(*rv->_new, lock_elem, rb);
		diff  = (s32)(lock_id - rv->el->lock_id);
		rv->parent = *rv->_new;
		if      (diff < 0) rv->_new = &((*rv->_new)->rb_left);
  		else if (diff > 0) rv->_new = &((*rv->_new)->rb_right);
  		else return rv->el;
  	}
	rv->el = NULL;			// Not found, but rv describes where to insert
	return NULL;
}

static lock_elem *__find_or_insert_by_key(struct stale_lock_resolver_cache_t *c,
										  u32 lock_id)
{	// Must hold spinlock!
	const bool is_full = is_cache_full(c);
	struct rb_root *r = &c->cache_root;
	t_insert rv;
	lock_elem *el;
	if (__find_by_key(r, lock_id, &rv))
		return rv.el;
	if ((el = __alloc_new_entry(c)) == NULL) // Not found, Allocate new
		return NULL;

	/* Add new node to the hash (but not to LRU!) */
	if (is_full)
		__find_by_key(r, lock_id, &rv);	// Deleting old node, changed the insert
	lock_elem_init(  el, lock_id);
  	rb_link_node(   &el->rb, rv.parent, rv._new);
  	rb_insert_color(&el->rb, r);
	return el;
}

enum stale_lock_resolve_status stale_lock_resolver_cache_t_store(
	struct stale_lock_resolver_cache_t *c, u32 lock_id, u32 answers_i,
	enum stale_lock_resolve_status status, const uuid_be *cuuid)
{
	enum stale_lock_resolve_status rv = stale_lock_resolve_broken;
	unsigned long flags;
	lock_elem *el;
	spin_lock_irqsave(&c->lock, flags);
	if ((el = __find_or_insert_by_key(c, lock_id)) == NULL) {
		_NT(error_decentralized_unreg_stale_lock_resolver_cache_t_store, "Error, insert/find in cache: @LOCK_ENT(ans=@X|@_RESOLVE_STATUS_TO_STRING)",
		   lock_id, answers_i, _resolve_status_to_string(status));
		goto _out;
	}
	rv = el->status;
	lock_elem_update(el, answers_i, &c->longest_resolve, status, cuuid);
	if (!list_empty(&el->link)) 	// If part of cache - remove from list to insert as head
		list_del(&el->link);
	list_add(&el->link, &c->head);
_out:
	spin_unlock_irqrestore(&c->lock, flags); // Note: 'el' points to garbage
	return rv;
}

int stale_lock_resolver_fill_cuuid_by_lockid(
	struct stale_lock_resolver_t *slr, u32 lock_id, uuid_be *cuuid)
{
	struct stale_lock_resolver_cache_t *c = &slr->cache;
	unsigned long flags;
	lock_elem *el;
	int rv = 0;
	spin_lock_irqsave(&c->lock, flags);
	if ((el = __find_or_insert_by_key(c, lock_id)) == NULL) {
		_NT(error_decentralized_unreg_stale_lock_resolver_fill_cuuid_by_lockid, "Error, insert/find in cache: @LOCK_ENT", lock_id);
		rv = -ENOENT;
	} else {
		if ((el->status == stale_lock_resolve_partial_ans && nvmeib_uuid_cmp(el->cuuid, NULL_UUID_BE)) || (el->status == stale_lock_resolve_safe_to_use)) {
			*cuuid = el->cuuid;	// At least 1 Toma already told us the UUID
		} else {
			rv = -ENOENT; //  We only just asked, dont have uuid yet
		}
	}
	spin_unlock_irqrestore(&c->lock, flags); // Note: 'el' points to garbage
	return rv;
}

/********************** stale lock resolver ***********************************/
int stale_lock_resolver_set_resolved(struct stale_lock_resolver_t *slr,
			int seg_ind_in_raid, struct nvmeibt_cleaned_stalock_info* pl)
{
	const u32 lock_id = (pl->lock_id | nvmeib_stale_bit_mask_ec.all);		// Toma answers about the raw lock. We store it with stale bit
	enum stale_lock_resolve_status rv = stale_lock_resolver_cache_t_store(
		&slr->cache, lock_id, seg_ind_in_raid, stale_lock_resolve_partial_ans,
		(void*)pl->cuuid);			// This is the only way to update cuuid
	return (rv != stale_lock_resolve_broken) ? 0 : -ENOENT;
}

void stale_lock_resolver_clear_all(struct stale_lock_resolver_t *slr)
{
	stale_lock_resolver_cache_t_clear(&slr->cache);
}

int stale_lock_resolver_to_str(const struct stale_lock_resolver_t *slr, char *buf, int len)
{
	if (slr) {
		if (buf)
			return stale_lock_resolver_cache_t_to_str(&slr->cache, buf, len);
		stale_lock_resolver_cache_t_to_log(&slr->cache);
	}
	return 0;
}

enum stale_lock_resolve_status stale_lock_resolver_get_status(struct stale_lock_resolver_t *slr, u32 lock_id, const struct nvmeibc_cmd_lock *l)
{
	enum stale_lock_resolve_status rv;
	const struct nvmeibc_raid1* r = nvmeibc_get_raid1_of_seg(l->ds);
	const int n_toma_bitmap = nvmeibc_raid1_get_inverse_sgmnts_bmp(r, dead);

	if (!nvmeibc_raid_is_ec(r) && (lock_id == R1_STALE_SPECIAL_LOCK_VAL)) {
		rv = stale_lock_resolve_safe_to_use; // Always safe to take stsp in R1
		goto _out;
	}
	WARN(!(lock_id&nvmeib_stale_bit_mask_ec.all),"nvmeibc bug, illegal lid=0x%x\n", lock_id);
	rv = stale_lock_resolver_cache_t_store(&slr->cache, lock_id, n_toma_bitmap,
										   stale_lock_resolve_asked_toma, NULL);
	switch (rv) {
		case stale_lock_resolve_broken:
		case stale_lock_resolve_asked_toma:
		case stale_lock_resolve_partial_ans:
		case stale_lock_resolve_safe_to_use:
			goto _out;		// cmp-exchng failed. Nothing to do, return status
		default: {			// Status was update to asked_toma
			int send_rv = 0, i;
			BUG_ON(rv != stale_lock_resolve_unkown);
			for (i = 0; (i < r->replicas)&&(!send_rv); i++ ) {
				if (is_seg_active(r->segments[i])) {
					send_rv = __send_toma_lock_help(l, &r->segments[i]);
				}
			}
			if (unlikely(send_rv < 0)) {	// Send failed, revert back, (will retry it in future)
				stale_lock_resolver_cache_t_store(&slr->cache, lock_id,
						INIT_AWATING_ANSWERS, stale_lock_resolve_unkown, NULL);
				_NT(trace_decentralized_unreg_stale_lock_resolver_get_status, "lid=@LID, ask_toma_failed, setting to unknown", lock_id);
			} else
				rv = stale_lock_resolve_asked_toma;
		}
	}
_out:
	return rv;
}

int stale_lock_resolver_get_create( struct stale_lock_resolver_t *slr)
{
	return stale_lock_resolver_cache_t_create(&slr->cache);
}

void stale_lock_resolver_get_destroy(struct stale_lock_resolver_t *slr)
{
	stale_lock_resolver_cache_t_destroy(&slr->cache);
}

