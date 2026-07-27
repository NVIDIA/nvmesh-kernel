#include "nvmeibc_block.h" // Must be first for simulator
#include "block/nvmeibc_block_common.h"	// Just for debug of 'tr'
#include "nvmeibc_b_cp_lost_srv_resources.h"

#define pbmp(_bm_) NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE, &(_bm_)	/* printk EC segments bitmap */

void nvmeibc_b_cp_loser_init(struct nvmeibc_b_cp_loser *l)
{
	memset(l, 0, sizeof(*l));		// All bitmaps are zero
	spin_lock_init(&(l)->lock);
}

void nvmeibc_b_cp_loser_clean(struct nvmeibc_b_cp_loser *l)
{
	nvmeibc_b_cp_loser_init(l);
}

static inline bool __is_empty(struct nvmeibs_lost_srv_resource_payload *p)
{
	if (!bitmap_empty(p->bmp, NVMEIB_EC_JOURNAL_MAX_ENTRIES_PER_RANGE))
		return false;
	#ifdef DEBUG_LOSER_CONDITIONS
		return (p->dummy == 0);
	#else
		return true;
	#endif
}

static inline void __assert_loser_correct_seg(struct nvmeibs_lost_srv_resource_payload *p, const struct nvmeibc_subscription_ctx * tr)
{
	#ifdef DEBUG_LOSER_CONDITIONS
		if (p->dummy) {
			WARN((p->dummy != (u32)tr->handle), "nvmeibc bug! LOSER sent to wrong seg %u!=%u\n", (u32)p->dummy, (u32)tr->handle);
		} else { /* This seg was never IOable so nothing could be abandoned */
			const int si = tr->seg;
			WARN(!__is_empty(p), "nvmeibc bug! seg=%d, [%*pbl]\n", si, pbmp(p->bmp));
		}
	#else
		(void)p, (void)tr;
	#endif
}

void nvmeibc_b_cp_loser_destroy(struct nvmeibc_b_cp_loser *l)
{
	const int n_abandon_locks = atomic_read(&l->n_abandon_locks);
	const int n_not_rel_locks = atomic_read(&l->n_not_rel_locks);
	int s;
	WARN((n_abandon_locks|n_not_rel_locks), "nvmeibc bug! n_ab=%d, nnrel=%d\n",
		 n_abandon_locks, n_not_rel_locks);

	for (s = 0; s < N_MAX_RAID_SLICE_LEN; s++) {
		struct nvmeibs_lost_srv_resource_payload *p = &l->abandon_jours[s];
		WARN(!__is_empty(p), "nvmeibc bug! seg=%d, [%*pbl]\n", s, pbmp(p->bmp));
	}
	//spin_lock_destroy(&(l)->lock);
}

void nvmeibc_b_cp_loser_aband_jour(struct nvmeibc_b_cp_loser *l, const struct nvmeibc_subscription_ctx * tr, int jent, u8 jent_gen_id)
{
	const int si = tr->seg;
	struct nvmeibs_lost_srv_resource_payload *p = &l->abandon_jours[si];
	ulong *bmp = p->bmp;
	WARN(test_bit(jent, bmp), "nvmeibc bug! [%*pbl]\n", pbmp(bmp));
	__set_bit(jent, bmp);
	p->gen_ids[jent] = jent_gen_id;
	BUG_ON(jent_gen_id < nvmeib_jrnl_ent_gen_id_min ||
	jent_gen_id > nvmeib_jrnl_ent_gen_id_max);
	__assert_loser_correct_seg(p, tr);
}

void nvmeibc_b_cp_loser_aband_blkset(struct nvmeibc_b_cp_loser *l,
									 struct nvmeibc_cmd_lock *ow)
{
	WARN(1, "nvmeibc bug, not implemented yet, %p, %p\n", l, ow);
}

void nvmeibc_b_cp_loser_aband_dummy(struct nvmeibc_b_cp_loser *l, const struct nvmeibc_subscription_ctx * tr, u32 d)
{
	const int si = tr->seg;
	struct nvmeibs_lost_srv_resource_payload *p = &l->abandon_jours[si];
	#ifdef DEBUG_LOSER_CONDITIONS
		if (p->dummy) {
			_NT(trace_nvmeib_nvmeibc_b_cp_loser_aband_dummy_1, "dummy already_exist: si=@SI, dummy=@LLU", si, p->dummy);
			__assert_loser_correct_seg(p, tr);
		} else {
			_NT(trace_nvmeib_nvmeibc_b_cp_loser_aband_dummy_2, "aband_dummy: si=@SI, old_dummy=@LLU, new_dummy=@LLU", si, p->dummy, tr->handle);
			p->dummy = (u32)tr->handle;
		}
		(void)d;
	#else
		(void)p; (void)d;
	#endif
}


struct nvmeibs_lost_srv_resource_payload *nvmeibc_b_cp_loser_get_seg_report(struct nvmeibc_b_cp_loser *l, const struct nvmeibc_subscription_ctx * tr)
{
	const int si = tr->seg;
	struct nvmeibs_lost_srv_resource_payload *rv = &l->abandon_jours[si];
	bool is_empty = __is_empty(rv);
	//ulong flags;
	//spin_lock_irqsave(&l->lock, flags);
	_NT(trace_nvmeib_nvmeibc_b_cp_loser_seg_clear, "Sending loser report: si=@SI, is_empty=@BOOL", tr->seg, is_empty);
	__assert_loser_correct_seg(rv, tr);
	rv = (is_empty ? NULL : rv);
	//spin_unlock_irqrestore(&l->lock, flags);
	return rv;
}

void nvmeibc_b_cp_loser_seg_clear(struct nvmeibc_b_cp_loser *l, int si)
{
	const int buf_len = (sizeof(l->abandon_jours[si]));
	_ND(debug_nvmeib_nvmeibc_b_cp_loser_seg_clear, "Clearing loser si=@SI", si);
	//ulong flags;
	//spin_lock_irqsave(&l->lock, flags);
	memset(&l->abandon_jours[si], 0, buf_len);
	//spin_unlock_irqrestore(&l->lock, flags);
}

void nvmeibc_b_cp_loser_report_praid(struct nvmeibc_b_cp_loser *l, void*dst_buf)
{
	ulong flags;
	spin_lock_irqsave(&l->lock, flags);
	WARN(1, "nvmeibc bug, not implemented yet, %p, %p\n", l, dst_buf);
	spin_unlock_irqrestore(&l->lock, flags);
}
