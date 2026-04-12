/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_PROCFS_H
#define NVMEIB_PROCFS_H

#include "kr_incs.h"

struct nvmeib_public_procfs_ent;
typedef ssize_t proc_fill_t(void *arg, char *buf, size_t len);
typedef ssize_t proc_chng_t(void *arg, char *buf, size_t len);
typedef int proc_oneshot_show_t(struct seq_file *m, void *arg);

struct nvmeib_public_procfs_ent *nvmeib_public_proc_create(const char *name,
	struct proc_dir_entry *dir, proc_fill_t *fill, proc_chng_t *chng, void *arg);
struct nvmeib_public_procfs_ent *nvmeib_public_proc_create_oneshot_data(const char *name,
	struct proc_dir_entry *dir, proc_oneshot_show_t *show, proc_chng_t *chng, void *arg);
void nvmeib_public_proc_remove(struct nvmeib_public_procfs_ent *p);

/* Backport of proc_create_seq_data */
struct nvmeib_public_procfs_seq_ent;
struct nvmeib_public_procfs_seq_ent *nvmeib_public_proc_create_seq_data(const char *name,
											  struct proc_dir_entry *dir,
											  const struct seq_operations *seq_ops,
											  void *data);
void nvmeib_public_proc_seq_remove(struct nvmeib_public_procfs_seq_ent *seq_ent);

struct nvmeib_proc_hash_tbl_pos {
	union {
		struct {
			unsigned bkt:32;
			unsigned pos_in_bkt:32;
		};
		loff_t pos;
	};
};

/* These macros are used for defining the seq_ops functions for printing a hash-table
 * ------------------------------------------------------------------------------
 * FN_PREFIX - Prefix for function
 * TBL_NAME - Name of hash-table
 * NUM_BKTS - Number of buckets in hash-table (Can be HASH_SIZE(TBL_NAME))
 * HDR_STR - Header string to print.
 * PRINT_NODE_FN - Print the current node - sig: void fn(struct seq_file *m, struct hlist_node *node)
 * GET_NODE_BKT_FN - Get the current bucket from a node ptr - sig: int fn(struct hlist_node *node)
 * PRE_BKT_FN - Function to call before starting next bucket - sig: void fn(int bkt)
 * POST_BKT_FN - Function to call when finishing current bucket - sig: void fn(int bkt)
 */
#define DEFINE_PRINT_HASH_TBL_SEQ_OPS_FNS(FN_PREFIX, TBL_NAME, NUM_BKTS, HDR_STR, PRINT_NODE_FN, GET_NODE_BKT_FN, PRE_BKT_FN, POST_BKT_FN)\
static void *FN_PREFIX##_start(struct seq_file *m, loff_t *pos) \
{\
	struct hlist_node *node; \
	unsigned bkt = 0;\
	loff_t lpos = *pos; \
	void *ret = NULL; \
	if (lpos == 0) {\
		ret = (void*)((long)bkt + 1);\
		goto out;\
	}\
	for (bkt = 0; bkt < NUM_BKTS; bkt++) {\
		PRE_BKT_FN(bkt); \
		hlist_for_each(node, &TBL_NAME[bkt]) {\
			if (lpos-- == 0) {\
				ret = node; \
				goto out;\
			}\
		}\
		POST_BKT_FN(bkt); \
	}\
out:\
	return ret;\
}\
static void *FN_PREFIX##_next(struct seq_file *m, void *p, loff_t *pos) \
{ \
	struct hlist_node *node = p; \
	void *ret = NULL; \
	int bkt = (long)p - 1; \
	if (bkt >= 0 && bkt < NUM_BKTS) { \
		if ((ret = TBL_NAME[bkt].first) == NULL) { \
			goto next_bkt; \
		} \
		goto out; \
	} else if (node->next == NULL) { \
		bkt = GET_NODE_BKT_FN(node); \
		goto next_bkt; \
	} \
	(*pos)++;\
	ret = node->next; \
	goto out; \
next_bkt: \
	POST_BKT_FN(bkt++);\
	if (bkt == NUM_BKTS) \
		goto out; \
	else { \
		ret = (void*)((long)bkt + 1); \
		goto out; \
	} \
out: \
	return ret; \
} \
static int FN_PREFIX##_show(struct seq_file *m, void *p) \
{ \
	struct hlist_node *node = p; \
	int bkt = (long)p - 1; \
	if (bkt >= 0 && bkt < NUM_BKTS) { \
		if (bkt == 0) \
			seq_printf(m, HDR_STR "\n");\
		PRE_BKT_FN(bkt); \
	} else \
		PRINT_NODE_FN(m, node); \
	return 0; \
} \
static void FN_PREFIX##_stop(struct seq_file *m, void *p) \
{ \
	int bkt = (long)p - 1;\
	if (bkt >= 0 && bkt < NVMEIB_ALLOC_HASH_BUCKETS)\
		POST_BKT_FN(bkt);\
	else if (p)\
		POST_BKT_FN(GET_NODE_BKT_FN(p)); \
}

#endif

