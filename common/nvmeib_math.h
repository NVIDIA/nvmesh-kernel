/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIBC_MATH_H
#define NVMEIBC_MATH_H

#include <linux/const.h>

#ifndef __KERNEL__
	#include <stdint.h>
	#include <math.h>

	#include "compat/kr_incs_compiler_types.h"
	// https://gcc.gnu.org/onlinedocs/gcc/Other-Builtins.html
	static inline uint32_t   hweight8(    uint32_t   w)	{return __builtin_popcount(  w&0xFF);}
	static inline uint32_t   hweight16(   uint32_t   w)	{return __builtin_popcount(  w&0xFFFF);}
	static inline uint32_t   hweight32(   uint32_t   w)	{return __builtin_popcount(  w);}
	static inline unsigned long hweight_long(unsigned long w)	{return __builtin_popcountl( w);} // Daniel: Can use: sizeof(w) == 4 ? hweight32(w) : hweight64(w);
	#if !defined(UM_APP)
	static inline int hweight64(uint64_t   w)	{return __builtin_popcountll(w);} //__builtin_popcountll return type is int
	#endif

	#define U8_MAX			((u8)~0U)
	#define U32_MAX			((u32)~0U)
	#define U64_MAX			((u64)~0ULL)
	#define min(x, y) ({ typeof(x) _minx = (x); typeof(y) _miny = (y); _minx < _miny ? _minx : _miny; })
	#define max(x, y) ({ typeof(x) _maxx = (x); typeof(y) _maxy = (y); _maxx > _maxy ? _maxx : _maxy; })
	#define min_t(T, x, y) ({ T __min1 = (x); T __min2 = (y); __min1 < __min2 ? __min1: __min2; })
	#define max_t(T, x, y) ({ T __max1 = (x); T __max2 = (y); __max1 > __max2 ? __max1: __max2; })

	#define __round_mask(x, y) ((__typeof__(x))((y)-1))
	#define round_up(    x, y) ((((x)-1) |  __round_mask(x, y))+1)
	#define round_down(  x, y) (  (x)    & ~__round_mask(x, y))

	#define DIV_ROUND_CLOSEST(x, divisor)({ typeof(x) __x = x;	typeof(divisor) __d = divisor;	\
		(((typeof(x))-1) > 0 ||	((typeof(divisor))-1) > 0 || (__x) > 0) ? \
			(((__x) + ((__d) / 2)) / (__d)) : \
			(((__x) - ((__d) / 2)) / (__d)); })

	#define DIV_ROUND_UP(n, d)		(((n) + (d) - 1) / (d))
	#ifndef __ALIGN_KERNEL_MASK
		#define __ALIGN_KERNEL_MASK(x, mask)	(((x) + (mask)) & ~(mask))
	#endif
	#define ALIGN(x,a) __ALIGN_KERNEL_MASK(x, (typeof(x))(a) - 1)
	#define IS_ALIGNED(x, a)		(((x) & ((typeof(x))(a) - 1)) == 0)
	#define PAGE_ALIGN(addr) ALIGN(addr, PAGE_SIZE)

#ifndef roundup
	#define roundup(_rounded_num, _round_to) ({			\
		__typeof__(_rounded_num) rnum = (_rounded_num);	\
		__typeof__(_round_to) rto = (_round_to);		\
		((rnum + rto - 1) / rto) * rto;					\
	})
#endif	// #ifndef roundup

	#define rounddown(_rounded_num, _round_to) ({		\
		__typeof__(_rounded_num) rnum = (_rounded_num);	\
		__typeof__(_round_to) rto = (_round_to);		\
		(rnum / rto) * rto;								\
	})

	#define divroundup(_dividend, _divisor) ({			\
		__typeof__(_dividend) dnum = (_dividend);		\
		__typeof__(_divisor) dto = (_divisor);			\
		(dnum + dto - 1) / dto;							\
	})
	#if !defined(swap)
		#define swap(x, y) ({ typeof(x) tmp = (x); (x) = (y); (y) = tmp; })
	#endif


/*****************     Statistics functions                 *******************/

typedef struct nvmeib_basic_statistics {
	char		*desc;
	double		sum;
	int			n;
	double		sum_of_squares;
} nvmeib_basic_statistics;

#define nvmeib_basic_statistics_reset(_stats_reset, _desc) ({					\
	struct nvmeib_basic_statistics		*stats_reset = (_stats_reset);			\
	memset(stats_reset, 0, sizeof(*stats_reset));								\
	stats_reset->desc = _desc;													\
})

#define nvmeib_basic_statistics_add_val(_stats_add, _val_add) ({				\
	struct nvmeib_basic_statistics		*stats_add = (_stats_add);				\
	double								val_add = (_val_add);					\
	stats_add->n += 1;															\
	stats_add->sum += val_add;													\
	stats_add->sum_of_squares += (val_add * val_add);							\
})

#define nvmeib_basic_statistics_get_desc(_stats_desc) ({						\
	struct nvmeib_basic_statistics		*stats_desc = (_stats_desc);			\
	stats_desc->desc;															\
})

#define nvmeib_basic_statistics_get_avg(_stats_avg) ({							\
	struct nvmeib_basic_statistics		*stats_avg = (_stats_avg);				\
	(stats_avg->n ? (stats_avg->sum / stats_avg->n) : 0.0);						\
})

#define nvmeib_basic_statistics_get_n(_stats_n) ({								\
	struct nvmeib_basic_statistics		*stats_n = (_stats_n);					\
	stats_n->n;																	\
})

#define nvmeib_basic_statistics_get_standard_deviation(_stats_sd) ({											\
	struct nvmeib_basic_statistics		*stats_sd = (_stats_sd);												\
	double								avg_sd = nvmeib_basic_statistics_get_avg(stats_sd);						\
	(stats_sd->n <= 1 ? 0.0 : 																					\
		sqrt(((stats_sd->sum_of_squares / stats_sd->n) - (avg_sd * avg_sd)) / (stats_sd->n - 1)));				\
})

#endif//__KERNEL__

// Additional Utils for both kernel and user space
#define MAX_WITH(a,b) { if (a < (b)) a = (b); }
#define MIN_WITH(a,b) { if (a > (b)) a = (b); }

/**
 * Overflow-safe (a * x / y) for integer arithmetic.
 *
 * Splits into integer and fractional parts to avoid intermediate overflow:
 *   result = x * (a / y) + x * (a % y) / y
 */
#define MUL_X_DIV_Y(a, x, y) ((x) * ((a) / (y)) + ((x) * ((a) % (y))) / (y))

static inline uint32_t ror32_width(uint32_t bm, uint32_t shift, uint32_t width)// Handling segments bitmaps
{
	uint64_t res;
	//BUG_ON(shift > width || width > 32);
	res = (bm << width) | bm;
	res >>= shift;
	res &= ((1 << width) - 1);
	return (uint32_t)res;
}

static inline uint32_t rol32_width(uint32_t bm, uint32_t shift, uint32_t width)
{
	uint64_t res;
	//BUG_ON(shift > width || width > 32);
	res = (bm << width) | bm;
	res >>= (width - shift);
	res &= ((1 << width) - 1);
	return (uint32_t)res;
}

static inline uint32_t choose_t(uint32_t n, uint32_t k)
{
	if (k == 0)
		return 1;
	return (n * choose_t(n - 1, k - 1)) / k;
}

#define __is_bmp_included_in(bmp, superset) (((bmp) & (superset)) == (bmp))

#endif // h file
