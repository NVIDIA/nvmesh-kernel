<!--
SPDX-FileCopyrightText: Copyright (c) 2024-2025 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->
# Toma

***
#### ⚠️ How to investigate logs
1. Last topo for clients
`calc_topo_for_clients.*n_seg`
2. Praid generation
   `Working on.*<praid_uuid>`
3. Waiting reasons - not enough owners, local clients are not synchronized
   `leader_is_waiting_for_any_remote_seg_to_apply_topo`
4. Clients sync
```
   recalc_seg_active_registrants_align_with_sync_cmd
   nvmeibt_disk_segment_applied_set_registrants_aligned_with_sync_cmd
   nvmeibt_register_make_all_seg_active_registrants_sync_praid_topology
   praid_leader_calc_praid_version_and_registrants_sync_cmd (sync_cmd)
```
5. Clients messages
   `TR_|RT_ + .*nvmeXXX`
6. TOMA messages
   `nvmeXXX.*APPEND_ENTRIES(_REP)`
   `is_with_raft_log=1` means this APPEND_ENTRIES msg has real data
7. Rebuilds
```
   nvmeibt_recovery_start_rebuild
   Start DIRTY_REBUILD|Done DIRTY_REBUILD|RECOVER_PROGRESS.*left + tid=XXX.*left
   Start EC_COLD_REBUILD|Done EC_COLD_REBUILD
   RT_RECOVER|TR_RECOVER
   remainingDirtyBits
```
8. Segment states
   `DIRTY_BITS\(.*<seg_uuid>`
9. MGMT reports
   `report_target.*long`  , `important` (for segs)
10. INIT_DONE
   - EC: `nvmeibt_ds_metadata_init_EC_locks_table`
   - RAID1: `nvmeibt_ds_metadata_init_non_EC_locks_table`
11. Sync with follower
   `nvmeibt_disk_segment_leader_sync_with_remote_applied` ,
   `nvmeibt_disk_segment_leader_upd_from_peer_applied`
12. Segs states (TOMA log view)
   `nvmeibt_disk_segment_leader_convert_unusable_to_dead` ,
   `praid_leader_check_whether_all_segments_registrants_are_aligned`
13. Last praid topo
   `SECTION NAME: TOPOLOGY_DISK_SEGMENTS_v1.4` ,
   `absorb_append_entries_data`
14. Commited by N/2+1
   `raft_leader_check_majority_and_act_upon`
15. Apply new topo
   `update_applied_topology`
16. Common prints
17. Disk In/Out
   `change_disk_event`
18. Client's log interesting messages:  Ignoring toma message
19. Remote issues
   `nvmeibt_disk_segment_leader_sync_with_remote_applied`,
   `nvmeibt_disk_segment_leader_upd_from_peer_applied`
20. Sync cmd
   `praid_leader_calc_praid_version_and_registrants_sync_cmd` ,
   `nvmeibt_praid_upd_registrants_sync_cmd`
21. Tables inits
```
   nvmeibt_seg_active_init_applied_dirty_and_stale_bits
   nvmeibt_ds_metadata_init_EC_locks_table
   nvmeibt_ds_metadata_init_non_EC_locks_table
```
22. MGMT
   - config received `nvmeibt_handle_new_config_files.*NVMEIBT_READ_CONFIG_MODIFIED`
   - report
```
   write_report_target_message
   write_dirty_progress_status
   write_one_disk_json
   leader_report_line_for_volume
   recalc_praid_report_to_mgmt_json
   calc_seg_lot_status_for_mgmt
```
23. Zeroing `launch_seg_active_zero_task`
25. disks operations
   - `got EVENT_DISK_CHANGE disk=S3HCNX0K700658.1 op=a` - netlink
   - process_waiting_udev_events
26. Disk report `write_one_disk_json`
27. Smart info  `/proc/nvmeibs/smartX`
28. Store processes  `n_stores_in_progress`
29. Disks
```
   DISK_CHANGE_EVENT.*op=
   udev notification action=
   new udev info, dev_file_name
```
30. TOMA NW   `'Node.*accessible'`
31. Flows
```
   nvmeibt_topology_parse_committed_topology
   nvmeibt_topology_apply_the_latest_committed_topology
   nvmeibt_praid_lot_duplicate_content
   nvmeibt_praid_lot_upd_from_praid_mgmt
   nvmeibt_seg_active_upd_active_topo_from_applied_topo
   nvmeibt_read_persisted_config
   raft_read_toma_state_from_persistency
   nvmeibt_write_config_to_persistency
   nvmeibt_mm_json_leader_serialize_baseline_topo_config_to_wire
   nvmeibt_topology_leader_serialize_baseline_topo_to_wire
   praid_leader_serialize_topo
   nvmeibt_praid_leader_calc_topo_main
   nvmeibt_topology_serialize_active_topology
   raft_convert_to_leader
      nvmeibt_praid_reset_due_to_new_raft_leader
   nvmeibt_global_issue_leader_or_jbod_report_status_to_mgmt
   launch_seg_active_zero_task
   seg_active_zeroing_wrapper
   nvmeibt_seg_active_init_locks_table
   nvmeibt_praid_set_segment_and_replacement
   nvmeibt_seg_active_handle_post_update_actions
   leader_increase_version_and_set_clients_sync_cmd_due_to_changed_objects
   encrypt prints
      detach_shadow_vol_for_encryption_finalize
      nvmeibt_run_exec_on_blkdev
      start_encrypt_action
   shutdown
      shutdown_from_management
   nvmeibt_raft_read_persistence_and_upd_committed
   incremental_TARGET_updates_consume
```
32. Kafka messages
```
   consumer_read_msg_from_kafka
      addVolume/addTarget
   producer_send_msg
   rd_kafka_consume.*k_msg
   TOMA.*messageType.*reportTarget, updatePRaidReport, encryptionCommandResponse
   messageType.*hardwareConfiguration, updateVolume, formatDrive
```
33. Get kafka queue entry content: `/opt/kafka_2.12-3.2.0/bin/kafka-dump-log.sh --print-data-log --files /var/lib/kafka/zone1.leader.incrementalUpdates.1.0.0-0/00000000000000000000.log`
34. Search client's logs related to a specific segment
   Example: `seg=(0,1,2)`
   0, 1, and 2 are indexes of chunk, praid, and segment
35. Potential issues in TOMA log `TOMAerr|TOMAwarn`
36. TOMA log when it starts      `Starting TOMA`
37. New leader election result   `new LEADER`

***
#### Trace config
- The TOMA logging is binary and output to syslog (`cat /var/log/messages`)

The files in this directory contain lines that should be put in
`/var/log/nvmesh/trace.config` when attempting to debug TOMA issues.
[Default](./toma_trace.config)
- Each production environment has its own default trace config.
   - Some examples here `toma.d/nvmesh_*.rpc`
   - They can be found in production under `/opt/nvmesh/common-repo/toma.d/`

Open the following traces if toma has hiccups
```
- all
+ filename nvmeibt_important_logs.h
# ------------ nvmeibt_register.c ------------
+ function brute_force_disconnect_registrant
+ function nvmeibt_register_handle_incoming_message
+ function nvmeibt_register_send_msg_to_registrant
# ------------ nvmeibt_disk_segment ------------
+ function nvmeibt_recovery_execute_dirty_bits_recoveries_as_needed
+ function stop_dirty_bits_recovery
# ------------ nvmeibt_ib.c ------------
+ function _mark_conn_usage_by_toma
#+ function poll_cq
+ function do_dying_state_timeout
+ function check_conn_sq_completion_timeout
+ function disconnect_conn
+ function free_remote_by_nic
# ------------ nvmeibt_praid.c ------------
+ function nvmeibt_praid_leader_calc_segments_states_and_owners
# ------------ nvmeibt_topology.c ------------
+ function leader_add_disk_to_node
+ function applied_add_disk_to_node
+ function nvmeibt_topology_applied_remove_disk_from_its_current_node
+ function nvmeibt_topology_leader_remove_disk_from_its_current_node
+ function probe_local_hardware_and_relate_to_config
+ function leader_serialize_global_topology
+ function nvmeibt_topology_serialize_applied_topology
# ------------ nvmeibt_toma.c ------------
#+ function run
# ------------ nvmeibt_read_config.c  ------------
+ function nvmeibt_parse_csv_buf
```
***
### Core dump analysis
#### Get the core and sources
In order to analyze a core from a customer's machine using gdb, you need the sources and the core.
1. Get the libraries from the remote machine
	- `ldd toma/bin/.../nvmeibt_toma | grep -E ' => ' | grep -v 'not found' | cut -d' ' -f3`
	- tar -czhvf ttt.tar `ldd toma/bin/.../nvmeibt_toma | grep -E ' => ' | grep -v 'not found' | cut -d' ' -f3`
	- The logs collector puts it in a file named like "target_MLNX_OFED_LINUX-4.3-3.0.2.1_3.10.0-862.14.4.el7.x86_64-toma-release.tar"
	- If customer does not have toma with symbols find it in our repository on server 3:
		- `cat /etc/redhat-release, ofed_info -s, rpm -qa| grep nvmesh`
		- Choose the exact fitting executable. Example: 10.0.3.3:/home/compilator/RPM_output/.../target_MLNX_OFED_LINUX-4.3-3.0.2.1-rhel7.5-x86_64_3.10.0-862.el7.x86_64/toma/bin/release
		- Your Toma file should be `CORE_FILE_PATH="./core.<numbers here>"`
		- Your Toma file should be `TOMA_EXEC_PATH=".../nvmeibt_toma"` or better `nvmeibt_toma.with_symbols`
	- Extracting Toma sources `gpg -d -z6 --batch --pinentry-mode default --passphrase AlexanderRonen > toma.tar < nvmeibt_toma_src_tar1.pgp; tar -xpvf toma.tar ./toma`;
2. Alternative to the above, if Toma crash is in a CI run use
	- copyCI command from devel bash
	- Example: Crash in http://mtv-excelero1:8080/job/MTV-CI-Build/20017/artifact/build/ROCE.stage-log.html
	- Use copyCI 20017
		- find where the core is: `find . -name core.*`.
		- script creates toma_src_dir, go inside.
		- Extract the core. Something like: `lz4 -d ./...../var/log/nvmesh/trace_daemon_snapshot__...../core.nvmeibt_toma.0........lz4 > core_file`
		- Your Toma file should be in `CORE_FILE_PATH="./core_file"`
		- Your Toma file should be in `TOMA_EXEC_PATH="./bin/release/nvmeibt_toma"`
3. Copy the libraries to the developer's machine and untar. The interesting libraries are lib64 (most libs) and lib (Kafka lib).
#### GDB
1. Launch gdb:  `gdb ${TOMA_EXEC_PATH} ${CORE_FILE_PATH}`. Preferably use the nvmeibt_toma.with_symbols
2. Inside gdb do
	- `set solib-search-path <the_lib64_path>:<the_lib_path>` . Path like:  ~/Downloads/ttt_unzipped/lib64 and lib
	- `set solib-absolute-prefix <the_lib64_path>:<the_lib_path>`.
	- `t apply all bt`
	- Continue as usual with gdb

#### Extract latest binary logs from a core:
1. Create or select a temporary directory, e.g. /tmp/logs
2. cp /var/log/nvmesh/trace_daemon/dict* /tmp/logs
3. cp /var/log/nvmesh/trace_daemon/pager* /tmp/logs
4. In gdb, run these commands:
```
cd /tmp/logs
set $i=0
while $i<=nvmeibt_trace_long->active_buf
eval "dump binary memory toma.binlog0.%d nvmeibt_trace_long->bufs[%d] nvmeibt_trace_long->bufs[%d]+4096", $i, $i, $i
set $i=$i+1
end
```
5. `cd /tmp/logs && ./pager.py --toma | less`
6. Daniel HsH: there is a script [here](./extract_log_from_core.sh) Not sure how to use it in this section

#### Convert TOMA persistence to json
1. Use `scripts/toma_persistence_to_JSON.sh`
2. Most recent TOMA persistence file is `/var/opt/nvmesh/toma/toma_persistence_*.0`
3. Those ending in 1, 2, 3, ..., are old persistence files.

#### Get TOMA status
- To generate status file: `pkill -10 nvmeibt_toma`
	- Default path of stat is `/var/log/nvmesh/toma_*.stat`
- To live monitor status: `/proc/nvmeibs/toma_status/all`

****
#### Execution types

| Exe  Type    |            What                         | Prod | Dev |
|--------------|-----------------------------------------|-------|-----|
| Toma         | Regular production control path         | ✅   |      |
| gpt util     | Reading/Fixing partitions on local disk | ✅   |    |
| config_util  | Editing persistency when Toma is down   | ✅   | 🚧 |
| Toma Simu    | Sandbox testing                         |       | 🚧 |

***

