#ifndef NVMEIB_MEASURED_WORK_H
#define NVMEIB_MEASURED_WORK_H

#include "common/kr_incs.h"
#include "common/compat/kr_incs_time_rdtsc.h"

/**
 * struct measured_work - a workqe_struct wrapper that records enqueue timestamp.
 * @work:        underlying workqe_struct scheduled on a workqueue.
 * @enqueue_ticks: raw TSC ticks captured at enqueue time via nvmeib_public_rdtsc().
 *
 * Analogous to delayed_work wrapping work_struct with a timer.
 *
 * Timestamps use raw TSC ticks (nvmeib_public_rdtsc) for minimal overhead.
 * NO conversion to nanoseconds on the hot path — histogram bins store tick
 * ranges directly. Conversion to ns happens only when readers access the
 * histogram via procfs/JDR, using tsc_khz from metadata.
 *
 * Usage:
 *   struct measured_work mw;
 *   MEASURED_INIT_WORK(&mw, my_callback);
 *   nvmeib_schedule_work_on(cpu, &mw.work);     // caller schedules
 *
 * In callback:
 *   void my_callback(struct workqe_struct *work) {
 *       struct measured_work *mw = measured_work_from(work);
 *       nvmeib_wq_metrics_update(my_wq_hist, measured_work_wait_ticks(mw));
 *       struct my_struct *s = container_of(mw, struct my_struct, mw_field);
 *       ...
 *   }
 */
struct measured_work {
	struct workqe_struct work;
	u64 enqueue_ticks;
};

/**
 * MEASURED_INIT_WORK() - initialize a measured_work with a callback function.
 * @mw: pointer to the &struct measured_work to initialize.
 * @fn: work callback function (receives &mw->work).
 *
 * Must be called before scheduling.
 * Also records the enqueue timestamp via nvmeib_public_rdtsc().
 */
#define MEASURED_INIT_WORK(mw, fn) \
	do { \
		WQ_INIT_WORK(&(mw)->work, (fn)); \
		(mw)->enqueue_ticks = nvmeib_public_rdtsc(); \
	} while (0)

/**
 * measured_work_from() - recover measured_work from a workqe_struct pointer.
 * @work: workqe_struct pointer passed to the callback.
 *
 * Intended to be called at the beginning of a work callback to obtain
 * the enclosing &struct measured_work and then compute wait time.
 *
 * Return: pointer to the containing &struct measured_work.
 */
static inline struct measured_work *measured_work_from(struct workqe_struct *work)
{
	return container_of(work, struct measured_work, work);
}

/**
 * measured_work_wait_ticks() - compute queue-wait duration in TSC ticks.
 * @mw: measured_work stamped before scheduling.
 *
 * Should be called at the beginning of the work callback, before doing
 * any significant processing, to get an accurate measurement.
 *
 * Return: elapsed TSC ticks since the enqueue timestamp.
 */
static inline u64 measured_work_wait_ticks(const struct measured_work *mw)
{
	return nvmeib_public_rdtsc() - mw->enqueue_ticks;
}

#endif /* NVMEIB_MEASURED_WORK_H */
