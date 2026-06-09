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
	/* message_id zero marks a spacer; args_n_bytes is the payload after this header. */
	struct nvmeib_pet_journalbuf_message_header const spacer = {
		.message_id = 0,
		.spacer = {
			.unused = 0,
			.args_n_bytes = (u8)body_n_bytes,
		},
	};

	//cannot fire - defensive local invariants
	BUG_ON(body_n_bytes > (u8)~0U);
	BUG_ON((size_t)physical_offset + sizeof(spacer) + body_n_bytes > self->data.iov_len);
	memcpy((u8*)self->data.iov_base + physical_offset, &spacer, sizeof(spacer));
}

/* Returns the physical record size at physical_offset, or 0 if the journalbuf
 * contents are inconsistent and rotation cannot safely consume the record.
 */
static inline u16 __nvmeib_pet_journalbuf_calculate_consumable_n_bytes(struct nvmeib_pet_journalbuf const* self, u16 physical_offset)
{
	u16 remaining = 0;
	struct nvmeib_pet_journalbuf_message_header header = {0};
	u16 record_n_bytes = 0;

	if(unlikely(physical_offset > self->max_written_bytes)){
		return 0;
	}

	remaining = self->max_written_bytes - physical_offset;

	/* EOF tail smaller than a header cannot be parsed by the viewer. */
	if(unlikely(remaining < sizeof(struct nvmeib_pet_journalbuf_message_header))){
		return 0;
	}

	/* Directly reading the shared args_n_bytes byte is possible, but the
	 * measured gain was small; keep the full header for invariant checks.
	 */
	memcpy(&header, (u8*)self->data.iov_base + physical_offset, sizeof(header));
	/* msg.args_n_bytes and spacer.args_n_bytes deliberately share an offset. */
	record_n_bytes = sizeof(header) + header.msg.args_n_bytes;
	/* Once a header is visible, the full record must also be visible. */
	if (unlikely(record_n_bytes > remaining)){
		return 0;
	}

	if (unlikely(header.message_id == 0 && header.spacer.unused != 0)){
		return 0;
	}
	return record_n_bytes;
}

/* Slow allocation path for rotation.
 * Called only when append cannot cover the write. It wraps when needed, writes
 * spacers, and maintains the viewer contract: message/spacer records up to
 * max_written_bytes, never partial records after EOF.
 *
 * Returns NULL if journalbuf state is inconsistent and rotation cannot safely
 * place the requested record; the caller treats that as a dropped PET write.
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
	if(unlikely(write_end > self->data.iov_len)){
		return NULL;
	}

	/* Wrapping may also make the new message reach the current EOF. */
	if (write_end >= self->max_written_bytes) {
		self->write_offset = (u16)write_end;
		self->max_written_bytes = self->write_offset;
		return (u8*)self->data.iov_base + write_offset;
	}

	/* Middle overwrites need space for the message and a spacer header. */
	while (write_offset + span < self->max_written_bytes) {
		u16 const consumable_n_bytes = __nvmeib_pet_journalbuf_calculate_consumable_n_bytes(self, write_offset + span);

		/* Zero means malformed journalbuf state, not an empty record. */
		if(unlikely(!consumable_n_bytes)){
			return NULL;
		}

		span += consumable_n_bytes;
		/* Exact fit, EOF, or enough room for the message and the spacer. */
		if (span == size ||
		    ((write_offset + span) == self->max_written_bytes) ||
		    span >= msg_and_spacer_n_bytes) {
			break;
		}
	}

	if(unlikely(span < size)){
		return NULL;
	}

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
	if(unlikely(self->max_written_bytes < self->write_offset)){
		return NULL;
	}
	return (u8*)self->data.iov_base + write_offset;
}
EXPORT_SYMBOL(__nvmeib_pet_journalbuf_allocate_rotate);
