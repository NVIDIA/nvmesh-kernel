/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef UNI_SCENARIO_VOL_CONFIGS_H
#define UNI_SCENARIO_VOL_CONFIGS_H
/* Sets of unitest scenarios for volumes configurations (non datapath):
   1. Attach/Detach volumes
   2. Various wrong volume configs given by mgmt and wrong mgmt requests
   3. Allerts to mgmt log
   4. Wrong cli commands
   5. scsi & cli ioctls */
#include "../bunitest.h"

typedef enum {										// How newever volume version configuration is processed by client
	UNITEST_UPDOWNGRADE_COLD	= 0,				// Volume dettach and reaatach
	UNITEST_UPDOWNGRADE_WARM	= 1,				// Toma gets new conf before client and client cannot comply to its switch topos
	UNITEST_UPDOWNGRADE_HOT		= 2,				// Client gets conf before toma and walks hand by hand through all swithc topos
	UNITEST_UPDOWNGRADE_HOT_IO	= 3,				// Like HOT but with IO on hybrid volumes (part is 1-mirrored, part is 2)
} __unitest_updowngrade_mode;

TEST_FUNC int unitest_volumes_config(struct NVMeshSystem *sys);
TEST_FUNC int unitest_VolumeReservation(struct NVMeshSystem *sys);

#endif
/*****************************************************************************/
// EOF.
