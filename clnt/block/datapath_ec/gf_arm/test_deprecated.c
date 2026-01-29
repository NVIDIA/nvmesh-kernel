/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include "common/compat/kr_incs_time.h"
#include "arm_um_ec.h"

#define unitest_print	printf
#define ec_crc	nvmeib_raid_ec_crc

#define GF_MAX_P        2           // Support up to EC 14+2
#define GF_MAX_D        14
#define GF_BUFFER_QUANT	4096

#define ec_encode_data(func, len, k, rows, data, coding, crc, data_copy)	nvmeib_raid_encode(len, k, rows, data, coding, crc, data_copy)
#define ec_encode_data_update(func, len, k, vec_i, data, coding, crc, data_copy)	nvmeib_raid_update(len, k, vec_i, data, coding, crc, data_copy)
#define ec_decode_data(func, len, k, rows, data, new_data, crc)	nvmeib_raid_decode(len, k, rows, data, new_data, crc)

#include "common/kr_incs.h"
/*
   CRC32-C calculation with no table lookup, little-endian only. No bit
   reversal because we're shifting right instead of left. Initial value is passed in crc_init.
*/
u32 __calculate_crc32c(u32 crc_init, const u8* buffer, u32 num) {
   unsigned int i, j;
   u32 byte, crc, mask;

   i = 0;
   crc = crc_init;                  // For RFC 3720, 0xffffffff
   for (i = 0; i < num; i++) {
      byte = buffer[i];             // Buffer byte
      crc = crc ^ byte;
      for (j = 0; j < 8; j++) {     // 8 bits per byte
         mask = -(crc & 1);         // LSb of crc
         crc = (crc >> 1) ^ (0x82F63B78 & mask); // BE:0x1EDC6F41 Reversed:0x82F63B78
      }
   }
   return crc;                      // For RFC 3720, XOR with 0xffffffff
}


/*
   Generate this table from the bit-wise implementation above. Index 0 in the
   table is crc of 0, etc.
*/
static u32 __attribute__((aligned(1024))) crc_table[] = {
    0x00000000,  0xf26b8303,  0xe13b70f7,  0x1350f3f4,
    0xc79a971f,  0x35f1141c,  0x26a1e7e8,  0xd4ca64eb,
    0x8ad958cf,  0x78b2dbcc,  0x6be22838,  0x9989ab3b,
    0x4d43cfd0,  0xbf284cd3,  0xac78bf27,  0x5e133c24,
    0x105ec76f,  0xe235446c,  0xf165b798,  0x030e349b,
    0xd7c45070,  0x25afd373,  0x36ff2087,  0xc494a384,
    0x9a879fa0,  0x68ec1ca3,  0x7bbcef57,  0x89d76c54,
    0x5d1d08bf,  0xaf768bbc,  0xbc267848,  0x4e4dfb4b,
    0x20bd8ede,  0xd2d60ddd,  0xc186fe29,  0x33ed7d2a,
    0xe72719c1,  0x154c9ac2,  0x061c6936,  0xf477ea35,
    0xaa64d611,  0x580f5512,  0x4b5fa6e6,  0xb93425e5,
    0x6dfe410e,  0x9f95c20d,  0x8cc531f9,  0x7eaeb2fa,
    0x30e349b1,  0xc288cab2,  0xd1d83946,  0x23b3ba45,
    0xf779deae,  0x05125dad,  0x1642ae59,  0xe4292d5a,
    0xba3a117e,  0x4851927d,  0x5b016189,  0xa96ae28a,
    0x7da08661,  0x8fcb0562,  0x9c9bf696,  0x6ef07595,
    0x417b1dbc,  0xb3109ebf,  0xa0406d4b,  0x522bee48,
    0x86e18aa3,  0x748a09a0,  0x67dafa54,  0x95b17957,
    0xcba24573,  0x39c9c670,  0x2a993584,  0xd8f2b687,
    0x0c38d26c,  0xfe53516f,  0xed03a29b,  0x1f682198,
    0x5125dad3,  0xa34e59d0,  0xb01eaa24,  0x42752927,
    0x96bf4dcc,  0x64d4cecf,  0x77843d3b,  0x85efbe38,
    0xdbfc821c,  0x2997011f,  0x3ac7f2eb,  0xc8ac71e8,
    0x1c661503,  0xee0d9600,  0xfd5d65f4,  0x0f36e6f7,
    0x61c69362,  0x93ad1061,  0x80fde395,  0x72966096,
    0xa65c047d,  0x5437877e,  0x4767748a,  0xb50cf789,
    0xeb1fcbad,  0x197448ae,  0x0a24bb5a,  0xf84f3859,
    0x2c855cb2,  0xdeeedfb1,  0xcdbe2c45,  0x3fd5af46,
    0x7198540d,  0x83f3d70e,  0x90a324fa,  0x62c8a7f9,
    0xb602c312,  0x44694011,  0x5739b3e5,  0xa55230e6,
    0xfb410cc2,  0x092a8fc1,  0x1a7a7c35,  0xe811ff36,
    0x3cdb9bdd,  0xceb018de,  0xdde0eb2a,  0x2f8b6829,
    0x82f63b78,  0x709db87b,  0x63cd4b8f,  0x91a6c88c,
    0x456cac67,  0xb7072f64,  0xa457dc90,  0x563c5f93,
    0x082f63b7,  0xfa44e0b4,  0xe9141340,  0x1b7f9043,
    0xcfb5f4a8,  0x3dde77ab,  0x2e8e845f,  0xdce5075c,
    0x92a8fc17,  0x60c37f14,  0x73938ce0,  0x81f80fe3,
    0x55326b08,  0xa759e80b,  0xb4091bff,  0x466298fc,
    0x1871a4d8,  0xea1a27db,  0xf94ad42f,  0x0b21572c,
    0xdfeb33c7,  0x2d80b0c4,  0x3ed04330,  0xccbbc033,
    0xa24bb5a6,  0x502036a5,  0x4370c551,  0xb11b4652,
    0x65d122b9,  0x97baa1ba,  0x84ea524e,  0x7681d14d,
    0x2892ed69,  0xdaf96e6a,  0xc9a99d9e,  0x3bc21e9d,
    0xef087a76,  0x1d63f975,  0x0e330a81,  0xfc588982,
    0xb21572c9,  0x407ef1ca,  0x532e023e,  0xa145813d,
    0x758fe5d6,  0x87e466d5,  0x94b49521,  0x66df1622,
    0x38cc2a06,  0xcaa7a905,  0xd9f75af1,  0x2b9cd9f2,
    0xff56bd19,  0x0d3d3e1a,  0x1e6dcdee,  0xec064eed,
    0xc38d26c4,  0x31e6a5c7,  0x22b65633,  0xd0ddd530,
    0x0417b1db,  0xf67c32d8,  0xe52cc12c,  0x1747422f,
    0x49547e0b,  0xbb3ffd08,  0xa86f0efc,  0x5a048dff,
    0x8ecee914,  0x7ca56a17,  0x6ff599e3,  0x9d9e1ae0,
    0xd3d3e1ab,  0x21b862a8,  0x32e8915c,  0xc083125f,
    0x144976b4,  0xe622f5b7,  0xf5720643,  0x07198540,
    0x590ab964,  0xab613a67,  0xb831c993,  0x4a5a4a90,
    0x9e902e7b,  0x6cfbad78,  0x7fab5e8c,  0x8dc0dd8f,
    0xe330a81a,  0x115b2b19,  0x020bd8ed,  0xf0605bee,
    0x24aa3f05,  0xd6c1bc06,  0xc5914ff2,  0x37faccf1,
    0x69e9f0d5,  0x9b8273d6,  0x88d28022,  0x7ab90321,
    0xae7367ca,  0x5c18e4c9,  0x4f48173d,  0xbd23943e,
    0xf36e6f75,  0x0105ec76,  0x12551f82,  0xe03e9c81,
    0x34f4f86a,  0xc69f7b69,  0xd5cf889d,  0x27a40b9e,
    0x79b737ba,  0x8bdcb4b9,  0x988c474d,  0x6ae7c44e,
    0xbe2da0a5,  0x4c4623a6,  0x5f16d052,  0xad7d5351
};

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

    num /= sizeof(u64);
    crc = crc_init;                  // For RFC 3720, 0xffffffff
    for (i = 0; i < num; i++) {
        data = ((u64 *)buffer)[i];
		crc = __crc32c_one_u64(data, crc);
    }

    return crc;                      // For RFC 3720, XOR with 0xffffffff
}

static int full_crc(int num, unsigned char *buffer) {
    return 0xffffffff ^ __calculate_crc32c(0xffffffff, (void *)buffer, num);
}
static int full_crc_64(int num, unsigned char *buffer) {
    return 0xffffffff ^ __calculate_crc32c_64(0xffffffff, (void *)buffer, num);
}
static int test_crc(void) {
    int rv=0;
    u32 crc;

    /* Test with the vectors from RFC 3270. Everything in the RFC was specified big-endian. */

    // 32 bytes of zeros
    static unsigned char vec1[] = {
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static u32 vec1_crc = 0x8a9136aa;   //        aa 36 91 8a

    // 32 bytes of ones
    static unsigned char vec2[] = {
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
        0xff, 0xff, 0xff, 0xff,
    };
    static u32 vec2_crc = 0X62a8ab43;     // 43 ab a8 62

    // 32 bytes of incrementing 00..1f:
    static unsigned char vec3[] = {
        0x00, 0x01, 0x02, 0x03,
        0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b,
        0x0c, 0x0d, 0x0e, 0x0f,
        0x10, 0x11, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f,
    };
    static u32 vec3_crc = 0x46dd794e;      // 4e 79 dd 46

    // 32 bytes of decrementing 1f..00:
    static unsigned char vec4[] = {
        0x1f, 0x1e, 0x1d, 0x1c,
        0x1b, 0x1a, 0x19, 0x18,
        0x17, 0x16, 0x15, 0x14,
        0x13, 0x12, 0x11, 0x10,
        0x0f, 0x0e, 0x0d, 0x0c,
        0x0b, 0x0a, 0x09, 0x08,
        0x07, 0x06, 0x05, 0x04,
        0x03, 0x02, 0x01, 0x00,
    };
    static u32 vec4_crc = 0x113fdb5c;      // 5c db 3f 11

    // An iSCSI - SCSI Read (10) Command PDU
    static unsigned char vec5[] = {
        0x01, 0xc0, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x14, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x14,
        0x00, 0x00, 0x00, 0x18,
        0x28, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    static u32 vec5_crc = 0xd9963a56;     // 56 3a 96 d9

    crc = full_crc(sizeof(vec1), vec1);
    if (crc != vec1_crc)
        rv = 1;
    crc = full_crc(sizeof(vec2), vec2);
    if (crc != vec2_crc)
        rv = 1;
    crc = full_crc(sizeof(vec3), vec3);
    if (crc != vec3_crc)
        rv = 1;
    crc = full_crc(sizeof(vec4), vec4);
    if (crc != vec4_crc)
        rv = 1;
    crc = full_crc(sizeof(vec5), vec5);
    if (crc != vec5_crc)
        rv = 1;

    crc = full_crc_64(sizeof(vec1), vec1);
    if (crc != vec1_crc)
        rv = 1;
    crc = full_crc_64(sizeof(vec2), vec2);
    if (crc != vec2_crc)
        rv = 1;
    crc = full_crc_64(sizeof(vec3), vec3);
    if (crc != vec3_crc)
        rv = 1;
    crc = full_crc_64(sizeof(vec4), vec4);
    if (crc != vec4_crc)
        rv = 1;
    crc = full_crc_64(sizeof(vec5), vec5);
    if (crc != vec5_crc)
        rv = 1;

    return rv;
}

u64 __calculate_parity_P(unsigned int num, const u64(* bufA)[8]) {
    unsigned int i;
    u64 p  = 0;
    for (i = 0; i < num; i++) {
        p ^= bufA[i][0];
    }
    return (p);
}

u64 __calculate_parity_Q(unsigned int num, const u64(* bufA)[8]) {
    int i, j;
    u64 *retp;
    unsigned char *a;
    unsigned char q;
    unsigned char p2[sizeof(u64)]; //Todo: __attribute__((aligned(8)));
    memset(p2, 0, sizeof(p2));

    for (j = num-1; j >= 0; j--) {
        a = (unsigned char *)&bufA[j][0];
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
   First test is to just do the simple tests originally written. It
   assumes 4 disks. Set the capability to zero (simplest test.
*/
int gf_simple(void) {
    int rv=0;
    unsigned char *d_array[3];
    unsigned char *p_array[2];
    unsigned char *data[4];
    unsigned char *new_data[2];
    u32 new_crc[5];
    u32 ref_crc[5];
    u32 val_crc;
    u64 parity0[8], parity1[8];
    u64 pkeep0, pkeep1;
    u64 val[3][8] = { {0x018200e1}, {0x018101e2}, {0x018200e3} };
    u64 valnew[8] = {val[1][0] ^ 0x100};
    u64 valfix[8], valfix1[8];

    if (test_crc()) {
        BUG_ON(1);
    }

    d_array[0] = (unsigned char *)&val[0];
    d_array[1] = (unsigned char *)&val[1];
    d_array[2] = (unsigned char *)&val[2];

    p_array[0] = (unsigned char *)&parity0;
    p_array[1] = (unsigned char *)&parity1;

    /* Reference CRCs. */
    ref_crc[0] = full_crc(8, (unsigned char *)(&val[0]));       // D0
    ref_crc[1] = full_crc(8, (unsigned char *)(&val[1]));       // D1
    val_crc = full_crc(8, (unsigned char *)(&valnew));          // New Value

    /* 2+1 encoding */
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    new_crc[2] = 0xffffffff;
    ec_encode_data(NULL, 8, 1, 2, d_array, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    new_crc[2] ^= 0xffffffff;
    ref_crc[2] = full_crc(8, p_array[0]);                   // Parity
    pkeep0 = __calculate_parity_P(2, val);
    if (parity0[0] != pkeep0 ||
        new_crc[0] != ref_crc[0] ||
        new_crc[1] != ref_crc[1] ||
        new_crc[2] != ref_crc[2]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+1 update */
    data[0] = (unsigned char *)&val[1];
    data[1] = (unsigned char *)&valnew;
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    new_crc[2] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 1, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    ref_crc[2] = full_crc(8, p_array[0]);                   // Parity
    if ((val[0][0] ^ valnew[0]) != parity0[0] ||
        new_crc[0] != val_crc ||
        new_crc[1] != ref_crc[2]) {
        rv = 1;
        BUG_ON(1);
    }
    data[0] = (unsigned char *)&valnew;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 1, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    ref_crc[2] = full_crc( 8, (unsigned char *)(&parity0));       // Parity

    if (__calculate_parity_P(2, val) != parity0[0] ||
        new_crc[0] != ref_crc[1] ||
        new_crc[1] != ref_crc[2]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+1 read decode data[0] */
    parity0[0] = __calculate_parity_P(2, val);
    new_crc[2] = full_crc(8, (unsigned char *)(&val));
    data[0] = 0;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_crc[0] = 0xffffffff;
    ec_decode_data(NULL, 8, 1, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[0][0] != valfix[0] ||
        new_crc[0] != ref_crc[0]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+1 read decode data[1] */
    data[0] = (unsigned char *)&val[0];
    data[1] = 0;
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    ec_decode_data(NULL, 8, 1, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[1][0] != valfix[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    ref_crc[2] = full_crc(8, (unsigned char *)(&val[2]));       // D2
    /* 3+1 encoding */
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    new_crc[2] = 0xffffffff;
    new_crc[3] = 0xffffffff;
    ec_encode_data(NULL, 8, 1, 3, d_array, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    new_crc[2] ^= 0xffffffff;
    new_crc[3] ^= 0xffffffff;
    ref_crc[3] = full_crc(8, p_array[0]);    // P
    pkeep0 = 0;
    pkeep0 = __calculate_parity_P(3, val);
    if (parity0[0] != pkeep0 ||
        new_crc[0] != ref_crc[0] ||
        new_crc[1] != ref_crc[1] ||
        new_crc[2] != ref_crc[2] ||
        new_crc[3] != ref_crc[3]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 3+1 update */
    data[0] = (unsigned char *)&val[1];
    data[1] = (unsigned char *)&valnew;
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 1, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    ref_crc[3] = full_crc(8,p_array[0]);    // P
    if ((val[0][0] ^ valnew[0] ^ val[2][0]) != parity0[0] ||
        new_crc[0] != val_crc ||
        new_crc[1] != ref_crc[3]) {
        rv = 1;
        BUG_ON(1);
    }
    data[0] = (unsigned char *)&valnew;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 1, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    ref_crc[3] = full_crc(8,p_array[0]);    // P
    if (__calculate_parity_P(3, val) != parity0[0] ||
        new_crc[0] != ref_crc[1] ||
        new_crc[1] != ref_crc[3]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 3+1 read decode data[0] */
    parity0[0] = __calculate_parity_P(3, val);
    data[0] = 0;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&val[2];
    data[3] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 1, 3, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[0][0] != valfix[0] ||
        new_crc[0] != ref_crc[0]) {
        rv = 1;
        BUG_ON(1);
    }


    /* 3+1 read decode data[1] */
    data[0] = (unsigned char *)&val[0];
    data[1] = 0;
    data[2] = (unsigned char *)&val[2];
    data[3] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 1, 3, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[1][0] != valfix[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }


    /* 3+1 read decode data[2] */
    data[0] = (unsigned char *)&val[0];
    data[1] = (unsigned char *)&val[1];
    data[2] = 0;
    data[3] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 1, 3, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[2][0] != valfix[0] ||
        new_crc[0] != ref_crc[2]) {
        rv = 1;
        BUG_ON(1);
    }


    /* 2+2 encoding */
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    new_crc[2] = 0xffffffff;
    new_crc[3] = 0xffffffff;
    ec_encode_data(NULL, 8, 2, 2, d_array, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    new_crc[2] ^= 0xffffffff;
    new_crc[3] ^= 0xffffffff;
    ref_crc[2] = full_crc(8,p_array[0]);    // P
    ref_crc[3] = full_crc(8,p_array[1]);    // P
    pkeep0 = 0;
    pkeep1 = 0;
    pkeep0 = __calculate_parity_P(2, val);
    pkeep1 = __calculate_parity_Q(2, val);
    if (parity0[0] != pkeep0) {
        rv = 1;
        BUG_ON(1);
    }
    if (parity1[0] != pkeep1) {
        rv = 1;
        BUG_ON(1);
    }
    if (new_crc[0] != ref_crc[0] ||
        new_crc[1] != ref_crc[1] ||
        new_crc[2] != ref_crc[2] ||
        new_crc[3] != ref_crc[3]) {
        BUG_ON(1);
    }

    /* 2+2 update */
    data[0] = (unsigned char *)&val[1];
    data[1] = (unsigned char *)&valnew;
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    new_crc[2] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 2, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    new_crc[2] ^= 0xffffffff;
    ref_crc[2] = full_crc(8,p_array[0]);    // P
    ref_crc[3] = full_crc(8,p_array[1]);    // Q
    if ((val[0][0] ^ valnew[0]) != parity0[0] ||
        new_crc[0] != val_crc ||
        new_crc[1] != ref_crc[2] ||
        new_crc[2] != ref_crc[3]) {
        rv = 1;
        BUG_ON(1);
    }
    data[0] = (unsigned char *)&valnew;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    new_crc[2] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 2, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    new_crc[2] ^= 0xffffffff;
    ref_crc[2] = full_crc(8,p_array[0]);    // P
    ref_crc[3] = full_crc(8,p_array[1]);    // Q
    if (__calculate_parity_P(2, val) != parity0[0]) {
        rv = 1;
        BUG_ON(1);
    }
    if (__calculate_parity_Q(2, val) != parity1[0]) {
        rv = 1;
        BUG_ON(1);
    }
    if (new_crc[0] != ref_crc[1] ||
        new_crc[1] != ref_crc[2] ||
        new_crc[2] != ref_crc[3]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 read decode data[0] */
    parity0[0] = __calculate_parity_P(2, val);
    parity1[0] = __calculate_parity_Q(2, val); // Put the candle back
    data[0] = 0;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[0][0] != valfix[0] ||
        new_crc[0] != ref_crc[0]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 read decode data[1] */
    data[0] = (unsigned char *)&val[0];
    data[1] = 0;
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1][0] != valfix[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[1] and q */
    data[0] = (unsigned char *)&val[0];
    data[1] = 0;
    data[2] = (unsigned char *)&parity0;
    data[3] = 0;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1][0] != valfix[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[1] and parity */
    data[0] = (unsigned char *)&val[0];
    data[1] = 0;
    data[2] = 0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1][0] != valfix[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[0] and data[1] */
    data[0] = 0;
    data[1] = 0;
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    valfix[0] = 0;
    valfix1[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_data[1] = (unsigned char *)&valfix1;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    if (val[0][0] != valfix[0] ||
        val[1][0] != valfix1[0] ||
        new_crc[0] != ref_crc[0] ||
        new_crc[1] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[0] reduced, data[1] missing, get data[0]. */
    data[0] = (void *)(0);
    data[1] = (void *)(-1);                 // Not me.
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    valfix1[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_data[1] = (unsigned char *)(-1);    // Don't follow this pointer!
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[0][0] != valfix[0] ||
        new_crc[0] != ref_crc[0]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[0] reduced, data[1] missing, get data[1]. */
    data[0] = (void *)(-1);
    data[1] = (void *)(0);
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    valfix1[0] = 0;
    new_data[0] = (unsigned char *)&valfix1;
    new_data[1] = (unsigned char *)(-1);
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1][0] != valfix1[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[1] reduced, data[0] missing, get data[0]. */
    data[0] = (void *)0;
    data[1] = (void *)(-1);
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    valfix1[0] = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_data[1] = (unsigned char *)(-1);
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[0][0] != valfix[0] ||
        new_crc[0] != ref_crc[0]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 2+2 data[1] reduced, data[0] missing, get data[1]. */
    data[0] = (void *)(-1);
    data[1] = (void *)(0);
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix[0] = 0;
    valfix1[0] = 0;
    new_data[0] = (unsigned char *)&valfix1;
    new_data[1] = (unsigned char *)(-1);
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1][0] != valfix1[0] ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    return rv;
}

static inline long int __getrandom(void) {
    static int initialized = 0;
    static char state[32];

    if (!initialized) {
        initstate((int)get_cycles(), state, sizeof(state));
        initialized = 1;
    }

    return random();
}

static inline int __compare_parity(unsigned int bs, unsigned int k, unsigned int d, unsigned char **data_bufs, unsigned char **parity_bufs) {
    int rv = 0;
    unsigned char *db;
    u64 val[d][8];
    u64 pkeep[k];
    unsigned int i, j;

    for (j = 0; j < bs; j+=sizeof(u64)) {
        for (i = 0; i < d; i++) {
            db = data_bufs[i];
            val[i][0] = *(u64*)(&db[j]);
        }

        if (k >= 2) {
            pkeep[1] = 0;
            pkeep[1] = __calculate_parity_Q(d, val);
        }
        if (k >= 1) {
            pkeep[0] = 0;
            pkeep[0] = __calculate_parity_P(d, val);
        }

        if (k >= 2) {
            if (*(u64*)(&parity_bufs[1][j]) != pkeep[1]) {
                rv = 1;
                BUG_ON(1);
            }
        }
        if (k >= 1) {
            if (*(u64*)(&parity_bufs[0][j]) != pkeep[0]) {
                rv = 1;
                BUG_ON(1);
            }
        }
    }

    return rv;
}


static inline int __compare_buffers(unsigned int bs, unsigned char *buf0, unsigned char *buf1) {
    const int cmp = memcmp(buf0, buf1, bs);
    const int rv = (cmp != 0);
    BUG_ON(rv);
    return rv;
}


#define SET_RV(msg)         \
do {                        \
	static char m[256] = {0}; \
	if (strcmp(m, msg)) {	\
		unitest_print(msg "\n"); \
		strcpy(m, msg);		\
	}						\
    rv = 1;                 \
} while (0)

/*
   Used for the gf extensive test to do the actual test. Gets called
   with params for the nested loops.
*/
static inline int __gfe(unsigned int bs, unsigned int d, unsigned int k) {
    static int rv = 0;
    unsigned char __attribute__((aligned(4096))) data_bufs[d][bs];
    unsigned char __attribute__((aligned(4096))) parity_bufs[k][bs];
    unsigned char __attribute__((aligned(4096))) decode_bufs[k][bs];
    unsigned char *data[d];
    unsigned char *parity[k];
    unsigned char *update[k+2];
    unsigned char *decode[k];
    unsigned char *new_data[d+k];
    int      	  *bp;
    unsigned int  i, j;
    u32 new_crc[GF_MAX_D+GF_MAX_P];
    u32 ref_crc[GF_MAX_D+GF_MAX_P];

    /* Initialize the data buffers with random cruft. */
    for (j = 0; j < d; j++) {
        for (i = 0; i < bs; i += sizeof(int)) {
            bp = (int *)(&data_bufs[j][i]);
            *bp = __getrandom();
        }
    }

    for (j = 0; j < d; j++) {
        data[j] = data_bufs[j];
        new_data[j] = data_bufs[j];
        ref_crc[j] = full_crc(bs, data_bufs[j]);
    }
    for (j = 0; j < k; j++) {
        parity[j] = parity_bufs[j];
        decode[j] = decode_bufs[j];
        new_data[j+d] = parity_bufs[j];
    }

    /* Encoding */
    memset(parity_bufs, 0, sizeof(parity_bufs));
    for (j = 0; j < d+k; j++) {
        new_crc[j] = 0xffffffff;
    }
    ec_encode_data(NULL, bs, k, d, data, parity, new_crc, NULL);
    for (j = d; j < d+k; j++) {
        ref_crc[j] = full_crc(bs, parity[j-d]);
    }
    for (j = 0; j < d + k; j++) {
        new_crc[j] ^= 0xffffffff;
        if (new_crc[j] != ref_crc[j]) {
            SET_RV("Encoding crc mismatch");
        }
    }

    if (__compare_parity(bs, k, d, data, parity)) {
        SET_RV("Encoding parity mismatch");
    }

    /* Update data[i] */
    for (i = 0; i < d; i++) {
        update[0] = data_bufs[i];
        update[1] = data_bufs[(i+1)%d];
        for (j = 0; j < k; j++) {
            update[j+2] = parity_bufs[j];
        }
        new_crc[0] = 0xffffffff;
        new_crc[1] = 0xffffffff;
        new_crc[2] = 0xffffffff;    // Only needed if k==2
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data_update(NULL, bs, k, i, update, decode, new_crc, NULL);
        new_crc[0] ^= 0xffffffff;
        new_crc[1] ^= 0xffffffff;
        new_crc[2] ^= 0xffffffff;    // Only needed if k==2
        for (j = 0; j < k; j++) {
            ref_crc[j+d] = full_crc(bs, decode[j]);
            if (new_crc[j+1] != ref_crc[j+d]) {
                SET_RV("Update parity crc mismatch");
            }
        }
        data[i] = data_bufs[(i + 1) % d];
        if (new_crc[0] != ref_crc[(i+1)%d]) {
            SET_RV("Update crc mismatch");
        }
        if (__compare_parity(bs, k, d, data, decode)) {
            SET_RV("Update parity mismatch");
        }

        update[0] = data_bufs[(i+1)%d];
        update[1] = data_bufs[i];
        for (j = 0; j < k; j++) {
            update[j+2] = decode[j];
        }
        new_crc[0] = 0xffffffff;
        new_crc[1] = 0xffffffff;
        new_crc[2] = 0xffffffff;    // Only needed if k==2
        /* No, I didn't leave out a memset. We need to reuse decode. */
        ec_encode_data_update(NULL, bs, k, i, update, decode, new_crc, NULL);
        new_crc[0] ^= 0xffffffff;
        new_crc[1] ^= 0xffffffff;
        new_crc[2] ^= 0xffffffff;    // Only needed if k==2
        for (j = 0; j < k; j++) {
            ref_crc[d+j] = full_crc(bs, decode[j]);
            if (new_crc[j+1] != ref_crc[d+j]) {
                SET_RV("Update 2 parity crc mismatch");
            }
        }
        data[i] = data_bufs[i];
        if (new_crc[0] != ref_crc[i]) {
            SET_RV("Update 2 crc mismatch");
        }
        if (__compare_parity(bs, k, d, data, decode)) {
            SET_RV("Update 2 parity mismatch");
        }
    }

    /* Read decode data[i] */
    for (i = 0; i < d; i++) {
        new_data[i] = 0;
        memset(decode_bufs, 0, sizeof(decode_bufs));
        new_crc[0] = 0xffffffff;
        ec_decode_data(NULL, bs, k, d, new_data, decode, new_crc);
        new_crc[0] ^= 0xffffffff;
        if (memcmp(decode[0], data_bufs[i], bs) != 0) {
            SET_RV("Decode compare failure");
        }
        if (new_crc[0] != ref_crc[i]) {
            SET_RV("Decode crc failure");
        }
        new_data[i] = data_bufs[i];
    }

    /* Read decode data[i] parity[k] if enough parity (at least 2) */
    if (k > 1) {
        for (i = 0; i < d; i++) {
            new_data[i] = 0;
            for (j = 0; j < k; j++) {
                new_data[d+j] = 0;
                memset(decode_bufs, 0, sizeof(decode_bufs));
                new_crc[0] = 0xffffffff;
                ec_decode_data(NULL, bs, k, d, new_data, decode, new_crc);
                new_crc[0] ^= 0xffffffff;
                if (memcmp(decode[0], data_bufs[i], bs) != 0) {
                    SET_RV("Decode 2:1 compare failure");
                }
                if (new_crc[0] != ref_crc[i]) {
                    SET_RV("Decode 2:1 crc failure");
                }
                new_data[d + j] = parity_bufs[j];
            }
            new_data[i] = data_bufs[i];
        }
    }

    /* Read decode data[i] data[j] if enough parity (at least 2) */
    if (k > 1) {
        for (i = 0; i < d; i++) {
            new_data[i] = 0;
            for (j = i+1; j < d; j++) {
                new_data[j] = 0;
                memset(decode_bufs, 0, sizeof(decode_bufs));
                new_crc[0] = 0xffffffff;
                new_crc[1] = 0xffffffff;
                ec_decode_data(NULL, bs, k, d, new_data, decode, new_crc);
                new_crc[0] ^= 0xffffffff;
                new_crc[1] ^= 0xffffffff;
                if (memcmp(decode[0], data_bufs[i], bs) != 0) {
                    SET_RV("Double decode compare error 0");
                }
                if (memcmp(decode[1], data_bufs[j], bs) != 0) {
                    SET_RV("Double decode compare error 1");
                }
                if (new_crc[0] != ref_crc[i] ||
                    new_crc[1] != ref_crc[j]) {
                    SET_RV("Double decode crc errors");
                }
                new_data[j] = data_bufs[j];
            }
            new_data[i] = data_bufs[i];
        }
    }

    /*
       Note that there are two buffers we don't have, which could be read
       errors, reduced, etc. We don't have two, and we want one of them. 0
       means that we don't have it and we want it; -1 means that we don't have
       it and we don't want it.
    */
    if (k > 1) {
        for (j = 0; j < d; j++) {
            new_data[j] = data_bufs[j];
        }
        for (j = 0; j < k; j++) {
            new_data[j+d] = parity_bufs[j];
        }

        /* Do every combination of data pairs, including their reverse. */
        for (i=0; i < d; i++) {
            for (j = 0; j < d; j++) {
                if (i == j) {       /* DIFFERENT i and j */
                    continue;
                }
                new_data[i] = (void *)(0);  /* The one we want. */
                new_data[j] = (void *)(-1); /* the one we dont. */
                memset(decode_bufs, 0, sizeof(decode_bufs));
                new_crc[0] = 0xffffffff;
                ec_decode_data(NULL, bs, k, d, new_data, decode, new_crc);
                new_crc[0] ^= 0xffffffff;
                if (memcmp(decode[0], data_bufs[i], bs) != 0) {
                    SET_RV("Reduced decode compare error");
                }
                if (new_crc[0] != ref_crc[i]) {
                    SET_RV("Reduced decode crc error");
                }
                new_data[i] = data_bufs[i];
                new_data[j] = data_bufs[j];
            }
        }

    }
    return rv;
}

/* Used to do inner loop of compatibility testing. */
static inline int __gfopt(unsigned int bs, unsigned int d, unsigned int k) {
    int rv = 0;
    unsigned char __attribute__((aligned(4096))) data_bufs[d][bs];
    unsigned char __attribute__((aligned(4096))) parity_bufs[k][bs];
    unsigned char __attribute__((aligned(4096))) decode_bufs[k][bs];
    unsigned char *data[d];
    unsigned char *parity[k];
    unsigned char *decode[k];
    int      *bp;
    unsigned int  i, j;
    unsigned int  capability = 0;

    /* Initialize the data buffers with random cruft. */
    for (j = 0; j < d; j++) {
        for (i = 0; i < bs; i += sizeof(int)) {
            bp = (int *)(&data_bufs[j][i]);
            *bp = __getrandom();
        }
    }

    for (j = 0; j < d; j++) {
        data[j] = data_bufs[j];
    }
    for (j = 0; j < k; j++) {
        parity[j] = parity_bufs[j];
        decode[j] = decode_bufs[j];
    }

    /*
       For now (famous last words), only test the optimizations of encode,
       for each of the interesting cases k==1 P, K==2 P,Q; P; Q
    */

    if (k == 1) {
        memset(parity_bufs, 0, sizeof(parity_bufs));
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        /* Now test against everything that we're cabable of. */
        for (i = 1; i <= capability; i++) {
            memset(decode_bufs, 0, sizeof(decode_bufs));
            ec_encode_data(NULL, bs, k, d, data, decode, NULL, NULL);
            rv |= __compare_buffers(bs, parity[0], decode[0]);
        }
    } else {    // k == 2
        BUG_ON(k != 2);

        /* P and Q */
        memset(parity_bufs, 0, sizeof(parity_bufs));
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        for (i = 1; i <= capability; i++) {
            memset(decode_bufs, 0, sizeof(decode_bufs));
            ec_encode_data(NULL, bs, k, d, data, decode, NULL, NULL);
            rv |= __compare_buffers(bs, parity_bufs[0], decode_bufs[0]);
            rv |= __compare_buffers(bs, parity_bufs[1], decode_bufs[1]);
        }

        /* P only */
        memset(parity_bufs, 0, sizeof(parity_bufs));
        memset(decode_bufs, 0, sizeof(decode_bufs));
        parity[1] = 0;
        decode[1] = 0;
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        for (i = 1; i <= capability; i++) {
            memset(decode_bufs, 0, sizeof(decode_bufs));
            ec_encode_data(NULL, bs, k, d, data, decode, NULL, NULL);
            rv |= __compare_buffers(bs, parity_bufs[0], decode_bufs[0]);
            parity[1] = parity_bufs[1];
            decode[1] = decode_bufs[1];
        }

        /* Q only */
        memset(parity_bufs, 0, sizeof(parity_bufs));
        memset(decode_bufs, 0, sizeof(decode_bufs));
        parity[0] = 0;
        decode[0] = 0;
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        for (i = 1; i <= capability; i++) {
            ec_encode_data(NULL, bs, k, d, data, decode, NULL, NULL);
            rv |= __compare_buffers(bs, parity_bufs[1], decode_bufs[1]);
            parity[0] = parity_bufs[0];
            decode[0] = decode_bufs[0];
        }
    }

    return rv;
}

/*
   More extensive GF tests. Different length buffers, different number of D & K
   (1 & 2). Buffer length must be a multiple of 512 bits (x86-64 zMM).
*/
int gf_extensive(void) {
    int rv=0;
    unsigned int parities, buf_size, d_drives, capability = 1, cap;

        /* Find out what this processor supports. */

    for (cap = 0; cap <= capability; cap++) {
		buf_size = GF_BUFFER_QUANT;

		for (d_drives = 2; d_drives <= GF_MAX_D; d_drives++) {
			for (parities = 1; parities <= GF_MAX_P; parities++) {
				rv |= __gfe(buf_size, d_drives, parities);
			}
		}
    }

    return rv;
}


/*
   Check for the same answer between optimizations.
*/
int gf_opt_check(void) {
    int rv = 0;
    unsigned int parities, d_drives;

    for (d_drives = 2; d_drives <= GF_MAX_D; d_drives++) {
        for (parities = 1; parities <= GF_MAX_P; parities++) {
            rv |= __gfopt(4096, d_drives, parities);
        }
    }

    return rv;
}

#ifdef __x86_64__
static inline void clflush(volatile void *p) {
	(void)p;
//    asm volatile ("clflush (%0)" :: "r"(p));
//    asm volatile ("mfence" : : : "memory");
}
#elif defined __aarch64__
static inline void clflush(volatile void *p) {
(void)p;
	//	asm volatile ("dc ivac, %0" :: "r"(p) : "memory");
}
#endif

static inline void flush_cache(int bs, int d, unsigned char ** data) {
    int i, j;
    #define CACHE_LINE 64

    for (i = 0; i < bs; i+=CACHE_LINE) {
        for (j = 0; j < d; j++) {
            if (data[j]) {
                clflush(&data[j][i]);
            }
        }
    }
}


/* Ticks/usec. Only calculate it once. */
static inline u64 timer_calibration(void) {
    u64 start, end;
    struct timeval t_start, t_end;
    static u64 calibration = 0;

    if (calibration) {
        return calibration;
    }

    gettimeofday(&t_start, NULL);
    start = get_cycles();
    sleep(1);
    end = get_cycles();
    gettimeofday(&t_end, NULL);
    timersub(&t_end, &t_start, &t_start);
    calibration = (end - start)/((t_start.tv_sec * 1000000)+t_start.tv_usec);
    return calibration;
}


static inline u64 get_timer(void) {
    return get_cycles();
}


static inline u64 t_to_nsec(unsigned long long t) {
    return t*1000/timer_calibration();
}


/*
   Measure the performance of several alternative implementations. Just how bad is this?
*/
#define PERF_ITER 1000


int gf_perf(void) {
    int rv = 0;
    const unsigned int d=8;
    const unsigned int k=2;         // Test 8+2
    const unsigned int bs=4096;
    unsigned char __attribute__((aligned(4096))) data_bufs[d+2][bs];
    unsigned char __attribute__((aligned(4096))) parity_bufs[k][bs];
    unsigned char __attribute__((aligned(4096))) decode_bufs[k][bs];
    unsigned char __attribute__((aligned(4096))) check_bufs[2*k][bs];
    unsigned char __attribute__((aligned(4096))) valnew[bs];
    unsigned char *data[d+2];
    unsigned char *parity[k];
    unsigned char *decode[k];
    unsigned char *update[k+2];
    u32 crc[d+k];
	u32 crc0;
	u32 crc1;
	u32 *pcrc;
    char *scrc;
    int      *bp;
    unsigned int  i, j, m;
    u64 t;
    u64 nsec;
    unsigned int capability = 1;

    /* Initialize the data buffers with random cruft. */
    for (j = 0; j < ARRAY_SIZE(data_bufs); j++) {
        for (i = 0; i < bs; i += sizeof(int)) {
            bp = (int *)(&data_bufs[j][i]);
            *bp = __getrandom();
        }
    }

    for (i = 0; i < bs; i += sizeof(int)) {
        bp = (int *)(&valnew);
        *bp = __getrandom();
    }

    for (j = 0; j < ARRAY_SIZE(data); j++) {
        data[j] = data_bufs[j];
    }
    for (j = 0; j < ARRAY_SIZE(parity); j++) {
        parity[j] = parity_bufs[j];
    }

    for (j = 0; j < ARRAY_SIZE(decode); j++) {
        decode[j] = decode_bufs[j];
    }

    unitest_print("             \t      Ref.   64bit-C                AVX2 \n");
    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf encode 8+1%s\t", scrc);
        for (j = 0; j <= capability; j++) {
            t = 0;
            for (i = 0; i < PERF_ITER; i++) {
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_encode_data(NULL, bs, 1, d, data, parity, pcrc, NULL);
                t += get_timer();
            }
            nsec = t_to_nsec(t);
            unitest_print("%10lld", nsec/PERF_ITER);
            if (j == 0)
                memcpy(check_bufs[0], parity[0], bs);
            else if (memcmp(check_bufs[0], parity[0], bs))
                unitest_print("Fail");
        }
        unitest_print("\n");
    }

    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf encode +2 P%s\t", scrc);
        parity[1] = NULL;
        for (j = 0; j <= capability; j++) {
            t = 0;
            for (i = 0; i < PERF_ITER; i++) {
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_encode_data(NULL, bs, 2, d, data, parity, pcrc, NULL);
                t += get_timer();
            }
            nsec = t_to_nsec(t);
            unitest_print("%10lld", nsec/PERF_ITER);
            if (memcmp(check_bufs[0], parity[0], bs))
                unitest_print("Fail");
        }
        unitest_print("\n");
    }

    parity[1] = parity_bufs[1];
    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf encode +2 Q%s\t", scrc);
        parity[0] = NULL;
        for (j = 0; j <= capability; j++) {
            t = 0;
            for (i = 0; i < PERF_ITER; i++) {
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_encode_data(NULL, bs, 2, d, data, parity, pcrc, NULL);
                t += get_timer();
            }
            nsec = t_to_nsec(t);
            unitest_print("%10lld", nsec/PERF_ITER);
            if (j == 0)
                memcpy(check_bufs[1], parity[1], bs);
            else if (memcmp(check_bufs[1], parity[1], bs))
                unitest_print("Fail");
        }
        parity[0] = parity_bufs[0];
        unitest_print("\n");
    }

    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf encode PQ %s\t", scrc);
        for (j = 0; j <= capability; j++) {
            t = 0;
            for (i = 0; i < PERF_ITER; i++) {
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_encode_data(NULL, bs, 2, d, data, parity, pcrc, NULL);
                t += get_timer();
            }
            nsec = t_to_nsec(t);
            unitest_print("%10lld", nsec/PERF_ITER);
            if (memcmp(check_bufs[0], parity[0], bs) ||
               memcmp(check_bufs[1], parity[1], bs))
                unitest_print("Fail");
        }
        unitest_print("\n");
    }

    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf update +1%s\t", scrc);
        for (j = 0; j <= capability; j++) {
            bool fail = 0;
            t = 0;
            for (i = 0; i < PERF_ITER; i++) {
                update[0] = data[2];
                update[1] = &valnew[0];
                        update[2] = parity[0];
                if (i == 0) {
                    if (j == 0) {
                        memcpy(check_bufs[0], parity[0], bs);
                    } else if (memcmp(check_bufs[0], parity[0], bs)) {
                        fail = true;
                    }
                }
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_encode_data_update(NULL, bs, 1, 2, update, parity, pcrc, NULL);
                t += get_timer();
                update[0] = &valnew[0];
                update[1] = data[2];
                update[2] = parity[0];
                update[3] = parity[1];
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_encode_data_update(NULL, bs, 1, 2, update, parity, pcrc, NULL);
                t += get_timer();
            }
            nsec = t_to_nsec(t);
            unitest_print("%10lld", nsec/PERF_ITER/2);
            if (j == 0) {
                memcpy(check_bufs[1], parity[0], bs);
            } else if (fail || memcmp(check_bufs[1], parity[0], bs)) {
                unitest_print("Fail");
            }
        }
        unitest_print("\n");
    }

    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf update +2%s\t", scrc);
        if (k > 1) {
            for (j = 0; j <= capability; j++) {
                bool fail = 0;
                t = 0;
                for (i = 0; i < PERF_ITER; i++) {
                    update[0] = data_bufs[2];
                    update[1] = &valnew[0];
                    update[2] = parity_bufs[0];
                    update[3] = parity_bufs[1];

                    if (i == 0) {
                        if (j == 0) {
                            memcpy(check_bufs[0], parity[0], bs);
                            memcpy(check_bufs[1], parity[1], bs);
                        } else if (memcmp(check_bufs[0], parity[0], bs) ||
                                   memcmp(check_bufs[1], parity[1], bs)) {
                            fail = true;
                        }
                    }
                    flush_cache(bs, d, data);
                    t -= get_timer();
                    ec_encode_data_update(NULL, bs, 2, 2, update, parity, pcrc, NULL);
                    t += get_timer();
                    update[0] = &valnew[0];
                    update[1] = data[2];
                    update[2] = parity[0];
                    update[3] = parity[1];
                    flush_cache(bs, d, data);
                    t -= get_timer();
                    ec_encode_data_update(NULL, bs, 2, 2, update, parity, pcrc, NULL);
                    t += get_timer();
                }
                nsec = t_to_nsec(t);
                unitest_print("%10lld", nsec/PERF_ITER/2);

                if (j == 0) {
                    memcpy(check_bufs[2], parity[0], bs);
                    memcpy(check_bufs[3], parity[1], bs);
                } else if (fail ||
                   memcmp(check_bufs[2], parity[0], bs) ||
                   memcmp(check_bufs[3], parity[1], bs))
                    unitest_print("Fail");
            }
        }
        unitest_print("\n");
    }

    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf decode 1%s\t", scrc);
        data[4] = 0;
        for (j=0; j <= capability; j++) {
            memset(decode[0], 0xaa, bs);
            t = 0;
            for (i = 0; i < PERF_ITER; i++) {
                flush_cache(bs, d, data);
                t -= get_timer();
                ec_decode_data(NULL, bs, 1, d, data, decode, pcrc);
                t += get_timer();
            }
            nsec = t_to_nsec(t);
            unitest_print("%10lld", nsec/PERF_ITER);
            if (j == 0)
                memcpy(check_bufs[0], decode[0], bs);
            else if (memcmp(check_bufs[0], decode[0], bs))
                unitest_print("Fail");
        }
		data[4] = data_bufs[4];
        unitest_print("\n");
    }

    for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
        unitest_print("gf decode 2P%s\t", scrc);
        if (k > 1) {
            data[d] = parity_bufs[0];
            data[d+1] = 0;
            data[4] = 0;
            for (j = 0; j <= capability; j++) {
                memset(decode[0], 0xbb, bs);
                memset(decode[1], 0xcc, bs);
                t = 0;
                for (i = 0; i < PERF_ITER; i++) {
                    flush_cache(bs, d, data);
                    t -= get_timer();
                    ec_decode_data(NULL, bs, 2, d, data, decode, pcrc);
                    t += get_timer();
                }
                nsec = t_to_nsec(t);
                unitest_print("%10lld", nsec/PERF_ITER);
                if (j == 0) {
                    memcpy(check_bufs[0], decode[0], bs);
                    memcpy(check_bufs[1], decode[1], bs);
                }
                if (memcmp(check_bufs[0], decode[0], bs) ||
                   memcmp(check_bufs[1], decode[1], bs))
                    unitest_print("Fail");
            }
        }
		data[4] = data_bufs[4];
        unitest_print("\n");
    }

	for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
		unitest_print("gf decode 2Q%s\t", scrc);
		if (k > 1) {
			data[d] = 0;
			data[d+1] = parity_bufs[1];
			data[4] = 0;
			for (j = 0; j <= capability; j++) {
				memset(decode[0], 0xbb, bs);
				memset(decode[1], 0xcc, bs);
				t = 0;
				for (i = 0; i < PERF_ITER; i++) {
					flush_cache(bs, d, data);
					t -= get_timer();
					ec_decode_data(NULL, bs, 2, d, data, decode, pcrc);
					t += get_timer();
				}
				nsec = t_to_nsec(t);
				unitest_print("%10lld", nsec/PERF_ITER);
				if (j == 0) {
					memcpy(check_bufs[0], decode[0], bs);
					memcpy(check_bufs[1], decode[1], bs);
				}
				if (memcmp(check_bufs[0], decode[0], bs) ||
				   memcmp(check_bufs[1], decode[1], bs))
					unitest_print("Fail");
			}
		}
		data[4] = data_bufs[4];
		unitest_print("\n");
	}

	for (m = 0, pcrc=0, scrc=" "; m < 2; m++, pcrc=crc, scrc="c") {
		unitest_print("gf decode PQ%s\t", scrc);
		if (k > 1) {
			data[d] = parity_bufs[0];
			data[d+1] = parity_bufs[1];
			data[4] = 0;
			data[5] = 0;
			for (j = 0; j <= capability; j++) {
				memset(decode[0], 0xbb, bs);
				memset(decode[1], 0xcc, bs);
				t = 0;
				for (i = 0; i < PERF_ITER; i++) {
					flush_cache(bs, d, data);
					t -= get_timer();
					ec_decode_data(NULL, bs, 2, d, data, decode, pcrc);
					t += get_timer();
				}
				nsec = t_to_nsec(t);
				unitest_print("%10lld", nsec/PERF_ITER);
				if (j == 0) {
					memcpy(check_bufs[0], decode[0], bs);
					memcpy(check_bufs[1], decode[1], bs);
				}
				if (memcmp(check_bufs[0], decode[0], bs) ||
				   memcmp(check_bufs[1], decode[1], bs))
					unitest_print("Fail");
			}
		}
		unitest_print("\n");
	}

	unitest_print("crc            \t");
	crc0 = ec_crc(~0, data[0], bs);
	for (j = 0; j <= capability; j++) {
		t = 0;
		for (i = 0; i < PERF_ITER; i++) {
			flush_cache(bs, d, data);
			t -= get_timer();
			crc1 = ec_crc(~0, data[0], bs);
			t += get_timer();
		}
		nsec = t_to_nsec(t);
		unitest_print("%10lld", nsec/PERF_ITER);
		if (crc0 != crc1) {
			unitest_print("Fail");
			unitest_print(" %08x %08x", crc0, crc1);
		}
	}
	unitest_print("\n");

	return rv;
}

int main(int argc, char *argv[]) {
    (void) argc;        // Unused
    (void) argv;        // Unused
	BUG_ON(gf_simple());
	BUG_ON(gf_extensive());
	//BUG_ON(gf_opt_check());
	BUG_ON(gf_perf());
}

