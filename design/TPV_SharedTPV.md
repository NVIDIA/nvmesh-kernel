# Shared TPV -- High-Level Design

> **Status: future consideration, not an implementation plan.** This document explores what it would take to support multi-client attach on a TPV and records the design tradeoffs that fall out of that exploration. It is intentionally high-level. It is **not** approved, scheduled, or scoped for implementation; the phase map, file lists, and test plan below are illustrative of the shape of the work, not commitments. Anyone picking this up as the basis for real work should expect to re-open every decision here against the current state of the codebase at that time.

**Scope.** Lift the single-client restriction on Thin-Provisioned Volumes (TPVs) so that one TPV can be attached concurrently to two or more clients in `SHARED_READ_WRITE` mode, enabling cluster file systems (OCFS2, GFS2), active/active failover, and live migration workloads that today either clone or disaggregate at a higher layer.

**Reference:** `TPV_ThinProvisioningImplementation.md` is authoritative for the existing single-client design. This document describes the *delta*; anything not called out here is unchanged.

**Dependencies:** per-CDV preemption (`TPV_PerClientCDVPreemption.md`) is live. The satellite allocator volume (`TPV_SatelliteVolumeForCDVAlloc.md`) is live. TPV encryption (`TPV_EncryptionPlan.md`) is live. Each of these makes an exclusivity assumption that this design reopens; Sec. 8 enumerates the interactions.

---

## 1. Problem statement

Today a TPV has exactly one holder at any moment.

- **Management** stamps `tpvConfig.exclusiveClient` / `exclusiveClientUUID` on the TPV document at attach time and refuses a second `attachTPV` until the current holder detaches. `updateTPV` rejects changes to `tpvConfig`; `deleteTPVs` requires `exclusiveClient === null` (`modules/volume.js`, `modules/client.js:attachTPV`).
- **Kafka/MCS attach** uses `reservation.mode = EXCLUSIVE_READ_WRITE` on the TPV `AttachVolumes` entry. TOMA's existing preempt mechanism refuses other registrants on an EXCLUSIVE volume.
- **Client kernel** assumes a single per-TPV in-memory allocator: an `xarray` at `nvmeibc_tpv_allocator.extent_map` mapping virtual extent index -> physical CDV offset, a free-slot list, a dirty L1/L2 tree, and a `persist_work` that rewrites the tree in place on the CDV (Sec. 3.4, `nvmeibc_tpv_allocator.c`, `nvmeibc_tpv_persist.c`).
- **TOMA CDV allocator** tracks extent ownership per `tpv_uuid` only. It has no notion of "this TPV is attached to client A, B, and C."
- **Encryption** is per-TPV LUKS whose init transiently attaches the TPV to a TOMA in EXCLUSIVE mode.

The combination means that attempting to attach the same TPV from a second client today is blocked at three places independently -- the Mongo stamp, the EXCLUSIVE reservation, and the client kernel's implicit assumption that *it* owns the mapping tree. Any one of these is sufficient to break a second writer; together they make the model invisible.

### 1.1 What "shared" must mean

Concretely, two writer clients on one TPV must observe:

- **Same virtual address space** -- a write at virtual offset `V` from client A must be visible at `V` from client B once the write is acknowledged (standard shared block device semantics; the cluster FS above is responsible for higher-level coherency).
- **Coherent physical mapping** -- if A allocates a fresh CDV extent for `V`, B must see the same `phys_offset` for `V` on its next access. There is no "split" state where A and B have independently allocated different physical slots for the same virtual extent.
- **Coherent free pool** -- a free CDV slot is held by exactly one client at a time. Two clients cannot independently pop the same slot.
- **Coherent DISCARD** -- a DISCARD from A that releases slot `s` must not race a write from B mapping `V` to `s`.

Reads of *mapped* extents are already trivially shared: the physical CDV is shared-RW today and clients both issue RDMA at the correct physical offset. The hard problem is mutation of the mapping tree and of the free-slot pool.

### 1.2 The tree-corruption hazard -- the load-bearing problem

The correctness concern dominating this design is **concurrent writes to the L1/L2 tree**. Today the tree lives in CDV data extents (Sec. 3.4.1 of the main design): slot 0 of the "L1 extent" holds the L1 header + entries; L2 tables are scattered across TPV-owned slots. The client writes these via `nvmeibc_tpv_flush_state` (`nvmeibc_tpv_persist.c`) using the CDV block layer -- ordinary RDMA writes to the CDV.

A naive shared-TPV design where two clients each flush their own `extent_map` produces three distinct failure modes:

1. **Lost updates.** Both clients hold dirty leaves for the same L2 page; whichever flushes last overwrites the other's leaves. The mapping for the overwritten leaf becomes unreachable -- the CDV extent is allocated from TOMA's point of view (`cdv_extent_md` persisted), but no L2 leaf references it. Orphan, silent data loss.
2. **Torn L1/L2 pointer updates.** Client A installs a new L2 table at `L1[i]`; client B, unaware, writes the L1 page from its stale cache where `L1[i]` is still null. The new L2 table becomes unreachable.
3. **Header/version rollback.** `tpv_l1_header.n_l2_tables_used` regresses when the slower writer lands last.

Even with a management-layer "one client is the designated writer" convention, the hazard is not resolved. A stale writer whose lease has transferred -- or that was never updated about the transfer -- still has its CDV attachment intact and can still issue an RDMA write into the tree-extent byte range. Nothing at the target refuses the write: the CDV is `SHARED_READ_WRITE`, so the target's preempt/admission check lets the write through. Generation fencing on RPC responses is useless here; the hazard is on the disk-write path, not on a response path.

The same problem appeared one level up for the CDV allocator (`<CDV>-mgmt` -- `TPV_SatelliteVolumeForCDVAlloc.md`). The fix there was to move the allocator metadata out of the CDV into a dedicated satellite volume held `EXCLUSIVE_READ_WRITE` by the current allocator TOMA. On re-election a standard preempt bumps the satellite's `reservation.version`, and the old allocator's disk writes are rejected at the target by the existing preempt admission check. No new target-side mechanism -- the feature is whole-volume `EXCLUSIVE_RW` semantics used at its native granularity on a volume whose purpose matches it exactly.

**This design reuses that pattern at TPV granularity.** Each TPV gains a per-TPV metadata satellite `<TPV>-meta` that holds the L1/L2 tree. Only the current Mapping Owner holds the `<TPV>-meta` attachment in `EXCLUSIVE_READ_WRITE`. MO failover is a preempt on the satellite: the new MO attaches at a higher `reservation.version`, and any in-flight tree write from the old MO is rejected at the target regardless of whether the old MO's kernel has yet processed the lease transfer. This is the only design choice that makes concurrent-write corruption structurally impossible rather than merely unlikely.

Sec. 3.4 specifies the satellite layout and fencing semantics. Sec. 4.2-Sec. 4.4 describe management orchestration (create, attach, handoff). Sec. 5.6 walks through the kernel-side handoff that exercises the preempt.

### 1.3 Non-goals

- **Cross-client file system coherency.** Out of scope. Users run a cluster FS, just as they would on any other shared block device.
- **Cross-client write ordering beyond the single-sector guarantees NVMesh already provides.** Unchanged.
- **Shared TPV across different `tpvExtentSizeKB` views.** Every client uses the same TPV geometry (this is already baked into the TPV document; clients read it from `AttachVolumes`).
- **Per-client views of a TPV** (e.g., client A sees a snapshot, client B sees live). That is a snapshot feature, handled separately.

---

## 2. Design space

Three shapes were considered. The tradeoff is *where the authoritative mapping tree lives* and *who pays the per-I/O coherency cost*.

### 2.1 Option A -- Client-side replicated tree with distributed locking

Every client keeps a full `extent_map` as today. A distributed lock manager (DLM) -- either Kafka-coordinated or a new side-channel -- serializes mutations. On an alloc, the mutating client takes a global lock on the virt_idx, adds the mapping to its xarray, flushes to the L1/L2 tree on CDV, and broadcasts an invalidation/update to peers.

- **Pro.** Reads and mapped-writes stay at their current cost (local `xa_load`, no RPC).
- **Con.** Every unmapped write, every DISCARD, and every persist flush takes a distributed lock. The broadcast channel is a new load-bearing system. Split-brain recovery is complex. For sparse workloads the DLM traffic is light; for active write workloads it rivals the CDV allocator traffic.
- **Verdict.** Reasonable only if "shared read, exclusive write per region" becomes a common case. Not the general answer.

### 2.2 Option B -- TOMA-authoritative mapping (recommended)

Move the L1/L2 mapping tree from the client to TOMA. TOMA becomes the single source of truth for `virt_idx -> phys_offset` and for the free-slot pool. The client holds a **read-through cache** of mappings it has accessed; writes to an unmapped extent take a new admin RPC (`TPV_MAP` or similar) instead of the current `CDV_ALLOC_EXTENT` (which is for pool refill). DISCARD becomes a `TPV_UNMAP` RPC.

Storage-side state moves to the satellite allocator volume alongside the existing `nvmeibt_cdv_alloc` data -- one per-TPV region per TPV, structured similarly to today's L1/L2 tree but owned and mutated by a single TOMA (the CDV allocator TOMA for this CDV, which is already RAFT-elected and preempt-protected via the satellite).

- **Pro.** Coherency is trivial: all mutation goes through one owner. No DLM, no cross-client invalidation. Failover follows the existing allocator election. The client's per-TPV kernel state shrinks dramatically (no persist workers, no dirty-page bitmaps, no `load_state_work`, no L1 dirty tracking).
- **Con.** One admin-RPC per newly-allocated extent on every client (not just the first one). Hot-path unmapped-write latency gains one RPC round-trip. Cold-cache mapped reads gain one RPC round-trip. Aggregate RPC load on the allocator TOMA scales with the number of writer clients; with N clients doing sparse writes, the existing client->TOMA CDV_ALLOC traffic becomes a per-client TPV_MAP traffic -- an order of magnitude more messages.
- **Verdict.** Correct long-term answer. The client becomes a thin shim; ownership lives where it can be serialized cheaply (already-RAFT-consistent TOMA allocator); all the existing preempt / failover machinery transfers.

### 2.3 Option C -- Mapping-Owner-Client (MOC) lease

Exactly one attached client at a time is the "mapping owner" for the TPV. It runs today's tree logic unchanged. Non-owner clients run a thin client that, on `xa_load` miss or on a write to an unmapped extent, RPCs the owner (`TPV_LOOKUP` / `TPV_ALLOC`) via a new client<->client channel routed through TOMA. The owner serves, mutates its tree, persists via today's path, and responds. On owner detach or failure, the lease transfers -- TOMA elects a new owner among the remaining registrants and delivers the current mapping to it via a `TPV_HANDOFF` message.

- **Pro.** Zero change to the persist/recovery path. Minimal change to TOMA -- just registrant tracking (already present) and a lease election. The hot path for the owner is unchanged; non-owners pay an RPC on miss but cache the result, so after warmup the RPC rate drops to "new-allocation only."
- **Con.** Asymmetric performance -- the owner is faster than everyone else. Owner failover is a visible pause. The new client<->owner channel is load-bearing but narrow (single-message RPC). Split workloads where every client writes different regions see heavy RPC traffic.
- **Verdict.** Best pragmatic near-term answer. Smaller blast radius on existing code than Option B; same asymptotic correctness properties.

### 2.4 Recommendation

Ship **Option C first**, then evaluate whether the asymmetry and owner-failover cost justifies the larger Option-B migration. The satellite allocator work showed that moving metadata off the client is achievable but invasive; it is not worth that cost unless real workloads demand it.

The rest of this document describes **Option C** unless explicitly noted.

---

## 3. Architectural overview (Option C)

```
                              +----------------------------------+
                              |           CDV allocator          |
                              |              (TOMA)              |
                              |  CDV_ALLOC_EXTENT / FREE / LIST  |
                              +----------------------------------+
                                 ^ IB admin       ^ IB admin
                                 | (owner only)   | (owner only)
                                 |                |
                +----------------+---+       +----+---------------+
                |  Client A          |       |  Client B          |
                |  Mapping Owner     |<----->|  Mapping Follower  |
                |  (has full tree)   | TPV_* |  (cache only)      |
                |  xarray, persist   |  RPC  |  xarray = cache    |
                |  L1/L2 flush       |       |                    |
                +--------------------+       +--------------------+
                                 ^                 ^
                                 |                 |
                                 | RDMA data plane |
                                 v                 v
                              +----------------------------------+
                              |              CDV                 |
                              |  data extents [0, end)           |
                              |  (user data only; no tree here   |
                              |   -- tree is persisted by owner) |
                              +----------------------------------+
```

- One attached client per TPV is the **Mapping Owner (MO)**. It runs every function in today's `nvmeibc_tpv_persist.c`, `nvmeibc_tpv_allocator.c`, `nvmeibc_tpv_ib_admin.c` unchanged. It drives `cdv_alloc_work`, owns `pending_return_list`, issues `CDV_ALLOC_EXTENT` / `CDV_FREE_EXTENT` to TOMA, and persists the L1/L2 tree.
- Every other attached client is a **Mapping Follower (MF)**. Its `struct nvmeibc_tpv` has the same shape but its `extent_map` is a best-effort cache, not authoritative. MFs never call `CDV_ALLOC_EXTENT`, never drive `persist_work`, never run `flush_state`.
- On a write to an unmapped extent, an MF issues a new `TPV_ALLOC_V` RPC to the MO via a client->client channel. The MO runs its normal `nvmeibc_tpv_alloc_extent` path (which may internally trigger a pool refill via `CDV_ALLOC_EXTENT`), persists the tree, and replies with the assigned `phys_offset`. The MF stores it in its own xarray and proceeds.
- On an MF `xa_load` miss (cold cache for an already-mapped extent), the MF issues `TPV_LOOKUP_V` and the MO returns the phys_offset without mutating state.
- On an MF DISCARD, the MF issues `TPV_UNMAP_V`. Only the MO mutates its tree and returns the slot to the pool.
- The MO publishes tree mutations lazily to MFs via `TPV_INVALIDATE_V` broadcast (batched). MFs that receive an invalidation for a virt_idx currently mapped in their cache evict the entry. Subsequent accesses re-miss and look up again.

### 3.1 Owner lifecycle

The MO role is a lease held by one of the attached clients. It is managed by management (not by TOMA RAFT) because TPV attachment lifecycle is already a management-coordinated flow and because the MO set equals the TPV's registrant set (which only management knows).

- **Election.** When the first client attaches a TPV in SHARED mode, management picks it as MO. Subsequent attaches are MF.
- **Lease record.** Stored in the TPV document: `tpvConfig.mappingOwnerClientID`, `tpvConfig.mappingOwnerClientUUID`, `tpvConfig.mappingOwnerGeneration` (monotonic).
- **Transfer.** On MO detach, management elects a new MO from the remaining registrants *before* the detach completes on the kernel side. The old MO's kernel module flushes its tree (`flush_state` synchronously), snapshots the in-memory state (extent_map entries, free_tpv_extents, cdv_extent_list, pending_return_list), and forwards it in a `TPV_HANDOFF` message to the incoming MO. The incoming MO rebuilds `struct nvmeibc_tpv_allocator` from the snapshot (equivalent to running `load_state` + `recovery` but from an in-memory source instead of from CDV + TOMA_LIST).
- **Failure.** If the MO dies or is preempted, management detects the detach on MongoDB (attachment removal by stale-client cleanup or preempt eviction) and promotes one of the surviving MFs. The new MO runs `load_state` + `recovery` from scratch -- the same path used at a cold attach today -- because no handoff snapshot was received.
- **Generation fencing.** Every MO<->MF RPC carries the MO generation. A response or invalidation from a stale MO is discarded by the MF (same pattern as the existing CDV allocator generation fencing).

### 3.2 The new RPC surface

Three new messages, all piggybacking on the existing client<->TOMA IB admin channel with client-to-client relay:

| Message | Direction | Payload | Response |
|---|---|---|---|
| `TPV_ALLOC_V` | MF -> MO | `(tpv_uuid, virt_idx, mo_generation)` | `(phys_offset, cdv_extent_index, mo_generation)` or error (`NO_MO`, `CDV_FULL`, `BELOW_CDV_FLOOR`) |
| `TPV_LOOKUP_V` | MF -> MO | `(tpv_uuid, virt_idx, mo_generation)` | `(phys_offset, cdv_extent_index)` or `NULL` if still unmapped |
| `TPV_UNMAP_V` | MF -> MO | `(tpv_uuid, virt_idx_start, virt_idx_count, mo_generation)` | ACK |
| `TPV_INVALIDATE_V` | MO -> MFs (broadcast) | `(tpv_uuid, [virt_idx...], mo_generation)` | fire-and-forget |
| `TPV_HANDOFF` | MO -> new MO (via mgmt) | tree + free pool snapshot | ACK |

**Routing.** Today the client->TOMA IB admin channel uses the CDV segment disk as the transport target (`cdv_find_segment_for_toma` in `nvmeibc_tpv_ib_admin.c`). For MF->MO we extend TOMA to forward `TPV_*_V` messages to the current MO's client UUID. TOMA is already the natural rendezvous -- it tracks every registrant on the CDV and therefore knows how to reach every attached client. Adding a "forward to this client" opcode is a small extension to the existing admin-message handler.

**Why not client-to-client direct.** NVMesh clients today have no direct RDMA path to each other; they only talk to targets. Building a new client<->client transport is a large lift for a narrow need. Relaying through TOMA reuses the existing IB admin channel, costs one extra hop on the miss path (invisible compared to the CDV I/O that follows), and keeps the trust boundary clean -- TOMA is already the shared trust root for CDV operations.

### 3.3 Degradation to exclusive

When only one client is attached, the MO is that client, no RPC traffic ever fires, and the system reduces exactly to today's behavior. SHARED with one registrant is indistinguishable in the hot path from EXCLUSIVE. This is intentional: it means the feature adds no regression risk for existing workloads.

### 3.4 `<TPV>-meta` satellite -- the tree-write fencing mechanism

Per Sec. 1.2, the L1/L2 tree **cannot** stay in CDV data extents under a shared-TPV model: there is no target-side mechanism today that refuses a tree-extent RDMA write from a client whose MO lease has transferred, because the CDV is `SHARED_READ_WRITE` and the write offset is indistinguishable from a user-data write. Management-layer lease tracking and kernel-side RPC-generation fencing address ownership of the *allocation* decision; neither stops the *disk write* that follows from stale in-memory state.

The design moves the tree onto a dedicated per-TPV satellite volume named `<TPV>-meta`, held `EXCLUSIVE_READ_WRITE` by the current MO. This is the identical construction that `TPV_SatelliteVolumeForCDVAlloc.md` uses to solve the analogous problem one level up (stale-allocator CDV-allocator writes).

**Layout.**

- `<TPV>-meta` is a small fixed-size volume (sized to the worst-case tree footprint for that TPV's `tpvExtentSizeKB` and `maxVirtualSizeGB`: one L1 slot of `T` bytes plus up to `N_L1` L2 tables of `T` bytes each; a 1 TiB TPV at 64 KiB extents needs ~4 MiB, sized conservatively to 16 MiB and padded to an NVMesh allocation unit).
- Written exclusively by the MO. The CDV's data extents hold **only user data** for shared TPVs -- no tree, no L1 header, no L2 tables.
- The L1 extent ownership reservation (`is_l1_extent` on `nvmeibc_cdv_extent_ref`) collapses for shared TPVs: the "first data CDV extent" loses its special slot-0 role. Address math for data extents therefore simplifies -- every TPV-owned CDV slot is a data slot, indexed directly.
- `<TPV>-meta` is its own volume document (`volumeClass: 'TPV_META'`), co-allocated with the TPV at TPV-create time using the `allocateAndSliceIntoVolumes` primitive already shipped for the CDV satellite. One allocator pass, one rollback path, same Mongo atomicity discipline.
- `volumeClass: 'TPV_META'` is refused by every user-facing REST path (attach, detach, delete, update, show with default filter). Only the internal `attachTPVMetaForMO` / `detachTPVMetaForMO` helpers may touch it.

**Fencing.**

MO re-election on a shared TPV is a standard NVMesh preempt on `<TPV>-meta`:

1. Management bumps `<TPV>-meta.reservation.version` via the existing preempt attach primitive (same code path `attachSatelliteForAllocator` uses for the CDV allocator satellite).
2. The incoming MO's `AttachVolumes(<TPV>-meta)` lands with the new version.
3. Any in-flight tree write from the old MO arrives at the target with the previous version; the target's standard `reservation_mode_version` admission check rejects it with `RESERVATION_PREEMPTED`. The old MO's kernel bubbles that up as `NCBD_PREEMPTED` on the satellite's gendisk and runs its own tree-writer teardown -- no new reason code, no new target admission path.

Exclusive-mode TPVs (`shared === false`) do **not** have a `<TPV>-meta` satellite. They keep today's in-CDV tree layout (Sec. 3.4 of the main design) verbatim. The satellite is an artifact of the shared-attach mode only. This keeps the storage overhead bounded: legacy deployments adopting the new code pay zero extra volumes until they explicitly opt into shared attach.

**Read path on MFs.** MFs never attach `<TPV>-meta`. They never read the tree directly -- they go through the MO via the RPC surface (Sec. 3.2). The satellite is MO-private.

**Storage overhead.** One extra volume per shared TPV. For a deployment with 1 000 shared TPVs this is 1 000 small satellites -- comparable to the CDV-satellite overhead of 1 per CDV at ~512x fewer volumes than the TPV-satellite count in a worst-case ratio. Operational tools must learn to filter `volumeClass in {CDV_MGMT, TPV_META}` by default.

**Why not keep the tree in the CDV and invent per-region exclusivity.** Same reasoning as Part 1.5.3 of the main design: a per-offset-range admission version inside the target hot path is a new load-bearing target extension with no standalone benefit. Satellite-plus-preempt uses the mechanism at its native granularity on a purpose-built volume.

**Why not move the tree to TOMA (Option B in Sec. 2).** Still viable as a future direction, but Option B also requires *somewhere* to persist the tree -- the natural place is still a satellite. Option B and Option C differ only in *who holds the satellite* (a TOMA vs. a client). Shipping Option C first with the satellite in place means Option B becomes a drop-in replacement later: migrate the MO role from client to TOMA, reuse the same on-disk format on the same satellite volume.

---

## 4. Management layer changes

### 4.1 Schema

`tpvConfig` gains:

```js
tpvConfig: {
    // existing fields unchanged
    shared:                { type: Boolean, default: false },      // NEW -- attach mode
    mappingOwnerClientID:  { type: String, default: null },         // NEW
    mappingOwnerClientUUID:{ type: String, default: null },         // NEW
    mappingOwnerGeneration:{ type: Number, default: 0 },            // NEW monotonic
    // exclusiveClient / exclusiveClientUUID are retained but null for shared TPVs
}
```

`shared` is set at create time and is immutable (changing the attach mode of a live TPV is not supported -- detach, update, re-attach is the operator-visible flow).

`exclusiveClient` is retained unchanged for `shared=false` TPVs -- the exclusive flow is untouched. For `shared=true` TPVs, the field is always null; the MO is tracked separately via the new fields.

### 4.2 TPV create -- satellite co-allocation

`createTPV` in `modules/volume.js` branches on the request payload's `shared` flag:

- `shared === false` (default): unchanged -- a plain TPV insert.
- `shared === true`:
  1. Compute the satellite size from `tpvConfig.tpvExtentSizeKB` and `tpvConfig.maxVirtualSizeGB` (worst-case tree footprint plus padding; concrete formula in Sec. 3.4).
  2. Build a slice list `[ { name: '<tpv>-meta', volumeClass: 'TPV_META', size }, { name: '<tpv>', volumeClass: 'TPV', size: 0 /* TPV has no own chunks */ } ]`. TPV satellites are allocated on the same chunk pool as the parent CDV by default -- but they are independent volumes from an allocator perspective; the satellite is a regular thin-provisioned volume, not a slice of the CDV.
  3. The satellite gets its own small chunk plan sized for the tree. Placement constraint: prefer the first pRAID of the parent CDV so the satellite is reachable from the same TOMA set that already serves the CDV.
  4. Cross-reference the two records (`tpv.tpvConfig.metaVolumeId`, `tpv.tpvConfig.metaVolumeUUID`; `meta.parentTPVId`, `meta.parentTPVUUID`) before `insertMany`.
  5. Emit `volumeInventoryChanged` for the TPV only (satellite is not user-visible).
  6. On create failure, both documents are rolled back and the satellite's chunks released -- same atomicity discipline as CDV+`<CDV>-mgmt` create.

TPV naming: enforce `tpv.name.length <= 16` for shared TPVs so `<tpv>-meta` fits NVMesh's name-length limit. `-meta` becomes a second reserved suffix alongside `-mgmt`.

### 4.3 `attachTPV` changes

`attachTPV(clientID, tpvID, opts)` in `modules/client.js` branches on `tpv.tpvConfig.shared`:

- **`shared === false` (today's path).** Unchanged. EXCLUSIVE_RW, `exclusiveClient` gating, current preempt semantics on reattach.
- **`shared === true`.** New path:
  1. The CDV hidden-attach and `attachCDVToAllTomaNodes` steps are identical.
  2. The TPV `AttachVolumes` payload uses `reservation.mode = SHARED_READ_WRITE` instead of EXCLUSIVE.
  3. **MO election and `<TPV>-meta` preempt attach.** If `mappingOwnerClientID === null`, this client becomes the MO. Management bumps `mappingOwnerGeneration`, then calls `attachTPVMetaForMO(metaUUID, clientID, newGeneration, requestId)` -- a new internal entry point that wraps the standard exclusive-preempt attach path with `reservation.mode = EXCLUSIVE_READ_WRITE`, `preempt = true`, `isDetachOthers = true`. This lands the satellite on the client at a strictly higher `reservation.version`, fencing any previous MO's tree writes at the target.
  4. If `mappingOwnerClientID !== null`, this client becomes an MF -- no `<TPV>-meta` attach for MFs. Include `{mo: false, mo_client_uuid, mo_generation}` in the TPV `AttachVolumes` payload so the kernel can route RPCs.
  5. The TPV `AttachVolumes` payload carries `{mo, mo_client_uuid, mo_generation, meta_uuid}`. `meta_uuid` is populated for MO and MF alike so an MF promoted on failover can locate the satellite.
  6. No `exclusiveClient` stamp.

### 4.4 `detachTPV` changes

- **`shared === false`.** Unchanged.
- **`shared === true`.**
  1. If the detaching client is an MF: regular detach. No lease change, no satellite preempt.
  2. If the detaching client is the MO **and** other registrants remain: run `electNewMO(tpv)` before the kernel-side detach. The flow is: management sends Kafka `TPVHandoffRequest` to the old MO; the old MO's kernel flushes the tree, serializes the snapshot, replies with `TPVHandoffSnapshot`; management calls `attachTPVMetaForMO(metaUUID, newMO, newGeneration)` to preempt the satellite onto the new MO *before* delivering the snapshot -- so if the old MO's flush was not yet complete, the satellite preempt fences any late tree write. The new MO then receives `TPVHandoffDeliver` with the snapshot and transitions MF->MO under the new generation. Only after the new MO ACKs does `DetachVolumes(TPV)` for the old MO go out. `mappingOwnerGeneration` bumps.
  3. If the detaching client is the MO **and** it is the last registrant: normal detach; the tree on `<TPV>-meta` is the last state. The satellite stays attached to nobody.

Failure handling for handoff: if the old MO fails to respond within a bounded timeout, management skips the snapshot path, preempts the satellite onto a surviving MF via `attachTPVMetaForMO`, and instructs it to run `load_state`+`recovery` from scratch against `<TPV>-meta` instead of the CDV. The new MO's `mappingOwnerGeneration` is bumped by two (so any in-flight RPC tagged with the intermediate value is rejected by both sides). Because the satellite preempt at the target already fenced the old MO's writes, there is no window in which both the old and new MO can successfully write the tree -- even if the old MO is still up and its kernel has not yet processed the detach.

### 4.5 `updateTPV` / `deleteTPVs` / `extendTPV`

- `deleteTPVs` gates on "all registrants detached" instead of "`exclusiveClient === null`." For `shared=true` TPVs that means the registrant count on the CDV's `tpv:<tpvUUID>` refs must be zero. The existing `tpv:*` referenceID tracking on `client.attachments[cdvUUID]` is the source of truth. After the TPV document is removed, the `<TPV>-meta` satellite is deleted in the same atomic step (same double-delete discipline as CDV + `<CDV>-mgmt`).
- `extendTPV` currently sends `UpdateVolumes` to the exclusive client. For shared TPVs it must broadcast `UpdateVolumes` to all registrants so every client's gendisk capacity grows consistently.
- `updateTPV` remains mutable only on `description`; the new fields (`shared`, `mappingOwnerClientID`, `metaVolumeId`) are immutable via user REST paths.

### 4.6 Encryption interaction

`attachTPVToTOMAForEncryption` (from `TPV_EncryptionPlan.md`) borrows the TPV EXCLUSIVE to let a TOMA run `cryptsetup`. This is incompatible with a TPV that currently has multiple MFs writing to it.

For `shared=true` TPVs the encryption flow is gated: `initEncryption` is accepted only when `registrants.length === 0` (TPV is detached from every client). `addPassphrase`/`rotatePassphrase`/`deletePassphrase` have the same gate. Operator-visible contract: passphrase-ops require full quiesce on shared TPVs. Attempts while attached return `TPV_SHARED_ATTACHED_CANNOT_ENCRYPT`. Documented in the CSI driver's encrypted-TPV StorageClass (must not combine `shared=true` with `encryption=dmcrypt` unless the workload quiesces around passphrase ops).

A future refinement -- a synchronized quiesce ("pause all MFs, run cryptsetup, resume") -- is possible but out of scope. LUKS format writes at offset 0 do not race user I/O if all registrants are quiesced.

### 4.7 Per-client CDV preemption interaction

Today's per-CDV admission floor preempts one client's `reg_ctx` on the CDV. For a shared TPV this still works: preempting client A from CDV `cdvC` tears down client A's CDV attachment, which triggers `nvmeibc_tpv_handle_cdv_preempted` on A for every TPV it rides. If A was an MF, other MFs and the MO are unaffected. If A was the MO, the preempt is a management-visible MO failover -- management notices the A detach, elects a new MO from the surviving registrants, and the new MO runs the cold `load_state`+`recovery` path. This is the same code path as an MO crash and is exercised by the failover test plan (Sec. 7).

**New case.** An operator may want to preempt a *specific client* from a *specific shared TPV* without touching the CDV (e.g., "evict client A from TPV X but leave it on the other TPVs of the same CDV"). The existing `preemptClientFromCDV` primitive is CDV-scoped and too coarse for this. A new `preemptClientFromTPV(tpvUUID, clientID)` operation is added:

- Management removes the client's `tpv:<tpvUUID>` reference, runs MO election if necessary, then sends a targeted `DetachVolumes(TPV)` to that client. If the client is unreachable, the admission-floor primitive is reused with a new per-TPV floor (stored on `tpvConfig.admissionFloor`, bumped per eviction, seeded into the kernel via `reservation_mode_version` on the TPV `AttachVolumes` -- not the CDV attach). TOMA's CDV-allocator TOMA rejects TPV registrants below the TPV floor with a new `BELOW_TPV_FLOOR` reason code; the client-kernel maps that to a TPV-only teardown (not a CDV teardown -- the CDV stays up for the client's other TPVs).

This mirrors the per-CDV design at a narrower scope. Sec. 8 walks through the invariants.

---

## 5. Client kernel changes

### 5.1 Role in `struct nvmeibc_tpv`

Add to `nvmeibc_tpv.h`:

```c
enum nvmeibc_tpv_role {
    TPV_ROLE_EXCLUSIVE = 0,   /* today's behavior, shared == false */
    TPV_ROLE_MO        = 1,   /* Mapping Owner for a shared TPV */
    TPV_ROLE_MF        = 2,   /* Mapping Follower for a shared TPV */
};

struct nvmeibc_tpv {
    /* ... existing fields ... */
    enum nvmeibc_tpv_role  role;
    char                   mo_client_uuid[NVMEIBC_CLIENT_UUID_LEN];  /* MF only */
    u64                    mo_generation;
    spinlock_t             mo_identity_lock;
};
```

`role` is set at attach time from the `{mo, mo_client_uuid, mo_generation}` fields in the `AttachVolumes` payload (Sec. 4.2) and updated by topology pushes on MO failover. `mo_identity_lock` fences in-flight MF->MO RPCs across a failover the same way `allocator_id_lock` fences CDV allocator RPCs today.

### 5.2 IO path

`nvmeibc_tpv_make_request` / `tpv_handle_one_bio` (`nvmeibc_tpv_io.c`) gets one branch at each mutation site:

- **`xa_load` hit (mapped R/W).** Unchanged for all roles -- forward to CDV at `phys_offset`. This is the common case.
- **`xa_load` miss + READ.** Unchanged for all roles -- zero-fill (no CDV I/O). Note that an `xa_load` miss in an MF does not mean "unmapped on the TPV"; it can mean "not yet cached." To distinguish, the MF issues a `TPV_LOOKUP_V` RPC before zero-filling a read. Optimization: skip the lookup for read-before-any-write workloads where the "assume unmapped" result is identical. The implementation performs the lookup lazily -- the read is zero-filled immediately and the lookup fires asynchronously to populate the cache; subsequent reads hit. For workloads that care about consistency with writes from other clients the cache-populating lookup must come first. A per-TPV `aggressive_lookup` toggle (default: true, prioritize correctness) controls this.
- **`xa_load` miss + WRITE.** Role-dependent:
  - **EXCLUSIVE / MO.** Unchanged -- call `nvmeibc_tpv_alloc_extent`, which pops from `free_tpv_extents` locally and, if below watermark, refills via `CDV_ALLOC_EXTENT`.
  - **MF.** Call a new helper `nvmeibc_tpv_alloc_extent_via_mo`, which issues `TPV_ALLOC_V` to the MO, waits on a completion, installs the returned `(phys_offset, cdv_extent_index)` in the local xarray via `xa_store`, and falls through to the mapped-write path. On `NO_MO`/`WRONG_MO_GEN` the MF retries after the `TPV_OWNER_UPDATE` topology push arrives (same pattern as `CDV_ALLOC_WRONG_GEN` today). On `CDV_FULL` or `BELOW_CDV_FLOOR` the bio is failed.
- **DISCARD.** Role-dependent:
  - **EXCLUSIVE / MO.** Unchanged.
  - **MF.** Issue `TPV_UNMAP_V` to the MO for the covered virt_idx range; on ACK, `xa_erase` locally and complete the bio.

### 5.3 Allocator state on an MF

An MF's `struct nvmeibc_tpv_allocator` exists but most of its fields are dead:

- `extent_map` -- alive, used as a cache.
- `cdv_extent_list`, `free_tpv_extents`, `pending_return_list`, `cdv_extents_count`, `free_tpv_extent_count`, `low_watermark` -- **not used**. MFs never allocate CDV extents.
- `l1_to_l2_ctx`, `l1_dirty_pages`, `l1_extent_index` -- **not used**. MFs never persist.
- `stat_*` counters -- alive but only the lookup/cache-specific ones are incremented (new `stat_mo_alloc_ok`, `stat_mo_lookup_ok`, `stat_mo_unmap_ok`, and `stat_mo_rpc_err`).

Compile-time cost: a few kbytes of dead fields per MF. Runtime cost: zero -- the MF never touches the dead paths.

### 5.4 Work items

- `cdv_alloc_work` -- MO only. MFs initialize but never schedule it.
- `persist_work`, `load_state_work`, `timeout_work` -- MO only. MFs leave them uninitialized.
- New: `mo_rpc_timeout_work` on MFs, firing when `TPV_ALLOC_V`/`TPV_LOOKUP_V`/`TPV_UNMAP_V` stalls beyond the admin-channel timeout. It fails the parked bios with `-EIO` (same timeout shape as `cdv_alloc_work` failure today).

### 5.5 Invalidations on MFs

`TPV_INVALIDATE_V` from the MO is received via the same IB admin dispatch path as the existing `CDV_ALLOCATOR_UPDATE` (`nvmeibc_topology.c`). The handler:

1. Validates `mo_generation` -- stale invalidations dropped.
2. For each `virt_idx` in the payload: `xa_erase`, `kfree_rcu` the entry.

The MO batches invalidations (default: 64 entries or 100 ms) and fires a single broadcast per batch to keep RPC traffic bounded. The MO includes an invalidation in every `TPV_UNMAP_V` ACK it sends, so UNMAPs from one MF are seen by the others on the next RPC even without a broadcast -- invalidations are primarily needed for *MO-internal* mutations (DISCARD-like scans from NVCK, extent migration, etc.) that don't originate from an MF RPC.

### 5.6 MO failover on the kernel side

The on-disk fencing invariant (Sec. 3.4) is that exactly one MO at a time holds the `<TPV>-meta` attachment in EXCLUSIVE_RW at the current `reservation.version`. All kernel-side steps below depend on the management-orchestrated satellite preempt for correctness; the snapshot transfer is a latency optimization, not a safety mechanism.

**On the losing MO (graceful detach path).**

1. Block new MF RPCs -- the MO sets a draining flag and replies to in-flight RPCs with `MO_HANDOFF_IN_PROGRESS`.
2. `flush_state` synchronously to `<TPV>-meta`. If the satellite has already been preempted (step 4 below reordered, or a racing `preemptClientFromTPV`), the flush fails at the target with `RESERVATION_PREEMPTED` and the outgoing handoff is marked best-effort -- tree state on the satellite may be slightly stale; the incoming MO's `load_state`+`recovery` reconciles.
3. Serialize the live in-memory state: `extent_map` contents (sparse -- an iterator over non-null xa entries), `cdv_extent_list` entries, `free_tpv_extents`, `pending_return_list`. This is a cache-warming optimization -- the authoritative data is on the satellite.
4. Emit `TPV_HANDOFF` with the serialized snapshot. On ACK from the new MO, proceed with `nvmeibc_tpv_detach` and release the `<TPV>-meta` attachment.
5. If management has already preempted the satellite before step 2 (the reverse order is also acceptable and is chosen by the `preemptClientFromTPV` path in Sec. 4.7), the losing MO observes `NCBD_PREEMPTED` on the satellite's gendisk, fails any parked tree-flush bios with `-EIO`, and proceeds to step 3.

**On the incoming MO (graceful path).**

1. Management calls `attachTPVMetaForMO(metaUUID, newClient, newGeneration)`. The kernel attaches `<TPV>-meta` EXCLUSIVE_RW at `reservation.version = newGeneration`. *This is the fencing event* -- from here, any tree write from the old MO is rejected at the target. No other step in the failover path relies on the old MO's cooperation for safety.
2. Receives `TPV_HANDOFF` (management-relayed via Kafka -> client MCS channel -- handoff is a management-plane event, not a hot-path IB admin event).
3. Materializes `extent_map`, `cdv_extent_list`, `free_tpv_extents`, `pending_return_list` from the snapshot into the MF's pre-existing structures.
4. Transitions `role` from MF to MO atomically under `mo_identity_lock` (MF-mode RPCs issued before the transition complete under the old generation and are drained).
5. Schedules `persist_work` to re-persist the L1/L2 tree under its own recovery context (the tree on the satellite should match the snapshot; this is a defensive rewrite that clears `dirty = false` cleanly).

**On the incoming MO (failure path, cold).**

If no handoff arrives (old MO died), management calls `attachTPVMetaForMO` to preempt the satellite onto a surviving MF, then the new MO runs `load_state` + `recovery` from scratch -- *but reads from `<TPV>-meta` instead of from CDV data extents*. `nvmeibc_tpv_recovery` still reconciles against `CDV_LIST_EXTENTS` via the existing RPC. Because the satellite preempt happened first, the new MO's reads of `<TPV>-meta` cannot race with writes from the old MO -- the old MO's writes are rejected at the target.

**Why the satellite preempt is sufficient for *tree* fencing even without a handoff.** Two concurrent kernel MOs can exist at the management layer for a narrow window (old MO's kernel hasn't yet processed the detach / preempt notification). But neither can successfully write the tree: the old MO's tree writes are rejected by the target because `<TPV>-meta.reservation.version` is stale, and the new MO is the sole accepted writer.

**Tree fencing vs data fencing are orthogonal.** The `<TPV>-meta` preempt fences tree writes only. User-data writes still flow from every registrant over the CDV's shared-RW attachment, fenced at CDV granularity by the existing `preemptClientFromCDV` / per-client CDV admission floor. The dangerous case -- the old MO's in-flight *data* write lands on a CDV slot that the new MO has since reassigned to a different virt_idx -- is handled by two independent invariants:

1. **Graceful handoff.** `nvmeibc_tpv_detach` already drains `io_inflight` to zero before releasing the atom (Sec. 11.7 of the main design). The losing MO in a graceful handoff completes its in-flight data I/O before signalling handoff ACK, so no data write from the old MO can outrace the `<TPV>-meta` preempt.
2. **Cold / partition handoff.** If the old MO's kernel is unresponsive, management escalates to `preemptClientFromCDV` on the old MO before reassigning freed slots. That is the existing mechanism for severing a client's data-plane access. The MO election is decoupled from CDV preempt in the normal case (to avoid disrupting a MO that is cleanly detaching), but the escalation path is available and is invoked whenever cold-handoff timeouts expire without a data drain confirmation from the old MO.

A **slot-reuse grace window** reinforces this: when the new MO frees a CDV extent (via `CDV_FREE_EXTENT`) or moves a slot to `free_tpv_extents`, the slot is not immediately reassignable to a new virt_idx. It enters a per-TPV cooling queue with age equal to at least one MO-handoff drain interval. The old design has no equivalent; for shared TPVs this is a load-bearing new invariant, and TPV self-tests must cover the DISCARD-reuse-race explicitly.

### 5.7 IO during handoff

MF bios parked on `pending_bios` during the handoff window see the failed RPC responses and re-park themselves, retrying under the new `mo_generation` once the `TPV_OWNER_UPDATE` topology push arrives (same pattern as CDV-allocator-generation handling today). The `timeout_work` limits the park duration; if handoff exceeds the timeout, parked bios fail with `-EIO`. Happy-path handoff is expected to complete in tens of milliseconds (tree snapshot is `O(allocated_extents)` bytes -- a few hundred kB for a TB-class TPV).

---

## 6. TOMA changes

Minimal compared to the satellite work or per-CDV preempt.

1. **Message forwarding.** `nvmeibt_kafka.c`'s IB admin dispatch learns a new "forward to client" opcode: given `(target_client_uuid, payload)`, find the registrant record and send the payload over that client's transport. Already a supported primitive -- TOMA today routes `CDV_ALLOCATOR_UPDATE` to all registrants. Adding a per-UUID routing variant is small.
2. **Optional MO visibility for observability.** A new per-TPV hash entry that records `{mapping_owner_client_uuid, mapping_owner_generation}` allows `/proc/nvmeibt/tpv/<uuid>/owner` to reveal the current MO. Advisory only -- TOMA does not enforce MO status; management does. The value is useful for debugging.
3. **Admission floor per TPV.** If the per-TPV preempt operation is shipped (Sec. 4.6), `nvmeibt_seg_active` gains a secondary floor check at TPV registration: the incoming `AttachVolumes` `reservation_mode_version` is compared against `tpv_admission_floor` (a new per-TPV field populated from the admin-floor Kafka push). Refusal reason: `BELOW_TPV_FLOOR`. Unchanged for CDV registrations.

No changes to `nvmeibt_cdv_alloc.*`. The CDV allocator is unaware of MO vs MF -- it sees the MO's `CDV_ALLOC_EXTENT` and `CDV_FREE_EXTENT` traffic exactly as it sees an exclusive client's traffic today.

---

## 7. Testing strategy

The test matrix is three-dimensional: role (MO/MF), operation (read/write/discard/grow/detach), and event (preempt, handoff, crash). Highlights:

- **Two-writer golden path.** Two clients, one TPV mounted as OCFS2. Run `fio --rw=randwrite` simultaneously from both clients. Verify: (a) no data corruption, (b) MO's RPC rate matches the fresh-allocation rate observed by its `CDV_ALLOC_EXTENT` counter, (c) MF's RPC rate decays to zero on a stable working set.
- **Graceful MO failover under load.** Two clients writing. Detach MO cleanly. Assert: MF's I/O pauses for < 100 ms, no failures, new MO resumes and serves further allocations correctly.
- **Cold MO failover.** Two clients writing. Kill MO (network partition). Assert: MF I/O pauses until `BELOW_CDV_FLOOR` or timeout-driven retry fires; management promotes surviving MF; new MO's `load_state`+`recovery` reconstructs the tree; previously-allocated extents remain correctly mapped.
- **Double-allocation race.** Two MFs concurrently write to the same unmapped virt_idx. Assert: MO serializes, exactly one `CDV_ALLOC` is consumed, both MFs learn the same `phys_offset`. This is the key correctness test -- the current Exclusive model has no analog of this race.
- **DISCARD vs write race.** MF-A DISCARDs virt_idx V while MF-B writes V. Outcome depends on order at the MO: either UNMAP wins (B allocates a fresh slot) or ALLOC wins (A's DISCARD is a no-op on an already-freed extent). Either is legal; assert "no dangling slot leaked."
- **Encryption gating.** Attempt `initEncryption` on a SHARED TPV with two registrants. Assert refusal with `TPV_SHARED_ATTACHED_CANNOT_ENCRYPT`.
- **Per-TPV preempt.** Evict client A from TPV X while A has another TPV Y on the same CDV. Assert A loses TPV X's gendisk but keeps Y and the CDV attachment.
- **Degradation.** Attach a shared TPV to one client, run today's self-test suite (`/proc/nvmeibc/tpv/<name>/selftest`). Assert: all tests pass (feature adds no regression on single-client workloads).

Reuse the existing self-test harness in `nvmeibc_tpv_test.c` extended with an MO/MF stub so the kernel-internal paths can be exercised without a multi-node cluster.

---

## 8. Risks and open questions

### 8.1 Interaction with per-CDV preempt (`TPV_PerClientCDVPreemption.md`)

The per-CDV preempt primitive was designed for the exclusive model. Walking through the shared case:

- **Preempt client A from CDV cdvC.** A's CDV attachment is torn down; `nvmeibc_tpv_handle_cdv_preempted` fires on A and detaches every TPV A rides. If A was an MF on TPV X, the other registrants are unaffected. If A was the MO, management must notice the detach and elect a new MO. This happens automatically via the detachTPV path in Sec. 4.4 -- specifically the "MO detaches" branch runs during the preempt's `cleanupDB` step, not during the CDV eviction itself, so management re-enters `electNewMO` with the remaining registrants. The remaining MFs continue to serve I/O under a new `mo_generation`, gated by the `<TPV>-meta` preempt so no stale tree write from A can land.
- The Sec. 2.10 invariant that "an evicted client cannot write to the CDV" still holds -- A's reg_ctx on cdvC is terminated at TOMA, so A's RDMA writes are rejected at the target even if A's kernel module is lagging in processing the preempt notification.
- The window between old MO detach and new MO election is a graceful-failover-under-load case. Covered by Sec. 7 test plan.

### 8.2 Interaction with TPV encryption (`TPV_EncryptionPlan.md`)

Encryption init attaches the TPV EXCLUSIVE to a TOMA. This is incompatible with SHARED. Sec. 4.6 handles it by gating `initEncryption` / `addPassphrase` / etc. on zero registrants. The CSI driver must not claim an encrypted TPV as shared without coordinating passphrase operations (document in the CSI README).

A richer design -- run passphrase ops online by briefly pausing all MFs and the MO -- is feasible but adds a new "global pause" primitive. Not in scope for the initial shared-TPV ship.

### 8.3 MO RPC load scaling

With `N` MFs all doing sparse writes, the MO processes `N x (miss-rate)` `TPV_ALLOC_V` RPCs. On a working set that fits in the MFs' local caches, miss-rate decays to "new allocations only" -- identical to the MO's own `CDV_ALLOC_EXTENT` rate today. On pathological all-miss workloads (every write is to a fresh virt_idx from a different MF), the MO serializes and becomes the bottleneck. For the first release this is acceptable; future optimizations -- MO-side batching, per-region sub-owner sharding -- are listed as post-MVP follow-ons.

### 8.4 Handoff snapshot size

A TPV with `V` mapped virtual extents has an `extent_map` with `V` xa entries at 32 bytes apiece, plus the free-slot list and CDV-extent-ref list. For V = 16 Mi (1 TiB at 64 KiB extents, fully dense -- unrealistic), the snapshot is ~500 MiB. For V = 256 Ki (16 GiB dense), it is ~8 MiB. Handoff must stream the snapshot in chunks rather than one message; reuse the existing Kafka-large-payload chunking pattern used by `TOMA state dump`. For typical sparse TPVs the snapshot is well under 1 MiB.

### 8.5 Downgrade path

A cluster that rolls back to a pre-shared-TPV image must not leave SHARED TPVs attached: the old management server and kernel have no concept of MO. The mNDU compatibility model blocks downgrade while any `tpvConfig.shared === true` record exists. Operators must detach and set `shared: false` (via a one-shot helper) to downgrade. The `nvmesh-interop-db` gate follows the pattern established by the per-CDV preempt feature -- a new `supportsSharedTPV` capability flag per component.

### 8.6 L1/L2 tree fencing -- the core correctness argument

The tree has exactly one accepted writer at a time because it lives on `<TPV>-meta` and `<TPV>-meta` is held `EXCLUSIVE_RW` by the current MO (Sec. 3.4). MO failover is a standard reservation-version preempt on `<TPV>-meta`; the old MO's tree writes are rejected at the target the instant the preempt lands, irrespective of whether the old MO's kernel has yet processed the notification. This reuses the same fencing construction shipped for `<CDV>-mgmt` (`TPV_SatelliteVolumeForCDVAlloc.md`) at TPV granularity.

What this does **not** fence: user-data writes into CDV extents. Those continue through the CDV's shared-RW attachment and are fenced by `preemptClientFromCDV` / per-client CDV admission floor as in the exclusive-TPV model. Sec. 5.6 walks through why the two fencing planes combine correctly and where the slot-reuse grace window closes the remaining race window.

### 8.7 Test requirements specifically targeting tree corruption

Because tree corruption is silent (the TPV keeps serving I/O until a stale mapping is read), the test plan carries dedicated adversarial cases beyond Sec. 7:

- **Stale-MO tree-write attempt.** Inject a delay between "management bumps `<TPV>-meta.reservation.version`" and "old MO observes the preempt." During the delay, the old MO tries to flush a dirty L2 page. Assert the write is rejected at the target with `RESERVATION_PREEMPTED`; the old MO transitions the satellite gendisk to `NCBD_PREEMPTED`; no garbled bytes land on the satellite.
- **Cold failover with stale in-memory tree.** Kill the old MO mid-flush (before the full L1 rewrite completes). Promote an MF. Assert `load_state`+`recovery` reconstructs a consistent tree even with the last partial write visible on the satellite -- standard torn-4 KB-write semantics (every entry fits in one 4 KB page, Sec. 3.4.3 of the main design).
- **Concurrent MO and MF attempt on the same virt_idx.** Force race between an MO-local `alloc_extent` (e.g., MO's own I/O) and an MF's `TPV_ALLOC_V` for the same unmapped virt_idx. Assert exactly one CDV extent is consumed, both paths return the same phys_offset, and the tree has exactly one leaf entry.
- **Slot-reuse grace.** Trigger DISCARD of virt_idx V, immediate WRITE of a different virt_idx V'. Verify V' is not assigned V's freed slot until the cooling window elapses; that no stale in-flight data write for V can overwrite V'.

### 8.8 Observability

- Add `/proc/nvmeibc/tpv/<name>/role` -- `exclusive | mo | mf`.
- On MFs, `/proc/nvmeibc/tpv/<name>/mo_identity` shows `mo_client_uuid + generation`.
- On the MO, `/proc/nvmeibc/tpv/<name>/mf_peers` lists registrants (advisory, fed by TOMA on topology push).
- Management UI: the TPV row grows a "Clients" column (count + expand) in place of the current single-client cell for shared TPVs.
- CLI: `nvmesh tpv show <tpv>` displays role info and peer registrants.

### 8.9 Not yet answered

- **Virtio-style block submission ordering.** If two MFs submit back-to-back writes to the same mapped virt_idx and each goes straight to RDMA, ordering at the CDV is whatever the RDMA transport happens to deliver. This matches any shared block device; higher-level coordination is the cluster FS's job. Confirm in stress testing that the existing single-writer `sync_flush` barrier is not silently assumed anywhere else.
- **Per-TPV admission floor migration.** If a TPV moves between CDVs (not supported today, but considered for future), the floor must move with it. Deferred.
- **Shared TPV + CSI.** A `ReadWriteMany` PVC backed by a shared TPV is the natural mapping. The CSI controller-side `CreateVolume` would create a `shared=true` TPV and the node-side `NodeStageVolume` would attach in MF/MO mode depending on whether this node is the first to stage. Work out the Kubernetes-side access-mode advertisement and the cluster FS selection defaults before shipping.

---

## 9. Phased rollout

| Phase | Scope |
|---|---|
| P1 | Management schema (`shared`, `mappingOwner*`, `metaVolumeId`), `<TPV>-meta` create/delete via `allocateAndSliceIntoVolumes`, `attachTPVMetaForMO` internal entry point. Reject shared attaches at the kernel level behind a disabled feature flag. Exercises the management flow end-to-end, including the core satellite fencing primitive. |
| P2 | Kernel role state + MF->MO RPC surface against a stub MO (self-test only). Verify RPC routing through TOMA works. The MO's `flush_state` is retargeted from CDV data extents to `<TPV>-meta` via the satellite's block device. |
| P3 | Full MO implementation: the MF RPC handlers on an MO, tree serialization for handoff, graceful MO failover with satellite preempt + snapshot handoff. Integration tests against a two-client cluster. |
| P4 | Cold MO failover via `load_state`+`recovery` from `<TPV>-meta` on a surviving MF. Adversarial tree-corruption tests (Sec. 8.7). Slot-reuse grace window. |
| P5 | Per-TPV preempt operation. CSI integration (ReadWriteMany). |
| P6 | UI surfacing, CLI ops, CSI storage class flag, mNDU interop-db capability, feature-flag flip. |

Every phase is independently mergeable behind the feature flag `management.sharedTPV.enabled` (default off through P5, flipped on cluster-wide after P6's soak test).

---

## 10. Done criteria

- Two clients running `fio --rw=randwrite --iodepth=32` against one OCFS2-mounted shared TPV for 24 hours with no corruption and no stalled I/O.
- Graceful MO failover measured at <= 100 ms pause on surviving MFs (tail latency histogram).
- Cold MO failover completes within the `load_state`+`recovery` budget established for single-client attach (measured today at a few seconds for TB-class TPVs).
- Per-TPV preempt passes the same adversarial suite as per-CDV preempt (Sec. 7, TPV_PerClientCDVPreemption.md).
- Encryption gating rejects `initEncryption` on a multi-registrant shared TPV with the documented error.
- mNDU gate rejects rolling upgrade to a shared-TPV-unaware version when any `shared=true` TPV exists.
- `TPV_ThinProvisioningImplementation.md` Sec. 3.2, Sec. 3.5, Sec. 3.8 cross-reference this document for the shared branch.
