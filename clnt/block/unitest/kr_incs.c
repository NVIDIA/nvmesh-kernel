/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "kr_incs.h"
#include "compat/kr_incs_percpu.h"
#include "kr_sim_svc.h"
#include <stdarg.h>						// using variable size arguments to functions.
#include <execinfo.h>					// For printing the stack in case of a bug
#include <stdatomic.h>
#include "uni_framework/range_algorithms.h"
//#include <sys/random.h> //getrandom
#include "nvmeibc_trace.h"
#include "common/compat/kr_incs_compiler_types.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeibs_memmgr_metrics.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"

NVMEIBC_MEMMGR_METRIC(client_total_mem, "component=client");
NVMEIBC_MEMMGR_METRIC(simulator_total_mem, "component=simulator");

enum allocated_by {AB_CLIENT = 0, AB_SIMULATOR = 1, AB_NUM};

bool simulator_alloc_tracking_enabled = false;

void simulator_alloc_tracking_init(void)
{
	simulator_alloc_tracking_enabled = true;
}

struct nvmesh_memmgr_metrics* memmgr_total_metrics[AB_NUM];

const struct nvmesh_memmgr_metrics *unitest_get_memmgr_metric_client_total_mem(void) { return client_total_mem; }
const struct nvmesh_memmgr_metrics *unitest_get_memmgr_metric_simulator_total_mem(void) { return simulator_total_mem; }

#define PER_CPU_DYN_MEM 0x1

struct kernel_sim	kernel_sim;
pthread_t 	main_os_id;	// the os_id (pthread_id) of main()
pthread_t 	ut_os_id;	// the os_id (pthread_id) of blk_unit_test()
#define _logFile	stderr				// or use: stdout, stderr
static bool logsEnabled = true;
void printk_enable(bool on_off){
	logsEnabled = on_off;
}

bool printk_is_enabled(void){
	return logsEnabled;
}

/* By default: Do not print debug messages. If logs disabled, print only critical messages and higher.*/
int printk(const char *fmt,...){
#if defined(USER_SPACE_TRACING)
	// There are 2 ways to get here.
	// 1. Someone called printk directly - in this case we would like to redirect to binary tracer
	// 2. It was called from binary tracer, when mirroring to DMESG. Those messages will have a
	//    special symbol ('\001') at the end of the format string. These messages we want to
	//    output directly to _logFile.
	int fmt_last_char = strlen(fmt) - 1;
	const bool is_mirrored_print = fmt_last_char > 0 && fmt[fmt_last_char] == '\001';

	{ // First - legacy behaviour
		bool should_print;
		extern int tracer_nvmeibc_debug_level;
		if (is_mirrored_print) {
			int dbg_lvl;
			// Printk here uses characters for debug level. We need to convert them to normal integer.
			// Error = 3, Warning = 4, Info = 6.
			switch (fmt[1]) {
				case '3': dbg_lvl = 1; break;
				case '4': dbg_lvl = 2; break;
				case '6': dbg_lvl = 3; break;
				default : dbg_lvl = 0; break;
			}
			should_print = (tracer_nvmeibc_debug_level >= dbg_lvl);
		} else
			should_print = (logsEnabled ? (fmt[1] != '7') : (fmt[1] <= '2'));
		if (should_print && fmt[0] == '\001') { /* Verify legal prefix cmd */
			va_list ap;
			va_start(ap, fmt);
			vfprintf(_logFile, fmt + 2, ap); /* Skip prefix */
			va_end(ap);
		}
	}
	if (!is_mirrored_print) { // Now see if we also would like to send it to binary tracer.
		char __temp_str[320] = {0};
		int len;
		va_list ap;
		va_start(ap, fmt);
		len = min(vsnprintf(__temp_str, sizeof(__temp_str), fmt + 2, ap), (int)ARRAY_SIZE(__temp_str));
		va_end(ap);
		if (len > 0 && __temp_str[len - 1] == '\n'){
			__temp_str[len - 1] = '\0'; // Excessive trailing \n
		}
		// Put the trace. Level is always error, as high possible
		NVMEIB_LOG_LONGTERM("@BUF_STR", _E, /*Default scope*/, printk, __temp_str);
	}
	return 1; // Return value is stub. It is never used anyway. Just make sure it is positive.
#else
	int res = 0;
	const bool should_print = (logsEnabled ? (fmt[1]!='7') : (fmt[1]<='2'));
	if (should_print && (fmt[0]=='\001')) {		/* Verify legal prefix cmd */
		va_list ap;
		va_start(ap,fmt);
		res = vfprintf(_logFile, fmt+2, ap);	/* Skip prefix */
		va_end(ap);
	}
	return res;
#endif
}

int vscnprintf(char *buf, size_t size, const char *fmt, va_list args)
{
	size_t i;

	if (!size)
		return 0;

	i = vsnprintf(buf, size, fmt, args);

	if (i < size)
		return i;

	return size - 1;
}

void dump_backtrace(void **buffer, int nptrs)
{
   	char **strings = backtrace_symbols(buffer, nptrs);

	pr_emerg("backtrace() returned %d addresses\n", nptrs);

	if (strings == NULL) {
		pr_emerg("No crash stack available..\n");
		return;
	}

	for (int j = 0; j < nptrs; j++){
		pr_emerg("\t%s\n", strings[j]);
	}

	free(strings);
}

void dump_stack(void)
{
	void *buffer[100];
	int nptrs = backtrace(buffer, ARRAY_SIZE(buffer));
	dump_backtrace(buffer, nptrs);
}

bool insert_single_failure(void){
	static u64 n_fails = 0;
	bool res = (n_fails++ == 0);
	return res;
}

/************************ Memory allocation mechanism ************************/
static u8 empty_zero_page_data_gpl_dont_use_directly[PAGE_SIZE] __attribute__ ((aligned (PAGE_SIZE))) = {0};	// zero page
static struct page empty_zero_page_gpl_dont_use_directly = { .mapped_vaddr = empty_zero_page_data_gpl_dont_use_directly };
struct page *ZERO_PAGE(u64 vaddr) { (void)vaddr; return &empty_zero_page_gpl_dont_use_directly; }	// (virt_to_page(ZERO_PGE))

#define MALLOC_BUF_OVERRUN_MAGIC	(0x1dadbeef)	// 32 bits magic number to guard against buffer overruns
#define MEM_ALLOCATION_INSERT_RANDOM_FAILURES	(0)	// If (1), will occasionaly throw out of memory errors. Used to test proper erro handling
static inline bool __kmem_was_canary_prefix_hurt(const u32* value) {return *value !=     MALLOC_BUF_OVERRUN_MAGIC; }
static inline bool __kmem_was_canary_suffix_hurt(const u8*  value) {return *value != (u8)MALLOC_BUF_OVERRUN_MAGIC; }

static inline bool __kmem_random_failure_occured(void){
	#if MEM_ALLOCATION_INSERT_RANDOM_FAILURES
		static UINT rndCnt = 0;
		if ((++rndCnt)%311==0){						// Good prime numbers to use: 97, 179, 311, 389, 619, 727
			pr_info("Random memory failure invoked...\n");
			return true;
		}
	#endif
	return false;
}

union t_malloc_flags {
	struct {
		u32 reserved                    : 24;		// Kernel flags are here
		u32 do_zero                     : 1; // 24
		u32 is_virtual_mem              : 1; // 25
		u32 is_alloced_with_page_struct : 1; // 26
		u32 was_already_freed           : 1; // 27
		u32 page_order                  : 4; // 28 - 31 - 4 bits in flags are reserved for page alloc order (16, more  then enouth, kernel limit is 5 anyway). For regular malloc  * the order is 0. For get_free_page it is 1. For  get_free_pages(x) it is x
	};
	u32 all;
};

struct __kmem_canary_prefix{
	struct list_head list;
	void* fn;
	u32 alloc_size;
	union t_malloc_flags flags;
	enum allocated_by allocated_by;
	u32 overrun_magic[4];
};

struct __kmem_canary_suffix{
	u8 overrun_magic[7*4];		// Cant bt u32 as it follows allocated user object which might be 1 byte alligned (like strdup())
};

struct allocs_manager {
	atomic_t numActiveAllocs;
	struct list_head all_allocs_list;
	spinlock_t lock;
	struct t_allocs_manager_measure {
		int n_allocs;
		int total_bytes;
		bool enabled;
	} measure;
} __aloc_mgr;
#define ALLOC_MANAGER_DEBUGGER (0)			// All mem allocs are serialized by spinlock but allows better debugging of memory leaks

void kget_num_allocs_start(void) {
	__aloc_mgr.measure.enabled = 1;
}
void kget_num_allocs_stop(void) {
	__aloc_mgr.measure.enabled = 0;
	pr_emerg("Total: %8d[b]\n", __aloc_mgr.measure.total_bytes);
	exit(1);
}

void allocs_manager_create(void) {
	struct allocs_manager *am = &__aloc_mgr;
	INIT_LIST_HEAD(&am->all_allocs_list);
	atomic_set(&am->numActiveAllocs, 0);
	memset(&am->measure, 0, sizeof(am->measure));
	spin_lock_init(&am->lock);

	memmgr_total_metrics[AB_CLIENT] = client_total_mem;
	memmgr_total_metrics[AB_SIMULATOR] = simulator_total_mem;
}

void allocs_manager_add(struct __kmem_canary_prefix *p) {
	struct allocs_manager *am = &__aloc_mgr;
	#if ALLOC_MANAGER_DEBUGGER
		ulong flags;
		spin_lock_irqsave(&am->lock, flags);
		if (get_kernel_status == KERNEL_STATUS_RUNNING) {
			list_add(&p->list, &am->all_allocs_list);
			#pragma GCC diagnostic push
			#pragma GCC diagnostic ignored "-Wframe-address"
			p->fn = __builtin_return_address(2);		// Safe becase we have at least 2 callers caller()->kzalloc()->kmalloc()->me
			#pragma GCC diagnostic pop
		}
		spin_unlock_irqrestore(&am->lock, flags);
	#endif
	(void)p;
	atomic_inc(&am->numActiveAllocs);
}
void allocs_manager_del(struct __kmem_canary_prefix *p) {
	struct allocs_manager *am = &__aloc_mgr;
	#if ALLOC_MANAGER_DEBUGGER
		ulong flags;
		spin_lock_irqsave(&am->lock, flags);
		if (get_kernel_status == KERNEL_STATUS_RUNNING)
			list_del(&p->list);
		spin_unlock_irqrestore(&am->lock, flags);
	#endif
	(void)p;
	BUG_ON(atomic_dec_return(&am->numActiveAllocs) < 0);
}
int kget_num_allocs(void){ return atomic_read(&__aloc_mgr.numActiveAllocs); }
void kget_num_allocs_print(void) {
	struct allocs_manager *am = &__aloc_mgr;
	struct __kmem_canary_prefix *p;
	ulong flags;
	spin_lock_irqsave(&am->lock, flags);
	list_for_each_entry(p, &am->all_allocs_list, list) {
		char **fn_names = backtrace_symbols(&p->fn, 1);
		pr_emerg("Size:%d[b] %s\n", p->alloc_size, fn_names[0]);
		free(fn_names);
	}
	spin_unlock_irqrestore(&am->lock, flags);
}

size_t __kmem_calc_alignment(size_t size) {
	return ((size & PAGE_MASK) == size) ? PAGE_SIZE : 64; // Align to 64 bytes, required by GF asm functions for new AVX.
}

size_t __kmem_prefix_size(size_t size) {
	const size_t allign = __kmem_calc_alignment(size);
	return round_up(sizeof(struct __kmem_canary_prefix), allign);
}

size_t __kmem_calc_alloc_size(size_t size) {
	const size_t prefix = __kmem_prefix_size(size);
	return prefix + size + sizeof(struct __kmem_canary_suffix);
}

void* __kmalloc(size_t size, gfp_t flags, enum allocated_by allocated_by){
	int rv;
	void* raw_alloc = NULL;
	const size_t alignment = __kmem_calc_alignment(size);
	const size_t malloc_size = __kmem_calc_alloc_size(size);

	BUG_ON(size==0);
	if (__kmem_random_failure_occured()){// Random failure testing mechanism
		return NULL;
	}
	if (__aloc_mgr.measure.enabled) {
		#pragma GCC diagnostic push
		#pragma GCC diagnostic ignored "-Wframe-address"
		void* caller[3] = {__builtin_return_address(1), __builtin_return_address(2), __builtin_return_address(3)};
		#pragma GCC diagnostic pop
		char **fn_names = backtrace_symbols(caller, 3);
		pr_emerg("%6d) Size:%8d[b],\t%s:%s:%s\n", __aloc_mgr.measure.n_allocs, (int)size, fn_names[0], fn_names[1], fn_names[2]);
		__aloc_mgr.measure.total_bytes += (int)size;
		__aloc_mgr.measure.n_allocs++;
		free(fn_names);
	}

	rv = posix_memalign(&raw_alloc, alignment, malloc_size); 	// Guards before and after the buffer. +4 size +4 flags
	BUG_ON(!raw_alloc || rv != 0);
	{
		const size_t prefix_size = __kmem_prefix_size(size);
		void* res = raw_alloc + prefix_size;
		struct __kmem_canary_prefix* prefix = res - sizeof(struct __kmem_canary_prefix);
		struct __kmem_canary_suffix* suffix = res + size;

		memset(res, (flags & __GFP_ZERO) ? 0 : 0xFB, size);
		prefix->alloc_size = size;
		prefix->allocated_by = allocated_by;
		array_fill(prefix->overrun_magic, MALLOC_BUF_OVERRUN_MAGIC);
		prefix->flags.all = flags;
		array_fill(suffix->overrun_magic, (u8)MALLOC_BUF_OVERRUN_MAGIC);

		/* Only add to allocs manager if tracking is enabled */
		if (simulator_alloc_tracking_is_enabled()) {
			allocs_manager_add(prefix);
		}

		if (prefix->flags.is_alloced_with_page_struct) {
			int n_pages = (1 << (prefix->flags.page_order));
			int i;
			struct page *pages = (void*)prefix - (sizeof(struct page)*n_pages);
			BUG_ON((void*)pages < raw_alloc);					// Inline array of pages before the prefix
			memset(pages, 0, sizeof(*pages));
			for (i = 0; i < n_pages; i++)
				pages[i].mapped_vaddr = res + (i << PAGE_SHIFT);
		}

		/* Only update memmgr metrics if tracking is enabled */
		if (simulator_alloc_tracking_is_enabled()) {
			nvmesh_memmgr_metric_on_alloc_update(memmgr_total_metrics[allocated_by], size, res);
		}

		return res;
	}
}

void* kmalloc(size_t size, gfp_t flags){
	void *p = __kmalloc(size, flags, AB_CLIENT);
	return p;
}

struct page *virt_to_page(const void* user_addr) {
	const struct __kmem_canary_prefix* prefix = user_addr - sizeof(struct __kmem_canary_prefix);
	const int n_pages = (1 << (prefix->flags.page_order));
	struct page *pages = (void*)prefix - (sizeof(struct page)*n_pages);
	if (prefix->flags.is_alloced_with_page_struct) {
		return (void*)pages;
	} else {
		return NULL;							// Page not mapped. Allocated with kmalloc()
	}
}

// Linker symbols defined in the linker script
extern char _etext; // End of the text segment
extern char _edata; // End of the data segment
extern char _end; // End of the BSS segment

// fast program that checks if the pointer is in the global memory segments
// if a "percpu" parameter is allocated on the global memory, we can be sure
//  it was not allocates with "percpu" allocator, and thus there is no "dynamic"
//  memory allocator header.
static bool __is_global(void *ptr)
{
	// Check if the pointer is within the .data or .bss segments
	if (ptr >= (void *)&_etext && ptr < (void *)&_edata) {
		return true; // Pointer is in the .data segment (global)
	}
	if (ptr >= (void *)&_edata && ptr < (void *)&_end) {
		return true; // Pointer is in the .bss segment (global)
	}
	return false; // Pointer is not in global segments
}

// to indicate that the memory is allocated "per_cpu" we use the 1st bit of the pointer
// the code assumes that kmalloc (or malloc in user space) always returns, at least, 8 byte aligned pointer
void __percpu *__alloc_percpu(size_t size, size_t align) {
	void *ptr;
	ptr = kzalloc(nr_cpu_ids * round_up(size, align), 0);
	BUG_ON((uintptr_t)ptr & PER_CPU_DYN_MEM);
	if (!ptr)
		goto out;
	ptr = (void *)((uintptr_t)ptr | PER_CPU_DYN_MEM);
out:
	return ptr;
}

void __percpu *__alloc_percpu_gfp(size_t size, size_t align, gfp_t gfp) {
	(void)gfp;
	return __alloc_percpu(size, align);
}

void free_percpu(void __percpu *ptr) {
	void *real_ptr;
	if (ptr) {
		BUG_ON(!((uintptr_t)ptr & PER_CPU_DYN_MEM));
		real_ptr = (void *)((uintptr_t)ptr & ~PER_CPU_DYN_MEM);
		BUG_ON(!real_ptr);
		kfree(real_ptr);
	}
}

bool is_kmalloc_percpu(void __percpu *ptr) {
	// Check if the pointer is a per-cpu pointer
	return ptr && !__is_global(ptr) && ((uintptr_t)ptr & PER_CPU_DYN_MEM);
}

void *per_cpu_kalloc_ptr(void __percpu *ptr, int cpu) {
	void *real_ptr;
	size_t allocated_size;
	BUG_ON(!ptr);
	BUG_ON(cpu < 0 || cpu >= nr_cpu_ids);
	BUG_ON(!((uintptr_t)ptr & PER_CPU_DYN_MEM));	// Must be 1 aligned
	real_ptr = (void *)((uintptr_t)ptr & ~PER_CPU_DYN_MEM);
	BUG_ON(!real_ptr);
	allocated_size = ksize(real_ptr) / nr_cpu_ids;
	return (char *)real_ptr + (cpu * allocated_size);
}

static void handle_allocated_by_mismatch(struct __kmem_canary_prefix* prefix, u32 freed_by)
{
	BUG_ON(prefix->allocated_by != freed_by);
	//printf("allocation source mismatch allocated by %x freed by %x size %u\n", prefix->allocated_by, freed_by, prefix->alloc_size);
	//fflush(stdout);
}

static void _kvfree(void *user_addr, gfp_t _f_flags, u32 freed_by) {
	if (user_addr!= NULL) {
		const union t_malloc_flags f_flags = {.all = _f_flags};
		struct __kmem_canary_prefix* prefix = user_addr - sizeof(struct __kmem_canary_prefix);
		struct __kmem_canary_suffix* suffix = user_addr + prefix->alloc_size;
		BUG_ON(array_find_if(prefix->overrun_magic, __kmem_was_canary_prefix_hurt));
		BUG_ON(array_find_if(suffix->overrun_magic, __kmem_was_canary_suffix_hurt));
		BUG_ON(prefix->flags.was_already_freed);
		BUG_ON(prefix->flags.page_order     != f_flags.page_order);
		BUG_ON(prefix->flags.is_virtual_mem != f_flags.is_virtual_mem);	// kmalloc+vfree or vmalloc+kfree
		prefix->flags.was_already_freed = true;

		if (prefix->allocated_by != freed_by)
			handle_allocated_by_mismatch(prefix, freed_by);

		/* Only update memmgr metrics if tracking is enabled */
		if (simulator_alloc_tracking_is_enabled()) {
			nvmesh_memmgr_metric_on_free_update(memmgr_total_metrics[prefix->allocated_by], prefix->alloc_size);
			allocs_manager_del(prefix);
		}

		memset(user_addr, 0xFA, prefix->alloc_size);
		free(user_addr - __kmem_prefix_size(prefix->alloc_size));
	} else {/* It is legal to call kernel free on NULL. don't warn on that*/}
}

inline unsigned long __get_free_pages(gfp_t _gfp_mask, unsigned int order){
	union t_malloc_flags f = {.all = _gfp_mask};
	f.page_order = order;
	f.is_alloced_with_page_struct = 1;
	if (order >= 8)
		return 0ULL; // 256 pages at once is the allowed maximum. In real kernel limit is even smaller
	return (u64)kmalloc(PAGE_SIZE*(1<<order), f.all); /* not used by simulator */
}

inline void free_pages(unsigned long addr, unsigned int order) {
	union t_malloc_flags f = {.all = 0};
	f.page_order = order;
	_kvfree((void*)addr, f.all, AB_CLIENT); /* not used by simulator */
}

struct page *alloc_pages(gfp_t flags, unsigned int order) {
#if MEM_ALLOCATION_SIMULATE_RANDOM_FRAGMENTATION
	if (order > __kmem_update_and_get_random_max_alloc_order()) {
		pr_info("Random fragmentation invoked...\n");
		return NULL;
	}
#endif
	u64 user_addr = __get_free_pages(flags, order);
	if (user_addr)
		return virt_to_page((void*)user_addr);
	else
		return NULL;
}

void nvmeib_split_page(struct page *page, unsigned int order)
{
	u32 i, n_pages = (1 << order);
	u8 *mem;
	for (i = 0, mem = page->mapped_vaddr; i < n_pages; i++, mem += PAGE_SIZE) {
		page[i].split._head = i+10000;
		page[i].mapped_vaddr = mem;
	}
	page->split.is_split = 1;
	page->split.n_refs = n_pages;
	page->split.order = order;
}

bool get_user_pages_fast_fail_generator_enabled = 0;

int get_user_pages_fast(unsigned long start, int nr_pages, int write, struct page **pages) {
	static int fail_generator = 0;
	int i, rv = (get_user_pages_fast_fail_generator_enabled && ((++fail_generator) & 0x100)) ? (nr_pages+1)/2 : nr_pages;			// Simulate failure of getting all user pages (get only half)
	void* virt_aligned = ((void*)(start & PAGE_MASK));
	struct page *page = virt_to_page(virt_aligned);
	BUG_ON(nr_pages < 1);
	for (i = 0; i < rv; i++, page++) {
		pages[i] = page;
		page->split.is_pinned = true;
		page->mapped_vaddr = (void*)(virt_aligned + (i<<PAGE_SHIFT));
	}
	BUG_ON(!write);
	return rv;
}
void put_page(struct page *page) {
	BUG_ON(page->split.is_pinned != true);
	page->split.is_pinned = false;
}

void __free_pages(struct page *page, unsigned int order) {
	free_pages((unsigned long)page->mapped_vaddr, order);
}

void __free_page(struct page *page) {
	if (page->split._head) {
		struct page *head = &page[-page->split._head+10000];
		BUG_ON((head->split.n_refs <= 0)||(!head->split.is_split));
		if (--head->split.n_refs == 0)
			__free_pages(head, head->split.order);
	} else {
		__free_pages(page, 0);
	}
}

void *kzalloc(size_t size, gfp_t flags){
	return kmalloc(size, flags | __GFP_ZERO);
}

void *kcalloc(size_t n, size_t size, gfp_t flags){
	return kmalloc((n*size), flags | __GFP_ZERO);
}

void *vzalloc(size_t size) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = f.do_zero = 1;
	return kmalloc(size, f.all);
}
void *vmalloc(size_t size) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = 1;
	return kmalloc(size, f.all);
}

void* sim_kmalloc(size_t size, gfp_t flags){
	void *p = __kmalloc(size, flags, AB_SIMULATOR);
	return p;
}

void *sim_kzalloc(size_t size, gfp_t flags){
	return sim_kmalloc(size, flags | __GFP_ZERO);
}

void *sim_kcalloc(size_t n, size_t size, gfp_t flags){
	return sim_kmalloc((n*size), flags | __GFP_ZERO);
}

void *sim_vzalloc(size_t size) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = f.do_zero = 1;
	return sim_kmalloc(size, f.all);
}
void *sim_vmalloc(size_t size) {
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = 1;
	return sim_kmalloc(size, f.all);
}

void __dump_mem_alloc_for_debug(void *user_addr){
	struct __kmem_canary_prefix* prefix = user_addr - sizeof(struct __kmem_canary_prefix);
	const size_t malloc_size = __kmem_calc_alloc_size(prefix->alloc_size);
	u8* malloc_addr = user_addr - __kmem_prefix_size(prefix->alloc_size);
	for (size_t i=0; i < malloc_size; i++) {
		pr_crit(" %x ", (unsigned char)(malloc_addr[i]));
	}
	pr_crit("\n");
}

void kfree(const void *addr){
	_kvfree((void*)addr, 0, AB_CLIENT);
}

void vfree(const void *addr){
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = 1;
	_kvfree((void*)addr, f.all, AB_CLIENT);
}

void sim_kfree(const void *addr){
	_kvfree((void*)addr, 0, AB_SIMULATOR);
}

void sim_vfree(const void *addr){
	union t_malloc_flags f = {.all = 0};
	f.is_virtual_mem = 1;
	_kvfree((void*)addr, f.all, AB_SIMULATOR);
}

size_t ksize(const void *user_addr){
	const struct __kmem_canary_prefix* prefix = user_addr - sizeof(struct __kmem_canary_prefix);
	BUG_ON(!user_addr);
	return prefix->alloc_size;
}

int is_vmalloc_addr(const void *user_addr){
	if (user_addr) {
		const struct __kmem_canary_prefix* prefix = user_addr - sizeof(struct __kmem_canary_prefix);
		return prefix->flags.is_virtual_mem;
	}
	return 0;
}

void *krealloc(const void *addr, size_t size, gfp_t flags){
	void *res = NULL;
	if (addr==NULL)
		return NULL;
	res = kmalloc(size, flags);
	memcpy(res,addr,ksize(addr));
	kfree(addr);
	return res;
}

#define MEM_ALLOCATION_SIMULATE_RANDOM_FRAGMENTATION (0)

#if MEM_ALLOCATION_SIMULATE_RANDOM_FRAGMENTATION

#define RANDOM_FRAGMENTATION_MAX_MAX_ORDER (10 + (NVMEIBC_SECTOR_SHIFT - PAGE_SHIFT))
#define RANDOM_FRAGMENTATION_MIN_MAX_ORDER (NVMEIBC_SECTOR_SHIFT - PAGE_SHIFT)

static inline unsigned int __kmem_update_and_get_random_max_alloc_order(void)
{
	static int max_alloc_order = -1;
	int delta = (rand() % 5) - 2;	// [-2..2]
	if (max_alloc_order == -1)
		max_alloc_order = RANDOM_FRAGMENTATION_MIN_MAX_ORDER + (rand() % ((RANDOM_FRAGMENTATION_MAX_MAX_ORDER + 1) - RANDOM_FRAGMENTATION_MIN_MAX_ORDER));
	max_alloc_order += delta;
	if (max_alloc_order < RANDOM_FRAGMENTATION_MIN_MAX_ORDER)
		max_alloc_order = RANDOM_FRAGMENTATION_MIN_MAX_ORDER;
	else if (max_alloc_order > RANDOM_FRAGMENTATION_MAX_MAX_ORDER)
		max_alloc_order = RANDOM_FRAGMENTATION_MAX_MAX_ORDER;
	return max_alloc_order;
}

#endif

/* Callbacks for detection of malloc corruption (if using libc built-in functionality)*/
#include <mcheck.h>
static void __malloc_check(enum mcheck_status err){
	switch (err) {
	case MCHECK_DISABLED: pr_crit("MEM Corruption: Disabled\n"); break;
	case MCHECK_OK      : pr_crit("MEM Corruption: Block is fine\n"); break;
	case MCHECK_FREE 	: pr_crit("MEM Corruption: Block freed twice\n"); break;
	case MCHECK_HEAD 	: pr_crit("MEM Corruption: Memory before the block was clobbered\n"); break;
	case MCHECK_TAIL 	: pr_crit("MEM Corruption: Memory after the block was clobbered\n"); break;
	default:;
	}
	BUG();
}

/********************************* Symbols *************************************/
unsigned long kallsyms_lookup_name(const char *name)
{
	BUG_ON(strcmp(name, "kthread_data") != 0);	// single usage for now.
	return (unsigned long)kthread_data;
}

#include "../common/compat/kr_incs_data_structs.inc.c"

// Taken from: https://github.com/alibaba/LVS/blob/master/kernel/lib/list_sort.c
/* Returns a list organized in an intermediate format suited to chaining of merge() calls: null-terminated, no reserved or sentinel head node, "prev" links not maintained. */
static struct list_head *merge(void *priv, int (*cmp)(void *priv, struct list_head *a, struct list_head *b), struct list_head *a, struct list_head *b){
	struct list_head head, *tail = &head;
	while (a && b) { 								/* if equal, take 'a' -- important for sort stability */
		if ((*cmp)(priv, a, b) <= 0) { 	tail->next = a; a = a->next;}
		else { 							tail->next = b; b = b->next;}
		tail = tail->next;
	}
	tail->next = a?:b;
	return head.next;
}

/* Combine final list merge with restoration of standard doubly-linked list structure.  This approach duplicates code from merge(), but runs faster than the tidier alternatives of either a separate final prev-link restoration pass, or maintaining the prev links throughout. */
static void merge_and_restore_back_links(void *priv, int (*cmp)(void *priv, struct list_head *a, struct list_head *b), struct list_head *head, struct list_head *a, struct list_head *b) {
	struct list_head *tail = head;
	while (a && b) {								/* if equal, take 'a' -- important for sort stability */
		if ((*cmp)(priv, a, b) <= 0) { 	tail->next = a; a->prev = tail; a = a->next; }
		else {							tail->next = b; b->prev = tail; b = b->next; }
		tail = tail->next;
	}
	tail->next = a ? : b;

	do {/* In worst cases this loop may run many iterations. Continue callbacks to the client even though no element comparison is needed, so the client's cmp() routine can invoke cond_resched() periodically. */
		(*cmp)(priv, tail, tail);
		tail->next->prev = tail;
		tail = tail->next;
	} while (tail->next);
	tail->next = head;
	head->prev = tail;
}

void list_sort(void *priv, struct list_head *head, int (*cmp)(void *priv, struct list_head *a, struct list_head *b)){
	#define MAX_LIST_LENGTH_BITS 20
	struct list_head *part[MAX_LIST_LENGTH_BITS+1]; /* sorted partial lists-- last slot is a sentinel */
	int lev /* index into part[] */, part_size = ARRAY_SIZE(part)-1;
	int max_lev = 0;
	struct list_head *list;
	if (list_empty(head))
		return;
	memset(part, 0, sizeof(part));
	head->prev->next = NULL;
	list = head->next;
	while (list) {
		struct list_head *cur = list;
		list = list->next;
		cur->next = NULL;
		for (lev = 0; part[lev]; lev++) {
			cur = merge(priv, cmp, part[lev], cur);
			part[lev] = NULL;
		}
		if (lev > max_lev) {
			if (unlikely(lev >= part_size)) { //printk_once(KERN_DEBUG "list passed to list_sort() too long for efficiency\n");
				lev--;
			}
			max_lev = lev;
		}
		part[lev] = cur;
	}

	for (lev = 0; lev < max_lev; lev++)
		if (part[lev])
			list = merge(priv, cmp, part[lev], list);
	merge_and_restore_back_links(priv, cmp, head, part[max_lev], list);
}

/********************************* HList *************************************/
void __hlist_del(struct hlist_node *n) {
	struct hlist_node *next = n->next, **pprev = n->pprev;
	*pprev = next;
	if (next)
		next->pprev = pprev;
}

void hlist_del(struct hlist_node *n) {
	__hlist_del(n);
	n->next = LIST_POISON1;
	n->pprev = LIST_POISON2;
}

void hlist_del_init(struct hlist_node *n){
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		INIT_HLIST_NODE(n);
	}
}

void hlist_del_init_rcu(struct hlist_node *n)
{
	if (!hlist_unhashed(n)) {
		__hlist_del(n);
		n->pprev = NULL;
	}
}

/******************* Emulation of Linux locks and mutexes ********************/
/*
 * every thread is a kthread apart from the main() - hence main() canno use this API (and has no need to do).
 * so main() gets a dummy task_struct, so it can use traces.
 */
struct task_struct* get_current(void){
	if (kthread_self_task) {
		return kthread_self_task;
	} else {
		static struct task_struct cur_task = {.os_id=0, .start_func=NULL, .parameters=NULL, .pid=0, .should_stop=0, .is_sleeping=0, .wq=NULL, .flags=0, .cpu_idx=0};
		pthread_spin_init(&cur_task.lock, PTHREAD_PROCESS_PRIVATE);
		BUG_ON(pthread_self() != main_os_id);
		cur_task.pid = pthread_self();
		strcpy(cur_task.threadName, "threadname");
		return &cur_task;
	}
}

int spin_lock_init(spinlock_t *l){
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	return pthread_spin_init(&l->m, PTHREAD_PROCESS_SHARED );
#else
	return pthread_mutex_init(&l->m, NULL);
#endif
#ifdef DEBUG_SPINLOCKS
	l->locked = false;
	{int i; for (i = 0; i < (int)ARRAY_SIZE(l->last_bt); l->last_bt[i++] = NULL);}
	l->last_locker = "Nobody ever locked this";
#endif
}

int spin_lock_destroy(spinlock_t *l){
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	return pthread_spin_destroy(&l->m);
#else
	return pthread_mutex_destroy(&l->m);
#endif
#ifdef DEBUG_SPINLOCKS
	l->locked = -1;
#endif
}

int spin_is_locked(const spinlock_t *l){
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	return (l->m!=PTHREAD_PROCESS_SHARED);
#else
	if (pthread_mutex_trylock((pthread_mutex_t *)&l->m)) {
		return true;
	}
	pthread_mutex_unlock((pthread_mutex_t *)&l->m);
	return false;
#endif
}

void spin_unlock(spinlock_t *l){
#ifdef DEBUG_SPINLOCKS
	BUG_ON(l->locked == -1); // Not destroyed
#endif
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	pthread_spin_unlock(&l->m);
#else
	pthread_mutex_unlock(&l->m);
#endif
#ifdef DEBUG_SPINLOCKS
	l->locked = false;
#endif
}

void spin_lock(spinlock_t *l){
#ifdef DEBUG_SPINLOCKS
	BUG_ON(l->locked == -1); // Not destroyed
#endif
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	pthread_spin_lock(&l->m);
#else
	pthread_mutex_lock(&l->m);
#endif
#ifdef DEBUG_SPINLOCKS
	{int i; for (i = 0; i < (int)ARRAY_SIZE(l->last_bt); l->last_bt[i++] = NULL);}
	l->locked = backtrace(l->last_bt, ARRAY_SIZE(l->last_bt));
	l->last_locker = get_current()->comm;
#endif
}

int spin_trylock(spinlock_t *l){
	int res;
#ifdef DEBUG_SPINLOCKS
	BUG_ON(l->locked == -1); // Not destroyed
#endif
#ifndef _USE_SPIN_LOCK_AS_MUTEX
	res = pthread_spin_trylock(&l->m);
#else
	res = pthread_mutex_trylock(&l->m);
#endif
#ifdef DEBUG_SPINLOCKS
	if (res) {
		{int i; for (i = 0; i < (int)ARRAY_SIZE(l->last_bt); l->last_bt[i++] = NULL);}
		l->locked = backtrace(l->last_bt, ARRAY_SIZE(l->last_bt));
		l->last_locker = get_current()->comm;
	}
#endif
	return res;
}

int spin_lock_irqsave_(spinlock_t *l, unsigned long *flags){
	raw_local_irq_save(flags);
	spin_lock(l);
	return 0;
}

int spin_unlock_irqre_(spinlock_t *l, unsigned long *flags){
	raw_local_irq_restore(*flags);
	spin_unlock(l);
	return 0;
}

// No need to implement this since there are no interrupts. All callbacks run as threads
//DEFINE_MUTEX(local_irq);

static struct cpu_prop *kernel_sim_getmy_cpu(void)
{
	int		cpu_idx = task_processor_id(kthread_self_task);
	return kernel_sim.smp.cpu + cpu_idx;
}

static void cpu_ownership_acquire(void)
{
	int					cpu_idx = task_processor_id(kthread_self_task);
	struct cpu_prop 	*cpu = kernel_sim.smp.cpu + cpu_idx;
	int 				rv = pthread_mutex_lock(&cpu->cpu_owner);
	BUG_ON(rv != 0);
}

static void cpu_ownership_release(struct cpu_prop *cpu)
{
	int rv = pthread_mutex_unlock(&cpu->cpu_owner);
	BUG_ON(rv != 0);
}

void raw_local_irq_save(unsigned long *f){
	struct cpu_prop	*cpu = kernel_sim_getmy_cpu();

	// TODO(EBA): need preempt disable when our threads will be rescheduled on cpu's
	cpu_ownership_acquire();
	mutex_lock(&cpu->access_lock);          // we serialize access althoug linux allow get_cpu() & this to run concurently.
#ifdef LOG_IRQ_SAVE_RESTORE
	{
		struct irq_change_log_entry	*change = &cpu->irq_mod.change_log[cpu->irq_mod.next_entry];
		cpu->irq_mod.next_entry = (cpu->irq_mod.next_entry + 1) % IRQ_CHANGE_LOG_SIZE;
		change->is_save = true;
		change->before = cpu->irq_flags;
		change->after = 0;
		change->numptr = backtrace(change->stack, IRQ_CHANGE_LOG_MAX_STACK_DEPTH);
	}
#endif
	BUG_ON(cpu->irq_save_count > KERNEL_SIM_CPU_IRQ_SAVE_MAX_RECURSION);
	cpu->irq_save_count++;
	*f = cpu->irq_flags;
	cpu->irq_flags = 0xD;
	mutex_unlock(&cpu->access_lock);
}
void raw_local_irq_restore(unsigned long f){
	struct cpu_prop	*cpu = kernel_sim_getmy_cpu();

	mutex_lock(&cpu->access_lock);		// we serialize access althoug linux allow get_cpu() & this to run concurently.
#ifdef LOG_IRQ_SAVE_RESTORE
	{
		struct irq_change_log_entry	*change = &cpu->irq_mod.change_log[cpu->irq_mod.next_entry];
		cpu->irq_mod.next_entry = (cpu->irq_mod.next_entry + 1) % IRQ_CHANGE_LOG_SIZE;
		change->is_save = false;
		change->before = cpu->irq_flags;
		change->after = f;
		change->numptr = backtrace(change->stack, IRQ_CHANGE_LOG_MAX_STACK_DEPTH);
	}
#endif
	BUG_ON(cpu->irq_save_count <= 0);
	cpu->irq_save_count--;
	cpu->irq_flags = f;
	mutex_unlock(&cpu->access_lock);
	cpu_ownership_release(cpu);
	// TODO(EBA): need preempt enable ???
}

bool irqs_disabled(void) {
	struct cpu_prop	*cpu = kernel_sim_getmy_cpu();
	ulong irq_flags;
	mutex_lock(&cpu->access_lock);
	irq_flags = cpu->irq_flags;
	mutex_unlock(&cpu->access_lock);
	return (irq_flags == 0xD);
}

void crash_if_irqs_disabled(void) {
	struct cpu_prop	*cpu = kernel_sim_getmy_cpu();
	mutex_lock(&cpu->access_lock);
	if (cpu->irq_flags == 0xD) {
		dump_stack(); pr_emerg("************************** BUG!!!! at %s, %s() line %d - Instant crash, look at stack of other threads\n", __kget_curr_time_stamp(), __FUNCTION__, __LINE__);
		BREAKPOINT(false);		// Instant crash without releasing mutex to be able to debug the other task which tries to do irq_enable()
	}
	mutex_unlock(&cpu->access_lock);
}

struct cpu_prop cpu_prop_take_debug_snapshot(void) {
	struct cpu_prop rv;
	struct cpu_prop	*cpu = kernel_sim_getmy_cpu();
	mutex_lock(&cpu->access_lock);
	rv = *cpu;
	mutex_unlock(&cpu->access_lock);
	return rv;
}

/*********************** Multi-threading issues *****************************/
// OS scheduler and wait queue for threads
#define MAX_NUM_KTHREADS           (CONFIG_NR_CPUS-1)	// Must not be greatet than CONFIG_NR_CPUS or else kthread will not be able to execute per_cpu() code
/*
 * This represent all of the simulated kernel task_struct objects.
 * make sure to lock the object for add/remove/lookup of tasks
 */
struct kernel_tasks {
	struct task_struct *tasks[MAX_NUM_KTHREADS];	// the set of tasks that can be running at any given time
	struct mutex 		lock;						// Insert/remove/lookup the lsit
	int					task_count;
#ifdef SCHED_NEW_KTHREAD_ROUND_ROBIN
	int					percpu_next;				// iterator to assign kthreads to "cpu" on which we consider them scheduled
#endif
} kernel_tasks;


static void kernel_tasks_init(void)
{
	memset(&kernel_tasks.tasks, 0, sizeof(kernel_tasks.tasks));
	mutex_init(&kernel_tasks.lock);
}

static void kernel_tasks_destroy(void)
{
	int	i;

	for (i=0; i< MAX_NUM_KTHREADS; i++) {
		BUG_ON(kernel_tasks.tasks[i]);
	}
	BUG_ON(kernel_tasks.task_count != 0);
	mutex_destroy(&kernel_tasks.lock);
}

// assign a "cpu" in round robin for newly created kthread.
static inline int tasks_assign_to_cpu(void)
{
#ifdef SCHED_NEW_KTHREAD_ROUND_ROBIN
	int  idx = kernel_tasks.percpu_next;
	kernel_tasks.percpu_next = (kernel_tasks.percpu_next + 1) % CONFIG_NR_CPUS;
	return idx;
#else
	// put the thread on a cpu that isnt running any thread
	bool	is_cpu_used[CONFIG_NR_CPUS];
	int  	cpu_idx, i;

	memset(is_cpu_used, 0, sizeof(is_cpu_used));
	for (i=0; i<MAX_NUM_KTHREADS ; i++) {
		if (kernel_tasks.tasks[i] == NULL)
			continue;
		cpu_idx = kernel_tasks.tasks[i]->cpu_idx;
		BUG_ON(cpu_idx >= CONFIG_NR_CPUS);
		is_cpu_used[cpu_idx] = true;
	}
	for (i=0; i<CONFIG_NR_CPUS ;i++) {
		if (is_cpu_used[i] == false) {
			return i;
		}
	}
	BUG_ON(true);	// we dont support 2 thead on a cpu yet
	return 0;
#endif
}

static int tasks_task_to_index(struct task_struct* t)
{
	int i, res = -1;
	for (i=0; i<MAX_NUM_KTHREADS; i++) {
		if (kernel_tasks.tasks[i] == t){
			res = i;
			break;
		}
	}
	return res;
}

/*
 * set a new task attributes, allocating a free position
 * returns index of new task
 */
static int tasks_set_new(struct task_struct* task)
{
	int i;

	mutex_lock(&kernel_tasks.lock);
	for (i = 0; (i<MAX_NUM_KTHREADS) && kernel_tasks.tasks[i]; i++);
	BUG_ON(i==MAX_NUM_KTHREADS);	// make sure we found an empty slot
	kernel_tasks.tasks[i] = task;
	kernel_tasks.task_count++;
	//pr_crit("kthread created: idx=%d, name=%s\n", i, task->threadName);
	mutex_unlock(&kernel_tasks.lock);
	return i;
}

// free the task entry used by the given task
static void	tasks_free(struct task_struct* task)
{
	int i;

	mutex_lock(&kernel_tasks.lock);
	i = tasks_task_to_index(task);
	//pr_crit("kthread terminates: idx=%d, name=%s\n", i, task->threadName);
	kernel_tasks.tasks[i] = NULL;
	kernel_tasks.task_count--;
	mutex_unlock(&kernel_tasks.lock);
}

void __put_task_struct(struct task_struct *task){
	tasks_free(task);
	pthread_spin_destroy(&task->lock);
	sim_kfree(task);
}

static struct task_struct* tasks_thread_to_task(pthread_t os_id)
{
	struct task_struct *res = NULL;
	int i;
	mutex_lock(&kernel_tasks.lock);
	for (i=0; i<MAX_NUM_KTHREADS; i++) {
		if ((kernel_tasks.tasks[i]) && (kernel_tasks.tasks[i]->os_id == os_id)){
			res = kernel_tasks.tasks[i];
			break;
		}
	}
	mutex_unlock(&kernel_tasks.lock);
	BUG_ON(res == NULL);
	return res;
}

struct task_struct* tasks_self(void)
{
	if (0) { // Obsolete, exhaustive linear search in tasks list
		struct task_struct		*my_task;
		const pthread_t my_os_id 		= pthread_self();
		my_task = tasks_thread_to_task(my_os_id);
		BUG_ON(kthread_self_task != my_task);
		return my_task;
	} else {
		return kthread_self_task;
	}
}

int mutex_is_locked(struct mutex*mutex) {
	int rv = pthread_mutex_trylock(&mutex->m);
	if (rv == 1) {
		pthread_mutex_unlock(&mutex->m);
		return false;
	} else {
		return true;
	}
}

/* Finds first task by the name. Insert the result of the function to continue to the next task with the same name */
static struct task_struct* tasks_find_next_task_by_thread_name(const char*threadName, u32 *start)
{
	struct task_struct *res = NULL;
	int i;
	BUG_ON(*start > MAX_NUM_KTHREADS); // last call will be at MAX
	mutex_lock(&kernel_tasks.lock);
	for (i=*start; i<MAX_NUM_KTHREADS; i++) {
		if ((kernel_tasks.tasks[i]) && (kernel_tasks.tasks[i]->threadName == threadName)){
			res = kernel_tasks.tasks[i];
			*start = i+1;
			break;
		}
	}
	mutex_unlock(&kernel_tasks.lock);
	return res;
}

// This is thread-local-storage for every thread that maintains the task_struct.
__thread struct task_struct	*kthread_self_task;

void *kthread_data(struct task_struct *task) {
	return task->parameters;	// same data argument that was passed to the kthread start function. in Linux this is to_kthread(task)->data
}
// This is the starting point of all simulated kthreads.
static void *kthread_start(void* p)
{
	struct task_struct *task = (struct task_struct*)p;
	int		rv __attribute__((__unused__));

	kthread_self_task = task;
	pthread_setname_np(pthread_self(), task->threadName);
	down(&task->sem_sleep); // wait till we are allowed to execute
	rv = task->start_func(task->parameters);
	//pr_crit("kthread exits: start_func=%p, exit_code=%d\n", task->start_func, rv);
	put_task_struct(task);		// Put coz running thread finished
	return NULL;
}

struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...)
{
	struct task_struct *task = (struct task_struct*)sim_kzalloc(sizeof(*task),0);
	va_list	args;

	(void)node;
	if (!task)
		return NULL;
	sema_init(&task->sem_sleep, 0); // make the thread block on this
	pthread_spin_init(&task->lock, PTHREAD_PROCESS_PRIVATE);
	va_start(args, namefmt);
	vsnprintf(task->threadName, sizeof(task->threadName), namefmt, args);
	va_end(args);
	task->start_func = threadfn;
	task->parameters = data;
	task->pid		 = 0;
	atomic_set(&task->usage, 2);	// 2 Refs. -1 upon thread finish, -1 upon join by caller
	task->should_stop= false;
	task->wq		 = NULL;
	task->cpu_idx = tasks_assign_to_cpu();
	tasks_set_new(task);	// add task *before* task is created, so the new task finds its task_struct !!!
	if (!pthread_create(&task->os_id, NULL, kthread_start, task)){
		task->pid = (unsigned int)(task->os_id&0xffffffff);
		return task; 			// Thread was created, return the task
	}
	tasks_free(task); // Unable to launch thread, free it (no need to put() it as no one will join this thread
	return NULL;
}

void kthread_bind(struct task_struct *task, unsigned int cpu)
{
#ifdef SCHED_NEW_KTHREAD_ROUND_ROBIN
	task->cpu_idx = cpu;
#else
	// Simulator non-RR scheduler doesn't support assigning more than 2 threads to a CPU, so this is a no-op.
	(void)task; (void)cpu;
#endif
}

static void __signal_suicide(wait_queue_head_t *q);
int kthread_stop(struct task_struct *task) {
	wait_queue_head_t *q;
	void* res = NULL;
	int		rv;

	if ((task->os_id==0)||(task==NULL)) { /* Handle the incorrect behaviour of current() */
		task = tasks_self();
		BUG_ON(!task);
	}
	BUG_ON(atomic_read(&task->usage) == (int)0xfafafafa);	// catch in case the thread has already terminated & freed its task_struct
	BUG_ON((atomic_read(&task->usage) < 1) || (atomic_read(&task->usage) > 3));	// 1 = Thread altready terminated, waits fot join, 2 = Thread running, 3 = kth lib took additional reference
	task_struct_set_should_stop(task);	// Notify unblocked thread of their death
	q = task->wq;						// Important: Cache on stack as 'task' can wakeup (task->wq == NULL) and even terminate
	if (q) {							// Here if (task->wq != 0) task->wq == q.
		//pr_crit("signaling thread: wq=%p, twq=%p\n", q, task->wq);
		__signal_suicide(q);	// Wake up blocked threads by signalling suicide value
	}
	//pr_crit("Stopping thread %s....\n", task->threadName);
	//pthread_kill(task->os_id, SIGQUIT);	// Daniel: _join sometimes does not work (system hangs). Can't solve it...
	rv = pthread_join(task->os_id, &res);
	WARN(rv, "Cannot join thread, rv=%d,  deadlock=%d\n", rv, EDEADLK);
	put_task_struct(task);					// Put coz it was joined
	return 0;
}

bool kthread_wait_for_paused(const char*threadName){
	u32 task_idx = 0;
	const struct task_struct*my_task = tasks_find_next_task_by_thread_name(threadName, &task_idx);
	BUG_ON(!my_task);
	while (my_task) {
		if (!my_task->wq)
			continue;
		while ((my_task->wq->lock.r>0)||(!task_struct_is_sleeping(my_task)))
			msleep(1);	// Ugly code. Should replace by signalling upon completion, not half-busy waiting.
		my_task = tasks_find_next_task_by_thread_name(threadName, &task_idx);
	}
	return false;
}

bool kthread_should_stop(void){
	const struct task_struct* my_task 	= tasks_self();
	BUG_ON(!my_task);
	return task_struct_is_should_stop(my_task);
}

static struct task_struct* __kthread_attach_to_wait_queue(wait_queue_head_t *q){
	struct task_struct* my_task 		= tasks_self();
	BUG_ON(!my_task);
	task_struct_attach_wait_queue(my_task, q);
	return my_task;
}

static void syncVar_init(syncVar* _this){
	pthread_mutex_init(&_this->m, NULL);		// Initialize the mutex
	pthread_cond_init( &_this->c, NULL);		// Todo: Check for errors on return
	_this->r = 0;								// Resource by default is set to zero.
}

/*static void syncVar_destroy(syncVar* _this){
	pthread_mutex_destroy(&_this->m);			// Destroy the mutex
	pthread_cond_destroy( &_this->c);			// Destroy the communication mechanism
	_this->r = 0;
}*/

// You must have the lock before you call wait
static int syncVar_wait(syncVar* _this){				// Todo: Check for errors on return
	return pthread_cond_wait(&_this->c, &_this->m);
}

/*
 * @timeout is in jiffies
*/
static int syncVar_wait_timeout(syncVar* _this, long timeout)
{				// Todo: Check for errors on return
	struct timespec 	end_time;
	clock_gettime(CLOCK_REALTIME, &end_time);
	end_time.tv_nsec += NSEC_PER_MSEC * jiffies_to_msecs(timeout);
	if (end_time.tv_nsec >= NSEC_PER_SEC) {
		end_time.tv_sec += end_time.tv_nsec / NSEC_PER_SEC;
		end_time.tv_nsec = end_time.tv_nsec % NSEC_PER_SEC;
	}
	return pthread_cond_timedwait(&_this->c, &_this->m, &end_time);
}


static void syncVar_signal(syncVar* _this, bool toAll){	// Announce to other threads that resource value was changed. By default (broadcasts, notifies everyone). If FALSE signals to only one thread (or few)
	if (toAll) pthread_cond_broadcast(&_this->c);
	else       pthread_cond_signal(   &_this->c);
}

// Say that everyone responded to the original signal(). Not needed under linux, but needed in windows.
/*static void syncVar_stopSignaling(syncVar* _this){		// Todo: Check for errors on return
}*/

void syncVar_lock(syncVar* _this){
	pthread_mutex_lock(&_this->m);
}

void syncVar_unlock(syncVar* _this){
	pthread_mutex_unlock(&_this->m);
}

void init_waitqueue_head(wait_queue_head_t* q){
	syncVar_init(  &q->lock);
	INIT_LIST_HEAD(&q->task_list);
}

void wake_up(wait_queue_head_t* q){
	syncVar_lock(&q->lock);
	q->lock.r++;										// Signal that there is a new work to be done
	syncVar_unlock(&q->lock);							// Try to wake up only one thread, beacuse only one task was added (do not bradcast to all).
	syncVar_signal(&q->lock, false);					// No need to hold the lock. The method which added new task to the queue already did it under lock and released it. No wake up threads to acquire lock and work
}

static void __signal_suicide(wait_queue_head_t *q){ 	// Very much like wake_up() but for wakes up to be killed
	syncVar_lock(&q->lock);
	q->lock.r = -9999;									// Wakeup with special value of resource. Very large negative number so if other threads do wake_up() they will not offset it to 0
	syncVar_unlock(&q->lock);
	syncVar_signal(&q->lock, true);						// Bradcast to all threads.
}

int __wait_event_interruptible(wait_queue_head_t *q){
	struct task_struct* task = NULL;
	syncVar_lock(&q->lock);								// Lock the tasks resource
	// All previous work was terminated. Wait for new work to arrive. q->lock.r==0 means no new work arrived
	// We need to put wait() in a loop for two reasons: 1. There can be spurious wakeups (due to signal/ENITR). 2. When mutex is released for waiting, another thread can be waken up from a signal/broadcast and that thread can mess up the condition. So when the current thread wakes up the condition may no longer be actually true!
	while (q->lock.r==0){ 								// Also means list_empty(&q->task_list))
		//pr_crit("waiting on queue %p....\n",q);
		task = __kthread_attach_to_wait_queue(q);
		task_struct_set_sleeping(task, true);
		syncVar_wait(&q->lock);							// Wait until there is a task in the queue/ Unlock mutex while wait, then lock it back when signaled
		task->wq = NULL;								// clear ASAP, though its still not atomic
		task_struct_set_sleeping(task, false);
	}
	// q->lock.r==0 - woke up to no work. >0 means new task arrived, <0 means have to quit
	if (q->lock.r>0)
		q->lock.r = 0;									// (Can do r-- instead of 0). All work will be done by us. Do not wake up anyone else.
	syncVar_unlock(&q->lock);
	BUG_ON(task->wq);									// Daniel: Who guarantees that task != NULL
	if (q->lock.r<0)
		return -1;										// Notify that must exit
	return 0;
}

/*
 * wait_event_interruptible_timeout - sleep until a condition gets true or a timeout elapses
 * @wq: the waitqueue to wait on
 * @condition: a C expression for the event to wait for
 * @timeout: timeout, in jiffies
 *
 * return:
 *  0 for signaling
 * -1 for thread exit
 * -ETIMEDOUT for timeout
 */
long __wait_event_interruptible_timeout(wait_queue_head_t *q, unsigned long jiff)
{
	struct task_struct* task;
	long 				rv = 0;

	syncVar_lock(&q->lock);								// Lock the tasks resource
	// All previous work was terminated. Wait for new work to arrive. q->lock.r==0 means no new work arrived
	// We need to put wait() in a loop for two reasons: 1. There can be spurious wakeups (due to signal/ENITR). 2. When mutex is released for waiting, another thread can be waken up from a signal/broadcast and that thread can mess up the condition. So when the current thread wakes up the condition may no longer be actually true!
	while ((q->lock.r==0) && (rv != ETIMEDOUT)) { 								// Also means list_empty(&q->task_list))
		//pr_crit("waiting on wait_queue %p (r=%d, rv=%d)....\n", q, q->lock.r, rv);
		task = __kthread_attach_to_wait_queue(q);
		task_struct_set_sleeping(task, true);
		rv = syncVar_wait_timeout(&q->lock, jiff);							// Wait until there is a task in the queue/ Unlock mutex while wait, then lock it back when signaled
		task->wq = NULL;													// clear ASAP, though its still not atomic
		switch (rv) {
		case 0: // success, i.e.: signaled. if we consume the token then were out
			break;
		case ETIMEDOUT:// timedout - return
			break;
		case ENOTRECOVERABLE:
		case EOWNERDEAD:
		case EPERM:
		case EINVAL:
		default:
			BUG_ON(1);
		}
		task_struct_set_sleeping(task, false);
	}
	// q->lock.r==0 - woke up to no work. >0 means new task arrived, <0 means have to quit
	if (q->lock.r>0)
		q->lock.r = 0;									// (Can do r-- instead of 0). All work will be done by us. Do not wake up anyone else.
	syncVar_unlock(&q->lock);
	if (q->lock.r<0)
		return -1;										// Notify that must exit
	if (rv == ETIMEDOUT) {
		return -ETIMEDOUT;
	}
	return 0;
}

// Insertnew tasks
/*static void __add_task(wait_queue_head_t *q){
	syncVar_lock(&q->lock);
    // Insert stud into '&q->task_list' here
	q->lock.r++;
	syncVar_unlock(&q->lock);
	wake_up(q);								// Try to wake up only one thread, beacuse only one task was added (do not bradcast to all).
}
*/

/******************* Emulation of percpu ********************/
// we emulate it by:
// 1) declaring the number of cpu's as a compile-time macro
// 2) assigning a "cpu" to a kthread upon creation.
// 3) creating a lock per cpu & having the lock acquired/release upon get/put cpu, to allow only one kthread to execute the critical section of percpu on the cpu it is executing.
// in the future, we can change the assignment of task_struct to cpu, when the state indicate that the getcpu() critical section hasnt been taken.
int nr_cpu_ids = CONFIG_NR_CPUS;



static inline void percpu_prop_init(struct percpu_prop		*per_cpu)
{
	memset(per_cpu, 0, sizeof(*per_cpu));
}
static inline void percpu_prop_destroy(struct percpu_prop		*per_cpu)
{
	(void)per_cpu;
}

void smp_init(struct smp_cpu_sim	*pc)
{
	struct cpu_prop			*cpu;
	pthread_mutexattr_t 	mtx_attr;
	int		i, rv;

	for (cpu=pc->cpu, i=0; i< CONFIG_NR_CPUS; i++, cpu++) {
		cpu->irq_flags = 0xE;	// Of this single CPU. 0xE = Enabled, 0xD = disabled
		cpu->irq_save_count = 0;
#ifdef LOG_IRQ_SAVE_RESTORE
		memset(&cpu->irq_mod, 0, sizeof(cpu->irq_mod));
#endif
		mutex_init(&cpu->access_lock);
		percpu_prop_init(&cpu->per_cpu);
		rv = pthread_mutexattr_init(&mtx_attr);										BUG_ON(rv != 0);
		rv = pthread_mutexattr_settype(&mtx_attr, PTHREAD_MUTEX_RECURSIVE_NP);		BUG_ON(rv != 0);
		rv = pthread_mutex_init(&cpu->cpu_owner, &mtx_attr);						BUG_ON(rv != 0);
	}
}

static void smp_destroy(struct smp_cpu_sim	*pc)
{
	struct cpu_prop			*cpu;
	int		i;

	for (cpu=pc->cpu, i=0; i< CONFIG_NR_CPUS; i++, cpu++) {
		BUG_ON(cpu->irq_save_count != 0);
		mutex_destroy(&cpu->access_lock);
		percpu_prop_destroy(&cpu->per_cpu);
		pthread_mutex_destroy(&cpu->cpu_owner);
	}
}

#ifdef USE_SMP_PER_CPU
// Since we simulate a single CPU each thread which accesses per CPU variables must be serialized usaing the below mutex
int get_cpu()
{
	int					cpu_idx;
	struct task_struct* my_task = kthread_self_task;

	BUG_ON(my_task->flags & TASK_STRUCT_FLAG_GETCPU); // catch recursive get_cpu() - not supported yet
	cpu_ownership_acquire();
	cpu_idx = task_get_cpu(my_task);
#ifdef TASK_STRUCT_SAVE_GETCPU_STACK
	// save my stack trace now that i've acquired the cpu
	{
		struct cpu_prop		*cpu = kernel_sim.smp.cpu + cpu_idx;
		struct percpu_prop	*per_cpu = &cpu->per_cpu;
		int		i, nptrs;
		char	**symbols;
		nptrs = backtrace(per_cpu->getcpu_stack, PERCPU_MAX_STACK_DEPTH);
        symbols = backtrace_symbols(per_cpu->getcpu_stack, nptrs);
		for (i=0; i<nptrs;i++) {
			strlcpy(per_cpu->stack_symbols[i], symbols[i]+PERCPU_STACK_SYMBOL_OFFSET, sizeof(per_cpu->stack_symbols[0]));
		}
		free(symbols);
	}
#endif // TASK_STRUCT_SAVE_GETCPU_STACK
	return cpu_idx;
}

void put_cpu()
{
	struct smp_cpu_sim	*smp = &kernel_sim.smp;
	struct task_struct* my_task = kthread_self_task;
	struct cpu_prop		*cpu;
	int		cpu_idx;

	cpu_idx = task_put_cpu(my_task);
	cpu = smp->cpu + cpu_idx;
#ifdef TASK_STRUCT_SAVE_GETCPU_STACK
	memset(cpu->per_cpu.getcpu_stack, 0, sizeof(cpu->per_cpu.getcpu_stack));
#endif
	cpu_ownership_release(cpu);
}

// HACK: For each CPU, spoof the current task's cpu_idx and execute directly. Warning, protects against concurrent access to percpu by func() only if the task running on another CPU uses get_cpu()
int on_each_cpu(smp_call_func_t func, void *info, int wait)
{
	struct task_struct* my_task = kthread_self_task;
	int my_cpu_idx = my_task->cpu_idx;
	int i;

	for_each_online_cpu(i) {
		struct smp_cpu_sim	*smp = &kernel_sim.smp;
		struct cpu_prop	*cpu = smp->cpu + i;

		my_task->cpu_idx = i;
		cpu_ownership_acquire();
		(*func)(info);
		cpu_ownership_release(cpu);
	}

	my_task->cpu_idx = my_cpu_idx;

	(void)wait;

	return 0;
}

#else
// Since we simulate a single CPU each thread which accesses per CPU variables must be serialized usaing the below mutex
DEFINE_MUTEX(cpu_access_mutex);
int get_cpu(void){
	mutex_lock(&cpu_access_mutex);
	return 0;
}
void put_cpu(void){
	mutex_unlock(&cpu_access_mutex);
}
#endif


/**************************** Userspace threads ******************************/
/* Short example of the bug in pthread_create21() method. Dettached thread can terminate before the callers pthread_create() ended. In this case the dettached thread cleans its data and pthread_create() causes memory error (wrong access)*/
typedef struct {
	int a, i, sum;
	//spinlock_t l;
} t_sum_params;
/*
static void* func_calc_sum(void* params){
	t_sum_params *p = (t_sum_params*)params;
	//spin_lock(&p->l);
	p->sum = p->a + p->i;
	//spin_unlock(&p->l);
	kfree(params);
	return NULL;
}
void* func_calc_sum_loop(void* unused){
	int i;  (void)unused;
	for (i=0; i<50; i++) {
		pthread_attr_t attr;
		pthread_t tID;
		t_sum_params *p = (t_sum_params*)kmalloc(sizeof(t_sum_params), GFP_KERNEL);
		p->a = 100; p->i = i;
		//spin_lock_init(&p->l);
		//spin_lock(&p->l);
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&tID, &attr, func_calc_sum, p);
		//spin_unlock(&p->l);
		pthread_attr_destroy(&attr);
	}
	return NULL;
}

void* func_calc_sum_loop_loop(void* unused){
	int i;  (void)unused;
	for (i=0; i<2; i++) {
		pthread_attr_t attr;
		pthread_t tID;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&tID, &attr, func_calc_sum_loop, NULL);
		pthread_attr_destroy(&attr);
	}
	return NULL;
}

void* func_calc_sum_loop_loop_loop(void* unused){
	int i;  (void)unused;
	for (i=0; i<2; i++) {
		pthread_attr_t attr;
		pthread_t tID;
		pthread_attr_init(&attr);
		pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
		pthread_create(&tID, &attr, func_calc_sum_loop_loop, NULL);
		pthread_attr_destroy(&attr);
	}
	return NULL;
}*/

/********************************** Time ***********************************/
#ifdef DISABLE_ALL_TRACING		// Framework for jiffies/cycles_khz is defined by tracing
	#include "common/compat/kr_incs_time_jiff.inc.c"
#endif
static char time_stamp_buff[26];					// Return the current request time stamp. Not thread safe! Todo: Fix this nicely
const char* __kget_curr_time_stamp(void){
    time_t t;
    struct tm* tm_info;
    time(&t); tm_info = localtime(&t);
    strftime(time_stamp_buff, sizeof(time_stamp_buff), "%Y:%m:%d %H:%M:%S", tm_info);
    return time_stamp_buff;
}

static void __get_jiffies_start(void) {
	int_cpu_freq_tsc_offset_jiffies();
	pr_crit("jiffies init: cpu_freq=%u[Mhz], loops_per_jiffy=%lu, jiffies/sec=%u, tsc_offset=%llu\n", DIV_ROUND_CLOSEST(tsc_khz, 1000), loops_per_jiffy, HZ, tsc_offset);
	if (0) {		// Internal unitest
		unsigned long j[2];
		j[0] = _jiffies();
		pr_emerg("jiffies=%lu\n", j[0]);
		usleep(3*1000*1000);
		j[1] = _jiffies();
		pr_emerg("j +3sec=%lu ~= %u\n", j[1]-j[0], 3*HZ);
	}
}

/********************************** Timers ***********************************/
#define TIMER_INITIALIZE_CODE 	(void*)0x1717FACE
const char timers_thread_name[] = "timers";
#define TIMER_BG_THREAD_MAX_SLEEP_JIFFIES	(HZ/100)	// The max time the bg thread will sleep. if next timer is further in future, bg will wakeup, check first timer & go to sleep again. This means that adding a new timer can safely skip the costly wakeup of bg thread in case timer is for more than this time


static inline int cpu_timers_get_num_active_timers(struct cpu_timers *timers){
	return atomic_read(&timers->num_active_timers);
}

/* Call, when holding lock */
struct timer_list *cpu_timers_remove_locked(struct cpu_timers	*timers,
											struct timer_list 	*t)
{
	struct rb_node *node = &t->rb_entry;

	rb_erase(node, &timers->rb_waiting_timers);
	node->__rb_parent_color = POISON_POINTER_DELTA;			// Mark that this node is outside of the tree
	atomic_dec(&timers->num_active_timers);
	return t;
}
/* Call, when NOT holding lock */
struct timer_list *cpu_timers_remove(struct cpu_timers	*timers,
									 struct timer_list 	*t)
{
	struct rb_node *node = &t->rb_entry;
	unsigned long 		flags;

	spin_lock_irqsave(&timers->lock, flags);
    // we re-check (under lock) whether timer is still linked to the rbtree.
	if (likely(node->__rb_parent_color != POISON_POINTER_DELTA)) {
		BUG_ON(cpu_timers_remove_locked(timers, t) != t);
	} else {
		t = NULL;
	}
	spin_unlock_irqrestore(&timers->lock, flags);
	return t;
}

static bool cpu_timers_should_drain(struct cpu_timers	*timers){
	return (timers->flags & (TIMER_FLAG_REQ_FORCED_DRAIN | TIMER_FLAG_REQ_DRAIN));
}

static int cpu_timers_execute_timers(void *param){					// Worker thread which executes all the timers
	struct cpu_timers	*timers = (struct cpu_timers*)param;
	unsigned long flags;
	long wait_time;
	struct rb_node *node = NULL;
	int num_active_timers = 0;
	while (!kthread_should_stop()) {
		spin_lock_irqsave(&timers->lock, flags);
		node = rb_first(&timers->rb_waiting_timers);				/* lowest exp time timer or null*/
		wait_time = LONG_MAX;
		if (node) {
			struct timer_list *t = rb_entry(node, struct timer_list, rb_entry);
			wait_time = (long)t->expires-(long)jiffies;
			if ((wait_time < 0) || (timers->flags & TIMER_FLAG_REQ_FORCED_DRAIN)) {
				BUG_ON(cpu_timers_remove_locked(timers, t) != t);
				__concurrent_store(timers->executing, t);
				spin_unlock_irqrestore(&timers->lock, flags);	// unlock before invoking callback
				t->function(t->data); 				// Once this function terminates, 't' may not exist
				__concurrent_store(timers->executing, NULL);
				continue;
			}
		}
		if (cpu_timers_should_drain(timers) && (atomic_read(&timers->num_active_timers) == 0)) {
			timers->flags &= ~(TIMER_FLAG_REQ_FORCED_DRAIN | TIMER_FLAG_REQ_DRAIN);
			timers->flags |= TIMER_FLAG_DRAIN_COMPLETED;
		}
		num_active_timers = atomic_read(&timers->num_active_timers);
		spin_unlock_irqrestore(&timers->lock, flags);
		BUG_ON(wait_time < 0);
		if (wait_time > TIMER_BG_THREAD_MAX_SLEEP_JIFFIES)	// limit sleep time so we can skip wakeup for long timers.
			wait_time = TIMER_BG_THREAD_MAX_SLEEP_JIFFIES;
		wait_event_interruptible_timeout(timers->timers_wait_queue,
										 (num_active_timers != atomic_read(&timers->num_active_timers)) || kthread_should_stop() || cpu_timers_should_drain(timers),
										 wait_time);
	}
	timers->timers_thread = NULL; // poison
	return 0;
}

void cpu_timers_init(struct cpu_timers	*timers,
					int					cpu_id)
{
	spin_lock_init(&timers->lock);
	init_waitqueue_head(&timers->timers_wait_queue);
	timers->rb_waiting_timers = RB_ROOT;
	atomic_set(&timers->num_active_timers, 0);
	atomic_set(&timers->num_dispatched_timers, 0);
	timers->cpu_id = cpu_id;
	timers->flags = 0;
	timers->timers_thread = kthread_run(cpu_timers_execute_timers, timers, "%s/%d", timers_thread_name, cpu_id);
	__concurrent_store(timers->executing, NULL);
}

void cpu_timers_destroy(struct cpu_timers   *timers)
{
	kthread_stop(timers->timers_thread);
}

/* Call, when holding lock. Implement sorting by expiration time */
static void cpu_timers_add(struct cpu_timers	*timers,
						   struct timer_list 	*t)
{
	struct rb_root 	*root = &timers->rb_waiting_timers;
	struct rb_node **link = &root->rb_node, *parent = NULL;
	struct timer_list *cur_t = NULL;
	unsigned long flags;


	t->cpu_id = timers->cpu_id;
	spin_lock_irqsave(&timers->lock, flags);
	if (t->rb_entry.__rb_parent_color != POISON_POINTER_DELTA)
		// BEWARE: we dont support modifying expiration time of active timers.
		goto out;												// Timer already in the tree, No need to re-add it.
	while (*link) {
		parent = *link;
		cur_t = rb_entry(parent, struct timer_list, rb_entry);
		if (time_before_eq(t->expires, cur_t->expires)) link = &parent->rb_left;
		else 											link = &parent->rb_right;
	}
	rb_link_node(&t->rb_entry, parent, link);
	rb_insert_color(&t->rb_entry, root);
	atomic_inc(&timers->num_active_timers);									// Increase amount of elements in the rb tree
	atomic_inc(&timers->num_dispatched_timers);
out:
	if ((t->expires - jiffies) < TIMER_BG_THREAD_MAX_SLEEP_JIFFIES) {	// no need to wakeup the bg thread as it wont sleep for that long
		wake_up(&timers->timers_wait_queue);
	}
	spin_unlock_irqrestore(&timers->lock, flags);
}

int cpu_timers_dump(struct cpu_timers	*timers,
					char				*buf,
					int					size)
{
	//struct rb_node *last_rb_node = rb_last(&timers->rb_waiting_timers);
	struct rb_node *rb_iter;
	struct timer_list *cur_t;
	int	written = 0, bytes;
	unsigned long flags;

	spin_lock_irqsave(&timers->lock, flags);
	bytes = snprintf(buf, size, "timers/%d:num_active_timers=%d, jiffies=%ld", timers->cpu_id, atomic_read(&timers->num_active_timers), jiffies);
	written += bytes; buf += written; size -= written;
	// dump the timers DB content
	rb_iter = rb_first(&timers->rb_waiting_timers);
	if (rb_iter == NULL)
		goto out;
	// loop to process last_tmer !!!
	do {
		cur_t = rb_entry(rb_iter, struct timer_list, rb_entry);
		bytes = snprintf(buf, size, "\nexpires=%ld", cur_t->expires);
		written += bytes; buf += bytes; size -= bytes;
		BUG_ON(size < 0);
		rb_iter = rb_next(rb_iter);
	} while (rb_iter);
out:
	spin_unlock_irqrestore(&timers->lock, flags);
	return written;
}


static void cpu_timers_drain(struct cpu_timers	*timers,
							 bool				force_expiration)
{
	unsigned long 	flags;
	bool			is_done;

	spin_lock_irqsave(&timers->lock, flags);
	timers->flags &= ~TIMER_FLAG_DRAIN_COMPLETED;
	if (force_expiration) {
		timers->flags |= TIMER_FLAG_REQ_FORCED_DRAIN;
	} else {
		timers->flags |= TIMER_FLAG_REQ_DRAIN;
	}
	wake_up(&timers->timers_wait_queue);
	spin_unlock_irqrestore(&timers->lock, flags);

	// wait for the draining
	do {
		spin_lock_irqsave(&timers->lock, flags);
		is_done = (timers->flags & TIMER_FLAG_DRAIN_COMPLETED);
		spin_unlock_irqrestore(&timers->lock, flags);
		if (!is_done) {
			//msleep(10);
			sched_yield();
		}
	} while (!is_done);
	BUG_ON((timers->flags & (TIMER_FLAG_REQ_FORCED_DRAIN | TIMER_FLAG_REQ_DRAIN)) != 0); // should be cleared by execution engine
}

void __init_timer(struct timer_list *t, unsigned int flags) {
	(void)flags;
	memset(t, 0, sizeof(*t));
	t->base = TIMER_INITIALIZE_CODE;				// Daniel Hack to identify initialized timer
	t->rb_entry.__rb_parent_color = POISON_POINTER_DELTA;
}

void kernel_timers_init(struct kernel_timers   *timers)
{
	int i;

	for (i=0; i< MAX_NUM_TIMERS_ENGINE; i++) {
		cpu_timers_init(timers->cpu_timer + i, i);
	}
	timers->next_cpu = 0;
}

void kernel_timers_destroy(struct kernel_timers   *timers)
{
	int i;

	for (i=0; i< MAX_NUM_TIMERS_ENGINE; i++) {
		cpu_timers_destroy(timers->cpu_timer + i);
	}
}

static inline u32 kernel_timers_get_next(struct kernel_timers   *timers)
{
	u32   next = timers->next_cpu % MAX_NUM_TIMERS_ENGINE;
	timers->next_cpu++;
	return next;
}

int get_kernel_num_active_timers(void)
{
	int i, num_active_timers;

	for (num_active_timers=0, i=0; i< MAX_NUM_TIMERS_ENGINE; i++) {
		num_active_timers += cpu_timers_get_num_active_timers(kernel_sim.timers.cpu_timer + i);
	}
	return num_active_timers;
}

static inline void kernel_timers_add(struct kernel_timers   *timers,
									 struct timer_list 		*t)
{
	u32  next_cpu = kernel_timers_get_next(timers);
	cpu_timers_add(timers->cpu_timer + next_cpu, t);
}

// drain all active timers, for testing purposes
void kernel_timers_drain(struct kernel_timers   *timers,
						 bool					force_expiration)
{
	int	i;

	for (i=0; i< MAX_NUM_TIMERS_ENGINE; i++) {
		cpu_timers_drain(timers->cpu_timer + i, force_expiration);
	}
}

char *kernel_timers_dump(struct kernel_timers  	*timers,
						 char					*buf,
						 int					size)
{
	char	*p = buf;
	int 	i, written;

	for (i=0; i< MAX_NUM_TIMERS_ENGINE; i++) {
		written = cpu_timers_dump(timers->cpu_timer + i, p, size);
		p += written;
		*p++ = '\n';
		size -= (written+1);
		BUG_ON(size < 0);
	}
	*p = 0;
	return buf;
}

int mod_timer(struct timer_list *t, unsigned long expires){
	BUG_ON(t->base != TIMER_INITIALIZE_CODE);
	BUG_ON(t->rb_entry.__rb_parent_color != POISON_POINTER_DELTA); // we dont support modifying the expiry - need to remove & add
	t->expires = expires;
	kernel_timers_add(&kernel_sim.timers, t);
	return 0;
}

// return 0 when timer has already expired, 1 when its found & removed from waiting timers
int del_timer(struct timer_list *t){
	return !!(cpu_timers_remove(kernel_sim.timers.cpu_timer + t->cpu_id, t));
}

// return 0 or 1 as from del_timer()
// return 2 when we found the timer callback executing & waited for its completion
int del_timer_sync(struct timer_list *timer)
{
	const struct cpu_timers	*cpu_timer = kernel_sim.timers.cpu_timer + timer->cpu_id;
	int rv = del_timer(timer);	// remove timer from waiting DB (if not processed yet)
	if (unlikely(rv == 0)) {
		// timers wasnt found in DB - it might be executing now.
		while (__concurrent_load(cpu_timer->executing) == timer) {
			// wait for the timer which is now executing.
			pr_debug("wait for timer (%p) cb to complete...\n", cpu_timer->executing);
			sched_yield();
			rv = 2;		// allow tests to know which flow we took
		}
	}
	return rv;
}

int timer_pending(const struct timer_list * timer)
{
	return timer->rb_entry.__rb_parent_color != POISON_POINTER_DELTA;
}

/********************************* Work Queue ********************************/
/* Assume: wq is not NULL and we hold the execution flag.*/
static int __process_workqueue_thread(void* p)
{
	struct workqueue_struct *wq = (struct workqueue_struct *)p;
	struct work_struct work_copy_for_crash_debug __attribute((unused));

	for (; wq->is_destroy == false;) {
		down(&wq->sem);	// wait for work item on the list
		if (unlikely(wq->is_destroy)) {
			goto out;
		}
		mutex_lock(&wq->add_mutex);
		if (!list_empty(&wq->w_list)) {	// for every work-item being canceled, the semaphore has an excess token.
			__concurrent_store(wq->curr_work, list_first_entry(&wq->w_list, struct work_struct, entry));
			list_del_init(&(__concurrent_load(wq->curr_work))->entry);			// Crucial to do it first and unlock the list
		} else
			BUG_ON(__concurrent_load(wq->curr_work) != NULL);
		mutex_unlock(&wq->add_mutex);
		if (__concurrent_load(wq->curr_work) == NULL)
			continue;
		work_copy_for_crash_debug = *(__concurrent_load(wq->curr_work));   // Save copy in case that system crashes during the next statement
		__concurrent_load(wq->curr_work)->func(__concurrent_load(wq->curr_work));				// This and other threads can add tasks to the list during this loop and execution of the work
		// decrement counter now, *AFTER* work was completed, for correctness of drain
		mutex_lock(&wq->add_mutex);
		__concurrent_store(wq->curr_work, NULL);
		wq->num_pending_works--;
		mutex_unlock(&wq->add_mutex);
	}
out:
	return 0;
}

struct workqueue_struct *create_singlethread_workqueue(const char *name){
	struct workqueue_struct* res = (struct workqueue_struct*)sim_kzalloc(sizeof(struct workqueue_struct),0);
	res->name = sim_kstrdup(name, 0);
	INIT_LIST_HEAD(&res->w_list);
	__concurrent_store(res->curr_work, NULL);
	mutex_init(&res->add_mutex);
	sema_init(&res->sem, 0);
	res->num_pending_works = 0;
	res->num_canceled = 0;
	res->is_destroy = false;
	res->worker_task = kthread_run(__process_workqueue_thread, res, name);
	BUG_ON(res->worker_task == NULL);
	get_task_struct(res->worker_task);	// we maintain a ref
	kernel_work_queues_add(res);
	return res;
}

bool __queue_work(int cpu, struct workqueue_struct *wq, struct work_struct *work){
	(void)cpu;
	mutex_lock(&wq->add_mutex);					// Insert new job to tail
	BUG_ON(wq->is_destroy == true);
	list_add_tail(&work->entry, &wq->w_list);
	wq->num_pending_works++;
	mutex_unlock(&wq->add_mutex);
	up(&wq->sem);	// signal worker about new item on the list
	return true;
}

void __queue_delayed_work(int cpu, struct workqueue_struct *wq, struct delayed_work *dwork, unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct work_struct *work = &dwork->work;
	BUG_ON((timer->function != delayed_work_timer_fn) || (timer->data != (unsigned long)dwork) || !list_empty(&work->entry));
	//WARN_ON_ONCE(timer_pending(timer));
	BUG_ON(delay < 0); // we do not support direct execution.
			    //
	mutex_lock(&dwork->simu.lock);
	if (dwork->simu.stop_request) {
		mutex_unlock(&dwork->simu.lock);
		return;
	}
	dwork->wq = wq;
	if (!delay) {
		__queue_work(cpu, wq, &dwork->work);
		mutex_unlock(&dwork->simu.lock);
		return;
	}
	dwork->cpu = cpu;
	timer->expires = jiffies + delay;
#if 0
	if (unlikely(cpu != WORK_CPU_UNBOUND))
			add_timer_on(timer, cpu);
	else
#endif
	add_timer(timer);
	mutex_unlock(&dwork->simu.lock);
}

static bool __cancel_delayed_work_sync(struct delayed_work *dwork)
{
	struct work_struct *work_iter, *work_iter_next;
	struct workqueue_struct *wq;
	int rv;
	bool ret = false;

	mutex_lock(&dwork->simu.lock);
	dwork->simu.stop_request = true;
	wq = dwork->wq;
	rv = del_timer_sync(&dwork->timer);

	if (rv == 1) {
		ret = true; // 1 => timer was found & removed from waiting timers before it expired
		goto out;
	}
	BUG_ON(!(rv == 0 || rv == 2));
	if (!wq) { // no workqueue - meaning the delayed work was never submitted.
		BUG_ON(rv == 2);
		ret = true;
		goto out;
	}
	mutex_lock(&wq->add_mutex);
	list_for_each_entry_safe(work_iter, work_iter_next, &wq->w_list, entry) {
		if (work_iter == &dwork->work) {
			list_del(&work_iter->entry);
			ret = true;
			wq->num_canceled++;
			wq->num_pending_works--;
			break;
		}
	}
	mutex_unlock(&wq->add_mutex);
out:
	mutex_unlock(&dwork->simu.lock);
	BUG_ON(dwork->simu.expect_recursive && !ret);
	BUG_ON(dwork->simu.expect_queued && !ret);
	BUG_ON(dwork->simu.expect_running && ret);
	while (wq && __concurrent_load(wq->curr_work) == &dwork->work) {
		ret = false;
		sched_yield();
	}
	BUG_ON(dwork->simu.expect_recursive && ret);
	BUG_ON(dwork->simu.expect_queued && !ret);
	BUG_ON(dwork->simu.expect_running && ret);
	return ret;
}

bool cancel_delayed_work_sync(struct delayed_work *dwork)
{
	bool ret;
	ret = __cancel_delayed_work_sync(dwork);
	dwork->simu.stop_request = false;
	return ret;
}

struct flush_qent {
	struct work_struct entry;
	struct completion *comp;
};

static void __flush_qent_work_fn(struct work_struct *entry)
{
	struct flush_qent *flush_qent = container_of(entry, struct flush_qent, entry);
	complete(flush_qent->comp);
}


static bool __cancel_and_flush_qent(struct workqueue_struct *wq, struct work_struct *work, bool try_cancel)
{
	DECLARE_COMPLETION_ONSTACK(comp);
	struct flush_qent flush_qent = {
		.entry = {
			.func = __flush_qent_work_fn,
		},
		.comp = &comp,
	};
	bool wait_comp = false;
	bool ret = false;

	mutex_lock(&wq->add_mutex);
	if (wq->curr_work == work) {
		/* Currently running our entry. Put the flush work at the head of the list */
		list_add(&flush_qent.entry.entry, &wq->w_list);
		wq->num_pending_works++;
		wait_comp = true;
		ret = true;
	} else {
		struct work_struct *work_iter;
		/* Check if work is in the list. If so, either remove it (try_cancel = true) or put the flush work after it */
		list_for_each_entry(work_iter, &wq->w_list, entry) {
			if (work_iter == work) {
				if (try_cancel) {
					list_del(&work_iter->entry);
					wq->num_canceled++;
					wq->num_pending_works--;
				} else {
					list_add(&flush_qent.entry.entry, &work_iter->entry);
					wq->num_pending_works++;
					wait_comp = true;
				}
				ret = true;
				break;
			}
		}
	}
	mutex_unlock(&wq->add_mutex);
	if (wait_comp)
		wait_for_completion(&comp);
	return ret;
}

bool cancel_work(struct workqueue_struct *wq, struct work_struct *work)
{
	return __cancel_and_flush_qent(wq, work, true);
}

bool flush_work(struct workqueue_struct *wq, struct work_struct *work)
{
	return __cancel_and_flush_qent(wq, work, false);
}

bool workqueue_is_empty(struct workqueue_struct *wq) {
	bool res;
	mutex_lock(&wq->add_mutex);
	res = !wq->num_pending_works;
	mutex_unlock(&wq->add_mutex);
	return res;
}

/* Wait until the workqueue becomes empty. While draining is in progress, only chain queueing is allowed. IOW, only currently pending or running work items on wq can queue further work items on it. wq is flushed repeatedly until it becomes empty. The number of flushing is determined by the depth of chaining and should be relatively short. Whine if it takes too long.*/
void drain_workqueue(struct workqueue_struct *wq) {
	int count;
	if (!wq)
		return;
	for (count = 1; true ; count++) {
		if (workqueue_is_empty(wq))
			return;

		schedule();		// Todo: Replace by completion
		WARN_ON((!(count & 0x5ffff))&&(!kernel_sim.drain_workqueue_is_infinite)); /* After ~4[sec] issue warning */
	}
}

void destroy_workqueue(struct workqueue_struct *wq) {
	if (!wq)
		return;
	mutex_lock(&wq->add_mutex);
	if (!list_empty(&wq->w_list))
		BUG_ON(true);				// Should never happen. All works should be drained
	wq->is_destroy = true; 			// signal worker to exit
	mutex_unlock(&wq->add_mutex);
	up(&wq->sem);	// signal worker to let it run & test the termination condition
	kthread_stop(wq->worker_task);		// wait for internal worker to terminate
	put_task_struct(wq->worker_task);	// loose our ref
	//sem_destroy(wq->sem);
	kernel_work_queues_remove(wq);
	mutex_destroy(&wq->add_mutex);
	sim_kfree(wq->name);
	sim_kfree(wq);
}

void delayed_work_timer_fn(unsigned long __data)
{
	struct delayed_work *dwork = (struct delayed_work *)__data;

	/* should have been called from irqsafe timer with irq already off */
	__queue_work(dwork->cpu, dwork->wq, &dwork->work);
}

// dump the state of the work-queue
void workqueue_dump(struct workqueue_struct *wq, struct nvmeib_txt* txt, bool	add_items) {
	struct list_head	*iter;
	void	*func[1];
	char	**symbols;
	mutex_lock(&wq->add_mutex);

	nvmeib_txt_append(txt, "wq = {name=%-20s, pending=%d}", wq->name, wq->num_pending_works);
	if (add_items) {
		list_for_each(iter, &wq->w_list) {
			struct work_struct	*wi = container_of(iter, struct work_struct, entry);
			// resolve function address to symbol
			func[0] = wi->func;
			symbols = backtrace_symbols(func,1);
			nvmeib_txt_append(txt, "\n\tfunc=%p (%s)", wi->func, symbols[0]);
			free(symbols);
		}
	}
	mutex_unlock(&wq->add_mutex);
}

void workqueue_dump_works_to_log(struct workqueue_struct *wq, void (*print_fn)(const struct work_struct *)) {
	struct list_head *cur;
	mutex_lock(&wq->add_mutex);
	if (wq->num_pending_works) {
		pr_emerg("WQ %s: n_works=%d\n", wq->name, wq->num_pending_works);
		list_for_each(cur, &wq->w_list) {
			print_fn(container_of(cur, struct work_struct, entry));
		}
	}
	mutex_unlock(&wq->add_mutex);
}

void flush_workqueue(struct workqueue_struct *wq) { (void)wq;
	BUG();		/* Not implemented yet! can use drain_workqueue() as first approximation. Maybe it is good enough */
}

typedef struct {
	eCPU_thread_internal_params;
	struct work_struct *w;
} t_async_dummy_sync_params;

static eCPU_cb_ret_type __async_dummy_sync(eCPU_cb_param_list) {
	t_async_dummy_sync_params *p = eCPU_thread_extract_param(t_async_dummy_sync_params);
	eCPU_thread_start_execution(p);
	p->w->func(p->w);
	eCPU_thread_end_execution(p);
}

void schedule_work_on_sys_wq_rand_cpu(struct work_struct *work) {
	eCPU_thread_prepare(t_async_dummy_sync_params, p);
	p->w = work;
	eCPU_thread_launch(__async_dummy_sync, p, false);
}

static int down_timeout(struct semaphore *sem, long timeout)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_REALTIME, &ts) == -1)
	    return -1;
	ts.tv_sec += timeout / HZ;
	return sem_timedwait(&sem->sem, &ts);
}

void reup(    struct completion *c) {
	BUG_ON(!c->done);
	up(&c->wait);
}
void complete(    struct completion *c){
	BUG_ON(c->done);
	c->done = true;
	up(&c->wait);
}

void complete_all(struct completion *c){
	(void)c; BUG();
}

void wait_for_completion(struct completion *c){
	down(&c->wait);
}

int wait_for_completion_timeout(struct completion *c, unsigned long timeout){
	if (down_timeout(&c->wait, timeout) < 0)
		return 0;	// timed out
	return timeout;	// completed, return time left (simulate immediately completed)
}


void completion_verify_not_waiting(struct completion *c){
	int wait_value = 0;
	sem_getvalue(&c->wait.sem, &wait_value);
	BUG_ON(wait_value != 0);
}

bool completion_done(struct completion *x){
	return x->done;
}

struct workqueue_struct *system_wq         = NULL;
struct workqueue_struct *system_long_wq    = NULL;	/* Do not allocate it! It is GPL, must not use it */
struct workqueue_struct *system_unbound_wq = NULL;	/* Do not allocate it! It is GPL, must not use it */

void __init_kernel_workqueues(void){
	system_wq = create_singlethread_workqueue("system_wq");
}

void __destroy_kernel_workqueues(void){
	destroy_workqueue(system_wq);
	system_wq = NULL;
}

/**************************** request_queue simulator *****************************/
static void blk_release_queue(struct kobject *kobj){
	struct request_queue *q = container_of(kobj, struct request_queue, kobj);
	mutex_destroy(&q->sysfs_lock);
	sim_kfree(q);
}

static const struct sysfs_ops queue_sysfs_ops = {
	.show   = NULL, // queue_attr_show,
	.store  = NULL, // queue_attr_store,
};

struct kobj_type blk_queue_ktype = {
	.release        = blk_release_queue,
	.sysfs_ops      = &queue_sysfs_ops,
	.default_attrs  = NULL, //default_attrs,
};

struct request_queue *blk_alloc_queue(gfp_t f)
{
	struct request_queue *q;
	(void)f;
	q = (struct request_queue *)sim_kzalloc(sizeof(struct request_queue),f);
	kobject_init(&q->kobj, &blk_queue_ktype);
	mutex_init(&q->sysfs_lock);
	return q;
}

/**************************** gendisk simulator *****************************/
static int __disk_name_to_index(const char *name) {
	const char *next = name, *s = NULL;
	while (next) {
		s = next;
		next = strstr(s+1, "_0"); // Need to find the last _0%d
	}
	BUG_ON((s[0] != '_')||!isdigit(s[1]));			// You forgot to add _X into bdev name
	return (int)simple_strtoul(s+1, NULL, 10);		// Unique conversion ov block device name to hash table entry (assuming each block device name terminates with unique number)
}

void add_disk(struct gendisk *disk){						// Note: the kernel currently holds the first reference form disk creation
	static int free_minor = 0;
	struct osSimulator *sim = osSimulator_getCurrent();
	//void* dev = (???*)disk->queue->queuedata;				// Don't use this as we don't know if this is virtual bdev or not
	const int n_max_disks = ARRAY_SIZE(sim->disks);
	int devUniqueHash = __disk_name_to_index(disk->disk_name);
	struct block_device* bds = &sim->bds[devUniqueHash];
	ulong *q_flags = &disk->queue->queue_flags;
	BUG_ON(devUniqueHash>=n_max_disks);
	mutex_lock(&sim->disks_lock);
	disk->kernel_ptr = (void*)sim;
	if (disk->flags	& GENHD_FL_EXT_DEVT) {
		disk->first_minor = free_minor++;
	} else {
		BUG_ON((disk->minors == 0) || (disk->first_minor == 0));	// Externally allocated
	}
	sim->disks[devUniqueHash] = disk;
	bds->bd_disk              = disk; 					// don't use dev->os.disk
	WARN_ON_ONCE(!blk_get_queue(disk->queue));			// Like in real kernel gen-disk holds reference counter to request queue
	WARN_ONCE(test_bit(QUEUE_FLAG_REGISTERED, q_flags), "%s is registering an already registered queue\n", disk->disk_name);
	set_bit(QUEUE_FLAG_REGISTERED, q_flags);
	atomic_set(&bds->n_active_ios, 0);		// No IO's awaiting completion
	MAX_WITH(sim->nVolumes, devUniqueHash+1);	// +1 since the counting starts from 0
	if (disk->nr_sects) {						// Simulate that gendisk blocks when capacity is not zero, by trying to read partition. http://stackoverflow.com/questions/13518404/add-disk-hangs-on-insmod
		mutex_lock(  &kernel_sim.add_gendisk_lock);
		mutex_unlock(&kernel_sim.add_gendisk_lock);
	}
	disk_to_dev(disk) = bds;
	disk->vol_i = devUniqueHash;
	mutex_unlock(&sim->disks_lock);
	return;
}

void set_capacity(struct gendisk *disk, sector_t size){
	disk->nr_sects = size;
}

sector_t get_capacity(struct gendisk *disk){
	return disk->nr_sects;
}

int revalidate_disk(struct gendisk *disk){
	(void)disk;
	return 0;
}
void del_gendisk(struct gendisk *disk){
	int	attempts, refcount;
	disk->nr_sects = 0;
	mutex_lock(&disk->lock);
	disk->is_closing = true;	// indicate that no new references will be given
	if (disk->queue) clear_bit(QUEUE_FLAG_REGISTERED, &disk->queue->queue_flags);
	mutex_unlock(&disk->lock);
	// No IO should be executing on the device (but open/close/ioctl is possible)
	for (attempts = 1; atomic_read(&disk_to_dev(disk)->n_active_ios) != 0; attempts ++) {
		if (attempts % 100 == 0) {
			refcount = atomic_read(&disk->kobj.kref.refcount);
			pr_warn("%s: disk %s taking too long %d\n", __func__, disk->disk_name, refcount);
		}
		sched_yield();
		msleep(1);						// Wait for other to close
		BUG_ON(attempts > 500);
	}
}

struct kobject *get_disk(struct gendisk *disk)
{
	kref_get(&disk->kobj.kref);
	return &disk->kobj;
}

/* the reference-counter of "struct gendisk" is used to monitor it as well as
 * the "struct request_queue". so both are freed at the same time. */
static void __gendisk_free(struct kref	*kref)
{
	struct kobject *kobj = container_of(kref, struct kobject, kref);
	struct gendisk *disk = container_of(kobj, struct gendisk, kobj);
	struct osSimulator *sim = (struct osSimulator *)disk->kernel_ptr;
	pr_debug("freeing disk %s(%p): (queue=%p), q.ref=%d\n", disk->disk_name, disk, disk->queue, disk->queue->kobj.kref.refcount.c);
	blk_put_queue(disk->queue);
	BUG_ON(osSimulator_diskfree(sim, disk) != 0);
	mutex_destroy(&disk->lock);
	kfree(disk);
}
void put_disk(struct gendisk *disk){
	//pr_crit("disk_put: %d\n", atomic_read(&disk->kobj.kref.refcount));
	kref_put(&disk->kobj.kref, __gendisk_free);
}

struct gendisk *alloc_disk(int minors){
	struct gendisk *res = kzalloc(sizeof(*res), 0);
	res->minors = minors;
	res->is_closing = false;
	kobject_init(&res->kobj, NULL);
	mutex_init(&res->lock);
	return res;
}

int blkdev_reread_part(struct block_device *bdev){
	int res;
	(void)bdev;
	//mutex_lock(&bdev->bd_mutex);
	res = 0; // Daniel: Partitions not supported yet. __blkdev_reread_part(bdev);
	//mutex_unlock(&bdev->bd_mutex);
	return res;
}

int ioctl_by_bdev(struct block_device *bdev, unsigned cmd, unsigned long arg){
	(void)arg;
	switch (cmd) {
	case BLKRRPART: return blkdev_reread_part(bdev);
	default: BUG();
	}
	return 0;
}

struct block_device *lookup_bdev(const char *path) {
	BUG(); (void)path; return NULL;		// Dont use, use the function below instead
}

/* here i take reference of gendisk bcz we in fact need to take a ref to the struct block_device and it should have taken a ref of gendisk (and probably request_queue).
 * the same for its pair API blkdev_put() which closes the device only to return the gendisk reference, bcz we have no reference for the struct block_device*/
struct block_device *blkdev_get_by_path(const char *path, fmode_t mode, void *holder){
	struct osSimulator *sim = osSimulator_getCurrent();
	int i, n_max_disks = ARRAY_SIZE(sim->disks);
	struct block_device *bdev = NULL;
	(void)holder;
	if (path[0]=='/')
		path += 5; // Skip "/dev/"
	mutex_lock(&sim->disks_lock);
	for (i = 0; i < n_max_disks; i++) {
		if ((sim->disks[i])&&(!strcmp(sim->disks[i]->disk_name,path))){
			break;
		}
	}
	mutex_unlock(&sim->disks_lock);
	if (i < n_max_disks) {							// If bdev found
		int rv = osSimulator_diskOpenIdx(sim, i, true, mode, &bdev);
		BUG_ON((rv == 0) && (bdev == NULL));
	}
	return bdev;
}

struct block_device *bdget_disk(struct gendisk *disk, int partno){
	struct osSimulator *sim = osSimulator_getCurrent();
	int i, n_max_disks = ARRAY_SIZE(sim->disks);
	struct block_device *bdev = NULL;
	(void)partno;			// Not supported yet
	mutex_lock(&sim->disks_lock);
	for (i=0; i<n_max_disks; i++) {
		if (sim->disks[i] == disk){
			bdev = &sim->bds[i];
			break;
		}
	}
	mutex_unlock(&sim->disks_lock);
	return bdev;
}

int blkdev_put(struct block_device *bdev, fmode_t mode){
	struct osSimulator *sim = osSimulator_getCurrent();
	bdev->bd_disk->fops->release(bdev->bd_disk, mode);
	return osSimulator_diskCloseIdx(sim, (int)(bdev-sim->bds));
}

int do_vfs_ioctl(struct file *filp, unsigned int fd, unsigned int cmd, unsigned long arg){
	struct gendisk *disk = (struct gendisk *)filp;				// Daniel, ugly! block device is found by name
	struct osSimulator *sim = osSimulator_getCurrent();
	int i, rv = -ENODEV, n_max_disks = ARRAY_SIZE(sim->disks);
	struct block_device *bdev = NULL;
	(void)fd;
	mutex_lock(&sim->disks_lock);
	for (i = 0; i < n_max_disks; i++) {
		if (sim->disks[i] == disk){
			bdev = &sim->bds[i];
			break;
		}
	}
	mutex_unlock(&sim->disks_lock);
	if (bdev) {
		if (!bdev->bd_disk->fops->ioctl)
			return -ENOTTY;
		rv = bdev->bd_disk->fops->ioctl(bdev, FMODE_READ, cmd, arg);
	}
	return rv;
}

/*********************** Linux 3.14+ newer bio iterator API *******************/
// Copied from: https://elixir.bootlin.com/linux/v4.0/source/include/linux/bio.h#L73
#define __bvec_iter_bvec(bvec, iter)	(&(bvec)[(iter).bi_idx])
#define bvec_iter_len(bvec, iter)	min((iter).bi_size, __bvec_iter_bvec((bvec), (iter))->bv_len - (iter).bi_bvec_done)

#define bvec_iter_page(bvec, iter)    (__bvec_iter_bvec((bvec), (iter))->bv_page)
#define bvec_iter_offset(bvec, iter)  (__bvec_iter_bvec((bvec), (iter))->bv_offset + (iter).bi_bvec_done)

#define bvec_iter_bvec(bvec, iter) ((struct bio_vec) {						\
	.bv_page	= bvec_iter_page((bvec), (iter)),	\
	.bv_len		= bvec_iter_len((bvec), (iter)),	\
	.bv_offset	= bvec_iter_offset((bvec), (iter)),	\
})

static inline void bvec_iter_advance(struct bio_vec *bv, struct bvec_iter *iter, unsigned bytes) {
	WARN_ONCE(bytes > iter->bi_size, "Attempted to advance past end of bvec iter\n");
	while (bytes) {
		unsigned x1 = bvec_iter_len(bv, *iter);
		unsigned len = min(bytes, x1);
		bytes -= len;
		iter->bi_size -= len;
		iter->bi_bvec_done += len;

		if (iter->bi_bvec_done == __bvec_iter_bvec(bv, *iter)->bv_len) {
			iter->bi_bvec_done = 0;
			iter->bi_idx++;
		}
	}
}

void __bio_clone_fast(struct bio *dst, struct bio *src) {
	dst->bi_bdev = src->bi_bdev;
	__SET_BI_RW(dst, src->bi_rw);
	dst->bi_flags = src->bi_flags;
#if KS_BVEC_ITER
	dst->bi_iter.bi_idx	= src->bi_iter.bi_idx;
#else
	dst->bi_idx	= src->bi_idx;
#endif
}

void bio_advance_iter(struct bio *bio, struct bvec_iter *iter, unsigned bytes) {
	// TODO - use KERNEL_SECTOR_SHIFT here instead of 9 - include order is problematic
	iter->bi_sector += bytes >> 9;
	if (0) //bio->bi_rw & BIO_NO_ADVANCE_ITER_MASK)
		iter->bi_size -= bytes;
	else
		bvec_iter_advance(bio->bi_io_vec, iter, bytes);
}

struct bio_vec bio_iter_iovec(struct bio *bio, struct bvec_iter iter) {
	return bvec_iter_bvec(bio->bi_io_vec, iter);
}

/***************************** IO related types ******************************/
void bio_endio(struct bio *b, int rv){
	b->bi_bdev->status_of_last_io = rv;
	wmb();	// esnure UT sees **EVERYTHING** this IO has updated, before we mark the IO as completed
	atomic_dec(&b->bi_bdev->n_active_ios);
	__bio_track_end(b, rv);
	// decrease the gendisk ref-count to prevent it from being freed while IO's are in-flight
	put_disk(b->bi_bdev->bd_disk);
	if (b->comp)							// (1 << BIO_USER_MAPPED)
		complete(b->comp);
	if (b->bi_flags & (1 << BIO_OWNS_VEC)) // Free physical memory if we own it
		sim_kfree(b->bi_io_vec[0].bv_page[0].mapped_vaddr);
	sim_kfree(b);
}

static void __bio_init_ker4_9(struct bio *bio) {
	memset(bio, 0, sizeof(*bio));
	//bio->bi_flags = 1 << BIO_UPTODATE;
	atomic_set(&bio->__bi_cnt, 1);
}

static void __bio_init_ker4_10(struct bio *bio, struct bio_vec *table, unsigned short max_vecs) {
	__bio_init_ker4_9(bio);
	// atomic_set(&bio->__bi_remaining, 1);
	bio->bi_io_vec = table;
	bio->bi_max_vecs = max_vecs;
}

#if KS_BIO_INIT_WITH_BVEC
	void bio_init(struct bio *bio, struct bio_vec *table, unsigned short max_vecs) { __bio_init_ker4_10(bio, table, max_vecs); }
#else
	void bio_init(struct bio *bio) { __bio_init_ker4_9(bio); }
#endif

unsigned bio_list_size(const struct bio_list *bl) {
	u32 sz = 0;
	struct bio *bio;
	bio_list_for_each(bio, bl) sz++;
	return sz;
}

void bio_list_add(struct bio_list *bl, struct bio *bio) {
	bio->bi_next = NULL;
	if (bl->tail)	bl->tail->bi_next = bio;
	else			bl->head =          bio;
	bl->tail = bio;
}

void bio_list_add_head(struct bio_list *bl, struct bio *bio) {
	bio->bi_next = bl->head;
	bl->head = bio;
	if (!bl->tail)
		bl->tail = bio;
}

void bio_list_merge(struct bio_list *bl, struct bio_list *bl2) {
	if (!bl2->head)
		return;
	if (bl->tail) bl->tail->bi_next = bl2->head;
	else          bl->head =          bl2->head;
	bl->tail = bl2->tail;
}

void bio_list_merge_head(struct bio_list *bl, struct bio_list *bl2) {
	if (!bl2->head)
		return;

	if (bl->head) bl2->tail->bi_next = bl->head;
	else          bl->tail =           bl2->tail;
	bl->head = bl2->head;
}

struct bio *bio_list_pop(struct bio_list *bl){
	struct bio *bio = bl->head;
	if (bio) {
		bl->head = bl->head->bi_next;
		if (!bl->head)
			bl->tail = NULL;
		bio->bi_next = NULL;
	}
	return bio;
}

struct bio *bio_list_get(struct bio_list *bl){
	struct bio *bio = bl->head;
	bl->head = bl->tail = NULL;
	return bio;
}
void bio_put(struct bio *bio) {
	/*if (!bio_flagged(bio, BIO_REFFED))
		bio_free(bio);
	else*/ {
		if (atomic_dec_and_test(&bio->__bi_cnt))
			bio_free(bio);
	}
}

// Note - page allocation should be done by the caller using page_alloc()
//
#if KS_BIO_ALLOC_HAS_BLOCK_DEVICE
struct bio *bio_kmalloc(unsigned short nr_iovecs, gfp_t gfp_mask)
#else
struct bio *bio_kmalloc(gfp_t gfp_mask, unsigned short nr_iovecs)
#endif
{
	const size_t neededMem = sizeof(struct bio) + nr_iovecs * (sizeof(struct bio_vec));	// Amount of memory needed for bio + bio_vec
	struct bio* bio = (struct bio*)kzalloc(neededMem, gfp_mask);
	struct bio_vec *vecs  = (struct bio_vec *)(bio+1);				// Memory after struct bio is given to array vecs
	__bio_init_ker4_10(bio, vecs, nr_iovecs);
	return bio;
}

static inline bool bio_full(struct bio *bio){
	return bio->bi_vcnt >= bio->bi_max_vecs;
}

static inline bool bio_flagged(struct bio *bio, unsigned int bit) {
	return (bio->bi_flags & (1U << bit)) != 0;
}

int bio_add_pc_page(void *q, struct bio *bio, struct page *page, unsigned int len, unsigned int offset) {
	(void)q;
	return bio_add_page(bio, page, len, offset);
}

int bio_add_page(struct bio *bio, struct page *page, unsigned int len, unsigned int offset) {
	struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];
	WARN_ON_ONCE(bio_flagged(bio, BIO_CLONED));
	WARN_ON_ONCE(bio_full(bio));
	bv->bv_page = page;
	bv->bv_offset = offset;
	bv->bv_len = len;
	bio->bi_iter.bi_size += len;
	bio->bi_size += len;
	bio->bi_vcnt++;
	return len;
}
#include "common/compat/kr_incs_sgl.inc.c"

void __sg_page_iter_start(struct sg_page_iter *piter, struct scatterlist *sglist, unsigned int nents, unsigned long pgoffset) {
	piter->__pg_advance = 0;
	piter->__nents = nents;

	piter->sg = sglist;
	piter->sg_pgoffset = pgoffset;
}

static unsigned sg_page_count(struct scatterlist *sg)
{
	return PAGE_ALIGN(sg->offset + sg->length) >> PAGE_SHIFT;
}

bool __sg_page_iter_next(struct sg_page_iter *piter)
{
	if (!piter->__nents || !piter->sg)
		return false;

	piter->sg_pgoffset += piter->__pg_advance;
	piter->__pg_advance = 1;

	while (piter->sg_pgoffset >= sg_page_count(piter->sg)) {
		piter->sg_pgoffset -= sg_page_count(piter->sg);
		piter->sg = sg_next(piter->sg);
		if (!--piter->__nents || !piter->sg)
			return false;
	}

	return true;
}

void sg_miter_start(struct sg_mapping_iter *miter, struct scatterlist *sgl,
		    unsigned int nents, unsigned int flags)
{
	memset(miter, 0, sizeof(struct sg_mapping_iter));

	__sg_page_iter_start(&miter->piter, sgl, nents, 0);
	WARN_ON(!(flags & (SG_MITER_TO_SG | SG_MITER_FROM_SG)));
	miter->__flags = flags;
}

static bool sg_miter_get_next_page(struct sg_mapping_iter *miter)
{
	if (!miter->__remaining) {
		struct scatterlist *sg;
		unsigned long pgoffset;

		if (!__sg_page_iter_next(&miter->piter))
			return false;

		sg = miter->piter.sg;
		pgoffset = miter->piter.sg_pgoffset;

		miter->__offset = pgoffset ? 0 : sg->offset;
		miter->__remaining = sg->offset + sg->length -
		(pgoffset << PAGE_SHIFT) - miter->__offset;
		miter->__remaining = min_t(unsigned long, miter->__remaining,
					   PAGE_SIZE - miter->__offset);
	}

	return true;
}

bool sg_miter_skip(struct sg_mapping_iter *miter, off_t offset)
{
	sg_miter_stop(miter);

	while (offset) {
		off_t consumed;

		if (!sg_miter_get_next_page(miter))
			return false;

		consumed = min_t(off_t, offset, miter->__remaining);
		miter->__offset += consumed;
		miter->__remaining -= consumed;
		offset -= consumed;
	}

	return true;
}

bool sg_miter_next(struct sg_mapping_iter *miter)
{
	sg_miter_stop(miter);

	/*
	 * Get to the next page if necessary.
	 * __remaining, __offset is adjusted by sg_miter_stop
	 */
	if (!sg_miter_get_next_page(miter))
		return false;

	miter->page = sg_page_iter_page(&miter->piter);
	miter->consumed = miter->length = miter->__remaining;

	miter->addr = page_address(miter->page) + miter->__offset;

	return true;
}

void sg_miter_stop(struct sg_mapping_iter *miter)
{
	WARN_ON(miter->consumed > miter->length);

	/* drop resources from the last iteration */
	if (miter->addr) {
		miter->__offset += miter->consumed;
		miter->__remaining -= miter->consumed;

		miter->page = NULL;
		miter->addr = NULL;
		miter->length = 0;
		miter->consumed = 0;
	}
}

/******************************* linux/proc_fs *******************************/
void proc_root_init(struct proc_dir_entry *r) {
	r->next = r->parent = r->subdir = NULL;
	r->data = NULL;
	r->depth   = 0;
	strlcpy(&r->name[0], "proc", sizeof(r->name));
}

// Returns last child directory or NULL if no children
static struct proc_dir_entry *__proc_get_last_child_of(struct proc_dir_entry *me) {
	struct proc_dir_entry *rv;
	for (rv = me->subdir; (rv)&&(rv->next); rv = rv->next);
	return rv;
}

#define __proc_is_entry_of(e, name) (!strcmp((e)->name, name))

static struct proc_dir_entry *__proc_get_prev_child_of(const char *name, struct proc_dir_entry *parent) {
	struct proc_dir_entry *prev_child = parent->subdir;
	BUG_ON(!prev_child);
	if (__proc_is_entry_of(prev_child, name))
		return NULL;			// name is of the first child
	for (; prev_child->next; prev_child = prev_child->next) {
		if (__proc_is_entry_of(prev_child->next, name))
			return prev_child;
	}
	BUG();	// name does not exists in this directory
	return NULL;
}

static void __proc_add_child(struct proc_dir_entry *child, struct proc_dir_entry *parent) {
	struct proc_dir_entry *last_child;
	spin_lock(&parent->lock);
	last_child = __proc_get_last_child_of(parent);
	// Todo: verify same etry is not addded twice via __proc_get_prev_child_of(child) == NULL
	if (last_child) {
		last_child->next = child;
	} else {
		parent->subdir   = child;
	}
	child->parent = parent;
	child->depth = parent->depth + 1;
	spin_unlock(&parent->lock);
}

static struct proc_dir_entry *__proc_create_entry(const char *name, struct proc_dir_entry *parent) {
	struct proc_dir_entry *res = (struct proc_dir_entry *)sim_kzalloc(sizeof(struct proc_dir_entry), 0);
	strlcpy(res->name, name, sizeof(res->name));
	spin_lock_init(&res->lock);
	if (!parent)
		parent = &kernel_sim.procfs;
	__proc_add_child(res, parent);
	return res;
}

struct proc_dir_entry *proc_mkdir(const char *name, struct proc_dir_entry *parent){
	struct proc_dir_entry *res = __proc_create_entry(name, parent);
	return res;
}

void remove_proc_entry(const char *name, struct proc_dir_entry *parent) {
	struct proc_dir_entry *prev_child, *me;
	if (!parent)
		parent = &kernel_sim.procfs;
	spin_lock(&parent->lock);
	prev_child = __proc_get_prev_child_of(name, parent);
	me = (prev_child ? prev_child->next : parent->subdir);
	BUG_ON(me->subdir);			// Illegal to delete directory before removing the entries
	if (prev_child) {
		prev_child->next = me->next;
	} else {
		parent->subdir = me->next;
	}
	spin_unlock(&parent->lock);
	sim_kfree(me);
	return;
}

struct proc_dir_entry *proc_create_data(const char *name, umode_t mode, struct proc_dir_entry *parent, const struct file_operations *proc_fops, void *data){
	struct proc_dir_entry *rv = __proc_create_entry(name, parent);
	(void)mode;
	BUG_ON(rv->parent == &kernel_sim.procfs);		// Daniel: We should not create files under root directery
	rv->data = data;
	rv->fops = proc_fops;
	return rv;
}

void procfs_traverse_tree_dfs(struct proc_dir_entry* e, void*c, int (*fn)(struct proc_dir_entry*e, void*c)) {
	bool should_enter_directory;
	if (!e)
		e = &kernel_sim.procfs;					// Node of /proc file system
	if (!strcmp(e->name, "profiling"))			// Daniel: Skip profiling file, dont want to see this long info
		return;
	should_enter_directory = fn(e, c);
	if (!strcmp(e->name, "serjio"))				// Daniel: Dont want to see any stuff of serjio
		should_enter_directory = false;
	if (should_enter_directory) {
		for (e = e->subdir; e; e = e->next) {
			procfs_traverse_tree_dfs(e, c, fn);
		}
	}
}

void seq_printf(struct seq_file *m, const char *fmt, ...)
{
	(void)m;
	(void)fmt;
}

int seq_open(struct file *file, const struct seq_operations *ops)
{
	(void)file;
	(void)ops;
	return 0;
}

int seq_release(struct inode *inode, struct file *file)
{
	(void)inode;
	(void)file;
	return 0;
}

ssize_t seq_read(struct file *file, char __user *buf, size_t size, loff_t *ppos)
{
	(void)file;
	(void)buf;
	(void)size;
	(void)ppos;
	return 0;
}

loff_t seq_lseek(struct file *file, loff_t offset, int whence)
{
	(void)file;
	(void)offset;
	(void)whence;
	return 0;
}

/*****************************************************************************/
struct new_utsname default_uts = {"sys1","node@exc.com","rel1","ver1","mach1","dom1"};
const struct new_utsname *utsname(void){
	return &default_uts;
}


/*********************************** Heap Sort *******************************/
// based on https://github.com/torvalds/linux/blob/8a72f3820c4d14b27ad5336aed00063a7a7f1bef/lib/sort.c
typedef struct __swap128_dummy { u64 dummy[2]; } u128;
static void u32__swap(void *a, void *b, int size){ u32  t = *(u32 *)a; *(u32 *)a = *(u32 *)b; *(u32 *)b = t; (void)size;}
static void u64__swap(void *a, void *b, int size){ u64  t = *(u64 *)a; *(u64 *)a = *(u64 *)b; *(u64 *)b = t; (void)size;}
static void u128_swap(void *a, void *b, int size){ u128 t = *(u128*)a; *(u128*)a = *(u128*)b; *(u128*)b = t; (void)size;}
static void generic_swap(void *a, void *b, int size){
	char t;
	do {
		t = *(char*)a;
		*(char*)a++ = *(char *)b;
		*(char*)b++ = t;
	} while (--size > 0);
}

void sort(void *base, const size_t num, const size_t _size, int (*cmp_func)(const void *, const void *), void (*swap_func)(void *, void *, int size)){
	const int size = (int)_size;								/* Daniel: kernle sorting supports only old 32bit size array */
	int i = (num/2 - 1) * size, n = num * size, c, r;			/* pre-scale counters for performance */
	if (!swap_func){
		switch (size) {
			case  4: swap_func = u32__swap; break;
			case  8: swap_func = u64__swap; break;
			case 16: swap_func = u128_swap; break;
			default: swap_func = generic_swap; break;
		}
	}

	for ( ; i >= 0; i -= size) {								/* heapify */
		for (r = i; r * 2 + size < n; r  = c) {
			c = r * 2 + size;
			if (c < n - size && cmp_func(base + c, base + c + size) < 0)
				c += size;
			if (cmp_func(base + r, base + c) >= 0)
				break;
			swap_func(base + r, base + c, size);
		}
	}

	for (i = n - size; i > 0; i -= size) {						/* sort */
		swap_func(base, base + i, size);
		for (r = 0; r * 2 + size < i; r = c) {
			c = r * 2 + size;
			if (c < i - size && cmp_func(base + c, base + c + size) < 0)
				c += size;
			if (cmp_func(base + r, base + c) >= 0)
				break;
			swap_func(base + r, base + c, size);
		}
	}
}

/* radix_tree implementation using hash-table */
struct radix_tree_node {
	unsigned long index;
	void *entry;
	struct hlist_node node;
};

int radix_tree_insert(struct radix_tree_root *rt, unsigned long index, void *entry)
{
	struct radix_tree_node *node;
	if (!(node = sim_kzalloc(sizeof(*node), rt->gfp_mask)))
		return -ENOMEM;
	node->index = index;
	node->entry = entry;
	INIT_HLIST_NODE(&node->node);
	hash_add(rt->tbl, &node->node, index);
	return 0;
}

static void *radix_tree_lookup_node(struct radix_tree_root *rt, unsigned long index)
{
	struct radix_tree_node *iter = NULL;
	hash_for_each_possible(rt->tbl, iter, node, index) {
		if (iter->index == index) {
			break;
		}
	}
	return iter;
}

void *radix_tree_lookup(struct radix_tree_root *rt, unsigned long index)
{
	struct radix_tree_node *node;

	if ((node = radix_tree_lookup_node(rt, index)))
		return node->entry;
	else
		return NULL;
}

void *radix_tree_delete(struct radix_tree_root *rt, unsigned long index)
{
	struct radix_tree_node *node;
	void *ret = NULL;
	if ((node = radix_tree_lookup_node(rt, index))) {
		hash_del(&node->node);
		ret = node->entry;
		sim_kfree(node);
	}
	return ret;
}

bool radix_tree_empty(struct radix_tree_root *root) {
	return hash_empty(root->tbl);
}

/************************ Shut down / restart kernel *************************/
void machine_restart(const struct ut_conf_kernel 	*krn_prm){
	if (0) {				// Don't use, not thread safe!!!
		int enbleMC = mcheck(&__malloc_check);
		BUG_ON(enbleMC); /* Must be first before any malloc*/
	}
	get_kernel_status = KERNEL_STATUS_BOOOTING;
	allocs_manager_create();
	__get_jiffies_start();
	kernel_sim.drain_workqueue_is_infinite = krn_prm->drain_workqueue_is_infinite;
	kernel_work_queues_init(&kernel_sim.wqs);
	smp_init(&kernel_sim.smp);
	kernel_tasks_init();
	kernel_timers_init(&kernel_sim.timers);
	__init_kernel_workqueues();
	ecpu_set_init(&kernel_sim.ecpu_set, krn_prm->num_ecpu);
	mutex_init(&kernel_sim.add_gendisk_lock);
	kernel_sim.T = &kernel_tasks;
	proc_root_init(&kernel_sim.procfs);
	kernel_sim.n_boot_allocs = kget_num_allocs();
	atomic_sub(kernel_sim.n_boot_allocs, &__aloc_mgr.numActiveAllocs);	// Hide internal memory allocations from the user, to prevent kernel resources counting as mem-leaks
	get_kernel_status = KERNEL_STATUS_RUNNING;
}
void machine_power_off(void){
	get_kernel_status = KERNEL_STATUS_SHUTDOWN;
	atomic_add(kernel_sim.n_boot_allocs, &__aloc_mgr.numActiveAllocs);	// Un-Hide internal memory allocations
	kernel_timers_destroy(&kernel_sim.timers);
	__destroy_kernel_workqueues();
	ecpu_set_destroy(&kernel_sim.ecpu_set);
	kernel_tasks_destroy();
	smp_destroy(&kernel_sim.smp);
	kernel_work_queues_destroy(&kernel_sim.wqs);
	if (kget_num_allocs() != 0) {
		pr_emerg("MEMORY LEAKS after kernel shutdown: %d detected!!!!\n", kget_num_allocs());
		BUG_ON(is_debugger_present());	/* If no debugger (like valgrind) don't crash or else we break the unitest flow of the system */
	}
}


// decide on the number of cpu's to use
// (num_ecpu == 0) means default. (num_ecpu > 0) sets the number of ecpu from command line
int get_default_ecpu_count(void)
{
	static int ecpu_num = 0;
	if (ecpu_num == 0) {
		ecpu_num = (is_debugger_present()) ? ECPU_IN_DBG_COUNT : ECPU_MAX_COUNT;
	}
	BUG_ON((ecpu_num < ECPU_MIN_COUNT) || (ecpu_num > ECPU_MAX_COUNT));
	return ecpu_num;
}

/***************************** bitrev.c **************************************/

const u8 byte_rev_table[256] = {
	0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
	0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0,
	0x08, 0x88, 0x48, 0xc8, 0x28, 0xa8, 0x68, 0xe8,
	0x18, 0x98, 0x58, 0xd8, 0x38, 0xb8, 0x78, 0xf8,
	0x04, 0x84, 0x44, 0xc4, 0x24, 0xa4, 0x64, 0xe4,
	0x14, 0x94, 0x54, 0xd4, 0x34, 0xb4, 0x74, 0xf4,
	0x0c, 0x8c, 0x4c, 0xcc, 0x2c, 0xac, 0x6c, 0xec,
	0x1c, 0x9c, 0x5c, 0xdc, 0x3c, 0xbc, 0x7c, 0xfc,
	0x02, 0x82, 0x42, 0xc2, 0x22, 0xa2, 0x62, 0xe2,
	0x12, 0x92, 0x52, 0xd2, 0x32, 0xb2, 0x72, 0xf2,
	0x0a, 0x8a, 0x4a, 0xca, 0x2a, 0xaa, 0x6a, 0xea,
	0x1a, 0x9a, 0x5a, 0xda, 0x3a, 0xba, 0x7a, 0xfa,
	0x06, 0x86, 0x46, 0xc6, 0x26, 0xa6, 0x66, 0xe6,
	0x16, 0x96, 0x56, 0xd6, 0x36, 0xb6, 0x76, 0xf6,
	0x0e, 0x8e, 0x4e, 0xce, 0x2e, 0xae, 0x6e, 0xee,
	0x1e, 0x9e, 0x5e, 0xde, 0x3e, 0xbe, 0x7e, 0xfe,
	0x01, 0x81, 0x41, 0xc1, 0x21, 0xa1, 0x61, 0xe1,
	0x11, 0x91, 0x51, 0xd1, 0x31, 0xb1, 0x71, 0xf1,
	0x09, 0x89, 0x49, 0xc9, 0x29, 0xa9, 0x69, 0xe9,
	0x19, 0x99, 0x59, 0xd9, 0x39, 0xb9, 0x79, 0xf9,
	0x05, 0x85, 0x45, 0xc5, 0x25, 0xa5, 0x65, 0xe5,
	0x15, 0x95, 0x55, 0xd5, 0x35, 0xb5, 0x75, 0xf5,
	0x0d, 0x8d, 0x4d, 0xcd, 0x2d, 0xad, 0x6d, 0xed,
	0x1d, 0x9d, 0x5d, 0xdd, 0x3d, 0xbd, 0x7d, 0xfd,
	0x03, 0x83, 0x43, 0xc3, 0x23, 0xa3, 0x63, 0xe3,
	0x13, 0x93, 0x53, 0xd3, 0x33, 0xb3, 0x73, 0xf3,
	0x0b, 0x8b, 0x4b, 0xcb, 0x2b, 0xab, 0x6b, 0xeb,
	0x1b, 0x9b, 0x5b, 0xdb, 0x3b, 0xbb, 0x7b, 0xfb,
	0x07, 0x87, 0x47, 0xc7, 0x27, 0xa7, 0x67, 0xe7,
	0x17, 0x97, 0x57, 0xd7, 0x37, 0xb7, 0x77, 0xf7,
	0x0f, 0x8f, 0x4f, 0xcf, 0x2f, 0xaf, 0x6f, 0xef,
	0x1f, 0x9f, 0x5f, 0xdf, 0x3f, 0xbf, 0x7f, 0xff,
};

/*****************************************************************************/
// Added by Daniel. Do not remove
const char* getToomb(void){
	static const char* toombstone = "In Comemoration of the Righteous Among the Nations: Archbishop Damaskinos, Feng-Shan Ho, Carl Lutz, Raoul Wallenberg, Oskar Schindler, Irena Sendler, Andrey Sheptytsky, Klymentiy Sheptytsky, Chiune Sugihara";
	return toombstone;
}

void get_random_bytes(void *buf, int nbytes){
	char *bf = buf;
	for (int idx = 0; idx < nbytes; ++idx){
		bf[idx] = (char)(rand() & 0xff);
	}
}

/*****************************************************************************/
void blk_start_plug(struct blk_plug *plug)
{
	current->plug = NULL; (void)plug;
	return;
}

static void __simulate_unplug_cb_schedule_out(struct work_struct *_w)
{
	struct blk_plug_cb *cb = container_of(_w, struct blk_plug_cb, w.work);
	int orig_cpu_id = kthread_self_task->cpu_idx;
	kthread_self_task->cpu_idx = cb->cpu_id;
	cb->callback(cb, false);
	kthread_self_task->cpu_idx = orig_cpu_id;
	current->plug = NULL;
	// cb is already freed!
}

struct blk_plug_cb *blk_check_plugged(blk_plug_cb_fn unplug, void *data, int size)
{
	struct blk_plug_cb *cb = kzalloc(size, GFP_ATOMIC);
	BUG_ON(size < (int)sizeof(*cb));
	if (cb) {
		cb->data = data;
		cb->callback = unplug;
		cb->cpu_id = raw_smp_processor_id();
		INIT_DELAYED_WORK(&cb->w, __simulate_unplug_cb_schedule_out);		//INIT_WORK(&cb->w, __simulate_unplug_cb_schedule_out);
		queue_delayed_work(system_wq, &cb->w, HZ/1000);						//schedule_work_on(cb->cpu_id, &cb->w);
	}
	return cb;
}
#define __nouse__ __attribute__((__unused__))
int local_nic_prio_cmp_fn(__nouse__ void *priv, __nouse__ struct list_head *a, __nouse__ struct list_head *b) { return 0;}
int nvmeibc_disk_prefix_priority_masks_validate_module_params(void) { return 0; }


#include "nvmeib_completion_noise.h"
void nvmeib_completion_noise_start(enum nvmeib_noise_type type) { (void)type; }
void nvmeib_completion_noise_end(enum nvmeib_noise_type type, const unsigned long *cpu_mask_bitmap, int bitmap_size,
	enum nvmeib_noise_ctrs ctr)
{
	(void)type; (void)cpu_mask_bitmap; (void)bitmap_size; (void)ctr;
}

int nvmeibc_nordda_channel_wq_init(void) { return 0; }
void nvmeibc_nordda_channel_wq_destroy(void) { }

int nvmeibc_locks_channel_wq_init(void) { return 0; }
void nvmeibc_locks_channel_wq_destroy(void) { }

//struct workqueue_struct *
struct workqueue_struct *alloc_workqueue(const char *name, unsigned int flags, int max_active) {
	(void)name;
	(void)flags;
	(void)max_active;
	return NULL;
}

int profile_event_register(enum profile_type type, struct notifier_block *n){(void)type; (void)n; return 0;}
int profile_event_unregister(enum profile_type type, struct notifier_block *n){(void)type; (void)n; return 0;}

/*****************************************************************************/
// EOF.

