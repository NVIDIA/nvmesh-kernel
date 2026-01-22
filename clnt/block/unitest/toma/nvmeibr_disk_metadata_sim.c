// For documentation, see Header in H file
/*****************************************************************************/
// Includes
#include "nvmeibr_disk_metadata_sim.h"
#include "../server/nvmeibs_main_sim.h"
#include "uni_framework/range_algorithms.h"
#include "mgmt/nvmeibm_conf_db.h"
#include "nvmeibs_serjio_gpt.h"
static inline bool is_not_null(const struct disk_range** p){ return !!(*p);}

static int __get_num_gpt_entries(struct serverSimulator *srv) {
	struct disk_sgmnts disk_sgmnts = tTopoOfNVMesh_list_disk_sgmnts(srv->simToma.globalTopo, srv->ramDisk.uniqueID, true);
	int num_of_segs_on_disk = array_count_if (disk_sgmnts.sgmnts, is_not_null);
	int num_additional_entries = 3; // Toma metadata partition, Journal, Serjio-DB
	(void)srv;
	return num_of_segs_on_disk + num_additional_entries;
}

static void __gpt_hdr_encode(struct gpt_header *g) {
	g->signature =						cpu_to_le64(g->signature);
	g->revision =						cpu_to_le32(g->revision);
	g->header_size =					cpu_to_le32(g->header_size);
	//g->header_crc32 =					cpu_to_le32(g->header_crc32);
	g->my_lba =							cpu_to_le64(g->my_lba);
	g->alternate_lba =					cpu_to_le64(g->alternate_lba);
	g->first_usable_lba =				cpu_to_le64(g->first_usable_lba);
	g->last_usable_lba =				cpu_to_le64(g->last_usable_lba);
	g->partition_entry_lba =			cpu_to_le64(g->partition_entry_lba);
	g->num_partition_entries =			cpu_to_le32(g->num_partition_entries);
	g->sizeof_partition_entry =			cpu_to_le32(g->sizeof_partition_entry);
	//g->partition_entry_array_crc32 =	cpu_to_le32(g->partition_entry_array_crc32);
}

void nvmeibr_disk_metadata_store_gpt(struct serverSimulator *srv, struct gpt_header *dst, bool primary) {
	unsigned long disk_blocks = srv->ramDisk.server_disk.di.hw_blocks;
	unsigned num_gpt_ents = __get_num_gpt_entries(srv);
	unsigned hdr_num_ents = (num_gpt_ents > GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES ? GPT_HDR_BIOS_WORKAROUND_NUM_ENTRIES : num_gpt_ents);
	unsigned part_ents_num_lba = DIV_ROUND_UP(num_gpt_ents * sizeof(struct gpt_entry), (1 << srv->ramDisk.sector_shift));
	dst->signature =					GPT_HEADER_SIGNATURE;
	dst->revision =						GPT_HEADER_REVISION_V1;
	dst->header_size =					sizeof(*dst);
	if (primary) {
		dst->my_lba = GPT_PRIMARY_HDR_LBA;
		dst->alternate_lba = (disk_blocks - 1);
		dst->partition_entry_lba = GPT_PRIMARY_ENTS_START_LBA;
	} else {
		dst->my_lba = disk_blocks - 1;
		dst->partition_entry_lba = dst->my_lba - 1 - part_ents_num_lba;
		dst->alternate_lba = GPT_PRIMARY_HDR_LBA;
	}
	dst->first_usable_lba = GPT_PRIMARY_ENTS_START_LBA + part_ents_num_lba;
	dst->last_usable_lba = (disk_blocks - 1) - 1 - part_ents_num_lba;
	memcpy(&dst->disk_guid, srv->hardware->disk_name, sizeof(efi_guid_t));
	dst->partition_entry_lba = 			primary ? GPT_PRIMARY_ENTS_START_LBA : disk_blocks - 2;
	dst->num_partition_entries = hdr_num_ents;

	dst->sizeof_partition_entry = 		sizeof(struct gpt_entry);
	nvmeibr_disk_metadata_store_entries(srv, NULL, num_gpt_ents, &dst->partition_entry_array_crc32);
	__gpt_hdr_encode(dst);
	dst->header_crc32 = 0;											// Must fill at the end
	dst->header_crc32 = (crc32(~0L, dst, sizeof(*dst)) ^ ~0L);		// != crc32c, != crc32 calculated in reed solomon, but a version compatible to efi
}

static void  __gpt_entry_encode(struct gpt_entry *g) {
	g->starting_lba =					cpu_to_le64(g->starting_lba);
	g->ending_lba =						cpu_to_le64(g->ending_lba);
	g->attributes =						cpu_to_le64(g->attributes);
}

#define TOMA_MD_PARTITION_GUID   "12345678-abcd-1414-fefe-543217789801"		// Just a number, no particular reason
#define JOUR_PARTITION_GUID      "20909090-0000-0000-0000-000000000000"
#define SERJIO_DB_PARTITION_GUID "30909090-0000-0000-0000-000000000000"
void nvmeibr_disk_metadata_store_entries(struct serverSimulator *srv, struct gpt_entry *ent, unsigned max_ent, u32 *ent_crc) {
	struct ramDiskSimulator * D = &srv->ramDisk;
	const union nvmeib_uuid toma_md_uuid = NVMESH_METADATA_PARTITION_TYPE_GUID_CONST;
	const union nvmeib_uuid journal_uuid = NVMESH_JOURNAL_DATA_PARTITION_TYPE_GUID_CONST;
	const union nvmeib_uuid serj_db_uuid = NVMESH_SERJIO_DB_PARTITION_TYPE_GUID_CONST;
	const union nvmeib_uuid seg_ec__uuid = NVMESH_DATA_PARTITION_TYPE_GUID_JOURNALED_CONST;
	u64 serjio_jour_start = ~0, serjio_jour_length = ~0, serjio_db_start = ~0, serjio_db_length = ~0;
	//const union nvmeib_uuid seg_noj_uuid = NVMESH_DATA_PARTITION_TYPE_GUID_NO_JOURNAL_CONST;
	struct gpt_entry tmp;
	struct gpt_entry *cur = ent ? ent : &tmp;								// Entries iterator
	struct disk_sgmnts disk_sgmnts = tTopoOfNVMesh_list_disk_sgmnts(srv->simToma.globalTopo, srv->ramDisk.uniqueID, true);
	u16 sgmnt_idx;
	unsigned ent_idx = 0;

	BUG_ON(max_ent == 0);

	if (ent_crc)
		*ent_crc = ~0L;

	ramDiskSimulator_get_serjio_partions_sectors_ranges(D, &serjio_jour_start, &serjio_jour_length, &serjio_db_start, &serjio_db_length);

	// Partition 1: Toma metadata partition
	memcpy(&cur->partition_type_guid, &toma_md_uuid, sizeof(efi_guid_t));
	uuid_parse(TOMA_MD_PARTITION_GUID, &cur->unique_partition_guid.b[0]);
	cur->starting_lba = cur->ending_lba = ~0ULL;					// Curently never written nor read by the simulators
	cur->attributes   = 0;											// Unused
	memset(&cur->partition_name, 0, sizeof(cur->partition_name));
	memcpy(&cur->partition_name, TOMA_MD_PARTITION_GUID, 16);
	__gpt_entry_encode(cur);

	if (ent_crc)
		*ent_crc = crc32(*ent_crc, cur, sizeof(*cur));
	if (cur != &tmp)
		cur++;

	if (++ent_idx == max_ent)
		goto out;

	// Partition 2: Journal entries (2GB on real disks)
	memcpy(&cur->partition_type_guid, &journal_uuid, sizeof(efi_guid_t));
	uuid_parse(JOUR_PARTITION_GUID, &cur->unique_partition_guid.b[0]);
	cur->starting_lba = serjio_jour_start;
	cur->ending_lba = serjio_jour_start + serjio_jour_length - 1;
	cur->attributes   = 0;											// Unused
	memset(&cur->partition_name, 0, sizeof(cur->partition_name));
	memcpy(&cur->partition_name, JOUR_PARTITION_GUID, 16);
	__gpt_entry_encode(cur);

	if (ent_crc)
		*ent_crc = crc32(*ent_crc, cur, sizeof(*cur));
	if (cur != &tmp)
		cur++;
	if (++ent_idx == max_ent)
		goto out;

	// Partition 3: Serjio-db
	memcpy(&cur->partition_type_guid, &serj_db_uuid, sizeof(efi_guid_t));
	uuid_parse(SERJIO_DB_PARTITION_GUID, &cur->unique_partition_guid.b[0]);
	cur->starting_lba = serjio_db_start;
	cur->ending_lba = serjio_db_start + serjio_db_length - 1;
	cur->attributes   = 0;											// Unused
	memset(&cur->partition_name, 0, sizeof(cur->partition_name));
	memcpy(&cur->partition_name, SERJIO_DB_PARTITION_GUID, 16);
	__gpt_entry_encode(cur);

	if (ent_crc)
		*ent_crc = crc32(*ent_crc, cur, sizeof(*cur));
	if (cur != &tmp)
		cur++;
	if (++ent_idx == max_ent)
		goto out;

	// Partition 4..: Segments.  seg_ec__uuid, seg_noj_uuid
	for (sgmnt_idx = 0; (sgmnt_idx < ARRAY_SIZE(disk_sgmnts.sgmnts)) && disk_sgmnts.sgmnts[sgmnt_idx]; ++sgmnt_idx){
		const struct disk_range *sgmnt = disk_sgmnts.sgmnts[sgmnt_idx];
		memcpy(&cur->partition_type_guid, &seg_ec__uuid, sizeof(efi_guid_t));
		uuid_parse(sgmnt->ruuid, &cur->unique_partition_guid.b[0]);
		cur->starting_lba = sgmnt->dlba_start;
		cur->ending_lba =   sgmnt->dlba_start + sgmnt->length;
		cur->attributes   = 0;											// Unused
		memset(&cur->partition_name, 0, sizeof(cur->partition_name));
		memcpy(&cur->partition_name, &cur->unique_partition_guid, sizeof(efi_guid_t));
		__gpt_entry_encode(cur);

		if (ent_crc)
			*ent_crc = crc32(*ent_crc, cur, sizeof(*cur));
		if (cur != &tmp)
			cur++;
		if (++ent_idx == max_ent)
			goto out;
	}

	// Fill the rest with empty entries
	for (; sgmnt_idx < max_ent; sgmnt_idx++) {
		memset(cur, 0, sizeof(*cur));
		if (ent_crc)
			*ent_crc = crc32(*ent_crc, cur, sizeof(*cur));
		if (cur != &tmp)
			cur++;
		if (++ent_idx == max_ent)
			goto out;
	}

out:
	if (ent_crc)
		*ent_crc = *ent_crc ^ ~0L;
}

/*****************************************************************************/
// EOF.

