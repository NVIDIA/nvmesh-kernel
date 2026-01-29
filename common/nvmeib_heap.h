/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_HEAP
#define NVMEIB_HEAP

#include <stdlib.h>

#define ILLEGAL_HEAP_ARR_IDX			(-10)

typedef struct nvmeib_heap_element_t {		// Must be embedded at the beginning of the element's data
	unsigned long	timestamp;
	int				idx_in_heap_arr;
} nvmeib_heap_element_t;

typedef struct nvmeib_heap_t {
	int							n_elements;
	int							n_allocated_elements;
	nvmeib_heap_element_t		**heap_arr;
} nvmeib_heap_t;

nvmeib_heap_element_t *nvmeib_heap_peek_top(nvmeib_heap_t *heap);
void nvmeib_heap_relocate_element(nvmeib_heap_t *heap, nvmeib_heap_element_t *relocated_element);
void nvmeib_heap_remove_element(nvmeib_heap_t *heap, nvmeib_heap_element_t *removed_element);
void nvmeib_heap_add_element(nvmeib_heap_t *heap, nvmeib_heap_element_t *added_element);

#endif	// #ifndef NVMEIB_HEAP

