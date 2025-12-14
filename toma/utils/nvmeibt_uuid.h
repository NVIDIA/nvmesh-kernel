/*
 * nvmeibt_uuid.h
 *
 *  Created on: Aug 26, 2020
 *      Author: yair
 */

#ifndef TOMA_NVMEIBT_UUID_H_
#define TOMA_NVMEIBT_UUID_H_

// TODO: use "nvmeib_uuid_be.h" and consolidate implementation
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LE_SWAP64(x)	({											\
	_Static_assert(sizeof(x) == 8, "sizeof(x) != 8");				\
    __builtin_bswap64(x);											\
})
#define LE_SWAP32(x)	({											\
	{ _Static_assert(sizeof(x) == 4, "sizeof(x) != 4"); }			\
    __builtin_bswap32(x);											\
})
#define LE_SWAP16(x)	({											\
	{ _Static_assert(sizeof(x) == 2, "sizeof(x) != 2"); }			\
    __builtin_bswap16(x);											\
})
#define LE_SWAP8(x)	({												\
	{ _Static_assert(sizeof(x) == 1, "sizeof(x) != 1"); }			\
    (x);															\
})
#define LE_SWAP32_BITFIELD(x)	({									\
    __builtin_bswap32(x);											\
})
#else	// #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#define LE_SWAP64(x)	({											\
	{ _Static_assert(sizeof(x) == 8, "sizeof(x) != 8"); }			\
    (x);															\
})
#define LE_SWAP32(x)	({											\
	{ _Static_assert(sizeof(x) == 4, "sizeof(x) != 4"); }			\
    (x);															\
})
#define LE_SWAP8(x)	({												\
	{ _Static_assert(sizeof(x) == 1, "sizeof(x) != 1"); }			\
    (x);															\
})
#define LE_SWAP32_BITFIELD(x)	({									\
    (x);															\
})
#endif	// #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__

#define SWAP64_STR_FIELD(s,f)	s->f = LE_SWAP64(s->f)
#define SWAP32_STR_FIELD(s,f)	s->f = LE_SWAP32(s->f)
#define SWAP16_STR_FIELD(s,f)	s->f = LE_SWAP16(s->f)
#define SWAP8_STR_FIELD(s,f)	s->f = LE_SWAP8(s->f)
#define SWAP32_STR_BITFIELD(s,f)	s->f = LE_SWAP32_BITFIELD(s->f)

#define COPY_SWAP64_STR_FIELD(s,d,f)	d->f = LE_SWAP64(s->f)
#define COPY_SWAP32_STR_FIELD(s,d,f)	d->f = LE_SWAP32(s->f)
#define COPY_SWAP16_STR_FIELD(s,d,f)	d->f = LE_SWAP16(s->f)
#define COPY_SWAP8_STR_FIELD(s,d,f)		d->f = LE_SWAP8(s->f)
#define COPY_SWAP32_STR_BITFIELD(s,d,f)	d->f = LE_SWAP32_BITFIELD(s->f)

union _uuid_swapper {
	uint64_t ll[2];
	uint32_t lw[4];
	uint16_t sw[8];
	uint8_t bytes[16];
	union nvmeib_uuid	union_nvmeib_uuid;
};

static inline union nvmeib_uuid swap_uuid_LE_BE(const union nvmeib_uuid *src)
{
	const union _uuid_swapper 	*in = (union _uuid_swapper *)src;
	union _uuid_swapper 		out;

	out.ll[0] = in->ll[0];
	out.ll[1] = in->ll[1];
	out.lw[0] = LE_SWAP32(in->lw[0]);
	out.sw[2] = LE_SWAP16(in->sw[2]);
	out.sw[3] = LE_SWAP16(in->sw[3]);
	return out.union_nvmeib_uuid;
}

#define SWAP_UUID_STR_FIELD(s,f) { \
	union _uuid_swapper *s_u = (union _uuid_swapper *)&(s->f); \
	s_u->ll[0] = s_u->ll[0];	\
	s_u->ll[1] = s_u->ll[1];	\
	s_u->lw[0] = LE_SWAP32(s_u->lw[0]);	\
	s_u->sw[2] = LE_SWAP16(s_u->sw[2]);	\
	s_u->sw[3] = LE_SWAP16(s_u->sw[3]);	\
};

#define COPY_SWAP_UUID_STR_FIELD(s,d,f) { \
	union _uuid_swapper *s_u = (union _uuid_swapper *)&(s->f); \
	union _uuid_swapper *d_u = (union _uuid_swapper *)&(d->f); \
	d_u->ll[0] = s_u->ll[0];	\
	d_u->ll[1] = s_u->ll[1];	\
	d_u->lw[0] = LE_SWAP32(s_u->lw[0]);	\
	d_u->sw[2] = LE_SWAP16(s_u->sw[2]);	\
	d_u->sw[3] = LE_SWAP16(s_u->sw[3]);	\
};

extern const union nvmeib_uuid nvmeib_uuid_null_val;

/******************************* UUID utils ***********************************/
struct nvmeibt_urn_uuid {
	char str[URN_UUID_STR_LENGTH + 4];	// 36+4=40 characters (for alignment and match with client_protocol)
};

void generate_random_uuid(union nvmeib_uuid *urn_uuid);
void uuid_mgmt_format_to_urn_str(const char uuid[16], char *out);

int nvmeibt_urn_uuid_str_to_union_uuid(union nvmeib_uuid *uuid, const char *urn_uuid_str);
int nvmeibt_urn_uuid_to_union_uuid(union nvmeib_uuid *uuid, const struct nvmeibt_urn_uuid *id);
void nvmeibt_union_uuid_to_urn_uuid_in_place(const union nvmeib_uuid *uuid, char *str);
struct nvmeibt_urn_uuid nvmeibt_union_uuid_to_urn_uuid(const union nvmeib_uuid *uuid);

static inline union nvmeib_uuid *GET_UNION_UUID_OF_URN_UUID_STR(char *urn_uuid_str) {
	static union nvmeib_uuid	__uuid;
	nvmeibt_urn_uuid_str_to_union_uuid(&__uuid, urn_uuid_str);
	return &__uuid;
}

#endif /* TOMA_NVMEIBT_UUID_H_ */
