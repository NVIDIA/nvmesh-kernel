/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_PERCPU_H
#define KR_INCS_PERCPU_H

#if defined(__KERNEL__)

	#include <linux/percpu.h>
	#include <linux/cpumask.h>

#elif defined(UM_APP)
	#include "framework/scheduler.h"

	#define NR_CPUS	MAX_SCHEDULERS
	typedef struct cpumask {} cpumask_t, *cpumask_var_t;
	static inline unsigned int smp_processor_id(void) { return scheduler_current_unique_id(); }
	#define for_each_online_cpu(i)  for (i=0; i< (typeof(i))spdk_env_get_core_count(); i++)
	#define for_each_possible_cpu(i)   for_each_online_cpu(i)
	#define __percpu
	#define get_cpu() smp_processor_id()
	#define put_cpu() do{} while(0)
	#define per_cpu_ptr(var, cpu) &((var)[(cpu)])
	#define DEFINE_PER_CPU(type, name) type name[MAX_SCHEDULERS]

	static inline void on_each_cpu(void (*func)(void *info), void *info, int wait) { (void)func; (void)info; (void)wait; BUG();} /* TODO */
	#define nvmeib_public_alloc_percpu_zeroed(type) calloc(MAX_SCHEDULERS, sizeof(type))
	#define nvmeib_public_free_percpu(ptr) free(ptr)
	#define nvmeib_public_alloc_percpu_cacheline(type) ({BUG(); NULL;})
	#define nvmeib_public_zero_percpu(_obj) ({BUG(); (void)_obj;})

#elif defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR
	#include "kr_incs_malloc.h"
	/************************ Per CPU implementation *****************************/
	// /linux/compiler.h, /linux/percpu.h, /linux/cpumask.h, /linux/cache.h
	// to simplify the code (and since performance arent that critical), we dont allocate cachaline's
	// rather, we treat the percpu allocation as a simple array.
	// so an aligned allocation will allocate more memory than required but that's ok for a simulator.
	#define USE_SMP_PER_CPU		// whether the real percpu is in use
	extern int nr_cpu_ids;
	#define ____page_aligned __attribute__((__aligned__(PAGE_SIZE)))

	// static per-cpu variable declaration & use
	// note that sizeof(type) must be greater or equal to 4 in order to ensure minimal alignment
	#define DEFINE_PER_CPU(        type, name)                        type name[NR_CPUS]
	#define DECLARE_PER_CPU(       type, name)                 extern type name[NR_CPUS]
	#define DEFINE_PER_CPU_ALIGNED(type, name)  ____cacheline_aligned type name[NR_CPUS]
	#define DEFINE_PER_CPU_PAGE_ALIGNED(type, name)  ____page_aligned type name[NR_CPUS]
	#define per_cpu_ptr(ptr,cpu)	(is_kmalloc_percpu(ptr) ? (typeof(ptr))per_cpu_kalloc_ptr(ptr, cpu) : ptr+cpu)
	#define get_cpu_var(var)		(*per_cpu_var(var, get_cpu())
	#define put_cpu_var(var)		({ (void)&(var); put_cpu(); })
	#define per_cpu(var, cpu)		(*(per_cpu_ptr(var, cpu)))
	// dynamic allocate percpu
	#define this_cpu_ptr(var) ({typeof(var) rv = per_cpu_ptr(var, get_cpu()); put_cpu(); rv; })
	int  get_cpu(void);
	void put_cpu(void);
	#define for_each_possible_cpu(i)  for (i=0; i<nr_cpu_ids; i++)
	#define for_each_online_cpu(i)   for_each_possible_cpu(i)
	#define __percpu
	void __percpu *__alloc_percpu(size_t size, size_t align);
	void __percpu *__alloc_percpu_gfp(size_t size, size_t align, gfp_t gfp);
	void free_percpu(void __percpu *__pdata);
	bool is_kmalloc_percpu(void __percpu *ptr);
	void *per_cpu_kalloc_ptr(void __percpu *ptr, int cpu);
	#define alloc_percpu(type)   (typeof(type) __percpu *)__alloc_percpu(sizeof(type), __alignof__(type))
	#define this_cpu_inc(var) {typeof((var)) *var_ptr = per_cpu_ptr(&var, get_cpu()); (*var_ptr)++; put_cpu();}
	#define this_cpu_dec(var) {typeof((var)) *var_ptr = per_cpu_ptr(&var, get_cpu()); (*var_ptr)--; put_cpu();}

	typedef void (*smp_call_func_t)(void *info);
	int on_each_cpu(smp_call_func_t func, void *info, int wait);

#else

#endif//

//__percpu concept - a bridge between Kernel & User space is needed
//In the user space we have few options:
//1. __thread variable (https://gcc.gnu.org/onlinedocs/gcc/Thread-Local.html#Thread-Local)
//2. arrays (kernel client simulator implementation)
//3. atomic counters
//4. dynamic arrays
//5. your idea
//The options 1 & 2 & 3 are not ideal;
//  __thread resolving the variable address could be done only in the runtime
//    we may place whole struct nvmesh_memmgr_metrics as __thread object
//    another problem we should solve is iteration over the section - looks like doable, but POC is needed
// arrays
//    doable and pretty easy, the main issue is memory consumption
//    sizeof(struct nvmesh_memmgr_metric_counters) == 176
//    if we will run on 256 CPU's - 44KB per counter, way too much
//    Today we have limitation on the schedulers numbers. The limitation is derived from the way we maintain 
//    EC journals.
//  atomic counters
//    slow and against shared nothing approach
//
//Dynamic arrays is the "go to market" solution
//1. We need to add "per_scheduler" section.
//2. We need to store 2 things there 
//	 * the desired object size and probably alignment 
//	 * pointer to allocated array, the array size is number of schedulers
//3. On "main" scheduler create we should iterate on the "per_scheduler" section and allocate the memory.
//4. We still have one problem to solve - we need to update the references to those "per scheduler" variables.
//   Of course if we refer to them during the compilation:
//   static DEFINE_PER_CPU(TypeXYZ, varname);
//   static struct ABC{TypeXYZ *x} abc = &varname;
//
//   This problem can be solved by one more section, which contains pointers to functions. Those functions will be executed after 
//   the per scheduler memory will be allocated. And will update the references. 
//
//The dynamic array was not implemented because of the development costs:
//Today the biggest user of static per CPU is nvmesh_memmgr_metrics_counters struct. It size is 168 bytes.
//168 bytes * 32 scheduler * 100 metrics / 1KB == 525KB of memory
//Not ideal, but not that awful. 

//since the kernel simulator incorrectly simulates per cpu functionality via array,
//while the real kernel exposes it as an single object, we need the following indirection
#define nvmesh_get_percpu_variable_address(type, varname)					   \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(varname), type),   &(varname), \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(varname), type[]),	(varname), \
	(void)0 ))


#endif//KR_INCS_PERCPU_H
