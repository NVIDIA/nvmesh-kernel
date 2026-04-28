/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_UUID_BE_H
#define NVMEIB_UUID_BE_H

#include <linux/types.h>
#include <linux/uuid.h>
#ifdef __KERNEL__
	#include "kr_version.h"
#else
	#include "../common/compat/kr_incs_types.h"
	#if __has_include(<uuid/uuid.h>)
		#include <uuid/uuid.h>
	#elif __has_include("spdk/uuid.h")
		#include "spdk/uuid.h"
		typedef unsigned char uuid_t[16];
		static inline void uuid_generate(uuid_t out) { spdk_uuid_generate((struct spdk_uuid *)out); }
		#define uuid_generate_random(out) uuid_generate(out)
		static inline int uuid_parse(const char *p, uuid_t out) { return spdk_uuid_parse((struct spdk_uuid *)out, p); }
		static inline void uuid_unparse(const uuid_t uu, char *p) { spd_iiod_fmt_lower(p, 37, (const struct spdk_uuid *)uu); }
		#define uuid_unparse_lower  uuid_unparse
	#else
		// Consider installing libuuid-devel, reimplementation below is done for unitests only, it might not work correctly for littli-big endian.
		typedef unsigned char uuid_t[16];
		#include "../common/nvmeib_str.h"
		static inline bool  __must_check uuid_is_valid(const char *uuid) {
			unsigned int i;
			for (i = 0; i < 36; i++) {
				if (i == 8 || i == 13 || i == 18 || i == 23) {
					if (uuid[i] != '-')
						return false;
				} else if (!isxdigit_imp(uuid[i])) {
					return false;
				}
			}
			return true;
		}
		static inline void uuid_generate(uuid_t out) {					// Todo: Consolidate with Toma: generate_random_uuid()
			unsigned int *rv = (unsigned int*)out;
			rv[0] = rand(); rv[1] = rand(); rv[1] = rand(); rv[1] = rand();
		}
		#define uuid_generate_random(out) uuid_generate(out)

		static inline int uuid_parse(const char *p, uuid_t out) {		// Todo: Consolidate with Toma: nvmeibt_urn_uuid_str_to_union_uuid()
			int i, j;
			for (i = 0; i < 2; i++) {
				u64 v = 0;
				for (j = 0; j < 16; j++) {
					const char c = *p++;
					v = (v << 4) | hex_to_bin((c != '-') ? c : *p++ );
				}
				((u64 *)out)[i] = __builtin_bswap64(v);	// Little endian
			}
			return 0;
		}
		static inline void uuid_unparse(const uuid_t uu, char *p) {	// Todo: Consolidate with Toma: nvmeibt_union_uuid_to_urn_uuid()
			#define __4BIT_TO_LETTER(v) (v<=9 ? (v+'0') : (v-10+'a'))
			int i;
			for (i = 0; i < 16; i++) {
				const unsigned char v0 = (uu[i] >> 4), v1 = (uu[i] & 0xF);
				*p++ = __4BIT_TO_LETTER(v0);
				*p++ = __4BIT_TO_LETTER(v1);
				if (i == 3 || i == 5 || i == 7 || i == 9)
					*p++ = '-';
			}
			*p=0;	// 37 bytes: 32 hex nibbles + 4 '-' + '\0'
		}
		#define uuid_unparse_lower  uuid_unparse
	#endif	// Reimplementation of uuid.h
#endif

/********************************* guid_t ************************************/
#ifndef GUID_INIT
	// Re-implementation of linux/uuid.h for old version, where it was missing this definition
	typedef struct {
		__u8 b[16];
	} guid_t;

	#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)			\
	((guid_t)								\
	{{ (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
	(b) & 0xff, ((b) >> 8) & 0xff,					\
	(c) & 0xff, ((c) >> 8) & 0xff,					\
	(d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})

	/* backwards compatibility, don't use in new code */
	#ifndef UUID_LE
		typedef guid_t uuid_le;
		#define UUID_LE(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)		\
			GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)
		#define NULL_UUID_LE							\
			UUID_LE(0x00000000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)
	#endif//UUID_LE
#endif //GUID_INIT

/********************************** efi **************************************/
#ifndef __KERNEL__
	// linux/efi.h
	typedef guid_t efi_guid_t;
	#define NULL_GUID {.b = {[0 ... 15] = 0}} // Daniel: Can define as NULL_UUID_BE
	static inline int efi_guidcmp(efi_guid_t left, efi_guid_t right) { return memcmp(&left, &right, sizeof (efi_guid_t)); }
#endif

/*****************************************************************************/
// The typedef and the UUID_BE constructor macro have separate guards:
//   * _NVMEIB_UUID_BE_TYPE_DEFINED guards just the type (shared with
//     utils/nvmeib_jdr/nvmeib_jdr.h, which may define the same struct first).
//   * UUID_BE guards the constructor macro (some legacy kernel/other headers
//     may define it together with the type).
#ifndef _NVMEIB_UUID_BE_TYPE_DEFINED
#define _NVMEIB_UUID_BE_TYPE_DEFINED
	typedef struct{
		unsigned char b[16];
	} uuid_be;
#endif

#ifndef UUID_BE
	#define UUID_BE(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7)		\
	((uuid_be)								\
	{{ ((a) >> 24) & 0xff, ((a) >> 16) & 0xff, ((a) >> 8) & 0xff, (a) & 0xff, \
	((b) >> 8) & 0xff, (b) & 0xff,					\
	((c) >> 8) & 0xff, (c) & 0xff,					\
	(d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})

	#define NULL_UUID_BE							\
		UUID_BE(0x00000000, 0x0000, 0x0000, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00)

#endif//UUID_BE

static inline int nvmeib_uuid_cmp(const uuid_be u1, const uuid_be u2) { return memcmp(&u1, &u2, sizeof(u1));}

#ifndef __KERNEL__
	static inline void nvmeib_public_uuid_gen(uuid_be *bu) { uuid_generate(bu->b); }
#endif

/*****************************************************************************/
#define URN_UUID_STR_LENGTH 36		// == kernel UUID_STRING_LEN. The length of a UUID string ("aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee") not including trailing NULL.

union nvmeib_uuid {
	unsigned long long int 	ll[2];
	unsigned char			bytes[16];
	unsigned int			ints[4];
};
#define NVMEIB_UUID_NULL_VAL				((union nvmeib_uuid){.ll = {0, 0}})
#define ARE_UUID_EQ(_u1, _u2)				({const union nvmeib_uuid *u1 = (_u1), *u2 = (_u2); ((u1 == u2) || (u1 && u2 && ((u1)->ll[0] == (u2)->ll[0]) && ((u1)->ll[1] == (u2)->ll[1])));})
#define UUID_TO_64_HASH_KEY(__uuid)			({const union nvmeib_uuid *u = (__uuid); (u->ll[0] ^ u->ll[1]);})

static inline unsigned int nvmeib_uuid_first_4_bytes(const union nvmeib_uuid *uuid)
{
	return (uuid ? uuid->ints[0] : (unsigned int)-1);
}
#define uuid_4bytes(uuid) ({						\
	const union nvmeib_uuid	*__uuid__ = &uuid;			\
	(__uuid__);										\
})

/*****************************************************************************/
#define UUID_ZERO_STRING	"00000000-0000-0000-0000-000000000000"

#endif//NVMEIB_UUID_BE_H
