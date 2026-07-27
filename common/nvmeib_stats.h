#ifndef NVMEIB_STATS_H
#define NVMEIB_STATS_H

#include <linux/types.h>

/* Timewatch: an object to profile the time it takes to go from A to B.
 * for accuracy, we use the CPU clock (i.e. nanosec)!!!
 * it takes the time in point A, then at point B, & also summs the sample so we
 * can get the average of last N samples */
struct nvmeib_stats {
	ktime_t start_t;
	ktime_t end_t;
	ktime_t last_dt;
	ktime_t sum_dt;
	u64 counts;
#ifdef NVMEIB_STATUS_USE_EXTENDED
	ktime_t sum_dt_ext;		// Commented out to reduce struct size
	u64 counts_ext;
#endif
};

static inline void nvmeib_stats_init(struct nvmeib_stats *st) {
	memset(st, 0, sizeof(*st));
}

static inline void nvmeib_stats_set_start(struct nvmeib_stats *st) {
	st->start_t = nvmeib_public_ktime_get();
}
static inline bool nvmeib_stats_is_start(struct nvmeib_stats *st) {
	return (ktime_to_ns(st->start_t) != 0);
}
static inline void nvmeib_stats_set_end(struct nvmeib_stats *st) {
	st->end_t = nvmeib_public_ktime_get();
}

static inline s64 nvmeib_stats_sum_dt_ns(struct nvmeib_stats *st) {
	return ktime_to_ns(st->sum_dt);
}

#ifdef NVMEIB_STATUS_USE_EXTENDED
static inline void nvmeib_stats_measure(struct nvmeib_stats *st) {
	nvmeib_stats_set_end(st);
	st->last_dt = ktime_sub(st->end_t, st->start_t);
	st->sum_dt = ktime_add(st->sum_dt, st->last_dt);
	++st->counts;
}
#endif

static inline void nvmeib_stats_measureq(struct nvmeib_stats *st) {
	st->last_dt = ktime_sub(nvmeib_public_ktime_get(), st->start_t);
	st->sum_dt = ktime_add(st->sum_dt, st->last_dt);
	++st->counts;
}

#ifdef NVMEIB_STATUS_USE_EXTENDED
static inline void nvmeib_stats_measureq_ext(struct nvmeib_stats *st) {
	st->sum_dt_ext = ktime_add(st->sum_dt_ext, ktime_sub(nvmeib_public_ktime_get(), st->start_t));
	++st->counts_ext;
}

static inline void nvmeib_stats_sum_dt_ext_ns(struct nvmeib_stats *st) {
	return ktime_to_ns(st->sum_dt_ext);
}
#endif

/******************************************************************************/
// Tiny version of the above
struct nvmeib_stop_watch {
	ktime_t start_t;
	ktime_t accumulated_t;
	u16 num_measures;
};

static inline void nvmeib_stop_watch_init(struct nvmeib_stop_watch *st) {
	(*st) = (struct nvmeib_stop_watch){0};
}

static inline void nvmeib_stop_watch_start(struct nvmeib_stop_watch *st) {
	st->start_t = nvmeib_public_ktime_get();
}

static inline bool nvmeib_stop_watch_is_start(struct nvmeib_stop_watch *st) {
	return (st->start_t != 0);
}

static inline void nvmeib_stop_watch_stop(struct nvmeib_stop_watch *st) {
	ktime_t end_t = nvmeib_public_ktime_get();

	#if defined(DBLKDEV_SIMULATOR) && DBLKDEV_SIMULATOR==1
		BUG_ON(nvmeib_stop_watch_is_start(st) == false);
	#endif

	/* Protect against negative intervals (clock-slippage) */
	if (nvmeib_stop_watch_is_start(st) && ktime_after(end_t, st->start_t)) {
		st->accumulated_t += ktime_sub(end_t, st->start_t);
		st->num_measures++;
	}

	st->start_t = 0;
}

static inline u64 nvmeib_stop_watch_measureq(struct nvmeib_stop_watch *st) {
	nvmeib_stop_watch_stop(st); /* account from last start to measure */

	if (!st->accumulated_t)
		return 0;
	else
		return ktime_to_ns(st->accumulated_t);
}

#endif
