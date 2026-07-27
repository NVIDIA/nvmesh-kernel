#include "common/kr_incs.h" /*Must be first*/
#include "nvmeib_shared.h"
#include "nvmeib_types.h"
#include "clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_blk.h"
#include "clnt/block/datapath_utils_generic/nvmeibc_block_dp_block_md.h"
#include "clnt/block/datapath_ec/nvmeibc_block_dp_ec_gf_praid.h"

extern struct nvmeib_get_disk_names_reply dummy1;
extern struct nlmsghdr dummy2;
extern struct nvmeib_nl_uk_comm_msg dummy3;
extern struct nvmeib_nl_uk_comm_rep dummy4;
extern struct nvmeib_io_to_disk dummy5;
extern struct t_data_blk dummy6;
extern enum uk_comm_opcode dummy7;
extern struct nvmeib_io_to_disk_reply dummy8;
extern u64 get_j2d(union jblock_md *md);
extern void set_j2d(union jblock_md *md, u64 j2d);
extern bool infra_was_data_never_written(const union nvmeibc_block_dp_ec_data_block_md *md);
extern int infra_has_problems_in_block(u64 rlba, const unsigned char *data, bool debug_di_enabled, const bool is_parity, const union nvmeibc_block_dp_ec_data_block_md *md);

int main(int argc, const char *argv[]) {
	union jblock_md jmd = {0};
	union nvmeibc_block_dp_ec_data_block_md dmd = {0};
	fprintf(stderr, "Starting %s.so tests\n", argv[0]);

	// Test all .so functions
	{
		u64 set, rv;
		jmd.version = NVMEIBC_JOURNAL_MD_VERSION_PACKED;
		jmd.j2d_0 = 8;
		rv = get_j2d(&jmd);
		BUG_ON(rv != 8);
		set_j2d(&jmd, 17);
		rv = get_j2d(&jmd);
		BUG_ON(rv != 17);
		set = 18UL | (1UL << 32);
		set_j2d(&jmd, set);
		rv = get_j2d(&jmd);
		BUG_ON(rv != set);
		BUG_ON(jmd.v1.j2d_extended != 1);
		set = 19UL | (1UL << 55);
		set_j2d(&jmd, set);
		rv = get_j2d(&jmd);		// Mote rh
		WARN(rv == set, "rv=0x%llx != 0x%llx\n", rv, set);
	}
	{
		bool rv = infra_was_data_never_written(&dmd);
		BUG_ON(rv != 1);
	}
	{
		unsigned char data[4096] = {1,2,3, 0};
		int rv = infra_has_problems_in_block(23, data, true, false, &dmd);
		WARN(rv != 'V', "rv=%c\n", (char)rv);		// Virgin data
		dmd.D.version = NVMEIBC_DATA_MD_VERSION;
		rv = infra_has_problems_in_block(23, data, true, false, &dmd);
		WARN(rv != 'C', "rv=%c\n", (char)rv);		// Edic problem
	}
	{	// Access to structs

	}

	fprintf(stderr, "Starting %s.so tests done\n", argv[0]);
	return 0;
}

