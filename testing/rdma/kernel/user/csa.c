/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "csa.h"
#include "trace.h"

#define MAX_PORTS (8)
#define DEFAULT_SA_TIMEOUT_MS (1000)

int init_sa(struct client_sa_info *info)
{
	uint32_t i = 0;
	uint64_t port_guid = (uint64_t)-1;
	osm_bind_handle_t h = OSM_BIND_INVALID_HANDLE;
	ib_api_status_t status;
	ib_port_attr_t attr_array[MAX_PORTS];
	uint32_t num_ports = MAX_PORTS;
	uint32_t ca_name_index = 0;

	complib_init();

	osm_log_construct(&info->log_osm);
	if ((status = osm_log_init_v2(&info->log_osm, TRUE, 0x0001, NULL, 0, TRUE)) != IB_SUCCESS) {
		trace("Failed to init osm_log: %s\n", ib_get_err_str(status));
		goto out;
	}
	osm_log_set_level(&info->log_osm, OSM_LOG_NONE);
	if (info->osm_debug)
		osm_log_set_level(&info->log_osm, OSM_LOG_DEFAULT_LEVEL);

	info->vendor = osm_vendor_new(&info->log_osm, info->sa_timeout_ms ?: DEFAULT_SA_TIMEOUT_MS);
	osm_mad_pool_construct(&info->mad_pool);
	if ((status = osm_mad_pool_init(&info->mad_pool)) != IB_SUCCESS) {
		trace("Failed to init mad pool: %s\n", ib_get_err_str(status));
		goto out;
	}

memset(attr_array, 0, sizeof(attr_array));

	if ((status = osm_vendor_get_all_port_attr(info->vendor, attr_array, &num_ports)) != IB_SUCCESS) {
		trace("Failed to get port attributes: %s\n", ib_get_err_str(status));
		goto out;
	}

	for (i = 0; i < num_ports; ++i) {
		if (i > 1 && cl_ntoh64(attr_array[i].port_guid) != (cl_ntoh64(attr_array[i - 1].port_guid) + 1))
			ca_name_index++;
		if (info->sa_port_num && info->sa_port_num != attr_array[i].port_num)
			continue;
		if (info->sa_hca_name && strcmp(info->sa_hca_name, info->vendor->ca_names[ca_name_index]) != 0)
			continue;
		if (attr_array[i].link_state == IB_LINK_ACTIVE) {
			port_guid = attr_array[i].port_guid;
			break;
		}
	}

	if (port_guid == (uint64_t)-1) {
		trace("Failed to find active port, check port status with \"ibstat\"\n");
		goto out;
	}

	h = osmv_bind_sa(info->vendor, &info->mad_pool, port_guid);

	if (h == OSM_BIND_INVALID_HANDLE) {
		trace("Failed to bind to SA\n");
		goto out;
	}
	
out:
	info->h = h;
	FOUT;
	return h != OSM_BIND_INVALID_HANDLE ? 0 : -1;
}

static void return_mad(struct client_sa_info *info) {
	/*
	 * Return the IB query MAD to the pool as necessary.
	 */
	FIN;
	if (info->result.p_result_madw != NULL) {
		osm_mad_pool_put(&info->mad_pool, info->result.p_result_madw);
		info->result.p_result_madw = NULL;
	}
	FOUT;
}


void free_sa(struct client_sa_info *info)
{
	osm_mad_pool_destroy(&info->mad_pool);
	osm_vendor_delete(&info->vendor);
	info->vendor = NULL;
}

static void dump_path_record(ib_path_rec_t *p_pr)
{
	char sgid_str[GUID_SIZE];
	char dgid_str[GUID_SIZE];
	
	FIN;
	format_gid_raw(p_pr->sgid.raw, sgid_str);
	format_gid_raw(p_pr->dgid.raw, dgid_str);
	trace("path_record dump:\n"
		"\t\tservice_id..............0x%016" PRIx64 "\n"
		"\t\tdgid....................%s\n"
		"\t\tsgid....................%s\n"
		"\t\tdlid....................0x%X\n"
		"\t\tslid....................0x%X\n"
		"\t\thop_flow_raw............0x%X\n"
		"\t\ttclass..................0x%X\n"
		"\t\tnum_path_revers.........0x%X\n"
		"\t\tpkey....................0x%X\n"
		"\t\tqos_class...............0x%X\n"
		"\t\tsl......................0x%X\n"
		"\t\tmtu.....................0x%X\n"
		"\t\trate....................0x%X\n"
		"\t\tpkt_life................0x%X\n"
		"\t\tpreference..............0x%X\n"
		"\t\tresv2...................0x%X\n"
		"\t\tresv3...................0x%X\n"
		"",
		cl_ntoh64(p_pr->service_id),
		dgid_str,
		sgid_str,
		cl_ntoh16(p_pr->dlid),
		cl_ntoh16(p_pr->slid),
		cl_ntoh32(p_pr->hop_flow_raw),
		p_pr->tclass,
		p_pr->num_path,
		cl_ntoh16(p_pr->pkey),
		ib_path_rec_qos_class(p_pr),
		ib_path_rec_sl(p_pr),
		p_pr->mtu,
		p_pr->rate,
		p_pr->pkt_life,
		p_pr->preference,
		*(uint32_t *)&p_pr->resv2, *((uint16_t *)&p_pr->resv2 + 2));
	FOUT;
}

int lookup_path_rec_by_gid(struct client_sa_info *info, const union ibv_gid *src_gid, const union ibv_gid *dst_gid, uint64_t service_id)
{
	osm_bind_handle_t h = info->h;
	osmv_query_req_t req;
	osmv_gid_pair_t gid_pair = {{{0}}};
	ib_api_status_t status;
	ib_path_rec_t *path_record = NULL;
	int i, found = 0;

	FIN;
	memcpy(gid_pair.src_gid.raw, src_gid->raw, sizeof(src_gid->raw));
	memcpy(gid_pair.dest_gid.raw, dst_gid->raw, sizeof(src_gid->raw));

	memset(&req, 0, sizeof(req));

	req.query_type = OSMV_QUERY_PATH_REC_BY_GIDS;
	req.timeout_ms = info->sa_timeout_ms ?: DEFAULT_SA_TIMEOUT_MS;
	req.retry_cnt = 1;
	req.flags = OSM_SA_FLAGS_SYNC;
	req.query_context = NULL;
	req.query_context = info;
	req.pfn_query_cb = info->query_res_cb;
	req.p_query_input = (void *)&gid_pair;
	req.sm_key = 0;

	if ((status = osmv_query_sa(h, &req)) != IB_SUCCESS) {
		trace("ERROR: Query SA failed: %s\n", ib_get_err_str(status));
		goto out;
	}
	status = info->result.status;
	if (info->result.status != IB_SUCCESS) {
		trace("ERROR: Query result returned: %s\n", ib_get_err_str(info->result.status));
		goto out;
	}
	else {
		for (i = 0; i < info->result.result_cnt; ++i) {
			path_record = osmv_get_query_path_rec(info->result.p_result_madw, i);
			dump_path_record(path_record);
			if (!found && ((path_record->service_id == service_id) || path_record->service_id == 0)) {
				info->match_path_record = *path_record;
				convert_path_recs(&info->dst_path_record, &info->match_path_record);
				found = 1;
			}
		}
	}
	
	return_mad(info);
	
out:
	FOUT;
	return (status == IB_SUCCESS) && found ? 0 : -1;
}

void convert_path_recs(struct ibv_sa_path_rec *dst, const ib_path_rec_t *src)
{
	FIN;
	//dst->service_id = src->service_id;
	memcpy(dst->dgid.raw, src->dgid.raw, sizeof(src->dgid.raw));
	memcpy(dst->sgid.raw, src->sgid.raw, sizeof(src->sgid.raw));
	dst->dlid = src->dlid;
	dst->slid = src->slid;
	dst->pkey = src->pkey;
	dst->mtu = src->mtu;
	dst->traffic_class = src->tclass;
	dst->numb_path = src->num_path;
	FOUT;
}

