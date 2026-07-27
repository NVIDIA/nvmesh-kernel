<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# NVMesh Server - Component Relationship Diagram

## Component Dependency Graph

```
                                    ┌─────────────────────┐
                                    │   nvmeibs_main.c    │
                                    │  (Module Core)      │
                                    │  • Init/Exit        │
                                    │  • Device Registry  │
                                    │  • Listeners        │
                                    └──────────┬──────────┘
                                               │
                   ┌───────────────────────────┼───────────────────────────┐
                   │                           │                           │
                   │                           │                           │
        ┌──────────▼──────────┐    ┌──────────▼──────────┐    ┌──────────▼──────────┐
        │ nvmeibs_ib_port.c   │    │  nvmeibs_nvme.c     │    │  nvmeibs_client.c   │
        │ (Port Management)   │    │  (NVMe Driver)      │    │  (Client Mgmt)      │
        │ • Port discovery    │    │  • PCI probe        │    │  • Lifecycle        │
        │ • Listeners         │    │  • Disk attach      │    │  • Keep-alive       │
        │ • New connections   │    │  • IO submission    │    │  • Channels         │
        └──────────┬──────────┘    └──────────┬──────────┘    └──────────┬──────────┘
                   │                           │                           │
                   │                           │                           │
                   │         ┌─────────────────┼─────────────────┐         │
                   │         │                 │                 │         │
        ┌──────────▼─────────▼─┐    ┌──────────▼──────────┐     │         │
        │   nvmeibs_net.c      │    │  nvmeibs_disk.c     │     │         │
        │   (Network/RDMA)     │    │  (Disk Resources)   │     │         │
        │   • QP/CQ/SRQ        │    │  • Disk registry    │     │         │
        │   • RDMA ops         │    │  • Client-disk link │     │         │
        │   • CM handling      │    │  • Resource alloc   │     │         │
        └───────────┬──────────┘    └──────────┬──────────┘     │         │
                    │                           │                │         │
                    │                           │                │         │
         ┌──────────┼───────────────────────────┼────────────────┼─────────┤
         │          │                           │                │         │
         │          │                ┌──────────▼──────────┐     │         │
         │          │                │ nvmeibs_disk_locks.c│     │         │
         │          │                │ (Distributed Locks) │     │         │
         │          │                │ • Lock ranges       │     │         │
         │          │                │ • LMI management    │     │         │
         │          │                └─────────────────────┘     │         │
         │          │                                            │         │
    ┌────▼──────────▼─────┐              ┌─────────────────────▼─────┐   │
    │ nvmeibs_nordda.c     │              │ nvmeibs_client_db.c       │   │
    │ (NoRDDA I/O Path)    │◄─────────────┤ (Client Database)         │◄──┘
    │ • NORDDA channels    │              │ • Hash table (CID)        │
    │ • IU management      │              │ • Client lookup           │
    │ • Direct RDMA I/O    │              │ • Lifecycle tracking      │
    │ • Send/Recv comp     │              └───────────────────────────┘
    └──────────┬───────────┘
               │
               │
    ┌──────────▼────────────────────────────────────────────┐
    │              nvmeibs_serjio.c                          │
    │          (Serialized Journal I/O)                      │
    │          • Journal management                          │
    │          • Write ordering                              │
    │          • Crash recovery                              │
    │          • GPT coordination                            │
    │          • Garbage collection                          │
    └────────┬───────────────────────────────────────────────┘
             │
             ├──────────────────────┬───────────────────────┐
             │                      │                       │
   ┌─────────▼─────────┐  ┌─────────▼─────────┐  ┌────────▼──────────┐
   │ nvmeibs_serjio_   │  │ nvmeibs_serjio_   │  │ nvmeibs_serjio_   │
   │   deps.c          │  │   gen_cmd_        │  │   stats.c         │
   │ (Dependencies)    │  │   handlers.c      │  │ (Statistics)      │
   └───────────────────┘  │ (Cmd Handlers)    │  └───────────────────┘
                          └───────────────────┘


         ┌─────────────────────────────────────────────────────┐
         │          Management & Control Interfaces            │
         └─────────────────────────────────────────────────────┘

    ┌───────────────────┐       ┌───────────────────┐       ┌───────────────────┐
    │ nvmeibs_toma.c    │       │ nvmeibs_mcs.c     │       │ nvmeibs_um_comm.c │
    │ (TOMA Management) │       │ (MCS Control)     │       │ (User Mode Comm)  │
    │ • Volume mgmt     │       │ • Control plane   │       │ • Netlink         │
    │ • Client queries  │       │ • Coordination    │       │ • Local I/O path  │
    │ • Stats reporting │       │ • Proc interface  │       │ • GPT updates     │
    └───────────────────┘       └───────────────────┘       └───────────────────┘


         ┌─────────────────────────────────────────────────────┐
         │              Support/Utility Components             │
         └─────────────────────────────────────────────────────┘

┌──────────────────────┐  ┌────────────────────────┐  ┌──────────────────────┐
│ nvmeibs_async_       │  │ nvmeibs_capabilities.c │  │ nvmeibs_distribute_  │
│   cookies.c          │  │ (Feature Negotiation)  │  │   intrs.c            │
│ (Async Tracking)     │  │ • Version matching     │  │ (Interrupt Shaping)  │
└──────────────────────┘  │ • Feature flags        │  │ • Interrupt control  │
                          └────────────────────────┘  └──────────────────────┘

┌──────────────────────┐
│ nvmeibs_memmgr_      │
│   metrics.c          │
│ (Memory Metrics)     │
└──────────────────────┘
```

## Layer Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              APPLICATION LAYER                               │
│                          (Management, Configuration)                         │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                  │
│  │     TOMA     │    │     MCS      │    │   UM_COMM    │                  │
│  └──────────────┘    └──────────────┘    └──────────────┘                  │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ▲
                                    │
┌─────────────────────────────────────────────────────────────────────────────┐
│                              CLIENT LAYER                                    │
│                      (Client Management & Tracking)                          │
│  ┌──────────────────────┐         ┌──────────────────────┐                 │
│  │   nvmeibs_client     │◄───────►│  nvmeibs_client_db   │                 │
│  └──────────────────────┘         └──────────────────────┘                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ▲
                                    │
┌─────────────────────────────────────────────────────────────────────────────┐
│                              DATA PLANE LAYER                                │
│                          (I/O Path Processing)                               │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────┐                  │
│  │   NORDDA     │    │   SERJIO     │    │     LOCKS    │                  │
│  │  (I/O Path)  │◄──►│  (Journal)   │◄──►│ (Dist Lock)  │                  │
│  └──────────────┘    └──────────────┘    └──────────────┘                  │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ▲
                                    │
┌─────────────────────────────────────────────────────────────────────────────┐
│                            TRANSPORT LAYER                                   │
│                        (Network & RDMA Operations)                           │
│  ┌──────────────────────┐         ┌──────────────────────┐                 │
│  │   nvmeibs_net        │◄───────►│  nvmeibs_ib_port     │                 │
│  │  (QP/CQ/RDMA)        │         │  (Port Management)   │                 │
│  └──────────────────────┘         └──────────────────────┘                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ▲
                                    │
┌─────────────────────────────────────────────────────────────────────────────┐
│                            STORAGE LAYER                                     │
│                        (Disk & NVMe Operations)                              │
│  ┌──────────────────────┐         ┌──────────────────────┐                 │
│  │   nvmeibs_disk       │◄───────►│   nvmeibs_nvme       │                 │
│  │  (Disk Resources)    │         │  (NVMe Driver)       │                 │
│  └──────────────────────┘         └──────────────────────┘                 │
└─────────────────────────────────────────────────────────────────────────────┘
                                    ▲
                                    │
                                    ▼
                          ┌────────────────────┐
                          │   NVMe Hardware    │
                          │   (Physical SSDs)  │
                          └────────────────────┘
```

## Request Flow Through Components

### Remote Client I/O Request

```
1. Network Arrival
   IB/RoCE/iWARP Transport
            │
            ▼
   nvmeibs_ib_port::listener
            │
            ▼
   nvmeibs_net::cm_handler
            │
            ▼
2. Client Identification
   nvmeibs_client_db::lookup(CID)
            │
            ▼
   nvmeibs_client (found)
            │
            ▼
3. Channel Processing
   nvmeibs_nordda::process_recv_completion()
            │
            ├─► Parse IO request
            ├─► Validate permissions
            ├─► Check resources
            │
            ▼
4. Journal Coordination
   nvmeibs_serjio::check_journal_state()
            │
            ├─► Verify write ordering
            ├─► Update journal metadata
            ├─► Check for conflicts
            │
            ▼
5. Disk Locking (if needed)
   nvmeibs_disk_locks::acquire_lock()
            │
            ▼
6. Storage Layer
   nvmeibs_disk::locate_disk()
            │
            ▼
   nvmeibs_nvme::submit_local_cmd()
            │
            ▼
   NVMe Hardware
            │
            ▼
7. Completion Path
   nvmeibs_nvme::completion_callback()
            │
            ▼
   nvmeibs_nordda::send_io_rsp()
            │
            ├─► For reads: RDMA WRITE data
            ├─► Send completion message
            │
            ▼
   nvmeibs_serjio::update_journal() (if write)
            │
            ▼
   Client receives completion
```

### Local I/O Request

```
1. User Space Request
   Application → Kernel
            │
            ▼
   nvmeibs_um_comm::netlink_handler
            │
            ▼
2. Request Processing
   parse IO parameters
            │
            ▼
3. Journal Check
   nvmeibs_serjio::gpt_update() (if GPT)
            │
            ▼
4. Storage Access
   nvmeibs_disk::locate()
            │
            ▼
   nvmeibs_nvme::submit_local_cmd()
            │
            ▼
   NVMe Hardware
            │
            ▼
5. Completion
   nvmeibs_um_comm::post_io()
            │
            ▼
   Netlink event to userspace
```

## Inter-Component Communication

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          Synchronization Points                          │
└─────────────────────────────────────────────────────────────────────────┘

nvmeibs_client ◄──────────────► nvmeibs_client_db
    (Registration, lookup, removal)

nvmeibs_client ◄──────────────► nvmeibs_disk
    (Resource allocation, disk access)

nvmeibs_nordda ◄──────────────► nvmeibs_serjio
    (Journal coordination, write ordering)

nvmeibs_nordda ◄──────────────► nvmeibs_disk_locks
    (Lock acquisition, release)

nvmeibs_serjio ◄──────────────► nvmeibs_nvme
    (Direct disk I/O for journal)

nvmeibs_disk ◄──────────────► nvmeibs_nvme
    (I/O submission to disk)

nvmeibs_net ◄──────────────► nvmeibs_ib_port
    (QP creation, port resources)

nvmeibs_toma ◄──────────────► nvmeibs_client_db
    (Client queries, statistics)

nvmeibs_um_comm ◄──────────────► nvmeibs_disk
    (Local I/O path)

┌─────────────────────────────────────────────────────────────────────────┐
│                          Shared Resources                                │
└─────────────────────────────────────────────────────────────────────────┘

Work Queues:
• main_wq - Shared by main module, initialization tasks
• ioqm_wq - Shared by disk/nvme for queue management
• per-port wqs - Owned by ib_port, used by client/net
• serjio io_wq - Per-disk, owned by serjio

Memory Pools:
• Client message buffers (nvmeibs_client)
• NORDDA IU pools (nvmeibs_nordda)
• SERJIO NVMe op pools (nvmeibs_serjio)
• Journal metadata cache (nvmeibs_serjio)

Lists:
• Global device list (nvmeibs_main)
• Global disk list (nvmeibs_disk)
• Global client list (nvmeibs_client_db)
• Per-disk client lists (nvmeibs_disk)
• Per-client disk lists (nvmeibs_client)
```

## Critical Locks & Synchronization

```
┌────────────────────────────────────────────────────────────────┐
│                      Lock Hierarchy                             │
└────────────────────────────────────────────────────────────────┘

Level 1: Module-wide
  • guard (nvmeibs_main.c) - Module state
  • device_list_lock - Device registry

Level 2: Subsystem-wide
  • nvmeibs_cdb.lock - Client database
  • disks_lock (nvmeibs_disk.c) - Disk list

Level 3: Per-resource
  • nvmeibs_client.lock - Client state
  • nvmeibs_disk_info.lock - Disk state
  • nvmeibs_serjio_pd.lock - SERJIO state
  • nvmeibs_net.lock - Network/QP state
  • nvmeibs_nr_channel.spinlock - Channel state

Level 4: Fine-grained
  • Journal range locks
  • JMDC locks
  • Lock ranges (for distributed locking)
  • GPT rwsem

Rules:
• Always acquire locks in level order (1→2→3→4)
• Hold for minimal time
• Use RCU where possible for read-mostly data
• Prefer per-resource locks over global locks
```

---

## Summary

The NVMesh server architecture is organized into clear layers:

1. **Transport Layer** - Handles RDMA/network operations
2. **Client Layer** - Manages client connections and state
3. **Data Plane** - Processes I/O requests (NORDDA, SERJIO, Locks)
4. **Storage Layer** - Interfaces with NVMe devices
5. **Management Layer** - Configuration and monitoring

Key architectural patterns:
- **Separation of concerns** - Each component has clear responsibilities
- **Asynchronous processing** - Work queues for non-blocking operations
- **Resource pooling** - Efficient memory management
- **Event-driven** - Completion-based I/O processing
- **Distributed coordination** - SERJIO for consistency
- **High performance** - Direct RDMA, zero-copy paths

