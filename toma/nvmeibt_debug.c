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
#include "nvmeibt_toma.h"
#include "interfaces/log/nvmeibt_binary_tracing.h"
#include "nvmeibt_seg_active.h"
#include "nvmeibt_topology.h"
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

#define TRACE_LIST_FILENAME				"/tracelist.txt"			// Toma outputs all its traces so developer knows which traces he can turn on/off
#define TRACE_CONFIG_FILENAME			"toma_trace.config"
#define TOMA_PARAMS_ENCODING_STRING				"SW_VER"			// Very confusing, but backwards compatible!!!

static struct config_params_io_t {
	char *full_path;
	time_t last_read_time;
	bool was_updated_by_toma;		// Just an optimization, dont re-read a file that we already wrote
} config_params = { TOMA_ROOT_DIR "opt/nvmesh/common-repo/tools/toma_rpc.config", 0, false};
int nvmeibt_disk_flow_params_try_read_from_config_line(const char*config, int *n_matches);		// Load parameters from config line
int nvmeibt_debug_config_params_parse(char *line, int *n_matches);

static inline size_t SANITIZE_STR_END(char *str)
{
	size_t len = strlen(str);
	while ((len > 0) && (str[len - 1] <= ' '))		// Remove all trailing invisible characters.
		str[--len] = '\0';
	return len;
}

static void try_to_read_params_encoding_ver(char *str, uint32_t *ver) {
	if (!strncmp(str, TOMA_PARAMS_ENCODING_STRING, sizeof(TOMA_PARAMS_ENCODING_STRING) - 1)) {
		const size_t line_len = SANITIZE_STR_END(str);
		if (line_len > (strlen(TOMA_PARAMS_ENCODING_STRING) + 3))
			sscanf(str + strlen(TOMA_PARAMS_ENCODING_STRING) + 1, "%x", ver);
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

static bool __is_unsupported_version_of_params_config(uint32_t ver, bool should_abort)
{
	if (ver > TOMA_ENCODING_VER) {
		N_WTf(hj3a05n, "Params ENC_VER mismatch too high @X > (max=@X), SW_VER=@X", ver, TOMA_ENCODING_VER, TOMA_SW_VER);
		if (should_abort)
			nvmeibt_abort(ES_FATAL);
		return true;
	}
	return false;
}

void persist_params_in_cfg_file(const struct nvmeibt_Str *s)
{
	int fd = open(config_params.full_path, O_RDWR | O_TRUNC | O_CREAT, 0644);
	if (fd>=0) {
		_Str_fwrite(s, fd);
		close(fd);
		config_params.was_updated_by_toma = true;
	}
}

void read_rpc_config_from_persist(bool is_initial_read)
{
	char						config[1024];
	struct stat					config_stat;
	uint32_t					params_encode_ver = 0;
	FILE						*f = 0;
	__MEASURE_TOOK_INIT();

	if (stat(config_params.full_path, &config_stat))
		return;	// File does not exist
	if (config_stat.st_mtime == config_params.last_read_time)
		return;	// Already read this file

	if (config_params.was_updated_by_toma) {
		config_params.was_updated_by_toma = false;
		return;
	}

	f = fopen(config_params.full_path, "r");
	__MEASURE_TOOK(N_IMf(nr5e38m, "fopen() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	if (!f)
		goto out;
	if (!fgets(config, sizeof(config), f)) {
		N_Wf(ga19a6b, "file=@STR is empty", config_params.full_path);
		goto out;
	}

	try_to_read_params_encoding_ver(config, &params_encode_ver);
	if (params_encode_ver == 0) {				// Support for old config files without version (TOMA_ENCODING_VER_OLDEST_SUPPORTED)
		goto continue_reading;
	} else if (__is_unsupported_version_of_params_config(params_encode_ver, is_initial_read)) {
		goto out;
	}

	nvmeibt_disk_flow_params_reset_models_before_new_scan();
	while (fgets(config, sizeof(config), f)) {
		int		n_matches = 0;
		size_t	line_len;

continue_reading:
		if (config[0] == '#')
			continue; /* Ignore comments */
		line_len = SANITIZE_STR_END(config);
		if (!line_len)
			continue;
		N_Tf(hsuk35n, "@STR", config);

		if (nvmeibt_disk_flow_params_try_read_from_config_line(config + 2, &n_matches)) {
			/* Nothing to do, line was consumed*/
		} else if (nvmeibt_debug_config_params_parse(config + 2, &n_matches)) {
			// line consumed
		}
		if (!n_matches) {
			N_WTf(ploi98c, "No config param matches '@STR'", config + 2);
		}
	}
	config_params.last_read_time = config_stat.st_mtime;		// Mark that we read this file
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

void update_traces(void) {
	struct _tracer			*t;
	uint8_t					explicit_change;
	char					config[1024];
	FILE					*f = 0;
	int						saved_errno;
	struct stat				config_stat;
	uint32_t				params_encode_ver = 0;
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

	try_to_read_params_encoding_ver(config, &params_encode_ver);
	if (params_encode_ver == 0) {				// Support for old config files without version (TOMA_ENCODING_VER_OLDEST_SUPPORTED)
		goto continue_reading;
	} else if (__is_unsupported_version_of_params_config(params_encode_ver, is_initial_read)) {
		goto out;
	}

	while (fgets(config, sizeof(config), f)) {
		int					n_matches = 0;
		size_t				line_len;

continue_reading:
		if (config[0] == '#')
			continue; /* Ignore comments */
		line_len = SANITIZE_STR_END(config);
		if (!line_len)
			continue;
		fprintf(stderr, "%s\n", config);

		if (config[0] == '-')
			explicit_change = explicit_minus;
		else if (config[0] == '+') {
			explicit_change = explicit_plus;
		} else {
			N_WTf(ayirmop, "Cannot parse the argument '@CONFIG_STR'", config);
			continue;
		}

		if (!strncmp(config + 2, "filename ", 9)) {
			const char *filename = config + 11;
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				if (!strcmp(kbasename(t->filename), filename)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		} else if (!strncmp(config + 2, "function ", 9)) {
			const char *function = config + 11;
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				if (!strcmp(t->function, function)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		} else if (!strncmp(config + 2, "LVL ", sizeof("LVL ") - 1)) {
			const char *lvl = config + 2 + strlen("LVL ");
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				if (!strcmp(t->lvl, lvl)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		} else if (!strncmp(config + 2, "line ", 5)) {
			char filename[line_len];
			int lineno;

			sscanf(config + 7, "%s %d", filename, &lineno);
			FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
				if (lineno == t->lineno && !strcmp(t->filename, filename)) {
					t->plus_minus_flag = explicit_change;
					n_matches++;
				}
			}
		} else if (!strcmp(config + 2, "all")) {
			n_matches = update_traces_turn_all_on_or_off(config[0], 0);
		}

		if (n_matches) {
			N_Tf(c4fjql2, "'@STR' n_matches=@INT", config, n_matches);
		} else {
			N_WTf(uv27bh4, "No trace point matches '@STR'", config);
		}
	}

	FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
		if (t->plus_minus_flag == explicit_plus) {
			t->is_on = 1;
		} else if (t->plus_minus_flag == explicit_minus) {
			t->is_on = 0;
		} else {
			t->is_on = TRACE_METADATA_is_on_DEFAULT;
		}
		t->plus_minus_flag = 0;
	}
out:
	is_initial_read = 0;
	__MEASURE_TOOK(N_IMf(ycbjiw3, "work Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	if (f) {
		fclose(f);
		__MEASURE_TOOK(N_IMf(zbmeofh, "fclose() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	}
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
			NULL, nvmeibt_raft_set_max_wait_for_non_registrable_seg_sec, nvmeibt_raft_get_max_wait_for_non_registrable_seg_sec},
		{"freeze_topology",									0,
				&nvmeibt_topology_freeze_topo, NULL, NULL},
		{"is_incremental_wire_buf",							0,
				NULL, nvmeibt_raft_set_incremental_wire_buf_enabled, nvmeibt_raft_get_incremental_wire_buf_enabled},
		{"trace_compress",									0,
				NULL, nvmeibt_binary_tracing_set_trace_compress, nvmeibt_binary_tracing_get_trace_compress},
};

static int _debug_config_params_set(struct oper_param_t *param, const char *valstr)
{
	int rc = 0;
	uint64_t val = 0;

	if (strncmp(valstr, "default", 7)==0) {
		rc = 1;
		val = param->default_value;
	} else if (sscanf(valstr, "%" PRIu64, &val) > 0) {
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
		const int len = strnlen(param->name, 128);		// All parameters have relatively short name
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
	nvmeibt_Str_sprintf(s, "%s=%x\n", TOMA_PARAMS_ENCODING_STRING, TOMA_ENCODING_VER);
	for (i=0; i<ARRAY_SIZE(oper_params); i++) {
		const struct oper_param_t *param = &oper_params[i];
		if (print_values) {
			const int64_t val = (param->getf ? param->getf() : (param->ptr ? *param->ptr : 0));
			if (print_defaults || (val != param->default_value))
				nvmeibt_Str_sprintf(s, "+ param %s %" PRIu64 "\n", param->name, val);
		} else {
				nvmeibt_Str_sprintf(s, "    %s\n", param->name);
		}
	}
}

void dump_traces_list_to_file(void)
{
	struct _tracer *t;
	FILE *f = 0;
	const char *dirname = nvmeibt_toma_get_log_dir_name();
	const size_t tracelist_len = strnlen(dirname, PATH_MAX) + (sizeof(TRACE_LIST_FILENAME) - 1) + 2;
	char *tracelist = NNVMEIBT_TOMA_MALLOC(hu821mv, tracelist_len);
	__MEASURE_TOOK_INIT();
	nvmeibt_strlcpy(tracelist, dirname, tracelist_len);
	nvmeibt_strlcat(tracelist, TRACE_LIST_FILENAME, tracelist_len);
	f = fopen(tracelist, "w");
	__MEASURE_TOOK(N_IMf(warn_prepare_all_traces_measure_0, "fopen() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
	NNVMEIBT_TOMA_FREE(trace_1_debug_prepare_all_traces, tracelist);
	if (!f)
		return;
	FOR_EACH_TRACER_IN_ALL_SECTIONS(t) {
		const char *eol = my_strchrnul(t->format_str, '\n');
		fprintf(f, "%s, %s, %d, %s, %.*s\n", t->filename, t->function, t->lineno, t->lvl, (int)(eol - t->format_str), t->format_str);
	}
	fclose(f);
	__MEASURE_TOOK(N_IMf(warn_prepare_all_traces_measure_1, "fclose() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));
}

void print_stack(void) {
	#define STACK_SIZE 100
	int j, nptrs;
	void *buffer[STACK_SIZE];
	char **strings;
	nptrs = backtrace(buffer, STACK_SIZE);
	strings = backtrace_symbols(buffer, nptrs);
	for (j = 0; j < nptrs; j++) {
		syslog(LOG_ERR," %s\n", strrchr(strings[j], '/'));
		//N_Wf(teps01, " @STRRCHR", strrchr(strings[j], '/'));	// Dont trust this as bin traces may not work, so can stuck in infinite loop
	}
}
