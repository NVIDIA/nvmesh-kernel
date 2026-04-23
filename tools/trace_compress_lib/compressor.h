/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <sys/uio.h>
#include <stdio.h>
#include "fspath.h"

struct compression_result{
	int error;
	struct iovec *iovecs;
	int n_iovecs; // length of compressed iovec
};

struct compressor {
	struct {
		struct compression_result (*start)(struct compressor *compressor);
		struct compression_result (*stop)(struct compressor *compressor);
		struct compression_result (*write)(struct compressor *compressor, struct iovec *iovecs,
							 size_t n_iovecs);
		void (*destroy)(struct compressor *compressor);
		const char *(*file_ext)(void);
	} ops;
};

struct compressor *
compressor_create_lz4(size_t max_in_size); //max size of data submitted to "process" function

struct decompress_file_result{
    int rv;
    char error[128];
	struct fspath fpath;
};

struct decompress_file_result decompress_file_if_needed(struct fspath const* compressed, struct fspath const* decompression_dir);

// Calculate total size of iovec array
static inline size_t iovecs_total_size(struct iovec *iovecs, size_t n_iovecs)
{
	size_t tot_size = 0;
	for (struct iovec *curr = iovecs; curr != iovecs + n_iovecs; ++curr) {
		tot_size += curr->iov_len;
	}
	return tot_size;
}
