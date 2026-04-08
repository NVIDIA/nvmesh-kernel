<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->


# Block Feature Matrix

## Preamble

The goal of this document is to be used as a **template**. The template should be used for

+ Kernel client versus UM feature parity
+ A new feature:
  + integration points overview
  + test matrix update
  + must have features (observability)

Ideally, the features matrix should be a mix of components & processes on pretty high level, but not too high. A golden mean is needed.

## OS Integration

+ system.d
+ udev
+ non disruptive upgrade
+ /proc file system
+ /sys/modules/nvmesh-x/parameters
+ packaging - *deb/rpm/dockerfile*

## Control path

+ Client
  + verbs
    + construct - *creates and store client uuid*
    + destroy - *destroys the client uuid and any persistent information*
    + update client token - *used to restart communication protocol*
    + keep alive
    + alerts - *send message to the user via the management*
    + nod disruptive upgrade
    + on kernel version changed - *same NVMesh software, different kernel*
    + configure
      + via params/ioctl/merged configuration files - *multiple ways to configure the product*
    + diagnostics collection
    + build & run time environment - *the huge kernel & compiler versions zoo we maintain*
    + create/destroy client instance - ***hidden product***
    + integration with local TOMA/server (used by recoveries and I/O optimization)
    + control plane - *mcs/cli*
+ Volume
  + verbs
    + attach
      + purpose(user/recovery/encryption)
        + migrate/combine - ***dead product**, a state machine, which allows to migrate/combine different attachments of the same volume*
      + reservation mode - *recovery only, read-only, read&write, exclusive*
      + preemption
      + upgrade - *used during EC journal expand*
      + expose - *make the block device*
    + detach
      + gentle - *try detach*
      + force - *leak & en-expose eventually*
      + upgrade - *used during EC journal expand*
      + OS API integration
        + un-expose - *delete block device*
    + update
      + extend - *add chunk*
        + passive - *the user extend the volume*
        + active - *I/O forces the volume to request more disk space* - **hidden product**?
      + replace segment
      + attachment references - *manages multiple volume attachments on a single node*
    + add/delete CPUs mask
    + report status
      + debouncing algorithm
    + request client configuration - **deprecated**
    + configure
      + params
      + ioctl
      + SCSI ioctl
      + mcs
      + cli
    + iostats
    + recover
      + verbs
        + start
        + stop
        + status
        + control
          + dimensions:
            + TOMA - *speed & parallelism*
            + client - *batching & parallelism*
        + recovery chain - *Example: topology transition from D->W- requires 2 passes: turn-on dirty bits + fix dirty bits - done solely by the client*
      + dimensions
        + hot - *I/O-enabled*
        + cold - *I/O disabled*
  + block device flows
    + read partition - *client triggers this process after the volume becomes IOable*
    + disk rescan, revalidation - *on expand, notify the OS, so the file system will be aware of new free space* 
  + dimensions
    + RAID geometry
    + block size
      + 512B - ***hidden product** unsupported and broken*
    + block md size
    + lock scheme
    + debug di
+ Targets
  + verbs
    + update
    + get target NICs

## I/O control path

+ TOMA/topologies
  + consensus algorithm (lock id and segments access modes)
  + reservation mode
  + stale locks
  + switch topology
  + segment replacement
    + dimensions
      + hot/warm/cold - *"hot & warm" are **hidden product**, not in use*
+ safety plugs - *read-only or no-I/O on too many errors*
+ resubmitter
  + CPU mask

## I/O path

+ Communication primitives
  + lock/disk/generic local/remote commands
    + user/recovery I/O statistics
+ Good path
  + CPUs mask
  + I/O throttling
  + multi-blockset R/W optimization
  + range trims
  + contention optimization
    + elevation algorithm - ***hidden product***
    + lock transfer optimization
    + operations chain - solve "multiple non-joinable I/O requests to the same blockset" problem
  + adjacent blocksets handling
  + memory optimizations:
    + for 4KB I/O
    + syncs
    + I/O buffers
  + read/write buffers are mutable
+ Bad path
  + syncs
  + disk pause/continue
  + lock complaining
+ erasure coding algorithms

## Observability

+ tracing
+ metrics
+ statistics
+ flow counters
+ profilers
+ events tags
+ eBPF

## Testing

+ simulator
+ CI/CD
+ KUnit

## Support utilities

+ block compare
+ the address translator
+ debug di data extractor
+ different NVCK utilities
