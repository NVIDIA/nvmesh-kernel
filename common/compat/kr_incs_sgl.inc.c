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

void sg_init_table( struct scatterlist *sg, unsigned int len) {	(void)sg; (void)len; BUG_ON(true); }			// Not implemented yet

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

size_t sg_copy_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen, long int skip, bool to_buffer) {
	struct scatterlist *cur_sg;
	unsigned int sg_ind;
	size_t copied = 0;
	for_each_sg(sgl, cur_sg, nents, sg_ind) {
		unsigned ent_skip = min(cur_sg->length, skip);
		unsigned ent_sz = cur_sg->length - ent_skip;
		void *ent_ptr = sg_virt(cur_sg) + ent_skip;
		unsigned copy_sz = min(ent_sz, buflen);
		if (to_buffer)
			memcpy(buf, ent_ptr, copy_sz);
		else
			memcpy(ent_ptr, buf, copy_sz);
		buf += copy_sz;
		BUG_ON(buflen < copy_sz);
		buflen -= copy_sz;
		copied += copy_sz;
		BUG_ON(skip < ent_skip);
		skip -= ent_skip;
	}
	return copied;
}

size_t sg_copy_from_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen) {
	return sg_copy_buffer(sgl, nents, buf, buflen, 0, false);
}

size_t sg_copy_to_buffer(struct scatterlist *sgl, unsigned int nents, void *buf, size_t buflen) {
	return sg_copy_buffer(sgl, nents, buf, buflen, 0, true);
}
