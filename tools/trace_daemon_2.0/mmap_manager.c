/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include "mmap_manager.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define __MODULE_HDR "mmap-mgr"

#include "trace_daemon_common.h"

/* @TODO: make this all configurable */
#define MMAP_CHUNK_SIZE_BYTES (1 << 20)
#define MAX_MMAP_CHUNKS 4096UL

struct mmap_manager
{
	pthread_mutex_t guard;
	int fd;
	void* mapping[MAX_MMAP_CHUNKS];
	unsigned long page_sz;
	unsigned long mmap_chunk_sz;
	unsigned long mmap_chunk_mask;
};

mmap_manager_t* init_mmap_manager(const char* proc_name)
{
	mmap_manager_t* mgr = calloc(sizeof(*mgr), 1);
	if(mgr)
	{
		pthread_mutex_init(&mgr->guard, NULL);
		if((mgr->fd = open(proc_name, O_RDONLY)) < 0)
		{
			_error("Opening %s for read", proc_name);
			goto err;
		}
		mgr->page_sz = sysconf(_SC_PAGESIZE);
		mgr->mmap_chunk_sz = MMAP_CHUNK_SIZE_BYTES / mgr->page_sz;
		assert(mgr->mmap_chunk_sz > 0);
		mgr->mmap_chunk_mask = (mgr->mmap_chunk_sz - 1);
	}
	return mgr;
err:
	destroy_mmap_manager(mgr);
	return NULL;
}

void destroy_mmap_manager(mmap_manager_t* self)
{
	if(self)
	{
		pthread_mutex_destroy(&self->guard);
		if(self->fd > 0)
		{
			int i = 0;
			for(i = 0; i < MAX_MMAP_CHUNKS; ++i)
			{
				if(self->mapping[i] && self->mapping[i] != MAP_FAILED)
					munmap(self->mapping[i], MMAP_CHUNK_SIZE_BYTES);
			}
			close(self->fd);
		}

		free(self);
	}
}

void* get_buf_addr(mmap_manager_t* self, unsigned long buf_idx)
{
	if(buf_idx >= MAX_MMAP_CHUNKS * self->mmap_chunk_sz)
		_suicide("Ivalid input: buf_idx = %lu", buf_idx);
	{ /* Else */
		unsigned long cell = (buf_idx / self->mmap_chunk_sz);
		if(!self->mapping[cell] && self->mapping[cell] != MAP_FAILED)
		{
			pthread_mutex_lock(&self->guard);
			/* Double check - first was a quick check before lock, then it could change. */
			if(!self->mapping[cell] && self->mapping[cell] != MAP_FAILED)
			{
				_info("mmap initiated on cell %lu {0x%lx,0x%lx} by buf_idx=0x%lx", cell,
					cell * MMAP_CHUNK_SIZE_BYTES, (cell + 1) * MMAP_CHUNK_SIZE_BYTES - 1, buf_idx);
				self->mapping[cell] = mmap(NULL, MMAP_CHUNK_SIZE_BYTES, PROT_READ, MAP_SHARED,
					self->fd, cell * MMAP_CHUNK_SIZE_BYTES);
				if(self->mapping[cell] == MAP_FAILED) {
					if (errno != ENOENT) {
						_suicide("mmap failed");
					} else {
						_error("mmap got ENOENT");
						pthread_mutex_unlock(&self->guard);
						return NULL;
					}
				}
			}
			pthread_mutex_unlock(&self->guard);
		}

		return self->mapping[cell] + ((buf_idx & self->mmap_chunk_mask) * self->page_sz);
	}
}

int get_control_proc(mmap_manager_t* self)
{
	return self->fd;
}

unsigned long get_trace_buf_size(mmap_manager_t* self)
{
	return self->page_sz;
}
