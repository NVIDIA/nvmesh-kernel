#include "nvmeibc_capabilities.h"
#include "common/nvmeib_macro_magic.h"
#include "common/proc_epilog.h"

//$ strings ./blk_unitest | sed -n '/NVMEIBC.CAPABILITIES.YAML.BEGIN/, /NVMEIBC.CAPABILITIES.YAML.END/p' | grep -v NVMEIBC.CAPABILITIES.YAML > bkl_unitest.capabilities
//$ yq bkl_unitest.capabilities

#define NVMEIBC_CAPABILITIES_STRING \
"\nformat:\n" \
"  version: 1\n" \
"build:\n" \
"  is_production: " STRINGIFY(NVMESH_IS_PRODUCTION_COMPILATION) "\n" \
"  sector_shift: " STRINGIFY(NVMEIBC_SECTOR_SHIFT) "\n" \
"  take_stats: " STRINGIFY(TAKE_STATS) "\n"

const char capabilities[] = NVMEIBC_CAPABILITIES_STRING;

MODULE_INFO(nvmeibc_capabilities, NVMEIBC_CAPABILITIES_STRING);

const char* nvmeibc_get_capabilities(void)
{
	return capabilities;
}


#define CFLAGS_PROC_FRMT_VER 1
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
ssize_t nvmeibc_get_compile_flags(char *buffer, size_t len)
{
	int count = 0;
	// ---------------- First line: compilation flags (external params)
	count += scnprintf(buffer + count, len - count, "-DNVMEIBC_SECTOR_SHIFT=%u ", NVMEIBC_SECTOR_SHIFT);
	count += scnprintf(buffer + count, len - count, "-DNVMESH_IS_PRODUCTION_COMPILATION=%u ", NVMESH_IS_PRODUCTION_COMPILATION);
	#ifdef DEBUG_SUM
		count += scnprintf(buffer + count, len - count, "-DDEBUG_SUM ");
	#endif
	#ifdef DEBUG_TOPO_CNTRS
		count += scnprintf(buffer + count, len - count, "-DDEBUG_TOPO_CNTRS ");
	#endif
	#ifdef AUTONOMOUS_SYNCS_STATS
		count += scnprintf(buffer + count, len - count, "-DAUTONOMOUS_SYNCS_STATS ");
	#endif
	#ifdef DEBUG_TOMA_REG_LEAKS
			count += scnprintf(buffer + count, len - count, "-DDEBUG_TOMA_REG_LEAKS ");
	#endif
	#ifdef DEBUG_TRANSFERS
		count += scnprintf(buffer + count, len - count, "-DDEBUG_TRANSFERS ");
	#endif
	#ifdef DEBUG_LOCKS_CORRUPTION
		count += scnprintf(buffer + count, len - count, "-DDEBUG_LOCKS_CORRUPTION ");
	#endif
	#ifdef DEBUG_CONTENDED_LOCKS
		count += scnprintf(buffer + count, len - count, "-DDEBUG_CONTENDED_LOCKS ");
	#endif
	#ifdef DNVMEIBC_ENABLE_PER_VOLUME_STATS
		count += scnprintf(buffer + count, len - count, "-DNVMEIBC_ENABLE_PER_VOLUME_STATS ");
	#endif

	#ifdef DEBUG_PERCPU_ISSUED_IO_CNTRS
		count += scnprintf(buffer + count, len - count, "-DDEBUG_PERCPU_ISSUED_IO_CNTRS ");
	#endif
	#ifdef TRACE_CPUID
		count += scnprintf(buffer + count, len - count, "-DTRACE_CPUID ");
	#endif
	#ifdef DEBUG_LOCK_RETRY
		count += scnprintf(buffer + count, len - count, "-DDEBUG_LOCK_RETRY ");
	#endif
	#ifdef DEBUG_LOSER_CONDITIONS
		count += scnprintf(buffer + count, len - count, "-DDEBUG_LOSER_CONDITIONS ");
	#endif
	#ifdef CONFIG_NVMEIB_FINE
		count += scnprintf(buffer + count, len - count, "-DCONFIG_NVMEIB_FINE ");
	#endif
	#ifdef TAKE_STATS
		count += scnprintf(buffer + count, len - count, "-DTAKE_STATS ");
	#endif
	#ifdef BLKDEV_PROFILING
		count += scnprintf(buffer + count, len - count, "-DBLKDEV_PROFILING ");
	#endif
	#ifdef BLKDEV_SIMULATOR
		count += scnprintf(buffer + count, len - count, "-DBLKDEV_SIMULATOR=%u ", BLKDEV_SIMULATOR);
	#endif
	#ifdef ALLOW_SIMULATED_MD
		count += scnprintf(buffer + count, len - count, "-DALLOW_SIMULATED_MD ");
	#endif
	#ifdef CONFIG_NVMEIB_DEBUG
		count += scnprintf(buffer + count, len - count, "-DCONFIG_NVMEIB_DEBUG ");
	#endif
	#ifdef LOW_MEM
		count += scnprintf(buffer + count, len - count, "-DLOW_MEM ");
	#endif
	#ifdef DEBUG
		count += scnprintf(buffer + count, len - count, "-DDEBUG ");
	#endif
	#ifdef DEBUG_UNCOMPLETED
		count += scnprintf(buffer + count, len - count, "-DDEBUG_UNCOMPLETED ");
	#endif
	#ifdef _NVMEIB_TRACE_BACKEND_KERNEL
		count += scnprintf(buffer + count, len - count, "-D_NVMEIB_TRACE_BACKEND_KERNEL ");
	#endif
	#ifdef _NVMEIB_TRACE_BACKEND_USER
		count += scnprintf(buffer + count, len - count, "-D_NVMEIB_TRACE_BACKEND_USER ");
	#endif
	#ifdef _NVMEIB_TRACE_BACKEND_DMESG
		count += scnprintf(buffer + count, len - count, "-D_NVMEIB_TRACE_BACKEND_DMESG ");
	#endif
	#if defined(DBGDI_REMOVED_IN_PRODUCTION)
		count += scnprintf(buffer + count, len - count, "-DDBGDI_REMOVED_IN_PRODUCTION ");
	#endif
	#if defined(NVMEIBC_DISK_CMDS_STATS)
		count += scnprintf(buffer + count, len - count, "-DNVMEIBC_DISK_CMDS_STATS=%u\n", NVMEIBC_DISK_CMDS_STATS);
	#endif
	#if defined(NVMEIBC_DISK_CMDS_STATS_PROBES)
		count += scnprintf(buffer + count, len - count, "-DNVMEIBC_DISK_CMDS_STATS_PROBES=%u\n", NVMEIBC_DISK_CMDS_STATS_PROBES);
	#endif
	#if defined(DEBUG_REQ_REUSED_BB_STATE)
		count += scnprintf(buffer + count, len - count, "-DDEBUG_REQ_REUSED_BB_STATE=%u\n", DEBUG_REQ_REUSED_BB_STATE);
	#endif
	#if defined(NVMEIBC_LOCKS_CHANNEL_GUARD_STATE)
		count += scnprintf(buffer + count, len - count, "-DNVMEIBC_LOCKS_CHANNEL_GUARD_STATE=%u\n", NVMEIBC_LOCKS_CHANNEL_GUARD_STATE);
	#endif
	#if defined(NVMEIBC_READ_POISON_BB)
		count += scnprintf(buffer + count, len - count, "-DNVMEIBC_READ_POISON_BB=%u\n", NVMEIBC_READ_POISON_BB);
	#endif
	#if defined(NVMEIBC_READ_POISON_BB_PANIC)
		count += scnprintf(buffer + count, len - count, "-DNVMEIBC_READ_POISON_BB_PANIC=%u\n", NVMEIBC_READ_POISON_BB_PANIC);
	#endif
	#if defined(NVMEIB_DEBUG_RDMA_CORRUPTION)
		count += scnprintf(buffer + count, len - count, "-DNVMEIB_DEBUG_RDMA_CORRUPTION=%u\n", NVMEIB_DEBUG_RDMA_CORRUPTION);
	#endif
	#if defined(NVMEIB_DVLP_UNSAFE)
		count += scnprintf(buffer + count, len - count, "-DNVMEIB_DVLP_UNSAFE=%u\n", NVMEIB_DVLP_UNSAFE);
	#endif
		count += scnprintf(buffer + count, len - count, "-DEC_PERF_CLNT_NORDDA_REDUCE_SEND_COMPS=0\n");
	#if defined(EC_PERF_CLNT_NORDDA_SHARED_CQ)
		count += scnprintf(buffer + count, len - count, "-DEC_PERF_CLNT_NORDDA_SHARED_CQ=%u\n", EC_PERF_CLNT_NORDDA_SHARED_CQ);
	#endif
	// ---------------- Other line: calculated values, not params
	count += scnprintf(buffer + count, len - count, "-DNVMEIB_EC_JMDC_BITS_TX_ID=%u\n", NVMEIB_EC_JMDC_BITS_TX_ID);
	count += scnprintf(buffer + count, len - count, "-DNVMEIBC_DP_EC_DMD_BITS_JCI=%u\n", NVMEIBC_DP_EC_DMD_BITS_JCI);
	count += scnprintf(buffer + count, len - count, "-DNVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS=%u\n", NVMEIBC_DP_EC_MD_D2J_IN_JRANGE_BITS);
	count += scnprintf(buffer + count, len - count, "-DNVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT=%u\n", NVMEIB_EC_JOURNAL_MAX_BLKS_PER_RANGE_SHIFT);
	count += scnprintf(buffer + count, len - count, "-DN_MAX_RAID_SLICE_LEN=%u\n", N_MAX_RAID_SLICE_LEN);
	count += scnprintf(buffer + count, len - count, "-DNVMEIBC_DP_EC_DMD_BITS_VERSION=%u\n", NVMEIBC_DP_EC_DMD_BITS_VERSION);
	count += scnprintf(buffer + count, len - count, "-DNVMEIBC_DP_EC_DMD_BITS_EDIC=%u\n", NVMEIBC_DP_EC_DMD_BITS_EDIC);
	count += scnprintf(buffer + count, len - count, "-DKS_BVEC_ITER=%u\n", KS_BVEC_ITER);
	count += nvmeib_proc_add_txt_proc_epilog(CFLAGS_PROC_FRMT_VER, buffer + count, len - count);
	return count;
}

