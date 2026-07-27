#include <stdlib.h>
#include "uni_scenario_gf.h"
#include "../datapath_ec/nvmeibc_block_dp_ec_gf.h"

#ifdef GF_PERF
int main(int argc, char *argv[]) {
    (void) argc;        // Unused
    (void) argv;        // Unused
    gf_perf();
}
#else

static int full_crc(int num, unsigned char *buffer) {
    return 0xffffffff ^ __calculate_crc32c(0xffffffff, buffer, num);
}
static int full_crc_64(int num, unsigned char *buffer) {
    return 0xffffffff ^ __calculate_crc32c_64(0xffffffff, buffer, num);
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
/*
   First test is to just do the simple tests originally written. It
   assumes 4 disks. Set the capability to zero (simplest test.
*/
TEST_FUNC int gf_simple(void) {
    int rv=0;
    unsigned char *data[4] = {0};
    unsigned char *new_data[2] = {0};
    u64 parity0, parity1;
    u64 pkeep0, pkeep1;
    u64 val[3] = { 0x018200e1, 0x018101e2, 0x018200e3 };
    unsigned char *d_array[3] = {(unsigned char *)&val[0], (unsigned char *)&val[1], (unsigned char *)&val[2] };
    unsigned char *p_array[2] = {(unsigned char *)&parity0, (unsigned char *)&parity1 };
    u64 valnew = val[1] ^ 0x100;
    u64 valfix, valfix1;
	u32 new_crc[5] = {0};
	u32 ref_crc[5] = {0};
	const u32 val_crc = full_crc(8, (unsigned char *)(&valnew));          // New Value

	BUG_ON(test_crc());
	__gf_choose_functions(NVMEIBC_GF_UNOPTIMIZED);               // Choose min possible

    /* Reference CRCs. */
    ref_crc[0] = full_crc(8, (unsigned char *)(&val[0]));       // D0
    ref_crc[1] = full_crc(8, (unsigned char *)(&val[1]));       // D1

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
	BUG_ON(parity0 != pkeep0);
	BUG_ON(new_crc[0] != ref_crc[0]);
	BUG_ON(new_crc[1] != ref_crc[1]);
	BUG_ON(new_crc[2] != ref_crc[2]);

    /* 2+1 update: {val[0],val[1]} -> {val[0],valnew}*/
    data[0] = (unsigned char *)&val[1];
    data[1] = (unsigned char *)&valnew;
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
	BUG_ON((val[0] ^ val[1]) != parity0);
    ec_encode_data_update(NULL, 8, 1, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
	BUG_ON((val[0] ^ valnew) != parity0);
	BUG_ON(new_crc[0] != val_crc);	// crc(valnew)
	BUG_ON(new_crc[1] != (u32)full_crc(8, p_array[0]));

    data[0] = (unsigned char *)&valnew;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    new_crc[1] = 0xffffffff;
    ec_encode_data_update(NULL, 8, 1, 1, data, p_array, new_crc, NULL);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    ref_crc[2] = full_crc( 8, p_array[0]);       // Parity
	BUG_ON(__calculate_parity_P(2, val) != parity0);
	BUG_ON(new_crc[0] != ref_crc[1]);
	BUG_ON(new_crc[1] != ref_crc[2]);

    /* 2+1 read decode data[0] */
    parity0 = __calculate_parity_P(2, val);
    new_crc[2] = full_crc(8, (unsigned char *)(&val));
    data[0] = 0;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_crc[0] = 0xffffffff;
    ec_decode_data(NULL, 8, 1, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[0] != valfix ||
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

    if (val[1] != valfix ||
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
    if (parity0 != pkeep0 ||
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
    if ((val[0] ^ valnew ^ val[2]) != parity0 ||
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
    if (__calculate_parity_P(3, val) != parity0 ||
        new_crc[0] != ref_crc[1] ||
        new_crc[1] != ref_crc[3]) {
        rv = 1;
        BUG_ON(1);
    }

    /* 3+1 read decode data[0] */
    parity0 = __calculate_parity_P(3, val);
    data[0] = 0;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&val[2];
    data[3] = (unsigned char *)&parity0;
    new_crc[0] = 0xffffffff;
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 1, 3, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[0] != valfix ||
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
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 1, 3, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;

    if (val[1] != valfix ||
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
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 1, 3, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[2] != valfix ||
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
    if (parity0 != pkeep0) {
        rv = 1;
        BUG_ON(1);
    }
    if (parity1 != pkeep1) {
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
    if ((val[0] ^ valnew) != parity0 ||
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
    if (__calculate_parity_P(2, val) != parity0) {
        rv = 1;
        BUG_ON(1);
    }
    if (__calculate_parity_Q(2, val) != parity1) {
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
    parity0 = __calculate_parity_P(2, val);
    parity1 = __calculate_parity_Q(2, val); // Put the candle back
    data[0] = 0;
    data[1] = (unsigned char *)&val[1];
    data[2] = (unsigned char *)&parity0;
    data[3] = (unsigned char *)&parity1;
    new_crc[0] = 0xffffffff;
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[0] != valfix ||
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
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1] != valfix ||
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
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1] != valfix ||
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
    valfix = 0;
    new_data[0] = (unsigned char *)&valfix;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1] != valfix ||
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
    valfix = 0;
    valfix1 = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_data[1] = (unsigned char *)&valfix1;
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    new_crc[1] ^= 0xffffffff;
    if (val[0] != valfix ||
        val[1] != valfix1 ||
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
    valfix = 0;
    valfix1 = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_data[1] = (unsigned char *)(-1);    // Don't follow this pointer!
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[0] != valfix ||
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
    valfix = 0;
    valfix1 = 0;
    new_data[0] = (unsigned char *)&valfix1;
    new_data[1] = (unsigned char *)(-1);
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1] != valfix1 ||
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
    valfix = 0;
    valfix1 = 0;
    new_data[0] = (unsigned char *)&valfix;
    new_data[1] = (unsigned char *)(-1);
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[0] != valfix ||
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
    valfix = 0;
    valfix1 = 0;
    new_data[0] = (unsigned char *)&valfix1;
    new_data[1] = (unsigned char *)(-1);
    ec_decode_data(NULL, 8, 2, 2, data, new_data, new_crc);
    new_crc[0] ^= 0xffffffff;
    if (val[1] != valfix1 ||
        new_crc[0] != ref_crc[1]) {
        rv = 1;
        BUG_ON(1);
    }

    return rv;
}

#endif  // GF_PERF

static inline long int __getrandom(void) {
    static int initialized = 0;
    static char state[32];

    if (!initialized) {
        initstate((int)get_cycles(), state, sizeof(state));
        initialized = 1;
    }

    return random();
}

#ifndef GF_PERF

static inline int __compare_parity(unsigned int bs, unsigned int k, unsigned int d, unsigned char **data_bufs, unsigned char **parity_bufs) {
    int rv = 0;
    unsigned char *db;
    u64 val[d];
    u64 pkeep[k];
    unsigned int i, j;

    for (j = 0; j < bs; j+=sizeof(u64)) {
        for (i = 0; i < d; i++) {
            db = data_bufs[i];
            val[i] = *(u64*)(&db[j]);
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
static inline int __gfe(unsigned int d, unsigned int k) {
	const unsigned int bs = 4096;
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
static inline int __gfopt(unsigned int d, unsigned int k) {
	const unsigned int bs = 4096;
    int rv = 0;
    unsigned char __attribute__((aligned(4096))) data_bufs[d][bs];
    unsigned char __attribute__((aligned(4096))) parity_bufs[k][bs];
    unsigned char __attribute__((aligned(4096))) decode_bufs[k][bs];
    unsigned char *data[d];
    unsigned char *parity[k];
    unsigned char *decode[k];
    int      *bp;
    unsigned int  i, j;
    unsigned int  capability;

    /* Find out what this processor supports. */
    __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);              // Choose max possible
    capability = __gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT); // Get it

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
        __gf_choose_functions(NVMEIBC_GF_UNOPTIMIZED); // "Known good" set.
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        /* Now test against everything that we're cabable of. */
        for (i = 1; i <= capability; i++) {
            __gf_choose_functions(i);
            memset(decode_bufs, 0, sizeof(decode_bufs));
            ec_encode_data(NULL, bs, k, d, data, decode, NULL, NULL);
            rv |= __compare_buffers(bs, parity[0], decode[0]);
        }
    } else {    // k == 2
        BUG_ON(k != 2);

        /* P and Q */
        memset(parity_bufs, 0, sizeof(parity_bufs));
        memset(decode_bufs, 0, sizeof(decode_bufs));
        __gf_choose_functions(NVMEIBC_GF_UNOPTIMIZED); // "Known good" set.
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        for (i = 1; i <= capability; i++) {
            __gf_choose_functions(i);
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
        __gf_choose_functions(NVMEIBC_GF_UNOPTIMIZED); // "Known good" set.
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        for (i = 1; i <= capability; i++) {
            __gf_choose_functions(i);
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
        __gf_choose_functions(NVMEIBC_GF_UNOPTIMIZED); // "Known good" set.
        memset(decode_bufs, 0, sizeof(decode_bufs));
        ec_encode_data(NULL, bs, k, d, data, parity, NULL, NULL);
        for (i = 1; i <= capability; i++) {
            __gf_choose_functions(i);
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
#define _for_each_d_and_p(d_drives, parities) \
	for (d_drives = 2; d_drives <= GF_MAX_D; d_drives++) \
	for (parities = 1; parities <= GF_MAX_P; parities++)

TEST_FUNC int gf_extensive(void) {
    int rv=0;
    unsigned int parities, d_drives, capability, cap;

        /* Find out what this processor supports. */
    __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);              // Choose max possible
    capability = __gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT); // Get it

    for (cap = 0; cap <= capability; cap++) {
        __gf_choose_functions(cap);
		_for_each_d_and_p(d_drives, parities) {
			rv |= __gfe(d_drives, parities);
		}
    }

    return rv;
}


/*
   Check for the same answer between optimizations.
*/
TEST_FUNC int gf_opt_check(void) {
    int rv = 0;
    unsigned int parities, d_drives;
	_for_each_d_and_p(d_drives, parities) {
		rv |= __gfopt(d_drives, parities);
	}
    return rv;
}

#endif      // !GF_PERF

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


TEST_FUNC int gf_perf(void) {
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
    unsigned int capability;

    /* Find out what this processor supports. */
    __gf_choose_functions(NVMEIBC_GF_AUTO_INIT);              // Choose max possible
    capability = __gf_choose_functions(NVMEIBC_GF_DISPLAY_CURRENT); // Get it

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
            __gf_choose_functions(j);
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
            __gf_choose_functions(j);
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
            __gf_choose_functions(j);
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
            __gf_choose_functions(j);
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
            __gf_choose_functions(j);
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
                __gf_choose_functions(j);
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
            __gf_choose_functions(j);
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
                __gf_choose_functions(j);
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
				__gf_choose_functions(j);
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
				__gf_choose_functions(j);
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
	__gf_choose_functions(NVMEIBC_GF_UNOPTIMIZED);
	crc0 = ec_crc(~0, data[0], bs);
	for (j = 0; j <= capability; j++) {
		__gf_choose_functions(j);
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

