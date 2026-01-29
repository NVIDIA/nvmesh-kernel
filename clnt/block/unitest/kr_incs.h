/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_H
#define KR_INCS_H
/*
 * Simulation of linux kernel API by user space library.
 * Intention: Debug NVMesh's linux kernel driver externally (possibly on different OS)
 */

// User space includes
#include <stdlib.h>
#include <stddef.h>     		// offsetof() macro
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>						// Simulating BUG_ON interrupt
#include <stdlib.h>
#include <semaphore.h>
#include <stdarg.h>
#include <sys/auxv.h>
#include "nvmeib_uuid_be.h"
#include "nvmeib_str.h"
#include "nvmeib_math.h"
#include "nvmeib_nvme.h"

void* sim_kmalloc(size_t size, gfp_t flags);
void* sim_kzalloc(size_t size, gfp_t flags);
void* sim_vmalloc(size_t size);
void sim_kfree(const void *addr);
void sim_vfree(const void *addr);

#if defined(__KERNEL__)
	#error	"Simulator is not running in kernel, it emulates the kernel"		// This is just a trap
#endif
#define BUG_ON(      condition) 				({ const int hit___ = !!(condition); if (hit___) panic("************************** BUG!!!! at %s, %s() line %d, condition=%s\n", __kget_curr_time_stamp(), __FUNCTION__, __LINE__, #condition); })
#define WARN(        condition, format, ...)	({ const int hit___ = !!(condition); if (hit___) { panic(format, ##__VA_ARGS__); } hit___; })
#include "../common/compat/kr_incs_types.h"
#include "../common/compat/kr_incs_time.h"
#include "../common/compat/kr_incs_asserts.h"
#include "../common/compat/kr_incs_data_structs.h"
#include "../common/compat/kr_incs_sched.h"
#include "../common/compat/kr_incs_percpu.h"
#include "../common/compat/kr_incs_locks.h"
/******************************* Simulator Build Config ********************************/

/*
 * whether new kthreads are assigned to cpu's with RR policy (can cause deadlocks without scheduling) or by picking a cpu on which no thread is running
 * (used until we support scheduling)
 */
//#define SCHED_NEW_KTHREAD_ROUND_ROBIN

/************************** forwards declarations ****************************/
struct kernel_timers;
struct task_struct;

#include "common/kr_version.h"
#define KS_IB_VERBS_SUPPORTS_FMR 1			/* This was removed in kernels 5.10+ for simulator lets just support it */
#define KS_REQUEST_QUEUE_HAS_REQUEST_FN 1	/* This was removed in kernels 5.9+ for simulator lets just support it */
#define KS_HAS_PROFILE_EVENT_REGISTER 1		/* This enables the pre 5.19 mini-elevator using profile_event_register From 5.19+, we use kprobes which we don't want to add to the simulator */
#undef KS_BVEC_ITER
#define KS_BVEC_ITER (0)					// Dont use New BIO api, to test the internal implementation
#define KS_BIO_HAS_BI_BDEV_PTR (1)
/******************************* Kernel macros *******************************/

extern pthread_t 	main_os_id;

typedef unsigned long nodemask_t; /* Not really used, more for compilation */

int  printk(const char *fmt, ...)  __attribute__ ((format (printf, 1, 2)));
void printk_enable(bool on_off);						// Disable/Enable all logs
bool printk_is_enabled(void);
#define printk_once(fmt, ...)({		\
	static bool __print_once;		\
	if (!__print_once) { __print_once = true; printk(fmt, ##__VA_ARGS__); } \
})
//#define printk_ratelimited(...)  printk(##__VA_ARGS__)	// Daniel: Rate limited not supported
#define printk_ratelimited  printk							// Daniel: Rate limited not supported
#ifndef pr_fmt
	#define pr_fmt(fmt) fmt
#endif
//include/printk.h  /kern_levels.h
#define KERN_SOH	"\001"				/* ASCII Start Of Header */
#define KERN_EMERG	KERN_SOH "0"		/* system is unusable */
#define KERN_ALERT	KERN_SOH "1"		/* action must be taken immediately */
#define KERN_CRIT	KERN_SOH "2"		/* critical conditions */
#define KERN_ERR	KERN_SOH "3"		/* error conditions */
#define KERN_WARNING	KERN_SOH "4"	/* warning conditions */
#define KERN_NOTICE	KERN_SOH "5"		/* normal but significant condition */
#define KERN_INFO	KERN_SOH "6"		/* informational */
#define KERN_DEBUG	KERN_SOH "7"		/* debug-level messages */
#define KERN_DEFAULT	KERN_SOH "d"	/* the default kernel loglevel */
#define KERN_CONT	""					/* Annotation for a 'continued" line */

#define KERN_COL_RED        "\x1b[31m"	// Colors description is here https://telepathy.freedesktop.org/doc/telepathy-glib/telepathy-glib-debug-ansi.html
#define KERN_COL_GREEN      "\x1b[32m"
#define KERN_COL_YELLOW     "\x1b[1;33m"
#define KERN_COL_RESET      "\x1b[0;0m"
#define KERN_COL_RED_BOLD   "\x1b[1;31m"	// Bold format is 1;
#define KERN_COL_WHITE_BOLD "\x1b[1;37m"
#define pr_emerg(fmt, ...) 	printk(KERN_EMERG KERN_COL_RED_BOLD fmt KERN_COL_RESET, ##__VA_ARGS__)
#define pr_alert(fmt, ...) 	printk(KERN_ALERT KERN_COL_YELLOW fmt KERN_COL_RESET, ##__VA_ARGS__)
#define pr_crit(fmt, ...)  	printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)   	printk(KERN_ERR KERN_COL_GREEN fmt KERN_COL_RESET, ##__VA_ARGS__)
#define pr_warning(fmt,...)	printk(KERN_WARNING fmt , ##__VA_ARGS__)
#define pr_notice(fmt, ...)	printk(KERN_NOTICE fmt , ##__VA_ARGS__)
#define pr_info(fmt, ...)  	printk(KERN_INFO fmt , ##__VA_ARGS__)
#define pr_debug(fmt, ...)  printk(KERN_DEBUG fmt , ##__VA_ARGS__)

#define pr_emerg_ratelimited( fmt, ...)	printk_ratelimited(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert_ratelimited( fmt, ...) printk_ratelimited(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit_ratelimited(  fmt, ...) printk_ratelimited(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err_ratelimited(   fmt, ...)	printk_ratelimited(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warn_ratelimited(  fmt, ...) printk_ratelimited(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice_ratelimited(fmt, ...) printk_ratelimited(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info_ratelimited(  fmt, ...)	printk_ratelimited(KERN_INFO fmt, ##__VA_ARGS__)

//#define pr_cont(fmt, ...)  	printk(KERN_CONT fmt , ##__VA_ARGS__)
#define pr_warn pr_warning

#define trace_printk(fmt, ...)		pr_debug(fmt , ##__VA_ARGS__)

/*
 * Allocation tracking control mechanism
 *
 * This provides a way to disable allocation tracking for specific functions
 * or code sections.
 */
extern bool simulator_alloc_tracking_enabled;

void simulator_alloc_tracking_init(void);

static inline bool simulator_alloc_tracking_is_enabled(void)
{
	return simulator_alloc_tracking_enabled;
}

static inline void simulator_alloc_tracking_disable(void)
{
	simulator_alloc_tracking_enabled = false;
}

static inline void simulator_alloc_tracking_enable(void)
{
	simulator_alloc_tracking_enabled = true;
}

void dump_stack(void);

int vscnprintf(char *buf, size_t size, const char *fmt, va_list args);

// linux/errno.h
#define ENOTSUPP	524	/* Operation is not supported */

// include/asm/barriers
#if defined(__aarch64__)
	#define mb()	asm volatile("dsb ish" ::: "memory")
	#define rmb()	asm volatile("dsb ishld" ::: "memory")
	#define wmb()	asm volatile("dsb ishst" ::: "memory")
#elif defined(__i386__)
	/* Some non-Intel clones support out of order store. wmb() ceases to be a nop for these. */
	#define mb()    asm volatile("lock; addl $0,0(%%esp)" ::: "memory")
	#define rmb()   asm volatile("lock; addl $0,0(%%esp)" ::: "memory")
	#define wmb()   asm volatile("lock; addl $0,0(%%esp)" ::: "memory")
#elif defined(__x86_64__)
	#define mb()    asm volatile("mfence" ::: "memory")
	#define rmb()   asm volatile("lfence" ::: "memory")
	#define wmb()   asm volatile("sfence" ::: "memory")
#else
	#error "unsupported architecture"
#endif

bool insert_single_failure(void);				// Daniel's helper function to generate failure only on first call (used for debugging)
const char* __kget_curr_time_stamp(void);		// Daniel's artificial function
void BREAKPOINT(bool do_state_dump);			// Daniel's artificial function, Dump state before break point or not
#define panic(fmt, ...) ({pr_emerg(fmt,  ##__VA_ARGS__); dump_stack(); BREAKPOINT(true);})

#define simple_strtoul(str, endPtr, base)		strtoul(str, endPtr, base)
#define simple_strtoull(str, endPtr, base)		strtoull(str, endPtr, base)


// linux/typecheck
#define typecheck(   type, x) 	((void)(x==x))				// Always evaluates to true
#define typecheck_fn(type, f) 	((void)(f==f))

// linux/random
static inline unsigned int get_random_u32(void){ return (unsigned int)(rand()&0xFFFFFFFF); }

// linux/numa.h
#define	NUMA_NO_NODE	(-1)

/************************ Shut down / restart kernel *************************/
// a configuration given to our kernel simulator for initialization
struct ut_conf_kernel {
	int		num_ecpu;	// the number of ecpu to create
	int 	drain_workqueue_is_infinite; //after a certain period of no-progress, drain_workqueue will complain
										 //and prevent convinient debugging; set this to true to make it limitless
};

// linux/reboot
void machine_restart(const struct ut_conf_kernel 	*krn_prm);
void machine_power_off(void);
int get_default_ecpu_count(void);

/************************* Kernel types and structs **************************/
#if 1
	// Byte ordering, simulate this via irreversible function to test wrong usage
	#define cpu_to_le64(x)  ((u64)((x)+0x7AF0F0F0F0F0F0F0))
	#define cpu_to_be64(x)  ((u64)((x)+0x1BF0F0F0F0F0F0F0))
	#define cpu_to_le32(x)  ((u32)((x)+0x7AF0F0F0))
	#define cpu_to_be32(x)	((u32)((x)+0x1BF0F0F0))
	#define cpu_to_le16(x)	((u16)((x)+0x7BF0))
	#define cpu_to_be16(x)	((u16)((x)+0x1BF0))
	#define le64_to_cpu(x)  ((u64)((x)-0x7AF0F0F0F0F0F0F0))
	#define be64_to_cpu(x)  ((u64)((x)-0x1BF0F0F0F0F0F0F0))
	#define le32_to_cpu(x)  ((u32)((x)-0x7AF0F0F0))
	#define be32_to_cpu(x)  ((u32)((x)-0x1BF0F0F0))
	#define le16_to_cpu(x)  ((u16)((x)-0x7BF0))
	#define be16_to_cpu(x)  ((u16)((x)-0x1BF0))
#else
	#define cpu_to_le64(x)  (x)
	#define cpu_to_be64(x)  (__builtin_bswap64(x))
	#define cpu_to_le32(x)  (x)
	#define cpu_to_be32(x)	(__builtin_bswap32(x))
	#define cpu_to_le16(x)	(x)
	#define cpu_to_be16(x)	(__builtin_bswap16(x))
	#define le64_to_cpu(x)  (x)
	#define be64_to_cpu(x)  (__builtin_bswap64(x))
	#define le32_to_cpu(x)  (x)
	#define be32_to_cpu(x)  (__builtin_bswap32(x))
	#define le16_to_cpu(x)  (x)
	#define be16_to_cpu(x)  (__builtin_bswap16(x))
#endif

#define htonl(x) cpu_to_be32(x)
#define htons(x) cpu_to_be16(x)
#define ntohl(x) be32_to_cpu(x)
#define ntohs(x) be16_to_cpu(x)

/*********** Page emulation: gfp.h, page_types.h, pgtable.h page.h ************/
#include "common/compat/kr_incs_crc32.h"
#include "common/compat/kr_incs_malloc.h"
#include "common/compat/kr_incs_bit_ops.h"
int  kget_num_allocs(void);					// Daniel artificial function to detect mem leacks
void kget_num_allocs_print(void);			// Daniel artificial function to detect mem leacks
void kget_num_allocs_start(void);			// Daniel artificial function to detect a lot of memory allocs
void kget_num_allocs_stop( void);			// Daniel artificial function to detect a lot of memory allocs

int get_user_pages_fast(unsigned long start, int nr_pages, int write, struct page **pages);
static inline unsigned long copy_from_user(void *to,  const void *from, unsigned long n){ memcpy(to , from, n   ); return 0 /*n*/;   }
static inline int           copy_to_user(  void *dst, const void *src , unsigned size){   memcpy(dst, src , size); return 0 /*size*/;}
static inline unsigned long nvmeib_trace_tsc_to_ns(unsigned long tsc) {	return tsc; }

// linux/kallsyms.h
unsigned long kallsyms_lookup_name(const char *name);

// linux/stacktrace.h
struct stack_trace {
	unsigned int nr_entries, max_entries;
	unsigned long *entries;
	int skip;	/* input argument: How many entries to skip */
};

// linux/threads.h
/*
 * TODO(EBA):
 * HACK: increase number of CPU's so we have a single kthread per cpu
 * This is required bcz:
 * 1) we have deadlock when 2 pthreads that are assigned to same cpu acquire the cpu & one is waiting for another - bcz they dont reschedule, they cannot continue.
 * 2) we have trouble with irq_flags bcz thread assume they run to completion & dont share the cpu with other threads, unless they yield.
 */
#define ECPU_MAX_COUNT 				(64)	// Max number of emulated CPU's which do scheduled async jobs (not including timers/wq etc). use a low number for easier debugging.
#define ECPU_MIN_COUNT 				(8)		// Min number of emulated CPU's
#define ECPU_IN_DBG_COUNT			(8)		// when debugging with debugger, use less cpu's.
#if ((ECPU_IN_DBG_COUNT < ECPU_MIM_COUNT) || (ECPU_IN_DBG_COUNT > ECPU_MAX_COUNT))
#error "ECPU count mismatch"
#endif
#define MAX_NUM_TIMERS_ENGINE       (4)		// Scale timers by having multiple background threads (each with its own timers DB). Those threads dont run on ECPU's
#define CONFIG_NR_CPUS			(ECPU_MAX_COUNT + MAX_NUM_TIMERS_ENGINE + 100)	// Declare enough cpu's to ensure each kthread has its own processors in the simulated kernel (25 for worqu
//20+80 actually CONFIG_NR_CPUS now depends on number of server disks
//since serjio allocates a thread
#define NR_CPUS		CONFIG_NR_CPUS

static inline char *kstrdup( const char *s,             gfp_t gfp){ char *d = kmalloc(strlen(s)+1, gfp); if (d) strcpy( d,s);     return d; }
static inline char *kstrndup(const char *s, size_t len, gfp_t gfp){ char *d = kmalloc(len      +1, gfp); if (d) strlcpy(d,s,len); return d; }
static inline void *kmemdup( const void *s, size_t len, gfp_t gfp){ char *d = kmalloc(len      +1, gfp); if (d) memcpy( d,s,len); return d; }

static inline char *sim_kstrdup( const char *s,             gfp_t gfp){ char *d = sim_kmalloc(strlen(s)+1, gfp); if (d) strcpy( d,s);     return d; }
static inline char *sim_kstrndup(const char *s, size_t len, gfp_t gfp){ char *d = sim_kmalloc(len      +1, gfp); if (d) strlcpy(d,s,len); return d; }
static inline void *sim_kmemdup( const void *s, size_t len, gfp_t gfp){ char *d = sim_kmalloc(len      +1, gfp); if (d) memcpy( d,s,len); return d; }

static inline unsigned num_online_cpus(void) { return NR_CPUS; }
#define num_possible_cpus num_online_cpus

// linux/sort
void sort(void *base, size_t num, size_t size, int (*cmp_func)(const void *, const void *), void (*swp_func)(void *, void *, int size));

#define DECLARE_HASHTABLE(name, bits)                                                                                  \
	struct name {                                                                                                      \
		u64 (*hash64)(u64, unsigned int);                                                                              \
		struct hlist_head htable[1 << (bits)];                                                                         \
	} name

#define DEFINE_HASHTABLE(name, bits)                                                                                   \
	DECLARE_HASHTABLE(name, bits) = {NULL, {[0 ...((1 << (bits)) - 1)] = HLIST_HEAD_INIT}}

//linux/radix_tree.h - Implemented using hash-table for simplicity */
#define RADIX_TREE_BKT_SHIFT	3

struct radix_tree_root {
	gfp_t			gfp_mask;
	DECLARE_HASHTABLE(tbl, 3);
};

#define RADIX_TREE_INIT(mask)	{			\
	.gfp_mask = (mask),						\
	.tbl = {NULL, {[0 ...7] = HLIST_HEAD_INIT}},	\
}

#define INIT_RADIX_TREE(root, gfp) ({ \
	(root)->gfp_mask = gfp; \
	hash_init((root)->tbl); \
})

#define RADIX_TREE(name, mask) struct radix_tree_root name = RADIX_TREE_INIT(mask)

int radix_tree_insert(struct radix_tree_root *, unsigned long, void *);
void *radix_tree_lookup(struct radix_tree_root *, unsigned long);
void *radix_tree_delete(struct radix_tree_root *, unsigned long);
bool radix_tree_empty(struct radix_tree_root *root);

//linux/list.h
#define HLIST_HEAD_INIT { .first = NULL }
#define HLIST_HEAD(name) struct hlist_head name = {  .first = NULL }
#define INIT_HLIST_HEAD(ptr) ((ptr)->first = NULL)
static inline void INIT_HLIST_NODE(struct hlist_node *h){h->next = NULL;h->pprev = NULL;}
static inline int hlist_unhashed(const struct hlist_node *h){ return !h->pprev;}
static inline int hlist_empty(const struct hlist_head *h){return !h->first;}
void __hlist_del(struct hlist_node *n);
void hlist_del(struct hlist_node *n);
void hlist_del_init(struct hlist_node *n);
void hlist_del_init_rcu(struct hlist_node *n);
#define hlist_entry(ptr, type, member) container_of(ptr,type,member)
#define hlist_for_each(pos, head)         for (pos = (head)->first; pos                           ; pos = pos->next)
#define hlist_for_each_safe(pos, n, head) for (pos = (head)->first; pos && ({ n = pos->next; 1; }); pos = n)
#define hlist_entry_safe(ptr, type, member) ({ typeof(ptr) _zp1 = (ptr); _zp1 ? hlist_entry(_zp1, type, member) : NULL; })
#define hlist_for_each_entry(pos, head, member)				\
	for (pos = hlist_entry_safe((head)->first, typeof(*(pos)), member); pos; \
	     pos = hlist_entry_safe((pos)->member.next, typeof(*(pos)), member))

#define hlist_for_each_entry_safe(pos, n, head, member) 		\
	for (pos = hlist_entry_safe((head)->first, typeof(*pos), member);\
	     pos && ({ n = pos->member.next; 1; });			\
	     pos = hlist_entry_safe(n, typeof(*pos), member))

#define __hlist_for_each_entry__ hlist_for_each_entry


static inline void hlist_add_head(struct hlist_node *n, struct hlist_head *h) {
	struct hlist_node *first = h->first;
	n->next = first;
	if (first)
		first->pprev = &n->next;
	h->first = n;
	n->pprev = &h->first;
}

// linux/hashtable.h

/* Use hash_32 when possible to allow for fast 32bit hashing in 64bit kernels. */
static inline u32 hash_32(u32 val, unsigned int bits){
	#define GOLDEN_RATIO_PRIME_32 0x9e370001UL
	const u32 hash = val * GOLDEN_RATIO_PRIME_32;
	return hash >> (32 - bits);
}
static inline u64 hash_64(u64 val, unsigned int bits) {
#if 0
	#define GOLDEN_RATIO_PRIME_64 0x9e37fffffffc0001UL
	u64 hash = val * GOLDEN_RATIO_PRIME_64;
#else
	u64 hash = val, n = hash;
	n <<= 18; hash -= n;
	n <<= 33; hash -= n;
	n <<= 3;  hash += n;
	n <<= 3;  hash -= n;
	n <<= 4;  hash += n;
	n <<= 2;  hash += n;
#endif
	return hash >> (64 - bits);	/* High bits are more random, so use them. */
}

#define HASH_SIZE(name) (ARRAY_SIZE(name.htable))
#define HASH_BITS(name) ilog2(HASH_SIZE(name))

#define hash_min(val, bits)	(sizeof(val) <= 4 ? hash_32(val, bits) : hash_64(val, bits))
static inline void __hash_init(struct hlist_head *ht, unsigned int sz) {
	unsigned int i;
	for (i = 0; i < sz; i++)
		INIT_HLIST_HEAD(&ht[i]);
}

#define __hash_func(key, hashtable) hashtable.hash64                                                                   \
	? hashtable.hash64(key, HASH_BITS(hashtable))                                                                      \
	: hash_min(key, HASH_BITS(hashtable))

#define hash_init(hashtable)                                                                                           \
	do {                                                                                                               \
		hashtable.hash64 = NULL;                                                                                       \
		__hash_init(hashtable.htable, HASH_SIZE(hashtable));                                                           \
	} while (0)

#define hash_add(hashtable, node, key)                                                                                 \
	hlist_add_head(node, &hashtable.htable[__hash_func(key, hashtable)])

static inline bool hash_hashed(struct hlist_node *node){ return !hlist_unhashed(node); }
static inline bool __hash_empty(struct hlist_head *ht, unsigned int sz) {
	unsigned int i;
	for (i = 0; i < sz; i++)
		if (!hlist_empty(&ht[i]))
			return false;
	return true;
}
#define hash_empty(hashtable) __hash_empty(hashtable.htable, HASH_SIZE(hashtable))

static inline void hash_del(    struct hlist_node *node){ hlist_del_init(    node); }
static inline void hash_del_rcu(struct hlist_node *node){ hlist_del_init_rcu(node); }
#define hash_for_each(name, bkt, obj, member)				\
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < (typeof(bkt))HASH_SIZE(name); (bkt)++) \
		hlist_for_each_entry(obj, &name.htable[bkt], member)

#define hash_for_each_rcu(name, bkt, obj, member)				\
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < (typeof(bkt))HASH_SIZE(name); (bkt)++)\
		hlist_for_each_entry_rcu(obj, &name.htable[bkt], member)

#define hash_for_each_safe(name, bkt, tmp, obj, member)			\
	for ((bkt) = 0, obj = NULL; obj == NULL && (bkt) < (typeof(bkt))HASH_SIZE(name); (bkt)++)\
		hlist_for_each_entry_safe(obj, tmp, &name.htable[bkt], member)

#define hash_for_each_possible(name, obj, member, key)			\
	hlist_for_each_entry(obj, &name.htable[__hash_func(key, name)], member)

#define hash_for_each_possible_rcu(name, obj, member, key)		\
	hlist_for_each_entry_rcu(obj, &name.htable[__hash_func(key, name)], member)

#define hash_for_each_possible_safe(name, obj, tmp, member, key)		\
	hlist_for_each_entry_safe(obj, tmp, &name.htable[__hash_func(key, name)], member)

#define __hash_for_each_safe__(_name_, _bkt_, __node__, _tmp_, _obj_, _member_)	\
	__node__ = NULL;	\
	(void)__node__;		\
	hash_for_each_safe(_name_, _bkt_, _tmp_, _obj_, _member_)

#define __hash_for_each_possible_safe__(_name_, _obj_, __node__, _tmp_, _member_, _key_)	\
	__node__ = NULL;	\
	hash_for_each_possible_safe(_name_, _obj_, _tmp_, _member_, _key_)

//linux/interval_tree_generic.h
/*
 * Template for implementing interval trees
 *
 * ITSTRUCT:   struct type of the interval tree nodes
 * ITRB:       name of struct rb_node field within ITSTRUCT
 * ITTYPE:     type of the interval endpoints
 * ITSUBTREE:  name of ITTYPE field within ITSTRUCT holding last-in-subtree
 * ITSTART(n): start endpoint of ITSTRUCT node n
 * ITLAST(n):  last endpoint of ITSTRUCT node n
 * ITSTATIC:   'static' or empty
 * ITPREFIX:   prefix to use for the inline tree definitions
 *
 * Note - before using this, please consider if generic version
 * (interval_tree.h) would work for you...
 */

#define INTERVAL_TREE_MAX_CAPACITY 10
#define INTERVAL_TREE_DEFINE(ITSTRUCT, ITRB, ITTYPE, ITSUBTREE, ITSTART, ITLAST, ITSTATIC, ITPREFIX)	\
ITSTATIC void ITPREFIX ## _insert(ITSTRUCT *node, struct rb_root *root)	 								\
{																										\
	ITSTRUCT **nodes = NULL;																			\
	if (root->rb_node == NULL){																			\
		root->rb_node = kzalloc(INTERVAL_TREE_MAX_CAPACITY*sizeof(ITSTRUCT*), GFP_KERNEL);				\
	}																									\
	nodes = (ITSTRUCT**)root->rb_node;																	\
	for (u16 i = 0; i < INTERVAL_TREE_MAX_CAPACITY; ++i){												\
		if (nodes[i] == NULL){																			\
			nodes[i] = node;																			\
			break;																						\
		} else if (i == INTERVAL_TREE_MAX_CAPACITY){													\
			BUG();																						\
		}																								\
	}																									\
}																										\
																										\
ITSTATIC void ITPREFIX ## _remove(ITSTRUCT *node, struct rb_root *root)									\
{																										\
	ITSTRUCT **nodes = NULL;																			\
	if (root->rb_node == NULL){																			\
		return;																							\
	}																									\
	nodes = (ITSTRUCT**)root->rb_node;																	\
	for (u16 i = 0; i < INTERVAL_TREE_MAX_CAPACITY; ++i){												\
		if (nodes[i] == node){																			\
			nodes[i] = NULL;																			\
		} else if (i == INTERVAL_TREE_MAX_CAPACITY){													\
			WARN(2, "node is not in the tree");															\
		}																								\
	}																									\
	for (u16 j = 0; j < INTERVAL_TREE_MAX_CAPACITY; ++j){												\
		if (nodes[j]){																					\
			return;																						\
		}																								\
	}																									\
	kfree(root->rb_node);																				\
	root->rb_node = NULL;																				\
}																										\
																										\
ITSTATIC ITSTRUCT * ITPREFIX ## _iter_first(struct rb_root *root, ITTYPE start, ITTYPE last)			\
{																										\
	ITSTRUCT **nodes = NULL;																			\
	if (root->rb_node == NULL){																			\
		return NULL;																					\
	}																									\
	nodes = (ITSTRUCT**)root->rb_node;																	\
	for (u16 i = 0; i < INTERVAL_TREE_MAX_CAPACITY; ++i){												\
		ITSTRUCT* node = nodes[i];																		\
		if (node && (ITSTART(node) <= start) && (last < ITLAST(node))){ 								\
			return node;																				\
		}																								\
	}																									\
	return NULL;																						\
}																										\
																										\
ITSTATIC ITSTRUCT *	ITPREFIX ## _iter_next(ITSTRUCT *node, ITTYPE start, ITTYPE last)		  			\
{																										\
	(void)node;																							\
	(void)start;																						\
	(void)last;																							\
	return NULL;																						\
}																										\

/******************************* Kernel strings ******************************/
/* @str start with @prefix?. @str: string to examine @prefix: prefix to look for. */
static inline bool strstarts(const char *str, const char *prefix){ return strncmp(str, prefix, strlen(prefix)) == 0; }

/***************************** bitrev.h **************************************/
extern u8 const byte_rev_table[256];
static inline u8  __bitrev8(  u8 x) { return byte_rev_table[x];}
static inline u16 __bitrev16(u16 x) { return (__bitrev8( x &   0xff) <<  8) | __bitrev8( x >>  8); }
static inline u32 __bitrev32(u32 x) { return (__bitrev16(x & 0xffff) << 16) | __bitrev16(x >> 16); }

#define __constant_bitrev32(x)	({	\
	u32 __x = x;			\
	__x = (__x >> 16) | (__x << 16);	\
	__x = ((__x & (u32)0xFF00FF00UL) >> 8) | ((__x & (u32)0x00FF00FFUL) << 8);	\
	__x = ((__x & (u32)0xF0F0F0F0UL) >> 4) | ((__x & (u32)0x0F0F0F0FUL) << 4);	\
	__x = ((__x & (u32)0xCCCCCCCCUL) >> 2) | ((__x & (u32)0x33333333UL) << 2);	\
	__x = ((__x & (u32)0xAAAAAAAAUL) >> 1) | ((__x & (u32)0x55555555UL) << 1);	\
	__x;								\
})

#define bitrev32(x) ({			\
	u32 ___x = x;				\
	__builtin_constant_p(___x) ? __constant_bitrev32(___x) : __bitrev32(___x); \
})

/**************************** Atomic Operations ******************************/
// Atomic operations /inlcude/asm/atomic.h
typedef struct { long long c; } atomic64_t, atomic_long_t; 	// c - counter. Artificial structto support {0} initialization
typedef struct { int       c; } atomic_t;					// c - counter
#define ATOMIC_INIT(i)	{i}
void atomic_set(             	atomic_t *v, int i);
int  atomic_read(      const 	atomic_t *v);
int  atomic_dec_return(			atomic_t *v);
int  atomic_inc_return(			atomic_t *v);
int  atomic_sub_return(  int x,	atomic_t *v);
int  atomic_add_return(  int x,	atomic_t *v);
void atomic_add(         int x, atomic_t *v);
void atomic_sub(         int x, atomic_t *v);
int  atomic_dec_and_test(     	atomic_t *v);
int  atomic_sub_and_test(int x,	atomic_t *v);
void atomic_inc(				atomic_t *v);
void atomic_dec(				atomic_t *v);
int  atomic_xchg(				atomic_t *v, int n);
int  atomic_cmpxchg(			atomic_t *v, int o, int n);
void	  atomic64_set(		  atomic64_t *v, long long i);
long long atomic64_dec_return(atomic64_t *v);
long long atomic64_inc_return(atomic64_t *v);
void 	  atomic64_inc(		  atomic64_t *v);
long long atomic64_read(const atomic64_t *v);

// Atomically adds @a to @v, so long as @v was not already @u.
static inline int __atomic_add_unless(atomic_t *v, int a, int u){
	int c, old;
	c = atomic_read(v);
	while (c != u && ((old = atomic_cmpxchg(v, c, c + a)) != c))
		c = old;
	return c;
}
static inline int atomic_add_unless(atomic_t *v, int a, int u){return __atomic_add_unless(v, a, u) != u; }
#define atomic_inc_not_zero(v)		atomic_add_unless((v), 1, 0)							// Atomically increments @v by 1, so long as @v is non-zero.
int atomic_dec_if_positive(atomic_t *v);

#ifdef __x86_64__
#define atomic_inc_volatile_int(i)			asm volatile ("lock; incl %0" : "+m"(i))		// The word lock is critical, or else this will be atomic only on 1 cpu. with more cores this will become non atomic
#define atomic_dec_volatile_int(i)			asm volatile ("lock; decl %0" : "+m"(i))		// TODO(EBA): consider using __sync_fetch_and_add(&(i), 1) & __sync_fetch_and_sub(&(i), 1)
#define atomic_inc_per_cpu_volatile_int(i)	asm volatile ("incl %0" : "+m"(i))				// simulator implements per-cpu properly so no need to lock while inc/dec
#define atomic_dec_per_cpu_volatile_int(i)	asm volatile ("decl %0" : "+m"(i))
	#ifndef barrier
		#define barrier()  asm volatile("": : :"memory")											//
	#endif
	#define smp_rmb() barrier()
	#define smp_wmb() barrier()
#else
#define atomic_inc_volatile_int(i)			__atomic_fetch_add (&i, 1, __ATOMIC_SEQ_CST)		// __ATOMIC_SEQ_CST is critical, or else this will be atomic only on 1 cpu. with more cores this will become non atomic
#define atomic_dec_volatile_int(i)			__atomic_fetch_sub (&i, 1, __ATOMIC_SEQ_CST))		// TODO(EBA): consider using __sync_fetch_and_add(&(i), 1) & __sync_fetch_and_sub(&(i), 1)
#define atomic_inc_per_cpu_volatile_int(i)	__atomic_fetch_add (&i, 1, __ATOMIC_ACQ_REL)				// simulator implements per-cpu properly so no need to lock while inc/dec
#define atomic_dec_per_cpu_volatile_int(i)	__atomic_fetch_sub (&i, 1, __ATOMIC_ACQ_REL)
#endif


/******************* Emulation of Linux locks and mutexes ********************/
#ifndef __USE_GNU
	#define __USE_GNU
#endif
#ifndef _GNU_SOURCE
	#define _GNU_SOURCE
#endif
#include <pthread.h>

// Built-in kernel spinlock_t (busy waiting lock)
#define FORCE_SPIN_LOCK_AS_MUTEX	// use to force a spinlock to be implemented using a Mutex
#if defined(__APPLE__) || defined(FORCE_SPIN_LOCK_AS_MUTEX)
#define _USE_SPIN_LOCK_AS_MUTEX
#endif

typedef struct spinlock {
	#ifdef DEBUG_SPINLOCKS
	int locked;
	const char *last_locker;
	void *last_bt[10];
	#endif
	#ifndef _USE_SPIN_LOCK_AS_MUTEX
		pthread_spinlock_t m;
	#else
		pthread_mutex_t m;
	#endif
} spinlock_t;
typedef struct spinlock raw_spinlock_t;			// Ugly....
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	#define __ARCH_SPIN_LOCK_UNLOCKED	PTHREAD_PROCESS_PRIVATE
#else
	#define __ARCH_SPIN_LOCK_UNLOCKED	PTHREAD_MUTEX_INITIALIZER
#endif
#define DEFINE_SPINLOCK(l) spinlock_t (l) = {.m=__ARCH_SPIN_LOCK_UNLOCKED}
#define __RAW_SPIN_LOCK_INITIALIZER(l){.m = __ARCH_SPIN_LOCK_UNLOCKED}
#define __SPIN_LOCK_INITIALIZER(lockname) { { .rlock = __RAW_SPIN_LOCK_INITIALIZER(lockname) } }
#define __SPIN_LOCK_UNLOCKED(lockname) (spinlock_t ) __SPIN_LOCK_INITIALIZER(lockname)
#define DECLARE_WAIT_QUEUE_HEAD(name)  wait_queue_head_t name;

int  	spin_lock_init(		spinlock_t *l);
int		spin_lock_destroy(	spinlock_t *l);
void 	spin_unlock(   		spinlock_t *l);
void 	spin_lock(     		spinlock_t *l);
int  	spin_trylock(  		spinlock_t *l);
int  	spin_is_locked(const spinlock_t *l);						 // Not suported yet
int  	spin_lock_irqsave_(	spinlock_t *l, unsigned long *flags);
#define spin_lock_irqsave(l,f) spin_lock_irqsave_(l,&f)	// Support for kernel macro definition
int  	spin_unlock_irqre_(	spinlock_t *l, unsigned long *flags);
#define spin_unlock_irqrestore(l,f) spin_unlock_irqre_(l,&f)
#define spin_lock_irq(  l) spin_lock(  l)
#define spin_unlock_irq(l) spin_unlock(l)

// /linux/irqflags.h
void raw_local_irq_save(   unsigned long *f);
void raw_local_irq_restore(unsigned long  f);
#define local_irq_save(f)  	 do { raw_local_irq_save(   &f); } while(0)
#define local_irq_restore(f) do { raw_local_irq_restore( f); } while (0)
bool irqs_disabled(void);		// Are interrupts disabled?

// /linux/hardirq.h
#define in_interrupt()		false		// Not supported yet
#define in_irq()			false		// Not supported yet

// Read-write locks /linux/rwlock_types.h /include/asm/rw_locks.h.
// Implementation based on http://heather.cs.ucdavis.edu/~matloff/158/PLN/RWLock.c
typedef union {
	s64 lock;
	struct {
		u32 read;
		s32 write;
	};
} arch_rwlock_t;
typedef struct {
	arch_rwlock_t raw_lock;
} rwlock_t;
#define RW_LOCK_BIAS		0x0001000;
#define __ARCH_RW_LOCK_UNLOCKED		{ RW_LOCK_BIAS }
#define __RW_LOCK_UNLOCKED(lockname) (rwlock_t){.raw_lock = __ARCH_RW_LOCK_UNLOCKED}
#define write_lock(lock)
#define read_lock(lock)

// /linux/mutex.h
struct mutex {
	pthread_mutex_t m;
	atomic64_t owner;
};						// Emulate kernel mutex by regular mutex

extern __thread struct task_struct	*kthread_self_task;
struct task_struct* tasks_self(void);
static inline void mutex_init(   struct mutex *mutex){ 			int rv = pthread_mutex_init(   &mutex->m, NULL); BUG_ON(rv!=0); }
static inline void mutex_destroy(struct mutex *mutex){ 			int rv = pthread_mutex_destroy(&mutex->m); BUG_ON(rv!=0); 		}
static inline void mutex_lock(   struct mutex *mutex){ 			int rv = pthread_mutex_lock(   &mutex->m); BUG_ON(rv!=0); }
static inline void mutex_unlock( struct mutex *mutex){ 			int rv; rv = pthread_mutex_unlock( &mutex->m);	BUG_ON(rv!=0);	}
static inline  int mutex_trylock(struct mutex *mutex){ 			int rv = pthread_mutex_trylock(&mutex->m);return rv; }
int mutex_is_locked(struct mutex*mutex);
static inline struct task_struct *__mutex_owner(struct mutex *lock){ (void)lock; BUG(); return NULL;}

#define DEFINE_MUTEX(mutexname)  struct mutex mutexname = {.m = PTHREAD_MUTEX_INITIALIZER}

// linux/semaphore.h
// no sem_destroy(&sem->sem) for now - hopelly not leaking anything
struct semaphore {
	sem_t   sem;
};
static inline void sema_init(struct semaphore *sem, int val)
{
	int rv = sem_init(&sem->sem, 0, val);BUG_ON(rv != 0);
}
static inline void down(struct semaphore *sem)
{
	int	rv = sem_wait(&sem->sem);
	BUG_ON(rv);
}
extern int down_interruptible(struct semaphore *sem);
extern int down_killable(struct semaphore *sem);
static inline int down_trylock(struct semaphore *sem)
{
	int rv = sem_trywait(&sem->sem);
	if (likely(rv == 0)) {
		return 0;
	}
	BUG_ON(errno != EAGAIN);
	return 1;
}

//int down_timeout(struct semaphore *sem, long jiffies);

static inline void up(struct semaphore *sem)
{
	int	rv = sem_post(&sem->sem);
	BUG_ON(rv);
}

// /linux/rwsem.h
struct rw_semaphore {
	long				count;
	raw_spinlock_t		wait_lock;
	struct list_head	wait_list;
};
#define __RWSEM_DEP_MAP_INIT(lockname)
#define RWSEM_UNLOCKED_VALUE		0x00000000L
#define __RAW_SPIN_LOCK_UNLOCKED(lockname) (raw_spinlock_t) __RAW_SPIN_LOCK_INITIALIZER(lockname)
#define __RWSEM_INITIALIZER(name) { RWSEM_UNLOCKED_VALUE,__RAW_SPIN_LOCK_UNLOCKED(name.wait_lock), LIST_HEAD_INIT((name).wait_list) __RWSEM_DEP_MAP_INIT(name) }
#define DECLARE_RWSEM(name) struct rw_semaphore name = __RWSEM_INITIALIZER(name)
#define init_rwsem(sem)	  do {DECLARE_RWSEM(__tmp_123); *sem = __tmp_123; } while (0)
static inline void down_read(         struct rw_semaphore *sem){ (void)sem; }
static inline int  down_read_trylock( struct rw_semaphore *sem){ (void)sem; return 1;}
static inline void down_write(        struct rw_semaphore *sem){ (void)sem; }
static inline int  down_write_trylock(struct rw_semaphore *sem){ (void)sem; return 1;}
static inline void up_read(           struct rw_semaphore *sem){ (void)sem; }
static inline void up_write(          struct rw_semaphore *sem){ (void)sem; }
static inline void downgrade_write(   struct rw_semaphore *sem){ (void)sem; }

/*********************** Emulation of Linux kthread.h ************************/
/* Struct which is used to synchronize an 'int' variable (named resource 'r') among a few threads.
 * Typically resource can be amount of tasks that worker threads must execute.
 * You should access the resource only after he successfully acquired a lock(), and remember to unlock() after usage
 */
typedef struct syncVar{
	pthread_mutex_t m;							// Mutex which is used to block threads via lock/unlock and allow only one to access the resource.
	pthread_cond_t	c;							// Thread communication: Condition variable used to signal/broadcast to all threads when resource is changed.
	volatile int	r;							// The resource we want to synchronize and share among all the threads.
} syncVar;

#define TASK_COMM_LEN			32 // 16 in Linux but we need longer names for UT threads.

// Emulation of Kernel wait queue (non busy waiting on queue of requests)
typedef struct __wait_queue_head {
	syncVar lock;
	struct list_head task_list;
} wait_queue_head_t;

struct blk_plug;

struct task_struct {							// Structure of information that describe each thread, todo: make agnostic to OS
	struct semaphore sem_sleep;					// A semaphore used for the kthread to sleep as it awaits for another thread to wake it. initially used only to allow creation of kthread without executing. the underlying pthread starts executing but then awaits a signal that it can execute. later, can be used to simulate the thread not RUNNABLE, until another thread calls wake_up to make it rUNNABLE again.
	pthread_spinlock_t lock;					// protected access to the object
	pthread_t   os_id;							// OS specific ID. returned by linux pthread_create(), in windows includes handle. Dont touch!!!!
	union {
		char threadName[TASK_COMM_LEN];			// Daniel's Optional: User given textual name of the thread. Just for readability
		char comm[TASK_COMM_LEN];				// Emulation for kernel task name, Name of the executable
	};
	int 		(*start_func)(void *param);		// starting point of execution
	void       *parameters;						// Additional parameters that can be passed to the thread
#ifndef __APPLE__
	int			pid;							// Emulating current_thread_info()->task->pid, while OS_id is the thread gettid()
#else
	pthread_t	pid;
#endif
	bool		should_stop;					// Becomes true when OS intends to stop the thread of this task
	bool 		is_sleeping;					// is waiting on waitqueue
	wait_queue_head_t *wq;						// Optional wait queue on which the thread is waiting. If not waiting, points to NULL
	atomic_t usage;
	u32			flags;							// various flags
#define TASK_STRUCT_FLAG_GETCPU		0x00000001	// 	 set when getcpu() was called yet putcpu() wasnt called yet.
#define PF_KTHREAD					0x00200000	/* I am a kernel thread */
	int			cpu_idx;						// the "physical cpu" on which we put the thread. for now it neveer changes, decided on kthread creation

	struct blk_plug *plug;
};

static inline void task_struct_set_should_stop(struct task_struct	*task)
{
	pthread_spin_lock(&task->lock);
	task->should_stop = true;
	pthread_spin_unlock(&task->lock);
}

static inline bool task_struct_is_should_stop(const struct task_struct	*_task)
{
	struct task_struct	*task = (struct task_struct	*)_task;
	bool is_should_stop;
	//we don't really have to take mutex here - anyway the answer may be incorrect before function returns
	//pthread_spin_lock(&task->lock);
	is_should_stop = task->should_stop;
	//pthread_spin_unlock(&task->lock);
	return is_should_stop;
}

static inline void task_struct_set_sleeping(struct task_struct	*task, bool is_sleeping)
{
	pthread_spin_lock(&task->lock);
	task->is_sleeping = is_sleeping;
	pthread_spin_unlock(&task->lock);
}

static inline bool task_struct_is_sleeping(const struct task_struct	*_task)
{
	struct task_struct	*task = (struct task_struct	*)_task;
	bool is_sleeping;

	pthread_spin_lock(&task->lock);
	is_sleeping = task->is_sleeping;
	pthread_spin_unlock(&task->lock);
	return is_sleeping;
}

// return the cpu on which the task is executing (read only)
static inline int task_processor_id(struct task_struct	*task)
{
	int		cpu_idx;
	pthread_spin_lock(&task->lock);
	cpu_idx = task->cpu_idx;
	BUG_ON((cpu_idx < 0) || (cpu_idx >= CONFIG_NR_CPUS));
	pthread_spin_unlock(&task->lock);
	return cpu_idx;
}

// implement the get_cpu() semantics of assigning the task to the cpu on which it is now executing.
static inline int task_get_cpu(struct task_struct	*task)
{
	int		cpu_idx;

	pthread_spin_lock(&task->lock);
	cpu_idx = task->cpu_idx;
	BUG_ON(task->flags & TASK_STRUCT_FLAG_GETCPU);
	BUG_ON((cpu_idx < 0) || (cpu_idx >= CONFIG_NR_CPUS));
	task->flags |= TASK_STRUCT_FLAG_GETCPU;
	pthread_spin_unlock(&task->lock);
	return cpu_idx;
}

static inline int task_put_cpu(struct task_struct	*task)
{
	int		cpu_idx;

	pthread_spin_lock(&task->lock);
	cpu_idx = task->cpu_idx;
	BUG_ON((task->flags & TASK_STRUCT_FLAG_GETCPU) == 0);
	task->flags &= ~TASK_STRUCT_FLAG_GETCPU;
	pthread_spin_unlock(&task->lock);
	return cpu_idx;
}

static inline void task_struct_attach_wait_queue(struct task_struct	*task, wait_queue_head_t *q)
{
	pthread_spin_lock(&task->lock);
	task->wq = q;
	pthread_spin_unlock(&task->lock);
}

// Run 'int func(void*)' as an async task. On error returns NULL
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...);
#define kthread_create(threadfn, data, namefmt, arg...) kthread_create_on_node(threadfn, data, NUMA_NO_NODE, namefmt, ##arg)
#define kthread_run(threadfn, data, namefmt, ...)	({						\
	struct task_struct *__task = kthread_create(threadfn, data, namefmt, ##__VA_ARGS__);	\
	if (__task)																				\
		wake_up_process(__task);															\
	__task;																					\
})
void kthread_bind(struct task_struct *task, unsigned int cpu);
void *kthread_data(struct task_struct *k);
int kthread_stop(struct task_struct *task);		// Input - task created by kthread_run(). If NULL, stops the current thread
bool kthread_should_stop(void);					// Use in your thread method to test if thread should terminate
#define do_exit(error_code) do { kthread_stop(NULL); return (int)(error_code); } while(0)
bool kthread_wait_for_paused(const char*threadName);
struct task_struct* get_current(void);			// Get the task of current thread. Curently not working
#define my_cpu_offset (get_current()->cpu_idx) // Unsafe
#define current get_current()
#define nvmeib_current() get_current()

#define preemptible() false
typedef struct {} call_single_data_t;

typedef struct cpumask { DECLARE_BITMAP(bits, NR_CPUS); } cpumask_t;
typedef struct cpumask *cpumask_var_t;

// It looks like, now,we cannot control the task state
#define TASK_INTERRUPTIBLE	1
#define set_current_state(state_value)	do {\
		(void)state_value;		\
	} while (0)

void init_waitqueue_head(wait_queue_head_t* q);
void wake_up(			 wait_queue_head_t* q);	// Signal to threads which handle requests to resume working.

int   __wait_event_interruptible(wait_queue_head_t *q); // Wait until someone wakes us up. Return -1 if must exit
long __wait_event_interruptible_timeout(wait_queue_head_t *q, unsigned long jiff);	// Wait until someone wakes us up, for max of specified jiffies. Return -1 if must exit, 0 on signal or timedout

/*
 * kernel implementation uses the schedule() API to constantly loop around testing the condition & yielding the processor.
 * yield must be done bcz we dont know when to test the condition, hence let other run, yet ensure we'll run again without starving others.
 * since the caller will eventually be scheduled again, it is assured that the condition will be checked again.  																												 .
 * as the system gets more loaded, the yield will take longer as the kernel will run many other threads till we get a slice again, thus allowing efficiency while also prompt awareness of the condition.
 * with pthreads, we simply dont sleep for too long, in order to wait for a signal while limiting the time it takes for us to re-test the condition 																			 .
 * TODO(EBA): I'm not sure about the change to exit loop upon event signal - need to verify this
 */
#define wait_event_interruptible_timeout(wq, condition, timeout) ({ \
	long    __ret = timeout; 										\
	ulong 	start_j = jiffies;										\
	ulong 	timeout_j = start_j + timeout;							\
	ulong	curr_j;													\
	for (; !(condition) ;) {										\
		curr_j = jiffies;											\
		if (curr_j >= timeout_j)									\
			break;													\
		__ret = __wait_event_interruptible_timeout(&wq, min(timeout_j - curr_j, (unsigned long)HZ/1000));\
		if ((__ret == 0/* we're signaled*/) || (__ret == -1/*need to exit*/)) \
			break; /* Exit the loop even if condition is false! */ 	\
	}																\
	__ret;															\
})

#define wait_event_interruptible(wq, condition)	wait_event_interruptible_timeout(wq, condition, LONG_MAX )

#define __wait_event_interruptible_lock_irq(wq, condition, lock, ret, cmd) ({ 	\
	for (;;) {																	\
		if (condition)															\
			break;																\
		ret = __wait_event_interruptible_timeout(&wq, (unsigned long)HZ/1000);	\
		if (ret == -1/*need to exit*/) 											\
			break; /* Exit the loop even if condition is false! */				\
		/* either timed-out or signaled. in either case, retest condition*/ 	\
		spin_unlock_irq(&lock);													\
		cmd;																	\
		schedule();																\
		spin_lock_irq(&lock);													\
	}																			\
	__ret;																		\
})


#define wait_event_interruptible_lock_irq(wq, condition, lock) ({	\
	int __ret = 0;													\
	if (!(condition))												\
		__wait_event_interruptible_lock_irq(wq, condition, lock, __ret, );	\
	__ret;															\
})

#define wait_event wait_event_interruptible

/* For unitests, launching regular user space detached threads (avoid pthread_create() bug) */
#define detached_thread_internal_params spinlock_t l

#define detached_thread_prepare_no_params() \
		pthread_attr_t attr; \
		pthread_t tID; \
		pthread_attr_init(&attr); \
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

typedef void* (*pthread_start_routine)(void*);
#define detached_thread_prepare(type, params)  \
	{ \
		type *params = (type*)sim_kmalloc(sizeof(type), GFP_KERNEL); \
		detached_thread_prepare_no_params()

#define detached_thread_launch(func, params, is_sync) \
		spin_lock_init(&(params)->l); \
		if (!(is_sync)){ \
			spin_lock(&(params)->l); \
			BUG_ON(pthread_create(&tID, &attr, (pthread_start_routine)(func), (void*)(params))); \
			spin_unlock(&(params)->l); /* Allow the thread to start running */ \
		} else { \
			func(params); \
		} \
		pthread_attr_destroy(&attr); \
	}

#define detached_thread_start_execution(p) \
	spin_lock(&p->l);		// No need to use: pthread_detach(pthread_self());

#define detached_thread_end_execution(params) \
	sim_kfree(params); \
	return NULL; /* or pthread_exit() */

/* Below is a simple example of bug in pthread_create() which can be avoided using above macros */
void* func_calc_sum_loop(void* unused);
void* func_calc_sum_loop_loop(void* unused);
void* func_calc_sum_loop_loop_loop(void* unused);

/******************************* sysfs_ops.h *********************************/
// linux/sysfs.h
struct kobject;
struct attribute {
	const char	*name;
	umode_t      mode;
};

struct sysfs_ops {
	ssize_t (*show )(struct kobject *, struct attribute *, char *);
	ssize_t (*store)(struct kobject *, struct attribute *, const char *, size_t);
};

/******************************* kref / Kobject ******************************/
// linux/kref
struct kref { atomic_t refcount; };							// /linux/kref.h
static inline void kref_init(struct kref *kref){ atomic_set(&kref->refcount, 1); }
static inline int kref_read(struct kref *kref){ return atomic_read(&kref->refcount);}
static inline void kref_get( struct kref *kref){
	/* If refcount was 0 before incrementing then we have a race
     * condition when this kref is freeing by some other thread right now.
     * In this case one should use kref_get_unless_zero()
     */
	WARN_ON_ONCE(atomic_inc_return(&kref->refcount) < 2);
}

static inline int  kref_sub( struct kref *kref, unsigned int count, void (*release)(struct kref *kref)){
	WARN_ON(release == NULL);
	if (atomic_sub_and_test((int) count, &kref->refcount)) {
		if (release)
			release(kref);
		return 1;
	}
	return 0;
}

static inline int  kref_put( struct kref *kref, void (*release)(struct kref *kref)){ return kref_sub(kref, 1, release); }
static inline int kref_get_unless_zero(struct kref *kref)
{
	return atomic_add_unless(&kref->refcount, 1, 0);
}

struct kobj_type {
	void (*release)(struct kobject *kobj);
	const struct sysfs_ops *sysfs_ops;
	struct attribute **default_attrs;
};

struct kobject {	// initial implementation with reference-counting only
	struct kref			kref;
	const char 			*name;
	struct kobj_type	*ktype;
};

static inline void kobject_init(struct kobject *kobj, struct kobj_type *ktype){
	//BUG_ON(!ktype);
	kobj->name = NULL;
	kref_init(&kobj->kref);
	kobj->ktype = ktype;
}

static inline struct kobject *kobject_get(struct kobject *kobj){
	if (kobj) { /* BUG_ON(!kobj->state_initialized); */ kref_get(&kobj->kref); } return kobj;
}

static void kobject_cleanup(struct kobject *kobj){
	struct kobj_type *t = kobj->ktype;
	const char *name = kobj->name;
	if (t && t->release)
		t->release(kobj);
	if (name)
		sim_kfree(name); /* free name if we allocated it */
}

static void kobject_release(struct kref *kref) {
	struct kobject *kobj = container_of(kref, struct kobject, kref);
	kobject_cleanup(kobj);
}

static inline void kobject_put(struct kobject *kobj){
	if (kobj) kref_put(&kobj->kref, kobject_release);
}

// /include/asm/cmpxchg.h
#ifdef __x86_64__
	static inline void __cmpxchg_wrong_size(void){ BUG_ON(1); }
	#define __raw_cmpxchg(ptr, old, New, size, lock) ({	\
		__typeof__(*(ptr)) __ret, __old = (old), __new = (New);	\
		switch (size) {								\
		case 1:	{ volatile u8  *__ptr = (volatile u8  *)(ptr); asm volatile(lock "cmpxchgb %2,%1" : "=a" (__ret), "+m" (*__ptr) : "q" (__new), "0" (__old) : "memory"); break; } \
		case 2:	{ volatile u16 *__ptr = (volatile u16 *)(ptr); asm volatile(lock "cmpxchgw %2,%1" : "=a" (__ret), "+m" (*__ptr) : "r" (__new), "0" (__old) : "memory"); break; } \
		case 4:	{ volatile u32 *__ptr = (volatile u32 *)(ptr); asm volatile(lock "cmpxchgl %2,%1" : "=a" (__ret), "+m" (*__ptr) : "r" (__new), "0" (__old) : "memory"); break; } \
		case 8: { volatile u64 *__ptr = (volatile u64 *)(ptr); asm volatile(lock "cmpxchgq %2,%1" : "=a" (__ret), "+m" (*__ptr) : "r" (__new), "0" (__old) : "memory"); break; } \
		default: __cmpxchg_wrong_size(); \
		} \
		__ret; \
	})
	#define __cmpxchg(		ptr, old, New, size)	__raw_cmpxchg(ptr, old, New, size, "")
	#define __sync_cmpxchg(	ptr, old, New, size)	__raw_cmpxchg(ptr, old, New, size, "lock; ")
	#define cmpxchg(		ptr, old, New)			__sync_cmpxchg(ptr, old, New, sizeof(*(ptr)))
#elif defined(__aarch64__)
	#define cmpxchg(ptr, old, New) ({				\
		__typeof__(*(ptr)) __old = (old), __new = (New); \
		__atomic_compare_exchange (ptr, &__old, &__new, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED); \
		__old; \
	})
#endif

// /include/sys/auxv.y
#ifndef _SYS_AUXV_H
	#define _SYS_AUXV_H 1
	#include <elf.h>
	#include <sys/cdefs.h>
	#include <bits/hwcap.h>
	__BEGIN_DECLS
	/* Return the value associated with an Elf*_auxv_t type from the auxv list passed to the program on startup.  If TYPE was not present in the auxv list, returns zero.  */
	extern unsigned long int getauxval (unsigned long int __type) __THROW __attribute_const__;
	__END_DECLS
#endif /* sys/auxv.h */

#define __user
#define __kernel
#define __force

// /linux/smp.h
#define raw_smp_processor_id()  (pthread_self() == main_os_id ? 0/*main is assigned to cpu 0*/ : task_processor_id(tasks_self()))
#define smp_processor_id()	raw_smp_processor_id()

// /linux/asm/paravirt.h
static inline ulong read_cr0(void) {   return (~0UL); }
static inline void write_cr0(ulong x){ (void)x;}
static inline void clts(void){}									// Todo: Implement
#define X86_CR0_TS (1UL << 3)
#define stts() write_cr0(read_cr0() | X86_CR0_TS)

/************************ Time / Date / Calendar  ****************************/




#define time_after(a,b)        ((long)(b) - (long)(a) < 0)
#define time_before(a,b)       time_after(b,a)
#define time_after_eq(a,b)  ((long)(a) - (long)(b) >= 0)
#define time_before_eq(a,b)    time_after_eq(b,a)

struct timer_list {
	struct rb_node rb_entry;		/* For implementation: one thread runs all timers one by one */
	unsigned long expires;            /* expiration value, in jiffies */
	void (*function)(unsigned long);  /* the timer handler function */
	unsigned long data;               /* lone argument to the handler */
	struct tvec_t_base_s *base;       /* internal timer field, do not touch */
	int 	cpu_id;					/* the index of cpu */
};

#define TIMER_CPUMASK           0x0003FFFF
#define TIMER_MIGRATING         0x00040000
#define TIMER_BASEMASK          (TIMER_CPUMASK | TIMER_MIGRATING)
#define TIMER_DEFERRABLE        0x00080000
#define TIMER_PINNED            0x00100000
#define TIMER_IRQSAFE           0x00200000
#define TIMER_ARRAYSHIFT        22
#define TIMER_ARRAYMASK         0xFFC00000

void __init_timer(struct timer_list *t, unsigned int flags);
#define init_timer(timer)  __init_timer((timer), 0)
#define __setup_timer(_timer, _fn, _data, _flags) do { \
		__init_timer((_timer), (_flags)); \
		(_timer)->function = (_fn);	\
		(_timer)->data = (_data); \
	} while (0)
#define setup_timer(timer, fn, data) __setup_timer((timer), (fn), (data), 0)
static inline void init_timer_key(struct timer_list *t, uint flags, const char *name, void /*struct lock_class_key*/ *key) {__init_timer(t, flags); (void)name; (void)key; };

int  mod_timer( struct timer_list *t, unsigned long expires);
static inline void add_timer(struct timer_list *t){ mod_timer(t, t->expires); }
int  del_timer( struct timer_list *t);
int del_timer_sync(struct timer_list *timer);	// This function tries to deactivate a timer. Upon successful (ret >= 0) exit the timer is not queued and the handler is not running on any CPU. (return == 0) when timer wasnt pending, (return > 0) when timer was pending.
int get_kernel_num_active_timers(void);			// Amount of kernel timers, some are armed, some are not.
void kernel_timers_drain(struct kernel_timers   *timers, bool force_expiration);	// debug methods
char *kernel_timers_dump(struct kernel_timers  	*timers, char *buf, int	size);		// debug methods
int timer_pending(const struct timer_list * timer);

// linux/workqueue.h, /linux/completion.h
struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
void delayed_work_timer_fn(unsigned long __data);
enum { /* not bound to any CPU, prefer the local CPU */ WORK_CPU_UNBOUND        = NR_CPUS, };

struct work_struct {
	/*atomic_long_t data_unused;*/
	struct list_head entry;						// Next work task
	work_func_t		func;
};

struct delayed_work {
	struct work_struct work;
	struct timer_list timer;
	struct workqueue_struct *wq; /* target workqueue and CPU ->timer uses to queue ->work */
	int cpu;
	//consider to create a single lock for all delayed_work, since the simulator
	//create a mutext for each delayed work that never being destroyed
	struct {
		struct mutex lock; /* protects the inclusion in timers and stop_request, so the dwork cannot be inserted to timers if stop is true */
		__concurrent_access bool stop_request; /*prevent from callback to insert new dwork while cancel_delayed_work_sync waits on the function to finish */

		//debug flags
		bool expect_recursive;
		bool expect_queued;
		bool expect_running;
	} simu;
};

#define __INIT_WORK(_work, _func, _onstack) do { \
		INIT_LIST_HEAD(&(_work)->entry);  \
		(_work)->func = (_func); \
	} while (0)
#define INIT_WORK(_work, _func) __INIT_WORK((_work), (_func), 0)

#define __INIT_DELAYED_WORK(_work, _func, _tflags)                                                      \
	do { 																								\
		INIT_WORK(&(_work)->work, (_func));                                               				\
		__setup_timer(&(_work)->timer, delayed_work_timer_fn, (ulong)_work, (_tflags) | TIMER_IRQSAFE); \
		(_work)->simu.stop_request = false;                                                             \
		mutex_init(&(_work)->simu.lock);																\
	} while (0)

#define INIT_DELAYED_WORK(_work, _func) __INIT_DELAYED_WORK(_work, _func, 0)

//#define WQ_NAME_LEN	(24)
struct workqueue_struct{ 						// single thread queue
	const char		*name;
	struct list_head w_list;					// List of 'struct work_struct *'
	__concurrent_access struct work_struct* curr_work;				// the work we are now executing, NULL when none
	struct mutex     add_mutex;					// Serialize addition of tasks to the queue
	struct semaphore sem;						// semaphore counter represent number of work item pending in queue
	int 			num_pending_works;
	int				num_canceled;
	struct task_struct	*worker_task;			// kthread of internal worker
	__concurrent_access bool            is_destroy /*: 1*/;			// is worker instructed to terminate
};
struct workqueue_struct *create_singlethread_workqueue(const char *name);
bool __queue_work(int cpu, struct workqueue_struct *wq, struct work_struct *work);
void drain_workqueue(  struct workqueue_struct *wq);
void flush_workqueue(struct workqueue_struct *wq);
bool workqueue_is_empty(struct workqueue_struct *wq);
void destroy_workqueue(struct workqueue_struct *wq);
char *workqueue_dump(struct workqueue_struct 	*wq, char	*buf, int max_size, bool add_items);
void workqueue_dump_works_to_log(struct workqueue_struct *wq, void (*print_fn)(const struct work_struct *));
/*static*/void __queue_delayed_work(     int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay);
static inline bool queue_delayed_work_on(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay){ __queue_delayed_work(cpu, wq, dwork, delay); return true; }
static inline bool queue_delayed_work(            struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay){ return queue_delayed_work_on(WORK_CPU_UNBOUND, wq, dwork, delay); }
#define queue_work(wq, work)			__queue_work(WORK_CPU_UNBOUND, wq, work)
#define queue_work_on(cpu, wq, work)	__queue_work(cpu, wq, work)
extern struct workqueue_struct *system_wq;
extern struct workqueue_struct *system_long_wq;
extern struct workqueue_struct *system_unbound_wq;
static inline bool schedule_work(struct work_struct *work){ return queue_work(system_wq, work); }
static inline bool schedule_work_on(int cpu, struct work_struct *work){ return queue_work_on(cpu, system_wq, work); }
void schedule_work_on_sys_wq_rand_cpu(struct work_struct *work);
static inline bool schedule_delayed_work_on(int cpu, struct delayed_work *dwork, unsigned long delay){ return queue_delayed_work_on(cpu, system_wq, dwork, delay); }
static inline bool schedule_delayed_work(            struct delayed_work *dwork, unsigned long delay){ return queue_delayed_work(        system_wq, dwork, delay); }
bool cancel_delayed_work_sync(struct delayed_work *dwork);	// return true if dwork was pending, false otherwise.
bool cancel_work(struct workqueue_struct *wq, struct work_struct *work);
bool flush_work(struct workqueue_struct *wq, struct work_struct *work);
#define nvmeib_schedule_work_on schedule_work_on

struct completion {
	volatile unsigned int done;
	struct semaphore wait;						// Daniel: I implemented completion with semmaphore unlike the kernel wait_queue_head_t
};

static inline void init_completion(  struct completion *x){ x->done = 0; sema_init(&x->wait, 0); }
static inline void reinit_completion(struct completion *x){ init_completion(x); }
#define COMPLETION_INITIALIZER_ONSTACK(work) (*({ init_completion(&work); &work; }))
#define DECLARE_COMPLETION_ONSTACK(work) struct completion work = COMPLETION_INITIALIZER_ONSTACK(work)
void wait_for_completion(struct completion *);
int wait_for_completion_timeout(struct completion *, unsigned long timeout);
static inline void wait_for_completion_interruptible(struct completion *comp){ wait_for_completion(comp); }

void reup(    struct completion *c);
void complete(           struct completion *);
void complete_all(       struct completion *);
bool completion_done(    struct completion *);
void completion_verify_not_waiting(struct completion *c);
#define nvmeib_reinit_completion reinit_completion
static inline long wait_for_completion_interruptible_timeout(struct completion *comp, unsigned long timeout) { return wait_for_completion_timeout(comp, timeout); }

#define workq_func_t    work_func_t
#define workqe_struct 	work_struct
#define workqe_struct_init {{0, 0},0}
#define wq_add_work  	queue_work
#define wq_flush_work 	flush_work
#define wq_cancel_work	cancel_work
#define workq_struct 	workqueue_struct
#define wq_flush 		flush_workqueue
#define wq_drain 		drain_workqueue
#define WQ_INIT_WORK 	INIT_WORK
#define wq_create 		create_singlethread_workqueue
#define wq_create_on(name, cpu) 	create_singlethread_workqueue(name)
#define wq_create_verbose wq_create
#define wq_destroy 		destroy_workqueue
#define wq_pid(wq) 		(((wq)->worker_task->pid)&0xffffffff)

// sched.h, processor.h
#define cond_resched() 	({ sched_yield();usleep(1); })
#define schedule()		({ sched_yield();usleep(1); })
static inline pid_t task_tgid_nr(struct task_struct *tsk){ (void)tsk; return getpgrp(); }
#define get_task_struct(tsk) do { atomic_inc(&(tsk)->usage); } while(0)

void __put_task_struct(struct task_struct *t);
static inline void put_task_struct(  struct task_struct *t){ if (atomic_dec_and_test(&t->usage)) __put_task_struct(t); }
static inline int   wake_up_process( struct task_struct *t){ up(&t->sem_sleep);  return 0;};

// uapi/linux/if.h
#define	IFNAMSIZ	16

/************************** Simulator of genhd.h *****************************/
// linux/kdev_t.h
#define MINORBITS	20
#define MINORMASK	((1U << MINORBITS) - 1)
#define MAJOR(dev)	((unsigned int) ((dev) >> MINORBITS))
#define MINOR(dev)	((unsigned int) ((dev) & MINORMASK))
#define MKDEV(ma,mi)	(((ma) << MINORBITS) | (mi))

// linux/fs.h, linux/blkdev.h
struct inode {
	umode_t			i_mode;
	dev_t			i_rdev;
};

#define FMODE_READ		((fmode_t)0x1)	/* file is open for reading */
#define FMODE_WRITE		((fmode_t)0x2)	/* file is open for writing */
#define FMODE_LSEEK		((fmode_t)0x4)	/* file is seekable */
#define FMODE_PREAD		((fmode_t)0x8)	/* file can be accessed using pread */
#define FMODE_PWRITE	((fmode_t)0x10)	/* file can be accessed using pwrite */
#define FMODE_EXEC		((fmode_t)0x20)	/* File is opened for execution with sys_execve / sys_uselib */
#define FMODE_NDELAY	((fmode_t)0x40)	/* File is opened with O_NDELAY (only set for block devices) */
#define FMODE_EXCL		((fmode_t)0x80)	/* File is opened with O_EXCL (only set for block devices) */

static inline unsigned iminor(const struct inode *inode){ (void)inode; return 11 /*Todo: MINOR(inode->i_rdev)*/;}
static inline unsigned imajor(const struct inode *inode){ (void)inode; return 12 /*Todo: MAJOR(inode->i_rdev)*/;}
static inline int  register_blkdev(  unsigned int majorVersion, const char *name){(void)majorVersion; (void)name; return 1; }
static inline void unregister_blkdev(unsigned int majorVersion, const char *name){(void)majorVersion; (void)name; 			}

struct file {
	int dummy;						// Daniel: Todo, currently not used
	void *private_data;
};

struct file_operations {
	struct module *owner;
	ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
	ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
	int (*open) (struct inode *, struct file *);
	loff_t (*llseek) (struct file *, loff_t, int);
	int (*release) (struct inode *, struct file *);
};

struct queue_limits {
	unsigned long		bounce_pfn;
	unsigned long		seg_boundary_mask;

	unsigned int		max_hw_sectors;
	unsigned int		max_sectors;
	unsigned int		max_segment_size;
	unsigned int		physical_block_size;		// this is the underlying sector size, i.e.: NVMEIBC_SECTOR_SIZE
	unsigned int		alignment_offset;
	unsigned int		io_min;
	unsigned int		io_opt;
	unsigned int		max_discard_sectors;
	unsigned int		max_write_same_sectors;
	unsigned int		discard_granularity;
	unsigned int		discard_alignment;

	unsigned short		logical_block_size;
	unsigned short		max_segments;
	unsigned short		max_integrity_segments;

	unsigned char		misaligned;
	unsigned char		discard_misaligned;
	unsigned char		cluster;
	unsigned char		discard_zeroes_data;
};

#define QUEUE_FLAG_QUEUED		1	/* uses generic tag queueing */
#define QUEUE_FLAG_STOPPED		2	/* queue is stopped */
#define	QUEUE_FLAG_SYNCFULL		3	/* read queue has been filled */
#define QUEUE_FLAG_ASYNCFULL	4	/* write queue has been filled */
#define QUEUE_FLAG_DYING		5	/* queue being torn down */
#define QUEUE_FLAG_BYPASS		6	/* act as dumb FIFO queue */
#define QUEUE_FLAG_BIDI			7	/* queue supports bidi requests */
#define QUEUE_FLAG_NOMERGES     8	/* disable merge attempts */
#define QUEUE_FLAG_SAME_COMP	9	/* complete on same CPU-group */
#define QUEUE_FLAG_FAIL_IO     10	/* fake timeout */
#define QUEUE_FLAG_STACKABLE   11	/* supports request stacking */
#define QUEUE_FLAG_NONROT      12	/* non-rotational device (SSD) */
#define QUEUE_FLAG_VIRT        QUEUE_FLAG_NONROT /* paravirt device */
#define QUEUE_FLAG_IO_STAT     13	/* do IO stats */
#define QUEUE_FLAG_DISCARD     14	/* supports DISCARD */
#define QUEUE_FLAG_NOXMERGES   15	/* No extended merges */
#define QUEUE_FLAG_ADD_RANDOM  16	/* Contributes to random pool */
#define QUEUE_FLAG_SECDISCARD  17	/* supports SECDISCARD */
#define QUEUE_FLAG_SAME_FORCE  18	/* force complete on same CPU */
#define QUEUE_FLAG_DEAD        19	/* queue tear-down finished */
#define QUEUE_FLAG_REGISTERED  29	/* queue has been registered to a disk */
#define NVMESH_QUEUE_FLAG_DEFAULT	((1 << QUEUE_FLAG_IO_STAT) | (1 << QUEUE_FLAG_STACKABLE) | (1 << QUEUE_FLAG_SAME_COMP) | (1 << QUEUE_FLAG_ADD_RANDOM))

#define BLK_MAX_CDB	16
typedef uint blk_qc_t;
struct bio;
struct request_queue;
typedef blk_qc_t (make_request_fn) (struct request_queue *q, struct bio *bio);

struct request_queue {
	struct kobject	kobj;
	struct queue_limits	limits;
	unsigned long	queue_flags;
	void			*queuedata;
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	make_request_fn	*make_request_fn;
#endif
	struct mutex	sysfs_lock;
};
#define blk_queue_dying(q)      test_bit(QUEUE_FLAG_DYING, &(q)->queue_flags)
static inline void queue_flag_set(unsigned int flag, struct request_queue *q){
	//queue_lockdep_assert_held(q);
	__set_bit(flag, &q->queue_flags);
}
static inline void queue_flag_set_unlocked(  unsigned int flag, struct request_queue *q){ __set_bit(  flag, &q->queue_flags); }
static inline void queue_flag_clear_unlocked(unsigned int flag, struct request_queue *q){ __clear_bit(flag, &q->queue_flags); }
static inline void blk_queue_flag_set(       unsigned int flag, struct request_queue *q){ queue_flag_set_unlocked(flag, q); }
static inline void blk_queue_flag_clear(     unsigned int flag, struct request_queue *q){ queue_flag_clear_unlocked(flag, q); }
static inline void blk_set_queue_dying(                         struct request_queue *q){ queue_flag_set(QUEUE_FLAG_DYING, q); }

struct hd_struct {
	atomic_t in_flight[2];		//[0] for READ, [1] for WRITE
	struct block_device *__dev;	// A shortcut. In real kernel this is not a direct pointer but allocated device
};

/* Simulator does NOT implement per IO stats. this requires a percpu struct on each partition.*/
#define blk_queue_io_stat(q)	test_bit(QUEUE_FLAG_IO_STAT, &(q)->queue_flags)
#define part_stat_lock()		(0)
#define __part_stat_add(cpu, part, field, addnd) (per_cpu_ptr((part)->dkstats, (cpu))->field += (addnd))
#define part_stat_unlock()
#define part_stat_inc(cpu, part0, ios)	do {(void)cpu;} while(0)
#define part_stat_add(cpu, part0, ticks, duration) do {(void)cpu; /*(void)ticks;*/ (void)duration;} while(0)

struct request_queue *blk_alloc_queue(gfp_t f);
static inline void __blk_get_queue(struct request_queue *q){ kobject_get(&q->kobj); }
static inline bool blk_get_queue(struct request_queue *q){
	if (likely(!blk_queue_dying(q))) {
		__blk_get_queue(q);
		return true;
	}
	return false;
}
static inline void blk_put_queue(struct request_queue *q){ kobject_put(&q->kobj);}
static inline void blk_cleanup_queue(struct request_queue *q) {
	mutex_lock(&q->sysfs_lock);	/* mark @q DYING, no new request or merges will be allowed afterwards */
	blk_set_queue_dying(q);
	mutex_unlock(&q->sysfs_lock);
	//BUG_ON(atomic_read(&q->kobj.kref.refcount) != 1);
	blk_put_queue(q);	// last of all, loose reference to queue, will be freed when its ref-count decreases to 0
}
#if !KS_HAS_NEW_BLK_ALLOC_QUEUE && KS_REQUEST_QUEUE_HAS_REQUEST_FN
	static inline void blk_queue_make_request(struct request_queue *q, make_request_fn *f) { q->make_request_fn = f; }
#endif
static inline void blk_queue_logical_block_size(	struct request_queue *q, unsigned short i ){ q->limits.logical_block_size  = i; }
static inline void blk_queue_physical_block_size(	struct request_queue *q, unsigned int   i ){ q->limits.physical_block_size = i; }
static inline void blk_queue_io_min(				struct request_queue *q, unsigned int 	i) { q->limits.io_min = i; }
static inline void blk_queue_io_opt(				struct request_queue *q, unsigned int 	i) { q->limits.io_opt = i; }
static inline void blk_queue_max_hw_sectors(		struct request_queue *q, unsigned int	i) { q->limits.max_hw_sectors = i; }

struct blk_plug_cb;
typedef void (*blk_plug_cb_fn)(struct blk_plug_cb *, bool);
struct blk_plug_cb {
	union {
		struct {						// Daniel: Tmp hacky implementation
			struct delayed_work w;
			int cpu_id;
		};
		struct list_head list;
	};
	blk_plug_cb_fn callback;
	void *data;
};
struct blk_plug_cb *blk_check_plugged(blk_plug_cb_fn unplug, void *data, int size);

struct blk_plug {
	unsigned long magic; /* detect uninitialized use-cases */
	struct list_head list; /* requests */
	struct list_head cb_list; /* md requires an unplug callback */
};

void blk_start_plug(struct blk_plug *);

// linux/genhd.h
#define DISK_NAME_LEN			32
struct gendisk {
	struct kobject	kobj;		// reference count of the block device we provide for the kernel to be able to access NVMesh.
	int major, first_minor, minors;
	struct request_queue *queue;
	char disk_name[DISK_NAME_LEN];
	int flags;
	bool	is_closing;			// whether the block device is being closed
	const struct block_device_operations *fops;
	void *private_data;			// Pointer to our actual block device
	struct hd_struct part0;
	sector_t nr_sects;			// Disk size in sectors
	void *kernel_ptr;			// Daniel: pointer to the kernel which holds this gendisk. Crucial for when using a few kernels simultanously (a few virtual machines)
	int	vol_i;					// Invented: Index of corresponding volume in client sim. volumes array
	struct mutex	lock;		// simulator implementation simplifier
};

#define GENHD_FL_REMOVABLE					1
#define GENHD_FL_MEDIA_CHANGE_NOTIFY		4
#define GENHD_FL_CD							8
#define GENHD_FL_UP							16
#define GENHD_FL_SUPPRESS_PARTITION_INFO	32
#define GENHD_FL_EXT_DEVT					64 /* allow extended devt */
#define GENHD_FL_NATIVE_CAPACITY			128
#define GENHD_FL_BLOCK_EVENTS_ON_EXCL_WRITE	256
#define GENHD_FL_NO_PART_SCAN				512
#define GENHD_FL_NO_PART GENHD_FL_NO_PART_SCAN

struct gendisk *alloc_disk(int minors);
void set_capacity(	 struct gendisk *disk, sector_t size);
sector_t get_capacity(struct gendisk *disk);
int  revalidate_disk(struct gendisk *disk);
extern void add_disk(struct gendisk *disk);		/* Implemented in external OS simulator */
void del_gendisk(	 struct gendisk *disk);
struct kobject *get_disk(struct gendisk *disk);
void put_disk(   	 struct gendisk *disk);
static inline void part_dec_in_flight(struct hd_struct *part, int rw) {
	// decrease the gendisk ref-count to prevent it from being freed while IO's are in-flight
	//struct gendisk	*disk = container_of(part, struct gendisk, part0);
	BUG_ON((rw < 0) || (rw > (int)ARRAY_SIZE(part->in_flight)));
	atomic_dec(&part->in_flight[rw]);
}
static inline void part_inc_in_flight(struct hd_struct *part, int rw) {
	BUG_ON((rw < 0) || (rw > (int)ARRAY_SIZE(part->in_flight)));
	atomic_inc(&part->in_flight[rw]);
}
static inline int part_in_flight(struct hd_struct *part){
	return atomic_read(&part->in_flight[0]) + atomic_read(&part->in_flight[1]);
}

static inline dev_t disk_devt(struct gendisk *disk){ return MKDEV(disk->major, disk->first_minor);}

/***************************** IO related types ******************************/
// Kernel block device. /linux/blk_types.h, /linux/fs.h /mm_types.h /scatter_list.h, linux/bio.h
#define bio_data_dir(bio)	((bio)->bi_rw & 1)
#define bio_set_dev(bio, dev) ((bio)->bi_bdev = (dev))	// API of new, post 3.10 kernels
#define __SET_BI_RW(b, rw) ({ (b)->bi_rw = rw; })			// Todo: Clean lowest bit and use: b->bi_rw |= rw ?
void __bio_clone_fast(struct bio *dst, struct bio *src);
#define __BIO_CLONE_FAST(dst, src) __bio_clone_fast(dst, src)
static inline void generic_start_io_acct(int rw, unsigned long sectors, struct hd_struct *part) { (void)rw; (void)sectors; (void)part; }
static inline void generic_end_io_acct(  int rw, struct hd_struct *part, unsigned long start_time) { (void)rw; (void)start_time; (void)part; }
static inline void blk_queue_max_discard_sectors(struct request_queue *q, unsigned int m) { q->limits.max_discard_sectors = m; }

#define READ			0
#define WRITE			1
#define READA			(1<<13)
#define REQ_DISCARD		(1<< 7)
#define REQ_WRITE_SAME	(1 << 9)

struct bio_vec {
	struct page	   *bv_page;		// Address to where read or write.
	unsigned int	bv_len;			// Length of the IO (typically 1 signel block)
	unsigned int	bv_offset;		// Typically 0 since we IO in units of full blocks
};

struct block_device {
	struct gendisk 	* bd_disk;
	struct inode 	* bd_inode;
	atomic_t 	n_active_ios;		// Daniel invented: Amount of IO requests that were sent to block device but haven't got a result yet (failure or success)
	int			status_of_last_io;	// Daniel invented: stores the 'rv' of the last completed io on this block device. For debugging
	struct kobject kobj;
};
#define _IO(nr,size) ((nr) << 16) | ((size) << 1)
#define BLKRRPART  _IO(0x12,95)	/* re-read partition table */
int ioctl_by_bdev(struct block_device *dev, unsigned cmd, unsigned long arg);
struct block_device *blkdev_get_by_path(const char *path, fmode_t mode, void *holder);
struct block_device *lookup_bdev(const char *path);
#define open_bdev_exclusive(...) ({blkdev_get_by_path(__VA_ARGS__); BUG() })			// Normally should never use this
struct block_device *bdget_disk(struct gendisk *disk, int partno);
int blkdev_put(struct block_device *bdev, fmode_t mode);
int do_vfs_ioctl(struct file *filp, unsigned int fd, unsigned int cmd, unsigned long arg);

struct block_device_operations {
	struct module *owner;
	int  (*open		)(struct block_device 	*, fmode_t);
	void (*release	)(struct gendisk 		*, fmode_t);		// For kernel version 3.10.0 and above
	int  (*ioctl	)(struct block_device 	*, fmode_t, unsigned, unsigned long);
	// int (*media_changed)(struct gendisk *gd); // Media changed removed from fops in Kernal 5.10+
#if KS_BLOCK_DEV_OPS_HAS_REVALIDATE_DISK
	int (*revalidate_disk)(struct gendisk *gd);
#endif
#if !KS_REQUEST_QUEUE_HAS_REQUEST_FN
	blk_qc_t (*submit_bio) (struct bio *bio);					// For kernel version 5.9 and above
#endif
};

#define BIO_CLONED	4		/* doesn't own data */
#define BIO_USER_MAPPED 6	/* contains user pages, simulated user space app will blocks until bio completes */
#define BIO_OWNS_VEC	13	/* bio_free() should free bvec */

struct bvec_iter {			// Linux 3.14+ newer bio iterator API
	sector_t			bi_sector;	/* device address in 512 byte sectors */
	unsigned int		bi_size;	/* residual I/O count */
	unsigned int		bi_idx;		/* current index into bvl_vec */
	unsigned int        bi_bvec_done;	/* number of bytes completed incurrent bvec */
};
struct bio_vec bio_iter_iovec(struct bio *bio, struct bvec_iter iter);	// Daniel: Instead of kernel define I used function to encapuslate the bio mechanism
void bio_advance_iter(struct bio *bio, struct bvec_iter *iter, unsigned bytes); // For KS_BVEC_ITER
typedef void (bio_end_io_t)(struct bio *, int);
struct bio {
	sector_t		bi_sector;	  	// device address in 512 byte sectors
	struct bio		*bi_next;	  	// request queue link
	struct block_device	*bi_bdev; 	// bdev (possibly remapped)
	unsigned long	bi_flags; 		// status, command, etc
	unsigned long	bi_rw;	  		// bottom bits READ/WRITE, top bits priority
	unsigned short	bi_vcnt;  		// how many bio_vec's
	unsigned short	bi_idx;	  		// current index into bvl_vec
	unsigned int	bi_phys_segments;// Number of segments in this BIO after physical address coalescing is performed.
	unsigned int	bi_size;  		// residual I/O count
	// To keep track of the max segment size, we account for the sizes of the first and last mergeable segments in this bio.
	unsigned int	bi_seg_front_size;
	unsigned int	bi_seg_back_size;
	bio_end_io_t   *bi_end_io;
	void		   *bi_private;
	//Everything starting with bi_max_vecs will be preserved by bio_reset()
	unsigned int	bi_max_vecs;	// max bvl_vecs we can hold
	atomic_t		__bi_cnt;		// pin count
	struct bio_vec *bi_io_vec;		// the actual vec list
	struct bio_set *bi_pool;
	struct completion *comp;
	struct di_tracker_io_context *dt_io_ctx;	// Invented. DI tracker's I/O context for this bio.
	struct bvec_iter	bi_iter;
	struct bio_vec	bi_inline_vecs[0]; //We can inline a number of vecs at the end of the bio, to avoid double allocations for a small number of bio_vecs. This member MUST obviously be kept at the very end of the bio.
};
#define bio_sectors(bio)	((bio)->bi_size >> 9)

#if KS_BIO_ALLOC_HAS_BLOCK_DEVICE
struct bio *bio_kmalloc(unsigned short nr_vecs, gfp_t gfp_mask);
#else
struct bio *bio_kmalloc(gfp_t gfp_mask, unsigned short nr_iovecs);
#endif

#define bio_free(bio) kfree(bio)
int bio_add_page(struct bio *bio, struct page *page, unsigned int len, unsigned int offset);
int bio_add_pc_page(void *q, struct bio *bio, struct page *page, unsigned int len, unsigned int offset);
//void bio_free_pages(struct bio *bio);
void bio_put(struct bio *bio);
void bio_endio(struct bio *, int);
#define bio_io_error(bio) bio_endio((bio), -EIO)

void bio_init(struct bio *bio
#if KS_BIO_INIT_WITH_BVEC
		, struct bio_vec *table, unsigned short max_vecs
#endif
		);

struct bio_list { struct bio *head, *tail;};
static inline int  bio_list_empty(const struct bio_list *bl){return bl->head == NULL;}
static inline void bio_list_init(       struct bio_list *bl){       bl->head = bl->tail = NULL;}
#define BIO_EMPTY_LIST	{ NULL, NULL }
#define bio_list_for_each(bio, bl) \
	for (bio = (bl)->head; bio; bio = bio->bi_next)
unsigned bio_list_size(const struct bio_list *bl);
void bio_list_add(           struct bio_list *bl, struct bio *bio);
void bio_list_add_head(      struct bio_list *bl, struct bio *bio);
void bio_list_merge(         struct bio_list *bl, struct bio_list *bl2);
void bio_list_merge_head(    struct bio_list *bl, struct bio_list *bl2);
static inline struct bio *bio_list_peek(   struct bio_list *bl){ return bl->head;}
struct bio *bio_list_pop(    struct bio_list *bl);
struct bio *bio_list_get(    struct bio_list *bl);

// 512B Support
#define bio_iovec_idx(bio, idx)    (&((bio)->bi_io_vec[(idx)]))
#define bio_iovec(bio)        bio_iovec_idx((bio), (bio)->bi_idx)
#define kmap_atomic(page) (page_address(page))
#define kunmap_atomic(page)

#define bio_for_each_segment(bvec, bio, iter)	\
	int bv_ind;	(void)iter;						\
	for (bv_ind = 0,							\
		 (bvec) = (bio)->bi_io_vec[bv_ind];		\
		 bv_ind < (bio)->bi_vcnt; bv_ind++)


static inline void nvmeib_bio_copy_data_with_offsets(struct bio *dst, unsigned dst_offset_start,
													 struct bio *src, unsigned src_offset_start)
{
	struct bio_vec *src_bv, *dst_bv;
	unsigned src_offset, dst_offset, bytes;
	void *src_p, *dst_p;

	src_bv = bio_iovec(src);
	dst_bv = bio_iovec(dst);

	src_offset = src_bv->bv_offset + src_offset_start;
	dst_offset = dst_bv->bv_offset + dst_offset_start;

	while (1) {
		if (src_offset == src_bv->bv_offset + src_bv->bv_len) {
			src_bv++;
			if (src_bv == bio_iovec_idx(src, src->bi_vcnt)) {
				src = src->bi_next;
				if (!src)
					break;

				src_bv = bio_iovec(src);
			}

			src_offset = src_bv->bv_offset;
		}

		if (dst_offset == dst_bv->bv_offset + dst_bv->bv_len) {
			dst_bv++;
			if (dst_bv == bio_iovec_idx(dst, dst->bi_vcnt)) {
				dst = dst->bi_next;
				if (!dst)
					break;

				dst_bv = bio_iovec(dst);
			}

			dst_offset = dst_bv->bv_offset;
		}

		bytes = min(dst_bv->bv_offset + dst_bv->bv_len - dst_offset,
			    src_bv->bv_offset + src_bv->bv_len - src_offset);

		src_p = kmap_atomic(src_bv->bv_page);
		dst_p = kmap_atomic(dst_bv->bv_page);

		memcpy(dst_p + dst_offset,
		       src_p + src_offset,
		       bytes);

		kunmap_atomic(dst_p);
		kunmap_atomic(src_p);

		src_offset += bytes;
		dst_offset += bytes;
	}
}

#include "../common/compat/kr_incs_sgl.h"

struct sg_page_iter {
	struct scatterlist	*sg;		/* sg holding the page */
	unsigned int		sg_pgoffset;	/* page offset within the sg */

	/* these are internal states, keep away */
	unsigned int		__nents;	/* remaining sg entries */
	int			__pg_advance;	/* nr pages to advance at the
						 * next step */
};

bool __sg_page_iter_next(struct sg_page_iter *piter);
void __sg_page_iter_start(struct sg_page_iter *piter, struct scatterlist *sglist, unsigned int nents, unsigned long pgoffset);
static inline struct page *sg_page_iter_page(struct sg_page_iter *piter) {
	/* panic if sg contains a virtual pointer and not a page */
	BUG_ON(piter->sg->offset&0x1);
	return nth_page(sg_page(piter->sg), piter->sg_pgoffset);
}

#define SG_MITER_ATOMIC		(1 << 0)	 /* use kmap_atomic (not relevant to sim) */
#define SG_MITER_TO_SG		(1 << 1)	/* flush back to phys on unmap (not relevant to sim) */
#define SG_MITER_FROM_SG	(1 << 2)	/* nop */

struct sg_mapping_iter {
	/* the following three fields can be accessed directly */
	struct page		*page;		/* currently mapped page */
	void			*addr;		/* pointer to the mapped area */
	size_t			length;		/* length of the mapped area */
	size_t			consumed;	/* number of consumed bytes */
	struct sg_page_iter	piter;		/* page iterator */

	/* these are internal states, keep away */
	unsigned int		__offset;	/* offset within page */
	unsigned int		__remaining;	/* remaining bytes on page */
	unsigned int		__flags;
};

void sg_miter_start(struct sg_mapping_iter *miter, struct scatterlist *sgl, unsigned int nents, unsigned int flags);
bool sg_miter_skip(struct sg_mapping_iter *miter, off_t offset);
bool sg_miter_next(struct sg_mapping_iter *miter);
void sg_miter_stop(struct sg_mapping_iter *miter);

/******************************* linux/proc_fs *******************************/
// linux/proc_fs.h
struct proc_dir_entry {					// Single proc file
	struct proc_dir_entry *next, *parent, *subdir;
	void *data;		// Directories dont have data, files do
	spinlock_t lock; /* Use to suncronize add and delete of sub dirs */
	char name[63];	// Local copy of the name of the directory
	u8 depth;		// For debug: depth in directories tree (nice alignment for 64bit)
	const struct file_operations  *fops; // Methods to access file
};

struct seq_file {
	void *private;
	const struct seq_operations *ops;
};

struct seq_operations {
	void * (*start) (struct seq_file *m, loff_t *pos);
	void (*stop) (struct seq_file *m, void *v);
	void * (*next) (struct seq_file *m, void *v, loff_t *pos);
	int (*show) (struct seq_file *m, void *v);
};

void seq_printf(struct seq_file *m, const char *fmt, ...);
int seq_open(struct file *, const struct seq_operations *);
int seq_release(struct inode* inode, struct file* file);
ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos);
loff_t seq_lseek(struct file *file, loff_t offset, int whence);

struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent);
void  remove_proc_entry(const char *name, struct proc_dir_entry *parent);
struct proc_dir_entry *proc_create_data(const char *name, umode_t mode, struct proc_dir_entry *parent, const struct file_operations *proc_fops, void *data);
void procfs_traverse_tree_dfs(struct proc_dir_entry*dir, void*context, int (*fn)(struct proc_dir_entry*, void*));	// Daniel: Method for unitests. Traverse the proc sub tree and run the function on each entry.

// /linux/stat.h
#define S_IRWXU 00700
#define S_IRUSR 00400
#define S_IWUSR 00200
#define S_IXUSR 00100

#define S_IRWXG 00070
#define S_IRGRP 00040
#define S_IWGRP 00020
#define S_IXGRP 00010

#define S_IRWXO 00007
#define S_IROTH 00004
#define S_IWOTH 00002
#define S_IXOTH 00001
#define S_IRWXUGO	(S_IRWXU|S_IRWXG|S_IRWXO)
#define S_IALLUGO	(S_ISUID|S_ISGID|S_ISVTX|S_IRWXUGO)
#define S_IRUGO		(S_IRUSR|S_IRGRP|S_IROTH)
#define S_IWUGO		(S_IWUSR|S_IWGRP|S_IWOTH)
#define S_IXUGO		(S_IXUSR|S_IXGRP|S_IXOTH)

static inline void *PDE_DATA(const struct inode *i) {  return (void*)i; };	// Unsupported yet.
static inline struct inode *file_inode(struct file *f){return (void*)f; };	// Unsupported yet.

/************************** ??? Miscelenous ??? ******************************/
// #include <linux/utsname.h>
#define __NEW_UTS_LEN 64
struct new_utsname {
	char sysname[	__NEW_UTS_LEN + 1];
	char nodename[	__NEW_UTS_LEN + 1];
	char release[	__NEW_UTS_LEN + 1];
	char version[	__NEW_UTS_LEN + 1];
	char machine[	__NEW_UTS_LEN + 1];
	char domainname[__NEW_UTS_LEN + 1];
};
const struct new_utsname *utsname(void);

// /capability.h
#define CAP_CHOWN            0
#define CAP_DAC_OVERRIDE     1
#define CAP_DAC_READ_SEARCH  2
#define CAP_FOWNER           3
#define CAP_FSETID           4
#define CAP_KILL             5
#define CAP_SETGID           6
#define CAP_SETUID           7
#define CAP_SETPCAP          8
#define CAP_LINUX_IMMUTABLE  9
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_BROADCAST    11
#define CAP_NET_ADMIN        12
#define CAP_NET_RAW          13
#define CAP_IPC_LOCK         14
#define CAP_IPC_OWNER        15
#define CAP_SYS_MODULE       16
#define CAP_SYS_RAWIO        17
#define CAP_SYS_CHROOT       18
#define CAP_SYS_PTRACE       19
#define CAP_SYS_PACCT        20
#define CAP_SYS_ADMIN        21
#define CAP_SYS_BOOT         22
#define CAP_SYS_NICE         23
#define CAP_SYS_RESOURCE     24
#define CAP_SYS_TIME         25
#define CAP_SYS_TTY_CONFIG   26
#define CAP_MKNOD            27
#define CAP_LEASE            28
#define CAP_AUDIT_WRITE      29
#define CAP_AUDIT_CONTROL    30
#define CAP_SETFCAP	     31
#define CAP_MAC_OVERRIDE     32
#define CAP_MAC_ADMIN        33
#define CAP_SYSLOG           34
#define CAP_WAKE_ALARM            35
#define CAP_BLOCK_SUSPEND    36
#define CAP_LAST_CAP         CAP_BLOCK_SUSPEND
#define cap_valid(x) ((x) >= 0 && (x) <= CAP_LAST_CAP)
#define CAP_TO_INDEX(x)     ((x) >> 5)        /* 1 << 5 == bits in __u32 */
#define CAP_TO_MASK(x)      (1 << ((x) & 31)) /* mask for indexed __u32 */
static inline bool capable(int cap) {(void)cap; return true; }

#include "common/compat/rdma_incs.h"

/****************************** Linux Drivers ********************************/
// linux/module.h
struct module{ atomic_t refcnt; };
#define THIS_MODULE ((struct module *)0)		// Daniel: Todo, create a real static module with correct refcount
static inline int try_module_get(struct module *m) { if (m) atomic_inc(&m->refcnt); return true;}
static inline void __module_get( struct module *m) { if (m) atomic_inc(&m->refcnt); }
static inline void   module_put( struct module *m) { if (m) atomic_dec(&m->refcnt); }

#define module_param_array_named(name, value, type, len_ptr, perm)
#define __init
#define __exit
#define __iomem

// linux/init.h
typedef int  (*initcall_t)(void);
typedef void (*exitcall_t)(void);

// asm/i387.h
struct irq_poll { struct list_head list; unsigned long state; int weight; }; // Daniel: removed irq_poll_fn *poll;
typedef int (irq_poll_fn)(struct irq_poll *, int);

#define module_init(initfn) 		static inline int  __attribute__((__unused__)) __inittest(void){ return initfn(); } int   insmod_##initfn(void) __attribute__((alias(#initfn)));
#define module_exit(exitfn)			static inline void __attribute__((__unused__)) __exittest(void){ 		exitfn(); } void  rm_mod_##exitfn(void) __attribute__((alias(#exitfn)));

// linux/profile.h
enum profile_type { PROFILE_TASK_EXIT = 0 };
struct notifier_block;

typedef	int (*notifier_fn_t)(struct notifier_block *nb,
			unsigned long action, void *data);

struct notifier_block {
	notifier_fn_t notifier_call;
	struct notifier_block *next;
	int priority;
};

int profile_event_register(enum profile_type type, struct notifier_block *n);
int profile_event_unregister(enum profile_type type, struct notifier_block *n);

enum nvmeib_cnt_mem_type;
struct nvmeib_rdma_iu;
struct nvmeib_stack_trace;
struct nvmeib_public_kth_ft;

typedef int (*prio_list_cmp_fn)(void *priv, struct list_head *a, struct list_head *b);
/* Add to tail of priority group in list */
void prio_list_add_tail(struct list_head *new, struct list_head *head, prio_list_cmp_fn cmp_fn, void *priv);

struct pci_dev {
	struct device dev;
};

int local_nic_prio_cmp_fn(void *priv, struct list_head *a, struct list_head *b);
#define	to_pci_dev(n) container_of(n, struct pci_dev, dev)

//linux/kobject.h
enum kobject_action {
	KOBJ_ADD,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_MOVE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
	KOBJ_BIND,
	KOBJ_UNBIND,
};

#define disk_to_dev(disk)  ((disk)->part0.__dev)

#define MODULE_NAME_LEN	32

struct net_device {
};

#define MODULE_INFO(tag, info)
#define DISK_MAX_PARTS 256

typedef unsigned long long time64_t;

struct hrtimer {};
enum hrtimer_mode {NOT_AN_EMPTY_ENUM = 0};

#define WQ_UNBOUND 0
#define WQ_HIGHPRI 0
#define WQ_MEM_RECLAIM 0
#define WQ_SYSFS 0
#define WQ_UNBOUND_MAX_ACTIVE 0

int profile_event_register(enum profile_type type, struct notifier_block *n);
int profile_event_unregister(enum profile_type type, struct notifier_block *n);

//#define NVME_IOCTL_ID			(0x4e40)
#endif // #ifndef KR_INCS_H
/*****************************************************************************/
#include "kr_sim_svc.h"	// emulation of additional features/hooks into the emulated kernel (not in read kernel)
// EOF.
