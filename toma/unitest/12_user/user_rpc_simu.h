#pragma once

/* Provides interface to the Kafka simulator and broker backend. Used only by simulators/unit-tests. */
#include "../sandbox_util.h"

void user_rpc_send_to_toma(const char *str);
void user_rpc_send_to_toma_and_set_expected_reply_size(const char *str, unsigned len_bytes);
bool user_rpc_did_toma_reply_to_all_rpcs(void);


