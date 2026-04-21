# Satellite Volume for CDV Allocator — Implementation Plan

> **Shipped status (retrospective, 2026-04).** Several plan bullets below diverged from what actually shipped. Where the plan says "Retire `cdvConfig.allocatorSizeGib`" / "fixed at 1 GiB", the implementation kept the field as a user-configurable CDV parameter (minimum 1 GiB, default 1 GiB). The max-extents ceiling therefore scales with the admin-chosen `allocatorSizeGib` rather than being pinned. All other elements (CDV_MGMT volumeClass, single raw allocation sliced into two volumes, preempt-based fencing) shipped as written. Treat this document as historical design with the `allocatorSizeGib` override noted inline.

## Overview

Every CDV gains a dedicated satellite volume named `<CDV>-mgmt` whose sole purpose is to hold the allocator header and `cdv_extent_md[]` array that previously lived in the CDV's leading `[0, A)` region. The satellite size is `cdvConfig.allocatorSizeGib` GiB (user-configurable, defaults to 1). The satellite is held `EXCLUSIVE_READ_WRITE` by the current allocator TOMA; re-election is a plain preempt on the satellite, reusing NVMesh's existing reservation-version fencing to reject writes from a replaced-but-still-alive allocator.

Rationale, correctness argument, and comparison against the range-fenced alternative (Option A) are in `ThinProvisioningImplementation.md` Part 1.5. This document is the **implementation plan**.

The plan is organized in four phases. Phases 1 and 2 ship together; Phase 3 follows; Phase 4 is parallelizable with Phase 3 after Phase 1 lands.

---

## Phase 1 — Management

**Goal:** CDV creation produces two durable volume documents atomically, the CDV on-disk layout no longer reserves `[0, A)`, satellite attach is a private (non-REST) operation, and TOMAs no longer auto-attach CDVs.

### 1.1 Schema and validation

- **New fields on CDV document** (`models/volume.js`): `allocatorVolumeId` (ObjectId), `allocatorVolumeUUID` (String). Mandatory for `volumeClass: 'CDV'`, absent on all other classes.
- **New `volumeClass: 'CDV_MGMT'`** added to `consts.volumeClass`. Satellites are their own class — makes filter queries straightforward and removes any name-parsing-based typing.
- **Fields on CDV_MGMT document:** `parentCDVId`, `parentCDVUUID`. No `cdvConfig`, no `tpvCount`, no user-facing mutability.
- **CDV name length limit = 16 characters.** Enforced in the existing name validator. Add `assertNotReservedCDVSuffix` that rejects user-facing create/rename of any volume whose name ends in `-mgmt`.
- ~~**Retire `cdvConfig.allocatorSizeGib`** from the create path. The satellite size is fixed at 1 GiB. The field remains readable on pre-migration records but is ignored on new creates.~~ **As shipped:** `cdvConfig.allocatorSizeGib` was retained as a user-configurable CDV create parameter (minimum 1 GiB, default 1 GiB). The satellite's size is set to that value at CDV create time and is fixed per-CDV thereafter. Max extents ≈ `allocatorSizeGib × 262 143` (one 4 KiB record per extent); admins provisioning very large CDVs at small extent sizes raise this value at create time.

### 1.2 Create path — single raw allocation, sliced into N volumes

The CDV and its satellite share a single raw allocation. This guarantees co-placement (the satellite is always in the CDV's first chunk) and keeps the allocator's placement decisions coherent — no separate "put the satellite on the same first pRAID as the CDV" constraint to re-derive.

#### 1.2.1 New generic allocator primitive: `allocateAndSliceIntoVolumes`

Add a new primitive to the allocation layer (likely `modules/allocation.js` or wherever the current `allocateChunks` helpers live):

```
allocateAndSliceIntoVolumes(
  totalSize: Number,                       // bytes, == sum of slice sizes
  slices: [ { name: String, size: Number, volumeClass: String, … }, … ],
  allocationOptions: { driveClasses, targetClasses, raidLevel, … }
) → [ VolumeDocument, … ]                  // one per slice, in input order
```

Semantics:
- Slices are allocated **sequentially** via the existing `utils.createVolume()` path (the actual implementation in `modules/volume.js` walks the slice list with `async.eachSeries`). Each slice uses the same placement rules (RAID level, drive class, target class, limit-by-nodes/disks, etc. supplied via `allocationOptions`).
- Slice 0 is allocated first; it therefore lands in the first available chunks and is contiguous with the start of the allocator's placement decision. Slice 1 then consumes the next chunks, and so on.
- Each output volume record owns its own whole chunks — slices do not share chunks.
- Atomic at the **plan** level, via rollback-on-failure: if any slice fails to allocate, slices created earlier in the loop are marked for deletion before returning the failure. The outcome from the caller's perspective is all-or-nothing.

**Chunks are not shared between slices.** The output volume records must be indistinguishable from volumes created the normal way — no new fields, no sub-chunk sharing. The allocator is asked for `totalSize`, and the resulting chunk plan is partitioned at slice boundaries so each slice owns whole chunks. For the CDV case this means: allocate 101 GiB for a 100 GiB CDV, the first 1 GiB forms the satellite's chunks, and the remaining 100 GiB forms the CDV's chunks. The allocator must be able to honor a request that the first chunk be sized exactly `slices[0].size`; everything after is allocated with the normal chunking rules.

Concretely, `allocateAndSliceIntoVolumes` only changes the allocator's driver — it does not change volume-chunk records, volume attach, or any downstream consumer. A volume produced by this path looks identical in Mongo and on the wire to a volume produced by a plain `createVolume`.

#### 1.2.2 CDV create flow

In `modules/volume.js` `prepareCDVForCreate` → `createCDV`:

1. Validate CDV name length (≤ 16 chars) and reserved-suffix rules.
2. Build slice list:
   ```
   slices = [
     { name: `${cdv.name}-mgmt`, size: 1 GiB, volumeClass: 'CDV_MGMT', parentCDVUUID: cdv.uuid },
     { name: cdv.name,           size: cdv.capacity, volumeClass: 'CDV', allocatorVolumeUUID: <sat.uuid> },
   ]
   ```
   Satellite first — guarantees it lands in the first chunk.
3. Compute total = `cdv.capacity + 1 GiB`. Validate against cluster free space.
4. Call `allocateAndSliceIntoVolumes(total, slices, allocationOptions)`. On failure, abort cleanly — nothing to roll back (no Mongo state yet, allocator rolled itself back).
5. Back-fill cross-references in the returned documents: set `cdv.allocatorVolumeId / allocatorVolumeUUID` from the satellite's record; set the satellite's `parentCDVId / parentCDVUUID` from the CDV's record.
6. Insert with `insertMany([cdv, sat], { ordered: true })` — single round-trip. On failure, release the allocation before exit.
7. Emit no Kafka until both inserts succeed. Only then `volumeInventoryChanged` fires for the CDV; satellite creation is not user-visible-broadcast.

CDV data extents now start at on-disk offset 0 — update CDV geometry helpers so no `[0, A)` is carved out of the CDV.

#### 1.2.3 Why this is cleaner than two separate allocations

- The satellite being in the first chunk of the CDV's allocation means any TOMA hosting the CDV's first pRAID is by construction also hosting the satellite. The §2.6 "live-path" invariant holds without a secondary placement constraint.
- One allocator pass, one rollback path, one place that knows about the raw total — reduces the surface area for "satellite leaked if CDV create failed between step N and N+1" bugs.
- The generic `allocateAndSliceIntoVolumes` primitive is reusable for future features that need co-placed volumes (e.g., metadata volumes for other subsystems, shadow devices, etc.). This is not a one-off.

### 1.3 Delete path — atomic double-delete

- Refuse delete of a volume with `volumeClass === 'CDV_MGMT'` from every user-facing path (REST, CLI, UI).
- When deleting a CDV: delete both documents in a single operation; emit `DeleteVolume` Kafka for both in the same batch.
- The existing refusal rule ("cannot delete a CDV with `tpvCount > 0`") is unchanged. Satellite deletion is deferred until the CDV itself is deletable.

### 1.4 Extend path — CDV-only

No change beyond confirming that `updateVolumes` targeting a `CDV_MGMT` volume is refused. Satellite size is fixed and not user-extendable.

### 1.5 Attach / detach — private satellite path

- **REST refuses satellite attach.** `modules/client.js` `attachVolumes` filters any target volume with `volumeClass === 'CDV_MGMT'` out of the request, records an error message per filtered row, and lets the standard route response carry a 4xx when everything the caller asked for was refused. The filter is unconditional — there is no `adminManualOperation` override — so user-facing REST cannot attach a satellite by any path.
- **New internal entry point** `modules/client.js` `attachSatelliteForAllocator(cdvUUID, tomaHostname, allocatorGeneration, requestId)`:
  1. Look up satellite via `cdv.allocatorVolumeId`.
  2. Call the existing exclusive-preempt attach path with `reservation.mode = EXCLUSIVE_READ_WRITE`, `preempt = true`, `isDetachOthers = true`, `clientID = tomaHostname`. Same code path user-driven exclusive preempts take today — no new attach primitive.
  3. Return the resulting attachment record (UUID, version, target placement) to the Kafka handler (Phase 2).
  4. Idempotency: if the satellite is already attached to the same TOMA at the same version and same `requestId`, return the cached response.
- **No explicit detach on re-election.** The next allocator's attach call with `preempt=true` bumps `reservation.version` and records the new holder; the old holder is fenced by the existing preempt mechanism.
- **Stale-TOMA cleanup:** existing `removeAlreadyDetachedAttachments` applies to satellites identically to any other volume.

### 1.6 Retire `cdvTomaAutoAttach` on the CDV

The current module attaches the CDV to every TOMA hosting the first pRAID so TOMAs can write `[0, A)`. That rationale is gone.

- Rename `modules/cdvTomaAutoAttach.js` → `modules/allocatorSatelliteAttach.js` and retarget it to satellites. `initCDV` becomes a no-op for auto-attach purposes; satellite attach happens only via TOMA request (§1.5 + Phase 2). `onTopologyUpdate` is no longer needed — the allocator election now drives its own re-attach.
- Remove `toma:<cdvUUID>` references entirely from CDV attachment `referenceIDs`. `maybeDetachCDVFromNode` simplifies to `tpv:*`-only.
- The `'RW_ENABLED'` typo site disappears with this refactor; no port needed.
- **Mongo cleanup migration:** one-shot pass to strip any lingering `toma:*` references and detach any CDV whose only remaining references were `toma:*`.

### 1.7 Client-facing behavior

- Client kernel attaches the CDV unchanged (`SHARED_READ_WRITE`). No client-side changes.
- `GET /volumes` and `GET /volumes/:id` filter out `CDV_MGMT` by default; add `?includeSatellites=true` for CLI/UI tools that need them.

### 1.8 Tests

- Unit test: CDV create rollback on `insertMany` partial failure — no leaked chunks, no partial Mongo state.
- Unit test: satellite-attach refusal over REST (403).
- Integration: create CDV → assert both volumes exist → attach TPV → delete CDV → assert both gone, no orphans.

---

## Phase 2 — Kafka: TOMA → management satellite attach

**Goal:** one new Kafka message pair lets an elected allocator TOMA request its exclusive attachment to the satellite.

### 2.0 Alternative considered: TOMA-driven RAFT eviction

We considered skipping the management round-trip and having the elected TOMA leader drive the satellite version bump directly via RAFT broadcast (calling `nvmeibt_block_device_reservation_mode_change()` locally on every TOMA hosting the satellite). This would save one Kafka request/response per re-election but requires:

1. A new RAFT message type carrying `{satellite_uuid, new_version}`.
2. A second source of reservation versions on the kernel client side (today the client learns versions exclusively from management's config push), so the new allocator's MCS register on the satellite can advertise the new version.
3. Acceptance that Mongo's `reservation` field on satellites permanently diverges from reality.

Rejected for now: piece (2) is the load-bearing change — touching the client kernel's reservation pipeline to source versions from a parallel channel (RAFT instead of management) is historically expensive to land and requires its own correctness argument. The Kafka round-trip with management is one extra hop on a rare path (re-election, not steady state) and reuses the existing preempt-attach machinery end-to-end, which makes it cheaper to ship and test.

**Future trigger to revisit:** if scaling tests show Kafka-coordinated re-election doesn't keep up with churn — many CDVs spread across many zones, simultaneous topology changes — switching to TOMA-driven RAFT eviction becomes worth its complexity. The Phase 2 message types and management handler can stay as a fallback path while RAFT-driven preempt becomes the steady-state mechanism.

### 2.1 Request message

New `models/kafkaMessages/AttachSatelliteRequest.js`:

```
{
  messageType:          'attachSatelliteRequest',
  direction:            TOMAToManagement,
  topic:                MANAGEMENT,
  payload: {
    cdvUUID:              String,
    allocatorTomaHostname: String,   // sender, echoed for validation
    allocatorGeneration:  Number,    // monotonic, from RAFT
    raftTerm:             Number,    // RAFT term at request time
    requestId:            String,    // client-assigned, for idempotent retry
  }
}
```

### 2.2 Management handler — `modules/kafka.js`

Register a consumer for `attachSatelliteRequest`. Handler calls `volumeModule.handleAttachSatelliteRequest`:

1. **Validate:** CDV exists; `volumeClass === 'CDV'`; `allocatorVolumeId` set; sender hostname matches `allocatorTomaHostname`; `allocatorGeneration ≥ cdv.allocatorGenerationLastAttached`.
2. **Idempotency:** if `(cdvUUID, requestId)` was already processed, reply from a per-CDV in-memory LRU.
3. **Monotonicity:** if `allocatorGeneration < cdv.allocatorGenerationLastAttached`, reject with `STALE_GENERATION`.
4. **Attach:** call `attachSatelliteForAllocator` (§1.5). Standard preempt-exclusive flow; result carries the new `reservation.version`.
5. **Persist:** update CDV document with `allocatorGenerationLastAttached` and `currentAllocatorTomaHostname`.
6. **Reply:** publish `AttachSatelliteResponse` on the requesting TOMA's zone `TOMA_COMMANDS` topic.

### 2.3 Response message

New `models/kafkaMessages/AttachSatelliteResponse.js`:

```
{
  messageType:          'attachSatelliteResponse',
  direction:            ManagementToTOMA,
  topic:                TOMA_COMMANDS (zone-addressed to sender),
  payload: {
    cdvUUID, requestId,
    status:              'OK' | 'STALE_GENERATION' | 'CDV_NOT_FOUND'
                           | 'CDV_BEING_DELETED' | 'INTERNAL_ERR',
    satelliteUUID:       String,   // only on OK
    satelliteTargets:    [...],    // placement info
    reservationVersion:  Number,
    allocatorGeneration: Number,   // echoed
  }
}
```

### 2.4 Failure / timeout semantics

- TOMA retries every N seconds with the **same** `requestId` until a terminal reply arrives.
- `CDV_BEING_DELETED` and `CDV_NOT_FOUND` are terminal; TOMA tears down its in-memory allocator entry for that CDV.
- Management's handler is fully idempotent (§2.2 step 2). If management restarts mid-flight, the TOMA's next retry rehits the handler; Mongo state is authoritative — if already attached, return the cached response.

### 2.5 Tests

- Unit-test the validation table: valid, stale generation, missing CDV, CDV deleted, idempotent retry.
- Kafka integration: publish `AttachSatelliteRequest` from a simulated TOMA, assert `AttachSatelliteResponse` returns OK with the satellite exclusive to the requester. Send a second request from a different TOMA and assert preempt succeeds, bumping `reservation.version`.

---

## Phase 3 — TOMA allocator on the satellite

**Goal:** all allocator reads and writes happen on the satellite's block device. Election remains unchanged; the post-election path goes through §1.5/Phase 2 instead of directly touching `[0, A)` of the CDV.

### 3.1 Topology push carries satellite UUID

`nvmeibt_topology_calc_topology` → per-pRAID topology broadcast already carries CDV metadata. Add `allocator_volume_uuid` to the push payload. Schema change in both the TOMA-to-TOMA topology replication and the management-to-TOMA topology seed — one new field.

### 3.2 Election handoff — two-stage flow

Replace the current body of `nvmeibt_cdv_alloc_handle_notify` (which directly scans/writes the CDV allocator area) with:

**Stage A — request attach.** On election or on RAFT-committed notify arrival at the chosen allocator:
- Enqueue an `AttachSatelliteRequest` work item with `{cdvUUID, my_hostname, allocator_generation, raft_term, requestId = random()}` and publish via existing `toma_kafka_send` helpers.
- Set the CDV's local alloc state to `STATE_AWAITING_SATELLITE_ATTACH`.
- Do **not** serve `CDV_ALLOC_EXTENT` in this state — respond `WRONG_GEN` so clients keep retrying.

**Stage B — on response.** New consumer in `toma/nvmeibt_kafka.c` for `attachSatelliteResponse`:
- `status == OK` → resolve the satellite's segment via the standard TOMA volume-open path (management has already attached the satellite to this TOMA; locally it shows up as a normally-attached exclusive volume). Open a R/W handle to it.
- Run `cdv_ondisk_scan_async` against the satellite (not the CDV). On completion, set `alloc->ondisk_loaded = true`, promote state to `STATE_ACTIVE`, and run `nvmeibt_cdv_alloc_push_to_registrants` as today.
- `status != OK` → back off, retry with a fresh `requestId`, unless the status is terminal (`CDV_NOT_FOUND`, `CDV_BEING_DELETED`), in which case tear down the local alloc entry.

### 3.3 Offset rebase in `nvmeibt_cdv_alloc.c`

Every allocator-area read or write moves from "CDV block device, offsets `[0, A)`" to "satellite block device, offsets `[0, 1 GiB)`":

- `cdv_async_write_record`, `cdv_async_write_record_needs_zeroing`, `cdv_async_write_header`, `cdv_zero_execute`: change the `fd` argument from the CDV handle to `alloc->satellite_fd`. Offset formulas unchanged — header at 0, record `i` at `4096 + i * record_size`.
- `cdv_ondisk_scan` / `cdv_ondisk_scan_async`: same rebase, identical scan logic.
- **CDV data-extent offsets no longer subtract `A`.** Helpers that compute "physical offset of CDV data extent `i`" (in `nvmeibc_tpv.c` on the client, and any TOMA CDV-stats code) simplify from `A + i*E` to `i*E`. Grep for `allocatorSizeGib`, `A_bytes`, `allocator_size_gib`.

### 3.4 Per-satellite I/O work queue

Rename the per-CDV WQ to per-satellite. Its lifetime is tied to the satellite attachment. Release the WQ when the satellite is preempted.

### 3.5 Handling preempt on the satellite

When the satellite attachment receives `'P'` status (next allocator preempted us):

1. Treat it as authoritative "I am no longer the allocator."
2. Drop pending `cdv_async_*` requests; do not retry.
3. Close the satellite handle.
4. Reset in-memory alloc state to `STATE_NOT_ALLOCATOR`. Keep the `cdv_alloc_hash` entry so allocator-generation monotonicity is preserved, but stop serving.
5. Log loudly; this is an expected event on re-election.

This path replaces the old design's "if it comes back, finds generation behind header" argument. The preempt signal is now the authoritative "you're no longer it."

### 3.6 RAFT-leader election logic unchanged

`nvmeibt_topology_calc_topology` + `nvmeibt_cdv_alloc_elect` continue as today. After the leader records `(allocator_toma_id, allocator_generation)` in its local state and unicasts `CDV_ALLOCATOR_NOTIFY` to the chosen TOMA, the chosen TOMA's handler runs Stage A of §3.2 instead of writing the CDV header directly.

### 3.7 Simulator

- `mgmt/nvmeibm_mgmt_simu.c` — fake handler for `attachSatelliteRequest` that replies OK with a simulator-generated satellite block device.
- `toma/nvmeibt_toma_simu.c` — Kafka path stubs and satellite-fd resolution.
- The existing allocator-failover test (`test_tpv_allocator_election.c`) extends naturally: a stale allocator's write to the satellite is rejected by the simulator's reservation-version check.

### 3.8 Tests

- Self-test in `toma/test_cdv_alloc.c`: election → request → response → scan → serve; preempt → close → STATE_NOT_ALLOCATOR; retry on `STALE_GENERATION`.
- End-to-end sim: attach CDV + TPVs → force allocator TOMA re-election → assert no corruption, assert old allocator's late writes to the satellite are preempted.

---

## Phase 4 — CLI: satellite rows under CDVs

**Goal:** satellites appear in user-facing listings as child rows of their parent CDV, with no selection/edit affordances, and without introducing new commands.

### 4.1 Entity registration — `rest.yaml`

- Add a `CDV_MGMT` entity as a sub-type of `Volume` with `_get_filter: { volumeClass: 'CDV_MGMT' }`, following the CDV/TPV pattern.
- `ops`: `show` only. `create`, `delete`, `update`, `attach`, `detach` → `Unsupported`.
- `display` fields: `name`, `status`, `parentCDVUUID`, `reservation.reservedBy`, `reservation.mode`, `reservation.version`, `health`, `targets[]`.
- No independent `route`; reads flow through `/volumes` with the `includeSatellites=true` flag added in §1.7.

### 4.2 `rest_custom.py`

- Add `CDVMgmtGroup(RestGroup)`. Override `show` to render with a leading indent marker (e.g., `"  └─ "`) so the row visually sits under its parent.
- In `CDVGroup`, override the default `show` to fetch and interleave satellites: for each CDV row, immediately follow with its `<CDV>-mgmt` row. Join by `parentCDVUUID`; single `GET /volumes?includeSatellites=true` keeps it to one REST call.
- Selection/edit affordances are disabled at the CLI by the `Unsupported` ops; UI-level implications (no checkbox, no edit button) belong to the UI layer.

### 4.3 Golden files

- `current.api` — append `CDV_MGMT show` entries (no other commands, since the rest are `Unsupported`).
- `current.display` — append satellite display columns.

### 4.4 SDK — `volume.py`

Add `CDVMgmt` as a trivial `SDKEntity` subclass (`volumeClass = 'CDV_MGMT'`). Read-only.

### 4.5 UI — parallel to CLI

In `Volumes.jsx`:
- Group satellites under their parent CDV. Render them as child rows with no checkbox column and no Edit button.
- Default filter excludes satellites; "Show CDVs" filter includes them nested.
- `ThinProvisioning.jsx` does not show satellites at all.

### 4.6 Tests

- CLI golden-file test against `current.api` / `current.display`.
- UI snapshot test of the nested row render.

---

## Ordering and dependencies

- **Phases 1 + 2 ship together.** Management without the Kafka path is half a system; Kafka without management is untestable.
- **Phase 3** starts once §1.5 and §2.3 interfaces are frozen. It does not depend on §1.7 or Phase 4.
- **Phase 4** starts in parallel with Phase 3 once Phase 1 lands.
- **Simulator (§3.7)** is the integration-test long pole. Start it early, in parallel with §3.3.

---

## Open items to resolve before coding

**O1 — §2.10 TPV-client preemption gap is separate.** This plan does **not** close the stale-TPV-client-writes-to-CDV hole. That remains open until §2.10 is designed and implemented (options P1/P2/P3 in `ThinProvisioningImplementation.md`). Decide whether §2.10 blocks shipping thin provisioning to customers or whether we ship with a known limitation documented.
