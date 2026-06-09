/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#if !defined(__KERNEL__) && !defined(WARN)
	#include <assert.h>
	#define WARN(condition, format, ...) ({assert(!(condition)); (void)format;})
#endif

#include "common/pet/nvmeib_pet_specification.h"

static inline void __nvmeib_pet_journalbuf_write_spacer(struct nvmeib_pet_journalbuf* self, u16 physical_offset, u16 body_n_bytes)
{
	/* message_id zero marks a spacer; bytes is the payload after this header. */
	struct nvmeib_pet_journalbuf_message_header const spacer = {
		.message_id = 0,
		.spacer = {
			.bytes = body_n_bytes,
			.unused = 0,
		},
	};

	//cannot fire - defensive local invariant
	BUG_ON((size_t)physical_offset + sizeof(spacer) + body_n_bytes > self->data.iov_len);
	memcpy((u8*)self->data.iov_base + physical_offset, &spacer, sizeof(spacer));
}

static inline u16 __nvmeib_pet_journalbuf_calculate_consumable_n_bytes(struct nvmeib_pet_journalbuf const* self, u16 physical_offset, u16 eof_offset)
{
	u16 remaining = 0;
	struct nvmeib_pet_journalbuf_message_header header = {0};
	u16 msg_n_bytes = 0;

	BUG_ON(physical_offset > eof_offset);
	remaining = eof_offset - physical_offset;

	/* EOF tail smaller than a header cannot be parsed by the viewer. */
	if (remaining < sizeof(header)) {
		return remaining;
	}

	memcpy(&header, (u8*)self->data.iov_base + physical_offset, sizeof(header));
	if (header.message_id == 0) {
		/* Spacer bytes are the payload after the header. */
		if (likely(header.spacer.unused == 0 &&
		    header.spacer.bytes <= (remaining - sizeof(header)))) {
			return (u16)(sizeof(header) + header.spacer.bytes);
		}

		/* Bad spacer is a journal buffer invariant violation. */
		BUG();
		return remaining;
	}

	/* Message length is header plus argument payload, capped by EOF. */
	msg_n_bytes = sizeof(header) + header.msg.args_n_bytes;
	return msg_n_bytes <= remaining ? msg_n_bytes : remaining;
}

/* Slow allocation path for rotation.
 * Called only when append cannot cover the write. It wraps when needed, writes
 * spacers, and maintains the viewer contract: message/spacer records up to
 * max_written_bytes, never partial records after EOF.
 */
u8* __nvmeib_pet_journalbuf_allocate_rotate(struct nvmeib_pet_journalbuf* self, u16 size)
{
	u16 const header_n_bytes = sizeof(struct nvmeib_pet_journalbuf_message_header);
	u16 const msg_and_spacer_n_bytes = size + header_n_bytes;
	u16 write_offset = self->write_offset;
	size_t write_end = (size_t)write_offset + size;
	u16 span = 0;

	/* No room at the tail; shrink EOF if needed and wrap to the prefix. */
	if (unlikely(write_end > self->data.iov_len)) {
		self->max_written_bytes = write_offset;

		self->write_offset = self->protected_prefix;
		write_offset = self->write_offset;
		write_end = (size_t)write_offset + size;
	}

	/* Even after wrap, the message must fit in the rotatable area. */
	BUG_ON(write_end > self->data.iov_len);

	/* Wrapping may also make the new message reach the current EOF. */
	if (write_end >= self->max_written_bytes) {
		self->write_offset = (u16)write_end;
		self->max_written_bytes = self->write_offset;
		return (u8*)self->data.iov_base + write_offset;
	}

	/* Middle overwrites need space for the message and a spacer header. */
	while (write_offset + span < self->max_written_bytes) {
		u16 const consumable_n_bytes = __nvmeib_pet_journalbuf_calculate_consumable_n_bytes(self, write_offset + span, self->max_written_bytes);

		BUG_ON(!consumable_n_bytes);

		span += consumable_n_bytes;
		/* Exact fit, EOF, or enough room for the message and the spacer. */
		if (span == size ||
		    ((write_offset + span) == self->max_written_bytes) ||
		    span >= msg_and_spacer_n_bytes) {
			break;
		}
	}

	BUG_ON(span < size);

	/* Extra consumed bytes must become a spacer or be hidden by EOF. */
	if (span > size) {
		u16 const leftover = span - size;
		u16 const leftover_physical_offset = write_offset + size;

		/* A header-sized leftover is kept parseable by turning it into a spacer. */
		if (leftover >= header_n_bytes) {
			__nvmeib_pet_journalbuf_write_spacer(self, leftover_physical_offset, leftover - header_n_bytes);
		} else {
			/* Tiny leftovers are only valid at EOF; hide them by shrinking EOF. */
			BUG_ON(write_offset + span != self->max_written_bytes);
			self->max_written_bytes = leftover_physical_offset;
		}
	}

	self->write_offset = write_offset + size;
	/* EOF extension is handled by the fast path before middle overwrite. */
	BUG_ON(self->max_written_bytes < self->write_offset);
	return (u8*)self->data.iov_base + write_offset;
}
EXPORT_SYMBOL(__nvmeib_pet_journalbuf_allocate_rotate);
