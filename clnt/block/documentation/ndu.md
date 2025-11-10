# Non Disruptive Upgrade (NDU)

NDU is a critical process, which allows us to update the product, without interfering the customer activities.

The process is managed by the central manager via the local upgrade agent. This document describes the process from the block point of view.

## Constraints

1. The interface to the OS (block device remains in place).
2. The maximal I/O latency increase is 1.5 seconds.
3. The upgrade process happens between two consecutive versions.

### Implementation and/or derived constraints

1. The nvmeiba module may have [1..N] version.
2. system.d services do not take "on time" argument.
3. During the upgrade process communication with the central management is forbidden - the request/response latency is not limited.
4. The upgrade process is one way ticket - we don't have rollback - only roll forward.

## Step 1

Install new packages.

## Step 2

Update '/lib/modules' directory to contain links to new modules and uncompress the modules. The following command is used:

```bash
$ /opt/nvmesh/client-repo/services/nvmeshclient status
```

> **NOTE:** since we already installed new version, the new control script will be used.

## Step 3

Pass "upgrade" flag to `system.d` `nvmeshclient` control script.

Actually `system.d` does not allow to pass one time arguments to the service control script. Thus the local upgrade agent is instructed to create the following file ```/var/run/nvmesh/nvmeshclient/upgrade```. The file will be deleted in two cases:

+ the upgrade process finished successfully
+ the node was rebooted
  
There are multiple reasons behind this decision:

1. The upgrade process is one way ticket - once `nvmeiba` module got the volume for preservation, we need to complete the upgrade process successfully. We cannot instruct `nvmeiba` to reject all I/O and close the block device. Thus, if the "upgrade" process fails, the flag remains and all subsequent actions/attempts will do upgrade.
2. `system.d` implements "restart" verb as "service stop" & "service start". Knowing in advance that we are stopping the service for "upgrade" reason allows us to execute additional "must-have" actions, before entering critical path. The idea (not implemented) is:
   
   + prepare the environment for the next start (files(configuration), directories & the module arguments)
   + stop the service
   + start the service - actually, ideally only load the new modules with their arguments

## Step 4

```bash
$ service nvmeshclient restart
```

The client service is restarted. As mentioned above systemd will issue client service stop and then client service start. The client service script
sets the connection-manager (CM) and trace-daemon (TD) services as dependencies such that they are stopped after the client stops and are started 
before the client starts. The dependent services don't have ordering between themselves, hence the resulted flow is that they are restarted in parallel.


## Timeline

1. Client service stop
2. CM and TD parallel restart
3. Client service start


## Client stop special flow for upgrade
1. The init-script (```/opt/nvmesh/client-repo/services/nvmeshclient``` code discovers the upgrade marker file
2. Special IOCTL directive is injected to the atom module (via ```/opt/nvmesh/bin/nvmesh_clnt_shutdown --upgrade```)
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
