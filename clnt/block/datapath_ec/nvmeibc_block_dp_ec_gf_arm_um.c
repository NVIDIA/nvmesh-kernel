/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeibc_block_dp_ec_gf_arm_defs.h"
#ifdef NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
#ifdef __KERNEL__
#include "arm_neon.h"

#ifndef __clang__
#define __crc32cd(a,b) __builtin_aarch64_crc32cx(a,b)
#define __crc32cb(a,b) __builtin_aarch64_crc32cb(a,b)
#define __crc32ch(a,b) __builtin_aarch64_crc32ch(a,b)
#define __crc32cw(a,b) __builtin_aarch64_crc32cw(a,b)
#else
#define __crc32cd(a,b) __builtin_arm_crc32cd(a,b)
#define __crc32cb(a,b) __builtin_arm_crc32cb(a,b)
#define __crc32ch(a,b) __builtin_arm_crc32ch(a,b)
#define __crc32cw(a,b) __builtin_arm_crc32cw(a,b)
#endif

#define alloca(len) __builtin_alloca(len)
#else
#include <string.h>
#include <alloca.h>
#include <arm_neon.h>
#include <arm_acle.h>
#endif

#include "common/kr_incs.h"
#include "common/nvmeib_str.h"		/* unsafe_memcpy: fortify-bypass memcpy.
					 * Required because the GF entry functions
					 * carry target("general-regs-only"), and
					 * kernel fortify-string.h declares
					 * fortify_memcpy_chk as always_inline with
					 * default target attributes — GCC then
					 * refuses to inline it into a
					 * general-regs-only caller. */
#include "nvmeibc_block_dp_ec_gf_arm_neon.h"
#include "nvmeibc_block_dp_ec_gf_arm_um.h"

/*
 * Fallback in case the userspace build of this TU does not pull in
 * common/nvmeib_str.h (or whatever else might provide unsafe_memcpy).
 * In userspace there is no fortify wrapper to bypass, so plain memcpy
 * is fine; the justification arg is just dropped.
 */
#ifndef unsafe_memcpy
#define unsafe_memcpy(_dst, _src, _len, _just) memcpy(_dst, _src, _len)
#endif


#ifdef __KERNEL__
#include <linux/ptrace.h>
#include <linux/preempt.h>

/*
 * IMPORTANT: every function that calls nvmeibc_arm_save_regs() /
 * nvmeibc_arm_rstr_regs() MUST be marked with the
 * NVMEIBC_ARM_GF_ENTRY attribute (see below). That attribute compiles the
 * surrounding C body with -mgeneral-regs-only, which is what prevents the
 * compiler from emitting V-register instructions in the function's prologue
 * (e.g. picking V31 as a zero source for an `stp q31, q31, ...` memset of
 * a local array). Without that guarantee, the compiler is free to write to
 * V0..V31 *before* the save asm runs, and we'd silently save and then
 * restore the wrong (compiler-clobbered) value back into the user task's
 * FP state on the return-to-user path — i.e. the bug this fix is for.
 *
 * The save asm itself does not declare V0..V31 as inputs because doing so
 * does not help when the asm is inlined into a caller whose prologue has
 * already corrupted a V reg; only the caller-side `target` attribute can
 * stop that. The rstr asm does declare V0..V31 as clobbers so the compiler
 * can't keep a live value across the call.
 */
#define NVMEIBC_ARM_GF_ENTRY \
	__attribute__((target("general-regs-only")))

static void nvmeibc_arm_save_regs(struct user_fpsimd_state *save_buf)
{
	uint64_t tmp;
	__asm__ volatile(
		/* save Q0–Q31 in 16-byte lanes, pairs at 32-byte strides */
		"stp   q0,  q1,  [%[s], #16 * 0]   \n"
		"stp   q2,  q3,  [%[s], #16 * 2]   \n"
		"stp   q4,  q5,  [%[s], #16 * 4]   \n"
		"stp   q6,  q7,  [%[s], #16 * 6]   \n"
		"stp   q8,  q9,  [%[s], #16 * 8]   \n"
		"stp   q10, q11, [%[s], #16 * 10]  \n"
		"stp   q12, q13, [%[s], #16 * 12]  \n"
		"stp   q14, q15, [%[s], #16 * 14]  \n"
		"stp   q16, q17, [%[s], #16 * 16]  \n"
		"stp   q18, q19, [%[s], #16 * 18]  \n"
		"stp   q20, q21, [%[s], #16 * 20]  \n"
		"stp   q22, q23, [%[s], #16 * 22]  \n"
		"stp   q24, q25, [%[s], #16 * 24]  \n"
		"stp   q26, q27, [%[s], #16 * 26]  \n"
		"stp   q28, q29, [%[s], #16 * 28]  \n"
		"stp   q30, q31, [%[s], #16 * 30]! \n"
		/* now save FPSR/FPCR into the same area */
		"mrs   %x[t], fpsr               \n"
		"str   %w[t], [%[s], #16 * 2]   \n"
		"mrs   %x[t], fpcr               \n"
		"str   %w[t], [%[s], #16 * 2 + 4]\n"
		: [s] "+r" (save_buf)      /* %0 = save_buf pointer, updated by the final “!” */
		, [t] "=&r" (tmp)          /* %1 = temp register for FPSR/FPCR */
		:                          /* no read-only inputs */
		: "memory"                 /* clobber memory so the compiler won’t reorder */
	);
}

static void nvmeibc_arm_rstr_regs(struct user_fpsimd_state *save_buf)
{
	uint32_t tmp;
	uint32_t save_fpcr;
	__asm__ volatile(
		/* Q-registers 0–31 */
		"ldp   q0,  q1,  [%[s], #16*0]   \n"
		"ldp   q2,  q3,  [%[s], #16*2]   \n"
		"ldp   q4,  q5,  [%[s], #16*4]   \n"
		"ldp   q6,  q7,  [%[s], #16*6]   \n"
		"ldp   q8,  q9,  [%[s], #16*8]   \n"
		"ldp   q10, q11, [%[s], #16*10]  \n"
		"ldp   q12, q13, [%[s], #16*12]  \n"
		"ldp   q14, q15, [%[s], #16*14]  \n"
		"ldp   q16, q17, [%[s], #16*16]  \n"
		"ldp   q18, q19, [%[s], #16*18]  \n"
		"ldp   q20, q21, [%[s], #16*20]  \n"
		"ldp   q22, q23, [%[s], #16*22]  \n"
		"ldp   q24, q25, [%[s], #16*24]  \n"
		"ldp   q26, q27, [%[s], #16*26]  \n"
		"ldp   q28, q29, [%[s], #16*28]  \n"
		/* last pair + pre-index writeback to bump save_buf by #16*30 */
		"ldp   q30, q31, [%[s], #16*30]! \n"

		/* restore FPSR from save_buf+32 */
		"ldr   %w[tmp], [%[s], #16*2]    \n"  /* tmp = *(uint32_t*)(state+32) */
		"msr   fpsr, %x[tmp]             \n"

		/* grab new FPCR from state+36 into tmp, then do the conditional write */
		"ldr   %w[save_fpcr], [%[s], #16*2+4] \n"
		"mrs   %x[tmp], fpcr                  \n"  /* tmp = current FPCR */
		"cmp   %w[tmp], %w[save_fpcr]         \n"
		"b.eq  1f                             \n"
		"msr   fpcr, %x[save_fpcr]            \n"
		"1:\n"
		: [s] "+r" (save_buf),
			[tmp] "=&r" (tmp),
			[save_fpcr] "=&r" (save_fpcr)
			:
			: "memory", "cc",
			  "v0",  "v1",  "v2",  "v3",  "v4",  "v5",  "v6",  "v7",
			  "v8",  "v9",  "v10", "v11", "v12", "v13", "v14", "v15",
			  "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23",
			  "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31"
	);
}
#else
#define NVMEIBC_ARM_GF_ENTRY
#endif

static void xor_blocks_into(unsigned int count, unsigned int len, void *dest, void **srcs)
{
	unsigned int i;
	unsigned int offset;
	register uint64x2_t v0, v1, v2, v3;

	for (offset = 0; offset < len; offset += 4*sizeof(uint64x2_t)) {
		v0 = veorq_u64(((uint64x2_t *)(srcs[0]+offset))[0], ((uint64x2_t *)(srcs[1]+offset))[0]);
		v1 = veorq_u64(((uint64x2_t *)(srcs[0]+offset))[1], ((uint64x2_t *)(srcs[1]+offset))[1]);
		v2 = veorq_u64(((uint64x2_t *)(srcs[0]+offset))[2], ((uint64x2_t *)(srcs[1]+offset))[2]);
		v3 = veorq_u64(((uint64x2_t *)(srcs[0]+offset))[3], ((uint64x2_t *)(srcs[1]+offset))[3]);

		for (i = 2; i < count; i++) {
			v0 = veorq_u64(v0, ((uint64x2_t *)(srcs[i]+offset))[0]);
			v1 = veorq_u64(v1, ((uint64x2_t *)(srcs[i]+offset))[1]);
			v2 = veorq_u64(v2, ((uint64x2_t *)(srcs[i]+offset))[2]);
			v3 = veorq_u64(v3, ((uint64x2_t *)(srcs[i]+offset))[3]);
		}

		/* store */
#ifndef __clang__
		vst1q_u64(((uint64_t *)(dest+offset))+0, v0);
		vst1q_u64(((uint64_t *)(dest+offset))+2, v1);
		vst1q_u64(((uint64_t *)(dest+offset))+4, v2);
		vst1q_u64(((uint64_t *)(dest+offset))+6, v3);
#else
		vst1q_u64(((unsigned long *)(dest+offset))+0, v0);
		vst1q_u64(((unsigned long *)(dest+offset))+2, v1);
		vst1q_u64(((unsigned long *)(dest+offset))+4, v2);
		vst1q_u64(((unsigned long *)(dest+offset))+6, v3);
#endif
	}
}

__attribute__((unused)) static void xor_blocks(unsigned int count, unsigned int len, void *dest, void **srcs)
{
	unsigned int i;
	unsigned int offset;
	register uint64x2_t v0, v1, v2, v3;

	for (offset = 0; offset < len; offset += 4*sizeof(uint64x2_t)) {
		v0 = veorq_u64(((uint64x2_t *)(dest+offset))[0], ((uint64x2_t *)(srcs[0]+offset))[0]);
		v1 = veorq_u64(((uint64x2_t *)(dest+offset))[1], ((uint64x2_t *)(srcs[0]+offset))[1]);
		v2 = veorq_u64(((uint64x2_t *)(dest+offset))[2], ((uint64x2_t *)(srcs[0]+offset))[2]);
		v3 = veorq_u64(((uint64x2_t *)(dest+offset))[3], ((uint64x2_t *)(srcs[0]+offset))[3]);

		for (i = 1; i < count; i++) {
			v0 = veorq_u64(v0, ((uint64x2_t *)(srcs[i]+offset))[0]);
			v1 = veorq_u64(v1, ((uint64x2_t *)(srcs[i]+offset))[1]);
			v2 = veorq_u64(v2, ((uint64x2_t *)(srcs[i]+offset))[2]);
			v3 = veorq_u64(v3, ((uint64x2_t *)(srcs[i]+offset))[3]);
		}

		/* store */
#ifndef __clang__
		vst1q_u64(((uint64_t *)(dest+offset))+0, v0);
		vst1q_u64(((uint64_t *)(dest+offset))+2, v1);
		vst1q_u64(((uint64_t *)(dest+offset))+4, v2);
		vst1q_u64(((uint64_t *)(dest+offset))+6, v3);
#else
		vst1q_u64(((unsigned long *)(dest+offset))+0, v0);
		vst1q_u64(((unsigned long *)(dest+offset))+2, v1);
		vst1q_u64(((unsigned long *)(dest+offset))+4, v2);
		vst1q_u64(((unsigned long *)(dest+offset))+6, v3);
#endif
	}
}

#if KS_CRC32C_USES_SIZE_T
u32 crc32c(u32 init_crc, const void *buf, size_t len);
#else
u32 crc32c(u32 init_crc, const void *buf, unsigned int len);
#endif

static inline u32 __impl_crc32c(u32 init_crc, const void *buf, unsigned int len)
{
	while (len >= sizeof(uint64_t)) {
		init_crc = __crc32cd(init_crc, *(uint64_t *)buf);
		buf += sizeof(uint64_t);
		len -= sizeof(uint64_t);
	}
	if (len >= sizeof(uint32_t)) {
		init_crc = __crc32cw(init_crc, *(uint32_t *)buf);
		buf += sizeof(uint32_t);
		len -= sizeof(uint32_t);
	}
	if (len >= sizeof(uint16_t)) {
		init_crc = __crc32ch(init_crc, *(uint16_t *)buf);
		buf += sizeof(uint16_t);
		len -= sizeof(uint16_t);
	}
	if (len >= sizeof(uint8_t))
		init_crc = __crc32cb(init_crc, *(uint8_t *)buf);
	return init_crc;
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

NVMEIBC_ARM_GF_ENTRY
enum gf_return_val ec_encode_data_arm_optimized(int len, int k, int rows, unsigned char ** data, unsigned char ** coding, u32 *crc, unsigned char **data_copy) {

	void *ptrs[16];
	int i;
#ifdef __KERNEL__
	struct user_fpsimd_state save_buf;
#endif
	/* Simple sanity checks. Some can go away later. */
	if((len == 0) || (k <= 0) || (k > 2) || rows+k > 16)
		return GF_ERROR;
	if (((k == 1) && !coding[0]) || ((k == 2) && !coding[0] && !coding[1]))
		return GF_SUCCESS;	// Nothing to do.

#ifdef __KERNEL__
	/*
	 * Block task switches across the save/use/restore window so the kernel's
	 * lazy-FP context-switch machinery can't observe (and persist) the GF
	 * intermediate V-reg state as if it were the user task's FP state.
	 * In hardirq/softirq context this is a no-op; in process context it
	 * matters.
	 */
	preempt_disable();
	nvmeibc_arm_save_regs(&save_buf);
#endif
	for (i = 0; i < rows; i++) {
		if (data_copy && data_copy[i]) {
			unsafe_memcpy(data_copy[i], data[i], len,
				      "GF ARM encode: caller-validated buffers of len bytes");
			ptrs[i] = data_copy[i];
		}
		else
			ptrs[i] = data[i];
		if (crc)
			crc[i] = __impl_crc32c(crc[i], ptrs[i], len);
	}
	for (i = 0; i < k; i++)
		ptrs[rows+i] = coding[i];

	if (k == 1 || (k == 2 && coding[1] == NULL))
		xor_blocks_into(rows, len, coding[0], ptrs);
	else if (k == 2) {
		if (coding[0] == NULL)
			ptrs[rows] = alloca((len+63)&~63);
		raid6_neon4_gen_syndrome_real(rows+k, len, ptrs);
	}

	for (i = 0; i < k; i++) {
		if (crc && coding[i])
			crc[rows+i] = __impl_crc32c(crc[rows+i], coding[i], len);
	}
#ifdef __KERNEL__
	nvmeibc_arm_rstr_regs(&save_buf);
	preempt_enable();
#endif

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
NVMEIBC_ARM_GF_ENTRY
enum gf_return_val ec_encode_data_update_arm_optimized(int len, int k, int vec_i, unsigned char **data, unsigned char **coding, u32 *crc, unsigned char *data_copy)
{
	void *ptrs[16] = {0};
#ifdef __KERNEL__
	struct user_fpsimd_state save_buf;
#endif

    if((len == 0) || (k <= 0) || (k > 2) || vec_i+k > 16)
		return GF_ERROR;

#ifdef __KERNEL__
	preempt_disable();
	nvmeibc_arm_save_regs(&save_buf);
#endif
	if (data_copy) {
		unsafe_memcpy(data_copy, data[1], len,
			      "GF ARM update: caller-validated buffers of len bytes");
		data[1] = data_copy;
	}
	if (k == 1) {
		xor_blocks_into(3, len, coding[0], (void **)data);
	}
	else if (k == 2) {
		unsigned char *x = alloca((len+63)&~63);
		if (coding[0] != data[2])
			unsafe_memcpy(coding[0], data[2], len,
				      "GF ARM update: caller-validated buffers of len bytes");
		if (coding[1] != data[3])
			unsafe_memcpy(coding[1], data[3], len,
				      "GF ARM update: caller-validated buffers of len bytes");
		xor_blocks_into(2, len, x, (void **)data);
		ptrs[vec_i] = x;
		ptrs[vec_i+1] = coding[0];
		ptrs[vec_i+2] = coding[1];
		raid6_neon4_xor_syndrome_real(vec_i+k+1, vec_i, vec_i, len, ptrs);
	}

    if (crc) {
        crc[0] = __impl_crc32c(crc[0], data[  1], len);     // new data
        crc[1] = __impl_crc32c(crc[1], coding[0], len);     // P
        if (k == 2) {
            crc[2] = __impl_crc32c(crc[2], coding[1], len);     // Maybe Q
        }
    }

#ifdef __KERNEL__
	nvmeibc_arm_rstr_regs(&save_buf);
	preempt_enable();
#endif
	return GF_SUCCESS;
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
NVMEIBC_ARM_GF_ENTRY
enum gf_return_val ec_decode_data_arm_optimized(int len, int k, int rows, unsigned char ** data,  unsigned char ** new_data, u32 *crc)
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
#ifdef __KERNEL__
	struct user_fpsimd_state save_buf;
#endif
	enum gf_return_val rv = GF_ERROR;

	if (k <= 0 || k > 2)
		return GF_ERROR;

#ifdef __KERNEL__
	preempt_disable();
	nvmeibc_arm_save_regs(&save_buf);
#endif
    /* Get a list of all the missing Ds. Keep track of which Ds we want. */
    d_index = 0;
    d_notme = -1;
    for (i = 0; i < rows; i++) {
        if (data[i] == (void *)0 || data[i] == (void *)(-1)) {
			if (d_index > k)
				goto out;
            d[d_index] = i;
            d_index++;
        }
        if (data[i] == (void *)(-1)) {              // Don't want this D.
			if (d_notme != -1)		// Change this for k > 2.
				goto out;
            d_notme = i;
        }
    }
	if (d_index == 0)
		goto out;

    /* Get a list of all the bad Ps */
	if (new_data[0] == 0)
		goto out;
    p_index = 0;

    for (i = 0; i < k; i++) {
		if (data[rows+i] == (void *)(-1)) {       	// "reduced" parity?
			data[rows+i] = 0;
		}
		if (data[rows+i] == 0) {
			if (d_index + p_index > k)
				goto out;
            p_index++;
        }
    }

    /* Cases 3 and 4: one D missing, parity not missing. Fix it with parity. */
    if (d_index == 1 && d_index + p_index <= k && data[rows] != 0) {
		void *ptrs[16];
		int ii;
		for (ii = 0; ii < rows; ii++)
			ptrs[ii] = data[ii];
		ptrs[d[0]] = data[rows];
		xor_blocks_into(rows, len, new_data[0], ptrs);
		if (crc)
			*crc = __impl_crc32c(*crc, new_data[0], len);
    }
    /* Everything after this requires at least 2 parity. */
    else if (k < 2) {
		goto out;
    }

    /* Case 5: One D and parity missing. Fix it with Q.*/

    else if (d_index == 1 && p_index == 1 && data[rows] == 0) {
		data[d[0]] = new_data[0];
		data[rows] = alloca((len+63)&~63);
		raid6_datap_recov_neon(rows+k, len, d[0], (void **)data);
		data[d[0]] = 0;
		data[rows] = 0;
		if (crc)
			*crc = __impl_crc32c(*crc, new_data[0], len);
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
			data[d_notme] = alloca((len+63)&~63);
		}
		else
			data[d[1]] = new_data[1];

		if (d[1] < d[0]) {
			int tmp = d[0];
			d[0] = d[1];
			d[1] = tmp;
		}
		raid6_2data_recov_neon(rows+k, len, d[0], d[1], (void **)data);
		data[d[0]] = 0;
		data[d[1]] = 0;
		if (crc) {
			crc[0] = __impl_crc32c(crc[0], new_data[0], len);
			if (new_data[1])
				crc[1] = __impl_crc32c(crc[1], new_data[1], len);
		}
    }

	rv = GF_SUCCESS;
out:
#ifdef __KERNEL__
	nvmeibc_arm_rstr_regs(&save_buf);
	preempt_enable();
#endif
	return rv;

}

#ifdef __KERNEL__
	#if KS_CRC32C_USES_SIZE_T
u32 		ec_crc_arm_optimized(u32 init_crc, const void *buf, size_t len)
	#else
u32 		ec_crc_arm_optimized(u32 init_crc, const void *buf, unsigned int len)
	#endif
#else
unsigned int 	ec_crc_arm_optimized(unsigned int init_crc, const u8 *buf, unsigned int len)
#endif
{
	return __impl_crc32c(init_crc, (const uint8_t *)(buf), len);
}
#endif//NVMEIBC_GF_ARM_USER_SPACE_IMPLEMENTATION
