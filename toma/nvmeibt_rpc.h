#ifndef NVMEIBT_RPC_H
#define NVMEIBT_RPC_H

#include "nvmeibt_debug.h"
#include "nvmeibt_common.h"

// Path to toma_rpc CLI tool binary (used by gpt_util to send RPC commands)
#define TOMA_RPC_TOOL_PATH	TOMA_ROOT_DIR "opt/nvmesh/common-repo/tools/toma_rpc"

void nvmeibt_rpc_run(void);
void nvmeibt_rpc_terminate(void);

#endif
