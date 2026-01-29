<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# Non Disruptive Upgrade (NDU)

NDU is a critical process, which allows us to update the product, without interfering the customer activities.

The process is managed by the central manager via the local upgrade agent. This document describes the process from the block point of view.

## Constraints

1. From the user point of view, the block device should stay "fully" operational.
2. The maximal I/O latency increase is 1.5 seconds.
3. The upgrade process happens between two consecutive versions.

### Implementation and/or derived constraints

1. The nvmeiba module may have [1..N] version.
2. system.d services do not take "one time" argument.
3. During the upgrade process communication with the central management is forbidden - the request/response latency is not limited.
4. The upgrade process is one way ticket - we don't have rollback - only roll forward.

## Step 1

Install new packages.

## Step 2

Execute `/usr/bin/nvmesh_client_upgrade` script

> **NOTE:** during the script execution, we have "mixed versions" environment. On disk - new version, in the memory - the old one.

`nvmesh_client_upgrade` script is simple and contains few steps:

+ link between current kernel and OFED versions and installed modules
+ turn on "upgrade" flag for system.d scripts
+ execute system.d nvmeshclient service restart

All those steps are implemented in a single place - `nvmeshclient` service control script

### "upgrade" flag

The problem: `system.d` does not allow to pass one time arguments to the service control script. So, `nvmesh_client_upgrade` script creates  ```/var/run/nvmesh/nvmeshclient/upgrade``` file. The file will be deleted in two cases:

+ the upgrade process finished successfully
+ the node was rebooted
  
There are multiple reasons behind this decision:

1. The upgrade process is one way ticket - once `nvmeiba` module got the volume for preservation, we need to complete the upgrade process successfully. We cannot instruct `nvmeiba` to reject all I/O and close the block device. Thus, if the "upgrade" process fails, the flag remains and all subsequent actions/attempts will do upgrade.
2. `system.d` implements "restart" verb as "service stop" & "service start". Knowing in advance that we are stopping the service for "upgrade" reason allows us to execute additional "must-have" actions, before entering critical path. The idea (not implemented) is:

   + prepare the environment for the next start (files(configuration), directories & the module arguments)
   + stop the service
   + start the service - actually, ideally only load the new modules with their arguments

### system.d nvmeshclient service restart

```bash
$ service nvmeshclient restart
```

The client service is restarted. As mentioned above systemd will issue client service stop and start commands. The CM(a local configuration management agent/service) and TD(trace daemon) are dependencies for the `nvmeshclient` service. So the `system.d` restarts them after the client was stopped. The CM & TD services are independent of each other, thus they are restarted in parallel.

## Timeline

1. Client service stop
2. CM and TD parallel restart
3. Client service start

## Client stop special flow for upgrade

1. The init-script (```/opt/nvmesh/client-repo/services/nvmeshclient``` code discovers the upgrade marker file
2. Special IOCTL directive is injected to the `nvmeibc` module (via ```/opt/nvmesh/bin/nvmesh_clnt_shutdown --upgrade```)
3. The interface to the OS is not removed, the block device (```struct gendisk```) instance is kept at the atom module (```orphan_abandon``` flow)
4. Existing IOs are drained (there's waiting for that), new IOs are held by the atom
5. A keeper module is loaded, the core layer hands HCA MRs (Memory Region handles) to the keeper
6. The client disconnects from all disks, etc
7. All modules other than the atom are unloaded

## Comments on CM/TD restart

1. CM/TD Stop flow - the flow involves sending signal to a daemon and waiting for the process to exit
2. CM start - special care is taken to first issue communication with the client (inject the cache content) and only later with Kafka
3. for details regarding CM/TD stop after client stop - see "PartOf" in ```/lib/systemd/system/nvmeshcm.service```
4. for details regarding CM/TD start before client start - see "Wants" in ```/lib/systemd/system/nvmeshclient.service```

## Client start special flow for upgrade

1. The init-script loads the modules and deletes the upgrade marker
2. Instead of allocating new OS API instance, the client gets one from the atom (```orphan_adopt``` flow)
3. The atom injects pending IOs to the OS API instance
4. The core layer gets HCA MRs from the keeper, the keeper module is unloaded
5. The CM senses that the client is up and sends attach and target-nics configuation packets to the client from their cache.
   This is not special to NDU, but mentioned here to stress that management/Kafka are not involved
6. The client continues to connect to the disks (CONT) and do TOMA registration for the segments

## Troubleshooting

The logical errors could be found by adding `set -x` directive to the `nvmeshclient` control script.
`journalctl -o short-precise --since "MM DD hh:mm:ss` command can be used to see all the messages from the script

The performance analysis should be done with `nvmesh_client_upgrade_breakdown.py`.
It is possible to use the script for online (on the node) and offline (only logs are available) modes.
