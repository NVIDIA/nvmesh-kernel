#ifndef NVMEIB_DRIVER_H
#define NVMEIB_DRIVER_H

#include "nvmeib.h"

/* deal with the ib driver internals */

struct nvmeib_cq_rsc;
struct nvmeib_sq_rsc;
struct nvmeibc_remote_net;
struct ib_srq;

struct nvmeib_device_ops {
	struct module *module;
	ssize_t (*get_qp_usage)(struct ib_device *ib_dev, struct ib_qp *ib_qp, enum nvmeib_cnt_mem_type mem_type);
	ssize_t (*get_srq_usage)(struct ib_device *ib_dev, struct ib_srq *ib_srq, enum nvmeib_cnt_mem_type mem_type);
	ssize_t (*get_cq_usage)(struct ib_device *ib_dev, struct ib_cq *ib_cq, enum nvmeib_cnt_mem_type mem_type);
	ssize_t (*get_mr_usage)(struct ib_device *ib_dev, struct ib_mr *ib_mr, enum nvmeib_cnt_mem_type mem_type);
	int (*check_rdda_fw)(struct ib_device *ib_dev);
};

struct nvmeib_device_public_ops;

/* qp send_q resources */
struct nvmeib_sq_rsc {
	/* the number of buffers allocated */
	int n_bufs;
	/* the number of contiguous pages in a buffer */
	int n_pages;
	/* the pages */
	dma_addr_t *pages;
	/* the address */
	u64 sq_address;
	/* the access key */
	u32 sq_rkey;
	/* the offset to the send queue */
	int offset;
	/* the size to the send queue */
	int size;
	/* last position set locally in qp send_q */
	u16 last_pos;
	unsigned head;
	/* the doorbell MMIO address */
	u64 doorbell_address;
	u64 doorbell_address_virt;
	/* the doorbell payload */
	u32 doorbell_payload;
	/* for doorbell - maybe we will use it if we can tell
	   IB to trigger mmio on another IB.
	*/
	u32 doorbell_rkey;
	/* info to allow client to shadow the qp */
	int sq_wqe_shift;
	int sq_wqe_cnt;

	/* mlx4 */
	int sq_spare_wqes;
	int sq_max_wqes_per_wr;

	/* mlx5 */
	int db_size_in_db;
	void *doorbell_descr_address_virt;
	u64 doorbell_descr_address;
	u32 doorbell_descr_rkey;

	/* bnxt_re */
	u16 q_full_delta;
	u32 qp_id;
	u32 max_elements;
	u32 psn;
	u16 max_sge;
	u16 mtu;
};

/* qp completion queue resources */
struct nvmeib_cq_rsc {
	/* completion queue info */
	u64 ci_db_addr;
	u32 cons_index;
	u32 cq_rkey;
	u32 entries;
};

/* qp resources */
struct nvmeib_qp_rsc {
	/* the send_q resources */
	struct nvmeib_sq_rsc sq;
	/* the completion queue resources */
	struct nvmeib_cq_rsc cq;
};

enum nvmeib_dev_type {
	DT_mlx4			= 0x01,
	DT_mlx5			= 0x02,
	/* Leave room for more mlx NICs */

	DT_bnxt_re		= 0x08, /*Broadcom Netxtreme (Cu) */

	DT_siw 			= 0x09, /* Soft-iWARP */

	DT_uknown		= 0xff,
};

enum nvmeib_dev_cap {
	NVMEIB_DEVCAP_RDDA		= 0x01,
	NVMEIB_DEVCAP_DUMP_SQ		= 0x02,
	NVMEIB_DEVCAP_SRQ		= 0x04,
	NVMEIB_DEVCAP_SRQ_LAST_WQE	= 0x08,
	NVMEIB_DEVCAP_ATOMICS_REQ			= 0x10,
	NVMEIB_DEVCAP_ATOMICS_RESP			= 0x20,
	NVMEIB_DEVCAP_MASKED_ATOMICS_REQ	= 0x40,
	NVMEIB_DEVCAP_MASKED_ATOMICS_RESP	= 0x80,
	NVMEIB_DEVCAP_RD_ATOM_8				= 0x100, /* Supports 8 in-progress Read/Atomic Ops */
	NVMEIB_DEVCAP_RD_ATOM_16			= 0x200, /* Supports 16 in-progress Read/Atomic Ops */
	NVMEIB_DEVCAP_RD_ATOM_32			= 0x400, /* Supports 32 in-progress Read/Atomic Ops */
	NVMEIB_DEVCAP_RD_ATOM_64			= 0x800, /* Supports 64 in-progress Read/Atomic Ops */
	NVMEIB_DEVCAP_PCIE_ATOMICS			= 0x1000, /* Atomic are implemented using PCIe Atomics (meaning it is interoperable with CPU cmpxchg) */
};

const char *nvmeib_ib_driver_dev_type(enum nvmeib_dev_type t);

#define NVMEIB_WAIT_DREP_TIMEOUT(dev_type)	\
	((dev_type) != DT_siw ? NVMEIB_WAIT_DREP : (2 * HZ))

/* access to remote qp */
struct nvmeibc_remote_net {
	enum nvmeib_dev_type type;
	bool wrap_around;
	u64 remote_sendq_buffer_raddr;
	u32 remote_sendq_buffer_size;
	u32 remote_sendq_buffer_rkey;
	void *rsq;
	void *rsqe;
	/* stuff we need to shadow the send_q */
	u16 sq_last_pos;
	unsigned sq_head;
	int sq_wqe_shift;
	int sq_wqe_cnt;
	/* stuff we need to shadow the completion queue */
	u64 cq_ci_db_raddr;
	u32 cq_rkey;
	u32 cq_initial_consumed_index;
	u32 cq_entries;

	/* mlx4 */
	int sq_spare_wqes;
	int sq_max_wqes_per_wr;
	int last_post_start_ind;
	int last_post_end_ind;

	/* mlx5 */
	int db_size_in_db;

	/* bnxt_re */
	u16 sq_full_delta;
	u32 qp_id;
	u32 sq_psn;
	int sq_max_sge;
	u16 mtu;

	dma_addr_t rsq_dma;
	u32 rsq_l_key;
	/* used by the ib_driver to save the remote qp handling */
	void *priv;
	/* to send the info into the remote send_q we need 2 rdma_iu for the case
	   of a wrap around in the send_q
	*/
	bool send_cleanup;
	/* Flags added for bnxt_re */
	bool update_msix; 		/* Update msix for each op - Always true */
	/* For bnxt_re and nvme drives that send irqs until cqe is cleared */
	bool mask_msix; 		/* Mask msix after each op */
	bool do_rsq_riu3;
	bool do_rsq_riu4;
	struct ib_sge rsq_sge1;
	struct nvmeib_rdma_iu rsq_riu1;
	struct ib_sge rsq_sge2;
	struct nvmeib_rdma_iu rsq_riu2;
	struct ib_sge rsq_sge3;
	struct nvmeib_rdma_iu rsq_riu3;
	struct ib_sge rsq_sge4;
	struct nvmeib_rdma_iu rsq_riu4;
	struct ib_sge rsq_clr_sge1;
	struct nvmeib_rdma_iu rsq_clr_riu1;
	/* the completion queue shadow */
	u32 consumed_index;
	struct ib_sge rcq_sge;
	struct nvmeib_rdma_iu rcq_riu;
	/* remote IB send_q doorbell */
	u64 sq_doorbell_raddr;
	u32 sq_doorbell_size;
	u32 sq_doorbell_rkey;
	u32 sq_doorbell_payload;
	struct ib_sge sq_db_sge;
	struct nvmeib_rdma_iu sq_db_riu;

	/* mlx5 */
	u64 sq_doorbell_descr_raddr;
	u32 sq_doorbell_descr_size;
	u32 sq_doorbell_descr_rkey;
	u32 sq_doorbell_descr_payload;
	struct ib_sge sq_db_descr_sge;
	struct nvmeib_rdma_iu sq_db_descr_riu;
};

__attribute__ ((unused)) static const char *nvmeib_rdma_transport_to_s(enum rdma_link_layer l, enum nvmeib_dev_type dt)
{
	if (l == IB_LINK_LAYER_INFINIBAND)
		return "Infiniband";
	else if (l == IB_LINK_LAYER_ETHERNET) {
		if (dt == DT_siw) return "TCP";
		else return "RoCE";
	}
	return "???";
}

struct ib_device;
struct ib_qp;
/* get the device type */
enum nvmeib_dev_type nvmeib_get_device_type(struct ib_device *ib_dev);
/* check if device type support rdda connection */
bool nvmeib_device_sup_cap(enum nvmeib_dev_type t, enum nvmeib_dev_cap cap);
int nvmeib_device_get_max_rd_atom_on_wire(enum nvmeib_dev_type t);
/* firmware supports RDDA */
int nvmeib_ibdr_check_rdda_fw(struct ib_device *ib_dev);

/* Used for memory usage calculation */
ssize_t nvmeib_ibdr_get_qp_usage(struct ib_device *ib_dev, struct ib_qp *ib_qp,
								 enum nvmeib_cnt_mem_type mem_type);
;ssize_t nvmeib_ibdr_get_srq_usage(struct ib_device *ib_dev, struct ib_srq *ib_srq,
								 enum nvmeib_cnt_mem_type mem_type);
ssize_t nvmeib_ibdr_get_cq_usage(struct ib_device *ib_dev, struct ib_cq *ib_cq,
								 enum nvmeib_cnt_mem_type mem_type);
ssize_t nvmeib_ibdr_get_mr_usage(struct ib_device *ib_dev, struct ib_mr *ib_mr,
								 enum nvmeib_cnt_mem_type mem_type);

/*option to enable capability of a given device type*/
void nvmeib_ib_driver_enable_cap(enum nvmeib_dev_type t, enum nvmeib_dev_cap cap, bool enable);

int nvmeib_ibdr_hwdev_register(enum nvmeib_dev_type dev_type,
			       const char *name,
			       struct nvmeib_device_ops *ops,
			       unsigned long dev_caps,
			       int max_rdda_sq_sz);
int nvmeib_ibdr_hwdev_unregister(enum nvmeib_dev_type type);

struct nvmeib_device_public_ops *nvmeib_ibdr_hwdev_pops_get(struct ib_device *ib_dev);
int nvmeib_ibdr_hwdev_pops_put(struct nvmeib_device_public_ops *pops);

#if KS_HAS_MODULE_MUTEX
int nvmeib_ibdr_hwdev_pops_set(enum nvmeib_dev_type t,
			       struct module *m,
			       struct nvmeib_device_public_ops *pops);
#else
int nvmeib_ibdr_hwdev_pops_set(enum nvmeib_dev_type t,
			       const char *m,
			       struct nvmeib_device_public_ops *pops);
#endif

void nvmeib_ibdr_hwdev_pops_call_all(void (*cb)(struct nvmeib_device_public_ops *pops, void *param), void *param);

#endif


