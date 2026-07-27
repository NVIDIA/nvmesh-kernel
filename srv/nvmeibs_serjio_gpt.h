#ifndef NVMEIBS_SERJIO_GPT_H
#define NVMEIBS_SERJIO_GPT_H

#include "kr_incs.h"
#include "nvmeib.h"
#include "nvmeibs_types.h"

#define GPT_HEADER_SIGNATURE 0x5452415020494645ULL
#define GPT_HEADER_REVISION_V1 0x00010000
#define GPT_PRIMARY_HDR_LBA 1
#define GPT_PRIMARY_ENTS_START_LBA 2

struct gpt_header {
	__le64 signature;
	__le32 revision;
	__le32 header_size;
	__le32 header_crc32;
	__le32 reserved1;
	__le64 my_lba;
	__le64 alternate_lba;
	__le64 first_usable_lba;
	__le64 last_usable_lba;
	efi_guid_t disk_guid;
	__le64 partition_entry_lba;
	__le32 num_partition_entries;
	__le32 sizeof_partition_entry;
	__le32 partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry {
	efi_guid_t partition_type_guid;
	efi_guid_t unique_partition_guid;
	__le64 starting_lba;
	__le64 ending_lba;
	__le64 attributes;
	efi_char16_t partition_name[72 / sizeof (efi_char16_t)];
} __attribute__((packed));

#endif //NVMEIBS_SERJIO_H
