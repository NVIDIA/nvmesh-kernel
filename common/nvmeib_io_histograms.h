/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/
#ifndef NVMEIB_IO_HISTOGRAMS_H
#define NVMEIB_IO_HISTOGRAMS_H

#include "kr_incs.h"
#include "nvmeib_io_stats.h"

struct nvmeib_io_histograms;

struct nvmeib_io_histograms *nvmeib_io_histograms_create(void);
void nvmeib_io_histograms_free(struct nvmeib_io_histograms *self);
__attribute__((nonnull (1)))
void nvmeib_io_histograms_clear(struct nvmeib_io_histograms *self);

__attribute__((nonnull (1)))
void nvmeib_io_histograms_update(struct nvmeib_io_histograms *self,
				 enum nvmeib_io_stat_verbs verb,
				 u64 size_bytes,
				 u64 latency_ns);

unsigned nvmeib_io_histograms_get_n_size_bins(void);
__attribute__((nonnull (2)))
const char *nvmeib_io_histograms_get_size_bin_name(unsigned bin, char *buf, size_t buf_size);

#define IO_LAT_BUCKETS_TFMT \
	"@NS_12_BINS"
#define NVMEIB_IO_HISTOGRAMS_N_LAT_BUCKETS 16
#define IO_LAT_BUCKETS_TARG(b) \
	(b)

__attribute__((nonnull (1, 4)))
void nvmeib_io_histograms_read_bucket_counts_per_bin(struct nvmeib_io_histograms *self,
						     enum nvmeib_io_stat_verbs verb,
						     unsigned size_bin,
						     u64 bucket_counts[NVMEIB_IO_HISTOGRAMS_N_LAT_BUCKETS],
						     u64 *size_bin_total);

struct jdr;
__attribute__((nonnull (1, 2)))
void nvmeib_io_histograms_jdr_fill(struct jdr *jdr,
				   struct nvmeib_io_histograms *self,
				   const char *labels_base);

#endif
