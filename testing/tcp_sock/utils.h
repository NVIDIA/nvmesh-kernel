#ifndef UTILS_H
#define UTILS_H

/* For 'current' */
#include <linux/sched.h>

#define DEBUG_TEST 0

#define trace_inf(x, y, fmt, ...) \
	pr_info("(%d/%d)[%.16s]" x "[%s](%d): " fmt, \
		current->pid, raw_smp_processor_id(), current->comm, \
		y, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define trace_wrn(x, y, fmt, ...) \
	pr_warn("(%d/%d)[%.16s]" x "[%s](%d): " fmt, \
		current->pid, raw_smp_processor_id(), current->comm, \
		y, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#define trace_err(x, y, fmt, ...) \
	pr_err("(%d/%d)[%.16s]" x "[%s](%d): " fmt, \
		current->pid, raw_smp_processor_id(), current->comm, \
		y, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#if 0
#define trace_dbg(x, y, fmt, ...) \
	printk("(%d/%d)[%.16s]" x "[%s](%d): " fmt, \
		current->pid, raw_smp_processor_id(), current->comm, \
		y, __FUNCTION__, __LINE__, ## __VA_ARGS__)
#endif

#if 0
#define trace_inf(x, y, fmt, ...) do { pr_info("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_wrn(x, y, fmt, ...) do { pr_warn("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_err(x, y, fmt, ...) do { pr_err("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#define trace_dbg(x, y, fmt, ...) do { printk("(%d/%d)" x "[%s](%d): " fmt, current->pid, raw_smp_processor_id(), y, __FUNCTION__, __LINE__, ## __VA_ARGS__); } while (0)
#endif

#define FILENAME kbasename(__FILE__)
#define _I(fmt, ...) trace_inf("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _W(fmt, ...) trace_wrn("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _E(fmt, ...) trace_err("%s", FILENAME, fmt, ## __VA_ARGS__)
#define _D(fmt, ...) if (!DEBUG_TEST); else; /* trace_dbg("%s", FILENAME, fmt, ## __VA_ARGS__) */
#define _T _I

#define FIN _D("-->\n")
#define FOUT _D("<--\n")
#define LINE _D("---\n")
#define IFIN _I("-->\n")
#define IFOUT _I("<--\n")

#endif /* UTILS_H */

