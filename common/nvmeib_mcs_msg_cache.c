#include "kr_incs.h"
#include "common_public/nvmeib_uuid_be.h"
#ifndef USER_SPACE
	#include "common_public/nvmeib_public.h"
#endif
#include "nvmeib_mcs_msg_cache.h"

struct nvmeib_mcs_msg_cache {
	struct rb_root tonkes_tree;
	struct rb_root uuid_tree;

	//callback to increase ref count on message data
	void (*get)(void *);

	// callback to release data
	void (*release)(void *);
	int count;
	int tot_count;
};

struct nvmeib_mcs_msg_cache_entry {
	struct rb_node token_node;
	unsigned char token[16];
	struct rb_node uuid_node;
	char unique_uuid[64];
	void *data;
};

static void __cache_init(struct nvmeib_mcs_msg_cache *cache,
			       void (*get)(void *), void (*release)(void *))
{
	NFIN;

	cache->tonkes_tree = RB_ROOT;
	cache->uuid_tree = RB_ROOT;
	cache->count = 0;
	cache->tot_count = 0;
	BUG_ON(release == NULL);
	cache->release = release;
	cache->get = get;
	NFOUT;
}

struct nvmeib_mcs_msg_cache *nvmeib_mcs_msg_cache_alloc(void (*get)(void *), void (*release)(void *))
{
	struct nvmeib_mcs_msg_cache *ret;
	NFIN;

	ret = kzalloc(sizeof(struct nvmeib_mcs_msg_cache), GFP_KERNEL);
	if (ret) {
		__cache_init(ret, get, release);
	}
	NFOUT;

	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_alloc);

void nvmeib_mcs_msg_cache_free(struct nvmeib_mcs_msg_cache *cache)
{
	NFIN;
	kfree(cache);
	NFOUT;

}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_free);

static struct nvmeib_mcs_msg_cache_entry *
search_by_uuid(struct nvmeib_mcs_msg_cache *cache, const char (*uuid)[64])
{
	struct nvmeib_mcs_msg_cache_entry *res;
	struct rb_node *node = cache->uuid_tree.rb_node;
	NFIN;

	while (node) {
		struct nvmeib_mcs_msg_cache_entry *data = container_of(
			node, struct nvmeib_mcs_msg_cache_entry, uuid_node);
		int result;

		result = strncmp(*uuid, data->unique_uuid,
				 sizeof(data->unique_uuid) - 1);

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else {
			res = data;
			goto out;
		}
	}
	res = NULL;
out:
	NFOUT;
	return res;
}

static struct nvmeib_mcs_msg_cache_entry *
search_by_token(struct nvmeib_mcs_msg_cache *cache, unsigned char token[16])
{
	struct nvmeib_mcs_msg_cache_entry *res;
	struct rb_node *node = cache->tonkes_tree.rb_node;
	NFIN;

	while (node) {
		struct nvmeib_mcs_msg_cache_entry *data = container_of(
			node, struct nvmeib_mcs_msg_cache_entry, token_node);
		int result;

		result = memcmp(token, data->token, sizeof(data->token));

		if (result < 0)
			node = node->rb_left;
		else if (result > 0)
			node = node->rb_right;
		else {
			res = data;
			goto out;
		}
	}
	res = NULL;
out:
	NFOUT;
	return res;
}

static int insert_token(struct nvmeib_mcs_msg_cache *cache,
			struct nvmeib_mcs_msg_cache_entry *entry)
{
	int res;
	struct rb_node **new = &(cache->tonkes_tree.rb_node), *parent = NULL;
	NFIN;

	while (*new) {
		struct nvmeib_mcs_msg_cache_entry *this = container_of(
			*new, struct nvmeib_mcs_msg_cache_entry, token_node);

		int result =
			memcmp(entry->token, this->token, sizeof(entry->token));

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else {
			res = -ENONET;
			goto out;
		}
	}

	rb_link_node(&entry->token_node, parent, new);
	rb_insert_color(&entry->token_node, &cache->tonkes_tree);
	++cache->count;
	++cache->tot_count;
	res = 0;

out:
	NFOUT;
	return res;
}

static int insert_uuid(struct nvmeib_mcs_msg_cache *cache,
		       struct nvmeib_mcs_msg_cache_entry *entry)
{
	int res;
	struct rb_node **new = &(cache->uuid_tree.rb_node), *parent = NULL;
	NFIN;

	while (*new) {
		struct nvmeib_mcs_msg_cache_entry *this = container_of(
			*new, struct nvmeib_mcs_msg_cache_entry, uuid_node);

		int result = strncmp(entry->unique_uuid, this->unique_uuid,
				     sizeof(entry->unique_uuid) - 1);

		parent = *new;
		if (result < 0)
			new = &((*new)->rb_left);
		else if (result > 0)
			new = &((*new)->rb_right);
		else {
			res = -ENONET;
			goto out;
		}
	}

	rb_link_node(&entry->uuid_node, parent, new);
	rb_insert_color(&entry->uuid_node, &cache->uuid_tree);
	res = 0;

out:
	NFOUT;
	return res;
}

static void kfree_entry(struct nvmeib_mcs_msg_cache *cache,
			struct nvmeib_mcs_msg_cache_entry *ent)
{
	NFIN;
	cache->release(ent->data);
	kfree(ent);
	NFOUT;
}

static void remove_entry(struct nvmeib_mcs_msg_cache *cache,
			 struct nvmeib_mcs_msg_cache_entry *entry)
{
	NFIN;

	rb_erase(&entry->token_node, &cache->tonkes_tree);
	rb_erase(&entry->uuid_node, &cache->uuid_tree);
	kfree_entry(cache, entry);
	--cache->count;
	NFOUT;
}

int nvmeib_mcs_msg_cache_ack(struct nvmeib_mcs_msg_cache *cache,
			     unsigned char token[16])
{
	struct nvmeib_mcs_msg_cache_entry *entry;
	NFIN;

	if (!token) {
		goto out;
	}
	_NT(nvmeib_mcs_msg_cache_ack, "mcs cache removing item @UUID_4B_LE",
	    token);

	entry = search_by_token(cache, token);
	if (!entry) {
		_NT(nvmeib_mcs_msg_cache_ack_missing,
		    "item acked is no longer exists");
		goto out;
	}

	remove_entry(cache, entry);

out:
	NFOUT;
	return 0;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_ack);

int nvmeib_mcs_msg_cache_size(struct nvmeib_mcs_msg_cache *cache)
{
	return cache->count;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_size);

int nvmeib_mcs_msg_cache_tot_count(struct nvmeib_mcs_msg_cache *cache)
{
	return cache->tot_count;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_tot_count);

static struct nvmeib_mcs_msg_cache_entry *alloc_entry(const char (*uuid)[64])
{
	struct nvmeib_mcs_msg_cache_entry *ent;
	NFIN;

	ent = kzalloc(sizeof(*ent), GFP_KERNEL);
	if (!ent) {
		_NE(mcs_cache_failed_to_alloc,
		    "failed to allocate cache entry");
	} else {
		ent->data = NULL;
		strncpy(ent->unique_uuid, *uuid, sizeof(ent->unique_uuid) - 1);
		ent->unique_uuid[sizeof(ent->unique_uuid) - 1] = '\0';
	}

	NFOUT;
	return ent;
}

struct nvmeib_mcs_msg_cache_entry *
nvmeib_mcs_msg_cache_insert(struct nvmeib_mcs_msg_cache *cache,
			    const char (*uuid)[64])
{
	int stat;
	struct nvmeib_mcs_msg_cache_entry *ent;
	uuid_be *uuid_b;
	bool item_reused = false;
	NFIN;

	if (!uuid || (*uuid)[0] == '\0') {
		ent = ERR_PTR(-ENOMEM);
		goto out;
	}

	_NT(nvmeib_mcs_msg_cache_insert, "mcs cache new item uuid: @STR",
	    *uuid);

	ent = search_by_uuid(cache, uuid);

	if (ent) {
		_NT(nvmeib_mcs_msg_cache_insert_replace,
		    "cache item is replaced uuid: @STR token @UUID_4B_LE",
		    ent->unique_uuid, ent->token);

		rb_erase(&ent->token_node, &cache->tonkes_tree);
		if (ent->data)
			cache->release(ent->data);
		ent->data = NULL;
		item_reused = true;
		--cache->count;
	} else {
		ent = alloc_entry(uuid);
		if (!ent) {
			ent = ERR_PTR(-ENOMEM);
			goto out;
		}
	}

	uuid_b = (uuid_be *)(&ent->token);
	nvmeib_public_uuid_gen(uuid_b);
	stat = insert_token(cache, ent);
	if (stat) {
		if (item_reused) {
			rb_erase(&ent->uuid_node, &cache->uuid_tree);
		}
		kfree_entry(cache, ent);
		ent = ERR_PTR(-ENOENT);
		goto out;
	}

	if (!item_reused)
		stat = insert_uuid(cache, ent);
	_NT(nvmeib_mcs_msg_cache_inserted,
	    "mcs cache item iserted uuid: @STR @UUID_4B_LE", ent->unique_uuid,
	    ent->token);
	BUG_ON(stat);

out:
	NFOUT;
	return ent;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_insert);

void nvmeib_mcs_msg_cache_get_token(struct nvmeib_mcs_msg_cache_entry *ent,
				    unsigned char (*out_token)[16])
{
	NFIN;
	if (ent) {
		memcpy(*out_token, ent->token, sizeof(*out_token));
	}

	NFOUT;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_get_token);

void nvmeib_mcs_msg_cache_set_data(struct nvmeib_mcs_msg_cache *cache,
				   struct nvmeib_mcs_msg_cache_entry *ent,
				   void *data)
{
	NFIN;
	if (ent) {
		cache->get(data);
		ent->data = data;
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_set_data);

int nvmeib_mcs_msg_cache_foreach(struct nvmeib_mcs_msg_cache *cache,
				 int (*cb)(void *data, void *magic),
				 void *magic)
{
	int ret;
	struct rb_node *node;
	NFIN;

	for (node = rb_first(&cache->tonkes_tree); node; node = rb_next(node)) {
		struct nvmeib_mcs_msg_cache_entry *ent = container_of(
			node, struct nvmeib_mcs_msg_cache_entry, token_node);
		ret = cb(ent->data, magic);
		if (ret)
			goto out;
	}
	ret = 0;

out:
	NFOUT;
	return ret;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_foreach);

void nvmeib_mcs_msg_cache_clean(struct nvmeib_mcs_msg_cache *cache)
{
	struct rb_node *node;
	NFIN;

	_NT(nvmeib_mcs_msg_cache_clean, "mcs cache is being cleaned");

	while ((node = rb_first(&cache->tonkes_tree)))
		rb_erase(node, &cache->tonkes_tree);

	while ((node = rb_first(&cache->uuid_tree))) {
		struct nvmeib_mcs_msg_cache_entry *entry = container_of(
			node, struct nvmeib_mcs_msg_cache_entry, uuid_node);
		rb_erase(node, &cache->uuid_tree);
		kfree_entry(cache, entry);
		cache->count--;
	}
	NFOUT;
}
EXPORT_SYMBOL(nvmeib_mcs_msg_cache_clean);
