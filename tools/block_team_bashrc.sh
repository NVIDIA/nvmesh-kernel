# .bashrc

# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

[ "`hostname`" == "danielhsh-laptop" ] && IS_LOCAL="Y" || IS_LOCAL="N";
if [ -f /etc/bashrc ]; then
	. /etc/bashrc
fi
# Uncomment the following line if you don't like systemctl's auto-paging feature:
# export SYSTEMD_PAGER=

# ------------------------------------ Generic Stuff ---------------------------
MY_DEV_SERVER='n34 n37 n38 n39'
MY_PROJECTS_DIR="$HOME/projects"
NVROCKS_PREFIX="nvrock";
NVMESH_PREFIX="nvmesh";
NVMESH_DIR_OPT="/var/opt/${NVMESH_PREFIX}"; # Toma config stuff
NVMESH_DIR_LOG="var/log/${NVMESH_PREFIX}"; # NVMesh binary traces
NVMESH_DIR_SRC="/opt/${NVMESH_PREFIX}";	# NVMesh repository
NVMESH_DIR_RUN="/var/run/${NVMESH_PREFIX}";	# Run time pids
NVMESH_DIR_ETC="/etc/${NVMESH_PREFIX}";	# config files
#unalias -a
if [[ $IS_LOCAL == "Y" ]]; then
	THIS_FILE=~/zshrc
	source /usr/share/bash-completion/completions/git;
else
	THIS_FILE=~/.bashrc
fi
if [ -z "$cur_branch" ]; then
	cur_branch="wip_work";
fi
alias ll='ls -lhA --group-directories-first --color=auto'
alias lsd='ls -lhA --group-directories-first | pr -2Tn -W200'
#alias which='(alias; declare -f) | /usr/bin/which --tty-only --read-alias --read-functions --show-tilde --show-dot'
alias grep='grep -a --color=auto'
alias gg=git_grep
alias gb=git_branch
alias view='less -RN'
[ -z "$(which pssh)" ] && PSSH_UTIL='parallel-ssh' || PSSH_UTIL='pssh';

function echo_red() {    echo -e "\e[0;31m$*\e[0m"; }
function echo_green() {  echo -e "\e[0;32m$*\e[0m"; }
function echo_yellow() { echo -e "\e[0;33m$*\e[0m"; }
function echo_title() {  echo -e "~~~~~~~~~~~~~ \e[16;34m$*\e[0;39m:"; }

function NVMESH_help() {
	clear;
	alias | pr -1Tn
	echo "typeset -f <func_name>"
	declare -F | cut -d ' ' -f 3 | grep -e '^NVMESH' -e '^APP' -e '^git_' -e '^LOGS_' -e '^NVROCKS'
	if [ $# -eq 0 ]; then
		echo "params: optional function name to display"
	else
		clear; typeset -f "$1"
	fi
	source ${THIS_FILE};
	echo -e "--------------------- \e[0;32mR&D Docs\e[0;39m ---------------------"
	echo "5.1 Videos (Recorded) -    server2:/home/qa/training/data_services_team"
	echo -e "--------------------- \e[0;32mNVRocks Clusters\e[0;39m ---------------------"
	echo "ssh opc@infra-jump-ashburn;          ssh nvmesh-ci-8012-n{1-10}  : NVRocks OCI";
	echo "    ssh client-5010-n{1-10}          ssh      in-c99021-n{1-10}  : NVRocks Luster";
	echo "NVrocksDev($MY_DEV_SERVER)"
	echo "Mgmt UI: From lapotop: ssh -f -N -L 4034:localhost:4000 n34;   Type in chrome: http://localhost:4034/";
	echo "Jenkins: ssh mtv-excelero1.mtl.labs.mlnx; cd /logs/jenkins/jenkins-MTV-CI-Build-15839/; /home/alexander/pager.py"; # root, password 3tango
	echo -e "--------------------- \e[0;32mNVmesh Production Clusters\e[0;39m ---------------------"
	echo "ssh opc@infra-jump-madrid;           ssh nvmesh-ci-1646-n{1-10}, ssh -f -N -L 1646:nvmesh-ci-1646-n1:4000 opc@infra-jump-madrid;  https://localhost:1646/";
	echo "ssh root@10.65.34.67;                ssh in-c{1000-1015}-n{1-10}";
	echo "Orange:  Ctrl+B + [0-9] to select window, dd skip=in, seek=out";
}

function APP_calc() {
	local INPUT="$*";
	if [[ "$INPUT" == *"."* ]]; then
		echo -e "{${INPUT}} = \t"`echo "${INPUT}" | bc -l`;
	else
		local RV=$(($INPUT));
		echo -e "{${INPUT}} = \t"`printf '%d   =  0x%x  =  0x%X ' ${RV} ${RV} ${RV}` = `echo "obase=2;${RV}" | bc`;
	fi
}

function NVMESH_perf() {
	PERF_EXE="/usr/bin/perf";
	sudo rm -f perf.data;
	sudo ${PERF_EXE} record -a -g sleep 5;
	sudo ${PERF_EXE} report;
	#sudo rm -f perf.data; sudo perf record -a -g sleep 5; sudo perf report;
	#sudo perf top
}

BASHRC_VER='1.22';
[ -f /usr/bin/git ] && GIT_USER=`git config --get user.name` || GIT_USER="???";
if [[ -f /usr/lib64/openmpi/bin/orted ]]; then
	module purge; module load mpi/openmpi-x86_64;
fi
if [[ $- != *i* ]]; then
	# bashrc is for aliases, functions, and shell configuration intended for use in
	# interactive shells.  However, in some circumstances, bash sources bashrc even
	# in non-interactive shells (e.g., when using scp), so it is standard practice
	# to check for interactivity at the top of .bashrc and return immediately if
	# the shell is not interactive.  The following line does that; don't remove it!
	cur_branch="";
else
	echo "Hello ${USER}@`hostname`, git[$GIT_USER/$cur_branch], ver=$BASHRC_VER[$0]";
	DISABLE_LINE_WRAP="echo -ne '\e[?7l'"
	[ ! -f ~/.vimrc ] && echo "set number" >> ~/.vimrc;
	lscpu | grep -e Architect -e On --color=never; grep MemTotal /proc/meminfo --color=never;
fi

if [[ $IS_LOCAL == "Y" ]]; then
	# ------------------------------------ GIT STUFF ---------------------------
	function git_note {
		NIGHTLY_NOTE="Passed nightly";
		NIGHTLY_REF=verified
		HAVE_DI_NOTE="Have DI";
		HAVE_DI_REF=have_di
		if [ $# -eq 0 ]; then
			echo "example: show/ nightly-push / di-report"
		elif [[ $1 == night* ]]; then
			git log -1 --oneline;
			read -p "${NIGHTLY_NOTE}? " -n 1 -r; echo    # (optional) move to a new line
			if [[ $REPLY =~ ^[Yy]$ ]]; then
				echo "---------- Pushing nightly note, updating origin/stable ---------"
				cmd="git notes --ref ${NIGHTLY_REF} add -m \"${NIGHTLY_NOTE}\" HEAD"; echo $cmd; eval $cmd;
				cmd="git push origin refs/notes/${NIGHTLY_REF}"; echo $cmd; eval $cmd;
				git push origin HEAD:stable;
				git fetch origin --prune --tags;
			else
				echo_red "Aborted";
			fi
		elif [[ $1 == di* ]]; then
			git log -1 --oneline;
			read -p "${HAVE_DI_NOTE}? " -n 1 -r; echo    # (optional) move to a new line
			if [[ $REPLY =~ ^[Yy]$ ]]; then
				echo "---------- Pushing DI note ---------"
				cmd="git notes --ref ${HAVE_DI_REF} add -m \"${HAVE_DI_NOTE}\" HEAD"; echo $cmd; eval $cmd;
				cmd="git push origin refs/notes/${HAVE_DI_REF}"; echo $cmd; eval $cmd;
				git fetch origin --prune --tags;
			else
				echo_red "Aborted";
			fi
		elif [[ $1 == sh* ]]; then
			#git notes --ref ${NIGHTLY_REF} list;
			echo "---------- /ref/${NIGHTLY_REF} -> ${NIGHTLY_NOTE} ---------"
			git log origin/master --oneline --notes=${NIGHTLY_REF} | grep -B2 "${NIGHTLY_NOTE}" | awk -F '\n' 'ln ~ /^$/ { ln = "matched"; print $1 } $1 ~ /^--$/ { ln = "" }';
			echo "---------- /ref/${HAVE_DI_REF} -> ${HAVE_DI_NOTE} ---------"
			git log origin/master --oneline --notes=${HAVE_DI_REF} | grep -B2 "${HAVE_DI_NOTE}" | awk -F '\n' 'ln ~ /^$/ { ln = "matched"; print $1 } $1 ~ /^--$/ { ln = "" }';
		fi
	}

	function git_grep {
		clear;
		str="'$@'";
		cmd="git grep -l -E ${str}";			echo_title "Files"; eval ${cmd};
		cmd="git grep -n -E ${str}";			echo_title ${str};  eval ${cmd};
		cmd="git log --oneline | grep ${str}";	echo_title "git";   eval ${cmd};
	}

	function git_branch() {
		if [ "$1" == "fap" ]; then
			#read -n 1 k <&1
			git fetch --all --prune --tags
		elif [ "$1" == "switch" ]; then
			# Switch fast between workign branches, without the need to stash push and stash pop
			git_switch_env_header_line="GIT_SWITCH_ENV_HEADER_LINE"
			echo "old_branch=`git rev-parse --abbrev-ref HEAD`, new_branch=$1"
			git commit --all --allow-empty --message "$git_switch_env_header_line" || return -5
			git checkout $2 || { git reset HEAD^ ; return -5; }
			last_commit_subject="`git log -1 --pretty=format:%s`"
			echo "last_commit_subject=$last_commit_subject"
			if [ "$last_commit_subject" == "$git_switch_env_header_line" ] ; then
				git reset HEAD^
			fi
			return 0;
		elif [ "$1" == "squash" ]; then
			# Squash n top commits. Very usefull for code review
			VVV=$2
			git reset --hard HEAD~$VVV;
			git merge --squash HEAD@{1};
			git commit;
			return 0;
		elif [ "$1" == "log" ]; then
			git log --color --graph --pretty=format:'%C(yellow)%h%C(bold red)%d%Creset %s %Cgreen(%cr) %C(bold blue)<%an>%Creset' --abbrev-commit --date=relative --all;
			return 0;
		elif [ "$1" == "latest" ]; then
			cur_user=$2
			echo "------------ Latest branches of user: $cur_user ------------"
			git branch -r --list $cur_user* --sort='-committerdate:rfc2822' --format='|%(align:60,left)%(color:blue)%(refname:short)%(color:reset)%(end) %(color:bold dim yellow)%(objectname:short)%(color:reset) %(align:31,left)%(committerdate:rfc2822)%(end)| %(color:bold)%(contents:subject)%(color:reset)'
			return 0;
		elif [ "$1" == "delpref" ]; then
			# https://stackoverflow.com/questions/10555136/delete-multiple-remote-branches-in-git
			git branch -r | awk -F/ '/\/PREFIX/{print $2}'; # Dry run
			#git branch -r | awk -F/ '/\/PREFIX/{print $2}' | xargs -I {} git push origin :{}
			return 0;
		elif [ "$1" == "stats" ]; then
			author="DanielHsH|danielhsh"
			echo "Top commiters, num commmits:"
			NCOMMITS=`git shortlog --numbered --summary | grep -i -e danielhsh -e shmulyan | awk '{ sum += $1; } END { print sum; }'`
			echo_green "  ${NCOMMITS}  danielhsh (/DanielHsH/Daniel Herman Shmulyan)";
			git shortlog --numbered --summary | head -12 | grep -v DanielHsH;
			#git log --author="${author}" --pretty=tformat: --numstat
			echo "------------------- Author : ${author}"
			git log --author="${author}" --format=tformat: --numstat | awk '{for (i=1;i<=2;i++) sum[i]+=$i;}; END{print "Num Added Lines: " sum[1] ",    Num Removed Lines: " sum[2]}'
			echo "------------------- Author : ${author} latest commits"
			git log --author="${author}" --oneline --shortstat
			#git log danielhsh/$cur_branch...origin/master --pretty=format:"%h%x09%an%x09%ad%x09%s" --abbrev-commit
		elif [ $# -eq 0 ]; then
			clear;
		else
			echo_title "params";
			echo -e "\t<none> \t\tShow control panel"
			echo -e "\tfap \t\tFetch latest code"
			echo -e "\tswitch \t\tSwitch to another branch"
			echo -e "\tlog \t\tShow graphical structure of branches"
			echo -e "\tlatest  \tShow latest of specific user. Example: latest danielg"
			echo -e "\tdelpref \tBatch Delete branches by prefix"
			echo -e "\tstats \t\tShow stats for top commiters."
			echo -e "\tsquash \t\tSquash <n> top commits. Example: squash 10"
			echo_title "Git quick help";
			echo -e  "git checkout -b myMaster origin/master;  \t\t# Create local branch";
			echo -e  "git branch -d;  \t\t\t\t\t# Delete local branch";
			echo -e  "git add -u;  \t\t\t\t\t\t# Add changes to local changes";
			echo -e  "git commit -av;  \t\t\t\t\t# Commit local changes to local branch";
			echo -e  "git stash; git stash pop; git stash drop;   \t\t# Stash local changes";
			echo -e  "git push -f danielhsh HEAD:$cur_branch \t\t\t# Push to my remote";
			echo -e  "git rebase origin/2.6 \t\t\t\t\t# Rebase on branch";
			echo -e  "git difftool origin/master -- clnt/nvmeibc_block.c; \t# Diff on file"
			echo -e  "git format-patch origin/master;  \t\t\t# Export commits as patches"
			echo -e  "git reflog; \t\t\t\t\t\t# Find commits that were deleted"
			echo -e  "git show a2c25061; git show v1.0;\t\t\t# Find a specific commit by hash or tag"
			echo -e  "git blame clnt/nvmeibc_block.c; \t\t\t# Find author of line"
			echo -e  "git rebase -i HEAD~10; \t\t\t\t\t# Interactive rebase"
			echo -e  'git log -L "/nvmeibc_block_cont(/,/^}/:clnt/my_file.c\t# History of a function';
			echo -e  'git log -L "899,1018:toma/nvmeibt_topology.c"\t\t# History of lines';
			echo -e  'git tag -a v0.04 -m "my version 4"; git push --tags; git ls-remote --tags danielhsh';
			echo -e  "git config --global --edit \t\t\t\t# edit config";
			echo -e  "git diff-tree --no-commit-id --name-only -r <id>\t\t# Files of commit";
			echo_title "GitLab help";
			echo -e  'Merge: https://gitlab-master.nvidia.com/excelero/users/danielhe/nvmesh/-/merge_requests/new?merge_request%5Bsource_branch%5D=master'
			echo "./tools/git_compare_branches.sh -since '2020-03-03' $cur_branch origin/master -direction t";
			return -5
		fi
		echo -e "+--------- \e[1;31m change \e[0;39m ------------------------------------------------------------------------+"
		git status -s -uno;	#git status -s;
		echo -e "+--------- \e[1;31m Untracked \e[0;39m ------- use:   git clean -fd --dry-run   ----------------------------+"
		git ls-files --others --exclude-standard;
		echo -e "+--------- \e[1;31m locals \e[0;39m ------------------------------------------------------------------------+"
		#git branch --set-origin-to=$GIT_USER/#ci_2master
		_GIT_FMT_CMT="%(align:60,left)%(color:blue)%(refname:short)%(color:reset)%(end) %(color:bold dim yellow)%(objectname:short)%(color:reset) %(align:26,left)";
		_GIT_FMT_SBJ="%(color:bold)%(contents:subject)%(color:reset)";
		git branch --format="|%(align:21,left)%(if)%(HEAD)%(then)* %(color:green)%(else)  %(color:blue)%(end)%(refname:short)%(color:reset)%(end)%(align:45,left)[%(color:blue)%(upstream:short)%(color:reset):%(upstream:track,nobracket)]%(end) %(color:bold dim yellow)%(objectname:short)%(color:reset) %(align:26,left)%(committerdate:local)%(end)| ${_GIT_FMT_SBJ}";
		echo -e "+--------- \e[1;31m remote \e[0;39m ------------------------------------------------------------------------+"
		git branch -r --list $GIT_USER* --format="|${_GIT_FMT_CMT}%(committerdate:local)%(end)| ${_GIT_FMT_SBJ}";
		echo -e "+--------- \e[1;31m Tags \e[0;39m --------------------------------------------------------------------------+"
		git tag --sort=-v:refname --list v2.5.2 v2.[79]* v3* v0.[01][0-9] --format="|${_GIT_FMT_CMT}%(end)| ${_GIT_FMT_SBJ}";
		echo -e "+--------- \e[1;31m others \e[0;39m ------------------------------------------------------------------------+"
		git for-each-ref --sort='-committerdate:rfc2822' --format="|${_GIT_FMT_CMT}%(committerdate:relative)%(end)| ${_GIT_FMT_SBJ}" refs/remotes/"$1" --count 20;
		echo -e "+--------- \e[1;31m Cur Branch \e[0;39m --------------------------------------------------------------------+"
		git log -10 --oneline;
		echo -e "+-------------------------------------------------------------------------------------------+"
		# echo "Last commit where 2.2.1 split from master: `git merge-base origin/master origin/2.2.1`"
		# clear; git ls-files *.c* *.h* *.json .sh ./unitest/run Makefile | xargs wc -l | grep total
	}

	# ------------------------------------ auto completion ---------------------------
	__auto_comp_blk_unitest() {
		KWDS=$(./blk_unitest --help 2>&1 | grep -Po '(\s+\[)\K(-.*?)(?=\s)' | paste -sd ' ')
		COMPREPLY=($(compgen -W "${KWDS}" "\\${COMP_WORDS[-1]}"))
	}
	complete -F __auto_comp_blk_unitest blk_unitest
	#__auto_comp_makefile_targets() { local makefile_targets; makefile_targets=$(make -qp | awk -F':' '/^[a-zA-Z0-9][^$#.\/\t=]*:([^=]|$)/ {split($1,A,/ /); for(i in A)print A[i]}' | grep -v '^\.PHONY'); COMPREPLY=($(compgen -W "${makefile_targets}" -- "${COMP_WORDS[COMP_CWORD]}")); } complete -F __auto_comp_makefile_targets make
	complete -W "\`make -qp | awk -F':' '/^[a-zA-Z0-9][^$#.\/\t=]*:([^=]|$)/ {split(\$1,A,/ /); for(i in A)print A[i]}'\`" make

	# ------------------------------------ LOGS & stat ---------------------------
	function LOGS__collect() {
		echo "todo"
		# Usage: ./copy_jenkins_logs.sh --execution_id <id> --specific_run_id <specific run id> --folder <bug_folder_name> --source <nvme196> --destionation <server2>
		# Example: ./copy_jenkins_logs.sh -e eae89046-1815-11ea-92c2-0cc47ab2a17f_0 -i eae89046-1815-11ea-92c2-0cc47ab2a17f_0_1 -f EC-1000 -s n196 -d server2
		# echo "Assemble QA logs:  ./logs/logs_collector.sh -p ${MY_PROJECTS_DIR}/$cur_branch -f bugn2677 -s n149 n148 n150"
	}
	# ------------------------------------ NVMESH stuff ---------------------------
	function NVMESH_build() {
		clear;
		NVMESH_simu clean;
		if [[ $1 == c* ]]; then
			./build.sh compileonly
		elif [[ $1 == m* ]]; then
			./build.sh machinesStrong2
		elif [ "$1" == "l" ]; then
			rmv_dir="${MY_PROJECTS_DIR}/$cur_branch/toma"
			echo -e "-------\e[34m cleaning toma dir on machines: $2, branch: $rmv_dir \e[39m--------"
			nodes=
			for i in "$2"; do
				nodes="$nodes -H $i"
			done
			cluster="$nodes -Pv"
			${PSSH_UTIL} $cluster "rm -rf $rmv_dir/*; ls $rmv_dir"
			rmv_rpms="${MY_PROJECTS_DIR}/$cur_branch/nvmesh-core*"
			echo -e "-------\e[34m ${rmv_rpms}.rpms removing from $cluster \e[39m--------"
			${PSSH_UTIL} $cluster "rm -f $rmv_rpms"
		elif [[ $1 == s* ]]; then
			./build.sh simulator
		elif [[ $1 == g* ]]; then
			./build.sh guest
		elif [[ $1 == um* ]]; then
			um_node="nvme224";
			echo_title "rsync latest nvmesh.kernel";
			cmd="cd ../nvmesh"; echo $cmd; eval $cmd;
			cmd="rsync --compress --cvs-exclude --include=core --ignore-errors --exclude-from=excludes -rlpgoDz --checksum . -e ssh ${um_node}:projects/nvmeshum/nvmesh.kernel"; echo $cmd; eval $cmd;
			echo_title "rsync latest um_app";
			cmd="cd ../nvmeshum/app"; echo $cmd; eval $cmd;
			cmd="rsync --compress --cvs-exclude --include=core --ignore-errors -rlpgoDz --checksum . -e ssh ${um_node}:~/projects/nvmeshum/app"; echo $cmd; eval $cmd;
			cd $cur_dir;
			echo_title "compile um_app";
			ssh ${um_node} "cd ~/projects/nvmeshum; UM_BUILD_TYPE=user UM_BUILD_DEBUG=true PKG=false ./build.sh";
			echo_title "compile um_simu";
			ssh ${um_node} "cd ~/projects/nvmeshum; UM_BUILD_TYPE=sim  UM_BUILD_DEBUG=true PKG=false ./build.sh";
		else
			echo "compile / m - rpm on machines / simulator / l n111 - clean toma dir + rpms / guest / um";
		fi
	}

	function NVMESH_reinstall_mgmt() {
		if [ $# -eq 0 ]; then
			MGMT=n34
		else
			MGMT=$1
		fi
		echo_title "nvmesh-mgmt-reinstalling on $MGMT"
		cd ${MY_PROJECTS_DIR}/management;
		git fetch -p --all;
		git rebase;
		cd -;
		cd ${MY_PROJECTS_DIR}/;
		#ssh $MGMT rm -rf projects/management;
		#scp -r management $MGMT:projects;		# Daniel can use scp -r, it is slower than rsync
		rsync -hvari --size-only --delete --modify-window=10000000 management $MGMT:projects/
		cd -;
		ssh $MGMT "NVMESH_service management reinstall";
	}

	function NVMESH_reinstall_core() {
		if [ $# -eq 0 ]; then
			SERVERS_LIST="$MY_DEV_SERVER";
		else
			SERVERS_LIST="$@";
		fi
		nodes=
		for i in $SERVERS_LIST; do
			nodes="$nodes -H $i"
		done
		cluster="$nodes -Pv"
		echo_title "nvmesh-core-removing from $cluster"
		${PSSH_UTIL} $cluster "NVMESH_service all stop"
		${PSSH_UTIL} $cluster sudo yum remove -y nvmesh-core.x86_64;
		${PSSH_UTIL} $cluster "echo RPM: `rpm -qa | grep nvmesh`"
		${PSSH_UTIL} $cluster "echo LSMOD: `sudo lsmod | grep nvmeib`"
		${PSSH_UTIL} $cluster "sudo depmod -a; sudo modprobe -r nvmeibc";
		echo_title "nvmesh-core-removed, installing $cur_branch"
		${PSSH_UTIL} $cluster "sudo rm -rf /${NVMESH_DIR_LOG}/toma*"
		#${PSSH_UTIL} $cluster "sudo yum install -y ${MY_PROJECTS_DIR}/$cur_branch/nvmesh-utils*.x86_64.rpm";
		#${PSSH_UTIL} $cluster "sudo yum install -y ${MY_PROJECTS_DIR}/$cur_branch/nvmesh-core*.x86_64.rpm";
		#${PSSH_UTIL} $cluster "cd ${MY_PROJECTS_DIR}/$cur_branch/; pwd; "'LST_RPM=$(ls -t nvmesh-core*.x86_64.rpm | head -1);'" echo Using: \$LST_RPM; sudo yum install -y \$LST_RPM"
		${PSSH_UTIL} $cluster "cd ${MY_PROJECTS_DIR}/$cur_branch/; pwd;   LST_RPM=\`ls -t nvmesh-core*.x86_64.rpm | head -1\` ; echo Using: \$LST_RPM; sudo yum install -y \$LST_RPM"
		${PSSH_UTIL} $cluster "echo RPM: `rpm -qa | grep nvmesh`"
		echo_title "generating conf file"
		${PSSH_UTIL} $cluster "NVMESH_generate_conf_file";
		echo_title "nvmesh-core-installed"
		${PSSH_UTIL} $cluster "NVMESH_service all restart";
	}
	function NVMESH__copy_bashrc_to_machines() {
		echo -e "-------\tCopying to local repo";
		cmd="cp -f $THIS_FILE ${MY_PROJECTS_DIR}/nvmesh/tools/block_team_bashrc.sh";
		echo $cmd; eval $cmd;
		cmd="cp -f $THIS_FILE ${MY_PROJECTS_DIR}/${NVROCKS_PREFIX}s/11scripts/block_team_bashrc.sh";
		echo $cmd; eval $cmd;
		if [ $# -eq 0 ]; then
			SERVERS_LIST="$MY_DEV_SERVER";
		else
			SERVERS_LIST="$@";
		fi
		echo -e "-------\tCopying to $SERVERS_LIST";
		for machine in $SERVERS_LIST; do
			echo "copy $THIS_FILE -> ${machine}:~/.bashrc"; scp $THIS_FILE "${machine}":~/.bashrc;
			#echo "copy /etc/hosts -> ${machine}"; scp /etc/hosts "${machine}:~/hosts"; ssh ${machine} "sudo mv ~/hosts /etc/hosts"
		done
	}

	function NVMESH_restart_machine() {
		# for n34-n39, use 'ping nvme34-ilo -c 1 | grep icmp' to see the ipmi ip. Like: 10.193.6+[34..39]
		# If machine rebooted and you dont have ping: login via brawser to with credentials below and open: remote control->iKVM, login with root and find interface with ifconfig. do ifdown ens6f0, ifup ens6f0
		if false; then
			[ "$1" == "r" ] && arg=r || arg=u;
			cmd="ipmiutil power -$arg -U ADMIN -P ADMIN -N 10.0.2.111"; echo $cmd; eval $cmd;
		else
			if [ $# -eq 0 ]; then
				SERVERS_LIST="$MY_DEV_SERVER";
			else
				SERVERS_LIST="$@";
			fi
			cluster=
			for i in $MY_DEV_SERVER; do
				cluster="$nodes -H $i"
			done
			cluster="$nodes"
			params="-Pv -t 5";
			echo_title "restarting cluster |$cluster| params |$params|"
			read -p "Are you sure? " -n 1 -r
			echo    # (optional) move to a new line
			if [[ $REPLY =~ ^[Yy]$ ]]; then
				echo_green "Restarting";
				${PSSH_UTIL} $cluster $params "NVMESH_restart_machine"
			else
				echo_red "Aborted";
			fi
		fi
	}

	function NVMESH_log_refactor() {
		OUT_FILE=z_log_refactor.txt
		if [ $# -eq 0 ]; then
			echo "Params: check/export"
			echo "   check  - find which traces have wrong format and save them to $OUT_FILE / export all error codes"
			echo "   export - all error codes. Verify manually they are unique and save them into user manual"
		fi
		if [ "$1" == "check" ]; then
			echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Invisible to dmesg, require fixing  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" > $OUT_FILE
			git grep -rE "_N?[IWE]h?\(" | grep -e 'atom/' -e 'block/' -e 'core/' -e 'main/' -e 'module/' -e 'va_block/' -e 'block\.[ch]' -e 'volume\.[ch]' -e 'cc_api\.[ch]' -e 'targets\.[ch]' -e 'c_main\.[ch]' -e 'pausable\.[ch]' | grep -v -e unitest -e DMESG_PREFIX -e QA_BLOCK_PREFIX -e MAIN_IOCTL_PREFIX -e DMESG_PD_PREFIX -e A_DMESG_PREFIX -e DMESG_MOD_PREFIX -e 'atom/' -e dp_dbg_tools -e nvmeibc_core_ibdev -e nvmeibc_core_common  >> $OUT_FILE
			echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Visible direct to dmesg, require fixing  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" >> $OUT_FILE
			git grep -e pr_info -e pr_notice -e pr_warning -e pr_err -e pr_crit -e pr_alert -e pr_emerg | grep -v -e 'block/unitest' -e '^testing/' -e '^toma/' -e '^kernels/' -e '^mlnx_ofed_' -e 'nvmeiba_infra' -e 'Linux-pq' -e 'exlog' -e subpr_info -e dp_dbg_tools -e nvmeib_trace_stress_test -e "\. Error code: [01]">> $OUT_FILE
			echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Visible old logging, require fixing  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" >> $OUT_FILE
			git grep -e '_[IWE]\(' | grep -v -e 'testing' -e 'toma/' -e nvmeib_utils_bin_traces -e 'block/unitest' -e nvmeib_kth_events -e JAM_FMT >> $OUT_FILE
			echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Visible new logging, require fixing  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" >> $OUT_FILE
			git grep -e 'N.*_dmesg' -e 'N.*_to_user' | grep -v -e dp_dbg_tools -e unitest -e nvmeib_utils_bin_traces -e kernel_to_user | grep -v "\. Error code: [01]"  >> $OUT_FILE
			echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Visible Toma logging, require fixing  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" >> $OUT_FILE
			git grep -rE -e "_[CEW]Tf\(" -e "_IMf\(" toma/ | grep -v "\. Error code: [01]" >> $OUT_FILE
			echo "~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~  Formatted OK  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~" >> $OUT_FILE
			git grep -e 'N.*_dmesg' -e 'N.*_to_user' | grep -v -e dp_dbg_tools -e unitest -e nvmeib_utils_bin_traces -e kernel_to_user | grep -e "\. Error code: [01]"  >> $OUT_FILE
			less -N $OUT_FILE
		elif [ "$1" == "export" ]; then
			git grep  "\. Error code: [01]" | grep 'Error code: [0-9]*' -o | sort
		fi
	}

	function NVMESH_crash() {
		if [ "$1" == "toma" ]; then
			#tar -czhvf ttt.tar `ldd ${NVMESH_DIR_SRC}/target-repo/target*/toma/bin/release/nvmeibt_toma | grep -E ' => ' | grep -v 'not found' | cut -d' ' -f3`
			#set solib-absolute-prefix ./lib64; set solib-search-path ./lib64
			#gpg -d -z6 --batch --pinentry-mode default --passphrase AlexanderRonen > toma.tar < nvmeibt_toma_src_tar1.pgp; tar -xpvf toma.tar ./toma;
			gdb ./../toma/bin/release/nvmeibt_toma.with_symbols "ls toma_core.* | tail -1"
		else
			echo "params: clnt/toma"
		fi
	}

	function NVMESH_clean_local() {
		cd ${MY_PROJECTS_DIR}/$cur_branch/;
		echo -n "Size before = ";  du -hcs . | grep total;
		NVMESH_simu clean;
		echo "********* Cleaning *.[d|i|o|gch|log] ***************"
		find . -type f -name "*.[dio]" -exec rm -f {} \;
		find . -type f -name "*.gch" -exec rm -f {} \;
		find . -type f -name "*\.log" -exec rm -f {} \;
		find . -type f -name "*binlog*" -exec rm -f {} \;
		echo "********* Cleaning bunitest ***************"
		cd ./clnt/block/unitest
		find . -type f -name "*.trace.json" -exec rm -f {} \;
		find . -type f -name "dict*json"    -exec rm -f {} \;
		find . -type f -name "gen*events*"  -exec rm -f {} \;
		cd -;
		git ls-files --others --exclude-standard
		if [ ! $# -eq 0 ]; then
			echo "********* Cleaning git untracked ***************"
			read -p "Do you wish to clean git untracked files?" yn
			case $yn in
				[Yy]* ) git clean -fd; break;;
				[Nn]* ) echo "untracked files remained"; break;;
				* ) echo "Please answer yes or no";;
			esac
			echo -n "Size after = ";  du -hcs . | grep total;
			ncdu
		fi
	}

	function NVMESH_simu() {
			local TOMA_UNITEST="${MY_PROJECTS_DIR}/nvmesh/toma/unitest/";
		if [[ $1 == clnt* ]]; then
			cd ${MY_PROJECTS_DIR}/$cur_branch/clnt/block/unitest/
			cmd='make clean_prev_run; ./build_block_testing.sh build; ./run_block_unitest.sh -n -d 0 -nRep 2 -hsync'
			echo $cmd; eval $cmd;
		elif [[ $1 == toma* ]]; then
			cd ${TOMA_UNITEST}; make all -j; ./nvmeibt_toma
		elif [[ $1 == clean* ]]; then
			local cur_dir=$PWD;
			echo "cleaning large simulator file"
			cd toma/unitest/;      make clean; cd ../..;
			cd clnt/block/unitest; make clean;
			cd ../um_integration;  make clean;
			cd $cur_dir;
		else
			echo -e "*\t\t\tparams to run: < clnt / toma / clean >"
			echo_green "---------------------- BLOCK SIMULATOR EXAMPLES --------------------";
			echo "make clean_prev_run; make all -j --output-sync=recurse USE_RELEASE=0 USE_SANITIZERS=1; ./blk_unitest -tracedbg 4 -nRep 1 -hsync -ECAllPerm >longdmesg.txt 2>&1";
			echo -e "\t Can use: 2>&1 | tee longdmesg.txt";
			echo -e "view binary traces:\t ./pager --color --dict_preload 99bin/*/dict.5.json --fmtlib_preload 99bin/*/libfmtrs.so > longterm.txt; less -R longterm.txt";
			echo -e "build in release  :\t ./build_block_testing.sh rebuild USE_RELEASE=1";
			echo -e "Help / valgrind   :\t ./blk_unitest --help";
			echo_green "---------------------- Toma SIMULATOR EXAMPLES --------------------";
			echo "cd ${TOMA_UNITEST};     make clean;    make all -j;   ./nvmeibt_toma";
			echo -e "\t./_root/${NVMESH_DIR_LOG}/trace_daemon/pager _root/${NVMESH_DIR_LOG}/trace_daemon --toma --color"
			echo_green "---------------------- Utils examples --------------------"
			echo -e "cd ${MY_PROJECTS_DIR}/perfTest/io_stress/cmp_blocks; examples/run_test.sh;"
		fi
	}

	# ------------------------------------ LOCAL APPS  ---------------------------
	function APP_launch() {
		cmd="";
		if [[ $1 == [Bb]ackup* ]]; then
			DST="~/Documents/LocalGDrive/99\ Backups/Linux/Nvidia";
			clear; echo_title  "Copying to gDrive";
			cmd="rsync $THIS_FILE ${DST}/home/zshrc"; echo $cmd; eval $cmd;
			cmd="rsync  ~/.gitconfig ${DST}/home/.gitconfig"; echo $cmd; eval $cmd;
			cmd="rsync  ~/.config/Code/User/*.json ${DST}/home/_config/Code_User/"; echo $cmd; eval $cmd;
			cmd="rsync  ~/.config/git/* ${DST}/home/_config/git/"; echo $cmd; eval $cmd;
			cmd="rsync  ~/.ssh/* ${DST}/home/_ssh/"; echo $cmd; eval $cmd;
			cmd="rsync /etc/hosts ${DST}/etc/hosts"; echo $cmd; eval $cmd;
			cmd="rsync /etc/resolv.conf* ${DST}/etc/"; echo $cmd; eval $cmd;
			#cmd="sudo rsync /etc/netplan/* ${DST}/etc/netplan/"; echo $cmd; eval $cmd;
			echo_title "Syncing gDrive to cloud";
			cmd="cd ~/Documents; rclone sync ./LocalGDrive Gdrive: --verbose --drive-acknowledge-abuse; #sync -> check;"
		elif [[ $1 == vs* ]]; then	# vscode
			cmd="code . --remote wsl+Ubuntu";
		elif [[ $1 == sle* ]]; then	# sleep, suspend
			# find /sys/power/ -type f | xargs tail -n +1
			sync; sync;
			cat /sys/power/mem_sleep;
			sudo systemctl suspend;
			#sudo bash -c 'echo mem > /sys/power/state';
		elif [[ $1 == cor* ]]; then	# Set core path
			ulimit -c unlimited;
			core_pat_file="/proc/sys/kernel/core_pattern";
			prev_cor_pat=`cat ${core_pat_file}`;
			sudo bash -c "echo 'core.%e.%p' > ${core_pat_file}";
			new_cor_pat=`cat ${core_pat_file}`;
			echo "$prev_cor_pat --> $new_cor_pat"
			cmd="ulimit -c";
		elif [[ $1 == nvi* ]]; then
			nvinit ssh -user danielhe;		# nvssh
		elif [[ $1 == ping* ]]; then
			cmd="ping 8.8.8.8 -c 2";
		elif [[ $1 == find* ]]; then
			cmd="xprop _NET_WM_PID | sed 's/_NET_WM_PID(CARDINAL) = //'"; cmd=$cmd'| ps `cat`';
		elif [[ $1 == boot* ]]; then
			# sudo lshw -c video; #sudo ip link set enxb445063339ce up
			cmd='xrandr --output eDP-1 -s 2560x1600 --rate 59.99 --scale 1 --output DP-1 -s 3840x2160+2560+0'; # --right-of eDP-1;
			echo ${cmd};
			TOUCH_SCREEN_DEV_ID=`xinput list | grep -e ouchscreen -e ELAN2097 | grep -oP 'id=\d+' | grep -oP '\d+'`;
			cmd="sudo xinput disable ${TOUCH_SCREEN_DEV_ID}";
			echo ${cmd}; eval ${cmd};
			APP_launch core;
			cmd="konsole --tabs-from-file ~/Documents/LocalGDrive/99\ Backups/Linux/kconsole_tabs.txt &"; echo $cmd; eval $cmd;
			cmd="/usr/bin/google-chrome-stable &";
		else
			echo "params: Backup /core / ping / find (proc by window) / boot / nvinit";
			echo "Disable Line Wrap: $DISABLE_LINE_WRAP";
		fi
		echo $cmd; eval $cmd;
	}

	function APP_killstuck() {
		if [ $# -eq 0 ]; then
			echo "input: vlc / wine / pulse / slick / foxit / audio"
			return 0;
		elif [[ $1 == *vlc* ]]; then
			PROG_NAME="vlc";			ps_prg=`pgrep ${PROG_NAME}`;	[ -z "$ps_prg" ] && echo "Nothing to kill" || sudo kill -9 ${ps_prg};
		elif [[ $1 == fox* ]]; then
			PROG_NAME="FoxitReader";	ps_prg=`pgrep ${PROG_NAME}`;	[ -z "$ps_prg" ] && echo "Nothing to kill" || sudo kill -9 ${ps_prg};
		elif [[ $1 == pulse* ]]; then
			PROG_NAME="pulseUI";		ps_prg=`pgrep ${PROG_NAME}`;	[ -z "$ps_prg" ] && echo "Nothing to kill" || sudo kill -9 ${ps_prg};
		elif [[ $1 == audio* ]]; then
			sudo systemctl stop bluetooth;
			PROG_NAME="pulseaudio";		ps_prg=`pgrep ${PROG_NAME}`;	[ -z "$ps_prg" ] && echo "Nothing to kill" || sudo kill -9 ${ps_prg};
			sudo systemctl start bluetooth;
		elif [[ $1 == chrome* ]]; then
			PROG_NAME="chrome";		ps_prg=`pgrep ${PROG_NAME}`;	[ -z "$ps_prg" ] && echo "Nothing to kill" || sudo pkill chrome;
		elif [[ $1 == disp* ]]; then
			sudo systemctl restart gdm; #cat /etc/X11/default-display-manager;
		else
			echo_red "Unrecognized program $1";
			PROG_NAME="$1";	ps_prg="???";
		fi
		echo "-----Done with ${PROG_NAME} ${ps_prg} --------"
	}

	function APP_network() {
		clear; dns_addr=`cat /etc/resolv.conf | grep -v '^#'`; echo_title "DNS"; echo "$dns_addr"; ll /etc/resolv.*;
		echo_title "/etc/hosts"; cat /etc/hosts | grep -v -e ^\# -e ^127 -e ^f[fe] -e ^::;
		echo_title "Net stats"; netstat -r; echo -e "\t\t----- arp"; arp -a;
		echo_title "Self Testing"; resolvectl status;
		MTL="lab.nvidia.com"; NVC="nvidia.com";
		resolvectl query nvmeserver2.${MTL} www.google.com geovpn.mellanox.com confluence.${NVC} nvme34-ilo.${MTL} nvme34.${MTL} mtv-excelero1.${MTL} gitlab-excelero.mtv.labs.mlnx gitlab-master.${NVC} server2 n34;
		echo "date; speedtest-cli | grep -e Upload -e Download";
	}

	function APP_help() {
		clear -x;
		if [ -f $THIS_FILE ]; then
			source $THIS_FILE;
		else
			echo "Unable to source $THIS_FILE ! Retry manually"
		fi
		echo_green "code $THIS_FILE -r";
		# echo_title "Aliasess"; # alias | pr -1Tn
		echo "typeset -f <func_name>";
		typeset -f | grep -E ' \(\)' | grep -e APP -e ^git -e NVMESH -e NVROCKS
		echo_title "Rsync";
		RSYNC="rsync -hvari --size-only --dry-run --delete --modify-window=1000000000";
		ZZOUT="grep -v -e '.DS_Store' -e 'desktop.ini' zz_out.txt | grep '^[^\.]'";
		echo "cd ~/Documents; $RSYNC './LocalGDrive/' '/media/${USER}/Seagate Expansion Drive/01 DanielDocs/Google Drive/' > zz_out.txt; $ZZOUT"
		#echo "vim ~/.config/rclone/rclone.conf"
		# echo "---------------------- MAC --------------------"
		# echo "Search   : Apps alt+{F1,F2,F3, F5}, in aps alt+F"
		# echo "Spectacle: Ctrl+Shift+ {qawsdfc3,<-,->}"
		#echo_title "Ubunto";
		#echo "Heb/Eng  : alt+space,        Alt + F2: Run Cmd";
		#echo "Workspaces: Win+A: Show all, Win+Pgup/Pgdn cycle, Win+Shift+Pgup/Pgdn move app to workspace"
		#echo "Spectacle: Win+ <-,->, Win+Shift+<-,->, Alt+F7/F8: Window Move/Resize, Hold Shift"
		#echo "Terminal: Ctrl+Shit+  {T-newTab, .-CopyInput /-StopCopy}"
		echo_title "Find";
		echo "find . -type f -regex '.*/proc[^/]*$'"
		echo "find . -name \"core.[0123456789]*\" # -type f -delete"
		echo -e "  \e[0;31mRed\e[0;39m  \e[0;32mgreen\e[0;39m  \e[16;34mBlue\e[0;39m  \e[1;31mBold\e[0;39m  \e[2;31mDark\e[0;39m  \e[4mUnd\e[0;39m  \e[5mBlink\e[0m  "
		echo "LineWrap off: " 'echo -ne "\x1b[?7l"' " LineWrap on: " 'echo -ne "\x1b[?7h"';
		echo_title "GOODIES";
		echo "* DEV: $MY_DEV_SERVER";
		#eval ${DISABLE_LINE_WRAP};
		wsl.exe -l -v; echo "wsl.exe --shutdown";
		if [[ $1 == na* ]]; then
			export PS1='\[\e]0;'$2'\a\]\[\033[01;32m\]\u@\h\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]\$';
		fi
	}

# ------------------------------------------------------
# ------------------------------------------------------ REMOTE MACHINE
# ------------------------------------------------------
else
	function APP_launch() {
		n_args=$#
		if [[ $1 == cor* ]]; then	# Set core path
			ulimit -c unlimited;
			core_pat_file="/proc/sys/kernel/core_pattern";
			prev_cor_pat=`cat ${core_pat_file}`;
			sudo bash -c "echo 'core.%e.%p' > ${core_pat_file}";
			new_cor_pat=`cat ${core_pat_file}`;
			echo "$prev_cor_pat --> $new_cor_pat"
			cmd="ulimit -c";
		elif [[ $1 == cls* ]]; then
			echo -e "*\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n*";
			clear -x
		else
			echo "args: core / cls "
		fi
	}

	nvmesh_conf_path="${NVMESH_DIR_ETC}/nvmesh.conf";
	nvmesh_conf_modprobe_path="/etc/modprobe.d/nvmesh.conf";
	io_stress_dir="${NVMESH_DIR_SRC}/perfTest/io_stress/"				# Can use ${MY_PROJECTS_DIR}/$cur_branch/perfTest/io_stress

	function NVMESH_service() {
		NVMESH_CLIENT_MODULES="nvmeibc nvmeib_common nvmeib_common_siw_public nvmeib_common_mlx5_public nvmeib_common_mlx4_public nvmeib_common_public nvmeiba";
		# -------------------------- mongo
		if [[ $1 == mon* ]]; then
			if [ "$2" == "load" ]; then
				sudo mongo management ${NVMESH_DIR_SRC}/management/clearDB.js;
				mongorestore -d management ~/mgt_dump/management/
			elif [ "$2" == "save" ]; then
				mongodump -d management -o mgt_dump
			elif [ "$2" == "clear" ]; then
				sudo mongo management ${NVMESH_DIR_SRC}/management/clearDB.js;
				sudo mongo management --eval 'db.dropDatabase()';	 # without it mgmt upgrade might not work
				sudo mongo management ${NVMESH_DIR_SRC}/management/initDB.js;
			else
				echo "mongo load/save/clear"
			fi
		# --------------------------  Management
		elif [[ $1 == manag* ]]; then
			if [ "$2" == "restart" ]; then
				sudo service nvmeshmgr restart; sudo systemctl enable nvmeshmgr; # sudo chkconfig nvmeshmgr on;
			elif [ "$2" == "stop" ]; then
				sudo service nvmeshmgr stop;
			elif [[ $2 == rein* ]]; then
				sudo service nvmeshmgr stop;
				NVMESH_service mongo clear;
				sudo yum remove -y nvmesh-management;
				cd ${MY_PROJECTS_DIR}/management/;
				#rm -fr node_modules; npm install;
				cd RPM;
				rm nvmesh-management-*rpm;
				./buildrpm;
				sudo yum install -y nvmesh-management-*.rpm;
				NVMESH_service management restart
			elif [[ $2 == stat* ]]; then
				sudo service nvmeshmgr status;
			elif [[ $2 == kafk* ]]; then
				cd /opt/kafka/bin;
				./kafka-consumer-groups.sh --bootstrap-server localhost:9092 --describe --group managements-group;
				./kafka-console-consumer.sh --bootstrap-server localhost:9092 --topic zone1.management.priority.1.0.0 | grep updatePRaidReport | jq .;
				cd -;
			else
				echo "management stop/restart/reinstall/status/kafka"
			fi
		# --------------------------  mgmtAgent
		elif [[ $1 == mgmtAgent* ]]; then
			agent_pids=`ps -ef | grep -e Agent -e managementCM | grep python | awk '{print $2;}'`;
			echo -e "Killing mgmtAgent_pids=\033[0;31m$agent_pids\033[0m";
			sudo kill -9 `ps -ef | grep -e Agent -e managementCM | grep python | awk '{print $2;}'`; #"$agent_pids";
			sleep 0.1;
			echo "Starting mgmtAgent"
			sudo systemctl start nvmeshagent
			ps -ef | grep management;
		# --------------------------  clnt
		elif [[ $1 == c* ]]; then
			SERVICE_CMD="sudo service nvmeshclient";
			if [[ $3 == f* ]]; then				# Force
				SERVICE_CMD="sudo ${NVMESH_DIR_SRC}/client-repo/services/nvmeshclient";
			fi
			if [ "$2" == "start" ] || [ "$2" == "restart" ]; then
				SERVICE_CMD="$SERVICE_CMD $2"
				echo Running: $SERVICE_CMD; $SERVICE_CMD
				cat /proc/nvmeibc/status | grep 'module state';
			elif [ "$2" == "stop" ]; then
				SERVICE_CMD="$SERVICE_CMD $2"
				echo Running: $SERVICE_CMD; $SERVICE_CMD;
				cmd="modprobe -n -v nvmeibc"; echo $cmd; eval $cmd; # When nvmesh removes unisntalled should return error
				cmd="sudo depmod -a"; echo $cmd; eval $cmd;
				remains=`sudo lsmod | grep nvmeibc`;
				if [ "$remains" != "" ]; then
					echo_red "Clnt Stop failed ($SERVICE_CMD)";
					echo "$remains"
					echo_red "Trying force stop...";
					sudo rmmod ${NVMESH_CLIENT_MODULES};
					remains=`sudo lsmod | grep nvmeibc`;
				fi
				[ "$remains" != "" ] && echo_red "Clnt Stop failed" || echo_green "Clnt Stopped OK";
				sudo lsmod | grep nvmeib;
			elif [[ $2 == ignore* ]]; then
				#sudo bash -c 'echo -n "#V1|recov_set_num_sw=1:0,0" > /proc/nvmeibc/cli/cli';
				sudo bash -c 'echo -n "#*|ignore_toma_rcv=1" > /proc/nvmeibc/cli/cli';
				sudo bash -c 'echo -n "#*|topo_flush" > /proc/nvmeibc/cli/cli';
				echo Running: ignore_recoveries
				cat /proc/nvmeibc/volumes/*/status | grep -e 'itm=' -e 'short_id=';
			elif [ "$2" == "replace_ko" ]; then
				echo_title "replacing clnts ko";
				cd ./clnt;
				NEW_KO_PATH_ZIPPED=./nvmeibc.ko.xz
				NEW_KO_PATH=./nvmeibc.ko
				if [ -f "$NEW_KO_PATH" ]; then
					echo "Taking $NEW_KO_PATH"
				elif [ -f "$NEW_KO_PATH_ZIPPED" ]; then
					echo "Unzipping $NEW_KO_PATH_ZIPPED"
					unxz $NEW_KO_PATH_ZIPPED
				else
					echo_red "$NEW_KO_PATH does not exist, aborting";
					return 0;
				fi
				# Reinstall client ko only without reinstalling nvmesh. Run from your nvmesh directory
				if [[ -z `command -v ofed_info` ]]; then
					OFED_PATH="none";
				else
					OFED_PATH="`ofed_info -s | rev | cut -c 2- | rev`";
				fi
				echo "ofed_path $OFED_PATH"
				KO_PATH="client_${OFED_PATH}_`uname -r`";
				NVMESH_service all stop;
				DEST_DIR="${NVMESH_DIR_SRC}/client-repo/$KO_PATH/client/"
				DEST_DIR2="/lib/modules/`uname -r`/extra/"
				echo_yellow "replacing ko in {$DEST_DIR, not in$DEST_DIR2}";
				sudo cp ./nvmeibc.ko $DEST_DIR/.;
				#sudo cp ./nvmeibc.ko $DEST_DIR2/.;
				echo "Copying dictionaries: `ls ./dict*.json`"
				sudo cp ./dict*.json /${NVMESH_DIR_LOG}/trace_daemon/
				cd ..;
				modprobe -n -v nvmeibc;
				SERVICE_CMD="NVMESH_service all restart";
				$SERVICE_CMD
			else
				echo "HELP: $SERVICE_CMD start/stop/restart/replace_ko/ignore_recov     You enetered |$2|"
			fi
		# -------------------------- target/srvr
		elif [[ $1 == ta* ]] || [[ $1 == s* ]]; then
			if [[ $3 == f* ]]; then				# Force
				SERVICE_CMD="sudo ${NVMESH_DIR_SRC}/target-repo/services/nvmeshtarget"
			else
				SERVICE_CMD="sudo service nvmeshtarget"
			fi
			if [ "$2" == "start" ]; then
				SERVICE_CMD="$SERVICE_CMD $2"
				echo Running: $SERVICE_CMD
				$SERVICE_CMD
				#cat /proc/nvmeibs/status | grep 'module state';
			elif [ "$2" == "stop" ]; then
				SERVICE_CMD="$SERVICE_CMD $2"
				$SERVICE_CMD;
				remains=`sudo lsmod | grep nvmeibs`;
				if [ "$remains" != "" ]; then
					echo_red "Srvr Stop failed";
				else
					echo_green "Srvr Stopped OK";
				fi
			elif [ "$2" == "restart" ]; then
				SERVICE_CMD="$SERVICE_CMD $2"
				$SERVICE_CMD;
			elif [ "$2" == "discover_disks" ]; then
				sudo nvmesh_target set drive-discovery all-inclusive;
				sudo service nvmeshtarget restart
			elif [ "$2" == "force_restart" ]; then
				NVMESH_service toma stop;
				remains=`sudo lsmod | grep nvmeibs`;
				if [ "$remains" != "" ]; then
					NVMESH_service target stop;
				fi
				NVMESH_service target start;
			else
				echo "HELP: $SERVICE_CMD start/stop/restart/force_restart/discover_disks"
				return 0;
			fi
		# -------------------------- toma
		elif [[ $1 == to* ]]; then
			toma_pid=`pgrep toma`;
			if [ "$2" == "up" ]; then
				leader=`cat /${NVMESH_DIR_LOG}/toma_leader_name`;
				[[ "$toma_pid" != "" ]] && echo_green "TOMA is Up ($toma_pid)" || echo_red "TOMA is Down";
				echo "Last Known TomaLeader: $leader"
			elif [ "$2" == "start" ]; then
				NVMESH_service toma stop;
				TOMA_CMD="sudo service nvmeshtoma start";
				#TOMA_EXEC=`NVMESH_service toma find | grep release`; TOMA_CMD="sudo $TOMA_EXEC --sm-query-burst=32 --ipv4-only &"
				echo Running: $TOMA_CMD; $TOMA_CMD
				NVMESH_service toma up;
			elif [ "$2" == "stop" ]; then
				if [ "$toma_pid" != "" ]; then
					echo "Brutal kill Toma, by pid: $toma_pid"
					sudo kill -9 $toma_pid;
					#sudo pkill toma;	# this is orderly shut down with server, we want to kill only toma not server
				else
					echo "Nothing to do, Toma is not running"
				fi
				NVMESH_service toma up;
			elif [ "$2" == "restart" ]; then
				if false; then
					sudo ${NVMESH_DIR_SRC}/target-repo/services/nvmeshtarget restart-toma
				else
					sudo service nvmeshtoma restart
				fi;
				NVMESH_service toma up;
			elif [ "$2" == "find" ]; then
				sudo find ${NVMESH_DIR_SRC}/ -name nvmeibt_toma;
			elif [ "$2" == "rpc" ]; then
				cmd="sudo ${NVMESH_DIR_SRC}/common-repo/tools/toma_rpc ${@:3}";
				echo $cmd; eval $cmd;
			elif [ "$2" == "dump" ]; then
				[[ "$toma_pid" == "" ]] && { echo_red "TOMA is Down, cannot dump"; return 1; };
				local toma_dir="./z_last_toma_$(date +%Y_%m_%d_%H_%M_%S)";
				local toma_cfg_file="/${NVMESH_DIR_LOG}/toma_trace.config";
				local td_dir="/${NVMESH_DIR_LOG}/trace_daemon";
				clear;
				echo_title "Dumping toma to ${toma_dir}";
				mkdir -p $toma_dir;
				sudo kill -10 $toma_pid; sleep 1; toma_stat_file=`ls /${NVMESH_DIR_LOG}/*.stat -A1 | tail -n1`; echo "toma stat: $toma_stat_file";
				cp $toma_stat_file ${toma_dir};
				cp /proc/nvmeibs/toma_status/all ${toma_dir}/all;
				cp /proc/nvmeibs/*.csv ${toma_dir};
				systemctl status nvmeshtoma > ${toma_dir}/sysctl.txt;
				cp ${toma_cfg_file} ${toma_dir}; # See also # src/nvmesh/toma/toma_trace.config, src/nvmesh/toma/debugging_tips/hiccups;
				local stack_txt="${toma_dir}/stacks.txt";
				echo_title "Gathering stacks to ${stack_txt}";
				for pid in $(pgrep -x nvmeibt_toma); do
					echo "=== Process PID $pid ===" > ${stack_txt};
					for t in /proc/$pid/task/*; do
						local tid=$(basename "$t"); echo -e "\n--- TID $tid stack ---" >> ${stack_txt};
						cmd="sudo cat /proc/$pid/task/$tid/stack >> ${stack_txt}"; echo $cmd; eval $cmd;
						cmd="sudo pstack $tid >> ${stack_txt}"; echo $cmd; eval $cmd;
					done
				done
				tail -n +1 ${toma_dir}/* | less;
				cmd="\t -\t sudo ${td_dir}/pager.py ${td_dir} --toma --since tail-5m | grep -E \"TOMAerr|TOMAwarn\"";
				echo -e $cmd;
				echo -e "\t -\t $toma_stat_file";
				local toma_exe=`NVMESH_service toma find`;
				echo_title "Toma file:"; echo ${toma_exe};
				toma_rpc="${NVMESH_DIR_SRC}/common-repo/tools/toma_rpc";
				echo -e "Set Param example:\t ${toma_rpc} config max_n_simultaneous_dirty_rebuild 2";
				echo -e "\t\t\t ${toma_rpc} config tracer_debug_level 5";
				echo -e "\t\t\t ${toma_rpc} config raft_leader_heartbeat_timeout_usec 200000";
				echo -e "\t\t\t echo '+ all' > ${toma_cfg_file}";
				echo_title "Excluded drives";
				cat /etc/nvmesh/target_devices.conf | grep -v '^#';
				echo_title "Fix Toma persistency";
				echo "/opt/nvmesh/target-repo/target*/toma/scripts/toma_persistence_to_JSON.sh";
				local toma_trace_conf="/var/log/nvmesh/toma_trace.config";
				echo "cp ${toma_trace_conf} ${toma_trace_conf}.ORIG_`date +%d%b%Y_%H%M%S`";
				echo "echo '+ all' > ${toma_trace_conf}";
				echo_title "Kafka";
				cmd="dpkg -s librdkafka1  librdkafka-dev| grep -e Package -e Version"; echo $cmd; eval $cmd;
				cmd="rpm -ql librdkafka | grep librdkafka.so*"; echo "************** $cmd"; eval $cmd;
				cmd="ll /usr/lib*/librdkafka.so*"; echo "************** $cmd"; eval $cmd;
				cmd="ldd ${toma_exe} | grep kafka"; echo "************** $cmd"; eval $cmd;
			elif [[ $2 == recov* ]]; then
				watch -n 1 -d cat /proc/nvmeibs/toma_status/recover;
			elif [ "$2" == "del_csv" ]; then
				echo_title "deleting toma persistency";
				sudo rm -rf ${NVMESH_DIR_OPT}/toma/*;
			elif [[ $2 == com* ]]; then
				local tfile="./bin/release/nvmeibt_toma";
				local toma_exe=`NVMESH_service toma find`;
				echo_green "Compile toma:";
				echo "cd toma;";
				echo "make clean; rm ${tfile}; rm -rf obj/*; make -j --debug=verbose all MOD=release make AUTOGEN_DIR='../autogen' AUTOGEN_SUBDIRS_TOMA='common toma' NVMEIBC_SECTOR_SHIFT=12 GIT_COMMIT_ID=0xdddaaa55;  [ -f ${tfile} ] && echo_green "OK" || echo_red "Fail";";
				echo -e "\nsudo mv ${toma_exe} ${toma_exe}.back";
				echo "sudo cp ./trace/nvmeibt_toma/release/dict.*.json /${NVMESH_DIR_LOG}/trace_daemon/";
				echo "NVMESH_service toma stop; sudo cp bin/release/nvmeibt_toma ${toma_exe}; NVMESH_service toma restart; sleep 1s; cat /proc/nvmeibs/toma_status/raft | grep commit;";
			else
				echo "params: up / start / restart / dump / stop / del_csv / find / recoveries / compile / rpc"
			fi
		# -------------------------- all
		elif [[ $1 == a* ]]; then
			if [ "$2" == "start" ]; then
				NVMESH_service target start $3;		# Starting target starts also client
				cat /proc/nvmeib*/version;
				sudo systemctl disable nvmeshclient;
				sudo systemctl disable nvmeshtarget;
				NVMESH_debug_set _t;
				#sudo bash -c "echo 'options nvmeibc default_dir_lsblk=\"nvmesh\"' >> ${nvmesh_conf_modprobe_path}";
				#sudo bash -c "echo 'options nvmeibc debug_level=2' >> ${nvmesh_conf_modprobe_path}";
				#sudo bash -c "echo 'options nvmeibs debug_level=2' >> ${nvmesh_conf_modprobe_path}";
				echo -debug_level = `cat /sys/module/nvmeibc/parameters/debug_level`;
			elif [ "$2" == "stop" ]; then
				NVMESH_service client stop $3;		# Stopping client stops also target
				remains=`sudo lsmod | grep nvmeib`;
				if [ "$remains" != "" ]; then
					remains_no_nvmeia=`sudo lsmod | grep nvmeib | grep -v nvmeiba`;
					if [[ -z "$remains_no_nvmeia" ]]; then
						echo_yellow "All Stop - nvmeiba remained";
					else
						echo_red "All Stop failed";
						echo "Remaining: $remains";
						echo_red "Attempt to rmmod directly";
						sudo rmmod nvmeibs ${NVMESH_CLIENT_MODULES};
						remains=`sudo lsmod | grep nvmeib`; #
						echo "Remaining: $remains";
					fi
				else
					echo_green "All Stopped OK";
				fi
			elif [ "$2" == "show" ]; then
				clear;
				nvmesh cluster show;
				echo_title "Targets"; nvmesh target show -o tabular -l 0;
				echo_title "Clients"; nvmesh client show -o tabular -l 0;
				echo_title "Volumes"; nvmesh volume show -o tabular -l 0;
			elif [ "$2" == "restart" ]; then
				echo "params: $2 $3"
				NVMESH_service all stop $3;
				sudo dmesg --clear;
				sudo journalctl --vacuum-time=1s;
				sudo rm -rf /var/log/messages*;
				NVMESH_service all start $3;
			else
				echo "all start /stop / restart / show"
				return 0;
			fi
		# -------------------------- all
		elif [[ $1 == um* ]]; then
			echo "cd ~/projects/nvmeshum; rsync -r ../${cur_branch}/ nvmesh.kernel/";
			echo "UM_BUILD_TYPE=user UM_BUILD_DEBUG=true PKG=true ONLY_NVMESHUM=true ./build.sh";
			echo "UM_BUILD_TYPE=sim  UM_BUILD_DEBUG=true PKG=false ./build.sh";
		else
			echo "HELP: NVMESH_service clnt / target /toma / all / mongo / mgmtAgent / management / um";
			return 0;
		fi
	}

	function NVMESH_generate_conf_file() {
		echo "Generating file: $nvmesh_conf_path"
		if [ $# -eq 0 ]; then
			echo "restoring from conf file from .rpmsave"
			sudo cp $nvmesh_conf_path.rpmsave $nvmesh_conf_path;
			return 0;
		fi
		num_inst=4
		if [ "_$(ifconfig | grep ens7)" == "_" ]; then
			nic="\"enp25s0f0;enp25s0f1\""
			mgmt="n37"
		else
			nic="ens7"
			mgmt="n111"
		fi
		#sudo rm -f "$nvmesh_conf_path"
		str='"# NVMesh configuration file"';			sudo bash -c "echo ${str} > $nvmesh_conf_path"; echo $str
		str='MANAGEMENT_PROTOCOL=\"https\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='MANAGEMENT_SERVERS=\"'${mgmt}':4001\"';		sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='STATISTICS_SERVERS=\"'${mgmt}':4002\"';		sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='CONFIGURED_NICS=\"'${nic}'\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='MCS_LOGGING_LEVEL=\"DEBUG\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='MCS_LOGGING_VERBOSE_TYPES=\"''"CLIENT<>MGMT"''\"';	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='AGENT_LOGGING_LEVEL=\"DEBUG\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='DUMP_FTRACE_ON_OOPS=\"Yes\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='AUTO_ATTACH_VOLUMES=\"Yes\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='MULTI_MC=\"'${num_inst}'\"';			sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='DEFAULT_DEBUG_DI=\"yes\"';				sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='TRACE_MAX_LOGS_nvmeibc_trace_goodpath=\"256\"';	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='MLX5_RDDA_ENABLED=\"No\"';                       	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='TCP_ENABLED=\"No\"';                       	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='TOMA_CLOUD_MODE=\"No\"';                       	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='AGENT_CLOUD_MODE=\"No\"';                       	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
		str='NVMF_IP=\"\"';                                  	sudo bash -c "echo ${str} >> $nvmesh_conf_path"; echo $str
	}

	function NVMESH_tirtur() {				# TIRTUR
		clear;
		#Todo: use for i in {100..1}; do echo 1 > /proc/nvmeibc/disks/S3HCNX0JC01991.1/rediscover; sleep 3; done
		while (true); do
			if [ $# -eq 0 ]; then
				rand_scenario=$(( ( RANDOM % 5 )  + 1 ))		# random disaster scenario
			else
				rand_scenario=$1
			fi
			rand_no_io_sec=$[ ( $RANDOM % 7 )  + 8 ]		# Disaster for 8..14[sec], enough to create some dbits
			rand_recovery_sec=$[ ( $RANDOM % 40 )  + 30 ]		# Recover for 0.5~1.5[min]
			if [ $rand_scenario -eq 1 ]; then
				echo "------------------>Toma restart: $rand_no_io_sec[sec]"
				NVMESH_service toma stop;
				sleep "$rand_no_io_sec"s;
				NVMESH_service toma restart;
			elif [ $rand_scenario -eq 2 ]; then
				echo "------------------>Port restart: $rand_no_io_sec[sec]"
				NVMESH_port ens7 down
				sleep "$rand_no_io_sec"s;
				NVMESH_port ens7 up
			elif [ $rand_scenario -eq 3 ]; then
				echo "------------------>Srvr restart: $rand_no_io_sec[sec]"
				NVMESH_service target stop
				sleep "$rand_no_io_sec"s;
				NVMESH_service target start
			elif [ $rand_scenario -eq 4 ] || [ $rand_scenario -eq 5 ]; then
				disk_addrs=$(cat /proc/nvmeibs/smart* | grep 'Pci Address=' | cut -f2- -d '=');
				if [ $rand_scenario -eq 4 ]; then
					echo "------------------>1 Disk pull/push: $rand_no_io_sec[sec]"
					disk_addrs="$(echo $disk_addrs | cut -f2 -d ' ')";	# Use only 1 disk
				else
					n_disks="$(echo "$disk_addrs" | wc -w)"
					echo "------------------>All ${n_disks} Local Disks pull/push: $rand_no_io_sec[sec]"
				fi
				slots="";
				for x in $disk_addrs; do
					slot_number=$(/usr/sbin/lspci -s ${x} -v | grep -i slot | cut -d ':' -f 2 | sed 's/^[[:blank:]]*//;s/[[:blank:]]*$//');
					echo "removing disk_addr |$x|, slot number |$slot_number|";
					slots="$slot_number $slots";
					sudo bash -c "echo 0 > /sys/bus/pci/slots/$slot_number/power";
				done;
				sleep "$rand_no_io_sec"s;
				for slot_number in $slots; do
					echo "Returning slot number |$slot_number|";
					sudo bash -c "echo 1 > /sys/bus/pci/slots/$slot_number/power"
				done;
			elif [ $rand_scenario -eq 10 ]; then
				echo "------------------>Disks rescan: $rand_no_io_sec[sec]"
				shopt -s nullglob;
				for x in /sys/bus/pci/drivers/nvmeibs/0*/remove; do
					sudo bash -c "echo 1 > $x";
				done;
				sleep "$rand_no_io_sec"s;
				sudo bash -c "echo 1 > /sys/bus/pci/rescan";
			else
				echo "Unknown scenario"
			fi
			echo "------------------>Recovering $rand_recovery_sec[sec]"
			sleep "$rand_recovery_sec"s;
		done
	}

	function NVMESH_vol_attach {
		if [ $# -eq 0 ]; then
			echo "input: Vol1,Vol2,Vol3  or  Vol*;  # No spaces between volume names";
			return 0;
		fi
		cmd="sudo nvmesh client attach --id `hostname` --volume $@";
		echo $cmd; eval $cmd;
	}

	function NVMESH_vol_detach {
		if [ $# -eq 0 ]; then
			echo "input:   Vol1,Vol2,Vol3 --force     or  Vol*   or  -a for all";
			return 0;
		fi
		cmd="sudo nvmesh client detach --id `hostname`";
		[[ "$1" == "-a" ]] && cmd="${cmd} --all ${@:2}" || cmd="${cmd} --volume $@";
		echo $cmd; eval $cmd;
	}
	function NVMESH_vol_upgrade {
		if [ $# -eq 0 ]; then
			echo "input:   vol1";
			return 0;
		fi
		VVV=$1
		sudo bash -c 'echo -n "detachv '$VVV' --upgrade" > /proc/nvmeibc/cli/cli';
	}

	function NVMESH_vol_watch {
		if [ $# -eq 0 ]; then
			echo "input: <vol name> -c clear io stats/ -C clear all counters/-a to attach, or {} to watch atom only, or {l} to watch lsblk"
		elif [ "$1" == "{}" ]; then
			cmd_vol='echo "----------- nvmeiba-------"; cat /proc/nvmeiba/status; echo -n "----------->Refcnt: nvmeibc="; cat /sys/module/nvmeibc/refcnt | tr -d "\n"; echo -n ", nvmeiba="; cat /sys/module/nvmeiba/refcnt | tr -d "\n"; echo -n ", WARN_ON="; cat /sys/module/nvmeibc/parameters/num_warnings; lsblk -o NAME,SIZE,MAJ\:MIN | grep -e "\!" -e "nvmesh/" -e NAME; cat /proc/nvmeibc/inst_list.json';
			cmd_blob='ls /proc/nvmeibc/volumes/*/blob.txt | while read par; do printf "_______________%-64s\n" "$par"; cat $par; done;';
			cmd_persist='echo "------- ${NVMESH_DIR_OPT}/clnt_instance_configuration -----"; tree ${NVMESH_DIR_OPT}/block_devices* --noreport; tree ${NVMESH_DIR_OPT}/clnt_instance_configuration --noreport'
			cmd_persist="echo ------- ${NVMESH_DIR_OPT}/clnt_instance_configuration -----; tree ${NVMESH_DIR_OPT}/block_devices\* --noreport; tree ${NVMESH_DIR_OPT}/clnt_instance_configuration --noreport";
			cmd_lblk='cat /proc/devices | grep nvmeibc; ls -l /dev/nvmesh';
			watch -d -n 1 "$cmd_vol; $cmd_blob"; # $cmd_persist";
		elif [ "$1" == "{l}" ]; then
			if [[ ! -z "`cat /sys/module/nvmeibc/parameters/default_dir_lsblk`" ]]; then
				NVMESH_VOLS_DIR=`cat /sys/module/nvmeibc/parameters/default_dir_lsblk`;
				lsblk -bDt | grep -e "\!$1" -e ${NVMESH_VOLS_DIR} -e NAME;	# grep by dir of nvmesh volumes
			else
				nvmeibc_module_major=`cat /proc/devices | grep nvmeibc | cut -d " " -f1`;
				blkext_module_major=`cat /proc/devices | grep blkext | cut -d " " -f1`;
				lsblk | grep -e "\!$1" -e "${nvmeibc_module_major}" -e "${blkext_module_major}" -e NAME; # grep by major of nvmeibc
			fi
		else
			VVV=$1
			PROC=/proc/nvmeibc/volumes/$VVV;
			if [ "$2" == "-c" ]; then
				sudo bash -c 'echo -n "#'$VVV'|clear_io_stats" > /proc/nvmeibc/cli/cli';
			elif [ "$2" == "-C" ]; then
				sudo bash -c 'echo -n "#'$VVV'|clear_all_cntrs" > /proc/nvmeibc/cli/cli';
			elif [ "$2" == "-a" ]; then
				NVMESH_vol_attach $VVV
			fi
			watch -d -n 1 "cat $PROC/status; cat $PROC/iostats; cat $PROC/client_processes; cat $PROC/recov_stats; cat $PROC/io_throttle; echo "----------- nvmeiba-------"; cat /proc/nvmeiba/status; echo -n "Toma: pid="; pgrep toma | tr -d '\n'; echo -n ", Leader="; cat /${NVMESH_DIR_LOG}/toma_leader_name; echo -n "Refcnt: nvmeibc="; cat /sys/module/nvmeibc/refcnt | tr -d '\n'; echo -n ", nvmeiba="; cat /sys/module/nvmeiba/refcnt | tr -d '\n'; echo -n ", WARN_ON="; cat /sys/module/nvmeibc/parameters/num_warnings; cat /proc/nvmeibc/status | grep Free"
		fi
	}
	function NVMESH_vol_watch_inst {
		if [ $# -eq 0 ]; then
			echo "input: <instance> <vol name> -c to clear io stats/-a toattach"
		else
			DIR=$1
			VVV=$2
			PROC=/proc/$DIR/volumes/$VVV;
			if [ "$3" == "-c" ]; then
				sudo bash -c 'echo -n "#'$VVV'|clear_io_stats" > /proc/nvmeibc/cli/cli';
			elif [ "$3" == "-a" ]; then
				NVMESH_vol_attach $VVV
			fi
			watch -d -n 1 "cat $PROC/status; cat $PROC/iostats; cat $PROC/client_processes; cat $PROC/recov_stats; cat $PROC/io_throttle; echo "----------- nvmeiba-------"; cat /proc/nvmeiba/status; echo -n "Toma: pid="; pgrep toma | tr -d '\n'; echo -n ", Leader="; cat /${NVMESH_DIR_LOG}/toma_leader_name; echo -n "Refcnt: nvmeibc="; cat /sys/module/nvmeibc/refcnt | tr -d '\n'; echo -n ", nvmeiba="; cat /sys/module/nvmeiba/refcnt"
		fi
	}

	function NVMESH_port() {
		if [ $# -eq 0 ]; then
			PORT=ens7
		else
			PORT=$1
		fi

		if [ "$2" == "up" ]; then
			CMD="sudo /usr/sbin/ifup $PORT";
		elif [[ $2 == do* ]]; then
			CMD="sudo /usr/sbin/ifdown $PORT";
		elif [[ $2 == re* ]]; then
			CMD="sudo /usr/sbin/ifdown $PORT && sleep 4s && sudo /usr/sbin/ifup $PORT";
		else
			echo "params: up/down/restart";
			return 0;
		fi
		echo $CMD; $CMD
		ifconfig | grep $PORT
	}

	function NVMESH_crash() {
		export VM_LINUX_PATH="/usr/lib/debug/lib/modules/`uname -r`/vmlinux"
		export LAST_CRASH_FOLDER=`ls -d /var/crash/*/ 2> /dev/null | tail -1`
		#### sudo yum install -y subscription-manager; sudo subscription-manager repos --enable rhel-8-server-debug-rpms
		#### sudo yum install crash -y; sudo yum search kernel-debug
		# Do: cat /etc/os-release | grep NAME, See which os this is
		# sudo vim /etc/yum.repos.d/lustre.repo or Rocky-Debuginfo.repo - mark enabled=1
		# sudo yum install kernel-debuginfo-`uname -r`;
		if true; then
			#find ${NVMESH_DIR_SRC} -name crash_analyzer.*
			if [[ -z `command -v ofed_info` ]]; then
				OFED_PATH_BASE="none";
			else
				OFED_PATH_BASE="`ofed_info -s | rev | cut -c 2- | rev`";
			fi
			OFED_PATH="${OFED_PATH_BASE}_`uname -r`";
			PATH_TO_KO_ATOM="${NVMESH_DIR_SRC}/client-repo/client_$OFED_PATH/client/atom/nvmeiba.ko"
			PATH_TO_KO_CLNT="${NVMESH_DIR_SRC}/client-repo/client_$OFED_PATH/client/nvmeibc.ko"
			PATH_TO_KO_SRVR="${NVMESH_DIR_SRC}/target-repo/target_$OFED_PATH/target/nvmeibs.ko"
			PATH_TO_KO_COMN="${NVMESH_DIR_SRC}/common-repo/common_$OFED_PATH/common/nvmeib_common.ko"
			PATH_TO_SO_CRAN="${NVMESH_DIR_SRC}/"
		else
			PATH_TO_KO=${MY_PROJECTS_DIR}/$cur_branch
			if [ "$1" == "infra" ]; then
				PATH_TO_KO=/tmp/infraClient/nvmesh
			elif [ "$1" == "tmp" ]; then
				PATH_TO_KO=./projects/a
				export LAST_CRASH_FOLDER=.
			elif [ "$1" == "here" ]; then
				PATH_TO_KO=./projects/a
				export LAST_CRASH_FOLDER=.
				export VM_LINUX_PATH="./vmlinux"
			fi
			PATH_TO_KO_ATOM="$PATH_TO_KO/clnt/atom/nvmeiba.ko"
			PATH_TO_KO_CLNT="$PATH_TO_KO/clnt/nvmeibc.ko"
			PATH_TO_KO_SRVR="$PATH_TO_KO/srv/nvmeibs.ko"
			PATH_TO_KO_COMN="$PATH_TO_KO/common/nvmeib_common.ko"
			PATH_TO_SO_CRAN="$PATH_TO_KO"
		fi
		my_crash_init="/tmp/${USER}_crashinit.txt"
		echo "Using file: $my_crash_init"
		echo "gdb set disassembly-flavor intel" > $my_crash_init
		echo "mod -s nvmeiba $PATH_TO_KO_ATOM" >> $my_crash_init
		echo "mod -s nvmeibc $PATH_TO_KO_CLNT" >> $my_crash_init
		echo "mod -s nvmeibs $PATH_TO_KO_SRVR" >> $my_crash_init
		echo "mod -s nvmeib_common $PATH_TO_KO_COMN" >> $my_crash_init
		CA_SO="$PATH_TO_SO_CRAN/tools/crash_analyzer/crash_analyzer.so"
		if test -f "$CA_SO"; then
			echo "Crash analyzer already compiled"
		else
			cd $PATH_TO_SO_CRAN/tools/crash_analyzer/;
			sudo make;
			cd -
		fi
		echo "extend $PATH_TO_SO_CRAN/tools/crash_analyzer/crash_analyzer.so" >> $my_crash_init
		echo "foreach bt -f > vmcore_output.txt" >> $my_crash_init
		echo "set > problem.txt"  >> $my_crash_init
		echo "kmem -i > allocs.txt" >> $my_crash_init
		echo "ps > processdata.txt" >> $my_crash_init
		echo "files > files.txt" >> $my_crash_init
		echo "tracedump all ./bin_traces/" >> $my_crash_init
		echo "---------- conent of $my_crash_init -----------------"
		cat $my_crash_init
		echo "---------- openning $LAST_CRASH_FOLDER -----------------"
		sudo crash $LAST_CRASH_FOLDER/vmcore $VM_LINUX_PATH -i $my_crash_init

		#sudo ${MY_PROJECTS_DIR}/$cur_branch/tools/traces_post_processor/pager_wrapper.py
		#gdb ./nvmeiba.ko.   list *nvmeiba_os_apis_tostring+0x15b
		#struct operation 0xFFF123123, struct nvmeibc_raid1 -ox, struct active_md_of 0xffffae4448a01000 -c 10 | grep -e wmx -e ow -e vlba
		#struct nvmeibc_disk_percpu_cmds_stats 0x3c64a1006cc0:a //:a for all cpus
		#rd -r ff.txt  <variable name/memory pointer>  <length 4096>
		# See source of function: dis -s nvmeibc_operation_destroy
		#struct nvmeibc_disk_io_command -l nvmeibc_disk_io_command.reqs1 ffff92469c0f39b0
		#kmem page
		#search ptr
	}

	function NVMESH_unitest() {
		clear;
		if [ "$2" == "-verbose" ]; then
			verbose="True";
			set -x;
		else
			verbose=""
		fi
		if [ $# -eq 0 ]; then
			echo "input: inst/reserv/clean/force_detach/mount/upgrade/ref_counts/elev -verbose"
			return 0;
		elif [[ $1 == inst* ]]; then
			echo_title "Clnt Instances Unitest"
			for i in {3..1}; do NVMESH_clnt_inst add ${i}; done;
			NVMESH_vol_attach V2;
			NVMESH_clnt_inst rmv 6;
			NVMESH_clnt_inst add 1;
			NVMESH_clnt_inst add 311111111111111111111111111111111;		#Fail: Incorect params, name too long
			NVMESH_clnt_inst rmv 1						# Should fail
			NVMESH_clnt_inst rmv 2
			# instaces 0 and mc0003 are still connected

			echo "--------------- Hot Upgrade -------------";  #instances/volumes are recreated
			NVMESH_unitest upgrade;
			# Note: Here age of volume > age of isntance

			echo "--------------- Restart client -------------";  #instances/volumes are recreated
			NVMESH_service all restart;

			echo "--------------- Cleanup -------------"
			NVMESH_clnt_inst rmv 3
			NVMESH_vol_detach -a --force # Should succeed
			# Post condition, single instance, no volumes attached
		elif [[ $1 == clean* ]]; then
			echo -e "---------------\e[34m Cleanup After Unitest \e[39m-------------"
			for i in {3..1}; do NVMESH_clnt_inst rmv ${i}; done;
			NVMESH_vol_detach -a --force;
			sudo bash -c 'echo 5 > /sys/module/nvmeibc/parameters/self_recovery_detach_time_sec';
		elif [ "$1" == "force_detach" ]; then
			echo "--------------- Attach/Detach unitest -------------"
			cd ${io_stress_dir}
			sudo ./heavy_io_unsafe_detach.py V4 -n 5 -m /tmp/mnt
		elif [ "$1" == "mount" ]; then
			VOL_NAME=V4
			VOL=/dev/nvmesh/$VOL_NAME;
			echo "--------------- remount Vol $VOL, EC-4462 -------------"
			sudo mkdir /mnt/${USER};
			sudo mkdir /mnt/${USER}2;
			#sudo mkfs -t ext4 $VOL;
			sudo mkfs.xfs -f $VOL;
			sudo mount -o ro $VOL /mnt/${USER}/;
			sudo mount -o rw,remount $VOL /mnt/${USER}/;
			sudo mount -o ro,remount $VOL /mnt/${USER}/;
			sudo mount -o rw,remount $VOL /mnt/${USER}/;
			sudo mount $VOL /mnt/${USER}2;		# Second mount point
			sudo umount /mnt/${USER}/
			sudo umount $VOL
			echo "--------------- mount via nvmeibc, umount via nvmeiba -------------"
			sudo mount -o ro $VOL /mnt/${USER}/;
			NVMESH_vol_detach $VOL_NAME --force;
			cat /proc/nvmeiba/status | grep $VOL_NAME;			# nvmeiba holds atom with 1 reference
			sudo mount -o rw,remount $VOL /mnt/${USER}/;			# Should fail, cannot open this block device
			sudo umount /mnt/${USER}/;
			cat /proc/nvmeiba/status | grep $VOL_NAME;			# nvmeiba released the atom
			echo "--------------- umount Vol $VOL -------------"
			for i in {3..1}; do
				sudo mount $VOL /mnt/${USER}/;
				NVMESH_service all stop
				sudo umount /mnt/${USER}/
				NVMESH_vol_detach -a --force;
				NVMESH_service all start;
			done;
			sudo rm -rf /mnt/${USER};
			sudo rm -rf /mnt/${USER}2;
		elif [ "$1" == "upgrade" ]; then
			sudo nvmesh_clnt_shutdown --upgrade --json;
			#clear; sudo python -m trace --trace ./bin/nvmesh_clnt_shutdown.py --upgrade --debug
			cat /proc/nvmeiba/status | grep Upg;
			NVMESH_service all stop;
			if [ "$2" == "reinstall" ]; then
				sudo yum remove nvmesh-core.x86_64
				sudo yum install nvmesh-core-1.9.0*.x86_64.rpm;
				NVMESH_generate_conf_file;
			fi
			NVMESH_service all start;
		elif [ "$1" == "ref_counts" ]; then
			echo -e "---------------\e[34m RRef-counts Unitest \e[39m-------------"
			# Precondition: Volumes V1 exists, Only 1 instance in the system
			VVV=V1;
			CLI=/proc/nvmeibc/cli/cli;
			echo "Refcnt ba: nvmeibc=$(cat /sys/module/nvmeibc/refcnt), nvmeiba=$(cat /sys/module/nvmeiba/refcnt)"
			NVMESH_vol_attach "$VVV";
			sleep 1s;
			echo "Refcnt aa: nvmeibc=$(cat /sys/module/nvmeibc/refcnt), nvmeiba=$(cat /sys/module/nvmeiba/refcnt)"
			sudo bash -c "echo -n \"#$VVV|os_ptr++\" > $CLI";
			sleep 1s;
			echo "Refcnt ++: nvmeibc=$(cat /sys/module/nvmeibc/refcnt), nvmeiba=$(cat /sys/module/nvmeiba/refcnt)"
			sudo bash -c "echo -n \"#$VVV|os_ptr--\" > $CLI";
			sleep 1s;
			echo "Refcnt --: nvmeibc=$(cat /sys/module/nvmeibc/refcnt), nvmeiba=$(cat /sys/module/nvmeiba/refcnt)"
			NVMESH_vol_detach "$VVV" --force;
			echo "Refcnt ad: nvmeibc=$(cat /sys/module/nvmeibc/refcnt), nvmeiba=$(cat /sys/module/nvmeiba/refcnt)"
		elif [[ $1 == elev ]]; then
			VOLUME=R1
			NVMESH_debug_set elev
			sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/mini_elevator";
			echo_title "Setting elevator params";
			#sudo bash -c "echo 100000000 > /sys/module/nvmeibc/parameters/max_ios_per_cpu";	# Unneeded, Mini elevator used to get stuck with throtteling
			#sudo bash -c "echo 100 > /sys/module/nvmeibc/parameters/mini_elevator_jiffies";
			#sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/mini_elev_plug_in_throttle_wq";	# Depricated module param
			NVMESH_show_info params | grep -e cpu -e elev;

			echo -e "------- 800[MB] sequential non direct-dd, Elevator=\e[34mN\e[39m --------"
			NUM_OPS=200000
			sudo bash -c "echo 0 > /sys/module/nvmeibc/parameters/mini_elevator";
			sudo bash -c 'echo -n "#|clear_io_stats" > /proc/nvmeibc/cli/cli';
			sudo dd of=/dev/nvmesh/${VOLUME} if=/dev/zero bs=4k seek=0 count=$NUM_OPS status=progress;
			echo -e "\033[0;32m`cat /proc/nvmeibc/volumes/${VOLUME}/iostats | grep  num_ops| awk  '{ print $1,$4 }'` instead of $NUM_OPS\033[0m"
			echo -e "------- 800[MB] sequential non direct-dd, Elevator=\e[34mY\e[39m --------"
			sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/mini_elevator";
			sudo bash -c 'echo -n "#|clear_io_stats" > /proc/nvmeibc/cli/cli';
			sudo dd of=/dev/nvmesh/${VOLUME} if=/dev/zero bs=4k seek=0 count=$NUM_OPS status=progress;
			echo -e "\033[0;32m`cat /proc/nvmeibc/volumes/${VOLUME}/iostats | grep  num_ops| awk  '{ print $1,$4 }'` instead of $NUM_OPS\033[0m"
			echo -e "------- 4[GB] sequential non direct-fio, Elevator=\e[34mN\e[39m --------"
			NUM_OPS=1000000
			sudo bash -c "echo 0 > /sys/module/nvmeibc/parameters/mini_elevator";
			sudo bash -c 'echo -n "#|clear_io_stats" > /proc/nvmeibc/cli/cli';
			time sudo fio --filename=/dev/nvmesh/${VOLUME} --rw=write --size=4g --numjobs=4 --iodepth=1 -bs=4k --ioengine=libaio --direct=0 --group_reporting --name=test.txt --verify=0
			echo -e "num_ops=\033[0;32m`cat /proc/nvmeibc/volumes/${VOLUME}/iostats | grep  num_ops| awk  '{ print $4 }'`\033[0m"
			echo -e "------- 4[GB] sequential non direct-fio, Elevator=\e[34mY\e[39m --------"
			sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/mini_elevator";
			sudo bash -c 'echo -n "#|clear_io_stats" > /proc/nvmeibc/cli/cli';
			time sudo fio --filename=/dev/nvmesh/${VOLUME} --rw=write --size=4g --numjobs=4 --iodepth=1 -bs=4k --ioengine=libaio --direct=0 --group_reporting --name=test.txt --verify=0
			echo -e "num_ops=\033[0;32m`cat /proc/nvmeibc/volumes/${VOLUME}/iostats | grep  num_ops| awk  '{ print $4 }'`\033[0m instead of $NUM_OPS"

			if false; then
				NUM_OPS=1000000
				sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/mini_elevator";
				echo -e "------- 10 loops 4[GB] sequential non direct-dd, Elevator=\e[34mY\e[39m --------"
				for i in {1..10}; do
					sudo bash -c 'echo -n "#|clear_io_stats" > /proc/nvmeibc/cli/cli';
					sudo dd of=/dev/nvmesh/v1 if=/dev/zero bs=4k seek=0 count=1000000;
					echo "`cat /proc/nvmeibc/volumes/v1/iostats | grep  num_ops| awk  '{ print $1,$4 }'` instead of $NUM_OPS"
				done;
			fi
			#sudo /${NVMESH_DIR_LOG}/trace_daemon/pager.py /${NVMESH_DIR_LOG}/trace_daemon -l nvmeibc_trace_long nvmeibc_trace_eter '-f (not file = nvmeibs_serjio.c) and (not file = nvmeibc_jam.c)' -t now-1m > z_clnt.txt; grep operation z_clnt.txt > z_opt.txt;
			#sudo fio --filename=/dev/nvmesh/v1  --rw=write --size=64m--runtime=60 --numjobs=1 --iodepth=1 -bs=4k --ioengine=libaio --direct=1 --group_reporting --name=elev.txt --time_based --verify=0
			#sudo bash -c 'echo "#|volume_elevflush" > /proc/nvmeibc/cli/cli'
			#sudo bash -c 'echo -n "#|clear_io_stats" > /proc/nvmeibc/cli/cli'; sudo dd of=/dev/nvmesh/v1 if=/dev/zero bs=4k seek=0 count=200000 status=progress; cat /proc/nvmeibc/volumes/v1/iostats | grep  num_ops
			#sudo bash -c '/tmp/btest -b 4k -r 1 -e --affinity -T 4 -w 1 S W /dev/nvmesh/v1'
		fi
		if [ ! -z "$verbose" ]; then
			set +x;
		fi
	}

	function NVMESH_clnt_inst() {
		if [ $# -eq 0 ]; then
			echo -e "\tinput: (add/rmv [0..9])/dump";
			echo -e "\tfor i in {3..1}; do NVMESH_clnt_inst add \${i}; done;"
		elif [[ $1 == dump* ]]; then
			cat /proc/nvmeiba/status | grep -o -G 'nvmeibc=[0-9]*';
			echo_title /proc/devices;
			cat /proc/devices | grep -e mc0 -e nvmeib;
			echo_title instances params;
			cat /proc/nvmeibc/inst_list.json
			NVMESH_show_info vol_stats;
		else
			[[ "$1" == a* ]] && action="++" || action="--";
			direct_ioctl="%clnt${action}{mc000$2,mc000$2}paramc:tcp_mode=0|paramm:cfg_name=Cluster Default|paramm:cfg_id=cluster_default|paramm:cfg_version=1";
			echo "\t$direct_ioctl";
			sudo bash -c "echo \"${direct_ioctl}\" > /proc/nvmeibc/instctls";
			NVMESH_clnt_inst dump;
		fi
	}

	function NVMESH_debug_set() {
		if [ "$1" == "max" ]; then
			echo_title "Setting max log levels";
			sudo dmesg --clear;
			sudo bash -c 'echo -n "#|clear_all_cntrs" > /proc/nvmeibc/cli/cli';
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level";
			sudo bash -c "echo 5 > /sys/module/nvmeibc/parameters/tracer_debug_level";
			sudo bash -c "echo 4 > /sys/module/nvmeibc/parameters/goodpath_debug_level";
			sudo bash -c "echo 1 > /sys/module/nvmeiba/parameters/verbose_debug";	# nvmeiba
		elif [ "$1" == "speed" ]; then
			sudo cpupower frequency-set --governor performance; #https://askubuntu.com/questions/1021748/set-cpu-governor-to-performance-in-18-04
			sudo bash -c "echo -n '#|set_read_edic=0' > /proc/nvmeibc/cli/cli";
			sudo bash -c "echo 0 > /sys/block/nvmesh\!*/queue/iostats"
			#sudo bash -c "echo 4096 > /sys/block/nvmesh\!*/queue/read_ahead_kb"
			sudo bash -c "echo 0 > /sys/module/nvmeibc/parameters/profiling_enabled"
			sudo bash -c "echo 100000000 > /sys/module/nvmeibc/parameters/max_ios_per_cpu"
			echo 1 > /sys/module/nvmeib_common/parameters/hide_warnings_stack;
			NVMESH_debug_set min
		elif [ "$1" == "min" ]; then
			echo_title "Setting min log levels";
			sudo dmesg --clear;
			sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/debug_level";
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/tracer_debug_level";
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/goodpath_debug_level";
		elif [ "$1" == "_t" ]; then
			echo_title "Setting _t log levels";
			sudo bash -c "echo 2 > /sys/module/nvmeibc/parameters/debug_level";
			sudo bash -c "echo 4 > /sys/module/nvmeibc/parameters/tracer_debug_level";
			sudo bash -c "echo 4 > /sys/module/nvmeib_common_public/parameters/tracer_debug_level";
			sudo bash -c "echo 4 > /sys/module/nvmeib_common/parameters/tracer_debug_level";
		elif [ "$1" == "elev" ]; then
			NVMESH_debug_set max
			sudo bash -c "echo 64 > /sys/module/nvmeibc/parameters/max_ios_per_cpu";
			echo "-------Setting elevator --------"
			sudo bash -c "echo 1 > /sys/module/nvmeibc/parameters/mini_elevator";
		elif [ "$1" == "dbgdi" ]; then
			echo_title "Setting debug di for all vols";
			VVV="*";
			sudo bash -c 'echo -n "#'$VVV'|di_debug_mode=1" > /proc/nvmeibc/cli/cli';
			sudo bash -c 'echo 64 > /sys/module/nvmeibc/parameters/max_ios_per_cpu';
			sudo bash -c "echo -n '#|set_read_edic=0' > /proc/nvmeibc/cli/cli";		// Disable edic check, coz dbg di and hidden attach screw it
			grep -o '\"edic\": *[01]' /proc/nvmeibc/volumes/$VVV/status
			grep -e 'debug di' /proc/nvmeibc/volumes/$VVV/status
		else
			echo "params: max/_t/min/speed/dbgdi/elev"
			return 0;
		fi
	}

	function NVMESH_bin_traces() {
		clear;
		tr_path=/${NVMESH_DIR_LOG}/trace_daemon;
		pg_path=$tr_path/pager.py;		# Auto locate path to pager
		[ ! -f $pg_path ] && pg_path=${NVMESH_DIR_SRC}/tools/traces_post_processor/pager.py;
		[ ! -f $pg_path ] && pg_path=${NVMESH_DIR_SRC}/common-repo/tools/traces_post_processor/pager.py
		[ ! -f $pg_path ] && pg_path=".${pg_path}";		# try local to current dir
		[ ! -f $pg_path ] && echo_red "pager.py could not be found!";
		# From Laptop: tools/connect_interactive.sh n111;
		# lba = VLBA("tv-38788-13", 5940581)
		# lba.dlba; lba.tab
		#param="'-f (not file = nvmeibs_serjio.c) and (not file = nvmeibc_jam.c)'"
		param=""
		if [ "$1" == "clnt" ]; then
			cmd="sudo $pg_path $tr_path -l nvmeibc_trace_long nvmeibc_trace_eter $param | less"; echo $cmd; eval $cmd;
		elif [ "$1" == "datapath" ]; then
			cmd="sudo $pg_path $tr_path --clnt $param | less"; echo $cmd; eval $cmd;
		elif [ "$1" == "toma" ]; then
			cmd="sudo $pg_path $tr_path --toma $param | less"; echo $cmd; eval $cmd;
		elif [ "$1" == "emerg" ]; then
			logs_dir="./z_last_logs";
			echo_title "Deleting old logs";
			cmd="sudo rm -rf $logs_dir"; echo $cmd; eval $cmd;
			mkdir -p $logs_dir
			#param="${param} -t now-5m"
			param="${param} --since tail-5m"
			echo_title "journalctl WARNINGS";
			cmd="sudo journalctl --since \"5 min ago\" > $logs_dir/z_jctl_5min.txt"; echo $cmd; eval $cmd;
			grep 'Call Trace' $logs_dir/z_jctl_5min.txt > $logs_dir/z_jctl_5min_warn.txt;
			[ -s $logs_dir/z_jctl_5min_warn.txt ] && echo_red "journalctl Has errors!" || echo "0";
			echo_title "Saving new logs";
			cmd="sudo /opt/nvmesh/common-repo/scripts/nvmesh_snapshot_logs.sh --toma"; echo $cmd; eval $cmd;
			cmd="sudo $pg_path $tr_path --clnt $param > $logs_dir/z_clnt_5min.txt"; echo $cmd; eval $cmd;
			cmd="sudo $pg_path $tr_path -l nvmeibp_trace_long nvmeibm_trace_long $param > $logs_dir/z_comn_5min.txt"; echo $cmd; eval $cmd;
			cmd="sudo $pg_path $tr_path -l nvmeibp_trace_long nvmeibs_trace_long $param > $logs_dir/z_srvr_5min.txt"; echo $cmd; eval $cmd;
			cmd="sudo $pg_path $tr_path --toma   $param > $logs_dir/z_toma_5min.txt"; echo $cmd; eval $cmd;
			#cmd="sudo $pg_path $tr_path -l nvmeibs_trace_long $param > $logs_dir/z_srvr_5min.txt"; echo $cmd; eval $cmd;
			echo_title "Saving counters";
			rm -f $logs_dir/z_clnt_cntrs.txt;
			ls /proc/nvmeibc/volumes/*/flow_cntr.json | while read vol; do
				echo -e "\n\n$vol --> " >> $logs_dir/z_clnt_cntrs.txt;
				cat $vol >> $logs_dir/z_clnt_cntrs.txt;
			done;
			echo_title "Saving serjio /proc"
			mkdir -p $logs_dir/serjio;
			sudo cp -r /proc/nvmeibs/serjio/* $logs_dir/serjio;
			echo_title "Optional: sudo nvmesh_logs_collector";
		elif [ "$1" == "my_debug" ]; then
			NVMESH_debug_set max;
			clear; NVMESH_bin_traces emerg;
			#grep -e owner_locks_release_group.*4db48e50 $logs_dir/z_toma.txt;
			#grep __blocksets_problems_read_cb.*EC21|__get_blksets_info_next_work_batch.*EC21;
			#grep -o 'HTR V.EC21.B.0x000000[0-9a-f]*' $logs_dir/z_clnt.txt | sort | uniq;
		elif [ "$1" == "make" ]; then
			cd $pg_path; make; cd -
		elif [ "$1" == "strings" ]; then
			strings toma.binlog0.0 | sort | uniq -c | sort -hr | less
		elif [ "$1" == "install" ]; then
			#cat ${MY_PROJECTS_DIR}/$cur_branch/tools/crash_analyzer/README.md
			sudo yum install -y json-c-devel libuuid-devel openssl-devel bison flex;
		else
			echo_green "params: emerg/clnt/toma/datapath/install/make/my_debug";
		fi
		echo_title "Examples";
		echo "[python3] ./pager.py --clnt $param > z_clnt.txt"
		echo "server2:/data/logs/debug_tools/pager.py --clnt -t '23/06/2020 16:20' | less;          or -t 10:40 10:41";
		echo "./pager.py -l nvmeibc_trace_eter -f '(@DEV_NAME=\"v1\") and (func=dup_topology)' | grep vol_short_id -m 1"
		echo "./pager.py --goodpath -f '(not file = nvmeibs_serjio.c) and (@DEV_NAME=\"V4\") and (@O_DBG_ID=16007557) and (func=__cli_msg_verify_size)' -t now-30m -t '23/03/2020 14:33' '23/03/2020 14:35' --print-date --reliable"
		echo "./nvme1046_nvmesh_logs/${NVMESH_DIR_LOG}/trace_daemon/pager.py -l nvmeibc_trace_goodpath nvmeibc_trace_long -f '(@VOL_ID = 17) and ((@VLBA in [495370+10]) or (@RLBA in [495360+320]))' --since \"2020-07-08 15:07:40\" --until \"2020-07-08 15:26:50\" --collector-dir --nogreet > z_goodpath.txt"
		echo "-f 'fmt like "*Sync start*" and @RLBA = 123 sticky @O_DBG_ID => [@O_DBG_ID, @ORIG_O_DBG_ID]  sticky_until like \"*Operation end*\"'"
		echo "./${NVMESH_DIR_LOG}/trace_daemon/pager.py --mode msg-stream-json --silent -l nvmeibc_trace_long -f ' @VLBA in [3910250+10]'"
		echo "cd ..qa/logs/...../${NVMESH_DIR_LOG}/trace_daemon/ && ./pager.py --silent -f '@VLBA in [3910250+10]' | grep -v PAGER"
		echo_yellow "sudo service nvmeshtrace@trace_daemon restart";
		echo "More info: https://excelero.atlassian.net/wiki/spaces/DEV/pages/1341030401/Binary+Tracer";
	}

	function NVMESH_restart_machine() {
		sync;	sync;
		sudo systemctl disable nvmeshclient;
		sudo systemctl disable nvmeshtarget;
		if true; then
			sudo reboot -f;
		else # Old way to do so
			sudo su; echo 1 > /proc/sys/kernel/sysrq; echo b > /proc/sysrq-trigger
		fi
	}

	function NVMESH_fix_blockset() {
		if [ $# -eq 0 ]; then
			echo "Fix range of blocksets: params: <vol name> <start blkset> <end blockset>. Examples:"
			echo "EC 5 8          - fix blockset 5,6,7 of EC volume"
			echo "R1 0 -1         - fix all blockset of R1 volume"
		else
			cmd="#$1|recov_launch sgmnt=(0,0,0) type=1 is_mandatory=1 do_only_owners=0 blocksets=[$2, $3) jgc_cookie=0"
			#cmd="#EC7|recov_launch sgmnt=(0,0,5) type=6 is_mandatory=0 do_only_owners=0 blocksets=[0, -1) jgc_cookie=0"
			echo "giving cmd: $cmd"
			sudo bash -c "echo \"${cmd}\" > /proc/nvmeibc/cli/cli"
		fi
	}

	function NVMESH_io_btest {
		#dd if=/dev/zero of=/dev/nvmesh/j1 bs=2M status=progress oflag=direct
		# VVV=r2; for ((i = 1; i <= 10; i++)); do ./heavy_io.sh $VVV 0 409600000; done
		if [ $# -eq 0 ]; then
			echo "params: <vol_name or *> <-jx,3>"
			return 0;
		fi
		VVV=$1;
		CCC=0
		[ ! -z "$2" ] && CCC=$2;
		bdev_dir=/dev/nvmesh;
		[ $VVV == *"/"* ] && bdev_dir=/dev; # Vol of client instance other than 0
		if [ "$3" == "-a" ]; then
			echo_title "Running btest IO, no verification";
			cmd="sudo ${io_stress_dir}/btestEX -d -t 0 -T 2 -w 8 -D -B 300000 R 50 $bdev_dir/$VVV";
		else
			echo_title "Running btest IO with verification";
			cmd="sudo ${io_stress_dir}/btestEX -d -c -j $CCC,3 -q -t 0 -T 2 -w 8 -D -B 300000 R 50 $bdev_dir/$VVV";
		fi
		echo $cmd; eval $cmd;
	}

	function NVMESH_io_fio {
		runtime=10
		if [ $# -eq 0 ]; then
			echo "params: <p/v> <vol name> <io depth> <num jobs> <rw%> <blocksize> <optional runtime[s]>"
			echo -e "\033[0;32mPerformance examples: Random fio without verification\033[0m:"
			echo "p EC      32 8 100 1M 25      = reads of 1[mb] for 25[sec]"
			echo "p 'V1 V2'  1 1   0 4K         = writes to 2 volumes for ${runtime}[sec]"
			echo -e "\033[0;32mVerification examples\033[0m:"
			echo "v 'R1'    32 10 50 4K 1d      = read/write io verfication for a full 1 day"
			echo -e "\033[0;32mNon Direct examples\033[0m:"
			echo "e 'R1'    32 10 50 12K 1d      ="
			# Reproduction of non direct bug
			# sudo NVMESH_vol_attach EC8; NVMESH_debug_set dbgdi; sudo bash -c "echo -n '#|set_read_edic=1' > /proc/nvmeibc/cli/cli"; NVMESH_vol_sub_add EC8 p2 0 3
			# sudo fio --filename=/dev/nvmesh/EC8_p2 --direct=0 --rw=readwrite --ioengine=sync --rwmixread=50 --group_reporting --name=edic_12k --time_based --iodepth=16 --numjobs=8 --bs=4k --runtime=25
		else
			fio=fio
			[ -e /usr/local/bin/fio ] && fio=/usr/local/bin/fio;
			declare -a filenames
			for x in $2; do
				if [ -e /dev/nvmesh/$x ]; then
					filenames[${#filenames[@]}]="--filename=/dev/nvmesh/$x"
				else
					if [ -e /dev/$x ]; then
						filenames[${#filenames[@]}]="--filename=/dev/$x"
					else
						filenames[${#filenames[@]}]="--filename=$x"
					fi
				fi
			done

			iodepth=$3
			numjobs=$4
			rw=$5
			blocksize=$6
			if [[ $# > 6 ]] ; then
				runtime=$7
			fi
			if [ "$1" == "p" ]; then
				cmd="sudo $fio ${filenames[@]} --direct=1 --rw=randrw --norandommap --randrepeat=0 --ioengine=libaio --rwmixread=$rw --group_reporting --name=perf_${blocksize} --time_based --verify=0   --iodepth=$iodepth --numjobs=$numjobs --bs=$blocksize --runtime=$runtime"
			elif [ "$1" == "v" ]; then
				cmd="sudo $fio ${filenames[@]} --direct=1 --rw=readwrite --verify_fatal=1          --ioengine=sync   --rwmixread=$rw --group_reporting --name=verf_${blocksize} --time_based --verify=md5 --iodepth=$iodepth --numjobs=$numjobs --bs=$blocksize --runtime=$runtime"
			else
				cmd="sudo $fio ${filenames[@]} --direct=0 --rw=readwrite --ioengine=sync --rwmixread=50 --group_reporting --name=edic_12k --time_based --iodepth=$iodepth --numjobs=8 --bs=4k --runtime=25 --size=12288"
			fi
			echo $cmd; eval $cmd;
		fi
	}

	function NVMESH_io_1block {
		if [ $# -eq 0 ]; then
			echo "do io of 1[blk] params: <R/W/T> <vol name> <vlba>"
			return 0;
		fi
		NVMESH_VOLS_DIR=/dev/`cat /sys/module/nvmeibc/parameters/default_dir_lsblk`;
		VOL_NAME=$2
		VLBA=$3
		cmd=""
		if [ "$1" == "R" ]; then
			cmd="sudo dd of=blk1.dat if=${NVMESH_VOLS_DIR}/$VOL_NAME bs=4K iflag=direct skip=$VLBA count=1"
		elif [ "$1" == "W" ]; then
			cmd="sudo dd if=.bashrc  of=${NVMESH_VOLS_DIR}/$VOL_NAME bs=4K oflag=direct seek=$VLBA count=1"
		else
			N_BYTES=$((${VLBA}*4096));
			cmd="sudo blkdiscard -v -o ${N_BYTES} -l 4K ${NVMESH_VOLS_DIR}/${VOL_NAME}"
		fi
		echo $cmd; eval $cmd;
	}
	function NVMESH_io_check_zero_on_chunk {
		local disk=$1; local start=$2; local end=$3;
		local count=$((end - start));
		echo -e "\tCreating temp zero file for comparison $count[blks]";
		local zero_file="./zero_f.dat";
		dd if=/dev/zero of="$zero_file" bs=4K count="$count" &>/dev/null;
		echo -e "\tReading from disk....";
		if sudo dd if=${disk} bs=4K iflag=direct skip=$start count=$count  2>/dev/null | cmp -s - "$zero_file"; then
			echo_green "\t$disk[$start] length $count[blks] is all zeros";
			rm "$zero_file"; return 0;
		else
			echo_red "\t$disk[$start] length $count[blks] contains non-zero data";
			rm "$zero_file"; return 1;
		fi
	}

	function NVMESH_io_check_zero {
		local disk=$1; local start_hex=$2; local end_hex=$3;
		# Convert the hexadecimal values to decimal
		local start=$((16#$start_hex));
		local end=$((16#$end_hex));
		local count=$((end - start));
		local chunk_size=1000000;	# Iterate over the range in chunks of 4GB
		local total_chunks=$((($count + $chunk_size - 1) / $chunk_size));
		echo "Zero testing: Range $disk[$start..$end),  $total_chunks iterations each of up to $chunk_size[blks]:";
		local i=0;
		while [ $count -gt 0 ]; do
			echo "Iter $i/$total_chunks, remaining $count[blks]";
			if [ $count -ge $chunk_size ]; then
				end=$(($start + $chunk_size));
				NVMESH_io_check_zero_on_chunk $disk $start $end;
				local chunk_rv=$?;
				count=$((count - chunk_size));
			else
				end=$(($start + $count));
				NVMESH_io_check_zero_on_chunk $disk $start $end;
				local chunk_rv=$?;
				count=0;
			fi
			start=$end;
			#echo "Debug: cunk_rv=$chunk_rv";
			if [[ $chunk_rv -eq 1 ]]; then
				echo_red "\n\nAborting after chunk $i of size $chunk_size[blks]. Non zero values found";
				return 1;
			fi;
			i=$(( i + 1 ));
		done
		return 0;
		#debug_cmd="sudo dd if=${disk} bs=4K iflag=direct skip=$start count=$count 2>/dev/null | hexdump -v -e '/1 \"%02x\"'";
		#echo -e "Use the follwoing command to debug the problematic area:\n\t $debug_cmd";
	}

	function NVMESH_io_Nblock {
		if [ $# -eq 0 ]; then
			echo "Write 4M, from each 4GB and verify it. Params: <vol name>"
			return 0;
		fi
		NVMESH_VOLS_DIR=/dev/`cat /sys/module/nvmeibc/parameters/default_dir_lsblk`;
		head -c 4M </dev/urandom > ./rand_blk.dat
		# vim -b ./rand_blk.dat     --> :%!xxd     Edit   :%!xxd -r
		VOL_NAME=$1;
		for i in {0..1}; do
			VLBA=${i}M
			echo "vlba = ${VLBA}[blks]";			# Skip 4[Gbytes]
			sudo dd if=./rand_blk.dat of=${NVMESH_VOLS_DIR}/$VOL_NAME bs=4K oflag=direct seek=$VLBA count=1024
			sudo dd of=./rand_out.dat if=${NVMESH_VOLS_DIR}/$VOL_NAME bs=4K iflag=direct skip=$VLBA count=1024
			rv=`diff ./rand_blk.dat ./rand_out.dat`;
			if [ -z "$rv" ]; then
				echo "Files are identical"
			else
				echo "Files match"
			fi
		done;

	function NVMESH_io_blktrace {
		if [ $# -eq 0 ]; then
			echo "..... Params: <vol name>"
			return 0;
		fi
		[[ -z `mount | grep debugfs` ]] && sudo mount -t debugfs none /sys/kernel/debug;
		NVMESH_VOLS_DIR=/dev/`cat /sys/module/nvmeibc/parameters/default_dir_lsblk`;
		VOL_NAME=$2;
		cmd="sudo blktrace -d ${NVMESH_VOLS_DIR}/$VOL_NAME -o trace_output"; echo ${cmd}; eval ${cmd};
		cmd="sudo blkparse -i trace_output.blktrace.*"; echo ${cmd}; eval ${cmd};
	}

		if false; then
			for i in {0..3}; do
				DISK="/dev/nvme100${i}n1"; filemy="./br.dat"
				head -c 4K </dev/urandom > ${filemy}; DLBA="1443953501"; sudo dd if=${filemy} of=$DISK bs=4K oflag=direct seek=$DLBA count=1; sudo echo "File: ${filemy} -> dlba $DISK:$DLBA"; xxd ${filemy} | head -1
				filemy="./read.dat";                  DLBA="1443953501"; sudo dd of=${filemy} if=$DISK bs=4K iflag=direct skip=$DLBA count=1; sudo echo "dlba $DISK:$DLBA ->  file ${filemy}"; xxd ${filemy} | head -1
			done;
			# Optionally can add to 'dd' >/dev/null 2>&1
		fi
	}
	function NVMESH_io_non_direct_debug {
		# NVMESH-5415. n37 pushes non direct much more in parallel than direct.
		# When bio_no_exec=Y iospeed does not change. max_ios_per_cpu does not matter.
		# Non direct ~30[sec], in_queue=19531   , max_ios_per_cpu=1 has no perf effect, rises in_queue=94409152, see large throttling queues
		# Direct     ~10[sec], in_queue=10802317, max_ios_per_cpu=1 has no perf effect
		PARAM=/sys/module/nvmeibc/parameters/bio_noexec;      sudo bash -c "echo N > ${PARAM}"; cat $PARAM;
		PARAM=/sys/module/nvmeibc/parameters/max_ios_per_cpu; sudo bash -c "echo 1 > ${PARAM}"; cat $PARAM;
		PARAM=/sys/module/nvmeibc/parameters/bio_noexec;      sudo bash -c "echo Y > ${PARAM}"; cat $PARAM;
		PARAM=/sys/module/nvmeibc/parameters/max_ios_per_cpu; sudo bash -c "echo 100000000 > ${PARAM}"; cat $PARAM;
		PARAM=/sys/module/nvmeibc/parameters/use_block_external_major; cat $PARAM;
		#strace -tt -o ~/z_fio.txt
		time fio --direct=1 --numjobs=8 --iodepth=256 --bs 4k --filename=/dev/nvmesh/J1 --name=test --rw=randrw --ioengine=libaio --rwmixwrite 100 --group_reporting --time_based --runtime 10 --refill_buffers
		#time fio --direct=0 --bs 4k --filename /dev/nvmesh/J1 --name test --rw randrw --numjobs=8 --iodepth 1 --ioengine psync --rwmixwrite 99 --group_reporting --time_based --runtime 20 --refill_buffers
		clear; VOL='nvmesh!J1'; ls /sys/block/$VOL/queue/ | while read par; do printf "%-64s" "$par"; cat /sys/block/$VOL/queue/$par; done;
	}

	function NVMESH_show_info {
		if [[ $1 == mod* ]]; then
			clear;
			echo_title "nvmeibc"; sudo modinfo nvmeibc;
			echo_title "nvmeibs"; sudo modinfo nvmeibs;
			echo_title "nvmeiba"; sudo modinfo nvmeiba;
			echo_title "usage";
			ls -d /sys/module/nvmeib* | while read mod; do echo -n "Refcount: $mod="; tail -n +1 $mod/refcnt; ls $mod/holders/; echo "---"; done;
		elif [[ $1 == size_vol* ]]; then
			if [ $# -eq 1 ]; then
				echo "Missing argument <vol name>";	return 5;
			fi
			vvv=$2;
			vol=nvmesh/${vvv};
			echo_green "blockdev --getsize64"; sudo blockdev --getsize64 /dev/$vol;
			echo_green "lsblk"; lsblk | grep $vol;
			echo_green "sys/block"; echo $((`cat /sys/block/nvmesh\!${vvv}/size`*512));
			echo_green "lsof"; sudo lsof /dev/${vol};
			echo_green "fdisk"; sudo fdisk -l /dev/${vol};
			echo_green "df -h";  df -h | grep /${vvv}
			echo_green "resize2fs"; sudo resize2fs /dev/${vol};
			echo_green "mounts"; sudo findmnt | grep /dev/${vol};
			echo_green "df -h";  df -h | grep /${vvv}
		elif [[ $1 == par* ]]; then
			echo "param: s - server, c - client"
			[[ $2 == s* ]] && module=nvmeibs || module=nvmeibc;
			echo_title "/sys/module/$module/parameters";
			if [ -d "/sys/module/$module/parameters" ]; then
				ls /sys/module/$module/parameters/ | while read par; do printf "%-64s" "$par"; cat /sys/module/$module/parameters/$par; done;
			fi
		elif [[ $1 == ver* ]]; then
			echo_title "Modules";
			for rpm in "nvmesh-base" "nvmesh-client" "nvmesh-target" "nvmesh-core" "nvmesh-util"; do
				echo "${rpm}.rpm     \"commit\" : \"$(rpm -qi ${rpm} | grep 'Commit' | awk '{print $NF}')\"";
			done
			if [ -d "/proc/nvmeiba" ]; then
				cat /proc/nvmeib*/version;
				if [ `((cat /proc/nvmeib*/version | grep 'atom\|clnt\|srvr' | awk '{print $6 $12}' | sed 's/[",}]/ /g' | sed 's/^[ \t]*//;s/[ \t]*$//' | tr -s " " | sort) && (rpm -qi nvmesh-base | grep 'Commit\|Version' | awk '{print $NF}' | sed '1!G;h;$!d' | sed 's/^0*//' | xargs))  | uniq | wc -l` -gt 1 ]; then
					echo_red "Versions dont match";
				else
					echo_green "Versions Match";
				fi
				echo_title "usage";
				ls -d /sys/module/nvmeib* | while read mod; do echo -n "Refcount: $mod="; tail -n +1 $mod/refcnt; ls $mod/holders/; echo "---"; done;
			else
				echo_red "NVMESH not running";
			fi
			mgmt_file=/${NVMESH_DIR_LOG}/management.out
			if test -f "$mgmt_file"; then
				echo_title "Mgmt"; head -4 $mgmt_file | grep -o "INFO.*" | paste -sd "|" -;
			else
				echo "Mgmt: not running";
			fi
			echo_title "Conf: $nvmesh_conf_path";
			grep -v -e '^#' -e '^[[:space:]]*$' $nvmesh_conf_path;
			echo_title "Conf: ${nvmesh_conf_modprobe_path}";
			grep -v -e '^#' -e '^[[:space:]]*$' ${nvmesh_conf_modprobe_path};
			echo_title ".ko files";
			sudo modinfo nvmeiba 2>/dev/null | grep filename | cut -d' ' -f2-;
			sudo modinfo nvmeibc 2>/dev/null | grep filename | cut -d' ' -f2-;
			sudo modinfo nvmeibs 2>/dev/null | grep filename | cut -d' ' -f2-;
			echo_title "Tracing";
			echo -e "\ttsc_khz=`cat /proc/nvmeib/tracer/sinfo`=tsc_offset";
			echo_title "Installed Kernels";
			echo -e "\tpage_size=`getconf PAGESIZE`";
			[[ -z `which grubby` ]] && return 0;	# No grubby info
			sudo grubby --info=ALL;
			sudo grubby --default-index; # grubby --set-default 1
		elif [[ $1 == dme* ]]; then
			sudo dmesg --clear;
			clear;
			sudo bash -c 'echo 0 > /sys/module/nvmeibc/parameters/num_warnings';
			echo_title "Watching dmesg";
			dmesg -wTL;
		elif [[ $1 == ioct* ]]; then
			clear; echo_title "Supported ioctls";
			sudo bash -c "echo -n '@help' > /proc/nvmeibc/cli/cli";
			sleep 1s;
			sudo journalctl --since "4 seconds ago" | grep -o -e '__handle_ioctl.*' -e '__help.*' --color=never
		elif [[ $1 == io* ]]; then
			[ "$2" == "hdr" ] && echo "                |                READ               WRITE                TRIM";
			ls /proc/nvmeibc/volumes/*/iostats | while read arg; do
				echo_title "$arg"; cat $arg | grep -e num_ops -e 'worst_e2e ';
			done;
		elif [[ $1 == loc* ]]; then
			echo_title "LOCAL DISKS";
			sudo nvme list;
			tail -n +1 /proc/nvmeibs/smart*
			echo_title "Numa";
			sudo lspci -vvv | grep -E 'Non-V|Mell.*Family' -A 10 | grep -E 'Non-V|Mell.*Family|NUMA'
		elif [[ $1 == trans* ]]; then
			echo "--------------- Translate Addr -----------------";
			if [ "$#" -lt 4 ]; then
				echo "Params: <translation_type> <volume name> <args..>";
				echo -e "\033[0;32mVLBA->DLBA\033[0m:"
				echo "Params Example: vlba vol1 12071681";
				echo -e "\033[0;32mDLBA->{VLBA/CLBA/RLBA}\033[0m:"
				echo "Deprecated    : dlba   vol1 0 0 1 52602432";
				echo "Params Example: dlba2v vol1 1 2 7 52602432";
				echo "Params Example: dlba2c vol1 3 0 1 526032";
				echo "Params Example: dlba2r vol1 0 0 1 3136832";
				echo -e "\033[0;32mTXBM pack/unpack\033[0m:"
				echo "Params Example: txbm vol1 0x700z";
				echo "Params Example: txbm vol1 0x40u";
				echo "--------------- Raw ioctls -----------------";
				sudo bash -c "echo -n '@help' > /proc/nvmeibc/cli/cli";
				sudo journalctl --since "3 seconds ago" | grep nvmeshioctl | grep translate
				return 0;
			elif [[ $2 == t* ]]; then
				cmd="#$3|translate_txbm=$4"
				echo "giving cmd: $cmd"
				sudo bash -c "echo \"$cmd\" > /proc/nvmeibc/cli/cli"
			elif [[ $2 == dlba2* ]]; then
				cmd="#$3|translate_${2} sgmnt=($4,$5,$6) dlba=$7"
				echo "giving cmd: $cmd"
				sudo bash -c "echo \"$cmd\" > /proc/nvmeibc/cli/cli"
			elif [[ $2 == dlba* ]]; then
				cmd="#$3|translate_dlba sgmnt=($4,$5,$6) dlba=$7"
				echo "giving cmd: $cmd"
				sudo bash -c "echo \"$cmd\" > /proc/nvmeibc/cli/cli"
			elif [[ $2 == vlba* ]]; then
				cmd="#$3|translate_addr=$4,1,"
				echo "giving cmds: ${cmd}C		${cmd}L"
				sudo bash -c "echo \"${cmd}C\" > /proc/nvmeibc/cli/cli"
				sudo bash -c "echo \"${cmd}L\" > /proc/nvmeibc/cli/cli"
				#sudo bash -c 'echo "#$3|translate_addr=$4,1,C" > /proc/nvmeibc/cli/cli'
				#sudo bash -c 'echo "#$3|translate_addr=$4,1,L" > /proc/nvmeibc/cli/cli'
			else
				echo "wrong params, type help"
				return 0;
			fi
			sudo journalctl --since "3 seconds ago" | grep nvmeshioctl;
		elif [[ $1 == vol* ]]; then
			echo_title "Volumes[atom]";
			cat /proc/nvmeiba/status | grep 'path=' --color=never;
			echo_title "Volumes[clnt]";
			if [[ $(ls /proc/nvmeibc/volumes/) ]]; then
				ls /proc/nvmeibc/volumes/*/status | while read vvv; do
					printf "%-64s" "$vvv"; echo -n "--> "; cat $vvv | grep 'Device status' --color=never;
				done;
			fi;
			if ls /proc/mc* 1> /dev/null 2>&1; then
				if ls /proc/mc*/volumes/* 1> /dev/null 2>&1; then
					ls /proc/mc*/volumes/*/status | while read vvv; do
						printf "%-64s" "$vvv"; echo -n "--> "; cat $vvv | grep 'Device status' --color=never;
					done;
				fi;
			fi;
		elif [ "$1" == "jam" ]; then
			echo "-------------- JAM ------------------";
			if ls /proc/nvmeibc/jam/* 1> /dev/null 2>&1; then
				tail -n +1 /proc/nvmeibc/jam/*
			fi;
		elif [ "$1" == "utils" ]; then
			echo "Source for utils: git/perfTest/io_stress/"
			echo -e "scan locks: Analyzes blockset entry on server side."
			echo -e "\tscan locks examples:"
			echo -e "\tsudo watch -n 0.5 -d '${NVMESH_DIR_SRC}/common-repo/tools/scan_locks_ec -filter=1{S3HCNX0JC01989.1=1:0x0b83d-0x0b85d}* -verbose -conf_d 8 -conf_p 2 -conf_role 3'"
			echo -e "\tsudo ${NVMESH_DIR_SRC}/common-repo/tools/scan_locks_ec -filter=1{S3HCNX0JC01989.1=1:0x1208b-0x120af}* -reset -set_txid 0x0 -set_dbits 0xFF0"
			echo -e "cmp_blocks: Analizes integrity of EC/R1 slice/blockset."
			echo -e "\tUsage: Check edic. Fix single error when 2 parities exist, generate parities"
			echo -e "\tLauch utility to get help about its"
			echo -e "di_parse: parses binary encoded debug info inside 4[KB] block."
			echo -e "\tUsage: Only when debug di is enabled"
			echo -e "gen_md: Generate metadata for manually roll-forwrd blocks and fixups of corrupted slices."
			echo -e "\tUsage: by Infra scripts. Activated by support guys"
			echo -e "manual_fops: Testing utility to take reference on block devices and issue controleld IO"
			echo -e "\tUsage: QA/ Unitests"
			echo "Directory may include other tools"
		elif [[ $1 == larg* ]]; then
			echo "-------------- Showing largest 100 files ------------------";
			sudo du -a / | sort -n -r | head -n 100;
			#ncdu
		elif [[ $1 == struct* ]]; then
			sudo bash -c "echo -n '#|show_struct_size' > /proc/nvmeibc/cli/cli";
			sudo journalctl --since "4 seconds ago" | grep -oe "__show_struct_size.*" --color=never;
		elif [[ $1 == sta* ]]; then
			STACK_FILE="nvmeib_stack.txt"
			echo "Searching all processes related to nvmesh to file ${STACK_FILE}";
			sudo grep nvmeib /proc/*/stack > ${STACK_FILE};
			CPROCS=`grep nvmeibc ${STACK_FILE} | grep "/proc/[0-9]*/stack" -o | uniq`;
			echo_title "Client processes";
			echo "$CPROCS" | grep "[0-9]*" -o;
			sudo head -n 10 $CPROCS;
			#grep nvmeibc ${STACK_FILE} | less -N;
		elif [[ $1 == prof* ]]; then
			echo "echo 1 > /sys/module/nvmeibc/parameters/profiling_enabled";
			VVV="vol_name"; [ ! -z "$2" ] && VVV=$2;
			cmd="${MY_PROJECTS_DIR}/$cur_branch/clnt/block/datapath_utils_generic/profiling/nvmesh_profiling.py -v $VVV"; echo ${cmd}; eval ${cmd};
		else
			echo "modinfo|params|version|profiler|local|jam|translate|ioctls|iostats|vol_stats|large_files|utils|dmesg|struct_size|stack|size_vol"
		fi
	}

	function NVMESH_clean_local() {
		echo "********* Cleaning *.[d|i|o|gch|log|trace|pp] from `pwd` ***************"
		echo -n "Size before = ";  du -hcs . | grep total
		find . -type f -name "*.[doi]" -exec rm -f {} \;
		find . -type f -name "*.ko" -exec rm -f {} \;
		find . -type f -name "gen*events*" -exec rm -f {} \;
		sudo bash -c 'find . -type f -name "*.trace.json" -exec rm -f {} \;'
		sudo bash -c 'find . -name .trace_pp_dir -exec rm -rf {} \;'
		find . -type f -name "*.gch" -exec rm -f {} \;
		find . -type f -name "*log" -exec rm -f {} \;
		echo -n "Size after = ";  du -hcs . | grep total
	}
fi
