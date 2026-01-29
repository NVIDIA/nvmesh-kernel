<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# NVMesh by NVIDIA

## Description
NVMesh by NVIDIA provides remote shared storage facilities with in-server flash performance characteristics while using commodity off-the-shelf components.

As NVMesh is a software only solution, it has the flexibility to provide storage in a hyper-converged architecture or as a top-of-rack flash appliance or as part of a dedicated storage rack.

NVMesh comprises three software elements:
* management server.
* storage client
* storage target (server)

### Management Server
The management server is used for providing storage definitions and monitoring the health and performance of the system.

### Storage Client
The storage client software implements block device functionality for storage consumers.

### Storage Target
The storage target software identifies storage hardware and sets up network pathways to the storage elements on behalf of the storage clients.
TOMA is part of the storage target

## Building
Use `build.sh` to build NVMesh. See `build.sh --help` output for details.

