<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# NVMesh Server (srv/) Architecture Diagram

## High-Level Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        NVMesh Server Module (nvmeibs)                       │
└─────────────────────────────────────────────────────────────────────────────┘
```

## 1. Module Initialization Flow

```
nvmeibs_nvme.c::nvmeibspci_init()  (Module Entry Point)
            │
            ├─► Create /proc/nvmeibs directories
            ├─► nvmeibs_init() ◄─── nvmeibs_main.c
            │        │
            │        ├─► Initialize memory manager metrics
            │        ├─► Create main work queue (main_wq)
            │        ├─► Start usermode communication (nvmeibs_um_comm)
            │        ├─► Initialize client database (nvmeibs_cdb)
            │        ├─► Setup interrupt shaper
            │        └─► Initialize device filters
            │
            ├─► Register block device (local_major)
            ├─► Create IOQM work queue (ioqm_wq)
            ├─► Start nvmeibs kernel thread
            └─► PCI probe: nvmeibs_probe()
                     │
                     └─► nvmeibs_nvme_disk_scan_done()
                              │
                              ├─► Create proc files
                              ├─► Start MCS (Management Communication Service)
                              ├─► Start TOMA (management interface)
                              └─► Register IB client (deferred on main_wq)
                                       │
                                       └─► srv_start_ib_work_fn()
```

## 2. Core Component Architecture

```
┌────────────────────────────────────────────────────────────────────────────┐
│                              MAIN COMPONENTS                               │
├────────────────────────────────────────────────────────────────────────────┤
│                                                                            │
│  ┌──────────────┐         ┌─────────────┐         ┌──────────────┐         │
│  │ nvmeibs_main │◄───────►│ nvmeibs_ib  │◄───────►│ nvmeibs_net  │         │
│  │              │         │   _port     │         │              │         │
│  │ - Init/Exit  │         │ - Port Mgmt │         │ - QP/CM Mgmt │         │
│  │ - Device Mgmt│         │ - Listeners │         │ - RDMA Ops   │         │
│  └──────┬───────┘         └──────┬──────┘         └──────┬───────┘         │
│         │                        │                        │                │
│         │                        │                        │                │
│  ┌──────▼───────┐         ┌──────▼──────┐         ┌──────▼───────┐         │
│  │ nvmeibs_     │         │ nvmeibs_    │         │ nvmeibs_     │         │
│  │   client     │◄───────►│   disk      │◄───────►│   nvme       │         │
│  │              │         │             │         │              │         │
│  │ - Client DB  │         │ - Disk List │         │ - NVMe Drv   │         │
│  │ - Channels   │         │ - Resources │         │ - IO Submit  │         │
│  │ - Keep-Alive │         │ - Locks     │         │ - PCI Mgmt   │         │
│  └──────────────┘         └─────────────┘         └──────────────┘         │
│                                                                            │
└────────────────────────────────────────────────────────────────────────────┘
```

## 3. Client Management & Connection Handling

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         CLIENT CONNECTION FLOW                              │
└─────────────────────────────────────────────────────────────────────────────┘

Client Connection Request
         │
         ▼
┌─────────────────────┐
│  RDMA Listeners     │
│  (nvmeibs_main.c)   │
├─────────────────────┤
│ • IB Listener       │  (Service ID based)
│ • RoCE Listener     │  (Port 4791)
│ • iWARP Listeners   │  (Primary + Secondary ports)
│ • Loopback Listener │  (Local connections)
└─────────┬───────────┘
          │
          ▼
┌─────────────────────────────────────────────────┐
│  cm_req_recv() - New Connection Handler         │
│  (nvmeibs_main.c)                               │
└─────────┬───────────────────────────────────────┘
          │
          ▼
┌──────────────────────────────────────────────────┐
│  nvmeibs_ib_port::new_connection_work()          │
│  (nvmeibs_ib_port.c)                             │
│                                                  │
│  ┌────────────────────────────────────────┐      │
│  │ Validate login request                 │      │
│  │ Extract client UUID, CID, version      │      │
│  │ Determine channel type (opcode):       │      │
│  │   - NVMEIBC_ADMIN_CHANNEL              │      │
│  │   - NVMEIBC_IO_CHANNEL                 │      │
│  │   - NVMEIBC_LOCK_CHANNEL               │      │
│  │   - NVMEIBC_NORDDA_CHANNEL             │      │
│  │   - NVMEIBC_SECONDARY_LOCK_CH          │      │
│  └────────────────────────────────────────┘      │
└──────────┬───────────────────────────────────────┘
           │
           ▼
┌──────────────────────────────────────────────────┐
│         Channel-Specific Connection              │
├──────────────────────────────────────────────────┤
│                                                  │
│  ADMIN CHANNEL (First connection)                │
│  ┌────────────────────────────────────┐          │
│  │ nvmeibs_client_allocate()          │          │
│  │  - Allocate struct nvmeibs_client  │          │
│  │  - Assign CID                      │          │
│  │  - Add to client DB (nvmeibs_cdb)  │          │
│  │                                    │          │
│  │ nvmeibs_client_connect_admin_ch()  │          │
│  │  - Create nvmeibs_net (QP/CQ)      │          │
│  │  - Setup keep-alive                │          │
│  │  - Accept connection               │          │
│  └────────────────────────────────────┘          │
│                                                  │
│  IO CHANNEL (Data path)                          │
│  ┌────────────────────────────────────┐          │
│  │ nvmeibs_client_connect_io_channel()│          │
│  │  - Allocate IO resources           │          │
│  │  - Link to disk                    │          │
│  │  - Setup NORDDA channels           │          │
│  └────────────────────────────────────┘          │
│                                                  │
│  LOCK CHANNEL (Distributed locks)                │
│  ┌────────────────────────────────────┐          │
│  │ nvmeibs_client_connect_lock_ch()   │          │
│  │  - Setup lock resources            │          │
│  │  - Link to disk locks              │          │
│  └────────────────────────────────────┘          │
│                                                  │
│  NORDDA CHANNEL (Direct RDMA data path)          │
│  ┌────────────────────────────────────┐          │
│  │ nvmeibs_nordda_connect_channel()   │          │
│  │  - Setup NoRDDA resources          │          │
│  │  - Allocate IU pool                │          │
│  │  - Configure SRQ (if supported)    │          │
│  └────────────────────────────────────┘          │
│                                                  │
└──────────────────────────────────────────────────┘

Client Database Structure (nvmeibs_client_db.c)
┌──────────────────────────────────────────┐
│     struct nvmeibs_cdb                   │
├──────────────────────────────────────────┤
│ • Hash table (by CID)                    │
│ • List of all clients                    │
│ • List of dying clients                  │
│ • Lock (spinlock)                        │
│ • Count tracking                         │
│ • No-new-clients flag                    │
└──────────────────────────────────────────┘
```

## 4. Disk Management & Resources

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        DISK MANAGEMENT (nvmeibs_disk.c)                     │
└─────────────────────────────────────────────────────────────────────────────┘

NVMe Device Detection (via PCI)
         │
         ▼
┌──────────────────────────────────────┐
│  nvmeibs_probe()                     │
│  (nvmeibs_nvme.c)                    │
│                                      │
│  • Allocate nvmeibs_disk_info        │
│  • Get disk geometry (blocks, size)  │
│  • Read disk ID                      │
│  • Initialize disk locks             │
│  • Initialize SERJIO                 │
│  • Register with kernel              │
└──────────┬───────────────────────────┘
           │
           ▼
┌─────────────────────────────────────────────────────┐
│  struct nvmeibs_disk_info                           │
├─────────────────────────────────────────────────────┤
│ • Disk identification (disk_id, UUID)               │
│ • NVMe device reference                             │
│ • Block device                                      │
│ • Geometry (hw_blocks, sw_blocks, sector sizes)     │
│ • Metadata configuration                            │
│ • Client list (per-disk)                            │
│ • Lock resources (nvmeibs_disk_locks)               │
│ • SERJIO private data                               │
│ • IO queue management                               │
│ • Statistics                                        │
│ • Preferred ports list                              │
│ • Proc entries                                      │
└─────────────────────────────────────────────────────┘
```

## 5. I/O Path Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              I/O DATA PATH                                   │
└─────────────────────────────────────────────────────────────────────────────┘

┌──────────────────┐                           ┌──────────────────┐
│  REMOTE I/O PATH │                           │  LOCAL I/O PATH  │
│  (From Clients)  │                           │ (From Host OS)   │
└────────┬─────────┘                           └────────┬─────────┘
         │                                              │
         │                                              │
    ┌────▼──────────┐                          ┌───────▼─────────┐
    │ NORDDA Channel│                          │ nvmeibs_um_comm │
    │ (nvmeibs_     │                          │                 │
    │  nordda.c)    │                          │ - Netlink msgs  │
    │               │                          │ - User requests │
    │ - Recv IU     │                          │ - GPT updates   │
    │ - Parse req   │                          └───────┬─────────┘
    └────┬──────────┘                                  │
         │                                             │
         ▼                                             ▼
┌────────────────────────────────────────────────────────────────────┐
│              handle_io_cmd / handle_io_msg                         │
│                                                                     │
│  Parse Request:                                                    │
│  ┌──────────────────────────────────────────────────────┐         │
│  │ • IO Type: Read/Write                                │         │
│  │ • Sector addressing (HW/SW sectors)                  │         │
│  │ • Data length                                        │         │
│  │ • Metadata requirements                              │         │
│  │ • Lock info (LMI - Lock Memory Info)                 │         │
│  │ • Journal entry (if applicable)                      │         │
│  └──────────────────────────────────────────────────────┘         │
│                                                                     │
│  Check with SERJIO:                                                │
│  ┌──────────────────────────────────────────────────────┐         │
│  │ • Verify journal state                               │         │
│  │ • Check for journaled data                           │         │
│  │ • Handle GPT updates                                 │         │
│  │ • Coordinate distributed writes                      │         │
│  └──────────────────────────────────────────────────────┘         │
│                                                                     │
└─────────────────────────────┬───────────────────────────────────────┘
                              │
                              ▼
                   ┌──────────────────────┐
                   │  submit_local_cmd()  │
                   │  (nvmeibs_nvme.c)    │
                   │                      │
                   │ • Allocate NVMe req  │
                   │ • Setup PRP/SGL      │
                   │ • Submit to NVMe     │
                   │ • Register callback  │
                   └──────────┬───────────┘
                              │
                              ▼
                   ┌──────────────────────┐
                   │   NVMe Hardware      │
                   │   (Physical SSD)     │
                   └──────────┬───────────┘
                              │
                              ▼
                   ┌──────────────────────┐
                   │  IO Completion       │
                   │  (nvmeibs_nvme.c)    │
                   │                      │
                   │ • Process status     │
                   │ • Update stats       │
                   │ • Invoke callback    │
                   └──────────┬───────────┘
                              │
         ┌────────────────────┴────────────────────┐
         │                                         │
         ▼                                         ▼
┌────────────────────┐                  ┌────────────────────┐
│ NORDDA Response    │                  │ UM_COMM Response   │
│ (nvmeibs_nordda.c) │                  │ (nvmeibs_um_comm.c)│
│                    │                  │                    │
│ • Encode response  │                  │ • Post event       │
│ • RDMA Write       │                  │ • Netlink reply    │
│   (for reads)      │                  │                    │
│ • Send completion  │                  │                    │
└────────────────────┘                  └────────────────────┘
```

## 6. SERJIO - Serialized Journal I/O

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                 SERJIO - Journal Management (nvmeibs_serjio.c)               │
└─────────────────────────────────────────────────────────────────────────────┘

Purpose: Manages distributed write ordering, journaling, and crash recovery

┌──────────────────────────────────────────────────────┐
│  struct nvmeibs_serjio_disk_private_data             │
├──────────────────────────────────────────────────────┤
│ • Journal Range Allocation Table                     │
│ • Journal Metadata Cache (JMDC) - RDMA accessible    │
│ • GPT (GUID Partition Table) management              │
│ • Journal Garbage Collection (JGC) timer             │
│ • NVMe operation resource pool                       │
│ • IO work queue (io_wq)                              │
│ • Red-black trees for segment tracking               │
│ • Boot ID for recovery                               │
│ • State machine (INIT → READY → ERROR states)        │
└──────────────────────────────────────────────────────┘

Key Operations:
┌────────────────────────────────────────────────────────┐
│ 1. Journal Range Allocation                            │
│    • nvmeibs_serjio_alloc_journal_range()              │
│    • Assign per-client journal entries                 │
│    • Setup RDMA access to JMDC                         │
│                                                        │
│ 2. Journal Entry Management                            │
│    • Track dirty/clean/abandoned entries               │
│    • Coordinate distributed writes                     │
│    • Maintain write ordering                           │
│                                                        │
│ 3. Garbage Collection (JGC)                            │
│    • Clean committed journal entries                   │
│    • Reclaim journal space                             │
│    • Timer-driven or watermark-triggered               │
│                                                        │
│ 4. GPT Updates                                         │
│    • nvmeibs_serjio_gpt_update()                       │
│    • Coordinate partition table changes                │
│    • Ensure consistency across clients                 │
│                                                        │
│ 5. Crash Recovery                                      │
│    • Detect stale journal entries (boot ID mismatch)   │
│    • Replay or discard uncommitted writes              │
│    • Restore consistent state                          │
└────────────────────────────────────────────────────────┘

State Machine:
  SERJIO_UNINIT → SERJIO_GPT_INIT → SERJIO_RD_DB → 
  SERJIO_INIT_JRNL → SERJIO_READY ⇄ SERJIO_ERR_*
```

## 7. NORDDA - Direct RDMA Data Path

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    NORDDA - No RDDA (nvmeibs_nordda.c)                       │
└─────────────────────────────────────────────────────────────────────────────┘

Purpose: High-performance data path using direct RDMA operations

┌────────────────────────────────────────────────┐
│  struct nvmeibs_nr_channel (NoRDDA Channel)    │
├────────────────────────────────────────────────┤
│ • Client reference                             │
│ • Network/QP resources                         │
│ • IU (IO Unit) pool for recv                   │
│ • SRQ (Shared Receive Queue) support           │
│ • Command tracking (underway_cmds)             │
│ • Send/Recv completion queues                  │
│ • Per-channel statistics                       │
└────────────────────────────────────────────────┘

Flow:
┌────────────────────────────────────────────────────────┐
│ 1. Client sends RDMA SEND with IO request              │
│    ↓                                                   │
│ 2. process_recv_completion()                           │
│    • Parse request from IU                             │
│    • Validate client state                             │
│    ↓                                                   │
│ 3. handle_io_cmd()                                     │
│    • new_cmd_underway() - track command                │
│    • can_handle_io_cmd() - check resources             │
│    ↓                                                   │
│ 4. io_cmd_process()                                    │
│    • Submit to disk                                    │
│    • Handle piggyback operations (locks, etc)          │
│    ↓                                                   │
│ 5. IO Completion callback                              │
│    • vex_nrch_io_rsp_srv_base_encode()                 │
│    • For reads: setup RDMA WRITE to client memory      │
│    ↓                                                   │
│ 6. send_io_rsp()                                       │
│    • RDMA WRITE (if read op) + SEND completion         │
│    • Or just SEND completion (if write op)             │
│    ↓                                                   │
│ 7. post_recv_iu() - Return IU to receive pool          │
└────────────────────────────────────────────────────────┘

Features:
• Zero-copy data transfer (RDMA READ/WRITE)
• Piggyback lock operations
• Metadata handling
• Error recovery
• Flow control (pending queue when resources low)
```

## 8. Management Interfaces

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          MANAGEMENT INTERFACES                               │
└─────────────────────────────────────────────────────────────────────────────┘

┌────────────────────────────┐
│  TOMA - Management Service │
│  (nvmeibs_toma.c)          │
├────────────────────────────┤
│ • Volume management        │
│ • Client queries           │
│ • Configuration            │
│ • Statistics               │
│ • Request-response model   │
└────────────────────────────┘
            ▲
            │
            │ Management Commands
            │
┌───────────┴─────────────┐
│  MCS - Control Service  │
│  (nvmeibs_mcs.c)        │
├─────────────────────────┤
│ • Control plane msgs    │
│ • Coordination          │
│ • Proc interface        │
└─────────────────────────┘

┌────────────────────────────────┐
│  UM_COMM - User Mode Comm      │
│  (nvmeibs_um_comm.c)           │
├────────────────────────────────┤
│ • Netlink socket               │
│ • Messages to/from userspace   │
│ • Local disk I/O path          │
│ • GPT update coordination      │
└────────────────────────────────┘

┌────────────────────────────────┐
│  Proc Filesystem               │
│  (/proc/nvmeibs/)              │
├────────────────────────────────┤
│ • /disks/ - per-disk info      │
│ • /clients - client list       │
│ • /gids - network identifiers  │
│ • /stats - statistics          │
│ • /debug - debug info          │
└────────────────────────────────┘
```

## 9. Network/RDMA Layer Details

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                     RDMA/NETWORK LAYER (nvmeibs_net.c)                       │
└─────────────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────┐
│  struct nvmeibs_net                          │
├──────────────────────────────────────────────┤
│ • Connection type (Admin/Lock/IO/NoRDDA)     │
│ • QP (Queue Pair)                            │
│ • CQ (Completion Queue)                      │
│ • SRQ (Shared Receive Queue) - optional      │
│ • CM ID (Connection Manager ID)              │
│ • Message buffers (in_msg_area/out_msg_area) │
│ • Send/Recv ring buffers                     │
│ • Client reference                           │
│ • Port reference                             │
│ • Message header (for addressing)            │
│ • State (connected, draining, etc)           │
└──────────────────────────────────────────────┘

Connection States & Events:
┌──────────────────────────────────────────────────┐
│  target_cm_handler() - CM Event Processing       │
├──────────────────────────────────────────────────┤
│ • NVMEIB_RTU_RECEIVED → start_qp()               │
│ • NVMEIB_DREQ_RECEIVED → disconnection_request() │
│ • NVMEIB_DREP_RECEIVED → drain_qp()              │
│ • NVMEIB_DEVICE_REMOVED → cleanup                │
│ • NVMEIB_REJ_RECEIVED → drain_qp()               │
└──────────────────────────────────────────────────┘

Supported Transports:
• IB (InfiniBand) - Service ID based
• RoCE (RDMA over Converged Ethernet) - Port-based
• iWARP (Internet Wide Area RDMA Protocol)
• TCP mode (fallback)
• Loopback (local client-server)
```

## 10. Locking & Synchronization

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    DISK LOCKS (nvmeibs_disk_locks.c)                         │
└─────────────────────────────────────────────────────────────────────────────┘

Purpose: Distributed locking for coordinating client access

• Lock Memory Info (LMI) - RDMA-accessible lock structures
• Lock ranges per disk
• Client-specific lock resources
• Lock channel for lock operations
• Piggyback lock operations with IO requests
• Lock timeout and recovery
```

## 11. Component Interaction Summary

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                        DATA FLOW SUMMARY                                     │
└─────────────────────────────────────────────────────────────────────────────┘

Client Request (Remote)
    │
    ├─► IB/RoCE/iWARP Listener
    │       │
    │       ├─► Admin Channel → Client Mgmt → Registration
    │       │
    │       ├─► IO Channel → NORDDA → Disk → NVMe Device
    │       │                  ▲
    │       │                  └─── SERJIO (journal coordination)
    │       │
    │       └─► Lock Channel → Disk Locks → Distributed coordination
    │
Local Request (OS/Apps)
    │
    └─► UM_COMM (Netlink) → Disk → NVMe Device
                             ▲
                             └─── SERJIO (journal coordination)

Management/Control
    │
    ├─► TOMA → Volume management, queries, stats
    ├─► MCS → Control plane messages
    └─► Proc FS → Debug, statistics, configuration
```

## 12. File Responsibilities

| File | Primary Responsibility |
|------|----------------------|
| `nvmeibs_main.c` | Module init/exit, device management, IB registration, listeners |
| `nvmeibs_nvme.c` | NVMe driver interface, disk probe/remove, local I/O submission |
| `nvmeibs_client.c` | Client lifecycle, channel connections, keep-alive |
| `nvmeibs_client_db.c` | Client database (hash table, lookups) |
| `nvmeibs_disk.c` | Disk resource management, client-disk linking |
| `nvmeibs_disk_locks.c` | Distributed locking primitives |
| `nvmeibs_ib_port.c` | IB port management, connection acceptance |
| `nvmeibs_net.c` | QP/CQ/SRQ management, RDMA connection handling |
| `nvmeibs_nordda.c` | High-performance I/O data path (NORDDA channels) |
| `nvmeibs_serjio.c` | Journaling, write ordering, crash recovery |
| `nvmeibs_serjio_deps.c` | SERJIO dependency tracking |
| `nvmeibs_serjio_gen_cmd_handlers.c` | Auto-generated SERJIO command handlers |
| `nvmeibs_serjio_stats.c` | SERJIO statistics tracking |
| `nvmeibs_toma.c` | Management interface (TOMA) |
| `nvmeibs_mcs.c` | Management communication service |
| `nvmeibs_um_comm.c` | Usermode communication (netlink), local I/O path |
| `nvmeibs_async_cookies.c` | Async operation tracking |
| `nvmeibs_capabilities.c` | Feature negotiation |
| `nvmeibs_distribute_intrs.c` | Interrupt distribution/shaping |
| `nvmeibs_memmgr_metrics.c` | Memory allocation tracking |

## 13. Key Data Structures

```
struct nvmeibs_client
├─ Client identification (UUID, CID, host_name)
├─ Version info (protocol negotiation)
├─ Network channel (nvmeibs_net for admin)
├─ Disk list (disks accessed)
├─ NIC list (NICs used)
├─ Message buffers (in/out)
├─ Keep-alive state
├─ NORDDA channels
├─ Lock resources
└─ Stats

struct nvmeibs_disk_info
├─ Disk identification (disk_id, UUID)
├─ NVMe device reference
├─ Block device
├─ Geometry (blocks, sectors)
├─ Client list
├─ SERJIO private data
├─ Lock resources
├─ IO queue info
└─ Stats

struct nvmeibs_net
├─ Connection type
├─ QP/CQ/SRQ
├─ CM ID
├─ Message buffers
├─ Client reference
├─ Port reference
└─ State

struct nvmeibs_nr_channel (NORDDA)
├─ Client reference
├─ Network resources
├─ IU pool
├─ SRQ support
├─ Command tracking
└─ Stats

struct nvmeibs_serjio_disk_private_data
├─ Journal range table
├─ JMDC (metadata cache)
├─ GPT management
├─ JGC timer
├─ NVMe op pool
├─ IO work queue
├─ Segment tracking (rbtree)
└─ State machine
```

## 14. Threading Model

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                          THREADING & WORK QUEUES                             │
└─────────────────────────────────────────────────────────────────────────────┘

main_wq (Main Work Queue)
├─ Module initialization tasks
├─ IB client registration
├─ Device hot-plug handling
└─ Synchronous operations requiring context

ioqm_wq (IO Queue Manager Work Queue)
├─ IO queue allocation/deallocation
├─ Queue management
└─ Resource setup

Per-Port Work Queues (nvmeibs_ib_port)
├─ Connection acceptance
├─ Client lifecycle on specific port
└─ Port-specific operations

SERJIO IO Work Queue (per disk)
├─ Journal operations
├─ GPT reads/writes
├─ Garbage collection
└─ Serialized disk operations

Completion Queue Processing (Soft IRQ / Dedicated threads)
├─ Send completions
├─ Receive completions
├─ RDMA operation completions
└─ Error handling

nvmeibs Kernel Thread
├─ Periodic tasks
├─ Monitoring
└─ Maintenance
```

---

## Architecture Highlights

1. **Multi-Channel Architecture**: Separate channels for admin, I/O, locks, and NORDDA
2. **RDMA-Centric Design**: Heavy use of RDMA for zero-copy data transfer
3. **Distributed Coordination**: SERJIO provides journaling and write ordering
4. **High Performance I/O**: NORDDA bypasses extra copies and uses direct RDMA
5. **Flexible Transport**: Supports IB, RoCE, iWARP, TCP, and loopback
6. **Resource Management**: Per-client and per-disk resource tracking
7. **Crash Recovery**: Journal-based recovery with boot ID tracking
8. **Scalability**: Per-port work queues, per-CPU CQs optional, SRQ support
9. **Management**: Multiple management interfaces (TOMA, MCS, proc, netlink)
10. **Observability**: Extensive statistics, tracing, and proc filesystem entries

