/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#include "nvmeib_io_histograms.h"
#include "nvmeib_metrics.h"
#include "nvmeib_metrics_jdr.h"
#include "nvmeib_public.h"
#include "nvmeib_jdr.h"

/* Temporary enable percentiles in proc until python scripts will be ready */
#define SHOW_LATENCY_PERCENTILES 1

/*
 * nvmeib_io_histograms design overview
 * =====================================
 *
 * Goals:
 * - Keep IO completion/update path very cheap.
 * - Provide global histogram visibility split by verb and IO size.
 *
 * Data layout:
 * - Per CPU we keep:
 *   1) total[verb]                      -> iosize histogram
 *   2) lat[verb][size_bin]              -> latency histogram for that size-bin
 * - Latency buckets are logarithmic (power-of-two) with 1us base shift.
 *
 * Why per-CPU:
 * - The hot update path only increments local CPU counters (no global lock).
 * - This avoids cache-line bouncing and contention under high IO rates.
 *
 * Read/serialize model:
 * - Query paths iterate all CPUs and merge into temporary local histograms.
 * - Percentiles are computed from merged bucket counts (approximate by bucket).
 * - JSON serialization emits all-CPU distributions for observability tooling.
 *
 * Tradeoff:
 * - Writes are extremely cheap; reads are heavier but much less frequent.
 */
enum {
	NVMEIB_IO_HISTOGRAMS_SIZE_BINS = NVMESH_METRIC_IOSIZE_HISTOGRAM12_SIZE,
	NVMEIB_IO_HISTOGRAMS_LAT_SHIFT = 10, /* 1us */
	NVMEIB_IO_HISTOGRAMS_LAT_BINS = ARRAY_SIZE(((struct nvmesh_metric_latency_histogram *)NULL)->bins)
};

struct nvmeib_io_histograms_pcpu {
	struct nvmesh_metric_latency_histogram lat[N_IO_STAT_VERBS][NVMEIB_IO_HISTOGRAMS_SIZE_BINS];
	struct nvmesh_metric_iosize_histogram12 total[N_IO_STAT_VERBS];
};

struct nvmeib_io_histograms {
	struct nvmeib_io_histograms_pcpu __percpu *percpu;
};

static inline unsigned size_to_bin(u64 size_bytes)
{
	return (unsigned)nvmesh_metric_iosize_histogram_get_bin_idx12(size_bytes);
}

static inline unsigned lat_to_bin(u64 latency_ns)
{
	return (unsigned)nvmesh_metric_get_bin_index(latency_ns,
						     NVMEIB_IO_HISTOGRAMS_LAT_SHIFT,
						     NVMEIB_IO_HISTOGRAMS_LAT_BINS);
}


struct nvmeib_io_histograms *nvmeib_io_histograms_create(void)
{
	struct nvmeib_io_histograms *self = kzalloc(sizeof(*self), GFP_KERNEL);

	if (!self) {
		return NULL;
	}

	self->percpu = nvmeib_public_alloc_percpu_zeroed(struct nvmeib_io_histograms_pcpu);
	if (!self->percpu) {
		kfree(self);
		return NULL;
	}

	return self;
}
EXPORT_SYMBOL(nvmeib_io_histograms_create);

void nvmeib_io_histograms_free(struct nvmeib_io_histograms *self)
{
	if (!self) {
		return;
	}

	nvmeib_public_free_percpu(self->percpu);
	kfree(self);
}
EXPORT_SYMBOL(nvmeib_io_histograms_free);

XDS_NONNULL(1)
void nvmeib_io_histograms_clear(struct nvmeib_io_histograms *self)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		struct nvmeib_io_histograms_pcpu *pcpu = per_cpu_ptr(self->percpu, cpu);
		unsigned verb, size_bin;

		for (verb = 0; verb < N_IO_STAT_VERBS; ++verb) {
			for (size_bin = 0; size_bin < NVMEIB_IO_HISTOGRAMS_SIZE_BINS; ++size_bin) {
				nvmesh_metric_latency_histogram_clear(&pcpu->lat[verb][size_bin]);
			}
			nvmesh_metric_iosize_histogram_clear12(&pcpu->total[verb]);
		}
	}
}
EXPORT_SYMBOL(nvmeib_io_histograms_clear);

XDS_NONNULL(1)
void nvmeib_io_histograms_update(struct nvmeib_io_histograms *self,
				 enum nvmeib_io_stat_verbs verb,
				 u64 size_bytes,
				 u64 latency_ns)
{
	unsigned long flags;
	unsigned size_bin;
	unsigned lat_bin;
	struct nvmeib_io_histograms_pcpu *pcpu;

	if (WARN_ON_ONCE(verb >= N_IO_STAT_VERBS))
		return;

	size_bin = size_to_bin(size_bytes);
	lat_bin = lat_to_bin(latency_ns);

	local_irq_save(flags);
	pcpu = this_cpu_ptr(self->percpu);
	pcpu->lat[verb][size_bin].bins[lat_bin]++;
	pcpu->total[verb].bins[size_bin]++;
	local_irq_restore(flags);
}
EXPORT_SYMBOL(nvmeib_io_histograms_update);

unsigned nvmeib_io_histograms_get_n_size_bins(void)
{
	return NVMEIB_IO_HISTOGRAMS_SIZE_BINS;
}
EXPORT_SYMBOL(nvmeib_io_histograms_get_n_size_bins);

XDS_NONNULL(2)
const char *nvmeib_io_histograms_get_size_bin_name(unsigned bin, char *buf, size_t buf_size)
{
	if (bin >= NVMEIB_IO_HISTOGRAMS_SIZE_BINS) {
		scnprintf(buf, buf_size, "n/a");
		return buf;
	}

	scnprintf(buf, buf_size, "%s", nvmesh_metric_get_iosize_bin_name(bin));

	return buf;
}
EXPORT_SYMBOL(nvmeib_io_histograms_get_size_bin_name);

XDS_NONNULL(1, 4)
void nvmeib_io_histograms_read_bucket_counts_per_bin(struct nvmeib_io_histograms *self,
						     enum nvmeib_io_stat_verbs verb,
						     unsigned size_bin,
						     u64 bucket_counts[NVMEIB_IO_HISTOGRAMS_N_LAT_BUCKETS],
						     u64 *size_bin_total)
{
	int cpu;
	unsigned lat_bin;

	memset(bucket_counts, 0, sizeof(*bucket_counts) * NVMEIB_IO_HISTOGRAMS_LAT_BINS);
	if (size_bin_total) {
		*size_bin_total = 0;
	}
	if (WARN_ON(!self->percpu ||
		    verb >= N_IO_STAT_VERBS ||
		    size_bin >= NVMEIB_IO_HISTOGRAMS_SIZE_BINS)) {
		return;
	}

	for_each_possible_cpu(cpu) {
		struct nvmeib_io_histograms_pcpu *pcpu = per_cpu_ptr(self->percpu, cpu);
		if (size_bin_total) {
			*size_bin_total += READ_ONCE(pcpu->total[verb].bins[size_bin]);
		}
		for (lat_bin = 0; lat_bin < NVMEIB_IO_HISTOGRAMS_LAT_BINS; ++lat_bin) {
			bucket_counts[lat_bin] += READ_ONCE(pcpu->lat[verb][size_bin].bins[lat_bin]);
		}
	}
}
EXPORT_SYMBOL(nvmeib_io_histograms_read_bucket_counts_per_bin);


#if SHOW_LATENCY_PERCENTILES

enum { NVMESH_METRIC_LATENCY_PCT_N = 9 };
struct nvmesh_metric_latency_percentiles {
	uint64_t values[NVMESH_METRIC_LATENCY_PCT_N];
};

XDS_UNUSED static const unsigned nvmesh_metric_latency_pct_points[NVMESH_METRIC_LATENCY_PCT_N] = {
	1, 5, 10, 25, 50, 75, 90, 95, 99
};

static void XDS_NONNULL(1,3)
jdr_write_latency_percentiles(struct nvmesh_metrics_closure* base,
			      char const* name,
			      struct nvmesh_metric_latency_percentiles const * const metric)
{
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;
	char key[8]; /* "P99\0" fits comfortably */
	unsigned i;

	jdr_object_scope(writer, name);
	{
		jdr_object_scope(writer, "percentiles");
		for (i = 0; i < NVMESH_METRIC_LATENCY_PCT_N; i++) {
			snprintf(key, sizeof(key), "P%u", nvmesh_metric_latency_pct_points[i]);
			writer->ops.u64(writer, key, metric->values[i]);
		}
	}
}

/*
 * Compute percentile values from a latency histogram.
 * shift_ns must match the shift used when the histogram was populated
 * (e.g. NVMEIB_IO_HISTOGRAMS_LAT_SHIFT = 10 for 1us base).
 * The result for bin N is the lower bound of that bucket: 1 << (N + shift_ns).
 */
static inline XDS_NONNULL(1,3)
void nvmesh_metric_latency_histogram_to_percentiles(
	const struct nvmesh_metric_latency_histogram *hist,
	unsigned shift_ns,
	struct nvmesh_metric_latency_percentiles *out)
{
	uint64_t targets[NVMESH_METRIC_LATENCY_PCT_N];
	uint64_t total = 0, cumulative = 0;
	unsigned pct_idx, bin;

	memset(out, 0, sizeof(*out));

	for (bin = 0; bin < ARRAY_SIZE(hist->bins); bin++)
		total += hist->bins[bin];

	if (!total)
		return;

	for (pct_idx = 0; pct_idx < NVMESH_METRIC_LATENCY_PCT_N; pct_idx++)
		targets[pct_idx] = (total * nvmesh_metric_latency_pct_points[pct_idx] + 99) / 100;

	pct_idx = 0;
	for (bin = 0; bin < ARRAY_SIZE(hist->bins) && pct_idx < NVMESH_METRIC_LATENCY_PCT_N; bin++) {
		cumulative += hist->bins[bin];
		while (pct_idx < NVMESH_METRIC_LATENCY_PCT_N && cumulative >= targets[pct_idx]) {
			out->values[pct_idx] = 1ULL << (bin + shift_ns);
			pct_idx++;
		}
	}
}
#endif

XDS_NONNULL(1, 2)
void nvmeib_io_histograms_jdr_fill(struct jdr *jdr_inst,
				   struct nvmeib_io_histograms *self,
				   const char *labels_base)
{
	struct nvmeib_jdr_write_closure jdr_writer = nvmeib_jdr_write_closure_create(jdr_inst);
	char labels[256];
	char iosize_name[32];
	int cpu;
	unsigned verb, size_bin;

	if (WARN_ON(!self->percpu)) {
		return;
	}

	/* Emit one all-CPU iosize histogram and one all-CPU latency histogram per iosize bin. */
	jdr_write_var(jdr_inst, version, 1);
	{
		jdr_array_scope(jdr_inst, "metrics.all_cpus");
		for (verb = 0; verb < N_IO_STAT_VERBS; ++verb) {
			struct nvmesh_metric_iosize_histogram12 merged_size_hist = nvmesh_metric_iosize_histogram_create12();
			struct nvmesh_metric_id size_id = {
				.name = "io_histograms.iosize_distribution",
				.labels = labels
			};
			for_each_possible_cpu(cpu) {
				struct nvmeib_io_histograms_pcpu *pcpu = per_cpu_ptr(self->percpu, cpu);
				nvmesh_metric_iosize_histogram_merge12(&merged_size_hist, &pcpu->total[verb]);
			}

			snprintf(labels, sizeof(labels), "%s;verb=%s",
				labels_base ? labels_base : "",
				verb_to_string((enum nvmeib_io_stat_verbs)verb, true));
			nvmesh_metric_visit_ptr(&jdr_writer.base, NULL, &merged_size_hist, size_id);

			for (size_bin = 0; size_bin < NVMEIB_IO_HISTOGRAMS_SIZE_BINS; ++size_bin) {
				struct nvmesh_metric_latency_histogram merged_lat = nvmesh_metric_latency_histogram_create();
				struct nvmesh_metric_id lat_id = {
					.name = "io_histograms.latency_histogram",
					.labels = labels
				};

				for_each_possible_cpu(cpu) {
					struct nvmeib_io_histograms_pcpu *pcpu = per_cpu_ptr(self->percpu, cpu);
					nvmesh_metric_latency_histogram_merge(&merged_lat, &pcpu->lat[verb][size_bin]);
				}

				nvmeib_io_histograms_get_size_bin_name(size_bin, iosize_name, sizeof(iosize_name));
				snprintf(labels, sizeof(labels), "%s;verb=%s;iosize=%s",
					labels_base ? labels_base : "",
					verb_to_string((enum nvmeib_io_stat_verbs)verb, true),
					iosize_name);
				nvmesh_metric_visit(jdr_writer.base, NULL, merged_lat, lat_id);
			#if SHOW_LATENCY_PERCENTILES
			{
				struct nvmesh_metric_latency_percentiles pct = {};
				nvmesh_metric_latency_histogram_to_percentiles(
					&merged_lat, NVMEIB_IO_HISTOGRAMS_LAT_SHIFT, &pct);
				jdr_write_latency_percentiles(&jdr_writer.base, NULL, &pct);
			}
			#endif
			}
		}
	}
}
EXPORT_SYMBOL(nvmeib_io_histograms_jdr_fill);
