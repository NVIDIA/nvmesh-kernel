# NVMesh Compatibility Matrix

Derived from git commit history (January 2024 – March 2026).

---

## Supported Kernel Versions

### Generic / Upstream Kernels

| Kernel Version | Notes |
|----------------|-------|
| 6.17.0-1008-nvidia | NVIDIA kernel; added in NVMESH-7899 |
| 6.15.x | rdma_cm / iw_cm / iwpm compat (NVMESH-6447, NVMESH-7377) |
| 6.14.x | rdma_cm / iw_cm compat; block layer, strlcpy, memcpy, vzalloc, objtool, trace.mk, partno fixes (NVMESH-5634, NVMESH-6447, NVMESH-7377) |
| 6.13.x | rdma_cm / iw_cm compat (NVMESH-6447, NVMESH-7377) |
| 6.12.x | rdma_cm / iw_cm compat (NVMESH-6447, NVMESH-7377) |
| 6.11.x | rdma_cm / iw_cm compat (NVMESH-6447, NVMESH-7377) |
| 6.11.0-1016-nvidia-64k | NVIDIA ARM 64k page-size kernel |
| 6.10.x | rdma_cm compat (NVMESH-6447, NVMESH-7377) |
| 6.9.x | rdma_cm compat (NVMESH-6447, NVMESH-7377) |
| 6.8.x | rdma_cm compat; dma_alloc_pool, vm_flags, strlcpy, timer_list, SIW tcp_sendpage, MSG_SPLICE_PAGES, UBSAN, KS_GET_USER_PAGES_REMOTE_HAS_VMAS (NVMESH-3015, NVMESH-6447, NVMESH-7377) |
| 6.8.0-71-generic | Ubuntu 24.04 (NVCF build) |
| 6.5.x | Block API changes; ARM virt_addr_valid fix |
| 5.19.0-50-generic | Ubuntu |
| 5.19.0-45-generic | Ubuntu |
| 5.19.9-1.el8.x86_64 | RHEL 8 |
| 5.15.x | KS_HAS_SET_FS / KERNEL_DS fix for siw_cm.c |
| 5.15.0-164 | Ubuntu 22.04 (NVCF build) |
| 5.15.0-113-generic | Ubuntu 22.04; pcpu dma pool bug workaround (NVMESH-6264) |
| 4.18.0-553.22.1.el8_10.x86_64 | RHEL/Rocky 8.10 |
| 4.18.0-553.16.1.el8_10.x86_64 | RHEL/Rocky 8.10 |
| 4.18.0-553 | Rocky 8.10 |
| 4.18.0-513.24.1.el8_9.x86_64 | RHEL/Rocky 8.9 |
| 4.18.0-477.13.1.el8_8.x86_64+debug | RHEL/Rocky 8.8 (debug kernel) |
| 4.18.0-477.10.1.el8_8.x86_64 | Rocky 8 |
| 4.18.0-425.19.2.el8_lustre | RHEL/CentOS el8 lustre variant |
| 4.18.0-425.3.1.el8.x86_64 | RHEL 8 |

### Cloud-Specific Kernels

| Kernel / Cloud | Notes |
|----------------|-------|
| 6.17.0-1008-nvidia (NVIDIA Cloud) | NVMESH-7899 |
| 6.11.0-1016-nvidia-64k (NVIDIA ARM) | 64k page size |
| 6.8.0-71-generic (NVIDIA NVCF / Ubuntu 24.04) | NVIDIA Cloud Functions |
| 6.5.0-1024-aws (AWS) | NVMESH-5621 |
| 5.15.0-1067-gke (GCP GKE) | GKE Docker support |
| 5.15.0-164 (NVIDIA NVCF / Ubuntu 22.04) | NVIDIA Cloud Functions |
| Azure kernel | Build fix |
| OCI kernel | NVMESH-3340 (nvmesh_oci.conf); NVMESH-5182 (/proc/interrupts caching fix) |

---

## Supported OFED / MLNX_OFED Versions

| OFED                       | DOCA-OFED       | Notes                                            |
|----------------------------|-----------------|--------------------------------------------------|
| OFED-internal-26.01-1.0.0 | DOCA-OFED 3.3.0 |                                                  |
| OFED 25.10-1.7.1           | DOCA-OFED 3.2.1 | iw_cm + iwpm patching (NVMESH-6447, NVMESH-7377) |
| OFED 25.04                 | DOCA-OFED 3.0.0 | Fix missing IB_MLX5 define                       |
| OFED 24.10                 | —               | SIW compat                                       |

---

## Supported Distributions

| Distribution | Kernel | Notes |
|--------------|--------|-------|
| Ubuntu 24.04 | 6.8.0-71-generic | Includes ARM/GB200 variant |
| Ubuntu 22.04 | 5.15.0-164, 5.15.0-113, 6.8.x | Clang 14 build fixes (NVMESH-3537) |
| Rocky Linux 9.6 | — | NVMESH-7914 |
| Rocky Linux 8.10 | 4.18.0-553 | |
| Rocky Linux 8.8 | 4.18.0-477.10.1 | |
| RHEL 8.10 | 4.18.0-553.x | |
| RHEL 8.9 | 4.18.0-513.x | |
| RHEL 8.8 | 4.18.0-477.x | |
| RHEL 8 | 4.18.0-425.x | |
| CentOS el8 (lustre) | 4.18.0-425.19.2.el8_lustre | inbox driver |

---

## Compiler Support

| Compiler | Notes |
|----------|-------|
| Clang 19 | Added support |
| Clang 14 | Ubuntu 22.04 build fixes (NVMESH-3537) |
| GCC 13.3.0 | Production compilation fix |
| GCC 13.2 | Ubuntu 24.04 UBSAN flexible array member fix (NVMESH-4367) |
| GCC 13.1 | Dangling pointer, enum mismatch fixes |
| GCC 12 | ARM_EC fix |
| GCC 9 | New warning suppression |
| GCC 8.5.0 | RHEL 8.10 |
