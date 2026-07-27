#pragma GCC push_options
#pragma GCC target ("+nothing+simd")

typedef __Int8x16_t int8x16_t;
typedef __Uint8x16_t uint8x16_t;
typedef __Int64x2_t int64x2_t;
typedef __Uint64x2_t uint64x2_t;
typedef __Poly8x16_t poly8x16_t;


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
#pragma GCC pop_options
