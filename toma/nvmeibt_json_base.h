/*
 * nvmeibt_json_base.h
 *
 * General-purpose JSON parsing infrastructure for TOMA
 * This contains the core JSON data structures and parsing macros
 * that can be used across all TOMA components.
 *
 * Created on: Dec 12, 2025
 *     Author: wcai
 */

#ifndef TOMA_NVMEIBT_JSON_BASE_H_
#define TOMA_NVMEIBT_JSON_BASE_H_

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"

/******************************************************************************/
// Core JSON Data Structures
/******************************************************************************/

struct mm_json_elem;

struct mm_json_kv_pair {
	char					*key;
	struct mm_json_elem		*value;
};

struct mm_json_dict {
	int							len;
	struct mm_json_kv_pair		*elements;
};

struct mm_json_array {
	int							len;
	struct mm_json_elem			**elements;
};

enum mm_json_type {
	JSON_E_UNKNOWN,
	JSON_E_STR,
	JSON_E_NUM,
	JSON_E_NUM_FLOAT,
	JSON_E_DICT,
	JSON_E_ARRAY,
	JSON_E_BOOL,
	JSON_E_NULL
};

struct mm_json_elem {
	enum mm_json_type		type;
	struct mm_json_elem		*parent;
	union {
		char				*str;
		int64_t				num;
		double				num_float;
		struct mm_json_dict	dict;
		struct mm_json_array	array;
	};
};

/******************************************************************************/
// Core JSON Parsing Functions
/******************************************************************************/

void nvmeibt_mm_json_free_kv_tree(struct mm_json_elem *root);
struct mm_json_elem *parse_json_txt_into_kv_tree(const char *in, int buff_len);
int serialize_json_tree_to_str(struct mm_json_elem *root, struct nvmeibt_Str *output);

/******************************************************************************/
// JSON Query Functions (for querying already-parsed trees)
/******************************************************************************/

struct mm_json_elem *json_get_dict_value(struct mm_json_elem *dict_elem, const char *key);
bool json_get_dict_bool(struct mm_json_elem *dict_elem, const char *key, bool default_val);
const char *json_get_dict_str(struct mm_json_elem *dict_elem, const char *key, const char *default_val);
int64_t json_get_dict_num(struct mm_json_elem *dict_elem, const char *key, int64_t default_val);

/******************************************************************************/
// JSON Modification Functions (for modifying already-parsed trees)
/******************************************************************************/

int json_set_dict_bool(struct mm_json_elem *dict_elem, const char *key, bool value);
int json_set_dict_str(struct mm_json_elem *dict_elem, const char *key, const char *value);
int json_set_dict_num(struct mm_json_elem *dict_elem, const char *key, int64_t value);

/******************************************************************************/
// JSON Parsing Macros - Reusable across all files that need JSON parsing
/******************************************************************************/

struct JSON_idx_token {
	bool		is_found;
	bool		is_avoid_warning_when_missing;		// The name is "Negative" for easy init to 0
	char		*token;
};

#define JSON_ASSIGN_AND_CALL_INIT()							\
	struct JSON_idx_token		JSON_ARR[50] = {0};			\
	int							json_n = 0;					\
	bool						is_found = 0;				\
	int							__json_iter;				\
	int							n_json_tokens = 0;

#define JSON_LOOP_FOR_DICT(__kv, __dict)																				\
	for (__json_iter = 0; ((__kv) = &(__dict)->elements[__json_iter]) && __json_iter < (__dict)->len; __json_iter++)

#define JSON_ASSIGN_AND_CALL_VALIDATE(name) ({																\
	for (__json_iter = 0; __json_iter < n_json_tokens; __json_iter++) {										\
		if (!JSON_ARR[__json_iter].is_found) {																\
			if (JSON_ARR[__json_iter].is_avoid_warning_when_missing) {										\
				N_Tf(name ## T1, "Missing token n=@INT '@STR'", __json_iter, JSON_ARR[__json_iter].token);	\
			} else {																						\
				N_WTf(name ## W1, "Missing token n=@INT '@STR'", __json_iter, JSON_ARR[__json_iter].token);	\
			}																								\
		}																									\
	}																										\
})

#define JSON_LOOP_ITERATION_START(__name, __JSON_token)							\
	is_found = 0;																\
	json_n = 0;																	\
	/* DUMP_kv_TO_LOG(__name ## KV, kv); */

#define JSON_LOOP_ITERATION_END(__name, __JSON_token)							\
	if (!is_found) {															\
		N_WTf(__name ## 1, "Extra key '@STR'", (__JSON_token));					\
	}

// The PROLOGUE handle the case where the input matches the token
#define JSON_ASSIGN_PROLOGUE(__name, __JSON_token)							\
	if (!strcmp(kv->key, (__JSON_token))) {									\
		if (JSON_ARR[json_n].is_found) {									\
			N_ETf(__name ## 1, "key @STR already exists", (__JSON_token));	\
		}																	\
		JSON_ARR[json_n].is_found = 1;										\
		is_found = 1;

// The Epilogue is mostly for updating the JSON_ARR of possible tokens
#define JSON_ASSIGN_EPILOGUE(__name, __JSON_token)								\
	}																			\
	if (__json_iter == 0) {														\
		JSON_ARR[json_n].token = (__JSON_token);								\
		/*N_Tf(__name ## 3, "token[@INT]=@STR", json_n, JSON_ARR[json_n].token);*/	\
		n_json_tokens++;														\
	}																			\
	json_n++;


#define JSON_ASSIGN_PLAIN(name, JSON_token, JSON_dst, JSON_src) ({				\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))									\
	(JSON_dst) = (JSON_src);													\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))									\
})

#define JSON_ASSIGN_PLAIN_OPTIONAL(name, JSON_token, JSON_dst, JSON_src) ({		\
	JSON_ARR[json_n].is_avoid_warning_when_missing = 1;							\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))									\
	(JSON_dst) = (JSON_src);													\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))									\
})

#define JSON_ASSIGN_STR(name, JSON_token, JSON_dst, JSON_src) ({				\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))									\
	nvmeibt_strlcpy((JSON_dst), (JSON_src), sizeof(JSON_dst));					\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))									\
})

#define JSON_ASSIGN_CALL(name, JSON_token, JSON_func, JSON_args...) ({			\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))									\
	JSON_func(JSON_args);														\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))									\
})

#define JSON_ASSIGN_OPTIONAL(name, JSON_token) ({			\
	JSON_ARR[json_n].is_avoid_warning_when_missing = 1;		\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))				\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))				\
})

#define JSON_ASSIGN_VALIDATE_STR(name, JSON_token, JSON_expected, JSON_src) ({	\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))									\
	if (strcmp(JSON_expected, JSON_src)) {										\
		N_Wf(name ## validate, "OOPS @STR='@STR'", JSON_token, JSON_src);		\
	}																			\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))									\
})

#define JSON_ASSIGN_VALIDATE_STR_OPTIONAL(name, JSON_token, JSON_expected, JSON_src) ({	\
	JSON_ARR[json_n].is_avoid_warning_when_missing = 1;									\
	JSON_ASSIGN_PROLOGUE(name, (JSON_token))											\
	if (strcmp(JSON_expected, JSON_src)) {												\
		N_Wf(name ## validate, "OOPS @STR='@STR'", JSON_token, JSON_src);				\
	}																					\
	JSON_ASSIGN_EPILOGUE(name, (JSON_token))											\
})

#define JSON_WARN_and_FIX(name, __relevant_key, __var, __badval, __fix, __log_args...) ({	\
	if ((__var) == (__badval) && (!strcmp(kv->key, __relevant_key))) {						\
		N_Wf(name ## warn_once, __log_args);												\
		(__var) = (__fix);																	\
	}																						\
})

#define DUMP_kv_TO_LOG(__name, __kv) ({											\
	struct mm_json_elem			*__v = (__kv)->value;							\
	char						*__key = (__kv)->key;							\
	if (!__key) N_Tf(__name ## 20, "__key=NULL");								\
	if (__v->type == JSON_E_NUM) {												\
		N_Tf(__name ## 11, "'@STR' num=@INT64_TD", __key, __v->num);			\
	} else if (__v->type == JSON_E_STR) {										\
		N_Tf(__name ## 12, "'@STR' str='@STR'", __key, __v->str);				\
	} else if (__v->type == JSON_E_DICT) {										\
		N_Tf(__name ## 13, "'@STR' dict.len=@INT", __key, __v->dict.len);		\
	} else if (__v->type == JSON_E_ARRAY) {										\
		N_Tf(__name ## 14, "'@STR' array.len=@INT", __key, __v->array.len);		\
	} else if (__v->type == JSON_E_NUM_FLOAT) {									\
		N_Tf(__name ## 15, "'@STR' (FLOAT)", __key);							\
	} else if (__v->type == JSON_E_BOOL) {										\
		N_Tf(__name ## 16, "'@STR' bool=@INT64_TD", __key, __v->num);			\
	} else if (__v->type == JSON_E_NULL) {										\
		N_Tf(__name ## 17, "'@STR' JSON_E_NULL", __key);						\
	} else if (__v->type == JSON_E_UNKNOWN) {									\
		N_Tf(__name ## 18, "'@STR' JSON_E_UNKNOWN", __key);						\
	}																			\
})

#endif /* TOMA_NVMEIBT_JSON_BASE_H_ */

