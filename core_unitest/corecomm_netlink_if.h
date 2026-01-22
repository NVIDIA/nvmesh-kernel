/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CORECOMM_NETLINK_IF
#define CORECOMM_NETLINK_IF

#include "corecomm.h"
#include "corecomm_netlink_rpc.h"

/**
 * This file specifies the communication interface between corecomm userspace
 * library and kernel space via netlink
 */

/**
 * Protocol number, needs to be known by both user and kernel space
 */
#define NETLINK_CORECOMM 27

/******************* USERSPACE=>KERNELSPACE *******************/

/* Those offsets are needed to avoid collision with standard netlink messages */
#define __COREDUMP_START_MSGID 0x1000

#define NLRPC_API_LIST                                                         \
	corecomm_format_local_disk_, corecomm_set_symbol_, corecomm_read_symbol_,  \
	    corecomm_register_arnic_, corecomm_discover_, corecomm_disk_remove_,   \
	    corecomm_pd_cmpxchg_, corecomm_pd_read_lock_, corecomm_alloc_ndb_,     \
	    corecomm_free_ndb_, corecomm_stamp_ndb_, corecomm_stamp_ndb_md_,       \
	    corecomm_read_stamp_ndb_, corecomm_read_stamp_ndb_md_,                 \
	    corecomm_direct_read_lock_, corecomm_pd_write_blkset_info_,            \
	    corecomm_nvmeibc_pd_io_, corecomm_pd_get_blkset_problems_,             \
	    corecomm_pd_jmdc_read_, corecomm_gen_blkset_recovered_,                \
	    corecomm_gen_get_uuid_jour_, corecomm_gen_jentry_erase_,               \
	    corecomm_pd_free_jrnl_ents_, corecomm_pd_please_kill_yourself_,        \
	    corecomm_freeze_, corecomm_unfreeze_, corecomm_alloc_jrnls_,           \
	    corecomm_free_jrnls_, corecomm_jam_lba_2_idx_

NLRPC_API_CODES(__COREDUMP_START_MSGID, NLRPC_API_LIST);

#endif /*CORECOMM_NETLINK_IF*/