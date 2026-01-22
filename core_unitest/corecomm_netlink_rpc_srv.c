/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "corecomm_netlink_rpc_srv.h"

#include <linux/skbuff.h>
#include <net/sock.h>

/***** HELPER FUNCTIONS ****/

int nl_response_init(struct nl_response *__self, size_t size, gfp_t alloc_flags,
                     int msg_type) {
	if (!(__self->skb_out = nlmsg_new(size, alloc_flags))) return -ENOMEM;
	if (!(__self->nlh = nlmsg_put(__self->skb_out, 0, 0, msg_type, size, 0))) {
		nlmsg_free(__self->skb_out);
		__self->skb_out = NULL;
		return -ENOMEM;
	}
	/* unicast, can be overriden but no use case for now */
	NETLINK_CB(__self->skb_out).dst_group = 0;
	__self->data                          = nlmsg_data(__self->nlh);
	__self->sent                          = false;
	return 0;
}

void nl_response_free(struct nl_response *__self) {
	if (!__self->sent && __self->skb_out) {
		nlmsg_free(__self->skb_out);
		__self->skb_out = NULL;
	}
}

int nl_response_send(struct nl_response *__self, struct sock *sk, pid_t pid) {
	int rv;
	if ((rv = nlmsg_unicast(sk, __self->skb_out, pid))) { return rv; }
	__self->sent = true;
	return 0;
}