/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __NVMEIB_BUFFER_H__
#define __NVMEIB_BUFFER_H__

#if defined(USER_SPACE)		// cmp_blocks, core unitest, um app, etc...
struct nvmeib_buffer {
	void *buf;
	size_t size;
};

static inline bool nvmeib_buffer_is_linear(const struct nvmeib_buffer *buf)
{
	(void)buf;
	return true;
}

static inline void *nvmeib_buffer_get_virt(struct nvmeib_buffer *buf, size_t *virt_sz)
{
	if (virt_sz)
		*virt_sz = buf->size;
	return buf->buf;
}

static inline void nvmeib_buffer_init_one(struct nvmeib_buffer *buf, void *ptr, unsigned int size)
{
	buf->buf = ptr;
	buf->size = size;
}

#else // kernel code defined(__KERNEL__) or kernel simulator
#ifdef __KERNEL__
	#include <linux/scatterlist.h>
#endif
struct nvmeib_buffer {
	struct scatterlist sg;
	struct sg_table sgt;
	size_t size;
	unsigned int offset;
};

static inline bool nvmeib_buffer_is_linear(const struct nvmeib_buffer *buf)
{
	return buf->sgt.nents == 1 && buf->offset == 0 && sg_virt(buf->sgt.sgl) != NULL;
}

static inline void *nvmeib_buffer_get_virt(struct nvmeib_buffer *buf, size_t *virt_sz)
{
	BUG_ON(!nvmeib_buffer_is_linear(buf));
	if (virt_sz)
		*virt_sz = buf->sgt.sgl->length;
	return sg_virt(buf->sgt.sgl);
}

static inline void nvmeib_buffer_init_one(struct nvmeib_buffer *buf, void *ptr, unsigned int size)
{
	sg_init_one(&buf->sg, ptr, size);
	buf->sgt.sgl = &buf->sg;
	buf->sgt.nents = 1;
	buf->size = size;
	buf->offset = 0;
}

static inline void nvmeib_buffer_init_sgl(struct nvmeib_buffer *buf, struct scatterlist *sgl, unsigned int nents, unsigned int size, unsigned int offset)
{
	buf->sgt.sgl = sgl;
	buf->sgt.nents = nents;
	buf->size = size;
	buf->offset = offset;
}

int nvmeib_buffer_alloc_sgl_from_pages(struct nvmeib_buffer *buf, struct page **pages,
				       unsigned int n_pages, unsigned int size, unsigned int offset, gfp_t gfp_mask);

static inline void nvmeib_buffer_free_sgl(struct nvmeib_buffer *buf)
{
	if (buf->sgt.sgl != &buf->sg) {
		sg_free_table(&buf->sgt);
		memset(&buf->sgt, 0, sizeof(buf->sgt));
		buf->size = 0;
		buf->offset = 0;
	}
}

static inline void nvmeib_buffer_copy_to_buffer_ext(const struct nvmeib_buffer *buf, void *dest, size_t dest_sz, off_t skip)
{
	sg_copy_buffer(buf->sgt.sgl, buf->sgt.nents,
		       dest, min_t(size_t, buf->size - buf->offset - skip, dest_sz), buf->offset + skip, true);
}

static inline void nvmeib_buffer_copy_to_buffer(const struct nvmeib_buffer *buf, void *dest, size_t dest_sz)
{
	nvmeib_buffer_copy_to_buffer_ext(buf, dest, dest_sz, 0);
}

static inline void nvmeib_buffer_copy_from_buffer_ext(struct nvmeib_buffer *buf, const void *src, size_t src_sz, off_t skip)
{
	sg_copy_buffer(buf->sgt.sgl, buf->sgt.nents,
		       src, min_t(size_t, buf->size - buf->offset - skip, src_sz), buf->offset + skip, false);
}

static inline void nvmeib_buffer_copy_from_buffer(struct nvmeib_buffer *buf, const void *src, size_t src_sz)
{
	nvmeib_buffer_copy_from_buffer_ext(buf, src, src_sz, 0);
}

void nvmeib_buffer_copy_md_ext_to_buffer(const struct nvmeib_buffer *buf,
					 size_t sect_sz, size_t md_sz, void *md_dest, size_t md_dest_sz);
void nvmeib_buffer_fill(struct nvmeib_buffer *buf, const void *pattern, size_t pattern_len);

#endif // kernel code

#endif //__NVMEIB_BUFFER_H__
