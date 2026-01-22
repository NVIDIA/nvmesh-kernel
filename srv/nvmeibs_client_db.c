/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

/* nvmeibs_client_db.c - Fast access clients database */

#include "nvmeibs_main.h"
#include "nvmeibs_defs.h"
#include "nvmeibs_client.h"

#include "nvmeib_utils.h"
#include "nvmeib_public_procfs.h"
#include "nvmeib.h"
#include "nvmeibs_trace.h"

#include "nvmeibs_client_db.h"

#define NVMEIBS_CDB_BITS 4

struct nvmeibs_cdb {
	struct list_head list;
	struct list_head dying_list;
	DECLARE_HASHTABLE(hcid, NVMEIBS_CDB_BITS);


	/* Protects nvmeibs_client_db, nvmeibs_cdb.count, remove_all_comp,
	 * remove_all_waiters, no_new_clients */
	spinlock_t lock;

	/* holds the number of clients when the server exit is called. */
	int count;
	/* holds the number of clients in the dying list */
	int dying_count;

	bool no_new_clients;
	int remove_all_waiters;
	struct completion remove_all_comp;
};

static struct nvmeibs_cdb nvmeibs_cdb = {
	.hcid = { [0 ... ((1 << (NVMEIBS_CDB_BITS)) - 1)] = HLIST_HEAD_INIT },
	.list = LIST_HEAD_INIT(nvmeibs_cdb.list),
	.dying_list = LIST_HEAD_INIT(nvmeibs_cdb.dying_list),

	.lock = __SPIN_LOCK_UNLOCKED(nvmeibs_cdb.nvmeibs_cdb.lock),
	.count = 0,
	.dying_count = 0,
	.no_new_clients = false,
	.remove_all_waiters = 0,
	.remove_all_comp = COMPLETION_INITIALIZER(nvmeibs_cdb.remove_all_comp),
};

int nvmeibs_cdb_count(bool incl_dying)
{
	_ND(trace_client_db_nvmeibs_cdb_count, "number of clients : @COUNT (dying: @COUNT)", 
	    nvmeibs_cdb.count, nvmeibs_cdb.dying_count);
	return nvmeibs_cdb.count + (incl_dying ? nvmeibs_cdb.dying_count : 0);
}

struct nvmeibs_client *nvmeibs_cdb_find_cid_locked(u64 cid)
{
	struct nvmeibs_client *cl = NULL;

	WARN_ON(!spin_is_locked(&nvmeibs_cdb.lock));

	hash_for_each_possible(nvmeibs_cdb.hcid, cl, cdb.hcid, cid) {
		if (cl->cid == cid)
			break;
	}

	_ND(trace_client_db_nvmeibs_cdb_find_cid_locked, "cid: @CID_LLONG cl: @CL", cid, cl);
	return cl;
}

struct nvmeibs_client *nvmeibs_cdb_find_cid(u64 cid)
{
	struct nvmeibs_client *cl;
	unsigned long flags;

	flags = nvmeibs_cdb_lock();

	cl = nvmeibs_cdb_find_cid_locked(cid);

	_ND(trace_client_db_nvmeibs_cdb_find_cid, "cid: @CID_LLONG cl: @CL", cid, cl);

	nvmeibs_cdb_unlock(flags);

	return cl;
}

unsigned long nvmeibs_cdb_lock(void)
{
	unsigned long flags;

	_ND(trace_client_db_nvmeibs_cdb_lock, "lock");

	spin_lock_irqsave(&nvmeibs_cdb.lock, flags);

	return flags;
}

void nvmeibs_cdb_unlock(unsigned long flags)
{
	WARN_ON(!spin_is_locked(&nvmeibs_cdb.lock));

	spin_unlock_irqrestore(&nvmeibs_cdb.lock, flags);
	_ND(trace_client_db_nvmeibs_cdb_unlock, "unlock");
}

int nvmeibs_cdb_set_no_new_clients(nvmeibs_cdb_fast_call_fn remove_fn, 
				   void *remove_arg,
				   bool will_wait)
{
	unsigned long flags;
	int rv;

	flags = nvmeibs_cdb_lock();

	_NT(trace_client_db_nvmeibs_cdb_set_no_new_clients, 
		"no_new_clients: @NO_NEW_CLIENTS, count @COUNT, "
		"dying_count @COUNT, waiters @COUNT, will_wait @BOOL_YN", 
		nvmeibs_cdb.no_new_clients, nvmeibs_cdb.count, 
		nvmeibs_cdb.dying_count, nvmeibs_cdb.remove_all_waiters, will_wait);

	nvmeibs_cdb.remove_all_waiters += !!will_wait;

	if (nvmeibs_cdb.no_new_clients) {
		/* No new clients set already */
		_NT(trace_2_client_db_nvmeibs_cdb_set_no_new_clients, 
		    "No new clients already set");
		rv = -EALREADY;
		goto unlock;
	}

	nvmeibs_cdb.no_new_clients = true;
	nvmeibs_cdb_all_fast_call_locked(remove_fn, remove_arg);

	rv = nvmeibs_cdb.count;

unlock:
	nvmeibs_cdb_unlock(flags);

	return rv;
}

int nvmeibs_cdb_wait_no_clients(bool killable)
{
	unsigned long flags;
	int rv = 0, wait_cl_count, n_rem_waiters = 0;
	
	flags = nvmeibs_cdb_lock();
	BUG_ON(nvmeibs_cdb.remove_all_waiters == 0);
	wait_cl_count = nvmeibs_cdb.count + nvmeibs_cdb.dying_count;

	nvmeibs_cdb_unlock(flags);
	
	_NT(trace_cdb_wait_no_clients, 
	    "Waiting for @COUNT clients to released (killable @BOOL_YN)", wait_cl_count, killable);

	if (wait_cl_count) {
		int wait_rv = killable ?
			wait_for_completion_killable_timeout(&nvmeibs_cdb.remove_all_comp,
						WAIT_INT_REM_ALL_CL_CNST + wait_cl_count * WAIT_INT_REM_ALL_CL_MULT) :
			wait_for_completion_timeout(&nvmeibs_cdb.remove_all_comp,
						WAIT_REM_ALL_CL_CNST + wait_cl_count * WAIT_REM_ALL_CL_MULT);
		if (wait_rv <= 0) {
			_NT(trace_2_cdb_wait_no_clients, "Wait failed (@RV)", wait_rv);
			if (wait_rv == 0) {
				/* Timed out */
				rv = -ETIMEDOUT;
			} else if (wait_rv == -ERESTARTSYS) {
				BUG_ON(!killable);
				rv = -EINTR;
			} else {
				rv = wait_rv;
			}
		}
	}

	flags = nvmeibs_cdb_lock();
	BUG_ON(!rv && (nvmeibs_cdb.count > 0 || nvmeibs_cdb.dying_count > 0));
	BUG_ON(nvmeibs_cdb.remove_all_waiters == 0);
	nvmeibs_cdb.remove_all_waiters--;
	if (!nvmeibs_cdb.remove_all_waiters)
		nvmeib_reinit_completion(&nvmeibs_cdb.remove_all_comp);
	else 
		n_rem_waiters = nvmeibs_cdb.remove_all_waiters;
	nvmeibs_cdb_unlock(flags);

	_NT(trace_3_cdb_wait_no_clients, 
	    "Finished waiting (remaining waiters @COUNT)", n_rem_waiters);

	if (!rv)
		rv = n_rem_waiters;

	return rv;
}

void nvmeibs_cdb_set_allow_new_clients(void)
{
	unsigned long flags;

	flags = nvmeibs_cdb_lock();

	_NT(trace_client_db_nvmeibs_cdb_set_allow_new_clients, 
	    "allow new clients");
	
	nvmeibs_cdb.no_new_clients = false;
	
	nvmeibs_cdb_unlock(flags);
}

bool nvmeibs_cdb_add(struct nvmeibs_client *cl)
{
	struct nvmeibs_cdb_item *item = &cl->cdb;
	bool added = false;
	unsigned long flags;

	flags = nvmeibs_cdb_lock();

	if (nvmeibs_cdb_find_cid_locked(cl->cid)) {
		WARN(true, "Client with cid %#x already exists\n", cl->cid);
		goto out;
	}

	_NT(trace_client_db_nvmeibs_cdb_add, "add @CL. no_new_clients: @NO_NEW_CLIENTS", cl, nvmeibs_cdb.no_new_clients);

	if (nvmeibs_cdb.no_new_clients)
		goto out;

	list_add(&item->list, &nvmeibs_cdb.list);
	hash_add(nvmeibs_cdb.hcid, &item->hcid, (u64)cl->cid);
	++nvmeibs_cdb.count;
	added = true;

out:
	nvmeibs_cdb_unlock(flags);

	_ND(trace_1_client_db_nvmeibs_cdb_add, "added: @ADDED. cl: @CL", added, cl);
	return added;
}

void nvmeibs_cdb_del_locked(struct nvmeibs_client *cl, bool move_to_dying)
{
	struct nvmeibs_cdb_item *item = &cl->cdb;

	WARN_ON(!spin_is_locked(&nvmeibs_cdb.lock));

	_ND(trace_client_db_nvmeibs_cdb_del_locked, "del: @CL", cl);

	if (unlikely(!nvmeibs_cdb_exists(cl))) {
		_NT(trace_1_client_db_nvmeibs_cdb_del_locked, "Trying to del an already deleted client from db");
		return;
	}

	WARN_ON(list_empty(&item->list));
	WARN_ON(hlist_unhashed(&item->hcid));

	list_del_init(&item->list);
	hlist_del_init(&item->hcid);

	BUG_ON(nvmeibs_cdb.count == 0);
	--nvmeibs_cdb.count;

	if (move_to_dying) {
		/* add the client to a temp dying list right after remove it from main list */
		list_add_tail(&item->dying_link, &nvmeibs_cdb.dying_list);
		++nvmeibs_cdb.dying_count;
	} else if (nvmeibs_cdb.count == 0 && nvmeibs_cdb.dying_count == 0 && nvmeibs_cdb.remove_all_waiters > 0) {
		_NT(trace_cdb_del_locked_remove_all, "All clients removed, signalling completion");
		complete(&nvmeibs_cdb.remove_all_comp);
	}

	_NT(trace_2_client_db_nvmeibs_cdb_del_locked, "Remaining number of clients : @COUNT", nvmeibs_cdb.count);
}

void nvmeibs_cdb_del(struct nvmeibs_client *cl, bool move_to_dying)
{
	unsigned long flags;

	flags = nvmeibs_cdb_lock();

	_ND(trace_client_db_nvmeibs_cdb_del, "del: @CL", cl);

	nvmeibs_cdb_del_locked(cl, move_to_dying);

	nvmeibs_cdb_unlock(flags);
}

void nvmeibs_cdb_dying_del(struct nvmeibs_client *cl)
{
	unsigned long flags;
	struct nvmeibs_cdb_item *item = &cl->cdb;

	flags = nvmeibs_cdb_lock();

	BUG_ON(!hlist_unhashed(&item->hcid));
	BUG_ON(!list_empty(&item->list));
	BUG_ON(list_empty(&item->dying_link));

	list_del_init(&item->dying_link);
	BUG_ON(nvmeibs_cdb.dying_count == 0);
	nvmeibs_cdb.dying_count--;

	_NT(trace_nvmeibs_cdb_dying_del, "del from dying list: @CL, "
		"remaining clients @COUNT, rem dying @COUNT", cl, nvmeibs_cdb.count, nvmeibs_cdb.dying_count);

	if (nvmeibs_cdb.count == 0 && nvmeibs_cdb.dying_count == 0 && nvmeibs_cdb.remove_all_waiters > 0) {
		_NT(trace_cdb_dying_del_remove_all, "All clients removed, signalling completion");
		complete(&nvmeibs_cdb.remove_all_comp);
	}

	nvmeibs_cdb_unlock(flags);
}

struct nvmeibs_client *nvmeibs_cdb_del_by_cid(u64 cid, bool move_to_dying)
{
	unsigned long flags;
	struct nvmeibs_client *cl;

	flags = nvmeibs_cdb_lock();

	cl = nvmeibs_cdb_find_cid_locked(cid);

	if (unlikely(!cl)) {
		_NW(warn_client_db_nvmeibs_cdb_del_by_cid, "Couldn't find client (cid: @CID_LLONG) to del", cid);
		goto out;
	}

	_ND(trace_client_db_nvmeibs_cdb_del_by_cid, "del: @CL", cl);

	nvmeibs_cdb_del_locked(cl, move_to_dying);

out:
	nvmeibs_cdb_unlock(flags);

	return cl;
}

bool nvmeibs_cdb_exists(struct nvmeibs_client *cl)
{
	_ND(trace_client_db_nvmeibs_cdb_exists, "exists: @LIST_EMPTY", !list_empty(&cl->cdb.list));

	return !list_empty(&cl->cdb.list);
}

int nvmeibs_cdb_cid_fast_call(u64 cid, nvmeibs_cdb_fast_call_fn fn, void *arg)
{
	struct nvmeibs_client *cl;
	unsigned long flags;
	int ret = -ENOENT;

	flags = nvmeibs_cdb_lock();

	_ND(trace_client_db_nvmeibs_cdb_cid_fast_call, "cid: @CID_LLONG call: @FN_FUNC", cid, fn);

	cl = nvmeibs_cdb_find_cid_locked(cid);
	if (cl)
		ret = fn(cl, arg);

	nvmeibs_cdb_unlock(flags);

	return ret;
}

int nvmeibs_cdb_all_fast_call_locked(nvmeibs_cdb_fast_call_fn fn, void *arg)
{
	struct nvmeibs_client *cl;
	int ret = 0;

	WARN_ON(!spin_is_locked(&nvmeibs_cdb.lock));

	list_for_each_entry(cl, &nvmeibs_cdb.list, cdb.list) {
		_ND(nvmeibs_cdb_all_fast_call_locked_d1, "call: @FN", fn);
		ret = fn(cl, arg);
		if (ret)
			break;
	}

	return ret;
}

int nvmeibs_cdb_all_fast_call(nvmeibs_cdb_fast_call_fn fn, void *arg)
{
	unsigned long flags;
	int ret;

	flags = nvmeibs_cdb_lock();

	_ND(trace_client_db_nvmeibs_cdb_all_fast_call, "call");
	ret = nvmeibs_cdb_all_fast_call_locked(fn, arg);

	nvmeibs_cdb_unlock(flags);

	return ret;
}

static int _cdb_dying_call(nvmeibs_cdb_fast_call_fn fn, void *arg)
{
	struct nvmeibs_client *cl;
	int ret = 0;

	_ND(nvmeibs_cdb_dying_call_d1, "call: @FN", fn);
	WARN_ON(!spin_is_locked(&nvmeibs_cdb.lock));
	
	list_for_each_entry(cl, &nvmeibs_cdb.dying_list, cdb.dying_link) {
		ret = fn(cl, arg);
		if (ret) {
			break;
		}
	}

	return ret;
}

int nvmeibs_cdb_dying_call(nvmeibs_cdb_fast_call_fn fn, void *arg) {
	int ret;
	unsigned long flags;
	
	flags = nvmeibs_cdb_lock();
	ret = _cdb_dying_call(fn, arg);
	nvmeibs_cdb_unlock(flags);

	return ret;
}

int nvmeibs_cdb_dying_call_locked(nvmeibs_cdb_fast_call_fn fn, void *arg) {
	return _cdb_dying_call(fn, arg);
}
