/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_METRICS_H_INCLUDED
#define NVMEIB_METRICS_H_INCLUDED

#include "kr_incs.h"
#include "compat/kr_incs_bit_ops.h"
#if !defined(log2_u64_nonzero)
	#define log2_u64_nonzero ilog2
#endif

#define XDS_PURE __attribute__((pure))
#define XDS_MALLOC __attribute__((malloc))
#define XDS_UNUSED __attribute__ ((unused))
#define XDS_NONNULL(...) __attribute__((nonnull (__VA_ARGS__)))
#define XDS_PRINTF(...) __attribute__((format (printf, __VA_ARGS__)))

#define xds_min(a,b) 						\
	({										\
		const __auto_type _a_min = (a);		\
		const __auto_type _b_min = (b);		\
		_a_min < _b_min ? _a_min : _b_min; 	\
	})

#define xds_max(a,b) 						\
	({										\
		const __auto_type _a_max = (a);		\
		const __auto_type _b_max = (b);		\
		_a_max > _b_max ? _a_max : _b_max; 	\
	})

#define xds_mv_typeof(type, mv) typeof(((type *)(NULL))->mv)

static inline XDS_PURE size_t nvmesh_metric_get_bin_index(uint64_t value, uint16_t shift, uint16_t n_bins)
{
	value >>= shift;
	if (likely(value)){
		const uint8_t idx = log2_u64_nonzero(value);
		return xds_min(idx, n_bins - 1);
	} else {
		return 0;
	}
}

static inline void nvmesh_metric_merge_histograms(uint64_t* dst, uint64_t const * src, uint16_t n_bins)
{
	size_t idx;

	for(idx = 0; idx < n_bins; ++idx){
		dst[idx] += src[idx];
	}
}

static inline uint64_t nvmesh_metric_histogram_total_volume(uint64_t const * src, uint16_t n_bins, uint16_t histogram_shift)
{
	uint64_t idx, total_volume = 0;
	for (idx = 0; idx < n_bins; idx++) {
		total_volume += src[idx] * (1UL << (idx + histogram_shift));
	}
	return total_volume;
}

static inline void nvmesh_metric_histograms_subtract(uint64_t * dst, uint64_t const * total, uint64_t const * sub, uint16_t n_bins)
{
	uint64_t idx;
	for (idx = 0; idx < n_bins; idx++) {
		BUG_ON(total[idx] < sub[idx]);
		dst[idx] = total[idx] - sub[idx];
	}
}

static inline void nvmesh_metric_histogram_clear(uint64_t* dst, uint16_t n_bins)
{
	size_t idx;

	for(idx = 0; idx < n_bins; ++idx){
		dst[idx] = 0;
	}
}

//For performance reasons those structs defined here and not hidden
//Please don't access their members; Use *create and *update functions

//To reduce memory consumption, some histograms are implemented with hard coded constants in mind 
//latency - in microseconds 
//ioblock_size [2**9 .. 2**20] - 12 bins 
//adding those constants to structs and more "generic" implementation will definitely consume more memory
//How many metrics instances we will have:
//* per volume 
//  * per chunk 
//    * per raid 
//	    * per segment 
//	    * per stage 
//* per disk 
//  * per channel 
//* per queue 
//* per potentially problematic synchronization primitive 
//
//Another perspective is an attempt to create structs that don't require dynamic memory. 
//I hope this will simplify the functionality propagation through our code

struct nvmesh_metric_monotonic_counter{
	//simple monotonically non-decreasing counter
	uint64_t counter;
};

static inline 
struct nvmesh_metric_monotonic_counter nvmesh_metric_monotonic_counter_create(void)
{
	return (struct nvmesh_metric_monotonic_counter){.counter=0};
}

static inline XDS_NONNULL(1) 
size_t nvmesh_metric_monotonic_counter_update(struct nvmesh_metric_monotonic_counter* self, uint64_t value)
{
	self->counter += value;
	return 0;
}

static inline XDS_NONNULL(1,2)
void nvmesh_metric_monotonic_counter_merge(struct nvmesh_metric_monotonic_counter* dst, struct nvmesh_metric_monotonic_counter const* src)
{
	dst->counter += src->counter;
}

static inline XDS_NONNULL(1)
void nvmesh_metric_monotonic_counter_clear(struct nvmesh_metric_monotonic_counter* self)
{
	self->counter = 0;
}

struct nvmesh_metric_indirect_counter{
	uint64_t * counter_ref;
};

static inline XDS_NONNULL(1)
struct nvmesh_metric_indirect_counter nvmesh_metric_indirect_counter_create(uint64_t * const counter_ref)
{
	return (struct nvmesh_metric_indirect_counter){.counter_ref=counter_ref};
}

static inline XDS_NONNULL(1) 
size_t nvmesh_metric_indirect_counter_update(struct nvmesh_metric_indirect_counter* self, int64_t value)
{
	*(self->counter_ref) += value;
	return 0;
}

static inline XDS_NONNULL(1, 2)
void nvmesh_metric_indirect_counter_merge(struct nvmesh_metric_indirect_counter* dst, struct nvmesh_metric_indirect_counter const* src)
{
	*(dst->counter_ref) += *(src->counter_ref);
}

static inline XDS_NONNULL(1)
void nvmesh_metric_indirect_counter_clear(struct nvmesh_metric_indirect_counter* self)
{
	*(self->counter_ref) = 0;
}

struct nvmesh_metric_gauge{
	int64_t counter;
};

static inline 
struct nvmesh_metric_gauge nvmesh_metric_gauge_create(void)
{
	return (struct nvmesh_metric_gauge){.counter=0};
}

static inline XDS_NONNULL(1) 
size_t nvmesh_metric_gauge_update(struct nvmesh_metric_gauge* self, int64_t value)
{
	self->counter += value;
	return 0;
}

static inline XDS_NONNULL(1,2)
void nvmesh_metric_gauge_merge(struct nvmesh_metric_gauge* dst, struct nvmesh_metric_gauge const* src)
{
	dst->counter += src->counter;
}

struct nvmesh_metric_max_value {
	int64_t counter;
};

static inline
struct nvmesh_metric_max_value nvmesh_metric_max_value_create(void)
{
	return (struct nvmesh_metric_max_value){.counter=0};
}

static inline XDS_NONNULL(1)
size_t nvmesh_metric_max_value_update(struct nvmesh_metric_max_value *self, int64_t value)
{
	if (value > self->counter)
		self->counter = value;
	return 0;
}

static inline XDS_NONNULL(1,2)
void nvmesh_metric_max_value_merge(struct nvmesh_metric_max_value *dst, struct nvmesh_metric_max_value const * src)
{
	dst->counter = xds_max(dst->counter, src->counter);
}

static inline XDS_NONNULL(1)
void nvmesh_metric_max_value_clear(struct nvmesh_metric_max_value* self)
{
	self->counter = 0;
}

//almost simple latency histogram;
//the latency is measured in ticks;
//bin X contains number of operations that last for [2^X .. 2^(X+1))
struct nvmesh_metric_latency_histogram{
	uint64_t bins[16];
};

static inline 
struct nvmesh_metric_latency_histogram nvmesh_metric_latency_histogram_create(void)
{
	return (struct nvmesh_metric_latency_histogram){.bins = {0}};
}

static inline XDS_NONNULL(1,2)
void nvmesh_metric_latency_histogram_merge(struct nvmesh_metric_latency_histogram* dst, struct nvmesh_metric_latency_histogram const* src)
{
	nvmesh_metric_merge_histograms(dst->bins, src->bins, ARRAY_SIZE(dst->bins));
}

static inline XDS_NONNULL(1)
void nvmesh_metric_latency_histogram_clear(struct nvmesh_metric_latency_histogram* hist)
{
	nvmesh_metric_histogram_clear(hist->bins, ARRAY_SIZE(hist->bins));
}

enum {
	NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT = 3,
	NVMESH_METRIC_BYTES_HISTOGRAM_BINS = 18
};
//used to track memory allocation histogram; we starts at log2(8) = 3
struct nvmesh_metric_bytes_histogram{
	uint64_t bins[NVMESH_METRIC_BYTES_HISTOGRAM_BINS];
};

static inline 
struct nvmesh_metric_bytes_histogram nvmesh_metric_bytes_histogram_create(void)
{
	return (struct nvmesh_metric_bytes_histogram){.bins = {0}};
}

static inline XDS_NONNULL(1) 
size_t nvmesh_metric_bytes_histogram_update(struct nvmesh_metric_bytes_histogram* self, uint64_t bytes)
{
	size_t const idx = nvmesh_metric_get_bin_index(bytes, NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT, ARRAY_SIZE(self->bins));

	if (!bytes) {
		return 0;
	}

	self->bins[idx] += 1;
	return idx;
}

static inline XDS_NONNULL(1,2)
void nvmesh_metric_bytes_histogram_merge(struct nvmesh_metric_bytes_histogram* dst, struct nvmesh_metric_bytes_histogram const* src)
{
	nvmesh_metric_merge_histograms(dst->bins, src->bins, ARRAY_SIZE(dst->bins));
}

static inline XDS_NONNULL(1)
uint64_t nvmesh_metric_bytes_histogram_total_allocations(struct nvmesh_metric_bytes_histogram const* self)
{
	return nvmesh_metric_histogram_total_volume(self->bins, ARRAY_SIZE(self->bins), NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT);
}

static inline XDS_NONNULL(1,2,3)
void nvmesh_metric_bytes_histograms_subtract(struct nvmesh_metric_bytes_histogram *dst, struct nvmesh_metric_bytes_histogram const *total, struct nvmesh_metric_bytes_histogram const *sub)
{
	nvmesh_metric_histograms_subtract(dst->bins, total->bins, sub->bins, ARRAY_SIZE(sub->bins));
}

static inline XDS_NONNULL(1)
void nvmesh_metric_bytes_histogram_clear(struct nvmesh_metric_bytes_histogram* hist)
{
	nvmesh_metric_histogram_clear(hist->bins, ARRAY_SIZE(hist->bins));
}

static inline 
struct nvmesh_metric_bytes_histogram nvmesh_metric_meta_bytes_histogram_create(void) {
	struct nvmesh_metric_bytes_histogram hist = nvmesh_metric_bytes_histogram_create();
	uint64_t idx;
	hist.bins[0] = 1 << NVMESH_METRIC_BYTES_HISTOGRAM_SHIFT;
	for(idx = 1; idx < ARRAY_SIZE(hist.bins); ++idx){
		hist.bins[idx] = hist.bins[idx-1]*2;
	}
	return hist;
}


//poor template :-(
//simple histogram; bin X contains counter of blocks with sizes between [2^X .. 2^(X+1))
//bin[0] accumulates data for sizes between [0..2^9]

enum {NVMESH_METRIC_IOSIZE_HISTOGRAM_SHIFT=9};

#define NVMESH_DECLARE_METRIC_IOSIZE_HISTOGRAM(n_bins)																	\
struct nvmesh_metric_iosize_histogram##n_bins{																			\
	uint64_t bins[n_bins];																								\
};																														\
																														\
static inline																											\
struct nvmesh_metric_iosize_histogram##n_bins nvmesh_metric_iosize_histogram_create##n_bins(void)						\
{																														\
	return (struct nvmesh_metric_iosize_histogram##n_bins){.bins = {0}};												\
}																														\
																														\
static inline 																											\
size_t nvmesh_metric_iosize_histogram_get_bin_idx##n_bins(uint64_t size) 												\
{																														\
	return nvmesh_metric_get_bin_index(size,																			\
			NVMESH_METRIC_IOSIZE_HISTOGRAM_SHIFT, 																		\
			ARRAY_SIZE(((struct nvmesh_metric_iosize_histogram##n_bins*)(NULL))->bins)); 								\
}																														\
																														\
static inline XDS_NONNULL(1)																							\
size_t nvmesh_metric_iosize_histogram_update##n_bins(struct nvmesh_metric_iosize_histogram##n_bins* self, uint64_t size)\
{																														\
	size_t const idx = nvmesh_metric_iosize_histogram_get_bin_idx##n_bins(size); 										\
	self->bins[idx] += 1;																								\
	return idx;																											\
}																														\
																														\
static inline																											\
struct nvmesh_metric_iosize_histogram##n_bins nvmesh_metric_meta_iosize_histogram_create##n_bins(void) {				\
	struct nvmesh_metric_iosize_histogram##n_bins hist = nvmesh_metric_iosize_histogram_create##n_bins();				\
	uint64_t idx; \
	for(idx = 1; idx < ARRAY_SIZE(hist.bins); ++idx){															\
		hist.bins[idx] = idx + NVMESH_METRIC_IOSIZE_HISTOGRAM_SHIFT;													\
	}																													\
	return hist;																										\
}																														\
																																									\
static inline XDS_NONNULL(1,2)																																		\
void nvmesh_metric_iosize_histogram_merge##n_bins(struct nvmesh_metric_iosize_histogram##n_bins* dst, struct nvmesh_metric_iosize_histogram##n_bins const* src)	\
{																																									\
	nvmesh_metric_merge_histograms(dst->bins, src->bins, ARRAY_SIZE(dst->bins));																					\
}																																									\
static inline XDS_NONNULL(1)																																		\
void nvmesh_metric_iosize_histogram_clear##n_bins(struct nvmesh_metric_iosize_histogram##n_bins* dst)	\
{																																									\
	nvmesh_metric_histogram_clear(dst->bins, ARRAY_SIZE(dst->bins));																					\
}																																									\

enum {NVMESH_METRIC_IOSIZE_HISTOGRAM12_SIZE=12};
NVMESH_DECLARE_METRIC_IOSIZE_HISTOGRAM(12); //[512B .. 1MB] == [2**9 .. 2**20] 12 == 20-9+1 - good for EC 8+2
typedef struct nvmesh_metric_iosize_histogram12 nvmesh_metric_iosize_1MB_histogram;


enum {NVMESH_METRIC_IOSIZE_HISTOGRAM9_SIZE=9};
NVMESH_DECLARE_METRIC_IOSIZE_HISTOGRAM(9); //[512B .. 128KB] ~= [2**9 .. 2**17] 9 == 17-9+1 - good for mirror & disks
typedef struct nvmesh_metric_iosize_histogram9 nvmesh_metric_iosize_128KB_histogram;

static inline char const * nvmesh_metric_get_iosize_bin_name(size_t idx) 
{
	static const char* names[12] = {
		"512B", "1KiB", "2KiB", "4KiB", "8KiB", "16KiB",
		"32KiB", "64KiB", "128KiB", "256KiB", "512KiB", "1MiB"
	};
	return names[idx];
}

//_Generic was introduce in c11, but we still use standard from the stone age
#define nvmesh_metric_create(metric_type) 																		\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_monotonic_counter),	\
		nvmesh_metric_monotonic_counter_create, 																\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_gauge),				\
		nvmesh_metric_gauge_create, 																			\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_max_value),				\
		nvmesh_metric_max_value_create, 																			\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_latency_histogram),	\
		nvmesh_metric_latency_histogram_create,																	\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_bytes_histogram),	\
		nvmesh_metric_bytes_histogram_create,																	\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_iosize_histogram12),	\
		nvmesh_metric_iosize_histogram_create12,																\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_iosize_histogram9),	\
		nvmesh_metric_iosize_histogram_create9,																	\
	(void)0 ))))))) ()
//  --------^^^^ number of ) is equal to amount of __builtin_choose_expr

#define nvmesh_metric_create_args(metric_type, ...)															\
	__builtin_choose_expr(__builtin_types_compatible_p(metric_type, struct nvmesh_metric_indirect_counter),	\
		nvmesh_metric_indirect_counter_create, 																\
	(void)0 ) (__VA_ARGS__)

#define nvmesh_metric_update_ptr(metric, value) 																	\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_monotonic_counter*),	\
		nvmesh_metric_monotonic_counter_update,																		\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_indirect_counter*),	\
		nvmesh_metric_indirect_counter_update,																		\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_gauge*),				\
		nvmesh_metric_gauge_update, 																				\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_max_value*),				\
		nvmesh_metric_max_value_update, 																			\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_bytes_histogram*),	\
		nvmesh_metric_bytes_histogram_update,																		\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_iosize_histogram12*),	\
		nvmesh_metric_iosize_histogram_update12,																	\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_iosize_histogram9*),	\
		nvmesh_metric_iosize_histogram_update9,																		\
	(void)0 ))))))) ((metric), (value))

#define nvmesh_metric_update(metric, value) nvmesh_metric_update_ptr(&(metric), value)

#define nvmesh_metric_merge(metric_dst, metric_src)																		\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_monotonic_counter*),		\
		nvmesh_metric_monotonic_counter_merge,																				\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_indirect_counter*),		\
		nvmesh_metric_indirect_counter_merge,																				\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_gauge*),					\
		nvmesh_metric_gauge_merge,																							\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_max_value*),				\
		nvmesh_metric_max_value_merge,																						\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_bytes_histogram*),		\
		nvmesh_metric_bytes_histogram_merge,																				\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_iosize_histogram12*),	\
		nvmesh_metric_iosize_histogram_merge12,																				\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_iosize_histogram9*),		\
		nvmesh_metric_iosize_histogram_merge9,																				\
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric_dst), struct nvmesh_metric_latency_histogram),		\
		nvmesh_metric_latency_histogram_merge,																				\
	(void)0 )))))))) ((metric_dst), (metric_src))

//metrics registry will store a copy of name and labels within the registry
struct nvmesh_metric_id {
	char const * const name;
	char const * const labels;
};

//name argument is optional and could be null
//for example, if we would like to serialize metrics to json as array, it is perfectly fine to pass NULL
struct nvmesh_metrics_closure
{
	void (*visit_monotonic_counter)(struct nvmesh_metrics_closure*,
									const char* name,
									struct nvmesh_metric_monotonic_counter const * const,
									struct nvmesh_metric_id);

	void (*visit_indirect_counter)(struct nvmesh_metrics_closure*,
								   const char* name,
								   struct nvmesh_metric_indirect_counter const * const,
								   struct nvmesh_metric_id);

	void (*visit_gauge)(struct nvmesh_metrics_closure*,
						const char* name,
						struct nvmesh_metric_gauge const * const,
						struct nvmesh_metric_id);

	void (*visit_maxval)(struct nvmesh_metrics_closure *,
						 const char* name,
						 struct nvmesh_metric_max_value const *const,
						 struct nvmesh_metric_id);

	void (*visit_bytes_histogram)(struct nvmesh_metrics_closure*,
									const char* name,
									struct nvmesh_metric_bytes_histogram const * const,
									struct nvmesh_metric_id);

	void (*visit_latency_histogram)(struct nvmesh_metrics_closure*,
									const char* name,
									struct nvmesh_metric_latency_histogram const * const,
									struct nvmesh_metric_id);

	void (*visit_iosize_histogram12)(struct nvmesh_metrics_closure*,
									 const char* name,
									 struct nvmesh_metric_iosize_histogram12 const * const,
									 struct nvmesh_metric_id);

	void (*visit_iosize_histogram9)(struct nvmesh_metrics_closure*,
									const char* name,
									struct nvmesh_metric_iosize_histogram9 const * const,
									struct nvmesh_metric_id);
};

#define nvmesh_metric_visit_ptr(closure, name, metric, id) \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_monotonic_counter*), \
        (closure)->visit_monotonic_counter, \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_indirect_counter*), \
        (closure)->visit_indirect_counter, \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_gauge*), \
        (closure)->visit_gauge, \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_max_value*), \
        (closure)->visit_maxval, \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_bytes_histogram*), \
        (closure)->visit_bytes_histogram, \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_iosize_histogram12*), \
        (closure)->visit_iosize_histogram12, \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_iosize_histogram9*), \
        (closure)->visit_iosize_histogram9, \
    (void)0 ))))))) ((closure), (name), (metric), (id))

#define nvmesh_metric_visit(closure, name, metric, id) nvmesh_metric_visit_ptr(&(closure), name, &(metric), id)

#define nvmesh_metric_clear_ptr(metric)                                                          \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_monotonic_counter*), \
        nvmesh_metric_monotonic_counter_clear,                                                   \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_max_value*),          \
        nvmesh_metric_max_value_clear,                                                           \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_indirect_counter*),   \
        nvmesh_metric_indirect_counter_clear,                                                    \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_bytes_histogram*),    \
        nvmesh_metric_bytes_histogram_clear,                                                     \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_iosize_histogram12*),\
	nvmesh_metric_iosize_histogram_clear12,									   \
    __builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_iosize_histogram9*), \
	nvmesh_metric_iosize_histogram_clear9,									   \
	__builtin_choose_expr(__builtin_types_compatible_p(__typeof(metric), struct nvmesh_metric_latency_histogram),   \
        nvmesh_metric_latency_histogram_clear,                                                   \
        (void) 0 ))))))) ((metric))

#define nvmesh_metric_clear(metric) nvmesh_metric_clear_ptr(&(metric))

#endif
