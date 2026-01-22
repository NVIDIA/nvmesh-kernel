/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_LOCKS_CHANNEL_H
#define NVMEIBC_LOCKS_CHANNEL_H

#include "nvmeibc_main.h"
#include "nvmeibc_channel.h"
#include "nvmeibc_admin_channel.h"
#include "nvmeibc_ib_net.h"
#include "nvmeibc_disk.h"
#include "nvmeib_wd.h"

#ifndef LOW_MEM
enum { NVMEIBC_CHANNEL_MAX_MAIN_LOCKS_MSGS = 16 /*32*/};
enum { NVMEIBC_CHANNEL_AV_NUM_OF_WR_PER_MSG = 8 /*10*/ };
#else
enum { NVMEIBC_CHANNEL_MAX_MAIN_LOCKS_MSGS = 1};
enum { NVMEIBC_CHANNEL_AV_NUM_OF_WR_PER_MSG = 1 };
#endif
enum { NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR = NVMEIBC_CHANNEL_MAX_MAIN_LOCKS_MSGS *
	NVMEIBC_CHANNEL_AV_NUM_OF_WR_PER_MSG,
	NVMEIBC_LOCK_2ND_CH_NUM_OF_OPR = NVMEIBC_CHANNEL_NUM_OF_ALLOC_APR,
};

#define DEBUG_2ND_LOCK_CH_KA 			0
#define DEBUG_2ND_LOCK_CH_TEST_ATMOIC 	0

#define DEBUG_LOCK_CH_SPINLOCK 1

struct nvmeibc_d_rdma_comp;

/* Following two enums are used under NVMEIBC_LOCKS_CHANNEL_GUARD_STATE */
enum lock_opr_state {
	LOCK_OPR_STATE_INVALID = 0,
	LOCK_OPR_IN_FREE_LIST = 1000,
	LOCK_OPR_PREPARING,
	LOCK_OPR_IN_PROGRESS,
	LOCK_OPR_ABORTED,
	LOCK_OPR_ERROR,
};

enum lock_opr_rdma_bypass_state {
	LOCK_OPR_BYPASS_INVALID = 0,
	LOCK_OPR_NO_BYPASS = 1000,
	LOCK_OPR_USING_BYPASS,
	LOCK_OPR_BYPASS_IN_PENDING,
	LOCK_OPR_BYPASS_POSTED,
	LOCK_OPR_BYPASS_IN_LOCAL_WQ,
	LOCK_OPR_BYPASS_COMPLETE,
	LOCK_OPR_BYPASS_ERROR,
};

struct nvmeibc_lock_opr_in_progress
{
	struct nvmeibc_disk_lock_cmd disk_lock_cmd;
	bool disk_piggyb;
	struct nvmeibc_locks_channel *ch;
	int index;
	/*pointer to a mapped value*/
	u64 val[NVMEIB_LOCK_DATA_BUFFERS];
	dma_addr_t val_phys;
	/*linked to list that maintain the value*/
	struct list_head link;
	/*comperator under process*/
	struct nvmeibc_d_rdma_comp *comp;
	/*wr used*/
	struct nvmeib_send_wr wr;
	struct ib_sge list;
	u32 sq_psn;
	/* watchdog */
	struct wd_info_common wdc;
	bool had_to_defer;
	unsigned long start_time;
	bool mark;
	/*indicates that this opr is the first in the post*/
	bool first;
	unsigned id_in_row;
	unsigned long jiffies_start;
	unsigned long opr_timeout;
	/* was aborted */
	bool aborted;
	/* version for checking for double-completion */
	u16 version;

#if	defined(NVMEIBC_LOCKS_CHANNEL_GUARD_STATE) && (NVMEIBC_LOCKS_CHANNEL_GUARD_STATE == 1)
	/* state tracking */
	struct nvmeib_state_guard state;
	struct nvmeib_state_guard bypass_state;
#endif
};


#if	defined(NVMEIBC_LOCKS_CHANNEL_GUARD_STATE) && (NVMEIBC_LOCKS_CHANNEL_GUARD_STATE == 1)

#define NVMEIBC_LOCK_GUARD_INIT(guard, state)\
do {\
	nvmeib_init_state_guard(guard, state);\
} while (0)

// assume lock channel spinlock
#define NVMEIBC_LOCK_GUARD_GET_CHECK(guard, state, _x_)\
do {\
	int s = nvmeib_get_state_guard(guard);\
	if (s != (int)state) {\
		_NE(_x_, "bad lock guard state=@INT expected=@INT", s, state);\
		nvmeib_state_guard_print_stack_trace(guard);\
		BUG();\
	}\
} while (0)

// assume lock channel spinlock
#define NVMEIBC_LOCK_GUARD_SWITCH_CHECK(guard, old, new, _x_)\
do {\
	if (!nvmeib_switch_state_guard(guard, old, new)) {\
		_NE(_x_, "failed to switch lock guard old=@INT new=@INT current=@INT",\
			old, new, nvmeib_get_state_guard(guard));\
		nvmeib_state_guard_print_stack_trace(guard);\
		BUG();\
	}\
} while (0)

#else /* NVMEIBC_LOCKS_CHANNEL_GUARD_STATE is OFF */
#define NVMEIBC_LOCK_GUARD_INIT(guard, state)
#define NVMEIBC_LOCK_GUARD_GET_CHECK(guard, state, _x_)
#define NVMEIBC_LOCK_GUARD_SWITCH_CHECK(guard, old, new, _x_)
#endif


static inline struct nvmeibc_lock_opr_in_progress * disk_to_opr(
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd)
{
	return container_of(disk_lock_cmd, struct nvmeibc_lock_opr_in_progress, disk_lock_cmd);
}

enum nvmeibc_lock_channel_choosing_method {
	LOCK_CHANNEL_CHOOSING_METHOD_LRU,
	LOCK_CHANNEL_CHOOSING_METHOD_BY_CPU,
	LOCK_CHANNEL_CHOOSING_METHOD_SHARDING,
};

struct nvmeibc_locks_channel_coremask_ops {
	void (*get_ref_fn)(void *coremask_cookie);
	void (*put_ref_fn)(void *coremask_cookie);
	u64 (*get_uid_fn)(void *coremask_cookie);
	const struct nvmeib_cpu_mask* (*get_mask_fn)(void *coremask_cookie);
};

struct nvmeibc_locks_channel {
	spinlock_t locks_spinlock;
	int locking_cpu;
#ifdef DEBUG_LOCK_CH_SPINLOCK
	const char *locking_file;
	int locking_line;
	const char *locking_fn;
	int locking_pid;
	unsigned long taken_time;
	unsigned long	called_time;
#endif
	/* our base channel */
	struct nvmeibc_channel base;
	/* counts the locks that being started and not returned */
	bool paused;
	/*will hold the underlying connection*/
	struct nvmeibc_ib_net net;
	/*option to store the channel in a list*/
	struct list_head link;
	/*defered jobs*/
	struct list_head defered;
	/*underway jobs*/
	struct list_head in_progress;
	/*An array of values to be used by operations in progress*/
	struct nvmeibc_lock_opr_in_progress *locks_ip_buffer;
	/*free pool of nvmeibc_lock_opr_in_progress*/
	struct list_head free_ip_pool;
	/* aborted ops */
	struct list_head aborted;
	int num_of_free;
	int num_in_progress;
	int num_of_atom_read_ip;
	int num_of_series_ip;
	int num_defered;
	int num_disk_piggyb;
	int max_atom_read_ip;
	int total_num_opr;
	int num_aborted;

	/* lock-channel stats */
	u64 n_comp_llp_opr;
	u64 n_comp_llp_test;
	u64 n_comp_llp_ka;
	/* lock-channel extended KA stats */
	u64 ka_post_cnt;
	u64 ka_post_jif;
	u64 ka_comp_jif;
	u64 ka_skip_use;

	/*base dma address of the channel*/
	u64 base_dma_addr;
	/*enumerates send requests*/
	unsigned send_enumerator;
	/* client-side atomic capabilities */
	enum ib_atomic_cap atomic_cap;
	enum ib_atomic_cap masked_atomic_cap;
	/* target-side atomic capabilities */
	enum ib_atomic_cap tgt_atomic_cap;
	enum ib_atomic_cap tgt_masked_atomic_cap;
	/* Used for testing atomics */
	u32 atomic_test_zone_rkey;
	u64 atomic_test_zone_raddr;
	u64 *atomic_test_src;
	u32 atomic_test_zone_lkey;
	u64 atomic_test_zone_laddr;
	struct completion atomic_test_comp;
	int atomic_test_wc_status;
	/* Atomic Endianness Requirements */
	bool atomic_req_endian_swap;
	bool atomic_reply_endian_swap;
	bool masked_atomic_req_endian_swap;
	bool masked_atomic_reply_endian_swap;
	/* Secondary Lock Channels */
	struct nvmeibc_locks_channel *_2nd_ch[NVMEIB_N_2ND_LOCK_CHS];
	int n_2nd_ch;
	/* Secondary Channel Only */
	struct nvmeibc_ib_net_params *_2nd_net_params;
	/* Back pointer to primary channel */
	struct nvmeibc_locks_channel *primary_ch;
	/* Period Handler to testing locks channel is alive */
	struct admin_periodic periodic_ka;
#if NVMEIBC_LOCK_CH_CB_KERNEL_WQ
	struct workqueue_struct *callback_wq;
#else
	/* Work-queue for callbacks */
	struct workq_struct *callback_wq;
#endif
	/* Local-bypass bitmap */
	DECLARE_BITMAP(local_bypass_bmp, NUM_NVMEIBC_LOCK_OPR);

	enum nvmeibc_disk_release_reason release_reason;

	enum nvmeibc_lock_channel_choosing_method method;
	unsigned int _2nd_ch_pcpu;
	bool _2nd_ch_pcpu_lockless;
	unsigned int _2nd_ch_coremask;
	struct nvmeibc_locks_channel *_2nd_ch_pcpu_map[NVMEIB_DFLT_MAX_CPUS];
	struct nvmeibc_locks_channel *_2nd_ch_coremask_map[NVMEIB_DFLT_MAX_CPUS];
	const char *_2nd_ch_pcpu_mask_str;
	cpumask_var_t _2nd_ch_pcpu_mask;
	/* For secondary coremask channels only. Used to indicate which incoming CPUs they will be used for (ie primary_ch->_2nd_ch_coremask_map[cpu] = secondary_ch) */
	struct nvmeib_cpu_mask _2nd_ch_coremask_mask;
	/* Ops for coremask channels */
	const struct nvmeibc_locks_channel_coremask_ops *coremask_ops;
	struct workqueue_struct *_2nd_ch_pcpu_wq;

	/* Used for connecting/disconnecting pcpu secondary channels */
	struct work_struct disconnect_work;
	struct completion *disconnect_comp;
	atomic_t *disconnect_cnt;
	int conn_rv;

	struct nvmeibc_login_request lreq;
};

#ifndef DEBUG_LOCK_CH_SPINLOCK

#define LOCKS_CHANNEL_SET_LOCKING_DATA(ch) do {\
	ch->locking_cpu = smp_processor_id();\
} while(0)

#define LOCKS_CHANNEL_RESET_LOCKING_DATA(ch) do {\
	ch->locking_cpu = -1;\
} while(0)

#else

#define SLOW_SPIN_LOCK_SECONDS (HZ * 10)

#define LOCKS_CHANNEL_SET_LOCKING_DATA(ch) do {\
	ch->locking_cpu = smp_processor_id();\
	ch->locking_file = __FILE__;\
	ch->locking_line = __LINE__;\
	ch->locking_fn = __FUNCTION__;\
	ch->locking_pid = current->pid;\
	ch->taken_time = jiffies;\
} while(0)

#define LOCKS_CHANNEL_RESET_LOCKING_DATA(ch) do {\
	ch->taken_time = 0;\
	ch->locking_cpu = -1;\
	ch->locking_file = NULL;\
	ch->locking_line = 0;\
	ch->locking_fn = NULL;\
	ch->locking_pid = 0;\
} while(0)

#endif

#define nvmeibc_locks_channel_spin_lock_init(ch) do {\
	spin_lock_init(&ch->locks_spinlock);\
	LOCKS_CHANNEL_RESET_LOCKING_DATA(ch);\
} while(0)

#define nvmeibc_locks_channel_spin_lock_irqsave(ch, flags) do {\
	if (nvmeibc_channel_is_ll_pcpu_ch(&ch->base)) {\
		BUG_ON(get_cpu() != nvmeibc_channel_pcpu_ch_get_cpu(&ch->base));\
		local_irq_save(flags);\
	} else {\
		unsigned long _start = jiffies; \
		spin_lock_irqsave(&ch->locks_spinlock, flags);\
		ch->called_time = _start; \
	}\
	LOCKS_CHANNEL_SET_LOCKING_DATA(ch);\
} while(0)

/* To ensure nvmeibc_locks_channel_spin_is_locked works,
 *	we need irqs to be disabled when locking the spinlock, so we BUG_ON if they are not */
#define nvmeibc_locks_channel_spin_lock(ch) do {\
	BUG_ON(!irqs_disabled());\
	if (nvmeibc_channel_is_ll_pcpu_ch(&ch->base)) {\
		BUG_ON(get_cpu() != nvmeibc_channel_pcpu_ch_get_cpu(&ch->base));\
	} else {\
		spin_lock(&ch->locks_spinlock);\
	}\
	LOCKS_CHANNEL_SET_LOCKING_DATA(ch);\
} while(0)

#define nvmeibc_locks_channel_spin_unlock_irqrestore(ch, flags) do {\
	if (nvmeibc_channel_is_ll_pcpu_ch(&ch->base)) {\
		BUG_ON(smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(&ch->base));\
		local_irq_restore(flags);\
		put_cpu();\
	} else { \
		int _total_hold = (jiffies - ch->taken_time) / HZ; \
		int _total = (jiffies - ch->called_time) / HZ; \
		if (_total_hold > SLOW_SPIN_LOCK_SECONDS || _total  > SLOW_SPIN_LOCK_SECONDS) { \
			_NW_dmesg(__AUTOID__, "Hold spinlock by @FILENAME:@LINENO took @INT seconds, waited @INT seconds", ch->locking_file , ch->locking_line, _total, _total_hold); \
			WARN_ON_ONCE(1); \
		} \
		LOCKS_CHANNEL_RESET_LOCKING_DATA(ch);\
		spin_unlock_irqrestore(&ch->locks_spinlock, flags);\
	}\
} while(0)

#define nvmeibc_locks_channel_spin_unlock(ch) do {\
	LOCKS_CHANNEL_RESET_LOCKING_DATA(ch);\
	if (nvmeibc_channel_is_ll_pcpu_ch(&ch->base)) {\
		BUG_ON(smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(&ch->base));\
		put_cpu();\
	} else {\
		spin_unlock(&ch->locks_spinlock);\
	}\
} while(0)

static inline bool nvmeibc_locks_channel_spin_is_locked(struct nvmeibc_locks_channel *ch)
{
	if (nvmeibc_channel_is_ll_pcpu_ch(&ch->base)) {
		if (preemptible())
			return false;
		BUG_ON(smp_processor_id() != nvmeibc_channel_pcpu_ch_get_cpu(&ch->base));
		return ch->locking_pid == current->pid;
	}
	/* work assumption: ch is always locked with irqsave */
	return irqs_disabled() && ch->locking_cpu == smp_processor_id();
}

void nvmeibc_locks_channel_free(struct nvmeibc_locks_channel *ch);

struct nvmeibc_disk_segments_locks;

/**
 * iterate over exiting channel and try to use existing connection. If no
 * connection is found and new connection is establised.
 * @author yaron (7/14/2015)
 *
 * @param admin_ch
 * @param gid
 * @param cid
 *
 * @return struct nvmeibc_locks_channel*
 */
struct nvmeibc_locks_channel *nvmeibc_locks_channel_connect_locks(
	struct nvmeibc_admin_channel *admin_ch, union ib_gid *gid, int max_tgt_atomic_ops,
	enum rdma_link_layer dest_link_layer, enum rdma_transport_type dest_transport_type, int rgid_idx,
	unsigned tcp_base_port, unsigned tcp_num_ports);

/* Connect lock channels by lport and rgid */
struct nvmeibc_locks_channel *nvmeibc_locks_channel_connect_lock_by_path(
	struct nvmeibc_admin_channel *admin_ch, struct nvmeibc_local_nic_port *lport, union ib_gid *gid, int max_tgt_atomic_ops,
	enum rdma_link_layer dest_link_layer, enum rdma_transport_type dest_transport_type, int rgid_idx,
	unsigned tcp_base_port, unsigned tcp_num_ports);

bool nvmeibc_locks_channel_all_2nd_connected(struct nvmeibc_locks_channel *ch);

/**
 * disconnects locks channel. deallocates channel memory
 *
 * @param ch channel created with nvmeibc_locks_channel_create
 */
void nvmeibc_locks_channel_disconnect(struct nvmeibc_locks_channel *ch);


/**
 * return free "operator in progress" or null if none available
 *
 * @author yaron (6/15/2015)
 *
 * @param ch
 *
 * @return struct nvmeibc_lock_opr_in_progress*
 */
struct nvmeibc_lock_opr_in_progress *nvmeibc_locks_channel_get_free_opr_ip(
	struct nvmeibc_locks_channel *ch);

/**
 * return item to the free pool list
 *
 * @author yaron (6/15/2015)
 *
 * @param ch
 * @param item
 */
void nvmeibc_locks_channel_free_opr_ip(struct nvmeibc_locks_channel *ch,
	struct nvmeibc_lock_opr_in_progress *item, enum lock_opr_state curr_state);

void nvmeibc_locks_channel_start_disconnect(struct nvmeibc_locks_channel *ch);

/* update all lock handles to be detached */
void nvmeibc_locks_channel_disconnect_all_handles(struct nvmeibc_disk *disk);

/* update all lock handles to be detached - the non lock versionh*/
void nvmeibc_locks_channel_disconnect_all_handles_(struct nvmeibc_disk *disk);

static inline bool nvmeibc_locks_channel_already_locked(
	struct nvmeibc_locks_channel *ch)
{
    return irqs_disabled() && ch->locking_cpu == smp_processor_id();
}

static inline bool nvmeibc_locks_channel_lock_cmd_is_timed_out(
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd, unsigned long now)
{
	struct nvmeibc_lock_opr_in_progress *opr = disk_to_opr(disk_lock_cmd);
	return (now - opr->jiffies_start) >= opr->opr_timeout;
}

void nvmeibc_locks_channel_lock_cmd_completion(
	struct nvmeibc_disk_lock_cmd *disk_lock_cmd, int err_code,
	enum lock_opr_rdma_bypass_state bypass_state);

bool nvmeibc_locks_channel_is_ka_timeout(struct nvmeibc_locks_channel *ch);

int nvmeibc_locks_channel_connect_coremask_chs(struct nvmeibc_locks_channel *ch, struct nvmeibc_admin_channel *admin_ch,
					       u64 coremask_uid, const struct nvmeib_cpu_mask *cpumask, void *coremask_cookie,
					       const struct nvmeibc_locks_channel_coremask_ops *coremask_ops, struct nvmeib_cpu_mask *lch_cpumask);

void nvmeibc_locks_channel_disconnect_coremask_chs(struct nvmeibc_locks_channel *ch, const struct nvmeib_cpu_mask *cpumask, 
						   void *coremask_cookie);

struct nvmeibc_channel *nvmeibc_locks_channel_get_coremash_ch_for_cpu(struct nvmeibc_locks_channel *ch, 
								      void *coremask_cookie, int cpu,
								      struct nvmeib_cpu_mask *ch_cpumask);

int nvmeibc_locks_channel_wq_init(void);
void nvmeibc_locks_channel_wq_destroy(void);
struct workqueue_struct *nvmeibc_locks_channel_get_wq(void);

#endif //NVMEIBC_LOCKS_CHANNEL_H

