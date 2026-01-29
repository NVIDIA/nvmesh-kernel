/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef CORECOMM_H
#define CORECOMM_H

#ifndef __KERNEL__
#include <stdint.h>
#include <sys/types.h>
typedef uint8_t u8;
typedef int8_t s8;
typedef uint16_t u16;
typedef int16_t s16;
typedef uint32_t u32;
typedef int32_t s32;
typedef long long unsigned u64;
typedef int64_t s64;
typedef u8 bool;

#define true 1
#define false 0

#define NVMEIBC_SECTOR_SHIFT 12
#endif

/**
 * This is the generic interface for corecomm library.
 * !!! Please keep implementation specific details away from this file. !!!
 * Currently there is only on implementation - via netlink, but his may
 * change in the future.
 */

/** Base type for handles */
typedef long long generic_handle;

/** Represents connection between single userspace pid and kernel nl socket*/
typedef long long corecomm_handle;

/** Reperesents a single cdisk object */
typedef generic_handle cdisk_handle;

/** Represents a single ndb entry */
typedef generic_handle ndb_handle;

/** Represents a pointer to the user space. Do not use directly. */
typedef unsigned long long corecomm_userspace_ptr;

/** Why is this needed?
 *
 * This typedef used to specify the return value of a function. 0 means success,
 * -1 means errno shall be checked. From pure C code point of view it is quite
 * pointless typedef, maybe adds a little clarity.
 * It is *very* important for python bindings - when python code encounters a
 * function that returns this type, it will be converted to python style
 * exception handling code.
 */
typedef int value_or_minus_1;

/**
 * Max bytes we can transfer safely in one op without overflowing the netlink
 * buffers.
 * @note I wanted to hide netlink implementation details from this file. These
 * couple of constants break the rule as they are netlink specific. Acceptable
 * that for now, lets not let it grow.
 */

/*
 * @TODO: Need to change this. Currently it is too limited. Need to add an
 * interface for the user space to pass a memory buffer to the kernel, while
 * kernel will fill that buffer directly. This way I will not have to limit
 * myself to these sizes.
 */
#define CORECOMM_SAFE_DATA_SIZE_ONE_OP 16000
#define CORECOMM_MAX_DISKS_FOR_JALLOC 20

#define CORECOMM_MAX_DATA_STAMPS_ONE_OP                                        \
	(CORECOMM_SAFE_DATA_SIZE_ONE_OP / sizeof(unsigned long))

#define CORECOMM_MAX_JOURNAL_RANGES_ONE_OP                                     \
	((CORECOMM_SAFE_DATA_SIZE_ONE_OP -                                         \
	  sizeof(struct corecomm_jmdc_read_jrnl_data)) /                           \
	 (sizeof(struct corecomm_jmdc_rng_data) +                                  \
	  CORECOMM_JOURNAL_ENTRIES_PER_RANGE *                                     \
	      sizeof(struct corecomm_jrnl_ent_md) +                                \
	  CORECOMM_JOURNAL_ENTRIES_PER_RANGE *                                     \
	      sizeof(struct corecomm_jblock_md_decompressed)))

/* Data structures useful to pass to kernel when dealing with large data */
typedef char name_t[40];
typedef char long_name_t[120];
struct page_container {
	char data[4096];
};
typedef unsigned long data_stamps_arr_t[CORECOMM_MAX_DATA_STAMPS_ONE_OP];
struct data_stamps_arr_container {
	data_stamps_arr_t data;
};
union problems_report_container {
	unsigned long
	    sparse[CORECOMM_SAFE_DATA_SIZE_ONE_OP / sizeof(unsigned long)];
	unsigned short
	    problems[CORECOMM_SAFE_DATA_SIZE_ONE_OP / sizeof(unsigned short)];
};

typedef cdisk_handle corecomm_disks_arr[CORECOMM_MAX_DISKS_FOR_JALLOC];
struct corecomm_disks_set {
	corecomm_disks_arr disks;
};

typedef unsigned long long corecomm_lbas_arr[CORECOMM_MAX_DISKS_FOR_JALLOC];
struct corecomm_lbas_set {
	corecomm_lbas_arr lbas;
};

/*@TODO: BIG TODO: find a way to include kernel data structures instead of copying **/
#define NVMEIB_EC_JMDC_BITS_J2D	        (32)
#define NVMEIB_EC_JMDC_BITS_TX_ID		(20)
#define NVMEIB_EC_JMDC_BITS_TX_BMP	    10
#define NVMEIB_EC_JMDC_BITS_TX_BMP_ZIP   8
#define NVMEIB_EC_JMDC_BITS_VER          2
#define NVMEIB_EC_JMDC_BITS_LINK         1
union corecomm_jblock_md {
	struct corecomm_jblock_md_v0 {
		u32 j2d_0		                : NVMEIB_EC_JMDC_BITS_J2D;
		u32 tx_id		                : NVMEIB_EC_JMDC_BITS_TX_ID;
		u32 tx_bmp		                : NVMEIB_EC_JMDC_BITS_TX_BMP;
		u32 version		                : NVMEIB_EC_JMDC_BITS_VER;
	} v0 __attribute__((packed));
	struct corecomm_jblock_md_v1 {
		u32 j2d_1	 	                : NVMEIB_EC_JMDC_BITS_J2D;
		u32 tx_id		                : NVMEIB_EC_JMDC_BITS_TX_ID;
		u32 tx_bmp_zip	                : NVMEIB_EC_JMDC_BITS_TX_BMP_ZIP;
		u32 has_next                    : NVMEIB_EC_JMDC_BITS_LINK;
		u32 j2d_extended                : NVMEIB_EC_JMDC_BITS_LINK;
		u32 version		                : NVMEIB_EC_JMDC_BITS_VER;
	} v1 __attribute__((packed));
	u64 raw;
}__attribute__((packed));

/**
 * JMDC related data structures.
 * @TODO: So for now I implemented these data structures as a plain copypasta
 * from the kernel. That has to change. For now I don't see a nice way without
 * serious code refactoring. Can't simply include kerenel data structures here,
 * too much of ugly mixed code inside the headers.
 */
#define CORECOMM_JOURNAL_ENTRIES_PER_RANGE 512
#define CORECOMM_JOURNAL_RANGES 1024
struct corecomm_jmdc_read_jrnl_data {
	unsigned short lba_shift;
	unsigned short num_ents_rng;
	unsigned short num_dirty_rng;
	unsigned short num_rng;
	unsigned long jrnl_start_lba;
	unsigned long jrnl_len_lba;
	unsigned int num_ents;
	name_t serjio_boot_id;
} __attribute__((packed));

struct corecomm_jmdc_rng_data {
	unsigned char client_uuid[16];
	unsigned short rng_idx;
	unsigned int rng_size_lba;
	unsigned long rng_start_lba;
	unsigned long rng_gen_id;
	unsigned char only_dirty_ents;
	unsigned char rsvd0[7];
	unsigned int rng_ent_offset;
	unsigned int
	    dirty_ents_bmp[(CORECOMM_JOURNAL_ENTRIES_PER_RANGE / sizeof(int)) / 8];
	unsigned int
	    abnd_ents_bmp[(CORECOMM_JOURNAL_ENTRIES_PER_RANGE / sizeof(int)) / 8];
	unsigned int binje;
} __attribute__((packed));

struct corecomm_jrnl_ent_md {
	unsigned char ent_gen_id;
} __attribute__((packed));

struct corecomm_jblock_md_decompressed {
	u64 j2d;
	u32 tx_id;
	u16 tx_bmp;
	u8 version; // For debug/print only: not really needed, store what was the
	            // original version of the decoded jblock_md.
	bool has_next;
} __attribute__((packed));

struct corecomm_jmdc_range {
	struct corecomm_jblock_md_decompressed
	    ents[CORECOMM_JOURNAL_ENTRIES_PER_RANGE];
};

struct corecomm_jmdc_container {
	struct corecomm_jmdc_read_jrnl_data jrnl_desc;
	struct __rng {
		struct corecomm_jmdc_rng_data arr[CORECOMM_JOURNAL_RANGES];
		size_t len;
	} rng;
	struct __ent_md {
		struct corecomm_jrnl_ent_md
		    arr[CORECOMM_JOURNAL_ENTRIES_PER_RANGE * CORECOMM_JOURNAL_RANGES];
		size_t len;
	} ent_md;
	struct __md {
		struct corecomm_jblock_md_decompressed
		    arr[CORECOMM_JOURNAL_ENTRIES_PER_RANGE * CORECOMM_JOURNAL_RANGES];
		size_t len;
	} md;
};

/* Format request */
struct corecomm_format_disk {
	name_t disk_id;
	unsigned int vendor_id;
	union corecomm_format_disk_format_id {
		unsigned int val;
		struct corecomm_format_disk_format_id_bitfields {
			unsigned int id : 4;
			unsigned int is_inline : 1;
		} bf;
	} format_id;
	union corecomm_format_disk_flags {
		unsigned int all;
		struct corecomm_format_disk_flags_bitfields {
			unsigned int flag_nvme_format : 1;
			unsigned int flag_delete_create_ns : 1;
			unsigned int flag_reset_ctrlr : 1;
		} bf;
	} flags;
};

/* Result of the disk format */
struct corecomm_new_format_info {
	char new_dev_file_name[256];
	unsigned long new_n_pblk;
	int new_seq;
};

/***** @COVENTIONS *****
 *
 * To the future generations: please make sure to follow these conventions,
 * not only they contribute to code clarity, they also help python bindings to
 * understand the code better and generate auto decorators.
 *
 * 1. Function returning success or error, will return value_or_minus_1. 0 means
 * success, -1 means error. If -1 is returned, error code is set in errno and
 * corecomm_errno.
 *
 * 2. Error codes returned by user space (before error message was dispatched)
 * are positive.
 *
 * 3. Error codes returned by kernel are negitive.
 *
 * 4. If a function returns complex data, its last argument will be a poiner to
 * the complex data structute. Variable name must be *output*.
 *
 * 5. All addresses are in LBAs by default, unless explicitly specified
 * otherwise (by _bytes / _locksets etc).
 */

struct lock_data {
	int status; /* Lock status (taken/contended/transmonoprogressive) */
	unsigned long long value; /* Lock value */
	int io_status;            /* IO status in case of piggiback operation */
};

enum rdma_type { CORECOMM_RDMA_IB, CORECOMM_RDMA_ROCE, CORECOMM_RDMA_IWARP };

enum io_type {
	CORECOMM_IO_READ = 1,
	CORECOMM_IO_WRITE,
	CORECOMM_IO_DISCARD,
	CORECOMM_IO_WRITE_UNCOR = 5
};

/**
 * Returns the value of errno after the last corecomm operation (unaffected by
 * other ops that normally change errno). This is needed due to unability of
 * remotely connected controller to retrieve last errno without modifying its
 * value.
 * @return Last errno value after any of corecom calls
 */
long corecomm_errno(void);

/**
 * Create corecomm connection top the backend.
 * @return Pointer to an initialized corecomm object or -1 and set errno.
 * @note Not thread safe. Call one at a time.
 * @todo: Make thread safe if need arises
 */
corecomm_handle corecomm_create(void);

/**
 * Destroy corecomm object
 * @param handle Corecomm connection object
 * @note Not thread safe. Call one at a time.
 * @todo: Make thread safe if need arises
 */
void corecomm_destroy(corecomm_handle handle);

/**
 * Use corecomm infra to directly inject data into an exposed kernel symbol
 * @param handle Corecomm connection object
 * @param sym_name Symbol name limited to 120 characters
 * @param size Data buffer size
 * @param value Data buffer
 * @param deref 1 if symbol is a pointer and data shall be written to its
 * destination, 0 otherwise
 * @return 0 or -1 and errno set.
 */
value_or_minus_1 corecomm_set_symbol(corecomm_handle handle,
                                     const char *sym_name, int size,
                                     struct page_container *value, int deref);

/**
 * Use corecomm infra to directly read data from an exposed kernel symbol
 * @param handle Corecomm connection object
 * @param sym_name Symbol name limited to 120 characters
 * @param size Data buffer size
 * @param deref 1 if symbol is a pointer and data shall be read from
 * @param output Data buffer for output
 * pointer destination, 0 otherwise
 * @return 0 or -1 and errno set.
 */
value_or_minus_1 corecomm_read_symbol(corecomm_handle handle,
                                      const char *sym_name, int size, int deref,
                                      struct page_container *output);

/**
 * Register admin channel rnic on some target, must be done before discovery
 * @param handle Corecomm connection object
 * @param node_id Target ID
 * @param gid arnic GID
 * @param pkey port key
 * @param type RDMA connection type
 * @return 0 or -1 and errno set.
 */
value_or_minus_1 corecomm_register_arnic(corecomm_handle handle,
                                         const char *node_id, const char *gid,
                                         unsigned short pkey,
                                         enum rdma_type type);

/**
 * Run disk discovery on given connetion
 * @param handle Corecomm connection object
 * @param disk_name Physical disk name
 * @param node_id Physical node id
 * @return Handle to nvmeibc_disk kernel object, that can be later used in
 * further ops, or -1 and set errno on failure
 */
cdisk_handle corecomm_discover(corecomm_handle handle, const char *disk_name,
                               const char *node_id);

/**
 * The inverse of @corecomm_discover
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @return 0 or -1 and errno set.
 * @note Shall not fail unless called with invalid arguments or netlink
 * connection problem
 */
value_or_minus_1 corecomm_disk_remove(corecomm_handle handle,
                                      cdisk_handle disk);

/**
 * Allocate NDB of a given size
 * @param handle Corecomm connection object
 * @param n_pages Size of NDB
 * @return Handle to ndb kernel object, that can be later used in
 * further ops, or -1 and set errno on failure
 * @note Will zero NDB
 */
ndb_handle corecomm_alloc_ndb(corecomm_handle handle, unsigned long n_pages);

/**
 * Free NDB object
 * @param handle Corecomm connection object
 * @param ndb NDB object handle
 * @return 0 or -1 and errno set.
 * @note Shall not fail unless called with invalid arguments or netlink
 * connection problem
 */
value_or_minus_1 corecomm_free_ndb(corecomm_handle handle, ndb_handle ndb);

/**
 * Write 64 bit stamp on each page in NDB at addr 0 starting given page up to
 * given page
 * @param handle Corecomm connection object
 * @param ndb NDB object handle
 * @param stamp_array Array of data stamps
 * @param n_stamps Number of stamps
 * @param offset_page Page to start stamping at
 * @param offset_bytes Offset of the stamp inside the page
 * @param is_md Whether the read is from data or metadata part of ndb
 * @return 0 or -1 and errno set.
 * @note If more than CORECOMM_MAX_DATA_STAMPS_ONE_OP stamps are sent, will
 * split into multiple netlink calls. Normally shall not fail unless there is
 * a netlink problem.
 */
value_or_minus_1 corecomm_stamp_ndb(corecomm_handle handle, ndb_handle ndb,
                                    unsigned long *stamp_array,
                                    unsigned int n_stamps,
                                    unsigned int offset_page,
                                    unsigned int offset_bytes, int is_md);

/**
 * Read 64 bit stamp from each page in NDB at addr 0 starting given page up to
 * given page
 * @param handle Corecomm connection object
 * @param ndb NDB object handle
 * @param stamp_array Array where to put data stamps
 * @param n_stamps Number of stamps
 * @param offset_page Page to start reading stamps at
 * @param offset_bytes Offset of the stamp inside the page
 * @param is_md Whether the read is from data or metadata part of ndb
 * @return 0 or -1 and errno set.
 * @note If more than CORECOMM_MAX_DATA_STAMPS_ONE_OP stamps are sent, will
 * split into multiple netlink calls. Normally shall not fail unless there is
 * a netlink problem.
 */
value_or_minus_1 corecomm_read_stamps_ndb(corecomm_handle handle,
                                          ndb_handle ndb,
                                          unsigned long *stamp_array,
                                          unsigned int n_stamps,
                                          unsigned int offset_page,
                                          unsigned int offset_bytes, int is_md);

/************* PAUSABLE LAYER API **************
 * Functions below invoke pausable layer API calls.
 */

/**
 * Invoke nvmeibc_pd_dbg_please_kill_yourself - initiate BUG on the server side
 * via client
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @return 0 or -1 and errno set.
 */
value_or_minus_1 corecomm_pd_dbg_please_kill_yourself(corecomm_handle handle,
                                                      cdisk_handle disk,
                                                      unsigned int rsc_id,
                                                      unsigned long long dlba);

/**
 * Invoke nvmeibc_pd_cmpxchg
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param addr Lock LBA address
 * @param compare Compare value
 * @param exchange Exchange value
 * @param output Lock data structure to fill in
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_pd_cmpxchg(corecomm_handle handle, cdisk_handle disk,
                                     unsigned long long addr,
                                     unsigned long long compare,
                                     unsigned long long exchange,
                                     struct lock_data *output);

/**
 * Invoke nvmeibc_pd_read_lock
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param addr Lock LBA address
 * @param output Lock data structure to fill in
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_pd_read_lock(corecomm_handle handle,
                                       cdisk_handle disk,
                                       unsigned long long addr,
                                       struct lock_data *output);

/**
 * Invoke nvmeibc_pd_write_blkset_info
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param addr Lock LBA address
 * @param bi Blockset info to write
 * @param output Lock data structure to fill in
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_pd_write_blkset_info(corecomm_handle handle,
                                               cdisk_handle disk,
                                               unsigned long long addr,
                                               unsigned long long bi,
                                               struct lock_data *output);

/**
 * Invoke nvmeibc_pd_execute_io_blocks
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param ndb NDB handle
 * @param addr Start LBA address
 * @param len IO length
 * @param io_type IO type to perform
 * @param has_piggy_back 1 if IO has piggy back, 0 otherwise
 * @param pb_addr Lock LBA address for piggiback, if has one
 * @param pb_compare Compare value for piggiback, if has one
 * @param pb_exchange Exchange value for piggiback, if has one
 * @param sub_block Sub block to IO to, -1 if unused
 * @param output Lock data structure to fill in for piggiback, if has one
 * piggiback
 * @return 0 or -1 and errno set.
 * @note In case of discard, NDB handle is ignored. In case of read, result will
 * go to the NDB. In case of write, data is taken from the NDB.
 */
value_or_minus_1
corecomm_pd_execute_io_blocks(corecomm_handle handle, cdisk_handle disk,
                              ndb_handle ndb, unsigned long long addr,
                              unsigned long long len, enum io_type io_type,
                              int has_piggy_back, unsigned long long pb_addr,
                              int sub_block, struct lock_data *output);

/**
 * Invoke nvmeibc_pd_execute_io_jour_blocks
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param ndb NDB handle
 * @param addr Start LBA address
 * @param len IO length
 * @param io_type IO type to perform
 * @param has_piggy_back 1 if IO has piggy back, 0 otherwise
 * @param pb_addr Lock LBA address for piggiback, if has one
 * @param pb_compare Compare value for piggiback, if has one
 * @param pb_exchange Exchange value for piggiback, if has one
 * @param sub_block Sub block to IO to, -1 if unused
 * @param output Lock data structure to fill in for piggiback, if has one
 * piggiback
 * @return 0 or -1 and errno set.
 * @note In case of discard, NDB handle is ignored. In case of read, result will
 * go to the NDB. In case of write, data is taken from the NDB.
 */
value_or_minus_1 corecomm_pd_execute_io_jour_blocks(
    corecomm_handle handle, cdisk_handle disk, ndb_handle ndb,
    unsigned long long addr, unsigned long long len, enum io_type io_type,
    int has_piggy_back, unsigned long long pb_addr, int sub_block,
    struct lock_data *output);

/**
 * Invoke NVMEIBC_DISK_CMD_GEN cmd via nvmeibc_pd_get_blkset_problems.
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param start Start LBA address
 * @param len Length (units of LBA)
 * @param get_dbits Passthrough to PD
 * @param get_stales Passthrough to PD
 */
value_or_minus_1 corecomm_pd_get_blkset_problems(
    corecomm_handle handle, cdisk_handle disk, unsigned long long start,
    unsigned long long len, int get_dbits, int get_stales,
    union problems_report_container *output);

/**
 * Invoke NVMEIB_GEN_OP_GET_JMDC cmd via nvmeibc_pd_jmdc_read.
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param start_rng Range to start with
 * @param num_rng Number of ranges
 * @param dirty_only True if meant to return dirty entries only
 * @param output Output data to fill in if success
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_pd_jmdc_read(corecomm_handle handle,
                                       cdisk_handle disk,
                                       unsigned int start_rng,
                                       unsigned int num_rng, int dirty_only,
                                       struct corecomm_jmdc_container *output);

/**
 * Invoke BLKSET_RECOVERED gen cmd
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param client_uuid Client UUID
 * @param sgmnt_uuid Segment UUID
 * @param slice_size Slice size (used to calculate blockset number)
 * @param jrange Journal range id
 * @param jentry Journal entry id
 * @param pass2toma Non zero if command shall be passed to toma 0 otherwise
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 * @warning Using pass2toma!=0 without real volumes will gladly crash toma.
 * Useful for some tests though.
 */
value_or_minus_1
corecomm_gen_blkset_recovered(corecomm_handle handle, cdisk_handle disk,
                              const char *client_uuid, const char *sgmnt_uuid,
                              unsigned int slice_size, unsigned long jrange,
                              unsigned long jentry, int pass2toma);

/**
 * Invoke GET_UUID_JOUR gen cmd
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param client_uuid Client UUID
 * @param sgmnt_uuid Segment UUID
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 * Useful for some tests though.
 */
value_or_minus_1 corecomm_gen_get_uuid_jour(corecomm_handle handle,
                                            cdisk_handle disk,
                                            name_t client_uuid,
                                            name_t sgmnt_uuid,
                                            struct corecomm_jmdc_range *output);

/**
 * Invoke jentry erase API used by JAM
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param jentry Jentry ID to erase
 * @param jentry_gen_id Jentry gen ID (last known to the jam dummy, usually can
 * be found via the serjio procs)
 * @param output Will contain the gen command comp_code
 * @return 0 or -1 and errno set.
 * Useful for some tests though.
 */
value_or_minus_1 corecomm_gen_jentry_erase(corecomm_handle handle,
                                           cdisk_handle disk,
                                           unsigned short jentry,
                                           unsigned char jentry_gen_id,
                                           int *output);

/**
 * Invoke pausable free_jrnl_ents API
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param seg_uuid Segment uuid
 * @param start_blkset_slba Start blockset in units of SLBA
 * @param len_blksets Length in units of BLOCKSETS
 * @param pass2toma Call toma blkset recovered (not actually respected as by
 * server, don't bother)
 * @param lock_entry_raw Value of previous lock entry to pass to toma
 * @param serjio_boot_id Serjio boot ID (UUID, see in procs)
 * @param jrange Journal range
 * @param jentry Jentry ID
 * @param jentry_gen_id Jentry gen ID (last known to the jam dummy, usually can
 * be found via the serjio procs)
 * @param output Will contain the gen command comp_code
 * @return 0 or -1 and errno set.
 * Useful for some tests though.
 */
value_or_minus_1 corecomm_pd_free_jrnl_ents(
    corecomm_handle handle, cdisk_handle disk, const char *seg_uuid,
    unsigned long long start_blkset_slba, unsigned long long len_blksets,
    int pass2toma, unsigned long long lock_entry_raw,
    const char *serjio_boot_id, unsigned int jrange, unsigned int jentry,
    unsigned char jentry_gen_id, int num_ents, int *output);

/************* SERVER SIDE API **************
 * Functions below access server side structs directly, bypassing the
 * pausable layer.
 */

/**
 * Format local disk
 * @warning Requires an existing local server
 * @param handle Corecomm connection object
 * @param fd Format input arguments
 * @param output Will be filled with new disk info on success
 * @return 0 or -1 and errno set.
 */
value_or_minus_1
corecomm_format_local_disk(corecomm_handle handle,
                           struct corecomm_format_disk fd,
                           struct corecomm_new_format_info *output);

/**
 * Directly read lock, must be executed on server
 * @warning Requires an existing local server
 * @param handle Corecomm connection object
 * @param disk_name Disk name
 * @param addr Lock LBA address
 * @param output Lock data structure to fill in
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_direct_read_lock(corecomm_handle handle,
                                           const char *disk_name,
                                           unsigned long long addr,
                                           struct lock_data *output);

/**
 * Directly call disk freeze, blocking IO
 * @warning Requires an existing local server
 * @todo Find a better way to achieve this. Currently used to write GPT.
 * @param handle Corecomm connection object
 * @param disk_name Disk name
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_freeze(corecomm_handle handle, const char *disk_name);

/**
 * Directly call disk unfreeze, will unblock IO and cause serjio reread gpt
 * @warning Requires an existing local server
 * @todo Find a better way to achieve this. Currently used to write GPT.
 * @param handle Corecomm connection object
 * @param disk_name Disk name
 * @return 0 or -1 and errno set. If 0, fills in @output structure
 */
value_or_minus_1 corecomm_unfreeze(corecomm_handle handle,
                                   const char *disk_name);

/**
 * Allocate journals (directly call jam)
 * @note This is reqired to preform an IO to journal blocks
 * @param handle Corecomm connection object
 * @param n_disks Number of disks to allocate
 * @param disks Array of disks handle
 * @param txid Txid to be used for hash key
 * @param dlbas DLBAs to be used for hash key
 * @param output Array with enough space for @n_disks jlbas for output
 * @return 0 or -1 and errno set. If 0, fills in @output
 */
value_or_minus_1 corecomm_alloc_jrnls(corecomm_handle handle, int n_disks,
                                      struct corecomm_disks_set *disks,
                                      int txid, struct corecomm_lbas_set *dlbas,
                                      struct corecomm_lbas_set *output);

/**
 * Free journals (directly call jam)
 * @param handle Corecomm connection object
 * @param n_disks Number of disks / jlbas allocated
 * @param disks Array of disks handle
 * @param jlbas Array of jlbas preiously allocated
 * @param wr_sts_bm Write status bitmap, 0 - success, 1 - failure (bit per disk)
 * @return 0 or -1 and errno set.
 */
value_or_minus_1 corecomm_free_jrnls(corecomm_handle handle, int n_disks,
                                     struct corecomm_disks_set *disks,
                                     struct corecomm_lbas_set *jlbas,
                                     unsigned int wr_sts_bm);

/**
 * Conver lba to jidx (jam)
 * @param handle Corecomm connection object
 * @param disk CDisk handle
 * @param lba The jlba (from @corecomm_alloc_jrnls)
 * @param output Array with enough space for @n_disks jlbas for output
 * @return 0 or -1 and errno set. If 0, fills in @output
 */
value_or_minus_1 corecomm_jam_lba_2_idx(corecomm_handle handle,
                                        cdisk_handle disk,
                                        unsigned long long lba, int *output);

#endif /*CORECOMM_H*/