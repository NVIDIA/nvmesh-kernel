/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_UTILS_H
#define NVMEIB_UTILS_H

#include "nvmeib.h"

/**
 * convert formated gid to raw
 *
 * @param buf formated gid
 * @param raw resulted gid
 */
void scan_gid_str(char *buf, u8 raw[16]);
union ib_gid;
enum ib_wc_status;
struct ib_sa_path_rec;
void format_gid_raw(u8 raw[16], char *buf);
void format_gid(union ib_gid *gid, char *buf);
void guid_show(union ib_gid *gid);
void path_show(struct ib_sa_path_rec *pathrec);
const char *nvmeib_status_str(enum ib_wc_status *status);
const char *nvmeib_block_io_op_str(enum nvmeib_block_io_op op);

extern int nvmeib_debug_level(void);

#ifdef TRACE_CPUID
#define TRACE_CPUID_FMT(x) "(%llu)(%d/%d)[%.15s]" x "[%s](%d): "
#define TRACE_CPUID_ARGS(y) ts, current->pid, raw_smp_processor_id(), \
	current->comm, y, __FUNCTION__, __LINE__
#else
#define TRACE_CPUID_FMT(x) "(%d)" x "[%s](%d): "
#define TRACE_CPUID_ARGS(y) current->pid, y, __FUNCTION__, __LINE__
#endif

#define pr_exlog(level, x, y, fmt, ...) do { \
	pr_ ## level(TRACE_CPUID_FMT(x) fmt, TRACE_CPUID_ARGS(y), ## __VA_ARGS__); \
} while (0)

#ifdef CONFIG_NVMEIB_DEBUG
#define nvmeib_dbg(x, y, fmt, ...) \
	do { \
		if (nvmeib_debug_level() > 0) \
			pr_exlog(info, x, y, fmt, ## __VA_ARGS__); \
	} while (0)

#define MIN_TRACE 0

#else /* CONFIG_NVMEIB_DEBUG */  /* Use if (0) to avoid unused warnings */
#define nvmeib_dbg(x, y, fmt, ...) \
	do { \
		if (0) \
			pr_debug("(%d)" x "[%s](%d): " fmt, \
				current->pid, y, __FUNCTION__, __LINE__, ## __VA_ARGS__); \
	} while (0)

#define MIN_TRACE 2

#endif /* CONFIG_NVMEIB_DEBUG */

#define nvmeib_trace(x, y, fmt, ...) \
	do { \
		if (nvmeib_debug_level() >= MIN_TRACE) \
			pr_exlog(info, x, y, fmt, ## __VA_ARGS__); \
		else if (KS_PR_DEBUG_NONGPL) \
			pr_exlog(debug, x, y, fmt, ## __VA_ARGS__); \
	} while (0)
#define nvmeib_inf(x, y, fmt, ...) \
	pr_exlog(info, x, y, fmt, ## __VA_ARGS__)
#define nvmeib_wrn(x, y, fmt, ...) \
	pr_exlog(warn_ratelimited, x, y, fmt, ## __VA_ARGS__)
#define nvmeib_err(x, y, fmt, ...) \
	pr_exlog(err_ratelimited, x, y, fmt, ## __VA_ARGS__)

#ifdef CONFIG_NVMEIB_FINE
#	define nvmeib_fine nvmeib_dbg
#else
#	define nvmeib_fine(x, y, fmt, ...) \
		do { \
			if (0) \
				pr_exlog(debug, x, y, fmt, ## __VA_ARGS__); \
		} while (0)
#endif

#if KS_KBASENAME
#define FILENAME kbasename(__FILE__)
#else
static inline const char *nvmeib_basename(const char *path)
{
	const char *tail = strrchr(path, '/');
	return tail ? tail + 1 : path;
}
#define FILENAME nvmeib_basename(__FILE__)
#endif

#define _Dbuf(buf,len)\
do { \
	if (!nvmeib_serial_console())\
		nvmeib_dump_buf(buf, len);\
} while(0)

#define _Dpage(page)\
do { \
	if (!nvmeib_serial_console())\
		nvmeib_dump_page(page);\
} while(0)

#include "nvmeib_utils_bin_traces.h"

#define NVMEIBT_THROTTLE_INTERVAL (3 * HZ) // 3 sec
#define NVMEIBT_THROTTLE_BURST 2
#define _THROTTLED_T(interval, burst, fmt, ...)				\
do {														\
	static DEFINE_RATELIMIT_STATE(_rs___, interval, burst);	\
	if (__ratelimit(&_rs___)) _NT(_THROTTLED_T_t1, fmt, ## __VA_ARGS__);		\
} while (0);

#define _NTHROTTLED_T(__name, interval, burst, fmt, ...)				\
do {														\
	static DEFINE_RATELIMIT_STATE(_rs___, interval, burst);	\
	if (__ratelimit(&_rs___)) _NT(__name, fmt, ## __VA_ARGS__);		\
} while (0);

#ifndef wait_event_interruptible_lock_irq_timeout
#define wait_event_interruptible_lock_irq_timeout(wq, condition, lock,\
                          timeout)	({ \
	long ret__ = timeout; \
	 while(ret__ > 0 && !(condition)) {\
		spin_unlock_irq(&lock); \
		ret__ = wait_event_interruptible_timeout(wq, condition, ret__); \
		spin_lock_irq(&lock); \
	} \
	ret__; })
#endif



/* a filter of a device and used ports */
struct nvmeib_used_dev_ports {
	/* linked into list*/
	struct list_head link;
	/* indication for the filtration type*/
	bool fit_by_port_guid;
	union {
		struct {
			/* the name of the device */
			char name[IB_DEVICE_NAME_MAX + 1];
			/* bit mask of the used ports */
			u8 used_ports_mask[32];
			/* Used for SW GID matching */
			u64 sw_gid_value[2];
			u64 sw_gid_mask[2];
		} dev_ports;
		/* port gid to use */
		u8 gid[16];
	};
};

/* filter a port by giving its guid */
struct nvmeib_used_port {
	/* linked into list*/
	struct list_head link;
	/* port gid to use */
	u8 gid[16];
};

/* parse module parameter that holds the list of port guids to use */
int nvmeib_set_used_pots_guids(const char *param, unsigned max_len,
	struct list_head *used_dev_list);

/* parse module parameter that holds the list of devices to use */
int nvmeib_set_used_dev_list(const char *param, unsigned max_len,
	struct list_head *used_dev_list);

/* free the list of devices to use*/
void nvmeib_free_used_dev_list(struct list_head *used_dev_list);

struct nvmeib_rdma_ib_port_gid;
/* returns true if a device is not filtered out */
bool nvmeib_use_dev(struct list_head *used_dev_list, struct ib_device *device,
	u8 port, struct list_head *out_list);

int nvmeib_run_usermode_script(const char *script_name);	// Blocking!
int nvmeib_set_roce_lossy_mode_on(void);
int nvmeib_remove_unsafe_symbols(char* dst, const char* src);

struct sockaddr_storage;
char * nvmeib_tss(struct sockaddr_storage *a, char buf[], int len);
bool nvmeib_tss_cmp_addr(
	struct sockaddr_storage *a, struct sockaddr_storage *b);

#if defined(__KERNEL__)
/**
 * mutex_is_locked_by_me - is the mutex locked by current pid
 * @lock: the mutex to be queried
 *
 * Returns 1 if the mutex is locked by this pid, 0 otherwise.
 */
static inline int mutex_is_locked_by_me(struct mutex *l)
{
	return (mutex_is_locked(l) && __mutex_owner(l) == nvmeib_current());
}
#endif//__KERNEL__

#endif /* NVMEIB_UTILS_H */
