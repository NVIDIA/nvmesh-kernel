/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "kr_incs.h"
#include "nvmeibc_io_pet.h"
#include "nvmeibc_memmgr_metrics.h"
#include "common/nvmeib_msgloop.h"
#include "nvmeibc_error_tags.h"


#if defined(BLKDEV_SIMULATOR) && BLKDEV_SIMULATOR

	struct nvmeib_pet_base_controller* nvmeibc_io_pet_controller_create(void)
	{
		extern struct nvmeib_pet_base_controller* sim_get_io_pet_controller(void);
		return sim_get_io_pet_controller();
	}

    void nvmeibc_io_pet_controller_free(struct nvmeib_pet_base_controller* self)
	{(void)self;}

	struct nvmeib_pet_journal nvmeibc_io_pet_journal_make(struct nvmeib_pet_base_controller* controller){
		bool const verbose = true;
		return nvmeib_pet_journal_make(controller, verbose);
	}

#elif defined(__KERNEL__)

	unsigned nvmeibc_io_pet_minimal_severity = NVMEIB_PET_SEVERITY_WARNING;
	module_param(nvmeibc_io_pet_minimal_severity, uint, 0644);
	MODULE_PARM_DESC(nvmeibc_io_pet_minimal_severity, "Defines the minimal severity for IO per-entity trace buffers to be written.");

	unsigned nvmeibc_io_pet_verbose = 0;
	module_param(nvmeibc_io_pet_verbose, uint, 0644);
	MODULE_PARM_DESC(nvmeibc_io_pet_verbose, "A non-zero value will allow IO per-entity trace buffers to provide even more information such as the first 8 bytes and metadata for every block. This may hurt performance and the buffer size should be taken into account.");

	unsigned nvmeibc_io_pet_disable = 0;
	module_param(nvmeibc_io_pet_disable, uint, 0644);
	MODULE_PARM_DESC(nvmeibc_io_pet_disable, "A non-zero value will disable IO per-entity trace functionality.");


	NVMEIBC_MEMMGR_METRIC(io_pet_buffers, "component=raid.io.pet.buffers");

	struct io_pet_controller{
		struct nvmeib_pet_base_controller base;
		struct msgloop_procfs_ent *writer;
		struct {
		    size_t pet_buffer_size; //memory available for pet buffers
			size_t msg_allocation_size; //total memory allocated for msgloop message
		} cfg;
	};

	static struct iovec __io_pet_controller_get_buffer(struct nvmeib_pet_base_controller const* base)
	{
		__auto_type self = (struct io_pet_controller*)(base);
		const bool io_pet_enable = !nvmeibc_io_pet_disable; 
		BUILD_BUG_ON(offsetof(struct io_pet_controller, base) != 0);

		if (io_pet_enable && self->cfg.pet_buffer_size && self->writer){
			struct msgloop_msg* msg = nvmeib_msgloop_alloc_msg(self->cfg.msg_allocation_size, GFP_NOIO);
			nvmesh_memmgr_metric_on_alloc_update(io_pet_buffers, self->cfg.msg_allocation_size, msg);
			if (msg) {
				_ND(__io_pet_controller_get_buffer, "msg=@PTR, msg->data=@PTR", msg, msg->data);
				return (struct iovec){.iov_base=msg->data, .iov_len=self->cfg.pet_buffer_size};
			}
		}
		return (struct iovec){0};
	}

	static void __io_pet_controller_put_buffer(struct nvmeib_pet_base_controller const* base, struct iovec data)
	{
		if (data.iov_base){
			__auto_type self = (struct io_pet_controller*)(base);
			struct msgloop_msg *msg = container_of(data.iov_base, struct msgloop_msg, data);

			BUILD_BUG_ON(offsetof(struct io_pet_controller, base) != 0);
			_ND(__io_pet_controller_put_buffer, "msg=@PTR, msg->data=@PTR", msg, msg->data);
			msgloop_put_msg(msg);
			nvmesh_memmgr_metric_on_free_update(io_pet_buffers, self->cfg.msg_allocation_size);
		}
	}

	static bool __io_pet_controller_should_send(enum nvmeib_pet_severity severity, struct iovec data)
	{
		if(!data.iov_base){
			return false; //memory was not allocated
		}
		if (!data.iov_len){
			return false; //the journal was not in use
		}
		return nvmeib_pet_severity_is_same_or_worse(nvmeibc_io_pet_minimal_severity, severity);
	}

	NVMEIBC_ERROR_TAG(io_pet_send_failures);
	static void __io_pet_controller_flush(struct nvmeib_pet_base_controller const* base, enum nvmeib_pet_severity severity, struct iovec data)
	{
		int sendm_rv = -EINPROGRESS;
		bool const should_send = __io_pet_controller_should_send(severity, data);
		__auto_type self = (struct io_pet_controller*)(base);
		if (should_send) {
			struct msgloop_msg *msg = container_of(data.iov_base, struct msgloop_msg, data);
			BUILD_BUG_ON(offsetof(struct io_pet_controller, base) != 0);

			msg->len = data.iov_len;
			sendm_rv = nvmeib_msgloop_sendm(self->writer, msg);
			nvmesh_error_tag_update(io_pet_send_failures, sendm_rv);
		}
		_ND(__io_pet_controller_flush1, "severity{min=@INT, curr=@INT}, iovec={iov_base=@PTR, iov_len=@SIZE} - send={should?=@BOOL, rv=@INT}",
			nvmeibc_io_pet_minimal_severity, severity, data.iov_base, data.iov_len, should_send, sendm_rv);
	}

	//TODO: in simulator we should understand for a single pass max consumed bytes (bad and good flow)
	//single pass: [start, complete] or [start, resubmit]
	unsigned nvmeibc_io_pet_buffer_size = 1024;
	module_param(nvmeibc_io_pet_buffer_size, uint, 0644);
	MODULE_PARM_DESC(nvmeibc_io_pet_buffer_size, "IO per-entity trace buffer size.");

	struct nvmeib_pet_base_controller* nvmeibc_io_pet_controller_create()
	{
		unsigned const min_buffer_size = 256;
		unsigned const msg_allocation_size = nvmeibc_io_pet_buffer_size;
		bool const is_valid_cfg = min_buffer_size <= msg_allocation_size;
		struct io_pet_controller* self = kzalloc(sizeof(struct io_pet_controller), GFP_KERNEL);
		extern struct msgloop_procfs_ent* nvmeib_trace_get_io_pet_msgloop(void);

		if (!self){
			return NULL;
		}

		BUILD_BUG_ON(min_buffer_size < sizeof(struct msgloop_msg));

		(*self) = (struct io_pet_controller){
			.base = {
				.flush = __io_pet_controller_flush,
				.get_buffer = __io_pet_controller_get_buffer,
				.put_buffer = __io_pet_controller_put_buffer
			},
			.writer = nvmeib_trace_get_io_pet_msgloop(),
			.cfg = {
				.pet_buffer_size = is_valid_cfg ? msg_allocation_size - sizeof(struct msgloop_msg) : 0,
				.msg_allocation_size = msg_allocation_size
			}
		};
		if (self->writer){
			nvmeib_msgloop_set_max(self->writer, 256); //TODO: probably need module param for this
		}

		_NI(nvmeibc_io_pet_controller_create, "writer=@PTR severity{min=@INT}, size={msg=@SIZE, pet=@SIZE}",
			self->writer, nvmeibc_io_pet_minimal_severity, self->cfg.msg_allocation_size, self->cfg.pet_buffer_size);

		return &(self->base);
	}

	void nvmeibc_io_pet_controller_free(struct nvmeib_pet_base_controller* base)
	{
		__auto_type self = (struct io_pet_controller*)(base);
		BUILD_BUG_ON(offsetof(struct io_pet_controller, base) != 0);
		kfree(self);
	}

	struct nvmeib_pet_journal nvmeibc_io_pet_journal_make(struct nvmeib_pet_base_controller* controller){
		bool const verbose = nvmeibc_io_pet_verbose;
		return nvmeib_pet_journal_make(controller, verbose);
	}


#else //probably UM
	struct io_pet_controller{
		struct nvmeib_pet_base_controller base;
	};

	static struct iovec __io_pet_controller_get_buffer(struct nvmeib_pet_base_controller const* base)
	{
		(void)base;
		return (struct iovec){0};
	}

	static void __io_pet_controller_put_buffer(struct nvmeib_pet_base_controller const* self, struct iovec data)
	{
		(void)self;
		(void)data;
	}

	static void __io_pet_controller_flush(struct nvmeib_pet_base_controller const* base, enum nvmeib_pet_severity severity, struct iovec data)
	{
		(void)base;
		(void)severity;
		(void)data;
	}

	struct nvmeib_pet_base_controller* nvmeibc_io_pet_controller_create(void)
	{
		static struct io_pet_controller dummy = {
			.base = {
				.flush = __io_pet_controller_flush,
				.get_buffer = __io_pet_controller_get_buffer,
				.put_buffer = __io_pet_controller_put_buffer
			}
		};
		return &(dummy.base);
	}

	void nvmeibc_io_pet_controller_free(struct nvmeib_pet_base_controller* self)
	{
		(void)self;
	}

	struct nvmeib_pet_journal nvmeibc_io_pet_journal_make(struct nvmeib_pet_base_controller* controller){
		bool const verbose = false;
		return nvmeib_pet_journal_make(controller, verbose);
	}

#endif
