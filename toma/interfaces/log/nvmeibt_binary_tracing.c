/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "nvmeibt_debug.h"
#include "nvmeibt_binary_tracing.h"
#include "nvmeibt_toma.h"
#include <signal.h>
#include "linux/limits.h"

// Forward-declare from trace_compress_lib/compressor.h
// (full include avoided: libgen.h redefines basename as a macro conflicting with struct field names)
struct compressor;
extern struct compressor *compressor_create_lz4(size_t max_in_size);

struct trace_channel *nvmeibt_trace_long;
struct trace_channel *nvmeibt_trace_eph;
struct trace_channel *nvmeibt_trace_eter;		// Toma Error / Warning, Possible info

pthread_t long_poller, eph_poller, eter_poller;


int64_t tracer_nvmeibt_requested_debug_level = TRACER_DEBUG_LEVEL_DEFAULT;
int tracer_nvmeibt_debug_level; 	// For the logging level see "tools/pre_processor/gen_probes2.py"
static bool is_logging_on = 1;
static bool is_trace_compress_enabled = 0;	// LZ4 compression of binlog files (kill switch: toggle via RPC)

#include <unistd.h>
#include <sys/syscall.h>
#include "nvmeibt_common.h"

static void* trace_poller_thread(void *param) {
	sigset_t mask;
	int rv;
	extern char tracing_cgroup[NAME_MAX];

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGUSR2);
	sigaddset(&mask, SIGHUP);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	if (strcmp(tracing_cgroup, "") != 0) {
		int fd = -1;
		char tid[10];
		char cgroup_path[PATH_MAX];
		struct nvmeibt_Buf buf;

		snprintf(cgroup_path, PATH_MAX, "/sys/fs/cgroup/blkio/%s/tasks", tracing_cgroup);
		buf.data_buf = (void *)tid;
		snprintf((char *)(buf.data_buf), 20, "%ld", syscall(SYS_gettid));
		buf.buf_len = strlen((char *)(buf.data_buf));

		fd = NNVMEIBT_OPEN(fdfsdy, cgroup_path, O_WRONLY, 0666);
		if (fd < 0) {
			N_Ef(gfdggghh, "Error while opening the file @STR for writing @AUTO_ERRNO", cgroup_path);
			goto start;
		}
		if (NNVMEIBT_BUF_FWRITE(gdfgdf, &buf, fd) < 0) {
			N_Ef(fdkfkd, "Error while writing the file @STR for writing @AUTO_ERRNO", cgroup_path);
		}
		NNVMEIBT_CLOSE(fsdfds, fd);
	}

start:
	rv = nvmeib_trace_poll_to_logrotated_file_loop((struct nvmeib_trace_channel_descriptor *)param);
	if (rv < 0)
		fprintf(stderr, "TOMA's trace poller activation failed rv=%d", rv);
	nvmeib_trace_channel_descriptor_cleanup((struct nvmeib_trace_channel_descriptor *)param);
	long_poller = 0;
	free(param);
	return NULL;
}

static int __start_trace_pollers(pthread_t *poller_long, pthread_t *poller_eph, pthread_t *poller_eter, bool is_running_as_a_utility) {
	int rv;
	const unsigned int file_s = nvmeibt_toma_get_bin_log_file_size();			/* Max single log file size[MB] */
	const unsigned int total_size = file_s * nvmeibt_toma_get_bin_log_file_n();	/* Total history size[MB] */

	struct nvmeib_trace_channel_descriptor *descriptor_long, *descriptor_eph, *descriptor_eter;
	nvmeibt_trace_eph =  nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 1);
	nvmeibt_trace_long = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 0);
	nvmeibt_trace_eter = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 0);
	#define init_channel(which, f_name) nvmeib_init_trace_channel_descriptor(which, f_name, TOMA_BINLOG_DIR, total_size, file_s, \
		.resume_old = 1, .place_markers = 1, \
		.compressor = compressor_create_lz4(TRACE_BUFFER_SIZE), \
		.compress_enabled = &is_trace_compress_enabled)
	if (is_running_as_a_utility) {
		descriptor_long = init_channel(nvmeibt_trace_long, "toma_util.binlog");
		descriptor_eter = init_channel(nvmeibt_trace_eter, "toma_util.eter.binlog");
		descriptor_eph =  init_channel(nvmeibt_trace_eph,  "toma_util.eph.binlog");
	} else {
		descriptor_long = init_channel(nvmeibt_trace_long, "toma.binlog");
		descriptor_eter = init_channel(nvmeibt_trace_eter, "toma.eter.binlog");
		descriptor_eph =  init_channel(nvmeibt_trace_eph,  "toma.eph.binlog");
	}
	if (!descriptor_long->compressor || !descriptor_eter->compressor || !descriptor_eph->compressor)
		fprintf(stderr, "Warning: LZ4 compressor creation failed, trace compression will be unavailable\n");

	// Start pollers
	if ((rv = pthread_create(poller_long, NULL, trace_poller_thread, descriptor_long)) != 0) {
		fprintf(stderr, "TOMA's poller_long creation failed rv=%d", rv);
		abort();
	}
	pthread_setname_np(*poller_long, "poller_long");
	//
	if ((rv = pthread_create(poller_eter, NULL, trace_poller_thread, descriptor_eter)) != 0) {
		fprintf(stderr, "TOMA's poller_eter creation failed rv=%d", rv);
		abort();
	}
	pthread_setname_np(*poller_eter, "poller_eter");
	//
	if ((rv = pthread_create(poller_eph, NULL, trace_poller_thread, descriptor_eph)) != 0) {
		fprintf(stderr, "TOMA's poller_eph creation failed rv=%d", rv);
		abort();
	}
	pthread_setname_np(*poller_eph, "poller_eph");
	return 0;
}

static int __join_trace_pollers(pthread_t poller_long,  pthread_t poller_eph, pthread_t poller_eter) {

	int rv;

	nvmeib_flush_and_terminate(nvmeibt_trace_long);
	nvmeib_flush_and_terminate(nvmeibt_trace_eph);
	nvmeib_flush_and_terminate(nvmeibt_trace_eter);
	// For now only long is in use. Later add further initializations when needed.
	if ((rv = pthread_join(poller_long, NULL)) != 0) {
		fprintf(stderr, "TOMA's poller_long exit failed rv=%d", rv);
		goto out;
	}
	if ((rv = pthread_join(poller_eter, NULL)) != 0) {
		fprintf(stderr, "TOMA's poller_eter exit failed rv=%d", rv);
		goto out;
	}
	if ((rv = pthread_join(poller_eph, NULL)) != 0) {
		fprintf(stderr, "TOMA's poller_eph exit failed rv=%d", rv);
	}
out:
	return rv;
}

void nvmeibt_flush_all_traces(void) {
	nvmeib_flush(nvmeibt_trace_long);
	nvmeib_flush(nvmeibt_trace_eph);
	nvmeib_flush(nvmeibt_trace_eter);
}


void nvmeibt_flush_all_and_terminate(void) {
	if (nvmeibt_trace_long) nvmeib_flush_and_terminate(nvmeibt_trace_long);
	if (nvmeibt_trace_eph) nvmeib_flush_and_terminate(nvmeibt_trace_eph);
	if (nvmeibt_trace_eter) nvmeib_flush_and_terminate(nvmeibt_trace_eter);
}

void nvmeibt_toggle_logging(void) {
	/* If logging is on switch it off and viсe versa */
	/* The tracer_nvmeibt_debug_level can be 0 or a configured value */
	is_logging_on = !is_logging_on;
	tracer_nvmeibt_debug_level = is_logging_on ? (int)tracer_nvmeibt_requested_debug_level : 0;
}

void nvmeibt_join_all_trace_pollers(void) {
	#ifdef TOMA_SIMULATOR_SANDBOX
		{ extern void toma_unitest_notify_stop_traces(void); toma_unitest_notify_stop_traces(); }
	#endif
	if (long_poller == 0) {
		fprintf(stderr, "long_poller=0, (previously closed?)\n");
		return;
	}
	if (eph_poller == 0) {
		fprintf(stderr, "eph_poller=0, (previously closed?)\n");
		return;
	}
	if (eter_poller == 0) {
		fprintf(stderr, "eter_poller=0, (previously closed?)\n");
		return;
	}
	__join_trace_pollers(long_poller, eph_poller, eter_poller);
	eter_poller = long_poller = eph_poller = 0;
	// Destroy channels
	nvmeib_destroy_trace_channel(nvmeibt_trace_eph);  nvmeibt_trace_eph =  NULL;
	nvmeib_destroy_trace_channel(nvmeibt_trace_long); nvmeibt_trace_long = NULL;
	nvmeib_destroy_trace_channel(nvmeibt_trace_eter); nvmeibt_trace_eter = NULL;
}

void nvmeibt_start_all_trace_pollers(bool is_running_as_a_utility) {
	tracer_nvmeibt_debug_level = (int)tracer_nvmeibt_requested_debug_level;
	__start_trace_pollers(&long_poller, &eph_poller, &eter_poller, is_running_as_a_utility);
	#ifdef TOMA_SIMULATOR_SANDBOX
		{ extern void toma_unitest_env_start(bool, int); toma_unitest_env_start(is_running_as_a_utility, tracer_nvmeibt_debug_level); }
	#endif
}

unsigned long long nvmeibt_get_total_bytes(void) {
	return nvmeib_trace_get_total_bytes(nvmeibt_trace_long) + nvmeib_trace_get_total_bytes(nvmeibt_trace_eter) + nvmeib_trace_get_total_bytes(nvmeibt_trace_eph);
}

unsigned long long nvmeibt_get_used_bufs(void) {
	return nvmeib_trace_get_total_bufs_used(nvmeibt_trace_long) + nvmeib_trace_get_total_bufs_used(nvmeibt_trace_eter) + nvmeib_trace_get_total_bufs_used(nvmeibt_trace_eph);
}

void nvmeibt_binary_tracing_set_tracer_debug_level(int64_t tracer_debug_level)
{
	if (tracer_debug_level > 6) {
		N_Ef(u55vx1, "invalid tracer_debug_level=@LONG > 6 (ignoring)", tracer_debug_level);
	} else {
		N_IMf(u55vx2, "new tracer_debug_level=@LONG", tracer_debug_level);
		tracer_nvmeibt_requested_debug_level = tracer_debug_level;
		if (is_logging_on)
			tracer_nvmeibt_debug_level = (int)tracer_nvmeibt_requested_debug_level;
	}
}

void nvmeibt_binary_tracing_enforce_active_tracer_nvmeibt_debug_level(int64_t tracer_debug_level)
{
	if (tracer_debug_level == -1LL) {
		tracer_nvmeibt_debug_level = (int)tracer_nvmeibt_requested_debug_level;
	} else {
		tracer_nvmeibt_debug_level = (int)tracer_debug_level;
	}
}

int64_t nvmeibt_binary_tracing_get_tracer_debug_level(void)
{
	return tracer_nvmeibt_requested_debug_level;
}

void nvmeibt_binary_tracing_set_trace_compress(int64_t is_enabled)
{
	bool new_val = is_enabled;
	if (is_trace_compress_enabled != new_val) {
		N_IMf(u66tc1, "is_trace_compress_enabled: @BOOL-->@BOOL (takes effect on next log rotation)",
			  is_trace_compress_enabled, new_val);
		is_trace_compress_enabled = new_val;
	}
}

int64_t nvmeibt_binary_tracing_get_trace_compress(void)
{
	return is_trace_compress_enabled;
}
