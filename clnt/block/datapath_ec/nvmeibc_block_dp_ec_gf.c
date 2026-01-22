/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#if defined(__KERNEL__)
	#include "nvmeib_public.h"		// Daniel: Thats an overkill, include less
	#include "linux/slab.h"
	#include "linux/printk.h"
	#include "linux/cpumask.h"
	#include "../common_public/nvmeib_public_raid.h"
#endif
#include "../common/compat/kr_incs_crc32.h"
#include "cpuid.h"
#include "nvmeibc_block_dp_ec_gf.h"
#include "nvmeibc_block_dp_ec_gf_arm_um.h"
#if defined (PARALLELS_COMPILATION_ONLY) && PARALLELS_COMPILATION_ONLY
	// OferOshri: This is a hack. Please define a proper condition for AVX not enabled
#elif !defined (__aarch64__)
	#define USE_GF_AVX2        NVMEIBC_GF_AVX2
#else
	int gf_asm_count = -1;
	EXPORT_SYMBOL(gf_asm_count);
#endif

#define POLY 0x1d
#define POLY64  0x1d1d1d1d1d1d1d1dULL

const struct raid6_sse_constants {
        u64 x1d[4];
} poly_constants  __attribute__((visibility("default"))) __attribute__((aligned(32))) = {
        { POLY64, POLY64, POLY64, POLY64 },
};

typedef void (*ec_encode_data_subfunc) (    int, int,           u8**, u8**, u32*, u8**);
typedef enum gf_return_val (*ec_encode_data_func) (       int, int, int,      u8**, u8**, u32*, u8**);
typedef enum gf_return_val (*ec_encode_data_update_func) (int, int, int,      u8**, u8**, u32*, u8*);
typedef enum gf_return_val (*ec_decode_data_func) (       int, int, int,      u8**, u8**, u32*);
typedef void (*ec_decode_data_subfunc_1) (  int, int, int,      u8**, u8**, u32*);
typedef void (*ec_decode_data_subfunc_2) (  int, int, int, int, u8**, u8**, u32*);

#if defined(__KERNEL__) && defined(__aarch64__)
	#if KS_CRC32C_USES_SIZE_T
		typedef u32 (*ec_crc_func) (u32 crc, const void *address, size_t length);
	#else
		typedef u32 (*ec_crc_func) (u32 crc, const void *address, unsigned int length);
	#endif
	#define CRC32_KERNEL_OR_AVX_FN 			crc32c
	#define CRC32_KERNEL_OR_UNOPT_FN 		crc32c
#else
	typedef u32 (*ec_crc_func) (u32, const u8 *, u32);		// Same prototype as kernel crc32c() and __x86_64__
	#define CRC32_KERNEL_OR_AVX_FN 			__calculate_crc32c_avx
	#define CRC32_KERNEL_OR_UNOPT_FN 		__calculate_crc32c
#endif
typedef struct {
	enum nvmeibc_gf_optimization_type id;
	ec_encode_data_func ec_encode;
	ec_encode_data_subfunc ec_encode_p;
	ec_encode_data_subfunc ec_encode_q;
	ec_encode_data_subfunc ec_encode_pq;
	ec_encode_data_update_func ec_update;
	ec_decode_data_func ec_decode;
	ec_decode_data_subfunc_1 ec_decode_p;
	ec_decode_data_subfunc_1 ec_decode_q;
	ec_decode_data_subfunc_2 ec_decode_pq;
	ec_crc_func ec_crc;
} gf_functions_t;

extern gf_functions_t gf_funcs;

unsigned const char __attribute__((aligned(256))) gff_base[] = {
        0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1d, 0x3a,
        0x74, 0xe8, 0xcd, 0x87, 0x13, 0x26, 0x4c, 0x98, 0x2d, 0x5a,
        0xb4, 0x75, 0xea, 0xc9, 0x8f, 0x03, 0x06, 0x0c, 0x18, 0x30,
        0x60, 0xc0, 0x9d, 0x27, 0x4e, 0x9c, 0x25, 0x4a, 0x94, 0x35,
        0x6a, 0xd4, 0xb5, 0x77, 0xee, 0xc1, 0x9f, 0x23, 0x46, 0x8c,
        0x05, 0x0a, 0x14, 0x28, 0x50, 0xa0, 0x5d, 0xba, 0x69, 0xd2,
        0xb9, 0x6f, 0xde, 0xa1, 0x5f, 0xbe, 0x61, 0xc2, 0x99, 0x2f,
        0x5e, 0xbc, 0x65, 0xca, 0x89, 0x0f, 0x1e, 0x3c, 0x78, 0xf0,
        0xfd, 0xe7, 0xd3, 0xbb, 0x6b, 0xd6, 0xb1, 0x7f, 0xfe, 0xe1,
        0xdf, 0xa3, 0x5b, 0xb6, 0x71, 0xe2, 0xd9, 0xaf, 0x43, 0x86,
        0x11, 0x22, 0x44, 0x88, 0x0d, 0x1a, 0x34, 0x68, 0xd0, 0xbd,
        0x67, 0xce, 0x81, 0x1f, 0x3e, 0x7c, 0xf8, 0xed, 0xc7, 0x93,
        0x3b, 0x76, 0xec, 0xc5, 0x97, 0x33, 0x66, 0xcc, 0x85, 0x17,
        0x2e, 0x5c, 0xb8, 0x6d, 0xda, 0xa9, 0x4f, 0x9e, 0x21, 0x42,
        0x84, 0x15, 0x2a, 0x54, 0xa8, 0x4d, 0x9a, 0x29, 0x52, 0xa4,
        0x55, 0xaa, 0x49, 0x92, 0x39, 0x72, 0xe4, 0xd5, 0xb7, 0x73,
        0xe6, 0xd1, 0xbf, 0x63, 0xc6, 0x91, 0x3f, 0x7e, 0xfc, 0xe5,
        0xd7, 0xb3, 0x7b, 0xf6, 0xf1, 0xff, 0xe3, 0xdb, 0xab, 0x4b,
        0x96, 0x31, 0x62, 0xc4, 0x95, 0x37, 0x6e, 0xdc, 0xa5, 0x57,
        0xae, 0x41, 0x82, 0x19, 0x32, 0x64, 0xc8, 0x8d, 0x07, 0x0e,
        0x1c, 0x38, 0x70, 0xe0, 0xdd, 0xa7, 0x53, 0xa6, 0x51, 0xa2,
        0x59, 0xb2, 0x79, 0xf2, 0xf9, 0xef, 0xc3, 0x9b, 0x2b, 0x56,
        0xac, 0x45, 0x8a, 0x09, 0x12, 0x24, 0x48, 0x90, 0x3d, 0x7a,
        0xf4, 0xf5, 0xf7, 0xf3, 0xfb, 0xeb, 0xcb, 0x8b, 0x0b, 0x16,
        0x2c, 0x58, 0xb0, 0x7d, 0xfa, 0xe9, 0xcf, 0x83, 0x1b, 0x36,
        0x6c, 0xd8, 0xad, 0x47, 0x8e, 0x01
};

unsigned const char __attribute__((aligned(256))) gflog_base[] = {
        0x00, 0xff, 0x01, 0x19, 0x02, 0x32, 0x1a, 0xc6, 0x03, 0xdf,
        0x33, 0xee, 0x1b, 0x68, 0xc7, 0x4b, 0x04, 0x64, 0xe0, 0x0e,
        0x34, 0x8d, 0xef, 0x81, 0x1c, 0xc1, 0x69, 0xf8, 0xc8, 0x08,
        0x4c, 0x71, 0x05, 0x8a, 0x65, 0x2f, 0xe1, 0x24, 0x0f, 0x21,
        0x35, 0x93, 0x8e, 0xda, 0xf0, 0x12, 0x82, 0x45, 0x1d, 0xb5,
        0xc2, 0x7d, 0x6a, 0x27, 0xf9, 0xb9, 0xc9, 0x9a, 0x09, 0x78,
        0x4d, 0xe4, 0x72, 0xa6, 0x06, 0xbf, 0x8b, 0x62, 0x66, 0xdd,
        0x30, 0xfd, 0xe2, 0x98, 0x25, 0xb3, 0x10, 0x91, 0x22, 0x88,
        0x36, 0xd0, 0x94, 0xce, 0x8f, 0x96, 0xdb, 0xbd, 0xf1, 0xd2,
        0x13, 0x5c, 0x83, 0x38, 0x46, 0x40, 0x1e, 0x42, 0xb6, 0xa3,
        0xc3, 0x48, 0x7e, 0x6e, 0x6b, 0x3a, 0x28, 0x54, 0xfa, 0x85,
        0xba, 0x3d, 0xca, 0x5e, 0x9b, 0x9f, 0x0a, 0x15, 0x79, 0x2b,
        0x4e, 0xd4, 0xe5, 0xac, 0x73, 0xf3, 0xa7, 0x57, 0x07, 0x70,
        0xc0, 0xf7, 0x8c, 0x80, 0x63, 0x0d, 0x67, 0x4a, 0xde, 0xed,
        0x31, 0xc5, 0xfe, 0x18, 0xe3, 0xa5, 0x99, 0x77, 0x26, 0xb8,
        0xb4, 0x7c, 0x11, 0x44, 0x92, 0xd9, 0x23, 0x20, 0x89, 0x2e,
        0x37, 0x3f, 0xd1, 0x5b, 0x95, 0xbc, 0xcf, 0xcd, 0x90, 0x87,
        0x97, 0xb2, 0xdc, 0xfc, 0xbe, 0x61, 0xf2, 0x56, 0xd3, 0xab,
        0x14, 0x2a, 0x5d, 0x9e, 0x84, 0x3c, 0x39, 0x53, 0x47, 0x6d,
        0x41, 0xa2, 0x1f, 0x2d, 0x43, 0xd8, 0xb7, 0x7b, 0xa4, 0x76,
        0xc4, 0x17, 0x49, 0xec, 0x7f, 0x0c, 0x6f, 0xf6, 0x6c, 0xa1,
        0x3b, 0x52, 0x29, 0x9d, 0x55, 0xaa, 0xfb, 0x60, 0x86, 0xb1,
        0xbb, 0xcc, 0x3e, 0x5a, 0xcb, 0x59, 0x5f, 0xb0, 0x9c, 0xa9,
        0xa0, 0x51, 0x0b, 0xf5, 0x16, 0xeb, 0x7a, 0x75, 0x2c, 0xd7,
        0x4f, 0xae, 0xd5, 0xe9, 0xe6, 0xe7, 0xad, 0xe8, 0x74, 0xd6,
        0xf4, 0xea, 0xa8, 0x50, 0x58, 0xaf
};

u64 __calculate_parity_P(unsigned int num, const u64* bufA) {
    unsigned int i;
    u64 p  = 0;
    for (i = 0; i < num; i++) {
        p ^= bufA[i];
    }
    return (p);
}

u64 __calculate_parity_Q(unsigned int num, const u64* bufA) {
    int i, j;
    u64 *retp;
    unsigned char *a;
    unsigned char q;
    unsigned char p2[sizeof(u64)]; //Todo: __attribute__((aligned(8)));
    memset(p2, 0, sizeof(p2));

    for (j = num-1; j >= 0; j--) {
        a = (unsigned char *)&bufA[j];
        for (i = 0; i < (int)(sizeof(u64) / sizeof(unsigned char)); i++) {
            q = p2[i];
            if (q & 0x80) {
                q ^= (q << 1) ^ a[i] ^ 0x1d;
            } else {
                q ^= (q << 1) ^ a[i];
            }
            p2[i] ^= q;
        }
    }
    retp = (u64 *)p2;
    return *retp;
}


/*
   CRC32-C calculation with no table lookup, little-endian only. No bit
   reversal because we're shifting right instead of left. Initial value is passed in crc_init.
*/
#include "../common/compat/kr_incs_crc32.inc.c"
u32 __calculate_crc32c(u32 crc_init, const u8* buffer, u32 num) {
	return __crc32c_unopt_mask(crc_init, buffer, num);
}

static inline u32 __crc32c_one_u64(u64 d64, u32 crc) {
	crc = (crc>>8) ^ crc_table[(crc ^ d64) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 8)) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 16)) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 24)) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 32)) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 40)) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 48)) & 0xFF];
	crc = (crc>>8) ^ crc_table[(crc ^ (d64 >> 56)) & 0xFF];
    return crc;
}

u32 __calculate_crc32c_64(u32 crc_init, const u8* buffer, u32 num) {
    unsigned int i;
    u32 crc;
    u64 data;

    BUG_ON(num % sizeof(u64));
#ifndef __LITTLE_ENDIAN
	BUG_ON(1);			// Can't handle big-endian yet
#endif

    num /= sizeof(u64);
    crc = crc_init;                  // For RFC 3720, 0xffffffff
    for (i = 0; i < num; i++) {
        data = ((u64 *)buffer)[i];
		crc = __crc32c_one_u64(data, crc);
    }

    return crc;                      // For RFC 3720, XOR with 0xffffffff
}

/* GF multiply */
static inline unsigned char __attribute__((unused)) gfmul(const unsigned char a, const unsigned char b) {
    unsigned int log;
    if (a == 0 || b == 0) {
        return 0;
    }

    log = gflog_base[a] + gflog_base[b];
    if (log > 255) {
        log = log - 255;
    }
    return gff_base[log];
}


/* GF division. Same as multiply except subtract instead of add */
static inline unsigned char gfdiv(const unsigned char num, const unsigned char denom) {
    int log;
    if (num == 0) {
        return 0;
    }
    BUG_ON(denom == 0);
    log = gflog_base[num] - gflog_base[denom];
    if ((int)log <= 0) {
        log = log + 255;
    }
    return gff_base[log];
}


/* GF Q step (q ^ d) * g */
static inline unsigned char gfmul_q(unsigned char q, const unsigned char d) {
    unsigned char hibit;
    hibit = q & 0x80;
    q = d ^ (q << 1);
    if (hibit) {
        q ^= POLY;
    }
    return q;
}

#define HIBITS  0x8080808080808080ULL
#define MASK    0xfefefefefefefefeULL
/* GF Q step wide (q ^ d) * g */
static inline u64 gfmul_q_64(const u64 q, const u64 d) {
    u64 mask;
    u64 result;

    mask = q & HIBITS;      // Isolate high bit of each byte. We're working in gf(8).
    /*
       Magic. 0x100 - 1 == 0xFF, so if the high bit of the byte is set, we get
       0xff in that byte, else zero. For the high byte, the bit is shifted out,
       but 0x00 - 1 is also 0xFF.
    */
    mask = (mask << 1) - (mask >> 7);
    /*
       The rest of the magic, from folks more clever than I. A multi-byte shift
       left needs the low byte of each byte cleared so that it looks like each
       byte was separately shifted left. THEN, we XOR in the polynomial for each
       byte that had a high bit set to start with.
    */
    result = (d ^ ((q << 1) & MASK)) ^ (mask & POLY64);
    return result;
}


/* GF mul by g */
static inline u64 gfmul_g_64(const u64 q) {
    u64 mask;
    u64 result;

    mask = q & HIBITS;      // Isolate high bit of each byte. We're working in gf(8).
    /*
       Magic. 0x100 - 1 == 0xFF, so if the high bit of the byte is set, we get
       0xff in that byte, else zero. For the high byte, the bit is shifted out,
       but 0x00 - 1 is also 0xFF.
    */
    mask = (mask << 1) - (mask >> 7);
    /*
       The rest of the magic, from folks more clever than I. A multi-byte shift
       left needs the low byte of each byte cleared so that it looks like each
       byte was separately shifted left. THEN, we XOR in the polynomial for each
       byte that had a high bit set to start with.
    */
    result = ((q << 1) & MASK) ^ (mask & POLY64);
    return result;
}


/* 64-bit GF mul. One bit at a time, 8 parallel bytes at a time. */
static inline u64 gf_mul_64(unsigned char factor, u64 val) {
	int i;
	u64 ret = 0;

	/* Each bit set in factor xor's val*g**bit */
	for (i = 0; i < 8; i++) {
		if (factor & 1) {
			ret ^= val;
		}
		factor >>= 1;
		val = gfmul_g_64(val);
	}
	return ret;
}


/* GF multiply g**pow with b */
static inline unsigned char gfpowmul(const unsigned char pow, const unsigned char b) {
    unsigned int power;

    if (b == 0){
        return 0;
    }
    power = pow + gflog_base[b];
    if (power > 255) {
        power = power - 255;
    }
    return gff_base[power];
}


/* Repeatedly multiply by g. Best for small values of pow. */
static inline u64 gfpowmul_64(const int pow, u64 b) {
	int j;

	for (j = 0; j < pow; j++) {
		b = gfmul_g_64(b);
	}

	return b;
}


#define LOBITS 	0x0101010101010101ULL
#define POLYROT 0x8E8E8E8E8E8E8E8EULL

/* Divide by g */
static inline u64 gfdiv_g_64(const u64 q) {
	u64 mask;
	u64 result;

	mask = q & LOBITS;			/* Instead of high bits, because rot right. */
	mask = (mask << 8) - mask;	/* 0xff iff bit0 was set. */
	result = ((q & ~LOBITS) >> 1) ^ (mask & POLYROT);	// divide by g

	return result;
}


/* Repeatedly divide by g. Best for small values of pow. */
static inline u64 gfpowdiv_64(const int pow, u64 b) {
	int j;

	for (j = 0; j < pow; j++) {
		b = gfdiv_g_64(b);
	}

	return b;
}


static void ec_encode_data_p_8(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    int i, j;
    unsigned char p;
	unsigned char *data_p[GF_MAX_D];

    /* Simple P calculation - if not NULL */
    /* Iterate down the buffer */
	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			data_p[i] = data_copy[i];
			memcpy(data_p[i], data[i], len);
		}
		else
			data_p[i] = data[i];
	}
    for (j = 0; j < len && coding[0]; j++) {
        p = 0;
        /* For each byte offset in the buffer, calculate across the buffers. */
        for (i = 0; i < rows; i++) {
            p ^= data_p[i][j];
        }
        coding[0][j] = p;
    }
    if (crc) {
        for (j = 0; j < rows; j++) {
            crc[j] = __calculate_crc32c(crc[j], data_p[j], len);
        }
        crc[rows] = __calculate_crc32c(crc[rows], coding[0], len);
    }
}

static void ec_encode_data_q_8(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    int i, j;
    unsigned char q;
	unsigned char *data_p[GF_MAX_D];

    /* Simple Q calculation - if not NULL */
	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			data_p[i] = data_copy[i];
			memcpy(data_p[i], data[i], len);
		}
		else
			data_p[i] = data[i];
	}
    for (j = 0; j < len && coding[1]; j++) {
        q = 0;
        for (i = rows-1; i >= 0; i--) {
            q = gfmul_q(q, data_p[i][j]);
        }
        coding[1][j] = q;
    }
    if (crc) {
        for (j = 0; j < rows; j++) {          // K is implicitly 2
                crc[j] = __calculate_crc32c(crc[j], data_p[j], len);
        }
        crc[rows+1] = __calculate_crc32c(crc[rows+1], coding[1], len);
    }

}

static void ec_encode_data_pq_8(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
	unsigned char *data_p[GF_MAX_D];
	int i;

	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			data_p[i] = data_copy[i];
			memcpy(data_p[i], data[i], len);
		}
		else
			data_p[i] = data[i];
	}
    (*gf_funcs.ec_encode_p)(len, rows, data_p, coding, NULL, NULL);
    (*gf_funcs.ec_encode_q)(len, rows, data_p, coding, crc, NULL);      // Very inefficient. Tough.
    if (crc) {
       crc[rows] = __calculate_crc32c(crc[rows], coding[0], len);
    }

}


static void ec_encode_data_p_64(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    int i, j, ll;
    u64 p;
	u64 dat;
	unsigned char *data_p[GF_MAX_D];

	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			data_p[i] = data_copy[i];
			memcpy(data_p[i], data[i], len);
		}
		else
			data_p[i] = data[i];
	}

	ll = len/sizeof(u64);

    for (j = 0; j < ll; j++) {                                     // Walk down all the buffers and calculate XOR
        p = 0;

		/* For each byte offset in the buffer, calculate across the buffers. (rows) */
        for (i = 0; i < rows; i++) {
			dat = ((u64*)data_p[i])[j];
            p ^= dat;
			if (crc) {
				crc[i] = __crc32c_one_u64(dat, crc[i]);
			}
        }
        ((u64*)coding[0])[j] = p;
		if (crc) {
			crc[rows] = __crc32c_one_u64(p, crc[rows]);
		}
	}

}

static void ec_encode_data_q_64(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    int i, j, ll;
    u64 q;
	u64 dat;
	unsigned char *data_p[GF_MAX_D];

	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			data_p[i] = data_copy[i];
			memcpy(data_p[i], data[i], len);
		}
		else
			data_p[i] = data[i];
	}

	ll = len/sizeof(u64);

    for (j = 0; j < ll; j++) {
		q = 0;

		for (i = rows - 1; i >= 0; i--) {
			dat = ((u64*)data_p[i])[j];
            q = gfmul_q_64(q, dat);
			if (crc) {
				crc[i] = __crc32c_one_u64(dat, crc[i]);
			}
		}
        ((u64*)coding[1])[j] = q;
		if (crc) {
			crc[rows+1] = __crc32c_one_u64(q, crc[rows+1]);
		}
	}

}

static void ec_encode_data_pq_64(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    int i, j, ll;
    u64 p, q, temp;
	unsigned char *data_p[GF_MAX_D];

	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			data_p[i] = data_copy[i];
			memcpy(data_p[i], data[i], len);
		}
		else
			data_p[i] = data[i];
	}

	ll = len/sizeof(u64);

	/* Only minor magic here. Q is calculated by gfmul(g): D0*g**0 + D1*g**1, etc.
     So instead we calculated it ((((Dn-1*g) + (Dn-2))*g + Dn-3)*g ...) + D0
     This means we calculate the buffers in reverse order to get cheap multiplies.
     P doesn't care about the order, so it goes along for the ride.
    */
    for (j = 0; j < ll; j++) {
		p = ((u64*)data_p[rows-1])[j];          // P init from last D
		q = p;                                // Q init from last D. Have to go from last to first.
		if (crc) {
			crc[rows-1] = __crc32c_one_u64(q, crc[rows-1]);
		}
		for (i = rows - 2; i >= 0; i--) {
			temp = ((u64*)data_p[i])[j];
			p ^= temp;
			q = gfmul_q_64(q, temp);
			if (crc) {
				crc[i] = __crc32c_one_u64(temp, crc[i]);
			}
		}
		((u64*)coding[0])[j] = p;
		((u64*)coding[1])[j] = q;
		if (crc) {
			crc[rows] = __crc32c_one_u64(p, crc[rows]);
			crc[rows+1] = __crc32c_one_u64(q, crc[rows+1]);
		}
    }

}

/*
   Generate the P[] encoding of the given data. Use 3 indeirect calls.
   len: buffer length in bytes
   k: number of P (1 = RAID5, 2=RAID6, etc.)
   rows: Number of D buffers
   data[]: D buffer pointers, in order 0 - rows-1
   coding[]: New P buffer pointers, in order 0 - k-1
   crc: array of d+k u32 to hold a CRC for each disk. The CRC is only meaningful across 4K.
        The seed of the crc is passed in the array of crcs. The answer is returned in the same place.
        A pointer of NULL means that the crc is not wanted.
*/
static enum gf_return_val ec_encode_data_1(int len, int k, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {

    /* Simple sanity checks. Some can go away later. */
    WARN((len == 0)||(k == 0)||(k > 2), "nvmeibc bug: len=%d, k=%d", len, k); // For now, at least.
    /*
       There are 6 possibilities for this. We can either code a lot of stuff
       multiple times, or put up with 'if' statements in the middle of loops.
       The cases:
       1) k==1, coding buffer exists.
       2) k==1, coding buffer doesn't exist (NOP).
       3) k==2, P buffer exists, Q buffer exists.
       4) k==2, P buffer doesn't exist.
       5) k==2, Q buffer doesn't exist.
       6) k==2, neither buffer exists (NOP).

       Cases 2 and 6 can be tested outside the loop.
       Case 1 and case 5 are the same.

       So that leaves 3 cases: 1, 3, and 4.

       To be the most efficient, we want to touch a word in a buffer only once
       for both the P and Q. So we have to either test inside the loop whether
       to do P and/or Q, or we have to have 3 loops. So, let's be performance
       efficient instead of worrying too much about the code.
    */

    if (((k == 1) && !coding[0]) ||                                 // case 2
        ((k == 2 && !coding[0]) && !coding[1])) {       // case 6
        // Just say success
    } else if ((k == 1) ||                                                  // case 1, we already know P buffer is there
                 ((k == 2) && !coding[1])) {                    // case 5, we already know at least one buffer is there
        (*gf_funcs.ec_encode_p)(len, rows, data, coding, crc, data_copy);
    } else if (coding[0]) {                                 // case 3. We know that Q exists, and that P exists, and that k isn't 1.
        (*gf_funcs.ec_encode_pq)(len, rows, data, coding, crc, data_copy);
    } else {                                                        // There's nothing left but case 4.
        (*gf_funcs.ec_encode_q)(len, rows, data, coding, crc, data_copy);
    }

    return GF_SUCCESS;

}

/*
   Update the Ps for new data (RMW).
   len: buffer length in bytes
   k: number of P (1 = RAID5, 2=RAID6, etc.)
   rows: Number of D buffers
   vec_i: index of D to update
   data: K+2 buffer pointers (Old D, New D, Old Ps)
   coding: (New Ps)

   New Ps and Old Ps may be the same, so use a scratch buffer if required.

   crc is an array of K+1 entries, that hold the encoded buffer crc, and the P and (maybe) Q crcs.
*/
static enum gf_return_val ec_encode_data_update_8(int len, int k, int vec_i, unsigned char **data, unsigned char **coding, u32 *crc, unsigned char *data_copy)
{
    unsigned char old_d;
    unsigned char new_d;
    unsigned char p, q;
    int i;

	WARN((len == 0)||(k == 0)||(k > 2), "nvmeibc bug: len=%d, k=%d", len, k); // For now, at least.

	if (data_copy)
		memcpy(data_copy, data[1], len);
	else
		data_copy = data[1];
    /* Iterate down the buffer. */
    for (i = 0; i < len; i++) {
        old_d = data[0][i];
        new_d = data_copy[i];

        switch (k) {
        case 2:
            q = data[3][i];
            /*
               NOTE: GF arithmetic

               Q = g**0 D0 + g**1 D1 etc.
               Q'= g**0 D0 + g**1 D1' etc.
               Xor the two equations:
               Q' = Q + g**1 (D1 + D1')
               More generally:
               Q' = Q + g**x * (Dx + Dx')
               Multiply by using log tables
               Q' = Q + alog(x + log(Dx + Dx'))  (Here it really is integer addition inside the alog)
            */
            q ^= gfpowmul(vec_i, old_d ^ new_d);
            coding[1][i] = q;
                        FALLTHRU;
        case 1:
            p = data[2][i];
            /* parity - doesn't matter what index. The easy case. */
            p ^= old_d ^ new_d;
            coding[0][i] = p;
            break;
        }
    }
    if (crc) {
        crc[0] = __calculate_crc32c(    crc[0], data[  1], len);     // new data
        crc[1] = __calculate_crc32c(    crc[1], coding[0], len);     // P
        if (k == 2) {
            crc[2] = __calculate_crc32c(crc[2], coding[1], len);     // Maybe Q
        }
    }

    return GF_SUCCESS;
}

/*
    There's one data disk missing, and P is available.
    Fix it with P. Q is not necessary.
    K can be assumed to be one because Q is not used.
    d0 is the index of the missing data drive.
*/
static void ec_decode_data_p_8(int len, int rows, int d0, unsigned char **data, unsigned char **new_data, u32 *crc)
{
    int i, j;
    unsigned char temp_p;

    for (j = 0; j < len; j++) {
        temp_p = 0;
        for (i = 0; i < rows; i++) {
            if (d0 != i) {                          // Not the missing disk.
                temp_p ^= data[i][j];
            }
        }
        new_data[0][j] = data[rows][j] ^ temp_p;    // Save parity.
    }

    if (crc) {
        crc[0] = __calculate_crc32c(crc[0], new_data[0], len);
    }
}


/*
    There's one data disk missing, and Q is available.
    Fix it with Q. P is missing and not necessary.
    K can be assumed to be two, because Q is always used.
    d0 is the index of the missing data drive.
*/
static void ec_decode_data_q_8(int len, int rows, int d0, unsigned char **data, unsigned char **new_data, u32 *crc)
{
    int i, j;
    unsigned char temp_d, temp_q;

    /* GF Arithmetic:

       Q = gfsum(g**n * Dn)                                                  (1)
       Q'= Q + g**x * Dx                                                     (2)
       Dx = (Q + Q') * g**-x                                                 (3)
    */
    for (j = 0; j < len; j++) {
        temp_q = 0;
        for (i = rows-1; i >= 0; i--) {
            if (d0 == i) {                          // The missing disk.
                temp_d = 0;                         // "erased".
            } else {
                temp_d = data[i][j];
            }
            temp_q = gfmul_q(temp_q, temp_d);
        }
        temp_q = data[rows+1][j] ^ temp_q;        // Save Q.

        /* Now divide by g**x (multiply by g**-x, which is the same thing as multiply by g**(255-x) */
        new_data[0][j] = gfpowmul(255-d0, temp_q);
    }

    if (crc) {
        crc[0] = __calculate_crc32c(crc[0], new_data[0], len);
    }
}


/*
    There are two data disks missing, and P and Q are available.
    Fix it with P and Q.
    K can be assumed to be two, because P and Q are always used.
    d0 is the index of the first missing data drive. d1 is the index of the second data drive.
    d0 < d1.
*/
static void ec_decode_data_pq_8(int len, int rows, int d0, int d1, unsigned char **data, unsigned char **new_data, u32 *crc)
{
    int i, j;
    unsigned char temp_d, temp_p, temp_q, numerator, denominator, dx;

    /* GF Arithmetic:

       P = gfsum(Dn)
       Q = gfsum(q**n * Dn)
       Dx and Dy are missing
       Dx + Dy = P + P'                                                      (1)
       g**x * Dx + g**y * Dy = Q + Q'                                        (2)
       Dy = P + P' + Dx                                 (solve 1 for Dy)     (3)
       g**x * Dx + g**y * (P + P' + Dx)  = Q + Q'       (Subsitute 3 into 2) (4)
       g**x * Dx + g**y * Dx = Q + Q' + g**y * (P + P') (solve 4 for Dx)
       (g**x + g**y) * Dx = Q + Q' + g**y * (P + P')    ("")
       Dx = (Q + Q' + g**y * (P + P'))/(g**x + g**y)    ("")
       Then figure out Dy using (1).
       Dy = Dx + P + P'
    */
    for (j = 0; j < len; j++) {
        temp_p = 0;
        temp_q = 0;
        /* Calculate P' and Q' (missing data blocks are zero)(erasure) */
        for (i = rows-1; i >= 0; i--) {
            if (d0 == i || d1 == i) {           // The missing ones are 0 ("erased").
                temp_d = 0;
            } else {
                temp_d = data[i][j];
            }
            temp_p ^= temp_d;
            temp_q = gfmul_q(temp_q, temp_d);
        }
        temp_p ^= data[rows][j];                // plus P, gives P + P'
        temp_q ^= data[rows+1][j];              // plus Q, gives Q + Q'


        numerator = temp_q ^ gfpowmul(d1, temp_p); // d1 is y
        denominator = gff_base[d0] ^ gff_base[d1];

        /* Now we can multiply the numerator * 1/denominator */
        dx = gfdiv(numerator, denominator);

        new_data[0][j] = dx;                    // Save reconstructed d0.
        if (data[d1] != (void *)(-1)) {          // If we have a valid (not -1) pointer, then
            new_data[1][j] = dx ^ temp_p;       // Save reconstructed d1.
        }
    }

    if (crc) {
        if (data[d0] != (void *)(-1)) {
            crc[0] = __calculate_crc32c(crc[0], new_data[0], len);
        }
        if (data[d1] !=  (void *)(-1)) {
            crc[1] = __calculate_crc32c(crc[1], new_data[1], len);
        }
    }
}


static enum gf_return_val ec_encode_data_update_64(int len, int k, int vec_i, unsigned char **data,  unsigned char **coding, u32 *crc, unsigned char *data_copy)
{
	u64 old_d;
	u64 new_d;
	u64 p, q;
	int i, ll;
	u32 dcrc, pcrc, qcrc;

	dcrc = pcrc = qcrc = 0;

	WARN((len % sizeof(u64))||(k == 0)||(k > 2), "nvmeibc bug: len=%d, k=%d", len, k); // For now, at least.
	ll = len/sizeof(u64);
	if (data_copy)
		memcpy(data_copy, data[1], len);
	else
		data_copy = data[1];

	if (crc) {
		dcrc = crc[0];
		pcrc = crc[1];
		if (k > 1) {
			qcrc = crc[2];
		}
	}

	/* Iterate down the buffer. */
	for (i = 0; i < ll; i++) {
		old_d = ((u64*)data[0])[i];
		new_d = ((u64*)data_copy)[i];

		/* Always have k >= 1 */
		p = ((u64*)data[2])[i];
		p ^= old_d ^ new_d;
		((u64*)coding[0])[i] = p;

		if (crc) {
			dcrc = __crc32c_one_u64(new_d, dcrc);
			pcrc = __crc32c_one_u64(p, pcrc);
		}

		if (k > 1) {
			/*
			   NOTE: GF arithmetic
			    Q' = Q + g**x * (Dx + Dx')
			*/
			q = old_d ^ new_d;		// Dx + Dx'
			q = gfpowmul_64(vec_i, q);
			q ^= ((u64*)data[3])[i];
			((u64*)coding[1])[i] = q;

			if (crc) {
				 qcrc = __crc32c_one_u64(q, qcrc);
			}
		}
	}
	if (crc) {
		crc[0] = dcrc;
		crc[1] = pcrc;
		if (k > 1) {
			crc[2] = qcrc;
		}
	}

    return GF_SUCCESS;
}


/*
    There's one data disk missing, and P is available.
    Fix it with P. Q is not necessary.
    K can be assumed to be one because Q is not used.
    d0 is the index of the missing data drive.
*/
static void ec_decode_data_p_64(int len, int rows, int d0, unsigned char **data, unsigned char **new_data, u32 *crc) {
    int i, j, ll;
    u64 temp_p;
	u32 dcrc;

	dcrc = 0;

	ll = len/sizeof(u64);
	if (crc) {
		dcrc = crc[0];
	}

    for (j = 0; j < ll; j++) {
        temp_p = 0;
        for (i = 0; i < rows; i++) {
            if (d0 != i) {                          // Not the missing disk.
                temp_p ^= ((u64*)data[i])[j];
				if (crc) {
				}
            }
        }
		temp_p ^= ((u64*)data[rows])[j];			// Old parity.
        ((u64*)new_data[0])[j] = temp_p;    		// Save parity.
		if (crc) {
			dcrc = __crc32c_one_u64(temp_p, dcrc);
		}
    }

    if (crc) {
        crc[0] = dcrc;
    }
}


/*
    There's one data disk missing, and Q is available.
    Fix it with Q. P is missing and not necessary.
    K can be assumed to be two, because Q is always used.
    d0 is the index of the missing data drive.
*/
static void ec_decode_data_q_64(int len, int rows, int d0, unsigned char **data, unsigned char **new_data, u32 *crc) {
    int i, j, ll;
    u64 temp_d, temp_q;
	u32 dcrc;

	dcrc = 0;
	ll = len/sizeof(u64);
	if (crc) {
		dcrc = crc[0];
	}

    /* GF Arithmetic:

       Q = gfsum(g**n * Dn)                                                  (1)
       Q'= Q + g**x * Dx                                                     (2)
       Dx = (Q + Q') / g**x                                                  (3)
    */
    for (j = 0; j < ll; j++) {
		/* Calculate Q' */
        temp_q = 0;
        for (i = rows-1; i >= 0; i--) {
            if (d0 == i) {                          // The missing disk.
                temp_d = 0;                         // "erased".
            } else {
                temp_d = ((u64*)data[i])[j];
            }
            temp_q = gfmul_q_64(temp_q, temp_d);
		}
        temp_q = ((u64*)data[rows+1])[j] ^ temp_q;        // Save Q ^ Q'.

		/* Now divide by g**x. We could just multiply by g**(255-x), but without
		   using tables we can only multiply by g, and 255-x is a big number.
		   Instead, divide by g x times.
		   To divide by g, right shift instead of left, and use rotated poly.
		*/
		temp_q = gfpowdiv_64(d0, temp_q);
		((u64*)new_data[0])[j] = temp_q;
		if (crc) {
			dcrc = __crc32c_one_u64(temp_q, dcrc);
		}
	}

    if (crc) {
        crc[0] = dcrc;
    }
}


/*
    There are two data disks missing, and P and Q are available.
    Fix it with P and Q.
    K can be assumed to be two, because P and Q are always used.
    d0 is the index of the first missing data drive. d1 is the index of the second data drive.
    d0 < d1.
*/
static void ec_decode_data_pq_64(int len, int rows, int d0, int d1, unsigned char **data, unsigned char **new_data, u32 *crc) {
    int i, j, ll;
    u64 temp_d, temp_p, temp_q, numerator, denominator;
	u32 crc0, crc1;
	unsigned char factor;

	crc0 = 0;
	crc1 = 0;

	if (crc) {
		crc0 = crc[0];
		crc1 = crc[1];
	}

	ll = len/sizeof(u64);

    /* GF Arithmetic:

       P = gfsum(Dn)
       Q = gfsum(q**n * Dn)
       Dx and Dy are missing
       Dx + Dy = P + P'                                                      (1)
       g**x * Dx + g**y * Dy = Q + Q'                                        (2)
       Dy = P + P' + Dx                                 (solve 1 for Dy)     (3)
       g**x * Dx + g**y * (P + P' + Dx)  = Q + Q'       (Subsitute 3 into 2) (4)
       g**x * Dx + g**y * Dx = Q + Q' + g**y * (P + P') (solve 4 for Dx)
       (g**x + g**y) * Dx = Q + Q' + g**y * (P + P')    ("")
       Dx = (Q + Q' + g**y * (P + P'))/(g**x + g**y)    ("")
       Then figure out Dy using (1).
       Dy = Dx + P + P'
    */
	denominator = gff_base[d0] ^ gff_base[d1];
	factor = gff_base[255-gflog_base[denominator]]; // 1/denominator

	for (j = 0; j < ll; j++) {
        temp_p = 0;
        temp_q = 0;
        /* Calculate P' and Q' (missing data blocks are zero)(erasure) */
        for (i = rows-1; i >= 0; i--) {
            if (d0 == i || d1 == i) {           // The missing ones are 0 ("erased").
                temp_d = 0;
            } else {
                temp_d = ((u64*)data[i])[j];
            }
            temp_p ^= temp_d;
            temp_q = gfmul_q_64(temp_q, temp_d);
        }
        temp_p ^= ((u64*)data[rows])[j];                // plus P, gives P + P'
        temp_q ^= ((u64*)data[rows+1])[j];              // plus Q, gives Q + Q'

		numerator = gfpowmul_64(d1, temp_p) ^ temp_q;

		temp_d = gf_mul_64(factor, numerator);

        ((u64*)new_data[0])[j] = temp_d;                // Save reconstructed d0.
		if (crc) {
			crc0 = __crc32c_one_u64(temp_d, crc0);
		}
		if (data[d1] != (void *)(-1)) {          // If we have a valid (not -1) pointer, then
			temp_d = temp_d ^ temp_p;
			((u64*)new_data[1])[j] = temp_d;       		// Save reconstructed d1.
			if (crc) {
				crc1 = __crc32c_one_u64(temp_d, crc1);
			}
		}
    }

	if (crc) {
		crc[0] = crc0;
		if (data[d1] !=  (void *)(-1)) {
			crc[1] = crc1;
		}
	}
}

/*
   Correct read data buffer. Only useful for reads.

   len: buffer length in bytes
   k: number of P (1 = RAID5, 2=RAID6, etc.)
   rows: Number of D buffers
   data: rows+k buffer pointers (Old D, Old Ps)
        (Unavalable buffers are nullptr.)
        (Reduced buffers are -1, and aren't used to repair, and aren't part of new_data).
   new_data: (New Ds) (From ptr 0 D pointers)
*/
static enum gf_return_val ec_decode_data_1(int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, u32 *crc) {
    /*
       Cases:
       1) P or Q missing - don't bother me.
       2) P and Q missing - don't bother me.
       3) One D missing - fix it with P.
       4) One D and Q missing - fix it with P. Also use for degraded Q.
       5) One D and P missing - fix it with Q. Also use for degraded P.
       6) Two D missing - fix it with P & Q.
            6a) One D missing, one D degraded. This is the only case where we'll
                see degraded. We don't care which is degraded, just which one(s)
                are desired. Ptr 0 means we want it. Ptr -1 means we don't.
       Anything else (k > 2) comes later, and probably requires matrix inversion.
    */
    int i, d_index, p_index, d[2], d_notme;

    BUG_ON(k > 2); // Not for now

    /* Get a list of all the missing Ds. Keep track of which Ds we want. */
    d_index = 0;
    d_notme = -1;
    for (i = 0; i < rows; i++) {
        if (data[i] == (void *)0 || data[i] == (void *)(-1)) {
            BUG_ON(d_index > k);
            d[d_index] = i;
            d_index++;
        }
        if (data[i] == (void *)(-1)) {              // Don't want this D.
            BUG_ON(d_notme != -1);                  // Change this for k > 2.
            d_notme = i;
        }
    }
    BUG_ON(d_index == 0);

    /* Get a list of all the bad Ps */
    BUG_ON(new_data[0] == 0);
    p_index = 0;

    for (i = 0; i < k; i++) {
		if (data[rows+i] == (void *)(-1)) {       	// "reduced" parity?
			data[rows+i] = 0;
		}
		if (data[rows+i] == 0) {
			BUG_ON(d_index + p_index > k);
            p_index++;
        }
    }

    /* Cases 3 and 4: one D missing, parity not missing. Fix it with parity. */
    if (d_index == 1 && d_index + p_index <= k && data[rows] != 0) {
        (*gf_funcs.ec_decode_p)(len, rows, d[0], data, new_data, crc);
    }

    /* Everything after this requires at least 2 parity. */
    else if (k < 2) {
        BUG_ON(k < 2);
    }

    /* Case 5: One D and parity missing. Fix it with Q.*/

    else if (d_index == 1 && p_index == 1 && data[rows] == 0) {
        (*gf_funcs.ec_decode_q)(len, rows, d[0], data, new_data, crc);
    }

    /* Case 6: Two D and no parity missing. Fix it with P and Q (oh, joy).
       Might also be one D missing and one D degraded. */

    else if (d_index == 2 && p_index == 0) {
        /* To make things easier, always put notme last. */
        /* 'd_notme' is a reduced disk, but we don't want its value. */
        if (d[0] == d_notme) {
            d[0] = d[1];
            d[1] = d_notme;
        }
        if (d[1] == d_notme){
            new_data[1] = NULL;
        }

        (*gf_funcs.ec_decode_pq)(len, rows, d[0], d[1], data, new_data, crc);
    } else {
        BUG_ON(1);      // For now, anyway. k > 2.
    }

    return GF_SUCCESS;
}


/*
   Each table will start with one of these functions. Each function sets up the
   tables, then calls the new function value.
*/
static enum gf_return_val ec_encode_data_init(int len, int k, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);      // Init function vectors.
    return (*gf_funcs.ec_encode)(len, k, rows, data, coding, crc, data_copy);
}
static enum gf_return_val ec_encode_data_update_init(int len, int k, int vec_i, unsigned char ** data,  unsigned char ** coding, u32 *crc, unsigned char *data_copy) {
    __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);
    return (*gf_funcs.ec_update)(len, k, vec_i, data, coding, crc, data_copy);
}
static enum gf_return_val ec_decode_data_init(int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, u32 *crc) {
    __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);
    return (*gf_funcs.ec_decode)(len, k, rows, data, new_data, crc);
}

/*
   The processor capabilities will determine which instance of an algorithm that
   we're going to use. For each algorithm, have an array of functions, and a
   pointer to the function that we've chosen. The generic name will invoke the
   chosen function.
*/

typedef void (*generic_func) (void);

gf_functions_t gf_funcs = {
	NVMEIBC_GF_AUTO_INIT,
	ec_encode_data_init,
	0,
	0,
	0,
	ec_encode_data_update_init,
	ec_decode_data_init,
	0,
	0,
	0,
	0,
};

#if defined(__KERNEL__)
gf_functions_t gf_funcs_kernel = {	// built-in kernel raid6 with raid_call struct
	NVMEIBC_GF_EC_CALC,
	nvmeib_raid_encode,
	0,
	0,
	0,
	nvmeib_raid_update,
	nvmeib_raid_decode,
	0,
	0,
	0,
	nvmeib_raid_ec_crc,
};
#else
	__attribute__((unused)) static inline bool nvmeib_raid_kernel_builtin_supported(void) { return false; }
#endif

#ifdef NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
gf_functions_t gf_funcs_arm = {
	NVMEIBC_GF_ARM_INTRINSICS,
	ec_encode_data_arm_optimized,
	0,
	0,
	0,
	ec_encode_data_update_arm_optimized,
	ec_decode_data_arm_optimized,
	0,
	0,
	0,
	ec_crc_arm_optimized,
};
#endif

gf_functions_t gf_functions_unoptimized = {
	NVMEIBC_GF_UNOPTIMIZED,
	ec_encode_data_1,
	ec_encode_data_p_8,
	ec_encode_data_q_8,
	ec_encode_data_pq_8,
	ec_encode_data_update_8,
	ec_decode_data_1,
	ec_decode_data_p_8,
	ec_decode_data_q_8,
	ec_decode_data_pq_8,
	CRC32_KERNEL_OR_UNOPT_FN,
};

gf_functions_t gf_functions_u64 = {
	NVMEIBC_GF_64_BIT,
	ec_encode_data_1,
	ec_encode_data_p_64,
	ec_encode_data_q_64,
	ec_encode_data_pq_64,
	ec_encode_data_update_64,
	ec_decode_data_1,
	ec_decode_data_p_64,
	ec_decode_data_q_64,
	ec_decode_data_pq_64,
	CRC32_KERNEL_OR_AVX_FN,
};

#ifdef USE_GF_SSE2
/* All of the following is not really done yet and may never be done... */
gf_functions_t gf_functions_sse2 = {
	NVMEIBC_GF_SSE2,
	ec_encode_data_1,
	ec_encode_data_p_64,
	ec_encode_data_q_64,
	ec_encode_data_pq_64,
	ec_encode_data_update_8,
	ec_decode_data_1,
	ec_decode_data_p_8,
	ec_decode_data_q_8,
	ec_decode_data_pq_8,
};
#endif

#if defined(USE_GF_AVX2)
// Functions below are implemented in assembly directly
extern void ec_encode_data_p_avx2(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy);
extern void ec_encode_data_q_avx2(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy);
extern void ec_encode_data_pq_avx2(int len, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy);
extern enum gf_return_val ec_encode_data_update_avx2(int len, int k, int vec_i, unsigned char ** data,  unsigned char ** coding, u32 *crc, unsigned char *data_copy);
extern void ec_decode_data_p_avx2(int len, int rows, int d0, unsigned char ** data,  unsigned char ** new_data, u32 *crc);
extern void ec_decode_data_q_avx2(int len, int rows, int d0, unsigned char ** data,  unsigned char ** new_data, u32 *crc);
extern void ec_decode_data_pq_avx2_asm(int len, int rows, int d0, int d1, unsigned char ** data,  unsigned char ** new_data, u32 *crc, unsigned char factor);
static void ec_decode_data_pq_avx2(int len, int rows, int d0, int d1, unsigned char ** data,  unsigned char ** new_data, u32 *crc) {
	#ifndef __aarch64__
		const unsigned char denominator = gff_base[d0] ^ gff_base[d1];
		const unsigned char factor = gff_base[255-gflog_base[denominator]];
		ec_decode_data_pq_avx2_asm(len, rows, d0, d1, data, new_data, crc, factor);
	#else
		return ec_decode_data_pq_64(len, rows, d0, d1, data, new_data, crc);	// No AVX
	#endif
}

gf_functions_t gf_functions_avx2 = {
	NVMEIBC_GF_AVX2,
	ec_encode_data_1,
	ec_encode_data_p_avx2,
	ec_encode_data_q_avx2,
	ec_encode_data_pq_avx2,
	ec_encode_data_update_avx2,
	ec_decode_data_1,
	ec_decode_data_p_avx2,
	ec_decode_data_q_avx2,
	ec_decode_data_pq_avx2,
	CRC32_KERNEL_OR_AVX_FN,
};
#endif

/******* Below functions to determine AVX/SSE support *****************/
#if defined(__x86_64__) || defined(__i386__)
	#define ___cpuid(level, a, b, c, d)	\
	__asm__ ("cpuid\n\t" : "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "0" (level))
	#define __cpuid_count(level, count, a, b, c, d)	\
	__asm__ ("cpuid\n\t" : "=a" (a), "=b" (b), "=c" (c), "=d" (d) : "0" (level), "2" (count))

	/* Return highest supported input value for cpuid instruction.  ext can
	be either 0x0 or 0x80000000 to return highest supported value for
	basic or extended cpuid information.  Function returns 0 if cpuid
	is not supported or whatever cpuid returns in eax register.  If sig
	pointer is non-null, then first four bytes of the signature
	(as found in ebx register) are returned in location pointed by sig.  */
	static inline unsigned int __get_cpuid_max(unsigned int __ext, unsigned int *__sig)
	{
		unsigned int __eax, __ebx, __ecx __attribute__((unused)), __edx __attribute__((unused));
		#ifndef __x86_64__
			// See if we can use cpuid.  On AMD64 we always can.
			#if __GNUC__ >= 3
				__asm__ ("pushf{l|d}\n\t"
					"pushf{l|d}\n\t"
					"pop{l}\t%0\n\t"
					"mov{l}\t{%0, %1|%1, %0}\n\t"
					"xor{l}\t{%2, %0|%0, %2}\n\t"
					"push{l}\t%0\n\t"
					"popf{l|d}\n\t"
					"pushf{l|d}\n\t"
					"pop{l}\t%0\n\t"
					"popf{l|d}\n\t"
					: "=&r" (__eax), "=&r" (__ebx)
					: "i" (0x00200000));
			#else
				//Host GCCs older than 3.0 weren't supporting Intel asm syntax nor alternatives in i386 code.
				__asm__ ("pushfl\n\t"
					"pushfl\n\t"
					"popl\t%0\n\t"
					"movl\t%0, %1\n\t"
					"xorl\t%2, %0\n\t"
					"pushl\t%0\n\t"
					"popfl\n\t"
					"pushfl\n\t"
					"popl\t%0\n\t"
					"popfl\n\t"
					: "=&r" (__eax), "=&r" (__ebx)
					: "i" (0x00200000));
			#endif
			if (!((__eax ^ __ebx) & 0x00200000))
				return 0;
		#endif

		/* Host supports cpuid.  Return highest supported cpuid input value.  */
		___cpuid(__ext, __eax, __ebx, __ecx, __edx);
		if (__sig)
			*__sig = __ebx;
		return __eax;
	}

	/* Return cpuid data for requested cpuid leaf, as found in returned
	eax, ebx, ecx and edx registers.  The function checks if cpuid is
	supported and returns 1 for valid cpuid information or 0 for
	unsupported cpuid leaf.  All pointers are required to be non-null.  */
	static inline int __get_cpuid(unsigned int __leaf,
			unsigned int *__eax, unsigned int *__ebx,
			unsigned int *__ecx, unsigned int *__edx)
	{
		const unsigned int __ext = __leaf & 0x80000000;
		const unsigned int __maxlevel = __get_cpuid_max (__ext, 0);
		if (__maxlevel == 0 || __maxlevel < __leaf)
			return 0;
		___cpuid(__leaf, *__eax, *__ebx, *__ecx, *__edx);
		return 1;
	}

	/* Same as above, but sub-leaf can be specified.  */
	static inline int __get_cpuid_count(unsigned int __leaf, unsigned int __subleaf,
			unsigned int *__eax, unsigned int *__ebx,
			unsigned int *__ecx, unsigned int *__edx)
	{
		const unsigned int __ext = __leaf & 0x80000000;
		const unsigned int __maxlevel = __get_cpuid_max (__ext, 0);
		if (__maxlevel == 0 || __maxlevel < __leaf)
			return 0;
		__cpuid_count(__leaf, __subleaf, *__eax, *__ebx, *__ecx, *__edx);
		return 1;
	}

	// Leaf 7.0 to look for AVX2 flag. EBX bit 5.
	static inline int get_avx2(void) {
		unsigned int a, b = 0, c, d;
		__get_cpuid_count(7, 0, &a, &b, &c, &d);
		return (b & bit_AVX2) != 0;
	}

	// Leaf 1 to find SSE2 flag. EDX bit 26.
	static inline int __attribute__((unused)) get_sse2(void) {
		unsigned int a, b, c, d = 0;
		__get_cpuid(1, &a, &b, &c, &d);
		return (d & bit_SSE2) != 0;
	}
#elif defined __aarch64__
	__attribute__((unused)) static inline int get_avx2(void) {
		return true; // YR: TODO: do this the "correct" way in a kernel. For now, we will go bonkers on a system without this command
		//return (HWCAP_CRC32 & getauxval(AT_HWCAP));
	}
	#define get_sse2 get_avx2
#endif

static bool gf_funcs_in_irq_ctx = true;

/********************* Choose a function and put it in the pointer ***********/
enum nvmeibc_gf_optimization_type __gf_choose_functions(enum nvmeibc_gf_optimization_type index)
{
	if (index == NVMEIBC_GF_DISPLAY_CURRENT) {
		return gf_funcs.id;
	}
	if (index == NVMEIBC_GF_AUTO_INIT) {
		#ifdef NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
				gf_funcs = gf_funcs_arm;
				return gf_funcs.id;
		#elif defined(__KERNEL__) && defined(__aarch64__)
			if (nvmeib_raid_kernel_builtin_supported()) {
				gf_funcs = gf_funcs_kernel;
				gf_funcs_in_irq_ctx = false;
				return gf_funcs.id;
			}
		#elif defined(USE_GF_AVX2)
			if (get_avx2()) {
				gf_funcs = gf_functions_avx2;
				return NVMEIBC_GF_AVX2;
			}
		#elif defined(USE_GF_SSE2)
			if (get_sse2()) {
				gf_funcs = gf_functions_sse2;
				return NVMEIBC_GF_SSE2;
			}
		#endif
			if (1) {		// Allways prefer 64bit cpu code over 8bit.
				gf_funcs = gf_functions_u64;
				return NVMEIBC_GF_64_BIT;
			} else {
				gf_funcs = gf_functions_unoptimized;
				return NVMEIBC_GF_UNOPTIMIZED;
			}
	}
	switch (index) {
	case NVMEIBC_GF_UNOPTIMIZED:	gf_funcs = gf_functions_unoptimized;	break;
	case NVMEIBC_GF_64_BIT:			gf_funcs = gf_functions_u64;			break;
	#ifdef USE_GF_SSE2
		case NVMEIBC_GF_SSE2:		gf_funcs = gf_functions_sse2;			break;
	#endif
	#ifdef USE_GF_AVX2
		case NVMEIBC_GF_AVX2:		gf_funcs = gf_functions_avx2;			break;
	#endif
	#ifdef NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
		case NVMEIBC_GF_ARM_INTRINSICS:	gf_funcs = gf_funcs_arm;			break;
	#endif
	#if defined(__KERNEL__)
		case NVMEIBC_GF_EC_CALC:
			if (!nvmeib_raid_kernel_builtin_supported()) {
				_NT(__AUTOID__, "Kernel EC calculation is not supported in current kernel");
				return -1;
			}
#		if defined(__aarch64__)
			if (gf_funcs_in_irq_ctx) {
				_NT(__AUTOID__, "Cannot choose Kernel EC calculation on ARM after startup");
				return -1;
			}
#		endif
			gf_funcs = gf_funcs_kernel;
			break;
	#endif
		default:
			#if defined(__KERNEL__)
				_NT(__AUTOID__, "Unknown gf_functions request, index=@INDEX\n", index);
			#endif
			return -1;
	}
	return index;
}

/* Returns true if GF calcs can be used in interrupt context */
bool nvmeibc_gf_calc_in_irq_ctx(void)
{
	return gf_funcs_in_irq_ctx;
}

/* Generic functions to call through vectors. */
enum gf_return_val ec_encode_data(gf_callback *callback, int len, int k, int rows, unsigned char ** data,  unsigned char ** coding, u32 *crc, unsigned char **data_copy) {
    enum gf_return_val rv;

    rv = (*gf_funcs.ec_encode)(len, k, rows, data, coding, crc, data_copy);
    if (callback) {
        (*callback)(NULL /*Daniel: pass 'data', 'coding' */, GF_SUCCESS);
        return GF_DEFERRED;
    }
    return rv;
}

enum gf_return_val ec_encode_data_update(gf_callback *callback, int len, int k, int vec_i, unsigned char ** data,  unsigned char ** coding, u32 *crc, unsigned char *data_copy) {
    enum gf_return_val rv;

    rv = (*gf_funcs.ec_update)(len, k, vec_i, data, coding, crc, data_copy);
    if (callback) {
        (*callback)(NULL, GF_SUCCESS);
        return GF_DEFERRED;
    }
    return rv;
}

enum gf_return_val ec_decode_data(gf_callback *callback, int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, u32 *crc) {
    enum gf_return_val rv;

    rv = (*gf_funcs.ec_decode)(len, k, rows, data, new_data, crc);
    if (callback) {
        (*callback)(NULL, GF_SUCCESS);
        return GF_DEFERRED;
    }
    return rv;
}

u32 ec_crc(unsigned int num, const u8 *buf, u32 init_crc) {
	return (*gf_funcs.ec_crc)(num, buf, init_crc);
}

const char *nvmeibc_gf_optimization_to_string(int val)
{
	switch (val) {
	case NVMEIBC_GF_AVX2:   return "AVX2";
	case NVMEIBC_GF_SSE2:   return "SSE2";
	case NVMEIBC_GF_64_BIT: return "64_BIT";
    case NVMEIBC_GF_EC_CALC: return "KERNEL_EC_CALC";
    case NVMEIBC_GF_ARM_INTRINSICS: return "INTRINSICS_ARM";
	default:                return "UNOPTIMIZED";
	}
}

int nvmeibc_gf_optimization_from_string(const char *str)
{
	switch (str[0]) {
    case 'A': case 'a': return NVMEIBC_GF_AVX2;
    case 'S': case 's': return NVMEIBC_GF_SSE2;
    case '6': case '8': return NVMEIBC_GF_64_BIT;
    case 'U': case 'u': return NVMEIBC_GF_UNOPTIMIZED;
    case 'K': case 'k': return NVMEIBC_GF_EC_CALC;
    case 'I': case 'i': return NVMEIBC_GF_ARM_INTRINSICS;
    default:            return -3;
	}
}

#if defined(__KERNEL__) && defined(__x86_64__)	// User spaces preemption already saves registers, !x64 doe snot have those registers
unsigned long nvmeibc_fpu_flags[NR_CPUS];		// To reduce arr size can use: CONFIG_NR_CPUS, nr_cpu_ids
void *nvmeibc_fpu_regs[NR_CPUS] ____cacheline_aligned;
extern void nvmeib_save_avx256(   void  *area);		// In assembly code
extern void nvmeib_restore_avx256(void  *area);		// In assembly code

static unsigned int nvmeib_get_xsave_size(void) {
	unsigned int eax, ebx, ecx, edx;
	unsigned int xsave_size = 0;
	// Use cpuid with leaf 0x0D and subleaf 0
	__cpuid_count(0x0D, 0, eax, ebx, ecx, edx);
	// ebx contains the size of the xsave area in bytes
	xsave_size = ebx;
	return xsave_size;
}

void nvmeib_fpu_begin(void)
{
	unsigned long flags;
	unsigned long *pf;
	int cpu = smp_processor_id();

	local_irq_save(flags);
	pf = nvmeibc_fpu_flags + cpu;
	BUG_ON(*pf);
	*pf = flags;
	nvmeib_save_avx256(nvmeibc_fpu_regs[cpu]);
}

int nvmeib_fpu_end(void)
{
	int cpu = smp_processor_id();
	unsigned long *pf = nvmeibc_fpu_flags + cpu;
	unsigned long flags = *pf;

	BUG_ON(!flags);
	nvmeib_restore_avx256(nvmeibc_fpu_regs[cpu]);
	*pf = 0;
	local_irq_restore(flags);
	return 0;
}

int nvmeib_allocate_xsave_bufs(void)
{
	unsigned int xsave_len = nvmeib_get_xsave_size();
	int cpu;

	pr_info("Xsave buffer size = %d\n", xsave_len);
	for_each_possible_cpu(cpu) {
		nvmeibc_fpu_regs[cpu] = kzalloc_node(xsave_len, GFP_KERNEL, cpu_to_node(cpu));
		if (nvmeibc_fpu_regs[cpu] == NULL)
			return -ENOMEM;
	}
	return 0;
}

void nvmeib_free_xsave_bufs(void)
{
	int cpu;
	for_each_possible_cpu(cpu)
		kfree(nvmeibc_fpu_regs[cpu]);
}
#else
	void nvmeib_fpu_begin(void) {}
	int nvmeib_fpu_end(void) { return 0; }
	int nvmeib_allocate_xsave_bufs(void) { return 0; }
	void nvmeib_free_xsave_bufs(void) {}
#endif
