/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

// For documentation, see Header in H file
/*****************************************************************************/
#include "nvmeibs_main_sim.h"
#include "./uni_framework/bunitest_conf.h"
#include "nvmeibc_block.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "block/datapath_utils_generic/nvmeibc_block_dp_common.h"

/********************* NVME disk internal representation **********************/
#define RAMDISK_PARTITION_ALIGNMENT (1<<NVMEIB_EC_JOURNAL_SECTOR_SHIFT)

// we need extra bytes before/after a RamDisk to allow tests to place guards before/after a segment/extent without corrupting any data.
#define RAM_DISK_EXTRA_BYTES			32				// allow these many bytes before/after the drive
#define ram_disk_memory_prefix_signature    0x10
#define ram_disk_memory_postfix_signature   0x20

int ramDiskSimulator_init_server_side(struct ramDiskSimulator* _this){
	/* Currently only the fields that are actually in use are initialized, i.e. required for serjio fields. Add more data here if needed by other server side components. */
	/* RRRR: Eliminate data duplication between server_disk and upper level ramdisk, use server_disk only.*/
	_this->server_disk.di.priv = &_this->server_disk.pd;

	_this->server_disk.di.mtdt_extd = false;
	_this->server_disk.di.blocks = _this->committed_addr_end.byte >> _this->sector_shift;
	_this->server_disk.di.hw_blocks = _this->server_disk.di.blocks;
	_this->server_disk.di.metadata = 8;
	_this->server_disk.di.block_shift = _this->sector_shift;
	_this->server_disk.di.block_size = 1 << _this->sector_shift;
	strcpy(_this->server_disk.di.disk_id, container_of(_this, struct serverSimulator, ramDisk)->hardware->disk_name);
	return 0;
}

#define NVMEIB_EC_JOURNAL_SIZE 	(NVMEIB_EC_TOTAL_JOURNAL_BLKS << NVMEIB_EC_JOURNAL_SECTOR_SHIFT) // not including meta-data

struct ramDiskCommitedAddress __calc_committed_addr(u64 byte_addr, bool check_alignment){
	if (check_alignment) {
		BUG_ON(byte_addr % BYTES_IN_LOCKSET);
	}
	return (struct ramDiskCommitedAddress){
		.byte = byte_addr,
		.sector = NVMEIBC_BYTE2SECTOR(byte_addr),
		.lock = byte_addr / BYTES_IN_LOCKSET,
		.block = __bytesTo4K(byte_addr)
	};
}

int ramDiskSimulator_init(struct ramDiskSimulator* _this, int uniqueID){
	int nLocks = ARRAY_SIZE(_this->locks), i;
	_this->state = ramDisk_running;
	_this->uniqueID	= uniqueID;
	_this->sector_shift	= NVMEIBC_SECTOR_SHIFT - 3 + (uniqueID % 4);	// Disk is formatted to psudo-rand sector size between 1 to 1/8 of logical block
	_this->md_size = (DISK_MIN_MD_SIZE_BYTE << (NVMEIBC_SECTOR_SHIFT - _this->sector_shift));		// At least 8[bytes] for each NVMEIBC_SECTOR_SHIFT[block]. At most 64[bytes]
	_this->max_dma_size = BYTES_IN_LOCKSET*(uniqueID ? 113 : 1);	// Must be at least size of a blockset, ensure disk 0 has the limit of some Intel SSDs, finds bugs
	_this->data_seg_size = ((u64)nLocks)*BYTES_IN_LOCKSET;
	_this->use_ec_stale_locks = false;

	BUILD_BUG_ON(ALIGN(NVMEIB_EC_JOURNAL_SIZE, RAMDISK_PARTITION_ALIGNMENT) != NVMEIB_EC_JOURNAL_SIZE);
	BUG_ON(      ALIGN(_this->data_seg_size  , RAMDISK_PARTITION_ALIGNMENT) != _this->data_seg_size);
	_this->committed_addr = __calc_committed_addr(24ull * (1ull << 40), true); //24TB; During GDB session use hex view
	_this->committed_disk_size = _this->data_seg_size + NVMEIB_EC_JOURNAL_SIZE + NVMEIB_EC_SERJIO_DB_SIZE; // units of [bytes]
	_this->committed_addr_end = __calc_committed_addr(24ull * (1ull << 40) + _this->committed_disk_size, false);

	_this->c.id_gen	= 7 + uniqueID;					// Start from psudo random cid
	spin_lock_init(&_this->cmpxchg_lock);
	memset(_this->locks, 0, sizeof(_this->locks));
	_this->_mem	= (u8*)sim_kzalloc(_this->committed_disk_size + 2*RAM_DISK_EXTRA_BYTES + RAMDISK_PARTITION_ALIGNMENT, 0);
	_this->mem = (u8*)(ALIGN((size_t)(_this->_mem + RAM_DISK_EXTRA_BYTES), RAMDISK_PARTITION_ALIGNMENT));
	_this->serjio.jranges_start = _this->mem + _this->data_seg_size;
	_this->serjio.db_start = _this->serjio.jranges_start + NVMEIB_EC_JOURNAL_SIZE;

	// set disk signature
	for (i=0; i < RAM_DISK_EXTRA_BYTES; i++) {
		_this->mem[              -i-1] = ram_disk_memory_prefix_signature  + i;
		_this->mem[_this->committed_disk_size+i] = ram_disk_memory_postfix_signature + i;
	}
	spin_lock_init(&_this->mem_lock);
	_this->_blk_md = sim_kzalloc(ramDiskSimulator_n_metadatas(_this) * _this->md_size, GFP_KERNEL);
	memset(_this->_blk_md, 0xff, ramDiskSimulator_n_metadatas(_this) * _this->md_size); // Format MD exactly like TOMA before volume is created
	ramDiskSimulator_format_metadata(_this, false);			// By default do not support metadata
	ramDiskSimulator_init_server_side(_this);
	return 0;
}

void ramDiskSimulator_verify_no_locks(struct ramDiskSimulator* _this){
	u32 i, nTaken, nLocks = ARRAY_SIZE(_this->locks);
	const u64 stale_mask = nvmeib_stale_bit_mask_ec.all;
	for (i=0, nTaken=0; i<nLocks; i++){								// Verify that all the locks were released
		if (_this->locks[i] != LS_UNLOCKED) {
			const u64 is_stale = (_this->locks[i] & stale_mask);
			_Emerg("Disk:%d, lock %d is %s locked by 0x%x\n", _this->uniqueID, i, (is_stale ? "stale" : "locked by"), _this->locks[i]);
			nTaken++;
			BUG_ON(nTaken);
		}
	}
}

void ramDiskSimulator_destroy(struct ramDiskSimulator* _this){
	int i;
	// verify disk signature
	for (i=0; i < RAM_DISK_EXTRA_BYTES; i++) {
		BUG_ON(_this->mem[              -i-1] != ram_disk_memory_prefix_signature + i);
		BUG_ON(_this->mem[_this->committed_disk_size+i] != ram_disk_memory_postfix_signature + i);
	}
	ramDiskSimulator_verify_no_locks(_this);
	spin_lock_destroy(&_this->cmpxchg_lock);
	spin_lock_destroy(&_this->mem_lock);
	sim_kfree(_this->_mem);
	_this->_mem = NULL;
	_this->mem = NULL;
	ramDiskSimulator_format_metadata(_this, true);
	sim_kfree(_this->_blk_md);
	_this->_blk_md = NULL;
}

void ramDiskSimulator_disconnect(struct ramDiskSimulator*_this) {
	spin_lock(&_this->cmpxchg_lock);
	_this->state |= ramDisk_down;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_reconnect(struct ramDiskSimulator*_this){
	spin_lock(&_this->cmpxchg_lock);
	_this->state &= (~ramDisk_down);
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_break(struct ramDiskSimulator*_this, short err_code){
	spin_lock(&_this->cmpxchg_lock);
	_this->state |= ramDisk_broken;
	_this->error_code = err_code;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_fail_data(struct ramDiskSimulator*_this){
	spin_lock(&_this->cmpxchg_lock);
	_this->state |= ramDisk_fail_data;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_fix_data(struct ramDiskSimulator*_this){
	spin_lock(&_this->cmpxchg_lock);
	_this->state &= (~ramDisk_fail_data);
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_fix(struct ramDiskSimulator*_this){
	spin_lock(&_this->cmpxchg_lock);
	_this->state &= (~ramDisk_broken);
	_this->error_code = 0;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_setrdma(struct ramDiskSimulator*_this, bool enable){
	spin_lock(&_this->cmpxchg_lock);
	if (!enable) _this->state |=   ramDisk_no_rdma;
	else         _this->state &= (~ramDisk_no_rdma);
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_get_serjio_partions_sectors_ranges(struct ramDiskSimulator *self, u64* jrnl_start, u64* jrnl_length, u64* db_start, u64* db_length) {
	*jrnl_start = ((u64)(self->serjio.jranges_start - self->mem + self->committed_addr.byte) >> self->sector_shift);
	*jrnl_length = (NVMEIB_EC_JOURNAL_SIZE >> self->sector_shift);
	*db_start = ((u64)(self->serjio.db_start - self->mem + self->committed_addr.byte) >> self->sector_shift);
	*db_length = (NVMEIB_EC_SERJIO_DB_SIZE >> self->sector_shift);
}

void ramDiskSimulator_wipe(struct ramDiskSimulator* _this, u8 val){
	memset(_this->mem, val, _this->committed_disk_size);
}

void ramDiskSimulator_wipeRange(struct ramDiskSimulator* _this, u64 dlba_, u64 size, u64 val)
{
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u64 range_start = NVMEIBC_SECTOR2BYTE(dlba);
	const u64 range_size_in_bytes = NVMEIBC_SECTOR2BYTE(size);
	BUG_ON(range_start + range_size_in_bytes > _this->committed_disk_size);
	memset(_this->mem + range_start, val, range_size_in_bytes);
}

void ramDiskSimulator_wipeMDRange(struct ramDiskSimulator* D, u64 dlba_, u64 size, const void *md){
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	u64	i, end = dlba + size;
	for (i = dlba; i < end; i++)
		memcpy(__ptr_to_ith_md(D, i), md, D->md_size);
}

#define meta_data_ptr_poison  ((1ULL << 63) | 1ULL)
void ramDiskSimulator_format_metadata(struct ramDiskSimulator *ram, bool enable_metadata) {
	if (!enable_metadata)
		ram->_blk_md = (void*)((u64)ram->_blk_md |   meta_data_ptr_poison);
	else
		ram->_blk_md = (void*)((u64)ram->_blk_md & (~meta_data_ptr_poison));	// Remove poison
}

bool ramDiskSimulator_has_metadata(const struct ramDiskSimulator *ram) {
	return ((u64)ram->_blk_md&0x1) == 0;		// If ptr%2 == 1, it is illegal so no metadata
}

void ramDiskSimulator_mark_ec(struct ramDiskSimulator *D, bool is_ec) {
	D->use_ec_stale_locks = is_ec;
}

void*ramDiskSimulator_get_metadataptr_unsafe(const struct ramDiskSimulator *D, u64 dlba_) {
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	return &((u8*)((u64)D->_blk_md & (~meta_data_ptr_poison)))[D->md_size*dlba];  // Todo: Daniel, Revisit the functions above and unite them into a prettier mechanism
}

void* ramDiskSimulator_get_metadataptr(const struct ramDiskSimulator *D, u64 dlba_) {
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	return __ptr_to_ith_md(D, dlba);
}

void* ramDiskSimulator_get_metadataptr_jblk(const struct ramDiskSimulator *D, u64 dlba_) {
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	return __ptr_to_ith_md(D, dlba + (D->data_seg_size >> 12));
}

const void *nvmeib_jmd_unused_entry_md_max(void) {
	static u8 rv[DISK_MAX_MD_SIZE_BYTE] = {[0 ... DISK_MAX_MD_SIZE_BYTE - 1] = DISK_MD_INIT_BYTE,};	// Entire MD is initialized to (~0)
	((union jblock_md*)((void*)rv))->raw = nvmeib_jmd_unused_entry_val.raw;
	/*static struct nvmeib_jmd_md_max {
		union jblock_md jmdc_val;
		u8 pad[DISK_MAX_MD_SIZE_BYTE - sizeof(union jblock_md)];
	} __attribute__((packed)) rv = { .jmdc_val.raw = nvmeib_jmd_unused_entry_val.raw, .pad = {[0 ... DISK_MAX_MD_SIZE_BYTE - sizeof(union jblock_md) - 1] = DISK_MD_INIT_BYTE,},};*/
	return rv;
}

void ramDiskSimulator_wipeMD(struct ramDiskSimulator* D, const void	*md){
	ramDiskSimulator_wipeMDRange(D, D->committed_addr.sector, ramDiskSimulator_n_metadatas(D), md);
}

#define __s2d_md(dlba) ((dlba) >> (NVMEIBC_SECTOR_SHIFT - D->sector_shift))
void ramDiskSimulator_wipeMD_jour(struct ramDiskSimulator* D, const void *md){
	u64 jrnl_start, jrnl_length, db_start, db_length;
	ramDiskSimulator_get_serjio_partions_sectors_ranges(D, &jrnl_start, &jrnl_length, &db_start, &db_length);
	ramDiskSimulator_wipeMDRange(D, __s2d_md(jrnl_start), __s2d_md(jrnl_length), md);
}

void ramDiskSimulator_wipeMD_serjioDB(struct ramDiskSimulator* D, const void *md){
	u64 jrnl_start, jrnl_length, db_start, db_length;
	ramDiskSimulator_get_serjio_partions_sectors_ranges(D, &jrnl_start, &jrnl_length, &db_start, &db_length);
	ramDiskSimulator_wipeMDRange(D, __s2d_md(db_start), __s2d_md(db_length), md);
}

void ramDiskSimulator_MD_read(struct ramDiskSimulator* D, u64 dlba_, u64 nlbas, void *md) {
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	const u8 *cur = __ptr_to_ith_md(D, dlba);
	BUG_ON(dlba + nlbas > ramDiskSimulator_n_metadatas(D));
	memcpy(md, cur, D->md_size * nlbas);
}

void ramDiskSimulator_MD_write(struct ramDiskSimulator* D, u64 dlba_, u64 nlbas, const void *md) {
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	u8 *cur = __ptr_to_ith_md(D, dlba);
	BUG_ON(dlba + nlbas > ramDiskSimulator_n_metadatas(D));
	memcpy(cur, md, D->md_size * nlbas);
}

void ramDiskSimulator_MD_set(struct ramDiskSimulator* D, u64 dlba_, u64 nlbas, int val) {
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	u8 *cur = __ptr_to_ith_md(D, dlba);
	BUG_ON(dlba + nlbas > ramDiskSimulator_n_metadatas(D));
	memset(cur, val, D->md_size * nlbas);
}

void ramDiskSimulator_wipe_dirty_bits(struct ramDiskSimulator* _this, u8 val) {
	memset(_this->dbits, val, sizeof(_this->dbits));
}

void ramDiskSimulator_verify_no_dirty_bits(struct ramDiskSimulator* _this) {
	for (u32 i=0; i<(u32)ARRAY_SIZE(_this->dbits); i++)
		BUG_ON(_this->dbits[i].all_bits);
}

void ramDiskSimulator_verify_no_other_dirty_bits(struct ramDiskSimulator* _this, const union nvmeibc_dbits_entry dbits) {
	for (u32 i=0; i<(u32)ARRAY_SIZE(_this->dbits); i++)
		BUG_ON(_this->dbits[i].all_bits && dbits.all_bits != _this->dbits[i].all_bits);
}

void ramDiskSimulator_set_lock(struct ramDiskSimulator*_this, u64 dlba_, u32 lock_id){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u64 i = dlba/LOCKSET_4KS;
	spin_lock(&_this->cmpxchg_lock);
	_this->locks[i] = lock_id;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_lockStale(struct ramDiskSimulator*_this, u64 dlba) {
	const u64 default_stale = (_this->use_ec_stale_locks ? nvmeib_stale_bit_mask_ec.lock_id.all /*Stale zero*/ : nvmeib_stale_special_raid1.lock_id.all /* Stale special */);
	ramDiskSimulator_set_lock(_this, dlba, default_stale);
	if (_this->use_ec_stale_locks) {
		tomaSimulator_set_stale_lock(serverSimulator_get_toma_by_ram(_this), dlba, default_stale);
	}
}

void ramDiskSimulator_lockStaleRO_EC(struct ramDiskSimulator*_this, u64 dlba) {
	const union nvmeib_lock_blkset_entry ec_ro_stale = {{ .lock_id = { .bits = {                        .is_read = 1,    .is_stale = 1, }}}};
	const u64 default_stale = ec_ro_stale.lock_id.all;
	BUG_ON(!_this->use_ec_stale_locks);
	ramDiskSimulator_set_lock(_this, dlba, default_stale);
	tomaSimulator_set_stale_lock(serverSimulator_get_toma_by_ram(_this), dlba, default_stale);
}

void ramDiskSimulator_set_unlock(struct ramDiskSimulator*_this, u64 dlba){
	ramDiskSimulator_set_lock(_this, dlba, LS_UNLOCKED);
	tomaSimulator_unset_stale_lock(serverSimulator_get_toma_by_ram(_this), dlba);
}

void ramDiskSimulator_setDirty(struct ramDiskSimulator*_this, u64 dlba_, u32 val){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u32 i = dlba/LOCKSET_4KS;
	spin_lock(&_this->cmpxchg_lock);
	_this->dbits[i].all_bits = val;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_DirtyVerifyAndClean(struct ramDiskSimulator*_this, u64 dlba_, u32 val) {
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u32 i = dlba/LOCKSET_4KS;
	spin_lock(&_this->cmpxchg_lock);
	BUG_ON(_this->dbits[i].all_bits != val);
	_this->dbits[i].all_bits = 0;
	spin_unlock(&_this->cmpxchg_lock);

}

void ramDiskSimulator_lockUnSta(struct ramDiskSimulator*_this, u64 dlba_){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u32 i = dlba/LOCKSET_4KS;
	const u64 stale_mask = nvmeib_stale_bit_mask_ec.all;
	spin_lock(&_this->cmpxchg_lock);
	WARN(((_this->locks[i]&stale_mask) == 0ULL), "Disk %d, Lock %d cant unstale: 0x%x\n", _this->uniqueID, i, _this->locks[i]);
	_this->locks[i] = LS_UNLOCKED;
	spin_unlock(&_this->cmpxchg_lock);
	if (_this->use_ec_stale_locks) {
		tomaSimulator_unset_stale_lock(serverSimulator_get_toma_by_ram(_this), dlba_);
	}
}

bool ramDiskSimulator_is_locked_by(struct ramDiskSimulator*_this, u64 dlba_, u32 lock_id){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u32 i = dlba/LOCKSET_4KS;
	bool rv;
	spin_lock(&_this->cmpxchg_lock);
	rv = (_this->locks[i] == lock_id);
	spin_unlock(&_this->cmpxchg_lock);
	return rv;
}

bool ramDiskSimulator_lockIsSta(struct ramDiskSimulator*_this, u64 dlba_){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u32 i = dlba/LOCKSET_4KS;
	bool rv;
	spin_lock(&_this->cmpxchg_lock);
	rv = ((_this->locks[i] & nvmeib_stale_bit_mask_ec.all) != 0ULL);
	spin_unlock(&_this->cmpxchg_lock);
	return rv;
}

bool ramDiskSimulator_is_locked(struct ramDiskSimulator*_this, u64 dlba){
	return !ramDiskSimulator_is_locked_by(_this, dlba, LS_UNLOCKED);
}

int ramDiskSimulator_clean_lock( struct ramDiskSimulator*_this, u32 lock_id){
	u32 i, nLocks = ARRAY_SIZE(_this->locks), n_cleaned = 0;
	spin_lock(&_this->cmpxchg_lock);
	for (i=0; i<nLocks; i++){
		if (_this->locks[i] == lock_id) {
			_this->locks[i] = LS_UNLOCKED;
			tomaSimulator_unset_stale_lock(serverSimulator_get_toma_by_ram(_this), i*LOCKSET_4KS + _this->committed_addr.block);
			n_cleaned++;
		}
	}
	spin_unlock(&_this->cmpxchg_lock);
	return n_cleaned;
}

int ramDiskSimulator_CleanSta( struct ramDiskSimulator*_this){
	u64 i, nLocks = ARRAY_SIZE(_this->locks), n_cleaned = 0;
	const u64 is_stale = nvmeib_stale_bit_mask_ec.all;
	spin_lock(&_this->cmpxchg_lock);
	for (i=0; i<nLocks; i++){
		if (_this->locks[i]&is_stale) {
			_this->locks[i] = LS_UNLOCKED;
			tomaSimulator_unset_stale_lock(serverSimulator_get_toma_by_ram(_this), i*LOCKSET_4KS + _this->committed_addr.block);
			n_cleaned++;
		}
	}
	spin_unlock(&_this->cmpxchg_lock);
	return (int)n_cleaned;
}

int ramDiskSimulator_CleanROL( struct ramDiskSimulator*_this){
	int i, nLocks = ARRAY_SIZE(_this->locks), n_cleaned = 0;
	const union nvmeib_lock_blkset_entry is_read_mask ={{.lock_id = {.bits = {.is_read = 1}}}};
	spin_lock(&_this->cmpxchg_lock);
	for (i=0; i<nLocks; i++){
		if (_this->locks[i]&is_read_mask.all) {
			_this->locks[i] = LS_UNLOCKED;
			n_cleaned++;
		}
	}
	spin_unlock(&_this->cmpxchg_lock);
	return n_cleaned;
}

void ramDiskSimulator_lockDo(   struct ramDiskSimulator*_this, u64 dlba_){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u64 i = dlba/LOCKSET_4KS;
	spin_lock(&_this->cmpxchg_lock);
	BUG_ON(_this->locks[i] != LS_UNLOCKED);
	_this->locks[i] = SIMULATOR_OTHER_CLIENT_LOCK_ID;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_lockUn(struct ramDiskSimulator*_this, u64 dlba_){
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	const u64 i = dlba/LOCKSET_4KS;
	spin_lock(&_this->cmpxchg_lock);
	if (_this->locks[i]!=SIMULATOR_OTHER_CLIENT_LOCK_ID){
		_Emerg("Disk %d, Lock %llu of other client was overwritten with value 0x%x\n", _this->uniqueID, i, _this->locks[i]);
		BUG_NOT_IMPLEMENTED_YET;
	}
	_this->locks[i] = LS_UNLOCKED;
	spin_unlock(&_this->cmpxchg_lock);
}

void ramDiskSimulator_reset_txid(struct ramDiskSimulator *ram)
{
	u32 i;
	for (i=0; i < ARRAY_SIZE(ram->TxIDs); i++)
		ram->TxIDs[i] = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;	// Todo: Currently we don't have mechanism of to handle cold recovery
}

void ramDiskSimulator_reset_txid_range(struct ramDiskSimulator *ram, const u32 start, const u32 end)
{
	u32 i;
	BUG_ON(end > ARRAY_SIZE(ram->TxIDs));
	for (i = start; i < end; i++)
		ram->TxIDs[i] = NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS;	// Todo: Currently we don't have mechanism of to handle cold recovery
}

void ramDiskSimulator_read_txid(const struct ramDiskSimulator *ram, u32 tx_id[], u64 lock_index_, u32 size)
{
	const u64 lock_index = COMMITTED_ADDR(ram, lock_index_, LOCK);
	BUILD_BUG_ON(sizeof(*tx_id) != sizeof(*ram->TxIDs));
	BUG_ON(lock_index + size > RAMDISK_DATA_LOCK_SIZE);
	memcpy(tx_id, &ram->TxIDs[lock_index], sizeof(*tx_id) * size);
}

void ramDiskSimulator_AllowUnlockedIO(struct ramDiskSimulator* _this, bool permit){
	_this->f_allowIOwithoutLocks=permit;
}

static u64 *ramDiskSimulator_sector_head(struct ramDiskSimulator* _this, u64 dlba_) {
	const u64 dlba = COMMITTED_ADDR(_this, dlba_, SECTOR);
	return (u64*)(&(_this->mem[NVMEIBC_SECTOR2BYTE(dlba)]));
}

#define RAMDISK_BLOCK_READ_FAIL_PATRN 0xBAAD0F0000000000ULL // indicate in the start of block that it is a bad sector
#define RAMDISK_BLOCK_READ_FAIL_MASK  0xFFFFFF0000000000ULL

void ramDiskSimulator_do_bad_sector_with_ptr(u64 *sec_start, u64 *md, short error_code) {
	u64 code64 = error_code;
	*sec_start = RAMDISK_BLOCK_READ_FAIL_PATRN | code64;
	if (md)
		*md = ~0ULL;        // Destroy the metadata as well (Turn on first 8 bytes)
}

void ramDiskSimulator_do_bad_sector(struct ramDiskSimulator* D, u64 dlba_, short error_code){
	const u64 dlba = COMMITTED_ADDR(D, dlba_, SECTOR);
	u64 *sec_start = ramDiskSimulator_sector_head(D, dlba_);
	u64 *md = NULL;
	if (ramDiskSimulator_has_metadata(D))
		md = (u64*)__ptr_to_ith_md(D, dlba);
	ramDiskSimulator_do_bad_sector_with_ptr(sec_start, md, error_code);
}

void ramDiskSimulator_un_bad_sector(struct ramDiskSimulator* _this, u64 dlba_){
	u64 *sec_start = ramDiskSimulator_sector_head(_this, dlba_);
	BUG_ON(!ramDiskSimulator_is_bad_sector(_this, dlba_));
	*sec_start = (u64)0;
}

short ramDiskSimulator_is_block_bad_sector(const u64 *sec_start) {
	short ret;
	if ((*sec_start & RAMDISK_BLOCK_READ_FAIL_MASK) == RAMDISK_BLOCK_READ_FAIL_PATRN)
		ret = *sec_start & 0XFFFF;
	else
		ret = 0;
	return ret;
}


short ramDiskSimulator_is_bad_sector(struct ramDiskSimulator* _this, u64 dlba){
	u64 *sec_start = ramDiskSimulator_sector_head(_this, dlba);
	return ramDiskSimulator_is_block_bad_sector(sec_start);
}

short ramDiskSimulator_is_bad_byte_addr(struct ramDiskSimulator* _this, u64 dlba_bytes, int lengthPage){
	short ret;
	u64 end_addr = dlba_bytes + lengthPage;
	while (dlba_bytes < end_addr) {
		if ((ret = ramDiskSimulator_is_bad_sector(_this,  NVMEIBC_BYTE2SECTOR(dlba_bytes))))
			return ret;
		dlba_bytes += NVMEIBC_SECTOR_SIZE;
	}
	return 0;
}
void ramDiskSimulator_verify_no_bad_sectors(struct ramDiskSimulator* _this){
	u64 sec, num_sectors = NVMEIBC_BYTE2SECTOR(_this->committed_disk_size);
	BUG_ON(NVMEIBC_SECTOR2BYTE(NVMEIBC_BYTE2SECTOR(_this->committed_disk_size - 1) + 1) != _this->committed_disk_size);
	for (sec = 0; sec < num_sectors; ++sec) {
		BUG_ON(ramDiskSimulator_is_bad_sector(_this, sec + _this->committed_addr.sector));
	}
}

int ramDiskSimulator_printTakenLocks(struct ramDiskSimulator*_this){
	u32 i, rv = 0, nLocks = ARRAY_SIZE(_this->locks);
	unsigned long flags;
	spin_lock_irqsave(&_this->cmpxchg_lock, flags);
	for (i=0; i<nLocks; i++){
		if (_this->locks[i] != LS_UNLOCKED){
			_NI_dmesg(trace_ssd_disk_sim_ramDiskSimulator_printTakenLocks, "--->disk[@RANDISK_UNIQUEID]->locks[@LSI] = @LOCK_ENT,",_this->uniqueID, i, _this->locks[i]);
			rv++;
		}
	}
	spin_unlock_irqrestore(&_this->cmpxchg_lock, flags);
	return rv;
}

/**************************** Writing IO to disk ******************************/
#include "nvmeibs_nic_dma_atomics.h"							// For piggybacks

// The includes below are breaking encapsulation concept (Simulator digs into Blocks code). This is done for debug purpose (verify internal states of Block locks).
#include "../nvmeibc_topology.h"
#include "../datapath_ec/nvmeibc_block_dp_ec_journal_common.h"
#include "../datapath_ec/nvmeibc_block_dp_ec.h"

/* return the offset (in locks) that needs to be added to the lock_offset in src_seg in order to get the lock entry for the same slice in the dst_seg. this can be positive/negative !!!
 * lock_offset : offset (in locks) from beggining of src_seg
 * src_seg : the segment on which we have a lock offset
 * dst_seg : the segment on which we need the lock offset*/
static inline s64 __get_phys_lock_offset_between_seg(const struct nvmeibc_disk_segment *src_seg, const struct nvmeibc_disk_segment *dst_seg){
	return (((s64)(dst_seg->first_lba)) - ((s64)(src_seg->first_lba))) / LOCKSET_SLICES;
}

/* verify that an IO extent is protected by locks of correct type & size
 * si: the member (segment index) of the disk within the raid */
static inline void __verify_locks_before_io(u64 dlba_bytes, u64 nlba_bytes, const struct nvmeibc_raid1* r1, int si, enum nvmeib_block_io_op op){
	const struct nvmeibc_disk_segment *my_seg = &r1->segments[si];
	struct dp_io_topo_iterator_res iter_res;
	const u64 lock_ind_first =  dlba_bytes			 	    /BYTES_IN_LOCKSET;
	const u64 lock_ind_last	=  (dlba_bytes+nlba_bytes-1)	/BYTES_IN_LOCKSET;
	struct lock_ownership_map rlmap;
	u64	lock_ind, other_lock_ind, li;

	/* check that the raid slice has the correct number of Onwer(s) & Actives and correct locations. */
	for (lock_ind=lock_ind_first; lock_ind<=lock_ind_last; lock_ind++) {
		const u64 dlba = lock_ind*LOCKSET_SLICES;
		dp_io_topo_iterator_conv_seg_lock_addr_to_raid_ofst(&iter_res, my_seg, dlba - my_seg->first_lba);
		lock_ownership_build_raid_map(r1, iter_res.rlba, op, &rlmap);
		for (li = 0; li < (u64)rlmap.n_locks; li++) {						// verify each member has the lock that the lock map says it needs
			const struct nvmeibc_disk_segment *seg = &r1->segments[rlmap.si[li]];
			union nvmeib_lock_id lock_val;
			struct ramDiskSimulator* ram = &serverOf(seg->disk)->ramDisk;
			other_lock_ind = COMMITTED_ADDR(ram, lock_ind + __get_phys_lock_offset_between_seg(my_seg, seg), LOCK);	// Which lock on 'seg' protects area on segments[si] ?
			BUG_ON((other_lock_ind >= RAMDISK_DATA_LOCK_SIZE));
			BUG_ON(!nvmeibtc_ds_owner_mode_is_valid(rlmap.type[li]));
			spin_lock(&ram->cmpxchg_lock);
			lock_val = (union nvmeib_lock_id){ .all = ram->locks[other_lock_ind] };
			// when member K is paused, the lock on K might have already been taken. in that case, the IO on the other members of the raid continues & succeeds.
			// but since disk K is paused, its recovery will recover locks into stale-special, causing a lock consistency verification to fail, when done for the IO's on the non paused members.
			if (!nvmeib_lockid_are_purified_eq(lock_val, r1->lid)) {
				BUG_ON(seg->disk->pausing_no_transfers != true);	// BEWARE: a quick pause/cont on one member while the other is still executing the IO from before the pause, might not catch the pause & fail !!!
				_NI_dmesg(t_01vlbi, "member @SI (disk @DISK_NAME) of r1 @R1 is paused - cannot verify lock state", si, seg->disk->name, r1);
			}
			spin_unlock(&ram->cmpxchg_lock);
		}
	}
}

// verify correctness of state before the IO is executed (e.g.: locks are taken, journal is of proer size, segment, journal space, ...)
static inline void __verify_before_io(struct ramDiskSimulator* ram, struct nvmeibc_block_command *cmd, u64 start_disk, u64 length_disk,
									  const struct nvmeibc_raid1* r1, enum nvmeib_block_io_op op){
	unsigned i;
	if (r1->replicas == 1)
		return;
	if (cmd->my_stage == E_CMDS_STAGE_WRITE_JOURNAL) {
		struct nvmeibc_block_command *rldr = dp_cmd_get_raid_leader(cmd);
		const struct multi_snake_slice_analyzer *mssa = rldr->o->mssa;
		const struct nvmeibc_block_command *io_cmd = &cmd[mssa->n_writes/2];
		BUG_ON(mssa->no_jour);
		BUG_ON(io_cmd->ds != cmd->ds);			// verify that the IO cmd on same member as this journal cmd, is of same size
		BUG_ON(io_cmd->iocmd->reqs1.ndb->length != cmd->iocmd->reqs1.ndb->length);
		start_disk = NVMEIBC_SECTOR2BYTE(io_cmd->iocmd->reqs1.disk_address); // map journal to the cmd it is protecting to check that locks are taken.
		BUG_ON(cmd->iocmd->reqs1.ndb != cmd->iocmd->reqs1.ndb); // verify the journal/data sgl are identical (coz they share the actual blocks data)
		if (1) {				// verify journal MD content
			union jblock_md	*jmd = (void*)cmd->iocmd->reqs1.md;
			const int cmd_ind = cmd - &rldr[mssa->n_reads];
			const u32 first_tx_id = rldr->rld.post.bits.txid, nlbas = (u32)cmd->nlbas;
			BUG_ON(cmd_ind >= r1->replicas);
			BUG_ON(jmd->tx_id != first_tx_id);
			BUG_ON(jmd->tx_id == NVMEIBC_DP_EC_MD_TX_ID_NO_JOURNALS);
			for (i = 0; i < nlbas; i++) {
				const u64 j2d = __dp_ec_jmd_get_u64_j2d(jmd);			// nvmeibc_block_dp_ec_jmd_decode_j2d_only(jmd)
				BUG_ON(j2d != NVMEIBC_BYTE2SECTOR(start_disk)+i);		// verify the journal->data pointer
				if (i >= 1)
					BUG_ON(jmd->tx_id != first_tx_id);
				jmd = (void*)((u8*)jmd + ram->md_size);
			}
		}
	}
	#if 0
	if (nvmeibc_raid_is_ec(r1) || cmd->iocmd.reqs1.md) {	// we have EC with (slice_size == 1)
		if (cmd->my_stage == E_CMDS_STAGE_DO_IO_AND_PAR && nvmeib_block_io_op_is_write(cmd->o->op)) {
			union nvmeibc_block_dp_ec_data_block_md *dmd = (void*)cmd->iocmd.reqs1.md;
			struct serverSimulator * srvr = serverSimulator_get_srvr_by_nvmedisk(ram);
			for (i=0; i<cmd->nlbas; i++){
				BUG_ON(dmd[i].jri != /* Todo: Current jri which serjio allocated to client)*/ srvr->serjio.jranges_allocs[dmd[i].jri]);//
			}
		}
	}
	#endif
	__verify_locks_before_io(start_disk, length_disk, r1, (cmd->ds-r1->segments), op);
}

typedef struct {
	eCPU_thread_internal_params;
	struct nvmeibc_d_iocmd_comp *cmd_comp;
} t_async_cb_params_iocmd;

static eCPU_cb_ret_type __async_io_cmd_cb(eCPU_cb_param_list) {
	t_async_cb_params_iocmd *p = eCPU_thread_extract_param(t_async_cb_params_iocmd);
	eCPU_thread_start_execution(p);
	nvmeibc_block_completion(p->cmd_comp);
	eCPU_thread_end_execution(p);
}

#define SIMULATE_DISK_STATS   (0)								// No need to simulate them (currently)
#if SIMULATE_DISK_STATS
	void disk_stats_init(struct nvmeibc_disk_io_command *cmd) {
		nvmeib_stats_init(     &cmd->io_stat.common);
		nvmeib_stats_set_start(&cmd->io_stat.common);
	}
	void disk_stats_measure(struct nvmeibc_disk_io_command *cmd, struct nvmeibc_disk *disk) {
		unsigned long flags;
		nvmeib_stats_measureq(&cmd->io_stat.common);
		spin_lock_irqsave(&disk->stats_spinlock, flags);
		disk->local_sum_dt += cmd->io_stat.common.sum_dt;
		disk->local_counts += cmd->io_stat.common.counts;
		spin_unlock_irqrestore(&disk->stats_spinlock, flags);
#if defined(NVMEIBC_ENABLE_PER_VOLUME_STATS)
		nvmeibc_disk_add_stats(disk, cmd->v_disk_stats, cmd->reqs, cmd->io_stat.common.sum_dt);
#else
		nvmeibc_disk_add_stats(disk, NULL,				cmd->reqs, cmd->io_stat.common.sum_dt);
#endif
	}
#else
	#define disk_stats_init(cmd)
	#define disk_stats_measure(cmd, disk)
#endif

#include "block/recovery/nvmeibc_block_dp_sync_common.h"			// Daniel: This is a hack. In slice by slice mode of sync the stack depth gets too large, think of better way to solve it...
static inline bool __is_sync_cb(const struct nvmeibc_block_command *bcmd) {
	const bool is_sync_cb = ut_conf__get_transport()->is_disk_callback_sync;
	const bool is_recov_op = ((bcmd->o->op) & (NVMEIB_BLOCK_IO_OP_RECOVER_STALE|NVMEIB_BLOCK_IO_OP_REC_SPARE));
	if (is_sync_cb && (is_recov_op && bcmd->o->rso->slice_by_slice_index))
		return false;
	return is_sync_cb;
}

int ramDisk_execute_io(struct ramDiskSimulator* ram, struct nvmeibc_disk_io_command *cmd) {
	struct nvmeibc_d_iocmd_comp *comp = &cmd->comp;
	struct nvmeibc_block_command *bcmd = dp_cmds_get_cmd_from_comp(comp);		// Simulator accesses internal block fields to verify locks. Real function in c_disk.c cannot do that!
	bool const is_stage_write_journal = (bcmd->my_stage == E_CMDS_STAGE_WRITE_JOURNAL);

	const struct nvmeibc_block_io_req *curReq = &cmd->reqs1;
	void *md = curReq->md;
	const struct nvmeib_data_buffer *ndb    = curReq->ndb;
	u64 dlba_bytes_abs = NVMEIBC_SECTOR2BYTE(curReq->disk_address);	// Address on the disk of the beggining of the IO.
	u64 dlba_bytes_rel = is_stage_write_journal ? dlba_bytes_abs - ram->committed_addr.byte
												: COMMITTED_ADDR_AS(ram, curReq->disk_address, SECTOR, BYTE);
	// jam is encoding addr
	u64 nlba_bytes						= ndb->length;									// Length of the IO (in bytes)
	int nSGelements						= ndb->table.nents;
	struct scatterlist *sgl				= ndb->table.sgl;
	struct scatterlist dummySG			= { .length = ndb->length };	// dummy sg entry for metadata only commands
	const enum nvmeib_block_io_op op 	= curReq->op;			// Type of IO operation != bcmd->o->op
	int i;
	struct scatterlist *curSG			= NULL;
	const struct nvmeibc_raid1* r1 = nvmeibc_disk_segment_get_praid(bcmd->ds);
	const u64 max_disk_alloc_address = ram->committed_addr_end.byte;
	//cmd->reqs->req.nvme_op = (enum e_NVMEIB_CMD)io_mode;

	if (unlikely(op >= NVMEIB_BLOCK_IO_OP_MD_READ)) {		// Metadata only command
		// Generally, we would have wanted to check the following, but there are some syncs
		// which reuse "full" commands for the purposes of MD read, so we allow that.
		// WARN_ON(sgl || nSGelements);
		sgl = &dummySG;
		nSGelements = 1;
	}

	if (ram->state & ramDisk_down) {
		_NW_dmesg(warn_ssd_disk_sim_ramDisk_execute_io, "Disk @RAM_UNIQUEID: is dead, IO canceled", ram->uniqueID);
		if (cmd->req_id&0x1)									// Simulate 50% chance of each failure according to unique ID of the command
			return -EIO;		// Simulate immediate failure - failure before send to server
		comp->comp_code = -EIO;	// Simulate as if command sent to server and returned with no execution error
		goto _mem_done;
	}
	if (ram->state & ramDisk_broken) {
		comp->comp_code = (ram->error_code ? ram->error_code : NVME_SC_DNR);
		_NW_dmesg(warn_1_ssd_disk_sim_ramDisk_execute_io, "Disk @RAM_UNIQUEID: is broken, IOs will return with err @COMP_CODE", ram->uniqueID, comp->comp_code);
		goto _mem_done;
	}
	if ((ram->state & ramDisk_fail_data) && (nvmeib_block_io_op_is_write(op)) && ((dlba_bytes_rel+nlba_bytes) <= ram->data_seg_size)) {  // Fail data part of EC IO (post journal)
		_NW_dmesg(warn_2_ssd_disk_sim_ramDisk_execute_io, "Disk @RAM_UNIQUEID: simulate data error", ram->uniqueID);
		if (cmd->req_id&0x1)									// Simulate 50% chance of each failure according to unique ID of the command
			return -EIO;		// Simulate immediate failure - failure before send to server
		comp->comp_code = -EIO;	// Simulate as if command sent to server and returned with no execution error
		goto _mem_done;
	}

	disk_stats_init(cmd);
	if ((nvmeib_block_io_op_is_write(op)) || bcmd->nlocks_take_before_cmd /* read takes locks*/) {
		__verify_before_io(ram, bcmd, dlba_bytes_abs, nlba_bytes, r1, op); // Verify that lock was taken for write
	}

	_ND(trace_ssd_disk_sim_ramDisk_execute_io, "Disk @RAM_UNIQUEID: op=@BLOCK_IO_OP:@BLOCK_IO_OP", ram->uniqueID, bcmd->o->op, op);
	BUG_ON(nSGelements > NVMEIBS_MAX_IO_CHANNEL_MSGS);			// Transport layer does not support such long sg-lists
	for_each_sg(sgl, curSG, nSGelements, i) {
		u64 dlba_abs, dlba_rel;
		int sg_nlba_bytes = curSG->length, sg_nlbas;
		short code;
		u8 *src = unlikely(op >= NVMEIB_BLOCK_IO_OP_MD_READ) ? NULL : (u8*)sg_virt(curSG);	// Pointer to the actual input memory we have IO
		if (curReq->do_512b_sub_block_x) {
			BUG_ON(nSGelements != 1);
			dlba_bytes_abs += curSG->offset;		// Add sub block offset to dlba
			dlba_bytes_rel += curSG->offset;		// Add sub block offset to dlba
			sg_nlbas = 1;
			atomic_inc(&ram->sub_blk_io_cnt);
		} else {
			if (op == NVMEIB_BLOCK_IO_OP_DISCARD) {
				nlba_bytes = sg_nlba_bytes = (le32_to_cpu(curReq->trim->nlb) << ram->sector_shift);
				dlba_bytes_abs = (le64_to_cpu(curReq->trim->slba)<< ram->sector_shift); // Alternatively use *((struct nvmeib_dsm_range*)src) instead of buffer
				dlba_bytes_rel = COMMITTED_ADDR(ram, dlba_bytes_abs, BYTE);
				__verify_before_io(ram, bcmd, dlba_bytes_abs, nlba_bytes, r1, op); // Verify that lock was taken for trim operations
			}
			sg_nlbas = NVMEIBC_BYTE2SECTOR(sg_nlba_bytes);
		}
		dlba_abs = NVMEIBC_BYTE2SECTOR(dlba_bytes_abs);
		dlba_rel = NVMEIBC_BYTE2SECTOR(dlba_bytes_rel);

		_ND(trace_1_ssd_disk_sim_ramDisk_execute_io, "IO range [@VLBA_BYTES:@N_BYTES], ramDisk [@PTR..@PTR), buffer @PTR", dlba_bytes_abs, sg_nlba_bytes, &ram->mem[dlba_bytes_rel], &ram->mem[dlba_bytes_rel]+sg_nlba_bytes, src);
		BUG_ON((dlba_bytes_rel + sg_nlba_bytes) > max_disk_alloc_address); // IO outside of last segment allocated on the disk
		atomic_inc(&ram->io_cnt);
		//spin_lock(&ram->mem_lock); - Uncomment to emulate 1 queue. Comment to emulate infinite queues
		if (op == NVMEIB_BLOCK_IO_OP_READ || op >= NVMEIB_BLOCK_IO_OP_MD_READ) {        // Any read, check bad sector injection
			if ((code = ramDiskSimulator_is_bad_byte_addr(ram, dlba_bytes_abs, sg_nlba_bytes))) {
				_ND(error_ssd_disk_sim_ramDisk_execute_io, "Injecting @INJECTED_CODE error on sector @DLBA", code, dlba_abs);
				comp->comp_code = code;
				goto _mem_done;
			}
		}
		if (unlikely(op >= NVMEIB_BLOCK_IO_OP_MD_READ))					// Metadata only command
			goto _do_metadata;
		switch (op) {
			case NVMEIB_BLOCK_IO_OP_READ:   memcpy(src, &ram->mem[dlba_bytes_rel],      sg_nlba_bytes); break;
			case NVMEIB_BLOCK_IO_OP_WRITE:	memcpy(     &ram->mem[dlba_bytes_rel], src, sg_nlba_bytes); break;
			case NVMEIB_BLOCK_IO_OP_DISCARD:memset(     &ram->mem[dlba_bytes_rel], ramDiskSimulator_TRIMVAL,sg_nlba_bytes); break; // Flash disk writes ones
			case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR: ramDiskSimulator_do_bad_sector(ram, dlba_abs, EPERM_READ_FAIL_NO_RETRY); break;
			default:
				BUG_NOT_IMPLEMENTED_YET;
				break;
		}

_do_metadata:
		if (md || op == NVMEIB_BLOCK_IO_OP_DISCARD) { // We either have metadata to set or it is TRIM (which clears metadata)
			int b;
			BUG_ON((op != NVMEIB_BLOCK_IO_OP_DISCARD && (sg_nlbas > LOCKSET_SLICES)));		// we never do IO of more than a LOCKSET, unless in TRIM
			switch (op) {
				case NVMEIB_BLOCK_IO_OP_READ:
				case NVMEIB_BLOCK_IO_OP_MD_READ:
					ramDiskSimulator_MD_read(ram, dlba_abs, sg_nlbas, md);
					break;

				case NVMEIB_BLOCK_IO_OP_WRITE:
					if (bcmd->my_stage == E_CMDS_STAGE_DO_IO_AND_PAR) {
						for (b = 0; b < sg_nlbas; b++) { // verify MD version
							union nvmeibc_block_dp_ec_data_block_md *bmd = (md + (b*ram->md_size));
							BUG_ON(bmd->D.version != NVMEIBC_DATA_MD_VERSION);
							/*if (!bcmd->is_parity && bmd->D.edic == 0) { // Trap on zero edic: D edic == 0 is rarer than P but happens once every blue moon.
								BUG_ON(((bmd->jri != JRI_MARK_DATA_INVALID_IS_ZEROS) && (bmd->jri != JRI_MARK_INVALID_FOR_READ)));
							}*/
						}
					}
					ramDiskSimulator_MD_write(ram, dlba_abs, sg_nlbas, md);
					break;
				case NVMEIB_BLOCK_IO_OP_DISCARD:
					if (ramDiskSimulator_has_metadata(ram)) {
						BUG_ON(nvmeibc_raid_is_ec(r1)); // Trim is not supported in EC
						ramDiskSimulator_MD_set(ram, dlba_abs, sg_nlbas, ramDiskSimulator_MD_TRIMVAL);
					}
					break;
				case NVMEIB_BLOCK_IO_OP_MD_RD_MOD_WR: {
					const u64 trim_param = *((u64*)cmd->reqs1.md);
					for (b = 0; b < sg_nlbas; b++)
						nvmeib_block_dp_ec_dmd_read_mod_wr(__ptr_to_ith_md(ram, dlba_rel + b), trim_param);
					break;
				}
				case NVMEIB_BLOCK_IO_OP_WRITE_UNCOR:
					break; // In case of write uncorrectable simply do nothing, but do not fail.
				default:
					BUG_NOT_IMPLEMENTED_YET;
					break;
			}
			if ((curReq->jam_op.n_ops)&&(i == 0))  {						// Todo: Remove i == 0. It works only for single slice IO
				serverRam_dma_set_jmdc(ram, dlba_abs, *((u64*)md));
			}
			md += (ram->md_size*sg_nlbas);
		}

		//spin_unlock(&ram->mem_lock);
		dlba_bytes_abs += sg_nlba_bytes;
		dlba_bytes_rel += sg_nlba_bytes;
		nlba_bytes -= sg_nlba_bytes;
	}

	BUG_ON(nlba_bytes!=0);
	comp->comp_code = 0;
_mem_done:

	if (dp_cmds_pigbck_has_any(cmd))
		serverRam_dma_do_pigback(ram, bcmd);				// Daniel: Is this correct that piggyback occurs even if disk is offline ????

	disk_stats_measure(cmd, disk);
	eCPU_thread_prepare(t_async_cb_params_iocmd, p);
	p->cmd_comp = comp;
	eCPU_thread_launch(__async_io_cmd_cb, p, __is_sync_cb(bcmd));
	return 0;
}

/*****************************************************************************/
// EOF.

