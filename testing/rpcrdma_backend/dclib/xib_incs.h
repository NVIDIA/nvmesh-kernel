#ifndef XIB_INCS_H
#define XIB_INCS_H

/*#ifndef CONFIG_INFINIBAND_ON_DEMAND_PAGING
#	define CONFIG_INFINIBAND_ON_DEMAND_PAGING
#endif*/

#ifdef NO_OFED
#ifndef OFED_VER_MAJ
#define OFED_VER_MAJ 0
#endif
#ifndef OFED_VER_MIN
#define OFED_VER_MIN 0
#endif
#ifndef OFED_VER_POINT_MAJ
#define OFED_VER_POINT_MAJ 0
#endif
#endif
#define MOFED_VERSION(a,b) (((a) << 8) + (b))

#define MOFED_VERSION_CODE MOFED_VERSION(OFED_VER_MAJ, OFED_VER_MIN)
#define MOFED_VERSION_LT(a,b) (MOFED_VERSION_CODE < MOFED_VERSION(a,b))
#define MOFED_VERSION_LE(a,b) (MOFED_VERSION_CODE <= MOFED_VERSION(a,b))
#define MOFED_VERSION_GT(a,b) (MOFED_VERSION_CODE > MOFED_VERSION(a,b))
#define MOFED_VERSION_GE(a,b) (MOFED_VERSION_CODE >= MOFED_VERSION(a,b))
#define MOFED_VERSION_EQ(a,b) (MOFED_VERSION_CODE ==MOFED_VERSION(a,b))

#define X_PATH_REC_SLID(path_rec) (path_rec).slid
#define X_PATH_REC_DLID(path_rec) (path_rec).dlid
#define X_AH_DLID(ah) (ah).dlid
#define MLX5_ACCESS_MODE access_mode

#ifndef NO_OFED
#	include <linux/compat-2.6.h>
#	define IB_HAS_GID_ATTR MOFED_VERSION_GE(3,0)
#	if MOFED_VERSION_GE(3,0)
#		define IB_FIND_GID_NUM_PARAM 7
#	else
#		define IB_FIND_GID_NUM_PARAM 4
#	endif
#	define IB_DEVICE_HAS_STATIC_ATTRS MOFED_VERSION_GE(4,0)
#	define IB_QUERY_DEVICE_NUM_PARAM 2
#	define IB_HAS_BIND_MW (MOFED_VERSION_GE(3,0) && MOFED_VERSION_LT(4,0))
#	define IB_HAS_MLX5_CAP_ATOMIC_MACRO MOFED_VERSION_GE(3,0)
#	define IB_MLX5_DEV_CAP_FLAG_ATOMIC MOFED_VERSION_LT(3,0) && MOFED_VERSION_LT(4,0)
#	define IB_MLX5_VFREE MOFED_VERSION_GE(3,4) && MOFED_VERSION_LT(4,0)
#	define IB_MLX5_MR_HAS_INVALIDATED MOFED_VERSION_GE(3,0) && MOFED_VERSION_LT(4,0)
#	define IB_MLX5_CORE_MKEY MOFED_VERSION_GE(4,0)
#	define IB_MLX5_QP_TRANS MOFED_VERSION_GE(4,0)
#	define IB_MLX5_QP_SWR_CTX MOFED_VERSION_GE(3,1) && MOFED_VERSION_LT(4,0)
#	define IB_MLX4 defined(CONFIG_MLX4_CORE)
#	define IB_MLX5 defined(CONFIG_MLX5_CORE)
#	define IB_RDMA_CREATE_ID_HAS_NET MOFED_VERSION_GE(4,0)
#	define IB_CM_LISTEN_HAS_COMPARE_DATA (MOFED_VERSION_GE(3,0) && MOFED_VERSION_LT(4,0))
#	define IB_CREATE_CQ_INIT_ATTRS MOFED_VERSION_GE(4,0)
#	define IB_QUERY_ROCE_GID_DOES_DEV_HOLD	MOFED_VERSION_GE(4,0)
#   if MOFED_VERSION_GE(4,0)
#		define IB_GID_TYPE_ROCE_V2 IB_GID_TYPE_ROCE_UDP_ENCAP
#	endif
#	define IB_MLX5_IFC MOFED_VERSION_GE(4,0)

#	define IB_NEW_FR						MOFED_VERSION_GE(4,0)
#	define IB_REMOVE_EXTRA_ARG				MOFED_VERSION_GE(4,0)
#	define IB_IB_MAP_MR_SG_OFFSET			MOFED_VERSION_GE(4,0)
#	define IB_SET_CPI_RESP_TIME				MOFED_VERSION_GE(4,0)
#	define IB_MLX_FENCE_VLAN				MOFED_VERSION_GE(4,0)
#	define IB_MLX5_BUF_PAGE_LIST			MOFED_VERSION_LT(4,0)
#	define IB_CREATE_SEND_MAD_BASE_VERSION MOFED_VERSION_GE(4,0)
#	define RDMA_CM_HAS_SET_TIMEOUT	MOFED_VERSION_LT(4,0)
#	define IB_DEVICE_HAS_GET_NETDEV MOFED_VERSION_GE(3,0)
#	define MLX5_IB_WQ_SWR_CTX 	MOFED_VERSION_LT(4,0)
#	define IB_MLX5_NEW_BF		MOFED_VERSION_GE(4,1)
#	define IB_HAS_RDMA_AH_ATTR_TYPE 		MOFED_VERSION_GE(4,2)
#	if MOFED_VERSION_EQ(4,2)
#		define RDMA_NODE_IB_SWITCH 0
#	endif
#	if MOFED_VERSION_GE(4,2)
#		define ib_sa_path_rec sa_path_rec
#		undef X_PATH_REC_SLID
#		undef X_PATH_REC_DLID
#		undef X_AH_DLID
#		undef MLX5_ACCESS_MODE
#		define X_PATH_REC_SLID(path_rec) (path_rec).ib.slid
#		define X_PATH_REC_DLID(path_rec) (path_rec).ib.dlid
#		define X_AH_DLID(ah) (ah).ib.dlid
#		define MLX5_ACCESS_MODE access_mode_1_0
#		define mlx5_vzalloc mlx5_vzalloc_compat
#		define ib_destroy_ah rdma_destroy_ah
#		define X_HAS_AH_ATTR_TYPE 1
#	endif
#	define MLX5_IB_QP_FRAG_BUF MOFED_VERSION_GE(4,4)
#	define MLX5_IB_WQ_FRAG_BUF_CTRL (MOFED_VERSION_GT(4,4) || (MOFED_VERSION_EQ(4,4) && OFED_VER_POINT_MAJ >= 2))
#	define MLX5_IB_FBC_HAS_FRAG_BUF	MOFED_VERSION_LT(4,6)
#	define MLX5_IB_CQ_FRAG_BUF_CTRL MOFED_VERSION_GE(4,4)
#	define MLX5_IB_SRQ_FRAG_BUF_CTRL MOFED_VERSION_GE(4,4)
#	ifndef IB_HAS_CMA_PRIV_H
#		define IB_HAS_CMA_PRIV_H MOFED_VERSION_GE(4,4)
#	endif
#	define MLX4_IB_SQ_MULTIPLE_WQES_PER_WR	MOFED_VERSION_LT(4,6)

#	if	MOFED_VERSION_GE(4,6)
#		define IB_DECLARE_BAD_SEND_WR(name) const struct ib_send_wr *name
#		define IB_DECLARE_BAD_RECV_WR(name) const struct ib_recv_wr *name
#		define IB_DECLARE_CM_HANDLER(name) int name(struct ib_cm_id *cm_id, const struct ib_cm_event *event)
#	else
#		define IB_DECLARE_BAD_SEND_WR(name) struct ib_send_wr *name
#		define IB_DECLARE_BAD_RECV_WR(name) struct ib_recv_wr *name
#		define IB_DECLARE_CM_HANDLER(name) int name(struct ib_cm_id *cm_id, struct ib_cm_event *event)
#	endif
#	define IB_HAS_RDMA_GET_GID_ATTR		MOFED_VERSION_GE(4,6)
#	define IB_SA_PATH_REC_GET_HAS_RETRIES 1

#	define MLX5_IB_WQ_SWR_CTX 			MOFED_VERSION_LT(4,0)
#	define MLX5_IB_QP_BASE				MOFED_VERSION_GE(4,0)
#	define MLX5_IB_QP_SWR_CTX			MOFED_VERSION_LT(4,0)

#else // NO_OFED
#	include "xkr_version.h"
#	define IB_MLX4 1
#	define IB_MLX5					(K_CHECK_VER(3,11,0) || K_CHECK_VER(3,10,0))
#	define IB_HAS_GID_ATTR				(K_CHECK_VER(4,4,0) || K_CHECK_VER(3,10,0))
#	define IB_QUERY_ROCE_GID_DOES_DEV_HOLD 		(IB_HAS_GID_ATTR)
#	if (K_CHECK_VER(4,7,0))
#		define IB_FIND_GID_NUM_PARAM 6
#	elif (K_CHECK_VER(4,4,0))
#		define IB_FIND_GID_NUM_PARAM 5
#	else
#		define IB_FIND_GID_NUM_PARAM 4
#	endif
#	define IB_GID_TYPE_ROCE_V2 IB_GID_TYPE_ROCE_UDP_ENCAP
#	if (K_CHECK_VER(4,4,0))
#		define IB_DEVICE_HAS_STATIC_ATTRS 1
#	else
#		define IB_DEVICE_HAS_STATIC_ATTRS 1
#		if (K_CHECK_VER(4,2,0))
#			define IB_QUERY_DEVICE_NUM_PARAM 3
#		else
#			define IB_QUERY_DEVICE_NUM_PARAM 2
#		endif
#	endif
#	define IB_HAS_BIND_MW 0
#	define IB_HAS_MLX5_CAP_ATOMIC_MACRO (K_CHECK_VER(4,2,0) || K_CHECK_VER(3,10,0))
#	define IB_MLX5_DEV_CAP_FLAG_ATOMIC 0
#	define IB_MLX5_VFREE 0
#	define IB_MLX5_MR_HAS_INVALIDATED 0
#	define IB_MLX5_CORE_MKEY	(K_CHECK_VER(4,6,0) || K_CHECK_VER(3,10,0))
#	define IB_MLX5_QP_TRANS		(K_CHECK_VER(4,5,0) || K_CHECK_VER(3,10,0))
#	define IB_MLX5_QP_SWR_CTX 0
#	define IB_RDMA_CREATE_ID_HAS_NET KS_RDMA_CREATE_ID_HAS_NET
#	define IB_CM_LISTEN_HAS_COMPARE_DATA KS_IB_CM_LISTEN_HAS_COMPARE_DATA
#	define IB_CREATE_CQ_INIT_ATTRS KS_CQ_INIT_ATTRS
#	define IB_MLX5_IFC						KS_MLX5_IFC
#	define IB_REMOVE_EXTRA_ARG				KS_REMOVE_EXTRA_ARG
#	define IB_NEW_FR						KS_NEW_FR
#	define IB_IB_MAP_MR_SG_OFFSET			KS_IB_MAP_MR_SG_OFFSET
#	define IB_SET_CPI_RESP_TIME				KS_SET_CPI_RESP_TIME
#	define IB_MLX_FENCE_VLAN				KS_MLX_FENCE_VLAN
#	define IB_MLX5_BUF_PAGE_LIST			KS_MLX5_BUF_PAGE_LIST
#	define IB_CREATE_SEND_MAD_BASE_VERSION KS_CREATE_SEND_MAD_BASE_VERSION
#	define RDMA_CM_HAS_SET_TIMEOUT	0
#	define IB_DEVICE_HAS_GET_NETDEV KS_IB_DEVICE_HAS_GET_NETDEV
#	define IB_HAS_RDMA_AH_ATTR_TYPE KS_IB_HAS_RDMA_AH_ATTR_TYPE
#	ifndef IB_MLX5_NEW_BF
#		define IB_MLX5_NEW_BF K_CHECK_VER(4,14,0)
#	endif
#	if KS_HAS_SA_PATH_REC
#		define	ib_sa_path_rec sa_path_rec
#		undef X_PATH_REC_SLID
#		undef X_PATH_REC_DLID
#		undef X_AH_DLID
#		define X_PATH_REC_SLID(path_rec) (path_rec).ib.slid
#		define X_PATH_REC_DLID(path_rec) (path_rec).ib.dlid
#		define X_AH_DLID(ah) (ah).ib.dlid
#		define mlx5_vzalloc mlx5_vzalloc_compat
#		define ib_destroy_ah rdma_destroy_ah
#	endif
#	if KS_MLX5_ACCESS_MODE_1_0
#		undef MLX5_ACCESS_MODE
#		define MLX5_ACCESS_MODE access_mode_1_0
#		define mlx5_vzalloc mlx5_vzalloc_compat
#	endif
#	ifndef MLX5_IB_QP_FRAG_BUF
#		define MLX5_IB_QP_FRAG_BUF KS_MLX5_IB_QP_FRAG_BUF
#	endif
#	ifndef MLX5_IB_WQ_FRAG_BUF_CTRL
#		define MLX5_IB_WQ_FRAG_BUF_CTRL 0
#	endif
#	ifndef MLX5_IB_FBC_HAS_FRAG_BUF
#		define MLX5_IB_FBC_HAS_FRAG_BUF KS_MLX5_IB_FBC_HAS_FRAG_BUF
#	endif
#	ifndef MLX5_IB_CQ_FRAG_BUF_CTRL
#		define MLX5_IB_CQ_FRAG_BUF_CTRL KS_MLX5_IB_CQ_FRAG_BUF_CTRL
#	endif
#	ifndef MLX5_IB_SRQ_FRAG_BUF_CTRL
#		define MLX5_IB_SRQ_FRAG_BUF_CTRL KS_MLX5_IB_SRQ_FRAG_BUF_CTRL
#	endif
#	define MLX4_IB_SQ_MULTIPLE_WQES_PER_WR	0
#	if KS_POST_SEND_HAS_CONST
#		define IB_DECLARE_BAD_SEND_WR(name) const struct ib_send_wr *name
#		define IB_DECLARE_BAD_RECV_WR(name) const struct ib_recv_wr *name
#		define IB_DECLARE_CM_HANDLER(name) int name(struct ib_cm_id *cm_id, const struct ib_cm_event *event)
#	else
#		define IB_DECLARE_BAD_SEND_WR(name) struct ib_send_wr *name
#		define IB_DECLARE_BAD_RECV_WR(name) struct ib_recv_wr *name
#		define IB_DECLARE_CM_HANDLER(name) int name(struct ib_cm_id *cm_id, struct ib_cm_event *event)
#	endif
#	define IB_HAS_RDMA_GET_GID_ATTR		KS_IB_HAS_RDMA_GET_GID_ATTR
#	define IB_SA_PATH_REC_GET_HAS_RETRIES	KS_IB_SA_PATH_REC_GET_HAS_RETRIES
#if K_CHECK_VER(4,18,0)
#	undef MLX5_ACCESS_MODE
#	define MLX5_ACCESS_MODE access_mode_1_0
#endif
#	define MLX5_IB_QP_SWR_CTX 0
#endif
#include <rdma/ib_verbs.h>
#include <rdma/ib_sa.h>
#include <rdma/ib_cm.h>
#include <rdma/ib_mad.h>
#include <rdma/ib_fmr_pool.h>
#include <rdma/rdma_cm.h>

#ifndef CONFIG_COMPAT_PM_QOS
#	define CONFIG_COMPAT_PM_QOS
#endif
#ifndef CONFIG_COMPAT_PM_QOS_V2
#	define CONFIG_COMPAT_PM_QOS_V2
#endif

/* Used to abstract away the old ib_send_wr union and new ib_rdma_wr, ib_atomic_wr which extend ib_send_wr */
#if MOFED_VERSION_GE(4,0) || (defined(NO_OFED) && KS_RDMA_WR)
	struct x_send_wr {
		union {
			struct ib_send_wr common;
			struct ib_rdma_wr rdma;
			struct ib_atomic_wr atomic;
			struct ib_reg_wr reg;
		};
	};
#	define x_send_wr_rdma(x_send_wr)	((x_send_wr).rdma)
#	define x_send_wr_atomic(x_send_wr) 	((x_send_wr).atomic)
#	define x_send_wr_reg(x_send_wr)	((x_send_wr).reg)
#	define x_send_wr_reg_access(x_send_wr) (x_send_wr_reg(x_send_wr).access)
#	define x_send_wr_reg_key(x_send_wr) (x_send_wr_reg(x_send_wr).key)
#else
	struct x_send_wr {
		struct ib_send_wr common;
	};
#	define x_send_wr_rdma(x_send_wr) 	((x_send_wr).common.wr.rdma)
#	define x_send_wr_atomic(x_send_wr) 	((x_send_wr).common.wr.atomic)
#	define x_send_wr_reg(x_send_wr) ((x_send_wr).common.wr.fast_reg)
#	define x_send_wr_reg_access(x_send_wr) (x_send_wr_reg(x_send_wr).access_flags)
#	define x_send_wr_reg_key(x_send_wr) (x_send_wr_reg(x_send_wr).rkey)
#endif
#define x_send_wr_ptr_from_ib(ib_send_wr_ptr)				(container_of((ib_send_wr_ptr), struct x_send_wr, common))
#define x_send_wr_to_ib_ptr(x_send_wr) 				(&(x_send_wr).common)
#define x_send_wr_common(x_send_wr) 					((x_send_wr).common)
#define x_send_wr_set_next(x_send_wr, x_send_wr_next_ptr) 	do { x_send_wr_common(x_send_wr).next = x_send_wr_to_ib_ptr(*x_send_wr_next_ptr); } while (0)
#define x_send_wr_clear_next(x_send_wr)				do { x_send_wr_common(x_send_wr).next = NULL; } while (0)
#define x_send_wr_ex(x_send_wr) 					(x_send_wr_common(x_send_wr).ex)
#define x_send_wr_next_valid(x_send_wr)				((x_send_wr_common(x_send_wr).next) != NULL)
#define x_send_wr_next_ptr(x_send_wr)					(x_send_wr_next_valid(x_send_wr) ? x_send_wr_ptr_from_ib(((x_send_wr).common.next)) : NULL)
#define x_send_wr_for_each(itr_ptr, head_ptr) \
	for (itr_ptr = head_ptr; itr_ptr != NULL; itr_ptr = x_send_wr_next_ptr(*itr_ptr))

/* Restore the original kernel module_init macro that does not force GPL */
#undef module_init
#define module_init(initfn)                                     \
        static inline initcall_t __inittest(void)               \
        { return initfn; }                                      \
        int init_module(void) __attribute__((alias(#initfn)));

/* Compensate for different ib_query_device possibilities */
static __always_inline int __ib_query_device(struct ib_device *device, struct ib_device_attr *device_attr)
{
	int rv = 0;
#if IB_DEVICE_HAS_STATIC_ATTRS
	if (device_attr)
		*device_attr = device->attrs;
#else
#if IB_QUERY_DEVICE_NUM_PARAM == 3
	{
		struct ib_udata uhw = {.outlen = 0, .inlen = 0};
		rv = ib_query_device(device, device_attr, &uhw);
	}
#else
	rv = ib_query_device(device, device_attr);
#endif
#endif
	return rv;
}
#define ib_query_device __ib_query_device

#if IB_RDMA_CREATE_ID_HAS_NET
static __always_inline struct rdma_cm_id *__rdma_create_id_no_net(rdma_cm_event_handler event_handler, 
			void *context,
#if KS_IB_RDMA_PORT_SPACE
			enum rdma_port_space ps,
#else
			enum rdma_ucm_port_space ps,
#endif
			enum ib_qp_type qp_type)
{
		return rdma_create_id(&init_net, event_handler, context, ps, qp_type);
}
#ifdef rdma_create_id
#undef rdma_create_id
#endif
#define rdma_create_id(evth, cxt, ps, qpt)\
	__rdma_create_id_no_net(evth, cxt, ps, qpt)
#endif

#ifndef PHYS_PFN
#define PHYS_PFN(x)     ((unsigned long)((x) >> PAGE_SHIFT))
#endif

#if IB_CM_LISTEN_HAS_COMPARE_DATA
static __always_inline int __ib_cm_listen_no_comp_data(struct ib_cm_id *cm_id, __be64 service_id, __be64 service_mask)
{
	return ib_cm_listen(cm_id, service_id, service_mask, NULL);
}
#define ib_cm_listen(cm_id, svc_id, svc_msk)\
	__ib_cm_listen_no_comp_data(cm_id, svc_id, svc_msk)
#endif

#if IB_CREATE_CQ_INIT_ATTRS
static __always_inline struct ib_cq *__ib_create_cq_no_attr(struct ib_device *device,
	ib_comp_handler comp_handler, void (*event_handler)(struct ib_event *, void *),
	void *cq_context, int cqe, int comp_vector)
{
	struct ib_cq_init_attr attr = {0};
	attr.cqe = cqe;
	attr.comp_vector = comp_vector;
	return ib_create_cq(device, comp_handler, event_handler, cq_context, &attr);
}
#define x_create_cq(dev, comp_h, evt_h, ctx, cqe, comp_v)\
	__ib_create_cq_no_attr(dev, comp_h, evt_h, ctx, cqe, comp_v)
#else
    #define x_create_cq ib_create_cq
#endif

#if defined(KS_HAS_IB_ALLOC_MACRO) && KS_HAS_IB_ALLOC_MACRO
#	undef ib_alloc_pd
#if KS_IB_ALLOC_HAS_SKIP_TRACKING
#	define ib_alloc_pd(device) \
		__ib_alloc_pd(device, IB_PD_UNSAFE_GLOBAL_RKEY, __func__, 0)
#else
#	define ib_alloc_pd(device) \
		__ib_alloc_pd(device, IB_PD_UNSAFE_GLOBAL_RKEY, __func__)
#endif
#	define x_get_lkey(dev) ((dev)->mr ? (dev)->mr->lkey : (dev)->pd->local_dma_lkey)
#	define x_get_rkey(dev) ((dev)->mr ? (dev)->mr->rkey : (dev)->pd->unsafe_global_rkey)
#else
#	define x_get_lkey(dev) (dev)->mr->lkey
#	define x_get_rkey(dev) (dev)->mr->rkey
#endif

struct net_device;
struct ethtool_cmd;
extern int __ethtool_get_settings(struct net_device *dev,
				  struct ethtool_cmd *cmd);

static inline void *mlx5_vzalloc_compat(unsigned long size)
{
	void *rtn;

	rtn = kzalloc(size, GFP_KERNEL | __GFP_NOWARN);
	if (!rtn)
		rtn = vzalloc(size);
        return rtn;
}


#if KS_IB_DEVICE_HAS_OPS

#define get_dma_mr ops.get_dma_mr

#define alloc_fmr ops.alloc_fmr
#define dealloc_fmr ops.dealloc_fmr
#define map_phys_fmr ops.map_phys_fmr
#define unmap_fmr ops.unmap_fmr
#define get_netdev ops.get_netdev
#define dereg_mr ops.dereg_mr

#define rdma_destroy_ah(_ah) rdma_destroy_ah(_ah, RDMA_DESTROY_AH_SLEEPABLE)

#endif

#endif
