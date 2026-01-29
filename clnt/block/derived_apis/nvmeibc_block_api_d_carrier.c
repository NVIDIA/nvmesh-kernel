/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "block/nvmeibc_block_common.h"
#include "block/derived_apis/nvmeibc_block_api_d_carrier.h"

static int __calc_max_amount_of_riders(const struct nvmeibc_block_device *dev)
{
	(void)dev; return 1;						// Unknown value
}

int nvmeibc_api_of_d_carrier_init(struct nvmeibc_block_device *dev)
{
	struct nvmeibc_api_of_d_carrier *c = &dev->c_d_api;
	memset(c, 0, sizeof(*c));
	spin_lock_init(&c->lock);
	c->max_riders = __calc_max_amount_of_riders(dev);
	if (!(c->riders = kzalloc(sizeof(*c->riders) * c->max_riders, GFP_KERNEL)))
		return ENOMEM;
	return 0;
}

void nvmeibc_api_of_d_carrier_destroy(struct nvmeibc_block_device *dev)
{
	struct nvmeibc_api_of_d_carrier *c = &dev->c_d_api;
	WARN_ON(c->n_riders != 0);
	kfree(c->riders);
}

static void __bio_add_pc_page(struct bio *bio, struct page *page, unsigned int len, unsigned int offset)
{
	struct bio_vec *bv = &bio->bi_io_vec[bio->bi_vcnt];
	bv->bv_page = page;
	bv->bv_offset = offset;
	bv->bv_len = len;
	__GET_BI_SIZE(bio) += len;
	bio->bi_vcnt++;
}

int nvmeibc_api_of_d_carrier_mount(struct nvmeibc_block_device *car, struct nvmeibc_block_device *r,
		t_carrier_interrupt_io_perm* io_perm_cb, t_carrier_interrupt_reconf* reconf_cb)
{
	struct nvmeibc_api_of_d_carrier *c = &car->c_d_api;
	ulong flags;
	int rv = 0, i;
	assert_dev_on_mainwq(car);
	BUILD_BUG_ON(sizeof(union nvmeibc_reconf_msg_car2rider) != sizeof(u32));
	WARN_ON(nvmeibc_block_status_is_detaching(car->status));
	car->max_retry_jiffies = 5 * HZ;			// At most 5[sec]. Just arbitrary number. Can be overwritten with ioctl
	spin_lock_irqsave(&c->lock, flags);
	for (i = 0; i < (int)c->max_riders; i++) {
		if (c->riders[i] == NULL) {				// Found free slot
			c->riders[i] = r;
			c->on_io_perm_change_cb = io_perm_cb;
			c->on_reconf_cb = reconf_cb;
			rv = (++c->n_riders);
			_NT(t00_elect, "rider[@INT]=@DEV_NAME, n_riders=@INT", i, r->name, c->n_riders);
			block_api_os_carrier_ref_add(car->os, r->os);
			goto _out;
		}
	}
	_NE(t09_elect, DMESG_PREFIX("@DEV_NAME") ": no free space, n_riders=@INT, max_riders=@INT", r->name, c->n_riders, c->max_riders);
	rv = -ENODEV;
_out:
	spin_unlock_irqrestore(&c->lock, flags);
	return rv;
}

int nvmeibc_api_of_d_carrier_umount(struct nvmeibc_block_device *car, struct nvmeibc_block_device *r)
{
	struct nvmeibc_api_of_d_carrier *c = &car->c_d_api;
	ulong flags;
	int rv = 0, i;
	assert_dev_on_mainwq(car);
	spin_lock_irqsave(&c->lock, flags);
	for (i = 0; i < (int)c->max_riders; i++) {
		if ((c->riders[i]) && !strcmp(c->riders[i]->name, r->name)) {				// Found the rider
			c->riders[i] = NULL;
			rv = (--c->n_riders);
			_NT(t01_elect, "rider[@INT]=@DEV_NAME->NULL, n_riders=@INT", i, r->name, c->n_riders);
			block_api_os_carrier_ref_del(car->os, r->os);
			goto _out;
		}
	}
	_NE(t0a_elect, DMESG_PREFIX("@DEV_NAME") ": cant umount, n_riders=@INT, max_riders=@INT", r->name, c->n_riders, c->max_riders);
	rv = -ENODEV;
_out:
	spin_unlock_irqrestore(&c->lock, flags);
	return rv;
}

static void __c_d_api_do_for_rider_unsafe(struct nvmeibc_api_of_d_carrier *c, void (*do_fn)(struct nvmeibc_block_device *r, void *ctx), void *ctx)
{
	int i;
	for (i = 0; i < (int)c->max_riders; i++) {
		struct nvmeibc_block_device *r = c->riders[i];
		if (r)
			do_fn(r, ctx);
	}
}

// Calls @do_fn for each rider, under c_d_api lock (atomic context!)
void nvmeibc_api_of_d_carrier_do_for_rider(struct nvmeibc_block_device *car, void (*do_fn)(struct nvmeibc_block_device *r, void *ctx), void *ctx)
{
	struct nvmeibc_api_of_d_carrier *c = &car->c_d_api;
	ulong flags;
	spin_lock_irqsave(&c->lock, flags);
	__c_d_api_do_for_rider_unsafe(c, do_fn, ctx);
	spin_unlock_irqrestore(&c->lock, flags);
}

// Calls @do_fn for each rider, under c_d_api lock (atomic context!), if @pred_fn returns true
void nvmeibc_api_of_d_carrier_cond_do_for_rider(struct nvmeibc_block_device *car, bool (*pred_fn)(struct nvmeibc_block_device *car, void *ctx), void (*do_fn)(struct nvmeibc_block_device *r, void *ctx), void *ctx)
{
	struct nvmeibc_api_of_d_carrier *c = &car->c_d_api;
	ulong flags;
	spin_lock_irqsave(&c->lock, flags);
	if (pred_fn(car, ctx))
		__c_d_api_do_for_rider_unsafe(c, do_fn, ctx);
	spin_unlock_irqrestore(&c->lock, flags);
}

#define BUF_ADD(...) pos += scnprintf(buf + pos, len - pos, __VA_ARGS__)
ssize_t nvmeibc_api_of_d_carrier_to_string(struct nvmeibc_block_device *dev, char *buf, size_t len, char fmt)
{
	int pos = 0;
	if (dev != NULL) {
		struct nvmeibc_api_of_d_carrier *c = &dev->c_d_api;
		if (c != NULL) {
			ulong flags;
			int i;
			char *qoute = (fmt == 'H') ? "" : "\"";
			BUF_ADD("%sriders%s: [", qoute, qoute);
			if (c->n_riders) {
				spin_lock_irqsave(&c->lock, flags);
				for (i = 0; i < (int)c->max_riders; i++)
					if (c->riders[i])
						BUF_ADD("%s%s%s,", qoute, c->riders[i]->name, qoute);
				spin_unlock_irqrestore(&c->lock, flags);
				pos--; // Remove last ',' if at least one rider exists
			}
			BUF_ADD("]");
		}
	}
	if (pos == 0) { // Remove 2 positions one is space the other is an unnecessary comma
		pos = -2;
	}
	return pos;
}

/**************************** CARRIER_D IO API ********************************/
#include "../datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.h"
#define __ext_bio_done ((void*)(((u64)os)|CARRIER_BIO))	// Special marker to carrier not to use bio_end_io() but call directly the function below
void rider_bio_endio(struct bio *bio, int rv)
{
	struct bio_extention *b = bio->bi_private;
	const struct nvmeibc_block_device *car = get_bdev_of_bio(bio);
	if (car->dp.enable_di_debug_mode) {
		if (dp_dbgdi_should_add_rider_rdr_info(b))
			dp_dbgdi_do_add_rider_rdr_info(b, car);
	}
	bx_carrier_give_cb_to_rider(b, rv, CAR_BX_CB_REASON_IO_DONE);
}

void rider_bio_init(struct bio *b, struct bio_vec *table, unsigned short max_vecs, sector_t bi_sector, unsigned rw);
void rider_bio_init(struct bio *b, struct bio_vec *table, unsigned short max_vecs, sector_t bi_sector, unsigned rw)
{
	memset(b, 0, sizeof(*b));				// Daniel: Not sure this is valid when using init() for reinit(). Maybe deleteing here important fields ?
	#if KS_BIO_INIT_HAS_BDEV_N_OPF
		bio_init(b, NULL, table, max_vecs, 0);
	#elif KS_BIO_INIT_WITH_BVEC
		bio_init(b, table, max_vecs);
	#else
		bio_init(b);
		b->bi_io_vec = table;
		b->bi_max_vecs = max_vecs;
	#endif
	__GET_BI_SECTOR(b) = bi_sector;
	__SET_BI_RW(b, rw);
}

void rider_bio_send_to_carrier(struct bio *b, struct bio_extention *bx, struct nvmeibc_block_device *carrier)
{
	struct nvmeibc_os_api *os = carrier->os;
	rider_bio_get_bio_extention_from(b) = bx;
	b->bi_end_io = __ext_bio_done;
	bx->exec.rider_rv_on_callback = 0;
	CALL_SUBMIT_BIO_FN(os->atom.queue, os->atom.disk, b);
}

int d_carrier_base_block_io_create_from_bio(struct d_carrier_base_block_io *d, struct bio_part *input_bio, u64 vlba, u32 skip_blks, int n_blks, int rw)
{
	sector_t bi_sector = (vlba << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	struct bio *b = &d->bio;
	bio_iter_t bi = __BI_INIT(input_bio);

	if (!__skip_bio_vec(input_bio, &bi, NVMEIBC_SECTOR2BYTE(skip_blks))) {
		_NE(t10_elect, DMESG_PREFIX() ": nvmeibc bug, could not skip @NLBA blocks", skip_blks);
		return -EINVAL;
	}

	d->table = kzalloc(sizeof(*d->table) * n_blks, GFP_NOFS | __GFP_NOWARN);
	if (!d->table)
		return -ENOMEM;

	rider_bio_init(b, d->table, n_blks, bi_sector, rw);
	while (n_blks--) {
		struct bio_vec bv = __get_bio_vec(input_bio, &bi, NVMEIBC_SECTOR_SIZE);
		if (unlikely(bv.bv_len != NVMEIBC_SECTOR_SIZE)) {
			_NE(t06_elect, DMESG_PREFIX() ": nvmeibc bug, io may stuck. bv_off=@OFF bv_len=@LEN)", bv.bv_offset, bv.bv_len);
			kfree(d->table);
			return -EINVAL;
		}
		__bio_add_pc_page(b, bv.bv_page, bv.bv_len, bv.bv_offset);
	}
	return 0;
}

int d_carrier_base_block_io_create_from_pages(struct d_carrier_base_block_io *d, struct nvmeibc_pages *pages, u64 vlba, u32 skip_blks, int n_blks, int rw)
{
	sector_t bi_sector = (vlba << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	struct bio *b = &d->bio;
	struct nps_block_iter nbi = NPS_BLOCK_ITER_INIT(*pages);

	nps_block_iter_advance(&nbi, skip_blks);
	d->table = kzalloc(sizeof(*d->table) * n_blks, GFP_NOFS | __GFP_NOWARN);
	if (!d->table)
		return -ENOMEM;

	rider_bio_init(b, d->table, n_blks, bi_sector, rw);
	while (n_blks) {
		int n_bv_blks = min((int)nps_block_iter_nblocks(&nbi), n_blks);
		__bio_add_pc_page(b, nps_block_iter_page(&nbi), NVMEIBC_SECTOR2BYTE(n_bv_blks), nps_block_iter_offset(&nbi));
		n_blks -= n_bv_blks;
		nps_block_iter_advance(&nbi, n_bv_blks);
	}
	return 0;
}

int d_carrier_base_block_io_create_from_vpges(struct d_carrier_base_block_io *d, struct page **pages, u64 vlba, u32 skip_pages, int n_blks, int rw)
{	// Unlke previous function, this is vmap pages so we cannot assume continuation, convert each page bv_page
	sector_t bi_sector = (vlba << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	struct bio *b = &d->bio;
	int i;
	d->table = kzalloc(sizeof(*d->table) * n_blks, GFP_NOFS | __GFP_NOWARN);
	if (!d->table)
		return -ENOMEM;
	rider_bio_init(b, d->table, n_blks, bi_sector, rw);
	for (i = 0; i < n_blks; i++)
		__bio_add_pc_page(b, pages[skip_pages+i], PAGE_SIZE, 0);
	return 0;
}

void d_carrier_base_block_io_reinit(struct d_carrier_base_block_io *d, u64 vlba, int rw)
{
	const sector_t bi_sector = (vlba << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	__GET_BI_SECTOR(&d->bio) = bi_sector;
	__SET_BI_RW(&d->bio, rw);
}

void d_carrier_base_block_io_destroy(struct d_carrier_base_block_io *d)
{
	kfree(d->table);
	d->table = NULL;
}

u64 d_carrier_base_block_get_vlba(const struct d_carrier_base_block_io *d)
{
	return (__GET_BI_SECTOR(&d->bio) >> KERNEL_SECTOR_TO_SECTOR_SHIFT);
}

void d_carrier_base_block_io_set_cb(struct d_carrier_base_block_io *d, rider_bio_cb_t fn, void *ctx)
{
	d->bext.cb.fn = fn;
	d->bext.cb.riders_ctx = ctx;
	WARN_ON(!fn || !ctx);
}

void d_carrier_base_block_io_execute(struct d_carrier_base_block_io *d, struct nvmeibc_block_device *dev, struct operation *o)
{
	if (dp_dbgdi_should_add_rider_info(o))
		dp_dbgdi_do_add_rider_info(d, dev, o);
	rider_bio_send_to_carrier(&d->bio, &d->bext, dev);
}

/*************************** CARRIER_MD IO API ********************************/
// Todo, eventually move to separate file
static void __md_carrier_init_bio(struct md_carrier_base_block_io *md, const u64 vlba, int rw)
{
	const sector_t bi_sector = (vlba << KERNEL_SECTOR_TO_SECTOR_SHIFT);
	struct bio *b = &md->bio;
	rider_bio_init(b, md->table, ARRAY_SIZE(md->table), bi_sector, rw);
	__bio_add_pc_page(b, md->page, PAGE_SIZE, 0);
}

int md_carrier_base_block_io_create(struct md_carrier_base_block_io *md, const u64 vlba, int rw)
{
	WARN_ON(md->page);                                      // Allready allocated, probably wrong usage
	if ((md->page = alloc_pages(GFP_ATOMIC, 0)) == NULL)	// Possibly atomic context: same reason as 'so' of syncs is allocated atomically
		return -ENOMEM;
	__md_carrier_init_bio(md, vlba, rw);
	return 0;
}

void md_carrier_base_block_io_reinit(struct md_carrier_base_block_io *md, int rw)
{
	const u64 vlba = (__GET_BI_SECTOR(&md->bio) >> KERNEL_SECTOR_TO_SECTOR_SHIFT);		// Daniel: Currently reinit does not require to change VLBA
	__md_carrier_init_bio(md, vlba, rw); // Consider: Just update from READ to WRITE in bio instead + reset some atomics, instead of full reinit
}

void md_carrier_base_block_io_set_cb(struct md_carrier_base_block_io *md, rider_bio_cb_t fn, void *ctx)
{
	md->bext.cb.fn = fn;
	md->bext.cb.riders_ctx = ctx;
	WARN_ON(!fn || !ctx);
}

void md_carrier_base_block_io_destroy(struct md_carrier_base_block_io *md)
{
	if (md->page) {
		__free_pages(md->page, 0);
		md->page = NULL;
	}
}

void md_carrier_base_block_io_execute(struct md_carrier_base_block_io *md, struct nvmeibc_block_device *dev)
{
	rider_bio_send_to_carrier(&md->bio, &md->bext, dev);
}

void* md_carrier_base_block_io_get_data(struct md_carrier_base_block_io *md)
{
	return page_address(md->table[0].bv_page);
}

#define NVMEIBC_ELECT_POC_SEPARATE_MTV_AND_QLC_MD_LBAS (0)

/*************************** QLC_DRV API ********************************/
// Todo, eventually move to separate file

#define MDV_BLKSET_NLBAS 32
#define N_MD_ENTRY_BLOCKS (1 + NVMEIBC_ELECT_POC_SEPARATE_MTV_AND_QLC_MD_LBAS)	// POC: entry is a block pair of MTV MD + QLC MD, GA: both share the same block
#define N_MD_ENTRIES_IN_MDV_BLKSET (MDV_BLKSET_NLBAS / N_MD_ENTRY_BLOCKS)

#if defined(BLKDEV_SIMULATOR) && (BLKDEV_SIMULATOR==1)
	// Algorithm 2: Identity permutation. Used in simulator, because we dont care about contention
	#define nvmeibc_convert_vlba_blkset_to_md_entry(vlba_blkset, qlc_topo) vlba_blkset
#else
	#if 0
		// Algorithm 1: Permutation applied to reduce contention on se mdv during sequential writes. Round robin fill blocks of P blocksets (prime P).  Blocks {0,1} in 37 blocksets, then blocks {2,3} in those blocksets, etc...
		#define N_MDV_BLKSETS_PER_PSEUDO_CHUNK 37	// Must be larger than write queue depth and relatively prime to the expected distance from multiple write heads
		#define N_MTV_BLKSETS_PER_PSEUDO_CHUNK (N_MD_ENTRIES_IN_MDV_BLKSET * N_MDV_BLKSETS_PER_PSEUDO_CHUNK)

		#define nvmeibc_pseudo_chunk_first_md_entry(vlba_blkset) \
			(N_MTV_BLKSETS_PER_PSEUDO_CHUNK * (vlba_blkset / N_MTV_BLKSETS_PER_PSEUDO_CHUNK))

		#define nvmeibc_convert_vlba_blkset_to_md_entry(vlba_blkset, qlc_topo) \
				(nvmeibc_pseudo_chunk_first_md_entry(vlba_blkset) + \
						(N_MD_ENTRIES_IN_MDV_BLKSET * ((vlba_blkset - nvmeibc_pseudo_chunk_first_md_entry(vlba_blkset)) % N_MDV_BLKSETS_PER_PSEUDO_CHUNK)) + \
						((vlba_blkset - nvmeibc_pseudo_chunk_first_md_entry(vlba_blkset)) / N_MDV_BLKSETS_PER_PSEUDO_CHUNK))
	#else
		// Algorithm 3: Same as 2, but round robin in each QLC chunk instead of in 37 blocksets pseudo-chunk
		#define nvmeibc_convert_vlba_blkset_to_md_entry(vlba_blkset, qlc_topo) __convert_vlba_blkset_to_md_entry(vlba_blkset, qlc_topo)
	#endif
#endif

static u64 __attribute__((unused)) __convert_vlba_blkset_to_md_entry(vlba_blkset_t vlba_blkset, struct nvmeibc_topology *qlc_topo)
{
	const u64 qlc_blkset_nlbas = qlc_topo->chunks->raid1s->slice_size * LOCKSET_SLICES;
	const u64 blkset_start_vlba = vlba_blkset * qlc_blkset_nlbas;
	const int c = nvmeibc_get_chunk_ind_of_lba(blkset_start_vlba, qlc_topo);
	const u64 chunk_nlbas = qlc_topo->chunks[c + 1].first_vlba - qlc_topo->chunks[c].first_vlba;
	const vlba_blkset_t chunk_blksets = chunk_nlbas / qlc_blkset_nlbas;
	const vlba_blkset_t chunk_start_blkset = qlc_topo->chunks[c].first_vlba / qlc_blkset_nlbas;
	const vlba_blkset_t blkset_in_chunk = vlba_blkset - chunk_start_blkset;
	const uint n_full_rounds = chunk_blksets % N_MD_ENTRIES_IN_MDV_BLKSET;
	const uint partial_round_entries = chunk_blksets / N_MD_ENTRIES_IN_MDV_BLKSET;
	const uint full_round_entries = partial_round_entries + 1;
	const uint partial_rounds_start_blkset = n_full_rounds * full_round_entries;
	vlba_blkset_t md_entry_offset;
	WARN_ON(qlc_topo->chunks[c].first_vlba % qlc_blkset_nlbas != 0);

	if (blkset_in_chunk < partial_rounds_start_blkset) {
		md_entry_offset = (blkset_in_chunk % full_round_entries) * N_MD_ENTRIES_IN_MDV_BLKSET + (blkset_in_chunk / full_round_entries);
	} else {
		const vlba_blkset_t blkset_in_partial_rounds = blkset_in_chunk - partial_rounds_start_blkset;
		md_entry_offset = (blkset_in_partial_rounds % partial_round_entries) * N_MD_ENTRIES_IN_MDV_BLKSET + n_full_rounds + (blkset_in_partial_rounds / partial_round_entries);
	}

	return chunk_start_blkset + md_entry_offset;
}

u64 nvmeibc_convert_vlba_blkset_to_mdv_vlba(vlba_blkset_t vlba_blkset, struct nvmeibc_topology *qlc_topo, bool is_mtv)
{
	#if NVMEIBC_ELECT_POC_SEPARATE_MTV_AND_QLC_MD_LBAS
		return (nvmeibc_convert_vlba_blkset_to_md_entry(vlba_blkset, qlc_topo) * N_MD_ENTRY_BLOCKS + is_mtv);		// POC: QLC-MD and MTV-MD reside on different blocks. This is the encoding
	#else
		(void)is_mtv;
		(void)qlc_topo;
		return nvmeibc_convert_vlba_blkset_to_md_entry(vlba_blkset, qlc_topo);		// QLC-MD and MTV-MD reside on the same block
	#endif
}

