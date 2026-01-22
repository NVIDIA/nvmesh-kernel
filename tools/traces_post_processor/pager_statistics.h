/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef PAGER_STATISTICS_H
#define PAGER_STATISTICS_H

/**
 * @todo: Better encapsulation is required
 * Statistics object shall become private
 * Only minimal interface exposed
 * Implementation shall be in a separate C file
 */

#include "pager_infra.h"

typedef struct per_key_stats {
	immutable_string_t key;
	unsigned long count;
	unsigned long data_size;
} per_key_stats_t;

DECLARE_NUMERIC_KEY_HASH_AND_GET(per_key_stats_t, key.token, 8, );
DECLARE_HASHTABLE(per_key_stats_t, 8);
DECLARE_NUMERIC_KEY_UTIL_FUNCTIONS(per_key_stats_t, immutable_string_t, key, );

typedef struct per_channel_stats {
	struct per_key_stats_t_ht per_trace;
	struct per_key_stats_t_ht per_token;
	struct per_key_stats_t_ht per_func;
	struct per_key_stats_t_ht per_file;
	timestamp_t first_trace_ts;
	timestamp_t last_trace_ts;
	unsigned long long count_system_trace;
} per_channel_stats_t;

#define ch_stats(ctx) ((per_channel_stats_t *)(ctx)->payload)
#define trace_stats_ht(ctx) ((per_channel_stats_t *)(ctx)->payload)->per_trace
#define token_stats_ht(ctx) ((per_channel_stats_t *)(ctx)->payload)->per_token
#define func_stats_ht(ctx) ((per_channel_stats_t *)(ctx)->payload)->per_func
#define file_stats_ht(ctx) ((per_channel_stats_t *)(ctx)->payload)->per_file
#define trace_stats(ctx, key) per_key_stats_t_ht_get(&trace_stats_ht(ctx), key)
#define token_stats(ctx, key) per_key_stats_t_ht_get(&token_stats_ht(ctx), key)
#define func_stats(ctx, key) per_key_stats_t_ht_get(&func_stats_ht(ctx), key)
#define file_stats(ctx, key) per_key_stats_t_ht_get(&file_stats_ht(ctx), key)

static int first_iteration = 1; /*@todo: dirty, but good enough - I am not spending time on statistics*/

void dump_per_key_stats_t(per_key_stats_t *elem) {
	if (!first_iteration)
		printf(",\n");
	printf("\t\t\"%s\" : {\n"
	       "\t\t\t\"count\" : %lu,\n"
	       "\t\t\t\"size\" : %lu}",
	       elem->key.token, elem->count, elem->data_size);
	first_iteration = 0;
}

void dump_per_key_stats_t_ht(struct per_key_stats_t_ht *ht, const char *title) {
	printf("\t\"%s\" : {\n", title);
	first_iteration = 1;
	per_key_stats_t_ht_doall(ht, dump_per_key_stats_t);
	printf("\n\t}");
}

void dump_per_channel_stats_t(per_channel_stats_t *stats) {
	printf("{\n");
	printf("\t\"first_trace_ts_ns\":%llu,\n", stats->first_trace_ts);
	printf("\t\"last_trace_ts_ns\":%llu,\n", stats->last_trace_ts);
	printf("\t\"count_system_trace\":%llu,\n", stats->count_system_trace);
	dump_per_key_stats_t_ht(&stats->per_trace, "TRACES");
	printf(",\n");
	dump_per_key_stats_t_ht(&stats->per_token, "TOKENS");
	printf(",\n");
	dump_per_key_stats_t_ht(&stats->per_func, "FUNCTIONS");
	printf(",\n");
	dump_per_key_stats_t_ht(&stats->per_file, "FILES");
	printf("\n}");
}

int take_per_channel_stats_t(binary_trace_t *bt) {
	if (bt->ctx->do_statistics) {
		int argi;
		if (bt->entry->trace_id == SYSTEM_TRACE_GREETING_ID)
			return 1;
		if (!ch_stats(bt->ctx)->first_trace_ts){
			ch_stats(bt->ctx)->first_trace_ts = bt->ts;
		}
		ch_stats(bt->ctx)->last_trace_ts = bt->ts;
		if (IS_SYSTEM_TRACE_ID(bt->entry->trace_id))
			ch_stats(bt->ctx)->count_system_trace++;
		if (bt->entry->trace_name) {
			per_key_stats_t *stats = trace_stats(bt->ctx, *bt->entry->trace_name);
			assert(stats);
			stats->count++;
			if (!IS_SYSTEM_TRACE_ID(bt->entry->trace_id))
				stats->data_size += trace_actual_length(bt->entry, bt->msg);
		}
		if (bt->entry->func_name) {
			per_key_stats_t *stats = func_stats(bt->ctx, *bt->entry->func_name);
			assert(stats);
			stats->count++;
			if (!IS_SYSTEM_TRACE_ID(bt->entry->trace_id))
				stats->data_size += trace_actual_length(bt->entry, bt->msg);
		}
		if (bt->entry->file_name) {
			per_key_stats_t *stats = file_stats(bt->ctx, *bt->entry->file_name);
			assert(stats);
			stats->count++;
			if (!IS_SYSTEM_TRACE_ID(bt->entry->trace_id))
				stats->data_size += trace_actual_length(bt->entry, bt->msg);
		}
		foreach_arg(argi, bt->entry) {
			per_key_stats_t *stats = token_stats(bt->ctx, *bt->entry->args[argi].name);
			assert(stats);
			stats->count++;
			if (bt->entry->args[argi].type != ARG_STRING)
				stats->data_size += bt->entry->args[argi].len;
			else
				stats->data_size += strlen((const char *)bt->arg_vec[argi]) + 1;
		}
		return 1;
	} else {
		return 0;
	}
}

#endif /*PAGER_STATISTICS_H*/
