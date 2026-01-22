/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef B_UNITEST_H
#define B_UNITEST_H
/* Architecture which stores the entire nvmesh, utilities for testing and can
 * execute scenarios.
 */
#include "nvmeib_common_all.h"
#include "module/instance/nvmeibc_cinst_params.h"			// To test states of client instance, access lists of volumes, etc

/* Extern params of production code, mostly visibile through: /sys/module/nvmeibc/parameters/ */
extern int  nvmeibc_debug_level;							// Module params: Debug level of clients module
extern int  tracer_nvmeibc_debug_level;						// Module params: Tracer debug level of clients module
extern int  goodpath_nvmeibc_debug_level;						// Module params: Goodpath tracer debug level of clients module
extern int  goodpath_nvmeibc_locks_debug_level;				// Module params: Goodpath locks tracer debug level of clients module
extern bool nvmeibc_tracer_test_mode;						// Indication that simulator is in tracer test mode
extern bool nvmeibc_copy_bio_buffers;
extern u32  max_ios_per_cpu;						// Module params: Max in air IO's per cpu
extern uint nvmeibc_jmd_wr_version;
extern bool force_reconf_reboot;						// disable hot transition.
extern bool warn_on_too_many_degraded;					// Do WARN_ON() if we have 3+ degraded segments, always true in real systems, but can be turned off in unitest, Because unitest environment causes 3 degraded segs
extern bool json_iostats_fixed_size;					// By default IO stats jsons are not padded to fixed size
extern bool nvmeibc_debug_ram_binfo;					// Module param
extern bool qa_ec_stress_debug;							// EC module param
extern uint nvmeibc_jentry_num_blocks;					// Clients module param of binje
extern unsigned self_recovery_detach_initial_time_sec;
extern ulong nvmeibc_jam_pending_req_timeout_jif;
extern bool cli_attach_check_if_already_attached;
extern bool nvmeibc_warn_on_edic_verification_failure;
extern bool nvmeibc_warn_on_mgmt_wrong_msg_logic;
extern bool nvmeibc_warn_on_parities_sync_missmatch;
extern bool nvmeibc_notify_toma_on_slice_by_slice_destruction_in_sync;
extern bool nvmeibc_raid1_destroy_force_physical_bad_sector_in_sync;
extern unsigned int num_warnings;

static inline void set_warn_on_too_many_degraded(  void) { warn_on_too_many_degraded = true; }
static inline void unset_warn_on_too_many_degraded(void) { warn_on_too_many_degraded = false; }
/****************** Framework for async testing with treads *******************/
typedef struct { 						// Parameters for IO async thread
	struct NVMeshSystem *sys;
	int disk_id;						// Which disk to pause
	bool is_ec;							// Is this test on an EC volume
	int nlbas;							// IO size in blocks
	void *write_buf;					// Shared buffer for writes, only for _thread_async_continuous_write()
	int mu_delay;						// Delay in microseconds between consecutive IO requests
	int n_cycles;						// Amount of work interations the thread did
	struct task_struct	*kthread;
	enum nvmeib_block_io_op		op;		// READ/WRITE/TRIM
	u32		flags;						// various knobs
#define BUNITEST_ASYNC_TEST_FLAG_ALLOW_IO_ERROR		0x0001		// the test expects some IO error
#define BUNITEST_ASYNC_TEST_RAND_IO_PATTERN			0x0002		// IO is not sequential but random
#define BUNITEST_ASYNC_TEST_SLOWED_DOWN_SYNCS		0x0004		// Slow down syncs to increase race conditions
} t_async_test_params;

#define t_async_test_params_init(p, _sys, _disk_id, _nlbas, _flags) do { \
	p.sys 				= _sys; \
	p.disk_id 			= _disk_id; \
	p.nlbas				= _nlbas; \
	p.mu_delay			= ut_conf__get_transport()->is_disk_callback_sync ? 50 : 250; /* 20K IO's per second, in async mode, we submit faster hence much more IO's. so increase delay to reduce load */ \
	p.n_cycles			= 0; \
	p.kthread			= NULL; \
	p.op				= NVMEIB_BLOCK_IO_OP_ILLEGAL_DBG; /* poison */\
	p.flags 			= _flags; \
	p.is_ec				= false; \
} while (0)

#define t_async_ec_test_params_init(p, _sys, _disk_id, _nlbas, _flags) do { \
		   t_async_test_params_init(p, _sys, _disk_id, _nlbas, _flags); \
		p.is_ec				= true; \
} while (0)

/* List of predefined threads one can use for his unitests*/
int __thread_async_sleep(                 void* param);					// Does nothing, a place holder
int __thread_async_pause_cont(            void* param);					// Do pause/cont on disks
int __thread_async_attach_dettach(        void* param);
int __thread_async_suspend_revive(        void* param);
int __thread_async_toma_messages(         void* param);
int __thread_async_trim_vol0_interlocking(void* param);
int __thread_async_resize_vol0(           void* param);
int __thread_async_continous_write(       void* param);
int __thread_gen_async_io(                void* param);
int __thread_async_degraded_mirror_vols(  void* param);
int __thread_async_degraded_ec_vols(	  void* param);

/********** Framework for unique bio generation and verification **************/
u64 __unitest_get_pattern(void);			/* Generate a unique 64b pattern */
u64 __unitest_get_trimmed_u64(void);		/* Generate patter that trim would yield */
u64 __unitest_fill_blocks_rand_pattern(void *buf, int n_blks);   /* fill the specified blocks with rand pattern and return it */
u64 __unitest_fill_blocks_unique_pattern(void *buf, int n_blks); /* fill the specified blocks with unique pattern and return it */
u64 __unitest_fill_blocks_unique_pattern_and_lba(void *buf, u64 start_lba, int n_blks); /* fill the specified blocks with unique pattern interleaved with block vlba and return the unique pattern */
void __unitest_verify_blocks_pattern_data(const void *buf, const int nblocks, u64 pat, bool verify_out_of_bound, bool verify_match); // Verify memory is fully set to 'val' value. Optionally verify that out of bounds is not set to 'val'
void __unitest_verify_blocks_pattern(const void *buf, const int nblocks, u64 pat, bool verify_out_of_bound);

/************************** Utilities for testing ****************************/
#define TEST_FUNC	 __attribute__ ((visibility ("default")))		// It will appear in stack dump when crash occurs
#define HIDDEN_FUNC	 __attribute__ ((visibility ("hidden")))		// Do not show in stack trace

enum bunitest_phase {										// Different phases of the unitest, in each phase system stability differs so different stability checks are employed
	BUNI_ILLEGAL = 0,
	BUNI_INITIALIZING = 1,									// Creating NVMesh environment, tests have not been started yet
	BUNI_VOLUMES_TESTING,									// Testing Thick volumes of NVMEsh V1: (JBOD, Raid1,0,10)
	BUNI_ERASURE_CODING_TESTING,							// Testing Thick volumes of NVMEsh V1.1: Triple mirror,...
	BUNI_CLIENT_REMOVAL_TESTING,							// Client restart/removal tests
	BUNI_ALL_DONE,											// All tests are done
};

#include "nvmesh_sim.h"

typedef struct {
	const struct blk_unittest_conf *conf;					// Configuration params of the test suit
	struct NVMeshSystem _sys;
	struct NVMeshSystem* sys;
	enum bunitest_phase test_phase;							// Current test-phase
	unsigned long timer;									// General purpose timer to measure unitests runs
} bunitest_s;

int  bunitest_s_create(bunitest_s* B);
void bunitest_phase_stack_do(bunitest_s* B, const char* cmd, enum bunitest_phase new_phase); // cmd - "push"/"pop" cur state and set the new state
void bunitest_tic(bunitest_s* B);							// Start timer
int  bunitest_toc(bunitest_s* B);							// Returned passed time from previous tic() in [mSecs].

static inline enum bunitest_phase get_sys_test_phase(struct NVMeshSystem *sys)
{
	return container_of(sys, bunitest_s, _sys)->test_phase;
}

/*****************************************************************************/
// Todo: Organize below
// Yuri: RRR replace macro __get_sdd_by_seg with strongly typed inline function, returning pointer to ramdisk
// This is needed in order to avoid further return type confusion
// To make this possible we need first to resolve the includes hell around here
#define __get_server_by_seg(sys, seg)  (&((sys)->servers[(seg)->node_id]))
#define __get_sdd_by_seg(   sys, seg)  (&((sys)->servers[(seg)->node_id].ramDisk))
#define _addr4k(l,o)  (LOCKSET_SLICES*(l) + ((o)<<MGMT2CLNT_SHIFT))	// Given lockset 'l' and offset 'o' in units of 4K blocks - calculate the exact address

#define physSegStartPtr_off(seg, off) 											\
({																				\
	__auto_type ssd = __get_sdd_by_seg(sys,seg);								\
	(&ssd->mem[COMMITTED_ADDR_AS(ssd, (seg)->dlba_start + off, 4KB, BYTE)]);	\
})

#define physSegDBIdxPtr_off(seg, off) 												\
({																					\
	__auto_type ssd = __get_sdd_by_seg(sys,seg);									\
	(&ssd->dbits[COMMITTED_ADDR_AS(ssd, (seg)->dlba_start + off, 4KB, LOCK)]);	\
})

#define physSegLKIdxPtr_off(seg, off) 												\
({ 																					\
	__auto_type ssd = __get_sdd_by_seg(sys,seg);									\
	(&ssd->locks[COMMITTED_ADDR_AS(ssd, (seg)->dlba_start + off, 4KB, LOCK)]);	\
})

#define physSegMDIdxPtr_off(seg, off) ramDiskSimulator_get_metadataptr(__get_sdd_by_seg(sys,seg), ((seg)->dlba_start + off))
#define physSegStartPtr(    seg     ) physSegStartPtr_off(seg,0ull)


int __cancel_and_drain_resubmitted_io(struct NVMeshSystem *sys);
void __volume_memset(struct NVMeshSystem *sys, int v, u8 val);	// fill volume segments with a unique pattern
void __my_rand_perm(u32 arr[], const int length);
void __my_rand_perm_multi_slice(u32 arg[], const int length, const int msn);
#define unitest_rv_to_string(rv) (rv ? KERN_COL_RED_BOLD "Fail" KERN_COL_RESET: "Pass")

/* Put Vol0 in different lock modes. */
void __set_different_lock_modes_of_vol(struct NVMeshSystem *sys, int volInd, const char *action);
TEST_FUNC int __unitest_downgrade_upgrade_raid(struct NVMeshSystem *sys, int volInd, bool only_hot);
void NVMeshSystem__detectStuckIOs(struct NVMeshSystem *sys);
#define REPORT_ERROR(rv) ({ BUG_ON(rv); })

/* Injection of no retry IO failures */
#define NUMBER_OF_ERROR_CODES 		3

#endif

