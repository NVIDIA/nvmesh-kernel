#include "nvmeib_pipe_tracer_defs.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

/* I reuse trace daemon debuggin infra here, it is handy */
static int nvmeib_pipe_tracer_internal_dbg_lvl = 2;
#define __DEBUG_LEVEL_SRC nvmeib_pipe_tracer_internal_dbg_lvl
#define __MODULE_HDR "pipe_tracer/%s"
#define __MODULE_HDR_ARGS __func__
#define __MODULE_PRE_SUICIDE                                                   \
	if (nvmeib_pipe_trace_long)                                                \
	nvmeib_flush_and_terminate(nvmeib_pipe_trace_long)
#include "nvmeib_trace_userspace_poller.h"
#include "trace_daemon_2.0/trace_daemon_dbg.h"

enum dbg_lvl {
	DBG_LVL_ERROR = 1,
	DBG_LVL_WARN,
	DBG_LVL_INFO,
	DBG_LVL_TRACE,
	DBG_LVL_DEBUG,
};

int tracer_nvmeib_pipe_debug_level = 6; /* 6 - max */

struct text_processor_data {
	struct text_processor_input {
		char *line;
		size_t len;
	} in;
	struct text_processor_output {
		char *line;
		size_t len;
		enum dbg_lvl lvl;
		unsigned long long ts_ns;
	} out;
};

enum text_processor_rv { TPRV_OK, TPRV_SKIP, TPRV_ERROR };
/**
 * Process input line, fill in the output structure.
 */
typedef enum text_processor_rv (*text_processor_fn)(
    struct text_processor_data *);

struct text_processor {
	text_processor_fn process_line;
	const char *data_source;
};

/**
 * Get the system boot timestamp in nanoseconds since epoch
 * @return Timestamp or -1 and errno set
 */
static unsigned long long __get_btime() {
	char *line = NULL;
	size_t len = 0;
	ssize_t nread;
	FILE *proc_stat = fopen("/proc/stat", "r");
	unsigned long long rv = -1;
	if (!proc_stat) { return -1; }
	while ((nread = getline(&line, &len, proc_stat)) != -1) {
		if (!strncmp("btime ", line, sizeof("btime ") - 1)) {
			rv = ((unsigned long long)atoll(line + sizeof("btime ") - 1)) *
			     1000000000ULL;
			break;
		}
	}
	fclose(proc_stat);
	free(line);
	return rv;
}

/**
 * Text processor used to read data from ftrace trace_pipe
 */
enum text_processor_rv __ftrace_tp(struct text_processor_data *d) {
	static unsigned long long btime = -1;
	if (btime == -1) {
		btime = __get_btime();
		if (btime == -1) return TPRV_ERROR;
	}
	{
		unsigned long long sec, usec;
		ssize_t buf_pos;
		if (sscanf(d->in.line, "%*s%*s%*s%llu.%llu%ln", &sec, &usec,
		           &buf_pos) == 2) {
			d->out.line = d->in.line + buf_pos;
			d->out.len = strlen(d->out.line) - 1;
			d->out.line[d->out.len] = '\0';
			d->out.lvl = DBG_LVL_INFO;
			d->out.ts_ns = btime + sec * 1000000000ULL + usec * 1000ULL;
			return TPRV_OK;
		} else {
			if (strncmp(d->in.line, "CPU", sizeof("CPU"))) {
				/* Being here means ftrace complains on lost traces */
				d->out.line = d->in.line;
				d->out.len = strlen(d->out.line) - 1;
				d->out.line[d->out.len] = '\0';
				d->out.lvl = DBG_LVL_INFO;
				/* keep the time stamp unchanged */
				return TPRV_OK;
			}
			else {
				_debug("malformed line %s", d->in.line);
				return TPRV_SKIP;
			}
		}
	}
}

struct trace_channel *nvmeib_pipe_trace_long;
pthread_t __long_ch_thread;
char __work_dir[255] = ".";
bool __terminate = false;
bool __restart = false;

/**
 * Generic poller thread main loop
 */
static void *__trace_poller_thread(void *param) {
	int rv = nvmeib_trace_poll_to_logrotated_file_loop(
	    (struct nvmeib_trace_channel_descriptor *)param);
	if (rv < 0) _error("trace poller activation failed rv=%d", rv);
	free(param);
	return NULL;
}

/**
 * Starts all pollers
 */
static void __start_pollers() {
	int rv;
	struct nvmeib_trace_channel_descriptor *long_descr;
	nvmeib_pipe_trace_long = nvmeib_init_trace_channel(4096, 32, 0, 0);
	if (!nvmeib_pipe_trace_long)
		_suicide("allocating nvmeib_pipe_trace_long") long_descr =
		    nvmeib_init_trace_channel_descriptor(
		        nvmeib_pipe_trace_long, "nvmeib_pipe_trace_long", __work_dir,
		        100 /*Total history size in MB*/,
		        10 /*Max single log file size in MB*/, .resume_old = 1,
		        .place_markers = 0);
	if (!long_descr) _suicide("allocating long_descr");
	if ((rv = pthread_create(&__long_ch_thread, NULL, __trace_poller_thread,
	                         long_descr) != 0)) {
		_suicide("starting poller thread");
	}
}

/**
 * Cleanly terminate pollers
 */
static void __join_pollers() {
	int rv;
	nvmeib_flush_and_terminate(nvmeib_pipe_trace_long);
	if ((rv = pthread_join(__long_ch_thread, NULL)) != 0) {
		_suicide("terminating poller long rv=%d", rv);
	}
	nvmeib_destroy_trace_channel(nvmeib_pipe_trace_long);
	nvmeib_pipe_trace_long = NULL;
}

/**
 * Used to send processed output to longerm channel
 */
static void __trace_to_longterm(struct text_processor_output *d) {
	nvmeib_trace_timestamp_ns = d->ts_ns;
	// printf("%d [%llu] %s\n", d->lvl, d->ts_ns, d->line);
	// return;
	switch (d->lvl) {
		case DBG_LVL_ERROR:
			NVMEIB_LOG_LONGTERM("@STR", _E, /*def*/, pipe_str_err, d->line);
			break;
		case DBG_LVL_WARN:
			NVMEIB_LOG_LONGTERM("@STR", _E, /*def*/, pipe_str_warn, d->line);
			break;
		case DBG_LVL_INFO:
			NVMEIB_LOG_LONGTERM("@STR", _E, /*def*/, pipe_str_info, d->line);
			break;
		case DBG_LVL_TRACE:
			NVMEIB_LOG_LONGTERM("@STR", _E, /*def*/, pipe_str_trace, d->line);
			break;
		case DBG_LVL_DEBUG:
			NVMEIB_LOG_LONGTERM("@STR", _E, /*def*/, pipe_str_debug, d->line);
			break;
	}
}

/**
 * Main flow function, self contained, can be rerun multiple times
 */
static void __main_flow(struct text_processor *tp) {
	ssize_t nread;
	struct text_processor_data tpd = {{0}};
	FILE *input_stream = fopen(tp->data_source, "r");

	if (!input_stream) _suicide("opening input stream %s", tp->data_source);

	_info("starting pollers");
	__start_pollers();
	while (!__restart && !__terminate &&
	       (nread = getline(&tpd.in.line, &tpd.in.len, input_stream)) != -1) {
		enum text_processor_rv rv = tp->process_line(&tpd);
		if (rv == TPRV_OK)
			__trace_to_longterm(&tpd.out); /* Trace */
		else if (rv == TPRV_ERROR)
			break; /* Terminate */
		           /* Else skip */
	}
	if (errno) _error("main loop terminated with an error");

	if (__terminate) {
		_info("terminated");
	} else if (__restart) {
		_info("restarted");
	} else {
		__terminate = true;
	}

	free(tpd.in.line);
	fclose(input_stream);

	_info("joining pollers");
	__join_pollers();
	_info("terminated");
}

struct text_processor siw_tp = {.process_line = __ftrace_tp,
                                .data_source =
                                    "/sys/kernel/debug/tracing/trace_pipe"};

int main() {
	/*@TODO: Add some configuration read, */
	while (!__terminate) {
		__main_flow(&siw_tp);
		__restart = false;
	}

	return 0;
}