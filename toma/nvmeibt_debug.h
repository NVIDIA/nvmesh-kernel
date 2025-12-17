#ifndef NVMEIBT_DEBUG_H
#define NVMEIBT_DEBUG_H
/* Note: This is the most basic and first include in all Toma files, It may
   depend only on external compilation flags, not on other files */
#include <assert.h>
#include <sys/time.h>
#include <string.h>
#include <stdio.h>

// API VS local file system
#define TOMA_ROOT_DIR "/"			// Toma is sudo, using root directory
#define TOMA_DIR_RUN_NVMESH TOMA_ROOT_DIR "var/run/nvmesh"	// Runtime vars directory
#define TOMA_DIR_OPT_NVMESH TOMA_ROOT_DIR "var/opt/nvmesh"	// Config_persistency directory
#define TOMA_LOG_DIR        TOMA_ROOT_DIR "var/log/nvmesh"	// Logs directory
#define TOMA_BINLOG_DIR     TOMA_LOG_DIR  "/trace_daemon"

#define TOMA_SW_COMPATIBILITY_VER					0x00000310
#define WARN(x...) ({})		// Just in order to compile

#include "common/nvmeib_str.h"
#define MACRO_DEF_TO_STR(s) __stringify(s)

/*
 * The behavior of logging and trace-logging depends on the compilation mode:
 *
 * Compilation         "stdlog" destination           trace.config
 * -----------   --------------------------------     ------------
 *  "release"              system syslog                enabled
 *  "delease"    TOMA_LOG_DIR/toma_0.{log,err}   disabled
 *   "debug"     TOMA_LOG_DIR/toma_0.{log,err}   disabled
 *
 * Logging macros (info, error, warn, debug, info-major, trace):
 * ------------------------------------------------------------
 *
 * _If(), _Ef(), _Wf(), _Df() are sent to "stdlog"
 * _IMf() is sent to syslog by standard and alos to stdout
 * _Tf() is first filtered per trace.config and sent to "stdlog"
 * FIN, FOUT, ... are translated to _Df()
 *
 * **** Use of _Tf() is highly encouraged *****
 *
 * Trace log and filtering:
 * -----------------------
 *
 * By default everything is filtered out, nothing is logged. When TOMA starts
 * it generates "tracelist.txt" in its LOG directory. The file shows all the
 * available TRACEABLE LOG entries.
 *
 * Filtering of traceable log entries is controled by "toma_trace.config" (in the
 * same directory). Normally that file does not exist. The file holds entries
 * to add or remove traces. It is read line by line, and latter instructions
 * override earlier ones.
 *
 * Trace log filtering syntax:
 *   [+-] all                           : all/remove possible traces
 *   [+-] function <FUNCTIONNAME>       : add/remove traces in a function
 *   [+-] filename <FILENAME>           : add/remove traces in a file
 *   [+-] line <FILENAME> <LINENUMBER>  : add/remove traces in a line
 * The "+" adds traces that meet the criteria, and the "-" remove such traces.
 *
 * Other available entires:
 *   [+-] record                        : turn TOMA recording on/off
 */

// default logging settings

#define is_block_device_stat(s)  S_ISBLK((s).st_mode)		// const struct stat s

// convenience log levels. Daniel: Todo: Delete most of the code below and use
// this file: #include "common/nvmeib_utils_bin_traces.h"
#ifdef TOMA_SIMULATOR_SANDBOX
	#include "unitest/toma_in_sandbox.h"
#endif
#include <syslog.h>
#include "interfaces/log/log_incs.h"
#include "../common/nvmeib_macro_utils.h"

	// config params defaults
#define RAFT_LEADER_HEARTBEAT_TIMEOUT_NSEC_DEFAULT				MSEC_TO_NSEC(200)
#define RAFT_MIN_ELECTION_TIMEOUT_FACTOR_DEFAULT				3
#define MAX_N_SIMULTANEOUS_DIRTY_REBUILD_DEFAULT				2
#define MAX_N_SIMULTANEOUS_STALE_AND_TXID_REBUILD_DEFAULT		2
#define MAX_N_SIMULTANEOUS_SCRUBBING_DEFAULT					2
#define SCRUBBING_DEFAULT_PERIOD_DAYS_DEFAULT					30
#define SCRUBBING_N_BLKSETS_PER_ITERATION_DEFAULT				16384
#define SCRUBBING_DEFAULT										0
#define TOPOLOGY_MAX_PRAIDS_IN_A_REPORT_DEFAULT					32
#define TOMA_DO_NOT_REPORT_DISK_SEGMENT_GPTS_DEFAULT			1
#define MGMT_KEEP_ALIVE_SECS_DEFAULT							5
#define MGMT_LEADER_KEEP_ALIVE_SECS_DEFAULT						5
#define REPORT_TARGET_MIN_BETWEEN_SECS_DEFAULT					2
#define GENERIC_BLOCK_DEVICE_SUPPORT_DEFAULT					0
#define PRAID_ACTIVATION_TIMEOUT_SEC_DEFAULT					20
#define MAX_HEADER_LENGTH_DEFAULT								128
#define DISABLE_PERIODIC_SMART_POLLING_DEFAULT					0
#define TRACER_DEBUG_LEVEL_DEFAULT								4
#define LOG_SNAPSHOTTING_MODE_START_TIME_SEC_DEFAULT			0
#define ENABLE_NETWORKING_PERIODIC_TRACES_DEFAULT				0
#define KAFKA_GET_OFFSET_TIMEOUT_SECS_DEFAULT					40
#define ATTACH_TIMEOUT_SEC_DEFAULT								5

#define PRAID_LEADER_MAX_NSEC_WAIT_FOR_NON_REGISTRABLE_SEG_TO_APPLY_DEFAULT		SEC_TO_NSEC(5) // Usually TOOK(long time) for write (commit) to system disk

#define __MEASURE_TOOK_INIT()									\
	struct timespec	__measure_took_t1, __measure_took_t2;		\
	getnstimeofday(&__measure_took_t1);

int64_t nvmeibt_raft_get_effective_heartbeat_timeout_ns(void);

#define __MEASURE_TOOK(__measure_took_prt_cmd) do {													\
		int64_t __measure_took_time_took_nsec;														\
		getnstimeofday(&__measure_took_t2);															\
		__measure_took_time_took_nsec = timespec_diff_ns(__measure_took_t2, __measure_took_t1);		\
		if (__measure_took_time_took_nsec  * 2 > nvmeibt_raft_get_effective_heartbeat_timeout_ns()) {	\
			__measure_took_prt_cmd;																	\
		}																							\
		__measure_took_t1 = __measure_took_t2;														\
	} while (0)

// log msg prefixesd

#define VERY_MIN_TIME_BETWEEN_SYSLOG_NS MSEC_TO_NSEC(50)
#define OK_AVG_TIME_BETWEEN_SYSLOG_NS MSEC_TO_NSEC(250)
#define CLIP_MAX_TIME_BETWEEN_SYSLOG_NS (OK_AVG_TIME_BETWEEN_SYSLOG_NS * 5)
#define THROTTLE_IIR_SIZE 10
#include "../common/nvmeib_iir.h"
#define NVMEIBT_THROTTLED_SYSLOG(SYSLOG_LOG_LVL, __FMT, ...)	({														\
	static struct nvmeib_iir	avg_ns_between_writes_IIR;															\
	static int64_t				prev_write_time_ns;																	\
	static int					_n_throttled;																		\
	struct nvmeib_iir			saved_IIR;																			\
	int64_t						time_since_prev_ns;																	\
	struct timespec				_now_;																				\
	int64_t						now_ns;																				\
	getnstimeofday(&_now_);																							\
	now_ns = timespec_to_nsec(_now_);								        									  	\
	time_since_prev_ns = now_ns - prev_write_time_ns;						       									\
	if (time_since_prev_ns > VERY_MIN_TIME_BETWEEN_SYSLOG_NS) {														\
		if (avg_ns_between_writes_IIR.new_sample_weight == 0.0) {				      								\
			nvmeib_iir_set_new_sample_weight(&avg_ns_between_writes_IIR, (1.0 / THROTTLE_IIR_SIZE));			   	\
		}											      							      							\
		saved_IIR = avg_ns_between_writes_IIR;																		\
		nvmeib_iir_add_sample(&avg_ns_between_writes_IIR, min(CLIP_MAX_TIME_BETWEEN_SYSLOG_NS, time_since_prev_ns));\
		if (nvmeib_iir_get_val(avg_ns_between_writes_IIR) < OK_AVG_TIME_BETWEEN_SYSLOG_NS) {						\
			avg_ns_between_writes_IIR = saved_IIR;																	\
			_n_throttled++;																							\
		} else {																									\
			prev_write_time_ns = now_ns;																			\
			if (_n_throttled) {																						\
				syslog(LOG_DEBUG, "n_throttled=%d", _n_throttled);													\
			}																										\
			syslog(SYSLOG_LOG_LVL, __FMT,  ## __VA_ARGS__);   	     												\
			_n_throttled = 0;																						\
		}												        													\
	} else {								        							      					  			\
		_n_throttled++;																								\
	}												        														\
})

#define get_my_tid() (unsigned long)pthread_self()	//syscall(__NR_gettid)
int trace_to_printf_fmt(char* printf_fmt, int printf_fmt_len, const char* trace_fmt, const char *filename, int line, const char *func_name);
#define SEND_TO_SYSLOG(_syslog_lvl, auto_generated_printf_fmt, ...) ({							\
	const int __errno_save = errno;																	\
	static char printf_fmt[2000];																\
	if (!printf_fmt[0]) {																		\
		trace_to_printf_fmt(printf_fmt, sizeof(printf_fmt), auto_generated_printf_fmt, kbasename(__FILE__), __LINE__, __FUNCTION__); \
	}																							\
	NVMEIBT_THROTTLED_SYSLOG(_syslog_lvl, printf_fmt, ## __VA_ARGS__);							\
	errno = __errno_save;																		\
})

#define LOG_TO_TRACE(LVL, name, ch, toma_lvl_str, fmt, ...) ({																		\
		DEFINE_TRACE_METADATA(toma_trace_metadata, LVL, fmt);																		\
		if (unlikely(toma_trace_metadata.is_on))																					\
			ch("@TID_INT_NOFMT " toma_lvl_str fmt, NVMEIB_CONCAT2(_,LVL), /*Default scope*/, name, get_my_tid(), ##__VA_ARGS__);	\
	})
#define _NLOGLEVEL_NO_PREFIX(LVL, name, fmt, ...) NVMEIB_LOG_LONGTERM(fmt, NVMEIB_CONCAT2(_,LVL) & no_prefix, /*Default scope*/, name, ##__VA_ARGS__)
#define _NMIRROR_LOGLEVEL(LVL, _syslog_lvl, name, ch, toma_lvl_str, fmt, ...) ({						\
	LOG_TO_TRACE(LVL, name, ch, toma_lvl_str, fmt, ## __VA_ARGS__);										\
	SEND_TO_SYSLOG(_syslog_lvl, ___trace_fmt_ ## name , get_my_tid(), ## __VA_ARGS__);		\
})

#define TOMA_ERR_STR        "*TOMAerr* "
#define TOMA_WARN_STR       "*TOMAwarn* "
#define TOMA_INFO_MAJOR_STR "*TOMAinfo* "

#define N_Df(name, fmt, ...)  LOG_TO_TRACE(Df, name, NVMEIB_LOG_LONGTERM, "", fmt, ## __VA_ARGS__)
#define N_Tf(name, fmt, ...)  LOG_TO_TRACE(Tf, name, NVMEIB_LOG_LONGTERM, "", fmt, ## __VA_ARGS__)
#define N_If(name, fmt, ...)  LOG_TO_TRACE(If, name, NVMEIB_LOG_ETERNAL, "", fmt, ## __VA_ARGS__)
#define N_IMf(name, fmt, ...) _NMIRROR_LOGLEVEL(IMf, LOG_NOTICE, name, NVMEIB_LOG_ETERNAL, TOMA_INFO_MAJOR_STR, fmt, ## __VA_ARGS__)
#define N_Wf(name, fmt, ...)  _NMIRROR_LOGLEVEL(Wf, LOG_WARNING, name, NVMEIB_LOG_ETERNAL, TOMA_WARN_STR, fmt, ## __VA_ARGS__)
#define N_Ef(name, fmt, ...)  _NMIRROR_LOGLEVEL(Ef, LOG_ERR, name, NVMEIB_LOG_ETERNAL, TOMA_ERR_STR, fmt, ## __VA_ARGS__)

// (Used to be) throttled-warning (once per minute)
#define N_WTf(name, fmt, ...) N_Wf(name, fmt, ## __VA_ARGS__)
#define N_ETf(name, fmt, ...) N_Ef(name, fmt, ## __VA_ARGS__)

#define _DILUTED_CMD(_msec, _diluted_cmd_...) ({				\
	struct timespec				now;							\
	static struct timespec		prev;							\
	getnstimeofday(&now);										\
	if (timespec_diff_ns(now, prev) >= MSEC_TO_NSEC(_msec)) {	\
		_diluted_cmd_;											\
		prev = now;												\
	}															\
})

// assert and abort

#ifdef TOMA_DEBUG
	#define TOMA_ABORT_IF_DEBUG(__nvmeibt_error_severity_es) nvmeibt_abort(__nvmeibt_error_severity_es);
#else
	#define TOMA_ABORT_IF_DEBUG(__nvmeibt_error_severity_es)
#endif

#define NTOMA_ASSERT(name, cond, fmt, ...) do {     	\
		if (!(cond)) {                                 	\
			N_ETf(name, fmt, ## __VA_ARGS__);			\
			TOMA_ABORT_IF_DEBUG(ES_FATAL);          	\
		}                                              	\
	} while (0)

void print_stack(void);

struct _tracer {
	const char *function;
	const char *filename;
	const char *format_str;
	unsigned int lineno:24;
	const char	*lvl;
	uint8_t		plus_minus_flag;
	uint8_t		is_on;
} __attribute__((aligned(8)));

#define TRACE_METADATA_is_on_DEFAULT 1
#define DEFINE_TRACE_METADATA(metadata, LVL, fmt)			\
	static struct _tracer __attribute((aligned(8)))			\
		__attribute__((section("__tracer"))) metadata = {	\
		.function = __func__,								\
		.filename = __FILE__,								\
		.format_str = (fmt),								\
		.lineno = __LINE__,									\
		.lvl = MACRO_DEF_TO_STR(LVL),						\
		.plus_minus_flag = 0,								\
		.is_on = TRACE_METADATA_is_on_DEFAULT				\
	}

#define NVMEIBT_LONG_TRACE_WRAPPER(__name__, __info__, __str_in__, __strlen_in__)	({				\
	int	MAX_PRINT_SIZE = (TRACE_BUFFER_SIZE - 128);													\
	char	*__p = (char *)__str_in__;																\
	char	*__end_of_in__ = __p + min((size_t)__strlen_in__, (size_t)64000);						\
	char	*end_of_syslog;																			\
	char	__memorized_end_char, __memorized_syslog_end_char;										\
	int		__round_no__ = -1;																		\
	char	*__cur_end;																				\
	while (__p < __end_of_in__) {																	\
		__round_no__++;																				\
		__cur_end = __end_of_in__;																	\
		if (__cur_end - __p > MAX_PRINT_SIZE) {														\
			char *__in_eol__ = memrchr(__p, '\n', MAX_PRINT_SIZE);									\
			if (__in_eol__) {																		\
				__cur_end = __in_eol__;																\
			} else {																				\
				__cur_end = __p + MAX_PRINT_SIZE;													\
			}																						\
		}																							\
		__memorized_end_char = *__cur_end;															\
		*__cur_end = '\0';																			\
		if (__round_no__ == 0) {																	\
			LOG_TO_TRACE(IMf, __name__ ## _1, NVMEIB_LOG_ETERNAL, TOMA_INFO_MAJOR_STR, " " __info__ " @STR", __p);		\
			end_of_syslog = min(__p + 512, __end_of_in__);											\
			__memorized_syslog_end_char = *end_of_syslog;											\
			*end_of_syslog = '\0';																	\
			SEND_TO_SYSLOG(LOG_NOTICE, "%s ", __p);													\
			/* NVMEIBT_THROTTLED_SYSLOG(LOG_NOTICE, "(%lu) " __str_in__, get_my_tid());	*/							\
			*end_of_syslog = __memorized_syslog_end_char;                                           \
		} else {																					\
			_NLOGLEVEL_NO_PREFIX(Tf, __name__ ## _2, "@STR", __p);									\
			/*NVMEIBT_THROTTLED_SYSLOG(LOG_INFO, "%s\n", __p);*/									\
		}																							\
		*__cur_end = __memorized_end_char;															\
		__p = __cur_end + (*__cur_end == '\n' || *__cur_end == '\0' ? 1 : 0);						\
	}																								\
	if ((ssize_t)(__p - (__str_in__)) < (ssize_t)(__strlen_in__)) {									\
		N_IMf(__name__ ## _3, "String was too long @SIZE_T", (__strlen_in__));						\
	}																								\
})

/* Auto binary trace ID, resolved to filename_line, requires __FILE_LITERAL__ infra */
#ifndef __FILE_LITERAL__
#pragma GCC error "__FILE_LITERAL__ not defined, Makefile error, should not happen"
#endif
#define __AUTOID__ NVMEIB_CONCAT2(__FILE_LITERAL__, NVMEIB_CONCAT2(_, __LINE__))

#ifndef NFIN
	#define NFIN  N_Tf(__AUTOID__, "-->")
	#define NFOUT N_Tf(__AUTOID__, "<--")
#endif

#define NFOUT_rv N_Tf(__AUTOID__, "<--(rv=@RV)", rv);

void prepare_all_traces(void);
void log_snapshotting_set_active_log_levels(char const *level);
void read_rpc_config_from_persist(bool is_initial_read);
int update_traces_turn_all_on_or_off(char plus_or_minus, bool is_forced);
void update_traces(void);
int nvmeibt_debug_config_params_set(const char *param, const char *valstr);
struct nvmeibt_Str;
void nvmeibt_debug_config_params_print(struct nvmeibt_Str *s, bool print_values, bool print_defaults);

// Tracer section management functions (for .so file support)
int nvmeibt_debug_register_tracer_section(struct _tracer *start, struct _tracer *end);
void nvmeibt_debug_init_tracer_sections(void);

#endif // NVMEIBT_DEBUG_H
