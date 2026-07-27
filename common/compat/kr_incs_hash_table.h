#ifndef KR_INCS_HASH_TABLE_H
#define KR_INCS_HASH_TABLE_H

/* Substitute for missing hashtable.h - It's basically an array of hlist */
#include <linux/hash.h>
#include <linux/list.h>

#define hash_init(tbl)\
	do {\
		unsigned int i;\
		for (i = 0; i < ARRAY_SIZE(tbl); ++i)\
			INIT_HLIST_HEAD(tbl +i);\
	} while(0)

#define hash_add(tbl,node,key)\
	do {\
		struct hlist_head *head = tbl + \
			hash_64(key, ilog2(ARRAY_SIZE(tbl)));\
		hlist_add_head(node,head);\
	} while (0)

#define hash_del(node)\
	hlist_del_init(node)

#define ___hlist_entry_safe(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
		____ptr ? hlist_entry(____ptr, type, member) : NULL; \
	})

#define ___hlist_for_each_entry_safe(pos, n, head, member) \
	for (pos = ___hlist_entry_safe((head)->first, typeof(*pos), member); \
		pos && ({ n = pos->member.next; 1; }); \
		pos = ___hlist_entry_safe(n, typeof(*pos), member))

#define __hash_for_each_safe__(_name_, _bkt_, __node__, _tmp_, _obj_, _member_)	\
	hash_for_each_safe(_name_, _bkt_, __node__, _tmp_, _obj_, _member_)

#define hash_for_each_safe(tbl, bkt, pos, tmp, obj, node) \
	pos = NULL; \
	for (bkt = 0, obj = NULL; obj == NULL && bkt < ARRAY_SIZE(tbl); bkt++) \
		___hlist_for_each_entry_safe(obj, tmp, tbl+bkt, node)

#define __hash_for_each_possible_safe__(tbl,obj,pos,tmp,node,key)\
	hlist_for_each_entry_safe(obj,pos,tmp,\
		tbl + hash_64(key, ilog2(ARRAY_SIZE(tbl))), node)


#define __hlist_entry_safe__(ptr, type, member) \
	({ typeof(ptr) ____ptr = (ptr); \
	   ____ptr ? hlist_entry(____ptr, type, member) : NULL; \
	})

#define __hlist_for_each_entry__(pos, head, member)				\
	for (pos = __hlist_entry_safe__((head)->first, typeof(*(pos)), member);\
	     pos;							\
	     pos = __hlist_entry_safe__((pos)->member.next, typeof(*(pos)), member))

#define hash_for_each_possible(tbl, obj, member, key) \
	__hlist_for_each_entry__(obj, tbl + hash_64(key, ilog2(ARRAY_SIZE(tbl))), member)

#define hash_for_each(name, bkt, obj, member)				\
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < ARRAY_SIZE(name);\
			(bkt)++)\
		__hlist_for_each_entry__(obj, &name[bkt], member)

static inline bool __hash_empty(struct hlist_head *ht, unsigned int sz)
{
	unsigned int i;

	for (i = 0; i < sz; i++)
		if (!hlist_empty(&ht[i]))
			return false;

	return true;
}

#define hash_empty(name) __hash_empty(name, ARRAY_SIZE(name))

#define DEFINE_HASHTABLE(name, bits)		\
	struct hlist_head name[1 << (bits)] =	\
			{ [0 ... ((1 << (bits)) - 1)] = HLIST_HEAD_INIT }

#define DECLARE_HASHTABLE(name, bits)	struct hlist_head name[1 << (bits)]

#endif