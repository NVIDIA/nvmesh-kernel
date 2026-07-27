#!/bin/bash

# to be invoked just as: ./install.sh
# ./build_sh_conf should contain all build options
#
# *** per-server options ***
# INSTALL_TARGETS="<build-tag>@<hostname> [<build-tag>@<hostname2>] [<build-tag2>@<hostname3>..."
#   example: INSTALL_TARGETS="rh72ofed31@nvme68 rh71ofed24@nvme40"
# INSTALL_CLIENTS="<build-tag>@<hostname> [<build-tag>@<hostname2>] [<build-tag2>@<hostname3>..."
#   example: INSTALL_CLIENTS="rh72ofed31@nvme68 rh71ofed24@nvme40"
#
# *** global options ***
# SSH_INVOKE=<ssh-cmd-line>
#   example: SSH_INVOKE="sshpass -e ssh -l root"
#            use sshpass to login as user "root" with password in ${SSHPASS}
#   example: SSH_INVOKE="sshpass -p Pa$$w0rd ssh -l root"
#            the same as above but with the password supplied in cmd line
# INSTALL_REMOTE_DIR=<remote-install-dir-name>
#   example: INSTALL_REMOTE_DIR=excelero
# REPO_DIR=<local-repo-dir-name>
#   example: REPO_DIR="~/build_repo"
# RSYNC_OPTS=<list_of_rsync_args>, to be added to default ones
#   example: RSYNC_OPTS="--compress"
# PARALLEL=(true|false), default=false
#   example: PARALLEL=true
# DRY_RUN=(true|false), default=false
#   example: DRY_RUN=true

# process command line opts, if any
usage()
{
	echo -e "Usage: $(basename $0) [options]\n"
	echo -e "options:"
	echo -e "\t[-c|--conf] <conf-file-name>"
	echo -e "\t[-s|--ssh] <ssh-cmd>"
	echo -e "\t[-r|--rsync] <extra-rsync-opts>"
	echo -e "\t[-d|--dir] <remote-install-dir>"
	echo -e "\t[-R|--repo] <local-repo-dir>"
	echo -e "\t[-T|--target] <target-server-conf>"
	echo -e "\t[-C|--client] <client-server-conf>"
	echo -e "\t[-N|--nics] <nics-conf>"
	echo -e "\t[-P|--parallel]"
	echo -e "\t[-D|--dry]"
	echo -e "\t[-h|--help]"
    exit 1
}

SHORT_OPTIONS="hPD" # no args
SHORT_OPTIONS+="c:s:r:d:R:T:C:" # with arg

LONG_OPTIONS="help,parallel,dry" # no args
LONG_OPTIONS+="conf:,ssh:,rsync:,dir:,repo:,trget:,client:" # with arg

options=$(getopt -o ${SHORT_OPTIONS} -l ${LONG_OPTIONS} -- "$@")
if [ $? -ne 0 ]; then
	# something went wrong, getopt should have printed an error message
	usage
fi

eval set -- ${options}

while [ $# -gt 1 ]; do
	case $1 in
		-h|--help) usage; ;;
		-P|--parallel) CMD_PARALLEL=true; ;;
		-D|--dry) CMD_DRY_RUN=true; ;;
		# for options with required arguments, an additional shift is required
		-c|--conf) CMD_CONFIG_FILE="$2"; shift ;;
		-s|--ssh) CMD_SSH_INVOKE="$2"; shift ;;
		-r|--rsync) CMD_RSYNC_OPTS+=" $2"; shift ;;
		-d|--dir) CMD_INSTALL_REMOTE_DIR="$2"; shift ;;
		-R|--repo) CMD_REPO_DIR="$2"; shift ;;
		-T|--target) CMD_INSTALL_TARGETS+=" ${2# }"; shift ;;
		-C|--client) CMD_INSTALL_CLIENTS+=" ${2# }"; shift ;;
		-N|--nics) CMD_INSTALL_NICS+=" ${2# }"; shift ;;
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

# obtain local git state info
BRANCH_NAME=$(git symbolic-ref --short --quiet HEAD) || BRANCH_NAME=$(git rev-parse --abbrev-ref HEAD)
COMMIT_ID=$(git log -n1 --format=%h)
CHANGE_ID=$(git log -n1 --format=%b | awk '/^Change-Id: / {print $2}')
GIT_TOP_DIR=$(git rev-parse --show-toplevel)

# make sure we start in the top dir of the git repo and include config
pushd "${GIT_TOP_DIR}"

CONFIG_FILE=${CMD_CONFIG_FILE:-${GIT_TOP_DIR}/build_sh_conf}
if [ ! -r ${CONFIG_FILE} ]; then
	echo -en "\nConfig file not found: ${CONFIG_FILE} - Exiting...\n"
	exit 1
fi
source ${CONFIG_FILE}

# set default opts; conf file overrides most of them
SSH_INVOKE=${SSH_INVOKE:-ssh}
INSTALL_REMOTE_DIR=${INSTALL_REMOTE_DIR:-"excelero"}
REPO_DIR=${REPO_DIR:-${HOME}/build_repo}
PARALLEL=${PARALLEL:-false}
DRY_RUN=${DRY_RUN:-false}

# for some opts cmd line values override everything else
SSH_INVOKE=${CMD_SSH_INVOKE:-${SSH_INVOKE}}
INSTALL_REMOTE_DIR=${CMD_INSTALL_REMOTE_DIR:-${INSTALL_REMOTE_DIR}}
REPO_DIR=${CMD_REPO_DIR:-${REPO_DIR}}
PARALLEL=${CMD_PARALLEL:-${PARALLEL}}
DRY_RUN=${CMD_DRY_RUN:-${DRY_RUN}}

# for some opts add cmd line arg value
RSYNC_OPTS+=${CMD_RSYNC_OPTS}
# filter out possible duplicate entries
RSYNC_OPTS=$(arr_entries_leave_unique "${RSYNC_OPTS}")

# add last arguments (invariant or indirectly configurable)
RSYNC_OPTS+=" --ignore-errors -av"

# check used tools options support
SED_UNBUF="sed --unbuffered"
`echo | ${SED_UNBUF} 's/$//' 2> /dev/null` || SED_UNBUF="sed"
GREP_UNBUF="grep --line-buffered"
`echo | ${GREP_UNBUF} '$' 2> /dev/null` || GREP_UNBUF="grep"

save_ret_val()
{
	client_install_err[$1]=$2
	return $2
}

wait_client_children()
{
	local ret_val=0
	for ((i=0; i<num_clients; i++)); do
	    if [ ${client_child_pid[${i}]} -eq 0 ]; then
			continue
		fi
		wait ${client_child_pid[${i}]}
		save_ret_val "${i}" "$?"
		if [ $? -ne 0 ]; then
			echo "Failure on: ${client_name[$i]}"
			ret_val=1
		fi
		client_child_pid[${i}]=0
	done
	return ${ret_val}
}

wait_target_children()
{
	local ret_val=0
	for ((i=0; i<num_targets; i++)); do
	    if [ ${target_child_pid[${i}]} -eq 0 ]; then
			continue
		fi
		wait ${target_child_pid[${i}]}
		save_ret_val "${i}" "$?"
		if [ $? -ne 0 ]; then
			echo "Failure on: ${target_name[$i]}"
			ret_val=1
		fi
		target_child_pid[${i}]=0
	done
	return ${ret_val}
}

exit_build()
{
	echo -en "\n$1 - Exiting...\n"

	if [[ `expr ${num_targets} + ${num_clients}` == 0 ]]; then exit 1; fi
	if [ $DRY_RUN = true ]; then exit 0; fi

	local exit_val=0

	echo "-----------------------------------------------------------------"
	for ((i=0; i<num_targets; i++)); do
		echo "$((i+1)): ${target_name[$i]} err:${target_install_err[${i}]} log_file: ${target_log_file[${i}]}"
		if [ ${target_install_err[${i}]} -ne 0 ]; then exit_val=1; fi
	done
	echo "-----------------------------------------------------------------"

	echo "-----------------------------------------------------------------"
	for ((i=0; i<num_clients; i++)); do
		echo "$((i+1)): ${client_name[$i]} err:${client_install_err[${i}]} log_file: ${client_log_file[${i}]}"
		if [ ${client_install_err[${i}]} -ne 0 ]; then exit_val=1; fi
	done
	echo "-----------------------------------------------------------------"

	popd
	exit ${exit_val}
}

# trap keyboard interrupt (control-c) and exit
control_c()
{
	for ((i=0; i<num_clients; i++)); do
	    if [ ${client_child_pid[${i}]} -eq 0 ]; then
			continue
		fi
		kill ${client_child_pid[${i}]}
	done
	for ((i=0; i<num_targets; i++)); do
	    if [ ${target_child_pid[${i}]} -eq 0 ]; then
			continue
		fi
		kill ${target_child_pid[${i}]}
	done
	wait_client_children
	wait_target_children

	exit_build "Aborted by user"
}

trap control_c SIGINT

remove_client_proc()
{
	for i in `grep /dev/nvmesh /proc/mounts | cut -d' ' -f1`; do
		sudo umount ${i}
		if [ $? -ne 0 ]; then
			echo "Failed to unmount ${i}. Removing nvmeshclient may fail"
		fi
	done

	if test -e /etc/redhat-release; then
		sudo yum remove -y nvmesh-client nvmesh-core
	elif test -e /etc/SuSE-release; then
		sudo zypper -n remove nvmesh-client nvmesh-core
	elif test -e /etc/debian_version; then
		sudo apt-get -y remove nvmesh-client nvmesh-core
	else
		echo "Unknown distribution!"
		exit -1
	fi
}

remove_prev_client()
{
	local client_num=$1
	local client_name="${client_name[${client_num}]}"
	local log_file="${client_log_file[${client_num}]}"

	# convert the function remove_client_proc to text and invoke remotely
	${SSH_INVOKE} -tt ${client_name} "$(declare -f remove_client_proc); remove_client_proc" | \
		${SED_UNBUF} -e "s/^/${client_name}: /" | \
		tee ${log_file}

	return ${PIPESTATUS[0]} # exit status of "rmmod nvmeibc"
}

remove_target_proc()
{
	if test -e /etc/redhat-release; then
		sudo yum remove -y nvmesh-target nvmesh-client nvmesh-core
	elif test -e /etc/SuSE-release; then
		sudo zypper -n remove nvmesh-target nvmesh-client nvmesh-core
	elif test -e /etc/debian_version; then
		sudo apt-get -y remove nvmesh-target nvmesh-client nvmesh-core
	else
		echo "Unknown distribution!"
		exit -1
	fi
}

remove_prev_target()
{
	local target_num=$1
	local target_name="${target_name[${target_num}]}"
	local log_file="${target_log_file[${target_num}]}"

	# invoke rmmod nvmeibs remotely
	${SSH_INVOKE} -tt ${target_name} "$(declare -f remove_target_proc); remove_target_proc" | \
		${SED_UNBUF} -e "s/^/${target_name}: /" | \
		tee ${log_file}

	return ${PIPESTATUS[0]} # exit status of "rmmod nvmeibs"
}

copy_to_client()
{
	local client_num=$1
	local client_name="${client_name[${client_num}]}"
	local client_tag="${client_tag[${client_num}]}"
	local log_file="${log_file[${client_num}]}"

	pushd ${REPO_DIR}/RPMS/

	ls ${BRANCH_NAME}/${COMMIT_ID}/${client_tag}/nvmesh-client*.rpm ${BRANCH_NAME}/${COMMIT_ID}/${target_tag}/nvmesh-core*.rpm | rsync ${RSYNC_OPTS} --files-from=- -e "${SSH_INVOKE}" . ${client_name}:${INSTALL_REMOTE_DIR} | \
			${SED_UNBUF} -e "s/^/${client_name}: /" | \
			tee -a ${log_file}
	popd

	return ${PIPESTATUS[0]} # exit status of "rsync nvmesh-client*.rpm"
}

copy_to_target()
{
	local target_num=$1
	local target_name="${target_name[${target_num}]}"
	local target_tag="${target_tag[${target_num}]}"
	local log_file="${log_file[${target_num}]}"

	pushd ${REPO_DIR}/RPMS/

	ls ${BRANCH_NAME}/${COMMIT_ID}/${target_tag}/nvmesh-target*.rpm ${BRANCH_NAME}/${COMMIT_ID}/${target_tag}/nvmesh-client*.rpm ${BRANCH_NAME}/${COMMIT_ID}/${target_tag}/nvmesh-core*.rpm | rsync ${RSYNC_OPTS} --files-from=- -e "${SSH_INVOKE}" . ${target_name}:${INSTALL_REMOTE_DIR} | \
			${SED_UNBUF} -e "s/^/${target_name}: /" | \
			tee -a ${log_file}
	popd

	return ${PIPESTATUS[0]} # exit status of "rsync nvmesh-target*.rpm"
}

install_target_proc()
{
	local repo_path="$1"
	local mgmt_prot="$2"
	local mgmt_servers="$3"
	local mgmt_server="$4"
	local nics="$5"
	local append_conf="$6"
	local no_autostart="$7"
	local mlx5_rdda_enabled="$8"
	local target_devices="$9"
	local rpms
	local debs

	if [ -f ${repo_path}/nvmesh-core*.rpm ]; then
        rpms="${repo_path}/nvmesh-core*.rpm"
    else
        rpms="${repo_path}/nvmesh-client*.rpm ${repo_path}/nvmesh-target*.rpm"
    fi

    if [ -f ${repo_path}/nvmesh-core*.deb ]; then
        rpms="${repo_path}/nvmesh-core*.deb"
    else
        debs="${repo_path}/nvmesh-client*.deb ${repo_path}/nvmesh-target*.deb"
    fi

	# install the nvmesh-target package
	echo Installing nvmesh-target package
	if test -e /etc/redhat-release; then
		sudo yum install -y ${rpms}
	elif test -e /etc/SuSE-release; then
		sudo zypper -n install ${rpms}
	elif test -e /etc/debian_version; then
		sudo apt-get -y install ${debs}
	else
		echo "Unknown distribution!"
		exit -1
	fi

	# update the nvmesh.conf file
	echo Updating nvmesh.conf MANAGEMENT_PROTOCOL to \"$mgmt_prot\" MANGEMENT_SERVERS to \"$mgmt_servers\" and CONFIGURED_NICS to \"$nics\"
	sudo sed -i {s#^MANAGEMENT_PROTOCOL=.*#MANAGEMENT_PROTOCOL=\"$mgmt_prot\"#} /etc/nvmesh/nvmesh.conf
	sudo sed -i {s#^MANAGEMENT_SERVERS=.*#MANAGEMENT_SERVERS=\"$mgmt_servers\"#} /etc/nvmesh/nvmesh.conf
	sudo sed -i {s#^CONFIGURED_NICS=.*#CONFIGURED_NICS=\"$nics\"#} /etc/nvmesh/nvmesh.conf
	if [ ! -z "$mlx5_rdda_enabled" ]; then
		sudo sed -i {s#^MLX5_RDDA_ENABLED=.*#MLX5_RDDA_ENABLED=\"$mlx5_rdda_enabled\"#} /etc/nvmesh/nvmesh.conf
	fi
	sudo sh -c "echo '$append_conf' >> /etc/nvmesh/nvmesh.conf"
	sudo sh -c "echo '$target_devices' > /etc/nvmesh/target_devices.conf"
	
	if [ "$no_autostart" = "true" ]; then
		sudo chkconfig --del nvmeshclient
		sudo chkconfig --del nvmeshtarget
	fi
	
	# start the nvmeshclient service
	echo Starting nvmeshclient
	sudo service nvmeshclient start

	# start the nvmeshtarget service
	echo Starting nvmeshtarget
	sudo service nvmeshtarget start
}

install_target()
{
	local target_num=$1
	local target_name="${target_name[${target_num}]}"
	local target_tag="${target_tag[${target_num}]}"
	local target_nics="${target_nics[$target_num]}"
	local log_file="${target_log_file[${target_num}]}"
	local rpm_path
	
	if [ "$REPO_RPMS" = "true" ]; then
		rpm_path="${INSTALL_REMOTE_DIR}/${BRANCH_NAME}/${COMMIT_ID}/${target_tag}"
	else
		rpm_path="${REMOTE_DIR}"
	fi

	# invoke yum install remotely
	${SSH_INVOKE} -tt ${target_name} "$(declare -f install_target_proc); install_target_proc $rpm_path '${INSTALL_MGMT_PROT}' '${INSTALL_MGMT_SRVS}' '${INSTALL_MGMT}' '${target_nics}' '${INSTALL_APPEND_TARGET_CONF}' ${INSTALL_DISABLE_AUTOSTART} ${INSTALL_MLX5_RDDA_ENABLED} ${INSTALL_TARGET_DEVICES}" | \
		${SED_UNBUF} -e "s/^/${target_name}: /" | \
		tee -a ${log_file}

	return ${PIPESTATUS[0]} # exit status of "yum install"
}

install_client_proc()
{
	local repo_path="$1"
	local mgmt_prot="$2"
	local mgmt_servers="$3"
	local mgmt_server="$4"
	local nics="$5"
	local append_conf="$6"
	local no_autostart="$7"
	local rpms
	local debs

	if [ -f ${repo_path}/nvmesh-core*.rpm ]; then
        rpms="${repo_path}/nvmesh-core*.rpm"
    else
        rpms="${repo_path}/nvmesh-client*.rpm"
    fi

    if [ -f ${repo_path}/nvmesh-core*.deb ]; then
        rpms="${repo_path}/nvmesh-core*.deb"
    else
        debs="${repo_path}/nvmesh-client*.deb"
    fi
	
	# install the nvmesh-client package
	echo Installing nvmesh-client package
	if test -e /etc/redhat-release; then
		sudo yum install -y ${rpms}
	elif test -e /etc/SuSE-release; then
		sudo zypper -n install ${rpms}
	elif test -e /etc/debian_version; then
		sudo apt-get -y install ${debs}
	else
		echo "Unknown distribution!"
		exit -1
	fi

	# update the nvmesh.conf file
	echo Updating nvmesh.conf MANAGEMENT_PROTOCOL to \"$mgmt_prot\" MANGEMENT_SERVERS to \"$mgmt_servers\" and CONFIGURED_NICS to \"$nics\"
	sudo sed -i {s#^MANAGEMENT_PROTOCOL=.*#MANAGEMENT_PROTOCOL=\"$mgmt_prot\"#} /etc/nvmesh/nvmesh.conf
	sudo sed -i {s#^MANAGEMENT_SERVERS=.*#MANAGEMENT_SERVERS=\"$mgmt_servers\"#} /etc/nvmesh/nvmesh.conf
	sudo sed -i {s#^CONFIGURED_NICS=.*#CONFIGURED_NICS=\"$nics\"#} /etc/nvmesh/nvmesh.conf
	sudo sh -c "echo '$append_conf' >> /etc/nvmesh/nvmesh.conf"
	
	if [ "$no_autostart" = "true" ]; then
		sudo chkconfig --del nvmeshclient
	fi

	# start the nvmeshclient service
	echo Starting nvmeshclient
	sudo service nvmeshclient start
}

install_client()
{
	local client_num=$1
	local client_name="${client_name[${client_num}]}"
	local client_tag="${client_tag[${client_num}]}"
	local client_nics="${client_nics[$client_num]}"
	local log_file="${client_log_file[${client_num}]}"
	local rpm_path
	
	if [ "$REPO_RPMS" = "true" ]; then
		rpm_path="${INSTALL_REMOTE_DIR}/${BRANCH_NAME}/${COMMIT_ID}/${client_tag}"
	else
		rpm_path="${REMOTE_DIR}"
	fi

	# invoke yum install remotely
	${SSH_INVOKE} -tt ${client_name} "$(declare -f install_client_proc); install_client_proc $rpm_path '${INSTALL_MGMT_PROT}' '${INSTALL_MGMT_SRVS}' '${INSTALL_MGMT}' '${client_nics}' '${INSTALL_APPEND_CLIENT_CONF}' ${INSTALL_DISABLE_AUTOSTART}" | \
		${SED_UNBUF} -e "s/^/${client_name}: /" | \
		tee -a ${log_file}

	return ${PIPESTATUS[0]} # exit status of "yum install"
}

print_per_target_tags()
{
	local i
	for ((i=0; i<num_targets; i++)); do
		echo -e "\t${target_name[${i}]}: ${target_tag[$i]}"
	done
}

print_per_client_tags()
{
	local i
	for ((i=0; i<num_clients; i++)); do
		echo -e "\t${client_name[${i}]}: ${client_tag[$i]}"
	done
}

# parse and prepare all per-target opts
INSTALL_NICS+="${CMD_INSTALL_NICS}"
INSTALL_TARGETS+="${CMD_INSTALL_TARGETS}"
i=0
for rs in ${INSTALL_TARGETS}; do
	target_tag[$i]=`echo "${rs}" | cut -d@ -f1`
	target_name[$i]=`echo "${rs}" | cut -d@ -f2`
	target_nics[$i]=""
	for nics in ${INSTALL_NICS}; do
		srv=`echo "${nics}" | cut -d: -f1`
		if [ "$srv" == "${target_name[$i]}" ]; then
			target_nics[$i]=`echo "${nics}" | cut -d: -f2-`
			break
		fi
	done
	target_log_file[$i]="/tmp/target_${target_name[$i]}.install"
	target_child_pid[${i}]=0
	target_install_err[${i}]=0
	((i++))
done
num_targets=${i}

# parse and prepare all per-client opts
INSTALL_CLIENTS+="${CMD_INSTALL_CLIENTS}"
i=0
for rs in ${INSTALL_CLIENTS}; do
	client_tag[$i]=`echo "${rs}" | cut -d@ -f1`
	client_name[$i]=`echo "${rs}" | cut -d@ -f2`
	client_nics[$i]=""
	for nics in ${INSTALL_NICS}; do
		srv=`echo "${nics}" | cut -d: -f1`
		if [ "$srv" == "${client_name[$i]}" ]; then
			client_nics[$i]=`echo "${nics}" | cut -d: -f2-`
			break
		fi
	done
	client_log_file[$i]="/tmp/client_${target_name[$i]}.install"
	client_child_pid[${i}]=0
	client_install_err[${i}]=0
	((i++))
done
num_clients=${i}

# print build opts final values
echo "Installing start: `date '+%a %d-%b-%y %H.%M.%S'`"
echo "Config file: ${CONFIG_FILE}"
echo "Branch: ${BRANCH_NAME}, Commit: ${COMMIT_ID}"
echo "SSH command: ${SSH_INVOKE}"
echo "Rsync opts: ${RSYNC_OPTS}"
echo "Local repo dir: ${REPO_DIR}"
echo "Remote install dir: ${INSTALL_REMOTE_DIR}"
echo "Parallel: ${PARALLEL}"

if [ "${SED_UNBUF}" = "sed" ]; then echo "sed --unbuffered unsupported, using: sed"; fi
if [ "${GREP_UNBUF}" = "grep" ]; then echo "grep --line-buffered unsupported, using: grep"; fi

if [[ ${num_targets} > 0 ]]; then
	if [[ ${num_targets} > 1 ]]; then
		echo -e "\n${num_targets} targets: ${target_name[*]}"
	else
		echo -e "\n1 target: ${target_name[*]}"
	fi
	arr_entries_are_unique "${target_name[@]}" || exit_build "Duplicate target names"

	echo "Per-target-tags:"
	print_per_target_tags
else # no targets
	if [[ ${num_clients} > 0 ]]; then
		echo "No targets defined. Installing to clients only"
	else
		exit_build "No targets and no clients defined. Nothing to do!"
	fi
fi

if [[ ${num_clients} > 0 ]]; then
	if [[ ${num_clients} > 1 ]]; then
		echo -e "\n${num_clients} clients: ${client_name[*]}"
	else
		echo -e "\n1 client: ${client_name[*]}"
	fi
	arr_entries_are_unique "${client_name[@]}" || exit_build "Duplicate client names"

	echo "Per-client-tags:"
	print_per_client_tags
else # no clients
	echo "No clients defined. Installing to targets only"
fi

if [ $DRY_RUN = true ]; then exit_build "Dry run requested"; fi
echo

if [[ $INSTALL_CLEAN ]]; then
    # remove previous nvmeshclients
    for ((i=0; i<num_clients; i++)); do
        if [ $PARALLEL = true ]; then
            remove_prev_client ${i} &
            # return codes are saved in wait_children()
            client_child_pid[${i}]=$!
        else
            remove_prev_client ${i}
            save_ret_val "${i}" "$?" || exit_build "Remove previous client failed: ${client_name[$i]}"
            echo
        fi
    done
    if [ $PARALLEL = true ]; then
        wait_client_children || exit_build "Remove previous client failed"
    fi
    echo -e "Done remove previous client(s)\n"

    # remove previous nvmeshtargets
    for ((i=0; i<num_targets; i++)); do
        if [ $PARALLEL = true ]; then
            remove_prev_target ${i} &
            # return codes are saved in wait_children()
            target_child_pid[${i}]=$!
        else
            remove_prev_target ${i}
            save_ret_val "${i}" "$?" || exit_build "Remove previous target failed: ${target_name[$i]}"
            echo
        fi
    done
    if [ $PARALLEL = true ]; then
        wait_target_children || exit_build "Remove previous target failed"
    fi
    echo -e "Done remove previous target(s)\n"
fi

if [ "$REPO_RPMS" = "true" ]; then
	# copy updated package to target
	for ((i=0; i<num_targets; i++)); do
		if [ $PARALLEL = true ]; then
			copy_to_target ${i} &
			# return codes are saved in wait_children()
			target_child_pid[${i}]=$!
		else
			copy_to_target ${i}
			save_ret_val "${i}" "$?" || exit_build "Remove previous target failed: ${target_name[$i]}"
			echo
		fi
	done
	if [ $PARALLEL = true ]; then
		wait_target_children || exit_build "Remove previous target failed"
	fi
	echo -e "Done copying RPM to target(s)\n"

	# copy updated package to client
	for ((i=0; i<num_clients; i++)); do
		if [ $PARALLEL = true ]; then
			copy_to_client ${i} &
			# return codes are saved in wait_children()
			client_child_pid[${i}]=$!
		else
			copy_to_client ${i}
			save_ret_val "${i}" "$?" || exit_build "Remove previous client failed: ${client_name[$i]}"
			echo
		fi
	done
	if [ $PARALLEL = true ]; then
		wait_client_children || exit_build "Remove previous client failed"
	fi
	echo -e "Done copying RPM to client(s)\n"
fi

# install client(s)
for ((i=0; i<num_clients; i++)); do
	if [ $PARALLEL = true ]; then
		install_client ${i} &
		# return codes are saved in wait_children()
		client_child_pid[${i}]=$!
	else
		install_client ${i}
		save_ret_val "${i}" "$?" || exit_build "Installing client failed: ${client_name[$i]}"
		echo
	fi
done
if [ $PARALLEL = true ]; then
	wait_client_children || exit_build "Installing client failed"
fi
echo -e "Done Installing client(s)\n"

# install target(s)
for ((i=0; i<num_targets; i++)); do
	if [ $PARALLEL = true ]; then
		install_target ${i} &
		# return codes are saved in wait_children()
		target_child_pid[${i}]=$!
	else
		install_target ${i}
		save_ret_val "${i}" "$?" || exit_build "Installing target failed: ${target_name[$i]}"
		echo
	fi
done
if [ $PARALLEL = true ]; then
	wait_target_children || exit_build "Installing target failed"
fi
echo -e "Done Installing target(s)\n"

exit_build "Installation complete"
