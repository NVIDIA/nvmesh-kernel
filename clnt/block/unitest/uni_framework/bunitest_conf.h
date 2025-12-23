#ifndef BUNITEST_CONF_H
#define BUNITEST_CONF_H
/*****************************************************************************/
/* CLI Conf class (CLI Options & possibly from file in the future)
 * Configuration is an "Environment", i.e.: its available from any test code/modeule
 * Singleton is declared with default values.
 * To add configuration to module <XXX> you need to:
 * (-) declare "struct ut_conf_<XXX>" with the required parameters. in the module header file (H).
 * (-) include the header file (H) in this module C file (bunitest_conf.c).
 * (-) declare a forward declaration of "struct ut_conf_<XXX>" in this header.
 * (-) declare getter API to retrieve the config : ut_conf__get_<XXX>(void);
 * (-) implement the above API in the C module.
 * (-) use the new API in module XXX
 *
 * for any new parameter:
 * (-) add the parameter in the appropriate "struct ut_conf_<XXX>"
 * (-) add parsing & setting of the parameter.
 * */
#include "nvmeib_common_all.h"
#include "block/unitest/nvmeibc_simu_disk.h"

// various config parameters that might be of interest to all simulators.
struct ut_conf_base {
	bool		is_valgrind;								// use when running under valgrind. code will have tweaks to make such a run (which is much slower) work to allow this test suit to run successfully under valgrind for memleak/corruption debugging.
	bool		disableDataIntegrityTracking;
	bool debug_dump_funcs;
};

// conf object for the UT suit
struct ut_conf_bunitest {
	int 		nRep;										// how many times do we repeat the whole test suit
	char 		*config_path;
	char 	    *procfs_dump_path;
	bool		disableActTests, disableErasureTests, disableNreplicaTests, disableClientRestartTests;
	bool		disableEC_8plus2_exhastiveTests;
	bool        disableEC_seg_reloc_test;					// Jam and serjio dont deal well with this
	bool        disableEC_async_pause_cont;
	bool        enableEC_exhastiveTests;
	bool		disableECColdTests;
	bool		disableSimulatorTests;
	bool		disableCmpBlocks;
	bool 		do_cat_proc_before_destroy;
	bool 		run_gf_sanity_tests;
	bool		run_bin_traces_tests;
	bool		half_sync_async_mode;
};

// unit test configuration parameters
struct blk_unittest_conf {									// Params to unitest environment
	struct ut_conf_base				base;
	struct ut_conf_kernel			kernel_prm;
	struct ut_conf_transport		transport;
	struct ut_conf_bunitest			bunitest;
};


void ut_conf__init(void);
void ut_conf__print_help(void);								// print help & exit
void ut_conf__parse_args(int argc, char* argv[]);			// parse CLI options & set the singletone conf instance

// get instance for various layers. every layer that exports a conf object has its own getter to retrieve that object
const struct blk_unittest_conf		*ut_conf__get_instance(void);
const struct ut_conf_base		 	*ut_conf__get_base(void);
const struct ut_conf_kernel		 	*ut_conf__get_kernel(void);
const struct ut_conf_bunitest		*ut_conf__get_bunitest(void);
const struct ut_conf_transport		*ut_conf__get_transport(void);

// Conf modifications
void ut_conf__platform_io_sync_set(bool is_sync);					// set sync/async of IO callback
bool ut_conf__platform_io_sync_get(void);

#endif // BUNITEST_CONF_H

