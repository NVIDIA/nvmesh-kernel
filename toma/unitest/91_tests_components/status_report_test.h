/**
 * status_report_test.h - Real-time error reporting and status visibility tests
 *
 * Invoked via: ./nvmeibt_toma status_report_test [selection]
 */

#ifndef STATUS_REPORT_TEST_H
#define STATUS_REPORT_TEST_H

#include "nvmeibt_kafka.h"		// For rd_kafka_resp_err_t

struct nvmeibt_Str;

int status_report_test_main(int argc, char *argv[]);

extern int nvmeibt_rpc_handle_command(char *in, struct nvmeibt_Str *out);

extern void check_if_kafka_init_preserve_state_vars_required(rd_kafka_resp_err_t err);
extern volatile int64_t kafka_last_transient_err_boot_sec;

#endif // #ifndef STATUS_REPORT_TEST_H
