#ifndef PAGER_HASHTABLE_H
#define PAGER_HASHTABLE_H

#include <assert.h>
#include <stdlib.h>

/************
 * This file contains a basic hash table implementation.
 * It has a simple interface, compatible with openssl hashtable.
 * Good enough for pager,
 * maybe in the future can be used in other components.
 *
 * Usage:
 * Start by definining what kind of data this HT contains:
    typedef whatver my_entry_type;
 * Then define two operations on this data type - hash function and equality.
 * Naming convention is important:
    unsigned long my_entry_type_hash(const my_entry_type *e) {
        return some_very_smart_hash(e);
    }
    int my_entry_type_eq(const my_entry_type *e1, const my_entry_type *e2) {
        return *t1 == *t2;
    }
 * Now declare the hash table type:
    DECLARE_HASHTABLE(my_entry_type, bits_per_hash_key);
 * And now you have the following that you can use:
    struct my_entry_type_ht; // Hash table type
    void my_entry_type_ht_init(struct my_entry_type_ht *ht); // Init, always succeeds (not much work done there)
    my_entry_type *my_entry_type_ht_retrieve(struct my_entry_type_ht *ht, my_entry_type *); // Get entry from HT or NULL
    my_entry_type *my_entry_type_ht_insert(struct my_entry_type_ht *ht, my_entry_type *); // Add entry and return 0 or
 return old entry if already there void my_entry_type_ht_free(struct my_entry_type_ht *ht); // Teardown void
 my_entry_type_ht_doall(struct my_entry_type_ht *ht, void (func*)(my_entry_type*)); // Call func on each element
 *
 * Have fun!
 ************/

#define _CAT2(x, y) x##y
#define CAT2(x, y) _CAT2(x, y)

/**
 * Reusable implementation of djb2 hash
 */
static inline unsigned long djb2(const char *str) {
	unsigned long hash = 5381;
	while (*str++)
		hash = ((hash << 5) + hash) + *str; /* hash * 33 + c */
	return hash;
}

static inline unsigned long splitmix64(unsigned long x) {
	x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9L;
	x = (x ^ (x >> 27)) * 0x94d049bb133111ebL;
	x = x ^ (x >> 31);
	return x;
}

#define WARN(condition, format, ...) ({ if (condition) {fprintf(stderr, format "\n", ##__VA_ARGS__);} })
#include "../../common/compat/kr_incs_data_structs.h"

static inline struct hlist_node *hlist_add(struct hlist_head *head, void *data) {
	struct hlist_node *node = malloc(sizeof(struct hlist_node));
	assert(node);
	node->next  = head->first;
	node->data  = data;
	head->first = node;
	return node;
}

#define hlist_foreach(iter, head) for (iter = (head)->first; iter; iter = (iter)->next)

static inline void hlist_free(struct hlist_head *head) {
	while (head->first) {
		struct hlist_node *node = head->first;
		head->first             = node->next;
		free(node);
	}
}

#define DECLARE_NUMERIC_KEY_HASH_AND_GET(typename, keyname, bits, _modifier)                                           \
	_modifier unsigned long CAT2(typename, _hash)(const typename *e) {                                                 \
		return splitmix64((unsigned long)e->keyname) & ((1L << bits) - 1);                                             \
	}                                                                                                                  \
	_modifier int CAT2(typename, _eq)(const typename *e1, const typename *e2) { return (e1->keyname) == (e2->keyname); }

#define DECLARE_NUMERIC_KEY_UTIL_FUNCTIONS(typename, keytype, keyname, _modifier)                                      \
	_modifier typename *CAT2(typename, _ht_get)(struct CAT2(typename, _ht) * ht, keytype key) {                        \
		{                                                                                                              \
			typename e        = {.keyname = key};                                                                      \
			typename *tryfind = CAT2(typename, _ht_retrieve)(ht, &e);                                                  \
			if (tryfind)                                                                                               \
				return tryfind;                                                                                        \
		}                                                                                                              \
		{                                                                                                              \
			typename *e = calloc(1, sizeof(typename));                                                                 \
			assert(e);                                                                                                 \
			e->keyname = key;                                                                                          \
			CAT2(typename, _ht_insert)(ht, e);                                                                         \
			return e;                                                                                                  \
		}                                                                                                              \
	}

#define __ht_hash(ht, e) ((ht)->hash(e) % ((sizeof(ht->htable) / sizeof(*ht->htable))))

#define INIT_HASHTABLE(typename)                                                                                       \
	(struct CAT2(typename, _ht)) { CAT2(typename, _hash), CAT2(typename, _eq) }

#define DECLARE_HASHTABLE_TYPE(typename, bits)                                                                         \
	struct CAT2(typename, _ht) {                                                                                       \
		unsigned long (*hash)(const typename *);                                                                       \
		int (*eq)(const typename *, const typename *);                                                                 \
		struct hlist_head htable[1 << (bits)];                                                                         \
	};

#define DECLARE_HASHTABLE_METHODS(typename, _modifier)                                                                 \
	void CAT2(typename, _ht_init)(struct CAT2(typename, _ht) * ht);                                                    \
	_modifier typename *CAT2(typename, _ht_retrieve)(struct CAT2(typename, _ht) * ht, typename * e);                   \
	_modifier typename *CAT2(typename, _ht_insert)(struct CAT2(typename, _ht) * ht, typename * e);                     \
	_modifier void CAT2(typename, _ht_free)(struct CAT2(typename, _ht) * ht);                                          \
	_modifier void CAT2(typename, _ht_doall)(struct CAT2(typename, _ht) * ht, void (*func)(typename *));               \
	_modifier void CAT2(typename, _ht_freeall)(struct CAT2(typename, _ht) * ht);


#define DEFINE_HASHTABLE_METHODS(typename, bits, _modifier)                                                            \
	void CAT2(typename, _ht_init)(struct CAT2(typename, _ht) * ht) { *ht = INIT_HASHTABLE(typename); }                 \
                                                                                                                       \
	_modifier typename *CAT2(typename, _ht_retrieve)(struct CAT2(typename, _ht) * ht, typename * e) {                  \
		struct hlist_node *iter;                                                                                       \
		hlist_foreach(iter, &ht->htable[__ht_hash(ht, e)]) {                                                           \
			if (ht->eq(iter->data, e))                                                                                 \
				return iter->data;                                                                                     \
		}                                                                                                              \
		return NULL;                                                                                                   \
	}                                                                                                                  \
                                                                                                                       \
	_modifier typename *CAT2(typename, _ht_insert)(struct CAT2(typename, _ht) * ht, typename * e) {                    \
		typename *prev = CAT2(typename, _ht_retrieve)(ht, e);                                                          \
		if (prev)                                                                                                      \
			return prev;                                                                                               \
		else                                                                                                           \
			hlist_add(&ht->htable[__ht_hash(ht, e)], e);                                                               \
		return NULL;                                                                                                   \
	}                                                                                                                  \
                                                                                                                       \
	_modifier void CAT2(typename, _ht_free)(struct CAT2(typename, _ht) * ht) {                                         \
		int i;                                                                                                         \
		for (i = 0; i < sizeof(ht->htable) / sizeof(*ht->htable); ++i) {                                               \
			hlist_free(&ht->htable[i]);                                                                                \
		}                                                                                                              \
	}                                                                                                                  \
                                                                                                                       \
	_modifier void CAT2(typename, _ht_doall)(struct CAT2(typename, _ht) * ht, void (*func)(typename *)) {              \
		int i;                                                                                                         \
		for (i = 0; i < sizeof(ht->htable) / sizeof(*ht->htable); ++i) {                                               \
			struct hlist_node *iter;                                                                                   \
			hlist_foreach(iter, &ht->htable[i]) { func((typename *)iter->data); }                                      \
		}                                                                                                              \
	}                                                                                                                  \
                                                                                                                       \
	_modifier void CAT2(typename, _ht_freeall)(struct CAT2(typename, _ht) * ht) {                                      \
		CAT2(typename, _ht_doall)(ht, (void (*)(typename *))free);                                                     \
	}

#define DECLARE_HASHTABLE(typename, bits)                                                                              \
	DECLARE_HASHTABLE_TYPE(typename, bits)                                                                             \
	DEFINE_HASHTABLE_METHODS(typename, bits, )
#define DECLARE_HASHTABLE_STATIC_INLINE(typename, bits)                                                                \
	DECLARE_HASHTABLE_TYPE(typename, bits)                                                                             \
	DEFINE_HASHTABLE_METHODS(typename, bits, static inline)

#endif /*PAGER_HASHTABLE_H*/