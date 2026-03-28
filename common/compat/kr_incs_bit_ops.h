/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KERNEL_BIT_ALGORITHMS_H
#define KERNEL_BIT_ALGORITHMS_H

#include "common/compat/kr_incs_compiler_types.h"

#ifdef __KERNEL__
	#include "linux/bits.h"

	#if !defined (GENMASK)	// 3.18-rc4
		#define GENMASK(h, l) 	(((~0UL) - (1UL << (l)) + 1) & (~0UL >> (BITS_PER_LONG - 1 - (h))))
	#endif
#else
	// Kernel already has those functions. Define as compatibility for user-space
	#include <assert.h>
	#include "../nvmeib_math.h"
	#include "kr_incs_types.h"

static inline int fls(int x){ return x ? sizeof(x) * 8 - __builtin_clz(x) : 0;}
static inline int ilog2(u32 n){ return fls(n) - 1; }

/******************************* Bit operations ******************************/
// non-atomic.h bitops.h
#define BITS_PER_BYTE		8
#define BITS_PER_LONG 		64		// (sizeof(long)<<3)
#if !defined(BIT) && !defined(UM_APP)
	#define BIT(bit_index)		(1UL << (bit_index))
#endif
#define BIT_MASK(bit_index)	(1UL << ((bit_index) % BITS_PER_LONG))
#define BIT_WORD(bit_index)	((bit_index) / BITS_PER_LONG)
#define GENMASK(h, l) 		(((~0UL) << (l)) & (~0UL >> (BITS_PER_LONG - 1 - (h))))

#ifdef __LITTLE_ENDIAN
	#define BITMAP_MEM_ALIGNMENT 8
#else
	#define BITMAP_MEM_ALIGNMENT (8 * sizeof(unsigned long))
#endif
#define BITMAP_MEM_MASK (BITMAP_MEM_ALIGNMENT - 1)

static inline void __attr_no_alignment_sanity __set_bit(int bit_index, volatile unsigned long *addr){
	const unsigned long mask = BIT_MASK(bit_index);
	unsigned long *p = ((unsigned long *)addr) + BIT_WORD(bit_index);
	*p  |= mask;
}

static inline void __clear_bit(int bit_index, volatile unsigned long *addr){
	const unsigned long mask = BIT_MASK(bit_index);
	unsigned long *p = ((unsigned long *)addr) + BIT_WORD(bit_index);
	*p &= ~mask;
}

#define set_bit(  bit_index, addr) __set_bit(  bit_index, addr)
#define clear_bit(bit_index, addr) __clear_bit(bit_index, addr)

static inline int __attr_no_alignment_sanity test_bit(int bit_index, const volatile unsigned long *addr) {
	return 1UL & (addr[BIT_WORD(bit_index)] >> (bit_index & (BITS_PER_LONG-1)));
}

static inline u32 rol32(u32 word, unsigned int shift){ return (word << shift) | (word >> ((-shift) & 31)); }
static inline u32 ror32(u32 word, unsigned int shift){ return (word >> shift) | (word << (32 - shift)); }

#define BITS_TO_LONGS(nr)	DIV_ROUND_UP(nr, BITS_PER_BYTE * sizeof(long))
#if !defined(DECLARE_BITMAP)
	#define DECLARE_BITMAP(name,bits) unsigned long name[BITS_TO_LONGS(bits)]
#endif
#define for_each_set_bit(bit, addr, size) \
	for ((bit) = find_first_bit((addr), (size));		\
	     (bit) < (size);					\
	     (bit) = find_next_bit((addr), (size), (bit) + 1))


/* same as for_each_set_bit() but use bit as value to start with */
#define for_each_set_bit_from(bit, addr, size) \
	for ((bit) = find_next_bit((addr), (size), (bit));	\
	     (bit) < (size);					\
	     (bit) = find_next_bit((addr), (size), (bit) + 1))

#define for_each_clear_bit(bit, addr, size) \
	for ((bit) = find_first_zero_bit((addr), (size));	\
	     (bit) < (size);					\
	     (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

/* same as for_each_clear_bit() but use bit as value to start with */
#define for_each_clear_bit_from(bit, addr, size) \
	for ((bit) = find_next_zero_bit((addr), (size), (bit));	\
	     (bit) < (size);					\
	     (bit) = find_next_zero_bit((addr), (size), (bit) + 1))

#define small_const_nbits(nbits) (__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG)
#define BITMAP_FIRST_WORD_MASK(start) (~0UL << ((start) & (BITS_PER_LONG - 1)))
#define BITMAP_LAST_WORD_MASK(nbits) (~0UL >> (-(nbits) & (BITS_PER_LONG - 1)))

static __always_inline unsigned long __fls(unsigned long word) {
	int num = BITS_PER_LONG - 1;
#if BITS_PER_LONG == 64
	if (!(word & (~0ul << 32))) {					num -= 32;	word <<= 32; }
#endif
	if (!(word & (~0ul << (BITS_PER_LONG-16)))) {	num -= 16;	word <<= 16; }
	if (!(word & (~0ul << (BITS_PER_LONG-8)))) {	num -= 8;	word <<= 8; }
	if (!(word & (~0ul << (BITS_PER_LONG-4)))) {	num -= 4;	word <<= 4;	}
	if (!(word & (~0ul << (BITS_PER_LONG-2)))) {	num -= 2;	word <<= 2;	}
	if (!(word & (~0ul << (BITS_PER_LONG-1))))		num -= 1;
	return num;
}

#if !defined(UTILS_H_INCLUDED)
	/* fls64(value) returns 0 if value is 0 or the position of the last
	* set bit if value is nonzero. The last (most significant) bit is
	* at position 64.*/
	static __always_inline int fls64(u64 x) { if (x == 0) return 0; return __fls(x) + 1; }
#endif

static __always_inline unsigned long __ffs(unsigned long word) {
	int num = 0;
#if BITS_PER_LONG == 64
	if ((word & 0xffffffff) == 0) {	num += 32;		word >>= 32;	}
#endif
	if ((word & 0xffff) == 0) {		num += 16;		word >>= 16;	}
	if ((word & 0xff) == 0) {		num += 8;		word >>= 8;		}
	if ((word & 0xf) == 0) {		num += 4;		word >>= 4;		}
	if ((word & 0x3) == 0) {		num += 2;		word >>= 2;		}
	if ((word & 0x1) == 0)			num += 1;
	return num;
}

static inline unsigned long __attr_no_alignment_sanity _find_next_bit(const unsigned long *addr, unsigned long nbits, unsigned long start, unsigned long invert) {
	const size_t addr_nbytes = __builtin_object_size(addr, 0);
	unsigned long tmp;

	/*
	 * Scalar bitmaps are often passed by address to the generic helpers.
	 * When the compiler knows the pointed storage size, clamp the scan to
	 * that storage to avoid false-positive array-bounds warnings in
	 * inlined callers while preserving valid behavior for real bitmaps.
	 */
	if (addr_nbytes != (size_t)-1) {
		const unsigned long addr_nbits = (addr_nbytes / sizeof(*addr)) * BITS_PER_LONG;

		assert(nbits <= addr_nbits);
	}

	if (unlikely(start >= nbits))
		return nbits;

	tmp = addr[start / BITS_PER_LONG] ^ invert;
	tmp &= BITMAP_FIRST_WORD_MASK(start);
	start = round_down(start, BITS_PER_LONG);
	while (!tmp) {
		start += BITS_PER_LONG;
		if (start >= nbits)
			return nbits;
		tmp = addr[start / BITS_PER_LONG] ^ invert;
	}
	return min(start + __ffs(tmp), nbits);
}

static inline unsigned long find_next_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
	return _find_next_bit(addr, size, offset, 0UL);
}

static inline unsigned long find_next_zero_bit(const unsigned long *addr, unsigned long size, unsigned long offset) {
	return _find_next_bit(addr, size, offset, ~0UL);
}

static inline unsigned long __attr_no_alignment_sanity find_first_bit(const unsigned long *addr, unsigned long size) {
	unsigned long idx;
	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx])
			return min(idx * BITS_PER_LONG + __ffs(addr[idx]), size);
	}
	return size;
}
#define ffz(x)  __ffs(~(x))
static inline unsigned long find_first_zero_bit(const unsigned long *addr, unsigned long size) {
	unsigned long idx;
	for (idx = 0; idx * BITS_PER_LONG < size; idx++) {
		if (addr[idx] != ~0UL)
			return min(idx * BITS_PER_LONG + ffz(addr[idx]), size);
	}
	return size;
}

static inline unsigned long find_last_bit(const unsigned long *addr, unsigned long size) {
	if (size) {
		unsigned long val = BITMAP_LAST_WORD_MASK(size);
		unsigned long idx = (size-1) / BITS_PER_LONG;
		do {
			val &= addr[idx];
			if (val)
				return idx * BITS_PER_LONG + __fls(val);

			val = ~0ul;
		} while (idx--);
	}
	return size;
}

static inline void __bitmap_set(unsigned long *map, unsigned int start, int len) {
	unsigned long *p = map + BIT_WORD(start);
	const unsigned int size = start + len;
	int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_set = BITMAP_FIRST_WORD_MASK(start);
	while (len - bits_to_set >= 0) {
		*p |= mask_to_set;
		len -= bits_to_set;
		bits_to_set = BITS_PER_LONG;
		mask_to_set = ~0UL;
		p++;
	}
	if (len) {
		mask_to_set &= BITMAP_LAST_WORD_MASK(size);
		*p |= mask_to_set;
	}
}

static inline void bitmap_set(unsigned long *map, unsigned int start, unsigned int nbits) {
	if (__builtin_constant_p(nbits) && nbits == 1)
		set_bit(start, map);
	else if (__builtin_constant_p(start & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(start, BITMAP_MEM_ALIGNMENT) &&
		 __builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
		 IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		memset((char *)map + start / 8, 0xff, nbits / 8);
	else
		__bitmap_set(map, start, nbits);
}

static inline int __bitmap_equal(const unsigned long *bitmap1,
		const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] != bitmap2[k])
			return 0;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] ^ bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 0;

	return 1;
}

static inline int bitmap_equal(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return !((*src1 ^ *src2) & BITMAP_LAST_WORD_MASK(nbits));
	if (__builtin_constant_p(nbits & BITMAP_MEM_MASK) &&
	    IS_ALIGNED(nbits, BITMAP_MEM_ALIGNMENT))
		return !memcmp(src1, src2, nbits / 8);
	return __bitmap_equal(src1, src2, nbits);
}

static int __bitmap_intersects(const unsigned long *bitmap1,
			const unsigned long *bitmap2, unsigned int bits)
{
	unsigned int k, lim = bits/BITS_PER_LONG;
	for (k = 0; k < lim; ++k)
		if (bitmap1[k] & bitmap2[k])
			return 1;

	if (bits % BITS_PER_LONG)
		if ((bitmap1[k] & bitmap2[k]) & BITMAP_LAST_WORD_MASK(bits))
			return 1;
	return 0;
}

static inline int bitmap_intersects(const unsigned long *src1,
			const unsigned long *src2, unsigned int nbits)
{
	if (small_const_nbits(nbits))
		return ((*src1 & *src2) & BITMAP_LAST_WORD_MASK(nbits)) != 0;
	else
		return __bitmap_intersects(src1, src2, nbits);
}

static inline int bitmap_empty(const unsigned long *src, unsigned nbits) {
	if (small_const_nbits(nbits))
		return ! (*src & BITMAP_LAST_WORD_MASK(nbits));
	return find_first_bit(src, nbits) == nbits;
}

static inline void bitmap_or(unsigned long *dst, const unsigned long *src1, const unsigned long *src2, unsigned int nbits) {
	if (small_const_nbits(nbits))
		*dst = *src1 | *src2;
	else {
		unsigned int k, nr = BITS_TO_LONGS(nbits);
		for (k = 0; k < nr; k++)
			dst[k] = src1[k] | src2[k];
	}
}

static inline void bitmap_and(unsigned long *dst, const unsigned long *src1, const unsigned long *src2, unsigned int nbits) {
	if (small_const_nbits(nbits))
		*dst = *src1 & *src2;
	else {
		unsigned int k, nr = BITS_TO_LONGS(nbits);
		for (k = 0; k < nr; k++)
			dst[k] = src1[k] & src2[k];
	}
}

static inline int __bitmap_weight(const unsigned long *bitmap, unsigned int bits) {
	unsigned int k, lim = bits/BITS_PER_LONG;
	int w = 0;
	for (k = 0; k < lim; k++)
		w += hweight_long(bitmap[k]);
	if (bits % BITS_PER_LONG)
		w += hweight_long(bitmap[k] & BITMAP_LAST_WORD_MASK(bits));
	return w;
}

static inline int bitmap_weight(const unsigned long *src, unsigned int nbits) {
	if (small_const_nbits(nbits))
		return hweight_long(*src & BITMAP_LAST_WORD_MASK(nbits));
	return __bitmap_weight(src, nbits);
}

static inline void bitmap_copy(unsigned long *dst, const unsigned long *src, unsigned int nbits) {
	if (small_const_nbits(nbits))
		*dst = *src;
	else {
		unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
		memcpy(dst, src, len);
	}
}

static inline void bitmap_zero(unsigned long *dst, unsigned int nbits) {
	if (small_const_nbits(nbits))
		*dst = 0UL;
	else {
		unsigned int len = BITS_TO_LONGS(nbits) * sizeof(unsigned long);
		memset(dst, 0, len);
	}
}

static inline void __bitmap_shift_left(unsigned long *dst, const unsigned long *src,
		unsigned int shift, unsigned int nbits)
{
	int k;
	unsigned int lim = BITS_TO_LONGS(nbits);
	unsigned int off = shift/BITS_PER_LONG, rem = shift % BITS_PER_LONG;
	for (k = lim - off - 1; k >= 0; --k) {
		unsigned long upper, lower;

		if (rem && k > 0)
			lower = src[k - 1] >> (BITS_PER_LONG - rem);
		else
			lower = 0;
		upper = src[k] << rem;
		dst[k + off] = lower | upper;
	}
	if (off)
		memset(dst, 0, off*sizeof(unsigned long));
}

#define CHUNKSZ 32

static inline int __bitmap_parse_hex_to_bin(char ch)
{
	if ((ch >= '0') && (ch <= '9'))
		return ch - '0';
	ch = tolower(ch);
	if ((ch >= 'a') && (ch <= 'f'))
		return ch - 'a' + 10;
	return -1;
}

static inline int bitmap_parse(const char *buf, unsigned int buflen,
		unsigned long *maskp, int nmaskbits)
{
	int c, old_c, totaldigits, ndigits, nchunks, nbits;
	u32 chunk;

	bitmap_zero(maskp, nmaskbits);

	nchunks = nbits = totaldigits = c = 0;
	do {
		chunk = 0;
		ndigits = totaldigits;

		/* Get the next chunk of the bitmap */
		while (buflen) {
			old_c = c;
			c = *buf++;
			buflen--;
			if (isspace(c))
				continue;

			if (totaldigits && c && isspace(old_c))
				return -EINVAL;

			if (c == '\0' || c == ',')
				break;

			if (!isxdigit(c))
				return -EINVAL;

			if (chunk & ~((1UL << (CHUNKSZ - 4)) - 1))
				return -EOVERFLOW;

			chunk = (chunk << 4) | __bitmap_parse_hex_to_bin(c);
			totaldigits++;
		}
		if (ndigits == totaldigits)
			return -EINVAL;
		if (nchunks == 0 && chunk == 0)
			continue;

		__bitmap_shift_left(maskp, maskp, CHUNKSZ, nmaskbits);
		*maskp |= chunk;
		nchunks++;
		nbits += (nchunks == 1) ? fls(chunk) : CHUNKSZ;
		if (nbits > nmaskbits)
			return -EOVERFLOW;
	} while (buflen && c == ',');

	return 0;
}

#undef CHUNKSZ

		#ifdef __x86_64__
			// Set a bit and return its old value
			static inline int test_and_set_bit(int bit_index, volatile unsigned long *addr){
				unsigned char oldbit;
				asm volatile("lock; bts %2,%1\n\tsetc %0"
					: "=q" (oldbit), "+m" (*(volatile long *)(addr))
					: "Ir" (bit_index)
					: "cc", "memory");
				return oldbit;
			}
			// Clear a bit and return its old value
			static inline int test_and_clear_bit(int bit_index, volatile unsigned long *addr){
				unsigned char oldbit;
				asm volatile("lock; btr %2,%1\n\tsetc %0"
					: "=q" (oldbit), "+m" (*(volatile long *)(addr))
					: "Ir" (bit_index)
					: "cc", "memory");
				return oldbit;
			}
	#else
	static inline int test_and_set_bit(int bit_index, unsigned long *addr) {
		unsigned long mask = BIT_MASK(bit_index);
		unsigned long *p = ((unsigned long *)addr) + BIT_WORD(bit_index);
		unsigned long old = *p;
		*p = old | mask;
		return (old & mask) != 0;
	}
	static inline int test_and_clear_bit(int bit_index, unsigned long *addr) {
		unsigned long mask = BIT_MASK(bit_index);
		unsigned long *p = ((unsigned long *)addr) + BIT_WORD(bit_index);
		unsigned long old = *p;
		*p = old & ~mask;
		return (old & mask) != 0;
	}
#endif

static inline __attribute__((const)) unsigned long __roundup_pow_of_two(unsigned long n) {
	#if BITS_PER_LONG == 32
		return 1UL << fls(n - 1);
	#else
		return 1UL << fls64(n - 1);
	#endif
}

#define roundup_pow_of_two(n) (	\
	__builtin_constant_p(n) ? 	\
		((n == 1) ? 1 : (1UL << (ilog2((n) - 1) + 1))) :	\
		__roundup_pow_of_two(n)			\
)

static inline __attribute_const__ int get_order(unsigned long size) {
	//WARN_ON(size == 0);	// Kernel says it is undefined, cannot trust this // Todo properly define the dependency on WARN() definition
	if (__builtin_constant_p(size)) {
		if (size < (1UL << PAGE_SHIFT))
			return 0;
		return ilog2((size) - 1) - PAGE_SHIFT + 1;
	}
	size--;
	size >>= PAGE_SHIFT;
#if BITS_PER_LONG == 32
	return fls(size);
#else
	return fls64(size);
#endif
}

static inline __attribute__((const)) bool is_power_of_2(unsigned long n) {
	return (n != 0 && ((n & (n - 1)) == 0));
}

#endif // __KERNEL__
#endif // KERNEL_BIT_ALGORITHMS_H
