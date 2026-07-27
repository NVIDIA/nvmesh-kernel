#ifndef NVMEIBC_CAPABILITIES_H
#define NVMEIBC_CAPABILITIES_H

#include <linux/types.h>
const char* nvmeibc_get_capabilities(void);
ssize_t nvmeibc_get_compile_flags(char *buffer, size_t len);

#endif //NVMEIBC_CAPABILITIES_H
