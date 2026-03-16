#pragma once

/* Provides interface to the Kafka simulator and broker backend. Used only by simulators/unit-tests. */
#include "../sandbox_util.h"

void user_rpc_send_to_toma(const char* str);
bool user_rpc_did_toma_reply_to_all_rpcs(void);


