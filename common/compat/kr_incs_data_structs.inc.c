/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs_data_structs.h"
#include "kr_incs_asserts.h"
/********************************* List *************************************/
/* These are non-NULL pointers that will result in page faults
 * under normal circumstances, used to verify that nobody uses
 * non-initialized list entries.
 */
#define POISON_POINTER_DELTA    0xdead000000000000
#define LIST_POISON1  ((void *) 0x00100100 + POISON_POINTER_DELTA)
#define LIST_POISON2  ((void *) 0x00200200 + POISON_POINTER_DELTA)

void list_del(struct list_head *entry){
	const struct list_head *prev = entry->prev, *next = entry->next;
	WARN(next == LIST_POISON1, "list_del corruption, %p->next is LIST_POISON1 (%p)\n", entry, LIST_POISON1);
	WARN(prev == LIST_POISON2, "list_del corruption, %p->prev is LIST_POISON2 (%p)\n", entry, LIST_POISON2);
	WARN(prev->next != entry , "list_del corruption. prev->next should be %p, but was %p\n", entry, prev->next);
	WARN(next->prev != entry,  "list_del corruption. next->prev should be %p, but was %p\n", entry, next->prev);
	__list_del(entry->prev, entry->next);
	entry->next = LIST_POISON1;
	entry->prev = LIST_POISON2;
}

static inline void __list_add(struct list_head *New, struct list_head *prev, struct list_head *next){
	WARN(next->prev != prev, "list_add corruption. next->prev should be prev (%p), but was %p. (next=%p).\n", prev, next->prev, next);
	WARN(prev->next != next, "list_add corruption. prev->next should be next (%p), but was %p. (prev=%p).\n", next, prev->next, prev);
	WARN(New == prev || New == next, "list_add double add: new=%p, prev=%p, next=%p.\n", New, prev, next);
	next->prev = New;
	New->next  = next;
	New->prev  = prev;
	prev->next = New;
}

bool is_in_list(const struct list_head *entry){
	if ((entry->next == entry->prev)&&(entry->prev == entry))
		return false;									// This entry is not part of any list
	if ((entry->next == LIST_POISON1)||(entry->prev == LIST_POISON2))
		return false;									// This entry was just removed from the list
	return true;
}

void list_add(struct list_head *New, struct list_head *head){
	__list_add(New, head, head->next);
}

void __list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry){
	struct list_head *new_first = entry->next;
	list->next = head->next;
	list->next->prev = list;
	list->prev = entry;
	entry->next = list;
	head->next = new_first;
	new_first->prev = head;
}

void list_add_tail(struct list_head *New, struct list_head *head){
	__list_add(New, head->prev, head);
}

bool llist_add_batch(struct llist_node *new_first,
		     struct llist_node *new_last,
		     struct llist_head *head)
{
	struct llist_node *first = __atomic_load_n((struct llist_node * volatile *)&head->first, __ATOMIC_RELAXED);
	int succ = __ATOMIC_RELEASE;   // publish initialized node
	int fail = __ATOMIC_RELAXED;   // on CAS failure we’ll retry anyway

	do {
		new_last->next = first;
	} while(!__atomic_compare_exchange_n((struct llist_node * volatile *)&head->first, &first, new_first,
				       /*weak=*/false, succ, fail));

	return !first;
}

/* Comparison Function - Same rv syntax as strcmp */
typedef int (*prio_list_cmp_fn)(void *priv, struct list_head *a, struct list_head *b);
/* Add to tail of priority group in list */
void prio_list_add_tail(struct list_head *new, struct list_head *head, prio_list_cmp_fn cmp_fn, void *priv);
void prio_list_add_tail(struct list_head *new, struct list_head *head, prio_list_cmp_fn cmp_fn, void *priv) {
	(void)cmp_fn;
	(void)priv;
	list_add_tail(new, head);
}

int list_calc_size(const struct list_head *head){
	const struct list_head *start = head, *cur = start->next;
	int i;
	for (i=0; cur!=start; cur=cur->next, i++);
	return i;
}

/*********************** RED BLACK tree Implementation ***********************/
// http://lxr.free-electrons.com/source/include/linux/rbtree_augmented.h
#define WRITE_ONCE(x, val) x=(val)

struct rb_augment_callbacks {
	void (*propagate)(struct rb_node *node, struct rb_node *stop);
	void (*copy)(struct rb_node *old, struct rb_node *new_);
	void (*rotate)(struct rb_node *old, struct rb_node *new_);
};

void __attribute__((__unused__)) __rb_insert_augmented(struct rb_node *node, struct rb_root *root, void (*augment_rotate)(struct rb_node *old, struct rb_node *new_));
static inline void __attribute__((__unused__)) rb_insert_augmented(struct rb_node *node, struct rb_root *root, const struct rb_augment_callbacks *augment){
	__rb_insert_augmented(node, root, augment->rotate);
}

#define RB_DECLARE_CALLBACKS(rbstatic, rbname, rbstruct, rbfield, rbtype, rbaugmented, rbcompute)		\
static inline void rbname ## _propagate(struct rb_node *rb, struct rb_node *stop){						\
	while (rb != stop) {						\
		rbstruct *node = rb_entry(rb, rbstruct, rbfield);	\
		rbtype augmented = rbcompute(node);			\
		if (node->rbaugmented == augmented)			\
			break;						\
		node->rbaugmented = augmented;				\
		rb = rb_parent(&node->rbfield);				\
	}								\
}									\
static inline void rbname ## _copy(struct rb_node *rb_old, struct rb_node *rb_new){			\
	rbstruct *old = rb_entry(rb_old, rbstruct, rbfield);		\
	rbstruct *new_ = rb_entry(rb_new, rbstruct, rbfield);		\
	new_->rbaugmented = old->rbaugmented;				\
}									\
static void rbname ## _rotate(struct rb_node *rb_old, struct rb_node *rb_new){				\
	rbstruct *old = rb_entry(rb_old, rbstruct, rbfield);		\
	rbstruct *new_ = rb_entry(rb_new, rbstruct, rbfield);		\
	new_->rbaugmented = old->rbaugmented;				\
	old->rbaugmented = rbcompute(old);				\
}									\
rbstatic const struct rb_augment_callbacks rbname = {			\
	rbname ## _propagate, rbname ## _copy, rbname ## _rotate	\
};


#define	RB_RED		0
#define	RB_BLACK	1

#define __rb_parent(pc)    ((struct rb_node *)(pc & ~3))
#define __rb_color(pc)     ((pc) & 1)
#define __rb_is_black(pc)  __rb_color(pc)
#define __rb_is_red(pc)    (!__rb_color(pc))
#define rb_color(rb)       __rb_color((rb)->__rb_parent_color)
#define rb_is_red(rb)      __rb_is_red((rb)->__rb_parent_color)
#define rb_is_black(rb)    __rb_is_black((rb)->__rb_parent_color)

static inline void rb_set_parent(struct rb_node *rb, struct rb_node *p){
	rb->__rb_parent_color = rb_color(rb) | (unsigned long)p;
}

static inline void rb_set_parent_color(struct rb_node *rb, struct rb_node *p, int color){
	rb->__rb_parent_color = (unsigned long)p | color;
}

static inline void __rb_change_child(struct rb_node *old, struct rb_node *new_, struct rb_node *parent, struct rb_root *root){
	if (parent) {
		if (parent->rb_left == old)
			parent->rb_left = new_;
		else
			parent->rb_right = new_;
	} else
		root->rb_node = new_;
}

void __rb_erase_color(struct rb_node *parent, struct rb_root *root, void (*augment_rotate)(struct rb_node *old, struct rb_node *new_));

#ifndef __always_inline
#define __always_inline inline
#endif

static __always_inline struct rb_node * __rb_erase_augmented(struct rb_node *node, struct rb_root *root, const struct rb_augment_callbacks *augment){
	struct rb_node *child = node->rb_right, *tmp = node->rb_left;
	struct rb_node *parent, *rebalance;
	unsigned long pc;

	if (!tmp) {
		pc = node->__rb_parent_color;
		parent = __rb_parent(pc);
		__rb_change_child(node, child, parent, root);
		if (child) {
			child->__rb_parent_color = pc;
			rebalance = NULL;
		} else
			rebalance = __rb_is_black(pc) ? parent : NULL;
		tmp = parent;
	} else if (!child) {
		/* Still case 1, but this time the child is node->rb_left */
		tmp->__rb_parent_color = pc = node->__rb_parent_color;
		parent = __rb_parent(pc);
		__rb_change_child(node, tmp, parent, root);
		rebalance = NULL;
		tmp = parent;
	} else {
		struct rb_node *successor = child, *child2;
		tmp = child->rb_left;
		if (!tmp) {
			parent = successor;
			child2 = successor->rb_right;
			augment->copy(node, successor);
		} else {
			do {
				parent = successor;
				successor = tmp;
				tmp = tmp->rb_left;
			} while (tmp);
			parent->rb_left = child2 = successor->rb_right;
			successor->rb_right = child;
			rb_set_parent(child, successor);
			augment->copy(node, successor);
			augment->propagate(parent, successor);
		}

		successor->rb_left = tmp = node->rb_left;
		rb_set_parent(tmp, successor);

		pc = node->__rb_parent_color;
		tmp = __rb_parent(pc);
		__rb_change_child(node, successor, tmp, root);
		if (child2) {
			successor->__rb_parent_color = pc;
			rb_set_parent_color(child2, parent, RB_BLACK);
			rebalance = NULL;
		} else {
			unsigned long pc2 = successor->__rb_parent_color;
			successor->__rb_parent_color = pc;
			rebalance = __rb_is_black(pc2) ? parent : NULL;
		}
		tmp = successor;
	}

	augment->propagate(tmp, NULL);
	return rebalance;
}

static __always_inline __attribute__((__unused__)) void rb_erase_augmented(struct rb_node *node, struct rb_root *root, const struct rb_augment_callbacks *augment){
	struct rb_node *rebalance = __rb_erase_augmented(node, root, augment);
	if (rebalance)
		__rb_erase_color(rebalance, root, augment->rotate);
}

// https://github.com/torvalds/linux/blob/master/lib/rbtree.c
static inline void rb_set_black(struct rb_node *rb){
	rb->__rb_parent_color |= RB_BLACK;
}

static inline struct rb_node *rb_red_parent(struct rb_node *red){
	return (struct rb_node *)red->__rb_parent_color;
}

static inline void __rb_rotate_set_parents(struct rb_node *old, struct rb_node *new_, struct rb_root *root, int color){
	struct rb_node *parent = rb_parent(old);
	new_->__rb_parent_color = old->__rb_parent_color;
	rb_set_parent_color(old, new_, color);
	__rb_change_child(old, new_, parent, root);
}

static __always_inline void __rb_insert(struct rb_node *node, struct rb_root *root, void (*augment_rotate)(struct rb_node *old, struct rb_node *new_)){
	struct rb_node *parent = rb_red_parent(node), *gparent, *tmp;
	while (true) {
		if (!parent) {
			rb_set_parent_color(node, NULL, RB_BLACK);
			break;
		} else if (rb_is_black(parent))
			break;
		gparent = rb_red_parent(parent);
		tmp = gparent->rb_right;
		if (parent != tmp) {	/* parent == gparent->rb_left */
			if (tmp && rb_is_red(tmp)) {
				rb_set_parent_color(tmp, gparent, RB_BLACK);
				rb_set_parent_color(parent, gparent, RB_BLACK);
				node = gparent;
				parent = rb_parent(node);
				rb_set_parent_color(node, parent, RB_RED);
				continue;
			}

			tmp = parent->rb_right;
			if (node == tmp) {
				tmp = node->rb_left;
				WRITE_ONCE(parent->rb_right, tmp);
				WRITE_ONCE(node->rb_left, parent);
				if (tmp)
					rb_set_parent_color(tmp, parent,
							    RB_BLACK);
				rb_set_parent_color(parent, node, RB_RED);
				augment_rotate(parent, node);
				parent = node;
				tmp = node->rb_right;
			}

			WRITE_ONCE(gparent->rb_left, tmp); /* == parent->rb_right */
			WRITE_ONCE(parent->rb_right, gparent);
			if (tmp)
				rb_set_parent_color(tmp, gparent, RB_BLACK);
			__rb_rotate_set_parents(gparent, parent, root, RB_RED);
			augment_rotate(gparent, parent);
			break;
		} else {
			tmp = gparent->rb_left;
			if (tmp && rb_is_red(tmp)) {
				/* Case 1 - color flips */
				rb_set_parent_color(tmp, gparent, RB_BLACK);
				rb_set_parent_color(parent, gparent, RB_BLACK);
				node = gparent;
				parent = rb_parent(node);
				rb_set_parent_color(node, parent, RB_RED);
				continue;
			}

			tmp = parent->rb_left;
			if (node == tmp) {
				/* Case 2 - right rotate at parent */
				tmp = node->rb_right;
				WRITE_ONCE(parent->rb_left, tmp);
				WRITE_ONCE(node->rb_right, parent);
				if (tmp)
					rb_set_parent_color(tmp, parent,
							    RB_BLACK);
				rb_set_parent_color(parent, node, RB_RED);
				augment_rotate(parent, node);
				parent = node;
				tmp = node->rb_left;
			}

			/* Case 3 - left rotate at gparent */
			WRITE_ONCE(gparent->rb_right, tmp); /* == parent->rb_left */
			WRITE_ONCE(parent->rb_left, gparent);
			if (tmp)
				rb_set_parent_color(tmp, gparent, RB_BLACK);
			__rb_rotate_set_parents(gparent, parent, root, RB_RED);
			augment_rotate(gparent, parent);
			break;
		}
	}
}

static __always_inline void ____rb_erase_color(struct rb_node *parent, struct rb_root *root, void (*augment_rotate)(struct rb_node *old, struct rb_node *new_)){
	struct rb_node *node = NULL, *sibling, *tmp1, *tmp2;
	while (true) {
		sibling = parent->rb_right;
		if (node != sibling) {	/* node == parent->rb_left */
			if (rb_is_red(sibling)) {
				tmp1 = sibling->rb_left;
				WRITE_ONCE(parent->rb_right, tmp1);
				WRITE_ONCE(sibling->rb_left, parent);
				rb_set_parent_color(tmp1, parent, RB_BLACK);
				__rb_rotate_set_parents(parent, sibling, root,
							RB_RED);
				augment_rotate(parent, sibling);
				sibling = tmp1;
			}
			tmp1 = sibling->rb_right;
			if (!tmp1 || rb_is_black(tmp1)) {
				tmp2 = sibling->rb_left;
				if (!tmp2 || rb_is_black(tmp2)) {
					rb_set_parent_color(sibling, parent,
							    RB_RED);
					if (rb_is_red(parent))
						rb_set_black(parent);
					else {
						node = parent;
						parent = rb_parent(node);
						if (parent)
							continue;
					}
					break;
				}
				tmp1 = tmp2->rb_right;
				WRITE_ONCE(sibling->rb_left, tmp1);
				WRITE_ONCE(tmp2->rb_right, sibling);
				WRITE_ONCE(parent->rb_right, tmp2);
				if (tmp1)
					rb_set_parent_color(tmp1, sibling,
							    RB_BLACK);
				augment_rotate(sibling, tmp2);
				tmp1 = sibling;
				sibling = tmp2;
			}
			tmp2 = sibling->rb_left;
			WRITE_ONCE(parent->rb_right, tmp2);
			WRITE_ONCE(sibling->rb_left, parent);
			rb_set_parent_color(tmp1, sibling, RB_BLACK);
			if (tmp2)
				rb_set_parent(tmp2, parent);
			__rb_rotate_set_parents(parent, sibling, root,
						RB_BLACK);
			augment_rotate(parent, sibling);
			break;
		} else {
			sibling = parent->rb_left;
			if (rb_is_red(sibling)) {
				/* Case 1 - right rotate at parent */
				tmp1 = sibling->rb_right;
				WRITE_ONCE(parent->rb_left, tmp1);
				WRITE_ONCE(sibling->rb_right, parent);
				rb_set_parent_color(tmp1, parent, RB_BLACK);
				__rb_rotate_set_parents(parent, sibling, root,
							RB_RED);
				augment_rotate(parent, sibling);
				sibling = tmp1;
			}
			tmp1 = sibling->rb_left;
			if (!tmp1 || rb_is_black(tmp1)) {
				tmp2 = sibling->rb_right;
				if (!tmp2 || rb_is_black(tmp2)) {
					/* Case 2 - sibling color flip */
					rb_set_parent_color(sibling, parent,
							    RB_RED);
					if (rb_is_red(parent))
						rb_set_black(parent);
					else {
						node = parent;
						parent = rb_parent(node);
						if (parent)
							continue;
					}
					break;
				}
				/* Case 3 - right rotate at sibling */
				tmp1 = tmp2->rb_left;
				WRITE_ONCE(sibling->rb_right, tmp1);
				WRITE_ONCE(tmp2->rb_left, sibling);
				WRITE_ONCE(parent->rb_left, tmp2);
				if (tmp1)
					rb_set_parent_color(tmp1, sibling,
							    RB_BLACK);
				augment_rotate(sibling, tmp2);
				tmp1 = sibling;
				sibling = tmp2;
			}
			/* Case 4 - left rotate at parent + color flips */
			tmp2 = sibling->rb_right;
			WRITE_ONCE(parent->rb_left, tmp2);
			WRITE_ONCE(sibling->rb_right, parent);
			rb_set_parent_color(tmp1, sibling, RB_BLACK);
			if (tmp2)
				rb_set_parent(tmp2, parent);
			__rb_rotate_set_parents(parent, sibling, root,
						RB_BLACK);
			augment_rotate(parent, sibling);
			break;
		}
	}
}

/* Non-inline version for rb_erase_augmented() use */
void __rb_erase_color(struct rb_node *parent, struct rb_root *root, void (*augment_rotate)(struct rb_node *old, struct rb_node *new_)){
	____rb_erase_color(parent, root, augment_rotate);
}

static inline void dummy_propagate(struct rb_node *node, struct rb_node *stop) {(void)node; (void)stop;}
static inline void dummy_copy(     struct rb_node *old,  struct rb_node *new_ ) {(void)old;  (void)new_; }
static inline void dummy_rotate(   struct rb_node *old,  struct rb_node *new_ ) {(void)old;  (void)new_; }

static const struct rb_augment_callbacks dummy_callbacks = {
	dummy_propagate, dummy_copy, dummy_rotate
};

void rb_insert_color(struct rb_node *node, struct rb_root *root){
	__rb_insert(node, root, dummy_rotate);
}

void rb_erase(struct rb_node *node, struct rb_root *root){
	struct rb_node *rebalance;
	rebalance = __rb_erase_augmented(node, root, &dummy_callbacks);
	if (rebalance)
		____rb_erase_color(rebalance, root, dummy_rotate);
}

void __rb_insert_augmented(struct rb_node *node, struct rb_root *root, void (*augment_rotate)(struct rb_node *old, struct rb_node *new_)){
	__rb_insert(node, root, augment_rotate);
}

struct rb_node *rb_first(const struct rb_root *root){
	struct rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_left)
		n = n->rb_left;
	return n;
}

struct rb_node *rb_last(const struct rb_root *root){
	struct rb_node	*n;

	n = root->rb_node;
	if (!n)
		return NULL;
	while (n->rb_right)
		n = n->rb_right;
	return n;
}

struct rb_node *rb_next(const struct rb_node *node){
	struct rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return NULL;

	if (node->rb_right) {
		node = node->rb_right;
		while (node->rb_left)
			node=node->rb_left;
		return (struct rb_node *)node;
	}

	while ((parent = rb_parent(node)) && node == parent->rb_right)
		node = parent;

	return parent;
}

struct rb_node *rb_prev(const struct rb_node *node){
	struct rb_node *parent;

	if (RB_EMPTY_NODE(node))
		return NULL;

	if (node->rb_left) {
		node = node->rb_left;
		while (node->rb_right)
			node=node->rb_right;
		return (struct rb_node *)node;
	}

	while ((parent = rb_parent(node)) && node == parent->rb_left)
		node = parent;

	return parent;
}

void rb_replace_node(struct rb_node *victim, struct rb_node *new_,
		     struct rb_root *root){
	struct rb_node *parent = rb_parent(victim);

	/* Set the surrounding nodes to point to the replacement */
	__rb_change_child(victim, new_, parent, root);
	if (victim->rb_left)
		rb_set_parent(victim->rb_left, new_);
	if (victim->rb_right)
		rb_set_parent(victim->rb_right, new_);

	/* Copy the pointers/colour from the victim to the replacement */
	*new_ = *victim;
}

static struct rb_node *rb_left_deepest_node(const struct rb_node *node){
	for (;;) {
		if (node->rb_left)
			node = node->rb_left;
		else if (node->rb_right)
			node = node->rb_right;
		else
			return (struct rb_node *)node;
	}
}

struct rb_node *rb_next_postorder(const struct rb_node *node){
	const struct rb_node *parent;
	if (!node)
		return NULL;
	parent = rb_parent(node);

	/* If we're sitting on node, we've already seen our children */
	if (parent && node == parent->rb_left && parent->rb_right) {
		/* If we are the parent's left node, go to the parent's right
		 * node then all the way down to the left */
		return rb_left_deepest_node(parent->rb_right);
	} else
		/* Otherwise we are the parent's right node, and the parent
		 * should be next */
		return (struct rb_node *)parent;
}

struct rb_node *rb_first_postorder(const struct rb_root *root){
	if (!root->rb_node)
		return NULL;

	return rb_left_deepest_node(root->rb_node);
}
