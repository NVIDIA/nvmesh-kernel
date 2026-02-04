#include "common/kr_incs.h"
#include "nvmeib_scatterlist_iter_test.h"
#include "common/nvmeib_scatterlist_iter.h"

#define SG_BLK_ITER_TEST_SECTOR_SIZE 4096

/**
 * Test case 1: Simple case - 3 SG entries, each 4KB (one sector each)
 * Expected: nvmeib_scatterlist_block_iter_next returns true 3 times, then false
 */
static void test_nvmeib_scatterlist_iter_simple_3_entries(void)
{
	struct scatterlist sg[3];
	struct nvmeib_scatterlist_block_iter iter;
	u8 *buf1, *buf2, *buf3;
	int count = 0;

	/* Allocate 3 buffers, each 4KB */
	buf1 = kzalloc(SG_BLK_ITER_TEST_SECTOR_SIZE, GFP_KERNEL);
	buf2 = kzalloc(SG_BLK_ITER_TEST_SECTOR_SIZE, GFP_KERNEL);
	buf3 = kzalloc(SG_BLK_ITER_TEST_SECTOR_SIZE, GFP_KERNEL);
	BUG_ON(!buf1 || !buf2 || !buf3);

	/* Fill with unique patterns for verification */
	memset(buf1, 0x11, SG_BLK_ITER_TEST_SECTOR_SIZE);
	memset(buf2, 0x22, SG_BLK_ITER_TEST_SECTOR_SIZE);
	memset(buf3, 0x33, SG_BLK_ITER_TEST_SECTOR_SIZE);

	/* Initialize SG list with 3 entries */
	sg_init_table(sg, 3);
	sg_set_buf(&sg[0], buf1, SG_BLK_ITER_TEST_SECTOR_SIZE);
	sg_set_buf(&sg[1], buf2, SG_BLK_ITER_TEST_SECTOR_SIZE);
	sg_set_buf(&sg[2], buf3, SG_BLK_ITER_TEST_SECTOR_SIZE);

	/* Initialize iterator */
	nvmeib_scatterlist_block_iter_init(&iter, SG_BLK_ITER_TEST_SECTOR_SIZE, sg, 3);

	/* Iterate and verify */
	while (nvmeib_scatterlist_block_iter_next(&iter)) {
		switch (count) {
		case 0:
			BUG_ON(iter.data != buf1);
			BUG_ON(iter.data[0] != 0x11);
			break;
		case 1:
			BUG_ON(iter.data != buf2);
			BUG_ON(iter.data[0] != 0x22);
			break;
		case 2:
			BUG_ON(iter.data != buf3);
			BUG_ON(iter.data[0] != 0x33);
			break;
		default:
			BUG_ON(1); /* Should not reach here */
		}
		count++;
	}

	BUG_ON(count != 3); /* Must have iterated exactly 3 times */

	kfree(buf1);
	kfree(buf2);
	kfree(buf3);

	pr_info("test_nvmeib_scatterlist_iter_simple_3_entries: PASSED\n");
}

/**
 * Test case 2: 2 SG entries with larger buffers (16KB + 8KB = 24KB total)
 * n_ents = 2, but total sectors = 6 (16KB/4KB + 8KB/4KB = 4 + 2)
 * Expected: nvmeib_scatterlist_block_iter_next returns true 6 times, then false
 */
static void test_nvmeib_scatterlist_iter_multi_sector_entries(void)
{
	struct scatterlist sg[2];
	struct nvmeib_scatterlist_block_iter iter;
	u8 *buf1, *buf2;
	u32 count = 0;
	const u32 buf1_size = 16 * 1024;  /* 16KB = 4 sectors */
	const u32 buf2_size = 8 * 1024;   /* 8KB = 2 sectors */
	const u32 expected_iterations = (buf1_size + buf2_size) / SG_BLK_ITER_TEST_SECTOR_SIZE; /* 6 */

	/* Allocate buffers */
	buf1 = kzalloc(buf1_size, GFP_KERNEL);
	buf2 = kzalloc(buf2_size, GFP_KERNEL);
	BUG_ON(!buf1 || !buf2);

	/* Fill with patterns: buf1 has sectors 0-3, buf2 has sectors 4-5 */
	memset(buf1, 0xAA, buf1_size);
	memset(buf2, 0xBB, buf2_size);

	/* Mark each sector with unique byte at start for verification */
	buf1[0 * SG_BLK_ITER_TEST_SECTOR_SIZE] = 0x01;
	buf1[1 * SG_BLK_ITER_TEST_SECTOR_SIZE] = 0x02;
	buf1[2 * SG_BLK_ITER_TEST_SECTOR_SIZE] = 0x03;
	buf1[3 * SG_BLK_ITER_TEST_SECTOR_SIZE] = 0x04;
	buf2[0 * SG_BLK_ITER_TEST_SECTOR_SIZE] = 0x05;
	buf2[1 * SG_BLK_ITER_TEST_SECTOR_SIZE] = 0x06;

	/* Initialize SG list with 2 entries */
	sg_init_table(sg, 2);
	sg_set_buf(&sg[0], buf1, buf1_size);
	sg_set_buf(&sg[1], buf2, buf2_size);

	/* Initialize iterator with n_ents = 2 */
	nvmeib_scatterlist_block_iter_init(&iter, SG_BLK_ITER_TEST_SECTOR_SIZE, sg, 2);

	/* Iterate and verify each sector */
	while (nvmeib_scatterlist_block_iter_next(&iter)) {
		u8 expected_marker = (u8)(count + 1);
		BUG_ON(iter.data[0] != expected_marker);

		/* Verify data pointer is at correct offset */
		if (count < 4) {
			BUG_ON(iter.data != buf1 + count * SG_BLK_ITER_TEST_SECTOR_SIZE);
		} else {
			BUG_ON(iter.data != buf2 + (count - 4) * SG_BLK_ITER_TEST_SECTOR_SIZE);
		}
		count++;
	}

	BUG_ON(count != expected_iterations); /* Must have iterated exactly 6 times */

	kfree(buf1);
	kfree(buf2);

	pr_info("test_nvmeib_scatterlist_iter_multi_sector_entries: PASSED (iterated %d times)\n", count);
}

void nvmeib_scatterlist_iter_tests(void)
{
	pr_info("Running nvmeib_scatterlist_block_iter tests...\n");

	test_nvmeib_scatterlist_iter_simple_3_entries();
	test_nvmeib_scatterlist_iter_multi_sector_entries();

	pr_info("All nvmeib_scatterlist_block_iter tests PASSED!\n");
}
