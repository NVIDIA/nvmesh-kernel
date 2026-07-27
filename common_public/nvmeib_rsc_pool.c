#include "nvmeib_rsc_pool.h"

/*********************/
/* Private functions */
/*********************/

/**
 * Helper function - lock, disble irq, save flags, return pointer to self.
 */
static void *__nvmeib_rsc_pool_lock(struct nvmeib_rsc_pool *pool) {
	spin_lock_irqsave(&pool->priv.guard.lock, pool->priv.guard.flags);
	return pool;
}

/**
 * Helper function, unlock, restore irq, restore flags, from cleanup ctx.
 */
static void __nvmeib_rsc_pool_unlock(struct nvmeib_rsc_pool **pool) {
	spin_unlock_irqrestore(&(*pool)->priv.guard.lock,
	                       (*pool)->priv.guard.flags);
}

/**
 * If declared, will lock the pool, and auto unlock it if out of scope.
 * Uses GCC cleaup extension.
 */
#define __nvmeib_rsc_pool_guard(pool_)                                         \
	__attribute__((cleanup(                                                    \
	    __nvmeib_rsc_pool_unlock))) struct nvmeib_rsc_pool *___pool_guard___ = \
	    __nvmeib_rsc_pool_lock(pool_)

/**
 * Invoke all callbacks on the given event
 */
#define __nvmeib_rsc_pool_invoke_callback(pool_, cb_)                          \
	({                                                                         \
		struct nvmeib_rsc_pool_cb *___cb___;                                   \
		list_for_each_entry(___cb___, &(pool_)->priv.cbs.cb_, link) {          \
			BUG_ON(!___cb___->act);                                            \
			___cb___->act(pool_, ___cb___->ctx);                               \
		}                                                                      \
	})

/**
 * Forces a full rebalance, in case configuration was updated at runtime.
 * @warning Must be called either:
 * 1. Under lock
 * 2. From init / destroy context
 */
static void
__nvmeib_rsc_pool_force_rebalance_unsafe(struct nvmeib_rsc_pool *pool) {
	/* Pool resize will not be called from interrupt context. */
	__nvmeib_rsc_pool_invoke_callback(pool, on_before_rebalance);

	/* Validate the configuration*/
	if (pool->conf.reserve_percent != pool->priv.accepted.reserve_percent ||
	    pool->conf.size_pcpu != pool->priv.accepted.size_pcpu) {
		if (pool->conf.size_pcpu > 0)
			pool->conf.size = num_online_cpus() * pool->conf.size_pcpu;
		if (pool->conf.reserve_percent > 0)
			pool->conf.reserve =
			    (pool->conf.size * pool->conf.reserve_percent) / 100;
		BUG_ON(pool->conf.reserve_percent >= 100);
		BUG_ON(pool->conf.reserve >= pool->conf.size);

		/* Accept the new configuration */
		pool->conf.reserve_percent = pool->priv.accepted.reserve_percent;
		pool->conf.size_pcpu = pool->priv.accepted.size_pcpu;
	}

	/* Create additional resources if needed */
	while (pool->conf.size > pool->priv.accepted.size) {
		struct nvmeib_rsc_pool_rsc *rsc =
		    pool->construction.construct_rsc(pool);
		if (!rsc) break; /* Out of mem. No resize. */
		list_add_tail(&rsc->link, &pool->priv.rsc_list);
		pool->priv.accepted.size++;
		pool->priv.n_available_rsc++;
	}

	/* Free extra resources if available */
	while (pool->conf.size < pool->priv.accepted.size) {
		if (list_empty(&pool->priv.rsc_list))
			break;
		else {
			struct nvmeib_rsc_pool_rsc *rsc = list_first_entry(
			    &pool->priv.rsc_list, struct nvmeib_rsc_pool_rsc, link);
			list_del_init(&rsc->link);
			pool->construction.destruct_rsc(pool, rsc);
			pool->priv.accepted.size--;
			pool->priv.n_available_rsc--;
		}
	}

	/* Try to set reserve, as close as possible to the conf. In any case,
	 * reserve cannot be >= size */
	if (pool->priv.accepted.reserve != pool->conf.reserve) {
		pool->priv.accepted.reserve =
		    min(pool->conf.reserve, pool->priv.accepted.size - 1);
	}

	/* Recalculate the pressure */
	{
		struct nvmeib_rsc_pool_actor *actor;
		size_t total_pressure = 0;
		list_for_each_entry(actor, &pool->priv.actor_list, link) {
			total_pressure += nvmeib_rsc_pool_actor_pressure(actor);
		}
		pool->priv.total_pressure = total_pressure;
		if (pool->priv.stats.peak_pressure < total_pressure)
			pool->priv.stats.peak_pressure = total_pressure;
	}

	__nvmeib_rsc_pool_invoke_callback(pool, on_after_rebalance);
}

/***********************************/
/* Public functions implementation */
/***********************************/

void nvmeib_rsc_pool_init(struct nvmeib_rsc_pool *pool) {
	/* Zero the pool, except conf and construction sections */
	*pool = (struct nvmeib_rsc_pool){.conf = pool->conf,
	                                 .construction = pool->construction};
	INIT_LIST_HEAD(&pool->priv.rsc_list);
	INIT_LIST_HEAD(&pool->priv.actor_list);
	spin_lock_init(&pool->priv.guard.lock);

	INIT_LIST_HEAD(&pool->priv.cbs.on_before_rebalance);
	INIT_LIST_HEAD(&pool->priv.cbs.on_after_rebalance);

	__nvmeib_rsc_pool_force_rebalance_unsafe(pool);

	BUG_ON(pool->conf.size !=
	       pool->priv.accepted.size); /* Validate that all reasource
	                            are allocated successfully*/
}

void nvmeib_rsc_pool_free(struct nvmeib_rsc_pool *pool) {
	BUG_ON(!list_empty(&pool->priv.actor_list));
	pool->conf.size = 0;

	nvmeib_rsc_pool_unregister_callback_all(pool, on_before_rebalance);
	nvmeib_rsc_pool_unregister_callback_all(pool, on_after_rebalance);

	/*Rebalancing with conf size = 0 will cause release of all resources */
	__nvmeib_rsc_pool_force_rebalance_unsafe(pool);
	/*Validate that all worked*/
	BUG_ON(!list_empty(&pool->priv.rsc_list));
	BUG_ON(pool->priv.accepted.size != 0);
}

void nvmeib_rsc_pool_register_actor(struct nvmeib_rsc_pool *pool,
                                    struct nvmeib_rsc_pool_actor *actor) {
#ifndef LLVM
	__nvmeib_rsc_pool_guard(pool); /* Do under lock */
#else
	__nvmeib_rsc_pool_lock(pool);
#endif
	BUG_ON(actor->conf.max_greed < 0);
	BUG_ON(actor->conf.min_greed < 0);
	BUG_ON(actor->conf.max_greed < actor->conf.min_greed);

	*actor = (struct nvmeib_rsc_pool_actor){
	    .conf = actor->conf,
	    .pool = pool,
	    .greed = actor->conf.min_greed}; /*zeroing*/
	INIT_LIST_HEAD(&actor->rsc_list);

	init_completion(&actor->is_safe_to_remove); /*we will need it in the end*/

	list_add_tail(&actor->link, &pool->priv.actor_list);
	pool->priv.n_actors++;

	__nvmeib_rsc_pool_force_rebalance_unsafe(pool);
#ifdef LLVM
	__nvmeib_rsc_pool_unlock(&pool);
#endif
}

void nvmeib_rsc_pool_unregister_actor(struct nvmeib_rsc_pool_actor *actor) {
	struct nvmeib_rsc_pool *pool = actor->pool;
	BUG_ON(!actor->closing);

	if (!actor->nbufs) complete(&actor->is_safe_to_remove);

	/* At this point, actor knows to shut down. Lets wait. */

	wait_for_completion(&actor->is_safe_to_remove);

	/* Actor returned all resources. */

	{
#ifndef LLVM
		__nvmeib_rsc_pool_guard(pool); /* Lock again */
#else
		__nvmeib_rsc_pool_lock(pool);
#endif
		BUG_ON(!list_empty(&actor->rsc_list));
		BUG_ON(list_empty(&actor->link));

		list_del_init(&actor->link);
		pool->priv.n_actors--;
#ifdef LLVM
		__nvmeib_rsc_pool_unlock(&pool);
#endif
	}
}

size_t nvmeib_rsc_pool_quota(struct nvmeib_rsc_pool_actor *actor) {
	struct nvmeib_rsc_pool *pool = actor->pool;
	/* Total pressure may change as we work, so back it up to avoid races */
	size_t total_pressure = pool->priv.total_pressure;
	actor->pressure = actor->quota = 0;
	if (actor->closing) { return 0; }
	if (!total_pressure)
		return 0;
	else {
		const size_t resulting_size =
		    min(pool->conf.size, pool->priv.accepted.size);
		actor->pressure = nvmeib_rsc_pool_actor_pressure(actor);
		actor->quota = max(actor->conf.min_rsc,
		                   resulting_size * actor->pressure / total_pressure);
		return actor->quota;
	}
}

void nvmeib_rsc_pool_upd_greed(struct nvmeib_rsc_pool_actor *actor,
                               ssize_t delta) {
	struct nvmeib_rsc_pool *pool = actor->pool;
	ssize_t new_greed = actor->greed;
	new_greed += delta;
	if (new_greed > actor->conf.max_greed) new_greed = actor->conf.max_greed;
	if (new_greed < actor->conf.min_greed) new_greed = actor->conf.min_greed;

	if (new_greed != actor->greed) {
#ifndef LLVM
		__nvmeib_rsc_pool_guard(pool); /* Do under lock */
#else
		__nvmeib_rsc_pool_lock(pool);
#endif
		actor->greed = new_greed;
		__nvmeib_rsc_pool_force_rebalance_unsafe(pool);
#ifdef LLVM
		__nvmeib_rsc_pool_unlock(&pool);
#endif
	}
}

size_t nvmeib_rsc_pool_fetch_rsc(struct nvmeib_rsc_pool_actor *actor,
                                 size_t count) {
	struct nvmeib_rsc_pool *pool = actor->pool;
	struct nvmeib_rsc_pool_rsc *rsc, *__tmp;
	size_t fetched = 0;
#ifndef LLVM
	__nvmeib_rsc_pool_guard(pool); /* Do under lock */
#else
	__nvmeib_rsc_pool_lock(pool);
#endif
	list_for_each_entry_safe(rsc, __tmp, &pool->priv.rsc_list, link) {
		if (fetched == count) break;
		list_del_init(&rsc->link);
		list_add_tail(&rsc->link, &actor->rsc_list);
		fetched++;
	}

	actor->nbufs += fetched;
#ifdef LLVM
	__nvmeib_rsc_pool_unlock(&pool);
#endif

	return fetched;

	/* No rebalance needed */
}

void nvmeib_rsc_pool_return_rsc(struct nvmeib_rsc_pool_actor *actor,
                                size_t count) {
	struct nvmeib_rsc_pool *pool = actor->pool;
	struct nvmeib_rsc_pool_rsc *rsc, *__tmp;
	size_t returned = 0;
#ifndef LLVM
	__nvmeib_rsc_pool_guard(pool); /* Do under lock */
#else
	__nvmeib_rsc_pool_lock(pool);
#endif
	list_for_each_entry_safe(rsc, __tmp, &actor->rsc_list, link) {
		if (returned == count) break;
		list_del_init(&rsc->link);
		list_add_tail(&rsc->link, &pool->priv.rsc_list);
		returned++;
	}

	BUG_ON(returned != count);

	actor->nbufs -= returned;

	if (actor->closing && !actor->nbufs) complete(&actor->is_safe_to_remove);
#ifdef LLVM
	__nvmeib_rsc_pool_unlock(&pool);
#endif

	/* No rebalance needed */
}
