#ifndef NVMEIBT_NM_IBUD_H
#define NVMEIBT_NM_IBUD_H
#include "nvmeibt_nm.h"
#include "nvmeibt_ib_common.h"
#include "nvmeibt_nm_hw_iface.h"

struct ibud_local_node {
	struct nvmeibt_nm_local_node base;
    struct ibv_device **dev_list;
    struct rdma_event_channel *ch;
	struct ibud_network_offload *network_offload;
	nvmeibt_nm_l_list_t defer_work;
};

struct ibud_per_nic;
struct hash_cq_key_type {
	struct nvmeibt_nm_hash_key_type base;
	struct ibud_per_nic *pn;
	struct ibv_cq *cq;
};

struct ibud_per_nic {
	struct nvmeibt_nm_per_nic base;
	struct ibv_device *device;
	struct ibv_context *context;
	struct ibv_device_attr_ex attr_ex;
	struct ibv_pd *pd;
	bool need_dry;
	struct ibv_comp_channel *comp_ch;
	struct ibv_cq *cq;
	struct hash_cq_key_type cqt;
	struct ibv_srq *srq;
	void *mapped_buffer;
	int mapped_buffer_size;
	struct ibv_mr *srq_mmr;
	void *gmapped_buffer;
	int gmapped_buffer_size;
	struct ibv_mr *srq_gmr;
	int recv_msg_size;
};

struct ibud_per_port {
	struct nvmeibt_nm_per_port base;
	struct ibv_port_attr attr;

	union ibv_gid gid;
	int gid_index;
	int global;
	struct sockaddr_storage bind_sin;
	struct rdma_cm_id *cm_id;
	struct ibv_qp *qp;
	uint16_t pkey;
};

#define ibudpp2pp(_pp) (&((_pp)->base))
#define ibudpp2pn(_pp) (ibudpp2pp(_pp)->pn)
#define ibudpp2ibudpn(_pp) ((struct ibud_per_nic *)(ibudpp2pn(_pp)))

union srq_context {
	uint64_t val;
	struct {
		uint16_t buf_idx;
	} __attribute__ ((packed)) bits;
};

struct ibud_add_nic_req {
	int is_roce;
	uint16_t pkey;
};

struct ibud_req {
	struct nvmeibt_nm_req base;
	
	/* Extensions */
	union {
		struct ibud_add_nic_req add_nic_req;
	};
};

struct ibud_login_data {
	struct nvmeibt_nm_login_data base;
	uint32_t qpn;
	uint16_t slid;
};

struct ibud_path {
	struct nvmeibt_nm_path base;
	
	struct rdma_cm_id *cmid;
	struct ibv_ah *ah;
	uint32_t remote_qp_num;
	uint32_t remote_qkey;
	struct ibv_mr *sbmr;
};

struct ibud_remote_addr {
	struct nvmeibt_nm_remote_addr base;
	uint16_t dlid;
};

#endif