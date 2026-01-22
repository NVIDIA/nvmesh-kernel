/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib_version_kernel.h"
#include "nvmeib_version_shared.h"

#include "nvmeib.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib_metrics_jdr.h"

#ifdef TRACE_INCLUDE_FILE
	/* TRACE_INCLUDE_FILE should be defined in Makefile of eac*h one of the modules that includes this file */
	#include TRACE_INCLUDE_FILE
#else
	#error "Tracing not not supported, fix compilation or use kr_incs_dummy_empty_traces.h"
#endif

static struct nvmeib_public_procfs_ent *version_proc = NULL;
static struct nvmeib_public_procfs_ent *version_json_proc = NULL;
static struct nvmeib_public_procfs_ent *meta_metrics_proc = NULL;

static ssize_t fill_version(void *dummy, char *buffer, size_t len)
{
	union nvmeib_version this_version = nvmeib_version_get();
	int count = 0;
	(void)dummy;

	count += scnprintf(buffer + count, len - count,
					   NVMEIB_VERSION_PRINT_FMT() "\n",
					   NVMEIB_VERSION_PRINT_ARG((&this_version)));
	return count;
}

static ssize_t fill_version_json(void *a, char *buffer, size_t len)
{
	int count = 0;
	(void)a;
	count += scnprintf(buffer + count, len - count,
			   "{\"module\" : \"common\", \"commit\" : \"%llx\", \"release\" : \"%s\", \"version\" : \"%s\", \"build_number\" : \"%s\", \"distro\" : \"%s\"",
		    (u64)COMMIT_ID, __stringify(NVMESH_RELEASE), __stringify(NVMESH_VERSION), __stringify(BUILD_NUMBER), __stringify(BUILD_DISTRO));
	count += scnprintf(buffer + count, len - count, "}\n");

	return count;
}

int nvmeib_version_proc_create(struct proc_dir_entry *proc_dir)
{
	int rv = -1;

	if (!proc_dir) {
		_NE(error_1_nvmeib_version_nvmeib_version_proc_create, "proc dir NULL");
		goto out;
	}

	if (!(version_proc = nvmeib_public_proc_create("version",
		proc_dir, &fill_version, NULL, NULL))) {
		_NE(error_nvmeib_version_nvmeib_version_proc_create, "Fail to create proc version");
		goto out;
	}
	if (!(version_json_proc = nvmeib_public_proc_create("version.json",
		proc_dir, &fill_version_json, NULL, NULL))) {
		_NE(error_2_nvmeib_version_nvmeib_version_proc_create, "Fail to create proc version.json");
		goto out;
	}

	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_version_proc_create);

void nvmeib_version_proc_remove(void)
{
	if (version_proc) {
		nvmeib_public_proc_remove(version_proc);
		version_proc = NULL;
	}
	if (version_json_proc) {
		nvmeib_public_proc_remove(version_json_proc);
		version_json_proc = NULL;
	}

	return;
}
EXPORT_SYMBOL(nvmeib_version_proc_remove);


int nvmeib_metrics_meta_proc_create(struct proc_dir_entry *proc_dir)
{
	int rv = -1;

	if (!proc_dir) {
		_NE(error_1_nvmeib_metrics_meta_proc_create, "proc dir NULL");
		goto out;
	}

	if (!(meta_metrics_proc = nvmeib_public_proc_create("metrics_meta_data",
		proc_dir, &nvmeib_jdr_serialize_meta_metrics, NULL, NULL))) {
		_NE(error_2_nvmeib_metrics_meta_proc_create, "Fail to create proc version");
		goto out;
	}

	rv = 0;

out:
	return rv;
}

void nvmeib_metrics_meta_proc_remove(void)
{
	if (meta_metrics_proc) {
		nvmeib_public_proc_remove(meta_metrics_proc);
		meta_metrics_proc = NULL;
	}

	return;
}
