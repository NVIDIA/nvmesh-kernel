/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "linux/mm_types.h"
#include "nvmeib.h"
#include <linux/kallsyms.h>
#include <linux/random.h>
#include "nvmeib_shared.h"
#include "nvmeib_str.h"
#include "../utils/nvmeib_jdr/nvmeib_jdr.h"
#if KS_TRACE_EVENTS
#	include <linux/trace_events.h>
#	define ftrace_event_file trace_event_file
#	define ftrace_event_field trace_event_field
#	define ftrace_event_call trace_event_call
#	define ftrace_event_reg trace_event_reg

#if KS_DUMMY_TRACE_REG
/********** Dummy implementation of include <linux/ftrace_event.h>*************/
enum trace_reg {
	TRACE_REG_REGISTER, TRACE_REG_UNREGISTER
};
#endif // KS_DUMMY_TRACE_REG

#else
	#include <linux/ftrace_event.h>
#endif

#ifdef __KERNEL__
EXPORT_SYMBOL(jdr_make);
EXPORT_SYMBOL(jdr_finalize);
EXPORT_SYMBOL(jdr_write_key_value_str);
EXPORT_SYMBOL(jdr_make_seq);
#endif

#include "nvmeib_public.h"
#include "nvmeib_ib_driver.h"

#define nvmeib_debug_level nvmeib_public_debug_level
#include "nvmeib_utils.h"
#include "nvmeib_rdma.h"

#include "ib_incs.h"
#include "kth/nvmeib_public_kth.h"
#include "poll/nvmeib_public_intr_poll.h"
#include "nvmeib_kth_events.h"
#include "nvmeibp_trace.h"
#include "nvmeib_public_keeper.h"

#include <linux/console.h>
#include <linux/blkdev.h>
#include <linux/sched.h>
#include <linux/pid_namespace.h>
#include <linux/bio.h>
#include <linux/module.h>

#include <linux/sched.h>
#include <linux/mm.h>
#if KSRC_INCLUDE_SCHED_MM
#include <linux/sched/mm.h>
#endif

#include <linux/kgdb.h>
#include <linux/kprobes.h>

#if KS_HAS_I387_HEADER
	#include <asm/i387.h>
#else
	#include <asm/simd.h>
#endif

MODULE_AUTHOR("NVIDIA CORPORATION");
MODULE_DESCRIPTION("NVMeIB Public");
MODULE_LICENSE("GPL and additional rights");

/* In case kallsyms is not available, this parameter allows the Infiniband On-Demand-Paging state to be provided on module load */
enum {
	NVMEIB_ODP_UNKNOWN = -1,
	NVMEIB_ODP_DISABLED = 0,
	NVMEIB_ODP_ENABLED = 1
};

int tracer_nvmeibp_debug_level = 3;
module_param_named(tracer_debug_level, tracer_nvmeibp_debug_level, int, 0644);
MODULE_PARM_DESC(tracer_debug_level, "This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above.");
EXPORT_SYMBOL(tracer_nvmeibp_debug_level);

static int nvmeib_odp_info = NVMEIB_ODP_UNKNOWN;
module_param_named(ib_odp_info, nvmeib_odp_info, int, 0644);
#if !KS_KALLSYMS_LOOKUP
MODULE_PARM_DESC(ib_odp_info, "Defines whether On-Demand-Paging is enabled for RDMA usage and NVMesh should use it. 1 = Enabled, 0 = Disabled, -1 = Auto. On-demand-paging enables using RDMA on non-pinned memory pages.");
#else
MODULE_PARM_DESC(ib_odp_info, "Defines whether On-Demand-Paging is enabled for RDMA usage and NVMesh should use it. 1 = Enabled, 0 = Disabled, -1 = Auto. On-demand-paging enables using RDMA on non-pinned memory pages.");
#endif

static bool nvmeibp_do_kasan_test = false;

#ifdef CONFIG_KASAN
module_param_named(do_kasan_test, nvmeibp_do_kasan_test, bool, 0444);
MODULE_PARM_DESC(do_kasan_test, "When set, run a test KASAN on module load. Results will be found in the system log, i.e., via dmesg.");
#endif

int *nvmeib_public_panic_on_warn = NULL;
EXPORT_SYMBOL(nvmeib_public_panic_on_warn);

static bool paging_enabled = false;

#define DEBUG_LEVEL (int)1
static int (*debug_level_f)(void);

int nvmeib_public_debug_level(void) {
	return debug_level_f ? debug_level_f() : DEBUG_LEVEL;
}

static void set_public_debug_level_cb(struct nvmeib_device_public_ops *pops, void *param) {
	int (*dlf)(void) = param;
	if (pops && pops->set_debug_level) pops->set_debug_level(dlf);
}

LIST_HEAD(public_hwdevs);
static void ibdr_hwdev_pops_call_all(void (*cb)(struct nvmeib_device_public_ops *pops, void *param), void *param)
{
	struct nvmeib_public_hwdev *dev;

	list_for_each_entry(dev, &public_hwdevs, list) {
		if (dev->ops)
			(*cb)(dev->ops, param);
	}
}

void nvmeib_public_set_debug_level(int (*dlf)(void))
{
	debug_level_f = dlf;
	ibdr_hwdev_pops_call_all(set_public_debug_level_cb, dlf);
}
EXPORT_SYMBOL(nvmeib_public_set_debug_level);

struct list_head *nvmeib_public_get_hwdevs(void)
{
	return &public_hwdevs;
}
EXPORT_SYMBOL(nvmeib_public_get_hwdevs);

static void free_public_hwdevs(void )
{
	struct nvmeib_public_hwdev *dev, *tmp;

	list_for_each_entry_safe(dev, tmp, &public_hwdevs, list) {
		/* we trust the mlx5/4/siw_public modules to remove themselves
		   from the list when they exiting before us
		 */
		_NW_dmesg(free_public_hwdevs_unregister,
		 "nvmeib_dev_type=@INT didn't clear itself from public hwdevs list", dev->type);
		list_del(&dev->list);
		kfree(dev);
	}
}

int nvmeib_public_set_pops(enum nvmeib_dev_type type,
						   const char *hwdriver,
						   struct nvmeib_device_public_ops *pops,
						   struct nvmeib_device_public_ops *pops_odp) {
	int rv = 0;
	struct nvmeib_public_hwdev *hdwev;
	const char *mod_name = "NOT FOUND";

	if (!(hdwev = kzalloc(sizeof(*hdwev), GFP_KERNEL))) {
		rv = -ENOMEM;
		goto out;
	}

	hdwev->type = type;

#if KS_HAS_MODULE_MUTEX
	mutex_lock(&module_mutex);
	hdwev->m = find_module(hwdriver);
	mutex_unlock(&module_mutex);
	if (!hdwev->m) {
		_NE(error_nvmeib_public_nvmeib_public_set_pops, "find_module failed for module @HWDRIVER", hwdriver);
		rv = -ENOENT;
		kfree(hdwev);
		goto out;
	}
	mod_name = hdwev->m->name;
#else
	/* find_module is no longer available, must match by name */
	strlcpy(hdwev->m, hwdriver, sizeof(hdwev->m));
	mod_name = hdwev->m;
#endif

	hdwev->ops = paging_enabled? pops_odp : pops;
	list_add(&hdwev->list, &public_hwdevs);

	_NI(trace_nvmeib_public_nvmeib_public_set_pops, "Registered pops module @MODULE_NAME for device type '@HWDRIVER' module: @M_NAME (@MP)",
	   (paging_enabled ? pops_odp->module->name : pops->module->name),
	    hwdriver, mod_name, hdwev->m);
out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_set_pops);

int nvmeib_public_clear_pops(enum nvmeib_dev_type type) {
	int rv = 0;
	struct nvmeib_public_hwdev *dev, *tmp;

	list_for_each_entry_safe(dev, tmp, &public_hwdevs, list) {
		if(dev->type == type) {
			list_del(&dev->list);
			kfree(dev);
			goto out;
		}
	}
	rv = 1;
	_NE(error_nvmeib_public_nvmeib_public_clear_pops,
	 "nvmeib_dev_type=@INT wasn't found in public_hwdevs nvmeib_public_set_pops was called on it?", dev->type);
out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_clear_pops);

bool nvmeib_mlx_on_demand_paging(void) {
	return paging_enabled;
}
EXPORT_SYMBOL(nvmeib_mlx_on_demand_paging);

static void __print_hooray(bool is_start, const char* mod_name)
{	/* Hooray */
	const char *ur = ((is_start) ? "registered" : "unregistered");		// Consider up/down
	struct timeval time;
	unsigned long local_time;
	struct rtc_time tm;
	do_gettimeofday(&time);
	local_time = (u32)(time.tv_sec - (sys_tz.tz_minuteswest * 60));
	rtc_time_to_tm(local_time, &tm);

	_NI_to_user(nvmeib_common_public_hooray,
			 "module", "Module @STR. Module: @STR. "
					   "Timestamp: @TM_YEAR-@TM_MON-@TM_MDAY @TM_HOUR:@TM_MIN:@TM_SEC. "
					   "Internal version for support cases: @COMMIT_ID_LONG",
			 ur, mod_name,
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
			 (unsigned long)COMMIT_ID); // Hooray //. Error code: 0
}

#if !NVMESH_IS_PRODUCTION_COMPILATION
#include <linux/kprobes.h>
static void _set_panic_on_warn(void) {
	static struct kprobe kp = {.symbol_name = "panic_on_warn"};

	register_kprobe(&kp);
	nvmeib_public_panic_on_warn = (int *) kp.addr;
	if (!nvmeib_public_panic_on_warn) {
		printk(KERN_INFO "Could not find panic_on_warn symbols");
	}
	unregister_kprobe(&kp);
}
#endif

static int __init nvmeib_public_module_init(void) /* Constructor */
{
	struct nvmeib_tracer_public_api t_api = {
		.sym_resolve_kernel_bug_can_happen = nvmeib_sym_resolve_kernel_bug_can_happen
	};
	int rv = -1;
	extern nvmeib_public_save_stack_trace_t nvmeib_public_save_stack_trace_ptr;

	if (nvmeib_odp_info != NVMEIB_ODP_UNKNOWN) {
		if (nvmeib_odp_info != 0) paging_enabled = true;
		rv = 0;
	} else {
		printk(KERN_ERR "On-Demand Paging State is not provided. "
				"Must be supplied with parameter ib_odp_info!\n");
		rv = -EINVAL;
		goto out;
	}

	printk(KERN_INFO "On-Demand-Paging is %s\n",
		   paging_enabled ? "enabled" : "disabled");
	
	if (nvmeibp_do_kasan_test) {
		nvmeib_public_kasan_test();
	}

#if !NVMESH_IS_PRODUCTION_COMPILATION
	_set_panic_on_warn();
#endif


	nvmeib_public_kth_init_lib();
	nvmeib_reg_tracer_public_api(&t_api);

#if defined(IO_POLL_THREAD) && IO_POLL_THREAD
	nvmeib_public_intr_pollers_start();
#endif

	nvmeib_public_save_stack_trace_ptr = nvmeib_public_save_stack_trace;
	
	/* Init interface to nvmeib_keeper module */
	nvmeib_public_keeper_init();

	_NI_to_user(nvmeib_common_public_hooray_registered, "module", "Hooray: nvmeib_common_public registered");

out:
	return rv;
}

#if defined(IO_POLL_THREAD) && IO_POLL_THREAD
void nvmeib_public_intr_poller_fill_ft(struct nvmeib_intr_pollers_ft *ift) {
	struct nvmeib_intr_pollers_ft public_ift = {
		.init = nvmeib_public_intr_poll_init,
		.sched = nvmeib_public_intr_poll_sched,
		.complete = nvmeib_public_intr_poll_complete,
		.is_sched = nvmeib_public_intr_poll_is_sched,
		.disable = nvmeib_public_intr_poll_disable,
		.enable = nvmeib_public_intr_poll_enable,
	};
	*ift = public_ift;
}
EXPORT_SYMBOL(nvmeib_public_intr_poller_fill_ft);
#endif

void nvmeib_public_kth_fill_ft(struct nvmeib_public_kth_ft *ft) {
	struct nvmeib_public_kth_ft public_ft = {
		.f_current = nvmeib_public_kth_current,
		.f_get_name = nvmeib_public_kth_get_name,
		.f_e2s = nvmeib_public_kth_e2s,
		.f_add_event = nvmeib_public_kth_add_event,
		.f_wait_events = nvmeib_public_kth_wait_events,
		.f_wait_events_timeout = nvmeib_public_kth_wait_events_timeout,
		.f_event_free = __nvmeib_public_kth_event_free,
		.f_wait_resume = nvmeib_public_kth_wait_resume,
		.f_resume = nvmeib_public_kth_resume,
	};
	*ft = public_ft;
}
EXPORT_SYMBOL(nvmeib_public_kth_fill_ft);

static void __exit nvmeib_public_module_exit(void) /* Destructor */
{
	nvmeib_public_kth_end_lib();

#if defined(IO_POLL_THREAD) && IO_POLL_THREAD
	nvmeib_public_intr_pollers_stop();
#endif

	{
		extern nvmeib_public_save_stack_trace_t nvmeib_public_save_stack_trace_ptr;
		nvmeib_public_save_stack_trace_ptr = NULL;
	}
	free_public_hwdevs();
	
	/* Finish keeper interface */
	nvmeib_public_keeper_fini();

	__print_hooray(false, "nvmeib_common_public");
}

/* percpu convenience wrappers remain for type casting */

void nvmeib_public_uuid_gen(uuid_be *bu) {
#if KS_HAS_UUID_BE_GEN
	uuid_be_gen(bu);
#else
	uuid_gen((uuid_t *)bu);
#endif
}
EXPORT_SYMBOL(nvmeib_public_uuid_gen);

#ifndef for_each_console
	#define for_each_console(con) \
	for (con = console_drivers; con != NULL; con = con->next)
#endif

#if KS_HAS_DISK_PART_ITER
int nvmeib_public_call_for_each_disk_part(struct gendisk *disk, int (*f)(struct hd_struct *, void *), void *args)
{
	struct disk_part_iter piter;
	struct hd_struct *part;
	int rv = -ENOENT;
	
	disk_part_iter_init(&piter, disk, 0);
	while ((part = disk_part_iter_next(&piter)) &&
		!(rv = (*f)(part, args)));
	disk_part_iter_exit(&piter);
	return rv;
}
#else
int nvmeib_public_call_for_each_disk_part(struct gendisk *disk, int (*f)(struct block_device *, void *), void *args)
{
	struct block_device *part;
	unsigned long idx;
	int rv = -ENOENT;
	
	rcu_read_lock();
	xa_for_each_start(&disk->part_tbl, idx, part, 1) {
		if ((rv = (*f)(part, args)) < 0)
			break;
	}
	rcu_read_unlock();
	return rv;
}
#endif
EXPORT_SYMBOL(nvmeib_public_call_for_each_disk_part);

bool nvmeib_public_serial_console(void) {
	struct console *con;

	for_each_console(con)
	if ((con->flags & CON_ENABLED) && (con->flags & CON_CONSDEV)) if (strncmp(con->name, "ttyS", 4) == 0) return true;
	return false;
}
EXPORT_SYMBOL(nvmeib_public_serial_console);

int nvmeib_public_generic_post_send_atomic(struct ib_qp *ibqp,
										   struct nvmeib_send_wr *wr,
										   struct nvmeib_send_wr **bad_wr)
{
	IB_DECLARE_BAD_SEND_WR(bad_ib_wr_ptr) = NULL;
	int rv;

	NFIN;
	rv = ib_post_send(ibqp, nvmeib_send_wr_to_ib_ptr(*wr), &bad_ib_wr_ptr);
	if (rv) *bad_wr = nvmeib_send_wr_ptr_from_ib(bad_ib_wr_ptr);

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_generic_post_send_atomic);


/*
 * Must be called under rcu_read_lock().
 */
static struct task_struct *_find_task_by_pid_ns(
	pid_t nr, struct pid_namespace *ns)
{
#ifdef RCU_LOCKDEP_WARN
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			"find_task_by_pid_ns() needs rcu_read_lock() protection");
#else
	rcu_lockdep_assert(rcu_read_lock_held(),
			"find_task_by_pid_ns() needs rcu_read_lock() protection");
#endif
	return pid_task(find_pid_ns(nr, ns), PIDTYPE_PID);
}

static struct task_struct *_find_task_by_vpid(pid_t vnr)
{
	return _find_task_by_pid_ns(vnr, task_active_pid_ns(current));
}

struct mm_struct * nvmeib_public_get_process_mm(int pid)
{
	struct mm_struct *mm = NULL;
	struct task_struct *tsk;

	NFIN;
	if (pid) {
		rcu_read_lock();
		tsk = _find_task_by_vpid(pid);
		if (tsk) {
			get_task_struct(tsk);
			rcu_read_unlock();
			mm = get_task_mm(tsk);
			put_task_struct(tsk);
		}
		else
			rcu_read_unlock();
	}
	NFOUT;
	return mm;
}
EXPORT_SYMBOL(nvmeib_public_get_process_mm);

void nvmeib_public_put_process_mm(struct mm_struct *mm)
{
	mmput(mm);
}
EXPORT_SYMBOL(nvmeib_public_put_process_mm);

#if defined(__aarch64__)
/* Not accessible on ARM architecture */
void copy_to_user_page(struct vm_area_struct *vma, struct page *page,
		       unsigned long uaddr, void *dst, const void *src,
		       unsigned long len)
{
#ifdef CONFIG_SMP
	preempt_disable();
#endif
	memcpy(dst, src, len);
	//flush_ptrace_access(vma, page, uaddr, dst, len);
#ifdef CONFIG_SMP
	preempt_enable();
#endif
}
#endif

int nvmeib_public_copy_user_pages(
	void *buf, int pid, void *src, int len, int copy_to)
{
	struct task_struct *tsk;
	struct mm_struct *mm;
	struct vm_area_struct *vma = NULL;
	struct page *page = NULL;
	unsigned long addr = (unsigned long int)src;
	void *maddr;
	int bytes, offset;
	int n, rv = 0;

	NFIN;
	_ND(nvmeib_public_copy_user_pages_d1,
		"buf=@PTR, pid=@INT, src=@PTR, len=@INT, copy_tp=@INT",
		buf, pid, src, len, copy_to);
	rcu_read_lock();
	if (pid) {
		tsk = _find_task_by_vpid(pid);
		if (!tsk) {
			rcu_read_unlock();
			rv = -ESRCH;
			goto out;
		}
	} else {
		rv = -1;
		goto out;
	}
	get_task_struct(tsk);
	rcu_read_unlock();
	mm = get_task_mm(tsk);
	put_task_struct(tsk);
	if (!mm) {
		_NE(error1_nvmeib_public_nvmeib_public_copy_user_pages,
			"MM is null - user process is going down\n");
		rv = -EINVAL;
		goto out;
	}
	mmap_read_lock(mm);
	while (len) {
#if !K_CHECK_VER(4,14,0)
		n = get_user_pages(
			tsk,
			mm,
			addr,
			1,
			copy_to,  /* 0: read, 1: write access only for in data */
			1, /* no force */
			&page,
			&vma);
#else
		n = get_user_pages_remote(
#if KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT
			tsk,
#endif
			mm,
			addr,
			1,
			copy_to ? FOLL_WRITE : 0,
			&page,
#if KS_GET_USER_PAGES_REMOTE_HAS_VMAS
			&vma,
#endif
			NULL);
#endif
		if (n != 1) {
			mmap_read_lock(mm);
			_NE(error2_nvmeib_public_nvmeib_public_copy_user_pages,
				"get_user_pages() failed");
			rv = -1;
			break;
		}
		bytes = len;
		offset = addr & (PAGE_SIZE-1);
		if (bytes > PAGE_SIZE - offset)
				bytes = PAGE_SIZE - offset;
		maddr = kmap_atomic(page);
		if (copy_to)
			copy_to_user_page(vma, page, addr, maddr + offset, buf, bytes);
		else
			copy_from_user_page(vma, page, addr, buf, maddr + offset, bytes);
		kunmap_atomic(maddr);
		if (copy_to)
			set_page_dirty_lock(page);
		put_page(page);
		len -= bytes;
		buf += bytes;
		addr += bytes;
	}
	mmap_read_unlock(mm);
	mmput(mm);

out:
	_ND(nvmeib_public_copy_user_pages_d2, "rv=@RV", rv);
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_copy_user_pages);

int nvmeib_public_user_pages_for_io_pin(pid_t pid, ulong userspace_vaddr, int n_pages, struct page **pages, int is_write)
{
	struct mm_struct	*mm;
	int					rv;
#if !K_CHECK_VER(4,14,0) || KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT
	struct task_struct	*task = _find_task_by_vpid(pid);
#endif

	mm = nvmeib_public_get_process_mm(pid);
	if (!mm) {
		_NE(t_01_eccpw, "PID=@INT is dying, aborting", pid);
		rv = -EINVAL;
		goto _out;
	}

	mmap_read_lock(mm);
#if !K_CHECK_VER(4,14,0)
	_NT(rvhjsa2, "local_cl get_user_pages(task=@PTR mm=@PTR userspace_vaddr=@LLX n_pages=@X is_write=@INT 1 pages=@PTR)",
		task, mm, userspace_vaddr, n_pages, is_write, pages);
	rv =  get_user_pages(       task, mm, userspace_vaddr, n_pages, is_write, 1 /*Force*/               , pages, NULL);			// TOMA does forks, We do force for the case where the memory arrived at the pocess after fork. In that case the page is marked as RO access and when we try to write the kernel will COW.
#else	// #if !K_CHECK_VER(4,14,0)
#if KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT
	_NT(cvshw2i, "local_cl get_user_pages_remote(task=@PTR mm=@PTR userspace_vaddr=@LLX n_pages=@X  is_write=@INT 1 pages=@PTR)",
		task, mm, userspace_vaddr, n_pages, is_write, pages);
	rv =  get_user_pages_remote(task, mm, userspace_vaddr, n_pages, is_write ? FOLL_WRITE|FOLL_FORCE : 0, pages, NULL, NULL);
#else	// #if KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT
	_NT(cvshw2i, "local_cl get_user_pages_remote(mm=@PTR userspace_vaddr=@LLX n_pages=@X  is_write=@INT 1 pages=@PTR)",
		mm, userspace_vaddr, n_pages, is_write, pages);
#if KS_GET_USER_PAGES_REMOTE_HAS_VMAS
	rv =  get_user_pages_remote(mm, userspace_vaddr, n_pages, is_write ? FOLL_WRITE|FOLL_FORCE : 0, pages, NULL, NULL);
#else // KS_GET_USER_PAGES_REMOTE_HAS_VMAS
	rv =  get_user_pages_remote(mm, userspace_vaddr, n_pages, is_write ? FOLL_WRITE|FOLL_FORCE : 0, pages, NULL);
#endif // KS_GET_USER_PAGES_REMOTE_HAS_VMAS
#endif	// #if KS_GET_USER_PAGES_REMOTE_HAS_TASK_STRUCT
#endif	// #if !K_CHECK_VER(4,14,0)
	_NT(morudgf, "local_cl rv=@INT", rv);
	{
#if 0
		int i;	// DanielHsH: Atomic mapping is much faster operation but there are less such pages, if used in datapath, consider switching to atomic
		for (i = 0; i < rv; i++)
			kmap_atomic(pages[i]);
#else
		const void *vaddr = /*(rv >= 0) ? vmap(pages, rv, VM_MAP, PAGE_KERNEL) :*/ NULL;		// Daniel: Currently we dont need contigous address space for BIO
		_NT(t_02_eccpw, "PID=@INT: Pinned @INT pages, pages[0].phys=@LLX, to virtual address @LLX", pid, rv, (unsigned long long)page_address(pages[0]), (unsigned long long)vaddr);
#endif
	}
	mmap_read_unlock(mm);
	nvmeib_public_put_process_mm(mm);
_out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_user_pages_for_io_pin);

void nvmeib_public_user_pages_for_io_unpin(int n_pages, struct page **pages, /*void * vaddr, */ int copy_to)
{
	int i;
	if (n_pages > 0) {
		const void *vaddr =  NULL;
		_NT(t_03_eccpw, "Unmapping @INT pages, pages[0].phys=@LLX from virtual address @LLX", n_pages, (unsigned long long)page_address(pages[0]), (unsigned long long)vaddr);
		//vunmap(vaddr);
	}
	for (i = 0; i < n_pages; i++) {
		struct page *page = pages[i];
		#if 0
			kunmap_atomic(page_address(page));
		#endif
		if (copy_to)
			set_page_dirty_lock(page);
		put_page(page);
	}
}
EXPORT_SYMBOL(nvmeib_public_user_pages_for_io_unpin);

#ifdef LLVM
extern struct workqueue_struct *system_wq;
#else
extern struct workqueue_struct *system_wq __read_mostly;
#endif

int nvmeib_schedule_delayed_work(struct delayed_work *dwork,
								 unsigned long delay) {
	return queue_delayed_work(system_wq, dwork, delay);
}
EXPORT_SYMBOL(nvmeib_schedule_delayed_work);

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-security"
#endif
struct workqueue_struct *nvmeib_public_alloc_workqueue(const char *fmt, unsigned int flags, int max_active)
{
	return alloc_workqueue(fmt, flags, max_active);
}
#ifdef __clang__
#pragma clang diagnostic pop
#endif
EXPORT_SYMBOL(nvmeib_public_alloc_workqueue);

void nvmeib_public_destroy_workqueue(struct workqueue_struct *wq)
{
	destroy_workqueue(wq);
}
EXPORT_SYMBOL(nvmeib_public_destroy_workqueue);

void nvmeib_public_flush_workqueue(struct workqueue_struct *wq)
{
	flush_workqueue(wq);
}
EXPORT_SYMBOL(nvmeib_public_flush_workqueue);

bool nvmeib_public_workqueue_congested(int cpu, struct workqueue_struct *wq)
{
	return workqueue_congested(cpu, wq);
}
EXPORT_SYMBOL(nvmeib_public_workqueue_congested);

#if KS_BIO_BI_STATUS
blk_status_t nvmeib_errno_to_blk_status(int errno) {
	return errno_to_blk_status(errno);
}
EXPORT_SYMBOL(nvmeib_errno_to_blk_status);

int nvmeib_blk_status_to_errno(blk_status_t status) {
	return blk_status_to_errno(status);
}
EXPORT_SYMBOL(nvmeib_blk_status_to_errno);
#endif

struct ring_buffer_event;
struct ring_buffer;
struct ftrace_event_call;
struct ftrace_event_file;

#if 0
extern void* ring_buffer_event_data(struct ring_buffer_event *event);
void* nvmeib_ring_buffer_event_data(struct ring_buffer_event *event) {
	return ring_buffer_event_data(event);
}
EXPORT_SYMBOL(nvmeib_ring_buffer_event_data);
extern int filter_current_check_discard(struct ring_buffer *buffer,
										struct ftrace_event_call *call, void *rec,
										struct ring_buffer_event *event);
int nvmeib_filter_current_check_discard(struct ring_buffer *buffer,
										struct ftrace_event_call *call, void *rec,
										struct ring_buffer_event *event) {
	return nvmeib_filter_current_check_discard(buffer, call, rec, event);
}
EXPORT_SYMBOL(nvmeib_filter_current_check_discard);
extern struct ring_buffer_event*
trace_event_buffer_lock_reserve(struct ring_buffer **current_rb,
								struct ftrace_event_file *ftrace_file,
								int type, unsigned long len,
								unsigned long flags, int pc);
struct ring_buffer_event*
nvmeib_trace_event_buffer_lock_reserve(struct ring_buffer **current_rb,
									   struct ftrace_event_file *ftrace_file,
									   int type, unsigned long len,
									   unsigned long flags, int pc) {
	return trace_event_buffer_lock_reserve(current_rb, ftrace_file, type, len, flags, pc);
}
EXPORT_SYMBOL(nvmeib_trace_event_buffer_lock_reserve);

extern int trace_define_field(struct ftrace_event_call *call, const char *type,
							  const char *name, int offset, int size, int is_signed,
							  int filter_type);
int nvmeib_trace_define_field(struct ftrace_event_call *call, const char *type,
							  const char *name, int offset, int size, int is_signed,
							  int filter_type) {
	return trace_define_field(call, type, name, offset, size, is_signed, filter_type);
}
EXPORT_SYMBOL(nvmeib_trace_define_field);

extern int ftrace_event_reg(struct ftrace_event_call *call,
							enum trace_reg type, void *data);
int nvmeib_ftrace_event_reg(struct ftrace_event_call *call,
							enum trace_reg type, void *data) {
	return ftrace_event_reg(call, type, data);
}
EXPORT_SYMBOL(nvmeib_ftrace_event_reg);

extern int trace_event_raw_init(struct ftrace_event_call *call);
int nvmeib_trace_event_raw_init(struct ftrace_event_call *call) {
	return trace_event_raw_init(call);
}
EXPORT_SYMBOL(nvmeib_trace_event_raw_init);
#endif

#ifndef __kprobes
	#define __kprobes
#endif

void nvmeib_public_save_stack_trace(struct nvmeib_stack_trace *trace)
{
	trace->skip++;
#ifdef CONFIG_ARCH_STACKWALK
	trace->nr_entries = stack_trace_save(trace->entries, trace->max_entries, trace->skip);
#else
	save_stack_trace((struct stack_trace *)trace);
#endif
}
EXPORT_SYMBOL(nvmeib_public_save_stack_trace);

NVMEIB_DECLARE_KERNEL_WARNINGS_TRAP

void nvmeib_public_kgdb_breakpoint(void)
{
#if 0
	if (kgdb_connected)
		kgdb_breakpoint();
#endif
}
EXPORT_SYMBOL(nvmeib_public_kgdb_breakpoint);

unsigned long nvmeib_kallsyms_lookup_name(const char *name)
{
#if KS_KALLSYMS_LOOKUP
	return kallsyms_lookup_name(name);
#else
	return 0;
#endif
}
EXPORT_SYMBOL(nvmeib_kallsyms_lookup_name);

/**
 * @WARNING: This is a hack, needed due to a kernel bug.
 * For details,
 * https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/commit/?id=8244062ef1e54502ef55f54cced659913f244c3e
 *
 * In short, there is a short period when it is possible that it is not safe
 * to resolve a symbol to string, even if module is loaded and all seems fine.
 * This is due to a race condition in earlier kernel versions.
 *
 * The following function returns true if we are in the critical section when
 * the race condition can happen, and false otherwise.
 *
 * It shall be used before any symbol resolve call, i.e. call to kallsyms,
 * sprintf or printk via %pf or %pF format etc.
 */
bool nvmeib_sym_resolve_kernel_bug_can_happen(void* addr)
{
#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 5)
	struct module* mod;
	bool answer;
	preempt_disable();
	mod = __module_address((unsigned long)addr);
	if(!mod)
	{ /* Address is bad, or modules list is buad, assume the worst. */
		answer = true;
	}
	else
	{ /* Address is good. Validate symtable is up to date. If not - bug can happen. */
		answer = (mod->symtab != mod->core_symtab);
	}
	preempt_enable();
	return answer;
#else
	return false; /* Bug is already fixed in this kernel version */
#endif
}
EXPORT_SYMBOL(nvmeib_sym_resolve_kernel_bug_can_happen);

void nvmeib_ref_init(struct nvmeib_ref *r)
{
	NFIN;
	atomic_set(&r->cnt, 1);
	r->comp = NULL;
	atomic_set(&r->dying, 0);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_ref_init);

int __must_check nvmeib_ref_get(struct nvmeib_ref *r)
{
	int _old;

	NFIN;
	if (!atomic_read(&r->dying))
		_old = atomic_inc_not_zero(&r->cnt);
	else
		_old = 0;
	NFOUT;
	return _old;
}
EXPORT_SYMBOL(nvmeib_ref_get);

int nvmeib_ref_put(struct nvmeib_ref *r)
{
	int _new;

	NFIN;
	if (!(_new = atomic_dec_return(&r->cnt))) {
		if (r->comp)
			complete(r->comp);
		else {
			_NW(warn_nvmeib_ref_release_put, "r @PTR: cnt = 0 w/o comp", r);
			WARN_ON(1);
		}
	}
	NFOUT;
	return _new;
}
EXPORT_SYMBOL(nvmeib_ref_put);

int nvmeib_ref_release_start(struct nvmeib_ref *r)
{
	int dying;
	int rv;

	NFIN;
	if ((dying = atomic_inc_return(&r->dying)) == 1) {
		_NT(nvmeib_ref_release_start_t1, "r @PTR: initiate release", r);
		rv = 0;
	} else {
		_NT(nvmeib_ref_release_start_t2,
			"r @PTR: release already initaited (@INT)", r, dying);
		rv = -1;
	}

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_ref_release_start);

void nvmeib_ref_release_wait_n(struct nvmeib_ref *r, unsigned num_attempts)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	int n, rvw;
	unsigned a = 0;

	NFIN;
	/* release prior init */
	if (atomic_read(&r->cnt) <= 0)
		goto out;

	r->comp = &comp;
	if ((n = atomic_dec_return(&r->cnt))) {
		_NT(nvmeib_ref_release_wait_t1, "r @PTR: wait for @INT to finish",
			r, n);
		while ((rvw = wait_for_completion_interruptible_timeout(
			r->comp, NVMEIB_REF_WAIT_RELEASE)) <= 0) {
			_NW(nvmeib_ref_release_wait_w1,
				"r @PTR: wait for @INT to finish, attempt @INT",
				r, atomic_read(&r->cnt), a);
			a++;
			if (a == num_attempts) {
				_NW(nvmeib_ref_release_wait_w2,
					"r @PTR: max attempts @INT. Giving up",
					r, num_attempts);
				break;
			}
		}
	}
	r->comp = NULL;

out:
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_ref_release_wait_n);


/* KASAN
 * 
 * Enable use of kasan_poison / kasan_unpoison for NVMesh
 * Can be used to find memory corruptions.
 * 
 * See nvmeib_public_kasan_test() for example usage
 *
 * NOTE: Only x86 and x86_64 is supported.
 * 
 * NOTE: kasan_multishot=on must be passed on kernel command-line.
 */
#if defined(CONFIG_KASAN) && defined(CONFIG_X86)
#include <linux/kasan.h>
extern void kasan_poison(const void *addr, size_t size, u8 value, bool init);

/* Taken from mm/kasan/shadow.c - Only x86 version */
inline static u8 get_tag(const void *addr)
{
	return 0;
}

#define KASAN_PAGE_FREE		0xFF  /* freed page */
#define KASAN_PAGE_REDZONE	0xFE  /* redzone for kmalloc_large allocation */
#define KASAN_SLAB_REDZONE	0xFC  /* redzone for slab object */
#define KASAN_SLAB_FREE		0xFB  /* freed slab object */
#define KASAN_VMALLOC_INVALID	0xF8  /* inaccessible space in vmap area */

#define KASAN_SHADOW_SCALE_SHIFT 3
#define KASAN_GRANULE_SIZE	(1UL << KASAN_SHADOW_SCALE_SHIFT)
#define KASAN_GRANULE_MASK	(KASAN_GRANULE_SIZE - 1)

int nvmeib_public_kasan_poison(const void *addr, size_t size, enum nvmeib_public_kasan_poison_type poison_type)
{
	int rv;
	u8 value;

	switch (poison_type) {
	case NVMEIB_PUBLIC_KASAN_POISON_TYPE_KMALLOC:
		value = KASAN_SLAB_REDZONE;
		break;
	case NVMEIB_PUBLIC_KASAN_POISON_TYPE_PAGES:
		value = KASAN_PAGE_REDZONE;
		break;
	case NVMEIB_PUBLIC_KASAN_POISON_TYPE_VMALLOC:
		value = KASAN_VMALLOC_INVALID;
		break;
	default:
		BUG();
	}

	if (is_vmalloc_addr(addr)) {
#ifndef CONFIG_KASAN_VMALLOC
		_NW_dmesg(warn_public_kasan_poison_vm_not_supp, 
			  "KASAN VMALLOC support is not enabled");
		rv = -EOPNOTSUPP;
		goto out;
#endif
	}

	if (((unsigned long)addr & KASAN_GRANULE_MASK)) {
		_NW_dmesg(warn_public_kasan_poison_addr_not_aligned, 
			  "KASAN - Address @ADDR alignment error", (u64)addr);
		rv = -EFAULT;
		goto out;
	}

	if (((unsigned long)size & KASAN_GRANULE_MASK)) {
		_NW_dmesg(warn_public_kasan_poison_size_not_aligned, 
			  "KASAN - Size @SIZE_T alignment error", size);
		rv = -EFAULT;
		goto out;
	}

	kasan_poison(addr, size, value, false);
	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kasan_poison);

int nvmeib_public_kasan_unpoison(const void *addr, size_t size)
{
	u8 tag = get_tag(addr);
	int rv;

#ifndef CONFIG_KASAN_VMALLOC
	if (is_vmalloc_addr(addr)) {
		_NW_dmesg(warn_public_kasan_unpoison_vm_not_supp, 
			  "KASAN VMALLOC support is not enabled");
		rv = -EOPNOTSUPP;
		goto out;
	}
#endif

	/*
	 * Perform shadow offset calculation based on untagged address, as
	 * some of the callers (e.g. kasan_unpoison_new_object) pass tagged
	 * addresses to this function.
	 */
	addr = kasan_reset_tag(addr);

	if (((unsigned long)addr & KASAN_GRANULE_MASK)) {
		_NW_dmesg(warn_public_kasan_unpoison_addr_not_aligned, 
			  "KASAN - Address @ADDR alignment error", (u64)addr);
		rv = -EFAULT;
		goto out;
	}

	if (((unsigned long)size & KASAN_GRANULE_MASK)) {
		_NW_dmesg(warn_public_kasan_unpoison_size_not_aligned, 
			  "KASAN - Size @SIZE_T alignment error", size);
		rv = -EFAULT;
		goto out;
	}

	/* Unpoison all granules that cover the object. */
	kasan_poison(addr, size, tag, false);
	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kasan_unpoison);

#ifdef CONFIG_KASAN_VMALLOC
static int __test_kasan_vmalloc(void) {
	int rv;
	void *p = NULL;
	const size_t redzone_sz = 32;

	/* Test vmalloc poison */
	if (!(p = vmalloc(redzone_sz))) {
		_NE(err_public_test_kasan_vmalloc, "OOM");
		rv = -ENOMEM;
		goto out;
	}

	_NI_dmesg(trace_public_kasan_test_vmalloc, "Testing KASAN poison of vmalloc memory with ptr @ADDR", (u64)p);

	/* Poison kmalloc area */
	if ((rv = nvmeib_public_kasan_poison(p, redzone_sz, NVMEIB_PUBLIC_KASAN_POISON_TYPE_VMALLOC)) <  0) {
		_NI(trace_public_kasan_test_vmalloc_poison_fail, "kasan_poison failed (@RV)", rv);
		goto out;
	}

	/* Probe poisoned vmalloc area - Should trigger KASAN */
	WRITE_ONCE(*(volatile u8 *)p, 0xcc);

	/* Unpoison vmalloc area */
	if ((rv = nvmeib_public_kasan_unpoison(p, redzone_sz)) <  0) {
		_NI(trace_public_kasan_test_vmalloc_unpoison_fail, "kasan_unpoison failed (@RV)", rv);
		goto out;
	}

out:
	vfree(p);
	return rv;
}
#else /* CONFIG_KASAN_VMALLOC */
static int __test_kasan_vmalloc(void) {
	return -ENOTSUPP;
}
#endif /* CONFIG_KASAN_VMALLOC */

int nvmeib_public_kasan_test(void)
{
	int rv;
	void *p_kmalloc = NULL, *p_pages = NULL;
	const size_t redzone_sz = 32;

	/* Test kmalloc poison */
	if (!(p_kmalloc = kmalloc(redzone_sz, GFP_KERNEL))) {
		_NE(err_public_kasan_test_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}

	_NI_dmesg(trace_public_kasan_test_kmalloc, "Testing KASAN poison of kmalloc memory with ptr @ADDR", (u64)p_kmalloc);

	/* Poison kmalloc area */
	if ((rv = nvmeib_public_kasan_poison(p_kmalloc, redzone_sz, NVMEIB_PUBLIC_KASAN_POISON_TYPE_KMALLOC)) <  0) {
		_NI(trace_public_kasan_test_kmalloc_poison_fail, "kasan_poison failed (@RV)", rv);
		goto out;
	}

	/* Probe poisoned kmalloc area - Should trigger KASAN */
	WRITE_ONCE(*(volatile u8 *)p_kmalloc, 0xcc);

	/* Unpoison kmalloc area */
	if ((rv = nvmeib_public_kasan_unpoison(p_kmalloc, redzone_sz)) <  0) {
		_NI(trace_public_kasan_test_kmalloc_unpoison_fail, "kasan_unpoison failed (@RV)", rv);
		goto out;
	}

	/* Test pages poison */
	if (!(p_pages = (void *)__get_free_page(GFP_KERNEL))) {
		_NE(err_2_public_kasan_test_oom, "OOM");
		rv = -ENOMEM;
		goto out;
	}

	_NI_dmesg(trace_public_kasan_test_pages, "Testing KASAN poison of pages memory with ptr @ADDR", (u64)p_pages);

	/* Poison pages area */
	if ((rv = nvmeib_public_kasan_poison(p_pages, redzone_sz, NVMEIB_PUBLIC_KASAN_POISON_TYPE_PAGES)) <  0) {
		_NI(trace_public_kasan_test_pages_poison_fail, "kasan_poison failed (@RV)", rv);
		goto out;
	}

	/* Probe poisoned pages area - Should trigger KASAN */
	WRITE_ONCE(*(volatile u8 *)p_pages, 0xcc);

	/* Unpoison pages area */
	if ((rv = nvmeib_public_kasan_unpoison(p_pages, redzone_sz)) <  0) {
		_NI(trace_public_kasan_test_pages_unpoison_fail, "kasan_unpoison failed (@RV)", rv);
		goto out;
	}

	rv = __test_kasan_vmalloc();

out:
	if (p_pages) {
		/* Free pages area */
		free_page((unsigned long)p_pages);
	}
	kfree(p_kmalloc);
	return rv;
}
EXPORT_SYMBOL(nvmeib_public_kasan_test);

#else /* defined(CONFIG_KASAN) && defined(CONFIG_X86) */

int nvmeib_public_kasan_poison(const void *addr, size_t size, enum nvmeib_public_kasan_poison_type poison_type)
{
	(void)addr;
	(void)size;
	(void)poison_type;

	_NW_dmesg(warn_public_kasan_poison_not_supp, "KASAN not enabled");
	return -ENOTSUPP;
}
EXPORT_SYMBOL(nvmeib_public_kasan_poison);

int nvmeib_public_kasan_unpoison(const void *addr, size_t size)
{
	(void)addr;
	(void)size;

	_NW_dmesg(warn_public_kasan_unpoison_not_supp, "KASAN not enabled");
	return -ENOTSUPP;
}
EXPORT_SYMBOL(nvmeib_public_kasan_unpoison);

int nvmeib_public_kasan_test(void)
{
	_NW_dmesg(warn_public_kasan_test_not_supp, "KASAN not enabled");
	return -ENOTSUPP;
}
EXPORT_SYMBOL(nvmeib_public_kasan_test);

#endif /* defined(CONFIG_KASAN) && defined(CONFIG_X86) */

struct task_struct *nvmeib_kthread_create_on_cpu(
	int (*threadfn)(void *data), void *data, unsigned int cpu, const char *namefmt)
{
	struct task_struct *p;
	p = kthread_create_on_node(threadfn, data, cpu_to_node(cpu), namefmt, cpu);
	if (!IS_ERR(p))
		kthread_bind(p, cpu);
	return p;
}
EXPORT_SYMBOL(nvmeib_kthread_create_on_cpu);

/* symbol_get/put convenience wrappers remain for type casting */

module_init(nvmeib_public_module_init);
module_exit(nvmeib_public_module_exit);
