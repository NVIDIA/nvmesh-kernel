/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs_sgl.h"

void sg_set_buf(struct scatterlist *sg, const void *buf, unsigned int buflen) {
	const unsigned long offset_in_page = ((unsigned long)buf & ~PAGE_MASK);
	struct page *page = virt_to_page((void*)buf);
	if (!page) {
		sg_set_page(sg, (void*)buf , buflen, offset_in_page | 0x1); 	// 0x1 is Daniels marker that marks that virtual 'buf' is stored in 'page*' pointer
	} else {
		sg_set_page(sg, page       , buflen, offset_in_page);
	}
}

void sg_init_table(struct scatterlist *sg, unsigned int nents) {
	BUG_ON(nents == 0);
	memset(sg, 0, sizeof(*sg) * nents);
	sg_mark_end(&sg[nents - 1]);
}

int sg_alloc_table(struct sg_table *t, unsigned int n, gfp_t flags) {
	(void)flags;
	t->orig_nents = t->nents = n;
	t->sgl = (struct scatterlist*)kzalloc(sizeof(struct scatterlist) * n, 0);
	return (t->sgl==NULL);
}

void sg_free_table(struct sg_table *t){
	kfree(t->sgl);
	memset(t,0,sizeof(*t));
}

struct scatterlist *sg_next(struct scatterlist *sg) {
	BUG_ON(sg == NULL);
	return (sg_is_chain(sg)) ? sg_chain_ptr(sg) : sg + 1; // Next element in the array
}

struct scatterlist *sg_last(struct scatterlist *sg, unsigned int nents){
	BUG_ON(sg == NULL);
	return sg+nents-1;						// Last element in the array
}

#define sg_copy_buffer_to_impl(sgl, nents, buf, buflen, skip) \
({\
	struct scatterlist *cur_sg;\
	unsigned int sg_ind;\
	size_t copied = 0;\
	void *walk = (buf);\
	long int skip_left = (long int)(skip);\
	\
	for_each_sg(sgl, cur_sg, nents, sg_ind) {\
		unsigned ent_skip = min(cur_sg->length, (unsigned)skip_left);\
		unsigned ent_sz = cur_sg->length - ent_skip;\
		void *ent_ptr = sg_virt(cur_sg) + ent_skip;\
		unsigned copy_sz = min(ent_sz, (unsigned)buflen);\
		memcpy(walk, ent_ptr, copy_sz);\
		walk += copy_sz;\
		BUG_ON(buflen < copy_sz);\
		buflen -= copy_sz;\
		copied += copy_sz;\
		BUG_ON(skip_left < (long int)ent_skip);\
		skip_left -= (long int)ent_skip;\
	}\
	copied;\
})

#define sg_copy_buffer_from_impl(sgl, nents, buf, buflen, skip) \
({\
	struct scatterlist *cur_sg;\
	unsigned int sg_ind;\
	size_t copied = 0;\
	const void *walk = (buf);\
	long int skip_left = (long int)(skip);\
	\
	for_each_sg(sgl, cur_sg, nents, sg_ind) {\
		unsigned ent_skip = min(cur_sg->length, (unsigned)skip_left);\
		unsigned ent_sz = cur_sg->length - ent_skip;\
		void *ent_ptr = sg_virt(cur_sg) + ent_skip;\
		unsigned copy_sz = min(ent_sz, (unsigned)buflen);\
		memcpy(ent_ptr, walk, copy_sz);\
		walk += copy_sz;\
		BUG_ON(buflen < copy_sz);\
		buflen -= copy_sz;\
		copied += copy_sz;\
		BUG_ON(skip_left < (long int)ent_skip);\
		skip_left -= (long int)ent_skip;\
	}\
	copied;\
})

size_t sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, long int skip, bool to_buffer) {
	if (to_buffer)
		return sg_copy_buffer_to_impl(sgl, nents, buf, buflen, skip);
	else
		return sg_copy_buffer_from_impl(sgl, nents, buf, buflen, skip);
}

size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf, size_t buflen) {
	return sg_copy_buffer_from_impl(sgl, nents, buf, buflen, 0);
}

size_t sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen) {
	return sg_copy_buffer_to_impl(sgl, nents, buf, buflen, 0);
}

size_t sg_pcopy_from_buffer(struct scatterlist *sgl, unsigned int nents, const void *buf, size_t buflen, size_t skip)
{
	return sg_copy_buffer_from_impl(sgl, nents, buf, buflen, skip);
}

size_t sg_pcopy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, size_t skip)
{
	return sg_copy_buffer_to_impl(sgl, nents, buf, buflen, skip);
}

size_t sg_zero_buffer(struct scatterlist *sgl, unsigned int nents, size_t skip, size_t buflen)
{
	struct scatterlist *cur_sg;
	unsigned int sg_ind;
	size_t zeroed = 0;

	for_each_sg(sgl, cur_sg, nents, sg_ind) {
		unsigned ent_skip = min(cur_sg->length, (unsigned)skip);
		unsigned ent_sz = cur_sg->length - ent_skip;
		void *ent_ptr = sg_virt(cur_sg) + ent_skip;
		unsigned zero_sz = min(ent_sz, (unsigned)buflen);

		if (zero_sz)
			memset(ent_ptr, 0, zero_sz);
		buflen -= zero_sz;
		zeroed += zero_sz;
		BUG_ON(skip < ent_skip);
		skip -= ent_skip;
		if (!buflen)
			break;
	}
	return zeroed;
}
