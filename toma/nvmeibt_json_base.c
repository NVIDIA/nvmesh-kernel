/*
 * nvmeibt_json_base.c
 *
 * General-purpose JSON parsing implementation for TOMA
 * Contains the core JSON parser that can be used across all components
 *
 * Created on: Dec 12, 2025
 *     Author: wcai
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <memory.h>

#include "nvmeibt_json_base.h"

// Memory management macros for JSON parsing (uses standard malloc/free)
#define JSON_PARSING_ALLOC(name, size) 			malloc(size)
#define JSON_PARSING_CALLOC(name, size) 		calloc(1, size)
#define JSON_PARSING_REALLOC(name, ptr, size) 	realloc(ptr, size)
#define JSON_PARSING_FREE(name, ptr) 			free(ptr)

#define OUTPUT_NEXT_JSON_CHAR(name, char, output_ptr, output_len_ptr) ({		\
	if (output_ptr) {															\
		*(output_ptr) = (char);													\
		(output_ptr)++;															\
	}																			\
	(*output_len_ptr)++;														\
	TODO(N_Tf(name, "'@CHAR' output_len=@INT", char, *output_len_ptr););		\
})

#define ADVANCE_INPUT_PTR(name, _p_, _end_buf_ptr_) ({				\
	(_p_)++;														\
	if (_p_ > _end_buf_ptr_) {										\
		N_Ef(name, "@PTR > @PTR", _p_, _end_buf_ptr_);				\
		nvmeibt_abort(ES_FATAL);									\
	}																\
})

void nvmeibt_mm_json_free_kv_tree(struct mm_json_elem *root)
{
	struct mm_json_elem *next_elem = root;

	if (!root) {
		goto out_no_logs;
	}
	NFIN;
	while (next_elem) {
		struct mm_json_elem *elem = next_elem;
		switch(next_elem->type) {
		case JSON_E_NULL:
		case JSON_E_STR:
			JSON_PARSING_FREE(mem_mgmt_50, elem->str);
			next_elem = elem->parent;
			JSON_PARSING_FREE(mem_mgmt_51, elem);
			break;
		case JSON_E_NUM:
		case JSON_E_BOOL:
		case JSON_E_UNKNOWN:
			next_elem = elem->parent;
			JSON_PARSING_FREE(mem_mgmt_52, elem);
			break;
		case JSON_E_DICT:
			if (elem->dict.len == 0) {
				next_elem = elem->parent;
				JSON_PARSING_FREE(zzzz_1, elem->dict.elements);
				JSON_PARSING_FREE(mem_mgmt_53, elem);
			}
			else {
				next_elem = elem->dict.elements[elem->dict.len-1].value;
				JSON_PARSING_FREE(mem_mgmt_54, elem->dict.elements[elem->dict.len-1].key);
				elem->dict.len--;
			}
			break;
		case JSON_E_ARRAY:
			if (elem->array.len == 0) {
				next_elem = elem->parent;
				JSON_PARSING_FREE(zzzz_2, elem->array.elements);
				JSON_PARSING_FREE(mem_mgmt_55, elem);
			}
			else {
				next_elem = elem->array.elements[elem->array.len-1];
				elem->array.len--;
			}
			break;
		case JSON_E_NUM_FLOAT:
		default:
			N_Ef(cbsk48c, "elem->type=@INT", next_elem->type);
			break;
		}
	}
	NFOUT;
out_no_logs: ;
}

static int parse_JSON_str(const char *in_start, char *out_start, int *output_in_len_ptr, int *output_out_len_ptr, const char *end_buf_ptr)
{
	const char	*pi;
	char		initial_quote_char = *in_start;
	char		*po = out_start;
	int			rv = 0;

//	NFIN;
	if (initial_quote_char != '"') {
		N_Ef(mubs7j3, "String first char '@CHAR' JSON string must start with a '\"'", initial_quote_char);
		nvmeibt_abort(ES_FATAL);
	}
	pi = in_start;
	ADVANCE_INPUT_PTR(5bhxi3k, pi, end_buf_ptr);
	*output_out_len_ptr = 0;
	while (1) {
		if (*pi == initial_quote_char) {
			TODO(N_Tf(8fj3kns, "Terminating @CHAR", initial_quote_char););
			ADVANCE_INPUT_PTR(6svju2k, pi, end_buf_ptr);
			break;
		}
		if (*(uint8_t *)pi > 0x7f) {
			N_Wf(shgieu6, "The string contains a non ASCII character '@CHAR@CHAR@CHAR@CHAR'. Failing!", *pi, *(pi + 1), *(pi + 2), *(pi + 3));
			rv = -1;
			goto out;
		}
		if (*pi != '\\') {
			OUTPUT_NEXT_JSON_CHAR(cthsik3, *pi, po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(7hwikko, pi, end_buf_ptr);
			continue;
		}
		// It is a \ - Examine the next character
		// Following "node" assignment behavior as below
		//  > a="\a=\b=\c\d\e=\f=\g\h\i\j\k\l\m=\n=\o\p\q=\r=\s=\t=\u0020=\v=\w=\x0001=\y\z\A\B\C\D\E\F\G\H\I\J\K\L\M\N\O\P\Q\R\S\T\U\V\W\X\Y\Z=\"=\\=\/\'\<\>\[\]\{\}\(\)\0\1\2\3\4\5\6\7\8\9"
		//  'a=\b=cde=\f=ghijklm=\n' +
		//    `=opq=\r=s=\t= =\x0B=w=\x0001=yzABCDEFGHIJKLMNOPQRSTUVWXYZ="=\\=/'<>[]{}()\x00\x01\x02\x03\x04\x05\x06\x0789`
		// > JSON.stringify(a)
		// `"a=\\b=cde=\\f=ghijklm=\\n=opq=\\r=s=\\t= =\\u000b=w=\\u000001=yzABCDEFGHIJKLMNOPQRSTUVWXYZ=\\"=\\\\=/'<>[]{}()\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u000789"`
		ADVANCE_INPUT_PTR(nbyzo4d, pi, end_buf_ptr);
		switch (*pi) {
		case 'b':
			OUTPUT_NEXT_JSON_CHAR(5v893lk, 0x08, po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(3ujsine, pi, end_buf_ptr);
			break;
		case 'f':
			OUTPUT_NEXT_JSON_CHAR(vgdsuy3, 0x0c, po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(8hj57sw, pi, end_buf_ptr);
			break;
		case 'n':
			OUTPUT_NEXT_JSON_CHAR(ibmkdpw, '\n', po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(0mbvubr, pi, end_buf_ptr);
			break;
		case 'r':
			OUTPUT_NEXT_JSON_CHAR(gvg48k3, '\r', po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(0xbuydk, pi, end_buf_ptr);
			break;
		case 't':
			OUTPUT_NEXT_JSON_CHAR(6gs8ik3, '\t', po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(gudkw9d, pi, end_buf_ptr);
			break;
		case 'u':	// Since we output ASCII, we cannot output high UTF-8
			ADVANCE_INPUT_PTR(bhv7siw, pi, end_buf_ptr);	// the 'u'
			if (*pi == '0' && *(pi + 1) == '0') {
				ADVANCE_INPUT_PTR(efv9sik, pi, end_buf_ptr);	// Skip the \u00 prefix first 0
				ADVANCE_INPUT_PTR(1dfp8bf, pi, end_buf_ptr);	// Skip the \u00 prefix second 0
				OUTPUT_NEXT_JSON_CHAR(bie9j3c, ((*pi - '0') & 0xf) << 4 | ((*(pi + 1) - '0') & 0xf), po, output_out_len_ptr);
				ADVANCE_INPUT_PTR(aunt7ve, pi, end_buf_ptr);
				ADVANCE_INPUT_PTR(9dbjsk2, pi, end_buf_ptr);
			} else {
				N_Wf(bvhdfj3, "\\u@CHAR@CHAR is not \\u00. Failing!", *pi, *(pi + 1));
				rv = -1;
				goto out;
			}
			break;
		case '"':
		case '/':	// By JSON standard it van be escaped
		case '\\':
		case 'v':	// Explicitly, nothing special about \v
		case 'x':	// Explicitly, nothing special about \x
		default:
			OUTPUT_NEXT_JSON_CHAR(x2htz10, *pi, po, output_out_len_ptr);
			ADVANCE_INPUT_PTR(dkicwnp, pi, end_buf_ptr);
			break;
		}
	}
	*output_in_len_ptr = pi - in_start;
	if (out_start) {
		out_start[*output_out_len_ptr] = '\0';
	}
	TODO(N_Tf(i4bjhzo, "'' --> '@STR' len=(@INT,@INT) '@CHAR'", out_start, *output_in_len_ptr, *output_out_len_ptr, *(in_start + *output_in_len_ptr)););
	N_Tf(i4bjhzo, "'@STR' len=@INT", out_start, *output_out_len_ptr);
out:
	return rv;
}

struct mm_json_elem *parse_json_txt_into_kv_tree(const char *in, int buff_len)
{
	const char				*p = in;
	const char				*p_prev_iteration = (p - 1);
	struct mm_json_elem		*root = (struct mm_json_elem *)JSON_PARSING_CALLOC(mem_mgmt_20,  sizeof(struct mm_json_elem));
	struct mm_json_elem		*elem = root;
	struct mm_json_elem		*elem_prev_iteration = elem - 1;
	int						is_null_value_accepted_and_ignored = 0;
	struct mm_json_kv_pair	*cur_kv = NULL;

	NFIN;
	root->parent = NULL;
	root->type = JSON_E_UNKNOWN;

	while (p && *p) {
		if (p == p_prev_iteration && elem == elem_prev_iteration) {
			N_Ef(rasvsjh, "OOPS, No progress parsing str='@CHAR@CHAR@CHAR@CHAR...'", *p, *(p+1), *(p+2), *(p+3));
			nvmeibt_mm_json_free_kv_tree(root);
			root = NULL;
			goto out;
		}
		p_prev_iteration = p;
		elem_prev_iteration = elem;
		while ((*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',' || *p == ':') && (p < in + buff_len)) {
			p++;
		}
		// N_Tf(rcshgdsb, "@CHAR", *p);
		if (!*p || (p >= in + buff_len))
			break;
		if (strcmp(p, "null") == 0) {
			if (is_null_value_accepted_and_ignored) {
				N_Ef(ksue82j, "Expecting a value got 'null' instead");
				elem->type = JSON_E_NULL;
				elem->num = 0;
				elem->str = JSON_PARSING_ALLOC(own4x7, strlen("null") + 1);
				memcpy(elem->str, "null", strlen("null") + 1);
				elem = elem->parent;
				p += 4;
			} else {
				N_Ef(5dbzjdw, "Unexpected null");
				nvmeibt_abort(ES_FATAL);
			}
			continue;
		}
		is_null_value_accepted_and_ignored = 0;
		if (*p == '}' || *p == ']') {
			p++;
			elem = elem->parent;
			continue;
		}
		if (elem->type == JSON_E_UNKNOWN) {
			if (*p == '{') {
				p++;
				elem->type = JSON_E_DICT;
				elem->dict.len = 0;
				elem->dict.elements = NULL;
				continue;
			}
			if (*p == '[') {
				p++;
				elem->type = JSON_E_ARRAY;
				elem->array.len = 0;
				elem->array.elements = NULL;
				continue;
			}
			if (*p == '"') {
				int			src_str_len;
				int			dst_str_len;
				const char	*end_buf_ptr = in + buff_len;

				elem->type = JSON_E_STR;
				if (parse_JSON_str(p, NULL, &src_str_len, &dst_str_len, end_buf_ptr) < 0) {
					nvmeibt_mm_json_free_kv_tree(root);
					root = NULL;
					goto out;
				}
				elem->str = (char *)JSON_PARSING_ALLOC(mem_mgmt_10, dst_str_len + 1);
				parse_JSON_str(p, elem->str, &src_str_len, &dst_str_len, end_buf_ptr);
				p += src_str_len;
				// N_Tf(hfb3y5f3, "@STR", elem->str);
				elem = elem->parent;
				continue;
			}
			if (((*p>='0' && *p<='9') || *p == '-')) {
				const char		*num_str_ptr = p;
				long long int 	val = atoll(p);
				double			fval = 0;
				if (*p == '-') {
					p++;
				}
				while (*p >= '0' && *p <= '9')
					p++;
				if (*p == '.') {
					// It is a float
					fval = atof(num_str_ptr);
					p++;
					while ((*p >= '0' && *p <= '9'))
						p++;
				}
				if (*p != ' ' && *p != ',' && *p != '}' && *p != ']' && *p != '\n' && *p != '\r' && *p != '\t') {
					N_Ef(37bnai4, "OOPS, Unexpected char following an integer str='@STR'", p);
					nvmeibt_mm_json_free_kv_tree(root);
					root = NULL;
					goto out;
				}
				if (fval) {
					char	float_str_for_log[20];
					elem->type = JSON_E_NUM_FLOAT;
					elem->num_float = fval;
//					N_Ef(dffiao2, "Received a float value '@STR'", num_str_ptr);
					// A workaround
					elem->type = JSON_E_NUM;
					elem->num = (int64_t)val;
					nvmeibt_strlcpy(float_str_for_log, num_str_ptr, min(sizeof(float_str_for_log), (size_t)(p - num_str_ptr)));
					N_Tf(d4vah4m, "Converted float to key=@STR num:@STR=@INT64_TD. Ignoring the fraction part, just to survive", cur_kv->key, float_str_for_log, elem->num);
				} else {
					elem->type = JSON_E_NUM;
					elem->num = (int64_t)val;
					// N_Tf(hfb3y5f, "@LLD", elem->num);
				}
				elem = elem->parent;
				continue;
			}
			if (*p == 'n' && strncmp(p, "null", 4)==0) {
				elem->type = JSON_E_NULL;
				p += 4;
				elem = elem->parent;
				continue;
			}
			if (*p == 't' && strncmp(p, "true", 4)==0) {
				elem->type = JSON_E_BOOL;
				elem->num = 1;
				p += 4;
				elem = elem->parent;
				continue;
			}
			if (*p == 'f' && strncmp(p, "false", 5)==0) {
				elem->type = JSON_E_BOOL;
				elem->num = 0;
				p += 5;
				// N_Tf(hb3y5555f, "@LLD", elem->num);
				elem = elem->parent;
				continue;
			}
		}
		if (elem->type == JSON_E_ARRAY) {
			elem->array.len++;
			elem->array.elements = (struct mm_json_elem **)JSON_PARSING_REALLOC(mem_mgmt_01, elem->array.elements, sizeof(struct mm_json_elem *) * elem->array.len);	// Array of pointers
			elem->array.elements[elem->array.len-1] = (struct mm_json_elem *)JSON_PARSING_CALLOC(mem_mgmt_21,  sizeof(struct mm_json_elem));
			elem->array.elements[elem->array.len-1]->parent = elem;
			elem->array.elements[elem->array.len-1]->type = JSON_E_UNKNOWN;
			elem = elem->array.elements[elem->array.len-1];
			continue;
		}
		if (elem->type == JSON_E_DICT) {
			char					*end_ch = strchr(p+1, '"');
			int						len = end_ch - (p+1);

			elem->dict.len++;
			elem->dict.elements = (struct mm_json_kv_pair *)JSON_PARSING_REALLOC(mem_mgmt_02, elem->dict.elements, sizeof(struct mm_json_kv_pair) * elem->dict.len);
			//
			cur_kv = &elem->dict.elements[elem->dict.len - 1];	// Take the newly added element only after the realloc
			memset(cur_kv, 0, sizeof(*cur_kv));
			cur_kv->key = (char *)JSON_PARSING_ALLOC(mem_mgmt_11, len+1);
			memcpy(cur_kv->key, p+1, len);
			cur_kv->key[len] = 0;
			// N_Tf(hb3y55534f5f, "key=@STR", cur_kv->key);
			p += len+2;		// The surrounding two '"'
			//
			cur_kv->value = (struct mm_json_elem *)JSON_PARSING_CALLOC(mem_mgmt_22,  sizeof(struct mm_json_elem));
			cur_kv->value->type = JSON_E_UNKNOWN;
			cur_kv->value->parent = elem;
			elem = cur_kv->value;
			is_null_value_accepted_and_ignored = 1;
		}
	}
out:
	NFOUT;
	if (!root) {
		// Parsing failed - just log the error (don't send to Kafka, that's config-specific)
		N_Ef(json_parse_failed, "Failed parsing JSON input");
	}
	return root;
}

/******************************************************************************/
// JSON Serialization - Convert JSON tree back to string
/******************************************************************************/

/**
 * Serialize JSON element to string (recursive helper with indentation)
 * Returns 0 on success, -1 on error
 */
static int serialize_json_elem_recursive(struct mm_json_elem *elem, struct nvmeibt_Str *output, int indent_level)
{
	int i;
	char indent[256];

	if (!elem) {
		return -1;
	}

	// Build indentation string
	memset(indent, ' ', min(indent_level * 2, (int)sizeof(indent) - 1));
	indent[min(indent_level * 2, (int)sizeof(indent) - 1)] = '\0';

	switch (elem->type) {
	case JSON_E_STR:
		nvmeibt_Str_sprintf(output, "\"%s\"", elem->str);
		break;

	case JSON_E_NUM:
		nvmeibt_Str_sprintf(output, "%ld", elem->num);
		break;

	case JSON_E_NUM_FLOAT:
		nvmeibt_Str_sprintf(output, "%f", elem->num_float);
		break;

	case JSON_E_BOOL:
		nvmeibt_Str_sprintf(output, "%s", elem->num ? "true" : "false");
		break;

	case JSON_E_NULL:
		nvmeibt_Str_sprintf(output, "null");
		break;

	case JSON_E_DICT:
		nvmeibt_Str_sprintf(output, "{\n");
		for (i = 0; i < elem->dict.len; i++) {
			nvmeibt_Str_sprintf(output, "%s  \"%s\": ", indent, elem->dict.elements[i].key);
			serialize_json_elem_recursive(elem->dict.elements[i].value, output, indent_level + 1);
			if (i < elem->dict.len - 1) {
				nvmeibt_Str_sprintf(output, ",\n");
			} else {
				nvmeibt_Str_sprintf(output, "\n");
			}
		}
		nvmeibt_Str_sprintf(output, "%s}", indent);
		break;

	case JSON_E_ARRAY:
		nvmeibt_Str_sprintf(output, "[\n");
		for (i = 0; i < elem->array.len; i++) {
			nvmeibt_Str_sprintf(output, "%s  ", indent);
			serialize_json_elem_recursive(elem->array.elements[i], output, indent_level + 1);
			if (i < elem->array.len - 1) {
				nvmeibt_Str_sprintf(output, ",\n");
			} else {
				nvmeibt_Str_sprintf(output, "\n");
			}
		}
		nvmeibt_Str_sprintf(output, "%s]", indent);
		break;

	default:
		N_Ef(serialize_unknown_type, "Unknown JSON element type=@INT", elem->type);
		return -1;
	}

	return 0;
}

/**
 * Serialize JSON tree to string
 * Converts parsed JSON tree back to JSON string with proper formatting
 * Output is appended to the provided nvmeibt_Str
 * Returns 0 on success, -1 on error
 */
int serialize_json_tree_to_str(struct mm_json_elem *root, struct nvmeibt_Str *output)
{
	if (!root || !output) {
		return -1;
	}

	return serialize_json_elem_recursive(root, output, 0);
}

/******************************************************************************/
// JSON Query Functions - For querying already-parsed JSON trees
/******************************************************************************/

/**
 * Get value element for a key from a dict element
 * Returns the value element, or NULL if key not found or not a dict
 */
struct mm_json_elem *json_get_dict_value(struct mm_json_elem *dict_elem, const char *key)
{
	int		i;
	if (!dict_elem || dict_elem->type != JSON_E_DICT || !key) {
		return NULL;
	}
	for (i = 0; i < dict_elem->dict.len; i++) {
		if (strcmp(dict_elem->dict.elements[i].key, key) == 0) {
			return dict_elem->dict.elements[i].value;
		}
	}
	return NULL;
}

bool json_get_dict_bool(struct mm_json_elem *dict_elem, const char *key, bool default_val)
{
	struct mm_json_elem *value = json_get_dict_value(dict_elem, key);
	if (value && value->type == JSON_E_BOOL) {
		return (value->num != 0);
	}
	return default_val;
}

const char *json_get_dict_str(struct mm_json_elem *dict_elem, const char *key, const char *default_val)
{
	struct mm_json_elem *value = json_get_dict_value(dict_elem, key);
	if (value && value->type == JSON_E_STR) {
		return value->str;
	}
	return default_val;
}

int64_t json_get_dict_num(struct mm_json_elem *dict_elem, const char *key, int64_t default_val)
{
	struct mm_json_elem *value = json_get_dict_value(dict_elem, key);
	if (value && value->type == JSON_E_NUM) {
		return value->num;
	}
	return default_val;
}

/******************************************************************************/
// JSON Modification Functions - For modifying values in already-parsed trees
/******************************************************************************/

/**
 * Set boolean value for a key in dict element
 * Returns 0 on success, -1 if key not found
 */
int json_set_dict_bool(struct mm_json_elem *dict_elem, const char *key, bool value)
{
	struct mm_json_elem *elem = json_get_dict_value(dict_elem, key);
	if (!elem) {
		return -1;
	}
	elem->type = JSON_E_BOOL;
	elem->num = value ? 1 : 0;
	return 0;
}

/**
 * Set string value for a key in dict element
 * Returns 0 on success, -1 if key not found
 * Note: Frees old string and allocates new one
 */
int json_set_dict_str(struct mm_json_elem *dict_elem, const char *key, const char *value)
{
	struct mm_json_elem *elem = json_get_dict_value(dict_elem, key);
	if (!elem) {
		return -1;
	}

	// Free old string if it was a string type
	if (elem->type == JSON_E_STR && elem->str) {
		free(elem->str);
	}

	elem->type = JSON_E_STR;
	elem->str = strdup(value);
	return 0;
}

/**
 * Set numeric value for a key in dict element
 * Returns 0 on success, -1 if key not found
 */
int json_set_dict_num(struct mm_json_elem *dict_elem, const char *key, int64_t value)
{
	struct mm_json_elem *elem = json_get_dict_value(dict_elem, key);
	if (!elem) {
		return -1;
	}
	elem->type = JSON_E_NUM;
	elem->num = value;
	return 0;
}
