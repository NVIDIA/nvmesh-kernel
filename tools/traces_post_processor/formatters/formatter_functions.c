/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

/*clang-format off*/
#include "common/kr_incs.h" /*Must be first*/
/*clang-format on*/
#include "nvmeib_shared.h"
#include "nvmeib_types.h"
#include "nvmeibc_trend_types.h"
#include "nvmeibs_trend_types.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "nvmeib_io_stats.h"

#include <arpa/inet.h>

int fmt_nvmeibc_md_buf(char *buf, int len, long arg, __attribute__((__unused__)) int datalen) {
	union nvmeibc_block_dp_ec_data_block_md *md = (void *)arg;
	return scnprintf(buf, len, "ver=0x%x, d2j_rng=0x%x, D={edic=0x%x}, P={edic=0x%x, db={%u,%u}}, txid=0x%x, jri=0x%x, raw=0x%llx",
	md->D.version, md->D.d2j_rng, md->D.edic, md->P.edic, md->P.dbits_0, md->P.dbits_1, md->tx_id, md->jri, md->raw);
}

int fmt_nvmeibc_md_int(char *buf, int len, long arg, int datalen) {
	return fmt_nvmeibc_md_buf(buf, len, (long)&arg, sizeof(arg));
}

int fmt_nvmeibc_jour_md_buf(char *buf, int len, long arg, __attribute__((__unused__)) int datalen) {
	union jblock_md *md = (void *)arg;
	const u64 j2d = nvmeibc_block_dp_ec_jmd_decode_j2d_only(md);
	if (md->version == NVMEIBC_JOURNAL_MD_VERSION_UNPACKED) {
		return scnprintf(buf, len, "J={j2d=0x%llx, tx_id=0x%x, tx_bmp=0x%x, version=0x%x}, raw=0x%llx",
			j2d, md->tx_id, md->tx_bmp, md->version, md->raw);
	} else {
		return scnprintf(buf, len, "J={j2d=0x%llx, tx_id=0x%x, tx_bmp=0x%x, has_next=0x%x, has_prev=0x%x, version=0x%x}, raw=0x%llx",
			j2d, md->tx_id, nvmeibc_block_dp_ec_md_txbm_decompress(md->v1.tx_bmp_zip), md->v1.has_next, 0 /*md->v1.has_prev*/, md->version, md->raw);
	}
}

int fmt_nvmeibc_jour_md_int(char *buf, int len, long arg, int datalen) {
	return fmt_nvmeibc_jour_md_buf(buf, len, (long)&arg, sizeof(arg));
}

int fmt_nvmeibc_dbits_entry(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return nvmeibc_dbits_entry_to_str(buf, len, arg);
}

int fmt_nvmeib_lock_id(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return nvmeib_lock_id_to_str(buf, len, arg);
}

int fmt_nvmeib_blkset_info(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return nvmeib_blkset_info_to_str(buf, len, arg);
}

int fmt_nvmeib_blkset_problem_report(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return nvmeib_blkset_problem_report_to_str(buf, len, arg);
}

int fmt_nvmeib_blkset_sparse_report(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return nvmeib_blkset_sparse_report_to_str(buf, len, arg);
}

int fmt_nvmeib_block_io_op(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s", nvmeib_block_io_op_str((enum nvmeib_block_io_op) arg));
}

int fmt_strerror(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s", strerror(arg));
}

int fmt_yesno(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	if (arg) return scnprintf(buf, len, "Yes");
	else return scnprintf(buf, len, "No");
}

int fmt_kafka_offset(char *buf, int len, long arg, int datalen) {
	const long topic_offset = (arg & ((1L << 48)-1));	// Split bit field
	const int topic_change_no = (int)(arg >> 56);
	(void)datalen;
	if (arg < 0)
		return scnprintf(buf, len, "k_offset[%ld]", arg);
	if (topic_change_no <= 1)			// No topic change so not interesting to print
		return scnprintf(buf, len, "k_offset[%lu]", topic_offset);
	return     scnprintf(buf, len, "k_offset[%lu|%u]", topic_offset, topic_change_no);
}

int fmt_disk_disconnection_status_from_buf(char *buf, int len, long arg, int datalen) {
	return scnprintf(buf, len, "%s(%d)", nvmeibc_disk_release_reason_str[(int)arg],
	(int)arg);
}

int fmt_disk_discover_status_from_buf(char *buf, int len, long arg, int datalen) {
	return scnprintf(buf, len, "%s(%d)", nvmeibc_disk_discover_status_str[(int)arg],
	(int)arg);
}

int fmt_arnic_discover_status_from_buf(char *buf, int len, long arg, int datalen) {
	return scnprintf(buf, len, "%s(%d)", nvmeibc_arnic_discover_status_str[(int)arg],
	(int)arg);
}

int fmt_logout_reason_status_from_buf(char *buf, int len, long arg, int datalen) {
	return scnprintf(buf, len, "%s(%d)", nvmeibs_logout_reason_str[(int)arg],
	(int)arg);
}

int fmt_serjio_status(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", nvmeib_shared_serjio_status_to_str((int)arg), (int)arg);
}

int fmt_serjio_state(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", nvmeib_shared_serjio_state_to_str((int)arg), (int)arg);
}

int fmt_serjio_jentry_state(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", nvmeib_shared_serjio_jentry_state_to_str((int)arg), (int)arg);
}

int fmt_serjio_jentry_state_mask(char *buf, int len, long arg, int datalen) {
	int i, count = 0;
	unsigned int mask = (unsigned int)arg;
	(void)datalen;
	for (i = 0; i < MAX_JENTRY_STATE; i++) {
		if ((1 << i) & mask) {
			count += scnprintf(buf + count, len - count, "%s|", nvmeib_shared_serjio_jentry_state_to_str(i));
		}
	}
	/* Remove the last '|' */
	buf[count - 1] = '\0';
	return count - 1;
}

int fmt_serjio_jentry_state_chng_reason(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", nvmeib_shared_serjio_jentry_state_chng_reason_to_str((int)arg), (int)arg);
}

int fmt_serjio_jrange_status(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", nvmeib_shared_serjio_jrange_status_to_str((int)arg), (int)arg);
}

int fmt_expect_last_wqe(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", nvmeib_rdma_expect_last_wqe_to_str((int)arg), (int)arg);
}

int fmt_find_sock_cep_state(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s(%d)", find_path_cep_state_to_str((int)arg), (int)arg);
}

int fmt_iostats_verb(char *buf, int len, long arg, int datalen) {
	(void)datalen;
	return scnprintf(buf, len, "%s", verb_to_string((int)arg, false));
}

/*******************************************************************************
 * The below functions are not used by pager, but they are pretty useful for   *
 * formatter.py utility to iteractively analyze binary content of trace files. *
 * They are on top of very generic structures, not nvmesh specific.            *
 *******************************************************************************/

int fmt_hex(char *buf, int len, long ptr, int datalen) {
	static char hex_asc[17] = "0123456789abcdef";
	int i;
	const char *msg = (const char *)ptr;
	const char *orig = buf;
	(void)len;

	for (i = 0; i < datalen; i++) {
		if (i)
			*buf++ = ' ';
		*buf++ = hex_asc[(msg[i] >> 4) & 0xf];
		*buf++ = hex_asc[msg[i] & 0xf];
	}
	*buf = '\0';
	return buf - orig + 1;
}

int fmt_uuid(char *buf, int len, long ptr, int datalen) {
	const unsigned char *uuid = (const unsigned char *)ptr;
	return snprintf(buf, len, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", uuid[0], uuid[1],
	                 uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
	                 uuid[12], uuid[13], uuid[14], uuid[15]);
}

int fmt_uuid_prefix_be(char *buf, int len, long ptr, int datalen) {
	const unsigned char *uuid = (const unsigned char *)ptr;
	return snprintf(buf, len, "%02X%02X%02X%02X", uuid[0], uuid[1], uuid[2], uuid[3]);
}

int fmt_uuid_prefix_le(char *buf, int len, long ptr, int datalen) {
	const unsigned char *uuid = (const unsigned char *)ptr;
	return snprintf(buf, len, "%02x%02x%02x%02x", uuid[3], uuid[2], uuid[1], uuid[0]);
}

int fmt_string_n(char *buf, int len, long ptr, int datalen) {
	const char *str = (const char *)ptr;
	strncpy(buf, str, datalen); buf[datalen] = '\0';
	return datalen + 1;
}

int fmt_ipv6(char *buf, int len, long ptr, int datalen) {
	const struct in6_addr *ip = (const struct in6_addr *)ptr;
	inet_ntop(AF_INET6, ip, buf, len);
	return len;
}

int fmt_u64_from_buf(char *buf, int len, long ptr, int datalen) {
	const unsigned long long *i = (const unsigned long long *)ptr;
	return scnprintf(buf, len, "0x%llx", *i);
}

int fmt_u32_from_buf(char *buf, int len, long ptr, int datalen) {
	const unsigned int *i = (const unsigned int *)ptr;
	return scnprintf(buf, len, "0x%x", *i);
}

__attribute__((constructor)) static void __ctor() {
	init_roles_pair_compression_tables();
}
