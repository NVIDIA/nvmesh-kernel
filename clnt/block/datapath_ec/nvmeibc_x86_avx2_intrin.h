/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
 *
 * Subset of x86 AVX2 / SSE4.2 intrinsics used by nvmeibc_block_dp_ec_gf_avx2.c, for Linux
 * kernel builds where kbuild's -nostdinc hides <immintrin.h>. Userspace uses the compiler's
 * system headers.
 *
 * Derived from GCC's immintrin.h / avxintrin.h / avx2intrin.h / emmintrin.h / smmintrin.h /
 * xmmintrin.h (GPLv3 + GCC Runtime Library Exception).
 */

#ifndef NVMEIBC_X86_AVX2_INTRIN_H
#define NVMEIBC_X86_AVX2_INTRIN_H

#if !defined(__x86_64__) && !defined(__i386__)
#error "nvmeibc_x86_avx2_intrin.h is x86 only"
#endif

#if defined(__KERNEL__) && defined(__x86_64__)

#ifndef __GNUC__
#error "Kernel GF AVX2 intrinsics require GCC-compatible builtins"
#endif

#ifndef __AVX2__
#pragma GCC push_options
#pragma GCC target("avx2", "sse4.2")
#define NVMEIBC_POP_AVX2_TARGET
#endif

typedef long long __v4di __attribute__((__vector_size__(32)));
typedef unsigned long long __v4du __attribute__((__vector_size__(32)));
typedef int __v8si __attribute__((__vector_size__(32)));
typedef short __v16hi __attribute__((__vector_size__(32)));
typedef char __v32qi __attribute__((__vector_size__(32)));
typedef signed char __v32qs __attribute__((__vector_size__(32)));
typedef unsigned char __v32qu __attribute__((__vector_size__(32)));

typedef long long __m256i __attribute__((__vector_size__(32), __may_alias__));
typedef long long __m256i_u __attribute__((__vector_size__(32), __may_alias__, __aligned__(1)));

typedef long long __v2di __attribute__((__vector_size__(16)));
typedef long long __m128i __attribute__((__vector_size__(16), __may_alias__));

enum _mm_hint {
	_MM_HINT_NTA = 0
};

__extension__ extern __inline void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_prefetch(const void *__P, enum _mm_hint __I)
{
	/*
	 * Use __builtin_prefetch: kernel builds may use Clang, which does not
	 * expose __builtin_ia32_prefetch like GCC's xmmintrin.h.  Current callers
	 * only use _MM_HINT_NTA → temporal locality 0.
	 */
	(void)__I;
	__builtin_prefetch(__P, 0, 0);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_loadu_si256(__m256i_u const *__P)
{
	return *__P;
}

__extension__ extern __inline void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_storeu_si256(__m256i_u *__P, __m256i __A)
{
	*__P = __A;
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_stream_load_si256(__m256i const *__A)
{
	return (__m256i)__builtin_ia32_movntdqa256((__v4di *)__A);
}

__extension__ extern __inline void
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_stream_si256(__m256i *__A, __m256i __B)
{
	__builtin_ia32_movntdq256((__v4di *)__A, (__v4di)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_setzero_si256(void)
{
	return __extension__((__m256i)(__v4di){ 0, 0, 0, 0 });
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_set_epi8(char __q31, char __q30, char __q29, char __q28, char __q27, char __q26,
		char __q25, char __q24, char __q23, char __q22, char __q21, char __q20,
		char __q19, char __q18, char __q17, char __q16, char __q15, char __q14,
		char __q13, char __q12, char __q11, char __q10, char __q09, char __q08,
		char __q07, char __q06, char __q05, char __q04, char __q03, char __q02,
		char __q01, char __q00)
{
	return __extension__((__m256i)(__v32qi){
		__q00, __q01, __q02, __q03, __q04, __q05, __q06, __q07, __q08, __q09, __q10,
		__q11, __q12, __q13, __q14, __q15, __q16, __q17, __q18, __q19, __q20, __q21,
		__q22, __q23, __q24, __q25, __q26, __q27, __q28, __q29, __q30, __q31
	});
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_set1_epi8(char __A)
{
	return _mm256_set_epi8(__A, __A, __A, __A, __A, __A, __A, __A, __A, __A, __A, __A,
			       __A, __A, __A, __A, __A, __A, __A, __A, __A, __A, __A, __A, __A,
			       __A, __A, __A, __A, __A, __A, __A);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_xor_si256(__m256i __A, __m256i __B)
{
	return (__m256i)((__v4du)__A ^ (__v4du)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_add_epi8(__m256i __A, __m256i __B)
{
	return (__m256i)((__v32qu)__A + (__v32qu)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_and_si256(__m256i __A, __m256i __B)
{
	return (__m256i)((__v4du)__A & (__v4du)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_andnot_si256(__m256i __A, __m256i __B)
{
	return (__m256i)__builtin_ia32_andnotsi256((__v4di)__A, (__v4di)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_cmpgt_epi8(__m256i __A, __m256i __B)
{
	return (__m256i)((__v32qs)__A > (__v32qs)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_cmpeq_epi8(__m256i __A, __m256i __B)
{
	return (__m256i)((__v32qi)__A == (__v32qi)__B);
}

__extension__ extern __inline __m256i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_srli_epi16(__m256i __A, int __B)
{
	return (__m256i)__builtin_ia32_psrlwi256((__v16hi)__A, __B);
}

__extension__ extern __inline __m128i
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm256_castsi256_si128(__m256i __A)
{
	return (__m128i)__builtin_ia32_si_si256((__v8si)__A);
}

#define _mm256_extracti128_si256(X, M) \
	((__m128i)__builtin_ia32_extract128i256((__v4di)(__m256i)(X), (int)(M)))

__extension__ extern __inline long long
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_cvtsi128_si64(__m128i __A)
{
	return ((__v2di)__A)[0];
}

__extension__ extern __inline long long
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_extract_epi64(__m128i __X, const int __N)
{
	return __builtin_ia32_vec_ext_v2di((__v2di)__X, __N);
}

__extension__ extern __inline unsigned long long
__attribute__((__gnu_inline__, __always_inline__, __artificial__))
_mm_crc32_u64(unsigned long long __C, unsigned long long __V)
{
	return __builtin_ia32_crc32di(__C, __V);
}

#ifdef NVMEIBC_POP_AVX2_TARGET
#pragma GCC pop_options
#undef NVMEIBC_POP_AVX2_TARGET
#endif

#elif defined(__KERNEL__)
#error "nvmeibc_x86_avx2_intrin.h: kernel build supported only for x86_64 (extend for ia32 if needed)"
#else

#include <immintrin.h>
#include <nmmintrin.h>

#endif /* __KERNEL__ x86_64 vs userspace */

#endif /* NVMEIBC_X86_AVX2_INTRIN_H */
