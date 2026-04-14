# Mirror (RAID-1) Write Flow

This document describes the mirror write path in the NVMesh block client.
Concepts are introduced before they are used, and data
structures are diagrammed before the algorithms that manipulate them.


Code references are relative to `clnt/block/` unless otherwise noted.

---

## 1. Basic Terminology

Before diving into the flow, it helps to pin down a few terms that appear
throughout the mirror datapath.

| Term | Meaning in this document |
| ---- | ------------------------ |
| **Block** | The basic data unit: one 4KB block (`NVMEIBC_SECTOR_SIZE`). Mirror reads and writes blocks, while locks usually protect larger groups of blocks. |
| **Block metadata (block MD)** | Optional metadata stored alongside each 4KB block on disk, typically 8 bytes on `4096+8` media. In mirror it is used mainly for EDIC, not for journal replay or parity reconstruction. For an already existing mirrored volume, EDIC calculation/checking can also be disabled later at the datapath level even if the disks were originally formatted with metadata. |
| **Dirty bits (dbits)** | A compact encoding of which mirror legs may be out of sync for a given blockset. The name is historical: in N-way mirror this field can also encode states such as unknown/convict, not just a literal 1-bit-per-segment bitmap. The current mirror/dbits encoding can represent only limited degradedness: practically, up to two degraded or unknown legs are tracked explicitly. |
| **Blockset** | A 128KB-aligned group of 32 consecutive 4KB blocks (`LOCKSET_SLICES = 32`). A blockset is the granularity protected by locks. In the generic scheme the protected lock set is discussed as `parities + 1` owner slots, but mirror does not automatically allocate one independent lock per replica for arbitrary N-way layouts: the actual number of locks used for one blockset is bounded by the lock scheme's `max_n_owners`. Mirror lock ownership, stale state, and dbits are all tracked per blockset. |
| **Blockset info** | The upper 32 bits of the in-memory lock entry. Structurally it contains `dirty` and `txid`. In mirror, `dirty` is the real payload. The `txid` field is not used as a monotonic transaction ID the way EC uses it; instead, mirror abuses it as a debug tag. Normal writes typically store `NVMEIB_BLOCK_IO_OP_WRITE`, and sync/recovery paths store the relevant recovery opcode, so tests and debugging can tell which path last updated the RAM binfo. |
| **Lock owner / copy-owner** | The owner lock is the authoritative lock for a blockset: clients serialize protected IO through it and use it to decide whether stale recovery is needed. Copy-owner locks are sibling lock entries on additional mirror legs that mirror the protected state for crash recovery and stale-state propagation. |
| **Lock ID** | The lower 32 bits of the lock entry. It records lock ownership and carries control bits such as `stale` and `read`. The lock ID value itself is allocated by TOMA during client/TOMA negotiation. |
| **Lock entry** | The full 64-bit RAM word associated with one blockset on one target. It is the combination of `lock_id` (lower 32 bits) and `blockset_info` (upper 32 bits). |

Two similarly named objects are easy to confuse:

- **Block metadata** is **per 4KB block** and lives with the data on disk.
- **Blockset info** is **per 128KB blockset** and lives in RAM inside the lock entry.

---

## 2. Physical Layout

### 2.1 Segments and Blocksets

A mirror volume replicates data across _N_ segments. Most examples below use
2-way and 3-way mirror because they are the officially supported cases, but the datapath and
layout code handle wider N-way RAID mirror as well. Ideally, each segment
resides on a different target disk, accessed via RDMA. Every write goes to all
live segments; reads go to one.

```
  2-way mirror layout:

  Segment 0 (Server A)          Segment 1 (Server B)
  +---------------------------+ +---------------------------+
  | Blockset 0  [blk 0..31]   | | Blockset 0  [blk 0..31]   |   <- Owner: Seg 0
  +---------------------------+ +---------------------------+
  | Blockset 1  [blk 0..31]   | | Blockset 1  [blk 0..31]   |   <- Owner: Seg 1
  +---------------------------+ +---------------------------+
  | Blockset 2  [blk 0..31]   | | Blockset 2  [blk 0..31]   |   <- Owner: Seg 0
  +---------------------------+ +---------------------------+
  | Blockset 3  [blk 0..31]   | | Blockset 3  [blk 0..31]   |   <- Owner: Seg 1
  +---------------------------+ +---------------------------+
  |           ...             | |           ...             |
  +---------------------------+ +---------------------------+

  3-way mirror layout:

  Segment 0 (Server A)          Segment 1 (Server B)          Segment 2 (Server C)
  +---------------------------+ +---------------------------+ +---------------------------+
  | Blockset 0  [blk 0..31]   | | Blockset 0  [blk 0..31]   | | Blockset 0  [blk 0..31]   |   <- Owner: Seg 0
  +---------------------------+ +---------------------------+ +---------------------------+
  | Blockset 1  [blk 0..31]   | | Blockset 1  [blk 0..31]   | | Blockset 1  [blk 0..31]   |   <- Owner: Seg 1
  +---------------------------+ +---------------------------+ +---------------------------+
  | Blockset 2  [blk 0..31]   | | Blockset 2  [blk 0..31]   | | Blockset 2  [blk 0..31]   |   <- Owner: Seg 2
  +---------------------------+ +---------------------------+ +---------------------------+
  | Blockset 3  [blk 0..31]   | | Blockset 3  [blk 0..31]   | | Blockset 3  [blk 0..31]   |   <- Owner: Seg 0
  +---------------------------+ +---------------------------+ +---------------------------+
  |           ...             | |           ...             | |           ...             |
  +---------------------------+ +---------------------------+ +---------------------------+

  Each blockset = 128KB = 32 x 4KB blocks (LOCKSET_SLICES = 32)
  Owner rotates: owner_seg = blockset_num % replicas
```

Every segment has a in-memory state - lock entry, 8 bytes allocated for every blockset.

---

## 3. Key Data Structures

### 3.1 Lock Entry (64 bits)

Lock entry and locking logic/implementation is shared between the mirror and EC data paths.

```
  union nvmeib_lock_blkset_entry (64 bits) — common/nvmeib_shared.h

  63           52 51       32 31 30 29 28 27                4 3          0
  +-------------+-----------+-----+--+--+--------------------+------------+
  | dirty (12b) | txid (20b)|  rv |rd|st|       lock_id (24b)| idx_praid  |
  +-------------+-----------+-----+--+--+--------------------+------------+
  |<--- blkset_info (32b) ->|<------------ lock_id (32b) ---------------->|

  Mirror-specific usage:
    dirty  [63:52]  Active — tracks degraded segments
    txid   [51:32]  abused by the mirror data path  
    st     [28]     Stale bit — triggers recovery when detected
    rd     [29]     Read-lock flag for view locks
```

### 3.2 Block Metadata (64 bits, Optional)

Mirror uses the **parity (P) variant** of the shared block MD structure
****(`datapath_utils_generic/nvmeibc_block_dp_block_md.h`). All mirror legs are
treated as parity (`NVMEIBC_DATA_MD_MIRROR_IS_PARITY`),
because the P-variant has fewer EDIC bits but includes dbit descriptor fields
(unused by mirror but structurally required).

```
  union nvmeibc_block_dp_ec_data_block_md — P variant (mirror)
  datapath_utils_generic/nvmeibc_block_dp_block_md.h

  63  62 61      52 51        32 31  28 27  24 23     11 10       2 1  0
  +----+----------+-----------+------+------+---------+----------+----+
  |rsv | jri (10) | tx_id(20) |db1(4)|db0(4)|edic(13) |d2j_rng(9)|ver |
  |(2) |          |           |      |      |         |          |(2) |
  +----+----------+-----------+------+------+---------+----------+----+

  Mirror-specific fill — nvmeibc_block_dp_ec_md_make_r1():
    ver      = 1 (NVMEIBC_DATA_MD_VERSION)
    d2j_rng  = 0 (no journal)
    edic     = CRC32c truncated to 13 bits (when EDIC enabled)
    db0, db1 = 0 (mirror does not write dbit descriptors to disk MD)
    tx_id    = 6 (NVMEIBC_MIRROR_UNUSED_TXID_JRI — sentinel, never used)
    jri      = 6 (same sentinel — no journal range)
    rsv      = 0

```

Block MD is OPTIONAL for mirror. It is only present when the disk is
formatted with 4096+8 metadata AND EDIC checking is enabled on the volume.
Without EDIC, mirror writes no per-block metadata at all.

## 4. Locking

### 4.1 Owner and Copy-Owner locks

- **Owner lock**: the main lock for the blockset. Writers must take this lock
  before issuing data writes. It provides mutual exclusion and defines which
  segment is the authoritative owner for that blockset.
- **Copy-owner lock**: an additional copy of the lock state written to the
  other mirror legs during a write. It does not decide ownership; its purpose
  is crash recovery. If the owner side disappears mid-write, the copied lock
  state tells recovery that this blockset was in flight.

### 4.2 Lock Acquisition

Lock acquisition proceeds in three conceptual phases: **optimize**, **acquire
owner**, and **fan-out copies**.

**1. Optimize — lock transfer.** Before touching the network, the client
checks whether a previous IO to the same blockset is about to release its
lock. If so, the lock is handed off locally (a "transfer") and no RDMA
round-trip is needed. This is a fast-path optimization for sequential or
repeated access to the same blockset.

**2. Acquire the owner lock.** The owner lock is always acquired first, using
an RDMA atomic compare-and-swap (CAS): "if the lock word equals UNLOCKED,
write my lock-ID." Three outcomes are possible:

- **Taken** — the lock was free and is now ours. Proceed to step 3.
- **Contended** — another node or IO holds the lock. The CAS returns the
  current holder's ID, which the client classifies:
  - *Live contention*: another client is actively using the blockset. Retry
    with quadratic backoff. If retries exceed a threshold, ask TOMA to break
    the contending lock (the holder may have died without releasing).
  - *Stale lock*: the holder is no longer active (crashed or disconnected).
    The stale-lock resolver decides whether it is safe to take the lock
    immediately or whether recovery must complete first.
  - *Stale read lock*: a special case — a stale read lock cannot have
    corrupted data, so it is safe to retry immediately.
  - If the topology is phased out or the operation has expired while waiting,
    the lock attempt is abandoned.
- **Transport failure** — the disk hosting the lock is unreachable. The lock
  (and all its siblings) are marked dead; the operation will skip this
  blockset.

**3. Fan-out copy-owner locks.** Once the owner lock is taken, all
copy-owner locks are issued **in parallel** to the remaining mirror legs.
Each copy-owner lock is acquired using the same CAS mechanism as the owner
and goes through the same contention and stale-lock resolution logic
(backoff, TOMA help, stale-lock resolver). These sibling lock entries
exist so crash recovery can detect in-flight IO operations, even if 
the owner side disappears.

**Read path short-circuit.** Reads use piggybacked view-locks: the lock value
is returned alongside the read data, so no separate lock RDMA is needed. If
the piggybacked value shows the blockset is unlocked, the read completes
immediately. If the lock is held (contended), the read retries with backoff
— just like write-path contention. If the lock is stale, a sync operation is
triggered to restore data consistency before the read can complete; until the
sync finishes, the read waits. If retries expire or the topology is phased
out, the read fails with a retryable error.

**Completion.** When all locks for a blockset (owner + copies) have resolved,
the operation transitions to command execution. If any sibling failed, the
entire blockset is failed (`-ENXIO` for retryable, `-ENOEXEC` for fatal),
and all commands for that blockset are skipped.

### 4.3 Lock Release

Lock release is asynchronous and runs **after bio completion** — the
application has already received its IO result.

**Release order.** Copy-owner locks are released first, the owner lock last.
This ensures mutual exclusion is held until all copies are cleaned up.

**Four possible outcomes** for each lock at release time:

1. **Abandon.** If the write partially failed (some commands succeeded, some
   did not), the lock is deliberately _not_ released. It stays on the server
   with the client's lock-ID and implicitly becomes stale. The next accessor
   will encounter it and trigger stale-lock recovery (`RECOVER_STALE`). This
   is the safe choice when the disk state is uncertain — it forces a sync
   before anyone reads potentially corrupted data.

2. **Transfer.** If another IO on the same client is already waiting for this
   blockset, the lock is handed off in RAM with no RDMA round-trip. This
   avoids a release-then-reacquire cycle and is the fast path for sequential
   or repeated writes to the same blockset.

3. **Normal release.** The lock is cleared via RDMA CAS (compare our lock-ID,
   write UNLOCKED). On success the blockset is free for any client. If the
   CAS fails or the transport reports an error, the lock remains on the
   server and becomes effectively stale — same outcome as an explicit
   abandon.

4. **Stale-special release.** If the IO held a stale lock and did not fully
   sync the blockset, the lock is released to a special sentinel value
   instead of UNLOCKED. This tells the next accessor that a sync is still
   needed.

**Cleanup.** Once all locks in a lockset (owner + copies) have completed
release, the topology reference is dropped and the lockset memory is freed.

### 4.4 Owner Rotation

Lock ownership alternates per blockset to distribute load:

```
  owner_segment = blockset_number % replicas

  Blockset 0 → Seg 0 owns lock, Seg 1 has active lock
  Blockset 1 → Seg 1 owns lock, Seg 0 has active lock
  Blockset 2 → Seg 0 owns lock, Seg 1 has active lock
  ...
```

This means, in 2-way mirror, reads to even blocksets go to Seg 0, reads to odd blocksets go to Seg 1 (load balanced).

---

## 5. The Write Flow

### 5.1 High level description

Mirror uses up to **three execution stages**. Under perfect topology,
write uses just **one stage**. Sub-block read-modify-write adds a pre-read stage,
and a successful full-blockset write in degraded topology may append a post-IO
binfo update stage.

**Preparation.** Allocate commands, fill locks, compute the execution plan.
At this point the number of commands per blockset equals the number of live
replicas (e.g. 2 for a healthy 2-way, 2 for a 3-way with one dead segment).
All write commands within a blockset share the same source data buffer.

**Lock acquisition.** Acquire owner + copy-owner locks (see section 4.2).
Once locks are taken the client reads the current blockset-info (TxID +
dirty bits) from the lock values.

**Blockset info (binfo) analysis (after locks taken, before IO).** The client resolves
unknown dirty bits to worst-case values and computes which dirty bits need
to change based on the topology:

- **Turn-on mask = dead segments.** If a segment is dead, its dirty bit must
  be set so recovery knows this blockset is out of sync on that leg. The
  updated binfo is piggybacked on the data write commands — no extra stage.
- **Turn-off mask = write-only (W) segments.** If a recovering segment
  receives a successful full-blockset write, its dirty bit can be cleared,
  since the write brings it in sync. This requires a **separate stage**
  after the data write (see POST_IO_RDMA below).

In a healthy topology neither mask is set and no dirty-bit work is needed.

In a 2-way degraded topology (e.g. RW, W) only the turn-off mask is active:
there are no dead segments to turn on bits for, but the recovering (W)
segment's dirty bit can be cleared after a successful full-blockset write.

Conversely, in a 2-way topology with one dead leg (RW, DEAD) only the
turn-on mask is active: the dead segment's dirty bit is piggybacked on the
write, but there is no W segment to turn off.

In a 3-way degraded topology (e.g. RW, W, DEAD) both masks may be active
simultaneously: dirty bits are turned on for the dead leg during the write
and turned off for the recovering leg after the write succeeds.

**Stages:**

1. **READ_PRE_DATA** — Rare. Only needed for sub-block writes (e.g. 512B).
   Reads the full 4KB block from disk, merges the partial write into it, and
   recomputes EDIC metadata if enabled.

2. **DO_IO_AND_PAR** — Always present. Sends data writes to all live
   replicas in parallel. If there are dead segments in the topology, the
   dirty-bit turn-on is piggybacked on these write commands (no extra RDMA).

3. **POST_IO_RDMA** — Conditional. Added dynamically after DO_IO_AND_PAR
   completes, only when all three conditions are met: the topology has
   write-only (recovering) segments, the write covers a full blockset, and
   the write succeeded. Sends a separate RDMA to each replica to write the
   updated binfo with the dirty bits cleared for the recovering segments.

### 5.2 Step-by-Step Description

#### Step 1: Prepare Commands

`dp_mirror_prepare_op()` (`datapath_mirror/nvmeibc_block_dp_mirror.c`)

1. Validate layout (LBA range fits topology).
2. Calculate allocation counts:
   - **Commands**: `max_blocksets x replicas` worth of command slots for writes
     (for example, up to 2 per blockset in 2-way mirror). 1 per blockset for
     reads.
   - **Locks**: up to `min(replicas, max_n_owners)` per blockset for protected
     mirror writes. For view-lock-safe reads, 1 owner lock is enough because
     the datapath sends the view-lock instruction to the owner only. 0 for
     JBOD.
3. Allocate commands, locks, and SGLs (co-allocated in one slab when possible).
4. Decide whether to copy user buffers (required when EDIC is enabled to prevent
   mirror divergence if the user modifies the buffer between the two writes).
5. Fill locks via `dp_fill_locks_for_io()`
   (`datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c`).
6. Build write commands via `__mirror_cmds_add_for_raid()`:
   - One command per live replica (dead segments excluded).
   - **Shared buffer**: both commands point to the same user data pages. The
     second command reuses the first command's bio vector iterator. No data
     copy occurs.
7. Link commands to locks via CL_MAT (Command-Lock Matrix).

Dirty-bit analysis is **not** finalized in prepare time. The current mirror
datapath first takes the locks, merges the blockset info visible through those
locks, and only then decides what binfo update should accompany the write.

```
  Shared buffer optimization:

  User data pages        Cmd 0 (Seg 0)         Cmd 1 (Seg 1)
  +-----------+          +----------+          +----------+
  | page 0    |<---------| SGL[0]   |    ,-----| SGL[0]   |
  | page 1    |<---------| SGL[1]   |<--'  ,---| SGL[1]   |
  | page 2    |<---------| SGL[2]   |<----'    | SGL[2]   |---->same pages
  +-----------+          +----------+          +----------+

  Both SGLs reference the same physical pages. Only the SGL metadata
  structures are separate (required because sg_dma_address() writes
  into the scatterlist during DMA mapping).
```

#### Step 2: Acquire Locks

See section 4 (Locking) for the full acquisition, contention, and release
algorithms.

`dp_mirror_execute_op()` (`datapath_mirror/nvmeibc_block_dp_mirror.c`) →
`dp_locks_send_all()` (`datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c`)

Once the locks for a blockset are resolved, the mirror path analyzes the
merged binfo, resolves unknown dbits conservatively, and programs any binfo
piggyback needed for the write stage.

#### Step 3: Pre-Read (Rare)

`E_CMDS_STAGE_READ_PRE_DATA` (stage 0) — only for sub-block RMW

When the volume exposes 512B sectors but internal blocks are 4KB, a write
smaller than 4KB requires a read-modify-write cycle:

```
  Sub-block write at offset 1024, length 512B:

  +----+----+----+----+----+----+----+----+   4KB block
  |    |    | WR |    |    |    |    |    |   (8 x 512B sectors)
  +----+----+----+----+----+----+----+----+
                ^
                |
  1. Pre-read entire 4KB block from owner segment
  2. Merge 512B write into the pre-read buffer
  3. Recompute EDIC over the full 4KB
  4. Write merged 4KB to all replicas
```

If the write spans multiple 4KB blocks but is not aligned at both ends, up
to two pre-reads are issued: one for the first partial block and one for the
last partial block. Fully covered blocks in the middle need no pre-read.

For normal 4KB-aligned writes, this stage is skipped entirely.

#### Step 4: Write to All Replicas

`E_CMDS_STAGE_DO_IO_AND_PAR` (stage 4)

All write commands fire in parallel. Each replica receives an identical
copy of the data (shared buffer). A single command may carry **multiple
4KB blocks** — up to a full blockset (128KB / 32 blocks), bounded by the
segment's DMA size limit. Each command carries:

- Data payload (up to blockset size, via scatter-gather list)
- 8-byte block MD per block if EDIC is enabled
- Optional dbit piggyback (via RDMA Write to lock entry on same server)

```
  Parallel mirror write (e.g. 32KB = 8 blocks):

  Client                    Server A               Server B
    |                           |                       |
    |-- Write Request: 8 blks ->|                       |
    |   + MD (if EDIC)          |                       |
    |   + dbit piggyback        |                       |
    |                           |--- NVMe Write         |
    |                           |    to disk A          |
    |                           |                       |
    |-- Write Request: 8 blks   ----------------------->|
    |   + MD (if EDIC)          |                       |
    |   + dbit piggyback        |                       |
    |                           |                       |--- NVMe Write
    |                           |                       |    to disk B
    |                           |                       |
    |<---- completion ----------|                       |
    |<---- completion ----------------------------------|
```

#### Step 5: Dirty-Bit Piggyback

`dp_cmds_piggyback_info_on_data_write()`
(`datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c`) — attached
after locks are taken, during binfo analysis

Mirror writes **blockset info** alongside the data write when it needs to turn
dbits on for dead legs. In other words, the piggyback carries the updated RAM
binfo for that blockset, not just a standalone dirty-bit bitmap.

#### Step 6: Complete BIO

`dp_mirror_calc_comp_state()` (`datapath_mirror/nvmeibc_block_dp_mirror.c`)
→ `nvmeibc_operation_complete()` (`datapath_utils_generic/nvmeibc_block_dp_operation.c`)
→ `bio_endio()`

The completion callback examines each command's result:

- If any sibling reports a transport or transient NVMe error, the operation is
  retried even if some other replica already completed successfully.
- Only when no retry-worthy error remains does the write complete as success.
- Permanent errors (bad sector, HW failure, internal fatal path) are
  propagated to the caller.

#### Step 7: Release Locks (post-BIO)

See section 4 (Locking) for the full acquisition, contention, and release
algorithms.

---

## 6. Read Flow

The mirror read path has two distinct modes depending on topology state.

### 6.1 Perfect Topology (all segments RW)

In a perfect topology, reads benefit from two optimizations:

**1. View-lock instead of exclusive lock.** Instead of acquiring the owner
lock via RDMA CAS, the read uses a **view-lock** — a read-only peek at the
lock value, piggybacked on the data read command. No separate lock
round-trip is needed. The client checks the returned lock value:

```
  View lock read:

  Client                    Server (Owner Segment)
    |                           |
    |-- Read Request: data ---->|
    |   + piggyback: view lock  |
    |                           |-- NVMe Read from disk
    |                           |-- Read lock entry from RAM
    |                           |
    |<-- data + lock value -----|
    |                           |
    |  Check lock value:
    |  lock == 0 (free)?  --> Data is consistent, return to app
    |  lock != 0 (held)?  --> Retry just the view lock (not the data read)
    |  lock is stale?     --> Trigger RECOVER_STALE
```

**Why view-lock is correct in a perfect topology:**

1. Reads always go to the **owner segment** — the authoritative source.
2. If a concurrent write is in progress, the owner lock is held — the
   view-lock sees it and the client retries.
3. If the writer crashed, the lock is stale — recovery copies from the owner
   segment, the same data the reader already saw.
4. Ownership is stable — all clients agree on which segment owns each
   blockset, so no client reads from the wrong segment.
5. Therefore: the read is always consistent with the eventual recovery
   outcome.

**2. Local-read optimization.**
(`__is_local_read_optimization_allowed()` and `__get_role_of_read()`,
`datapath_mirror/nvmeibc_block_dp_mirror.c`).


If one of the replica segments resides on the same physical server as the
client, reads may be directed to that local segment instead of following normal
owner rotation. This avoids a network hop entirely.

At the mirror helper level, `__is_local_read_optimization_allowed()` only
re-checks two things: that the datapath has enabled the optimization for this
volume, and that the topology is perfect. The policy decision behind that
enablement comes from higher-level attach/reservation management rather than
from this helper itself. See (`__is_local_read_optimization_allowed()`) function.

### 6.2 Degraded Topology (recovering or partially dead)

When the topology is not perfect, TOMA may be **moving primary lock
ownership** between segments — for example, when a segment transitions from
W (recovering) to RW. During this transition, different clients may briefly
hold different topologies and disagree on which segment owns a given
blockset.

In this state, a view-lock is not safe:

- **The read might go to the wrong segment.** If the client's topology is
  stale, it may read from a segment that is no longer the owner, potentially
  seeing partially-synced data.
- **A sync operation could be writing concurrently.** Recovery takes
  exclusive locks to copy data from the good segment to the recovering one.
  A view-lock provides no mutual exclusion, so the read could see a block
  mid-sync.

Therefore, reads in a degraded topology **take an exclusive lock** (same CAS
mechanism as writes), ensuring they wait for any in-flight sync to complete
and read from a segment whose data is known to be consistent. The
local-read optimization is also disabled, since not all replicas hold valid
data.

### 6.3 Segment Selection

`__get_role_of_read()` (`datapath_mirror/nvmeibc_block_dp_mirror.c`)
selects which segment to read from:

1. Default: the **owner segment** for this blockset (follows rotation).
2. Perfect topology only: if a local segment is available, prefer it (skips
   network hop).

---

## 7. Trim (Discard) Flow

Trim shares the lock infrastructure with writes but differs in command
preparation, execution, and has no stage machine.

### 7.1 Overview

A trim tells the storage that a range of blocks is no longer needed. Unlike
writes, trims carry no data payload — they send an NVMe Dataset Management
(DSM) range descriptor (start LBA + length) to each live replica. Trims may
span many blocksets in a single operation.

**Maximum trim size reported to the OS.** The kernel block layer splits
incoming trims based on `max_discard_sectors`
(`nvmeibc_block_api_os.c:__set_max_trim()`). The limit depends on volume type:

- **Mirrored volumes**: 128 blocksets = **16 MB** (module parameter
  `max_trim_size_mirrored`, default 128).
- **Non-mirrored volumes**: 16384 blocksets = **2 GB** (module parameter
  `max_trim_size_non_mirrored`, default 16384).

The mirrored limit is much smaller because each blockset requires a
distributed lock and the prediscard barrier (section 7.3) must collect all
lock results before proceeding. Both parameters are tunable at runtime via
`/sys/module/nvmeibc/parameters/`.

### 7.2 Command Preparation

(`__mirror_cmds_add_for_raid`, `__concat_discard_op`,
`datapath_mirror/nvmeibc_block_dp_mirror.c` and
`datapath_mirror/nvmeibc_block_dp_mirror_trim.c`)

Trim commands are prepared without staging (`use_stages = false`). For each
replica, `__concat_discard_op()` tries to **merge adjacent trim ranges** on
the same disk into a single command. If the current trim range is contiguous
with the previous command on the same disk, the existing command is extended
rather than creating a new one. This reduces the number of NVMe DSM
commands sent to the target.

All trim commands across all blocksets are treated as siblings under a
single uncompleted-commands counter, unlike writes where each blockset has
its own raid-leader and stage machine.

### 7.3 Lock Acquisition: The Prediscard Barrier

Trim lock acquisition has a two-phase design that differs from writes.

**Phase 1 — Acquire all owner locks.** Owner locks for all blocksets are
issued in parallel using the same CAS mechanism as writes, but are typed as
PREDISCARD instead of LOCK_OWNER. A global `prediscards` counter tracks how
many owner locks are still outstanding.

**Phase 2 — Split decision.** When the last PREDISCARD lock completes
(counter reaches zero), the code inspects the results of all owner locks
before proceeding:

- **All locks taken** — no split needed. Each PREDISCARD lock is converted
  to a normal LOCK_OWNER and proceeds through the standard lock actions
  (fan-out copy-owner locks, etc.).
- **Some locks contended** — the trim is **split**. The contended blocksets
  are separated into their own commands so they can be retried independently
  while the uncontended blocksets proceed. The split creates new command
  structures with adjusted LBA ranges, relinks them to the appropriate
  locks, and replaces the original command array.
- **Any lock dead** — no split, the operation will be retried on a new
  topology.

This barrier exists because trim commands may be merged across blockset
boundaries. The split logic must see all lock results at once to correctly
separate contended ranges from uncontended ones.

### 7.4 Execution and Completion

Trim commands execute without a stage machine — there is no pre-read, no
dirty-bit piggyback, and no post-IO RDMA stage. Commands are sent directly
to all live replicas and each command releases its locks immediately upon
completion, without waiting for other commands.

The abandon decision uses the same logic as writes: if some trim commands
succeeded and others were not issued, the lock is abandoned (becomes stale)
to ensure recovery handles the inconsistency.

### 7.5 Trim Consistency Limitations

NVMesh reports `discard_zeroes_data = 0` to the kernel, meaning reads after
trim return **indeterminate data**. The content depends on each physical
NVMe disk's firmware (DLFEAT): some return zeros, some return old data, some
return 0xFF. This creates two known consistency gaps:

**1. Failover after trim.** Two different disks in a mirror may return
different values for the same trimmed block. If the owner dies and the
client fails over to the copy, the read returns different data — without any
write having occurred. At the block device level the mirror replicas are
silently inconsistent for trimmed blocks.

**2. Trim during degraded topology.** Trims do not turn on dirty bits for
dead segments. The rationale: turning on dirty bits would force a write
operation during rebuild, defeating the purpose of trim (which exists to
avoid writes and free flash blocks). As a result, when the dead segment
recovers, `RECOVER_DB` may not visit trimmed blocksets — it sees no dirty
bit and skips them. The recovering segment retains old (pre-trim) data while
the live segments have deallocated blocks.

**Why this is acceptable in practice.** Filesystems (ext4, xfs, etc.) never
read trimmed blocks without first writing to them. The `discard_zeroes_data
= 0` contract tells the OS that post-trim reads are undefined, so correctly
behaving filesystems will not observe the inconsistency. However, at the raw
block device level, the guarantee is broken.

---

## 8. I/O Flow Error Handling

### 8.1 Abandon Decision

`dp_mirror_calc_should_abandon()` (`datapath_mirror/nvmeibc_block_dp_mirror.c`)
— after all write/trim commands for a blockset complete:

```
  Abandon decision tree:

  All commands succeeded?
    |
    Yes --> Normal lock release (RDMA CAS unlock)
    |
    No
    |
    Some succeeded + some not issued?
    |   (wr_not_issued > 0 && wr_succeeded > 0)
    |
    Yes --> FORCE_ABANDON (reason: PARTIAL_SLICE)
    |       One replica has new data, the other has old data.
    |       Lock becomes stale → recovery will sync from owner.
    |
    No
    |
    Any command failed?
    |   (wr_failed > 0)
    |
    Yes --> FORCE_ABANDON (reason: WRITE_FAILED)
    |       Error → unknown state on disk.
    |       Lock becomes stale → recovery will verify + sync.
    |
    No --> Normal release (all were not-issued = retry)
```

### 8.2 Operation-Level Outcome

`dp_mirror_calc_comp_state()` (`datapath_mirror/nvmeibc_block_dp_mirror.c`)
— scans all commands after the operation completes:

- **Transport errors and transient NVMe errors**: the entire operation is
  retried on the current or a new topology.
- **Permanent NVMe errors**: propagated up as a failure to the application.

### 8.3 Trim Error Handling

Trim failures follow the same abandon logic as writes, with two differences:

**Execution cascade.** Trim commands linked to a blockset are executed
sequentially (`dp_cmds_execute_stageless_trim_cmds`,
`datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c`). If any
command fails, all remaining commands for that blockset are aborted without
being sent. This prevents partial-trim scenarios where one replica is
trimmed and the other is not.

**No data corruption risk.** A failed trim does not corrupt data — the
blocks simply remain allocated. However, the lock is still abandoned if the
failure was partial, because the trim may have succeeded on one replica but
not the other, leaving the replicas in different states regarding which
blocks are deallocated. Recovery will reconcile this.

---

## 9. Sync Operations and Recovery

A **sync operation** is a small, per-blockset repair action. It fixes one
concrete problem on one blockset: a stale lock, a dirty bit, a read failure,
etc. Sync operations can be called **inline from the I/O flow** (e.g. when
lock acquisition encounters a stale lock) or from a **recovery process**.

A **recovery** is a TOMA-requested process that iterates over a range of
problematic blocksets and applies the appropriate sync operation to each one.
Recovery is the iterator; sync is the per-blockset fix it applies.

### 9.1 Sync Operations

All sync operations are dispatched through `dp_mirror_sync_execute_op()`
(`datapath_mirror/nvmeibc_block_dp_mirror_sync.c`). Entry points are in
`recovery/nvmeibc_block_dp_sync_common.c`.

#### Data sync operations (read/write disk)

**RECOVER_STALE** (`nvmeibc_sync_fix_stale`) — Fix a stale lock.
The owner segment is authoritative. Reads from owner, writes to all others
that differ. Can operate on a partial blockset via
`nvmeibc_sync_fix_some_slices_in_stale()`.
**Inline caller:** lock acquisition path — when CAS finds a stale lock and
the stale-lock resolver says it's safe to take, the I/O triggers this sync
before proceeding. Also called from the read view-lock path when a stale
lock is encountered.

**RECOVER_DB** (`nvmeibc_sync_recover_dirty`) — Fix dirty bits.
Reads from healthy segments, writes to the recovering segment, clears dirty
bits. This is the workhorse of segment rebuild.

**RECOVER_READFAIL** (`nvmeibc_sync_read_failure`) — Fix a bad
sector. Reads from the other mirror leg. If successful, writes the good data
back to the failed segment (sector repair). If both fail, returns error.
**Inline caller:** read completion path — when a read command returns a
permanent disk error, the datapath calls this sync to attempt repair from
the other replica.

**RECOVER_SCRUBBING** (`nvmeibc_sync_scrubbing`) — Background data
integrity check. Reads all replicas and compares them. If a mismatch is
found, copies from the authoritative source.

#### Commandless sync operations (lock manipulation only, no disk I/O)

**REC_R1_COMMIT_STALE** (`nvmeibc_sync_commit_stale_lock`) —
Propagate a stale lock value from the owner to all copy-owner locks, so the
state is consistent across replicas. Used during dirty rebuild when a
blockset has a stale lock but no dirty bits — deferring the full stale fix
makes the rebuild faster. Mirror-only.

**REC_R1_CONV_STALE2DB** (`__convert_stale_special_2_dirty_bit`) —
Convert a stale-special lock value into a dirty bit instead of performing a
full stale recovery. Only possible in degraded mode (e.g. (RW, DEAD) or (RW,DEAD,DEAD)) where
full sync cannot proceed. Cheaper: marks the blockset as needing future sync
without reading data. Triggered internally from
`nvmeibc_sync_fix_some_slices_in_stale()`. Mirror-only.

**REC_COMMIT_BINFO** (`nvmeibc_sync_commit_binfo`) — Propagate the
owner's binfo to all copy-owner locks. Relevant when copy-owner locks have
stale or incorrect binfo (e.g. failed dbit turn-off, or W segment with
uncommitted binfo). If a stale lock is encountered, mutates to
`REC_R1_COMMIT_STALE`.

#### Maintenance sync operation

**REC_DCONVICT_TURN_ON** (`nvmeibc_sync_turn_on_dirty_convict`) —
Mark a segment's dirty bits as "untrusted" (convict) in the binfo. The
operation itself is generic — it works for any RAID type regardless of
segment count. It modifies the dirty bits via the dbits engine and commits
the updated binfo to all locks. Delegated to `dp_maintenance_execute_op()`.

### 9.2 Recovery (TOMA-requested blockset iteration)

TOMA requests a recovery by sending the client a recovery type and a
blockset range. The client runs an iterator that walks the range in batches,
querying the server for which blocksets have problems (stale locks, dirty
bits, uncommitted binfo).

A recovery is used as the barrier before switching to a better topology.

Each recovery type has a **hard-coded default sync function**, set via
`nvmeibcbdpec_sync_get_fn_by_rtype()`
(`recovery/nvmeibc_block_dp_sync_common.c`). This default is applied to
every blockset in the range. For some recovery types, the per-blockset
problem report may **mutate** the sync function to a more suitable one
before dispatch.

Recovery types relevant to mirror (`enum NVMEIBT_RECOVERY_TYPE`,
`toma/clnt/nvmeibt_client_protocol.h`):

| Recovery Type       | Default Sync                            |
| ------------------- | --------------------------------------- |
| DIRTY_REBUILD       | `nvmeibc_sync_recover_dirty`            |
| STALE_REBUILD       | `nvmeibc_sync_fix_stale`                |
| SCRUBBING           | `nvmeibc_sync_scrubbing`                |

**DIRTY_REBUILD** — Segment returns after failure. TOMA marks it as
`W`. Default sync is `nvmeibc_sync_recover_dirty`, but the
per-blockset problem report may mutate it:

- Stale lock without dirty bits → mutates to
  `nvmeibc_sync_commit_stale_lock` (defer stale fix, just propagate the
  lock to copies — makes rebuild faster)
- Uncommitted binfo → mutates to `nvmeibc_sync_commit_binfo`

When complete, the segment is promoted to `RW`.

**STALE_REBUILD** — Rebuild a range of blocksets that have stale locks (e.g.
a client crashed). Uses `nvmeibc_sync_fix_stale` for every blockset. The
lock's initial CAS compare value is pre-set to the stale-special value to
reduce contention on the first attempt.

**SCRUBBING** — Background integrity check across a blockset range.
Uses `nvmeibc_sync_scrubbing` for every blockset.

**EC_DCONVICT_TURNON** — Turn on dirty convict flags across a range.
Uses `nvmeibc_sync_turn_on_dirty_convict` for every blockset. Despite the
"EC" prefix in the enum name, this also applies to mirror. Despite being a 
recovery enum value, TOMA does not request it.

The recovery loop (`recovery/nvmeibc_raid_recovery.c:__recover_next_blockset()`) runs up to 64
sync operations in parallel per recovery, processing blocksets in batches
requested from the server.

### 9.3 Segment Replacement: W- and the DCONVICT_TURNON & DIRTY_REBUILD chain

When a segment returns after being temporarily dead (W access mode), its
dirty bits in lock RAM are accurate — they were maintained by writes while
the segment was unavailable. DIRTY_REBUILD can ask the server "which
blocksets have dirty bits?" and trust the answers.

When a segment's **disk is replaced** (W- / W_IS_DIRTY access mode), the
situation is different. The **owner's binfo** may not have dirty bit set 
for the W- segment: from the owner's perspective, the data appears in sync. 
However, any blockset that was **not written since replacement** still contains 
whatever garbage the new disk has, yet the owner shows it as clean. 
If DIRTY_REBUILD ran directly, it would ask the server for problem reports, 
see no dirty bits for those blocksets, skip them, and leave the replaced segment with invalid data.

To solve this, the **client** auto-launches a DCONVICT_TURNON recovery as a
prerequisite before DIRTY_REBUILD
(`recovery/nvmeibc_raid_recovery.c:__aux_recovery_call_if_needed_or_do_first_batch()`).
TOMA never sends this recovery
directly — the comment in TOMA's code says "Should not be executed directly.
Auto launched by client."

The chain works as follows:

1. TOMA sends DIRTY_REBUILD to the client.
2. The client inspects its topology and detects W- segments
   (`w_is_dirty_segs_bmp = nvmeibc_raid1_get_sgmnts_bmp(r1, wm)`).
3. If W- segments exist, the client launches DCONVICT_TURNON as an
   **auxiliary recovery** that runs first. This is a full-range pass that
   visits **every blockset unconditionally** (no server query needed — line
   293: `need_info_from_server = false`), marking all dirty bits as
   "convict" (untrusted, assume worst case).
4. DIRTY_REBUILD waits for DCONVICT_TURNON to finish via callback chaining.
5. Once convicts are set, the server's problem reports become accurate and
   DIRTY_REBUILD can proceed correctly.

**Why a separate recovery pass, not per-blockset or I/O inline?** The
server's problem reports are the input to DIRTY_REBUILD's iterator. If
convicts aren't set first, the iterator receives wrong answers and skips
blocksets — a chicken-and-egg problem. Folding it into the I/O flow doesn't
work either: I/O only visits blocksets the application accesses, leaving
unvisited blocksets on the replaced disk silently wrong. A separate
unconditional full-range pass is the only correct approach.

**Why client-side, not TOMA?** Whether dirty bits are trustworthy is a
client-local observation based on the current topology. The topology can
change between when TOMA sends a recovery and when the client processes it.
By handling it client-side, the client atomically inspects its topology,
chains the prerequisite, and runs both recoveries in a single session — no
extra TOMA round-trips, no ordering races.

#### Optimization: zero-init of W-segment RAM dirty bits in R1-2

When TOMA places a segment into W mode it initializes that segment's server
lock RAM before the client connects. For dirty bits, the conservative choice
is to write unknown markers (`0xF` nibbles) — this forces dirty rebuild to
treat every blockset as potentially needing sync. That is always correct, but
it causes dirty rebuild to commit binfo for every blockset even when most of
them are actually clean.

For **R1-2** (two-replica mirror), TOMA instead initializes the W segment's
RAM dirty bits to **zero**. This is safe because R1-2 has only two segments.
There is no third segment that a dbit could reference, so zero dirty bits
truthfully represent "no known dirty state." Dirty rebuild then queries the
server, receives zero dbits for clean blocksets, and skips the binfo commit
for those blocksets — a significant speedup on large volumes where only a
small fraction of blocksets were dirtied while the segment was down.

The optimization is also safe because TOMA **never promotes the segment to
RW until dirty rebuild has fully completed**. By the time the segment enters
service as RW, all dirtied blocksets (those with non-zero dbits, correctly
preserved by the writes-during-failure path) have been fully synchronized.
Blocksets with zero dbits were genuinely clean and required no sync. The two
properties together — no phantom third-segment dbits, and full sync before
promotion — guarantee the segment's data is complete and consistent when it
becomes authoritative.

For **R1-3** (three-replica mirror), zero-init is **not safe** and unknown
markers must be used. The hazard arises as follows:

1. Topology is `[RW, W, DEAD]`. The RW segment's binfo for some blockset
   records a dbit pointing to DEAD — written before DEAD died, never synced
   back.
2. W's RAM dirty bits are initialized to zero. Dirty rebuild queries the
   server, sees no dbit on W for that blockset, and **skips committing binfo
   from RW to W**. W's copy-owner lock for that blockset is never updated
   from RW's authoritative value.
3. Dirty rebuild completes and W is promoted to RW. Its binfo for the
   skipped blockset still reflects the zeroed initial state — it has no
   record of DEAD's dbit.
4. The original RW now fails. W (now the sole RW) is the only source of
   binfo. That binfo is missing the DEAD dbit entirely.
5. When DEAD eventually returns and requests a dirty rebuild, the new RW
   reports no dirty bits for DEAD on those blocksets. The rebuild skips
   them, leaving DEAD with stale data and no record of the gap.

Using unknown markers for W in R1-3 forces dirty rebuild to commit binfo
from RW to W for every blockset that has any dbit activity, ensuring W
learns about DEAD's dbit before it can become the sole source of truth.

Implemented in `nvmeib_dbits_entry_build_unknowns_generic()`
(`common/nvmeib_shared.h`).

### 9.4 Cold Recovery (all servers reboot)

Clients are stateless — all lock state (lock values, TxID, dirty bits)
lives in **server (target) RAM**. Cold recovery handles the scenario where
**all servers reboot** and lock RAM is lost.

**What gets lost.** Each server holds lock RAM for its disk segments. When
a server reboots, all lock values and blockset info (TxID + dirty bits) for
its segments disappear. Locks become zero (unlocked), dirty bits are gone,
and TxID is unknown.

**TOMA re-initializes server lock RAM before allowing clients to register.**
For each RW (owner) segment, TOMA writes:

- **Lock values** → set to stale-special sentinel
  (`nvmeib_stale_special_raid1`). This marks every blockset as "unknown
  state after RAM loss."
- **TxID** → set to 0 (`INITIAL_LAZY_READ_TXID` — unknown).
- **Dirty bits** → set based on topology. In normal mode (all RW), dirty
  bits are zero — no segment is out of sync. In degraded mode, unknown
  dirty bit markers (`0xF` nibbles) are used, except for R1-2 where
  W-segment dirty bits are zero-initialized as an optimization (see
  section 9.3).

**Optimization: orderly shutdown.** If TOMA can perform a graceful shutdown
(not a crash), it saves TxID, dirty bits, and stale lock values to disk
before the servers go down. On restart, it reloads these values into server
RAM instead of setting everything to unknown, avoiding expensive recovery.

**Why no client-side work is needed for mirror.** Mirror has no journals —
there is no write hole to recover from. EC needs client-side cold recovery
to read journals on the servers and reconstruct which writes were in
flight — mirror doesn't. TOMA's server RAM initialization is sufficient:
every blockset gets a stale-special lock, and clients resolve these lazily
during normal I/O through the standard stale lock handling (RECOVER_STALE
sync or REC_R1_CONV_STALE2DB in degraded mode).

**After cold recovery.** When clients connect and begin I/O, every blockset
they touch has a stale lock on the server. The first access triggers inline
stale lock resolution (see section 4.2). For mirror this is cheap: read
from the owner segment, compare with the other legs, and if the data
matches (the common case after a clean crash with no in-flight writes),
just clear the stale lock. TOMA may also request a STALE_REBUILD recovery
to proactively resolve stale locks across the volume rather than waiting
for I/O to visit each blockset.

---

## 10. Blockset Info & Dirty Bits

This section is probably incorrect and/or does not reflect the final implementation.

If the segment access mode is "Write", the block code will not take into account the blockset info.
So, it is perfectly correct to write "any" value to it, but a performance wise decision would be to write "0"(no dirty segments).
Why? Because we will use owner blockset info to find out if the block was written or not. And if not,
then we can skip "Write" segment blockset info update.

### 10.1 Blockset Info Structure

Each lock word on a segment's RAM stores a **blockset info (binfo)**: a
packed value containing a **TxID** (transaction ID) and **dirty bits**. The
dirty bits field encodes per-segment dirty status using 4-bit nibbles, where
each nibble identifies a degraded segment (by index+1), or the special value
`0xF` meaning "unknown."

Every lock sibling (owner, copy-owners) holds its own copy of the binfo.
After lock acquisition, the client reads all copies and merges them via
intersection to produce a single authoritative binfo for the blockset.

### 10.2 How Unknown Dirty Bits Arise

Unknown dirty bits (`0xF`) appear when the stored binfo on a segment does
not reflect reality. Three scenarios produce them:

1. **Segment was dead and rejoined.** While the segment was down, writes
   to other legs changed which segments are dirty. The copy-owner binfo on
   the formerly-dead segment is stale — it still holds whatever value it had
   before the segment died.

2. **Merge of disagreeing copies.** When the owner and copy-owner binfos
   disagree (e.g. one saw a write the other missed, due to a crash
   mid-write), the intersection merge may produce unknown markers. If one
   copy says a segment is dirty and the other does not mention it, the merge
   encodes this ambiguity as unknown.

3. **Initial state.** A freshly formatted or reset segment starts with
   TxID = 0 (`INITIAL_LAZY_READ_TXID`) and zeroed dirty bits. When the
   topology later becomes degraded and writes happen, the binfo on
   non-owner copies may not have been updated, leaving stale values that
   conflict with the owner's reality.

### 10.3 Resolution: Worst-Case Assumption

Unknown dirty bits are resolved in the binfo analysis phase (after locks are
taken, before IO). The client replaces every unknown marker with "dirty for
all segments that could possibly be degraded in the current topology." This
is conservative — it may flag segments as dirty when they are not — but it
guarantees that recovery will never miss a blockset that actually needs
syncing.

This is also where the current implementation limit matters: mirror can exist
on more than two replicas, but the binfo/dbits encoding is not designed to
carry arbitrary degraded-state detail for every leg. The datapath therefore
falls back to conservative worst-case handling once the represented degraded
state reaches its encoding limits.

## 11. Why Mirror Has No Write Hole

The EC write-hole problem (see `ec_io_flow.md` Section 1) arises because
parity and data are interdependent: a crash after writing data but before
updating parity leaves the array inconsistent. Reconstruction from stale parity
yields wrong data.

Mirror has no such problem. Every replica is a complete, self-contained copy of
the data. A crash mid-write leaves replicas divergent, but no information is
lost — the surviving copy is authoritative.

```
  EC (3+1):  A crash can corrupt parity → silent data loss on rebuild
  Mirror:    A crash can diverge copies → but the owner copy is authoritative

  Mirror crash scenario:

  Time   Seg 0 (copy A)   Seg 1 (copy B)
  -----  ---------------  ---------------
  t=0    D (old)          D (old)           consistent

         Write D' to both...

  t=1    D' (new)         D (old)           <-- CRASH after Seg 0, before Seg 1

  Recovery:
    Lock on Seg 0 was taken → Seg 0 is authoritative
    Stale lock recovery copies Seg 0 → Seg 1
    Result: both have D' (new)           consistent again
```

Mirror consistency relies on three mechanisms instead of journals:

1. **Distributed locks** — mutual exclusion via RDMA CAS
2. **Dirty bits** — track which segments are out of sync

## 12. TODO

1. How does the segment memory is initialized?
2. What process introduces unknown dirty bits?
