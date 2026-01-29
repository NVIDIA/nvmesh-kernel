/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef NVMEIBT_CONFIG_READ_FILE
#define NVMEIBT_CONFIG_READ_FILE

#include "nvmeibt_common.h"
#include "nvmeibt_mm_json.h"
#include "nvmeibt_str.h"
#include "nvmeibt_kafka.h"

#define NVMEIBT_MAX_NODE_NAME_LENGTH 256

enum NVMEIBT_CSV_TYPE {
	NVMEIBT_CSV_TYPE_NONE = 0x1,
	NVMEIBT_CSV_TYPE_LOCAL_DISKS = 0x20,
	NVMEIBT_CSV_TYPE_LOCAL_NICS = 0x21,
	NVMEIBT_CSV_TYPE_TOPO = 0x62,
	NVMEIBT_CSV_TYPE_REMOTE_APPLIED = 0x63,
	NVMEIBT_CSV_TYPE_FULL_HW_CONFIG = 0x70,
	NVMEIBT_CSV_TYPE_FULL_TOPO_CONFIG_VOLUMES = 0x71,
	NVMEIBT_CSV_TYPE_FULL_KAFKA_MGMT_CONFIG_VOLUMES = 0x72,
	NVMEIBT_CSV_TYPE_INCREMENTAL_UPDATE = 0x80,
};

struct nvmeibt_csv_file_ctx {
	const char 					*name;
	enum NVMEIBT_CSV_TYPE 	section_type;
};

struct nvmeibt_topology;
struct nvmeibt_local_disk;
struct nvmeibt_Str;
struct nvmeibt_ascii_uuid;
struct nvmeibt_disk_gpt_partition_entry;
struct nvmeibt_persistency_wq_entry;
struct nvmeibt_seg_local;
struct nvmeibt_seg_lot;
struct nvmeibt_local_disk_config;
struct local_disk_info;
struct nvmeibt_disk_gpt;
struct nvmeibt_disk_mbr;
struct nvmeibt_node;
struct netlink_io_context;
struct nvmeibt_raft_member;

struct mm_mgmt_conf;

bool nvmeibt_read_config_am_i_eligible_to_read_config_directly(void);
enum NVMEIBT_CSV_TYPE nvmeibt_get_section_type_by_section_header(char *section_header);
const char *nvmeibt_get_csv_section_header_by_section_type(int section_type);
const char *nvmeibt_get_csv_header_by_section_type(int section_type);
struct nvmeibt_disk_segment;
void nvmeibt_read_config_add_missing_seg_to_praid(struct nvmeibt_praid *praid, struct nvmeibt_disk_segment *seg);
int nvmeibt_read_config_file(struct nvmeibt_Str *config_struct, struct nvmeibt_csv_file_ctx *file_entry);
int nvmeibt_read_config_vol_removed_from_mgmt(struct mm_mgmt_conf *conf, bool is_updating_leader);
void nvmeibt_read_config_vol_mark_vol_and_segs_for_removal(struct mm_mgmt_conf *conf, bool is_updating_leader);
int nvmeibt_read_config_apply_vol_mgmt_conf(struct mm_mgmt_conf *conf, int vol_config_tag, bool is_updating_leader, enum KAFKA_EVENT_TYPE event_type, bool is_topo_config);
int nvmeibt_read_config_apply_vol_committed_topo_conf(struct mm_mgmt_conf *conf, int vol_config_tag);
int nvmeibt_read_config_apply_HW_full_config_mgmt_conf(struct HW_mgmt_conf *conf, int HW_config_tag, bool is_trim_missing_objects);
int nvmeibt_parse_buf(const char *csv_or_bin_buf, int csv_or_bin_buf_len, int is_updating_leader, unsigned long long serialization_version,
					  struct nvmeibt_raft_member *remote_member, enum NVMEIBT_CSV_TYPE content_type, struct nvmeibt_Str *JSON_output);

int nvmeibt_parse_csv_buf(struct nvmeibt_Str *csv_ctx, int is_updating_leader, enum NVMEIBT_CSV_TYPE content_type);
int nvmeibt_write_config_to_persistency(struct nvmeibt_persistency_wq_entry *entry);
int nvmeibt_write_GPTS_of_a_local_disk(struct local_disk_info *ld_info);
int nvmeibt_write_PMBR_of_a_local_disk(struct netlink_io_context *nl_ctx, int fd, int pblk_size, struct nvmeibt_disk_mbr *mbr, const char *ld_display);
int nvmeibt_write_all_disks_GPTs(struct nvmeibt_persistency_wq_entry *entry);
int launch_read_of_local_disk_gpt_and_segs_metadata_and_persist(struct nvmeibt_local_disk *cur_disk);	TODO(was nvmeibt_read_both_gpts_and_persisted_config_from_disk);
void nvmeibt_read_config_print_status(int (*printf_fn)(void *ctx, const char *fmt, ...), void *printf_ctx);
int nvmeibt_read_config_notify_server_about_journal_partition(const struct nvmeibt_disk_gpt_partition_entry *journal_data_entry,
															  const struct nvmeibt_disk_gpt_partition_entry *serjio_db_entry,
															  const char *ldisk_id, const char *ld_display);
int nvmeibt_read_excluded_target_drives(void);
int nvmeibt_read_auto_takeover_target_drives(void);

#define NNVMEIBT_HASH_GET_OBJ_BY_UUID(name, __hash, __uuid, __obj_name)			\
({																				\
	XHASHTABLE_TYPE(__hash)		_iter_;											\
	XHASHTABLE_TYPE(__hash)		_obj1_ = NULL;									\
	unsigned long				_key_ = UUID_TO_64_HASH_KEY(__uuid);			\
																				\
	XHASHTABLE_FOR_EACH_POSSIBLE_SAFE(_iter_, __hash, _key_) {					\
		if (ARE_UUID_EQ(nvmeibt_##__obj_name##_UUID(_iter_), __uuid)) {			\
			_obj1_ = _iter_;													\
			break;																\
		}																		\
	}																			\
	N_Df(name, "hash: @STR found " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE",	\
		 (_obj1_ ? "" : "Not "), __uuid);										\
	_obj1_;																		\
})

static inline void _hash_copy(void* dst, void* src, int size) {
	if (dst != NULL)
		memcpy(dst, src, size);
}

#define CONFIG_TAG_OUTDATED -1

/* Inserts __newobj to hash table or if exists overrrides its from_config
   Return values:
	1. _rv_ - action the hash table performed
    2. get_hash_obj_ptr_addr - if not NULL, returns address of object in hash
    3. get_prev_config_buf - if not NULL, copies prev config value to it */
#define NNVMEIBT_HASH_ADD_OBJ(name, __hash, __newobj, __tag, MAX_N,						\
				get_hash_obj_ptr_addr, get_prev_config_buf, __obj_name)					\
({																						\
	enum nvmeibt_add_rv				_rv_ = NVMEIBT_ADD_UNINITIALIZED;					\
	const int _config_size = sizeof((__newobj)->from_config);							\
	const union nvmeib_uuid			*_uuid_ = nvmeibt_##__obj_name##_UUID(__newobj);	\
	XHASHTABLE_TYPE(__hash)			_obj2_;												\
																						\
	_obj2_ = NNVMEIBT_HASH_GET_OBJ_BY_UUID(name ## _hash, __hash, _uuid_, __obj_name);	\
	if (_obj2_) {																		\
		if (memcmp(&(__newobj)->from_config, &_obj2_->from_config, _config_size) == 0) {\
			N_Tf(name ## _1, "hash: object already exists " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE",		\
								_uuid_);												\
			_rv_ = NVMEIBT_ADD_ALREADY_UP_TO_DATE;										\
		} else if (_obj2_->config_tag == (__tag)) {										\
			N_Ef(name ## _error_1, "hash: Same ID diff objects " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE",	\
									_uuid_);											\
			_rv_ = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;								\
		} else {																		\
			_hash_copy(get_prev_config_buf, &_obj2_->from_config, _config_size);		\
			_obj2_->from_config = (__newobj)->from_config;								\
			N_Tf(name ## _2, "hash: object modified " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);	\
			_rv_ = NVMEIBT_ADD_MODIFIED;												\
		}																				\
	} else {																			\
		if (XHASHTABLE_N_ELEMENTS(__hash) > MAX_N) {									\
			N_Wf(name ## _4, "hash: too many entires " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);	\
		}																				\
		_obj2_ = (__newobj);															\
		N_Tf(name ## _3, "hash: add " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);					\
		NTOMA_ASSERT(name ## _assert, XDLIST_NULL(&_obj2_->topo_link),					\
					"object already linked " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);				\
		XHASHTABLE_ADD(__hash, _obj2_, UUID_TO_64_HASH_KEY(_uuid_));					\
		_rv_ = NVMEIBT_ADD_NEW;															\
	}																					\
	if (_obj2_ != NULL && !NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(_obj2_)) {				\
		_obj2_->config_tag = (__tag);													\
	}																					\
	get_hash_obj_ptr_addr = _obj2_;		(void)get_hash_obj_ptr_addr;					\
	_rv_;																				\
})

#define NNVMEIBT_HASH_DEL_OBJ(name, __hash, __oldobj, __obj_name)						\
do {																					\
	const union nvmeib_uuid			*_uuid_ = nvmeibt_##__obj_name##_UUID(__oldobj);	\
	XHASHTABLE_TYPE(__hash)			_obj2_ = NULL;										\
																						\
	_obj2_ = NNVMEIBT_HASH_GET_OBJ_BY_UUID(name ## _hash_get, __hash, _uuid_, __obj_name);					\
	if (_obj2_ == NULL) {																\
		N_Ef(name ## _error, "hash: unknown entry" MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);		\
	} else {																			\
		N_Tf(name, "hash: del " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);		\
		XHASHTABLE_DEL(__hash, &_obj2_->topo_link);										\
		_obj2_->config_tag = CONFIG_TAG_OUTDATED;										\
	}																					\
} while (0)

#define NVMEIBT_HASH_N_OBJS(__hash)														\
	XHASHTABLE_N_ELEMENTS(__hash)

#define NVMEIBT_HASH_IS_OLDER_OBJ(__oldobj, tag)										\
	((__oldobj)->config_tag < tag)

// OUTDATING of objects
#define NVMEIBT_HASH_MARK_OBJ_OUTDATED(__name__, __oldobj, __obj_name)					\
do {																					\
	const union nvmeib_uuid	*_uuid_ = nvmeibt_##__obj_name##_UUID(__oldobj);			\
	N_Tf(__name__, "hash: mark old " MACRO_DEF_TO_STR(__obj_name) "=@UUID_LE", _uuid_);	\
	(__oldobj)->config_tag = CONFIG_TAG_OUTDATED;										\
} while (0)

#define NVMEIBT_HASH_IS_OBJ_MARKED_OUTDATED(__oldobj)									\
	((__oldobj)->config_tag == CONFIG_TAG_OUTDATED)

	// HASH functions for ascii_uuid

#define NNVMEIBT_HASH_GET_OBJ_BY_UUID_ASCII(name, __hash, __uuid_str, __obj_name)		\
({																				\
	XHASHTABLE_TYPE(__hash)		_iter_;											\
	XHASHTABLE_TYPE(__hash)		_obj1_ = NULL;									\
																				\
	XHASHTABLE_FOR_EACH_POSSIBLE_SAFE(_iter_, __hash, xhash_str_to_32_bits(__uuid_str)) {	\
		/* use strcmp() -and not strncmp()- intentionally, to catch bugs */		\
		if (strcmp(nvmeibt_##__obj_name##_UUID_str(_iter_), __uuid_str) == 0) {	\
			_obj1_ = _iter_;													\
			break;																\
		}																		\
	}																			\
	N_Df(name, "hash: @STR found " MACRO_DEF_TO_STR(__obj_name) "=@UUID",		\
		 (_obj1_ ? "" : "Not "), __uuid_str);									\
	_obj1_;																		\
})

#define NNVMEIBT_HASH_ADD_OBJ_ASCII(name, __hash, __newobj, __tag, MAX_N,				\
				get_hash_obj_ptr_addr, get_prev_config_buf, __obj_name)					\
({																						\
	enum nvmeibt_add_rv				_rv_ = NVMEIBT_ADD_UNINITIALIZED;					\
	const int _config_size = sizeof((__newobj)->from_config);							\
	const struct nvmeibt_ascii_uuid	*_uuid_ = nvmeibt_##__obj_name##_UUID(__newobj);	\
	const char						*_uuid_str = _uuid_->str;							\
	XHASHTABLE_TYPE(__hash)			_obj2_;												\
																						\
	_obj2_ = NNVMEIBT_HASH_GET_OBJ_BY_UUID_ASCII(name ## _hash, __hash, _uuid_str, __obj_name);						\
	if (_obj2_) {																		\
		if (memcmp(&(__newobj)->from_config, &_obj2_->from_config, _config_size) == 0) {\
			N_Tf(name ## _1, "hash: object already exists " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);		\
			_rv_ = NVMEIBT_ADD_ALREADY_UP_TO_DATE;										\
		} else if (_obj2_->config_tag == (__tag)) {										\
			N_Ef(name ## _error_1, "hash: Same ID diff objects " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);	\
			_rv_ = NVMEIBT_ADD_FAILED_OTHERS_FUNCTIONAL;								\
		} else {																		\
			_hash_copy(get_prev_config_buf, &_obj2_->from_config, _config_size);		\
			_obj2_->from_config = (__newobj)->from_config;								\
			N_Tf(name ## _2, "hash: object modified " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);			\
			_rv_ = NVMEIBT_ADD_MODIFIED;												\
		}																				\
	} else {																			\
		if (XHASHTABLE_N_ELEMENTS(__hash) > MAX_N) {									\
			N_Wf(name ## _4, "hash: too many entires " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);			\
		}																				\
		_obj2_ = (__newobj);														\
		N_Tf(name ## _3, "hash: add " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);						\
		NTOMA_ASSERT(name ## _assert, XDLIST_NULL(&_obj2_->topo_link),				\
					"object already linked " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_->str);				\
		XHASHTABLE_ADD(__hash, _obj2_, xhash_str_to_32_bits(_uuid_str));			\
		_rv_ = NVMEIBT_ADD_NEW;														\
	}																					\
	if (_obj2_ != NULL) {																\
		_obj2_->config_tag = (__tag);													\
	}																					\
	get_hash_obj_ptr_addr = _obj2_;		(void)get_hash_obj_ptr_addr;					\
	_rv_;																				\
})

#define NNVMEIBT_HASH_DEL_OBJ_ASCII(name, __hash, __oldobj, __obj_name)					\
do {																					\
	const struct nvmeibt_ascii_uuid	*_uuid_ = nvmeibt_##__obj_name##_UUID(__oldobj);	\
	const char						*_uuid_str = _uuid_->str;							\
	XHASHTABLE_TYPE(__hash)			_obj2_ = NULL;										\
																						\
	_obj2_ = NNVMEIBT_HASH_GET_OBJ_BY_UUID_ASCII(name ## _hash_get, __hash, _uuid_str, __obj_name);		\
	if (_obj2_ == NULL) {																\
		N_Ef(name ## _error, "hash: unknown entry " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);	\
	} else {																			\
		N_Tf(name, "hash: del " MACRO_DEF_TO_STR(__obj_name) "=@UUID", _uuid_str);		\
		XHASHTABLE_DEL(__hash, &_obj2_->topo_link);										\
		_obj2_->config_tag = CONFIG_TAG_OUTDATED;										\
	}																					\
} while (0)

#endif	// #ifndef NVMEIBT_CONFIG_READ_FILE

