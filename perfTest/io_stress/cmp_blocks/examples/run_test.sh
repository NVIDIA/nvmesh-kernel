#!/bin/bash
PROJ="../../..";
function __run_cmd() {	cmd=("$@"); echo -e "\e[1;33m************************ ${cmd}\e[0;39m"; eval ${cmd}; }
function echo_error { echo -e "\e[0;31m$*\e[0m"; }

function run_all_tests() {
	ERROR="";

	EXE="./cmp_blocks";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	DUMMY_ARGS="1 2 3 4 5 6 7 8 9 0 a b c d";
	make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	__run_cmd "${EXE} --help";
	# R1 compatibility mode, no metadata
	__run_cmd "${EXE} examples/R1_no_md/data_0 examples/R1_no_md/data_1";
	__run_cmd "${EXE} examples/R1_no_md/corrupt_0 examples/R1_no_md/corrupt_1";

	# R1 New format
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_no_md/data_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_no_md/corrupt_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/d_%d --md_path examples/R1_with_md/md_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/corrupt_%d --md_path examples/R1_with_md/md_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/d_%d --md_path examples/R1_with_md/md_wrong_edic_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/corrupt_%d --md_path examples/R1_with_md/md_never_written_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/d_%d --md_path examples/R1_with_md/md_bad_sector_%d";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/d_%d --md_path examples/R1_with_md/md_bad_and_good_%d";

	# Errors
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 213 --d_path examples/R1_with_md/d_%d --md_path examples/R1_with_md/md_%d --reconst_bmp 1";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 2 --rlba 213 --d_path examples/EC_3_2/d_%d --md_path examples/xx_%d";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path X --md_path X --reconst_bmp 0x2";
	__run_cmd "${EXE} -d 3 -p 2 --rlba 0 --d_path X --md_path X --reconst_bmp 0x29";
	__run_cmd "${EXE} -d 13 -p 20 --rlba 0 --d_path X --md_path X --reconst_bmp 0x29";

	# EC, no metadata
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 2 --rlba 213 --d_path examples/EC_3_2/d_%d";

	# EC correct slice tests
	MPATH="examples/EC_3_2/md_";
	DPATH="examples/EC_3_2/d_";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "${EXE} --verbose false --dbg_di false -d 3 -p 1 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 1 --rlba 555 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d --reconst_bmp 7 --dry_run true";

	# EC corrupted D1 slice tests
	DPATH="examples/EC_3_2/d_worng_d1_";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d --reconst_bmp 0x1d --dry_run true";

	# EC corrupted P0 slice tests
	DPATH="examples/EC_3_2/d_wrong_p_";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d --reconst_bmp 0x17 --dry_run true";

	# EC never written slice tests
	DPATH="examples/EC_3_2/d_wrong_p_";
	MPATH="examples/EC_3_2/md_never_written_";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d --reconst_bmp 0x1d --dry_run true";

	# EC real fixup of slice, non dry run, and verify!
	DPATH="examples/EC_3_2/d_worng_d1_";
	MPATH="examples/EC_3_2/md_";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d --reconst_bmp 0x1d --dry_run false";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "git checkout -q ${DPATH}1 ${MPATH}1";
	DPATH="examples/EC_3_2/d_wrong_p_";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d --reconst_bmp 0x17 --dry_run false";
	__run_cmd "${EXE} --verbose true  --dbg_di false -d 3 -p 2 --rlba 0 --d_path ${DPATH}%d --md_path ${MPATH}%d";
	__run_cmd "git checkout -q ${DPATH}3 ${MPATH}3";

	# Generate metadata for existing column of blocks
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	DIR="examples/0_GEN_MD";
	DPATH="${DIR}/d_0";
	__run_cmd "${EXE} --help";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 2 --rlba 227 --role P --d_path ${DIR}/d_0 --md_path ${DIR}/md_0 --dry_run true";
	__run_cmd "${EXE} --verbose fals --dbg_di false -d 3 -p 2 --rlba 227 --role P --d_path ${DIR}/d_0 --md_path ${DIR}/md_0 --dry_run true";

	# gen_md utility dry run parity with dbits
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 2 --rlba 227 --role P --d_path ${DIR}/d_0 --md_path ${DIR}/md_0 --dry_run true --dbits {1/3}";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 0 --rlba 227 --role P --d_path ${DIR}/d8k --md_path ${DIR}/md8k --dry_run true --dbits {11/5}";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 0 --rlba 227 --role P --d_path ${DIR}/d8k --md_path ${DIR}/md8k --dry_run true --dbits {-1/-1}";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 2 --rlba 227 --role P --d_path ${DIR}/d8k --md_path ${DIR}/md8k --dry_run true --dbits {-1/0}";

	# gen_md utility dry run data with meaningless dbits
	__run_cmd "${EXE} --verbose true --dbg_di false -d 3 -p 2 --rlba 227 --role D --d_path ${DIR}/d_0 --md_path ${DIR}/md_0 --dry_run true --dbits {1/3}";

	# gen_md utility dry run R1 blocks
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1 --rlba 227 --role R1 --d_path ${DPATH} --md_path ${DIR}/md_0              --dry_run true --dbits {1/3}";
	__run_cmd "${EXE} --verbose true --dbg_di false -d 1 -p 1            --role R1 --d_path ${DPATH} --md_path ${DIR}/md_0              --dry_run true";

	# Various error testing
	__run_cmd "${EXE} --dbits {wrong_format11/-1} ${DUMMY_ARGS}";
	__run_cmd "${EXE} --verbose nonbool           ${DUMMY_ARGS}";
	__run_cmd "${EXE} --role Q                    ${DUMMY_ARGS}";

	# verify that infra .so compiles properly, and run unitest
	EXE="./infra_shared_test";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/infra_shared;
	make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	__run_cmd "${EXE}";

	# verify that miniscrub compiles and uses .so properly
	EXE="./miniscrub_test";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../miniscrub;
	make clean; make NON_PROD=1;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};
	cd ${current_dir};

	# gen_md utility generate parity
	EXE="./gen_md";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../gen_md; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	DPATH="examples/data4k";
	__run_cmd "${EXE} --help";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_valid";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_wrong_edic";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_never_written";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_never_written2";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 113 --role P --d_path ${DPATH} --md_path ./examples/md_bad_sector";

	# gen_md utility dry run parity with dbits
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_valid         --dry_run true --dbits {1/3}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_valid         --dry_run true --dbits {11/5}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_wrong_edic    --dry_run true --dbits {-1/-1}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_wrong_edic    --dry_run true --dbits {-1/0}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role P --d_path ${DPATH} --md_path ./examples/md_never_written --dry_run true --dbits {-1/0}";

	# gen_md utility dry run data with/without meaningless dbits
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role D --d_path ${DPATH} --md_path ./examples/md_valid         --dry_run true --dbits {1/3}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role D --d_path ${DPATH} --md_path ./examples/md_wrong_edic    --dry_run true --dbits {4/2}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role D --d_path ${DPATH} --md_path ./examples/md_never_written --dry_run true --dbits {4/2}";
	__run_cmd "${EXE} --verbose false --dbg_di true --rlba 111 --role D --d_path ${DPATH} --md_path ./examples/md_wrong_edic    --dry_run true --dbits {11/-1}";

	# gen_md utility dry run R1 blocks
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role R1 --d_path ${DPATH} --md_path ./examples/md_valid         --dry_run true --dbits {1/3}";
	__run_cmd "${EXE} --verbose true --dbg_di false --rlba 227 --role R1 --d_path ${DPATH} --md_path ./examples/md_wrong_edic    --dry_run true --dbits {4/2}";
	__run_cmd "${EXE} --verbose true --dbg_di false            --role R1 --d_path ${DPATH} --md_path ./examples/md_never_written --dry_run true";
	__run_cmd "${EXE} --verbose true --dbg_di false            --role R1 --d_path ${DPATH} --md_path ./examples/md_never_written2 --dry_run true";

	# Various error testing
	__run_cmd "${EXE} --dbits {wrong_format11/-1} ${DUMMY_ARGS}";
	__run_cmd "${EXE} --verbose nonbool           ${DUMMY_ARGS}";
	__run_cmd "${EXE} --role Q                    ${DUMMY_ARGS}";
	cd - > /dev/null;

	# scan_locks utility
	EXE="./scan_locks_ec";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../scan_locks; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	sudo ${EXE} --help

	# di_parser utility
	EXE="./parse_block";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../di_parser; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};
	EXE="./dbgdi_log_test";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};

	EXE="./linear_di_test";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../linear_di_test; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};

	EXE="./aio_in_air";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../change_bio_in_air; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};
	
	LIB="lz4";
	echo -e "\e[1;32m******************************* ${LIB} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/lz4
	make BUILD_SHARED=no BUILD_STATIC=yes lib-release;
	ls *.so;
	cd ../trace_compress_lib;
	make all;
	cd ${current_dir};

	EXE="./cpager";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/lz4 && make clean && make BUILD_SHARED=no BUILD_STATIC=yes lib-release;
	cd ../trace_compress_lib && make clean all;
	cd ../traces_post_processor/formatters;
	make clean all;
	ls *.so;
	cd ..;
	rm -f ./libfmtrs.so; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};
	cd ${current_dir};

	EXE="./trace_daemon";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/trace_daemon_2.0;
	make clean all; # ${EXE} --help;
	[ ! -f ${EXE} ] && ERROR="-5";
	cd ${current_dir};
	if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	EXE="./build/bin/nvmeib_pipe_tracer";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/pipe_tracer;
	make clean all; # ${EXE} --help;
	[ ! -f ${EXE} ] && ERROR="-5";
	cd ${current_dir};
	if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi

	EXE="toma_rpc/link/read_conf";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/toma_rpc;
	make clean all;
	cmd="./toma_rpc";         echo $cmd; eval $cmd;
	EXE="./toma_link";
	[ ! -f ${EXE} ] && ERROR="-5";
	cd ${current_dir};
	if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	echo "${EXE} was built successfully";

	EXE="crash_analyzer.so"
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/tools/crash_analyzer
	make clean all;
	cd ${current_dir};

	EXE="./nvme";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/scripts/target/nvme-cli;
	make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE} --help;
	git checkout ${EXE}; # exe file is in git, no need to change it
	cd ${current_dir};

	EXE="test_pack_attach.py";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ};
	./management_cm/clnt/test/attach/${EXE}.py
	cd ${current_dir};

	EXE="./nvme";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/scripts/target/nvme-cli;
	make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE} --help;
	git checkout ${EXE}; # exe file is in git, no need to change it
	cd ${current_dir};

	EXE="./manual_fops";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	cd ../manual_fops; make clean all;
	[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	${EXE};

	EXE="gf_asm.o";
	echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	current_dir=$(pwd)
	cd ${PROJ}/clnt/block/datapath_ec; make all;
	cd ${current_dir};

	#DP_LIB is broken; too many low level (platform) features were not and will not be ported to DP_LIB
	#EXE="./nvmesh_dp_lib";
	#echo -e "\e[1;32m******************************* ${EXE} *******************************\e[0;39m";
	#cd ${PROJ}/clnt/block/um_integration/; make clean;
	#make all COMPILATION_DATE=dummy_build HAS_LOCAL_GIT=false;
	#[ ! -f ${EXE} ] && ERROR="-5"; if [ ! -z ${ERROR} ]; then echo_error "${EXE} could not be build, abort, rv ${ERROR}!"; return ${ERROR}; fi
	#${EXE};

	echo -e "\e[1;32m******************************* Done *******************************\e[0;39m";
	return $?;
}

clear;
run_all_tests 2>&1 | tee examples/run_test_res.txt;
if [ ${PIPESTATUS[0]} -eq 0 ]; then
	exit 0;
else
	exit 5;
fi

