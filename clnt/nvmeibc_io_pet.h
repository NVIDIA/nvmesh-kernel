#ifndef NVMEIBC_IO_PET_H_INCLUDED
#define NVMEIBC_IO_PET_H_INCLUDED

#include "common/pet/nvmeib_pet_specification.h"
#include "compat/kr_incs_compiler_types.h"

#if defined(__KERNEL__)
	#define NVMEIBC_IO_PET_SECTION ".nvmeibc_io_pet_msgs"
#else
	#define NVMEIBC_IO_PET_SECTION  "nvmeibc_io_pet_msgs"
#endif

extern const char __start_nvmeibc_io_pet_msgs[];
extern const char __stop_nvmeibc_io_pet_msgs[];

#define NVMEIBC_IO_PET_MSG(pet_journal, msg, severity,...)   																				\
({																																			\
	u16 __io_pet_msg_written = 0;																											\
	if (nvmeib_pet_journal_is_activated(pet_journal)) {																						\
		static const char NVMESH_USED NVMESH_SECTION(NVMEIBC_IO_PET_SECTION) __io_pet_msg[] = msg;											\
		u16 const __io_pet_msg_offset = (u64)(&__io_pet_msg) - (u64)(&__start_nvmeibc_io_pet_msgs); 										\
		struct nvmeib_pet_journal* __io_pet_journal = (struct nvmeib_pet_journal*)(pet_journal); /*droping const*/							\
		nvmeib_pet_journal_add_msg_verify_format(__io_pet_msg, __VA_ARGS__);															\
		__io_pet_msg_written = nvmeib_pet_journal_add_msg(__io_pet_journal, severity, NVMEIB_PET_MSG(__io_pet_msg_offset, __VA_ARGS__)); 	\
	}																																		\
    __io_pet_msg_written;                                                                                                       			\
})

#define NVMEIBC_IO_PET_MSG_NORM(pet_journal, msg, ...) NVMEIBC_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_NORMAL, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_WARN(pet_journal, msg, ...) NVMEIBC_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_WARNING, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_ERROR(pet_journal, msg, ...) NVMEIBC_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_ERROR, __VA_ARGS__)
#define NVMEIBC_IO_PET_MSG_CRIT(pet_journal, msg, ...) NVMEIBC_IO_PET_MSG(pet_journal, msg, NVMEIB_PET_SEVERITY_CRITICAL, __VA_ARGS__)


struct msgloop_procfs_ent;

__attribute__((nonnull (1)))
struct nvmeib_pet_base_controller* nvmeibc_io_pet_controller_create(struct msgloop_procfs_ent* msgloop);

__attribute__((nonnull (1)))
void nvmeibc_io_pet_controller_free(struct nvmeib_pet_base_controller* self);

#endif //NVMEIBC_IO_PET_H_INCLUDED
