#include <pthread.h>
#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"
#include "nvmeibt_bm.h"
#include "nvmeibt_ds.h"

struct memory_buffer {
	struct xdlist	link;
	int				len;
	BOOL			is_dma;
	BOOL			is_in_use;
	long long		data[0];	// Align to 8
} /*__attribute((packed))*/;

typedef XDLIST_DECLARE(buffer_pool, struct memory_buffer, link) buffer_pool_t;

#define BM_DMA_ALIGNMENT_BITS 12
#define BM_DMA_ALIGNMENT_SIZE (1 << BM_DMA_ALIGNMENT_BITS)
#define BM_BUFFER_POOL_SIZE 31
#define BM_MAX_BUFFER_SIZE (1 << (BM_BUFFER_POOL_SIZE - 1))

struct nvmeibt_bm {
	pthread_mutex_t guard;
	buffer_pool_t buffer_pool[BM_BUFFER_POOL_SIZE];
	buffer_pool_t dma_buffer_pool[BM_BUFFER_POOL_SIZE];

	unsigned long allocated_size;
	unsigned long report_size;
	unsigned long report_step_size;
} *bm;									// Static root of memory allocator

static inline void update_allocated(int size) {
	bm->allocated_size += size;
	if (bm->allocated_size > bm->report_size) {
		bm->report_size += bm->report_step_size;
		N_Tf(trace_bm_update_allocated, "Buffer manager allocated @ALLOCATED_SIZE bytes", bm->allocated_size);
	}
}

static int lock(void) {
	const int rv = pthread_mutex_lock(&bm->guard);
	if (rv != 0) {
		N_Ef(error_bm_lock, "Failed to lock buffer manager (@RV)", rv);
		nvmeibt_abort(ES_FATAL);
	}
	return rv;
}

static int unlock(void) {
	const int rv = pthread_mutex_unlock(&bm->guard);
	if (rv != 0) {
		N_Ef(error_bm_unlock, "Failed to unlock buffer manager (@RV)", rv);
		nvmeibt_abort(ES_FATAL);
	}
	return rv;
}

static void free_buffer(struct memory_buffer *b) {
	const int log2_len = nvmeibt_log2_int(b->len);
	if (!b->is_in_use) {
		N_Ef(psk2j4n, "!b->is_in_use b=@PTR b->data=@PTR", b, b->data);
		nvmeibt_abort(ES_FATAL);
	}
	b->is_in_use = 0;
	lock();
	if (b->is_dma) {
		XDLIST_ADD_TAIL(&bm->dma_buffer_pool[log2_len], b);
	} else {
		XDLIST_ADD_TAIL(&bm->buffer_pool[log2_len], b);
	}
	unlock();
	// nvmeibt_bm_garbage_collect(1);	// NOTE: When debugging memory leak enable this line, otherwise you will not find who leaked it but the one who first used the buffer
}

void nvmeibt_bm_garbage_collect(int min_log_bytes) {
	int log2_len = (min_log_bytes >= 1) ? min_log_bytes : 10;	// Default > 1[kb]
	NFIN;
	for (; log2_len < BM_BUFFER_POOL_SIZE; log2_len++) {
		buffer_pool_t			*head;
		head = &bm->buffer_pool[log2_len];
		if (!(XDLIST_EMPTY(head))) {
			lock();
			while (!(XDLIST_EMPTY(head))) {
				int size;
				struct memory_buffer *b = XDLIST_FIRST(head);
				XDLIST_DEL(&b->link);
				size = offsetof(struct memory_buffer, data) + b->len;
				update_allocated(-size);
				NNVMEIBT_TOMA_FREE(bm_garbage_collect_1, b);
			}
			unlock();
		}
		head = &bm->dma_buffer_pool[log2_len];
		if (!(XDLIST_EMPTY(head))) {
			lock();
			while (!(XDLIST_EMPTY(head))) {
				void *q;
				struct memory_buffer *b = XDLIST_FIRST(head);
				XDLIST_DEL(&b->link);
				q = ((void *)b - (BM_DMA_ALIGNMENT_SIZE - offsetof(struct memory_buffer, data)));
				NNVMEIBT_TOMA_FREE(bm_garbage_collect_2, q);
			}
			unlock();
		}
	}
	NFOUT;
}

int nvmeibt_bm_get_buf_alloc_size(const void *buffer) {
	if (!buffer) {
		N_Tf(tbmbl0, "buffer=NULL");
		return 0;
	}
	if (!bm) {
		N_Ef(tbmbl1, "No bm - potential memory leakage");
		return 0;
	}
	return (container_of(buffer, struct memory_buffer, data))->len;
}

void nvmeibt_bm_free_buffer(void *buffer) {
	struct memory_buffer *b = container_of(buffer, struct memory_buffer, data);
	if (!buffer) {
		N_Tf(tbmbl2, "buffer=NULL");
		return;
	}
	if (!bm) {
		N_Ef(tbmbl3, "No bm - potential memory leakage");
		return;
	}
	N_Tf(tbmbl4, "return address=@ADDRESS_PTR to pool=@POOL", buffer, b);
	free_buffer(b);
}

static struct memory_buffer *allocate_buffer_unsafe(int unaligned_len) {
	struct memory_buffer	*b;
	const int				log2_len = nvmeibt_log2_int(unaligned_len);
	const int 				len = (1 << log2_len);
	buffer_pool_t *			head = &bm->buffer_pool[log2_len];
	if (XDLIST_EMPTY(head)) {
		const int size = offsetof(struct memory_buffer, data) + len;
		b = NNVMEIBT_TOMA_MALLOC(trace_bm_allocate_buffer, size);
		update_allocated(size);
		N_Tf(trace_1_bm_allocate_buffer, "MALLOC: log2_len=@LOG2_LEN len=@LEN", log2_len, len);
	} else {
		b = XDLIST_FIRST(head);
		XDLIST_DEL(&b->link);
	}
	if (b) {
		b->len = len;
		b->is_dma = 0;
		b->is_in_use = 1;
	} else {
		N_Ef(error_bm_allocate_buffer, "Failed to allocate small buffer");
	}
	return b;
}

static void *nvmeibt_bm_allocate_buffer_(int len) {
	struct memory_buffer *b;
	lock();
	b = allocate_buffer_unsafe(len);
	unlock();
	return b ? (void *)b->data : NULL;
}

void *nvmeibt_bm_allocate_buffer(int len) {
	void *p;
	if (len > BM_MAX_BUFFER_SIZE) {
		N_Ef(tbmbla, "len=@LEN > @LEN)", len, BM_MAX_BUFFER_SIZE);
		nvmeibt_abort(ES_FATAL);
	}
	if (!bm) {
		N_Ef(tbmblb, "No bm");
		nvmeibt_abort(ES_FATAL);
	}
	p = nvmeibt_bm_allocate_buffer_(len);
	N_Tf(tbmblc, "allocate @PPP len @LEN", p, len);
	return p;
}

void* nvmeibt_bm_calloc_buffer(int len) {
	void *p = nvmeibt_bm_allocate_buffer(len);
	if (p)
		memset(p, 0, len);
	return p;
}

static struct memory_buffer *allocate_dma_buffer_unsafe(int requested_len) {
	struct memory_buffer	*b;
	const int				log2_len = max(nvmeibt_log2_int(requested_len), BM_DMA_ALIGNMENT_BITS);
	const int				len = (1 << log2_len);
	buffer_pool_t *			head = &bm->dma_buffer_pool[log2_len];
	if (XDLIST_EMPTY(head)) {
		void *q = 0;
		NNVMEIBT_TOMA_POSIX_MEMALIGN(trace_bm_allocate_dma_buffer, &q, BM_DMA_ALIGNMENT_SIZE, len + BM_DMA_ALIGNMENT_SIZE);
		b = (struct memory_buffer *)(q + BM_DMA_ALIGNMENT_SIZE - offsetof(struct memory_buffer, data));
		N_Tf(tbmblj, "POSIX_MEMALIGN: log2_len=@LOG2_LEN len=@LEN", log2_len, len);
	} else {
		b = XDLIST_FIRST(head);
		XDLIST_DEL(&b->link);
	}
	if (b) {
		b->len = len;
		b->is_dma = 1;
		b->is_in_use = 1;
	} else {
		N_Ef(tbmblk, "Failed to allocate small buffer");
	}
	return b;
}

void *nvmeibt_bm_allocate_dma_buffer_(int len) {
	struct memory_buffer *b;
	lock();
	b = allocate_dma_buffer_unsafe(len);
	unlock();
	return b ? (void *)b->data : NULL;
}

void *nvmeibt_bm_allocate_dma_buffer(int alignment, int len) {
	void *p;
	if (!bm) {
		N_Ef(tbmbll, "No bm");
		return NULL;
	}
	if (len > BM_MAX_BUFFER_SIZE) {
		N_Ef(tbmblm, "len=@LEN > @LEN)", len, BM_MAX_BUFFER_SIZE);
		return NULL;
	}
	if (alignment > BM_DMA_ALIGNMENT_SIZE) {
		N_Ef(tbmbln, "bm dma alignment is @ALIGNMENT (requested @ALIGNMENT)", BM_DMA_ALIGNMENT_SIZE, alignment);
		return NULL;
	}
	p = nvmeibt_bm_allocate_dma_buffer_(len);
	N_Tf(tbmblo, "allocate @PPP, len @LEN", p, len);
	return p;
}

static void init_buffer_pools(void) {
	unsigned int i;
	NFIN;
	for (i = 0; i < ARRAY_SIZE(bm->buffer_pool); ++i) {
		XDLIST_HEAD_INIT(&bm->buffer_pool[i]);
	}
	for (i = 0; i < ARRAY_SIZE(bm->dma_buffer_pool); ++i) {
		XDLIST_HEAD_INIT(&bm->dma_buffer_pool[i]);
	}
	NFOUT;
}

static void free_buffer_pools(void) {
	struct memory_buffer *p;
	unsigned int i;
	NFIN;
	for (i = 0; i < ARRAY_SIZE(bm->buffer_pool); ++i) {
		XDLIST_FOREACH_SAFE(p, &bm->buffer_pool[i]) {
			XDLIST_DEL(&p->link);
			NNVMEIBT_TOMA_FREE(trace_bm_free_buffer_pools, p);
		}
	}
	for (i = 0; i < ARRAY_SIZE(bm->dma_buffer_pool); ++i) {
		XDLIST_FOREACH_SAFE(p, &bm->dma_buffer_pool[i]) {
			void *pp = (void *)((void *)p - (BM_DMA_ALIGNMENT_SIZE - offsetof(struct memory_buffer, data)));
			XDLIST_DEL(&p->link);
			NNVMEIBT_TOMA_FREE(trace_1_bm_free_buffer_pools, pp);
		}
	}
	NFOUT;
}

int nvmeibt_bm_create(void) {
	int rv = -1;
	pthread_mutexattr_t attr;
	NFIN;
	if (bm) {
		N_Ef(error_bm_nvmeibt_bm_create, "bm already exist");
		goto out;
	}
	bm = NNVMEIBT_TOMA_CALLOC(trace_bm_nvmeibt_bm_create, 1, sizeof(*bm));
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	if (pthread_mutex_init(&bm->guard, &attr) < 0) {
		N_Ef(error_1_bm_nvmeibt_bm_create, "Failed to create beffer manager guard");
		NNVMEIBT_TOMA_FREE(trace_1_bm_nvmeibt_bm_create, bm);
	} else {
		init_buffer_pools();
		rv = 0;
	}
out:
	NFOUT;
	return rv;
}

void nvmeibt_bm_destroy(void) {
	if (bm) {
		free_buffer_pools();
		NNVMEIBT_TOMA_FREE(tombmd2, bm);
	} else {
		N_Ef(tombmd1, "No bm");
	}
}
