#include "kr_incs.h"
#include <fcntl.h>
#include "io_pet_traces_controller.h"
#include "common/pet/nvmeib_pet_specification.h"
#include "nvmeibc_memmgr_metrics.h"

struct nvmeibc_io_pet_controller {
	struct nvmeib_pet_base_controller base;
	size_t buffer_size;
	int fd_output;
};

static void __io_pet_controller_flush(struct nvmeib_pet_base_controller const* base, enum nvmeib_pet_severity severity, struct iovec const data)
{
	ssize_t written = 0;
	__auto_type self = (struct nvmeibc_io_pet_controller const*)base;
	if (self->fd_output == -1){
		return;
	}

	BUG_ON(data.iov_len == 0);
	BUG_ON(data.iov_base == NULL);

	if (severity == NVMEIB_PET_SEVERITY_NORMAL){
		return;
	}

	written = write(self->fd_output, data.iov_base, data.iov_len);
	if (written != (ssize_t)data.iov_len){
		perror("Failed to write IO PET to file");
		BUG();
	}
}

NVMEIBC_MEMMGR_METRIC(io_pet_buffers, "component=raid.io.pet.buffers");

struct iovec __io_pet_controller_get_buffer(struct nvmeib_pet_base_controller const* base)
{
	__auto_type self = (struct nvmeibc_io_pet_controller*)(base);
	void* ptr = kmalloc(self->buffer_size, GFP_KERNEL);
	nvmesh_memmgr_metric_on_alloc_update(io_pet_buffers, ptr? ksize(ptr): self->buffer_size, ptr);
	return (struct iovec){.iov_base = ptr, .iov_len = ptr ? self->buffer_size : 0};
}

void __io_pet_controller_put_buffer(struct nvmeib_pet_base_controller const* self, struct iovec data)
{
	(void)self;
	if( data.iov_base){
		nvmesh_memmgr_metric_on_free_update(io_pet_buffers, ksize(data.iov_base));
		kfree(data.iov_base);
	}
}

struct nvmeibc_io_pet_controller io_pet_controller = {
	.base = {
		.flush = __io_pet_controller_flush,
		.get_buffer = __io_pet_controller_get_buffer,
		.put_buffer = __io_pet_controller_put_buffer
	},
	.buffer_size = 4096,
	.fd_output = -1
};

static void __attribute__ ((constructor)) io_pet_controller_init(void)
{
	int const fd = open("io_pet_messages.binlog", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { 
		perror("failed to open 'io_pet_messages.binlog' file"); 
	} else {
		io_pet_controller.fd_output = fd;
	}
}

static void __attribute__ ((destructor)) io_pet_controller_fini(void)
{
	if (io_pet_controller.fd_output != -1)
	{
		close(io_pet_controller.fd_output);
		io_pet_controller.fd_output = -1;
	}
}

struct nvmeib_pet_base_controller* sim_get_io_pet_controller(void)
{
	return &io_pet_controller.base;
}

