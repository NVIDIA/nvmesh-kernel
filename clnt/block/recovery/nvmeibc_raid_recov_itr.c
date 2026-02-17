#include "nvmeibc_raid_recov_itr.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.h"	// Stale special value
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"			// vfree()
#include "nvmeibc_msgs_shared.h"			// usage of NVMEIBC_DIRTY_BITS_PAGES
#include "nvmeibc_memmgr_metrics.h"
#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	ulong nvmeibc_iter_colldown = 1;			// 1[msec]			// To make stuff much faster
#else
	ulong nvmeibc_iter_colldown = 500;			// 0.5[sec]
#endif
module_param_named(recovery_iterator_cooldown, nvmeibc_iter_colldown, ulong, 0644);
MODULE_PARM_DESC(recovery_iterator_cooldown, "Recovery iterator timeout to wait after completing full recovery cycle in jiffies. Increasing this trades recovery load vs. recovery time.");

NVMEIBC_MEMMGR_METRIC(dp_recovery_problems_report, "component=raid.io_ctrl.recovery.problems");

struct recovery_itr_ctx {
	u64 n_elems;                     /* number of elements in the current batch */
	u64 cur;                         /* current postion in the array */
	u64 n_fixed;                     /* Total amount of successfully fixed locks up to now */
	u64 n_broken;                    /* Amount of non zero elems in the original array. Upon finish n_fixed==n_broken */
	u64 n_fixed_at_cycle_start;      /* snapshot of 'n_fixed' at the end last cycle */
	// Note n_fixed_at_cycle_start <= n_fixed <= n_broken
	bool is_mandatory;               /* Should recover all or is it allowed to skip entries */
	u64 jiff_last_ineffective_cycle; /* When was last full cycle of this iterator completed (in units of jiffies), with
	                                    less than 50% fixed, if ever (0 if never) */
};

// Note: Inheritted iterator from base, for each job must include Trinary-status: job is {todo, in process, finished}
struct recovery_itr_ctx_blocksets {						// Iterator for data recovery (blocksets)
	struct recovery_itr_ctx base;
	union {
		void *_arr;   									// raw reference for allocations
		union nvmeib_blkset_problem_report *arr16;   	// u16map
		union nvmeib_blkset_sparse_report  *arr64;		// u64map
	};
	s64 arr2raid_offset;/*Translation offset from element 'i' in the array to corresponding blockset in raid */
	int elem_size   : 8;/*in bits: 16,64 entry. 64 bits is sparse array */
};

struct recovery_itr_ctx_jour {					// Iterator for journal recoveries JGC / COLD
	struct recovery_itr_ctx base;
	struct nvmibc_blockset_candidates *candidates;
};

/*************************** Base class of iterator ***************************/
static void __rcvr_itr_common_on_batch_done(struct nvmeibc_recovery_itr_t* self)
{
	self->ctx->n_broken = self->ctx->n_fixed = self->ctx->n_fixed_at_cycle_start = 0;
}

static void __rcvr_itr_common_set_upper_bound(struct nvmeibc_recovery_itr_t* self, u64 n_elems)
{
	self->ctx->n_broken = n_elems;	// n_broken = gets upper bound (very important)
}

static void __rcvr_itr_common_skip_all(struct nvmeibc_recovery_itr_t* self)
{
	self->ctx->n_fixed = self->ctx->n_broken;
}

static struct recovery_itr_ctx *__rcvr_itr_common_create_ctx(u32 derived_size, bool is_mandatory)
{
	struct recovery_itr_ctx *rv = kzalloc(derived_size, GFP_KERNEL);
	if (rv) {
		rv->is_mandatory = is_mandatory;
	}
	return rv;		// Note: zeroing all fields is a perfect initialization
}

#define MAX_ARR_PROBLEMS_SIZE (NVMEIBC_DIRTY_BITS_PAGES * PAGE_SIZE)
static void *__rcvr_itr_blksets_create_arr(void)
{
	void *res = vmalloc(MAX_ARR_PROBLEMS_SIZE);
	nvmesh_memmgr_metric_on_alloc_update(dp_recovery_problems_report, MAX_ARR_PROBLEMS_SIZE, res);
	if (!res)
		_NT(err_rcvr_itr_blksets_create_arr, "nvmeibc OOM. size=@LLU", MAX_ARR_PROBLEMS_SIZE);
	return res;
}

static void __rcvr_itr_common_get_progress(const struct nvmeibc_recovery_itr_t* self, u64* done, u64* total)
{
	*done  = self->ctx->n_fixed;
	*total = self->ctx->n_broken;
}

static inline bool __rcvr_itr_common_has_next(const struct recovery_itr_ctx *ctx)
{ // If false: iterator cannot give next job but it might not be done. Some blocksets might still be in process by other workers
	return (ctx->cur < ctx->n_elems);
}

static inline void __rcvr_itr_common_complete_cycle(struct recovery_itr_ctx *ctx)
{
	if (__rcvr_itr_common_has_next(ctx)) {
		const u64 expected_work = (ctx->n_broken - ctx->n_fixed_at_cycle_start);
		const u64 complete_work = (ctx->n_fixed  - ctx->n_fixed_at_cycle_start);
		if ((8*complete_work) < expected_work) {	// Cycle is considered effective if at least 12.5% of remaining work was done
			_NT(t_01_recovitr, "Inefficiency triggered: ctx_ptr=@PTR, nf_start=@LLU < nf_now=@LLU < nbroken=@LLU", ctx, ctx->n_fixed_at_cycle_start, ctx->n_fixed, ctx->n_broken);
			ctx->jiff_last_ineffective_cycle = jiffies; /* You are not effective. Stand in the corner and think about your behaviour. */
		}
		ctx->n_fixed_at_cycle_start = ctx->n_fixed;
	} else { /* This worker is going to sleep anyways (maybe it never started working). Let other workers decide for themselves */ }
}

static void __rcvr_itr_common_reinit(struct recovery_itr_ctx *ctx, u64 num_items)
{
	ctx->n_elems = num_items;
	ctx->cur = 0;		 		// Invalid value, initialized by begin() method
	ctx->n_fixed = ctx->n_fixed_at_cycle_start = 0;
	ctx->n_broken = num_items;	// Default: all is broken
	ctx->jiff_last_ineffective_cycle = 0;
}

/* Calculate the delay needed before next job start */
static inline u64 __rcvr_itr_common_delay(const struct recovery_itr_ctx *ctx) {
	if (ctx->jiff_last_ineffective_cycle == 0) {		// Common case, all cycles are effective, no delay, immediate execution
		return 0;
	} else {
		const u64 now = jiffies; /* Freeze time */
		const u64 jif_after_cooldown = ctx->jiff_last_ineffective_cycle + ((nvmeibc_iter_colldown * HZ) / 1000);
		return (now >= jif_after_cooldown) ? 0 : (jif_after_cooldown - now);
	}
}

static const struct nvmeibc_recovery_itr_job itr_job_dummy = { .cookie = ~0ULL,
	.blkset_lba = ~0, .aux = {0}, .err = 0 /* must be 0, dont touch*/, .jif_delay_before_work = 0ULL, }; // When iterator allocates job to worker: .err field must is zero
void nvmeibc_recovery_itr_job_init(struct nvmeibc_recovery_itr_job *j)
{
	*j = itr_job_dummy;	// As if dummy job completed with success
}

bool nvmeibc_recovery_itr_job_has_job(struct nvmeibc_recovery_itr_job *j)
{
	return (j->cookie != ~0ULL);	// Worker initialized to dummy job and gets dummy job when nothing to do
}

/*************************** Iterator on data blocksets ***********************/
static inline struct recovery_itr_ctx_blocksets* __get_blocksets_ctx(struct recovery_itr_ctx* base)
{
	return container_of(base, struct recovery_itr_ctx_blocksets, base);
}

static void __blocksets_clean_temp_bits(struct recovery_itr_ctx_blocksets *ctx)
{
	u64 i;
	if (ctx->elem_size == 16) {
		for (i = 0; i < ctx->base.n_elems; i++)
			ctx->arr16[i].in_progress = 0;
	} else if (ctx->elem_size == 64) {
		for (i = 0; i < ctx->base.n_elems; i++)
			ctx->arr64[i].in_progress = 0;
	}
}

static u64 __blocksets_calc_n_broken(struct recovery_itr_ctx_blocksets *ctx)
{
	u64 i, rv = 0;
	if (ctx->elem_size == 16) {
		for (i = 0; i < ctx->base.n_elems; i++)
			rv += (ctx->arr16[i].all != 0);
	} else if (ctx->elem_size == 64) {
		for (i = 0; i < ctx->base.n_elems; i++)
			rv += (ctx->arr64[i].val != 0);
	}
	return rv;
}

static s64 __blocksets_calc_arr2raid(struct recovery_itr_ctx_blocksets *ctx, const u64 slba_start, const u64 rlba_start)
{
	if (ctx->elem_size == 64)   // Absolute index on drive - need to adjust by -segment start
		return (s64)-slba_start;
	else						// Relative index to rlba of current batch
		return (s64)rlba_start;
}

static void __recovery_itr_ctx_blocksets_to_string(const struct recovery_itr_ctx_blocksets* ctx)
{
	int i, n_elem = (int)min(ctx->base.n_elems, 64ULL);		// Print at most first 100 elements
	_ND(t_01_recitr2str, "arr2raid=@COUNTS, elem_size=@INT, n_broken=@COUNTS, is_mand=@INT",
		ctx->arr2raid_offset, ctx->elem_size, ctx->base.n_broken, ctx->base.is_mandatory);
	if (ctx->elem_size == 16) {
		for (i = 0; i < n_elem; i++) {
			const union nvmeib_blkset_problem_report el = ctx->arr16[i];
			_ND(t_02_recitr2str, "@INT: @BLKSET_PROBLEM_REPORT", i, el.all);
		}
	} else if (ctx->elem_size == 64) {
		for (i = 0; i < n_elem; i++) {
			const union nvmeib_blkset_sparse_report el = ctx->arr64[i];
			_ND(t_03_recitr2str, "@INT: @BLKSET_SPARSE_REPORT", i, el.all);
		}
	}
}

// Update iterator with the new batch of work
static int __blocksets_reinit(struct nvmeibc_recovery_itr_t* self, u64 slba_start, u64 rlba_start, u64 num_items,
						  int elem_size, const u8 *arr)
{
	int rv = 0;
	struct recovery_itr_ctx_blocksets* ctx = __get_blocksets_ctx(self->ctx);

	WARN_ON(num_items != ctx->base.n_broken);		// Upper bound was not set correctly
	WARN((elem_size != 16) && (elem_size != 64), "nvmeibc elem_size=%d", elem_size);
	if (num_items) { // Allow empty sparse array
		const u64 num_bytes = DIV_ROUND_UP((num_items*elem_size), 8); // Needed bytes to copy the array
		if (unlikely(num_bytes > MAX_ARR_PROBLEMS_SIZE)) {
			_NT(t_04_recitr2str, "nvmeibc bug, cannot complete recovery: arr_size=@LLX > max_size=@LLX", num_bytes, MAX_ARR_PROBLEMS_SIZE);
			rv = -ENOMEM;
			goto _out;
		}
		memcpy(ctx->_arr, arr, num_bytes);
	}
	ctx->elem_size = elem_size;
	ctx->arr2raid_offset = __blocksets_calc_arr2raid(ctx, slba_start, rlba_start);
	__rcvr_itr_common_reinit(&ctx->base, num_items);
	__blocksets_clean_temp_bits(ctx);
	ctx->base.n_broken = __blocksets_calc_n_broken(ctx);
	__recovery_itr_ctx_blocksets_to_string(ctx);

_out:
	return rv;
}

static void __blocksets_free(struct nvmeibc_recovery_itr_t* self)
{
	struct recovery_itr_ctx_blocksets* ctx = __get_blocksets_ctx(self->ctx);
	if (ctx) {
		if (ctx->_arr) {
			nvmesh_memmgr_metric_on_free_update(dp_recovery_problems_report, MAX_ARR_PROBLEMS_SIZE);
			my_vfree_deferred(ctx->_arr); 		//kfree(ctx->_arr);
		}
		kfree(ctx);
	}
}

static void __blocksets_convert_blkset_to_job(const struct recovery_itr_ctx_blocksets* ctx, struct nvmeibc_recovery_itr_job *res)
{
	if (unlikely(!__rcvr_itr_common_has_next(&ctx->base))) {
		nvmeibc_recovery_itr_job_init(res);
		return;
	}

	res->cookie = ctx->base.cur;
	res->err = 0;
	WARN_ON(ctx->base.n_elems == 0);	// If batch is empty, than iterator begin should have said this and batch would be skipped
	if (ctx->elem_size == 16) {
		union nvmeib_blkset_problem_report *e = &ctx->arr16[ctx->base.cur];
		e->in_progress = 1;
		res->aux.blckset_problem = *e;
		res->blkset_lba = (ctx->base.cur + ctx->arr2raid_offset);
	} else {
		union nvmeib_blkset_sparse_report* e = &ctx->arr64[ctx->base.cur];
		e->in_progress = 1;
		res->aux.stale_lock = e->val;
		res->blkset_lba = (       e->ind + ctx->arr2raid_offset);
	}
	res->jif_delay_before_work = __rcvr_itr_common_delay(&ctx->base);
}

static inline bool __u16_can_fix(union nvmeib_blkset_problem_report  p) { return (p.all != 0)&&(p.in_progress == 0); }
static inline bool __u64_can_fix(union nvmeib_blkset_sparse_report   p) { return (p.val != 0)&&(p.in_progress == 0); }
static inline void __u16_set_fix(union nvmeib_blkset_problem_report *p, bool is_fixed) { WARN_ON(!p->in_progress); p->in_progress = 0; if (is_fixed) p->all = 0; }
static inline void __u64_set_fix(union nvmeib_blkset_sparse_report  *p, bool is_fixed) { WARN_ON(!p->in_progress); p->in_progress = 0; if (is_fixed) p->val = 0; }

static void __blocksets_find_first_dirty_blkst(struct recovery_itr_ctx_blocksets* ctx)
{
	u64 i, n_elems = ctx->base.n_elems;
	if (ctx->elem_size == 16)
		for (i = 0; (i < n_elems)&&(!__u16_can_fix(ctx->arr16[i])); ++i);
	else
		for (i = 0; (i < n_elems)&&(!__u64_can_fix(ctx->arr64[i])); ++i);
	ctx->base.cur = i;			// Not found, batch is empty, or other workers already fixing all problems
}

static void __blocksets_begin(struct nvmeibc_recovery_itr_t* self, struct nvmeibc_recovery_itr_job *res)
{
	struct recovery_itr_ctx_blocksets* ctx = __get_blocksets_ctx(self->ctx);
	__blocksets_find_first_dirty_blkst(ctx);
	__blocksets_convert_blkset_to_job(ctx, res);
}

static bool __blocksets_is_dirty(const struct recovery_itr_ctx_blocksets* ctx)
{
	if (ctx->elem_size == 16) return __u16_can_fix(ctx->arr16[ctx->base.cur]);
	else  					  return __u64_can_fix(ctx->arr64[ctx->base.cur]);
}

static void __blocksets_skip_to_next(struct recovery_itr_ctx_blocksets* ctx)
{
	do {
		++ctx->base.cur;
		if (!__rcvr_itr_common_has_next(&ctx->base)) {
			__blocksets_find_first_dirty_blkst(ctx);
			__rcvr_itr_common_complete_cycle(&ctx->base);
			break;
		}
	} while (!__blocksets_is_dirty(ctx));
}

static inline void __blocksets_mark_prev_rv(struct recovery_itr_ctx_blocksets* ctx, const struct nvmeibc_recovery_itr_job *res)
{
	const bool is_fixed = ((!res->err) || (!ctx->base.is_mandatory));	// If err && mandatory dont mark as fixed and allow retry failed blkset
	if (ctx->elem_size == 16) __u16_set_fix(&ctx->arr16[res->cookie], is_fixed);	// ctx->base.cur
	else					  __u64_set_fix(&ctx->arr64[res->cookie], is_fixed);
	if (is_fixed)
		ctx->base.n_fixed++;
}

static void __blocksets_next(struct nvmeibc_recovery_itr_t* self, struct nvmeibc_recovery_itr_job *res)
{
	struct recovery_itr_ctx_blocksets* ctx = __get_blocksets_ctx(self->ctx);
	if (nvmeibc_recovery_itr_job_has_job(res))		// Else, first call, no completed job
		__blocksets_mark_prev_rv(ctx, res);
	__blocksets_skip_to_next(ctx);
	__blocksets_convert_blkset_to_job(ctx, res);
}

int nvmeibc_recovery_itr_t_init_blocksets(struct nvmeibc_recovery_itr_t *self, bool is_mandatory)
{
	int rv = -ENOMEM;
	self->set_upper_bound =  __rcvr_itr_common_set_upper_bound;
	self->reinit =           __blocksets_reinit;
	self->begin_cb =         __blocksets_begin;
	self->next =             __blocksets_next;
	self->skip_all =         NULL;
	self->on_batch_done =    __rcvr_itr_common_on_batch_done;
	self->destroy=           __blocksets_free;
	self->get_progress =     __rcvr_itr_common_get_progress;

	if ((self->ctx = __rcvr_itr_common_create_ctx(sizeof(struct recovery_itr_ctx_blocksets), is_mandatory)) != NULL) {
		struct recovery_itr_ctx_blocksets *ctx = __get_blocksets_ctx(self->ctx);
		if ((ctx->_arr = __rcvr_itr_blksets_create_arr()) != NULL)
			rv = 0;
	}
	return rv;
}

/********** Iterator on sparse list of blocksets with jour candidates**********/
#include "../datapath_ec/recov/nvmeibc_block_dp_ec_recov_cold.h"
#include "../datapath_ec/recov/nvmeibc_block_dp_ec_recovery_common.h"

static inline struct recovery_itr_ctx_jour* __get_candidates_ctx(struct recovery_itr_ctx* base)
{
	return container_of(base, struct recovery_itr_ctx_jour, base);
}

static int __cold_cands_reinit(struct nvmeibc_recovery_itr_t* self, u64 slba_start, u64 rlba_start,
							 u64 num_items, int elem_size, const u8 *arr)
{
	struct recovery_itr_ctx_jour* ctx = __get_candidates_ctx(self->ctx);
	WARN_ON(num_items != ctx->base.n_broken);		// Upper bound was not set correctly
	(void)slba_start; (void)rlba_start; (void)elem_size;
	ctx->candidates = (typeof(ctx->candidates))arr;
	__rcvr_itr_common_reinit(&ctx->base, num_items);
	return 0;
}

enum { JOUR_INIT = 0, JOUR_PROCESSING = 1, JOUR_FIXED = 3 };
static inline void __jour_mark_prev_rv(struct recovery_itr_ctx_jour *ctx, const struct nvmeibc_recovery_itr_job *res)
{
	struct nvmibc_blockset_candidates *c = &ctx->candidates[res->cookie];
	const bool is_fixed = ((!res->err) || (!ctx->base.is_mandatory));	// If err && mandatory dont mark as fixed and allow retry failed blkset
	WARN_ON(c->status != JOUR_PROCESSING);
	if (is_fixed) {
		c->status = JOUR_FIXED;
		ctx->base.n_fixed++;
	} else {
		c->status = JOUR_INIT;
	}
}

static void __jour_find_first_dirty_blkst(struct recovery_itr_ctx_jour* ctx)
{
	struct nvmibc_blockset_candidates *c = ctx->candidates;
	u64 i;
	for (i = 0; (i < ctx->base.n_elems)&&(c[i].status != JOUR_INIT); i++);	// Find first candidate that can be processed
	ctx->base.cur = i;
}

static void __jour_convert_blkset_to_job(const struct recovery_itr_ctx_jour* ctx, struct nvmeibc_recovery_itr_job *res)
{
	struct nvmibc_blockset_candidates *c = &ctx->candidates[ctx->base.cur];
	if (unlikely(!__rcvr_itr_common_has_next(&ctx->base))) {
		nvmeibc_recovery_itr_job_init(res);
		return;
	}
	res->cookie = ctx->base.cur;		// Allways true for single worker. For multiple, 'cur' can advance while cookie is in used
	res->aux.blckset_candidates_ptr = c;
	res->blkset_lba = c->blkset_lba;
	res->err = 0;
	res->jif_delay_before_work = __rcvr_itr_common_delay(&ctx->base);
	c->status = JOUR_PROCESSING;
}

static void __jour_itr_begin(struct nvmeibc_recovery_itr_t* self, struct nvmeibc_recovery_itr_job *res)
{
	struct recovery_itr_ctx_jour* ctx = __get_candidates_ctx(self->ctx);
	__jour_find_first_dirty_blkst(ctx);
	__jour_convert_blkset_to_job(ctx, res);
}

static void __jour_itr_free(struct nvmeibc_recovery_itr_t* self)
{
	struct recovery_itr_ctx_jour* ctx = __get_candidates_ctx(self->ctx);
	cold_iterator_free(ctx->candidates, ctx->base.n_elems);
	kfree(self->ctx);
}

static void __jour_skip_to_next(struct recovery_itr_ctx_jour* ctx)
{
	struct nvmibc_blockset_candidates *c = ctx->candidates;
	do {
		++ctx->base.cur;
		if (!__rcvr_itr_common_has_next(&ctx->base)) {
			__jour_find_first_dirty_blkst(ctx);	// if lock clean failed in the past we need to retry
			__rcvr_itr_common_complete_cycle(&ctx->base);
			break;
		}
	} while (c[ctx->base.cur].status != JOUR_INIT);
}

static void __jour_next(struct nvmeibc_recovery_itr_t* self, struct nvmeibc_recovery_itr_job *res)
{
	struct recovery_itr_ctx_jour* ctx = __get_candidates_ctx(self->ctx);
	if (nvmeibc_recovery_itr_job_has_job(res))		// Else, first call, no completed job
		__jour_mark_prev_rv(ctx, res);
	__jour_skip_to_next(ctx);
	__jour_convert_blkset_to_job(ctx, res);
}

int nvmeibc_recovery_itr_init_cold_candidates(struct nvmeibc_recovery_itr_t *self, bool is_mandatory)
{
	self->set_upper_bound =  __rcvr_itr_common_set_upper_bound;
	self->reinit =           __cold_cands_reinit;
	self->begin_cb =         __jour_itr_begin;
	self->next =             __jour_next;
	self->skip_all =         __rcvr_itr_common_skip_all;
	self->on_batch_done =    __rcvr_itr_common_on_batch_done;
	self->destroy=           __jour_itr_free;
	self->get_progress =     __rcvr_itr_common_get_progress;
	self->ctx =              __rcvr_itr_common_create_ctx(sizeof(struct recovery_itr_ctx_jour), is_mandatory);
	return (self->ctx) ? 0 : -ENOMEM;
}

/*************************** Assist funcs ***********************************/
void nvmeibc_recovery_itr_t_destroy(struct nvmeibc_recovery_itr_t *self)
{
	if (self->destroy)
		self->destroy(self);
	nvmeibc_recovery_itr_t_init_null(self);		// Just for debug
}
