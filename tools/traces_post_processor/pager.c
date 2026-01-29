/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <string.h>
#define __USE_XOPEN
#include <limits.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "pager_filter.h"
#include "pager_msg_stream.h"
#include "pager_statistics.h"
#include "pager_serialized_msg_stream.h"

#include "json-c/json.h"

#define MAX_LINES_PER_HELP 24
#define MAX_COMMA_SEPARATED_LIST 4096

#define FORCE_SUCCESS 0

typedef int (*cmd_handler_t)(int argc, char **argv, char *exe);

long GLOBAL_PAGER_DEBUG = 0;

typedef struct pager_cmd {
	const char *name;
	const char *args;
	cmd_handler_t handler;
	const char *summary[MAX_LINES_PER_HELP];
} pager_cmd_t;

enum output_type { OUTTYPE_BINARY = 0, OUTTYPE_TEXT, OUTTYPE_JSON, OUTTYPE_CSV_STATS_ONLY };
const char *output_type_str(enum output_type o) {
	switch (o) {
		case OUTTYPE_BINARY: return "msg-stream-bin";
		case OUTTYPE_TEXT: return "msg-stream-txt";
		case OUTTYPE_JSON: return "msg-stream-json";
		case OUTTYPE_CSV_STATS_ONLY: return "msg-stream-csv-stats-only";
	}
	assert(0);
}

int print_help_handler(int argc, char **argv, char *exe);
int deserialize_stream_handler(int argc, char **argv, char *exe);
int msg_stream_handler_bin(int argc, char **argv, char *exe);
int msg_stream_handler_txt(int argc, char **argv, char *exe);
int msg_stream_handler_json(int argc, char **argv, char *exe);
int msg_stream_handler_csv_stats_only(int argc, char **argv, char *exe);

// clang-format off
const pager_cmd_t pager_cmds[] = {
	{	"help", "",
		print_help_handler,
		{
			"Show help message and exit.",
			NULL
		}
	},
	{	"deserialize-msg-stream", "work-dir [input-streams]",
		deserialize_stream_handler,
		{
			"Deserialize binary message stream from stdin into text.",
			NULL
		}
	},
	{	"msg-stream-bin", "dict-list log-channels-list time-frame [-f filter]",
		msg_stream_handler_bin,
		{
			"Dump all messages from all CPUs from given timeframe to stdout in binary format.",
			"[-f] Either filter expression or '-' (to read filters from stdin).",
			"     If not specified - no filters are applied.",
			NULL
		}
	},
	{	"msg-stream-txt", "dict-list log-channels-list time-frame [-f filter] [--color] [--print-date] [--fmtlib] [--statistics] [--reliable] [--nogreet]",
		msg_stream_handler_txt,
		{
			"Dump all messages from all CPUs from given timeframe to stdout in text format.",
			"[-f] Either filter expression or '-' (to read filters from stdin).",
			"     If not specified - no filters are applied.",
			"[--color] Colorize the output.",
			"[--print-date] Print date (by default prints only time).",
			"[--fmtlib] Custom formatters so to preload. Default = NULL.\n",
			"[--statistics] Statistics collection mode.\n",
			NULL
		}
	},
	{	"msg-stream-json", "dict-list log-channels-list time-frame [-f filter] [--fmtlib]",
		msg_stream_handler_json,
		{
			"Dump all messages from all CPUs from given timeframe to stdout in json format.",
			"[-f] Either filter expression or '-' (to read filters from stdin).",
			"     If not specified - no filters are applied.",
			"[--fmtlib] Custom formatters so to preload. Default = NULL.\n",
			NULL
		}
	},
	{	"msg-stream-csv-stats-only", "dict-list log-channels-list time-frame [-f filter] [--fmtlib]",
		msg_stream_handler_csv_stats_only,
		{
			"Dump all messages from all CPUs from given timeframe to stdout in csv format containing time-stamp, binary-size, trace-name, etc.",
			"[-f] Either filter expression or '-' (to read filters from stdin).",
			"     If not specified - no filters are applied.",
			"[--fmtlib] Custom formatters so to preload. Default = NULL.\n",
			NULL
		}
	},
	{NULL}
};
// clang-format on

int print_help(const char *exe, const char *cmdname) {
	const pager_cmd_t *cmd = pager_cmds;
	fprintf(stderr,
			"Usage: %s COMMAND [--dbg level] [arguments]\n"
			"\n"
			"  Commands:\n",
			exe);
	while (cmd->name) {
		if (!cmdname || !strcmp(cmd->name, cmdname)) {
			const char *const *summary_line = cmd->summary;
			fprintf(stderr, "\n    [%s]: %s\n", cmd->name, cmd->args);
			while (*summary_line) {
				fprintf(stderr, "        %s\n", *summary_line);
				++summary_line;
			}
		}
		++cmd;
	}
	return 1;
}

int print_help_handler(int argc, char **argv, char *exe) {
	if (argc == 0) return print_help(exe, NULL);
	else           return print_help(exe, argv[0]);
}

static timestamp_t str_to_timestamp(const char *s, channel_ctx_t **ctx, int nctx) {
	if (!strchr(s, ':')) {
		if (strstr(s, "now")) { /*now-x*/
			time_t milliseconds;
			time_t now = time(NULL);
			struct tm t = *localtime(&now);
			time_t seconds;
			seconds = mktime(&t);
			if (sscanf(s, "now-%lu", &milliseconds) != 1) {
				fprintf(stderr, "Bad time format %s\n", s);
				exit(-1);
			}
			return 1000000000UL * seconds - 1000000UL * milliseconds;
		} else if (strstr(s, "tail")) {
			timestamp_t tail_time = 0;
			time_t milliseconds;
			int i;
			// In case of tail, startpoint is the latest timestamp of all buffers in all channels, -delta
			for (i = 0; i < nctx; ++i) {
				timestamp_t peek = peek_tail_timestamp(ctx[i]);
				tail_time = (tail_time > peek) ? tail_time : peek;
			}
			if (tail_time == -1) {
				fprintf(stderr, "No data found, cannot determine tail\n");
				exit(-1);
			}
			if (sscanf(s, "tail-%lu", &milliseconds) != 1) {
				fprintf(stderr, "Bad time format %s\n", s);
				exit(-1);
			}
			return tail_time - 1000000UL * milliseconds;
		} else { /*numberic timestamp*/
			return strtoull(s, NULL, 10);
		}
	} else { // It is formatted date string
		time_t now = time(NULL);
		struct tm t = *localtime(&now);
		time_t seconds;
		if (strptime(s, "%d/%m/%Y %H:%M:%S", &t) == NULL) {
			fprintf(stderr, "Bad time format %s\n", s);
			exit(-1);
		}
		seconds = mktime(&t);
		return 1000000000UL * seconds;
	}
}

#define KERN_COL_RED "\x1b[31m"
#define KERN_COL_GREEN "\x1b[32m"
#define KERN_COL_YELLOW "\x1b[1;33m"
#define KERN_COL_RESET "\x1b[0;0m"
#define KERN_COL_RED_BOLD "\x1b[1;31m"
#define KERN_COL_WHITE_BOLD "\x1b[1;37m"

static void format_binary_trace(const binary_trace_t *bt, int color, int print_date, int print_cpu_id, const char *hostname) {
	if (GLOBAL_PAGER_DEBUG & PDBG_BUFFERS) {
		printf("["BUF_POS_FMT_SHORT"]", BUF_POS_ARG_SHORT(bt->ctx, bt->buf_descr->meta.src_pos));
	}
	if (color) {
		if (bt->entry->severity == SEV_E)
			printf(KERN_COL_RED_BOLD);
		else if (bt->entry->severity == SEV_W)
			printf(KERN_COL_YELLOW);
		else if (bt->entry->severity == SEV_SYSTEM)
			printf(KERN_COL_GREEN);
	}
	if (!bt->entry->is_no_prefix) {
		if (hostname[0])
			printf("%s: ", hostname);
		print_timestamp(bt->ts, print_date);
		printf("(%llu) ", bt->ts);
		if (print_cpu_id)
			printf("[%02lld]", bt->cpu_id);
	}
	if(bt->entry->severity <= SEV_W)
		printf("%s: ", severity_str[bt->entry->severity]);
	if (IS_SYSTEM_NON_PARSABLE(bt->entry->trace_id))
		printf("%s\n", (const char *)bt->msg);
	else
		stdout_argvec(bt->entry, bt->arg_vec, bt->arg_count);
	if (color && bt->entry->severity <= SEV_W)
		printf(KERN_COL_RESET);
}

static void format_binary_trace_json(const binary_trace_t *bt) {
	static char msg[MAX_FMT];
	static char formatted_arg[MAX_FMT];
	unsigned int i = 0;

	struct json_object *jobj;

	/* When formatting json, ignore system/greeting messages */
	if (bt->entry->trace_id <= SYSTEM_TRACE_LAST) return;

	jobj = json_object_new_object();

	for(i = 0; i < bt->arg_count; i++) {
		const char *token_name = bt->entry->args[i].name->token + 1; /* drop @ prefix for each token name */
		snprintf(formatted_arg, sizeof(formatted_arg), bt->entry->args[i].fmt->token, bt->arg_vec[i]);
		json_object_object_add(jobj, token_name, json_object_new_string(formatted_arg));
	}

	json_object_object_add(jobj, "hostname", json_object_new_string(bt->ctx->hostname->token));
	json_object_object_add(jobj, "severity", json_object_new_int(bt->entry->severity));
	json_object_object_add(jobj, "cpu_id", json_object_new_int64(bt->cpu_id));
	json_object_object_add(jobj, "nanoseconds", json_object_new_int64(bt->ts));
	json_object_object_add(jobj, "binary_size", json_object_new_int64(bt->len));
	json_object_object_add(jobj, "trace_name", json_object_new_string(bt->entry->trace_name->token));
	json_object_object_add(jobj, "func_name", json_object_new_string(bt->entry->func_name->token));
	json_object_object_add(jobj, "channel", json_object_new_string(bt->ctx->basename));

	snprintf_argvec(msg, sizeof(msg), bt->entry, bt->arg_vec, bt->arg_count);
	json_object_object_add(jobj, "message", json_object_new_string(msg));

	printf("%s\n", json_object_to_json_string(jobj));

	json_object_put(jobj);
}

static void print_csv_header(void) {
	printf("hostname,severity,cpu_id,nanoseconds,binary_size,trace_name,func_name,channel\n");
}

static void format_binary_trace_csv_stats_only(const binary_trace_t *bt)
{
	static bool first_time = true;

	/* When formatting csv, ignore system/greeting messages */
	if (bt->entry->trace_id <= SYSTEM_TRACE_LAST) return;

	if (first_time) {
		print_csv_header();
		first_time = false;
	}

	printf("%s,%d,%llu,%llu,%zu,%s,%s,%s\n",
		bt->ctx->hostname->token,
		bt->entry->severity,
		bt->cpu_id,
		bt->ts,
		bt->len,
		bt->entry->trace_name->token,
		bt->entry->func_name->token,
		bt->ctx->basename
	);
}

static void _parse_timeframe(const char *t1, const char *t2, timestamp_t *_start, timestamp_t *_end, channel_ctx_t **ctx, int nctx) {
	*_start = str_to_timestamp(t1, ctx, nctx);
	*_end = str_to_timestamp(t2, ctx, nctx);
}

static filter_t *_compile_filter(char *filter_str, immutable_string_store_t *is) {
	filter_t *filter = NULL;
	if (filter_str) {
		if (!strcmp(filter_str, "-"))
			filter = build_filter(is, stdin);
		else {
			FILE *memfile = fmemopen(filter_str, strlen(filter_str), "r");
			assert(memfile);
			filter = build_filter(is, memfile);
			fclose(memfile);
		}
		assert(filter);
	}
	return filter;
}

/**
 * Some pretty trivial utility functions for string manipulations
 */
static inline int __count_ch(const char *s, char ch) {
	int i;
	for (i = 0; s[i]; s[i] == ch ? i++ : *s++)
		;
	return i;
}
static char **__split_str(char *s, char ch, int *count) {
	int i;
	char **res;
	if (!s[0]) {
		*count = 0;
		return NULL;
	}
	*count = __count_ch(s, ch) + 1;
	res = malloc(sizeof(char**)*(*count));
	assert(res);
	res[0] = s;
	for (i = 1; i < *count; ++i) {
		char *ptr = strchr(res[i-1], ',');
		*(ptr++) = '\0';
		res[i] = ptr;
	}
	return res;
}

/**
 * Free multiple pager ctx
 */
void free_multiple_channel_ctx(channel_ctx_t **ctx_arr, int count) {
	int i;
	if (!ctx_arr) // Nothing to do
		return;
	for (i = 0; i < count && ctx_arr[i]; ++i) {
		free_channel_ctx(ctx_arr[i]);
	}
	free(ctx_arr);
}

/**
 * Dump statistics for all channels
 */
void dump_statistics_multiple(channel_ctx_t **ctx_arr, int count) {
	int i, first = 1, dumped_any = 0;
	if (!ctx_arr) // Nothing to do
		return;
	for (i = 0; i < count && ctx_arr[i]; ++i) {
		if (ctx_arr[i]->do_statistics) {
			if (!first)
				printf(",\n");
			else
				printf("{\n");
			printf("\"%s\" : ", ctx_arr[i]->basename);
			dump_per_channel_stats_t(ch_stats(ctx_arr[i]));
			first = 0;
			dumped_any = 1;
		}
	}
	if (dumped_any)
		printf("\n}\n");
}

/**
 * Initialize multiple pager ctx, based on coma separated configs from basename_list and dict_list
 * @return Pointer to array of ctx instances or NULL.
 *         If not NULL, set *count to number of ctx initialized, else *count value is undefined.
 */
channel_ctx_t **init_multiple_channel_ctx(const char *basename_list, const char *dict_list, const char *fmtlib_file,
                                          int buf_size, int safe_offset, int do_statistics, int *count,
                                          immutable_string_store_t *store) {
	char *__basename_buf = strdup(basename_list), *__nvmeibx_buf = strdup(dict_list);
	int i, j;
	int count_dicts;
	int count_channels;
	char **ch_names = NULL, **dict_names = NULL;

	assert(__basename_buf);
	assert(__nvmeibx_buf);

	/* Prepare strings */
	strcpy(__basename_buf, basename_list);
	strcpy(__nvmeibx_buf, dict_list);
	ch_names = __split_str(__basename_buf, ',', &count_channels);
	assert(ch_names);
	dict_names = __split_str(__nvmeibx_buf, ',', &count_dicts);


	// Allocate anough memory for all ctx, and initialize all to NULL
	channel_ctx_t **res = calloc(1, sizeof(channel_ctx_t *) * count_channels);
	if (!res)
		goto err;

	/* For each channel */
	for (i = 0; i < count_channels; ++i) {
		char *ch_name, *host_name = "";
		/* Start by extracting the hostname if supplied */
		ch_name = strchr(ch_names[i], ':');
		if (!ch_name) ch_name = ch_names[i];
		else {
			/* Validate the hostname */
			char *first_slash = strchr(ch_names[i], '/');
			if (!first_slash || first_slash > ch_name) {
				/*Good hostname*/
				*(ch_name++) = '\0';
				host_name = ch_names[i];
			} else {
				/*Not a hostname*/
				ch_name = ch_names[i];
			}
		}
		res[i] = init_channel_ctx(ch_name, host_name, buf_size, safe_offset, store);

		/* If we have any preload dictionaries - attach them */
		for (j = 0; j < count_dicts; ++j)
			if (!try_attach_dict_to_pool(&res[i]->dicts, dict_names[j], fmtlib_file))
				fprintf(stderr, "ERROR: Could not load dictionary file %s, not citical\n", dict_names[j]);

		/* If we have statistics enabled - init it */
		if (do_statistics) {
			per_channel_stats_t *pcs = calloc(1, sizeof(per_channel_stats_t));
			res[i]->payload = pcs;
			res[i]->do_statistics = 1;
			assert(pcs);
			per_key_stats_t_ht_init(&trace_stats_ht(res[i]));
			per_key_stats_t_ht_init(&token_stats_ht(res[i]));
			per_key_stats_t_ht_init(&func_stats_ht(res[i]));
			per_key_stats_t_ht_init(&file_stats_ht(res[i]));
		}
	}

	/* We do not need these any longer */
	free(ch_names); free(dict_names);
	free(__basename_buf); free(__nvmeibx_buf);

	*count = count_channels;

	return res;

err:
	/*If something bad happened, free all and return NULL
	Free will not fail, since we starting by setting all to NULL, so we will not
	free struff twice*/
	if (__basename_buf) free(__basename_buf);
	if (__nvmeibx_buf) free(__nvmeibx_buf);
	if (ch_names) free(ch_names);
	if (dict_names) free(dict_names);
	free_multiple_channel_ctx(res, *count);
	return NULL;
}

int msg_stream_handler(int argc, char **argv, char *exe, enum output_type outtype) {
	int rv = 0;
	if (argc < 4) return print_help(exe, output_type_str(outtype));
	else {
		int ctxid, cpuid; /*Loop indices*/
		timestamp_t start, end;
		buf_stream_per_cpu_t * **bss = NULL; /* 2D Array of pointers. (nctx x ncpu)->buf_stream. Scary...*/
		msg_stream_t *ms = NULL;
		int color = 0, print_date = 0, print_cpu_id = 1;
		filter_t *filter = NULL;
		serialized_msg_write_stream_t *binstream = NULL;
		immutable_string_store_t *is = NULL;
		const char *fmtlib_file = NULL;
		int do_statistics = 0;
		int only_reliable = 0;
		int nogreet = 0;
		const char *channels, *dicts;
		channel_ctx_t **ctx = NULL;
		int nctx = 0;
		const char *t1, *t2; /*not yet parsed time frame*/
		int have_any_output = 0;

		assert((is = init_immutable_string_store()));

		/*Postional args*/

		/*Directories*/
		dicts = *(argv++);
		if (!strcmp(dicts, "none")) dicts = "";
		channels = *(argv++);
		t1 = *(argv++);
		t2 = *(argv++);
		argc -= 4;

		/*Optional args*/
		while (argc) {
			if (!strcmp(*argv, "-f")) {
				if (--argc <= 0) {
					print_help(exe, output_type_str(outtype));
					goto cleanup;
				}
				filter = _compile_filter(*(++argv), is);
			} else if (!strcmp(*argv, "--color")) {
				color = 1;
			} else if (!strcmp(*argv, "--print-date")) {
				print_date = 1;
			} else if (!strcmp(*argv, "--no-print-cpu-id")) {
				print_cpu_id = 0;
			} else if (!strcmp(*argv, "--fmtlib")) {
				if (--argc <= 0) {
					rv = print_help(exe, output_type_str(outtype));
					goto cleanup;
				}
				fmtlib_file = *(++argv);
			} else if (!strcmp(*argv, "--statistics")) {
				do_statistics = 1;
			} else if (!strcmp(*argv, "--reliable")) {
				only_reliable = 1;
			} else if (!strcmp(*argv, "--nogreet")) {
				nogreet = 1;
			}
			--argc; ++argv;
		}

		/*Prepare context*/
		ctx = init_multiple_channel_ctx(channels, dicts, fmtlib_file, BUFFER_SIZE, RINGBUFFER_SIZE, do_statistics, &nctx, is);
		if (!ctx) {
			rv = print_help(argv[0], NULL);
			goto cleanup;
		}

		/*Read timeframe - must be adter ctx init*/
		_parse_timeframe(t1, t2, &start, &end, ctx, nctx);

		/*Prepare streams*/

		/*Serialized messages strema if needed*/
		if (outtype == OUTTYPE_BINARY)
			binstream = init_serialized_msg_write_stream(STDOUT_FILENO);

		/*Msg stream*/
		ms = start_msg_stream();
		assert(ms);

		/* Buffer streams per CPU per channel */
		assert((bss = calloc(1, nctx*sizeof(buf_stream_per_cpu_t **))));
		for (ctxid = 0; ctxid < nctx; ++ctxid) {
			for_each_active_cpu(ctx[ctxid], cpuid) {
				assert((bss[ctxid] = calloc(1, MAX_CPUS*sizeof(buf_stream_per_cpu_t *))));
				assert((bss[ctxid][cpuid] = start_buf_stream(ctx[ctxid], cpuid, start, end)));
				attach_buf_stream(ms, bss[ctxid][cpuid]);
			}
		}

		if (filter && (GLOBAL_PAGER_DEBUG & PDBG_FILTERS)) {
			printf("Filter dump: ");
			if (filter->ast) print_ast_node(filter->ast);
			printf("\n");
		}

		/***** The main envent loop ******/
		{
			binary_trace_t *bt;
			while ((bt = peek_next_msg(ms))) {
				if ((only_reliable && !msg_stream_is_reliable(ms)) || (nogreet && bt->entry->trace_id == SYSTEM_TRACE_GREETING_ID)) {
					pop_next_msg(ms);
					continue;
				}
				if (outtype != OUTTYPE_BINARY || filter) /*Parse trace actual data only if we need it for filters or for print*/
					bt->arg_count = build_arg_vec(bt->arg_vec, bt->entry, bt->msg, NULL);
				if (bt->entry->trace_id == SYSTEM_TRACE_GREETING_ID || (bt->ts >= start && bt->ts <= end && trace_passes_filter(bt, filter))) {
					/**
					 *@todo Currently do_statistics yields no other prints on the screen.
					  Maybe make it a flag? Doesn't seem important.
					 */
					if (!take_per_channel_stats_t(bt)) {
						if (!IS_SYSTEM_TRACE_ID(bt->entry->trace_id)) have_any_output = 1;
						switch (outtype) {
						case OUTTYPE_BINARY: assert(!write_msg_serialize(binstream, bt)); break;
						case OUTTYPE_TEXT: format_binary_trace(bt, color, print_date, print_cpu_id, bt->ctx->hostname->token); break;
						case OUTTYPE_JSON: format_binary_trace_json(bt); break;
						case OUTTYPE_CSV_STATS_ONLY: format_binary_trace_csv_stats_only(bt); break;
						}
					}
				}
				pop_next_msg(ms);
			}
		}

		if (do_statistics) dump_statistics_multiple(ctx, nctx);

		if (!do_statistics && !have_any_output) rv = ENODATA;

cleanup:

		/*Teardown buffers*/
		if (ms) stop_msg_stream(ms);
		if (binstream) free_serialized_msg_write_stream(binstream);
		for (ctxid = 0; ctxid < nctx; ++ctxid) {
			if (bss[ctxid]) {
				for_each_active_cpu(ctx[ctxid], cpuid) if (bss[ctxid] && bss[ctxid][cpuid])
				    stop_buf_stream(bss[ctxid][cpuid]);
				free(bss[ctxid]);
			}
		}
		free(bss);

		if (is) free_immutable_string_store(is);
		if (ctx) free_multiple_channel_ctx(ctx, nctx);
	}

	return rv;
}

struct stream_merger_ctx {
	serialized_msg_read_stream_t *stream;
	int fd;
	binary_trace_t last;
	const char *hname;
};

int deserialize_stream_handler(int argc, char **argv, char *exe) {
	int rv = 0;
	if (argc < 2) return print_help(exe, "deserialize-msg-stream");
	else {
		stream_merger_t *merger _autoclean_stream_merger = NULL;
		immutable_string_store_t *is = NULL;

		assert((is = init_immutable_string_store()));

		/*Work dir*/
		assert((merger = init_stream_merger(*(argv++), is)));
		argc -= 1;

		/*Init streams*/
		while (argc--) {
			if ((rv = attach_input_to_stream_merger(merger, *argv)) < 0) {
				goto cleanup;
			}
			++argv;
		}

		while (!stream_merger_is_eof(merger)) {
			binary_trace_t *bt = peek_stream_merger(merger);
			bt->arg_count = build_arg_vec(bt->arg_vec, bt->entry, bt->msg, NULL);
			format_binary_trace(bt, 0, 0, 1, peek_stream_merger_hostname(merger));
			if ((rv = pop_stream_merger(merger)) < 0) {
				goto cleanup;
			}
		}

		rv = 0; /*All went well*/

cleanup:
		if (is) free_immutable_string_store(is);
	}

	if (rv < 0) {
		fprintf(stderr, "An error occurred: %s\n", strerror(-rv));
	}

	return rv;
}

int msg_stream_handler_bin(int argc, char **argv, char *exe) {
	return msg_stream_handler(argc, argv, exe, OUTTYPE_BINARY);
}

int msg_stream_handler_txt(int argc, char **argv, char *exe) {
	return msg_stream_handler(argc, argv, exe, OUTTYPE_TEXT);
}

int msg_stream_handler_json(int argc, char **argv, char *exe) {
	return msg_stream_handler(argc, argv, exe, OUTTYPE_JSON);
}

int msg_stream_handler_csv_stats_only(int argc, char **argv, char *exe) {
	return msg_stream_handler(argc, argv, exe, OUTTYPE_CSV_STATS_ONLY);
}

int main(int argc, char **argv) {
	const pager_cmd_t *cmd = pager_cmds;

	/*Minimal args required are command name*/
	if (argc < 2) return print_help(argv[0], NULL);

	while (cmd->name) {
		if (!strcmp(cmd->name, argv[1])) {
			if (!strcmp(argv[2], "--dbg")) {
				if (argc <= 3)
					return print_help(argv[0], NULL);
				GLOBAL_PAGER_DEBUG = atol(argv[3]);
				argc -= 2; argv += 2;
			}
			return cmd->handler(argc - 2, argv + 2, argv[0]);
		}
		++cmd;
	}
	return print_help(argv[0], NULL);
}
