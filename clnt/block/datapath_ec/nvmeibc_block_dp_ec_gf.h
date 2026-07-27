#ifndef _NVMEIBC_BLOCK_DP_EC_GF_H_
#define _NVMEIBC_BLOCK_DP_EC_GF_H_

#include "nvmeibc_block_dp_ec_gf_defs.h"

const char *nvmeibc_gf_optimization_to_string( int /*enum nvmeibc_gf_optimization_type*/);
int         nvmeibc_gf_optimization_from_string(const char *);

u64 __calculate_parity_P(unsigned int num, const u64* bufA); // Only for simulation
u64 __calculate_parity_Q(unsigned int num, const u64* bufA); // Only for simulation
u32 __calculate_crc32c(   u32 crc_seed, const u8* buffer, u32 num);
u32 __calculate_crc32c_64(u32 crc_seed, const u8* buffer, u32 num);
u32            ec_crc(    u32 crc_seed, const u8 *buffer, u32 num);
enum nvmeibc_gf_optimization_type __gf_choose_functions(enum nvmeibc_gf_optimization_type index);
bool nvmeibc_gf_calc_in_irq_ctx(void);

enum gf_return_val ec_encode_data(       gf_callback *callback, int len, int k, int rows,  u8* data[], u8*   coding[], u32 crc[], u8* data_copy[]);
enum gf_return_val ec_encode_data_update(gf_callback *callback, int len, int k, int vec_i, u8* data[], u8*   coding[], u32 crc[], u8* data_copy);
enum gf_return_val ec_decode_data(       gf_callback *callback, int len, int k, int rows,  u8* data[], u8* new_data[], u32 crc[]);
int nvmeib_allocate_xsave_bufs(void);
void nvmeib_free_xsave_bufs(void);

void nvmeib_fpu_begin(void);
int nvmeib_fpu_end(void);

#endif /*_NVMEIBC_BLOCK_DP_EC_GF_H_ */

