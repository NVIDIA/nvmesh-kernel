/*
 * nvmeibt_uuid.c
 *
 *  Created on: Aug 26, 2020
 *      Author: yair
 */

#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#if defined __has_include
#	if __has_include (<sys/random.h>)
#		include <sys/random.h>
#		define FOUND_SYS_RANDOM_H
#	endif
#endif
#include "../common/nvmeib_shared.h"
#include "nvmeibt_uuid.h"

union nvmeib_uuid nvmeibt_dummy_uuid = {.ll[0]=0, .ll[1]=0xfff1fff2fff3fff4};
const union nvmeib_uuid nvmeib_uuid_null_val = {.ll = {0, 0}};

// string:    532d84c0-db9a-11ea-a483-d5b03158bd81
// bytes:     c0 84 2d 53 9a db ea 11  a4 83 d5 b0 31 58 bd 81
// LE Qwords: { 0x11eadb9a532d84c0ULL, 0x81bd5831b0d583a4ULL }
// TODO: include and use "nvmeib_uuid_be.h"
int nvmeibt_urn_uuid_str_to_union_uuid(union nvmeib_uuid *uuid, const char *urn_uuid_str)
{
	int i,j, k = 0;
	int	rv = 0;
	union _uuid_swapper *u = (union _uuid_swapper *)uuid;
	const char	*urn_uuid_ptr = urn_uuid_str;

	for (i=0; i<2; i++) {
		uint64_t v = 0;
		for (j=0; j<16; j++) {
			char c = *urn_uuid_ptr++;
			k++;

			if (c == '-') {
				if (k != (8 + 1) && k != (13 + 1) && k != (18 + 1) && k != (23 + 1)) {
#ifdef N_Ef
				N_Ef(vxrwh81, "Illegal uuid=@STR k=@INT c=@CHAR", urn_uuid_str, k, c);
#endif	// #ifdef N_Ef
					rv = -1;
				}
				c = *urn_uuid_ptr++;
				k++;
			}
			if (c>='0' && c<='9')
				v = (v<<4) | (c - '0');
			else if (c >= 'a' || c <= 'f')
				v = (v << 4) | (c - 'a' + 10);
			else if (c >= 'A' || c <= 'F')
				v = (v << 4) | (c - 'A' + 10);
			else {
#ifdef N_Ef
				N_Ef(vxrwh82, "Illegal uuid=@STR k=@INT c=@CHAR", urn_uuid_str, k, c);
#endif	// #ifdef N_Ef
				rv = -1;
			}
		}
		u->ll[i] = v;
	}

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	u->ll[0] =  __builtin_bswap64(u->ll[0]);
	u->ll[1] =  __builtin_bswap64(u->ll[1]);
	u->lw[0] = __builtin_bswap32(u->lw[0]);
	u->sw[2] = __builtin_bswap16(u->sw[2]);
	u->sw[3] = __builtin_bswap16(u->sw[3]);
#endif
	if (rv == -1) {
		*uuid = nvmeib_uuid_null_val;
	}
	return rv;
}

int nvmeibt_urn_uuid_to_union_uuid(union nvmeib_uuid *uuid, const struct nvmeibt_urn_uuid *urn_uuid)
{
		return nvmeibt_urn_uuid_str_to_union_uuid(uuid, urn_uuid->str);
}

#define HEX_NIBBLE(v) ((v)<=9 ? (v)+'0' : (v)-10+'a')
static uint32_t rotr(uint32_t x, unsigned int n)
{
	// optimized to a single "ror" instruction
	const unsigned int  mask = (8*sizeof(x) - 1);
	n &= mask;
	return (x>>n) | (x<<( (-n)&mask ));
}

void nvmeibt_union_uuid_to_urn_uuid_in_place(const union nvmeib_uuid *uuid, char *str)
{
	union _uuid_swapper u = *((union _uuid_swapper *)uuid);
	int i;
	unsigned int j;
	char *p = str;
	unsigned short v;

#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
	u.lw[0] = __builtin_bswap32(u.lw[0]);
	u.sw[2] = __builtin_bswap16(u.sw[2]);
	u.sw[3] = __builtin_bswap16(u.sw[3]);
#endif

	for (i=0; i<16; i++) {
		v = u.bytes[i] >> 4;
		*p++ = HEX_NIBBLE(v);
		v = u.bytes[i] & 0xF;
		*p++ = HEX_NIBBLE(v);
		j = rotr(i-3, 1);
		if (j<4)		// i is one of 3, 5, 7, 9
			*p++ = '-';
	}
	*p=0;
}

struct nvmeibt_urn_uuid nvmeibt_union_uuid_to_urn_uuid(const union nvmeib_uuid *uuid)
{
	struct nvmeibt_urn_uuid result;

	if ((uuid->ll[0] == nvmeibt_dummy_uuid.ll[0]) && (uuid->ll[1] == nvmeibt_dummy_uuid.ll[1])) {
		result.str[0] = 0;
	}
	else {
		nvmeibt_union_uuid_to_urn_uuid_in_place(uuid, result.str);
	}
	return result;
}

void generate_random_uuid(union nvmeib_uuid *uuid)
{
	struct timespec ts;
	static unsigned int		noisy_tv_nsec = 0;
	int				rv;

#ifdef FOUND_SYS_RANDOM_H
	rv = getrandom(uuid, sizeof(*uuid), GRND_RANDOM | GRND_NONBLOCK);
#else
	rv = 1;
	errno = ENOPKG;
#endif
	if (rv) {
#ifdef N_Tf
		N_Tf(9sjcb4t, "getrandom() returned @AUTO_ERRNO. Using a less 'random' method");
#endif	// #ifdef N_Tf
		// Add noise to random
		getnstimeofday(&ts);
		if (!noisy_tv_nsec) {
			noisy_tv_nsec = (unsigned int)((unsigned long long)&generate_random_uuid) ^ getpid();
		}

		srand48((unsigned int)((unsigned long long)&ts) ^ ts.tv_nsec ^ lrand48());
		uuid->ll[0] = (lrand48() << 32) | lrand48();
		srand48(noisy_tv_nsec ^ lrand48());
		uuid->ll[1] = (lrand48() << 32) | lrand48();
		noisy_tv_nsec ^= ts.tv_nsec;
	}
}

