#ifndef NVMEIB_VERSION_H
#define NVMEIB_VERSION_H

#include "common/kr_incs.h"
#ifdef __KERNEL__
	#include "kr_version.h"
	#include "nvmeib_utils.h"
	#include "ib_incs.h"
	#include "nvmeib_str.h"
#endif
#include "nvmeib_math.h"
#include "nvmeib_types.h"
#include "nvmeib_utils_shared.h"
#include "nvmeib_pp_lib.h"
#include "nvmeib_msgs_shared.h"
#define VEX_BUILD_BUG_ON_MSG_STATIC(cond, msg)	BUILD_BUG_ON_MSG(cond, msg)
#define VEX_BUILD_BUG_ON_MSG(cond, msg)			BUILD_BUG_ON_MSG(cond, msg)

#pragma push_macro("__FILE_LITERAL__")
#undef __FILE_LITERAL__
#define __FILE_LITERAL__ nvmeib_version_shared_h

#ifdef TRACE_INCLUDE_FILE
	/* TRACE_INCLUDE_FILE should be defined in Makefile of eac*h one of the modules that includes this file */
	#include TRACE_INCLUDE_FILE
#else
	#error "Tracing not not supported, fix compilation or use kr_incs_dummy_empty_traces.h"
#endif

#define VEX_DEBUG

#ifdef VEX_DEBUG
#	ifndef LLVM
#		pragma GCC push_options
#		pragma GCC optimize "-Og"
#	endif
#endif

extern const struct vex_ops nvmeib_vex_invalid_ops;

/* Foritfy misbehave for some reason on some gcc versions */
#ifdef _LINUX_FORTIFY_STRING_H_
	#define vex_memcpy __builtin_memcpy
#else
	#define vex_memcpy memcpy
#endif

#define NVMEIB_PROTOCOL_VERSION	0x0001

union version {
	struct {
		u8 mr;
		u8 subminor;
		u8 minor;
		u8 major;
	};

	u32 all;
};

union nvmeib_version {
	struct {
		union version product;
		union version protocol;
	};

	u64 all;
};

#define NVMEIB_INVALID_VERSION_INIT {.all = ~(u64)0 }

static const union nvmeib_version nvmeib_invalid_version = NVMEIB_INVALID_VERSION_INIT;

#define NVMEIB_13_PRODUCT_VERSION {.major = 1, .minor = 3}
#define NVMEIB_13_PROTOCOL_VERSION {.major = 1, .minor = 0}
#define NVMEIB_13_VERSION_INIT {.product = NVMEIB_13_PRODUCT_VERSION, .protocol = NVMEIB_13_PROTOCOL_VERSION}

static const union version nvmeib_13_product_version = NVMEIB_13_PRODUCT_VERSION;
static const union version nvmeib_13_protocol_version = NVMEIB_13_PROTOCOL_VERSION;
static const union nvmeib_version nvmeib_13_version = NVMEIB_13_VERSION_INIT;

#define NVMEIB_20_PRODUCT_VERSION {.major = 2, .minor = 0}
#define NVMEIB_20_PROTOCOL_VERSION {.major = 1,.minor = 1}
#define NVMEIB_20_VERSION_INIT {.product = NVMEIB_20_PRODUCT_VERSION, .protocol = NVMEIB_20_PROTOCOL_VERSION}

static const union version nvmeib_20_product_version = NVMEIB_20_PRODUCT_VERSION;
static const union version nvmeib_20_protocol_version = NVMEIB_20_PROTOCOL_VERSION;
static const union nvmeib_version nvmeib_20_version = NVMEIB_20_VERSION_INIT;

/* version 2.1 - changes:
   IOCH-DRAINED: New  volume_server_cmd_ioch_drained_req and
				 Ext. volume_client_config_alloc_net_req */
#define NVMEIB_2p1_PRODUCT_VERSION 	{.major = 2,.minor = 1}
#define NVMEIB_2p1_PROTOCOL_VERSION {.major = 1,.minor = 2}
#define NVMEIB_2p1_VERSION_INIT 	{.product 	= NVMEIB_2p1_PRODUCT_VERSION, \
									 .protocol 	= NVMEIB_2p1_PROTOCOL_VERSION}
static const union version 			nvmeib_2p1_product_version 	= NVMEIB_2p1_PRODUCT_VERSION;
static const union version 			nvmeib_2p1_protocol_version 	= NVMEIB_2p1_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_2p1_version 			= NVMEIB_2p1_VERSION_INIT;

/* version 2.2 - changes:
   nvmeibs_login_response  - tgt_num_cpus, tgt_max_nrchs_per_path */
#define NVMEIB_2p2_PRODUCT_VERSION 	{.major = 2,.minor = 2}
#define NVMEIB_2p2_PROTOCOL_VERSION {.major = 1,.minor = 3}
#define NVMEIB_2p2_VERSION_INIT 	{.product 	= NVMEIB_2p2_PRODUCT_VERSION, \
									 .protocol 	= NVMEIB_2p2_PROTOCOL_VERSION}
static const union version 			nvmeib_2p2_product_version = NVMEIB_2p2_PRODUCT_VERSION;
static const union version 			nvmeib_2p2_protocol_version = NVMEIB_2p2_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_2p2_version = NVMEIB_2p2_VERSION_INIT;

/* version 2.3 - changes:
   EC-multislice (exchanging binje) */
#define NVMEIB_2p3_PRODUCT_VERSION 	{.major = 2,.minor = 3}
#define NVMEIB_2p3_PROTOCOL_VERSION {.major = 1,.minor = 4}
#define NVMEIB_2p3_VERSION_INIT 	{.product 	= NVMEIB_2p3_PRODUCT_VERSION, \
									 .protocol 	= NVMEIB_2p3_PROTOCOL_VERSION}
static const union version 			nvmeib_2p3_product_version = NVMEIB_2p3_PRODUCT_VERSION;
static const union version 			nvmeib_2p3_protocol_version = NVMEIB_2p3_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_2p3_version = NVMEIB_2p3_VERSION_INIT;

/* version 2.4 - changes:
   Server logout msg */
#define NVMEIB_2p4_PRODUCT_VERSION 	{.major = 2,.minor = 4}
#define NVMEIB_2p4_PROTOCOL_VERSION {.major = 1,.minor = 5}
#define NVMEIB_2p4_VERSION_INIT 	{.product 	= NVMEIB_2p4_PRODUCT_VERSION, \
									 .protocol 	= NVMEIB_2p4_PROTOCOL_VERSION}
static const union version 			nvmeib_2p4_product_version = NVMEIB_2p4_PRODUCT_VERSION;
static const union version 			nvmeib_2p4_protocol_version = NVMEIB_2p4_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_2p4_version = NVMEIB_2p4_VERSION_INIT;

/* version 2.5 - changes:
 * Add ext2 to PortInfo with node_guid
 * Add ext1 to Access Map Disk Info reply with node_guids of resources */
 #define NVMEIB_2p5_PRODUCT_VERSION 	{.major = 2,.minor = 5}
 #define NVMEIB_2p5_PROTOCOL_VERSION {.major = 1,.minor = 6}
 #define NVMEIB_2p5_VERSION_INIT 	{.product 	= NVMEIB_2p5_PRODUCT_VERSION, \
 .protocol 	= NVMEIB_2p5_PROTOCOL_VERSION}
 static const union version 			nvmeib_2p5_product_version = NVMEIB_2p5_PRODUCT_VERSION;
 static const union version 			nvmeib_2p5_protocol_version = NVMEIB_2p5_PROTOCOL_VERSION;
 static const union nvmeib_version 	nvmeib_2p5_version = NVMEIB_2p5_VERSION_INIT;

 #define NVMEIB_2p6_PRODUCT_VERSION 	{.major = 2,.minor = 6}
 #define NVMEIB_2p6_PROTOCOL_VERSION {.major = 1,.minor = 7}
 #define NVMEIB_2p6_VERSION_INIT 	{.product 	= NVMEIB_2p6_PRODUCT_VERSION, \
 .protocol 	= NVMEIB_2p6_PROTOCOL_VERSION}
 static const union version 			nvmeib_2p6_product_version = NVMEIB_2p6_PRODUCT_VERSION;
 static const union version 			nvmeib_2p6_protocol_version = NVMEIB_2p6_PROTOCOL_VERSION;
 static const union nvmeib_version 	nvmeib_2p6_version = NVMEIB_2p6_VERSION_INIT;

#define NVMEIB_2p7_PRODUCT_VERSION 	{.major = 2,.minor = 7}
#define NVMEIB_2p7_PROTOCOL_VERSION 	{.major = 1,.minor = 8}
#define NVMEIB_2p7_VERSION_INIT 	{.product 	= NVMEIB_2p7_PRODUCT_VERSION, \
					.protocol 	= NVMEIB_2p7_PROTOCOL_VERSION}
static const union version 			nvmeib_2p7_product_version = NVMEIB_2p7_PRODUCT_VERSION;
static const union version 			nvmeib_2p7_protocol_version = NVMEIB_2p7_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_2p7_version = NVMEIB_2p7_VERSION_INIT;

 #define NVMEIB_2p8_PRODUCT_VERSION 	{.major = 2,.minor = 8}
 #define NVMEIB_2p8_PROTOCOL_VERSION 	{.major = 1,.minor = 8}
 #define NVMEIB_2p8_VERSION_INIT 	{.product 	= NVMEIB_2p8_PRODUCT_VERSION, \
					 .protocol 	= NVMEIB_2p8_PROTOCOL_VERSION}
static const union version 		nvmeib_2p8_product_version = NVMEIB_2p8_PRODUCT_VERSION;
static const union version 		nvmeib_2p8_protocol_version = NVMEIB_2p8_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_2p8_version = NVMEIB_2p8_VERSION_INIT;

#define NVMEIB_3p0_PRODUCT_VERSION 	{.major = 3,.minor = 0}
#define NVMEIB_3p0_PROTOCOL_VERSION 	{.major = 1,.minor = 9}
#define NVMEIB_3p0_VERSION_INIT 	{.product 	= NVMEIB_3p0_PRODUCT_VERSION, \
					.protocol 	= NVMEIB_3p0_PROTOCOL_VERSION}

static const union version 		nvmeib_3p0_product_version = NVMEIB_3p0_PRODUCT_VERSION;
static const union version 		nvmeib_3p0_protocol_version = NVMEIB_3p0_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_3p0_version = NVMEIB_3p0_VERSION_INIT;

#define NVMEIB_3p1_PRODUCT_VERSION 	{.major = 3,.minor = 1}
#define NVMEIB_3p1_PROTOCOL_VERSION 	{.major = 1,.minor = 10}
#define NVMEIB_3p1_VERSION_INIT 	{.product 	= NVMEIB_3p1_PRODUCT_VERSION, \
.protocol 	= NVMEIB_3p1_PROTOCOL_VERSION}

static const union version 		nvmeib_3p1_product_version = NVMEIB_3p1_PRODUCT_VERSION;
static const union version 		nvmeib_3p1_protocol_version = NVMEIB_3p1_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_3p1_version = NVMEIB_3p1_VERSION_INIT;

#define NVMEIB_3p3_PRODUCT_VERSION 	{.major = 3,.minor = 3}
#define NVMEIB_3p3_PROTOCOL_VERSION 	{.major = 1,.minor = 11}
#define NVMEIB_3p3_VERSION_INIT 	{.product 	= NVMEIB_3p3_PRODUCT_VERSION, \
					.protocol 	= NVMEIB_3p3_PROTOCOL_VERSION}

static const union version 		nvmeib_3p3_product_version = NVMEIB_3p3_PRODUCT_VERSION;
static const union version 		nvmeib_3p3_protocol_version = NVMEIB_3p3_PROTOCOL_VERSION;
static const union nvmeib_version 	nvmeib_3p3_version = NVMEIB_3p3_VERSION_INIT;

#define NVMEIB_BASE_VERSION_INIT 		NVMEIB_13_VERSION_INIT
#define NVMEIB_CURRENT_VERSION_INIT 	NVMEIB_3p3_VERSION_INIT

#define NVMEIB_BASE_CLIENT_NAME_SIZE			64
#define NVMEIB_BASE_DISK_MAX_NVMEXPRESS_ID_SIZE	40

#ifndef PRIx64
#define PRIx64 "llx"
#endif

#define NVMEIB_VERSION_PRINT_FMT() \
	"{\"module\" : \"comn\", \"commit\" : \"%" PRIx64 "\", \"NVMesh\" : \"%d.%d.%d.%d\", \"Protocol\" : \"%d.%d.%d.%d\"}"

#define NVMEIB_VERSION_TRACE_FMT() \
	"{\"module\" : \"comn\", \"commit\" : \"@COMMIT_ID\", \"NVMesh\" : \"@MAJOR.@MINOR.@SUBMINOR.@MR\", \"Protocol\" : \"@MAJOR.@MINOR.@SUBMINOR.@MR\"}"

#define NVMEIB_VERSION_PRINT_ARG(_v_) \
		(u64)COMMIT_ID, \
		(_v_)->product.major, \
		(_v_)->product.minor, \
		(_v_)->product.subminor, \
		(_v_)->product.mr, \
		(_v_)->protocol.major, \
		(_v_)->protocol.minor, \
		(_v_)->protocol.subminor, \
		(_v_)->protocol.mr

#define NVMEIB_VERSION_PRINT_PROT_FMT() \
	"%d.%d.%d.%d"

#define NVMEIB_VERSION_PRINT_PROT_FMTN() \
	"@INT.@INT.@INT.@INT"

#define NVMEIB_VERSION_PRINT_PROT_ARG(_v_) \
	(_v_)->protocol.major, \
	(_v_)->protocol.minor, \
	(_v_)->protocol.subminor, \
	(_v_)->protocol.mr

//[IOCH-DRAINED TBD]: shall be integrated to VEX
static inline bool nvmeib_version_protocol_lt(const union nvmeib_version *va,
											  const union nvmeib_version *vb)
{
	bool lt;
	if ((lt = ((va)->protocol.all < (vb)->protocol.all))) {
		_NT(trace_nvmeib_version_protocol_lt,
			NVMEIB_VERSION_TRACE_FMT() " < "
			NVMEIB_VERSION_TRACE_FMT(),
			NVMEIB_VERSION_PRINT_ARG(va),
			NVMEIB_VERSION_PRINT_ARG(vb));
	}
	return lt;
}

#define NVMEIB_CONTAINER_MAGIC 				cpu_to_be64(0x4e766d65436f6e74ULL) /* "NvmeCont" as hex-value */
#define NVMEIB_CONTAINER_ELEMENT_MAGIC 		cpu_to_be64(0x4e76436e74456c6dULL) /* "NvCntElm" as hex-value */
#define NVMEIB_STRING_MAGIC					cpu_to_be64(0x4e76537472696e67ULL) /* "NvString" as hex-value */
#define NVMEIB_BITMAP_MAGIC					cpu_to_be64(0x4e764269746d6170ULL) /* "NvBitmap" as hex-value */
#define NVMEIB_GET_LOCK_GIDS_MAGIC			cpu_to_be64(0x4c6f636b47696473ULL) /* "LockGids" as hex-value */
#define NVMEIB_GET_IO_DISKINFO_MAGIC		cpu_to_be64(0x4469736b496e666fULL) /* "DiskInfo" as hex-value */
#define NVMEIB_GET_IO_PORTINFO_MAGIC		cpu_to_be64(0x506f7274496e666fULL) /* "PortInfo" as hex-value */
#define NVMEIB_ACCESS_MAP_CLNT_IONIC_MAGIC	cpu_to_be64(0x436c74496f4e6963ULL) /* "CltIoNic" as hex-value */
#define NVMEIB_ACCESS_MAP_SRV_IONIC_MAGIC	cpu_to_be64(0x537276496f4e6963ULL) /* "SrvIoNic" as hex-value */
#define NVMEIB_ACCESS_MAP_DISKS_MAGIC		cpu_to_be64(0x4163734469736b73ULL) /* "AcsDisks" as hex-value */
#define NVMEIB_ACCESS_MAP_ARNIC_MAGIC		cpu_to_be64(0x41637341726e6963ULL) /* "AcsArnic" as hex-value */
#define NVMEIB_TOMA_REQ_DATA_MAGIC			cpu_to_be64(0x546f6d6152657144ULL) /* "TomaReqD" as hex-value */
#define NVMEIB_TOMA_REQ_PB_MAGIC			cpu_to_be64(0x546f6d6152715062ULL) /* "TomaRqPb" as hex-value */
#define NVMEIB_IO_REQ_DIRECT_BUF_MAGIC		cpu_to_be64(0x496f446374427566ULL) /* "IoDctBuf" as hex-value */
#define NVMEIB_IO_REQ_INDIRECT_BUF_MAGIC	cpu_to_be64(0x496f496e64427566ULL) /* "IoIndBuf" as hex-value */

#define NVMEIB_EMPTY_MAGIC					cpu_to_be64(0ULL)
#define VEX_OPS_ZERO_MAGIC_STR 				"\0\0\0\0\0\0\0\0"

static inline void nvmeib_container_init(struct nvmeib_container *c, __be64 contents_magic,
										 u16 version_tag, unsigned n_elem, size_t size) {
	BUG_ON(size > (size_t)~(u32)0);
	BUG_ON(n_elem > (unsigned)~(u16)0);
	c->magic = NVMEIB_CONTAINER_MAGIC;
	c->contents_magic = contents_magic;
	c->size = cpu_to_be32(size);
	c->elem_stride = 0;
	c->n_elem = cpu_to_be16(n_elem);
	c->version_tag = cpu_to_be16(version_tag);
}

static inline void nvmeib_container_raw_payload_init(struct nvmeib_container *c, __be64 contents_magic,
													 u16 version_tag, size_t payload_sz) {
	size_t ctr_sz = payload_sz + sizeof(*c);
	BUG_ON(ctr_sz > (size_t)~(u32)0);
	c->magic = NVMEIB_CONTAINER_MAGIC;
	c->contents_magic = contents_magic;
	c->size = cpu_to_be32(ctr_sz);
	c->elem_stride = 0;
	c->n_elem = 0;
	c->version_tag = cpu_to_be16(version_tag);
}

static inline void nvmeib_container_cnst_stride_init(struct nvmeib_container *c, __be64 contents_magic,
													 u16 version_tag, unsigned n_elem, size_t elem_stride) {
	BUG_ON(n_elem > (unsigned)~(u16)0);
	BUG_ON(elem_stride > (size_t)~(u32)0);
	c->magic = NVMEIB_CONTAINER_MAGIC;
	c->contents_magic = contents_magic;
	c->elem_stride = cpu_to_be32(elem_stride);
	c->n_elem = cpu_to_be16(n_elem);
	c->size = cpu_to_be32(n_elem * elem_stride + sizeof(*c));
	c->version_tag = cpu_to_be16(version_tag);
}

static inline void nvmeib_container_empty_init(struct nvmeib_container *c) {
	c->magic = NVMEIB_CONTAINER_MAGIC;
	c->contents_magic = NVMEIB_EMPTY_MAGIC;
	c->elem_stride = 0;
	c->n_elem = 0;
	c->size = cpu_to_be32(sizeof(*c));
	c->version_tag = 0;
}

static inline void nvmeib_container_elem_init(struct nvmeib_container_elem *elem,
											  unsigned idx, size_t payload_sz) {
	elem->magic = NVMEIB_CONTAINER_ELEMENT_MAGIC;
	elem->idx = cpu_to_be16(idx);
	elem->size = cpu_to_be32(sizeof(*elem) + payload_sz);
}

int nvmeib_container_validate(const struct nvmeib_container *c,
							__be64 contents_magic, u16 version_tag);

static inline u16 nvmeib_container_n_elem(const struct nvmeib_container *c)
{
	return be16_to_cpu(c->n_elem);
}

static inline size_t nvmeib_container_elem_stride(const struct nvmeib_container *c) {
	return be32_to_cpu(c->elem_stride);
}

static inline ssize_t nvmeib_container_size(const struct nvmeib_container *c) {
	return be32_to_cpu(c->size);
}

static inline ssize_t nvmeib_container_payload_size(const struct nvmeib_container *c) {
	return be32_to_cpu(c->size) - sizeof(*c);
}

static inline const void* nvmeib_container_end(const struct nvmeib_container *c) {
	return (const void *)c + nvmeib_container_size(c);
}

int nvmeib_container_elem_validate(const struct nvmeib_container *c,
	const struct nvmeib_container_elem *elem,
	unsigned idx, size_t min_payload_sz);

static inline size_t nvmeib_container_elem_size(const struct nvmeib_container_elem *elem) {
	return be32_to_cpu(elem->size);
}

static inline const struct nvmeib_container_elem* nvmeib_container_elem_first(
	const struct nvmeib_container *c) {
	if (be16_to_cpu(c->n_elem)) return &c->elements[0];
	else return NULL;
}

static inline const struct nvmeib_container_elem* nvmeib_container_elem_next(
	const struct nvmeib_container *c,
	const struct nvmeib_container_elem *elem) {
	const struct nvmeib_container_elem *rv = NULL;
	if (be16_to_cpu(elem->idx) < be16_to_cpu(c->n_elem)) {
		rv = (void *)elem + be32_to_cpu(elem->size);
		if ((const void *)(rv + 1) > (const void *)c + be32_to_cpu(c->size)) rv = NULL;
	}
	return rv;
}

#define for_each_nvmeib_container_elem(c, tmp_elem, idx, elem_struct, rv) \
		for(idx = 0, rv = 0, tmp_elem = nvmeib_container_elem_first(c), \
				elem_struct = NVMEIB_CONT_ELEM_PAYLOAD(tmp_elem); \
			tmp_elem && (rv = nvmeib_container_elem_validate(\
				c, tmp_elem, idx, sizeof(*elem_struct))) >= 0; \
			idx++, tmp_elem = nvmeib_container_elem_next(c, tmp_elem), \
				elem_struct = (tmp_elem ? NVMEIB_CONT_ELEM_PAYLOAD(tmp_elem) : NULL))

#define for_each_nvmeib_container_elem_cnst_stride(c, idx, elem_struct) \
		for(idx = 0, elem_struct = NVMEIB_CONT_PAYLOAD(c); \
			idx < nvmeib_container_n_elem(c); idx++, elem_struct++)


#define CAST_TO_NVMEIB_CONT(RC) ((struct nvmeib_container *)(RC))
#define CAST_TO_CONST_NVMEIB_CONT(RC) ((const struct nvmeib_container *)(RC))

#define NVMEIB_CONT_ELEM_SIZE(elem_struct) (sizeof(struct nvmeib_container_elem) + sizeof(elem_struct))

#define NVMEIB_CONT_PAYLOAD(cont)		(void *)(cont)->payload
#define NVMEIB_CONT_CONST_PAYLOAD(cont)		(const void *)(cont)->payload

#define NVMEIB_CONT_ELEM_PAYLOAD(elem) (void *)(elem)->payload
#define NVMEIB_CONT_ELEM_CONST_PAYLOAD(elem) (const void *)(elem)->payload

#define NVMEIB_CONT_PAYLOAD_SIZE(_n, elem_struct) (_n * NVMEIB_CONT_ELEM_SIZE(elem_struct))
#define NVMEIB_CONTAINER_SIZE(_n, elem_struct)\
	((offsetof(struct nvmeib_container, payload) + \
		NVMEIB_CONT_PAYLOAD_SIZE(_n, elem_struct)))

#define NVMEIB_CONT_ELEM_IS_SPC_AVAIL(_head, _lim, elem_struct) \
	((_head + NVMEIB_CONT_ELEM_SIZE(elem_struct)) <= _lim)

#define NVMEIB_CONT_IS_SPC_AVAIL(_head, _lim, _n, elem_struct) \
	((_head + NVMEIB_CONTAINER_SIZE(_n, elem_struct)) <= _lim)

#define NVMEIB_CONT_IS_EMPTY(cont) \
	((cont)->contents_magic == NVMEIB_EMPTY_MAGIC)

union nvmeib_version nvmeib_version_get(void);

/*
 * Gregory - in this release server accepts matching or advanced versions only
 */
static inline bool nvmeibs_compare_versions(
	const union nvmeib_version *c_ver, const union nvmeib_version *s_ver) {
	bool verdict = false;
	if (c_ver->product.all > s_ver->product.all) {
		verdict = true;
	} else if (c_ver->product.all == s_ver->product.all) {
		verdict = c_ver->protocol.all >= s_ver->protocol.all;
	}
	return verdict;
}

enum vex_ext_enum {
	vex_invalid = -1,
	vex_base = 0,
	vex_ext1,
	vex_ext2,
	vex_ext3,
	vex_ext4,
};

enum vex_sz_type_enum {
	vex_const_sz = 0,
	vex_var_sz,
};

struct wire_dummy {
} __attribute__((packed));

struct vex_encode_dref_alloc_ctx;
/* Signature for vex_ops fns */
#define VEX_OPS_INIT_FN_SIG(fn_name)	int fn_name(__attribute__((unused)) enum vex_ext_enum link_ext, __attribute__((unused)) const struct vex_ops *ops, void *arg)
#define VEX_OPS_ENCODE_FN_SIG(fn_name)	ssize_t fn_name(__attribute__((unused)) enum vex_ext_enum link_ext, __attribute__((unused)) const struct vex_ops *ops, __attribute__((unused)) struct vex_encode_dref_alloc_ctx *dref_alloc_ctx, void *wire_buf, const void *wire_buf_end, int elem_idx, void *arg)
#define VEX_OPS_DECODE_FN_SIG(fn_name)	ssize_t fn_name(__attribute__((unused)) enum vex_ext_enum link_ext, __attribute__((unused)) const struct vex_ops *ops, const void *wire_buf, const void *wire_buf_end, __attribute__((unused)) int elem_idx, __attribute__((unused)) int n_elem, void *arg)
#define VEX_OPS_FINI_FN_SIG(fn_name)	int fn_name(__attribute__((unused)) enum vex_ext_enum link_ext, __attribute__((unused)) const struct vex_ops *ops, void *arg)

/*
 * A structure can have several encoders / decoders / initializers etc
 * Let the type to assing ops
 */
struct vex_ops {
	union {
		const char magic_str[8];                /* Magic String / Value - Used for Containers */
		__be64 magic_val;
	};
	union nvmeib_version version;
	enum vex_ext_enum vex_ext;				/* This extension in the array */
	enum vex_ext_enum vex_max_ext;			/* The maximum extension in the array */
	enum vex_sz_type_enum vex_sz_type;		/* Is the contents, fixed or variable size */
	size_t wire_size;
	VEX_OPS_INIT_FN_SIG((* init));
	VEX_OPS_ENCODE_FN_SIG((* encode));
	VEX_OPS_DECODE_FN_SIG((* decode));
	VEX_OPS_FINI_FN_SIG((* fini));
};

/*
 * DREF references memory filled during run time.
 * DREF meory areas stacked after "simple" structure types
 * the first dref area follows the last structure member
 * each new dref area follows previous one
 */
struct vex_dref {
	union {
		__be64 contents_magic; /* Contents Magic Value */
		char contents_magic_str[8]; /* Contents Magic String */
	};
	u8 pad[2];
	__be16 version_tag; /* Version tag (vex_base, etc.) */
	__be32 offset;  /* offset in bytes from parent structure base */
	__be32 size	/* size in bytes */;
};

static inline void* vex_dref_memory(const struct vex_dref* dref) {
	void* ret = (void *)dref + be32_to_cpu(dref->offset);
	BUG_ON(!IS_ALIGNED((unsigned long)ret, sizeof(u64)));
	return ret;
}

static inline u32 vex_dref_get_size(const struct vex_dref *dref)
{
	return be32_to_cpu(dref->size);
}

static inline void vex_dref_set_offset(struct vex_dref* dref,
									   const struct vex_dref* prev, const struct vex_ops* ops) {
	if (!prev) {
		dref->offset = cpu_to_be32(ops->wire_size);
	} else {
		u32 offset = be32_to_cpu(prev->offset);
		u32 size = be32_to_cpu(prev->size);
		dref->offset = cpu_to_be32(offset + size);
	}
}

static inline void vex_dref_set_size(struct vex_dref* dref, u32 size) {
	dref->size = cpu_to_be32(size);
}

static inline void vex_dref_init_empty(void *dref_ptr, const struct vex_ops* ops) {
	/* Init struct on stack and the memcpy to msg
	 * (Solves alignment warning) */
	struct vex_dref dref = {
		.contents_magic = NVMEIB_EMPTY_MAGIC,
		.offset = 0,
		.size = 0,
		.version_tag = cpu_to_be16(ops->vex_ext),
	};

	vex_memcpy(dref_ptr, &dref, sizeof(dref));
}

static inline int link_version_match(union nvmeib_version link_ver, union nvmeib_version ops_ver)
{
	return link_ver.protocol.all >= ops_ver.protocol.all;
}

static inline const struct vex_ops *vex_select_ops(union nvmeib_version link_ver, const struct vex_ops *const vex_ops_extensions)
{
	const struct vex_ops *ops, *base_ops = vex_ops_extensions;

	if (base_ops == &nvmeib_vex_invalid_ops) {
		ops = &nvmeib_vex_invalid_ops;
		goto out;
	}

	for (ops = base_ops + (base_ops->vex_max_ext - base_ops->vex_ext); ops >= base_ops; ops--) {
		if (link_version_match(link_ver, ops->version)) goto out;
	}
	ops = &nvmeib_vex_invalid_ops;

out:
	return ops;
}

void vex_link_ops(union nvmeib_version link_ver,
		const struct vex_ops * const *vex_ops_collection, const struct vex_ops **link_ops,
		const char *(*vex_op_name)(int),
		int ops_collection_size);

static inline const struct vex_ops* vex_base_ops(const struct vex_ops* vex_ops) {
	return vex_ops - vex_ops->vex_ext;
}

static inline int vex_init(const struct vex_ops* link_ext_ops, void* arg) {
	int rv = -EPROTO;
	const struct vex_ops* ops_itr;

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	for (ops_itr = vex_base_ops(link_ext_ops);
		 ops_itr <= link_ext_ops; ++ops_itr) {
		rv = ops_itr->init(link_ext_ops->vex_ext, ops_itr, arg);
		if (rv < 0) break;
	}

out:
	return rv;
}

struct vex_encode_dref_alloc_ctx {
	const struct vex_ops* link_ext_ops;
	void* arg;
	void* dref_base;
	void* dref_curr;
	void* out;
	const void* out_end;
};

static inline ssize_t vex_size(const struct vex_ops* link_ext_ops, void* arg, bool only_const);

/* Allocates memory for a dref object at the end of the current encode context */
int vex_encode_alloc_dref(void **alloc_ptr, struct vex_encode_dref_alloc_ctx* dref_alloc_ctx,
	size_t size, __be64 contents_magic, void *dref);

/* Allocates and encodes a dref string */
static inline ssize_t vex_encode_dref_str(struct vex_encode_dref_alloc_ctx* dref_alloc_ctx,
	const char* str, size_t str_len, void *dref_ptr) {
	ssize_t rv;
	size_t size = str_len;
	void *dst;

	if (!size) size = strlen(str) + 1;

	if ((rv = vex_encode_alloc_dref(&dst, dref_alloc_ctx, size, NVMEIB_STRING_MAGIC, dref_ptr)) < 0) {
		goto out;
	}
	strlcpy(dst, str, str_len);
	rv = size;
out:
	return rv;
}

/* Allocates and encodes a dref bitmap */
static inline ssize_t vex_encode_dref_bitmap(struct vex_encode_dref_alloc_ctx* dref_alloc_ctx,
	const unsigned long* bmp, unsigned int nbits, void *dref_ptr) {
	ssize_t rv;
	size_t size = DIV_ROUND_UP(nbits, 32);
	void *dst;

	if ((rv = vex_encode_alloc_dref(&dst, dref_alloc_ctx, size, NVMEIB_BITMAP_MAGIC, dref_ptr)) < 0) {
		goto out;
	}
	nvmeib_bitmap_to_be32(dst, bmp, nbits);
	rv = size;
out:
	return rv;
}

static inline ssize_t vex_encode_elem(const struct vex_ops* link_ext_ops,
	void* out, const void* out_end, int elem_idx, void* arg) {
	ssize_t rv = -EPROTO;
	const struct vex_ops* ops_itr;
	void* ext_out = out;
	struct vex_encode_dref_alloc_ctx dref_alloc_ctx = {
		.link_ext_ops = link_ext_ops,
		.arg = arg,
		.out = out,
		.out_end = out_end,
	};

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	for (ops_itr = vex_base_ops(link_ext_ops);
		 ops_itr <= link_ext_ops; ++ops_itr) {
		if (ops_itr->wire_size && ext_out + ops_itr->wire_size > out_end) {
			rv = -ENOSPC;
			goto out;
		}

		//@ext_out is 'wire_buf', @rv is expected to be sizeof(ext) on success or < 0 otherwise.
		rv = ops_itr->encode(link_ext_ops->vex_ext, ops_itr, &dref_alloc_ctx, ext_out, out_end, elem_idx, arg);
		if (rv < 0) goto out;
		BUG_ON(ops_itr->vex_sz_type == vex_const_sz && (size_t)rv != ops_itr->wire_size);
		ext_out += rv;
	}
	/* If we have allocated memory for dref(s) include that in the size calculation */
	if (dref_alloc_ctx.dref_base) {
		BUG_ON(dref_alloc_ctx.dref_base < ext_out);
		BUG_ON(!dref_alloc_ctx.dref_curr);
		if (dref_alloc_ctx.dref_curr != dref_alloc_ctx.dref_base) rv = dref_alloc_ctx.dref_curr - out;
		goto out;
	}
	rv = ext_out - out;

out:
	return rv;
}

static inline ssize_t vex_encode(const struct vex_ops* link_ext_ops,
								 void* out, const void* out_end, void* arg) {
	return vex_encode_elem(link_ext_ops, out, out_end, 0, arg);
}

static inline int vex_fini(const struct vex_ops* link_ext_ops, void* arg) {
	int rv;
	const struct vex_ops* ops_itr,* base_ops = vex_base_ops(link_ext_ops);

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	for (ops_itr = link_ext_ops; ops_itr >= base_ops; ops_itr--) {
		rv = link_ext_ops->fini(link_ext_ops->vex_ext, ops_itr, arg);
		if (rv < 0)
			goto out;
	}
	rv = 0;

out:
	return rv;
}

static inline ssize_t vex_size(const struct vex_ops* link_ext_ops, void* arg, bool only_const) {
	ssize_t rv;
	size_t sz = 0;
	const struct vex_ops* ops_itr;

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	for (ops_itr = vex_base_ops(link_ext_ops);
		 ops_itr <= link_ext_ops; ++ops_itr, sz += rv) {
		if (ops_itr->vex_sz_type == vex_const_sz) rv = ops_itr->wire_size;
		else if (only_const) rv = -EPROTO;
		else if (ops_itr->encode) rv = ops_itr->encode(link_ext_ops->vex_ext, ops_itr, NULL, NULL, NULL, 0, arg);
		else rv = -EPROTO;

		if (rv < 0) goto out;
	}
	rv = sz;

out:
	return rv;
}

static inline ssize_t vex_decode_elem(const struct vex_ops* link_ext_ops,
									  const void* in, const void* in_end, int elem_idx, int n_elem, void* arg) {
	ssize_t rv = -EPROTO;
	const struct vex_ops* ops_itr;
	const struct vex_ops* max_ops_ext;
	const void* ext_in = in;

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	ops_itr = vex_base_ops(link_ext_ops);
	max_ops_ext = ops_itr +
		(ops_itr->vex_max_ext - ops_itr->vex_ext);
	for (; ops_itr <= max_ops_ext; ++ops_itr) {
		if (ops_itr <= link_ext_ops) {
			if (ops_itr->wire_size && ext_in + ops_itr->wire_size > in_end) {
			_NE(err_vex_decode_elem_no_spc, "Out of space processing extension @INT of container magic @STR_8 (in @PX ext_in @PX wire_size @ZU in_end @PX",
				ops_itr->vex_ext, ops_itr->magic_str, in, ext_in, ops_itr->wire_size, in_end);
				rv = -ENOSPC;
				goto out;
			}
			if ((rv = ops_itr->decode(link_ext_ops->vex_ext, ops_itr, ext_in, in_end, elem_idx, n_elem, arg)) < 0)
				goto out;
			BUG_ON(ops_itr->vex_sz_type == vex_const_sz && (size_t)rv != ops_itr->wire_size);
			ext_in += rv;
		} else {
			/* Send NULL buffer to fill defaults */
			if ((rv = ops_itr->decode(link_ext_ops->vex_ext, ops_itr,
									  NULL, NULL, elem_idx, n_elem, arg)) < 0) goto out;
			BUG_ON(rv > 0);
		}
	}
	rv = ext_in - in;
out:
	return rv;
}

static inline ssize_t vex_decode(const struct vex_ops* link_ext_ops,
								 const void* in, const void* in_end, void* arg) {
	return vex_decode_elem(link_ext_ops, in, in_end, 0, 1, arg);
}

static inline int vex_encode_container_hdr(struct nvmeib_container **c_out,
					const struct vex_ops* link_ext_ops,
					void* out, const void* out_end) {
	struct nvmeib_container *c = out;
	ssize_t rv;

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	if (out + sizeof(*c) > out_end) {
		rv = -ENOSPC;
		goto out;
	}

	if ((rv = vex_size(link_ext_ops, NULL, true)) >= 0) {
		/* All extensions are constant size */
		nvmeib_container_cnst_stride_init(c, link_ext_ops->magic_val,
						  link_ext_ops->vex_ext, 0, rv);
	} else {
		/* One or more extensions are of variable size */
		nvmeib_container_init(c, link_ext_ops->magic_val,
				  link_ext_ops->vex_ext, 0, sizeof(*c));
		rv = 0;
	}

	if (c_out)
		*c_out = c;
out:
	return rv;
}

static inline ssize_t vex_encode_container_elem(const struct vex_ops* link_ext_ops,
												struct nvmeib_container* c,
												const void* out_end, void* arg) {
	ssize_t rv;
	size_t ctr_size = be32_to_cpu(c->size);
	size_t elem_stride = be32_to_cpu(c->elem_stride);
	int elem_idx = be16_to_cpu(c->n_elem);
	void* out = (void *)c + ctr_size;
	struct nvmeib_container_elem* elem = out;
	void* payload = elem_stride != 0 ? out : NVMEIB_CONT_ELEM_PAYLOAD(elem);

	if (link_ext_ops->vex_ext == vex_invalid) {
		rv = -EOPNOTSUPP;
		goto out;
	}

	if ((rv = vex_encode_elem(link_ext_ops, payload, out_end, elem_idx, arg)) < 0) goto out;

	BUG_ON(payload + rv > out_end);

	if (elem_stride != 0) {
		/* Constant stride container - check size returned from encode */
		BUG_ON((size_t)rv != elem_stride);
	} else {
		/* Variable stride container - Init Element Header */
		nvmeib_container_elem_init(elem, elem_idx, rv);
		rv += sizeof(*elem);
	}

	c->n_elem = cpu_to_be16(elem_idx + 1);
	c->size = cpu_to_be32(ctr_size + rv);

out:
	return rv;
}

ssize_t vex_decode_container(const struct vex_ops* link_ext_ops,
	const void* in, const void* in_end, void* arg);

/* ****************************************************************
 * Macros for VEX_OPS declarations and definitions
 * ****************************************************************
 * DO NOT TOUCH UNLESS YOU KNOW WHAT YOU ARE DOING!
 * ****************************************************************
 *
 * SYNTAX:
 * VEX_OPS(ops_enum, decl_def_struct, decl_fns, num_ext, num_ops,
 * 			ops_struct,
 * 				base, base_ver, base_wire_struct,
 * 					base_op0_type, base_op0_flags, base_op0_fn,
 * 					...
 * 					base_opO_type, base_opO_flags, base_opO_fn,
 * 				ext0, ext0_ver, ext0_wire_struct,
 * 					ext0_op0_type, ext0_op0_flags, ext0_op0_fn,
 * 					...
 * 					ext0_opO_type, ext0_opO_flags, ext0_opO_fn,
 * 				...
 * 				extE, extE_ver, extE_wire_struct,
 * 					extE_op0_type, extE_op0_flags, extE_op0_fn,
 * 					...
 * 					extE_opO_type, extE_opO_flags, extE_opO_fn,
 *
 * WHERE:
 * 	-	ops_enum - Enum value for the OPs (used in vex_ops_collection)
 * 	-	decl_def_struct - Either DECLARE_STRUCT or DEFINE_STRUCT
 *  -	decl_fns - Either NO_DECLARE_FNS or NO_DECLARE_FNS
 * 	-	num_ext - BASE_ONLY, ONE_EXT, TWO_EXT, THREE_EXT, FOUR_EXT
 * 	-	num_ops - NO_OPS, ONE_OP, TWO_OPS, THREE_OPS, FOUR_OPS, FIVE_OPS
 *	-	ops_struct - Struct name
 * 	-	base_ver - Base Version
 * 	-	base_wire_struct - Struct for the base format on the wire
 * 	-	base_op0_type, ext0_op0_type, extE_op0_type - op type - One of: init, encode, decode, fini, defaults
 * 	-	base_op0_flags, ext0_op0_flags, ext0_opO_flags - op function flags - static, extern
 * 	- 	base_op0_fn, extE_op0_fn, extE_opO_fn - op function name
 *
 * EXAMPLE:
 *
 * VEX_OPS(vex_nrch_io_read_clnt, DEFINE_STRUCT, DECLARE_FNS, BASE_ONLY, TWO_OPS,
			vex_nrch_io_read_clnt_ops,
				base, NVMEIB_BASE_VERSION, volume_client_io_req_base,
					encode, static, vex_nrch_io_read_clnt_base_encode,
					fini, static, vex_nrch_io_read_clnt_base_fini);
 * EXAMPLE OUTPUT:
	static int vex_nrch_io_read_clnt_base_encode(void *arg);
	static int vex_nrch_io_read_clnt_base_fini(void *arg);
	const struct vex_nrch_io_read_clnt_ops[1] = {
		[vex_base] = {
			.version = NVMEIB_BASE_VERSION,
			.vex_ext = vex_base
			.wire_size = sizeof(struct volume_client_io_req_base)
			.encode = vex_nrch_io_read_clnt_base_encode,
			.fini = vex_nrch_io_read_clnt_base_fini,
		}
	}

 * EXAMPLE USE:
	rv = CALL_VEX_OP(encode, vex_ops, &cmd_ctx,
					vex_nrch_io_read_clnt_ops, BASE_ONLY,
					base, vex_nrch_io_read_clnt_base_encode);
 */

#ifdef VEX_OPS_V
	#define VEX_OPS_LOG_V1(...)	NV_PP_MSG_IF(NV_PP_GT(VEX_OPS_V,0),__VA_ARGS__)
	#define VEX_OPS_LOG_V2(...)	NV_PP_MSG_IF(NV_PP_GT(VEX_OPS_V,1),__VA_ARGS__)
#else
	#define VEX_OPS_LOG_V1(...)
	#define VEX_OPS_LOG_V2(...)
#endif

#define VEX_OPS_EXT_ENT_N_PARAMS	4	/* extension (base, ext1, ...), version, wire_struct, vex_sz_type */
#define VEX_OPS_EXT_OP_N_PARAMS		3	/* fn_type (encode, decode, ...) , fn_flags, fn_name */

#define VEX_DECLARE_OPS_FIVE_OPS(magic_str, max_ext, ...) \
	VEX_DECLARE_OPS_HDR(magic_str, max_ext, __VA_ARGS__),\
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_ONE_OP_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_TWO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_THREE_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_FOUR_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_FTR()

#define VEX_DECLARE_OPS_FOUR_OPS(magic_str, max_ext, ...) \
	VEX_DECLARE_OPS_HDR(magic_str, max_ext, __VA_ARGS__),\
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_ONE_OP_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_TWO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_THREE_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_FTR()

#define VEX_DECLARE_OPS_THREE_OPS(magic_str, max_ext, ...) \
	VEX_DECLARE_OPS_HDR(magic_str, max_ext, __VA_ARGS__),\
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_ONE_OP_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_TWO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_FTR()

#define VEX_DECLARE_OPS_TWO_OPS(magic_str, max_ext, ...) \
	VEX_DECLARE_OPS_HDR(magic_str, max_ext, __VA_ARGS__),\
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_ONE_OP_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_FTR()

#define VEX_DECLARE_OPS_ONE_OP(magic_str, max_ext, ...) \
	VEX_DECLARE_OPS_HDR(magic_str, max_ext, __VA_ARGS__),\
	VEX_DECLARE_OPS_OP_FN(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__)), \
	VEX_DECLARE_OPS_FTR()

#define VEX_DECLARE_OPS_NO_OPS(max_ext, ...) \
	VEX_DECLARE_OPS_HDR(max_ext, __VA_ARGS__), \
	VEX_DECLARE_OPS_FTR()

#define VEX_DECLARE_OPS_HDR(magic_str_val, max_ext, vtag, nvmesh_version, type, sz_type, ...) \
	[vex_##vtag] = {\
		.magic_str = magic_str_val,\
		.version = NV_PP_DEFER(NV_PP_VARGS nvmesh_version),\
		.vex_ext = vex_##vtag,\
		.vex_max_ext = vex_##max_ext, \
		.wire_size = sizeof(struct type), \
		.vex_sz_type = vex_##sz_type

#define VEX_DECLARE_OPS_FTR() }

#define _VEX_DECLARE_OPS_OP_FN(op_fn_field, op_fn_flags, op_fn, ...) \
		.op_fn_field = op_fn

#define VEX_DECLARE_OPS_OP_FN(...) \
			NV_PP_DEFER(_VEX_DECLARE_OPS_OP_FN(__VA_ARGS__))

#define VEX_OPS_DECLARE_FNS_BASE_ONLY(num_ops, ...) \
	VEX_OPS_DECLARE_FNS_##num_ops(NV_PP_EAT(VEX_OPS_EXT_ENT_N_PARAMS, __VA_ARGS__))

#define VEX_OPS_DECLARE_FNS_ONE_EXT(num_ops, ...) \
	VEX_OPS_DECLARE_FNS_BASE_ONLY(num_ops, __VA_ARGS__);\
	VEX_OPS_DECLARE_FNS(num_ops, NV_PP_EAT(VEX_OPS_EXT_ENT_N_PARAMS, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__)))

#define VEX_OPS_DECLARE_FNS_TWO_EXT(num_ops, ...) \
	VEX_OPS_DECLARE_FNS_ONE_EXT(num_ops, __VA_ARGS__);\
	VEX_OPS_DECLARE_FNS(num_ops, NV_PP_EAT(VEX_OPS_EXT_ENT_N_PARAMS, VEX_OPS_EAT_2_PHASE_PARAMS(num_ops, __VA_ARGS__)))

#define VEX_OPS_DECLARE_FNS_THREE_EXT(num_ops, ...) \
	VEX_OPS_DECLARE_FNS_TWO_EXT(num_ops, __VA_ARGS__);\
	VEX_OPS_DECLARE_FNS(num_ops, NV_PP_EAT(VEX_OPS_EXT_ENT_N_PARAMS, VEX_OPS_EAT_3_PHASE_PARAMS(num_ops, __VA_ARGS__)))

#define VEX_OPS_DECLARE_FNS_FOUR_EXT(num_ops, ...) \
	VEX_OPS_DECLARE_FNS_THREE_EXT(num_ops, __VA_ARGS__);\
	VEX_OPS_DECLARE_FNS(num_ops, NV_PP_EAT(VEX_OPS_EXT_ENT_N_PARAMS, VEX_OPS_EAT_4_PHASE_PARAMS(num_ops, __VA_ARGS__)))

#define VEX_OPS_DECLARE_FNS(num_ops, ...)\
	VEX_OPS_DECLARE_FNS_##num_ops(__VA_ARGS__)

#define VEX_OPS_DECLARE_FNS_NO_OPS(...)

#define VEX_OPS_DECLARE_FNS_ONE_OP(...) \
	VEX_OPS_DECLARE_OP_FN(__VA_ARGS__)

#define VEX_OPS_DECLARE_FNS_TWO_OPS(...) \
	VEX_OPS_DECLARE_FNS_ONE_OP(__VA_ARGS__); \
	VEX_OPS_DECLARE_FNS_ONE_OP(VEX_OPS_EAT_1_OPS_PARAMS(__VA_ARGS__));

#define VEX_OPS_DECLARE_FNS_THREE_OPS(...) \
	VEX_OPS_DECLARE_FNS_TWO_OPS(__VA_ARGS__); \
	VEX_OPS_DECLARE_FNS_ONE_OP(VEX_OPS_EAT_2_OPS_PARAMS(__VA_ARGS__));

#define VEX_OPS_DECLARE_FNS_FOUR_OPS(...) \
	VEX_OPS_DECLARE_FNS_THREE_OPS(__VA_ARGS__); \
	VEX_OPS_DECLARE_FNS_ONE_OP(VEX_OPS_EAT_3_OPS_PARAMS(__VA_ARGS__));

#define VEX_OPS_DECLARE_FNS_FIVE_OPS(...) \
	VEX_OPS_DECLARE_FNS_FOUR_OPS(__VA_ARGS__); \
	VEX_OPS_DECLARE_FNS_ONE_OP(VEX_OPS_EAT_4_OPS_PARAMS(__VA_ARGS__));

#define VEX_OPS_DECLARE_OP_FN(op_fn_field, ...) \
	VEX_OPS_DECLARE_OP_FN_##op_fn_field(__VA_ARGS__)

#define VEX_OPS_DECLARE_OP_FN_init(op_fn_flags, op_fn, ...) \
	VEX_OPS_LOG_V2(__BASE_FILE__ ": VEX_OPS - Declare Op Init Fn: " NV_PP_STR(op_fn)); \
	op_fn_flags VEX_OPS_INIT_FN_SIG(op_fn)

#define VEX_OPS_DECLARE_OP_FN_encode(op_fn_flags, op_fn, ...) \
	VEX_OPS_LOG_V2(__BASE_FILE__ ": VEX_OPS - Declare Op Encode Fn: " NV_PP_STR(op_fn)); \
	op_fn_flags VEX_OPS_ENCODE_FN_SIG(op_fn)

#define VEX_OPS_DECLARE_OP_FN_decode(op_fn_flags, op_fn, ...) \
	VEX_OPS_LOG_V2(__BASE_FILE__ ": VEX_OPS - Declare Op Decode Fn: " NV_PP_STR(op_fn)); \
	op_fn_flags VEX_OPS_DECODE_FN_SIG(op_fn)

#define VEX_OPS_DECLARE_OP_FN_fini(op_fn_flags, op_fn, ...) \
	VEX_OPS_LOG_V2(__BASE_FILE__ ": VEX_OPS - Declare Op Fini Fn: " NV_PP_STR(op_fn)); \
	op_fn_flags VEX_OPS_FINI_FN_SIG(op_fn)

#define VEX_OPS_STRUCT_DECLARE_FNS(ops_enum, num_ext, num_ops, ops_struct, ...)\
	VEX_OPS_LOG_V1(__BASE_FILE__ ": VEX_OPS: " NV_PP_STR(ops_enum) " - Declare Fns: " NV_PP_STR(ops_struct) " Num Ext: " NV_PP_STR(num_ext) " Num Ops: " NV_PP_STR(num_ops)); \
	VEX_OPS_DECLARE_FNS_##num_ext(num_ops, __VA_ARGS__)

#define VEX_OPS_TERMINATOR { .version = 0 }

#define VEX_OPS_EAT_1_OPS_PARAMS(...) \
	NV_PP_EAT(VEX_OPS_EXT_OP_N_PARAMS, __VA_ARGS__)

#define VEX_OPS_EAT_2_OPS_PARAMS(...) \
	NV_PP_EAT(VEX_OPS_EXT_OP_N_PARAMS, VEX_OPS_EAT_1_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_3_OPS_PARAMS(...) \
	NV_PP_EAT(VEX_OPS_EXT_OP_N_PARAMS, VEX_OPS_EAT_2_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_4_OPS_PARAMS(...) \
	NV_PP_EAT(VEX_OPS_EXT_OP_N_PARAMS, VEX_OPS_EAT_3_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_5_OPS_PARAMS(...) \
	NV_PP_EAT(VEX_OPS_EXT_OP_N_PARAMS, VEX_OPS_EAT_4_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, ...) \
	VEX_OPS_EAT_1_PHASE_##num_ops##_PARAMS(__VA_ARGS__)

#define VEX_OPS_EAT_2_PHASE_PARAMS(num_ops, ...) \
	VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__))

#define VEX_OPS_EAT_3_PHASE_PARAMS(num_ops, ...) \
	VEX_OPS_EAT_2_PHASE_PARAMS(num_ops, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__))

#define VEX_OPS_EAT_4_PHASE_PARAMS(num_ops, ...) \
	VEX_OPS_EAT_3_PHASE_PARAMS(num_ops, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__))

#define VEX_OPS_EAT_5_PHASE_PARAMS(num_ops, ...) \
	VEX_OPS_EAT_4_PHASE_PARAMS(num_ops, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__))

#define VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(...) \
	NV_PP_EAT(VEX_OPS_EXT_ENT_N_PARAMS, __VA_ARGS__)

#define VEX_OPS_EAT_1_PHASE_ONE_OP_PARAMS(...) \
	VEX_OPS_EAT_1_OPS_PARAMS(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_1_PHASE_TWO_OPS_PARAMS(...) \
	VEX_OPS_EAT_2_OPS_PARAMS(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_1_PHASE_THREE_OPS_PARAMS(...) \
	VEX_OPS_EAT_3_OPS_PARAMS(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_1_PHASE_FOUR_OPS_PARAMS(...) \
	VEX_OPS_EAT_4_OPS_PARAMS(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_EAT_1_PHASE_FIVE_OPS_PARAMS(...) \
	VEX_OPS_EAT_5_OPS_PARAMS(VEX_OPS_EAT_1_PHASE_NO_OPS_PARAMS(__VA_ARGS__))

#define VEX_OPS_DEFINE_STRUCT_FOUR_EXT(num_ops, ops_struct, magic_str, ...) \
	const struct vex_ops ops_struct[5] = { \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext4, __VA_ARGS__), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext4, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext4, VEX_OPS_EAT_2_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext4, VEX_OPS_EAT_3_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext4, VEX_OPS_EAT_4_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
	}

#define VEX_OPS_DEFINE_STRUCT_THREE_EXT(num_ops, ops_struct, magic_str, ...) \
	const struct vex_ops ops_struct[4] = { \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext3, __VA_ARGS__), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext3, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext3, VEX_OPS_EAT_2_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext3, VEX_OPS_EAT_3_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
	}

#define VEX_OPS_DEFINE_STRUCT_TWO_EXT(num_ops, ops_struct, magic_str, ...) \
	const struct vex_ops ops_struct[3] = { \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext2, __VA_ARGS__), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext2, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext2, VEX_OPS_EAT_2_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
	}

#define VEX_OPS_DEFINE_STRUCT_ONE_EXT(num_ops, ops_struct, magic_str, ...) \
	const struct vex_ops ops_struct[2] = { \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext1, __VA_ARGS__), \
		VEX_DECLARE_OPS_##num_ops(magic_str, ext1, VEX_OPS_EAT_1_PHASE_PARAMS(num_ops, __VA_ARGS__)), \
	}

#define VEX_OPS_DEFINE_STRUCT_BASE_ONLY(num_ops, ops_struct, magic_str, ...) \
	const struct vex_ops ops_struct[1] = { \
		VEX_DECLARE_OPS_##num_ops(magic_str, base, __VA_ARGS__), \
	}

#define VEX_OPS_DEFINE_STRUCT(ops_enum, num_ext, num_ops, ops_struct, magic_str, ...)\
	VEX_OPS_LOG_V1(__BASE_FILE__ ": VEX_OPS: " NV_PP_STR(ops_enum) " - Define Struct: " NV_PP_STR(ops_struct) " Num Ext: " NV_PP_STR(num_ext) " Num Ops: " NV_PP_STR(num_ops)); \
	VEX_OPS_DEFINE_STRUCT_##num_ext(num_ops, ops_struct, magic_str, __VA_ARGS__)

#define VEX_OPS_DECLARE_STRUCT_FOUR_EXT(ops_struct) \
	extern const struct vex_ops ops_struct[5]

#define VEX_OPS_DECLARE_STRUCT_THREE_EXT(ops_struct) \
	extern const struct vex_ops ops_struct[4]

#define VEX_OPS_DECLARE_STRUCT_TWO_EXT(ops_struct) \
	extern const struct vex_ops ops_struct[3]

#define VEX_OPS_DECLARE_STRUCT_ONE_EXT(ops_struct) \
	extern const struct vex_ops ops_struct[2]

#define VEX_OPS_DECLARE_STRUCT_BASE_ONLY(ops_struct) \
	extern const struct vex_ops ops_struct[1]

#define VEX_OPS_DECLARE_STRUCT(ops_enum, num_ext, num_ops, ops_struct, magic_str, ...)\
	VEX_OPS_LOG_V2(__BASE_FILE__ ": VEX_OPS: " NV_PP_STR(ops_enum) " - Declare Struct: " NV_PP_STR(ops_struct) " Num Ext: " NV_PP_STR(num_ext) " Num Ops: " NV_PP_STR(num_ops)); \
	VEX_OPS_DECLARE_STRUCT_##num_ext(ops_struct)

#define VEX_OPS_STRUCT_NO_DECLARE_FNS(...)

#define _VEX_OPS(ops_enum, def_decl_struct, def_decl_fns, num_ext, num_ops, ops_struct, magic_str, ...)\
	VEX_OPS_STRUCT_##def_decl_fns(ops_enum, num_ext, num_ops, ops_struct, __VA_ARGS__); \
	VEX_OPS_##def_decl_struct(ops_enum, num_ext, num_ops, ops_struct, magic_str, __VA_ARGS__)

#define VEX_OPS(ops_enum, def_decl_struct, def_decl_fns, num_ext, num_ops, ops_struct, magic_str, ...)\
	NV_PP_DEFER(_VEX_OPS(ops_enum, def_decl_struct, def_decl_fns, num_ext, num_ops, ops_struct, magic_str, __VA_ARGS__))

#define VEX_OPS_DECLARE_COLLECTION_ENTRY(_op, _op_struct) \
	[_op] = _op_struct

#define VEX_VERSION_ALL -1ULL
#define VEX_VERSION_NONE 0

#define VEX_UNUSED_OP(op, handle, vtag) \
static int op##_##handle##_##vtag(void *arg) { return -EPROTO; }

/*Allows for calling vex fns (encode, decode, etc.) with references to ops and fns for code navigation */
#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR(fn_suffix) \
	VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_##fn_suffix

#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_init							init
#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_encode						encode
#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_encode_container_elem		encode
#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_decode						decode
#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_decode_container				decode
#define VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR_fini							fini

/* TBD: Make these from other macros */
#define VERIFY_CALL_VEX_OP_EXT_LIST_BASE_ONLY(fn_ptr, ops, ext0, fn0, ...) \
({\
	VEX_BUILD_BUG_ON_MSG_STATIC(ARRAY_SIZE(ops) != 1, "Mismatch between num_ext and size of " NV_PP_STR(ops));\
	VEX_BUILD_BUG_ON_MSG(ops[0].vex_ext != vex_##ext0, NV_PP_STR(ops) "[0].vex_ext != vex_" NV_PP_STR(ext0));\
	VEX_BUILD_BUG_ON_MSG(ops[0].fn_ptr != fn0, NV_PP_STR(ops) "[0]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn0));\
})

#define VERIFY_CALL_VEX_OP_EXT_LIST_ONE_EXT(fn_ptr, ops, ext0, fn0, ext1, fn1, ...) \
({\
	VEX_BUILD_BUG_ON_MSG_STATIC(ARRAY_SIZE(ops) != 2, "Mismatch between num_ext and size of " NV_PP_STR(ops));\
	VEX_BUILD_BUG_ON_MSG(ops[0].vex_ext != vex_##ext0, NV_PP_STR(ops) "[0].vex_ext != vex_" NV_PP_STR(ext0));\
	VEX_BUILD_BUG_ON_MSG(ops[0].fn_ptr != fn0, NV_PP_STR(ops) "[0]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn0));\
	VEX_BUILD_BUG_ON_MSG(ops[1].vex_ext != vex_##ext1, NV_PP_STR(ops) "[1].vex_ext != vex_" NV_PP_STR(ext1));\
	VEX_BUILD_BUG_ON_MSG(ops[1].fn_ptr != fn1, NV_PP_STR(ops) "[1]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn1));\
})

#define VERIFY_CALL_VEX_OP_EXT_LIST_TWO_EXT(fn_ptr, ops, ext0, fn0, ext1, fn1, ext2, fn2, ...) \
({\
	VEX_BUILD_BUG_ON_MSG_STATIC(ARRAY_SIZE(ops) != 3, "Mismatch between num_ext and size of " NV_PP_STR(ops));\
	VEX_BUILD_BUG_ON_MSG(ops[0].vex_ext != vex_##ext0, NV_PP_STR(ops) "[0].vex_ext != vex_" NV_PP_STR(ext0));\
	VEX_BUILD_BUG_ON_MSG(ops[0].fn_ptr != fn0, NV_PP_STR(ops) "[0]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn0));\
	VEX_BUILD_BUG_ON_MSG(ops[1].vex_ext != vex_##ext1, NV_PP_STR(ops) "[1].vex_ext != vex_" NV_PP_STR(ext1));\
	VEX_BUILD_BUG_ON_MSG(ops[1].fn_ptr != fn1, NV_PP_STR(ops) "[1]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn1));\
	VEX_BUILD_BUG_ON_MSG(ops[2].vex_ext != vex_##ext2, NV_PP_STR(ops) "[2].vex_ext != vex_" NV_PP_STR(ext2));\
	VEX_BUILD_BUG_ON_MSG(ops[2].fn_ptr != fn2, NV_PP_STR(ops) "[2]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn2));\
})

#define VERIFY_CALL_VEX_OP_EXT_LIST_THREE_EXT(fn_ptr, ops, ext0, fn0, ext1, fn1, ext2, fn2, ext3, fn3, ...) \
({\
	VEX_BUILD_BUG_ON_MSG_STATIC(ARRAY_SIZE(ops) != 4, "Mismatch between num_ext and size of " NV_PP_STR(ops));\
	VEX_BUILD_BUG_ON_MSG(ops[0].vex_ext != vex_##ext0, NV_PP_STR(ops) "[0].vex_ext != vex_" NV_PP_STR(ext0));\
	VEX_BUILD_BUG_ON_MSG(ops[0].fn_ptr != fn0, NV_PP_STR(ops) "[0]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn0));\
	VEX_BUILD_BUG_ON_MSG(ops[1].vex_ext != vex_##ext1, NV_PP_STR(ops) "[1].vex_ext != vex_" NV_PP_STR(ext1));\
	VEX_BUILD_BUG_ON_MSG(ops[1].fn_ptr != fn1, NV_PP_STR(ops) "[1]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn1));\
	VEX_BUILD_BUG_ON_MSG(ops[2].vex_ext != vex_##ext2, NV_PP_STR(ops) "[2].vex_ext != vex_" NV_PP_STR(ext2));\
	VEX_BUILD_BUG_ON_MSG(ops[2].fn_ptr != fn2, NV_PP_STR(ops) "[2]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn2));\
	VEX_BUILD_BUG_ON_MSG(ops[3].vex_ext != vex_##ext3, NV_PP_STR(ops) "[3].vex_ext != vex_" NV_PP_STR(ext3));\
	VEX_BUILD_BUG_ON_MSG(ops[3].fn_ptr != fn3, NV_PP_STR(ops) "[3]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn3));\
})

#define VERIFY_CALL_VEX_OP_EXT_LIST_FOUR_EXT(fn_ptr, ops, ext0, fn0, ext1, fn1, ext2, fn2, ext3, fn3, ext4, fn4, ...) \
({\
	VEX_BUILD_BUG_ON_MSG_STATIC(ARRAY_SIZE(ops) != 5, "Mismatch between num_ext and size of " NV_PP_STR(ops));\
	VEX_BUILD_BUG_ON_MSG(ops[0].vex_ext != vex_##ext0, NV_PP_STR(ops) "[0].vex_ext != vex_" NV_PP_STR(ext0));\
	VEX_BUILD_BUG_ON_MSG(ops[0].fn_ptr != fn0, NV_PP_STR(ops) "[0]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn0));\
	VEX_BUILD_BUG_ON_MSG(ops[1].vex_ext != vex_##ext1, NV_PP_STR(ops) "[1].vex_ext != vex_" NV_PP_STR(ext1));\
	VEX_BUILD_BUG_ON_MSG(ops[1].fn_ptr != fn1, NV_PP_STR(ops) "[1]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn1));\
	VEX_BUILD_BUG_ON_MSG(ops[2].vex_ext != vex_##ext2, NV_PP_STR(ops) "[2].vex_ext != vex_" NV_PP_STR(ext2));\
	VEX_BUILD_BUG_ON_MSG(ops[2].fn_ptr != fn2, NV_PP_STR(ops) "[2]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn2));\
	VEX_BUILD_BUG_ON_MSG(ops[3].vex_ext != vex_##ext3, NV_PP_STR(ops) "[3].vex_ext != vex_" NV_PP_STR(ext3));\
	VEX_BUILD_BUG_ON_MSG(ops[3].fn_ptr != fn3, NV_PP_STR(ops) "[3]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn3));\
	VEX_BUILD_BUG_ON_MSG(ops[4].vex_ext != vex_##ext4, NV_PP_STR(ops) "[4].vex_ext != vex_" NV_PP_STR(ext4));\
	VEX_BUILD_BUG_ON_MSG(ops[4].fn_ptr != fn4, NV_PP_STR(ops) "[4]." NV_PP_STR(fn_ptr) " != " NV_PP_STR(fn4));\
})

#define VERIFY_CALL_VEX_OP_EXT_LIST(fn_suffix, ops, num_ext, ...) \
	NV_PP_PRIM_CAT(VERIFY_CALL_VEX_OP_EXT_LIST_,num_ext)(VERIFY_CALL_VEX_OP_EXT_LIST_FN_PTR(fn_suffix), ops, __VA_ARGS__)

#define CALL_VEX_OP_N_ARGS_EXT(num_ext) \
	CALL_VEX_OP_N_ARGS_EXT_##num_ext

#define CALL_VEX_OP_N_ARGS_EXT_BASE_ONLY	2
#define CALL_VEX_OP_N_ARGS_EXT_ONE_EXT		4
#define CALL_VEX_OP_N_ARGS_EXT_TWO_EXT		6
#define CALL_VEX_OP_N_ARGS_EXT_THREE_EXT	8
#define CALL_VEX_OP_N_ARGS_EXT_FOUR_EXT		10

/* Number of params for fn to CALL_VEX_OPS */
#define CALL_VEX_OPS_FN_N_PARAM(fn) NV_PP_PRIM_CAT(CALL_VEX_OPS_FN_N_PARAM_, fn)

#define CALL_VEX_OPS_FN_N_PARAM_init				2
#define CALL_VEX_OPS_FN_N_PARAM_encode				4
#define CALL_VEX_OPS_FN_N_PARAM_decode				4
#define CALL_VEX_OPS_FN_N_PARAM_fini				2

#define CALL_VEX_OPS_FN_N_PARAM_decode_container 		4
#define CALL_VEX_OPS_FN_N_PARAM_encode_container_elem 	4

/* Number of params for each ext to CALL_VEX_OPS */
#define CALL_VEX_OPS_NUM_EXT_N_PARAM(num_ext) NV_PP_PRIM_CAT(CALL_VEX_OPS_NUM_EXT_N_PARAM_, num_ext)

#define CALL_VEX_OPS_NUM_EXT_N_PARAM_BASE_ONLY	2
#define CALL_VEX_OPS_NUM_EXT_N_PARAM_ONE_EXT	4
#define CALL_VEX_OPS_NUM_EXT_N_PARAM_TWO_EXT	6
#define CALL_VEX_OPS_NUM_EXT_N_PARAM_THREE_EXT	8
#define CALL_VEX_OPS_NUM_EXT_N_PARAM_FOUR_EXT	10
/* ... */

#define CALL_VEX_OP_N_ARGS(fn_suffix, num_ext) \
	NV_PP_ADD(CALL_VEX_OPS_FN_N_PARAM_##fn_suffix, CALL_VEX_OPS_NUM_EXT_N_PARAM_##num_ext)

#define CALL_VEX_OP(fn_suffix, ext_ops, num_ext, ...) \
	NV_PP_IF_ELSE_VARGS(NV_PP_LIST_SZ_LT(CALL_VEX_OP_N_ARGS(fn_suffix, num_ext), __VA_ARGS__),\
		CALL_VEX_OP_NARGS_ERR_MSG, _CALL_VEX_OP, fn_suffix, ext_ops, num_ext, __VA_ARGS__)

#define CALL_VEX_OP_NARGS_ERR_MSG(fn_suffix, ext_ops, num_ext, ...) \
	NV_PP_ERR(CALL_VEX_OP - fn_suffix - num_ext - Invalid number of arguments - __VA_ARGS__)

#define _CALL_VEX_OP(fn_suffix, ext_ops, num_ext, ...) \
({\
	int ret; \
	VERIFY_CALL_VEX_OP_EXT_LIST(fn_suffix, ext_ops, num_ext, NV_PP_TAKE(CALL_VEX_OPS_NUM_EXT_N_PARAM_##num_ext, __VA_ARGS__)); \
	ret = vex_##fn_suffix(NV_PP_TAKE(CALL_VEX_OPS_FN_N_PARAM_##fn_suffix, \
		NV_PP_EAT(CALL_VEX_OPS_NUM_EXT_N_PARAM_##num_ext, __VA_ARGS__)));\
	ret; \
})

#ifdef VEX_DEBUG
#	ifndef LLVM
#		pragma GCC pop_options
#	endif
#endif

#pragma pop_macro("__FILE_LITERAL__")

#endif /* NVMEIB_VERSION_H */
