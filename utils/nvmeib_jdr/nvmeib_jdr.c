/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_jdr.h"
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

static size_t indent = 4;

static void __jdr_append_buffer(struct jdr* self, char const * const fmt, va_list args)
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

static void __jdr_append_seq(struct jdr* self, char const * const fmt, va_list args)
{
	seq_vprintf(self->impl.seq, fmt, args);
}

static void __attribute__((format (printf, 2, 3))) __jdr_append(struct jdr* self, char const * const fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	self->impl.append(self, fmt, args);
	va_end(args);
}

static void __jdr_vappend(struct jdr* self, char const * const fmt, va_list args)
{
	self->impl.append(self, fmt, args);
}

static void __jdr_on_value_append(struct jdr* self)
{
	if (!self->impl.is_first_value){
		__jdr_append(self, ",\n");
	}
	self->impl.is_first_value = false;
}

#define __jdr_append_name_value(jdr_inst, value_specifier, name, ...) 											\
({																												\
	__jdr_on_value_append(jdr_inst);																			\
	if (name){																									\
		__jdr_append(jdr_inst, "%*s\"%s\": " value_specifier, jdr_inst->impl.nesting, " ", name, __VA_ARGS__);	\
	} else {																									\
		__jdr_append(jdr_inst, "%*s"         value_specifier, jdr_inst->impl.nesting, " "      , __VA_ARGS__);	\
 	}																											\
})

static void __jdr_null(struct jdr* self, char const * name, struct jdr_null_type null)
{
	(void)null;
	__jdr_append_name_value(self, "%s", name, "null");
}

static void __jdr_boolean(struct jdr* self, char const * name, bool value)
{
	__jdr_append_name_value(self, "%s", name, value ? "true" : "false");
}

static void __jdr_u8(struct jdr* self, char const * name, uint8_t value)
{
#ifdef __KERNEL__
	__jdr_append_name_value(self, "%u", name, value);
#else
	__jdr_append_name_value(self, "%" PRIu8, name, value);
#endif
}

static void __jdr_s8(struct jdr* self, char const * name, int8_t value)
{
#ifdef __KERNEL__
	__jdr_append_name_value(self, "%d", name, value);
#else
	__jdr_append_name_value(self, "%" PRId8, name, value);
#endif
}

static void __jdr_u16(struct jdr* self, char const * name, uint16_t value)
{
	__jdr_append_name_value(self, "%u", name, value);
}

static void __jdr_s16(struct jdr* self, char const * name, int16_t value)
{
	__jdr_append_name_value(self, "%i", name, value);
}

static void __jdr_u32(struct jdr* self, char const * name, uint32_t value)
{
	__jdr_append_name_value(self, "%u", name, value);
}

static void __jdr_s32(struct jdr* self, char const * name, int32_t value)
{
	__jdr_append_name_value(self, "%i", name, value);
}

static void __jdr_u64(struct jdr* self, char const * name, uint64_t value)
{
#ifdef __KERNEL__
	__jdr_append_name_value(self, "%llu", name, value);
#else
	__jdr_append_name_value(self, "%" PRIu64, name, value);
#endif
}

static void __jdr_s64(struct jdr* self, char const * name, int64_t value)
{
#ifdef __KERNEL__
	__jdr_append_name_value(self, "%lld", name, value);
#else
	__jdr_append_name_value(self, "%" PRId64, name, value);
#endif
}

static void __jdr_ull(struct jdr* self, char const * name, unsigned long long value)
{
	__jdr_append_name_value(self, "%llu", name, value);
}

static void __jdr_sll(struct jdr* self, char const * name, long long value)
{
	__jdr_append_name_value(self, "%lli", name, value);
}

static void __jdr_ptr(struct jdr* self, char const * name, void const * const value)
{
	__jdr_append_name_value(self, "\"%p\"", name, value);
}

static void __jdr_ascii(struct jdr* self, char const * name, char const * const text)
{
	JDR_ASSERT((strstr(text, "\"") == NULL));
	__jdr_append_name_value(self, "\"%s\"", name, text);
}

static void __attribute__((format (printf, 3, 4))) __jdr_ascii_format(struct jdr* self, char const * name, char const * fmt, ...)
{
	va_list args;
	
	/* start appending a new key/value */
	__jdr_on_value_append(self);
	/* append the key and/or a quote */
	if (name){
		__jdr_append(self, "%*s\"%s\": \"", self->impl.nesting, " ", name);
	} else {
		__jdr_append(self, "%*s\"", self->impl.nesting, " ");
	}
	/* append the value */
	va_start(args, fmt);
	__jdr_vappend(self, fmt, args);
	va_end(args);

	/* append the closing quote */
	__jdr_append(self, "\"");
}

static void __jdr_bitmap(struct jdr* self, char const * name, unsigned long long value)
{
	if (value){
		enum{max_chars = 3 + sizeof(value)*8};

		char bits[max_chars] = {[0 ... max_chars-2] = '0', [max_chars-1] = '\0'};
		char* curr = &bits[max_chars - 2]; //start from the end
		for(; value; curr -= 1, value >>= 1){
			if (value % 2){
				*curr = '1';
			}
		}
		if (*curr == '1' || curr == &bits[max_chars-2])
			curr -= 1;
		*curr = 'b';
		curr -= 1;

		__jdr_ascii(self, name, curr);
	} else {
		__jdr_ascii(self, name, "0");
	}
}

static void __jdr_uuid(struct jdr* self, char const * name, uint8_t const* uuid)
{
	__jdr_append_name_value(self,
						 "\"%2.2x%2.2x%2.2x%2.2x-%2.2x%2.2x-%2.2x%2.2x-%2.2x%2.2x-%2.2x%2.2x%2.2x%2.2x%2.2x%2.2x\"",
						 name,
						 uuid[0], uuid[1], uuid[2],  uuid[3],  uuid[4],  uuid[5],  uuid[6],  uuid[7],
						 uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
}

static void __jdr_uuid_be(struct jdr* self, char const * name, uuid_be uuid)
{
	__jdr_uuid(self, name, (uint8_t const*)uuid.b);
}

void jdr_write_key_value_str(struct jdr *jdr, const char *key, const char *value) {
    // call the underlying writer that takes string keys (no token stringification)
    __jdr_ascii(jdr, key, value);
}

static struct jdr* __jdr_begin_scope(struct jdr* self, char const * name, char what)
{
	__jdr_append_name_value(self, "%c\n", name, what);
	self->impl.nesting += indent;
	self->impl.is_first_value = true;
	return self;
}

static void __jdr_end_scope(struct jdr* self, char what)
{
	self->impl.nesting -= indent;
	__jdr_append(self, "\n%*s%c", self->impl.nesting, " ", what);
	self->impl.is_first_value = false;
}

static struct jdr* __jdr_object(struct jdr* self, char const * name)
{
	return __jdr_begin_scope(self, name, '{');
}

static void __jdr_object_done(struct jdr* self)
{
	__jdr_end_scope(self, '}');
}

static struct jdr* __jdr_array(struct jdr* self, char const * name)
{
	return __jdr_begin_scope(self, name, '[');
}

static void __jdr_array_done(struct jdr* self)
{
	__jdr_end_scope(self, ']');
}

static void __jdr_start_document(struct jdr* self)
{
	__jdr_begin_scope(self, NULL, '{');
}

static void __jdr_end_document(struct jdr* self)
{
	__jdr_end_scope(self, '}');
}

static struct charvec __jdr_finalize_buffer(struct jdr* self)
{
	if (self->impl.total <= self->impl.input.len){
		return (struct charvec){.base = self->impl.input.base, .len = self->impl.total};
	} else {
		return (struct charvec){.base = NULL, .len = self->impl.total};
	}
}

static struct charvec __jdr_finalize_seq(struct jdr* self)
{
	if (seq_has_overflowed(self->impl.seq)) {
		return (struct charvec){.base = NULL, .len = 0};
	} else {
		return (struct charvec){.base = self->impl.seq->buf, .len = self->impl.seq->count};
	}
}

static struct jdr jdr_get_default(void)
{
	return (struct jdr){ .impl = {
		.nesting = 0,
		.is_first_value = true,
	},
	.ops = {
		.null = __jdr_null,
		.boolean = __jdr_boolean,
		.u8 = __jdr_u8,
		.s8 = __jdr_s8,
		.u16 = __jdr_u16,
		.s16 = __jdr_s16,
		.u32 = __jdr_u32,
		.s32 = __jdr_s32,
		.u64 = __jdr_u64,
		.s64 = __jdr_s64,
		.ull = __jdr_ull,
		.sll = __jdr_sll,
		.ptr = __jdr_ptr,
		.ascii = __jdr_ascii,
		.ascii_format = __jdr_ascii_format,
		.bitmap = __jdr_bitmap,
		.uuid_be = __jdr_uuid_be,

		.object = __jdr_object,
		.object_done = __jdr_object_done,

		.array = __jdr_array,
		.array_done = __jdr_array_done
	}

	};
}

struct jdr jdr_make(struct charvec buffer)
{
	struct jdr jdr = jdr_get_default();
	jdr.impl.input = buffer;
	jdr.impl.remaining = buffer;
	jdr.impl.total = 0;
	jdr.impl.append = __jdr_append_buffer;
	jdr.impl.finalize = __jdr_finalize_buffer;
	__jdr_start_document(&jdr);
	return jdr;
}

struct jdr jdr_make_seq(struct seq_file *seq)
{
	struct jdr jdr = jdr_get_default();
	jdr.impl.seq = seq;
	jdr.impl.append = __jdr_append_seq;
	jdr.impl.finalize = __jdr_finalize_seq;
	__jdr_start_document(&jdr);
	return jdr;
}

struct charvec jdr_finalize(struct jdr* jdr)
{
	__jdr_end_document(jdr);
	return jdr->impl.finalize(jdr);
}

#ifndef __KERNEL__
int nvmeib_write_file(const char *filename, const char *buf, size_t len)
{
	FILE *file = NULL;
	size_t written;
	int rv;

	file = fopen(filename, "w");
	if (!file) {
		rv = errno;
		goto out;
	}

	/* write buffer to file */
	written = 0;
	while (written < len) {
		size_t ret = fwrite(buf + written, 1, len - written, file);
		if ((ret < len - written) && ferror(file)) {
			rv = errno;
			goto out;
		}
		written += ret;
	}

	rv = 0;
out:
	if (file)
		fclose(file);
	return rv;
}
#endif
