#pragma once

#include "nvmeibc_block_dp_ec_gf_defs.h"

#ifdef NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
enum gf_return_val ec_encode_data_arm_optimized(int len, int k, int rows, unsigned char ** data, unsigned char ** coding, unsigned int *crc, unsigned char **data_copy);
enum gf_return_val ec_encode_data_update_arm_optimized(int len, int k, int vec_i, unsigned char **data, unsigned char **coding, unsigned int *crc, unsigned char *data_copy);
enum gf_return_val ec_decode_data_arm_optimized(int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, unsigned int *crc);

#ifdef __KERNEL__
	#if KS_CRC32C_USES_SIZE_T
u32 		ec_crc_arm_optimized(u32 init_crc, const void *buf, size_t len);
	#else
u32 		ec_crc_arm_optimized(u32 init_crc, const void *buf, unsigned int len);
	#endif
#else
unsigned int 	ec_crc_arm_optimized(unsigned int init_crc, const u8 *buf, unsigned int len);
#endif

#endif
