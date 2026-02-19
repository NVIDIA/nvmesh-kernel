#include "kr_incs.h"
#include <stdint.h>
#include "nvmeibc_trace.h"
#include "di_tracker.h"
#include "nvmeibc_block.h"
#include "utils/nvmeib_jdr/nvmeib_txt.h"
#include <execinfo.h> // backtrace()

enum kernel_status_t get_kernel_status = KERNEL_STATUS_BOOOTING;

/**************************** Emulation of CPU's *****************************/
/*
 * an ecpu_set emulates a set of CPU's' with a work dispatch policy
 * the first ecpu's are in use when not all ecpu's are running
 * with multi-client, we might need this per client
 */

void ecpu_set_init(struct ecpu_set	*set,int	ecpu_num){
	char 	cpu_name[10];
	int 	i;
	BUG_ON((ecpu_num < ECPU_MIN_COUNT) || (ecpu_num > ECPU_MAX_COUNT));
	set->ecpu_num = ecpu_num;
	for (i=0; i<ecpu_num; i++) {
		snprintf(cpu_name, sizeof(cpu_name), "ecpu/%hu", (u16)i);
		__ecpu_init(set->eCPU + i, cpu_name);
	}
	atomic64_set(&set->next_eCPU, 0);		// Next task will be scheduled on this CPU
}

void ecpu_set_destroy(struct ecpu_set	*set){
	int i;
	for (i=0; i<set->ecpu_num; i++)
		__ecpu_destroy(set->eCPU + i);
	//atomic64_set(&next_eCPU, 0);
}

char *ecpu_set_dump_queues(struct ecpu_set *set, char *buf, int	buf_size){
	int	i, len;
	for (i=0, len=0; i<set->ecpu_num; i++)
		len += scnprintf(buf+len, buf_size, "%4i, ", set->eCPU[i].wq->num_pending_works);
	len += scnprintf(buf+len, buf_size, "\nnext_cpu=%20lld", atomic64_read(&set->next_eCPU));
	BUG_ON(len >= buf_size);
	return buf;
}

void ecpu_set_drain(struct ecpu_set *set){
	int	i;
	for (i=0; i<set->ecpu_num; i++)
		__ecpu_drain(set->eCPU + i);
}

extern struct kernel_sim	kernel_sim;
void __shcedule_on_eCPU(work_func_t	func, struct work_struct *w){
	struct ecpu_set *set = &kernel_sim.ecpu_set;
	long long next_cpu   = atomic64_inc_return(&set->next_eCPU) % set->ecpu_num;
	INIT_WORK(w, (void*)func);
	__ecpu_set_queue_work(set->eCPU + next_cpu, w);
}

/**************************** Maintain kernel workqueues, for debugging access*****************************/

void kernel_work_queues_init(struct kernel_work_queues	*wqs)
{
	memset(wqs, 0, sizeof(*wqs));
	mutex_init(&wqs->wq_mutex);
}
void kernel_work_queues_destroy(struct kernel_work_queues	*wqs)
{
	// verify all wq's were destroyed
	int	i;
	for (i=0; i < KR_SIM_MAX_WORK_QUEUE; i++) {
		BUG_ON(wqs->wq[i] != NULL);
	}
	mutex_destroy(&wqs->wq_mutex);
}
// find an empty index; wqs->wq_mutex must be locked
static int __kernel_work_queues_get_free_entry(struct kernel_work_queues	*wqs)
{
	int	i;
	for (i=0; i < KR_SIM_MAX_WORK_QUEUE; i++) {
		if (wqs->wq[i] == NULL) {
			return i;
		}
	}
	BUG_ON(true); 	// its time increase the amount of wq's we can hold.
	return -1;		//poison
}
// find a wq by name
struct workqueue_struct *kernel_work_queues_get_by_name(struct kernel_work_queues	*wqs,
														const char					*name)
{
	int	i;
	for (i=0; i < KR_SIM_MAX_WORK_QUEUE; i++) {
		if ((wqs->wq[i]) && !strcmp(wqs->wq[i]->name, name)) {
			return wqs->wq[i];
		}
	}
	if (get_kernel_status != KERNEL_STATUS_CRASHING)
		BUG();	// Bug in the logic of the one who requests a wrong workqueue
	return NULL;
}
void kernel_work_queues_add(struct workqueue_struct	*wq)
{
	int unused_idx;
	mutex_lock(&kernel_sim.wqs.wq_mutex);
	unused_idx = __kernel_work_queues_get_free_entry(&kernel_sim.wqs);
	kernel_sim.wqs.wq[unused_idx] = wq;
	mutex_unlock(&kernel_sim.wqs.wq_mutex);
}
void kernel_work_queues_remove(struct workqueue_struct	*wq)
{
	struct workqueue_struct	**wq_iter;
	int	i;
	mutex_lock(&kernel_sim.wqs.wq_mutex);
	for (i=0, wq_iter=kernel_sim.wqs.wq; i < KR_SIM_MAX_WORK_QUEUE; i++, wq_iter++) {
		if (*wq_iter == wq) {
			*wq_iter = NULL;
			mutex_unlock(&kernel_sim.wqs.wq_mutex);
			return;
		}
	}
	BUG_ON(true);	// how come we didnt find it ?
}

void kernel_work_queues_drain_all(void)
{
	int i, count;
	struct workqueue_struct *wq;

	// We cannot simply call drain_workqueue() because we since we are not familiar
	// with each particular workqueue, we cannot be sure that it won't disappear
	// while it is being drained. On the other hand, we cannot hold wq_mutex while
	// draining a workqueue, because draining itself might need removing another
	// workqueue - a livelock.
	for (count = 1; true ; count++) {
		mutex_lock(&kernel_sim.wqs.wq_mutex);
		for (i = 0; i < KR_SIM_MAX_WORK_QUEUE; i++) {
			wq = kernel_sim.wqs.wq[i];
			if (wq && !workqueue_is_empty(wq)) {
				break;
			}
		}
		mutex_unlock(&kernel_sim.wqs.wq_mutex);

		if (i == KR_SIM_MAX_WORK_QUEUE)	// all drained
			break;

		schedule();
		WARN_ON((!(count & 0x5ffff))&&(!kernel_sim.drain_workqueue_is_infinite)); /* After ~4[sec] issue warning */
	}
}

/*
 * Debugging facilities
 */

// Determine if running with debugger
#include <sys/stat.h>
#include <string.h>
#include <fcntl.h>
static int is_being_debugged = -1;	// Enusre the costly function below is called only once
int is_debugger_present(void) {
    char buf[4096], *debugger_pid = NULL;
	const char TracerPid[] = "TracerPid:";
	ssize_t num_read;
    int rv = 0, status_fd;
	if (is_being_debugged>=0){
		rv = is_being_debugged;		// Already called this function and know the result
		goto _out;
	}
	status_fd = open("/proc/self/status", O_RDONLY);
    if (status_fd == -1)
		goto _out;					// Cant read /proc?
    num_read = read(status_fd, buf, sizeof(buf));
    if (num_read <= 0)
		goto _out;

	buf[num_read] = 0;
	//pr_info("%s",buf);
	debugger_pid  = strstr(buf, TracerPid);
	if (debugger_pid)
		rv = !!atoi(debugger_pid+sizeof(TracerPid)-1);
	is_being_debugged = rv;			// Function completed with success, update the flag to prevent future executions
_out:
    return rv;
}

void BREAKPOINT(bool dump_kernel) {
	if (get_kernel_status == KERNEL_STATUS_CRASHING){
		pr_emerg("CRASH in CRASH!!!!!\n");
		return;								// Exit without printing, to avoid infinite printing loop
	} else if (get_kernel_status != KERNEL_STATUS_RUNNING)
		dump_kernel = false;				// Cannot print, some parts may not exist, because they were already shut down or kernel has not booted yet
	get_kernel_status = KERNEL_STATUS_CRASHING;
	if (dump_kernel) {
		kernel_sim_dump_state();
	}

	if(1){
		nvmeib_flush_and_terminate(nvmeibc_trace_long);		// Flush traces buffers
		nvmeib_flush_and_terminate(nvmeibc_trace_goodpath);
		nvmeib_flush_and_terminate(nvmeibc_trace_metrics);
		nvmeib_flush_and_terminate(nvmeibc_trace_eter);
		nvmeib_flush_and_terminate(nvmeibc_trace_eph);
	}

	if (is_debugger_present())
		raise(SIGINT);									// Do not create core dump. Just break point in debugger
	else
		raise(SIGABRT);									// No debugger active, create core dump to debug later the core. Use __breakpoint() on windows phone, raise(SIGABRT) on windows
}

/* Single "request other thread's stack dump" in progress (signal handler is process-wide).
 * Caller slot: 0 = none, else caller pthread_t as uintptr_t (CAS'd).
 * Target: which thread should dump when it receives SIGUSR2 (set by caller after winning CAS).
 * Cast pthread_t <-> uintptr_t is only valid when pthread_t fits in one word (e.g. Linux). */
_Static_assert(sizeof(pthread_t) <= sizeof(uintptr_t),
		"pthread_t must fit in uintptr_t for atomic caller slot");
static struct stack_dump {
	uintptr_t caller_slot;
	uintptr_t target_os_id;
	void *buffer[100];
	int buffer_nptrs;
	bool is_initialized;
} stack_dump = { 0 };

/**
 * Request that @p target_os_id dump its stack via SIGUSR2; this thread blocks until that dump
 * and the target has signaled back. Only one such request is in progress process-wide.
 * @param target_os_id  thread to signal (will run handler and dump its stack).
 * @param best_effort   if true and another request is in progress, return false without waiting;
 *                      if false, spin until the slot is free, then proceed.
 * @return true if the request was performed (target dumped and we returned); false only when
 *         best_effort is true and the slot was already taken.
 */
bool request_other_thread_stack_dump(pthread_t target_os_id_param, bool best_effort)
{
	bool is_success = false;
	const pthread_t self_os_id = pthread_self();
	uintptr_t expected;
	uintptr_t desired = (uintptr_t)self_os_id;
	sigset_t mask;
	int err;

	if (target_os_id_param == self_os_id)
		return true; /* no-op: would be dumping our own stack; caller can dump_stack() directly */

	for (;;) {
		expected = 0;
		if (__atomic_compare_exchange_n(&stack_dump.caller_slot, &expected, desired,
					       false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
			break;
		if (best_effort)
			return false;
		/* Spin until slot is free */
	}

	/* Call backtrace at least once outside of a signal handler to make the subsequent calls async-signal-safe */
	if (!stack_dump.is_initialized) {
		backtrace(stack_dump.buffer, ARRAY_SIZE(stack_dump.buffer));
		stack_dump.is_initialized = true;
	}

	/* Block the return SIGUSR2 so that we won't miss it if it fires before we sigsuspend() */
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR2);
	pthread_sigmask(SIG_BLOCK, &mask, NULL);

	__atomic_store_n(&stack_dump.target_os_id, (uintptr_t)target_os_id_param, __ATOMIC_SEQ_CST);

	if ((err = pthread_kill(target_os_id_param, SIGUSR2)) != 0) {
		pr_emerg("Failed to signal thread to dump stack: pthread_kill() returned %d\n", err);
		goto out;
	}

	sigfillset(&mask);
	sigdelset(&mask, SIGUSR2);
	sigsuspend(&mask);

	dump_backtrace(stack_dump.buffer, stack_dump.buffer_nptrs);

	is_success = true;

out:
	sigemptyset(&mask);
	sigaddset(&mask, SIGUSR2);
	pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
	__atomic_store_n(&stack_dump.target_os_id, 0, __ATOMIC_SEQ_CST);
	__atomic_store_n(&stack_dump.caller_slot, 0, __ATOMIC_SEQ_CST);
	return is_success;
}

void dump_this_and_ut_stacks(void)
{
	dump_stack();

	if (ut_os_id == 0) {
		pr_emerg("Not dumping UT thread stack: ut_os_id is not set\n");
		return;
	}

	if (pthread_self() == ut_os_id)
		return;

	/* Wait for our turn so the UT stack is dumped before any thread proceeds to BREAKPOINT/abort.
	 * Otherwise a second panicking thread can hit BREAKPOINT and raise(SIGABRT) before the first
	 * thread's UT-dump handshake completes, and the UT stack is never printed. */
	(void)request_other_thread_stack_dump(ut_os_id, false);
}

static void __signal_stack_dump_handler(__attribute__((__unused__)) int signr, __attribute__((__unused__)) siginfo_t *info, __attribute__((__unused__)) void *vcontext)
{
	const pthread_t self = pthread_self();
	uintptr_t target_id, caller_id;
	int err;

	target_id = __atomic_load_n(&stack_dump.target_os_id, __ATOMIC_SEQ_CST);
	if (self != (pthread_t)target_id)
		return; /* not the requested target (e.g. caller waking from sigsuspend) */

	stack_dump.buffer_nptrs = backtrace(stack_dump.buffer, ARRAY_SIZE(stack_dump.buffer));

	caller_id = __atomic_load_n(&stack_dump.caller_slot, __ATOMIC_SEQ_CST);
	if ((err = pthread_kill((pthread_t)caller_id, SIGUSR2)) != 0)
		pr_emerg("Failed to signal stack dump caller back: pthread_kill() returned %d\n", err);
}

void set_up_other_thread_stack_dump_handler(void)
{
	struct sigaction sa;
	sigfillset(&sa.sa_mask);
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = __signal_stack_dump_handler;
	if (sigaction(SIGUSR2, &sa, NULL) != 0)
		pr_emerg("Failed to set stack dump signal handler: errno=%d\n", errno);
}

/************************ Simulator of OS testing API ************************/
struct osSimulator *_simOS = NULL; // Singletone pointer. One OS can be active at a time. Daniel: Put somewhere else (redesign system architecture)
void osSimulator_setCurrent(struct osSimulator* sim){ _simOS = sim;}
struct osSimulator* osSimulator_getCurrent(void){ return _simOS; }

void osSimulator_nullify(struct osSimulator* sim, int volInd){
	memset(&sim->bds[volInd], 0, sizeof(*sim->bds));
	sim->disks[volInd]= NULL;
}

int osSimulator_init(struct osSimulator* sim){
	memset(sim, 0, sizeof(*sim));
	sim->kernel = &kernel_sim;
	mutex_init(&sim->disks_lock);
	return 0;
}

/* max_wait_time has the following coding options
 *  <  0 : wait until all IO's are done (max 300 sec) & crash if IO's dont terminate by then.
 *  == 0 : Only sample the state but not really waiting
 *  >  0 : same as (<0) but with a max timeout as provided in call. */
int osSimulator_allert_pending_ios(struct osSimulator* sim, int max_wait_time){
	int i, ios, poll_after = 3;									// Poll each 3[mSec] up to max_wait_time[mSec]. In actual linux this is 30[seconds]
	bool alert_with_bug = (max_wait_time!=0);					// If user wants to wait for IO then respond with BUG_ON(), otherwise he just queries the current amount of ios
	if (max_wait_time<0)
		max_wait_time = 300;									// Default is 300[mSec]
	while(true) {
		for (ios=0, i=0; i<sim->nVolumes; i++)
			ios += atomic_read(&sim->bds[i].n_active_ios); 	// Amount of pending ios
		if ((ios<=0)||(max_wait_time<=0))
			break;
		msleep(poll_after);										// Half busy waiting.
		max_wait_time -= poll_after;
	};
	if ((ios!=0) && alert_with_bug) {
		pr_emerg("Kernel detected %d of pending IOs\n", ios);
		BUG_ON(ios);											// Kernel would kill the block after 120 of no answer + detects a memory leak.
	}
	return ios;
}

int osSimulator_rv_of_last_io_get(const struct osSimulator* sim, int volInd){
	return sim->bds[volInd].status_of_last_io;
}

void osSimulator_rv_of_last_io_clean_all(struct osSimulator* sim){
	int i, n_vols = ARRAY_SIZE(sim->bds);
	for (i = 0; i < n_vols; i++)
			sim->bds[i].status_of_last_io = 0;
}

void osSimulator_destroy(struct osSimulator* sim){
	osSimulator_allert_pending_ios(sim, 0);						// Immediately allert on pending IO's
	memset(sim, 0, sizeof(*sim));
}

int osSimulator_mount(struct osSimulator* sim, int volInd, fmode_t mode){
	struct block_device *bdev;
	int rv = osSimulator_diskOpenIdx(sim, volInd, true, mode, &bdev);
	return rv;
}

int osSimulator_unmount(struct osSimulator* sim, int volInd){
	const int n_ios = atomic_read(&sim->bds[volInd].n_active_ios);
	struct gendisk *gdisk = sim->disks[volInd];
	BUG_ON(n_ios);												// OS cannot unmount. Probably bad design of unitests
	if (gdisk) {
		gdisk->fops->release(sim->disks[volInd], FMODE_EXCL | FMODE_WRITE); /* Default*/
		osSimulator_diskCloseIdx(sim, volInd);
	} else
		pr_emerg("Block device was detached leaving IO queue working\n");
	return 0;
}

int osSimulator_unmount_no_close(struct osSimulator* sim, int volInd){
	const int n_ios = atomic_read(&sim->bds[volInd].n_active_ios);
	struct gendisk *gdisk = sim->disks[volInd];
	BUG_ON(!gdisk || n_ios);									// OS cannot unmount. Probably bad design of unitests
	osSimulator_diskCloseIdx(sim, volInd);
	return 0;
}

/* open a block device, so it wont be gone under our feets. This is similiar to an application opening the block device file
  since we might be trying to get reference when the request is being detached, we start by attempting to take a ref & only if its not closing, take the disk ref.
  otherwise, get_disk() might be the one increasing 0 -> 1 which is not allowed. Assume: no race with destroy of the device. this is for UT code to set the stage */
int osSimulator_diskOpenIdx(struct osSimulator	*sim, unsigned volInd,
							bool invoke_open, fmode_t mode, struct block_device **bdev) {
	struct gendisk 	   *disk;
	struct request_queue	*q;
	int	rv = -ENOENT;

	*bdev = NULL;
	BUG_ON(volInd >= ARRAY_SIZE(sim->disks));
	mutex_lock(&sim->disks_lock);
	disk = sim->disks[volInd];
	if (unlikely(!disk))
		goto _out;

	mutex_lock(&disk->lock);
	q = disk->queue;	// this is eliminated during destroy of our block device, so use a temporary copy
	if (likely(!disk->is_closing && (disk->nr_sects > 0) && q)) {
		mutex_lock(&q->sysfs_lock);
		if (likely(!blk_queue_dying(q))) {
			pr_info("open block device %d\n", volInd);
			get_disk(disk);
			BUG_ON(blk_get_queue(q) == false);	// if this can fail, we'll need to abort & fail the open request !!!
			if (invoke_open) {
				*bdev = &sim->bds[volInd];
				rv = (*bdev)->bd_disk->fops->open(*bdev, mode);
				BUG_ON(rv != 0); // open cannot fail in simulation
			}
			rv = 0;
		}
		mutex_unlock(&q->sysfs_lock);
	}
	mutex_unlock(&disk->lock);
_out:
	mutex_unlock(&sim->disks_lock);
	return rv;
}

/* close a block device, so it can be destroyed. this is similiar to an application closing the block device file
   Assume: no race with destroy of the device. this is for UT code to set the stage */
int osSimulator_diskCloseIdx(struct osSimulator	*sim, unsigned volInd)
{
	struct gendisk 	   *disk = sim->disks[volInd];
	BUG_ON(volInd >= ARRAY_SIZE(sim->disks));
	blk_put_queue(disk->queue);
	put_disk(disk);	// since we hold a reference, the volume must be there
	return 0;
}

int osSimulator_diskfree(struct osSimulator	*sim, struct gendisk *disk)
{
	const int n_max_disks = ARRAY_SIZE(sim->disks);
	int i, rv = EINVAL;
	mutex_lock(&sim->disks_lock);
	for (i=0; i<n_max_disks; i++){
		if (sim->disks[i] == disk){
			pr_info("delete block device %d\n", i);
			sim->disks[      i] = NULL;
			sim->bds[i].bd_disk = NULL;
			sim->bds[i].bd_inode= NULL;
			rv = 0;
			break;
		}
	}
	mutex_unlock(&sim->disks_lock);
	return rv; // not found
}

// return true when volume is attached & ready for IO's, false otherwise
bool osSimulator_diskIsAttached(struct osSimulator	*sim, unsigned volInd)
{
	int 	rv = false;
	BUG_ON(volInd >= ARRAY_SIZE(sim->disks));
	mutex_lock(&sim->disks_lock);
	if (sim->disks[volInd] && sim->bds[volInd].bd_disk)
		rv = true;
	mutex_unlock(&sim->disks_lock);
	return rv;
}

#define __init_default_bio(res, bdev, action, vecs, n_blks) \
	bio_set_dev(res, bdev); \
	res->bi_rw   		= action; \
	res->bi_idx			= 0; \
	res->bi_io_vec 		= vecs; \
	res->comp           = NULL; \

#define __inject_first_n_blocks_as_first_bv_entry_in_one_page(N, blk_size) \
	vecs[ 0].bv_page = &pages[0]; \
	vecs[ 0].bv_len =  (N*blk_size); \
	vecs[ 0].bv_offset = 0; \
	pages[0].mapped_vaddr = arr; \
	/*update the state*/ \
	n_blks -= N; \
	arr = &arr[vecs[0].bv_len]; \
	pages = &pages[1]; \
	res->bi_max_vecs = n_blks+1; \
	res->bi_vcnt = n_blks+1; \
	n_bvecs++; \
	vecs = &vecs[1];

/* Creates bio struct for IO request. A request is Read 'arr' from the volume blocks [start..start+n_blks] or write the array to those blocks.
 * Length of the array must be exactly n_blks*SECTOR_SIZE.
 * Target volume is described by OS (struct block_device)
 * 'action' is READ, WRITE, REQ_DISCARD. Upon write: 'arr' is const and for 'DISCARD' it is not needed at all */
static struct bio* os_createBio(struct block_device* bdev, u64 start, u64 n_blks, u8 arr[], int action){
	const size_t neededMem = sizeof(struct bio) + n_blks * (sizeof(struct bio_vec)+sizeof(struct page));	// Amount of memory needed for bio
	struct bio     *res   = (struct bio*)sim_kzalloc(neededMem, 0);		// Todo: Use bio_alloc()
	struct bio_vec *vecs  = (struct bio_vec *)(res+1);				// Memory after struct bio is given to array vecs
	struct page    *pages = (struct page    *)(vecs+n_blks);		// Array of pages starts after the array of vecs
	const int bdev_block_size = bdev->bd_disk->queue->limits.physical_block_size;
	int i, b, n_blocks_in_page = (PAGE_SIZE/bdev_block_size);
	u64 n_bvecs = 0;
	__init_default_bio(res, bdev, action, vecs, n_blks);
	res->bi_iter.bi_sector = res->bi_sector = (start  * (bdev_block_size/512));			// Offset in units of kernel sectors
	res->bi_iter.bi_size =   res->bi_size =   (n_blks *  bdev_block_size);				// Length in bytes
	get_disk(bdev->bd_disk);										// increase the gendisk ref-count to prevent it from being freed until this IO is completed
	if (!arr) {														// Trim
		res->bi_io_vec = NULL;										// Verify our code does not use io_vec
		goto _out;
	}

	if (unlikely(bdev_block_size == 512) && (n_blks == 8) && !(start % 8)) { // Make IO aligned
		__inject_first_n_blocks_as_first_bv_entry_in_one_page(8, bdev_block_size);
	}

	// Test various build of bio scatter-gather
	//arr - is a sequential memory, the device works with bio_vec
	//if array is longer then 4 pages, then we map it to bio_vec in the following scheme:
	//  -----------------   <== arr
	//  ****                <== first 16KB will be represented by a single entry in the bio_vec
	//     *^               <== [16KB - 20KB] will be represented by another entry, with a small twist
	//                          we actually pass pointer to 12KB and offset 4KB and len 4KB, thus we introduce non zero offset usecase
	//       ************   <== the rest is split as usuall

	//PAY ATTENTION: the different arrays pointers are changed, including input arguments!!!
	if (n_blks > 60) {
		__inject_first_n_blocks_as_first_bv_entry_in_one_page(33, bdev_block_size);
	} else if (n_blks >= 4) {
		__inject_first_n_blocks_as_first_bv_entry_in_one_page(4, bdev_block_size);
		if (n_blks) {			// Inject 1 block with offset
			vecs[ 0].bv_page = &pages[0];
			vecs[ 0].bv_len = bdev_block_size;
			vecs[ 0].bv_offset = bdev_block_size;
			pages[0].mapped_vaddr = &arr[-bdev_block_size];

			//update the state
			n_blks -= 1;
			arr = &arr[bdev_block_size]; //same as above
			pages = &pages[1]; //but we used single page for this
			n_bvecs++;
			vecs = &vecs[1];
		}
	}

	for (i = 0; i < (int)n_blks; i += b) {
		int n_remaining_blocks_in_page = min(n_blocks_in_page, ((int)n_blks-i));
		for (b=0; b<n_remaining_blocks_in_page; b++) { 				// Fill 4KB page
			// Todo: Use bio_add_page()
			vecs[ i+b].bv_page = &(pages[i+b]);					// The page with reference to part of 'arr' array for the IO
			vecs[ i+b].bv_len = bdev_block_size;				// Each vec is an IO of a single block of 4K or 8 blocks of 512[b]
			vecs[ i+b].bv_offset += b     * bdev_block_size;		// Offset of the block in the page (if page includes a few blocks)
			pages[i+b].mapped_vaddr = &arr[i* bdev_block_size];		// Page 'i' covers the [i,i+1,...i+b] part of the array
			n_bvecs++;
		}
	}
	res->bi_max_vecs = res->bi_vcnt = res->bi_phys_segments = n_bvecs;
	if (0) {
		for (i = 0; i < res->bi_vcnt; i++) {
			struct bio_vec *tv = &res->bi_io_vec[i];
			pr_emerg("---- %2d) %p + %05x, len=%05x, pg=%p\n", i, tv->bv_page->mapped_vaddr, tv->bv_offset, tv->bv_len, tv->bv_page);
		}
		pr_emerg("----\n\n\n\n");
	}
_out:
	atomic_inc(&res->bi_bdev->n_active_ios);
	return res;
}

static struct bio* os_createBio_missaligned(struct block_device* bdev, u64 start, u64 n_blks, u8 arr[], int action){
	const size_t neededMem = sizeof(struct bio) + (n_blks + 1)* (sizeof(struct bio_vec)+sizeof(struct page));	// Amount of memory needed for bio
	struct bio     *res   = (struct bio*)sim_kzalloc(neededMem, 0);
	struct bio_vec *vecs  = (struct bio_vec *)(res+1);				// Memory after struct bio is given to array vecs
	struct page    *pages = (struct page    *)(vecs+n_blks+1);		// Array of pages starts after the array of vecs
	u64 i, fraction = 7*(PAGE_SIZE/8);
	BUG_ON(!IS_ALIGNED((u64)arr, PAGE_SIZE));
	WARN_ON(PAGE_SIZE != bdev->bd_disk->queue->limits.physical_block_size);	// Illegal usage of the function
	__init_default_bio(res, bdev, action, vecs, n_blks);
	res->bi_max_vecs = res->bi_vcnt = res->bi_phys_segments = n_blks;
	res->bi_iter.bi_sector = res->bi_sector = (start  * (PAGE_SIZE/512));				// Offset in units of kernel sectors
	res->bi_iter.bi_size =   res->bi_size =   (n_blks *  PAGE_SIZE);					// Length in bytes
	get_disk(bdev->bd_disk);										// increase the gendisk ref-count to prevent it from being freed until this IO is completed
	for (i = 0; i <= n_blks; i++ ) {		// Fill 4KB page
		vecs[ i].bv_page = &(pages[i]);		// The page with reference to part of 'arr' array for the IO
		vecs[ i].bv_len = PAGE_SIZE;		// Each vec is a fraction of i'th block and fraction of i+1 block
		vecs[ i].bv_offset = 0;
		pages[i].mapped_vaddr = &arr[(i << PAGE_SHIFT) + fraction];
	}
	vecs[0].bv_len = PAGE_SIZE - fraction;
	vecs[n_blks].bv_len = vecs[0].bv_offset = fraction;
	atomic_inc(&res->bi_bdev->n_active_ios);
	return res;
}

#define __gd(bio) ((bio)->bi_bdev->bd_disk)
#if KS_REQUEST_QUEUE_HAS_REQUEST_FN
	#define CALL_SUBMIT_BIO_FN(bioReq)		__gd(bioReq)->queue->make_request_fn(__gd(bioReq)->queue, bioReq)
#else
	#define CALL_SUBMIT_BIO_FN(bioReq)		__gd(bioReq)->fops->submit_bio(bioReq)
#endif
static inline int execute_bio_request(struct bio *bioReq, ulong flags, struct completion* comp) {
	bioReq->bi_flags = flags;
	__bio_track_start(bioReq);
	if (flags & (1 << BIO_USER_MAPPED)) {		/* Blocking, beware, can stuck the unitest forever */
		DECLARE_COMPLETION_ONSTACK(stack_comp);
		bioReq->comp = &stack_comp;
		CALL_SUBMIT_BIO_FN(bioReq);
		wait_for_completion(&stack_comp);
	} else {
		if (comp)
			bioReq->comp = comp;
		CALL_SUBMIT_BIO_FN(bioReq);
	}
	/* No need to free bioReq. It is done by block_api_os_end_io() unless initiaing the IO failed */
	return 0;
}

int osSimulator_readArr_f(struct osSimulator* sim, int volInd, u64 startBlock, u64 lenBlocks, u8 dst[], ulong flags, struct completion* comp){
	struct block_device* bdev = &sim->bds[volInd];
	struct bio *bioReq = os_createBio(bdev, startBlock, lenBlocks, dst, READ);
	return execute_bio_request(bioReq, flags, comp);
}

int osSimulator_readArr_f_no_vcnt(struct osSimulator* sim, int volInd, u64 startBlock, u64 lenBlocks, u8 dst[], ulong flags, struct completion* comp){
	struct block_device* bdev = &sim->bds[volInd];
	struct bio *bioReq = os_createBio(bdev, startBlock, lenBlocks, dst, READ);
	bioReq->bi_vcnt = 0;
	return execute_bio_request(bioReq, flags, comp);
}

int osSimulator_writeArr_f(struct osSimulator* sim, int volInd, u64 startBlock, u64 lenBlocks, const u8 src[], ulong flags, struct completion* comp){
	struct block_device* bdev = &sim->bds[volInd];
	struct bio *bioReq = os_createBio(bdev, startBlock, lenBlocks, (u8*)src, WRITE);
	return execute_bio_request(bioReq, flags, comp);
}

int osSimulator_writeArr_f_missaligned(struct osSimulator* sim, int volInd, u64 startBlock, u64 lenBlocks, const u8 src[], ulong flags, struct completion* comp){
	struct block_device* bdev = &sim->bds[volInd];
	struct bio *bioReq = os_createBio_missaligned(bdev, startBlock, lenBlocks, (u8*)src, WRITE);
	return execute_bio_request(bioReq, flags, comp);
}

int osSimulator_writeArr_f_no_vcnt(struct osSimulator* sim, int volInd, u64 startBlock, u64 lenBlocks, const u8 src[], ulong flags, struct completion* comp){
	struct block_device* bdev = &sim->bds[volInd];
	struct bio *bioReq = os_createBio(bdev, startBlock, lenBlocks, (u8*)src, WRITE);
	bioReq->bi_vcnt = 0;
	return execute_bio_request(bioReq, flags, comp);
}

int osSimulator_trim_f(struct osSimulator* sim, int volInd, u64 startBlock, u64 lenBlocks, ulong flags){
	struct block_device* bdev = &sim->bds[volInd];
	struct bio *bioReq = os_createBio(bdev, startBlock, lenBlocks, NULL, REQ_DISCARD|WRITE);		// Discard is always a write action as well
	return execute_bio_request(bioReq, flags, NULL);
}

void __bio_track_start(struct bio *b) {
	struct osSimulator *sim = (struct osSimulator *)__gd(b)->kernel_ptr;
	const int vol_i = __gd(b)->vol_i;
	BUG_ON(!sim);

	if (vol_i >= sim->di_tracker->nvols)
		return;

	if (sim->di_tracker->volume_di_trackers[vol_i].enabled) {	// unsafe access but it's OK since illegal I/Os are tested with tracker disabled well beforehand
		BUG_ON((b->bi_sector >> KERNEL_SECTOR_TO_SECTOR_SHIFT) << KERNEL_SECTOR_TO_SECTOR_SHIFT != b->bi_sector);
		BUG_ON((b->bi_size >> NVMEIBC_SECTOR_SHIFT) << NVMEIBC_SECTOR_SHIFT != b->bi_size);
	}

	b->dt_io_ctx = volume_di_tracker_track_io_start(
			&sim->di_tracker->volume_di_trackers[vol_i],
			b->bi_sector >> KERNEL_SECTOR_TO_SECTOR_SHIFT,
			b->bi_size >> NVMEIBC_SECTOR_SHIFT,
			b->bi_rw,
			b->bi_io_vec,
			b->bi_vcnt);
}

void __bio_track_end(struct bio *b, int rv) {
	struct osSimulator *sim = (struct osSimulator *)__gd(b)->kernel_ptr;
	const int vol_i = __gd(b)->vol_i;
	if (vol_i >= sim->di_tracker->nvols)
		return;

	volume_di_tracker_track_io_end(&sim->di_tracker->volume_di_trackers[vol_i], b->dt_io_ctx, rv);
	b->dt_io_ctx = NULL;
}


static void __dump_workqueue_emerg(struct workqueue_struct* wq, struct charvec buffer)
{
	struct charvec txt_result = {0};
	struct nvmeib_txt txt = nvmeib_txt_make(buffer);
	workqueue_dump(wq, &txt, true);
	txt_result = nvmeib_txt_finalize(&txt);
	BUG_ON(txt_result.base == NULL); //it means we don't allocated enough memory
	pr_emerg("%s\n", buffer.base);
}

void kernel_sim_dump_state(void) {
	#define KERN_DUMP_SIZE 	(10*4096)
	struct workq_struct	*wq;
	char *buf = sim_kmalloc(KERN_DUMP_SIZE, 0);
	struct charvec txt_buf = {.base=buf, .len=KERN_DUMP_SIZE};
	BUG_ON(!txt_buf.base);

	pr_emerg("------------Kernel Simulator Dump start -------------------\n");
	ecpu_set_dump_queues(&kernel_sim.ecpu_set, buf, KERN_DUMP_SIZE);
	pr_emerg("pending eCPU tasks:%s\n", buf);
	kernel_timers_dump(&kernel_sim.timers, buf, KERN_DUMP_SIZE);
	pr_emerg("pending timers:\n%s", buf);

	__dump_workqueue_emerg(system_wq, txt_buf);

	wq = kernel_work_queues_get_by_name(&kernel_sim.wqs, "c_main_wq");
	if (wq) {
		__dump_workqueue_emerg(wq, txt_buf);
	}
	wq = kernel_work_queues_get_by_name(&kernel_sim.wqs, TOMA_THREAD_NAME);
	if (wq) {
		__dump_workqueue_emerg(wq, txt_buf);
	}
	pr_emerg("------------Kernel Simulator Dump ends -------------------\n");
	sim_kfree(buf);
}
/*****************************************************************************/
// EOF.
