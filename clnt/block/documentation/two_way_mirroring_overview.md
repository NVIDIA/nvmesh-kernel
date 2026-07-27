# NVMesh Two-Way Mirroring Architecture & Operations

## Architecture Overview

The focus of this document is on the 2-way mirroring functionality in the block layer.
For a full picture of the block layer, please reference [^1].

### Core Components

```text
┌─────────────────────────────────────────────────────────────┐
│                   Client Block Device                       │
│                  (nvmeibc_block_device)                     │
└──────────────────────┬──────────────────────────────────────┘
                       │
           ┌───────────┴───────────┐
           │     Topology          │
           │  (nvmeibc_topology)   │
           └───────────┬───────────┘
                       │
           ┌───────────┴───────────┐
           │    Chunks Array       │
           │ (nvmeibc_chunk)       │
           └───────────┬───────────┘
                       │
           ┌───────────┴───────────────┐
           │    RAID1 Array            │
           │  (nvmeibc_raid1)          │
           │  replicas = 2             │
           └───────────┬───────────────┘
                       │
           ┌───────────┴───────────────┐
           │   Segment Array [0,1]     │
           │ (nvmeibc_disk_segment)    │
           │                           │
           │  [0]: Mirror Segment 0    │
           │  [1]: Mirror Segment 1    │
           │                           │
           │  Owner role rotates       │
           │  per blockset:            │
           │  • Blockset 0 → Seg 0     │
           │  • Blockset 1 → Seg 1     │
           │  • Blockset 2 → Seg 0     │
           │  • ...                    │
           └───────────────────────────┘
                   │           │
              ┌────┴────┐  ┌───┴────┐
              │ Server1 │  │Server2 │
              │  Disk   │  │  Disk  │
              └─────────┘  └────────┘
```

### Vocabulary

We define a set of vocabulary for our discussion. For a full list of terminology, please reference [^2].

| Term | Definition |
|------|------------|
| **ACM** | **Access Control Mode** - Defines read/write permissions for each segment. Set by TOMA server, enforced by clients during I/O. Values: `RW` (read-write), `W` (write-only during recovery), `DEAD` (unavailable), `W_NO_DIRTY` (fully recovered, ready for promotion), `W_IS_DIRTY` (assume completely dirty). See `NVMEIBTC_DS_MODE` enum. |
| **Command** | A disk I/O command (`nvmeibc_disk_command`) representing a single NVMe operation (read/write/trim) to a specific disk segment. Contains disk address, length, data buffers, completion callback, and piggyback metadata. Multiple commands are created per operation to handle different segments and RAID legs. |
| **EDIC** | **Excelero Data Integrity Check** - Optional per-block checksums and metadata stored on disk to detect silent data corruption. When enabled, each block includes integrity metadata for validation during reads. Separate from BINFO which is RAM-only for mirrors. |
| **Journal** | Write-ahead log used in EC (Erasure Coding) to prevent write-hole. Journals temporarily store data+parity before committing to final locations. After successful final write, journal entries are garbage collected. Not used in 2-way mirroring (mirrors use distributed locks instead). |
| **Operation** | High-level I/O request (`struct operation`) representing a single bio from the kernel. Encompasses the full lifecycle from submission to completion, including lock acquisition, command generation, execution across multiple segments, and callback. One operation spawns multiple commands for different RAID legs/segments. |
| **Owner Lock** | Distributed lock residing on a specific segment, providing mutual exclusion across clients. The "owner" segment holds the authoritative lock state. Lock ownership rotates per blockset (not per segment). Implemented via RDMA atomic compare-and-swap operations on remote memory. |
| **PRAID** | **Protection RAID** - A single protection group within a chunk, represented by `nvmeibc_raid1` structure (naming is historical; applies to all RAID types). Contains N segments (replicas for mirrors, data+parity for EC). Multiple PRAIDs form a chunk (e.g., RAID10 = multiple RAID1 PRAIDs). |
| **RAID Leader** | The client designated to coordinate compound recovery operations involving multiple clients. In per-blockset locking, "leader" is actually the owner of a specific blockset's lock (rotates per blockset, not static per segment). Not to be confused with TOMA's RAFT leader. |
| **Stage** | Discrete phase in an I/O operation's lifecycle used for state machine execution and performance profiling. Examples: `E_CMDS_STAGE_READ_PRE_DATA`, `E_CMDS_STAGE_DO_IO_AND_PAR`, `E_CMDS_STAGE_POST_IO_RDMA`. Each stage measures time, counts, and latency distribution. Different RAID types use different stage subsets. |
| **Task** | Recovery work unit managed by TOMA server. Represents a recovery job assigned to a specific client for a disk segment. Has lifecycle states: INIT, WAIT_CLIENT, IN_PROGRESS, FINISHED, CANCELED, ERROR. Multiple tasks can run concurrently across different segments. Task ID uniquely identifies each recovery instance. |
| **TOMA** | **Target Orchestration and Management Agent** - Server-side daemon running on storage targets. Manages topology changes, segment state transitions (ACM values), recovery coordination, disk failure detection, client registration, and cluster leadership (RAFT-based). Part of the NVMesh target software stack. |

### Key Structures

1. **`nvmeibc_raid1`** - Represents a protection group (naming is historical; applies to all RAID types, JBOD, mirrors, and EC)
   - `replicas`: Number of replicas/segments in the protection group (2 for two-way mirror)
   - `segments[]`: Array of disk segments (mirrors for RAID1, data+parity segments for EC)
   - `slice_size`: 1 for RAID1 (vs >1 for EC)
   - `chunk_spec`: chunk specs and its derivations can be found in the [code](https://gitlab-master.nvidia.com/excelero/teams/team-data-services/nvmeshum/-/blob/master/app/test/simulator/management/conf_checks.c?ref_type=heads)
   - **Note:** Despite the name, this structure is used for all protection schemes, not just RAID1

2. **`nvmeibc_disk_segment`** - Represents one leg/mirror
   - `uuid`: Unique segment identifier
   - `first_lba`: Starting LBA on physical disk
   - `toma_acm`: Access Control Mode (RW/R/Dead/etc.)
   - `toma_reg`: Registration with TOMA server

3. **Lock Ownership** - Distributed locking system
   - Locks reside on specific segments
   - Lock ownership determines which segment is "authoritative"

## Key Routines and Their Functions

### **Write Operations**

**`__mirror_cmds_add_for_raid()`**

- **Purpose**: Prepares write commands to be sent to both mirror segments. This can be invoked several times per operation, for different PRAIDs (stripes). `dp_io_topo_iterator` is the helper structure for preparing commands.

- **What it does**:
  - Determines which segments are alive (not dead)
  - Creates separate commands for each writeable mirror
  - All mirrors share the same source data buffer (efficiency optimization)
  - Handles data size limitations based on disk capabilities
  - Ensures writes don't cross 128KB boundaries (performance optimization)
  - **Pre-calculated topology bitmaps (`nvmeibc_roles_bmps`)**: Since the I/O-able topology is stable during operation execution, bitmaps are calculated once at topology update and reused
  - **I/O traits optimization**: For complex operations, traits(`io_traits`) are computed once at operation start and reused
  - **Performance principle**: Calculate once at operation start, reuse throughout execution. Topology stability during an operation guarantees bitmap validity
  - **Debug DI (Data Integrity) injection**: When enabled, injects debugging metadata into each written block

**`__prepare_mirror_binfo_for_write()`**

- **Purpose**: Sets up metadata (dirty bits and transaction info) for the write operation. `binfo` is a RAM-only metadata, as the only block metadata stored on disk for 2-way mirroring is EDIC (if enabled)

- **What it does**:
  - Decides which segments should be marked as "dirty" after the write, or "clean" as the operation overwrites the whole blockset ("implicit_sync") through `dbits`.
  - Prepares write metadata (e.g., dirty bits and version/epoch) to track this specific write. We this field for mirror, and to record `nvmeib_block_io_op` operation. In practice, it can be only a write or a sync op.
  - Attaches this metadata to the write commands as a piggy-back RAM operation, so it gets written alongside the data, and only on those which coincide with the lock locations (same disk)
  - For an online recovery, `dbits` is an optimization for not synchronizing whole segments data after a `D` disk/segment returns as `W` on the way to becoming `RW`

**`dp_mirror_calc_comp_state()`**

- **Purpose**: Checks if all mirror writes succeeded and decides what to do about failures

- **What it does**:
  - Examines the result from each mirror write
  - Classifies errors (transient network issues vs permanent disk failures)
  - Decides whether to retry the operation
  - The lock release (or abandon) decision is in `__release_locks_of_completed_command()` and `calc_should_abandon()`
  - Suspends volume on too many non-transient errors

### **Read Operations**

**`__get_role_of_read()`**

- **Purpose**: Intelligently picks which mirror to read from

- **What it does**:
  - First choice: Read from the "lock owner" segment (authoritative source). Piggybacking a view‑lock on the read preserves a consistent snapshot without acquiring write locks, ensuring correctness with concurrent writers/syncs.
  - Optimization: If a mirror is on the same server as the client, read locally (much faster). This is a very specific use case optimization (for asymmetric mirroring) that is currently not enabled for any customer due to additional overhead
  - Considers topology health - avoids reading from degraded segments
  - Returns which specific mirror should handle this read

**`get_owner_seg_of_read()`**

- **Purpose**: Determines which segment "owns" a particular blockset

- **What it does**:
  - Uses the block address to calculate ownership
  - Ensures consistent read source selection
  - Important for maintaining lock coherency across the cluster

### **Topology Management**

**`__raid1_apply_downgrade()`**

- **Purpose**: Handles when a mirror fails and needs to be removed from service

- **What it does**:
  - Reduces the replica count from 2 to 1
  - Updates lock ownership maps (so remaining mirror becomes sole owner)
  - Removes the failed segment from the active segment array
  - If segment[0] failed, promotes segment[1] to position 0
  - Notifies the system that mirroring level has changed

- **Status**
  - Upgrade/downgrade functionality is currently unused (except for in the simulator). Management does segment replacement without going up to 3-mirror then back to 2-mirror, so redundancy suffers temporarily

**`__raid1_apply_upgrade()`**

- **Purpose**: Adds a new mirror to restore two-way protection

- **What it does**:
  - Increases replica count from 1 to 2
  - Integrates the new segment into the segment array
  - Updates lock ownership to include the new mirror
  - Marks the new segment as needing synchronization
  - Preserves order: new segment goes to specified position based on TOMA instructions

**`__raid1_apply_replacement()`**

- **Purpose**: Swaps out a segment with a replacement (like replacing a bad disk)

- **What it does**:
  - Keeps replica count at 2
  - Substitutes the old segment with the new one in the same array position
  - Updates lock mappings to point to the new segment
  - Cleans up references to the old segment

### **Recovery and Synchronization**

**`__mirror_sync_data_prepare_op()`**

- **Purpose**: Sets up the machinery to copy data from healthy mirror to recovering mirror

- **What it does**:
  - Allocates commands for reads AND writes (one pair per segment)
  - Read commands: Fetch data from all healthy segments
  - Write commands: Will push that data to the segment being synced
  - Links read/write command pairs to reuse memory efficiently
  - Handles metadata if enabled (checksums, version numbers)
  - Marks segments that are "write-only" (dirty, being recovered)

**Recovery Type: `NVMEIB_BLOCK_IO_OP_RECOVER_DB`** (Dirty Bit Recovery)

- **Trigger**: TOMA server requests it, or any I/O detects dirty bits that need clearing

- **Process**:
  1. Read from all readable segments
  2. If we get valid data from at least one source, use it to restore all mirrors
  3. If no valid sources exist, try block-by-block recovery
  4. Write the good data to all writable segments
  5. Clear the dirty bits once writes succeed

**Recovery Type: `NVMEIB_BLOCK_IO_OP_RECOVER_READFAIL`** (Read Failure Recovery)

- **Trigger**: When a read from a segment fails with a permanent disk error

- **Process**:
  1. Attempt to read from the other mirror
  2. If successful, write the good data back to the failed segment
  3. If both mirrors fail to read, mark the block as uncorrectable
  4. Return appropriate error to the application

**Recovery Type: `NVMEIB_BLOCK_IO_OP_RECOVER_STALE`** (Stale Lock Recovery)

- **Trigger**: Discovering a lock that wasn't properly released (node crash scenario)

- **Process**:
  - Mirror always takes data from lock owner segment and synchronizes to the other
  - No transaction IDs are used for RAID1 mirrors; only EC involves transaction IDs. Mirror consistency relies on distributed locks, dirty‑bit metadata, and version/epoch markers
  - Synchronize all mirrors to the most recent version
  - Release the stale lock

### **Error Handling**

**`dp_cmds_rv_failed_ACID()` / `dp_cmds_rv_failed_non_ACID()`**

- **Purpose**: Categorizes failures by severity and consistency implications

- **What they do**:
  - ACID failures: Ones that might have broken consistency (retry required), which might have written or corrupted data on disk
  - Non-ACID failures: Network/transport issues (can retry safely)
  - Used to determine retry strategy

**`is_transient_disk_error()`**

- **Purpose**: Distinguishes temporary errors from permanent failures

- **What it does**:
  - Temporary: Busy, queue full, transient network issues → retry
  - Permanent: Bad sector, hardware failure → don't retry, trigger recovery
  - No degradation based on client side errors. If non-transient, complete the bio to user with error, otherwise retry

### **Optimization Features**

#### Local Read Optimization

- **What it does**:
  - Checks if any mirror is on the same physical server as the client
  - If yes, reads from that local mirror instead of going over the network
  - Dramatically reduces read latency (microseconds instead of network roundtrip)
  - Only enabled when topology is "perfect" (all mirrors healthy and synchronized)
  - Currently disabled by default

#### Shared Buffer for Mirrors

- **What it does**:
  - Instead of copying data twice (once per mirror), uses the same source buffer
  - Both mirror write commands point to the same scatter-gather list
  - Saves memory bandwidth and CPU cycles
  - Works because both writes happen simultaneously

#### Intel Disk Optimization

- **What it does**:
  - Detects Intel NVMe drives by their characteristics
  - Ensures I/Os don't cross 128KB boundaries on these drives
  - Intel drives have latency penalties for boundary-crossing writes
  - Splits larger operations to maintain optimal performance

---

## Simplified Operational Flows

### **Normal Write Flow (Both Mirrors Healthy)**

1. Application writes data
2. System acquires distributed locks on affected blocks
3. Creates two write commands pointing to the same data
4. Sends both writes in parallel via RDMA
5. Updates metadata:
    - If segment ACM state = W_NO_DIRTY (write‑only, clean), clear dbits upon successful mirrored write
    - If segment ACM state = W_IS_DIRTY (write‑only, dirty), piggyback turning on dbits for the recovering leg, then clear after sync completion
6. Releases locks
7. Returns success to application upon all I/O completions

### **Degraded Write Flow (One Mirror Down)**

1. Application writes data
2. System checks topology, sees only one mirror is healthy
3. Creates write command for healthy mirror only
4. Sends write via RDMA
5. Marks the down mirror as "needs sync" in metadata
6. Releases locks
7. Returns success to application (write is durable on one mirror)

### **Read Flow**

1. Application requests data
2. System picks best mirror (local if available, otherwise lock owner)
3. Issues single RDMA read, a "view lock" will be acquired and piggy-backed to client, retries just view lock if contended
4. If read fails, tries the other mirror
5. If both fail, triggers recovery
6. Returns data to application

### **Recovery Flow (Mirror Returns to Service)**

1. TOMA server detects segment is back online
2. Marks segment as "Write-only" (dirty, needs sync)
3. Pushes topology update to all clients
4. Background recovery process starts:
   - Acquires locks for a range of blocks
   - Reads from healthy mirror
   - Writes to recovering mirror
   - Clears dirty bits
   - Releases locks
   - Repeats for next range
5. When sync completes, marks segment as "Read-Write"
6. Full two-way mirroring resumed

---

## Key Design Principles

 **Consistency First**: Uses distributed locks, dirty-bit metadata, and version/epoch markers for RAID1. EC protection schemes use transaction IDs in addition to locks.

**Performance Optimized**: RDMA for low latency, local reads when possible, shared buffers to reduce copying

**Fault Tolerant**: Can operate with one mirror down, automatically recovers when it returns

**No Data Loss**: Writes succeed only when at least one mirror confirms, dirty bits track what needs resync

**Transparent Recovery**: Background synchronization doesn't block normal I/O operations

**Smart Error Handling**: Distinguishes temporary glitches from permanent failures to avoid unnecessary degradation

## Discussion

### NVMesh request (bio) lifecycle

This section describes the complete lifecycle of a block I/O request (bio) in NVMesh, from kernel submission to completion.

#### Overview

When a **bio** (block I/O request) arrives from the Linux kernel, NVMesh transforms it into **operations** - the fundamental unit of I/O processing in the datapath. Operations manage commands, locks, and state transitions asynchronously using a callback-driven architecture.

**Critical Contract**: Until `bio_end_io()` is called, the kernel guarantees the bio memory won't be freed. This contract has no timeout - the bio remains valid until we explicitly complete it.

#### 1. Bio Arrival and Operation Creation

##### Entry Point

- Kernel invokes `make_request()` → `execute_bio()`
- Must be called from thread context (not interrupt) to allow memory allocation

##### Bio Decomposition

```text
bio (from kernel)
  ↓ split into
bio_part (points back to original bio)
  ↓ combined into
operation (fundamental datapath unit)
  ↓ contains
commands[N] + locks[M] + CL_MAT (command-lock matrix)
```

**Key Insight**: The datapath works with operations, not bios directly. A bio can span multiple operations, and bio_parts bridge between them.

#### 2. Operation Preparation (`prepare_op`)

Preparation happens before any I/O execution. All allocations are done upfront since operations are created in non-atomic context.

##### Topology Assignment

Each operation takes a reference to a **topology** - a snapshot of the current I/O routing configuration:

```c
// From topology get (simplified)
operation->topo = topology_get(nnd);  // First entry of topologies list
operation->debug_id = generate_debug_id();  // For tracing
```

**Topology Properties**:

- Immutable during operation execution (guarantee of bitmap validity)
- If operation retries, it releases old topology and takes a new one
- Contains all segment health, lock ownership, and routing information

**debug_id**: Near-unique identifier for tracing

- Format: `per-CPU bits | running counter`
- Wraps approximately every 20 seconds under high load
- Used for correlating all traces related to one operation
- Preserved across retries

##### Resource Calculation

The system calculates exactly how many commands and locks are needed:

**Commands** (disk I/O operations):

```text
JBOD write:  num_blocks × 1 (single leg)
Mirror write: num_blocks × 2 (both mirrors)
Mirror read:  num_blocks × 1 (one mirror only)
Trim/discard: Special calculation (can combine multiple ranges)
```

**Locks** (distributed locking units):

```text
JBOD:         0 (no locking needed)
Mirror read:  1 per blockset (view lock optimization)
Mirror write: 2 per blockset (owner lock + copy lock)
              OR 1 per blockset (when special configs apply)
```

Each blockset = 128KB logical unit that spans multiple segments.

##### Memory Allocation

All structures are allocated upfront in one batch:

```c
// Allocation strategy (simplified)
operation = alloc_from_pool();
commands = placement_new_in_spare_space(operation);  // Same page optimization
locks = alloc_locks(count);

// Each command structure
for each command:
    block_cmd->disk_cmd = alloc_disk_cmd();
    disk_cmd->nvme_db = alloc_nvme_db();  // Contains SGL
```

**Structure Hierarchy**:

- **block_cmd**: Block layer command (high-level)
- **disk_cmd**: Disk layer command (low-level)
- **nvme_db**: Contains scatter-gather list (SGL) for DMA

**Optimization Note**: Only commands[0] links back to operation. Not all command struct fields are used by all commands - this is historical and somewhat confusing but efficient.

##### Buffer Management Decision

Critical decision: Should data buffers be copied or used directly?

**Problem 1 - User Buffer Modification**:

```text
User submits write → buffer content = "A"
  ↓
Write to Mirror 0 starts (DMA reads "A")
  ↓
User modifies buffer → content = "B"
  ↓
Write to Mirror 1 starts (DMA reads "B")
  ↓
Result: Mirrors contain different data!
```

**Problem 2 - Shared Read Buffers**:

```text
User submits multiple concurrent reads to same scratch buffer
  ↓
Read 1 completes, writes data (EDIC checksum = X)
  ↓
Read 2 writes different data (EDIC checksum = Y)
  ↓
EDIC verification fails → false error reported
```

**Solution**: Copy buffers when:

- EDIC (CRC integrity checking) is enabled on the volume
- Controlled by module parameters
- Creates isolated bounce buffers for DMA operations

```c
// Buffer copy decision
if (volume_has_edic || shared_read_buffers_detected) {
    allocate_bounce_buffers();
    for writes: copy_from_user_buffer();
    for reads:  read_to_bounce_then_copy_back();
}
```

**Debug DI (Data Integrity)**: When enabled (NVMESH-4505 solution), debugging metadata is injected into each written block to help with forensics. The buffer copy mechanism ensures DI metadata doesn't corrupt shared user buffers.

##### Pre-reads (Sub-block Writes)

When exposed block size (512B sectors) is smaller than internal block size (4KB):

```text
User wants to write 512 bytes at offset 1024
  ↓
Must preserve other 3.5KB in the 4KB block
  ↓
Allocate pre-read buffers
Read entire 4KB block → Modify 512B → Write back 4KB
```

Pre-read buffer allocation uses efficient kernel page allocation:

- Tries to allocate larger orders first
- Falls back to smaller orders as needed
- Reuses buffers when possible

##### Command-Lock Linking (CL_MAT)

**CL_MAT** (Common Locks Bit Matrix) is a bitmask that links commands to locks:

```text
Bit matrix representation:
         Lock0  Lock1  Lock2  Lock3
Cmd0:     1      1      0      0    <- Needs Lock0 and Lock1
Cmd1:     0      1      1      0    <- Needs Lock1 and Lock2
Cmd2:     0      0      1      1    <- Needs Lock2 and Lock3
```

**Purpose**:

- Commands execute only when ALL their required locks are acquired
- Enables independent progress of different blocksets in the same operation
- Example: I/O spanning 2 blocksets can have commands for blockset 0 executing while blockset 1 is still acquiring locks

##### Stages Assignment

Operations progress through **stages** (sequential execution phases):

**Main stages for mirror**:

- **Pre-reads**: Read-modify-write for sub-block operations
- **Do_IO_n_parties**: Main stage - actual disk I/O operations
- **Post_IO_RDMA**: Metadata updates (dirty bit operations)

**For simple mirror operations**: Usually just 1 stage (Do_IO_n_parties)

**Stage execution principle**: ALL stages execute even on errors (for cleanup purposes).

#### 3. Operation Execution (`execute_op`)

Execution is **asynchronous, non-blocking, and callback-driven**. The `execute_bio()` function returns immediately after launching operations.

##### Execution Model

```text
execute_bio() called
  ↓
Launch commands/locks
  ↓
Return to kernel immediately  ← execute_bio() returns here
  ↓
... (time passes) ...
  ↓
Callbacks invoked as operations complete (possibly on different CPUs/interrupts)
```

**Critical Rule**: NEVER block in kernel code. No waiting for external events (network, disk, etc.).

##### Path 1: Unprotected Commands

**Used for**:

- JBOD reads/writes (no locking required)
- Mirror reads with view-lock optimization

```c
// Simplified flow
if (is_unprotected_command()) {
    for each command in current_stage:
        if (is_jbod_read || is_jbod_write) {
            dp_cmds_execute_cmd(cmd);  // Just send it
        } else if (is_mirror_read) {
            // Piggyback view lock on read command
            cmd->piggyback = create_view_lock_piggyback();
            dp_cmds_execute_cmd(cmd);
        }
}
```

**View Lock Mechanism** (Mirror Reads):

- Instead of acquiring exclusive lock, "view" the lock value
- Server executes: (1) Read data, (2) Read lock value, (3) Return both
- Client checks lock value to ensure no write was in progress

**Why View Lock Works**:

1. Always read from lock owner segment (authoritative source)
2. If lock is held during read, view-lock sees it
3. If write fails mid-flight, lock remains held or becomes stale
4. Stale locks are resolved by copying owner segment data (the data we already read)
5. Therefore: read data is always consistent with eventual lock resolution

This is described in detail in NVMesh's consistency document.

##### Path 2: Protected Commands (Mirror Writes)

**Flow**:

```text
1. Send lock acquisition requests (RDMA atomic CAS)
     ↓
2. Lock callbacks invoked as locks acquired
     ↓
3. Check lock state (may trigger sync/stale_sync)
     ↓
4. When all locks for a blockset acquired:
     - Consult CL_MAT to find ready commands
     - Execute first stage commands
     ↓
5. Command completion callbacks invoked
     - Decrement stage completion counter
     ↓
6. When stage completes:
     - execute_next_stage()
     ↓
7. When all stages complete:
     - Release locks (asynchronous, doesn't block completion)
     - Complete operation
```

**Lock Acquisition**:

```c
// Locks are sent first
for each lock in operation:
    icore_ops->cmpxchg(icore_ops, disk, handle, lock_address, compare_swap_data);
    // RDMA atomic compare-and-swap operation
```

**Lock Callback Processing** (`lock_response_callback`):

```c
void lock_response_callback(lock, result) {
    // Complex state machine
    if (lock_acquired_successfully) {
        mark_blockset_locked();
        check_if_all_locks_for_blockset_ready();
        if (ready) {
            // Consult CL_MAT to find commands that can now execute
            execute_first_stage_for_blockset();
        }
    } else if (lock_contended) {
        retry_lock_acquisition();
    } else if (stale_lock_detected) {
        initiate_stale_sync();
        // Will come back here after sync completes
    }
}
```

**Command Execution**:

```c
void execute_stage_commands(operation, stage) {
    for each command in stage:
        if (operation->rv != 0) {
            skip_but_call_for_cleanup();  // Error path
        } else {
            dp_cmds_execute_cmd(command);  // Actual I/O
        }
    // Completion tracking happens via callbacks
}
```

**Command Completion** (`block_command_callback`):

```c
void block_command_callback(command, result) {
    operation->rv = check_and_combine_errors(result);

    if (--operation->stage_completion_counter == 0) {
        // All commands in this stage done
        execute_next_stage(operation);
    }
}
```

**Stage Progression**:

```c
void execute_next_stage(operation) {
    operation->current_stage++;

    if (operation->current_stage > LAST_STAGE) {
        complete_operation(operation);
    } else {
        execute_stage_commands(operation, current_stage);
    }
}
```

#### 4. Concurrency and Threading Model

##### Asynchronous, Multi-CPU Architecture

```text
Bio arrives on CPU 0 (thread context)
  ↓
execute_bio() creates operation (CPU 0)
  ↓
Launches locks/commands (CPU 0)
  ↓
Returns immediately (CPU 0)
  ↓
... network/disk activity ...
  ↓
Lock callback invoked (CPU 2, interrupt context)
  ↓
Commands launched (CPU 2)
  ↓
... disk I/O ...
  ↓
Command callback invoked (CPU 5, interrupt context)
  ↓
Operation completed (CPU 5)
  ↓
bio_end_io() called (CPU 5)
```

**Key Properties**:

- No blocking, ever
- Callbacks can run on any CPU
- Callbacks can run in interrupt context
- Operation state is protected by atomic counters and careful ordering
- No locks held across asynchronous boundaries

##### Independent Blockset Progression

For operations spanning multiple blocksets:

```text
Operation for LBA range spanning 3 blocksets
  ↓
Locks requested for all 3 blocksets simultaneously
  ↓
Blockset 0 locks acquired first
  ↓
Commands for blockset 0 execute (other blocksets still locking)
  ↓
Blockset 2 locks acquired next
  ↓
Commands for blockset 2 execute (blockset 1 still locking)
  ↓
Blockset 1 locks acquired last
  ↓
Commands for blockset 1 execute
  ↓
All complete → operation completes
```

This is enabled by the CL_MAT (command-lock matrix) design.

#### 5. Operation Completion

When the last stage completes for all commands:

```c
void complete_operation(operation) {
    // 1. Handle read buffer copies if needed
    if (operation->used_bounce_buffers && is_read) {
        copy_data_to_user_buffers(operation);
    }

    // 2. Check for resubmit conditions
    if (operation->rv != 0 && is_transient_error(operation->rv)) {
        if (topology_changed || io_disabled) {
            queue_for_resubmit(operation);
            return;  // Don't complete bio yet
        }
    }

    // 3. Release locks asynchronously
    for each lock in operation:
        send_lock_release(lock);  // Don't wait for completion

    // 4. Complete bio parts
    complete_bio_parts(operation);

    // 5. Trace completion
    gdbg_trace(OPERATION_COMPLETE, operation->debug_id, operation->rv);

    // 6. Free operation
    put_operation(operation);
}
```

##### Bio Part Completion

```c
void complete_bio_parts(operation) {
    for each bio_part in operation->bio_parts:
        if (--bio_part->operation_counter == 0) {
            // All operations for this bio_part complete
            complete_bio_part(bio_part);
        }
}

void complete_bio_part(bio_part) {
    bio* original_bio = bio_part->bio;

    if (--original_bio->part_counter == 0) {
        // All parts of the original bio complete
        bio_end_io(original_bio, operation_result);  ← Kernel contract fulfilled
    }
}
```

**Tracing**: Operation completion is logged with `debug_id` for correlation with all prior traces.

#### 6. Retry and Error Handling

##### Transient Errors → Resubmit

```c
if (topology_phased_out || io_became_disabled) {
    // Don't complete bio yet
    topology_put(operation->topology);  // Release old
    operation->topology = NULL;

    list_add(operation, &resubmit_queue);
    schedule_resubmit_work();

    // When resubmit worker runs:
    operation->topology = topology_get();  // Get fresh topology
    execute_operation(operation);  // Try again

    // debug_id preserved for tracing continuity
}
```

##### Sync Operations (Complex Flows)

Syncs can be triggered mid-operation:

```text
Operation executing
  ↓
Lock callback detects stale lock
  ↓
Initiate stale sync operation
  ↓
Sync reads owner segment → writes to copy segment
  ↓
Sync completes
  ↓
Return to original operation flow
  ↓
Continue with normal execution
```

**Note**: Syncs are more complex than normal I/O but already have extensive tracing infrastructure.

##### Error Handling Functions

**`dp_mirror_calc_comp_state()`**: Analyzes command results

- Classifies errors by type and severity
- Determines if operation succeeded despite partial failures
- Decides on retry vs. completion vs. degradation

**`is_transient_disk_error()`**: Categorizes disk errors

- Transient: Queue full, busy, network timeout → retry
- Permanent: Bad sector, hardware failure → don't retry, may trigger recovery

**`dp_cmds_rv_failed_ACID()` / `dp_cmds_rv_failed_non_ACID()`**:

- ACID: Might have corrupted disk state → careful recovery needed
- Non-ACID: Transport/network only → safe to retry

#### 7. Stage Execution Details

##### Stage Types (for Mirror)

```c
enum dp_cmd_stage {
    DP_CMD_STAGE_PRE_READ,      // Read-modify-write for sub-blocks
    DP_CMD_STAGE_DO_IO_N_PARTIES, // Main I/O stage (used by mirror)
    DP_CMD_STAGE_POST_IO_RDMA,  // Metadata updates (sometimes used)
    // ... other stages for EC ...
};
```

##### Stage Execution Loop

```c
void send_commands_of_stage(operation, stage) {
    int prev_rv = operation->rv;

    for each command in commands_array:
        if (command->stage == stage) {
            if (prev_rv != 0) {
                // Execute anyway for cleanup, but mark as skipped
                command->skip = true;
            }

            rv = dp_cmds_execute_cmd(command);
            if (rv != 0) {
                operation->rv = rv;  // Propagate error
            }
        }

    // Completion tracking via callbacks
}
```

**Design Principle**: All stages execute even on error. This ensures:

- Proper cleanup of allocated resources
- Locks are released even if some commands failed
- Consistent state machine progression

#### 8. Tracing and Debugging

##### Debug ID Usage

Every trace related to an operation includes its `debug_id`:

```c
// Operation start
gdbg_trace(OPERATION_START, op->debug_id, "vlba=%llu len=%u", vlba, length);

// Lock acquisition
gdbg_trace(LOCK_ACQUIRE, op->debug_id, "blockset=%u state=%s", bs, state);

// Command execution
gdbg_trace(CMD_EXECUTE, op->debug_id, "cmd=%p dlba=%llu", cmd, dlba);

// Stage completion
gdbg_trace(STAGE_DONE, op->debug_id, "stage=%d rv=%d", stage, rv);

// Operation completion
gdbg_trace(OPERATION_COMPLETE, op->debug_id, "rv=%d duration=%llu", rv, delta);
```

##### Sticky Trace Queries

The tracing system supports "sticky" queries:

```text
1. Find OPERATION_START with vlba=1000
2. Extract debug_id from that trace
3. Show ALL traces with that debug_id until OPERATION_COMPLETE
```

This allows following a single I/O through its entire lifecycle across CPUs, retries, and sync operations.

##### Retry Tracing

When an operation retries:

- Same `debug_id` is used
- Multiple OPERATION_START / OPERATION_COMPLETE pairs appear
- Allows tracking retry attempts and eventual success/failure

#### 9. Key Data Structures Summary

```c
// High-level bio decomposition
struct bio {                        // Kernel structure
    // ... kernel fields ...
};

struct bio_part {
    struct bio *bio;                // Points back to original
    atomic_t operation_counter;     // How many operations reference this
    // ... other fields ...
};

struct operation {
    struct topology *topo;          // Immutable during execution
    u64 debug_id;                   // For tracing

    struct bio_part *bio_parts;     // List of bio parts

    struct block_cmd *commands;     // Array of commands
    int num_commands;

    struct lock *locks;             // Array of locks
    int num_locks;

    u64 cl_mat;                     // Command-lock bit matrix

    enum dp_cmd_stage current_stage;
    atomic_t stage_completion_counter;

    int rv;                         // Accumulated return value

    // ... many other fields ...
};

struct block_cmd {
    struct operation *operation;    // Only set for commands[0]
    struct disk_cmd *disk_cmd;      // Lower layer command
    enum dp_cmd_stage stage;        // Which stage this command executes in
    // ... other fields ...
};

struct disk_cmd {
    struct nvme_db *nvme_db;        // Contains SGL for DMA
    u64 disk_address;               // DLBA (disk LBA)
    u32 length;                     // In blocks
    // ... other fields ...
};

struct lock {
    u64 address;                    // Remote memory address for RDMA CAS
    u64 compare_value;
    u64 swap_value;
    void (*callback)(struct lock*, int result);
    // ... other fields ...
};
```

#### 10. Lifecycle Summary Diagram

```text
[Kernel bio arrives]
        ↓
[Create operation + allocate commands/locks]
        ↓
[Take topology reference] (operation->topo = topology_get())
        ↓
[Link commands ↔ locks via CL_MAT]
        ↓
┌───────────────────────────────┐
│   Start lock acquisition      │ ← RDMA atomic CAS operations
│   (asynchronous, parallel)    │
└───────────┬───────────────────┘
            ↓
     [Lock callbacks invoked]
            ↓
     [Check lock state] ──→ [May trigger sync] ──→ [Return to flow]
            ↓
     [Consult CL_MAT: which commands ready?]
            ↓
┌───────────────────────────────┐
│   Execute stage commands      │ ← Actual disk I/O
│   (asynchronous, parallel)    │
└───────────┬───────────────────┘
            ↓
     [Command callbacks invoked]
            ↓
     [Decrement stage counter]
            ↓
     [Stage complete?] ──No──→ [Wait for more callbacks]
            ↓ Yes
     [Execute next stage] ──→ [Loop if more stages]
            ↓
     [All stages complete]
            ↓
┌───────────────────────────────┐
│  Release locks (async)        │
│  Complete bio parts           │
│  Check resubmit conditions    │
│  Free operation               │
└───────────┬───────────────────┘
            ↓
     [bio_end_io()] ← Kernel contract fulfilled
```

#### 11. Important Principles

**Non-Blocking Architecture**: No blocking, ever. All external dependencies handled via callbacks.

**Topology Immutability**: Once an operation takes a topology reference, it doesn't change during execution. This guarantees validity of pre-calculated bitmaps and traits.

**Independent Progress**: Different blocksets in the same operation can be at different stages simultaneously (enabled by CL_MAT).

**Error Resilience**: All stages execute even on errors (for cleanup). Transient errors trigger resubmit with fresh topology.

**Memory Safety**: Bio memory is guaranteed valid until `bio_end_io()` is called. Operation memory is reference-counted.

**Tracing Continuity**: `debug_id` preserved across retries allows full I/O lifecycle tracking.

**Performance Optimization**:

- Same-page allocation for operation + commands
- Shared buffers for mirror writes (when safe)
- View-lock optimization for mirror reads
- Pre-calculated topology bitmaps (calculated once, reused throughout operation)
- I/O traits computed once at operation start

**Lock Management**: Locks released asynchronously after operation completes (don't block bio completion).

#### 12. Three‑Way Mirroring (Design readiness)

Note: Not GA/productized at the time of writing; enablement is gated behind management, recovery, and testing work.

The architecture is designed to support N‑way mirroring:

```c
// Topology already provides:
raid1->replicas;  // Can be 2, 3, or more
raid1->segments[];  // Array size determined by replicas

// Command allocation already scales:
num_commands = num_blocks × raid1->replicas;

// Lock ownership already rotates across segments:
owner_segment_idx = blockset_num % raid1->replicas;
```

No fundamental changes to prepare/execute logic needed for 3-way - the code is already generalized.

### References

[^1]: Block [book](./clients_block_device_overview.md).
[^2]: NVMesh [README](../../../README.md).
