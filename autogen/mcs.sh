#!/bin/bash -x

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# Auto generate mcs *.h files; invoked by autogen/Makefile

if [[ $# -ne 4 ]] ; then
    echo 'Usage: mcs.sh scheme.py clnt-scheme.json toma-scheme.json srv-scheme.json'
    exit 1
fi

scheme_py=$1
clnt_scheme_json=$2
toma_scheme_json=$3
srv_scheme_json=$4

#echo scheme_py=$scheme_py
#echo clnt_scheme_json=clnt_scheme_json
#echo toma_scheme_json=toma_scheme_json
#echo srv_scheme_json=$srv_scheme_json

PYTHON_BIN=$(type -fp type -fp python3 python | head -1)
retVal=$?
if [ $retVal -ne 0 ]; then
	echo -en "Could not find python3 executable"
    mcs_exit
fi

"$PYTHON_BIN" -c "import sys; assert sys.version_info[0] > 3 or sys.version_info[0] == 3 and sys.version_info[1] >= 8, 'python version should be greater than 3.8'"

mcs_exit()
{
    echo -en "\n$1 - Exiting...\n"
    exit 1
}

if which astyle >& /dev/null ; then
    USE_ASTYLE=true
else
    USE_ASTYLE=false
    echo "   [formatter astyle package not installed]"
fi

gen_file()
{
    local gen_file_name=$1
    local dep_file_name=$2

    if [ ! -e $gen_file_name -o \
	$gen_file_name -ot $dep_file_name -o \
	$gen_file_name -ot $scheme_py ]; then
	    echo "Generating $gen_file_name"
	    $PYTHON_BIN $scheme_py $dep_file_name $gen_file_name || \
	    mcs_exit "Fail to gen $gen_file_name"
	    if $USE_ASTYLE ; then
		astyle --indent=tab --suffix=none $gen_file_name
	    fi
    else
	    echo "'$gen_file_name' is up to date"
    fi

}

#generate mcs scheme file on common
if [ ! -e common/nvmeib_mcs_header.h -o \
    common/nvmeib_mcs_header.h -ot $scheme_py ]; then
	echo "Generating common/nvmeib_mcs_header.h"
	$PYTHON_BIN $scheme_py --header common/nvmeib_mcs_header.h || \
	mcs_exit "Fail to gen mcs nvmeib_mcs_header.h"
	if $USE_ASTYLE ; then
	    astyle --indent=tab --suffix=none common/nvmeib_mcs_header.h
	fi
else
	echo "'common/nvmeib_mcs_header.h' is up to date"
fi

#generate mcs *stub.h files
gen_file clnt/nvmeibc_mcs_stub.h $clnt_scheme_json
gen_file toma/nvmeibt_mcs_stub.h $toma_scheme_json
gen_file srv/nvmeibs_mcs_stub.h $srv_scheme_json
