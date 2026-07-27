#ifndef NVMEIB_UTILS_SHARED_H
#define NVMEIB_UTILS_SHARED_H

/************************************************************
 * NVMesh Utils shared between Kernel and User-Space
 ***********************************************************/
#include "common/kr_incs.h"

static inline unsigned int nvmeib_bitmap_to_be32_ext(__be32 *out, const unsigned long *bitmap, unsigned int nbits, bool complement)
{
	unsigned int out_bits = 0;
	u32 next_32;
	while (nbits >= 32) {
		next_32 = (u32)*bitmap;
		if (complement)
			*out = ~cpu_to_be32(next_32);
		else
			*out = cpu_to_be32(next_32);
		out++;
		nbits -= 32;
		out_bits += 32;
#if BITS_PER_LONG == 64
		next_32 = ((*bitmap) >> 32);
		if (nbits < 32)
			break;
		if (complement)
			*out = ~cpu_to_be32(next_32);
		else
			*out = cpu_to_be32(next_32);
		out++;
		nbits -= 32;
		out_bits += 32;
#endif
		bitmap++;
	}
	if (unlikely(nbits > 0)) {
		int i;
		next_32 = (u32)*bitmap;
		*out = 0;
#if defined (__BIG_ENDIAN)
		(void)i;
		if (complement)
			*out = (next_32 & ((1 << nbits) - 1));
		else
			*out = ~(next_32 & ((1 << nbits) - 1));
#else
		for (i = 0; i < nbits; i++) {
			if (complement) {
				if (!(next_32 & (1 << i)))
					(*out) |= (1 << (31 - i));
			} else {
				if ((next_32 & (1 << i)))
					(*out) |= (1 << (31 - i));
			}
		}
#endif
		out_bits += nbits;
	}
	return out_bits;
}

static inline unsigned int nvmeib_bitmap_to_be32(__be32 *out, const unsigned long *bitmap, unsigned int nbits)
{
	return nvmeib_bitmap_to_be32_ext(out, bitmap, nbits, false);
}


static inline unsigned int nvmeib_bitmap_to_be32_inplace(void *bmp, unsigned int nbits)
{
	uint out_bits = 0;
	__be32 *out =    (void*)bmp;
	ulong  *bitmap = (void*)bmp;
	for (; nbits >= 64; nbits -= 64, bitmap++, out +=2) {
		__be32 tmp[2];
		out_bits += nvmeib_bitmap_to_be32(tmp, bitmap, 64);
		out[0] = tmp[0];
		out[1] = tmp[1];
	}
	for (; nbits >= 32; nbits -= 32, bitmap++, out +=1) {
		__be32 tmp[1];
		out_bits += nvmeib_bitmap_to_be32(tmp, bitmap, 32);
		out[0] = tmp[0];
	}
	return out_bits;
}

static inline unsigned int nvmeib_bitmap_from_be32_ext(unsigned long *bitmap, unsigned int nbits, const __be32 *in, bool complement)
{
	unsigned int out_bits = 0;
	int i;
	u8 final_shift = 0;
	while (nbits >= 32) {
		if (complement)
			*bitmap = ~be32_to_cpu(*in);
		else
			*bitmap = be32_to_cpu(*in);
		in++;
		nbits -= 32;
		out_bits += 32;
#if BITS_PER_LONG == 64
		if (nbits < 32) {
			final_shift = 32;
			break;
		}
		if (complement)
			*bitmap |= ((u64)(~be32_to_cpu(*in)) << 32);
		else
			*bitmap |= ((u64)be32_to_cpu(*in) << 32);
		in++;
		nbits -= 32;
		out_bits += 32;
#endif
		bitmap++;
	}
	if (unlikely(nbits > 0)) {
		if (nbits < 32) {
			unsigned long clr_mask = ~(u32)0;
			*bitmap &= clr_mask << final_shift;
#if defined (__BIG_ENDIAN)
			(void)i;
			if (complement)
				*bitmap |= ((~(*in) & ((1 << nbits) - 1)) << final_shift);
			else
				*bitmap |= ((*in & ((1 << nbits) - 1)) << final_shift);
#else
			for (i = 0; i < nbits; i++) {
				if (complement) {
					if (!(*in & (1 << (31 - i))))
						*bitmap |= ((1ULL << i) << final_shift);
				} else {
					if ((*in & (1 << (31 - i))))
						*bitmap |= ((1ULL << i) << final_shift);
				}
			}
#endif
			out_bits += nbits;
		} else
			BUG_ON(1);
	}
	return out_bits;
}

static inline unsigned int nvmeib_bitmap_from_be32(unsigned long *bitmap, unsigned int nbits, const __be32 *in)
{
	return nvmeib_bitmap_from_be32_ext(bitmap, nbits, in, false);
}

static inline int nvmeib_test_bit_be32(unsigned int bit, const __be32 *bmp)
{
	const __be32 *sect32 = bmp + (bit / 32);
	u32 hsect32 = be32_to_cpu(*sect32);
	return (hsect32 & (1UL << (bit % 32))) != 0;
}

static inline void nvmeib_set_bit_be32(unsigned int bit, __be32 *bmp)
{
	__be32 *sect32 = bmp + (bit / 32);
	u32 hsect32 = be32_to_cpu(*sect32);
	hsect32 |= (1UL << (bit % 32));
	*sect32 = cpu_to_be32(hsect32);
}

static inline void nvmeib_clr_bit_be32(unsigned int bit, __be32 *bmp)
{
	__be32 *sect32 = bmp + (bit / 32);
	u32 hsect32 = be32_to_cpu(*sect32);
	hsect32 &= ~(1UL << (bit % 32));
	*sect32 = cpu_to_be32(hsect32);
}

static inline int nvmeib_call_for_each_bit_set_be32(unsigned int nbits, const __be32 *bmp,
											int (*fn)(int bit, void *param), void *param)
{
	const __be32 *sect32 = bmp;
	u32 hsect32, mask;
	unsigned int bit;
	int rv = 0;
	for (bit = 0; bit < nbits; bit++) {
		if ((bit % 32) == 0) {
			hsect32 = be32_to_cpu(*sect32);
			sect32++;
			mask = 0x1;
		}
		if (hsect32 & mask) {
			if ((rv = (*fn)(bit, param)))
				break;
		}
		mask <<= 1;
	}
	return rv;
}

static inline int nvmeib_call_for_each_bit_be32(unsigned int nbits, const __be32 *bmp,
											int (*fn)(int bit, bool is_set, void *param), void *param)
{
	const __be32 *sect32 = bmp;
	u32 hsect32, mask;
	unsigned int bit;
	int rv = 0;
	for (bit = 0; bit < nbits; bit++) {
		if ((bit % 32) == 0) {
			hsect32 = be32_to_cpu(*sect32);
			sect32++;
			mask = 0x1;
		}
		if ((rv = (*fn)(bit, !!(hsect32 & mask), param)))
			break;
		mask <<= 1;
	}
	return rv;
}

static inline bool nvmeib_any_bit_be32(unsigned int nbits, const __be32 *bmp)
{
	unsigned int i;
	for (i = 0; i < DIV_ROUND_UP(nbits, 32); i++, bmp++) {
		const u32 val = be32_to_cpu(*bmp);
		if (val != 0)
			return true;
	}
	return false;
}

#define DWORD_SHIFT 5
#define DWORD_BITS_NUM 32 /* 8 * sizeof (int) */
#define DWORD_BITS_FILL ((DWORD_BITS_NUM) - 1)
#define nvmeib_bitmap32_len(nbits) (((nbits) + DWORD_BITS_FILL) >> DWORD_SHIFT)

/* named code blocks */
#define BLOCK(name)	goto name; name##_skip: if (0) name:
#define BREAK(name) goto name##_skip

#endif /* NVMEIB_UTILS_SHARED_H */
