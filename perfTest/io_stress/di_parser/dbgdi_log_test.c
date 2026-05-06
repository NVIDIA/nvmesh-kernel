/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "common/kr_incs.h"
#include "../../../clnt/block/datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.h"

#define BLOCK_SIZE 4096
#define BLOCK_LOG_SIZE DBG_DI_INJ_SPACE

#define MAX_BREC_SIZE BLOCK_LOG_SIZE

void dbgdi_log_print_records(struct dbgdi_log *log)
{
	u8 buf[MAX_BREC_SIZE];
	struct dbgdi_log_entry *e;
	int rc, loc = dbgdi_log_iter_init(log);

	if (loc == -1) {
		printf("%s - can't init log iterator\n", __func__);
		return;
	}

	printf("%s size %d occupancy %d free space %d\n", __func__, log->header.size, dbgdi_log_occupancy(log), dbgdi_log_free_space(log));

	do {
		rc = dbgdi_log_get_rec(log, loc, buf, MAX_BREC_SIZE);
		if (rc != -1) {
			e = db_entry(&buf[0]);
			printf("offset %.4d type %d size %d\n", loc, e->type, e->size);
		}
		loc = dbgdi_log_iter_next(log, loc);
		if (loc < 0) {
			printf("%s - iter_next failed (corrupt log?)\n", __func__);
			break;
		}
	} while (loc != (int)log->header.head);
}

void test_dbgdi_log_block(void)
{
	struct dbgdi_log src_blk, dst_blk;
	struct dbgdi_log *slog = &src_blk;
	struct dbgdi_log *dlog = &dst_blk;
	u8 db_rec[MAX_BREC_SIZE];

	printf("\n\n");
	printf("*** dbgdi_log block uni-test, block size %d log buf size %ld max record size %ld\n", BLOCK_SIZE, BLOCK_LOG_SIZE, MAX_BREC_SIZE);

	memset(db_rec, 0xff, MAX_BREC_SIZE);

	dbgdi_log_init(slog, BLOCK_LOG_SIZE);

	dbgdi_log_add_rec(slog, db_rec, 1, 256);
	dbgdi_log_add_rec(slog, db_rec, 2, 1277);
	dbgdi_log_add_rec(slog, db_rec, 3, 2212);
	dbgdi_log_add_rec(slog, db_rec, 4, 63);
	dbgdi_log_add_rec(slog, db_rec, 5, 1200);

	printf("log written to block\n");
	dbgdi_log_print_records(&src_blk);

	/* poison dst block */
	memset(&dst_blk, 0xbb, sizeof(dst_blk));

	/* use copy to emulate disk write from src and read to dst */
	memcpy(&dst_blk, &src_blk, sizeof(dst_blk));

	printf("log read (e.g from file)\n");
	dbgdi_log_print_records(dlog);
}


#define MAX_REC_SIZE 512
#define MAX_REC_SIZE_U64 (MAX_REC_SIZE >> 3)

void test_dbgdi_log_get_record(struct dbgdi_log *log, int type, int size, u8 val)
{
	u64 db_rec_out[MAX_REC_SIZE_U64], db_rec_expected[MAX_REC_SIZE_U64];
	struct dbgdi_log_entry *e;
	int rc;

	e = db_entry(db_rec_expected);
	e->type = type;
	e->size = size;
	memset((void *)db_rec_expected + sizeof(*e), val, size - sizeof(*e));

	rc = dbgdi_log_get_record_by_type(log, type, db_rec_out, size);

	if (rc)
		printf("can't get record\n");
	else if (memcmp(db_rec_out, db_rec_expected, size))
		printf("record content doesn't matches the expected %llx %llx\n",
			*db_rec_out, *db_rec_expected);
}

enum {
	SIZE_1 = 510,
	SIZE_2 = 12,
	SIZE_3 = 4,
	SIZE_4 = 5,
	SIZE_5 = 64,
	SIZE_6 = 128,
};

struct dbgdi_log_test_container {
	struct dbgdi_log log;
	u8 buf[0];
};

void test_dbgdi_log(int log_size)
{
	u8 *buf = malloc(sizeof(struct dbgdi_log_test_container) + log_size);
	struct dbgdi_log_test_container *dbgdi_log_t = (struct dbgdi_log_test_container *)buf;
	struct dbgdi_log *log = &dbgdi_log_t->log;
	u8 db_rec[MAX_REC_SIZE];

	printf("\n\n");
	printf("*** dbgdi_log basic uni-test, log buf size %d max record size %d\n", log_size, MAX_REC_SIZE);

	memset(db_rec, 0xff, MAX_REC_SIZE);

	dbgdi_log_print_records(log);

	dbgdi_log_init(log, log_size);
	dbgdi_log_print_records(log);

	memset(db_rec, 0x11, SIZE_1);
	dbgdi_log_add_rec(log, db_rec, 1, SIZE_1);
	memset(db_rec, 0x22, SIZE_2);
	dbgdi_log_add_rec(log, db_rec, 2, SIZE_2);
	memset(db_rec, 0x33, SIZE_3);
	dbgdi_log_add_rec(log, db_rec, 3, SIZE_3);
	memset(db_rec, 0x44, SIZE_4);
	dbgdi_log_add_rec(log, db_rec, 4, SIZE_4);
	memset(db_rec, 0x55, SIZE_5);
	dbgdi_log_add_rec(log, db_rec, 5, SIZE_5);
	memset(db_rec, 0x66, SIZE_5);
	dbgdi_log_add_rec(log, db_rec, 6, SIZE_6);
	dbgdi_log_print_records(log);

	/* might not exist if deleted due to wraparound */
	test_dbgdi_log_get_record(log, 1, SIZE_1, 0x11);
	test_dbgdi_log_get_record(log, 5, SIZE_5, 0x55);

	test_dbgdi_log_get_record(log, 5, SIZE_5, 0x44); /* wrong content */
	test_dbgdi_log_get_record(log, 6, SIZE_6 - 4, 0x66); /* too small */
	test_dbgdi_log_get_record(log, 7, MAX_REC_SIZE, 0x77); /* doesn't exist */

	dbgdi_log_reset(log);
	dbgdi_log_add_rec(log, db_rec, 1, 256);
	dbgdi_log_add_rec(log, db_rec, 2, 127);
	dbgdi_log_add_rec(log, db_rec, 3, 255);
	dbgdi_log_add_rec(log, db_rec, 4, 63);
	dbgdi_log_add_rec(log, db_rec, 5, 120);
	dbgdi_log_add_rec(log, db_rec, 6, 11);
	dbgdi_log_add_rec(log, db_rec, 7, 22);
	dbgdi_log_add_rec(log, db_rec, 8, 33);
	dbgdi_log_add_rec(log, db_rec, 9, 44);
	dbgdi_log_add_rec(log, db_rec, 10, 101);
	dbgdi_log_add_rec(log, db_rec, 11, 202);
	dbgdi_log_add_rec(log, db_rec, 12, 4);
	dbgdi_log_add_rec(log, db_rec, 13, 2);
	dbgdi_log_add_rec(log, db_rec, 14, log_size + 1);
	dbgdi_log_print_records(log);

	dbgdi_log_add_rec(log, db_rec, 13, log_size);
	dbgdi_log_print_records(log);

	free(buf);
}

int main(int argc, char **argv)
{
	int log_buf_size;

	if (argc > 1 && atoi(argv[1]) > 0)
		log_buf_size = atoi(argv[1]);
	else
		log_buf_size = 512;

	test_dbgdi_log(log_buf_size);

	test_dbgdi_log_block();

	return 0;
}
