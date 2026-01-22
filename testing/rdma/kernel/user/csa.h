/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <ctype.h>
#include <string.h>

#include <infiniband/sa.h>
#include <infiniband/mad.h>
#include <infiniband/opensm/osm_log.h>
#include <infiniband/vendor/osm_vendor_api.h>
#include <infiniband/vendor/osm_vendor_sa_api.h>
#include <infiniband/opensm/osm_mad_pool.h>
#include <infiniband/complib/cl_debug.h>
#include <infiniband/complib/cl_nodenamemap.h>

struct client_sa_info {
	osm_bind_handle_t h;
	osm_log_t log_osm;
	osm_mad_pool_t mad_pool;
	osm_vendor_t *vendor;
	/* which HCA to use */
	const char *sa_hca_name;
	/* which port on the HCA to use */
	uint32_t sa_port_num;
	uint32_t sa_timeout_ms;
	int osm_debug;
	void (*query_res_cb)(osmv_query_res_t *res);
	osmv_query_res_t result;
	ib_path_rec_t match_path_record;
	struct ibv_sa_path_rec dst_path_record;
};

int init_sa(struct client_sa_info *info);
void free_sa(struct client_sa_info *info);
int lookup_path_rec_by_gid(struct client_sa_info *info, const union ibv_gid *src_gid, const union ibv_gid *dst_gid, uint64_t service_id);
void convert_path_recs(struct ibv_sa_path_rec *dst, const ib_path_rec_t *src);

