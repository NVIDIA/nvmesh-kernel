/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_TPV_SIMU_H
#define NVMEIBC_TPV_SIMU_H

/*
 * nvmeibc_tpv_simu.h — CDV/TOMA simulator for TPV unit tests.
 *
 * Provides in-memory implementations of the five extern functions that the
 * TPV production modules (nvmeibc_tpv_allocator.c, nvmeibc_tpv_persist.c,
 * nvmeibc_tpv_recovery.c, nvmeibc_tpv_io.c) use to reach the CDV/TOMA
 * back-end:
 *
 *   nvmeibc_ib_admin_cdv_alloc_extent()  — allocate one CDV data extent
 *   nvmeibc_ib_admin_cdv_free_extent()   — return one CDV data extent
 *   nvmeibc_ib_admin_cdv_list_extents()  — list extents owned by a TPV UUID
 *   nvmeibc_tpv_cdv_sync_read()          — synchronous CDV read (RAM-backed)
 *   nvmeibc_tpv_cdv_sync_write()         — synchronous CDV write (RAM-backed)
 *   nvmeibc_tpv_cdv_submit_bio()         — async bio (BUG: not used in tests)
 *
 * Test geometry (fixed for simplicity):
 *   T = tpv_extent_size_kb = 64 KiB
 *   E = cdv_extent_size_mib = 1 MiB  →  n_slots = E/T = 16 per CDV extent
 *   A = allocator_size_gib  = 0      →  L1 tree at CDV byte offset 0
 *   virtual_size            = 4 MiB →  64 virtual extents
 *   N data CDV extents      = 4     →  indices 1..4, 64 slots total
 *   CDV total               = 5 MiB (L1 at 0..1MiB, data at 1..5MiB)
 */

#include "common/kr_incs.h"
#include "clnt/nvmeibc_msgs_shared.h"	/* nvmeibc_cdv_alloc_req/resp, free_req */

/* ── Geometry constants ─────────────────────────────────────────────────── */

#define TPV_SIMU_TPV_EXTENT_KB	64ULL		/* T — TPV extent size in KiB */
#define TPV_SIMU_CDV_EXTENT_MB	1ULL		/* E — CDV extent size in MiB */
#define TPV_SIMU_ALLOC_GB	0ULL		/* A — metadata region size in GiB */
#define TPV_SIMU_VIRTUAL_MB	4ULL		/* virtual volume size in MiB */
#define TPV_SIMU_N_DATA_EXT	4		/* number of data CDV extents */

#define TPV_SIMU_T_BYTES	(TPV_SIMU_TPV_EXTENT_KB * 1024ULL)
#define TPV_SIMU_E_BYTES	(TPV_SIMU_CDV_EXTENT_MB * 1024ULL * 1024ULL)
#define TPV_SIMU_N_SLOTS	(TPV_SIMU_E_BYTES / TPV_SIMU_T_BYTES)	/* 16 */
#define TPV_SIMU_VIRT_EXTENTS	((TPV_SIMU_VIRTUAL_MB * 1024ULL) / TPV_SIMU_TPV_EXTENT_KB) /* 64 */

/* CDV RAM buffer: 1 L1 extent + N data extents */
#define TPV_SIMU_CDV_MB		(1ULL + TPV_SIMU_N_DATA_EXT)		/* 5 MiB */
#define TPV_SIMU_CDV_BYTES	(TPV_SIMU_CDV_MB * 1024ULL * 1024ULL)

/* CDV block device size in 4 KiB sectors (used by nvmeibc_volume_get_size) */
#define TPV_SIMU_CDV_SECTORS	(TPV_SIMU_CDV_BYTES / 4096ULL)

/* Maximum data extents the simulator can track (indices 1..N) */
#define TPV_SIMU_MAX_EXTENTS	8

/* ── String identifiers ──────────────────────────────────────────────────── */

#define TPV_SIMU_CDV_UUID	"cdv-simu-uuid-000000000000000000"
#define TPV_SIMU_TPV_UUID	"tpv-simu-uuid-000000000000000000"
#define TPV_SIMU_TPV_NAME	"simu_tpv"
#define TPV_SIMU_TOMA_ID	"toma-simu-node"
#define TPV_SIMU_TOMA_GEN	1ULL

/* ── CDV simulator state ────────────────────────────────────────────────── */

/*
 * Per-extent entry in the TOMA ownership table.
 * extent_index 0 is the L1 tree and is never allocatable.
 * Entries 1..TPV_SIMU_MAX_EXTENTS represent data CDV extents.
 */
struct tpv_cdv_sim_extent {
	bool allocated;
	char owner_uuid[64];	/* NVMEIBC_BD_UUID_LEN */
};

struct tpv_cdv_sim {
	/* RAM-backed CDV storage (indexed by CDV byte offset) */
	uint8_t *cdv_ram;
	u64      cdv_bytes;

	/* TOMA extent ownership table.  Index 0 unused; 1..max_extents valid. */
	struct tpv_cdv_sim_extent extents[TPV_SIMU_MAX_EXTENTS + 1];
	u64      max_extents;		/* highest data extent index */

	/* Fault injection: inject these on the next alloc call */
	bool     inject_full;		/* return NVMEIBC_CDV_ALLOC_CDV_FULL */
	bool     inject_wrong_gen;	/* return NVMEIBC_CDV_ALLOC_WRONG_GEN */
	u64      inject_wrong_gen_val;	/* generation to report in WRONG_GEN response */
};

/* Global CDV simulator instance (one per test run). */
extern struct tpv_cdv_sim *g_tpv_cdv_sim;

/* ── Simulator lifecycle ────────────────────────────────────────────────── */

/*
 * tpv_cdv_sim_create — allocate and initialise a fresh CDV simulator.
 * Returns a pointer to the simulator (also stored in g_tpv_cdv_sim), or NULL.
 */
struct tpv_cdv_sim *tpv_cdv_sim_create(void);

/*
 * tpv_cdv_sim_destroy — free the CDV simulator and clear g_tpv_cdv_sim.
 */
void tpv_cdv_sim_destroy(void);

/* ── Mock CDV volume ────────────────────────────────────────────────────── */

struct nvmeibc_volume;
struct nvmeibc_block_device;

/*
 * tpv_cdv_vol_create — allocate a minimal nvmeibc_volume + nvmeibc_block_device
 * that represents the CDV for the simulator.  The block_dev->size is set to
 * TPV_SIMU_CDV_SECTORS (4 KiB sectors).  The hdr.uuid is set to
 * TPV_SIMU_CDV_UUID.  Status is set to NVS_ATTACHED.
 */
struct nvmeibc_volume *tpv_cdv_vol_create(void);

/*
 * tpv_cdv_vol_destroy — free the mock CDV volume and its block device.
 */
void tpv_cdv_vol_destroy(struct nvmeibc_volume *cdv);

/* ── Test helper ─────────────────────────────────────────────────────────── */

struct nvmeibc_tpv;

/*
 * tpv_simu_set_toma_id — set the allocator TOMA identity on a TPV.
 * Must be called after nvmeibc_tpv_attach() to enable cdv_alloc_work.
 */
void tpv_simu_set_toma_id(struct nvmeibc_tpv *tpv,
			   const char *toma_id, u64 generation);

/*
 * tpv_simu_fill_pool — trigger cdv_alloc_work until the CDV simulator
 * reports CDV_FULL, filling the TPV free-slot pool with all available extents.
 * Calls schedule_work + flush_workqueue in a loop.
 */
void tpv_simu_fill_pool(struct nvmeibc_tpv *tpv);

#endif /* NVMEIBC_TPV_SIMU_H */
