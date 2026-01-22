/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#pragma once
#include "kr_incs.h"
#include "nvmeib_shared.h"

/* Useful structs and utilities for unitests */

struct volume_segment_index {					// Unique id of seg in NVMesh by its indices
	int volume : 8;								// Which volume
	int chunk  : 8;
	int raid   : 8;
	int segment: 8;
};

struct TstPRaid {								// Object which stores pointer to protection raid. Used as iterator and reduce the amount of parameters passed to functions
	struct disk_range   *cpr;							// Configuration of protection raid
	struct tTopoOfPraid *tpr;							// Toma topology of protection raid
	struct volume_segment_index vsi;
};

//See nvmesh_sim.h for convinient initialization
//struct TstPRaid NVMeshSystem_TstPRaid_init_abs(struct NVMeshSystem *sys, u8 volume_index, u8 abs_praid_index);
//struct TstPRaid NVMeshSystem_TstPRaid_init_rel(struct NVMeshSystem *sys, u8 volume_index, u8 chunk_index, u8 praid_index, u8 segment_index);


struct test_context{
	//groups commonly accessed variables under a single roof
	struct NVMeshSystem *sys;
	struct clientSimulator *client;
	struct nvmeibc_block_device *dev;
	struct TstPRaid sraid;
};
void __dd_clean_dlba_pointers(struct test_context env);



