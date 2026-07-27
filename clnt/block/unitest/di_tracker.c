#include "../nvmeibc_block_common.h"
#include "./uni_framework/simu_test.h"
#include <sys/queue.h>

#include "di_tracker.h"

/*
 * Data integrity tracker ("DI tracker") is used to verify plausibility of I/O
 * serializability, when notified on each I/O start and end.
 * For this purpose, DI tracker stores, per each block in volume (in the tracked
 * range), a history of I/O start and stop events, including short prefixes of
 * the read/written data blocks.
 * Not all events are stored forever though, but rather only those needed for
 * future read verifications (see the verification algorithm description below).
 * A mutex per block in volume protects only the access to that block's history
 * data structure, and is not held for the I/O duration. This fine-grained
 * (per block) synchronization allows for more I/O concurrency in tests.
 * For each I/O start notification the DI tracker returns an opaque context
 * object, which is passed back to DI tracker on that I/O's end notification.
 * That I/O context contains sub-contexts per each block of that I/O's range,
 * and its lifetime can exceed the lifetime of the I/O itself if any of its
 * sub-contexts are present in some volume block's history (as a start and/or
 * as an end event).
 * Given the above infrastructure, the verification algorithm can be run on each
 * block independently.
 *
 * Verification algorithm:
 * =======================
 * The basic idea of read verification is that a value read by a read I/O R can
 * be possibly written by:
 * 1. any write which is still pending when the read I/O completes, or
 * 2. any write W which has completed so far, unless it was certainly
 *    overwritten before R started, i.e. unless there is another write W' which
 *    started after W ended and completed successfully, given that W' completed
 *    before R started.
 *
 * We call a write W "write candidate" for a read R if it is possible
 * that R has/will read a value written by W.
 *
 * Generally speaking, when a pending write W completes, any write W' that is
 * certainly overwritten by W is no longer a candidate for any read that will
 * start in the future, and so it can be pruned from history right away,
 * allowing simple verification against all remaining writes when a future
 * read completes.
 * But the problem with this approach is that there might be pending reads when
 * W completes, and the writes that we want to prune might still be needed by
 * some of those pending reads. We solve this as follows:
 * - At all times we maintain a reference to the earliest-completed write
 *   candidate for any read to start in the future.
 * - Each starting read copies that reference to be used for its verification
 *   when it completes.
 * - We maintain a reference to the earliest started pending read and only
 *   prune writes completed before this read's earliest-completed write candidate.
 *
 * This solution is supported by the following facts:
 * - Any write W' completed after a completion of a write candidate W for a read R,
 *   is also a candidate for R. Otherwise, there is a write W'', which completed
 *   before R started, s.t. W'' started after W' completed. But then necessarily
 *   W'' started after W completed, which makes W a non-candidate for R,
 *   a contradiction.
 *   In particular, all completed candidates for R are exactly the writes that
 *   completed after (including) the completion of the earliest completed write
 *   candidate of R.
 * - If a write W' completes and renders a previously completed write W a
 *   non-candidate for a future read, and if W was the earliest-completed such
 *   candidate, then the new earliest-completed candidate is necessarily completed
 *   after W completed. Therefore an earliest-completed candidate for a read R
 *   had completed no later than the earliest-completed candidate for a read R',
 *   if R' started after R started.
 */

CIRCLEQ_HEAD(events_queue, di_tracker_block_event);

struct block_di_tracker {
	 struct events_queue events_head;
	 pthread_mutex_t mutex;
	 struct volume_di_tracker *vdt;
	 struct di_tracker_block_io_context *first_open_read;	// The earliest started not yet completed read
	 struct di_tracker_block_io_context *first_done_write_candidate;	// The earliest ended write which might have written the current value
};

static void __block_di_tracker_init(struct block_di_tracker *bdt, struct volume_di_tracker *vdt)
{
	CIRCLEQ_INIT(&bdt->events_head);
	pthread_mutex_init(&bdt->mutex, NULL);
	bdt->vdt = vdt;
	BUG_ON(bdt < vdt->block_di_trackers || bdt >= vdt->block_di_trackers + vdt->size);
	BUG_ON(bdt->first_open_read || bdt->first_done_write_candidate);
}

static void __block_di_tracker_fini(struct block_di_tracker *bdt)
{
	pthread_mutex_destroy(&bdt->mutex);
	BUG_ON(!CIRCLEQ_EMPTY(&bdt->events_head));
}

static void __volume_di_tracker_init(struct volume_di_tracker *vdt, u64 size, struct volume_di_tracker_conf *conf, int vol_i)
{
	int i;

	vdt->size = size;
	vdt->conf = conf;
	vdt->vol_i = vol_i;
	vdt->enabled = false;
	vdt->block_di_trackers = calloc(size, sizeof(struct block_di_tracker));
	pthread_rwlock_init(&vdt->rwlock, NULL);

	for (i = 0; i < (int)size; i++)
		__block_di_tracker_init(&vdt->block_di_trackers[i], vdt);
}

static void __volume_di_tracker_fini(struct volume_di_tracker *vdt)
{
	int i;

	for (i = 0; i < (int)vdt->size; i++)
		__block_di_tracker_fini(&vdt->block_di_trackers[i]);

	pthread_rwlock_destroy(&vdt->rwlock);

	free(vdt->block_di_trackers);
}


static void __volume_di_tracker_enable(struct volume_di_tracker *vdt)
{
	pthread_rwlock_wrlock(&vdt->rwlock);
	BUG_ON(vdt->enabled);
	vdt->enabled = true;
	pthread_rwlock_unlock(&vdt->rwlock);
}

static void __volume_di_tracker_disable(struct volume_di_tracker *vdt)
{
	pthread_rwlock_wrlock(&vdt->rwlock);
	BUG_ON(!vdt->enabled);
	vdt->enabled = false;
	pthread_rwlock_unlock(&vdt->rwlock);
}

static void __volume_di_tracker_reconf(struct volume_di_tracker *vdt, struct volume_di_tracker_conf *conf)
{
	pthread_rwlock_wrlock(&vdt->rwlock);
	BUG_ON(vdt->enabled);
	vdt->conf = conf;
	pthread_rwlock_unlock(&vdt->rwlock);
}


void di_tracker_init(struct di_tracker *dit, struct volume_di_tracker_conf *conf, int nvols)
{
	int i;

	dit->nvols = nvols;
	dit->volume_di_trackers = calloc(nvols, sizeof(*dit->volume_di_trackers));

	for (i = 0; i < nvols; i++)
		__volume_di_tracker_init(&dit->volume_di_trackers[i], DI_TRACKER_VOL_SIZE, conf, i);
}

void di_tracker_fini(struct di_tracker *dit)
{
	int i;

	for (i = 0; i < dit->nvols; i++)
		__volume_di_tracker_fini(&dit->volume_di_trackers[i]);

	free(dit->volume_di_trackers);
}

void di_tracker_enable(struct di_tracker *dit)
{
	int i;
	for (i = 0; i < dit->nvols; i++)
		__volume_di_tracker_enable(&dit->volume_di_trackers[i]);
}

void di_tracker_disable(struct di_tracker *dit)
{
	int i;
	for (i = 0; i < dit->nvols; i++)
		__volume_di_tracker_disable(&dit->volume_di_trackers[i]);
}

void di_tracker_reconf(struct di_tracker *dit, struct volume_di_tracker_conf *conf)
{
	int i;
	for (i = 0; i < dit->nvols; i++)
		__volume_di_tracker_reconf(&dit->volume_di_trackers[i], conf);
}

static void __block_di_tracker_remove_event(struct block_di_tracker *bdt, struct di_tracker_block_event *event);

static void __block_di_tracker_reset_unsafe(struct block_di_tracker *bdt)
{
	while (!CIRCLEQ_EMPTY(&bdt->events_head))
		__block_di_tracker_remove_event(bdt, CIRCLEQ_FIRST(&bdt->events_head));

	bdt->first_open_read = NULL;
	bdt->first_done_write_candidate = NULL;
}

void volume_di_tracker_reset(struct volume_di_tracker *vdt)
{
	int i;

	pthread_rwlock_wrlock(&vdt->rwlock);

	for (i = 0; i < (int)vdt->size; i++)
		__block_di_tracker_reset_unsafe(&vdt->block_di_trackers[i]);	// no need to take per-block mutex since we hold volume rwlock for write

	pthread_rwlock_unlock(&vdt->rwlock);
}

void di_tracker_reset(struct di_tracker *dit)
{
	int i;

	for (i = 0; i < dit->nvols; i++)
		volume_di_tracker_reset(&dit->volume_di_trackers[i]);
}

enum di_tracker_event_type {
	DTET_START,
	DTET_END,

	DI_TRACKER_EVENT_TYPE_COUNT
};

struct di_tracker_block_event {
	CIRCLEQ_ENTRY(di_tracker_block_event) events;	// in history list per block in volume_di_tracker
	enum di_tracker_event_type type;
};

struct di_tracker_block_io_context {
	struct di_tracker_block_event events[DI_TRACKER_EVENT_TYPE_COUNT];	// start, end
	union {
		struct {	// write, done read
			u8 data[DI_TRACKER_TRACKED_BLOCK_PREFIX_SIZE];
		};
		struct {	// pending read
			void *pdata;	// for not yet completed reads, dereferenced after read completes
			struct di_tracker_block_io_context *first_done_write_candidate;	// at the time of read start
		} pending_read;
	};
	int index;	// index in di_tracker_io_context->per_block
};

// "Trimmed" data for comparison. For now we assume zeros.
static char trimmed_data[DI_TRACKER_TRACKED_BLOCK_PREFIX_SIZE] = { 0 };

struct di_tracker_io_context {
	u64 start_lba;
	u64 nlbas;
	struct kref kref;	// each per-block context holds a ref to the container per list membership
	unsigned long bi_rw;
	int bi_rv;			// bio completion status
	struct di_tracker_block_io_context per_block[];	// variable size, must be last
};

static void __di_tracker_block_event_init(struct di_tracker_block_event *event)
{
	event->type = DI_TRACKER_EVENT_TYPE_COUNT;	// not-in-events-list mark
}

static void __di_tracker_block_event_fini(struct di_tracker_block_event *event)
{
	BUG_ON(event->type != DI_TRACKER_EVENT_TYPE_COUNT);
}

static void __di_tracker_block_io_context_init(struct di_tracker_block_io_context *bctx, int index, void *data, void *pdata)
{
	int i;

	bctx->index = index;

	BUG_ON(data && pdata);

	if (data) {
		memcpy(bctx->data, data, ARRAY_MEM_SIZE(bctx->data));
	}
	else if (pdata) {
		bctx->pending_read.pdata = pdata;
		BUG_ON(bctx->pending_read.first_done_write_candidate);	// verify zero init
	}

	for (i = 0; i < (int)ARRAY_SIZE(bctx->events); i++)
		__di_tracker_block_event_init(&bctx->events[i]);
}

static void __di_tracker_block_io_context_fini(struct di_tracker_block_io_context *bctx)
{
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(bctx->events); i++)
		__di_tracker_block_event_fini(&bctx->events[i]);
}

static void __di_tracker_io_context_init(struct di_tracker_io_context *ctx, u64 start_lba, u64 nlbas, unsigned long bi_rw, struct bio_vec *bi_io_vec, unsigned short bi_vcnt)
{
	int i;
	unsigned int cur_offset = 0;

	ctx->start_lba = start_lba;
	ctx->nlbas = nlbas;
	ctx->bi_rw = bi_rw;
	kref_init(&ctx->kref);

	for (i = 0; i < (int)nlbas; i++) {
		void *data = NULL, *pdata = NULL;	// data - save as value, pdata - save as reference, to dereference later

		if (((bi_rw & WRITE) && !(bi_rw & REQ_DISCARD)) || !(bi_rw & WRITE)) {
			struct scatterlist tmp_sg;
			BUG_ON(!bi_vcnt);
			BUG_ON(!bi_io_vec);

			sg_set_page(&tmp_sg, bi_io_vec->bv_page, NVMEIBC_SECTOR_SIZE, bi_io_vec->bv_offset + cur_offset);
			if (bi_rw & WRITE)
				data = sg_virt(&tmp_sg);
			else
				pdata = sg_virt(&tmp_sg);

			cur_offset += NVMEIBC_SECTOR_SIZE;
			WARN_ON(cur_offset > bi_io_vec->bv_len);
			if (cur_offset == bi_io_vec->bv_len) {
				bi_io_vec++;
				bi_vcnt--;
				cur_offset = 0;
			}
		}

		__di_tracker_block_io_context_init(&ctx->per_block[i], i, data, pdata);
	}
}

static void __di_tracker_io_context_fini(struct di_tracker_io_context *ctx)
{
	int i;

	for (i = 0; i < (int)ctx->nlbas; i++)
		__di_tracker_block_io_context_fini(&ctx->per_block[i]);
}

static struct di_tracker_io_context *__di_tracker_io_context_alloc(u64 start_lba, u64 nlbas, unsigned long bi_rw, struct bio_vec *bi_io_vec, unsigned short bi_vcnt)
{
	struct di_tracker_io_context *ctx = calloc(1, sizeof(struct di_tracker_io_context) + sizeof(struct di_tracker_block_io_context) * nlbas);
	__di_tracker_io_context_init(ctx, start_lba, nlbas, bi_rw, bi_io_vec, bi_vcnt);
	return ctx;
}

static void __di_tracker_io_context_free(struct di_tracker_io_context *ctx)
{
	__di_tracker_io_context_fini(ctx);
	free(ctx);
}

static void __di_tracker_io_context_kref_release(struct kref *kref)
{
	__di_tracker_io_context_free(container_of(kref, struct di_tracker_io_context, kref));
}

#define DT_BLOCK_IO_CTX_TO_IO_CTX(_bctx) \
	container_of((_bctx), struct di_tracker_io_context, per_block[(_bctx)->index])

#define DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(_event) \
({ \
	BUG_ON((_event)->type == DI_TRACKER_EVENT_TYPE_COUNT); \
	container_of((_event), struct di_tracker_block_io_context, events[(_event)->type]); \
})

#define DT_BLOCK_EVENT_TO_IO_CTX(_event) \
({ \
	struct di_tracker_block_io_context *_bctx = DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(_event); \
	DT_BLOCK_IO_CTX_TO_IO_CTX(_bctx); \
})

static bool __di_tracker_block_event_is_tracked(struct di_tracker_block_event *event)
{
	return (event->type != DI_TRACKER_EVENT_TYPE_COUNT);
}

static void __block_di_tracker_add_event(struct block_di_tracker *bdt, struct di_tracker_block_event *event, enum di_tracker_event_type type)
{
	BUG_ON(__di_tracker_block_event_is_tracked(event));
	event->type = type;
	CIRCLEQ_INSERT_TAIL(&bdt->events_head, event, events);
	kref_get(&DT_BLOCK_EVENT_TO_IO_CTX(event)->kref);
}

static void __block_di_tracker_remove_event(struct block_di_tracker *bdt, struct di_tracker_block_event *event)
{
	struct di_tracker_io_context *ctx;

	BUG_ON(!__di_tracker_block_event_is_tracked(event));

	ctx = DT_BLOCK_EVENT_TO_IO_CTX(event);
	CIRCLEQ_REMOVE(&bdt->events_head, event, events);
	event->type = DI_TRACKER_EVENT_TYPE_COUNT;
	kref_put(&ctx->kref, __di_tracker_io_context_kref_release);
}

static void __block_di_tracker_add_block_context_event(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx, enum di_tracker_event_type type)
{
	struct di_tracker_block_event *event = &bctx->events[type];

	BUG_ON(type >= ARRAY_SIZE(bctx->events));

	__block_di_tracker_add_event(bdt, event, type);
}

static void __block_di_tracker_remove_block_context_event(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx, enum di_tracker_event_type type)
{
	BUG_ON(type >= ARRAY_SIZE(bctx->events));

	__block_di_tracker_remove_event(bdt, &bctx->events[type]);
}


void __block_di_tracker_remove_all_ended_before(struct block_di_tracker *bdt, struct di_tracker_block_event *event)
{
	BUG_ON(!__di_tracker_block_event_is_tracked(event));
	while(CIRCLEQ_FIRST(&bdt->events_head) != event) {
		struct di_tracker_block_event *prev = CIRCLEQ_PREV(event, events);
		if (prev->type == DTET_END) {
			struct di_tracker_block_io_context *prev_bctx = DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(prev);
			__block_di_tracker_remove_block_context_event(bdt, prev_bctx, DTET_START);
			__block_di_tracker_remove_block_context_event(bdt, prev_bctx, DTET_END);
			prev = NULL;	// no longer in list, its context might be already freed here
		}
		else {
			event = prev;
		}
	}
}

static const char *__di_tracker_block_event_get_data_str(struct di_tracker_block_event *event)
{
	static char buf[DI_TRACKER_TRACKED_BLOCK_PREFIX_SIZE * 2 + 1];
	struct di_tracker_block_io_context *bctx = DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(event);
	struct di_tracker_io_context *ctx = DT_BLOCK_IO_CTX_TO_IO_CTX(bctx);
	int i;

	for (i = 0; i < DI_TRACKER_TRACKED_BLOCK_PREFIX_SIZE; i++) {
		if ((event->type == DTET_START && ((ctx->bi_rw & WRITE) && !(ctx->bi_rw & REQ_DISCARD))) ||
				(event->type == DTET_END && !(ctx->bi_rw & WRITE) && !ctx->bi_rv)) {
			sprintf(buf + i * 2, "%02x", bctx->data[i]);
		}
		else {
			sprintf(buf + i * 2, "  ");
		}
	}

	return buf;
}

static void __block_di_tracker_dump_history(struct block_di_tracker *bdt)
{
	struct di_tracker_block_event *event;
	unitest_trace(trace_block_di_tracker_dump_history,
			"ctx\t\tevent\ttype\tlba\tnlbas\trv\tdata\n"
			"----------------------------------------------------------------------------------------");
	CIRCLEQ_FOREACH(event, &bdt->events_head, events) {
		struct di_tracker_io_context *ctx = DT_BLOCK_EVENT_TO_IO_CTX(event);
		unitest_trace(trace_block_di_tracker_dump_history_event,
				"@DI_TRACKER_IO_CTX_PTR\t@STR\t@CHAR\t@VLBA\t@LEN_NLBAS\t@RV\t@STR",
				ctx, (event->type == DTET_START) ? "-->" : "<--",
				(ctx->bi_rw & WRITE) ? ((ctx->bi_rw & REQ_DISCARD) ? 'T' : 'W') : 'R',
				ctx->start_lba, ctx->nlbas ,ctx->bi_rv,
				__di_tracker_block_event_get_data_str(event));
	}
}

static pthread_mutex_t report_di_violation_mutex = PTHREAD_MUTEX_INITIALIZER;

static void __block_di_tracker_report_di_violation(struct block_di_tracker *bdt)
{
	struct volume_di_tracker *vdt = bdt->vdt;
	int lba = bdt - vdt->block_di_trackers;

	pthread_mutex_lock(&report_di_violation_mutex);
	unitest_trace_note(trace_block_di_tracker_report_di_violation, "DI violation on vol[@VOL_I] lba=@LBA! Relevant history:\n", vdt->vol_i, lba);
	__block_di_tracker_dump_history(bdt);
	WARN_ON(true);	// TODO: Elaborate more: print di_debug etc.
	pthread_mutex_unlock(&report_di_violation_mutex);
}

static void __block_di_tracker_verify_read_is_valid(struct block_di_tracker *bdt, struct di_tracker_block_io_context *read_bctx, struct di_tracker_block_io_context *first_done_write_candidate)
{
	// Find a matching write for a read, either complete or a not yet complete one,
	// unless no write had successfully ended before read started ("bedrock" read).
	// "Bedrock" data is defined as either volume format data or a data which was
	// written to the volume while DI tracking was disabled.

	struct di_tracker_block_event *event = &read_bctx->events[DTET_END];
	bool bedrock_read_possible = false;	// actually, more like "before read start and bedrock read possible"
	bool before_first_done_write_candidate = false;

	while (CIRCLEQ_FIRST(&bdt->events_head) != event) {
		event = CIRCLEQ_PREV(event, events);

		if (event == &read_bctx->events[DTET_START]) {
			bedrock_read_possible = true;	// Initialize as "bedrock read possible" only,
											// since "before read start" is now true.
		}
		else {
			struct di_tracker_block_io_context *cur_bctx = DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(event);
			struct di_tracker_io_context *cur_ctx = DT_BLOCK_IO_CTX_TO_IO_CTX(cur_bctx);
			if (cur_ctx->bi_rw & WRITE) {
				if (bedrock_read_possible && event->type == DTET_END && !cur_ctx->bi_rv) {	// bedrock_read_possible is true => necessarily "before read start" is true
					// Some I/O completed before our read started, which means that there is
					// no chance that our read I/O had read the "bedrock" data.
					bedrock_read_possible = false;
				}

				// Try match, on done write/trim end (if visible), or on pending write/trim start
				if ((cur_ctx->bi_rw & WRITE) &&
						((!before_first_done_write_candidate && event->type == DTET_END) ||
								( event->type == DTET_START && !__di_tracker_block_event_is_tracked(&cur_bctx->events[DTET_END])))) {
					if (cur_ctx->bi_rw & REQ_DISCARD) {
						if (!memcmp(read_bctx->data, trimmed_data, ARRAY_MEM_SIZE(read_bctx->data)))
							return;	// Bingo!
					}
					else {	// Normal write/read
						if (!memcmp(read_bctx->data, cur_bctx->data, ARRAY_MEM_SIZE(read_bctx->data)))
							return;	// Bingo!
					}
				}
			}

			if (cur_bctx == first_done_write_candidate)
				before_first_done_write_candidate = true;
		}
	}

	// No match. Is it possible that our read I/O had read the "bedrock" data?
	if (unlikely(!bedrock_read_possible))
		__block_di_tracker_report_di_violation(bdt);
}

static bool __block_di_tracker_is_write_certainly_overwritten_by(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx_write, struct di_tracker_block_io_context *bctx_by)
{
	struct di_tracker_io_context *ctx_by = DT_BLOCK_IO_CTX_TO_IO_CTX(bctx_by);
	struct di_tracker_io_context *ctx_write = DT_BLOCK_IO_CTX_TO_IO_CTX(bctx_write);
	struct di_tracker_block_event *event;

	BUG_ON(!(ctx_write->bi_rw & WRITE) || !(ctx_by->bi_rw & WRITE));

	if (ctx_by->bi_rv)	// "by" write failed
		return false;	// not _certainly_ overwritten

	event = &bctx_write->events[DTET_END];
	while (event != CIRCLEQ_LAST(&bdt->events_head)) {
		event = CIRCLEQ_NEXT(event, events);
		if (event == &bctx_by->events[DTET_START]) {
			return true;
		}
	}

	return false;
}

static void __block_di_tracker_prune_all_certainly_overwritten_by(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx_by)
{
	__block_di_tracker_remove_all_ended_before(bdt, &bctx_by->events[DTET_START]);
}

static void __block_di_tracker_track_io_start_unsafe(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx)
{
	struct di_tracker_io_context *ctx = DT_BLOCK_IO_CTX_TO_IO_CTX(bctx);

	__block_di_tracker_add_block_context_event(bdt, bctx, DTET_START);

	if (!(ctx->bi_rw & WRITE)) {	// a read
		// Take a snapshot of what the current first write candidate is, so it won't
		// get pruned and so it will be used to start the read verification from,
		// when the read eventually completes.
		bctx->pending_read.first_done_write_candidate = bdt->first_done_write_candidate;

		if (!bdt->first_open_read)
			bdt->first_open_read = bctx;
	}
}

static void __block_di_tracker_track_io_start(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx)
{
	pthread_mutex_lock(&bdt->mutex);

	__block_di_tracker_track_io_start_unsafe(bdt, bctx);

	pthread_mutex_unlock(&bdt->mutex);
}

static void __block_di_tracker_track_read_end(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx, struct di_tracker_io_context *ctx)
{
	__block_di_tracker_add_block_context_event(bdt, bctx, DTET_END);	// a convenience, for reporting a DI violation if any

	if (!ctx->bi_rv) {
		// Read success. Dereference read data and verify:

		// Copy pending_read fields to stack before dereferencing (will be overwritten)
		void *pdata = bctx->pending_read.pdata;
		struct di_tracker_block_io_context *first_done_write_candidate = bctx->pending_read.first_done_write_candidate;

		// Dereference
		memcpy(bctx->data, pdata, ARRAY_MEM_SIZE(bctx->data));

		// Verify
		__block_di_tracker_verify_read_is_valid(bdt, bctx, first_done_write_candidate);
	}

	// Going to remove the read, update first_open_read if needed.
	if (bctx == bdt->first_open_read) {
		struct di_tracker_block_event *event;

		bdt->first_open_read = NULL;
		event = &bctx->events[DTET_START];
		while (event != CIRCLEQ_LAST(&bdt->events_head)) {
			event = CIRCLEQ_NEXT(event, events);
			if (event->type == DTET_START) {
				struct di_tracker_block_io_context *cur_bctx = DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(event);
				struct di_tracker_io_context *cur_ctx = DT_BLOCK_IO_CTX_TO_IO_CTX(cur_bctx);

				if (!(cur_ctx->bi_rw & WRITE)) {	// found a read start
					BUG_ON(__di_tracker_block_event_is_tracked(&cur_bctx->events[DTET_END]));	// a pending read
					bdt->first_open_read = cur_bctx;
					break;
				}
			}
		}

		// We've advanced the first open read, and it might have a newer (i.e. done later) first
		// write candidate. All writes that were overwritten by the new write candidate for the
		// oldest pending read can be pruned now.
		if (1) {
			struct di_tracker_block_io_context *first_done_write_candidate =
					bdt->first_open_read ?
							bdt->first_open_read->pending_read.first_done_write_candidate :
							bdt->first_done_write_candidate;

			if (first_done_write_candidate)
				__block_di_tracker_prune_all_certainly_overwritten_by(bdt, first_done_write_candidate);
			// else all writes are still candidates
		}
	}

	// Remove the read.
	__block_di_tracker_remove_block_context_event(bdt, bctx, DTET_START);
	__block_di_tracker_remove_block_context_event(bdt, bctx, DTET_END);
}

static void __block_di_tracker_track_write_end(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx)
{
	__block_di_tracker_add_block_context_event(bdt, bctx, DTET_END);

	if (!bdt->first_done_write_candidate) {
		bdt->first_done_write_candidate = bctx;
	}
	else if (__block_di_tracker_is_write_certainly_overwritten_by(
			bdt, bdt->first_done_write_candidate, bctx /* by */)) {

		// first_done_write_candidate is no longer a candidate, find a newer one.
		struct di_tracker_block_event *event = &bctx->events[DTET_START];
		while (true) {
			event = CIRCLEQ_NEXT(event, events);
			if (event->type == DTET_END) {
				struct di_tracker_block_io_context *cur_bctx = DT_BLOCK_EVENT_TO_BLOCK_IO_CTX(event);
				struct di_tracker_io_context *cur_ctx = DT_BLOCK_IO_CTX_TO_IO_CTX(cur_bctx);

				if (cur_ctx->bi_rw & WRITE) {
					bdt->first_done_write_candidate = cur_bctx;
					break;
				}
			}
			BUG_ON(event == CIRCLEQ_LAST(&bdt->events_head));
		}

		if (!bdt->first_open_read) {
			// Write/trim has ended successfully and there are no ongoing reads.
			// All writes/trims which ended before it had started are no longer relevant
			// for any future read.
			__block_di_tracker_prune_all_certainly_overwritten_by(bdt, bctx);
		}
	}
}

static void __block_di_tracker_track_io_end_unsafe(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx)
{
	struct di_tracker_io_context *ctx = DT_BLOCK_IO_CTX_TO_IO_CTX(bctx);

	if (!(ctx->bi_rw & WRITE)) {	// Read
		__block_di_tracker_track_read_end(bdt, bctx, ctx);
	}
	else {	// Write/trim
		__block_di_tracker_track_write_end(bdt, bctx);
	}
}

static void __block_di_tracker_track_io_end(struct block_di_tracker *bdt, struct di_tracker_block_io_context *bctx)
{
	pthread_mutex_lock(&bdt->mutex);

	__block_di_tracker_track_io_end_unsafe(bdt, bctx);

	pthread_mutex_unlock(&bdt->mutex);
}

static void __di_tracker_io_context_track_start(struct di_tracker_io_context *ctx, struct volume_di_tracker *vdit)
{
	int i;

	_NT(trace_di_tracker_io_context_track_start, "@DI_TRACKER_IO_CTX_PTR start", ctx);

	for (i = 0; i < (int)ctx->nlbas; i++)
		__block_di_tracker_track_io_start(&vdit->block_di_trackers[ctx->start_lba + i], &ctx->per_block[i]);
}

static void __di_tracker_io_context_track_end(struct di_tracker_io_context *ctx, struct volume_di_tracker *vdit, int rv)
{
	int i, nlbas = ctx->nlbas;

	_NT(trace_di_tracker_io_context_track_end, "@DI_TRACKER_IO_CTX_PTR end", ctx);

	ctx->bi_rv = rv;

	for (i = 0; i < nlbas; i++)
		__block_di_tracker_track_io_end(&vdit->block_di_trackers[ctx->start_lba + i], &ctx->per_block[i]);
}

struct di_tracker_io_context *volume_di_tracker_track_io_start(struct volume_di_tracker *vdit, u64 start_lba, u64 nlbas, unsigned long bi_rw, struct bio_vec *bi_io_vec, unsigned short bi_vcnt)
{
	struct di_tracker_io_context *ctx;

	pthread_rwlock_rdlock(&vdit->rwlock);

	if (!vdit->enabled)
		return NULL;

	if (vdit->conf->trim_assumed_action == DT_TRIM_NOOP &&
			((bi_rw & (WRITE | REQ_DISCARD)) == (WRITE | REQ_DISCARD)))
		return NULL;

	if (start_lba >= vdit->size)
		return NULL;

	nlbas = min(nlbas, vdit->size - start_lba);

	ctx = __di_tracker_io_context_alloc(start_lba, nlbas, bi_rw, bi_io_vec, bi_vcnt);
	__di_tracker_io_context_track_start(ctx, vdit);

	// Counteract kref_init
	BUG_ON(kref_put(&ctx->kref, __di_tracker_io_context_kref_release));	// ctx cannot disappear here, held by its blocks

	return ctx;
}

void volume_di_tracker_track_io_end(struct volume_di_tracker *vdit, struct di_tracker_io_context *ctx, int rv)
{
	if (ctx) {
		BUG_ON(!vdit->enabled);

		__di_tracker_io_context_track_end(ctx, vdit, rv);	// might free ctx
	}

	pthread_rwlock_unlock(&vdit->rwlock);
}
