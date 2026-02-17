# NVMesh 3.4.0 Module Params Guide

<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

## Table of Contents

- [NVMesh 3.4.0 Module Params Guide](#nvmesh-340-module-params-guide)
  - [Table of Contents](#table-of-contents)
- [​Copyright and Trademark Information](#copyright-and-trademark-information)
- [​Preface](#preface)
- [​Acronyms and Terms](#acronyms-and-terms)
- [Module Parameters](#module-parameters)
  - [Tracer Severities](#tracer-severities)
  - [nvmeiba](#nvmeiba)
  - [nvmeibc](#nvmeibc)
  - [nvmeib\_common](#nvmeib_common)
  - [nvmeib\_common\_public](#nvmeib_common_public)
  - [nvmeibs](#nvmeibs)
  - [siw](#siw)

# ​Copyright and Trademark Information

© 2026 NVIDIA All rights reserved.

Specifications are subject to change without notice.

NVMesh® is a registered trademark of NVIDIA.

All other brands or products are trademarks or registered trademarks of their respective holders and should be treated as such.

# ​Preface

**<u>Audience</u>**

The primary audience for this document is intended to be storage and/or application administration personnel responsible for installing and deploying NVMesh.

**<u>Feedback</u>**

We continually try to improve the quality and usefulness of documentation. If you have any corrections, feedback, or requests for additional documentation, send an e-mail message to <nvmesh-documentation@nvidia.com>.

# ​Acronyms and Terms

| Acronym | Description |
| --- | --- |
| Hidden volume | A hidden volume is volume attached to a client for the client to perform recovery operations on it. This should only happen on targets. <br>As volume is only attached for recovery by the storage system, it does not have a /dev device |
| NVLustre | Lustre-over-NVMesh. |
| RDMA IO | This is IO executed using RoCE or Infiniband for communication. |
| SIW | SoftiWarp, which provides an RDMA API, but performs communication over TCP without RDMA. <br>Often referred to as TCP in module parameter names. |
| SIW IO | This is IO executed using SIW for communication, in contrast to RDMA IO. |

# Module Parameters

For any module, it is possible to obtain a description of the module’s parameters using:

modinfo &lt;module name&gt;

## Tracer Severities

Tracer severities are defined by these values:

- 1 = Error

- 2 = Warn

- 3 = Info

- 4 = Trace

- 5 = Debug

- 6 = Fine

## nvmeiba

| Parameter | Description |
| --- | --- |
| verbose_debug | Defines logging level. <br>This is disabled by default for NVMesh versions compiled for production use. If a non-production version is used, the default is true, i.e., enabled. <br>For NVLustre, set explicitly to false, in case non-production versions are used. |
|  |  |

## nvmeibc

| Parameter | Description |
| --- | --- |
| bio_noexec | This is for debugging. When set to true, BIOs (kernel block IOs) are ignored instead of being executed. <br>Default is false. |
| cfg_id | Client configuration profile ID. |
| cfg_name | Client configuration profile name. |
| cfg_version | Client configuration profile version. |
| cli_attach_check_if_already_attached | Debug only: used to simulate and test race conditions of cli attach. Default is true and should not be changed. |
| debug_level | Enables debug logging (to the system log not NVMesh tracer) if set above 1, which is the default value. Deprecated. |
| default_dir_lsblk | Defines the directory within /dev in which block devices will be generated. Can be left empty to use /dev. <br>The default value is “nvmesh”. |
| disk_locks_fast_reuse | Reuse a disk lock (disk-lock opr) before calling the callback from releasing the lock (ulp cb). This is a boolean. This is a potential optimization. <br>Default is false. |
| disk_nrch_defer_block_cb | Defines whether to defer the IO callbacks until after reenabling interrupts. Default is true. |
| disk_pause_at_first_discover | Defines whether to set the disk as paused for the first discovery to make attach operations faster. False simulates pre-2.6 behavior. <br>Default is true. |
| disk_pcpu_nrch_poll_proc | Allow polling of per-cpu IO channels through a proc file, which is useful for running kernel IO from SPDK. Useful for initial SNAP versions that did some IO from the kernel. <br>Default is false. |
| disk_prio_pending | Defines whether to prioritize IO in the pending IO queue, which hold both locks and journal entries. <br>This was added to improve the performance of EC rebuilds. <br>Default is true. |
| ec_reuse_req | Enable reusing feature for requests (EC). This parameter was added to facilitate disabling this reuse as a potential optimization for NVMesh in DPU mode. <br>Default is true. |
| elect_max_destages_per_dev | Experimental for future functionality. |
| force_reconf_reboot | Force all block device configuration changes to be done via device reboot, i.e. restarting the block device. <br>Default is false. |
| goodpath_debug_level | This determines the level of tracing for the regular data path. <br>Only traces with this level or lower will be issued, see tracer severities above. |
| goodpath_locks_debug_level | This determines the level of tracing for the regular data path locks. <br>Only traces with this level or lower will be issued, see tracer severities above. |
| goodpath_transport_debug_level | This determines the level of tracing for the regular data path networking. <br>Only traces with this level or lower will be issued, see tracer severities above. |
| guids | Used for port filtering functionality. <br>Typically populated from nvmesh.conf parameters. |
| ioch_ka_only_no_rdda | Use only IO channnels for IO keep alive messages, requires disk discovery to take effect. <br>Default is true. |
| io_max_retry_secs | The maximum time in seconds to try and execute IO before failing it. <br>The default is 0, which is to use the kernel's default. |
| jam_max_used_entries | Sets the maximum journal entries to be used by the JAM (journal allocation manager). <br>The default is 0, which is to use the built-in default. |
| jam_non_free_entry_timeout | JAM timeout for having a journal entry in a non-free state in seconds. <br>Default is 300. |
| jam_pending_enb | Controls whether to enable or allow pending allocations on the JAM. <br>Default is true. |
| jam_pending_req_timeout_jif | JAM timeout for pending allocation request in jiffies. <br>If set to 0, use system default. <br>Default is 10 milliseconds. |
| local_io_use_md_dma_pool | When a local IO request is made without providing space for the metadata buffer and the drive has metadata enabled, then this determines whether to use a preallocated pool of memory or to dynamically allocate memory per IO. <br>Default is true. |
| local_io_use_prpl | Determines whether local IO requests use PRPLs instead of SGLs (see the NVMe standard for more information). PRPLs are needed for environments with the IOMMU enabled. <br>Default is true. |
| local_io_use_rd_md_pool | Determines whether to use a preallocated pool or to dynamically allocate memory for metadata read operations, as the NVMe standard forces reading the block data with the metadata. <br>Default is true. |
| lock_ch_2nd_ch_pcpu | Enable, disable or set the number of secondary lock channels as per-cpu lock channels. <br>Offline CPUs within the configured CPUs range are not compensated for. <br>Possible values: <br>0 = disabled. This is the default. <br>1 = use max possible channels (min(num-cpus, 128)) <br>Other value (when lock_ch_pcpu_cpus="") = cpu [0, i) use a per-cpu secondary-channel [0, i). Other cpus share the primary lock channel. |
| lock_ch_get_method | Determines the method for choosing the lock channel for RDMA communication. <br>Possible values: <br>0 = LRU - tie break by sharding. This is the default. <br>1 = BY_CPU <br>2 = SHARDING by destination address |
| lock_ch_get_method_tcp | Determines the method for choosing the lock channel for TCP communication, same values as for RDMA, see above. |
| lock_ch_pcpu_cpus | A list of CPUs on which to pin the secondary per-cpu lock channels. <br>The format is a hex-mask list where each entry is 32-bits , e.g., 1f,ff for CPUS 0-7, 32-36). <br>If the list is empty, use all cores. This is the default. |
| lock_ch_scq_offload_thread | Use a thread for offload processing for RDMA shared completion queue handling. <br>Default is true. |
| lock_ch_scq_offload_thread_tcp | Use a thread for offload processing for SIW shared completion queue handling. <br>Default is true. |
| lock_ch_scq_use_kwq | Determines whether to use kernel workqueue instead of kthread for SCQ offload processing. <br>Default is false. |
| locks_scq_wq_unbound | Determines whether to use an unbound kernel workqueue for nvmeibc_locks_scq (true) or a bound one (false). <br>Default is false. |
| lock_retry_delay_multiplier | Lock retry timeout in microseconds when failing to obtain a lock. The retry timeout undergoes exponential backoff. <br>Default is 5000. |
| management_report_frequency | Report frequency for management in seconds. <br>Default is 5. |
| map_each_sg_entry | IB DMA map each SG-entry separately. <br>Default is false. |
| map_sg_mode | Defines the mode for mapping data SG lists: <br>0 (default) = Combined, use global key when able to collapse all sg-entries to one, otherwise map-mr, i.e., map WR and rdma-write WR. <br>1 = Use only map-mr (IB_WR_REG_MR, IB_WR_FAST_REG_MR). <br>2 = Use only the global dma key, which means that multiple rdma-write WRs from clnt/srv in wr/rd, respectively, may be needed. The target must have enough WRs to write the data back for read operations. |
| map_sg_result_trace | Defines how to trace the result of mapping the data SG list: <br>0 (default) = Disabled <br>1 = One-shot (edge) <br>2 = Continuous (level) |
| max_comp_intr_pct_cpu | Max percentage of CPU time to spend processing completions in an interrupt before entering poll mode. <br>Default is 10. |
| max_ioch_path_fail | Number of failures allowed per path in a connection cycle. <br>Default is 1. |
| max_ioch_rm_works | Max concurrent IO communication channel removal operations. <br>Default is the number of cores in the server. |
| max_ios_per_cpu | Maximum number of concurrent IO operations handled per core. Can be used to prevent IO flooding. <br>In other words, the upper limit on the number of outstanding IOs to issue via the block driver per CPU core. Some file systems and applications queue or perform read-ahead very aggressively, likely to overcome problems with legacy storage solutions. With NVMesh, large numbers of outstanding read requests may lead to network congestion especially when target bandwidth exceeds client bandwidth. Throttling the number of outstanding requests using this parameter can reduce this congestion and improve overall quality of service. Limiting this value often ends up improving performance for the Client and others on the network. If in doubt, start with a value of 8. <br>This setting can be applied dynamically to the kernel module without restarting services. <br>Default is 64. |
| max_lock_channels | The maximum number of lock channels for non-TCP transports. <br>Default is 5. |
| max_lock_channels_tcp | The maximum number of lock channels for TCP transports. <br>Default is 16. |
| max_nic_srqs | Maximum number of shared receive queues per NIC. <br>Default is 16. <br>For NVLustre production clusters, set to 32. |
| max_rcomp_intr | Max number of recv completions to handle in an interrupt before entering poll mode. <br>Default is 64. |
| max_trim_size_mirrored | Maximum size of a single NVMesh internal TRIM operation for mirrored volumes. The value is for multiples of 128 KB. <br>Default is 128, which is equivalent to 16 MB. |
| max_trim_size_non_mirrored | Maximum size of a single NVMesh internal TRIM operation for non- mirrored volumes. <br>The default had been 256, which translates to 32 MB. <br>This was raised to 16384, which translates to 2 GB, to address NVMESH-2000. |
| mini_elevator | Enable mini-elevator which combines writes to erasure coded volumes to fill stripes. <br>This functionality was experimental (and promising), but did not reach production quality. <br>Default is false. |
| no_part_scan | Disable partition scan on nvmesh block devices. <br>Default is false. |
| nordda_wq_unbound | Determines whether to use an unbound kernel workqueue for nvmeibc_nordda (true) or bound (false). <br>Default is false. |
| nr_defer_recv_comps | Defer processing of IO completions for RDMA transports. <br>Default is true. |
| nr_defer_recv_comps_tcp | Defer processing of IO completions for RDMA transports. <br>Default is false. |
| nr_defer_recv_comps_use_kwq | Determines whether to use a kernel workqueue for deferred receive completions on nordda channels. <br>Default is false. |
| nr_get_by_cpu_index | Determines how to choose an IO channel for RDMA transport, as follows: <br>0 (default) = use non-core based method as defined elsewhere <br>1 = by CPU core only from this core’s channels <br>> 1 = by CPU core, and if not available fallback to non-core based method. |
| nr_get_by_cpu_index_tcp | Determines how to choose an IO channel for SIW transport, as follows: <br>0 = use non-core based method as defined elsewhere <br>1 (default) = by CPU core only from this core’s channels <br>> 1 = by CPU core, and if not available fallback to non-core based method. |
| nr_get_least_used | Get least used IO channel for RDMA transport. This improves performance in certain scenarios, such as congested multi-NIC access to a small number of remote drives. <br>Default is false. |
| nr_max_channels_per_disk | Maximum number of RDMA IO channels per disk. <br>Default is 64. <br>For NVLustre production clusters, set to 8. |
| nr_max_channels_per_path | Maximum number of RDMA IO channels per disk per networking path. <br>Default is 4. |
| nr_max_channels_per_path_tcp | Maximum number of SIW IO channels per disk per networking path. <br>Default is 16. <br>For NVLustre production clusters, set to 3. |
| nr_max_channels_per_path_iommu | Maximum number of RDMA IO channels per disk per networking path when the IOMMU is enabled. <br>Default is 4. |
| nr_max_used_reqs_per_channel | Maximum number of requests issued simultaneously on a channel. <br>Default is 64, can be increased up to 96. <br>For NVLustre production clusters, set to 8. |
| nr_pcpu_channels_per_disk | Connect per-cpu RDMA IO channels (up to 128) in addition to the nr_max_channels_per_disk any-cpu channels. <br>The total number of channels between a client and a target’s disk is limited by the lower of nr_max_channels_per_path on the Client and the Target. <br>Typically set to true for kernel-based DPU usage. <br>Default is false. |
| nr_pcpu_ch_ll_cpus | A list of CPU cores for lockless per-cpu IO RDMA channels. The format is as a hex-mask list of cores, where each entry is 32-bits. <br>Default is “” (an empty string). |
| nr_pcpu_ch_lockless | Per-cpu RDMA IO channels are lockless. <br>This reduces contention and increases performance, but requires a lot more channels typically. <br>Default is false. |
| nr_rotate_in_pending | Rotate RDMA IO channel list when reusing channels for pending commands (which did not have a free slot in the channel previously). <br>This improves performance in certain scenarios leading to more balanced use of channels. <br>Default is false. |
| nr_shared_cq | Use a shared completion queue (SCQ) for RDMA IO. <br>Default is true. |
| nr_shared_cq_tcp | Use a shared completion queue (SCQ) for SIW IO. <br>Default is false. |
| nr_skip_rdma_write | This is an unsafe debug mode. <br>RDMA IOs skip the RDMA write for write operations. <br>This will always work on Legacy volumes and on EC when CRC check is off and block size is equal to the slice length of the volumes, but it will not store the right data! <br>Used for performance testing only. <br>Default is false. |
| nr_store_fr | Enables a workaround for RDMA resource usage to avoid rare RDMA protection errors in EC writes. <br>Default is true. <br>Consult support before changing. |
| nr_use_srq | Use a shared receive queue (RCQ) for RDMA IO. <br>Default is true. |
| nr_use_srq_tcp | Use a shared receive queue (RCQ) for SIW IO. <br>Default is false. |
| nr_wd_long_timeout | IO watchdog timeout in jiffies. <br>Default is 0, which means 10 seconds. |
| nr_wd_rescue_timeout | IO watchdog rescue timeout in jiffies. An IO watchdog resuce is an attempt to handle any missed receive interrupts even through there was no interrupt. This functionality was an escape and is considered unnecessary. <br>A value under 1 second disables this functionality. <br>Default is 0, i.e. disabled. |
| num_warnings | Returns the number of warnings the module has triggered. <br>Contact support if not 0. |
| nvmeibc_copy_bio_buffers | Copy bio buffers in writes. Should be on except for specific file systems that never write into a buffer during IO. <br>Default is true. |
| nvmeibc_debug_ram_binfo | Enforce detection of topological data corruptions in RAM. <br>Default is true. |
| nvmeibc_default_debug_di | Upon volume attach, enable “debug di” mode. <br>Default is false. <br>Consult support before changing! |
| nvmeibc_jentry_num_blocks | Length of erasure coding journal, in blocks. <br>For erasure coding volumes, increasing this means fewer parallel write IO operations, but more efficient large writes. It is highly recommended to increase for use cases with large writes. <br>Range is 1 to 16. <br>Default is 16. |
| nvmeibc_jmd_wr_version | Version of JMD (journal metadata) to use to faciliate backwards compatibility: <br>0 (default) = packed <br>1 = unpacked <br>Consult support before changing! |
| nvmeibc_should_sync_reuse_memory | Reuse pages across syncs (internal storage recovery operations) to reduce the number of page allocations and deallocations. <br>Default is true. |
| nvmeibc_sync_full_lockset_probability_factor | Defines the probability of sync’ing full 128K blocks instead of the current IO requested. It is used to avoid very slow IO during recovery, while avoiding wasteful repeat synchronizations. If the entire block is not synchronized, this will still need to be done by the regular recovery mechanism. <br>The probabillity is computed by multiplying the number of blocks in the IO x <param value> / 3200. <br>Range is 0 – 3200. <br>0 = never synchronize the full block. <br>100 (default) = 1/32 chance for a 4k IO ; 50% chance for a 64K IO <br>3200 = always |
| nvmeibc_sync_max_operations_per_dev | Maximum number of outstanding sync (recovery) operations per volume. <br>Default is 384. <br>Maximum is 6144. |
| nvmeibc_warn_on_edic_werification_failure | Issue kernel warning upon CRC-based read block verification failure. Useful for detecting data that has been correct on drives. <br>Default is true. |
| pages_max_alloc | Maximum (kernel) order of page allocations allowed. <br>Default is 31. |
| panic_on_core_dbgdi | In “debug di” mode, panic on detection of an issue, on both client and target. This is an internal debugging facility. <br>Default is false. |
| pcpu_cq_poll_proc | Create /proc files for polling the nvmeibs shared completion queues from SPDK. <br>This requires pcpu_cq_all_cpus=Y for nvmeib_common. <br>Default is false. |
| per_cpu_lock_transfer_num | The number of lock transfer candidates or slots per CPU. <br>Lock transfers are used to optimize serial writes and transfer lock ownership from one IO to another to avoid having to wait for it to be released and then acquired again. <br>Default is 8. <br>For NVLustre production clusters, set to 32. |
| ports | Used for port filtering functionality. This is typically set by service startup based on nvmesh.conf information. |
| profiling_enabled | Enable statistics gathering, should be turned of if the clocksource is not tsc. <br>Default is true. |
| qa_ec_stress_debug | For QA only! <br>Stresses the EC datapath. <br>Default is false. |
| rdda_pois_bb | Fills the remote buffer with a special value (“poison”) to avoid memory corruption errors. This is a 2nd level of protection. <br>RDDA is no longer supported. <br>Default is true. |
| recovery_iterator_cooldown | Recovery iterator timeout to wait after completing full recovery cycle in jiffies. Increasing this trades recovery load vs. recovery time. <br>Default is 500 (0.5 seconds). |
| restart_io_timeout_secs | Time in seconds to wait between full cycles of IO channels reconnection. Upon a disconnection, reconnection attempts will be more often and exponentially backoff as needed up to this value. <br>Default is 30. |
| resub_awake_throttle_sleep_ms | Resubmit thread’s sleep time for throttling, in milliseconds. <br>Should be in the order of scheduler process switching. <br>Default is 10. |
| resub_awake_throttle_threshold_ms | Resubmit thread’s threshold for throttling, in milliseconds. <br>The thread will yield after running for this amount of time. <br>Default is 1000. |
| self_recovery_detach_carrier_grace_time_sec | Experimental for future functionality. |
| self_recovery_detach_idle_time_sec | If a hidden volume is idle for this time, then it auto-detaches, in seconds. Default is 60. |
| self_recovery_detach_initial_time_sec | If a hidden volume is idle for this time after initially attaching, then it auto-detaches, in seconds. Default is 5. |
| skip_disk_iocmds_flags | This is an unsafe debug mode. <br>Skip disk access (remote and local): <br>0 (default) = Disabled <br>1 = Skip read operations <br>2 = Skip write operations <br>3 = Skip read & write operations <br>4 = Skip journal write operations or any combination using this operation <br>5 = Skip all IO operations – including those not mentioned above <br>Used for performance tuning and debugging. |
| skip_lock_cmds_flags | This is an unsafe debug mode. <br>Skip locking operations for non-EC volumes (remote and local): <br>0 (default) = Disabled <br>Bit-0 = skip cmp_exchange (regular locks) <br>Bit-1 = skip active table locking <br>Bit-2 = skip read locks <br>Bit-3 = skip writing block-info <br>Used for performance tuning and debugging. |
| sm_th | Maximum number of concurrent Infiniband subnet manager requests. Used for throttling subnet manager access. <br>Default is 32. |
| spread_resub_work | Controls whether and how to spread lock resubmission work to other cores: <br>0 (default) = No spreading <br>1 = SysWq (system workqueue based). This may improve throughput for serial write workloads. |
| tcp_mode | Activate the SIW communicate mode exclusively, i.e., filter out any RoCE devices. Usually set by service startup from nvmesh.conf information. <br>Default is false. <br>NVLustre at OCI sets this to true. |
| topology_debug_level | This determines the level of tracing for topology operations, i.e. changes to volume health and layout, for this module. Only traces with this level or lower will be issued, see tracer severities above. |
| tracer_debug_level | This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above. |
| unprotected_write_period_seconds | Deprecated. <br>Timeout in seconds for an unprotected volume until it becomes read-only. <br>Default is infinite. |
| use_async_subscribe | Determines whether to run “subscribe” operations asynchronously. <br>Subscribe operations are used for clients to subscribe to TOMA for instructions regarding a volume’s disk segment. <br>Default is true. |
| use_block_extrenal_major | Determines whether to use a dedicated block external major for the NVMesh block devices. This is rarely required. <br>Default is false. |
| use_local_bypass | Access local drives directly and not via a NIC, i.e. over the network. Mainly used for debugging local disk access. <br>Default is true. |
| use_norrda_for_io | Allow using non-RDDA operations for IO. As RDDA is deprecated, this should always be true. |
| use_only_norrda_for_io | Use only non-RDDA operations for IO. As RDDA is deprecated, this is meaningless. <br>Default is false. |
| use_pcpu_cq | Use a per-cpu shared completion queue (SCQ) and shared receive queue (SRQ). <br>Default is false. |
| use_rdda | Allow using RDDA operations for IO. As RDDA is deprecated, this should always be false. |
| warn_if_lock_took_more_than_n_msec | If lock acquisition takes more than this value in milliseconds, issue a warning to the log. <br>Default is 20000, i.e., 20 seconds. |
|  |  |

## nvmeib_common

| Parameter | Description |
| --- | --- |
| cm_ephemeral_debug_level | This determines the level of ephemeral tracing for the RDMA connection manager (CM) for this module. Only traces with this level or lower will be issued, see tracer severities above. |
| cq_vec_flags | CQ (completion queue) completion-vector selection flags, as follows: <br>Bit 0: Reserve vec 0 for userspace. <br>Bit 1: Index based, set by CQ creator. <br>Bit 2 : Use the same vector for SCQ/RCQ. <br>The default value is 0. <br>Consult support before changing. |
| cq_vec_flags_tcp | Same as cq_vec_flags for TCP (SIW) completion queues. |
| cq_vec_snd_rcv_delta | If cq_vec_flags (see above) it set to use index-based selection for the vector, then this value will be the delta between the send and receive queue’s vector. <br>The default value is 0. |
| cq_vec_snd_rcv_delta_tcp | Same as cq_vec_snd_rcv_delta for TCP (SIW) completion queues. |
| debug_level | Enables debug logging (to the system log not NVMesh tracer) if set above 1, which is the default value. |
| ib_cross_subnet | Determines whether to attempt to communicate across IB subnets. <br>Default is false. |
| ipv6_mode | Determines IPv6 Mode, as follows: <br>0 – No IPv6 <br>1 – IPv6 enabled, but prefer IPv4 addresses (default) <br>2 – IPv6 enabled and preferred <br>3 – IPv6 Only <br>The default will be altered by the IPV4_ONLY and the IPV6_ONLY configuration settings in /etc/nvmesh/nvmesh.conf. |
| iwarp_cm_inv_time_sec | Timeout for SIW (iWARP) CM Invalidate in seconds. <br>The default is 30 seconds. |
| iwarp_find_path_sock | Use a socket for iwarp_find_path, i.e., SIW path discovery. <br>This reduces the load on the SIW connection manager’s workqueue (siw_cm_wq). <br>By default this is true. |
| iwarp_find_path_sock_port | The listener port number to use for SIW path discovery. <br>The default port is 8915. |
| json_iostats_fixed_size | Determines whether to pad JSON iostats to a fixed size for readability. <br>The default is false. |
| mlx5_rdda_blacklist | A comma-separated list of firmware versions for which RDDA is blacklisted. |
| nic_blacklist | A comma-separated list of NICs to blacklist, i.e. not use. |
| numa_alloc_granularity | Defines the number of pages on the same NUMA node before traversing to the next one when doing large memory allocations. <br>Default is 64. |
| numa_alloc_policy | Defines the memory allocation policy per NUMA for large allocations. <br>0 – kernel-defined, usually where the allocating thread was run. This is the default value. <br>1 – Round robin on all NUMA nodes. <br>2 – Round robin on nodes within the same CPU socket as the PCI device associated with the allocation. |
| num_warnings | Returns the number of warnings the module has triggered. <br>Contact support if not 0. |
| pcpu_cq2srq_size_margin | When employing a per CPU shared completion and receive queue, this determines how much bigger the SCQ is than the SRQ. <br>Margin = CQ-size – SRQ-size. <br>Default is 1024. |
| pcpu_cq_all_cpus | Allocated CQ per online-cpus (per the Linux kernel) per device, which always process completions in a thread context, i.e. on the ipoller. <br>Default is false. |
| pcpu_cq_comp_vecs_per_dev | Y – use first N comp-vectors of device where N=‘max num of pcpu-cqs per device’ <br>N – use all comp-vectors spread globally between all devices, which is usually not recommended. <br>Default is true (Y). |
| pcpu_cq_flush_del_qps | percpu cqs flush QPs pending for deletion (bool). <br>Do not change with consulting support. <br>Default is false. |
| pcpu_cq_intr_budget | percpu cqs interrupt-mode’s budget (uint). <br>This is the maximum number of completions to handle in a single interrupt. <br>Default is 4. |
| pcpu_cq_max_cqs_per_dev | Maximum number of percpu cqs per device. <br>If set to 0, use system default (uint). <br>Default is 0. |
| pcpu_cq_poll_budget | percpu cqs polling-mode’s budget (uint). <br>This is the maximum number of times to poll for a completion. <br>Default is 256. |
| pcpu_cq_size | The length or size of the shared completion queue when employing a per CPU shared completion and receive queue. <br>Default is 4096. |
| pcpu_cq_user_poll_budget | percpu cqs user-mode polling budget (uint). <br>This is the maximum number of times to poll for a completion. <br>Default is 64. |
| pcpu_cq_user_poll_timeout_msecs | Timeout to switch from user polling back to ipoller for a CQ. <br>Default is 100 msecs. |
| qp_retry_cnt | QP retry count, as defined for RDMA QPs. <br>Default is 7. |
| qp_timeout | QP timeout (4.096 × 2^N) microseconds, as defined for RDMA QPs. <br>Default (N) is 14, which translates to 67 milliseconds. |
| tcp_base_port_id | The first (base) port ID for secondary SIW (iWARP) listeners. <br>Default is 7915. |
| tcp_num_ports | The number of secondary SIW (iWARP) TCP ports. <br>0 = number of CPUs. <br>Default is 16. |
| tracer_debug_level | This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above. |
|  |  |

## nvmeib_common_public

| Parameter | Description |
| --- | --- |
| config | This parameter can be used to alter the binary tracer engine configuration. <br>This string can be up to 4 kbytes. <br>The tracer’s configuration defines the resources consumed by the tracer, its performance, and aspects of its ephemeral behaviour. <br>Consult support before changing . |
| hide_warnings_stack | Hide warnings from dmesg, the kernel log, while keeping them in the binary traces. <br>This is enabled by default for NVMesh versions compiled for production use. If a non-production version is used, the default is false, i.e., not to hide the warnings. |
| ib_odp_info | Defines whether On-Demand-Paging is enabled for RDMA usage and NVMesh should use it. <br>1 – Enabled <br>0 – Disabled <br>-1 – Auto <br>By default, which is equivalent to setting auto (-1), then this parameter is the result of a count from the grep of ib_umem_odp_get from /proc/kallsyms. <br>On-demand-paging enables using RDMA on non-pinned memory pages. |
| ipoller_poll_duration_jif | ipoller poll duration till reschedule, 0=default (uint). <br>This is used for RDMA completion queue handling, albeit it can be used for other purposes as a generic NVMesh infrastructure component. |
| num_warnings | Returns the number of warnings the module has triggered. <br>Contact support if not 0. |
| tracer_dbg_level | This determines the level of tracing for debugging the tracer itself. <br>0-2 do not generate traces. <br>3-6 generate error traces. <br>7+ generate info traces also. |
| tracer_dbg_mask | Facilitates debugging the tracer system. |
| tracer_debug_level | This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above. |
| tracer_wq_debug_level | This determines the level of tracing for work queues for this module. Only traces with this level or lower will be issued, see tracer severities above. |
| wq_max_processing_time | The maximum wq (workqueue) processing time before the workqueue reschedules itself in jiffies. Default is 3000 jiffies. |
|  |  |

## nvmeibs

| Parameter | Description |
| --- | --- |
| cap_transfer_size | Cap all disks’ max-transfer-size to 128 KB, even if the drive supports larger transfers. <br>Default is true. |
| debug_level | Enables debug logging (to the system log not NVMesh tracer) if set above 1, which is the default value. Deprecated. |
| defer_process_io_cq | Defer all IO completions to a per completion queue thread, so it is not done in the interrupt context. <br>Default is false. |
| defer_recv_comps | Defer handling of IO receive completions, so it is not done in the interrupt context. <br>Default is false. |
| disk_collect_stats | Enable collecting statistics for disk operations. Can be used for performance optimization. <br>Default is false. |
| distr_intr_program | Path to the interrupt distribution program for NVMe device interrupts. <br>Default is “/opt/NVMesh/common-repo/scripts/nvmesh_set_irq_affinity”. |
| dummy_id | Serial ID to be used for drives on drive-less targets. <br>Default is empty. <br>Dummy drives are rarely needed, only for an arbiter on a 2-node system. |
| fake_large_disks | Do not use for production systems. <br>Fake the system having larger disks by overriding their size. <br>This is used for developing support for larger drives. <br>Default is false. |
| fake_large_disk_size_lba | Do not use for production systems. <br>The size of fake large disks, in 4k units. <br>Default is 8589934592, which translates to 32 TiB. |
| fake_serial | Do not use for production systems. <br>Fake serial number for a fake NVMe drive, which should be machine specific. <br>Default is empty. |
| format_timeout_seconds_seconds_try | Seconds to wait for an NVMe format to complete on a second attempt after a failed first attempt. <br>Default is 3600, i.e., 1 hour. |
| gcp_drives_to_uuid_list | Related to GCP mode, i.e., specifically for GCP virtual NVMe drives. <br>This provides a list of UUIDs of drives to be used. This should be provided on module invocation, i.e., during Target service startup. <br>Default is empty. |
| gcp_mode | Use only drives that are specified in gcp_drives_to_uuid_list. <br>Default is false. |
| goodpath_debug_level | This determines the level of tracing for the regular data path. <br>Only traces with this level or lower will be issued, see tracer severities above. |
| guids | Used for port filtering functionality. <br>Typically populated from nvmesh.conf parameters. |
| ib_port_prio | Defines the priority of Infiniband, ROCE is 10 by default and SIW is 20. Enables overriding the form of network transportation to prefer. <br>See roce_port_prio and tcp_port_prio also. <br>Default is 0, lower is preferred. |
| ignore_disks | Comma separated list of PCI IDs of NVMe drives to ignore. <br>For example, “0000:03:00.0,0000:12:01.0”. <br>Default is empty string. |
| ignore_disks_serials | Comma separated list of serial IDs of NVMe drives to ignore. <br>For example, “S23YNAAH201234,S23YNAAH202345”. <br>Default is empty string. |
| ioka_timeout_sec | Keepalive timeout failure for an IO channel, in second. <br>Default is 8. |
| iommu_enabled | Informs the internal NVMesh NVMe driver that the IOMMU is enabled on the node. <br>Default is false. |
| jgc_avail_ent_low_wm_div | Triggers JGC (journal garbage collection) if the available range of entries falls below the low watermark of total-range-entries \* mult / div. <br>Default is 8. |
| jgc_avail_ent_low_wm_mult | Triggers JGC (journal garbage collection) if the available range of entries falls below the low watermark of total-range-entries \* mult / div. <br>Default is 8. |
| local_skip_disk_access | Unsafe debug mode. <br>Skip local disk access, i.e., complete disk operations immediately instead of performing them. Used for debugging and performance optimization. <br>Default is false. |
| max_client_rsrc | RDDA is deprecated. <br>Maximum number of RDDA connections per client. <br>Default is 0. |
| max_completions | Maximum number of networking completions to handle per interrupt. <br>Default is 64. |
| max_local_nvmeqs | Maximum NVMe queues for local operation. <br>A value of 0 sets the actual maximum to the lower of the number of CPUs, drive queues, doorbells and MSI-X interrupts available. <br>The default is 0. The value is replaced by the actual number calculated. |
| max_nic_srqs | Maximum number of shared receive queues to define per NIC. <br>Default is 16. <br>For NVLustre production clusters, set to 32. |
| max_outstanding_cm_work_items | Max outstanding CM (connection manager) work items. <br>Important for larger environments using RDMA. <br>Default is 8. |
| max_req_size | Maximum size of client-target messages. <br>Default is 4096 (bytes). |
| min_local_nvmeqs | Minimum number of NVMe queues per drive to reserve for non-RDDA usage. As RDDA is deprecated, this is obsolete. <br>Default is 1. |
| mostly_idle_ch | Defines whether to use the first shared CQ for “mostly” idle channels. <br>Default is false. |
| nordda_kernel_wq_unbound | Determines whether to use an unbound kernel workqueue (true) or a bound one (false). <br>Default is false. |
| nordda_use_kernel_wq | Determines whether to use a kernel workqueue for nordda deferred IO commands (reduces IRQ latency). <br>Default is true. |
| nr_max_channels_per_path | The maximum number of RDMA IO channels per network path. <br>Default is 4. |
| nr_max_channels_per_path_tcp | The maximum number of SIW IO channels per network path. <br>Default is 16. <br>For NVLustre production clusters, set to 4. |
| nr_max_wrs_per_req | The maximum number of WRs (RDMA work requests) per IO channel request, used in response to a read request. <br>For 0 (default), use system’s default. |
| nr_post_recv_on_send_comp | In per-cpu CQ and SRQ mode, post a receive buffer on send completion of an IO response. <br>Default is false. |
| nr_skip_disk_access | Unsafe debug mode. <br>Skip local disk access, i.e., complete disk operations immediately instead of performing them for remote IO operations. Used for debugging and performance optimization. <br>Default is false. |
| nr_skip_rdma_write_back | Unsafe debug mode. <br>Skip performing the RDMA write-back usually done for disk read operations for remote IO operations. Used for debugging and performance optimization. <br>Default is false. |
| nr_wq_set_cpu_affinity | Set CPU affinity of IO communication work queues, based on channel index. <br>Default is false. |
| num_warnings | Returns the number of warnings the module has triggered. <br>Contact support if not 0. |
| nvme_doorbell_batch | Determines whether to batch NVMe doorbell requests. <br>Default is true. |
| nvme_wq_unbound | Determines whether to use an unbound kernel workqueue for nvmeibs_nvme (true) or a bound one (false). <br>Default is false. |
| nvmeibs_jrange_num_blocks | Total number of journal blocks in journal range, typically allocated to a single client. <br>Should be set to a power of 2, between 64 and 16384. <br>Default is 512. |
| nvmeibs_nordda_io_req_num | Number of IO requests per IO channel. More can increase throughput, but may hurt caching. Less reduces memory consumption. <br>Default is 96. <br>For NVLustre production clusters, set to 8, as there are many channels. |
| nvme_number_offset | Offset for /dev/nvme%d device names. <br>Default is 1000. |
| pcpu_cq_poll_proc | Create /proc files for polling the nvmeibs shared completion queues from SPDK. <br>This requires pcpu_cq_all_cpus=Y for nvmeib_common. <br>Default is false. |
| ports | Used for port filtering functionality. This is typically set by service startup based on nvmesh.conf information. |
| qid_hint | Send data on a channel per the CPU id, mainly relevant for SIW. <br>Default is false |
| roce_ipv4_only | Deprecated. <br>Use IPv4 only for RoCE, which was needed for CX-3. <br>Default is false. |
| roce_port_prio | Defines the priority of ROCE, Infiniband is 0 by default and SIW is 20. Enables overriding the form of network transportation to prefer. <br>See ib_port_prio and tcp_port_prio also. <br>Default is 10, lower is preferred. |
| serjio_resched_work_wait_max | Amount of time in seconds to wait for a rescheduled SERJIO work item to run. SERJIO work items are related to garbage collection and cleaning up of journal entries. <br>Default is 10. |
| service_guid | Override cm_listen_id with this value. <br>Default is to use the system’s value. |
| shared_rq_size | Networking shared receive queue (SRQ) size. <br>Default is 32767. |
| stamp_free_jrnl_entries | Stamp free journal entries for debugging purposes:. <br>Default is true. |
| submit_wait_timeout | Timeout for NVMe admin operations such as drive formatting. Does not affect a second format attempt after a failure, as some drives take a long time to format, especially larger ones. <br>Value in milliseconds. <br>Default is 15000, i.e., 15 seconds. |
| tcp_mode | Activate the SIW communicate mode exclusively, i.e., filter out any RoCE devices. Usually set by service startup from nvmesh.conf information. <br>Default is false. <br>NVLustre at OCI sets this to true. |
| tcp_port_prio | Defines the priority of ROCE, Infiniband is 0 by default and ROCE is 10. Enables overriding the form of network transportation to prefer. <br>See ib_port_prio and roce_port_prio also. <br>Default is 20, lower is preferred. |
| tracer_debug_level | This determines the level of tracing for this module. Only traces with this level or lower will be issued, see tracer severities above. |
| use_nvme_kwq | Determines whether to use a kernel workqueue instead of a wakeup thread for processing completion queues. <br>Default is true. |
| use_pcpu_cq | Use a per-cpu shared completion queue (SCQ) and shared receive queue (SRQ). <br>Default is false. |
|  |  |

## siw

| Parameter | Description |
| --- | --- |
| ack_signal_wr | Request responder to ack signaled writes (bool). <br>Default is true. |
| comp_vector_cpu0 | Defines the first CPU0 to use for completion vector allocation (int). <br>Default is 0. |
| connect_non_block | Perform non-block TCP connects (bool). <br>Default is true. |
| cq_notify_tasklet | Use tasklet (instead of WQ) for CQ notify (bool). <br>Default is true. |
| debug_level | Enables debug logging (to the system log not NVMesh tracer) if set above 1, which is the default value. Deprecated. |
| iface_list | Interface list SIW attaches to if present (array of characters). <br>Default is “”. |
| loopback_enabled | Enable loopback (bool). <br>Default is true. |
| low_delay_tx | Run tight transmit thread loop if activated (bool). <br>Default is true. |
| low_delay_tx_cpu_set | bitmap of tx-cpus thread in tight loop (ulong). <br>Default is all CPUs. |
| mpa_crc_required | MPA CRC required (bool). <br>Default is false. |
| mpa_crc_strict | MPA CRC off enforced (bool). <br>Default is true. |
| notify_on_wq | Notify CQ on Workqueue (bool). <br>Default is true. |
| panic_on_rx_err | Panic on RX Error (bool). <br>Default is false. |
| panic_remote_on_rx_err | Panic remote on RX Error (bool). <br>Default is false. |
| sock_buff_sz | Socket buffers size in bytes. <br>Default is 64k. |
| tcp_nodelay | Set TCP NODELAY (bool). <br>Default is true. |
| tcp_quickack | Set TCP QUICKACK (bool). <br>Default is true. |
| tx_cpu_list | List of CPUs siw TX thread shall be bound to (format: comma separated no spaces) (string). <br>Default is none (empty string). |
| tx_flags_from_upstream | Determines whether to take Tx flags from upstream version (bool). <br>Default is false. |
| tx_flags_use_eor | Determines whether to take Tx flags from upstream for use EOR (bool). <br>Default is false. |
| tx_thread_high_prio_bmp | A bitmap of CPU Tx Threads to set to high priority. <br>Default is 0. |
| use_pbe_fixed_size | Use fixed size buffers. <br>Default is false. <br>For NVLustre production clusters, set to true. |
| use_so_incoming_cpu | Set the RX CPU of socket to RCQ’s comp-vector index (after connect/ accept). <br>Default is true. |
| wait_rqe_delay_ms | Delay to wait on empty S(RQ) (in ms) (int). <br>Default is 10. |
| wait_rqe_max_retries | Number of retries on empty S(RQ) (int). <br>Default is 10. |
| zcopy_tx | Zero copy user data transmit if possible (bool). <br>Default is true. |
| zero_delay_tx | Run tight transmit thread loop always (bool). <br>Default is true. |
|  |  |
