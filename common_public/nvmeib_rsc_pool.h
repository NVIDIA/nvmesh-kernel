/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#ifndef NVMEIB_RSC_POOL
#define NVMEIB_RSC_POOL

#include "kr_incs.h"

struct nvmeib_rsc_pool;
struct nvmeib_rsc_pool_rsc;
struct nvmeib_rsc_pool_cb;

/**
 * This is a generic library for dynamic resource management.
 * Resource is an abstract entity, tha has a construct, destructor and data;
 * Resource pool contains a collection of X resources;
 */

/**
 * Resource, a single element of the pool
 */
struct nvmeib_rsc_pool_rsc {
	struct list_head link;
};

/**
 * An entity that requires resources. For example, in case of the binary tracer,
 * it is a capuch.
 * @note Greed is signed, for calculations simplicity. Can't really be < 0.
 */
struct nvmeib_rsc_pool_actor {
	/* Configuration shall be set before registering the actor */
	struct nvmeib_rsc_pool_actor_conf {
		size_t priority; /* Actor priority */
		size_t min_rsc; /* Desired min rsc per actor */
		ssize_t min_greed; /* Minimum greed */
		ssize_t max_greed; /* Maximum greed */
	} conf;
	struct nvmeib_rsc_pool *pool; /* For simplicity, actors points back */
	struct list_head link;
	struct list_head rsc_list;
	ssize_t greed;
	size_t nbufs;

	/* These two are actually not needed here, these are functions, not var. I
	 * keep them for statistics. */
	size_t pressure;
	size_t quota;

	bool closing;
	struct completion is_safe_to_remove;
};

/**
 * Defines a callback struct for resource pool actions.
 */
struct nvmeib_rsc_pool_cb {
	struct list_head link;
	void *ctx; /* Context to pass to the callback */
	void (*act)(struct nvmeib_rsc_pool *,
	            void *); /* The actual callback to be invoked */
};

struct nvmeib_rsc_pool {

	/* Pool configuration, must me set before calling init */
	struct {
		/* Used to construct/destruct new resources*/
		struct nvmeib_rsc_pool_rsc *(*construct_rsc)(struct nvmeib_rsc_pool *);
		void (*destruct_rsc)(struct nvmeib_rsc_pool *,
		                     struct nvmeib_rsc_pool_rsc *);
	} construction;
	struct nvmeib_rsc_pool_conf {
		size_t size; /* Desired pool size */
		size_t size_pcpu; /* Desired pool size per cpu, if > 0 overrides the
		                     desired size, else discarded */
		size_t reserve; /* Desired reserve size */
		size_t
		    reserve_percent; /* Desired reserve size in percents of size. If > 0
		                        overrides desired reserve, else discarded. */
	} conf;

	/* Private data, shall not be accessed directly, only via the interface
	 * functions and macros */
	struct {
		/* Callbacks, needed for extensions, optional.
		 * Type of list member: struct nvmeib_rsc_pool_cb
		 */
		struct {
			/**
			 * Invoked before the pool is rebalanced.
			 * Can be used for example to check if some module param has
			 * changed, and modify pool settings accordingly, before rebalance
			 * starts.
			 * @warning Will be called under pool lock
			 */
			struct list_head on_before_rebalance;

			/**
			 * Invoked after the pool is rebalanced.
			 * Can be used for example to notify sleeping actors that rebalance
			 * has occurrred, and they shall check their quota.
			 * @warning Will be called under pool lock
			 */
			struct list_head on_after_rebalance;
		} cbs;

		struct {
			spinlock_t lock;
			unsigned long flags;
		} guard;

		/* The accepted configuration */
		struct nvmeib_rsc_pool_conf accepted;

		size_t total_pressure;
		size_t n_actors;
		size_t n_available_rsc;

		struct list_head rsc_list;
		struct list_head actor_list;

		struct {
			u64 locks_taken;
			u64 rebalances_issued;
			u64 peak_pressure;
		} stats;
	} priv;
};

/**
 * Initialize the pool
 */
void nvmeib_rsc_pool_init(struct nvmeib_rsc_pool *pool);

/**
 * Initialize the pool free the pool
 * @warning Must be called only if all
 */
void nvmeib_rsc_pool_free(struct nvmeib_rsc_pool *pool);

/**
 * Register an actor within the pool.
 * @note Actor data structure shall be allocated and conf section initialized
 * @warning Uses lock
 */
void nvmeib_rsc_pool_register_actor(struct nvmeib_rsc_pool *pool,
                                    struct nvmeib_rsc_pool_actor *actor);

/**
 * Initiate actor's closing process.
 */
#define nvmeib_rsc_pool_actor_start_closing(actor_) (actor_)->closing = true;

/**
 * Unregister actor from the pool.
 * @warning Uses lock
 * @warning Assumes actor closing process started
 */
void nvmeib_rsc_pool_unregister_actor(struct nvmeib_rsc_pool_actor *actor);

/**
 * Get actor's quota. A fast operation, never blocks.
 */
size_t nvmeib_rsc_pool_quota(struct nvmeib_rsc_pool_actor *actor);

/**
 * Update pool greed by delta and rebalance.
 * @warning Uses lock
 */
void nvmeib_rsc_pool_upd_greed(struct nvmeib_rsc_pool_actor *actor,
                               ssize_t delta);
#define nvmeib_rsc_pool_inc_greed(actor_) nvmeib_rsc_pool_upd_greed(actor_, 1)
#define nvmeib_rsc_pool_dec_greed(actor_) nvmeib_rsc_pool_upd_greed(actor_, -1)

/**
 * Give up to @count resources from the @pool to the @actor.
 * @warning Uses lock
 * @return The actual number of resources fetched
 */
size_t nvmeib_rsc_pool_fetch_rsc(struct nvmeib_rsc_pool_actor *actor,
                                 size_t count);

/**
 * Return a list of resources to the pool
 * @warning Uses lock
 */
void nvmeib_rsc_pool_return_rsc(struct nvmeib_rsc_pool_actor *actor,
                                size_t count);

/**
 * Register a new callback
 */
#define nvmeib_rsc_pool_register_callback(pool_, evt_, act_, ctx_, gfp_)       \
	({                                                                         \
		int ___rv___ = 0;                                                      \
		struct nvmeib_rsc_pool_cb *___cb___ =                                  \
		    kzalloc(sizeof(*___cb___), gfp_);                                  \
		if (!___cb___)                                                         \
			___rv___ = -ENOMEM;                                                \
		else {                                                                 \
			___cb___->ctx = ctx_;                                              \
			___cb___->act = act_;                                              \
			list_add_tail(&___cb___->link, &(pool_)->priv.cbs.evt_);           \
		}                                                                      \
		___rv___;                                                              \
	})

/**
 * Register unregister a callback (if exists)
 */
#define nvmeib_rsc_pool_unregister_callback(pool_, evt_, act_, ctx_)           \
	({                                                                         \
		struct nvmeib_rsc_pool_cb *___cb___, *___cb_tmp___;                    \
		list_for_each_entry_safe(___cb___, ___cb_tmp___,                       \
		                         &(pool_)->priv.cbs.evt_, link) {              \
			if (___cb___->act == act_ && ___cb___->ctx_ == ctx_) {             \
				list_del_init(&___cb___->link);                                \
				kfree(___cb___);                                               \
			}                                                                  \
		}                                                                      \
	})

/**
 * Unregister all callbacks on specific event
 */
#define nvmeib_rsc_pool_unregister_callback_all(pool_, evt_)                   \
	({                                                                         \
		struct nvmeib_rsc_pool_cb *___cb___, *___cb_tmp___;                    \
		list_for_each_entry_safe(___cb___, ___cb_tmp___,                       \
		                         &(pool_)->priv.cbs.evt_, link) {              \
			list_del_init(&___cb___->link);                                    \
			kfree(___cb___);                                                   \
		}                                                                      \
	})

/**
 * Get the pressure of a given actor
 */
#define nvmeib_rsc_pool_actor_pressure(actor_)                                 \
	(actor_->closing ? 0 : ((1 << (actor_)->greed) * (actor_)->conf.priority))

/**
 * Get the total pressure on the pool (last calculated value)
 */
#define nvmeib_rsc_pool_total_pressure(pool_) pool->priv.total_pressure

/**
 * Shortcut macros to check if actor has to return resources or get more
 */
#define nvmeib_rsc_pool_actor_need_return(actor_)                              \
	(nvmeib_rsc_pool_quota(actor_) < (actor_)->nbufs)
#define nvmeib_rsc_pool_actor_need_get(actor_)                                 \
	(nvmeib_rsc_pool_quota(actor_) > (actor_)->nbufs)
#define nvmeib_rsc_pool_actor_need_rebalance(actor_)                           \
	(nvmeib_rsc_pool_actor_need_return(actor_) ||                              \
	 nvmeib_rsc_pool_actor_need_get(actor_))

#endif /*NVMEIB_RSC_POOL*/