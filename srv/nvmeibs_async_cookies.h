#ifndef NVMEIBS_ASYNC_COOKIES_H
#define NVMEIBS_ASYNC_COOKIES_H

#include "kr_incs.h"
#include "nvmeib_utils.h"
#include "nvmeib_public.h"

struct gens_cmd_ctx;
struct nvmeibc_disk_gen_cmd;
struct nvmeibs_nr_channel;
struct nvmeib_iu;

#define NVMEIBS_ASYNC_COOKIE_DEFAULT_TIMEOUT (2 * HZ)
#define NVMEIBS_ASYNC_LOCAL_CHANNEL 0

typedef u64 nvmeibs_async_cookie_channel_t;

union nvmeibs_async_cookie_ctx;

typedef void (*nvmeibs_async_cookie_completion_t)(
    union nvmeibs_async_cookie_ctx *, int);

/* Defines a cookie, uniquely defines an async operation that await completion
 * from an external actor (toma / disk / etc) */
union nvmeibs_async_cookie {
	struct { /* Uniq identifier - consist of client id and a running number */
		u32 cid;
		u32 stamp;
	} uniq;
	u64 raw;
} __attribute__((packed));

/* Defines the data structure of the global (per server) async cookies store */
struct nvmeibs_async_cookie_store {
	struct list_head
	    list; /* of type struct nvmeibs_async_cookie_channel_data */
	spinlock_t guard;
};

/* Defines the data structure of per channel async cookies store */
struct nvmeibs_async_cookie_channel_data {
	struct list_head link;               /* Link to the global store */
	nvmeibs_async_cookie_channel_t chid; /* Primary key */
	struct nvmeibs_async_cookie_store
	    *store;            /* Back pointer to the owner store */
	struct list_head list; /* of type nvmeibs_async_cookie_data */
	spinlock_t guard;
	struct nvmeib_ref ref; /* cookies counter, used to stop channel */
};

union nvmeibs_async_cookie_ctx {
	struct { /* Data needed for local server */
		union nvmeib_gen_cmd_rsp *rsp;
		nvmeib_local_gen_cmd_cb_t *cb; /* local callback */
	} local;
	struct { /* Data needed for nordda channel */
		struct gens_cmd_ctx *cmd_ctx;
	} nordda;
};

/* Defines the data structure used to add a new cookie */
struct nvmeibs_async_cookie_params {
	u32 cid;       /* Client ID */
	bool is_local; /* Is local channel */
	struct nvmeibs_async_cookie_channel_data
	    *ch; /* Cookie channel (if known, else will be resolved by cid) */
	nvmeibs_async_cookie_completion_t cookie_completion_cb;
	union nvmeibs_async_cookie_ctx ctx;
};

/* Defines a cookie with its related data, by which a cookie can be completed
 * (local server / nordda channel etc.) */
struct nvmeibs_async_cookie_data {
	struct list_head link;             /* Link to the channel store */
	union nvmeibs_async_cookie cookie; /* Primary key */
	struct nvmeibs_async_cookie_channel_data
	    *ch;             /* Channel this cookie belong to */
	u64 expiration_time; /* Jiffies when cookie expires */
	nvmeibs_async_cookie_completion_t cookie_completion_cb;
	union nvmeibs_async_cookie_ctx ctx;
};

#define NVMEIBS_INIT_ASYNC_COOKIE_PARAMS(cid_, is_local_, ch_, ...)            \
	((struct nvmeibs_async_cookie_params){                                     \
	    .cid = cid_,                                                           \
	    .is_local = is_local_,                                                 \
	    .ch = ch_,                                                             \
	    .cookie_completion_cb = is_local_                                      \
	                                ? nvmeibs_async_cookie_local_completion    \
	                                : nvmeibs_async_cookie_nordda_completion,  \
	    ##__VA_ARGS__})

/******* Operations on async cookie store *******/

/** Initialization, probably should be called from module __init
 * @param store Cookie store
 */
void nvmeibs_async_cookie_store_init(struct nvmeibs_async_cookie_store *store);

/** Is store empty
 * @param store Cookie store
 */
bool nvmeibs_async_cookie_store_is_empty(
    struct nvmeibs_async_cookie_store *store);

/** Return channel data associated with the
 * given channel or NULL
 * @param store Cookie store
 * @param chid Channel ID
 * @return Cookie data or NULL if not found
 */
struct nvmeibs_async_cookie_channel_data *
nvmeibs_async_cookie_store_get_ch(struct nvmeibs_async_cookie_store *store,
                                  nvmeibs_async_cookie_channel_t chid);

/** Add empty channel data for given ID
 * @param store Cookie store
 * @param chid Channel ID
 * @return Cookie data or NULL if error
 */
struct nvmeibs_async_cookie_channel_data *
nvmeibs_async_cookie_store_add_ch(struct nvmeibs_async_cookie_store *store,
                                  nvmeibs_async_cookie_channel_t chid,
                                  gfp_t gfp);

/** Wait for all cookies to complete on given
 * chanel.
 * @param ch Channel ID
 * @return Cookie data or NULL if error
 */
void nvmeibs_async_cookie_store_wait_ch(
    struct nvmeibs_async_cookie_channel_data *ch);

/** Same as above, uses chid when it is unsafe to use a pointer
 */
void nvmeibs_async_cookie_store_wait_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid);

/** Remove given channel from the store
 * @param ch Channel ID
 * @return Cookie data or NULL if error
 */
void nvmeibs_async_cookie_store_remove_ch(
    struct nvmeibs_async_cookie_channel_data *ch);

/** Alternative, remove channel by store and chid
 * @return Cookie data or NULL if error
 * @param store Cookie store
 * @param chid Channel ID
 */
void nvmeibs_async_cookie_store_remove_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid);

/** Add cookie to store on given channel.
 * Will create channel if not exists.
 * @param store Cookie store
 * @param data Cookie data to add
 * @param gfp GFP flags to use for allocation
 * @param raw Raw cookie value will be written to this var
 * @note Will create a copy of data
 * @return Error code or 0
 */
int nvmeibs_async_cookie_store_add_cookie(
    struct nvmeibs_async_cookie_params *cookie_params, gfp_t gfp, u64 *raw);

/** Get cookie data and *remove* it from
 * store.
 * @param store Cookie store
 * @param cookie Cookie
 * @return Cookie data or NULL
 */
struct nvmeibs_async_cookie_data *
nvmeibs_async_cookie_store_pull_cookie(struct nvmeibs_async_cookie_store *store,
                                       u64 cookie);

/** Complete cookie data from the store.
 * @param data Cookie data to remove
 * @param rv RV of the operation cookie
 * represents
 * @note Will release and poison cookie data!!!
 */
void nvmeibs_async_cookie_store_put_cookie(
    struct nvmeibs_async_cookie_data *data, int rv);

/** Iterate over store and put all cookies
 * @param store Cookie store
 * @param only_expired Whether or not shall use only expired cookies
 */
void nvmeibs_async_cookie_store_bail_all_store(
    struct nvmeibs_async_cookie_store *store);

/** Iterate over channel and put all cookies
 * @param ch Cookie channel
 */
void nvmeibs_async_cookie_store_bail_all_ch(
    struct nvmeibs_async_cookie_channel_data *ch);

/** Same as above, uses chid when it is unsafe to use a pointer
 */
void nvmeibs_async_cookie_store_bail_all_chid(
    struct nvmeibs_async_cookie_store *store,
    nvmeibs_async_cookie_channel_t chid);

/**
 * Iterate over channel and put all cookies that are expired
 * @param ch Cookie channel
 */
void nvmeibs_async_cookie_store_bail_expired_ch(
    struct nvmeibs_async_cookie_channel_data *ch);

/** Iterate over store and put all cookies that are expired.
 * @param store Cookie store
 */
void nvmeibs_async_cookie_store_bail_expired_store(
    struct nvmeibs_async_cookie_store *store);

/* Below there are asyn cookie completion
 * callbacks to be use with
 * differen channels */

void nvmeibs_async_cookie_local_completion(union nvmeibs_async_cookie_ctx *data,
                                           int rv);
void nvmeibs_async_cookie_nordda_completion(
    union nvmeibs_async_cookie_ctx *data, int rv);

#endif /*NVMEIBS_ASYNC_COOKIES_H*/
