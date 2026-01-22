/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_SIM_SVC_H
#define KR_SIM_SVC_H
/*
 * The Kernel Simulator backdoor services, i.e. API's that dont emulate a kernel
 * API but rather allow the testing code to access internals of the emulated kernel
 */

enum kernel_status_t {
	KERNEL_STATUS_CRASHING = -1,
	KERNEL_STATUS_BOOOTING = 0,
	KERNEL_STATUS_RUNNING  = 1,
	KERNEL_STATUS_SHUTDOWN = 2,
};
extern enum kernel_status_t get_kernel_status;

/**************************** Emulation of CPU's *****************************/

//#define TASK_STRUCT_SAVE_GETCPU_STACK			// enable saving the stack that acquired the last getcpu(), used for debugging

// the simulated percpu implementation guts
struct percpu_prop {
#ifdef TASK_STRUCT_SAVE_GETCPU_STACK
#define PERCPU_MAX_STACK_DEPTH		30		// max number of stack addresses (depth) to be saved
#define PERCPU_MAX_STACK_FUNC_NAME_LEN	100		// the name of a function symbol that is saved
#define PERCPU_STACK_SYMBOL_OFFSET		40		// since ymbol has full workspace path, skip its first part (dependent on build location)
	void			*getcpu_stack[PERCPU_MAX_STACK_DEPTH];	// stack of last one who completed the getcpu()
	char			stack_symbols[PERCPU_MAX_STACK_DEPTH][PERCPU_MAX_STACK_FUNC_NAME_LEN];	// symbol of stack trace
#endif // TASK_STRUCT_SAVE_GETCPU_STACK
};

/*
 * maintain log of irq save/restore, for debugging only !!!
 */
struct irq_change_log_entry {
	bool			is_save; // or restore
	unsigned long	before, after;	// irq flags before/after operation
#define IRQ_CHANGE_LOG_MAX_STACK_DEPTH		20
	void			*stack[IRQ_CHANGE_LOG_MAX_STACK_DEPTH];	// stack of who made the change
	int				numptr;
};
struct irq_change_log {
#define IRQ_CHANGE_LOG_SIZE		20
	struct irq_change_log_entry 	change_log[IRQ_CHANGE_LOG_SIZE];
	int								next_entry;				// next entry to be used
};

// properties of a single cpu
struct cpu_prop {
	struct mutex	access_lock;				// a lock for consistency access to object
	unsigned long	irq_flags;
	int				irq_save_count;	// count number of times the irq was saved & is waiting for a matching restore.
//#define LOG_IRQ_SAVE_RESTORE
#ifdef LOG_IRQ_SAVE_RESTORE
	struct irq_change_log	irq_mod;	// log irq modifications
#endif
#define KERNEL_SIM_CPU_IRQ_SAVE_MAX_RECURSION	64	// when irq are saved (recursively) too many time, warn !!!
	struct percpu_prop		per_cpu;			// percpu state on a cpu
	pthread_mutex_t			cpu_owner;			// a lock that allows a thread to acquire the cpu, hence not allowing any other thread to run on that cpu until the cpu is release. this is simulating the fact that the kthread cannot be preempted (either by kernel or interrupt). its a recursive mutex bcz the same thread can disable irq many times in recursion, as long as each save() has a matching restore()
};

// the properties of a smp (multiple cpu), for simulating the interrupts, percpu, ...
struct smp_cpu_sim {
	struct cpu_prop				cpu[CONFIG_NR_CPUS];
};

/**************************** Emulation of CPU's *****************************/
/* Used to emulate async callbacks without the cost of launching thread on each
   callback an object emulating a single CPU using its own work-queue processed
   by an internal thread */
struct eCPU {
#define ECPU_NAME_LEN		32
	char					name[ECPU_NAME_LEN];
	struct workqueue_struct *wq;
};

static inline void __ecpu_init(struct eCPU   	*ecpu,
							   const char		*name)
{
	strlcpy(ecpu->name, name, sizeof(ecpu->name));
	ecpu->wq = create_singlethread_workqueue(ecpu->name);
	BUG_ON(!ecpu->wq);
}
static inline void __ecpu_destroy(struct eCPU   	*ecpu)
{
	destroy_workqueue(ecpu->wq);
	ecpu->wq = NULL;
}
static inline void __ecpu_set_queue_work(struct eCPU   		*ecpu,
										 struct work_struct	*work)
{
	queue_work(ecpu->wq, work);
}
static inline void __ecpu_drain(struct eCPU   		*ecpu)
{
	wq_drain(ecpu->wq);
}

/* an ecpu_set emulates a set of CPU's' with a work dispatch policy
 * with multi-client, we might need this per client */
struct ecpu_set {
	struct eCPU		eCPU[ECPU_MAX_COUNT];	// set of CPU's
	atomic64_t 		next_eCPU;			// dispatch Round-Robin policy
	int				ecpu_num;				// number of ecpu's now active
};

void ecpu_set_init(struct ecpu_set	*set,int	ecpu_num);
void ecpu_set_destroy(struct ecpu_set	*set);
char *ecpu_set_dump_queues(struct ecpu_set		*set,
						   char					*buf,
						   int					buf_size);
void ecpu_set_drain(struct ecpu_set		*set);
void __shcedule_on_eCPU(work_func_t	func, struct work_struct *w);

/*****************************************************************************/
// implement emulated CPU's (each CPU has a workqueue)
#define USE_CPU_EMULATION    1
#if USE_CPU_EMULATION
	#define eCPU_cb_param_list           struct work_struct *_work_item
	#define eCPU_cb_ret_type             void
	#define eCPU_thread_internal_params struct work_struct workq_chain
	#define eCPU_thread_prepare(type, params) { type *params = (type*)sim_kmalloc(sizeof(type), GFP_KERNEL);
	#define eCPU_thread_launch(func, params, is_sync) if (!(is_sync)) __shcedule_on_eCPU(func, &((params)->workq_chain)); else func(&(params)->workq_chain); }
	#define eCPU_thread_extract_param(type)		container_of(_work_item, type, workq_chain)
	#define eCPU_thread_start_execution(p)		(void)p
	#define eCPU_thread_end_execution(params) do {sim_kfree(params);} while(0)
#else
	void *detached_pthread_func_wrapper(void *p);  // a wrapper with pthread callback prototype to wrap our prototype which is for work-queue
	#define eCPU_cb_param_list          void *_lock//struct spinlock *_lock
	#define eCPU_cb_ret_type             void*
	#define eCPU_thread_internal_params detached_thread_internal_params
	#define eCPU_thread_prepare         detached_thread_prepare
	#define eCPU_thread_launch          detached_thread_launch
	#define eCPU_thread_extract_param(type)			container_of(_lock, type, l)// cast from lock member to enclosing type
	#define eCPU_thread_start_execution detached_thread_start_execution
	#define eCPU_thread_end_execution   detached_thread_end_execution
#endif


/********************************** Timers ***********************************/
/* a single timer, consuming timers with its own internal worker thread */
struct cpu_timers {
	struct rb_root 		rb_waiting_timers;	// Hash map of awaiting timers, arranged in red-black tree sorted by timer expiration timestamp
	struct task_struct *timers_thread; 		// Scheduling thread, which executes all timers, one by one.
	spinlock_t 			lock;				// Lock to syncronize inserting and removing timers to and from the tree.
	wait_queue_head_t timers_wait_queue;	// work queue to wake up timers execution thread
	atomic_t num_active_timers;
	atomic_t num_dispatched_timers;
	int		cpu_id;							// index of this object within the kernel per-cpu timers array.
	int		flags;
#define TIMER_FLAG_REQ_DRAIN			0x0001	// some awaits for draining if all timers but expiration time should be honored !!
#define TIMER_FLAG_REQ_FORCED_DRAIN		0x0002	// drain all timers, regardless of their expiration time
#define TIMER_FLAG_DRAIN_COMPLETED		0x0004	// timer engine indicates that it has completed draining (assumes no new timers are added)
	struct timer_list * __concurrent_access executing;		// the timer that is executing (BEWARE: it might have been freed & now returning). NULL when invocation returns until another timer is picked for expiration.
};

/*
 * scale timers by supporting multiple object, each with its own thread
 * timers are dispatched in round robin.
 */
struct kernel_timers {
	struct cpu_timers	cpu_timer[MAX_NUM_TIMERS_ENGINE];
	__concurrent_access u32					next_cpu;	// allow non accurate distribution (atomic is slower)
};


/********************************** kernel existing work-queues ***********************************/
#define KR_SIM_MAX_WORK_QUEUE		(ECPU_MAX_COUNT + 24 /* NVMESH_N_PHYS_DISKS */ + 32)	// max workqueues we expect at a given time
struct kernel_work_queues {
	struct workqueue_struct		*wq[KR_SIM_MAX_WORK_QUEUE];
	struct mutex				wq_mutex;
};
void kernel_work_queues_init(struct kernel_work_queues	*wqs);
void kernel_work_queues_destroy(struct kernel_work_queues	*wqs);
void kernel_work_queues_add(struct workqueue_struct	*wq);
void kernel_work_queues_remove(struct workqueue_struct	*wq);
struct workqueue_struct *kernel_work_queues_get_by_name(struct kernel_work_queues	*wqs,
														const char					*name);
void kernel_work_queues_drain_all(void);
/* Debugging facilities */
int is_debugger_present(void);					// Daniel's helper function: returns 1 if debugger is present, 0 if not.

/******************* OS simulator for IO generation API ***********************/
#define N_GENDISKS 		(12)							// Kernel supports up to 12 block devices simultanously
struct osSimulator{
	int nVolumes;										// Amount of blockdevices/volumes mounted in the system
	struct gendisk 	   *disks[N_GENDISKS];				// Hashtable. supports up to N volumes. Volume's uuid is converted to hash of [0..MAX_VOLUMES_IN_NVMESH-1]
	struct mutex		disks_lock;						// a lock to gain exclusive access to the 'disks'.
	struct block_device bds[  N_GENDISKS];
	struct di_tracker *di_tracker;						// A reference to NVMeshSystem->di_tracker
	struct kernel_sim *kernel;							// Simulator of the kernel
};
#undef N_GENDISKS

// Testing API - control of the simulator from testing methods. Each client must have its own OS simulator
int  osSimulator_init(		struct osSimulator* sim);
void osSimulator_destroy(	struct osSimulator* sim);
void osSimulator_setCurrent(struct osSimulator* sim);	// Currently we cannot emulate block device registration with OS for a few clients in parallel so they have to do it sequentially. Select the input OS to handle block device registration request
struct osSimulator* osSimulator_getCurrent(void);		// Inverse of the above
int  osSimulator_mount(		struct osSimulator* sim, int volInd, fmode_t mode);	// Mount the block device: mount /dev/vol_name
int  osSimulator_unmount(	struct osSimulator* sim, int volInd);	// Un-mount: umount -t /dev/vol_name
int  osSimulator_unmount_no_close(struct osSimulator* sim, int volInd);	// Un-mount: but dont call close on nvmesh block device, leaving it open
void osSimulator_nullify(	struct osSimulator* sim, int volInd);	// After volume unregistered from OS, freed gen disk it is marked as free. Convert it to NULL (just for debug).
int  osSimulator_diskOpenIdx( struct osSimulator *sim,unsigned volInd, bool	invoke_open, fmode_t mode, struct block_device **bdev);
int  osSimulator_diskCloseIdx(struct osSimulator *sim,unsigned volInd);
int  osSimulator_diskfree(struct osSimulator	*sim, struct gendisk *disk);
bool osSimulator_diskIsAttached(struct osSimulator	*sim, unsigned volInd);

/* IO operations: Read/Write 'arr' from/to the volume (volInd) at blocks [start..start+length]
 * Length of the array must be exactly length*SECTOR_SIZE. 'start' and 'length' are in units of blocks
 * if comp = NULL nothing */
int  osSimulator_readArr_f( struct osSimulator* sim, int volInd, u64 startBlock, u64 length,       u8 dst[], ulong flags, struct completion* comp);
int  osSimulator_writeArr_f(struct osSimulator* sim, int volInd, u64 startBlock, u64 length, const u8 src[], ulong flags, struct completion* comp);
int  osSimulator_trim_f(	struct osSimulator* sim, int volInd, u64 startBlock, u64 length, ulong flags);
int  osSimulator_writeArr_f_missaligned(struct osSimulator* sim, int volInd, u64 startBlock, u64 length, const u8 src[], ulong flags, struct completion* comp);
int  osSimulator_readArr_f_no_vcnt( struct osSimulator* sim, int volInd, u64 startBlock, u64 length,       u8 dst[], ulong flags, struct completion* comp);
int  osSimulator_writeArr_f_no_vcnt(struct osSimulator* sim, int volInd, u64 startBlock, u64 length, const u8 src[], ulong flags, struct completion* comp);

#define osSimulator_trim(        sim, volInd, startBlock, length) osSimulator_trim_f(         sim, volInd, startBlock, length, 0L)
#define osSimulator_trimWait(    sim, volInd, startBlock, length) osSimulator_trim_f(         sim, volInd, startBlock, length, (1 << BIO_USER_MAPPED))
#define osSimulator_readArr(     sim, volInd, startBlock, length, dst) osSimulator_readArr_f( sim, volInd, startBlock, length, dst, 0L, NULL)
#define osSimulator_readArrWait( sim, volInd, startBlock, length, dst) osSimulator_readArr_f( sim, volInd, startBlock, length, dst, (1 << BIO_USER_MAPPED), NULL)
#define osSimulator_readArrFree( sim, volInd, startBlock, length, dst) osSimulator_readArr_f( sim, volInd, startBlock, length, dst, (1 << BIO_OWNS_VEC), NULL)
#define osSimulator_readArr_async_wait( sim, volInd, startBlock, length, dst, comp) osSimulator_readArr_f( sim, volInd, startBlock, length, dst, 0L, comp)

#define osSimulator_writeArr(    sim, volInd, startBlock, length, src) osSimulator_writeArr_f(sim, volInd, startBlock, length, src, 0L, NULL)
#define osSimulator_writeArrWait(sim, volInd, startBlock, length, src) osSimulator_writeArr_f(sim, volInd, startBlock, length, src, (1 << BIO_USER_MAPPED), NULL)
#define osSimulator_writeArrFree(sim, volInd, startBlock, length, src) osSimulator_writeArr_f(sim, volInd, startBlock, length, src, (1 << BIO_OWNS_VEC), NULL)
#define osSimulator_writeArr_async_wait(    sim, volInd, startBlock, length, src, comp) osSimulator_writeArr_f(sim, volInd, startBlock, length, src, 0L, comp)

int  osSimulator_allert_pending_ios(struct osSimulator* sim, int max_wait_time);	// Allert if after max_wait_time[mSecs] there are still pending IO's. Put -1 for defualt kernel value
int  osSimulator_rv_of_last_io_get(  const struct osSimulator* sim, int volInd);
void osSimulator_rv_of_last_io_clean_all(  struct osSimulator* sim);				// Clean for all volumes

// a simulation of a single kernel entity
struct kernel_tasks;
struct kernel_sim {
	struct smp_cpu_sim		smp;		// multi-processor simulation
	struct ecpu_set			ecpu_set;	// The cpu-set within the client emulated kernel
	struct kernel_timers	timers;		// kernel timers execution
	struct kernel_work_queues	wqs;	// all of the live kernel qork-queues
	struct kernel_tasks		*T;
	struct mutex add_gendisk_lock;
	struct proc_dir_entry	  procfs;	// Emulation of proc-fs (stores the root directory "/proc")
	int 					n_boot_allocs;	// number of memory allocations made by the kernel, before any NVMesh code is executed.
	int drain_workqueue_is_infinite;
};

void __bio_track_start(struct bio *b);
void __bio_track_end(struct bio *b, int rv);

void kernel_sim_dump_state(void);

#endif // KR_SIM_SVC_H



