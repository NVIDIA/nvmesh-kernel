/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef KR_INCS_LLIST_COMPAT_H
#define KR_INCS_LLIST_COMPAT_H

#ifdef __KERNEL__
#include <linux/llist.h>

/* WARNING: the following functions:
 * __llist_add_batch, __llist_add, __llist_del_all
 * are NOT thread-safe!
 * User must ensure that the list is not accessed from multiple threads when using these functions.
 */

#if !KS_HAS___LLIST_ADD_BATCH
inline static bool __llist_add_batch(struct llist_node *new_first,
				     struct llist_node *new_last,
				     struct llist_head *head)
{
	new_last->next = head->first;
	head->first = new_first;
	return new_last->next == NULL;
}
#endif

#if !KS_HAS___LLIST_ADD
inline static bool __llist_add(struct llist_node *new,
			       struct llist_head *head)
{
	return __llist_add_batch(new, new, head);
}
#endif

#if !KS_HAS___LLIST_DEL_ALL
inline static struct llist_node *__llist_del_all(struct llist_head *head)
{
	struct llist_node *first = head->first;
	head->first = NULL;
	return first;
}
#endif

#endif /* __KERNEL__ */

#endif /* KR_INCS_LLIST_COMPAT_H */