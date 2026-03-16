/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "formatter.h"
#include <stdio.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <json-c/json.h>
#include <string.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <endian.h>

immutable_string_t SYSTEM_TRACE_NAME = {
	"SYSTEM_TRACE"
};

immutable_string_t SYSTEM_TRACE_AGAIN_NAME = {
	"SYSTEM_TRACE_AGAIN"
};

immutable_string_t SYSTEM_TRACE_GREETING_NAME = {
	"SYSTEM_TRACE_GREETING"
};

trace_entry_t SYSTEM_TRACE = {
    /* Special message by pager */
    .trace_id   = SYSTEM_TRACE_ID, /* Trace ID */
    .severity   = SEV_SYSTEM,
    .trace_name = &SYSTEM_TRACE_NAME};

trace_entry_t SYSTEM_TRACE_AGAIN = {
    /* Special message used by pager to indicate its previous messages are the same */
    .trace_id   = SYSTEM_TRACE_AGAIN_ID, /* Trace ID */
    .severity   = SEV_SYSTEM,
    .trace_name = &SYSTEM_TRACE_AGAIN_NAME};

trace_entry_t SYSTEM_TRACE_GREETING = {
    /* Special message to indicate start of buffer */
    .trace_id   = SYSTEM_TRACE_GREETING_ID, /* Trace ID */
    .severity   = SEV_SYSTEM,
    .trace_name = &SYSTEM_TRACE_GREETING_NAME};



/* Special Runtime traces*/
immutable_string_t SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF_NAME = {
	"SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF"
};

trace_entry_t SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF = {
    /* Special message by pager */
    .trace_id   = SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF_ID, /* Trace ID */
    .severity   = SEV_SYSTEM,
    .trace_name = &SYSTEM_TRACE_NAME,
    .fmt = "PAGER MSG: Application tried to write too long buffer, that was discarded\n"};

/* Declare trace_entry_htable_t */
unsigned long trace_entry_t_hash(const trace_entry_t *tohash) { return tohash->trace_id; }
int trace_entry_t_eq(const trace_entry_t *arg1, const trace_entry_t *arg2) { return arg1->trace_id == arg2->trace_id; }
DECLARE_HASHTABLE(trace_entry_t, 16);


unsigned long immutable_string_t_hash(const immutable_string_t *te) { // djb2
	return djb2(te->token);
}
int immutable_string_t_eq(const immutable_string_t *t1, const immutable_string_t *t2) {
	return !strcmp(t1->token, t2->token);
}
DECLARE_HASHTABLE(immutable_string_t, 16);
/* End trace_entry_htable_t */

struct dict {
	unsigned int cksum; /*MUST BE FIRST (performance): HASH KEY - unique identifier of a dictionary*/

	struct trace_entry_t_ht ht;
	struct json_object *root_object;
	struct json_object *dictionary;
	struct json_object *nvmeibx_json;

	void *fmtlib; /*@todo: Refactor, there is no reason for fmtlib to be under dictionary except historical*/
};


int fmt_bitmap(char *buf, int len, long ptr, int datalen) {
	int i;
	int first = 1;
	int bit;
	int prevbit = 0;
	int seq_start = 0;
	const char *msg = (const char *)ptr;
	const char *orig = buf;
	for (i = 0; i <= 8 * datalen; i++) {
		if (i == 8 * datalen)
			bit = 0;
		else
			bit = (msg[i >> 3] >> (i & 7)) & 1;
		if (bit != prevbit) {
			if (bit == 1) {
				if (!first){
					*buf++ = ',';
					--len;
				}
				else
					first = 0;
				seq_start = i;
				buf += snprintf(buf, len - (buf - orig + 1), "%d", i);
			} else {
				if (i > seq_start + 1)
					buf += snprintf(buf, len - - (buf - orig + 1), "-%d", i - 1);
			}
			prevbit = bit;
		}
	}
	*buf = '\0';
	return buf - orig + 1;
}

int fmt_array_u64(char *buf, int len, long ptr, int datalen) {
	const unsigned long long *arr = (const unsigned long long *)ptr;
	const unsigned int n = datalen / (int)sizeof(unsigned long long);
	int count = 0;

	count += snprintf(buf, len, "[");
	for (unsigned int i = 0; i < n; i++) {
		if (i > 0)
			count += snprintf(buf + count, len - count, ",");
		count += snprintf(buf + count, len - count, "%llu", arr[i]);
	}
	count += snprintf(buf + count, len - count, "]");
	return count;
}

typedef enum { FLEX_FMT_UNSIGNED, FLEX_FMT_SIGNED, FLEX_FMT_HEX } flex_elem_fmt_t;

static int fmt_array_flex_generic(char *buf, int len, long ptr, unsigned int elem_size, flex_elem_fmt_t fmt_type)
{
	const unsigned char *data = (const unsigned char *)ptr;
	unsigned int n = le32toh(*(const unsigned int *)data);
	const unsigned char *arr = data + sizeof(unsigned int);
	int count = 0;

	count += snprintf(buf, len, "[");
	for (unsigned int i = 0; i < n; i++) {
		unsigned long long uval = 0;

		if (i > 0)
			count += snprintf(buf + count, len - count, ",");

		memcpy(&uval, arr + i * elem_size, elem_size);

		switch (fmt_type) {
		case FLEX_FMT_UNSIGNED:
			count += snprintf(buf + count, len - count, "%llu", uval);
			break;
		case FLEX_FMT_SIGNED: {
			long long sval = (elem_size == 4) ? (int)(unsigned int)uval : (long long)uval;
			count += snprintf(buf + count, len - count, "%lld", sval);
			break;
		}
		case FLEX_FMT_HEX:
			count += snprintf(buf + count, len - count, "0x%llx", uval);
			break;
		}
	}
	count += snprintf(buf + count, len - count, "]");
	return count;
}

int fmt_array_u64_flex(char *buf, int len, long ptr, int datalen) {
	return fmt_array_flex_generic(buf, len, ptr, sizeof(unsigned long long), FLEX_FMT_UNSIGNED);
}

int fmt_array_u32_flex(char *buf, int len, long ptr, int datalen) {
	return fmt_array_flex_generic(buf, len, ptr, sizeof(unsigned int), FLEX_FMT_UNSIGNED);
}

int fmt_array_int_flex(char *buf, int len, long ptr, int datalen) {
	return fmt_array_flex_generic(buf, len, ptr, sizeof(int), FLEX_FMT_SIGNED);
}

int fmt_array_ptr_flex(char *buf, int len, long ptr, int datalen) {
	return fmt_array_flex_generic(buf, len, ptr, sizeof(unsigned long long), FLEX_FMT_HEX);
}

int fmt_hex(char *buf, int len, long ptr, int datalen) {
	static char hex_asc[17] = "0123456789abcdef";
	int i;
	const char *msg = (const char *)ptr;
	const char *orig = buf;
	(void)len;

	for (i = 0; i < datalen; i++) {
		if (i)
			*buf++ = ' ';
		*buf++ = hex_asc[(msg[i] >> 4) & 0xf];
		*buf++ = hex_asc[msg[i] & 0xf];
	}
	*buf = '\0';
	return buf - orig + 1;
}

int fmt_uuid_le(char *buf, int len, long ptr, int datalen) {
	const unsigned char *uuid = (const unsigned char *)ptr;
	return snprintf(buf, len, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", uuid[3], uuid[2],
	                 uuid[1], uuid[0], uuid[5], uuid[4], uuid[7], uuid[6], uuid[8], uuid[9], uuid[10], uuid[11],
	                 uuid[12], uuid[13], uuid[14], uuid[15]);
}

int fmt_uuid(char *buf, int len, long ptr, int datalen) {
	const unsigned char *uuid = (const unsigned char *)ptr;
	return snprintf(buf, len, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", uuid[0], uuid[1],
	                 uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
	                 uuid[12], uuid[13], uuid[14], uuid[15]);
}

int fmt_string_n(char *buf, int len, long ptr, int datalen) {
	const char *str = (const char *)ptr;
	strncpy(buf, str, datalen); buf[datalen] = '\0';
	return datalen + 1;
}

int fmt_ipv6(char *buf, int len, long ptr, int datalen) {
	const struct in6_addr *ip = (const struct in6_addr *)ptr;
	inet_ntop(AF_INET6, ip, buf, len);
	return len;
}

static struct json_object *read_json(const char *filename) {
	struct json_object *json;

	json = json_object_from_file(filename);
	if (json == NULL) {
		perror(filename);
		exit(1);
	}
	return json;
}

dict_t *init_dict(const char *nvmeibx_json_file, const char *fmtlib_file) {
	dict_t *dict = calloc(1, sizeof(dict_t));
	struct json_object *cksum_obj;

	assert(dict);

	if (access(nvmeibx_json_file, F_OK) < 0) {
		/* If dictionary file does not exist - too bad but we can live with it */
		_verbprint(stderr, "Error opening %s, not critical, continuing execution (%m)\n", nvmeibx_json_file);
		free(dict);
		return NULL;
	}

	dict->root_object = read_json(nvmeibx_json_file);
	if (!dict->root_object){
		free(dict);
		return NULL;
	}
	assert(json_object_object_get_ex(dict->root_object, "dictionary", &dict->dictionary));
	assert(json_object_object_get_ex(dict->root_object, "traces", &dict->nvmeibx_json));
	assert(json_object_object_get_ex(dict->root_object, "cksum", &cksum_obj));

	dict->cksum = json_object_get_int64(cksum_obj);

	assert(dict->dictionary && dict->nvmeibx_json);

	trace_entry_t_ht_init(&dict->ht);

	if (fmtlib_file && !(dict->fmtlib = dlopen(fmtlib_file, RTLD_LAZY))) {
		_verbprint(stderr, "Error opening %s, not critical, continuing execution (%m)\n", fmtlib_file);
	}

	return dict;
}

void _trace_entry_destructor(trace_entry_t *te) {
	void *fmt = (void *)((trace_entry_t *)te)->fmt;
	free(fmt);
}

void free_dict(dict_t *dict) {
	json_object_put(dict->root_object);
	trace_entry_t_ht_doall(&dict->ht, _trace_entry_destructor);
	trace_entry_t_ht_free(&dict->ht);
	if (dict->fmtlib)
		dlclose(dict->fmtlib);
	free(dict);
}

/**
 * Deduce event severity from name if possible.
 * Default severity is _T.
 */
severity_t _get_event_severity(const char *macro) {
	// Get severity level
	if (strstr(macro, "_F"))
		return SEV_F;
	else if (strstr(macro, "_D"))
		return SEV_D;
	else if (strstr(macro, "_T"))
		return SEV_T;
	else if (strstr(macro, "_I"))
		return SEV_I;
	else if (strstr(macro, "_W"))
		return SEV_W;
	else if (strstr(macro, "_E"))
		return SEV_E;
	return SEV_T;
}

/**
 * Sort trace args in a manner that fix length args are at the beginning, va length are at the end
 * other sorting is not affected.
 * Also build a mapping that can be used to determine the original sorting (for output)
 */
void __sort_args(trace_entry_t *te) {
	int i, j = 0;
	trace_arg_t new_args[MAX_ARGS] = {{0}};
	foreach_arg(i, te) if (!IS_VA_LEN(te->args[i])) {
		te->mapping[i] = j;
		new_args[j++] = te->args[i];
	}
	foreach_arg(i, te) if ( IS_VA_LEN(te->args[i])) {
		te->mapping[i] = j;
		new_args[j++] = te->args[i];
	}
	memcpy(te->args, new_args, sizeof(new_args));
}

#define __fetch_json_or_fail(parent, child, name, header, rv)                                                          \
	if (!json_object_object_get_ex(parent, name, &child)) {                                                            \
		fprintf(stderr, "%s: could not extract %s from json\n", header, name);                                         \
		return rv;                                                                                                      \
	}

/**
 * @param token_prefix Prefix to attach to any token parsed (or null)
 * @param te Trace entry to write output to
 * @param dict Dictionary object containing token resolutions
 * @param is Immutable string store
 * @param fmtlib Optional pointer to dynamic loaded fmtlib
 * @param fmt Raw trace format being parsed
 * @param new_fmt Processed trace format to be generated
 * @param fmt_write_off Write offset inside new_fmt, normally 0 unless in recursive call while using compisite tokens
 * @param arg_i Index of the first argument (if we are parsing composite) or 0 on enter, index of the next argument on exit
 */
int _lazy_unwrap_etry_format(const char *token_prefix, trace_entry_t *te, struct json_object *dict, immutable_string_store_t *is, void *fmtlib, char const *fmt, char *new_fmt, int *fmt_write_off, int *arg_i) {
	char starting_fmt[MAX_FMT] = "";
	int starting_fmt_len = 0;
	bool is_floating_point = false;

	while (fmt[0] != '\0') {
		if (fmt[0] == '\\' && *(fmt + 1) == '@') { /* Escape @ */
			new_fmt[(*fmt_write_off) ++] = '@';
			fmt += 2;
		} else if (fmt[0] == '@' && (isupper(fmt[1]) || (fmt[1] == '_'))) {
			struct json_object *token_obj, *composite_obj;
			char token_name[256], *p;
			/* Start by copying token name aside */
			p    = token_name;
			*p++ = *fmt++; *p++ = *fmt++;
			while ((isalnum(*fmt) && !islower(*fmt)) || *fmt == '_') {
				*p++ = *fmt++;
				if (p >= token_name + sizeof(token_name)) { /* Sanity check, should not happen in any reasonable (and most unreasonable) secenarios */
					fprintf(stderr, "Buffer overflow %s\n", fmt);
					exit(1);
				}
			}
			*p = '\0';
			__fetch_json_or_fail(dict, token_obj, token_name, token_name, -1);
			if (json_object_object_get_ex(token_obj, "composite", &composite_obj)) {
				/* Composite token - token that contains sub-tokens */
				struct json_object *composite_fmt_obj;
				char const *composite_fmt;
				__fetch_json_or_fail(token_obj, composite_fmt_obj, "fmt", token_name, -1);
				if (!(composite_fmt = json_object_get_string(composite_fmt_obj))) {
					fprintf(stderr, "No format for composite token %s\n", token_name);
					return -1;
				}
				if (_lazy_unwrap_etry_format(token_name, te, composite_obj, is, fmtlib, composite_fmt, new_fmt, fmt_write_off, arg_i)) {
					fprintf(stderr, "Error parsing composite token %s\n", token_name);
					return -1;
				}
			} else {
				/* Simple token */
				struct json_object *type_obj, *pfmt_obj, *is_bitfield_obj, *size_obj, *fmt_func_obj, *is_at_start_obj;
				int size;
				char const *pfmt, *type;
				int is_at_start = 0;
				__fetch_json_or_fail(token_obj, type_obj, "type", token_name, -1);
				__fetch_json_or_fail(token_obj, pfmt_obj, "fmt", token_name, -1);
				pfmt = json_object_get_string(pfmt_obj);
				__fetch_json_or_fail(token_obj, size_obj, "size", token_name, -1);
				if (json_object_object_get_ex(token_obj, "is_bitfield", &is_bitfield_obj))
					te->is_bitfield = te->is_bitfield || json_object_get_boolean(is_bitfield_obj);
				if (json_object_object_get_ex(token_obj, "is_at_start", &is_at_start_obj))
					is_at_start = json_object_get_boolean(is_at_start_obj);

				/* Try to resolve advanced formatters if any - this step is optional and may fail */
				te->args[*arg_i].fmtr = NULL;
				if (fmtlib && json_object_object_get_ex(token_obj, "fmt_func", &fmt_func_obj)) {
					char const *fmt_func = json_object_get_string(fmt_func_obj);
					if (fmt_func) {
						/*If custom formatter function specified - do the best effort to get it*/
						te->args[*arg_i].fmtr = dlsym(fmtlib, fmt_func);
						if (te->args[*arg_i].fmtr)
							pfmt = "%s";
						else
							fprintf(stderr, "Could not resolve custom formatter function %s, not critical, continuing\n", fmt_func);
					}
				}

				/* Now save all the data we extracted to the trace */
				if (!token_prefix)
					te->args[*arg_i].name = get_immutable_string(is, token_name);
				else { /* For composite tokens */
					char token_name_buf[256];
					te->args[*arg_i].short_name = get_immutable_string(is, token_name);
					snprintf(token_name_buf, sizeof(token_name_buf), "%s.%s", token_prefix, token_name + 1);
					te->args[*arg_i].name = get_immutable_string(is, token_name_buf);
				}
				size = json_object_get_int(size_obj);
				te->args[*arg_i].fmt = get_immutable_string(is, pfmt);
				type = json_object_get_string(type_obj);
				is_floating_point = strcmp(type, "double") == 0 || strcmp(type, "float") == 0;
				if (is_at_start) {
					starting_fmt_len += snprintf(starting_fmt + starting_fmt_len, MAX_FMT - starting_fmt_len, "%s", pfmt);
					if (starting_fmt_len >= MAX_FMT)
						starting_fmt_len = MAX_FMT-1;
				}
				else {
					/* we trick the formatter to use %s for double/float */
					*fmt_write_off += snprintf(new_fmt + *fmt_write_off, MAX_FMT - *fmt_write_off, "%s", is_floating_point ? "%s" : pfmt);
				}

				if (strcmp(type, "string") == 0 || strcmp(type, "symbol") == 0 || strcmp(type, "stack_trace") == 0 || strcmp(type, "printf_arg") == 0 || strcmp(type, "vprintf_arg") == 0) {
					te->args[*arg_i].type = ARG_STRING;
					te->args[*arg_i].len  = 0;
				} else {
					if (strcmp(type, "uuid") == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_uuid;
					} else if (strcmp(type, "uuid_le") == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_uuid_le;
					} else if (strcmp(type, "ipv6") == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_ipv6;
					} else if (strcmp(type, "bitmap") == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_bitmap;
					} else if (strcmp(type, "hex") == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_hex;
					} else if (strcmp(type, "array_u64") == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_array_u64;
					} else if (strcmp(type, "array_u64_flex") == 0) {
						te->args[*arg_i].type = ARG_ARR_FLEX;
						te->args[*arg_i].len  = sizeof(unsigned long long);
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_array_u64_flex;
					} else if (strcmp(type, "array_u32_flex") == 0) {
						te->args[*arg_i].type = ARG_ARR_FLEX;
						te->args[*arg_i].len  = sizeof(unsigned int);
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_array_u32_flex;
					} else if (strcmp(type, "array_int_flex") == 0) {
						te->args[*arg_i].type = ARG_ARR_FLEX;
						te->args[*arg_i].len  = sizeof(int);
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_array_int_flex;
					} else if (strcmp(type, "array_ptr_flex") == 0) {
						te->args[*arg_i].type = ARG_ARR_FLEX;
						te->args[*arg_i].len  = sizeof(unsigned long long);
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_array_ptr_flex;
					} else if (strncmp(type, "string_", sizeof("string_") - 1) == 0) {
						te->args[*arg_i].type = ARG_DATA;
						te->args[*arg_i].fmtr = te->args[*arg_i].fmtr ? te->args[*arg_i].fmtr : fmt_string_n;
					} else if (is_floating_point) {
						te->args[*arg_i].type = ARG_DOUBLE;
					} else {
						te->args[*arg_i].type = ARG_INT;
					}
					if (te->args[*arg_i].type != ARG_ARR_FLEX) {
						te->args[*arg_i].len    = (size + 7) / 8;
						te->args[*arg_i].bitlen = size;
					}
				}

				(*arg_i) ++;
			}
		} else
			new_fmt[(*fmt_write_off) ++] = *fmt++;

		if (*fmt_write_off >= MAX_FMT) {
			fprintf(stderr, "Event format too long, while processing fmt=%s\n", fmt);
			return -1;
		}
	}

	if (starting_fmt_len) {
		// some tokens were added at the start of the line, prepend them to the format
		new_fmt[*fmt_write_off] = 0;
		starting_fmt_len += snprintf(starting_fmt + starting_fmt_len, MAX_FMT - starting_fmt_len, "%s", new_fmt);
		if (starting_fmt_len >= MAX_FMT)
			starting_fmt_len = MAX_FMT-1;
		strcpy(new_fmt, starting_fmt);
		*fmt_write_off = starting_fmt_len;
	}

	return 0;
}

trace_entry_t *lazy_compile_entry(dict_t *dict, immutable_string_store_t *is, void *msg, int add_trace_name) {
	trace_entry_t lookup_entry, *te;
	char const *trace_name;
	char const *func_name;
	json_bool trace_found;
	struct json_object *trace;
	struct json_object *line_obj, *file_obj, *fmt_obj, *macro_obj, *no_prefix_obj;
	struct json_object *name_obj, *func_obj;
	char const *fmt;
	int arg_i = 0;

	if (*(int *)msg & 1) // skip timestamp
		msg += 8;
	else
		msg += 4;
	lookup_entry.trace_id = *(unsigned short *)msg;

	if (IS_SYSTEM_TRACE_ID(lookup_entry.trace_id)) {
		switch(lookup_entry.trace_id) {
			case SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF_ID: return &SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF;
		}
	}

	te = trace_entry_t_ht_retrieve(&dict->ht, &lookup_entry);
	if (!te) {
		// Entry not found. We need to compile it.
		char new_fmt[MAX_FMT];
		int fmt_len = 0;

		msg += 2;
		assert((te = calloc(1, sizeof *te)));

		te->trace_id = lookup_entry.trace_id;

		if (json_object_is_type(dict->nvmeibx_json, json_type_object)) {
			/* backward compitabilty support for older json versions */
			char id_str[100];
			snprintf(id_str, sizeof id_str, "%d", lookup_entry.trace_id);
			trace_found = json_object_object_get_ex(dict->nvmeibx_json, id_str, &trace);
		} else {
			/* newer jsons has fast access by array indexes */
			trace = json_object_array_get_idx(dict->nvmeibx_json, te->trace_id - FIRST_TRACE_ID);
			trace_found = !!trace;
		}
		if (!trace_found) {
			_verbprint(stderr, "cannot find %hu\n", te->trace_id);
			return NULL;
		}

		__fetch_json_or_fail(trace, name_obj, "name", "Trace", NULL);
		trace_name = json_object_get_string(name_obj);
		__fetch_json_or_fail(trace, func_obj, "func", trace_name, NULL);
		func_name = json_object_get_string(func_obj);
		__fetch_json_or_fail(trace, line_obj, "line", trace_name, NULL);
		__fetch_json_or_fail(trace, file_obj, "file", trace_name, NULL);
		__fetch_json_or_fail(trace, fmt_obj, "fmt", trace_name, NULL);
		__fetch_json_or_fail(trace, macro_obj, "macro", trace_name, NULL);

		if (json_object_object_get_ex(trace, "no_prefix", &no_prefix_obj)) {
			if (json_object_get_boolean(no_prefix_obj)) {
				te->is_no_prefix = 1;
			}
		}

		/* Fill in the data we have just read */
		te->trace_name = get_immutable_string(is, trace_name);
		te->func_name = func_name ? get_immutable_string(is, func_name) : get_immutable_string(is, "BAD_FUNCTION_NAME");
		te->file_name = get_immutable_string(is, json_object_get_string(file_obj));
		te->severity = _get_event_severity(json_object_get_string(macro_obj));
		te->orig_fmt = get_immutable_string(is, json_object_get_string(fmt_obj));
		fmt = te->orig_fmt->token;

		if (!te->is_no_prefix) {
			if (add_trace_name)
				fmt_len += snprintf(new_fmt + fmt_len, MAX_FMT - fmt_len, "%s:%d event=%s ", te->file_name->token, json_object_get_int(line_obj), trace_name);
			else
				fmt_len += snprintf(new_fmt + fmt_len, MAX_FMT - fmt_len, "%s:%d [%s] ", te->file_name->token, json_object_get_int(line_obj), te->func_name->token);
		}

		if (fmt_len > MAX_FMT)
			fmt_len = MAX_FMT;

		te->is_bitfield = 0;

		if (_lazy_unwrap_etry_format(NULL, te, dict->dictionary, is, dict->fmtlib, fmt, new_fmt, &fmt_len, &arg_i)) {
			fprintf(stderr, "Error parsing %s\n", trace_name);
			return NULL;
		}

		__sort_args(te);

		new_fmt[fmt_len++] = '\n';
		new_fmt[fmt_len] = '\0';
		te->fmt = strdup(new_fmt);
		if (trace_entry_t_ht_insert(&dict->ht, te) != NULL)
			fprintf(stderr, "%s duplicate\n", trace_name);
	}

	return te;
}

/**
 * Generate code that invokes func with different arguments on vec based on selector
 * @param func Function to invoke
 * @param vec Target array
 * @param map Mapping function to invoke on index
 * @param ... Non variable func arguments, currently at least 1 is required
 */
#define __identity_map(idx) idx
#define __vector_control_switch(func, vec, map, ...) \
		case 0: \
			return func(__VA_ARGS__); \
		case 1: \
			return func(__VA_ARGS__, vec[map(0)]); \
		case 2: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)]); \
		case 3: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)]); \
		case 4: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)]); \
		case 5: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)]); \
		case 6: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)]); \
		case 7: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)]); \
		case 8: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)]); \
		case 9: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)]); \
		case 10: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)]); \
		case 11: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)]); \
		case 12: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)]); \
		case 13: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)]); \
		case 14: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)]); \
		case 15: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)]); \
		case 16: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)]); \
		case 17: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)]); \
		case 18: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)]); \
		case 19: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)]); \
		case 20: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)]); \
		case 21: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)]); \
		case 22: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)]); \
		case 23: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)], vec[map(22)]); \
		case 24: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)], vec[map(22)], vec[map(23)]); \
		case 25: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)], vec[map(22)], vec[map(23)], vec[map(24)]); \
		case 26: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)], vec[map(22)], vec[map(23)], vec[map(24)], vec[map(25)]); \
		case 27: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)], vec[map(22)], vec[map(23)], vec[map(24)], vec[map(25)], vec[map(26)]); \
		case 28: \
			return func(__VA_ARGS__, vec[map(0)], vec[map(1)], vec[map(2)], vec[map(3)], vec[map(4)], vec[map(5)], vec[map(6)], vec[map(7)], vec[map(8)], vec[map(9)], vec[map(10)], vec[map(11)], vec[map(12)], vec[map(13)], vec[map(14)], vec[map(15)], vec[map(16)], vec[map(17)], vec[map(18)], vec[map(19)], vec[map(20)], vec[map(21)], vec[map(22)], vec[map(23)], vec[map(24)], vec[map(25)], vec[map(26)], vec[map(27)]);

/**
 * Fills arguments array given trace entry and msg buffer.
 * Assume the array provided is large enough to hold all the args.
 * Assume trace entry is not a bitfield.
 * Sets length to the number of bytes parsed (how much to skip in msg until next msg)
 * Returns the number of args parsed.
 */
int _build_arg_vec(long *arg_vec, trace_entry_t *entry, void *msg, int *length) {
	static char custom_fmt_buf[MAX_ARGS][65536];
	void *start_msg = msg;
	unsigned long len_mask;
	int i;

	if (*(int *)msg & 1) // skip timestamp
		msg += 8;
	else
		msg += 4;
	msg += 2; // Skip trace id

	foreach_arg(i, entry) {
		switch (entry->args[i].type) {
		case ARG_STRING:
			arg_vec[i] = (long)msg;
			while (*(char *)msg)
				++msg;
			++msg; // Skip the '\0' too.
			break;
		case ARG_INT:
			len_mask = entry->args[i].bitlen >= 64 ? 0xffffffffffffffffUL : ((1ULL << entry->args[i].bitlen) - 1);
			arg_vec[i] = *(unsigned long *)msg & len_mask;
			msg += entry->args[i].len;
			break;
		case ARG_DOUBLE: {
			double *double_ptr = (double *)msg;
			// for double we apply the user provided format token as we can't later print double with %f
			snprintf(custom_fmt_buf[i], sizeof(custom_fmt_buf[i]), entry->args[i].fmt->token, *double_ptr);
			arg_vec[i] = (long)custom_fmt_buf[i];
			msg += entry->args[i].len;
			break;
			}
		case ARG_DATA:
			arg_vec[i] = (long)msg;
			msg += entry->args[i].len;
			break;
		case ARG_ARR_FLEX:
			arg_vec[i] = (long)msg;
			{
				unsigned int count = le32toh(*(unsigned int *)msg);
				msg += sizeof(count) + (count * (unsigned int)entry->args[i].len);
			}
			break;
		case ARG_NONE:
		case ARG_LAST:
			fprintf(stderr, "Bad arg type entry->arg_type[%d] = %d\n", i, entry->args[i].type);
			exit(1);
		}
		/*If arg has custom formatter - apply it*/
		if (entry->args[i].fmtr) {
			entry->args[i].fmtr(custom_fmt_buf[i], sizeof(custom_fmt_buf[i]), arg_vec[i], entry->args[i].len);
			arg_vec[i] = (long)custom_fmt_buf[i];
		}
	}

	if (length)
		*length = msg - start_msg;

	return i;
}

/**
 * Fills arguments array given trace entry and msg buffer.
 * Assume the array provided is large enough to hold all the args.
 * Assume trace entry is a bitfield.
 * Assume all args are of type ARG_INT.
 * Sets length to the number of bytes parsed (how much to skip in msg until next msg)
 * Returns the number of args parsed.
 */
int _build_bitfield_arg_vec(long *arg_vec, trace_entry_t *entry, char *msg, int *length) {
	static char custom_fmt_buf[MAX_ARGS][65536];
	char *start_msg = msg;
	int i;
	int bits_off = 0; // Offset in bits inside current byte in message

	if (*(int *)msg & 1) // skip timestamp
		msg += 8;
	else
		msg += 4;
	msg += 2; // Skip trace id

	foreach_arg(i, entry) {
		long accumulator = 0;				  // Will accumulate bits of current parameter, until all bits are read
		int bits_left = entry->args[i].bitlen; // How many bits we have to read
		int bits_read = 0;					  // How many bits we read, used to shift new info
		while (bits_left) {
			int bits_to_grab = bits_left > 8 - bits_off ? 8 - bits_off : bits_left;
			unsigned char newbits = *msg << (8 - bits_to_grab - bits_off);
			newbits >>= (8 - bits_to_grab);
			accumulator |= ((long)newbits) << bits_read;
			bits_off += bits_to_grab;
			assert(bits_off <= 8);
			if (bits_off == 8) {
				++msg;
				bits_off = 0;
			}
			bits_read += bits_to_grab;
			bits_left -= bits_to_grab;
		}
		arg_vec[i] = accumulator;
		/*If arg has custom formatter - apply it*/
		if (entry->args[i].fmtr) {
			entry->args[i].fmtr(custom_fmt_buf[i], sizeof(custom_fmt_buf[i]), arg_vec[i], entry->args[i].len);
			arg_vec[i] = (long)custom_fmt_buf[i];
		}
		// Next argument
	}

	if (bits_off) // Bits are not fractioned between messages
		++msg;

	if (length)
		*length = msg - start_msg;

	return i;
}

int build_arg_vec(long *arg_vec, trace_entry_t *entry, char *msg, int *length) {
	int count;
	if (entry->trace_id == SYSTEM_TRACE_ID)
		return 0;
	if (entry->is_bitfield)
		count = _build_bitfield_arg_vec(arg_vec, entry, msg, length);
	else
		count = _build_arg_vec(arg_vec, entry, msg, length);

	return count;
}
#define __entry_map(index) entry->mapping[index]
int stdout_argvec(trace_entry_t *entry, const long *arg_vec, int count) {
	switch (count)
	{
		__vector_control_switch(printf, arg_vec, __entry_map, entry->fmt);
	default:
		/*Should not be here - too many arguments*/
		assert(0);
	}
}

int snprintf_argvec(char *buf, size_t len, trace_entry_t *entry, const long *arg_vec, int count) {
	switch (count)
	{
		__vector_control_switch(snprintf, arg_vec, __entry_map, buf, len, entry->fmt);
	default:
		/*Should not be here - too many arguments*/
		assert(0);
	}
}

int trace_actual_length(trace_entry_t *entry, void *msg) {
	void *start_msg = msg;
	int i;

	if (*(int *)msg & 1) // skip timestamp
		msg += 8;
	else
		msg += 4;
	msg += 2;

	if (entry->is_bitfield) {
		int bitslength = 0;
		foreach_arg(i, entry) {
			bitslength += entry->args[i].bitlen;
		}
		return (bitslength + 7) / 8 + (msg - start_msg);
	} else {
		foreach_arg(i, entry) {
			switch (entry->args[i].type) {
				case ARG_STRING:
					while (*(char *)msg)
						++msg;
					++msg; // Skip the '\0' too.
					break;
				case ARG_DATA:
					msg += entry->args[i].len;
					break;
				case ARG_ARR_FLEX:
					{
						unsigned int count = le32toh(*(unsigned int *)msg);
						msg += sizeof(count) + (count * (unsigned int)entry->args[i].len);
					}
					break;
				case ARG_INT:
				case ARG_DOUBLE:
					msg += entry->args[i].len;
					break;
				case ARG_NONE:
				case ARG_LAST:
					assert(0);
					break;
			}
		}
		return msg - start_msg;
	}
}

/**
 * @note: For @tsc_khz = 0 return -1
 */
unsigned long tsc_to_ns(unsigned long timestamp, unsigned int tsc_khz) {
	/* Avoid overflow, alternative is to use __int128 */
	if (!tsc_khz) return -1;
	return 1000000L * (timestamp / tsc_khz) +                  // Lose precision
	       (1000000L * (timestamp % tsc_khz)) / tsc_khz; // Compensate lost precision
}

unsigned long ns_to_tsc(unsigned long timestamp, unsigned int tsc_khz) {
	/* Avoid overflow, alternative is to use __int128 */
	return tsc_khz * (timestamp / 1000000L) +             // Lose precision
	       (tsc_khz * (timestamp % 1000000L)) / 1000000L; // Compensate lost precision
}

#ifdef PRINT_LONG_TIMESTAMP
void print_timestamp(unsigned long ts_ns, int print_date) {
	time_t seconds;
	struct tm *tm;
	int msec, usec, nsec;

	(void)print_date;

	seconds = ts_ns / 1000000000;
	nsec = ts_ns % 1000;
	usec = (ts_ns / 1000) % 1000;
	msec = (ts_ns / 1000000) % 1000;
	tm = localtime(&seconds);
	printf("%02d/%02d/%04d %2d:%02d:%02d.%03d,%03d,%03d ", tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900, tm->tm_hour,
		   tm->tm_min, tm->tm_sec, msec, usec, nsec);
}
#else
void print_timestamp(unsigned long ts_ns, int print_date) {
	time_t seconds;
	struct tm *tm;

	seconds = ts_ns / 1000000000;
	tm = localtime(&seconds);
	if (print_date)
		printf("%02d/%02d %02d:%02d:%02d.%06ld ", tm->tm_mday, tm->tm_mon + 1, tm->tm_hour, tm->tm_min, tm->tm_sec, ts_ns % 1000000000 / 1000);
	else
		printf("%02d:%02d:%02d.%06ld ", tm->tm_hour, tm->tm_min, tm->tm_sec, ts_ns % 1000000000 / 1000);
}
#endif

immutable_string_t *get_immutable_string(immutable_string_store_t *store, const char *name) {
	immutable_string_t *e = immutable_string_t_ht_retrieve(store, &((immutable_string_t){name}));
	if (!e) { // It is a new entry
		e = calloc(1, sizeof(immutable_string_t));
		assert(e);
		e->token = strdup(name);
		assert(e->token);
		immutable_string_t_ht_insert(store, e);
	}
	return e;
}

int has_immutable_string(immutable_string_store_t *store, const char *name) {
	return !!immutable_string_t_ht_retrieve(store, &((immutable_string_t){name}));
}

void _immutable_string_destructor(immutable_string_t *te) {
	void *token = (void *)((immutable_string_t *)te)->token;
	free(token);
}

immutable_string_store_t *init_immutable_string_store() {
	immutable_string_store_t *store = calloc(1, sizeof(immutable_string_store_t));
	if (store)
		immutable_string_t_ht_init(store);
	return store;
}

void free_immutable_string_store(immutable_string_store_t *store) {
	immutable_string_t_ht_doall(store, _immutable_string_destructor);
	immutable_string_t_ht_free(store);
}
