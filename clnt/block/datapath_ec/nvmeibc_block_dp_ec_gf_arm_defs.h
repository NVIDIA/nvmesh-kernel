#ifndef _NVMEIBC_BLOCK_DP_EC_GF_ARM_DEFS_H_
#define _NVMEIBC_BLOCK_DP_EC_GF_ARM_DEFS_H_

/*  __ARM_NEON is defined when gcc is run with -march=...+simd
 * All CUs that include this file must have this set. This includes:
 * - nvmeibc_block_dp_ec_gf.o
 * - nvmeibc_block_dp_ec_gf_arm_neon4.o
 * - nvmeibc_block_dp_ec_gf_arm_recov_neon.o
 * - nvmeibc_block_dp_ec_gf_arm_um.o
*/
#if defined(__ARM_NEON)
#define NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
#endif

#endif//_NVMEIBC_BLOCK_DP_EC_GF_ARM_DEFS_H_
