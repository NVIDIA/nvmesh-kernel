#/usr/local/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

TOMA_DIR="toma"
HOME_PATH="\/Users\/${USER}\/projects"
HOME_DIR="nvmeib.drs"
REMOTE_PATH="\/home\/${USER}\/projects"
KERNEL_DIR="\/Users\/${USER}\/linux_src\/linux"
OFA_KERNEL="\/usr\/src\/ofa_kernel\/default"
OFED_DIR="\/Users\/${USER}\/OFED4\/MLNX_OFED_SRC-4.7-1.0.0.1\/SRPMS\/mlnx-ofa_kernel\/mlnx-ofa_kernel-4.7"

pattern_1="s/$REMOTE_PATH\/([^\/ ]*)([\/ ])(.*)/$HOME_PATH\/$HOME_DIR\2\3/g"
pattern_2="s/$OFA_KERNEL\/(.*)/$OFED_DIR\/\1/g"
pattern_3="s/from include\/(.*)/from $KERNEL_DIR\/include\/\1/g"
pattern_4="s/^include\/(.*)/$KERNEL_DIR\/include\/\1/g"
pattern_5="s/^(nvmeibt_.*)(.*)/$HOME_PATH\/$HOME_DIR\/$TOMA_DIR\/\1\2/g"

while read line; do
	#echo "$line"
	#output=$(echo "$line" | sed -E "$pattern_one")
	while : ;
	do
	    output=$(echo "$line" | sed -E "$pattern_1")
	    if [ "$output" != "$line" ]; then
		if [ -z $vv ]; then
		    #echo "$line"
		    #echo "======================"
		    vv="set"
		fi
		line=$output
	    else
		break
	    fi
	done
	unset vv
	while : ;
	do
	    output=$(echo "$line" | sed -E "$pattern_2")
	    if [ "$output" != "$line" ]; then
		if [ -z $vv ]; then
		    #echo "$line"
		    #echo "======================"
		    vv="set"
		fi
		line=$output
	    else
		break
	    fi
	done
	unset vv
	while : ;
	do
	    output=$(echo "$line" | sed -E "$pattern_3")
	    if [ "$output" != "$line" ]; then
		if [ -z $vv ]; then
		    #echo "$line"
		    #echo "======================"
		    vv="set"
		fi
		line=$output
	    else
		break
	    fi
	done
	unset vv
	while : ;
	do
	    output=$(echo "$line" | sed -E "$pattern_4")
	    if [ "$output" != "$line" ]; then
		if [ -z $vv ]; then
		    #echo "$line"
		    #echo "======================"
		    vv="set"
		fi
		line=$output
	    else
		break
	    fi
	done
	unset vv
	while : ;
	do
	    output=$(echo "$line" | sed -E "$pattern_5")
	    if [ "$output" != "$line" ]; then
		if [ -z $vv ]; then
		    #echo "$line"
		    #echo "======================"
		    vv="set"
		fi
		line=$output
	    else
		break
	    fi
	done
	unset vv
	echo "$output"
done


# pattern_kernel="s/^\/home\/${USER}\/projects\/[^\/]*\/(.*):([0-9]*):([0-9]*):( +)([Ee]rror|[Ww]arning|[Nn]ote|fatal error):(.*)/\/Users\/${USER}\/projects\/nvmeib\.drs\/\1:\2:\3:\4\5:\6/"
# pattern_toma="/^\//!s/^(nvmeibt_.*):([0-9]*):([0-9]*):( +)([Ee]rror|[We]arning):(.*)/\/Users\/${USER}\/projects\/nvmeib\.drs\/$TOMA_DIR\/\1:\2:\3:\4\5:\6/"
# #        pattern_toma="s/^(nvmeibt_.*):([0-9]*):([0-9]*):( +)([Ee]rror|[We]arning):(.*)/\/Users\/${USER}\/projects\/nvmeib\.drs\/$TOMA_DIR\/\1:\2:\3:\4\5:\6/"
# pattern_linux="/^\//!s/^(include.*):([0-9]*):([0-9]*):( +)(note):(.*)/$KERNEL_DIR\/\1:\2:\3:\4\5:\6/"
# #        pattern_linux="s/^(include.*):([0-9]*):([0-9]*):( +)(note):(.*)/$KERNEL_DIR\/\1:\2:\3:\4\5:\6/"
# pattern_ofa="s/^In file included from $OFA_KERNEL\/(.*):([0-9]*):([0-9]*),( +)(.*)/$OFED_DIR\/\1:\2:\3,\4\5/"
# pattern_from="s/^( +)from ([^<].*):([0-9]*),( +)(.*)/$KERNEL_DIR\/\2:\3,\4\5/"


#while read line; do
#	#echo "$line"
#	#output=$(echo "$line" | sed -E "$pattern_one")
#	output=$(echo "$line" | sed -E "$pattern_one")
#	while [ "$line" != "$output" ]; then
#	    echo "$line"
#	    echo "-------------------------"
#	fi
##	echo "$line" | sed -E 's/^\/home\/${USER}\/projects\/.*?\/\(.*\):\([0-9]+\):\([0-9]+\):\(warning\|error\):\(.*\)$/\1:\2:\3/'
#	output=$(echo "$line" | sed -E "$pattern_kernel")
#	echo "1 --> $output"
##	echo "$line" | sed -E "$pattern_kernel"
#	if [ "$line" = "$output" ]; then
#	    output=$(echo "$line" | sed -E "$pattern_toma")
#	    echo "2 --> $output"
#	    if [ "$line" = "$output" ]; then
#		output=$(echo "$line" | sed -E "$pattern_linux")
#		echo "3 --> $output"
#		if [ "$line" = "$output" ]; then
#		    output=$(echo "$line" | sed -E "$pattern_ofa")
#		    echo "4 --> $output"
#		    if [ "$line" = "$output" ]; then
#			output=$(echo "$line" | sed -E "$pattern_from")
#			echo "5 --> $output"
#		    fi
#		fi
#	    fi
#	fi
#	echo "$output"
#done

