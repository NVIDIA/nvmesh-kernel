#ifndef BINARY_HEAP_H
#define BINARY_HEAP_H

#include <assert.h>
#include <stdlib.h>

#define HEAP_INITIAL_ALLOC 1024

typedef unsigned long long heap_key_t;

typedef struct heap_element {
	heap_key_t key;
	void *data;
} heap_element_t;

typedef struct heap {
	heap_element_t *arr;
	int count, alloc;
} heap_t;

void heap_init(heap_t *h);
void heap_destroy(heap_t *h);
heap_element_t heap_pop_min(heap_t *h);
heap_element_t heap_peek_min(heap_t *h);
void heap_add(heap_t *h, heap_key_t key, void *data);
#define heap_empty(h) (!(h)->count)

#define foreach_heap_element(h, elem) for (elem = (h).arr; elem < (h).arr + (h).count; ++elem)

#endif