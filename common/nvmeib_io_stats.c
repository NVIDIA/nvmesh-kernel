/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_io_stats.h"
#include "compat/kr_incs_types.h"
#include "nvmeib_types.h"
#include "nvmeib_utils.h"
#include "nvmeibm_trace.h"
#include "nvmeib_json.h"

/* Simulator does not support lockless per-cpu so we acquire/release the spinlock rather than just disabling irqs
 *
 * In the non-simulator case, we have to work around the fact that on_each_cpu does not pass the cpu to the function,
 * but instead it expects the function to call get_cpu/put_cpu
 * We get around this by creating an outer function in the DECLARE_IO_VERBS_ON_EACH_CPU_FN macro that
 * does get_cpu/put_cpu and then calls the inner function with the cpu
 *
 */

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)

#define io_verbs_lock_irqsave(iov, flags) spin_lock_irqsave(&iov->spinlock, flags)
#define io_verbs_unlock_irqrestore(iov, flags) spin_unlock_irqrestore(&iov->spinlock, flags)

#define DECLARE_IO_VERBS_ON_EACH_CPU_FN(fn_name, params) \
static void fn_name(int cpu, void *params)

#define io_verbs_on_each_cpu_check()

#define io_verbs_on_each_cpu_wait(iov, fn, params) do {\
	int cpu;\
	unsigned long flags;\
	io_verbs_lock_irqsave(iov, flags);\
	for_each_possible_cpu(cpu) {\
		fn(cpu, params);\
	}\
	io_verbs_unlock_irqrestore(iov, flags);\
} while(0)

#else
#define io_verbs_lock_irqsave(iov, flags) do {\
	(void)iov;\
	get_cpu();\
	local_irq_save(flags);\
} while(0)

#define io_verbs_unlock_irqrestore(iov, flags) do {\
	(void)iov;\
	local_irq_restore(flags);\
	put_cpu();\
} while(0)

#define DECLARE_IO_VERBS_ON_EACH_CPU_FN(fn_name, params) \
static void fn_name(int cpu, void *params);\
static void fn_name##_outer(void *p) {\
	int cpu = get_cpu();\
	fn_name(cpu, p);\
	put_cpu();\
}\
static void fn_name(int cpu, void *params)

#define io_verbs_on_each_cpu_check() BUG_ON(irqs_disabled() || in_interrupt())

#define io_verbs_on_each_cpu_wait(iov, fn, params) do {\
	(void)iov; \
	on_each_cpu(fn##_outer, params, true);\
} while(0)

#endif

#define MAX_IO_SIZES_HIST_BINS	32

#define CACHELINE_SIZE 64

static unsigned short io_sizes_hist[MAX_IO_SIZES_HIST_BINS] = {	/* Bins of histogram of io sizes */
	   1,    2,   4,    8,    16,   32,  64,  128
};
static unsigned int io_sizes_hist_n_bins = 8;

module_param_array_named(io_stats_sizes_hist, io_sizes_hist, ushort, &io_sizes_hist_n_bins, 0444);
MODULE_PARM_DESC(io_stats_sizes_hist, "Defines the buckets for the iostats histogram using an array.");

/* Look-up-table from io-size (in blocks) to histogram bin */
#define MAX_IO_SIZES_LUT_BLOCK_SIZE 256 // 128kb / 512b

static unsigned io_sizes_bin_lut[MAX_IO_SIZES_LUT_BLOCK_SIZE] = {};
static atomic_t io_sizes_bin_lut_init = ATOMIC_INIT(0);

static void init_io_sizes_bin_lut(void) {
	unsigned i, j;
	if (atomic_inc_return(&io_sizes_bin_lut_init) == 1) {
		for (i = 0; i < MAX_IO_SIZES_LUT_BLOCK_SIZE; i++) {
			for (j = 0; j < io_sizes_hist_n_bins; j++) {
				if (i <= io_sizes_hist[j]) {
					io_sizes_bin_lut[i] = j;
					break;
				}
			}
			/* We create 1 more bin than io_sizes_hist_n_bins for all sizes > io_sizes_hist[io_sizes_hist_n_bins - 1] */
			if (j == io_sizes_hist_n_bins) {
				io_sizes_bin_lut[i] = j;
			}
		}
	}
}

static const char* io_sizes_name(char *buf, size_t buf_size, unsigned bin, unsigned block_size)

{
	/* Last bin has name >[last_size_bin] */
	int is_last = (bin == io_sizes_hist_n_bins);
	unsigned bin_size = is_last ? io_sizes_hist[io_sizes_hist_n_bins - 1] : io_sizes_hist[bin];
	unsigned bin_size_bytes = bin_size * block_size;
	const unsigned _1M = 1024 * 1024;
	const unsigned _1k = 1024;
	size_t len;
	if (bin_size_bytes >= _1M)
		len = scnprintf(buf, buf_size, "%s%uM", is_last ? ">" : "", bin_size_bytes / _1M);
	else if (bin_size_bytes >= _1k)
		len = scnprintf(buf, buf_size, "%s%uK", is_last ? ">" : "", bin_size_bytes / _1k);
	else
		len = scnprintf(buf, buf_size, "%s%uB", is_last ? ">" : "", bin_size_bytes);
	(void)len;
	return buf;
}

static inline int __to_hist_bin(const unsigned verb, u64 size, unsigned sector_size)
{
	const unsigned bin_size = size / sector_size;
	int rv = 0;
	unsigned i;

	(void)verb;

	/* Short-cut - check if bigger than last sized-bin */
	if (bin_size > io_sizes_hist[io_sizes_hist_n_bins - 1]) {
		rv = io_sizes_hist_n_bins;
		goto out;
	}

	/* Short-cut using look-up-table */
	if (likely(bin_size < MAX_IO_SIZES_LUT_BLOCK_SIZE)) {
		rv = io_sizes_bin_lut[bin_size];
		goto out;
	}
	/* To big for short-cut, just do a plain old loop.
	 * But we can at least start from the highest index in the LUT  */
	for (i = io_sizes_bin_lut[MAX_IO_SIZES_LUT_BLOCK_SIZE - 1]; i < io_sizes_hist_n_bins; i++) {
		if (bin_size <= io_sizes_hist[i]) {
			rv = i;
			goto out;
		}
	}
	/* To big for the bins, just put in last bin */
	rv = io_sizes_hist_n_bins;

out:
	return rv;
}	/* Can't use percpu allocation - it is GPL */

struct nvmeib_io_stats {
	char name[NAME_MAX + 1];
	unsigned block_size;
	unsigned long verbs_bitmask;
	spinlock_t spinlock; /* Only for simulator */
	struct nvmeib_io_counters __percpu *percpu;
	struct nvmeib_io_counters __percpu *percpu_traced; /* Copy of the counters that were last traced, as the were traced (already aggregated over bins and converted to 100ns units) */
	unsigned n_percpu_ctrs;
	unsigned n_percpu_traced_ctrs;
};

/* Calculates the number of bits set before bit in the mask
 * Used to work out the location of a verb in the counters using the verb_bitmask */
static inline unsigned bitrank(unsigned long mask, unsigned bit)
{
	return hweight_long(mask & ((1U << bit) - 1));
}

/* Last bin is for all sizes > io_sizes_hist_n_bins */
#define IO_COUNTERS_NUM_BINS(n_sized_bins) (n_sized_bins + 1)

#define NUM_IO_COUNTERS_PER_BIN(verbs_bitmask) (hweight_long(verbs_bitmask))
#define NUM_IO_COUNTERS_PER_CPU(verbs_bitmask, n_sized_bins) (NUM_IO_COUNTERS_PER_BIN(verbs_bitmask) * IO_COUNTERS_NUM_BINS(n_sized_bins))

#define IO_COUNTERS_PER_CPU_VERB_BIN(pcpu_ptr, n_pcpu_ctrs, verbs_bitmask, n_sized_bins, verb, bin) ({\
	struct nvmeib_io_counters *ctrs = ((struct nvmeib_io_counters *)pcpu_ptr + (bitrank(verbs_bitmask, verb) * IO_COUNTERS_NUM_BINS(n_sized_bins) + bin));\
	BUG_ON((ctrs + 1) > pcpu_ptr + n_pcpu_ctrs);\
	ctrs;\
})

// Traced IO counters only have a single bin (which is the sum of all normal counters bins)
#define NUM_IO_COUNTERS_PER_CPU_TRACED(verbs_bitmask) NUM_IO_COUNTERS_PER_BIN(verbs_bitmask)
#define IO_COUNTERS_PER_CPU_TRACED_VERB(pcpu_ptr, n_pcpu_traced_ctrs, verbs_bitmask, verb) ({\
	struct nvmeib_io_counters *ctrs = ((struct nvmeib_io_counters *)pcpu_ptr + bitrank(verbs_bitmask, verb));\
	BUG_ON((ctrs + 1) > pcpu_ptr + n_pcpu_traced_ctrs);\
	ctrs;\
})

struct nvmeib_io_stats *nvmeib_io_stats_create(const char *name, unsigned long verbs_bitmask, unsigned block_size)
{
	struct nvmeib_io_stats *ds;

	NFIN;
	init_io_sizes_bin_lut();
	if (!(ds = kzalloc(sizeof(*ds), GFP_KERNEL))) {
		_NE(error_nvmeib_io_stats_nvmeib_io_stats_create, "OOM: cannot allocat memory for disk @NAME stats", name);
		goto out;
	}
	strncpy(ds->name, name, NAME_MAX);
	ds->verbs_bitmask = verbs_bitmask;
	ds->block_size = block_size;
	spin_lock_init(&ds->spinlock);
	ds->percpu_traced = NULL;
	ds->n_percpu_ctrs = NUM_IO_COUNTERS_PER_CPU(verbs_bitmask, io_sizes_hist_n_bins);
	if ((ds->percpu = __nvmeib_public_alloc_percpu_zeroed(
		ds->n_percpu_ctrs * sizeof(struct nvmeib_io_counters), CACHELINE_SIZE)) == NULL) 
	{
		_NE(error_1_nvmeib_io_stats_nvmeib_io_stats_create, "Failed to allocate per_cpu stats for @NAME", name);
		goto free_ds;
	}
	goto out;

free_ds:
	nvmeib_io_stats_free(ds);
	ds = NULL;

out:
	NFOUT;
	return ds;
}
EXPORT_SYMBOL(nvmeib_io_stats_create);

struct nvmeib_io_stats *nvmeib_io_stats_create_traced(const char *name, unsigned long verbs_bitmask, unsigned block_size)
{
	struct nvmeib_io_stats *ds;
	NFIN;

	if (!(ds = nvmeib_io_stats_create(name, verbs_bitmask, block_size))) {
		goto out;
	}
	ds->n_percpu_traced_ctrs = NUM_IO_COUNTERS_PER_CPU_TRACED(verbs_bitmask);
	if ((ds->percpu_traced = __nvmeib_public_alloc_percpu_zeroed(
		ds->n_percpu_traced_ctrs * sizeof(struct nvmeib_io_counters), CACHELINE_SIZE)) == NULL) 
	{
		_NE(error_1_nvmeib_io_stats_nvmeib_io_stats_create_traced, "Failed to allocate per_cpu_traced stats for @NAME", name);
		goto free_ds;
	}
	goto out;

free_ds:
	nvmeib_io_stats_free(ds);
	ds = NULL;

out:
	NFOUT;
	return ds;
}
EXPORT_SYMBOL(nvmeib_io_stats_create_traced);

void nvmeib_io_stats_free(struct nvmeib_io_stats *ds)
{
	NFIN;
	if (ds != NULL) {
		if (ds->percpu) {
			struct nvmeib_io_counters c = {0};
			int i;
			for (i = 0; i < N_IO_STAT_VERBS; i++) {
				if (!nvmeib_io_stats_counts_verb(ds, i))
					continue;
				memset(&c, 0, sizeof(c));
				nvmeib_io_stats_readc(ds, (const enum nvmeib_io_stat_verbs)i, 0, &c);
				_NI(trace_nvmeib_io_stats_free, "STATS: @IOSTATS_NAME @IOSTATS_VERB - total_iops: "
					"@IOSTATS_IOPS_COUNT total_size: @IOSTATS_IOPS_SIZE total_latency: @IOSTATS_IOPS_LATENCY",
					ds->name, i, c.total_ops, c.total_size, c.total_latency);
			}
		}

		nvmeib_public_free_percpu(ds->percpu_traced);
		nvmeib_public_free_percpu(ds->percpu);
		kfree(ds);
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_io_stats_free);

void nvmeib_io_stats_set_block_size(struct nvmeib_io_stats *ds, unsigned block_size)
{
	NFIN;
	ds->block_size = block_size;
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_io_stats_set_block_size);

bool nvmeib_io_stats_counts_verb(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb)
{
	return !!(ds->verbs_bitmask & (VERB_TO_MASK(verb)));
}
EXPORT_SYMBOL(nvmeib_io_stats_counts_verb);

static void __io_counters_merge(struct nvmeib_io_counters *dst,
				const struct nvmeib_io_counters *src)
{
	dst->total_ops                += src->total_ops;
	dst->inflight_ops             += src->inflight_ops;
	dst->total_executions         += src->total_executions;
	dst->total_size               += src->total_size;
	dst->total_latency            += src->total_latency;
	dst->total_latency_sqr        += src->total_latency_sqr;
	dst->total_io_exec            += src->total_io_exec;
	dst->total_e2e_exec           += src->total_e2e_exec;
	dst->total_sub_block          += src->total_sub_block;
	MAX_WITH(dst->worst_latency , src->worst_latency);
	MAX_WITH(dst->worst_io_exec , src->worst_io_exec);
	MAX_WITH(dst->worst_e2e_exec, src->worst_e2e_exec);
}

// Can be updated with spinlock taken (disk/OS stats) or without (volume disk stats)
static inline void nvmeib_io_verbs_update(struct nvmeib_io_stats *stats, const unsigned verb, const struct nvmeib_io_counters *src)
{
	const int bin_size = __to_hist_bin(verb, src->total_size, stats->block_size);
	unsigned long flags;

	/* Only if verb is in verb_bitmask */
	if (VERB_TO_MASK(verb) & stats->verbs_bitmask) {
		struct nvmeib_io_counters *cpu_ctrs, *dst;
		/* Only does spinlock on simulator, just get_cpu() and local_irq_save otherwise */
		io_verbs_lock_irqsave(stats, flags);
		cpu_ctrs = this_cpu_ptr(stats->percpu);
		dst = IO_COUNTERS_PER_CPU_VERB_BIN(cpu_ctrs, stats->n_percpu_ctrs, stats->verbs_bitmask, io_sizes_hist_n_bins, verb, bin_size);
		__io_counters_merge(dst, src);
		io_verbs_unlock_irqrestore(stats, flags);
	}
}

void nvmeib_io_stats_update_one(struct nvmeib_io_stats *child_stats, struct nvmeib_io_stats *parent_stats,
			    const enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters src)
{
	NFIN;
	if (child_stats && verb < N_IO_STAT_VERBS) {
		nvmeib_io_verbs_update(child_stats, verb, &src);
		if (parent_stats) {
			nvmeib_io_verbs_update(parent_stats, verb, &src);
		}
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_io_stats_update_one);
static void __count_inflight(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size, int inc)
{
	unsigned long flags;
	if (ds && verb < N_IO_STAT_VERBS) {
		int bin_size;
		struct nvmeib_io_counters *cpu_ctrs;
		/* Only does spinlock on simulator, just local_irq_save otherwise */
		io_verbs_lock_irqsave(ds, flags);
		cpu_ctrs = this_cpu_ptr(ds->percpu);
		if(VERB_TO_MASK(verb) & ds->verbs_bitmask) {
			struct nvmeib_io_counters *dst;
			bin_size = __to_hist_bin(verb, size, ds->block_size);
			dst = IO_COUNTERS_PER_CPU_VERB_BIN(cpu_ctrs, ds->n_percpu_ctrs, ds->verbs_bitmask,  io_sizes_hist_n_bins, verb, bin_size);
			dst->inflight_ops += inc;
		}
		io_verbs_unlock_irqrestore(ds, flags);
	}
}

void nvmeib_io_stats_operation_start(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size)
{
	__count_inflight(ds, verb, size, 1);
}
EXPORT_SYMBOL(nvmeib_io_stats_operation_start);

void nvmeib_io_stats_operation_end(struct nvmeib_io_stats *ds, const enum nvmeib_io_stat_verbs verb, u64 size)
{
	__count_inflight(ds, verb, size, -1);
}
EXPORT_SYMBOL(nvmeib_io_stats_operation_end);

static void __stats_conv_to_10nanosec_units(struct nvmeib_io_counters *c)
{
	/* latency is in units of ns */
	const int factor = 10;
	/* latency sqr is in units of 1/(1 << (SQUARE_LAT_DIV_TERM_POW2 * 2)) ns - currently 1/256 ns [(lat>>4) * (lat>>4)]  */
	const int factor2_mult = (1 << (SQUARE_LAT_DIV_TERM_POW2 * 2));
	const int factor2_div = 10;
	c->total_latency      /= factor;
	c->total_latency_sqr  *= factor2_mult;
	c->total_latency_sqr  /= factor2_div;
	c->total_io_exec      /= factor;
	c->total_e2e_exec     /= factor;
	c->worst_latency      /= factor;
	c->worst_io_exec      /= factor;
}

static inline void nvmeib_io_verbs_readc_all_bins(const struct nvmeib_io_counters *cpu_ctrs, unsigned n_cpu_ctrs,
						  const unsigned long verb_bitmask,
						  const unsigned verb, struct nvmeib_io_counters *c)
{
	unsigned s;
	const struct nvmeib_io_counters *ci;
	for (s = 0; s < IO_COUNTERS_NUM_BINS(io_sizes_hist_n_bins); s++, ci++) {
		ci = IO_COUNTERS_PER_CPU_VERB_BIN(cpu_ctrs, n_cpu_ctrs, verb_bitmask, io_sizes_hist_n_bins, verb, s);
		__io_counters_merge(c, ci);
	}
}

static inline void nvmeib_io_verbs_readc_bin(const struct nvmeib_io_counters *cpu_ctrs, unsigned n_cpu_ctrs,
	const unsigned long verb_bitmask, const unsigned verb, const int bin, struct nvmeib_io_counters *c)
{
	const struct nvmeib_io_counters *ci = IO_COUNTERS_PER_CPU_VERB_BIN(cpu_ctrs, n_cpu_ctrs, verb_bitmask, io_sizes_hist_n_bins, verb, bin);
	__io_counters_merge(c, ci);
}

struct io_stats_readc_bin_cpu_params {
	unsigned verb;
	int bin;
	const struct nvmeib_io_stats *ds;
	struct nvmeib_io_counters *c;
};

DECLARE_IO_VERBS_ON_EACH_CPU_FN(io_stats_readc_bin_per_cpu_fn, arg)
{
	struct io_stats_readc_bin_cpu_params *params = arg;
	struct nvmeib_io_counters *cpu_ctrs;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		return;
	if (!(VERB_TO_MASK(params->verb) & params->ds->verbs_bitmask))
		return;

	cpu_ctrs = per_cpu_ptr(params->ds->percpu, cpu);

	if (params->bin == -1) {
		nvmeib_io_verbs_readc_all_bins(cpu_ctrs, params->ds->n_percpu_ctrs, params->ds->verbs_bitmask, params->verb, &params->c[cpu]);
	} else {
		nvmeib_io_verbs_readc_bin(cpu_ctrs, params->ds->n_percpu_ctrs, params->ds->verbs_bitmask, params->verb, params->bin, &params->c[cpu]);
	}
}

static void nvmeib_io_stats_readc_bin(struct nvmeib_io_stats *ds,
	const unsigned verb, const int bin, struct nvmeib_io_counters *c)
{
	if (VERB_TO_MASK(verb) & ds->verbs_bitmask) {
		struct io_stats_readc_bin_cpu_params params = {
			.verb = verb,
			.bin = bin,
			.ds = ds,
		};
		int i;
		io_verbs_on_each_cpu_check();
		if ((params.c = kcalloc(NVMEIB_DFLT_MAX_CPUS, sizeof(*params.c), GFP_KERNEL))) {
			io_verbs_on_each_cpu_wait(ds, io_stats_readc_bin_per_cpu_fn, &params);
			for (i = 0; i < NVMEIB_DFLT_MAX_CPUS; i++)
				__io_counters_merge(c, &params.c[i]);
			kfree(params.c);
		}
		__stats_conv_to_10nanosec_units(c);
	}
}

void nvmeib_io_stats_readc(struct nvmeib_io_stats *ds,
	const enum nvmeib_io_stat_verbs verb, const u64 size, struct nvmeib_io_counters *c)
{
	const int bin = (size==0 /* Sum all sizes */) ? -1 : __to_hist_bin(verb, size, ds->block_size);
	nvmeib_io_stats_readc_bin(ds, verb, bin, c);
}
EXPORT_SYMBOL(nvmeib_io_stats_readc);

DECLARE_IO_VERBS_ON_EACH_CPU_FN(io_stats_cpu_clear_all, arg)
{
	struct nvmeib_io_stats *ds = arg;
	struct nvmeib_io_counters *cpu_ctrs;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		return;

	cpu_ctrs = per_cpu_ptr(ds->percpu, cpu);

	memset(cpu_ctrs, 0, ds->n_percpu_ctrs * sizeof(*cpu_ctrs));
}

DECLARE_IO_VERBS_ON_EACH_CPU_FN(io_stats_cpu_clear_worst_case, arg)
{
	struct nvmeib_io_stats *ds = arg;
	struct nvmeib_io_counters *cpu_ctrs;
	unsigned verb, s;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		return;

	cpu_ctrs = per_cpu_ptr(ds->percpu, cpu);

	for_each_set_bit(verb, &ds->verbs_bitmask, N_IO_STAT_VERBS) {
		for (s = 0; s < IO_COUNTERS_NUM_BINS(io_sizes_hist_n_bins); s++) {
			struct nvmeib_io_counters *c = IO_COUNTERS_PER_CPU_VERB_BIN(cpu_ctrs, ds->n_percpu_ctrs, 
										    ds->verbs_bitmask, io_sizes_hist_n_bins, verb, s);
			c->worst_latency = c->worst_io_exec = c->worst_e2e_exec = 0ULL;
		}
	}
}

DECLARE_IO_VERBS_ON_EACH_CPU_FN(io_stats_cpu_clear_totals, arg)
{
	struct nvmeib_io_stats *ds = arg;
	struct nvmeib_io_counters *cpu_ctrs;
	unsigned verb, s;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		return;

	cpu_ctrs = per_cpu_ptr(ds->percpu, cpu);

	for_each_set_bit(verb, &ds->verbs_bitmask, N_IO_STAT_VERBS) {
		for (s = 0; s < IO_COUNTERS_NUM_BINS(io_sizes_hist_n_bins); s++) {
			struct nvmeib_io_counters *c = IO_COUNTERS_PER_CPU_VERB_BIN(cpu_ctrs, ds->n_percpu_ctrs, 
										    ds->verbs_bitmask, io_sizes_hist_n_bins, verb, s);
			c->total_ops = c->total_executions = c->total_size = c->total_latency = c->total_latency_sqr = c->total_io_exec = c->total_e2e_exec = 0ULL;
		}
	}
}

void nvmeib_io_stats_clear(struct nvmeib_io_stats *ds, const int which)
{
	io_verbs_on_each_cpu_check();
	if (which == 'A')
		io_verbs_on_each_cpu_wait(ds, io_stats_cpu_clear_all, ds);
	else if (which == 'W')
		io_verbs_on_each_cpu_wait(ds, io_stats_cpu_clear_worst_case, ds);
	else if (which == 'T')
		io_verbs_on_each_cpu_wait(ds, io_stats_cpu_clear_totals, ds);
}
EXPORT_SYMBOL(nvmeib_io_stats_clear);

static ssize_t nvmeib_json_io_stats(char *buf, size_t len,
				    const char *obj_name, const struct nvmeib_json_ops *jops,
				    size_t indent, const int is_last, unsigned long verbs_bitmask,
				    const struct nvmeib_io_counters *c);

ssize_t nvmeib_io_stats_to_json(struct nvmeib_io_stats *ds,
								char *buf, size_t len, const ulong uptime_jiff,
								const struct nvmeib_json_ops *jops, size_t indent,
								bool is_last)
{
	ssize_t count  = 0;
	char tmp_str[32];
	unsigned verb, bin, lim = IO_COUNTERS_NUM_BINS(io_sizes_hist_n_bins);
	struct nvmeib_io_counters *c, *ci;
	unsigned n_ctrs = NUM_IO_COUNTERS_PER_BIN(ds->verbs_bitmask);
	size_t ctrs_sz = n_ctrs * sizeof(*c);
	
	if (!(c = kzalloc(ctrs_sz, GFP_KERNEL))) {
		_NW(warn_nvmeib_io_stats_to_json_oom, "OOM");
		return -ENOMEM;
	}

	if (!jops) jops = &nvmeib_json_ops;
	sprintf(tmp_str, "%ld.%03ld", uptime_jiff/HZ, 1000*(uptime_jiff%HZ)/HZ);

	count += jops->data_str(buf + count, len - count, "uptime_secs", tmp_str,
							!JSON_LAST_ELEM, indent);

	scnprintf(tmp_str, sizeof(tmp_str), "%u", ds->block_size);
	count += jops->data_str(buf + count, len - count, "block_size_bytes", tmp_str,
							!JSON_LAST_ELEM, indent);

	count += jops->start_obj(buf + count, len - count, "stats", indent++);
	for (bin = 0; bin < lim; bin++) {
		memset(c, 0, ctrs_sz);
		ci = c;
		for_each_set_bit(verb, &ds->verbs_bitmask, N_IO_STAT_VERBS) {
			nvmeib_io_stats_readc_bin(ds, verb, bin, ci);
			ci++;
		}
		count += nvmeib_json_io_stats(buf + count, len - count,
					      io_sizes_name(tmp_str, sizeof(tmp_str), bin, ds->block_size),
						jops, indent, bin == (lim - 1), ds->verbs_bitmask, c);
	}
	count += jops->end_obj(buf + count, len - count,
							is_last? JSON_LAST_ELEM: !JSON_LAST_ELEM, --indent);

	kfree(c);

	return count;
}
EXPORT_SYMBOL(nvmeib_io_stats_to_json);
static ssize_t nvmeib_json_io_stats(char *buf, size_t len,
							 const char *obj_name, const struct nvmeib_json_ops *jops,
							 size_t indent, const int is_last, unsigned long verbs_bitmask,
							 const struct nvmeib_io_counters *c)
{
	const struct nvmeib_io_counters *ci;
	unsigned verb, last_verb = find_last_bit(&verbs_bitmask, N_IO_STAT_VERBS);
	ssize_t count = 0;

	count += jops->start_obj(      buf + count, len - count, obj_name,                                     indent++);

	count += jops->start_obj(      buf + count, len - count, "ops",                                        indent++);
	count += jops->data_str(       buf + count, len - count, "units", "operations",       !JSON_LAST_ELEM, indent);

	ci = c;
	for_each_set_bit(verb, &verbs_bitmask, N_IO_STAT_VERBS) {
		count += jops->data_uval(buf + count, len - count, verb_to_string(verb, true),
					 ci->total_ops, verb == last_verb ? JSON_LAST_ELEM : !JSON_LAST_ELEM, indent);
		ci++;
	}

	count += jops->end_obj(        buf + count, len - count,                              !JSON_LAST_ELEM, --indent);

	count += jops->start_obj(      buf + count, len - count, "size",                                       indent++);
	count += jops->data_str(       buf + count, len - count, "units", "bytes",            !JSON_LAST_ELEM, indent);

	ci = c;
	for_each_set_bit(verb, &verbs_bitmask, N_IO_STAT_VERBS) {
		count += jops->data_uval(buf + count, len - count, verb_to_string(verb, true),
					 ci->total_size, verb == last_verb ? JSON_LAST_ELEM : !JSON_LAST_ELEM, indent);
		ci++;
	}

	count += jops->end_obj(        buf + count, len - count,                              !JSON_LAST_ELEM, --indent);

	count += jops->start_obj(      buf + count, len - count, "latency",                                    indent++);
	count += jops->data_str(       buf + count, len - count, "units", "100ns",            !JSON_LAST_ELEM, indent);

	ci = c;
	for_each_set_bit(verb, &verbs_bitmask, N_IO_STAT_VERBS) {
		count += jops->data_uval_float(buf + count, len - count, verb_to_string(verb, true),
					 ci->total_latency, 10, 1, verb == last_verb ? JSON_LAST_ELEM : !JSON_LAST_ELEM, indent);
		ci++;
	}
	count += jops->end_obj(        buf + count, len - count,                               JSON_LAST_ELEM, --indent);

	count += jops->end_obj(        buf + count, len - count,                               is_last,        --indent);
	return count;
}

static bool __stats_update_traced(struct nvmeib_io_counters *dst, const struct nvmeib_io_counters *src)
{
	bool any_diff = false;

#define __UPDATE_CNTR(_name) \
	if (dst->_name != src->_name) { \
		dst->_name = src->_name; \
		any_diff = true; \
	}

	__UPDATE_CNTR(total_ops);
	__UPDATE_CNTR(total_size);
	__UPDATE_CNTR(total_latency);
	__UPDATE_CNTR(total_latency_sqr);
	__UPDATE_CNTR(total_io_exec);
	__UPDATE_CNTR(total_e2e_exec);

#undef __UPDATE_CNTR

	return any_diff;
}

struct io_stats_trace_cpu_params {
	struct nvmeib_io_stats *ds;
	void (*trace_fn)(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx);
	void *trace_fn_ctx;
};

DECLARE_IO_VERBS_ON_EACH_CPU_FN(io_stats_cpu_trace, arg)
{
	struct io_stats_trace_cpu_params *params = arg;
	struct nvmeib_io_stats *ds = params->ds;
	struct nvmeib_io_counters *cpu_ctrs, *cpu_traced_ctrs;
	unsigned verb;

	if (cpu >= NVMEIB_DFLT_MAX_CPUS)
		return;

	cpu_ctrs = per_cpu_ptr(ds->percpu, cpu);
	cpu_traced_ctrs = per_cpu_ptr(ds->percpu_traced, cpu);

	for_each_set_bit(verb, &ds->verbs_bitmask, N_IO_STAT_VERBS) {
		struct nvmeib_io_counters c = { 0 };
		struct nvmeib_io_counters *c_traced = IO_COUNTERS_PER_CPU_TRACED_VERB(cpu_traced_ctrs, ds->n_percpu_traced_ctrs, ds->verbs_bitmask, verb);
		nvmeib_io_verbs_readc_all_bins(cpu_ctrs, ds->n_percpu_ctrs, ds->verbs_bitmask, verb, &c);
		if (__stats_update_traced(c_traced, &c))
			params->trace_fn(verb, c_traced, params->trace_fn_ctx);
	}
}

void nvmeib_io_stats_trace(struct nvmeib_io_stats *ds, void (*trace_fn)(enum nvmeib_io_stat_verbs verb, const struct nvmeib_io_counters *c, void *ctx), void *trace_fn_ctx)
{
	struct io_stats_trace_cpu_params params = { .ds = ds, .trace_fn = trace_fn, .trace_fn_ctx = trace_fn_ctx };

	io_verbs_on_each_cpu_check();
	io_verbs_on_each_cpu_wait(ds, io_stats_cpu_trace, &params);
}
EXPORT_SYMBOL(nvmeib_io_stats_trace);


static inline u64 _diff_sec(u64 exec_time_msec, u64 io_prob_msec)
{
	const s64 diff = (exec_time_msec - io_prob_msec);
	return (diff < 0LL) ? 0 : (diff/1000);
}

/* dumps IO statistics into a buffer */
ssize_t nvmeib_iostats_sum_to_string(struct nvmeib_io_stats *stats, const ulong up_time, const u64 io_prob,
				     char *buf, size_t len)
{
	#define BUF_ADD(...) count += scnprintf(buf+count, len-count, __VA_ARGS__)
	#define BUF_ADD_DOT0(v)	BUF_ADD("%20llu"   , v);
	#define BUF_ADD_DOT1(v)	BUF_ADD("%18llu.%d", (v)/10, (int)((v)%10))
	#define BUF_ADD_DOT2(v)	BUF_ADD("%20d"   , v);
	struct nvmeib_io_counters c[N_IO_STAT_VERBS];
	ssize_t count = 0;
	int i;
	/* Print in units of micro-seconds. Latency/factor is in units of 1/10^7 of
	   a second and we also do /10 when printing. so total of 1/10^6 sec */
	BUF_ADD("up_time=%ld.%01ld[sec]\n", up_time / HZ, (10 * (up_time % HZ))/HZ);
	if (unlikely(!stats)) {			/* No IO API supported (Carrier Volume)*/
		BUF_ADD("Stats not supported for this object!\n");
		goto _out;
	}

	memset(c, 0, sizeof(c));			/* Fill IO stats lists */
	for (i = 0; i < N_IO_STAT_VERBS; i++)
		nvmeib_io_stats_readc(stats, i, 0 /* All sizes */, &c[i]);
	BUF_ADD("%-16s|%20s%20s%20s\n", "*", verb_to_string(IO_STAT_VERB_READ, false), verb_to_string(IO_STAT_VERB_WRITE, false), verb_to_string(IO_STAT_VERB_DISCARD, false));
	BUF_ADD("%-16s|","num_ops");         for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT0(c[i].total_ops);           BUF_ADD("\n");
	BUF_ADD("%-16s|","size");            for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT0(c[i].total_size);          BUF_ADD(" [bytes]\n");
	BUF_ADD("%-16s|","inflight");        for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT2(c[i].inflight_ops);       BUF_ADD("\n");
	BUF_ADD("%-16s|","total_latency");   for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT1(c[i].total_latency);       BUF_ADD(" [usec]\n");
	BUF_ADD("%-16s|","total_execution"); for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT1(c[i].total_io_exec);       BUF_ADD(" [usec]\n");
	BUF_ADD("%-16s|","total_e2e");       for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT1(c[i].total_e2e_exec);      BUF_ADD(" [usec]\n");
	BUF_ADD("%-16s|","total_executions");for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT0(c[i].total_executions);    BUF_ADD("\n");
	BUF_ADD("%-16s|","latency^2");       for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT1(c[i].total_latency_sqr);   BUF_ADD("\n");
	BUF_ADD("%-16s|","worst_latency");   for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT1(c[i].worst_io_exec);       BUF_ADD(" [usec]\n");
	BUF_ADD("%-16s|","worst_e2e");       for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT0(c[i].worst_e2e_exec/USEC_PER_SEC); BUF_ADD(" [msec]\n");
	BUF_ADD("%-16s|","worst_e2e_enbl");  for (i=IO_STAT_VERB_READ; i<=IO_STAT_VERB_DISCARD; i++) BUF_ADD_DOT0(_diff_sec(c[i].worst_e2e_exec/USEC_PER_SEC, io_prob)); BUF_ADD(" [sec]\n");
_out:
	return count;
	#undef BUF_ADD
	#undef BUF_ADD_DOT0
	#undef BUF_ADD_DOT1
}
EXPORT_SYMBOL(nvmeib_iostats_sum_to_string);
