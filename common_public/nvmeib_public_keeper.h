#ifndef _NVMEIB_PUBLIC_KEEPER_H_
#define _NVMEIB_PUBLIC_KEEPER_H_

#include "keeper/nvmeib_keeper_iface.h"

DEFINE_COMMON_REGISTER_KEEPER_FN(nvmeib_register_keeper);
DEFINE_COMMON_UNREGISTER_KEEPER_FN(nvmeib_unregister_keeper);

struct nvmeib_keeper_ops *nvmeib_public_get_keeper(void);
void nvmeib_public_put_keeper(void);

int nvmeib_public_load_keeper(void);
int nvmeib_public_unload_keeper(void);

void nvmeib_public_keeper_init(void);
void nvmeib_public_keeper_fini(void);

#endif /* _NVMEIB_PUBLIC_KEEPER_H_ */
