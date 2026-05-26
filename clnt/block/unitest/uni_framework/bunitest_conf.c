/*
* SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
* SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
*/

#include "bunitest.h"
#include "bunitest_conf.h"
#include "io_pet_traces_controller.h"

// CLI options
#define ARG_NREP 		"-nRep"
#define ARG_NO_ACT 		"-noTestAct"
#define ARG_NO_ERASURE 	"-noTestEras"
#define ARG_NO_EC_8p2 	"-noTestEC8p2"
#define ARG_NO_EC_SRL 	"-noTestECsrl"
#define ARG_NO_EC_APC 	"-noTestECapc"
#define ARG_EC_ALL		"-ECAllPerm"
#define ARG_SB_ALL		"-SBAllPerm"
#define ARG_NO_EC_COLD	"-noECCold"
#define ARG_NO_NREPLICA "-noTestNrep"
#define ARG_DO_RESTARTS "-noTestRestart"
#define ARG_DO_SIM      "-doSim"
#define ARG_DEBUG		"-dbg"
#define ARG_DEBUG_TRACER "-tracedbg"
#define ARG_TRACER_TEST "-tracertest"
#define ARG_GOODPATH_DEBUG "-good-path-dbg"
#define ARG_GOODPATH_LOCKS_DEBUG "-good-path-locks-dbg"
#define ARG_FULL_ASYNC 	"-async"
#define ARG_FULL_SYNC 	"-sync"
#define ARG_HALF_SYNC 	"-hsync"
#define ARG_MAX_IOS 	"-maxIOs"
#define ARG_PRINT_PROC 	"-proc"
#define ARG_ECPU_COUNT	"-ecpu-count"
#define ARG_VALGRIND	"-valgrind"
#define ARG_GF			"-gf"
#define ARG_UNI_CONF	"-conf"
#define ARG_DRAIN_WORKQUEUE_IS_INFINITE "-drain_workqueue_is_infinite"
#define ARG_PROCFS_DUMP_PATH "-procfsDumpPath"
#define ARG_DEBUG_DUMP_FUNCS "-debug_dump_funcs"
#define ARG_IO_PET_BUFFER_SIZE "-nvmeibc_io_pet_buffer_size"
#define ARG_IO_PET_VERBOSE "-nvmeibc_io_pet_verbose"
#define ARG_IO_PET_MAX_TRACED_OPS_PER_CPU "-nvmeibc_io_pet_max_traced_ops_per_cpu"

// command arguments to the executable
#define CMD_VALGRIND    "valgrind --leak-check=yes"
#define ARG_PROG_NAME	"./blk_unitest"
#define ARG_TO_FILE		">out.txt 2>&1"

// The singleton conf object, with defaults settings
static struct blk_unittest_conf	ut_conf = {						// defaults
	.base = {
		.is_valgrind = false,									// By default were not running under valgrind
		.disableDataIntegrityTracking = false,					// By default DI tracking is enabled
	},
	.kernel_prm = {
		.num_ecpu = 0,
		.drain_workqueue_is_infinite = 0,
	},
	.transport = {
		.is_disk_callback_sync = true,							// When true, callback will be syncronous - less accurate but much faster
	},
	.bunitest = {
		.nRep = 1,												// By default do one repetition of tests
		.config_path = 0, 							  			// By default Developer mode
		.disableActTests = 0, 									// By default actual  volumes tests are enabled
		.disableErasureTests = 0,								// By default erasure-coded volumes tests are enabled
		.disableNreplicaTests = 0,
		.disableClientRestartTests = 0,							// By default restart tests are enabled
		.disableEC_8plus2_exhastiveTests = 0,
		.disableEC_seg_reloc_test = 0,
		.disableEC_async_pause_cont = 0,
		.enableEC_exhastiveTests = 0,							// Not all permutations will be tested by default enable for All permutations (1 iteration will be ~2 minutes)
		.disableECColdTests = 0,
		.disableSimulatorTests = true,							// By default Kernel Simulator tests are disabled
		.disableCmpBlocks = false,
		.do_cat_proc_before_destroy = 0,
		.run_gf_sanity_tests = 0,
		.run_bin_traces_tests = 0,
		.half_sync_async_mode = 0,								// Consider to make this default
	},
};

void ut_conf__init(void){
	ut_conf.kernel_prm.num_ecpu	= get_default_ecpu_count();
}

const struct blk_unittest_conf		*ut_conf__get_instance(void)		{ return &ut_conf;				}
const struct ut_conf_base		 	*ut_conf__get_base(void)			{ return &ut_conf.base;			}
const struct ut_conf_kernel		 	*ut_conf__get_kernel(void)			{ return &ut_conf.kernel_prm;	}
const struct ut_conf_bunitest		*ut_conf__get_bunitest(void)		{ return &ut_conf.bunitest;		}
const struct ut_conf_transport 		*ut_conf__get_transport(void)		{ return &ut_conf.transport;	}

void ut_conf__print_help(void){
	pr_alert("Usage: \n"
				"\t[-h or -help to see this help]\n"
				"\t[" ARG_DEBUG "            N. 1=all including _DBG(), 0=No _ND(), -1=No _NT() -2=No _NI(),_NW(),_NE()]\n"
				"\t[" ARG_DEBUG_TRACER "       [0,6] 0=No traces, 1=_NE only, 2=_NW (and above), 3=_NI, 4=_NT, 5=_ND, 6=_NF]\n"
				"\t[" ARG_GOODPATH_DEBUG "  Goodpath only: [0,6] See above\n"
				"\t[" ARG_GOODPATH_LOCKS_DEBUG "  Goodpath locks only: [0,6] See above\n"
				"\t[" ARG_TRACER_TEST "     Run tracer test and exit, discards any other flags\n"
				"\t[" ARG_NREP "           <Amount of repetitions of all the unitests>]\n"
				"\t[" ARG_UNI_CONF "             Path to unitest configuration file]\n"
				"\t[" ARG_NO_ACT "      Disable tests for actual  volumes (run only VV's tests)]\n"
				"\t[" ARG_NO_ERASURE "     Disable tests for erasure-coded volumes\n"
				"\t[" ARG_NO_EC_8p2 "    Disable exhaustive 5[sec] degraded IO to EC 8+2\n"
				"\t[" ARG_NO_EC_SRL "    Disable segment relocation tests on EC 8+2\n"
				"\t[" ARG_NO_EC_APC "    Disable async pause cont on EC 8+2\n"
				"\t[" ARG_EC_ALL "      Enable exhaustive multi-minute Permutations for EC 8+2\n"
				"\t[" ARG_NO_EC_COLD "       Disable EC cold recovery tests\n"
				"\t[" ARG_GF "             Enable GF Arithmetics sanity tests\n"
				"\t[" ARG_NO_NREPLICA "     Disable tests for N-relpica (N>2) volumes\n"
				"\t[" ARG_DO_RESTARTS "  Disable client restart tests (long tests without IO)\n"
				"\t[" ARG_DO_SIM "          Enable tests for Simulator modules\n"
				"\t[" ARG_MAX_IOS "         Max in air IO's that block device supports per cpu\n"
				"\t[" ARG_FULL_ASYNC "          Very slow but more thorough test. Uses async threads for callbacks, can use " ARG_FULL_SYNC " to force sync, " ARG_HALF_SYNC ", for half sync/async]\n"
				"\t[" ARG_ECPU_COUNT "     set the number of emulated CPU's executing the work]\n"
			    "\t[" ARG_VALGRIND   "       instructs the test code to adapt to running under valgrind (IO's will probably timeout when running under valgrind without this option)\n"
				"\t[" ARG_PRINT_PROC "           print content or /proc files when unitest finishes. Default - no print]\n"
				"\t[" ARG_PROCFS_DUMP_PATH "           dump procfs to filesystem. Default - no dump]\n"
				"\t[" ARG_DRAIN_WORKQUEUE_IS_INFINITE " wait for kernel workqueue drain will become infinite; by default - false ]\n");
	unitest_print("\t[" ARG_IO_PET_BUFFER_SIZE " <bytes> set simulator IO PET buffer size. Default - 4096]\n"
				"\t[" ARG_IO_PET_VERBOSE " <0|1> set simulator IO PET verbose mode. Default - 1]\n"
				"\t[" ARG_IO_PET_MAX_TRACED_OPS_PER_CPU " <value> set simulator IO PET max traced operations per CPU. Default - 0 (unlimited)]\n");
	unitest_print("\t---------------------------------------------------------------------------------\n"
				"\t Find memory leaks use:\t\t" CMD_VALGRIND " " ARG_PROG_NAME " " ARG_NREP " 1\n"
				"\t Redirect to file  use:\t\t" ARG_PROG_NAME " " ARG_NREP " 1 " ARG_DEBUG " 0 " ARG_TO_FILE "\n"
				"\t Combination: " CMD_VALGRIND " " ARG_PROG_NAME " " ARG_NREP " 1 " ARG_DEBUG " -1 " ARG_TO_FILE "\n"
				"\t valgrind --log-file=\"val.log\" --track-origins=yes --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all ./blk_unitest -dbg -2 -tracedbg 0 -valgrind -nRep 1 \n\n"
				"\t Debug latest run: gdb " ARG_PROG_NAME " \'ls -t core.* | tail -1\'\n"
				"\t Debug stuck  run: ps -ef | grep " ARG_PROG_NAME " | grep -v 'auto'; gdb " ARG_PROG_NAME " -p 14436\n");
	unitest_print("\n");
	exit(0);
}

void ut_conf__parse_args(int argc, char* argv[]){
	int i;
	ut_conf__init();
	for (i=1; i<argc; ++i ){
		if	   ( !strcmp( argv[i], ARG_NREP 		)){ ++i; if (i<argc) ut_conf.bunitest.nRep = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_ECPU_COUNT	)){ ++i; ut_conf.kernel_prm.num_ecpu = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_UNI_CONF     )){ ++i; ut_conf.bunitest.config_path = argv[i];}
		else if ( !strcmp( argv[i], ARG_NO_ACT		)){ ut_conf.bunitest.disableActTests  = 1; }
		else if ( !strcmp( argv[i], ARG_PROCFS_DUMP_PATH	)){ ++i; ut_conf.bunitest.procfs_dump_path = argv[i];}
		else if ( !strcmp( argv[i], ARG_NO_ERASURE	)){ ut_conf.bunitest.disableErasureTests = 1; }
		else if ( !strcmp( argv[i], ARG_NO_EC_8p2	)){ ut_conf.bunitest.disableEC_8plus2_exhastiveTests = 1; }
		else if ( !strcmp( argv[i], ARG_NO_EC_SRL	)){ ut_conf.bunitest.disableEC_seg_reloc_test = 1; }
		else if ( !strcmp( argv[i], ARG_NO_EC_APC	)){ ut_conf.bunitest.disableEC_async_pause_cont = 1; }
		else if ( !strcmp( argv[i], ARG_EC_ALL		)){ ut_conf.bunitest.enableEC_exhastiveTests = 1; }
		else if ( !strcmp( argv[i], ARG_NO_EC_COLD	)){ ut_conf.bunitest.disableECColdTests = 1; }
		else if ( !strcmp( argv[i], ARG_GF			)){ ut_conf.bunitest.run_gf_sanity_tests = 1; }
		else if ( !strcmp( argv[i], ARG_NO_NREPLICA	)){ ut_conf.bunitest.disableNreplicaTests = 1; }
		else if ( !strcmp( argv[i], ARG_DO_RESTARTS	)){ ut_conf.bunitest.disableClientRestartTests = true; }
		else if ( !strcmp( argv[i], ARG_DO_SIM		)){ ut_conf.bunitest.disableSimulatorTests = false; }
		else if ( !strcmp( argv[i], "-signal" 		)){ i++;}
		else if ( !strcmp( argv[i], ARG_DEBUG		)){ ++i; if (i<argc) nvmeibc_debug_level = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_DEBUG_TRACER	)){ ++i; if (i<argc) tracer_nvmeibc_debug_level = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_GOODPATH_DEBUG)){ ++i; if (i<argc) goodpath_nvmeibc_debug_level = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_GOODPATH_LOCKS_DEBUG)){ ++i; if (i<argc) goodpath_nvmeibc_locks_debug_level = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_TRACER_TEST	)){ ut_conf.bunitest.run_bin_traces_tests = true; }
		else if ( !strcmp( argv[i], ARG_MAX_IOS		)){ ++i; if (i<argc) max_ios_per_cpu = atoi(argv[i]); }
		else if ( !strcmp( argv[i], ARG_PRINT_PROC	)){ ut_conf.bunitest.do_cat_proc_before_destroy = 1; }
		else if ( !strcmp( argv[i], ARG_FULL_ASYNC	)){ ut_conf.transport.is_disk_callback_sync = false; }
		else if ( !strcmp( argv[i], ARG_FULL_SYNC	)){ ut_conf.transport.is_disk_callback_sync = true; }
		else if ( !strcmp( argv[i], ARG_HALF_SYNC	)){ ut_conf.bunitest.half_sync_async_mode = true; }
		else if ( !strcmp( argv[i], ARG_VALGRIND		)){ ut_conf.base.is_valgrind = true; }
		else if ( !strcmp( argv[i], ARG_DRAIN_WORKQUEUE_IS_INFINITE)){ ut_conf.kernel_prm.drain_workqueue_is_infinite = true; }
		else if ( !strcmp( argv[i], ARG_DEBUG_DUMP_FUNCS	)){ ut_conf.base.debug_dump_funcs = true; }
		else if ( !strcmp( argv[i], ARG_IO_PET_BUFFER_SIZE	)){ ++i; if (i<argc) sim_io_pet_controller_set_buffer_size((size_t)strtoull(argv[i], NULL, 0)); }
		else if ( !strcmp( argv[i], ARG_IO_PET_VERBOSE	)){ ++i; if (i<argc) sim_io_pet_controller_set_verbose(!!atoi(argv[i])); }
		else if ( !strcmp( argv[i], ARG_IO_PET_MAX_TRACED_OPS_PER_CPU	)){ ++i; if (i<argc) sim_io_pet_controller_set_max_traced_ops_per_cpu((unsigned)strtoul(argv[i], NULL, 0)); }
		else {
			unitest_print("Error: Unknown argument %d: \"%s\"\n", i, argv[i] );
			ut_conf__print_help();
		}
	}
	// Default CI execution: ut_conf.bunitest.config_path = "clnt/block/unitest/ci.cfg";
}

void ut_conf__platform_io_sync_set(bool is_sync){ut_conf.transport.is_disk_callback_sync = is_sync;}
bool ut_conf__platform_io_sync_get(void){return ut_conf.transport.is_disk_callback_sync;}
