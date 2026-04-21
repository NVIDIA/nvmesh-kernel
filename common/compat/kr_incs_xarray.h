/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KERNEL_XARRAY_SIM_H
#define KERNEL_XARRAY_SIM_H

/*
 * kr_incs_xarray.h - minimal xarray simulation for userspace unit tests.
 *
 * Implements the subset of the kernel xarray API used by the TPV allocator:
 *   xa_init / xa_destroy / xa_store / xa_erase / xa_load / xa_for_each / xa_err
 *
 * Backed by an unsorted singly-linked list of (key, value) nodes.
 * Not thread-safe: tests must hold appropriate locks or be single-threaded.
 *
 * Performance note: O(N) lookups are acceptable for the small extent maps
 * encountered in unit tests (<<1000 entries).
 */

#ifndef __KERNEL__

/* Forward declarations needed from kr_incs_data_structs.h (already included). */
/* struct list_head, list_empty, list_first_entry, list_next_entry, etc. */

struct xa_node {
	unsigned long    key;
	void            *val;
	struct list_head node;
};

struct xarray {
	struct list_head head;
};

/* xa_init_flags / xa_init */
static inline void xa_init_flags(struct xarray *xa, unsigned long flags)
{
	(void)flags;
	INIT_LIST_HEAD(&xa->head);
}

static inline void xa_init(struct xarray *xa)
{
	xa_init_flags(xa, 0);
}

/* xa_destroy - free all xa_node structs; caller is responsible for values */
static inline void xa_destroy(struct xarray *xa)
{
	struct xa_node *n, *tmp;

	list_for_each_entry_safe(n, tmp, &xa->head, node) {
		list_del(&n->node);
		kfree(n);
	}
}

/* xa_load - look up the value for @index; NULL if not present */
static inline void *xa_load(struct xarray *xa, unsigned long index)
{
	struct xa_node *n;

	list_for_each_entry(n, &xa->head, node) {
		if (n->key == index)
			return n->val;
	}
	return NULL;
}

/*
 * xa_store - store @entry at @index.
 *
 * Returns: NULL if no prior mapping existed (new key).
 *          the old value if there was a prior mapping.
 *          ERR_PTR(-ENOMEM) if allocation failed.
 *
 * Unlike the kernel xarray, this simulator does not distinguish between
 * xa_store returning the old entry and returning NULL for a new entry;
 * callers that only check xa_err() are unaffected.
 */
static inline void *xa_store(struct xarray *xa, unsigned long index,
			      void *entry, gfp_t gfp)
{
	struct xa_node *n;

	/* Replace existing mapping if key already present. */
	list_for_each_entry(n, &xa->head, node) {
		if (n->key == index) {
			void *old = n->val;
			n->val = entry;
			return old;
		}
	}

	/* New key: allocate a node. */
	n = kmalloc(sizeof(*n), gfp);
	if (!n)
		return ERR_PTR(-ENOMEM);

	n->key = index;
	n->val = entry;
	INIT_LIST_HEAD(&n->node);
	list_add_tail(&n->node, &xa->head);
	return NULL;	/* no prior mapping */
}

/*
 * xa_erase - remove the mapping for @index and return the value.
 * Returns NULL if @index was not mapped.
 */
static inline void *xa_erase(struct xarray *xa, unsigned long index)
{
	struct xa_node *n, *tmp;

	list_for_each_entry_safe(n, tmp, &xa->head, node) {
		if (n->key == index) {
			void *val = n->val;
			list_del(&n->node);
			kfree(n);
			return val;
		}
	}
	return NULL;
}

/*
 * xa_err - extract an errno from an xa_store return value.
 * Returns 0 on success, negative errno on failure.
 */
static inline int xa_err(void *entry)
{
	return IS_ERR(entry) ? (int)PTR_ERR(entry) : 0;
}

/*
 * xa_for_each - iterate over all entries in an xarray.
 *
 * Usage (caller declares @index and @entry before the loop):
 *
 *   unsigned long index;
 *   struct foo *entry;
 *   xa_for_each(&xa, index, entry) {
 *       ... use index, entry ...
 *   }
 *
 * Do NOT erase entries during xa_for_each; use xa_destroy afterward.
 * (Production code never erases during xa_for_each either.)
 */
#define xa_for_each(xa, index, entry)					\
	for (struct xa_node *__xa_n =					\
		     list_empty(&(xa)->head) ? NULL :			\
		     list_first_entry(&(xa)->head, struct xa_node, node);\
	     __xa_n != NULL &&						\
		     ((index) = __xa_n->key,				\
		      (entry) = (typeof(entry))__xa_n->val, 1);		\
	     __xa_n = list_is_last(&__xa_n->node, &(xa)->head) ? NULL :	\
		      list_next_entry(__xa_n, node))

#endif /* !__KERNEL__ */
#endif /* KERNEL_XARRAY_SIM_H */
