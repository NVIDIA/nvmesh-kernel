#ifndef NVMEIB_WD_H
#define NVMEIB_WD_H

#include "kr_incs.h"
/**
 * about watchdog 
 *  
 * there are two thins that we have to take care of when setting 
 * a watchdog: 
 *  
 * 1. scheduling - this is done by create wd_obj objects 
 *  				every object initialtes a thread witch scan
 *  				fuction.
 * 2. callbacks - we may define one or more nvmeib_wd_entry that 
 *  will be added to a scheduling object and called when the
 *  timing criterias are met. we add an entry to sched objec
 *  with the call nvmeib_wd_add_entry.
 */

struct nvmeib_wd_entry {
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct list_head entry;
	/*
	 function checks whether an entry is expired based on its submittion
	 time and the current
	 Param1 is my nvmeib_wd_entry->data (was saved when issuing the call)
	 Param2 is the time now in JIFFIES
	*/
	int (*function)(void *, unsigned long);

	/**
	 * used as a cookie
	 * 
	 * @param  
	 */
	void *data;

	/**
	 * true if the watchdog is registered. 
	 * considered to be false by default 
	 */
	bool wd_registered;
};

struct wd_obj;

// ROUNDED_HZ is a power of 2 that approximates 1 second
#define WD_ROUNDED_HZ (1 << SHIFT_HZ)
#define WD_DEFAULT_TIMEOUT_SEC 10
#define WD_DEFAULT_HZ_PER_BUCKET WD_ROUNDED_HZ
/*
    A value of 0 in timeout_sec or hz_per_bucket implies that a default value
    will be used.
    If n_buckets == 0, then it is calculated to match the other values.
*/
struct wd_obj* nvmeib_wd_create_on_cpu(unsigned int timeout_sec,
				unsigned int n_buckets,
				unsigned int hz_per_bucket,
				int cpu);

#define nvmeib_wd_create(t, n, h) nvmeib_wd_create_on_cpu(t, n, h, NVMEIB_CPU_INVALID)

void nvmeib_wd_remove(struct wd_obj *p);
void nvmeib_wd_add_entry(struct wd_obj *p, struct nvmeib_wd_entry *e);
void nvmeib_wd_del_entry(struct wd_obj *p, struct nvmeib_wd_entry *e);

void nvmeib_wd_mod_thiscpu_bucket(struct wd_obj *wd, unsigned long jif, int n);

static inline void nvmeib_wd_bucket_inc(struct wd_obj *wd, unsigned long jif) {
	nvmeib_wd_mod_thiscpu_bucket(wd, jif, 1);
}

static inline void nvmeib_wd_bucket_dec(struct wd_obj *wd, unsigned long jif) {
	nvmeib_wd_mod_thiscpu_bucket(wd, jif, -1);
}

int nvmeib_wd_init(void);
void nvmeib_wd_exit(void);

bool nvmeib_wd_is_pcpu(struct wd_obj *wd);
int nvmeib_wd_pcpu_get_cpu(struct wd_obj *wd);

//TODO:
//Hide this struct (specifically watchdog_armed,
//it must be reset atomically w/ bucket-dec op)
struct wd_info_common {
	/* the watchdog object */
	struct wd_obj *wd;
	/* watchdog entry */
	struct nvmeib_wd_entry watchdog_entry;
	/* NOT zero if we are in watchdog mode */
	int watchdog_armed;
	/* saved jiffies the first called on */
	unsigned long called_on;
	/* the last time the channel was used (in jiffies) */
	unsigned long last_used; 
	/* number time WD events raised to channel and ignored i.e.
	   wdc remained in WD */
	int n_events;
	/* callbacks */
	void *cntx;
	/* true if we wish to proceed to process */
	bool (*on_start)(void *cntx);
	/* 0 if no updated is needed, 1 for new WD entry */
	int (*process)(void *cntx, unsigned long time_passed);
	void (*on_end)(void *cntx);
	spinlock_t wdc_guard;
	int locking_pid;
	/* cpu for per-cpu WD (-1 for not per-cpu WD) */
	int pcpu_cpu;
};

void nvmeib_wd_init_wdc(struct wd_info_common *c);
void nvmeib_wd_add_wdc(struct wd_info_common *c);
void nvmeib_wd_remove_wdc(struct wd_info_common *c);
void nvmeib_wd_start_wdc(struct wd_info_common *c);
void nvmeib_wd_stop_wdc(struct wd_info_common *c);
bool nvmeib_wd_is_armed_wdc(struct wd_info_common *c);

#endif
