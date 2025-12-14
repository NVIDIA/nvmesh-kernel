#!/bin/bash

# Local on the server (on nvme1018 and alike)
core_file_origin=`ls -t /var/lib/systemd/coredump/core.nvmeibt_toma.* | head -1`
if [ "_${core_file_origin}" != "_" ]; then
	sudo lz4 -d -f ${core_file_origin} core
fi
core_file=`ls -t core core.[0-9]* | head -1`
trace_daemon_dir='/var/log/nvmesh/trace_daemon/'

if [ "_${core_file}" == "_" ]; then
	echo Assuming that running on a desktop and got the files using copyCI or copybug
	read -p "hostname? " host_name_to_debug
	nvmesh_dir="${host_name_to_debug}_nvmesh_logs_*"
	#
	gpg -d -z6 --batch --passphrase AlexanderRonen ${nvmesh_dir}/opt/nvmesh/target-repo/target_*/toma/bin/release/nvmeibt_toma_src_tar.pgp | tar -xvpf -
	toma_dir=toma
	cd ${toma_dir}
	core_file_origin=`ls -t ../${nvmesh_dir}/var/lib/systemd/coredump/core.nvmeibt_toma.* | head -1`
	if [ "_${core_file_origin}" != "_" ]; then
		sudo lz4 -d -f ${core_file_origin} core
	else
		core_file=`ls -t ../${nvmesh_dir}/var/opt/nvmesh/core* ../${nvmesh_dir}/var/log/nvmesh/trace_daemon_snapshot__*/core* | head -1`
		if [ "_${core_file}" == "_" ]; then
			echo "Couldn't find a core file"
			exit 1
		fi
		cp ${core_file} core
	fi
	core_file="core"
	trace_daemon_dir="../${nvmesh_dir}/var/log/nvmesh/trace_daemon/"
	tar -xvpf ../${nvmesh_dir}/target_*.tar
	tar -xvpf ../${nvmesh_dir}/toma_libs_*.tar
fi

default_executable='./bin/release/nvmeibt_toma'
read -p "executable file name? (Default ${default_executable})? " executable_file
if [ "_${executable_file}" == "_" ]; then
executable_file=${default_executable};
fi;

# Extract goodpath into files named short since otherwise pager ignores them

tmp_trace_daemon_dir='/tmp/trace_daemon';
(mkdir -p ${tmp_trace_daemon_dir}; cd ${tmp_trace_daemon_dir} && sudo rm -rf *)
cp -rp ${trace_daemon_dir}/*pager* ${trace_daemon_dir}/dict* ${tmp_trace_daemon_dir};
sudo touch ${tmp_trace_daemon_dir}/toma.binlog_marker.0;
sudo touch ${tmp_trace_daemon_dir}/toma.eph.binlog_marker.0;

# (cd /var/log/nvmesh/trace_daemon; sudo rm toma.binlog* toma.eph.binlog*; sudo touch toma.binlog_marker.0; sudo touch toma.eph.binlog_marker.0)

sudo gdb ${executable_file} ${core_file} << 'END'
	set solib-search-path ./lib64
	set solib-absolute-prefix ./lib64

	set $end_at_buf=(nvmeibt_trace_long->active_buf + 1) % nvmeibt_trace_long->bufs_per_channel
	set $i=0
	set $j=nvmeibt_trace_long->next_flush
	if ($j==nvmeibt_trace_long->active_buf)
		set $j = ($j + nvmeibt_trace_long->bufs_per_channel - 10) % nvmeibt_trace_long->bufs_per_channel
	end
	while $j!=$end_at_buf
		eval "dump binary memory /tmp/trace_daemon/toma.binlog0.%d nvmeibt_trace_long->bufs[%d] nvmeibt_trace_long->bufs[%d]+4096", $i, $j, $j
#		eval "dump binary memory nvmeibt_trace_long%d.0 nvmeibt_trace_long->bufs[%d] nvmeibt_trace_long->bufs[%d]+4096", $i, $j, $j
		set $i=$i+1
		set $j=($j+1)%nvmeibt_trace_long->bufs_per_channel
	end

	set $end_at_buf=(nvmeibt_trace_eter->active_buf + 1) % nvmeibt_trace_eter->bufs_per_channel
	set $i=0
	set $j=nvmeibt_trace_eter->next_flush
	if ($j==nvmeibt_trace_eter->active_buf)
		set $j = ($j + nvmeibt_trace_eter->bufs_per_channel - 10) % nvmeibt_trace_eter->bufs_per_channel
	end
	while $j!=$end_at_buf
		eval "dump binary memory /tmp/trace_daemon/toma.eter.binlog0.%d nvmeibt_trace_eter->bufs[%d] nvmeibt_trace_eter->bufs[%d]+4096", $i, $j, $j
		set $i=$i+1
		set $j=($j+1)%nvmeibt_trace_eter->bufs_per_channel
	end

	set $end_at_buf=(nvmeibt_trace_eph->active_buf + 1) % nvmeibt_trace_eph->bufs_per_channel
	set $i=0
	set $j=nvmeibt_trace_eph->next_flush
	if ($j==nvmeibt_trace_eph->active_buf)
		set $j = ($j + nvmeibt_trace_eph->bufs_per_channel - 10) % nvmeibt_trace_eph->bufs_per_channel
	end
	while $j!=$end_at_buf
		eval "dump binary memory /tmp/trace_daemon/toma.eph.binlog0.%d nvmeibt_trace_eph->bufs[%d] nvmeibt_trace_eph->bufs[%d]+4096", $i, $j, $j
		set $i=$i+1
		set $j=($j+1)%nvmeibt_trace_eph->bufs_per_channel
	end
END

(cd /tmp/trace_daemon; sudo ./pager.py --toma -l toma.eter.binlog toma.binlog) > ../toma_log_from_core_dump.txt
