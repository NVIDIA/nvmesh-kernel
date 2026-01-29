#/bin/sh

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

# usage: switch_env_to_git_branch.sh <branch_name>
# Saves and restores all of the modified files. Remember to save the files in the editor first
#
git_switch_env_header_line="GIT_SWITCH_ENV_HEADER_LINE"
echo "old_branch=`git rev-parse --abbrev-ref HEAD`, new_branch=$1"
git commit --all --allow-empty --message "$git_switch_env_header_line" || exit
git checkout $1 || { git reset HEAD^ ; exit ; }
last_commit_subject="`git log -1 --pretty=format:%s`"
echo "last_commit_subject=$last_commit_subject"
if [ "$last_commit_subject" == "$git_switch_env_header_line" ] ; then
	git reset HEAD^
fi
