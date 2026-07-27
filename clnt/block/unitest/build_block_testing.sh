#!/usr/bin/env bash
set -x
clear
echo "----------------- Block Unitest ------------------------"

COMMIT_ID=$(git log 2>/dev/null -n1 --format=%h)
CHANGE_ID=$(git log 2>/dev/null -n1 --format=%b | awk '/^Change-Id: / {print $2}')
VER_TAGID=$(git describe --abbrev=0)
BRANCH_NAME=$(git symbolic-ref 2>/dev/null --short --quiet HEAD) || BRANCH_NAME=$(git rev-parse 2>/dev/null --abbrev-ref HEAD)

set -e #exit on first error

if [ -z "$COMMIT_ID" ]; then
	COMMIT_ID='deadbeef'
fi
if [ -z "$CHANGE_ID" ]; then
	CHANGE_ID='deadbeef'
fi

if [ -z "$VER_TAGID" ]; then
	VER_TAGID='v0.0.0'
fi

if [ -z "$BRANCH_NAME" ] ; then
	BRANCH_NAME='Unknown'
fi

if [ $# -eq 0 ] || [ "$1" = "help" ] || [ "$1" = "h" ] || [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
	echo "Usage: ./build_block_testing.sh <command> <iterations>"
	echo "First parameter is the type of command to run it can be one of:"
	echo "clean/build/rebuild/run/exec/ci/depend/help"
	echo ""
	echo "Second parameter will only affect run/exec/ci and will set the number of unitest repetitions"
	echo ""
	echo "Command explanation:"
	echo "clean   - does: make clean in the unitest folder"
	echo "rebuild - does: make clean and make all"
	echo "build   - does:   			 make all"
	echo "exec    - does: unitest with 10000 (can be overriden by second parameter) iterations, the blk_unitest file must exist"
	echo "run     - does: 'rebuild', then runs 'exec'"
	echo "ci	  - does: 'run' with 5 iterations + gathers logs (continuos integration)"
	echo "nightly - does: heavy test - run tests multiple times with/without sanitizers + gathers logs (continuos integration)"
	echo "sanity  - does: sanity test - run tests several times with/without sanitizers + gathers logs (continuos integration)"
	echo "help    - shows this help message, will also be shown without parameters or using -h, h or --help"
	echo ""
	echo "In case of failure, this script will output the location of the core file, as expected it to be or hints"
	echo "Return code in case of failure will be the blk_unitest failure code"
	exit 1
fi

# ---------- Preparing settings
echo "args:$@"
echo "Cur dir:"$(pwd)
MAKE_VER=`make --version | head -1 |  sed 's/[^0-9.]//g' | cut -c 1`
echo "Make version:$MAKE_VER"
TEST_SUB_DIR=./clnt/block/unitest/
AUTOGEN_DIR=./autogen
if ! [ -d $TEST_SUB_DIR ]; then
	echo $TEST_SUB_DIR "-->Does not exist, trying to run in current dir..."
	TEST_SUB_DIR=./
	AUTOGEN_DIR=../../../autogen
fi
REPEAT="2"
MAKE_CMD=make

clean () {
	echo "-->Cleaning... "
	(cd $TEST_SUB_DIR && $MAKE_CMD clean)
}

write_mutiple () {
	logger -s "[$(date +%c)]: $0 - $1"
	echo "[$(date +%c)]: $0 - $1" >> $log_file
}

execute_unitest () {
	echo "-->Running: " $runcmd
	set -o pipefail
	ulimit -c unlimited
	eval $runcmd
}


build() {
	echo "-->Build started (commit_id:"$COMMIT_ID",change_id:"$CHANGE_ID", Tag:"$VER_TAGID", Branch:"$BRANCH_NAME") ${@}"
	if [ "$MAKE_VER" -gt 3 ]; then
		PARAM_FOR_MAKE="--print-directory --output-sync=recurse"		# Make will printe when it goes in and out of directory
	fi
	full_make_cmd="$MAKE_CMD write_compilation_cmd COMMIT_ID=$COMMIT_ID VER_TAGID=$VER_TAGID BRANCH_NAME=$BRANCH_NAME -j 10 $PARAM_FOR_MAKE ${@}";
	echo -e "|\e[0;32m$full_make_cmd\e[0;0m|";
	(cd $TEST_SUB_DIR && ${full_make_cmd})
	ret_code=$?
	if [ $ret_code != 0 ]; then
		echo "Error : [$ret_code] when executing make write_compilation_cmd"
		exit $ret_code
	fi
	full_make_cmd="$MAKE_CMD all COMMIT_ID=$COMMIT_ID VER_TAGID=$VER_TAGID BRANCH_NAME=$BRANCH_NAME -j 10 $PARAM_FOR_MAKE ${@}";
	echo -e "|\e[0;32m$full_make_cmd\e[0;0m|";
	(cd $TEST_SUB_DIR && ${full_make_cmd})
	ret_code=$?
	if [ $ret_code != 0 ]; then
		echo "Error : [$ret_code] when executing make"
		exit $ret_code
	fi
	echo "-->Done"
}

TRACES_FLAGS="-dbg 1 -tracedbg 3 -good-path-dbg 3"
# ---------- Allows killing the running execution with ctrl+c
case "$1" in
	"clean")
		shift
		clean
		;;
	"build")
		shift
		build ${@} USE_RELEASE=0 USE_SANITIZERS=1
		;;
	"build-tsan")
		shift
		build ${@} USE_RELEASE=0 USE_SANITIZERS=0 USE_THREAD_SANITIZER=1
		;;
	"rebuild")
		shift
		clean
		build ${@} USE_RELEASE=0 USE_SANITIZERS=1
		;;
	"run")
		shift
		clean
		build ${@}
		runcmd="./blk_unitest -async -nRep $REPEAT $TRACES_FLAGS  2>&1 | tee out.txt"
		execute_unitest
		# If it crashed analyze the core files
		./analyze_core.sh
		;;
	"run-tsan")
		shift
		runcmd="TSAN_OPTIONS=\"suppressions=suppressions.tsan\" ./blk_unitest $TRACES_FLAGS ${@} 2>&1 | tee out.txt"
		execute_unitest
		# If it crashed analyze the core files
		./analyze_core.sh
		;;
	"exec")
		shift
		build ${@}
		runcmd="./blk_unitest -async -nRep $REPEAT $TRACES_FLAGS 2>&1 | tee out.txt"
		execute_unitest
		# If it crashed analyze the core files
		./analyze_core.sh
		;;
	"nightly")
		shift
		NIGHTLY_FLAGS="-async -conf ./nightly.cfg"
		clean
		build ${@} USE_SANITIZERS=1
		execute_unitest
		runcmd="./blk_unitest -debug_dump_funcs $NIGHTLY_FLAGS $TRACES_FLAGS 2>&1 | tee out.txt"
		execute_unitest -debug_dump_funcs
		clean
		build ${@}
		runcmd="./blk_unitest $NIGHTLY_FLAGS $TRACES_FLAGS 2>&1 | tee out.txt"
		execute_unitest
		;;
	"sanity")
		shift
		SANITY_FLAGS="-nRep 3 $TRACES_FLAGS" 
		clean
		build ${@} USE_RELEASE=0 USE_SANITIZERS=1
		runcmd="./blk_unitest $SANITY_FLAGS  2>&1 | tee sanity_sync_r0_s1.out.txt"
		execute_unitest
		clean
		build ${@} USE_RELEASE=0 USE_SANITIZERS=1
		runcmd="./blk_unitest -async $SANITY_FLAGS  2>&1 | tee sanity_async_r0_s1.out.txt"
		execute_unitest
		clean
		build ${@} USE_RELEASE=1
		runcmd="./blk_unitest $SANITY_FLAGS  2>&1 | tee sanity_sync_r1_s0.out.txt"
		execute_unitest
		clean
		build ${@} USE_RELEASE=1
		runcmd="./blk_unitest -async $SANITY_FLAGS  2>&1 | tee sanity_async_r1_s0.out.txt"
		execute_unitest
		;;
	"ci")
		# ---------- For continous integration: generate logs and coredump location, might not die with ctrl+c
		shift
		MAKE_CMD="scl enable devtoolset-8 -- make"
		#if [ ! -z "$1" ]; then
		#	REPEAT=$1
		#	shift
		#	echo "Using custom iteration number: $REPEAT"
		#fi
		CI_FLAGS="-async -conf ./ci.cfg $TRACES_FLAGS"
		clean
		build ${@} USE_RELEASE=1
		extra_args=("" "-debug_dump_funcs")
		for e_arg in "${extra_args[@]}";do
			runcmd="./blk_unitest $e_arg $CI_FLAGS -nRep $REPEAT 2>&1 | tee out.txt"
			execute_unitest &
			ppid=$!
			wait $ppid
			retcode=$?
			dir=`pwd`
			log_file="$dir/build_block_testing.log"
			result_file="$dir/result.txt"
			if [ -e $log_file ]; then
				rm $log_file
			fi
			if [ $retcode != 0 ]; then
				echo "RC:$retcode" > $result_file
				write_mutiple "Failed with exit code: $retcode"
				expected_pid=$((ppid + 1))
				max_pid=32768
				if [ "$expected_pid" -gt "$max_pid" ]; then
					expected_pid=1
				fi
				core_file=`ls -t core*|head -1`
				if [ -z $core_file ]; then
					core_pat=`cat /proc/sys/kernel/core_pattern`
					write_mutiple "Could not find core file in current directory $dir, expected core pattern: $core_pat, expected core name core.$expected_pid"
					echo "CORE:None" >> $result_file
					echo "ERROR:No core file found in $dir, system core pattern is $core_pat" >> $result_file
				else
					if [ "core.$expected_pid" = "$core_file" ]; then
						write_mutiple "Core file location $dir/$core_file"
						echo "CORE:$dir/$core_file" >> $result_file
					else
						write_mutiple "Core file expected to be core.$expected_pid, but found $core_file"
						write_mutiple "Core file location $dir/$core_file"
						echo "CORE:$dir/$core_file" >> $result_file
						echo "ERROR:Core file expected to be core.$expected_pid, but found $core_file" >> $result_file
					fi
				fi
				exit $retcode
			fi
		done
		echo "RC:0" > $result_file
		;;
esac

#cd -

