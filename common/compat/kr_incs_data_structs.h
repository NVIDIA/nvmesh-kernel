#ifndef KERNEL_DATA_STRUCTS_H
#define KERNEL_DATA_STRUCTS_H
#ifndef __KERNEL__
	// Kernel already has those functions. Define as compatibility for user-space
	#include "kr_incs_types.h"
	#include <stddef.h>	// typeof

// C style double connected linked list (Queue)
// @ptr:	the list head to take the element from. @pos:    the type * to cursor. @type:	the type of the struct this is embedded in. @member:	the name of the list_struct within the struct. Note, that list is expected to be not empty.
struct list_head { struct list_head *next, *prev; };
#define LIST_HEAD_INIT(name) { &(name), &(name) }
#if !defined(LIST_HEAD)
	#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)
#endif

static inline void INIT_LIST_HEAD(struct list_head *list){ list->next = list->prev = list; }
static inline int list_empty(const struct list_head *head){ return head->next == head; }
static inline int list_is_last(const struct list_head *list,const struct list_head *head){return list->next == head;}
static inline int list_is_singular(const struct list_head *head){ return !list_empty(head) && (head->next == head->prev);}
static inline void __list_splice(const struct list_head *list,struct list_head *prev,struct list_head *next) {
	struct list_head *first = list->next;
	struct list_head *last = list->prev;
	first->prev = prev;
	prev->next = first;
	last->next = next;
	next->prev = last;
}
static inline void list_splice(const struct list_head *list, struct list_head *head) {
	if (!list_empty(list))
		__list_splice(list, head, head->next);
}
static inline void list_splice_init(struct list_head *list, struct list_head *head) {
	if (!list_empty(list)) {
		__list_splice(list, head, head->next);
		INIT_LIST_HEAD(list);
	}
}
static inline void list_splice_tail_init(struct list_head *list, struct list_head *head) {
	if (!list_empty(list)) {
		__list_splice(list, head->prev, head);
		INIT_LIST_HEAD(list);
	}
}

static inline void __list_del(     struct list_head * prev, struct list_head * next){ next->prev = prev; prev->next = next; }
static inline void __list_del_entry(struct list_head *entry){ __list_del(entry->prev, entry->next); }
			  void   list_del(     struct list_head *entry);
static inline void   list_del_init(struct list_head *entry){ __list_del_entry(entry); INIT_LIST_HEAD(entry);}
			  bool is_in_list(     const struct list_head *entry);		// Daniel's debug method. Return true if this entry is inside a list. If it was deleted or just initialized returns 0
			  void   list_add(     struct list_head *New, struct list_head *head);
			  void list_add_tail(  struct list_head *New, struct list_head *head);
static inline void list_move(      struct list_head *New, struct list_head *head){ __list_del_entry(New); list_add(     New, head); }
static inline void list_move_tail( struct list_head *New, struct list_head *head){ __list_del_entry(New); list_add_tail(New, head); }
			  int list_calc_size(const struct list_head *head);
#define list_entry(ptr, type, member) container_of(ptr, type, member)
#define list_last_entry(         ptr, type, member) list_entry((ptr)->prev, type, member)
#define list_first_entry(ptr, type, member) list_entry((ptr)->next, type, member)
#define list_first_entry_or_null(ptr, type, member) (!list_empty(ptr) ? list_first_entry(ptr, type, member) : (type*)NULL)
#define list_next_entry(               pos, member) list_entry((pos)->member.next, typeof(*(pos)), member)
#define list_prev_entry(               pos, member) list_entry((pos)->member.prev, typeof(*(pos)), member)
#define list_for_each_entry(pos, head, member)	       for (pos = list_entry((head)->next, typeof(*pos), member);  														  &pos->member != (head); pos        = list_entry(pos->member.next, typeof(*pos), member))
#define list_for_each_entry_safe(pos, n, head, member) for (pos = list_entry((head)->next, typeof(*pos), member), n = list_entry(pos->member.next, typeof(*pos), member); &pos->member != (head); pos = n, n = list_entry(pos->member.next, typeof(*pos), member))
#define list_for_each(pos, head) 					   for (pos = (head)->next; pos != (head); pos = pos->next)
#define list_for_each_safe(pos, n, head) 			   for (pos = (head)->next, n = pos->next; pos != (head); pos = n, n = pos->next)

/* priv: private data, opaque to list_sort(), passed to @cmp. @head: the list to sort @cmp: the elements comparison function. This function implements "merge sort", which has O(nlog(n)) complexity. */
void list_sort(void *priv, struct list_head *head, int (*cmp)(void *priv, struct list_head *a, struct list_head *b));

			  void __list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry);
static inline void   list_cut_position(struct list_head *list, struct list_head *head, struct list_head *entry) { /* Move all elements up to (including entry) from 'head; to 'list' */
	if (list_empty(head)) return;
	if (list_is_singular(head) && (head->next != entry && head != entry)) return;
	if (entry == head) INIT_LIST_HEAD(list);
	else __list_cut_position(list, head, entry);
}

struct llist_head {
	struct llist_node *first;
};

struct llist_node {
	struct llist_node *next;
};

inline static void init_llist_head(struct llist_head *list)
{
	list->first = NULL;
}

#define member_address_is_nonnull(ptr, member)	\
	((uintptr_t)(ptr) + offsetof(typeof(*(ptr)), member) != 0)

#define llist_entry(ptr, type, member)	container_of(ptr, type, member)

#define llist_for_each_safe(pos, n, node)			\
	for ((pos) = (node); (pos) && ((n) = (pos)->next, true); (pos) = (n))

#define llist_for_each_entry(pos, node, member)				\
	for ((pos) = llist_entry((node), typeof(*(pos)), member);	\
	     member_address_is_nonnull(pos, member);			\
	     (pos) = llist_entry((pos)->member.next, typeof(*(pos)), member))

#define llist_for_each_entry_safe(pos, n, node, member)			       \
	for (pos = llist_entry((node), typeof(*pos), member);		       \
	     member_address_is_nonnull(pos, member) &&			       \
	        (n = llist_entry(pos->member.next, typeof(*n), member), true); \
	     pos = n)

inline static bool llist_empty(const struct llist_head *head)
{
	struct llist_node *first = __atomic_load_n((struct llist_node * volatile *)&head->first, __ATOMIC_RELAXED);

	return first == NULL;
}

inline static struct llist_node *llist_next(struct llist_node *node)
{
	return node->next;
}

bool llist_add_batch(struct llist_node *new_first,
			    struct llist_node *new_last,
			    struct llist_head *head);

static inline bool __llist_add_batch(struct llist_node *new_first,
				     struct llist_node *new_last,
				     struct llist_head *head)
{
	new_last->next = head->first;
	head->first = new_first;
	return new_last->next == NULL;
}

static inline bool __llist_add(struct llist_node *new, struct llist_head *head)
{
	return __llist_add_batch(new, new, head);
}

inline static bool llist_add(struct llist_node *new, struct llist_head *head)
{
	return llist_add_batch(new, new, head);
}

static inline struct llist_node *llist_del_all(struct llist_head *head)
{
	return __atomic_exchange_n((void *volatile *)&head->first, NULL, __ATOMIC_ACQUIRE);
}

static inline struct llist_node *__llist_del_all(struct llist_head *head)
{
	struct llist_node *first = head->first;
	
	head->first = NULL;
	return first;
}

// Other lists, not supported yet (copy from linux/types.h)
struct hlist_head { struct hlist_node *first; };
#ifndef PAGER_APP
	struct hlist_node { struct hlist_node *next, **pprev; };
#else
	struct hlist_node { void *data; struct hlist_node *next; }; // Yura's Nudelman Hack, Todo, fix
#endif

#endif // ! __KERNEL__

/******************************************************/
#if KS_RBTREE
	#include <linux/rbtree.h>		// Kernel code + has rbtree implamantation
#else
	struct rb_node {
		unsigned long  __rb_parent_color;
		struct rb_node *rb_right, *rb_left;
	} __attribute__((aligned(sizeof(long)))); 	// The alignment might seem pointless, but allegedly CRIS needs it
	struct rb_root { struct rb_node *rb_node; };
	#define rb_parent(r)   ((struct rb_node *)((r)->__rb_parent_color & ~3))
	#define RB_ROOT (struct rb_root) { NULL, }
	#define	rb_entry(ptr, type, member) container_of(ptr, type, member)
	#define RB_EMPTY_ROOT(root)  ((root)->rb_node == NULL)
	#define RB_EMPTY_NODE(node)  ((node)->__rb_parent_color == (unsigned long)(node))				// 'empty' nodes are nodes that are known not to be inserted in an rbree */
	#define RB_CLEAR_NODE(node)  ((node)->__rb_parent_color =  (unsigned long)(node))
	void     rb_insert_color(      struct rb_node *n, struct rb_root *r);
	void		  rb_erase(        struct rb_node *n, struct rb_root *r);
	struct rb_node *rb_next( const struct rb_node *n );
	struct rb_node *rb_prev( const struct rb_node *n );
	struct rb_node *rb_first(const struct rb_root *n );
	struct rb_node *rb_last( const struct rb_root *n );
	struct rb_node *rb_first_postorder(const struct rb_root *); /* Postorder iteration - always visit the parent after its children */
	struct rb_node *rb_next_postorder( const struct rb_node *);
	void rb_replace_node(          struct rb_node *old, struct rb_node *n, struct rb_root *r);
	static inline void rb_link_node(             struct rb_node *n, struct rb_node *parent, struct rb_node ** rb_link){
		n->__rb_parent_color = (unsigned long)parent;
		n->rb_left = n->rb_right = NULL;
		*rb_link = n;
	}

	#define rb_entry_safe(ptr, type, member) ({ typeof(ptr) ____ptr = (ptr); ____ptr ? rb_entry(____ptr, type, member) : NULL; 	})
	#define rbtree_postorder_for_each_entry_safe(pos, n, root, field) \
		for (pos = rb_entry_safe(rb_first_postorder(root), typeof(*pos), field); \
			pos && ({ n = rb_entry_safe(rb_next_postorder(&pos->field), typeof(*pos), field); 1; }); \
			pos = n)
#endif
#endif // KERNEL_DATA_STRUCTS_H
