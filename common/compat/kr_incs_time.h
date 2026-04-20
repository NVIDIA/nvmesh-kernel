/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_TIME_H
#define KR_INCS_TIME_H
#ifdef __KERNEL__
	#if !(KS_HAS_DO_GETTIMEOFDAY)
		#if KS_NO_TIMESPEC_STRUCT_FOR_KERNEL
		struct timeval {
				__kernel_long_t tv_sec;
				__kernel_suseconds_t tv_usec;
		};
		#endif
		static inline void do_gettimeofday(struct timeval *tv)
		{
			struct timespec64 now;
			ktime_get_real_ts64(&now);
			tv->tv_sec = now.tv_sec;
			tv->tv_usec = now.tv_nsec/1000;
		}
	#endif

	#if KS_NO_TIMESPEC_STRUCT_FOR_KERNEL	// Implemented only in newer kernels
		struct timespec {
			__kernel_long_t	tv_sec;		/* seconds */
			long			tv_nsec;	/* nanoseconds */
		};

		static inline struct timespec timespec64_to_timespec(const struct timespec64 ts64)
		{
				struct timespec ret;
				ret.tv_sec = (long long)ts64.tv_sec;
				ret.tv_nsec = ts64.tv_nsec;
				return ret;
		}
	#endif
		// Deliberately dont use kernel getnstimeofday(struct timespec*) as it is confusing, and implicit. Use one of below
		static inline void getnstimeofday_real(struct timespec *ts)
		{
				struct timespec64 ts64;
				ktime_get_real_ts64(&ts64);
				*ts = timespec64_to_timespec(ts64);
		}

		static inline void getnstimeofday_boot(struct timespec *ts)
		{
				struct timespec64 ts64;
				ktime_get_boottime_ts64(&ts64);
				*ts = timespec64_to_timespec(ts64);
		}

	#if !KS_HAS_RTC_TM_TO_TIME		/* Removed in newer kernels, reimplement */
		static inline void rtc_time_to_tm(unsigned long time, struct rtc_time *tm) {
			rtc_time64_to_tm(time, tm);
		}

		static inline int rtc_tm_to_time(struct rtc_time *tm, unsigned long *time) {
			*time = rtc_tm_to_time64(tm);
			return 0;
		}
	#endif

#else	// Kernel already has those functions. Define as compatibility for user-space
	// #define _POSIX_C_SOURCE 199309L
	#include <time.h>
	#include <sys/time.h>	// access gettimeofday()
	#include "kr_incs_time_jiff.h"
	// kernel/rtc.h, linux/time.h linux/rtc.h linux/tsc.h linux/ktime.h
	struct rtc_time {  // Identical to 'struct tm';
		int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
	};
	static inline void rtc_time64_to_tm(unsigned long time, struct rtc_time *rtc_tm) {
		struct timeval tv = {.tv_sec = (time_t) time, .tv_usec = 0};
		struct tm result; //tm contains time zone information tm == {rtc_time + timezone information, so, will copy only the rtc_time variables
		localtime_r(&tv.tv_sec, &result);
		memcpy(rtc_tm, &result, sizeof(*rtc_tm));
	}
	static inline void rtc_time_to_tm(unsigned long time, struct rtc_time *rtc_tm) { return rtc_time64_to_tm(time, rtc_tm); }

	static inline void time_to_tm(time_t totalsecs, int offset, struct tm *result){
		(void)offset;
		localtime_r(&totalsecs, result);
	}

	typedef s64 ktime_t;

	#define KTIME_MAX			((s64)~((u64)1 << 63))
	#define KTIME_MIN			(-KTIME_MAX - 1)
	#define KTIME_SEC_MAX			(KTIME_MAX / NSEC_PER_SEC)
	#define KTIME_SEC_MIN			(KTIME_MIN / NSEC_PER_SEC)

	#define ktime_sub(lhs, rhs)	((lhs) - (rhs))
	#define ktime_add(lhs, rhs)	((lhs) + (rhs))

	static inline ktime_t ktime_set(const s64 secs, const unsigned long nsecs)
	{
		if (unlikely(secs >= KTIME_SEC_MAX))
			return KTIME_MAX;

		return secs * NSEC_PER_SEC + (s64)nsecs;
	}

	static inline ktime_t ktime_get(void)
	{
		struct timeval tv;
		gettimeofday(&tv, NULL);
		return ktime_set(tv.tv_sec, tv.tv_usec * 1000);
	}

	/* nvmeib_public_ktime_get was removed - use ktime_get() directly */

	static inline int ktime_compare(const ktime_t cmp1, const ktime_t cmp2)
	{
		if (cmp1 < cmp2)
			return -1;
		if (cmp1 > cmp2)
			return 1;
		return 0;
	}

	static inline bool ktime_after(const ktime_t cmp1, const ktime_t cmp2)
	{
		return ktime_compare(cmp1, cmp2) > 0;
	}

	static inline bool ktime_before(const ktime_t cmp1, const ktime_t cmp2)
	{
		return ktime_compare(cmp1, cmp2) < 0;
	}

	static inline s64 ktime_divns(const ktime_t kt, s64 div)
	{
		return kt / div;
	}

	static inline s64 ktime_to_ns(const ktime_t kt)
	{
		return kt;
	}

	static inline s64 ktime_to_us(const ktime_t kt)
	{
		return ktime_divns(kt, NSEC_PER_USEC);
	}

	static inline s64 ktime_to_ms(const ktime_t kt)
	{
		return ktime_divns(kt, NSEC_PER_MSEC);
	}

	extern struct timezone sys_tz;			// Daniel set it to represent Israel.
	static inline void do_gettimeofday(struct timeval *tv)	{ gettimeofday(tv, &sys_tz); }
	static inline int getnstimeofday_real(struct timespec *ts)	{ return clock_gettime(CLOCK_REALTIME, ts); }
	static inline int getnstimeofday_boot(struct timespec *ts)	{ return clock_gettime(CLOCK_BOOTTIME, ts); }

/***********************         TIMESPEC         *****************************/

#define TIMESPEC_SEC_MAX 3999999999LL	// less than LLONG_MAX nsec, also if multiplied by 2, large enough for many years, with lots of 999..999 for visibility
#define NSEC_IN_1_SEC (1LL * 1000 * 1000 * 1000)
#define TIMESPEC_MAX_C99 ((struct timespec){TIMESPEC_SEC_MAX, 999999999LL})
#define TIMESPEC_ZERO ((struct timespec){0LL, 0LL})
#define MSEC_TO_NSEC(m) (1LL * (m) * 1000 * 1000)
#define NSEC_TO_SEC(n) ((1LL * (n) + 500000000) / 1000000000)
#define NSEC_TO_MSEC(n)	((1LL * (n) + 500000) / 1000000)
#define NSEC_TO_USEC(n)	((1LL * (n) + 500) / 1000)
#define SEC_TO_NSEC(s) (1LL * (s) * NSEC_IN_1_SEC)
#define SEC_TO_MSEC(s) (1LL * (s) * 1000)

#define are_equal(_x_, _y_) ({													\
	((sizeof(_x_) == sizeof(_y_)) && !memcmp(&(_x_), &(_y_), sizeof(_x_)));		\
})

static inline bool timespec_eq(const struct timespec a, const struct timespec b)
{
	return (a.tv_nsec == b.tv_nsec && a.tv_sec == b.tv_sec);
}

static inline struct timespec timespec_min(const struct timespec a, const struct timespec b)
{
	if (a.tv_sec < b.tv_sec)	return a;
	if (a.tv_sec > b.tv_sec)	return b;
	return (a.tv_nsec < b.tv_nsec ? a : b);
}

static inline struct timespec timespec_max(const struct timespec a, const struct timespec b)
{
	if (a.tv_sec > b.tv_sec)	return a;
	if (a.tv_sec < b.tv_sec)	return b;
	return (a.tv_nsec > b.tv_nsec ? a : b);
}

static inline void timespec_handle_nsec_overflow(struct timespec *r)
{
	// Optimized for one iteration, since we assume that it is the output of a single add/sub
	if ((r->tv_nsec) >= SEC_TO_NSEC(1)) {
		while ((r->tv_nsec) >= SEC_TO_NSEC(1)) {
			r->tv_nsec -= SEC_TO_NSEC(1);
			r->tv_sec++;
		}
	} else if ((r->tv_nsec) < 0) {
		while ((r->tv_nsec) < 0) {
			r->tv_nsec += SEC_TO_NSEC(1);
			--r->tv_sec;
		}
	}
}

#ifndef _LINUX_TIME32_H
static inline struct timespec timespec_add(const struct timespec a, const struct timespec b)
{
	struct timespec		r;
	r.tv_sec = a.tv_sec + b.tv_sec;
	r.tv_nsec = a.tv_nsec + b.tv_nsec;
	timespec_handle_nsec_overflow(&r);
	return r;
}

static inline struct timespec timespec_sub(const struct timespec a, const struct timespec b)
{
	struct timespec		r;
	r.tv_sec = a.tv_sec - b.tv_sec;
	r.tv_nsec = a.tv_nsec - b.tv_nsec;
	timespec_handle_nsec_overflow(&r);
	return r;
}
#endif	// #ifndef _LINUX_TIME32_H

static inline bool timespec_lt(const struct timespec a, const struct timespec b)
{
	return (a.tv_sec < b.tv_sec || (a.tv_sec == b.tv_sec && a.tv_nsec < b.tv_nsec));
}

static inline bool timespec_ge(const struct timespec a, const struct timespec b)
{
	return !timespec_lt(a, b);
}

static inline int64_t timespec_diff_ns(const struct timespec end, const struct timespec start)
{
	return (SEC_TO_NSEC(end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec));
}

static inline int64_t timespec_diff_ms(const struct timespec end, const struct timespec start)
{
	return (SEC_TO_MSEC(end.tv_sec - start.tv_sec) + NSEC_TO_MSEC(end.tv_nsec - start.tv_nsec));
}

static inline void timespec_update_by_a_few_nsec(struct timespec *a, int64_t nsec_to_add_CAN_BE_NEGATIVE)
{
	a->tv_nsec += nsec_to_add_CAN_BE_NEGATIVE;
	timespec_handle_nsec_overflow(a);
}

static inline struct timespec timespec_from_nsec(int64_t ns)
{
	struct timespec		r;
	r.tv_sec = ns / SEC_TO_NSEC(1);
	r.tv_nsec = ns % SEC_TO_NSEC(1);
	return r;
}

static inline int64_t timespec_to_nsec(const struct timespec ts)
{
	return (SEC_TO_NSEC(ts.tv_sec) + ts.tv_nsec);
}

static inline int64_t timespec_to_msec(const struct timespec ts)
{
	return (SEC_TO_MSEC(ts.tv_sec) + NSEC_TO_MSEC(ts.tv_nsec));
}

static inline void getnstimeofday_convert_boot_to_real(const struct timespec *ts_boot, struct timespec *ts_real)
{
	struct timespec					ts_boot_now;
	struct timespec					ts_real_now;

	getnstimeofday_boot(&ts_boot_now);
	getnstimeofday_real(&ts_real_now);
	*ts_real = timespec_add(ts_real_now, ts_boot_now);
	*ts_real = timespec_sub(*ts_real, *ts_boot);
}

/******************************************************************************/

#endif // __KERNEL__
#endif
