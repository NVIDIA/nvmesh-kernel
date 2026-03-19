/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef IB_INCS_H
#define IB_INCS_H
/*Simulation of Infiniband connection. Complementary file to linux kernel simulator for programs using IB or RDMA verbs */
#include "kr_incs.h"								// Including kernel libraries or kernel simulator

// Linux/device.h
struct device {
	struct device		*parent;
	struct device_private	*p;
};

// Linux/dma-mapping.h /Linux/dma-direction
enum dma_data_direction { DMA_BIDIRECTIONAL = 0, DMA_TO_DEVICE = 1, DMA_FROM_DEVICE = 2, DMA_NONE = 3 };
struct ib_device;
static inline void *dma_alloc_coherent(struct device *dev, size_t size,                 dma_addr_t *dma_handle, gfp_t flag){ (void)dev; (void)dma_handle; return kmalloc(size, flag); }
static inline void  dma_free_coherent( struct device *dev, size_t size, void *cpu_addr, dma_addr_t  dma_handle)            { (void)dev; (void)dma_handle; (void)size; kfree(cpu_addr);}
static inline dma_addr_t dma_map_single(   struct device *dev,      void *addr, size_t size, enum dma_data_direction dir){ (void)dev; (void)size; (void)dir; return (dma_addr_t)addr; }
static inline void       dma_unmap_single( struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir){ (void)dev; (void)size; (void)dir;              (void)addr; }
static inline dma_addr_t dma_map_page(struct device *dev, struct page *page, size_t offset, size_t size,
				      enum dma_data_direction dir)
{
	(void)dev;
	(void)size;
	(void)dir;
	return (dma_addr_t)((unsigned char *)page_address(page) + offset);
}
static inline void dma_unmap_page(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	(void)dev;
	(void)addr;
	(void)size;
	(void)dir;
}
static inline int        dma_mapping_error(struct device *dev, dma_addr_t addr)                                          { (void)dev;                              return (addr == 0);}
static inline void  *ib_dma_alloc_coherent( struct ib_device *dev, size_t size, dma_addr_t *dma_handle, gfp_t  flag)            { (void)dev; (void)dma_handle; return kmalloc(size, flag);}
static inline void  ib_dma_free_coherent( struct ib_device *dev, size_t size, void *cpu_addr, dma_addr_t  dma_handle)            { (void)dev; (void)dma_handle; (void)size; kfree(cpu_addr);}
static inline int	ib_dma_mapping_error(struct ib_device *ib_dev, dma_addr_t addr)                                  { (void)ib_dev;                              return (addr == 0);}
static inline dma_addr_t ib_dma_map_single(   struct ib_device *ib_dev,      void *addr, size_t size, enum dma_data_direction dir){ (void)ib_dev; (void)size; (void)dir; return (dma_addr_t)addr; }
static inline void       ib_dma_unmap_single( struct ib_device *ib_dev, dma_addr_t addr, size_t size, enum dma_data_direction dir){ (void)ib_dev; (void)size; (void)dir;              (void)addr; }

static inline int ib_dma_map_sg(struct ib_device *dev, struct scatterlist *sg, int nelems, enum dma_data_direction dir) { (void)dev; (void)sg; (void)nelems; (void)dir; return 0; }
static inline void ib_dma_unmap_sg(struct ib_device *dev, struct scatterlist *sg, int nelems, enum dma_data_direction dir) { (void)dev; (void)sg; (void)nelems; (void)dir;}
static inline void dma_sync_sg_for_device(struct device *dev, struct scatterlist *sg, int nelems, enum dma_data_direction dir) { (void)dev; (void)sg; (void)nelems; (void)dir; }
static inline void dma_sync_sg_for_cpu(struct device *dev, struct scatterlist *sg, int nelems, enum dma_data_direction dir) { (void)dev; (void)sg; (void)nelems; (void)dir; }
static inline void dma_sync_single_for_cpu(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir) { (void)dev; (void)addr; (void)size; (void)dir; }
static inline void dma_sync_single_for_device(struct device *dev, dma_addr_t addr, size_t size, enum dma_data_direction dir) { (void)dev; (void)addr; (void)size; (void)dir; }

union ib_gid {
	u8	raw[16];
	struct {
		__be64	subnet_prefix;
		__be64	interface_id;
	} global;
};

struct ib_sa_path_rec { union ib_gid dgid; };
enum ib_wc_status { IB_WC_SUCCESS, };
enum ib_mtu { IB_MTU_512  = 2 };
static inline int ib_mtu_enum_to_int(enum ib_mtu mtu) {
	switch (mtu) {
	case IB_MTU_512:  return  512;
	default: return -1;
	}
}

enum rdma_link_layer { IB_LINK_LAYER_UNSPECIFIED, IB_LINK_LAYER_INFINIBAND, IB_LINK_LAYER_ETHERNET };
enum ib_event_type {
	IB_EVENT_CQ_ERR,
	IB_EVENT_QP_FATAL,
	IB_EVENT_QP_REQ_ERR,
	IB_EVENT_QP_ACCESS_ERR,
	IB_EVENT_COMM_EST,
	IB_EVENT_SQ_DRAINED,
	IB_EVENT_PATH_MIG,
	IB_EVENT_PATH_MIG_ERR,
	IB_EVENT_DEVICE_FATAL,
	IB_EVENT_PORT_ACTIVE,
	IB_EVENT_PORT_ERR,
	IB_EVENT_LID_CHANGE,
	IB_EVENT_PKEY_CHANGE,
	IB_EVENT_SM_CHANGE,
	IB_EVENT_SRQ_ERR,
	IB_EVENT_SRQ_LIMIT_REACHED,
	IB_EVENT_QP_LAST_WQE_REACHED,
	IB_EVENT_CLIENT_REREGISTER,
	IB_EVENT_GID_CHANGE,
	IB_EXP_EVENT_DCT_KEY_VIOLATION = 32,
	IB_EXP_EVENT_DCT_ACCESS_ERR,
	IB_EXP_EVENT_DCT_REQ_ERR,
};

enum rdma_node_type { RDMA_NODE_IB_CA = 1, RDMA_NODE_IB_SWITCH, RDMA_NODE_IB_ROUTER, RDMA_NODE_RNIC, RDMA_NODE_USNIC, RDMA_NODE_USNIC_UDP };
enum ib_port_state { IB_PORT_NOP = 0, IB_PORT_DOWN = 1, IB_PORT_INIT = 2, IB_PORT_ARMED = 3, IB_PORT_ACTIVE = 4, IB_PORT_ACTIVE_DEFER = 5 };
struct ib_qp_init_attr { void *qp_context; };
struct ib_cq { struct ib_device *device; };
struct ib_qp { struct ib_device *device;};
struct ib_wc { u64 wr_id; };
struct ib_send_wr { struct ib_send_wr *next; };
struct nvmeib_send_wr {struct ib_send_wr common; };
struct ib_sge { u64 addr; u32 length, lkey; };
#ifndef UM_APP		// Already defined in UM in /user/lib/rdma....
	struct rdma_conn_param { const void *private_data; };
	enum ib_cm_data_size { IB_CM_REQ_PRIVATE_DATA_SIZE = 92 };
#endif

struct ib_event {
	struct ib_device *device;
	union {
		u8 port_num;
	} element;
	enum ib_event_type event;
};

struct ib_event_handler {
	struct ib_device *device;
	void (*handler)(struct ib_event_handler *, struct ib_event *);
};

#define INIT_IB_EVENT_HANDLER(_ptr, _device, _handler)  do { (_ptr)->device  = _device; (_ptr)->handler = _handler; INIT_LIST_HEAD(&(_ptr)->list); } while (0)
struct ib_port_attr {
	enum ib_port_state state;
	u32 active_mtu, max_mtu;
};

#define IB_DEVICE_NAME_MAX 64
struct ib_device {
	struct device *dma_device;
	char name[IB_DEVICE_NAME_MAX];
	u8 	node_type;
};

struct ib_device_attr { int max_srq; };
struct ib_sa_client { };
struct ib_pd { struct ib_device  *device; };
struct ib_fmr_pool { };
struct ib_fmr_pool_param { };

struct ib_client {
	char  *name;
	void (*add)   (struct ib_device *);
	void (*remove)(struct ib_device *);
	struct ib_device *dev;		// Daniel: Too lazy to implement struct list_head of of devices. Anyways we have only 1 device per client in the simulator
	void *data;					// Daniel added a field holding data. I don't know how it is implemented in the real IB.
};

enum ib_atomic_cap { IB_ATOMIC_NONE };
enum ib_gid_type { IB_GID_TYPE_IB = 0, };
enum rdma_network_type { RDMA_NETWORK_IB, RDMA_NETWORK_IPV4, RDMA_NETWORK_IPV6 };
enum rdma_transport_type { RDMA_TRANSPORT_IB, RDMA_TRANSPORT_IWARP, RDMA_TRANSPORT_USNIC, RDMA_TRANSPORT_USNIC_UDP};

#define RDMA_IB_IP_PS_MASK   0xFFFFFFFFFFFF0000ULL
int   ib_register_client  (	 						struct ib_client *client);
void  ib_unregister_client(	 						struct ib_client *client);
void  ib_set_client_data(struct ib_device *device, 	struct ib_client *client, void *data);
void *ib_get_client_data(struct ib_device *device, 	struct ib_client *client);

static inline int ib_register_event_handler  (struct ib_event_handler *event_handler){(void)event_handler; return 0;}
static inline int ib_unregister_event_handler(struct ib_event_handler *event_handler){(void)event_handler; return 0;}

static inline struct ib_fmr_pool *ib_create_fmr_pool(struct ib_pd *pd, struct ib_fmr_pool_param *params){(void)pd; (void)params; return (struct ib_fmr_pool*)kmalloc(sizeof(struct ib_fmr_pool),0);}
static inline void ib_destroy_fmr_pool(struct ib_fmr_pool *pool){ kfree(pool); }
static inline int  ib_query_port(struct ib_device *device, u8 port_num, struct ib_port_attr *a){ (void)device; (void)port_num; a->state = IB_PORT_ACTIVE; a->active_mtu = a->max_mtu = 0; return 0; }
static inline int  ib_query_pkey(struct ib_device *device, u8 port_num, u16 index, u16 *pkey)		  {	 (void)device; (void)port_num; (void)index; (void)pkey; return 0; }
static inline int  ib_dealloc_pd(			 struct ib_pd 		 *pd){ 		(void)pd; return 0; }
static inline void ib_sa_register_client(	 struct ib_sa_client *client){	(void)client; return; }
static inline void ib_sa_unregister_client(	 struct ib_sa_client *client){	(void)client; return; }

enum rdma_link_layer rdma_port_get_link_layer(struct ib_device *device, u8 port_num);
__attribute_const__ enum rdma_transport_type rdma_node_get_transport(enum rdma_node_type node_type);

// /linux/if_eth.h
#ifndef ETH_ALEN
	#define ETH_ALEN	6		/* Octets in one ethernet addr	 */
#endif
#endif
