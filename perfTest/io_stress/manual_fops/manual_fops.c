/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <pthread.h>

#define MAX_ELEM_COUNT			256		// the maximum number of elements in IOV
#define DEFAULT_IO_DELAY		100		// msec
#define DEFAULT_IOV_ELEM_COUNT	1
#define DEFAULT_IOV_ELEM_SIZE	4096	// Bytes

pthread_t	io_thread;
struct {
	char *buf;					// Buffer of the io
	int	elem_size;
	int	elem_count;
	struct iovec arr[MAX_ELEM_COUNT];	// the iov to be used for IO
} __iov = {NULL, DEFAULT_IOV_ELEM_SIZE, DEFAULT_IOV_ELEM_COUNT};

struct t_io {					// Stats about executed io
	int msec_delay;
	int n_threads;			// Number of io threads
	volatile int is_running;
	int	success_count;		// count successfull IO's
	int	fail_count;			// count failed IO's
} io = {DEFAULT_IO_DELAY, 1, 0, 0, 0};

static void __print_io_stats(const struct t_io *io) {
	printf("io_success_count = %d, io_fail_count = %d\n", io->success_count, io->fail_count);
	fflush(stdout);		// for test scripts waiting for output
}

struct t_file {
	const char *name;
	int fd;
	enum storage_type { stg_type_block, stg_type_file} type;
} __file = {NULL, -1};

void __open(struct t_file *f) {
	int flags = O_CREAT | O_RDWR;
	if (f->type == stg_type_block)
		flags |= O_DIRECT;
	fprintf(stderr, "Openning file %s\n", f->name);
	f->fd = open(f->name, flags);
	if (f->fd < 0)
		printf("Failed to open file. errno=%d\n", errno);
}

void __close(struct t_file *f) {
	fprintf(stderr, "Closing file %s\n", f->name);
	close(f->fd);
	f->fd = -1;
}

void *__write_io(void *_p) {
	fprintf(stderr, "Thread %ld starts IO to file %s\n", pthread_self(), __file.name);
	while (io.is_running) {
		int rv = pwritev(__file.fd, __iov.arr, __iov.elem_count, 0);
		if (rv == -1) {
			__sync_add_and_fetch(&io.fail_count, 1);			// count failed IO's
			printf("Failed to write: errno=%d\n", errno);
		} else {
			__sync_add_and_fetch(&io.success_count, 1);			// count success IO's
		}
		usleep(io.msec_delay * 1000);
	}
	fprintf(stderr, "Thread %ld end IO to file %s\n", pthread_self(), __file.name);
	return NULL;
}

// http://www.thegeekstuff.com/2012/03/catch-signals-sample-c-code
void sig_handler(int signo) {
	switch (signo) {
		case SIGUSR1:
			if (__file.fd == -1) {
				__open(&__file);
			} else {
				__close(&__file);
				__print_io_stats(&io);
			}
			break;

		case SIGUSR2: {
			int	rv, active_threads = 0, i;
			if (__file.fd == -1) {
				fprintf(stderr, "Error: Cannot run IO to closed file %s. errno=%d\n", __file.name, errno);
			} else {
				if (!io.is_running) {
					io.is_running = 1;
					for (i=0; i < io.n_threads; i++) {
						rv = pthread_create(&io_thread, NULL, __write_io, NULL);
						if (rv != 0) {
							printf("Failed to create IO thread # %d. rc=%d\n", i, rv);
						} else {
							active_threads++;
						}
					}
					if (active_threads > 0) {
						printf("Created %d IO threads\n", active_threads);
					} else {
						io.is_running = 0;
					}
				} else {
					fprintf(stderr, "Stopping all IO to file %s\n", __file.name);
					io.is_running = 0;
					__print_io_stats(&io);
				}
			}
			break;
		}
		default:
			fprintf(stderr, "received unsuported signal %d\n", signo);
	}
}

/*****************************************************************************/
void show_help_params(void) {
	printf("This app must be run as sudo\n");
	printf("manual_fops:  <options>\n");
	printf("\t[-f <file_name>] or [-b <block device>]\n");
	printf("\t[-t <number of threads, def=1>]\n");
	printf("\t[-d <delay in msec, def=>%d]\n", DEFAULT_IO_DELAY);
	printf("\t[-s <iovector element size (Bytes), def=%d]\n", DEFAULT_IOV_ELEM_SIZE);
	printf("\t[-e <iovector element count, def=%d]\n", DEFAULT_IOV_ELEM_COUNT);
	printf("Examples:\n");
	printf("\t\t-b /dev/nvmesh/sm1 -t 5                          : 5 threads, IO's go to block device 'sm1'\n");
	printf("\t\t-f /mnt/a.txt -d 10 -t 16                        : 16 threads, 10 msec between IO's, IO's go to a file\n");
	printf("\t\t-d 1 -t 33 -e 11 -b /dev/nvmesh/r2               : 33 threads, 1 msec between IO's, 11 elements in IOV, IO's go to block device 'r2'\n");
	printf("\t\t-d 10 -t 33 -e 16 -s 4096 -b /dev/nvmesh/r2      : 33 threads, 10 msec between IO's, 16 elements in IOV, 4096B per IOV element, IO's go to block device 'r2'\n");
}

void show_help_execution(int on_error) {
	const long int my_thread_id = on_error ? 0 : getpid();
	printf("Use the following cmds:\n");
	printf("sudo kill -%d %ld\t\t\t- open  the file\n"			, SIGUSR1, my_thread_id);
	printf("sudo kill -%d %ld\t\t\t- start IO to file\n"		, SIGUSR2, my_thread_id);
	printf("sudo kill -%d %ld\t\t\t- stop  IO to file\n"		, SIGUSR2, my_thread_id);
	printf("sudo kill -%d %ld\t\t\t- close the file\n"			, SIGUSR1, my_thread_id);
	printf("sudo kill -%d %ld\t\t\t- kill me\n"					, SIGTERM, my_thread_id);
}

static int parse_args(int argc, char *argv[]) {
	int c, rv = 0;
	int	is_file_set = 0;

	if ((argc <= 1) || (geteuid() != 0)) {
		rv = -5; goto __terminate_app;
	}

	opterr = 0;
	while ((c = getopt(argc, argv, "f:b:t:d:s:e:h")) != -1)
	switch (c) {
	case 'f':
		__file.type = stg_type_file;
		__file.name = optarg;
		is_file_set = 1;
		break;
	case 'b':
		__file.type = stg_type_block;
		__file.name = optarg;
		is_file_set = 1;
		break;
	case 't':
		io.n_threads = atoi(optarg);
		if ((io.n_threads < 1) || (io.n_threads > 100)) {
			printf("Too many threads : %d\n", io.n_threads);
			exit(-EINVAL);
		}
		break;
	case 'd':
		io.msec_delay = atoi(optarg);
		break;
	case 's':
		__iov.elem_size = atoi(optarg);
		if ((__iov.elem_size != 512) && (__iov.elem_size != 4096)) {
			printf("IOV element size must be 512 or 4096\n");
			exit(-EINVAL);
		}
		break;
	case 'e':
		#define MIN_ELEM_COUNT			1
		__iov.elem_count = atoi(optarg);
		if ((__iov.elem_count < MIN_ELEM_COUNT) || (__iov.elem_count > MAX_ELEM_COUNT)) {
			printf("Invalid IOV element count. valid range is [%d..%d]\n", MIN_ELEM_COUNT, MAX_ELEM_COUNT);
			exit(-EINVAL);
		}
		break;
	case 'h':
		rv = 0;
		goto __terminate_app;
	case '?':
		printf("Unknown option -%c\n",optopt);
		exit(-EINVAL);
	default:
		exit(-EINVAL);
	}
	if (is_file_set == 0) {
		printf("missing file (block/file)\n");
		exit(-EINVAL);
	}

	if (1) { // Dump config, Todo: Make json
		const long int my_thread_id = pthread_self();
		const int      my_group_id  = getpid();
		printf("My thread id: %d/%ld, file: %s\n", my_group_id, my_thread_id, __file.name);
		printf("filename:\"%s\", storage type:\"%s\"\n", __file.name, ((__file.type == stg_type_block) ? "block" : "file"));
		printf("thread_count: %d, delay between IOs: %d[msec], IO size of %d[blocks], block=%d[bytes]\n", io.n_threads, io.msec_delay, __iov.elem_count, __iov.elem_size);
		fflush(stdout);	// flush stdout to ensure that a script that parses the output gets it. it happened to be buffered !!!
	}
	return 0;

__terminate_app:
	show_help_params();
	show_help_execution(1);
	exit(rv);
}

#define VERSION "1.3" /* Please make sure to update this in case breaking changes are made */
int main(int argc, char *argv[]) {
	int	i, rv;

	fprintf(stderr, "Version: " VERSION "\n");
	parse_args(argc, argv);

	rv = posix_memalign((void**)&__iov.buf, 4096/*page align*/, __iov.elem_size * __iov.elem_count);
	if (rv != 0) {
		printf("failed to allocated buffer: error=%d\n", rv);
		exit(rv);
	}
	if (1) {					// Set unique pattern inside write blocks
		int	*ptr;
		for (i = 0, ptr = (int *)__iov.buf; i < __iov.elem_size * __iov.elem_count; i += sizeof(*ptr), ptr++)
			*ptr = i;
	}
	if (1) {					// Set iov to point to buff
		for (i = 0; i < __iov.elem_count; i++) {
			__iov.arr[i].iov_base = &__iov.buf[i * __iov.elem_size];
			__iov.arr[i].iov_len =  __iov.elem_size;
		}
	}
	#define register_signal(si) if (signal(si, sig_handler) == SIG_ERR) printf("\ncan't catch %d\n", si);
	register_signal(SIGUSR1);
	register_signal(SIGUSR2);
	show_help_execution(0);
	while (1)
		sleep(1);
	return 0;
}

