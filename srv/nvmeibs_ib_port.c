#include "nvmeib.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_ib_port.h"
#include "nvmeibs_client.h"
#include "nvmeibs_main.h"
#include "nvmeibs_toma.h"
#include "nvmeibs_net.h"
#include "nvmeibs_test.h"
#include "nvmeib_utils.h"
#include "nvmeibs_nordda.h"
#include "nvmeib_srq.h"
#include "nvmeib_version_shared.h"
#include "nvmeibs_trace.h"
#include "nvmeib_public_procfs.h"

static unsigned int nvmeibs_tcp_port_prio = NVMEIB_TCP_PORT_PRIORITY;
module_param_named(tcp_port_prio, nvmeibs_tcp_port_prio, uint, 0644);
MODULE_PARM_DESC(tcp_port_prio, "TCP Port Priority");

static unsigned int nvmeibs_roce_port_prio = NVMEIB_ROCE_PORT_PRIORITY;
module_param_named(roce_port_prio, nvmeibs_roce_port_prio, uint, 0644);
MODULE_PARM_DESC(roce_port_prio, "RoCE Port Priority");

static unsigned int nvmeibs_ib_port_prio = NVMEIB_IB_PORT_PRIORITY;
module_param_named(ib_port_prio, nvmeibs_ib_port_prio, uint, 0644);
MODULE_PARM_DESC(ib_port_prio, "IB Port Priority");

/**
 * nvmeibs_mad_send_handler() - Post MAD-send callback function.
 */
static void __attribute__ ((unused))
mad_send_handler(struct ib_mad_agent *mad_agent,
	struct ib_mad_send_wc *mad_wc)
{
	NFIN;
	ib_destroy_ah(mad_wc->send_buf->ah);
	ib_free_send_mad(mad_wc->send_buf);
	NFOUT;
}

/**
 * nvmeibs_get_class_port_info() - Copy ClassPortInfo to a
 * management datagram.
 *
 * See also section 16.3.3.1 ClassPortInfo in the InfiniBand Architecture
 * Specification.
 */
static void get_class_port_info(struct ib_dm_mad *mad)
{
	struct ib_class_port_info *cif;

	NFIN;
	cif = (struct ib_class_port_info *)mad->data;
	memset(cif, 0, sizeof *cif);
	cif->base_version = 1;
	cif->class_version = 1;
#if IB_SET_CPI_RESP_TIME
	ib_set_cpi_resp_time(cif, 20);
#else
	cif->resp_time_value = 20;
#endif
	mad->mad_hdr.status = 0;
	NFOUT;
}

/**
 * nvmeibs_set_ioc() - Helper function for initializing an
 * IOUnitInfo structure.
 *
 * @slot: one-based slot number.
 * @value: four-bit value.
 *
 * Copies the lowest four bits of value in element slot of the array of four
 * bit elements called c_list (controller list). The index slot is one-based.
 */
static void set_ioc(u8 *c_list, unsigned slot, u8 value)
{
	u16 id;
	u8 tmp;

	NFIN;
	id = (slot - 1) / 2;
	if (slot & 0x1) {
		tmp = c_list[id] & 0xf;
		c_list[id] = (value << 4) | tmp;
	} else {
		tmp = c_list[id] & 0xf0;
		c_list[id] = (value & 0xf) | tmp;
	}
	NFOUT;
}

/**
 * nvmeibs_get_iou() - Write IOUnitInfo to a management
 * datagram.
 *
 * See also section 16.3.3.3 IOUnitInfo in the InfiniBand Architecture
 * Specification.
 */
static void get_iou(struct ib_dm_mad *mad)
{
	struct ib_dm_iou_info *ioui;
	u8 slot;
	int i;

	NFIN;
	ioui = (struct ib_dm_iou_info *)mad->data;
	ioui->change_id = __constant_cpu_to_be16(1);
	ioui->max_controllers = 16;
	/* set present for slot 1 and empty for the rest */
	set_ioc(ioui->controller_list, 1, 1);
	for (i = 1, slot = 2; i < 16; i++, slot++)
		set_ioc(ioui->controller_list, slot, 0);
	mad->mad_hdr.status = 0;
	NFOUT;
}

/**
 * nvmeibs_get_ioc() - Write IOControllerprofile to a management
 * datagram.
 *
 * See also section 16.3.3.4 IOControllerProfile in the InfiniBand
 * Architecture Specification.
 */
static void get_ioc(struct nvmeibs_ib_port *ib_port, unsigned slot,
	struct ib_dm_mad *mad)
{
	struct nvmeibs_dev *nis_dev = ib_port->nis_dev;
	struct nvmeib_dev *dev = P2NV(ib_port);
	struct ib_dm_ioc_profile *iocp;
	struct nvmeib_srq_params prim_srq;

	NFIN;
	iocp = (struct ib_dm_ioc_profile *)mad->data;
	if (!slot || slot > 16) {
		mad->mad_hdr.status =
			__constant_cpu_to_be16(DM_MAD_STATUS_INVALID_FIELD);
		NFOUT;
		return;
	}

	if (slot > 2) {
		mad->mad_hdr.status = __constant_cpu_to_be16(DM_MAD_STATUS_NO_IOC);
		NFOUT;
		return;
	}

	if (nvmeib_srq_pool_query(nis_dev->dev, &prim_srq, NULL) < 0) {
		mad->mad_hdr.status = __constant_cpu_to_be16(DM_MAD_STATUS_NO_IOC);
		NFOUT;
		return;
	}

	memset(iocp, 0, sizeof *iocp);
	strcpy(iocp->id_string, NVMEIBS_ID_STRING);
	iocp->guid = cpu_to_be64(nvmeibs_get_service_guid());
	iocp->vendor_id = cpu_to_be32(dev->dev_attr->vendor_id);
	iocp->device_id = cpu_to_be32(dev->dev_attr->vendor_part_id);
	iocp->device_version = cpu_to_be16(dev->dev_attr->hw_ver);
	iocp->subsys_vendor_id = cpu_to_be32(dev->dev_attr->vendor_id);
	iocp->subsys_device_id = 0x0;
	iocp->io_class = __constant_cpu_to_be16(NVMEIB_IB_IO_CLASS);
	iocp->io_subclass = __constant_cpu_to_be16(NVMEIB_IO_SUBCLASS);
	iocp->protocol = __constant_cpu_to_be16(NVMEIB_PROTOCOL);
	iocp->protocol_version = __constant_cpu_to_be16(NVMEIB_PROTOCOL_VERSION);
	iocp->send_queue_depth = cpu_to_be16(prim_srq.q_size);
	iocp->rdma_read_depth = 4;
	iocp->send_size = cpu_to_be32(nvmeibs_get_max_req_size());
	iocp->rdma_size = cpu_to_be32(min(ib_port->port_attrib.max_rdma_size,
		1U << 24));
	iocp->num_svc_entries = 1;
	iocp->op_cap_mask = NVMEIBS_SEND_TO_IOC | NVMEIBS_SEND_FROM_IOC |
		NVMEIBS_RDMA_READ_FROM_IOC | NVMEIBS_RDMA_WRITE_FROM_IOC;
	mad->mad_hdr.status = 0;
	NFOUT;
}

/**
 * nvmeibs_get_svc_entries() - Write ServiceEntries to a
 * management datagram.
 *
 * See also section 16.3.3.5 ServiceEntries in the InfiniBand Architecture
 * Specification.
 */
static void get_svc_entries(u64 ioc_guid, u16 slot, u8 hi, u8 lo,
	struct ib_dm_mad *mad)
{
	struct ib_dm_svc_entries *svc_entries;

	NFIN;
	WARN_ON(!ioc_guid);

	if (!slot || slot > 16) {
		mad->mad_hdr.status =
			__constant_cpu_to_be16(DM_MAD_STATUS_INVALID_FIELD);
		NFOUT;
		return;
	}

	if (slot > 2 || lo > hi || hi > 1) {
		mad->mad_hdr.status = __constant_cpu_to_be16(DM_MAD_STATUS_NO_IOC);
		NFOUT;
		return;
	}

	svc_entries = (struct ib_dm_svc_entries *)mad->data;
	memset(svc_entries, 0, sizeof *svc_entries);
	svc_entries->service_entries[0].id = cpu_to_be64(ioc_guid);
	snprintf(svc_entries->service_entries[0].name,
		sizeof(svc_entries->service_entries[0].name), "%s%016llx",
		NVMEIB_SERVICE_NAME_PREFIX, ioc_guid);
	mad->mad_hdr.status = 0;
	NFOUT;
}

/**
 * nvmeibs_mgmt_method_get() - Process a received management
 * datagram.
 * @sp:      source port through which the MAD has been received.
 * @rq_mad:  received MAD.
 * @rsp_mad: response MAD.
 */
static void mgmt_method_get(struct nvmeibs_ib_port *ib_port,
	struct ib_mad *rq_mad, struct ib_dm_mad *rsp_mad)
{
	u16 attr_id;
	unsigned slot;
	u8 hi, lo;

	NFIN;
	attr_id = be16_to_cpu(rq_mad->mad_hdr.attr_id);
	switch (attr_id) {
	case DM_ATTR_CLASS_PORT_INFO:
		get_class_port_info(rsp_mad);
		break;
	case DM_ATTR_IOU_INFO:
		get_iou(rsp_mad);
		break;
	case DM_ATTR_IOC_PROFILE:
		slot = be32_to_cpu(rq_mad->mad_hdr.attr_mod);
		get_ioc(ib_port, slot, rsp_mad);
		break;
	case DM_ATTR_SVC_ENTRIES:
		slot = be32_to_cpu(rq_mad->mad_hdr.attr_mod);
		hi = (u8) ((slot >> 8) & 0xff);
		lo = (u8) (slot & 0xff);
		slot = (u16) ((slot >> 16) & 0xffff);
		get_svc_entries(nvmeibs_get_service_guid(), slot, hi, lo, rsp_mad);
		break;
	default:
		rsp_mad->mad_hdr.status =
		    __constant_cpu_to_be16(DM_MAD_STATUS_UNSUP_METHOD_ATTR);
		break;
	}
	NFOUT;
}

/**
 * nvmeibs_mad_recv_handler() - MAD reception callback function.
 */
static void __attribute__ ((unused))
mad_recv_handler(struct ib_mad_agent *mad_agent,
	struct ib_mad_recv_wc *mad_wc)
{
	struct nvmeibs_ib_port *ib_port =
		(struct nvmeibs_ib_port *)mad_agent->context;
	struct ib_ah *ah;
	struct ib_mad_send_buf *rsp;
	struct ib_dm_mad *dm_mad;

	NFIN;
	if (!mad_wc || !mad_wc->recv_buf.mad) {
		NFOUT;
		return;
	}

	ah = ib_create_ah_from_wc(mad_agent->qp->pd, mad_wc->wc,
		mad_wc->recv_buf.grh, mad_agent->port_num);
	if (IS_ERR(ah))
		goto err;

	BUILD_BUG_ON(offsetof(struct ib_dm_mad, data) != IB_MGMT_DEVICE_HDR);

	rsp = ib_create_send_mad(mad_agent,
		mad_wc->wc->src_qp, mad_wc->wc->pkey_index, 0,
		IB_MGMT_DEVICE_HDR, IB_MGMT_DEVICE_DATA, GFP_KERNEL
#if IB_NEW_FR
		, IB_MGMT_BASE_VERSION
#endif
		);
	if (IS_ERR(rsp))
		goto err_rsp;

	rsp->ah = ah;
	dm_mad = rsp->mad;
	memcpy(dm_mad, mad_wc->recv_buf.mad, sizeof *dm_mad);
	dm_mad->mad_hdr.method = IB_MGMT_METHOD_GET_RESP;
	dm_mad->mad_hdr.status = 0;

	switch (mad_wc->recv_buf.mad->mad_hdr.method) {
	case IB_MGMT_METHOD_GET:
		mgmt_method_get(ib_port, mad_wc->recv_buf.mad, dm_mad);
		break;
	case IB_MGMT_METHOD_SET:
		dm_mad->mad_hdr.status =
		    __constant_cpu_to_be16(DM_MAD_STATUS_UNSUP_METHOD_ATTR);
		break;
	default:
		dm_mad->mad_hdr.status =
			__constant_cpu_to_be16(DM_MAD_STATUS_UNSUP_METHOD);
		break;
	}

	if (!ib_post_send_mad(rsp, NULL)) {
		ib_free_recv_mad(mad_wc);
		/* will destroy_ah & free_send_mad in send completion */
		NFOUT;
		return;
	}

	ib_free_send_mad(rsp);

err_rsp:
	ib_destroy_ah(ah);

err:
	ib_free_recv_mad(mad_wc);
	NFOUT;
}

static void set_port_unused(struct nvmeibs_ib_port *ib_port)
{
	struct nvmeibs_dev *nis_dev = ib_port->nis_dev;

	if (ib_port->loop_listener) {
		nvmeib_rdma_destroy_cm(ib_port->loop_listener);
		ib_port->loop_listener = NULL;
	}
	if (ib_port->gids_csv_proc_ent) {
		nvmeib_public_proc_remove(ib_port->gids_csv_proc_ent);
		ib_port->gids_csv_proc_ent = NULL;
	}
	ib_port->port_used = false;
	/* Lock the nvmeibs_dev_guard as we are modifying the port list */
	nvmeibs_get_devices(NULL);
	list_del(&ib_port->port_list_n);
	if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
		nis_dev->ib_ports--;
	else if (ib_port->layer == IB_LINK_LAYER_ETHERNET) {
		if (ib_port->transport == RDMA_TRANSPORT_IWARP)
			nis_dev->iwarp_ports--;
		else
			nis_dev->roce_ports--;
	}
	list_add_tail(&ib_port->port_list_n, &nis_dev->unused_port_list);
	nis_dev->unused_ports++;
	/* Unlock the nvmeibs_dev_guard */
	nvmeibs_put_devices();
}

extern struct proc_dir_entry *nvmeibs_gids_proc_dir;

static ssize_t fill_port_gids(void *arg, char *buffer, size_t len)
{
	struct nvmeibs_ib_port *ib_port = arg;
	struct nvmeib_rdma_ib_port_gid *gid_iter;
	int count = 0;
	char buf_ip_str[48];

	count += scnprintf(buffer + count, len - count,
		"%s\n", NVMEIBS_SRV_SW_GIDS_CSV_HEADER);
	list_for_each_entry(gid_iter, &ib_port->gid_list, link) {
		count += scnprintf(buffer + count, len - count,
				"%d,0x%016llx%016llx,%s,%s,%s,%s,%s,%u,%s\n",
			gid_iter->gid_index,
			be64_to_cpu(gid_iter->gid.global.subnet_prefix),
			be64_to_cpu(gid_iter->gid.global.interface_id),
			nvmeib_rdma_gid_type_str(gid_iter->gid_type, gid_iter->link_layer),
			nvmeib_rdma_net_type_str(gid_iter->net_type),
			nvmeib_rdma_gid_ip_str(buf_ip_str, &gid_iter->gid, gid_iter->net_type),
			gid_iter->ndev_name,
			gid_iter->is_vlan ? "true" : "false",
			gid_iter->vlan_id,
			gid_iter->preferred ? "true" : "false");
	}
	return count;
}

static void create_nic_gid_csv(struct nvmeibs_ib_port *ib_port)
{
	char gids_csv_fname[NVMEIB_GID_STR_MAX + strlen(".csv")];
	/* Create entry in /proc/nvmeibs/gids */
	sprintf(gids_csv_fname, "%016llx%016llx.csv",
			be64_to_cpu(ib_port->hw_gid.global.subnet_prefix),
			be64_to_cpu(ib_port->hw_gid.global.interface_id));
	ib_port->gids_csv_proc_ent = nvmeib_public_proc_create(gids_csv_fname,
													nvmeibs_gids_proc_dir, fill_port_gids, NULL, ib_port);
}

static void set_port_used(struct nvmeibs_ib_port *ib_port)
{
	struct nvmeibs_dev *nis_dev = ib_port->nis_dev;

	ib_port->port_used = true;
	/* Lock the nvmeibs_dev_guard as we are modifying the port list */
	nvmeibs_get_devices(NULL);
	list_del(&ib_port->port_list_n);
	nis_dev->unused_ports--;
	list_add_tail(&ib_port->port_list_n, &nis_dev->port_list);
	if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
		nis_dev->ib_ports++;
	else if (ib_port->layer == IB_LINK_LAYER_ETHERNET) {
		if (ib_port->transport == RDMA_TRANSPORT_IWARP)
			nis_dev->iwarp_ports++;
		else
			nis_dev->roce_ports++;
	}
	/* Unlock the nvmeibs_dev_guard */
	nvmeibs_put_devices();
	nvmeibs_init_loopback_listener(ib_port);
	create_nic_gid_csv(ib_port);
}

static void free_gid_list(struct nvmeibs_ib_port *ib_port)
{
	struct nvmeib_rdma_ib_port_gid *gid_iter;
	while ((gid_iter = list_first_entry_or_null(&ib_port->gid_list,
		struct nvmeib_rdma_ib_port_gid, link))) {
		list_del(&gid_iter->link);
		kfree(gid_iter);
	}
}

static void replace_gid_list(struct nvmeibs_ib_port *ib_port, struct list_head *new_gid_list)
{
	struct nvmeib_rdma_ib_port_gid *gid_iter;

	_NT(t0_replace_gid_list,
		"@DEV_NAME:@PORT_NUM, replace gid-list",
		nvmeibs_device_name(ib_port->nis_dev), ib_port->port);

	free_gid_list(ib_port);
	list_splice(new_gid_list, &ib_port->gid_list);
	ib_port->n_gids = 0;
	ib_port->gid.gid.global.interface_id = 0;
	ib_port->gid.gid.global.subnet_prefix = 0;
	ib_port->gid.valid = false;
	list_for_each_entry(gid_iter, &ib_port->gid_list, link) {
		_NT(t1_replace_gid_list,
			"[@INT] gid=@GUID_RAW, v=@BOOL, p=@BOOL",
			ib_port->n_gids, &gid_iter->gid,
			ib_port->gid.valid, gid_iter->preferred);

		if (!ib_port->gid.valid || gid_iter->preferred) {
			ib_port->gid = (*gid_iter);
			INIT_LIST_HEAD(&ib_port->gid.link);
		}
		ib_port->n_gids++;
	}

	_NT(t2_replace_gid_list,
		"Curr gid: valid=@BOOL, used=@BOOL, enabled=@BOOL, @GUID_RAW",
		ib_port->gid.valid, ib_port->port_used, nvmeibs_ib_port_enabled(ib_port),
		&ib_port->gid.gid);
}

/**
 * nvmeibs_refresh_port() - Configure a HCA port.
 *
 * Enable InfiniBand management datagram processing, update the cached sm_lid,
 * lid and gid values, and register a callback function for processing MADs
 * on the specified port.
 *
 * Note: It is safe to call this function more than once for the same port.
 */
static int refresh_port(struct nvmeibs_ib_port *ib_port, bool gid_change, bool port_error)
{
	struct ib_port_modify port_modify;
	struct ib_port_attr port_attr;
	union ib_gid curr_gid;
	int rv = 0;
	struct nvmeibs_dev *nis_dev = ib_port->nis_dev;
	enum rdma_link_layer new_link_layer = rdma_port_get_link_layer(P2IB(ib_port), ib_port->port);
	bool link_layer_change = ib_port->layer != new_link_layer;
	bool new_link_layer_compat = new_link_layer == nvmeibs_selected_layer || nvmeibs_selected_layer == IB_LINK_LAYER_UNSPECIFIED;
	bool port_was_used = ib_port->port_used;
	bool port_was_enabled = nvmeibs_ib_port_enabled(ib_port);
	struct list_head new_gid_list = LIST_HEAD_INIT(new_gid_list);
	bool port_used, tcp_toggle;

	NFIN;

	_NT(trace_ib_port_refresh_port,
		"Refresh port called for @IB_DEV_NAME(@IB_PORT), gid_change=@BOOL,\
		 port_error=@BOOL, port_was_used=@BOOL, port_was_enabled=@BOOL, link_layer_change=@BOOL"
		,P2IB(ib_port)->name, ib_port, gid_change, port_error, port_was_used, port_was_enabled, link_layer_change);

	if ((rv = ib_query_port(P2IB(ib_port), ib_port->port, &port_attr))) {
		_NT(trace_1_ib_port_refresh_port, "ib_query_port() failed.");
		goto err_query_port;
	}

	ib_port->sm_lid = port_attr.sm_lid;
	ib_port->lid = port_attr.lid;
	ib_port->layer = new_link_layer;
	ib_port->port_active = port_attr.state == IB_PORT_ACTIVE;

	if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
		ib_port->transport_priority = nvmeibs_ib_port_prio;
	else if (ib_port->layer == IB_LINK_LAYER_ETHERNET) {
		if (ib_port->transport == RDMA_TRANSPORT_IB) {
			ib_port->transport_priority = nvmeibs_roce_port_prio;
		} else if (ib_port->transport == RDMA_TRANSPORT_IWARP) {
			ib_port->transport_priority = nvmeibs_tcp_port_prio;
		}
	}

	if ((rv = ib_query_pkey(P2IB(ib_port), ib_port->port, 0,
		&ib_port->pkey))) {
		_NT(trace_2_ib_port_refresh_port, "ib_query_pkey() failed.");
		goto err_query_port;
	}

	/* check if tcp port was ifdown/ifup, see siw_query_port for implementation */
	tcp_toggle = (ib_port->transport == RDMA_TRANSPORT_IWARP) &&
	((port_was_enabled && port_attr.phys_state == 3) || (!port_was_enabled \
	 && port_attr.phys_state == 5));

	_NT(trace_ib_port_refresh_port_after_queries,
		"Refresh port after queries for @IB_DEV_NAME(@IB_PORT): "
		"port_active=@BOOL, phys_state=@BOOL, (tcp_toggle=@BOOL)",
		P2IB(ib_port)->name, ib_port, ib_port->port_active,
		port_attr.phys_state, tcp_toggle);

	if ((ib_port->port_active && (gid_change || link_layer_change)) || tcp_toggle) {
		curr_gid = ib_port->gid.gid;

		port_used = nvmeib_use_dev(nvmeibs_get_used_dev_list(), P2IB(ib_port),
				ib_port->port, &new_gid_list);
		replace_gid_list(ib_port, &new_gid_list);

		_NT(trace_ib_port_refresh_port_port_used,
		"Refresh port after nvmeib_use_dev for @IB_DEV_NAME(@IB_PORT): port_used=@BOOL",
		P2IB(ib_port)->name, ib_port, port_used);
		/* Check to see if the port/dev is still enabled (new GID may be filtered out) */
		if (!port_used || !new_link_layer_compat) {
			if (ib_port->port_used) {
				_NT(trace_3_ib_port_refresh_port, "Device @IB_DEV_NAME port @PORT is now unused due to GID / link-layer change",
					P2IB(ib_port)->name, ib_port->port);
				set_port_unused(ib_port);
			}
		} else {
			if (!ib_port->port_used) {
				_NT(trace_4_ib_port_refresh_port, "Device @IB_DEV_NAME port @PORT is now used due to GID / link-layer change",
					P2IB(ib_port)->name, ib_port->port);
				if (!nis_dev->device_used) {
					/* Device has now become used */
					if ((rv = nvmeibs_activate_device(nis_dev, false)) < 0) {
						_NE(error_ib_port_refresh_port, "Failed to activate device @RV", rv);
						goto out;
					}
				}
				set_port_used(ib_port);
				if (nvmeibs_selected_layer == IB_LINK_LAYER_UNSPECIFIED) {
					nvmeibs_selected_layer = ib_port->layer;

					if (nvmeibs_selected_layer == IB_LINK_LAYER_ETHERNET) {
						/* Start RoCE Listener */
						nvmeibs_start_roce();
					}
				}
			}
		}

		if (memcmp(curr_gid.raw, ib_port->gid.gid.raw, sizeof(curr_gid.raw))) {
			_NT(trace_5_ib_port_refresh_port, "Device @IB_DEV_NAME port @PORT GID change from @CURR_GID to @GID_IPV6",
			   P2IB(ib_port)->name, ib_port->port, &curr_gid, &ib_port->gid.gid);
			nvmeibs_toma_report_event_port_gid_change(ib_port);
			nvmeibs_disk_locks_update_dev_gids(ib_port->nis_dev);

			if (ib_port->loop_listener) {
				/* Update loopback listener */
				struct nvmeib_rdma_listen_lb_params lb_params;
				lb_params.gid = ib_port->gid.gid;
				lb_params.gid_index = ib_port->gid.gid_index;
				memcpy(lb_params.roce_mac, ib_port->gid.roce_mac, ETH_ALEN);
				lb_params.pkey_index = 0;
				lb_params.link_layer = ib_port->gid.link_layer;
				nvmeib_rdma_update_lb_listen(ib_port->loop_listener, &lb_params);
			}
		}
		else if(!port_was_enabled && (ib_port->port_active && ib_port->port_used)) {
			nvmeibs_disk_locks_update_dev_gids(ib_port->nis_dev);
		}
	}

	if (port_was_enabled && !nvmeibs_ib_port_enabled(ib_port)) {
		_NT(trace_6_ib_port_refresh_port, "Got port disabled on device @IB_DEV_NAME port @PORT (state @STATE). Releasing clients",
			P2IB(ib_port)->name, ib_port->port, port_attr.state);
		nvmeibs_release_port_clients(ib_port, NVMEIBS_LOGOUT_REASON_PORT_DISABLE);
	}

	if (port_was_enabled != nvmeibs_ib_port_enabled(ib_port) || gid_change || link_layer_change) {
		_NT(trace_7_ib_port_refresh_port, "Update all clients of device @IB_DEV_NAME port @PORT hw-gid=@GID_IPV6 (a.state @STATE) change: "
		   "prev: used=@USED, enb=@ENB, gid=@GID_IPV6, "
		   "curr: used=@USED, enb=@ENB, gid=@GID_IPV6",
		   P2IB(ib_port)->name, ib_port->port, &ib_port->gid.hw_gid, port_attr.state,
		   port_was_used, port_was_enabled, &curr_gid,
		   ib_port->port_used, nvmeibs_ib_port_enabled(ib_port), &ib_port->gid.gid);
		nvmeibs_update_all_clients_gid_change(ib_port);
		// notify TOMA on ALL active port link state changes
		if (port_was_enabled != nvmeibs_ib_port_enabled(ib_port)) {
			nvmeibs_toma_report_event_port_gid_change(ib_port);
			nvmeibs_toma_report_event_nic_change(nis_dev, nvmeibs_ib_port_enabled(ib_port));
		}
	}

	goto out;

err_query_port:
	port_modify.set_port_cap_mask = 0;
	port_modify.clr_port_cap_mask = IB_PORT_DEVICE_MGMT_SUP;
	ib_modify_port(P2IB(ib_port), ib_port->port, 0, &port_modify);

out:
	nvmeib_ref_put(&nis_dev->n_refresh_port);

	NFOUT;
	return rv;
}

struct refresh_port_work_param {
	struct workqe_struct main_work; /* Work queue element for main wq */
	struct nvmeibs_dev *nis_dev; /* Set by main wq fn */
	struct ib_device *ib_dev;
	u8 port;
	bool gid_change;
	bool port_error;
	struct workqe_struct port_work; /* Work queue element for port wq */
	struct nvmeibs_ib_port *ib_port;
};

static void refresh_port_work(struct workqe_struct *work)
{
	struct refresh_port_work_param *param =
		container_of(work, struct refresh_port_work_param, port_work);

	NFIN;
	refresh_port(param->ib_port, param->gid_change, param->port_error);
	kfree(param);
	NFOUT;
}

/* Runs on the main wq. Finds the port (after locking the nvmeibs_dev_guard)
 * and then puts the work on its wq */
static void refresh_port_main_work(struct workqe_struct *work)
{
	struct refresh_port_work_param *param =
		container_of(work, struct refresh_port_work_param, main_work);
	NFIN;
	if (!(param->nis_dev = nvmeibs_get_nis_dev(param->ib_dev))) {
		_NT(trace_ib_port_refresh_port_main_work, "nis_dev not found for IB device @IB_DEV_NAME. Must be going down", param->ib_dev->name);
		goto free_work;
	}
	param->ib_port = nvmeibs_ib_port_find_ib_port(param->nis_dev, param->port);
	if (!param->ib_port) {
		_NE(error_ib_port_refresh_port_main_work, "Port @PORT of Device @IB_DEV_NAME not found", param->port, N2IB(param->nis_dev)->name);
		goto free_work;

	}

	if (nvmeib_ref_get(&param->nis_dev->n_refresh_port) == 0) {
		_NE(error_1_ib_port_refresh_port_main_work, "Fail to get work");
		goto free_work;
	}

	/* reuse work-item to schedule refresh_port_work */
	if (nvmeibs_ib_port_add_work(param->ib_port, &param->port_work) < 0) {
		_NE(error_2_ib_port_refresh_port_main_work, "Failed to add work");
		nvmeib_ref_put(&param->nis_dev->n_refresh_port);
		goto free_work;
	}
	goto out;

free_work:
	kfree(param);

out:
	// nvmeib_set_roce_lossy_mode_on();
	NFOUT;
}



/* Schedules a port for update. This fn is running in async event interrupt context
 * so we must first schedule work on the main wq in order to find the port
 * and then onto its port wq */
static void refresh_port_int_ctx(struct ib_device *ib_dev, u8 port, bool gid_change, bool port_error)
{
	struct refresh_port_work_param *work;

	NFIN;
	if (!(work = kzalloc(sizeof(*work), GFP_ATOMIC))) {
		_NE(error_ib_port_refresh_port_int_ctx, "Memory allocation error");
		goto out;
	}

	WQ_INIT_WORK(&work->main_work, refresh_port_main_work);
	work->ib_dev = ib_dev;
	work->port = port;
	work->gid_change = gid_change;
	work->port_error = port_error;
	WQ_INIT_WORK(&work->port_work, refresh_port_work);

	if (nvmeibs_add_work(&work->main_work) < 0) {
		_NT(trace_ib_port_refresh_port_int_ctx, "Failed to add work");
		kfree(work);
	}

out:
	NFOUT;
}



struct remove_ib_device_work_param {
	struct workqe_struct main_work; /* Work queue element for main wq */
	struct ib_device *ib_dev;
};

static void remove_ib_device_work(struct workqe_struct *work)
{
	struct remove_ib_device_work_param *param =
		container_of(work, struct remove_ib_device_work_param, main_work);

	NFIN;
	nvmeibs_remove_ib_device(param->ib_dev);
	kfree(param);
	NFOUT;
}


static void exec_nvmeibs_remove_ib_device(struct ib_device *ib_dev)
{
	struct remove_ib_device_work_param *work;

	NFIN;
	if (!(work = kzalloc(sizeof(*work), GFP_ATOMIC))) {
		_NE(error_ib_port_exec_nvmeibs_remove_ib_device, "Memory allocation error");
		goto out;
	}
	work->ib_dev = ib_dev;
	WQ_INIT_WORK(&work->main_work, remove_ib_device_work);

	if (nvmeibs_add_work(&work->main_work) < 0) {
		_NT(trace_ib_port_exec_nvmeibs_remove_ib_device, "Failed to add work");
		kfree(work);
	}
out:
	NFOUT;
}

static bool verify_client_cred(struct nvmeibs_ib_port *ib_port, u64 cid)
{
	return true;
}

static int validate_login_request(struct nvmeibs_ib_port *ib_port,
	struct nvmeibc_login_request *req, struct nvmeibs_client **cl,
	struct nvmeibs_ib_port **cl_ib_port, struct nvmeibs_login_reject *rej,
	bool *port_queue_switched, const union ib_gid *dgid)
{
	u32 client_msg_size;
	u8 opcode = nvmeib_wire_op_cid_get_req_opcode(&req->op_cid);
	u64 cid = nvmeib_wire_op_cid_get_cid(&req->op_cid);
	union ib_gid sgid;
	u8 msg_hdr_out_flags;
	int rv = 0;

	NFIN;
	if (nvmeibs_is_exit_called()) {
		rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_MODULE_EXIT);
		_NT(trace_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because the module is exiting");
		rv = -EPERM;
		goto out;
	}
	if (!nvmeibs_toma_is_connected()) {
		rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_TOMA_NOT_CONNECTED);
			_NT(trace_1_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because "
				"toma is not connected");
			rv = -ENXIO;
			goto out;
	}
	if (!nvmeibs_ib_port_enabled(ib_port)) {
		rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_PORT_DISABLED);
		_NT(trace_2_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because port @IB_DEV_NAME:@PORT is disabled",
			P2IB(ib_port)->name, ib_port->port);
		rv = -ENXIO;
		goto out;
	}

	if (NVMEIB_UPDATE_NW_PATHS &&
		opcode != NVMEIBC_NORDDA_CHANNEL &&
		opcode != NVMEIBC_IO_CHANNEL) {
		sgid.global.subnet_prefix = ib_port->gid.gid.global.subnet_prefix;
		sgid.global.interface_id = ib_port->gid.gid.global.interface_id;
	}
	else {
		sgid.global.subnet_prefix = ib_port->gid.hw_gid.global.subnet_prefix;
		sgid.global.interface_id = ib_port->gid.hw_gid.global.interface_id;
	}

	if (sgid.global.subnet_prefix != dgid->global.subnet_prefix ||
		sgid.global.interface_id != dgid->global.interface_id) {
		if (opcode == NVMEIBC_ADMIN_CHANNEL) {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_ADMIN_INVALID_GID);
			_NT(trace_3_ib_port_validate_login_request, "Rejected NVMEIB_ADMIN_LOGIN_REQ because "
				"GID is invalid: req @DGID vs. mine @SGID", dgid, &sgid);
		}
		else if (opcode == NVMEIBC_LOCK_CHANNEL) {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_LOCK_INVALID_GID);
			_NT(trace_4_ib_port_validate_login_request, "Rejected NVMEIBS_LOGIN_REJ_LOCK_INVALID_GID because "
				"GID is invalid: req @DGID vs. mine @SGID", dgid, &sgid);
		}
		else if (opcode == NVMEIBC_SECONDARY_LOCK_CH) {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_LOCK_2ND_INVALID_GID);
			_NT(trace_5_ib_port_validate_login_request, "Rejected NVMEIBS_LOGIN_REJ_LOCK_2ND_INVALID_GID because "
				"GID is invalid: req @DGID vs. mine @SGID", dgid, &sgid);
		}
		else if (opcode == NVMEIBC_IO_CHANNEL) {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_RDDA_INVALID_GID);
			_NT(trace_6_ib_port_validate_login_request, "Rejected NVMEIBS_LOGIN_REJ_RDDA_INVALID_GID because "
				"GID is invalid: req @DGID vs. mine @SGID", dgid, &sgid);
		}
		else {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_NRDDA_INVALID_GID);
			_NT(trace_7_ib_port_validate_login_request, "Rejected NVMEIBS_LOGIN_REJ_NRDDA_INVALID_GID because "
				"GID is invalid: req @DGID vs. mine @SGID", dgid, &sgid);
		}
		rv = -ENOENT;
		goto out;
	}

	if (opcode == NVMEIBC_ADMIN_CHANNEL) {
		_ND(trace_8_ib_port_validate_login_request, "---- @SUBNET_PREFIX_LLONG, @D_SUBNET_ID, @INTERFACE_ID_LLONG, @D_INTERFACE_ID",
			ib_port->gid.gid.global.subnet_prefix, dgid->global.subnet_prefix,
			ib_port->gid.gid.global.interface_id, dgid->global.interface_id);
		if (cid && nvmeibs_find_client_(cid, NULL)) {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_CLIENT_IS_ALREADY_CONNECTED);
			_NT(trace_9_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because "
			   "cid @CID_LLONG already exist", cid);
			rv = -ENOENT;
			goto out;
		}
	}
	else {
		if (!(*cl = nvmeibs_find_client(cid, cl_ib_port))) {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_NO_SUCH_CLIENT);
			_NT(trace_10_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ (opcode @OPCODE) because "
			   "client ID @CID_LLONG  was not found", opcode, cid);
			rv = -ENOENT;
			goto out;
		}
		if (!*port_queue_switched &&
			*cl_ib_port && *cl_ib_port != ib_port) {
			*port_queue_switched = true;
			goto out;
		}
		if (opcode == NVMEIBC_LOCK_CHANNEL) {
			_ND(trace_11_ib_port_validate_login_request, "---- @SUBNET_PREFIX_LLONG, @D_SUBNET_ID, @INTERFACE_ID_LLONG, @D_INTERFACE_ID",
				ib_port->gid.gid.global.subnet_prefix, dgid->global.subnet_prefix,
				ib_port->gid.gid.global.interface_id, dgid->global.interface_id);
			if (!nvmeibs_client_check_lock(*cl, ib_port)) {
				rej->reason = __constant_cpu_to_be32(
					NVMEIBS_LOGIN_REJ_INVALID_LOCK_PORT);
				_NT(trace_12_ib_port_validate_login_request, "Rejected NVMEIBC_LOCK_CHANNEL because "
					"port does not belong to lock device");
				NFOUT;
				return -EXDEV;
			}
		}
		else if (opcode == NVMEIBC_IO_CHANNEL) {
			/* no specific checks */
		}
		else if (opcode == NVMEIBC_NORDDA_CHANNEL) {
			/* no specific checks */
		}
		else if (opcode == NVMEIBC_SECONDARY_LOCK_CH) {
			if (!(*cl)->lock_validated) {
				rej->reason = __constant_cpu_to_be32(
					NVMEIBS_LOGIN_REJ_NO_PRIMARY_LOCK_CHANNEL);
				_NT(trace_13_ib_port_validate_login_request, "Rejected NVMEIBC_SECONDARY_LOCK_NET because primary lock "
				"channel is not connected");
				NFOUT;
				return -ENOENT;
			}
			else if ((*cl)->lock_net->params.port->nis_dev != ib_port->nis_dev) {
				rej->reason = __constant_cpu_to_be32(
					NVMEIBS_LOGIN_REJ_INVALID_LOCK_PORT);
				_NT(trace_14_ib_port_validate_login_request, "Rejected NVMEIBC_LOCK_CHANNEL because "
					"port does not belong to lock device");
				NFOUT;
				return -EXDEV;
			} else {
				if (atomic_read(&(*cl)->n_2nd_lock_ch) >= NVMEIB_N_2ND_LOCK_CHS) {
					rej->reason = __constant_cpu_to_be32(
						NVMEIBC_SECONDARY_LOCK_CH);
					_NT(trace_15_ib_port_validate_login_request, "Rejected NVMEIBC_LOCK_CHANNEL because "
						"the maximum is already connected");
					NFOUT;
					return -ENOSPC;
				}
			}
		}
		else {
			rej->reason = __constant_cpu_to_be32(
				NVMEIBS_LOGIN_REJ_INVALID_CMD);
			_NT(trace_16_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because opcodeis invalid @OPCODE",
				opcode);
			rv = -EPERM;
			goto out;
		}
	}
	if (cid && !verify_client_cred(ib_port, cid)) {
		rej->reason =
			__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_REQ_IT_IU_INSUFF_CRED);
		_NT(trace_17_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because client "
			"has insufficient credentials");
		rv = -EINVAL;
		goto out;
	}
	/* check that client messages are not too big */
	if (opcode == NVMEIBC_ADMIN_CHANNEL) {
		if (!nvmeibc_login_req_get_local(req)) {
			nvmeibc_login_req_get_ach(req, NULL, &client_msg_size, NULL);
			if (client_msg_size > ib_port->port_attrib.max_req_size) {
				rej->reason =
					__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_CLIENT_MSG_TOO_SMALL);
				_NT(trace_18_ib_port_validate_login_request, "Rejected NVMEIB_LOGIN_REQ because client "
					"message size for server is too small - needed @CLIENT_MSG_SIZE, has @MAX_REQ_SIZE",
					client_msg_size, ib_port->port_attrib.max_req_size);
				rv = -E2BIG;
				goto out;
			}
		}
	}
	/* check that client does not require putting an outgoing message header (not supported by kernel target) */
	nvmeibc_login_req_get_msg_hdr(req, &msg_hdr_out_flags, NULL, NULL, NULL);
	if (msg_hdr_out_flags == NVMEIBC_LOGIN_MSG_HDR_REQUIRED) {
		_NT(trace_ib_port_validate_login_request_msg_hdr_req,
		    "Rejected NVMEIB_LOGIN_REQ because client requires outgoing message header - not supported");
		rej->reason =
			__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_MSG_HDR_NOT_SUPPORTED);
		rv = -ENOTSUPP;
		goto out;
	}

out:
	NFOUT;
	return rv;
}

struct nvmeibs_ib_port *nvmeibs_ib_port_find_ib_port(
	struct nvmeibs_dev *nis_dev, u8 port)
{
	struct nvmeibs_ib_port *ib_port = NULL, *tmp;
	bool found = false;

	NFIN;
	/* Lock nvmeibs_dev_guard */
	nvmeibs_get_devices(NULL);
	list_for_each_entry_safe(ib_port, tmp, &nis_dev->port_list, port_list_n) {
		if (ib_port->port == port) {
			found = true;
			goto out;
		}
	}
	list_for_each_entry_safe(ib_port, tmp, &nis_dev->unused_port_list, port_list_n) {
		if (ib_port->port == port) {
			found = true;
			goto out;
		}
	}

out:
	/* Unlock nvmeibs_dev_guard */
	nvmeibs_put_devices();
	NFOUT;
	return found ? ib_port : NULL;
}

struct nvmeibs_ib_port *nvmeibs_ib_port_add(struct nvmeibs_dev *nis_dev, u8 port)
{
	struct nvmeibs_ib_port *ib_port = NULL;
	struct ib_port_attr port_attr = {0};
	struct list_head new_gid_list = LIST_HEAD_INIT(new_gid_list);
	char kmem_cache_name[32];
	int rv = 0;

	NFIN;
	if (!(ib_port = kzalloc(sizeof *ib_port, GFP_KERNEL))) {
		_NE(error_ib_port_nvmeibs_ib_port_add, "Fail to allocate new port");
		goto out;
	}

	if (!(ib_port->wq = wq_create_verbose(proc_name_format("S", "WQ", "port")))) {
		_NE(error_1_ib_port_nvmeibs_ib_port_add, "Fail to allocate main port work queue");
		goto err;
	}

	snprintf(kmem_cache_name, sizeof(kmem_cache_name), "nrch_cmd_req-%.10s-%d",
		 N2IB(nis_dev)->name, port);
	if (!(ib_port->nrch_cmd_req_cache = kmem_cache_create(kmem_cache_name,
			sizeof(*((struct nvmeibs_nr_cmd *)0)->cmd_req), 0, SLAB_RED_ZONE, NULL)))
	{
		_NE(error_2_ib_port_nvmeibs_ib_port_add, "Failed to allocate nrch_cmd_req_cache");
		goto err;
	}

	ib_port->nis_dev = nis_dev;
	ib_port->port = port;
	ib_port->port_attrib.max_rdma_size = NVMEIBS_DEFAULT_MAX_RDMA_SIZE;
	ib_port->port_attrib.max_req_size = nvmeibs_get_max_req_size();
	ib_port->port_attrib.sq_size = nvmeibs_get_shared_recv_queue_size();
	ib_port->port_attrib.max_requests = NVMEIB_MAX_NORDDA_IO_REQ;
	ib_port->hw_type = nvmeib_get_device_type(P2IB(ib_port));
	nvmeib_ref_init(&ib_port->n_port_conns);
	INIT_LIST_HEAD(&ib_port->gid_list);

	if ((rv = ib_query_port(P2IB(ib_port), ib_port->port, &port_attr))) {
		_NT(trace_ib_port_nvmeibs_ib_port_add, "ib_query_port() failed.");
		goto err;
	}

	ib_port->sm_lid = port_attr.sm_lid;
	ib_port->lid = port_attr.lid;
	ib_port->layer = rdma_port_get_link_layer(P2IB(ib_port), ib_port->port);
	ib_port->transport = rdma_node_get_transport(P2IB(ib_port)->node_type);

	if (ib_port->layer == IB_LINK_LAYER_INFINIBAND)
		ib_port->transport_priority = nvmeibs_ib_port_prio;
	else if (ib_port->layer == IB_LINK_LAYER_ETHERNET) {
		if (ib_port->transport == RDMA_TRANSPORT_IB) {
			ib_port->transport_priority = nvmeibs_roce_port_prio;
		} else if (ib_port->transport == RDMA_TRANSPORT_IWARP) {
			ib_port->transport_priority = nvmeibs_tcp_port_prio;
		}
	}

	ib_port->port_active = port_attr.state == IB_PORT_ACTIVE;

	if ((rv = ib_query_pkey(P2IB(ib_port), ib_port->port, 0,
		&ib_port->pkey))) {
		_NT(trace_1_ib_port_nvmeibs_ib_port_add, "ib_query_pkey() failed.");
		goto err;
	}

	if ((rv = nvmeib_rdma_get_port_hw_gid(P2IB(ib_port), ib_port->port, &ib_port->hw_gid))) {
		_NT(trace_2_ib_port_nvmeibs_ib_port_add, "nvmeib_rdma_get_port_hw_gid() failed (@RV).\n", rv);
		goto err;
	}

	ib_port->port_used = true;
	if (!nvmeib_use_dev(nvmeibs_get_used_dev_list(), P2IB(ib_port), (u8)port, &new_gid_list)) {
		_NT(trace_3_ib_port_nvmeibs_ib_port_add, "Device @DEVICE_NAME port @PORT filtered out", P2IB(ib_port)->name, port);
		ib_port->port_used = false;
	}
	replace_gid_list(ib_port, &new_gid_list);
	if (!ib_port->gid.valid) {
		_NT(trace_4_ib_port_nvmeibs_ib_port_add, "Device @DEVICE_NAME port @PORT has not valid gid", P2IB(ib_port)->name, port);
		ib_port->port_used = false;
	}
	else if (nvmeibs_selected_layer != IB_LINK_LAYER_UNSPECIFIED &&
		ib_port->gid.link_layer != nvmeibs_selected_layer) {
		/* Link layer has already been chosen and this port doesn't match */
		_NT(trace_5_ib_port_nvmeibs_ib_port_add, "Device @DEVICE_NAME port @PORT filtered out due to link layer",
		    P2IB(ib_port)->name, port);
		ib_port->port_used = false;
	}
	if (ib_port->port_used)
		create_nic_gid_csv(ib_port);
	goto out;

err:
	if (ib_port) {
		if (ib_port->wq)
			wq_destroy(ib_port->wq);
		if (ib_port->nrch_cmd_req_cache)
			kmem_cache_destroy(ib_port->nrch_cmd_req_cache);
	}
	kfree(ib_port);
	ib_port = NULL;

out:
	NFOUT;
	return ib_port;
}

void nvmeibs_ib_port_clear(struct nvmeibs_ib_port *ib_port)
{
	struct ib_port_modify port_modify;

	NFIN;
	port_modify.set_port_cap_mask = 0;
	port_modify.clr_port_cap_mask = IB_PORT_DEVICE_MGMT_SUP;
	ib_modify_port(P2IB(ib_port), ib_port->port, 0, &port_modify);
	if (ib_port->mad_agent) {
		ib_unregister_mad_agent(ib_port->mad_agent);
		ib_port->mad_agent = NULL;
	}
	//cancel_work_sync(ib_port->wq, &ib_port->work);
	//nvmeib_delq(ib_port->wq, &ib_port->work)
	_NT(trace_ib_port_nvmeibs_ib_port_clear, "Waiting for all clients on port to disconnect");
	nvmeibs_ib_port_wait_no_conns(ib_port);
	wq_drain(ib_port->wq);
	_NT(trace_1_ib_port_nvmeibs_ib_port_clear, "All clients are disconnected");
	NFOUT;
}

/**
 * nvmeibs_ib_port_event_handler() - Asynchronous IB event
 * callback function.
 *
 * Callback function called by the InfiniBand core when an asynchronous IB
 * event occurs. This callback may occur in interrupt context. See also
 * section 11.5.2, Set Asynchronous Event Handler in the InfiniBand
 * Architecture Specification.
 */
void nvmeibs_ib_port_event_handler(struct nvmeib_rdma_event_handler *event_handler,
			   struct ib_event *event)
{
	struct ib_device *ib_dev = event_handler->ib_dev;
	static const char *event_desc[] = {
		"CQ Error",
		"QP Fatal",
		"QP Request Error",
		"QP Access Error",
		"Comm Established",
		"SQ Drained",
		"Path Migration",
		"Path Migration Error",
		"Device Fatal",
		"Port Active",
		"Port Error",
		"LID Change",
		"P-Key Change",
		"SM Change",
		"SRQ Error",
		"SRQ Limit Reached",
		"Last WQE Reached",
		"Client Reregister",
		"GID Change"
	};

	NFIN;
	if (event->event > ARRAY_SIZE(event_desc)) {
		NFOUT;
		_NT(trace_ib_port_nvmeibs_ib_port_event_handler, "Filtering out ASYNC event= @EVENT", event->event);
		return;
	}

	_NT(trace_1_ib_port_nvmeibs_ib_port_event_handler, "Got ASYNC event @IB_EVENT_STR on IB Device @IB_DEV_NAME", event_desc[event->event], ib_dev->name);

	switch (event->event) {
	case IB_EVENT_LID_CHANGE:
	case IB_EVENT_PKEY_CHANGE:
	case IB_EVENT_SM_CHANGE:
	case IB_EVENT_CLIENT_REREGISTER:
	case IB_EVENT_PORT_ERR:
	case IB_EVENT_PORT_ACTIVE:
	case IB_EVENT_GID_CHANGE:
		/* Refresh port data asynchronously. */
		_NT(trace_2_ib_port_nvmeibs_ib_port_event_handler, "Updating port @PORT_NUM of device @IB_DEV_NAME", event->element.port_num, ib_dev->name);
		if (event->element.port_num <= ib_dev->phys_port_cnt) {
			/*
			 * query for GID change in all above cases but PORT_ERR
			 */
			bool gid_error = (event->event == IB_EVENT_PORT_ERR);
			bool gid_change = !gid_error;
			_NT(trace_3_ib_port_nvmeibs_ib_port_event_handler, "Updating port");
			refresh_port_int_ctx(ib_dev, event->element.port_num,
					     gid_change, gid_error);
		}
		break;
	case IB_EVENT_DEVICE_FATAL:
		_NE(error_ib_port_nvmeibs_ib_port_event_handler, "Got @IB_DEV_NAME Device Fatal - Removing", ib_dev->name);
		if (nvmeibs_main_start_fatal()) {
			exec_nvmeibs_remove_ib_device(ib_dev);
			nvmeibs_main_set_ok();
		}
		break;
	default:
		_NE(error_1_ib_port_nvmeibs_ib_port_event_handler, "received unrecognized IB event @EVENT", event->event);
		break;
	}
	NFOUT;
}

/**
 * nvmeibs_ib_port_new_connection_work() - Process the event
 * IB_CM_REQ_RECEIVED.
 *
 * Ownership of the cm_id is transferred to the client session
 * if this functions returns zero. Otherwise the caller remains
 * the owner of cm_id.
 */
static void new_connection_work(struct workqe_struct *work)
{
	struct port_work *pw = container_of(work, struct port_work, work);
	struct add_client_workq *w =
		container_of(pw, struct add_client_workq, port);
	struct nvmeibs_ib_port *ib_port = w->port.port;
	struct nvmeib_rdma_cm *cm_id = w->cm_id;
	struct nvmeibc_login_request *req = &w->req;
	struct nvmeibs_login_reject *rej;
	struct nvmeibs_client *cl = NULL;
	union ib_gid sgid, dgid;
	u8 opcode = nvmeib_wire_op_cid_get_req_opcode(&req->op_cid);
	u64 cid = nvmeib_wire_op_cid_get_cid(&req->op_cid);
	struct nvmeibs_ib_port *cl_ib_port = NULL;
	int rv = 0;
	unsigned long passed = jiffies - w->sent_time;
	u64 start;
	bool port_queue_switched;
	bool free_work = true;

	NFIN;
	start = jiffies;
	_NT(trace_ib_port_new_connection_work, "new_connection_work called with cid=@CID_LLONG opcode=@OPCODE local=@LOCAL_INT version=@VERSION passed=@PASSED",
	   cid, opcode, nvmeibc_login_req_get_local(req), nvmeibc_login_req_get_version(req), passed);
	if (!nvmeibc_login_req_get_dgid(req, &dgid.global.subnet_prefix, &dgid.global.interface_id)) {
		/* Login request does not contain DGID - get from cm_id
		 * NOTE: That the cm_id denotes sgid and dgid for the return path so we need the return sgid as our dgid */
		nvmeib_rdma_read_gids(cm_id, &dgid, NULL);
		_NT(trace_ib_port_new_connection_work_read_dgid,
		    "read dgid @DGID from cm_id @CM_ID", &dgid, cm_id);
	}
	if (opcode == NVMEIBC_ADMIN_CHANNEL) {
		_NT(trace_1_ib_port_new_connection_work, "Received ADMIN-CH login from client to port @PORT on gid @GID_IPV6 (@DGID)"
		   " time passed @PASSED", ib_port->port, &ib_port->gid.gid,
			&dgid, passed);
	}
	else if (opcode == NVMEIBC_LOCK_CHANNEL) {
		_NT(trace_2_ib_port_new_connection_work, "Received LOCK-CH login from client to port @PORT on gid @GID_IPV6 (@DGID)",
			ib_port->port, &ib_port->gid.gid, &dgid);
	}
	else if (opcode == NVMEIBC_IO_CHANNEL ||
			 opcode == NVMEIBC_NORDDA_CHANNEL) {
		u16 qp_num;
		nvmeibc_login_req_get_ioch(req,
					&sgid.global.subnet_prefix, &sgid.global.interface_id,
					&dgid.global.subnet_prefix, &dgid.global.interface_id,
					&qp_num, NULL);
		_NT(trace_3_ib_port_new_connection_work, "Received @TYPE_STR-CH login from client path: @SGID->@DGID, "
		   "cid @CID_LLONG, on port @PORT (@GID_IPV6) and qpn @QPN",
			(opcode == NVMEIBC_IO_CHANNEL) ? "IO" : "NORDDA",
			&sgid, &dgid, cid, ib_port->port, &ib_port->gid.gid,
			(int)qp_num);
	}
	else if (opcode == NVMEIBC_SECONDARY_LOCK_CH) {
		_NT(trace_4_ib_port_new_connection_work, "Received 2ND-LOCK-NET login from client to port @PORT on gid @GID_IPV6",
		   ib_port->port, &ib_port->gid.gid);
	}
	else
		_NE(error_ib_port_new_connection_work, "Received unsupported command @OPCODE", (int)opcode);

	if (!(rej = kzalloc(sizeof *rej, GFP_KERNEL))) {
		_NE(error_1_ib_port_new_connection_work, "rejected NVMEIB_LOGIN_REQ because no "
			"memory to start the process.");
		rv = -ENOMEM;
		goto free_cm;
	}
	rej->reason = __constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_INVALID_CMD);

	port_queue_switched = w->port_queue_switched;
	if ((rv = validate_login_request(ib_port, req, &cl, &cl_ib_port, rej,
				&port_queue_switched, &dgid))) {
		goto reject;
	}
	else if (!w->port_queue_switched && port_queue_switched) {
		w->port_queue_switched = true;
		if ((rv = nvmeibs_ib_port_add_work(cl_ib_port, work)) < 0) {
			_NT(trace_5_ib_port_new_connection_work, "Failed to queue new connection work: @RV", rv);
			goto reject;
		}
		else {
			free_work = false;
			goto out;
		}
	}

	_ND(trace_6_ib_port_new_connection_work, "Validated");
	if (!cid) {
		nvmeib_wire_op_cid_set_cid(&req->op_cid, nvmeibs_get_client_uid());
	}

	WARN_ON(ib_port == NULL);
	if (opcode == NVMEIBC_ADMIN_CHANNEL) {
		rv = nvmeibs_client_allocate(ib_port, cm_id, &cl, req, rej);
		if (likely(!rv)) {
			rv = nvmeibs_client_connect_admin_channel(
				ib_port, cm_id, cl, req, rej);
		}
	} else {
		/* here we are safe that the client exists since we are running
		   on the client port work_queue so if there is a client it will
		   not disappear underneath
		*/
		if (likely(cl)) {
			if (opcode == NVMEIBC_LOCK_CHANNEL)
				rv = nvmeibs_client_connect_lock_channel(
					ib_port, cm_id, cl, req, rej);
			else if (opcode == NVMEIBC_IO_CHANNEL)
				rv = nvmeibs_client_connect_io_channel(
					ib_port, cm_id, cl, req, rej);
			else if (opcode == NVMEIBC_NORDDA_CHANNEL)
				rv = nvmeibs_nordda_connect_channel(
					ib_port, cm_id, cl, req, rej);
			else if (opcode == NVMEIBC_SECONDARY_LOCK_CH) {
				rv = nvmeibs_client_connect_2nd_lock_ch(
					ib_port, cm_id, cl, req, rej);
			}
			else {
				_NE(error_2_ib_port_new_connection_work, "Not supported opcode=@OPCODE", (int)opcode);
				rej->reason = __constant_cpu_to_be32(
					NVMEIBS_LOGIN_REJ_INVALID_CMD);
				rv = -1;
			}
		}
		else {
			_NE(error_3_ib_port_new_connection_work, "OOPS, No such client although login validated");
			rv = -1;
			rej->reason =
				__constant_cpu_to_be32(NVMEIBS_LOGIN_REJ_NO_SUCH_CLIENT);

		}
	}

	if (!cl || rv)
		goto reject;

	_NT(trace_7_ib_port_new_connection_work, "Finish to handle new connection from client @CLIENT_UUID (@CL) ", &cl->client_uuid, cl);
	goto out;

reject:
	if (cl) {
		_NT(trace_8a_ib_port_new_connection_work,
			"Reject new connection from client @CLIENT_UUID rv=@RV",
			&cl->client_uuid, rv);
	} else {
		_NT(trace_8b_ib_port_new_connection_work,
			"Reject new connection from UNKNOWN client rv=@RV", rv);
	}
	if (cm_id)
		nvmeibs_send_login_reject(cm_id, rej, req);

free_cm:
	/* we must remember to close the cm_id */
	_ND(trace_9_ib_port_new_connection_work, "After free_cm");
	if (cm_id)
		nvmeib_rdma_destroy_cm(cm_id);

out:
	if (ib_port && free_work) {
		int n;
		if ((n = atomic_read(&ib_port->outstanding)) < 1) {
			_NE(new_connection_work_e1, "OOPS, port @STR:@INT, outstanding new-conn-works @INT",
				P2IB(ib_port)->name, ib_port->port, n);
		}
		else {
			atomic_dec(&ib_port->outstanding);
		}
	}
	kfree(rej);
	if (free_work)
		kfree(w);
	_ND(trace_10_ib_port_new_connection_work, "PORT_ANA new connection took @DIFF_JIFFIES", jiffies - start);
	NFOUT;
}

int nvmeibs_ib_port_new_connection(struct nvmeibs_ib_port *ib_port,
	struct nvmeib_rdma_cm *cm_id, struct nvmeib_rdma_conn_params *param)
{
	struct add_client_workq *w;
	int rv = 0;

	NFIN;
	WARN_ON(ib_port == NULL);
	if (!(w = kzalloc(sizeof(*w), GFP_KERNEL))) {
		_NE(error_ib_port_nvmeibs_ib_port_new_connection, "OOM: cannot allocate new connection work");
		rv = -ENOMEM;
		goto out;
	}
	WQ_INIT_WORK(&w->port.work, new_connection_work);
	w->port.port = ib_port;
	w->cm_id = cm_id;
	w->sent_time = jiffies;
	w->port_queue_switched = false;
	memcpy(&w->req, param->private_data, sizeof(w->req));
	_ND(trace_ib_port_nvmeibs_ib_port_new_connection, "w=@CLIENT, cm_id=@CM_ID w->req->local=@LOCAL_INT, ib_port=@IB_PORT",
		w, cm_id, nvmeibc_login_req_get_local(&w->req), w->port.port);
	if ((rv = nvmeibs_ib_port_add_work(ib_port, &w->port.work)) < 0) {
		_NT(trace_1_ib_port_nvmeibs_ib_port_new_connection, "Failed to queue new connection work: @RV", rv);
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

static void remove_single_client(struct nvmeibs_client *cl)
{
	NFIN;
	_NT(trace_ib_port_remove_single_client,
		"Remaining number of connections on port is @ATOMIC_READ",
		nvmeib_ref_read(&cl->ib_port->n_port_conns));
	if (cl->release_done)
		kfree(cl->release_done);
	_NT(trace_1_ib_port_remove_single_client, "cl @CL_NAME release - Done", cl->name);

	/* ensure cl is not reachable from dying list right before freeing the wqs */
	nvmeibs_cdb_dying_del(cl);

	wq_destroy(cl->wq);
	wq_destroy(cl->remove_wq);
	kfree(cl);

	NFOUT;
}

static void free_client_work(struct workqe_struct *work)
{
	struct port_work *pw = container_of(work, struct port_work, work);
	struct cid_port_work *w =
		container_of(pw, struct cid_port_work, port);
	struct nvmeibs_client *cl;
	u64 start;
	NFIN;

	start = jiffies;
	_NT(trace_ib_port_free_client_work, "Lookup cid @CID_LLONG and remove it from srv's hash", w->cid);
	cl = nvmeibs_cdb_del_by_cid(w->cid, true);
	if (cl) {
		_NT(trace_1_ib_port_free_client_work, "cl @CL_NAME release - Start", cl->name);
		nvmeibs_client_release(cl, remove_single_client, w->reason);
	} else
		_NT(trace_2_ib_port_free_client_work, "Fail to find cid @CID_LLONG", w->cid);

	kfree(w);
	_ND(trace_3_ib_port_free_client_work, "PORT_ANA free client work took @DIFF_JIFFIES", jiffies - start);

	NFOUT;
}

int nvmeibs_ib_port_free_client(struct nvmeibs_ib_port *ib_port, u64 cid, enum nvmeibs_logout_reason reason)
{
	struct cid_port_work *w;
	int rv = 0;

	NFIN;
	if (!(w = kzalloc(sizeof(*w), GFP_ATOMIC))) {
		_NE(error_ib_port_nvmeibs_ib_port_free_client, "OOM: cannot allocate free client work");
		rv = -ENOMEM;
		goto out;
	}
	WQ_INIT_WORK(&w->port.work, free_client_work);
	w->port.port = ib_port;
	w->cid = cid;
	w->reason = reason;
	if ((rv = nvmeibs_ib_port_add_work(ib_port, &w->port.work)) < 0) {
		_NT(trace_ib_port_nvmeibs_ib_port_free_client, "Failed to queue new connection work: @RV", rv);
		kfree(w);
	}

out:
	NFOUT;
	return rv;
}

/* wait for all connection to be removed */
void nvmeibs_ib_port_wait_no_conns(struct nvmeibs_ib_port *ib_port)
{
	NFIN;

	nvmeib_ref_release_start(&ib_port->n_port_conns);
	nvmeib_ref_release_wait(&ib_port->n_port_conns);

	NFOUT;
}

void nvmeibs_ib_port_free(struct nvmeibs_ib_port *ib_port)
{
	NFIN;
	if (ib_port->loop_listener)
		nvmeib_rdma_destroy_cm(ib_port->loop_listener);
	free_gid_list(ib_port);

	if (ib_port->gids_csv_proc_ent) {
		nvmeib_public_proc_remove(ib_port->gids_csv_proc_ent);
		ib_port->gids_csv_proc_ent = NULL;
	}
	kmem_cache_destroy(ib_port->nrch_cmd_req_cache);
	wq_destroy(ib_port->wq);
	kfree(ib_port);
	NFOUT;
}

int nvmeibs_ib_port_add_work(struct nvmeibs_ib_port *ib_port,
	struct workqe_struct *work)
{
	return wq_add_work(ib_port->wq, work) ? 0 : -1;
}

void nvmeibs_ib_port_drain_q(struct nvmeibs_ib_port *ib_port)
{
	wq_drain(ib_port->wq);
}
