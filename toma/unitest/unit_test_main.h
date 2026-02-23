#pragma once

// API towards epoll sandbox (switch Toma main thread execution with unit test main thread)
void toma_unit_test_thread_create(void);
int  toma_unit_test_thread_switch_to(void);			// Returns <0 on error, >0 if unit-test still run and 0 when all unit-tests finished
void toma_unit_test_thread_destroy(void);
