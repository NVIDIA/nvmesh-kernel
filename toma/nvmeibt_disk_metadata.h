#ifndef NVMEIBR_DISK_METADATA_H
#define NVMEIBR_DISK_METADATA_H

#include "nvmeibt_params.h"
#include "nvmeibt_common.h"
#include "interfaces/srvr/nvmeibt_srvr_proc.h"

#define UEFI_GPT_REVISION		0x00010000		/* GPT revision 1.0 by UEFI standard */
#define UEFI_GPT_HEADER_SIZE	92				/* GPT header size by UEFI standard */

struct nvmeibt_mbr_partition_record
{
	char									boot_indicator;							// Set to 0x00
	char									starting_chs[3];						// Set to the value of the address of starting PBA.
	char									os_type;								// Should be 0xEE (GPT protective)
	char									ending_chs[3];							// 0xffffff
	int										pba_s __attribute__((packed));	// 0x1 for GPT partition header.
	int										n_pblk __attribute__((packed));	// Size of disk in PBAs, or 0xFFFFFFFF if the disk is to large (2TB for
																					// 512 byte PBA size, and 8TB for 4096 PBA size)
} __attribute__((packed));

struct nvmeibt_disk_mbr
{
	char 									boot_code[440];							// unused by UEFI systems.
	int 									disk_signature __attribute__((packed));	// unused by UEFI systems.
	short 									unknown __attribute__((packed));		// unused by UEFI systems.
	struct nvmeibt_mbr_partition_record   	partitions[4];
	short									signature __attribute__((packed));		// 0xAA55
} __attribute__((packed));

enum NVMEIBR_PARTITION_ALIGNMENT {
	NVMEIBR_PARTITION_ALIGNMENT_NONE = 	0x0,
	NVMEIBR_PARTITION_ALIGNMENT_4KB = 	0x1000,
	NVMEIBR_PARTITION_ALIGNMENT_128KB = 0x20000,
	NVMEIBR_PARTITION_ALIGNMENT_1MB = 0x100000
};

/**
 * Format metadata that is written by the nvmesh_format.py script
 * after finishing the disk format. It includes details about how the disk was
 * formatted.
 *
 * @author max (12/26/17)
 */
struct nvmeibt_disk_format_data
{
	union nvmeib_uuid			disk_obj_uuid;
	struct nvmeibt_ascii_uuid	ldisk_id;
	struct nvmeibt_ascii_uuid	native_serial;
	int							nsid;
	union nvmeib_uuid			mgmt_db_uuid;
	unsigned int 				pblk_size;
	unsigned int 				metadata_size;
	unsigned int				format_request_counter;
	BOOL						is_nvmesh_formatted_and_awaiting_convert_to_pmbr;
	BOOL						is_md_supported;
} __attribute__((packed));

struct nvmeibt_disk_gpt_header
{
	uint64_t			gpt_signature __attribute__((packed));
	int					revision __attribute__((packed));
	int					header_size __attribute__((packed));
	int					header_crc32 __attribute__((packed));
	int					reserved __attribute__((packed));
	uint64_t			my_pba __attribute__((packed));
	uint64_t			alternate_pba __attribute__((packed));
	uint64_t			first_usable_pba __attribute__((packed));
	uint64_t			last_usable_pba __attribute__((packed));
	union nvmeib_uuid	disk_obj_uuid __attribute__((packed));
	uint64_t			partition_entry_pba __attribute__((packed));
	int					n_partition_entries __attribute__((packed));
	int					size_of_partition_entry __attribute__((packed));
	uint32_t			partition_entry_array_crc32 __attribute__((packed));
} __attribute__((packed));

struct nvmeibt_disk_gpt_partition_entry
{
	union nvmeib_uuid 	partition_type_guid __attribute__((packed));
	union nvmeib_uuid 	partition_guid __attribute__((packed));
	uint64_t 			pba_s __attribute__((packed));
	uint64_t 			pba_e __attribute__((packed));
	uint64_t 			attributes __attribute__((packed));
	char16_t 			partition_name[GPT_MAX_PARTITION_NAME_LENGTH];
} __attribute__((packed)) __attribute__((aligned(2)));	// aligned(2) to overcome a compiler loop unroling bug that uses SSE or whatever.


struct nvmeibt_disk_gpt
{
	struct nvmeibt_disk_gpt_header 				header;
	struct nvmeibt_disk_gpt_partition_entry 	entries[LARGE_GPT_MAX_NUM_GPT_ENTRIES];
	int											max_n_entries;
	int 										n_entries_in_use;
	char										main_or_metadata[100];	TODO(Put secondary-metadata or mem-main etc in here and use for logging)
	struct nvmeibt_ascii_uuid					ldisk_id;
	BOOL										is_valid;

} __attribute__((packed));

/**
 * Disk metadata that is stored in a small partition in the metadata section of
 * the disk, it includes details mostly about how the disk was formatted and
 * whether it support EC, in order for TOMA to treat the disk correctly once
 * the disk is read.
 *
 * @author max (12/26/17)
 */
struct nvmeibt_disk_metadata
{
	uint64_t				signature;													// 8
	uint64_t				last_pba_zeroed;											// 16
	union nvmeib_uuid		mgmt_db_uuid;												// 32
	unsigned int			disk_metadata_version;										// 36
	unsigned int 			format_pblk_size;											// 40
	unsigned int 			format_metadata_size;										// 44
	unsigned int			format_request_counter;										// 48
	int						disk_version_unused; // Always 0 now						// 52
	int						crc32; // CRC is calculated on the struct eith crc32 = 0;	// 56
	BOOL					is_md_supported;											// 57
	char	 				ldisk_id_str[ASCII_UUID_MAX_STR_LEN]; // S3HCNX0K701246.1	// 121
	char					marker_of_end_of_fields_PRE_1_3_1[0];						// 121
	char					filler_1[3];												// 124
	int						nsid;														// 128
	char					native_serial_str[ASCII_UUID_MAX_STR_LEN];					// 192
	union nvmeib_uuid		native_nguid_unused;										// 208
	uint8_t					unused[512 - 208];											// 512
} __attribute__((packed));

struct nvmeibt_disk_segment;
struct nvmeibt_local_disk;
struct local_disk_info;
struct nvmeibt_local_disk_config;
struct nvmeibt_Str;

struct netlink_context_io_data {
	pthread_mutex_t guard_mutex;
	pthread_cond_t 	completion_signal;
	int 			rv;
};

struct netlink_io_context {
	struct km_comm_msg_hdr nl_msg;
	struct nvmeib_io_to_disk nl_msg_payload;
	pthread_condattr_t attr;
	struct netlink_context_io_data nl_io_data;
	unsigned int max_request_size;
	unsigned int pblk_size;
} __attribute__((packed));

struct netlink_io_context *nvmeibt_make_netlink_context_from_config(struct nvmeibt_local_disk_config *ldc);
int nvmeibt_netlink_do_io_sync(struct netlink_io_context *nl_ctx);
void nvmeibt_netlink_io_free(struct netlink_io_context **nl_ctx_p);

enum nvmeib_io_is_read { NVMEIB_IO_IS_WRITE = 0, NVMEIB_IO_IS_READ, };		// Todo: Get rid of this enum, just bool is good enough or use 'r', 'w'
int nvmeibt_disk_metadata_do_sync_IO_with_disk_netlink_or_not(struct netlink_io_context *nl_ctx, const int fd,
										void *buf,
										const uint64_t pbyte_s,
										int pblk_size,
										const int n_bytes,
										char *md, unsigned int md_len,
										enum nvmeib_io_is_read is_read, enum nvmeib_main_gpt_update_flags main_gpt_update_flags,
										uint64_t min_offset_allowed);

void nvmeibt_disk_metadata_fill_dump_mbr_str(struct nvmeibt_Str *str, struct nvmeibt_disk_mbr *mbr);
struct nvmeibt_disk_gpt_partition_entry *nvmeibt_disk_metadata_add_mem_gpt_entry(struct nvmeibt_disk_gpt *gpt,
																				 const union nvmeib_uuid *partition_type,
																				 const union nvmeib_uuid *uuid,
																				 uint64_t pba_s,
																				 uint64_t pba_e,
																				 const char *part_name,
																				 int part_name_len);
int nvmeibt_disk_metadata_deprecate_entry_in_mem_gpt(struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *uuid);
int nvmeibt_disk_metadata_remove_entry_from_mem_gpt(struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *uuid);
struct nvmeibt_disk_gpt_partition_entry *nvmeibt_disk_metadata_allocate_partition_and_add_to_mem_gpt(struct nvmeibt_disk_gpt *gpt,
																									 uint64_t req_n_pblks,
																									 int pblk_size,
																									 const union nvmeib_uuid *partition_type,
																									 const union nvmeib_uuid *uuid,
																									 const char * part_name,
																									 int part_name_len,
																									 enum NVMEIBR_PARTITION_ALIGNMENT alignment_bytes);
void nvmeibt_disk_metadata_init_mem_gpt(struct nvmeibt_local_disk *disk,
											uint64_t pba_s,
											uint64_t pba_e,
											struct nvmeibt_disk_gpt *gpt,
											int pblk_size, int max_entries,
											const union nvmeib_uuid *disk_obj_uuid);

void nvmeibt_disk_metadata_init_disk_metadata_struct(struct nvmeibt_local_disk *cur_local_disk,
													  struct nvmeibt_disk_format_data *format_data);

void nvmeibt_disk_metadata_init_gpt_structure(uint64_t pba_s,
											  uint64_t pba_e,
											  struct nvmeibt_disk_gpt *gpt,
											  int pblk_size, int max_entries,
											  const union nvmeib_uuid *disk_obj_uuid);

void nvmeibt_disk_metadata_init_pmbr(struct nvmeibt_disk_mbr *mbr,
									 uint64_t n_disk_pblks,
									 int pblk_size);

int nvmeibt_disk_metadata_read_mbr_blk(struct netlink_io_context *nl_ctx, int fd, int pblk_size, struct nvmeibt_disk_mbr *read_mbr,
								   const char *ld_display, struct nvmeibt_disk_mbr *reference_mbr);
int nvmeibt_disk_metadata_write_mbr(struct netlink_io_context *nl_ctx, int fd, int pblk_size, const struct nvmeibt_disk_mbr *mbr);

BOOL nvmeibt_disk_metadata_is_mbr_any_mbr(const struct nvmeibt_disk_mbr *mbr);
BOOL nvmeibt_disk_metadata_is_protective_mbr(const struct nvmeibt_disk_mbr *mbr);
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_gpt_entry_of_metadata_gpt(const struct nvmeibt_disk_gpt *gpt);
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_journal_data_entry(const struct nvmeibt_disk_gpt *gpt);
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_serjio_db_entry(const struct nvmeibt_disk_gpt *gpt);
const struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_disk_metadata_entry(const struct nvmeibt_disk_gpt *gpt);
struct nvmeibt_disk_gpt_partition_entry* nvmeibt_disk_metadata_get_gpt_entry_by_uuid(struct nvmeibt_disk_gpt *gpt, const union nvmeib_uuid *uuid);
// GPT validity status enum
enum GPT_VALIDITY {
	GPT_VALIDITY_UNKNOWN			= 0,
	GPT_VALIDITY_OK					= 1,
	GPT_VALIDITY_CRC_ERR			= 2,
	GPT_VALIDITY_MAGIC_ERR			= 3,
	GPT_VALIDITY_TECHNICAL_ERR		= 4,
	GPT_VALIDITY_DATA_ERR			= 5,
	GPT_VALIDITY_MIDST_WRITE		= 6,
};

char *gpt_validity_str(enum GPT_VALIDITY validity);

// Read all 4 GPT structures into caller-allocated buffers with validity info
// Caller must allocate buffers before calling (use NNVMEIBT_BM_ALIGNED_CALLOC)
void nvmeibt_disk_metadata_read_all_4_gpt_structs_into_buffers(
	struct netlink_io_context *nl_ctx,
	int fd,
	int pblk_size,
	struct nvmeibt_disk_gpt *gpt,
	uint64_t pba_s,
	uint64_t pba_e,
	enum GPT_VALIDITY *primary_header_validity,
	enum GPT_VALIDITY *alternate_header_validity,
	enum GPT_VALIDITY *primary_entries_validity,
	enum GPT_VALIDITY *alternate_entries_validity,
	struct nvmeibt_disk_gpt_header *primary_header,
	struct nvmeibt_disk_gpt_header *alternate_header,
	struct nvmeibt_disk_gpt_partition_entry *primary_entries,
	struct nvmeibt_disk_gpt_partition_entry *alternate_entries);

// Restore the dest gpt from the given disk at the given positions.
int nvmeibt_disk_metadata_restore_gpt(struct netlink_io_context *nl_ctx, int fd,
									  int pblk_size,
									  struct nvmeibt_disk_gpt *gpt,
									  uint64_t pba_s,
									  uint64_t pba_e,
									  BOOL do_recover);
// Store both GPT copies (primary and backup)
int nvmeibt_disk_metadata_store_gpt(struct netlink_io_context *nl_ctx, int dummy_fd,
									int pblk_size,
									struct nvmeibt_disk_gpt *gpt,
									bool init_serjio);
// Store a single GPT copy (primary or alternate)
int nvmeibt_disk_metadata_store_gpt_one_copy(struct netlink_io_context *nl_ctx,
											  int fd,
											  int pblk_size,
											  struct nvmeibt_disk_gpt *gpt,
											  BOOL is_alternate,
											  BOOL init_serjio);
int nvmeibt_disk_metadata_read_disk_metadata(struct netlink_io_context *nl_ctx, int fd,
											 int pblk_size,
											 uint64_t pbyte_s,
											 struct nvmeibt_disk_metadata *disk_metadata);

int nvmeibt_disk_metadata_write_disk_metadata_due_to_zeroing_progress(struct local_disk_info *ld_info, uint64_t pba_s, uint64_t pba_e);

void nvmeibt_disk_metadata_print_gpt_header(const struct nvmeibt_disk_gpt_header *gpt_header,
											const char *gpt_main_or_metadata_str, const char *gpt_primary_or_alternate_or_mem_str);
void nvmeibt_disk_metadata_print_gpt_entry(const struct nvmeibt_disk_gpt_partition_entry *entry, int index);
void nvmeibt_disk_metadata_print_all_gpt_entries(const struct nvmeibt_disk_gpt_partition_entry *gpt_entries, int n_partition_entries, BOOL is_skip_inactive,
												 const char *gpt_main_or_metadata_str,
												 const char *gpt_primary_or_alternate_or_mem_str,
												 const char *ldisk_id);
BOOL nvmeibt_disk_metadata_is_gpt_entry_in_use(const struct nvmeibt_disk_gpt_partition_entry *entry);
BOOL nvmeibt_disk_metadata_is_gpt_entry_active_and_matching_uuid(const union nvmeib_uuid *uuid, const struct nvmeibt_disk_gpt_partition_entry *entry);
BOOL nvmeibt_disk_metadata_are_gpt_headers_equal(struct nvmeibt_disk_gpt_header *pri, struct nvmeibt_disk_gpt_header *alt);
BOOL nvmeibt_disk_metadata_are_gpt_entries_equal(const struct nvmeibt_disk_gpt_partition_entry *pri, const struct nvmeibt_disk_gpt_partition_entry *alt, int n_bytes);
void nvmeibt_disk_metadata_fill_gpt_entry_str(const struct nvmeibt_disk_gpt_partition_entry *entry, int index, struct nvmeibt_Str *str_ctx);
void nvmeibt_disk_metadata_fill_all_gpt_entries_str(const struct nvmeibt_disk_gpt_partition_entry *gpt_entries,
													const int n_partition_entries,
													const BOOL is_skip_inactive,
													struct nvmeibt_Str *str_ctx,
													const char *gpt_main_or_metadata_str,
													const char *gpt_primary_or_alternate_or_mem_str);
void nvmeibt_disk_metadata_fill_gpt_header_str(const struct nvmeibt_disk_gpt_header *gpt_header, struct nvmeibt_Str *str_ctx,
											   const char *gpt_main_or_metadata_str, const char *gpt_primary_or_alternate_or_mem_str);
const char* nvmeibt_disk_metadata_get_partition_type_name(union nvmeib_uuid partition_type_guid);

#endif
