/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block.h"					// Must be first for simulator
#include "nvmeib_public.h"
#include "nvmeib_types.h"

#include "nvmeibc_disk.h"
#include "nvmeibc_disk_locks.h"
#include "nvmeibc_pausable.h"
#include "nvmeibc_jam.h"

// #define DEBUG
// #define CONFIG_NVMEIB_DEBUG
#include "nvmeib_utils.h"

/* Verifies that interrupt flags are not modified by the call to the transport layer. Todo: check for identical irq before/after.
 * Assumption: kthreads are not migrated to another cpu
 *
 * If transport completes (io) cmd inline, block will issue next cmd (unlock) inline ->  DEBUG_IRQS_DISABLED will false alarm.
 */
#ifndef NVMEIB_TRANSPORT_AUTOCOMP_IO
#define DEBUG_IRQS_DISABLED
#endif

#ifdef DEBUG_IRQS_DISABLED
	#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR == 1)
		extern bool ut_conf__platform_io_sync_get(void);
		extern void crash_if_irqs_disabled(void);
		#define IRQS_DISABLED_STORE   bool disabled_before = irqs_disabled()
		#define IRQS_DISABLED_VERIFY  if (!disabled_before) { crash_if_irqs_disabled(); }
		// In simulator sync mode the assumption pause_preventers == 1 during put_pause_state() is not true, since callback are called synchronously and may do a round trip and increase pause_preventers again
		#define BUG_SCHED_IN_ATOMIC_SECTION  BUG_ON(!ut_conf__platform_io_sync_get())
	#else
		#define IRQS_DISABLED_STORE   bool disabled_before = irqs_disabled()
		#define IRQS_DISABLED_VERIFY  WARN_ON_ONCE(!disabled_before && irqs_disabled())
		#define BUG_SCHED_IN_ATOMIC_SECTION  BUG()
	#endif
#else
	#define IRQS_DISABLED_STORE
	#define IRQS_DISABLED_VERIFY
#endif

/* Debug the sum of per-cpu disk transfers correctnes by using a single atomic
   counter instead and testing it correctness */
#ifdef DEBUG_SUM
	static void DEBUG_SUM__dec_trans(struct nvmeibc_disk *disk)
	{
		const int a_sum = atomic_dec_return(&disk->in_transfers);
		if (a_sum < 0) {
			_NE(DEBUG_SUM__dec_trans_e1, DMESG_PD_PREFIX("@DISK_NAME") ": a_sum=@INT", disk->name, a_sum);
			WARN_ON(true);	// Daniel todo: put BUG()
		}
	}

	static void DEBUG_SUM__inc_trans(struct nvmeibc_disk *disk)
	{
		WARN_ON(disk->pausing);	// Illegal to issue new transfers
		atomic_inc(&disk->in_transfers);
	}

	static void DEBUG_SUM__is_trans(struct nvmeibc_disk *disk, int sum)
	{
		const int a_sum = atomic_read(&disk->in_transfers);
		_NT(DEBUG_SUM__is_trans_t1, DMESG_PD_PREFIX("@DISK_NAME") "(@PTR) t_sum=@INT a_sum=@INT", disk->name, disk, sum, a_sum);
		if (a_sum < 0) {
			_NE(DEBUG_SUM__is_trans_e1,DMESG_PD_PREFIX("@DISK_NAME") ": t_sum=@INT a_sum=@INT", disk->name, sum, a_sum);
			WARN_ON(true);	// Daniel todo: put BUG()
		}
	}

	static void DEBUG_SUM__verify_sum_trans_zero(const struct nvmeibc_disk *disk)
	{
		const int a_sum = atomic_read(&disk->in_transfers);
		BUG_ON(a_sum);
	}
#else
	#define DEBUG_SUM__dec_trans(disk)
	#define DEBUG_SUM__inc_trans(disk)
	#define DEBUG_SUM__is_trans(disk, sum)
	#define DEBUG_SUM__verify_sum_trans_zero(disk)
#endif
#define __verify_valid_pause_preventer(ppi) ({ int __i = (ppi); (void)__i; /*BUG_ON((__i<0)||(__i>1)) */ })


static int __get_remaining_transferring(struct nvmeibc_disk *disk);

static void __dump_all_cpus_pause_state(const struct nvmeibc_disk *disk)
{
	int i;
	_NW(t_01_cplcs, DMESG_PD_PREFIX("@DISK_NAME") ": should_pause=@SHOULD_PAUSE, pausing=@PAUSING", disk->full_name, disk->should_pause, disk->pausing);
	for_each_possible_cpu(i) {
		_NW(t_02_cplcs, DMESG_PD_PREFIX("@CPU") ": pause_preventers=@PAUSE_PREVENTERS", i, per_cpu_ptr(disk->percpu, i)->pause_preventers);
	}
}
/* Use this method only when should pause is on (should_pause!=0).
 * The method sums all pause preventer on all cpu's. It is used to update
 * transition from should_pause to pausing (when no preventers)
 * WARNING: must be executed with interrupt disabled or counting will be
 * incorrect! */
static int __get_sum_all_cpu_preventers(const struct nvmeibc_disk *disk)
{
	int i, sum = 0;		/* IMPORTANT!! typeof(sum) == typeof(pause_preventers)*/
	NFIN;
	for_each_possible_cpu(i) {
		const int ppi = per_cpu_ptr(disk->percpu, i)->pause_preventers;
		__verify_valid_pause_preventer(ppi);
		sum += ppi;
	}
	if (sum < 0) {    /* Sum is also < nr_cpu_ids (number of CPU's) */
		_NE(error_pausable_get_sum_all_cpu_preventers, DMESG_PD_PREFIX("@DISK_NAME") ": sum=@SUM", disk->name, sum);
		BUG();
	}
	_ND(trace_pausable_get_sum_all_cpu_preventers, "@DISK_NAME: pausing disk=@DISK sum=@SUM", disk->name, disk, sum);
	NFOUT;
	return sum;
}

/* Current cpu will wait for all other cpu's to drop their preventers to 0.
   can_self_prevent - whether current cpu can hold a preventer. WARNING: If 2+
   cpu's (each with preventer) call this function it will be a deadlock */
static void __wait_for_pausing(struct nvmeibc_disk *disk, bool can_self_prevent)
{
	struct disk_percpu	*dp;
	int cpuid, count, my_cpu_preventers, sum_all_preventers = 0;
	unsigned long flags;
	unsigned long wait_pause_last_dump = jiffies;
	unsigned long wait_pause_last_print = jiffies;

	NFIN;
	local_irq_save(flags);
	cpuid = get_cpu();
	dp = per_cpu_ptr(disk->percpu, cpuid);
	my_cpu_preventers = dp->pause_preventers;
	WARN(!disk->should_pause, DMESG_PD_PREFIX("%s") "cont arrived, too early\n", disk->name);
	WARN_ON((!can_self_prevent) && (my_cpu_preventers != 0)); // Transport layer made illegal call. We expect this CPU to have zero preventers
	for (count = 1; !disk->pausing; count++) {
		if (jiffies - wait_pause_last_print > HZ) {
			_NW(warn_pausable_wait_for_pausing, DMESG_PD_PREFIX("@DISK_NAME") ": @CPU: Looping @COUNT times, waiting for pausing(@SUM_ALL_PREVENTERS) on disk=@DISK", disk->name, cpuid, count, sum_all_preventers, disk);
			wait_pause_last_print = jiffies;
		}
		if (jiffies - wait_pause_last_dump > 5*HZ) {
			__dump_all_cpus_pause_state(disk);
			wait_pause_last_dump = jiffies;
		}
		if (!(count & 0xfffff)) {
			_NE(err_pausable_wait_for_pausing, DMESG_PD_PREFIX("@DISK_NAME") ": Disk=@DISK Stuck!", disk->name, disk);
		}
		sum_all_preventers = __get_sum_all_cpu_preventers(disk);
		if (sum_all_preventers == my_cpu_preventers) {
			disk->pausing = true;
			WARN(!disk->should_pause, DMESG_PD_PREFIX("%s") "cont arrived, too early\n", disk->name);
		}
		udelay(10);
	}
	if (my_cpu_preventers != 0) {
		/* Very unlikely case: while issuing executing a transfer (transport
		   layer encountered an error and called for disk pause on this cpu.
		   Loop above waited for all other cpu's pause_preventers to == 0, but
		   this cpu has 1 preventer in it's stack so it never drops to zero */
		_NT(trace_pausable_wait_for_pausing, "@DISK_NAME: Self prevent, pprev=@PPREV", disk->name, my_cpu_preventers);
		/* This cpu disabled interrupts so it can have exactly 1 preventer */
		WARN_ON(my_cpu_preventers != 1);	// Daniel todo: put BUG()
	}

	put_cpu();
	local_irq_restore(flags);
	NFOUT;
}

static const char *pd_reason_str(int reason);

/* This method must be called, right after the disk action (that was approved
 * to be safe by __get_pause_state()). If disk action was not approved, don't
 * perform it and don't call this method */
static void __put_pause_state(struct nvmeibc_disk *disk, unsigned long flags, int reason)
{
	struct disk_percpu *dp;
#ifdef DEBUG_IRQS_DISABLED
	extern uint nvmeibc_skip_disk_iocmds_flags;
	extern uint nvmeibc_skip_lock_cmds_flags;
#endif
	NFIN;
	dp = per_cpu_ptr(disk->percpu, get_cpu());

	#ifdef DEBUG_IRQS_DISABLED
		if (((!irqs_disabled()) || (dp->pause_preventers != 1)) &&
			 likely(!(nvmeibc_skip_disk_iocmds_flags | nvmeibc_skip_lock_cmds_flags))) { /* catch sched while atomic */
			_NE(__put_pause_state_e1, DMESG_PD_PREFIX("@DISK_NAME") ": (@PTR), irqs_disabled=@INT, cpu-pause_preventers=@INT, pd_reason=@PD_REASON_STR, local_bypass=@BOOL_YN",
			disk->name, disk, irqs_disabled(), dp->pause_preventers, pd_reason_str(reason), disk->access_local);
			BUG_SCHED_IN_ATOMIC_SECTION;
		}
	#endif
	dp->pause_preventers--;
	put_cpu();
	__verify_valid_pause_preventer(dp->pause_preventers);
	local_irq_restore(flags);
	NFOUT;
}

/* This method must be called before each action of block device that uses the
 * disk (IO,locks,etc).
 * Returns 0 if it's safe to continue with the disk action.
 * Once the action is started it will prevent the disk from pausing
 * Returns other value if disk is pausing and the action should not be taken,
 * because disk will not be able to perform it.
 */
static bool __get_pause_state(struct nvmeibc_disk *disk, unsigned long *flags, int pd_reason)
{
	bool should_pause;
	struct disk_percpu *dp;
	NFIN;
	local_irq_save(*flags);
	dp = per_cpu_ptr(disk->percpu, get_cpu());
	dp->pause_preventers++;
	__verify_valid_pause_preventer(dp->pause_preventers);
	should_pause = disk->should_pause;
	put_cpu();
	if (should_pause) {
		__put_pause_state(disk, *flags, pd_reason);
	}
	NFOUT;
	return should_pause;
}

int nvmeibc_pd_pause(struct nvmeibc_disk *disk,
	void (*cb)(void *cntx), void *cntx)
{
	struct pause_req *p_req = NULL;
	int rv = 0, transferring;
	unsigned long flags;

	_NT(trace_pausable_nvmeibc_pd_pause, "@DISK_NAME: Pause (@DISK), cb=@CB, cntx=@CNTX", disk->name, disk, cb, cntx);
	/* Note: if cb == NULL -> We are in interrupt context, and this CPU can hold
	   pause preventer. cb != NULL -> We are on disk wq and our CPU's pause
	   preventers count == 0. Call to this function with cb == NULL on 2 cpu's
	   may result in dead lock.
	   interrupt ctx call is meant to signal the block layer to stop sending
	   IO's. this allows the draining to begin ASAP. */
	__wait_for_pausing(disk, (cb == NULL));
	// Here, by definition disk->pausing == true.
	if (!cb)
		goto out;

	spin_lock_irqsave(&disk->pause_reqs_lock, flags);
	transferring = __get_remaining_transferring(disk);
	_NT(trace_1_pausable_nvmeibc_pd_pause, "@DISK_NAME: Transferring=@TRANSFERRING", disk->name, transferring);
	if (transferring != 0) {
		if (!(p_req = kmalloc(sizeof(struct pause_req), GFP_ATOMIC))) {
			_NE(error_pausable_nvmeibc_pd_pause, DMESG_PD_PREFIX("@DISK_NAME") ": out of memory pause request cb=@CB, cntx=@CNTX", disk->name, cb, cntx);
			rv = -ENOMEM;
		} else {
			p_req->cb = cb;
			p_req->cntx = cntx;
			_NT(trace_2_pausable_nvmeibc_pd_pause, "@DISK_NAME: adding - cb=@CB, cntx=@CNTX", disk->name, p_req->cb, p_req->cntx);
			list_add_tail(&p_req->reqs, &disk->pause_reqs);
		}
		spin_unlock_irqrestore(&disk->pause_reqs_lock, flags);
	} else {
		DEBUG_SUM__verify_sum_trans_zero(disk);
		disk->pausing_no_transfers = true;
		spin_unlock_irqrestore(&disk->pause_reqs_lock, flags);
		/* Immediate call back*/
		_NT(trace_3_pausable_nvmeibc_pd_pause, "Pause callback for disk=@DISK, cb=@CB, cntx=@CNTX", disk, cb, cntx);
		(*cb)(cntx);
	}

out:
	_ND(trace_4_pausable_nvmeibc_pd_pause, "### disk=@DISK, cb=@CB, cntx=@CNTX, rv=@RV", disk, cb, cntx, rv);
	return rv;
}

#include "nvmeibc_volume.h"


/* Call fn for each disk of each volume that this disk is a member of */
static void call_for_each_disks_vols_disks(struct nvmeibc_disk *disk, void (*fn)(struct nvmeibc_disk *)) {
	struct nvmeibc_disk_id *disk_id_out, *disk_id_in;
	unsigned long flags;
	spin_lock_irqsave(&disk->volume_spinlock, flags);
	list_for_each_entry(disk_id_out, &disk->volumes, slink) {
		if (disk_id_out->volume) {
			nvmeibc_volume_get(disk_id_out->volume, NULL);
			list_for_each_entry(disk_id_in, &disk_id_out->volume->info.disks, link) {
				(*fn)(disk_id_in->disk);
			}
			nvmeibc_volume_put(disk_id_out->volume, NULL);
		}
	}
	spin_unlock_irqrestore(&disk->volume_spinlock, flags);
}

static void __wait_for_cont_preventors_to_end(struct nvmeibc_disk *disk){
	ulong last_dump = jiffies, last_print = jiffies;
	int count;
	for (count = 1; (atomic_read(&disk->n_cont_preventors)>0); count++) {
		if (jiffies - last_print > HZ/16) { /* Roughly ~60[mSec] */
			if (!disk->n_cont_prevents_waited_too_long) { // while not cirtical error, just print to dmesg
				_NT(t_01_pausable, "@DISK_NAME: Looping (@COUNT times), waiting for cont, n_preventers=@INT", disk->name, count, atomic_read(&disk->n_cont_preventors));
			}
			last_print = jiffies;
			cond_resched();
		}
		if (jiffies - last_dump > 5*HZ) {
			/*WARN_ON_ONCE(true);*/  /* After ~5[sec] Stop printing, do warning: EC-4571 reproduction */
			_NE(t_02_pausable, DMESG_PD_PREFIX("@DISK_NAME") ": Waiting for cont, n_preventers=@INT", disk->name, atomic_read(&disk->n_cont_preventors));
			call_for_each_disks_vols_disks(disk, nvmeibc_pd_dump_transfers);
			disk->n_cont_prevents_waited_too_long = true;	// Notify next preventer that it took too much time
			last_dump = jiffies;
		}
		udelay(10);
	}
	disk->n_cont_prevents_waited_too_long = false;	// Last cont preventer finished
	DEBUG_SUM__verify_sum_trans_zero(disk);
}

int nvmeibc_pd_cont(struct nvmeibc_disk *disk)
{
	NFIN;
	__wait_for_cont_preventors_to_end(disk);
	mb(); // be safe - dont count on the above call (wait loop) to be a memory-barrier. ensure we dont write any of the following before we complete the wait.
	/*BUG_ON(!disk->should_pause); - May be 0 if volume dettached and attached*/
	disk->pausing = false;	// Only place in code where pausing becomes false
	disk->should_pause = false;
	disk->pausing_no_transfers = false;
	NFOUT;
	return 0;
}

/* Mark that a given disk started io transfer (read/write)
 * This is always called with irqs_disabled, so no need for asm incl */
static void __inc_transferring(struct nvmeibc_disk *disk)
{
	volatile int *p_it;
	NFIN;
	DEBUG_SUM__inc_trans(disk);
	p_it = &(per_cpu_ptr(disk->percpu, get_cpu())->in_transfers);
	atomic_inc_per_cpu_volatile_int(*p_it);
	put_cpu();

	NFOUT;
}

static int __get_sum_all_in_transfer(const struct nvmeibc_disk *disk)
{
	int i, sum = 0; /* IMPORTANT!! typeof(sum) == typeof(in_transfers)*/
	for_each_possible_cpu(i)
		sum += per_cpu_ptr(disk->percpu, i)->in_transfers; // Overflowing sum
	return sum;
}

/* Call this method in pausing state only (new transfers not starting) and with
   irq disabled. Returns how much IO transfers occuring now */
static int __get_remaining_transferring(struct nvmeibc_disk *disk)
{
	int sum;

	NFIN;
	WARN_ON(!disk->pausing);	// If true, summation will give arbitrary result
	sum = __get_sum_all_in_transfer(disk);
	DEBUG_SUM__is_trans(disk, sum);
	/* Sum can be very big, but no overflow (2G IO's in air) */
	WARN((sum < 0), DMESG_PD_PREFIX("%s") "sum=%d\n", disk->name, sum);
	NFOUT;
	return sum;
}

/* Mark that a given disk finished io transfer (read/write of data
 * or updating locks). When finished, return all pending pause call backs */
static void __dec_transferring(struct nvmeibc_disk *disk)
{
	bool pausing = disk->pausing; /* Copy here, to avoid change in function */
	unsigned long flags = 0;
	int cpuid;
	volatile int *p_it;
	NFIN;

	if (pausing) {
		spin_lock_irqsave(&disk->pause_reqs_lock, flags);
	}

	DEBUG_SUM__dec_trans(disk);
	cpuid = get_cpu();
	p_it = &(per_cpu_ptr(disk->percpu, cpuid)->in_transfers);
	atomic_dec_per_cpu_volatile_int(*p_it);
	put_cpu();

	if (disk->pausing) {		/* Intentionally not using 'pausing' */
		bool should_send_resp;
		if (!pausing) /*disk->pausing was turned on inside __dec_transferring*/
			spin_lock_irqsave(&disk->pause_reqs_lock, flags);
		should_send_resp = ((disk->pausing) && (__get_remaining_transferring(disk) == 0));
		if (should_send_resp) {
			/* Ready to transition from should pause to pausing.
			   Conusme all pause requests in the list and respond */
			struct list_head callbacks;
			struct pause_req *entry = NULL;
			DEBUG_SUM__verify_sum_trans_zero(disk);
			INIT_LIST_HEAD(&callbacks);
			list_splice_init(&disk->pause_reqs, &callbacks);
			disk->pausing_no_transfers = true;
			spin_unlock_irqrestore(&disk->pause_reqs_lock, flags);
			_ND(trace_pausable_dec_transferring, "@DISK_NAME: sending pause_resp", disk->full_name);

			/* Now disk->pause_reqs and spinlock are ready for next pause. */
			while (!list_empty(&callbacks)) {
				entry = list_first_entry(&callbacks, struct pause_req, reqs);
				list_del(&entry->reqs);
				_NT(trace_1_pausable_dec_transferring, "@DISK_NAME: removing - cb=@CB, cntx=@CNTX", disk->name, entry->cb, entry->cntx);
				(*((void (*)(void *))entry->cb))(entry->cntx);
				kfree(entry);
			}
		} else {
			_ND(trace_2_pausable_dec_transferring, "@DISK_NAME: not sending pause_resp", disk->full_name);
			spin_unlock_irqrestore(&disk->pause_reqs_lock, flags);
		}
	} else {
		if (pausing) {
			/* pd_cont() called during dec transferring. How on earth ??? */
			WARN_ON(true);
			spin_unlock_irqrestore(&disk->pause_reqs_lock, flags);
		}
	}
	NFOUT;
}

enum e_pd_reason {	// Debug ENUM grouped by completion type
	/***************** RDMA ops *****************/
	NVMEIBC_PD_REASON_CMPXCHG =				1,
	NVMEIBC_PD_REASON_READ_LOCK =			4,
	NVMEIBC_PD_REASON_WRITE_DBIT =			5,
	NVMEIBC_PD_REASON_READ_DBIT =			6,
	NVMEIBC_PD_REASON_GET_BLKST_PROBLEMS =	7,
	NVMEIBC_PD_REASON_BINFO_WRITE =			8,
	NVMEIBC_PD_REASON_BINFO_READ =			9,
	/***************** IO disk commands *****************/
	NVMEIBC_PD_REASON_EXEC_IO_BASE =		0x100, // 0x100..0x1FF {0x100+(enum nvmeib_block_io_op)}
	NVMEIBC_PD_REASON_EXEC_JOUR_BASE =		0x200, // 0x200..0x2FF {0x200+(enum nvmeib_block_io_op)}
	/***************** Gen commands commands *****************/
	NVMEIBC_PD_REASON_EXECUTE_GEN_BASE =	0x300, // 0x300..0x32b {0x300+(enum nvmeib_wr_opcode)}
	/*********** Temp Ugly hacks which should become GEN_BASE ******/
	NVMEIBC_PD_REASON_READ_JCMD =			0x400,
	NVMEIBC_PD_REASON_FREE_JRNL_ENTS =		0x401,
	/***************** Untracked instructions *****************/
	NVMEIBC_PD_REASON_JAM_GET =				0x500,
	NVMEIBC_PD_REASON_JAM_GETALL =			0x501,
	NVMEIBC_PD_REASON_TOMA_GET =			0x502,
	NVMEIBC_PD_REASON_REUSED_BB_RELEASE =	0x503,
	NVMEIBC_PD_REASON_DRAIN_DEFERRED_LOCKS = 0x504,

	NVMEIBC_PD_REASON_DBG_CMD =				0x550,

	NVMEIBC_PD_REASON_MAX_INVALID =			0x600,
};

static __attribute__ ((unused)) const char *pd_reason_str(int reason)
{
	switch (reason) {
	case NVMEIBC_PD_REASON_CMPXCHG: return "CMPXCHG";
	case NVMEIBC_PD_REASON_READ_LOCK: return "READ_LOCK";
	case NVMEIBC_PD_REASON_WRITE_DBIT: return "WRITE_DBIT";
	case NVMEIBC_PD_REASON_READ_DBIT: return "READ_DBIT";
	case NVMEIBC_PD_REASON_GET_BLKST_PROBLEMS: return "GET_BLKST_PROBLEMS";
	case NVMEIBC_PD_REASON_BINFO_WRITE: return "BINFO_WRITE";
	case NVMEIBC_PD_REASON_BINFO_READ: return "BINFO_READ";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_IO_BASE + NVMEIB_BLOCK_IO_OP_READ: return "IO_READ";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_IO_BASE + NVMEIB_BLOCK_IO_OP_WRITE: return "IO_WRITE";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_IO_BASE + NVMEIB_BLOCK_IO_OP_DISCARD: return "IO_DISCARD";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_IO_BASE + NVMEIB_BLOCK_IO_OP_WRITE_UNCOR: return "WRITE_UNCOR";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_JOUR_BASE + NVMEIB_BLOCK_IO_OP_READ: return "JOUR_READ";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_JOUR_BASE + NVMEIB_BLOCK_IO_OP_WRITE: return "JOUR_WRITE";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_JOUR_BASE + NVMEIB_BLOCK_IO_OP_DISCARD: return "JOUR_DISCARD";
	case (unsigned)NVMEIBC_PD_REASON_EXEC_JOUR_BASE + NVMEIB_BLOCK_IO_OP_WRITE_UNCOR: return "WRITE_UNCOR";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_GET_UUID_JOUR: return
		"GEN_OP_GET_UUID_JOUR";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_BLKSET_RECOVERED: return "GEN_OP_BLKSET_RECOVERED";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_GET_EC_DB: return "GEN_OP_GET_EC_DB";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_FREE_JRNL_ENTS: return "GEN_OP_FREE_JRNL_ENTS";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_LOCK: return "GEN_OP_LOCK";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_GET_JMDC: return "GEN_OP_GET_JMDC";
	case (unsigned)NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + NVMEIB_GEN_OP_JENTRY_ERASE: return "GEN_OP_JENTRY_ERASE";
	case NVMEIBC_PD_REASON_READ_JCMD: return "READ_JMDC";
	case NVMEIBC_PD_REASON_FREE_JRNL_ENTS: return "FREE_JRNL_ENTS";
	case NVMEIBC_PD_REASON_JAM_GET: return "JAM_GET";
	case NVMEIBC_PD_REASON_JAM_GETALL: return "JAM_GETALL";
	case NVMEIBC_PD_REASON_TOMA_GET: return "TOMA_GET";
	case NVMEIBC_PD_REASON_REUSED_BB_RELEASE: return "REUSED_BB_RELEASE";
	default: return "UNKNOWN";
	}
}

#ifdef DEBUG_TRANSFERS

#define INC_TRANSFER(disk, object, value) ({\
	if (object) { \
		int cpu = get_cpu(); \
		unsigned long _flags; \
		(object)->reason.type = value; \
		spin_lock_irqsave(&disk->transfer_spinlock[cpu], _flags); \
		list_add_tail(&(object)->reason.link, &disk->transferring[cpu]); \
		disk->n_transferring[cpu]++; \
		(object)->reason.stack[(object)->reason.sp] = value; \
		(object)->reason.sp = ((object)->reason.sp+1)%DEBUG_TRANSFERS_STACK_SIZE; \
		(object)->reason.inc_cpu = cpu; \
		(object)->in_flight = true; \
		spin_unlock_irqrestore(&disk->transfer_spinlock[cpu], _flags); \
		put_cpu();\
	} else {\
		int cpu = get_cpu(); \
		unsigned long _flags; \
		local_irq_save(_flags);\
		disk->n_untracked_transferring[cpu]++;\
		local_irq_restore(_flags);\
		put_cpu();\
	}\
	__inc_transferring(disk);\
})

#define DEC_TRANSFER(disk, object) ({\
	if (object) { \
		int inc_cpu = (object)->reason.inc_cpu;\
		unsigned long _flags; \
		spin_lock_irqsave(&disk->transfer_spinlock[inc_cpu], _flags); \
		BUG_ON(list_empty(&(object)->reason.link)); \
		list_del_init(&(object)->reason.link); \
		disk->n_transferring[inc_cpu]--; \
		(object)->reason.stack[(object)->reason.sp] = -(object)->reason.type; \
		(object)->reason.sp = ((object)->reason.sp+1)%DEBUG_TRANSFERS_STACK_SIZE; \
		(object)->reason.inc_cpu = -1; \
		(object)->in_flight = false; \
		spin_unlock_irqrestore(&disk->transfer_spinlock[inc_cpu], _flags); \
	} else {\
		int cpu = get_cpu(); \
		unsigned long _flags; \
		local_irq_save(_flags);\
		disk->n_untracked_transferring[cpu]--;\
		local_irq_restore(_flags);\
		put_cpu();\
	}\
	__dec_transferring(disk);\
})

void nvmeibc_pd_dump_transfers(struct nvmeibc_disk *disk)
{
	unsigned long flags;
	struct list_head *pos;
	char cntr_string[64];
	int i = 0, cpu;
	unsigned long long n_xfer = 0;

	if (disk == NULL) {
		_NT(t_bf_dp_dbg_tools, DMESG_PD_PREFIX() "disk ptr=NULL");
		goto _out;					// Skip
	}
	for_each_possible_cpu(cpu) {
		spin_lock_irqsave(&disk->transfer_spinlock[cpu], flags);
		n_xfer += disk->n_transferring[cpu];
		spin_unlock_irqrestore(&disk->transfer_spinlock[cpu], flags);
	}
	_NT(t_bg_dp_dbg_tools, DMESG_PD_PREFIX("@DISK_NAME") "ptr=@DISK, Waiting for completions (n=@SIZE_LLONG):", disk->name, disk, n_xfer);
	for_each_possible_cpu(cpu) {
		spin_lock_irqsave(&disk->transfer_spinlock[cpu], flags);
		list_for_each(pos, &disk->transferring[cpu]) {
			struct nvmeibc_transfer_reason *rsn;
			void *caller;
			rsn = container_of(pos, struct nvmeibc_transfer_reason, link);
			if (       rsn->type < NVMEIBC_PD_REASON_EXEC_IO_BASE) {
				caller = container_of(rsn, struct nvmeibc_d_rdma_comp, reason);	// RDMA's
			} else if (rsn->type < NVMEIBC_PD_REASON_EXEC_JOUR_BASE) {
				caller = container_of(rsn   , struct nvmeibc_disk_command, reason);	// NVMEIBC_PD_REASON_EXEC_IO_BASE
				caller = container_of(caller, struct nvmeibc_disk_io_command, disk_cmd);
			} else if (rsn->type < NVMEIBC_PD_REASON_EXECUTE_GEN_BASE) {
				caller = container_of(rsn   , struct nvmeibc_disk_command, reason);	// NVMEIBC_PD_REASON_EXEC_JOUR_BASE
				caller = container_of(caller, struct nvmeibc_disk_io_command, disk_cmd);
			} else if (rsn->type < NVMEIBC_PD_REASON_READ_JCMD) {
				caller = container_of(rsn   , struct nvmeibc_disk_command, reason);	// NVMEIBC_PD_REASON_EXECUTE_GEN_BASE
				caller = container_of(caller, struct nvmeibc_disk_gen_cmd, disk_cmd);
			} else if (rsn->type == NVMEIBC_PD_REASON_READ_JCMD) {
				caller = container_of(rsn, struct nvmeibc_disk_jmdc_read_comp, reason);
			} else if (rsn->type == NVMEIBC_PD_REASON_FREE_JRNL_ENTS) {
				caller = container_of(rsn, struct nvmeibc_disk_free_jrnl_ents_comp, reason);
			} else {
				caller = NULL;	// Unknown caller. BUG?
			}
			_NT(t_bh_dp_dbg_tools, DMESG_PD_PREFIX() "(@INDEX) type=@INDEX ptr=@PTR inc_cpu=@CPU\n", i, rsn->type, caller, cpu);
			i++;
		}
		spin_unlock_irqrestore(&disk->transfer_spinlock[cpu], flags);
	}
	nvmeibc_pd_tostring(disk, cntr_string, sizeof(cntr_string));
	_NT(t_bi_dp_dbg_tools, DMESG_PD_PREFIX() "@STR", cntr_string);
	_NT(t_bj_dp_dbg_tools, DMESG_PD_PREFIX() "Total tracked pending transfers: @INDEX\n", i);
_out:;
}

#else // DEBUG_TRANSFERS
	#define INC_TRANSFER(disk, object, value) ({ __inc_transferring(disk); })
	#define DEC_TRANSFER(disk, object)        ({ __dec_transferring(disk); (void)object; })
	void nvmeibc_pd_dump_transfers(struct nvmeibc_disk *disk) {(void)disk; }
#endif // DEBUG_TRANSFERS

// Daniel: Very bad idea, introducing this for support of Omri L. JAM 01/02/2018. Todo, Track also JAM requests
#define UNTRACKED_TRANSFER ((struct nvmeibc_disk_command *)NULL)

void nvmeibc_pd_cb_called_comp(struct nvmeibc_disk *disk,
	struct nvmeibc_d_rdma_comp *comp)
{
	DEC_TRANSFER(disk, comp);
}

void nvmeibc_pd_cb_called_cmd(struct nvmeibc_disk *disk,
		struct nvmeibc_disk_command *cmd)
{
	DEC_TRANSFER(disk, cmd);
}

void nvmeibc_pd_cb_called_jmdc(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp)
{
	DEC_TRANSFER(disk, comp);
}

void nvmeibc_pd_cb_called_free_jrnl_ents(struct nvmeibc_disk *disk,
					 struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	DEC_TRANSFER(disk, comp);
}

#define __verify_disk_version_match(disk, comp) (true)

#define __pausable_layer_call(bin_trace_name, comp, pd_reason, fn_call) ({\
	int __rv; \
	unsigned long __flags; \
	int __pd_reason = pd_reason; \
	IRQS_DISABLED_STORE; \
	/*NFIN; */\
	if (!__get_pause_state(disk, &__flags, __pd_reason)) { \
		if (__verify_disk_version_match(disk, comp)) { \
			INC_TRANSFER(disk, comp, __pd_reason); \
			__rv = fn_call; \
			if (__rv < 0) { \
				DEC_TRANSFER(disk, comp); \
			} \
		} else { __rv = -0xDEAC; } \
		__put_pause_state(disk, __flags, __pd_reason); \
	} else { \
		__rv = -EDEAD; \
	} \
	IRQS_DISABLED_VERIFY; \
	/*NFOUT; */\
	__rv; \
})

int nvmeibc_pd_cmpxchg(struct nvmeibc_disk *disk, void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp)
{
	return __pausable_layer_call(nvmeibc_pd_cmpxchg, comp, NVMEIBC_PD_REASON_CMPXCHG,
				nvmeibc_disk_locks_interlocked_cmp_exchange(handle, addr, comp));
}

int nvmeibc_pd_read_lock(struct nvmeibc_disk *disk, void *handle, u64 addr,
	struct nvmeibc_d_rdma_comp *comp)
{
	return __pausable_layer_call(nvmeibc_pd_read_lock, comp, NVMEIBC_PD_REASON_READ_LOCK,
				nvmeibc_disk_locks_read_lock(handle, addr, comp));
}


int nvmeibc_pd_get_blkset_problems(struct nvmeibc_disk *disk, void *handle,
	u64 start, u64 length, struct nvmeibc_d_rdma_comp *comp)
{
	return __pausable_layer_call(nvmeibc_pd_get_blkset, comp, NVMEIBC_PD_REASON_GET_BLKST_PROBLEMS,
				nvmeibc_disk_get_db_ec_rl(handle, start, length, comp));
}

int nvmeibc_pd_write_blkset_info(struct nvmeibc_disk *disk, void *handle,
	u64 addr, struct nvmeibc_d_rdma_comp *comp)
{
	return __pausable_layer_call(nvmeibc_pd_wr_blkset, comp, NVMEIBC_PD_REASON_BINFO_WRITE,
				nvmeibc_disk_locks_write_blkset_info(handle, addr, comp));
}

int nvmeibc_pd_execute_io_blocks(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *cmd)
{
	return __pausable_layer_call(nvmeibc_pd_exec_io, (&cmd->disk_cmd), (NVMEIBC_PD_REASON_EXEC_IO_BASE + (int)cmd->reqs1.op),
				nvmeibc_disk_execute_io(disk, cmd));
}

static int __pd_execute_io_jour(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *cmd)
{
	struct nvmeibc_jam_rdma_op *jo = &cmd->reqs1.jam_op;
	struct jentry_md jmdc_ent;
	u8 ent_gen_id;
	bool lba_enc = false;
	int idx, rv;

	jmdc_ent.md_arr = cmd->reqs1.md;	// In future: Do a loop here
	if (!nvmeibc_jam_jmd_set(disk, cmd->reqs1.disk_address,
		&jmdc_ent, &idx, &lba_enc, &ent_gen_id)) {
		jo->n_ops = 1;
		jo->jour_idx[0] = idx;
		jo->jmdc_ent[0].md_arr = jmdc_ent.md_arr;
		jo->ent_md[0].ent_gen_id = ent_gen_id;
		jo->rng_gen_id = disk->jour.rng_gen_id;
		jo->rng_idx = disk->jour.rng_id;
		get_rcookie_ptr(cmd)->lba_jam_enc = lba_enc ? 1 : 0;
		rv = nvmeibc_disk_execute_io(disk, cmd);
	} else
		rv = -EINVAL;

	return rv;
}

int nvmeibc_pd_execute_io_jour_blocks(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_io_command *cmd)
{
	return __pausable_layer_call(nvmeibc_pd_exec_jr, (&cmd->disk_cmd), (NVMEIBC_PD_REASON_EXEC_JOUR_BASE + (int)cmd->reqs1.op),
				__pd_execute_io_jour(disk, cmd));
}

int nvmeibc_pd_execute_gen(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_gen_cmd *cmd)
{
	return __pausable_layer_call(nvmeibc_pd_exec_gen, (&cmd->disk_cmd), (NVMEIBC_PD_REASON_EXECUTE_GEN_BASE + (int)cmd->opcode),
				nvmeibc_disk_execute_gen(disk, cmd));
}

int nvmeibc_pd_tostring(const struct nvmeibc_disk*disk, char *buf, int buf_len)
{
	#define BUF_ADD(...) pos += scnprintf(buf + pos, buf_len - pos, __VA_ARGS__)
	int pos = 0, sum_all_preventers = 0, sum_all_transfers = 0;
	unsigned long flags;

	NFIN;
	local_irq_save(flags);
	sum_all_preventers = __get_sum_all_cpu_preventers(disk);
	sum_all_transfers  = __get_sum_all_in_transfer(disk);
	local_irq_restore(flags);

	BUF_ADD("pause_preventers=%d, in_transfers=%d\n", sum_all_preventers, sum_all_transfers);
	NFOUT;
	return pos;
}

int nvmeibc_pd_jmdc_read(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_jmdc_read_comp *comp)
{
	return __pausable_layer_call(nvmeibc_pd_jmdc_read, comp, NVMEIBC_PD_REASON_READ_JCMD,
				nvmeibc_disk_jmdc_read(disk, comp));
}

int nvmeibc_pd_dbg_please_kill_yourself(struct nvmeibc_disk *disk,
                                          void (*cb)(void*), void *ctx, int rsc_id, u64 dlba)
{
	return __pausable_layer_call(nvmeibc_pd_dbg_please_kill_yourself, UNTRACKED_TRANSFER, NVMEIBC_PD_REASON_DBG_CMD,
				nvmeibc_disk_dbg_please_kill_yourself(disk, cb, ctx, rsc_id, dlba));
}

int nvmeibc_pd_free_jrnl_ents(struct nvmeibc_disk *disk,
	struct nvmeibc_disk_free_jrnl_ents_comp *comp)
{
	return __pausable_layer_call(nvmeibc_pd_free_jrml, comp, NVMEIBC_PD_REASON_FREE_JRNL_ENTS,
				nvmeibc_disk_free_jrnl_ents(disk, comp));
}

int nvmeibc_pd_jam_get(struct nvmeibc_disk *disk)
{
	return __pausable_layer_call(pd_jam_get, UNTRACKED_TRANSFER, NVMEIBC_PD_REASON_JAM_GET, 0);
}

void nvmeibc_pd_jam_put(struct nvmeibc_disk *disk)
{
	DEC_TRANSFER(disk, UNTRACKED_TRANSFER);
}

void nvmeibc_pd_jam_put_all(int n_disks, struct nvmeibc_disk *disks[])
{
	int i;
	NFIN;
	for (i = 0 ; i < n_disks; i++)
		DEC_TRANSFER(disks[i], UNTRACKED_TRANSFER);
	NFOUT;
	return;
}

int nvmeibc_pd_jam_get_all(int n_disks, struct nvmeibc_disk *disks[])
{
	int i, rv_all = 0;
	NFIN;
	for (i = 0; i < n_disks; i++) {  //EC-TODO: Optimize by doing local_irq_save()/restore() once
		struct nvmeibc_disk *disk = disks[i];
		rv_all = __pausable_layer_call(pd_jam_get_loop, UNTRACKED_TRANSFER, NVMEIBC_PD_REASON_JAM_GETALL, 0);
		if (unlikely(rv_all)) {
			nvmeibc_pd_jam_put_all(i, disks);	// Put 0..(i-1)
			break;
		}
	}
	NFOUT;
	return rv_all;
}

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	#define nvmeibc_pd_toma_get(...) 	(0)				// Simulated c_disk does not need pausable layer and it must alwasys pass msgs to toma coz even unsent msgs are verified
	#define nvmeibc_pd_toma_put(...)
#else
static int nvmeibc_pd_toma_get(struct nvmeibc_disk *disk)
{
	return __pausable_layer_call(pd_toma_get, UNTRACKED_TRANSFER, NVMEIBC_PD_REASON_TOMA_GET, 0);
}

static void nvmeibc_pd_toma_put(struct nvmeibc_disk *disk)
{
	DEC_TRANSFER(disk, UNTRACKED_TRANSFER);
}
#endif

int nvmeibc_pd_toma_send(struct nvmeibc_disk *disk, u64 handle,
						 struct nvmeibc_disk_toma_send_params *params)
{
	int rv;
	NFIN;
	if (!(rv = nvmeibc_pd_toma_get(disk))) {
		rv = nvmeibc_disk_toma_send(disk, handle, params);
		nvmeibc_pd_toma_put(disk);
	}
	NFOUT;
	return rv;
}

int nvmeibc_pd_toma_unsubscribe(struct nvmeibc_disk *disk, u64 handle)
{
	int rv;
	NFIN;
	if (!(rv = nvmeibc_pd_toma_get(disk))) {
		rv = nvmeibc_disk_unsubscribe_toma_service(disk, handle);
		nvmeibc_pd_toma_put(disk);
	}
	NFOUT;
	return rv;
}

int nvmeibc_pd_reused_bb_release(struct nvmeibc_disk *disk,
								 struct nvmeib_data_reuse_buf_params *p)
{
	int rv;
	NFIN;

	/* try inc in-transfers w/o executing any functionality */
	rv =  __pausable_layer_call(nvmeibc_pd_reused_bb_release,
								UNTRACKED_TRANSFER,
								NVMEIBC_PD_REASON_REUSED_BB_RELEASE, 0);
	if (!rv) {
		nvmeibc_disk_reused_bb_release(disk, p);
		DEC_TRANSFER(disk, UNTRACKED_TRANSFER);
	}

	NFOUT;
	return rv;
}