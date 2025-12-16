#ifndef NVMEIBT_BM_H
#define NVMEIBT_BM_H

/* Buffer Manager (Internal memory allocation, Todo: May just replacing it with malloc improves results) */

int	 nvmeibt_bm_create(void);
void nvmeibt_bm_destroy(void);

void *nvmeibt_bm_allocate_buffer(int len);
void *nvmeibt_bm_calloc_buffer(int len);
void *nvmeibt_bm_allocate_dma_buffer(int alignment, int len); /* 4K alignment */
void  nvmeibt_bm_free_buffer(void *buffer);
int	  nvmeibt_bm_get_buf_alloc_size(const void *buffer);
void  nvmeibt_bm_garbage_collect(int min_log_bytes);	// Collect only buffers larger > 2^min_log_bytes[b]. -1 for default

#endif /* NVMEIBT_BM_H */
