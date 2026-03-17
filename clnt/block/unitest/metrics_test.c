/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "metrics_test.h"
#include "common/nvmeib_metrics.h"
#include "common/kr_incs.h"

static void __ut_highres_histogram_create(void)
{
	struct nvmesh_metric_highres_histogram h = nvmesh_metric_highres_histogram_create();

	for (size_t i = 0; i < NVMESH_METRIC_HIGHRES_HISTOGRAM_BINS; ++i) {
		BUG_ON(h.bins[i] != 0);
	}
	BUG_ON(h.max != 0);
}

static void __ut_highres_histogram_update(void)
{
	struct nvmesh_metric_highres_histogram h = nvmesh_metric_highres_histogram_create();
	size_t idx;

	/* ticks 0..127 → bin 0 (value >> 7 == 0) */
	idx = nvmesh_metric_highres_histogram_update(&h, 0);
	BUG_ON(idx != 0);
	idx = nvmesh_metric_highres_histogram_update(&h, 100);
	BUG_ON(idx != 0);
	idx = nvmesh_metric_highres_histogram_update(&h, 127);
	BUG_ON(idx != 0);

	/* ticks 128..255 → bin 0 (value >> 7 in {1}, log2(1) == 0) */
	idx = nvmesh_metric_highres_histogram_update(&h, 128);
	BUG_ON(idx != 0);
	idx = nvmesh_metric_highres_histogram_update(&h, 255);
	BUG_ON(idx != 0);

	/* ticks 256..511 → bin 1 (value >> 7 in {2,3}, log2 == 1) */
	idx = nvmesh_metric_highres_histogram_update(&h, 256);
	BUG_ON(idx != 1);
	idx = nvmesh_metric_highres_histogram_update(&h, 511);
	BUG_ON(idx != 1);

	/* ticks 512..1023 → bin 2 (value >> 7 in {4..7}, log2 == 2) */
	idx = nvmesh_metric_highres_histogram_update(&h, 512);
	BUG_ON(idx != 2);
	idx = nvmesh_metric_highres_histogram_update(&h, 1023);
	BUG_ON(idx != 2);

	/* very large value → clamped to last bin (27) */
	idx = nvmesh_metric_highres_histogram_update(&h, 1ULL << 40);
	BUG_ON(idx != NVMESH_METRIC_HIGHRES_HISTOGRAM_BINS - 1);

	/* verify bin counts */
	BUG_ON(h.bins[0] != 5);  /* 0, 100, 127, 128, 255 */
	BUG_ON(h.bins[1] != 2);  /* 256, 511 */
	BUG_ON(h.bins[2] != 2);  /* 512, 1023 */
	BUG_ON(h.bins[NVMESH_METRIC_HIGHRES_HISTOGRAM_BINS - 1] != 1);

	/* verify max tracks the largest value */
	BUG_ON(h.max != 1ULL << 40);
}

static void __ut_highres_histogram_merge(void)
{
	struct nvmesh_metric_highres_histogram a = nvmesh_metric_highres_histogram_create();
	struct nvmesh_metric_highres_histogram b = nvmesh_metric_highres_histogram_create();

	nvmesh_metric_highres_histogram_update(&a, 256);   /* bin 1 */
	nvmesh_metric_highres_histogram_update(&a, 512);   /* bin 2 */
	/* a.max == 512 */

	nvmesh_metric_highres_histogram_update(&b, 256);   /* bin 1 */
	nvmesh_metric_highres_histogram_update(&b, 1024);  /* bin 3 */
	/* b.max == 1024 */

	nvmesh_metric_highres_histogram_merge(&a, &b);

	BUG_ON(a.bins[1] != 2);  /* 1 + 1 */
	BUG_ON(a.bins[2] != 1);  /* 1 + 0 */
	BUG_ON(a.bins[3] != 1);  /* 0 + 1 */
	BUG_ON(a.max != 1024);   /* max(512, 1024) */
}

static void __ut_highres_histogram_clear(void)
{
	struct nvmesh_metric_highres_histogram h = nvmesh_metric_highres_histogram_create();

	nvmesh_metric_highres_histogram_update(&h, 256);
	nvmesh_metric_highres_histogram_update(&h, 512);
	BUG_ON(h.max == 0);

	nvmesh_metric_highres_histogram_clear(&h);

	for (size_t i = 0; i < NVMESH_METRIC_HIGHRES_HISTOGRAM_BINS; ++i) {
		BUG_ON(h.bins[i] != 0);
	}
	BUG_ON(h.max != 0);
}

void test_metrics(void)
{
	__ut_highres_histogram_create();
	__ut_highres_histogram_update();
	__ut_highres_histogram_merge();
	__ut_highres_histogram_clear();
}
