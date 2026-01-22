/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "common/kr_incs.h"
#include "common/compat/kr_incs_compiler_types.h"

#if defined(__KERNEL__) || defined(BLKDEV_SIMULATOR)
	#define DBGDI_LOG_ERROR(fmt, ...)
	#define DBGDI_LOG_DEBUG(fmt, ...)
#elif defined(UM_APP) // NVMesh UM
	#define DBGDI_LOG_ERROR(fmt, ...) __DE(fmt, ## __VA_ARGS__)
	#define DBGDI_LOG_DEBUG(fmt, ...) __DD(fmt, ## __VA_ARGS__)
	#define FMT_PTR "log @PTR"
	#define FMT_INT "@INT"
	#define LOG_ADDR(log)  (log)
#else // command line utilities, like cmp_blocks / infra.so / di_parser
	#define DBGDI_LOG_ERROR(fmt, ...) printf("%s E " fmt "\n", __func__, ## __VA_ARGS__)
	#define DBGDI_LOG_DEBUG(fmt, ...) printf("%s D " fmt "\n", __func__, ## __VA_ARGS__)
	#define FMT_PTR "%c"
	#define FMT_INT "%d"
	#define LOG_ADDR(log) ' '		/* log address is not important, breaks repeatability of unitests */
#endif

#include "nvmeibc_block_dp_dbgdi_log.h"

#define DB_LOG_MAGIC 0x12345678

void dbgdi_log_reset(struct dbgdi_log *log)
{
	log->header.head = 0;
	log->header.tail = 0;
	log->header.full = 0;
	log->header.over = 0;
	log->header.reserved = 0;
	log->header.last_rec_type = 0;
}

void dbgdi_log_init(struct dbgdi_log *log, int size)
{

	log->header.magic = DB_LOG_MAGIC;
	log->header.size = size;
	dbgdi_log_reset(log);
}

bool dbgdi_log_initialized(const struct dbgdi_log *log)
{
	return (log->header.magic == DB_LOG_MAGIC);
}

static bool dbgdi_log_full(const struct dbgdi_log *log)
{
	return (log->header.full == 1);
}

static bool dbgdi_log_empty(const struct dbgdi_log *log)
{
	return (log->header.head == log->header.tail && !log->header.full);
}

int dbgdi_log_free_space(const struct dbgdi_log *log)
{
	u32 free_space;

	if (dbgdi_log_empty(log))
		free_space = log->header.size;
	else if (dbgdi_log_full(log))
		free_space = 0;
	else {
		if (log->header.tail >= log->header.head)
			free_space = log->header.tail - log->header.head;
		else
			free_space = log->header.size + log->header.tail - log->header.head;
	}

	return free_space;
}

int dbgdi_log_occupancy(const struct dbgdi_log *log)
{
	return (log->header.size - dbgdi_log_free_space(log));
}

static void __attr_no_alignment_sanity __dbgdi_log_get_entry(const struct dbgdi_log *log, int loc, struct dbgdi_log_entry *entry)
{
	const void *p = &log->buf[loc];
	u32 s1, s2, entry_size;
	void *e;

	entry_size = sizeof(struct dbgdi_log_entry);

	if(p + entry_size <= (void *)log->buf + log->header.size)
		*entry = *db_entry(p);
	else { // wraparound
		s1 = (void *)(log->buf + log->header.size) - p;
		s2 = entry_size - s1;

		e = entry;
		memcpy(e, p, s1);
		memcpy(e + s1, &log->buf[0], s2);
	}
}

static void __dbgdi_log_consume(struct dbgdi_log *log, int req_size)
{
	int free_space, needed_space, freed_space = 0;
	struct dbgdi_log_entry tail_ent;

	free_space = dbgdi_log_free_space(log);

	if (free_space >= req_size) {
		return;
	}

	log->header.over = 1; /* mark we over-wrote some records */

	needed_space = req_size - free_space;
	/* clear records from the tail to have room for a record with this size */
	do {
		__dbgdi_log_get_entry(log, log->header.tail, &tail_ent);
		freed_space += tail_ent.size;
		log->header.tail = (log->header.tail + tail_ent.size) % log->header.size;
	} while (freed_space < needed_space);
}

static void __dbgdi_log_update(struct dbgdi_log *log, int req_size)
{
	__dbgdi_log_consume(log, req_size);

	log->header.head = (log->header.head + req_size) % log->header.size;

	log->header.full = (log->header.head == log->header.tail);
}

static void *__dbgdi_log_produce(struct dbgdi_log *log, int req_size)
{
	void *produce;

	produce = &log->buf[log->header.head];
	__dbgdi_log_update(log, req_size);

	return produce;
}

static void __dbgdi_log_add_rec(struct dbgdi_log *log, struct dbgdi_log_entry *e)
{
	u32 s1, s2, rec_size = e->size;
	void *p;

	p = __dbgdi_log_produce(log, rec_size);

	/* now copy record to the log buffer */

	/* easier case, no wraparound */
	if(p + rec_size <= (void *)log->buf + log->header.size) {
		memcpy(p, e, e->size);
		return;
	}

	/*  wraparound.. */
	s1 = (void *)(log->buf + log->header.size) - p;
	s2 = rec_size - s1;
	memcpy(p, e, s1);
	memcpy(&log->buf[0], (void *)e + s1, s2);
}

int dbgdi_log_get_rec(const struct dbgdi_log *log, int loc, void *buf, int buf_size)
{
	const void *p = &log->buf[loc];
	struct dbgdi_log_entry entry;
	u16 s1, s2;

	if (!dbgdi_log_initialized(log)) {
		DBGDI_LOG_ERROR("log is not initialized - can't get record");
		return -1;
	}

	__dbgdi_log_get_entry(log, loc, &entry);

	if (entry.size > buf_size) {
		DBGDI_LOG_ERROR("buffer too small (" FMT_INT ") to contain record (" FMT_INT " " FMT_INT ")", buf_size, entry.type, entry.size);
		return -1;
	}

	if(p + entry.size <= (void *)log->buf + log->header.size) {
		memcpy(buf, p, entry.size);
		return 0;
	}

	/*  wraparound.. */
	s1 = (void *)(log->buf + log->header.size) - p;
	s2 = entry.size - s1;
	memcpy(buf, p, s1);
	memcpy(buf + s1, &log->buf[0], s2);

	return 0;
}

int dbgdi_log_add_rec(struct dbgdi_log *log, void *buf, u16 type, u16 size)
{
	struct dbgdi_log_entry *e = db_entry(buf);

	if (!dbgdi_log_initialized(log)) {
		DBGDI_LOG_ERROR(FMT_PTR " is not initialized - can't add rec of type " FMT_INT " size " FMT_INT, LOG_ADDR(log), type, size);
		return -1;
	}

	if (size < sizeof(*e)) {
		DBGDI_LOG_ERROR("can't add record smaller than the log entry size (" FMT_INT "), ignoring", (int)sizeof(*e));
		return -1;
	}

	if (size > log->header.size) {
		DBGDI_LOG_ERROR("can't add record larger than the log buffer size (" FMT_INT "), ignoring", log->header.size);
		return -1;
	}

	e->type = type;
	e->size = size;
	DBGDI_LOG_DEBUG(FMT_PTR " type " FMT_INT " size " FMT_INT " added at loc " FMT_INT, LOG_ADDR(log), type, size, log->header.head);
	__dbgdi_log_add_rec(log, e);
	log->header.last_rec_type = type;
	return 0;
}

int dbgdi_log_iter_init(const struct dbgdi_log *log)
{
	if(!dbgdi_log_initialized(log)) {
		DBGDI_LOG_ERROR("log is not initialized - can't iterate");
		return -1;
	}

	if (dbgdi_log_empty(log)) {
		DBGDI_LOG_ERROR("log is empty - can't iterate");
		return -1;
	}

	return log->header.tail;
}

int dbgdi_log_iter_next(const struct dbgdi_log *log, int iter)
{
	int loc = iter;
	struct dbgdi_log_entry entry;

	__dbgdi_log_get_entry(log, loc, &entry);

	loc = (loc + entry.size) % log->header.size;

	return loc;
}

int dbgdi_log_get_record_by_type(const struct dbgdi_log *log, int type, void *buf, int buf_size)
{
	int loc = dbgdi_log_iter_init(log);
	struct dbgdi_log_entry entry;

	if (loc == -1) {
		DBGDI_LOG_ERROR("can't init log iterator");
		return -1;
	}

	do {
		__dbgdi_log_get_entry(log, loc, &entry);
		if (entry.type == type) {
			if (entry.size <= buf_size) {
				DBGDI_LOG_DEBUG(FMT_PTR " rec type " FMT_INT " size " FMT_INT " found at loc " FMT_INT, LOG_ADDR(log), entry.type, entry.size, loc);
				dbgdi_log_get_rec(log, loc, buf, entry.size);
				return 0;
			}
			else {
				DBGDI_LOG_ERROR("rec type " FMT_INT " size " FMT_INT " bigger than the provided size " FMT_INT, entry.type, entry.size, buf_size);
				return -1;
			}
		}
		loc = dbgdi_log_iter_next(log, loc);
	} while ((u32)loc != log->header.head);

	DBGDI_LOG_DEBUG("rec type " FMT_INT " was not found", type);
	return -1;
}
