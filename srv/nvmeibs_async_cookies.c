/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "nvmeib.h"
#include "nvmeibs_async_cookies.h"
#include "nvmeib_public.h"
#include "nvmeib_version_shared.h"
#include "nvmeibs_client.h"
#include "nvmeibs_msgs_shared.h"

atomic_t nvmeibs_async_cookie_stamp = ATOMIC_INIT(0);

void nvmeibs_async_cookie_store_init(struct nvmeibs_async_cookie_store *store) {
	NFIN;
	spin_lock_init(&store->guard);
	INIT_LIST_HEAD(&store->list);
	NFOUT;
}

bool nvmeibs_async_cookie_store_is_empty(
    struct nvmeibs_async_cookie_store *store) {
	NFIN;
	NFOUT;
	return list_empty(&store->list);
}

static struct nvmeibs_async_cookie_channel_data *
__async_cookie_store_get_ch_unsafe(struct nvmeibs_async_cookie_store *store,
                                   nvmeibs_async_cookie_channel_t chid) {
	struct nvmeibs_async_cookie_channel_data *ch;

	NFIN;

	list_for_each_entry(ch, &store->list, link) {
		if (ch->chid == chid) goto out;
	}
	ch = NULL; /* Not found */
out:
	NFOUT;

	return ch;
}

struct nvmeibs_async_cookie_channel_data *
nvmeibs_async_cookie_store_get_ch(struct nvmeibs_async_cookie_store *store,
                                  nvmeibs_async_cookie_channel_t chid) {
	unsigned long flags;
	struct nvmeibs_async_cookie_channel_data *ch;

	NFIN;

	spin_lock_irqsave(&store->guard, flags);
	ch = __async_cookie_store_get_ch_unsafe(store, chid);
	spin_unlock_irqrestore(&store->guard, flags);

	NFOUT;

	return ch;
}

/* alloc & add ch to its cl's store for future linking of cookies to it.
   ch can be nrch or ldisk i.e. local-c-disk abnd then ch is added from
   nrch-connect or ldisk_register_fc, respectively */
struct nvmeibs_async_cookie_channel_data *
nvmeibs_async_cookie_store_add_ch(struct nvmeibs_async_cookie_store *store,
                                  nvmeibs_async_cookie_channel_t chid,
                                  gfp_t gfp) {
	unsigned long flags;
	struct nvmeibs_async_cookie_channel_data *ch;

	NFIN;

	ch = kzalloc(sizeof(struct nvmeibs_async_cookie_channel_data), gfp);
	if (!ch) goto out;

	ch->chid = chid;
	ch->store = store;
	INIT_LIST_HEAD(&ch->list);
	nvmeib_ref_init(&ch->ref);

	spin_lock_irqsave(&store->guard, flags);
	list_add_tail(&ch->link, &store->list);
	spin_unlock_irqrestore(&store->guard, flags);
out:
	NFOUT;

	return ch;
}

void nvmeibs_async_cookie_store_wait_ch(
    struct nvmeibs_async_cookie_channel_data *ch) {
	NFIN;
	if (ch) {
		nvmeib_ref_release_start(&ch->ref);
		nvmeib_ref_release_wait(&ch->ref);
	}
	NFOUT;
}

void nvmeibs_async_cookie_store_wait_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid) {
	unsigned long flags;
	struct nvmeibs_async_cookie_channel_data *ch;
	NFIN;
	spin_lock_irqsave(&store->guard, flags);
	ch = __async_cookie_store_get_ch_unsafe(store, chid);
	if (ch) {
		nvmeib_ref_release_start(&ch->ref);
		nvmeib_ref_release_wait(&ch->ref);
	}
	spin_unlock_irqrestore(&store->guard, flags);
	NFOUT;
}

void nvmeibs_async_cookie_store_remove_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid) {
	unsigned long flags;
	struct nvmeibs_async_cookie_channel_data *ch;

	NFIN;

	_NT(t0_nvmeibs_async_cookie_store_remove_chid,
		"remove-ch chid=@LLU from store=@PTR", chid, store);

	spin_lock_irqsave(&store->guard, flags);
	ch = __async_cookie_store_get_ch_unsafe(store, chid);
	if (ch) {
		_NT(t1_nvmeibs_async_cookie_store_remove_chid,
			"remove-ch @PTR", ch);
		BUG_ON(atomic_read(&ch->ref.cnt));
		list_del(&ch->link);
		memset(ch, 0xfa, sizeof(*ch)); /* DEBUG */
		kfree(ch);
	}
	spin_unlock_irqrestore(&store->guard, flags);

	NFOUT;
}

void nvmeibs_async_cookie_store_remove_ch(
    struct nvmeibs_async_cookie_channel_data *ch) {
	NFIN;
	if (ch) { /* ch == NULL is valid input, don't crash */
		unsigned long flags;

		_NT(t0_nvmeibs_async_cookie_store_remove_ch,
			"remove-ch @PTR", ch);
		BUG_ON(atomic_read(&ch->ref.cnt));
		spin_lock_irqsave(&ch->store->guard, flags);
		list_del(&ch->link);
		spin_unlock_irqrestore(&ch->store->guard, flags);

		memset(ch, 0xfa, sizeof(*ch)); /* DEBUG */
		kfree(ch);
	}
	NFOUT;
}

#define __MAX_UNIQ_ITERATIONS 0xffffff

static int __generate_cookie_uniq_unsafe(struct nvmeibs_async_cookie_data *data) {

	unsigned n_iter = 0;
	int rv = -EINVAL;
	NFIN;
	while (1) {
		struct nvmeibs_async_cookie_data *other;
		bool is_ok = true;
		if (n_iter++ >= __MAX_UNIQ_ITERATIONS) {
			WARN_ON_ONCE(1);
			break;
		}
		data->cookie.uniq.stamp =
		    atomic_inc_return(&nvmeibs_async_cookie_stamp);
		list_for_each_entry(other, &data->ch->list, link) {
			if (data->cookie.uniq.stamp == other->cookie.uniq.stamp) {
				is_ok = false;
				break;
			}
		}
		if (is_ok) {
			rv = 0;
			break;
		}
	}
	NFOUT;
	return rv;
}

/* Helper function, assumes cookie params are already resolved */
static int __async_cookie_store_add_cookie(struct nvmeibs_async_cookie_data *data,
                                    gfp_t gfp, u64 *raw) {
	int rv;
	NFIN;

	if (!data->ch) {
		rv = NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED;
		goto out;
	}

	if (nvmeib_ref_get(&data->ch->ref)) {
		unsigned long flags;
		/* Now actually add the cookie */
		spin_lock_irqsave(&data->ch->guard, flags);
		if (!(rv = __generate_cookie_uniq_unsafe(data))) {
			*raw = data->cookie.raw;
			list_add_tail(&data->link, &data->ch->list);
		}
		spin_unlock_irqrestore(&data->ch->guard, flags);
	} else { /* Stopping */
		rv = NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED;
	}
out:
	NFOUT;
	return rv;
}

int nvmeibs_async_cookie_store_add_cookie(
    struct nvmeibs_async_cookie_params *cookie_params, gfp_t gfp, u64 *raw) {
	struct nvmeibs_async_cookie_data *data;
	int rv;
	NFIN;
	data = kmalloc(sizeof(struct nvmeibs_async_cookie_data), gfp);
	if (!data) {
		rv = -ENOMEM;
		goto out;
	}

	data->cookie.uniq.cid = cookie_params->cid;
	data->cookie_completion_cb = cookie_params->cookie_completion_cb;
	data->ctx = cookie_params->ctx;
	data->expiration_time = jiffies + NVMEIBS_ASYNC_COOKIE_DEFAULT_TIMEOUT;
	data->ch = cookie_params->ch;

	if (!data->ch) { /* Channel is unknown, try to resolve it, under lock */
		rv = NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED;
		nvmeibs_cdb_cid_fast_call_code(data->cookie.uniq.cid, cl, {
			data->ch = nvmeibs_async_cookie_store_get_ch(
			    &cl->cookie_store, NVMEIBS_ASYNC_LOCAL_CHANNEL);
			rv = __async_cookie_store_add_cookie(data, gfp, raw);
		});
	} else /* Channel is known, just use it */
		rv = __async_cookie_store_add_cookie(data, gfp, raw);
out:
	if (rv) kfree(data);
	NFOUT;
	return rv;
}

static struct nvmeibs_async_cookie_data *__async_cookie_store_pull_cookie_ch(
    struct nvmeibs_async_cookie_channel_data *ch, u64 cookie) {
	struct nvmeibs_async_cookie_data *data;
	unsigned long flags;
	NFIN;
	spin_lock_irqsave(&ch->guard, flags);
	list_for_each_entry(data, &ch->list, link) {
		if (data->cookie.raw == cookie) {
			list_del_init(&data->link);
			_NT(cv6sjh, "del cookie @COOKIE", cookie);
			/* cookie has refcnt on data->ch */
			goto out;
		}
	}
	data = NULL;
out:
	spin_unlock_irqrestore(&ch->guard, flags);
	NFOUT;
	return data;
}

struct nvmeibs_async_cookie_data *
nvmeibs_async_cookie_store_pull_cookie(struct nvmeibs_async_cookie_store *store,
                                       u64 cookie) {
	struct nvmeibs_async_cookie_channel_data *ch;
	unsigned long flags;
	struct nvmeibs_async_cookie_data *data;
	NFIN;
	spin_lock_irqsave(&store->guard, flags);
	list_for_each_entry(ch, &store->list, link) {
		data = __async_cookie_store_pull_cookie_ch(ch, cookie);
		if (data) goto out;
	}
	data = NULL;
out:
	spin_unlock_irqrestore(&store->guard, flags);
	NFOUT;
	return data;
}

void nvmeibs_async_cookie_store_put_cookie(
    struct nvmeibs_async_cookie_data *data, int rv) {
	NFIN;
	if (data) {
		BUG_ON(!list_empty(&data->link));
		data->cookie_completion_cb(&data->ctx, rv);
		nvmeib_ref_put(&data->ch->ref);
		memset(data, 0xfa, sizeof(*data)); /* DEBUG */
		kfree(data);
	}
	NFOUT;
}

/** Helper function, pull (append) all cookies from the channel */
static void __async_cookie_store_pull_all_release(
    struct nvmeibs_async_cookie_channel_data *ch, struct list_head *pulled) {
	unsigned long flags;
	NFIN;

	spin_lock_irqsave(&ch->guard, flags);
	nvmeib_ref_release_start(&ch->ref);
	list_splice_tail_init(&ch->list, pulled);
	spin_unlock_irqrestore(&ch->guard, flags);

	NFOUT;
}

/** Helper function, pull (append) expired cookies from the channel */
static void
__async_cookie_store_pull_expired(struct nvmeibs_async_cookie_channel_data *ch,
                                  struct list_head *pulled) {

	struct nvmeibs_async_cookie_data *data, *tmp;
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&ch->guard, flags);
	list_for_each_entry_safe(data, tmp, &ch->list, link) {
		if (jiffies > data->expiration_time) {
			list_del_init(&data->link);
			list_add_tail(&data->link, pulled);
		}
	}
	spin_unlock_irqrestore(&ch->guard, flags);

	NFOUT;
}

/** Helper function, bail full store using puller callback */
static void __async_cookie_store_bail_store(
    struct nvmeibs_async_cookie_store *store,
    void (*puller)(struct nvmeibs_async_cookie_channel_data *,
                   struct list_head *),
    int rv) {
	unsigned long flags;
	struct nvmeibs_async_cookie_channel_data *ch;
	struct list_head pulled;
	struct nvmeibs_async_cookie_data *data, *tmp;

	NFIN;

	INIT_LIST_HEAD(&pulled);

	spin_lock_irqsave(&store->guard, flags);
	list_for_each_entry(ch, &store->list, link) { puller(ch, &pulled); }
	spin_unlock_irqrestore(&store->guard, flags);

	list_for_each_entry_safe(data, tmp, &pulled, link) {
		list_del_init(&data->link);
		nvmeibs_async_cookie_store_put_cookie(data, rv);
	}

	NFOUT;
}

/** Helper function, bail a single channel using puller callback */
static void __async_cookie_store_bail_ch(
    struct nvmeibs_async_cookie_channel_data *ch,
    void (*puller)(struct nvmeibs_async_cookie_channel_data *,
                   struct list_head *),
    int rv) {
	struct list_head pulled;
	struct nvmeibs_async_cookie_data *data, *tmp;

	NFIN;

	INIT_LIST_HEAD(&pulled);

	/* __async_cookie_store_pull_all_release
	   __async_cookie_store_pull_expired
	 */
	puller(ch, &pulled); /* Lock is inside */

	list_for_each_entry_safe(data, tmp, &pulled, link) {
		list_del_init(&data->link);
		nvmeibs_async_cookie_store_put_cookie(data, rv);
	}

	NFOUT;
}

static void __async_cookie_store_bail_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid,
    void (*puller)(struct nvmeibs_async_cookie_channel_data *,
                   struct list_head *),
    int rv) {
	struct list_head pulled;
	struct nvmeibs_async_cookie_channel_data *ch;
	struct nvmeibs_async_cookie_data *data, *tmp;
	unsigned long flags;

	NFIN;

	spin_lock_irqsave(&store->guard, flags);
	/* The from this point until unlock, a channel cannot be removed */
	INIT_LIST_HEAD(&pulled);
	ch = __async_cookie_store_get_ch_unsafe(store, chid);
	if (ch) {
		puller(ch, &pulled); /* Lock is inside */
	}
	/* End of the critical section */
	spin_unlock_irqrestore(&store->guard, flags);

	list_for_each_entry_safe(data, tmp, &pulled, link) {
		list_del_init(&data->link);
		nvmeibs_async_cookie_store_put_cookie(data, rv);
	}

	NFOUT;
}

void nvmeibs_async_cookie_store_bail_all_store(
    struct nvmeibs_async_cookie_store *store) {
	NFIN;

	__async_cookie_store_bail_store(store, __async_cookie_store_pull_all_release,
	                                NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED);

	NFOUT;
}

void nvmeibs_async_cookie_store_bail_all_ch(
    struct nvmeibs_async_cookie_channel_data *ch) {

	NFIN;

	__async_cookie_store_bail_ch(ch, __async_cookie_store_pull_all_release,
	                             NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED);

	NFOUT;
}

void nvmeibs_async_cookie_store_bail_all_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid) {

	NFIN;

	__async_cookie_store_bail_chid(store, chid,
	                               __async_cookie_store_pull_all_release,
	                               NVMEIBS_IO_RSP_ERR_ASYNC_OP_BAILED);

	NFOUT;
}

void nvmeibs_async_cookie_store_bail_expired_store(
    struct nvmeibs_async_cookie_store *store) {
	NFIN;

	__async_cookie_store_bail_store(store, __async_cookie_store_pull_expired,
	                                NVMEIBS_IO_RSP_ERR_ASYNC_OP_TIMEDOUT);

	NFOUT;
}

void nvmeibs_async_cookie_store_bail_expired_ch(
    struct nvmeibs_async_cookie_channel_data *ch) {
	NFIN;

	__async_cookie_store_bail_ch(ch, __async_cookie_store_pull_expired,
	                             NVMEIBS_IO_RSP_ERR_ASYNC_OP_TIMEDOUT);

	NFOUT;
}
