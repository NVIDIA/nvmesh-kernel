#!/bin/bash

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

usage_str="Usage: $0 [-a author] [-since '2019-01-15'] [-until '2019-01-20'] [-- './toma'] <branch_1> <branch_2>"
# Parse the arguments
author_str=""
path_str=""
since_str=""
until_str=""
branch_1=""
branch_2=""
diff_direction="b"		#b=bidirectional, t=to (in second branch not in first), f=from (in first branch not in second)
while [ "$#" -gt 0 ]; do
        cmd="$1"
        val="$2"
        case "$cmd" in
                -h|-\?) echo $usage_str; exit 1;;
                -a|-author) author_str="--author=$val "; shift;;
                --) path_str="-- $val "; shift;;
                -since) since_str="--since=$val "; shift;;
                -until) until_str="--until=$val "; shift;;
                -direction) diff_direction="$val"; shift;;
                -*) echo "unknown option: '$cmd'" >&2; echo $usage_str; exit 1;;
		*) branch_1="$1 "; branch_2="$2 "; shift;;
        esac
        case "$cmd" in
                -rt|-dt)
                        if [[ !("$val" =~ ^[0-9]+$) ]] ; then
                                echo "OOPS $cmd argument $val" >&2
                                exit 1
                        fi;;
        esac
        shift
done

echo comparing '"'$branch_1'"' with '"'$branch_2'"'
#echo "params: author_str=${author_str}; since_str=${since_str}; until_str=${until_str} branch_1=${branch_1}; branch_2=${branch_2}; path_str=${path_str};"
search_str='Change-Id:'
tmp1=/tmp/"$$"_1
tmp2=/tmp/"$$"_2
git log ${author_str}${since_str}${until_str}${branch_1}${path_str} | grep "$search_str" | sort -u > $tmp1
git log ${author_str}${branch_1}${path_str} | grep "$search_str" | sort -u > ${tmp1}_full_history
git log ${author_str}${since_str}${until_str}${branch_2}${path_str} | grep "$search_str" | sort -u > $tmp2
git log ${author_str}${branch_2}${path_str} | grep "$search_str" | sort -u > ${tmp2}_full_history
git_log_grep_str_spaces1=`diff --ignore-space-change $tmp1 ${tmp2}_full_history | grep '^< *Change-Id:' | awk '{print $3;}'`
git_log_grep_str_spaces2=`diff --ignore-space-change ${tmp1}_full_history $tmp2 | grep '^> *Change-Id:' | awk '{print $3;}'`
if grep 'extendedRegexp = true' ~/.gitconfig -q; then
    git_log_grep_str1=`echo $git_log_grep_str_spaces1 | sed 's/ /\\|/g'`
    git_log_grep_str2=`echo $git_log_grep_str_spaces2 | sed 's/ /\\|/g'`
else
    git_log_grep_str1=`echo $git_log_grep_str_spaces1 | sed 's/ /\\\\|/g'`
    git_log_grep_str2=`echo $git_log_grep_str_spaces2 | sed 's/ /\\\\|/g'`
fi;
#rm $tmp1 $tmp2 ${tmp1}_full_history ${tmp2}_full_history

if [ "$diff_direction" != "t" ]; then
    echo "------------ In $branch_1 missing from $branch_2 -------------"
    if [ -z "$git_log_grep_str1" ]; then
        echo "Nothing!"
    else
        git log --grep $git_log_grep_str1 ${branch_1}
    fi
    echo "------------ In $branch_1  -  Commits without a Change-Id -------------"
    git log ${author_str}${since_str}${until_str} --grep 'Change-Id' --invert-grep ${branch_1}${path_str}
fi
if [ "$diff_direction" != "f" ]; then
    echo "------------ In $branch_2 missing from $branch_1 -------------"
    if [ -z "$git_log_grep_str2" ]; then
        echo "Nothing!"
    else
        git log --grep $git_log_grep_str2 ${branch_2}
    fi
    echo "------------ In $branch_2  -  Commits without a Change-Id -------------"
    git log ${author_str}${since_str}${until_str} --grep "Change-Id" --invert-grep ${branch_2}${path_str}
fi
echo "------------ Last common commit of $branch_1 & $branch_2 -------------"
echo `git merge-base $branch_1 $branch_2`;
