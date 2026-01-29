/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "nvmeibt_debug.h"
#include "nvmeibt_ib_common.h"

#define NVMEIBT_SRV_NICS_CSV_PATH "/proc/nvmeibs/nics.csv"

int nvmeibt_ib_common_read_local_nics(struct local_nics_data *lnd)
{
	FILE *file;
	int rv = -1;
	char *line_ptr = NULL;
	size_t line_ptr_size = 0;
	ssize_t line_len;
	int line = 0;
	__MEASURE_TOOK_INIT();

	NFIN;
	if (!(file = fopen(NVMEIBT_SRV_NICS_CSV_PATH, "r"))) {
		N_Ef(nvmeibt_ib_read_local_nics_e1, "Error (@ERRNO @AUTO_ERRNO) opening local server nics.csv string=@STR", errno,
		    NVMEIBT_SRV_NICS_CSV_PATH);
		goto out;
	}
	__MEASURE_TOOK(N_IMf(4cfghw8, "fopen() Took @LLD ms", NSEC_TO_MSEC(__measure_took_time_took_nsec)));

	lnd->n_nics = 0;
	lnd->n_ib_nics = 0;

	while ((line_len = getline(&line_ptr, &line_ptr_size, file)) != -1) {
		if (line == 0) {
			/* Check header line */
			if (line_len != strlen(NVMEIBS_NICS_CSV_HEADER_EOL) ||
				strncmp(line_ptr, NVMEIBS_NICS_CSV_HEADER_EOL, sizeof(NVMEIBS_NICS_CSV_HEADER_EOL) - 1) != 0) {
				N_Ef(nvmeibt_ib_read_local_nics_e2, "Local server nics.csv has invalid header @STR", line_ptr);
				break;
			}
		}
		else {
			char *field;
			char *tokptr = line_ptr;

			char ibv_devname[NVMEIB_IB_DEVICE_NAME_MAX];
			int port = 0;
			int pkey = 0;
			enum nvmeib_rdma_transport transport = rtr_unknown;
			union ibv_gid gid, hw_gid, sw_gid;
			bool found = false;
			int i;
			int gid_index = 0;
			int mtu = 1024;
			bool roce_v2 = false;
			bool roce_ipv6 = false;
			char ndev_name[NVMEIB_IB_DEVICE_NAME_MAX];

			for (i = 0; (field = strsep(&tokptr, ",")) != NULL; i++) {
				switch (i) {
				case 0: /* Device name */
					nvmeibt_strlcpy(ibv_devname, field, sizeof(ibv_devname));
					break;
				case 1: /* HW GID */
				case 13: /* SW GID */
					if (sscanf(field, "0x%16lx%16lx",
						/*Jared: Casting is needed to compile on FC27 */
						(unsigned long *)&gid.global.subnet_prefix,
						(unsigned long *)&gid.global.interface_id) != 2) {
						N_Ef(nvmeibt_ib_read_local_nics_e3, "Error processing GID field @STR", field);
						goto free_mem;
					}
					if (i == 1) {
						hw_gid.global.subnet_prefix = NVMEIB_HTONLL(gid.global.subnet_prefix);
						hw_gid.global.interface_id = NVMEIB_HTONLL(gid.global.interface_id);
					} else {
						sw_gid.global.subnet_prefix = NVMEIB_HTONLL(gid.global.subnet_prefix);
						sw_gid.global.interface_id = NVMEIB_HTONLL(gid.global.interface_id);
					}
					break;
				case 2: /* Port */
					port = atoi(field);
					if (port < 0 || port > 256) {
						N_Ef(nvmeibt_ib_read_local_nics_e4, "Invalid port @STR in nics.csv", field);
						goto free_mem;
					}
					break;
				case 3: /* Pkey */
					pkey = (int)strtol(field, NULL, 16);
					if (pkey < 0 || pkey > 0xffff) {
						N_Ef(nvmeibt_ib_read_local_nics_e5, "Invalid pkey @STR in nics.csv", field);
						goto free_mem;
					}
					break;
				case 4: /* Link type */
					transport = nvmeib_transport_cton(field[0]);
					if (transport == rtr_unknown) {
						N_Ef(nvmeibt_ib_read_local_nics_e6, "Invalid link type @STR in nics.csv", field);
						goto free_mem;
					}
					else if (transport == rtr_ib)
						lnd->n_ib_nics++;
					break;
				case 5: /* State */
					break;
				case 6: /* MTU */
					mtu = atoi(field);
					if (mtu != 256 && mtu != 512 && mtu != 1024 && mtu != 2048 && mtu != 4096) {
						N_Ef(nvmeibt_ib_read_local_nics_e7, "Invalid mtu @STR in nics.csv", field);
						goto free_mem;
					}
					break;
				case 7: /* Max MTU */
					break;
				case 8: /* GID Index */
					gid_index = atoi(field);
					if (gid_index < 0 || gid_index > 256) {
						N_Ef(nvmeibt_ib_read_local_nics_e8, "Invalid gid_index @STR in nics.csv", field);
						goto free_mem;
					}
					break;
				case 9: /* RoCE V2 */
					if (strcmp(field, "true") == 0)
						roce_v2 = true;
					else if (strcmp(field, "false") != 0) {
						N_Ef(nvmeibt_ib_read_local_nics_e9, "Invalid RoCE V2 value @STR in nics.csv", field);
						goto free_mem;
					}
					break;
				case 10: /* RoCE IPv6 */
					if (strcmp(field, "true") == 0)
						roce_ipv6 = true;
					else if (strcmp(field, "false") != 0) {
						N_Ef(nvmeibt_ib_read_local_nics_e10, "Invalid RoCE IPv6 value @STR in nics.csv", field);
						goto free_mem;
					}
					break;
				case 11: /* Used */
					break;
				case 12: /* Device network name */
					nvmeibt_strlcpy(ndev_name, field, sizeof(ndev_name));
					break;
				default:
					N_Ef(nvmeibt_ib_read_local_nics_e11, "Invalid number of fields in nics.csv");
					goto free_mem;
				}
			}

			rv = 0;

			/* Look to see if nic is already there */
			for (i = 0; i < lnd->n_nics; i++) {
				if (strncmp(lnd->nics[i].ibv_devname, ibv_devname, NVMEIB_IB_DEVICE_NAME_MAX) == 0) {
					found = true;
					break;
				}
			}

			if (!found) {
				memset(&lnd->nics[i], 0, sizeof(lnd->nics[i]));
				nvmeibt_strlcpy(lnd->nics[i].ibv_devname, ibv_devname, sizeof(lnd->nics[i].ibv_devname));
			}

			lnd->nics[i].ports[port].hw_gid = hw_gid;
			lnd->nics[i].ports[port].sw_gid = sw_gid;
			lnd->nics[i].ports[port].gid_index = (u8)gid_index;
			lnd->nics[i].ports[port].mtu = (uint16_t)mtu;
			lnd->nics[i].ports[port].pkey = (uint16_t)pkey;
			lnd->nics[i].ports[port].transport = transport;
			lnd->nics[i].ports[port].roce_v2 = roce_v2;
			lnd->nics[i].ports[port].roce_ipv6 = roce_ipv6;
			lnd->nics[i].ports[port].valid = true;
			nvmeibt_strlcpy(lnd->nics[i].ports[port].ndev_name, ndev_name, sizeof(lnd->nics[i].ports[port].ndev_name));

			if (!found)
				lnd->n_nics++;
		}
		line++;
	}

	N_Tf(nvmeibt_ib_read_local_nics_t1, "Read @N_NICS local nics from @LINE_INT lines of nics.csv", lnd->n_nics, line);

free_mem:

	free(line_ptr);
	fclose(file);
out:
	NFOUT;
	return rv;
}

int nvmeibt_ib_common_ib_is_dev_allowed(struct local_nics_data *lnd, char *dev_name)
{
	struct local_nic_data *nic;
	int i, ret = 0;

	NFIN;
	for (i = 0; i < lnd->n_nics; i++) {
		nic = &lnd->nics[i];
		if (strncmp(dev_name, nic->ibv_devname, NVMEIB_IB_DEVICE_NAME_MAX) == 0 ||
			/* For TCP we must check the ndev_name as siw wraps it with a new name*/ 
			strncmp(dev_name, nic->ports[1].ndev_name, NVMEIB_IB_DEVICE_NAME_MAX) == 0) {
			ret = 1;
			break;
		}
	}
	NFOUT;
	return ret;
}

const void *nvmeibt_sockaddr_get_inet_addr(
	const struct sockaddr_storage *addr)
{
    switch (((struct sockaddr *)addr)->sa_family) {
    case AF_INET:
    	return &TOMA_SOCKET_INET_ADDR(addr);
    case AF_INET6:
    	return &TOMA_SOCKET_INET6_ADDR(addr);
	case AF_IB:
		return ((struct sockaddr_ib *)addr)->sib_addr.sib_raw;
    default:
        N_Df(nvmeibt_sockaddr_get_inet_addr_e1,
			"unknown address family: @INT",
			((struct sockaddr *)addr)->sa_family);
        return NULL;
    }
}

int nvmeibt_ib_common_device_uuid_str_to_raw(
	union ibv_gid *ibv_gid, const char *device_uuid_str)
{
	int i, rv;
	char gid[3];

	if (strstr(device_uuid_str, ":") != NULL) {
		/* String is formatted, we can use IPv6 routines to decode it */
		if (inet_pton(AF_INET6, device_uuid_str,
			(struct in6_addr *)ibv_gid) <= 0) {
			N_Tf(tibc_dustr_t1,
				"Bad device_uuid_str '@DEVICE_UUID_STR'", device_uuid_str);
			rv = -1;
        }
		else
			rv = 0;

		goto out;
	}

	if (strlen(device_uuid_str) != 32) {
		N_Tf(tibc_dustr_t2,
			"Bad device_uuid_str '@DEVICE_UUID_STR'", device_uuid_str);
		rv = -1;
		goto out;
	}
	gid[2] = '\0';
	for (i = 0; i < 16; ++i) {
		memcpy(gid, device_uuid_str + i * 2, 2);
		ibv_gid->raw[i] = strtoul(gid, NULL, 16);
	}
	rv = 0;

out:
	return rv;
}

int nvmeibt_ib_common_rdma_gid2ip( struct sockaddr_storage *out,
	union ibv_gid *gid, unsigned short port, int is_ib, uint16_t pkey)
{
	struct sockaddr_in *pip;
	struct sockaddr_in6 *pip6;
	struct sockaddr_ib *pib;
	int rv;
	if (is_ib) {
		pib = (struct sockaddr_ib *)out;
		pib->sib_family = AF_IB;
		pib->sib_sid = NVMEIB_HTONLL(RDMA_CM_IB_UD_SERVICE_ID);
		pib->sib_sid_mask = ~(uint64_t)0;
		pib->sib_pkey = nvmeib_htons(pkey);
		memcpy(pib->sib_addr.sib_raw, gid->raw, sizeof(pib->sib_addr.sib_raw));
		rv = -1;
	}
	else {
		if (ipv6_addr_v4mapped((struct in6_addr *)gid)) {
			pip = (struct sockaddr_in *)out;
			memset(pip, 0, sizeof(*pip));
			pip->sin_family = PF_INET;
			memcpy(&pip->sin_addr.s_addr, gid->raw + 12, 4);
			pip->sin_port = nvmeib_htons(port);
			rv = 0;
		}
		else {
			pip6 = (struct sockaddr_in6 *)out;
			pip6->sin6_family = PF_INET6;
			memcpy(&pip6->sin6_addr.s6_addr, gid->raw, 16);
			pip6->sin6_port = nvmeib_htons(port);
			rv = 1;
		}
	}
	return rv;
}
