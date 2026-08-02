# Security Policy: NVMesh

NVIDIA is dedicated to the security and trust of our software products and
services, including all source code repositories managed through our
organization.

## Reporting a Vulnerability

If you discover a potential security vulnerability, please **do not open a public
issue** and **do not report security vulnerabilities through public pull
requests**.

Please use one of the following NVIDIA reporting channels:

* **NVIDIA Vulnerability Disclosure Program (preferred):**
  [NVIDIA Product Security portal](https://www.nvidia.com/en-us/security/)
* **E-Mail:** [psirt@nvidia.com](mailto:psirt@nvidia.com)
  - We encourage you to use the following PGP key for secure email
    communication: [NVIDIA public PGP Key](https://www.nvidia.com/en-us/security/pgp-key)
* **GitHub:** On the public NVMesh mirrors, use the **Security** tab >
  **Report a vulnerability** to submit a report privately on the repository.

Please include the following information:

- Product/project name and version/branch that contains the vulnerability
- Type of vulnerability (code execution, denial of service, buffer overflow,
  privilege escalation, information disclosure, etc.)
- Step-by-step reproduction instructions
- Proof-of-concept or exploit code (if available)
- Impact assessment, including how an attacker could exploit the issue

**Detailed reports help NVIDIA evaluate and address issues faster.**

NVIDIA's PSIRT team will acknowledge receipt, validate severity, develop fixes,
and publish security bulletins as appropriate. While NVIDIA currently does not
have a bug bounty program, we do offer acknowledgement when an externally
reported security issue is addressed under our coordinated vulnerability
disclosure policy. See the
[PSIRT policies page](https://www.nvidia.com/en-us/security/psirt-policies/) for
more information.

## Security Architecture & Context

NVMesh is a software-defined distributed block storage system that provides
remote shared storage with local-flash performance characteristics over
standard RDMA fabrics (InfiniBand / RoCE). This repository contains the source
for NVMesh's three core software elements:

- **Storage client** — a Linux kernel block driver (`clnt/`, e.g.
  `clnt/nvmeibc_admin_channel.c`) that exposes remote volumes as local block
  devices to storage consumers.
- **Storage target** — a Linux kernel driver (`srv/`, e.g. `srv/nvmeibs_main.c`,
  `srv/nvmeibs_nvme.c`) plus the user-space **TOMA** orchestrator (`toma/`) that
  claims local NVMe hardware and serves it to remote clients.
- **Management agent / control plane** — Python services on each host
  (`management_cm/`, e.g. `managementCM.py`, `managementAgent.py`,
  `CMSocket.py`) that distribute storage definitions and monitor health.

The primary data path is RDMA-based messaging (`common/nvmeib_rdma.c`,
`toma/interfaces/network/`, and a bundled soft-iWARP provider under
`softiwarp/`). The control path uses Kafka (`toma/nvmeibt_kafka.c`,
`management_cm/managementAgent.py`) and local Unix-domain / file sockets
(`management_cm/CMSocket.py`). The repository also vendors third-party kernel
and fabric sources (`kernels/`, `ofeds/`, `scripts/target/nvme-cli/`,
`tools/lz4/`).

This software operates primarily at the **Driver** level (kernel-mode client and
target block drivers), supported by user-space **Service** and **CLI/tooling**
components. Its primary security responsibility is to preserve the
**integrity, confidentiality, and availability of block-storage data** as it
moves between clients and targets, and to safely mediate between untrusted
network peers and privileged kernel and NVMe hardware resources.

**Repository Exposure Classification:** Public.
Basis: the project is distributed under a permissive license (Apache-2.0, with
kernel components dual-licensed GPL-2.0-only OR Apache-2.0) and the README
enumerates public open-source mirrors of the NVMesh components; this SECURITY.md
is written to public-safe detail.

**Service Exposure Classification:** External / Regulated (high confidence).
Basis: externally distributed as open source, enterprise-supported storage 
product packaged as RPMs/DEBs; it handles customer block-storage data 
on the storage data path and integrates deeply with the host kernel and 
NVMe hardware.

**Trust boundaries.** The storage fabric between clients and targets, the Kafka
control bus, and the host kernel/hardware boundary are the principal trust
boundaries. Kernel drivers accept input from local userspace (ioctl / sysfs /
netlink) and from remote peers on the storage network; the target trusts the
management control plane to describe volume topology and attachments.

### Threat Model

The following scenarios represent the primary security concerns for this project,
including auxiliary/support code. They are derived from the actual interfaces in
this repository.

1. **Local privilege escalation via kernel driver entry points:** The client and
   target kernel modules expose ioctl, sysfs, and netlink surfaces
   (`common/nvmeib.c`, `srv/nvmeibs_nvme.c`, `srv/nvmeibs_client.c`,
   `srv/nvmeibs_um_comm.c`, `softiwarp/kernel/siw_verbs.c`). A local process that
   can reach these interfaces could supply malformed arguments to trigger memory
   corruption or invalid hardware operations in ring-0, leading to privilege
   escalation or a kernel crash.

2. **Malicious or spoofed RDMA peer on the storage fabric:** The data path
   accepts connections and messages from remote peers over RDMA/RoCE
   (`common/nvmeib_rdma.c`, `toma/interfaces/network/nvmeibt_nm_ibud.c`,
   `clnt/nvmeibc_admin_channel.c`, and the vendored `softiwarp/`). A host that
   can join the storage network and send crafted connection parameters or wire
   messages could read or corrupt volume data, exhaust target resources, or
   corrupt target state if messages are trusted without sufficient per-peer
   authentication.

3. **Memory-safety flaws in the user-space TOMA parsers:** TOMA parses
   management configuration and Kafka payloads in C using hand-rolled JSON code
   over raw `malloc`/`realloc`/`memcpy` (`toma/nvmeibt_json_base.c`,
   `toma/nvmeibt_mm_json.c`, `toma/nvmeibt_kafka.c`). A malformed or oversized
   configuration/topology message could cause a buffer overflow or
   out-of-bounds access in the privileged target orchestrator.

4. **Control-plane message injection via Kafka / management sockets:** The
   management agent and TOMA consume attach/detach/topology and keepalive
   messages over Kafka and local sockets (`management_cm/managementAgent.py`,
   `management_cm/CMSocket.py`, `toma/nvmeibt_kafka.c`). Message framing is a
   length prefix plus JSON (`struct.unpack("I", ...)` and `json.loads` in
   `CMSocket.py`) validated only for schema shape and monotonic client tokens. A
   compromised broker or peer able to inject messages could redirect, attach, or
   detach volumes, exposing or disrupting client storage.

5. **Code execution through writable configuration files:** The management agent
   evaluates configuration files as Python via `exec(compile(open(...)))`
   (`readBashFile` / `include` in `management_cm/managementAgent.py` and
   `CMSocket.py`, reading paths such as `/etc/nvmesh/nvmesh.conf` and the client
   version file). If a lower-privileged user can write to these files, arbitrary
   code runs in the privileged agent process. Key and token material is loaded
   from an on-disk keys directory, so its file permissions are security-critical.

6. **Vendored third-party and fabric code carrying known vulnerabilities:** The
   repository bundles external kernel trees, OFED/InfiniBand core sources, an
   `nvme-cli` copy, and `lz4` (`kernels/`, `ofeds/`, `scripts/target/nvme-cli/`,
   `tools/lz4/`). Vulnerabilities discovered upstream in these components ship
   with NVMesh until the vendored copies are refreshed, creating a supply-chain
   and stale-CVE risk.

7. **Privileged diagnostic and build tooling abuse:** Numerous helper scripts and
   tools run privileged operations, spawn subprocesses, or issue device ioctls
   (`tools/`, `management_cm` `subprocess.Popen` calls, `tools/miniscrub/`,
   `tools/toma_rpc/`). If invoked with attacker-influenced arguments or on shared
   hosts, these auxiliary paths can be leveraged for local privilege escalation
   or data destruction.

### Critical Security Assumptions

- **Trusted storage fabric.** The RDMA/RoCE storage network is assumed to be a
  physically and logically isolated, trusted segment; NVMesh does not assume the
  data path is exposed to arbitrary internet peers and relies on network-level
  segmentation for peer trust.
- **Trusted control plane.** The target and clients assume the Kafka control bus
  and management services are operated by trusted administrators; message
  authenticity beyond schema and token checks is delegated to the transport and
  deployment environment.
- **Privileged, correctly configured host.** Kernel modules assume the host OS,
  MMU, and NVMe hardware behave correctly and that only privileged, trusted local
  processes can reach ioctl/sysfs/netlink and device nodes.
- **Protected on-disk configuration and secrets.** The agent assumes
  configuration files it evaluates as code and the key/token material under the
  NVMesh configuration directory are writable only by root/trusted
  administrators.
- **Callers validate upstream input.** Kernel and TOMA entry points assume the
  immediate caller (management plane, orchestration layer) has already validated
  volume/topology definitions; they do not perform exhaustive re-validation of
  every field.
- **Timely vendored-dependency updates.** Security of the bundled kernel, OFED,
  `nvme-cli`, and `lz4` sources assumes these copies are kept current with
  upstream security fixes as part of the release process.
