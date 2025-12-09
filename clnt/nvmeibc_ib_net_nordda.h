#ifndef NVMEIBC_IB_NET_NORDDA_H
#define NVMEIBC_IB_NET_NORDDA_H

#include "vex/nvmeibc_vex_shared.h"
#include "nvmeibc_ib_net.h"

struct nvmeibc_ib_net_nordda {
	struct nvmeibc_ib_net base;
	struct nvmeibc_ib_nordda_channel *nrch;
	/* jmds DMA map to remote nic */
	struct nvmeib_remote_access_info jmdc_rai;
	const struct vex_ops *vex_nrio_ops[vex_nrch_ops_num];
};

#if 0
union nordda_wr_id {
	struct {
		u32 version;
		u16 opcode;
		u16 index;
	};
	u64 wr_id;
};

static inline u64 nordda_wr_id_encode(u32 version, u16 opcode, u16 index)
{
	union nordda_wr_id wrid;
	wrid.index = index;
	wrid.opcode = opcode;
	wrid.version = version;
	return wrid.wr_id;
}

static inline u64 nordda_wr_id_encode_gen(u32 version, enum nvmeib_gen_cmd_op gen_op, u16 index)
{
	union nordda_wr_id wrid;
	wrid.index = index;
	wrid.opcode = gen_op + NVMEIB_WR_GEN_OP_START;
	wrid.version = version;
	return wrid.wr_id;
}

static inline u64 nordda_wr_id_decode_version(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return wrid.version;
}

static inline u16 nordda_wr_id_decode_opcode(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return wrid.opcode;
}

static inline bool nordda_wr_opcode_is_gen_cmd(u16 opcode)
{
	return (opcode >= NVMEIB_WR_GEN_OP_START && opcode <= NVMEIB_WR_GEN_OP_END);
}

static inline enum nvmeib_gen_cmd_op nordda_wr_opcode_get_gen_op(u16 opcode)
{
	return opcode - NVMEIB_WR_GEN_OP_START;
}

static inline u16 nordda_wr_id_decode_index(u64 wr_id)
{
	union nordda_wr_id wrid = {.wr_id = wr_id};
	return wrid.index;
}
#endif

static inline struct nvmeibc_ib_net_nordda *in_to_inrn(struct nvmeibc_ib_net *net)
{
	return container_of(net, struct nvmeibc_ib_net_nordda, base);
}

struct nvmeibc_login_request;
struct nvmeibc_volume_req_info;
int nvmeibc_ib_net_nordda_alloc(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_ib_net_params *params, struct nvmeibc_login_request *lreq);
void nvmeibc_ib_net_nordda_free(struct nvmeibc_ib_net_nordda *net);
int nvmeibc_ib_net_nordda_execute_io(struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk *disk, struct nvmeibc_volume_req_info *info,
	struct nvmeibc_disk_command *disk_cmd);

struct volume_server_rsp;

/* gen-cmd */
struct nvmeibc_disk_gen_cmd;
struct nvmeibc_disk_command;
struct nvmeibc_dev;
int nvmeibc_ib_net_nordda_decode_gen_rsp(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_disk_gen_cmd *g,
	struct volume_server_rsp *rsp);
void nvmeibc_ib_net_nordda_unmap_and_unlink_gcmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, int comp_code);
void nvmeibc_ib_net_nordda_complete_gcmd(
	struct nvmeibc_disk_command *dcmd,
	enum stats_done_info_type done_type,
	struct nvmeibc_dev *local_dev);
void nvmeibc_ib_net_nordda_complete_gen_cmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, enum stats_done_info_type done_type, int comp_code);

/* lock-cmd */
struct nvmeibc_disk_lock_cmd;
int nvmeibc_ib_net_nordda_decode_lock_rsp(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req,
	struct volume_server_rsp *rsp);
void nvmeibc_ib_net_nordda_unlink_lcmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req);
void nvmeibc_ib_net_nordda_complete_lcmd(
	struct nvmeibc_disk_lock_cmd *lock_cmd, enum stats_done_info_type done_type, int comp_code);
void nvmeibc_ib_net_nordda_complete_lock_cmd(
	struct nvmeibc_ib_net_nordda *net,
	struct nvmeibc_volume_request *req, enum stats_done_info_type done_type, int comp_code);

#endif
