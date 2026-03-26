/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "nvmeibc_volume_targets_tests.h"

#include "nvmeibc_disk.h"
#include "nvmeibc_volume_targets.h"

static void __init_test_disk(struct nvmeibc_disk *disk, const char *name)
{
	memset(disk, 0, sizeof(*disk));
	snprintf(disk->name, sizeof(disk->name), "%s", name);
}

static void __init_test_disk_id(
	struct nvmeibc_disk_id *disk_id,
	struct nvmeibc_disk *disk,
	const char *name,
	struct nvmeib_io_stats *v_disk_stats)
{
	memset(disk_id, 0, sizeof(*disk_id));
	snprintf(disk_id->name, sizeof(disk_id->name), "%s", name);
	disk_id->disk = disk;
	INIT_LIST_HEAD(&disk_id->link);
	INIT_LIST_HEAD(&disk_id->slink);
	INIT_LIST_HEAD(&disk_id->dlink);
	INIT_LIST_HEAD(&disk_id->disk_wl_link);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	disk_id->v_disk_stats = v_disk_stats;
#else
	(void)v_disk_stats;
#endif
}

static void test_nvmeibc_volume_targets_init_and_accessors(void)
{
	struct nvmeibc_volume_targets targets;
	struct list_head *disks;

	memset(&targets, 0xA5, sizeof(targets));
	nvmeibc_volume_targets_init(&targets);

	disks = nvmeibc_volume_targets_get_disks(&targets);

	BUG_ON(nvmeibc_volume_targets_base(&targets) != &targets.base);
	BUG_ON(disks != &targets.disks);
	BUG_ON(!list_empty(disks));
	BUG_ON(nvmeibc_volume_targets_count_disks(&targets) != 0);
	BUG_ON(nvmeibc_volume_targets_should_retain_disks(&targets));
	BUG_ON(nvmeibc_volume_targets_base(&targets)->find_disk_by_name == NULL);

	pr_info("test_nvmeibc_volume_targets_init_and_accessors: PASSED\n");
}

static void test_nvmeibc_volume_targets_add_disk_id_and_count_disks(void)
{
	struct nvmeibc_volume_targets targets;
	struct nvmeibc_disk disk_a, disk_b;
	struct nvmeibc_disk_id disk_id_a, disk_id_b;
	struct list_head *disks;

	nvmeibc_volume_targets_init(&targets);
	__init_test_disk(&disk_a, "disk-a");
	__init_test_disk(&disk_b, "disk-b");
	__init_test_disk_id(&disk_id_a, &disk_a, "disk-a", NULL);
	__init_test_disk_id(&disk_id_b, &disk_b, "disk-b", NULL);

	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id_a);
	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id_b);

	disks = nvmeibc_volume_targets_get_disks(&targets);

	BUG_ON(nvmeibc_volume_targets_count_disks(&targets) != 2);
	BUG_ON(list_first_entry(disks, struct nvmeibc_disk_id, link) != &disk_id_a);
	BUG_ON(list_last_entry(disks, struct nvmeibc_disk_id, link) != &disk_id_b);

	pr_info("test_nvmeibc_volume_targets_add_disk_id_and_count_disks: PASSED\n");
}

static void test_nvmeibc_volume_targets_find_disk_id_by_name(void)
{
	struct nvmeibc_volume_targets targets;
	struct nvmeibc_disk disk_a, disk_b;
	struct nvmeibc_disk_id disk_id_a, disk_id_b;

	nvmeibc_volume_targets_init(&targets);
	__init_test_disk(&disk_a, "disk-a");
	__init_test_disk(&disk_b, "disk-b");
	__init_test_disk_id(&disk_id_a, &disk_a, "disk-a", NULL);
	__init_test_disk_id(&disk_id_b, &disk_b, "disk-b", NULL);

	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id_a);
	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id_b);

	BUG_ON(nvmeibc_volume_targets_find_disk_id_by_name(&targets, "disk-a") != &disk_id_a);
	BUG_ON(nvmeibc_volume_targets_find_disk_id_by_name(&targets, "disk-b") != &disk_id_b);
	BUG_ON(nvmeibc_volume_targets_find_disk_id_by_name(&targets, "missing-disk") != NULL);

	pr_info("test_nvmeibc_volume_targets_find_disk_id_by_name: PASSED\n");
}

static void test_nvmeibc_volume_targets_find_disk_id_by_disk(void)
{
	struct nvmeibc_volume_targets targets;
	struct nvmeibc_disk disk_a, disk_b, disk_missing;
	struct nvmeibc_disk_id disk_id_a, disk_id_b;

	nvmeibc_volume_targets_init(&targets);
	__init_test_disk(&disk_a, "disk-a");
	__init_test_disk(&disk_b, "disk-b");
	__init_test_disk(&disk_missing, "missing");
	__init_test_disk_id(&disk_id_a, &disk_a, "disk-a", NULL);
	__init_test_disk_id(&disk_id_b, &disk_b, "disk-b", NULL);

	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id_a);
	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id_b);

	BUG_ON(nvmeibc_volume_targets_find_disk_id_by_disk(&targets, &disk_a) != &disk_id_a);
	BUG_ON(nvmeibc_volume_targets_find_disk_id_by_disk(&targets, &disk_b) != &disk_id_b);
	BUG_ON(nvmeibc_volume_targets_find_disk_id_by_disk(&targets, &disk_missing) != NULL);

	pr_info("test_nvmeibc_volume_targets_find_disk_id_by_disk: PASSED\n");
}

static void test_nvmeibc_volume_targets_find_disk_by_name(void)
{
	struct nvmeibc_volume_targets targets;
	struct nvmeibc_disk disk;
	struct nvmeibc_disk_id disk_id;
	struct dp_target_find_disk_result found;
	struct dp_target_find_disk_result found_via_base;
	u8 stats_storage = 0;

	nvmeibc_volume_targets_init(&targets);
	__init_test_disk(&disk, "disk-a");
	__init_test_disk_id(&disk_id, &disk, "disk-a", (struct nvmeib_io_stats *)&stats_storage);
	nvmeibc_volume_targets_add_disk_id(&targets, &disk_id);

	found = nvmeibc_volume_targets_find_disk_by_name(&targets, "disk-a");
	BUG_ON(found.disk != &disk.base);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	BUG_ON(found.v_disk_stats != (struct nvmeib_io_stats *)&stats_storage);
#endif

	found_via_base = nvmeibc_volume_targets_base(&targets)->find_disk_by_name(
		nvmeibc_volume_targets_base(&targets), "disk-a");
	BUG_ON(found_via_base.disk != &disk.base);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
	BUG_ON(found_via_base.v_disk_stats != (struct nvmeib_io_stats *)&stats_storage);
#endif

	found = nvmeibc_volume_targets_find_disk_by_name(&targets, "missing-disk");
	BUG_ON(found.disk != NULL);
	BUG_ON(found.v_disk_stats != NULL);

	pr_info("test_nvmeibc_volume_targets_find_disk_by_name: PASSED\n");
}

static void test_nvmeibc_volume_targets_retain_disks_flag(void)
{
	struct nvmeibc_volume_targets targets;

	nvmeibc_volume_targets_init(&targets);

	BUG_ON(nvmeibc_volume_targets_should_retain_disks(&targets));

	nvmeibc_volume_targets_set_retain_disks(&targets, true);
	BUG_ON(!nvmeibc_volume_targets_should_retain_disks(&targets));

	nvmeibc_volume_targets_set_retain_disks(&targets, false);
	BUG_ON(nvmeibc_volume_targets_should_retain_disks(&targets));

	pr_info("test_nvmeibc_volume_targets_retain_disks_flag: PASSED\n");
}

void nvmeibc_volume_targets_tests(void)
{
	pr_info("Running nvmeibc_volume_targets tests...\n");

	test_nvmeibc_volume_targets_init_and_accessors();
	test_nvmeibc_volume_targets_add_disk_id_and_count_disks();
	test_nvmeibc_volume_targets_find_disk_id_by_name();
	test_nvmeibc_volume_targets_find_disk_id_by_disk();
	test_nvmeibc_volume_targets_find_disk_by_name();
	test_nvmeibc_volume_targets_retain_disks_flag();

	pr_info("All nvmeibc_volume_targets tests PASSED!\n");
}
