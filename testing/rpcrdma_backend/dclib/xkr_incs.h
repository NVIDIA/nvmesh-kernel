/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef XKR_INCS_H
#define XKR_INCS_H
#include <generated/autoconf.h>

#undef CONFIG_FSL_ERRATUM_A008585
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/hrtimer.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/seq_file.h>
#include <linux/random.h>
#include <linux/wait.h>
#include <linux/delay.h>
#include <linux/vmalloc.h>
#include <linux/atomic.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/interrupt.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/proc_fs.h>
#include <linux/configfs.h>
#include <linux/utsname.h>
#include <linux/time.h>
#include <linux/moduleparam.h>
#include <linux/rtc.h>
#include <linux/uuid.h>
#include <linux/sort.h>
#include <linux/crc32.h>
#include <linux/crc32c.h>
#include <linux/kallsyms.h>
#include <linux/clocksource.h>
#include <linux/completion.h>
#include <linux/byteorder/generic.h>
#include <linux/kref.h>
#include <linux/log2.h>
#if KS_HAS_SCHED_SIGNAL_HEADER
#include <linux/sched/signal.h>
#endif
#if KS_HAS_SCHED_TASK_HEADER
#include <linux/sched/task.h>
#endif
#include <linux/ratelimit.h>
#include <linux/efi.h>
#include <linux/interval_tree_generic.h>
#include <linux/numa.h>
#include <linux/stacktrace.h>

#if KS_HAS_IRQ_POLL
#include <linux/irq_poll.h>
#else
struct irq_poll;
typedef int (irq_poll_fn)(struct irq_poll *, int);

struct irq_poll {
	struct list_head list;
	unsigned long state;
	int weight;
	irq_poll_fn *poll;
};
#endif

#include "kr_version.h"
#include "../common/compat/kr_incs_types.h"


#if 0 // YR: This can be useful for debugging compilation issues
/* definition to expand macro then apply to pragma message */
#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)
#define VAR_NAME_VALUE(var) #var "="  VALUE(var)
// Example: #pragma message(VAR_NAME_VALUE(WHAT_I_WANT_TO_PRINT_DURING_COMPILE))
#endif


//#if KS_INCLUDE_SCSI_REQUEST
//#include <scsi/scsi_request.h>
//#endif


#if KS_HASHTABLE
#include <linux/hashtable.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(3, 9, 0))
	#define __hash_for_each_safe__(_name_, _bkt_, __node__, _tmp_, _obj_, _member_)	\
		__node__ = NULL;	\
		hash_for_each_safe(_name_, _bkt_, _tmp_, _obj_, _member_)

	#define __hash_for_each_possible_safe__(_name_, _obj_, __node__, _tmp_, _member_, _key_)	\
		__node__ = NULL;	\
		hash_for_each_possible_safe(_name_, _obj_, _tmp_, _member_, _key_)

#else /* LINUX_VERSION_CODE < KERNEL_VERSION(3, 9, 0) */
	#define __hash_for_each_safe__(_name_, _bkt_, __node__, _tmp_, _obj_, _member_)	\
		hash_for_each_safe(_name_, _bkt_, __node__, _tmp_, _obj_, _member_)

	#define __hash_for_each_possible_safe__(_name_, _obj_, __node__, _tmp_, _member_, _key_)	\
		hash_for_each_possible_safe(_name_, _obj_, __node__, _tmp_, _member_, _key_)
#endif

#define __hlist_for_each_entry__(pos, head, member)				\
		hlist_for_each_entry(pos, head, member)

#include "common/compat/kr_incs_bit_ops.h"
#else
#include "common/compat/kr_incs_hash_table.h"
#endif

#include <linux/smp.h>
#include <linux/list_sort.h>
#include <linux/kthread.h>
#include <linux/pci.h>
#include <linux/workqueue.h>

#if KS_DMA_DIRECTION
#	include <linux/dma-direction.h>
#else
#	include <linux/dma-mapping.h>
#endif

#if !KS_REINIT_COMPLETION
/**
 * reinit_completion - reinitialize a completion structure
 * @x:  pointer to completion structure that is to be reinitialized
 *
 * This inline function should be used to reinitialize a completion structure so it can
 * be reused. This is especially important after complete_all() is used.
 */
static inline void x_reinit_completion(struct completion *x)
{
	x->done = 0;
}

#else

#	define x_reinit_completion reinit_completion

#endif

#if !KS_LIST_LAST_ENTRY
/**
 * list_last_entry - get the last element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 *
 * Note, that list is expected to be not empty.
 */
#define list_last_entry(ptr, type, member) \
	list_entry((ptr)->prev, type, member)

#endif

#if !KS_LIST_FIRST_ENTRY_OR_NULL
/**
 * list_first_entry_or_null - get the first element from a list
 * @ptr:	the list head to take the element from.
 * @type:	the type of the struct this is embedded in.
 * @member:	the name of the list_struct within the struct.
 *
 * Note that if the list is empty, it returns NULL.
 */
#define list_first_entry_or_null(ptr, type, member) \
	(!list_empty(ptr) ? list_first_entry(ptr, type, member) : NULL)

#endif

#if !KS_LIST_PREV_NEXT
/**
 * list_next_entry - get the next element in list
 * @pos:        the type * to cursor
 * @member:     the name of the list_struct within the struct.
 */
#define list_next_entry(pos, member) \
        list_entry((pos)->member.next, typeof(*(pos)), member)

/**
 * list_prev_entry - get the prev element in list
 * @pos:        the type * to cursor
 * @member:     the name of the list_struct within the struct.
 */
#define list_prev_entry(pos, member) \
        list_entry((pos)->member.prev, typeof(*(pos)), member)
#endif

/* Atomic operations on per cpu variables. In user space this requires lock */
#ifdef __x86_64__
#define atomic_inc_per_cpu_volatile_int(i)	asm volatile ("incl %0" : "+m"(i))
#define atomic_dec_per_cpu_volatile_int(i)	asm volatile ("decl %0" : "+m"(i))
#else
#define atomic_inc_per_cpu_volatile_int(i)	{ unsigned long flags; local_irq_save(flags); ++(i); local_irq_restore(flags); }
#define atomic_dec_per_cpu_volatile_int(i)	{ unsigned long flags; local_irq_save(flags); --(i); local_irq_restore(flags); }
#endif // 0

#ifndef U32_MAX
#	define U32_MAX ((u32)~0U)
#endif

#ifndef BUILD_BUG_ON_MSG
#	define BUILD_BUG_ON_MSG(cond, msg) do { ((void)sizeof(char[1 - 2 * cond])); } while (0)
#endif

#if !KS_HAS_KREF_READ
		static inline unsigned int kref_read(const struct kref *kref)
		{
#	if KS_KREF_USES_REFCOUNT
			return refcount_read(&kref->refcount);
#	else
			return atomic_read(&kref->refcount);
#	endif
		}
#endif

#if !KS_HAS_ATOMIC_DEC_IF_POSITIVE
static inline int atomic_dec_if_positive(atomic_t *v)
{
	int c, old, dec;
	c = atomic_read(v);
	for (;;) {
		dec = c - 1;
		if (unlikely(dec < 0))
			break;
		old = atomic_cmpxchg((v), c, dec);
		if (likely(old == c))
			break;
		c = old;
	}
	return dec;
}
#endif

#if KS_NEW_TIMER_API
#	define INIT_TIMER(x) __init_timer((x), 0, 0)

#	define TIMER_CALLBACK_DECL(func_name) \
static void func_name(struct timer_list* _tl);

#	define TIMER_CALLBACK(func_name, container_type, container_field, data_type, instance)	\
void func_name(struct timer_list* _tl)				\
{								\
data_type * instance = (data_type*)container_of(_tl, container_type, container_field)->container_field ## _data;

#	define TIMER_LIST_INSTANCE(container_field)			\
struct {							\
	struct timer_list	container_field;		\
	unsigned long		container_field ## _data;	\
};

#	define TIMER_SET_DATA(container, container_field, data)	\
do { 								\
	container->container_field ## _data = data;		\
} while (0)


#	define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
        do {                                                    \
                __init_timer((_timer), (_fn), (_flags));        \
                (_timer)->function = (_fn);                     \
        } while (0)
#else

#	define INIT_TIMER(x) init_timer(x)

#	define TIMER_CALLBACK_DECL(func_name) \
static void func_name(unsigned long _data);

#	define TIMER_CALLBACK(func_name, container_type, container_field, data_type, instance)	\
void func_name(unsigned long _data)				\
{								\
data_type * instance = (data_type*)_data;


#	define TIMER_LIST_INSTANCE(container_field)	\
struct timer_list	container_field;


#	define TIMER_SET_DATA(container, container_field, _data)	\
do { 								\
	container->container_field.data = _data;		\
} while (0)


#	if KS_INIT_TIMER_KEY_HAS_FLAGS
#		define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
        do {                                                    \
                init_timer_key((_timer), (_flags), NULL, NULL); \
                (_timer)->function = (_fn);                     \
                (_timer)->data = (_data);                       \
        } while (0)
#	else
#		define SETUP_TIMER(_timer, _fn, _data, _flags)                 \
        do {                                                    \
                init_timer_key((_timer), NULL, NULL); 			\
                (_timer)->function = (_fn);                     \
                (_timer)->data = (_data);                       \
        } while (0)
#	endif

#endif

#endif
