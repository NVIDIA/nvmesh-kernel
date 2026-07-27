#ifndef NVMEIBC_MSC_MSG_CACHE
#define NVMEIBC_MSC_MSG_CACHE

struct nvmeib_mcs_msg_cache;

struct nvmeib_mcs_msg_cache_entry;

/**
 * nvmeib_mcs_msg_cache_alloc - allocate new message cache
 * @get: call back to get ownership on message data
 * @arg2: call back to release ownership on message data
 *
 * Return: message cache ponter
 */
struct nvmeib_mcs_msg_cache *
nvmeib_mcs_msg_cache_alloc(void (*get)(void *), void (*release)(void *));

/**
 * nvmeib_mcs_msg_cache_insert - insert a new entry to message cache
 * @cache: message cache pointer
 * @uuid: a unique uuid of nvmesh object that sends the message
 *
 * Return: pointer to new entry
 */
struct nvmeib_mcs_msg_cache_entry *
nvmeib_mcs_msg_cache_insert(struct nvmeib_mcs_msg_cache *cache,
			    const char (*uuid)[64]);

/**
 * nvmeib_mcs_msg_cache_get_token - return message unique token
 * @ent: message entry
 * @out_token: output
 */
void nvmeib_mcs_msg_cache_get_token(struct nvmeib_mcs_msg_cache_entry *ent,
				    unsigned char (*out_token)[16]);

/**
 * nvmeib_mcs_msg_cache_set_data - set message body to an allocate entry
 */
void nvmeib_mcs_msg_cache_set_data(struct nvmeib_mcs_msg_cache *cache,
				   struct nvmeib_mcs_msg_cache_entry *ent,
				   void *data);

/**
 * nvmeib_mcs_msg_cache_foreach - iterate over message cache
 * @cache: message cache pointer
 * @cb: callback to be called for each entry in cache.
 *
 * this function returns non zero value for error. Retruning error will stop iteration.
 *
 * @magic: passed to cb
 *
 * Return: 0 for success
 */
int nvmeib_mcs_msg_cache_foreach(struct nvmeib_mcs_msg_cache *cache,
				 int (*cb)(void *data, void *magic),
				 void *magic);

/**
 * nvmeib_mcs_msg_cache_ack - acknowledge and delete entry (if exists) from cache
 * @cache: message cache pointer
 * @token: message unique entry.
 *
 * Return: 0 for success
 */
int nvmeib_mcs_msg_cache_ack(struct nvmeib_mcs_msg_cache *cache,
			     unsigned char token[16]);

/* clean the entire cache */
void nvmeib_mcs_msg_cache_clean(struct nvmeib_mcs_msg_cache *cache);

/* the emount of currently cached messages */
int nvmeib_mcs_msg_cache_size(struct nvmeib_mcs_msg_cache *cache);

/* the number of messages that passed the cache (including deleted) */
int nvmeib_mcs_msg_cache_tot_count(struct nvmeib_mcs_msg_cache *cache);

void nvmeib_mcs_msg_cache_free(struct nvmeib_mcs_msg_cache *cache);

#endif
