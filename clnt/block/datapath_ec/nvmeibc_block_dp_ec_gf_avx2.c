/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 *
 * x86_64 AVX2 GF(EC) helpers — C port of nvmeibc_block_dp_ec_gf_asm.S_shipped using
 * intrinsics from nvmeibc_x86_avx2_intrin.h (kernel) or <immintrin.h> (userspace).
 * Requires -mavx2 -msse4.2 (or equivalent).
 */

#if defined(__x86_64__) || defined(__i386__)

#include "common/kr_incs.h"
#if defined(__KERNEL__)
#include "nvmeib_public.h"
#endif
#include "nvmeibc_block_dp_ec_gf_defs.h"
#include "nvmeibc_block_dp_ec_gf.h"
#include "nvmeibc_block_dp_ec_gf_avx2.h"

#include "nvmeibc_x86_avx2_intrin.h"

#if defined(__KERNEL__)
#include <linux/export.h>
#endif

/* A faithful port of the shipped asm in nvmeibc_block_dp_ec_gf_asm.S_shipped to C + AVX2 intrinsics.
This removes the need for mucking around with the Kbuild and objtool non-standard asm handling.
Tested using unittests in uni_scenarios/uni_scenario_gf.c.
*/

struct raid6_sse_constants {
	u64 x1d[4];
};

extern const struct raid6_sse_constants poly_constants;

/* Version tag for nvmeibc_main / diagnostics (was gf_asm_count in .S). */
#if defined(__KERNEL__) && defined(__x86_64__)
int gf_asm_count = 10;
EXPORT_SYMBOL(gf_asm_count);
#elif defined(__x86_64__)
int gf_asm_count = 10;
#endif

#define AVX2_ITER_SIZE 128

/*
 * The shipped asm used vmovntdqa (non-temporal aligned load) and vmovntdq
 * (non-temporal aligned store). These intrinsics do the same.
 */
static inline __m256i loadu(const void *p)
{
	return _mm256_loadu_si256((const __m256i *)p);
}

static inline void stream_store(void *p, __m256i v)
{
	_mm256_storeu_si256((__m256i *)p, v);
}

static inline void prefetch_nta(const void *p)
{
	__builtin_prefetch(p, 0, 0);
}

/*
 * CRC32C over one ymm (32 bytes / four u64 lanes).
 * Direct inline asm to match the shipped _crc32_32 macro exactly:
 *   vmovq       xreg, crctmp        # u64[0]
 *   crc32q      crctmp, dest
 *   vextracti128 $1, yreg, t1L      # u128[1]
 *   vpextrq     $1, xreg, crctmp    # u64[1]
 *   crc32q      crctmp, dest
 *   vmovq       t1L, crctmp         # u64[2]
 *   crc32q      crctmp, dest
 *   vpextrq     $1, t1L, crctmp     # u64[3]
 *   crc32q      crctmp, dest
 */
static inline u32 crc32c_ymm(__m256i v, u32 crc)
{
	__m128i lo = _mm256_castsi256_si128(v);
	__m128i hi = _mm256_extracti128_si256(v, 1);

	crc = _mm_crc32_u64(crc, (u64)_mm_cvtsi128_si64(lo));
	crc = _mm_crc32_u64(crc, (u64)_mm_extract_epi64(lo, 1));
	crc = _mm_crc32_u64(crc, (u64)_mm_cvtsi128_si64(hi));
	crc = _mm_crc32_u64(crc, (u64)_mm_extract_epi64(hi, 1));

	return (u32)crc;
}

void ec_encode_data_p_avx2(int len, int rows, unsigned char **data, unsigned char **coding,
			 u32 *crcp, unsigned char **data_copy)
{
	__m256i P0, P1, P2, P3, D0, D1, D2, D3;
	long j;
	int i;
	unsigned char *base;

	nvmeib_fpu_begin();

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		P0 = _mm256_setzero_si256();
		P1 = _mm256_setzero_si256();
		P2 = _mm256_setzero_si256();
		P3 = _mm256_setzero_si256();

		for (i = rows - 1; i >= 0; i--) {
			base = data[i];
			prefetch_nta(base + j + 128);
			prefetch_nta(base + j + 196);
			D0 = loadu(base + j);
			D1 = loadu(base + j + 32);
			D2 = loadu(base + j + 64);
			D3 = loadu(base + j + 96);
			P0 = _mm256_xor_si256(P0, D0);
			P1 = _mm256_xor_si256(P1, D1);
			P2 = _mm256_xor_si256(P2, D2);
			P3 = _mm256_xor_si256(P3, D3);

			if (data_copy && data_copy[i]) {
				base = data_copy[i];
				stream_store(base + j, D0);
				stream_store(base + j + 32, D1);
				stream_store(base + j + 64, D2);
				stream_store(base + j + 96, D3);
			}

			if (crcp) {
				u32 c = crcp[i];

				c = crc32c_ymm(D0, c);
				c = crc32c_ymm(D1, c);
				c = crc32c_ymm(D2, c);
				c = crc32c_ymm(D3, c);
				crcp[i] = c;
			}
		}

		base = coding[0];
		stream_store(base + j, P0);
		stream_store(base + j + 32, P1);
		stream_store(base + j + 64, P2);
		stream_store(base + j + 96, P3);

		if (crcp) {
			u32 c = crcp[rows];

			c = crc32c_ymm(P0, c);
			c = crc32c_ymm(P1, c);
			c = crc32c_ymm(P2, c);
			c = crc32c_ymm(P3, c);
			crcp[rows] = c;
		}
	}

	nvmeib_fpu_end();
}

/* Matches asm _one_Q / _one_vec Q part: Q' = f(LD, Q). */
static inline void gf_one_Q_step(__m256i *Q, __m256i LD, __m256i poly)
{
	__m256i Qv = *Q;
	__m256i z = _mm256_setzero_si256();
	__m256i t1 = _mm256_cmpgt_epi8(z, Qv);
	__m256i Q2 = _mm256_add_epi8(Qv, Qv);

	t1 = _mm256_and_si256(poly, t1);
	t1 = _mm256_xor_si256(Q2, t1);
	*Q = _mm256_xor_si256(LD, t1);
}

static inline void gf_one_vec_step(__m256i *P, __m256i *Q, __m256i LD, __m256i poly)
{
	__m256i Qv = *Q;
	__m256i Q2;
	__m256i z = _mm256_setzero_si256();
	__m256i t1 = _mm256_cmpgt_epi8(z, Qv);

	*P = _mm256_xor_si256(*P, LD);
	Q2 = _mm256_add_epi8(Qv, Qv);
	t1 = _mm256_and_si256(poly, t1);
	t1 = _mm256_xor_si256(Q2, t1);
	*Q = _mm256_xor_si256(LD, t1);
}

void ec_encode_data_q_avx2(int len, int rows, unsigned char **data, unsigned char **coding,
			   u32 *crcp, unsigned char **data_copy)
{
	__m256i Q0, Q1, Q2, Q3, D0, D1, D2, D3;
	__m256i poly;
	long j;
	int i;
	unsigned char *base;

	nvmeib_fpu_begin();
	poly = loadu(&poly_constants);

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		i = rows - 1;
		base = data[i];
		prefetch_nta(base + j + 128);
		prefetch_nta(base + j + 192);
		Q0 = loadu(base + j);
		Q1 = loadu(base + j + 32);
		Q2 = loadu(base + j + 64);
		Q3 = loadu(base + j + 96);

		if (data_copy && data_copy[i]) {
			base = data_copy[i];
			stream_store(base + j, Q0);
			stream_store(base + j + 32, Q1);
			stream_store(base + j + 64, Q2);
			stream_store(base + j + 96, Q3);
		}

		if (crcp) {
			u32 c = crcp[i];

			c = crc32c_ymm(Q0, c);
			c = crc32c_ymm(Q1, c);
			c = crc32c_ymm(Q2, c);
			c = crc32c_ymm(Q3, c);
			crcp[i] = c;
		}

		for (i = rows - 2; i >= 0; i--) {
			base = data[i];
			prefetch_nta(base + j + 128);
			prefetch_nta(base + j + 192);
			D0 = loadu(base + j);
			D1 = loadu(base + j + 32);
			D2 = loadu(base + j + 64);
			D3 = loadu(base + j + 96);
			gf_one_Q_step(&Q0, D0, poly);
			gf_one_Q_step(&Q1, D1, poly);
			gf_one_Q_step(&Q2, D2, poly);
			gf_one_Q_step(&Q3, D3, poly);

			if (data_copy && data_copy[i]) {
				base = data_copy[i];
				stream_store(base + j, D0);
				stream_store(base + j + 32, D1);
				stream_store(base + j + 64, D2);
				stream_store(base + j + 96, D3);
			}

			if (crcp) {
				u32 c = crcp[i];

				c = crc32c_ymm(D0, c);
				c = crc32c_ymm(D1, c);
				c = crc32c_ymm(D2, c);
				c = crc32c_ymm(D3, c);
				crcp[i] = c;
			}
		}

		base = coding[1];
		stream_store(base + j, Q0);
		stream_store(base + j + 32, Q1);
		stream_store(base + j + 64, Q2);
		stream_store(base + j + 96, Q3);

		if (crcp) {
			u32 c = crcp[rows + 1];

			c = crc32c_ymm(Q0, c);
			c = crc32c_ymm(Q1, c);
			c = crc32c_ymm(Q2, c);
			c = crc32c_ymm(Q3, c);
			crcp[rows + 1] = c;
		}
	}

	nvmeib_fpu_end();
}

void ec_encode_data_pq_avx2(int len, int rows, unsigned char **data, unsigned char **coding,
			    u32 *crcp, unsigned char **data_copy)
{
	__m256i P0, P1, P2, P3, Q0, Q1, Q2, Q3, D0, D1, D2, D3;
	__m256i poly;
	long j;
	int i;
	unsigned char *base;

	nvmeib_fpu_begin();
	poly = loadu(&poly_constants);

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		i = rows - 1;
		base = data[i];
		prefetch_nta(base + j + 128);
		prefetch_nta(base + j + 192);
		Q0 = loadu(base + j);
		Q1 = loadu(base + j + 32);
		Q2 = loadu(base + j + 64);
		Q3 = loadu(base + j + 96);
		P0 = Q0;
		P1 = Q1;
		P2 = Q2;
		P3 = Q3;

		if (data_copy && data_copy[i]) {
			base = data_copy[i];
			stream_store(base + j, Q0);
			stream_store(base + j + 32, Q1);
			stream_store(base + j + 64, Q2);
			stream_store(base + j + 96, Q3);
		}

		if (crcp) {
			u32 c = crcp[i];

			c = crc32c_ymm(Q0, c);
			c = crc32c_ymm(Q1, c);
			c = crc32c_ymm(Q2, c);
			c = crc32c_ymm(Q3, c);
			crcp[i] = c;
		}

		for (i = rows - 2; i >= 0; i--) {
			base = data[i];
			prefetch_nta(base + j + 128);
			prefetch_nta(base + j + 192);
			D0 = loadu(base + j);
			D1 = loadu(base + j + 32);
			D2 = loadu(base + j + 64);
			D3 = loadu(base + j + 96);
			gf_one_vec_step(&P0, &Q0, D0, poly);
			gf_one_vec_step(&P1, &Q1, D1, poly);
			gf_one_vec_step(&P2, &Q2, D2, poly);
			gf_one_vec_step(&P3, &Q3, D3, poly);

			if (data_copy && data_copy[i]) {
				base = data_copy[i];
				stream_store(base + j, D0);
				stream_store(base + j + 32, D1);
				stream_store(base + j + 64, D2);
				stream_store(base + j + 96, D3);
			}

			if (crcp) {
				u32 c = crcp[i];

				c = crc32c_ymm(D0, c);
				c = crc32c_ymm(D1, c);
				c = crc32c_ymm(D2, c);
				c = crc32c_ymm(D3, c);
				crcp[i] = c;
			}
		}

		base = coding[0];
		stream_store(base + j, P0);
		stream_store(base + j + 32, P1);
		stream_store(base + j + 64, P2);
		stream_store(base + j + 96, P3);

		base = coding[1];
		stream_store(base + j, Q0);
		stream_store(base + j + 32, Q1);
		stream_store(base + j + 64, Q2);
		stream_store(base + j + 96, Q3);

		if (crcp) {
			u32 c = crcp[rows];

			c = crc32c_ymm(P0, c);
			c = crc32c_ymm(P1, c);
			c = crc32c_ymm(P2, c);
			c = crc32c_ymm(P3, c);
			crcp[rows] = c;
			c = crcp[rows + 1];
			c = crc32c_ymm(Q0, c);
			c = crc32c_ymm(Q1, c);
			c = crc32c_ymm(Q2, c);
			c = crc32c_ymm(Q3, c);
			crcp[rows + 1] = c;
		}
	}

	nvmeib_fpu_end();
}

static inline void gf_one_upd(__m256i *Q, __m256i poly)
{
	__m256i Qv = *Q;
	__m256i z = _mm256_setzero_si256();
	__m256i t1 = _mm256_cmpgt_epi8(z, Qv);
	__m256i Q2 = _mm256_add_epi8(Qv, Qv);
	__m256i temp = _mm256_and_si256(poly, t1);

	*Q = _mm256_xor_si256(Q2, temp);
}

enum gf_return_val ec_encode_data_update_avx2(int len, int k, int vec_i, unsigned char **data,
					      unsigned char **coding, u32 *crcp,
					      unsigned char *data_copy)
{
	__m256i P0, P1, P2, P3, Q0, Q1, Q2, Q3, temp;
	__m256i poly;
	long j;
	int i;
	unsigned char *base;
	nvmeib_fpu_begin();
	poly = loadu(&poly_constants);

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		base = data[1];
		P0 = loadu(base + j);
		P1 = loadu(base + j + 32);
		P2 = loadu(base + j + 64);
		P3 = loadu(base + j + 96);

		if (data_copy) {
			stream_store(data_copy + j, P0);
			stream_store(data_copy + j + 32, P1);
			stream_store(data_copy + j + 64, P2);
			stream_store(data_copy + j + 96, P3);
		}

		if (crcp) {
			u32 c = crcp[0];

			c = crc32c_ymm(P0, c);
			c = crc32c_ymm(P1, c);
			c = crc32c_ymm(P2, c);
			c = crc32c_ymm(P3, c);
			crcp[0] = c;
		}

		base = data[0];
		temp = loadu(base + j);
		P0 = _mm256_xor_si256(temp, P0);
		temp = loadu(base + j + 32);
		P1 = _mm256_xor_si256(temp, P1);
		temp = loadu(base + j + 64);
		P2 = _mm256_xor_si256(temp, P2);
		temp = loadu(base + j + 96);
		P3 = _mm256_xor_si256(temp, P3);

		if (k > 1) {
			Q0 = P0;
			Q1 = P1;
			Q2 = P2;
			Q3 = P3;

			for (i = vec_i - 1; i >= 0; i--) {
				gf_one_upd(&Q0, poly);
				gf_one_upd(&Q1, poly);
				gf_one_upd(&Q2, poly);
				gf_one_upd(&Q3, poly);
			}

			base = data[3];
			temp = loadu(base + j);
			Q0 = _mm256_xor_si256(temp, Q0);
			temp = loadu(base + j + 32);
			Q1 = _mm256_xor_si256(temp, Q1);
			temp = loadu(base + j + 64);
			Q2 = _mm256_xor_si256(temp, Q2);
			temp = loadu(base + j + 96);
			Q3 = _mm256_xor_si256(temp, Q3);

			base = coding[1];
			stream_store(base + j, Q0);
			stream_store(base + j + 32, Q1);
			stream_store(base + j + 64, Q2);
			stream_store(base + j + 96, Q3);

			if (crcp) {
				u32 c = crcp[2];

				c = crc32c_ymm(Q0, c);
				c = crc32c_ymm(Q1, c);
				c = crc32c_ymm(Q2, c);
				c = crc32c_ymm(Q3, c);
				crcp[2] = c;
			}
		}

		base = data[2];
		temp = loadu(base + j);
		P0 = _mm256_xor_si256(temp, P0);
		temp = loadu(base + j + 32);
		P1 = _mm256_xor_si256(temp, P1);
		temp = loadu(base + j + 64);
		P2 = _mm256_xor_si256(temp, P2);
		temp = loadu(base + j + 96);
		P3 = _mm256_xor_si256(temp, P3);

		base = coding[0];
		stream_store(base + j, P0);
		stream_store(base + j + 32, P1);
		stream_store(base + j + 64, P2);
		stream_store(base + j + 96, P3);

		if (crcp) {
			u32 c = crcp[1];

			c = crc32c_ymm(P0, c);
			c = crc32c_ymm(P1, c);
			c = crc32c_ymm(P2, c);
			c = crc32c_ymm(P3, c);
			crcp[1] = c;
		}
	}

	nvmeib_fpu_end();
	return GF_SUCCESS;
}

void ec_decode_data_p_avx2(int len, int rows, int d0, unsigned char **data,
			 unsigned char **new_data, u32 *crcp)
{
	__m256i P0, P1, P2, P3, temp;
	long j;
	int i;
	unsigned char *base;

	nvmeib_fpu_begin();

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		base = data[rows];
		P0 = loadu(base + j);
		P1 = loadu(base + j + 32);
		P2 = loadu(base + j + 64);
		P3 = loadu(base + j + 96);

		for (i = rows - 1; i >= 0; i--) {
			if (i == d0)
				continue;
			base = data[i];
			temp = loadu(base + j);
			P0 = _mm256_xor_si256(P0, temp);
			temp = loadu(base + j + 32);
			P1 = _mm256_xor_si256(P1, temp);
			temp = loadu(base + j + 64);
			P2 = _mm256_xor_si256(P2, temp);
			temp = loadu(base + j + 96);
			P3 = _mm256_xor_si256(P3, temp);
		}

		base = new_data[0];
		stream_store(base + j, P0);
		stream_store(base + j + 32, P1);
		stream_store(base + j + 64, P2);
		stream_store(base + j + 96, P3);

		if (crcp) {
			u32 c = crcp[0];

			c = crc32c_ymm(P0, c);
			c = crc32c_ymm(P1, c);
			c = crc32c_ymm(P2, c);
			c = crc32c_ymm(P3, c);
			crcp[0] = c;
		}
	}

	nvmeib_fpu_end();
}

static inline void gf_one_div(__m256i *Q, __m256i PolyRot)
{
	__m256i one = _mm256_set1_epi8(1);
	__m256i Qv = *Q;
	__m256i lb = _mm256_and_si256(Qv, one);
	__m256i mask = _mm256_cmpeq_epi8(lb, one);
	__m256i temp = _mm256_and_si256(PolyRot, mask);

	Qv = _mm256_andnot_si256(one, Qv);
	Qv = _mm256_srli_epi16(Qv, 1);
	*Q = _mm256_xor_si256(Qv, temp);
}

void ec_decode_data_q_avx2(int len, int rows, int d0, unsigned char **data,
			   unsigned char **new_data, u32 *crcp)
{
	__m256i Q0, Q1, Q2, Q3, temp;
	__m256i poly;
	__m256i PolyRot;
	int i, bit;
	long j;
	unsigned char *base;

	nvmeib_fpu_begin();
	poly = loadu(&poly_constants);
	PolyRot = _mm256_set1_epi8((char)0x8e);

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		Q0 = _mm256_setzero_si256();
		Q1 = _mm256_setzero_si256();
		Q2 = _mm256_setzero_si256();
		Q3 = _mm256_setzero_si256();

		i = rows - 1;
		while (1) {
			if (i != d0) {
				base = data[i];
				temp = loadu(base + j);
				Q0 = _mm256_xor_si256(Q0, temp);
				temp = loadu(base + j + 32);
				Q1 = _mm256_xor_si256(Q1, temp);
				temp = loadu(base + j + 64);
				Q2 = _mm256_xor_si256(Q2, temp);
				temp = loadu(base + j + 96);
				Q3 = _mm256_xor_si256(Q3, temp);
			}
			i--;
			if (i < 0)
				break;
			gf_one_upd(&Q0, poly);
			gf_one_upd(&Q1, poly);
			gf_one_upd(&Q2, poly);
			gf_one_upd(&Q3, poly);
		}

		base = data[rows + 1]; /* Q parity; P is data[rows] */
		temp = loadu(base + j);
		Q0 = _mm256_xor_si256(Q0, temp);
		temp = loadu(base + j + 32);
		Q1 = _mm256_xor_si256(Q1, temp);
		temp = loadu(base + j + 64);
		Q2 = _mm256_xor_si256(Q2, temp);
		temp = loadu(base + j + 96);
		Q3 = _mm256_xor_si256(Q3, temp);

		for (bit = d0 - 1; bit >= 0; bit--) {
			gf_one_div(&Q0, PolyRot);
			gf_one_div(&Q1, PolyRot);
			gf_one_div(&Q2, PolyRot);
			gf_one_div(&Q3, PolyRot);
		}

		base = new_data[0];
		stream_store(base + j, Q0);
		stream_store(base + j + 32, Q1);
		stream_store(base + j + 64, Q2);
		stream_store(base + j + 96, Q3);

		if (crcp) {
			u32 c = crcp[0];

			c = crc32c_ymm(Q0, c);
			c = crc32c_ymm(Q1, c);
			c = crc32c_ymm(Q2, c);
			c = crc32c_ymm(Q3, c);
			crcp[0] = c;
		}
	}

	nvmeib_fpu_end();
}

void ec_decode_data_pq_avx2_asm(int len, int rows, int d0, int d1, unsigned char **data,
				unsigned char **new_data, u32 *crcp, unsigned char factor)
{
	__m256i P0, P1, P2, P3, Q0, Q1, Q2, Q3, R0, R1, R2, R3, temp;
	__m256i poly;
	int i, bit;
	long j;
	unsigned char *base;
	unsigned char f = factor;

	nvmeib_fpu_begin();
	poly = loadu(&poly_constants);

	for (j = 0; j < len; j += AVX2_ITER_SIZE) {
		base = data[rows];
		P0 = loadu(base + j);
		P1 = loadu(base + j + 32);
		P2 = loadu(base + j + 64);
		P3 = loadu(base + j + 96);
		Q0 = _mm256_setzero_si256();
		Q1 = _mm256_setzero_si256();
		Q2 = _mm256_setzero_si256();
		Q3 = _mm256_setzero_si256();

		i = rows - 1;
		while (1) {
			if (i != d0 && i != d1) {
				base = data[i];
				temp = loadu(base + j);
				Q0 = _mm256_xor_si256(Q0, temp);
				P0 = _mm256_xor_si256(P0, temp);
				temp = loadu(base + j + 32);
				Q1 = _mm256_xor_si256(Q1, temp);
				P1 = _mm256_xor_si256(P1, temp);
				temp = loadu(base + j + 64);
				Q2 = _mm256_xor_si256(Q2, temp);
				P2 = _mm256_xor_si256(P2, temp);
				temp = loadu(base + j + 96);
				Q3 = _mm256_xor_si256(Q3, temp);
				P3 = _mm256_xor_si256(P3, temp);
			}
			i--;
			if (i < 0)
				break;
			gf_one_upd(&Q0, poly);
			gf_one_upd(&Q1, poly);
			gf_one_upd(&Q2, poly);
			gf_one_upd(&Q3, poly);
		}

		base = data[rows + 1];
		temp = loadu(base + j);
		Q0 = _mm256_xor_si256(Q0, temp);
		temp = loadu(base + j + 32);
		Q1 = _mm256_xor_si256(Q1, temp);
		temp = loadu(base + j + 64);
		Q2 = _mm256_xor_si256(Q2, temp);
		temp = loadu(base + j + 96);
		Q3 = _mm256_xor_si256(Q3, temp);

		R0 = P0;
		R1 = P1;
		R2 = P2;
		R3 = P3;

		for (i = d1 - 1; i >= 0; i--) {
			gf_one_upd(&R0, poly);
			gf_one_upd(&R1, poly);
			gf_one_upd(&R2, poly);
			gf_one_upd(&R3, poly);
		}

		R0 = _mm256_xor_si256(R0, Q0);
		R1 = _mm256_xor_si256(R1, Q1);
		R2 = _mm256_xor_si256(R2, Q2);
		R3 = _mm256_xor_si256(R3, Q3);

		Q0 = _mm256_setzero_si256();
		Q1 = _mm256_setzero_si256();
		Q2 = _mm256_setzero_si256();
		Q3 = _mm256_setzero_si256();

		f = factor;
		for (bit = 8; bit >= 0; bit--) {
			unsigned b = f & 1;

			f >>= 1;
			if (b) {
				Q0 = _mm256_xor_si256(Q0, R0);
				Q1 = _mm256_xor_si256(Q1, R1);
				Q2 = _mm256_xor_si256(Q2, R2);
				Q3 = _mm256_xor_si256(Q3, R3);
			}
			gf_one_upd(&R0, poly);
			gf_one_upd(&R1, poly);
			gf_one_upd(&R2, poly);
			gf_one_upd(&R3, poly);
		}

		base = new_data[0];
		stream_store(base + j, Q0);
		stream_store(base + j + 32, Q1);
		stream_store(base + j + 64, Q2);
		stream_store(base + j + 96, Q3);

		if (new_data[1]) {
			P0 = _mm256_xor_si256(Q0, P0);
			P1 = _mm256_xor_si256(Q1, P1);
			P2 = _mm256_xor_si256(Q2, P2);
			P3 = _mm256_xor_si256(Q3, P3);
			base = new_data[1];
			stream_store(base + j, P0);
			stream_store(base + j + 32, P1);
			stream_store(base + j + 64, P2);
			stream_store(base + j + 96, P3);
		}

		if (crcp) {
			u32 c = crcp[0];

			c = crc32c_ymm(Q0, c);
			c = crc32c_ymm(Q1, c);
			c = crc32c_ymm(Q2, c);
			c = crc32c_ymm(Q3, c);
			crcp[0] = c;
			c = crcp[1];
			c = crc32c_ymm(P0, c);
			c = crc32c_ymm(P1, c);
			c = crc32c_ymm(P2, c);
			c = crc32c_ymm(P3, c);
			crcp[1] = c;
		}
	}

	nvmeib_fpu_end();
}

#if defined(__KERNEL__) && defined(__x86_64__)
void nvmeib_save_avx256(void *area)
{
	unsigned long long mask = ~0ULL;

	asm volatile("xsave (%0)" : : "r"(area), "a"(mask), "d"(mask) : "memory");
}

void nvmeib_restore_avx256(void *area)
{
	unsigned long long mask = ~0ULL;

	asm volatile("xrstor (%0)" : : "r"(area), "a"(mask), "d"(mask) : "memory");
}
#endif /* __KERNEL__ && __x86_64__ */

#endif /* __x86_64__ || __i386__ */
