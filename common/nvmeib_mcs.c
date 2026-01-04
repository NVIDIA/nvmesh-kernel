#include "nvmeib_mcs.h"
#include "nvmeib_mcs_header.h"
#include "nvmeib_mcs_msg_cache.h"
#include "common/compat/kr_incs_compiler_types.h"
#ifndef USER_SPACE
	#include "nvmeib.h"
	#include "nvmeib_utils.h"
	#include "nvmeib_msgloop.h"
	#include "nvmeib_public.h"
	#include "nvmeibm_trace.h"
	#define GENERIC_ZALLOC kzalloc
	#define GENERIC_ZALLOC_SIM sim_kzalloc
	#define GENERIC_FREE kfree
#else
	#ifdef TOMA		// implementation of kernel functions in Toma
		#define BUG_ON(cond)		// Wrong definition of WARN() in toma causing wrong BUG_ON()
		#include "toma/nvmeibt_common.h"
		#include "toma/nvmeibt_bm.h"
		#define GENERIC_ZALLOC(a,b) NNVMEIBT_BM_CALLOC(__LINE__, a)
		#define GENERIC_FREE(a)     NNVMEIBT_BM_FREE(__LINE__, a)
		#define _NT(name, fmt, ...) N_Tf(name ## _toma, fmt, ##__VA_ARGS__)
		#define _NW(name, fmt, ...) N_Wf(name ## _toma, fmt, ##__VA_ARGS__)
		#define _NE(name, fmt, ...) N_Ef(name ## _toma, fmt, ##__VA_ARGS__)
		#define _ND(name, fmt, ...) N_Df(name ## _toma, fmt, ##__VA_ARGS__)
	#else		/* UM_APP: Used by nvmeshum's client control manager for mcs protocol*/
		#define NFIN    FFIN
		#define NFOUT   FFOUT
		#define GENERIC_ZALLOC(a,b) calloc(1, (a))
		#define GENERIC_FREE(a)     free(a)
	#endif
	#include "compat/kr_incs_types.h"
	#include "compat/kr_incs_time_rdtsc.h"
#endif

//initialize message loop
int nvmeib_mcs_init(struct mcs_info *mcs_info, unsigned int scheme_version, int num_msg, int num_offs,
	const int *_incr_offs, const int *_incr_types,
	const int *_opcodes_to_incr_id_upstr, const int *_opcodes_to_incr_id_downstr,
	const size_t *_sizeof_items, size_t max_msg_id)
{
	int ret = 0;

	mcs_info->mcs_num_msg = num_msg;
	mcs_info->mcs_num_offs = num_offs;
	mcs_info->mcs_incr_offs = _incr_offs;
	mcs_info->mcs_incr_types = _incr_types;
	mcs_info->mcs_opcodes_to_incr_id_upstr = _opcodes_to_incr_id_upstr;
	mcs_info->mcs_opcodes_to_incr_id_downstream = _opcodes_to_incr_id_downstr;
	mcs_info->mcs_header_version = MCS_HEADER_VERSION;
	mcs_info->mcs_scheme_version = scheme_version;
	mcs_info->mcs_sizeof_items = _sizeof_items;
	mcs_info->mcs_max_msg_id = max_msg_id;
	mcs_info->cache = NULL;
#ifndef USER_SPACE
	mcs_info->cache = nvmeib_mcs_msg_cache_alloc(msgloop_get_msg, msgloop_put_msg);
	if (!mcs_info->cache) {
		_NE(mcs_init_bad_alloc, "could not allocate memory for mcs cache");
		ret = -ENOMEM;
	}
#endif
	// _NE("info=@PTR,_id_downstream=@PTR\n", mcs_info,	mcs_info->mcs_opcodes_to_incr_id_downstream);
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_init);

void *nvmeib_mcs_allocate_and_init(unsigned int scheme_version, int num_msg, int num_offs,
	const int *_incr_offs, const int *_incr_types,
	const int *_opcodes_to_incr_id_upstr, const int *_opcodes_to_incr_id_downstr,
	const size_t *_sizeof_items, size_t max_msg_id)
{
	struct mcs_info *mcs_info = NULL;

	NFIN;

	mcs_info = GENERIC_ZALLOC(sizeof(*mcs_info), GFP_KERNEL);
	if (!mcs_info) {
	    _NE(error_nvmeib_mcs_nvmeib_mcs_init, "memory allocation problem when trying to allocate mcs_info");
	    goto err;
	}

	if (nvmeib_mcs_init(mcs_info, scheme_version, num_msg, num_offs,
			    _incr_offs, _incr_types, _opcodes_to_incr_id_upstr,
			    _opcodes_to_incr_id_downstr, _sizeof_items,
			    max_msg_id))
		goto err;

	goto out;

err:
	if (mcs_info)
	    GENERIC_FREE(mcs_info);
	mcs_info = NULL;
out:
    NFOUT;
    return mcs_info;
}
EXPORT_SYMBOL(nvmeib_mcs_allocate_and_init);

void nvmeib_mcs_remove(void *desc)
{
	struct mcs_info *mcs_info = (struct mcs_info *)desc;

	NFIN;
	GENERIC_FREE(mcs_info->cache);
	GENERIC_FREE(mcs_info);

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_mcs_remove);

static int __attr_no_alignment_sanity incarnate_item(int max_offset, struct mcs_info *info,
	void *item, int num_items, void *var_data, int indx)
{
	int rv = 0;
	int ioffs, item_i;
	const int *offs = info->mcs_incr_offs + indx * info->mcs_num_offs;
	const int *sub_types = info->mcs_incr_types + indx * info->mcs_num_offs;
	int soffs;
	unsigned long long offs_to_new_type;
	int new_type ,num_of_new_types;
	void *next_item, *arr_ptr;
	int offset_to_next_end = 0;
	size_t sizeof_item = info->mcs_sizeof_items[indx];

	NFIN;
	if (num_items <= 0)
		goto out;
	_ND(trace_nvmeib_mcs_incarnate_item, "incr_offst[0]=@IOFFS", info->mcs_incr_offs[0]);
	_ND(trace_1_nvmeib_mcs_incarnate_item, "indx=@INDX num_items=@NUM_ITEMS info->mcs_num_msg=@MCS_NUM_MSG offs[0]=@IOFFS", indx,
	   num_items, info->mcs_num_msg, offs[0]);
	if (0) {
		_ND(trace_2_nvmeib_mcs_incarnate_item, "#@NUM_ITEMS x size=@SIZE, item_offset=@ITEM_OFFSET, VD=@VAR_DATA Relative:@VAR_DATA, indx=@INDX", num_items, (int)info->mcs_sizeof_items[indx], item, var_data, (var_data) ? var_data : 0, indx);
	}
	for (item_i = 0; item_i < num_items; ++item_i)
	{
		void **this_item = (void **)((char *)item + item_i * sizeof_item);
		if (!var_data)
			var_data = (void *)((char *)this_item + sizeof_item);
		for (ioffs = 0; ioffs < info->mcs_num_offs; ++ioffs)
		{
			soffs = offs[ioffs];
			if (soffs < 0)
				break;
			new_type = sub_types[ioffs];
			num_of_new_types = *(int *)((char *)this_item + soffs - sizeof(int));

			if (!num_of_new_types) {
				arr_ptr = (char *)this_item + soffs;
				*(unsigned long long *)(arr_ptr) = 0;
				continue;
			}
			offs_to_new_type = *(unsigned long long *)((char *)this_item + soffs);
			offset_to_next_end = offs_to_new_type +
				info->mcs_sizeof_items[new_type] * num_of_new_types;
			if (offset_to_next_end > max_offset) {
				_NE(error_nvmeib_mcs_incarnate_item, "offset @OFFS_TO_NEW_TYPE is out of bounds (max @MAX_OFFSET)", offs_to_new_type,
					max_offset);
					rv = -EINVAL;
					goto out;
			}

			next_item = var_data + offs_to_new_type;
			_ND(trace_3_nvmeib_mcs_incarnate_item, "new_type:@NEW_TYPE,offs_to_new_type:@OFFS_TO_NEW_TYPE,num_of_new_types:@NUM_OF_NEW_TYPES",
				new_type, offs_to_new_type, num_of_new_types);

			if ((rv = incarnate_item(max_offset, info, next_item, num_of_new_types,
				 var_data, new_type)) < 0)
				 goto out;

			arr_ptr = (char *)this_item + soffs;
			if (num_of_new_types > 0)
				*(unsigned long long *)(arr_ptr) = (u64)next_item;
		}
	}

out:
	NFOUT;
	return rv;
}

//return a pointer to the buffer to send
void *nvmeib_mcs_get_msg_data(void *msg_in)
{
	struct mcs_message *msg = container_of(msg_in, struct mcs_message, msg);
	return msg;
}
EXPORT_SYMBOL(nvmeib_mcs_get_msg_data);

//return the total length of the buffer to send
int nvmeib_mcs_get_msg_len(void *msg_in)
{
	struct mcs_message *msg = container_of(msg_in, struct mcs_message, msg);
	return msg->header.msg_len.msg_len;
}
EXPORT_SYMBOL(nvmeib_mcs_get_msg_len);

static int validate_opcode_for_incarnation(struct mcs_info *info, int opcode)
{
	int rv = 0; //OK value

	NFIN;
	if (opcode == 80){
		rv = 0; //0xb10bb10b
	} else if (opcode >= info->mcs_max_msg_id ||
		info->mcs_opcodes_to_incr_id_downstream[opcode] < 0) {
		rv = -1;
		_NE(error_nvmeib_mcs_validate_opcode_for_incarnation, "invalid opcode @OPCODE", opcode);
	}

	NFOUT;
	return rv;
}

//go over all offsets in message and replace them with pointers
static int incarnate_message(struct mcs_info *info, struct mcs_message *msg)
{
    int rv = 0;
    int opcode = msg->header.opcode;
    int msg_indx;

	NFIN;
	if (validate_opcode_for_incarnation(info, opcode) < 0) {
		_NE(error_nvmeib_mcs_incarnate_message, "Wrong opcode=@OPCODE", opcode);
		rv = -2;
		goto out;
	}
	_NT(trace_nvmeib_mcs_incarnate_message, "Opcode=@OPCODE,info=@INFO_PTR,_id_downstream=@_ID_DOWNSTREAM",opcode,
		info, info->mcs_opcodes_to_incr_id_downstream);
	if (opcode == 80){ //0xb10bb10b
		msg_indx = -1;
	} else {
		msg_indx = info->mcs_opcodes_to_incr_id_downstream[opcode];
	}

	if (info->mcs_header_version != (int)msg->header.header_version) {
		_NE(error_1_nvmeib_mcs_incarnate_message, "cannot incarnate message @OPCODE, message header version (@HEADER_VERSION) does"
		" not match module header version @MCS_HEADER_VERSION",
			opcode, msg->header.header_version,
			info->mcs_header_version);
		rv = -1;
		goto out;
	}
	if (info->mcs_scheme_version != (int)msg->header.scheme_version) {
		_NE(error_2_nvmeib_mcs_incarnate_message, "cannot incarnate message @OPCODE, message scheme version (@SCHEME_VERSION) does"
			" not match module scheme version @MCS_SCHEME_VERSION", opcode,
			 msg->header.scheme_version, info->mcs_scheme_version);
		rv = -1;
		goto out;
	}
	if (msg_indx < 0)
		_NT(trace_1_nvmeib_mcs_incarnate_message, "Message does not have any dynamic pointers");
	else {
		int message_size = msg->header.msg_len.msg_len;
		int max_offset = message_size - sizeof(msg->header) -
			info->mcs_sizeof_items[msg_indx];
		_ND(trace_2_nvmeib_mcs_incarnate_message, "msg_indx=@MSG_INDX", msg_indx);
		if ((rv =
			incarnate_item(max_offset, info, &msg->msg, 1, NULL, msg_indx)) < 0) {
				_NE(error_3_nvmeib_mcs_incarnate_message, "failed to incarnate item");
				goto out;
			}
	}

out:
	NFOUT;
	return rv;
}

/*an instruction to send an item with a given type amount of items and location*/
struct send_instr {
	/*the location in memory where the items exist*/
	void *ptr;
	/*the type of item*/
	int type;
	/*amount of items in this instruction*/
	int size;
	/*a pointer to the next item*/
	struct send_instr *next;
};

/*manages a list of instructions*/
struct inst_list {
	struct send_instr *first;
	struct send_instr *last;
};

/*send function*/
static int __attr_no_alignment_sanity send_items(struct mcs_info *info, int indx, void *buf,
	int *var_offs, send_cb send_tool, void *context)
{

	struct send_instr *inst = NULL;
	int rv = 0;
	const int *incr_type;
	const int *offs_arr;
	int i, j, msg_size, ptrs_indx = 0;
	int curr_offs, num_items2, next_type;
	void **ptrs = (void **)NULL;
	void **buf2_ptr;
	struct send_instr *new_inst;
	int sizeof_toalloc = 32, to_alloc;
	int var_offs_, base_offs = 0;
	struct inst_list list;

	NFIN;
	memset(&list, 0, sizeof(list));
	*var_offs = 0;
	//assume that will not need more than 32 pointers
	to_alloc = info->mcs_num_offs * sizeof(*ptrs) * sizeof_toalloc;
	ptrs = (void **)GENERIC_ZALLOC(to_alloc, GFP_KERNEL);
	if (!ptrs) {
		_NE(error_nvmeib_mcs_send_items, "cannot allocate memory in send to mcs");
		rv = -ENOMEM;
		goto out;
	}
	list.first = (struct send_instr *)GENERIC_ZALLOC(sizeof(*list.first), GFP_KERNEL);
	if (!list.first) {
		_NE(error_1_nvmeib_mcs_send_items, "Memory allocation failure");
		goto out;
	}
	list.last = list.first;
	list.first->ptr = buf;
	list.first->size = 1;
	list.first->type = indx;
	list.first->next = (struct send_instr *)NULL;
	base_offs = info->mcs_sizeof_items[list.first->type];
	var_offs_ = base_offs;
	while (list.first) {
		inst = list.first;
		list.first = list.first->next;
		if (list.last == inst)
			list.last = (struct send_instr*)NULL;
		if (inst->size == 0) {
			GENERIC_FREE(inst);
			inst = (struct send_instr *)NULL;
			continue;
		}
		if (info->mcs_num_offs > 0 && inst->size > sizeof_toalloc) {
			GENERIC_FREE(ptrs);
			sizeof_toalloc = inst->size;
			to_alloc = info->mcs_num_offs * sizeof(*ptrs) * inst->size;
			ptrs = (void **)GENERIC_ZALLOC(to_alloc, GFP_KERNEL);
			if (!ptrs) {
				_NE(error_2_nvmeib_mcs_send_items, "cannot allocate memory in send to mcs");
				rv = -ENOMEM;
				goto out;
			}
		}

		incr_type =  (info->mcs_incr_types + inst->type * info->mcs_num_offs);
		offs_arr = (info->mcs_incr_offs + inst->type * info->mcs_num_offs);
		msg_size = info->mcs_sizeof_items[inst->type];

		//calculate the offset to the arrays of this item

		ptrs_indx = 0;
		for (j = 0; j < inst->size; ++j) {
			for (i = 0; i < info->mcs_num_offs; ++i) {

				if (incr_type[i] < 0)
					break;
				curr_offs = offs_arr[i];
				buf2_ptr = (void **)((char *)inst->ptr + curr_offs + j * msg_size);
				num_items2 = *(int *)((char *)buf2_ptr - 4);
				ptrs[ptrs_indx++] = *buf2_ptr;
				*(unsigned long long *)buf2_ptr = var_offs_ - base_offs;
				next_type = incr_type[i];
				var_offs_ += (num_items2 * info->mcs_sizeof_items[next_type]);
				new_inst = (struct send_instr *)GENERIC_ZALLOC(sizeof(*new_inst), GFP_KERNEL);
				if (!new_inst) {
					_NE(error_3_nvmeib_mcs_send_items, "Memory allocation problem");
					goto out;
				}
				new_inst->ptr = ptrs[ptrs_indx - 1];
				new_inst->size = num_items2;
				new_inst->next = (struct send_instr *)NULL;
				new_inst->type = next_type;
				if (list.last) {
					BUG_ON(list.last->next);
					list.last->next = new_inst;
				}
				else {
					BUG_ON(list.first);
					list.first = new_inst;
				}
				list.last = new_inst;
			}
		}
		if (send_tool && (rv =
			send_tool(context, inst->ptr, info->mcs_sizeof_items[inst->type] * inst->size)) < 0) {
			_NE(error_4_nvmeib_mcs_send_items, "could not send message index @INST_TYPE top to the other side. ret = @RV",
				inst->type, rv);
			goto out;
		}
		//Now, restore ptrs
		ptrs_indx = 0;
		for (j = 0; j < inst->size; ++j)
			for (i = 0; i < info->mcs_num_offs; ++i)
			{
				if (incr_type[i] < 0)
					break;
				curr_offs = offs_arr[i];
				buf2_ptr = (void **)((char *)inst->ptr + curr_offs + j * msg_size);
				*buf2_ptr = ptrs[ptrs_indx++];
			}
		*var_offs += info->mcs_sizeof_items[inst->type] * inst->size;
		GENERIC_FREE(inst);
		inst = (struct send_instr *)NULL;
	}


out:
	if (inst)
		GENERIC_FREE(inst);
	if (ptrs)
		GENERIC_FREE(ptrs);
	while (list.first) {
		struct send_instr *that = list.first;
		list.first = list.first->next;
		GENERIC_FREE(that);
	}
	list.last = (struct send_instr *)NULL;
	NFOUT;
	return rv;
}

static void init_mcs_header(struct header *hdr, struct mcs_info *info, int indx,
			    int msg_size, int opcode,
			    const unsigned char token[16])
{
	memset(hdr, 0, sizeof(*hdr));
	hdr->msg_len.msg_len = msg_size;
	hdr->msg_len.var_offset = msg_size - (int)(sizeof(struct header)) -
		info->mcs_sizeof_items[indx];
	hdr->header_version = info->mcs_header_version;
	hdr->scheme_version = info->mcs_scheme_version;
	hdr->opcode = opcode;
	hdr->timestamp = nvmeib_public_rdtsc();
	if (token)
		memcpy(hdr->token, token, sizeof(hdr->token));
}

//sends a message
int nvmeib_mcs_send(struct mcs_info *info, int opcode, void *buf,
		    const unsigned char token[16], send_cb send_tool, void *context)
{
	int rv = 0;
	int indx = info->mcs_opcodes_to_incr_id_upstr[opcode];
	int msg_size = 0;
	struct header hdr;
	int var_offs = 0;

	NFIN;
	//use send item to calculate the size of the message, not including
	// header
	send_items(info, indx, buf, &msg_size, (send_cb)NULL, NULL);

	//send_item(info, indx, buf, 1, &msg_size, NULL, NULL);
	//msg_size += info->mcs_sizeof_items[indx];
	msg_size += (int)(sizeof(struct header));
	_ND(trace_nvmeib_mcs_nvmeib_mcs_send, "Total message size is @MSG_SIZE", msg_size);
	if ((rv = send_tool(context, &msg_size, sizeof(msg_size))) < 0) {
		_NE(error_nvmeib_mcs_nvmeib_mcs_send, "could not send message size to the other side. ret = @RV", rv);
		goto out;
	}

	init_mcs_header(&hdr, info, indx, msg_size, opcode, token);

	if ((rv = send_tool(context, &hdr, sizeof(hdr))) < 0) {
		_NE(error_1_nvmeib_mcs_nvmeib_mcs_send, "could not send message header. ret = @RV", rv);
		goto out;
	}

	rv = send_items(info, indx, buf, &var_offs, send_tool, context);
	if (rv < 0) {
		_NE(error_2_nvmeib_mcs_nvmeib_mcs_send, "Error while sending msg of opcode @OPCODE", opcode);
	}

out:
//	if (ptrs)
//		GENERIC_FREE(ptrs);

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_mcs_send);

int nvmeib_mcs_build_msg(struct mcs_info *info, int opcode, void *msg_to_send,
			 unsigned char token[16], build_cb bcb, void *context)
{
	return nvmeib_mcs_send(info, opcode, msg_to_send, token, bcb, context);
}
EXPORT_SYMBOL(nvmeib_mcs_build_msg);

static void prepare_msg_to_send(struct mcs_info *info, struct mcs_message *msg)
{
	int opcode = msg->header.opcode;
	int ioffs, offs;
	int indx = info->mcs_opcodes_to_incr_id_upstr[opcode];
	size_t net_message_size = info->mcs_sizeof_items[indx];
	void *msg_v = (void *)(&msg->msg);
	void **member_p;
	int *target, *typeid;
	const int *rt;
	void *payload = (void *)&msg->msg + net_message_size;

	NFIN;
	_NT(trace_nvmeib_mcs_prepare_msg_to_send, "Prepagin message @OPCODE", opcode);
	if (indx < 0) {
		_NT(trace_1_nvmeib_mcs_prepare_msg_to_send, "Message does not have dynamic arrays, nothing to fix");
		goto out;
	}
	rt = (info->mcs_incr_offs + indx * info->mcs_num_offs);
	for (ioffs = 0; ioffs < info->mcs_num_msg; ++ioffs) {
	    offs = rt[ioffs];
	    if (offs < 0)
			break;
	    member_p = msg_v + offs;
		typeid = (int *)member_p;
	    target = typeid + 1;
	    *target = *member_p - payload;
		*typeid = 0;
	    _NT(trace_2_nvmeib_mcs_prepare_msg_to_send, "The calculated offset(@IOFFS) is @IO_OFFSET", ioffs, *target);
	}

out:
	NFOUT;
}

int nvmeib_mcs_prepare_to_send(void *_info, void *msg_in)
{
	struct mcs_info *info = (struct mcs_info *)_info;
	int rv = 0;
	struct mcs_message *msg = container_of(msg_in, struct mcs_message, msg);

	NFIN;
	prepare_msg_to_send(info, msg);
	msg->header.header_version = info->mcs_header_version;
	msg->header.scheme_version = info->mcs_scheme_version;
	msg->header.timestamp = nvmeib_public_rdtsc();

	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_mcs_prepare_to_send);

//return message opcode
int nvmeib_mcs_get_opcode(void *msg_in)
{
    int ret;
    struct mcs_message *msg = container_of(msg_in, struct mcs_message, msg);

    NFIN;
    ret = msg->header.opcode;

    NFOUT;
    return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_get_opcode);

void *nvmeib_mcs_get_msg(void *_info, char *buf, size_t len)
{
	struct mcs_info *info = (struct mcs_info *)_info;
	struct mcs_message *msg = (struct mcs_message *)buf;
	size_t expected_len = offsetof(struct mcs_message, msg);
	void *ret = NULL;

	NFIN;
	_NT(trace_nvmeib_mcs_nvmeib_mcs_get_msg, "Received new mcs message, tot_len=@TOT_LEN, msg_len=@MSG_LEN, var_offset=@VAR_OFFSET, "
	   "opcode=@OPCODE, timestamp=@TIMESTAMP ex_len=@EX_LEN", len,
		msg->header.msg_len.msg_len, msg->header.msg_len.var_offset,
		msg->header.opcode, msg->header.timestamp, expected_len);

	if (len < expected_len) {
		_NE(error_nvmeib_mcs_nvmeib_mcs_get_msg, "Illegal message size expected:@EXPECTED_LEN recived:@LEN_LONG",
			expected_len, len);
		goto out;
	}
	if (incarnate_message(info, msg) < 0) {
	   _NE(error_1_nvmeib_mcs_nvmeib_mcs_get_msg, "Message incarnation failed");
	   ret = NULL;
	    goto out;
	}
	ret = &msg->msg;
out:
    NFOUT;
    return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_get_msg);

//allocate message with payload
void *nvmeib_mcs_alloc(void *_info, int opcode, int payload_len)
{
	struct mcs_info *info = (struct mcs_info *)_info;
	void *rv = NULL;
	struct mcs_message *msg;
	int item_id = info->mcs_opcodes_to_incr_id_upstr[opcode];
	size_t net_msg_size = info->mcs_sizeof_items[item_id];

	NFIN;
	msg = GENERIC_ZALLOC(sizeof(*msg) + net_msg_size + payload_len + 1, GFP_KERNEL);
	if (!msg) {
		_NE(error_nvmeib_mcs_nvmeib_mcs_alloc, "Memory allocation problem");
		goto out;
	}

	msg->header.msg_len.msg_len = sizeof(*msg) + net_msg_size + payload_len;
	msg->header.msg_len.var_offset = 0;
	msg->header.opcode = opcode;

	rv = &msg->msg;
out:
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_mcs_alloc);

//realloc message, will alloc only if the current size is too small
void *nvmeib_mcs_realloc(void *old_msg_, void *info_, int opcode)
{
	struct mcs_info *info = (struct mcs_info *)info_;
	struct mcs_message *msg;
	int item_id = info->mcs_opcodes_to_incr_id_upstr[opcode];
	size_t net_msg_size = info->mcs_sizeof_items[item_id];
	struct mcs_message *old_msg;
	size_t old_size;
	size_t new_msg_size = sizeof(*msg) + net_msg_size;
	void * ret = NULL;

	NFIN;
	if (!old_msg_) {
		ret = nvmeib_mcs_alloc(info_, opcode, 0);
		goto out;
	}
	old_msg = container_of(old_msg_, struct mcs_message, msg);
	old_size = (size_t)(old_msg->header.msg_len.msg_len);
	if (old_size <  new_msg_size) {
		GENERIC_FREE(old_msg);
		ret = nvmeib_mcs_alloc(info_, opcode, 0);
	} else {
		old_msg->header.msg_len.msg_len = sizeof(*old_msg) + net_msg_size;
		old_msg->header.msg_len.var_offset = 0;
		old_msg->header.opcode = opcode;
		ret = &old_msg->msg;
	}

out:
	NFOUT;
	return ret;
}

//return chunk of memory from payload area
void *nvmeib_mcs_get_chunck(void *_info,void *msg_, size_t block_size,
	int num_blocks)
{
	struct mcs_info *info = (struct mcs_info *)_info;
	struct mcs_message *msg = container_of(msg_, struct mcs_message, msg);
	void *ret = NULL;
	int opcode = msg->header.opcode;
	int item_id = info->mcs_opcodes_to_incr_id_upstr[opcode];
	size_t net_msg_size = info->mcs_sizeof_items[item_id];
	size_t mem_size = block_size * num_blocks;
	size_t msg_size = offsetof(struct mcs_message, msg) + net_msg_size;
	void *payload = (void *)((char *)&msg->msg + net_msg_size);

	NFIN;
	_NT(trace_nvmeib_mcs_nvmeib_mcs_get_chunck, "net_msg_size=@NET_MSG_SIZE", net_msg_size);
	if (msg_size + msg->header.msg_len.var_offset + mem_size >
			msg->header.msg_len.msg_len)
	    goto out;

	ret = payload + msg->header.msg_len.var_offset;
	msg->header.msg_len.var_offset += mem_size;

out:
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_get_chunck);

//free message allocated with nvmeibc_mcs_alloc
void nvmeib_mcs_free(void *msg_)
{
	struct mcs_message *msg = container_of(msg_, struct mcs_message, msg);

	NFIN;
	GENERIC_FREE(msg);
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_mcs_free);

#ifndef USER_SPACE

struct mcs_build_info {
	struct list_head msg_list;
	unsigned total_len;
};

static int mcs_count_tool(void *context, void *buf,  int len)
{
	struct mcs_build_info *info = context;
	info->total_len += len;
	(void)buf;
	return 0;
}

static int mcs_alloc(struct mcs_build_info *info)
{
	struct msgloop_msg *msg;
	int rv;

	msg = nvmeib_msgloop_alloc_msg(info->total_len, GFP_KERNEL);
	if (msg) {
		list_add_tail(&msg->link, &info->msg_list);
		rv = 0;
	}
	else
		rv = -ENOMEM;
	return rv;
}

int nvmeib_mcs_ack_msg(void *_info, unsigned char token[16])
{
	int ret;
	struct mcs_info *info = (struct mcs_info *)_info;
	NFIN;

	ret = nvmeib_mcs_msg_cache_ack(info->cache, token);

	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_ack_msg);

int nvmeib_mcs_cache_size(void *_info)
{
	int ret;
	struct mcs_info *info = (struct mcs_info *)_info;
	NFIN;

	ret = nvmeib_mcs_msg_cache_size(info->cache);

	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_cache_size);

int nvmeib_mcs_cache_count(void *_info)
{
	int ret;
	struct mcs_info *info = (struct mcs_info *)_info;
	NFIN;

	ret = nvmeib_mcs_msg_cache_tot_count(info->cache);

	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_cache_count);

static int mcs_prepare_tool(void *context, void *buf,  int len)
{
	struct mcs_build_info *info = context;
	struct msgloop_msg *msg;
	char *p;
	int rv;

	NFIN;

	msg = list_first_entry(&info->msg_list, struct msgloop_msg, link);
	_ND(trace_nvmeib_mcs_mcs_prepare_tool, "msg->len=@LEN_LONG len=@LEN info->total_len=@TOTAL_LEN", msg->len, len, info->total_len);
	if (msg->len + len <= info->total_len) {
		p = msg->data + msg->len;
		memcpy(p, buf, len);
		msg->len += len;
		rv = 0;
	}
	else {
		_NE(error_nvmeib_mcs_mcs_prepare_tool, "Tryinig to copy more than been allocated - how come???");
		rv = -ENOMEM;
	}
	NFOUT;
	return rv;
}

static int prepare_message_buffer(struct mcs_info *info,
				  struct mcs_build_info *b_info, void *msg,
				  int opcode)
{
	int rv;
	NFIN;
	INIT_LIST_HEAD(&b_info->msg_list);

	rv = nvmeib_mcs_build_msg(info, opcode, msg, NULL, mcs_count_tool,
				  b_info);
	_ND(trace_1_nvmeib_mcs_nvmeib_mcs_send_proc, "rv1=@RV", rv);
	if (rv < 0) {
		goto out;
	}

	rv = mcs_alloc(b_info);
	_ND(trace_2_nvmeib_mcs_nvmeib_mcs_send_proc44, "rv2=@RV", rv);

out:
	NFOUT;
	return rv;
}

//send a message to mcs
int nvmeib_mcs_send_proc(struct mcs_info *info, void *msg,
		const char (*unique_uuid)[64], int opcode,
		struct msgloop_procfs_ent *ent)
{
	struct mcs_build_info b_info = {{0}, 0};
	struct msgloop_msg *msg_l = NULL;
	unsigned char token[16] = {0};
	struct nvmeib_mcs_msg_cache_entry *cache_entry = NULL;
	int rv;

	NFIN;

	_NI(info_nvmeib_mcs_nvmeib_mcs_send_proc, "Sending to MCS opcode @OPCODE", opcode);

	if (!msg) {
		rv = 0;
		goto out;
	}

	rv = prepare_message_buffer(info, &b_info, msg, opcode);
	_ND(trace_2_nvmeib_mcs_nvmeib_mcs_send_proc, "rv2=@RV", rv);
	if (rv < 0) {
		goto out;
	}

	msg_l = list_first_entry_or_null(&b_info.msg_list, struct msgloop_msg, link);

	if (unique_uuid) {
		cache_entry = nvmeib_mcs_msg_cache_insert(info->cache, unique_uuid);
		if (IS_ERR_OR_NULL(cache_entry)) {
			rv = cache_entry ? PTR_ERR(cache_entry) : -ENOMEM;
			goto out;
		}
		nvmeib_mcs_msg_cache_get_token(cache_entry, &token);
	}

	rv = nvmeib_mcs_build_msg(info, opcode, msg, token, mcs_prepare_tool, &b_info);

	if (rv >= 0) {
		nvmeib_mcs_msg_cache_set_data(info->cache, cache_entry, msg_l);
		rv = nvmeib_msgloop_sendl(ent, &b_info.msg_list);
	} else {
		// could not build message, strange, will "un cache" the message iff cached
		nvmeib_mcs_msg_cache_ack(info->cache, token);
	}

out:
	if (rv < 0) {
		msgloop_put_msg(msg_l);
	}
	NFOUT;
	return rv;
}
EXPORT_SYMBOL(nvmeib_mcs_send_proc);

struct mcs_iter_arg {
	struct mcs_info *info;
	struct msgloop_procfs_ent *ent;
};

static int nvmeib_mcs_send_single_cached_message(void *msg, void *magic)
{
	int rv;
	struct list_head msg_list = {0};
	struct msgloop_msg *msg_l = msg;
	struct mcs_iter_arg *args = magic;
	NFIN;


	INIT_LIST_HEAD(&msg_list);
	list_add_tail(&msg_l->link, &msg_list);
	msgloop_get_msg(msg_l);

	// if we fail to send message it means that msgloop returned to "error-wait" mode (like flushing).
	// in that case we stop sending the messages and wait for msgloop to recover.
	rv = nvmeib_msgloop_sendl(args->ent, &msg_list);
	if (rv < 0)
		msgloop_put_msg(msg_l);

	NFOUT;
	return rv;
}


int nvmeib_mcs_send_cached_msg(struct mcs_info *info,
			       struct msgloop_procfs_ent *ent)
{
	int ret;

	struct mcs_iter_arg arg = { info, ent };

	NFIN;

	ret = nvmeib_mcs_msg_cache_foreach(info->cache, nvmeib_mcs_send_single_cached_message, &arg);
	BUG_ON(ret == -ENOMEM); // when replaying from local cache there are no memory allocations.

	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_send_cached_msg);

#ifdef BLKDEV_SIMULATOR
// Used by client simulator to allow serializing a downstream message
void *replace_upstream_with_downstream(struct mcs_info *orig)
{
	struct mcs_info *changed = GENERIC_ZALLOC_SIM(sizeof(*changed),GFP_KERNEL);
	*changed = *orig;
	changed->mcs_opcodes_to_incr_id_upstr = changed->mcs_opcodes_to_incr_id_downstream;
	return changed;
}
#else
void *replace_upstream_with_downstream(struct mcs_info *orig) { BUG_ON(1); }
#endif

/******************* Daniel: Todo, move to dedicated file *********************/
#include "nvmeib_proc_cli.h"
struct cli_info {
	int dummy;
};

void *nvmeib_cli_init(void){
	return GENERIC_ZALLOC(sizeof(struct cli_info), GFP_KERNEL);
}
EXPORT_SYMBOL(nvmeib_cli_init);

void nvmeib_cli_remove(void *cli){
	GENERIC_FREE(cli);
}
EXPORT_SYMBOL(nvmeib_cli_remove);

int nvmeib_cli_send_proc(void *cli, void *msg, size_t len,
						 struct msgloop_procfs_ent *ent)
{
	(void)cli;
	return nvmeib_msgloop_send(ent, msg, len);
}
EXPORT_SYMBOL(nvmeib_cli_send_proc);

#endif	// Kernel code
