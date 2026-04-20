/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_TXT_H_INCLUDE
#define NVMEIB_TXT_H_INCLUDE

#include "nvmeib_jdr.h"

struct nvmeib_txt{
	struct{
		union{
			struct{
				size_t total;
				struct charvec input;
				struct charvec remaining;
			};
			#ifdef __KERNEL__
			struct seq_file *seq; // if non-NULL, output to seq_file instead of buffer
			#endif
		};
		void (*append)(struct nvmeib_txt* self, char const * const fmt, va_list args);
		struct charvec (*finalize)(struct nvmeib_txt* self);
	} impl;
};

struct nvmeib_txt nvmeib_txt_make(struct charvec buffer);
#ifdef __KERNEL__
struct nvmeib_txt nvmeib_txt_make_seq(struct seq_file *seq);
#endif

__attribute__((nonnull (1)))
__attribute__((format (printf,2,3)))
void nvmeib_txt_append(struct nvmeib_txt* self, char const * const fmt, ...);

struct charvec nvmeib_txt_finalize(struct nvmeib_txt* txt);

#endif//NVMEIB_TXT_H_INCLUDE
