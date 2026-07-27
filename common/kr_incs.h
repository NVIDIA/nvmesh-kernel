#if defined(__KERNEL__)
	// See the rest of the file below
#elif defined(TOMA)
	#ifndef KR_INCS_H
		#define KR_INCS_H	/* Avoid including rest of the file */
	#endif
	#include "common/compat/kr_incs_types.h"
#elif defined(BLKDEV_SIMULATOR)
	#include "block/unitest/nvmeib_common_all.h"		// Injection of simulator
#elif defined(DP_LIB) /* UM app and other future userspace apps, using dplib */
	#include "clnt/block/um_integration/nvmesh_dp_lib_depend.h"
#elif defined(UM_APP) /* UM app including kernel code, not via dplib */
	#include "common/compat/kr_incs_um_framework.h"
	#ifndef KR_INCS_H
		#define KR_INCS_H	/* Avoid including rest of the file */
	#endif
#else
	#include "perfTest/io_stress/cmp_blocks/nvmesh_util_kernel_emulation.h"	/* pager, cmp_blocks, di_parser, etc... */
#endif

#ifndef KR_INCS_H
#define KR_INCS_H

#ifndef __DISABLE_TRACE_MMIO__
#define __DISABLE_TRACE_MMIO__	/* See: include/asm-generic/io.h */
#endif
#include <generated/autoconf.h>

#include "compat/kr_incs_percpu.h"
#include "compat/kr_incs_llist_compat.h"

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
#include <linux/crc16.h>
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
#include <linux/profile.h>
#include <linux/stddef.h>
#include <uapi/linux/time.h>
#include <asm-generic/getorder.h>
#include <linux/cpumask.h>
#include <linux/percpu.h>
#include <linux/rwlock.h>
#include <linux/seqlock.h>
#include <linux/bitmap.h>
#if KS_HAS_GENHD_H
	#include <linux/genhd.h>
#else
	#define GENHD_FL_EXT_DEVT 0 // This is not a limit anymore in later kernels
#endif

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

enum {
	IRQ_POLL_F_SCHED	= 0,
	IRQ_POLL_F_DISABLE	= 1,
};

#endif

#include "nvmeib_nvme.h"
#include "nvmeib_shared.h"
#include "kr_version.h"
#include "./compat/kr_incs_types.h"
#include "nvmeib_error_report.h"

#if 0 // YR: This can be useful for debugging compilation issues
/* definition to expand macro then apply to pragma message */
#define VALUE_TO_STRING(x) #x
#define VALUE(x) VALUE_TO_STRING(x)
#define VAR_NAME_VALUE(var) #var "="  VALUE(var)
// Example: #pragma message(VAR_NAME_VALUE(WHAT_I_WANT_TO_PRINT_DURING_COMPILE))
#endif

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

	#include "./compat/kr_incs_bit_ops.h"
#else
	#include "./compat/kr_incs_hash_table.h"
#endif

#include <linux/smp.h>
#include <linux/list_sort.h>
#include <linux/kthread.h>
#include <linux/pci.h>

#define NVMEIB_WORKQ
#ifdef NVMEIB_WORKQ
#	include "nvmeib_q.h"
#else
#	include <linux/workqueue.h>
#endif

#if KS_DMA_DIRECTION
#	include <linux/dma-direction.h>
#else
#	include <linux/dma-mapping.h>
#endif

#if KS_BIO_BI_STATUS
#	define bio_error(bio) nvmeib_blk_status_to_errno(bio->bi_status)
#else
#	define bio_error(bio) (bio->bi_error)
#endif

#if !KS_BVEC_ITER
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
#endif //!KS_BVEC_ITER

#if !KS_BIO_SET_DEV
#	define bio_set_dev(bio, dev) bio->bi_bdev = dev;
#endif

#if KS_ENDIO_1ARG
#if KS_BIO_BI_STATUS
	#define bio_endio(bio, errno) 			\
	do {					\
		bio->bi_status = nvmeib_errno_to_blk_status(errno);	\
		bio_endio(bio);			\
	} while (0)
#else
	#define bio_endio(bio, errno) 			\
	do {					\
		bio->bi_error = (errno);	\
		bio_endio(bio);			\
	} while (0)
#endif
#endif

#if !KS_REINIT_COMPLETION
/**
 * reinit_completion - reinitialize a completion structure
 * @x:  pointer to completion structure that is to be reinitialized
 *
 * This inline function should be used to reinitialize a completion structure so it can
 * be reused. This is especially important after complete_all() is used.
 */
static inline void nvmeib_reinit_completion(struct completion *x)
{
	x->done = 0;
}

#else

#	define nvmeib_reinit_completion reinit_completion

#endif

#ifdef REQ_OP_BITS
#if KS_HAS_BIO_SET_OP_ATTRS
	#define __SET_BI_RW(b, rw) bio_set_op_attrs(b, rw, 0);
	#define BIO_CLONE_FAST_SET_BI_RW(dst, src) do {} while(0);
#else
	#define __SET_BI_RW(b, rw) ({ (b)->bi_opf = (((b)->bi_opf & ~REQ_OP_MASK) | rw); })
	#define BIO_CLONE_FAST_SET_BI_RW(dst, src) do {} while(0);
#endif
#else
	#define __SET_BI_RW(b, rw) ({ (b)->bi_rw = rw; })			// Todo: Clean lowest bit and use: b->bi_rw |= rw ?
	#define BIO_CLONE_FAST_SET_BI_RW(dst, src) __SET_BI_RW((dst), (src)->bi_rw);
#endif

#include "nvmeib_common_os_block_api.h"

#if KS_BIO_HAS_BI_OPF
	#define BIO_CLONE_FAST_OPF(dst, src) ((dst)->bi_opf) = ((src)->bi_opf);
#else
	#define BIO_CLONE_FAST_OPF(dst, src) do {} while(0);
#endif

#if KS_BIO_HAS_BI_WRITE_HINT
	#define BIO_CLONE_FAST_WRITE_HINT(dst, src) ((dst)->bi_write_hint) = ((src)->bi_write_hint);
#else
	#define BIO_CLONE_FAST_WRITE_HINT(dst, src) ((dst)->bi_flags)= ((src)->bi_flags);
#endif


// Clone relevant fields (but not the actual bvec's so do not use kernel clone)
#define __BIO_CLONE_FAST(dst, src)						\
do {													\
	BIO_CLONE_FAST_DISK_OR_BDEV(dst, src);				\
	BIO_CLONE_FAST_OPF(dst, src);						\
	BIO_CLONE_FAST_WRITE_HINT(dst, src);				\
	BIO_CLONE_FAST_SET_BI_RW(dst, src); 				\
} while (0);

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


#ifdef NVMEIB_WORKQ
#	define workq_func_t nvmeib_qfunc
#	define WQ_INIT_WORK(_work, _func) \
	do { \
		INIT_LIST_HEAD(&(_work)->link); \
		(_work)->f = (_func); \
	} while (false)
#	define workqe_struct nvmeib_qent
#	define workq_struct nvmeib_q
#	define wq_create(name) nvmeib_startq(name, NR_CPUS)
#	define wq_create_on(name, cpu) nvmeib_startq(name, cpu)
#	define workqe_struct_init {{0}}

#ifdef DEBUG_NVMEIB_Q
/* __builtin_return_address can cause a segfault on GCC 4.7 or less if the stack isn't that many levels deep */
#if (__GNUC__ == 4 && __GNUC_MINOR__ > 8)
#		define wq_add_work(wq,w) nvmeib_addq(wq,w,\
			__builtin_return_address(0),\
			__builtin_return_address(1),\
			__builtin_return_address(2))
#else
#		define wq_add_work(wq,w) nvmeib_addq(wq,w, NULL, NULL, NULL)
#endif
#else
#	define wq_add_work(wq,w) nvmeib_addq(wq,w)
#endif

#	define wq_cancel_work	nvmeib_cancel_qent
#	define wq_flush_work nvmeib_flush_qent

#	define wq_flush nvmeib_flushq
#	define wq_drain nvmeib_drainq
#	define wq_destroy nvmeib_endq
#	define wq_pid	nvmeib_qpid
static inline struct nvmeib_q *wq_create_on_verbose(const char *name, int cpu) {
	struct nvmeib_q *q = wq_create_on(name, cpu);
	if (q)
		nvmeib_q_set_log_level(q, NVMEIB_Q_LOG_LEVEL_VERBOSE);
	return q;
}
#define wq_create_verbose(name) wq_create_on_verbose(name, NR_CPUS)
#else
#	define workq_func_t work_func_t
#	define WQ_INIT_WORK INIT_WORK
#	define workqe_struct work_struct
#	define workqe_struct_init {{0}}
#	define workq_struct workqueue_struct
#	define wq_create create_singlethread_workqueue
#	define wq_add_work queue_work

#	define wq_cancel_work(q, w) do {\
		(void)q;\
		cancel_work_sync(w);\
	} while(0)

#	define wq_flush_work(q, w) do {\
		(void)q;\
		flush_work(w);\
	} while(0)

#	define wq_flush flush_workqueue
#	define wq_drain drain_workqueue
#	define wq_destroy destroy_workqueue
#	define wq_pid(wq) (-1)
#   define wq_create_verbose wq_create
#endif
#define schedule_work_on_sys_wq_rand_cpu schedule_work

#define nvmeib_schedule_work_on(cpu_id, work) nvmeib_pcpu_wq_add_work_on_core(nvmeib_get_system_wq(), (cpu_id), (work))

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

#ifndef KS_NVME_DPTR
#       ifdef NVME_DISC_SUBSYS_NAME
#               define KS_NVME_DPTR 1
#       else
#               define KS_NVME_DPTR 0
#       endif
#endif
#if KS_NVME_DPTR
#define dptr_prp1 dptr.prp1
#define dptr_prp2 dptr.prp2
#else
#define dptr_prp1 prp1
#define dptr_prp2 prp2
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

#include "compat/kr_incs_crc32.h"	// Implementation: client - in gf file, server - in serjio file. Grep for the .inc.c include

#define nvmeib_current() current
#if !KS_HAS_MUTEX_OWNER

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 10, 0))
#define NVMEIB_MUTEX_FLAGS 0x07
static inline struct task_struct *__mutex_owner(struct mutex *lock)
{
	return (struct task_struct *)(atomic64_read(&lock->owner) & ~NVMEIB_MUTEX_FLAGS);
}
#elif (LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 39))
#define __mutex_owner(l) ((l)->owner)
#else
	//dont reference speculative pointers "owner" and "owner->task"
	//return (mutex_is_locked(l) && l->owner->task->pid == current->pid);
	//return false;
#define __mutex_owner(l) ((l)->owner)
#undef nvmeib_current()
#define nvmeib_current() current_thread_info()
#endif

#endif

#include "compat/kr_incs_time.h"

#if !(KS_HAS_ATOMIC_INC_NOT_ZERO_HINT)
#define atomic_inc_not_zero_hint(v, hint) atomic_inc_not_zero(v)
#endif

#if defined(QUEUE_FLAG_DEFAULT)
#define NVMESH_QUEUE_FLAG_DEFAULT QUEUE_FLAG_DEFAULT
#else
#define NVMESH_QUEUE_FLAG_DEFAULT QUEUE_FLAG_MQ_DEFAULT
#endif

// A good-enough replacement for split_page(), which we cannot use
static inline void nvmeib_split_page(struct page *page, unsigned int order)
{
	int i;
	for (i = 1; i < (1 << order); i++)
		init_page_count(page + i);	// HACK: init_ just calls set_, which is what we would have wanted to use, but it is not exposed in some kernel versions
}

/* Implementation of Priority List ie List where items have a priority group number that is respected when doing list_add, list_add_tail, list_rotate.
 * Items where cmp_fn(a,b) returns > 0 means that "a group" will be before "b group" in the list */

/* Comparison Function - Same rv syntax as strcmp */
typedef int (*prio_list_cmp_fn)(void *priv, struct list_head *a, struct list_head *b);

/* Add to tail of priority group in list */
static inline void prio_list_add_tail(struct list_head *new, struct list_head *head, prio_list_cmp_fn cmp_fn, void *priv)
{
	struct list_head *iter = head;

	/* Shortcuts 1: List is empty so can add directly to tail */
	if (list_empty(head))
		goto add;

	/* Shortcuts 2: Tail of list has same priority or less than new so we can directly to the tail */
	if ((*cmp_fn)(priv, head->prev, new) <= 0)
		goto add;

	/* Loop over list looking for place to insert new */
	list_for_each(iter, head) {
		if ((*cmp_fn)(priv, iter, new) > 0)
			break;
	}

add:
	/* iter will either be the first entry where the cmp_fn returns > 0 or the head of the list */
	list_add_tail(new, iter);
}

/* Add to head of priority group in list */
static inline void prio_list_add(struct list_head *new, struct list_head *head, prio_list_cmp_fn cmp_fn, void *priv)
{
	struct list_head *iter = head;

	/* Shortcuts 1: List is empty so can add directly to head */
	if (list_empty(head))
		goto add;

	/* Shortcuts 2: Head of list has same priority or less than new so we can directly to the tail */
	if ((*cmp_fn)(priv, head->next, new) <= 0)
		goto add;

	/* Loop over list looking for place to insert new */
	list_for_each(iter, head) {
		if ((*cmp_fn)(priv, iter, new) > 0)
			break;
	}

add:
	/* iter will either be the first entry where the cmp_fn returns > 0 or the head of the list */
	list_add(new, iter);
}

/* Rotate priority groups in list */
static inline void prio_list_rotate_n(struct list_head *head, unsigned n_rotate, prio_list_cmp_fn cmp_fn, void *priv)
{
	struct list_head *link_start_prio, *link_end_prio, *link_iter;
	unsigned prio_shuffle_n;
	unsigned i = 0;
	for (link_start_prio = head->next; link_start_prio != head; link_start_prio = link_end_prio) {
		unsigned n_prio = 0;
		list_for_each(link_iter, link_start_prio) {
			n_prio++;
			if (link_iter == head || (*cmp_fn)(priv, link_start_prio, link_iter) != 0)
				break;
		}
		link_end_prio = link_iter;
		/* Rotate priority group */
		prio_shuffle_n = n_rotate % n_prio; /* Note if n_prio <= 1, then prio_shuffle_n will be 0 */
		for (i = 0; i < prio_shuffle_n; ++i) {
			link_iter = link_start_prio;
			link_start_prio = link_start_prio->next;
			list_del(link_iter);
			list_add_tail(link_iter, link_end_prio);
		}
	}
}

#if KS_HAS_MMAP_LOCK_FUNCTIONS
#include <linux/mmap_lock.h>
#else
#define mmap_read_lock(mm) down_read(&mm->mmap_sem)
#define mmap_read_unlock(mm) up_read(&mm->mmap_sem)
#endif

#if KS_HAS_REVALIDATE_DISK_SIZE && !KS_BLOCK_DEV_OPS_HAS_REVALIDATE_DISK
#define revalidate_disk(_disk) revalidate_disk_size(_disk, false);
#endif // KS_HAS_REVALIDATE_DISK_SIZE

#ifndef fallthrough
# define fallthrough                    do {} while (0)  /* fallthrough */
#endif

#if KS_BDI_PTR_IN_QUEUE
#	define gendisk_to_bdi(_gendisk) (_gendisk)->queue->backing_dev_info
#elif KS_BDI_IN_QUEUE
#	define gendisk_to_bdi(_gendisk) (&((_gendisk)->queue->backing_dev_info))
#else
#	define gendisk_to_bdi(_gendisk) (_gendisk)->bdi
#endif
#define gendisk_to_device(_gendisk) gendisk_to_bdi(_gendisk)->dev

#if !KS_SMP_CALL_SINGLE_DATA_T
typedef struct call_single_data call_single_data_t;
#endif

#if !KS_HAS_SGL_ALLOC_ORDER
static inline void sgl_free_order(struct scatterlist *sgl, int order)
{
	struct scatterlist *sg;
	struct page *page;
	int i;

	for_each_sg(sgl, sg, INT_MAX, i) {
		if (!sg)
			break;
		page = sg_page(sg);
		if (page)
			__free_pages(page, order);
	}
	kfree(sgl);
}

static inline struct scatterlist *sgl_alloc_order(unsigned long long length,
				    unsigned int order, bool chainable,
				    gfp_t gfp, unsigned int *nent_p)
{
	struct scatterlist *sgl, *sg;
	struct page *page;
	unsigned int nent, nalloc;
	u32 elem_len;

	nent = round_up(length, PAGE_SIZE << order) >> (PAGE_SHIFT + order);
	/* Check for integer overflow */
	if (length > (nent << (PAGE_SHIFT + order)))
		return NULL;
	nalloc = nent;
	if (chainable) {
		/* Check for integer overflow */
		if (nalloc + 1 < nalloc)
			return NULL;
		nalloc++;
	}
	sgl = kmalloc_array(nalloc, sizeof(struct scatterlist),
			    (gfp & ~GFP_DMA) | __GFP_ZERO);
	if (!sgl)
		return NULL;

	sg_init_table(sgl, nalloc);
	sg = sgl;
	while (length) {
		elem_len = min_t(u64, length, PAGE_SIZE << order);
		page = alloc_pages(gfp, order);
		if (!page) {
			sgl_free_order(sgl, order);
			return NULL;
		}

		sg_set_page(sg, page, elem_len, 0);
		length -= elem_len;
		sg = sg_next(sg);
	}
	WARN_ONCE(length, "length = %lld\n", length);
	if (nent_p)
		*nent_p = nent;
	return sgl;
}
#endif

#if !KS_HAS_SG_DMA_PAGE_ITER
#define sg_dma_page_iter 	sg_page_iter
#define for_each_sg_dma_page 	for_each_sg_page
#define sg_dma_page_iter_pg_offset(iter) iter.sg_pgoffset
#else
#define sg_dma_page_iter_pg_offset(iter) iter.base.sg_pgoffset
#endif

/* From kernel 5.15 GENHD_FL_NO_PART_SCAN renamed to GENHD_FL_NO_PART
   Also support older kernels without GENHD_FL_NO_PART_SCAN (and GENHD_FL_NO_PART) */
#ifndef GENHD_FL_NO_PART
#	ifndef GENHD_FL_NO_PART_SCAN
#		define GENHD_FL_NO_PART_SCAN 0
#	endif
#	define GENHD_FL_NO_PART GENHD_FL_NO_PART_SCAN
#endif

#endif //ifndef KR_INCS_H
