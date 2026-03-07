#ifndef NVMEIBC_ERROR_TAGS_H_INCLUDED
#define NVMEIBC_ERROR_TAGS_H_INCLUDED

#include "common/nvmeib_error_tags.h"

#define NVMEIBC_ERROR_TAG(name) NVMESH_DEFINE_ERROR_TAG(name, "nvmeibc_error_tags")

extern struct nvmesh_error_tag __start_nvmeibc_error_tags[];
extern struct nvmesh_error_tag  __stop_nvmeibc_error_tags[];

ssize_t nvmeibc_error_tags_info(void *_ctx, char *buffer, size_t len);
#endif //NVMEIBC_ERROR_TAGS_H_INCLUDED
