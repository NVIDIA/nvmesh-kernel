/*
 * Software iWARP device driver for Linux
 *
 * Authors: Bernard Metzler <bmt@zurich.ibm.com>
 *
 * Copyright (c) 2008-2016, IBM Corporation
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * BSD license below:
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   - Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *
 *   - Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *
 *   - Neither the name of IBM nor the names of its contributors may be
 *     used to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <linux/errno.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
/* for in_dev-get/put */
#include <linux/inetdevice.h>
/* for in6_dev-get/put */
#include <net/addrconf.h>

#include <rdma/iw_cm.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_smi.h>
#include <rdma/ib_user_verbs.h>
#include <rdma/uverbs_ioctl.h>

#include "siw.h"
#include "siw_verbs.h"
#include "siw_obj.h"
#include "siw_cm.h"

static int ib_qp_state_to_siw_qp_state[IB_QPS_ERR+1] = {
	[IB_QPS_RESET]	= SIW_QP_STATE_IDLE,
	[IB_QPS_INIT]	= SIW_QP_STATE_IDLE,
	[IB_QPS_RTR]	= SIW_QP_STATE_RTR,
	[IB_QPS_RTS]	= SIW_QP_STATE_RTS,
	[IB_QPS_SQD]	= SIW_QP_STATE_CLOSING,
	[IB_QPS_SQE]	= SIW_QP_STATE_TERMINATE,
	[IB_QPS_ERR]	= SIW_QP_STATE_ERROR
};

enum ib_qp_state siw_2_ib_qp_state(enum siw_qp_state siw_state)
{
	enum ib_qp_state ib_state = IB_QPS_RESET;

	switch (siw_state) {
	case SIW_QP_STATE_IDLE:
		ib_state = IB_QPS_INIT;
		break;
	case SIW_QP_STATE_RTR:
		ib_state = IB_QPS_RTR;
		break;
	case SIW_QP_STATE_RTS:
		ib_state = IB_QPS_RTS;
		break;
	case SIW_QP_STATE_CLOSING:
		ib_state = IB_QPS_SQD;
		break;
	case SIW_QP_STATE_TERMINATE:
		ib_state = IB_QPS_SQE;
		break;
	case SIW_QP_STATE_ERROR:
		ib_state = IB_QPS_ERR;
		break;
	default:
		break;
	}
	return ib_state;
}

static inline struct siw_pd *siw_pd_ofa2siw(struct ib_pd *ofa_pd)
{
	return container_of(ofa_pd, struct siw_pd, ofa_pd);
}

static inline struct siw_ucontext *siw_ctx_ofa2siw(struct ib_ucontext *ofa_ctx)
{
	return container_of(ofa_ctx, struct siw_ucontext, ib_ucontext);
}

static inline struct siw_cq *siw_cq_ofa2siw(struct ib_cq *ofa_cq)
{
	return container_of(ofa_cq, struct siw_cq, ofa_cq);
}

static inline struct siw_srq *siw_srq_ofa2siw(struct ib_srq *ofa_srq)
{
	return container_of(ofa_srq, struct siw_srq, ofa_srq);
}

static u32 siw_insert_uobj(struct siw_ucontext *uctx, void *vaddr, u32 size)
{
	struct siw_uobj *uobj;
	u32	key = SIW_INVAL_UOBJ_KEY;

	uobj = kzalloc(sizeof *uobj, GFP_KERNEL);
	if (!uobj)
		goto out;

	size = PAGE_ALIGN(size);

	spin_lock(&uctx->uobj_lock);

	if (list_empty(&uctx->uobj_list))
		uctx->uobj_key = 0;

	key = uctx->uobj_key;

	uobj->key = uctx->uobj_key;
	uctx->uobj_key += size; /* advance for next object */

	if (key > SIW_MAX_UOBJ_KEY) {
		uctx->uobj_key -= size;
		key = SIW_INVAL_UOBJ_KEY;
		kfree (uobj);
		goto out;
	}
	uobj->size = size;
	uobj->addr = vaddr;

	list_add_tail(&uobj->list, &uctx->uobj_list);

	spin_unlock(&uctx->uobj_lock);
out:
	return key;
}

static struct siw_uobj *        
siw_remove_uobj(struct siw_ucontext *uctx, u32 key, u32 size)
{       
	struct list_head *pos, *nxt;

	spin_lock(&uctx->uobj_lock);

	list_for_each_safe(pos, nxt, &uctx->uobj_list) {
		struct siw_uobj *uobj = list_entry(pos, struct siw_uobj, list);
		if (uobj->key == key && uobj->size == size) {
			list_del(&uobj->list);
			spin_unlock(&uctx->uobj_lock);
			return uobj;
		}
	}
	spin_unlock(&uctx->uobj_lock);

	return NULL;
}

int     
siw_mmap(struct ib_ucontext *ctx, struct vm_area_struct *vma)
{       
	struct siw_ucontext     *uctx = siw_ctx_ofa2siw(ctx);
	struct siw_uobj         *uobj;
	u32     key = vma->vm_pgoff << PAGE_SHIFT;
	int     size = vma->vm_end - vma->vm_start;

	int     rv = -EINVAL;

	/*
	* Must be page aligned
	*/
	if (vma->vm_start & (PAGE_SIZE - 1)) {
		pr_warn("map not page aligned\n");
		goto out;
	}

	uobj = siw_remove_uobj(uctx, key, size);
	if (!uobj) {
		pr_warn("mmap lookup failed: %u, %d\n", key, size);
		goto out;
	}
	rv = remap_vmalloc_range(vma, uobj->addr, 0);
	if (rv)
		pr_warn("remap_vmalloc_range failed: %u, %d\n", key, size);

	kfree(uobj);
out:
	return rv;
}

#if KS_IB_DEVICE_ALLOC_USES_UCONTEXT

static inline struct siw_dev *to_siw_dev(struct ib_device *base_dev)
{
	return container_of(base_dev, struct siw_dev, ofa_dev);
}

static inline struct siw_ucontext *to_siw_ctx(struct ib_ucontext *base_ctx)
{
	return container_of(base_ctx, struct siw_ucontext, ib_ucontext);
}

int siw_alloc_ucontext(struct ib_ucontext *base_ctx, struct ib_udata *udata)
{
	struct siw_dev *sdev = to_siw_dev(base_ctx->device);
	struct siw_ucontext *ctx = to_siw_ctx(base_ctx);
	int rv;

	dprint(DBG_CM, "(device=%s)\n", sdev->ofa_dev.name);

	if (atomic_inc_return(&sdev->num_ctx) > SIW_MAX_CONTEXT) {
		dprint(DBG_ON, ": Out of CONTEXT's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	spin_lock_init(&ctx->uobj_lock);
	INIT_LIST_HEAD(&ctx->uobj_list);
	ctx->uobj_key = 0;

	ctx->sdev = sdev;
	if (udata) {
		struct siw_uresp_alloc_ctx uresp;

		memset(&uresp, 0, sizeof uresp);
		uresp.dev_id = sdev->attrs.vendor_part_id;
#ifdef SIW_DB_SYSCALL
		uresp.rdma_db_nr = __NR_rdma_db;
#else
		uresp.rdma_db_nr = -1;
#endif

		rv = ib_copy_to_udata(udata, &uresp, sizeof uresp);
		if (rv)
			goto err_out;
	}
	return 0;

err_out:

	atomic_dec(&sdev->num_ctx);
	return rv;
}

void siw_dealloc_ucontext(struct ib_ucontext *base_ctx)
{
	struct siw_ucontext *uctx = to_siw_ctx(base_ctx);

	atomic_dec(&uctx->sdev->num_ctx);
	/*no kfree*/
}

#else /*if !KS_IB_DEVICE_OPS_USES_UCONTEXT */

struct ib_ucontext *siw_alloc_ucontext(struct ib_device *ofa_dev,
				       struct ib_udata *udata)
{
	struct siw_ucontext *ctx = NULL;
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_dev);
	int rv;

	dprint(DBG_CM, "(device=%s)\n", ofa_dev->name);

	if (atomic_inc_return(&sdev->num_ctx) > SIW_MAX_CONTEXT) {
		dprint(DBG_ON, ": Out of CONTEXT's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		rv = -ENOMEM;
		goto err_out;
	}
	spin_lock_init(&ctx->uobj_lock);
	INIT_LIST_HEAD(&ctx->uobj_list);
	ctx->uobj_key = 0;

	ctx->sdev = sdev;
	if (udata) {
		struct siw_uresp_alloc_ctx uresp;

		memset(&uresp, 0, sizeof uresp);
		uresp.dev_id = sdev->attrs.vendor_part_id;
#ifdef SIW_DB_SYSCALL
		uresp.rdma_db_nr = __NR_rdma_db;
#else
		uresp.rdma_db_nr = -1;
#endif

		rv = ib_copy_to_udata(udata, &uresp, sizeof uresp);
		if (rv)
			goto err_out;
	}
	return &ctx->ib_ucontext;

err_out:
	if (ctx)
		kfree(ctx);

	atomic_dec(&sdev->num_ctx);
	return ERR_PTR(rv);
}

int siw_dealloc_ucontext(struct ib_ucontext *ofa_ctx)
{
	struct siw_ucontext *ctx = siw_ctx_ofa2siw(ofa_ctx);

	atomic_dec(&ctx->sdev->num_ctx);
	kfree(ctx);
	return 0;
}

#endif /* KS_IB_DEVICE_OPS_USES_UCONTEXT */

int siw_query_device(struct ib_device *ofa_dev, struct ib_device_attr *attr,
		     struct ib_udata *unused)
{
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_dev);
	/*
	 * A process context is needed to report avail memory resources.
	 */
	if (in_interrupt())
		return -EINVAL;

	memset(attr, 0, sizeof *attr);

	attr->fw_ver = ((u64)SIW_VENDOR_ID << 32) | VERSION_ID_SOFTIWARP;
	attr->max_mr_size = sdev->attrs.max_mr_size;
	attr->vendor_id = sdev->attrs.vendor_id;
	attr->vendor_part_id = sdev->attrs.vendor_part_id;
	attr->max_qp = sdev->attrs.max_qp;
	attr->max_qp_wr = sdev->attrs.max_qp_wr;

	/*
	 * RDMA Read parameters:
	 * Max. ORD (Outbound Read queue Depth), a.k.a. max_initiator_depth
	 * Max. IRD (Inbound Read queue Depth), a.k.a. max_responder_resources
	 */
	attr->max_qp_rd_atom = sdev->attrs.max_ord;
	attr->max_qp_init_rd_atom = sdev->attrs.max_ird;
	attr->max_res_rd_atom = sdev->attrs.max_qp * sdev->attrs.max_ird;
	attr->device_cap_flags = sdev->attrs.cap_flags;
#if KS_IB_DEVICE_ATTR_HAS_MAX_SEND_SGE
	attr->max_send_sge = sdev->attrs.max_sge;
	attr->max_recv_sge = sdev->attrs.max_sge;
#else
	attr->max_sge = sdev->attrs.max_sge;
#endif
	attr->max_sge_rd = sdev->attrs.max_sge_rd;
	attr->max_cq = sdev->attrs.max_cq;
	attr->max_cqe = sdev->attrs.max_cqe;
	attr->max_mr = sdev->attrs.max_mr;
	attr->max_pd = sdev->attrs.max_pd;
	attr->max_mw = sdev->attrs.max_mw;
#if KS_IB_HAS_FMR
	attr->max_fmr = sdev->attrs.max_fmr;
#endif

	attr->max_srq = sdev->attrs.max_srq;
	attr->max_srq_wr = sdev->attrs.max_srq_wr;
	attr->max_srq_sge = sdev->attrs.max_srq_sge;
	attr->max_fast_reg_page_list_len = SIW_MAX_SGE_PBL;

	memcpy(&attr->sys_image_guid, sdev->netdev->dev_addr, 6);

	attr->atomic_cap = SIW_ATOMIC_CAP;

	attr->page_size_cap = PAGE_SIZE;

	/*
	 * TODO: understand what of the following should
	 * get useful information
	 *
	 * attr->fw_ver;
	 * attr->max_ah
	 * attr->max_map_per_fmr
	 * attr->max_ee
	 * attr->max_rdd
	 * attr->max_ee_rd_atom;
	 * attr->max_ee_init_rd_atom;
	 * attr->max_raw_ipv6_qp
	 * attr->max_raw_ethy_qp
	 * attr->max_mcast_grp
	 * attr->max_mcast_qp_attach
	 * attr->max_total_mcast_qp_attach
	 * attr->max_pkeys
	 * attr->atomic_cap;
	 * attr->page_size_cap; //omril: NVMesh uses minimum of 4096 (value is set during init and when mapping )
	 * attr->hw_ver;
	 * attr->local_ca_ack_delay;
	 */
	return 0;
}

/*
 * Approximate translation of real MTU for IB.
 *
 * TODO: is that needed for RNIC's? We may have a medium
 *       which reports MTU of 64kb and have to degrade to 4k??
 */
static inline enum ib_mtu siw_mtu_net2ofa(unsigned short mtu)
{
	if (mtu >= 4096)
		return IB_MTU_4096;
	if (mtu >= 2048)
		return IB_MTU_2048;
	if (mtu >= 1024)
		return IB_MTU_1024;
	if (mtu >= 512)
		return IB_MTU_512;
	if (mtu >= 256)
		return IB_MTU_256;
	return IB_MTU_4096;
}

int siw_query_port(struct ib_device *ofa_dev, t_ib_port port,
		     struct ib_port_attr *attr)
{
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_dev);
	bool	carrier_ok = netif_running(sdev->netdev) && netif_carrier_ok(sdev->netdev);

	memset(attr, 0, sizeof *attr);

	/* Same behavior as ROCE, if operstate/carrier is down we consider the state as down 
	   Also consider an option we didn't finish handle the NETDEV_UP event so we are still in 
	   IB_PORT_INIT but ndev already up */
	if ((sdev->state == IB_PORT_ACTIVE || (sdev->netdev->flags & IFF_UP ||
			 sdev->state == IB_PORT_INIT)) && !carrier_ok) {
			attr->state = IB_PORT_DOWN;
	} else {
			attr->state = sdev->state;
	} 
	attr->max_mtu = siw_mtu_net2ofa(sdev->netdev->mtu);
	attr->active_mtu = attr->max_mtu;
	attr->gid_tbl_len = 3; /* 1 for HW GID (MAC), 1 for SW GID (IPv4) and 1 for SW GID (IPv6) */
	attr->port_cap_flags = IB_PORT_CM_SUP;	/* ?? */
	attr->port_cap_flags |= IB_PORT_DEVICE_MGMT_SUP;
	attr->max_msg_sz = -1;
	attr->pkey_tbl_len = 1;
	attr->active_width = 2;
	attr->active_speed = 2;
	attr->phys_state = sdev->state == IB_PORT_ACTIVE ? 5 : 3;
	/*
	 * All zero
	 *
	 * attr->lid = 0;
	 * attr->bad_pkey_cntr = 0;
	 * attr->qkey_viol_cntr = 0;
	 * attr->sm_lid = 0;
	 * attr->lmc = 0;
	 * attr->max_vl_num = 0;
	 * attr->sm_sl = 0;
	 * attr->subnet_timeout = 0;
	 * attr->init_type_repy = 0;
	 */
	return 0;
}

#if KS_IB_DEVICE_ATTR_HAS_GET_PORT_IMMUTABLE
int siw_get_port_immutable(struct ib_device *ofa_dev, t_ib_port port,
			   struct ib_port_immutable *port_immutable)
{
	struct ib_port_attr attr;

	/*
	 * Gregory: why to query port ?
	 * this is software emulation - we already know it's settings
	 */
	int rv = siw_query_port(ofa_dev, port, &attr);
	if (rv)
		return rv;

	port_immutable->pkey_tbl_len = attr.pkey_tbl_len;
	port_immutable->gid_tbl_len = attr.gid_tbl_len;
	port_immutable->core_cap_flags = RDMA_CORE_PORT_IWARP;

	return 0;
}
#endif

int siw_query_pkey(struct ib_device *ofa_dev, t_ib_port port, u16 idx, u16 *pkey)
{
	/* Report the default pkey */
	*pkey = 0xffff;
	return 0;
}

int siw_query_gid(struct ib_device *ofa_dev, t_ib_port port, int idx,
		   union ib_gid *gid)
{
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_dev);
	struct net_device *netdev = sdev->netdev;
	struct in_device *in_dev = NULL;
	struct in_ifaddr *ifa;
	struct inet6_dev *in6_dev = NULL;
	struct inet6_ifaddr *ifp;
	int rv, gid_count = 0;

	if (idx == gid_count++) {
		/* HW-GID - Combine VLAN and MAC address into an IPv6 address with local prefix */
		u8 *eui48 = (u8 *)&gid->global.interface_id;
		u16 vlan_id = 0;

		if (netdev->addr_len != ETH_ALEN) {
			dprint(DBG_DM, "%s [%d]: not an ethernet device",
			       netdev->name, idx);
			goto zgid;
		}

		/* Set the local subnet prefix */
		gid->global.subnet_prefix = cpu_to_be64(0xfe80000000000000LL);

		/* Set the interface id from the MAC using EUI-48 standard */
		addrconf_ifid_eui48(eui48, netdev);

		if (is_vlan_dev(netdev)) {
			vlan_id = vlan_dev_vlan_id(netdev);
			/* Set vlan_id in unused bytes 3 and 4 */
			eui48[3] = (vlan_id >> 8) & 0xFF;  // High byte of VLAN ID
			eui48[4] = vlan_id & 0xFF;         // Low byte of VLAN ID
		}

		dprint(DBG_DM, "%s [%d]: return MAC " dprint_mac_str() 
				", VLAN %u as HW GID " dprint_ip6_str() "\n",
				netdev->name, idx, dprint_mac_param(netdev->dev_addr), 
				vlan_id, dprint_ip6_param(*(struct in6_addr *)&gid));
		rv = 0;
		goto out;
	}

	if (!(in_dev = in_dev_get(netdev))) {
		dprint(DBG_DM, ": %s: no in_dev\n", netdev->name);
		goto ipv6;
	}

	/* Loop over all ipv4 addr until idx == gid_count */
	for (ifa = rtnl_dereference(in_dev->ifa_list); ifa != NULL; ifa = rtnl_dereference(ifa->ifa_next), gid_count++) {
		if (idx == gid_count) {
			__be32 in_addr = ifa->ifa_local;
			dprint(DBG_DM, "[%d]: return IPv4 addr " dprint_ip4_str() " as GID " dprint_ip6_str() "\n", 
			       idx, dprint_ip4_param(in_addr), dprint_ip6_param(*(struct in6_addr *)&gid));
			
			gid->global.subnet_prefix = 0;
			gid->global.interface_id = cpu_to_be64(((uint64_t)0xffff << (sizeof(in_addr) << 3)) | (uint64_t)be32_to_cpu(in_addr));

			rv = 0;
			goto out;
		}
	}

ipv6:
	if (!(in6_dev = in6_dev_get(sdev->netdev))) {
		dprint(DBG_DM, ": %s: no in6_dev\n", netdev->name);
		goto zgid;
	}

	list_for_each_entry(ifp, &in6_dev->addr_list, if_list) {
		/* Skip deprecated addresses */
		if (ifp->flags & IFA_F_DEPRECATED) {
			dprint(DBG_DM, "(dev %s): skipping IPv6 addr " dprint_ip6_str() " with flags %x\n", 
				netdev->name, dprint_ip6_param(ifp->addr), ifp->flags);
			continue;
		}
		/* Skip link-local and loopback (not global) and multicast (does not support TCP) addresses */
		if (ipv6_addr_type(&ifp->addr) & (IPV6_ADDR_LINKLOCAL | IPV6_ADDR_LOOPBACK | IPV6_ADDR_MULTICAST)) {
			dprint(DBG_DM, "(dev %s): skipping non-global IPv6 addr " dprint_ip6_str() " with flags %x\n", 
				netdev->name, dprint_ip6_param(ifp->addr), ifp->flags);
			continue;
		}
		if (idx == gid_count++) {
			dprint(DBG_DM, "[%d]: return IPv6 " dprint_ip6_str() " as GID\n", idx,  dprint_ip6_param(ifp->addr));

			*(struct in6_addr *)&gid->raw = ifp->addr;
			
			rv = 0;
			goto out;
		}
	}
	dprint(DBG_OL, "[%d]: no GID found\n", idx);

zgid:
	/* subnet_prefix == interface_id == 0; */
	*gid = zgid;

	rv = 0;

out:
	if (in_dev)
		in_dev_put(in_dev);
	if (in6_dev)
		in6_dev_put(in6_dev);
	return rv;
}

struct net_device *siw_get_netdev(struct ib_device *ofa_dev, t_ib_port port)
{
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_dev);
	struct net_device *netdev = sdev->netdev;
	dev_hold(netdev);
	return netdev;
}

#if !KS_IB_DEVICE_PD_USES_UCONTEXT
int siw_alloc_pd(struct ib_pd *ofa_pd, struct ib_udata *udata)
{
	struct siw_pd *pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev *sdev = to_siw_dev(ofa_pd->device);

	int rv;
	int n;

	(void)udata;

	if ((n = atomic_inc_return(&sdev->num_pd)) > SIW_MAX_PD) {
		dprint(DBG_ON, ": Out of PD's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	rv = siw_pd_add(sdev, pd);
	if (rv) {
		dprint(DBG_ON, ": siw_pd_add\n");
		rv = -ENOMEM;
		goto err_out;
	}
	dprint(DBG_OBJ|DBG_OL, "inc num_pd to %d\n", n);
	return 0;

err_out:
	atomic_dec(&sdev->num_pd);

	return rv;
}

#if KS_IB_DEALLOC_PD_INT_RETURN
int siw_dealloc_pd(struct ib_pd *ofa_pd, struct ib_udata *udata)
{
	struct siw_pd *pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_pd->device);

	siw_remove_obj(&sdev->idr_lock, &sdev->pd_idr, &pd->hdr);
	siw_pd_put(pd);

	(void)udata;

	return 0;
}
#else
void siw_dealloc_pd(struct ib_pd *ofa_pd, struct ib_udata *udata)
{
	struct siw_pd *pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev *sdev = siw_dev_ofa2siw(ofa_pd->device);

	siw_remove_obj(&sdev->idr_lock, &sdev->pd_idr, &pd->hdr);
	siw_pd_put(pd);

	(void)udata;
}
#endif
#else
struct ib_pd *siw_alloc_pd(struct ib_device *ofa_dev,
			   struct ib_ucontext *context, struct ib_udata *udata)
{
	struct siw_pd	*pd = NULL;
	struct siw_dev	*sdev  = siw_dev_ofa2siw(ofa_dev);
	int rv;
	int n;

	if ((n = atomic_inc_return(&sdev->num_pd)) > SIW_MAX_PD) {
		dprint(DBG_ON, ": Out of PD's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	pd = kmalloc(sizeof *pd, GFP_KERNEL);
	if (!pd) {
		dprint(DBG_ON, ": malloc\n");
		rv = -ENOMEM;
		goto err_out;
	}
	rv = siw_pd_add(sdev, pd);
	if (rv) {
		dprint(DBG_ON, ": siw_pd_add\n");
		rv = -ENOMEM;
		goto err_out;
	}
	if (context) {
		if (ib_copy_to_udata(udata, &pd->hdr.id, sizeof pd->hdr.id)) {
			rv = -EFAULT;
			goto err_out_idr;
		}
	}

	dprint(DBG_OBJ|DBG_OL, "inc num_pd to %d\n", n);
	return &pd->ofa_pd;

err_out_idr:
	siw_remove_obj(&sdev->idr_lock, &sdev->pd_idr, &pd->hdr);
err_out:
	kfree(pd);
	atomic_dec(&sdev->num_pd);

	return ERR_PTR(rv);
}

int siw_dealloc_pd(struct ib_pd *ofa_pd)
{
	struct siw_pd	*pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev	*sdev = siw_dev_ofa2siw(ofa_pd->device);

	siw_remove_obj(&sdev->idr_lock, &sdev->pd_idr, &pd->hdr);
	siw_pd_put(pd);

	return 0;
}
#endif

DECLARE_SIW_CREATE_AH(siw_create_ah)
{
#if KS_IB_CREATE_AH_HAS_AH
	return -ENOSYS;
#else
	return ERR_PTR(-ENOSYS);
#endif
}

DECLARE_SIW_DESTROY_AH(siw_destroy_ah)
{
#if KS_IB_DESTROY_AH_RETURNS_INT
	return -ENOSYS;
#endif
}

void siw_qp_get_ref(struct ib_qp *ofa_qp)
{
	struct siw_qp	*qp = siw_qp_ofa2siw(ofa_qp);

	dprint(DBG_OBJ|DBG_CM, "(QP%d): Get Reference\n", QP_ID(qp));
	siw_qp_get(qp);
}

extern bool connect_non_block;
void siw_qp_put_ref(struct ib_qp *ofa_qp)
{
	struct siw_qp	*qp = siw_qp_ofa2siw(ofa_qp);

	dprint(DBG_OBJ|DBG_CM, "(QP%d): Put Reference\n", QP_ID(qp));
	siw_qp_put(qp);
}

#if !KS_PROCESS_MAD_HAS_OUT_MAD_PKEY_INDEX
int siw_no_mad(struct ib_device *ofa_dev, int flags, t_ib_port port,
	       struct ib_wc *wc, struct ib_grh *grh,
	       struct ib_mad *in_mad, struct ib_mad *out_mad)
#elif KS_PROCESS_MAD_HAS_IB_MAD_HDR
int siw_no_mad(struct ib_device *ofa_dev, int flags, t_ib_port port,
	       const struct ib_wc *wc, const struct ib_grh *grh,
	       const struct ib_mad_hdr *in_mad, size_t in_mad_size,
	       struct ib_mad_hdr *out_mad, size_t *out_mad_size,
	       u16 *outmad_pkey_index)
#else
int siw_no_mad(struct ib_device *device, int process_mad_flags,
	       t_ib_port port_num, const struct ib_wc *in_wc,
		   const struct ib_grh *in_grh,
		   const struct ib_mad *in_mad, struct ib_mad *out_mad,
		   size_t *out_mad_size, u16 *out_mad_pkey_index)
#endif
{
	return -ENOSYS;
}

#ifdef USE_SQ_KTHREAD
extern int qp_tx_vector_cpu[];
extern int num_tx_vector;
#endif


/*
 * siw_create_qp()
 *
 * Create QP of requested size on given device.
 *
 * @ofa_pd:	OFA PD contained in siw PD
 * @attrs:	Initial QP attributes.
 * @udata:	used to provide QP ID, SQ and RQ size back to user.
 */
#if KS_IB_CREATE_QP_INT_RV
#define SIW_CREATE_QP_EFAULT -EFAULT
#else
#define SIW_CREATE_QP_EFAULT ERR_PTR(-EFAULT)
#endif

#define RETURN_IF_NULL(_p_)							\
do { 												\
	if (!_p_) {										\
		dprint(DBG_OL, "--- OOPS: %s---\n", #_p_);	\
		return SIW_CREATE_QP_EFAULT;				\
	}												\
} while (0)


#if KS_IB_CREATE_QP_INT_RV //add proper backport entry for IB_DEVICE_OPS
int siw_create_qp(struct ib_qp *ibqp,
			    struct ib_qp_init_attr *attrs,
			    struct ib_udata *udata)
{
	struct siw_qp *qp = to_siw_qp(ibqp);
	struct ib_pd *ofa_pd = ibqp->pd;
#else
struct ib_qp* siw_create_qp(struct ib_pd *ofa_pd,
			    struct ib_qp_init_attr *attrs,
			    struct ib_udata *udata)
{
	struct siw_qp			*qp = NULL;
#endif

	struct siw_pd			*pd = siw_pd_ofa2siw(ofa_pd);
	struct ib_device		*ofa_dev = ofa_pd->device;
	struct siw_dev			*sdev = siw_dev_ofa2siw(ofa_dev);
	struct siw_cq			*scq = NULL, *rcq = NULL;

	unsigned long flags;
	int num_sqe, num_rqe, rv = 0;

	dprint(DBG_OL, "-->\n");

	dprint(DBG_OBJ|DBG_CM, ": new QP on device %s\n",
		ofa_dev->name);

	dprint(DBG_OL, "---\n");
	RETURN_IF_NULL(sdev);
	RETURN_IF_NULL(attrs);
	RETURN_IF_NULL(attrs->send_cq);
	RETURN_IF_NULL(attrs->recv_cq);


	dprint(DBG_OL, "---\n");
	if (atomic_inc_return(&sdev->num_qp) > SIW_MAX_QP) {
		dprint(DBG_ON, ": Out of QP's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	dprint(DBG_OL, "---\n");
	if (attrs->qp_type != IB_QPT_RC) {
		dprint(DBG_ON, ": Only RC QP's supported\n");
		rv = -EINVAL;
		goto err_out;
	}
	dprint(DBG_OL, "---\n");
	if (attrs->cap.max_send_wr > SIW_MAX_QP_WR) {
		dprint(DBG_ON, "attrs->cap.max_send_wr=%u > SIW_MAX_QP_WR=%d\n",
			attrs->cap.max_send_wr, SIW_MAX_QP_WR);
		rv = -EINVAL;
		goto err_out;
	}
	if (attrs->cap.max_send_wr == 0) {
		dprint(DBG_ON, "cannot have zero-length send queue\n");
		rv = -EINVAL;
		goto err_out;
	}
	if (attrs->cap.max_recv_wr > SIW_MAX_QP_WR) {
		dprint(DBG_ON, "attrs->cap.max_recv_wr=%u > SIW_MAX_QP_WR=%d\n",
			attrs->cap.max_recv_wr,  SIW_MAX_QP_WR);
		rv = -EINVAL;
		goto err_out;
	}
	if (attrs->cap.max_send_sge > SIW_MAX_SGE) {
		dprint(DBG_ON, "attrs->cap.max_send_sge=%u > SIW_MAX_SGE=%d\n",
			attrs->cap.max_send_sge, SIW_MAX_SGE);
		rv = -EINVAL;
		goto err_out;
	}
	if (attrs->cap.max_recv_sge > SIW_MAX_SGE) {
		dprint(DBG_ON, "attrs->cap.max_recv_sge=%u > SIW_MAX_SGE=%d\n",
			attrs->cap.max_recv_sge, SIW_MAX_SGE);
		rv = -EINVAL;
		goto err_out;
	}
	if (attrs->cap.max_inline_data > SIW_MAX_INLINE) {
		dprint(DBG_ON, ": attrs->cap.max_inline_data=%u > SIW_MAX_INLINE=%d!\n",
		       attrs->cap.max_inline_data, (int)SIW_MAX_INLINE);
		rv = -EINVAL;
		goto err_out;
	}
	/*
	 * NOTE: we allow for zero element SQ and RQ WQE's SGL's
	 * but not for a QP unable to hold any WQE (SQ + RQ)
	 */
	dprint(DBG_OL, "---\n");
	if (attrs->cap.max_send_wr + attrs->cap.max_recv_wr == 0) {
		rv = -EINVAL;
		goto err_out;
	}

	dprint(DBG_OL, "---\n");
	scq = siw_cq_id2obj(sdev, ((struct siw_cq *)attrs->send_cq)->hdr.id);
	dprint(DBG_OL, "---\n");
	rcq = siw_cq_id2obj(sdev, ((struct siw_cq *)attrs->recv_cq)->hdr.id);

	dprint(DBG_OL, "---\n");
	if (!scq || (!rcq && !attrs->srq)) {
		dprint(DBG_OBJ, ": Fail: SCQ: " dprint_ptr_str() ", RCQ: " dprint_ptr_str() "\n",
			scq, rcq);
		rv = -EINVAL;
		goto err_out;
	}
	dprint(DBG_OL, "---\n");
#if !KS_IB_CREATE_QP_INT_RV
	qp = kzalloc(sizeof *qp, GFP_KERNEL);
#endif
	if (!qp) {
		dprint(DBG_ON, ": kzalloc\n");
		rv = -ENOMEM;
		goto err_out;
	}

	dprint(DBG_OL, "---\n");
	init_rwsem(&qp->state_lock);
	spin_lock_init(&qp->sq_lock);
	spin_lock_init(&qp->rq_lock);
	spin_lock_init(&qp->orq_lock);
	atomic_set(&qp->state_lock_failed, 0);

	init_waitqueue_head(&qp->tx_ctx.waitq);

	INIT_WORK(&qp->cq_notify_work, siw_qp_notify_work);

#ifdef SIW_TX_COMP_WAIT_ACK
	INIT_LIST_HEAD(&qp->tx_ctx.sent_fpdus);
	atomic_set(&qp->tx_ctx.n_completed_fpdus, 0);
	INIT_LIST_HEAD(&qp->tx_ctx.completed_fpdus);
#endif

#if KS_IB_DEVICE_OPS_HAS_QP_SIZE
	init_completion(&qp->free_comp);
#endif

	INIT_DELAYED_WORK(&qp->rx_ctx.rx_work, siw_rx_work_handler);
	INIT_LIST_HEAD(&qp->srq_wait_link);
	qp->srq_rqe_ready.flags = 0;
	
#if SIW_SRQ_RQE_TRACK
	INIT_LIST_HEAD(&qp->srq_rqe_link);
#endif

	INIT_DELAYED_WORK(&qp->flush_work, siw_flush_xqes_work);
	INIT_LIST_HEAD(&qp->tx_ctx.flush_sqes);
	INIT_LIST_HEAD(&qp->rx_ctx.flush_rqes);

	atomic_set(&qp->tx_ctx.scq_qp_ref_cnt, 1);
	atomic_set(&qp->rx_ctx.rcq_qp_ref_cnt, 1);

	if (!ofa_pd->uobject)
		qp->kernel_verbs = 1;

	dprint(DBG_OL, "---\n");
	rv = siw_qp_add(sdev, qp);
	if (rv)
		goto err_out;

	dprint(DBG_OL, "---\n");
	/* attrs->cap.max_send_wr == 0 is not allowed, but attrs->cap.max_recv_wr == 0 is,
	 * roundup_pow_of_two(0) is undefined so we must protect against it */
	num_sqe = roundup_pow_of_two(attrs->cap.max_send_wr);
	num_rqe = attrs->cap.max_recv_wr > 0 ? roundup_pow_of_two(attrs->cap.max_recv_wr) : 0;
	
	if (qp->kernel_verbs) {
		dprint(DBG_OL, "---\n");
		if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
			qp->sendq = (void *)__get_free_pages(GFP_KERNEL, get_order(num_sqe * sizeof(struct siw_sqe)));
		else
			qp->sendq = vmalloc(num_sqe * sizeof(struct siw_sqe));
	} else {
		dprint(DBG_OL, "---\n");
		qp->sendq = vmalloc_user(num_sqe * sizeof(struct siw_sqe));
	}

	if (qp->sendq == NULL) {
		pr_warn("QP(%d): send queue size %d alloc failed\n",
			QP_ID(qp), num_sqe);
		rv = -ENOMEM;
		goto err_out_idr;
	}
	dprint(DBG_OL, "---\n");
	if (attrs->sq_sig_type != IB_SIGNAL_REQ_WR) {
		if (attrs->sq_sig_type == IB_SIGNAL_ALL_WR)
			qp->attrs.flags |= SIW_SIGNAL_ALL_WR;
		else {
			rv = -EINVAL;
			goto err_out_idr;
		}
	}

	qp->pd  = pd;
	qp->scq = scq;
	qp->rcq = rcq;

	dprint(DBG_OL, "---\n");
	if (attrs->srq) {
		/*
		 * SRQ support.
		 * Verbs 6.3.7: ignore RQ size, if SRQ present
		 * Verbs 6.3.5: do not check PD of SRQ against PD of QP
		 */
		dprint(DBG_OL, "---\n");
		qp->srq = siw_srq_ofa2siw(attrs->srq);

#if SIW_SRQ_RQE_TRACK
		do {
			unsigned long flags;
			lock_srq_rxsave(qp->srq, flags);
			qp->srq->rqe_track.n_qp++;
			unlock_srq_rxsave(qp->srq, flags);
		} while(0);
#endif

		qp->attrs.rq_size = 0;
		dprint(DBG_OBJ | DBG_OL, " QP(%d): SRQ(" dprint_ptr_str() ") attached\n",
			QP_ID(qp), qp->srq);
	} else if (num_rqe) {
		dprint(DBG_OL, "---\n");
		qp->srq = NULL;

		if (qp->kernel_verbs) {
			dprint(DBG_OL, "---\n");
			if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
				qp->recvq = (void *)__get_free_pages(GFP_KERNEL, get_order(num_rqe * sizeof(struct siw_rqe)));
			else
				qp->recvq = vmalloc(num_rqe * sizeof(struct siw_rqe));
		} else {
			dprint(DBG_OL, "---\n");
			qp->recvq = vmalloc_user(num_rqe *
						 sizeof(struct siw_rqe));
		}

		if (qp->recvq == NULL) {
			pr_warn("QP(%d): recv queue size %d alloc failed\n",
				QP_ID(qp), num_rqe);
			rv = -ENOMEM;
			goto err_out_idr;
		}

		qp->attrs.rq_size = num_rqe;
	}
	qp->attrs.sq_size = num_sqe;
	qp->attrs.sq_max_sges = attrs->cap.max_send_sge;
	/*
	 * ofed has no max_send_sge_rdmawrite
	 */
	qp->attrs.sq_max_sges_rdmaw = attrs->cap.max_send_sge;
	qp->attrs.rq_max_sges = attrs->cap.max_recv_sge;

	qp->attrs.state = SIW_QP_STATE_IDLE;

	if (qp->kernel_verbs && num_sqe) /* vmalloc_user already zeroes mem */ {
		dprint(DBG_OL, "---\n");
		memset(qp->sendq, 0, num_sqe * sizeof(struct siw_sqe));
	}
	if (qp->kernel_verbs && num_rqe) /* vmalloc_user already zeroes mem */ {
		if (qp->srq) {
			dprint(DBG_OL, "--- omril: looks like we dont need to init qp->recvq if we use srq - SKIPPING!!!\n");
			//omril: hopefully we wont deref qp->recvq or qp->attrs.rq_size if we using SRQ.
		}
		else {
			memset(qp->recvq, 0, num_rqe * sizeof(struct siw_rqe));
		}
	}

	if (udata) {
		struct siw_uresp_create_qp uresp;
		struct siw_ucontext *ctx;

		dprint(DBG_OL, "---\n");
		memset(&uresp, 0, sizeof uresp);
		ctx = siw_ctx_ofa2siw(ofa_pd->uobject->context);

		uresp.sq_key = uresp.rq_key = SIW_INVAL_UOBJ_KEY;
		uresp.num_sqe = num_sqe;
		uresp.num_rqe = num_rqe;
		uresp.qp_id = QP_ID(qp);

		if (qp->sendq) {
			dprint(DBG_OL, "---\n");
			uresp.sq_key = siw_insert_uobj(ctx, qp->sendq,
					num_sqe * sizeof(struct siw_sqe));
			if (uresp.sq_key > SIW_MAX_UOBJ_KEY)
				pr_warn("Preparing mmap SQ failed\n");
		}
		if (qp->recvq) {
			dprint(DBG_OL, "---\n");
			uresp.rq_key = siw_insert_uobj(ctx, qp->recvq,
					num_rqe * sizeof(struct siw_rqe));
			if (uresp.rq_key > SIW_MAX_UOBJ_KEY)
				pr_warn("Preparing mmap RQ failed\n");
		}
		dprint(DBG_OL, "---\n");
		rv = ib_copy_to_udata(udata, &uresp, sizeof uresp);
		if (rv)
			goto err_out_idr;
	}
	dprint(DBG_OL, "---\n");
	atomic_set(&qp->tx_ctx.in_use, 0);

	qp->ofa_qp.qp_num = QP_ID(qp);

	siw_pd_get(pd);

	dprint(DBG_OL, "---\n");
	INIT_LIST_HEAD(&qp->devq);
	dprint(DBG_OL, "---\n");
	spin_lock_irqsave(&sdev->idr_lock, flags);
	dprint(DBG_OL, "---\n");
	list_add_tail(&qp->devq, &sdev->qp_list);
	dprint(DBG_OL, "---\n");
	spin_unlock_irqrestore(&sdev->idr_lock, flags);

	dprint(DBG_OL, "---\n");
	qp->cpu = (smp_processor_id() + 1) % NR_CPUS;

#ifdef USE_SQ_KTHREAD
	if (scq)
		qp->cpu = qp_tx_vector_cpu[scq->comp_vector % num_tx_vector];
#endif

	dprint(DBG_OL, "<--\n");
#if !KS_IB_CREATE_QP_INT_RV
	return &qp->ofa_qp;
#else
	return 0;
#endif

err_out_idr:
	dprint(DBG_OL, "---\n");
	siw_remove_obj(&sdev->idr_lock, &sdev->qp_idr, &qp->hdr);
err_out:
	dprint(DBG_OL, "---\n");
	if (scq)
		siw_cq_put(scq);
	if (rcq)
		siw_cq_put(rcq);

	if (qp) {
		if (qp->sendq) {
			if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
				free_pages((unsigned long)qp->sendq, get_order(num_sqe * sizeof(struct siw_sqe)));
			else
				vfree(qp->sendq);
		}
		if (qp->recvq) {
			if (SIW_NO_VMALLOC_FOR_KVERBS_QP)
				free_pages((unsigned long)qp->recvq, get_order(num_rqe * sizeof(struct siw_rqe)));
			else
				vfree(qp->recvq);
		}
	#if !KS_IB_DEVICE_OPS_HAS_QP_SIZE
		kfree(qp);
	#endif
	}
	atomic_dec(&sdev->num_qp);

#if !KS_IB_CREATE_QP_INT_RV
	return ERR_PTR(rv);
#else
	return rv;
#endif
}

/*
 * Minimum siw_query_qp() verb interface.
 *
 * @qp_attr_mask is not used but all available information is provided
 */
int siw_query_qp(struct ib_qp *ofa_qp, struct ib_qp_attr *qp_attr,
		 int qp_attr_mask, struct ib_qp_init_attr *qp_init_attr)
{
	struct siw_qp *qp;
	struct siw_dev *sdev;

	if (ofa_qp && qp_attr && qp_init_attr) {
		qp = siw_qp_ofa2siw(ofa_qp);
		sdev = siw_dev_ofa2siw(ofa_qp->device);
	} else
		return -EINVAL;

	qp_attr->qp_state = siw_2_ib_qp_state(qp->attrs.state);
	qp_attr->cap.max_inline_data = SIW_MAX_INLINE;
	qp_init_attr->cap.max_inline_data = SIW_MAX_INLINE;

	qp_attr->cap.max_send_wr = qp->attrs.sq_size;
	qp_attr->cap.max_recv_wr = qp->attrs.rq_size;
	qp_attr->cap.max_send_sge = qp->attrs.sq_max_sges;
	qp_attr->cap.max_recv_sge = qp->attrs.rq_max_sges;

	qp_attr->path_mtu = siw_mtu_net2ofa(sdev->netdev->mtu);
	qp_attr->max_rd_atomic = qp->attrs.irq_size;
	qp_attr->max_dest_rd_atomic = qp->attrs.orq_size;

	qp_attr->qp_access_flags = IB_ACCESS_LOCAL_WRITE |
			IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ;

	qp_init_attr->cap = qp_attr->cap;

	return 0;
}

int siw_ofed_modify_qp(struct ib_qp *ofa_qp, struct ib_qp_attr *attr,
		       int attr_mask, struct ib_udata *udata)
{
	struct siw_qp_attrs	new_attrs;
	enum siw_qp_attr_mask	siw_attr_mask = 0;
	struct siw_qp		*qp = siw_qp_ofa2siw(ofa_qp);
	int			rv = 0;

	dprint(DBG_OL, "-->\n");
	if (!attr_mask) {
		dprint(DBG_CM, "(QP%d): attr_mask==0 ignored\n", QP_ID(qp));
		goto out;
	}

	siw_dprint_qp_attr_mask(attr_mask);

	memset(&new_attrs, 0, sizeof new_attrs);

	if (attr_mask & IB_QP_ACCESS_FLAGS) {

		siw_attr_mask |= SIW_QP_ATTR_ACCESS_FLAGS;

		if (attr->qp_access_flags & IB_ACCESS_REMOTE_READ)
			new_attrs.flags |= SIW_RDMA_READ_ENABLED;
		if (attr->qp_access_flags & IB_ACCESS_REMOTE_WRITE)
			new_attrs.flags |= SIW_RDMA_WRITE_ENABLED;
		if (attr->qp_access_flags & IB_ACCESS_MW_BIND)
			new_attrs.flags |= SIW_RDMA_BIND_ENABLED;
	}
	if (attr_mask & IB_QP_STATE) {
		dprint(DBG_CM, "(QP%d)(QP=" dprint_ptr_str() "): Desired IB QP state: %s\n",
			   QP_ID(qp), qp, ib_qp_state_to_string[attr->qp_state]);

		new_attrs.state = ib_qp_state_to_siw_qp_state[attr->qp_state];

		if (new_attrs.state > SIW_QP_STATE_RTS) {
			unsigned long flags;
			/* Set the suspend flags so tx-thread (or WQ) and rx ctx will exit */
			lock_rq_rxsave(qp, flags);
			qp->rx_ctx.rx_suspend = 1;
			unlock_rq_rxsave(qp, flags);
			
			lock_sq_rxsave(qp, flags);
			qp->tx_ctx.tx_suspend = 1;
			unlock_sq_rxsave(qp, flags);
		}

		/* TODO: SIW_QP_STATE_UNDEF is currently not possible ... */
		if (new_attrs.state == SIW_QP_STATE_UNDEF) {
			dprint(DBG_OL, "<-- Err\n");
			return -EINVAL;
		}

		siw_attr_mask |= SIW_QP_ATTR_STATE;
	}
	if (!attr_mask) {
		dprint(DBG_OL, "<-- (rv=%d)\n", rv);
		goto out;
	}

	write_lock_qp(qp);

	rv = siw_qp_modify(qp, &new_attrs, siw_attr_mask);

	write_unlock_qp(qp);

out:
	dprint(DBG_CM, "(QP%d): Exit with %d\n", QP_ID(qp), rv);
	return rv;
}

#if KS_DESTROY_QP_HAS_UDATA
int siw_destroy_qp(struct ib_qp *ofa_qp, struct ib_udata *udata)
{
	struct siw_qp		*qp = siw_qp_ofa2siw(ofa_qp);
	struct siw_qp_attrs	qp_attrs;
	struct siw_xqe_flush *flush_xqe;
	int ref_cnt;
	unsigned long flags;
	(void)udata;
#else
int siw_destroy_qp(struct ib_qp *ofa_qp)
{
	struct siw_qp		*qp = siw_qp_ofa2siw(ofa_qp);
	struct siw_qp_attrs	qp_attrs;
	struct siw_xqe_flush *flush_xqe;
	int ref_cnt;
	unsigned long flags;
#endif

	dprint(DBG_CM|DBG_ON, "(QP%d): SIW QP state=%d, cep=" dprint_ptr_str() "\n",
		QP_ID(qp), qp->attrs.state, qp->cep);

	/* Ensure TX and RX are suspended */
	lock_rq_rxsave(qp, flags);
	qp->rx_ctx.rx_suspend = 1;
	unlock_rq_rxsave(qp, flags);

	lock_sq_rxsave(qp, flags);
	qp->tx_ctx.tx_suspend = 1;
	unlock_sq_rxsave(qp, flags);

	/* Move to QP to Error State (in case it isn't already) and then to Moribund state */
	write_lock_qp(qp);

	qp_attrs.state = SIW_QP_STATE_ERROR;
	(void)siw_qp_modify(qp, &qp_attrs, SIW_QP_ATTR_STATE);

	if (qp->cep) {
		siw_cep_put(qp->cep);
		qp->cep = NULL;
	}

	do {
		int state_lock_failed;
		if ((state_lock_failed = atomic_xchg(&qp->state_lock_failed, 0))) {
			dprint(DBG_CM|DBG_ON, "QP(%d): state_lock_failed %d for siw_qp " dprint_ptr_str() "\n",
					QP_ID(qp), state_lock_failed, qp);
			WARN_ON(1);
		}
	} while(0);
	write_unlock_qp(qp);
	
#if SIW_SRQ_RQE_TRACK
	BUG_ON(!list_empty(&qp->srq_rqe_link) || qp->srq_rqe_state != SRQ_RQE_IDLE);
#endif

	/* Flush CQ notifications from move to error state */
	flush_work(&qp->cq_notify_work);

	/*
	 * Set flag SIW_QP_IN_DESTROY to act as barrier for any pending cq_notify_work 
	 */
	write_lock_qp(qp);
	qp->attrs.flags |= SIW_QP_IN_DESTROY;
	write_unlock_qp(qp);

	/* Flush any CQ notify work in progress */
	flush_work(&qp->cq_notify_work);

	/* Flush any Flush XQEs work in progress */
	flush_delayed_work(&qp->flush_work);

	if ((ref_cnt = atomic_dec_return(&qp->tx_ctx.scq_qp_ref_cnt)) != 0) {
		WARN_ON_ONCE(1);
		dprint(DBG_ON | DBG_WR | DBG_CQ, "(QP:%d) Send CQ not drained. %d un-polled CQEs. "
			"qp_ptr=" dprint_ptr_str() " scq_ptr=" dprint_ptr_str() "\n", QP_ID(qp), ref_cnt, qp, qp->scq);
	}

	if ((ref_cnt = atomic_dec_return(&qp->rx_ctx.rcq_qp_ref_cnt)) != 0) {
		WARN_ON_ONCE(1);
		dprint(DBG_ON | DBG_WR | DBG_CQ, "(QP:%d) Recv CQ not drained. %d un-polled CQEs. "
			"qp_ptr=" dprint_ptr_str() " rcq_ptr=" dprint_ptr_str() "\n", QP_ID(qp), ref_cnt, qp, qp->rcq);
	}

	/* Free any Flush XQEs in the lists. No need to complete them to CQ as the QP is being destroyed anyway. */
	while ((flush_xqe = list_first_entry_or_null(&qp->tx_ctx.flush_sqes, struct siw_xqe_flush, link))) {
		WARN_ON_ONCE(1);
		dprint(DBG_ON | DBG_WR, "(QP:%d) QP destroyed with SQE with opcode %d ID: %llx still in flush list\n",
		       QP_ID(qp), flush_xqe->opcode, flush_xqe->id);
		list_del(&flush_xqe->link);
		kfree(flush_xqe);
	}

	while ((flush_xqe = list_first_entry_or_null(&qp->rx_ctx.flush_rqes, struct siw_xqe_flush, link))) {
		WARN_ON_ONCE(1);
		dprint(DBG_ON | DBG_WR, "(QP:%d) QP destroyed with RQE ID: %llx still in flush list\n",
		       QP_ID(qp), flush_xqe->id);
		list_del(&flush_xqe->link);
		kfree(flush_xqe);
	}

	if (qp->rx_ctx.mpa_crc_hd) {
		crypto_free_shash(qp->rx_ctx.mpa_crc_hd->tfm);
		kfree(qp->rx_ctx.mpa_crc_hd);
	}
	if (qp->tx_ctx.mpa_crc_hd) {
		crypto_free_shash(qp->tx_ctx.mpa_crc_hd->tfm);
		kfree(qp->tx_ctx.mpa_crc_hd);
	}
	if (qp->tx_ctx.trailer_page) {
		free_page((unsigned long)qp->tx_ctx.trailer_page_virt);
	}
	
#ifdef SIW_DEBUG_RX_CRC
	free_pages((unsigned long)qp->rx_ctx.curr_fpdu, 
			   get_order(SIW_DEBUG_RX_CRC_MAX_FPDU));
	free_pages((unsigned long)qp->rx_ctx.prev_fpdu, 
			   get_order(SIW_DEBUG_RX_CRC_MAX_FPDU));
	if (qp->rx_ctx.curr_fpdu_shash_desc) {
		crypto_free_shash(qp->rx_ctx.curr_fpdu_shash_desc->tfm);
		kfree(qp->rx_ctx.curr_fpdu_shash_desc);
	}
	free_pages((unsigned long)qp->rx_ctx.crc_trace_log,
			   get_order(SIW_DEBUG_RX_CRC_TRACE_LOG_SZ));
#endif

#if SIW_SRQ_RQE_TRACK
	if (qp->srq) {
		unsigned long flags;
		lock_srq_rxsave(qp->srq, flags);
		BUG_ON(qp->srq->rqe_track.n_qp == 0);
		qp->srq->rqe_track.n_qp--;
		unlock_srq_rxsave(qp->srq, flags);
	}
#endif

	/* Drop references */
	siw_cq_put(qp->scq);
	siw_cq_put(qp->rcq);
	siw_pd_put(qp->pd);
	
	qp->scq = qp->rcq = NULL;

	siw_qp_put(qp);
#if KS_IB_DEVICE_OPS_HAS_QP_SIZE
	wait_for_completion(&qp->free_comp);
	/* qp memory will be freed right after we return from this function */
#endif

	return 0;
}

/*
 * siw_copy_sgl()
 *
 * Copy SGL from OFA representation to local
 * representation.
 */
static inline void siw_copy_sgl(struct ib_sge *ofa_sge, struct siw_sge *siw_sge,
			       int num_sge)
{
	while (num_sge--) {
		siw_sge->laddr = ofa_sge->addr;
		siw_sge->length  = ofa_sge->length;
		siw_sge->lkey = ofa_sge->lkey;

		siw_sge++; ofa_sge++;
	}
}

/*
 * siw_copy_inline_sgl()
 *
 * Prepare sgl of inlined data for sending. For userland callers
 * function checks if given buffer addresses and len's are within
 * process context bounds.
 * Data from all provided sge's are copied together into the wqe,
 * referenced by a single sge.
 */
#if KS_POST_SEND_HAS_CONST
static int siw_copy_inline_sgl(const struct ib_send_wr *ofa_wr, struct siw_sqe *sqe)
#else
static int siw_copy_inline_sgl(struct ib_send_wr *ofa_wr, struct siw_sqe *sqe)
#endif
{
	struct ib_sge	*ofa_sge = ofa_wr->sg_list;
	void		*kbuf	 = &sqe->sge[1];
	int		num_sge	 = ofa_wr->num_sge,
			bytes	 = 0;

	sqe->sge[0].laddr = (u64)kbuf;
	sqe->sge[0].lkey = 0;

	while (num_sge--) {
		if (!ofa_sge->length) {
			ofa_sge++;
			continue;
		}
		bytes += ofa_sge->length;
		if (bytes > SIW_MAX_INLINE) {
			bytes = -EINVAL;
			break;
		}
		memcpy(kbuf, (void *)(uintptr_t)ofa_sge->addr, ofa_sge->length);

		kbuf += ofa_sge->length;
		ofa_sge++;
	}
	sqe->sge[0].length = bytes > 0 ? bytes : 0;
	sqe->num_sge = bytes > 0 ? 1 : 0;

	return bytes;
}

#ifdef SIW_DB_SYSCALL
extern long (*db_orig_call) (u32, u32, u32);
extern struct list_head siw_devlist;


static long siw_doorbell_sq(u32 dev_id, u32 qp_id)
{
	struct siw_qp *qp = NULL;
	int rv = 0;

	if (likely(qp_id)) {
		struct siw_dev *sdev = NULL;
		if (likely(!list_empty(&siw_devlist))) {
			struct list_head *pos;
			list_for_each(pos, &siw_devlist) {
				sdev = list_entry(pos, struct siw_dev, list);
				if (sdev->attrs.vendor_part_id == dev_id)
					break;
				sdev = NULL;
			}
		}
		if (unlikely(!sdev)) {
			dprint(DBG_ON, "Doorbell: No such device: ID %d, QP[%d]\n",
				dev_id, qp_id);
			rv = -ENODEV;
			goto out;
		}
		qp = siw_qp_id2obj(sdev, qp_id);
		if (likely(qp)) {
			unsigned long flags;

			if (unlikely(!down_read_trylock(&qp->state_lock))) {
				atomic_inc(&qp->state_lock_failed);
				dprint(DBG_ON, "QP[%d]: DB: cannot get state lock\n",
					QP_ID(qp));
				rv = -ENOTCONN;
				goto out;
			}
			if (unlikely(qp->attrs.state != SIW_QP_STATE_RTS)) {
				dprint(DBG_ON, "QP[%d]: DB: out of state\n",
					QP_ID(qp));
				rv = -ENOTCONN;
				up_read(&qp->state_lock);
				goto out;
			}
			lock_sq_rxsave(qp, flags);
			if (unlikely(qp->tx_ctx.tx_suspend)) {
				unlock_sq_rxsave(qp, flags);
				up_read(&qp->state_lock);
				dprint(DBG_ON, "QP[%d]: DB: tx suspended\n",
				       QP_ID(qp));
				rv = -ESHUTDOWN;
				goto out;
			}
			if (tx_wqe(qp)->wr_status == SR_WR_IDLE) {

				dprint(DBG_OL, "(QP%d): TXTX: call siw_activate_tx...\n",
					QP_ID(qp));
				rv = siw_activate_tx(qp);
				unlock_sq_rxsave(qp, flags);

				if (rv > 0) {

					qp->tx_ctx.in_syscall = 1;
					dprint(DBG_OL, "(QP%d): TXTX: call siw_qp_sq_process...\n",
						QP_ID(qp));
					rv = siw_qp_sq_process(qp);
					qp->tx_ctx.in_syscall = 0;

					if (unlikely(rv < 0))
						siw_qp_cm_drop(qp, 0);
				} else if (rv < 0)
					siw_qp_cm_drop(qp, 0);
			} else
				unlock_sq_rxsave(qp, flags);

			up_read(&qp->state_lock);
			rv = 0;
		} else {
			dprint(DBG_ON, "not found QP %d for dev %s\n",
				qp_id, sdev->ofa_dev.name);
			rv = -EINVAL;
		}
	}
out:
	if (likely(qp))
		siw_qp_put(qp);

	return rv;
}

long siw_doorbell(u32 resource, u32 id, u32 arg)
{
	switch (resource) {

	case SIW_DB_SQ:		return siw_doorbell_sq(id, arg);

	default:
		if (db_orig_call)
			return (*db_orig_call)(resource, id, arg);

		pr_warn("unknown doorbell %u\n", resource);
		return -ENOSYS;
	}
}
#endif

/*
 * siw_post_send()
 *
 * Post a list of S-WR's to a SQ.
 *
 * @ofa_qp:	OFA QP contained in siw QP
 * @wr:		Null terminated list of user WR's
 * @bad_wr:	Points to failing WR in case of synchronous failure.
 * rdmamojo.com:
 * == If the QP is in RESET, INIT or RTR state an immediate error should be returned.
 * However, they may be some low-level driver that won't follow this rule
 * (to eliminate extra check in the data path, thus providing better performance)
 * and posting Send Requests at one or all of those states may be silently ignored.
 * == If the QP is in RTS state, Send Requests can be posted and they will be processed.
 * == If the QP is in SQE or ERROR state, Send Requests can be posted
 * and they will be completed with error.
 * == If the QP is in SQD state, Send Requests can be posted, but they won't be processed.
 */
#if KS_POST_SEND_HAS_CONST
int siw_post_send(struct ib_qp *ofa_qp,
		const struct ib_send_wr *wr,
		const struct ib_send_wr **bad_wr)
#else
int siw_post_send(struct ib_qp *ofa_qp, struct ib_send_wr *wr,
		  struct ib_send_wr **bad_wr)
#endif
{
	struct siw_qp	*qp = siw_qp_ofa2siw(ofa_qp);
	struct siw_wqe	*wqe = tx_wqe(qp);
#if 0
	struct siw_mr   *mr;
#endif
	unsigned long flags;
	int rv = 0;
	int force_post = 0;
	enum siw_tx_ctx_pref tx_ctx_pref = SIW_TX_CTX_PREF_RR;

	dprint(DBG_WR|DBG_TX|DBG_OL, "(QP%d): state=%d, WR num-sge %d\n",
		QP_ID(qp), qp->attrs.state, wr ? wr->num_sge : -1);

	/*
	 * Try to acquire QP state lock. Must be non-blocking
	 * to accommodate kernel clients needs.
	 */
	if (!down_read_trylock(&qp->state_lock)) {
		enum siw_qp_state qp_state = smp_load_acquire(&qp->attrs.state);
		enum siw_qp_flags qp_flags = smp_load_acquire(&qp->attrs.flags);
		int tx_suspend;
		unsigned long flags;
		lock_sq_rxsave(qp, flags);
		tx_suspend = qp->tx_ctx.tx_suspend;
		unlock_sq_rxsave(qp, flags);
		if ((tx_suspend || qp_state == SIW_QP_STATE_ERROR) && !(qp_flags & SIW_QP_IN_DESTROY)) {
			/*
			 * ERROR state is final, so we can be sure
			 * this state will not change as long as the QP
			 * exists.
			 *
			 * This handles an ib_drain_sq() call with
			 * a concurrent request to set the QP state
			 * to ERROR.
			 */

			rv = siw_sq_flush_wr(qp, wr, (const struct ib_send_wr **)bad_wr, false);
		} else {
			atomic_inc(&qp->state_lock_failed);
			qp_state = smp_load_acquire(&qp->attrs.state);
			dprint(DBG_WR|DBG_ON, "(QP%d): siw_qp=" dprint_ptr_str() " state=%d flags=%x tx_suspend=%d\n",
			       QP_ID(qp), qp, qp_state, qp_flags, tx_suspend);
			BUG();
			if (qp_state != SIW_QP_STATE_ERROR)
				WARN_ON(1);
			*bad_wr = wr;
			rv = -ENOTCONN;
		}
		return rv;
	}

	if (unlikely(qp->attrs.state != SIW_QP_STATE_RTS)) {
		if (!(qp->attrs.flags & SIW_QP_IN_DESTROY) &&
			(qp->attrs.state == SIW_QP_STATE_ERROR ||
			qp->attrs.state == SIW_QP_STATE_CLOSING))
		{
			/*
			 * Immediately flush this WR to CQ, if QP
			 * is in ERROR state. SQ is guaranteed to
			 * be empty, so WR complets in-order.
			 *
			 * Typically triggered by ib_drain_sq().
			 */

			/* At this point, both tx_suspend and sq_flushed should be set */
			BUG_ON(!qp->tx_ctx.tx_suspend);
			BUG_ON(!qp->tx_ctx.sq_flushed);

			rv = siw_sq_flush_wr(qp, wr, (const struct ib_send_wr **)bad_wr, false);
		} else {
			dprint(DBG_WR|DBG_ON, "(QP%d): state=%d flags=%x\n",
				QP_ID(qp), qp->attrs.state, qp->attrs.flags);
			*bad_wr = wr;
			rv = -ENOTCONN;
		}
		up_read(&qp->state_lock);
		return rv;
	}
	if (wr && qp->kernel_verbs == 0) {
		dprint(DBG_WR|DBG_ON, "(QP%d): user mapped SQ with OFA WR\n",
			QP_ID(qp));
		up_read(&qp->state_lock);
		*bad_wr = wr;
		return -EINVAL;
	}

	lock_sq_rxsave(qp, flags);

	while (wr) {
		u32 idx = qp->sq_put % qp->attrs.sq_size;
		struct siw_sqe *sqe = &qp->sendq[idx];
		struct siw_sqe_md *sqe_md = qp->sendq_kern_md ? &qp->sendq_kern_md[idx] : NULL;

		if (sqe->flags) {
			dprint(DBG_OL, "(QP%d): SQ full, size=%d, sq_put=%d, sq_get=%d, flags=%08x\n",
				QP_ID(qp), qp->attrs.sq_size, qp->sq_put, qp->sq_get, sqe->flags);
			rv = -ENOMEM;
			break;
		}
		if (wr->num_sge > qp->attrs.sq_max_sges) {
			/*
			 * NOTE: we allow for zero length wr's here.
			 */
			dprint(DBG_WR, "(QP%d): Num SGE: %d\n",
				QP_ID(qp), wr->num_sge);
			rv = -EINVAL;
			break;
		}

		dprint(DBG_OL, "(QP%d): TXTX: Start WR processing, opcode=%x, num_sge=%d\n",
			QP_ID(qp), wr->opcode, wr->num_sge);


		sqe->id = wr->wr_id;
		sqe->flags = 0;

		if ((wr->send_flags & IB_SEND_SIGNALED) ||
		    (qp->attrs.flags & SIW_SIGNAL_ALL_WR)) {

			dprint(DBG_OL, "(QP%d): TXTX: set SIW_WQE_SIGNALLED\n",
				QP_ID(qp));

			sqe->flags |= SIW_WQE_SIGNALLED;
		}

		if (wr->send_flags & IB_SEND_FENCE)
			sqe->flags |= SIW_WQE_READ_FENCE;

		
		if (wr->send_flags & SIW_IB_SEND_MORE_WQES)
			sqe->flags |= SIW_WQE_MORE_WQES;

		if (wr->send_flags & SIW_IB_SEND_TX_TIMESTAMP) {
			if (sqe_md) {
				sqe->flags |= SIW_WQE_TX_TIMESTAMP;
				memset(sqe_md, 0, sizeof(*sqe_md));
				sqe_md->post_send_time = ktime_get();
			}
		}

		if (wr->send_flags & SIW_IB_SEND_TX_CTX_PREF_SAME_CPU) {
			tx_ctx_pref = SIW_TX_CTX_PREF_SAME_CPU;
			WARN_ON(wr->send_flags & SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT);
		} else if (wr->send_flags & SIW_IB_SEND_TX_CTX_PREF_SCQ_VECT) {
			tx_ctx_pref = SIW_TX_CTX_PREF_SCQ_VECT;
		}

		switch (wr->opcode) {

		case IB_WR_SEND:
		case IB_WR_SEND_WITH_INV:
			if (wr->send_flags & IB_SEND_SOLICITED)
				sqe->flags |= SIW_WQE_SOLICITED;

			if (!(wr->send_flags & IB_SEND_INLINE)) {
				siw_copy_sgl(wr->sg_list, sqe->sge,
					     wr->num_sge);
				sqe->num_sge = wr->num_sge;
			} else {
				rv = siw_copy_inline_sgl(wr, sqe);
				if (rv <= 0) {
					rv = -EINVAL;
					break;
				}
				sqe->flags |= SIW_WQE_INLINE;
				sqe->num_sge = 1;
			}
			if (wr->opcode == IB_WR_SEND)
				sqe->opcode = SIW_OP_SEND;
			else {
				sqe->opcode = SIW_OP_SEND_REMOTE_INV;
				sqe->rkey = wr->ex.invalidate_rkey;
			}
			break;

		case IB_WR_RDMA_READ_WITH_INV:
		case IB_WR_RDMA_READ:
			/*
			 * OFED WR restricts RREAD sink to SGL containing
			 * 1 SGE only. we could relax to SGL with multiple
			 * elements referring the SAME ltag or even sending
			 * a private per-rreq tag referring to a checked
			 * local sgl with MULTIPLE ltag's. would be easy
			 * to do...
			 */
			if (unlikely(wr->num_sge != 1)) {
				rv = -EINVAL;
				break;
			}
			siw_copy_sgl(wr->sg_list, &sqe->sge[0], 1);
			/*
			 * NOTE: zero length RREAD is allowed!
			 */
			sqe->raddr	= rdma_wr(wr)->remote_addr;
			sqe->rkey	= rdma_wr(wr)->rkey;
			sqe->num_sge	= 1;

			if (wr->opcode == IB_WR_RDMA_READ)
				sqe->opcode = SIW_OP_READ;
			else
				sqe->opcode = SIW_OP_READ_LOCAL_INV;
			break;

		case IB_WR_RDMA_WRITE:
			if (!(wr->send_flags & IB_SEND_INLINE)) {
				siw_copy_sgl(wr->sg_list, &sqe->sge[0],
					     wr->num_sge);
				sqe->num_sge = wr->num_sge;
			} else {
				rv = siw_copy_inline_sgl(wr, sqe);
				if (unlikely(rv < 0)) {
					rv = -EINVAL;
					break;
				}
				sqe->flags |= SIW_WQE_INLINE;
				sqe->num_sge = 1;
			}
			sqe->raddr	= rdma_wr(wr)->remote_addr;
			sqe->rkey	= rdma_wr(wr)->rkey;
			sqe->opcode	= SIW_OP_WRITE;

			break;

		case IB_WR_REG_MR:
#if 0
			mr = siw_mr_ofa2siw(reg_wr(wr)->mr);
			if (mr->mem.is_pbl && mr->pbl->ulp_map) {
				mr->pbl->ulp_map = false;
				rv = siw_mr_ulp_map_finalize(&mr->ofa_mr);
				if (unlikely(rv < 0)) {
					rv = -EINVAL;
					break;
				}
			}
#endif
			sqe->ofa_mr = (uint64_t)reg_wr(wr)->mr;
			sqe->rkey = reg_wr(wr)->key;
			sqe->access = SR_MEM_LREAD;
			if (reg_wr(wr)->access & IB_ACCESS_LOCAL_WRITE)
				sqe->access |= SR_MEM_LWRITE;
			if (reg_wr(wr)->access & IB_ACCESS_REMOTE_WRITE)
				sqe->access |= SR_MEM_RWRITE;
			if (reg_wr(wr)->access & IB_ACCESS_REMOTE_READ)
				sqe->access |= SR_MEM_RREAD;
			if (reg_wr(wr)->access & IB_ACCESS_REMOTE_ATOMIC)
				sqe->access |= SR_MEM_RATOMIC;
			sqe->opcode = SIW_OP_REG_MR;

			break;

		case IB_WR_LOCAL_INV:
			sqe->rkey = wr->ex.invalidate_rkey;
			sqe->opcode = SIW_OP_INVAL_STAG;

			break;

		case IB_WR_ATOMIC_CMP_AND_SWP:
		case IB_WR_MASKED_ATOMIC_CMP_AND_SWP:
			if (unlikely(!wr->sg_list || wr->num_sge != 1 ||
				wr->sg_list->length != sizeof(u64))) {
				dprint(DBG_ON|DBG_ATOMIC, "(QP%d): Invalid atomic sgl\n",
					QP_ID(qp));
				rv = -EINVAL;
				break;
			}
			if (unlikely(atomic_wr(wr)->remote_addr & (sizeof(u64) - 1))) {
				dprint(DBG_ON|DBG_ATOMIC,
					"(QP%d): Unaligned atomic raddr %llx\n",
					QP_ID(qp), atomic_wr(wr)->remote_addr);
				rv = -EINVAL;
				break;
			}
			siw_copy_sgl(wr->sg_list, &sqe->sge[0],
					 wr->num_sge);
			sqe->num_sge = wr->num_sge;
			sqe->raddr = atomic_wr(wr)->remote_addr;
			sqe->rkey = atomic_wr(wr)->rkey;
			sqe->compare_add = atomic_wr(wr)->compare_add;
			sqe->swap = atomic_wr(wr)->swap;
			if (wr->opcode == IB_WR_ATOMIC_CMP_AND_SWP) {
				sqe->opcode	= SIW_OP_COMP_AND_SWAP;
				sqe->compare_add_mask = ~(u64)0;
				sqe->swap_mask = ~(u64)0;
			} else {
				BUG_ON(wr->opcode != IB_WR_MASKED_ATOMIC_CMP_AND_SWP);
				sqe->opcode = SIW_OP_MASKED_COMP_AND_SWAP;
				sqe->compare_add_mask = atomic_wr(wr)->compare_add_mask;
				sqe->swap_mask = atomic_wr(wr)->swap_mask;
			}
			
			break;

		default:
			dprint(DBG_WR|DBG_TX|DBG_ON,
				"(QP%d): IB_WR %d not supported\n",
				QP_ID(qp), wr->opcode);
			rv = -EINVAL;
			break;
		}
		dprint(DBG_WR|DBG_TX, "(QP%d): opcode %d, flags 0x%x\n",
			QP_ID(qp), sqe->opcode, sqe->flags);

		if (unlikely(rv < 0)) {
			//omril: if so, we may still call siw-activate_tx and override rv...?!
			dprint(DBG_OL, "TXTX: Error rv=%d\n",
				rv);

			break;
		}

		smp_wmb();
		sqe->flags |= SIW_WQE_VALID;

		//omril: what if we fail in the middle of wqe/ what about WRs we had already put on sq?
		//omril: also shouldn't we check if we wrap over the sq_get pointer?
		dprint(DBG_TX, "(QP%d): add to sendq\n",
			QP_ID(qp));

		qp->sq_put++;
		dprint(DBG_OL, "(QP%d): TXTX: inc sq_put to %d\n",
			QP_ID(qp), qp->sq_put);

		wr = wr->next;
	}

	if (unlikely(qp->attrs.state != SIW_QP_STATE_RTS) && force_post) {
		unlock_sq_rxsave(qp, flags);
		goto tx_queue;
	}

	/*
	 * Send directly if SQ processing is not in progress.
	 * Eventual immediate errors (rv < 0) do not affect the involved
	 * RI resources (Verbs, 8.3.1) and thus do not prevent from SQ
	 * processing, if new work is already pending. But rv must be passed
	 * to caller.
	 */
	dprint(DBG_OL, "(QP%d): TXTX: wqe->wr_status=%x\n",
		QP_ID(qp), wqe->wr_status);

	if (wqe->wr_status != SR_WR_IDLE) {
		unlock_sq_rxsave(qp, flags);
		goto skip_direct_sending;
	}

	dprint(DBG_OL, "(QP%d): TXTX: call siw_activate_tx...\n",
		QP_ID(qp));

	rv = siw_activate_tx(qp);
	dprint(DBG_OL, "(QP%d): TXTX: siw_activate_tx retuned rv=%d\n",
		QP_ID(qp), rv);

	unlock_sq_rxsave(qp, flags);

	if (rv <= 0) {
		if (rv < 0) {
			dprint(DBG_TX | DBG_ON, "(QP%d): TX: siw_activate_tx failed with rv=%d\n",
			       QP_ID(qp), rv);
			/* Don't return error as we have already put the WRs in the Send Queue */
			if (rv != -ESHUTDOWN) {
				/* Schedule CM to close QP on its WQ (we are not allowed to wait in this ctx) */
				siw_qp_cm_drop(qp, 1);
			}
			rv = 0;
		}
		goto skip_direct_sending;
	}

tx_queue:
	if (qp->kernel_verbs) { //omril: PERFORMANCE
		dprint(DBG_OL, "(QP%d): TXTX: sq-queue-work\n",
			QP_ID(qp));
		siw_sq_queue_work(qp, tx_ctx_pref);
	} else {
		qp->tx_ctx.in_syscall = 1;

		dprint(DBG_OL, "(QP%d): TXTX: call siw_qp_sq_process...\n",
			QP_ID(qp));

		if ((rv = siw_qp_sq_process(qp) != 0) && 
			rv != -ESHUTDOWN && 
			rv != -EAGAIN && 
			rv != -EBUSY)
				siw_qp_cm_drop(qp, 0);
		/* Don't return error as we have already put the WRs in the Send Queue */
		rv = 0;

		qp->tx_ctx.in_syscall = 0;
	}
	
skip_direct_sending:

	up_read(&qp->state_lock);

	if (rv >= 0) {
		dprint(DBG_OL, "(QP%d): TXTX: post-send out, rv=%d\n", QP_ID(qp), rv);
		return 0;
	}
	/*
	 * Immediate error
	 */
	dprint(DBG_WR|DBG_ON, "(QP%d): error=%d\n", QP_ID(qp), rv);

	*bad_wr = wr;
	return rv;
}

/*
 * siw_post_receive()
 *
 * Post a list of R-WR's to a RQ.
 *
 * @ofa_qp:	OFA QP contained in siw QP
 * @wr:		Null terminated list of user WR's
 * @bad_wr:	Points to failing WR in case of synchronous failure.
 */
#if KS_POST_SEND_HAS_CONST
int siw_post_receive(struct ib_qp *ofa_qp,
		const struct ib_recv_wr *wr,
		const struct ib_recv_wr **bad_wr)
#else
int siw_post_receive(struct ib_qp *ofa_qp, struct ib_recv_wr *wr,
		     struct ib_recv_wr **bad_wr)
#endif
{
	struct siw_qp	*qp = siw_qp_ofa2siw(ofa_qp);
	int rv = 0;

	dprint(DBG_WR|DBG_TX, "(QP%d): state=%d\n", QP_ID(qp),
		qp->attrs.state);

	if (qp->srq) {
		*bad_wr = wr;
		return -EOPNOTSUPP; /* what else from errno.h? */
	}
	/*
	 * Try to acquire QP state lock. Must be non-blocking
	 * to accommodate kernel clients needs.
	 */
	if (!down_read_trylock(&qp->state_lock)) {
		enum siw_qp_state qp_state = smp_load_acquire(&qp->attrs.state);
		enum siw_qp_flags qp_flags = smp_load_acquire(&qp->attrs.flags);
		int rx_suspend;
		unsigned long flags;

		lock_rq_rxsave(qp, flags);
		rx_suspend = qp->rx_ctx.rx_suspend;
		unlock_rq_rxsave(qp, flags);

		if ((rx_suspend || qp_state == SIW_QP_STATE_ERROR) && !(qp_flags & SIW_QP_IN_DESTROY)) {
			/*
			 * ERROR state is final, so we can be sure
			 * this state will not change as long as the QP
			 * exists.
			 *
			 * This handles an ib_drain_rq() call with
			 * a concurrent request to set the QP state
			 * to ERROR.
			 */
			rv = siw_rq_flush_wr(qp, wr, (const struct ib_recv_wr **)bad_wr, false);
		} else {
			dprint(DBG_ON, " (QP%d): state=%d\n", QP_ID(qp),
						qp->attrs.state);
			*bad_wr = wr;
			rv = -ENOTCONN;
		}
		return rv;
	}
	if (qp->kernel_verbs == 0) {
		dprint(DBG_WR|DBG_ON, "(QP%d): user mapped RQ with OFA WR\n",
			QP_ID(qp));
		up_read(&qp->state_lock);
		*bad_wr = wr;
		return -EINVAL;
	}
	if (qp->attrs.state > SIW_QP_STATE_RTS) {
		if (!(qp->attrs.flags & SIW_QP_IN_DESTROY) &&
			(qp->attrs.state == SIW_QP_STATE_CLOSING ||
			qp->attrs.state == SIW_QP_STATE_ERROR))
		{
			/*
			 * Immediately flush this WR to CQ, if QP
			 * is in ERROR state. RQ is guaranteed to
			 * be empty, so WR complets in-order.
			 *
			 * Typically triggered by ib_drain_rq().
			 */
			rv = siw_rq_flush_wr(qp, wr, (const struct ib_recv_wr **)bad_wr, false);
		} else {
			dprint(DBG_ON, " (QP%d): state=%d flags=%x\n", QP_ID(qp),
						qp->attrs.state, qp->attrs.flags);
			*bad_wr = wr;
			rv = -ENOTCONN;
		}
		up_read(&qp->state_lock);
		return rv;
	}
	while (wr) {
		u32 idx = qp->rq_put % qp->attrs.rq_size;
		struct siw_rqe *rqe = &qp->recvq[idx];

		if (rqe->flags) {
			dprint(DBG_WR, "(QP%d): RQ full\n", QP_ID(qp));
			rv = -ENOMEM;
			break;
		}
		if (wr->num_sge > qp->attrs.rq_max_sges) {
			dprint(DBG_WR|DBG_ON, "(QP%d): Num SGE: %d\n",
				QP_ID(qp), wr->num_sge);
			rv = -EINVAL;
			break;
		}
		rqe->id = wr->wr_id;
		rqe->num_sge = wr->num_sge;
		siw_copy_sgl(wr->sg_list, rqe->sge, wr->num_sge);

		smp_wmb();

		rqe->flags = SIW_WQE_VALID;


		qp->rq_put++;
		wr = wr->next;
	}
	if (rv < 0) {
		dprint(DBG_WR|DBG_ON, "(QP%d): error=%d\n", QP_ID(qp), rv);
		*bad_wr = wr;
	}
	up_read(&qp->state_lock);

	return rv > 0 ? 0 : rv;
}

extern bool cq_notify_tasklet;

#if KS_IB_DESTROY_CQ_HAS_IB_UDATA
#if KS_IB_DESTROY_CQ_INT_RETURN
int siw_destroy_cq(struct ib_cq *ofa_cq, struct ib_udata *udata)
{
	struct siw_cq		*cq  = siw_cq_ofa2siw(ofa_cq);
	struct ib_device	*ofa_dev = ofa_cq->device;
	struct siw_dev		*sdev = siw_dev_ofa2siw(ofa_dev);
	(void)udata;
#else
void siw_destroy_cq(struct ib_cq *ofa_cq, struct ib_udata *udata)
{
	struct siw_cq		*cq  = siw_cq_ofa2siw(ofa_cq);
	struct ib_device	*ofa_dev = ofa_cq->device;
	struct siw_dev		*sdev = siw_dev_ofa2siw(ofa_dev);
	(void)udata;
#endif
#else
int siw_destroy_cq(struct ib_cq *ofa_cq)
{
	struct siw_cq		*cq  = siw_cq_ofa2siw(ofa_cq);
	struct ib_device	*ofa_dev = ofa_cq->device;
	struct siw_dev		*sdev = siw_dev_ofa2siw(ofa_dev);
#endif
	
	atomic_inc(&cq->dying);
	
	if (cq_notify_tasklet) {
		//tasklet_disable(&cq->notify_task);
		tasklet_kill(&cq->notify_task);
	} else {
		flush_work(&cq->notify_work);
	}

	siw_cq_flush(cq);

	siw_remove_obj(&sdev->idr_lock, &sdev->cq_idr, &cq->hdr);
	siw_cq_put(cq);

#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
	wait_for_completion(&cq->free_comp);
#endif

#if KS_IB_DESTROY_CQ_INT_RETURN
	return 0;
#endif
}

extern bool notify_on_wq;

/*
 * siw_create_cq()
 *
 * Create CQ of requested size on given device.
 *
 * @ofa_dev:	OFA device contained in siw device
 * @size:	maximum number of CQE's allowed.
 * @ib_context: user context.
 * @udata:	used to provide CQ ID back to user.
 */
#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
static inline struct siw_cq *to_siw_cq(struct ib_cq *base_cq)
{
	return container_of(base_cq, struct siw_cq, ofa_cq);
}
#if !KS_IB_CREATE_CQ_HAS_ATTR_BUNDLE
int siw_create_cq(struct ib_cq *base_cq, const struct ib_cq_init_attr *attr,
			 struct ib_udata *udata)
#else
int siw_create_cq(struct ib_cq *base_cq, const struct ib_cq_init_attr *attr,
			 struct uverbs_attr_bundle *attrs)
#endif
#elif KS_CREATE_CQ_HAS_IB_CQ_INIT_ATTR
struct ib_cq *siw_create_cq(struct ib_device *ofa_dev,
			    const struct ib_cq_init_attr *attr,
			    struct ib_ucontext *ib_context,
			    struct ib_udata *udata)
#else
struct ib_cq *siw_create_cq(struct ib_device *ofa_dev, int size,
			    int vec /* unused */,
			    struct ib_ucontext *ib_context,
			    struct ib_udata *udata)
#endif
{
#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
	struct ib_device *ofa_dev = base_cq->device;
#endif
	struct siw_cq			*cq = NULL;
	struct siw_dev			*sdev = siw_dev_ofa2siw(ofa_dev);
	struct siw_uresp_create_cq	uresp;
	int rv;
#if KS_CREATE_CQ_HAS_IB_CQ_INIT_ATTR
	int size = attr->cqe;
#endif
#if KS_IB_CREATE_CQ_HAS_ATTR_BUNDLE
	struct ib_udata *udata = &attrs->driver_udata;
#endif

	dprint(DBG_OL, "-->\n");

	if (!ofa_dev) {
		pr_warn("NO OFA device\n");
		rv = -ENODEV;
		goto err_out;
	}
	if (atomic_inc_return(&sdev->num_cq) > SIW_MAX_CQ) {
		dprint(DBG_ON, ": Out of CQ's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	if (size < 1 || size > SIW_MAX_CQE) {
		dprint(DBG_ON, ": CQE: %d\n", size);
		rv = -EINVAL;
		goto err_out;
	}
#if KS_IB_CREATE_CQ_HAS_IB_DEVICE
	cq = kzalloc(sizeof *cq, GFP_KERNEL);
	if (!cq) {
		dprint(DBG_ON, ":  kmalloc\n");
		rv = -ENOMEM;
		goto err_out;
	}
#else
	cq = to_siw_cq(base_cq);
	init_completion(&cq->free_comp);
#endif
	size = roundup_pow_of_two(size);
	cq->ofa_cq.cqe = size;
	cq->num_cqe = size;
	cq->comp_vector = attr->comp_vector;

#if KS_IB_CREATE_CQ_HAS_IB_UCONTEXT
	if (!ib_context)
		cq->kernel_verbs = 1;
#else
	if (!udata)
		cq->kernel_verbs = 1;
#endif
	if (cq->kernel_verbs) {
		if (SIW_NO_VMALLOC_FOR_KVERBS_CQ) {
			cq->queue = (void *)__get_free_pages(GFP_KERNEL,
							get_order(size * sizeof(struct siw_cqe) + sizeof(struct siw_cq_ctrl)));
			cq->cqe_md = (void *)__get_free_pages(GFP_KERNEL,
							      get_order(size * sizeof(struct siw_cqe_md)));
		} else {
			cq->queue = vmalloc(size * sizeof(struct siw_cqe) + sizeof(struct siw_cq_ctrl));
			cq->cqe_md = vmalloc(size * sizeof(struct siw_cqe_md));
		}
	} else {
		cq->queue = vmalloc_user(size * sizeof(struct siw_cqe)
				+ sizeof(struct siw_cq_ctrl));
	}

	if (cq->queue == NULL || (cq->kernel_verbs && !cq->cqe_md)) {
		rv = -ENOMEM;
		dprint(DBG_ON, "siw_create_cq: vmalloc");
		goto err_out;
	}
	if (cq->kernel_verbs)
		memset(cq->queue, 0, size * sizeof(struct siw_cqe)
			+ sizeof(struct siw_cq_ctrl));

	rv = siw_cq_add(sdev, cq);
	if (rv)
		goto err_out;

	spin_lock_init(&cq->lock);
	cq->notify = &((struct siw_cq_ctrl *)&cq->queue[size])->notify;
	cq->notify_on_wq = notify_on_wq;

	if (cq_notify_tasklet) {
#if !KS_HAS_TASKLET_SETUP
		tasklet_init(&cq->notify_task,
			siw_cq_notify_task, (unsigned long)cq);
#else
		tasklet_setup(&cq->notify_task, siw_cq_notify_task);
#endif
	} else {
		INIT_WORK(&cq->notify_work, siw_cq_notify_work);
	}

	if (!cq->kernel_verbs) {
		#if !KS_IB_CREATE_CQ_HAS_IB_UCONTEXT
		if (udata) {
		struct siw_ucontext *ctx =
			rdma_udata_to_drv_context(udata, struct siw_ucontext,
						  ib_ucontext);
		#else
		struct siw_ucontext *ctx = siw_ctx_ofa2siw(ib_context);
		#endif

		uresp.cq_key = siw_insert_uobj(ctx, cq->queue,
					size * sizeof(struct siw_cqe) +
					sizeof(struct siw_cq_ctrl));

		if (uresp.cq_key > SIW_MAX_UOBJ_KEY)
			pr_warn("Preparing mmap CQ failed\n");

		uresp.cq_id = OBJ_ID(cq);
		uresp.num_cqe = size;

		rv = ib_copy_to_udata(udata, &uresp, sizeof uresp);
		if (rv)
			goto err_out_idr;
		#if !KS_IB_CREATE_CQ_HAS_IB_UCONTEXT
		}
		#endif
	}
	dprint(DBG_OL, "<-- OK\n");
#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
	return 0;
#else
	return &cq->ofa_cq;
#endif

err_out_idr:
	siw_remove_obj(&sdev->idr_lock, &sdev->cq_idr, &cq->hdr);
err_out:
	dprint(DBG_OBJ, ": CQ creation failed %d", rv);

	if (cq && cq->queue) {
		if (SIW_NO_VMALLOC_FOR_KVERBS_CQ) {
			free_pages((unsigned long)cq->queue, get_order(size * sizeof(struct siw_cqe) + sizeof(struct siw_cq_ctrl)));
			if (cq->cqe_md)
				free_pages((unsigned long)cq->cqe_md, get_order(size * sizeof(struct siw_cqe_md)));
		} else {
			vfree(cq->queue);
			if (cq->cqe_md)
				vfree(cq->cqe_md);
		}
	}

#if KS_IB_CREATE_CQ_HAS_IB_DEVICE
	kfree(cq);
#else
	/* CQ memory was allocated by RDMA Layer, so don't free it */
#endif
	atomic_dec(&sdev->num_cq);

	dprint(DBG_OL, "<-- Err\n");
#if !KS_IB_CREATE_CQ_HAS_IB_DEVICE
	return rv;
#else
	return ERR_PTR(rv);
#endif
}

/*
 * siw_poll_cq()
 *
 * Reap CQ entries if available and copy work completion status into
 * array of WC's provided by caller. Returns number of reaped CQE's.
 *
 * @ofa_cq:	OFA CQ contained in siw CQ.
 * @num_cqe:	Maximum number of CQE's to reap.
 * @wc:		Array of work completions to be filled by siw.
 */
int siw_poll_cq(struct ib_cq *ofa_cq, int num_cqe, struct ib_wc *wc)
{
	struct siw_cq		*cq  = siw_cq_ofa2siw(ofa_cq);
	int			i;

	for (i = 0; i < num_cqe; i++) {
		if (!(siw_reap_cqe(cq, wc)))
			break;
		wc++;
	}
	return i;
}

/*
 * siw_req_notify_cq()
 *
 * Request notification for new CQE's added to that CQ.
 * Defined flags:
 * o SIW_CQ_NOTIFY_SOLICITED lets siw trigger a notification
 *   event if a WQE with notification flag set enters the CQ
 * o SIW_CQ_NOTIFY_NEXT_COMP lets siw trigger a notification
 *   event if a WQE enters the CQ.
 * o IB_CQ_REPORT_MISSED_EVENTS: return value will provide the
 *   number of not reaped CQE's regardless of its notification
 *   type and current or new CQ notification settings.
 *
 * @ofa_cq:	OFA CQ contained in siw CQ.
 * @flags:	Requested notification flags.
 */
int siw_req_notify_cq(struct ib_cq *ofa_cq, enum ib_cq_notify_flags flags)
{
	struct siw_cq	 *cq  = siw_cq_ofa2siw(ofa_cq);
	int rv = 0;
	bool work_sched = false;

	dprint(DBG_CQ, "(CQ%d:) flags: 0x%8x\n", OBJ_ID(cq), flags);

	if ((flags & IB_CQ_SOLICITED_MASK) == IB_CQ_SOLICITED)
		smp_store_mb(*cq->notify, SIW_NOTIFY_SOLICITED);
	else
		smp_store_mb(*cq->notify, SIW_NOTIFY_ALL);

#if SIW_CQ_NOTIFY_WORK_QP_INDEPENDENT
	do {
		struct siw_cqe *cqe;
		unsigned long flags;

		lock_cq_rxsave(cq, flags);
		cqe = &cq->queue[cq->cq_get % cq->num_cqe];
		work_sched = (cqe->flags & SIW_WQE_VALID);
		unlock_cq_rxsave(cq, flags);
		if (work_sched) {
			if (!siw_schedule_cq_notify_work(NULL, cq)) {
				work_sched = false;
				dprint(DBG_CQ, "(CQ%d): (" dprint_ptr_str() ") - Scheduling work failed\n", OBJ_ID(cq), cq);
			}
		}
	} while(0);
#endif

	/* TODO
	if (flags & IB_CQ_REPORT_MISSED_EVENTS)
		return atomic_read(&cq->qlen);
	*/
	if (flags & IB_CQ_REPORT_MISSED_EVENTS) {
		struct siw_cqe *cqe;
		unsigned long flags;

		if (!work_sched) {
			lock_cq_rxsave(cq, flags);
			cqe = &cq->queue[cq->cq_get % cq->num_cqe];
				rv = !!((cqe->flags & SIW_WQE_VALID)) || !(_load_shared(*cq->notify));
			unlock_cq_rxsave(cq, flags);
		}
	}

	return rv;
}

/*
 * siw_dereg_mr()
 *
 * Release Memory Region.
 *
 * TODO: Update function if Memory Windows are supported by siw:
 *       Is OFED core checking for MW dependencies for current
 *       MR before calling MR deregistration?.
 *
 * @ofa_mr:     OFA MR contained in siw MR.
 */
#if KS_IB_DEREG_MR_HAS_IB_UDATA
int siw_dereg_mr(struct ib_mr *ofa_mr, struct ib_udata *udata)
{
	struct siw_mr	*mr;
	struct siw_dev	*sdev;
	(void)udata;
#else
int siw_dereg_mr(struct ib_mr *ofa_mr)
{
	struct siw_mr	*mr;
	struct siw_dev	*sdev;
#endif

	mr = siw_mr_ofa2siw(ofa_mr);
	sdev = mr->pd->hdr.sdev;

	if (!sdev) {
		dprint(DBG_OBJ|DBG_MM|DBG_ON, "sdev NULL - mem/obj leak (may fail to rmmod)\n");
		return -1;
	}

	dprint(DBG_OBJ|DBG_MM, "(MEM%d): Dereg MR, object " dprint_ptr_str() ", #ref's: %d\n",
		mr->mem.hdr.id, mr->mem_obj,
		kref_read(&mr->mem.hdr.ref));

	mr->mem.stag_valid = 0;

	siw_pd_put(mr->pd);
	siw_remove_obj(&sdev->idr_lock, &sdev->mem_idr, &mr->mem.hdr);
	siw_mem_put(&mr->mem); //omril: calls siw_free_mem which calls
						   //siw_umem_release or siw_pbl_free

	return 0;
}

static struct siw_mr *siw_create_mr(struct siw_dev *sdev, void *mem_obj,
				    u64 start, u64 len, int rights)
{
	struct siw_mr *mr = kzalloc(sizeof *mr, GFP_KERNEL);
	if (!mr)
		return NULL;

	mr->mem.stag_valid = 0;

	if (siw_mem_add(sdev, &mr->mem) < 0) {
		dprint(DBG_ON, ": siw_mem_add\n");
		kfree(mr);
		return NULL;
	}
	dprint(DBG_OBJ|DBG_MM, "(MEM%d): New MR, object " dprint_ptr_str() "\n",
		mr->mem.hdr.id, mem_obj);

	mr->ofa_mr.lkey = mr->ofa_mr.rkey = mr->mem.hdr.id << 8;

	mr->mem.va  = start;
	mr->mem.len = len;
	mr->mem.mr  = NULL;
	mr->mem.perms = SR_MEM_LREAD | /* not selectable in OFA */
			(rights & IB_ACCESS_REMOTE_READ   ? SR_MEM_RREAD   : 0) |
			(rights & IB_ACCESS_LOCAL_WRITE   ? SR_MEM_LWRITE  : 0) |
			(rights & IB_ACCESS_REMOTE_WRITE  ? SR_MEM_RWRITE  : 0) |
			(rights & IB_ACCESS_REMOTE_ATOMIC ? SR_MEM_RATOMIC : 0);

	dprint(DBG_OBJ|DBG_MM, "(MEM%d): perms 0x%08x\n",
		mr->mem.hdr.id, mr->mem.perms);

	mr->mem_obj = mem_obj;

	return mr;
}

/*
 * siw_reg_user_mr()
 *
 * Register Memory Region.
 *
 * @ofa_pd:	OFA PD contained in siw PD.
 * @start:	starting address of MR (virtual address)
 * @len:	len of MR
 * @rnic_va:	not used by siw
 * @rights:	MR access rights
 * @udata:	user buffer to communicate STag and Key.
 */
#if KS_IB_REG_USER_MR_HAS_ATTR
struct ib_mr *siw_reg_user_mr(struct ib_pd *ofa_pd,
			      struct ib_mr_init_attr *attr,
			      struct ib_udata *udata)
{
	u64 start = attr->start;
	u64 len = attr->length;
	u64 rnic_va = attr->hca_va;
	int rights = attr->access_flags;
#elif KS_IB_REG_USER_MR_HAS_DMAH
struct ib_mr *siw_reg_user_mr(struct ib_pd *ofa_pd, u64 start, u64 len,
			      u64 rnic_va, int rights,
			      struct ib_dmah *dmah __attribute__((unused)), struct ib_udata *udata)
{
#else
struct ib_mr *siw_reg_user_mr(struct ib_pd *ofa_pd, u64 start, u64 len,
			      u64 rnic_va, int rights, struct ib_udata *udata)
{
#endif
	struct siw_mr		*mr = NULL;
	struct siw_pd		*pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_umem		*umem = NULL;
	struct siw_ureq_reg_mr	ureq;
	struct siw_uresp_reg_mr	uresp;
	struct siw_dev		*sdev = pd->hdr.sdev;

	unsigned long mem_limit = rlimit(RLIMIT_MEMLOCK);
	int rv;

	dprint(DBG_MM|DBG_OBJ, " start: 0x%016llx, "
		"va: 0x%016llx, len: %llu, ctx: " dprint_ptr_str() "\n",
		(unsigned long long)start,
		(unsigned long long)rnic_va,
		(unsigned long long)len,
		ofa_pd->uobject->context);
	if (atomic_inc_return(&sdev->num_mem) > SIW_MAX_MR) {
		dprint(DBG_ON, ": Out of MRs: %d\n",
			atomic_read(&sdev->num_mem));
		rv = -ENOMEM;
		goto err_out;
	}
	if (!len) {
		rv = -EINVAL;
		goto err_out;
	}
	if (mem_limit != RLIM_INFINITY) {
		unsigned long num_pages =
			(PAGE_ALIGN(len + (start & ~PAGE_MASK))) >> PAGE_SHIFT;
		mem_limit >>= PAGE_SHIFT;

		if (num_pages > mem_limit - current->mm->locked_vm) {
			dprint(DBG_ON|DBG_MM,
				": pages req: %lu, limit: %lu, locked: %lu\n",
				num_pages, mem_limit, current->mm->locked_vm);
			rv = -ENOMEM;
			goto err_out;
		}
	}
	umem = siw_umem_get(start, len);
	if (IS_ERR(umem)) {
		dprint(DBG_MM, " siw_umem_get:%ld LOCKED:%lu, LIMIT:%lu\n",
			PTR_ERR(umem), current->mm->locked_vm,
			current->signal->rlim[RLIMIT_MEMLOCK].rlim_cur >>
			PAGE_SHIFT);
		rv = PTR_ERR(umem);
		umem = NULL;
		goto err_out;
	}
	mr = siw_create_mr(sdev, umem, start, len, rights);
	if (!mr) {
		rv = -ENOMEM;
		goto err_out;
	}

	if (udata) {
		rv = ib_copy_from_udata(&ureq, udata, sizeof ureq);
		if (rv)
			goto err_out_mr;

		mr->ofa_mr.lkey |= ureq.stag_key;
		mr->ofa_mr.rkey |= ureq.stag_key; /* XXX ??? */
		uresp.stag = mr->ofa_mr.lkey;

		rv = ib_copy_to_udata(udata, &uresp, sizeof uresp);
		if (rv)
			goto err_out_mr;
	}
	mr->pd = pd;
	siw_pd_get(pd);

	mr->mem.stag_valid = 1;

	return &mr->ofa_mr;

err_out_mr:
	siw_remove_obj(&sdev->idr_lock, &sdev->mem_idr, &mr->mem.hdr);
	kfree(mr);

err_out:
	if (umem)
		siw_umem_release(umem);

	atomic_dec(&sdev->num_mem);

	return ERR_PTR(rv);
}

static struct ib_mr *__siw_alloc_mr(struct ib_pd *ofa_pd, enum ib_mr_type mr_type,
			   u32 max_sge, bool check_max_sge)
{
	struct siw_mr	*mr;
	struct siw_pd	*pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev	*sdev = pd->hdr.sdev;
	struct siw_pbl	*pbl = NULL;
	int rv;

	if (atomic_inc_return(&sdev->num_mem) > SIW_MAX_MR) {
		dprint(DBG_ON, ": Out of MRs: %d\n",
			atomic_read(&sdev->num_mem));
		rv = -ENOMEM;
		goto err_out;
	}
	if (mr_type != IB_MR_TYPE_MEM_REG) {
		dprint(DBG_ON, ": Unsupported MR type's: %d\n", mr_type);
		rv = -ENOSYS;
		goto err_out;
	}
	//omril: lock table probably needs more (and contiguous)...
	if (check_max_sge && max_sge > SIW_MAX_SGE_PBL) {
		dprint(DBG_ON, ": Too many SGE's: %d\n", max_sge);
		rv = -ENOMEM;
		goto err_out;
	}

	pbl = siw_pbl_alloc(max_sge); //omril: just kzalloc...
	if (IS_ERR(pbl)) {
		rv = PTR_ERR(pbl);
		dprint(DBG_ON, ": siw_pbl_alloc failed: %d\n", rv);
		pbl = NULL;
		goto err_out;
	}

	//omril:
	//kzalloc() mr,
	//siw_mem_add mr->mem (which gets unique id),
	//set lkey, rky etc...
	mr = siw_create_mr(sdev, pbl, 0, max_sge * PAGE_SIZE, 0); //vmap???
	if (!mr) {
		rv = -ENOMEM;
		goto err_out;
	}
	mr->ofa_mr.device = &sdev->ofa_dev;
	mr->mem.is_pbl = 1;
	mr->pd = pd;
	mr->ofa_mr.page_size = PAGE_SIZE;
	siw_pd_get(pd);

	dprint(DBG_MM, " MEM(%d): Created with %u SGEs\n", OBJ_ID(&mr->mem),
		max_sge);

	dprint(DBG_MM, " mr=" dprint_ptr_str() ", &mr->ofa_mr=" dprint_ptr_str() ", mr->ofa_mr.device=" dprint_ptr_str() ", mr->mem.is_pbl=%d\n",
		mr, &mr->ofa_mr, mr->ofa_mr.device, mr->mem.is_pbl);

	return &mr->ofa_mr;

err_out:
	if (pbl)
		siw_pbl_free(pbl);

	dprint(DBG_ON, ": failed: %d\n", rv);
		
	atomic_dec(&sdev->num_mem);

	return ERR_PTR(rv);
}

#if KS_IB_ALLOC_MR_HAS_IB_UDATA
struct ib_mr *siw_alloc_mr(struct ib_pd *ofa_pd, enum ib_mr_type mr_type,
			   u32 max_sge, struct ib_udata *udata) {
	(void)udata;
#else
struct ib_mr *siw_alloc_mr(struct ib_pd *ofa_pd, enum ib_mr_type mr_type,
			   u32 max_sge) {
#endif
	return __siw_alloc_mr(ofa_pd, mr_type, max_sge, 1);
}

struct ib_mr *siw_mr_alloc(struct ib_pd *ofa_pd, u32 max_sge)
{
	//dprint(DBG_ON, ": External mr alloc\n");
	return __siw_alloc_mr(ofa_pd, IB_MR_TYPE_MEM_REG, max_sge, 0);
}
EXPORT_SYMBOL(siw_mr_alloc);

#if 1
int siw_mr_enable(struct ib_mr *ofa_mr, u32 access)
{
	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
	struct siw_mem *mem = siw_mem_id2obj(mr->pd->hdr.sdev, ofa_mr->rkey >> 8); //omril: under rcu-lock
	int rv = -1;

	//dprint(DBG_ON, ": External mr enable\n");

	if (!mem) {
		dprint(DBG_MM, ": STag %u unknown\n", ofa_mr->rkey >> 8);
		return -EINVAL;
	}
	if (mem != &mr->mem) {
		dprint(DBG_MM, ": MR-mem mismatch: " dprint_ptr_str() " != " dprint_ptr_str() "\n", &mr->mem, mem);
		return -EINVAL;
	}
	if (mem->stag_valid) {
		dprint(DBG_MM, ": STag already valid: %u\n",
			ofa_mr->rkey >> 8);
		rv = -EINVAL;
		goto out;
	}
	mem->perms = access;
	mem->stag_valid = 1;
	rv = 0;
	dprint(DBG_MM, ": STag now valid: %u\n", ofa_mr->rkey >> 8);

out:
	siw_mem_put(mem);
	return rv;
}
EXPORT_SYMBOL(siw_mr_enable);
#else
{
	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
	struct siw_sqe sqe = {
		.ofa_mr = ofa_mr;
		.rkey   = ofa_mr->rkey;
		.access = access;
	};

	dprint(DBG_ON, ": External mr enable\n");
	return siw_fastreg_mr(mr->pd, &sqe); //omril: Not good - what if the func will do other stuff with SQE.
}
#endif

int siw_mr_free(struct ib_mr *ofa_mr)
{
	//dprint(DBG_ON, ": External mr free\n");
	#if KS_IB_DEREG_MR_HAS_IB_UDATA
	return siw_dereg_mr(ofa_mr, NULL);
	#else
	return siw_dereg_mr(ofa_mr); //omril: should we rcu-lock siw_mem?
	#endif
}
EXPORT_SYMBOL(siw_mr_free);

/* Just used to count number of pages being mapped */
static int siw_set_pbl_page(struct ib_mr *ofa_mr, u64 buf_addr)
{
	return 0;
}

#if 0
static int siw_map_mr_sg_bypass(struct ib_mr *ofa_mr)
{
	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
	struct siw_pbl *pbl = mr->pbl;
	struct siw_pble *pble = pbl->pbe;
	u64 pbl_size = 0;
	unsigned int mr_page_size = ofa_mr->page_size;
	u32 first_offset;
	u64 first_size, last_size, size;
	int last_page;
	int i, rv = -1;
	unsigned int n_pages;

	if (!pbl) {
		dprint(DBG_MM, "No mr-pages\n");
		return -EINVAL;
	}
	if (pbl->max_buf < n_pages) {
		dprint(DBG_MM, "Exceed max mr pages (%d, %u)\n", n_pages, pbl->max_buf);
		return -ENOMEM;
	}
	if (is_power_of_2(mr_page_size)) {
		dprint(DBG_MM, "mr_page_size not power of 2 (%u)\n", mr_page_size);
		return -EINVAL;
	}

	n_pages = pbl->num_buf;
	last_page = n_pages - 1;
	first_offset = ofa_mr->iova & (mr_page_size - 1);
	first_size = mr_page_size - first_offset;
	last_size = ofa_mr->length - first_size - (n_pages - 2) * mr_page_size;
	if (last_size <= 0 || last_size > mr_page_size) {
		dprint(DBG_MM, "Bad last-page-size=%llu (mr_page_size=%u, length=%u, "
		   "first_size=%llu, n_pages=%d)\n",
			last_size, mr_page_size, ofa_mr->length, first_size, n_pages);
		rv = -EINVAL;
		goto out;
	}

	/* re-format pble */
	//omril: maybe all we need is seeting the size and pbl_off
	for (i = 0; i < n_pages; i++) {
		if (pble[i].addr & (PAGE_SIZE - 1)) {
			dprint(DBG_MM, "page %d is not aligned %llx\n", i, pble[i].addr);
			rv = -EINVAL;
			goto out;
		}

		if (i == 0) {
			pble->addr = pble[0].addr | first_offset;
			pble->size = first_size;
			pble->pbl_off = 0;
			pbl->num_buf = 1;
			size = pble->size;
		}
		else {
			size = (i == last_page) ? last_size : mr_page_size;
			if (pble->addr + pble->size != pble[i].addr) {
				pble++;
				pbl->num_buf++;
				pble->addr = pble[i].addr;
				pble->size = size;
				pble->pbl_off = pbl_size;
			}
			else
				pble->size += size;
		}

		pbl_size += size;

		dprint(DBG_MM, "mr " dprint_ptr_str() ": page %d, addr=%llx, size=%llu, total %llu\n",
			mr, i, pble->addr, pble->size, pbl_size);
	}

	if (pbl_size != ofa_mr->length) {
		dprint(DBG_MM, "Total calc size (%llu) and mr's length (%u) differ\n",
			pbl_size, ofa_mr->length);
		rv = -EINVAL;
		goto out;
	}

	rv = pbl->num_buf;

//	mr->mem.len = ofa_mr->length;
//	mr->mem.va = ofa_mr->iova;

out:
	return rv;
}
#else
//omril: same code just changed rv of success
#if 0
static int siw_mr_ulp_map_finalize(struct ib_mr *ofa_mr)
{
	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
	struct siw_pbl *pbl = mr->pbl;
	struct siw_pble *pble = pbl->pbe;
	u64 pbl_size = 0;
	unsigned int mr_page_size = ofa_mr->page_size;
	u32 first_offset;
	u64 first_size, last_size, size;
	int last_page;
	int i, rv = -1;
	unsigned int n_pages;

	if (!pbl) {
		dprint(DBG_MM, "No mr-pages\n");
		return -EINVAL;
	}
	if (pbl->max_buf < n_pages) {
		dprint(DBG_MM, "Exceed max mr pages (%d, %u)\n", n_pages, pbl->max_buf);
		return -ENOMEM;
	}
	if (is_power_of_2(mr_page_size)) {
		dprint(DBG_MM, "mr_page_size not power of 2 (%u)\n", mr_page_size);
		return -EINVAL;
	}

	n_pages = pbl->num_buf;
	last_page = n_pages - 1;
	first_offset = ofa_mr->iova & (mr_page_size - 1);
	first_size = mr_page_size - first_offset;
	last_size = ofa_mr->length - first_size - (n_pages - 2) * mr_page_size;
	if (last_size <= 0 || last_size > mr_page_size) {
		dprint(DBG_MM, "Bad last-page-size=%llu (mr_page_size=%u, length=%u, "
		   "first_size=%llu, n_pages=%d)\n",
			last_size, mr_page_size, ofa_mr->length, first_size, n_pages);
		rv = -EINVAL;
		goto out;
	}

	/* re-format pble */
	//omril: maybe all we need is seeting the size and pbl_off
	for (i = 0; i < n_pages; i++) {
		if (pble[i].addr & (PAGE_SIZE - 1)) {
			dprint(DBG_MM, "page %d is not aligned %llx\n", i, pble[i].addr);
			rv = -EINVAL;
			goto out;
		}

		if (i == 0) {
			pble->addr = pble[0].addr | first_offset;
			pble->size = first_size;
			pble->pbl_off = 0;
			pbl->num_buf = 1;
			size = pble->size;
		}
		else {
			size = (i == last_page) ? last_size : mr_page_size;
			if (pble->addr + pble->size != pble[i].addr) {
				pble++;
				pbl->num_buf++;
				pble->addr = pble[i].addr;
				pble->size = size;
				pble->pbl_off = pbl_size;
			}
			else
				pble->size += size;
		}

		pbl_size += size;

		dprint(DBG_MM, "mr " dprint_ptr_str() ": page %d, addr=%llx, size=%llu, total %llu\n",
			mr, i, pble->addr, pble->size, pbl_size);
	}

	if (pbl_size != ofa_mr->length) {
		dprint(DBG_MM, "Total calc size (%llu) and mr's length (%u) differ\n",
			pbl_size, ofa_mr->length);
		rv = -EINVAL;
		goto out;
	}

	mr->mem.len = ofa_mr->length;
	mr->mem.va = ofa_mr->iova;
	rv = 0;
out:
	return rv;
}
#endif
#endif

//omril:
//called from ib_map_mr_sg() verb which first
//sets mr->page_size to the given @page_size
//
//This function will set the mr->length and mr->iova
int siw_map_mr_sg(struct ib_mr *ofa_mr, struct scatterlist *sl, int num_sle,
		  unsigned int *sg_off)
{
	struct scatterlist *slp;
	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
	struct siw_pbl *pbl = mr->pbl;
	struct siw_pble *pble = pbl->pbe;
	u64 pbl_size;
	int i, rv;

//	if (!sl && !num_sle) {
//		dprint(DBG_ON, ": calling map-mr-sg bypass...\n");
//		return siw_map_mr_sg_bypass(ofa_mr);
//	}

	dprint(DBG_OBJ|DBG_MM, "mr=" dprint_ptr_str() ", ofa_mr=" dprint_ptr_str() ", ofa_mr->device=" dprint_ptr_str() "\n",
		mr, ofa_mr, ofa_mr->device);


	if (!pbl) {
		dprint(DBG_ON, ": No PBL allocated\n");
		return -EINVAL;
	}
	if (pbl->max_buf < num_sle) {
		dprint(DBG_ON, ": Too many SG entries: %u : %u\n",
			mr->pbl->max_buf, num_sle);
		return -ENOMEM;
	}

	for_each_sg(sl, slp, num_sle, i) {
		if (sg_dma_len(slp) == 0)
			return -EINVAL;

		if (i == 0) {
			pble->addr = sg_dma_address(slp); //omril: why do we need DMA-able address if we are going to acess from CPU?
			pble->size = sg_dma_len(slp);
			pble->pbl_off = 0; //omril: Q: why offset=0 is assumed? A: coz it represents the offset from pbl
			pbl_size = pble->size;
			pbl->num_buf = 1;

			dprint(DBG_MM, " MEM(%d): SGE[%d], reg. %llu byte, "
				"addr " dprint_ptr_str() ", total %llu\n",
				OBJ_ID(&mr->mem), i, pble->size, (void *)pble->addr,
				pbl_size);

			continue;
		}
		if (pble->addr + pble->size != sg_dma_address(slp)) {
			pble++;
			pbl->num_buf++;
			pble->addr = sg_dma_address(slp);
			pble->size = sg_dma_len(slp);
			pble->pbl_off = pbl_size;
		} else
			//omril: append to current pble
			pble->size += sg_dma_len(slp);

		pbl_size += sg_dma_len(slp);

		dprint(DBG_MM, " MEM(%d): SGE[%d], reg. %llu byte, "
			"addr " dprint_ptr_str() ", total %llu\n",
			OBJ_ID(&mr->mem), i, pble->size, (void *)pble->addr,
			pbl_size);
	}
	rv = ib_sg_to_pages(ofa_mr, sl, num_sle, sg_off, siw_set_pbl_page);
	if (rv > 0) {
		//omril:
		//Q: where ofa_mr->length and  ofa_mr->iova getting assigned?
		//A: in ib_sg_to_pages()
		mr->mem.len = ofa_mr->length;
		mr->mem.va = ofa_mr->iova;
		dprint(DBG_MM, " MEM(%d): got %llu byte, %u SLE "
			"into %u entries\n",
			OBJ_ID(&mr->mem), mr->mem.len, num_sle, pbl->num_buf);
	}
	return rv;
}

//int siw_wr_reg_mr(struct ib_mr *ofa_mr, phys_addr_t *pages, int n_pages)
//{
//	struct siw_mr *mr = siw_mr_ofa2siw(ofa_mr);
//	struct siw_pbl *pbl = mr->pbl;
//	struct siw_pble *pble = pbl->pbe;
//	u64 pbl_size;
//	unsigned int mr_page_size = ofa_mr->page_size;
//	u32 first_offset;
//	u64 first_size, last_size, size;
//	int i, rv;
//
//	if (!pbl) {
//		dprint(DBG_ON, ": No PBL allocated\n");
//		return -EINVAL;
//	}
//	if (pbl->max_buf < n_pages) {
//		dprint(DBG_ON, ": Too many SG entries: %u : %u\n",
//			mr->pbl->max_buf, n_pages);
//		return -ENOMEM;
//	}
//	if (is_power_of_2(mr_page_size)) {
//		dprint(DBG_ON, ": mr_page_size not power of 2 (%u)\n",
//			mr_page_size);
//		return -EINVAL;
//	}
//
//	first_offset = (ofa_mr->iova & (mr_page_size - 1);
//	first_size = mr_page_size - first_offset;
//	last_size = ofa_mr->length - first_size - (n_pages - 2) * mr_page_size;
//	if (last_size <= 0 || last_size > mr_page_size) {
//		dprint(DBG_ON, ": Bad last_size=%llu (mr_page_size=%llu, length=%llu, "
//					   "first_size=%llu, n_pages=%d)\n",
//			last_size, mr_page_size, ofa_mr->length, first_size, n_pages);
//		return -EINVAL;
//	}
//
//
//	for (i = 0; i < n_pages; i++) {
//		if (page[i] & (PAGE_SIZE - 1)) {
//			dprint(DBG_ON, ": page %d is not aligned %x\n", i, page[i]);
//			return -EINVAL;
//		}
//
//		if (i == 0) {
//
//			dprint(DBG_ON, ": page[0] offset=%x\n", offset);
//
//			pble->addr = page[0] | first_offset;
//			pble->size = first_size;
//			pble->pbl_off = 0;
//			pbl_size = pble->size;
//			pbl->num_buf = 1;
//			continue;
//		}
//
//		size = (i == (n_pages - 1)) ? last_size : mr_page_size;
//		if (pble->addr + pble->size != page[i]) {
//			pble++;
//			pbl->num_buf++;
//			pble->addr = page[i];
//			pble->size = size;
//			pble->pbl_off = pbl_size;
//		} else
//			pble->size += size;
//
//
//	}
//}

/*
 * siw_get_dma_mr()
 *
 * Create a (empty) DMA memory region, where no umem is attached.
 * All DMA addresses are created via siw_dma_mapping_ops - which
 * will return just kernel virtual addresses, since siw runs on top
 * of TCP kernel sockets.
 */
struct ib_mr *siw_get_dma_mr(struct ib_pd *ofa_pd, int rights)
{
	struct siw_mr	*mr;
	struct siw_pd	*pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev	*sdev = pd->hdr.sdev;
	int rv;

	//dump_stack();
	if (atomic_inc_return(&sdev->num_mem) > SIW_MAX_MR) {
		dprint(DBG_ON, ": Out of MRs: %d\n",
			atomic_read(&sdev->num_mem));
		rv = -ENOMEM;
		goto err_out;
	}
	mr = siw_create_mr(sdev, NULL, 0, ULONG_MAX, rights);
	if (!mr) {
		rv = -ENOMEM;
		goto err_out;
	}
	mr->mem.stag_valid = 1;

	mr->pd = pd;
	siw_pd_get(pd);

	dprint(DBG_MM, ": MEM(%d): created DMA MR\n", OBJ_ID(&mr->mem));

	return &mr->ofa_mr;

err_out:
	atomic_dec(&sdev->num_mem);

	return ERR_PTR(rv);
}


/*
 * siw_create_srq()
 *
 * Create Shared Receive Queue of attributes @init_attrs
 * within protection domain given by @ofa_pd.
 *
 * @ofa_pd:	OFA PD contained in siw PD.
 * @init_attrs:	SRQ init attributes.
 * @udata:	not used by siw.
 */
#if !KS_IB_CREATE_SQR_HAS_IB_PD
static inline struct siw_srq *to_siw_srq(struct ib_srq *base_srq)
{
	return container_of(base_srq, struct siw_srq, ofa_srq);
}

int siw_create_srq(struct ib_srq *base_srq,
		   struct ib_srq_init_attr *init_attrs, struct ib_udata *udata)
{
	struct ib_pd *ofa_pd = base_srq->pd;
	struct siw_srq		*srq = to_siw_srq(base_srq);
	struct ib_srq_attr	*attrs = &init_attrs->attr;
	struct siw_pd		*pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev		*sdev = pd->hdr.sdev;
#else
struct ib_srq *siw_create_srq(struct ib_pd *ofa_pd,
			      struct ib_srq_init_attr *init_attrs,
			      struct ib_udata *udata)
{
	struct siw_srq		*srq = NULL;
	struct ib_srq_attr	*attrs = &init_attrs->attr;
	struct siw_pd		*pd = siw_pd_ofa2siw(ofa_pd);
	struct siw_dev		*sdev = pd->hdr.sdev;
#endif

	int kernel_verbs = ofa_pd->uobject ? 0 : 1;
	int rv;

	if (atomic_inc_return(&sdev->num_srq) > SIW_MAX_SRQ) {
		dprint(DBG_ON, " Out of SRQ's\n");
		rv = -ENOMEM;
		goto err_out;
	}
	if (attrs->max_wr == 0 || attrs->max_wr > SIW_MAX_SRQ_WR ||
	    attrs->max_sge > SIW_MAX_SGE || attrs->srq_limit > attrs->max_wr) {
		rv = -EINVAL;
		goto err_out;
	}

#if KS_IB_CREATE_SQR_HAS_IB_PD
	srq = kzalloc(sizeof *srq, GFP_KERNEL);
	if (!srq) {
		dprint(DBG_ON, " malloc\n");
		rv = -ENOMEM;
		goto err_out;
	}
#endif

	rv = siw_srq_add(sdev, srq);
	if (rv)
		goto err_out;
	init_completion(&srq->free_comp);

	srq->max_sge = attrs->max_sge;
	srq->num_rqe = roundup_pow_of_two(attrs->max_wr);
	atomic_set(&srq->space, srq->num_rqe);

	srq->limit = attrs->srq_limit;
	if (srq->limit)
		srq->armed = 1;

	INIT_WORK(&srq->event_work, siw_srq_event_work);
	atomic_set(&srq->event_work_sched, 0);

	if (kernel_verbs) {
		if (SIW_NO_VMALLOC_FOR_KVERBS_SRQ) {
			srq->recvq = (void *)__get_free_pages(GFP_KERNEL, get_order(srq->num_rqe * sizeof(struct siw_rqe)));
#ifdef SIW_DEBUG_SRQ
			srq->srqe_md = (void *)__get_free_pages(GFP_KERNEL, get_order(srq->num_rqe * sizeof(struct siw_srqe_md)));
#endif
		} else {
			srq->recvq = vmalloc(srq->num_rqe * sizeof(struct siw_rqe));
#ifdef SIW_DEBUG_SRQ
			srq->srqe_md = vmalloc(srq->num_rqe * sizeof(struct siw_srqe_md));
#endif
		}
	} else {
		srq->recvq = vmalloc_user(srq->num_rqe * sizeof(struct siw_rqe));
	}

	if (srq->recvq == NULL) {
		rv = -ENOMEM;
		goto err_out;
	}
	if (kernel_verbs) {
		memset(srq->recvq, 0, srq->num_rqe * sizeof(struct siw_rqe));
		srq->kernel_verbs = 1;
	}
	else if (udata) {
		struct siw_uresp_create_srq uresp;
		struct siw_ucontext *ctx;

		memset(&uresp, 0, sizeof uresp);
		ctx = siw_ctx_ofa2siw(ofa_pd->uobject->context);

		uresp.num_rqe = srq->num_rqe;
		uresp.srq_key = siw_insert_uobj(ctx, srq->recvq,
					srq->num_rqe * sizeof(struct siw_rqe));

		if (uresp.srq_key > SIW_MAX_UOBJ_KEY)
			pr_warn("Preparing mmap SRQ failed\n");

		rv = ib_copy_to_udata(udata, &uresp, sizeof uresp);
		if (rv)
			goto err_out;
	}
	srq->pd	= pd;
	siw_pd_get(pd);

	spin_lock_init(&srq->lock);

#if SIW_SRQ_WAIT_LIST
	INIT_LIST_HEAD(&srq->wait_qp_head);
#endif
	
#if SIW_SRQ_RQE_TRACK
	memset(&srq->rqe_track, 0, sizeof_field(typeof(*srq), rqe_track));
	INIT_LIST_HEAD(&srq->rqe_track.rsvd_qp_head);
	INIT_LIST_HEAD(&srq->rqe_track.recv_qp_head);
#endif

	dprint(DBG_OBJ|DBG_CM, ": new SRQ on device %s\n",
		sdev->ofa_dev.name);
#if !KS_IB_CREATE_SQR_HAS_IB_PD
	return 0;
#else
	return &srq->ofa_srq;
#endif

err_out:
	if (srq) {
		if (srq->hdr.id)
			siw_remove_obj(&sdev->idr_lock, &sdev->srq_idr, &srq->hdr);
		if (srq->recvq) {
			if (SIW_NO_VMALLOC_FOR_KVERBS_SRQ) {
				free_pages((unsigned long)srq->recvq, 
						   get_order(srq->num_rqe * sizeof(struct siw_rqe)));
#ifdef SIW_DEBUG_SRQ
				if (srq->srqe_md)
					free_pages((unsigned long)srq->srqe_md, 
						   get_order(srq->num_rqe * sizeof(struct siw_srqe_md)));
#endif
			}
			else {
				vfree(srq->recvq);
#ifdef SIW_DEBUG_SRQ
				if (srq->srqe_md)
					vfree(srq->srqe_md);
#endif
			}
		}
#if KS_IB_CREATE_SQR_HAS_IB_PD
		kfree(srq);
#else
		/* srq memory was allocated by RDMA layer so don't free it */
#endif
	}
	atomic_dec(&sdev->num_srq);

#if !KS_IB_CREATE_SQR_HAS_IB_PD
	return rv;
#else
	return ERR_PTR(rv);
#endif
}

/*
 * siw_modify_srq()
 *
 * Modify SRQ. The caller may resize SRQ and/or set/reset notification
 * limit and (re)arm IB_EVENT_SRQ_LIMIT_REACHED notification.
 *
 * NOTE: it is unclear if OFA allows for changing the MAX_SGE
 * parameter. siw_modify_srq() does not check the attrs->max_sge param.
 */
int siw_modify_srq(struct ib_srq *ofa_srq, struct ib_srq_attr *attrs,
		   enum ib_srq_attr_mask attr_mask, struct ib_udata *udata)
{
	struct siw_srq	*srq = siw_srq_ofa2siw(ofa_srq);
	unsigned long	flags;
	int rv = 0;

	lock_srq_rxsave(srq, flags);

	if (attr_mask & IB_SRQ_MAX_WR) {
		/* resize request not yet supported */
		rv = -EOPNOTSUPP;
		goto out;
	}
	if (attr_mask & IB_SRQ_LIMIT) {
		if (attrs->srq_limit) {
			if (unlikely(attrs->srq_limit > srq->num_rqe)) {
				rv = -EINVAL;
				/* FIXME: restore old space & max_wr?? */
				goto out;
			}
			srq->armed = 1;
		} else
			srq->armed = 0;

		srq->limit = attrs->srq_limit;
	}
out:
	unlock_srq_rxsave(srq, flags);

	return rv;
}

/*
 * siw_query_srq()
 *
 * Query SRQ attributes.
 */
int siw_query_srq(struct ib_srq *ofa_srq, struct ib_srq_attr *attrs)
{
	struct siw_srq	*srq = siw_srq_ofa2siw(ofa_srq);
	unsigned long	flags;

	lock_srq_rxsave(srq, flags);

	attrs->max_wr = srq->num_rqe;
	attrs->max_sge = srq->max_sge;
	attrs->srq_limit = srq->limit;

	unlock_srq_rxsave(srq, flags);

	return 0;
}

/*
 * siw_destroy_srq()
 *
 * Destroy SRQ.
 * It is assumed that the SRQ is not referenced by any
 * QP anymore - the code trusts the OFA environment to keep track
 * of QP references.
 */
#if KS_IB_DESTROY_SQR_HAS_IB_UDATA
#if KS_IB_DESTROY_SRQ_INT_RETURN
int siw_destroy_srq(struct ib_srq *ofa_srq, struct ib_udata *udata)
{
	struct siw_srq		*srq = siw_srq_ofa2siw(ofa_srq);
	struct siw_dev		*sdev = srq->pd->hdr.sdev;
	(void)udata;
#else
void siw_destroy_srq(struct ib_srq *ofa_srq, struct ib_udata *udata)
{
	struct siw_srq		*srq = siw_srq_ofa2siw(ofa_srq);
	struct siw_dev		*sdev = srq->pd->hdr.sdev;
	(void)udata;
#endif
#else
int siw_destroy_srq(struct ib_srq *ofa_srq)
{
	struct siw_srq		*srq = siw_srq_ofa2siw(ofa_srq);
	struct siw_dev		*sdev = srq->pd->hdr.sdev;
#endif
	dprint(DBG_OBJ, ": Destroy SRQ\n");
	
#if SIW_SRQ_WAIT_LIST
	BUG_ON(!list_empty(&srq->wait_qp_head));
#endif
	
#if SIW_SRQ_RQE_TRACK
	BUG_ON(!list_empty(&srq->rqe_track.rsvd_qp_head));
	BUG_ON(!list_empty(&srq->rqe_track.recv_qp_head));
	WARN_ON(srq->rqe_track.n_rsvd != 0 || srq->rqe_track.n_recv != 0);
#endif

	flush_work(&srq->event_work);
	BUG_ON(atomic_read(&srq->event_work_sched) != 0);

	siw_pd_put(srq->pd);

	siw_srq_put(srq);
	wait_for_completion(&srq->free_comp);

	if (SIW_NO_VMALLOC_FOR_KVERBS_SRQ) {
		free_pages((unsigned long)srq->recvq, get_order(srq->num_rqe * sizeof(struct siw_rqe)));
#ifdef SIW_DEBUG_SRQ
		if (srq->srqe_md)
			free_pages((unsigned long)srq->srqe_md, 
				   get_order(srq->num_rqe * sizeof(struct siw_srqe_md)));
#endif
	} else {
		vfree(srq->recvq);
#ifdef SIW_DEBUG_SRQ
		if (srq->srqe_md)
			vfree(srq->srqe_md);
#endif
	}
#if KS_IB_CREATE_SQR_HAS_IB_PD
	kfree(srq);
#endif

	atomic_dec(&sdev->num_srq);
#if KS_IB_DESTROY_SRQ_INT_RETURN
	return 0;
#endif
}

#ifdef SIW_DEBUG_SRQ
#include <linux/stacktrace.h>
#endif


/*
 * siw_post_srq_recv()
 *
 * Post a list of receive queue elements to SRQ.
 * NOTE: The function does not check or lock a certain SRQ state
 *       during the post operation. The code simply trusts the
 *       OFA environment.
 *
 * @ofa_srq:	OFA SRQ contained in siw SRQ
 * @wr:		List of R-WR's
 * @bad_wr:	Updated to failing WR if posting fails.
 * [Gregory]: consider several channels pushing into single SRQ
 */
#if KS_POST_SRQ_RECV_HAS_CONST
int siw_post_srq_recv(struct ib_srq *ofa_srq,
				const struct ib_recv_wr *wr,
				const struct ib_recv_wr **bad_wr)
#else
int siw_post_srq_recv(struct ib_srq *ofa_srq, struct ib_recv_wr *wr,
		      struct ib_recv_wr **bad_wr)
#endif
{
	struct siw_srq	*srq = siw_srq_ofa2siw(ofa_srq);
	int rv = 0;
	unsigned long flags;
	struct siw_qp *wait_qp;

	if (srq->kernel_verbs == 0) {
		dprint(DBG_WR|DBG_ON, "SRQ " dprint_ptr_str() ": mapped SRQ with OFA WR\n", srq);
		rv = -EINVAL;
		goto out;
	}

	lock_srq_rxsave(srq, flags);

	while (wr) {
		u32 idx = srq->rq_put % srq->num_rqe;
		struct siw_rqe *rqe = &srq->recvq[idx];
		uint32_t qflags = _load_shared(rqe->flags);

		if (qflags) {
			dprint(DBG_WR|DBG_ON, "(SRQ%d/" dprint_ptr_str() ") full idx=%u flags=%x\n", srq->hdr.id, srq, idx, rqe->flags);
			BUG();
			rv = -ENOMEM;
			break;
		}
		if (wr->num_sge > srq->max_sge) {
			dprint(DBG_WR|DBG_ON, "Num SGE: %d\n", wr->num_sge);
			rv = -EINVAL;
			break;
		}

#ifdef SIW_DEBUG_SRQ
		if (srq->kernel_verbs) {
			struct siw_srqe_md *srqe_md = &srq->srqe_me[idx];
			u32 i;
			struct stack_trace st = {
				.entries = srqe_md->post_bt,
				.max_entries = ARRAY_SIZE(srqe_md->post_bt),
				.skip = 1,
			};
			//for (i = srq->rq_get; i != srq->rq_put; i++) {
			for (i = 0; i < srq->num_rqe; i++) {
				struct siw_rqe *chk_rqe = &srq->recvq[i % srq->num_rqe];
				if (chk_rqe->id == wr->wr_id && _load_shared(chk_rqe->flags)) {
					pr_err("SIW: SRQ " dprint_ptr_str() " double-post of id 0x%llx in wr " dprint_ptr_str() ". Previous post in idx %u - Post call-stack %pF <- %pF <- %pF <- %pF <- %pF\n",
					       srq, wr->wr_id, wr, i, (void *)srqe_md->post_bt[0], (void *)srqe_md->post_bt[1], (void *)srqe_md->post_bt[2], (void *)srqe_md->post_bt[3], (void *)srqe_md->post_bt[4]);
					BUG_ON(1);
				}
			}
			save_stack_trace(&st);
			srqe_md->post_pid = current->pid;
			trace_printk("SRQ " dprint_ptr_str() " posting wr_id %llx - Post call-stack %pF <- %pF <- %pF <- %pF <- %pF\n", 
				     srq, wr->wr_id, (void *)srqe_md->post_bt[0], (void *)srqe_md->post_bt[1], (void *)srqe_md->post_bt[2], (void *)srqe_md->post_bt[3], (void *)srqe_md->post_bt[4]);
			for (i = 0; i < wr->num_sge; i++) {
				struct siw_mem *mem = siw_mem_id2obj(srq->pd->hdr.sdev, wr->sg_list[i].lkey >> 8);
				if (mem) {
					struct siw_mr *mr = siw_mem2mr(mem);
					if (!mr || !mr->mem_obj) {
						/* Direct Kernel Address */
						/*trace_printk("SRQ " dprint_ptr_str() " wr_id %llx sge[%d] - filling " dprint_ptr_str() " with %u bytes of 0xcc\n",
								srq, wr->wr_id, i, (void *)wr->sg_list[i].addr, wr->sg_list[i].length);
						memset((void *)wr->sg_list[i].addr, 0xcc, wr->sg_list[i].length);*/
					} else {
						/* TBD: Add memset of PBL/user mem */
					}
					siw_mem_put(mem);
				}
			}
		}
#endif

#if SIW_SRQ_WAIT_LIST
		while ((wait_qp = list_first_entry_or_null(&srq->wait_qp_head, struct siw_qp, srq_wait_link))) {
			bool locked;
			if (!(locked = down_read_trylock(&wait_qp->state_lock)) || wait_qp->attrs.state != SIW_QP_STATE_RTS) {
				dprint(DBG_ON | DBG_RX, "(SRQ%d/" dprint_ptr_str() "): Waiting QP(%d) is going down. Removing from wait list\n", 
				       srq->hdr.id, srq, QP_ID(wait_qp));
				list_del_init(&wait_qp->srq_wait_link);
				if (locked)
					up_read(&wait_qp->state_lock);
				continue;
			} else {
				u32 rqe_ready_count = _load_shared(wait_qp->srq_rqe_ready_count);
				dprint(DBG_ON | DBG_RX, "(SRQ%d/" dprint_ptr_str() "): Passing RQE %llx to waiting QP(%d)\n", 
					srq->hdr.id, srq, wr->wr_id, QP_ID(wait_qp));
				list_del_init(&wait_qp->srq_wait_link);
				rqe = &wait_qp->srq_rqe_ready;
				BUG_ON(_load_shared(rqe->flags));
				smp_store_mb(wait_qp->srq_rqe_ready_count, rqe_ready_count++);
				break;
			}
		}
#endif
		rqe->id = wr->wr_id;
		rqe->num_sge = wr->num_sge;
		rqe->srq_id = srq->hdr.id;
		siw_copy_sgl(wr->sg_list, rqe->sge, wr->num_sge);

		smp_wmb();

		rqe->flags = SIW_WQE_VALID; /* qp->srq_rqe_ready.flags <-- SIW_WQE_VALID */

#if SIW_SRQ_RQE_TRACK
		srq->rqe_track.last_ret_jif = jiffies;
		srq->rqe_track.n_q++;
		srq->rqe_track.max_q = max(srq->rqe_track.n_q, srq->rqe_track.max_q);
		/* We assume that if there are QPs attached to the SRQ, then this a return from ULP */
		if (srq->rqe_track.n_qp > 0)
			srq->rqe_track.n_ulp--;
		if (wait_qp) {
			BUG_ON(!list_empty(&wait_qp->srq_rqe_link) || wait_qp->srq_rqe_state != SRQ_RQE_IDLE);
			list_add_tail(&wait_qp->srq_rqe_link, &srq->rqe_track.rsvd_qp_head);
			wait_qp->srq_rqe_state = SRQ_RQE_RSVD;
			srq->rqe_track.n_q--;
			srq->rqe_track.n_rsvd++;
		}
#endif

		if (!wait_qp) {
			srq->rq_put++;
		}
		else {
			/* we have populated @qp->srq_rqe_ready with a returned @wr and
			   set its flags to SIW_WQE_VALID. Now, (instead of putting it
			   back to SRQ) we schedule siw_retry_get_rqe_work() so this qp
			   can use this rqe to resume rx */
			siw_rx_queue_work(wait_qp, 0);
			up_read(&wait_qp->state_lock);
		}
		wr = wr->next;
	}
	unlock_srq_rxsave(srq, flags);
out:
	if (unlikely(rv < 0)) {
		dprint(DBG_WR|DBG_ON, "(SRQ " dprint_ptr_str() "): error=%d\n",
			srq, rv);
		*bad_wr = wr;
	}
	return rv;
}

int siw_return_reserved_rqe(struct siw_srq *srq, struct siw_rqe *ret_rqe, bool already_locked)
{
	struct siw_qp *wait_qp;
	u32 idx = ~(u32)0;
	struct siw_rqe *rqe;
	uint32_t qflags;
	unsigned long flags = 0;
	int rv;

	if (!already_locked)
		lock_srq_rxsave(srq, flags);
	
	if (ret_rqe->srq_id != srq->hdr.id) {
		dprint(DBG_RX|DBG_ON, "(SRQ%d): RQE has invalid SRQ ID: %d\n", 
		       srq->hdr.id, ret_rqe->srq_id);
		rv = -EINVAL;
		BUG();
		goto unlock;
	}

	if (ret_rqe->num_sge > srq->max_sge) {
		dprint(DBG_RX|DBG_ON, "Num SGE: %d\n", ret_rqe->num_sge);
		rv = -EINVAL;
		BUG();
		goto unlock;
	}

	if (ret_rqe->flags != SIW_WQE_VALID) {
		dprint(DBG_RX|DBG_ON, "Invalid flags: %d\n", ret_rqe->flags);
		rv = -EINVAL;
		BUG();
		goto unlock;
	}

	while ((wait_qp = list_first_entry_or_null(&srq->wait_qp_head, struct siw_qp, srq_wait_link))) {
		bool locked;
		if (!(locked = down_read_trylock(&wait_qp->state_lock)) || wait_qp->attrs.state != SIW_QP_STATE_RTS) {
			dprint(DBG_ON | DBG_RX, "(SRQ%d/" dprint_ptr_str() "): Waiting QP(%d) is going down. Removing from wait list\n", 
			       srq->hdr.id, srq, QP_ID(wait_qp));
			list_del_init(&wait_qp->srq_wait_link);
			if (locked)
				up_read(&wait_qp->state_lock);
			continue;
		} else {
			u32 rqe_ready_count = _load_shared(wait_qp->srq_rqe_ready_count);
			dprint(DBG_ON | DBG_RX, "(SRQ%d/" dprint_ptr_str() "): Passing RQE %llx to waiting QP(%d)\n", 
				srq->hdr.id, srq, ret_rqe->id, QP_ID(wait_qp));
			list_del_init(&wait_qp->srq_wait_link);
			rqe = &wait_qp->srq_rqe_ready;
			BUG_ON(_load_shared(rqe->flags));
			smp_store_mb(wait_qp->srq_rqe_ready_count, rqe_ready_count++);
			break;
		}
	}
	if (!wait_qp) {
		idx = srq->rq_put % srq->num_rqe;
		rqe = &srq->recvq[idx];
		qflags = _load_shared(rqe->flags);
		
		if (qflags) {
			dprint(DBG_WR|DBG_ON, "(SRQ%d/" dprint_ptr_str() ") full idx=%u flags=%x\n", srq->hdr.id, srq, idx, qflags);
			BUG();
			rv = -ENOMEM;
			goto unlock;
		}
	}

	rqe->id = ret_rqe->id;
	rqe->num_sge = ret_rqe->num_sge;
	rqe->srq_id = srq->hdr.id;
	memcpy(rqe->sge, ret_rqe->sge, sizeof(rqe->sge));

	smp_store_mb(rqe->flags, SIW_WQE_VALID);

#if SIW_SRQ_RQE_TRACK
	BUG_ON(ret_rqe->srq_id != srq->hdr.id);
	srq->rqe_track.last_ret_jif = jiffies;
	if (!wait_qp) {
		srq->rqe_track.n_q++;
	} else {
		srq->rqe_track.n_rsvd++;
		BUG_ON(!list_empty(&wait_qp->srq_rqe_link) || wait_qp->srq_rqe_state != SRQ_RQE_IDLE);
		list_add_tail(&wait_qp->srq_rqe_link, &srq->rqe_track.rsvd_qp_head);
		wait_qp->srq_rqe_state = SRQ_RQE_RSVD;
	}
#endif

	if (!wait_qp) {
		dprint(DBG_WR|DBG_ON, "(SRQ%d/" dprint_ptr_str() ") returned RQE %llx to index %u (" dprint_ptr_str() ")\n", 
		       srq->hdr.id, srq, rqe->id, idx, rqe);
		srq->rq_put++;
	} else {
		siw_rx_queue_work(wait_qp, 0);
		up_read(&wait_qp->state_lock);
	}

	rv = 0;

unlock:
	if (!already_locked)
		unlock_srq_rxsave(srq, flags);
	return rv;
}


