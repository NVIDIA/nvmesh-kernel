/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "bunitest.h"
#include "block/datapath_utils_generic/dp_io_stats/nvmeibc_b_dp_iostats.h"
#include "clnt/nvmeibc_user_space_simu.h"
#include "common/nvmeib_error_report.h"
#include "compat/kr_incs_percpu.h"
#include "management_utils_common/nvmeibc_management_capi_parse_conf.h"
#include "nvmeib_io_stats.h"
#include "nvmeibc_block.h"
#include "nvmeibc_mcs_stub.h"
#include "nvmesh_sim.h"										// Entire NVMEsh system
#include "./uni_framework/bunitest_conf.h"
#include "clnt/block/nvmeibc_block_common.h"
#include "clnt/block/recovery/nvmeibc_block_dp_sync_common.h"
#include "block/recovery/nvmeibc_raid_recovery.h"
#include "uni_scenarios/uni_scenario_ec.h"
#include "uni_scenarios/uni_scenario_mtv.h"
#include "uni_scenarios/uni_scenario_ec_jam.h"
#include "uni_scenarios/uni_scenario_ec_serjio.h"
#include "uni_scenarios/uni_scenario_gf.h"
#include "uni_scenarios/uni_scenario_vol_config.h"
#include "uni_scenarios/uni_scenario_io_perm_alert.h"
#include "../../../perfTest/io_stress/cmp_blocks/cmp_blocks_impl.h"
#include "uni_framework/range_algorithms.h"
#include "uni_framework/cond_wait_algorithms.h"
#include "uni_scenarios/uni_enumerators.h"
#include "uni_scenarios/uni_recoveries.h"
#include "nvmeibc_tracer_unitest.h"
#include "uni_framework/simu_test.h"
#include "tests_conf.h"
#include <math.h>
#include "../../common/nvmeib_consts_shared.h"
#include "nvmeibc_memmgr_metrics.h"
#include "nvmeibs_memmgr_metrics.h"
#include "nvmeibc_error_tags.h"
#include "memmgr_metrics_tests.h"
#include "metrics_test.h"
#include "wq_metrics_tests.h"
#include "clnt/nvmeibc_wq_metrics.h"
#include "error_tags_tests.h"
#include "kr_incs_bit_ops_test.h"
#include "nvmeib_scatterlist_iter_test.h"
#include "nvmeibc_management_capi_parse_conf_test.h"
#include <stdint.h>

/******************************************************************************/
// Equivalent to implementation of Toma nvmeibt_binary_tracing.[c/h]
#ifdef USER_SPACE_TRACING

#include <nvmeib_trace_userspace_poller.h>
#define TRACE_BUFFER_SIZE  4096
#define TRACE_CHANNEL_BUFS 1024

//Trace channels handles, must be defined here.  All these symbols are exported via: common_public/nvmeibc_trace.h. Not all are currently in use.
struct trace_channel *nvmeibc_trace_eter;
struct trace_channel *nvmeibc_trace_long;
struct trace_channel *nvmeibc_trace_eph;
struct trace_channel *nvmeibc_trace_goodpath;
struct trace_channel *nvmeibc_trace_metrics;

// Handles of all trace pollers
pthread_t __poller_long, __poller_goodpath, __poller_metrics, __poller_eph, __poller_eter;

bool debug_dump_funcs = false;

// Simply run poller main loop in a separate thread
void* trace_poller_thread(void *_p) {
	struct nvmeib_trace_channel_descriptor *p = _p;
	extern int pthread_setname_np(pthread_t, const char *);
	char name[11];									// Arbitrary decision to use 10 first character. No more than 15 is allowed
	strlcpy(name, p->basename, sizeof(name));		// Give name to the thread as prefix of channel
	pthread_setname_np(pthread_self(), name);		// Too lazy to chekc errors
	nvmeib_trace_poll_to_logrotated_file_loop(p);
	free(p);
	return NULL;
}

static void __init_trace_channels(void) {
	nvmeibc_trace_goodpath = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 0);
	nvmeibc_trace_metrics = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 0);
	nvmeibc_trace_eph = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 1/*Is Ephemeral*/);
	nvmeibc_trace_long = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 0);
	nvmeibc_trace_eter = nvmeib_init_trace_channel(TRACE_BUFFER_SIZE, TRACE_CHANNEL_BUFS, 0, 0);
}

static void __destroy_trace_channels(void) {
	nvmeib_destroy_trace_channel(nvmeibc_trace_eph);
	nvmeib_destroy_trace_channel(nvmeibc_trace_long);
	nvmeib_destroy_trace_channel(nvmeibc_trace_eter);
	nvmeib_destroy_trace_channel(nvmeibc_trace_metrics);
	nvmeib_destroy_trace_channel(nvmeibc_trace_goodpath);
}

static void __start_trace_poller(pthread_t *poller, struct trace_channel *channel, const char *basename, const char *workdir) {
	struct nvmeib_trace_channel_descriptor *descr = malloc(sizeof(struct nvmeib_trace_channel_descriptor));  // Quick assignment instead of calling nvmeib_init_trace_channel_descriptor()
#ifndef NVMEIBC_TRACER_UNITEST_ALTERNATIVE_COMPILATION
	*descr = (struct nvmeib_trace_channel_descriptor){channel, basename, workdir, -1, -1, 0, 0};
#else
	/*This is for tracer unitest - if compiled with this special flag - preserve old logs*/
	*descr = (struct nvmeib_trace_channel_descriptor){channel, basename, workdir, -1, -1, 1, 0};
#endif /*NVMEIBC_TRACER_UNITEST_ALTERNATIVE_COMPILATION*/
	pthread_create(poller, NULL, trace_poller_thread, descr);
}

static void __join_trace_poller(pthread_t poller, struct trace_channel *channel) {
	nvmeib_flush_and_terminate(channel);
	pthread_join(poller, NULL);
}

void __join_all_trace_pollers(void) {
	__join_trace_poller(__poller_eter, nvmeibc_trace_eter);
	__join_trace_poller(__poller_long, nvmeibc_trace_long);
	__join_trace_poller(__poller_eph, nvmeibc_trace_eph);
	__join_trace_poller(__poller_metrics, nvmeibc_trace_metrics);
	__join_trace_poller(__poller_goodpath, nvmeibc_trace_goodpath);
	__destroy_trace_channels();
}

void __start_all_trace_pollers(void) {
	__init_trace_channels();
	__start_trace_poller(&__poller_eter,     nvmeibc_trace_eter,     "eternal.binlog",   ".");
	__start_trace_poller(&__poller_long,     nvmeibc_trace_long,     "longterm.binlog",  ".");
	__start_trace_poller(&__poller_eph,      nvmeibc_trace_eph,      "ephemeral.binlog", ".");
	__start_trace_poller(&__poller_goodpath, nvmeibc_trace_goodpath, "goodpath.binlog",  ".");
	__start_trace_poller(&__poller_metrics,  nvmeibc_trace_metrics,  "metrics.binlog",   ".");
	atexit(__join_all_trace_pollers);
}

#endif /*USER_SPACE_TRACING*/

/******************************************************************************/
static inline char bool_to_yes_no(bool b){ return b ? 'y' : 'n';}

struct NVMeshSystem* g_sys;									// Global variable to be able to access it during debugging and core dump from every thread
void bunitest_phase_stack_do(bunitest_s* B, const char* cmd, enum bunitest_phase new_phase){ // push/pop cur state and set the new state
	static enum bunitest_phase prev_test_phase = BUNI_ILLEGAL;
	if (cmd[1]=='u') { /* Push */
		BUG_ON(prev_test_phase != BUNI_ILLEGAL);                              // Todo: Support depth of stack of more than 1
		prev_test_phase = B->test_phase;
		B->test_phase = new_phase;
	} else { /* pop*/
		B->test_phase = prev_test_phase;
		prev_test_phase = BUNI_ILLEGAL;
	}
}

void bunitest_tic(bunitest_s* B){ B->timer = jiffies; }
int  bunitest_toc(bunitest_s* B){ return (int)(((jiffies-B->timer)*1000)/HZ); }		// Returns the passed time in msecs

/*************************** Entire NVMEsh system ****************************/
// wait until a change of topology (that was already applied) stabalizes
void __wait_for_topology_change(const struct nvmeibc_block_device *bdev)
{
	// by now, msgs between client & Toma were processed.
	nvmeibc_topo_wait_for_single_topo_no_io(&bdev->topologies, 1); // wait for the register/unregister to complete (hanged on topo->on_active)
	clientSimulator_wait_for_mainwq(&g_sys->clients[0]);		// Reconfigurations
	tomaSimulator_waitProtoEnd(NULL);
	wq_drain(system_wq);										// Daniel: Not sure needed
	clientSimulator_wait_for_all_ecpus_to_finish(&g_sys->clients[0]);	// wait for ecpu drain to process async disk operations
}

/* R1 only version: switchTopoology & wait for the whole execution to complete. upon return, NVMesh is in stable state.
   This is an overkill to use in cases where no configuration changes are intdoduced (use SW_TOPO__WAIT_ACK_DR instead)*/
void tomaSimulator_switchTopoBlocked(const struct nvmeibc_block_device *bdev, const char *r1_uuid, enum NVMEIBTC_DS_MODE s0, enum NVMEIBTC_DS_MODE s1) {
	tomaSimulator_switchTopo(r1_uuid, s0, s1, SW_TOPO__WAIT_ACK);
	__wait_for_topology_change(bdev);
}

void NVMeshSystem__detectStuckIOs(struct NVMeshSystem *sys) {
	const bool is_ec_ms = (nvmeibc_jentry_num_blocks > 1) && (get_sys_test_phase(sys) == BUNI_ERASURE_CODING_TESTING);
	NVMeshSystem_detectStuckIOs(sys, ut_conf__get_base()->is_valgrind || is_ec_ms);	// In valgrind, wait more, coz everything is slower. ec_ms IO only have 2 journal entries
}

/************************* Unitest apps for clients **************************/
static bool dbg_di_inject_on = 0;			// Todo: Turn on when debug di is used
/*
	Note this funcion always verifies nblocks of 4K sized sectors on the input buffer
*/
void __unitest_verify_blocks_pattern_data(const void *buf, const int nblocks, u64 pat, bool verify_out_of_bound, bool verify_match) {
	u64 *p = (u64*)buf;

	int i, n_u64_in_block = NVMEIBC_SECTOR_SIZE / sizeof(u64);
	for (i = 0; i < nblocks; i++) {			// Iterate over all blocks
		if (dbg_di_inject_on) {
			if (verify_match){
				BUG_ON(*p != pat);				// Verify only first 8 bytes
			} else {
				BUG_ON(*p == pat);				// Verify only first 8 bytes
			}
			p += n_u64_in_block;
		} else {
			const u64* end = p + n_u64_in_block;
			for (; p < end; p++)			// Verify entire block
				if (verify_match){
					BUG_ON(*p != pat);
				} else {
					BUG_ON(*p == pat);
				}
		}
	}
	if (verify_out_of_bound && !dbg_di_inject_on)
		BUG_ON((((u64*)buf)[-1] == pat)||(p[0] == pat)); // Check that no byte was written before/after the array
}

void __unitest_verify_blocks_pattern(const void *buf, const int nblocks, u64 pat, bool verify_out_of_bound){
	__unitest_verify_blocks_pattern_data(buf, nblocks, pat, verify_out_of_bound, true);
}

/* generate a unique 64b pattern */
u64 __unitest_get_pattern(void) {
	static u64 magic = 0x12345678;			// Must be > 0xFF;
	magic++; //magic = magic + (time(NULL) & 0xf + 1); // using bits from time adds some randomness
	return magic;
}
u64 __unitest_get_trimmed_u64(void) {
	static const union { u64 all; u8 bytes[8]; } rv = {.bytes = {[0 ... 7] = ramDiskSimulator_TRIMVAL}};
	return rv.all;
}

/* fill the specified blocks with unique pattern and return it */
u64 __unitest_fill_blocks_rand_pattern(void *buf, int n_blks) {
	const u64 pattern = rand() + 0xFF; // Must be > 0xFF;
	u64 *p = (u64*)buf, *end = p + (NVMEIBC_SECTOR2BYTE(n_blks) / sizeof(u64));
	for (; p < end; p++)
		*p = pattern;
	return pattern;
}

void __unitest_fill_blocks_with_pattern(void *buf, int n_blks, const u64 pattern) {
	u64 *p = (u64*)buf, *end = p + (NVMEIBC_SECTOR2BYTE(n_blks) / sizeof(u64));
	for (; p < end; p++)
		*p = pattern;
}

/* fill the specified blocks with unique pattern and return it */
u64 __unitest_fill_blocks_unique_pattern(void *buf, int n_blks) {
	const u64 pattern = __unitest_get_pattern();
	__unitest_fill_blocks_with_pattern(buf, n_blks, pattern);
	return pattern;
}

/* fill the specified blocks with unique pattern interleaved with block vlba and return the unique pattern */
u64 __unitest_fill_blocks_unique_pattern_and_lba(void *buf, u64 start_lba, int n_blks) {
	const u64 pattern = __unitest_get_pattern();
	u64 *p = (u64*)buf, *end = p + (NVMEIBC_SECTOR2BYTE(n_blks) / sizeof(u64));
	for (; p < end; p++) {
		*(p++) = pattern;
		*p = start_lba + ((((void *)p) - buf) >> NVMEIBC_SECTOR_SHIFT);
	}
	return pattern;
}

/* Turn on a bit at a specific offset indicating a certain sector needs to be written on */
void __set_sector_to_write(u8* flags, u8 offset) {
	BUG_ON(offset > (2 ^(NVMEIBC_SECTOR_SHIFT - KERNEL_SECTOR_SHIFT)));
	*flags |= 1 << offset;
}

/* Fill a kernel sector with a unique pattern */
void __unitest_fill_one_kernel_sector_with_pattern(void *buf, u64 kernel_sector_offset) {
	const u64 pattern = __unitest_get_pattern();
	u64 *p = (u64*)((u8*)buf + (kernel_sector_offset << KERNEL_SECTOR_SHIFT)), *end = p + ((1 << KERNEL_SECTOR_SHIFT) / sizeof(u64));
	for (; p < end; p++) {	// If we want to add any additional identification we can add them here
		*p = pattern;
	}
}

/* Write unique patterns on specific kernel sectors of a buffer */
void __unitest_fill_buffer_sectors_with_pattern(void *buf, u64 buf_len, u8 kernel_sectors_to_write_flags) {
	u64 sector_index;
	BUG_ON(buf_len < (1 << KERNEL_SECTOR_SHIFT));	// Must be larger than a single sector
	BUG_ON(buf_len > (1 << NVMEIBC_SECTOR_SHIFT));	// Cannot be larger than a 4k block due to number of bits in flag
	for (sector_index = 0; sector_index < (buf_len >> KERNEL_SECTOR_SHIFT); sector_index++) {
		if (kernel_sectors_to_write_flags & (1 << sector_index))
			__unitest_fill_one_kernel_sector_with_pattern(buf, sector_index);
	}
}

void __unitest_verify_sector_pattern(u64 total_buf_len, const void *inbuf, const void *outbuf, const int kernel_sector_offset, const int nksectors) {
	u64 offset = (kernel_sector_offset << KERNEL_SECTOR_SHIFT);
	u64 *desired = (u64*)((u8*)inbuf + offset);
	u64 *desired_end = (u64*)((u8*)desired + (nksectors << KERNEL_SECTOR_SHIFT));

	u64 *actual = (u64*)(outbuf);
	u64 *actual_start_read = (u64*)((u8*)actual + offset);
	u64 *actual_end_buf = (u64*)((u8*)actual + total_buf_len);

	BUG_ON(total_buf_len % (1 << KERNEL_SECTOR_SHIFT));
	// Verify all sectors before where the data actually starts are clear
	for (; actual < actual_start_read; actual++)
		BUG_ON(*actual);
	// Verify the read section is identical
	for (; desired < desired_end; desired++, actual++)
		BUG_ON(*desired != *actual);
	// Verify all sectors after where the data actually ends are clear
	for (; actual < actual_end_buf; actual++)
		BUG_ON(*actual);
}

// returns offset of first byte that differ between the buffers. -1 if they are identical
static int __mem_find_first_mismatch_8(const u8 *buf1, const u8 *buf2, int n_blks)
{
	u64 *p1 = (u64*)buf1, *p2 = (u64*)buf2;
	int i, n_u64_in_block = NVMEIBC_SECTOR_SIZE / sizeof(u64);
	if (!dbg_di_inject_on) {				// If no injection test for equality using fast memcmp
		if (memcmp(buf1, buf2, NVMEIBC_SECTOR2BYTE(n_blks)) == 0)
			return -1;
	}
	for (i = 0; i < n_blks; i++) {			// Iterate over all blocks
		if (dbg_di_inject_on) {
			if (*p1 != *p2)
				return ((u8*)p1 - buf1);	// Offset in bytes of first mismatch
			p1 += n_u64_in_block;
			p2 += n_u64_in_block;
		} else {
			const u64* end = p1 + n_u64_in_block;
			for (; p1 < end; p1++, p2++)	// Verify entire block
				if (*p1 != *p2)
					return ((u8*)p1 - buf1);	// Offset in bytes of first mismatch
		}
	}
	return -1;								// memcmp(buf1,buf2) == 0
}

// TODO - review below code when enabled
//AK: TODO - for write do the following validation flow
//To test sub-block write 'len_512B_Blocks' at 'start_512B_Block' * 512B:
//1. Write minimal number (let it be 'X') of 4k aligned blocks of '0' such that they contain all kernel sectors in range [start_512B_Block, start_512B_Block + len_512B_Blocks)
//2. Zero-initialize some sub-block write buffer of the length 4k * 'X' (let it be 'write_buf')
//		You can use __unitest_fill_buffer_sectors_with_pattern() on the sectors in range [start_512B_Block, start_512B_Block + len_512B_Blocks)
//		Write 'len_512B_Blocks' * 512B from (u8*)write_buf + start_512B_Block * 512
//3. Do a 4k aligned read of 'X' blocks (let it be 'read_buf')
//4. For every sector index-
//		if index is part of the range [start_512B_Block, start_512B_Block + len_512B_Blocks) - compare to the source buffer at the proper offset
//		else, make sure it's all '0'
//	This can be done by calling __unitest_verify_sector_pattern()
/*
static int unitest_partial_write_IO(struct clientSimulator* client, int volInd, u64 start_512B_Block, int len_512B_Blocks)
{
	int rv = 0;
	//The last NVMEIBC_SECTOR_SIZE sized block index
	u64 start_4k_block;
	//The last NVMEIBC_SECTOR_SIZE sized block index
	u64 last_4k_block;
	//The number of NVMEIBC_SECTOR_SIZE sized blocks required by the wrapper bio
	uint num_4k_blocks = get_numer_of_required_4k_blocks_from_512B_IO(start_512B_Block, (len_512B_Blocks << KERNEL_SECTOR_SHIFT), &start_4k_block, &last_4k_block);

	const int memSize	= num_4k_blocks * NVMEIBC_SECTOR_SIZE;	// Total array in bytes - note that we allocate NVMEIBC_SECTOR_SIZE blocks to use the magic generation funciton
	u8	*write_mem		= sim_kmalloc(memSize, GFP_KERNEL);		// Array to read/write to disk
	u8	*read_mem		= sim_kmalloc(memSize, GFP_KERNEL);		// Array to read/write to disk
	u64 offset	= start_512B_Block - nvmeibc_block_to_kernel_block(start_4k_block);
	unsigned int i;

	// Clear the arrays
	memset(write_mem, 0, memSize);
	memset(read_mem, 0, memSize);

	//First, write all '0' on the disk
	rv = osSimulator_writeArrWait(&client->OS, volInd,
			nvmeibc_block_to_kernel_block(start_4k_block),
			nvmeibc_block_to_kernel_block(num_4k_blocks),
			write_mem);

	//Fill all the write buffer kernel sectors with numbers unique to each
	for(i = 0; i < num_4k_blocks; i++)
	{
		__unitest_fill_buffer_sectors_with_pattern(write_mem + i * NVMEIBC_SECTOR_SIZE, NVMEIBC_SECTOR_SIZE, -1);
	}

	//Only write the relevant range from write_mem on the disk
	rv = osSimulator_writeArrWait(
			&client->OS, volInd,
			offset, len_512B_Blocks,
			write_mem + (offset * (1 << KERNEL_SECTOR_SHIFT)));
	REPORT_ERROR(rv);

	// Write should always succeed
	rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
	BUG_ON(!rv);

	//Read the entire 4k aligned range
	rv = osSimulator_readArrWait(&client->OS, volInd,
		nvmeibc_block_to_kernel_block(start_4k_block),
		nvmeibc_block_to_kernel_block(num_4k_blocks),
		read_mem);
	REPORT_ERROR(rv);

	__unitest_verify_sector_pattern(memSize, write_mem, read_mem, offset, len_512B_Blocks);

	sim_kfree(write_mem);
	sim_kfree(read_mem);
	return rv;
}
*/

/* Testing partial IOs
 * Validate cases where the volume is attached as one supporting 512B IO
 * client - client simulator instance
 * volInd - Request io on volume 'volInd'
 * start_512B_Block	- Start block (of 512B blocks)
 * len_512B_Blocks	- Length of IO in blocks of '512B'
 * should_succeed 	- true iff the read should succeed and the result can be compared to whatever pattern was written
 *
 * Note - this function assumes that the volume at volInd was attached as a 512B alignment IO allowed before the function was called
 */
static int unitest_partial_read_IO(struct clientSimulator* client, int volInd, u64 start_512B_Block, int len_512B_Blocks, const bool should_succeed){
	int rv = 0;
	u64 start_4k_block;
	u64 last_4k_block;
	const int num_4k_blocks = get_numer_of_required_4k_blocks_from_512B_IO(start_512B_Block, (len_512B_Blocks << KERNEL_SECTOR_SHIFT), &start_4k_block, &last_4k_block);
	const int memSize	= num_4k_blocks * NVMEIBC_SECTOR_SIZE;	// Total array in bytes - note that we allocate NVMEIBC_SECTOR_SIZE blocks to use the magic generation funciton
	// CR allocate buffers and pass them to the test, reduces amount of sim_kmalloc/sim_kfree, use memset regardless
	u8	*write_mem			= sim_kmalloc(memSize, GFP_KERNEL);		// Array to read/write to disk
	u8	*read_mem			= sim_kmalloc(memSize, GFP_KERNEL);		// Array to read/write to disk

	u64 read_offset	= start_512B_Block - nvmeibc_block_to_kernel_block(start_4k_block);
	int i;

	// Preparing for 512 Byte write
	// Clear the arrays
	memset(write_mem, 0, memSize);
	memset(read_mem, 0, memSize);
	for(i = 0; i < num_4k_blocks; i++)	// Fill each 4k block with unique 8 512b patterns
		__unitest_fill_buffer_sectors_with_pattern(write_mem + i * NVMEIBC_SECTOR_SIZE, NVMEIBC_SECTOR_SIZE, -1);

	// TODO - os_createBio uses full pages and page size constants and not bdev_block_size
	// Writes are currently only supported if aligned to 4k blocks
	rv = osSimulator_writeArrWait(&client->OS, volInd, nvmeibc_block_to_kernel_block(start_4k_block), nvmeibc_block_to_kernel_block(num_4k_blocks), write_mem);
	REPORT_ERROR(rv);

	// Both read and write fail
	rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
	if (should_succeed) {
		BUG_ON(rv);
	}

	// Reads have 512b granularity, same note as above os_createBio uses PAGE_SIZE for length
	rv = osSimulator_readArrWait(&client->OS, volInd, start_512B_Block, len_512B_Blocks, read_mem + (read_offset << KERNEL_SECTOR_SHIFT));
	REPORT_ERROR(rv);

	// Verify IO execution
	rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
	if (should_succeed) {
		BUG_ON(rv);
		__unitest_verify_sector_pattern(memSize, write_mem, read_mem, read_offset, len_512B_Blocks);
	} else {
		BUG_ON(rv != -EIO);
		rv = 0;
	}

	if (should_succeed) {
		memset(read_mem, 0, memSize);

		for(i = 0; i < len_512B_Blocks; i++)	// Fill each 512b sub-block with unique patterns
			__unitest_fill_buffer_sectors_with_pattern(&write_mem[(read_offset + i) << KERNEL_SECTOR_SHIFT], 1 << KERNEL_SECTOR_SHIFT, -1);

		// Overwrite only the sub-block parts of the data
		rv = osSimulator_writeArrWait(&client->OS, volInd, start_512B_Block, len_512B_Blocks, &write_mem[read_offset << KERNEL_SECTOR_SHIFT]);
		REPORT_ERROR(rv);

		// Read the entire 4k block range, verify that previously written blocks remain, and overridden sub-blocks are updated
		rv = osSimulator_readArrWait(&client->OS, volInd, nvmeibc_block_to_kernel_block(start_4k_block), nvmeibc_block_to_kernel_block(num_4k_blocks), read_mem);
		REPORT_ERROR(rv);

		__unitest_verify_sector_pattern(memSize, write_mem, read_mem, 0, nvmeibc_block_to_kernel_block(num_4k_blocks));
	}

	sim_kfree(write_mem);
	sim_kfree(read_mem);
	return rv;
}

/* Testing IO good path. Trim the area, write to it, read and verify that the results are OK.
 * volInd - Request io on volume 'volInd'
 * startBlock - Start from X block
 * lenBlocks - Length of IO in blocks */
static int unitest_IO(struct clientSimulator* client, int volInd, u64 startBlock, int lenBlocks){
	int rv = 0;
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8        *mem 		 = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	rv = osSimulator_trim(	  &client->OS, volInd, startBlock, lenBlocks);			REPORT_ERROR(rv);
	clientSimulator_wait_for_all_bio_ops(client);
	rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(mem,lenBlocks,__unitest_get_trimmed_u64(), false);	// Verify that discarded areas are ok.
	magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	memset(mem, 0   	, memSize);								// Clear the array
	rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(mem, lenBlocks, magic_pattern, false);			// Verify that read and write matched.
	sim_kfree(mem);
	_NI_dmesg(trace_bunitest_unitest_IO, "*************** (@VOL_I) end", volInd);
	return rv;
}

/* Test Good path IO on all volumes. */
TEST_FUNC int unitest_GoodPathIO(struct NVMeshSystem *sys){
	int rv = 0;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	NVMeshSystem_all_clients_dbg_di(sys, true);
	if (NVMEIBC_SECTOR_SIZE < PAGE_SIZE) {
		rv = unitest_IO(client, 0, 1,  10);						// Test partial write of 4K pages if block devices uses smaller blocks
	}
	rv = unitest_IO(client, 0, _addr4k(3,30),  _addr4k(3,5)); 	// Test IO crossing stripes on all disks of Raid1+0
	rv = unitest_IO(client, 1, _addr4k(3,30),  _addr4k(2,31)); 	// Test embedded IO limit (trim) JBOD
	rv = unitest_IO(client, 1, _addr4k(2,30),  _addr4k(4,5)); 	// Test IO on all chunks of JBOD
	rv = unitest_IO(client, 1, _addr4k(0,0),  _addr4k(0,1)); 	// Test embedded IO JBOD
	rv = unitest_IO(client, 1, _addr4k(1,16),  _addr4k(1,0)); 	// Test embedded IO JBOD mid lockset
	rv = unitest_IO(client, 1, _addr4k(0,20),  _addr4k(1,20)); 	// Test embedded IO JBOD mid lockset++
	rv = unitest_IO(client, 1, _addr4k(0, 0),  _addr4k(1,26));	// Test embedded IO limit (read/write) JBOD
	rv = unitest_IO(client, 1, _addr4k(0,0),  _addr4k(1,27)); 	// Test embedded IO over limit (read/write) JBOD
	rv = unitest_IO(client, 1, _addr4k(0,0),  _addr4k(2,31)); 	// Test embedded IO limit (trim) JBOD
	rv = unitest_IO(client, 1, _addr4k(0,0),  _addr4k(3,0)); 	// Test embedded IO over limit (trim) JBOD
	rv = unitest_IO(client, 2, _addr4k(1,30),  _addr4k(4,5)); 	// Write on all chunks/disks of Raid0
	rv = unitest_IO(client, 3, _addr4k(0,30),  _addr4k(2,5)); 	// Write on all chunks/disks of Raid1
	NVMeshSystem_all_clients_dbg_di(sys, false);
	NVMeshSystem_wipe_all_md_of_disks(sys); // After switching off debug di, metadata becomes invalid (edic changes). We have to clear it.
	return rv;
}

TEST_FUNC int unitest_read_mutable_buffer(struct NVMeshSystem *sys, int vol){
	int rv = 0;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	u64 const start_lba = 0;
	u64 const n_lbas = 3;
	u64 const n_lbas_in_bytes = n_lbas*NVMEIBC_SECTOR_SIZE;
	u8 *mem = sim_kmalloc(n_lbas_in_bytes, GFP_KERNEL);						// Array to read from disk
	const u64 magic_pattern = __unitest_fill_blocks_unique_pattern(mem, n_lbas);

	const int read_has_mutable_bio_buffers_prev = client->devs[0]->dp.read_has_mutable_bio_buffers;

	client->devs[vol]->dp.read_has_mutable_bio_buffers = 2;

	rv = osSimulator_writeArrWait(&client->OS, vol, start_lba, n_lbas, mem);
	REPORT_ERROR(rv);

	for(u32 idx = 0; idx < 10; ++idx){
		rv = osSimulator_readArr(&client->OS, vol, start_lba, n_lbas, mem);		REPORT_ERROR(rv);
		for(u32 mem_idx = 0; mem_idx < n_lbas_in_bytes; ++mem_idx){
			mem[mem_idx] = mem_idx % 128;
		}
		clientSimulator_wait_for_all_bio_ops(client);
	}

	rv = osSimulator_readArrWait( &client->OS, vol, start_lba, n_lbas, mem);		REPORT_ERROR(rv);
	__unitest_verify_blocks_pattern(mem, n_lbas, magic_pattern, false);			// Verify that read and write matched.
	sim_kfree(mem);
	client->devs[vol]->dp.read_has_mutable_bio_buffers = read_has_mutable_bio_buffers_prev;
	return rv;
}

/* Testing metadata during IO good path. Write to an area, wait get the metadata that was written.
   Look at another vlba. Validate that we got some different metadata.
   Write to the same area with different data. Validate that we got something different again.
   Perform the same write again. Validate that this time nothing changes.
   Enable debug di and validate we have something different one more time.

   First volume is mirror. For mirror we use 13 bits for edic. Sometimes, generated random data introduce collisions.
*/
TEST_FUNC int unitest_MetadataGoodPathIO(struct NVMeshSystem *sys){
	const int volInd = 0; // Look at the first volume
	struct volumeDescriptor	*vol = &sys->mdb.vols[0];
	struct disk_range *seg = vol->segs; // Look at the first segment
	const u64 start_block = 0, len_blocks = 2; // Test by writing 2 blocks in first segment
	struct clientSimulator *client = &sys->clients[0]; // Test via the first client
	int rv = 0;
	const int memSize = len_blocks*NVMEIBC_SECTOR_SIZE; // Total array in bytes

	u8 *mem = sim_kmalloc(memSize, GFP_KERNEL); // Array to read/write to disk
	u64 *md_block_0 = physSegMDIdxPtr_off(seg, 0); // First block in this segment
	u64 *md_block_1 = physSegMDIdxPtr_off(seg, 1); // Second block in segment
	u64 prev_mem;

	//cleanup
	__unitest_fill_blocks_with_pattern(mem, len_blocks, 0); // Set the first pattern.
	rv = osSimulator_writeArrWait(&client->OS, volInd, start_block, len_blocks, mem); REPORT_ERROR(rv);
	BUG_ON(rv);

	// Test without debug di
	prev_mem = *md_block_0;
	__unitest_fill_blocks_with_pattern(mem, len_blocks, 0xABCABC00); // Set the first pattern.
	rv = osSimulator_writeArrWait(&client->OS, volInd, start_block, len_blocks, mem); REPORT_ERROR(rv);
	BUG_ON(*md_block_0 == prev_mem);
	BUG_ON(*md_block_0 == *md_block_1);

	// Write new data to the same area. Metadata should change
	prev_mem = *md_block_0;
	__unitest_fill_blocks_with_pattern(mem, len_blocks, 0xABCABC01); // Set the second pattern.
	rv = osSimulator_writeArrWait(&client->OS, volInd, start_block, len_blocks, mem); REPORT_ERROR(rv);
	BUG_ON(*md_block_0 == prev_mem);
	BUG_ON(*md_block_0 == *md_block_1);

	// Write same data again. This time metadata should not change
	prev_mem = *md_block_0;
	rv = osSimulator_writeArrWait(&client->OS, volInd, start_block, len_blocks, mem); REPORT_ERROR(rv);
	BUG_ON(*md_block_0 != prev_mem);
	BUG_ON(*md_block_0 == *md_block_1);

	// Test with debug di. Metadata should change
	NVMeshSystem_all_clients_dbg_di(sys, true);
	prev_mem = *md_block_0;
	__unitest_fill_blocks_with_pattern(mem, len_blocks, 0xABCABC02); // Set the third pattern.
	rv = osSimulator_writeArrWait(&client->OS, volInd, start_block, len_blocks, mem); REPORT_ERROR(rv);
	BUG_ON(*md_block_0 == prev_mem);
	BUG_ON(*md_block_0 == *md_block_1);
	NVMeshSystem_all_clients_dbg_di(sys, false);

	sim_kfree(mem);
	_NI_dmesg(trace_bunitest_unitest_MetadataIntegrity, "*************** (@VOL_I) end", volInd);
	return rv;
}

/* Test illegal IO on some volumes. */
TEST_FUNC int unitest_IllegalIO(struct NVMeshSystem *sys){
	int rv = 0, lenBlocks = 1, v = 0;
	const int size	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8        *mem 		 = sim_kmalloc(size, GFP_KERNEL);						// Array to read/write to disk
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	struct osSimulator *OS = &client->OS;

	//rv = osSimulator_writeArr(OS, 0, -1, -1, mem); // Write to negative lba with negative length. Commented due to attempt to allocate 2^64 blocks
	if (NVMEIBC_SECTOR_SIZE == PAGE_SIZE) {						// Test IO to fractions of blocks
		struct block_device* bdev = &OS->bds[v];
		const u32 f = 4;										// Issue in fractions of 1/4 blocks
		u32 *bdev_b_size = &bdev->bd_disk->queue->limits.physical_block_size;
		*bdev_b_size /= f;
		BUG_ON(osSimulator_writeArr(OS, 0, 1,               2*f, mem)); // Length is valid (2 blocks) but offset is invalid
		BUG_ON(osSimulator_writeArr(OS, 0, 1,                 5, mem)); // Both length and offset are invalid

		*bdev_b_size *= f;
		BUG_ON(osSimulator_writeArr_f_missaligned(OS, 0, 1,   4, mem, 0L, NULL));	// Blocks are not aligned to pages
		BUG_ON(dp_io_stats_get_counter(&client->devs[0]->dp.io_stats, DP_IO_STATS_IGNORED_ERR)     != 3); // all utests failed as expected
	}

	// Write and read with bi_vcnt=0. Should succeed.
	if (KS_BVEC_ITER) {		// Otherwise we simulate an older kernel where bi_vcnt can be used.
		BUG_ON(osSimulator_writeArr_f_no_vcnt(OS, v, 0, lenBlocks, mem, 0L, NULL));
		BUG_ON(osSimulator_readArr_f_no_vcnt(OS, v, 0, lenBlocks, mem, 0L, NULL));
		clientSimulator_wait_for_all_bio_ops(client);
		BUG_ON(dp_io_stats_get_counter(&client->devs[0]->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL) != 0); // all ops succeeded as expected
	}

	BUG_ON(osSimulator_writeArr(OS, v, client->devs[0]->size, 1, mem)); // Write outside of the volume. Should not be executed
	BUG_ON(osSimulator_writeArr(OS, v, client->devs[0]->size, 0, mem)); // Write zero length array to end of volume. 	Should not be executed
	BUG_ON(osSimulator_writeArr(OS, v, 0,                     0, mem)); // Write zero length array to start of volume. 	Should not be executed
	BUG_ON(osSimulator_writeArr(OS, v, -1,                  100, mem)); // Write to negative lba a lot of data. 	Should not be executed
	BUG_ON(osSimulator_writeArr(OS, v, -10,                   9, mem)); // Write to negative lba. 	Should not be executed
	BUG_ON(osSimulator_writeArr(OS, v, ((0xFFULL << 56)+1),size, mem)); // Write to very negative lba. 	Should not be executed
	clientSimulator_wait_for_all_bio_ops(client);
	BUG_ON(dp_io_stats_get_counter(&client->devs[0]->dp.io_stats, DP_IO_STATS_CRITICAL_FAIL) != 6); // all utests failed as expected
	sim_kfree(mem);
	return rv;
}

static int osSimulator_gen_generic_op(enum nvmeib_block_io_op op, struct osSimulator* sim, int volInd, u64 start, u64 length, u8 dst[]){
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_READ:		return osSimulator_readArr( sim, volInd, start, length, dst);
	case NVMEIB_BLOCK_IO_OP_WRITE:		return osSimulator_writeArr(sim, volInd, start, length, dst);
	case NVMEIB_BLOCK_IO_OP_DISCARD:	return osSimulator_trim(    sim, volInd, start, length);
	default: BUG();
	}
	return -1;
}

static void attach_volume_with_sub_block_io(struct NVMeshSystem *sys, int volInd) {
	sys->mdb.vols[volInd].nextCmd = volCmds_New;
	NVMeshSystem_send_volumes_config_to_clients_with_param(sys, -1, 'r', false, RESERVATION_MODE_IRRELEVANT, true);
	NVMeshSystem_serialize(sys);
	sys->mdb.vols[volInd].nextCmd = volCmds_Illegal;
}

static int unitest_subblock_io_iteration(struct clientSimulator *client, const int volInd, u64 *n_topo_ios, const bool is_ec)
{
	int rv = 0;
	const uint n_max_kernel_sectors_for_io = (LOCKSET_SLICES << KERNEL_SECTOR_TO_SECTOR_SHIFT);	// Issue up to 32 4k Block IOs, use for blockset offset in 512B sectors
	uint block_offset_in_512_sectors = (rand() % n_max_kernel_sectors_for_io), num_512_sectors, end_of_blockset;

	// 4k aligned read for sanity:
	rv |= unitest_partial_read_IO(client, volInd, 16, 8, true); *n_topo_ios += 4;

	// sub-block read 3 512B blocks (part of 1 4K block):
	rv |= unitest_partial_read_IO(client, volInd, 0, 3, true); *n_topo_ios += 4;

	// sub-block read 2 4[K] blocks with 7/8 block offset:
	rv |= unitest_partial_read_IO(client, volInd, 8+7, 16, true); *n_topo_ios += 4;

	// sub-block read 3 512B blocks (as part of 2 4K block):
	rv |= unitest_partial_read_IO(client, volInd, 6, 3, true); *n_topo_ios += 4;

	// Test random start offset within the first blockset, issue all IO sizes from 1->31 4k blocks (248 kernel sectors), and randomize blockset offset if starts outside of first blockset
	for (num_512_sectors = 1; num_512_sectors <= n_max_kernel_sectors_for_io; num_512_sectors++) {
		rv |= unitest_partial_read_IO(client, volInd, block_offset_in_512_sectors, num_512_sectors, true); *n_topo_ios += 4;
		block_offset_in_512_sectors += num_512_sectors;
		if (block_offset_in_512_sectors > n_max_kernel_sectors_for_io) {	// If next offset is in 2nd blockset -> randomly reset offset within first block
			block_offset_in_512_sectors = rand() % KERNEL_SECTORS_IN_NVMEIBC_SECTOR;
		}
	}

	{	// Reading at the end of blockset around 128K (i.e. around 256 512b block) alignment must succeed:
		int slice_size = (is_ec) ? 8 : 1;
		end_of_blockset = (LOCKSET_SLICES * (slice_size << KERNEL_SECTOR_TO_SECTOR_SHIFT)) - 4;
		rv |= unitest_partial_read_IO(client, volInd, end_of_blockset++, 3, true); *n_topo_ios += 4;
		rv |= unitest_partial_read_IO(client, volInd, end_of_blockset++, 3, true); *n_topo_ios += 4;
		rv |= unitest_partial_read_IO(client, volInd, end_of_blockset++, 3, true); *n_topo_ios += 4;
		rv |= unitest_partial_read_IO(client, volInd, end_of_blockset++, 3, true); *n_topo_ios += 4;
		rv |= unitest_partial_read_IO(client, volInd, end_of_blockset++, 3, true); *n_topo_ios += 4;
		// Write into partial first block, 2 full slices and anoter block and partial
		rv |= unitest_partial_read_IO(client, volInd, 7, slice_size*8*2+14, true); *n_topo_ios += 4;
		// Write into 2nd partial block complete the slice and the first block of the next slice is partial
		rv |= unitest_partial_read_IO(client, volInd, 12, (slice_size*8)-6, true); *n_topo_ios += 4;
	}

	// Teting reading at the end of the volume:
	// Reading past the volume end should fail:
	rv |= unitest_partial_read_IO(client, volInd, nvmeibc_block_to_kernel_block(client->devs[0]->size), 3, false); *n_topo_ios += 2;
	rv |= unitest_partial_read_IO(client, volInd, nvmeibc_block_to_kernel_block(client->devs[0]->size) - 2, 3, false); *n_topo_ios += 2;

	// Reading at the end of the volume should succeed:
	rv |= unitest_partial_read_IO(client, volInd, nvmeibc_block_to_kernel_block(client->devs[0]->size) - 3, 3, true); *n_topo_ios += 4;
	rv |= unitest_partial_read_IO(client, volInd, nvmeibc_block_to_kernel_block(client->devs[0]->size) - 12, 11, true); *n_topo_ios += 4;

	return rv;
}

TEST_FUNC int unitest_SubBlockIO(struct NVMeshSystem *sys, const bool is_ec) {
	int rv = 0;
	int volInd = 0;
	struct volume_segment_index vsi = {0,0,0,0};
	struct clientSimulator *client = &sys->clients[0];			// Test using the first client
	const u64 test_timer_all = jiffies;
	u64 test_timer = test_timer_all;
	struct disk_range *curSeg = &sys->mdb.vols[vsi.volume].segs[0];
	struct topologies_enumerator topos = create_no_protection_topo_enum_ordered(NVMeshSystem_TstPRaid_init_rel(sys, vsi));	// TODO - after stability change to random out of 20 (50 total for EC, 2/4 for mirror)
	struct tTopoOfPraid* r1 = &sys->tcf.vols[vsi.volume].chunks[0].raids[0];
	u64 n_total_ios = 0;
	char iter_descript[64];
	char binje_descript[10] = {0};
	const u32 rand_seed = (u32)test_timer;
	struct nvmeibc_block_device *dev;
	const u32 old_binje = nvmeibc_jentry_num_blocks;
	u32 new_binje = 2;

	srand(rand_seed);

	// Reattach volume to support 512b operations
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	attach_volume_with_sub_block_io(sys, volInd);
	BUG_ON(!clientSimulator_does_vol_allow_512B_IO(client, volInd));

	if (is_ec && (nvmeibc_jentry_num_blocks == 1)) { // Extend binje to two slices, ensures we test multi-slice IO with sub-block on different slices, always repeated with binje == 1
restart_test:
		dev = sys->clients->devs[0];
		dev->dp.alignment_sectors.write = ((dev->dp.p.slice_size * new_binje) << KERNEL_SECTOR_TO_SECTOR_SHIFT);
		BUG_ON(dev->dp.p.binje != nvmeibc_jentry_num_blocks);
		dev->dp.p.binje = nvmeibc_jentry_num_blocks = new_binje;
		scnprintf(binje_descript, 10, "binje=%d ", new_binje);
	}

	rv |= unitest_subblock_io_iteration(client, volInd, &n_total_ios, is_ec);
	unitest_print("**** SubBlockIO Test (seed = %x) %ssent %llu 512B IOs result => PASS took %d[mSec]\n", rand_seed, binje_descript, n_total_ios, (int)(((jiffies-test_timer)*1000)/HZ));

	while (topos.move_next(&topos)) {
		const u64 topo_timer = jiffies;
		u64 n_topo_ios = 0;
		struct topology_sgmnts_t topo = topos.curr;
		__switch_to_new_topo(client, topo, r1, curSeg);
		topo_enum_tostring(&topos, iter_descript);

		rv |= unitest_subblock_io_iteration(client, volInd, &n_topo_ios, is_ec);

		unitest_print("**** SubBlockIO Test (seed = %x) %s %ssent %llu 512B IOs result => PASS took %d[mSec]\n", rand_seed, iter_descript, binje_descript, n_topo_ios, (int)(((jiffies-topo_timer)*1000)/HZ));
		__prepare_for_next_iteration(client, topo, curSeg);
		NVMeshSystem_wipe_all_dirty_bits(sys);
		n_total_ios += n_topo_ios;
	}

	{	// Reset all topologies back to RW
		const enum NVMEIBTC_DS_MODE reset[N_MAX_RAID_SLICE_LEN] = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
		tomaSimulator_switchTopoEC(r1->header.uuid, reset, SW_TOPO__WAIT_ACK_DR, NULL);
	}

	if (is_ec) {
		if (nvmeibc_jentry_num_blocks == old_binje) {	// Restart test with binje 1, will also clean up journals, reset all topos
			test_timer = jiffies; // restart timer for non-degraded topo with new_binje
			new_binje = 1;
			topos = create_no_protection_topo_enum_ordered(NVMeshSystem_TstPRaid_init_rel(sys, vsi));
			goto restart_test;
		} else {	// Restore original binje value post test
			dev = sys->clients->devs[0];
			dev->dp.p.binje = nvmeibc_jentry_num_blocks = old_binje;
		}
	}

	// Return volume attachment to 4k blocks
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	send_command_to_vol(sys, -1, volInd, volCmds_New);

	unitest_print("*************** Sub Block tests (seed = %x) end sent %llu IOs - took %d[Sec]\n", rand_seed, n_total_ios, (int)((jiffies-test_timer_all)/HZ));
	return rv;
}

TEST_FUNC int unitest_IOonReadOnlyVols(struct NVMeshSystem *sys){
	int rv = 0, volInd = 0;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const int startBlock = 17, lenBlocks = 1;					// 1[Block]
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;
	u8        *mem 		 = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	rv = osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	clientSimulator_wait_for_all_bio_ops(client);
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	send_command_to_vol(sys, -1, volInd, volCmds_AttachReadOnly);
	rv = osSimulator_mount(&client->OS, volInd, FMODE_READ | FMODE_EXCL);	BUG_ON(rv); // Convert to read only
	memset(mem, 0, memSize);									// Set a pattern which will not be written
	rv = osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Not executed
	rv = osSimulator_trim(	  &client->OS, volInd, startBlock, lenBlocks);			REPORT_ERROR(rv);	// Not executed
	rv = osSimulator_readArr( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);	// Read the original magic
	clientSimulator_wait_for_all_bio_ops(client);
	__unitest_verify_blocks_pattern(mem, lenBlocks, magic_pattern, false);					// Verify that read and write matched.
	osSimulator_unmount(&client->OS, volInd);
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	send_command_to_vol(sys, -1, volInd, volCmds_New);
	sim_kfree(mem);
	return rv;
}

struct io_cmd_completion_count_disk_hooks {
	struct nvmeibc_disk_hooks disk_hooks;
	struct NVMeshSystem *sys;
	int cb_counts[NVMESH_N_PHYS_DISKS];
};

static int __io_cmd_completion_count(struct nvmeibc_disk_hook_args *args, struct nvmeibc_block_command *cmd)
{
	struct io_cmd_completion_count_disk_hooks *hooks = container_of(args, struct io_cmd_completion_count_disk_hooks, disk_hooks.args);
	int disk_i = nvmeibc_disk_from_base(cmd->ds->disk) - hooks->sys->clients[0].physDiscs;
	++(hooks->cb_counts[disk_i]);
	return 0;
}

static void __io_cmd_completion_count_reset_counts(struct io_cmd_completion_count_disk_hooks *hooks)
{
	memset(hooks->cb_counts, 0, sizeof(hooks->cb_counts[0]) * ARRAY_SIZE(hooks->cb_counts));
}

static void __io_cmd_completion_count_verify_single(const struct io_cmd_completion_count_disk_hooks *hooks, int disk_i, int expected_count)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(hooks->cb_counts); ++i) {
		if (i == disk_i)
			BUG_ON(hooks->cb_counts[i] != expected_count);
		else
			BUG_ON(hooks->cb_counts[i] != 0);
	}
}

TEST_FUNC int unitest_LocalReadOptimization(struct NVMeshSystem *sys){
	int rv = 0, volInd = 0;
	struct io_cmd_completion_count_disk_hooks hooks = {
			.sys = sys,
			.disk_hooks = { .io_cmd_completion = __io_cmd_completion_count } };
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const int startBlock = 17, lenBlocks = 1;					// 1[Block]
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;
	u8        *mem 		 = sim_kmalloc(memSize, GFP_KERNEL);		// Array to read/write to disk
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem); REPORT_ERROR(rv);

	#define __verify_read_from_disk(_disk_i) \
		memset(mem, 0, memSize); \
		rv = osSimulator_readArrWait(&client->OS, volInd, startBlock, lenBlocks, mem); REPORT_ERROR(rv); \
		__unitest_verify_blocks_pattern(mem, lenBlocks, magic_pattern, false); \
		__io_cmd_completion_count_verify_single(&hooks, (_disk_i), 2 /* expected_count */); \
		__io_cmd_completion_count_reset_counts(&hooks);

	NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, &hooks.disk_hooks);

	// Consider both disks non-local
	client->physDiscs[0].access_local = false;
	client->physDiscs[1].access_local = false;

	__verify_read_from_disk(0);	// Sanity

	// Enable local read optimization, no other conditions are met yet
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	sys->mdb.vols[volInd].enable_local_read_optimization = true;
	send_command_to_vol(sys, -1, volInd, volCmds_New);

	__verify_read_from_disk(0);

	// Make the second disk local, not yet exclusive/readonly attach
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	client->physDiscs[1].access_local = true;
	send_command_to_vol(sys, -1, volInd, volCmds_New);

	__verify_read_from_disk(0);

	// Re-attach as read-only
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	send_command_to_vol(sys, -1, volInd, volCmds_AttachReadOnly);
	NVMeshSystem_serialize(sys);

	__verify_read_from_disk(1);

	// Re-attach as exclusive
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	send_command_to_vol(sys, -1, volInd, volCmds_AttachExclusive);
	NVMeshSystem_serialize(sys);

	__verify_read_from_disk(1);

	// Revert to all disks being non-local
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	client->physDiscs[1].access_local = false;
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	send_command_to_vol(sys, -1, volInd, volCmds_AttachExclusive);
	NVMeshSystem_serialize(sys);

	__verify_read_from_disk(0);

	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	sys->mdb.vols[volInd].enable_local_read_optimization = false;
	__unitest_volume_reservation_reset(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
	send_command_to_vol(sys, -1, volInd, volCmds_New);
	NVMeshSystem_serialize(sys);

	NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);

	#undef __verify_read_from_disk

	sim_kfree(mem);
	return rv;
}

TEST_FUNC int unitest_IOonRaid10_without_metadata(struct NVMeshSystem *sys) {
	int rv = 0, v = 0, i;
	struct clientSimulator *client = &sys->clients[0];
	for (i = 0; i < NVMESH_N_PHYS_DISKS_REGULAR_USE; i++)
		ramDiskSimulator_format_metadata(&sys->servers[i].ramDisk, false);
	NVMeshSystem_all_clients_di_metadata_wr(sys, false);
	rv = unitest_IO(client, v, _addr4k(3,30),  _addr4k(3,5)); 	// Test IO crossing stripes on all disks of Raid1+0
	NVMeshSystem_all_clients_di_metadata_wr(sys, true);
	for (i = 0; i < NVMESH_N_PHYS_DISKS_REGULAR_USE; i++)
		ramDiskSimulator_format_metadata(&sys->servers[i].ramDisk, true);
	NVMeshSystem_wipe_all_md_of_disks(sys);
	return rv;
}

bool tTopoOfPraid_verify_consistency(const struct NVMeshSystem *sys,
									 const struct tTopoOfPraid *r1_topo,
									 const struct disk_range *r1_conf)
{
	int i, is_same = true;
	const struct nvmeibt_client_topo_disk_segment *seg= r1_topo->s;
	u8 *first_seg_data, *curr_seg_data;						// pointer to RAM disk area of first/current segment
	for (i=0; i < r1_topo->header.n_segments; i++) {		// Find the first live segment
		if (seg[i].access_mode != NVMEIBTC_DS_MODE_DEAD)
			break;											// Daniel: Should take RW or W?
	}
	first_seg_data = physSegStartPtr((&r1_conf[i]));
	for (i++; i < r1_topo->header.n_segments; i++) {
		if (seg[i].access_mode == NVMEIBTC_DS_MODE_DEAD)
			continue;
		curr_seg_data  = physSegStartPtr((&r1_conf[i]));
		is_same = (__mem_find_first_mismatch_8(first_seg_data, curr_seg_data, __from4K(r1_conf->length)) == -1);
		if (!is_same) break;
	}
	return is_same;
}

/* Direct switch topology to different degraded modes */
TEST_FUNC int unitest_Raid1_SwitchTopo(struct NVMeshSystem *sys){
	const int volInd = 3;
	int i, node_ind;								// Volume will be used to test switch topology
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	int rv = 0;
	union nvmeibc_dbits_entry *db_val;
	const int lenBlocks = __from4K(2);			// Length of IO. 1 4K-block is written as mirrored, 1 4K-block only in degraded mode
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	enum NVMEIBTC_DS_MODE seg_statsDRW[2] = {NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW};
	enum NVMEIBTC_DS_MODE seg_statsWRW[2] = {NVMEIBTC_DS_MODE_W, NVMEIBTC_DS_MODE_RW};
	struct tTopoOfPraid*r1;
	struct switch_topo_options sto = {0};
	u8 *dst = NULL, *mem = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	u64 magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	const int startBlock = sys->mdb.vols[volInd].segs[0].length - 1;

	// -------------------- Iterate on all chunks, first and second raids
	for (int chunk_idx=0; chunk_idx<2; chunk_idx++) {
		r1 = &sys->tcf.vols[volInd].chunks[chunk_idx].raids[0]; // 1 block is written to {disk2,disk3}, 1 block only to disk5
		for (int s=0; s<2; s++) { 											// Kill one segment at a time of the current raid
			struct disk_range* curSeg = &sys->mdb.vols[volInd].segs[2*chunk_idx];
			tomaSimulator_switchTopoBlocked(client->devs[volInd], r1uuid(r1), seg_statsDRW[s], seg_statsDRW[s^1]);
			if (1) {													// Verify client correctly regi/unreg from the segments
				const int isDeadRegistered = tomaSimulator_isSegRegistered(&sys->servers[curSeg[s  ].node_id].simToma, curSeg[s  ].ruuid, client->inst_id);
				const int isLiveRegistered = tomaSimulator_isSegRegistered(&sys->servers[curSeg[s^1].node_id].simToma, curSeg[s^1].ruuid, client->inst_id);
				BUG_ON(isDeadRegistered || !isLiveRegistered);
			}
			magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
			rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
			for (i=0; i<2; i++, curSeg++){ 								// Verify that magic number was written to both mirrors
				const u64 phys_off = ((chunk_idx==0) ? __to4K(startBlock) : 0);
				const enum NVMEIBTC_DS_MODE dbm = r1->s[i^1].access_mode;	// Dirty bit of dead segment
				dst = physSegStartPtr_off(curSeg, phys_off);
				if (r1->s[i].access_mode == NVMEIBTC_DS_MODE_DEAD) {
					BUG_ON(*(u64*)dst == magic_pattern);				// This disk should not be written. Verify this
				} else {
					__unitest_verify_blocks_pattern(dst, __from4K(1), magic_pattern, true);		// This disk should be written
					db_val = physSegDBIdxPtr_off(curSeg, phys_off);
					switch (dbm) {
						case NVMEIBTC_DS_MODE_DEAD: BUG_ON(db_val->all_bits == 0); db_val->all_bits = 0; break;		// Turn on
						case NVMEIBTC_DS_MODE_W   : BUG_ON(db_val->all_bits != 0); break;		// Turn off;
						default: BUG_NOT_IMPLEMENTED_YET; break; // should never happen
					}
				}
			}
			tomaSimulator_switchTopo(r1uuid(r1), seg_statsWRW[s]    , seg_statsWRW[s^1], SW_TOPO__WAIT_ACK);		// {RW,W}
			if (1) {	// EXC-1716 - New type of switch-topo  {WR,W} -> {WR,D}, second Toma is not really dead and communicates with the client
				tomaSimulator_switchTopo(r1uuid(r1), seg_statsDRW[s], seg_statsDRW[s^1], SW_TOPO__WAIT_ACK);		// {RW,D}
				if (chunk_idx==0 && s==1)	{		// Pseudo-randomly send unregister from the "dead" toma to cause client-toma out of sync and verify client recovers in a warm fashion
					curSeg = &sys->mdb.vols[volInd].segs[2*chunk_idx];				// Live seg
					tomaSimulator_sendUnregisterMsg(r1, s, curSeg, client->inst_id);
					tomaSimulator_waitProtoEnd(&sys->servers[curSeg[1].node_id].simToma);	// wait for DEAD to unregister
				}
				tomaSimulator_switchTopo(r1uuid(r1), seg_statsWRW[s], seg_statsWRW[s^1], SW_TOPO__WAIT_ACK);	// Back to RW,W
			}
			tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		}
	}
	// -------------------- Genrate switch topology from toma whose segment becomes DEAD. Client will need to choose between UNREGISTER & SWITCH_TOPO_ACK to that toma (it will pick the SWITCH_TOPO_ACK as per Ronen's request) while all other TOMA's will get SWITCH_TOPO_ACK
	sto.dont_send_msg = false;
	sto.wait_for_ack = true;
	sto.use_seg_index = true;
	sto.seg_index = 1;
	r1 = &sys->tcf.vols[volInd].chunks[0].raids[0];
	tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW  , SW_TOPO__WAIT_ACK);
	tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW  , NVMEIBTC_DS_MODE_DEAD, sto);
	tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW  , NVMEIBTC_DS_MODE_RW  , SW_TOPO__WAIT_ACK);
	// -------------------- Genrate incorrect switch topology. Client will get out of sync and recover
	sto.wait_for_ack = false;
	sto.seg_index = 0;
	r1 = &sys->tcf.vols[volInd].chunks[0].raids[0];
	tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW  , SW_TOPO__WAIT_ACK);
	node_ind = tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW  , NVMEIBTC_DS_MODE_DEAD, sto); // client will see switch-topo with (lock_id == 0) & fail to warm. it wont send switch-topo-ack !!! it will unregister & register again.
	BUG_ON(node_ind < 0);
	tomaSimulator_waitProtoEnd(&sys->servers[node_ind].simToma);
	tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW  , NVMEIBTC_DS_MODE_RW  , SW_TOPO__WAIT_ACK);

	// -------------------- Genrate contradicting topologies between 2 tomas: Transition {RW(unreg),W(reg)} ==> {RW(reg),DEAD(unreg)}
	if (1) {
		int s = 0;
		struct disk_range *curSeg = &sys->mdb.vols[volInd].segs[0];
		struct serverSimulator *server = &sys->servers[curSeg->node_id];
		struct nvmeibc_topology *t = NULL;
		struct nvmeibc_disk_segment *rs;
		tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W, SW_TOPO__WAIT_ACK);
		serverSimulator_disconnect(server);
		server->simToma.state = tomaState_not_ready;
		tomaSimulator_sendUnregisterMsg(r1, s, &sys->mdb.vols[volInd].segs[0], client->inst_id);
		NVMeshSystem_serialize(sys);
		t = ___get_tail_topo_of_device(sys, volInd);
		rs = t->chunks[0].raid1s[0].segments;				// Verify client in {NVMEIBTC_DS_MODE_RW unregistered, NVMEIBTC_DS_MODE_W registered}. No IO.
		rv = ((t->newer == NULL) && (!nvmeibc_topo_is_io_ok(t)) &&
			  (rs[s  ].toma_acm == NVMEIBTC_DS_MODE_RW) && (!is_seg_active(rs[s  ])) &&
			  (rs[s^1].toma_acm == NVMEIBTC_DS_MODE_W ) && ( is_seg_active(rs[s^1])) &&
			  tomaSimulator_isSegRegistered(&server[1].simToma, rs[s^1].uuid, client->inst_id));
		BUG_ON(!rv);
		tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__SILENT);
		serverSimulator_re_connect(server);
		NVMeshSystem_serialize(sys);
		t = ___get_tail_topo_of_device(sys, volInd);
		rs = t->chunks[0].raid1s[0].segments;				// Verify client in {NVMEIBTC_DS_MODE_RW registered, NVMEIBTC_DS_MODE_W properly}. IO enabled.
		rv = ((t->newer == NULL) && (nvmeibc_topo_is_io_ok(t)) &&
			  (rs[s  ].toma_acm == NVMEIBTC_DS_MODE_RW  ) && ( is_seg_active(rs[s  ])) &&
			  (rs[s^1].toma_acm == NVMEIBTC_DS_MODE_DEAD) && (!is_seg_active(rs[s^1])) &&
			  (!tomaSimulator_isSegRegistered(&server[1].simToma, rs[s^1].uuid, client->inst_id)  ));
		BUG_ON(!rv);
		tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW  , NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
	}

	// -------------------- Verify we are back to normal topology
	{
		struct disk_range* curSeg = &sys->mdb.vols[volInd].segs[0];
		magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
		rv = osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
		clientSimulator_wait_for_all_bio_ops(client);
		for (i=0; i<4; i++, curSeg++){ 							// Verify that magic number was written to both mirrors
			dst = physSegStartPtr_off(curSeg, ((i<2)?__to4K(startBlock):0));
			__unitest_verify_blocks_pattern(dst,  __from4K(1), magic_pattern, true);
		}
		NVMeshSystem_wipe_all_dirty_bits(sys);
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}

	// -------------------- Test VOLUME_MISMATCH messages: clntVol > Tomavol (dont test clntVol < TomaVol, it is good path of segment relocation)
	if (1) {
		struct volumeDescriptor   *vol = &sys->mdb.vols[volInd];
		struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];			// Toma Configuration of the current volume
		int	seg_ind;
		cfv->version -= 10;											// Client's volume becomes outdated by 10 versions
		tomaSimulator_unreg_raid1(r1uuid(r1), 1);				// Client will get volume mismatch and io will be disabled
		cfv->version += 10;											// Revert everything back (as if Toma got the latest configuration and sent registrables)
		for (seg_ind=0; seg_ind < vol->nSegments; seg_ind++) {
			node_ind = vol->segs[seg_ind].node_id;
			tomaSimulator_send_registrables(&sys->servers[node_ind].simToma);	// Toma's that sent REGISTRABLE msgs will now trigger registration.
			_NT(trace_bunitest_unitest_Raid1_SwitchTopo, "Wait for registraion of Toma @NODE_IND...", node_ind);
		}
		tomaSimulator_waitProtoEnd(NULL);							// wait for REGISTRABLE msg
		tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}
	sim_kfree(mem);
	_NI_dmesg(trace_1_bunitest_unitest_Raid1_SwitchTopo, "*************** end");
	return rv;
}

/* Test New mechanism of decentralized unregister from Toma's. */
#define __rand_lock(l)   ((u32)((++(l)&(0xF000FFFFULL)) | ((rand()%0xFF) << 20)))	// Generate stale lock not In clients cache/ increase the lower bits to guarantee uniqueness and randomize higher bits so LRU will not be sorted

#define addr_of_stale(curSeg) 												\
({																			\
	__auto_type ssd = &sys->servers[(curSeg)->node_id].ramDisk;				\
	(&ssd->locks[COMMITTED_ADDR_AS(ssd, (curSeg)->dlba_start, 4KB, LOCK)]);	\
})

static u32 __dual_lock_stale_locks(const char* action, int scenario, u32 *curLock, u32 *dualLock, u64 broken_lock) {		// # types of different lock combinations
	if (action[0] == 's') {			// Set
		switch (scenario) {
			case 0: /*{S1 , S1 }*/ *curLock  =                             *dualLock  = __rand_lock(broken_lock);   break;
			case 1: /*{S1 , S2 }*/ *curLock  = __rand_lock(broken_lock);   *dualLock  = __rand_lock(broken_lock);   break;
			case 2: /*{S1 , SSP}*/ *curLock  = R1_STALE_SPECIAL_LOCK_VAL;  *dualLock  = __rand_lock(broken_lock);   break;
			case 3: /*{SSP, S1 }*/ *curLock  = __rand_lock(broken_lock);   *dualLock  = R1_STALE_SPECIAL_LOCK_VAL;  break;
		};
	} else {						// Verify
		BUG_ON((*curLock  != R1_STALE_SPECIAL_LOCK_VAL) || (*dualLock != R1_STALE_SPECIAL_LOCK_VAL));
		*curLock = *dualLock = 0;
	}
	return broken_lock;
}

TEST_FUNC int unitest_DecentralizedUnregister(struct NVMeshSystem *sys){
	int rv = 0, i, lenBlocks = 1, volInd = 3, ioctl_num, expected_num_of_executed_ioctls = 0;
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8        *mem 		 = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	struct disk_range *curSeg = &sys->mdb.vols[volInd].segs[0];

	const union nvmeib_lock_blkset_entry broken_lock_basic_id        ={ .lock_id = { .bits = { .idx_in_praid = 9, .lock_id = 0x13, .is_stale = 1 }}};
	const union nvmeib_lock_blkset_entry nvmeib_stale_unlocked_daniel={ .lock_id = { .bits = { .idx_in_praid = 7, .lock_id =  0x0, .is_stale = 1 }}};// Not a real lock
 	u32 broken_lock = broken_lock_basic_id.all;
	u32 *curLock = addr_of_stale(curSeg);
	enum nvmeib_block_io_op op;
	const int lockset_prob_backup = nvmeibc_sync_full_lockset_probability_factor;
	ioctl_num = clientSimulator_get_num_executed_ioctls(client);

	nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_MIN;			// force no partial syncs
	// Verify client can execute IO regardless of stale lock
	*curLock = broken_lock;
	for (u32 op__ = NVMEIB_BLOCK_IO_OP_READ; op__ <= NVMEIB_BLOCK_IO_OP_DISCARD; op__++) {
		op = (enum nvmeib_block_io_op)op__;
		rv = osSimulator_gen_generic_op(op, &client->OS, volInd, 0, lenBlocks, mem); REPORT_ERROR(rv);
		clientSimulator_wait_for_all_bio_ops(client);
		BUG_ON(*curLock != nvmeib_stale_special_raid1.all);				// client must not release the lock but leave it stale, unless it did full sync of the lock

		// Same as above but with zero stale lock
		*curLock = nvmeib_stale_unlocked_daniel.all;
		rv = osSimulator_gen_generic_op(op, &client->OS, volInd, 0, lenBlocks, mem); REPORT_ERROR(rv);
		clientSimulator_wait_for_all_bio_ops(client);
		BUG_ON(*curLock != nvmeib_stale_special_raid1.all);				// client must not release the lock but leave it stale, unless it did full sync of the lock
		*curLock = __rand_lock(broken_lock);
	}
	if (1) {
		struct tTopoOfVolume *cfv = &sys->tcf.vols[volInd];
		struct tTopoOfPraid* r1 = tTopoOfVolume_getRaid1(cfv, 0);
		enum NVMEIBTC_DS_MODE seg_stats[N_MAX_RAID_SLICE_LEN]; // = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
		u32 *dualLock = addr_of_stale(curSeg+1);
		int locks_scenario;
		array_fill(seg_stats, NVMEIBTC_DS_MODE_RW);
		if (1) {     					// Test purging the locks
			const struct nvmeibc_topology * t = ___get_tail_topo_of_device(sys, volInd);
			const struct stale_lock_resolver_cache_t *cache = &t->chunks[0].raid1s[0].hdr->slr.cache;
			const struct toma_recovery_args rcvr_args = {.type = NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE, .cmd=NVMEIBT_CLIENT_MSG_TC_LOCKID_CACHE_PURGE, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA};
			BUG_ON(cache->size == 0);
			BUG_ON(tomaSimulator_recoverThing(r1, &sys->mdb.vols[volInd].segs[0], rcvr_args) < 0);	// Test Direct msg API
			BUG_ON(cache->size != 0);
		}
		if (1) {													// Test stale lock in dual lock
			seg_stats[0] = NVMEIBTC_DS_MODE_W_NO_DIRTY;
			tomaSimulator_switchTopoEC(r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);  /* {W	, RW} - lock both */
			for (locks_scenario = 0; locks_scenario < 4; locks_scenario++) {
				for (u32 op__ = NVMEIB_BLOCK_IO_OP_READ; op__ <= NVMEIB_BLOCK_IO_OP_DISCARD; op__++) {
					op = (enum nvmeib_block_io_op)op__;
					if (op == NVMEIB_BLOCK_IO_OP_READ)
						continue; 						// Backwards compatibility, sync-ops are not supported for READ in dual locks mode, without forcing the read to take lock. This is not a valid topology that was removed after version 1.0.5
					broken_lock = __dual_lock_stale_locks("set", locks_scenario, curLock, dualLock, broken_lock);
					rv = osSimulator_gen_generic_op(op, &client->OS, volInd, 0, lenBlocks, mem); REPORT_ERROR(rv);
					clientSimulator_wait_for_all_bio_ops(client);
					__dual_lock_stale_locks("verify", locks_scenario, curLock, dualLock, broken_lock);
				}
			}
		}
		if (1) {													// Test  Read operation with force take locks
			tTopoOfPraid_force_lock_on_read(r1, true);
			seg_stats[0] = NVMEIBTC_DS_MODE_RW;
			tomaSimulator_switchTopoEC(r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);  /* {RW	, RW} - normal, but force taking locks on read */
			*curLock = __rand_lock(broken_lock);
			rv = osSimulator_readArr(&client->OS, volInd, 0, lenBlocks, mem); REPORT_ERROR(rv);
			clientSimulator_wait_for_all_bio_ops(client);
			tTopoOfPraid_force_lock_on_read(r1, false);
			*curLock = 0;
		}
		if (1) {													// Test degraded mode
			for (i = 0; i < 2; i++) {								// Kill first or second segment
				struct disk_range *liveSeg = &curSeg[i];
				tomaSimulator_unreg_raid1(r1uuid(r1), i^1);    /* {RW	, DEAD} - Degraded mode */
				curLock = addr_of_stale(curSeg+i);
				*curLock = __rand_lock(broken_lock);
				rv = osSimulator_writeArr(&client->OS, volInd, 0, lenBlocks, mem); REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				BUG_ON(*curLock != 0);								// Write did trigger implicit stale-to-dirty-sync
				*curLock = 0;
				ramDiskSimulator_wipe_dirty_bits(&sys->servers[liveSeg->node_id].ramDisk, 0x0);
				tomaSimulator_switchTopoEC(r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);  /* {RW	, RW} - normal */
			}
			for (i = 0; i < 1; i++) {								// Switch topology while client is waiting for replies
				struct disk_range *liveSeg = &curSeg[i];
				struct tomaSimulator *deadToma = &sys->servers[curSeg[i^1].node_id].simToma;
				tomaSimulator_enable_msg_q_to_client(deadToma);
				curLock = addr_of_stale(curSeg+i);
				*curLock = (++broken_lock);
				rv = osSimulator_writeArr(&client->OS, volInd, 0, lenBlocks, mem); REPORT_ERROR(rv); // IO is stuck coz 1 toma does not return an answer
				tomaSimulator_unreg_raid1(r1uuid(r1), i^1);    		/* {RW	, DEAD} - Degraded mode */
				clientSimulator_wait_for_all_bio_ops(client);		// Toma which did not return answer about stale lock is dead and not needed anymore
				BUG_ON(*curLock != 0);								// Write did trigger implicit stale-to-dirty-sync
				*curLock = 0;
				ramDiskSimulator_wipe_dirty_bits(&sys->servers[liveSeg->node_id].ramDisk, 0x0);
				tomaSimulator_disable_msg_q_to_client(deadToma);	// Typically has 3 messages: 1. Stale lock cleaned, 2. Unregister ack, 3. Nack
				tomaSimulator_switchTopoEC(r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);  /* {RW	, RW} - normal */
			}
			curLock = addr_of_stale(curSeg);
		}
	}
	if (1) { // Test clean caches -- ioctl is not counted in qa_cmd_ignore_str_cmd
		char cmd[128];
		//clientSimulator_print_proc_files_of_vol(client, true, volInd);
		sprintf(cmd, "#%s|STLR_print %d,%d", client->devs[volInd]->name, 0 /*chunk*/, 0 /*raid*/);
		clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls++;
		sprintf(cmd, "#%s|STLR_clear=%d,%d", client->devs[volInd]->name, 0 /*chunk*/, 0 /*raid*/);
		clientSimulator_send_to_cli(client, cmd); expected_num_of_executed_ioctls++;
	}
	nvmeibc_sync_full_lockset_probability_factor = lockset_prob_backup;
	BUG_ON(!NVMeshSystem_is_stable(sys));
	sim_kfree(mem);
	BUG_ON(ioctl_num + expected_num_of_executed_ioctls != clientSimulator_get_num_executed_ioctls(client));
	return rv;
}

/*****************************************************************************/
static const char *get_async_io_thread_name(enum nvmeib_block_io_op	op) {
	switch (op) {
	case NVMEIB_BLOCK_IO_OP_WRITE:		return "ut:async write";
	case NVMEIB_BLOCK_IO_OP_READ:		return "ut:async read";
	case NVMEIB_BLOCK_IO_OP_DISCARD:	return "ut:async trim";
	default:			BUG();
	}
	return NULL;
}

int __thread_gen_async_io_detach_safe(void* param);
// helper to create a thread that fires IO's.
static void create_async_io_thread(t_async_test_params *prm, enum nvmeib_block_io_op op, int nlbas) {
	prm->op = op;
	prm->nlbas = nlbas;
	prm->kthread = kthread_run(__thread_gen_async_io_detach_safe, prm, get_async_io_thread_name(op));
	BUG_ON(prm->kthread == NULL);
}

/* Issue IO when it cannot be executed and test scheduled resubmittion of continous writes {[addr1..adrr2],[addr2+1..addr3],[addr3+1..addr4]} */
int __thread_async_continous_write(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct clientSimulator *client = &p->sys->clients[0];
	const int lenBlocks = p->nlbas, volInd=0;
	const u32 size 		= client->devs[volInd]->size;			// Size of the volume
	const bool do_trims_instead_writes = (p->disk_id != 0);
	int nWritesCycle = size/lenBlocks;							// Amount of writes to cover the entire volume
	u8 *mem = p->write_buf;
	int round, startBlock = 0, i, start_offset = 0;
	if (lenBlocks==1){
		nWritesCycle = LOCKSET_SLICES;							// Write only 1 lockset. Each 32 consecutive IO's write 32 consecutive blocks of this lockset
	}else {
		start_offset = 3;										// Write N lockset with fractional offset
	}
	startBlock = start_offset;									// You can move this line inside the 'round' loop to increase lock transferring ratio
	for (round=0; !kthread_should_stop(); round++) {
		for (i=0; i<nWritesCycle; i++) {
			if (!do_trims_instead_writes)
				BUG_ON(osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem));
			else
				BUG_ON(osSimulator_trim(    &client->OS, volInd, startBlock, lenBlocks     ));
			startBlock = (startBlock + lenBlocks) % size;
		}
		udelay(p->mu_delay);
	}
	p->n_cycles = round*nWritesCycle;
	return 0;
}

int unitest_resubmitIO_continousWrites(struct NVMeshSystem *sys){
	int i, v, rv = 0, n_total_ios = 0;
	const int blk_sizes[] 	= {1, LOCKSET_SLICES*2}, n_blk_sizes = ARRAY_SIZE(blk_sizes);
	const int memSize		= blk_sizes[n_blk_sizes-1]*NVMEIBC_SECTOR_SIZE;	// Total array in bytes
	u8        *mem			= sim_kmalloc(memSize, GFP_KERNEL);			// Array to write to disk

	memset(mem, (u8)('#'), memSize);

	// Issue many 1[block]  writes to the same lockset via a few threads. All will transfer locks one to another
	// Issue many 64[block] writes in between locksets via a few threads. All will transfer locks one to another
	for (i=0; i<n_blk_sizes; i++) {
		t_async_test_params p[4];
		const int n_threads = ARRAY_SIZE(p), per_len_blk = blk_sizes[i];
		n_total_ios = 0;
		for (v=0; v<n_threads; v++) {
			const bool do_trims_instead_writes = false; // Trims do not transfer locks. (v&0x1);			// Half threads do trims / half writes
			t_async_test_params_init(p[v], sys, do_trims_instead_writes, per_len_blk, 0);
			p[v].write_buf = mem;
			p[v].kthread = kthread_run(__thread_async_continous_write, &p[v], "ut:continuos write");
			BUG_ON(p[v].kthread == NULL);
		}
		msleep(100);												// Let the threads run together for 0.1[sec]
		for (v=0; v<n_threads; v++) kthread_stop(p[v].kthread);
		for (v=0; v<n_threads; v++) n_total_ios+= p[v].n_cycles;
		NVMeshSystem__detectStuckIOs(sys);
		_NI_dmesg(trace_bunitest_unitest_resubmitIO_continousWrites, "*************** Sent @N_TOTAL_IOS IO's of size @PER_LEN_BLK blocks", n_total_ios, per_len_blk);
	}
	BUG_ON(!NVMeshSystem_is_stable(sys));

	_NI_dmesg(trace_1_bunitest_unitest_resubmitIO_continousWrites, "*************** end");
	sim_kfree(mem);
	return rv;
}

/* Issue at once many IO's that fill the mas amount of IO's in air and start using per cpu IO's waiting lists */
int unitest_percpu_io_throttle_queues(struct NVMeshSystem *sys) {
	int v, rv = 0, n_total_ios = 0;
	const int write_len_blk = 1;						// Issue many 1[block]  writes to the same lockset via a few threads. All will transfer locks one to another
	const int memSize = write_len_blk << NVMEIBC_SECTOR_SHIFT;
	u8 *mem = sim_kmalloc(memSize, GFP_KERNEL);	// Array to read/write to disk
	unsigned int save_orig_val = max_ios_per_cpu;
	t_async_test_params p[2];
	const int n_threads = ARRAY_SIZE(p);
	max_ios_per_cpu = 1;											// Permit 1 IO's in air
	memset(mem, (u8)('#'), memSize);
	for (v=0; v<n_threads; v++) {
		t_async_test_params_init(p[v], sys, 0, write_len_blk, 0);
		p[v].mu_delay = 500*LOCKSET_SLICES*n_threads; // 2K locksets per second as sum of all threads
		p[v].write_buf = mem;
		p[v].kthread = kthread_run(__thread_async_continous_write, &p[v], "ut:percpu io queues");
		BUG_ON(p[v].kthread == NULL);
	}
	msleep(100); // Let the threads run together for 0.1[sec] - generate 200-300 IO's?
	for (v=0; v<n_threads; v++) kthread_stop(p[v].kthread);
	for (v=0; v<n_threads; v++) n_total_ios+= p[v].n_cycles;
	drain_workqueue(system_wq);
	NVMeshSystem__detectStuckIOs(sys);
	_NI_dmesg(trace_bunitest_unitest_percpu_io_throttle_queues, "*************** Sent @N_TOTAL_IOS IO's of size @PER_LEN_BLK blocks", n_total_ios, write_len_blk);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	max_ios_per_cpu = save_orig_val;

	BUG_ON(rv);
	sim_kfree(mem);
	return rv;
}

int __thread_async_attach_dettach(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct NVMeshSystem *sys = p->sys;
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int v, round;
	for (round=0; !kthread_should_stop(); round++) {
		send_command_to_all(sys, -1, volCmds_ForceDetach);		// Detach the entire system
		send_command_to_all(sys, -1, volCmds_New);			// Reattach the entire system

		// Detach all but Vol0 thus not freeing the disks (there is a volume which uses all of them)
		sys->mdb.vols[0].nextCmd = volCmds_Illegal;
		for (v=1; v<client->nBdevs; v++)
			sys->mdb.vols[v].nextCmd = volCmds_ForceDetach;
		send_command_predefined_to_all(sys, -1);
		send_command_to_all(sys, -1, volCmds_New);						// Reattach the entire system (unneeded update volume 0)
	}
	for (v=0; v<client->nBdevs; v++)
		sys->mdb.vols[v].nextCmd = volCmds_Illegal;
	p->n_cycles = round*(client->nBdevs*2-1);
	return 0;
}

static void __thread_async_pause_cont_run_ec_cleanup_recoveries(struct NVMeshSystem *sys) {
	struct tTopoOfPraid *r1 = tTopoOfVolume_getRaid1(&sys->tcf.vols[0], 0);
	struct disk_range *seg = &sys->mdb.vols[0].segs[0];
	const int n_segs = (int)seg->replicas;
	int i, recov_status;

	// Quiesce SERJIO-initiated JGC recoveries and prevent starting new ones for the cleanup duration
	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(sys, true);
	clientSimulator_wait_for_all_recoveries_done(&sys->clients[0]);
	tomaSimulator_waitProtoEnd(NULL);

	for (i = 0; i < n_segs; i++) {
		do {
			BUG_ON(tomaSimulator_recoverThingStatus(r1, seg + i, RCVR_STALE_REBUILD, &recov_status) < 0);
		} while (recov_status);

		do {
			BUG_ON(tomaSimulator_recoverThingStatus(r1, seg + i, RCVR_EC_JOUR_GC, &recov_status) < 0);
		} while (recov_status);
	}

	clientSimulator_wait_for_all_recoveries_done(&sys->clients[0]);
	tomaSimulator_waitProtoEnd(NULL);

	NVMeshSystem_set_ignore_jour_gc_launch_requests_from_serjio(sys, false);
}


int __thread_async_pause_cont(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	const bool should_rotate_disks = (p->disk_id<0);				// -1: [0..NVMESH_N_PHYS_DISKS_REGULAR_USE), <-2: [0..-p->disk_id), >0: Pause only this specific disk
	const int  n_used_disks = (p->disk_id < -1) ? (-p->disk_id) : NVMESH_N_PHYS_DISKS_REGULAR_USE;
	int round, cur_disk = p->disk_id, toma_ind;
	int sleep_time_msecs = (p->is_ec) ? 100 : 10;
	for (round=0; !kthread_should_stop(); round++) {
		if (should_rotate_disks)
			cur_disk = round % n_used_disks;
		tTopoOfNVMesh_incVer(&p->sys->tcf);						// All volumes are outdated.
		NVMeshSystem__invoke_pause_on_disk(p->sys, cur_disk);
		//msleep(1);
		tTopoOfNVMesh_incVer(&p->sys->tcf);						// All volumes are outdated.
		NVMeshSystem__invoke_cont_on_disk( p->sys, cur_disk, false);
		if (!p->is_ec && (round & 0x1)) {						// Randomly send unneeded switch topo
			for (int v=0; v<p->sys->tcf.nVolumes; v++) {
				struct switch_topo_dest	switch_topo_dst;
				// since we dont know if a switchTopo msg can be sent, we cannot ask to wait. so wait only if we sent a msg
				toma_ind = tomaSimulator_switchTopo_dummy(v, 0, SW_TOPO__NONE, &switch_topo_dst);
				if (toma_ind >= 0) {
					tomaSimulator_waitSwitchTopoAck(switch_topo_dst.disk_ind, switch_topo_dst.r1->s[switch_topo_dst.seg_ind].uuid);
				}
			}
		}

		// Resolve abandoned journal entries using stale and JGC recoveries, otherwise IOs can get stuck waiting for JAM
		if (p->is_ec)
			__thread_async_pause_cont_run_ec_cleanup_recoveries(p->sys);

		msleep(sleep_time_msecs);
	}
	p->n_cycles = round;
	return 0;
}

static struct nvmeibc_multi_completion n_suspends_waiting;
static void __on_suspend_finish_cb(void *susped_context_unused){
	(void)susped_context_unused;
	nvmeibc_multi_completion_done(&n_suspends_waiting);
}
int __thread_async_suspend_revive(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct clientSimulator *client = &p->sys->clients[0];
	int round, v, res;
	for (v=0; v<client->nBdevs; v++)
		p->sys->mdb.vols[v].nextCmd = volCmds_Update;
	for (round=0; !kthread_should_stop(); round++) {
		if ((0)&&(round%2==0)) {		/* Todo: 50% Do supsend/revive */
			init_completion(&n_suspends_waiting.done);
			atomic_set(&n_suspends_waiting.counter, client->nBdevs);
			for (v=0; v<client->nBdevs; v++){			  /* Suspend all volumes */
				res = nvmeibc_block_suspend(client->devs[v], client->devs[v], &__on_suspend_finish_cb);
				if (res<0)  __on_suspend_finish_cb(NULL); /* Suspend request failed */
			}
			nvmeibc_multi_completion_wait_for(&n_suspends_waiting);	/* Wait for all suspends to occur */
			for (v=0; v<client->nBdevs; v++){
				nvmeibc_block_revive(client->devs[v]);	  /* Revive all volumes */
			}
		} else {			/* 50% Do reboot */
			for (v=0; v<client->nBdevs; v++) {
				__unitest_volume_config_version_inc(&p->sys->mdb.vols[v], &p->sys->tcf.vols[v]);
				__unitest_volume_config_version_inc(&p->sys->mdb.vols[v], &p->sys->tcf.vols[v]);
			}
			send_command_predefined_to_all(p->sys, -1);
		}
	}
	NVMeshSystem_serialize(p->sys);			// Daniel: Not sure this is enough. Have to wait for IO enabled on all volumes
	for (v=0; v<client->nBdevs; v++)
		clientSimulator_wait_for_io_enabled_for_vol_non_idle(client, v);
	p->n_cycles = round*client->nBdevs;
	for (v=0; v<client->nBdevs; v++)
		p->sys->mdb.vols[v].nextCmd = volCmds_Illegal;
	return 0;
}

int __thread_async_toma_messages(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	const int mvols[] = {0,3};		// Iterate only on mirrored volumes
	int v, c, r, round;
	for (round=0; !kthread_should_stop(); ) {
		struct tTopoOfNVMesh *cf = &p->sys->tcf;
		for (v=0; v<2 /*cf->nVolumes*/; v++){
			struct tTopoOfVolume *vol = &cf->vols[mvols[v]];
			for (c=0; c<vol->nChunks; c++) {
				struct tTopoOfRaid0Chunk *chunk = &vol->chunks[c];
				for (r=0; r<chunk->stripeWidth; r++) {
					struct tTopoOfPraid *r1 = &chunk->raids[r];
					round += tomaSimulator_unreg_raid1(r1uuid(r1), 1);
					round += tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK) ? 1 : 0;
	}	}	}	}
	p->n_cycles = round;
	return 0;
}

#define ARE_LOCKS_ZIGZAGING ((1<<LOCKSET_SHIFT) == LOCKSET_SLICES)
int __thread_gen_async_io_detach_safe(void* param) {							// Async IO, while blockdevice can dessapear
	t_async_test_params *p = (t_async_test_params*)param;
	struct clientSimulator *client = &p->sys->clients[0];
	struct block_device	*bdev;
	const int n_devs = client->nBdevs;
	int		open_rv[MAX_NORMAL_VOLUMES_IN_NVMESH];										// 0 means the volume is open, negative means its closed
	u64 	vol_size[MAX_NORMAL_VOLUMES_IN_NVMESH];
	int min_len_for_alocks_merge[4];
	int v, rv = 0, round;
	u64 start_block, trim_len_blocks, seed, num_issued_ios = 0;
	u8 *mem;
	BUG_ON(n_devs > MAX_NORMAL_VOLUMES_IN_NVMESH);
	BUG_ON(n_devs > (int)ARRAY_SIZE(min_len_for_alocks_merge));
	for (v=0; v<n_devs; v++) {
		open_rv[v] = -ENOENT;													// set all volumes state so they invoke open()
		vol_size[v] = 0;
		if (p->op == NVMEIB_BLOCK_IO_OP_DISCARD) {
			const struct disk_range* curSeg = &p->sys->mdb.vols[v].segs[0];
			const int stripe_length = (curSeg->stripe_width * curSeg->stripe_size);
			min_len_for_alocks_merge[v] = stripe_length * (1+ARE_LOCKS_ZIGZAGING) + 1;
		}
	}
	for (round=0; !kthread_should_stop(); round++) {
		for (v=0; v<n_devs; v++) {
			if (unlikely(open_rv[v] < 0)) {										// If block device is not opened, open it (Will not open twice)
				open_rv[v] = osSimulator_diskOpenIdx(&client->OS, v, false, FMODE_WRITE, &bdev);
				if (open_rv[v] != 0)
					continue;													// Volume is detaching, and we could not open it. Skip it to give CPU time to other volumes
				if (unlikely(vol_size[v] == 0)) {								// Cache size of each block device, bcz they are not available when volume is detached
					vol_size[v] = clientSimulator_sizeof_bdev(client, v);       // get each volume's size, bcz the size is zeroed async while detach executes.
					if (vol_size[v] == 0)
						goto _close;											// Rare race condition: We opened the device but it is unsafely detaching and size does not exists. Skip issuing IO's because they will fail anyways
				}
			}

			seed = (p->flags & BUNITEST_ASYNC_TEST_RAND_IO_PATTERN) ? rand() : round;
			num_issued_ios++;
			switch (p->op) {													// Weve got a handle (open) to the device - our reference prevents it from being deleted. Note: call to IO execution will either fail or invoke bio_endio()
			case NVMEIB_BLOCK_IO_OP_READ:
				start_block = seed % (vol_size[v] - p->nlbas + 1);
				mem = sim_kmalloc(p->nlbas << NVMEIBC_SECTOR_SHIFT, GFP_KERNEL);
				NVMeshSystem_async_io_gate_wait(p->sys);
				rv = osSimulator_readArrFree(&client->OS, v, start_block, p->nlbas, mem);
				break;
			case NVMEIB_BLOCK_IO_OP_WRITE:
				start_block = seed % (vol_size[v] - p->nlbas + 1);
				mem = sim_kmalloc(p->nlbas << NVMEIBC_SECTOR_SHIFT, GFP_KERNEL);
				__unitest_fill_blocks_unique_pattern(mem, p->nlbas);	// Set a unique pattern for each write.
				NVMeshSystem_async_io_gate_wait(p->sys);
				rv = osSimulator_writeArrFree(&client->OS, v, start_block, p->nlbas, mem);
				break;
			case NVMEIB_BLOCK_IO_OP_DISCARD: // a.k.a TRIM
				// decide on the trim size distribution
				start_block = seed % vol_size[v];
				if ((round & 0x7) == 0) { /* Once every 8 rounds make long trim */	trim_len_blocks = min_len_for_alocks_merge[v];
				} else if (round&0x1)   { /* Half of cases regualr trim */			trim_len_blocks = p->nlbas;
				} else                  { /* 3/8 of cases very short trim */		trim_len_blocks = 7; }
				if (start_block + trim_len_blocks > vol_size[v])
					start_block = (seed) % (vol_size[v] - trim_len_blocks); /* Move start block backwards to pseudo random location */
				NVMeshSystem_async_io_gate_wait(p->sys);
				rv = osSimulator_trim(&client->OS, v, start_block, trim_len_blocks);
				break;
			default:
				BUG();
			}
			BUG_ON(rv != 0);													// nvmeibc_block implementation: All errors are returned via bio_endio(), If falls here - probably a problem with a unitests
			rv = osSimulator_rv_of_last_io_get(&client->OS, v);					// read the 'rv' of last async IO
			if (rv == 0) 														// Note: rv of the last async completion of IO (returned via bio_endio()
				continue;
			if (p->flags & BUNITEST_ASYNC_TEST_FLAG_ALLOW_IO_ERROR) {
				BUG_ON((rv != 0) && (rv != -ENOMEM) && (rv != -EIO) && (rv != -EPIPE));
			} else {
				BUG_ON(rv != 0);
			}
	_close:
			osSimulator_diskCloseIdx(&client->OS, v);							// close the device to allow it to be freed, in case its pending detach.
			open_rv[v] = -ENOENT;
		} // for (v=0; v<n_devs; v++)
		udelay(p->mu_delay);
	}
	// when we're instructed to terminate, we break out, leaving some volumes still open
	for (v=0; v<n_devs; v++)
		if (open_rv[v] == 0)
			osSimulator_diskCloseIdx(&client->OS, v);
	p->n_cycles = num_issued_ios;		// <= round*n_devs
	return 0;
}

int __thread_async_trim_vol0_interlocking(void* param){
	t_async_test_params *p = (t_async_test_params*)param;
	struct clientSimulator *client = &p->sys->clients[0];
	int round;
	const struct disk_range* curSeg = &p->sys->mdb.vols[0].segs[0];
	const int stripe_length = (curSeg->stripe_width*curSeg->stripe_size);
	const int lenBlocks = (4*stripe_length) + 1;

	for (round=0; !kthread_should_stop(); round++) {
		int start_block = (4*stripe_length);
		BUG_ON(osSimulator_trim(&client->OS, 0, start_block, lenBlocks));
		udelay(p->mu_delay);
	}
	p->n_cycles = round;
	return 0;
}

/* Put Vol0 in different lock modes. */
void __set_different_lock_modes_of_vol(struct NVMeshSystem *sys, int volInd, const char *action) {
	struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];
	struct tTopoOfPraid* r1;
	enum NVMEIBTC_DS_MODE ss[3] = {NVMEIBTC_DS_MODE_W, NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W_NO_DIRTY};
	switch (action[0]) {
	case 'D' /* Dual locks */:
		/* {W	, RW} - A-lock recovered*/ r1 = tTopoOfVolume_getRaid1(cfv, 0); tomaSimulator_switchTopo(r1uuid(r1), ss[0]	, ss[1], SW_TOPO__WAIT_ACK);
		/* {W	, RW} - lock both*/        r1 = tTopoOfVolume_getRaid1(cfv, 1); tomaSimulator_switchTopo(r1uuid(r1), ss[2]	, ss[1], SW_TOPO__WAIT_ACK);
		/* {RW	,  W} - lock both*/        r1 = tTopoOfVolume_getRaid1(cfv, 2); tomaSimulator_switchTopo(r1uuid(r1), ss[1]	, ss[2], SW_TOPO__WAIT_ACK);
		break;
	case 'U' /* Unsafe read-locks */:
		/* {W	, RW} -lock both,unsafe */ r1 = tTopoOfVolume_getRaid1(cfv, 0); tTopoOfPraid_force_lock_on_read(r1, true); tomaSimulator_switchTopo( r1uuid(r1), ss[2]	, ss[1], SW_TOPO__WAIT_ACK);
		/* {RW	, RW} -normal,   unsafe */ r1 = tTopoOfVolume_getRaid1(cfv, 1); tTopoOfPraid_force_lock_on_read(r1, true); tomaSimulator_switchTopo( r1uuid(r1), ss[1]	, ss[1], SW_TOPO__WAIT_ACK);
		/* {DEAD, RW} -degraded mode    */ r1 = tTopoOfVolume_getRaid1(cfv, 2);                                            tomaSimulator_unreg_raid1(r1uuid(r1), 0);
		break;
	case 'N' /* All moved back to {RW	, RW} - normal */:
		r1 = tTopoOfVolume_getRaid1(cfv, 0); tTopoOfPraid_force_lock_on_read(r1, false);  tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		r1 = tTopoOfVolume_getRaid1(cfv, 1); tTopoOfPraid_force_lock_on_read(r1, false);  tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		r1 = tTopoOfVolume_getRaid1(cfv, 2); tTopoOfPraid_force_lock_on_read(r1, false);  tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		break;
	default: BUG();
	}
}

int __cancel_and_drain_resubmitted_io(struct NVMeshSystem *sys) {		// Remove all IO from resubmit threads of all volumes by cancelling it (setting time out to zero)
	unsigned long prev[MAX_NORMAL_VOLUMES_IN_NVMESH];
	struct clientSimulator *client = &sys->clients[0];
	int v, num_canceled_ios = osSimulator_allert_pending_ios(&client->OS, 0);
	NFIN;
	for (v=0; v<client->nBdevs; v++){
		if (client->devs[v]) {
			prev[v] = client->devs[v]->max_retry_jiffies;
			_NT(trace_bunitest_cancel_and_drain_resubmitted_io, "dev=@DEV_IDX max_retry_secs=0", v);
			clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs 0", client->devs[v]->name);
		}
	}
	NVMeshSystem__detectStuckIOs(sys);
	clientSimulator_wait_for_all_sync_ops(client);						// Let sync operations terminate
	tomaSimulator_waitProtoEnd(NULL);									// Todo: Remove, me
	// wait for locks to be released (after bio's are completed)
	clientSimulator_wait_for_all_bio_ops(client);
	for (v=0; v<client->nBdevs; v++){
		if (client->devs[v]) {
			clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", client->devs[v]->name, (int)(prev[v] / HZ));
		}
	}
	tomaSimulator_waitProtoEnd(NULL);									// Abandoned locks, can trigger UNREG/REG toma messages
	for (v=0; v<client->nBdevs; v++)
		if (client->devs[v])											// Wait for all IO's to be enabled once again
			clientSimulator_wait_for_io_enabled_for_vol_non_idle(client, v);
	NFOUT;
	return num_canceled_ios;
}

/* Unitest which runs full async IO Read/Write/Trim during pause/conts. Does not include toma mesages and topology changes */
#define unitest_async_pause_cont_during_io_regu_lock_mode(sys) 			__unitest_async_pause_cont_during_io(sys, NULL, false)
#define unitest_async_pause_cont_during_io_dual_lock_mode(sys) 			__unitest_async_pause_cont_during_io(sys, "Dual locks" , false)
#define unitest_async_pause_cont_during_io_unsafe_lock_mode(sys)		__unitest_async_pause_cont_during_io(sys, "Unsafe" , false)
#define unitest_async_double_pause_cont_during_io_regu_lock_mode(sys) 	__unitest_async_pause_cont_during_io(sys, NULL, true)
int __unitest_async_pause_cont_during_io(struct NVMeshSystem *sys, const char* lock_modes, bool double_pause){
	t_async_test_params p[5];
	const int write_len_blk = 65, read_len_blk = 3, disk_id = -1;				// Write/Trim 65 blocks, read 3 blocks, rotate pause disks
	const int n_threads = ARRAY_SIZE(p);
	struct nvmeibc_disk_hooks disk_hooks = { .args.trerr = {true, true, true, 0, 0, (rand() % INJECT_TRANSPORT_ERROR_CYCLE_SIZE), true, 0, 0}, .inject_transport_error = inject_transport_error };
	int v, rv, n_total_ios = 0, n_total_pauses = 0, num_canceled_ios = 0, nBdevs = sys->clients[0].nBdevs;
	for (v=0; v<n_threads; v++)
		t_async_test_params_init(p[v], sys, disk_id, -1, BUNITEST_ASYNC_TEST_FLAG_ALLOW_IO_ERROR);
	if (lock_modes){
		__set_different_lock_modes_of_vol(sys, 0, lock_modes);
	}
	NVMeshSystem_di_tracking_enable(sys);
	NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, &disk_hooks);
	create_async_io_thread(&p[0], NVMEIB_BLOCK_IO_OP_WRITE, write_len_blk);
	create_async_io_thread(&p[1], NVMEIB_BLOCK_IO_OP_DISCARD, write_len_blk);
	create_async_io_thread(&p[2], NVMEIB_BLOCK_IO_OP_READ, read_len_blk);
	if (double_pause)
		p[3].disk_id = 2;
	p[3].kthread = kthread_run(__thread_async_pause_cont, &p[3], "ut:async pause");		BUG_ON(p[3].kthread == NULL);
	if (double_pause) {
		p[4].disk_id = 4;
		p[4].kthread = kthread_run(__thread_async_pause_cont, &p[4], "ut:async double pause");	BUG_ON(p[4].kthread == NULL);
	}

	msleep(1000);												// Let the threads run together
	for (v=0; v<n_threads;   v++) if (p[v].kthread) kthread_stop(p[v].kthread);
	for (v=0; v<n_threads-2; v++) n_total_ios    += p[v].n_cycles;
	for (   ; v<n_threads;   v++) n_total_pauses += p[v].n_cycles;
	num_canceled_ios = __cancel_and_drain_resubmitted_io(sys);	// PAUSE/CONT thread has finished, it is a waste of time to resbumit good path remaining IO.
	if (lock_modes){
		sys->clients[0].nBdevs = nBdevs;
		__set_different_lock_modes_of_vol(sys, 0, "Normal");
	}

	tomaSimulator_waitProtoEnd(NULL);
	NVMeshSystem_di_tracking_disable(sys);
	NVMeshSystem_wipe_all_stale_locks(sys);
	NVMeshSystem_wipe_all_dirty_bits(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
	rv = 0;

	unitest_print("*************** Pauses %d, (Sent=%d, Done=%d) IO's, %s\n", n_total_pauses, n_total_ios, n_total_ios - num_canceled_ios, (lock_modes?lock_modes:""));
	return rv;
}

TEST_FUNC int unitest_async_trim_interlocking_vol0(struct NVMeshSystem *sys){
	t_async_test_params p[4];
	const int n_threads = ARRAY_SIZE(p);
	int v, rv, n_total_ios = 0;
	for (v=0; v<n_threads; v++)
		t_async_test_params_init(p[v], sys, 0, -1, 0);
	for (v=0; v<n_threads; v++){
		p[v].kthread = kthread_run(__thread_async_trim_vol0_interlocking, &p[v], "ut:trim interlocked");	BUG_ON(p[v].kthread == NULL);
	}
	msleep(100);												// Let the threads run together
	for (v=0; v<n_threads; v++)
		if (p[v].kthread)
			kthread_stop(p[v].kthread);
	for (v=0; v<n_threads; v++) n_total_ios+= p[v].n_cycles;
	NVMeshSystem__detectStuckIOs(sys);
	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);			// Let sync operations terminate
	tomaSimulator_waitProtoEnd(NULL);									// Abandoned locks, can trigger toma messages
	NVMeshSystem_wipe_all_stale_locks(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	rv = 0;

	BUG_ON(rv);
	unitest_print("*************** %s Sent %d IO's\n", __FUNCTION__, n_total_ios);
	return rv;
}

TEST_FUNC int unitest_async_suspend_revive_during_io(struct NVMeshSystem *sys){
	t_async_test_params p[4];
	const int write_len_blk = 65, read_len_blk = 3;
	const int n_threads = ARRAY_SIZE(p);
	int v, test, rv, n_total_ios = 0, n_total_suspends = 0, num_canceled_ios = 0;
	struct clientSimulator *client = &sys->clients[0];
	// ------------------------------------ Test Suspend/Revive on good topology + IO
	for (v=0; v<n_threads; v++)
		t_async_test_params_init(p[v], sys, -1, -1, BUNITEST_ASYNC_TEST_FLAG_ALLOW_IO_ERROR);	// TODO(Daniel): why do we get IO failures in this test

	NVMeshSystem_di_tracking_enable(sys);

	create_async_io_thread(&p[0], NVMEIB_BLOCK_IO_OP_WRITE, write_len_blk);
	create_async_io_thread(&p[1], NVMEIB_BLOCK_IO_OP_DISCARD, write_len_blk);
	create_async_io_thread(&p[2], NVMEIB_BLOCK_IO_OP_READ, read_len_blk);
	p[3].kthread = kthread_run(__thread_async_suspend_revive, 	&p[3], "ut:async suspend/revive");	BUG_ON(p[3].kthread == NULL);
	msleep(100);												// Let the threads run together
	for (v=0; v<n_threads; v++)
		kthread_stop(p[v].kthread);
	for (v=0; v<n_threads-1; v++) n_total_ios+= p[v].n_cycles;
	n_total_suspends = p[3].n_cycles;
	num_canceled_ios = __cancel_and_drain_resubmitted_io(sys);	// PAUSE/CONT thread has finished, it is a waste of time to resbumit good path remaining IO.
	BUG_ON(!NVMeshSystem_is_stable(sys));

	// ------------------------------------ Test Suspend/Revive when topology is erronous
	for (test=0; test<2; test++) {
		NVMeshSystem__invoke_pause_on_disk(sys, 1);
		init_completion(&n_suspends_waiting.done);
		atomic_set(&n_suspends_waiting.counter, client->nBdevs);
		for (v=0; v<client->nBdevs; v++){			  /* Suspend all volumes */
			int res = nvmeibc_block_suspend(client->devs[v], client->devs[v], &__on_suspend_finish_cb);
			if (res<0)  __on_suspend_finish_cb(NULL); /* Suspend request failed */
		}
		if (test==0)
			NVMeshSystem__invoke_cont_on_disk(sys, 1, false);/* Cont during suspended state test */
		nvmeibc_multi_completion_wait_for(&n_suspends_waiting);	/* Wait for all suspends to occur */
		for (v=0; v<client->nBdevs; v++)
			nvmeibc_block_revive(client->devs[v]);	  /* Revive all volumes */
		if (test==1)
			NVMeshSystem__invoke_cont_on_disk(sys, 1, false);/* Full cycle of suspend+revive during erroneous topology */
		NVMeshSystem_serialize(sys);
	}
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_di_tracking_disable(sys);
	rv = 0;

	unitest_print("*************** Suspends %d, (Sent=%d, Done=%d) IO's\n", n_total_suspends, n_total_ios, n_total_ios - num_canceled_ios);
	return rv;
}

// Still has bugs in the testing environment so this test fails sometimes. Cont cannot arrive during dettach or attach since it on the same main work queue, while we test with 2 threads.
TEST_FUNC int unitest_async_pause_cont_during_attach_dettach(bunitest_s* B){
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	t_async_test_params p[2];
	const int disk_id = -1;	// rotate pause disks
	const int n_threads = ARRAY_SIZE(p);
	int v, _try, n_total_pauses = 0, n_total_attaches = 0;
	bunitest_tic(B);
	for (_try = 0; _try<3; _try++) {
		for (v = 0; v < n_threads; v++)
			t_async_test_params_init(p[v], sys, disk_id, -1, 0);
		tomaSimulator_protoBugs(true, false);								// Todo: Daniel, unable to force client behave well (Cont issues Register, while volume detach closes the topology and calls unsubscribe for registered segment)
		p[0].kthread = kthread_run(__thread_async_attach_dettach, 	&p[0], "ut:async attach/detach");	BUG_ON(p[0].kthread == NULL);
		p[1].kthread = kthread_run(__thread_async_pause_cont, 		&p[1], "ut:async pause/cont");		BUG_ON(p[1].kthread == NULL);
		msleep(300);												// Let the threads run together
		for (v=0; v<n_threads; v++)
			kthread_stop(p[v].kthread);
		n_total_attaches += p[0].n_cycles;
		n_total_pauses   += p[1].n_cycles;
		tomaSimulator_protoBugs(false, false);
		osSimulator_allert_pending_ios(&client->OS, 1);			// IO should terminate during at most 10[mSec] because sync operation cleaned the stale lock
		BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	}

	_NI_dmesg(trace_bunitest_unitest_async_pause_cont_during_attach_dettach, "*************** end Pauses/Conts=@CONTS, Attach/Detach=@DETACH, @BUNITEST_TOC[mSec]", n_total_pauses, n_total_attaches, bunitest_toc(B));
	return 0;
}

TEST_FUNC int unitest_async_attach_dettach_during_toma_messages(bunitest_s* B, bool test1){
	struct NVMeshSystem *sys = B->sys;
	t_async_test_params p[2];
	const int n_threads = ARRAY_SIZE(p);
	int i, v, rv = 0, n_total_messages = 0, n_total_attaches = 0;
	bunitest_tic(B);
	if (test1) {
		for (v = 0; v < n_threads; v++)
			t_async_test_params_init(p[v], sys, 0, -1, 0);
		tomaSimulator_protoBugs(true, true);	  // Must remove simulator guards because client unsibscribes so fast that toma did not have time to receive unregister message, but the message was legaly sent
		p[0].kthread = kthread_run(__thread_async_toma_messages , &p[0], "ut:async toma msgs");		BUG_ON(p[0].kthread == NULL);		// Order of threads is important. We first stop the toma messages and then the attach, or else we risk terminating in attached volume in degraded mode
		p[1].kthread = kthread_run(__thread_async_attach_dettach, &p[1], "ut:async attach/detach");	BUG_ON(p[1].kthread == NULL);
		msleep(300);												// Let the threads run together
		for (v=0; v<n_threads; v++)
			kthread_stop(p[v].kthread);
		n_total_attaches += p[0].n_cycles;
		n_total_messages   += p[1].n_cycles;
		tomaSimulator_protoBugs(false, false);

		BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
		unitest_print("*************** end Toma Msgs=%d, Attach/Detach=%d, %d[mSec]\n", n_total_messages, n_total_attaches, bunitest_toc(B));
	}
	// ------------------------------------ Test the case when Toma's unregister ack messages are lost
	if (1) {
		struct clientSimulator *client = &sys->clients[0];			// Test via the first client
		tomaSimulator_permitUnregAcks(false);
		send_command_to_vol(sys, -1, 0, volCmds_Detach);
		for (i=0; i<NVMESH_N_PHYS_DISKS_REGULAR_USE; i++)
			NVMeshSystem__invoke_pause_cont_on_disk(sys, i);		// Disk pause solves the problem
		send_command_to_vol(sys, -1, 0, volCmds_New);
		BUG_ON(!NVMeshSystem_is_stable(sys));						// System must be in a stable state

		// Now disk remove on dettach should solve the problem
		send_command_to_all(sys, -1, volCmds_Detach);
		send_command_to_all(sys, -1, volCmds_New   );
		for (v=0; v<client->nBdevs; v++) {sys->mdb.vols[v].nextCmd = volCmds_Illegal;}
		tomaSimulator_permitUnregAcks(true);
		BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	}

	return rv;
}

/* Test the unsafe-detahc mechanism, in which we detahc a volume while IO's are arriving at it. */
//TODO(EBA):
// 1) do we need to test on all volumes ?
// 2) we need cont/pause, suspend/revive on some disks while in this ?
TEST_FUNC int unitest_async_unsafe_detach_attach_during_io(struct NVMeshSystem *sys)
{
	t_async_test_params p[4];
	const int write_len_blk = 65, read_len_blk = 3;
	const int n_threads = ARRAY_SIZE(p);
	int v, n_total_ios = 0, n_total_attaches = 0;
	tomaSimulator_protoBugs(true, false);
	NVMeshSystem_di_tracking_enable(sys);
	for (v=0; v<n_threads; v++)
		t_async_test_params_init(p[v], sys, -1, -1, BUNITEST_ASYNC_TEST_FLAG_ALLOW_IO_ERROR);
	create_async_io_thread(&p[0], NVMEIB_BLOCK_IO_OP_WRITE, write_len_blk);
	create_async_io_thread(&p[1], NVMEIB_BLOCK_IO_OP_DISCARD, write_len_blk);
	create_async_io_thread(&p[2], NVMEIB_BLOCK_IO_OP_READ, read_len_blk);
	p[3].kthread = kthread_run(__thread_async_attach_dettach, 	&p[3], "ut:async attach/detach");	BUG_ON(p[3].kthread == NULL);
	msleep(300);												// Let the threads run together
	for (v=0; v<n_threads; v++) kthread_stop(p[v].kthread);
	n_total_attaches += p[3].n_cycles;
	for (v=0; v<3; v++)
		n_total_ios   += p[v].n_cycles;
	NVMeshSystem__detectStuckIOs(sys);
	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);			// Let sync operations terminate
	// Here we dont expect any stale locks as IO which fails due to detach will fail both R1 commands or they both succeed
	tomaSimulator_waitProtoEnd(NULL);									// Abandoned locks, can trigger toma messages
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_di_tracking_disable(sys);
	unitest_print("*************** Unsafe Detach: Attach/detach %d, Sent %d/%d RW/TR\n", n_total_attaches, n_total_ios, p[1].n_cycles);
	tomaSimulator_protoBugs(false, false);
	return 0;
}

static void __verify_resubmitter_state(struct nvmeibc_io_resubmitter *resub, int num_total, u32 num_reads, const u32 *vlba_order /*expected order of io's in queue*/) {
	ulong flags;
	int size, i;
	struct operation *op;
	spin_lock_irqsave(&resub->lock, flags);
	size = list_calc_size(&resub->list_paused_ops);
	BUG_ON(size != num_total);
	BUG_ON(resub->n_reads_ops != num_reads);
	BUG_ON(vlba_order == NULL && size != 0);
	i = 0;
	list_for_each_entry(op, &resub->list_paused_ops, list_paused) {  // Checking order of operations in queue.
		u64 start_lba = get_op_start_lba(op);
		NVMESH_BUG((vlba_order[i] != start_lba), __dump_operation_report, op, "Expected %d, got %llu", vlba_order[i], start_lba);
		++i;
	}
	spin_unlock_irqrestore(&resub->lock, flags);
}

/* Issue IO when it cannot be executed and test scheduled resubmittion */
int unitest_resubmitIO(struct NVMeshSystem *sys) {
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	const struct volume_segment_index vsi = {0,0,0,0};			// First seg/praid of first volume
	int rv = 0, volInd = vsi.volume;
	u8 *mem;
	int startBlock = _addr4k(3,30), lenBlocks = _addr4k(3,5);
	int memSize	= lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u64 magic;
	struct nvmeibc_block_device *dev = client->devs[volInd];
	struct TstPRaid praid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);
	struct nvmeibc_io_resubmitter *resub = &client->devs[volInd]->dp.resub;

	nvmeibc_io_perm_alert_set_unprotect_period(&dev->dp.io_perm_alert, 0);

	if (praid.cpr->slice_size == 1) {  				// R1 (mirror)
		mem = sim_kmalloc(memSize, 0);						// Array to read/write to disk
		magic = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
		NVMeshSystem__invoke_pause_on_disk(sys, 0);
		rv = osSimulator_trim(	  &client->OS, volInd, startBlock, lenBlocks);			REPORT_ERROR(rv);
		rv = osSimulator_readArr( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
		rv = osSimulator_trim(	  &client->OS, 1     , __from4K(57), __from4K(1));		REPORT_ERROR(rv);
		NVMeshSystem__invoke_cont_on_disk(sys, 0, false);
		NVMeshSystem__detectStuckIOs(sys);	// Impossible to verify that discarded areas are ok, because IO is async. Read could execute before trim
		magic = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
		rv = osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
		clientSimulator_wait_for_all_bio_ops(client);
		memset(mem, 0   	, memSize);								// Clear the array
		rv = osSimulator_readArr( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
		clientSimulator_wait_for_all_bio_ops(client);
		__unitest_verify_blocks_pattern(mem, lenBlocks, magic, false); // Verify that read and write matched.
		sim_kfree(mem);
	} else {  										// EC
		enum NVMEIBTC_DS_MODE seg_mode[N_MAX_RAID_SLICE_LEN];
		u32 vlba_order[3] = {0};
		DECLARE_COMPLETION_ONSTACK(comp);

		/*  Test resubmission of read on RONLY Topology.
			wA - write A, wB - write B, rA - read A
			Operations order: wA -> NO_IO -> wB -> rA -> wC -> R_ONLY -> rA -> wD -> R+W -> wait for all ios
			Resubmission queue expected states: [] -> NO_IO -> [wB|rA|wC] -> R_ONLY -> [wC|wB] -> [wC|wB|wD] -> R+W -> []*/
		startBlock = 0;
		lenBlocks = 1;
		memSize = 3 * NVMEIBC_SECTOR_SIZE;				// Total array in bytes (for 3 ios)
		mem = sim_kmalloc(memSize, 0);						// Array to read/write to disk
		array_fill(seg_mode, NVMEIBTC_DS_MODE_RW);
		NVMeshSystem_all_clients_dbg_di(sys, true);
		NVMeshSystem_all_clients_ec_edic(sys, true);
		magic = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);

		// Check that when IO can execute, it does not enter resubmittion
		rv |= osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);	REPORT_ERROR(rv); // wA
		__verify_resubmitter_state(resub, 0, 0, NULL);	   // resub queue should still be empty after wA

		//change topology to NO_IO
		seg_mode[1] = seg_mode[2] = seg_mode[3] = NVMEIBTC_DS_MODE_DEAD;
		tomaSimulator_switchTopoEC(r1uuid(praid.tpr), seg_mode, SW_TOPO__WAIT_ACK, NULL);  // {RW, DEAD, DEAD, DEAD}

		//-- Now Topology is NO_IO
		rv |= osSimulator_writeArr(          &client->OS, volInd, startBlock + 0, lenBlocks, &mem[0*lenBlocks*NVMEIBC_SECTOR_SIZE]);          REPORT_ERROR(rv); // wB
		rv |= osSimulator_readArr_async_wait(&client->OS, volInd, startBlock + 1, lenBlocks, &mem[1*lenBlocks*NVMEIBC_SECTOR_SIZE], &comp);   REPORT_ERROR(rv); // rA
		rv |= osSimulator_writeArr(          &client->OS, volInd, startBlock + 2, lenBlocks, &mem[2*lenBlocks*NVMEIBC_SECTOR_SIZE]);          REPORT_ERROR(rv); // wC
		vlba_order[0] = startBlock; vlba_order[1] = startBlock + 1; vlba_order[2] = startBlock + 2;
		__verify_resubmitter_state(resub, 3, 1, vlba_order);   	// resub queue should be [wB|rA|wC], There should be 1 read

		/* put the bd in double degraded for Read_ONLY topology */
		seg_mode[3] = NVMEIBTC_DS_MODE_RW;
		tomaSimulator_switchTopoEC(r1uuid(praid.tpr), seg_mode, SW_TOPO__WAIT_ACK, NULL);  // {RW, DEAD, DEAD, RW}

		//-- Now Topology is Read Only, verify that read is sent, while writes remain
		wait_for_completion(&comp); // Wait for rA exexution (# of pending ios == 2)
		vlba_order[0] = startBlock + 2; vlba_order[1] = startBlock;
		__verify_resubmitter_state(resub, 2, 0, vlba_order);   	// resub queue should be [wC|wB], Should be 0 reads in queue

		rv |= osSimulator_writeArr(&client->OS, volInd, startBlock + 3, lenBlocks, mem); REPORT_ERROR(rv); // wD
		vlba_order[0] = startBlock + 2; vlba_order[1] = startBlock; vlba_order[2] = startBlock + 3;
		__verify_resubmitter_state(resub, 3, 0, vlba_order);   	// resub queue should be [wC|wB|wD], Should be 0 reads in queue

		// Back_to normal topology
		seg_mode[1] = seg_mode[2] = NVMEIBTC_DS_MODE_RW;
		tomaSimulator_switchTopoEC(r1uuid(praid.tpr), seg_mode, SW_TOPO__WAIT_ACK, NULL);  // {RW, RW, RW, RW}

		// Wait for wC, wB, wD exexution (# of pending ios == 0)
		clientSimulator_wait_for_all_bio_ops(client);
		__verify_resubmitter_state(resub, 0, 0, NULL);   // Resub queue should be empty now
		sim_kfree(mem);
	}

	nvmeibc_io_perm_alert_set_unprotect_period(&dev->dp.io_perm_alert, 10*60);
	NVMeshSystem_all_clients_dbg_di(sys, false);
	BUG_ON(!NVMeshSystem_is_stable(sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.
	_NI_dmesg(trace_bunitest_unitest_resubmitIO, "*************** end");
	return rv;
}

#define __unitest_test_degraded_write(sys, r1, mem, volInd, ioVLBA, lenBlocks) __unitest_do_degraded_io(sys, r1, 0, mem, volInd, ioVLBA, lenBlocks)
static int __unitest_do_degraded_io(struct NVMeshSystem *sys, struct tTopoOfPraid* r1, int first_seg_ind, u8 *mem, int volInd, u64 ioVLBA, int lenBlocks){
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	int i, rv;
	struct disk_range *curSeg = &sys->mdb.vols[volInd].segs[first_seg_ind];
	const bool verify_bounds = (client->devs[volInd]->size > (unsigned)lenBlocks);
	const bool isStriped  = tTopoOfVolume_isStriped(&sys->tcf.vols[volInd]);
	u64        magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	int n_deg = 0, live_seg_ind = 0;
	tomaSimulator_waitProtoEnd(NULL);											// Wait for switch_topos which entered the client into degraded mode to terminate
	rv = osSimulator_trim(    &client->OS, volInd, ioVLBA, lenBlocks);			REPORT_ERROR(rv);
	clientSimulator_wait_for_all_bio_ops(client);
	rv = osSimulator_writeArrWait(&client->OS, volInd, ioVLBA, lenBlocks, mem);		REPORT_ERROR(rv);
	BUG_ON(curSeg->stripe_index != 0);												// Must be first in chunk. Otherwise calculations below will not work
	for (i=0; i<r1->header.n_segments; i++, curSeg++){ 								// Verify that magic number was written to both mirrors (first raid)
		const u64 phys_seg_end = curSeg->length + curSeg->dlba_start;												// All the calculations below are done in units of 4K
		const u64 phys_offset4K  = __to4K(ioVLBA) - curSeg->bd_start;											// Correct only for first r1 in a stripe (chunk)
		const u64 phys_start = curSeg->dlba_start + phys_offset4K;
		const u64 phys_len   = __to4K(lenBlocks);
		int verifyLength = ((phys_start+phys_len-1)>=phys_seg_end) ? (phys_seg_end-phys_start) : phys_len;		// The IO wraps to a different segment after the end of the disk
		const enum NVMEIBTC_DS_MODE access_mode = r1->s[i].access_mode;
		const int b4K_within_stripe = (curSeg->stripe_size - phys_offset4K%curSeg->stripe_size);
		u8 *dst = physSegStartPtr_off(curSeg, phys_offset4K);
		if (access_mode == NVMEIBTC_DS_MODE_DEAD) {
			BUG_ON(((u64*)dst)[0] == magic_pattern);
			n_deg++;
		} else {
			if (isStriped)
				verifyLength = min(verifyLength, b4K_within_stripe);
			__unitest_verify_blocks_pattern(dst, __from4K(verifyLength), magic_pattern, verify_bounds);
			live_seg_ind = i;
		}
	}

	if ((n_deg == (r1->header.n_segments-1)) && (r1->header.n_segments == 2)) { 							// Verify that read succeeds in full degraded even with stale special lock
		const struct disk_range *ownerSeg = &sys->mdb.vols[volInd].segs[first_seg_ind+live_seg_ind];						// Live segment in praid
		const u64 seg_start = __from4K(disk_range_get_start_addr(ownerSeg));	// First address of the segment.
		union nvmeibc_dbits_entry* db = physSegDBIdxPtr_off(ownerSeg, __to4K(seg_start));
		ramDiskSimulator_lockStale(&sys->servers[ownerSeg->node_id].ramDisk, ownerSeg->dlba_start);			// Put stale special value in the first lock of the segment.
		db->all_bits = 0;
		rv = osSimulator_readArrWait(&client->OS, volInd, seg_start, 1, mem);		REPORT_ERROR(rv);   // Just test that the read succeeds and clears the stale special lock
		BUG_ON(db->all_bits == 0);															// Write in degraded mode turns dbits on

		// Test that read view lock does not change unknowns
		nvmeibc_debug_ram_unknown_dbits = false;	// Double unknowns in 1-degraded
		db->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; 							// Set both unknowns, S2D must keep them intact
		rv = osSimulator_readArrWait(&client->OS, volInd, seg_start, 1, mem);		REPORT_ERROR(rv);   // Just test that the read succeeds and clears the stale special lock
		BUG_ON(db->all_bits != nvmeib_dbits_entry_build_unk(-1,-1).all_bits);
		nvmeibc_debug_ram_unknown_dbits = true;

		db->all_bits = nvmeib_dbits_entry_single_unk().all_bits;
		rv = osSimulator_readArrWait(&client->OS, volInd, seg_start, 1, mem);		REPORT_ERROR(rv);   // Just test that the read succeeds and clears the stale special lock
		BUG_ON(db->all_bits != nvmeib_dbits_entry_single_unk().all_bits);

		// Test that sync analises unknowns and decreases their incorrect number
		db->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits; 							// Set 2-unknowns, in 1-deg topo
		ramDiskSimulator_lockStale(&sys->servers[ownerSeg->node_id].ramDisk, ownerSeg->dlba_start);			// Put stale special value in the first lock of the segment.
		rv = osSimulator_readArrWait(&client->OS, volInd, seg_start, 1, mem);		REPORT_ERROR(rv);   // Just test that the read succeeds and clears the stale special lock
		BUG_ON(db->all_bits != nvmeib_dbits_entry_single_unk().all_bits);						// 1-deg topo, so we get back a single unknown
		if (ioVLBA > LOCKSET_SLICES) {
			db->all_bits = 0;
		}
		ramDiskSimulator_verify_no_locks(&sys->servers[ownerSeg->node_id].ramDisk);
	}

	rv = osSimulator_readArrWait(&client->OS, volInd, ioVLBA, lenBlocks, mem);		REPORT_ERROR(rv);   // Just test that the read succeeds
	return rv;
}

/* This function starts in {D,RW - io enabled} mode of raid 1 and finishes in {W,RW - io enabled} */
static int unitest_verify_dead_to_write_hot_transition(struct NVMeshSystem *sys) { // EXC-2145 Bug scenario reproduction (Verify client transitions {D,RW} -> {W.RW} without stopping IO even if dead toma sends registrables or nacks!
	const int volInd = 3;											// Volume will be used to test switch topology
	struct tTopoOfPraid* r1 = &sys->tcf.vols[volInd].chunks[0].raids[0];							// Select volume 3 for tests


	int ind_dead_seg = 0;//, flushed_msgs;
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	enum NVMEIBTC_DS_MODE seg_stats[N_MAX_RAID_SLICE_LEN];// = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
	struct disk_range *dead_seg   = &sys->mdb.vols[volInd].segs[ind_dead_seg];
	struct disk_range *otherSeg = &sys->mdb.vols[volInd].segs[ind_dead_seg^1];
	struct nvmeibc_topologies *nt = &(client->devs[volInd]->topologies);
	struct nvmeibc_topology *t;		// For debug
	const int dead_disk = dead_seg->node_id;
	const int live_disk = otherSeg->node_id;
	const int vol_on_dead_disk = mongo_db_simu_get_num_vol_on_disk(&sys->mdb, dead_disk);
	u64	test_vol_topo_unique_id;

	// Verify that registrable does not cause client to fallback to warm (registrable is delayed)
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;

	array_fill(seg_stats, NVMEIBTC_DS_MODE_RW);
	seg_stats[0] = NVMEIBTC_DS_MODE_W;
	_ND(trace_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "--------------------------------------- (case 1) -------------------------------------------");
	sys->servers[dead_seg->node_id].simToma.state = tomaState_not_ready;                                              // Dead toma (Toma index=2) is not ready (Toma index 3 is the live RW)
	tomaSimulator_switchTopo( r1uuid(r1), seg_stats[ind_dead_seg]	, seg_stats[ind_dead_seg^1], SW_TOPO__NONE);	// {W	, RW} - Lock live, client cannot complete the switch topo becuase he delayes it until Toma 2 will give him register ACK
	tomaSimulator_waitProtoEnd(NULL);
	tomaSimulator_reconnect(&sys->servers[dead_seg->node_id].simToma);												// W Toma=2 send registrable, it should not be applied because client will stop IO
	NVMeshSystem_serialize(sys);																					// Wait for registration with this Toma to finish
	tomaSimulator_waitSwitchTopoAck(otherSeg->node_id, r1->s[ind_dead_seg^1].uuid);									// Wait for switch topo ack to arrive to RW Toma

	if (force_reconf_reboot == false){
		BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);
	}

	_ND(trace_1_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "--------------------------------------- (case 2) -------------------------------------------");
	// Repeat the above test, now with pause and conts to verify that NACK does not cause client to fallback to warm (it is delayed)
	tomaSimulator_unreg_raid1(r1uuid(r1), ind_dead_seg);	NVMeshSystem_serialize(sys);						// {DEAD , RW}  IO enabled
	BUG_ON(!nvmeibc_topo_is_io_ok(nt));
	//t = ___get_tail_topo_of_device(sys, volInd);
	_ND(trace_2_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "Pause disk");
	NVMeshSystem__invoke_pause_on_disk(sys, dead_disk);																// Use PAUSE to disconnect client from dead Toma
	BUG_ON(!nvmeibc_topo_is_io_ok(nt));																				// PAUSE should not affect IO state, dead toma is irrelevant for IO
	io_toggle = nt->dbg_num_enabling_io_toggles;																	// Verify transition will be hot
	_ND(trace_3_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "Update toma topo");
	tomaSimulator_switchTopoEC(r1uuid(r1), seg_stats, SW_TOPO__SILENT, NULL);										// Toma's moved to {W,RW}, Client does not know about that (As if Switch Topo is on the way)
	//t = ___get_tail_topo_of_device(sys, volInd);
	_ND(trace_4_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "CONT disk");
	NVMeshSystem__invoke_cont_on_disk(sys, dead_disk, false);														// Cont causes client to communicate with previously dead toma and get NACK (because Tomas have {W,RW} and client has previos PD,RW} topo)
	NVMeshSystem_serialize(sys);
	BUG_ON((!nvmeibc_topo_is_io_ok(nt)) || (io_toggle != nt->dbg_num_enabling_io_toggles));									// Verify IO enabled in hot transition
	t = ___get_tail_topo_of_device(sys, volInd);
	BUG_ON(t->chunks[0].raid1s[0].version != r1->header.praid_version);												// Verify client is indeed in {W,RW}
	(void)t;

	_NT(trace_5_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "--------------------------------------- (case 3) -------------------------------------------");
	// Unitest where NACK is delayed and switch topo arrives and overwrites it (we hold only the latest on_active per segment)
	// Eitan: start by moving to {D,RW}, pause then before CONT, set TOMA to pass 1 msg to the client & then queue further msgs so the client wont get them. the client will see the REGISTER_NACK but when it tries to REGISTER again it wont get the ACK response. however, the second TOMA (server 3) will send its SWITCH_TOPO_ACK. this is not enough for the next topology, & as we flush queued msgs, the ACK from TOMA 2 will let the topology move to {W,RW} & become active.
	tomaSimulator_unreg_raid1(r1uuid(r1), ind_dead_seg);	NVMeshSystem_serialize(sys);						// {DEAD , RW}  IO enabled
	BUG_ON(!nvmeibc_topo_is_io_ok(nt));
	//t = ___get_tail_topo_of_device(sys, volInd);
	_ND(trace_6_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "Pause disk");
	NVMeshSystem__invoke_pause_on_disk(sys, dead_disk);																// Use PAUSE to disconnect client from dead Toma
	BUG_ON(!nvmeibc_topo_is_io_ok(nt));																				// PAUSE should not affect IO state, dead toma is irrelevant for IO
	io_toggle = nt->dbg_num_enabling_io_toggles;																	// Verify transition will be hot
	_ND(trace_7_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "Update toma topo");
	tomaSimulator_switchTopoEC(r1uuid(r1), seg_stats, SW_TOPO__SILENT, NULL);										// Toma's moved to {W,RW} of vol3, Client does not know about that (As if Switch Topo is on the way)
	tomaSimulator_enable_msg_q_to_client(&sys->servers[2].simToma);
	t = ___get_tail_topo_of_device(sys, volInd);
	test_vol_topo_unique_id = t->debug_unique_index;
	_ND(trace_8_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "@T_PRV, @C_PRV - CONT disk", r1->header.praid_version, t->chunks[0].raid1s[0].version);
	NVMeshSystem__invoke_cont_on_disk(sys, dead_disk, false);														// Cont causes client to send REGISTER to previously dead toma but doesnt get NACK
	// since CONT schedules async msgs, we wait until they are executed & the 3 msgs from TOMA are queued.
	clientSimulator_wait_for_single_topo_no_io(client);
	BUG_ON(tomaSimulator_get_msg_queue_size(&sys->servers[dead_disk].simToma) != 3);
	for (;test_vol_topo_unique_id > client->devs[volInd]->topologies.topo_debug_last_freed_version;) {
		sched_yield();
	}
	NVMeshSystem_serialize(sys);																					// wait for msgs from Toma 2 to be queued.
	_ND(trace_9_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "flush ACK/NACK msgs on REGISTER_SEGMENT request");

	//the race condition is between flush messages and switchTopo
	//flush messages to client sends messages to client, the clients handles them quickly and returns with another group of messages
	//toma is capable to proceed some of them in the current topology, but does not send them to client back
	//now, switch topology increments praid_version and sends the instruction to client
	//client process the instruction, but still remembers the segment it needs to activate
	//the segment praid version is 118, curr raid before switch is 119, toma raid 120
	//once the switch topology received we flush the rest of the messages
	//and now client is confused:
	tomaSimulator_disable_msg_q_to_client(&sys->servers[dead_disk].simToma);										// let the client rcv the switch topo from toma - this overwrites the on_active msg
	//flushed_msgs = tomaSimulator_flush_msg_to_client(&sys->servers[dead_disk].simToma, 3);							// flush the 3 msgs REGISTER_{ACK (vol0), NACK (vol2), NACK(vol3} that were created upon reply to REGISTER_SEGMENT. this will make the client topology put a delayed switch topo on topology.
	tomaSimulator_enable_msg_q_to_client(&sys->servers[dead_disk].simToma);
	//BUG_ON(flushed_msgs != vol_on_dead_disk);																		// a msg per volume
	_NT(trace_10_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "server @VOL_ON_DEAD_DISK (RW member) sends SwitchTopo to client to force switch topo", vol_on_dead_disk);
	BUG_ON(tomaSimulator_switchTopo( r1uuid(r1), seg_stats[ind_dead_seg]	, seg_stats[ind_dead_seg^1], SW_TOPO__NONE) != live_disk);	// switch topo (through Toma 3) so that Toma 3 will send SWITCH_TOPO msg to client in the middle of it attempting to REGISTER_SEGMENT. we cannot wait for ACK bcz the SWITCH_TOPO is put as delayed.


	// wait untill msg REGISTER_DISK_SEGMENT for "s_15_D_2" is proceesed by Toma
	tomaSimulator_wait_seg_registered(&sys->servers[dead_disk].simToma, dead_seg->ruuid, client->inst_id);
	tomaSimulator_flush_msg_to_client(&sys->servers[dead_disk].simToma, 0);							// flush the 3 msgs REGISTER_{ACK (vol0), NACK (vol2), NACK(vol3} that were created upon reply to REGISTER_SEGMENT. this will make the client topology put a delayed switch topo on topology.
	tomaSimulator_disable_msg_q_to_client(&sys->servers[dead_disk].simToma);										// let the client rcv the switch topo from toma - this overwrites the on_active msg
	NVMeshSystem_serialize(sys);																					// wait for the client rcv & process the SWITCH_TOPO msg that was delayed.
	_ND(trace_11_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "wait for switch Topo to complete");
	tomaSimulator_waitSwitchTopoAck(otherSeg->node_id, r1->s[ind_dead_seg^1].uuid);									// Wait for switch topo ack to arrive to RW Toma
	BUG_ON(!nvmeibc_topo_is_io_ok(nt));
	BUG_ON(force_reconf_reboot == false && (io_toggle != nt->dbg_num_enabling_io_toggles));									// Verify IO enabled in hot transition
	t = ___get_tail_topo_of_device(sys, volInd);
	BUG_ON(t->chunks[0].raid1s[0].version != r1->header.praid_version);												// Verify client is indeed in {W,RW}
	BUG_ON(tomaSimulator_get_msg_queue_size(&sys->servers[dead_disk].simToma) != 0);
	_ND(trace_12_bunitest_DRW__to__WRW_verify_hot_transition_EXC2145, "--------------------------------------- (Done) -------------------------------------------");

	return 0;
}

/* Test client's raid going in and out of a degraded mode */
TEST_FUNC int unitest_DegradedMode(struct NVMeshSystem *sys){
	const int volInd = 3;
	int i, ind_dead_seg = 0;						// Volume will be used to test switch topology
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	struct tTopoOfPraid* r1 = &sys->tcf.vols[volInd].chunks[0].raids[0];							// Select volume 3 for tests
	struct disk_range *curSeg = NULL, *otherSeg = NULL; // Other seg used to verify dirty bit
	int rv = 0;
	union nvmeibc_dbits_entry *db_val;
	/*const*/int lenBlocks = __from4K(2);			// Length of IO. 1 4k-block is written as mirrored, 1 4K-block only in degraded mode
	/*const*/int memSize	 		 = lenBlocks*NVMEIBC_SECTOR_SIZE;// Total array in bytes
	u8 *dst = NULL, *mem = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	u64       magic_pattern=__unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	enum NVMEIBTC_DS_MODE seg_stats[2] = {NVMEIBTC_DS_MODE_W, NVMEIBTC_DS_MODE_RW};
	struct serverSimulator *curServer, *otherServer;

	// -------------------- Simulate full toma protocol of entering and quitting from degraded state on each segment.
	u64 startBlock = sys->mdb.vols[volInd].segs[0].length - 1; // 1 block is written to {disk2,disk3}, 1 block to {disk2,disk3}
	for (i=0; i<2; i++, startBlock-=LOCKSET_SLICES) { 			// First iteration write to 4 disks, second iteration, write to 2 disks but on 2 locksets on each disk
		for (ind_dead_seg = 0; ind_dead_seg < 2; ind_dead_seg++) {
			curSeg   = &sys->mdb.vols[volInd].segs[ind_dead_seg];
			otherSeg = &sys->mdb.vols[volInd].segs[ind_dead_seg^1];
			curServer = serverOf(&client->physDiscs[curSeg->node_id]);
			otherServer = serverOf(&client->physDiscs[otherSeg->node_id]);
			ramDiskSimulator_verify_no_locks(&otherServer->ramDisk);
			serverSimulator_disconnect(curServer);
			tomaSimulator_unreg_raid1(r1uuid(r1), ind_dead_seg);																// {DEAD , RW} -> Lock only live
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			db_val = physSegDBIdxPtr_off(otherSeg, __to4K(startBlock));
			BUG_ON(db_val->all_bits == 0);																						// Verify DB is set after write
			// Test unknown DBs are not changed after read, and ARE changed after write
			db_val->all_bits = nvmeib_dbits_entry_single_unk().all_bits;
			rv = osSimulator_readArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
			BUG_ON(db_val->all_bits != nvmeib_dbits_entry_single_unk().all_bits);
			// Add stale lock, stale should be release and unknown should remain (since merged by sync we will have a single unknown value)
			db_val->all_bits = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;
			ramDiskSimulator_lockStale(&otherServer->ramDisk, otherSeg->dlba_start+startBlock);			// Put stale special value in the lock of the IO.
			rv = osSimulator_readArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
			BUG_ON(db_val->all_bits != nvmeib_dbits_entry_single_unk().all_bits); // Double unknown resolved to single unknowns
			ramDiskSimulator_verify_no_locks(&otherServer->ramDisk);
			rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
			BUG_ON(db_val->all_bits == nvmeib_dbits_entry_single_unk().all_bits);
			BUG_ON(db_val->all_bits == 0);
			serverSimulator_re_connect(curServer); tomaSimulator_waitProtoEnd(NULL);											// Reconnect and wait for regiistrables to arrive
			NVMeshSystem_serialize(sys);						// wait until r1 has registered again (after unregistering) before switchTopo
			tomaSimulator_switchTopo(r1uuid(r1), seg_stats[ind_dead_seg]	, seg_stats[ind_dead_seg^1]	, SW_TOPO__WAIT_ACK);	// {W	, RW} - Lock live
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			// Set unknown again, verify read doesn't change it
			db_val->all_bits = nvmeib_dbits_entry_single_unk().all_bits;
			rv = osSimulator_readArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
			BUG_ON(db_val->all_bits != nvmeib_dbits_entry_single_unk().all_bits);
			// Write will not fix unknown if small but will if big
			rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
			BUG_ON(db_val->all_bits != nvmeib_dbits_entry_single_unk().all_bits);
			if (1) {
				u8 *full_bs = sim_kmalloc(LOCKSET_SLICES*NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
				rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock - (startBlock % LOCKSET_SLICES), LOCKSET_SLICES, full_bs);		REPORT_ERROR(rv);
				sim_kfree(full_bs);
			}
			BUG_ON(db_val->all_bits != 0);
			if (i) { 															// Verify that for 2 blocksets IO, 2 dbits were turned on
				BUG_ON(db_val[1].all_bits == 0); db_val[1].all_bits = 0;
			}
			seg_stats[0] = NVMEIBTC_DS_MODE_W_NO_DIRTY;
			tomaSimulator_switchTopo(r1uuid(r1), seg_stats[ind_dead_seg]	, seg_stats[ind_dead_seg^1], SW_TOPO__WAIT_ACK);	// {W+	, RW} - lock both, safe topo. Illegal since V1.0.5, was replaced by unsafe version. Keeping it for backwards compatibility
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			tTopoOfPraid_force_lock_on_read(r1, true);
			tomaSimulator_switchTopo(r1uuid(r1), seg_stats[ind_dead_seg]	, seg_stats[ind_dead_seg^1], SW_TOPO__WAIT_ACK);	// {W+	, RW} - lock both, unsafe
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			seg_stats[0] = NVMEIBTC_DS_MODE_W;
			tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	   	, NVMEIBTC_DS_MODE_RW		, SW_TOPO__WAIT_ACK);	// {RW	, RW} - normal, unsafe
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			tTopoOfPraid_force_lock_on_read(r1, false);
			tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	   	, NVMEIBTC_DS_MODE_RW		, SW_TOPO__WAIT_ACK);	// {RW	, RW} - normal
			clientSimulator_wait_for_all_bio_ops(client); NVMeshSystem_serialize(sys); // Todo: Wait until all topos are freed. Last IO operation was resubmitted and terminated but its topo reference was passed to the locks and did not have time to free
			BUG_ON(!NVMeshSystem_is_stable(sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.
		}
	}

	// -------------------- Verify we returned back to normal topology by writing one 4K-block on all 4 disks
	startBlock = sys->mdb.vols[volInd].segs[0].length - 1; // 1 block is written to {disk2,disk3}, 1 block to {disk2,disk3}
	curSeg = &sys->mdb.vols[volInd].segs[0];
	for (i=0; i<4; i++) { 									// Set fictitious illegal dirty bits to verify that write does not clean them
		db_val = physSegDBIdxPtr_off(&curSeg[i], ((i<2)?__to4K(startBlock):0));
		db_val->all_bits = 0x17;
	}
	magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	warn_on_too_many_degraded = nvmeibc_debug_ram_binfo = false;			// Deliberate ficticious dirty-bit
	rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
	warn_on_too_many_degraded = nvmeibc_debug_ram_binfo = true;
	for (i=0; i<4; i++){ 									// Verify that magic number was written to both mirrors
		dst = physSegStartPtr_off(&curSeg[i], ((i<2)?__to4K(startBlock):0));
		__unitest_verify_blocks_pattern(dst, __from4K(1), magic_pattern, true);
		db_val = physSegDBIdxPtr_off(&curSeg[i], ((i<2)?__to4K(startBlock):0));
		BUG_ON(db_val->all_bits != 0x17);							// Verify DB is still set (since we never wrote the full segment during W mode)
	}
	for (i=0; i<4; i++)
		ramDiskSimulator_wipe_dirty_bits(&sys->servers[curSeg[i].node_id].ramDisk, 0);
	BUG_ON(!NVMeshSystem_is_stable(sys));

	// -------------------- Do very long trims (partially on degraded raid1 (only owner locks) partially on regular raid1 (prediscards))
	startBlock = 0;	lenBlocks = client->devs[volInd]->size;
	sim_kfree(mem);
	memSize = lenBlocks*NVMEIBC_SECTOR_SIZE;
	mem = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
	ind_dead_seg = 0;
	curSeg = &sys->mdb.vols[volInd].segs[ind_dead_seg];
	otherSeg = &sys->mdb.vols[volInd].segs[ind_dead_seg^1];
	curServer = serverOf(&client->physDiscs[curSeg->node_id]);
	serverSimulator_disconnect(curServer);
	tomaSimulator_unreg_raid1(r1uuid(r1), ind_dead_seg);														// {DEAD , RW} -> Lock only live
	__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
	serverSimulator_re_connect(curServer);
	NVMeshSystem_serialize(sys);
	for (i=0; i<(signed)((otherSeg->length)/LOCKSET_SLICES); i++) {													// We overwrite the entire volume so all Dirty bits of the segment should be set
		db_val = physSegDBIdxPtr_off(otherSeg, i*LOCKSET_SLICES);
		BUG_ON(db_val->all_bits == 0);																				// Verify DB is set
	}
	//fucking cross test dependency - the following test should leave raid in degraded mode
	unitest_verify_dead_to_write_hot_transition(sys);
	__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
	for (i=0; i<(signed)((otherSeg->length)/LOCKSET_SLICES); i++) {													// We overwrite entire locksets so the dirty bits must be unset
		db_val = physSegDBIdxPtr_off(otherSeg, i*LOCKSET_SLICES);
		BUG_ON(db_val->all_bits != 0);																				// Verify DB is unset after full lockset write
	}
	seg_stats[0] = NVMEIBTC_DS_MODE_W_NO_DIRTY;
	tomaSimulator_switchTopo(r1uuid(r1), seg_stats[ind_dead_seg]	, seg_stats[ind_dead_seg^1], SW_TOPO__WAIT_ACK);// {W	, RW} - lock both
	__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
	seg_stats[0] = NVMEIBTC_DS_MODE_W;
	tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	   	, NVMEIBTC_DS_MODE_RW	   , SW_TOPO__WAIT_ACK);// {RW	, RW} - normal
	clientSimulator_wait_for_all_bio_ops(client); NVMeshSystem_serialize(sys); // Todo: Wait until all topos are freed. Last IO operation was resubmitted and terminated but its topo reference was passed to the locks and did not have time to free
	BUG_ON(!NVMeshSystem_is_stable(sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.

	// -------------------- Simulate entering degraded mode, and while recovering in dual lock, fall to degraded again.
	startBlock = sys->mdb.vols[volInd].segs[0].length - 1; // 1 block is written to {disk2,disk3}, 1 block to {disk2,disk3}
	lenBlocks = __from4K(2);									// Length of IO. 1 4k-block is written as mirrored, 1 4K-block only in degraded mode
	for (i=0; i<2; i++, startBlock-=LOCKSET_SLICES) { 			// First iteration write to 4 disks, second iteration, write to 2 disks but on 2 locksets on each disk
		for (ind_dead_seg = 0; ind_dead_seg < 2; ind_dead_seg++) {
			bool second_degraded = true;
			curSeg =   &sys->mdb.vols[volInd].segs[ind_dead_seg];
			otherSeg = &sys->mdb.vols[volInd].segs[ind_dead_seg^1];
			curServer = serverOf(&client->physDiscs[curSeg->node_id]);
		__start_degraded:
			serverSimulator_disconnect(curServer);
			tomaSimulator_unreg_raid1(r1uuid(r1), ind_dead_seg);														// {DEAD , RW} -> Lock only live
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			serverSimulator_re_connect(curServer); tomaSimulator_waitProtoEnd(NULL);									// Reconnect and wait for regiistrables to arrive
			NVMeshSystem_serialize(sys);
			db_val = physSegDBIdxPtr_off(otherSeg, __to4K(startBlock));
			BUG_ON(db_val[0].all_bits == 0); db_val[0].all_bits = 0;
			if (i) {
				BUG_ON(db_val[1].all_bits == 0); db_val[1].all_bits = 0;
			}
			tomaSimulator_switchTopo(r1uuid(r1), seg_stats[ind_dead_seg], seg_stats[ind_dead_seg^1], SW_TOPO__WAIT_ACK);// {W	, RW} - Lock live
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			seg_stats[0] = NVMEIBTC_DS_MODE_W_NO_DIRTY;
			tomaSimulator_switchTopo(r1uuid(r1), seg_stats[ind_dead_seg], seg_stats[ind_dead_seg^1], SW_TOPO__WAIT_ACK);// {W	, RW} - lock both
			seg_stats[0] = NVMEIBTC_DS_MODE_W;
			__unitest_test_degraded_write(sys, r1, mem, volInd, startBlock, lenBlocks);
			if (second_degraded) {
				second_degraded = false;
				goto __start_degraded;
			}
			tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW	   	, NVMEIBTC_DS_MODE_RW  , SW_TOPO__WAIT_ACK);// {RW	, RW} - normal
			clientSimulator_wait_for_all_bio_ops(client); NVMeshSystem_serialize(sys); // Todo: Wait until all topos are freed. Last IO operation was resubmitted and terminated but its topo reference was passed to the locks and did not have time to free
			BUG_ON(!NVMeshSystem_is_stable(sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.
		}
	}
	sim_kfree(mem);
	_NI_dmesg(trace_bunitest_unitest_DegradedMode, "*************** end");
	return rv;
}

u64 __convert_dlba_to_vlba(struct NVMeshSystem *sys, const struct TstPRaid *tst_raid, u64 dlba)
{
	struct clientSimulator *client = sys->clients;
	struct nvmeibc_block_device *dev = client->devs[tst_raid->vsi.volume];
	struct nvmeibc_datapath *dp = &dev->dp;
	const struct volume_segment_index vsi = tst_raid->vsi;

	struct nvmeibc_topology *topo = nvmeibc_topology_get(&dev->topologies);
	struct nvmeibc_raid1 *client_raid = &topo->chunks[vsi.chunk].raid1s[vsi.raid];
	const u64 result = nvmeibc_datapath_dlba_to_vlba(dp, client_raid, tst_raid->vsi.segment, dlba);
	nvmeibc_topology_put(topo);
	return result;
}

//writes to segment via OS and then check that the data was written, by inspecting physical segment location
//returns the written magic_pattern
u64 __verify_segment_write(struct NVMeshSystem *sys, const struct TstPRaid * raid, u8 nblocks) {
	const struct disk_range *curr_dr = &raid->cpr[raid->vsi.segment];
	const int mem_size = nblocks * NVMEIBC_SECTOR_SIZE;   // Total array in bytes
	u8 *mem = sim_kmalloc(mem_size, 0);                           // Array to read/write to disk
	u8 *curr_io_dest = physSegStartPtr(curr_dr);
	struct clientSimulator *client = &sys->clients[0];    // Current client
	const u64 magic_pattern = __unitest_fill_blocks_unique_pattern(mem, nblocks);    // Set a pattern.
	const u64 vlba = __convert_dlba_to_vlba(sys, raid, __from4K(curr_dr->dlba_start));
	osSimulator_writeArrWait(&client->OS, raid->vsi.volume, vlba, nblocks, mem);
	__unitest_verify_blocks_pattern_data(curr_io_dest, nblocks, magic_pattern, true, true);
	sim_kfree(mem);
	return magic_pattern;
}

void __verify_segment_read(struct NVMeshSystem *sys, const struct TstPRaid * raid, u8 nblocks, u64 magic) {
	const struct disk_range *curr_dr = &raid->cpr[raid->vsi.segment];
	const int mem_size = nblocks * NVMEIBC_SECTOR_SIZE;   // Total array in bytes
	u8 *mem = sim_kmalloc(mem_size, 0);                           // Array to read/write to disk
	struct clientSimulator *client = &sys->clients[0];    // Current client
	osSimulator_readArrWait(&client->OS, raid->vsi.volume, __from4K(curr_dr->bd_start), nblocks, mem);
	__unitest_verify_blocks_pattern(mem, nblocks, magic, false);
	sim_kfree(mem);
}


const enum NVMEIBTC_DS_MODE SEGMENT_LIFE_CYCLE[] =
	{ NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_W, NVMEIBTC_DS_MODE_W_NO_DIRTY, NVMEIBTC_DS_MODE_RW };

void __switch_segment_topos(struct NVMeshSystem *sys
							, const struct TstPRaid* praid
							, enum NVMEIBTC_DS_MODE start_from){
	bool should_apply = false;
	struct clientSimulator *client = &sys->clients[0];
	for (u64 index = 0; index < ARRAY_SIZE(SEGMENT_LIFE_CYCLE); ++index){
		if (should_apply == false)
			should_apply = SEGMENT_LIFE_CYCLE[index] == start_from;
		if (should_apply)
			tomaSimulator_switchSegmentTopo(praid, SEGMENT_LIFE_CYCLE[index], SW_TOPO__WAIT_ACK, NULL);
	}
	__wait_for_topology_change(client->devs[praid->vsi.volume]);
	BUG_ON(!NVMeshSystem_is_stable(sys));
}


int __test_segment_relocation_hot(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange){
	int rv = -1;

	struct disk_range *src_seg = &src->cpr[src->vsi.segment];
	const struct nvmeibc_topologies *nt = &(sys->clients[0].devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	const int src_node = src_seg->node_id;
	const u64 src_start = src_seg->dlba_start;
	// ------------------------------------ Relocate last segment normally (hot relocation).
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, dst_drange->dlba_start); // Move the segment to different address (colliding with Vol1)

	/* Jared: Wait for SERJIO to disconnect the client and for it to reconnect */
	NVMeshSystem_serialize(sys);

	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_DEAD);
	// Test that it did move by performing write on the new location of segment of Vol3, and reading from Vol1
	BUG_ON(src_seg->node_id != dst_drange->node_id || src_seg->dlba_start != dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a hot transition

	// ------------------------------------ Move the segment back, fallback to warm (forced by update configuration arriving while previous update is still being processed)
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, src_start); // Move the segment to different address (colliding with Vol1)

	/* Jared: Wait for SERJIO to disconnect the client and for it to reconnect */
	NVMeshSystem_serialize(sys);

	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);	__unitest_volume_config_version_inc(&sys->mdb.vols[src->vsi.volume], &sys->tcf.vols[src->vsi.volume]);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);	__unitest_volume_config_version_inc(&sys->mdb.vols[src->vsi.volume], &sys->tcf.vols[src->vsi.volume]); // Cause warm fallback by sending the same configuration (generating erroneous state)
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	//tomaSimulator_switchSegmentTopo(r1uuid(src->tpr), NVMEIBTC_DS_MODE_RW,src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__?); - Not mandatory, Client's logic works without this message, by fallbacking to warm
	tomaSimulator_waitProtoEnd(NULL);			// wait for toma registrations that might result from VOLUME_MISMATCH msgs
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W);
	// Test that it did move back by performing write on the new location of segment
	BUG_ON(src_seg->node_id == dst_drange->node_id && src_seg->dlba_start == dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	BUG_ON(++io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a warm transition
	return rv;
}

int __test_segment_relocation_hot_with_induced_topo_changes(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange, struct TstPRaid* unrelated){
	//right now not applicable to EC, since we don't have a volume to borrow segment from
	int rv = -1;

	struct clientSimulator *client = &sys->clients[0];
	const struct nvmeibc_topologies *nt = &(client->devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	struct disk_range *src_seg = &src->cpr[src->vsi.segment];
	const int src_node = src->cpr[src->vsi.segment].node_id;

	struct switch_topo_dest toma_courier = {0};

	// ------------------------------------ Relocate last segment normally (hot relocation), but induce topo change while previous switch topo was not applied yet.
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, dst_drange->dlba_start);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);

	serverSimulator_disconnect(&sys->servers[src_seg->node_id]);					// Simulate as if the Dead toma is comming up but not ready to receive registration even though live toma said {RW,W}
	sys->servers[src_seg->node_id].simToma.state = tomaState_not_ready;
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W, SW_TOPO__NONE, &toma_courier);
	if (unrelated)
		tomaSimulator_switchSegmentTopo(unrelated, NVMEIBTC_DS_MODE_W, SW_TOPO__WAIT_ACK, NULL);
	serverSimulator_re_connect(&sys->servers[src_seg->node_id]);					// Registrable arrives, Client will apply the delayed switch-topo to {RW,W}
	tomaSimulator_waitSwitchTopoAck(src_seg->node_id, src->tpr->s[src->vsi.segment].uuid);
	tomaSimulator_waitSwitchTopoAck(toma_courier.disk_ind, src->tpr->s[toma_courier.seg_ind].uuid);
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W_NO_DIRTY, SW_TOPO__WAIT_ACK, NULL);
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK, NULL);
	__wait_for_topology_change(client->devs[src->vsi.volume]);
	if (unrelated)
		tomaSimulator_switchSegmentTopo(unrelated, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK, NULL);	// Return unrelated raid to its normal state
	BUG_ON(!NVMeshSystem_is_stable(sys));

	// Test that it did move by performing write on the new location of segment of Vol3, and reading from Vol1
	BUG_ON(src_seg->node_id != dst_drange->node_id || src_seg->dlba_start != dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a hot transition

	// ------------------------------------ Move the segment back, Confuse the client to force him out of sync with Dying Toma doing switch topo
	if (1) { /* Raid one moves from Tomas {T4,T1} back to {T4,T5}. Force T1 to send illegal switch topo but delay it */
		struct switch_topo_options sto = {.dont_send_msg = false, .wait_for_ack = false, .use_seg_index = true, .seg_index = src->vsi.segment}; //Send through segment which will become depricated

		struct tomaSimulator *toma = &sys->servers[dst_drange->node_id].simToma;
		struct nvmeibt_client_msg* msg;
		tomaSimulator_enable_msg_q_to_client(toma);	// Wrong Message is delyaed to inject it in the right moment
		tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_RW, sto, NULL); // client will see switch-topo with (lock_id == 0) & fail to warm. it wont send switch-topo-ack !!! it will unregister & register again.
		msg = (struct nvmeibt_client_msg*)toma_msg_q__view_last_msg(&toma->msg_q);
		msg->thick.lock_id ^= 0x1111;														// Change the lock id so there will not be a match
		msg->thick.praid_version = nvmeib_ntohl(nvmeib_htonl(msg->thick.praid_version)+10); // Inc raid version, to make sure client will not ignore this message (coz next switch topos will also increase it)
		tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);	// R1 = {T4=RW, T1=DEAD}, in EC: src->si is DEAD
		mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, src->cpr->dlba_start);
		rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
		tomaSimulator_disable_msg_q_to_client(toma);		// Now T1 sends unrelated switch topos and forces client to move from T1->T5 in the context of T1 message
		tomaSimulator_waitProtoEnd(toma);
	}

	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W);
	// Test that it did move back by performing write on the new location of segment
	BUG_ON(src_seg->node_id == dst_drange->node_id && src_seg->dlba_start == dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	BUG_ON(++io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a warm transition
	return rv;
}

int __test_segment_relocation_forward_reconfigure_twice_backward_client_suicide(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange){
	int rv = -1;

	struct clientSimulator *client = &sys->clients[0];
	const struct nvmeibc_topologies *nt = &(client->devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	const struct disk_range *src_seg = &src->cpr[src->vsi.segment], *other_seg_rw = &src->cpr[(src_seg->stripe_index+1)%2];
	const u64 src_start = src_seg->dlba_start;
	const int src_node = src_seg->node_id;

	// ------------------------------------ Relocate forward again, management sends by mistake a few copies of the same reconfs
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, dst_drange->dlba_start); // Move the segment to different address (colliding with Vol1)
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W   , SW_TOPO__WAIT_ACK, NULL);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W_NO_DIRTY, SW_TOPO__WAIT_ACK, NULL);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	tomaSimulator_switchTopoBlocked(client->devs[src->vsi.volume], r1uuid(src->tpr), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW);
	BUG_ON(!NVMeshSystem_is_stable(sys));

	BUG_ON(src_seg->node_id != dst_drange->node_id || src_seg->dlba_start != dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);								// Test that it did move by performing write on the new location of segment of Vol3, and reading from Vol1
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a hot transition

	// ------------------------------------ Move the segment back, Toma gets conf a bit before client, client cannot comply to switch topo and Toma cuts the client.
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, src_start); // Restore the address from its RAID counterpart.
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__NONE, NULL);	// Tomas received new conf and sent switch topologies before client received the new conf
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);								// Client finally gets the configuration
	NVMeshSystem__invoke_pause_cont_on_disk(sys, other_seg_rw->node_id);				// Toma cut the client. Client got pause and continue on the RW disk
	tomaSimulator_waitSwitchTopoAck(other_seg_rw->node_id, other_seg_rw->ruuid);
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W);
	BUG_ON(src_seg->node_id == dst_drange->node_id && src_seg->dlba_start == dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);								// Test that it did move back by performing write on the new location of segment
	BUG_ON(++io_toggle != nt->dbg_num_enabling_io_toggles);				// ++ Due to PAUSE, Verify this is a warm transition
	return rv;
}

int __test_segment_relocation_via_registrable_forward_toma_first_backward_client_first_may_force_client_ahead(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange, int pause_cont_node, bool force_client_ahead){
	int rv = -1;

	struct disk_range *src_seg = &src->cpr[src->vsi.segment];
	const struct nvmeibc_topologies *nt = &(sys->clients[0].devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	const u64 src_start = src_seg->dlba_start;
	const int src_node = src->cpr[src->vsi.segment].node_id;
	struct tTopoOfVolume *cfv = &sys->tcf.vols[src->vsi.volume];
	int injected_conf_version_delta = 10;

	// ------------------------------------ Relocate forward again, Toma delivers reconf via registrable message, before client has the configuration
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, dst_drange->dlba_start); // Move the segment to different address (colliding with Vol1)

	NVMeshSystem__invoke_pause_on_disk(sys, pause_cont_node);
	NVMeshSystem__invoke_cont_on_disk( sys, pause_cont_node, true);

	if (force_client_ahead) { // use injection to get client's volume version be way ahead of toma (reference from unitest_Raid1_SwitchTopo)
		_NT(tsrvrftfbcfmi_inject, "forcing client ahead by setting config version seen by toma from t_@C_VOL_VER to @INT", cfv->version, cfv->version - injected_conf_version_delta);
		BUG_ON(cfv->version < injected_conf_version_delta);
		cfv->version -= injected_conf_version_delta;
	}

	tomaSimulator_reconnect(&sys->servers[pause_cont_node].simToma);					// Registrable arrives, client can only mark that toma requested relocation but it does not have it

	if (force_client_ahead) {
		tomaSimulator_waitProtoEnd(NULL);
		_NT(tsrvrftfbcficv_deinject, "reverting force client ahead, setting config version seen by toma back to t_@C_VOL_VER", cfv->version + injected_conf_version_delta);
		cfv->version += injected_conf_version_delta; // revert the injection
	}

	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);		// Now client applies the relocation immediately
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W);
	// Test that it did move by performing write on the new location of segment of Vol3, and reading from Vol1
	BUG_ON(src_seg->node_id != dst_drange->node_id || src_seg->dlba_start != dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	BUG_ON(++io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a warm transition

	// ------------------------------------ Move the segment back, Again via registrable but now client gets the configuration before Toma
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, src_start); // Restore the address from its RAID counterpart.

	NVMeshSystem__invoke_pause_on_disk(sys, pause_cont_node);
	NVMeshSystem__invoke_cont_on_disk( sys, pause_cont_node, true);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);	// Now client applies the relocation immediately
	tomaSimulator_reconnect(&sys->servers[pause_cont_node].simToma);					// Registrable arrives, client already has the latest config and has to apply it
	tomaSimulator_waitProtoEnd(&sys->servers[pause_cont_node].simToma);

	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_DEAD);
	// Test that it did move back by performing write on the new location of segment
	BUG_ON(src_seg->node_id == dst_drange->node_id && src_seg->dlba_start == dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	BUG_ON(++io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a warm transition
	return rv;
}

int __test_segment_relocation_via_registrable_forward_toma_first_backward_client_first(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange, int pause_cont_node){
	return __test_segment_relocation_via_registrable_forward_toma_first_backward_client_first_may_force_client_ahead(sys, src, dst_drange, pause_cont_node,
															 false /* don't force client ahead */);
}

int __test_segment_relocation_via_registrable_client_ahead(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange, int pause_cont_node){
	return __test_segment_relocation_via_registrable_forward_toma_first_backward_client_first_may_force_client_ahead(sys, src, dst_drange, pause_cont_node,
															 true /* force client ahead */);
}

int __test_segment_relocation_forward_toma_client_delayed(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange, int reconnect_node){
	int rv = -1;

	struct disk_range *src_seg = &src->cpr[src->vsi.segment];
	const struct nvmeibc_topologies *nt = &(sys->clients[0].devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	const u64 src_seg_start = src_seg->dlba_start;
	const int src_node = src->cpr[src->vsi.segment].node_id;
	struct switch_topo_dest toma_courier = {0};

	BUG_ON(src->vsi.segment == 0);							// Coz Toma sends switch topo via the smallest possible segment index

	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, dst_drange->dlba_start);

	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__NONE, NULL);	// Tomas received new conf and sent switch topologies before client received the new conf
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W   , SW_TOPO__NONE, NULL);	// Client did not ACK the switch topos. Toma is going to cut the client from the disk
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);								// Client finally gets the configuration
	NVMeshSystem__invoke_pause_cont_on_disk(sys,reconnect_node);				// Toma cut the client. Client got pause and continue on the RW disk
	tomaSimulator_waitSwitchTopoAck(dst_drange->node_id, src->tpr->s[src->vsi.segment].uuid);
	NVMeshSystem__invoke_pause_cont_on_disk(sys, dst_drange->node_id);											// Random cont on dst disk
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W_NO_DIRTY);
	// Test that it did move by performing write on the new location of segment of Vol3, and reading from Vol1
	BUG_ON(src_seg->node_id != dst_drange->node_id || src_seg->dlba_start != dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	io_toggle += 2;

	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);				// 2 Pause/Conts

	// ------------------------------------ Move the segment back: Hot fashion, Client misses all switch topos of toma, since it gets the configuration very late (30 minutes after toma)
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK, NULL);
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, src_seg_start); // Restore the address from its RAID counterpart.
	// Wait for serjio to induce pause cont on disk 0
	NVMeshSystem_serialize(sys);
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__NONE, &toma_courier); // Client cannot ACK the switch topos and will ignore it because it does not have the latest configuration.
	BUG_ON((u32)toma_courier.disk_ind != src->cpr->node_id || toma_courier.seg_ind != 0);
	// ... Here 30 seconds pass, with many PAUSE/CONTS, switch-topos etc. Client still haven't got the configuration. First Toma of R1 (The live segment) cuts the client
	tomaSimulator_disconnect(&sys->servers[reconnect_node].simToma);
	// Now client will try to reregister. The live toma will answer that configuration version is behind, the toma of the deprecated segment will answer WTF? (I am not participating in R1 anymore). Client will not get NACK's so it is stuck unregistered and will not get the switch topos below
  /*tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W         , SW_TOPO__NONE);	// As if Toma sent those switch topos to other clients but our client could not get them
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_W_NO_DIRTY, SW_TOPO__NONE);*/
	tomaSimulator_switchSegmentTopo(src, NVMEIBTC_DS_MODE_RW  , SW_TOPO__NONE, NULL);
	tomaSimulator_reconnect(&sys->servers[reconnect_node].simToma);

	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);								// Client finally gets the configuration
	tomaSimulator_unsetSwitchTopoWait(toma_courier.disk_ind, 0, src->tpr->s[toma_courier.seg_ind].uuid);
	BUG_ON(!NVMeshSystem_is_stable(sys));

	// Test that it did move back by performing write on the new location of segment
	BUG_ON(src_seg->node_id == dst_drange->node_id && src_seg->dlba_start == dst_drange->dlba_start);
	__verify_segment_write(sys, src, 1);
	{
		const bool is_ec = (1 < src_seg->slice_size);
		if (is_ec){
			BUG_ON((io_toggle != nt->dbg_num_enabling_io_toggles) && ((io_toggle+1) != nt->dbg_num_enabling_io_toggles));
			//on sgmnt deletion serjio disconnects the clients - one more pause/cont
		} else {
			BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);
		}
	}
	return rv;
}


int __test_segment_relocation_forward_disk_rediscovery_hot_backward_from_degraded_mode(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange){
	int rv = -1;

	struct disk_range *seg0 = &src->cpr[0];
	const struct nvmeibc_topologies *nt = &(sys->clients[0].devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;

	const int src_node = src->cpr[src->vsi.segment].node_id;
	const u8  magic_wipe1 = 'r';
	const u64 magic_wipe8 = 0x7272727272727272; //hex(ord('r')) == 0x72
	const int nblocks = 1;									// Length of IO
	// ------------------------------------ Relocate first segment of Vol0, from disk 0 to new 6'th disk in HOT fashion (test disk redescovery)
	tomaSimulator_switchTopo(r1uuid(src->tpr), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);		// Optional. Must be present for HOT relocation
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, seg0->dlba_start);

	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W);

	// Test that it did move by performing read on newly wiped disk
	ramDiskSimulator_wipe(&sys->servers[dst_drange->node_id].ramDisk, magic_wipe1);		// Overwrite the new disk with a magic number
	__verify_segment_read(sys, src, nblocks, magic_wipe8);
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);

	// ------------------------------------ Move the segment back (hot relocation) from degraded mode
	tomaSimulator_unreg_raid1(r1uuid(src->tpr), seg0->stripe_index);
	io_toggle = nt->dbg_num_enabling_io_toggles;						// Unknown, yet positive amount of toggles due to unreg
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, seg0->dlba_start);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_DEAD);
	__verify_segment_write(sys, src, nblocks);							// Test that it did move back by performing write on the new location of segment
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a hot transition
	return rv;
}

int __test_segment_relocation_forward_disk_rediscovery_warm_backward_from_degraded_mode(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange){
	int rv = -1;
	const struct nvmeibc_topologies *nt = &(sys->clients[0].devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	struct disk_range *seg0 = &src->cpr[0];
	const int src_node = src->cpr[src->vsi.segment].node_id;

	const u8  magic_wipe1 = 'r';
	const u64 magic_wipe8 = 0x7272727272727272; //hex(ord('r')) == 0x72
	const int nblocks = 1;									// Length of IO

	// ------------------------------------ Relocate first segment of Vol0, from disk 0 to new 6'th disk in warm fashion (test disk redescovery)
	tomaSimulator_switchTopo(r1uuid(src->tpr), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);		// Optional. Must be present for HOT relocation
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, seg0->dlba_start);

	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	NVMeshSystem__invoke_pause_cont_on_disk(sys,seg0[1].node_id);				// Toma cut the client. Client got pause and continue on the RW disk
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W_NO_DIRTY);

	// Test that it did move by performing read on newly wiped disk
	ramDiskSimulator_wipe(&sys->servers[dst_drange->node_id].ramDisk, magic_wipe1);		// Overwrite the new disk with a magic number
	__verify_segment_read(sys, src, nblocks, magic_wipe8);
	BUG_ON(++io_toggle != nt->dbg_num_enabling_io_toggles);

	// ------------------------------------ Move the segment back (hot relocation) from degraded mode
	tomaSimulator_unreg_raid1(r1uuid(src->tpr), seg0->stripe_index);
	io_toggle = nt->dbg_num_enabling_io_toggles;						// Unknown, yet positive amount of toggles due to unreg
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, seg0->dlba_start);
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_DEAD);
	// Test that it did move back by performing write on the new location of segment
	__verify_segment_write(sys, src, nblocks);
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);				// Verify this is a hot transition
	return rv;
}

int __test_segment_relocation_with_segment_shift(struct NVMeshSystem *sys, struct TstPRaid* src, struct disk_range_info* dst_drange){
	int rv = -1;
	struct clientSimulator *client = &sys->clients[0];
	const struct nvmeibc_topologies *nt = &(client->devs[src->vsi.volume]->topologies);
	u64 io_toggle = nt->dbg_num_enabling_io_toggles;
	struct disk_range *seg0 = &src->cpr[0];

	const int src_node = src->cpr[src->vsi.segment].node_id;
	const int nblocks = 1;									// Length of IO
	const u64 addr_shift = 64;

	// ------------------------------------ Relocate first segment of Vol0, from disk 0 to the same 0'th disk with shift in address
	tomaSimulator_switchTopo(r1uuid(src->tpr), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);		// Optional. Must be present for HOT relocation
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, seg0->node_id, seg0->dlba_start+addr_shift);	// No deprecated disks, nor segments	// Move the segment down
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W);
	__verify_segment_write(sys, src, nblocks);									// Test that it did move back by performing write on the new location of segment
	BUG_ON(io_toggle != nt->dbg_num_enabling_io_toggles);

	// ------------------------------------ Move the segment back (hot relocation) from degraded mode
	tomaSimulator_unreg_raid1(r1uuid(src->tpr), seg0->stripe_index);
	io_toggle = nt->dbg_num_enabling_io_toggles;						// Unknown, yet positive amount of toggles due to unreg
	mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, seg0->node_id, seg0->dlba_start-addr_shift);		// Move the segment up
	rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);
	__wait_for_topology_change(client->devs[src->vsi.volume]);					// wait until r1 has registered again (after unregistering) before switchTopo
	NVMeshSystem_serialize(sys);
	//tomaSimulator_switchTopo(r1uuid(src->tpr), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW);
	tomaSimulator_switchTopoBlocked(client->devs[src->vsi.volume], r1uuid(src->tpr), NVMEIBTC_DS_MODE_W   , NVMEIBTC_DS_MODE_RW);
	clientSimulator_wait_for_io_enabled_for_vol(client, src->vsi.volume, false);		// Unlike usual segment relocation, Here the unregister acks tomaSimulator_unreg_raid1() may have not be send before the switch topo above and all hell breaks loose. Wait syncronously for client to reregister to the entire volume. This use case tests client's warming on switch topo, that is why I (daniel) let it exist instead of waiting for previous unregister to finish before calling switch topo!
	__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_W_NO_DIRTY);
	__verify_segment_write(sys, src, nblocks);									// Test that it did move back by performing write on the new location of segment
	if (io_toggle != nt->dbg_num_enabling_io_toggles) _Emerg("unitest bug: %llu!=%llu\n", io_toggle, nt->dbg_num_enabling_io_toggles);				// Verify this is a hot transition

	// ------------------------------------ Test detach during relocation of Vol0, As if, Relocate first segment of Vol0, from disk 0 to new 6't
	if (true){
		tomaSimulator_switchTopoBlocked(client->devs[src->vsi.volume], r1uuid(src->tpr), NVMEIBTC_DS_MODE_DEAD, NVMEIBTC_DS_MODE_RW);  // On seg relocation old seg should be dead.
		mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, dst_drange->node_id, seg0->dlba_start);
		rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);

		send_command_to_vol(sys, -1, src->vsi.volume, volCmds_Detach);
		mongo_db_simu_move_disk_range(&sys->mdb, &sys->tcf, src, src_node, seg0->dlba_start);		// Revert the configuration back
		rv = send_command_to_vol_attach_or_update(sys, -1, src->vsi.volume);  REPORT_ERROR(rv);	// Update will actually reattach the volume with the new configuration

		// Now restore topology to RW
		__wait_for_topology_change(client->devs[src->vsi.volume]);					// wait until r1 has registered again (after unregistering) before switchTopo
		NVMeshSystem_serialize(sys);
		__switch_segment_topos(sys, src, NVMEIBTC_DS_MODE_RW);
		__verify_segment_write(sys, src, nblocks);									// Test that it did move by performing write on the new location of segment
	}
	return rv;
}

/* Segments relocation unitest*/
TEST_FUNC int unitest_segment_relocation(struct NVMeshSystem *sys){
	typedef struct TstPRaid TestPRaid;
	int rv = -1;

	if(force_reconf_reboot == true){
		return 0;
	}

	{
		const struct volume_segment_index vsi =           {.volume=3, .chunk=1, .raid=0, .segment=1};
		const struct volume_segment_index vsi_unrelated = {.volume=3, .chunk=0, .raid=0, .segment=1};
		//--------------------------------^^^^^^^^^^^^^^^^^^^^^^ the volume should be identical, but other raid

		TestPRaid src_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);
		TestPRaid unrelated_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi_unrelated);
		TestPRaid dst_raid = NVMeshSystem_pick_other_volume_disk_range(sys, &src_raid);

		struct disk_range_info dst_drange = {.node_id= dst_raid.cpr[dst_raid.vsi.segment].node_id, .dlba_start = dst_raid.cpr[dst_raid.vsi.segment].dlba_start};
		const int pause_cont_node = src_raid.cpr->node_id;
		BUG_ON(vsi.volume != vsi_unrelated.volume);

		// ------------------------------------ Relocate last segment normally (hot relocation).
		rv |= __test_segment_relocation_hot(sys, &src_raid, &dst_drange);
		// ------------------------------------ Relocate last segment normally (hot relocation), but induce topo change while previous switch topo was not applied yet.
		rv |= __test_segment_relocation_hot_with_induced_topo_changes(sys, &src_raid, &dst_drange, &unrelated_raid);
		// ------------------------------------ Relocate forward again, management sends by mistake a few copies of the same reconfs
		rv |= __test_segment_relocation_forward_reconfigure_twice_backward_client_suicide(sys, &src_raid, &dst_drange);
		// ------------------------------------ Relocate forward again, Toma delivers reconf via registrable message, before client has the configuration
		rv |= __test_segment_relocation_via_registrable_forward_toma_first_backward_client_first(sys, &src_raid, &dst_drange, pause_cont_node);
		// ------------------------------------ Relocate forward again, Toma delivers reconf via registrable message, client conf version ahead of toma */
		rv |= __test_segment_relocation_via_registrable_client_ahead(sys, &src_raid, &dst_drange, pause_cont_node);
		// ------------------------------------ Relocate last segment Toma gets conf long before client, with random pause on destination disk.
		rv |= __test_segment_relocation_forward_toma_client_delayed(sys, &src_raid, &dst_drange, pause_cont_node);

		mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, src_raid.vsi.volume);							// Clear remaining data from seg1 relocation test
		// ------------------------------------ Void segment relocation (Configuration with zero changes). Nothing should happen
		rv = send_command_to_vol_attach_or_update(sys, -1, src_raid.vsi.volume);  REPORT_ERROR(rv);
	}
	{
		const struct volume_segment_index vsi =     {.volume=0, .chunk=0, .raid=0, .segment=0};
		TestPRaid src_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);
		struct disk_range_info dst_drange = {.node_id=20, .dlba_start=sys->servers[20].ramDisk.committed_addr.block};
		rv |= __test_segment_relocation_forward_disk_rediscovery_hot_backward_from_degraded_mode(sys, &src_raid, &dst_drange);
		rv |= __test_segment_relocation_forward_disk_rediscovery_warm_backward_from_degraded_mode(sys, &src_raid, &dst_drange);
		rv |= __test_segment_relocation_with_segment_shift(sys, &src_raid, &dst_drange);

		mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, vsi.volume);
	}
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return rv;
}

/* Multiple Segments relocation unitest*/
TEST_FUNC int unitest_segment_relocation_multi(struct NVMeshSystem *sys){
	int rv = -1, i, _try;
	int volInd 			 = 3;									// Volume which will be relocated
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	const int lenBlocks = __from4K(2);							// Length of IO
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8 *dst = NULL, *mem = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	struct tTopoOfPraid *cr1[2] = {NULL, NULL};					// The raid1's of chunks 0 and 1 in which we are relocating a segment
	int	node_id, seg_ind;

	// ------------------------------------ Relocate 2 segments of Vol3 to collide with a segment of other volumes.
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client
	struct volumeDescriptor   *vol 		 = &sys->mdb.vols[volInd];	// Move segment of third volume
	const struct nvmeibc_block_device* bdev = client->devs[volInd];
	struct nvmeibc_block_disk *disks 	 = sys->mdb.discs[volInd];// Disks of the volume
	struct disk_range *oSeg[2] = {&sys->mdb.vols[1].segs[2], &sys->mdb.vols[2].segs[1]};
	struct disk_range *seg[ 2] = {        &vol->segs[1],         &vol->segs[3]};
	int srcNode[2] = { seg[0]->node_id,  seg[1]->node_id};
	int dstNode[2] = {oSeg[0]->node_id, oSeg[1]->node_id}; 		// Source and destination disks [2,3,4,5] -> [2,1,4,3]

	for (_try=0; _try<3; _try++) {
		// ------------------------------------ Relocate segments normally (hot relocation).
		i=0; cr1[i] = &sys->tcf.vols[volInd].chunks[i].raids[0];
		i=1; cr1[i] = &sys->tcf.vols[volInd].chunks[i].raids[0];
		i=0; tomaSimulator_switchTopo(r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK);
		i=1; tomaSimulator_switchTopo(r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK);
		mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, 2, seg, 1, &seg[1]->node_id);
		i=0; mongo_db_simu_reconf_set_new(disks,srcNode[i],dstNode[i],seg[i], oSeg[i]->dlba_start);
		i=1; mongo_db_simu_reconf_set_new(disks,srcNode[i],dstNode[i],seg[i], oSeg[i]->dlba_start);
		i=0; mongo_db_simu_reconf_tell_toma(cr1[i], seg[i], false, sys->tcf.vols[volInd].locks_scheme);
		i=1; mongo_db_simu_reconf_tell_toma(cr1[i], seg[i], false, sys->tcf.vols[volInd].locks_scheme);
		rv = send_command_to_vol_attach_or_update(sys, -1, volInd);  REPORT_ERROR(rv);

		for (i=0; i<2; i++) {
			tomaSimulator_switchTopoBlocked(bdev, r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD);
			tomaSimulator_switchTopo(             r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W         , SW_TOPO__WAIT_ACK);
			tomaSimulator_switchTopo(             r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W_NO_DIRTY, SW_TOPO__WAIT_ACK);
			tomaSimulator_switchTopoBlocked(bdev, r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		// Test that it did move by performing write on the new location
		for (i=0; i<2; i++) {
			magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
			rv = osSimulator_writeArr(&client->OS, volInd, __from4K(seg[i]->bd_start), lenBlocks, mem);		REPORT_ERROR(rv);
			clientSimulator_wait_for_all_bio_ops(client);
			memset(mem, 0   	, memSize);								// Clear the array
			dst = physSegStartPtr(oSeg[i]);
			__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);				// This disk should be written
		}

		// ------------------------------------ Move the segment back, fallback to warm  (forced by update configuration arriving while previous update is still being processed)
		i=0; tomaSimulator_switchTopo(r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK);
		i=1; tomaSimulator_switchTopo(r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD, SW_TOPO__WAIT_ACK);
		mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, 2, seg, 1, &seg[0]->node_id);
		i=1; mongo_db_simu_reconf_set_new(disks,dstNode[i],srcNode[i],seg[i], seg[i][-1].dlba_start);
		i=0; mongo_db_simu_reconf_set_new(disks,dstNode[i],srcNode[i],seg[i], seg[i][-1].dlba_start);
		i=0; mongo_db_simu_reconf_tell_toma(cr1[i], seg[i], false, sys->tcf.vols[volInd].locks_scheme);
		i=1; mongo_db_simu_reconf_tell_toma(cr1[i], seg[i], false, sys->tcf.vols[volInd].locks_scheme);
		rv = send_command_to_vol_attach_or_update(sys, -1, volInd);  REPORT_ERROR(rv);	__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		rv = send_command_to_vol_attach_or_update(sys, -1, volInd);  REPORT_ERROR(rv);	// Cause warm fallback by sending the same configuration (generating erroneous state)
		rv = send_command_to_vol_attach_or_update(sys, -1, volInd);  REPORT_ERROR(rv);	__unitest_volume_config_version_inc(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd]);
		rv = send_command_to_vol_attach_or_update(sys, -1, volInd);  REPORT_ERROR(rv);	// Simulate unneded repetitions of the update message.
		rv = send_command_to_vol_attach_or_update(sys, -1, volInd);  REPORT_ERROR(rv);
		for (seg_ind=0; seg_ind < vol->nSegments; seg_ind++) {
			node_id = vol->segs[seg_ind].node_id;
			tomaSimulator_send_registrables(&sys->servers[node_id].simToma);	// Toma's that sent REGISTRABLE msgs will now trigger registration.
			_NT(trace_bunitest_unitest_segment_relocation_multi, "Wait for registraion of Toma @NODE_ID...", node_id);
			tomaSimulator_waitProtoEnd(&sys->servers[node_id].simToma);			// wait for REGISTRABLE msg
		}
		for (i=0; i<2; i++) {
			tomaSimulator_switchTopoBlocked(bdev, r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD);
			tomaSimulator_switchTopo(             r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W         , SW_TOPO__WAIT_ACK);
			tomaSimulator_switchTopo(             r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W_NO_DIRTY, SW_TOPO__WAIT_ACK);
			tomaSimulator_switchTopoBlocked(bdev, r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}

	mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, volInd);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	sim_kfree(mem);
	_NI_dmesg(trace_1_bunitest_unitest_segment_relocation_multi, "*************** (@VOL_I) end", volInd);
	return rv;
}


TEST_FUNC int unitest_segment_relocation_ec(struct NVMeshSystem *sys){
	/**
	 * Please refer to @Segment UUID explained
	 * Comment at the top of the file nvmeibm_conf_db.c
	 */
	typedef struct TstPRaid TestPRaid;
	//struct clientSimulator *client = &sys->clients[0];
	int rv = 0;

	const int pause_cont_node = 9;
	const struct volume_segment_index vsi_dst = {.volume=1, .chunk=0, .raid=0, .segment=0};
	TestPRaid dst_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi_dst);
	struct disk_range_info dst_drange = {.node_id= dst_raid.cpr[dst_raid.vsi.segment].node_id, .dlba_start = dst_raid.cpr[dst_raid.vsi.segment].dlba_start};

	bool const force_reconf_reboot_orig = force_reconf_reboot;													// In unitest allow testing of hot/warm reconfigurations, even if in real system this is disabled
	force_reconf_reboot = true;

	{
		const struct volume_segment_index vsi = {    .volume=0, .chunk=0, .raid=0, .segment=0};
		TestPRaid src_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);
		//There is a need to prepare src and dst segments before replacement, regardless the direction.
		//Right now, TOMA is not envolved, so we read junk from the segment and blow up on incorrect edic
		NVMeshSystem_all_clients_ec_edic(sys, false);
		// ------------------------------------ Relocate last segment normally (hot relocation).
		_NI(seg_reloc_ec_2866, "__test_segment_relocation_hot ...");
		if( force_reconf_reboot == false ){
			rv |= __test_segment_relocation_hot(sys, &src_raid, &dst_drange);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		_NI(seg_reloc_ec_2868, "__test_segment_relocation_hot done");
		// ------------------------------------ Relocate last segment normally (hot relocation), but induce topo change while previous switch topo was not applied yet.
		// skipped, since it requires 2 volumes and not capable to work with just 2 disk_ranges
		{
			struct disk_range_info dr = {.node_id = 21, .dlba_start = sys->servers[21].ramDisk.committed_addr.block};
			_NI(seg_reloc_ec_2873, "__test_segment_relocation_hot_with_induced_topo_changes ...");

			if( force_reconf_reboot == false ){
				rv |= __test_segment_relocation_hot_with_induced_topo_changes(sys, &src_raid, &dr, NULL);
			}
			BUG_ON(!NVMeshSystem_is_stable(sys));
			_NI(seg_reloc_ec_2875, "__test_segment_relocation_hot_with_induced_topo_changes done");
		}
		// ------------------------------------ Relocate forward again, management sends by mistake a few copies of the same reconfs
		_NI(seg_reloc_ec_2878, "__test_segment_relocation_forward_reconfigure_twice_backward_client_suicide ...");
		if (force_reconf_reboot == false){
			rv |= __test_segment_relocation_forward_reconfigure_twice_backward_client_suicide(sys, &src_raid, &dst_drange);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		_NI(seg_reloc_ec_2880, "__test_segment_relocation_forward_reconfigure_twice_backward_client_suicide done");
		// ------------------------------------ Relocate forward again, Toma delivers reconf via registrable message, before client has the configuration
		_NI(seg_reloc_ec_2882, "__test_segment_relocation_via_registrable_forward_toma_first_backward_client_first ...");
		rv |= __test_segment_relocation_via_registrable_forward_toma_first_backward_client_first(sys, &src_raid, &dst_drange, pause_cont_node);
		BUG_ON(!NVMeshSystem_is_stable(sys));
		// ------------------------------------ Relocate last segment Toma gets conf long before client, with random pause on destination disk.
		_NI(seg_reloc_ec_2885, "__test_segment_relocation_via_registrable_forward_toma_first_backward_client_first done");
	}
	{
		const struct volume_segment_index vsi = {    .volume=0, .chunk=0, .raid=0, .segment=1};
		TestPRaid src_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);
		struct disk_range_info dr = {.node_id = 21, .dlba_start = sys->servers[21].ramDisk.committed_addr.block};
		_NI(seg_reloc_ec_2888, "__test_segment_relocation_forward_toma_client_delayed ...");
		if (force_reconf_reboot == false){
			rv |= __test_segment_relocation_forward_toma_client_delayed(sys, &src_raid, &dr, pause_cont_node);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		_NI(seg_reloc_ec_2893, "__test_segment_relocation_forward_toma_client_delayed done");
	}
	{
		const struct volume_segment_index vsi = {.volume=0, .chunk=0, .raid=0, .segment=0};
		TestPRaid src_raid = NVMeshSystem_TstPRaid_init_rel(sys, vsi);
		_NI(seg_reloc_ec_2898, "__test_segment_relocation_forward_disk_rediscovery_hot_backward_from_degraded_mode ...");

		if( force_reconf_reboot == false ){
			rv |= __test_segment_relocation_forward_disk_rediscovery_hot_backward_from_degraded_mode(sys, &src_raid, &dst_drange);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		_NI(seg_reloc_ec_2900, "__test_segment_relocation_forward_disk_rediscovery_hot_backward_from_degraded_mode done");
		_NI(seg_reloc_ec_2901, "__test_segment_relocation_forward_disk_rediscovery_warm_backward_from_degraded_mode ...");

		if( force_reconf_reboot == false ){
			rv |= __test_segment_relocation_forward_disk_rediscovery_warm_backward_from_degraded_mode(sys, &src_raid, &dst_drange);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		_NI(seg_reloc_ec_2903, "__test_segment_relocation_forward_disk_rediscovery_warm_backward_from_degraded_mode done");
		_NI(seg_reloc_ec_2904, "__test_segment_relocation_with_segment_shift ...");
		if (force_reconf_reboot == false){
			rv |= __test_segment_relocation_with_segment_shift(sys, &src_raid, &dst_drange);
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
		_NI(seg_reloc_ec_2906, "__test_segment_relocation_with_segment_shift done");
		mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, vsi.volume);
	}
	force_reconf_reboot = force_reconf_reboot_orig;
	// Wipe EC volume to clean up EDIC and data discrepancies with DEGUB_DI
	NVMeshSystem_all_clients_ec_edic(sys, true);
	NVMeshSystem_wipe_all_md_of_disks(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return rv;
}

/*	Volume downgrade: Toma sends to client
    	SwitchTopo(nSegs=2,{RW,DEAD} or {DEAD,RW}) - Entering Degraded mode. Important to allow client warm transition.
    	SwitchTopo(nSegs=1,{RW,DEAD}) - Switch to non mirrored raid1. Once in this mode, client will stop locking, even though there are other clients which haven't switch topo and are still locking both
	Alternatively:
    	SwitchTopo(nSegs=2,{RW,W}) 	- Dual lock where the RW is the owner
		SwitchTopo(nSegs=1,{RW,DEAD}) - Switch to non mirrored raid1. Once in this mode, client will stop locking,

	Volume Upgrade:	Toma sends to client
		SwitchTopo(nSegs=2,{RW,DEAD}) - Upgrade to degraded mirrored (Owner lock on live segment). Client will start locking after switch topo
		Regular messages to return from degraded mode
*/
/* Toma orders client to enter into degraded mode. Can be done via unregister or switch topo message. Test both cases*/
static void __unitest_DUV_toma_send_downgrade_switch_topos(int n_raid1s, struct tTopoOfPraid *cr1[/*n_raid1s*/], int rem_seg_ind[/*n_raid1s*/], bool wait_for_switch_ack) {
	const struct switch_topo_options opts = (wait_for_switch_ack ? SW_TOPO__WAIT_ACK : SW_TOPO__NONE);
	int i;
	enum NVMEIBTC_DS_MODE acms[] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD};
	for (i=0; i<n_raid1s; i++) {
		const int ind_of_live = (rem_seg_ind) ? (rem_seg_ind[i]%2) : 0; 	// If Entering degraded mode, send DEAD to the correct segment. But when downgraded, always the second is DEAD
		const int ind_of_dead = ind_of_live^1;
		const bool force_switch_topo = (ind_of_dead >= cr1[i]->header.n_segments); // Simulator does not support dead seg sending unreg
		if ((!rem_seg_ind)||force_switch_topo) 								// Fictitious condition, Sometimes use switch topo
			 tomaSimulator_switchTopo( r1uuid(cr1[i]), acms[ind_of_live], acms[ind_of_dead], opts);
		else tomaSimulator_unreg_raid1(r1uuid(cr1[i]), ind_of_dead);	// In other cases use unregister message
	}
}

static void __unitest_DUV_toma_send_upgrade_switch_topos(int n_raid1s, struct tTopoOfPraid *cr1[/*n_raid1s*/], int rem_seg_ind[/*n_raid1s*/], bool toma_before_client, struct volumeDescriptor *vol, struct NVMeshSystem *sys) {
	const bool wait_for_switch_topo_ack = !toma_before_client;
	const struct switch_topo_options opts = (wait_for_switch_topo_ack ? SW_TOPO__WAIT_ACK : SW_TOPO__NONE);
	int i;
	enum NVMEIBTC_DS_MODE acms[] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_DEAD};
	enum NVMEIBTC_DS_MODE acm1[] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W};
	enum NVMEIBTC_DS_MODE acmd[] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W_NO_DIRTY};
	for (i=0; i<n_raid1s; i++) {
		int ind_of_live  = rem_seg_ind[i]%2;
		int dead_toma_id = vol->segs[rem_seg_ind[i]^1].node_id;
		tomaSimulator_switchTopo(r1uuid(cr1[i]), acms[ind_of_live]	, acms[ind_of_live^1], opts);
		if (toma_before_client) tomaSimulator_disconnect(&sys->servers[dead_toma_id].simToma);
		tomaSimulator_switchTopo(r1uuid(cr1[i]), acm1[ind_of_live]	, acm1[ind_of_live^1], opts);
		tomaSimulator_switchTopo(r1uuid(cr1[i]), acm1[ind_of_live]  , acmd[ind_of_live^1], opts);
		tomaSimulator_switchTopo(r1uuid(cr1[i]), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, opts);
		if (toma_before_client) tomaSimulator_reconnect(&sys->servers[dead_toma_id].simToma);
	}
}

/* Convert JBOD to RAID 1 and back, RAID0 to RAID10 and back.
   EC-1901: Todo: only_warm - change to only hot (it is used as 1 time upgrade instead of test all possible upgrades) */
TEST_FUNC int __unitest_downgrade_upgrade_raid(struct NVMeshSystem *sys, int volInd, bool only_warm){
	const int   	lenBlocks = __from4K(7);					// Length of IO
	int  rv = -1, i, _try;
    u64  magic_pattern;
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8 *dst = NULL, *mem = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client

	// ------------------------------------ Downgrade volume in cold fashion (dettach/attach), thus converting it to Raid1->JBOD and back, Raid10->Raid0 and back.
	struct volumeDescriptor   *vol 		 = &sys->mdb.vols[volInd];	// Move segment of third volume
	struct volumeDescriptor   backup_vol = *vol;				// Save the volume so we will be able to restore it.
	struct nvmeibc_block_disk *disks 	 = sys->mdb.discs[volInd];// Disks of the volume
	struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];			// Toma Configuration of the current volume
	const int n_nonmirrored_segs			 = vol->nSegments/2;
	const int n_tries						 = (1<<n_nonmirrored_segs);	// Amount of combinations to downgrade Raid1 to JBOD.
	__unitest_updowngrade_mode mode, m_start, m_end;
	BUG_ON(n_nonmirrored_segs>3);								// We will use constant of 3 to define arrays on stack for easiser debugging
	if (only_warm) {
		m_end = m_start = UNITEST_UPDOWNGRADE_WARM;
	} else {
		m_start = UNITEST_UPDOWNGRADE_COLD;
		m_end = UNITEST_UPDOWNGRADE_HOT_IO - 2;					// EC-1901: Daniel, disabled HOT_IO coz it is not really supported anyways for upgrade/downgrade
	}
	for (u32 mode__ = m_start; mode__ <= m_end; mode__++) {
	mode = (__unitest_updowngrade_mode)mode__;
	for (_try = 0; _try < n_tries; _try++) {                        // Example volume has 2x2 segments, downgrade/upgrade to every combination: {0,2},{1,2},{0,3},{1,3}
		struct nvmeibc_block_device *dev = client->devs[volInd];
		const u64 dbg_num_enabling_io_toggles = dev->topologies.dbg_num_enabling_io_toggles;
		int rem_seg_ind[3] = {0}, del_seg_ind[3] = {0};
		struct disk_range *rem_segs[3] = {NULL}, *del_segs[3] = {NULL};
		u32 deprecDisks[3] = {0};
		struct tTopoOfPraid *cr1[3] = {NULL};					// The raid1's of the volume (each raid1 is downgraded to a single segment)
		for (i=0; i<n_nonmirrored_segs; i++) {					// Create unique combination of remaining segments as function of '_try'
			rem_seg_ind[i] = (i<<1)^((_try>>i)&0x1);			// The indices of the remaining segments
			del_seg_ind[i] = rem_seg_ind[i]^1;					// The complementary deleted segments.
			rem_segs[	i] = &vol->segs[rem_seg_ind[i]];
			del_segs[	i] = &vol->segs[del_seg_ind[i]];
			deprecDisks[i] = del_segs[i]->node_id;
			cr1[i] = tTopoOfVolume_getRaid1(cfv, i);
		}
		if (mode == UNITEST_UPDOWNGRADE_COLD) {
			send_command_to_vol(sys, -1, volInd, volCmds_Detach);
		} else {
			// Always sent by toma but for this message is not crucial. Pseudo randomly send it. For HOT IO don't send it to test hybrid mode io
			if ((mode != UNITEST_UPDOWNGRADE_HOT_IO)&&(_try%3))
				__unitest_DUV_toma_send_downgrade_switch_topos(n_nonmirrored_segs, cr1, rem_seg_ind, true);
		}

		vol->nSegments = n_nonmirrored_segs;					// Generate an unmirrored volume
		vol->segs = (struct disk_range*)sim_kmalloc(sizeof(*vol->segs)*vol->nSegments, GFP_KERNEL);
		mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, n_nonmirrored_segs, del_segs, n_nonmirrored_segs, deprecDisks);
		for (i=0; i<vol->nSegments; i++) {
			vol->segs[i] = *rem_segs[i];
			vol->segs[i].replicas = 1;
			vol->segs[i].stripe_index/= 2;
		}
		for (i=0; i<vol->nDepreSegments; i++) {
			vol->depre_segs[i].replicas = 1;
			vol->depre_segs[i].stripe_index/= 2;
		}
		for (i=0; i<n_nonmirrored_segs; i++) {
			mongo_db_simu_reconf_set_new(disks, deprecDisks[i], -1, NULL, -1);
			mongo_db_simu_reconf_tell_toma(cr1[i], del_segs[i], true, cfv->locks_scheme);
		}

		// If Toma got reconf before client it will send downgrade switch topos. Client will miss them but mark that toma requested the transition. Else Client gets before toma
		if (mode == UNITEST_UPDOWNGRADE_WARM)
			__unitest_DUV_toma_send_downgrade_switch_topos(n_nonmirrored_segs, cr1, NULL, false);
		if (mode == UNITEST_UPDOWNGRADE_COLD)  {
			send_command_to_vol_attach_or_update(sys, -1, volInd);
		} else {
			clientSimulator_wait_for_io_toggle_init(client, volInd);
			send_command_to_vol_attach_or_update(sys, -1, volInd);
			clientSimulator_wait_for_io_toggle_wait(client, volInd);
		}
		clientSimulator_wait_for_io_enabled_for_vol(client, volInd, false);		// LKJ: This is a hack! Daniel: IO enabled to have a toma which can send switch topo.
		if (mode == UNITEST_UPDOWNGRADE_HOT){
			BUG_ON(dbg_num_enabling_io_toggles != dev->topologies.dbg_num_enabling_io_toggles);
			__unitest_DUV_toma_send_downgrade_switch_topos(n_nonmirrored_segs, cr1, NULL, true);
		}
		if (mode == UNITEST_UPDOWNGRADE_HOT_IO){						// Test IO on hybrid volume
			for (i=0; i<n_nonmirrored_segs-1; i++) {
				struct disk_range *ds = &vol->segs[i];
				u64 startBlock = disk_range_get_start_addr(ds);
				u64 withinFirstStripe  = __from4K(__to4K(lenBlocks)/2);	// Amount of blocks in the first stripe
				u64 withinSecondStripe = lenBlocks - withinFirstStripe;	// Amount of blocks in the next  stripe
				u64 stripesCrossingAddr= __from4K((ds->stripe_width>1) ? ds->stripe_size : ds->length);
				u64 off = __to4K(stripesCrossingAddr - withinFirstStripe);		// IO catches 2 stripes

				__unitest_DUV_toma_send_downgrade_switch_topos(1, cr1+i, NULL, true);			// i'th raid is downgraded
				// Stripe of this raid1 should be written once, verify it
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
				rv = osSimulator_writeArr(&client->OS, volInd, 	__from4K(startBlock+off), lenBlocks, mem);		REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				dst = physSegStartPtr_off(rem_segs[i],off);
				__unitest_verify_blocks_pattern(dst, withinFirstStripe, magic_pattern, true);	// This disk should be written
				dst = physSegStartPtr_off(del_segs[i],off);
				BUG_ON(*(u64*)dst == magic_pattern);

				// Next stripe should be written twice (because downgrading switch topo haven't arrived yet)
				dst = physSegStartPtr(rem_segs[i+1]);
				__unitest_verify_blocks_pattern(dst, withinSecondStripe, magic_pattern, true);// This disk should be written
				dst = physSegStartPtr(del_segs[i+1]);
				__unitest_verify_blocks_pattern(dst, withinSecondStripe, magic_pattern, true);	// This disk should be written
			}
			__unitest_DUV_toma_send_downgrade_switch_topos(1, cr1+i, NULL, true);			// i'th raid is downgraded
		}
		NVMeshSystem_serialize(sys);	// Needed for sure: Finish async toma messages
		// Test that volume was downgraded by verifying write on the remaining segments and no write on the deleted segments
		if (!only_warm) {
			for (i=0; i<n_nonmirrored_segs; i++) {
				struct disk_range *ds = &vol->segs[i];
				const u64 startBlock = disk_range_get_start_addr(ds);
				const u64 off = 1; // +1 offset to avoid boundary problems of verification
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
				rv = osSimulator_writeArr(&client->OS, volInd, 	__from4K(startBlock+off), lenBlocks, mem);		REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				dst = physSegStartPtr_off(rem_segs[i], off);
				__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);// This disk should be written
				dst = physSegStartPtr(del_segs[i]);
				BUG_ON(*(u64*)dst == magic_pattern);
			}
		}

		// Upgrade the volume back. Do a trick: Instead of reconstructing 'vol' back (like: vol->segs[i].stripe_index*= 2), just use the backup of the original.
		if (mode == UNITEST_UPDOWNGRADE_COLD)
			send_command_to_vol(sys, -1, volInd, volCmds_Detach);
		mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, 0, NULL, 0, NULL);
		volumeDescriptor_free(vol);
		backup_vol.info.version = vol->info.version;							// Don't forget that version was increased.
		*vol = backup_vol;														// Generate a mirrored volume
		for (i=0; i<n_nonmirrored_segs; i++) {
			mongo_db_simu_reconf_set_new(disks, -1, deprecDisks[i], del_segs[i], del_segs[i]->dlba_start);
			mongo_db_simu_reconf_tell_toma(cr1[i], del_segs[i], true, sys->tcf.vols[volInd].locks_scheme);
		}

		// If Toma got reconf before client it will send upgrade switch topos. Client will not be able to reply, toma will cut it and client will miss switch topos, but mark that toma requested the transition. Else Client gets before toma
		// When upgrading a raid and the old segment is reordered we currently handle "correctly" and we unregister, otherwise we ignore the switch topo request and don't change the remaining segment (no unregister and no ack)
		if (mode == UNITEST_UPDOWNGRADE_WARM){
			__unitest_DUV_toma_send_upgrade_switch_topos(n_nonmirrored_segs, cr1, rem_seg_ind, true, vol, sys);
		}
		send_command_to_vol_attach_or_update(sys, -1, volInd);

		if (mode == UNITEST_UPDOWNGRADE_HOT)  __unitest_DUV_toma_send_upgrade_switch_topos(n_nonmirrored_segs, cr1, rem_seg_ind, false, vol, sys);
		if (mode == UNITEST_UPDOWNGRADE_HOT_IO){						// Test IO on hybrid volume
			/* In the WARM upgrade, we sent the sequence of SwitchTopo at a time in which the client cannot process the msgs bcz it doesnt have a matching config for them. So it droppped these SwitchTopo msgs. when the config arrives later on, the client will register the new segment but will not response to the SwitchTopo & will not do anything on the existing segment. hence, it remains with an older topology. the segment-registeration of the newly added segment will allow the volume to do IO's. however. in the real system, the lack of SwitchTopoAck will take 30 [sec] until Toma timeouts & cuts the client. the client will reconnect & tcontinue IO's. but topoloy changes will halt for that 30 sec? here, we dont have all that so we'll get stuck bcz the SwitchTopo Ack will never arrive. howeber, we can do IO's on the single segment we had all the time, so test can simply continue */
			for (i=0; i<n_nonmirrored_segs-1; i++) {
				struct disk_range *ds = &vol->segs[rem_seg_ind[i]];
				u64 startBlock = disk_range_get_start_addr(ds);
				u64 withinFirstStripe  = __from4K(__to4K(lenBlocks)/2);	// Amount of blocks in the first stripe
				u64 withinSecondStripe = lenBlocks - withinFirstStripe;	// Amount of blocks in the next  stripe
				u64 stripesCrossingAddr= __from4K((ds->stripe_width>1) ? ds->stripe_size : ds->length);
				u64 off = __to4K(stripesCrossingAddr - withinFirstStripe);		// IO catches 2 stripes

				__unitest_DUV_toma_send_upgrade_switch_topos(1, cr1+i, rem_seg_ind+i, false, vol, sys);
				// Stripe of this raid1 should be written twice, verify it
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
				rv = osSimulator_writeArr(&client->OS, volInd, 	__from4K(startBlock+off), lenBlocks, mem);		REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				dst = physSegStartPtr_off(rem_segs[i],off);
				__unitest_verify_blocks_pattern(dst, withinFirstStripe, magic_pattern, true);				// This disk should be written
				dst = physSegStartPtr_off(del_segs[i],off);
				__unitest_verify_blocks_pattern(dst, withinFirstStripe, magic_pattern, true);				// This disk should be written

				// Next stripe should be written once (because upgrading switch topo haven't arrived yet)
				dst = physSegStartPtr(rem_segs[i+1]);
				__unitest_verify_blocks_pattern(dst, withinSecondStripe, magic_pattern, true);				// This disk should be written
				dst = physSegStartPtr(del_segs[i+1]);
				BUG_ON(*(u64*)dst == magic_pattern);
			}
			__unitest_DUV_toma_send_upgrade_switch_topos(1, cr1+i, rem_seg_ind+i, false, vol, sys);
		}
		NVMeshSystem_serialize(sys);	// Needed for sure: Finish async toma messages
		BUG_ON(!NVMeshSystem_is_stable(sys));

		// Test that volume was upgraded by verifying write on all the segments (each raid1 pair)
		if (!only_warm) {
			for (i=0; i<n_nonmirrored_segs; i++) {
				struct disk_range *ds = &vol->segs[i*2];
				const u64 startBlock = disk_range_get_start_addr(ds);
				const u64 off = 1;	// Write in each segments the blocks [1,2,...len]
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);
				rv = osSimulator_writeArr(&client->OS, volInd,__from4K(startBlock+off), lenBlocks, mem);		REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				dst = physSegStartPtr_off(&ds[0], off); __unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
				dst = physSegStartPtr_off(&ds[1], off); __unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
			}
		}

		BUG_ON(!NVMeshSystem_is_stable(sys));
		// When we upgrade a raid and the new segment is added (on the right seg index == 1) we do not do anything on the pre-existing segment (no ack and no unregister) so we must unset the wait bit
	}
	}
	sim_kfree(mem);
	return rv;
}

TEST_FUNC int unitest_downgrade_upgrade_raid(struct NVMeshSystem *sys){
	__unitest_downgrade_upgrade_raid(sys, 0, false);	// Downgrade/Upgrade volume 0, thus converting it Raid10 -> Raid0 and back.
	__unitest_downgrade_upgrade_raid(sys, 3, false);	// Downgrade/Upgrade volume 3, thus converting it Raid1  -> JBOD  and back,
	_NI_dmesg(trace_bunitest_unitest_downgrade_upgrade_raid, "*************** (0,3) end");
	return 0;
}

TEST_FUNC int unitest_expand_shrink_volume_by_1_chunk(struct NVMeshSystem *sys){
	static int    vol_id_pair[MAX_NORMAL_VOLUMES_IN_NVMESH] = {1, 0, -1, -1}; // index k for the k-th volId indicate the paired volume to be removd. -1 implies no pair
	int rv = -1, i, j;
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	const int lenBlocks = __from4K(7);	        				// Length of IO
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8 *dst = NULL, *mem = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client

	// ------------------------------------ Expand/Shrink each volume by a single chunk.
	int volInd, is_pair_up;
	for (is_pair_up = false; is_pair_up <= true; is_pair_up++) {	// Example: Test resize of vol0, while its pair (vol1) is attached or detached. 2 Tests
	for (volInd=0; volInd<client->nBdevs; volInd++) {
		struct volumeDescriptor   *vol 		 = &sys->mdb.vols[volInd];  // Move segment of third volume
		struct nvmeibc_block_disk *disks 	 = sys->mdb.discs[volInd];// Disks of the volume
		struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];			// Toma Configuration of the current volume
		unsigned long orig_size = client->devs[volInd]->size;
		const bool should_detach_pair_vol = ((!is_pair_up) && (vol_id_pair[volInd] != -1));
		for (u32 mode__ = UNITEST_UPDOWNGRADE_COLD; mode__ <= UNITEST_UPDOWNGRADE_HOT; mode__++) {
			__unitest_updowngrade_mode mode = (__unitest_updowngrade_mode)mode__;
			struct tTopoOfRaid0Chunk* last_chunk = &cfv->chunks[cfv->nChunks-1];
			struct disk_range *del_segs[6] = {NULL};
			unsigned long last_chunk_size = 0;
			int n_del_segs = 0;
			u32 deprecDisks[6] = {0};
			if (should_detach_pair_vol)
				send_command_to_vol(sys, -1, vol_id_pair[volInd], volCmds_Detach);
			_ND(trace_bunitest_unitest_expand_shrink_volume_by_1_chunk, "Shrinking volume (removing last chunk): @VOL_I, nChunks=@N_CHUNKS, orig_size=@ORIG_SIZE", volInd, cfv->nChunks, orig_size);
			// the segments are sorted, so segments of last chunk are the last ones within the volume.
			for (i=0; i<last_chunk->stripeWidth;	i++) { 				// Mark the segments which should be deleted
				struct tTopoOfPraid *r1 = &last_chunk->raids[i];
				for (j=0; j<r1->header.n_segments; j++, n_del_segs++) {
					del_segs[n_del_segs] 	= &vol->segs[vol->nSegments-1-n_del_segs]; //&r1->s[j];
					deprecDisks[n_del_segs] = del_segs[n_del_segs]->node_id;
					_ND(trace_1_bunitest_unitest_expand_shrink_volume_by_1_chunk, "Removing seg: stripe_idx=@STRIPE_IDX, @SEG, disk_id=@DISK_ID, range_size=@RANGE_SIZE", j, del_segs[n_del_segs]->ruuid, deprecDisks[n_del_segs], del_segs[n_del_segs]->length);
				}
				last_chunk_size += del_segs[n_del_segs-1]->length;
			}
			if (mode == UNITEST_UPDOWNGRADE_COLD)
				send_command_to_vol(sys, -1, volInd, volCmds_Detach);
			else {
				// Todo:
			}

			vol->nSegments -= n_del_segs;
			cfv->nChunks--;
			vol->nChunks--;
			mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, n_del_segs, del_segs, n_del_segs, deprecDisks);
			for (i=0; i<n_del_segs; i++)
				mongo_db_simu_reconf_set_new(disks, deprecDisks[i], -1, NULL, -1);
			if (!cfv->nChunks) { // Volume will not attach
				const char* expected_status = ((mode == UNITEST_UPDOWNGRADE_COLD) ? CLI_ATTACH_FAILED : CLI_UPDATE_FAILED);
				reset_cli_status_verification(client);
				set_cli_status_verification_expector(client, failed_attach_string(&client->vols[volInd], expected_status), volInd);
			}
			send_command_to_vol_attach_or_update(sys, -1, volInd);

			// Test that volume was shrinked by verifying write on the remaining segments
			if (cfv->nChunks) {												// Volume was not shrink to zero size
				unsigned long new_size = client->devs[volInd]->size;
				const struct disk_range *last_seg = &vol->segs[vol->nSegments-1];
				const u64 startBlock = new_size-lenBlocks;
				BUG_ON((orig_size-__from4K(last_chunk_size)) != new_size );
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
				rv = osSimulator_writeArr(&client->OS, volInd, 	startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				dst = physSegStartPtr_off(last_seg, last_seg->length-__to4K(lenBlocks));
				__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);				// This disk should be written
			} else {
				cfv->version = (--vol->info.version);	// Reduce the version by -1 because this configuration is illegal and was not applied
			}

			// Expand the volume back
			_ND(trace_2_bunitest_unitest_expand_shrink_volume_by_1_chunk, "Expanding volume (adding last chunk back): @VOL_I, nChunks=@N_CHUNKS", volInd, cfv->nChunks);
			if (mode == UNITEST_UPDOWNGRADE_COLD)
				send_command_to_vol(sys, -1, volInd, volCmds_Detach);
			mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, 0, NULL, 0, NULL);
			vol->nSegments += n_del_segs;
			cfv->nChunks++;
			vol->nChunks++;
			for (i=0; i<n_del_segs; i++)
				mongo_db_simu_reconf_set_new(disks, -1, deprecDisks[i], del_segs[i], del_segs[i]->dlba_start);

			if ((volInd == 3) && (mode == UNITEST_UPDOWNGRADE_HOT) && is_pair_up /* pseudo random */) {
				// When extending a volume 3 in HOT we will get a toma REGISTERABLE while our topology is not in error state but the REGISTERABLE must be on the extending chunk. Test this case
				int t_not_ready = vol->segs[2].node_id;	// Can use Toma 4 or 5 which both bear r1 of second chunk of volume 3
				sys->servers[t_not_ready].simToma.state = tomaState_not_ready;
				send_command_to_vol_attach_or_update(sys, -1, volInd);
				serverSimulator_re_connect(&sys->servers[t_not_ready]);
				NVMeshSystem_serialize(sys);
			} else {
				send_command_to_vol_attach_or_update(sys, -1, volInd);
			}

			// Test that volume was upgraded by verifying write on all the segments (each raid1 pair)
			{
				unsigned long new_size = client->devs[volInd]->size;
				const struct disk_range *last_seg = &vol->segs[vol->nSegments-1];
				const u64 startBlock = new_size-lenBlocks;
				BUG_ON(orig_size != new_size);
				magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
				rv = osSimulator_writeArr(&client->OS, volInd, 	startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				dst = physSegStartPtr_off(last_seg, last_seg->length-__to4K(lenBlocks));
				__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);				// This disk should be written
			}

			if (should_detach_pair_vol){
				__unitest_volume_config_version_inc(&sys->mdb.vols[vol_id_pair[volInd]], &sys->tcf.vols[vol_id_pair[volInd]]);			// Update pair volume with configuration indetical to previous one
				send_command_to_vol_attach_or_update(sys, -1, vol_id_pair[volInd]);
			}
			BUG_ON(!NVMeshSystem_is_stable(sys));
		}
	} // for (volInd=0; volInd<client->nBdevs; volInd++)
	} // for (is_pair_up = false; is_pair_up <= true; is_pair_up++)
	sim_kfree(mem);
	return rv;
}

/* Scenario where client missed few reconfigurations (disconnect from management) and is not able to reconstruct the delta. Invokes volume reboots */
TEST_FUNC int unitest_missing_few_reconfs(struct NVMeshSystem *sys){
	int rv = 0;
	const u32 vol_victim=0;
	struct clientSimulator  *client		= &sys->clients[0];				// Current client
	struct volumeDescriptor *vol 		= &sys->mdb.vols[vol_victim];
	struct tTopoOfVolume	*cfv		= &sys->tcf.vols[vol_victim];		// Toma Configuration of the current volume

	// Save the volume so we will be able to restore it, easily.
	struct volumeDescriptor backup_vol= *vol;
	struct tTopoOfVolume	backup_cfv= *cfv;
	struct nvmeibc_block_disk backup_discs[NVMESH_N_PHYS_DISKS];
	const struct nvmeibc_block_device	*bdev = client->devs[vol_victim];
	const unsigned int orig_size 		      = bdev->size;
	memcpy(&backup_discs[0], sys->mdb.discs[vol_victim], sizeof(backup_discs));

	for (int vol_other=1; vol_other<4; vol_other++) {				// Generate weired configuration by sending vol[i] conf to vol[0]
		{	//client module does not orchestrate volumes configuration changes
			//thus it is possible moved sgmnt will be re-registered without unsubscribing from the other volume
			_NT(trace_bunitest_unitest_missing_few_reconfs, "detach volumn=@VOL_I", vol_other); //interesting what promisses us the segment (un)registration order?
			send_command_to_vol(sys, -1, vol_other, volCmds_Detach); //waits for detach drains
		}

		{
			_NT(trace_1_bunitest_unitest_missing_few_reconfs, "update volumn=@VOL_I", vol_victim); //interesting what promisses us the segment (un)registration order?
			//copies volume i configuration to volInd
			memcpy(sys->mdb.discs[vol_victim], sys->mdb.discs[vol_other], sizeof(backup_discs));
			*vol = sys->mdb.vols[vol_other];
			vol->expect_clnt_ref_ids_to_not_match_mongodb = true; // NVMESH-4797: this test copies confs between volumes with ref-ids so they will not match. Fix the test
			memcpy(cfv,&sys->tcf.vols[vol_other], sizeof(*cfv));
			strcpy(vol->info.devname, backup_vol.info.devname);
			strcpy(vol->info.uuid   , backup_vol.info.uuid);
			sys->tcf.vols[vol_victim].version = vol->info.version = (backup_vol.info.version+6*vol_other);	// As if missed arbitrary 3 or 6 reconfigurations

			// Since segments UUID is unique, temporary detach vol[i], or else Toma will get confused with registering same segment twice
			send_command_to_vol(sys, -1, vol_victim, volCmds_Update); //waits for detach drains
			NVMeshSystem_serialize(sys);
			_NT(trace_2_bunitest_unitest_missing_few_reconfs, "update volumn=@VOL_I - done", vol_victim);
		}

		BUG_ON(orig_size == client->devs[vol_victim]->size);					// Size should have changed
		rv = unitest_IO(client, vol_victim, client->devs[vol_victim]->size-1, 1);	// Test IO to last block
		_NT(trace_3_bunitest_unitest_missing_few_reconfs, "testing IO volumn=@VOL_I - done(rv=@RV)", vol_victim, rv);

		{
			// Revert back vol[0] and reattach vol[i]
			_NT(trace_4_bunitest_unitest_missing_few_reconfs, "restoring attach/update volumn=@VOL_I", vol_victim); //interesting what promisses us the segment (un)registration order?
			memcpy(cfv, &backup_cfv, sizeof(*cfv));
			*vol = backup_vol;
			memcpy(sys->mdb.discs[vol_victim], &backup_discs[0], sizeof(backup_discs));
			sys->tcf.vols[vol_victim].version = vol->info.version = (backup_vol.info.version+6*vol_other+3);	// As if missed arbitrary 3 reconfigurations

			send_command_to_vol(sys, -1, vol_victim, volCmds_Update);
			// wait for older topologies to terminate, the following method expects the volume to already be in an IO enabled state and will print leaking Tomatos in case we aren't there, here we can suppress this warning as we know that it might take a short while to get there as the volume is rebooting (single topology exists, main wq ws drain but the previous topology being freed and puts bottom half of reboot work on main wq).
			clientSimulator_wait_for_io_enabled_for_vol(client, vol_victim, true);
			NVMeshSystem_serialize(sys);
			BUG_ON(orig_size != client->devs[vol_victim]->size);				// Size should have changed back to original size
			_NT(trace_5_bunitest_unitest_missing_few_reconfs, "restoring attach/update volumn=@VOL_I - done", vol_victim); //interesting what promisses us the segment (un)registration order?
		}

		{
			_NT(trace_6_bunitest_unitest_missing_few_reconfs, "restoring volumn=@VOL_I", vol_other); //interesting what promisses us the segment (un)registration order?
			send_command_to_vol(sys, -1, vol_other, volCmds_New); //waits for detach drains
			NVMeshSystem_serialize(sys);
			_NT(trace_7_bunitest_unitest_missing_few_reconfs, "restoring volumn=@VOL_I - done", vol_other); //interesting what promisses us the segment (un)registration order?
		}
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}
	sys->mdb.vols[vol_victim].expect_clnt_ref_ids_to_not_match_mongodb = false; // NVMESH-4797: this test copies confs between volumes with ref-ids so they will not match. Fix the test
	return rv;
}

/* Generate locks pattern for volume 0. Recevies the volume as parameter to make the locks relative to the offset of its segments, in case that segments don't start at the begginig of disk */
static int __unitest_TrimSplit_genLockPattern(struct NVMeshSystem* sys, int pat_ind, bool do_lock){
	struct volumeDescriptor *vol = NULL;
	u64 i, l, d, nLocks = (384*2)/LOCKSET_4KS, max_n_owners, nDisks  = 6;// 384*2 because it is mirrored, 6 disks participate in locks in 2-mirrored or n-mirrored volumes
	#define T_PAT   (24*3)												// We ahve 3 testing patterns, each with 24 locks
	// ' ' - unlocked. 'O' - owner taken, 'A' - active taken, 'B' - both are taken
	//					Disks		D0  D1  D2  D3  D4  D5				// For n, mirrored volumes disks are different
	const char pattern[T_PAT] = {	'O','O',' ',' ',' ',' ',			// First pattern of locks on 6 disks
									' ',' ','O','O',' ',' ',			// 6 commands split to 16
									' ',' ',' ',' ','O','O',
									' ',' ',' ',' ','O','O',
	// Commands (0,1) on disks (0,1) will be split each into 2 commands. {[0..32),[32..128)} on each disk
	// Commands (2,3) on disks (2,3) will be split each into 3 commands. {[0..32),[32..64),[64..128)}
	// Commands (4,5) on disks (4,5) will be split each into 3 commands. {[0..64),[64..96),[96..128)}
									'O','O',' ',' ','O','O',			// Second pattern
									' ',' ',' ',' ',' ',' ',			// 6 commands split to 12
									' ',' ',' ',' ',' ',' ',
									'O','O',' ',' ',' ',' ',
	// Commands (0,1) on disks (0,1) will be split each into 3 commands. {[0..32),[32..96),[96..128)}
	// Commands (2,3) on disks (2,3) will not be split.
	// Commands (4,5) on disks (4,5) will be split each into 2 commands. {[0..32),[32..128)}.
									' ',' ','O','O',' ',' ',			// Third pattern
									' ',' ',' ',' ',' ',' ',			// 6 commands split to 14
									' ',' ','O','O',' ',' ',
									'O','O',' ',' ',' ',' '};
	// Commands (0,1) on disks (0,1) will be split each into 2 commands. {[0..96),[96..128)}.
	// Command  (2,3) on disks (2,3) will be split each into 4 commands (one per lockset).
	// Commands (4,5) on disks (4,5) will not be split.
	const char* pc = &pattern[pat_ind*nLocks];
	if (!sys)
		goto __out;														// Just querying the amount of patterns

	vol = &sys->mdb.vols[0];

	max_n_owners = vol->locks_scheme.maxNOwners;
	for (i=0; i<nLocks; i++, pc++) {
		l = (i/nDisks);
		d = (i%nDisks);													// On 2 mirrored volume those 6 disks hold the locking segments
		if (vol->segs[0].replicas > 2) {
			const u64 seg_dlba_offset = (vol->segs[d].dlba_start - sys->servers[d].ramDisk.committed_addr.block);
			d = (d/max_n_owners)*vol->segs[0].replicas + (d%max_n_owners);	// Convert 'd' into the d'th locking segment
			l += seg_dlba_offset/LOCKSET_4KS;
		}
		if (*pc=='O') {
			if (do_lock)  	ramDiskSimulator_lockDo( &sys->servers[d].ramDisk, l * LOCKSET_4KS + sys->servers[d].ramDisk.committed_addr.block);
			else  			ramDiskSimulator_lockUn( &sys->servers[d].ramDisk, l * LOCKSET_4KS + sys->servers[d].ramDisk.committed_addr.block);
		}
	}
__out:
	return (T_PAT/nLocks);
}

/* Inject failure in various stages of taking lock */
static void __stop_injecting_lock_failures(unsigned long param) {
	NVMeshSystem_gen_cmd_hooks_clean_all_disks((struct NVMeshSystem *)param);
}
TEST_FUNC int unitest_FailureOfLocksOnVol0(struct NVMeshSystem *sys){
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int _try, e, rv = 0, volInd = 0;
	const u64 startBlock = _addr4k(3,31), lenBlocks = _addr4k(3,2);
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	const enum INJECT_TRANSPORT_ERROR errors[]	 = {INJECT_TRANSPORT_ERROR_SEND_OWNLOCK, INJECT_TRANSPORT_ERROR_RELE_OWNLOCK};
	const int n_errors   = ARRAY_SIZE(errors);
	u8        *mem 		 = sim_kmalloc(memSize, GFP_KERNEL);						// Array to read/write to disk
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	struct timer_list timer;									// Timer for disabling injected errors
	enum nvmeib_block_io_op op;
	int num_attempts = (ut_conf__get_base()->is_valgrind) ? 1 : 5;
	u64 n_failed_locks_before = dp_io_stats_get_counter(&client->devs[volInd]->dp.io_stats, DP_IO_STATS_LOCKSET_FAILED);
	u64 n_failed_locks_after = 0;
	__setup_timer(&timer, __stop_injecting_lock_failures, (unsigned long)sys, 0);

	BUG_ON(!NVMeshSystem_is_stable(sys));

	for (_try = 0; _try < num_attempts; _try++) {							// Do - a few iterations
		for (u32 op__ = NVMEIB_BLOCK_IO_OP_WRITE; op__ <= NVMEIB_BLOCK_IO_OP_DISCARD; op__++) {
			op = (enum nvmeib_block_io_op)op__;
			for (e = 0; e < n_errors; e++) {
				struct nvmeibc_disk_hooks disk_hooks = { .args.trerr = { true, true, true, 0, 0, errors[e], false, 0, 0}, .inject_transport_error = inject_transport_error };
				NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, &disk_hooks);

				if (op == NVMEIB_BLOCK_IO_OP_DISCARD) {
					magic_pattern = __unitest_get_trimmed_u64();
					rv = osSimulator_trim(    &client->OS, volInd, startBlock, lenBlocks     );		REPORT_ERROR(rv);
				} else {
					magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);   // Set a pattern.
					rv = osSimulator_writeArr(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
				}

				if (!ut_conf__get_base()->is_valgrind) {
					mod_timer(&timer, jiffies + 1 * HZ / 1000);                 // A few milliseconds, in which io is going into resubmition thread, relaunched and failed in a loop
				} else {
					NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
				}
				clientSimulator_wait_for_all_bio_ops(client);			// No need to use: NVMeshSystem__detectStuckIOs(sys);
				clientSimulator_wait_for_all_sync_ops(client);
				memset(mem, 0, memSize);								// Clear the array
				rv = osSimulator_readArrWait( &client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
				__unitest_verify_blocks_pattern(mem, lenBlocks, magic_pattern, false);			// Verify that read and write/trim matched.

				{//cleanup
					NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
					//now we do "write", fault injection may fire during lock release operation (lock copy and not the owner), thus we need to clean them
					rv = osSimulator_writeArrWait(&client->OS, volInd, startBlock, lenBlocks, mem);		REPORT_ERROR(rv);
				}

				clientSimulator_wait_for_all_bio_ops(client);
				clientSimulator_wait_for_all_sync_ops(client);			// Wait for remaining sync operations (if any) to free itself
				BUG_ON(!NVMeshSystem_is_stable(sys));
			}
		}
	}
	n_failed_locks_after = dp_io_stats_get_counter(&client->devs[volInd]->dp.io_stats, DP_IO_STATS_LOCKSET_FAILED);
	BUG_ON(n_failed_locks_after <= n_failed_locks_before);	// We should have failed at least once
	NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	sim_kfree(mem);
	unitest_print("*************** Normal IO testing Done\n");
	return rv;
}

/* Large trim operation breaking to smaller via retrying contended locks */
TEST_FUNC int unitest_RetryLocksTrimSplit(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	struct serverSimulator *server, *lock_server;
	int i, volInd, rv = 0, startBlock = 0, lenBlocks = 0;			// [start,length] of IO in block device units
	u64 physStart, virtStart, _try;									// Start address on the physical disc and virtual block devices
	u64 size = client->devs[0]->size;								// Volume size, now too big for a single trim
	u8  magic_wipe	 = (u8)('T'), *dst = NULL;						// Special number to fill the array with
	struct disk_range* curSeg = NULL, *lock_seg = NULL;
	bool test_trim_via_write = false;								// Instead of trim operation, do write.
	u8 *tmp = sim_kmalloc(client->devs[0]->size*NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	u64 n_contended_locks_before = dp_io_stats_get_counter(&client->devs[0]->dp.io_stats, DP_IO_STATS_LOCK_CONTENDED_COUNT);
	u64 n_contended_locks_after = 0;
	memset(tmp,ramDiskSimulator_TRIMVAL, client->devs[0]->size*NVMEIBC_SECTOR_SIZE);
	for (_try=0; _try<10; _try++) { 								// Since the timing is not exact, do a few loops to detect statistical failures
	// ------------------------------------ Trim the first segment (on disk 0), with locked area. Should succeed since JBOD operations do not require locks.
	if (true) {
		volInd 	  = 1;												// Test trim split on JBOD
		startBlock= 1;												// Just to leave a space of 1 block between Volume 0 and 1
		curSeg    = &sys->mdb.vols[volInd].segs[0];                     // Trim the first segment (on disk 0)
		server	  = &sys->servers[curSeg->node_id];
		physStart = curSeg->dlba_start+startBlock;
		lenBlocks = curSeg->length-startBlock-1;					// Trim the first segment except the first and last block
		ramDiskSimulator_wipe(  &server->ramDisk, magic_wipe);		// Overwrite the disk with a magic number
		ramDiskSimulator_lockDo(&server->ramDisk, physStart);
		dst = &server->ramDisk.mem[ COMMITTED_ADDR_AS(&server->ramDisk, physStart, SECTOR, BYTE)];
		rv = osSimulator_trim(&client->OS, volInd, __from4K(startBlock), __from4K(lenBlocks));	REPORT_ERROR(rv);
		clientSimulator_wait_for_all_bio_ops(client);
		__unitest_verify_blocks_pattern(dst, __from4K(lenBlocks), __unitest_get_trimmed_u64(), true);	// Verify it was indeed trimmed
		ramDiskSimulator_lockUn(&server->ramDisk, physStart);
		magic_wipe++;
	}
	// ------------------------------------ Trim one segment of Raid1 volume at a time, with locked area. IO will enter retry state and as soon as lock is freed it will execute
	if (B->test_phase < BUNI_ERASURE_CODING_TESTING) {				// vol3 is irrelevant for erasure coding
		volInd    = 3;
		startBlock= 0;
		for (i=0; i<4; i++) {
			curSeg    = &sys->mdb.vols[volInd].segs[i];                     // Trim the first segment (on disk 0)
			lock_seg  = &sys->mdb.vols[volInd].segs[i & 2];                 // lock of first LBA is on first member
			server	  = &sys->servers[curSeg->node_id];
			lock_server	= &sys->servers[lock_seg->node_id];
			physStart = curSeg->dlba_start+startBlock;
			virtStart = startBlock + (i>=2 ? curSeg->bd_start : 0);
			lenBlocks = curSeg->length;
			ramDiskSimulator_wipe(  &server->ramDisk, magic_wipe);		// Overwrite the disk with a magic number
			ramDiskSimulator_lockDo(&lock_server->ramDisk, physStart);
			dst = &server->ramDisk.mem[COMMITTED_ADDR_AS(&server->ramDisk, physStart, SECTOR, BYTE)];
			rv = osSimulator_trim(&client->OS, volInd, __from4K(virtStart), __from4K(lenBlocks));	REPORT_ERROR(rv);
			ramDiskSimulator_lockUn(&lock_server->ramDisk, physStart);
			osSimulator_allert_pending_ios(&client->OS,-1);				// Let the IO enough time to finish
			__unitest_verify_blocks_pattern(dst, __from4K(lenBlocks) , __unitest_get_trimmed_u64(), true);	// Verify it was indeed trimmed
			magic_wipe++;
		}
	}
	// ------------------------------------ Trim the entire Raid10 volume, with locked areas according to specific patterns.
	if (true) {
		const int n_patterns = __unitest_TrimSplit_genLockPattern(NULL,0,0);
		volInd    = 0;
		startBlock= 0;
		curSeg    = &sys->mdb.vols[volInd].segs[0];
		server	  = &sys->servers[curSeg->node_id];
		physStart = curSeg->dlba_start+startBlock;
		lenBlocks = (int)min(size, block_api_os_get_max_supported_trim_blks(client->devs[volInd]->os));
		test_trim_via_write = (_try%5==1);								// Alternate between trims and writes
		for (i=0; i<n_patterns; i++) {
			__unitest_TrimSplit_genLockPattern(sys, i, true);
			if (test_trim_via_write){
				rv = osSimulator_writeArr(&client->OS, volInd, __from4K(startBlock), size, tmp);	REPORT_ERROR(rv);
			} else{
				int remaining_size = size;
				int new_start = startBlock;
				int op_len = lenBlocks;
				while (remaining_size > 0) {
					rv = osSimulator_trim(&client->OS, volInd, __from4K(new_start), op_len);	REPORT_ERROR(rv);
					remaining_size -= op_len;
					new_start += op_len;
					op_len = min(op_len, remaining_size);
				}
			}
			__unitest_TrimSplit_genLockPattern(sys, i, false);
			//NVMeshSystem__printUncompletedIOs(sys);					// Print operations that havent finished
			osSimulator_allert_pending_ios(&client->OS,-1);				// Let the IO enough time to finish
			dst = &server->ramDisk.mem[COMMITTED_ADDR_AS(&server->ramDisk, physStart, SECTOR, BYTE)];
			__unitest_verify_blocks_pattern(dst, __from4K(curSeg->length), __unitest_get_trimmed_u64(), false);	// Verify it was indeed trimmed
		}
	}

	// ------------------------------------ Same as above but test PAUSE/CONT arriving during the trim split
	if (true) {
		const int n_patterns = __unitest_TrimSplit_genLockPattern(NULL,0,0);
		const int n_used_disks = NVMESH_N_PHYS_DISKS_REGULAR_USE, disk_to_pause = _try%n_used_disks;
		volInd    = 0;
		curSeg    = &sys->mdb.vols[volInd].segs[0];
		server	  = &sys->servers[curSeg->node_id];
		physStart = curSeg->dlba_start+startBlock;
		lenBlocks = (int)min(size, block_api_os_get_max_supported_trim_blks(client->devs[volInd]->os));
		test_trim_via_write = false;
		for (i=0; i<n_patterns; i++) {
			const int mem_size = NVMEIBC_SECTOR2BYTE(__from4K(curSeg->length));
			dst = &server->ramDisk.mem[COMMITTED_ADDR_AS(&server->ramDisk, physStart, SECTOR, BYTE)];
			BUG_ON(dst[-1] 		== ramDiskSimulator_TRIMVAL);			// make sure we have a guard that is differebt than what we write (BEWARE: since its at beggining of ramDisk we CANNOT write here so we just verify that were lucky:)
			dst[mem_size] = ~ramDiskSimulator_TRIMVAL;			// make sure we have a guard that is differebt than what we write
			__unitest_TrimSplit_genLockPattern(sys, i, true);
			if (test_trim_via_write){
				rv = osSimulator_writeArr(&client->OS, volInd, __from4K(startBlock), lenBlocks, tmp);	REPORT_ERROR(rv);
			} else{
				int remaining_size = size;
				int new_start = startBlock;
				int op_len = lenBlocks;
				while (remaining_size > 0) {
					rv = osSimulator_trim(&client->OS, volInd, __from4K(new_start), op_len);	REPORT_ERROR(rv);
					remaining_size -= op_len;
					new_start += op_len;
					op_len = min(op_len, remaining_size);
				}
			}
			NVMeshSystem__invoke_pause_on_disk(sys, disk_to_pause);
			NVMeshSystem__invoke_cont_on_disk( sys, disk_to_pause, false);
			__unitest_TrimSplit_genLockPattern(sys, i, false);
			//NVMeshSystem__printUncompletedIOs(sys);					// Print operations that havent finished
			osSimulator_allert_pending_ios(&client->OS,-1);				// Let the IO enough time to finish
			__unitest_verify_blocks_pattern(dst, __from4K(curSeg->length), __unitest_get_trimmed_u64(), true);	// Verify it was indeed trimmed
			ramDiskSimulator_verify_no_locks(&sys->servers[disk_to_pause  ].ramDisk);
			ramDiskSimulator_verify_no_locks(&sys->servers[disk_to_pause^1].ramDisk);
		}
	}
	clientSimulator_wait_for_all_sync_ops(client);
	NVMeshSystem_serialize(sys);									// Due to abandoned locks client and toma can engage in unreg-rereg. Wait for this to terminate
	BUG_ON(!NVMeshSystem_is_stable(sys));
	}	// for (_try=0; loop
	n_contended_locks_after = dp_io_stats_get_counter(&client->devs[0]->dp.io_stats, DP_IO_STATS_LOCK_CONTENDED_COUNT);
	BUG_ON(n_contended_locks_after <= n_contended_locks_before);	// We should have more contended locks after the test, as we retried IO's
	sim_kfree(tmp);
	_NI_dmesg(trace_bunitest_unitest_RetryLocksTrimSplit, "*************** (@VOL_I) end", volInd);
	return rv;
}

/*
 * on vol0 (raid 10), set a lock pattern that prevents our client from reading (bcz it thinks another client has taken the lock).
 * issue some IO's & then as they content & retry, remove the locks 															.
 * when our IO's retry, they should succeed, as they see no locks are taken. 																															.
 */
TEST_FUNC int unitest_RetryLocksInDifferentLockModes(struct NVMeshSystem *sys){
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	struct serverSimulator *server;
	int i, volInd = 0, rv = 0, startBlock = 0, lenBlocks = 0;		// [start,length] of IO in block device units
	u64 physStart_abs, physStart_rel, _try;											// Start address on the physical disc and virtual block dcevices
	u8  magicNum	 = ramDiskSimulator_TRIMVAL, *dst = NULL;		// Special number to fill the array with
	struct disk_range* curSeg = NULL;
	bool test_trim_via_write = false;								// Instead of trim operation, do write.
	struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];				// Toma Configuration of the current volume
	u8 *buf_write = sim_kmalloc(client->devs[0]->size*NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	u8 *buf_read  = sim_kmalloc(client->devs[0]->size*NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
	const int n_patterns = __unitest_TrimSplit_genLockPattern(NULL,0,0);
	const int n_segs_in_r1 = tTopoOfVolume_getRaid1(cfv, 0)->header.n_segments;
	u8		pattern = 1;
	bool prev_mirror_edic;
	u64 size = client->devs[volInd]->size;
	memset(buf_write,magicNum, size*NVMEIBC_SECTOR_SIZE);
	curSeg    = &sys->mdb.vols[volInd].segs[0];
	server	  = &sys->servers[curSeg->node_id];
	physStart_abs = curSeg->dlba_start+startBlock;
	physStart_rel = COMMITTED_ADDR(&server->ramDisk, physStart_abs, 4KB);
	lenBlocks = (int)min(size, block_api_os_get_max_supported_trim_blks(client->devs[volInd]->os));
	NVMeshSystem_wipe_all_disks(sys);
	NVMeshSystem_wipe_all_md_of_disks(sys);

	prev_mirror_edic = NVMeshSystem_all_clients_mirror_edic(sys, false); // Disable edic check for this test as it will corrupt data in async mode

	for (_try=0; _try<10; _try++) { 								// Since the timing is not exact, do a few loops to detect statistical failures
		tomaSimulator_switchTopo(r1uuid(tTopoOfVolume_getRaid1(cfv, 0)), NVMEIBTC_DS_MODE_W         , NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		tomaSimulator_switchTopo(r1uuid(tTopoOfVolume_getRaid1(cfv, 1)), NVMEIBTC_DS_MODE_W_NO_DIRTY, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
		tomaSimulator_switchTopo(r1uuid(tTopoOfVolume_getRaid1(cfv, 2)), NVMEIBTC_DS_MODE_RW        , NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);

		// ------------------------------------ Trim/Write/Read the entire Raid10 volume, with locked areas according to specific patterns.
		if (true) {
			test_trim_via_write = (_try%5==1);								// Alternate between trims and writes
			for (i=0; i<n_patterns; i++) {
				__unitest_TrimSplit_genLockPattern(sys, i, true);
				NVMeshSystem_volume_memset(sys, volInd, pattern++); // write poison all volume (on both copies of data).
				NVMeshSystem_volume_wipe_MD(sys, volInd); // Make sure to clean md
				if (test_trim_via_write){
					rv = osSimulator_writeArr(&client->OS, volInd, __from4K(startBlock), size, buf_write);	REPORT_ERROR(rv);
				} else {
					int remaining_size = size;
					int new_start = startBlock;
					int op_len = lenBlocks;
					while (remaining_size > 0) {
						rv = osSimulator_trim(&client->OS, volInd, __from4K(new_start), op_len);        REPORT_ERROR(rv);
						remaining_size -= op_len;
						new_start += op_len;
						op_len = min(op_len, remaining_size);
					}
				}
					rv = osSimulator_readArr( &client->OS, volInd, __from4K(startBlock), lenBlocks, buf_read);	REPORT_ERROR(rv);
				__unitest_TrimSplit_genLockPattern(sys, i, false);
				osSimulator_allert_pending_ios(&client->OS,-1);				// Let the IO enough time to finish
				dst = &server->ramDisk.mem[__from4K(physStart_rel*NVMEIBC_SECTOR_SIZE)];
				__unitest_verify_blocks_pattern(dst, __from4K(curSeg->length), __unitest_get_trimmed_u64(), true);	// Verify it was indeed trimmed
			}
		}

		// ------------------------------------ Same as above but test PAUSE/CONT arriving during the trim split
		if (true) {
			const int n_used_disks = NVMESH_N_PHYS_DISKS_REGULAR_USE;
			int disk_to_pause = _try%n_used_disks;	// PAUSE also RW disks forcing the volume into unaccessible mode
			test_trim_via_write = false;
			for (i=0; i<n_patterns; i++) {
				__unitest_TrimSplit_genLockPattern(sys, i, true);
				NVMeshSystem_volume_memset(sys, volInd, pattern++); // write poison all volume (on both copies of data).
				NVMeshSystem_volume_wipe_MD(sys, volInd); // Make sure to clean md
				if (test_trim_via_write){
					rv = osSimulator_writeArr(&client->OS, volInd, __from4K(startBlock), size, buf_write);	REPORT_ERROR(rv);
				} else {
					int remaining_size = size;
					int new_start = startBlock;
					int op_len = lenBlocks;
					while (remaining_size > 0) {
						rv = osSimulator_trim(&client->OS, volInd, __from4K(new_start), op_len);        REPORT_ERROR(rv);
						remaining_size -= op_len;
						new_start += op_len;
						op_len = min(op_len, remaining_size);
					}
				}
					rv = osSimulator_readArr( &client->OS, volInd, __from4K(startBlock), size, buf_read);	REPORT_ERROR(rv);
				NVMeshSystem__invoke_pause_on_disk(sys, disk_to_pause);
				__unitest_TrimSplit_genLockPattern(sys, i, false);
				NVMeshSystem__invoke_cont_on_disk( sys, disk_to_pause, false);
				for (;osSimulator_allert_pending_ios(&client->OS, 0); msleep(1)) { // BUG here!!!! Problem in unitest with owner disk of dual locked raid1 {2,3}. When Write/Trim generates stale lock, Read can stuck forever, becasue it cannot invoke sync operations (having 1 lock instead of 2)
					const int node_id = sys->mdb.vols[volInd].segs[n_segs_in_r1 + 1].node_id;	// Second disk of Second raid1
					ramDiskSimulator_CleanSta(&sys->servers[node_id].ramDisk);
				}
				NVMeshSystem__detectStuckIOs(sys);
				dst = &server->ramDisk.mem[__from4K(physStart_rel*NVMEIBC_SECTOR_SIZE)];
				__unitest_verify_blocks_pattern(dst, __from4K(curSeg->length), __unitest_get_trimmed_u64(), true);	// Verify it was indeed trimmed
				ramDiskSimulator_CleanSta(&sys->servers[disk_to_pause  ].ramDisk); // It is perfectly legal for IO to succeed leaving behind stale locks. Happens when all the commands were completed, but locks failed to be released and were abandoned.
				ramDiskSimulator_CleanSta(&sys->servers[disk_to_pause^1].ramDisk);
			}
		}

		tomaSimulator_switchTopo(tTopoOfVolume_getRaid1(cfv, 0)->header.uuid, NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);	// {RW	, RW} - normal
		tomaSimulator_switchTopo(tTopoOfVolume_getRaid1(cfv, 1)->header.uuid, NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);	// {RW	, RW} - normal
		tomaSimulator_switchTopo(tTopoOfVolume_getRaid1(cfv, 2)->header.uuid, NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);	// {RW	, RW} - normal
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}	// for (_try=0; loop
	NVMeshSystem_all_clients_mirror_edic(sys, prev_mirror_edic); // Return edic check
	NVMeshSystem_wipe_all_md_of_disks(sys);

	NVMeshSystem_wipe_all_dirty_bits(sys);
	sim_kfree(buf_write);
	sim_kfree(buf_read);
	_NI_dmesg(trace_bunitest_unitest_RetryLocksInDifferentLockModes, "*************** (@VOL_I) end", volInd);
	return rv;
}

static short tested_error_codes[NUMBER_OF_ERROR_CODES] = { EPERM_READ_FAIL, EPERM_READ_FAIL_NO_RETRY, NVME_SC_DNR };

// Offset to start from to force the IO take owner lock on 'si' segment in raid. For 2-mirror: even segments have owner lock on even locksets, odd segments have owner lock on odd locksets. So add offset to always apply the stale owner lock
static int __offset_to_put_owner_lock_on_seg(int si, int n_mirror, int max_n_owners){
	const int lock_seg = ((si%n_mirror)%max_n_owners);	// Segment of owner lock in raid1
	return lock_seg * (1<<LOCKSET_SHIFT);
}

void __verify_raid_no_locks_no_dbits(struct NVMeshSystem *sys, struct disk_range* segs){
	for (struct disk_range* sgmnt = segs; sgmnt != (segs + segs->replicas); ++sgmnt) {
		struct ramDiskSimulator* ssd = &(sys->servers[sgmnt->node_id].ramDisk);
		ramDiskSimulator_verify_no_locks(ssd);
		ramDiskSimulator_verify_no_dirty_bits(ssd);
	}
}

void tomaSimulator_recoverOK_Blocking(const struct tTopoOfPraid *r1, const struct disk_range *seg, struct toma_recovery_args args) {
	struct sim_recovery_hooks hooks = sim_recovery_hooks_create(0, 0, 1 + args.has_dconv, 0); //don't sleep, wait for rcvr cleanup
	int recov_status;
	sim_recovery_setup_hooks(&hooks);
	BUG_ON(tomaSimulator_recoverThingStatus(r1, seg, args, &recov_status) < 0);	//start recovery - wait for launching
	nvmeibc_multi_completion_wait_for(&hooks.cleaned);
	BUG_ON(recov_status < 0);
	sim_recovery_clean_hooks();
}

struct stale_dirty_unknown {
	union {
		struct {
			u8 is_stale : 1;
			u8 is_dirty : 1;
			u8 is_unknown : 1;
			u8 unused : 5;
		};
		u8 all;
	};
};

static void __set_bi_inj_from_stale_dirty_unknown(union nvmeib_blkset_problem_report *bi_inj, struct stale_dirty_unknown sdu, int dirty_value)
{
	BUG_ON(sdu.all == 0); // No injection
	if (sdu.is_dirty) {
		BUG_ON(sdu.is_unknown); // Mutual exclusive
		bi_inj->dbits = dirty_value;
	}
	if (sdu.is_unknown) {
		bi_inj->dbits = nvmeib_dbits_entry_single_unk().all_bits;	// Used in 1-degraded
	}
	if (sdu.is_stale) {
		bi_inj->is_stale = true;
	}
}

static void __set_ram_binfo_from_stale_dirty_unknown(struct ramDiskSimulator *ssd, const u64 addr, struct stale_dirty_unknown sdu, int dirty_value)
{
	if (sdu.is_dirty) {
		BUG_ON(sdu.is_unknown); // Mutual exclusive
		ramDiskSimulator_setDirty(ssd, addr, dirty_value);
	}
	if (sdu.is_unknown) {
		ramDiskSimulator_setDirty(ssd, addr, nvmeib_dbits_entry_single_unk().all_bits);	// Used in 1-degraded
	}
	if (sdu.is_stale) {
		ramDiskSimulator_lockStale(ssd, addr);
	}
}

/*run test that simulate recovery*/
TEST_FUNC int unitest_R1_recovery_Basic(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	//struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	const enum NVMEIBTC_DS_MODE sgmnts_mode[2] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W};
	int volIndx;
	u32 rlba, sgmnt_idx;

	clientSimulator_wait_for_all_recoveries_done(sys->clients); //precondition

	for (volIndx = 0; volIndx < sys->mdb.nVols; ++volIndx) {		// Only vol 0 is relevant for recovery
		struct clientSimulator *clnt = sys->clients;
		struct nvmeibc_block_device *dev = clnt->devs[volIndx];
		struct disk_range *sgmnts = &sys->mdb.vols[volIndx].segs[0];
		struct tTopoOfVolume *cfv = &sys->tcf.vols[volIndx];
		struct tTopoOfPraid  *r1 = tTopoOfVolume_getRaid1(cfv, 0);
		if (!tTopoOfVolume_isMirrored(cfv))
			continue;													// recovery of non mirorred volumes is irrelevant

		for (sgmnt_idx = 0; sgmnt_idx < sgmnts->replicas; sgmnt_idx++) {				// ------------------------------------- Stale locks recovery: msg sent by each of 2 tomas
			struct nvmeibc_topology *t   = ___get_tail_topo_of_device(sys, volIndx);
			struct nvmeibc_raid1 *c_raid = &t->chunks[0].raid1s[0];
			for (rlba = sgmnt_idx*LOCKSET_4KS; rlba < sgmnts->length; rlba += LOCKSET_4KS) {							// Mark subset of raid with stale locks.
				const int si = get_owner_seg_of_lock(c_raid, __from4K(rlba));
				ramDiskSimulator_lockStale(&sys->servers[sgmnts[si].node_id].ramDisk, sgmnts[si].dlba_start + rlba);
			}
			for (rlba = 0; rlba < sgmnts->replicas; rlba++) {								// Verify stale lock was indeed cleared
				BUG_ON(tomaSimulator_recoverThing(r1, &sgmnts[rlba], RCVR_STALE_REBUILD) < 0);
				ramDiskSimulator_verify_no_locks(&sys->servers[sgmnts[rlba].node_id].ramDisk);
			}

			BUG_ON(tomaSimulator_recoverThing(r1, &sgmnts[sgmnt_idx], RCVR_STALE_REBUILD_PING) < 0);		// Test, ping and abort messages
			BUG_ON(tomaSimulator_recoverThing(r1, &sgmnts[sgmnt_idx], RCVR_STALE_REBUILD_ABORT) < 0);
		}
		__verify_raid_no_locks_no_dbits(sys, sgmnts);
		for (sgmnt_idx = 0; sgmnt_idx < sgmnts->replicas; sgmnt_idx++) { //checks PING & ABORT messages, while recovery is running
			struct toma_recovery_args async_rebuild = RCVR_DIRTY_REBUILD;
			struct disk_range *sgmnt = &sgmnts[sgmnt_idx];
			struct ramDiskSimulator *ssd = &sys->servers[sgmnt->node_id].ramDisk;
			struct sim_recovery_hooks hooks = sim_recovery_hooks_create(0, 1, 1, 0); //don't sleep, wait for rcvr launched and cleanup
			union nvmeib_blkset_problem_report bi_inj[RAMDISK_DATA_LOCK_SIZE];

			async_rebuild.on_start_wait_for_end = false;
			async_rebuild.recov_caller = UNI_RECOV_CALLER_TOMA;
			ramDiskSimulator_set_lock(ssd, sgmnt->dlba_start, SIMULATOR_OTHER_CLIENT_LOCK_ID);
			array_fill(bi_inj, ((union nvmeib_blkset_problem_report){.dbits=0xFF}));
			ssd->c.bi_inj = bi_inj;
			sim_recovery_setup_hooks(&hooks);

			//start recovery - wait for launching
			BUG_ON(tomaSimulator_recoverThing(r1, sgmnt, async_rebuild) < 0);
			nvmeibc_multi_completion_wait_for(&hooks.launched);

			//send ping - some progress should be reported back to toma; would be nice to verify it to
			BUG_ON(tomaSimulator_recoverThing(r1, sgmnt, RCVR_DIRTY_REBUILD_PING) < 0);

			//now send abort message & wait
			BUG_ON(tomaSimulator_recoverThing(r1, sgmnt, RCVR_DIRTY_REBUILD_ABORT) < 0);
			nvmeibc_multi_completion_wait_for(&hooks.cleaned);

			//now lets cleanup after ourself with one more process
			ssd->c.bi_inj = NULL;
			ramDiskSimulator_set_unlock(ssd, sgmnt->dlba_start);
			sim_recovery_clean_hooks();
			clientSimulator_wait_for_all_recoveries_done(clnt);
			clientSimulator_wait_for_all_sync_ops(clnt);

			busy_wait_forever_more(microseconds(20), atomic_read(&sys->servers[sgmnt->node_id].simToma.num_running_recoveries) == 0);
			_NT(trace_bunitest_unitest_R1_recovery_Basic, "waiting for toma=@TOMA_UNIQUEID recoveries done", sys->servers[sgmnt->node_id].simToma.uniqueID);
		}
		__verify_raid_no_locks_no_dbits(sys, sgmnts);
		for (sgmnt_idx = 0; sgmnt_idx < sgmnts->replicas; sgmnt_idx++) {				// ------------------------------------- Dbits recovery
			u64 *cur_num_syncs = &dev->dp.sync_rsrcs.stats.num_dirty_bit_suspect;
			u64 before = *cur_num_syncs, count, after;
			tomaSimulator_switchTopo(r1uuid(r1), sgmnts_mode[sgmnt_idx], sgmnts_mode[sgmnt_idx^1], SW_TOPO__WAIT_ACK);				// {RW,W} or {W,RW}
			__verify_raid_no_locks_no_dbits(sys, sgmnts);
			tomaSimulator_recoverOK_Blocking(r1, &sgmnts[sgmnt_idx], RCVR_DIRTY_REBUILD);		// No dirtybits, call recovery which will do nothing
			BUG_ON(tomaSimulator_recoverThing(r1, &sgmnts[sgmnt_idx], RCVR_INVALID) < 0);											// Illegal type of recovery
			__verify_raid_no_locks_no_dbits(sys, sgmnts);
			{	// Verify cant launch scrubbing in degraded mode
				struct toma_recovery_args scrub_args = { .type = NVMEIBT_RECOVERY_TYPE_SCRUBBING, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA };
				BUG_ON(tomaSimulator_recoverThing(r1, &sgmnts[sgmnt_idx], scrub_args) < 0);
			}
			if (1) {			// Test recovery ioctl
				char cmd[256];
				struct disk_range *sgmnt = &sgmnts[sgmnt_idx];
				struct ramDiskSimulator *ssd = &sys->servers[sgmnt->node_id].ramDisk;
				int n_exec_ioctls = clientSimulator_get_num_executed_ioctls(clnt);
				struct sim_recovery_hooks hooks = sim_recovery_hooks_create(0, 1, 1, 0); //don't sleep, wait for rcvr launched and cleanup

				sim_recovery_setup_hooks(&hooks);
				ramDiskSimulator_set_lock(ssd, sgmnt->dlba_start, SIMULATOR_OTHER_CLIENT_LOCK_ID);	// Acquire lock to prevent recovery from finishing.

				sprintf(cmd, "#%s|recov_launch sgmnt=(0,0,%u) type=%u is_mandatory=1 do_only_owners=1 blocksets=[0, -1) jgc_cookie=0", dev->name, (uint8_t)sgmnt_idx, NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD);
				clientSimulator_send_to_cli(clnt, cmd);
				nvmeibc_multi_completion_wait_for(&hooks.launched);

				sprintf(cmd, "#%s|recov_handle  sgmnt=(0,0,%u) type=%u task=-1 msg=%d", dev->name, (uint8_t)sgmnt_idx, -1 /* ALL running recoveries */    , NVMEIBT_CLIENT_MSG_TR_RECOVER_PING);
				clientSimulator_send_to_cli(clnt, cmd);

				sprintf(cmd, "#%s|recov_handle  sgmnt=(0,0,%u) type=%u task=-1 msg=%d", dev->name, (uint8_t)sgmnt_idx, NVMEIBT_RECOVERY_TYPE_DIRTY_REBUILD, NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT);
				clientSimulator_send_to_cli(clnt, cmd);
				//now lets cleanup after ourself with one more process
				ramDiskSimulator_set_unlock(ssd, sgmnt->dlba_start);
				nvmeibc_multi_completion_wait_for(&hooks.cleaned);
				//wait for recovery end; would be nice to verify we actually reported "abort" to toma
				BUG_ON(n_exec_ioctls+3 != clientSimulator_get_num_executed_ioctls(clnt));
				sim_recovery_clean_hooks();
				__verify_raid_no_locks_no_dbits(sys, sgmnts);
			}
			if (1) { // Test all problem injections with different real lock scenarios
				struct disk_range *sgmnt = &sgmnts[sgmnt_idx];
				struct ramDiskSimulator *ssd = &sys->servers[sgmnt->node_id].ramDisk;
				struct stale_dirty_unknown problem_injection;
				struct stale_dirty_unknown lock_injection;
				const u64 first_blockset_index_abs = (sgmnt->dlba_start / LOCKSET_SLICES);
				const u64 first_blockset_index_rel = COMMITTED_ADDR_AS(ssd, sgmnt->dlba_start, 4KB, LOCK);
				int solve_with_io;
				u8 *mem = sim_kzalloc(NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
				union nvmeib_blkset_problem_report bi_inj[RAMDISK_DATA_LOCK_SIZE];
				for (solve_with_io = 0; solve_with_io < 3; solve_with_io++) { // Solve with recovery or read/write IO
					for (problem_injection.all = 2; problem_injection.all < 6; problem_injection.all++) { // Dirty (2), Stale + Dirty (3), Unknown (4), Unknown + Stale (5). No problem (0) and Stale (1) (will only trigger copy stale) tested below.
						for (lock_injection.all = 0; lock_injection.all < 6; lock_injection.all++) { // None (0), Stale (1), Dirty (2), Stale + Dirty (3), Unknown (4), Unknown + Stale (5)
							memset(bi_inj, 0, sizeof(bi_inj));
							*cur_num_syncs = 0;
							if (!solve_with_io) { // Only for tiggered syncs
								__set_bi_inj_from_stale_dirty_unknown(&bi_inj[first_blockset_index_rel], problem_injection, (1 ^ sgmnt_idx) + 1);
							}
							__set_ram_binfo_from_stale_dirty_unknown(ssd, first_blockset_index_abs*LOCKSET_SLICES, lock_injection, (1^sgmnt_idx)+1);
							ssd->c.bi_inj = bi_inj;
							if (solve_with_io) { // Cause IO to encounter injection in lock
								if (solve_with_io == 1) { // Read can only resolve stale locks
									int rv = osSimulator_readArrWait( &clnt->OS, volIndx, 0, 1, mem);	REPORT_ERROR(rv);
								} else { 				  // Small Write can only resolve stale locks
									int rv = osSimulator_writeArrWait(&clnt->OS, volIndx, 0, 1, mem);	REPORT_ERROR(rv);
								}
								if (lock_injection.is_dirty || lock_injection.is_unknown) { // Read/Write will not solve any DBits
									ramDiskSimulator_setDirty(ssd, sgmnt->dlba_start, 0);
								}
							} else { // Trigger recovery to get problem injected, but when taking the lock will encounter (the same or different lock value)
								tomaSimulator_recoverOK_Blocking(r1, &sgmnts[sgmnt_idx], RCVR_DIRTY_REBUILD);
							}
							clientSimulator_wait_for_all_sync_ops(clnt);
							after = *cur_num_syncs;
							BUG_ON(!solve_with_io && (after != lock_injection.is_unknown)); // Single unknown sync (IO doesn't trigger it) only if the lock was injected with unknown
							__verify_raid_no_locks_no_dbits(sys, sgmnts);
							// Clear injection for next round
							ssd->c.bi_inj = NULL;
						}
					}
				}
				sim_kfree(mem);
				__verify_raid_no_locks_no_dbits(sys, sgmnts);
			}
			if (volIndx == 0) {				// --------------------- R1: {RW,W} read-op causes partial sync which copies dirtybit to 'W' seg.
				struct disk_range *rws = &sgmnts[sgmnt_idx];
				struct ramDiskSimulator* ssd = &sys->servers[rws->node_id  ].ramDisk;
				struct ramDiskSimulator* oth = &sys->servers[rws->node_id^1].ramDisk;		// The disk to which stale locks should be copied
				const int inject[2] = {0,2}, inj = inject[sgmnt_idx];						// Dirty bit+Stale   on blocksets {0,2}. 0 rbla for {RW,W} and 2 for {W,RW}
				u8	*read_blk = sim_kmalloc(NVMEIBC_SECTOR_SIZE, GFP_KERNEL);
				const u16 all_dbits = (sgmnt_idx^1)+1; 										// Dbit for 'W' segment
				int read_rv, vlba;
				rlba = LOCKSET_4KS*inj;
				vlba = sys->tcf.vols[volIndx].chunks->stripeWidth * rlba;					// VLBA of read IO to fall on rlba blockset
				ramDiskSimulator_setDirty( ssd, rws->dlba_start + rlba, all_dbits);
				ramDiskSimulator_lockStale(ssd, rws->dlba_start + rlba);
				oth->TxIDs[inj] = ssd->TxIDs[inj] = 0;
				BUG_ON(nvmeibc_sync_full_lockset_probability_factor != NVMEIBC_SYNC_PROB_FORCE_LOCKSET);
				nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_MIN;	// Force read to launch partial sync
				read_rv = osSimulator_readArr(&clnt->OS, volIndx, vlba, 1, read_blk);	REPORT_ERROR(read_rv);
				clientSimulator_wait_for_all_bio_ops(clnt);
				BUG_ON(ssd->dbits[inj].all_bits != all_dbits);								// Injected dbit remain on owner seg, they could not be solved by partial sync
				BUG_ON(ssd->TxIDs[inj] != oth->TxIDs[inj]);
				if (1) { // BUG: NVMESH-3032 was fixed yet
					BUG_ON((ssd->TxIDs[inj] != 0) || (oth->dbits[inj].all_bits != 0));		// Partial sync could not write binfo (txid, dbits)
				} else {
					BUG_ON(ssd->TxIDs[inj] != NVMEIB_BLOCK_IO_OP_RECOVER_STALE);			// Verify via debug TxID that this sync indeed ran
					BUG_ON(oth->dbits[inj].all_bits != all_dbits);							// Wrong dirty bits on 'W' seg for themselves, created by partial sync
				}
				nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET;		// force full LOCKSET sync
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD);
				clientSimulator_wait_for_all_sync_ops(clnt);
				sim_kfree(read_blk);
				__verify_raid_no_locks_no_dbits(sys, sgmnts); // Verify that all stale locks/dbits were removed
			}
			if (volIndx == 0) {				// --------------------- R1: {RW,W} corrupted copy owner has wrong dbit. Sync of commit stale encounters it
				struct disk_range *rws = &sgmnts[sgmnt_idx];
				struct ramDiskSimulator* ssd = &sys->servers[rws->node_id  ].ramDisk;
				struct ramDiskSimulator* oth = &sys->servers[rws->node_id^1].ramDisk;		// The disk to which stale locks should be copied
				const int inject[2] = {0,2}, inj = inject[sgmnt_idx];						// Dirty bit+Stale   on blocksets {0,2}. 0 rbla for {RW,W} and 2 for {W,RW}
				const u16 all_dbits = (sgmnt_idx^1)+1; 										// Dbit for 'W' segment
				rlba = LOCKSET_4KS*inj;
				ramDiskSimulator_lockStale(ssd, rws->dlba_start + rlba);
				ramDiskSimulator_setDirty( oth, rws->dlba_start + rlba, all_dbits);	// Copy owner has wrong dbit!
				dev->dp.io_stats.mgr.n_binfo_copy_owner_error = 0;
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD);
				clientSimulator_wait_for_all_sync_ops(clnt);
				BUG_ON((ssd->dbits[inj].all_bits != 0) || (oth->dbits[inj].all_bits != 0));	// Wrong dbit on copy owner was cleaned
				BUG_ON(ssd->TxIDs[inj] != oth->TxIDs[inj]);
				BUG_ON(oth->TxIDs[inj] != NVMEIB_BLOCK_IO_OP_REC_R1_COMMIT_STALE);			// Verify via debug TxID that this sync indeed ran
				BUG_ON(dev->dp.io_stats.mgr.n_binfo_copy_owner_error != 1);
				ramDiskSimulator_lockUnSta(ssd, rws->dlba_start + rlba);					// Verify that stale lock exists on both segments
				ramDiskSimulator_lockUnSta(oth, rws->dlba_start + rlba);
				__verify_raid_no_locks_no_dbits(sys, sgmnts); // Verify that all stale locks/dbits were removed
			}

			if (volIndx == 0) {             // --------------------- Verify dirty bits recovery also copies stale locks without solving them
				struct disk_range *rws = &sgmnts[sgmnt_idx];
				struct ramDiskSimulator* ssd = &sys->servers[rws->node_id  ].ramDisk;
				struct ramDiskSimulator* oth = &sys->servers[rws->node_id^1].ramDisk;		// The disk to which stale locks should be copied
				const int inject_dbits[2][2] = {{0,2}, {1,3}};								// Dirty bit   on blocksets {0,2}. Thus we have all options (D+S, D, S, None)
				const int inject_stale[2][2] = {{2,3}, {1,0}};								// Stale locks on blocksets {2,3}
				before = *cur_num_syncs;
				count = 0;

				nvmeibc_debug_ram_unknown_dbits = false;	// Deliberately using double unknowns in 1-degraded
				for (rlba = 0; rlba < 2; rlba++) {
					ramDiskSimulator_setDirty( ssd, rws->dlba_start + LOCKSET_4KS*inject_dbits[sgmnt_idx][rlba], nvmeib_dbits_entry_build_unk(-1,-1).all_bits);
					ramDiskSimulator_lockStale(ssd, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][rlba]);
					//_ND(t_simuav20, "sss @INT, @INT", inject_dbits[sgmnt_idx][rlba], inject_stale[sgmnt_idx][rlba]);
					count++;
				}
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD);
				clientSimulator_wait_for_all_sync_ops(clnt);
				// Verify that stale lock exists on both segments (where there was no dbits)
				ramDiskSimulator_lockUnSta(ssd, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][1]);
				ramDiskSimulator_lockUnSta(oth, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][1]);
				after = *cur_num_syncs;
				BUG_ON(after != before + count);
				nvmeibc_debug_ram_unknown_dbits = true;		// Double unknowns in 1-degraded

				for (rlba = 0; rlba < 2; rlba++) { // Repeat with single unknown
					ramDiskSimulator_setDirty( ssd, rws->dlba_start + LOCKSET_4KS*inject_dbits[sgmnt_idx][rlba], nvmeib_dbits_entry_single_unk().all_bits);
					ramDiskSimulator_lockStale(ssd, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][rlba]);
					//_ND(t_simuav21, "sss @INT, @INT", inject_dbits[sgmnt_idx][rlba], inject_stale[sgmnt_idx][rlba]);
					count++;
				}
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD);
				clientSimulator_wait_for_all_sync_ops(clnt);
				// Verify that stale lock exists on both segments (where there was no dbits)
				ramDiskSimulator_lockUnSta(ssd, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][1]);
				ramDiskSimulator_lockUnSta(oth, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][1]);
				after = *cur_num_syncs;
				BUG_ON(after != before + count);
				if (1) {// --------------------- Verify dirty bits recovery also does not copy stale locks which were reported by server but solved by other client
					union nvmeib_blkset_problem_report bi_inj[RAMDISK_DATA_LOCK_SIZE];
					memset(bi_inj, 0, sizeof(bi_inj));
					ssd->c.bi_inj = bi_inj;
					for (rlba = 0; rlba < 2; rlba++) { // Create All 4 subsets of combinations (reported stale vs actual stale lock in RAM)
						bi_inj[inject_dbits[sgmnt_idx][rlba]].is_stale = true;
						ramDiskSimulator_lockStale(ssd, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][rlba]);
					}
					tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD);
					for (rlba = 0; rlba < 2; rlba++)		// Verify stales remained unchanged on primary owner (regardless of what server has reported), and no new stales were created
						ramDiskSimulator_lockUnSta(ssd, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][rlba]);
					ramDiskSimulator_verify_no_locks(ssd);

					ramDiskSimulator_lockUnSta(oth, rws->dlba_start + LOCKSET_4KS*inject_stale[sgmnt_idx][0]);// Verify stales were copied only for blockset where actual stale existed AND stale was reported
					ramDiskSimulator_verify_no_locks(oth);
					ssd->c.bi_inj = NULL;
				}
				__verify_raid_no_locks_no_dbits(sys, sgmnts);
			}
			if (volIndx == 0) {				// --------------------- R1: Dirty bits recovery also turns on convicts and handles them
				const enum NVMEIBTC_DS_MODE conv[2] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W_IS_DIRTY};
				struct disk_range *rws = &sgmnts[sgmnt_idx];
				struct ramDiskSimulator* ssd = &sys->servers[rws->node_id  ].ramDisk;
				//struct ramDiskSimulator* oth = &sys->servers[rws->node_id^1].ramDisk;		// The disk to which stale locks should be copied
				const int inject_dbits[2][2] = {{0,2}, {1,3}};								// Dirty bit   on blocksets {0,2}
				// DB suspect will still be assumed even though we had a convict and did not consider it known
				before = *cur_num_syncs;
				tomaSimulator_switchTopo(r1uuid(r1), conv[sgmnt_idx], conv[sgmnt_idx^1], SW_TOPO__WAIT_ACK);				// {RW,W-} or {W-,RW}
				// When injecting invalid Dbit + convict we get more than allowed degraded segments
				for (rlba = 0; rlba < 2; rlba++) {
					ramDiskSimulator_setDirty( ssd, rws->dlba_start + LOCKSET_4KS*inject_dbits[sgmnt_idx][rlba], 0x6);		// Strange dirty + convict will be turned on and then turned off
				}
				warn_on_too_many_degraded = false;					// Deliberatly inject more dbits than degraded segs, see remark above
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD_CONV);
				warn_on_too_many_degraded = true;
				ramDiskSimulator_verify_no_locks(ssd);

				for (rlba = 0; rlba < 2; rlba++) {
					ramDiskSimulator_setDirty( ssd, rws->dlba_start + LOCKSET_4KS*inject_dbits[sgmnt_idx][rlba], nvmeib_dbits_entry_single_unk().all_bits);		// Unknown + convict will be turned on and then turned off
				}
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD_CONV);
				ramDiskSimulator_verify_no_locks(ssd);

				nvmeibc_debug_ram_unknown_dbits = false;	// Same as above but deliberate double unknowns in 1-degraded
				for (rlba = 0; rlba < 2; rlba++) {
					ramDiskSimulator_setDirty( ssd, rws->dlba_start + LOCKSET_4KS*inject_dbits[sgmnt_idx][rlba], nvmeib_dbits_entry_build_unk(-1,-1).all_bits);		// 2 Unknown + convict will be turned on and then turned off
				}
				tomaSimulator_recoverOK_Blocking(r1, rws, RCVR_DIRTY_REBUILD_CONV);
				nvmeibc_debug_ram_unknown_dbits = true;

				ramDiskSimulator_verify_no_locks(ssd);
				clientSimulator_wait_for_all_sync_ops(clnt);
				// DB Unknowns with convicts are not considered suspect (For 2 mirror)
				after = *cur_num_syncs;
				BUG_ON(after != before);
			}

			tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);			// {RW	, RW} - normal
			NVMeshSystem_serialize(sys);
			__verify_raid_no_locks_no_dbits(sys, sgmnts);
		}
	}
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return 0;
}

struct t_multi_sync_context {
	struct sim_recovery_hooks rh;
	struct nvmeibc_subscription_ctx *tr;			// Seg on which recovery is launched
	char cmd[150];					// ioctl cmd to launch recovery
	union nvmeib_blkset_problem_report bi_inj[256];	// Array of problems
	u32 n_blksets_in_seg;			// Length of the segment in blocksets
	u32 n_blksets_given;			// Amount of elements that were given in all previous batches <= n_blksets_in_seg
	u32 n_elem_in_batch;			// Amount of elements that will be given upon batch request
	u32 n_sw_to_use   : 16;			// Amount of concurent syncs to run
	u32 n_batches_req : 16;			// Amount of requested batches
	enum NVMEIBT_RECOVERY_TYPE rtype;// == NVMEIBT_RECOVERY_TYPE_VOID_DUMMY
};

static void __multi_sync_next_batch(struct nvmeibc_recovery_hooks* base, struct nvmeibc_recovery* recov, const u8**arr, u64 *n_elem) {
	struct t_multi_sync_context* ctx = (void*)container_of(base, struct sim_recovery_hooks, base);
	u32 i, r = get_random_u32();
	ctx->n_elem_in_batch = ((r % ARRAY_SIZE(ctx->bi_inj))+1);						// No more than the array size
	MIN_WITH(ctx->n_elem_in_batch, recov->args.r_end/5);							// Batch is no more than 20% of the total
	MIN_WITH(ctx->n_elem_in_batch, (ctx->n_blksets_in_seg-ctx->n_blksets_given));// No more than total
	if ((ctx->n_batches_req%5)==3)
		MIN_WITH(ctx->n_elem_in_batch, (ctx->n_elem_in_batch%ctx->n_sw_to_use)+1);// Once every 5 batches, make a very small batch (less than amount of workers)
	BUG_ON(ctx->n_elem_in_batch == 0);
	for (i = 0; i < ctx->n_elem_in_batch; i++)
		ctx->bi_inj[i] = ((union nvmeib_blkset_problem_report){ .dbits=0x1});		// Every blockset has a problem
	*arr = (u8*)ctx->bi_inj;
	*n_elem = ctx->n_elem_in_batch;
	ctx->n_blksets_given += ctx->n_elem_in_batch;
	ctx->n_batches_req++;
}

TEST_FUNC int unitest_multi_sync_recov(bunitest_s* B) {
	const struct volume_segment_index vsi = {3,0,0,0};
	struct test_context env = {.sys = B->sys, .client = B->sys->clients, .dev = B->sys->clients->devs[vsi.volume], .sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, vsi)};
	struct t_multi_sync_context sc = {.n_sw_to_use = 17, .rtype = NVMEIBT_RECOVERY_TYPE_VOID_DUMMY, .n_blksets_given = 0, .n_batches_req = 0};

	srand(jiffies);
	bunitest_tic(B);
	sc.tr = ___get_tail_topo_of_device(env.sys, vsi.volume)->chunks[vsi.chunk].raid1s[vsi.raid].segments[vsi.segment].toma_reg;
	sc.rh = sim_recovery_hooks_create(0, 0, 1, 0);
	sprintf(sc.cmd , "#%s|recov_set_num_sw=%d:0,0" , env.dev->name, sc.n_sw_to_use); clientSimulator_send_to_cli(env.client, sc.cmd);
	BUG_ON((sc.tr->hdr->recoveries[sc.rtype]->n_sw_to_use != sc.n_sw_to_use)||(!env.dev->ignore_all_recov_toma_speed_req));
	sc.rh.base.on_dummy_get_next_batch = __multi_sync_next_batch;
	sim_recovery_setup_hooks(&sc.rh);
	sc.n_blksets_in_seg = 80000;							// ~10[GB] volume
	sc.tr->length += sc.n_blksets_in_seg*LOCKSET_SLICES;
	snprintf(sc.cmd, sizeof(sc.cmd), "#%s|recov_launch sgmnt=(0,0,%u) type=%u is_mandatory=1 do_only_owners=1 blocksets=[0, %u) " , env.dev->name, vsi.segment, sc.rtype, sc.n_blksets_in_seg);
	clientSimulator_send_to_cli(env.client, sc.cmd);	// struct toma_recovery_args r_args = {.type = sc.rtype, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true}; BUG_ON(tomaSimulator_recoverThing(env.sraid.tpr, env.sraid.cpr, r_args) < 0);
	nvmeibc_multi_completion_wait_for(&sc.rh.cleaned);
	sc.tr->length -= sc.n_blksets_in_seg*LOCKSET_SLICES;
	clientSimulator_wait_for_all_recoveries_done(env.client);
	sim_recovery_clean_hooks();
	sprintf(sc.cmd , "#%s|recov_set_num_sw=0:0,0" , env.dev->name); clientSimulator_send_to_cli(env.client, sc.cmd);
	BUG_ON(env.dev->ignore_all_recov_toma_speed_req);

	if (1) {	// Test batch size ioctl
		int expected_num_ioctls = clientSimulator_get_num_executed_ioctls(env.client);
		sprintf(sc.cmd , "#%s|recov_set_nbatch=%d:0,0" , env.dev->name, 5777); clientSimulator_send_to_cli(env.client, sc.cmd); expected_num_ioctls++;
		BUG_ON(!env.dev->ignore_all_recov_toma_speed_req);
		sprintf(sc.cmd , "#%s|recov_set_nbatch=%d:0,0" , env.dev->name, 0);    clientSimulator_send_to_cli(env.client, sc.cmd); expected_num_ioctls++;
		BUG_ON(env.dev->ignore_all_recov_toma_speed_req);
		BUG_ON(expected_num_ioctls != clientSimulator_get_num_executed_ioctls(env.client));
	}
	BUG_ON(!NVMeshSystem_is_stable(env.sys));
	unitest_print("*** R1-multi_sync_recov: n_blksets=%u, n_batches=%u, %d[mSec]\n", sc.n_blksets_in_seg, sc.n_batches_req, bunitest_toc(B));
	return 0;
}

struct t_scrub_tester_r1 {
	struct t_slice_data_r1 {
		u8 block[NVMEIBC_SECTOR_SIZE];
		union nvmeibc_block_dp_ec_data_block_md md;
	} correct, corrupt;
	struct block_inject_ptrs bptrs[N_MAX_RAID_SLICE_LEN];
	enum corruption_type {	CORRUPT_BAD_SECT_PHYS = 'P',			// Physical bad sector
							CORRUPT_BAD_SECT_LOGICAL = 'L',			// Logical bad sector
							CORRUPT_BAD_SECT_NEVER_WRITTEN = 'b',	// Logical bad sector in never written slice
							CORRUPT_WRONG_EDIC = 'E',				// Change edic of D or P so it will be wrong. Causes edic fail and treated as bad sector
							CORRUPT_NON_IDENTICAL = 'N',	// One of the legs of R1 does no match the other.
							ALREADY_FIXED = 'A' }			// No corruption in slice, it was already fixed
							type;
	struct NVMeshSystem *sys;
	const struct disk_range *pr;					// praid in which the test runs
	u32 ind;										// Index of corrupted block in a slice
	u32 slba;										// slba where of the corrupted slice
	u32 lockset_owner_seg;							// Which disk is primary owner of corrutped slice
	int n_total_syncs, n_expected_scrub_nop, n_expected_scrubs, n_bad_sectors; // Expected stats
};
bool corruption_type_is_bad_sector(enum corruption_type e) {
	return ((e != CORRUPT_NON_IDENTICAL) && (e != ALREADY_FIXED));
}

static void t_scrub_tester_set_inject_ptrs(struct t_scrub_tester_r1 *st, struct NVMeshSystem *sys, const struct disk_range *pr, u32 slba) {
	int i, n_segs = pr->replicas;
	st->sys = sys;
	st->pr = pr;
	st->slba = slba;
	st->lockset_owner_seg = (st->slba >> LOCKSET_SHIFT) % n_segs;
	for (i = 0; i < n_segs; i++) {		// Set inject pointers
		const raid_sgmnt_t di = (i + st->lockset_owner_seg) % n_segs;		// Convert role to seg
		const u64 dlba = st->slba + pr[di].dlba_start;
		st->bptrs[i] = serverSimulator_get_block_inject_ptrs(&sys->servers[di], dlba, -1, -1, 0);	// -1 Means not using journals
	}
}

static void t_scrub_tester_reinit_random(struct t_scrub_tester_r1 *st, struct NVMeshSystem *sys, const struct disk_range *pr, u32 slba, const int rand_seed) {
	t_scrub_tester_set_inject_ptrs(st, sys, pr, slba);
	st->ind = ((rand_seed / 2) % pr->replicas);
	st->type = (rand_seed % 2) ? CORRUPT_NON_IDENTICAL : CORRUPT_BAD_SECT_LOGICAL;
}

static void t_scrub_tester_reinit_to(struct t_scrub_tester_r1 *st, u32 slba, enum corruption_type t) {
	t_scrub_tester_set_inject_ptrs(st, st->sys, st->pr, slba);
	st->ind = ~0;		// Invalid
	st->type = t;
}

static void t_scrub_tester_set_expectors(struct t_scrub_tester_r1 *st) {
	const int n_blocksets_to_recover = (1<<LOCK_CHANGE_STRIDE_SHIFT);	// This many syncs in 1 recovery
	st->n_total_syncs += n_blocksets_to_recover;
	st->n_expected_scrub_nop += (n_blocksets_to_recover - 1);			// We inject problem in 1 blockset only, the rest are nothing to do
	st->n_expected_scrubs += (st->type == CORRUPT_NON_IDENTICAL);
	st->n_bad_sectors += corruption_type_is_bad_sector(st->type);
}

static void t_scrub_tester_generate_valid_slice(struct t_scrub_tester_r1 *st) { // Generate full slice of data + parities + all edics
	u32 edic, debug_di_enabled = 0;
	extern u32 nvmeibc_calculate_edic_from_data_and_rlba(const u64 rlba, const unsigned char *data, const bool debug_di_enabled);
	__unitest_fill_blocks_unique_pattern(st->correct.block, 1);
	__unitest_fill_blocks_unique_pattern(st->corrupt.block, 1);
	edic = nvmeibc_calculate_edic_from_data_and_rlba(st->slba, st->correct.block, debug_di_enabled);
	nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_r1(&st->correct.md, edic);
	edic = nvmeibc_calculate_edic_from_data_and_rlba(st->slba, st->corrupt.block, debug_di_enabled);
	nvmeibc_block_dp_ec_md_set_externally_written_by_nvck_r1(&st->corrupt.md, edic);
}

static void t_scrub_tester_inject_valid_slice(struct t_scrub_tester_r1 *st) {
	int i, n_segs = st->pr->replicas;
	for (i = 0; i < n_segs; i++) {
		struct block_inject_ptrs *bp = &st->bptrs[i];
		memcpy(bp->data, st->correct.block, NVMEIBC_SECTOR_SIZE);
		bp->dmd->raw = st->correct.md.raw;
	}
}

static void t_scrub_tester_inject_bad_sector(struct t_scrub_tester_r1 *st, int seg_i) {
	struct block_inject_ptrs *bp = &st->bptrs[seg_i];
	if (st->type == CORRUPT_WRONG_EDIC)
		bp->dmd->P.edic ^= 0x2;	// Alter 1 bit of edic
	else if (st->type == CORRUPT_BAD_SECT_LOGICAL) {
		nbdpec_md_mark_data_invalid_for_read(bp->dmd, ((u32)seg_i >= st->pr->slice_size), NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS);
	} else if (st->type == CORRUPT_BAD_SECT_NEVER_WRITTEN) {
		bp->dmd->raw = nvmeibc_ec_unwritten_md_entry_val.raw;
		nbdpec_md_mark_data_invalid_for_read(bp->dmd, ((u32)seg_i >= st->pr->slice_size), NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS);
	} else if (st->type == CORRUPT_BAD_SECT_PHYS) {
		const raid_sgmnt_t di = (seg_i + st->lockset_owner_seg) % st->pr->replicas;		// Convert role to seg
		const u64 dlba = st->slba + st->pr[di].dlba_start;
		ramDiskSimulator_do_bad_sector(&st->sys->servers[di].ramDisk, dlba, EPERM_READ_FAIL_NO_RETRY);
	} else { BUG(); }
}

static void t_scrub_tester_inject_corrupt_block(struct t_scrub_tester_r1 *st, int seg_i) {
	struct block_inject_ptrs *bp = &st->bptrs[seg_i];
	memcpy(bp->data, st->corrupt.block, NVMEIBC_SECTOR_SIZE);
	bp->dmd->raw = st->corrupt.md.raw;
}

static void t_scrub_tester_verify_fixed_r1(struct t_scrub_tester_r1 *st)
{ 	// Bad sector in data fixed and correct edic was recalculated
	const union nvmeibc_block_dp_ec_data_block_md* dmd = st->bptrs[st->ind^0].dmd;
	const union nvmeibc_block_dp_ec_data_block_md* oth = st->bptrs[st->ind^1].dmd;
	BUG_ON(dmd->raw != oth->raw);
	BUG_ON(*(u64*)st->bptrs[st->ind^0].data != *(u64*)st->bptrs[st->ind^1].data);
	if (corruption_type_is_bad_sector(st->type)) {				// Fixup direction is always from good source to bad
		BUG_ON((dmd->raw != st->correct.md.raw) || is_data_invalid_for_read(dmd));	// Result is correct block
	} else if (st->type == CORRUPT_NON_IDENTICAL) {	// Fixup depending on the direction
		const bool is_source_corrupted = (st->lockset_owner_seg == st->ind);
		const union nvmeibc_block_dp_ec_data_block_md* expected = (is_source_corrupted ? &st->corrupt.md : &st->correct.md);
		BUG_ON(dmd->raw != expected->raw);
	} else if (st->type == ALREADY_FIXED) {			// Do nothing
	} else { BUG();	}
}

#include "block/recovery/nvmeibc_block_dp_sync_no_write_hole.h"
TEST_FUNC int unitest_scrubRecovery_R1(bunitest_s* B) {
	struct NVMeshSystem *sys = B->sys;
	struct TstPRaid sraid = NVMeshSystem_TstPRaid_init_rel(B->sys, (const struct volume_segment_index){0,0,0,0});
	struct toma_recovery_args rcvr_args = { .type = NVMEIBT_RECOVERY_TYPE_SCRUBBING, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA };
	struct t_scrub_tester_r1 st;
	struct test_context env = {.sys = sys, .client = sys->clients, .dev = sys->clients->devs[sraid.vsi.volume], .sraid = sraid};
	const struct disk_range *pr = env.sraid.cpr;
	int rep, rand_seed = (u32)(jiffies);	// Store it to be able to reproduce random sequence
	srand(rand_seed);
	memset(&st, 0, sizeof(st));
	__dd_clean_dlba_pointers(env);
	nvmeibc_nowhole_stats_reset();
	rcvr_args.ext_args.lock_range.override = true;								// Segments have 8 blocksets, for now run recovery on only 2 segments, for simplicity
	rcvr_args.ext_args.lock_range.start = 0;
	rcvr_args.ext_args.lock_range.count = 2 + 0 * sraid.cpr->length/LOCKSET_SLICES;
	NVMeshSystem_volume_memset(sys, 0, 0);
	BUG_ON(!env.dev->dp.enable_edic_check);		// If metadata is not enabled then this test is meaningless
	nvmeibc_warn_on_edic_verification_failure = false;							// Unitest cases edic failure
	for (rep = 0; rep < 50; rep++) {
		const u32 n_blksets_in_praid = rcvr_args.ext_args.lock_range.count;
		const u32 blockset = (rand()%n_blksets_in_praid);
		const u32 slice =    (rand()%LOCKSET_SLICES);
		t_scrub_tester_reinit_random(&st, sys, pr, (slice + blockset * LOCKSET_SLICES), rep);
		t_scrub_tester_set_expectors(&st);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		if (corruption_type_is_bad_sector(st.type)) {
			t_scrub_tester_inject_bad_sector(&st, st.ind);
		} else {
			t_scrub_tester_inject_corrupt_block(&st, st.ind);
		}
		// } if (1) {	// Todo: Instead of many iterations, inject a few problems to single blockset
		// unitest_print("-----------------------Corrupt: slice=%d, slba=%u, ind=0x%x, rep=%u, err=%c\n", slice, st.slba, st.ind, rep, st.type);
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
		t_scrub_tester_verify_fixed_r1(&st);
		{	// Verify correct flow
			struct nvmeibc_nowhole_stats stats;
			nvmeibc_nowhole_stats_get(&stats);
			BUG_ON(atomic_read(&stats.n_scrub_fix) != st.n_expected_scrubs);
			BUG_ON(atomic_read(&stats.n_bdsec_fix) != st.n_bad_sectors);
			BUG_ON(atomic_read(&stats.n_other_fix) != st.n_expected_scrub_nop);
		}
	}
	clientSimulator_wait_for_all_sync_ops(env.client);
	NVMeshSystem_serialize(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));

	// --------------------------------------------- Slice by slice mode with multiple failures in blockset
	__dd_clean_dlba_pointers(env);		// Wipevolume metadata to make it unwritten
	nvmeibc_nowhole_stats_reset();
	rcvr_args.ext_args.lock_range.count = 1;	// Working only on 1 blockset
	{
		const u32 blockset = 0;					// Use first blockset
		u32 failed_slices = 0, fixed_slices = 0;	// Bitmpas of expected failed / fixed slices.

		// Slice 31 OK (Never written)
		u32 slice = 30;	// Slice {BAD_SECT,OK}	- Will be fixed, logical bad sector removed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_LOGICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		fixed_slices |= (1 << slice);

		slice = 28;	// Slice {OK, BAD_SECT}		- Will be fixed, physical bad sector removed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_PHYS);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 1);
		fixed_slices |= (1 << slice);

		slice = 26;	// Slice {OK, Wrong Parity}  - Scrubbing will replace parity with D0
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_LOGICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_corrupt_block(&st, 1);
		fixed_slices |= (1 << slice);

		slice = 25;	// Slice {Corrupted, Parity OK}- Scrubbing will replace parity with D0
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_NON_IDENTICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_corrupt_block(&st, 0);
		fixed_slices |= (1 << slice);

		slice = 24;	// Slice {BAD_SECT, BAD_SECT}	// Never written slice with 2 bad sectors. Slice destroyed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_NEVER_WRITTEN);
		t_scrub_tester_inject_bad_sector(&st, 0);
		t_scrub_tester_inject_bad_sector(&st, 1);
		failed_slices |= (1 << slice);

		slice = 23;	// Slice {BAD_SECT, WRONG_EDIC}	Written slice with 2 failures. Slice destroyed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_LOGICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		st.type = CORRUPT_WRONG_EDIC;
		t_scrub_tester_inject_bad_sector(&st, 1);
		failed_slices |= (1 << slice);

		slice = 22;	// Slice {WRONG_EDIC, BAD_SECT}	Written slice with 2 failures. Slice destroyed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_PHYS);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 1);
		st.type = CORRUPT_WRONG_EDIC;
		t_scrub_tester_inject_bad_sector(&st, 0);
		failed_slices |= (1 << slice);

		slice = 20;	// Slice {WRONG_EDIC, OK} - Slice will be fixed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_WRONG_EDIC);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		st.type = CORRUPT_WRONG_EDIC;
		t_scrub_tester_inject_bad_sector(&st, 0);
		fixed_slices |= (1 << slice);

		slice = 18;	// Slice {WRONG_EDIC, WRONG_EDIC OK}- Slice destroyed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_WRONG_EDIC);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		t_scrub_tester_inject_bad_sector(&st, 1);
		failed_slices |= (1 << slice);
		// The rest of slices is OK

		// Set expectors
		tomaSimulator_expectIOFailure(&sys->servers[pr[st.lockset_owner_seg^0].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, failed_slices, fixed_slices);
		tomaSimulator_expectIOFailure(&sys->servers[pr[st.lockset_owner_seg^1].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, failed_slices, fixed_slices);

		unitest_print("-----------------------Corrupt: MultiSlice\n");
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
		{	// Verify correct flow
			struct nvmeibc_nowhole_stats stats;
			nvmeibc_nowhole_stats_get(&stats);
			BUG_ON(atomic_read(&stats.n_destoyed)  != 1);	// Some slices were destroyed in 1 blockset
			BUG_ON(atomic_read(&stats.n_bdsec_fix) != 1);	// Some slices were fixed     in 1 blockset
		}
		{
			const ulong slices_to_verify = (failed_slices | fixed_slices) | 0xF000000F;
			st.type = ALREADY_FIXED; st.ind = 0;
			for_each_set_bit(slice, &slices_to_verify, 32) {
				t_scrub_tester_set_inject_ptrs(&st, sys, pr, (slice + blockset * LOCKSET_SLICES));
				t_scrub_tester_verify_fixed_r1(&st);
			}
		}
		unitest_print("-----------------------Corrupt: MultiSlice2\n");
		tomaSimulator_expectIOFailure(&sys->servers[pr[st.lockset_owner_seg^0].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, failed_slices, 0);
		tomaSimulator_expectIOFailure(&sys->servers[pr[st.lockset_owner_seg^1].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, failed_slices, 0);
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
	}

	// --------------------------------------------- Slice by slice mode without failure
	__dd_clean_dlba_pointers(env);		// Wipevolume metadata to make it unwritten
	nvmeibc_nowhole_stats_reset();
	{
		const u32 blockset = 0;					// Use first blockset
		u32 fixed_slices = 0;	// Bitmpas of expected fixed slices.

		u32 slice = 31;	// Slice {BAD_SECT,OK}	- Will be fixed, logical bad sector removed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_LOGICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		fixed_slices |= (1u << slice);

		slice = 30;	// Slice {OK, BAD_SECT}		- Will be fixed, physical bad sector removed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_PHYS);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 1);
		fixed_slices |= (1u << slice);

		slice = 29;	// Slice {OK, Wrong Parity}  - Scrubbing will replace parity with D0
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_BAD_SECT_LOGICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_corrupt_block(&st, 1);
		fixed_slices |= (1u << slice);

		slice = 28;	// Slice {Corrupted, Parity OK}- Scrubbing will replace parity with D0
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_NON_IDENTICAL);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_corrupt_block(&st, 0);
		fixed_slices |= (1u << slice);

		slice = 27;	// Slice {WRONG_EDIC, OK} - Slice will be fixed
		t_scrub_tester_reinit_to(&st, (slice + blockset * LOCKSET_SLICES), CORRUPT_WRONG_EDIC);
		t_scrub_tester_generate_valid_slice(&st);
		t_scrub_tester_inject_valid_slice(&st);
		t_scrub_tester_inject_bad_sector(&st, 0);
		fixed_slices |= (1u << slice);
		// The rest of slices is OK

		// Set expectors
		tomaSimulator_expectIOFailure(&sys->servers[pr[st.lockset_owner_seg^0].node_id].simToma, EPERM_READ_FAIL, 0, fixed_slices);
		tomaSimulator_expectIOFailure(&sys->servers[pr[st.lockset_owner_seg^1].node_id].simToma, EPERM_READ_FAIL, 0, fixed_slices);

		unitest_print("-----------------------Corrupt: MultiSlice-OK\n");
		BUG_ON(tomaSimulator_recoverThing(sraid.tpr, sraid.cpr + st.lockset_owner_seg, rcvr_args) < 0);
		{	// Verify correct flow
			const ulong slices_to_verify = fixed_slices;
			struct nvmeibc_nowhole_stats stats;
			nvmeibc_nowhole_stats_get(&stats);
			BUG_ON(atomic_read(&stats.n_destoyed)  != 0);	// None slices were destroyed in 1 blockset
			BUG_ON(atomic_read(&stats.n_bdsec_fix) != 1);	// Some slices were fixed     in 1 blockset
			st.type = ALREADY_FIXED; st.ind = 0;
			for_each_set_bit(slice, &slices_to_verify, 32) {
				t_scrub_tester_set_inject_ptrs(&st, sys, pr, (slice + blockset * LOCKSET_SLICES));
				t_scrub_tester_verify_fixed_r1(&st);
			}
		}
	}

	if (1) { // --------------------------------------------- Scrubbing on Jbod
		struct TstPRaid sjbod = NVMeshSystem_TstPRaid_init_rel(B->sys, (const struct volume_segment_index){1,0,0,0});
		struct nvmeibc_nowhole_stats * nowhole_stats = &nvmeibc_flow_counters_ref()->nowh;
		env.dev = env.client->devs[sjbod.vsi.volume];
		env.sraid = sjbod;
		pr = env.sraid.cpr;
		BUG_ON(pr->replicas != 1);		// Jbod
		// unitest_print("-----------------------Corrupt: Jbod\n");
		// nvmeibc_nowhole_stats_reset();
		BUG_ON(tomaSimulator_recoverThing(sjbod.tpr, sjbod.cpr, rcvr_args) < 0);
		BUG_ON(atomic_read(&nowhole_stats->n_other_fix) != 0);	// Recoveries on jbod a re disabled
	}

	clientSimulator_wait_for_all_sync_ops(env.client);
	nvmeibc_warn_on_edic_verification_failure = true;
	NVMeshSystem_serialize(sys);
	nvmeibc_nowhole_stats_reset();
	__dd_clean_dlba_pointers(env);				// Clean bad sectors
	NVMeshSystem_volume_memset(sys, 0, 0);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	return 0;
}

struct sync_stale_sync_io_ctx {		// data we need for the IO which we expect to make the sync
	u8 	pref;		// the value of byte preceding the IO extent on the segment from which we expect to do the sync.
	u8	post;		// the value of byte following the IO extent on the segment from which we expect to do the sync.
};

static const u8 STALE_SYNC_EXTENT_GUARD = 0x69;
static void PUT_IO_EXTENT_PRE_POST_SIG(struct sync_stale_sync_io_ctx *ext, bool is_lockset_sync, u8 *dst, u8 *mirrored_dst, int lenBlocks) {
	const int len = NVMEIBC_SECTOR2BYTE(lenBlocks);
	ext->pref = dst[ -1];
	ext->post = dst[len];
	dst[ -1] ^= STALE_SYNC_EXTENT_GUARD;
	dst[len] ^= STALE_SYNC_EXTENT_GUARD;
	if (is_lockset_sync) { 										// verify that we can identify the issues we check after the sync, LOCKSET sync *fix* bytes before/after io extent
		BUG_ON(dst[ -1] == mirrored_dst[-1]);					// we need them different, so we can identify the full LOCKSET sync was made.
		BUG_ON(dst[len] == mirrored_dst[len]);
	}
}

static void VERIFY_IO_EXTENT_PRE_POST_SIG(struct sync_stale_sync_io_ctx *ext, bool is_lockset_sync, u64 physStart, u8 *dst, u8 *mirrored_dst, int lenBlocks, struct serverSimulator *server) {
	const int len = NVMEIBC_SECTOR2BYTE(lenBlocks);
	u64 phys_addr = __to4K(physStart);
	if (is_lockset_sync) { 										// LOCKSET sync *fix* bytes before/after io extent
		BUG_ON(dst[-1] 	!= mirrored_dst[-1]);
		BUG_ON(dst[len] != mirrored_dst[len]);
	} else { 													// minimal sync *doesnt* fix bytes before/after io extent
		ramDiskSimulator_lockUnSta(&server->ramDisk, phys_addr);				// Verify stale lock was NOT cleared & clear the stale lock leftover
		BUG_ON(dst[-1] 	!= (ext->pref ^ STALE_SYNC_EXTENT_GUARD));
		BUG_ON(dst[len] != (ext->post ^ STALE_SYNC_EXTENT_GUARD));
		dst[ -1] = ext->pref;									// restore valid data
		dst[len] = ext->post;
	}
	ramDiskSimulator_verify_no_locks(&server->ramDisk);			// Verify stale lock was indeed cleared
}

/* save original data where we put signature, so we can revert corruption */
static inline void SAVE_EXTENT_SIGNATURE(u8 *dst1, u8 *dst2, int len /* io size bytes*/, u64 ext_sig[4*2]){
	ext_sig[0*2+0] = dst1[   -1];   ext_sig[0*2+1] = dst2[   -1];
	ext_sig[1*2+0] = dst1[    0];   ext_sig[1*2+1] = dst2[    0];
	ext_sig[2*2+0] = dst1[len-1];   ext_sig[2*2+1] = dst2[len-1];
	ext_sig[3*2+0] = dst1[len  ];   ext_sig[3*2+1] = dst2[len  ];
}

static inline void RESTORE_EXTENT_SIGNATURE(u8 *dst1, u8 *dst2, int len /* io size bytes*/, u64 ext_sig[4*2]){
	dst1[   -1] = ext_sig[0*2+0];   dst2[   -1] = ext_sig[0*2+1];
	dst1[    0] = ext_sig[1*2+0];   dst2[    0] = ext_sig[1*2+1];
	dst1[len-1] = ext_sig[2*2+0];   dst2[len-1] = ext_sig[2*2+1];
	dst1[len  ] = ext_sig[3*2+0];   dst2[len  ] = ext_sig[3*2+1];
}

static inline void SET_EXTENT_SIGNATURE(u8 *dst1, u8 *dst2, int len /*bytes*/) {
	dst1[   -1] = 0x34;				dst2[   -1] = ~dst1[   -1];
	dst1[    0] = 0x45;				dst2[    0] = ~dst1[    0];
	dst1[len-1] = 0x56;				dst2[len-1] = ~dst1[len-1];
	dst1[len  ] = 0x67;				dst2[len  ] = ~dst1[len  ];
}

// with locket  sync, verify the data is identical within extent as well as outside the extent.
// with minimal sync, verify the data is identical within extent yet different outside the extent
static inline void VERIFY_EXTENT_SIGNATURE(u8 *dst1, u8 *dst2, int len /*bytes*/, bool is_pre_sig_id, bool is_post_sig_id) {
	BUG_ON(dst1[0    ] != dst2[0    ]);
	BUG_ON(dst1[len-1] != dst2[len-1]);
	if (is_pre_sig_id)  { BUG_ON(dst1[ -1] != dst2[ -1]);							}
	else			    { BUG_ON(dst1[ -1] == dst2[ -1]);	dst2[ -1] = dst1[ -1];	}
	if (is_post_sig_id) { BUG_ON(dst1[len] != dst2[len]);							}
	else				{ BUG_ON(dst1[len] == dst2[len]);	dst2[len] = dst1[len];	}
}

// test various {offset,size} within a LOCKSET, to verify that we get the correct data into the READ IO buffer
// Note that the below {offset,size} will create IO's on first LOCKSET, second LOCKSET & some spanning both.
// each IO is executed twice: once with minimal sync & then with full LOCKSET sync. the test verifies that both yield the same data
// TODO(EBA):
// 1) consider loop with/out difference in replicas so write phase is executed/skipped.
// 2) consider an IO extent that spans multiple LOCKSET
// 3) consider testing an IO that spans across LOCKSET_SLICES.
TEST_FUNC int unitest_SyncStaleLockVerifyData(bunitest_s* B, const int volInd, const int seg_ind, struct disk_range *curSeg, struct disk_range *mirror_seg) {
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator 	*client = &sys->clients[0];				// Test via the first client
	const int max_n_owners = sys->mdb.vols[volInd].locks_scheme.maxNOwners;
	const u64 io_offsets[] = {0,1,3,8,17,LOCKSET_SLICES-1}; 	// IO offset into the BLKSET. dont use offset 0 so we can verify the signature *before* the extent.
	const u64	io_sizes[] = {1,2,3,7,11,LOCKSET_SLICES-1,LOCKSET_SLICES};
	const int lockset_prob_backup = nvmeibc_sync_full_lockset_probability_factor;
	u64	ext_sig[4*2];
	u8	*mem_min_sync  = sim_kmalloc(BYTES_IN_LOCKSET, GFP_KERNEL);	// the data we read when a minimal sync was done
	u8	*mem_full_sync = sim_kmalloc(BYTES_IN_LOCKSET, GFP_KERNEL);	// the data we read when a full sync was done
	u8	*dst, *mirrored_dst;
	uint io_offset_ind, io_size_ind, step = (ut_conf__get_base()->is_valgrind) ? 3 : 1;				// When valgrind is active, do less iterations
	int	 rv;

	for (  io_offset_ind = 0; io_offset_ind < ARRAY_SIZE(io_offsets); io_offset_ind += step) {
		for (io_size_ind = 0;   io_size_ind < ARRAY_SIZE(  io_sizes); io_size_ind +=   step) {
			struct serverSimulator *server = &sys->servers[curSeg->node_id], *mirror_server;
			const u64 io_offset = io_offsets[io_offset_ind];
			const int io_size = min(io_sizes[io_size_ind], (int)LOCKSET_SLICES - io_offset), io_size_b = NVMEIBC_SECTOR2BYTE(io_size);
			const int n_mirror = (int)curSeg->replicas;
			const u64 ls_offset = __offset_to_put_owner_lock_on_seg(seg_ind, n_mirror, max_n_owners);
			const u64 small_offset= io_offset;						// Offset to write somwhere in the middle of the lockset
			const u64 virtStart = __from4K(disk_range_get_start_addr(curSeg)) + (ls_offset*curSeg->stripe_width) + small_offset;
			const u64 physStart = __from4K(curSeg->dlba_start)				      +  ls_offset					     + small_offset;
			dst = &server->ramDisk.mem[COMMITTED_ADDR_AS(&server->ramDisk, physStart, SECTOR, BYTE)];

			_ND(trace_bunitest_unitest_SyncStaleLockVerifyData, "-------------------io_offset=@IO_OFFSET, io_size=@INT, First Read (Full LOCKSET sync) ------------------", io_offset, io_size);
			mirror_server = &sys->servers[mirror_seg->node_id];
			mirrored_dst = &mirror_server->ramDisk.mem[COMMITTED_ADDR_AS(&mirror_server->ramDisk,  __from4K(mirror_seg->dlba_start) + ls_offset + small_offset, SECTOR, BYTE)];
			SAVE_EXTENT_SIGNATURE(dst, mirrored_dst, io_size_b, ext_sig);

			SET_EXTENT_SIGNATURE(dst, mirrored_dst, io_size_b);
			nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET;		// force full LOCKSET sync
			ramDiskSimulator_lockStale(&server->ramDisk, __to4K(physStart));
			rv = osSimulator_readArr(&client->OS, volInd, virtStart, io_size, mem_full_sync);	REPORT_ERROR(rv);	// Sync operation
			clientSimulator_wait_for_all_bio_ops(client);						// IO should terminate fast because sync operation cleaned the stale lock
			BUG_ON(ramDiskSimulator_lockIsSta(&server->ramDisk, __to4K(physStart)));
			VERIFY_EXTENT_SIGNATURE(dst, mirrored_dst, io_size_b, (io_offset > 0), (io_offset+io_size < LOCKSET_SLICES/*single LOCKSET?*/));							// verify that the sync fixed mismatch between replicas

			_ND(trace_1_bunitest_unitest_SyncStaleLockVerifyData, "-------------------io_offset=@IO_OFFSET, io_size=@INT, Second Read (Minimal LOCKSET sync) ------------------", io_offset, io_size);
			SET_EXTENT_SIGNATURE(dst, mirrored_dst, io_size_b);
			nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_MIN;			// force minimal sync
			ramDiskSimulator_lockStale(&server->ramDisk, __to4K(physStart));
			rv = osSimulator_readArr(&client->OS, volInd, virtStart, io_size, mem_min_sync);	REPORT_ERROR(rv);	// Sync operation
			clientSimulator_wait_for_all_bio_ops(client);						// IO should terminate fast because sync operation cleaned the stale lock
			VERIFY_EXTENT_SIGNATURE(dst, mirrored_dst, io_size_b, false, false);					// verify that the sync fixed mismatch between replicas
			BUG_ON(__mem_find_first_mismatch_8(mem_full_sync, mem_min_sync, io_size) != -1);		// compare the data we read with minimal/lockset sync.
			if (io_size == LOCKSET_SLICES) {
				BUG_ON( ramDiskSimulator_lockIsSta( &server->ramDisk, __to4K(physStart)));	// io size is whole lockset -> it clears the stale-special
			} else {
				ramDiskSimulator_lockUnSta(			&server->ramDisk, __to4K(physStart));	// io size < lockset so it does **NOT** clear the stale-special
			}

			// restore original data & trigger replication to all replica's match
			// with multiple replicas, trigger sync to copy owner to all replicas bcz we restored only 2 copies
			RESTORE_EXTENT_SIGNATURE(dst, mirrored_dst, io_size_b, ext_sig);
			if (B->test_phase <= BUNI_ERASURE_CODING_TESTING) {
				nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET;		// force full LOCKSET sync
				ramDiskSimulator_lockStale(&server->ramDisk, __to4K(physStart));
				rv = osSimulator_readArr(&client->OS, volInd, virtStart, io_size, mem_full_sync);	REPORT_ERROR(rv);	// Sync operation
				clientSimulator_wait_for_all_bio_ops(client);						// IO should terminate fast because sync operation cleaned the stale lock
				BUG_ON(ramDiskSimulator_lockIsSta(&server->ramDisk, __to4K(physStart)));
			}
		}
	}

	sim_kfree(mem_min_sync);
	sim_kfree(mem_full_sync);
	nvmeibc_sync_full_lockset_probability_factor = lockset_prob_backup;		// restore value
	return rv;
}

/* When IO operation encounters a stale lock it should overccome it by synching the data between mirrored disks and removing the stale lock */
TEST_FUNC int unitest_SyncStaleLocks(bunitest_s* B){
	struct NVMeshSystem *sys = B->sys;
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int s, ei, c, flr, volInd, is_lockset_sync, rv = 0, lenBlocks = 1;
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;			// Total array in bytes
	const int io_offset = 7;
	u8       *dst = NULL, *mirrored_dst, *mem = sim_kmalloc(max(memSize,BYTES_IN_LOCKSET), GFP_KERNEL);
	struct sync_stale_sync_io_ctx	extent_info;
	u64       magic_pattern;    								// unique 64b signaturre filling the array
	bool prev_mirror_edic;
	struct nvmeibc_nowhole_stats *nowhole_stats = &nvmeibc_flow_counters_ref()->nowh;
	atomic_t *which = &nowhole_stats->n_other_fix;
	BUG_ON((io_offset < 1) ||							// so we can put a signature in block preceding IO extent.
		   (io_offset + lenBlocks >= LOCKSET_SLICES-1)); 	// so we can put a signature in block post 		IO extent.
	// verify probability decision
	nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_DEFAULT;
	BUG_ON(nvmeibc_sync_is_trigger_full_blkset_sync(LOCKSET_SLICES) == false);        // IO of whole LOCKSET size must yield a LOCKSET sync.
	nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET;	// this mode MUST yield a LOCKSET sync decision for any io size
	for (s=1; s <= LOCKSET_SLICES; s++) {
		BUG_ON(nvmeibc_sync_is_trigger_full_blkset_sync(s) == false);
	}
	nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_MIN;	// this mode MUST yield a minimal sync decision for any io size
	for (s=1; s <= LOCKSET_SLICES; s++) {
		BUG_ON(nvmeibc_sync_is_trigger_full_blkset_sync(s) == true);
	}

	prev_mirror_edic = NVMeshSystem_all_clients_mirror_edic(sys, false); // This test will corrupt data intentionally, hece edic check is pointless
	nvmeibc_nowhole_stats_reset();
	// ------------------------------------ Write on every segment of Raid1 and Raid10 volumes with a stale lock
	for (volInd=0; volInd<sys->mdb.nVols; volInd++) {
		struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];
		const int max_n_owners                 = sys->mdb.vols[volInd].locks_scheme.maxNOwners;
		struct disk_range *first_seg_in_chunk = &sys->mdb.vols[volInd].segs[0];
		struct disk_range *first_seg_in_raid;
		int n_mirror = 0, stripe_width = 0, raid_index;
		if (!tTopoOfVolume_isMirrored(&sys->tcf.vols[volInd]))
			continue;	// no stale locks
		if ((B->test_phase == BUNI_ERASURE_CODING_TESTING) && (volInd>0))
			continue;	// Only vol 0 is relevant

		NVMeshSystem_volume_memset(sys, volInd, 0x12);
		for (c=0, raid_index=0; c<cfv->nChunks; c++, raid_index+=stripe_width, first_seg_in_chunk += n_mirror*stripe_width) {
			n_mirror = (int)first_seg_in_chunk->replicas; stripe_width = (int)first_seg_in_chunk->stripe_width;
			_ND(trace_bunitest_unitest_SyncStaleLocks, "Testing Stale-Lock for @VOL_I, chunk_idx=@CHUNK_IDX", volInd, c);
			for (s = 0; s < (n_mirror * stripe_width); s++) {
				// ---------------------------- Test in RW, RW mode
				struct tTopoOfPraid* r1 = tTopoOfVolume_getRaid1(cfv, raid_index + s/n_mirror);
				struct disk_range* curSeg[2]   = { &first_seg_in_chunk[s], curSeg[0] + (((s % n_mirror) < (n_mirror-1)) ? +1 : -1)};	// corrupt on last member
				struct serverSimulator *server[2] = {&sys->servers[curSeg[0]->node_id],
													 &sys->servers[curSeg[1]->node_id]};
				const u64 ls_offset = __offset_to_put_owner_lock_on_seg(s, n_mirror, max_n_owners);	// Below calculation is done in units of blocks, not 4K
				const u64 small_offset= __from4K(io_offset+s);						// Offset to write somwhere in the middle of the lockset
				const u64 virtStart = __from4K(disk_range_get_start_addr(curSeg[0])) + (ls_offset*curSeg[0]->stripe_width)+ small_offset;
				const u64 physStart_abs[2]= {__from4K(curSeg[0]->dlba_start)			 +  ls_offset						  + small_offset,
										 	 __from4K(curSeg[1]->dlba_start)			 +  ls_offset						  + small_offset };	// For dual locked second segment
				const u64 physStart_rel[2]= {COMMITTED_ADDR(&server[0]->ramDisk, physStart_abs[0], SECTOR),
											 COMMITTED_ADDR(&server[1]->ramDisk, physStart_abs[1], SECTOR)};
				if ((s%n_mirror>=max_n_owners))
					continue;											// This is not a locking segment so it is irrelevant
				if (ls_offset+small_offset > __from4K(curSeg[0]->length))
					continue; // Raid1 is too short (all the owner locks are on the first segment)
				mirrored_dst = &server[1]->ramDisk.mem[COMMITTED_ADDR_AS(&server[1]->ramDisk, __from4K(curSeg[1]->dlba_start) + small_offset + ls_offset, SECTOR, BYTE)];
				for (is_lockset_sync=false; is_lockset_sync <= true; is_lockset_sync++) {
					const bool sync_runs_on_write =   (is_lockset_sync && (n_mirror >= 2));				// Write will launch sync
					const bool set_copy_owner_stale = (is_lockset_sync && (n_mirror == 2));				// In N-replica we test various lock-schemes and it is hard to know where non primary locks reside
					if (is_lockset_sync)
						nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET; // force sync of whole LOCKSET
					else
						nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_MIN; 	// force sync of minimal extent
					first_seg_in_raid= first_seg_in_chunk + ((s/n_mirror)*n_mirror);
					BUG_ON(!tTopoOfPraid_verify_consistency(sys, r1, first_seg_in_raid));
					magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
					ramDiskSimulator_lockStale(&server[0]->ramDisk, __to4K(physStart_abs[0]));
					if (set_copy_owner_stale) ramDiskSimulator_lockStale(&server[1]->ramDisk, __to4K(physStart_abs[1]));		// Simulate that Toma also converts second lock to stale (Active-to-owner)
					dst = &server[0]->ramDisk.mem[physStart_rel[0]*NVMEIBC_SECTOR_SIZE];
					if ((first_seg_in_chunk + s - sys->mdb.vols[volInd].segs)%3==0)
						dst[0] = mem[0]^0xFF;								// Generate artificial descrepancy (on member 0) between 2 sides of raid
					if (is_lockset_sync) {
						ramDiskSimulator_do_bad_sector(&server[0]->ramDisk, physStart_abs[0]-2, tested_error_codes[0]);
					}
					PUT_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, dst, mirrored_dst, lenBlocks);
					rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);
					NVMeshSystem__detectStuckIOs(sys);						// IO should terminate fast because sync operation cleaned the stale lock
					__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
					VERIFY_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, physStart_abs[0], dst, mirrored_dst, lenBlocks, server[0]);
					if (set_copy_owner_stale) BUG_ON(ramDiskSimulator_is_locked(&server[1]->ramDisk, __to4K(physStart_abs[1])));	// Client cleaned Tomas active-to-owner lock
					BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);

					if (is_lockset_sync) {
						BUG_ON(ramDiskSimulator_is_bad_sector(&server[0]->ramDisk, physStart_abs[0]-2));
					}

					if ((c>0)||(s>0))
						continue;											// Too long to test the below code on every segment
					// ---------------------------- Test Sync Stale lock in degraded Mode (RW,Dead)
					serverSimulator_disconnect(server[1]);
					tomaSimulator_unreg_raid1(r1uuid(r1), 1);

					magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
					ramDiskSimulator_lockStale(&server[0]->ramDisk, __to4K(physStart_abs[0]));
					if (set_copy_owner_stale) ramDiskSimulator_lockStale(&server[1]->ramDisk, __to4K(physStart_abs[1]));		// Simulate that Toma also converts second lock to stale (Active-to-owner)
					rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// No sync operation, just writes dirty bits
					clientSimulator_wait_for_all_bio_ops(client);
					ramDiskSimulator_verify_no_locks(&server[0]->ramDisk);		// Verify stale lock was indeed cleared
					if (set_copy_owner_stale) ramDiskSimulator_lockUnSta(&server[1]->ramDisk, __to4K(physStart_abs[1]));		// Client could not cleaned Tomas active-to-owner lock

					ramDiskSimulator_lockStale(&server[0]->ramDisk, __to4K(physStart_abs[0]));
					if (set_copy_owner_stale) ramDiskSimulator_lockStale(&server[1]->ramDisk, __to4K(physStart_abs[1]));		// Simulate that Toma also converts second lock to stale (Active-to-owner)
					memset(mem, 0, memSize);
					rv = osSimulator_readArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation -> convert stale to dirty bit
					NVMeshSystem__detectStuckIOs(sys);						// IO should terminate fast because sync operation cleaned the stale lock
					ramDiskSimulator_verify_no_locks(&server[0]->ramDisk);		// Verify stale lock was indeed cleared
					__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
					if (set_copy_owner_stale) ramDiskSimulator_lockUnSta(&server[1]->ramDisk, __to4K(physStart_abs[1]));		// Client could not cleaned Tomas active-to-owner lock
					BUG_ON(atomic_sub_return(0, which) != 0);												// Because even if sync runs, it will be stale-2-dirtybit

					// ---------------------------- Test Sync Stale lock in Mode (RW,W) (Owner and Copy locks)
					serverSimulator_re_connect(server[1]);
					NVMeshSystem_serialize(sys);
					tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W, SW_TOPO__WAIT_ACK);
					magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
					if (volInd==0) dst[0] = mem[0]^0xFF;								// Generate artificial descrepancy between 2 sides of raid
					ramDiskSimulator_lockStale(&server[0]->ramDisk, __to4K(physStart_abs[0]));
					if (set_copy_owner_stale) ramDiskSimulator_lockStale(&server[1]->ramDisk, __to4K(physStart_abs[1]));		// Simulate that Toma also converts second lock to stale (Active-to-owner)
					PUT_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, dst, mirrored_dst, lenBlocks);
					rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation, RW/W
					clientSimulator_wait_for_all_bio_ops(client);
					VERIFY_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, physStart_abs[0], dst, mirrored_dst, lenBlocks, server[0]);
					if (set_copy_owner_stale) BUG_ON(ramDiskSimulator_is_locked(&server[1]->ramDisk, __to4K(physStart_abs[1])));// Client cleaned Tomas active-to-owner lock
					BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);
					__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);

					if (1) {		// --------- Disabled: test the correct extent sync when Read IO requires a lock (Daniel: Reads that take locks cannot do partial sync!)
						//tTopoOfPraid_force_lock_on_read(r1, true);
						//tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
						if (tTopoOfVolume_isMirrored(&sys->tcf.vols[volInd])) {
							unitest_SyncStaleLockVerifyData(B, volInd, s, curSeg[0], curSeg[1]);     // test the sync of each block within a LOCKSET
							atomic_set(which, 0);											// Too much syncs to verify them
						}
						tTopoOfPraid_force_lock_on_read(r1, false);
						tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
					}
					for (flr = false; flr <= true; flr++) {					// Test (W,RW) Dual lock with forced lock on read and without (without is an illegal topology but was legal in v1.0.4)
						tTopoOfPraid_force_lock_on_read(r1, flr);
						// ---------------------------- Test Sync Stale lock by Write IO in Dual lock mode (W,RW), when secondary is stale or owner is stale
						tomaSimulator_waitProtoEnd(NULL);			// wait for all toma msgs to be processed, completing the client <--> toma updates before we attempt switchTopo
						tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_W_NO_DIRTY, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
						for (ei = 0; ei <= 1; ei++) {				// Seg[0] bears secondary lock, Seg[1] bears the owner lock
							magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);   // Set a pattern.
							if (volInd==0) dst[0] = mem[0]^0xFF;									// Generate artificial descrepancy between 2 sides of raid
							ramDiskSimulator_lockStale(&server[ei]->ramDisk, __to4K(physStart_abs[ei]));
							PUT_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, dst, mirrored_dst, lenBlocks);
							rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation, RW/W
							clientSimulator_wait_for_all_bio_ops(client);

							VERIFY_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, physStart_abs[ei], dst, mirrored_dst, lenBlocks, server[ei]);
							__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
							BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);
						}

						// ---------------------------- Same as above but for Read IO
						for (ei = 0; ei <= 1; ei++) {
							const bool sync_runs_on_read = (flr || ei);	// When read detects the stale lock, and lanuches sync, that will now succeed, after latest changes
							ramDiskSimulator_lockStale(&server[ei]->ramDisk, __to4K(physStart_abs[ei]));
							rv = osSimulator_readArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);   	// Sync operation, RW/W
							if (sync_runs_on_read) {
								clientSimulator_wait_for_all_sync_ops(client);				// Wait for sync to run (either full blockset or 1 slice)
							}
							clientSimulator_wait_for_all_bio_ops(client);					// Read acquires locks and is able to launch sync operation or the line above cleaned the stale lock
							if (sync_runs_on_read) {
								if (is_lockset_sync || flr) {	// Sync will be full blockset and clan all locks
								} else {						// Sync operates onyl on some slices and leaves stale locks
									ramDiskSimulator_lockUnSta(&server[0]->ramDisk, __to4K(physStart_abs[0]));
									ramDiskSimulator_lockUnSta(&server[1]->ramDisk, __to4K(physStart_abs[1]));
								}
							} else{
								ramDiskSimulator_lockUnSta(&server[ei]->ramDisk, __to4K(physStart_abs[ei]));
							}
							BUG_ON(ramDiskSimulator_is_locked(&server[0]->ramDisk, __to4K(physStart_abs[0]))); // Verify all locks are clean
							BUG_ON(ramDiskSimulator_is_locked(&server[0]->ramDisk, __to4K(physStart_abs[1])));
							BUG_ON(atomic_sub_return((sync_runs_on_read ? 1 : 0), which) != 0);
						}

						// ---------------------------- Test Sync Stale lock in Dual Mode (W,RW) Dual lock when both are stale
						magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
						if (volInd==0) dst[0] = mem[0]^0xFF;									// Generate artificial descrepancy between 2 sides of raid
						ramDiskSimulator_lockStale(&server[0]->ramDisk, __to4K(physStart_abs[0]));
						ramDiskSimulator_lockStale(&server[1]->ramDisk, __to4K(physStart_abs[1]));
						PUT_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, dst, mirrored_dst, lenBlocks);
						rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation, RW/W
						clientSimulator_wait_for_all_bio_ops(client);
						VERIFY_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, physStart_abs[0], dst, mirrored_dst, lenBlocks, server[0]);
						if (is_lockset_sync) 										// LOCKSET sync *fix* bytes before/after io extent
							BUG_ON(ramDiskSimulator_is_locked(&server[1]->ramDisk,__to4K(physStart_abs[1])));	// Verify stale lock was indeed cleared
						else 														// minimal sync *doesnt* fix bytes before/after io extent
							ramDiskSimulator_lockUnSta(&server[1]->ramDisk, __to4K(physStart_abs[1]));						// clear the stale lock leftover
						__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
						BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);

						// ---------------------------- Test Sync Stale lock in Dual Mode (W,RW) Dual lock when owner is stale, another client holds the secondary
						magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
						if (volInd==0) dst[0] = mem[0]^0xFF;									// Generate artificial descrepancy between 2 sides of raid
						ramDiskSimulator_lockStale(&server[1]->ramDisk, __to4K(physStart_abs[1]));
						ramDiskSimulator_lockDo(   &server[0]->ramDisk, __to4K(physStart_abs[0]));
						PUT_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, dst, mirrored_dst, lenBlocks);
						rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation, RW/W, fails
						udelay(10);
						ramDiskSimulator_lockUn(   &server[0]->ramDisk, __to4K(physStart_abs[0]));	// Now second client unlocked the secondary owner, sync can complete
						clientSimulator_wait_for_all_bio_ops(client);
						VERIFY_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, physStart_abs[1], dst, mirrored_dst, lenBlocks, server[1]);
						__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);
						BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);

						for (ei = 0; ei < NUMBER_OF_ERROR_CODES; ei++) {
							const u64 read_fail_offset_abs[2] = {(physStart_abs[0] - ls_offset - small_offset), (physStart_abs[1] - ls_offset - small_offset) };
							const u64 read_fail_offset_rel[2] = {(physStart_rel[0] - COMMITTED_ADDR(&server[0]->ramDisk, curSeg[0]->dlba_start, SECTOR) - ls_offset - small_offset),
																 (physStart_rel[1] - COMMITTED_ADDR(&server[0]->ramDisk, curSeg[1]->dlba_start, SECTOR) - ls_offset - small_offset) };

							const u32 slice_rf_bitmap = (1u <<read_fail_offset_rel[0]);
							const bool can_rw_w_sync_solve_bad_sector = (true || (n_mirror > 2));		// Used to need 3 sources, now 2 is enough, because we dont inject dbits!
							// ---------------------------- Test Sync Stale lock in Dual Mode (W,RW) Dual lock, but the recovered side has permanent read fail. Sync should succeed
							magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
							ramDiskSimulator_lockStale(    &server[1]->ramDisk, __to4K(physStart_abs[1]));
							ramDiskSimulator_do_bad_sector(&server[0]->ramDisk, read_fail_offset_abs[0], tested_error_codes[ei]);
							PUT_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, dst, mirrored_dst, lenBlocks);
							rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation, RW/W, fails
							clientSimulator_wait_for_all_bio_ops(client);
							VERIFY_IO_EXTENT_PRE_POST_SIG(&extent_info, is_lockset_sync, physStart_abs[1], dst, mirrored_dst, lenBlocks, server[1]);
							if (is_lockset_sync) 												// LOCKSET sync *fix* bytes before/after io extent
								BUG_ON(ramDiskSimulator_is_bad_sector(&server[0]->ramDisk, read_fail_offset_abs[0]));
							else 																// minimal sync *doesnt* fix bytes before/after io extent - so bad sector isnt fixed
								ramDiskSimulator_un_bad_sector(       &server[0]->ramDisk, read_fail_offset_abs[0]);
							__unitest_verify_blocks_pattern(dst, lenBlocks, magic_pattern, true);  // ???
							BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);

							// ---------------------------- Test Sync Stale lock in Dual Mode (W,RW) Dual lock, but the recovering (owner) side has permanent read fail. Sync should fail
							magic_pattern = __unitest_fill_blocks_unique_pattern(mem, lenBlocks);	// Set a pattern.
							ramDiskSimulator_lockStale(    &server[1]->ramDisk, __to4K(physStart_abs[1]));
							ramDiskSimulator_do_bad_sector(&server[1]->ramDisk, read_fail_offset_abs[1], tested_error_codes[ei]);
							if (is_lockset_sync) {	// If 2 mirrosync_runsr (no sources). Lockset sync fails and updates TOMA. On 3+ sources will succeed
								if (!can_rw_w_sync_solve_bad_sector) {
									int i;
									for (i = 0; i < n_mirror; i++) { // Expect slice 0(1) failure
										tomaSimulator_expectIOFailure(&sys->servers[first_seg_in_raid[i].node_id].simToma, EPERM_READ_FAIL_NO_RETRY, slice_rf_bitmap, 0);
									}
								} else { // We have a good source that fixed the entire blockset so no report sent to toma
									//tomaSimulator_expectIOFailure(&server[1]->simToma, EPERM_READ_FAIL,          0, slice_rf_bitmap);
								}
							}
							rv = osSimulator_writeArr(&client->OS, volInd, virtStart, lenBlocks, mem);	REPORT_ERROR(rv);	// Sync operation, RW/W, fails
							clientSimulator_wait_for_all_bio_ops(client);
							rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);					// Verify IO failed, stale lock was not cleared
							if (!is_lockset_sync) { 		// Implicit sync is invoked, owner and secondary remain as were before IO
								BUG_ON(ramDiskSimulator_is_bad_sector(&server[1]->ramDisk, read_fail_offset_abs[1])!=tested_error_codes[ei]); // Verify bad sector was not cleared (since we do not write to owner)
								ramDiskSimulator_un_bad_sector(       &server[1]->ramDisk, read_fail_offset_abs[1]);
								BUG_ON(ramDiskSimulator_lockIsSta(&server[0]->ramDisk, __to4K(physStart_abs[0])));		// No Attempt of sync so secondary owner remains as it was
								ramDiskSimulator_lockUnSta(       &server[1]->ramDisk, __to4K(physStart_abs[1]));		// Stale lock on secondary was not cleared
								BUG_ON(rv != 0);
							} else {
								if (!can_rw_w_sync_solve_bad_sector) { // We cannot fix in 2 mirror so stales remain and error code is set to EPERM_READ_FAIL_NO_RETRY on both sides
									// Owner
									BUG_ON(ramDiskSimulator_is_bad_sector(&server[1]->ramDisk, read_fail_offset_abs[1]) != EPERM_READ_FAIL_NO_RETRY);       // Verify bad sector was not cleared (since we do not write to owner)
									ramDiskSimulator_un_bad_sector(       &server[1]->ramDisk, read_fail_offset_abs[1]);
									// Secondary - is now stale as well since recovery doesn't complete
									BUG_ON(ramDiskSimulator_is_bad_sector(&server[0]->ramDisk, read_fail_offset_abs[0]) != EPERM_READ_FAIL_NO_RETRY);       // Verify bad sector was not cleared
									ramDiskSimulator_un_bad_sector(       &server[0]->ramDisk, read_fail_offset_abs[0]);
									ramDiskSimulator_lockUnSta(           &server[0]->ramDisk, __to4K(physStart_abs[0]));		// Since IO failed in dual lock, Stale locks are now present on both sides
									ramDiskSimulator_lockUnSta(           &server[1]->ramDisk, __to4K(physStart_abs[1]));		// Since IO failed in dual lock, Stale locks are now present on both sides
									BUG_ON(rv != -ENOEXEC);
								} else {
									BUG_ON(rv);
									BUG_ON(atomic_sub_return((sync_runs_on_write ? 1 : 0), which) != 0);
								}
							}
							tomaSimulator_waitProtoEnd(NULL);
							tomaSimulator_verifyIOFailure();
							rv = 0;
						}
					} // for (flr = false; flr <= true; flr++) {
					// ---------------------------- Test Sync Stale lock when blkset_sync_safty is unsafe (Rw,RW), EXC-1619 verify that all IO types timeout (cannot complete) or succeed on dual lock
					if (1) {
						const unsigned long prev_max_jiffies = client->devs[volInd]->max_retry_jiffies;
						u64 n_timed_out;
						flr = true;
						tTopoOfPraid_force_lock_on_read(r1, flr);
						tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
						for (u32 op__ = NVMEIB_BLOCK_IO_OP_READ; op__ <= NVMEIB_BLOCK_IO_OP_DISCARD; op__++) {
							enum nvmeib_block_io_op op = (enum nvmeib_block_io_op)op__;
							dp_io_stats_clear_counter(&client->devs[volInd]->dp.io_stats, DP_IO_STATS_TIMED_OUT);
							ramDiskSimulator_lockStale(&server[0]->ramDisk, __to4K(physStart_abs[0]));
							rv = osSimulator_gen_generic_op(op, &client->OS, volInd, virtStart, lenBlocks   , mem);	 BUG_ON(rv);    // IO is stuck, cannot do sync.
							rv = osSimulator_gen_generic_op(op, &client->OS, volInd, virtStart, LOCKSET_SLICES, mem);	 BUG_ON(rv);    // Even Write/TRIM full lockset blocks must not sync either (implicit sync)
							if (!flr){
								clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", client->devs[volInd]->name, 0);
							}
							clientSimulator_wait_for_all_bio_ops(client);
							if (!flr) {
								clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", client->devs[volInd]->name, (int)(prev_max_jiffies / HZ));
							}
							// Verify that the stale lock is not/still there. Changed between v1.1.0 and v1.1.1
							n_timed_out = dp_io_stats_get_counter(&client->devs[volInd]->dp.io_stats, DP_IO_STATS_TIMED_OUT);
							if (!flr) { BUG_ON(n_timed_out == 0); ramDiskSimulator_lockUnSta(&server[0]->ramDisk, __to4K(physStart_abs[0]));}
							else      { BUG_ON(n_timed_out != 0);
										if (is_lockset_sync || (op == NVMEIB_BLOCK_IO_OP_READ/*READ IO with lock implies LOCKSET sync*/))
											 BUG_ON( ramDiskSimulator_lockIsSta(&server[0]->ramDisk, __to4K(physStart_abs[0]))); /* Was already unlocked by sync op */
										else BUG_ON(!ramDiskSimulator_lockIsSta(&server[0]->ramDisk, __to4K(physStart_abs[0])));
							}
						}
						BUG_ON(atomic_sub_return(1 + (is_lockset_sync ? 2 : 0), which) != 0);		// Only read op issues a stale lock sync (always), write/trim do it only when sync must run. See must_do_full_blkset_sync()
						tTopoOfPraid_force_lock_on_read(r1, false);							// Return sync_safety to OK
						tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);
					}
					tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW   , SW_TOPO__WAIT_ACK);
				}	// for (is_lockset_sync=false;is_lockset_sync<=true;is_lockset_sync++)
				if (tTopoOfVolume_isMirrored(&sys->tcf.vols[volInd]) && (s<n_mirror)) {		// Test only on the firsts raid1 to be faster
					unitest_SyncStaleLockVerifyData(B, volInd, s, curSeg[0], curSeg[1]);     // test the sync of each block within a LOCKSET
					atomic_set(which, 0);											// Too much syncs to verify them
				}
			}	// for (s = 0; s < (n_mirror * stripe_width); s++)
		}	// for (c=0; c<cfv->nChunks; c++
	} // For volume
	clientSimulator_wait_for_all_sync_ops(client);					// Let sync operations terminate (unneded sync's which fail because stale was already cleaned).
	NVMeshSystem_wipe_all_dirty_bits(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET; // all UT code assumes a sync fixes the whole LOCKSET & clears the stale lock.
	sim_kfree(mem);

	NVMeshSystem_all_clients_mirror_edic(sys, prev_mirror_edic); // Return edic check
	NVMeshSystem_wipe_all_md_of_disks(sys); // Cleanup
	if (ut_conf__platform_io_sync_get() && (client->devs[0]->dp.sync_rsrcs.stats.n_resources_reused == 0))	// Syncs should reuse resources.
		pr_emerg("Sync did not reuse resources, strange. Investigate..\n");
	_NI_dmesg(trace_1_bunitest_unitest_SyncStaleLocks, "*************** end");
	return rv;
}

/* fire an IO & verify it properly handles the read error, possibly syncing all the copies of r1 */
static int unitest_PermanentReadError_CheckIo(struct NVMeshSystem     *sys,
											  struct clientSimulator  *client,
											  struct serverSimulator  *server, // server on which the read should get a bad sector
											  int                      volInd,
											  const u64                small_offset,
											  const u64                phys_start,
											  u8                      *mem,
											  const int                len_blocks,
					bool is_read_ok, bool sync_launched, int is_bad_sector_fixed /* 0 - NO, 1 = Yes, -1 = No but squash errors */)
{
	const bool isMirrored = tTopoOfVolume_isMirrored(&sys->tcf.vols[volInd]);
	const int  memSize = len_blocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	struct nvmeibc_nowhole_stats *nowhole_stats = &nvmeibc_flow_counters_ref()->nowh;
	int rv, io_rv, i;
	for (i = 0; i < NUMBER_OF_ERROR_CODES; i++) {
		u64 magic_pattern = __unitest_fill_blocks_unique_pattern(mem, len_blocks);  // Set a pattern.
		tomaSimulator_waitProtoEnd(NULL);											// Wait for switch_topos which entered the client into degraded mode to terminate
		rv = osSimulator_writeArr(&client->OS, volInd, small_offset, len_blocks, mem);	REPORT_ERROR(rv);	// Write same data to all segments.
		clientSimulator_wait_for_all_bio_ops(client);
		if (isMirrored && !sync_launched && !is_read_ok) {					// R1: Read has to notice the problem (fail) and sync was not lunched. If sync launched it will fix the problem or destroy slice but IO will not complain
			tomaSimulator_expectIOFailure(&server->simToma, EPERM_READ_FAIL_NO_RETRY, U32_MAX, 0);
		} else if (!isMirrored && (tested_error_codes[i] & NVME_SC_DNR)) 	// Jbod notifies TOMA on DO_NOT_RETRY
			tomaSimulator_expectIOFailure(&server->simToma, tested_error_codes[i], U32_MAX, 0);

		ramDiskSimulator_do_bad_sector(&server->ramDisk, phys_start, tested_error_codes[i]);
		memset(mem, 0, memSize);
		rv = osSimulator_readArr(&client->OS, volInd, small_offset, len_blocks, mem);	REPORT_ERROR(rv);
		NVMeshSystem__detectStuckIOs(sys);						// IO should terminate fast because sync operation succeeded or failed
		if (is_read_ok) {										// Read IO should have fixed the bad sector
			__unitest_verify_blocks_pattern(mem, len_blocks, magic_pattern, false);   // Should have recovered and read successfully
		} else {
			BUG_ON(((u64*)&mem[len_blocks*NVMEIBC_SECTOR_SIZE])[-1] == magic_pattern); // no mirror -> no recovery -> Read IO doesnt write the buffer. Test last 8 bytes of the read memory
		}
		io_rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
		BUG_ON(is_read_ok != (io_rv == 0)); // read succeded if and only if kernel got rv 0
		if (is_bad_sector_fixed > 0) {
			ramDiskSimulator_verify_no_bad_sectors(&server->ramDisk);
		} else {
			const short expected_error = ((is_bad_sector_fixed == 0) ? tested_error_codes[i] : EPERM_READ_FAIL_NO_RETRY);
			const short actual_error = ramDiskSimulator_is_bad_sector(&server->ramDisk, phys_start);
			BUG_ON(actual_error != expected_error);
			ramDiskSimulator_un_bad_sector(&server->ramDisk, phys_start); // Could not recover, read failed
		}
		tomaSimulator_waitProtoEnd(NULL);
		tomaSimulator_verifyIOFailure();
	}
	if (1) {						// Verify exact number of syncs
		const int num_expected = (sync_launched ? NUMBER_OF_ERROR_CODES : 0);
		atomic_t *which = is_read_ok ? &nowhole_stats->n_bdsec_fix : &nowhole_stats->n_destoyed;
		BUG_ON(atomic_read(which) != num_expected);
		atomic_set(which, 0);
	}
	return rv;
}

static void __verify_volume_autosuspends_on_too_much_nvme_errors(struct NVMeshSystem *sys, int volInd) {
	struct serverSimulator *dead_S = &sys->servers[sys->mdb.vols[volInd].segs[0].node_id];
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int prev_warn = nvmeibcb_dp_io_fail_mgr_set_limit(NULL, 0), io_rv;
	ramDiskSimulator_break(&dead_S->ramDisk, EPERM_WRITE_FAIL);
	tomaSimulator_expectIOFailure(&dead_S->simToma, EPERM_WRITE_FAIL, U32_MAX, 0);
	BUG_ON(osSimulator_writeArrWait(&client->OS, volInd, 0, 1, page_address(ZERO_PAGE(0))));
	io_rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
	BUG_ON((short)io_rv == (short)EPERM_WRITE_FAIL);				// Error should not be this but rather retry time out
	tomaSimulator_waitProtoEnd(NULL);
	tomaSimulator_verifyIOFailure();
	clientSimulator_wait_for_single_topo_no_io(client);			// wait for topology to be suspended
	tomaSimulator_waitProtoEnd(NULL);							// wait for recovery thread to start & terminate as it will write stale-special that we need to clean before the next IO.
	ramDiskSimulator_fix(&dead_S->ramDisk);
	NVMeshSystem_wipe_all_stale_locks(sys);
	nvmeibc_block_revive(client->devs[volInd]);
	NVMeshSystem_serialize(sys);			// Daniel: Not sure this is enough. Have to wait for IO enabled on all volumes
	clientSimulator_wait_for_io_enabled_for_vol(client, volInd, false);
	nvmeibcb_dp_io_fail_mgr_set_limit(NULL, prev_warn);
}

struct unitest_nvme_drive_error {
	int  drive_code;					// Drive returns this error code
	int  expected_kernel_rv[3];			// The error we expect block device to report to kernel for read/write/trim in that order
	int  notify_toma[3];				// The error code with whihc client complains to Toma (if any)
};
/* When IO encounters do not retry failure it should never succeed and must update Toma about the IO failure to the segment */
TEST_FUNC int unitest_NVMeDriveFailure(struct NVMeshSystem *sys){
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int volInd, io_rv, ei, rv = 0, loops = 0, lenBlocks = 2;		// 2 blocks to be able to generate 2 commands
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;			// Total array in bytes
	u8  *mem = sim_kmalloc(memSize, GFP_KERNEL);			// Special number to fill the array with
	#define N_TESTED_ERRORS  (6)
	#define TIME_OUT 		(-EIO)		// When error is not critical, IO is retried and eventually failed due to time out (to prevent infinite loop)
	const int eee[N_TESTED_ERRORS] = {-1, -EIO, NVME_SC_DNR, 0xDEAE, NVME_SC_ABORT_QUEUE, -ENOMEM};
	struct unitest_nvme_drive_error errs[N_TESTED_ERRORS] = {
		{eee[0], {TIME_OUT, eee[0]  , eee[0]  },  {0     , eee[0], eee[0]}},
		{eee[1], {TIME_OUT, eee[1]  , eee[1]  },  {0     , 0     , 0     }},
		{eee[2], {eee[2]  , eee[2]  , eee[2]  },  {eee[2], eee[2], eee[2]}},
		{eee[3], {TIME_OUT, eee[3]  , eee[3]  },  {0     , eee[3], eee[3]}},
		{eee[4], {TIME_OUT, TIME_OUT, TIME_OUT},  {0     , eee[4], eee[4]}},
		{eee[5], {TIME_OUT, TIME_OUT, TIME_OUT},  {0     , 0     , 0     }}};
	__unitest_fill_blocks_unique_pattern(mem, lenBlocks);			// Set a pattern.
	// ------------------------------------ regardless of the raid type all IOs to the broken drive fail with NVME_SC_DNR, use the first seg of each volume
	for (volInd=0; volInd<client->nBdevs; volInd++){
		struct nvmeibc_block_device *dev = client->devs[volInd];
		const unsigned long prev_rtj = dev->max_retry_jiffies;
		const struct disk_range *dead_seg = &sys->mdb.vols[volInd].segs[0];
		struct serverSimulator *dead_S = &sys->servers[dead_seg->node_id];
		clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", dev->name, 0);// Errors which induce retry will fail due to timeout to avoid infinite loop.
		for (ei = 0; ei < N_TESTED_ERRORS; ei++) {
			ramDiskSimulator_break(&dead_S->ramDisk, (short)errs[ei].drive_code);
			for (u32 op__ = NVMEIB_BLOCK_IO_OP_READ; op__ <= NVMEIB_BLOCK_IO_OP_DISCARD; op__++) {
				const enum nvmeib_block_io_op op = (enum nvmeib_block_io_op)op__;
				tomaSimulator_expectIOFailure(&dead_S->simToma, errs[ei].notify_toma[op - 1], (errs[ei].notify_toma[op - 1]) ? U32_MAX : 0, 0);
				rv = osSimulator_gen_generic_op(op, &client->OS, volInd, 0, lenBlocks, mem);	BUG_ON(rv);
				NVMeshSystem__detectStuckIOs(sys);					// Wait for all IO's to drain, timeouts to kick in
				io_rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
				BUG_ON((short)io_rv != (short)errs[ei].expected_kernel_rv[op-1]);
				tomaSimulator_waitProtoEnd(NULL);
				tomaSimulator_verifyIOFailure();
				loops++;
				clientSimulator_wait_for_single_topo_no_io(client);			// wait for topology to be IO'able again (when possible) before we go for next iteration
				tomaSimulator_waitProtoEnd(NULL);							// wait for recovery thread to start & terminate as it will write stale-special that we need to clean before the next IO.
				NVMeshSystem_wipe_all_stale_locks(sys);
			}
			ramDiskSimulator_fix(&dead_S->ramDisk);
		}
		__verify_volume_autosuspends_on_too_much_nvme_errors(sys, volInd);
		clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", dev->name, (int)(prev_rtj/HZ));
	}
	BUG_ON(loops != (client->nBdevs)*N_TESTED_ERRORS*3 /* 3 = R/W/T ios*/);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	sim_kfree(mem);
	_NI_dmesg(trace_bunitest_unitest_NVMeDriveFailure, "*************** end");
	return rv;
}

/* When IO encounters permanent read failure it should overcome it by reading the data from second raid1 leg and writing it again to both legs */
TEST_FUNC int unitest_PermanentReadError(struct NVMeshSystem *sys){
	const enum NVMEIBTC_DS_MODE ssa[2] = {NVMEIBTC_DS_MODE_W         , NVMEIBTC_DS_MODE_RW};
	const enum NVMEIBTC_DS_MODE ssd[2] = {NVMEIBTC_DS_MODE_W_NO_DIRTY, NVMEIBTC_DS_MODE_RW};
	// topology access modes to test
	struct clientSimulator *client = &sys->clients[0];				// Test via the first client
	int i, volInd, rv = 0, lenBlocks = 2;							// 2 blocks to be able to generate 2 commands (when needed)
	int ind_deg_seg;
	u64 phys_start;
	const int memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;			// Total array in bytes
	u8  *mem = sim_kmalloc(memSize, GFP_KERNEL);	// Special number to fill the array with
	struct nvmeibc_nowhole_stats *nowhole_stats = &nvmeibc_flow_counters_ref()->nowh;
	nvmeibc_raid1_destroy_force_physical_bad_sector_in_sync = true;
	nvmeibc_nowhole_stats_reset();
	// ------------------------------------ On mirrored volumes permanent failure should succeed
	for (volInd=0; volInd<client->nBdevs; volInd++) {
		const bool isMirrored = tTopoOfVolume_isMirrored(&sys->tcf.vols[volInd]);
		const bool isStriped  = tTopoOfVolume_isStriped(&sys->tcf.vols[volInd]);
		const int max_deg_seg = isMirrored ? 2 : 1;
		for (ind_deg_seg = 0; ind_deg_seg < max_deg_seg; ind_deg_seg++) {
			// test in multiple topology modes, always on first raid of volume
			struct tTopoOfPraid		*r1 = &sys->tcf.vols[volInd].chunks[0].raids[0];
			struct disk_range *degraded_seg = &sys->mdb.vols[volInd].segs[ind_deg_seg];
			struct disk_range *good_seg     = &sys->mdb.vols[volInd].segs[ind_deg_seg^1];
			struct serverSimulator *degraded_server = &sys->servers[degraded_seg->node_id];
			struct serverSimulator *good_server     = &sys->servers[    good_seg->node_id];
			const u64 small_offset = 1, DBC = LOCKSET_SLICES -1 - small_offset;	// DBC - special offset to generate 2 read commands (2 locksets), where the second will fail
			const bool can_fix_bad_sector = (isMirrored && (ind_deg_seg == 0));

			// transition though 4 state combinations for mirrored or just simple RW for JBOD
			nvmeibc_notify_toma_on_slice_by_slice_destruction_in_sync = false;		// We will test that IO notifies toma. Testing sync notification is redundant
			if (isMirrored) {
				serverSimulator_disconnect(degraded_server);
				tomaSimulator_unreg_raid1(r1uuid(r1), ind_deg_seg);														 // {DEAD , RW} -> Lock only live
				// Read must go to second, sync cannot read from first
				phys_start = __from4K(good_seg->dlba_start) + small_offset;
				unitest_PermanentReadError_CheckIo(sys, client, good_server, volInd, small_offset, phys_start, mem, lenBlocks, false, false, false);
				serverSimulator_re_connect(degraded_server);
				NVMeshSystem_serialize(sys);
				ramDiskSimulator_wipe_dirty_bits(&good_server->ramDisk, 0x0); 	// Cleanup dirty bits created by the above degraded mode
				tomaSimulator_switchTopo( r1uuid(r1), ssa[ind_deg_seg]	, ssa[ind_deg_seg^1]	, SW_TOPO__WAIT_ACK);		     // {W	, RW} - Lock live

				// ------ {RW+BadSector ,W }
				// On long segments test the case when IO has 2 read commands (2 blocks one to each lock). The second one is a bad sector. Volume must not be striped or else the read will spread to other segments
				if (!isStriped) {
					// This case should destroy the slice: RW (bad-secor), W (with dbits on it)
					ramDiskSimulator_wipe_dirty_bits(&good_server->ramDisk, nvmeib_dbits_entry_single_unk().all_bits);
					unitest_PermanentReadError_CheckIo(sys, client, good_server, volInd, small_offset + DBC, phys_start + DBC + 1, mem, lenBlocks, false, 1, -1);
					// This case should successfully remove bad sector: RW (bad-secor), W (no dbits on it)
					ramDiskSimulator_wipe_dirty_bits(&good_server->ramDisk, 0x0);
					unitest_PermanentReadError_CheckIo(sys, client, good_server, volInd, small_offset + DBC, phys_start + DBC + 1, mem, lenBlocks, true, 1, 1);
				}

				phys_start = __from4K(good_seg->dlba_start) + small_offset;
				unitest_PermanentReadError_CheckIo(sys, client, good_server, volInd, small_offset, phys_start, mem, lenBlocks, true, 1, 1);

				tTopoOfPraid_force_lock_on_read(r1, true);
				tomaSimulator_switchTopo( r1uuid(r1), ssd[ind_deg_seg]	, ssd[ind_deg_seg^1], SW_TOPO__WAIT_ACK);  // {W	, RW} - lock both, unsafe
				unitest_PermanentReadError_CheckIo(sys, client, good_server, volInd, small_offset, phys_start, mem, lenBlocks, true, 1, 1);

				//tomaSimulator_switchTopo( r1uuid(r1), NVMEIBTC_DS_MODE_RW	   	, NVMEIBTC_DS_MODE_RW		, SW_TOPO__WAIT_ACK);			// {RW	, RW} - normal, unsafe
				//unitest_PermanentReadError_CheckIo(sys, client, good_server, volInd, small_offset, phys_start, mem, lenBlocks, true, true);

				tTopoOfPraid_force_lock_on_read(r1, false);
				tomaSimulator_switchTopo( r1uuid(r1), NVMEIBTC_DS_MODE_RW	   	, NVMEIBTC_DS_MODE_RW		, SW_TOPO__WAIT_ACK);			// {RW	, RW} - normal
			}
			nvmeibc_notify_toma_on_slice_by_slice_destruction_in_sync = true;

			phys_start = __from4K(degraded_seg->dlba_start) + small_offset;
			// bad sector on owner => Read fails, sync from second -> first, retry read succeed
			// bad sector on dual  => not identified.
			unitest_PermanentReadError_CheckIo(sys, client, degraded_server, volInd, small_offset, phys_start, mem, lenBlocks, isMirrored, can_fix_bad_sector, can_fix_bad_sector);

			// Same as previous test but with forced locks on read
			tTopoOfPraid_force_lock_on_read(r1, true);
			tomaSimulator_switchTopo( r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);			// {RW	, RW} - normal, unsafe
			unitest_PermanentReadError_CheckIo(sys, client, degraded_server, volInd, small_offset, phys_start, mem, lenBlocks, isMirrored, can_fix_bad_sector, can_fix_bad_sector);
			tTopoOfPraid_force_lock_on_read(r1, false);
			tomaSimulator_switchTopo( r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, SW_TOPO__WAIT_ACK);			// {RW	, RW} - normal

			// On long segments test the case when IO has 2 read commands (2 blocks one to each lock). The second one is a bad sector. Volume must not be striped or else the read will spread to other segments
			if (!isStriped)
				unitest_PermanentReadError_CheckIo(sys, client, degraded_server, volInd, small_offset + DBC, phys_start + DBC + 1, mem, lenBlocks, isMirrored, can_fix_bad_sector, can_fix_bad_sector);

			// Test failure of all operations, when all (both) raid segments have permanent read failure and topo is normal RW,RW
			if (isMirrored) {
				atomic_t *destroyed_slices_counter = &nowhole_stats->n_destoyed;
				int test_with_stale_locks;
				enum nvmeib_block_io_op op;
				for (u32 op__ = NVMEIB_BLOCK_IO_OP_READ; op__ <= NVMEIB_BLOCK_IO_OP_DISCARD; op__++) {
					op = (enum nvmeib_block_io_op)op__;
					for (test_with_stale_locks = 0; test_with_stale_locks <= 1; test_with_stale_locks++) {
						for (i=0;i<NUMBER_OF_ERROR_CODES;i++) {
							if (test_with_stale_locks) {
								ramDiskSimulator_lockStale(&degraded_server->ramDisk, degraded_seg->dlba_start);
								ramDiskSimulator_lockStale(&    good_server->ramDisk,     good_seg->dlba_start);
								if (op != NVMEIB_BLOCK_IO_OP_READ) {//Same as unitest_SyncStaleLocks lock in Dual Mode (RW,W) Dual lock, but the recovering (owner) side has permanent read fail
									tomaSimulator_expectIOFailure(&degraded_server->simToma, EPERM_READ_FAIL_NO_RETRY, 1, 0); // First slice in the lockset
									tomaSimulator_expectIOFailure(    &good_server->simToma, EPERM_READ_FAIL_NO_RETRY, 1, 0); // First slice in the lockset
								}
							}
							if (op == NVMEIB_BLOCK_IO_OP_READ) { // Read fails on owner, takes lock (stale or other), fails to read from mirror sends toma update on both
								tomaSimulator_expectIOFailure(    &good_server->simToma, EPERM_READ_FAIL_NO_RETRY, 1, 0); // First slice in the lockset
								tomaSimulator_expectIOFailure(&degraded_server->simToma, EPERM_READ_FAIL_NO_RETRY, 1, 0); // First slice in the lockset
							}
							ramDiskSimulator_do_bad_sector(&degraded_server->ramDisk, __from4K(degraded_seg->dlba_start), tested_error_codes[i]);
							ramDiskSimulator_do_bad_sector(&    good_server->ramDisk, __from4K(    good_seg->dlba_start), tested_error_codes[i]);
							rv = osSimulator_gen_generic_op(op, &client->OS, volInd, 0, 1, mem);	BUG_ON(rv);
							NVMeshSystem__detectStuckIOs(sys);						// IO should terminate fast because sync operation cleaned the stale lock
							rv = osSimulator_rv_of_last_io_get(&client->OS, volInd);
							if (op != NVMEIB_BLOCK_IO_OP_READ){
								if (test_with_stale_locks)
									// Stale locks will not allow write/trim/discard to complete since we cannot complete a NVMEIB_BLOCK_IO_OP_RECOVER_STALE command
									// Same as unitest_SyncStaleLocks lock in Dual Mode (RW,W) Dual lock, but the recovering (owner) side has permanent read fail
									BUG_ON(rv != -ENOEXEC);
								else
									BUG_ON(rv); // Both write and TRIM/DISCARD fix the error
							} else { // read must fail and sync as well (read from secondary)
								BUG_ON(rv != EPERM_READ_FAIL_NO_RETRY);
							}
							tomaSimulator_waitProtoEnd(NULL);
							tomaSimulator_verifyIOFailure();
							if (rv) {
								BUG_ON(atomic_dec_return(destroyed_slices_counter) != 0);	// Was 1 -> set to zero
								ramDiskSimulator_un_bad_sector(&degraded_server->ramDisk, __from4K(degraded_seg->dlba_start));
								ramDiskSimulator_un_bad_sector(&    good_server->ramDisk, __from4K(    good_seg->dlba_start));
							}
							if (test_with_stale_locks) {
								ramDiskSimulator_lockUnSta(&degraded_server->ramDisk, degraded_seg->dlba_start);
								ramDiskSimulator_lockUnSta(&    good_server->ramDisk,     good_seg->dlba_start);
							}
							rv = 0;
						}
					}
				}
			}
			clientSimulator_wait_for_all_bio_ops(client); NVMeshSystem_serialize(sys); // Todo: Wait until all topos are freed. Last IO operation was resubmitted and terminated but its topo reference was passed to the locks and did not have time to free
			NVMeshSystem_wipe_all_dirty_bits(sys);
			BUG_ON(!NVMeshSystem_is_stable(sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.
		}
	}
	nvmeibc_raid1_destroy_force_physical_bad_sector_in_sync = false;
	sim_kfree(mem);
	_NI_dmesg(t01upredt, "*************** end");
	return rv;
}

/* Test pause and continue on disk without IO */
TEST_FUNC int unitest_PauseContDisk_noIO(struct NVMeshSystem *sys){
	int i, rv = -1;
	for (i=0; i<2; i++) {
		NVMeshSystem__invoke_pause_cont_on_disk(sys, 0);		// Shut down disk with RAID1+0 and JBOD.
		rv = unitest_GoodPathIO(sys);			REPORT_ERROR(rv);
		NVMeshSystem__invoke_pause_cont_on_disk(sys, 3);		// Shut down disk 3, with Raid1+0, Raid-1, Raid-0
		rv = unitest_GoodPathIO(sys);			REPORT_ERROR(rv);
	}
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_wipe_all_dirty_bits(sys);
	_NI_dmesg(trace_bunitest_unitest_PauseContDisk_noIO, "*************** end");
	return rv;
}

void __my_rand_perm(u32 arr[], const int length){
	int i;
	for (i=0; i<length; i++) 	arr[i] = i;				// Create unit permutation
	range_shuffle(arr, arr + length);
}

void __my_rand_perm_multi_slice(u32 arg[], const int length, const int msn){
	int i;
	__my_rand_perm(arg, length);
	for (i = 0; i < length; i++)
		arg[i] *= msn;
}

TEST_FUNC int unitest_Registrable(struct NVMeshSystem *sys){
	int s, rv = 0;
	u32 shuffle[NVMESH_N_PHYS_DISKS];
	struct clientSimulator *client = &sys->clients[0];

	send_command_to_vol(sys, -1, 0, volCmds_Detach); 		// Detach vol 0
	// Put all tomas in unresponsive state
	for (s=0; s<sys->nServers; s++) {
		serverSimulator_disconnect(&sys->servers[s]);
		sys->servers[s].simToma.state = tomaState_not_ready;
	}

	// Here IO enabled comes later - wait for attach,
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string_no_io(&client->vols[0]), 0);
	send_command_to_vol(sys, -1, 0, volCmds_New); 		// Attach vol 0

	// reset expector for IO enabled
	reset_cli_status_verification(client);
	set_cli_status_verification_expector(client, attach_string(&client->vols[0]), 0);
	NVMeshSystem_serialize(sys);

	__my_rand_perm(shuffle,sys->nServers);						// Tomas comming online in random order.
	// Screw up with the client a bit :-) Send registrables but respond with Toma not ready.
	for (s=0; s<sys->nServers; s++){
		tomaSimulator_send_registrables(&sys->servers[shuffle[s]].simToma);
	}

	for (s=0; s<sys->nServers; s++){
		serverSimulator_re_connect(&sys->servers[shuffle[s]]);
		NVMeshSystem_serialize(sys);							// Tomas comming online one at a time
	}

	// Wait for IO enabled before sending IOs
	wait_for_cli_status_verification(client);
	BUG_ON(!NVMeshSystem_is_stable(sys));

	if (1) { // -----------------Scenario 2. Toma replies with outdated registrable, client retries to register -----------
		struct nvmeibc_topologies *nt0 = &sys->clients[0].devs[0]->topologies;
		struct nvmeibc_topologies *nt1 = &sys->clients[0].devs[1]->topologies;
		struct tTopoOfNVMesh* cf = &sys->tcf;
		NVMeshSystem__invoke_pause_on_disk(sys, 0);
		NVMeshSystem__invoke_cont_on_disk( sys, 0, true);					// Leave Toma 0 in unresponsive state
		BUG_ON(nvmeibc_topo_is_io_ok(nt0) || nvmeibc_topo_is_io_ok(nt1));
		cf->vols[0].chunks->raids[0].header.praid_version -= 10;			// Decrease raid version of first raid in vol0 to force the registrable to be outdated
		tomaSimulator_send_registrables(&sys->servers[0].simToma);			// Send registrables (vol0 - wrong raid version, vol1 - correct version). But toma is still not ready for both
		NVMeshSystem_serialize(sys);
		BUG_ON(nvmeibc_topo_is_io_ok(nt0) || nvmeibc_topo_is_io_ok(nt1));
		cf->vols[0].chunks->raids[0].header.praid_version += 10;			// Return raid version to its normal state
		serverSimulator_re_connect(&sys->servers[0]);						// Vol0 and Vol1 becomes ready
		NVMeshSystem_serialize(sys);
		BUG_ON(!NVMeshSystem_is_stable(sys));
	}
	BUG_ON(rv);
	_NI_dmesg(trace_bunitest_unitest_Registrable, "*************** end");
	return rv;
}

/*****************************************************************************/
TEST_FUNC int __resize_vol0(struct NVMeshSystem *sys, char* action, bool all_chunks_at_once){
	int rv = 0, i, j, n_new_chunks=0, volInd=0;
	struct clientSimulator  *client		= &sys->clients[0];				// Current client
	struct volumeDescriptor *vol 		= &sys->mdb.vols[volInd];  			// Move segment of first volume
	struct tTopoOfVolume	*cfv		= &sys->tcf.vols[volInd];		// Toma Configuration of the current volume
	struct nvmeibc_block_device *dev	= client->devs[volInd];
	const int striping	= cfv->chunks[0].stripeWidth;
	const int mirror	= cfv->chunks[0].raids[0].header.n_segments;
	const int n_segs_in_chunk = mirror*striping;
	const u64 new_segs_size = RAMDISK_DATA_LOCK_SIZE;										// 8 locksets, 1[mb] each segment, occupying entire disk
	struct nvmeibc_block_disk *disks 	 = sys->mdb.discs[volInd];		// Disks of the volume
	int max_n_segs, max_n_chunks, orig_ver;

	struct ramDiskSimulator *ramDisk = NULL;
	// Static variables to store during expansion so we can easily reduce the size of the CV back
	static struct volumeDescriptor backup_vol;                         // Save the volume so we will be able to restore it, easily.
	static int n_orig_chunks;
	static u64 orig_size;

	if (action[0] == 'e') goto _expand;
	if (action[0] == 's') goto _shrink;
	BUG_ON(true);

_expand:	// ------------------------------------ Expand volume by a single chunk in a loop.
	backup_vol		= *vol;                         // Save the volume so we will be able to restore it, easily.
	n_orig_chunks 	= cfv->nChunks;
	orig_size 		= client->devs[volInd]->size;
	orig_ver		= vol->info.version;

	vol->segs = (struct disk_range*)sim_kmalloc(sizeof(*vol->segs)*NVMESH_N_PHYS_DISKS, GFP_KERNEL);
	BUG_ON(NVMESH_N_PHYS_DISKS > NVMESH_MAX_SEG_PER_VOLUME);
	for (i=0; i<backup_vol.nSegments; i++)
		disk_range_copy(&vol->segs[i], &backup_vol.segs[i]);

	for (n_new_chunks=0; i<NVMESH_N_PHYS_DISKS; n_new_chunks++) {		// Append one chunk (of n_segs_in_chunk segsments) at a time
		// Update config-fs input
		for (j=0; j<n_segs_in_chunk; j++, i++) {
			disk_range_init(&sys->mdb, &vol->segs[i], __to4K(orig_size), i, j, n_orig_chunks + n_new_chunks, volInd, mirror, striping, 1, new_segs_size, 1, false);
			mongo_db_simu_reconf_set_new(disks, -1, i, &vol->segs[i], vol->segs[i].dlba_start);
		}
		vol->nChunks++;
		vol->nSegments += n_segs_in_chunk;
		orig_size += (new_segs_size*LOCKSET_SLICES)*striping;
		mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, 0, NULL, 0, NULL);

		// Convert sys-admin definition to configuration
		tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
		mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
		if (!all_chunks_at_once) {
			send_command_to_vol_attach_or_update(sys, -1, volInd);
			BUG_ON(dev->size != orig_size);
		}
	}
	BUG_ON(vol->nSegments != NVMESH_N_PHYS_DISKS);
	if (all_chunks_at_once) {
		cfv->version = vol->info.version = orig_ver+1;	// As if version jumped only by 1 as in multi-chunk reconfiguration
		send_command_to_vol_attach_or_update(sys, -1, volInd);
		BUG_ON(dev->size != orig_size);
	}
	goto _out;

_shrink:	// ------------------------------------ Shrink volume back to its original size.
	max_n_segs	= vol->nSegments;
	max_n_chunks= cfv->nChunks;
	orig_ver		= vol->info.version;
	for (n_new_chunks=max_n_chunks-n_orig_chunks; n_new_chunks>0; n_new_chunks--) {			// In a loop remove last chunk
		// Update config-fs input
		u32 deprecDisk_ids[NVMESH_N_PHYS_DISKS] = {0}, nDeprec;
		struct disk_range *deprecSegs[NVMESH_N_PHYS_DISKS]= {NULL};
		for (j=0; j<n_segs_in_chunk; j++, vol->nSegments--) {
			mongo_db_simu_reconf_set_new(disks, vol->nSegments-1, -1, NULL, -1);

			ramDisk = &sys->servers[vol->nSegments-1].ramDisk;
			sys->mdb.srvrs[vol->nSegments-1].disk_alloc_end = ramDisk->committed_addr.block;	// Disk is completely empty and not used
		}																// the loop did: vol->nSegments-= n_segs_in_chunk;
		nDeprec = NVMESH_N_PHYS_DISKS - vol->nSegments;
		for (j=vol->nSegments, i=0; j<NVMESH_N_PHYS_DISKS; j++, i++){
			deprecDisk_ids[ i] = j;				// Write the depricated disks as complementary to the existing segments (we have 1 seg on each disk)
			deprecSegs[ 	i] = &vol->segs[j];
		}
		orig_size -= (new_segs_size*LOCKSET_SLICES)*striping;
		cfv->nChunks--;
		vol->nChunks--;
		mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, nDeprec, deprecSegs, nDeprec, deprecDisk_ids);

		if (!all_chunks_at_once) {
			send_command_to_vol_attach_or_update(sys, -1, volInd);
			BUG_ON(dev->size != orig_size);
		}
	}
	if (all_chunks_at_once) {
		cfv->version = vol->info.version = orig_ver+1;	// As if version jumped only by 1 as in multi-chunk reconfiguration
		send_command_to_vol_attach_or_update(sys, -1, volInd);
		BUG_ON(dev->size != orig_size);
	}
	mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, volInd);
	BUG_ON(n_orig_chunks != cfv->nChunks);
	backup_vol.info.version = vol->info.version;							// Don't forget that version was increased.
	vol->nSegments = max_n_segs;
	volumeDescriptor_free(vol);
	*vol = backup_vol;
	cfv->nChunks = max_n_chunks;
	tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
	mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);

_out:
	return rv;
}

TEST_FUNC int unitest_hot_multichunk_resize(struct NVMeshSystem *sys) {
	int rv = 0;
	rv |= __resize_vol0(sys, "expand_to24[mb]", true);
	rv = unitest_IO(&sys->clients[0], 0, _addr4k(13,30),  _addr4k(3,5));
	rv |= __resize_vol0(sys, "shrink_back", true);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_wipe_all_dirty_bits(sys);
	return rv;
}

int __thread_async_resize_vol0(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	int round;
	for (round=0; !kthread_should_stop(); round++) {
		const bool all_chunks_at_once = (round&0x1);							// Alternate types of expansion and shrinking
		__resize_vol0(p->sys, "expand_to24[mb]", all_chunks_at_once);
		msleep(1);
		__resize_vol0(p->sys, "shrink_back", all_chunks_at_once);
	}
	p->n_cycles = round*2*3;	// expand+shrink each of 3 chunks
	return 0;
}

TEST_FUNC int unitest_async_pause_cont_during_resize(struct NVMeshSystem *sys, bool full_test){
	if (full_test){
	t_async_test_params p[4];
	const int /*write_len_blk = 65, read_len_blk = 3,*/ disk_id = -1;				// Write/Triem 65 blocks, read 3 blocks, rotate pause disks
	const int n_threads = ARRAY_SIZE(p);
	int v, rv = 0, n_total_ios = 0, n_total_pauses = 0, n_total_resizes = 0;
	for (v = 0; v < n_threads; v++)
		t_async_test_params_init(p[v], sys, disk_id, -1, 0);
	//create_async_io_thread(&p[0], NVMEIB_BLOCK_IO_OP_WRITE, write_len_blk);
	//create_async_io_thread(&p[1], NVMEIB_BLOCK_IO_OP_READ, read_len_blk);
	p[2].kthread = kthread_run(__thread_async_resize_vol0, 	&p[2], "ut:async resize vol0");	BUG_ON(p[2].kthread == NULL);
	p[3].kthread = kthread_run(__thread_async_pause_cont, 	&p[3], "ut:async pause/cont");	BUG_ON(p[3].kthread == NULL);
	msleep(1000);												// Let the threads run together
	for (v=0; v<n_threads; v++)
		if (p[v].kthread)
			kthread_stop(p[v].kthread);
	for (v=0; v<n_threads-2; v++) n_total_ios    += p[v].n_cycles;
	for (   ; v<n_threads-1; v++) n_total_resizes+= p[v].n_cycles;
	for (   ; v<n_threads; v++)   n_total_pauses += p[v].n_cycles;
	NVMeshSystem__detectStuckIOs(sys);
	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);			// Let sync operations terminate
	tomaSimulator_waitProtoEnd(NULL);									// Abandoned locks, can trigger toma messages
	NVMeshSystem_wipe_all_stale_locks(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	rv = 0;

	BUG_ON(rv);
	unitest_print("*************** Pauses %d, Resizes %d, Sent %d IO's\n", n_total_pauses, n_total_resizes, n_total_ios);
	return rv;
	} else return 0;
}

int __thread_async_degraded_mirror_vols(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct NVMeshSystem *sys = p->sys;
	const enum NVMEIBTC_DS_MODE ss[2] = {NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_W};
	int v, c, r, si, round, rv = 0;
	for (round=0; !kthread_should_stop(); ) {
		struct tTopoOfNVMesh *cf = &sys->tcf;
		for (v=0; v < cf->nVolumes; v++) {
			struct tTopoOfVolume *vol = &cf->vols[v];
			struct disk_range *cur_raid = sys->mdb.vols[v].segs;			// First seg in configuration of current raid
			if (!tTopoOfVolume_isMirrored(&cf->vols[v]))
				continue;													// Non mirrores
			for (c = 0; c < vol->nChunks; c++) {
				const struct tTopoOfRaid0Chunk *chunk = &vol->chunks[c];
				for (r=0; r<chunk->stripeWidth; r++) {
					const struct tTopoOfPraid *r1 = &chunk->raids[r];
					for (si = 0; si < r1->header.n_segments; si++) {		// 'si' of live segment
						struct switch_topo_options sw_opts = {.dont_send_msg = false, .can_fail = true, .wait_for_ack=true, .use_seg_index=true, .seg_index = si,  .dry_run=0};
						struct switch_topo_dest	dummy_st_dst = {.r1 = NULL, .disk_ind = -1, .seg_ind = -1};
						struct disk_range *deadSeg = &cur_raid[si^1], *liveSeg = &cur_raid[si];
						u32 n_attemp_sw_topos = 0;
						int recov_status;
						NVMeshSystem__invoke_pause_on_disk(sys, deadSeg->node_id);		// Pause the disk in order to stop io on that disk
						tomaSimulator_unreg_raid1(r1uuid(r1), si^1);           // Uregister the raid, now the client will be able to write on the second disk, and set dirty bits
						clientSimulator_wait_for_io_enabled_for_vol_non_idle(&sys->clients[0], v);
						//msleep(1);											// time to create dbits														// Let the client do some IO
						NVMeshSystem_serialize(sys);
						NVMeshSystem__invoke_cont_on_disk(sys, deadSeg->node_id, false);
						for (n_attemp_sw_topos = 0, rv = -1; (rv < 0)&&(n_attemp_sw_topos < 10000); n_attemp_sw_topos++) {
							rv = tomaSimulator_switchTopo(r1uuid(r1), ss[si], ss[si ^ 1], sw_opts);   // {RW,W} or {W,RW}
							if (rv < 0) { msleep(1); }	// Can happen if client unregistered due to IO failure, and switch Topo could not be sent
						}

						if (!((c == 0)&&(r==0))) {							// Once in a while, do dummy sxwitch topo on first praid. Make sure it does not
							tomaSimulator_switchTopo_dummy(v, 0, SW_TOPO__NONE, &dummy_st_dst);
						}
						do {
							BUG_ON(tomaSimulator_recoverThingStatus(r1, liveSeg, RCVR_DIRTY_REBUILD, &recov_status) < 0);
						} while (recov_status);
						if (dummy_st_dst.r1 != NULL) {
							tomaSimulator_waitSwitchTopoAck(dummy_st_dst.disk_ind, dummy_st_dst.r1->s[dummy_st_dst.seg_ind].uuid);
							memset(&dummy_st_dst, 0, sizeof(dummy_st_dst));
						}
						for (n_attemp_sw_topos = 0, rv = -1; (rv < 0)&&(n_attemp_sw_topos < 10000); n_attemp_sw_topos++) {
							rv = tomaSimulator_switchTopo(r1uuid(r1), NVMEIBTC_DS_MODE_RW, NVMEIBTC_DS_MODE_RW, sw_opts);
							if (rv < 0) { msleep(1); }	// Can happen if client unregistered due to IO failure, and switch Topo could not be sent
						}
						if (round %23 == 0){
							struct toma_recovery_args rcvr_args = {.type = NVMEIBT_RECOVERY_TYPE_STALE_LOCKS_PURGE, .cmd = NVMEIBT_CLIENT_MSG_TR_RECOVER_START, .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA};
							BUG_ON(tomaSimulator_recoverThing(r1, liveSeg, rcvr_args) < 0);
						}
						round++;
					}
					cur_raid += cur_raid->replicas;
				}
			}
		}
	}
	p->n_cycles = round;
	return 0;
}

int __thread_async_degraded_ec_vols(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct NVMeshSystem *sys = p->sys;
	int v, c, r, si, round, rv = 0;
	for (round=0; !kthread_should_stop(); ) {
		struct tTopoOfNVMesh *cf = &sys->tcf;
		for (v=0; v < cf->nVolumes; v++) {
			struct tTopoOfVolume *vol = &cf->vols[v];
			struct disk_range *cur_raid = sys->mdb.vols[v].segs;			// First seg in configuration of current raid
			for (c = 0; c < vol->nChunks; c++) {
				const struct tTopoOfRaid0Chunk *chunk = &vol->chunks[c];
				for (r=0; r<chunk->stripeWidth; r++) {
					const struct tTopoOfPraid *r1 = &chunk->raids[r];
					enum NVMEIBTC_DS_MODE modes[N_MAX_RAID_SLICE_LEN] = { [0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW };
					for (si = 0; si < r1->header.n_segments; si++) {		// 'si' of dead segment
						struct disk_range *deadSeg = &cur_raid[si];
						// Second dead segment, every 7 iterations:
						int si_2 = (round % 7) ? (si + (round % (r1->header.n_segments - 2)) + 1) % r1->header.n_segments : -1;
						struct disk_range *deadSeg2 = si_2 != -1 ? &cur_raid[si_2] : NULL;
						u32 n_attemp_sw_topos = 0;
						int recov_status;
						int recov_si;
						struct switch_topo_options sw_opts = { .dont_send_msg = false, .can_fail = true, .wait_for_ack = true, .wait_for_ack_drain = true, .use_seg_index = false, .dry_run = false };

						NVMeshSystem__invoke_pause_on_disk(sys, deadSeg->node_id);	// Pause the disk in order to stop io on that disk
						tomaSimulator_unreg_raid1(r1uuid(r1), si);	// Uregister the raid, now the client will be able to write while setting dirty bits
						clientSimulator_wait_for_io_enabled_for_vol_non_idle(&sys->clients[0], v);
						msleep(1);	// time to create dbits
						if (deadSeg2) {
							NVMeshSystem__invoke_pause_on_disk(sys, deadSeg2->node_id);
							tomaSimulator_unreg_raid1(r1uuid(r1), si_2);
							clientSimulator_wait_for_io_enabled_for_vol_non_idle(&sys->clients[0], v);
							msleep(1);	// time to create more dbits
						}
						NVMeshSystem_serialize(sys);
						NVMeshSystem__invoke_cont_on_disk(sys, deadSeg->node_id, false);
						if (deadSeg2)
							NVMeshSystem__invoke_cont_on_disk(sys, deadSeg2->node_id, false);

						modes[si] = NVMEIBTC_DS_MODE_W;
						if (deadSeg2)
							modes[si_2] = NVMEIBTC_DS_MODE_W;
						for (n_attemp_sw_topos = 0, rv = -1; (rv < 0)&&(n_attemp_sw_topos < 10000); n_attemp_sw_topos++) {
							rv = tomaSimulator_switchTopoEC(r1->header.uuid, modes, sw_opts, NULL);
							if (rv < 0) { msleep(1); }	// Can happen if client unregistered due to IO failure, and switch Topo could not be sent
						}

						// Run DB recoveries on all live segments. TODO: in parallel.
						for (recov_si = 0; recov_si < r1->header.n_segments; recov_si++) {
							if (recov_si == si || recov_si == si_2)
								continue;

							do {
								BUG_ON(tomaSimulator_recoverThingStatus(r1, &cur_raid[recov_si], RCVR_DIRTY_REBUILD, &recov_status) < 0);
							} while (recov_status);
						}

						modes[si] = NVMEIBTC_DS_MODE_W_NO_DIRTY;
						if (deadSeg2)
							modes[si_2] = NVMEIBTC_DS_MODE_W_NO_DIRTY;
						for (n_attemp_sw_topos = 0, rv = -1; (rv < 0)&&(n_attemp_sw_topos < 10000); n_attemp_sw_topos++) {
							rv = tomaSimulator_switchTopoEC(r1->header.uuid, modes, sw_opts, NULL);
							if (rv < 0) { msleep(1); }
						}

						modes[si] = NVMEIBTC_DS_MODE_RW;
						if (deadSeg2)
							modes[si_2] = NVMEIBTC_DS_MODE_RW;
						for (n_attemp_sw_topos = 0, rv = -1; (rv < 0)&&(n_attemp_sw_topos < 10000); n_attemp_sw_topos++) {
							rv = tomaSimulator_switchTopoEC(r1->header.uuid, modes, sw_opts, NULL);
							if (rv < 0) { msleep(1); }
						}

						round++;
					}
					cur_raid += cur_raid->replicas;
				}
			}
		}
	}
	p->n_cycles = round;
	return 0;
}

int __thread_async_recov_cancel_ping_msg(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct NVMeshSystem *sys = p->sys;
	struct mgmt_sgmnts_enumerator sgmnts_enum = create_mirror_volumes_sgmnts_enum(sys);
	enum NVMEIBT_CLIENT_MSG_TYPES msgs[] = {NVMEIBT_CLIENT_MSG_TR_RECOVER_PING, NVMEIBT_CLIENT_MSG_TR_RECOVER_ABORT};
	int round = 0;
	while(!kthread_should_stop()) {
		while(sgmnts_enum.move_next(&sgmnts_enum)){
			struct test_context curr = sgmnts_enum.curr;
			for (u16 rcvr_type = 0; rcvr_type < NVMEIBT_RECOVERY_TYPE_NUM_RECOVERIES; ++rcvr_type){
				struct toma_recovery_args rcvr_args = {.type=rcvr_type, .cmd=msgs[(round%7 ==3)], .on_start_wait_for_end = true, .recov_caller = UNI_RECOV_CALLER_TOMA};
				BUG_ON(tomaSimulator_recoverThing(curr.sraid.tpr, &curr.sraid.cpr[curr.sraid.vsi.segment], rcvr_args) < 0);
			}
			round++;
			usleep(10);
		}
		sgmnts_enum.reset(&sgmnts_enum);
	}
	p->n_cycles = round;
	return 0;
}

TEST_FUNC int unitest_async_degraded_mode_rebuild_during_io(struct NVMeshSystem *sys){
	t_async_test_params p[4];
	const int write_len_blk = 65, read_len_blk = 3, disk_id = -1;				// Write/Triem 65 blocks, read 3 blocks, rotate pause disks
	const int n_threads = ARRAY_SIZE(p);
	int v, rv = 0;
	struct nvmeibc_disk_hooks disk_hooks = { .args.trerr = { true, true, true, 0, 0, (rand() % INJECT_TRANSPORT_ERROR_CYCLE_SIZE), true, 0, 0}, .inject_transport_error = inject_transport_error };

	for (v = 0; v < n_threads; v++)
		t_async_test_params_init(p[v], sys, disk_id, -1, BUNITEST_ASYNC_TEST_RAND_IO_PATTERN);
	NVMeshSystem_gen_cmd_hooks_setup_all_disks(sys, &disk_hooks);
	NVMeshSystem_di_tracking_enable(sys);

	create_async_io_thread(&p[0], NVMEIB_BLOCK_IO_OP_WRITE, write_len_blk);
	create_async_io_thread(&p[1], NVMEIB_BLOCK_IO_OP_READ, read_len_blk);
	p[2].kthread = kthread_run(__thread_async_degraded_mirror_vols,	&p[2], "ut:deg_mirr" );	BUG_ON(p[2].kthread == NULL);
	p[3].kthread = kthread_run(__thread_async_recov_cancel_ping_msg,&p[3], "ut:recv_ping");	BUG_ON(p[3].kthread == NULL);
	msleep(400);												// Let the threads run together
	for (v=0; v<n_threads; v++) if (p[v].kthread) kthread_stop(p[v].kthread);
	NVMeshSystem__detectStuckIOs(sys);
	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);			// Let sync operations terminate
	NVMeshSystem_di_tracking_disable(sys);
	tomaSimulator_waitProtoEnd(NULL);									// Abandoned locks, can trigger toma messages
	NVMeshSystem_wipe_all_stale_locks(sys);
	NVMeshSystem_wipe_all_dirty_bits(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_gen_cmd_hooks_clean_all_disks(sys);

	unitest_print("*************** DegradedModes %d, Sent {W=%d/R=%d} IO's, recov_pings=%d\n", p[2].n_cycles, p[0].n_cycles, p[1].n_cycles, p[3].n_cycles);
	return rv;
}

int __thread_async_stale_R1_rebuild(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;

	int round = 0;
	struct NVMeshSystem *sys = p->sys;
	const bool slow_syncs = (p->flags & BUNITEST_ASYNC_TEST_SLOWED_DOWN_SYNCS);
	struct sim_recovery_hooks slow_hooks = sim_recovery_hooks_create( (useconds_t)10*1000, 0, 0/*do not follow launches*/, 0);
	struct mgmt_sgmnts_enumerator sgmnts_enum = create_mirror_volumes_sgmnts_enum(sys);

	if (slow_syncs)
		sim_recovery_setup_hooks(&slow_hooks);

	while(!kthread_should_stop()) {
		sgmnts_enum.reset(&sgmnts_enum);
		while(sgmnts_enum.move_next(&sgmnts_enum)){
			struct test_context curr = sgmnts_enum.curr;
			if (tomaSimulator_recoverThing(curr.sraid.tpr, &curr.sraid.cpr[curr.sraid.vsi.segment], RCVR_STALE_REBUILD)) {
				round++;
			} else {
				usleep(10);										// Client is not subscribed
			}
		}
	}

	if (slow_syncs)
		sim_recovery_clean_hooks();

	clientSimulator_wait_for_all_sync_ops(&sys->clients[0]);	// All syncs should finish
	clientSimulator_wait_for_all_recoveries_done(&sys->clients[0]);
	tomaSimulator_waitProtoEnd(NULL);							// wait for recovery done message to arrive to Toma
	p->n_cycles = round;
	return 0;
}

int __thread_async_R1_stale_injects(void* param) {						// Todo: Unify with __thread_async_stale_R1_rebuild, using param
	t_async_test_params *p = (t_async_test_params*)param;
	struct NVMeshSystem *sys = p->sys;
	int round = 0;

	struct mgmt_sgmnts_enumerator sgmnts_enum = create_mirror_volumes_sgmnts_enum(sys);

	while (!kthread_should_stop()) {
		while(sgmnts_enum.move_next(&sgmnts_enum)){
			struct test_context curr = sgmnts_enum.curr;
			const int n_locks = curr.sraid.cpr->length / LOCKSET_SLICES;
			for (int li = 0; li < n_locks; ++li){
				struct disk_range *sgmnt = &curr.sraid.cpr[curr.sraid.vsi.segment];
				struct ramDiskSimulator *rd = &sys->servers[sgmnt->node_id].ramDisk;
				u64 lock_start = COMMITTED_ADDR_AS(rd, sgmnt->dlba_start, 4KB, LOCK);
				//struct volume_segment_index vsi = curr.sraid.vsi;
				//unitest_print("%s sgmnt=(%d, %d, %d, %d) li=%d\n", __FUNCTION__, vsi.volume, vsi.chunk, vsi.raid, vsi.segment, li);
				cmpxchg(&rd->locks[lock_start + li], 0, R1_STALE_SPECIAL_LOCK_VAL);
			}
		}
		sgmnts_enum.reset(&sgmnts_enum);
		usleep(100);
		round++;
	}
	p->n_cycles = round;
	return 0;
}

int __thread_async_sleep(void* param) {
	t_async_test_params *p = (t_async_test_params*)param;
	struct NVMeshSystem *sys = p->sys;
	int round;
	for (round=0; !kthread_should_stop(); round++) {
		msleep(10);
	}
	(void)sys;
	p->n_cycles = round;
	return 0;
}

TEST_FUNC int unitest_async_suspend_revive_during_rebuild(struct NVMeshSystem *sys){
	t_async_test_params p[4];
	const int write_len_blk = 17, read_len_blk = 3;
	const int n_threads = ARRAY_SIZE(p);
	int v, num_canceled_ios;
	// ------------------------------------ Test Suspend/Revive on good topology + IO
	for (v=0; v<n_threads; v++)
		t_async_test_params_init(p[v], sys, -1, -1, BUNITEST_ASYNC_TEST_FLAG_ALLOW_IO_ERROR|BUNITEST_ASYNC_TEST_RAND_IO_PATTERN);	// TODO(Daniel): why do we get IO failures in this test
	tomaSimulator_protoBugs(true, true);	  // Must remove simulator guards because client unsibscribes so fast that toma did not have time to receive unregister message, but the message was legaly sent
	NVMeshSystem_di_tracking_enable(sys);
	create_async_io_thread(&p[0], NVMEIB_BLOCK_IO_OP_WRITE, write_len_blk);
	create_async_io_thread(&p[1], NVMEIB_BLOCK_IO_OP_READ, read_len_blk);
	BUG_ON(!(p[2].kthread = kthread_run(__thread_async_suspend_revive,  &p[2], "ut:suspend/revive")));
	BUG_ON(!(p[3].kthread = kthread_run(__thread_async_stale_R1_rebuild,&p[3], "ut:stale_rebuild" )));
	msleep(400);												// Let the threads run together
	for (v=0; v<n_threads; v++) if (p[v].kthread) kthread_stop(p[v].kthread);
	tomaSimulator_protoBugs(false, false);
	num_canceled_ios = __cancel_and_drain_resubmitted_io(sys);	// PAUSE/CONT thread has finished, it is a waste of time to resbumit good path remaining IO.
	BUG_ON(!NVMeshSystem_is_stable(sys));
	NVMeshSystem_di_tracking_disable(sys);
	unitest_print("*************** Reboots %d, SSP-Rebuilds %d, Sent {W=%d/R=%d/Canceled=%d} IO's\n", p[2].n_cycles, p[3].n_cycles, p[0].n_cycles, p[1].n_cycles, num_canceled_ios);
	return 0;
}

TEST_FUNC int unitest_async_attach_detach_during_rebuild(struct NVMeshSystem *sys) {
	t_async_test_params p[3];
	int n_threads = ARRAY_SIZE(p), v;
	for (v=0; v<n_threads; v++)
		t_async_test_params_init(p[v], sys, -1, -1, BUNITEST_ASYNC_TEST_SLOWED_DOWN_SYNCS);

	tomaSimulator_protoBugs(true, true);	  // Must remove simulator guards because client unsibscribes so fast that toma did not have time to receive unregister message, but the message was legaly sent
	BUG_ON(!(p[0].kthread = kthread_run(__thread_async_R1_stale_injects, &p[0], "ut:set_SSP")));
	BUG_ON(!(p[1].kthread = kthread_run(__thread_async_attach_dettach  , &p[1], "ut:attach_detach")));
	BUG_ON(!(p[2].kthread = kthread_run(__thread_async_stale_R1_rebuild, &p[2], "ut:stale_rebuild")));
	if (!ut_conf__get_base()->is_valgrind) msleep(400);												// Let the threads run together
	else 								   msleep(40);												// Let the threads run together
	for (v=0; v<n_threads; v++) if (p[v].kthread) kthread_stop(p[v].kthread);
	tomaSimulator_protoBugs(false, false);
	NVMeshSystem_wipe_all_stale_locks(sys);
	BUG_ON(!NVMeshSystem_is_stable(sys));
	unitest_print("*************** Attach/Detach %d, Stale-Rebuilds %d, SSp=%d\n", p[1].n_cycles, p[2].n_cycles, p[0].n_cycles);
	return 0;
}
/* Upgrade volume vol0 to first chunk 4 mirrored, second 3 mirrored */
static inline void __set_rdma_status_for_n_mirror(struct NVMeshSystem *sys, const struct volumeDescriptor *vol, bool enable) {
	const u32 max_n_owners   = vol->locks_scheme.maxNOwners;
	struct disk_range * seg = vol->segs;
	int c, s;
	for (c=0; c<vol->nChunks; c++) {
		const int nMirror = (int)seg->replicas, stripe_width = (int)seg->stripe_width;
		for (s=0; s<nMirror*stripe_width; s++, seg++)
			if ((seg->stripe_index % nMirror) >= max_n_owners)
				ramDiskSimulator_setrdma(&sys->servers[seg->node_id].ramDisk, enable);
	}
}

TEST_FUNC int unitest_n_mirror(struct NVMeshSystem *sys, const char* action, __unitest_updowngrade_mode mode){
	int rv = 0, s, volInd = 0;
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client
	struct volumeDescriptor   *vol 		 = &sys->mdb.vols[volInd];  // Move segment of third volume
	struct nvmeibc_block_disk *disks 	 = sys->mdb.discs[volInd];	// Disks of the volume
	struct tTopoOfVolume* cfv = &sys->tcf.vols[volInd];			// Toma Configuration of the current volume
	const int striping	= cfv->chunks[0].stripeWidth;
	unsigned long orig_size 			 = client->devs[volInd]->size;
	int n_new_segs= 2*vol->nSegments;
	int mirror	= -1;

	// Static variables to store during expansion so we can easily reduce the size of the CV back
	static struct volumeDescriptor backup_vol;    // Save the volume so we will be able to restore it, easily.

	if (action[0] == 'u') goto _upgrade_to_4_mirrored;
	if (action[0] == 'd') goto _downgrade_back_to_2_mirrored;
	if (action[0] == 'r') goto _reattach;
	BUG_ON(true);

_upgrade_to_4_mirrored:			// ------------------------------------ Convert Vol0 chunk0 to 4 mirrored (12 segments of 4 locks each)
	mirror	= 4;
	backup_vol = *vol;
	vol->segs = (struct disk_range*)sim_kmalloc(sizeof(*vol->segs)*NVMESH_N_PHYS_DISKS, GFP_KERNEL);
	for (s=0; s<backup_vol.nSegments; s++)
		disk_range_copy(&vol->segs[s], &backup_vol.segs[s]);

	if (mode == UNITEST_UPDOWNGRADE_COLD)
		send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	for (s=0; s<backup_vol.nSegments; s++){
		vol->segs[s].replicas = mirror;			// Update config-fs by replicating each segment to 2
		vol->segs[s].dlba_start = (s%5)*LOCKSET_4KS + sys->servers[s].ramDisk.committed_addr.block;	// Pseudo-random different offset of segment on each disk. This better tests the linkage of commands and locks
	}
	for (s=vol->nSegments; s<n_new_segs; s++) {
		disk_range_init(&sys->mdb, &vol->segs[s], __to4K(0), s, s, 0, volInd, mirror, striping, 1, vol->segs[0].length/LOCKSET_4KS, 1, false);
		mongo_db_simu_reconf_set_new(disks, -1, s, &vol->segs[s], vol->segs[s].dlba_start);
		vol->segs[s].dlba_start = (s%5)*LOCKSET_4KS + sys->servers[s].ramDisk.committed_addr.block;	// Pseudo-random different offset of segment on each disk. This better tests the linkage of commands and locks
	}
	vol->nSegments += vol->nSegments;

	// ------------------------------------ Convert Vol0 chunk1 to 3 mirrored (9 segments of 8 locks each )
	mirror = 3;
	n_new_segs = (mirror*striping) + vol->nSegments;
	for (s=vol->nSegments; s<n_new_segs; s++) {
		disk_range_init(&sys->mdb, &vol->segs[s], __to4K(orig_size), s, (s-vol->nSegments), 1, volInd, mirror, striping, 1, RAMDISK_DATA_LOCK_SIZE, 1, false);	// Update config-fs input
		mongo_db_simu_reconf_set_new(disks, -1, s, &vol->segs[s], vol->segs[s].dlba_start);
	}
	vol->nSegments = n_new_segs;

	mongo_db_simu_reconf_set_depricate(&sys->mdb, &sys->tcf, volInd, 0, NULL, 0, NULL);
	// Convert sys-admin definition to configuration
	tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
	vol->nChunks++;
	mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
	send_command_to_vol_attach_or_update(sys, -1, volInd);
	__set_rdma_status_for_n_mirror(sys, vol, false);
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	goto _out;

_downgrade_back_to_2_mirrored:		// ------------------------------------ Convert Vol0 back to 2 mirrord.
	mirror	= 2;
	if (mode == UNITEST_UPDOWNGRADE_COLD)
		send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	for (s=backup_vol.nSegments; s<vol->nSegments; s++) {
		mongo_db_simu_reconf_set_new(disks, s, -1, NULL, -1);
		sys->mdb.srvrs[s].disk_alloc_end = sys->servers[s].ramDisk.committed_addr.block;
	}
	mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, volInd);
	__set_rdma_status_for_n_mirror(sys, vol, true);
	volumeDescriptor_free(vol);
	*vol = backup_vol;
	tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
	mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
	send_command_to_vol_attach_or_update(sys, -1, volInd);
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	goto _out;

_reattach:
	mongo_db_simu_reconf_cleanup(&sys->mdb, &sys->tcf, volInd);
	tTopoOfVolume_destroy(&sys->tcf.vols[volInd]);
	mongo_db_simu_cnv_to_toma_topo_vol(&sys->mdb.vols[volInd], &sys->tcf.vols[volInd], volInd);
	send_command_to_vol(sys, -1, volInd, volCmds_Detach);
	send_command_to_vol(sys, -1, volInd, volCmds_New);
	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	goto _out;

_out:
	BUG_ON(rv);
	return rv;
}

/* Verify that lock server works properly (Takes correct types of locks on correct segments) */
typedef struct ground_thruth_lock {
	struct nvmeibc_disk *disk;	// disk on which 'si' segment reside
	u16 si;		// Index of the segment in the raid
	u16 type;	// enum nvmeibc_rdma_intent, Owner or Active
	u16 addr;	// Address on disk
} ground_thruth_lock;
#define ground_thruth_lock_create(T, A, B, C) ({T.si = A; T.type = B; T.addr = C;})
TEST_FUNC int unitest_GoodPathLockServer_n_mirrored(struct NVMeshSystem *sys) {
	int vol_i = 0, gti = 0, i;
	const struct nvmeibc_locks_scheme_conf *lock_srvr = &sys->mdb.vols[vol_i].locks_scheme;
	const int locks_shift = ((1<<LOCKSET_SHIFT)/LOCKSET_SLICES);				// Todo, change to lock server
	const struct nvmeibc_topology *t = ___get_tail_topo_of_device(sys, vol_i);
	const struct disk_range *conf = sys->mdb.vols[vol_i].segs;
	const u64 vol_size = nvmeibc_block_t_to_b(t)->size, start_lba = 0;				// start from first block of volume
	struct dp_io_topo_iterator topo_it;
	struct nvmeibc_cmd_lock locksets[N_MAX_RAID_LOCKS] = {{0}};
	ground_thruth_lock gt[N_MAX_RAID_LOCKS] = {{0}};
	dp_io_topo_iterator_init(&topo_it, start_lba, vol_size, t, nvmeibc_get_chunk_ind_of_lba(start_lba, t));
	for (gti = 0; dp_io_topo_iterator_next(&topo_it, 'l'); gti++) {								// Traverse the volume by locksets
		/* Step 1. Generate locks with production code */
		const struct disk_range *cpr = &conf[(gti % conf->stripe_width) * conf->replicas];		// Raid start in mongo-db configuration
		const int disk_offset =             ((gti /  cpr->stripe_width) * cpr->stripe_size);	// Disk offset
		int own_i, step = 0, n_rdma_segs = 0;
		enum nvmeibc_rdma_intent sibs_type = 0;
		const struct nvmeibc_raid1 *pr = topo_it.res.r;
		int n_locks = dp_fill_locks_for_raid(pr, NVMEIB_BLOCK_IO_OP_WRITE, topo_it.res.rlba, locksets);
		n_locks = min(n_locks, N_MAX_RAID_LOCKS);
		BUG_ON(n_locks != min((int)lock_srvr->maxNOwners, (int)cpr->replicas));

		/* 2. Build ground truth locks for comparison*/
		switch ((lock_server_type_e)lock_srvr->type) {
			case OWNER_SCHEME_FIRST_L_INC_A: n_rdma_segs = n_locks;       step = +1; sibs_type = NVMEIBC_CMD_LOCK_COPY_OWNER; break;
			case OWNER_SCHEME_SL_START_INC_A:n_rdma_segs = cpr->replicas; step = +1; sibs_type = NVMEIBC_CMD_LOCK_COPY_OWNER; break;
			case OWNER_SCHEME_SL_START_DEC_C:n_rdma_segs = cpr->replicas; step = -1; sibs_type = NVMEIBC_CMD_LOCK_COPY_OWNER; break;
			default: BUG_NOT_IMPLEMENTED_YET;
		}
		own_i = ((gti / cpr->stripe_width) / locks_shift) % n_rdma_segs;
		ground_thruth_lock_create(gt[0], own_i, NVMEIBC_CMD_LOCK_OWNER, cpr[own_i].dlba_start + disk_offset);	// Owner lock
		for (i = 1; i < n_locks; i++) {																		// Active locks
			gt[i].si   = ((n_rdma_segs + gt[i-1].si + step) % n_rdma_segs);
			gt[i].disk = nvmeibc_disk_from_base(pr->segments[gt[i].si].disk);
			gt[i].type = sibs_type;
			gt[i].addr = cpr[gt[i].si].dlba_start + disk_offset;
		}
		// Verify that locks are taken correctly
		for (i = 0; i < n_locks; i++) {
			struct nvmeibc_cmd_lock * l = &locksets[i];
			BUG_ON( (u16)(l->ds-pr->segments) != gt[i].si);
			BUG_ON( (u16)l->type              != gt[i].type);
			BUG_ON( (u16)l->address           != gt[i].addr);
		}
		if (topo_it.nlbas_chunk == LOCKSET_SLICES) {								// Move ground thruth to next chunk
			conf += (conf->stripe_width*conf->replicas);
			gti  = -1;															// Next iteration will start from zero
		}
	}
	return 0;
}

TEST_FUNC int unitest_GoodPathIO_n_mirrored(struct NVMeshSystem *sys){
	int c, rv = 0;
	u64 la;
	struct clientSimulator    *client 	 = &sys->clients[0];	// Current client
	if (NVMEIBC_SECTOR_SIZE < PAGE_SIZE) {
		rv = unitest_IO(client, 0, 1,  10);						// Test partial write of 4K pages if block devices uses smaller blocks
	}
	rv = unitest_IO(client, 0, _addr4k(3,30),  _addr4k(0,1)); 	// Test IO on a single raid1
	rv = unitest_IO(client, 0, _addr4k(3,30),  _addr4k(3,5)); 	// Test IO crossing stripes on all disks of Raid1+0

	NVMeshSystem_all_clients_mirror_edic(sys, false); // Edic should be disabled as data will be intentionally corrupted during this test

	if (1) {				// --------------------- Test that read lock piggibacked on correct segments (read owner != lock owner), by making lock contended and forcing IO to time-out
		const int lenBlocks  = 1, volInd = 0, memSize	 = lenBlocks*NVMEIBC_SECTOR_SIZE;
		struct nvmeibc_block_device *dev = client->devs[volInd];
		const struct nvmeibc_locks_scheme_conf *lock_srvr = &sys->mdb.vols[volInd].locks_scheme;
		const unsigned long prev_rtj = dev->max_retry_jiffies;
		u8        *mem 		 = sim_kmalloc(memSize, GFP_KERNEL);
		memset(mem, 0, memSize);
		clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs 0", dev->name);
		for (c = 0; c < sys->mdb.vols[volInd].nChunks; c++) {						// For each chunks
			struct disk_range *first_seg_in_chunk = &sys->mdb.vols[volInd].segs[c*12];
			const u64 for_each_blockset = (__from4K(first_seg_in_chunk[0].length)/(1u<<LOCKSET_SHIFT)); // Test read on each lockset zigzagging in the first protection raid
			for (la = 0; la < for_each_blockset; la++) {  // Below calculation is done in units of blocks, not 4K
				const u32 raid_members = first_seg_in_chunk[0].replicas;
				const u64 num_lock_segments = min(lock_srvr->maxNOwners, raid_members);
				const u64 slice_start_seg_ind = la % raid_members;
				const u64 lock_seg_ind = (lock_srvr->type == OWNER_SCHEME_FIRST_L_INC_A) ? (la % num_lock_segments) : slice_start_seg_ind;			// 'i' = segment ind - read ownership rotates on all segments or always from first
				struct disk_range* curData = &first_seg_in_chunk[slice_start_seg_ind];
				struct disk_range* curLock = &first_seg_in_chunk[lock_seg_ind]; // Locks rotate on first L segments starting from either member_0 or slice_start
				struct serverSimulator *serverLock = &sys->servers[curLock->node_id];
				const u64 ls_offset = la* (1<<LOCKSET_SHIFT);				// Even segments have owner lock on even locksets, odd segments have owner lock on odd locksets. So add offset to always apply the stale owner lock
				const u64 small_offset= __from4K(3);						// Offset to read somwhere in the middle of the lockset
				const u64 virtStart = __from4K(disk_range_get_start_addr(curData)) + (ls_offset*curData->stripe_width) + small_offset;
				const u64 physStart = __from4K(curLock->dlba_start)			       +  ls_offset	  			  		   + small_offset;
				u64 *dst            = (u64*)physSegStartPtr_off(curData,       __to4K(ls_offset                        + small_offset));
				dp_io_stats_clear_counter(&dev->dp.io_stats, DP_IO_STATS_TIMED_OUT);
				dst[0] = __unitest_get_pattern();                           // Add a unique info for the IO to read
				ramDiskSimulator_lockDo(&serverLock->ramDisk, __to4K(physStart));
				rv = osSimulator_readArr( &client->OS, volInd, virtStart, lenBlocks, mem); BUG_ON(rv);
				clientSimulator_wait_for_all_bio_ops(client);
				BUG_ON(dp_io_stats_get_counter(&dev->dp.io_stats, DP_IO_STATS_TIMED_OUT) != 1);					// IO was timed-out because lock was contended
				BUG_ON(((u64*)mem)[0] != dst[0]);						// The data was read, so verify that IO accessed correct location regardless of piggyback lock
				ramDiskSimulator_lockUn(&serverLock->ramDisk, __to4K(physStart));
			}
		}
		clientSimulator_send_to_cli_va(client, "#%s|max_retry_secs %d", dev->name, (int)(prev_rtj/HZ));


		if (1) { 			// --------------------- Test that dirtybit piggibaged on correct segments
			const int r1_ind = 0;
			struct tTopoOfPraid *r1  = tTopoOfVolume_getRaid1(&sys->tcf.vols[volInd], r1_ind);
			struct tTopoOfVolume *tv = &sys->tcf.vols[volInd];
			struct disk_range *seg = &sys->mdb.vols[volInd].segs[r1_ind];
			const u64 small_offset= __from4K(6);						// Offset to read somwhere in the middle of the lockset
			const u64 vlba = (1<<LOCKSET_SHIFT)*seg->stripe_width + small_offset;
			const u64 dlba = (1<<LOCKSET_SHIFT)					  + small_offset;
			union nvmeibc_dbits_entry expected_db_val;
			int di, di_arr[2][2] = {{0, 2}, {1, 3}};						// 2 Double Degraded modes type. Todo: change to proper loops NVMESH-4712
			for (di = 0; di < 2; di++) {
				r1->s[di_arr[di][0]].access_mode = NVMEIBTC_DS_MODE_DEAD;	//
				r1->s[di_arr[di][1]].access_mode = NVMEIBTC_DS_MODE_DEAD;
				tTopoOfPraid_update_segs_by_access_mode(r1, tv->locks_scheme);
				expected_db_val.all_bits = 0;
				expected_db_val.bsmod.dead1 = di_arr[di][0] + 1; // dead1 < dead0, di_arr[0] < di_arr[1]
				expected_db_val.bsmod.dead0 = di_arr[di][1] + 1; // dead1 < dead0, di_arr[0] < di_arr[1]
				tomaSimulator_switchTopo_dummy(volInd, r1_ind, SW_TOPO__WAIT_ACK, NULL);
				rv = osSimulator_writeArrWait( &client->OS, volInd, vlba, 1, mem);	BUG_ON(rv);
				for (la = 0; la < (u64)r1->header.n_segments; la++) {
					union nvmeibc_dbits_entry *db_val = physSegDBIdxPtr_off(&seg[la], __to4K(dlba));
					if (r1->s[la].access_mode != NVMEIBTC_DS_MODE_DEAD) {
						BUG_ON(db_val->all_bits != expected_db_val.all_bits);
						db_val->all_bits = 0;		// Turn off dirtybits back to zero
					} else
						BUG_ON(db_val->all_bits != 0);
				}
				r1->s[di_arr[di][0]].access_mode = NVMEIBTC_DS_MODE_RW;
				r1->s[di_arr[di][1]].access_mode = NVMEIBTC_DS_MODE_RW;
				tTopoOfPraid_update_segs_by_access_mode(r1, tv->locks_scheme);
				tomaSimulator_switchTopo_dummy(volInd, r1_ind, SW_TOPO__WAIT_ACK_DR, NULL); // Wait for registration process to finish on the previously dead segments and system become IOable
			}
		}
		sim_kfree(mem);
	}

	// Reenable edic
	NVMeshSystem_all_clients_mirror_edic(sys, true);
	NVMeshSystem_wipe_all_md_of_disks(sys); // Cleanup

	BUG_ON(!NVMeshSystem_is_stable(sys));					// System must be in a stable state
	BUG_ON(rv);
	return rv;
}

bool is_not_ioable_n_rep_all_locks_down(int first_dead, int n_deg, struct nvmeibc_locks_scheme_conf *ls) {
	const bool more_dead_than_locks = (n_deg >= (int)ls->maxNOwners);
	if (ls->type == OWNER_SCHEME_FIRST_L_INC_A)
		return more_dead_than_locks && (first_dead == 0);
	else
		return more_dead_than_locks;		// ls->max_n_owners consecutive dead segments, At least 1 segment does not have any lock to protect it
}

/* Test client's raid going in and out of a degraded mode */
TEST_FUNC int unitest_DegradedMode_n_mirrored(struct NVMeshSystem *sys){
	int volInd = 0, ind_dead_seg = 0, j, n_deg, node_ind;	// Volume will be used to test switch topology
	struct clientSimulator *client = &sys->clients[0];			// Test via the first client
	struct nvmeibc_locks_scheme_conf *locks_scheme = &sys->mdb.vols[volInd].locks_scheme;
	struct nvmeibc_block_device *dev = client->devs[0];
	struct nvmeibc_sync_stats *stats = &dev->dp.sync_rsrcs.stats;
	struct disk_range *curSeg = NULL;
	const int chunkOffsetVLBA = _addr4k(0,31);						// IO's will be at this offset from beggining of the test chunk
	int rv = 0, c, seg_in_r1_offset = 0;
	int lenBlocks = __from4K(2);								// Length of IO. Span on 2 blocksets
	int memSize	 		 = lenBlocks*NVMEIBC_SECTOR_SIZE;		// Total array in bytes
	u8 *mem = sim_kmalloc(memSize, GFP_KERNEL);									// Array to read/write to disk
	enum NVMEIBTC_DS_MODE seg_stats[N_MAX_RAID_SLICE_LEN];// = {[0 ... N_MAX_RAID_SLICE_LEN-1] = NVMEIBTC_DS_MODE_RW};
	array_fill(seg_stats, NVMEIBTC_DS_MODE_RW);
	#define segs_in_chunk() (curSeg->replicas * curSeg->stripe_width)
	curSeg = &sys->mdb.vols[volInd].segs[0];
	#define __verify_no_dbits(db_vals) ({ for (j = 0; j < r1->header.n_segments; j++) {	BUG_ON(db_vals[j]->all_bits != 0); } })
	#define __clean_cur_dbits(db_vals) ({ for (j = 0; j < r1->header.n_segments; j++) {	db_vals[j]->all_bits = 0; } })

	for (c = 0; c < sys->tcf.vols[volInd].nChunks; c++, seg_in_r1_offset += segs_in_chunk(), curSeg += segs_in_chunk()) {	// Loop on 4,3 mirror
		struct tTopoOfPraid* r1 = &sys->tcf.vols[volInd].chunks[c].raids[0];
		const u64 ioVLBA = __from4K(curSeg->bd_start) + chunkOffsetVLBA;		// Hit the first blockset of first praid in a chunk
		union nvmeibc_dbits_entry* db_vals[4];
		BUG_ON(curSeg->stripe_index != 0);						// Must be first in chunk. Otherwise calculations below will not work
		BUG_ON(r1->header.n_segments != (4-c));					// We are testing 3/4-mirror here, 4 mirror first chunk, 3 mirror second
		for (j = 0; j < r1->header.n_segments; j++) {			// Set dbit entries pointers for injection and verification
			db_vals[j] = physSegDBIdxPtr_off(curSeg+j, __to4K(chunkOffsetVLBA));
		}
		for (j = r1->header.n_segments; j < N_MAX_RAID_SLICE_LEN; j++)
			seg_stats[j] = NVMEIBTC_DS_MODE_INVALID;

	// -------------------- Simulate full toma protocol of entering and quitting from double/tripple degraded modes (seg 'ind_dead_seg' until 'ind_dead_seg+n_deg' are dead)
	for (ind_dead_seg = 0; ind_dead_seg < r1->header.n_segments; ind_dead_seg++) {
		for (n_deg = 1; n_deg<=(r1->header.n_segments-ind_dead_seg); n_deg++) {
			const bool is_no_io_topo = is_not_ioable_n_rep_all_locks_down(ind_dead_seg, n_deg, locks_scheme);
			const bool only_1_lock_is_alive = ((n_deg + 1) == r1->header.n_segments);
			const int owner_seg_ind = ((ind_dead_seg != 0) ? 0 : r1->header.n_segments-1);	// holding primary owner lock
			struct disk_range *ownerSeg = &curSeg[owner_seg_ind];
			const u64 io_blockset_ind = __from4K(ownerSeg->dlba_start) + chunkOffsetVLBA;
			// pr_alert("______________c=%d_[%d..%d]\n",c, ind_dead_seg, ind_dead_seg+n_deg-1);
			if (n_deg == r1->header.n_segments)
				continue;			// Illegal topo, nothing to test
			for (j = 0; j < n_deg; j++) {		// Move all degradedes to D
				seg_stats[ind_dead_seg+j] = NVMEIBTC_DS_MODE_DEAD;
			}
			for (j = 0; j < n_deg; j++) {
				serverSimulator_disconnect(serverOf(&client->physDiscs[curSeg[ind_dead_seg+j].node_id]));
				tomaSimulator_unreg_raid1(r1uuid(r1),                    ind_dead_seg+j); // {DEAD , RW}
			}
			BUG_ON(client->devs[volInd]->topologies.io_perm != (is_no_io_topo ? NVMEIB_IO_TYPE_PERMIT_NONE_INV : NVMEIB_IO_TYPE_PERMIT_ALL));
			if (is_no_io_topo){
				for (j = 0; j < n_deg; j++) {
					node_ind = curSeg[ind_dead_seg+j].node_id;
					serverSimulator_re_connect(serverOf(&client->physDiscs[node_ind]));
					tomaSimulator_waitProtoEnd(&sys->servers[node_ind].simToma);
				}
				goto _back_to_normal_topo;
			}
			// Degraded D mode tests
			warn_on_too_many_degraded = (n_deg <= 2);		// Dbits marker still support only 2 degraded segs
			__unitest_do_degraded_io(sys, r1, seg_in_r1_offset, mem, volInd, ioVLBA, lenBlocks);

			if (only_1_lock_is_alive) { // Test Stale 2 dirty in max degraded
				struct serverSimulator *curServer = serverOf(&client->physDiscs[ownerSeg->node_id]);
				union nvmeibc_dbits_entry* db = db_vals[owner_seg_ind];
				const int one_dead_seg = (r1->header.n_segments - owner_seg_ind - 1);	// Just some dead segment
				u16 double_unknown = nvmeib_dbits_entry_build_unk(-1,-1).all_bits;

				// Test conversion of stale to Exact max dirty bits, without unknowns
				nvmeibc_datapath_syncs_zero_stats(&dev->dp.sync_rsrcs);		// Zero stats for easier counting
				db->all_bits = 0;
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				ramDiskSimulator_lockStale(&curServer->ramDisk, io_blockset_ind);			// Put stale special value in the lock of the IO.
				rv = osSimulator_readArrWait(&client->OS, volInd, ioVLBA, lenBlocks, mem);		REPORT_ERROR(rv);
				BUG_ON(nvmeibc_dbits_has_unknowns(db));
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				BUG_ON(stats->num_full_blockset_ok != 0);				// Stale to dirty dont count as syncs

				// Test conversion of stale + 1 dbit to max known dirty bits
				nvmeibc_datapath_syncs_zero_stats(&dev->dp.sync_rsrcs);		// Zero stats for easier counting
				db->all_bits = nvmeib_dbits_entry_build_for_seg(one_dead_seg).all_bits;
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				ramDiskSimulator_lockStale(&curServer->ramDisk, io_blockset_ind);			// Put stale special value in the lock of the IO.
				rv = osSimulator_readArrWait(&client->OS, volInd, ioVLBA, lenBlocks, mem);		REPORT_ERROR(rv);
				BUG_ON(nvmeibc_dbits_has_unknowns(db));
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				BUG_ON(stats->num_full_blockset_ok != 0);				// Stale to dirty dont count as syncs

				// Inject unknown + Stale special, Expect: Removes Stale and retains Unknown Dbits but exact
				db->all_bits = double_unknown;
				ramDiskSimulator_lockStale(&curServer->ramDisk, io_blockset_ind);			// Put stale special value in the lock of the IO.
				rv = osSimulator_readArrWait(&client->OS, volInd, ioVLBA, lenBlocks, mem);		REPORT_ERROR(rv);
				BUG_ON(nvmeibc_dbits_has_unknowns(db) == false);
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				if (curSeg->replicas > 2) {		// Todo: Here add tripple/quadrupple unknown as well
					BUG_ON(db->all_bits != double_unknown);
				} else {
					BUG_ON(db->all_bits != nvmeib_dbits_entry_single_unk().all_bits);
				}
			}
			__clean_cur_dbits(db_vals);	// Remove unknowns before we transitino to next topo

			for (j = 0; j < n_deg; j++) {		// Move all degradedes to W-
				node_ind = curSeg[ind_dead_seg+j].node_id;
				serverSimulator_re_connect(serverOf(&client->physDiscs[node_ind]));
				tomaSimulator_waitProtoEnd(&sys->servers[node_ind].simToma);
				seg_stats[ind_dead_seg+j] = NVMEIBTC_DS_MODE_W_IS_DIRTY;
			}
			tomaSimulator_switchTopoEC( r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);
			__unitest_do_degraded_io(sys, r1, seg_in_r1_offset, mem, volInd, ioVLBA, lenBlocks);

			if (true) { // Dirty convict tests (various dbits values combination)
				struct serverSimulator *curServer = serverOf(&client->physDiscs[ownerSeg->node_id]);
				const u16 dbits_vals[3] = {0x7 /*Invalid seg6*/, nvmeib_dbits_entry_single_unk().all_bits, nvmeib_dbits_entry_build_unk(-1,-1).all_bits};
				int d;
				__clean_cur_dbits(db_vals);	// Remove all dbits before we transitino to next topo
				for (d = 0; d < 3; d++ ){
					db_vals[owner_seg_ind]->all_bits = dbits_vals[d];		// Inject invalid DBit into primary owner - consider replacing this
					warn_on_too_many_degraded &= (d != 0);					// Invaliud dbit for non existing seg 6
					tomaSimulator_recoverOK_Blocking(r1, ownerSeg, RCVR_DIRTY_REBUILD_CONV);
					warn_on_too_many_degraded = (n_deg <= 2);
					BUG_ON(db_vals[owner_seg_ind]->all_bits);
					ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
					ramDiskSimulator_verify_no_dirty_bits(&curServer->ramDisk);
				}
				__verify_no_dbits(db_vals);
			}

			for (j = 0; j < n_deg; j++) {
				seg_stats[ind_dead_seg+j] = NVMEIBTC_DS_MODE_W;
			}
			tomaSimulator_switchTopoEC( r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);
			__unitest_do_degraded_io(sys, r1, seg_in_r1_offset, mem, volInd, ioVLBA, lenBlocks);

			if (true) { // Test Unknown Dbits
				struct serverSimulator *curServer = serverOf(&client->physDiscs[ownerSeg->node_id]);

				// Stale lock fixes dbits as well as stale
				nvmeibc_datapath_syncs_zero_stats(&dev->dp.sync_rsrcs);		// Zero stats for easier counting
				*db_vals[owner_seg_ind] = nvmeib_dbits_entry_build_for_seg(ind_dead_seg);
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				ramDiskSimulator_lockStale(&curServer->ramDisk, io_blockset_ind);			// Put stale special value in the lock of the IO.
				rv = osSimulator_readArrWait(&client->OS, volInd, ioVLBA, lenBlocks, mem);		REPORT_ERROR(rv);
				// Fixes DBs and Stale in this blockset on all segs
				ramDiskSimulator_verify_no_locks(&curServer->ramDisk);
				__verify_no_dbits(db_vals);
				clientSimulator_wait_for_all_sync_ops(client);
				BUG_ON(stats->num_full_blockset_ok != 1);

				// Unknown Dbits recovery with dbits only in owner lock
				*db_vals[owner_seg_ind] = nvmeib_dbits_entry_build_unk(-1,-1);
				tomaSimulator_recoverOK_Blocking(r1, ownerSeg, RCVR_DIRTY_REBUILD);
				clientSimulator_wait_for_all_sync_ops(client);
				if (only_1_lock_is_alive) {
					BUG_ON(stats->num_dirty_bit_suspect != 1); 	// Dirty suspect was resolved via sync
				} else {
					BUG_ON(stats->num_dirty_bit_suspect != 0); 	// Dirty suspect was resolved via other locks copies
				}
				__verify_no_dbits(db_vals);
				nvmeibc_datapath_syncs_zero_stats(&dev->dp.sync_rsrcs);		// Zero stats for easier counting

				// Unknown Dbits recovery with dbits only in all lock copies
				for (j = 0; j < r1->header.n_segments; j++) {
					*db_vals[j] = nvmeib_dbits_entry_build_unk(-1,-1);
				}
				tomaSimulator_recoverOK_Blocking(r1, ownerSeg, RCVR_DIRTY_REBUILD);
				clientSimulator_wait_for_all_sync_ops(client);
				BUG_ON(stats->num_dirty_bit_suspect != 1); 	// Dirty suspect was resolved via sync
				__verify_no_dbits(db_vals);
			}
			warn_on_too_many_degraded = true;	// Set back to enable, done with 3+ degraded mode

_back_to_normal_topo:
			for (j = 0; j < n_deg; j++) {
				seg_stats[ind_dead_seg+j] = NVMEIBTC_DS_MODE_RW;
			}
			tomaSimulator_switchTopoEC( r1uuid(r1), seg_stats, SW_TOPO__WAIT_ACK, NULL);// {RW	, RW} - normal
			clientSimulator_wait_for_all_bio_ops(client); NVMeshSystem_serialize(sys); // Todo: Wait until all topos are freed. Last IO operation was resubmitted and terminated but its topo reference was passed to the locks and did not have time to free
			BUG_ON(!NVMeshSystem_is_stable(sys));	// Extremely rarelly reports unreal bug when prev topo did not have time to delete.
		}
	}
	} // for (c = 0; chunks....
	BUG_ON(rv || (warn_on_too_many_degraded == false));
	sim_kfree(mem);
	_NI_dmesg(trace_bunitest_unitest_DegradedMode_n_mirrored, "*************** end");
	return rv;
}

TEST_FUNC int unitest_nvmesh_client_restart(struct NVMeshSystem *sys) {
	int rv;
	if (1) { // Scenario 1: Test as if shutdown was called 1 or more times
		rv = NVMeshSystem_init(sys);

		prepare_cli_status_verification_for_shutdown(&sys->clients[0], false);
		clientSimulator_send_to_cli(sys->clients, "shutdown\nshutdown\nshutdown");
		clientSimulator_send_to_cli(sys->clients, "attachv vol1 012345678901234 --RW 0\ndetachu vol2\n"); // Request should be ignored
		wait_for_cli_status_verification(&sys->clients[0]);
		rv = NVMeshSystem_destroy(sys);
	}
	if (1) { // Scenario 2: Test as if shutdown never called, unsafe but valid behaviour
		rv = NVMeshSystem_init(sys);
		rv = NVMeshSystem_destroy(sys);
	}
	if (0) { // Scenario 3: Test as if shutdown was called but before it detached all volumes, module remove was called
		rv = NVMeshSystem_init(sys);
		// Daniel, Todo: Fill here
		rv = NVMeshSystem_destroy(sys);
	}
	BUG_ON(rv);
	return rv;
}

TEST_FUNC int unitest_cmp_blocks_validation(void) {
	u32 i, j, data_size = 2, parity_size = 2, snake_size = 1;		// Checking for D = 2 P = 2, Snake = 1
	bool check_crc = true, dbg_di = true;
	cmp_blocks_file_data f_data[N_MAX_RAID_SLICE_LEN];
	uint64_t rlba = 0;
	int rv = 0;

	memset(f_data, 0, sizeof(f_data));

	//init sgements and crcs with random data
	srand ((unsigned int) time (NULL));
	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i) {
		f_data[i].md.raw = 0;
	}

	if (cmp_blocks_ec(data_size, parity_size, f_data, check_crc, dbg_di, snake_size, rlba, false , false /*fix the CRC*/, 0, 0) != CB_OK) {
		rv = -1;
	}

	for (i = 0; i < N_MAX_RAID_SLICE_LEN; ++i) {
		f_data[i].md.D.version = NVMEIBC_DATA_MD_VERSION;
		for (j = 0; j < 16; ++j) {
			f_data[i].segment[j] = rand();
			if (i >= data_size)
				f_data[i].md.P.edic = rand();
			else
				f_data[i].md.D.edic = rand();
		}
	}

	if (cmp_blocks_ec(data_size, parity_size, f_data, check_crc, dbg_di, snake_size, rlba, true /*fix the slice*/, false, 0, 0) != CB_CRC_AND_PARITY_MISMATCH) {
		rv = -1;
	}

	if (cmp_blocks_ec(data_size, parity_size, f_data, check_crc, dbg_di, snake_size, rlba, false , true /*fix the CRC*/, 0, 0) != CB_CRC_MISMATCH) {
		rv = -1;
	}

	if (cmp_blocks_ec(data_size, parity_size, f_data, check_crc, dbg_di, snake_size, rlba, false , false /*fix the CRC*/, 0, 0) != CB_OK) {
		rv = -1;
	}


	if (rv) {
		unitest_print("*** cmp_blocks validation  - %s\n", unitest_rv_to_string(rv));
		BUG_ON(true);
	} else
		unitest_print("*** cmp_blocks validation - %s\n", unitest_rv_to_string(rv));
	return rv;
}

/*****************************************************************************/
static const struct blk_unittest_conf * __parseArgs(int argc, char* argv[]){
	unitest_print("%d[args]: ", argc);
	for (int i = 0; i < argc; i++) {
		unitest_print("|%s| ", argv[i]);
	}
	unitest_print("\n");
	if (argc < 1) 	// Missing mandatory params. Print parameters and exit
		ut_conf__print_help();

	// set module param
	nvmeibc_debug_level = -2;
	tracer_nvmeibc_debug_level = 0;		// By default disable logs, since unitests produce a massive amount of megabytes of those.
	max_ios_per_cpu	= (1<<30);							// By default infinite
	nvmeibcb_dp_io_fail_mgr_disable(NULL);					// We do allow IO errors, because unitest environment causes them
	json_iostats_fixed_size = false;
	ut_conf__parse_args(argc, argv);						// Set defaultconf & parse options
	if (nvmeibc_debug_level<-1) printk_enable(false);		// Disable entire printk() except for critical messages
	return ut_conf__get_instance();
}

static void __print_status_header(const struct blk_unittest_conf *ut_conf) {
	unitest_print("\tCompiled @ " __DATE__ "/" __TIME__ ", git (commit:" KERN_COL_YELLOW "%07lx" KERN_COL_RESET ", tag:%s, branch:%s), USE_RELEASE=%d %s debugger, %d[b] blocks, dbg:%d, nRep:%d, sync:%d, max-pcpu-io=%d\n", (unsigned long)COMMIT_ID, __stringify(VER_TAGID), __stringify(BRANCH_NAME), USE_RELEASE, (is_debugger_present() ? "with" : "no"), NVMEIBC_SECTOR_SIZE, nvmeibc_debug_level, ut_conf->bunitest.nRep, ut_conf->transport.is_disk_callback_sync, max_ios_per_cpu);
	unitest_print("\tECPUs="  KERN_COL_YELLOW "%d" KERN_COL_RESET " TimerCPUs=%d PerCPUs=%d, is_valgrind=%c |\n", ut_conf->kernel_prm.num_ecpu, MAX_NUM_TIMERS_ENGINE, CONFIG_NR_CPUS, bool_to_yes_no(ut_conf->base.is_valgrind));
	_ND(trace_0_bunitest_parseArgs, "D() - is active");		// pr_debug() Display the active prints in the log
	_NT(trace_1_bunitest_parseArgs, "T() - is active");
	_NI_dmesg(trace_2_bunitest_parseArgs, "I() - is active");		// pr_info()
	pr_notice("Notice - is active\n");
	_NW_dmesg(trace_3_bunitest_parseArgs, "W() - is active");		// pr_warning()
	_NE_dmesg(trace_4_bunitest_parseArgs, "E() - is active");		// pr_err()
	unitest_print("unitest_print - is active\n");			// pr_crit()
	pr_alert("Alert - is active\n");						// Always active
	pr_emerg("Emerg - is active\n");
	#ifdef BLKCMP_SO_COMPLETION_PRESERVE_STACK
		pr_crit("BLKCMP: SO");
		#ifdef BLKCMP_IO_COMPLETION_PRESERVE_STACK
			pr_crit("+ IO");
		#endif
		#ifdef BLKCMP_RC_COMPLETION_PRESERVE_STACK
			pr_crit("+ Recov");
		#endif
		pr_crit("\n");
	#endif
}

static inline const char* __lock_server_type_to_string(lock_server_type_e pos) {
	switch (pos) {
	case OWNER_SCHEME_FIRST_L_INC_A : return "FIRST_L++";
	case OWNER_SCHEME_SL_START_INC_A: return "SL_START++";
	case OWNER_SCHEME_SL_START_DEC_C: return "SL_START--";
	default: BUG(); return NULL;
	}
}

int bunitest_s_create(bunitest_s* B) {
	B->conf = ut_conf__get_instance();
	B->test_phase = BUNI_INITIALIZING;
	B->sys        = &B->_sys;
	g_sys 		  = B->sys;		// Todo: remove
	return 0;
}

bool NVMeshSystem_all_clients_ec_edic(struct NVMeshSystem *sys, bool set) {
	bool previous_edic_value = false;
	struct clientSimulator *client = &sys->clients[0];
	for (int v = 0; v < client->nBdevs; v++)
		if (client->devs[v]) {
			previous_edic_value = client->devs[v]->dp.enable_edic_check;
			client->devs[v]->dp.enable_edic_check = set;
		}
	for (int v = 0; v < sys->mdb.nVols; v++) {
		sys->mdb.vols[v].enable_crc_check = set;
	}
	return previous_edic_value;
}

void NVMeshSystem_all_clients_read_has_mutable_bio_buffers(struct NVMeshSystem *sys, int value)
{
	struct clientSimulator *client = &sys->clients[0];
	BUG_ON(value < 0 || 2 < value);
	for (int v = 0; v < client->nBdevs; v++){
		if (client->devs[v]) {
			client->devs[v]->dp.read_has_mutable_bio_buffers = value;
		}
	}
	/*
	 * Fix me when management will add new functionality
	for (int v = 0; v < sys->mdb.nVols; v++) {
		sys->mdb.vols[v].enable_crc_check = set;
	}
	*/
}

bool NVMeshSystem_all_clients_mirror_edic(struct NVMeshSystem *sys, bool set) {
	return NVMeshSystem_all_clients_ec_edic(sys, set);
}

void NVMeshSystem_precondition_mirror_md(struct NVMeshSystem *sys) {
	NVMeshSystem_all_clients_mirror_edic(sys, true);
}


void NVMeshSystem_all_clients_dbg_di(struct NVMeshSystem *sys, bool set) {
	struct clientSimulator *client = &sys->clients[0];
	int v;
	dbg_di_inject_on = set;			// Notify bunitest
	for (v = 0; v < client->nBdevs; v++)  {
		struct nvmeibc_block_device *dev = client->devs[v];
		if (dev)
			dev->dp.enable_di_debug_mode = set;
	}
	unitest_trace_note(all_clients_dbg_di, "enabled=@BOOL_YN", set);
}

void NVMeshSystem_all_clients_di_metadata_wr(struct NVMeshSystem *sys, bool set) {
	struct clientSimulator *client = &sys->clients[0];
	int v;
	if (set)  clientSimulator_send_to_cli(client, "#*|set_read_edic=1");
	else      clientSimulator_send_to_cli(client, "#*|set_read_edic=0");
	for (v = 0; v < client->nBdevs; v++)
		BUG_ON(client->devs[v]->dp.enable_edic_check != set);
}

static void __trace_note_di_tracking(bool enabled)
{
	unitest_trace_note(di_tracking, "enabled=@BOOL", enabled);
}

void NVMeshSystem_di_tracking_enable(struct NVMeshSystem *sys)
{
	if (ut_conf__get_base()->disableDataIntegrityTracking)
		return;

	di_tracker_enable(&sys->di_tracker);
	__trace_note_di_tracking(true);
}

void NVMeshSystem_di_tracking_disable(struct NVMeshSystem *sys)
{
	if (ut_conf__get_base()->disableDataIntegrityTracking)
		return;

	di_tracker_disable(&sys->di_tracker);
	di_tracker_reset(&sys->di_tracker);
	__trace_note_di_tracking(false);
}

void NVMeshSystem_di_tracking_reset(struct NVMeshSystem *sys)
{
	di_tracker_reset(&sys->di_tracker);
}

struct volume_di_tracker_conf vdt_confs[] = {
		{ .trim_assumed_action = DT_TRIM_ZEROES },	// Mirror
		{ .trim_assumed_action = DT_TRIM_NOOP }		// EC
};

void NVMeshSystem_di_tracking_reconf(struct NVMeshSystem *sys, bool for_ec)
{
	di_tracker_reconf(&sys->di_tracker, &vdt_confs[!!for_ec]);
}

struct bunch_of_percpus{
	char single_byte;
	DEFINE_PER_CPU(char , percpu_test);
} __packed;

static struct bunch_of_percpus b_percpus;

TEST_FUNC int test_basic_percpu(__attribute__((__unused__)) struct NVMeshSystem *sys) {
	void *percpu_dyn_test1;
	BUG_ON(!((uintptr_t)&b_percpus.percpu_test & 0x1));
	percpu_dyn_test1 = __alloc_percpu(22, 1);;
	BUG_ON(!percpu_dyn_test1);
	BUG_ON(is_kmalloc_percpu(b_percpus.percpu_test));
	BUG_ON(!is_kmalloc_percpu(percpu_dyn_test1));
	free_percpu(percpu_dyn_test1);
	return 0;
}

struct delayed_work_data {
	struct delayed_work dwork;
	struct completion start_event;
	bool send_event;
	__concurrent_access int data;
	uint32_t sleep_secs;
	uint32_t recurse_delay_jiffs;
};

static void delayed_work_func(struct work_struct *work) {
	struct delayed_work *dwork = container_of(work, struct delayed_work, work);
	struct delayed_work_data *delayed_work_data = container_of(dwork, struct delayed_work_data, dwork);
	if (delayed_work_data->recurse_delay_jiffs)
		schedule_delayed_work(&delayed_work_data->dwork, delayed_work_data->recurse_delay_jiffs);
	if (delayed_work_data->send_event)
		complete(&delayed_work_data->start_event);
	if (delayed_work_data->sleep_secs)
		sleep(delayed_work_data->sleep_secs);
	delayed_work_data->data = 1;
}

static void prepare_delayed_work(struct delayed_work_data *delayed_work_data)
{
	delayed_work_data->data = 0;
	delayed_work_data->send_event = false;
	delayed_work_data->sleep_secs = 0;
	delayed_work_data->recurse_delay_jiffs = 0;
	delayed_work_data->dwork.simu.expect_recursive = false;
	delayed_work_data->dwork.simu.expect_queued = false;
	delayed_work_data->dwork.simu.expect_running = false;
	init_completion(&delayed_work_data->start_event);
	INIT_DELAYED_WORK(&delayed_work_data->dwork, delayed_work_func);
}

TEST_FUNC int unitest_delayed_work(__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	// test cancel_delayed_work_sync in case of submitting and  imediate cancel.
	bool ret;
	struct delayed_work_data delayed_work_data = { 0 };

	//check that we can cancel delayed work that was never scheduled.
	prepare_delayed_work(&delayed_work_data);
	ret = cancel_delayed_work_sync(&delayed_work_data.dwork);
	BUG_ON(ret != true);
	BUG_ON(delayed_work_data.data != 0);

	// check that we can pull out delayed work from timers list.
	for (int indx = 0; indx < 1000; ++indx) {
		prepare_delayed_work(&delayed_work_data);
		delayed_work_data.data = 0;
		delayed_work_data.dwork.simu.expect_queued = true;
		schedule_delayed_work(&delayed_work_data.dwork, HZ);
		ret = cancel_delayed_work_sync(&delayed_work_data.dwork);
		BUG_ON(ret != true);
		BUG_ON(delayed_work_data.data != 0);
	}

	//check that cancel_delayed_work_sync waits for running work to complete.
	prepare_delayed_work(&delayed_work_data);
	delayed_work_data.send_event = true;
	delayed_work_data.sleep_secs = 1;
	delayed_work_data.dwork.simu.expect_running = true;
	schedule_delayed_work(&delayed_work_data.dwork, 0);
	wait_for_completion(&delayed_work_data.start_event);
	ret = cancel_delayed_work_sync(&delayed_work_data.dwork);
	BUG_ON(ret != false);
	BUG_ON(delayed_work_data.data != 1);

	//check that cancel_delayed_work_sync waits for running work to complete, in case of recursive scheduling.
	for (int indx = 0; indx < 5; ++indx) {
		prepare_delayed_work(&delayed_work_data);
		delayed_work_data.send_event = true;
		delayed_work_data.sleep_secs = 1;
		delayed_work_data.dwork.simu.expect_recursive = true;
		delayed_work_data.recurse_delay_jiffs = HZ;
		schedule_delayed_work(&delayed_work_data.dwork, 0);
		wait_for_completion(&delayed_work_data.start_event);
		ret = cancel_delayed_work_sync(&delayed_work_data.dwork);
		BUG_ON(ret != false);
	}

	return 0;
}

TEST_FUNC int unitest_io_stats(__attribute__((__unused__)) struct NVMeshSystem *sys)
{
	const u64 nlbas = 1;
	struct nvmeib_io_stats *stats;
	struct nvmeib_io_counters counters = { 0 };
	char buf[4096];
	struct nvmeib_txt txt = nvmeib_txt_make((struct charvec){.base = buf, .len = sizeof(buf)});
	struct nvmeib_io_counters upd_counters = {
		.total_ops = 1,
		.total_executions = 1,
		.total_size = (nlbas << NVMEIBC_SECTOR_SHIFT),
		.total_latency = 1,
		.total_latency_sqr = 1,
		.total_io_exec = 1,
		.total_e2e_exec = 1,
		.worst_latency = 1,
		.worst_io_exec = 1,
		.worst_e2e_exec = 1,
		.total_sub_block = 0
	};
	stats = nvmeib_io_stats_create_traced("testing_123", VERB_RW_T_BITMASK, NVMEIBC_SECTOR_SIZE);

	nvmeib_io_stats_update_one(stats, NULL, IO_STAT_VERB_WRITE, upd_counters);
	nvmeib_iostats_sum_to_string(stats, 1, 1, &txt);
	BUG_ON(nvmeib_txt_finalize(&txt).len >= sizeof(buf));
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_WRITE, 0 /* All sizes */, &counters);
	BUG_ON(counters.total_ops != upd_counters.total_ops);

	//read from single bin
	counters.total_ops = 0;
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_WRITE, nlbas << NVMEIBC_SECTOR_SHIFT, &counters);
	BUG_ON(counters.total_ops != upd_counters.total_ops);

	//read from other bin
	counters.total_ops = 0;
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_WRITE, (8 * nlbas) << NVMEIBC_SECTOR_SHIFT, &counters);
	BUG_ON(counters.total_ops != 0);

	//read from other verb
	counters.total_ops = 0;
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_READ, 0 /* All sizes */, &counters);
	BUG_ON(counters.total_ops != 0);

	nvmeib_io_stats_free(stats);

	stats = nvmeib_io_stats_create_traced("testing_123", VERB_RW_T_BITMASK, NVMEIBC_SECTOR_SIZE);
	BUG_ON(!stats);
	nvmeib_io_stats_operation_start(stats, IO_STAT_VERB_WRITE, nlbas << NVMEIBC_SECTOR_SHIFT);
	BUG_ON(counters.inflight_ops != 0);
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_WRITE, 0 /* All sizes */, &counters);
	BUG_ON(counters.inflight_ops != 1);
	nvmeib_io_stats_operation_end(stats, IO_STAT_VERB_WRITE, nlbas << NVMEIBC_SECTOR_SHIFT);
	counters.inflight_ops = 0;
	nvmeib_io_stats_readc(stats, IO_STAT_VERB_WRITE, nlbas << NVMEIBC_SECTOR_SHIFT, &counters);
	BUG_ON(counters.inflight_ops != 0);
	nvmeib_io_stats_free(stats);
	return 0;
}

TEST_FUNC int unitest_txbm_compression(__attribute__((__unused__)) struct NVMeshSystem *sys) {
	int rv = 0, orig_ver = nvmeibc_jmd_wr_version;
	u32 start, end, slice_size;
	nvmeibc_jmd_wr_version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;
	for (slice_size = 3; slice_size <= N_MAX_RAID_SLICE_LEN; slice_size++) {
		for (start = 0; start < slice_size; start++) {
			for (end = start; end < slice_size; end++) {
				roles_bmp_t txbm = GENMASK(end, start);
				txbm_compressed_t txbm_compressed = nvmeibc_block_dp_ec_md_txbm_compress(txbm);
				roles_bmp_t txbm_decompressed = nvmeibc_block_dp_ec_md_txbm_decompress(txbm_compressed);

				if (txbm_decompressed != txbm) {
					BUG();
					rv = 1;
				}
			}
		}
	}

	nvmeibc_jmd_wr_version = orig_ver;
	return rv;
}

TEST_FUNC int unitest_update_targets_nics_clone(__attribute__((__unused__)) struct NVMeshSystem *sys) {
	struct nvmeibc_nic_conf nics[2] = {
		{.pkey=1, .protocol=1},{.pkey=2, .protocol=2}
	};

	struct nvmeibc_target_conf target_conf = {
		.nicsVersion=1, .n_nics=2, .nics=nics
	};

	struct nvmeib_mgmt_to_client_update_targets_nics update = {
		.n_targets=1, .targets = &target_conf
	};

	struct nvmeib_mgmt_to_client_update_targets_nics* update_clone
		= nvmeib_mgmt_to_client_update_targets_nics_clone(&update);

	if(!update_clone){
		return -ENOMEM;
	}

	BUG_ON(update_clone->n_targets != update.n_targets);
	BUG_ON(update_clone->targets[0].disks != NULL);
	BUG_ON(update_clone->targets[0].n_disks != update.targets[0].n_disks);
	BUG_ON(update_clone->targets[0].nics == NULL);
	BUG_ON(update_clone->targets[0].n_nics != update.targets[0].n_nics);
	BUG_ON(update_clone->targets[0].nics[0].pkey != update.targets[0].nics[0].pkey);
	BUG_ON(update_clone->targets[0].nics[1].protocol != update.targets[0].nics[1].protocol);
	nvmeibc_cc_api_free_update_targets_nics(update_clone);
	return 0;
}

TEST_FUNC int unitest_MultiClient(struct NVMeshSystem *sys) {
	int vol_idx = 0; /* Use volume 0. Because why not. */
	struct clientSimulator *client0 = &sys->clients[0];								// First 1 - we already have
	struct clientSimulator *client1 = &sys->clients[NVMeshSystem_new_client_add(sys)];	// Second 1 - we make
	struct test_context env0 = {.sys = sys, .client = client0, .dev = client0->devs[0], .sraid = NVMeshSystem_TstPRaid_init_rel(sys, (struct volume_segment_index){vol_idx, 0, 0, 0})};
	struct test_context env1 = {.sys = sys, .client = client1, .dev = client1->devs[0], .sraid = NVMeshSystem_TstPRaid_init_rel(sys, (struct volume_segment_index){vol_idx, 0, 0, 0})};

	/* This is for IOs */
	const size_t mem_size = NVMEIBC_SECTOR_SIZE; /* All IOs in this test will be 1 block exactly */
	u8* mem = sim_kmalloc(mem_size, GFP_KERNEL); /* Allocate mem */
	u64 magic = __unitest_fill_blocks_unique_pattern(mem, 1); /* Fill magic pattern */

	/* Do 1 write and verify via a different client */
	REPORT_ERROR(osSimulator_writeArrWait(&env0.client->OS, vol_idx, 0, 1, mem));
	memset(mem, 0, mem_size);
	REPORT_ERROR(osSimulator_readArrWait( &env1.client->OS, vol_idx, 0, 1, mem));
	__unitest_verify_blocks_pattern(mem, 1, magic, false);

	/* Cleanup */
	sim_kfree(mem);
	NVMeshSystem_new_client_rmv(sys, client1->inst_id);
	return 0;
}

struct ka_cb_data {
	pthread_mutex_t m;
	pthread_cond_t 	s;
	int flag;
};

static void signal_ka_msg(void *_ctx)
{
	struct ka_cb_data *ctx = _ctx;
	pthread_mutex_lock(&ctx->m);
	ctx->flag = 1;
    pthread_cond_signal(&ctx->s);
    pthread_mutex_unlock(&ctx->m);
}

static int64_t timeval_abs_diff(struct timeval* curr, struct timeval* past)
{
	struct timeval res = {.tv_sec=0, .tv_usec=0};
	timersub(curr, past, &res);
	return (int64_t)res.tv_sec * 1000000ll + res.tv_usec;
}

static int64_t calc_time_passed(struct timeval* past)
{
	struct timeval curr = {0,0};
	gettimeofday(&curr, NULL);
	return timeval_abs_diff(&curr, past);
}

static void __test_ka_interval(struct mcs_simu *mcs)
{
	struct ka_cb_data ctx = {.m = PTHREAD_MUTEX_INITIALIZER, .s = PTHREAD_COND_INITIALIZER, .flag = 0};
	struct timeval start;
	int64_t usec_time_taken;

	// allow 80 ms error margin
	const int64_t max_interval_drift_us = 1008000ll; // keepalive acts in 1 second resolution.

	int64_t max_interval_us = (int64_t)mcs->expected_keepalive_interval * 1000000ll + max_interval_drift_us;
	gettimeofday(&start, NULL);

	mgmt_set_ka_callback(mcs, signal_ka_msg, &ctx);

	pthread_mutex_lock(&ctx.m);
	while (ctx.flag == 0) {
		pthread_cond_wait(&ctx.s, &ctx.m);
	}
	pthread_mutex_unlock(&ctx.m);

	usec_time_taken = calc_time_passed(&start);
	mgmt_clear_ka_callback(mcs);
	BUG_ON(usec_time_taken > max_interval_us);
}

static void test_ka_interval(struct mcs_simu *mcs, int iterations) {
	int i;
	if (!ut_conf__get_base()->is_valgrind && !is_debugger_present()) {
		// Break points make this unpredictable
		for (i = 0; i < iterations; i++) {
			__test_ka_interval(mcs);
		}
	}
}

TEST_FUNC int unitest_client_update_targets_nics(struct NVMeshSystem *sys) {
	struct clientSimulator *client 	= &sys->clients[0];	//the single client in the system
	struct mcs_simu *mcs = &sys->mgmt.mcs[client->inst_id];
	BUG_ON(!generate_mcs_fake_update_targets_nics(mcs));
	return 0;
}

TEST_FUNC int unitest_client_counters(struct NVMeshSystem *sys) {
	struct clientSimulator *client 	= &sys->clients[0];	//the single client in the system
	struct mcs_simu *mcs = &sys->mgmt.mcs[client->inst_id];
	long long sequence_num = -1, reportID = -1, client_token = -1;
        static const unsigned long long high_keep_alive_freq = 30; // 30 seconds

	//Make sure to fail the message if it has an invalid message type version
	BUG_ON(!generate_mcs_update_token(mcs, sequence_num, reportID, client_token, 1, NVMEIB_MCS_MSG_WITH_BAD_MESSAGE_VER));

	//Since the message type version was bad, a MCS_GET_CLIENT_CONFIGURATION_MSG_MSG is sent upwards, make sure it's handled before proceeding
	NVMeshSystem_serialize(sys);

	//Set counter values to default - keepaliveInterval should be set to DEFAULT_MANAGEMENT_REPORT_FREQUENCY (5 seconds)
	mcs_set_expected_counters(mcs, sequence_num, reportID, client_token, DEFAULT_MANAGEMENT_REPORT_FREQUENCY);
	BUG_ON(generate_mcs_update_token(mcs, sequence_num, reportID, client_token, 0, NVMEIB_MCS_MSG_WITH_NO_ERROR) <= 0);

	//Consume the immediate keepalive
	NVMeshSystem_serialize(sys);

	//Whenever updating counter, sequence num / report id are what mgmt currently has, so the next time they are sent,
	// they should be increased by the client when they are used to send something upstream.
	sequence_num = 10;
	reportID = 20;
	//The above is not the case with client_token - it's only updated by mgmt and the value is always overwritten (even if locally it's larger)
	//The keepalive interal is capped at MAXIMAL_MANAGEMENT_REPORT_FREQUENCY (currently 30 seconds)
	client_token = 30;
	mcs_set_expected_counters(mcs, sequence_num + 1, reportID, client_token, high_keep_alive_freq);
	BUG_ON(generate_mcs_update_token(mcs, sequence_num, reportID, client_token, high_keep_alive_freq, NVMEIB_MCS_MSG_WITH_NO_ERROR) <= 0);

	//Consume the immediate keepalive
	NVMeshSystem_serialize(sys);

	sequence_num = 100;
	//Keepalive interval should remain MAXIMAL_MANAGEMENT_REPORT_FREQUENCY
	mcs_set_expected_counters(mcs, sequence_num + 1, 0, 0, DEFAULT_MANAGEMENT_REPORT_FREQUENCY);
	//sequence/report id counters will only be updated if the value passed in the message is larger than that in RAM
	BUG_ON(generate_mcs_update_token(mcs, sequence_num, 1, 2, DEFAULT_MANAGEMENT_REPORT_FREQUENCY, NVMEIB_MCS_MSG_WITH_NO_ERROR) <= 0);

	//Sending a lower report id (2 instead of 20) will cause a report to be sent, make sure there are no more messages on the WQ before
	// + Consume the immediate keepalive and client report
	NVMeshSystem_serialize(sys);

	reportID = 100;
	client_token = client_token - 1; // Decrease the client token to make sure it is *not* updated
	mcs_set_expected_counters(mcs, 0, reportID, client_token, 1);
	BUG_ON(generate_mcs_update_token(mcs, 1, reportID, client_token, 1, NVMEIB_MCS_MSG_WITH_NO_ERROR) <= 0);

	//Consume the immediate keepalive
	NVMeshSystem_serialize(sys);

	//Make sure the ka interval is what was requested
	test_ka_interval(mcs, 3);

	//increase the timeout to 5 seconds, from the moment we change the timeout till first  ka we expect wait about 5 secs.we
	client_token = 200; // increase client token and expect to be updated
	mcs_set_expected_counters(mcs, 0, reportID, client_token, DEFAULT_MANAGEMENT_REPORT_FREQUENCY);
	BUG_ON(generate_mcs_update_token(mcs, sequence_num, 1, client_token, DEFAULT_MANAGEMENT_REPORT_FREQUENCY, NVMEIB_MCS_MSG_WITH_NO_ERROR) <= 0);
	NVMeshSystem_serialize(sys);
	test_ka_interval(mcs, 1);

	NVMeshSystem_serialize(sys);

	client_token = 300;
	mcs_set_expected_counters(mcs, 0, 0, client_token, DEFAULT_MANAGEMENT_REPORT_FREQUENCY);
	BUG_ON(generate_mcs_update_token(mcs, 1, 2, client_token, DEFAULT_MANAGEMENT_REPORT_FREQUENCY, NVMEIB_MCS_MSG_WITH_NO_ERROR) <= 0);

	//Consume the immediate keepalive
	NVMeshSystem_serialize(sys);

	BUG_ON(!NVMeshSystem_is_stable(sys));
	return 0;
}

extern const struct nvmesh_memmgr_metrics *unitest_get_memmgr_metric_client_total_mem(void);
extern const struct nvmesh_memmgr_metrics *unitest_get_memmgr_metric_simulator_total_mem(void);

struct nvmesh_memmgr_metric_total_allocations {
	struct nvmesh_memmgr_metric_counters simulator;
	struct nvmesh_memmgr_metric_counters client_total;
	struct nvmesh_memmgr_metric_counters client_audited;
};

void nvmesh_memmgr_total_allocations_update(struct nvmesh_memmgr_metric_total_allocations *self,
					    struct nvmesh_memmgr_metrics const *mgr)
{
	struct nvmesh_memmgr_metric_counters merged_cpus = nvmesh_memmgr_metrics_merge_cpus(mgr);

	if (mgr == unitest_get_memmgr_metric_simulator_total_mem())
		nvmesh_memmgr_metric_counters_merge(&self->simulator, &merged_cpus);
	else if (mgr == unitest_get_memmgr_metric_client_total_mem())
		nvmesh_memmgr_metric_counters_merge(&self->client_total, &merged_cpus);
	else
		nvmesh_memmgr_metric_counters_merge(&self->client_audited, &merged_cpus);
}

#define MEMMGR_METRIC_ACCOUNTING_THRESHOLD 75

static void unitest_memmgr_metric_check_client(void)
{
	struct nvmesh_memmgr_metric_total_allocations total_allocations = {0};
	struct nvmesh_metric_bytes_histogram client_non_accounted;
	uint64_t client_non_accounted_allocation;
	uint64_t client_accounted_allocation;
	uint64_t client_total_allocation;
	float accounted_pct, non_accounted_pct;
	struct nvmesh_memmgr_metrics *curr;

	/* step 1 - compute the total allocations */
	for (curr = __start_nvmeibc_memmgr_metrics; curr < __stop_nvmeibc_memmgr_metrics; curr++)
		nvmesh_memmgr_total_allocations_update(&total_allocations, curr);

	/* step 2 - compute the per-bin delta between the sum of the accounting points to the client total mem allocation measures */
	nvmesh_metric_bytes_histograms_subtract(&client_non_accounted,
						&total_allocations.client_total.allocation_distribution,
						&total_allocations.client_audited.allocation_distribution);

	/* step 3 - compute the overall delta */
	client_total_allocation = nvmesh_metric_bytes_histogram_total_allocations(&total_allocations.client_total.allocation_distribution);
	client_accounted_allocation = nvmesh_metric_bytes_histogram_total_allocations(&total_allocations.client_audited.allocation_distribution);
	client_non_accounted_allocation = nvmesh_metric_bytes_histogram_total_allocations(&client_non_accounted);

	accounted_pct = ((float)client_accounted_allocation/(float)client_total_allocation)*100;
	non_accounted_pct = ((float)client_non_accounted_allocation/(float)client_total_allocation)*100;
	_NI(mat, "memmgr allocation tracking: client total @INT GB, accounted @INT%%, non-accounted @INT%%\n",
	    (int)((float)client_total_allocation/1e9), (int)accounted_pct, (int)non_accounted_pct);

#if 0
{
	uint64_t idx;
	printf("memmgr allocation tracking: client total %.1fGB, accounted %.1fGB (%.1f%%), non-accounted %.1fGB (%.1f%%)\n",
		(float)client_total_allocation/1e9,
		(float)client_accounted_allocation/1e9, accounted_pct,
		(float)client_non_accounted_allocation/1e9, non_accounted_pct);

	printf("non-accounted bin distribution:\n");
	printf("%-12s %-10s %-15s\n", "alloc order", "val", "volume(bytes)");
	for (idx = 0; idx < ARRAY_SIZE(client_non_accounted.bins); idx++)
		printf("%-12ld %-10ld %-15ld\n", idx+NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT, client_non_accounted.bins[idx],
				(1UL << (idx+NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT)) * client_non_accounted.bins[idx]);
	fflush(stdout);
}
#endif

	BUILD_BUG_ON(MEMMGR_METRIC_ACCOUNTING_THRESHOLD >= 100);
	BUG_ON(non_accounted_pct > (100 - MEMMGR_METRIC_ACCOUNTING_THRESHOLD));
}

/* execute all tests from a kthread !!! */
static bunitest_s * __alloc_bunny(void) { return sim_kmalloc(sizeof(bunitest_s), GFP_KERNEL); };
static int blk_unit_test(void *param __attribute__((unused))) {
	bunitest_s *buni = __alloc_bunny();
	int sr, rv = 0;
	struct NVMeshSystem *sys = &buni->_sys;

	ut_os_id = pthread_self();

	bunitest_s_create(buni);
	if (!buni->conf->bunitest.disableSimulatorTests) {
		void test_kernel_infra(struct kernel_sim *);
		test_kernel_infra(sys->clients[0].OS.kernel);                            // do infrastructure tests
		sim_kfree(buni);
		return 0;
	}

	test_metrics();
	test_wq_metrics();
	test_memmgr_metrics();
	test_error_tags();
	kr_incs_bit_ops_tests();
	nvmeib_scatterlist_iter_tests();
	nvmeibc_management_capi_parse_conf_tests();

	if (unlikely(buni->conf->bunitest.nRep == 0))
		unitest_print("*** Skipping all unitests. Intentional?\n");

	for (sr=0; sr < buni->conf->bunitest.nRep; sr++) {								// Do many rounds to test stability
		if (buni->conf->bunitest.half_sync_async_mode) {
			const bool sync_mode = (sr*2 < buni->conf->bunitest.nRep);				// Important, start with sync == true, so in case someone puts half_sync as default, the first iteration is still in sync mode.
			unitest_trace_note(sync_mode_set_to, "@BOOL", sync_mode);
			ut_conf__platform_io_sync_set(sync_mode);
		}
		rv |= NVMeshSystem_init(sys);
		clientSimulator_print_proc_file_by_path(&sys->clients[0], "/proc/nvmeibc/cflags");	// Print compilation flags
		NVMeshSystem_precondition_all_disks_for_R1(sys);
		NVMeshSystem_precondition_mirror_md(sys);
		buni->test_phase = BUNI_VOLUMES_TESTING;
		nvmeibc_sync_full_lockset_probability_factor = NVMEIBC_SYNC_PROB_FORCE_LOCKSET; // force sync of whole LOCKSET
		force_reconf_reboot = true;													// In unitest allow testing of hot/warm reconfigurations, even if in real system this is disabled

		if (!buni->conf->bunitest.disableActTests) {
			bunitest_tic(buni);
			rv |= SIMU_RUN_TEST(unitest_delayed_work, sys);
			rv |= SIMU_RUN_TEST(test_basic_percpu, sys);
			rv |= SIMU_RUN_TEST(unitest_io_stats, sys);
			rv |= SIMU_RUN_TEST(unitest_txbm_compression, sys);
			rv |= SIMU_RUN_TEST(unitest_update_targets_nics_clone, sys);
			rv |= SIMU_RUN_TEST(unitest_client_update_targets_nics, sys);
			rv |= SIMU_RUN_TEST(unitest_client_counters, sys);
			rv |= SIMU_RUN_TEST(unitest_volumes_config, sys);
			rv |= SIMU_RUN_TEST(unitest_IllegalIO, sys);
			rv |= SIMU_RUN_TEST(unitest_MultiClient, sys);
			rv |= SIMU_RUN_TEST(unitest_VolumeReservation, sys);
			rv |= SIMU_RUN_TEST(unitest_segment_relocation, sys);
			rv |= SIMU_RUN_TEST(unitest_PauseContDisk_noIO, sys);
			rv |= SIMU_RUN_TEST(unitest_segment_relocation_multi, sys);
			rv |= SIMU_RUN_TEST(unitest_downgrade_upgrade_raid, sys);
			rv |= SIMU_RUN_TEST(unitest_expand_shrink_volume_by_1_chunk, sys);
			rv |= SIMU_RUN_TEST(unitest_hot_multichunk_resize, sys);
			rv |= SIMU_RUN_TEST(unitest_missing_few_reconfs, sys);
			rv |= SIMU_RUN_TEST(unitest_IOonRaid10_without_metadata, sys);
			rv |= SIMU_RUN_TEST(unitest_GoodPathIO, sys);
			rv |= SIMU_RUN_TEST(unitest_SubBlockIO, sys, false);
			rv |= SIMU_RUN_TEST(unitest_MetadataGoodPathIO, sys);
			rv |= SIMU_RUN_TEST(unitest_IOonReadOnlyVols, sys);
			rv |= SIMU_RUN_TEST(unitest_LocalReadOptimization, sys);
			rv |= SIMU_RUN_TEST(unitest_FailureOfLocksOnVol0, sys);
			rv |= SIMU_RUN_TEST(unitest_DecentralizedUnregister, sys);
			rv |= SIMU_RUN_TEST(unitest_Raid1_SwitchTopo, sys);
			rv |= SIMU_RUN_TEST(unitest_DegradedMode, sys);
			//rv |= SIMU_RUN_TEST(unitest_verify_dead_to_write_hot_transition, sys);

			unitest_print("*** Raid1+0 basics - %s\n", unitest_rv_to_string(rv));
			rv |= SIMU_RUN_TEST_ID(unitest_read_mutable_buffer, mirror, sys, 0);
			rv |= SIMU_RUN_TEST(unitest_scrubRecovery_R1, buni);
			NVMeshSystem_all_clients_mirror_edic(sys, false);	// Async tests will create corruptions, hence edic check will often fail, while not indicating correctness issue. Disable edic check.
			rv |= SIMU_RUN_TEST(unitest_R1_recovery_Basic, buni);
			rv |= SIMU_RUN_TEST(unitest_async_degraded_mode_rebuild_during_io, sys);
			rv |= SIMU_RUN_TEST(unitest_async_suspend_revive_during_rebuild, sys);
			rv |= SIMU_RUN_TEST(unitest_async_attach_detach_during_rebuild, sys);
			unitest_print("*** Raid1+0 Recovery - %s\n", unitest_rv_to_string(rv));
			rv |= SIMU_RUN_TEST(unitest_multi_sync_recov, buni);
			rv |= SIMU_RUN_TEST_ID(unitest_resubmitIO, raid_10, sys);
			rv |= SIMU_RUN_TEST(unitest_resubmitIO_continousWrites, sys);
			rv |= SIMU_RUN_TEST(unitest_percpu_io_throttle_queues, sys);
			rv |= SIMU_RUN_TEST(unitest_SyncStaleLocks, buni);
			rv |= SIMU_RUN_TEST(unitest_PermanentReadError, sys);
			rv |= SIMU_RUN_TEST(unitest_NVMeDriveFailure, sys);
			rv |= SIMU_RUN_TEST(unitest_Registrable, sys);
			rv |= SIMU_RUN_TEST(unitest_RetryLocksTrimSplit, buni);
			rv |= SIMU_RUN_TEST(unitest_RetryLocksInDifferentLockModes, sys);
			rv |= SIMU_RUN_TEST(unitest_async_suspend_revive_during_io, sys);
			rv |= SIMU_RUN_TEST(unitest_async_attach_dettach_during_toma_messages, buni, false /* Impossible in the current implementation of toma simulator */ );
			rv |= SIMU_RUN_TEST(unitest_async_pause_cont_during_io_regu_lock_mode, sys);
			rv |= SIMU_RUN_TEST(unitest_async_pause_cont_during_io_dual_lock_mode, sys);
			rv |= SIMU_RUN_TEST(unitest_async_pause_cont_during_io_unsafe_lock_mode, sys);
			if (0) rv |= SIMU_RUN_TEST(unitest_async_pause_cont_during_attach_dettach, buni);
			if (0) rv |= SIMU_RUN_TEST(unitest_async_double_pause_cont_during_io_regu_lock_mode, sys);
			rv |= SIMU_RUN_TEST(unitest_async_pause_cont_during_resize, sys, false /* due to bugs */);
			rv |= SIMU_RUN_TEST(unitest_async_unsafe_detach_attach_during_io, sys);
			rv |= __resize_vol0(sys, "expand_to24[mb]", false);
			rv |= SIMU_RUN_TEST(unitest_async_trim_interlocking_vol0, sys);
			rv |= __resize_vol0(sys, "shrink_back", false);

			NVMeshSystem_all_clients_mirror_edic(sys, true); // Return edic check
			NVMeshSystem_wipe_all_md_of_disks(sys); // Cleanup
			unitest_print("*** Raid1+0 extended - %s, %d[mSec]\n", unitest_rv_to_string(rv), bunitest_toc(buni));
		}
		bunitest_phase_stack_do(buni, "Push", BUNI_ERASURE_CODING_TESTING);
		rv |= unitest_n_mirror(sys, "u_upgrade_to_4_3_mirror", UNITEST_UPDOWNGRADE_COLD);
		if (!buni->conf->bunitest.disableErasureTests) {
			struct nvmeibc_locks_scheme_conf *locks = &sys->mdb.vols[0].locks_scheme;
			qa_ec_stress_debug = !true;			// Put additional stress on EC
			if (NVMEIBC_SECTOR_SHIFT == 12) {	// raid 50 operational only with 4KB sector sizes
				if (1) {
					rv |= unitest_raid_ec_transform(sys, "to_raid_50");
					rv |= SIMU_RUN_TEST_ID(unitest_GoodPathIO_raid50_or_60, raid_50, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_DoubleDegradedMode_EC, raid_50, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_DegradedMode_EC, raid_50, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_GoodPathIO_block_md_illegal_splits, raid_50, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_io_perm_alert, raid_50, sys);
					rv |= unitest_raid_ec_transform(sys, "back_to_mirror");
					clientSimulator_dump_procfs_to_disk(&sys->clients[0], buni->conf->bunitest.procfs_dump_path);
					rv |= unitest_raid_ec_transform(sys, "to_raid_60");
					rv |= SIMU_RUN_TEST_ID(unitest_GoodPathIO_raid50_or_60, raid_60, buni);
					rv |= SIMU_RUN_TEST(unitest_ec_view_lock, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_DegradedMode_EC, raid_60, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_GoodPathIO_block_md_illegal_splits, raid_60, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_io_perm_alert, raid_60, sys);
					rv |= SIMU_RUN_TEST_ID(unitest_DoubleDegradedMode_EC, raid_60, buni);
					rv |= unitest_raid_ec_transform(sys, "back_to_mirror");
				}
				rv |= unitest_raid_ec_transform(sys, "raid6_8plus2");
				if (1) {
					rv |= SIMU_RUN_TEST_ID(unitest_GoodPathIO_raid50_or_60, 8_plus_2, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_resubmitIO, 8_plus_2, sys);
					rv |= SIMU_RUN_TEST_ID(unitest_EC_GoodPath_binje8, 8_plus_2, buni);
				}
				if (!buni->conf->bunitest.disableEC_seg_reloc_test) { //our simulator sgmnt relocation is full of tricks and shortcuts. to simplify jam/serjio recovery temporally use atomic weapon
					rv |= SIMU_RUN_TEST(unitest_segment_relocation_ec, sys);
					rv |= unitest_raid_ec_transform(sys, "back_to_mirror");
					rv |= unitest_raid_ec_transform(sys, "raid6_8plus2");
				}
				if (1) {
					NVMeshSystem_all_clients_read_has_mutable_bio_buffers(sys, (1+sr)%3);
					rv |= SIMU_RUN_TEST_ID(unitest_io_perm_alert, 8_plus_2, sys);
					rv |= SIMU_RUN_TEST_ID(unitest_read_mutable_buffer, 8_plus_2, sys, 0);
					rv |= SIMU_RUN_TEST_ID(unitest_DoubleDegradedMode_EC, 8_plus_2, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_SubBlockIO, 8_plus_2, sys, true);
				}
				if (1) {	//tx framework does not keep JAM & Serjio synced
					rv |= unitest_raid_ec_transform(sys, "back_to_mirror");
					rv |= unitest_raid_ec_transform(sys, "raid6_8plus2");
					rv |= SIMU_RUN_TEST(unitest_jam_AllTests, sys);
					rv |= SIMU_RUN_TEST(unitest_serjio_AllTests, sys);
				}
				if (!buni->conf->bunitest.disableEC_8plus2_exhastiveTests) {
					NVMeshSystem_all_clients_read_has_mutable_bio_buffers(sys, (1+sr)%3);
					rv |= SIMU_RUN_TEST(unitest_PermanentReadError_EC, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_DegradedMode_EC, 8_plus_2, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_EC_8127, 8_plus_2, buni);
				}
				if (1) {	// Basic TOMA driven recovery
					unitest_print("*** EC Raid 8+2 Basic Recovery tests\n");
					NVMeshSystem_all_clients_read_has_mutable_bio_buffers(sys, (1+sr)%3);
					rv |= SIMU_RUN_TEST(unitest_EC_recovery_Basic, buni);
					rv |= SIMU_RUN_TEST(unitest_ECHotJgcRecovery, buni);
					rv |= SIMU_RUN_TEST(unitest_scrubRecovery_ec, buni);
				}
				unitest_print("*** EC Raid 8+2 Tx-framework tests, binje=%d\n", nvmeibc_jentry_num_blocks);
				qa_ec_stress_debug = false;					// HTR/Cold/JGC dont expect TXID-wraparounds
				if (1) {
					NVMeshSystem_all_clients_read_has_mutable_bio_buffers(sys, (1+sr)%3);
					rv |= SIMU_RUN_TEST(unitest_ECJgcRecovery, buni);
					rv |= SIMU_RUN_TEST(unitest_ECHotRecovery, buni);
					rv |= SIMU_RUN_TEST(unitest_ECColdRecovery, buni);
				}
				rv |= SIMU_RUN_TEST(unitest_ECDbitsOnDisk, buni);
				rv |= unitest_raid_ec_transform(sys, "back_to_mirror");
				rv |= unitest_raid_ec_transform(sys, "raid6_8plus2");
				if (!buni->conf->bunitest.disableEC_async_pause_cont) {
					send_command_to_vols(sys, -1, volCmds_Detach, sys->mdb.nVols - 1, &sys->mdb.vols[1]);
					sys->tcf.nVolumes = sys->clients[0].nBdevs = 1;
					if (1) rv |= SIMU_RUN_TEST_ID(unitest_EC_single_slice_async_pause_disks, no_errro, sys, false);
					if (1) rv |= SIMU_RUN_TEST_ID(unitest_EC_single_slice_async_pause_disks, with_error, sys, true);
					if (0) rv |= SIMU_RUN_TEST(unitest_EC_async_degraded_mode_rebuild_during_single_slice_io, sys);
					sys->tcf.nVolumes = sys->clients[0].nBdevs = MAX_NORMAL_VOLUMES_IN_NVMESH;
					send_command_to_vols(sys, -1, volCmds_New, sys->mdb.nVols - 1, &sys->mdb.vols[1]);
				}
				rv = unitest_raid_ec_transform(sys, "back_to_mirror");
				unitest_print("*** ErasureCoding Raid60 (locks=%d:%s) - %s\n", locks->maxNOwners, __lock_server_type_to_string(locks->type), unitest_rv_to_string(rv));
			}
		}
		if (!buni->conf->bunitest.disableNreplicaTests) {
			struct nvmeibc_locks_scheme_conf *locks = &sys->mdb.vols[0].locks_scheme, backup_locks = *locks;
			bunitest_tic(buni);
			BUG_ON(nvmeibc_debug_ram_binfo != true);	// This test should not create any binfo corruption
			for (locks->type = OWNER_SCHEME_SL_START_DEC_C; locks->type <= OWNER_SCHEME_SL_START_DEC_C; locks->type++) {
				for (locks->maxNOwners = 2; locks->maxNOwners <= N_MAX_RAID_LOCKS; locks->maxNOwners++) {
					rv |= unitest_n_mirror(sys, "reattach_to_change_lock_server", UNITEST_UPDOWNGRADE_COLD);
					rv |= SIMU_RUN_TEST(unitest_GoodPathLockServer_n_mirrored, sys);
					rv |= SIMU_RUN_TEST(unitest_GoodPathIO_n_mirrored, sys);
					if (locks->maxNOwners==4)		//		Todo: Fix me, with less locks there are no 0 dbits visible so merge of locks yields unknowns
						rv |= SIMU_RUN_TEST(unitest_DegradedMode_n_mirrored, sys);
					// Todo: also unitest_DegradedMode()
					if (locks->maxNOwners>2)
						continue;										// Existing bugs, Daniel: Fix this
					if (0) rv |= SIMU_RUN_TEST_ID(unitest_SyncStaleLocks, n_replica, buni);				// Very slow unitest
					rv |= SIMU_RUN_TEST_ID(unitest_RetryLocksTrimSplit, n_replica, buni);
					rv |= SIMU_RUN_TEST_ID(unitest_RetryLocksInDifferentLockModes, n_replica, sys);
					//unitest_print("*** N-replica (l=%d) - %s\n", locks->max_n_owners, unitest_rv_to_string(rv));
				}
			}
			*locks = backup_locks;
			unitest_print("*** raid1-N-replica (locks=[%d..%d]:%s) - %s, %d[mSec]\n", 2, N_MAX_RAID_LOCKS, "3 types", unitest_rv_to_string(rv), bunitest_toc(buni));
		}
		rv |= unitest_n_mirror(sys, "d_udowngrade_to_2_mirror", UNITEST_UPDOWNGRADE_COLD);

		bunitest_phase_stack_do(buni, "Pop", 0);
		if (!buni->conf->bunitest.disableCmpBlocks)
			rv |= unitest_cmp_blocks_validation();

		clientSimulator_print_proc_dir(sys->clients, buni->conf->bunitest.do_cat_proc_before_destroy);		// Test print output to /proc (only once in the normal tests. No need to test it each time client is removed
		rv |= NVMeshSystem_destroy(sys);
		buni->test_phase = BUNI_ALL_DONE;
		if (!buni->conf->bunitest.disableClientRestartTests) {
			bunitest_phase_stack_do(buni, "Push", BUNI_CLIENT_REMOVAL_TESTING);
			rv = unitest_nvmesh_client_restart(sys);
			unitest_print("*** client_restart - %s\n", unitest_rv_to_string(rv));
			bunitest_phase_stack_do(buni, "Pop", 0);
		}
		if (buni->conf->bunitest.run_gf_sanity_tests) {
			unitest_print("*** gf simple sanity tests\n");
			rv |= gf_simple();
			unitest_print("*** gf extensive sanity tests\n");
			rv |= gf_extensive(); // Very long test (8 seconds)
			unitest_print("*** gf optimized sanity tests\n");
			rv |= gf_opt_check(); // Currently fails
			unitest_print("*** gf performance tests\n");
			rv |= gf_perf();
		}
		pr_alert("--------------------commit ID: %07lx, Round " KERN_COL_WHITE_BOLD "%3d" KERN_COL_RESET " of %d - %s--------------------\n", (unsigned long)COMMIT_ID, sr + 1, buni->conf->bunitest.nRep, unitest_rv_to_string(rv));
	}	// for (sr=0; sr < buni->conf->bunitest.nRep; sr++)
	sim_kfree(buni);
	rv |= nvmesh_error_tags_dump_to_file("error_tags_run_end.json", __start_nvmeibc_error_tags, __stop_nvmeibc_error_tags);
	return rv;
}

int main(int argc, char* argv[])
{
	const struct blk_unittest_conf *ut_conf;
	struct task_struct		*ut_kthread;

	simulator_alloc_tracking_init();

	/* we don't track allocations done by section_init */
	simulator_alloc_tracking_disable();
	nvmesh_memmgr_metrics_alloc_pcpu(__start_nvmeibs_memmgr_metrics, __stop_nvmeibs_memmgr_metrics);
	nvmesh_memmgr_metrics_alloc_pcpu(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	nvmeib_wq_metrics_alloc_pcpu(__start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics);
	simulator_alloc_tracking_enable();

	main_os_id  = pthread_self();							// Important! Emulate kernel thread for main(). Must be done before any to print using printk(). This is not part of kernel boot because parsing arguments requires using printk(), and we parse arguments before boot!
	set_up_other_thread_stack_dump_handler();				// Process-wide: allows request_other_thread_stack_dump() for any thread
	kthread_self_task = get_current();
	__start_all_trace_pollers();
	ut_conf = __parseArgs(argc, argv);
	debug_dump_funcs = ut_conf->base.debug_dump_funcs;
	if (ut_conf->bunitest.run_bin_traces_tests) {
		tracer_nvmeibc_debug_level = 6;
		nvmeibc_run_tracer_unitest();						// In case of tracer test mode discard any further actions, just run tracer test and exits
		exit(0);
	}
	__print_status_header(ut_conf);

	if (!ut_conf->bunitest.config_path) {
		unitest_global_cfg = NULL;
	} else BUG_ON(!(unitest_global_cfg = unitest_init_config(ut_conf->bunitest.config_path)));

	machine_restart(&ut_conf->kernel_prm);					// Create kernel simulator (as if we started VM)
	BUG_ON(!(ut_kthread = kthread_run(blk_unit_test, NULL, "blk unit test")));
	get_task_struct(ut_kthread);
	kthread_stop(ut_kthread);
	put_task_struct(ut_kthread);

	{	// Test integrity of the system
		const int nMemLeaks = kget_num_allocs();
		if (nMemLeaks!=0){
			_Emerg("MEMORY LEAKS: %d detected!!!!\n", nMemLeaks);
			kget_num_allocs_print();
			if (is_debugger_present())
				BUG_ON(nMemLeaks);	/* If no debugger (like valgrind) don't crash or else we break the unitest flow of the system */
		}
	}
	machine_power_off();									// Destroy kernel simulator (as if we closed VM)
	unitest_destroy_config(unitest_global_cfg);

	/* must be done after all simulator allocations are freed */
	nvmesh_memmgr_metrics_dump_to_file("memmgr_info_run_end.json", __start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	nvmesh_memmgr_metrics_verify_idle(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	unitest_memmgr_metric_check_client();
	simulator_alloc_tracking_disable();
	nvmeib_wq_metrics_free_pcpu(__start_nvmeibc_wq_metrics, __stop_nvmeibc_wq_metrics);
	nvmesh_memmgr_metrics_free_pcpu(__start_nvmeibc_memmgr_metrics, __stop_nvmeibc_memmgr_metrics);
	nvmesh_memmgr_metrics_free_pcpu(__start_nvmeibs_memmgr_metrics, __stop_nvmeibs_memmgr_metrics);
}
