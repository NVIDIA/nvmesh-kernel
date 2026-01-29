/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <stdlib.h>
#include <string.h>
#include "nvmeib_heap.h"

// Written by Omri Mann. Modified by Ronen

static inline void heap_realloc_as_needed(nvmeib_heap_t *heap)
{
	if (heap->n_elements == heap->n_allocated_elements) {
		heap->n_allocated_elements = (heap->n_allocated_elements < 128 ? 128 : 2 * heap->n_allocated_elements);
		heap->heap_arr = realloc(heap->heap_arr, heap->n_allocated_elements * sizeof(nvmeib_heap_element_t *));
	}
}

static inline void heap_move_element(nvmeib_heap_element_t *heap_arr[], int dst_idx, int src_idx)
{
	heap_arr[dst_idx] = heap_arr[src_idx];
	heap_arr[dst_idx]->idx_in_heap_arr = dst_idx;
}

static inline void heap_assign_element(nvmeib_heap_element_t *heap_arr[], int dst_idx, nvmeib_heap_element_t *src_heap_element)
{
	heap_arr[dst_idx] = src_heap_element;
	heap_arr[dst_idx]->idx_in_heap_arr = dst_idx;
}

nvmeib_heap_element_t *nvmeib_heap_peek_top(nvmeib_heap_t *heap)
{
	return (heap->n_elements ? heap->heap_arr[0] : NULL);
}

void nvmeib_heap_relocate_element(nvmeib_heap_t *heap, nvmeib_heap_element_t *relocated_element)
{
	int							i = relocated_element->idx_in_heap_arr;
	nvmeib_heap_element_t		**heap_arr;

	heap_realloc_as_needed(heap);
	heap_arr = heap->heap_arr;
	if (i > 0 && relocated_element->timestamp < heap_arr[(i-1)>>1]->timestamp) {	// trickle towards the root
		do {
			heap_move_element(heap_arr, i, (i-1)>>1);
			i = (i-1)>>1;
		} while (i > 0 && relocated_element->timestamp < heap_arr[(i-1)>>1]->timestamp);
	}
	else {	// trickle towards the leafs
		while (2*i+2 < heap->n_elements) {
			if (heap_arr[2*i+1]->timestamp < heap_arr[2*i+2]->timestamp) {
				if (relocated_element->timestamp > heap_arr[2*i+1]->timestamp) {
					heap_move_element(heap_arr, i, 2*i+1);
					i = 2*i+1;
				}
				else
					break;
			}
			else {
				if (relocated_element->timestamp > heap_arr[2*i+2]->timestamp) {
					heap_move_element(heap_arr, i, 2*i+2);
					i = 2*i+2;
				}
				else
					break;
			}
		}
		if (2*i+2 == heap->n_elements) {
			if (relocated_element->timestamp > heap_arr[2*i+1]->timestamp) {
				heap_move_element(heap_arr, i, 2*i+1);
				i = 2*i+1;
			}
		}
	}
	heap_assign_element(heap_arr, i, relocated_element);
}

void nvmeib_heap_remove_element(nvmeib_heap_t *heap, nvmeib_heap_element_t *removed_element)
{
	int							i = removed_element->idx_in_heap_arr;
	nvmeib_heap_element_t		**heap_arr = heap->heap_arr;

	if (heap->n_elements > 0) {
		heap_move_element(heap_arr, i, heap->n_elements - 1);
		--heap->n_elements;
		nvmeib_heap_relocate_element(heap, heap->heap_arr[i]);
	}
}

void nvmeib_heap_add_element(nvmeib_heap_t *heap, nvmeib_heap_element_t *added_element)
{
	int						i;
	nvmeib_heap_element_t	**heap_arr;

	heap_realloc_as_needed(heap);
	i = heap->n_elements;
	heap_arr = heap->heap_arr;
	while (i > 0 && heap_arr[(i-1)>>1]->timestamp > added_element->timestamp) {
		heap_move_element(heap_arr, i, (i-1)>>1);
		i = (i-1)>>1;
	}
	heap_assign_element(heap_arr, i, added_element);
	++heap->n_elements;
}

/******************************************************************************/

#if (0)
int main(int ac ,char **av) {
	nvmeib_heap_t 	heap;
	memset(&heap, 0, sizeof(heap));
	return 0;
}
#endif

void __attribute__((__unused__)) make_heap(nvmeib_heap_t *heap)		// Turn an array into a heap (linear complexity)
{
	nvmeib_heap_element_t			**heap_arr = heap->heap_arr;
	int								i;
	struct nvmeib_heap_element_t	*tmp;

	for (i = heap->n_elements-1; i > 0; i--) {
		if (heap_arr[(i-1)>>1]->timestamp > heap_arr[i]->timestamp) {
			tmp = heap_arr[i];
			heap_move_element(heap_arr, i, (i-1)>>1);
			heap_assign_element(heap_arr, (i-1)>>1, tmp);
		}
	}
}


