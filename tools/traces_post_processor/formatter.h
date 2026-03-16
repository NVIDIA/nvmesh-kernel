/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef DICTIONARY_COMPILER_H
#define DICTIONARY_COMPILER_H

#define MAX_FILENAME 256
#define MAX_FMT 4096
#define MAX_ARGS 32
#define MAX_ENTRIES 28
#define MAX_CPUS 256
#define FIRST_TRACE_ID 256

#define SYSTEM_TRACE_DICT_CKSUM 0 /*Special trace id used to prevent spamming with errors*/
#define SYSTEM_TRACE_ID 1 /*Special trace id used to report pager debug/error info*/
#define SYSTEM_TRACE_AGAIN_ID 2 /*Special trace id used to prevent spamming with errors*/
#define SYSTEM_TRACE_GREETING_ID 3 /*Special trace id used to prevent spamming with errors*/
#define SYSTEM_TRACE_LAST SYSTEM_TRACE_GREETING_ID

/* Special traces generated at application runtime, before log analysis */
#define SYSTEM_TRACE_RUNTIME_INVAL_SIZE_BUF_ID 8 /*Indicate application tried to write invalid size buffer*/

#define IS_SYSTEM_NON_PARSABLE(id) ((id) < 8)
#define IS_SYSTEM_TRACE_ID(id) ((id) < 9)

#define _verbprint(fd, ...)                                                                                            \
	({                                                                                                                 \
		extern int GLOBAL_PAGER_DEBUG;                                                                                 \
		int _____rv = 0;                                                                                               \
		if (GLOBAL_PAGER_DEBUG & PDBG_VERBOSE)                                                                                   \
			_____rv = fprintf(fd, ##__VA_ARGS__);                                                                      \
		_____rv;                                                                                                       \
	})

enum pager_dbg_flag {
	PDBG_BUFFERS   = 0b00000000000000000000000000000001,
	PDBG_FILTERS   = 0b00000000000000000000000000000010,
	PDBG_VERBOSE   = 0b10000000000000000000000000000000
};

#include "formatters/formatter_functions.h"
#include "pager_hashtable.h"

enum arg_types { ARG_NONE, ARG_STRING, ARG_INT, ARG_DATA, ARG_DOUBLE, ARG_ARR_FLEX, ARG_LAST };

/**
 * Used to give a dictionary of immutable string tokens for fast string lookups (for token ids and trace ids)
 */
typedef struct immutable_string {
	const char *token;
} immutable_string_t;

/**
 * @toto: Move all immutable strings stuff outside this file
 */
struct immutable_string_t_ht;
typedef struct immutable_string_t_ht immutable_string_store_t;

typedef enum severity { SEV_SYSTEM = 0, SEV_E = 1, SEV_W, SEV_I, SEV_T, SEV_D, SEV_F } severity_t;

static const char * const severity_str[] =
{
    [SEV_SYSTEM] = "SYSTEM",
    [SEV_E] = "ERROR",
    [SEV_W]  = "WARN",
    [SEV_I]  = "INFO",
	[SEV_T] = "TRACE",
	[SEV_D] = "DEBUG",
	[SEV_F] = "FINE"
};

typedef struct trace_args {
	enum arg_types type;
	short len;
	short bitlen;
	immutable_string_t *name;
	immutable_string_t *fmt;
	immutable_string_t *short_name;
	formatter_function fmtr;
} trace_arg_t;

#define IS_VA_LEN(arg) ((arg).type == ARG_STRING || (arg).type == ARG_ARR_FLEX)

typedef struct trace_entry {
	unsigned short trace_id;
	trace_arg_t args[MAX_ARGS];
	int mapping[MAX_ENTRIES];
	immutable_string_t *trace_name;
	immutable_string_t *func_name;
	immutable_string_t *file_name;
	immutable_string_t *orig_fmt;
	severity_t severity;
	int is_bitfield;
	int is_no_prefix;
	char *fmt;
} trace_entry_t;

#define foreach_arg(i, entry) for (i = 0; i < MAX_ARGS && (entry)->args[i].type; ++i)

struct dict;
typedef struct dict dict_t;


/**
 * Initialize dictionary. Read input files and allocate resources.
 * Do not compile, as it is done on the fly. Return pointer to the
 * new dictionary descriptor on success, NULL on failure.
 * Will try to ope fmtlib, as best effort, but will not fail if couldn't
 */
dict_t *init_dict(const char *nvmeibx_json_file, const char *fmtlib_file);

/**
 * Release resources allocated for the dictionary.
 */
void free_dict(dict_t *dict);

/**
 * Retrieve trace_id from buffer @msg, compile dictionary entry if needed, and return the compiled entry.
 */
trace_entry_t *lazy_compile_entry(dict_t *dict, immutable_string_store_t *is, void *msg, int add_event_name);

/**
 * Given trace entry and its parsed argvec, print it to stdout
 */
int stdout_argvec(trace_entry_t *entry, const long *arg_vec, int count);

/**
 * Given trace entry and its parsed argvec, print it to a buffer
 */
int snprintf_argvec(char *buf, size_t len, trace_entry_t *entry, const long *arg_vec, int count);

/**
 * Given a buffer containing a trace, and trace entry from dictionary, get trace actual length, considering vaiable size
 * strings it may or may not contain. Return 0 if message is corrupted.
 */
int trace_actual_length(trace_entry_t *entry, void *msg);

/**
 * Convert ticks to timestamp using cpu freq
 */
unsigned long tsc_to_ns(unsigned long timestamp, unsigned int tsc_khz);

/**
 * Convert timestamp to ticks using cpu freq
 */
unsigned long ns_to_tsc(unsigned long timestamp, unsigned int tsc_khz);

/**
 * Prints formatted timestamp to stdout
 */
void print_timestamp(unsigned long ts_ns, int print_date);

/**
 * Fills arguments array given trace entry and msg buffer.
 * Assume the array provided is large enough to hold all the args.
 * Sets length to the number of bytes parsed (how much to skip in msg until next msg)
 * Returns the number of args parsed.
 */
int build_arg_vec(long *arg_vec, trace_entry_t *entry, char *msg, int *length);

/**
 * Retrieve expected cksum from dictionary
 */
static inline unsigned int dict_cksum(const dict_t *dict) { return *((unsigned int *)dict); }


/**
 * Get a pointer to an immutable string. If not found - create and return.
 * Guaranteed to return non NULL value.
 * @todo: Move all immutable strings related stuff to a separate file
 */
immutable_string_t *get_immutable_string(immutable_string_store_t *store, const char *str);

/**
 * Tets whether or not an immutable string is in the store. Do not modify the store.
 */
int has_immutable_string(immutable_string_store_t *store, const char *str);

/**
 * Immutable string store constructor
 */
immutable_string_store_t *init_immutable_string_store(void);

/**
 * Immutable string store destructor
 */
void free_immutable_string_store(immutable_string_store_t *store);

/**
 * Special trace types
 */
extern trace_entry_t SYSTEM_TRACE;
extern trace_entry_t SYSTEM_TRACE_AGAIN;
extern trace_entry_t SYSTEM_TRACE_GREETING;

#endif /*DICTIONARY_COMPILER_H*/
