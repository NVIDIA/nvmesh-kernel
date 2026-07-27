<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# NVMesh Server - Key Data Structures

## Core Data Structures and Their Relationships

### 1. Client Structure (`struct nvmeibs_client`)

Located in: `nvmeibs_client.h`

```c
struct nvmeibs_client {
    // Identification
    uuid_be client_uuid;                    // Unique client identifier
    u32 cid;                                // Client ID (numeric)
    char host_name[NVMEIB_HOST_NAME_LEN];   // Client hostname
    char disk_name[...];                    // Disk being accessed
    char name[NVMEIBS_CLIENT_NAME_SIZE];    // Full client name
    
    // Network & Communication
    struct nvmeibs_ib_port *ib_port;        // Associated IB port
    struct nvmeibs_net *net;                // Admin channel (QP)
    struct list_head anics;                 // Admin NICs list
    struct list_head lnics;                 // Local NICs list
    
    // Disk Resources
    struct nvmeibs_disk_info *di;           // Linked disk
    bool disk_linked;                       // Disk link status
    struct list_head disks;                 // List of disks used
    int n_disks;                            // Number of disks
    struct list_head ldisk_link;            // Link for local disk list
    
    // RDMA Message Buffers
    struct nvmeib_alloc_n_map in_msg_area_map;
    struct nvmeib_alloc_n_map out_msg_area_map;
    void *in_msg_area;
    void *out_msg_area;
    
    // Keep-Alive
    struct nvmeib_hdr *ka_msg_area;         // Keep-alive message
    u64 ka_msg_dma_addr;                    // DMA address for KA
    atomic_t ka_sent;                       // KA in flight
    unsigned long ka_timeout;               // Timeout value
    u64 ka_id;                              // KA sequence number
    enum nvmeibs_ka_state ka_state;         // KA state machine
    spinlock_t ka_spinlock;                 // KA synchronization
    
    // NORDDA (I/O Channels)
    struct nvmeibs_nr_channel **nrchs;      // Array of NORDDA channels
    int n_nrchs;                            // Number of channels
    u32 nrch_ioreq_num;                     // I/O requests per channel
    u32 max_wrs_per_req;                    // Max WRs per request
    
    // Lock Resources
    struct nvmeibs_client_lock_resources *lrsc;
    
    // Version & Capabilities
    union nvmeib_version c_version;         // Client version
    union nvmeib_version link_version;      // Negotiated version
    
    // State & Lifecycle
    atomic_t dying;                         // Shutdown in progress
    bool is_local;                          // Local client flag
    struct completion ldisk_done;           // Local disk completion
    
    // Database Integration
    struct nvmeibs_cdb_item cdb;            // DB membership
        struct list_head list;              // Global list link
        struct hlist_node hcid;             // Hash table link
        struct list_head dying_link;        // Dying list link
    
    // Statistics & Async Ops
    struct nvmeibs_async_cookie_store cookie_store;
    struct volume_server_rsp rsp;           // Response buffer
};
```

**Relationships:**
- `1:1` with `nvmeibs_net` (admin channel)
- `1:N` with `nvmeibs_nr_channel` (NORDDA channels)
- `N:1` with `nvmeibs_ib_port` (port)
- `N:M` with `nvmeibs_disk_info` (can access multiple disks)
- Member of `nvmeibs_cdb` (client database)

---

### 2. Disk Structure (`struct nvmeibs_disk_info`)

Located in: `nvmeibs_types.h`

```c
struct nvmeibs_disk_info {
    // Identification
    char disk_id[NVMEIB_DISK_MAX_NVMEXPRESS_ID_SIZE];  // Disk identifier
    uuid_be disk_uuid;                                  // UUID
    
    // NVMe Device
    struct nvmeibs_dev *dev;                            // NVMe device handle
    struct nvme_ctrl *ctrl;                             // NVMe controller
    struct nvme_ns *ns;                                 // NVMe namespace
    struct gendisk *disk;                               // Block device
    
    // Geometry
    u64 hw_blocks;                                      // HW block count
    u64 sw_blocks;                                      // SW block count
    u32 hw_sector_size;                                 // HW sector size
    u32 sw_sector_size;                                 // SW sector size
    
    // Metadata
    bool metadata;                                      // Has metadata
    bool mtdt_extd;                                     // Extended metadata
    u16 mtdt_size;                                      // Metadata size
    
    // Client Management
    struct list_head clients;                           // List of clients
    int n_clients;                                      // Client count
    spinlock_t clients_lock;                            // Client list lock
    
    // Lock Resources
    struct nvmeibs_disk_lock_info *lock_info;           // Distributed locks
    
    // SERJIO (Journaling)
    struct nvmeibs_serjio_disk_private_data *serjio_pd; // Journal data
    
    // I/O Queue Management
    struct nvmeibs_ioq_mgr *ioq_mgr;                    // I/O queue manager
    struct list_head ioqs;                              // I/O queues
    int n_ioqs;                                         // Queue count
    
    // Network Resources
    struct list_head prefered_ports;                    // Preferred ports
    
    // Statistics
    struct nvmeibs_disk_stats stats;                    // Performance stats
    
    // Proc Filesystem
    struct proc_dir_entry *disk_proc_dir;               // /proc entry
    
    // State
    atomic_t ref_count;                                 // Reference count
    bool removing;                                      // Removal in progress
    
    // Global List
    struct list_head nvmeibs_disks_list_n;              // Global disk list
};
```

**Relationships:**
- `N:1` with `nvmeibs_dev` (NVMe device)
- `1:N` with `nvmeibs_client` (multiple clients)
- `1:1` with `nvmeibs_serjio_disk_private_data` (journaling)
- `1:1` with `nvmeibs_disk_lock_info` (locking)
- Member of global disk list

---

### 3. Network/QP Structure (`struct nvmeibs_net`)

Located in: `nvmeibs_net.h`

```c
struct nvmeibs_net {
    // Connection Type
    enum s_net_t net_type;                  // ADMIN/LOCK/IO/NORDDA
    
    // RDMA Resources
    struct nvmeib_qp *qp;                   // Queue Pair
    struct nvmeib_cq *scq;                  // Send CQ
    struct nvmeib_cq *rcq;                  // Recv CQ
    struct nvmeib_srq *srq;                 // Shared Receive Queue
    
    // Connection Management
    struct nvmeib_cm_id cm_id;              // CM identifier
    struct rdma_cm_id *cm_id_ptr;           // RDMA CM ID
    bool connection_accepted;               // Accept status
    
    // Message Buffers
    struct nvmeib_iu *send_ring;            // Send ring buffer
    struct nvmeib_iu *recv_ring;            // Recv ring buffer
    int send_ring_size;                     // Send ring entries
    int recv_ring_size;                     // Recv ring entries
    
    // Message Header (for addressing)
    struct nvmeibs_msg_hdr *msg_hdr;        // Message header
    u64 msg_hdr_dma_addr;                   // DMA address
    
    // Client & Port References
    struct nvmeibs_client *cl;              // Associated client
    struct nvmeibs_ib_port *port;           // Associated port
    
    // Parameters
    struct nvmeibs_net_allocate_params params;
    
    // State
    enum nvmeibs_net_state state;           // Connection state
    atomic_t n_flush;                       // Flush counter
    
    // Handlers
    void (*scq_handler)(void *context);     // Send completion
    void (*rcq_handler)(void *context);     // Recv completion
    void *scq_context;                      // Send context
    void *rcq_context;                      // Recv context
    
    // Statistics
    struct nvmeibs_net_stats stats;         // Network stats
};
```

**Relationships:**
- `1:1` with `nvmeibs_client` (for admin channel)
- `1:1` with `nvmeibs_qp` (RDMA queue pair)
- `N:1` with `nvmeibs_ib_port` (port)
- Created/managed via `nvmeibs_ib_port` work queue

---

### 4. NORDDA Channel (`struct nvmeibs_nr_channel`)

Located in: `nvmeibs_nordda.h`

```c
struct nvmeibs_nr_channel {
    // Identification
    char name[NVMEIBS_CONNECTION_NAME_SIZE];    // Channel name
    int qp_num;                                  // QP number
    
    // Client Reference
    struct nvmeibs_client *cl;                   // Owning client
    
    // Network Resources
    struct nvmeibs_net *net;                     // Underlying QP/CQ
    
    // IU (IO Unit) Management
    struct list_head rxiu_list;                  // Receive IU list
    int n_rxiu;                                  // Active IUs
    int n_rxiu_tot;                              // Total IUs allocated
    int rxiu_dying;                              // IUs being freed
    
    // SRQ Support
    bool priv_srq;                               // Private SRQ flag
    struct nvmeib_srq *srq;                      // SRQ reference
    
    // Command Tracking
    atomic_t underway_cmds;                      // Commands in flight
    spinlock_t spinlock;                         // Channel lock
    
    // Pending Messages
    struct list_head pending_received_msgs;      // Deferred messages
    int n_pending_received_msgs;                 // Pending count
    
    // Statistics
    struct nvmeibs_nr_channel_stats stats;       // Channel stats
    
    // Disk Linkage
    struct nvmeibs_nr_disk *nrdisk;              // Disk reference
    struct nvmeibs_rionic *rionic;               // Remote ionic
};
```

**Relationships:**
- `N:1` with `nvmeibs_client` (client has multiple channels)
- `1:1` with `nvmeibs_net` (network resources)
- Processes I/O requests from client
- Interacts with `nvmeibs_serjio` for journal coordination

---

### 5. SERJIO Disk Private Data (`struct nvmeibs_serjio_disk_private_data`)

Located in: `nvmeibs_serjio.c`

```c
struct nvmeibs_serjio_disk_private_data {
    // Disk Reference
    struct nvmeibs_disk_info *di;            // Associated disk
    
    // Boot & Identity
    char boot_id[NVMEIB_GID_STR_MAX];        // SERJIO boot ID
    
    // Journal Metadata Cache (RDMA-accessible)
    struct {
        size_t len;                          // Cache size
        int n_pages;                         // Page count
        struct page **pages;                 // Page array
        struct nvmeib_ref refcount;          // Reference count
    } jmdc_mem;
    
    // JMDC Mappings (per-NIC)
    struct list_head jmdc_mems;              // List of mappings
    spinlock_t jmdc_mem_lock;                // Mapping lock
    
    // Journal Range Allocation
    struct jranges_allocation_table jranges_alloc_tbl;
        // Tracks which ranges are assigned to which clients
    
    struct nvmeibs_serjio_disk_ranges disk_ranges;
        // On-disk journal structure
    
    // NVMe Operation Pool
    struct nvme_op_rsrc_pool nvme_op_rsrc_pool;
    
    // I/O Work Queue
    struct workq_struct *io_wq;              // Serialized I/O queue
    int io_wq_pid;                           // Work queue PID
    atomic_t io_wq_cnt;                      // Work count
    struct completion io_wq_cmp;             // Completion
    struct list_head io_wq_pend_list;        // Pending work
    unsigned io_wq_pend_cnt;                 // Pending count
    spinlock_t io_wq_pend_lock;              // Pending lock
    
    // State Machine
    spinlock_t state_lock;                   // State lock
    enum nvmeibs_serjio_state state;         // Current state
    
    // GPT (GUID Partition Table)
    struct gpt_header gpt_hdr;               // GPT header
    struct gpt_entry *gpt_entry;             // GPT entries
    unsigned n_gpt_ents;                     // Entry count
    size_t gpt_ents_sz;                      // Total size
    struct rw_semaphore gpt_rwsem;           // GPT lock
    
    // Timers
    struct timer_list jgc_timer;             // Journal GC timer
    struct timer_list wq_pend_timer;         // Work queue timer
    
    // Red-Black Trees
#if KS_RB_ROOT_CACHED
    struct rb_root_cached jrnl_seg_rb_root;  // Active segments
    struct rb_root_cached del_seg_rb_root;   // Deleted segments
#else
    struct rb_root jrnl_seg_rb_root;
    struct rb_root del_seg_rb_root;
#endif
    
    // Statistics
    struct nvmeibs_serjio_stats stats;       // SERJIO stats
    
    // Proc Entries
    struct proc_dir_entry *disk_proc_dir;
    // ... various proc file entries ...
};
```

**Relationships:**
- `1:1` with `nvmeibs_disk_info` (one per disk)
- `1:N` with client journal ranges
- `1:N` with JMDC mappings (per NIC)
- Coordinates with `nvmeibs_nordda` during I/O

---

### 6. IB Port (`struct nvmeibs_ib_port`)

Located in: `nvmeibs_ib_port.h`

```c
struct nvmeibs_ib_port {
    // Device & Port
    struct nvmeibs_dev *nis_dev;             // Parent device
    int port;                                // Port number
    
    // Port Attributes
    struct nvmeibs_ib_port_attrib port_attrib;
        u32 max_req_size;                    // Max request size
        u32 max_rsp_size;                    // Max response size
        u32 max_send_sge;                    // Max SGEs
        // ... other limits ...
    
    // GID (Global Identifier)
    struct nvmeibs_gid gid;                  // Port GID
    
    // Link Layer
    enum nvmeib_link_layer layer;            // IB/RoCE/iWARP
    
    // Work Queue
    struct workq_struct *wq;                 // Port work queue
    int wq_pid;                              // Work queue PID
    
    // Listeners
    struct rdma_cm_id *loop_listener;        // Loopback listener
    
    // Clients
    struct list_head clients;                // Clients on this port
    int n_clients;                           // Client count
    
    // Reference Counting
    struct nvmeib_ref n_port_conns;          // Connection count
    
    // State
    bool active;                             // Port active
    bool stopping;                           // Stopping flag
    
    // Statistics
    struct nvmeibs_ib_port_stats stats;      // Port stats
};
```

**Relationships:**
- `N:1` with `nvmeibs_dev` (device has multiple ports)
- `1:N` with `nvmeibs_client` (port serves multiple clients)
- `1:N` with `nvmeibs_net` (QPs created on port)

---

### 7. Client Database (`struct nvmeibs_cdb`)

Located in: `nvmeibs_client_db.c`

```c
struct nvmeibs_cdb {
    // Client Lists
    struct list_head list;                   // Active clients
    struct list_head dying_list;             // Dying clients
    
    // Hash Table (by CID)
    DECLARE_HASHTABLE(hcid, NVMEIBS_CDB_BITS);  // 16 buckets
    
    // Synchronization
    spinlock_t lock;                         // Database lock
    
    // Counters
    int count;                               // Active count
    int dying_count;                         // Dying count
    
    // State
    bool no_new_clients;                     // Shutdown flag
    
    // Removal Synchronization
    int remove_all_waiters;                  // Waiters count
    struct completion remove_all_comp;       // Completion
};
```

**Relationships:**
- Contains all `nvmeibs_client` structures
- Provides fast lookup by CID (hash table)
- Provides iteration (list)
- Manages lifecycle (dying list)

---

## Data Structure Relationship Diagram

```
                    ┌─────────────────────┐
                    │ nvmeibs_cdb         │
                    │ (Client Database)   │
                    └──────────┬──────────┘
                               │
                               │ contains
                               │
                    ┌──────────▼──────────┐
                    │ nvmeibs_client      │◄──────────┐
                    │ (Client State)      │           │
                    └──┬────────┬────────┬┘           │
                       │        │        │            │
         ┌─────────────┘        │        └──────┐     │
         │                      │               │     │
         │ admin_ch             │ nrchs[]       │     │
         │                      │               │     │
    ┌────▼─────┐      ┌─────────▼─────────┐    │     │
    │nvmeibs_  │      │ nvmeibs_nr_channel│    │     │
    │  net     │      │ (NORDDA I/O Ch)   │    │     │
    │(Admin QP)│      └─────────┬─────────┘    │     │
    └────┬─────┘                │              │     │
         │                      │              │     │
         │ uses                 │ uses         │     │
         │                      │              │     │
    ┌────▼──────────────────────▼──┐           │     │
    │   nvmeibs_ib_port            │           │     │
    │   (IB Port)                  │           │     │
    └────┬─────────────────────────┘           │     │
         │                                     │     │
         │ belongs_to                          │     │
         │                                     │     │
    ┌────▼─────┐                               │     │
    │nvmeibs_  │                               │     │
    │  dev     │                               │     │
    │(NIC)     │                               │     │
    └──────────┘                               │     │
                                               │     │
                                               │ accesses
                                               │     │
                               ┌───────────────▼─────▼───┐
                               │   nvmeibs_disk_info      │
                               │   (Disk)                 │
                               └───┬──────────────────┬───┘
                                   │                  │
                                   │ has              │ has
                                   │                  │
                  ┌────────────────▼──┐    ┌──────────▼──────────┐
                  │nvmeibs_serjio_pd  │    │nvmeibs_disk_lock_   │
                  │(Journaling)       │    │  info (Locking)     │
                  └───────────────────┘    └─────────────────────┘
```

## Key Collections

### Global Lists
```
1. Device List (nvmeibs_main.c)
   LIST: used_dev_list
   TYPE: struct nvmeibs_dev
   LOCK: device_list_lock
   
2. Disk List (nvmeibs_disk.c)
   LIST: nvmeibs_disks_list
   TYPE: struct nvmeibs_disk_info
   LOCK: disks_lock
   
3. Client Database (nvmeibs_client_db.c)
   LIST: nvmeibs_cdb.list
   HASH: nvmeibs_cdb.hcid
   TYPE: struct nvmeibs_client
   LOCK: nvmeibs_cdb.lock
```

### Per-Resource Lists
```
1. Per-Disk Client List
   LIST: nvmeibs_disk_info.clients
   TYPE: struct nvmeibs_client
   LOCK: nvmeibs_disk_info.clients_lock
   
2. Per-Client Disk List
   LIST: nvmeibs_client.disks
   TYPE: struct nvmeibs_cdisk
   
3. Per-Port Client List
   LIST: nvmeibs_ib_port.clients
   TYPE: struct nvmeibs_client
   
4. Per-Channel IU List
   LIST: nvmeibs_nr_channel.rxiu_list
   TYPE: struct nvmeib_iu
   LOCK: nvmeibs_nr_channel.spinlock
```

---

## Memory Ownership

```
Allocated by main:
• nvmeibs_dev (per NIC)
• nvmeibs_ib_port (per port)

Allocated by nvme:
• nvmeibs_disk_info (per disk)
• nvmeibs_dev (NVMe device part)

Allocated by client:
• nvmeibs_client (per client)
• nvmeibs_net (admin channel)
• Message buffers

Allocated by nordda:
• nvmeibs_nr_channel (per I/O channel)
• nvmeib_iu pool

Allocated by serjio:
• nvmeibs_serjio_disk_private_data (per disk)
• Journal metadata cache (JMDC)
• Journal range tables

Shared/Referenced:
• Work queues (main_wq, ioqm_wq, port wqs)
• Proc entries
• Statistics structures
```

---

## Size Estimates (Typical Values)

```
struct nvmeibs_client:          ~2-4 KB (depends on message buffers)
struct nvmeibs_disk_info:       ~1-2 KB (depends on client count)
struct nvmeibs_net:             ~1 KB + ring buffers
struct nvmeibs_nr_channel:      ~1 KB + IU pool
struct nvmeibs_serjio_pd:       ~100 KB - several MB (JMDC)
struct nvmeibs_ib_port:         ~1 KB
struct nvmeibs_cdb:             ~256 bytes + hash table entries

Per-client memory:              ~10-50 MB (depends on # channels, buffers)
Per-disk memory:                ~100 KB - 10 MB (depends on journal size)
```

These are rough estimates; actual sizes vary based on configuration.

