# Mirror (RAID-1) Write Flow

This document describes the mirror write path in the NVMesh block client. It
follows the same textbook structure as the companion EC write flow document
(`ec_io_flow.md`). Concepts are introduced before they are used, and data
structures are diagrammed before the algorithms that manipulate them.

Mirror is dramatically simpler than EC. Where EC requires journals, transaction
IDs, parity calculation, and a 7-stage pipeline, mirror writes identical copies
to N segments using a 1-stage pipeline (occasionally 2). This document explains
what mirror does, why it is simpler, and where the complexity hides.

Code references are relative to `clnt/block/` unless otherwise noted.

---

## 1. Why Mirror Has No Write Hole

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
3. **Active locks** — crash-recovery journal for the lock itself

---

## 2. Physical Layout

### 2.1 Segments and Blocksets

A mirror volume replicates data across _N_ segments (typically 2). Each segment
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

  Each blockset = 128KB = 32 x 4KB blocks (LOCKSET_SLICES = 32)
  Owner rotates: owner_seg = blockset_num % replicas
```

### 2.2 Contrast with EC

| Property           | Mirror                  | EC                    |
| ------------------ | ----------------------- | --------------------- |
| `slice_size`       | 1                       | k (data segments)     |
| `replicas`         | N (copies, typically 2) | k + m (data + parity) |
| Parity segments    | None                    | m                     |
| Data per write     | Identical to all        | Different per segment |
| Planning structure | None                    | MSSA (2D bitmap)      |

---

## 3. Key Data Structures

### 3.1 Lock Entry (64 bits)

Mirror uses the same 64-bit lock entry as EC (see `ec_io_flow.md` Section
3.1). The `blkset_info` upper 32 bits contain TxID and Dbits, but mirror only
uses the Dbits field.

```
  union nvmeib_lock_blkset_entry (64 bits) — common/nvmeib_shared.h:444

  63           52 51       32 31 30 29 28 27                4 3          0
  +-------------+-----------+-----+--+--+--------------------+------------+
  | dirty (12b) | txid (20b)|  rv |rd|st|       lock_id (24b)| idx_praid  |
  +-------------+-----------+-----+--+--+--------------------+------------+
  |<--- blkset_info (32b) ->|<------------ lock_id (32b) ---------------->|

  Mirror-specific usage:
    dirty  [63:52]  Active — tracks degraded segments
    txid   [51:32]  UNUSED (always 0 in piggybacks; never read or compared)
    st     [28]     Stale bit — triggers recovery when detected
    rd     [29]     Read-lock flag for view locks
```

### 3.2 Block Metadata (64 bits, Optional)

Mirror uses the **parity (P) variant** of the shared block MD structure
(`datapath_utils_generic/nvmeibc_block_dp_block_md.h:51`). All mirror legs are
treated as parity (`NVMEIBC_DATA_MD_MIRROR_IS_PARITY` — `:84`),
because the P-variant has fewer EDIC bits but includes dbit descriptor fields
(unused by mirror but structurally required).

```
  union nvmeibc_block_dp_ec_data_block_md — P variant (mirror)
  datapath_utils_generic/nvmeibc_block_dp_block_md.h:51

  63  62 61      52 51        32 31  28 27  24 23     11 10       2 1  0
  +----+----------+-----------+------+------+---------+----------+----+
  |rsv | jri (10) | tx_id(20) |db1(4)|db0(4)|edic(13) |d2j_rng(9)|ver |
  |(2) |          |           |      |      |         |          |(2) |
  +----+----------+-----------+------+------+---------+----------+----+

  Mirror-specific fill — nvmeibc_block_dp_ec_md_make_r1() (:143):
    ver      = 1 (NVMEIBC_DATA_MD_VERSION)
    d2j_rng  = 0 (no journal)
    edic     = CRC32c truncated to 13 bits (when EDIC enabled)
    db0, db1 = 0 (mirror does not write dbit descriptors to disk MD)
    tx_id    = 6 (NVMEIBC_MIRROR_UNUSED_TXID_JRI — sentinel, never used)
    jri      = 6 (same sentinel — no journal range)
    rsv      = 0

  Block MD is OPTIONAL for mirror. It is only present when the disk is
  formatted with 4096+8 metadata AND EDIC checking is enabled on the volume.
  Without EDIC, mirror writes no per-block metadata at all.
```

### 3.3 Lock Types

Mirror uses three RDMA lock operations (`enum nvmeibc_rdma_intent` —
`common/nvmeib.h:249`), each with a distinct purpose:

```
  RDMA Lock Operations for a 2-way mirror write to one blockset:

  Client                    Server A (Owner Seg)     Server B (Copy Seg)
    |                           |                        |
    |-- CAS (lock_id=0→me) ---->|                        |
    |   NVMEIBC_CMD_LOCK_OWNER  |                        |
    |   (atomic compare-swap)   |                        |
    |                           |                        |
    |-- Force-Write (me) ------------------------------->|
    |   NVMEIBC_CMD_LOCK_COPY_OWNER                      |
    |   (no compare, just write)                         |
    |   = "Active Lock"                                  |
    |                           |                        |

  Lock types:
    LOCK_OWNER      CAS on owner segment. Provides mutual exclusion.
                    Only succeeds if lock is currently free (value=0).

    LOCK_COPY_OWNER Force-write on replica segment. No compare.
                    Acts as a "crash journal" for the lock — if Server A
                    crashes, Server B's active lock records that the
                    blockset was in-flight, triggering recovery.

    LOCK_READ_PB    View lock (reads only). Piggybacked on a read command.
                    Does not acquire the lock; just reads its value to
                    check for concurrent writers.
```

**Why active locks exist:** If only the owner segment held the lock and that
server crashed, the lock state would be lost. The active lock on the other
server ensures the cluster can detect the in-flight write and trigger stale-lock
recovery.

### 3.4 Owner Rotation

Lock ownership alternates per blockset to distribute load:

```
  owner_segment = blockset_number % replicas

  Blockset 0 → Seg 0 owns lock, Seg 1 has active lock
  Blockset 1 → Seg 1 owns lock, Seg 0 has active lock
  Blockset 2 → Seg 0 owns lock, Seg 1 has active lock
  ...

  This means reads to even blocksets go to Seg 0,
  reads to odd blocksets go to Seg 1 (load balanced).
```

---

## 4. The Write Flow

### 4.1 Stage Machine Overview

Mirror uses at most 3 stages from the shared `enum e_cmds_stage`. A typical
healthy-topology write uses just **one stage**.

```
  Mirror stage machine (contrast with EC's 7 stages):

  dp_mirror_prepare_op()         Allocate commands, fill locks, compute dbits
         |
  dp_mirror_execute_op()         Acquire locks (RDMA CAS + active lock)
         |
         v
  +----- WAIT_FOR_LOCK ----------+  Profiling: lock acquisition time
         |
  dp_mirror_exec_func_on_locks_tkn()   Minimal (no binfo analysis)
         |
         v
  +== READ_PRE_DATA (0) ========+  RARE: only for sub-block RMW (512B writes)
  |  Read full 4KB block,       |
  |  merge 512B write, re-EDIC  |
  +=============================+
         |
  +== DO_IO_AND_PAR (4) ========+  ALWAYS: write to all live replicas
  |  Both mirror cmds share     |  in parallel, same data buffer.
  |  the same source buffer.    |  Piggyback dbit turn-on if degraded.
  +=============================+
         |
  +-- POST_IO_RDMA (5) ---------+  CONDITIONAL: only when clearing dbits
  |  Separate RDMA to turn off  |  (W_NO_DIRTY segment, full blockset write)
  |  dbit after IO succeeds     |
  +-----------------------------+
         |
  Operation complete -> bio_endio()
         |
  Lock release / abandon / transfer
```

### 4.2 Step-by-Step Description

#### Step 1: Prepare Commands

`dp_mirror_prepare_op()` (`datapath_mirror/nvmeibc_block_dp_mirror.c:795`)

1. Validate layout (LBA range fits topology).
2. Calculate allocation counts:
   - **Commands**: `max_blocksets x replicas` for writes (2 per blockset for
     2-way mirror). 1 per blockset for reads.
   - **Locks**: 2 per blockset for writes (owner + active). 1 for reads (view).
     0 for JBOD.
3. Allocate commands, locks, and SGLs (co-allocated in one slab when possible).
4. Decide whether to copy user buffers (required when EDIC is enabled to prevent
   mirror divergence if the user modifies the buffer between the two writes).
5. Fill locks via `dp_fill_locks_for_io()`
   (`datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c:836`).
6. Build write commands via `__mirror_cmds_add_for_raid()` (`:501`):
   - One command per live replica (dead segments excluded).
   - **Shared buffer**: both commands point to the same user data pages. The
     second command reuses the first command's bio vector iterator. No data
     copy occurs.
7. Compute dirty-bit action via `__prepare_mirror_binfo_for_write()` (`:347`):
   - Turn-on dbits for dead segments.
   - Turn-off dbits for `W_NO_DIRTY` segments (only if full blockset write).
   - Attach dbit piggyback to write commands.
8. Link commands to locks via CL_MAT (Command-Lock Matrix).

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

`dp_mirror_execute_op()` (`datapath_mirror/nvmeibc_block_dp_mirror.c:901`) →
`dp_locks_send_all()` (`datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c:1356`)

For writes, two RDMA operations are issued per blockset:

1. **RDMA CAS** to the owner segment (acquire exclusive lock).
2. **RDMA Write** to the copy segment (record active lock).

Both are asynchronous. When all locks for a blockset are acquired, the
`dp_mirror_exec_func_on_locks_tkn()` callback
(`datapath_mirror/nvmeibc_block_dp_mirror.c:946`) fires. For mirror, this
callback is intentionally minimal — it does NOT read binfo, analyze TxID, or run any
maintenance syncs (unlike EC which runs a multi-phase sub-state-machine here).

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

For normal 4KB-aligned writes, this stage is skipped entirely.

#### Step 4: Write to All Replicas

`E_CMDS_STAGE_DO_IO_AND_PAR` (stage 4)

All write commands fire in parallel via RDMA. Both replicas receive the same
data (shared buffer). Each command carries:

- 4KB data payload (via RDMA Write to server's bounce buffer)
- 8-byte block MD if EDIC is enabled (via separate RDMA to server's MD buffer)
- Optional dbit piggyback (via RDMA Write to lock entry on same server)

```
  Parallel mirror write:

  Client                    Server A               Server B
    |                          |                       |
    |-- RDMA Write: data ----->|                       |
    |   + MD (if EDIC)         |                       |
    |   + dbit piggyback       |                       |
    |                          |--- NVMe Write         |
    |                          |    to disk A          |
    |                          |                       |
    |-- RDMA Write: data ----------------------------->|
    |   + MD (if EDIC)         |                       |
    |   + dbit piggyback       |                       |
    |                          |                       |--- NVMe Write
    |                          |                       |    to disk B
    |                          |                       |
    |<---- completion ---------|                       |
    |<---- completion ---------------------------------|
```

#### Step 5: Dirty-Bit Piggyback

`dp_cmds_piggyback_dbR1_on_write()`
(`datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c:181`) — attached
during Step 1 (prepare)

Dbits are written to the lock entry as a piggyback RDMA alongside the data
write. The transport atomically sends both the data and the binfo update to the
same server.

```
  Dbit actions during mirror write:

  Topology state       Action           Condition
  ------------------   ---------------  ----------------------------
  All segments RW      No dbit action   Healthy — nothing to track
  Seg N is DEAD        Turn ON dbit N   Write can't reach Seg N
  Seg N is W_NO_DIRTY  Turn OFF dbit N  Only if full blockset write
                                        (nlbas == LOCKSET_SLICES = 32)
  Seg N is W_IS_DIRTY  Turn ON dbit N   Still dirty, being recovered
```

**Turn-off requires a separate stage**: When dbits need clearing
(`W_NO_DIRTY` + full blockset), the piggyback is set to `WR_DR` (direct RDMA)
with a callback (`__post_cmd_dirtybit_turnoff_cb`). The actual turn-off RDMA
fires in `POST_IO_RDMA` (stage 5) after the data write succeeds.

**Implicit sync**: Writing a complete 128KB blockset to a recovering
(`W_NO_DIRTY`) segment implicitly synchronizes it — the entire blockset is now
up-to-date, so the dbit can be cleared without a separate recovery operation.

**Key difference from EC**: Mirror computes dbits during `prepare_op` (before
locks are acquired), using the topology's static `dbits_on_mask` and
`dbits_off_mask`. EC computes dbits after lock acquisition because it needs to
read the lock's pre-IO binfo and merge with the IO's action. Mirror skips this
because it does not use TxID and always starts from zero dbits.

#### Step 6: Complete BIO

`dp_mirror_calc_comp_state()` (`datapath_mirror/nvmeibc_block_dp_mirror.c:144`)
→ `nvmeibc_operation_complete()` (`datapath_utils_generic/nvmeibc_block_dp_operation.c:312`)
→ `bio_endio()`

The completion callback examines each command's result:

- If any command succeeded: IO can be considered successful (data is on at
  least one replica).
- Transient errors (network timeout, queue full): trigger retry.
- Permanent errors (bad sector, HW failure): propagated to the caller.

#### Step 7: Release Locks (post-BIO)

Same three paths as EC:

- **Release**: Normal. RDMA CAS releases the lock.
- **Abandon**: After partial write (one replica written, the other not). Lock
  becomes stale, triggering recovery on the next access.
- **Transfer**: Fast-path handoff to the next IO targeting the same blockset.

---

## 5. Read Flow

Mirror reads are optimized to avoid lock acquisition entirely.

### 5.1 View Lock

Instead of acquiring the owner lock (RDMA CAS), a mirror read **piggybacks a
lock read** onto the data read command. The server executes both atomically:
read the data block, then read the lock value.

```
  View lock read:

  Client                    Server (Owner Segment)
    |                           |
    |-- RDMA Read: data blk --->|
    |   + piggyback: read lock  |
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

**Why view lock is correct:**

1. Reads always go to the **owner segment** — the authoritative source.
2. If a concurrent write is in progress, the lock is held → view lock sees it
   → client retries.
3. If the writer crashed, the lock is stale → recovery copies from the owner
   segment — the same data the reader already saw.
4. Therefore: the read is always consistent with eventual recovery outcome.

### 5.2 Segment Selection

`__get_role_of_read()` (`datapath_mirror/nvmeibc_block_dp_mirror.c:457`) selects
which segment to read from:

1. Default: the **owner segment** for this blockset (follows rotation).
2. Optional optimization: if a segment is on the same physical server as the
   client, prefer the local segment (skips RDMA network hop). Only enabled when
   topology is perfect (all segments RW).

---

## 6. Error Handling and Recovery

### 6.1 Abandon Decision

`dp_mirror_calc_should_abandon()` (`datapath_mirror/nvmeibc_block_dp_mirror.c:123`)
— after all write commands complete:

```
  Abandon decision tree:

  All writes succeeded?
    |
    Yes --> Normal lock release (RDMA CAS unlock)
    |
    No
    |
    Some writes succeeded + some not issued?
    |   (wr_not_issued > 0 && wr_succeeded > 0)
    |
    Yes --> FORCE_ABANDON (reason: PARTIAL_SLICE)
    |       One replica has new data, the other has old data.
    |       Lock becomes stale → recovery will sync from owner.
    |
    No
    |
    Any write failed?
    |   (wr_failed > 0)
    |
    Yes --> FORCE_ABANDON (reason: WRITE_FAILED)
    |       Write error → unknown state on disk.
    |       Lock becomes stale → recovery will verify + sync.
    |
    No --> Normal release (all were not-issued = retry)
```

### 6.2 Recovery Types

Mirror has three recovery operations, all simpler than EC's journal-based
hot-recovery:

**RECOVER_STALE** — Stale lock detected (previous holder crashed)

- The owner segment is authoritative. Read from owner, write to all others.
- Compare data across replicas; only write segments that differ.
- No transaction IDs or journal scanning needed.

**RECOVER_DB** — Dirty-bit recovery (segment returns after failure)
(`__mirror_sync_data_prepare_op()` —
`datapath_mirror/nvmeibc_block_dp_mirror_sync.c:403`)

- TOMA marks the returning segment as `W_IS_DIRTY` (assume all data stale).
- Background process iterates all blocksets:
  1. Acquire locks.
  2. Read from healthy segment(s).
  3. Write to recovering segment.
  4. Clear dirty bits.
  5. Release locks.
- When complete, segment is promoted to `RW`.

**RECOVER_READFAIL** — Inline read failure

- A read from one segment returns a permanent disk error.
- Try reading from the other mirror.
- If successful, write the good data back to the failed segment (repair).
- If both fail, return error to application.

### 6.3 Comparison: Mirror vs EC Recovery

| Aspect               | Mirror                 | EC                              |
| -------------------- | ---------------------- | ------------------------------- |
| Journal              | None                   | Write-ahead journal per segment |
| TxID                 | Not used               | 20-bit monotonic counter        |
| Recovery trigger     | Stale lock detection   | Stale lock + JMDC scan          |
| Authoritative source | Owner segment (always) | Journal (roll-forward)          |
| Roll-forward         | Copy owner → replicas  | Copy journal → data LBAs        |
| Dirty-bit storage    | RAM only (lock binfo)  | RAM + parity block MD           |

---

## 7. Comparison with EC

| Feature                | Mirror (RAID-1)              | EC (RAID-5/6)                                   |
| ---------------------- | ---------------------------- | ----------------------------------------------- |
| Data protection        | N identical copies           | k data + m parity                               |
| Write-hole             | None (no parity)             | Yes → journal-first protocol                    |
| Journal (JAM)          | Not used                     | Required for all writes                         |
| Transaction ID         | Not used (sentinel=6)        | 20-bit, incremented per write                   |
| JMDC                   | Not used                     | Server-RAM journal cache                        |
| Parity calculation     | None                         | GF(2^8) / XOR                                   |
| Planning structure     | None (direct)                | MSSA (2D bitmap, strategies)                    |
| Pre-read strategies    | None (except sub-block RMW)  | UPDATE/COMPLEMENT/RESTORE                       |
| Stages (typical write) | 1 (`DO_IO_AND_PAR`)          | 5+ (pre-read → calc → journal → data → cleanup) |
| Block MD usage         | Optional (EDIC only)         | Always (EDIC + TxID + JRI + D2J)                |
| Dbits computation      | Before lock (prepare_op)     | After lock (analyze binfo)                      |
| Lock types             | Owner (CAS) + Active (write) | Owner (CAS) + Copy-owner (write)                |
| Recovery mechanism     | Owner segment is source      | Journal roll-forward                            |
| Buffer sharing         | Both replicas share SGL      | Each segment gets unique data                   |
| Cookie reuse           | N/A (no journal)             | SEND_REL optimization                           |
| Piggyback stages       | 0-1 (`POST_IO_RDMA`)         | 2 (`POST_JR_RDMA` + `POST_IO_RDMA`)             |

---

## Appendix A: Code Reference Map

| Concept                            | Primary File(s)                                                                |
| ---------------------------------- | ------------------------------------------------------------------------------ |
| Mirror vtable init                 | `datapath_utils_generic/nvmeibc_block_dp_common.c` (case `MIR_BIO`)            |
| Mirror main flow                   | `datapath_mirror/nvmeibc_block_dp_mirror.c`                                    |
| Mirror sync/recovery               | `datapath_mirror/nvmeibc_block_dp_mirror_sync.c`                               |
| Mirror trim                        | `datapath_mirror/nvmeibc_block_dp_mirror_trim.c`                               |
| Stage enum                         | `datapath_utils_generic/nvmeibc_block_dp_io_generic_stages.h`                  |
| Lock types (`nvmeibc_rdma_intent`) | `common/nvmeib.h`                                                              |
| Lock acquire/release               | `datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c`                   |
| Lock transfer                      | `datapath_utils_generic/operation/nvmeibc_block_dp_operation_locks_transfer.c` |
| Block MD (`make_r1`)               | `datapath_utils_generic/nvmeibc_block_dp_block_md.h`                           |
| Dirty bits                         | `datapath_utils_generic/nvmeibc_block_dp_dbits.h`                              |
| Dbit piggyback (R1)                | `datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c`                    |
| Lock entry / binfo                 | `common/nvmeib_shared.h`                                                       |
| BIO completion                     | `datapath_utils_generic/nvmeibc_block_dp_operation.c`                          |
| Architecture overview              | `documentation/two_way_mirroring_overview.md`                                  |
