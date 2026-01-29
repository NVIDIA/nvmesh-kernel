/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CORECOMM_INJECTIONS_H
#define CORECOMM_INJECTIONS_H

struct corecomm_inj_sym_store;

extern struct corecomm_inj_sym_store *corecomm_inj_sym_store_create(void);
extern void
corecomm_inj_sym_store_destroy(struct corecomm_inj_sym_store *store);
extern void *corecomm_inj_get_sym(struct corecomm_inj_sym_store *store,
                                  const char *sym, int data_size);
extern void *corecomm_inj_try_get_sym(struct corecomm_inj_sym_store *store,
                                      const char *sym);

#ifdef CORE_UNITEST
#define corecomm_inj_var(store_, type_, name_, default_)                       \
	({                                                                         \
		static void *__sym_ptr = NULL;                                         \
		extern struct corecomm_inj_sym_store *corecomm_inj_default_store;      \
		struct corecomm_inj_sym_store *__store =                               \
		    store_ ? store_ : corecomm_inj_default_store;                      \
		if (!__store) {                                                        \
			corecomm_inj_default_store = corecomm_inj_sym_store_create();      \
			__store                    = corecomm_inj_default_store;           \
		}                                                                      \
		if (!__sym_ptr) {                                                      \
			__sym_ptr = corecomm_inj_try_get_sym(__store, #name_);             \
			if (!__sym_ptr) {                                                  \
				__sym_ptr =                                                    \
				    corecomm_inj_get_sym(__store, #name_, sizeof(type_));      \
				*(type_ *)__sym_ptr = default_;                                \
			}                                                                  \
		}                                                                      \
		*(type_ *)__sym_ptr;                                                   \
	})
#define corecomm_inj_code(...) __VA_ARGS__
#define corecomm_inj_code_else(injcode, notestcode) injcode
#else
#define corecomm_inj_code(...)
#define corecomm_inj_code_else(injcode, notestcode) notestcode
#define corecomm_inj_var(store_, type_, name_, default_) default_
#endif

#endif /*CORECOMM_INJECTIONS_H*/