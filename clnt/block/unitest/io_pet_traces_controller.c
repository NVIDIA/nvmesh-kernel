/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include <fcntl.h>
#include "io_pet_traces_controller.h"
#include "nvmeibc_io_pet_admission.h"
#include "common/pet/nvmeib_pet_specification.h"
#include "nvmeibc_memmgr_metrics.h"

struct nvmeibc_io_pet_controller {
	struct nvmeib_pet_base_controller base;
	size_t buffer_size;
	unsigned max_traced_ops_per_cpu;
	struct nvmeibc_io_pet_pcpu_counter active_traced_ops[NR_CPUS];
	bool verbose;
	struct {
		pthread_rwlock_t lock;
		int fd;
	} output;
};

static void __io_pet_controller_flush(struct nvmeib_pet_base_controller const* base, enum nvmeib_pet_severity severity, struct iovec const data)
{
	ssize_t written = 0;
	__auto_type self = (struct nvmeibc_io_pet_controller const*)base;

	BUG_ON(data.iov_len == 0);
	BUG_ON(data.iov_base == NULL);

	if (severity == NVMEIB_PET_SEVERITY_NORMAL){
#ifndef NVMEIBC_ENABLE_PET
		return;
#endif
	}

	pthread_rwlock_rdlock((pthread_rwlock_t*)&self->output.lock);
	if (self->output.fd != -1) {
		written = write(self->output.fd, data.iov_base, data.iov_len);
		if (written != (ssize_t)data.iov_len){
			perror("Failed to write IO PET to file");
			BUG();
		}
	}
	pthread_rwlock_unlock((pthread_rwlock_t*)&self->output.lock);
}


static void __io_pet_controller_close_fd_unsafe(struct nvmeibc_io_pet_controller *controller)
{
	if (controller->output.fd != -1)
	{
		close(controller->output.fd);
		controller->output.fd = -1;
	}
}

NVMEIBC_MEMMGR_METRIC(io_pet_buffers, "component=raid.io.pet.buffers");

struct nvmeib_pet_buffer __io_pet_controller_get_buffer(struct nvmeib_pet_base_controller const *base)
{
	__auto_type self = (struct nvmeibc_io_pet_controller*)(base);
	s16 release_cpu = NVMEIB_PET_NO_RELEASE_CPU;
	void* ptr;

	if (!self->buffer_size ||
	    !nvmeibc_io_pet_try_acquire_slot(self->active_traced_ops, &self->max_traced_ops_per_cpu, &release_cpu)) {
		return (struct nvmeib_pet_buffer){
			.data = { 0 },
			.release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
		};
	}

	// Mirror production __io_pet_controller_get_buffer (clnt/nvmeibc_io_pet.c):
	// runs in atomic context (recv-CQ poll holds the channel spinlock with
	// irqsave; in the simulator, also via the timer-callback path). GFP_NOWAIT
	// prevents sleep; __GFP_NOWARN keeps best-effort failures quiet.
	// Stays as kmalloc/kfree (not sim_*) so the bytes land in AB_CLIENT, which
	// matches the io_pet_buffers audited metric updated below.
	ptr = kmalloc(self->buffer_size, GFP_NOWAIT | __GFP_NOWARN);
	nvmesh_memmgr_metric_on_alloc_update(io_pet_buffers, ptr? ksize(ptr): self->buffer_size, ptr);
	if (ptr) {
		return (struct nvmeib_pet_buffer){
			.data = { .iov_base = ptr, .iov_len = self->buffer_size },
			.release_cpu = release_cpu,
		};
	}

	nvmeibc_io_pet_release_slot(self->active_traced_ops, release_cpu);
	return (struct nvmeib_pet_buffer){
		.data = { 0 },
		.release_cpu = NVMEIB_PET_NO_RELEASE_CPU,
	};
}

void __io_pet_controller_put_buffer(struct nvmeib_pet_base_controller const *base, struct nvmeib_pet_buffer buffer)
{
	__auto_type self = (struct nvmeibc_io_pet_controller *)(base);
	struct iovec data = buffer.data;

	if( data.iov_base){
		nvmesh_memmgr_metric_on_free_update(io_pet_buffers, ksize(data.iov_base));
		kfree(data.iov_base);
	}
	nvmeibc_io_pet_release_slot(self->active_traced_ops, buffer.release_cpu);
}

struct nvmeibc_io_pet_controller io_pet_controller = {
	.base = {
		.flush = __io_pet_controller_flush,
		.get_buffer = __io_pet_controller_get_buffer,
		.put_buffer = __io_pet_controller_put_buffer,
	},
	.buffer_size = 4096,
	.max_traced_ops_per_cpu = 0,
	.verbose = true,
	.output.lock = PTHREAD_RWLOCK_INITIALIZER,
	.output.fd = -1,
};

static void __attribute__ ((constructor)) io_pet_controller_init(void)
{
	// no need to lock here, as it is called before any other thread is created
	int const fd = open("nvmeibc_io_pet.binlog", O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror("failed to open 'nvmeibc_io_pet.binlog' file");
	} else {
		io_pet_controller.output.fd = fd;
	}
}


static void __attribute__ ((destructor)) io_pet_controller_fini(void)
{
	//no need to lock here, as it is called after all threads are finished
	__io_pet_controller_close_fd_unsafe(&io_pet_controller);
	pthread_rwlock_destroy(&io_pet_controller.output.lock);
}

struct nvmeib_pet_base_controller* sim_get_io_pet_controller(void)
{
	return &io_pet_controller.base;
}

void sim_io_pet_controller_set_buffer_size(size_t buffer_size)
{
	io_pet_controller.buffer_size = buffer_size;
}

size_t sim_io_pet_controller_get_buffer_size(void)
{
	return io_pet_controller.buffer_size;
}

void sim_io_pet_controller_set_max_traced_ops_per_cpu(unsigned value)
{
	io_pet_controller.max_traced_ops_per_cpu = value;
}

unsigned sim_io_pet_controller_get_max_traced_ops_per_cpu(void)
{
	return io_pet_controller.max_traced_ops_per_cpu;
}

void sim_io_pet_controller_set_verbose(bool verbose)
{
	io_pet_controller.verbose = verbose;
}

bool sim_io_pet_controller_get_verbose(void)
{
	return io_pet_controller.verbose;
}

struct nvmeib_pet_journal __wrap_nvmeibc_io_pet_journal_make(struct nvmeib_pet_base_controller* controller)
{
	return nvmeib_pet_journal_make(controller, sim_io_pet_controller_get_verbose());
}

void sim_io_pet_controller_rotate(char const* test_id)
{
	int fd = -1;
	char fname[512];
	snprintf(fname, sizeof(fname), "nvmeibc_io_pet.%s.binlog", test_id);
	pthread_rwlock_wrlock(&io_pet_controller.output.lock);
	__io_pet_controller_close_fd_unsafe(&io_pet_controller);
	fd = open(fname, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (fd < 0) {
		fprintf(stderr, "failed to open '%s' file: %s\n", fname, strerror(errno));
	} else {
		io_pet_controller.output.fd = fd;
	}
	pthread_rwlock_unlock(&io_pet_controller.output.lock);
}
