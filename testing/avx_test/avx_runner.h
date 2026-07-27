#ifndef __nvmesh_avx_runner___h
#define __nvmesh_avx_runner___h

#define AVX_REGS_NUM 32
#define AVX512_QWORD_SZ 8
#define AVX2_QWORD_SZ   4
typedef unsigned long (avx2_reg_t)[AVX2_QWORD_SZ];
typedef unsigned long (avx512_reg_t)[AVX512_QWORD_SZ];
typedef int (*avx512_regtest_t)(avx512_reg_t *ra, avx512_reg_t *rb, avx512_reg_t *result);
typedef int (*avx2_regtest_t)(avx2_reg_t *ra, avx2_reg_t *rb, avx2_reg_t *result);

static inline void avx512_compare_registers(avx512_reg_t reg[AVX_REGS_NUM], unsigned long k_mask[AVX_REGS_NUM])
{
	__asm__ ("vpcmpeqw %1, %%zmm0, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[0]): "m" (reg[0]));
	__asm__ ("vpcmpeqw %1, %%zmm1, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[1]): "m" (reg[1]));
	__asm__ ("vpcmpeqw %1, %%zmm2, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[2]): "m" (reg[2]));
	__asm__ ("vpcmpeqw %1, %%zmm3, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[3]): "m" (reg[3]));
	__asm__ ("vpcmpeqw %1, %%zmm4, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[4]): "m" (reg[4]));
	__asm__ ("vpcmpeqw %1, %%zmm5, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[5]): "m" (reg[5]));
	__asm__ ("vpcmpeqw %1, %%zmm6, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[6]): "m" (reg[6]));
	__asm__ ("vpcmpeqw %1, %%zmm7, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[7]): "m" (reg[7]));
	__asm__ ("vpcmpeqw %1, %%zmm8, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[8]): "m" (reg[8]));
	__asm__ ("vpcmpeqw %1, %%zmm9, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[9]): "m" (reg[9]));
	__asm__ ("vpcmpeqw %1, %%zmm10, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[10]): "m" (reg[10]));
	__asm__ ("vpcmpeqw %1, %%zmm11, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[11]): "m" (reg[11]));
	__asm__ ("vpcmpeqw %1, %%zmm12, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[12]): "m" (reg[12]));
	__asm__ ("vpcmpeqw %1, %%zmm13, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[13]): "m" (reg[13]));
	__asm__ ("vpcmpeqw %1, %%zmm14, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[14]): "m" (reg[14]));
	__asm__ ("vpcmpeqw %1, %%zmm15, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[15]): "m" (reg[15]));
	__asm__ ("vpcmpeqw %1, %%zmm16, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[16]): "m" (reg[16]));
	__asm__ ("vpcmpeqw %1, %%zmm17, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[17]): "m" (reg[17]));
	__asm__ ("vpcmpeqw %1, %%zmm18, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[18]): "m" (reg[18]));
	__asm__ ("vpcmpeqw %1, %%zmm19, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[19]): "m" (reg[19]));
	__asm__ ("vpcmpeqw %1, %%zmm20, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[20]): "m" (reg[20]));
	__asm__ ("vpcmpeqw %1, %%zmm21, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[21]): "m" (reg[21]));
	__asm__ ("vpcmpeqw %1, %%zmm22, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[22]): "m" (reg[22]));
	__asm__ ("vpcmpeqw %1, %%zmm23, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[23]): "m" (reg[23]));
	__asm__ ("vpcmpeqw %1, %%zmm24, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[24]): "m" (reg[24]));
	__asm__ ("vpcmpeqw %1, %%zmm25, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[25]): "m" (reg[25]));
	__asm__ ("vpcmpeqw %1, %%zmm26, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[26]): "m" (reg[26]));
	__asm__ ("vpcmpeqw %1, %%zmm27, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[27]): "m" (reg[27]));
	__asm__ ("vpcmpeqw %1, %%zmm28, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[28]): "m" (reg[28]));
	__asm__ ("vpcmpeqw %1, %%zmm29, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[29]): "m" (reg[29]));
	__asm__ ("vpcmpeqw %1, %%zmm30, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[30]): "m" (reg[30]));
	__asm__ ("vpcmpeqw %1, %%zmm31, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[31]): "m" (reg[31]));
}

static inline void avx2_compare_registers(avx2_reg_t reg[AVX_REGS_NUM], unsigned long k_mask[AVX_REGS_NUM])
{
	__asm__ ("vpcmpeqw %1, %%ymm0, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[0]): "m" (reg[0]));
	__asm__ ("vpcmpeqw %1, %%ymm1, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[1]): "m" (reg[1]));
	__asm__ ("vpcmpeqw %1, %%ymm2, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[2]): "m" (reg[2]));
	__asm__ ("vpcmpeqw %1, %%ymm3, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[3]): "m" (reg[3]));
	__asm__ ("vpcmpeqw %1, %%ymm4, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[4]): "m" (reg[4]));
	__asm__ ("vpcmpeqw %1, %%ymm5, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[5]): "m" (reg[5]));
	__asm__ ("vpcmpeqw %1, %%ymm6, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[6]): "m" (reg[6]));
	__asm__ ("vpcmpeqw %1, %%ymm7, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[7]): "m" (reg[7]));
	__asm__ ("vpcmpeqw %1, %%ymm8, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[8]): "m" (reg[8]));
	__asm__ ("vpcmpeqw %1, %%ymm9, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[9]): "m" (reg[9]));
	__asm__ ("vpcmpeqw %1, %%ymm10, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[10]): "m" (reg[10]));
	__asm__ ("vpcmpeqw %1, %%ymm11, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[11]): "m" (reg[11]));
	__asm__ ("vpcmpeqw %1, %%ymm12, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[12]): "m" (reg[12]));
	__asm__ ("vpcmpeqw %1, %%ymm13, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[13]): "m" (reg[13]));
	__asm__ ("vpcmpeqw %1, %%ymm14, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[14]): "m" (reg[14]));
	__asm__ ("vpcmpeqw %1, %%ymm15, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[15]): "m" (reg[15]));
	__asm__ ("vpcmpeqw %1, %%ymm16, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[16]): "m" (reg[16]));
	__asm__ ("vpcmpeqw %1, %%ymm17, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[17]): "m" (reg[17]));
	__asm__ ("vpcmpeqw %1, %%ymm18, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[18]): "m" (reg[18]));
	__asm__ ("vpcmpeqw %1, %%ymm19, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[19]): "m" (reg[19]));
	__asm__ ("vpcmpeqw %1, %%ymm20, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[20]): "m" (reg[20]));
	__asm__ ("vpcmpeqw %1, %%ymm21, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[21]): "m" (reg[21]));
	__asm__ ("vpcmpeqw %1, %%ymm22, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[22]): "m" (reg[22]));
	__asm__ ("vpcmpeqw %1, %%ymm23, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[23]): "m" (reg[23]));
	__asm__ ("vpcmpeqw %1, %%ymm24, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[24]): "m" (reg[24]));
	__asm__ ("vpcmpeqw %1, %%ymm25, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[25]): "m" (reg[25]));
	__asm__ ("vpcmpeqw %1, %%ymm26, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[26]): "m" (reg[26]));
	__asm__ ("vpcmpeqw %1, %%ymm27, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[27]): "m" (reg[27]));
	__asm__ ("vpcmpeqw %1, %%ymm28, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[28]): "m" (reg[28]));
	__asm__ ("vpcmpeqw %1, %%ymm29, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[29]): "m" (reg[29]));
	__asm__ ("vpcmpeqw %1, %%ymm30, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[30]): "m" (reg[30]));
	__asm__ ("vpcmpeqw %1, %%ymm31, %%k1\n\t" "kmovq %%k1, %0\n\t" :"=m" (k_mask[31]): "m" (reg[31]));
}

static inline void avx512_load_registers(avx512_reg_t reg[AVX_REGS_NUM])
{
	__asm__ ("vmovdqu64 %0, %%zmm0\n\t" ::"m" (reg[0]));
	__asm__ ("vmovdqu64 %0, %%zmm1\n\t" ::"m" (reg[1]));
	__asm__ ("vmovdqu64 %0, %%zmm2\n\t" ::"m" (reg[2]));
	__asm__ ("vmovdqu64 %0, %%zmm3\n\t" ::"m" (reg[3]));
	__asm__ ("vmovdqu64 %0, %%zmm4\n\t" ::"m" (reg[4]));
	__asm__ ("vmovdqu64 %0, %%zmm5\n\t" ::"m" (reg[5]));
	__asm__ ("vmovdqu64 %0, %%zmm6\n\t" ::"m" (reg[6]));
	__asm__ ("vmovdqu64 %0, %%zmm7\n\t" ::"m" (reg[7]));
	__asm__ ("vmovdqu64 %0, %%zmm8\n\t" ::"m" (reg[8]));
	__asm__ ("vmovdqu64 %0, %%zmm9\n\t" ::"m" (reg[9]));
	__asm__ ("vmovdqu64 %0, %%zmm10\n\t" ::"m" (reg[10]));
	__asm__ ("vmovdqu64 %0, %%zmm11\n\t" ::"m" (reg[11]));
	__asm__ ("vmovdqu64 %0, %%zmm12\n\t" ::"m" (reg[12]));
	__asm__ ("vmovdqu64 %0, %%zmm13\n\t" ::"m" (reg[13]));
	__asm__ ("vmovdqu64 %0, %%zmm14\n\t" ::"m" (reg[14]));
	__asm__ ("vmovdqu64 %0, %%zmm15\n\t" ::"m" (reg[15]));
	__asm__ ("vmovdqu64 %0, %%zmm16\n\t" ::"m" (reg[16]));
	__asm__ ("vmovdqu64 %0, %%zmm17\n\t" ::"m" (reg[17]));
	__asm__ ("vmovdqu64 %0, %%zmm18\n\t" ::"m" (reg[18]));
	__asm__ ("vmovdqu64 %0, %%zmm19\n\t" ::"m" (reg[19]));
	__asm__ ("vmovdqu64 %0, %%zmm20\n\t" ::"m" (reg[20]));
	__asm__ ("vmovdqu64 %0, %%zmm21\n\t" ::"m" (reg[21]));
	__asm__ ("vmovdqu64 %0, %%zmm22\n\t" ::"m" (reg[22]));
	__asm__ ("vmovdqu64 %0, %%zmm23\n\t" ::"m" (reg[23]));
	__asm__ ("vmovdqu64 %0, %%zmm24\n\t" ::"m" (reg[24]));
	__asm__ ("vmovdqu64 %0, %%zmm25\n\t" ::"m" (reg[25]));
	__asm__ ("vmovdqu64 %0, %%zmm26\n\t" ::"m" (reg[26]));
	__asm__ ("vmovdqu64 %0, %%zmm27\n\t" ::"m" (reg[27]));
	__asm__ ("vmovdqu64 %0, %%zmm28\n\t" ::"m" (reg[28]));
	__asm__ ("vmovdqu64 %0, %%zmm29\n\t" ::"m" (reg[29]));
	__asm__ ("vmovdqu64 %0, %%zmm30\n\t" ::"m" (reg[30]));
	__asm__ ("vmovdqu64 %0, %%zmm31\n\t" ::"m" (reg[31]));
}

static inline void avx2_load_registers(avx2_reg_t reg[AVX_REGS_NUM])
{
	__asm__ ("vmovdqu64 %0, %%ymm0\n\t" ::"m" (reg[0]));
	__asm__ ("vmovdqu64 %0, %%ymm1\n\t" ::"m" (reg[1]));
	__asm__ ("vmovdqu64 %0, %%ymm2\n\t" ::"m" (reg[2]));
	__asm__ ("vmovdqu64 %0, %%ymm3\n\t" ::"m" (reg[3]));
	__asm__ ("vmovdqu64 %0, %%ymm4\n\t" ::"m" (reg[4]));
	__asm__ ("vmovdqu64 %0, %%ymm5\n\t" ::"m" (reg[5]));
	__asm__ ("vmovdqu64 %0, %%ymm6\n\t" ::"m" (reg[6]));
	__asm__ ("vmovdqu64 %0, %%ymm7\n\t" ::"m" (reg[7]));
	__asm__ ("vmovdqu64 %0, %%ymm8\n\t" ::"m" (reg[8]));
	__asm__ ("vmovdqu64 %0, %%ymm9\n\t" ::"m" (reg[9]));
	__asm__ ("vmovdqu64 %0, %%ymm10\n\t" ::"m" (reg[10]));
	__asm__ ("vmovdqu64 %0, %%ymm11\n\t" ::"m" (reg[11]));
	__asm__ ("vmovdqu64 %0, %%ymm12\n\t" ::"m" (reg[12]));
	__asm__ ("vmovdqu64 %0, %%ymm13\n\t" ::"m" (reg[13]));
	__asm__ ("vmovdqu64 %0, %%ymm14\n\t" ::"m" (reg[14]));
	__asm__ ("vmovdqu64 %0, %%ymm15\n\t" ::"m" (reg[15]));
	__asm__ ("vmovdqu64 %0, %%ymm16\n\t" ::"m" (reg[16]));
	__asm__ ("vmovdqu64 %0, %%ymm17\n\t" ::"m" (reg[17]));
	__asm__ ("vmovdqu64 %0, %%ymm18\n\t" ::"m" (reg[18]));
	__asm__ ("vmovdqu64 %0, %%ymm19\n\t" ::"m" (reg[19]));
	__asm__ ("vmovdqu64 %0, %%ymm20\n\t" ::"m" (reg[20]));
	__asm__ ("vmovdqu64 %0, %%ymm21\n\t" ::"m" (reg[21]));
	__asm__ ("vmovdqu64 %0, %%ymm22\n\t" ::"m" (reg[22]));
	__asm__ ("vmovdqu64 %0, %%ymm23\n\t" ::"m" (reg[23]));
	__asm__ ("vmovdqu64 %0, %%ymm24\n\t" ::"m" (reg[24]));
	__asm__ ("vmovdqu64 %0, %%ymm25\n\t" ::"m" (reg[25]));
	__asm__ ("vmovdqu64 %0, %%ymm26\n\t" ::"m" (reg[26]));
	__asm__ ("vmovdqu64 %0, %%ymm27\n\t" ::"m" (reg[27]));
	__asm__ ("vmovdqu64 %0, %%ymm28\n\t" ::"m" (reg[28]));
	__asm__ ("vmovdqu64 %0, %%ymm29\n\t" ::"m" (reg[29]));
	__asm__ ("vmovdqu64 %0, %%ymm30\n\t" ::"m" (reg[30]));
	__asm__ ("vmovdqu64 %0, %%ymm31\n\t" ::"m" (reg[31]));
}

#endif //__nvmesh_avx_runner___h
