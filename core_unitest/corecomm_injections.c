/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "corecomm_injections.h"

#include <linux/hashtable.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

unsigned long __hash(const char *str) {
	unsigned long hash = 5381;
	int c;
	while ((c = *str++))
		hash = ((hash << 5) + hash) + c;
	return hash;
}

struct corecomm_inj_sym {
	char *name;
	void *val;
	struct hlist_node hlink;
};

struct corecomm_inj_sym_store {
	DECLARE_HASHTABLE(ht, 8);
};

struct corecomm_inj_sym_store *corecomm_inj_sym_store_create(void) {
	struct corecomm_inj_sym_store *store = kzalloc(sizeof(*store), GFP_KERNEL);
	BUG_ON(!store);
	hash_init(store->ht);
	return store;
}
EXPORT_SYMBOL(corecomm_inj_sym_store_create);
void corecomm_inj_sym_store_destroy(struct corecomm_inj_sym_store *store) {
	int bkt;
	struct hlist_node *tmp;
	struct corecomm_inj_sym *obj;
	if (!store) return;
	hash_for_each_safe(store->ht, bkt, tmp, obj, hlink) {
		hash_del(&obj->hlink);
		kfree(obj->val);
        kfree(obj->name);
		kfree(obj);
	}
	kfree(store);
}
EXPORT_SYMBOL(corecomm_inj_sym_store_destroy);
void *corecomm_inj_get_sym(struct corecomm_inj_sym_store *store,
                           const char *sym, int data_size) {
	struct corecomm_inj_sym *obj;
	hash_for_each_possible(store->ht, obj, hlink, __hash(sym)) {
		if (!strcmp(sym, obj->name)) { return obj->val; }
	}
	obj       = kzalloc(sizeof(*obj), GFP_KERNEL);
	obj->val  = kzalloc(data_size, GFP_KERNEL);
	obj->name = kzalloc(strlen(sym) + 1, GFP_KERNEL);
	memcpy(obj->name, sym, strlen(sym) + 1);
	BUG_ON(!obj || !obj->val || !obj->name);

	hash_add(store->ht, &obj->hlink, __hash(sym));
	return obj->val;
}
EXPORT_SYMBOL(corecomm_inj_get_sym);
void *corecomm_inj_try_get_sym(struct corecomm_inj_sym_store *store,
                               const char *sym) {
	struct corecomm_inj_sym *obj;
	hash_for_each_possible(store->ht, obj, hlink, __hash(sym)) {
		if (!strcmp(sym, obj->name)) { return obj->val; }
	}
	return NULL;
};
EXPORT_SYMBOL(corecomm_inj_try_get_sym);

/* Default store, externed by others */
struct corecomm_inj_sym_store *corecomm_inj_default_store;
EXPORT_SYMBOL(corecomm_inj_default_store);