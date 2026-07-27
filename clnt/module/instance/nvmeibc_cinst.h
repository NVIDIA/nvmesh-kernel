#ifndef NVMEIBC_CINST_H
#define NVMEIBC_CINST_H

#include "nvmeibc_cinst_params.h"
/* Mechanism which implements a few client instances within the same module.
   This allow better container support + testing of scale environment with ~100
   clients on each machine */

void nvmeibc_cinst_array_init(   void);
void nvmeibc_cinst_array_destroy(void);
int nvmeibc_cinst_array_debug_print(void *ctx, char *buffer, size_t len);

// Iterator to traverse the array of instances. Note: This can be done only from main-wq of module to prevent race conditions with add-remove instance
const struct nvmeibc_cinst_params* nvmeibc_cinst_array_get_itr_next(const struct nvmeibc_cinst_params*);
#define for_each_cinst(i) for (nvmeibc_assert_on_module_wq(), i = nvmeibc_cinst_array_get_itr_next(NULL); i != NULL; i = nvmeibc_cinst_array_get_itr_next(i))
//int nvmeibc_cinst_array_get_num_instances(void);

const struct nvmeibc_cinst_params *nvmeibc_cinst_params_get_default(void);	// Dafault values for client instace we are going to add

void *nvmeibc_cinst_array_add(const struct nvmeibc_cinst_params *params);
void  nvmeibc_cinst_array_del(const struct nvmeibc_cinst_params *params);

#endif

