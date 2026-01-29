<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Preamble
Following are basic instructions for building NVMesh.
Send mail to support-nvmesh@nvidia.com for help.

**Note:** the NVMesh team validates NVMesh for specific operating systems, Linux kernels and OFED versions. It is not recommended to use self-built NVMesh for production use cases without taking one of the following steps:

 1. Obtaining sign-off from the NVMesh team.
 2. Running the version through the NVMesh team's CI process for regression testing.

# Overview
Compilation or building is performed by running the `build.sh` script at the root of the source tree.
Typically, it will sync sources and then perform a build on one or more remote servers.
`build_sh_conf` is used to configure the specifics of the process. You should not need to change the `build.sh` script itself.

# Prerequisites

Several packages are required for the build and only some are automatically installed as part of the build process.
The specifics will vary between operating systems and depending on whether OFED is used.

The following example is for RockyLinux 8.5 (TBD: be more exhaustive).

> [root@nvmesh3]# cat /etc/yum.repos.d/baseos-vault.repo
> [baseos-vault]
> baseurl=https://dl.rockylinux.org/vault/rocky/8.5/BaseOS/x86_64/os/
> gpgcheck=1
> enabled=0
> 
> [root@nvmesh3 2.5.2]# dnf install --enablerepo=baseos-vault kernel-devel-\$(uname -r) kernel-headers-\$(uname -r) kernel-modules-extra-$(uname -r) git elfutils-libelf-devel make tk tcsh tcl gcc-gfortran autoconf bc rdma-core-devel systemd-devel rpmdevtools

# build_sh_conf
An example build_sh_conf that covers the primary options follows:
> EXCLUDES=excludes
> REMOTE_SERVERS="MY_SERVER_DNS:IM_BOTH=yes:MK_RPM=yes "
> REMOTE_SERVERS+="MY_2nd_SERVER_IP:IM_BOTH=yes:MK_RPM=yes "
> BRANCH=`git symbolic-ref --short HEAD`
> REMOTE_DIR=projects/$BRANCH
> PARALLEL=true
> MAKE_OPTS=" -j clean TCM=TCMR"

For build_sh_conf instructions, see the head of `build.sh`, provided here for convenience.
## Config file format
### Per-server options
REMOTE_SERVERS="[<hostname>:[<option>=<value>:...] ...]"
Supported options:
`IM_SERVER=yes`
`IM_CLIENT=yes`
`IM_BOTH=yes`
`MK_RPM=yes`
`COMPRESS_KO=yes`
`RPM_BUILD_TYPE=kmod`

Example:
`REMOTE_SERVERS="nvme4:IM_SERVER=yes nvme5:IM_BOTH=yes:MK_RPM=yes"`

### Global options
SSH_INVOKE="<ssh-cmd-line>"

Example: `SSH_INVOKE="sshpass -e ssh -l root"`
Use sshpass to login as user "root" with password in ${SSHPASS}.

Example: `SSH_INVOKE="sshpass -p Pa$$w0rd ssh -l root"`
The same as above, but with the password supplied in the command line.

PRIV_SSH="\<srv>:\<ssh-cmd-line>|\<srv>:\<ssh-cmd-line>..."
Override global SSH_INVOKE per server.
Example: `PRIV_SSH="nvme4:ssh|nvme5:sshpass -e ssh -l root"`

REMOTE_DIR=<remote_dir_name>
Example: `REMOTE_DIR=projects/${BRANCH_NAME}`

MAKE_OPTS=<list_of_make_args>, to be passed to remote make
Example: `MAKE_OPTS="-j clean"`
`-j` is often used to speed up compilation.

RSYNC_OPTS=<list_of_rsync_args>, to be added to default ones
Example: `RSYNC_OPTS="--compress"`

EXCLUDES=<file-name-with-rsync-exclude-patterns>
Example: `EXCLUDES=excludes`

PARALLEL=(true|false), default=false
Defines whether to build in parallel on the remote servers.
Example: `PARALLEL=true`

FILTER_OUT=(true|false), default=false
Can be used to reduce console output.
Example: `FILTER_OUT=true`

DRY_RUN=(true|false), default=false
Example: `DRY_RUN=true`

REPO_DIR=<local-build-repo-dir>, used for tagged servers only
Example: `REPO_DIR="~/build_repo"`

REPO_RPMS=(true|false), default=false, copy RPMs to local repo
Example: `REPO_RPMS=true`

REPO_SYMS=(true|false), default=false, copy symbols to local repo
Example: `REPO_SYMS=true`

REPO_MODS=(true|false), default=false, copy modules to local repo
Example: `REPO_MODS=true`

# Example Output
On the console you should see the details of the compilation running on the remote machines.
Following is example output from a successful build:
> Build complete at Tue Jun 14 15:38:12 IDT 2022 - Exiting...
> \-----------------------------------------------------------------
> 1: 10.0.11.62 err:0 log_file: /tmp/10.0.11.62.build
> \-----------------------------------------------------------------

On the remote machine(s),  the RPMs or DEBs generated will be in your `<home>/projects/<branch name>` directory per the `REMOTE_DIR` definition in `build_sh_conf.` The directory will contain both the source code and the output packages, e.g., `nvmesh-core-2.5.2-21.el7_9.x86_64.rpm`.

To install NVMesh, you will also need nvmesh-utils & nvmesh-management. They are not kernel dependent, so usually you can obtain them from NVMesh repo. Alternatively, they can be built from the `nvmesh-management` source, which is kept in a different git project.

If your build fails, contact support-nvmesh@nvidia.com.
