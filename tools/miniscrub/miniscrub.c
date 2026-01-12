#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/nvme_ioctl.h>

enum nvme_opcode {
	nvme_cmd_flush		= 0x00,
	nvme_cmd_write		= 0x01,
	nvme_cmd_read		= 0x02,
	nvme_cmd_write_uncor	= 0x04,
	nvme_cmd_compare	= 0x05,
	nvme_cmd_write_zeroes	= 0x08,
	nvme_cmd_dsm		= 0x09,
	nvme_cmd_verify		= 0x0c,
	nvme_cmd_resv_register	= 0x0d,
	nvme_cmd_resv_report	= 0x0e,
	nvme_cmd_resv_acquire	= 0x11,
	nvme_cmd_resv_release	= 0x15,
	nvme_cmd_zone_mgmt_send	= 0x79,
	nvme_cmd_zone_mgmt_recv	= 0x7a,
	nvme_cmd_zone_append	= 0x7d,
	nvme_cmd_vendor_start	= 0x80,
};

#define NUM_BLOCKS 512


unsigned char *data_buffer;
unsigned char *metadata_buffer;
bool write_temp_files = false;
bool exit_on_problems = true;

/// extern int infra_has_problems_in_block(u64 rlba, const unsigned char *data, bool debug_di_enabled, const bool is_parity, const union nvmeibc_block_dp_ec_data_block_md *md);
extern int infra_has_problems_in_block(uint64_t rlba, const unsigned char *data, bool debug_di_enabled, const bool is_parity, void *md);

int test_segment(char *volume_name, uint64_t praid_vlba, uint64_t data_disks, uint64_t parity_disks, char *drive_name, uint64_t segment_index, uint64_t dlba_start, uint64_t dlba_end, uint64_t drive_block_size, bool dbg_di, char *statuses[])
{
	uint64_t nb = 0;
	uint64_t slice_width = data_disks + parity_disks;
	int fd = open(drive_name, O_RDONLY);
	if (fd < 0) goto out;

	// fprintf(stderr, "data_buffer=%p metadata_buffer=%p\n", data_buffer, metadata_buffer);

	segment_index = slice_width - segment_index;

	data_buffer = aligned_alloc(4096, 4096 * NUM_BLOCKS);
	metadata_buffer = aligned_alloc(4096, 8 * NUM_BLOCKS);
	memset(data_buffer, 0xff, 4096 * NUM_BLOCKS);
	memset(metadata_buffer, 0xff, 8 * NUM_BLOCKS);


	for (uint64_t d1 = dlba_start; d1 <= dlba_end ; d1 += 32) { // Reading more than 32 blocks does not seem to work

		uint64_t d2 = d1 + 31;
		if (d2 > dlba_end) d2 = dlba_end;
		nb = d2 - d1 + 1;
		struct nvme_user_io io = {
			.opcode		= 2,
			.flags		= 0,
			.control	= 0,
			.nblocks	= d2 - d1, // zero-based
			.rsvd		= 0,
			.metadata	= (__u64)(uintptr_t) metadata_buffer,
			.addr		= (__u64)(uintptr_t) data_buffer,
			.slba		= d1,
			.dsmgmt = 0,
			.reftag = 0,
			.appmask = 0,
			.apptag = 0,
		};

		int ioctl_out;
		if ((ioctl_out = ioctl(fd, NVME_IOCTL_SUBMIT_IO, &io)) < 0) {
			perror("NVMe ioctl failed");
			fprintf(stderr, "d1=%ld, d2=%ld\n", d1, d2);
			break;
		}
		if (ioctl_out > 0) {
			fprintf(stderr, "NVME_IOCTL_SUBMIT_IO returned 0x%x\n", ioctl_out);
			fprintf(stderr, "d1=%ld, d2=%ld\n", d1, d2);
		}

		unsigned char *db = data_buffer;
		unsigned char *mdb = metadata_buffer;
		for (uint64_t d = d1; d <= d2; ++d, db += 4096, mdb += 8) {
			uint64_t slice_offset = d - dlba_start; // ignoring 512b blocks for now
			// uint64_t role = (segment_index - slice_offset / 64 /* 32 blocks * 2 count */) % slice_width;
			uint64_t role = (slice_width - ((segment_index + slice_offset / 64 /* 32 blocks * 2 count */) % slice_width)) % slice_width ;
			bool is_parity = role >= data_disks;
			uint64_t rlba = slice_offset * data_disks + (is_parity ? 0 : role);
			int problems = infra_has_problems_in_block(rlba, db, dbg_di, is_parity || (parity_disks == 1 && data_disks == 1), mdb);
			// fprintf(stderr, "d=%ld, slice_offset=%ld, rlba=%ld, problems='%c' role=%ld is_parity=%c\n", d, slice_offset, rlba, problems, role, is_parity ? 'Y' : 'N');
			if (!problems || problems == 'V' || problems == 'Z')
				continue;
			fprintf(stderr, "d=%ld, slice_offset=%ld, rlba=%ld, problems='%c' role=%ld is_parity=%c\n", d, slice_offset, rlba, problems, role, is_parity ? 'Y' : 'N');
			if (problems == 'C') { // let's check if it's just a role thing
				for (role = 0; role < data_disks; role++) {
					is_parity = false;
					rlba = slice_offset * data_disks + (is_parity ? 0 : role);
					problems = infra_has_problems_in_block(rlba, db, dbg_di, is_parity, mdb);
					fprintf(stderr, "role %ld problems '%c'\n", role, problems);
				}
				if (parity_disks > 0) {
					is_parity = true;
					rlba = slice_offset * data_disks + (is_parity ? 0 : role);
					problems = infra_has_problems_in_block(rlba, db, dbg_di, is_parity, mdb);
					fprintf(stderr, "parity problems '%c'\n", problems);
				}
			}

			if (write_temp_files) {
				char tf[100];

				sprintf(tf, "block_d_%lx", d);
				int fd1 = open(tf, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH | S_IWOTH);
				if (fd1 >= 0) {
					int w = write(fd1, db, 4096);
					close(fd1);
				}

				sprintf(tf, "block_d_%lx.md", d);
				fd1 = open(tf, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH | S_IWOTH);
				if (fd1 >= 0) {
					int w = write(fd1, mdb, 8);
					close(fd1);
				}
				if (exit_on_problems) goto out;
			}
		}
	}

out:
	close(fd);
	return 0;
}

#ifdef COMPILE_FOR_TEST
int main(int argc, char *argv[])
{
	char *volume_name = "<unknown>";
	char *drive_name = "/dev/nvme1000n1";
	uint64_t data_disks = 8, parity_disks = 2, segment_index=3;
	int i;

	uint64_t dlba_start = 0, dlba_end = 0;
	if (argc <= 1) {
		fprintf(stderr, "Version: 1.0, args:\n");
		fprintf(stderr, "\t 1=dlba_start,  2=dlba_end\n");
		fprintf(stderr, "\t 3=volume_name, 4=drive_name\n");
		fprintf(stderr, "\t 5,6 is D+P of praid\n");
		fprintf(stderr, "\t 7, is segment index in praid [0..D+P-1]\n");
		fprintf(stderr, "\t 8, do output to file (boolean), default=%u\n", write_temp_files);
		fprintf(stderr, "\t 9, exit on first problem (boolean), default=%u\n", exit_on_problems);
		return -1;
	}
	if (argc > 1) { sscanf(argv[1], "%ld", &dlba_start); }
	if (argc > 2) { sscanf(argv[2], "%ld", &dlba_end); }
	if (argc > 3) { volume_name = argv[3]; }
	if (argc > 4) { drive_name = argv[4]; }
	if (argc > 5) { sscanf(argv[5], "%ld", &data_disks); }
	if (argc > 6) { sscanf(argv[6], "%ld", &parity_disks); }
	if (argc > 7) { sscanf(argv[7], "%ld", &segment_index); }
	if (argc > 8) { sscanf(argv[8], "%d", &i); write_temp_files = (i == 1); }
	if (argc > 9) { sscanf(argv[9], "%d", &i); exit_on_problems = (i == 1); }

	// int test_segment(char *volume_name, uint64_t praid_vlba, uint64_t data_disks, uint64_t parity_disks, char *drive_name, uint64_t segment_index, uint64_t dlba_start, uint64_t dlba_end, uint64_t drive_block_size, bool dbg_di, char *statuses[])
	int p = test_segment(volume_name, 0, data_disks, parity_disks, drive_name, segment_index, dlba_start, dlba_end, 4096, false, NULL);
	return p;
}
#endif // COMPILE_FOR_TEST
