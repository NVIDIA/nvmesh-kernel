// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "nvmeibc_block_dp_ec_gf_arm_defs.h"

#ifdef NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION

void raid6_neon1_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs);
void raid6_neon1_xor_syndrome_real(int disks, int start, int stop,
				    unsigned long bytes, void **ptrs);
void raid6_neon2_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs);
void raid6_neon2_xor_syndrome_real(int disks, int start, int stop,
				    unsigned long bytes, void **ptrs);
void raid6_neon4_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs);
void raid6_neon4_xor_syndrome_real(int disks, int start, int stop,
				    unsigned long bytes, void **ptrs);
void raid6_neon8_gen_syndrome_real(int disks, unsigned long bytes, void **ptrs);
void raid6_neon8_xor_syndrome_real(int disks, int start, int stop,
				    unsigned long bytes, void **ptrs);
void raid6_2data_recov_neon(int disks, size_t bytes, int faila, int failb, void **ptrs);
void raid6_datap_recov_neon(int disks, size_t bytes, int faila, void **ptrs);

#endif
