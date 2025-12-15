# NVMesh Server Architecture Documentation

This directory contains the NVMesh server module implementation, which provides remote access to local NVMe storage devices over RDMA networks.

## Documentation Files

This architecture documentation consists of four files:

1. **README_ARCHITECTURE.md** (this file) - Overview and quick start
2. **[ARCHITECTURE_DIAGRAM.md](ARCHITECTURE_DIAGRAM.md)** - Detailed architecture with initialization flow, I/O paths, and component descriptions
3. **[COMPONENT_DIAGRAM.md](COMPONENT_DIAGRAM.md)** - Component relationships, dependencies, and layer architecture
4. **[DATA_STRUCTURES.md](DATA_STRUCTURES.md)** - Key data structures and their relationships

## Quick Overview

The NVMesh server is a Linux kernel module that:
- Exposes local NVMe SSDs to remote clients over RDMA networks
- Supports InfiniBand, RoCE, iWARP, and TCP transports
- Provides distributed locking and journaling (SERJIO)
- Enables high-performance zero-copy data transfer
- Handles crash recovery and consistency

## Architecture at a Glance

```
┌─────────────────────────────────────────────────────────────┐
│                    Remote Clients                            │
│          (Running NVMesh Client - /clnt module)              │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        │ RDMA (IB/RoCE/iWARP)
                        │
┌───────────────────────▼─────────────────────────────────────┐
│              NVMesh Server (/srv module)                     │
├──────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   Client     │  │   NORDDA     │  │   SERJIO     │      │
│  │   Manager    │  │  (I/O Path)  │  │ (Journaling) │      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│                                                              │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐      │
│  │   Network    │  │    Disk      │  │    Locks     │      │
│  │  (RDMA/QP)   │  │   Manager    │  │ (Distributed)│      │
│  └──────────────┘  └──────────────┘  └──────────────┘      │
│                                                              │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        │ NVMe Commands
                        │
┌───────────────────────▼─────────────────────────────────────┐
│                   NVMe Driver (nvme.ko)                      │
└───────────────────────┬─────────────────────────────────────┘
                        │
                        │ PCIe
                        │
┌───────────────────────▼─────────────────────────────────────┐
│              Physical NVMe SSDs (Hardware)                   │
└─────────────────────────────────────────────────────────────┘
```

## Core Components

### 1. **Client Management** (`nvmeibs_client.c`, `nvmeibs_client_db.c`)
- Tracks connected clients
- Manages client lifecycle (connect, keep-alive, disconnect)
- Maintains client database with fast CID-based lookup

### 2. **Network/RDMA Layer** (`nvmeibs_net.c`, `nvmeibs_ib_port.c`)
- Manages RDMA connections (QP, CQ, SRQ)
- Handles connection management (CM) events
- Supports multiple transports (IB, RoCE, iWARP)

### 3. **NORDDA - High-Performance I/O** (`nvmeibs_nordda.c`)
- Direct RDMA data path for client I/O
- Zero-copy data transfer
- Supports RDMA READ/WRITE operations
- Handles I/O completions and responses

### 4. **SERJIO - Journaling** (`nvmeibs_serjio.c`)
- Distributed write ordering
- Journal management and garbage collection
- Crash recovery coordination
- GPT (partition table) management

### 5. **Disk Management** (`nvmeibs_disk.c`, `nvmeibs_nvme.c`)
- Discovers and manages NVMe disks
- Handles disk-to-client resource allocation
- Submits I/O to NVMe devices
- Tracks disk statistics

### 6. **Distributed Locking** (`nvmeibs_disk_locks.c`)
- Coordinates client access to shared data
- RDMA-accessible lock structures
- Piggyback lock operations with I/O

### 7. **Management Interfaces** (`nvmeibs_toma.c`, `nvmeibs_mcs.c`, `nvmeibs_um_comm.c`)
- TOMA: Volume management and queries
- MCS: Control plane messaging
- UM_COMM: Usermode communication (netlink)

## Key Files

| File | Lines | Purpose |
|------|-------|---------|
| `nvmeibs_main.c` | ~4,300 | Module initialization, device management, listeners |
| `nvmeibs_nvme.c` | ~7,600 | NVMe driver interface, I/O submission |
| `nvmeibs_client.c` | ~9,200 | Client lifecycle, channels, keep-alive |
| `nvmeibs_nordda.c` | ~10,400 | High-performance NORDDA I/O path |
| `nvmeibs_serjio.c` | ~10,400 | Journaling, write ordering, recovery |
| `nvmeibs_disk.c` | ~4,000 | Disk resource management |
| `nvmeibs_net.c` | ~3,000 | RDMA connection management |
| `nvmeibs_ib_port.c` | ~3,000 | Port management, connection acceptance |
| `nvmeibs_toma.c` | ~4,000 | Management interface |
| `nvmeibs_um_comm.c` | ~5,000 | Usermode communication, local I/O |

## Request Flow Examples

### Remote Client Read Request

```
1. Client sends RDMA SEND with read request
2. Server receives on NORDDA channel (nvmeibs_nordda.c)
3. Request validated and parsed
4. SERJIO checks journal state (nvmeibs_serjio.c)
5. Lock acquired if needed (nvmeibs_disk_locks.c)
6. Read submitted to NVMe device (nvmeibs_nvme.c)
7. NVMe completion processed
8. Server performs RDMA WRITE to client memory
9. Server sends completion message
10. Lock released, journal updated
```

### Remote Client Write Request

```
1. Client sends RDMA SEND with write request + data
2. Server receives on NORDDA channel
3. Request validated and parsed
4. SERJIO coordinates journal entry (nvmeibs_serjio.c)
5. Lock acquired
6. Write submitted to NVMe device
7. NVMe completion processed
8. Journal updated, entry marked clean
9. Server sends completion message
10. Lock released
```

### Local I/O Request (from host OS)

```
1. Application issues I/O via block device
2. Netlink message to nvmeibs_um_comm.c
3. SERJIO checks for GPT updates if needed
4. I/O submitted to NVMe device (nvmeibs_nvme.c)
5. Completion returned via netlink
```

## Threading Model

```
main_wq
├─ Module initialization
├─ Device hot-plug
└─ Synchronous operations

ioqm_wq
├─ I/O queue management
└─ Queue allocation

Per-Port Work Queues
├─ Connection acceptance
├─ Client operations
└─ Port-specific tasks

SERJIO I/O Work Queue (per disk)
├─ Journal operations
├─ GPT management
└─ Garbage collection

Completion Queues (CQ processing)
├─ Send completions
├─ Receive completions
└─ RDMA completions

nvmeibs Kernel Thread
├─ Periodic maintenance
└─ Monitoring
```

## Important Concepts

### Client Lifecycle
1. **Connection**: Client connects admin channel
2. **Registration**: Client registered in database
3. **Resource Allocation**: Disk resources allocated
4. **I/O Channels**: NORDDA channels established
5. **Active**: Client performs I/O
6. **Keep-Alive**: Periodic health checks
7. **Disconnect**: Graceful or error-based cleanup
8. **Resource Release**: Resources freed

### SERJIO States
```
UNINIT → GPT_INIT → RD_DB → INIT_JRNL → READY
                                          ↕
                                        ERROR
```

### Channel Types
- **Admin Channel**: First connection, management commands
- **I/O Channels (NORDDA)**: High-performance data path
- **Lock Channel**: Distributed locking operations
- **Secondary Lock Channel**: Additional lock resources

### Transport Support
- **InfiniBand**: Native IB, service ID based
- **RoCE**: RDMA over Ethernet, port 4791
- **iWARP**: Internet RDMA, multiple ports
- **TCP**: Fallback mode
- **Loopback**: Local client-server

## Configuration

Key module parameters (see `nvmeibs_main.c`):

```bash
# Debug levels
nvmeibs.debug_level=1             # General debug level (0-2)
nvmeibs.tracer_debug_level=4      # Control path tracing (0-4)
nvmeibs.goodpath_debug_level=2    # Data path tracing

# Filtering
nvmeibs.ports="mlx5_0"            # Filter by NIC name
nvmeibs.guids="0x..."             # Filter by GUID

# Performance
nvmeibs.use_pcpu_cq=0             # Per-CPU completion queues
nvmeibs.max_req_size=1048576      # Max request size
```

## Debugging

### Proc Filesystem
```bash
/proc/nvmeibs/
├── clients              # Active clients
├── disks/
│   └── <disk-id>/
│       ├── info         # Disk information
│       ├── clients      # Clients using disk
│       ├── serjio/      # Journal info
│       └── stats        # I/O statistics
├── gids                 # Network identifiers
├── stats                # Global statistics
└── nic_stats/           # Per-NIC statistics
```

### Tracing
The module uses an extensive tracing system (see `nvmeibs_trace.h`):
- Trace points throughout code
- Configurable debug levels
- Post-processing tools in `/tools/traces_post_processor/`

### Common Issues
1. **Client connection failure**: Check ports/guids filtering, verify RDMA connectivity
2. **I/O errors**: Check disk state, SERJIO status, journal space
3. **Lock timeouts**: Check network latency, client health
4. **Memory allocation failures**: Check system memory, adjust buffer sizes

## Building

```bash
# Build server module
cd /home/jholzman/src/ssda
make -C srv/

# Install
sudo insmod srv/nvmeibs.ko

# Uninstall
sudo rmmod nvmeibs
```

## Dependencies

### Kernel Modules
- `nvme.ko` - NVMe driver
- `nvmeib.ko` - Common NVMesh infrastructure (from `/common/`)
- RDMA drivers: `ib_core`, `rdma_cm`, `mlx5_core`, etc.

### Build Dependencies
- Linux kernel headers
- RDMA development libraries
- Compiler toolchain

## Performance Considerations

1. **NORDDA vs Regular Path**: NORDDA provides lowest latency
2. **Per-CPU CQs**: Enable for high core count systems
3. **SRQ**: Reduces memory overhead for many clients
4. **Journal Size**: Larger journal = better write coalescing
5. **Lock Ranges**: Fine-grained locking reduces contention

## Related Documentation

- `/clnt/` - Client module (counterpart to server)
- `/common/` - Shared infrastructure
- `/tools/` - Utilities and scripts
- `BUILD-ME-STEPS-LINUX.md` - Build instructions

## Architecture Decisions

### Why separate channels?
- **Admin**: Infrequent, can tolerate latency
- **I/O (NORDDA)**: High throughput, low latency critical
- **Lock**: Separate from I/O to avoid head-of-line blocking

### Why SERJIO?
- Distributed consistency without centralized coordinator
- Crash recovery without full sync
- Write ordering across multiple clients

### Why per-port work queues?
- Scalability: Parallel connection processing
- Isolation: Port failures don't affect others
- Affinity: Can pin to NUMA nodes

### Why NORDDA?
- Zero-copy: RDMA READ/WRITE directly
- Low latency: Bypasses extra protocol layers
- Piggyback: Lock operations with data

## Future Enhancements

Potential areas for improvement:
- NVME-oF integration for hybrid deployments
- Enhanced statistics and observability
- Dynamic resource tuning
- Multi-path I/O optimization
- Improved error recovery

---

For detailed information, see:
- [ARCHITECTURE_DIAGRAM.md](ARCHITECTURE_DIAGRAM.md) - Complete architecture
- [COMPONENT_DIAGRAM.md](COMPONENT_DIAGRAM.md) - Component relationships
- [DATA_STRUCTURES.md](DATA_STRUCTURES.md) - Data structure details

**Last Updated**: December 2025

