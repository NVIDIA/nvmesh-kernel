/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_jdr.h"
#include "nvmeib_metrics.h"
#include "nvmeib_metrics_jdr.h"
#include "compat/kr_incs_time_rdtsc.h"

static void XDS_NONNULL(1,2)
jdr_write_passport(struct jdr* writer,
		   char const * const type,
		   struct nvmesh_metric_id const id)
{
	jdr_write_var(writer, type, type);
	jdr_write_var(writer, name, id.name);
	jdr_write_var(writer, labels, id.labels);
}

static void XDS_NONNULL(1,3)
jdr_write_monotonic_counter(struct nvmesh_metrics_closure* base,
			    char const* name,
			    struct nvmesh_metric_monotonic_counter const * const metric,
			    struct nvmesh_metric_id const id)
{
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;
	jdr_object_scope(writer, name);
		jdr_write_passport(writer, "monotonic_counter", id);
		jdr_write_var(writer, value, metric->counter);
}

static void XDS_NONNULL(1,3)
jdr_write_indirect_counter(struct nvmesh_metrics_closure* base,
			   char const* name,
			   struct nvmesh_metric_indirect_counter const * const metric,
			   struct nvmesh_metric_id const id)
{
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;
	jdr_object_scope(writer, name);
		jdr_write_passport(writer, "indirect_counter", id);
		jdr_write_var(writer, value, *(metric->counter_ref));
}

static void XDS_NONNULL(1,3)
jdr_write_gauge(struct nvmesh_metrics_closure* base,
		char const* name,
		struct nvmesh_metric_gauge const * const metric,
		struct nvmesh_metric_id const id)
{
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;
	jdr_object_scope(writer, name);
		jdr_write_passport(writer, "gauge", id);
		jdr_write_var(writer, value, metric->counter);
}

static void XDS_NONNULL(1,3)
jdr_write_maxval(struct nvmesh_metrics_closure* base,
		 char const* name,
		 struct nvmesh_metric_max_value const * const metric,
		 struct nvmesh_metric_id const id)
{
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;
	jdr_object_scope(writer, name);
		jdr_write_passport(writer, "max_val", id);
		jdr_write_var(writer, value, metric->counter);
}

static void XDS_NONNULL(1,3)
jdr_write_histogram(struct nvmesh_metrics_closure* base,
		    char const* name,
		    char const * const type,
		    uint64_t const * const values, size_t size,
		    struct nvmesh_metric_id const id,
		    char const * meta_metric_name)
{
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;

	jdr_object_scope(writer, name);
		jdr_write_passport(writer, type, id);
		jdr_write_var(writer, meta, meta_metric_name ? meta_metric_name : "");
		jdr_write_fundamental_array(writer, "values", values, size);
}

static void XDS_NONNULL(1,3)
jdr_write_latency_histogram(struct nvmesh_metrics_closure* base,
			    char const* name,
			    struct nvmesh_metric_latency_histogram const * const metric,
			    struct nvmesh_metric_id const id)
{
	static char const * const meta_metric_latency_name = "metrics.meta.latency";

	char const * meta = meta_metric_latency_name;
	jdr_write_histogram(base, name, "latency_histogram", metric->bins, ARRAY_SIZE(metric->bins), id, meta);
}

static void XDS_NONNULL(1,3)
jdr_write_bytes_histogram(struct nvmesh_metrics_closure* base,
			  char const* name,
			  struct nvmesh_metric_bytes_histogram const * const metric,
			  struct nvmesh_metric_id const id)
{
	static char const * const meta_metric_bytes_name = "metrics.meta.bytes";

	char const * meta = meta_metric_bytes_name;
	jdr_write_histogram(base, name, "bytes_histogram", metric->bins, ARRAY_SIZE(metric->bins), id, meta);
}

static void XDS_NONNULL(1,3)
jdr_write_iosize_histogram12(struct nvmesh_metrics_closure* base,
			     char const* name,
			     struct nvmesh_metric_iosize_histogram12 const * const metric,
			     struct nvmesh_metric_id const id)
{
	static char const * const meta_metric_iosize12_name = "metrics.meta.iosize12";

	char const * meta = meta_metric_iosize12_name;
	jdr_write_histogram(base, name, "iosize_histogram12", metric->bins, ARRAY_SIZE(metric->bins), id, meta);
}

static void XDS_NONNULL(1,3)
jdr_write_iosize_histogram9(struct nvmesh_metrics_closure* base,
			    char const* name,
			    struct nvmesh_metric_iosize_histogram9 const * const metric,
			    struct nvmesh_metric_id const id)
{
	static char const * const meta_metric_iosize9_name = "metrics.meta.iosize9";

	char const * meta = meta_metric_iosize9_name;
	jdr_write_histogram(base, name, "iosize_histogram9", metric->bins, ARRAY_SIZE(metric->bins), id, meta);
}

static void XDS_NONNULL(1,3)
jdr_write_highres_histogram(struct nvmesh_metrics_closure* base,
			       char const* name,
			       struct nvmesh_metric_highres_histogram const * const metric,
			       struct nvmesh_metric_id const id)
{
	static char const * const meta = "metrics.meta.highres";
	__auto_type writer = ((struct nvmeib_jdr_write_closure*)(base))->writer;

	jdr_object_scope(writer, name);
		jdr_write_passport(writer, "highres_histogram", id);
		jdr_write_var(writer, meta, meta);
		jdr_write_fundamental_array(writer, "values", metric->bins, ARRAY_SIZE(metric->bins));
		jdr_write_var(writer, max_ticks, metric->max);
		jdr_write_var(writer, tsc_khz, (uint64_t)nvmeib_public_tsc_khz());
}

struct nvmeib_jdr_write_closure nvmeib_jdr_write_closure_create(struct jdr *writer)
{
	struct nvmeib_jdr_write_closure jdr_write_closure = {
		.base = {
			.visit_monotonic_counter = jdr_write_monotonic_counter,
			.visit_indirect_counter = jdr_write_indirect_counter,
			.visit_gauge = jdr_write_gauge,
			.visit_maxval = jdr_write_maxval,
			.visit_latency_histogram = jdr_write_latency_histogram,
			.visit_bytes_histogram = jdr_write_bytes_histogram,
			.visit_iosize_histogram12 = jdr_write_iosize_histogram12,
			.visit_iosize_histogram9 = jdr_write_iosize_histogram9,
			.visit_highres_histogram = jdr_write_highres_histogram,
		},
		.writer = writer
	};

	return jdr_write_closure;
}
EXPORT_SYMBOL(nvmeib_jdr_write_closure_create);

static void __human_readable_size(char *buf, size_t bufsize, u64 size_bytes)
{
	const char *units[] = { "B", "KB", "MB", "GB", "TB", "PB" };
	unsigned int unit_idx = 0;
	u64 size = size_bytes;

	while (size >= 1024 && unit_idx < (sizeof(units) / sizeof(units[0])) - 1) {
		size >>= 10;  // divide by 1024 using shift
		unit_idx++;
	}

	snprintf(buf, bufsize, "%llu%s", (unsigned long long)size, units[unit_idx]);
}

static void nvmesh_metric_encode_histogram_bins(struct jdr *jdr, unsigned int shift, unsigned int num_bins)
{
	char start_str[32], end_str[32];
	char idx_str[16], bin_str[72];
	unsigned int i;
	u64 start, end;

	jdr_object_scope(jdr, "bins"); {
		for (i = 0; i < num_bins; i++) {
			if (i == 0)
				start = 1ULL;
			else
				start = 1ULL << (i + shift);

			if (i == num_bins - 1)
				end = (u64)-1;
			else
				end = (1ULL << (i + shift + 1)) - 1;

			snprintf(idx_str, sizeof(idx_str), "%u", i);

			__human_readable_size(start_str, sizeof(start_str), start);

			if (end == (u64)-1) {
				snprintf(bin_str, sizeof(bin_str), "[%s..infB)", start_str);
			} else {
				__human_readable_size(end_str, sizeof(end_str), end + 1); /* round up end by one for unit selection */
				snprintf(bin_str, sizeof(bin_str), "[%s..%s)",  start_str, end_str);
			}

			jdr_write_key_value_str(jdr, idx_str, bin_str);
		}
	}
}

static void nvmesh_metric_encode_histogram_meta(struct jdr *jdr, const char* type_str, unsigned int shift, unsigned int num_bins)
{
	const char* name_str = "primary identifier; describes what is being measured";
	const char* labels_str = "key-value pairs that provide additional dimensions or context for the metric; describes where, how and why the metric was collected";
	const char* values_str = "distribution of values(bytes in this case) over a range";

	jdr_object_scope(jdr, NULL); { // root object
		jdr_write_var(jdr, type, type_str);
		jdr_write_var(jdr, name, name_str);
		jdr_write_var(jdr, labels, labels_str);
		jdr_write_var(jdr, values, values_str);

		jdr_write_var(jdr, shift, shift);
		jdr_write_var(jdr, bins, num_bins);

		nvmesh_metric_encode_histogram_bins(jdr, shift, num_bins);
	}
}

ssize_t nvmeib_jdr_serialize_meta_metrics(void *dummy, char *buffer, size_t len)
{
	struct charvec result = {0};
	struct jdr jdr_inst = jdr_make((struct charvec){.base = buffer, .len = len});
	(void)dummy;

	{
		jdr_array_scope(&jdr_inst, "all_meta_metrics");
		nvmesh_metric_encode_histogram_meta(&jdr_inst, "bytes_histogram", NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT, NVMESH_METRIC_BYTES_HISTOGRAM_BINS);
		nvmesh_metric_encode_histogram_meta(&jdr_inst, "highres_histogram", NVMESH_METRIC_HIGHRES_HISTOGRAM_SHIFT, NVMESH_METRIC_HIGHRES_HISTOGRAM_BINS);
	}

	result = jdr_finalize(&jdr_inst); /* the resulted json resides in the user-buffer */
	return result.len;
}
