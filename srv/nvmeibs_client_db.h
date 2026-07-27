#ifndef __NVMEIBS_CLIENT_DB_H__
#define __NVMEIBS_CLIENT_DB_H__

struct nvmeibs_cdb_item {
	struct list_head list;
	struct hlist_node hcid;
	struct list_head dying_link;
};

typedef int (*nvmeibs_cdb_fast_call_fn)(struct nvmeibs_client *, void *);

unsigned long nvmeibs_cdb_lock(void);
void nvmeibs_cdb_unlock(unsigned long flags);

int nvmeibs_cdb_set_no_new_clients(nvmeibs_cdb_fast_call_fn remove_fn, 
				   void *remove_arg,
				   bool will_wait);
void nvmeibs_cdb_set_allow_new_clients(void);
int nvmeibs_cdb_wait_no_clients(bool killable);

bool nvmeibs_cdb_add(struct nvmeibs_client *cl);
void nvmeibs_cdb_del(struct nvmeibs_client *cl, bool move_to_dying);
void nvmeibs_cdb_dying_del(struct nvmeibs_client *cl);
void nvmeibs_cdb_del_locked(struct nvmeibs_client *cl, bool move_to_dying);
struct nvmeibs_client *nvmeibs_cdb_del_by_cid(u64 cid, bool move_to_dying);

int nvmeibs_cdb_count(bool incl_dying);

bool nvmeibs_cdb_exists(struct nvmeibs_client *cl);

/* Attention: fn() will be called while a spinlock is taken */
int nvmeibs_cdb_cid_fast_call(u64 cid, nvmeibs_cdb_fast_call_fn fn, void *arg);
int nvmeibs_cdb_all_fast_call(nvmeibs_cdb_fast_call_fn fn, void *arg);
int nvmeibs_cdb_all_fast_call_locked(nvmeibs_cdb_fast_call_fn fn, void *arg);
int nvmeibs_cdb_dying_call(nvmeibs_cdb_fast_call_fn fn, void *arg);
int nvmeibs_cdb_dying_call_locked(nvmeibs_cdb_fast_call_fn fn, void *arg);

#define nvmeibs_cdb_cid_fast_call_code(cid, cl, code)                          \
	({                                                                         \
		struct nvmeibs_client *cl;                                             \
		unsigned long ____flags;                                               \
		int ____ret = -ENOENT;                                                 \
		____flags = nvmeibs_cdb_lock();                                        \
		cl = nvmeibs_cdb_find_cid_locked(cid);                                 \
		if (cl) {                                                              \
			____ret = 0;                                                       \
			code;                                                              \
		}                                                                      \
		nvmeibs_cdb_unlock(____flags);                                         \
		____ret;                                                               \
	})

struct nvmeibs_client *nvmeibs_cdb_find_cid_locked(u64 cid);
struct nvmeibs_client *nvmeibs_cdb_find_cid(u64 cid);

#endif /* __NVMEIBS_CLIENT_DB_H__ */
