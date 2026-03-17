# NVMesh 3.4.0 Module Params Guide

<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

## Table of Contents

- [NVMesh 3.4.0 Module Params Guide](#nvmesh-340-module-params-guide)
  - [Table of Contents](#table-of-contents)
- [Copyright and Trademark Information](#copyright-and-trademark-information)
- [Preface](#preface)
- [Acronyms and Terms](#acronyms-and-terms)
- [Module Parameters](#module-parameters)
  - [Tracer Severities](#tracer-severities)
  - [nvmeiba](#nvmeiba)
  - [nvmeibc](#nvmeibc)
  - [nvmeib\_common](#nvmeib_common)
  - [nvmeib\_common\_public](#nvmeib_common_public)
  - [nvmeibs](#nvmeibs)
  - [siw](#siw)

# Copyright and Trademark Information

© 2026 NVIDIA All rights reserved.

Specifications are subject to change without notice.

NVMesh® is a registered trademark of NVIDIA.

All other brands or products are trademarks or registered trademarks of their respective holders and should be treated as such.

# Preface

**<u>Audience</u>**

The primary audience for this document is intended to be storage and/or application administration personnel responsible for installing and deploying NVMesh.

**<u>Feedback</u>**

We continually try to improve the quality and usefulness of documentation. If you have any corrections, feedback, or requests for additional documentation, send an e-mail message to <nvmesh-documentation@nvidia.com>.

# Acronyms and Terms

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
|  |  |

## nvmeibc

| Parameter | Description |
| --- | --- |
|  |  |

## nvmeib_common

| Parameter | Description |
| --- | --- |
|  |  |

## nvmeib_common_public

| Parameter | Description |
| --- | --- |
|  |  |

## nvmeibs

| Parameter | Description |
| --- | --- |
|  |  |

## siw

| Parameter | Description |
| --- | --- |
|  |  |
