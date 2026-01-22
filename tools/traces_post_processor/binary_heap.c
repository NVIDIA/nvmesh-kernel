/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "binary_heap.h"

void heap_init(heap_t *h) {
	h->count = 0;
	h->alloc = HEAP_INITIAL_ALLOC;
	h->arr = (heap_element_t *)malloc(sizeof(heap_element_t) * HEAP_INITIAL_ALLOC);
}

void heap_destroy(heap_t *h) {
	if (h->arr)
		free(h->arr);
	*h = (heap_t){0};
}

static void _heapify_bottom_top(heap_t *h, int index) {
	heap_element_t temp;
	int parent_node = (index - 1) / 2;

	if (h->arr[parent_node].key > h->arr[index].key) {
		// swap and recursive call
		temp = h->arr[parent_node];
		h->arr[parent_node] = h->arr[index];
		h->arr[index] = temp;
		_heapify_bottom_top(h, parent_node);
	}
}

static void _heapify_top_bottom(heap_t *h, int parent_node) {
	int left = parent_node * 2 + 1;
	int right = parent_node * 2 + 2;
	int min;
	heap_element_t temp;

	if (left >= h->count || left < 0)
		left = -1;
	if (right >= h->count || right < 0)
		right = -1;

	if (left != -1 && h->arr[left].key < h->arr[parent_node].key)
		min = left;
	else
		min = parent_node;
	if (right != -1 && h->arr[right].key < h->arr[min].key)
		min = right;

	if (min != parent_node) {
		temp = h->arr[min];
		h->arr[min] = h->arr[parent_node];
		h->arr[parent_node] = temp;

		// recursive  call
		_heapify_top_bottom(h, min);
	}
}

heap_element_t heap_pop_min(heap_t *h) {
	heap_element_t pop;
	// replace first node by last and delete last
	pop = heap_peek_min(h);
	h->arr[0] = h->arr[h->count - 1];
	--h->count;
	_heapify_top_bottom(h, 0);
	return pop;
}

heap_element_t heap_peek_min(heap_t *h) {
	assert(h->count > 0);
	return h->arr[0];
}

void heap_add(heap_t *h, heap_key_t key, void *data) {
	if (h->count >= h->alloc) {
		h->alloc *= 2;
		h->arr = (heap_element_t *)realloc(h->arr, h->alloc * sizeof(heap_element_t));
		assert(h->arr);
	}

	h->arr[h->count] = (heap_element_t){key, data};
	_heapify_bottom_top(h, h->count);
	++h->count;
}
