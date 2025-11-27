#!/bin/bash

usage_str="Usage: $0 [--toma] [--clnt] [--srv] [--reason <one_word>]"
# Parse the arguments
declare i is_toma=0
declare i is_clnt=0
declare i is_srv=0
reason_for_snapshot=""
arg_1=""
arg_2=""
while [ "$#" -gt 0 ]; do
	cmd="$1"
	val="$2"
	case "$cmd" in
		# Example:
		-rt|-dt)	# Where we require numeric arguments
			if [[ !("$val" =~ ^[0-9]+$) ]] ; then
				echo "OOPS $cmd argument $val" >&2
				exit 1
			fi;;
		#
		-h|-\?|--help) echo $usage_str; exit 1;;
		--reason) reason_for_snapshot=${val}; shift; shift; continue;;
		#
		--toma) is_toma=1; shift; continue;;
		--clnt) is_clnt=1; shift; continue;;
		--srv)  is_srv=1;  shift; continue;;
		#
		-*) echo "unknown option: '$cmd'" >&2; echo $usage_str; exit 1;;
		*) arg_1="$1 "; arg_2="$2 "; echo "arg_1=${arg_1} arg_2=${arg_2}"; shift; shift; continue;;
	esac
done

nvmesh_log_dir='/var/log/nvmesh'
trace_daemon_dir="${nvmesh_log_dir}/trace_daemon"
snapshot_prefix="${trace_daemon_dir}_snapshot_"
now_epoch_sec=`date '+%s'`
declare i ten_days_secs=$((10 * 24 * 60 * 60))

##############################   Remove (old) snapshots   ##################################

declare i creation_epoch_sec
declare i sec_since_create
cd ${nvmesh_log_dir} || exit
# Sort snapshots by date (old to new), and erase those that are more than 10 days old (except for the newest one)
for i in `ls -trd ${snapshot_prefix}* | tail -n +1` ; do		# Ignore the newest in this cleanup
	creation_epoch_sec=`stat --printf='%W' ${i}`		# %W is time of file birth, seconds since Epoch
	sec_since_create=$((${now_epoch_sec} - ${creation_epoch_sec}))
	if [ ${sec_since_create} -lt ${ten_days_secs} ]; then
		break
	fi
	echo "rm -rf ${i}.   sec_since_create=${sec_since_create}"
	rm -rf ${i}
done

declare i avail_disk_space_in_Mega
# Sort snapshots by date (new to old), and erase the newest as long as "df" free space is less than 500M
for i in `ls -td ${snapshot_prefix}*` ; do
	avail_disk_space_in_Mega=`df --output="avail" -BM ${trace_daemon_dir} | tail -1 | cut -d'M' -f1`
	if [ "_${avail_disk_space_in_Mega}" == "_" ]; then
		break
	fi
	if (( ${avail_disk_space_in_Mega} > 10000 )); then	# Erase snapshots until we have 10000M available space in df
		break
	fi
	echo "rm -rf ${i}   avail_disk_space_in_Mega=${avail_disk_space_in_Mega}"
	rm -rf ${i}
done
avail_disk_space_in_Mega=`df --output="avail" -BM ${trace_daemon_dir} | tail -1 | cut -d'M' -f1`
if (( ${avail_disk_space_in_Mega} < 10000 )); then
	exit 101;
fi

# Sort snapshots by date (new to old), and erase the middle once until we are left with 10 snapshot dirs
list_of_dirs=`ls -td ${snapshot_prefix}*`
declare i n_snapshots=`echo ${list_of_dirs} | wc -w`
declare i n_snapshot=0
declare i max_n_snapshots_to_keep=20
for i in ${list_of_dirs} ; do
	if (( ${n_snapshots} <= ${max_n_snapshots_to_keep} )); then
		break
	fi
	(( n_snapshot++ ))
	if (( ${n_snapshot} < $(( ${max_n_snapshots_to_keep} / 2)) )); then
        	continue
	fi
	(( n_snapshots-- ))
	echo "rm -rf ${i}   n_snapshots=${n_snapshots}"
	rm -rf ${i}
done

# remove old stat files. We already have them linked from the snapshot dirs
for i in `ls -td ${nvmesh_log_dir}/toma_*.stat | tail -n +100` ; do		# Leave the newest in this cleanup
	echo "rm -rf ${i}"
	rm -rf ${i}
done

##############################   Generate a snapshot   ##################################

function link_or_copy_file_to_cwd {
	target_file=$1
	if [[ "_" != "_${target_file}" && -a ${target_file} ]]; then		# probably more generic than if [[ -n ${target_file} ]]
		ln ${target_file} ./ || cp --sparse=always ${target_file} ./
	fi
}

snapshot_dir=${snapshot_prefix}`date +_%d%b%Y_%H%M%S`
mkdir ${snapshot_dir} || exit
if (( ! ${is_toma} && ! ${is_clnt} && ! ${is_srv} )); then
	is_toma=1; is_clnt=1; is_srv=1;
fi
match_str="$( ((${is_toma})) && echo 'toma*' || echo '' ) $( ((${is_clnt})) && echo 'nvmeibc* nvmeibp* nvmeibm*' || echo '' ) $( ((${is_srv})) && echo 'nvmeibs* nvmeibp* nvmeibm*' || echo '')"
echo "Taking a snapshot of the logs etc. for '${match_str}' debugging into ${snapshot_dir}"
cd ${trace_daemon_dir}
files_list=`ls ${match_str} 2> /dev/null | sort -u`
for i in ${files_list} ; do
	ln ${i} ${snapshot_dir}/${i}
done
files_list=`ls dict* 2> /dev/null`
if [[ "_" != "_${files_list}" ]]; then
    for i in ${files_list} ; do
	    ln ${i} ${snapshot_dir}/${i}
    done
fi
cd ${snapshot_dir}
link_or_copy_file_to_cwd /opt/nvmesh/common-repo/tools/traces_post_processor/pager
link_or_copy_file_to_cwd /opt/nvmesh/common-repo/tools/traces_post_processor/cpager
link_or_copy_file_to_cwd /opt/nvmesh/common-repo/tools/traces_post_processor/pager.py
cp --sparse=always /etc/nvmesh/.nvmesh.conf ./
cp --sparse=always /opt/NVMesh/common-repo/tools/toma_rpc.config ./
cp --sparse=always ${nvmesh_log_dir}/toma_trace.config ./
#
if ((${is_toma})); then
	sudo pkill -10 nvmeibt_toma
	latest_stat=`ls -t ${nvmesh_log_dir}/toma_*.stat | head -1`
	link_or_copy_file_to_cwd ${latest_stat}
	#
	latest_core=`ls -t /var/lib/{systemd,apport}/coredump/core* 2>/dev/null | head -1`
	link_or_copy_file_to_cwd ${latest_core}
	#
	# toma_executable=/lib/modules/`uname -r`/extra/nvmesh/target/toma/bin/release/nvmeibt_toma
	toma_executable="/opt/nvmesh/target-repo/*`uname --kernel-release`/toma/bin/release/nvmeibt_toma"
	link_or_copy_file_to_cwd ${toma_executble}
	link_or_copy_file_to_cwd ${nvmesh_log_dir}/nvmeibt_toma_src_tar.pgp
	#
	for i in /var/opt/nvmesh/toma/*; do
		link_or_copy_file_to_cwd ${i}
	done
	#
	if [[ "_" != "_${toma_executable}" && -a ${toma_executable} ]]; then
		tar -czhvf libs_for_gdb.tar `ldd ${toma_executable} | grep -E ' => ' | grep -v 'not found' | cut -d' ' -f3`
	fi
fi
#
echo "${reason_for_snapshot} " > ./REASON_FOR_SNAPSHOT.txt
