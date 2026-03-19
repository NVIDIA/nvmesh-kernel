/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef __clang__
#pragma GCC push_options
#pragma GCC target ("+nothing+simd")

typedef __Int8x16_t int8x16_t;
typedef __Uint8x16_t uint8x16_t;
typedef __Int64x2_t int64x2_t;
typedef __Uint64x2_t uint64x2_t;
typedef __Poly8x16_t poly8x16_t;

#ifndef __GNUC__
/* TBD: Add support for clang */
#error "Compiler not supported"

#else /*  __GNUC__ */

/* GCC Support */

#if __GNUC__ >= 12

/* GCC >= 12 Support - Copied from /usr/lib/gcc/aarch64-linux-gnu/12/include/arm_neon.h */

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vshlq_n_u8 (uint8x16_t __a, const int __b)
{
	return (uint8x16_t) __builtin_aarch64_ashlv16qi ((int8x16_t) __a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vshrq_n_u8 (uint8x16_t __a, const int __b)
{
	return __builtin_aarch64_lshrv16qi_uus (__a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vqtbl1q_u8 (uint8x16_t __tab, uint8x16_t __idx)
{
	return __builtin_aarch64_qtbl1v16qi_uuu (__tab, __idx);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
veorq_u8 (uint8x16_t __a, uint8x16_t __b)
{
	return __a ^ __b;
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vld1q_u8 (const uint8_t *__a)
{
	return __builtin_aarch64_ld1v16qi_us (
		(const __builtin_aarch64_simd_qi *) __a);
}

__extension__ extern __inline void
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vst1q_u8 (uint8_t *__a, uint8x16_t __b)
{
	__builtin_aarch64_st1v16qi_su ((__builtin_aarch64_simd_qi *) __a, __b);
}

__extension__ extern __inline void
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vst1q_u64 (uint64_t *__a, uint64x2_t __b)
{
	__builtin_aarch64_st1v2di_su ((__builtin_aarch64_simd_di *) __a, __b);
}

__extension__ extern __inline poly8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vmulq_p8 (poly8x16_t __a, poly8x16_t __b)
{
	return __builtin_aarch64_pmulv16qi_ppp (__a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vandq_u8 (uint8x16_t __a, uint8x16_t __b)
{
	return __a & __b;
}

__extension__ extern __inline int8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vshrq_n_s8 (int8x16_t __a, const int __b)
{
	return (int8x16_t) __builtin_aarch64_ashrv16qi (__a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vdupq_n_u8 (uint8_t __a)
{
	return (uint8x16_t) {__a, __a, __a, __a, __a, __a, __a, __a,
		__a, __a, __a, __a, __a, __a, __a, __a};
}

__extension__ extern __inline uint64x2_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
veorq_u64 (uint64x2_t __a, uint64x2_t __b)
{
	return __a ^ __b;
}

#else /* __GNUC__ >= 12 */

/* GCC <= 11 Support - Copied from /usr/lib/gcc/aarch64-linux-gnu/11/include/arm_neon.h */

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vshlq_n_u8 (uint8x16_t __a, const int __b)
{
	return (uint8x16_t) __builtin_aarch64_ashlv16qi ((int8x16_t) __a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vshrq_n_u8 (uint8x16_t __a, const int __b)
{
	return (uint8x16_t) __builtin_aarch64_lshrv16qi ((int8x16_t) __a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vqtbl1q_u8 (uint8x16_t a, uint8x16_t b)
{
	uint8x16_t result;
	__asm__ ("tbl %0.16b, {%1.16b}, %2.16b"
			: "=w"(result)
			: "w"(a), "w"(b)
			: /* No clobbers */);
	return result;
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
veorq_u8 (uint8x16_t __a, uint8x16_t __b)
{
	return __a ^ __b;
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vld1q_u8 (const uint8_t *a)
{
	return (uint8x16_t)
		__builtin_aarch64_ld1v16qi ((const __builtin_aarch64_simd_qi *) a);
}

__extension__ extern __inline void
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vst1q_u8 (uint8_t *a, uint8x16_t b)
{
	__builtin_aarch64_st1v16qi ((__builtin_aarch64_simd_qi *) a,
								(int8x16_t) b);
}

__extension__ extern __inline void
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vst1q_u64 (uint64_t *a, uint64x2_t b)
{
	__builtin_aarch64_st1v2di ((__builtin_aarch64_simd_di *) a,
								(int64x2_t) b);
}

__extension__ extern __inline poly8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vmulq_p8 (poly8x16_t __a, poly8x16_t __b)
{
	return (poly8x16_t) __builtin_aarch64_pmulv16qi ((int8x16_t) __a,
													(int8x16_t) __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vandq_u8 (uint8x16_t __a, uint8x16_t __b)
{
	return __a & __b;
}

__extension__ extern __inline int8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vshrq_n_s8 (int8x16_t __a, const int __b)
{
	return (int8x16_t) __builtin_aarch64_ashrv16qi (__a, __b);
}

__extension__ extern __inline uint8x16_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
vdupq_n_u8 (uint8_t __a)
{
	return (uint8x16_t) {__a, __a, __a, __a, __a, __a, __a, __a,
			__a, __a, __a, __a, __a, __a, __a, __a};
}

__extension__ extern __inline uint64x2_t
__attribute__ ((__always_inline__, __gnu_inline__, __artificial__))
veorq_u64 (uint64x2_t __a, uint64x2_t __b)
{
	return __a ^ __b;
}

#endif /* __GNUC__ >= 12 */

#endif /* __GNUC__ */

#pragma GCC pop_options
#else
typedef __attribute__((neon_vector_type(16))) int8_t int8x16_t;
typedef __attribute__((neon_vector_type(16))) uint8_t uint8x16_t;
typedef __attribute__((neon_vector_type(2))) int64_t int64x2_t;
typedef __attribute__((neon_vector_type(2))) uint64_t uint64x2_t;
#if defined(__aarch64__) || defined(__arm64ec__)
typedef uint8_t poly8_t;
#else
typedef int8_t poly8_t;
#endif
typedef __attribute__((neon_polyvector_type(16))) poly8_t poly8x16_t;

#define __ai static __inline__ __attribute__((__always_inline__, __nodebug__))

#ifdef __LITTLE_ENDIAN__
#define vshlq_n_u8(__p0, __p1) __extension__ ({ \
  uint8x16_t __ret; \
  uint8x16_t __s0 = __p0; \
  __ret = (uint8x16_t) __builtin_neon_vshlq_n_v((int8x16_t)__s0, __p1, 48); \
  __ret; \
})
#else
#define vshlq_n_u8(__p0, __p1) __extension__ ({ \
  uint8x16_t __ret; \
  uint8x16_t __s0 = __p0; \
  uint8x16_t __rev0;  __rev0 = __builtin_shufflevector(__s0, __s0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret = (uint8x16_t) __builtin_neon_vshlq_n_v((int8x16_t)__rev0, __p1, 48); \
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret; \
})
#endif

#ifdef __LITTLE_ENDIAN__
#define vshrq_n_u8(__p0, __p1) __extension__ ({ \
  uint8x16_t __ret; \
  uint8x16_t __s0 = __p0; \
  __ret = (uint8x16_t) __builtin_neon_vshrq_n_v((int8x16_t)__s0, __p1, 48); \
  __ret; \
})
#else
#define vshrq_n_u8(__p0, __p1) __extension__ ({ \
  uint8x16_t __ret; \
  uint8x16_t __s0 = __p0; \
  uint8x16_t __rev0;  __rev0 = __builtin_shufflevector(__s0, __s0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret = (uint8x16_t) __builtin_neon_vshrq_n_v((int8x16_t)__rev0, __p1, 48); \
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret; \
})
#endif

#ifdef __LITTLE_ENDIAN__
__ai __attribute__((target("neon"))) uint8x16_t vqtbl1q_u8(uint8x16_t __p0, uint8x16_t __p1) {
  uint8x16_t __ret;
  __ret = (uint8x16_t) __builtin_neon_vqtbl1q_v((int8x16_t)__p0, (int8x16_t)__p1, 48);
  return __ret;
}
#else
__ai __attribute__((target("neon"))) uint8x16_t vqtbl1q_u8(uint8x16_t __p0, uint8x16_t __p1) {
  uint8x16_t __ret;
  uint8x16_t __rev0;  __rev0 = __builtin_shufflevector(__p0, __p0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  uint8x16_t __rev1;  __rev1 = __builtin_shufflevector(__p1, __p1, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  __ret = (uint8x16_t) __builtin_neon_vqtbl1q_v((int8x16_t)__rev0, (int8x16_t)__rev1, 48);
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  return __ret;
}
#endif

#ifdef __LITTLE_ENDIAN__
__ai __attribute__((target("neon"))) uint8x16_t veorq_u8(uint8x16_t __p0, uint8x16_t __p1) {
  uint8x16_t __ret;
  __ret = __p0 ^ __p1;
  return __ret;
}
#else
__ai __attribute__((target("neon"))) uint8x16_t veorq_u8(uint8x16_t __p0, uint8x16_t __p1) {
  uint8x16_t __ret;
  uint8x16_t __rev0;  __rev0 = __builtin_shufflevector(__p0, __p0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  uint8x16_t __rev1;  __rev1 = __builtin_shufflevector(__p1, __p1, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  __ret = __rev0 ^ __rev1;
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  return __ret;
}
#endif

#ifdef __LITTLE_ENDIAN__
#define vld1q_u8(__p0) __extension__ ({ \
  uint8x16_t __ret; \
  __ret = (uint8x16_t) __builtin_neon_vld1q_v(__p0, 48); \
  __ret; \
})
#else
#define vld1q_u8(__p0) __extension__ ({ \
  uint8x16_t __ret; \
  __ret = (uint8x16_t) __builtin_neon_vld1q_v(__p0, 48); \
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret; \
})
#endif

#ifdef __LITTLE_ENDIAN__
#define vst1q_u8(__p0, __p1) __extension__ ({ \
  uint8x16_t __s1 = __p1; \
  __builtin_neon_vst1q_v(__p0, (int8x16_t)__s1, 48); \
})
#else
#define vst1q_u8(__p0, __p1) __extension__ ({ \
  uint8x16_t __s1 = __p1; \
  uint8x16_t __rev1;  __rev1 = __builtin_shufflevector(__s1, __s1, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __builtin_neon_vst1q_v(__p0, (int8x16_t)__rev1, 48); \
})
#endif

#ifdef __LITTLE_ENDIAN__
#define vst1q_u64(__p0, __p1) __extension__ ({ \
  uint64x2_t __s1 = __p1; \
  __builtin_neon_vst1q_v(__p0, (int8x16_t)__s1, 51); \
})
#else
#define vst1q_u64(__p0, __p1) __extension__ ({ \
  uint64x2_t __s1 = __p1; \
  uint64x2_t __rev1;  __rev1 = __builtin_shufflevector(__s1, __s1, 1, 0); \
  __builtin_neon_vst1q_v(__p0, (int8x16_t)__rev1, 51); \
})
#endif

#ifdef __LITTLE_ENDIAN__
__ai __attribute__((target("neon"))) poly8x16_t vmulq_p8(poly8x16_t __p0, poly8x16_t __p1) {
  poly8x16_t __ret;
  __ret = (poly8x16_t) __builtin_neon_vmulq_v((int8x16_t)__p0, (int8x16_t)__p1, 36);
  return __ret;
}
#else
__ai __attribute__((target("neon"))) poly8x16_t vmulq_p8(poly8x16_t __p0, poly8x16_t __p1) {
  poly8x16_t __ret;
  poly8x16_t __rev0;  __rev0 = __builtin_shufflevector(__p0, __p0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  poly8x16_t __rev1;  __rev1 = __builtin_shufflevector(__p1, __p1, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  __ret = (poly8x16_t) __builtin_neon_vmulq_v((int8x16_t)__rev0, (int8x16_t)__rev1, 36);
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  return __ret;
}
#endif

#ifdef __LITTLE_ENDIAN__
__ai __attribute__((target("neon"))) uint8x16_t vandq_u8(uint8x16_t __p0, uint8x16_t __p1) {
  uint8x16_t __ret;
  __ret = __p0 & __p1; 
  return __ret;
}
#else
__ai __attribute__((target("neon"))) uint8x16_t vandq_u8(uint8x16_t __p0, uint8x16_t __p1) {
  uint8x16_t __ret;
  uint8x16_t __rev0;  __rev0 = __builtin_shufflevector(__p0, __p0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  uint8x16_t __rev1;  __rev1 = __builtin_shufflevector(__p1, __p1, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  __ret = __rev0 & __rev1;
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  return __ret;
}
#endif

#ifdef __LITTLE_ENDIAN__
#define vshrq_n_s8(__p0, __p1) __extension__ ({ \
  int8x16_t __ret; \
  int8x16_t __s0 = __p0; \
  __ret = (int8x16_t) __builtin_neon_vshrq_n_v((int8x16_t)__s0, __p1, 32); \
  __ret; \
})
#else
#define vshrq_n_s8(__p0, __p1) __extension__ ({ \
  int8x16_t __ret; \
  int8x16_t __s0 = __p0; \
  int8x16_t __rev0;  __rev0 = __builtin_shufflevector(__s0, __s0, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret = (int8x16_t) __builtin_neon_vshrq_n_v((int8x16_t)__rev0, __p1, 32); \
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0); \
  __ret; \
})
#endif

#ifdef __LITTLE_ENDIAN__
__ai __attribute__((target("neon"))) uint8x16_t vdupq_n_u8(uint8_t __p0) {
  uint8x16_t __ret;
  __ret = (uint8x16_t) {__p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0};
  return __ret;
}
#else
__ai __attribute__((target("neon"))) uint8x16_t vdupq_n_u8(uint8_t __p0) {
  uint8x16_t __ret;
  __ret = (uint8x16_t) {__p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0, __p0};
  __ret = __builtin_shufflevector(__ret, __ret, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
  return __ret;
}
#endif

#ifdef __LITTLE_ENDIAN__
__ai __attribute__((target("neon"))) uint64x2_t veorq_u64(uint64x2_t __p0, uint64x2_t __p1) {
  uint64x2_t __ret;
  __ret = __p0 ^ __p1;
  return __ret;
}
#else
__ai __attribute__((target("neon"))) uint64x2_t veorq_u64(uint64x2_t __p0, uint64x2_t __p1) {
  uint64x2_t __ret;
  uint64x2_t __rev0;  __rev0 = __builtin_shufflevector(__p0, __p0, 1, 0);
  uint64x2_t __rev1;  __rev1 = __builtin_shufflevector(__p1, __p1, 1, 0);
  __ret = __rev0 ^ __rev1;
  __ret = __builtin_shufflevector(__ret, __ret, 1, 0);
  return __ret;
}
#endif
#endif
