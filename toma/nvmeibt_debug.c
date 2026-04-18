/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>

#include <linux/types.h>
#include <time.h>
#include <syslog.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <inttypes.h>

#include "interfaces/log/nvmeibt_dumper.h"
#include "nvmeibt_toma.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topology.h"
#include "nvmeibt_str.h"
#include "nvmeibt_global.h"
#include "clnt/nvmeibt_client_protocol.h"

// Structure to hold tracer section information
struct tracer_section {
	struct _tracer *start;
	struct _tracer *end;
};

#define MAX_TRACER_SECTIONS 8
static struct tracer_section tracer_sections[MAX_TRACER_SECTIONS];
static int num_tracer_sections = 0;

#define FOR_EACH_TRACER_IN_ALL_SECTIONS(t) \
	for (int __section_idx = 0; __section_idx < num_tracer_sections; __section_idx++) \
		for ((t) = tracer_sections[__section_idx].start; \
			 (t) < tracer_sections[__section_idx].end; ++(t))

#define TRACE_LIST_FILENAME				"/tracelist.txt"
#define TRACE_CONFIG_FILENAME			"/toma_trace.config"
#define	TOMA_CONFIG_PARAMS_FULL_PATH	TOMA_ROOT_DIR "opt/nvmesh/common-repo/tools/toma_rpc.config"
#define STACK_SIZE 100

bool trace_config_updated_by_toma = false;
char *config_params_full_path = TOMA_CONFIG_PARAMS_FULL_PATH;
int nvmeibt_disk_flow_params_try_read_from_config_line(const char*config, int *n_matches);		// Load parameters from config line
int nvmeibt_debug_config_params_parse(char *line, int *n_matches);

#define SANITIZE_STR_END(str, len) \
		len = strlen(str); \
		while (((str[len - 1] == '\n') || (str[len - 1] == '\r')) && (len > 0)) \
			str[--len] = '\0'

static void try_to_read_sw_ver(char *str, uint32_t *sw_ver)
{
	size_t	line_len;

	if (!strncmp(str, TOMA_SW_VER_STRING, sizeof(TOMA_SW_VER_STRING) - 1)) {
		SANITIZE_STR_END(str, line_len);
		if (line_len > (strlen(TOMA_SW_VER_STRING) + 3))
			sscanf(str + strlen(TOMA_SW_VER_STRING) + 1, "%x", sw_ver);
	}
}

int nvmeibt_debug_register_tracer_section(struct _tracer *start, struct _tracer *end)
{
	if (!start || !end || start >= end) {
		N_Ef(nd_register_tracer_section_invalid_params, "Invalid tracer section parameters: start=@PTR end=@PTR", (void*)start, (void*)end);
		return -EINVAL;
	}

	if (num_tracer_sections >= MAX_TRACER_SECTIONS) {
		N_Ef(nd_register_tracer_section_max_sections_reached, "Maximum number of tracer sections @INT reached", MAX_TRACER_SECTIONS);
		return -ENOMEM;
	}

	for (int i = 0; i < num_tracer_sections; i++) {
		if (tracer_sections[i].start == start && tracer_sections[i].end == end) {
			N_Ef(nd_register_tracer_section_already_registered, "Tracer section already registered: start=@PTR end=@PTR", (void*)start, (void*)end);
			return -EEXIST;
		}
	}

	tracer_sections[num_tracer_sections].start = start;
	tracer_sections[num_tracer_sections].end = end;
	num_tracer_sections++;

	return 0;
}

void nvmeibt_debug_init_tracer_sections(void)
{
	extern struct _tracer __tracer_start[], __tracer_end[];
	int rv;

	if ((rv = nvmeibt_debug_register_tracer_section(__tracer_start, __tracer_end)) != 0) {
		N_Ef(nm_register_main_tracer_section_failed, "Failed to register main tracer section: rv=@RV", rv);
		nvmeibt_abort(ES_FATAL);
	}
}

void read_rpc_config_from_persist(bool is_initial_read)
{
	char						config[1024];
	static struct stat			config_stat_last;
	struct stat					config_stat;
	uint32_t					sw_ver = 0;
	FILE						*f = 0;
	__MEASURE_TOOK_INIT();

	if (stat(config_params_full_path, &config_stat))
		goto out;

	if (config_stat.st_mtime == config_stat_last.st_mtime)
		goto out;

	if (trace_config_updated_by_toma) {
		trace_config_updated_by_toma = false;
		goto out;
	}

	f = fopen(config_params_full_path, "r");
	__MEASURE_TOOK(N_IMf(nr5e38m, "fopen() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	if (!f)
		goto out;

	if (!fgets(config, sizeof(config), f)) {
		N_Wf(ga19a6b, "file=@STR is empty", config_params_full_path);
		goto out;
	}

	try_to_read_sw_ver(config, &sw_ver);
	if (sw_ver == 0) {
#if 0 /* for future versions */
		N_WTf(hs2n85n, "SW_VER not found");
		if (is_initial_read)
			nvmeibt_abort(ES_FATAL);
		else
			goto out;
#else
		goto continue_reading;
#endif
	} else if (sw_ver == 0x00000310 || sw_ver == 0x00000330) {
		/* v0x340 bumped the wire format of praid_serialized_topo (added
		 * CDV allocator identity fields).  RPC-config format is unchanged,
		 * so older persisted RPC configs remain readable. */
		N_WTf(hj3a06n, "SW_VER old but compatible @X != @X", sw_ver, TOMA_SW_COMPATIBILITY_VER);
	} else if (sw_ver != TOMA_SW_COMPATIBILITY_VER) {
		N_WTf(hj3a05n, "SW_VER mismatch");
		if (is_initial_read)
			nvmeibt_abort(ES_FATAL);
		else
			goto out;
	}

	nvmeibt_disk_flow_params_reset_models_before_new_scan();
	while (fgets(config, sizeof(config), f)) {
		int		n_matches = 0;
		size_t	line_len;

continue_reading:
		SANITIZE_STR_END(config, line_len);
		line_len = strlen(config);
		if (!line_len)
			continue;
		N_Tf(hsuk35n, "@STR", config);
		fprintf(stderr, "%s\n", config);

		if (config[0] == '#')
    		continue; /* Ignore comments */

		if (nvmeibt_disk_flow_params_try_read_from_config_line(config + 2, &n_matches)) {
			/* Nothing to do, line was consumed*/
		}
		else if (nvmeibt_debug_config_params_parse(config + 2, &n_matches)) {
			// line consumed
		}

		if (!n_matches) {
			N_WTf(ploi98c, "No config param matches '@BUFFER_DUMP'", config + 2);
		}
	}
	config_stat_last = config_stat;

out:
	if (f)
		fclose(f);
    return;
}

static const uint8_t			explicit_plus = 0x10;
static const uint8_t			explicit_minus = 0x20;
static struct stat				std_config_stat_last;

int update_traces_turn_all_on_or_off(char plus_or_minus, bool is_forced)
{
	struct _tracer			*t;
	uint8_t					explicit_change = explicit_plus;	// Default. As if "+ all"
	int						n_matches = 0;

	N_IMf(agunwi4, "@CHAR all", plus_or_minus);
	if (plus_or_minus == '-') {
		explicit_change = explicit_minus;
	} else if (plus_or_minus != '+') {
		N_ETf(rviopvw, "OOPS plus_or_minus=@CHAR", plus_or_minus);
		nvmeibt_abort(ES_FATAL);
	}
	//
	FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
		t->plus_minus_flag = explicit_change;
		//_Tf("filename=%s lineno=%d t->plus_minus_flag=%d\n", t->filename, t->lineno, t->plus_minus_flag);
		n_matches++;
	}
	if (is_forced) {
		memset(&(std_config_stat_last.st_mtime), 0, sizeof(std_config_stat_last.st_mtime)); // Force reread in future update_traces()
	}
	return n_matches;
}

char	std_trace_config_file_name[512] = "";

static const char *basename_const(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

void update_traces(void)
{
	struct _tracer			*t;
	uint8_t					explicit_change;
	char					config[1024];
	FILE					*f = 0;
	int						saved_errno;
	struct stat				config_stat;
	uint32_t				sw_ver = 0;
	static bool				is_initial_read;

	__MEASURE_TOOK_INIT();
	NFIN;
	if (std_trace_config_file_name[0] == '\0') {	// Only if was never created
		snprintf(std_trace_config_file_name, sizeof(std_trace_config_file_name),"%s/%s",
				 nvmeibt_toma_get_log_dir_name(),
				 TRACE_CONFIG_FILENAME);
	}
	//
	if (stat(std_trace_config_file_name, &config_stat)) {
		goto out;
	}
	if (config_stat.st_mtime == std_config_stat_last.st_mtime) {     // We read it only if modified
		goto out;
	}
	std_config_stat_last = config_stat;		// Remember only the std (default) date. Not affected by alternative file
	//
	errno = 0;
	f = fopen(std_trace_config_file_name, "r");
	saved_errno = errno;
	__MEASURE_TOOK(N_IMf(5vh8jwk, "fopen() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	errno = saved_errno;
	N_Tf(gaimw6b, "fopen(@STR) @AUTO_ERRNO", std_trace_config_file_name);
	if (!f) {
		goto out;
	}

	if (!fgets(config, sizeof(config), f)) {
		N_WTf(ga1cw6b, "file=@STR is empty", std_trace_config_file_name);
		goto out;
	}

	try_to_read_sw_ver(config, &sw_ver);
	if (sw_ver == 0) {
#if 0 /* for future versions */
		N_WTf(hs6785n, "SW_VER not found");
		if (is_initial_read)
			nvmeibt_abort(ES_FATAL);
		else
			goto out;
#else
		goto continue_reading;
#endif
	} else if (sw_ver == 0x00000310 || sw_ver == 0x00000330) {
		/* See comment in read_rpc_config_from_persist: v0x340 only bumped
		 * the praid wire format; other on-disk formats are unchanged. */
		N_WTf(hj3836n, "SW_VER old but compatible @X != @X", sw_ver, TOMA_SW_COMPATIBILITY_VER);
	} else if (sw_ver != TOMA_SW_COMPATIBILITY_VER) {
		N_WTf(hj3835n, "SW_VER mismatch @X!=@X", sw_ver, TOMA_SW_COMPATIBILITY_VER);
		if (is_initial_read)
			nvmeibt_abort(ES_FATAL);
		else {
			goto out;
		}
	}

	while (fgets(config, sizeof(config), f)) {
		int					n_matches = 0;
		size_t				line_len;

continue_reading:
		SANITIZE_STR_END(config, line_len);
		if (!line_len)
			continue;
		fprintf(stderr, "%s\n", config);

		if (config[0] == '-')
			explicit_change = explicit_minus;
		else if (config[0] == '+') {
			explicit_change = explicit_plus;
		} else {
			if (config[0] != '#') {
				N_WTf(ayirmop, "Cannot parse the argument '@CONFIG_STR'", config);
			} else { /* Ignore comments */}
			continue;
		}

		// fprintf(stderr, "value_or=%d value_and=%d\n", value_or, value_and);

		if (!strncmp(config + 2, "filename ", 9)) {
			const char *filename = config + 11;
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				// fprintf(stderr, "%d cmp '%s' '%s'\n", __LINE__, t->filename, filename);
				if (!strcmp(basename_const(t->filename), filename)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		}
		else if (!strncmp(config + 2, "function ", 9)) {
			const char *function = config + 11;
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				// fprintf(stderr, "%d cmp '%s' '%s'\n", __LINE__, t->function, function);
				if (!strcmp(t->function, function)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		}
		else if (!strncmp(config + 2, "LVL ", sizeof("LVL ") - 1)) {
			const char *lvl = config + 2 + strlen("LVL ");
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				// fprintf(stderr, "%d cmp '%s' '%s'\n", __LINE__, t->function, function);
				if (!strcmp(t->lvl, lvl)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		}
		else if (!strncmp(config + 2, "line ", 5)) {
			char filename[line_len];
			int lineno;

			sscanf(config + 7, "%s %d", filename, &lineno);
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				if (lineno == t->lineno && !strcmp(t->filename, filename)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		}
		else if (!strcmp(config + 2, "all")) {
			n_matches = update_traces_turn_all_on_or_off(config[0], 0);
		}

		if (n_matches) {
			N_Tf(c4fjql2, "'@STR' n_matches=@INT", config, n_matches);
		} else {
			N_WTf(uv27bh4, "No trace point matches '@STR'", config);
		}
	}

	FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
		// fprintf(stderr, "%d t->plus_minus_flag=%d\n", __LINE__, t->plus_minus_flag);
		if (t->plus_minus_flag == explicit_plus) {
			t->is_on = 1;
		} else if (t->plus_minus_flag == explicit_minus) {
			t->is_on = 0;
		} else {
			t->is_on = TRACE_METADATA_is_on_DEFAULT;
		}
		t->plus_minus_flag = 0;
		/* fprintf(stderr, "%d: %s, %s, %d, %s%s",
			t->is_on,
			t->filename, t->function, t->lineno, t->format,
			t->format[strlen(t->format) - 1] == '\n' ? "" : "\n"); */
	}
out:
	is_initial_read = 0;
	__MEASURE_TOOK(N_IMf(ycbjiw3, "work Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	if (f) {
		fclose(f);
		__MEASURE_TOOK(N_IMf(zbmeofh, "fclose() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	}
	//NNVMEIBT_TOMA_FREE(v5t9sl3, traceconfig);
	NFOUT;
}

extern int64_t nvmeibt_topology_max_praids_in_a_report;
extern int64_t nvmeibt_toma_is_not_reporting_data_segs_gpt_entries;
extern int64_t nvmeibt_follower_keep_alive_secs;
extern int64_t nvmeibt_leader_keep_alive_secs;
extern int64_t nvmeibt_toma_report_target_min_between_secs;
extern int64_t generic_block_device_support;
extern int64_t praid_time_from_activation_attempt_to_degraded_mode_sec;
extern int64_t disable_periodic_smart_polling;
extern int64_t udp_max_header_length;
extern int64_t nvmeibt_kafka_get_offset_timeout_secs;
extern int64_t recovery_timeout_wait_client_sec;
extern int64_t nvmeibt_topology_freeze_topo;
extern int64_t nvmeibt_cdv_extent_zero_on_free;
#ifdef TOMA_IB_ROCE
extern int64_t ibud_enable_periodic_traces;
#endif

struct oper_param_t {
	char *name;
	int64_t		default_value;
	int64_t		*ptr;
	void		(*setf)(int64_t);
	int64_t		(*getf)(void);
} oper_params[] = {
		{"raft_leader_heartbeat_timeout_usec",				(RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT / 1000),	// Cannot convert to timespec for backwards compatibility
			NULL, nvmeibt_raft_set_effective_leader_heartbeat_timeout_usec_USED_ONLY_BY_RPC, nvmeibt_raft_get_effective_leader_heartbeat_timeout_usec_USED_ONLY_BY_RPC},
		{"raft_min_election_timeout_factor",				RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT,
			NULL, nvmeibt_raft_set_effective_min_election_timeout_factor_USED_ONLY_BY_RPC, nvmeibt_raft_get_effective_min_election_timeout_factor},
		{"max_n_simultaneous_dirty_rebuild",				MAX_N_SIMULTANEOUS_DIRTY_REBUILD_DEFAULT,
			NULL, nvmeibt_recovery_set_max_n_simultaneous_dirty_rebuild, nvmeibt_recovery_get_max_n_simultaneous_dirty_rebuild},
		{"max_n_simultaneous_stale_and_txid_rebuild",		MAX_N_SIMULTANEOUS_STALE_AND_TXID_REBUILD_DEFAULT,
			NULL, nvmeibt_recovery_set_max_n_simultaneous_stale_and_txid_rebuild, nvmeibt_recovery_get_max_n_simultaneous_stale_and_txid_rebuild},
		{"max_n_simultaneous_scrubbing",					MAX_N_SIMULTANEOUS_SCRUBBING_DEFAULT,
			NULL, nvmeibt_recovery_set_max_n_simultaneous_scrubbing, nvmeibt_recovery_get_max_n_simultaneous_scrubbing},
		{"scrubbing_default_period_days",					SCRUBBING_DEFAULT_PERIOD_DAYS_DEFAULT,
			NULL, nvmeibt_recovery_set_scrub_default_period_days, nvmeibt_recovery_get_scrub_default_period_days},
		{"scrubbing_n_blksets_per_iteration",				SCRUBBING_N_BLKSETS_PER_ITERATION_DEFAULT,
			NULL, nvmeibt_recovery_set_n_blksets_per_scrub_iteration, nvmeibt_recovery_get_n_blksets_per_scrub_iteration},
		{"scrubbing",										SCRUBBING_DEFAULT,
			NULL, nvmeibt_recovery_set_is_scrub_enabled, nvmeibt_recovery_get_is_scrub_enabled},
		{"recovery_client_batch_n_blksets",					NVMEIBT_CLIENT_PROTOCOL_BATCH_SIZE_DONT_CARE,
			NULL, nvmeibt_recovery_client_batch_n_blksets_set, nvmeibt_recovery_client_batch_n_blksets_get},
		{"topology_max_praids_in_a_report",					TOPOLOGY_MAX_PRAIDS_IN_A_REPORT_DEFAULT,
			&nvmeibt_topology_max_praids_in_a_report, NULL, NULL},
		{"toma_do_not_report_disk_segment_gpts",			TOMA_DO_NOT_REPORT_DISK_SEGMENT_GPTS_DEFAULT,
			&nvmeibt_toma_is_not_reporting_data_segs_gpt_entries, NULL, NULL},
		{"mgmt_keep_alive_secs",							MGMT_KEEP_ALIVE_SECS_DEFAULT,
			&nvmeibt_follower_keep_alive_secs, NULL, NULL},
		{"mgmt_leader_keep_alive_secs",						MGMT_LEADER_KEEP_ALIVE_SECS_DEFAULT,
			&nvmeibt_leader_keep_alive_secs, NULL, NULL},
		{"report_target_min_between_secs",					REPORT_TARGET_MIN_BETWEEN_SECS_DEFAULT,
			&nvmeibt_toma_report_target_min_between_secs, NULL, NULL},
		{"generic_block_device_support",					GENERIC_BLOCK_DEVICE_SUPPORT_DEFAULT,
			&generic_block_device_support, NULL, NULL},
		{"praid_activation_timeout_sec",					PRAID_ACTIVATION_TIMEOUT_SEC_DEFAULT,
			&praid_time_from_activation_attempt_to_degraded_mode_sec, NULL, NULL},
		{"udp_max_header_length",							MAX_HEADER_LENGTH_DEFAULT,
			&udp_max_header_length, NULL, NULL},
		{"disable_periodic_smart_polling",					DISABLE_PERIODIC_SMART_POLLING_DEFAULT,
			NULL, nvmeibt_local_disk_set_disable_periodic_smart_polling, NULL},
		{"tracer_debug_level",								TRACER_DEBUG_LEVEL_DEFAULT,
			NULL, nvmeibt_binary_tracing_set_tracer_debug_level, nvmeibt_binary_tracing_get_tracer_debug_level},
		{"log_snapshotting_mode",							LOG_SNAPSHOTTING_MODE_START_TIME_SEC_DEFAULT,
			NULL, nvmeibt_global_set_log_snapshotting_mode, nvmeibt_global_get_log_snapshotting_mode},
		{"kafka_get_offset_timeout_secs",					KAFKA_GET_OFFSET_TIMEOUT_SECS_DEFAULT,
			&nvmeibt_kafka_get_offset_timeout_secs, NULL, NULL},
		{"attach_timeout_sec",								ATTACH_TIMEOUT_SEC_DEFAULT,
			&recovery_timeout_wait_client_sec, NULL, NULL},
		{"max_wait_for_non_registrable_seg_sec",			PRAID_LEADER_MAX_NSEC_WAIT_FOR_NON_REGISTRABLE_SEG_TO_APPLY_DEFAULT,
				NULL, nvmeibt_binary_tracing_set_tracer_debug_level, nvmeibt_binary_tracing_get_tracer_debug_level},
		{"freeze_topology",									0,
				&nvmeibt_topology_freeze_topo, NULL, NULL},
		{"cdv_extent_zero_on_free",							CDV_EXTENT_ZERO_ON_FREE_DEFAULT,
				&nvmeibt_cdv_extent_zero_on_free, NULL, NULL},
#ifdef TOMA_IB_ROCE
		{"enable_networking_periodic_traces",				ENABLE_NETWORKING_PERIODIC_TRACES_DEFAULT,
			&ibud_enable_periodic_traces, NULL, NULL},
#endif
};

static int _debug_config_params_set(struct oper_param_t *param, const char *valstr)
{
	int rc = 0;
	uint64_t val = 0;

	if (strncmp(valstr, "default", 7)==0) {
		rc = 1;
		val = param->default_value;
	}
	else if (sscanf(valstr, "%" PRIu64, &val) > 0) {
		rc = 1;
	}
	if (rc) {
		N_IMf(trace_debug_config_params_set, "Set config parameter @STR=@LEN_LONG", param->name, val);
		if (param->ptr)
			*param->ptr = val;
		if (param->setf)
			param->setf(val);
	}
	return rc;
}

int nvmeibt_debug_config_params_set(const char *p_name, const char *valstr)
{
	int rc = 0;
	int i;
	for (i=0; i<ARRAY_SIZE(oper_params); i++) {
		struct oper_param_t *param = &oper_params[i];
		if (strncmp(p_name, param->name, strlen(param->name))==0) {
			N_Tf(vstvgwj, "@STR=@STR", param->name, valstr);
			rc = _debug_config_params_set(param, valstr);
			break;
		}
	}
	return rc;
}

int nvmeibt_debug_config_params_parse(char *line, int *n_matches)
{
	int i;

	if (strncmp(line, "param ", 6) == 0) {
		line += 6;
	}

	for (i=0; i<ARRAY_SIZE(oper_params); i++) {
		struct oper_param_t *param = &oper_params[i];
		int len = strlen(param->name);
		if (strncmp(line, param->name, len)==0) {
			*n_matches = _debug_config_params_set(param, line+len+1);
			break;
		}
	}
	return *n_matches;
}

void nvmeibt_debug_config_params_print(struct nvmeibt_Str *s, bool print_values, bool print_defaults)
{
	int i;
	nvmeibt_Str_sprintf(s, "%s=%x\n", TOMA_SW_VER_STRING, TOMA_SW_COMPATIBILITY_VER);
	for (i=0; i<ARRAY_SIZE(oper_params); i++) {
		struct oper_param_t *param = &oper_params[i];

		if (print_values) {
			int64_t val=0;
			if (param->getf)
				val = param->getf();
			else if (param->ptr)
				val = *param->ptr;

			if (!print_defaults && val == param->default_value)
				continue;

			nvmeibt_Str_sprintf(s, "+ param %s %" PRIu64 "\n", param->name, val);
		}
		else {
			nvmeibt_Str_sprintf(s, "    %s\n", param->name);
		}
	}
}

void prepare_all_traces(void)
{
	struct _tracer *t;

	const char *dirname;
	char *tracelist;
	size_t tracelist_len;

	FILE *f = 0;
	__MEASURE_TOOK_INIT();

	dirname = nvmeibt_toma_get_log_dir_name();
	tracelist_len = strlen(dirname) + strlen(TRACE_LIST_FILENAME) + 2;
	tracelist = NNVMEIBT_TOMA_MALLOC(hu821mv, tracelist_len);
	nvmeibt_strlcpy(tracelist, dirname, tracelist_len);
	nvmeibt_strlcat(tracelist, TRACE_LIST_FILENAME, tracelist_len);
	f = fopen(tracelist, "w");
	__MEASURE_TOOK(N_IMf(warn_prepare_all_traces_measure_0, "fopen() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	NNVMEIBT_TOMA_FREE(trace_1_debug_prepare_all_traces, tracelist);

	FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
		if (f) {
			const char *eol = my_strchrnul(t->format_str, '\n');
			fprintf(f, "%s, %s, %d, %s, %.*s\n", t->filename, t->function, t->lineno, t->lvl, (int)(eol - t->format_str), t->format_str);
		}
	}
	if (f) {
		fclose(f);
		__MEASURE_TOOK(N_IMf(warn_prepare_all_traces_measure_1, "fclose() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	}
}

void print_stack(void) {
	int j, nptrs;
	void *buffer[STACK_SIZE];
	char **strings;
	nptrs = backtrace(buffer, STACK_SIZE);
	strings = backtrace_symbols(buffer, nptrs);
	for (j = 0; j < nptrs; j++) {
		syslog(LOG_ERR," %s\n", strrchr(strings[j], '/'));
	}
}

void print_stack_warn(void) {
	int j, nptrs;
	void *buffer[STACK_SIZE];
	char **strings;
	nptrs = backtrace(buffer, STACK_SIZE);
	strings = backtrace_symbols(buffer, nptrs);
	for (j = 0; j < nptrs; j++) {
		N_Wf(warn_debug_print_stack_warn, " @STRRCHR", strrchr(strings[j], '/'));
	}
}

// Not thread safe but who cares, as long as messages under 5000 will not crash process
// Temporary (TODO)
int trace_to_printf_fmt(char* printf_fmt, int printf_fmt_len, const char* auto_generated_printf_fmt, const char *filename, int line, const char *func_name)
{
	int		rv = -1;
    char 	*p = printf_fmt;
	int		auto_generated_printf_fmt_len = (int)strlen(auto_generated_printf_fmt);
    // keep 100 for safety, and needs at least 120
    if ((printf_fmt_len > 10) && (auto_generated_printf_fmt_len > (int)(printf_fmt_len - 100))) {
    	p += sprintf(p, "STRING TOO LONG!");
        goto out;
    }
    p += sprintf(p, "%s[%d]:%s:%s", filename, line, func_name, auto_generated_printf_fmt);
	// remove last "\n"
	if (*(p - 1) == '\n') {
		--p;
    }
	*p = '\0';
	rv = 0;
out:
    return rv;
}

