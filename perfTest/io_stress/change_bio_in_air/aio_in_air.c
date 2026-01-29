/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <aio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#define BLOCK_SIZE 4096
#ifndef O_DIRECT
	#define O_DIRECT	00040000
#endif

#define COL_GR "\x1b[32m" // green
#define COL_RED "\x1b[1;31m" // Bold red
#define COL_R "\x1b[0;0m" // Reset color
#define N_IN_AIR_IOS (4)
struct my_io_ctx_t {
	struct aiocb cb;
} my_io[N_IN_AIR_IOS];
char io_type = 'w';	// write
void* io_buf;
#define IO_BUF_SIZE (BLOCK_SIZE*4)

static void aio_completion_handler(sigval_t sigval) {
	typedef unsigned long long u64;
	struct my_io_ctx_t *io = (struct my_io_ctx_t *)sigval.sival_ptr;
	const int err = aio_error(&io->cb);
	(void)sigval;
	if (err == 0) {
		const int rv = aio_return(&io->cb);
		printf("Async io=%c completed: %d[b] vlba=%5u[b], nlba=%5u[b] buf=0x%16llx-%16llx\n", io_type, rv, (unsigned)io->cb.aio_offset, (unsigned)io->cb.aio_nbytes, ((u64*)io_buf)[0], ((u64*)io_buf)[1]);
	} else {
		printf("Async io=%c failed (%d): %s\n", io_type, err, strerror(errno));
	}
	fflush(stdout);
}

static void my_io_ctx_t_init(struct my_io_ctx_t *io, int fd, unsigned vlba, unsigned nlbas) {
	struct aiocb *req = &io->cb;
	req->aio_fildes = fd;
	req->aio_buf = io_buf;
	req->aio_nbytes = (BLOCK_SIZE*nlbas);
	req->aio_offset = (BLOCK_SIZE*vlba);
	req->aio_sigevent.sigev_notify = SIGEV_THREAD;
	req->aio_sigevent.sigev_notify_function = aio_completion_handler;
	req->aio_sigevent.sigev_notify_attributes = NULL;
	req->aio_sigevent.sigev_value.sival_ptr = io;
}

static void change_buffer_while_async_io_is_in_air(unsigned long m_iters) {
	unsigned long i, j;
	char *p = (char *)io_buf;
	for (j = 0; j < m_iters; j++) {
		const char c = (0x21+(j%32)); // ASCII characters
		for (i = 0; i < IO_BUF_SIZE; i++)
			p[i] = c;
	}
	printf("Done changing io=%c buffer in air\n", io_type);
	fflush(stdout);
}

static void __wait_for_async_io_to_complete(struct aiocb *req) {
	while (aio_error(req) == EINPROGRESS) {
		printf("Waiting for aync io=%c to complete...\n", io_type);
		usleep(100000); // Sleep for 100ms
	}
}

int main(int argc, char *argv[]) {
	const char* path = (argc < 2) ? "/dev/nvmesh/ec" : argv[1];
	unsigned long m_iters = 1000;
	int fd = -1, rv = -1;
	if (argc > 2) {
		sscanf(argv[2], "%c", &io_type);
	}
	printf("Version: 0.2, Params: /path/to/bdev <read/write/help>.  Running with: " COL_GR "path=%s, n_iters=%lu io=%c\n" COL_R, path, m_iters, io_type);
	if (1) { // print help
		printf("\t\tReproduces Jira: EC8005-SyncHasNothingToDo, EC7676-Edic error. NVMESH5544-Changes r/w io in air buffers\n");
		printf(COL_GR "\tStep 1: Attach volume (example 'ec'), Run ioctls on it:\n" COL_R);
		printf("\t\tsudo bash -c 'echo -n \"#ec|max_retry_secs=1\" > /proc/nvmeibc/cli/cli';\n");
		printf("\t\tsudo bash -c 'echo -n \"#ec|di_debug_mode=0\" > /proc/nvmeibc/cli/cli';\n");
		printf("\t\tsudo bash -c 'echo -n \"#ec|set_read_edic=1\" > /proc/nvmeibc/cli/cli';\n");
		printf("\t\tsudo bash -c 'echo 1 > /sys/module/nvmeibc/parameters/nvmeibc_copy_bio_buffers';\n");
		printf(COL_GR "\tStep2.1 Remove buffer copy to force in-flight changing write to trigger edic corruption:\n" COL_R);
		printf("\t\tsudo bash -c 'echo 0 > /sys/module/nvmeibc/parameters/nvmeibc_copy_bio_buffers';\n");
		printf("\t\tsudo %s %s write; sudo rm ./b.txt; sudo dd if=%s of=./b.txt bs=4k skip=0 count=4 iflag=direct oflag=direct; xxd ./b.txt | grep '[0-3]00:';\n", argv[0], path, path);
		printf(COL_GR "\tStep2.2: Cleanup-After-Edic-Failure:\n" COL_R);
		printf("\t\tsudo bash -c 'echo -n \"#ec|volume_suspend=0\" > /proc/nvmeibc/cli/cli';\n");
		printf("\t\tsudo dd of=%s if=/dev/zero bs=16k skip=0 count=1000 oflag=direct;\n", path);
		printf("\t\tsudo bash -c 'echo 0 > /sys/module/nvmeibc/parameters/num_warnings';\n");
		printf(COL_GR "\tStep3.1 Test concurent reads to different vlba but same destination buffer\n" COL_R);
		printf("\t\tsudo dd of=%s if=%s bs=16k skip=0 count=16 oflag=direct;\n", path, argv[0]);
		printf("\t\tsudo %s %s read;\n", argv[0], path);
		printf(COL_GR "\tStep3.2: Cleanup-After-Edic-Failure:\n" COL_R);
		printf("\t\tnothing to do: sync should fix the failed read. Volume is io enabled\n");
		if (io_type == 'h')
			return 0;
	}
	posix_memalign(&io_buf, sysconf(_SC_PAGESIZE), IO_BUF_SIZE);
	if (!io_buf) {
		perror("malloc");
		goto _finish;
	}
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC | O_DIRECT, S_IRUSR | S_IWUSR);
	if (fd <= 0) {
		perror("open failed");
		goto _finish;
	}
	memset(io_buf, 'A', IO_BUF_SIZE);

	io_type = ((io_type != 'r') ? 'w' : 'r');
	if (io_type == 'w') {
		struct aiocb *aio_req = &my_io[0].cb;
		my_io_ctx_t_init(&my_io[0], fd, 0, 4);			// Write to vlba [0..4)
		if (aio_write(aio_req) < 0) {
			perror("aio_write");
			goto _finish;
		}
		change_buffer_while_async_io_is_in_air(m_iters);
		__wait_for_async_io_to_complete(aio_req);
	} else {
		for (int i = 0; i < N_IN_AIR_IOS; i++) {
			my_io_ctx_t_init(&my_io[i], fd, i, 4);	// io 'i' reads vlba at offset 'i'
			if (aio_read(&my_io[i].cb) < 0) {
				perror("aio_read");
				goto _finish;
			}
		}
		change_buffer_while_async_io_is_in_air(m_iters);
		{ // Trigger multi-completion event for of all reads
			struct aiocb afsync;
			memset(&afsync, 0, sizeof(struct aiocb));
			afsync.aio_fildes = fd;
			if (aio_fsync(O_SYNC, &afsync) < 0) {
				perror("aio_fsync");
			}
			__wait_for_async_io_to_complete(&afsync);
		}
	}
	fflush(stdout);
	rv = 0;
 _finish:
	if (io_buf) free(io_buf);
	if (fd > 0)	close(fd);
	return rv;
}
