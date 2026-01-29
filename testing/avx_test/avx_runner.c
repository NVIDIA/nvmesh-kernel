/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: Apache-2.0
*/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sched.h>
#include <errno.h>
#include <error.h>
#include <pthread.h>
#include <string.h>

#include "avx_runner.h"

#define RUNTIME_DEFAULT	10
#define RUNTIME_MAX	360000

#define AVX512_K_MASK_VALID 0x00000000ffffffffUL
#define AVX2_K_MASK_VALID   0x000000000000ffffUL

struct cmd_options {
	int faults;
	int verbose;
	int runtime_sec;
	int test_avx2;
	int test_avx512;
};

enum avx_type {
	avx2_tp,
	avx512_tp
};

struct thread_info {
	enum avx_type avx_type;
	int tid;
	pthread_t t;
};

static struct cmd_options cmd_options = { 0, };

static pthread_mutex_t guard;
static int test_active = 0;

static unsigned long random_ulong(void)
{
	unsigned long i, val;
	unsigned char *cptr = (unsigned char*)&val;
	for (i = 0; i < __SIZEOF_LONG__; i++) {
		do {
			cptr[i] = (unsigned char)lrand48();
		} while (!cptr[i]);
	}
	return val;
}

static void init_avx_reg(unsigned long *ptr, size_t qwords_num)
{
	int i;
	for (i = 0; i < qwords_num; ptr[i++] = random_ulong());
}


static void *run(void *context)
{
	int i, cpu, verdict = 1;
	cpu_set_t cpuset;
	struct thread_info *runner = context;
	avx2_reg_t   *avx2_reg;
	avx512_reg_t *avx512_reg;
	void *reg;
	unsigned long k_mask[AVX_REGS_NUM];
	char avx_reg_name[4];
	unsigned int __fault_cnt = 0;
	unsigned long valid_k_mask =
			runner->avx_type == avx512_tp ? AVX512_K_MASK_VALID : AVX2_K_MASK_VALID;


	strcpy(avx_reg_name, runner->avx_type == avx512_tp ? "ZMM" : "YMM");

	CPU_ZERO(&cpuset);

	int rv = pthread_getaffinity_np(runner->t, sizeof(cpuset), &cpuset);
	if (rv) {
		int err = errno;
		error(0, err, "tid=%d: failed to get affinity\n", runner->tid);
		pthread_cancel(runner->t);
		goto out;
	}

	if (cmd_options.verbose) {
		for (cpu = 0; cpu < CPU_SETSIZE; cpu++) {
			if (CPU_ISSET(cpu, &cpuset)) {
				printf("tid=%d is running on cpu=%d\n", runner->tid, cpu);
			}
		}
	}

	if (runner->avx_type == avx512_tp) {
		avx512_reg = calloc(AVX_REGS_NUM, sizeof(avx512_reg_t));
		if (!avx512_reg) {
			error(-1, errno, "failed to allocate AVX512 registers");
		}
		reg = avx512_reg;
	} else {
		avx2_reg = calloc(AVX_REGS_NUM, sizeof(avx2_reg_t));
		if (!avx2_reg) {
			error(-1, errno, "failed to allocate AVX2 registers");
		}
		reg = avx2_reg;
	}

	for (i = 0; i < AVX_REGS_NUM; i++) {
		if (runner->avx_type == avx512_tp)
			init_avx_reg(avx512_reg[i], AVX512_QWORD_SZ);
		else
			init_avx_reg(avx2_reg[i], AVX2_QWORD_SZ);
	}

	if (runner->avx_type == avx512_tp) {
		avx512_load_registers(avx512_reg);
	} else {
		avx2_load_registers(avx2_reg);
	}

	do {
		if (runner->avx_type == avx512_tp) {
			avx512_compare_registers(avx512_reg, k_mask);
		} else {
			avx2_compare_registers(avx2_reg, k_mask);
		}

		for (i = 0; i < AVX_REGS_NUM; i++) {
			if (k_mask[i] != valid_k_mask) {
				pthread_mutex_lock(&guard);
				printf("tid%d %s%d invalid k mask %016lx\n",
						runner->tid, avx_reg_name, i, k_mask[i]);
				pthread_mutex_unlock(&guard);
				verdict = 0;
			}
		}

		if (!verdict) {
			pthread_mutex_lock(&guard);
			printf("%s tid%d test failed\n",
					runner->avx_type == avx512_tp ? "AVX512" : "AVX2",
					runner->tid);
			pthread_mutex_unlock(&guard);
			exit(-1);
		}
		usleep(100000);

		if (cmd_options.faults) {
			if (++__fault_cnt == 7) {
				if (runner->avx_type == avx512_tp) avx512_reg[3][3] = ~avx512_reg[3][3];
				else avx2_reg[3][3] = ~avx2_reg[3][3];
			}
		}
	} while (test_active && verdict);

out:
	if (!verdict) test_active = 0;
	return (void*)(long)verdict;
}


static void help(const char *name)
{
	error(-1, 0, "Usage: %s -h | -v | -t runtime| -f\n", name);
}

static const char getopt_short[] = "fht:vyz";

static void parse_commands(int argc, char **argv)
{
	int opt;
	while ((opt = getopt(argc, argv, getopt_short)) != -1) {
		switch (opt) {
		case 't':
			cmd_options.runtime_sec = atoi(optarg);
			break;
		case 'v':
			cmd_options.verbose = 1;
			break;
		case 'f':
			cmd_options.faults = 1;
			break;
		case 'y':
			cmd_options.test_avx2 = 1;
			break;
		case 'z':
			cmd_options.test_avx512 = 1;
			break;
		case 'h':
		default:
			help(argv[0]);
		}
	}

	if (!cmd_options.test_avx2 && !cmd_options.test_avx512) {
		error(-1, 0, "no AVX test to run");
	}

	if (!cmd_options.runtime_sec) {
		printf("set runtime to default - %d sec\n", RUNTIME_DEFAULT);
		cmd_options.runtime_sec = RUNTIME_DEFAULT;
	} else if (cmd_options.runtime_sec > RUNTIME_MAX) {
		printf("runtime too big trim to %d sec\n", RUNTIME_MAX);
		cmd_options.runtime_sec = RUNTIME_MAX;
	} else {
		printf("runtime set to %d sec\n", cmd_options.runtime_sec);
	}
}

#define AVX2_FLAG   5
#define AVX512_FLAG 16
void check_cpu(int *avx2, int *avx512)
{
	unsigned int ebx;

	__asm__ __volatile__ (
		"cpuid\n\t"
		"movl %%ebx, %0\n\t"
		: "=m" (ebx) : "a" (7), "c" (0) 
	);

	*avx2   = ebx & (1 << AVX2_FLAG);
	*avx512 = ebx & (1 << AVX512_FLAG);
}

static struct thread_info *
avx_test_init(long nr_cores, enum avx_type avx_type)
{
	struct thread_info *runners = calloc(nr_cores, sizeof(struct thread_info));
	if (!runners) {
		int err = errno;
		error(-1, err, "failed to allocate context for %ld runners\n", nr_cores);
	}

	for (int i = 0; i < nr_cores; i++) {
		cpu_set_t cpuset;

		runners[i].tid = i;
		runners[i].avx_type = avx_type;
		int rv = pthread_create(&runners[i].t, NULL, run, runners + i);
		if (rv) {
			int err = errno;
			error(-1, err, "failed to create thread\n");
		}

		CPU_ZERO(&cpuset);
		CPU_SET(i, &cpuset);
		rv = pthread_setaffinity_np(runners[i].t, sizeof(cpuset), &cpuset);
		if (rv) {
			int err = errno;
			error(-1, err, "failed to set affinity\n");
		}
	}

	return runners;
}

int main(int argc, char **argv)
{
	int i, verdict;
	int avx2_present, avx512_present;
	long nr_cores = sysconf( _SC_NPROCESSORS_ONLN );
	struct thread_info *avx512_runners, *avx2_runners;

	parse_commands(argc, argv);

	check_cpu(&avx2_present, &avx512_present);
	printf("AVX2   %s\n", avx2_present   ? "supported" : "not supportred");
	printf("AVX512 %s\n", avx512_present ? "supported" : "not supportred");
	printf("nr_cores=%ld\n", nr_cores);

	test_active = 1;
	pthread_mutex_init(&guard, NULL);

	if (cmd_options.test_avx512 && avx512_present) {
		avx512_runners = avx_test_init(nr_cores, avx512_tp);
	}

	if (cmd_options.test_avx2 && avx2_present) {
		avx2_runners = avx_test_init(nr_cores, avx2_tp);
	}

	sleep(cmd_options.runtime_sec);
	test_active = 0;

	if (cmd_options.test_avx512) {
		for (i = 0; i < nr_cores; i++) {
			pthread_join(avx512_runners[i].t, NULL);
		}
	}

	if (cmd_options.test_avx2) {
		for (i = 0; i < nr_cores; i++) {
			pthread_join(avx2_runners[i].t, NULL);
		}
	}

out:
	return 0;
}
