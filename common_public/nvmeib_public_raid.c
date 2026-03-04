#include "nvmeib_public_raid.h"
#include "nvmeib_utils.h"
#include "nvmeibp_trace.h"
#include <linux/raid/xor.h>
#include <linux/raid/pq.h>
#include "block/datapath_ec/nvmeibc_block_dp_ec_gf.h"

static void xor_blocks_into(unsigned int count, unsigned int len, void *dest, void **srcs)
{
	memcpy(dest, srcs[0], len);
	++srcs;
	--count;
	while (count > 0) {
		unsigned int xc = count;
		if (xc > MAX_XOR_BLOCKS)
			xc = MAX_XOR_BLOCKS;
		xor_blocks(xc, len, dest, srcs);
		srcs += xc;
		count -= xc;
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

#define RET_ON_GF_ERROR_AND_WARN(_cond) \
	do { \
		if (unlikely((_cond))) { \
			WARN_ON_ONCE(1); \
			return GF_ERROR; \
		} \
	} while(0);

enum gf_return_val nvmeib_raid_encode(int len, int k, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {

	void *ptrs[16];
	int i;

	RET_ON_GF_ERROR_AND_WARN(irqs_disabled());
	RET_ON_GF_ERROR_AND_WARN(in_interrupt());

	BUG_ON(!raid6_call.gen_syndrome);
    /* Simple sanity checks. Some can go away later. */
    WARN((len == 0)||(k == 0)||(k > 2), "nvmeibc bug: len=%d, k=%d", len, k); // For now, at least.
	BUG_ON(rows+k > 16);
	preempt_disable();

	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			memcpy(data_copy[i], data[i], len);
			ptrs[i] = data_copy[i];
		}
		else
			ptrs[i] = data[i];
		if (crc)
			crc[i] = crc32c(crc[i], ptrs[i], len);
	}
	for (i = 0; i < k; i++)
		ptrs[rows+i] = coding[i];

	if (k == 1 || (k == 2 && coding[1] == NULL))
		xor_blocks_into(rows, len, coding[0], ptrs);
	else if (k == 2) {
		if (coding[0] == NULL) {
			if ((ptrs[rows] = kmalloc(len, GFP_ATOMIC)) == NULL)
				BUG();
		}
		raid6_call.gen_syndrome(rows+k, len, ptrs);
		if (coding[0] == NULL)
			kfree(ptrs[rows]);
	}
	else
		BUG_ON(k != 0);
	for (i = 0; i < k; i++) {
		if (crc && coding[i])
			crc[rows+i] = crc32c(crc[rows+i], coding[i], len);
	}

	preempt_enable();

	return GF_SUCCESS;

}
EXPORT_SYMBOL(nvmeib_raid_encode);

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
enum gf_return_val nvmeib_raid_update(int len, int k, int vec_i, unsigned char **data, unsigned char **coding, u32 *crc, unsigned char *data_copy)
{
	void *ptrs[16] = {0};

	RET_ON_GF_ERROR_AND_WARN(irqs_disabled());
	RET_ON_GF_ERROR_AND_WARN(in_interrupt());

	BUG_ON(!raid6_call.xor_syndrome);
	WARN((len == 0)||(k == 0)||(k > 2), "nvmeibc bug: len=%d, k=%d", len, k); // For now, at least.
	BUG_ON(vec_i+k >= 16);
	preempt_disable();

	if (data_copy) {
		memcpy(data_copy, data[1], len);
		data[1] = data_copy;
	}
	if (coding[0] != data[2])
		memcpy(coding[0], data[2], len);
	if (k == 1) {
		xor_blocks(2, len, coding[0], (void **)data);
	}
	else if (k == 2) {
		if (coding[1] != data[3])
			memcpy(coding[1], data[3], len);
		xor_blocks(1, len, data[0], (void **)&data[1]);
		ptrs[vec_i] = data[0];
		ptrs[vec_i+1] = coding[0];
		ptrs[vec_i+2] = coding[1];
		raid6_call.xor_syndrome(vec_i+k+1, vec_i, vec_i, len, ptrs);
	}
	else
		BUG();
    if (crc) {
        crc[0] = crc32c(crc[0], data[  1], len);     // new data
        crc[1] = crc32c(crc[1], coding[0], len);     // P
        if (k == 2) {
            crc[2] = crc32c(crc[2], coding[1], len);     // Maybe Q
        }
    }

	preempt_enable();

	return GF_SUCCESS;

}
EXPORT_SYMBOL(nvmeib_raid_update);


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
enum gf_return_val nvmeib_raid_decode(int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, u32 *crc)
{
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

	RET_ON_GF_ERROR_AND_WARN(irqs_disabled());
	RET_ON_GF_ERROR_AND_WARN(in_interrupt());
    BUG_ON(k > 2); // Not for now
	preempt_disable();

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
		void *ptrs[16];
		int i;
		for (i = 0; i < rows; i++)
			ptrs[i] = data[i];
		ptrs[d[0]] = data[rows];
		xor_blocks_into(rows, len, new_data[0], ptrs);
		if (crc)
			*crc = crc32c(*crc, new_data[0], len);
    }

    /* Everything after this requires at least 2 parity. */
    else if (k < 2) {
        BUG_ON(k < 2);
    }

    /* Case 5: One D and parity missing. Fix it with Q.*/

    else if (d_index == 1 && p_index == 1 && data[rows] == 0) {
		data[d[0]] = new_data[0];
		if ((data[rows] = kmalloc(len, GFP_ATOMIC)) == NULL)
			BUG();
        raid6_datap_recov(rows+k, len, d[0], (void **)data);
		kfree(data[rows]);
		data[rows] = 0;
		if (crc)
			*crc = crc32c(*crc, new_data[0], len);
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
		data[d[0]] = new_data[0];
        if (d[1] == d_notme){
            new_data[1] = NULL;
			if ((data[d_notme] = kmalloc(len, GFP_ATOMIC)) == NULL)
				BUG();
		}
		else
			data[d[1]] = new_data[1];

		if (d[1] < d[0]) {
			int tmp = d[0];
			d[0] = d[1];
			d[1] = tmp;
		}
        raid6_2data_recov(rows+k, len, d[0], d[1], (void **)data);
		if (crc) {
			crc[0] = crc32c(crc[0], new_data[0], len);
			if (new_data[1])
				crc[1] = crc32c(crc[1], new_data[1], len);
		}
		if (new_data[1] == NULL)
			kfree(data[d_notme]);
    } else {
		BUG();      // For now, anyway. k > 2.
    }

	preempt_enable();

	return GF_SUCCESS;

}
EXPORT_SYMBOL(nvmeib_raid_decode);


bool nvmeib_raid_kernel_builtin_supported(void) {
	/* some kernels don't have all algos */
	return !!raid6_call.xor_syndrome && !!raid6_call.gen_syndrome;
}
EXPORT_SYMBOL(nvmeib_raid_kernel_builtin_supported);

#ifdef __x86_64__
u32 nvmeib_raid_ec_crc(u32 init_crc, const u8 *buf, unsigned int len) {
#elif KS_CRC32C_USES_SIZE_T
u32 nvmeib_raid_ec_crc(u32 init_crc, const void *buf, size_t len) {
#else
u32 nvmeib_raid_ec_crc(u32 init_crc, const void *buf, unsigned int len) {
#endif
    return crc32c(init_crc, (const u8 *)(buf), len);
}
EXPORT_SYMBOL(nvmeib_raid_ec_crc);
