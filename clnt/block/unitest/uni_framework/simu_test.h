#pragma once

/**
 * Common utility functions shared between multiple scenarios
 */

#include "../bunitest.h"
#include "nvmeib_macro_utils.h"
#include "nvmesh_sim.h"
#include "tests_conf.h"

/**
 * Unitest traces. Used to replace unitest prints with binary tracer backend
 */
#define unitest_trace(name, fmt, ...)                                                                                  \
	do {                                                                                                               \
		NVMEIB_LOG_LONGTERM(fmt, _I, /*Deault*/, name, ##__VA_ARGS__);                                         \
		fprintf(stderr, N_FORMAT_STRING(name) "\n", ##__VA_ARGS__);                                         \
	} while (0)

#define unitest_trace_event(pre, test, event, post, ...)                                                               \
	unitest_trace(NVMEIB_CONCAT2(test, NVMEIB_CONCAT2(_, event)), pre NVMEIB_STRINGIFY1(test) " - " NVMEIB_STRINGIFY1(event) post,     \
				  ##__VA_ARGS__)

#define unitest_trace_checkpoint_1(test, event, fmt, ...) unitest_trace_event("***", test, event, " " fmt, ##__VA_ARGS__)
#define unitest_trace_checkpoint_0(test, event) unitest_trace_checkpoint_1(test, event, "")
#define unitest_trace_checkpoint(test, event, ...) NVMEIB_CONCAT2( unitest_trace_checkpoint_, NVMEIB_HAS_ARGS(__VA_ARGS__) )(test, event, ##__VA_ARGS__)

#define unitest_trace_note(event, fmt, ...) unitest_trace_event("***!", Note, event, " " fmt, ##__VA_ARGS__)

#define __SIMU_RUN_TEST(test_id, test, _sys, ...)                                                                       \
	({                                                                                                                  \
		int __rv_simu_run = 0;                                                                                          \
		int __num_rep = unitest_get_test_num_of_rep(unitest_global_cfg, NVMEIB_STRINGIFY1(test_id));                    \
		if (__num_rep != 0 ) {                                                                                          \
			while(__num_rep--) {                                                                                        \
				unitest_trace_checkpoint(test_id, start, "");                                                           \
				__rv_simu_run |= test((_sys), ##__VA_ARGS__);                                                           \
				if (__builtin_types_compatible_p(typeof(*(_sys)), struct NVMeshSystem))                                 \
					BUG_ON(!NVMeshSystem_is_stable((struct NVMeshSystem *)(_sys)));                                     \
				else if (__builtin_types_compatible_p(typeof(*(_sys)), bunitest_s))                                     \
					BUG_ON(!NVMeshSystem_is_stable(((bunitest_s *)(_sys))->sys));                                       \
				unitest_trace_checkpoint(test_id, result, "=> @OK_FAIL", ((__rv_simu_run) ? "FAIL" : "PASS"));          \
			}                                                                                                           \
		}                                                                                                               \
		__rv_simu_run;                                                                                                  \
	})

#define SIMU_RUN_TEST(test, sys, ...) __SIMU_RUN_TEST(test, test, sys, ##__VA_ARGS__)
#define SIMU_RUN_TEST_ID(test, id, sys, ...) __SIMU_RUN_TEST(NVMEIB_CONCAT2(test, NVMEIB_CONCAT2(_, id)), test, sys, ##__VA_ARGS__)
