/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_txt.h"
#ifdef __KERNEL__
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/string.h>
#else
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>
#endif

static void __txt_append_buffer(struct nvmeib_txt* self, char const * const fmt, va_list args)
{
	int rc = vsnprintf(self->impl.remaining.base, self->impl.remaining.len, fmt, args);

	JDR_ASSERT((0 <= rc)); //this is only valid for user space; kernel don't deal with encoding error?

	self->impl.total += rc;
	if ((size_t)rc <= self->impl.remaining.len){
		self->impl.remaining.base += rc;
		self->impl.remaining.len -=  rc;
	} else {
		self->impl.remaining = (struct charvec){0};
	}
}
static struct charvec __txt_finalize_buffer(struct nvmeib_txt* self)
{
	if (self->impl.total <= self->impl.input.len){
		return (struct charvec){.base = self->impl.input.base, .len = self->impl.total};
	} else {
		return (struct charvec){.base = NULL, .len = self->impl.total};
	}
}

struct nvmeib_txt nvmeib_txt_make(struct charvec buffer)
{
	if (buffer.base){
		buffer.base[0] = '\0';
	}
	return (struct nvmeib_txt){
		.impl = {
			.total = 0,
			.input = buffer,
			.remaining = buffer,
			.append = __txt_append_buffer,
			.finalize = __txt_finalize_buffer
		}
	};
}

#ifdef __KERNEL__
static void __txt_append_seq(struct nvmeib_txt* self, char const * const fmt, va_list args)
{
	seq_vprintf(self->impl.seq, fmt, args);
}

static struct charvec __txt_finalize_seq(struct nvmeib_txt* self)
{
	if (seq_has_overflowed(self->impl.seq)) {
		return (struct charvec){.base = NULL, .len = 0};
	} else {
		return (struct charvec){.base = self->impl.seq->buf, .len = self->impl.seq->count};
	}
}

struct nvmeib_txt nvmeib_txt_make_seq(struct seq_file *seq)
{
	return (struct nvmeib_txt){
		.impl = {
			.seq = seq,
			.append = __txt_append_seq,
			.finalize = __txt_finalize_seq
		}
	};
}
#endif

void nvmeib_txt_append(struct nvmeib_txt* self, char const * const fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	self->impl.append(self, fmt, args);
	va_end(args);
}

struct charvec nvmeib_txt_finalize(struct nvmeib_txt* txt)
{
	return txt->impl.finalize(txt);
}
