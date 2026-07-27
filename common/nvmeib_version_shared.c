#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeib_msgs_shared.h"
#include "nvmeib_version_shared.h"

#ifdef TRACE_INCLUDE_FILE
	/* TRACE_INCLUDE_FILE should be defined in Makefile of eac*h one of the modules that includes this file */
	#include TRACE_INCLUDE_FILE
#else
	#error "Tracing not not supported, fix compilation or use kr_incs_dummy_empty_traces.h"
#endif


#define NVMEIB_PRODUCT_MAJOR		2
#define NVMEIB_PRODUCT_MINOR		0
#define NVMEIB_PRODUCT_SUBMINOR 	0
#define NVMEIB_PRODUCT_MR		0

#define NVMEIB_PROTOCOL_MAJOR		1
#define NVMEIB_PROTOCOL_MINOR		1
#define NVMEIB_PROTOCOL_SUBMINOR	0
#define NVMEIB_PROTOCOL_MR		0

union nvmeib_version this_version = NVMEIB_CURRENT_VERSION_INIT;

union nvmeib_version nvmeib_version_get(void)
{
	return this_version;
}
EXPORT_SYMBOL(nvmeib_version_get);

bool debug_dump_funcs = false;

VEX_OPS_DECLARE_OP_FN(init, static, vex_invalid_init_op)
{
	(void)arg;
	return -EOPNOTSUPP;
}

VEX_OPS_DECLARE_OP_FN(encode, static, vex_invalid_encode_op)
{
	(void)arg;
	return -EOPNOTSUPP;
}

VEX_OPS_DECLARE_OP_FN(decode, static, vex_invalid_decode_op)
{
	(void)arg;
	return -EOPNOTSUPP;
}

VEX_OPS_DECLARE_OP_FN(fini, static, vex_invalid_fini_op)
{
	(void)arg;
	return -EOPNOTSUPP;
}

const struct vex_ops nvmeib_vex_invalid_ops = {
	.version = NVMEIB_INVALID_VERSION_INIT,
	.vex_ext = vex_invalid,
	.vex_max_ext = vex_invalid,
	.init = vex_invalid_init_op,
	.encode = vex_invalid_encode_op,
	.decode = vex_invalid_decode_op,
	.fini = vex_invalid_fini_op
};
EXPORT_SYMBOL(nvmeib_vex_invalid_ops);


int nvmeib_container_validate(const struct nvmeib_container *c,
	__be64 contents_magic, u16 version_tag)
{
	int rv;
	size_t c_size, elem_stride;
	int n_elem;
	if (c->magic != NVMEIB_CONTAINER_MAGIC) {
		_NE(nvmeib_container_validate_e1,
			"invalid container magic: @STR_8 (@INT_XULLONG_8) at @PTR",
		   (const char *)&c->magic, c->magic, c);
		rv = -EPROTO;
		goto out;
	}
	if (c->contents_magic == NVMEIB_EMPTY_MAGIC) {
		_NT(nvmeib_container_validate_t1, "empty container at @PTR", c);
		rv = 0;
		goto out;
	}
	if (c->contents_magic != contents_magic) {
		_NE(nvmeib_container_validate_e2,
			"unexpected container contents magic: @STR_8 (@INT_XULLONG_8) "
		   "at @PTR", (const char *)&c->contents_magic, c->contents_magic, c);
		rv = -EPROTO;
		goto out;
	}
	if (cpu_to_be16(c->version_tag) != version_tag) {
		_NE(nvmeib_container_validate_e3,
			"invalid container version tag @UINT for contents magic: @STR_8 "
		    "(@INT_XULLONG_8) at @PTR", cpu_to_be16(c->version_tag),
			(const char *)&c->contents_magic, c->contents_magic, c);
		rv = -EPROTO;
		goto out;
	}
	c_size = be32_to_cpu(c->size);
	elem_stride = be32_to_cpu(c->elem_stride);
	n_elem = be16_to_cpu(c->n_elem);
	if (c_size < sizeof(*c)) {
		_NE(nvmeib_container_validate_e4,
			"invalid container size @BUFF_SIZE\n", c_size);
		rv = -EPROTO;
		goto out;
	}
	if (elem_stride != 0 &&
		sizeof(*c) + elem_stride * n_elem != c_size) {
		_NE(nvmeib_container_validate_e5,
			"container size: @BUFF_SIZE, stride: @BUFF_SIZE, "
			"n_elem: @INT mismatch", c_size, elem_stride, n_elem);
		rv = -EPROTO;
		goto out;
	}
	rv = n_elem;
out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_container_validate);

int nvmeib_container_elem_validate(const struct nvmeib_container *c,
	const struct nvmeib_container_elem *elem,
	unsigned idx, size_t min_payload_sz)
{
	int rv;
	unsigned elem_num = be16_to_cpu(c->n_elem);
	size_t elem_sz = be32_to_cpu(elem->size);
	size_t c_size = be32_to_cpu(c->size);
	if (elem->magic != NVMEIB_CONTAINER_ELEMENT_MAGIC) {
		_NE(nvmeib_container_elem_validate_e1,
			"invalid container elem magic: @STR_8 (@INT_XULLONG_8) at @PTR "
			"(container offset @BUFF_SIZE)",
			(const char *)&elem->magic, elem->magic, elem,
			(const void *)elem - (const void *)c);
		rv = -EPROTO;
		goto out;
	}
	if (elem->idx != cpu_to_be16(idx) || idx >= elem_num) {
		_NE(nvmeib_container_elem_validate_e2,
			"invalid container elem index @UINT (not @UINT/@UINT)",
		   be16_to_cpu(elem->idx), idx, elem_num);
		rv = -EPROTO;
		goto out;
	}
	if ((const void *)elem + elem_sz > (const void *)c + c_size) {
		_NE(nvmeib_container_elem_validate_e3,
			"elem @INT at @PTR (size @BUFF_SIZE)  "
			"does not fit in container mem range @PTR - @PTR\n",
			idx, elem, elem_sz, c, (const void *)c + c_size);
		rv = -EPROTO;
		goto out;
	}
	if (min_payload_sz && elem_sz < min_payload_sz + sizeof(*elem)) {
		_NE(nvmeib_container_elem_validate_e4,
			"elem @INT at @PTR (payload size @BUFF_SIZE) is smaller than "
			"minimum payload size @BUFF_SIZE",
			idx, elem, elem_sz - sizeof(*elem), min_payload_sz);
		rv = -EPROTO;
		goto out;
	}
	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(nvmeib_container_elem_validate);

int vex_encode_alloc_dref(void **alloc_ptr, struct vex_encode_dref_alloc_ctx* dref_alloc_ctx,
	size_t size, __be64 contents_magic, void *dref_ptr)
{
	ssize_t rv;
	size_t alloc_sz;
	struct vex_dref dref;
	if (dref_alloc_ctx->dref_base == NULL) {
		/* Calculate dref_base, dref_end using vex_size */
		if ((rv = vex_size(dref_alloc_ctx->link_ext_ops, dref_alloc_ctx->arg, false)) < 0) {
			goto out;
		}
		dref_alloc_ctx->dref_base = dref_alloc_ctx->out + ALIGN(rv, sizeof(u64));
		dref_alloc_ctx->dref_curr = dref_alloc_ctx->dref_base;
	}
	/* Verify some things */
	BUG_ON(dref_ptr < dref_alloc_ctx->out || dref_ptr >= dref_alloc_ctx->dref_base);
	BUG_ON(dref_alloc_ctx->dref_curr < dref_alloc_ctx->dref_base);
	BUG_ON(dref_alloc_ctx->dref_curr >= dref_alloc_ctx->out_end);
	BUG_ON(!IS_ALIGNED((unsigned long)dref_alloc_ctx->dref_curr, sizeof(u64)));

	/* Align size */
	alloc_sz = ALIGN(size, sizeof(u64));

	/* Try and allocate memory from dref_curr until out_end */
	if (dref_alloc_ctx->dref_curr + alloc_sz > dref_alloc_ctx->out_end) {
		_NE(vex_encode_alloc_dref_e1, "Out of memory trying to allocate "
			"@BUFF_SIZE bytes for dref object", alloc_sz);
		rv = -ENOMEM;
		goto out;
	}

	*alloc_ptr = dref_alloc_ctx->dref_curr;

	/* Initialise struct on the stack and then memcpy to msg
	 * (Solves alignment warnings) */
	dref.offset = cpu_to_be32(dref_alloc_ctx->dref_curr - dref_ptr);
	dref.size = cpu_to_be32(size);
	dref.contents_magic = contents_magic;
	dref.version_tag = cpu_to_be16(dref_alloc_ctx->link_ext_ops->vex_ext);
	vex_memcpy(dref_ptr, &dref, sizeof(dref));
	dref_alloc_ctx->dref_curr += alloc_sz;

	rv = 0;

out:
	return rv;
}
EXPORT_SYMBOL(vex_encode_alloc_dref);

ssize_t vex_decode_container(const struct vex_ops* link_ext_ops,
	const void* in, const void* in_end, void* arg)
{
	const struct nvmeib_container* c = in;
	const struct nvmeib_container_elem* elem;
	int i, n_elem;
	ssize_t rv = -EPROTO;
	const void* p_in = in;
	size_t elem_stride;

	if ((const void *)(c + 1) > in_end) {
		_NT(vex_decode_container_t1,
			"Out of space processing container header at @PTR", in);
		rv = -ENOSPC;
		goto out;
	}

	if ((rv = nvmeib_container_validate(c, link_ext_ops->magic_val,
										link_ext_ops->vex_ext)) < 0) {
		_NE(vex_decode_container_e1,
			"Invalid container hdr magic @STR_8 (@INT_XULLONG_8) "
			"contents magic @STR_8 (@INT_XULLONG_8) (not @STR_8) version "
			"@INT32_HEX (not @INT32_HEX)",
		   (const char *)&c->magic, c->magic,
		   (const char *)&c->contents_magic, c->contents_magic,
		   link_ext_ops->magic_str, be16_to_cpu(c->version_tag), link_ext_ops->vex_ext);
		goto out;
	}

	if (NVMEIB_CONT_IS_EMPTY(c)) {
		rv = -ENOENT;
		goto out;
	}

	n_elem = rv;

	if (n_elem == 0) {
		/* Single entry container */
		size_t ctnr_payload_sz;
		if ((rv = vex_decode_elem(link_ext_ops, NVMEIB_CONT_CONST_PAYLOAD(c),
								  in_end, 0, 1, arg)) < 0) goto out;
		if ((size_t)rv != (ctnr_payload_sz = nvmeib_container_payload_size(c))) {
			_NE(vex_decode_container_e2, "Mismatch between payload size "
				"@BUFF_SIZE and decode size @BUFF_SIZE", ctnr_payload_sz, rv);
			rv = -EPROTO;
			goto out;
		}
		rv += sizeof(*c);
		goto out;
	}

	elem_stride = nvmeib_container_elem_stride(c);

	_NT(vex_decode_container_t2,
		"Decoding @STR @STR_8 container (size @UINT) with @INT elements",
		c->elem_stride ? "const-stride" : "variable-stride",
		(const char *)&c->contents_magic,
	   be32_to_cpu(c->size), n_elem);

	if (elem_stride) {
		/* Constant-stride container */
		for (i = 0, p_in = NVMEIB_CONT_CONST_PAYLOAD(c); i < n_elem;
			 i++, p_in += elem_stride) {
			if ((rv = vex_decode_elem(link_ext_ops, p_in, p_in + elem_stride, i, n_elem, arg)) < 0) goto out;
			if ((size_t)rv != elem_stride) {
				_NE(vex_decode_container_e3, "Mismatch between element size "
					"@BUFF_SIZE and decode size @BUF_SIZE", elem_stride, rv);
				rv = -EPROTO;
				goto out;
			}
		}
		rv = p_in - in;
		goto out;
	}

	/* Variable-stride container */
	for_each_nvmeib_container_elem(c, elem, i, p_in, rv) {
		size_t elem_sz = nvmeib_container_elem_size(elem);
		_NT(vex_decode_container_t3, "Decoding @STR_8 container elem @INT "
			"payload at container offset @BUFF_SIZE",
			(const char *)&c->contents_magic, i, p_in - (const void *)c);
		if ((rv = vex_decode_elem(link_ext_ops, p_in, (const void *)elem + elem_sz, i, n_elem, arg)) < 0) {
			goto out;
		}
		if (rv + sizeof(*elem) != elem_sz) {
			_NE(vex_decode_container_e4, "Mismatch between element size "
				"@BUFF_SIZE and decode size @BUF_SIZE", elem_sz, rv);
			rv = -EPROTO;
			goto out;
		}
	}
	rv = nvmeib_container_size(c);

out:
	return rv;
}
EXPORT_SYMBOL(vex_decode_container);

void vex_link_ops(union nvmeib_version link_ver,
		const struct vex_ops * const *vex_ops_collection, const struct vex_ops **link_ops,
		const char *(*vex_op_name)(int),
		int ops_collection_size)
{
	int op;
	for (op = 0; op < ops_collection_size; op++) {
		link_ops[op] = vex_select_ops(link_ver, vex_ops_collection[op]);
		_NT(_t1, "vex op=@STR vex link ext=@INT",
			vex_op_name(op), link_ops[op]->vex_ext);
	}
}
EXPORT_SYMBOL(vex_link_ops);

