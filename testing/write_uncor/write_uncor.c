/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/mman.h>
#include <stddef.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <sys/uio.h>
#include <errno.h>

#include <sys/types.h>
#include <fcntl.h>
#include <libaio.h>
#include <poll.h>
#include <sys/eventfd.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>


#define DISK_4KB_BLK_SIZE	(4096)
#define NUM_BLKS_IN_BLKSET 	32
#define DISK_BLKSET_SIZE (NUM_BLKS_IN_BLKSET * DISK_4KB_BLK_SIZE)
#define SECTOR_SIZE 512

int lseek_blk(int fd, uint64_t blkset_num, uint64_t blk_num)
{
	off_t fd_offset;
	off_t retval;
	int rv = 0;

	//lseek to 1st blk in blkset
	fd_offset = blkset_num * NUM_BLKS_IN_BLKSET * DISK_4KB_BLK_SIZE +
		        blk_num * DISK_4KB_BLK_SIZE;
	if((retval = lseek(fd, fd_offset, SEEK_SET)) != fd_offset) {
		printf("Failed lseek (retval = %ld, '%m')\n", retval);
		rv = -1;
	}

	return rv;
}

int read_blk(int fd, char *disk_blk_buf)
{
	off_t retval;
	int rv = 0;

	//read blk from disk
	if ((retval = read(fd, disk_blk_buf, DISK_4KB_BLK_SIZE)) != DISK_4KB_BLK_SIZE) {
		printf("Failed to read data from disk (retval = %ld, '%m')\n", retval);
		rv = -1;
	}

	return rv;
}


int write_blk(int fd, char *disk_blk_buf)
{
	off_t retval;
	int rv = 0;

	//write blk to disk
	if ((retval = write(fd, disk_blk_buf, DISK_4KB_BLK_SIZE)) != DISK_4KB_BLK_SIZE) {
		printf("Failed to write data from disk (retval = %ld, '%m')\n", retval);
		rv = -1;
	}

	return rv;
}

#include <linux/nvme.h>
#include <sys/ioctl.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

int wr_ucerr_blk(int fd, uint64_t blkset_num, uint64_t blk_num)
{
#ifdef NVME_IOCTL_IO_CMD
//	struct nvmeibt_local_disk_config *cfg =
//		&disk_segment->its_disk->its_local_disk->from_config;
	struct nvme_admin_cmd cmd = {0};
	u64 s4kb, slba;
	u16 n4kb, nlb;
	int block_size = SECTOR_SIZE;
	int rv = -1;

	printf("wr-ucerr: blkset_num=%lld, blk_num=%lld\n", blkset_num, blk_num);

	s4kb = (/*(u64)disk_segment->from_config.lb_s + */
			(u64)blkset_num * NUM_BLKS_IN_BLKSET) + blk_num;

	n4kb = 1;
	if (block_size <= DISK_4KB_BLK_SIZE) {
		slba = s4kb * (DISK_4KB_BLK_SIZE / block_size);
		nlb =  n4kb * (DISK_4KB_BLK_SIZE / block_size);
	}
	else {
		printf("Unsupported sector size %d\n", block_size);
		goto out;
	}

	printf("submit wr-ucerr: nsid %d, slba=%lld, nlb=%d "
		"(sector-size %d, s4kb=%lld, n4kb=%d)\n",
		1, slba, nlb, block_size, s4kb, n4kb);
	cmd.opcode	= nvme_cmd_write_uncor;
	cmd.nsid	= 1; //cfg->nsid;
	cmd.cdw10	= slba & 0xffffffff;
	cmd.cdw11	= slba >> 32;
	cmd.cdw12	= nlb;

	if (ioctl(fd, NVME_IOCTL_IO_CMD, &cmd)) {
		printf("Failed wr-ucerr ioctl\n");
		goto out;
	}
	rv = 0;

out:
	return rv;

#else
	printf("Unsupported ioctl request NVME_IOCTL_IO_CMD\n");
	return -1;
#endif
}

void test(char *dev_file_name)
{
	int fd = -1;
	off_t fd_offset;
	off_t retval;
	char disk_blk_buf_[DISK_BLKSET_SIZE*2];
	char *disk_blk_buf = (disk_blk_buf_ + DISK_BLKSET_SIZE -
						  ((u64)((void *)disk_blk_buf_))%DISK_BLKSET_SIZE);
	uint64_t blkset_num = 0;
	uint64_t blk_num = 0;
	uint64_t len = DISK_4KB_BLK_SIZE;
	int ii;

	//open nvme device
	if ((fd = open(dev_file_name, (O_RDWR | O_DIRECT | O_SYNC))) < 0) {
		printf("Failed to open dev file %s\n", dev_file_name);
		goto out;
	}
	printf("open dev file %s\n", dev_file_name);


	//---------------------------------
	printf("-----------------------\n");
	printf("lseek...\n");
	if (lseek_blk(fd, blkset_num, blk_num))
		goto out;

	printf("read...\n");
	if (read_blk(fd, disk_blk_buf))
		goto rd_err;

	//---------------------------------
	printf("-----------------------\n");
	printf("write ucerr...\n");
	if (wr_ucerr_blk(fd, blkset_num, blk_num))
		goto out;

	//---------------------------------
	printf("-----------------------\n");
	printf("lseek...\n");
	if (lseek_blk(fd, blkset_num, blk_num))
		goto out;

	ii = 0;
	while (ii++ < 1) {
		printf("read...\n");
		if (read_blk(fd, disk_blk_buf))
			printf("read failed try write and read again... (ii %d)\n", ii);
	}

rd_err:
	//---------------------------------
	printf("-----------------------\n");
	printf("lseek...\n");
	if (lseek_blk(fd, blkset_num, blk_num))
		goto out;

	printf("write...\n");
	if (write_blk(fd, disk_blk_buf))
		goto out;

	//---------------------------------
	printf("-----------------------\n");
	printf("lseek...\n");
	if (lseek_blk(fd, blkset_num, blk_num))
		goto out;

	printf("read...\n");
	if (read_blk(fd, disk_blk_buf))
		goto out;

out:
	printf("-----------------------\n");
	printf("Bye...\n");
	return;
}

int main (int argc, char *argv[])
{
	if (argc != 2) {
		printf("Usage: sudo ./write_uncor /devnvmeXnY\n");
		return 0;
	}
	test(argv[1]);
	return 0;
}

