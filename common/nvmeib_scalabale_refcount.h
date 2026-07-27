#ifndef NVMEIB_SCALABALE_REFCOUNT_H
#define NVMEIB_SCALABALE_REFCOUNT_H

/*
 * this macro enables selection of a simple (yet slow) implementation vs. a
 * scalabale (in terms of cpu count, especially NUMA) implementation aimed at
 * maximum performance of get/put reference.
 * use the simple one for debugging & verifying that whatever issue you witness
 * isnt bcz of the the scalabale imlpementation
 */

//#	define NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC

struct scalabale_refcount;
/*
 * a callback that the counter is invoking in order to know whether the object  							   .
 * on which it protects needs to die.
 * return 'true' when the no references should be allowed anymore.
 * 	      'false' when a reference can be taken.
 */
typedef bool (*scalabale_refcount_condition)(struct scalabale_refcount	*s,
											 void						*ctx);
/*
 * a counter that is maintaining a private counter for each core & whose sum is
 * the actual value.
 * assuming the sum is rarely needed, this is very fast on the critical count
 * flow (e.g.: count IO's, packets, ...)
 * the only overhead this generic implementation has over a tailor-made one is
 * the callback it uses to check the termination condition.
 * if need to, this can be eliminated by implementing this whole class as a
 * template (using macros)
 */
struct scalabale_refcount {
#ifdef NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC
	atomic_t		count;
#else
	int	__percpu	*counters;
#endif
	void			*ctx;
	scalabale_refcount_condition	cond_cb;
};

static inline void scalabale_refcount_init(struct scalabale_refcount	*s,
										   scalabale_refcount_condition	cond_cb,
										   void							*ctx)
{
#ifdef NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC
	atomic_set(&s->count, 0);
#else
	int	i;
	s->counters = __nvmeib_public_alloc_percpu_zeroed(sizeof(*s->counters), __alignof__(*s->counters));	// Todo: Change to __alloc_percpu()
	for_each_possible_cpu(i) {
		*per_cpu_ptr(s->counters, i) = 0;
	}
#endif
	s->cond_cb = cond_cb;
	s->ctx = ctx;
}

static inline void scalabale_refcount_destroy(struct scalabale_refcount	*s)
{
#ifdef NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC
	(void)s;
#else
	nvmeib_public_free_percpu(s->counters);	// Todo: Change to free_percpu
#endif
}

// external API to read the aggregated count
static inline int scalabale_refcount_get_count(struct scalabale_refcount	*s)
{
	int	count;
#ifdef NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC
	count = atomic_read(&s->count);
#else
	int	i;
	count = 0;
	for_each_possible_cpu(i)
		count += *((volatile int *)per_cpu_ptr(s->counters, i));
#endif
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	BUG_ON(count < 0);
#endif
	return count;
}

/* Retrieve a reference. Returns true if reference taken or false otherwise */
static inline bool scalabale_refcount_get_ref(struct scalabale_refcount	*s)
{
#ifdef NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC
	/*int	val = */atomic_inc_return(&s->count);
	if (unlikely(s->cond_cb(s, s->ctx))) {
		atomic_dec(&s->count);
		return false;	// caller has failed to get a safe reference - must not use the guarded resource
	}
	return true;
#else
	int	rv = true;
	this_cpu_inc(*s->counters);
	if (unlikely(s->cond_cb(s, s->ctx))) {
		this_cpu_dec(*s->counters);
		rv = false;
	}
	return rv;	// caller has got a safe reference
#endif
}

/* release a reference taken by prev function */
static inline void scalabale_refcount_put_ref(struct scalabale_refcount	*s)
{
#ifdef NVMEIB_SCALABALE_REFCOUNT_IS_ATOMIC
	atomic_dec(&s->count);
#else
	this_cpu_dec(*s->counters);
#endif
}

/* Example of draining function
 * wait until all references are returned. Don't do in interrupt context!
 * BEWARE: new attempts to acquire a reference will increase the count & then   											   .
 * immediately decrease it.
 * hence, after the overall count decreaes to 0, it will be noisy for very short    										   .
 * times as the counters go up & immediately down.
 */
static inline void scalabale_refcount_drain(struct scalabale_refcount	*s)
{
	int	val, attempts;
	for (attempts = 1; true; attempts++) {
		val = scalabale_refcount_get_count(s);
		if (val == 0)
			return;
		BUG_ON(val < 0);
		msleep(1);	// TODO(EBA): in template, make this an optional callback
		if ((attempts & 0xFF) == 0) {
			pr_warn("Drain of usage counter taking too long : %d\n", attempts);
			cond_resched();
		}
	}
}

#endif

