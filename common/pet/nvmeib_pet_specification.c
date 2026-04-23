/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "pet/nvmeib_pet_specification.h"
#include "nvmeib_build_info.h"

struct nvmeib_pet_stream nvmeib_pet_stream_make(struct iovec data)
{
	struct nvmeib_pet_stream stream = {
		.data = data,
		.written_bytes = data.iov_base ? NVMEIB_PET_ENTITY_HEADER_SIZE : 0,
		.written_msgs = data.iov_base ? (u16*)((u8*)data.iov_base + NVMEIB_PET_STREAM_WRITTEN_MSGS_OFFSET) : (u16*)NULL
	};

	if (stream.written_msgs) {
		*(u64*)data.iov_base = (u64)COMMIT_ID;
		(*stream.written_msgs) = 0;
	}

	if (unlikely(NVMEIB_PET_MAX_STREAM_SIZE < data.iov_len)) {
		stream.data.iov_len = NVMEIB_PET_MAX_STREAM_SIZE;
	}

	return stream;
}
EXPORT_SYMBOL(nvmeib_pet_stream_make);
