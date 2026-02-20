#!/usr/bin/env bash

# Config file (default: build_sh_conf) may contain all build options.
# Alternatively some or all options may be supplied on command line.
#
# *** config file format ***
# *** per-server options ***
# REMOTE_SERVERS="[<hostname>:[<option>=<value>:...] ...]"
#   supported options:
#     IM_SERVER=yes
#     IM_CLIENT=yes
#     IM_BOTH=yes
#     MK_RPM=yes
#     COMPRESS_KO=yes
#     RPM_BUILD_TYPE=kmod
#   example: REMOTE_SERVERS="nvme4:IM_SERVER=yes nvme5:IM_BOTH=yes:MK_RPM=yes"
#
# *** global options ***
# SSH_INVOKE="<ssh-cmd-line>"
#   example: SSH_INVOKE="sshpass -e ssh -l root"
#            use sshpass to login as user "root" with password in ${SSHPASS}
#   example: SSH_INVOKE="sshpass -p Pa$$w0rd ssh -l root"
#            the same as above but with the password suuplied in cmd line
# PRIV_SSH="<srv>:<ssh-cmd-line>|<srv>:<ssh-cmd-line>..."
#   override global SSH_INVOKE per server
#   example: PRIV_SSH="nvme4:ssh|nvme5:sshpass -e ssh -l root"
# REMOTE_DIR=<remote_dir_name>
#   example: REMOTE_DIR=projects/${BRANCH_NAME}
# MAKE_OPTS=<list_of_make_args>, to be passed to remote make
#   example: MAKE_OPTS="-j clean"
# RSYNC_OPTS=<list_of_rsync_args>, to be added to default ones
#   example: RSYNC_OPTS="--compress"
# EXCLUDES=<file-name-with-rsync-exclude-patterns>
#   example: EXCLUDES=excludes
# PARALLEL=(true|false), default=false
#   example: PARALLEL=true
# FILTER_OUT=(true|false), default=false
#   example: FILTER_OUT=true
# DRY_RUN=(true|false), default=false
#   example: DRY_RUN=true
# REPO_DIR=<local-build-repo-dir>, used for tagged servers only
#	example: REPO_DIR="~/build_repo"
# REPO_RPMS=(true|false), default=false, copy RPMs to local repo
#   example: REPO_RPMS=true
# REPO_SYMS=(true|false), default=false, copy symbols to local repo
#   example: REPO_SYMS=true
# REPO_MODS=(true|false), default=false, copy modules to local repo
#   example: REPO_MODS=true

source build_common.sh
build_started=false

exit_build()
{
	now=`date`
	echo -en "\n$1 at $now - Exiting...\n"

	if [[ ${num_servers} < 1 ]]; then exit 1; fi
	if [ ${build_started} = false ]; then exit 0; fi

	echo "-----------------------------------------------------------------"
	for ((i=0; i<num_servers; i++)); do
		echo "$((i+1)): ${server_name[$i]} err:${build_err[${i}]} log_file: ${log_file[${i}]}"
	done
	echo "-----------------------------------------------------------------"

	local exit_val=0
	for ((i=0; i<num_servers; i++)); do
	    if [ ${build_err[${i}]} -ne 0 ]; then exit_val=1; fi
	done

	exit ${exit_val}
}

trim_str()
{
	local str="$@"
	str="${str%%+([[:space:]])}"
	str="${str##+([[:space:]])}"
	echo -n "${str}"
}

# process command line opts, if any
usage()
{
	echo -e "Usage: $(basename $0) [options]\n"
	echo -e "options:"
	echo -e "\t[-c|--conf] <src-conf-file-name>"
	echo -e "\t[-g|--gen] <dst-generated-conf-file-name>"
	echo -e "\t[-l|--log-dir] <local-logs-dir>"
	echo -e "\t[-s|--ssh] <ssh-cmd>"
	echo -e "\t[--priv-ssh] <server:ssh-cmd>"
	echo -e "\t[-m|--make] <extra-make-opts>"
	echo -e "\t[-r|--rsync] <extra-rsync-opts>"
	echo -e "\t[-e|--exclude] <rsync-exclude-patterns-file>"
	echo -e "\t[-d|--dir] <remote-build-dir>"
	echo -e "\t[-R|--repo] <local-repo-dir>"
	echo -e "\t[-S|--server] <tag@server[:make-opts:...]>"
	echo -e "\t[--repo-all] = {[--repo-rpms][--repo-mods][--repo-syms]}"
	echo -e "\t[-P|--parallel]"
	echo -e "\t[-F|--filter]"
	echo -e "\t[-D|--dry]"
	echo -e "\t[--commit-id]"
	echo -e "\t[--full-commit-id]"
	echo -e "\t[--change-id]"
	echo -e "\t[--build-dir]"
	echo -e "\t[--branch-name]"
	echo -e "\t[--git-describe]"
	echo -e "\t[--spdk-commit-id]"
	echo -e "\t[--spdk-change-id]"
	echo -e "\t[--spdk-branch-name]"
	echo -e "\t[--spdk-git-describe]"
	echo -e "\t[--sign-rpm]"
	echo -e "\t[--compile-pager]"
	echo -e "\t[--compile-nvmft]"
	echo -e "\t[--run-local]"
	echo -e "\t[--compilator]"
	echo -e "\t[--sector-shift X]"
	echo -e "\t[-h|--help]"
    exit 1
}

SHORT_OPTIONS="hPFD" # no args
SHORT_OPTIONS+="c:s:l:m:r:e:d:R:S:g:" # with arg

LONG_OPTIONS="help,parallel,filter,dry,run-local,sign-rpm,compile-nvmft" # no args
LONG_OPTIONS+=",repo-all,repo-rpms,repo-mods,repo-syms,compilator" # no short opts
LONG_OPTIONS+=",conf:,ssh:,log-dir:,make:,rsync:,exclude:,dir:,repo:,server:,gen:" # with arg
LONG_OPTIONS+=",priv-ssh:" # private (per-server) cmds, with arg
LONG_OPTIONS+=",commit-id:,full-commit-id:,change-id:,build-dir:,branch-name:,git-describe:,sector-shift:,compile-pager:,spdk-commit-id:,spdk-change-id:,spdk-branch-name:,spdk-git-describe:,no-python-executables"


options=$(getopt -o ${SHORT_OPTIONS} -l ${LONG_OPTIONS} -- "$@")
if [ $? -ne 0 ]; then
        # something went wrong, getopt should have printed an error message
        usage
fi

if [ `uname -s` = "Darwin" ]; then
	echo 'will not run eval set -- ${options} as it will probably fail - need to find a workaround that will work to everyone'
else
	eval set -- ${options}
fi

CREATE_PYTHON_TOOLS_EXEC="yes"

while [ $# -gt 1 ]; do
	case $1 in
		-h|--help) usage; ;;
		-P|--parallel) CMD_PARALLEL=true; ;;
		-F|--filter) CMD_FILTER_OUT=true; ;;
		-D|--dry) CMD_DRY_RUN=true; ;;
		--repo-rpms) CMD_REPO_RPMS=true; ;;
		--repo-mods) CMD_REPO_MODS=true; ;;
		--repo-syms) CMD_REPO_SYMS=true; ;;
		--compilator) IS_COMPILATOR=true; ;;
		--repo-all)
			CMD_REPO_RPMS=true;
			CMD_REPO_MODS=true;
			CMD_REPO_SYMS=true; ;;
		# for options with required arguments, an additional shift is required
		-c|--conf) CMD_CONFIG_FILE="$2"; shift ;;
		-l|--log-dir) CMD_LOG_DIR="$2"; shift ;;
		-s|--ssh) CMD_SSH_INVOKE="$2"; shift ;;
		--priv-ssh)
			[ -n "${CMD_PRIV_SSH}" ] && CMD_PRIV_SSH+="|";
			CMD_PRIV_SSH+="`trim_str $2`";
			shift ;;
		-m|--make) CMD_MAKE_OPTS+=" $2"; shift ;;
		-r|--rsync) CMD_RSYNC_OPTS+=" $2"; shift ;;
		-e|--exclude) CMD_EXCLUDES+="$2"; shift ;;
		-d|--dir) CMD_REMOTE_DIR="$2"; shift ;;
		-R|--repo) CMD_REPO_DIR="$2"; shift ;;
		-S|--server) CMD_REMOTE_SERVERS+=" `trim_str $2`"; shift ;;
		-g|--gen) CMD_GENERATE=true; CMD_GEN_CONFIG_FILE="$2"; shift ;;
		--commit-id) COMMIT_ID="$2"; shift ;;
		--full-commit-id) FULL_COMMIT_ID="$2"; shift ;;
		--change-id) CHANGE_ID="$2"; shift ;;
		--build-dir) GIT_TOP_DIR="$2"; shift ;;
		--branch-name) BRANCH_NAME="$2"; shift ;;
		--git-describe) GIT_DESCRIBE="$2"; shift ;;
		--spdk-commit-id) SPDK_COMMIT_ID="$2"; shift ;;
		--spdk-change-id) SPDK_CHANGE_ID="$2"; shift ;;
		--spdk-branch-name) SPDK_BRANCH_NAME="$2"; shift ;;
                --spdk-git-describe) SPDK_GIT_DESCRIBE="$2"; shift ;;
		--sector-shift) SECTOR_SHIFT="$2"; shift ;;
		--ofed-sym-ver) OFED_SYM_VER="$2"; shift ;;
		--sign-rpm) SIGN_RPM=true ;;
		--compile-pager) COMPILE_PAGER="$2"; shift ;;
		--compile-nvmft) COMPILE_NVMFT=true ;;
		--run-local) RUN_LOCAL=true ;;
		--core-unitest) CMD_CORE_UNITEST=yes ;;
		--no-python-executables) CREATE_PYTHON_TOOLS_EXEC=no ;;
		(--) shift; break ;;
		(-*) echo "$0: error - unrecognized option $1" 1>&2; usage; ;;
		(*) break ;;
	esac
	shift
done

arr_entries_are_unique()
{
	local a=($@) # transform array entries string to array
	local -A b # associative array
	local i
	for i in ${a[@]}; do b[${i}]=1; done
	# duplicate entries in would override the same entry in b
	if [[ ${#b[@]} == ${#a[@]} ]]; then
		return 0
	else
		return 1
	fi
}

arr_entries_leave_unique()
{
	local a=($@) # transform array entries string to array
	local -A b # associative array
	local i
	local s
	for i in ${a[@]}; do
		if [[ -z "${b[${i}]}" ]]; then
			b[${i}]=1
			s+="${i} " # save unique entries in their original order
		fi
	done
	echo "${s% }" # trim trailing whitespace
}

arr_entries_sort_for_make()
{
	local a=($@) # transform array entries string to array
	local s # accumulated sorted entries here
	for ((i=0; i<${#a[@]}; i++)) {
		# save all entries starting with - (compilation flags)
		if [[ ${a[$i]} == -* ]]; then s+="${a[$i]} "; fi
	}
	for ((i=0; i<${#a[@]}; i++)) {
		# save all entries of x=y form (symbol definitions)
		if [[ ${a[$i]} == [!-]*=* ]]; then s+="${a[$i]} "; fi
	}
	for ((i=0; i<${#a[@]}; i++)) {
		# save all remaining entries
		if [[ ${a[$i]} != -* &&  ${a[$i]} != [!-]*=* ]]; then s+="${a[$i]} "; fi
	}
	echo "${s% }" # trim trailing whitespace
}

remove_duplicated_params()
{
		# get entries array and ignore global ones when set for a server
        local a=($@) # transform array entries string to array
        local b
        local i
        local s
        local v

        for i in ${a[@]}; do
                b=$(echo $i | grep = | cut -d'=' -f1)
                if [[ -n $b ]]; then
                        v=$(echo $s | grep -q "${b}")
                        rc=$?
                        if [[ $rc -ne 0 ]]; then
                                s+="${i} "
                        fi
                else
                        s+="${i} "
                fi
        done
        echo "${s% }" # trim trailing whitespace
}

server_index_by_name()
{
	local name="$1"
	for ((i=0; i<num_servers; i++)) {
		if [ "${server_name[${i}]}" == "${name}" ]; then
			echo "${i}"
			break
		fi
	}
}


if [ -z "$BRANCH_NAME" ] ; then
	BRANCH_NAME=$(git symbolic-ref --short --quiet HEAD) || BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
fi

if [ -z "$COMMIT_ID" ] ; then
	COMMIT_ID=$(git_commit_id)
fi

if [ -z "$FULL_COMMIT_ID" ] ; then
	FULL_COMMIT_ID=$(git rev-parse HEAD)
fi

if [ -z "$CHANGE_ID" ] ; then
	CHANGE_ID=$(git log -n1 --format=%b | awk '/^Change-Id: / {print $2}')
fi

if [ -z "$GIT_TOP_DIR" ] ; then
	GIT_TOP_DIR=$(git rev-parse --show-toplevel)
fi

if [ -z "$GIT_DESCRIBE" ] ; then
	GIT_DESCRIBE=$(git describe)
fi

# removing the v prefix of the version number
GIT_DESCRIBE=$(echo $GIT_DESCRIBE | cut -c 2-)

if [ -z "$RUN_LOCAL" ] ; then
	RUN_LOCAL=false
fi

if [ -z "$COMPILE_NVMFT" ]; then
	COMPILE_NVMFT=false
fi


if [ ! -z "$SECTOR_SHIFT" ] ; then
	CMD_MAKE_OPTS+=" SECTOR_SHIFT=$SECTOR_SHIFT"
fi

IFS='-' read -ra git_describe <<< "$GIT_DESCRIBE"

# make sure we start in the top dir of the git repo and include config
cd "${GIT_TOP_DIR}"

CONFIG_FILE=${CMD_CONFIG_FILE:-${GIT_TOP_DIR}/build_sh_conf}
if [ -r ${CONFIG_FILE} ]; then
	source ${CONFIG_FILE}
else
	if [ -n "${CMD_CONFIG_FILE}" ]; then
		echo -e "\nUser-supplied config file not found or unreadable:"
		echo -e "--> ${CONFIG_FILE}"
		echo -e "Exiting...\n"
		exit 1
	else
		echo -e "\nDefault config file not found or unreadable:"
		echo -e "--> ${CONFIG_FILE}"
		echo -e "Warning: continuing with command-line options only...\n"
		CONFIG_FILE=
	fi
fi

# parse and prepare all per-server opts from conf file + cmd line
REMOTE_SERVERS+="${CMD_REMOTE_SERVERS}"
i=0
for rs in ${REMOTE_SERVERS}; do
	srv_delim=${rs//@/ } # replace @ with space
	token_arr=(${srv_delim})
	if [[ ${#token_arr[@]} == 1 ]]; then # probably no tag
		if [[ "${srv_delim}" != "${rs}" ]]; then
			exit_build "Invalid server conf string: \"${rs}\" (empty tag or server name?)"
		fi
		build_tag[$i]=""
	elif [[ ${#token_arr[@]} == 2 ]]; then # tag + name:<flags>
		build_tag[$i]="${token_arr[0]}"
		srv_delim="${token_arr[1]}"
	else # multiple @ - invalid format
		exit_build "Invalid server conf string: \"${rs}\" (multiple tags?)"
	fi

	srv_delim=${srv_delim//:/ } # replace all : with spaces
	token_arr=(${srv_delim})
	server_name[$i]="${token_arr[0]}"
	if [ -z "server_name[$i]" ]; then
		exit_build "Invalid server conf string:	\"${rs}\" (empty server name?)"
	fi
	if [[ ${#token_arr[@]} == 1 ]]; then # no flags
		make_flags[$i]=""
	else
		make_flags[$i]="${token_arr[@]:1}" # drop the first element
	fi

	child_pid[${i}]=0
	build_err[${i}]=0
	((i++))
done
num_servers=${i}

# set default opts; conf file overrides most of them
SSH_INVOKE=${SSH_INVOKE:-ssh}
PRIV_SSH=${PRIV_SSH:-}
EXCLUDES=${EXCLUDES:-excludes} # RSYNC_OPTS depends on this
REMOTE_DIR=${REMOTE_DIR:-"${BRANCH_NAME}/${COMMIT_ID}"}
LOG_DIR=${LOG_DIR:-/tmp}
PARALLEL=${PARALLEL:-false}
FILTER_OUT=${FILTER_OUT:-false}
DRY_RUN=${DRY_RUN:-false}
REPO_DIR=${REPO_DIR:-${HOME}/build_repo}
REPO_RPMS=${REPO_RPMS:-false}
REPO_SYMS=${REPO_SYMS:-false}
REPO_MODS=${REPO_MODS:-false}
SYNC_ONLY=${SYNC_ONLY:-false}
MAKE_CMD=${MAKE_CMD:-make}
VERBOSE=${VERBOSE:-false}
IS_COMPILATOR=${IS_COMPILATOR:-false}

# for some opts cmd line values override everything else
SSH_INVOKE=${CMD_SSH_INVOKE:-${SSH_INVOKE}}
EXCLUDES=${CMD_EXCLUDES:-${EXCLUDES}} # RSYNC_OPTS depends on this
REMOTE_DIR=${CMD_REMOTE_DIR:-${REMOTE_DIR}}
LOG_DIR=${CMD_LOG_DIR:-${LOG_DIR}}
PARALLEL=${CMD_PARALLEL:-${PARALLEL}}
FILTER_OUT=${CMD_FILTER_OUT:-${FILTER_OUT}}
DRY_RUN=${CMD_DRY_RUN:-${DRY_RUN}}
CMD_GENERATE=${CMD_GENERATE:-false}
REPO_DIR=${CMD_REPO_DIR:-${REPO_DIR}}
REPO_RPMS=${CMD_REPO_RPMS:-${REPO_RPMS}}
REPO_SYMS=${CMD_REPO_SYMS:-${REPO_SYMS}}
REPO_MODS=${CMD_REPO_MODS:-${REPO_MODS}}
CORE_UNITEST=${CMD_CORE_UNITEST:-${CORE_UNITEST}}

# for some opts add cmd line arg value
RSYNC_OPTS+=${CMD_RSYNC_OPTS}
MAKE_OPTS+=${CMD_MAKE_OPTS}
if [ -n "${CMD_PRIV_SSH}" ]; then
	[ -n "${PRIV_SSH}" ] && PRIV_SSH+="|"
	PRIV_SSH+="${CMD_PRIV_SSH}"
fi

if [ -n "${CORE_UNITEST}" ]; then
	MAKE_OPTS+=" CORE_UNITEST=${CORE_UNITEST}"
fi

# filter out possible duplicate entries
RSYNC_OPTS=$(arr_entries_leave_unique "${RSYNC_OPTS}")
MAKE_OPTS=$(arr_entries_leave_unique "${MAKE_OPTS}")
MAKE_OPTS=$(arr_entries_sort_for_make "${MAKE_OPTS}")

if $COMPILE_NVMFT; then
	if ! $IS_COMPILATOR; then
		SPDK_BRANCH_NAME="master"
		echo "Cloning NVMfT/spdk repo branch $SPDK_BRANCH_NAME"
		git clone -b "$SPDK_BRANCH_NAME" git@gitlab.excelero.com:excelero/spdk.git
		cd spdk
	fi

	if [ -z "$SPDK_COMMIT_ID" ]; then
		SPDK_COMMIT_ID=$(git log -n1 --format=%h)
        	if [ -z "$SPDK_COMMIT_ID" ]; then
                	SPDK_COMMIT_ID=unknown
	        fi
	fi

	if [ -z "$SPDK_CHANGE_ID" ]; then
	        SPDK_CHANGE_ID=$(git log -n1 --format=%b | awk '/^Change-Id: / {print $2}')
		if [ -z "$SPDK_CHANGE_ID" ] ; then
			SPDK_CHANGE_ID=unknown
		fi
	fi

	if [ -z "$SPDK_GIT_DESCRIBE" ]; then
		SPDK_GIT_DESCRIBE=$(git describe | cut -c 2-)
	fi

        IFS='-' read -ra spdk_git_describe <<< "$SPDK_GIT_DESCRIBE"
        SPDK_VERSION=${spdk_git_describe[0]}
        SPDK_RELEASE="${spdk_git_describe[1]}.${spdk_git_describe[3]}"

	if [ -z "$SPDK_VERSION" ] || [ -z "$SPDK_RELEASE" ]; then
		SPDK_VERSION="1.0.0"
		SPDK_RELEASE="0"
	fi

	if ! $IS_COMPILATOR; then
		cd -
	fi
fi

for ((i=0; i<${num_servers}; i++)); do
	# set global default for ssh invoke cmd
	ssh_invoke[${i}]="${SSH_INVOKE}"
	# set log file name
	log_file[${i}]="${LOG_DIR}/${server_name[$i]}.build"
done

if [ -n "${PRIV_SSH}" ]; then
	# parse private (per-server) ssh invoke cmds and override where required
	OIFS=${IFS}
	IFS="|"
	ssh_cmds=(${PRIV_SSH}) # separate into per-server entries
	IFS=${OIFS}

	if [[ ${#ssh_cmds[@]} == 0 ]]; then # per-server options supplied
		exit_build "Invalid per-server ssh invoke cmd:\"${PRIV_SSH}\""
	fi

	for ((i=0; i<${#ssh_cmds[@]}; i++)); do
	    ssh_srv="${ssh_cmds[${i}]%:*}" # server name before :
	    ssh_cmd="${ssh_cmds[${i}]#*:}" # ssh cmd after :
		srv_ind=`server_index_by_name ${ssh_srv}`
		if [ -z "${srv_ind}" ]; then
			echo -en "Private ssh invoke cmds:\n\t\"${PRIV_SSH}\"\n"
			exit_build "Invalid server name ${ssh_srv} in ssh invoke cmd:\"${ssh_cmds[${i}]}\""
		fi
		ssh_invoke[${srv_ind}]="${ssh_cmd}"
	done
fi

# generate config file if requested
if [ ${CMD_GENERATE} = true ]; then
	if [ ${DRY_RUN} = false ]; then
		GEN_CONFIG_FILE=${CMD_GEN_CONFIG_FILE}
	else
		GEN_CONFIG_FILE="/dev/tty"
	fi

	echo "# build.sh configuration file" > ${GEN_CONFIG_FILE}
	echo -n "# auto-generated " >> ${GEN_CONFIG_FILE}
	echo -n "at host:`hostname` by user:`whoami`, " >> ${GEN_CONFIG_FILE}
	echo -n "`date '+%a %d-%b-%y %H:%M:%S'`" >> ${GEN_CONFIG_FILE}
	echo -e "\n" >> ${GEN_CONFIG_FILE}

	echo -n "REMOTE_SERVERS=\"" >> ${GEN_CONFIG_FILE}
	for ((i=0; i<num_servers; i++)); do
		if [[ $i > 0 ]]; then echo -n " " >> ${GEN_CONFIG_FILE}; fi
		if [ -n "${build_tag[$i]}" ]; then
			echo -n "${build_tag[$i]}@" >> ${GEN_CONFIG_FILE}
		fi
		echo -n "${server_name[$i]}" >> ${GEN_CONFIG_FILE}
		if [ -n "${make_flags[$i]}" ]; then
			echo -n ":${make_flags[$i]// /:}" >> ${GEN_CONFIG_FILE}
		fi
	done
	echo "\"" >> ${GEN_CONFIG_FILE}

	echo "SSH_INVOKE=\"${SSH_INVOKE}\"" >> ${GEN_CONFIG_FILE}
	echo "PRIV_SSH=\"${PRIV_SSH}\"" >> ${GEN_CONFIG_FILE}
	echo "LOG_DIR=\"${LOG_DIR}\"" >> ${GEN_CONFIG_FILE}
	echo "REMOTE_DIR=\"${REMOTE_DIR}\"" >> ${GEN_CONFIG_FILE}
	echo "EXCLUDES=\"${EXCLUDES}\"" >> ${GEN_CONFIG_FILE}
	echo "RSYNC_OPTS=\"${RSYNC_OPTS}\"" >> ${GEN_CONFIG_FILE}
	echo "MAKE_OPTS=\"${MAKE_OPTS}\"" >> ${GEN_CONFIG_FILE}
	echo "PARALLEL=\"${PARALLEL}\"" >> ${GEN_CONFIG_FILE}
	echo "FILTER_OUT=\"${FILTER_OUT}\"" >> ${GEN_CONFIG_FILE}
	echo "CORE_UNITEST=\"${CORE_UNITEST}\"" >> ${GEN_CONFIG_FILE}

	if [ ${DRY_RUN} = false ]; then
		exit_build "Configuration file ${CMD_GEN_CONFIG_FILE} generated"
	else
		echo -e "\nConfiguration file to be generated: ${CMD_GEN_CONFIG_FILE}\n"
	fi
fi

if ( $VERBOSE ); then
	RSYNC_OPTS+=" -v"
fi

# add last arguments (invariant or indirectly configurable)
RSYNC_OPTS+=" --ignore-errors --exclude-from=${EXCLUDES} -rlpgoDz --checksum"
MAKE_OPTS+=" all COMMIT_ID=${COMMIT_ID} GIT_COMMIT_ID=${FULL_COMMIT_ID} BRANCH_NAME=${BRANCH_NAME} VERSION=${git_describe[0]} RELEASE=${git_describe[1]} VERBOSE=${VERBOSE} IS_COMPILATOR=${IS_COMPILATOR} IS_TOMA_FIRST=${IS_TOMA_FIRST} BUILD_NUMBER=${BUILD_NUMBER}"

if [ ! -z $SIGN_RPM ]; then
	MAKE_OPTS+=" SIGN_RPM=${SIGN_RPM}"
	if $SIGN_RPM; then
		SPDK_SIGN_RPM_FLAG="--sign-rpm"
	else
		SPDK_SIGN_RPM_FLAG=""
	fi
fi

if [ "$COMPILE_PAGER" == "no" ]; then
	MAKE_OPTS+=" BUILD_PAGER=${COMPILE_PAGER}"
fi

SECTOR_SHIFT=`echo "$MAKE_OPTS" | grep -o -e SECTOR_SHIFT=[0-9]* | cut -d "=" -f2`

if [ -z $SECTOR_SHIFT ]; then
	SECTOR_SHIFT=12
	MAKE_OPTS+=" SECTOR_SHIFT=${SECTOR_SHIFT}"
fi
BLOCK_SIZE=$(( 2 ** $SECTOR_SHIFT ))

if [ "$BLOCK_SIZE" -gt "1024" ]; then
	BLOCK_SIZE=$(( $BLOCK_SIZE / 1024 ))KB
else
	BLOCK_SIZE+=B
fi
MAKE_OPTS+=" BLOCK_SIZE=${BLOCK_SIZE}"

if [ "$CREATE_PYTHON_TOOLS_EXEC" == "yes" ]; then
	MAKE_OPTS+=" CREATE_PYTHON_TOOLS_EXEC=yes"
fi

# filter out possible duplicate entries
RSYNC_OPTS=$(arr_entries_leave_unique "${RSYNC_OPTS}")
MAKE_OPTS=$(arr_entries_leave_unique "${MAKE_OPTS}")
MAKE_OPTS=$(arr_entries_sort_for_make "${MAKE_OPTS}")

# check used tools options support
# On macOS, disable filtering to avoid buffering issues
if [ `uname -s` = "Darwin" ]; then
	FILTER_OUT=false
	SED_UNBUF="sed"
	GREP_UNBUF="grep"
else
	SED_UNBUF="sed --unbuffered"
	`echo | ${SED_UNBUF} 's/$//' 2> /dev/null` || SED_UNBUF="sed"
	GREP_UNBUF="grep --line-buffered"
	`echo | ${GREP_UNBUF} '$' 2> /dev/null` || GREP_UNBUF="grep"
fi

# print build opts final values
echo "Build start: `date '+%a %d-%b-%y %H.%M.%S'`"
echo "Config file: ${CONFIG_FILE}"
echo "Branch: ${BRANCH_NAME}, commit: ${COMMIT_ID}"
echo "Log dir: ${LOG_DIR}"
echo "SSH command: ${SSH_INVOKE}"
echo "Remote dir: ${REMOTE_DIR}"
echo "Local repo dir: ${REPO_DIR}"
echo "Copy to repo: rpms:${REPO_RPMS}, syms:${REPO_SYMS}, modules:${REPO_MODS}"
echo "Filter screen output: ${FILTER_OUT}"
echo "Parallel: ${PARALLEL}"
echo "Excludes: ${EXCLUDES}"
echo "Rsync opts: ${RSYNC_OPTS}"
echo "Make cmd (global): ${MAKE_CMD}"
echo "Make opts (global): ${MAKE_OPTS}"

if [ "${SED_UNBUF}" = "sed" ]; then echo "sed --unbuffered unsupported, using: sed"; fi
if [ "${GREP_UNBUF}" = "grep" ]; then echo "grep --line-buffered unsupported, using: grep"; fi

if [[ ${num_servers} > 1 ]]; then
	echo -e "\n${num_servers} servers: ${server_name[*]}"
elif [[ ${num_servers} == 1 ]]; then
	echo -e "\n1 server: ${server_name[*]}"
else
	exit_build "${num_servers} servers - invalid number"
fi
arr_entries_are_unique "${server_name[@]}" || exit_build "Duplicate server names"

echo "Build tags (per-server):"
for ((i=0; i<num_servers; i++)); do
	if [ -n "${build_tag[${i}]}" ]; then
		echo -e "\t${build_tag[${i}]} @ ${server_name[${i}]}"
	fi
done
echo "Make opts (per-server):"
for ((i=0; i<num_servers; i++)); do
	echo -e "\t${server_name[${i}]}: ${make_flags[$i]}"
done
if [[ ${#ssh_cmds[@]} > 0 ]]; then
	echo "SSH command (per-server):"
	for ((i=0; i<num_servers; i++)); do
		if [ "${ssh_invoke[$i]}" != "${SSH_INVOKE}" ]; then
			echo -e "\t${server_name[${i}]}: ${ssh_invoke[$i]}"
		fi
	done
fi

if [ ${DRY_RUN} = true ]; then exit_build "Dry run requested"; fi
echo

save_ret_val()
{
	build_err[$1]=$2
	return $2
}

wait_children()
{
	local ret_val=0
	for ((i=0; i<num_servers; i++)); do
	    if [ ${child_pid[${i}]} -eq 0 ]; then
			continue
		fi
		wait ${child_pid[${i}]}
		save_ret_val "${i}" "$?"
		if [ $? -ne 0 ]; then
			echo "Failure on: ${server_name[$i]}"
			ret_val=1
		fi
		child_pid[${i}]=0
	done
	return ${ret_val}
}

# trap keyboard interrupt (control-c) and exit
control_c()
{
	for ((i=0; i<num_servers; i++)); do
	    if [ ${child_pid[${i}]} -eq 0 ]; then
			continue
		fi
		kill ${child_pid[${i}]}
	done
	wait_children

	exit_build "Aborted by user"
}

trap control_c SIGINT

make_dir()
{
	local dir="$1"
	if [ -d ${dir} ]; then
		echo "${dir} already exists";
	else
		mkdir -pv ${dir} || echo "Failed mkdir -pv ${dir}";
	fi
}

create_remote_dir()
{
	local server_num=$1
	local ssh_cmd="${ssh_invoke[${server_num}]}"
	local server_name="${server_name[${server_num}]}"
	local log_file="${log_file[${server_num}]}"

	# convert the function to text and invoke remotely
	${ssh_cmd} ${server_name} "$(declare -f make_dir); make_dir ${REMOTE_DIR}" | \
		${SED_UNBUF} -e "s/^/${server_name}: /" | \
		tee ${log_file}

	return ${PIPESTATUS[0]} # exit status of "ssh mkdir"
}

rsync_remote_dir()
{
	local server_num=$1
	local ssh_cmd="${ssh_invoke[${server_num}]}"
	local server_name="${server_name[${server_num}]}"
	local log_file="${log_file[${server_num}]}"

	if [ $RUN_LOCAL = true ] ; then
		destination=~/${REMOTE_DIR}
	else
		destination="${server_name}:${REMOTE_DIR}"
	fi

        echo rsync ${RSYNC_OPTS} . -e "${ssh_cmd}" $destination
	if [ ${FILTER_OUT} = true ]; then
		# leave only lines with keywords
		local filter_in="deleting|sent"
		rsync ${RSYNC_OPTS} . -e "${ssh_cmd}" $destination | \
			${SED_UNBUF} -e "s/^/${server_name}: /" | \
			tee -a ${log_file} | \
			${GREP_UNBUF} -E "(${filter_in})"
	else
		rsync ${RSYNC_OPTS} . -e "${ssh_cmd}" $destination | \
			${SED_UNBUF} -e "s/^/${server_name}: /" | \
			tee -a ${log_file}
	fi

	return ${PIPESTATUS[0]} # exit status of "ssh rsync"
}

invoke_remote_make()
{
	local server_num=$1
	local ssh_cmd="${ssh_invoke[${server_num}]}"
	local server_name="${server_name[${server_num}]}"
	local make_flags="${make_flags[${server_num}]}"
	local log_file="${log_file[${server_num}]}"

	# leave only lines with keywords
	local filter_in="make|CLEAN|error|warning|In function|In file included|Wrote"
	# from which filter our some more
	local filter_out="Leaving|Entering"

	# choose latest devtoolset(gcc) if exists
        DTS=$(rpm -qa devtoolset-* | cut -f2 -d- | sort -nu | tail -n1)
        if [ -n "${DTS}" ]; then
		echo "Enable devtools-${DTS}"
        	SCL="scl enable devtoolset-$DTS"
        fi

	if [ -z "${PRE_MAKE_CMD}" ]; then
		local make_cmd="cd ${REMOTE_DIR}; ${MAKE_CMD} ${make_flags} ${MAKE_OPTS}"
	else
		local make_cmd="cd ${REMOTE_DIR}; ${PRE_MAKE_CMD}; ${MAKE_CMD} ${make_flags} ${MAKE_OPTS}"
	fi

	# specific server config will be set instead of general config
	make_cmd=$(remove_duplicated_params "${make_cmd}")

	remove_old_koxz_cmd="find . -type f -name '*.ko.xz' -exec rm {} +"
	#removes old compressed ko.xz if exists
	if [ $RUN_LOCAL = true ] ; then
		${remove_old_koxz_cmd}
	else
		${ssh_cmd} ${server_name} "${remove_old_koxz_cmd}" 2>&1
	fi

	LOCAL_MAKE="${MAKE_CMD} ${MAKE_OPTS} ${make_flags}"
	if [ -n "${SCL}" ]; then
		LOCAL_MAKE="${SCL} \"${LOCAL_MAKE}\""
	fi
	# save output in the log file and pass on to stdout filtering
	if [ ${FILTER_OUT} = true ]; then
		if [ $RUN_LOCAL = true ] ; then
			${PRE_MAKE_CMD}
			eval "${LOCAL_MAKE}" | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/" | \
                        ${GREP_UNBUF} -E "(${filter_in})" | \
                        ${GREP_UNBUF} -Ev "(${filter_out})"
		else
			${ssh_cmd} ${server_name} "${make_cmd}" 2>&1 | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/" | \
                        ${GREP_UNBUF} -E "(${filter_in})" | \
                        ${GREP_UNBUF} -Ev "(${filter_out})"
		fi
	else
		if [ $RUN_LOCAL = true ] ; then
			${PRE_MAKE_CMD}
			eval "${LOCAL_MAKE}" | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/"
		else
			${ssh_cmd} ${server_name} "${make_cmd}" 2>&1 | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/"
		fi
	fi

	RET_VAL=${PIPESTATUS[0]} # exit status of "ssh make"

	if $COMPILE_NVMFT && [ $RET_VAL -eq 0 ]; then
		echo "Compiling NVMft (spdk)"
		invoke_remote_nvmft_spdk_make "$server_num" "$server_name" "$filter_in" "$filter_out"
		RET_VAL=$? # exit status of "ssh make nvmft"
	fi

	return $RET_VAL
}

invoke_remote_nvmft_spdk_make() {
	local server_num="$1"
	local server_name="$2"
	local ssh_cmd="${ssh_invoke[${server_num}]}"
	local make_spdk_cmd="./build_spdk.sh --create-rpm --copy-rpm ../ --rpm-version $SPDK_VERSION --rpm-release $SPDK_RELEASE --branch $SPDK_BRANCH_NAME --commit-id $SPDK_COMMIT_ID --change-id $SPDK_CHANGE_ID $SPDK_SIGN_RPM_FLAG"
	local make_local_spdk_cmd="cd spdk; sudo $make_spdk_cmd"
	local make_remote_spdk_cmd="cd ${REMOTE_DIR}/spdk; sudo $make_spdk_cmd"
        local log_file="${log_file[${server_num}]}"

	# leave only lines with keywords
	local filter_in=$3
	# from which filter our some more
	local filter_out=$4

	if [ ${FILTER_OUT} = true ]; then
                if [ $RUN_LOCAL = true ] ; then
                        ${make_local_spdk_cmd} | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/" | \
                        ${GREP_UNBUF} -E "(${filter_in})" | \
                        ${GREP_UNBUF} -Ev "(${filter_out})"
                else
                        ${ssh_cmd} ${server_name} "${make_remote_spdk_cmd}" 2>&1 | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/" | \
                        ${GREP_UNBUF} -E "(${filter_in})" | \
                        ${GREP_UNBUF} -Ev "(${filter_out})"
                fi
        else
                if [ $RUN_LOCAL = true ] ; then
                        ${make_local_spdk_cmd} ${MAKE_OPTS} ${make_flags} | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/"
                else
                        ${ssh_cmd} ${server_name} "${make_remote_spdk_cmd}" 2>&1 | \
                        tee -a ${log_file} | \
                        ${SED_UNBUF} -e "s/$/ [${server_name}]/"
                fi
        fi

	return ${PIPESTATUS[0]}
}

get_py_executable() {
	script=$1
	executable=$(head -1 "$script" | grep -E "^#!" | sed  's/^#!//')
	if [ -n "$executable" ]; then
		echo "$executable"
	else
		echo "python2.7"
	fi
}

byte_compile_python_scripts() {
	echo "Byte compiling python scripts"

	declare -A py_scripts=([bin]="nvmesh_gen_symvers nvmesh_client_instance_do nvmesh_clnt_shutdown")
	management_cm_scripts=$(ls management_cm| grep -E ".py$")
	py_scripts[management_cm]=${management_cm_scripts}

	mgn_cm_client_scripts=$(ls management_cm/clnt | grep -E ".py$")
	py_scripts[management_cm/clnt]=${mgn_cm_client_scripts}

	for script_path in "${!py_scripts[@]}"; do
		for py_script in ${py_scripts[${script_path}]}; do
			echo "working on ${script_path}/${py_script}"
			executable=$(get_py_executable "${script_path}/${py_script}")
			echo "executable=$executable"
			$executable -m py_compile ${script_path}/${py_script}
			if [ $? -ne 0 ]; then
				echo "will try to compile with python2.7"
				python2.7 -m py_compile ${script_path}/${py_script}
			fi
			if [ $? -ne 0 ]; then
				exit_build "Failed to byte-compile ${script_path}/${py_script}"
			else
				echo "successfully build ${script_path}/${py_script}"
			fi
		done
	done

	echo "Done byte compiling python scripts"
}

build_started=true

#byte_compile_python_scripts

# make logs dir
mkdir -p ${LOG_DIR} || exit_build "Local logs mkdir ${LOG_DIR} failed"

# make remote dirs
if [ $RUN_LOCAL = false ] ; then
	for ((i=0; i<num_servers; i++)); do
		if [ $PARALLEL = true ]; then
			if [ $RUN_LOCAL = true ] ; then
				mkdir -p ~/$REMOTE_DIR
			else
				create_remote_dir ${i} &
			fi
			# return codes are saved in wait_children()
			child_pid[${i}]=$?
		else
			if [ $RUN_LOCAL = true ] ; then
				mkdir -p ~/$REMOTE_DIR
			else
				create_remote_dir ${i}
			fi
			save_ret_val "${i}" "$?" || exit_build "Remote mkdir failed: ${server_name[$i]}"
			echo
		fi
	done
fi
if [ $PARALLEL = true ]; then
	wait_children || exit_build "Remote mkdir failed"
fi
echo -e "Done remote mkdir\n"

# rsync
if [ $RUN_LOCAL = false ] ; then
	for ((i=0; i<num_servers; i++)); do
		if [ $PARALLEL = true ]; then
			rsync_remote_dir ${i} &
			# return codes are saved in wait_children()
			child_pid[${i}]=$!
		else
			rsync_remote_dir ${i}
			save_ret_val "${i}" "$?" || exit_build "rsync failed: ${server_name[$i]}"
			echo
		fi
	done
fi
if [ $PARALLEL = true ]; then
	wait_children || exit_build "rsync failed"
fi
echo -e "Done rsync\n"

if [ $SYNC_ONLY = true ]; then
	exit_build
fi

# remote make
for ((i=0; i<num_servers; i++)); do
	if [ $PARALLEL = true ]; then
		invoke_remote_make ${i} &
		# return codes are saved in wait_children()
		child_pid[${i}]=$!
	else
		invoke_remote_make ${i}
		save_ret_val "${i}" "$?" || exit_build "Make failed: ${server_name[$i]}"
		echo
	fi
done
# Local make pager copy, will be autocompiled on first run
(rsync -rlpgoD tools/traces_post_processor ..)
# Wait
if [ $PARALLEL = true ]; then
	wait_children || exit_build "Make failed"
fi
echo -e "Done make\n"


# retrieve remote files to repository
if [[ "${build_tag[@]}" != " " ]]; then echo "Start copy to install repo"; fi
for ((i=0; i<num_servers; i++)); do
	if [ -z "${build_tag[$i]}" ]; then continue; fi

	ssh_cmd="${ssh_invoke[${server_num}]}"

	if [ "$REPO_RPMS" = "true" ]; then
		rpms_repo_dir="${REPO_DIR}/RPMS/${BRANCH_NAME}/${COMMIT_ID}/${build_tag[$i]}"
		echo -e "\nCopy RPMS ${server_name[$i]} --> ${rpms_repo_dir}\n"
		mkdir -p ${rpms_repo_dir} || exit_build "Failed to create ${rpms_repo_dir}"
		rsync -zmrv -e "${ssh_cmd}" --include '*/' --include '*.rpm' --exclude '*' ${server_name[$i]}:${REMOTE_DIR}/ ${rpms_repo_dir}
	fi

	if [ "$REPO_SYMS" = "true" ]; then
		syms_repo_dir="${REPO_DIR}/SYMS/${BRANCH_NAME}/${COMMIT_ID}/${build_tag[$i]}"
		echo -e "\nCopy SYMS ${server_name[$i]} --> ${syms_repo_dir}\n"
		mkdir -p ${syms_repo_dir} || exit_build "Failed to create ${syms_repo_dir}"
		rsync -zmrv -e "${ssh_cmd}" --include '*/' --include '*.o' --exclude '*' ${server_name[$i]}:${REMOTE_DIR}/ ${syms_repo_dir}
	fi

	if [ "$REPO_MODS" = "true" ]; then
		mods_repo_dir="${REPO_DIR}/MODS/${BRANCH_NAME}/${COMMIT_ID}/${build_tag[$i]}"
		echo -e "\nCopy MODS ${server_name[$i]} --> ${mods_repo_dir}\n"
		mkdir -p ${mods_repo_dir} || exit_build "Failed to create ${mods_repo_dir}"
		rsync -zmrv -e "${ssh_cmd}" --include '*/' --include '*.ko' --exclude '*' ${server_name[$i]}:${REMOTE_DIR}/ ${mods_repo_dir}
	fi
done
if [[ "${build_tag[@]}" != " " ]]; then echo -e "\nDone copy to install repo"; fi

if $COMPILE_NVMFT && ! $IS_COMPILATOR; then
	#remove spdk cloned repo
	rm -rf spdk
fi

exit_build "Build complete"
