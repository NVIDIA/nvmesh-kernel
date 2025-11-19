#ifndef TRACE_DAEMON_DBG_H
#define TRACE_DAEMON_DBG_H

#include <stdio.h>
#include <time.h>
#include <string.h>
#include <assert.h>

#ifndef __DEBUG_LEVEL_SRC
#error "__DEBUG_LEVEL_SRC must be defined"
#endif

#define _trace(LVL, HDR, fmt, ...)                                                                                     \
	{                                                                                                                  \
		struct timespec ts;                                                                                     \
		struct tm *timeinfo;                                                                                    \
		char timebuf[100];                                                                                      \
		clock_gettime(CLOCK_REALTIME, &ts);                                                                     \
		timeinfo = localtime(&ts.tv_sec);                                                                       \
		strftime(timebuf, sizeof(timebuf), "%F %T", timeinfo);                                                  \
		fprintf(stderr, "%s.%06ld " LVL " (" HDR "): " fmt "\n", timebuf, ts.tv_nsec / 1000, ##__VA_ARGS__);    \
	}

#ifdef __MODULE_HDR_ARGS
#define _error(fmt, ...)  if (__DEBUG_LEVEL_SRC>=1) _trace("Error", __MODULE_HDR, fmt " (errno=%m)", __MODULE_HDR_ARGS, ##__VA_ARGS__)
#define _info(fmt, ...)   if (__DEBUG_LEVEL_SRC>=2) _trace("Info ", __MODULE_HDR, fmt, __MODULE_HDR_ARGS, ##__VA_ARGS__)
#define _debug(fmt, ...)  if (__DEBUG_LEVEL_SRC>=3) _trace("Debug ", __MODULE_HDR, fmt, __MODULE_HDR_ARGS, ##__VA_ARGS__)
#else
#define _error(fmt, ...)  if (__DEBUG_LEVEL_SRC>=1) _trace("Error", __MODULE_HDR, fmt " (errno=%m)", ##__VA_ARGS__)
#define _info(fmt, ...)   if (__DEBUG_LEVEL_SRC>=2) _trace("Info ", __MODULE_HDR, fmt, ##__VA_ARGS__)
#define _debug(fmt, ...)  if (__DEBUG_LEVEL_SRC>=3) _trace("Debug ", __MODULE_HDR, fmt, ##__VA_ARGS__)
#endif

#ifndef __MODULE_PRE_SUICIDE
#define __MODULE_PRE_SUICIDE
#endif

/*Trace daemon is running under systemctl. In case of REALLY critical error it is often wiser to just kill the process
  and let systemctl to start as again after a short delay. Don't abuse.*/
#define _suicide(reason, ...)                                                                                          \
	{                                                                                                                  \
		_error("Goodbye cruel world, " reason, ##__VA_ARGS__);                                                         \
		fflush(stderr);                                                                                                \
        __MODULE_PRE_SUICIDE;                                                                                          \
		assert(0);                                                                                                     \
	}

#define _suicide_on(condition, reason, ...) if (!!(condition)) {_suicide(reason, ##__VA_ARGS__)}
#define _error_on(condition, reason, ...) if (!!(condition)) {_error(reason, ##__VA_ARGS__)}

#endif /*TRACE_DAEMON_DBG_H*/
