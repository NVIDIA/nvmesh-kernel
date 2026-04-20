# Thin Provisioning Implementation Plan — v3

Based on: *2026-03 Minimal Extensible Thin Provisioned Volumes.md*

Authoritative merged document. Supersedes ThinProvisioningImplementation1.md and ThinProvisioningImplementation2.md.

---

## Terminology

| Term | Meaning |
|------|---------|
| **CDV** | Carrier Direct Volume — thick-provisioned shared volume holding capacity for TPVs |
| **CDV\_extent** | Allocation unit carved from CDV; size is configurable per-CDV (`cdvExtentSizeMiB`), power-of-2, between 64 MB and 64 GB |
| **TPV** | Thin-Provisioned Volume — virtual volume riding on a CDV, exclusively attached to one client |
| **TPV\_extent** | Fine-grained allocation unit within the TPV; size is configurable per-TPV (`tpvExtentSizeKB`), power-of-2, between 64 KB and 64 MB |
| **CDV.allocator** | Central allocator running on a TOMA node; manages CDV\_extent allocation |
| **TPV.allocator** | Client-local allocator; manages TPV\_extent sparse map within already-allocated CDV\_extents |

---

## Architecture Decisions (resolved)

1. **CDV.allocator placement**: Dedicated TOMA node (central, not distributed).
2. **CDV.allocator persistence**: A configurable area at the start of the CDV of size `allocatorSizeGiB` GB (default 1 GB). Max addressable data extents = $(\texttt{allocatorSizeGiB} \times 1\,\text{GB} - 4\,\text{KB}) / 24$ (see §2.2). Decoupled from `cdvExtentSizeMiB` so the allocator area size can be chosen independently of the allocation granularity.
3. **Management → kernel channel**: Existing MCS over Kafka (`AttachVolumes` / `DetachVolumes` / `UpdateVolume` messages extended with new fields).
4. **CDV attach mode**: Hidden shared-RW to clients with a TPV on that CDV (so the kernel thin-provisioning code can access CDV data); non-hidden shared-RW to TOMA nodes that are candidate allocators (so TOMA can perform block I/O to the allocator metadata area).
5. **TPV\_extent size**: Variable — configurable per-TPV at creation time (`tpvExtentSizeKB`). Different TPVs on the same CDV may use different TPV\_extent sizes.
6. **TPV.Delete**: Force-reclaim from CDV.allocator via Kafka message to TOMA (no client attach needed).
7. **MongoDB modeling**: Extend existing `volumes` collection; no new collections.
8. **CDV\_extent allocation requests**: Client → TOMA allocator via the per-disk ADMIN channel (new opcode). TOMA notifies management only when CDV reaches 90% capacity via existing TOMA→management Kafka path.
9. **TPV.allocator state persistence**: Centralized in CDV\_extent[0] (the first data CDV\_extent, at CDV byte offset `A`), which is reserved at CDV init as the **L1 table**. The L1 table is a flat array of 16-byte entries (8B `extent_index` pointer + 8B `debug_meta`). The first half of the L1 table points to **L2 tables**; each L2 entry is a leaf pointing to a data CDV\_extent (2-level path, common case). The second half of L1 points to **L2a tables**, each of whose entries points to an **L3 table**, whose leaves point to data CDV\_extents (3-level path, rarely needed). L2/L2a/L3 tables are CDV\_extents allocated from the pool when first needed. Data CDV\_extents contain **no per-extent metadata** — all virtual→physical mapping lives in the tree. TPV UUID is the sole identifier for TOMA-side ownership tracking (`cdv_extent_md`).
10. **Allocator identity**: `(allocator_toma_id, allocator_generation)` are two fields carried in the existing RAFT-replicated pRAID topology record (the CDV's first-pRAID `topo_ctx`). They reach every TOMA through the standard `AppendEntries` → follower-apply pipeline — no parallel channel, no custom retry. Further propagation to clients uses the CDV topology push TOMAs already send. Allocator role is sticky — changes only when the current TOMA leaves the RAFT group, not on RAFT leader rotation. See §2.6 and `EmbedAllocatorInRaft.md`.
11. **New allocator selection rule**: Chosen from TOMAs hosting a RW-enabled disk segment in the CDV's first pRAID. One candidate → chosen by default. Multiple candidates → chosen at random by the RAFT leader (avoids bias).
12. **Allocation transactionality**: Allocator writes `cdv_extent_md` durably to CDV disk **before** sending the allocation response (write-before-respond). Generation fencing discards stale-generation responses on the client.
13. **CDV attach/detach lifecycle — auto-managed only**: CDV attachments are exclusively auto-managed by management; `POST /clients/attach` and `POST /clients/detach` reject CDV volumes with an error. Two independent reasons trigger CDV attachment to a node; both must be absent before the CDV is detached:
    - **TOMA reason** (`toma:<cdvUUID>` referenceID): node hosts a RW disk segment in the CDV's first pRAID (i.e., is a candidate allocator). TOMA attachment is added by `attachCDVToAllTomaNodes()` when the first TPV on the CDV is attached to any client; it is removed by `onTPVDetached()` when the last active TPV is detached. Topology changes (`onTopologyUpdate()`) add/remove the TOMA ref for nodes that enter/leave the first pRAID. CDV deletion also triggers `detachCDVFromAllNodes()` as a defensive cleanup before the volume record is removed.
    - **TPV client reason** (`tpv:<tpvUUID>` referenceID): node has a TPV backed by this CDV attached to it. Added by `attachTPV()` step `attachCDV`; removed by `detachTPV()` or `cleanupTPVReferencesForDetachedClient()` when the TPV is detached.
    - A node that is both a TOMA candidate and hosts an active TPV will hold both refs simultaneously. TOMA attachment runs first within `attachTPV()` (so the CDV is non-hidden when the client's `tpv:` ref is added); a node is only detached when both refs are removed.
    - **Hidden/non-hidden for mixed nodes**: if a node is simultaneously a TOMA candidate and a TPV client, it should have `isHidden=false` so TOMA can perform block I/O to the allocator area. Running `attachCDVToAllTomaNodes()` before `attachCDV()` in `attachTPV()` achieves this for the first TPV on a TOMA node. If the CDV was already attached hidden (from an earlier TPV on the same node), the `toma:` ref update via `updateOnlyRefIdIfPossible` preserves the existing `isHidden=true`. This edge case is accepted as a known limitation; in practice TOMA nodes are dedicated storage nodes and do not run TPVs.
14. **CDV hidden-attach orchestration**: Management orchestrates. When a TPV is attached to a client, `attachTPV()` in `modules/client.js` first ensures all TOMA nodes have the CDV (non-hidden), then sends a hidden CDV `AttachVolumes` message to the TPV client, then sends the TPV `AttachVolumes` message.
15. **RAID level for CDVs**: No restriction. User chooses RAID level freely (EC is recommended but not enforced by the UI or backend).
16. **REST API for CDV/TPV creation**: Both CDV and TPV creation reuse `POST /volumes/save` with `volumeClass` and the appropriate config sub-object in the payload. The backend `createVolume` handler branches on `volumeClass`.
17. **TOMA zeroing IO path**: The background zeroing worker issues zero-writes via the normal TOMA IO path to the EC volume. No special block-zero command path is needed.
18. **Encryption scope & zero-on-free default**: **CDVs and their `<CDV>-mgmt` satellite volumes are never created with NVMesh volume-level encryption** — neither carries an `isEncrypted` flag and management refuses to enable encryption on either. Encryption is handled exclusively at the TPV level (per-TPV LUKS overlay on the client; see Part 5). A TPV-layer rekey makes stale data at the CDV physical offsets cryptographically unreadable to the next TPV that allocates the slot. The satellite holds only allocator metadata (TPV UUIDs, extent indices) — no user data — so leaving it plaintext is by design and not a regression vs. the previous in-CDV layout. Zero-on-free on TPV delete is therefore not required for the typical encrypted deployment and is **disabled by default**. It can be enabled per-TOMA via the `cdv_extent_zero_on_free` runtime config parameter (`toma_rpc config set cdv_extent_zero_on_free 1`) for unencrypted or stricter-isolation deployments. See §3.9 for the release-path details.

---

```mermaid
graph TB
    subgraph management["Management Layer"]
        mgmt["Management Server\n(Node.js + MongoDB + Kafka)"]
    end
    subgraph toma_layer["TOMA Nodes"]
        toma["TOMA Node\n(CDV.allocator, RAFT-elected, sticky)"]
    end
    subgraph client_layer["Storage Client (kernel)"]
        client["Kernel Client\n(TPV.allocator in-memory)"]
        tpvdev[("TPV block device\n(virtual gendisk, EXCLUSIVE_RW)")]
    end
    subgraph storage["Physical Storage"]
        cdv[("CDV on NVMe\n(EC volume, shared-RW hidden)")]
    end

    mgmt -->|"AttachVolumes / DetachVolumes (MCS)"| client
    mgmt -->|"CDVAllocatorFreeAll (Kafka)"| toma
    toma -->|"CDV topology push (allocator_toma_id + generation)"| client
    client -->|"CDV_ALLOC_EXTENT (ADMIN channel)"| toma
    toma -.->|"write cdv_extent_md before respond"| cdv
    client <-->|"RDMA data plane (R/W)"| cdv
    client --- tpvdev
```

*Figure 7: System architecture layers. Management orchestrates attach/detach; TOMA runs the CDV.allocator with write-before-respond persistence; clients hold TPV.allocator state in-memory and access the CDV directly over RDMA.*

---

## Part 1 — Management Layer

### 1.1 MongoDB Schema — Volume Model Extensions

File: `nvmesh-management/validationSchemes/definitions/volume.js`

Add the following fields to the existing volume validation schema:

```js
// Class discriminator — added alongside existing 'type' field (METADATA_VOLUME / DATA_VOLUME)
volumeClass: {
    type: String,
    enum: ['REGULAR', 'CDV', 'TPV'],
    default: 'REGULAR',
},

// CDV-specific fields (present when volumeClass === 'CDV')
cdvConfig: {
    maxTPVs:          { type: Number, default: 512 },   // mutable cap on hosted TPVs; default 512
    cdvExtentSizeMiB:  { type: Number, required: true }, // power-of-2, 64–65536 MB
    allocatorSizeGiB:  { type: Number, default: 1 },     // allocator area size in GB; integer >= 1
},

// TPV-specific fields (present when volumeClass === 'TPV')
tpvConfig: {
    cdvId:               { type: String, required: true },  // _id of parent CDV
    cdvUUID:             { type: String, required: true },
    tpvExtentSizeKB:     { type: Number, required: true }, // power-of-2, 64–65536 KB
    // Constraint: tpvExtentSizeKB <= cdv.cdvConfig.cdvExtentSizeMiB * 1024
    virtualSizeGB:       { type: Number, required: true },  // current virtual size
    maxVirtualSizeGB:    { type: Number, default: 1000 },   // hard cap (1 TB default)
    exclusiveClient:     { type: String, default: null },   // clientID when attached
    exclusiveClientUUID: { type: String, default: null },
},
```

Also add a `tpvCount` field to CDV records (not in the schema definition above, managed directly by `createTPV` / `deleteTPVs`):

```js
tpvCount: { type: Number, default: 0 },  // tracked via $inc; never set directly
```

The `capacity` field on a TPV record holds the *virtual* capacity. Physical capacity consumed is tracked via CDV.allocator on-disk state, not in MongoDB.

### 1.2 REST API — New and Modified Endpoints

#### CDV creation — extends existing `POST /volumes/save`

No new endpoint. CDV creation goes through the existing `POST /volumes/save` with `volumeClass: 'CDV'` and `cdvConfig` in the body. The `createVolume()` handler in `modules/volume.js` validates the CDV-specific fields (see §1.4). The frontend `VolumesService.create(volume)` requires no changes.

#### TPV creation — extends existing `POST /volumes/save`

TPV creation also goes through `POST /volumes/save` with `volumeClass: 'TPV'` and `tpvConfig` in the body. The `createVolume()` handler performs the cross-document validation (CDV lookup, `tpvCount` cap check, `tpvCount` increment) when it sees `volumeClass === 'TPV'`.

#### New endpoints in `routes/volumes.js`

```
POST /volumes/tpv/update
  Body: { _id, name?, description?, tpvConfig.maxVirtualSizeGB? }
  → Mutable fields only: name, description, maxVirtualSizeGB.
  → volumeClass and tpvConfig.cdvId are immutable.

POST /volumes/tpv/delete
  Body: [ _id, ... ]  (array of TPV _id strings)
  → Requires tpvConfig.exclusiveClient === null for each (must be detached).
  → Sends CDVAllocatorFreeAll Kafka message to TOMA for each TPV.
  → Decrements CDV.tpvCount via $inc.
  → Deletes TPV record.
  → Errors reported same as regular volume delete (no disabled-button precondition in UI).

POST /volumes/tpv/extend
  Body: { tpvId, newSizeGB }
  → Validates newSizeGB > current && <= tpvConfig.maxVirtualSizeGB.
  → Updates tpvConfig.virtualSizeGB + capacity.
  → If TPV is currently attached (exclusiveClient !== null), sends UpdateVolume
    Kafka message to client with updated virtualSizeGB.
```

CDV create, update, delete, and extend all use the existing `/volumes` endpoints (`POST /volumes/save`, `POST /volumes/update`, `POST /volumes/delete`, `POST /volumes/extend`). The `updateVolume()` handler in `modules/volume.js` branches on `volumeClass === 'CDV'` to apply CDV-specific update logic: `maxTPVs` is mutable; `cdvExtentSizeMiB` and `allocatorSizeGiB` are immutable and ignored if present in the payload. If `maxTPVs` is set below the current `tpvCount`, the update is accepted — existing excess TPVs are unaffected, and new TPV creation is blocked until `tpvCount` drops below the new limit. Backend enforces "all TPVs must be deleted first" for CDV delete and returns a standard error if violated; no special UI handling.

#### Modified endpoints

`POST /clients/attach` — when `volumeClass === 'TPV'`:
1. Reject if `tpvConfig.exclusiveClient !== null` (already attached elsewhere).
2. Call `client.js:attachTPV(clientID, tpvID)` — see §1.4 for full orchestration.
3. Set `tpvConfig.exclusiveClient = clientID`.

`POST /clients/detach` — when `volumeClass === 'TPV'`:
1. Call `client.js:detachTPV(clientID, tpvID)`.
2. After client confirms detach, clear `tpvConfig.exclusiveClient`.

### 1.3 Kafka/MCS Message Changes

Files in `nvmesh-management/models/kafkaMessages/`

#### Extend `VolumeMessage.js`

```js
// Added to VolumeMessage.toJSON() when volume.volumeClass is set:
volumeClass: volume.volumeClass || 'REGULAR',
tpvConfig:   volume.tpvConfig   || null,
cdvConfig:   volume.cdvConfig   || null,
isHidden:    volume.isHidden    || false,  // CDV hidden-attach flag
// For TPV rows, the existing `mdvUUID` field on the envelope is repurposed
// to carry the parent CDV UUID so the kernel can bind the TPV to its CDV via
// the existing MCS field set without a new kernel ABI (see `preparePayload`
// mapping table in the TPV attach section below).
```

#### Extend `AttachVolumes.js`

When attaching a TPV, include the full CDV configuration inline:

```js
{
    messageType: 'AttachVolumes',
    payload: {
        volumes: [
            {
                ...tpvVolumeConf,
                volumeClass: 'TPV',
                tpvConfig: { ... },
                cdvConf: {
                    uuid:   cdv.uuid,
                    name:   cdv.name,
                    chunks: cdv.chunks,
                    // ... full CDV volume configuration
                }
            }
        ]
    }
}
```

The CDV hidden-attach is sent as a *separate* `AttachVolumes` message before this one (§1.4). The `cdvConf` inline field is provided so the kernel can cross-check it has the correct CDV already attached.

#### New message: `CDVAllocatorFreeAll.js`

Sent from management to TOMA when a TPV is deleted (force-reclaim all CDV\_extents owned by that TPV):

```js
// payload:
{
    cdvUUID:          string,
    tpvUUID:          string,
    allocatorSizeGiB: number,  // needed so TOMA can recompute CDV-extent physical offsets
    cdvExtentSizeMiB:  number,  // without a management round-trip back to the CDV config
}
```

#### New message: `CDVCapacityWarning.js` (TOMA → management)

TOMA sends this when allocator usage reaches the warn threshold. Hysteresis is built into TOMA: the warning is raised when `n_allocated / total_data_extents ≥ NVMEIBT_CDV_WARN_PCT` (90%) and the flag clears only when usage drops back under `NVMEIBT_CDV_WARN_CLEAR_PCT` (85%).

```js
// payload:
{
    cdvUUID:      string,
    usedExtents:  number,
    totalExtents: number,
}
```

`CDVAllocatorRequest` and `CDVAllocatorResponse` are **not** management-layer messages — they flow directly between the client kernel and TOMA via the ADMIN channel.

### 1.4 Module Changes

#### `modules/volume.js`

Extend `createVolume()` to handle CDV and TPV:

```js
// After existing field validation:
if (volumeData.volumeClass === consts.volumeClass.CDV) {
    const { cdvExtentSizeMiB, allocatorSizeGiB, maxTPVs } = volumeData.cdvConfig || {};

    if (!consts.cdvExtentSizeMiBValues.includes(cdvExtentSizeMiB))
        throw new Error('cdvExtentSizeMiB must be a power-of-2 between 64 and 65536 MB');
    if (!Number.isInteger(allocatorSizeGiB) || allocatorSizeGiB < 1)
        throw new Error('allocatorSizeGiB must be a positive integer (minimum 1)');

    volumeData.tpvCount = 0;
    volumeData.cdvConfig.maxTPVs = maxTPVs ?? 512;
    volumeData.cdvConfig.allocatorSizeGiB = allocatorSizeGiB ?? 1;

    // After volume is created and chunk layout is known:
    await cdvTomaAutoAttach.initCDV(createdVolume);   // §5
}

if (volumeData.volumeClass === consts.volumeClass.TPV) {
    const { cdvId, tpvExtentSizeKB, virtualSizeGB, maxVirtualSizeGB } = volumeData.tpvConfig || {};

    const cdv = await db.volumes.findOne({ _id: cdvId, volumeClass: 'CDV' });
    if (!cdv) throw new Error('Parent CDV not found');
    if (cdv.tpvCount >= cdv.cdvConfig.maxTPVs)
        throw new Error(`CDV is at capacity (${cdv.cdvConfig.maxTPVs} TPVs)`);
    if (virtualSizeGB > cdv.capacity)
        throw new Error('virtualSizeGB cannot exceed parent CDV capacity');

    if (!consts.tpvExtentSizeKBValues.includes(tpvExtentSizeKB))
        throw new Error('tpvExtentSizeKB must be a power-of-2 between 64 and 65536 KB');
    if (tpvExtentSizeKB > cdv.cdvConfig.cdvExtentSizeMiB * 1024)
        throw new Error(`tpvExtentSizeKB (${tpvExtentSizeKB}) cannot exceed cdvExtentSizeMiB * 1024`);

    volumeData.tpvConfig.cdvUUID = cdv.uuid;
    volumeData.capacity = virtualSizeGB;
    volumeData.status = 'initializing';

    // Insert TPV record, then atomically increment CDV.tpvCount:
    await db.volumes.updateOne({ _id: cdvId }, { $inc: { tpvCount: 1 } });
}

// Strip class-specific configs from records that don't need them:
if (!volumeData.volumeClass || volumeData.volumeClass === consts.volumeClass.REGULAR) {
    delete volumeData.cdvConfig;
    delete volumeData.tpvConfig;
}
```

Extend `updateVolume()` to handle CDV-specific fields, and add new exported functions `updateTPV`, `deleteTPVs`, `extendTPV`:

```js
// In updateVolume(), branch on volumeClass === 'CDV':
//   Mutable: name, description, cdvConfig.maxTPVs
//   Immutable: cdvExtentSizeMiB, allocatorSizeGiB — strip from payload before update
//   If maxTPVs < current tpvCount: accept; no error.
//     Existing TPVs are unaffected; createTPV will reject new ones until tpvCount < maxTPVs.

async function updateTPV({ _id, description }, user) {
    // Mutable: description only.
    // Everything else (name, volumeClass, every field in tpvConfig — including
    // cdvId, virtualSizeGB, tpvExtentSizeKB, exclusiveClient, etc.) is immutable
    // after creation and is silently ignored if present in the payload.
    // Grow is handled by the separate extendTPV() entry point below.
}

async function deleteTPVs(ids, user) {
    // For each id:
    //   1. Load TPV; error if not found or not TPV class
    //   2. Require tpvConfig.exclusiveClient === null
    //   3. sendCDVAllocatorFreeAll(cdvUUID, tpvUUID, allocatorSizeGiB, cdvExtentSizeMiB)  [via modules/kafka.js]
    //   4. db.volumes.updateOne({ _id: cdvId }, { $inc: { tpvCount: -1 } })
    //   5. db.volumes.deleteOne({ _id })
    // Return aggregate result (same shape as existing deleteVolumes)
}

async function extendTPV({ tpvId, newSizeGB }, user) {
    // 1. Load TPV
    // 2. Validate: newSizeGB > current && <= tpvConfig.maxVirtualSizeGB
    // 3. Update: tpvConfig.virtualSizeGB, capacity
    // 4. If exclusiveClient !== null: send UpdateVolume MCS Kafka message with new virtualSizeGB
}
```

Add a `$lookup` aggregation for TPV queries to denormalize `tpvConfig.cdvName`:

```js
// Appended to the MongoDB aggregation pipeline only when filter includes
// volumeClass: 'TPV', to avoid overhead on regular volume fetches:
{ $lookup: {
    from: 'volume',
    localField: 'tpvConfig.cdvId',
    foreignField: '_id',
    as: '_cdv',
    pipeline: [{ $project: { name: 1 } }]
}},
{ $addFields: { 'tpvConfig.cdvName': { $arrayElemAt: ['$_cdv.name', 0] } }},
{ $unset: '_cdv' }
```

#### `modules/client.js`

Add `attachTPV(clientID, tpvID, opts)`:
1. Load TPV and parent CDV from MongoDB.
2. Inspect `client.attachments[cdvUUID].referenceIDs` for an existing `tpv:<tpvUUID>` or `toma:<cdvUUID>` entry.
3. If CDV is **not** already attached to this client, send `AttachVolumes` for CDV with `isHidden: true` and `reservation.mode = SHARED_READ_WRITE`. Wait for `block_devices` confirmation.
4. Add `tpv:<tpvUUID>` to `client.attachments[cdvUUID].referenceIDs`.
5. Send `AttachVolumes` for TPV with `reservation.mode = EXCLUSIVE_READ_WRITE` and `cdvConf` inline.
6. Set `tpvConfig.exclusiveClient = clientID`.

Add `detachTPV(clientID, tpvID, opts)`:
1. Send `DetachVolumes` for TPV.
2. After confirmation, remove `tpv:<tpvUUID>` from `client.attachments[cdvUUID].referenceIDs`.
3. If no other `tpv:*` referenceID remains (all TPVs relying on this CDV have detached from this client) **and** no `toma:*` referenceID exists — the CDV is no longer required on this client — send a hidden `DetachVolumes` for the CDV.
4. Clear `tpvConfig.exclusiveClient`.

```mermaid
sequenceDiagram
    participant M as Management
    participant C as Storage Client

    rect rgb(235,245,255)
        Note over M,C: attachTPV
        M->>C: AttachVolumes(CDV, isHidden=true, SHARED_RW)
        C-->>M: block_devices confirmed
        Note over M: add tpv:tpvUUID to client referenceIDs
        M->>C: AttachVolumes(TPV, EXCLUSIVE_RW, cdvConf inline)
        C-->>M: block_devices confirmed
        Note over M: set tpvConfig.exclusiveClient = clientID
    end

    rect rgb(255,245,235)
        Note over M,C: detachTPV
        M->>C: DetachVolumes(TPV)
        C-->>M: detach confirmed
        Note over M: remove tpv:tpvUUID from referenceIDs
        alt no tpv:* and no toma:* referenceIDs remain
            M->>C: DetachVolumes(CDV, hidden)
            C-->>M: detach confirmed
        end
        Note over M: clear tpvConfig.exclusiveClient
    end
```

*Figure 4: TPV attach/detach orchestration. The CDV is hidden-attached before the TPV is attached. The CDV is detached only when all TPV and TOMA references for that CDV are gone from this client.*

#### Involuntary TPV detach — CDV cleanup requirement

**Critical invariant:** A client that loses access to a TPV **must also lose access to the underlying CDV** (unless it still holds other `tpv:*` or `toma:*` references on that CDV). Writes to a TPV are physically performed on the CDV — the TPV is a virtual entity with no storage of its own. Leaving the CDV attached after TPV detach would allow an evicted client to continue issuing RDMA writes into the CDV, corrupting other TPVs.

TOMA cannot enforce this on its own. The CDV reference tracking (`tpv:<tpvUUID>` entries in `client.attachments[cdvUUID].referenceIDs`) and the `tpvConfig.exclusiveClient` marker are MongoDB constructs managed entirely by the management layer. Therefore **management must handle CDV cleanup in every code path that can detach a TPV**, not only the normal user-initiated detach.

All three involuntary detach paths in `modules/client.js` call `cleanupTPVReferencesForDetachedClient()`. Rather than manipulating attachment records directly, this helper delegates to the standard `scope.detachVolumes()` orchestration for each TPV on the detached client, passing `referenceID: 'tpv:<tpvUUID>'`. That reuses the same code path user-initiated detaches take — it removes the `tpv:<tpvUUID>` entry from `client.attachments[cdvUUID].referenceIDs`, conditionally detaches the CDV when no `tpv:*` or `toma:*` references remain, and clears `tpvConfig.exclusiveClient`. Keeping the cleanup on the standard path avoids drift between the "normal" and "involuntary" code-paths as attach/detach semantics evolve.

- **Preemption** (`detachPreemptedClients`) — `cleanupTPVState` step calls `cleanupTPVReferencesForDetachedClient` for every preempted client.
- **Stale client cleanup** (`removeAlreadyDetachedAttachments`) — `cleanupTPVState` step calls `cleanupTPVReferencesForDetachedClient` after reservation update.
- **Client deletion** (`deleteClient`) — fire-and-forget call to `cleanupTPVReferencesForDetachedClient` after `findOneAndDelete`.

#### `modules/kafka.js`

- Register consumer handler for `CDVCapacityWarning` messages from TOMA. On receipt: trigger CDV extend flow (reuse existing volume extend logic).
- Add `sendCDVAllocatorFreeAll(cdvUUID, tpvUUID, allocatorSizeGiB, cdvExtentSizeMiB)` — publishes `CDVAllocatorFreeAll` message to TOMA.

### 1.5 UI Changes

See Part 8 for complete file-by-file implementation detail. Summary:

- **Regular Volumes table (`/volumes`)**: Two filter checkboxes to the right of the Delete/Rebuild buttons: "Show regular volumes" and "Show CDVs", both checked by default. TPVs are never shown in this table (they have their own page).
- **Create/Edit Volume dialog**: "Use as CDV" toggle appears on new-volume forms. When toggled on, CDV-specific fields appear (`cdvExtentSizeMiB`, `allocatorSizeGiB`, `maxTPVs`). In edit mode, `cdvExtentSizeMiB` and `allocatorSizeGiB` are shown read-only; `maxTPVs` remains editable. Any RAID level is allowed.
- **New "Thin Provisioning" sidebar section** (top-level, after Volumes): one sub-item "TPV List" at `/thin-provisioning/tpv`.
- **TPV list page**: FiltSortTable with columns Name, Parent CDV, Virtual Size, Max Size, Client, Status. Parent CDV column is filterable.
- **Attach dialog**: Informational note when selecting a TPV to attach.

---

## Part 1.5 — Allocator Satellite Volume (`<CDV>-mgmt`)

### 1.5.1 Problem Statement — Why a Satellite Volume

The original design (§2.6, §2.7) relied on two claims to keep a replaced-but-still-alive allocator from corrupting the CDV's allocator area:

1. A server-side `nvmeibt_raft_is_raft_valid()` gate on `handle_cdv_alloc_extent`.
2. Client-side generation fencing that discards stale allocator responses.

Neither prevents the actual write. The RAFT-majority gate only fires when *this* TOMA is in a minority partition; a normally-running ex-allocator that was replaced by topology change is still in the RAFT majority and its local `alloc->allocator_toma_id`/`allocator_generation` cache remains unchanged until it is either restarted or receives a `CDV_ALLOCATOR_NOTIFY`, which §2.6 explicitly sends **only to the new allocator**. In that window a stale allocator will continue accepting `CDV_ALLOC_EXTENT` requests, pick a "free" extent index from its own in-memory state, and issue `cdv_async_write_record` / `cdv_async_write_header` to `[0, A)` — concurrently with the new allocator doing the same. Both writes can succeed, both clients see success responses, and the on-disk allocator state is corrupted (two TPVs assigned the same extent index, oscillating header generations, or cross-TPV data exposure once the duplicated extent is written).

Client-side generation fencing only discards stale **responses** on the client; it does not stop the old allocator's disk I/O. There is no mechanism in NVMesh today that fences the CDV itself against writes from a node that has been logically replaced.

### 1.5.2 How NVMesh Actually Enforces Exclusivity — Preemption

NVMesh enforces `EXCLUSIVE_READ_WRITE` via per-volume **preemption**, not via target-side identity or MR fencing:

- Management owns a monotonic `reservation.version` per volume (`modules/client.js` `getTransitionQuery`).
- On a reservation change, management publishes `ReservationModeChange` (Kafka, `ManagementToTOMA.reservationModeChange`, `modules/client.js:1510` `sendReservationModeChangeMessageToAllTargets`) carrying the new version to every TOMA serving the volume.
- TOMA stores the new version in `active_reservation_mode_version` per segment (`toma/nvmeibt_seg_active.h:148`, `toma/nvmeibt_register.h:198`).
- Every client I/O carries a `reservation_mode_version` (`toma/clnt/nvmeibt_client_protocol.h:389`). On each request TOMA compares: **if TOMA's version is higher than the client's, the I/O is rejected at the target.**
- Clients that remain valid are told the new version via a topology push and bump their local cache; preempted clients are not notified and continue to issue I/O at the old version, which the target then rejects.

This is a real server-side admission check — not sender-trusted — keyed on a monotonic version rather than on identity. It is whole-volume in its scope.

### 1.5.3 Options Considered

**Option A — per-range exclusivity on the CDV.** Mark `[0, A)` of the CDV as reservation-fenced for the allocator TOMA while `[A, end)` stays shared-RW for TPV clients. Because `reservation_mode_version` is per-volume and not per-range or per-client-identity, implementing this would require one of: a per-offset-range reservation version in the target admission path, a per-client-identity reservation version, or a rule that the allocator TOMA may never also be a TPV client of the same CDV. All three are new, load-bearing extensions to the core exclusivity mechanism, touching the data-plane hot path.

**Option B — satellite allocator volume.** Introduce a dedicated small volume whose only purpose is to hold what would otherwise live in `[0, A)` of the CDV. The satellite is held `EXCLUSIVE_READ_WRITE` by the current allocator TOMA. Re-election is a plain preempt on the satellite: management bumps `reservation.version`, the new allocator attaches at the new version, and the old allocator's writes are rejected at the target by the existing version check. TPV clients hold attachments to the CDV, which has an independent `reservation.version`, and are not affected.

**Decision: Option B.** Option B uses the existing preemption mechanism at its native granularity (per-volume, whole-attachment) on a volume whose purpose matches exactly what the mechanism supports. Option A requires inventing a new dimension of reservation versioning inside the target I/O admission path and has no standalone benefit. The cost of Option B is management-layer coupling between two volumes and a placement constraint — ordinary work in well-covered code.

### 1.5.4 Implementation Spec

#### 1.5.4.1 Volume layout

- Every CDV has a **satellite volume** named `<CDV>-mgmt`. It is `1 GiB` in size (matches today's default `allocatorSizeGiB = 1`). Future sizing parity: if `allocatorSizeGiB` becomes tunable, the satellite volume size tracks it one-for-one.
- The CDV itself **no longer reserves `[0, A)` for the allocator**. The CDV's user-visible capacity equals its on-disk data capacity; the leading allocator-area region is gone from the CDV on-disk layout. All user-facing presentations of CDV size (UI, CLI, REST, mNDU, CSI) show the CDV capacity without any satellite overhead.
- The satellite volume holds exactly what used to live in `[0, A)`: the 4 KB header and the `cdv_extent_md[]` array. Offset math in `nvmeibt_cdv_alloc.c` is relative to the satellite volume's offset 0 (not to the CDV's offset 0). Data extents now start at CDV offset `0`.

#### 1.5.4.2 Naming and sizing constraints

- **CDV name limit: 16 characters.** `<CDV>-mgmt` is therefore ≤ 21 characters, well within NVMesh's volume name limit. Enforced in the create-CDV path (REST validation + UI form validation).
- `<CDV>-mgmt` is a reserved suffix; regular volume creation rejects names ending in `-mgmt`.

#### 1.5.4.3 Lifecycle — atomic create / delete, CDV-only extend

- **Create.** `POST /volumes/save` for a CDV allocates `capacity + 1 GiB` of raw disk capacity and writes **both** documents (`<CDV>` and `<CDV>-mgmt`) in a single Mongo operation. If either write fails the operation is rolled back before any Kafka traffic is emitted. No state is published to TOMA/clients until both volumes are durable.
- **Delete.** Deleting a CDV deletes both volumes in one operation. Users cannot delete the satellite independently; the satellite has no delete affordance in UI or REST (see §1.5.4.5).
- **Extend.** Volume-extend on a CDV extends **only** the CDV. The satellite size is fixed at creation (1 GiB covers ~44.7M extents — sufficient for any realistic CDV). This keeps the Mongo update path single-document.
- **Resize-down / encryption / other mutations** on the CDV do not touch the satellite.

This "allocate raw + write two docs + single rollback" discipline keeps the Mongo transactional surface the same as today's single-volume create path — which is important because NVMesh Mongo writes are not multi-document atomic.

#### 1.5.4.4 Attach discipline — allocator-driven, not management-driven

- **TOMAs no longer auto-attach CDVs.** The existing `cdvTomaAutoAttach` mechanism (Part 5) was motivated by the need to give TOMA write access to `[0, A)`. With the allocator area moved to the satellite, the CDV itself requires no TOMA attachment. The auto-attach module becomes allocator-volume-scoped (see below) and CDV attachment is driven solely by TPV client flows.
- **Allocator-initiated attach.** When RAFT elects an allocator TOMA (§2.6 election), the elected TOMA sends a new Kafka message `TOMAToManagement.attachSatelliteRequest(cdvUUID, tomaHostname)` to management. Management responds by:
  1. Bumping the satellite's `reservation.version` and publishing `ReservationModeChange` to all targets of the satellite with `reservationMode = EXCLUSIVE_READ_WRITE`.
  2. Attaching the satellite to the requesting TOMA in `EXCLUSIVE_READ_WRITE` with `preempt = true`, `isDetachOthers = true`. This reuses the existing preempt path (`modules/client.js:3513-3539`) verbatim — the prior allocator, if any, is the "other" client being preempted.
- **No REST attach.** `POST /clients/attachVolumes` rejects any attach to a `-mgmt` volume. The satellite is attachable only via the internal Kafka request above. The UI never surfaces an attach control for satellites.
- **Detach on re-election.** When a new allocator is elected, the old allocator's satellite attachment is preempted by the new allocator's attach request (above). No explicit detach message is required — preemption is the mechanism.
- **Detach on TOMA failure.** Standard stale-client cleanup (`removeAlreadyDetachedAttachments`) applies; the satellite follows the same rules as any other EXCLUSIVE_READ_WRITE volume.

#### 1.5.4.5 UI presentation

- The satellite appears in the **Volumes table as a child row beneath its CDV**, named `<CDV>-mgmt`. It shows the same status columns as any volume (health, attached targets, capacity).
- **No selection checkbox** for satellite rows — bulk-delete cannot target it.
- **No edit button** — the satellite is not editable.
- No create flow exposes satellites; they only come into existence via CDV creation.
- The Thin Provisioning page does not show satellites at all; they belong to the CDV and are a CDV implementation detail.

#### 1.5.4.6 TOMA kernel changes

- All code in `toma/nvmeibt_cdv_alloc.c` that currently reads/writes the CDV's `[0, A)` range (`cdv_async_write_record`, `cdv_async_write_header`, `cdv_ondisk_scan`, `cdv_ondisk_scan_async`) now operates on the satellite volume. Offsets become relative to the satellite: header at offset 0; `cdv_extent_md[i]` at offset `4096 + i * sizeof(cdv_extent_md)`.
- The satellite's UUID is resolved at allocator-initialization time from the CDV's management metadata (delivered in the topology push for the CDV, which now also carries `allocator_volume_uuid`).
- On-disk header layout is unchanged — it's just at a different physical location.

#### 1.5.4.7 Client kernel changes

- **None.** Clients attach the CDV exactly as today, at `SHARED_READ_WRITE`. TPVs continue to allocate CDV extents via `CDV_ALLOC_EXTENT` on the admin channel. Clients do not attach the satellite and are not aware of it.
- Offset translation in `nvmeibc_tpv.c` changes by a constant: the first CDV data extent is now at CDV offset `0` instead of `A`. This is a single `A = 0` simplification in `tpv_cdv_offset_for_extent` and related helpers.

#### 1.5.4.8 Correctness argument

- The allocator area can only be written by a holder of the satellite's `EXCLUSIVE_READ_WRITE` reservation at the current `reservation.version`.
- Re-election bumps the version via the standard preempt path. The old allocator's I/O carries the old version; the target rejects it per §1.5.2. No identity cache, no notification, and no cooperation from the old allocator is required.
- The node-may-also-be-a-TPV-client concern dissolves: the TPV client attaches the CDV, which has an independent `reservation.version`. Preempting the satellite does not touch the CDV.
- The RAFT-majority gate and the `CDV_ALLOCATOR_NOTIFY` monotonicity guard in §2.6 become defense-in-depth rather than the primary safety argument. §2.6 / §2.7 will be revised in a follow-up to reflect this.

### 1.5.5 What This Supersedes

- **§2.2 On-Disk Format:** The allocator area moves out of the CDV and into the satellite. The `allocatorSizeGiB` CDV property is replaced by the fixed-1-GiB satellite size; `cdvConfig.allocatorSizeGiB` becomes obsolete (retained in the schema only for pre-migration volumes, if any).
- **§2.6 Election and Identity Propagation:** Election unchanged. Identity propagation simplifies: no need for `CDV_ALLOCATOR_NOTIFY` unicast; the attach-satellite Kafka request + preempt is the propagation mechanism. The on-disk header-generation monotonicity guard stays as belt-and-suspenders but is not safety-critical.
- **§2.7 Split-Brain Protection:** Write-before-respond stays. The RAFT-majority gate (`nvmeibt_raft_is_raft_valid`) stays but is no longer load-bearing — the reservation-version check at the target is the authority.
- **Part 5 CDV Auto-Attachment:** Becomes "Allocator Volume Attachment" and applies only to the satellite. The CDV is no longer auto-attached to TOMAs.

---

## Part 2 — TOMA: CDV.allocator

### 2.1 Overview

The CDV.allocator is a role held by exactly one TOMA at any time. It owns all CDV\_extent allocation and reclamation, persists its state in the first `allocatorSizeGiB` GB of the CDV (a configurable property, default 1 GB), and is the sole authority on which extents are free, allocated, or pending zeroing.

**Communication**: Clients send allocation requests to the allocator TOMA via the **per-disk ADMIN channel** — the same channel already used for journal range queries, resource location, and other control-plane operations. No Kafka is involved in the allocation hot path.

**Discovery**: The identity of the current allocator TOMA (`allocator_toma_id`) is included in the CDV topology that TOMAs already push to subscribing clients at attach time and whenever it changes.

**Election**: RAFT-committed state. Sticky — does not change when the RAFT leader rotates, only when the current allocator TOMA leaves the RAFT group (§2.6).

### 2.2 On-Disk Format (Allocator Area)

The allocator area occupies the first `allocatorSizeGiB` GiB of the CDV (bytes `0` to `A`). Data CDV\_extents follow immediately after. Two independent size parameters:

- $A = \texttt{allocatorSizeGiB} \times 1\,\text{GiB}$ — allocator area size (configurable CDV property, default 1 GiB)
- $E = \texttt{cdvExtentSizeMiB} \times 1\,\text{MiB}$ — CDV\_extent size (configurable CDV property)

The allocator region is organised as an array of 4 KiB blocks — the smallest atomic unit the CDV block stack guarantees. The first block is the header; each subsequent block is one record describing one CDV\_extent. This wastes space relative to a packed 24-byte layout, but each header/record write is atomic on its own, which eliminates torn-write concerns during allocate/free and during allocator migration between TOMAs (§2.6).

```
[0 .. 4 KiB)                CDV.allocator header (cdv_alloc_ondisk_header, 4 KiB block)
[4 KiB .. A)                cdv_alloc_ondisk_record[N] — one 4 KiB block per data CDV_extent
[A   .. A+E)   CDV_extent[0]  — reserved; never available for TPV data
[A+E .. A+2E)  CDV_extent[1]  — first allocatable data extent
...
```

```mermaid
graph LR
    H["[0, 4 KiB)\nHeader (4 KiB block)\nmagic, version,\nallocator_toma_id,\nallocator_generation"]
    R["[4 KiB, A)\nPer-extent records\ncdv_alloc_ondisk_record\n(4 KiB per slot)"]
    D0["CDV_extent[0]\n[A, A+E)\nreserved\n(never TPV data)"]
    D1["CDV_extent[1]\n[A+E, A+2E)\nFirst allocatable"]
    D2["..."]
    DN["CDV_extent[N-1]\nLast allocatable"]
    H --> R --> D0 --> D1 --> D2 --> DN
```

*Figure 1: CDV physical layout. The allocator area (A bytes) is a 4 KiB header plus a flat array of 4 KiB per-extent records. All allocatable data extents start at CDV\_extent[1].*

With a 1 GiB allocator region and 4 KiB blocks this supports $(1\,\text{GiB} / 4\,\text{KiB}) - 1 = 262{,}143$ extent slots. Larger allocator regions scale this linearly.

#### Header

```c
/* toma/nvmeibt_cdv_alloc.h */
#define CDV_ONDISK_BLOCK_SIZE  4096U
#define CDV_ONDISK_MAGIC       0x43444D31U   /* 'CDM1' */
#define CDV_ONDISK_VERSION     1
#define NVMEIBT_CDV_UUID_STRLEN 64           /* hostname / UUID string length */

struct cdv_alloc_ondisk_header {
    uint32_t magic;                                 /* CDV_ONDISK_MAGIC */
    uint32_t version;                               /* CDV_ONDISK_VERSION */
    uint64_t total_data_extents;
    char     allocator_toma_id[NVMEIBT_CDV_UUID_STRLEN];  /* 64-byte hostname string */
    uint64_t allocator_generation;
    uint32_t crc32;
    uint8_t  reserved[CDV_ONDISK_BLOCK_SIZE - 4 - 4 - 8 - 64 - 8 - 4];
} __attribute__((packed));
```

The header stores the allocator-identity handshake state (`allocator_toma_id`, `allocator_generation`) as the durable source of truth across full cluster restart (see §2.6). Geometry parameters `allocatorSizeGiB` and `cdvExtentSizeMiB` are CDV-volume properties carried in the Kafka `AddVolume`/`UpdateVolume` path and are not repeated in the on-disk header.

#### Per-extent record

```c
#define CDV_ONDISK_RECORD_FLAG_ALLOCATED     0x01
#define CDV_ONDISK_RECORD_FLAG_NEEDS_ZEROING 0x02  /* freed but blocked from reuse */

struct cdv_alloc_ondisk_record {
    uint8_t  flags;                                 /* CDV_ONDISK_RECORD_FLAG_* */
    uint8_t  reserved1[7];
    char     tpv_uuid[NVMEIBT_CDV_UUID_STRLEN];     /* 64-byte owner string; zeroed if free */
    uint32_t crc32;
    /* Zeroing geometry — not CRC-covered; valid only when NEEDS_ZEROING is set.
     * Stored here so the zeroing worker can recover geometry on restart without
     * requiring a separate management query.
     */
    uint32_t zeroing_allocator_size_gib;
    uint32_t zeroing_cdv_extent_size_mib;
    uint8_t  reserved2[CDV_ONDISK_BLOCK_SIZE - 1 - 7 - 64 - 4 - 4 - 4];
} __attribute__((packed));
```

Record `i` at byte offset `4 KiB × (i + 1)` describes CDV\_extent `i`. Records are read in bulk at attach time (`cdv_ondisk_scan_async`) to rebuild the in-memory allocator state. There is **no CDV-wide L1/L2 tree or `cdv_extent_type` enum** in the on-disk format — the previous design iteration with `CDV_EXTENT_{L1,L2,L2A,L3,DATA}` enum values and a flat 24-byte record has been superseded. Per-TPV mapping trees (§3.4) are stored inside the TPV's own data CDV\_extents, not in the allocator region.

### 2.3 In-Memory State

New files: `toma/nvmeibt_cdv_alloc.c` / `toma/nvmeibt_cdv_alloc.h`

```c
/* Skeleton — see toma/nvmeibt_cdv_alloc.h for full definition. */
struct nvmeibt_cdv_alloc {
    char                  cdv_uuid[NVMEIBT_CDV_UUID_STRLEN]; /* first field; hash key */
    uint64_t              total_data_extents;
    uint64_t              n_allocated;
    uint64_t              allocator_generation;
    char                  allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];
    /* Per-extent book-keeping: linked list of nvmeibt_cdv_extent_entry (not a
     * flat array), plus a per-CDV IO workqueue for zeroing and ondisk scan. */
    struct xdlist         extents;
    struct work_struct    zeroing_work;
    struct work_struct    ondisk_scan_work;
    spinlock_t            handler_lock;
    /* ... admission floor, registrant hooks, RAFT identity apply state ... */
};

// alloc():     write record block (generation++, flags=ALLOCATED, tpv_uuid);
//              on success, link new nvmeibt_cdv_extent_entry into `extents`.
// free():      clear flags=ALLOCATED (optionally set NEEDS_ZEROING) and rewrite
//              record; enqueue zeroing_work if NEEDS_ZEROING set.
// free_all():  scan `extents`, free every record where tpv_uuid matches.
```

```mermaid
stateDiagram-v2
    [*] --> FREE : CDV init (all records zeroed)
    FREE --> ALLOCATED : CDV_ALLOC_EXTENT (flags=ALLOCATED, tpv_uuid stamped)
    ALLOCATED --> NEEDS_ZEROING : CDV_FREE_EXTENT or TPV delete (free_all), when cdv_extent_zero_on_free enabled
    ALLOCATED --> FREE : CDV_FREE_EXTENT, when cdv_extent_zero_on_free disabled
    NEEDS_ZEROING --> FREE : zeroing worker completes; record rewritten with flags=0
```

*Figure 6: CDV extent lifecycle as reflected in the on-disk record. `tpv_uuid` identifies the owning TPV while `ALLOCATED` is set. `NEEDS_ZEROING` blocks reuse until the background zeroing worker has zeroed the physical extent; zeroing geometry is stored in the record so it survives restarts. CDV\_extent[0] is reserved at CDV creation time and never transitions through the allocator.*

### 2.4 Cold Recovery (TOMA restart)

Runs after CDV EC cold recovery completes, while IO gates are still closed (`cdv_ondisk_scan_async` — see §2.6):

1. Read the 4 KiB header block at CDV offset 0; validate magic `CDV_ONDISK_MAGIC` and `crc32`.
2. Read all `cdv_alloc_ondisk_record` blocks (4 KiB each), validate per-record `crc32`, skip records failing validation.
3. For each record with `flags & ALLOCATED`, create an `nvmeibt_cdv_extent_entry` and link it into `alloc->extents`. Records with `NEEDS_ZEROING` are also kept in `alloc->extents` — they are blocked from reuse until the zeroing worker completes and the record is rewritten with `flags = 0`.
4. Schedule any `NEEDS_ZEROING` records onto the per-CDV zeroing workqueue.
5. Open IO gates; resume allocator service.

### 2.5 CDV Topology: Allocator Identity

Extend the CDV topology payload sent by TOMA to subscribing clients during `DD_STG_TOMA_REREG`:

```c
struct nvmeibc_cdv_toma_topology {
    // ... existing topology fields ...
    u8  allocator_toma_id[...];   // node ID of the current CDV.allocator TOMA
    u64 allocator_generation;     // epoch counter; increments on every allocator change
};
```

The client stores `(allocator_toma_id, allocator_generation)` in its `nvmeibc_tpv` struct. When the allocator changes, TOMA pushes an updated topology to all CDV subscribers using the existing topology-update mechanism.

### 2.6 Allocator Election and Identity Propagation

**Allocator identity is two fields on the RAFT-replicated first-pRAID topology record.** `allocator_toma_id` and `allocator_generation` are added to `nvmeibt_praid_topo_ctx` (and its wire/persist analog `nvmeibt_praid_serialized_topo`), travel in the TOPO TLV of the existing `AppendEntries` commit path, and are applied on every TOMA through the standard `applied_praid_lot` update. There is no parallel message and no custom retry logic.

**Election (RAFT leader only):**
1. `nvmeibt_topology_calc_topology()` runs only on the leader. For every CDV pRAID with `stripe_idx == 0` and `PRAID_REGISTRANTS_SYNC_CMD_STABLE`, the leader builds a candidate list as the intersection of (a) distinct owner hostnames of the pRAID's data disk segments and (b) alive RAFT members (`raft_member->is_alive_for_topo`). Scoping to segment owners co-locates the allocator with the CDV data and with the satellite; the RAFT-liveness filter drops a dead segment owner within the heartbeat timeout (~200 ms) without having to wait for the pRAID topology to catch up.
2. `cdv_alloc_elect(cdv_uuid, candidates, out_new_identity)`:
   - Current `allocator_toma_id` still in candidates → sticky, no mutation.
   - Otherwise pick one candidate (random tie-break) → stage `(new_toma_id, current_gen + 1)` into `calculated_praid_lot.topo_ctx`.
3. The staged `calculated_praid_lot` is promoted to `baseline_praid_lot` via the existing `nvmeibt_praid_leader_we_have_a_new_baseline()`, serialized into `praid_wire_topo`, shipped in the next `AppendEntries`, and — on majority-ack — applied on every follower's `applied_praid_lot`.

**Identity apply — every TOMA, leader and followers:**
1. In the topology-apply hook for the CDV pRAID (`update_applied_topology` path), diff `applied_praid_lot.topo_ctx.allocator_*` against the previously applied values for this pRAID.
2. Generation increased and `allocator_toma_id == my_hostname`, local state not `ACTIVE`:
   - Transition `cdv_alloc_hash[cdv_uuid]` to `AWAITING_SATELLITE_ATTACH`.
   - Fire `cdv_send_attach_satellite_request()` — Stage A of the satellite-attach handshake (§1.5.4.4).
3. Generation increased and `allocator_toma_id != my_hostname`, previous was this node:
   - Demote: transition to `NOT_ALLOCATOR`, drain the I/O WQ, close cached fds, clear satellite state.
4. Generation unchanged, or `allocator_toma_id` points elsewhere and was already not us: no-op.

Because this hook runs inside the deterministic, linearized topology-apply path — which every TOMA traverses for every committed topology change and which is replayed on restart — every TOMA converges on the same allocator identity at the same committed log index.

**Client propagation is unchanged** (§2.5). When a TOMA serving a client applies the new topology, it pushes the updated `(allocator_toma_id, allocator_generation)` to its subscribed clients as part of the existing CDV topology push.

```mermaid
sequenceDiagram
    participant L as RAFT Leader TOMA
    participant F as Followers (incl. elected allocator)
    participant M as Management
    participant C as Client

    Note over L: topology_calc_topology → cdv_alloc_elect
    L->>L: calculated_praid_lot.topo_ctx.<br/>allocator_toma_id = chosen<br/>allocator_generation++
    L->>F: AppendEntries (TOPO TLV carries<br/>new allocator fields)
    F->>F: persist + apply<br/>(applied_praid_lot)
    F-->>L: ACK (majority commits)

    Note over F: topology-apply hook on every TOMA
    rect rgb(240,240,240)
        Note over F: On the elected TOMA only:<br/>state → AWAITING_SATELLITE_ATTACH
        F->>M: attachSatelliteRequest (Stage A)
        M-->>F: attachSatelliteResponse (Stage B)
        F->>F: scan satellite; state → ACTIVE
    end

    F->>C: CDV topology push (new allocator + gen)
    C->>F: CDV_ALLOC_EXTENT
    F-->>C: CDV_ALLOC_OK | WRONG_GEN
```

*Figure 9: Election and identity propagation via RAFT topology. The leader writes the new `(allocator_toma_id, allocator_generation)` into the same topology record that carries every other pRAID state change. RAFT delivers it to every follower reliably and in log order. The elected allocator detects its new role inside the apply hook and fires Stage A of the satellite-attach handshake.*

**Correctness and failure modes.**
- **Delivery reliability is RAFT's.** Once committed, the log entry is delivered to every follower; a disconnected follower catches up on reconnect. The "unicast was dropped and Stage A never fired" failure mode that motivated this design cannot occur: either the entry is committed and every follower eventually applies it, or it is not committed and no TOMA acts on it.
- **Monotonicity is by log order.** Only the leader writes `allocator_generation`, as part of the atomic topology commit. Followers apply in log order; a late-arriving older-generation entry is impossible.
- **Cold start.** After a full cluster restart the first elected leader runs `calc_topology` and re-writes the identity if needed. The newly-elected allocator's first topology-apply delivers the committed identity and fires Stage A normally — no special recovery path.
- **Split-brain.** A partitioned minority TOMA cannot commit new topology entries; `handle_cdv_alloc_extent` fails the `nvmeibt_raft_has_majority()` gate (§2.7). The satellite's EXCLUSIVE_READ_WRITE hold (§1.5.4.8) blocks any residual data write from a stale allocator.

**Re-election on allocator departure** is mechanically identical to the initial election, driven by the RAFT-member candidate set: when the old allocator leaves the RAFT group, the sticky rule no longer matches, a new candidate is picked, `allocator_generation++` is staged into `calculated_praid_lot.topo_ctx`, and the commit propagates to every TOMA. The old allocator — if it comes back later — receives the higher-generation entry through catch-up replication and demotes itself in the same topology-apply hook.

**What this supersedes.** The following mechanisms from earlier iterations of this design collapse into the single RAFT-replicated field and the single topology-apply hook:
- `RAFT_MSG_CDV_ALLOC_NOTIFY` unicast and `nvmeibt_raft_send_cdv_alloc_notify` / `handle_notify` plumbing.
- `nvmeibt_cdv_alloc_send_notify_to_elected` + the `cdv_alloc_set_generation` post-send call introduced to patch a self-apply race.
- `cdv_alloc_elect`'s `out_proposed_gen` out-param (introduced by commit `0f4db106` to preserve `handle_notify` as the sole generation writer).
- The receive-side monotonicity guard in `handle_notify`.
- The cold-recovery contract that elect()'s sticky rule might be invoked against an empty local state after full-cluster restart — the RAFT-committed identity makes the bootstrap explicit.

Detailed migration plan, code-changes-by-file, and phased delivery are in `EmbedAllocatorInRaft.md`.

### 2.7 Allocation Transactionality and Split-Brain Protection

#### Write-before-respond

```
alloc():
  1. Lock allocator.
  2. Find free extent idx in free_bitmap.
  3. Write cdv_extent_md[idx] = {tpv_uuid, flags=0} to CDV disk. Wait for completion.
  4. Increment generation; write header to CDV disk. Wait for completion.
  5. Set bit in free_bitmap (RAM).
  6. Unlock.
  7. Send cdv_alloc_resp to client.
```

```mermaid
sequenceDiagram
    participant C as Client
    participant T as Allocator TOMA
    participant D as CDV Disk

    C->>T: CDV_ALLOC_EXTENT(tpv_uuid, req_id, client_generation)
    T->>T: lock, find free extent idx in free_bitmap
    T->>D: write cdv_extent_md[idx].tpv_uuid (durable)
    D-->>T: write confirmed
    T->>D: write header (generation++) (durable)
    D-->>T: write confirmed
    T->>T: set bit in free_bitmap, unlock
    T-->>C: cdv_alloc_resp(extent_index, allocator_generation)
    Note over C: install extent_index into L2/L3 leaf in tree
    Note over C: flush modified tree pages to CDV
    Note over C: add n_slots slot addresses to free_tpv_extents

    Note over C,T: Generation fencing on allocator failover
    T->>C: CDV topology push (allocator_generation++)
    C->>C: discard in-flight responses with old generation
    C->>T: retry CDV_ALLOC_EXTENT with new (toma_id + generation)
```

*Figure 5: Write-before-respond allocation protocol. Both the extent metadata and the header are written durably to disk before the response is sent. Generation fencing discards stale responses after allocator failover.*

Crash scenarios:
- Before step 3: client gets no response, retries. New allocator sees extent as free. Consistent.
- Between step 3 and step 7: client gets no response, retries. New allocator cold-recovers, finds extent allocated to this TPV. Client retries, gets a new extent. The first extent is "orphaned" — NVCK detects it (`tpv_uuid` set in `cdv_extent_md` but no corresponding leaf entry in the L1/L2/L3 tree). Recovery: clear the orphaned `cdv_extent_md` entry and return the extent to the free pool.

#### Generation fencing

Every `cdv_alloc_resp` carries `allocator_generation`. After re-election, clients receive topology with `allocator_generation++`. Clients discard responses with stale generation and retry with the new allocator.

If a partitioned old allocator writes `cdv_extent_md` entries, the new allocator sees them during cold recovery and skips those extents. They become orphans detected by NVCK.

#### Free path transactionality

```
free():
  1. Set cdv_extent_md[idx].flags |= NEEDS_ZEROING. Persist to disk.
  2. Keep bit set in free_bitmap (not yet reusable).
  3. Enqueue zeroing_work.
  4. Zeroing completes.
  5. Clear cdv_extent_md[idx].tpv_uuid. Persist to disk.
  6. Clear bit in free_bitmap (now reusable).
```

On crash between steps 1 and 5: cold recovery sees NEEDS\_ZEROING → re-enqueues zeroing before marking extent free.

### 2.8 CDV\_extent Allocation Protocol (ADMIN Channel)

New opcodes (add to `nvmeibc_config_ops` enum):

```c
NVMEIBC_MA_CDV_ALLOC_EXTENT = 0x20,
NVMEIBC_MA_CDV_FREE_EXTENT  = 0x21,
```

Message structs:

```c
// NVMEIBC_MA_CDV_ALLOC_EXTENT request:
struct nvmeibc_cdv_alloc_req {
    u8  tpv_uuid[16];
    u8  cdv_uuid[16];
    u64 req_id;              // monotonically increasing per-TPV, for idempotency
    u64 client_generation;   // allocator_generation the client believes is current
};

// Response:
struct nvmeibc_cdv_alloc_resp {
    u64 req_id;
    u64 extent_index;        // data CDV_extent index i; CDV byte offset = A + i * E
    u64 allocator_generation;
    u8  status;              // 0=OK, 1=CDV_FULL, 2=WRONG_GENERATION, 3=ERROR
};

// NVMEIBC_MA_CDV_FREE_EXTENT request:
struct nvmeibc_cdv_free_req {
    u8  tpv_uuid[16];
    u8  cdv_uuid[16];
    u64 extent_index;
};
```

Client sends to the admin channel of a disk belonging to the `allocator_toma_id` node. TOMA-side dispatches to `cdv_allocator_alloc()` / `cdv_allocator_free()`.

On `CDV_FULL`: TOMA sends `CDVCapacityWarning` to management. Client pauses write IOs needing new allocation until a topology push signals capacity is available.

On `WRONG_GENERATION`: Client re-fetches CDV topology via TOMA subscription, then retries.

### 2.9 Recovery Changes to Existing TOMA Code

In `toma/nvmeibt_recovery.c`:
- After EC recovery completes, before IO gates open: call `cdv_allocator_cold_recovery(cdv_uuid)`.
- Background scrubbing: skip unallocated extents by checking `free_bitmap[extent_idx]`.

### 2.10 Per-Client CDV Preemption

#### 2.10.1 The actual problem

A TPV is a virtual volume with no storage of its own. All TPV data writes land on the underlying **CDV**, which is attached `SHARED_READ_WRITE` to potentially many TPV clients at once. Consequently, fencing a TPV from a misbehaving client at the TPV layer is ineffective: once the client has populated its TPV `extent_map` with (virt_idx → CDV physical offset) entries, bio forwarding in `nvmeibc_tpv_cdv_submit_bio` sends writes directly from the client to CDV segments over RDMA, not through TOMA. The only way to stop a stale client's writes is to fence **that client specifically** from the CDV, without disturbing the other `SHARED` holders.

The earlier version of this section proposed a per-TPV fencing cookie validated by TOMA on allocation requests. That design fenced the allocation control path but not the data path — the stale client's already-mapped CDV writes were not affected. The subsequent P1/P2/P3 triad (volume-wide preempt with survivor auto-reattach; per-registrant threshold as a hot-path admission gate; parallel revoke message) proposed various ways to bridge that gap, each with significant costs. This section replaces all of them. See `TPV_PerClientCDVPreemption.md` for the full execution plan.

#### 2.10.2 Design — two orthogonal primitives

Per-client CDV preemption is expressed as two primitives that compose but do not overlap:

**Primitive A — CDV admission floor.** A monotonic `u64` maintained by management on every CDV, replicated to every TOMA serving that CDV's segments. Semantics: every incoming `REGISTER` against a CDV segment is rejected at registration time if the client's `reservation_mode_version` is below the CDV's current floor. Existing registrants at older versions are not affected — the floor is an additive admission gate, not a segment-transition marker. In particular, the floor does not interact with `seg_active->active_reservation_mode_version` or `seg_active->highest_reservation_mode_version` (`toma/nvmeibt_seg_active.h:148-151`) and does not put the segment into the "rejecting" state described at `toma/nvmeibt_register.c:1015-1031`.

**Primitive B — targeted registrant termination.** A new Kafka message `ManagementToTOMA.preemptClientFromCDV(clientID, cdvUUID, newFloor)`. The TOMA handler, under the per-CDV lock, does exactly two things in order:

1. Raise the CDV's local admission floor to `newFloor`.
2. Locate the client's active registrant on every segment of the CDV and terminate it via the existing `nvmeibt_register_terminate_reg_ctx` path. Drain in-flight I/O as that path already does for EXCLUSIVE preempts.

TOMA ACKs back to management on a new `TOMAToManagement.preemptClientFromCDVResponse` after both steps complete. The order (floor first, then termination) matters — see §2.10.4.

Both primitives are CDV-specific. Non-CDV volumes (including the allocator satellite `<CDV>-mgmt`, which has its own `EXCLUSIVE_READ_WRITE` reservation semantics per §1.5) do not have an admission floor and do not accept `preemptClientFromCDV`.

#### 2.10.3 Protocol

**CDV creation.** The CDV document gains `cdvConfig.admissionFloor: u64` (default 0). Floor 0 is the sentinel "no gate" — matches pre-feature behavior.

**Normal client attach.** `client.js:attachTPV` stamps the outgoing `AttachVolumes` message for the hidden CDV with `reservationModeVersion = cdv.cdvConfig.admissionFloor`. The client's subsequent `REGISTER` carries that value and is admitted.

**Preempt flow.**

1. Management detects the need to evict `clientA` from `cdvC` (TPV force-detach, stale-client cleanup, involuntary-detach path).
2. Management, in order (the ordering is load-bearing — see "crash recovery" below):
   (a) writes `EVICTING` into `client[clientA].attachments[cdvC].action` — reusing the existing `ATTACHING`/`DETACHING` state enum (`consts.volumeAttachmentActions`);
   (b) sets `cdvC.cdvConfig.admissionFloor = floor + 1` via `$max`.
3. Management publishes `preemptClientFromCDV(clientA, cdvC, newFloor)` on every `TOMA_COMMANDS` topic for the zones hosting `cdvC`'s pRaids.
4. Each TOMA processes the message: raise floor, terminate `clientA`'s registrants on every local CDV segment, drain, ACK.
5. Management collects ACKs fan-in (same pattern as `sendReservationModeChangeMessageToAllTargets` in `client.js:1511`). Once every TOMA has ACKed, management clears the `EVICTING` action, removes the `(clientA, cdvC)` Mongo attachment entry (stripping all `tpv:*` references), and clears `exclusiveClient` on every TPV `clientA` previously held on `cdvC`.
6. New TPV assignments proceed. Client B attaches TPV X; its hidden CDV attach is stamped with the new floor; registration succeeds.

**Crash recovery (ordering rationale for step 2).** The `EVICTING` mark is the recoverable signal. A management crash between steps 2(a) and 2(b) leaves `EVICTING` set with the floor at its old value; a reaper observes the `EVICTING` state on restart and resumes by calling the entry point again (floor bump is idempotent via `$max`, Kafka fan-out is idempotent because the TOMA handler uses `max` on its own floor and a second termination pass is a no-op). The reverse order — floor bumped first, `EVICTING` marked second — leaves no recoverable signal: a bumped floor with no `EVICTING` is indistinguishable from a completed eviction, so the reaper cannot know to finish the job. The true-atomic alternative is a Mongo transaction across the `volume` and `client` collections; it is available as a future hardening if the reaper's latency becomes operationally unacceptable.

**Re-admission flow for a cooperative evicted client.**

1. `clientA`'s kernel observes registration failure (reason `NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR`) or an in-flight `'P'` status on the CDV. Device enters `NCBD_PREEMPTED`. Per §3.8.1 (new), every TPV whose `cdv_vol` points to this CDV is torn down: `extent_map`s discarded, `cdv_alloc_work` cancelled, `gendisk` unregistered. This is the **cleanup barrier** — no stale CDV offsets remain in kernel memory.
2. `clientA` contacts management to re-attach a TPV on `cdvC` (via the normal attach path; no new RPC).
3. Management's attach gate refuses if `client[clientA].attachments[cdvC].action == 'EVICTING'` (eviction still in flight). The client retries.
4. Once eviction has cleared, management re-admits `clientA` normally, stamping the current floor on the new attach. If the TPV `clientA` was using has been reassigned to `clientB`, management refuses that TPV specifically (`exclusiveClient != null` for someone else) — but a fresh TPV attach for the same `clientA` on the same CDV is fine.

#### 2.10.4 Correctness walkthrough

Three paths a stale client might try to keep writing.

**Path 1 — raw RDMA writes without re-registering.** The target validates `reg_ctx` on every I/O request; with the `reg_ctx` terminated, writes are rejected at the registrant-lookup check that already exists. No new mechanism needed.

**Path 2 — direct retry REGISTER with the cached old version.** The client bypasses management and sends `REGISTER` to TOMA at its cached version V. The new admission-floor predicate (inserted in `handle_register_registrant_on_disk_segment` at `toma/nvmeibt_register.c:2539`, before `is_valid_register_req`) compares V against the per-CDV floor. V < V+1 → rejected with reason `BELOW_CDV_FLOOR`. No corresponding target-side client-identity state is needed; the floor is sufficient because the only path to a fresh (current) version is management.

**Path 3 — ask management for a new attach.** Gated by the `EVICTING` action in the client's attachment record. Management refuses re-admission while the eviction is in flight, and after eviction completes, `clientA` no longer holds any TPV on `cdvC` whose `extent_map` could be abused — it is just a new attacher.

**The handler step order matters.** If TOMA terminated the `reg_ctx` before raising the floor, a `clientA` `REGISTER` retry landing in the window between those two steps would be admitted at the old version, re-establishing the stale registrant. Raising the floor first closes that window; the per-CDV `handler_lock` serializes the handler against concurrent `REGISTER` paths on the same CDV. The REGISTER predicate takes the same `handler_lock` and holds it across (floor-check + `active_registrants_hash` insert), so the handler's termination loop sees every concurrently-arriving registrant.

**Eventual consistency across TOMAs.** A single `preemptClientFromCDV` fans out to every TOMA of the CDV; each TOMA processes independently. Between the first TOMA's ACK and the last TOMA's ACK, `clientA`'s already-mapped CDV writes can still land on TOMAs whose handler has not yet run. The window is bounded by Kafka delivery + handler time per TOMA (happy path: sub-second). Invariant A ("evicted client cannot write to the CDV") holds **strictly only after** management clears the `EVICTING` action, which happens only after every TOMA has ACKed. During the window, reassignment of `clientA`'s TPVs to a new client is gated by the `EVICTING` state (re-admission flow step 3), so a both-writing race between the evicted client and a successor is structurally impossible.

**Survivor impact.** Client C holds TPV Y on the same CDV `cdvC` at version V. During and after `clientA`'s eviction: no `ReservationModeChange` fires for the CDV, no segment `highest_reservation_mode_version` bump occurs (the gate at `register.c:1020` is never tripped), no `is_seg_active_reservation_mode_version_registrable` rejection for C's I/O. C's writes continue at line rate. C's cached `reservation_mode_version = V` is stale relative to the floor, but stale-version **existing** registrants are explicitly grandfathered — only **new** `REGISTER`s are gated. C only learns the new floor if it disconnects and re-attaches, at which point management stamps it.

**Cooperative survivor losing its registration.** If a survivor reg_ctx is reaped by TOMA (timeout, reconnect cycle) and the client-kernel retries `REGISTER` with its cached `reservation_mode_version`, and the floor has advanced since the original attach, the predicate returns `BELOW_CDV_FLOOR`. Per §2.10.5 client-kernel handling, this is treated like `NCBD_PREEMPTED` and triggers a full TPV teardown. Recovery is via the normal management-driven re-attach flow: the survivor's agent calls management, management stamps the current floor on the new `AttachVolumes`, and the TPV is re-registered. Bounded disruption; accepted trade-off for "management is the only source of current-floor truth."

**Failover.** The admission floor lives in Mongo on the CDV document — durable by construction. TOMAs hold it only in memory. On TOMA cold start, the floor arrives as part of the standard CDV metadata delivery (topology push from the RAFT leader, seeded from management). No per-client state survives failover because none is kept. If a registrant was terminated and the TOMA then fails over before the client has retried, the new primary simply has no record of the client — which is the correct state, since the client's `reg_ctx` was terminated by design.

**Concurrent evictions.** Two operators issue `preemptClientFromCDV` for two different clients (A and B) on the same CDV. Management serializes per-CDV via the existing CDV lock (`lockUtils` in `modules/`). Floor advances V → V+1 (evict A) → V+2 (evict B). Both clients retry and are gated by the corresponding floor. No interaction.

#### 2.10.5 Implementation surface

**Management (`nvmesh-management`)**

- Schema: `cdvConfig.admissionFloor: Number`, default 0 on the CDV document (`models/volume.js`, `validationSchemes/definitions/volume.js`). Immutable through user REST paths.
- `client.js:attachTPV`: stamp `reservationModeVersion` on the CDV `AttachVolumes` payload from `cdv.cdvConfig.admissionFloor`.
- New `volume.js:preemptClientFromCDV(cdvUUID, clientID, cb)`: increments floor, marks `EVICTING`, publishes Kafka fan-out, waits for ACKs, clears state, clears `exclusiveClient` on the client's TPVs on this CDV. Uses the existing per-CDV lock.
- New Kafka consumer for `preemptClientFromCDVResponse` (ACK aggregation).
- New Kafka message definitions: `models/kafkaMessages/PreemptClientFromCDV.js`, `PreemptClientFromCDVResponse.js`.
- Attach-path gate in `client.js:attachTPV`: refuse with `CLIENT_EVICTING_FROM_CDV` system message if the client has `EVICTING` on the CDV's attachment entry.
- Extend `consts.volumeAttachmentActions` with `EVICTING`.
- `handleAttachSatelliteRequest` (§1.5 / Phase 2) is unchanged — satellite reservation is separate from CDV admission floor.
- Hook into the existing TPV force-detach path and `removeAlreadyDetachedAttachments` stale-client cleanup: both call `preemptClientFromCDV` before clearing `exclusiveClient`.

**TOMA (`nvmesh-kernel/toma`)**

- Per-CDV state extended with `u64 admission_floor` and `struct mutex handler_lock`. Location TBD: extend `nvmeibt_cdv_alloc` entry and ensure it is created eagerly on CDV topology arrival (today it is created lazily by the first `CDV_ALLOC_EXTENT`); or add a sibling per-CDV hash keyed by CDV UUID populated at CDV attach time. Eager creation is preferred for locality with the existing CDV bookkeeping.
- **Lock ordering invariant.** The new per-CDV `handler_lock` is acquired **before** any per-`seg_active` lock on every path that takes both. Audit existing callers of `nvmeibt_register_terminate_reg_ctx` and every REGISTER-path lock acquisition to confirm no path takes per-segment before per-CDV; refactor any that do.
- New Kafka handler for `preemptClientFromCDV` in `nvmeibt_kafka.c`:
  1. Look up CDV state. If not present (topology push has not yet reached this TOMA), create the entry eagerly and seed `admission_floor = msg.newFloor` — the message carries management's authoritative floor, so on-the-fly creation is correct and avoids spurious retries.
  2. Acquire per-CDV `handler_lock`.
  3. `cdv->admission_floor = max_t(u64, cdv->admission_floor, newFloor);` (idempotent; safe to interleave with topology seeding).
  4. For each segment of the CDV on this TOMA: linear walk over `active_registrants_hash_by_handle` for this `clientID`; call `nvmeibt_register_terminate_reg_ctx` with `is_deleting_seg_active = false`. (Linear walk is acceptable — preempt is a control-plane event; an `active_registrants_hash_by_client` index was considered and rejected because the register/unregister hot-path cost isn't recouped.)
  5. Release `handler_lock`. Drain machinery inside `terminate_reg_ctx` completes asynchronously.
  6. Publish `preemptClientFromCDVResponse`.
- New predicate in `handle_register_registrant_on_disk_segment` (`nvmeibt_register.c:2539`), inserted before the existing `is_valid_register_req` check. The predicate must take `cdv->handler_lock` and hold it across (floor-check + hash-insert) so a concurrent preempt handler cannot observe an intermediate state:
  ```c
  if (nvmeibt_seg_active_is_cdv(seg_active)) {
      struct nvmeibt_cdv_alloc *cdv = nvmeibt_cdv_alloc_for_seg(seg_active);
      mutex_lock(&cdv->handler_lock);      // held across the hash insert below
      if (incoming_reg_ctx->reservation_mode_version < cdv->admission_floor) {
          mutex_unlock(&cdv->handler_lock);
          reg_refusal_reason = NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR;
          goto reject;
      }
      /* … existing admission checks and the active_registrants_hash insert
         run under the existing per-segment lock while handler_lock is held … */
      mutex_unlock(&cdv->handler_lock);    // released only after the insert
  }
  ```
- Floor seeding on CDV topology arrival: the CDV-metadata message from management (same channel that already delivers CDV parameters) carries `admission_floor`; handler stores it via `max_t` (monotonic). A fallback seed path fires from the first `AttachVolumes` for the CDV if topology push has not yet arrived.
- Extend `enum NVMEIBT_CLIENT_TR_REASON` with `NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR`. mNDU feature-compatibility gate on the new message and reason code.

**Client kernel (`nvmesh-kernel/clnt`)**

- Extend CDV attach path to propagate `reservation_mode_version` from the management payload into the `REGISTER` header (existing field at `toma/clnt/nvmeibt_client_protocol.h:389`).
- On CDV device entering `NCBD_PREEMPTED` (`clnt/nvmeibc_block.c:1079`), tear down every TPV whose `cdv_vol` points to this CDV. New hook in `clnt/tpv/nvmeibc_tpv.c`: `nvmeibc_tpv_handle_cdv_preempted(struct nvmeibc_volume *cdv)` walks the per-CDV TPV list and calls the existing `nvmeibc_tpv_detach` path for each. This is the cleanup barrier that makes Path 2 / Path 3 safety arguments valid. Without it, `extent_map`s remain in memory and a re-attached client could theoretically replay them.
- **`nvmeibc_tpv_detach` must be idempotent.** Two paths can invoke it: the CDV-preempted hook above (client-kernel initiated) and the subsequent management-initiated `DetachVolumes` (observed after management's `cleanupDB` removes the attachment from Mongo). The detach body must gate mutating work on `state != TPV_DETACHING && state != TPV_DETACHED`; a second entry observes the transition from the first and returns without re-running teardown. No double-free, no use-after-free.
- No new client-to-management RPC: a `REGISTER` failure with `BELOW_CDV_FLOOR` is treated like `NCBD_PREEMPTED` — device teardown plus reliance on the existing management-driven re-attach flow.

**Satellite allocator volume**

Unaffected. The satellite's `EXCLUSIVE_READ_WRITE` reservation is per-volume and uses the existing `reservation.version` machinery (§1.5). CDV admission floor and satellite reservation are orthogonal: a CDV allocator re-election changes the satellite's reservation version; a TPV-client eviction changes the CDV's admission floor. Neither requires coordination with the other.

#### 2.10.6 Testing

**Unit tests (management).**

- `test/testThinProvisioning.js` — new `describe('CDV preempt client')` block:
  - Floor initialized to 0 on CDV create; stamped on every CDV `AttachVolumes` message.
  - `preemptClientFromCDV` bumps floor exactly once, marks `EVICTING`, clears on ACK.
  - Double-preempt of the same client is a no-op (idempotent by floor monotonicity).
  - Attach request during `EVICTING` is refused with `CLIENT_EVICTING_FROM_CDV`.
  - ACK timeout scenario — management retries the Kafka fan-out; idempotent handler.

**Integration tests (kernel + management).**

- Two TPV clients on one CDV, each doing I/O at ~100 MB/s. Evict one. Assert: evicted client's I/O stops within 50 ms; surviving client's I/O shows zero interruption in latency histograms.
- Evict a client that is offline (Kafka unreachable): floor bump and Mongo state advance; on client reconnect, registration is rejected with `BELOW_CDV_FLOOR`; client kernel tears down; operator flow proceeds.
- Force-reassign TPV X from client A to client B. Assert: B can write to X with no observable race; A's TPV X device is absent after teardown; A's CDV `NCBD_PREEMPTED`.

**Adversarial tests.**

- Client A ignores `DetachVolumes`, continues to retry `REGISTER` with cached old version. Assert every retry is rejected with `BELOW_CDV_FLOOR`.
- Client A, after eviction, calls `nvmeibc_tpv_cdv_submit_bio` via the `unitest` harness with raw CDV offsets bypassing the TPV layer. Assert all bios fail at target registrant-lookup.
- Client A attempts re-attach during the `EVICTING` window. Assert refusal; assert admission after `EVICTING` clears.

**Failover tests.**

- TOMA failover mid-eviction: old primary ACKed floor bump and started drain when it died. New primary is re-seeded from management's CDV metadata (floor is current) and has no `reg_ctx` for the evicted client (since the client was terminated and cannot re-register). Assert no replay of stale I/O.
- Management failover mid-eviction: `EVICTING` state persists in Mongo; new management instance observes it and resumes the fan-out.

#### 2.10.7 Superseded

The following alternatives are retired and must not resurface in downstream design work without explicit reopening of the tradeoff:

- Per-TPV fencing cookie in the CDV allocator header (fences the control path only, not the data path).
- `ForceDetachTPV` Kafka message as a distinct primitive (replaced by `preemptClientFromCDV` which reuses the preempt semantic at the correct granularity).
- TOMA-side revocation of CDV segment registrations for a stale client as a bespoke mechanism (replaced by the generic registrant-termination path plus admission floor).
- Client-side cookie presentation in `CDV_ALLOC_EXTENT` requests (unnecessary once the data path is fenced by registrant termination).
- **Option P1** (volume-wide preempt with survivor auto-reattach): unnecessary survivor stall and novel client-kernel auto-reattach state machine.
- **Option P2** (per-registrant admission threshold in the I/O hot path): changes the hot-path predicate; excessive risk for a rare control-plane event.
- **Option P3** (`RevokeClientFromVolume` as a parallel admission concept): introduces a second admission mechanism alongside reservation versions; the admission-floor approach achieves the same effect as a natural extension of the existing version check.
- Per-(client, CDV) `evictedVersions` map in Mongo: not needed; the admission floor plus `reg_ctx` termination are sufficient, and the floor's monotonic bump guarantees no re-admission of a stale client without management.

### 2.11 NVCK Support

Add a check to NVCK that:
- Reads `cdv_alloc_header` and `cdv_extent_md` array.
- Verifies `allocated_extents` counter matches the count of non-zero UUID entries.
- Reports extents in `NEEDS_ZEROING` state.
- Reports extents whose `tpv_uuid` no longer exists in management.
- Reports orphaned extents: `extent_type == CDV_EXTENT_DATA` and `tpv_uuid` set in `cdv_extent_md` but no corresponding leaf entry in the L1/L2/L3 tree. Safe to reclaim: clear `cdv_extent_md` entry and return extent to free pool.
- Verifies RAFT-committed `allocator_toma_id` matches the TOMA currently serving alloc requests on the ADMIN channel.

---

## Part 3 — Kernel Client: TPV Datapath

### 3.1 New Kernel Subdirectory: `clnt/tpv/`

Files (as shipped):
- `nvmeibc_tpv.h` — data structures, public API
- `nvmeibc_tpv.c` — volume attach/detach, block-device registration via ATOM, CDV-preempt and NDU hooks
- `nvmeibc_tpv_allocator.c` — TPV\_extent alloc/free, `cdv_alloc_work`, pending-return drain
- `nvmeibc_tpv_io.c` — IO dispatch, zero-read, write-allocate, bio split, sync\_flush barrier
- `nvmeibc_tpv_cdv.c` — CDV transport (read/write/zero-copy helpers against the parent CDV)
- `nvmeibc_tpv_ib_admin.c` — IB-admin dispatch for `CDV_ALLOC_EXTENT` / `CDV_FREE_EXTENT` / `CDV_LIST_EXTENTS` responses
- `nvmeibc_tpv_persist.c` — L1/L2 tree serialization (`load_state` / `flush_state`), partial-page flush, deferred `load_state_work` / `persist_work` / `timeout_work`
- `nvmeibc_tpv_recovery.c` — reconciliation against TOMA's `CDV_LIST_EXTENTS`, orphan extent adoption
- `nvmeibc_tpv_proc.c` — `/proc/nvmeibc/tpv/<name>/` entries
- `nvmeibc_tpv_test.c` / `nvmeibc_tpv_test.h` — 5 kernel self-tests triggered via `/proc/.../selftest`

### 3.2 New Volume Class Handling

In `nvmeibc_main_capi_manipulate_vols.inc.c`, parse `volumeClass` from the MCS `AttachVolumes` message:

```c
enum nvmeibc_volume_class {
    NVC_REGULAR = 0,
    NVC_CDV     = 1,
    NVC_TPV     = 2,
};
```

Add `volume_class` and `is_hidden` to `nvmeibc_volume_header`.

**CDV attach path** (`volume_class == NVC_CDV && is_hidden == 1`):
- Perform full existing volume attach (discovery, IO channels, etc.).
- Do **not** register a block device with the OS (`gendisk`). CDV is accessible internally as `nvmeibc_volume *` but invisible to user space.
- Store the CDV volume pointer in a per-client CDV registry keyed by CDV UUID.

**TPV attach path** (`volume_class == NVC_TPV`): delegated to `nvmeibc_tpv_attach()` in `clnt/tpv/nvmeibc_tpv.c` (§3.8).

### 3.3 Core Data Structures

The authoritative definitions live in `clnt/tpv/nvmeibc_tpv.h`; the summaries below are intentionally elided to the fields that matter for the design discussion. Field-level contracts are in-header; this section exists to orient a reader to the shape of the state machine.

```c
enum nvmeibc_volume_class { NVC_REGULAR = 0, NVC_CDV = 1, NVC_TPV = 2 };

enum nvmeibc_tpv_state {
    TPV_ATTACHING = 0,
    TPV_ATTACHED  = 1,
    TPV_DETACHING = 2,
    TPV_ORPHAN    = 3,   /* NDU: nvmeibc gone, ATOM buffering bios */
};
```

#### Extent mapping

```c
/* xarray value type: virtual_extent_index V -> nvmeibc_tpv_extent_entry*.
 * phys_offset == 0 is the unmapped sentinel: CDV offset 0 is inside the
 * allocator area and is never a valid TPV_extent location.
 * Freed via kfree_rcu after xa_erase; see §3.3.1 for the RCU protocol. */
struct nvmeibc_tpv_extent_entry {
    u64             phys_offset;
    u64             cdv_extent_index;  /* parent CDV_extent — lets free decrement the right ref */
    bool            persisted;         /* true once flush_state has written this leaf */
    struct rcu_head rcu;
};

/* One available physical slot inside an already-allocated CDV_extent.
 * Lives on alloc->free_tpv_extents. */
struct nvmeibc_tpv_free_slot {
    u64              phys_offset;
    u64              cdv_extent_index;
    struct list_head node;
};

/* One CDV_extent allocated from TOMA.  Holds n_slots = E / T slots which may be:
 *   (a) free — linked on alloc->free_tpv_extents
 *   (b) a data slot mapped by the xarray
 *   (c) an L2 table (dynamic-L2 placement; see §3.4.1)
 *   (d) slot 0 of the L1 extent (pinned; is_l1_extent == true)
 * allocated_count covers (b) + (c). l2_slots counts (c) only.
 * Return-to-TOMA eligibility:  allocated_count == 0 && !is_l1_extent. */
struct nvmeibc_cdv_extent_ref {
    u64              extent_index;
    u64              allocated_count;
    u64              l2_slots;
    bool             is_l1_extent;
    struct list_head node;
};
```

#### Per-L2 persistence context (for §3.4.3 partial-page flush)

```c
/* One per in-memory L2 table, kept in alloc->l1_to_l2_ctx (xarray keyed by L1_idx).
 * dirty_pages is a bitmap over DIV_ROUND_UP(T, 4096) 4 KiB pages; IO-path
 * alloc/free sets a bit, flush_state clears it after the page is on disk.  A
 * freshly allocated L2 ctx starts with every bit set so the first flush writes
 * the full T bytes. */
struct tpv_l2_ctx {
    u64              phys;          /* CDV byte offset of this L2 slot */
    unsigned long   *dirty_pages;
};
```

#### Allocator

```c
struct nvmeibc_tpv_allocator {
    struct xarray    extent_map;             /* V -> nvmeibc_tpv_extent_entry*; see §3.3.1 */
    spinlock_t       lock;

    u32              tpv_extent_size_kb;     /* T in KB */
    u64              virtual_extents_total;  /* ceil(virtual_size / T) */

    /* CDV geometry — received in the AttachVolumes cdvConf payload, constant afterwards. */
    u32              cdv_extent_size_mib;     /* E in MB */
    u64              allocator_size_gib;      /* A in GB (byte offset of first data extent) */

    /* Physical slot pool */
    struct list_head cdv_extent_list;        /* nvmeibc_cdv_extent_ref entries */
    u64              cdv_extents_count;
    struct list_head free_tpv_extents;       /* nvmeibc_tpv_free_slot entries */
    u64              free_tpv_extent_count;
    struct list_head pending_return_list;    /* refs with allocated_count == 0 awaiting */
                                             /* CDV_FREE_EXTENT; drained by cdv_alloc_work */
                                             /* because free_extent runs in IO context */
    u64              low_watermark;          /* default: 50 MiB / T; triggers cdv_alloc_work */

    /* Per-TPV L1/L2 tree metadata (§3.4.1 / §3.4.3) */
    u64              l1_extent_index;        /* first CDV_extent ever allocated; holds L1 in slot 0 */
    u64              n_l2_tables_used;
    struct xarray    l1_to_l2_ctx;           /* L1_idx -> tpv_l2_ctx* (second xarray!) */
    unsigned long   *l1_dirty_pages;         /* bitmap over the L1's T-byte slot */

    /* Cached CDV_LIST_EXTENTS result — handed from load_state to recovery to
     * avoid a second TOMA round-trip; kvmalloc'd. */
    u64             *toma_extent_list;
    u64              toma_extent_count;

    /* Statistics (atomic — readable from /proc/nvmeibc/tpv/<name>/stats) */
    atomic64_t       stat_tpv_alloc_ok, stat_tpv_alloc_eagain, stat_tpv_alloc_enomem;
    atomic64_t       stat_tpv_free_ok;
    atomic64_t       stat_cdv_alloc_ok, stat_cdv_alloc_full, stat_cdv_alloc_wgen, stat_cdv_alloc_err;
    atomic64_t       stat_cdv_free_ok;
    atomic64_t       stat_cdv_alloc_ns;      /* cumulative CDV alloc RTT */
};
```

#### Per-TPV instance

```c
struct nvmeibc_tpv {
    struct nvmeiba_atom_os_api    atom;      /* MUST be first — container_of target from gendisk */
    struct nvmeibc_volume        *cdv_vol;
    struct nvmeibc_tpv_allocator  allocator;

    char                          tpv_uuid[NVMEIBC_BD_UUID_LEN];
    char                          tpv_name[NVMEIBC_BD_NAME_LEN];
    u64                           virtual_size;
    atomic_t                      state;     /* enum nvmeibc_tpv_state */

    /* CDV.allocator identity — set from CDV topology; updated on topology push */
    char                          allocator_toma_id[NVMEIB_HOST_NAME_LEN];
    u64                           allocator_generation;
    spinlock_t                    allocator_id_lock;

    /* Background CDV_extent allocation work (§3.7) */
    struct work_struct            cdv_alloc_work;
    atomic_t                      cdv_alloc_pending;

    /* Bios parked waiting for a free TPV_extent slot.  Drained by
     * nvmeibc_tpv_retry_pending_bios() once cdv_alloc_work installs new slots. */
    struct bio_list               pending_bios;
    spinlock_t                    pending_bio_lock;

    /* Synchronous L1-barrier mode (default, set from sourceUUID CM field).
     * When true, data bios for freshly-allocated extents park on
     * pending_l1_flush_bios until the L1/L2 tree has been persisted, closing
     * the crash window where data reaches the CDV before the L1 pointer does. */
    bool                          sync_flush;
    struct bio_list               pending_l1_flush_bios;

    /* Deferred load of allocator state (tree read + recovery).  Retries while
     * the CDV block device is not yet available.  state_loaded uses
     * double-checked locking under pending_bio_lock. */
    struct delayed_work           load_state_work;
    bool                          state_loaded;

    /* Deferred persist of dirty allocator state */
    struct work_struct            persist_work;
    spinlock_t                    persist_lock;
    bool                          dirty;

    /* IO timeout for parked bios — uses nvmeibc_io_max_retry_secs module param,
     * or IO_TIME_OUT_ATTACH (30 s) until state_loaded, or IO_TIME_OUT_NORMAL
     * afterwards.  Cut to 10 ms at detach for fast drain. */
    unsigned long                 max_retry_jiffies;
    struct delayed_work           timeout_work;

    struct list_head              list_node;   /* per-client active-TPV list */

    /* /proc/nvmeibc/tpv/<name>/ entries */
    struct proc_dir_entry              *proc_dir;
    struct nvmeib_public_procfs_ent    *proc_status, *proc_allocator;
    struct nvmeib_public_procfs_ent    *proc_tpv_extent_map, *proc_cdv_extent_map;
    struct nvmeib_public_procfs_ent    *proc_stats, *proc_selftest;

    /* Private fops copy kept in kzalloc'd memory so it survives NDU (nvmeibc
     * module unload/reload) — see nvmeibc_tpv_abandon_all_for_inst. */
    struct block_device_operations tpv_live_fops;

    /* In-flight IO counter for NDU drain.  make_request increments on entry;
     * decrements after the bio is handed off to the CDV.  Abandon waits for
     * zero before orphaning the atom to ATOM's buffering mode. */
    atomic_t                      io_inflight;
};
```

**State-machine touchpoints not visible in the struct.** The per-TPV active list (`list_node`) and the CDV-preempt cleanup hook (`nvmeibc_tpv_handle_cdv_preempted`, §3.8) together enforce the §2.10 preempt cleanup barrier: when a parent CDV's block device enters `NCBD_PREEMPTED`, every TPV riding on it is discovered via the list and torn down so no stale `extent_map` survives to be replayed after re-attach. The NDU path (`nvmeibc_tpv_abandon_all_for_inst` + `TPV_ORPHAN`) uses the same list in reverse: each TPV flushes dirty state, cancels all workers, and hands its atom to ATOM's orphan-buffering mode before the old nvmeibc module unloads.

#### 3.3.1 xarray design rationale and characteristics

`extent_map` is a Linux `xarray` (radix tree) keyed by **virtual extent index** (`virt_idx = virt_offset / tpv_extent_size_kb / 1024`). It was chosen over alternatives for four reasons:

1. **Sparse by design.** An unwritten virtual extent consumes no memory — `xa_load` returns NULL, which the IO path interprets as unmapped. A flat array would waste memory proportional to virtual size, not actual usage.
2. **Lockless reads via RCU.** `xa_load` is safe under `rcu_read_lock()` alone. The IO hot path therefore acquires no spinlock on the common read/mapped-write case.
3. **Ordered iteration.** `xa_for_each` visits entries in ascending key order, which the persist path relies on when scanning the xarray to build the L1/L2 tree snapshot.
4. **`kfree_rcu` integration.** `nvmeibc_tpv_extent_entry` carries an `rcu_head` field so the free path can call `kfree_rcu(entry, rcu)` after `xa_erase`, deferring the actual `kfree` until all concurrent RCU readers have finished dereferencing the pointer — no use-after-free.

**Locking model:**

| Operation | Lock held |
|---|---|
| `xa_load` (IO hot path) | `rcu_read_lock()` only |
| `xa_store` (alloc) | `alloc->lock` spinlock |
| `xa_erase` (free / DISCARD) | `alloc->lock` spinlock |
| `xa_for_each` (proc / persist) | `rcu_read_lock()` only |

The `rcu_read_lock` / `rcu_read_unlock` bracket in `tpv_handle_one_bio` covers only the `xa_load` and the subsequent read of `entry->phys_offset`; it is released before submitting the bio to the CDV block layer.

**Memory usage:**

Each mapped extent consumes one `nvmeibc_tpv_extent_entry` (≈ 32 bytes: 2 × u64 + bool + `rcu_head`). The xarray radix tree uses 64-way branching (6 bits per level); nodes are allocated lazily at ≈ 512 bytes each.

| Virtual size | Extent size | Max `virt_idx` | Entry memory (fully dense) | Radix nodes |
|---|---|---|---|---|
| 256 GiB | 64 KiB | 4 Mi | 128 MiB | ~32 MiB |
| 1 TiB | 64 KiB | 16 Mi | 512 MiB | ~128 MiB |
| 1 TiB | 256 KiB | 4 Mi | 128 MiB | ~32 MiB |

Sparse TPVs (typical case) use memory proportional to *written* capacity, not virtual size.

**Radix tree depth:**

Maximum depth = ⌈log₆₄(virtual_extents_total)⌉:

- Up to 64 extents → 1 level
- Up to 4 096 extents (64² ) → 2 levels
- Up to 262 144 extents (64³ ) → 3 levels (covers 16 GiB at 64 KiB)
- Up to 16 Mi extents (64⁴ ) → 4 levels (covers 1 TiB at 64 KiB)

For nearly all production TPV sizes the tree is 2–3 levels deep, making `xa_load` a small, cache-friendly pointer chase. The xarray `MARKS` facility is not used.

**Second xarray: `l1_to_l2_ctx`.** The allocator also holds a second xarray, keyed by L1 index → `tpv_l2_ctx *`. It is populated by `load_state` (one entry per L1 slot that references a live L2 table) and grown by `flush_state` (a new entry is inserted when a previously-null L1 index first acquires an L2 table). Unlike `extent_map`, this xarray is **not** on the IO hot path — it is touched only by flush, load, and the per-L2 dirty-page bookkeeping (§3.4.3). Locking is simpler: xarray operations run under the allocator `lock` or the `persist_lock`; no RCU read-side is needed.

### 3.4 Allocator State Persistence Format (per-TPV L1/L2 Tree)

Each TPV owns a private 2-level mapping tree that is persisted inside CDV\_extents allocated to that TPV. There is **no CDV-wide metadata region** beyond the TOMA CDV.allocator area `[0, A)`; everything from `A` on is TPV-owned. Per-TPV ownership is required because different TPVs on the same CDV may use different `tpvExtentSizeKB` values (and therefore different slot sizes, L1/L2 fanout, etc.), so a single shared tree cannot encode all of them.

Let $T = \texttt{tpvExtentSizeKB} \times 1024$ (per-TPV slot size in bytes), $E = \texttt{cdvExtentSizeMiB} \times 1024^2$ (CDV\_extent size in bytes), $A = \texttt{allocatorSizeGiB} \times 1024^3$ (allocator area size in bytes, CDV-wide).

$n_{\text{slots}} = E / T$ — slots per CDV\_extent. Slot $s$ within (1-based) data CDV\_extent $i$ occupies CDV bytes $[A + (i-1) \times E + s \times T,\; A + (i-1) \times E + (s+1) \times T)$.

#### Baseline: dedicated tree extent (historical — superseded by §3.4.1 and §3.4.2)

The first implementation allocated a dedicated CDV\_extent ("tree extent") per TPV at first-write time. Slot 0 held the L1 table (with a `tpv_l1_header` for recovery identification); slots 1+ held L2 tables.

This cost one full CDV\_extent of physical space per TPV regardless of TPV size — ~50% overhead for a small TPV. See §3.4.1 for the replacement.

#### 3.4.1 Dynamic L2 placement — no dedicated tree extent

Client-only change. The first CDV\_extent allocated to the TPV is a normal data extent. Its **slot 0** is reserved for the L1 table and its `tpv_l1_header`; all remaining $n_{\text{slots}} - 1$ slots are data slots entering `free_tpv_extents` alongside the rest of the TPV's free pool.

L2 tables are allocated lazily from the same free pool any time `flush_state` needs to write an L1\_idx whose L2 slot has not been assigned yet. Each L2 table consumes exactly one TPV\_extent slot — on any CDV\_extent owned by the TPV.

Each `nvmeibc_cdv_extent_ref` records, per extent: the total in-use slot count (data + L2), the number of L2 tables living in this extent, and a flag for the pinned L1-host extent. The full struct appears in §3.3.

**Per-slot "kind" is not tracked explicitly.** An early design kept a `slot_kind` bitmap (one bit per slot = data vs. L2). It was removed: L2 tables in `change 1` are sticky-monotonic — once placed, they are never migrated or freed during the TPV's lifetime — so an extent with `l2_slots > 0` always has `allocated_count > 0` and is not a return candidate regardless. The scalar `l2_slots` plus the `is_l1_extent` flag is sufficient.

**Free-extent rule.** A ref is eligible for `CDV_FREE_EXTENT` when `allocated_count == 0 && !is_l1_extent`. Because `allocated_count` covers data *and* L2 slots, the check implicitly rejects any extent that still pins L2 tables. An extent that reaches `allocated_count == 0` is moved to `alloc->pending_return_list` (§3.6) and sent to TOMA from `cdv_alloc_work`, not from the free path itself.

**First-write cost**: 1 CDV\_extent (down from 2). For a TPV that only ever has one mapped virtual extent, the L1 header, one L2 table, and the single data slot all live in the same CDV\_extent (slots 0, 1, and some slot ≥ 2 respectively).

#### 3.4.2 Compact 8-byte tree entry — direct CDV byte offset

Client-only change. The 16-byte `{extent_index, debug_meta}` entry is replaced by a single `u64 cdv_offset` holding the raw CDV byte offset of the referenced object (either a data slot or an L2 table).

```c
struct tpv_tree_entry { u64 cdv_offset; };   /* 0 = TPV_TREE_NULL (unmapped) */

/* For an L1 entry pointing at an L2 table:
 *     cdv_offset = A + (extent_idx - 1) * E + slot * T
 * For an L2 leaf pointing at a data slot:
 *     cdv_offset = A + (data_idx   - 1) * E + slot * T
 */
```

0 is a safe null sentinel because a valid L1-entry or L2-leaf pointer always satisfies `cdv_offset >= A > 0`.

Consequences:

- $N_{L1} = (T - \texttt{sizeof}(\texttt{tpv\_l1\_header})) / 8$ (was $T/16$-ish)
- $N_{L2} = T / 8$ (was $T/16$)
- Max addressable virtual extents per 2-level tree quadruples.
- `flush_state` encodes a single u64 per leaf; `load_state` decodes `extent_idx = (cdv_offset - A) / E + 1`, `slot = ((cdv_offset - A) % E) / T`.
- The `debug_meta` field is retired; `/proc/extent_map` still prints the derived `extent_idx, slot`.

The on-disk format changes break compatibility — `TPV_L1_VERSION` is bumped. Because TPV is not yet GA, existing dev volumes are reformatted rather than migrated.

#### 3.4.3 Partial-page L1/L2 flush — 4 KB writes instead of full T-byte rewrites

Client-only change.  Today `nvmeibc_tpv_flush_state()` rebuilds each touched table in a vmalloc'd buffer and writes the entire T-byte slot back to CDV — a single 16-byte (or 8-byte post-§3.4.2) leaf change triggers a T-byte write.  For T = 64 KB that is a 4096× amplification on the L2 write plus another T-byte L1 write whenever a new L1 index becomes non-null.  For a 4 KB data write to an unmapped extent the worst-case metadata cost is ~2 × T per leaf.

The CDV block device sector is 4 KB.  An L2 page of 4 KB covers `4096 / sizeof(entry)` leaves (256 with 16-byte entries, 512 with 8-byte entries).  The fix is to write only the 4 KB pages that actually changed:

1. **Per-table dirty-page bitmap.**  Attach a small `unsigned long *dirty_pages` bitmap (`ceil(T / 4096)` bits, typically 16 bits for T = 64 KB) to each in-memory L2 slot.  Maintain a sibling bitmap for the L1 extent's slot 0.
2. **Allocator sets the bit.**  Every `alloc_extent` / `free_extent` that changes leaf `L2[j]` sets bit `j × sizeof(entry) / 4096` in the owning L2's `dirty_pages`.  Creating a new L2 table (persist_get_or_alloc_l2_phys) sets the bit for the page holding `L1[L1_idx]` in the L1 dirty bitmap.  A header-field change (`n_l2_tables_used`) sets bit 0 of the L1 dirty bitmap.
3. **Flush writes only dirty pages.**  `flush_state` iterates each L1/L2 table, performs a coalesced 4 KB-page write per set bit (or merges adjacent set bits into a single bio), then clears the bitmap on successful write.
4. **Full-rewrite paths untouched.**  First-ever write of a new L2 table still writes the full T bytes (bitmap starts all-ones for a freshly allocated L2 slot).  The cache-cold `load_state` reads full T-byte buffers — dirty tracking is flush-side only.

Write amplification drops from ~2 × T to 4–8 KB of metadata per 4 KB data write to an unmapped extent — the metadata budget is now proportional to the sector size, not the TPV extent size.

Crash-consistency is unchanged: a torn 4 KB write leaves some leaves stale but every individual entry fits inside a single 4 KB page, so no entry is split across a write boundary.  Ordering (L2 before L1 pointer update; data before L2 in sync_flush mode) is preserved by the per-table dirty tracking — we still flush modified L2 pages before rewriting the L1 pointer that references them.

Mostly orthogonal to §3.4.1 and §3.4.2.  Best landed after §3.4.2 so the dirty-page bookkeeping is written once against the final entry size.

#### Combined tree layout (after both changes)

```c
#define TPV_L1_MAGIC    0x5450564C31544142ULL   /* "TPVL1TAB" */
#define TPV_L1_VERSION  2                       /* bumped by §3.4.2 */

struct tpv_l1_header {                          /* 64 bytes, slot 0 of L1 extent */
    u64 magic;
    u64 version;
    u8  tpv_uuid[16];
    u64 l1_extent_index;                        /* CDV_extent holding this L1 */
    u64 n_l2_tables_used;                       /* active L2 tables */
    u8  reserved[16];
};

struct tpv_tree_entry { u64 cdv_offset; };      /* 8 bytes */
```

#### Address translation (both changes applied)

For virtual extent index $V$:

$$
\begin{aligned}
L1_{\text{idx}} &= \lfloor V / N_{L2} \rfloor \\
L2_{\text{idx}} &= V \bmod N_{L2} \\
\texttt{off}_{L2} &= L1[L1_{\text{idx}}].\texttt{cdv\_offset} \\
\texttt{off}_{\text{data}} &= L2[L2_{\text{idx}}].\texttt{cdv\_offset}
\end{aligned}
$$

where $N_{L2} = T / 8$. `cdv_offset == 0` means the entry is null (unmapped virtual range for leaves; absent L2 table for L1 entries). Recovering `{extent_idx, slot}` from a non-null `cdv_offset`: `extent_idx = (cdv_offset − A) / E + 1`, `slot = ((cdv_offset − A) % E) / T`.

```mermaid
graph TD
    L1["L1 table (slot 0 of the first TPV-owned CDV_extent)\nheader + N_L1 entries of 8 bytes each"]
    L2_a["L2 table (any TPV-owned slot, tracked in cdv_extent_ref.slot_kind)"]
    L2_b["L2 table (any other TPV-owned slot)"]
    DATA_a[("Data slot (T bytes)")]
    DATA_b[("Data slot (T bytes)")]

    L1 -->|"L1[i].cdv_offset"| L2_a
    L1 -->|"L1[j].cdv_offset"| L2_b
    L2_a -->|"L2[k].cdv_offset"| DATA_a
    L2_b -->|"L2[m].cdv_offset"| DATA_b
```

*Figure 2: Post-change tree layout. The L1 table lives in slot 0 of the first data extent allocated to the TPV; L2 tables are ordinary T-byte slots scattered across any TPV-owned CDV\_extents; both kinds of entries are 8-byte CDV byte offsets.*

#### Reconstruction at attach time (`nvmeibc_tpv_persist.c`)

```c
// nvmeibc_tpv_load_state():
//
// 1. Query TOMA via CDV_LIST_EXTENTS to get all CDV_extents owned by this TPV.
// 2. For each extent in the list, read slot 0 and probe for (magic, tpv_uuid).
//    The matching extent is the L1 extent; its header gives n_l2_tables_used.
// 3. Walk L1: for each non-null entry:
//      off_L2 = L1[i].cdv_offset
//      Read T bytes at CDV offset off_L2 → L2 table.
//      Mark the (extent_idx, slot) containing that L2 as TPV_SLOT_L2 in cdv_extent_ref.
//      For each non-null L2 leaf:
//          V          = i * N_L2 + j
//          phys       = L2[j].cdv_offset
//          (ext, slot)= decode(phys)
//          xa_store(&alloc->extent_map, V, {phys_offset=phys, cdv_extent_index=ext})
//          Mark (ext, slot) TPV_SLOT_DATA in that cdv_extent_ref.
// 4. For each TOMA-reported extent, any slot not marked L1/L2/data goes into
//    free_tpv_extents.
// 5. Open IO gates.
```

On flush: rewrite only modified L2 tables; rewrite the L1 extent only when L1 entries or the header change (e.g. a new L2 table is allocated).

### 3.5 IO Path

```c
// nvmeibc_tpv_io.c

static blk_qc_t nvmeibc_tpv_make_request(struct request_queue *q, struct bio *bio)
{
    struct nvmeibc_tpv *tpv = q->queuedata;
    u64 virt_offset = bio->bi_iter.bi_sector << 9;
    u64 extent_size = (u64)tpv->allocator.tpv_extent_size_kb << 10;

    // For each extent-aligned segment of the bio:
    //   virt_idx = virt_offset / extent_size
    //
    //   rcu_read_lock();
    //   entry = xa_load(&tpv->allocator.extent_map, virt_idx);
    //   // entry is RCU-protected: safe to dereference until rcu_read_unlock().
    //   // entry is freed via kfree_rcu, so it cannot disappear under us.
    //
    //   READ  + entry == NULL → rcu_read_unlock(); complete bio with zero pages (no CDV IO)
    //   WRITE + entry == NULL → rcu_read_unlock(); nvmeibc_tpv_alloc_extent(tpv, virt_idx, &entry)
    //                           (alloc takes alloc->lock spinlock; does xa_store GFP_ATOMIC)
    //                           then fall through to mapped case
    //   mapped                → snapshot phys_offset; rcu_read_unlock();
    //                           rewrite bio sector to (phys_offset + intra_extent_offset)
    //                           submit to cdv_vol's block layer
    //   DISCARD               → rcu_read_unlock(); nvmeibc_tpv_free_extent(tpv, virt_idx)
    //                           (free takes alloc->lock; does xa_erase + kfree_rcu)
    //                           complete bio immediately
    //
    // xa_load is called on EVERY bio — it is the hot path.  The RCU read-side critical
    // section is kept as short as possible: just the load + phys_offset snapshot.
}
```

Bios crossing extent boundaries must be split using `bio_split` / `bio_chain`.

```mermaid
flowchart TD
    bio_in["bio arrives at nvmeibc_tpv_make_request()"]
    bio_in --> split_check{"crosses extent\nboundary?"}
    split_check -->|Yes| split["bio_split + bio_chain\nprocess each sub-bio separately"]
    split_check -->|No| lookup["virt_idx = virt_offset / extent_size\nentry = xa_load(extent_map, virt_idx)"]
    split --> lookup

    lookup --> op_check{"operation?"}

    op_check -->|"READ + unmapped"| zero_read["complete with zero pages\nno CDV IO issued"]
    op_check -->|"DISCARD"| discard["nvmeibc_tpv_free_extent(virt_idx)\ncomplete bio immediately"]
    op_check -->|"WRITE + unmapped"| avail_check{"free_tpv_extents\navailable?"}
    op_check -->|"READ/WRITE + mapped"| remap["phys sector = phys_offset + intra_offset\nsubmit bio to CDV block layer"]

    avail_check -->|Yes| alloc_extent["pop from free_tpv_extents\nxa_store into extent_map\nschedule persist_work"]
    avail_check -->|No| queue_bio["queue bio\nschedule cdv_alloc_work\n(CDV_ALLOC_EXTENT to TOMA)"]
    alloc_extent --> remap
    queue_bio --> wait["await CDV_ALLOC_EXTENT response\nthen retry queued bios"]
```

*Figure 10: TPV IO dispatch path. Reads to unmapped extents return zeroes without touching the CDV. Writes to unmapped extents trigger CDV_extent allocation. Bios crossing extent boundaries are split before processing.*

### 3.6 TPV\_extent Alloc / Free

```c
// nvmeibc_tpv_allocator.c

int nvmeibc_tpv_alloc_extent(struct nvmeibc_tpv *tpv, u64 virt_idx,
                              struct nvmeibc_tpv_extent_entry **out)
{
    // 1. Lock allocator.
    // 2. Re-check extent_map[virt_idx] under the lock (race with concurrent alloc).
    // 3. Pop one nvmeibc_tpv_free_slot from free_tpv_extents.
    //    If empty: unlock, return -EAGAIN.  Caller parks bio on tpv->pending_bios.
    // 4. kzalloc a nvmeibc_tpv_extent_entry (GFP_ATOMIC); on -ENOMEM, return the
    //    slot and fail with -ENOMEM.
    // 5. xa_store(extent_map, virt_idx, entry, GFP_ATOMIC).
    // 6. Bump owning cdv_extent_ref.allocated_count.
    // 7. If free_tpv_extent_count < low_watermark and no request is in flight,
    //    schedule cdv_alloc_work.
    // 8. Mark tpv->dirty; schedule persist_work (deferred) or park bio for
    //    pending_l1_flush_bios (sync_flush mode).
    // 9. Set *out = entry; unlock.
}

int nvmeibc_tpv_free_extent(struct nvmeibc_tpv *tpv, u64 virt_idx)
{
    // 1. Lock allocator.
    // 2. xa_erase(extent_map, virt_idx) → entry.  If NULL, -ENOENT.
    // 3. kfree_rcu(entry, rcu)  — deferred free; concurrent rcu_read_lock'd
    //    readers on the IO path may still be dereferencing the pointer.
    // 4. Push a fresh nvmeibc_tpv_free_slot back onto free_tpv_extents.
    // 5. Decrement owning cdv_extent_ref.allocated_count.
    // 6. If allocated_count drops to 0 AND !is_l1_extent:
    //      Move ref from cdv_extent_list to pending_return_list.
    //      (Do NOT send CDV_FREE_EXTENT here — this runs in IO context and
    //      cannot block on IB admin.)  cdv_alloc_work will drain
    //      pending_return_list in process context.
    // 7. Mark tpv->dirty; schedule persist_work.
    // 8. Unlock.
}
```

### 3.7 CDV\_extent Request from Client

`cdv_alloc_work` runs in process context and does two jobs in order:

**Drain `pending_return_list` first** — for each ref there, send `NVMEIBC_MA_CDV_FREE_EXTENT` (fire-and-forget) and free the ref. Keeping this ahead of new allocation requests means a churn workload doesn't grow the CDV's extent count monotonically.

**Then request more slots** if `free_tpv_extent_count < low_watermark` and `cdv_alloc_pending` is clear:

1. Snapshot `(allocator_toma_id, allocator_generation)` under `allocator_id_lock`. Resolve an ADMIN-channel handle to any disk owned by that TOMA.
2. Send `NVMEIBC_MA_CDV_ALLOC_EXTENT`. Set `cdv_alloc_pending = 1`.
3. On response (delivered via `nvmeibc_cdv_dispatch_alloc_response`):
   - **`CDV_ALLOC_WRONG_GEN`** — stale allocator identity; clear `cdv_alloc_pending` and wait. Fresh identity arrives via the `CDV_ALLOCATOR_UPDATE` topology push and triggers `nvmeibc_tpv_update_allocator_for_cdv`, which re-arms `cdv_alloc_work`.
   - **`CDV_ALLOC_CDV_FULL`** — no capacity on the CDV. Clear `cdv_alloc_pending`; parked bios stay parked. The next free on any TPV on this CDV that releases a whole CDV\_extent will re-arm the work. If none does, the parked bios eventually time out via `timeout_work` and are failed with `-EIO`. A future extension could re-arm on a CDV-capacity-extended notification.
   - **`CDV_ALLOC_OK`** — build an `nvmeibc_cdv_extent_ref` for `resp.extent_index`, link it on `cdv_extent_list`, and splice all $n_{\text{slots}} = E/T$ slot addresses (`A + extent_index * E + s * T` for `s ∈ [0, n_slots)`) onto `free_tpv_extents`. If this is the TPV's first-ever extent, reserve slot 0 as the L1 host (set `is_l1_extent`, call `nvmeibc_tpv_mark_l1_full_dirty`). Clear `cdv_alloc_pending`. Call `nvmeibc_tpv_retry_pending_bios` to drain parked bios.

The tree-install step (the old "compute group index `G`, install into L2/L2a/L3") does not happen at allocation time. Leaves are written into the flat L1 + dynamic-L2 tree by `flush_state` (§3.4.1, §3.4.3), driven by `persist_work` — separate from the CDV\_extent alloc path.

> **Security note:** `CDV_FREE_EXTENT` from the client, and `CDVAllocatorFreeAll` from management on TPV delete, both invoke the TOMA-side release path described in §3.9. On DISCARD (TRIM) the TPV immediately unmaps the virtual extent so reads from the TPV return zeros from the zero-fill path, independent of what is on the CDV. Whether the underlying CDV blocks are scrubbed before reuse is controlled by the `cdv_extent_zero_on_free` TOMA runtime config (Architecture Decision #18); by default the blocks are not rewritten and are overwritten only when the next TPV allocates that slot. This is acceptable for the default deployment assumption that TPVs are encrypted, rendering stale data cryptographically unreadable after rekey. Operators with a different threat model should enable `cdv_extent_zero_on_free`.

### 3.8 Attach / Detach

**Attach** (`nvmeibc_tpv_attach`):
1. Resolve the parent CDV via its attached (hidden) `nvmeibc_volume` — the CDV was attached by the preceding `AttachVolumes` with `isHidden=true`.
2. Allocate `nvmeibc_tpv`; initialise allocator, work items, bio lists, timeout work, and set `sync_flush` from the CM `sourceUUID` field.
3. Register the block device (`gendisk`) via the ATOM API. IO is accepted immediately but parks on `pending_bios` until `state_loaded` becomes true.
4. Schedule `load_state_work` with zero delay. The worker runs `nvmeibc_tpv_load_state` (read L1 from the L1 extent, walk L2 tables, populate `extent_map` and `l1_to_l2_ctx`) followed by `nvmeibc_tpv_recovery` (cross-check against `CDV_LIST_EXTENTS` from TOMA; adopt orphan extents present in TOMA but missing from the tree). On CDV-not-ready failure, the worker retries with backoff.
5. On successful load+recovery the worker sets `state_loaded` under `pending_bio_lock`, drains `pending_bios`, and — if the free pool is below watermark — arms `cdv_alloc_work`.
6. Register `/proc/nvmeibc/tpv/<name>/` entries; insert the TPV into the per-client active list.

**Detach** (`nvmeibc_tpv_detach`) — must be idempotent: both the CDV-preempted hook and the subsequent management-driven `DetachVolumes` can call it, and the second entry observes `TPV_DETACHING`/`TPV_DETACHED` and returns without re-running teardown.

1. CAS `state` from `TPV_ATTACHED` → `TPV_DETACHING`; bail out early on any other prior state.
2. Drop `max_retry_jiffies` to `HZ/100` so parked bios fail fast.
3. Cancel `load_state_work`, `cdv_alloc_work`, `persist_work`, `timeout_work` (sync).
4. If dirty, run a final `flush_state` synchronously (best-effort — recovery will reconcile on re-attach if this fails).
5. Fail every bio on `pending_bios` / `pending_l1_flush_bios` with `-EIO`.
6. Unregister the `gendisk`, deregister `/proc` entries, remove from per-client list.
7. Free the `extent_map` (kfree_rcu each entry; `rcu_barrier` before destroy), the `l1_to_l2_ctx` xarray, `cdv_extent_list`, `free_tpv_extents`, `pending_return_list`, and per-L2 dirty-page bitmaps.

**CDV preempt cleanup** (`nvmeibc_tpv_handle_cdv_preempted`) is invoked from `nvmeibc_block.c` when the CDV's block device enters `NCBD_PREEMPTED`. It walks the per-client active-TPV list and calls `nvmeibc_tpv_detach` on every TPV whose `cdv_vol` points at the preempted CDV. This is the cleanup barrier required by `TPV_PerClientCDVPreemption.md` §2.10 — without it, stale `extent_map`s survive in memory and a re-attached client could replay them.

**NDU abandon** (`nvmeibc_tpv_abandon_all_for_inst`) is invoked before the old nvmeibc module's CDV abandon. For every TPV in the instance it: flushes dirty state; cancels all workers; waits for `io_inflight` to reach zero; calls `nvmeiba_os_api_orphan_abandon()` (which swaps the live fops with a buffering stub so bios are queued by ATOM instead of routed to nvmeibc); and transitions state to `TPV_ORPHAN`. The new nvmeibc module's attach path observes the orphaned atom and re-binds it.

### 3.9 TPV.Delete

From management (no client attach needed):
1. Verify `tpvConfig.exclusiveClient === null`.
2. Send `CDVAllocatorFreeAll(cdvUUID, tpvUUID, allocatorSizeGiB, cdvExtentSizeMiB)` to TOMA.
3. TOMA iterates its in-memory allocator for this CDV and releases every extent owned by `tpvUUID`. The release path is gated by the `cdv_extent_zero_on_free` TOMA runtime config parameter (registered in `oper_params[]`, settable via `toma_rpc`):
   - **`cdv_extent_zero_on_free = 0` (default):** TOMA writes a free on-disk record for each extent, removes it from the allocator's in-memory list, and decrements `n_allocated`. The CDV physical blocks are not rewritten; stale data remains visible at those offsets until the next allocation overwrites them.
   - **`cdv_extent_zero_on_free != 0`:** TOMA persists an `ALLOCATED|NEEDS_ZEROING` record (geometry carried in the record's `reserved2` area, not CRC-covered), dispatches a background zero write (1 MiB chunks on the per-CDV I/O work queue), and leaves the entry in the allocator's extent list — blocking reallocation of that slot. When the zero completes, `cdv_zero_finalize` writes a free record, removes the entry, and decrements `n_allocated` and `n_pending_zeroing`.
4. Management deletes the TPV record and decrements `CDV.tpvCount`.

`NEEDS_ZEROING` on-disk records are honored on allocator scan regardless of the current flag value, so toggling `cdv_extent_zero_on_free` off does not strand extents that were previously marked for zeroing.

There is no CDV-level encryption; encryption is at the TPV level (see Architecture Decision #18). `cdv_extent_zero_on_free` therefore defaults to **off** — operators who need stale-data scrubbing for an untrusted-multi-tenant deployment can opt in via `toma_rpc config set cdv_extent_zero_on_free 1`.

#### Design gap: zero-on-free is non-functional after the satellite-volume migration

After Phase 1 of `SatelliteVolumeForCDVAlloc.md` retired the CDV auto-attach to TOMA nodes, the allocator TOMA no longer has the CDV's block device open. `cdv_zero_execute` writes to CDV data-extent offsets, which requires `/dev/nvmesh/<cdv>` to be available on the allocator TOMA — which it is not in the default deployment.

The kernel-side opener (`cdv_worker_open_cdv_fd_for_zeroing` in `toma/nvmeibt_cdv_alloc.c`) returns `-ENODEV` when the CDV is not attached locally, causing `cdv_zero_execute` to fail gracefully: the freed extent stays in `NEEDS_ZEROING` state on the satellite and is **not reused**. No silent corruption, but if `cdv_extent_zero_on_free` is enabled, every freed extent is permanently lost capacity.

**Status:** known limitation. Acceptable while `cdv_extent_zero_on_free` is off-by-default (the only justification for ever turning it on is unencrypted multi-tenant deployments, and that combination is not currently supported). To re-enable zero-on-free post-migration, options include:

1. Add a separate auto-attach path that attaches the CDV (not the satellite) to the elected allocator TOMA when zero-on-free is enabled.
2. Move the zeroing work to a TOMA that does host the CDV (e.g., a first-pRAID owner) via a new Kafka instruction.
3. Have the client kernel zero the extent before releasing it (TPV side instead of TOMA side).

The `cdv_zero_execute` worker logs a one-shot warning per CDV when invoked in this state so operators can see the behavior in traces.

### 3.10 TPV Grow

`POST /volumes/tpv/extend` sends `UpdateVolume` MCS to client:
- Client receives new `virtual_size`.
- Updates `tpv->virtual_size` and `allocator.virtual_extents_total`.
- Calls `set_capacity(gendisk, new_sectors)` to inform the OS.
- No allocator state flush needed for grow (new extents start unmapped = read-as-zero).

---

## Part 4 — Testing

### Shipped kernel self-tests (`clnt/tpv/nvmeibc_tpv_test.c`)

Five tests, triggered by writing to `/proc/nvmeibc/tpv/<name>/selftest`. CDV transport is stubbed by an in-memory buffer in the test file; the production `nvmeibc_tpv_cdv.c` path is not exercised by these tests.

| Test | Covers |
|---|---|
| `tpv_ktest_alloc_free` | xarray `xa_store`/`xa_load`/`xa_erase` round-trip via `nvmeibc_tpv_alloc_extent` / `nvmeibc_tpv_free_extent`, slot accounting |
| `tpv_ktest_persist` | `flush_state` → `load_state` round-trip; verifies L1/L2 tree is rebuilt with identical mappings |
| `tpv_ktest_pool_exhaustion` | All slots consumed → next alloc returns `-EAGAIN`; freeing one slot re-enables alloc; `cdv_alloc_work` re-arm accounting |
| `tpv_ktest_double_free` | Second free of the same `virt_idx` returns `-ENOENT` |
| `tpv_ktest_recovery` | Orphan adoption: TOMA's `CDV_LIST_EXTENTS` reports an extent that is absent from the on-disk tree → recovery creates the ref + free slots |

### Planned / not-yet-shipped tests

The scenarios below are called out as design intent. None of them are currently implemented; they remain open work.

**Management (`nvmesh-management/test/`):**
- `tpv_lifecycle.js` — create CDV, create TPV, attach, detach, delete TPV, delete CDV
- `tpv_quota.js` — attempt to create the `maxTPVs + 1`-th TPV on a CDV, expect rejection
- `tpv_extend.js` — extend TPV, verify schema update and that `UpdateVolume` MCS is sent when attached
- `tpv_attach_hidden_cdv.js` — verify hidden CDV attach precedes TPV exclusive attach

**Kernel / cluster-level bad-paths** (require a real cluster or a TOMA/RAFT stub richer than what the in-tree self-tests provide):
- Crash after a TPV write but before the corresponding tree flush — verify `load_state` + recovery leaves the virtual extent unmapped (READ-as-zero) and makes the physical slot reclaimable.
- `CDV_ALLOC_EXTENT` in flight when the allocator TOMA crashes — verify RAFT elects a new allocator, `CDV_ALLOCATOR_UPDATE` topology push updates the client, `cdv_alloc_work` re-arms against the new generation, and `req_id` prevents double-counting.
- CDV\_extent allocated on TOMA but client crashes before installing the L2 leaf — verify NVCK flags the orphan and recovery reconciles it.
- TPV detach races ongoing writes — verify the detach idempotency gate (§3.8) plus `io_inflight` draining leaves no lingering bios.
- Force-delete one TPV while many others are active on the same CDV — verify `CDVAllocatorFreeAll` only touches the deleted TPV's extents.

**Scale targets** (not automated yet):
- 512 clients simultaneously attached to distinct TPVs on one CDV.
- CDV at the warn threshold: `CDVCapacityWarning` fires, management extends CDV, TOMA resumes allocation, and the warning clears at the 85% hysteresis boundary.
- 1000 concurrent writes to a single TPV: no allocator lock-contention deadlock, `/proc/.../stats` reflects expected counters.

---

## Part 5 — CDV Auto-Attachment to TOMA Nodes

### 5.1 Rationale

A CDV should be attached to a given node whenever **either** of two independent reasons holds:

1. **TOMA reason** — the node has a disk segment in the CDV's first pRAID, so its TOMA may need to act as the CDV allocator. This is true regardless of disk segment status.
2. **TPV reason** — a TPV on that client uses this CDV for physical storage.

The CDV is detached from a node only when **both** reasons are gone. A node may be a TOMA and also host TPVs, or it may be one without the other.

### 5.2 Attachment Reference Tracking

The existing `referenceIDs` array on `client.attachments[volumeUUID]` tracks why a volume is attached. Two namespaced prefixes for CDV:

- `"tpv:<tpvUUID>"` — CDV attached because a TPV on this client uses it. Set by `attachTPV`, cleared by `detachTPV` or involuntary-detach cleanup.
- `"toma:<cdvUUID>"` — CDV attached because this node is a candidate allocator. Set at CDV creation and on topology additions. Cleared on topology removals or CDV deletion. **Not** affected by TPV attach/detach.

The two reference classes have **independent lifecycles**. CDV is detached from a node only when both classes are empty (handled by `detachVolumes` ref logic).

```mermaid
graph TD
    subgraph refs["client.attachments[cdvUUID].referenceIDs (per node)"]
        tpv_refs["tpv:tpvUUID entries\n(one per attached TPV that uses this CDV)"]
        toma_refs["toma:cdvUUID entry\n(present if node is a first-pRAID allocator candidate)"]
    end

    attachTPV["attachTPV(clientID, tpvID)"] -->|"add tpv:tpvUUID"| tpv_refs
    detachTPV["detachTPV(clientID, tpvID)"] -->|"remove tpv:tpvUUID"| tpv_refs
    cdvCreate["CDV creation /\ntopology addition"] -->|"add toma:cdvUUID"| toma_refs
    topoRemove["Topology removal /\nCDV deletion"] -->|"remove toma:cdvUUID"| toma_refs

    tpv_refs --> check{"both tpv:* set\nand toma:* set\nare empty?"}
    toma_refs --> check

    check -->|"No: keep CDV attached"| keep["CDV remains attached to node"]
    check -->|"Yes: detach"| detach_cdv["send DetachVolumes(CDV)"]
```

*Figure 9: CDV attachment reference tracking. Two independent reference classes prevent premature CDV detach. TPV detach only removes `tpv:` refs — it never touches `toma:` refs. A CDV is detached from a node only when all references of both classes are cleared.*

### 5.3 Determining the TOMA Node Set

All nodes that have any disk segment in the CDV's first pRAID chunk, **regardless of segment status** (NORMAL, INITIALIZING, DEAD, etc.):

```js
// modules/cdvTomaAutoAttach.js
_firstPRaidNodeIds(cdv) {
    if (!cdv.chunks || !cdv.chunks[0]) return [];
    const firstChunk = cdv.chunks[0];
    return [...new Set(
        firstChunk.pRaids
            .flatMap(pRaid => pRaid.diskSegments)
            .map(seg => seg.node_id)
    )];
}
```

### 5.4 Module: `modules/cdvTomaAutoAttach.js`

```js
class CDVTomaAutoAttach {
    // Attaches CDV to all first-pRAID nodes. Idempotent.
    // Called at CDV creation and at startup reconciliation.
    async attachCDVToAllTomaNodes(cdv)

    // Queries all CDVs and calls attachCDVToAllTomaNodes for each.
    // Run once at startup to recover from missed creation-time attaches.
    async reconcileAllCDVs()

    // Computes delta between previousNodeIds and current first-pRAID nodes,
    // attaches new nodes, detaches removed nodes.
    // Called after a pRAID segment change on a CDV's first chunk.
    async reconcileFirstPRaidAttachments(cdv, previousNodeIds)

    // Removes toma: referenceID from all first-pRAID nodes.
    // Called only during CDV deletion.
    async detachCDVFromAllNodes(cdv)

    async attachCDVToNode(cdv, nodeId)
    //   → attachVolumes() with referenceID = `toma:${cdv.uuid}`,
    //     reservation = SHARED_READ_WRITE, isHidden = false

    maybeDetachCDVFromNode(cdv, nodeId)
    //   → detachVolumes() with referenceID = `toma:${cdv.uuid}`
    //   → actual DetachVolumes sent only if no other refs (tpv:* or toma:*) remain
}
```

**No `onTPVDetached` method.** TPV detach only removes `tpv:` refs — it never triggers removal of `toma:` refs. The `toma:` lifecycle is managed entirely by creation, topology changes, and CDV deletion.

### 5.5 `toma:` Reference Lifecycle

| Event | Action | Code path |
|---|---|---|
| CDV created (chunks allocated) | Add `toma:` ref on all first-pRAID nodes | `volume.js` `saveVolumes` → `attachCDVToAllTomaNodes` |
| Management startup | Reconcile `toma:` refs for all CDVs | `bootstrapper.js` → `reconcileAllCDVs` |
| Disk segment added/removed in first pRAID | Add/remove `toma:` ref on affected nodes | `volume.js` `handleSegmentChangeInPRaid` → `reconcileFirstPRaidAttachments` |
| CDV deleted | Remove `toma:` ref from all nodes | `volume.js` delete path → `detachCDVFromAllNodes` |

### 5.6 `tpv:` Reference Lifecycle

| Event | Action | Code path |
|---|---|---|
| TPV attached to client | Add `tpv:` ref on that client | `client.js` `attachTPV` → `attachVolumes` with `tpv:<tpvUUID>` |
| TPV detached from client | Remove `tpv:` ref on that client | `client.js` `detachTPV` → `detachVolumes` with `tpv:<tpvUUID>` |
| Involuntary detach (preemption, stale, deletion) | Remove `tpv:` ref on that client | `client.js` `cleanupTPVReferencesForDetachedClient` |

In all cases, `detachVolumes` sends an actual kernel `DetachVolumes` message for the CDV only when the `tpv:` removal leaves zero referenceIDs (no `tpv:*` and no `toma:*`) on that client.

### 5.7 CDV Deletion

In the CDV delete path:
1. Verify all TPVs are deleted (backend check; returns error if not).
2. Detach CDV from all TOMA-attached nodes (clear `toma:<cdvUUID>` referenceIDs via `detachCDVFromAllNodes`).
3. Proceed with volume delete.

### 5.8 Edge Cases

- **Node failure**: TOMA attach timeout — management marks attachment as stale and proceeds with CDV delete or allocator re-election regardless. CDV is EC and tolerates node loss.
- **New node joins with first-pRAID segment**: Triggered by `handleSegmentChangeInPRaid` → `reconcileFirstPRaidAttachments`.
- **CDV extend**: Adds new pRAIDs (chunks[1], ...). TOMA attachment is keyed to `chunks[0]` only; no change needed.
- **Multiple CDVs on same node**: Each generates an independent `toma:<cdvUUID>` referenceID; a node may hold multiple CDV TOMA attachments.
- **Management crash during TPV detach**: `tpv:` ref may be stranded on the client. This is a pre-existing gap in all referenceID-based tracking. The `toma:` refs are unaffected — they persist correctly and are reconciled at next startup.
- **Management crash during CDV creation**: `toma:` refs may not have been added yet. `reconcileAllCDVs` at startup fixes this.

---

## Part 6 — Observability: proc Files

### 6.1 Client-Side: TPV Allocator State

Using existing `nvmeib_public_proc_create` from `common_public/nvmeib_public_procfs.h`.

**Directory**: `/proc/nvmesh/volumes/<tpv_name>/tpv/`

Created in `nvmeibc_tpv.c` when the TPV block device is registered.

#### `tpv_alloc` — allocator summary (text)

```
virtual_size_gb:       200
virtual_extents_total: 409600
tpv_extent_size_kb:    512
cdv_extent_size_mib:    1024
cdv_extents_count:     3
free_tpv_extents:      1842
low_watermark:         102
mapped_extents:        4294
cdv_alloc_pending:     0
dirty:                 1
allocator_toma_id:     <node-id>
allocator_generation:  2
```

Fill callback: `nvmeibc_tpv_proc_fill_alloc(tpv, buf, len)`.

#### `tpv_cdv_extents` — per-CDV\_extent breakdown (text)

```
extent_index  allocated  total_slots
0             1024       2048
1             2048       2048
4             222        2048
```

Fill callback: `nvmeibc_tpv_proc_fill_cdv_extents(tpv, buf, len)` — walks `cdv_extent_list`.

#### `tpv_alloc.json` — machine-readable JSON version

Combines both views for tooling consumption.

### 6.2 TOMA-Side: CDV Allocator State

**Directory**: `/proc/nvmesh/toma/cdv/<cdv_name>/`

Created in `toma/nvmeibt_cdv_allocator.c` when the allocator is initialized.

#### `alloc_state` — allocator header summary (text)

```
cdv_name:             mycdv
cdv_extent_size_mib:   1024
total_extents:        1000
allocated_extents:    6
free_extents:         992
needs_zeroing:        2
generation:           47
is_local_allocator:   1
allocator_toma_id:    <node-id>
allocator_generation: 2
```

#### `alloc_extents` — per-extent ownership table (text, allocated only)

```
extent_index  tpv_uuid                              flags
0             550e8400-e29b-41d4-a716-446655440000  -
1             550e8400-e29b-41d4-a716-446655440000  needs_zeroing
4             7a3f9c21-1234-5678-abcd-ef0123456789  -
```

#### `alloc_state.json` — machine-readable JSON version

### 6.3 Python Analysis Scripts

- **`tools/tpv_inspect.py`**: Reads `/proc/nvmesh/volumes/*/tpv/tpv_alloc.json` from a node and produces a summary of all TPV allocator states.
- **`tools/cdv_inspect.py`**: Reads `/proc/nvmesh/toma/cdv/*/alloc_state.json` from a TOMA node and cross-checks: `allocated_extents` vs. actual non-free row count, `tpv_uuid` values vs. management REST API, orphaned extents.

---

## Part 7 — Simulator Updates

### 7.1 TOMA Simulator (`toma/nvmeibt_toma_simu.h/.c`)

Add CDV allocator state:

```c
struct toma_cdv_alloc_sim {
    u8               cdv_uuid[16];
    u64              cdv_extent_size_mib;
    u64              total_extents;
    unsigned long   *free_bitmap;
    struct {
        u8  tpv_uuid[16];
        u8  flags;
    } *extent_md;
    bool             is_local_allocator;
    u64              allocator_generation;
    spinlock_t       lock;
};
```

Add ADMIN channel opcode handlers:

```c
case NVMEIBC_MA_CDV_ALLOC_EXTENT:
    return toma_sim_handle_cdv_alloc(toma, req, resp);
case NVMEIBC_MA_CDV_FREE_EXTENT:
    return toma_sim_handle_cdv_free(toma, req, resp);
```

`toma_sim_handle_cdv_alloc()`:
1. Check `req->client_generation == alloc_sim->allocator_generation`; if not, return `WRONG_GENERATION`.
2. Find first clear bit in `free_bitmap`. If none, return `CDV_FULL`.
3. Write `extent_md[idx].tpv_uuid = req->tpv_uuid` (simulates write-before-respond).
4. Set bit in `free_bitmap`.
5. Fill `resp->extent_index = idx`, `resp->allocator_generation`, `resp->status = OK`.

Add `toma_sim_trigger_allocator_failover(cdv_uuid)` — simulates allocator TOMA death, picks new random TOMA instance, increments `allocator_generation`, pushes updated topology to subscribed client simulators.

Extend `toma_msg_q_to_client` message types to include `CDV_TOPOLOGY_UPDATE` carrying `allocator_toma_id` and `allocator_generation`.

### 7.2 Management Simulator (`mgmt/nvmeibm_mgmt_simu.h/.c`)

Add CDV and TPV volume descriptors to the config database:

```c
struct sim_cdv_config {
    u8   cdv_uuid[16];
    u32  allocator_size_gib;  // default 1
    u32  cdv_extent_size_mib;
    u32  capacity_mb;
    u32  max_tpvs;
};

struct sim_tpv_config {
    u8   tpv_uuid[16];
    u8   cdv_uuid[16];
    u32  tpv_extent_size_kb;
    u64  virtual_size_mb;
};
```

Extend `nvmeibm_mcs_simu` to emit `AttachVolumes` messages when `mgmt_sim_attach_tpv(client_id, tpv_uuid)` is called:
1. Look up TPV → find parent CDV.
2. Emit `AttachVolumes` for CDV with `is_hidden=true`, `reservation.mode=SHARED_READ_WRITE`.
3. After CDV attach confirmed: emit `AttachVolumes` for TPV with `cdvConf` inline and `volumeClass=TPV`.

Add `mgmt_sim_attach_cdv_to_toma_nodes(cdv_uuid)` — simulates TOMA auto-attach on CDV creation.

### 7.3 Client Simulator

Add to `clientSimulator`:

```c
struct sim_tpv_state {
    u8                   tpv_uuid[16];
    u8                   cdv_uuid[16];
    struct nvmeibc_tpv  *tpv;
};

struct sim_tpv_state tpvs[MAX_SIM_TPVS];
int                  n_tpvs;
```

Helpers:

```c
int  clientSim_attach_tpv(clientSimulator *c, u8 *tpv_uuid);
int  clientSim_tpv_write_verify(clientSimulator *c, u8 *tpv_uuid,
                                u64 lba, u64 len, u8 pattern);
void clientSim_tpv_crash(clientSimulator *c, u8 *tpv_uuid);
```

### 7.4 RAM Disk Simulator

No structural changes — CDV allocator area is plain readable/writable memory in the simulated disk.

Add initializer helper:

```c
void ramDiskSim_init_cdv_allocator(ramDiskSimulator *rd,
                                   u32 allocator_size_gib,
                                   u32 cdv_extent_size_mib,
                                   u64 total_extents);
```

### 7.5 New Simulator Test Scenarios

**`test_tpv_lifecycle.c`**: Create CDV (`cdvExtentSizeMiB=64`) → TOMA attach → create/attach TPV (`tpvExtentSizeKB=512`) → write pattern → detach (verify flush) → re-attach (verify reconstruction) → delete TPV (verify zeroing/reclaim).

**`test_tpv_allocator_recovery.c`**: Write data → `clientSim_tpv_crash` (no flush) → re-attach (verify `load_state` re-walks L1/L2/L3 tree and reconstructs extent_map) → verify read-back. Also test orphan case: TOMA allocated extent (`cdv_extent_md` updated) but client crashed before installing the tree leaf → verify NVCK detects orphan and `cdv_extent_md` entry is cleared.

**`test_tpv_allocator_election.c`**: Attach CDV + TPVs → `toma_sim_trigger_allocator_failover` → verify topology update received → verify subsequent CDV\_extent requests reach new allocator → verify stale-generation requests rejected with `WRONG_GENERATION` and retried.

**`test_tpv_watermark.c`**: Small CDV (2 CDV\_extents) → write until low-watermark fires CDV\_extent request → verify ADMIN channel request reaches TOMA → write until 90% full → verify `CDVCapacityWarning` fires → extend CDV → verify allocation resumes.

---

## Part 8 — UI Implementation Detail

### 8.1 consts.js

File: `nvmesh-management/consts.js`

Add to `componentsPages` object (~line 1029):

```js
tpv: 'tpv',
```

Add new const groups:

```js
consts.volumeClass = {
    REGULAR: 'REGULAR',
    CDV:     'CDV',
    TPV:     'TPV',
};

// Valid power-of-2 values for CDV and TPV extent sizes
consts.cdvExtentSizeMiBValues  = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536];
consts.tpvExtentSizeKBValues  = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536];
```

### 8.2 Express Route — Thin Provisioning Page

New file: `nvmesh-management/routes/thinProvisioning.js`

```js
/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

var express = require('express');
var consts  = require('../consts.js');
var router  = express.Router();

router.get('/tpv', function(req, res) {
    var renderData = {};
    if (req.headers['x-pjax'])
        renderData.layout = false;

    renderData.user = { email: req.user.email, isAdmin: req.user.role === consts.userRoles.ADMIN };
    renderData.componentName = consts.componentsPages.tpv;

    res.render('react', renderData);
});

module.exports = router;
```

File: `nvmesh-management/app.js` — mount alongside existing routers:

```js
const thinProvisioningRouter = require('./routes/thinProvisioning.js');
app.use('/thin-provisioning', thinProvisioningRouter);
```

### 8.3 Router Registration

File: `public/javascripts/components/Router.jsx`

Add to `componentsRegistry`:

```js
[consts.componentsPages.tpv]: `${pagesFolder}/thinProvisioning/ThinProvisioning.js`,
```

### 8.4 Sidebar — New "Thin Provisioning" Section

File: `public/javascripts/components/shared/Sidebar.jsx`

Insert a new top-level entry after the Volumes entry in the `links` array:

```jsx
{
    icon: 'fa-cubes',
    caption: 'Thin Provisioning',
    adminOnly: false,
    subItems: [
        {
            url: '/thin-provisioning/tpv',
            icon: 'fa fa-database',
            caption: 'TPV List',
        }
    ]
},
```

### 8.5 Regular Volumes Table — CDV Filter Checkboxes

File: `public/javascripts/components/pages/volumes/Volumes.jsx`

Add two filter checkboxes **to the right of the Delete/Rebuild buttons** in the toolbar. Both are checked by default. TPVs are not shown in this table at all (they have their own page).

```jsx
// State (add near other useState declarations):
const [showRegular, setShowRegular] = useState(true);
const [showCDVs,    setShowCDVs]    = useState(true);

// Filter applied to the loadVolumes call:
const volumeClassFilter = useMemo(() => {
    const classes = [];
    if (showRegular) classes.push(consts.volumeClass.REGULAR, null, undefined);
    if (showCDVs)    classes.push(consts.volumeClass.CDV);
    // TPVs are excluded — they live on /thin-provisioning/tpv
    return { volumeClass: { $in: classes } };
}, [showRegular, showCDVs]);

// In the toolbar JSX, to the right of the Rebuild button:
<label style={{ marginLeft: 16, fontWeight: 'normal', cursor: 'pointer' }}>
    <input
        type="checkbox"
        checked={showRegular}
        onChange={e => setShowRegular(e.target.checked)}
        style={{ marginRight: 4 }}
    />
    Regular volumes
</label>
<label style={{ marginLeft: 8, fontWeight: 'normal', cursor: 'pointer' }}>
    <input
        type="checkbox"
        checked={showCDVs}
        onChange={e => setShowCDVs(e.target.checked)}
        style={{ marginRight: 4 }}
    />
    CDVs
</label>
```

Pass `volumeClassFilter` into the `loadVolumes` call as an additional filter. For CDV rows, show TPV count as a sub-label in the Capacity column:

```jsx
// In the Capacity column value renderer:
{volume.volumeClass === consts.volumeClass.CDV && volume.cdvConfig && (
    <small className="text-muted"> ({volume.tpvCount || 0}/{volume.cdvConfig.maxTPVs} TPVs)</small>
)}
```

### 8.6 Create/Edit Volume Dialog — CDV Toggle

File: `public/javascripts/components/pages/volumes/createEditModal/CreateEditVolumeModal.jsx`

#### Dropdown constants (add near top of file)

```js
// Power-of-2 values from 64 MB to 64 GB for CDV extent size
const CDV_EXTENT_SIZE_OPTIONS = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536].map(mb => ({
    value: mb,
    label: mb >= 1024 ? `${mb / 1024} GB` : `${mb} MB`,
}));
```

#### useForm default values

The existing `useForm({ mode: 'all', defaultValues: volume })` call already receives `volume` as defaults. Since `volumeClass` and `cdvConfig` are now fields on volume records, they populate automatically. For new volumes `volumeClass` defaults to `'REGULAR'` and `cdvConfig.allocatorSizeGiB` defaults to `1`.

#### CDV toggle (create only)

Insert **after the name/description block and before the RAID level selector**:

```jsx
{isCreate && (
    <FormControl label="Carrier Direct Volume (CDV)">
        <Controller
            name="volumeClass"
            control={control}
            render={({ field }) => (
                <Toggle
                    checked={field.value === 'CDV'}
                    onChange={checked => field.onChange(checked ? 'CDV' : 'REGULAR')}
                    label="Use this volume as a CDV for thin provisioning"
                />
            )}
        />
    </FormControl>
)}
{!isCreate && volume.volumeClass === consts.volumeClass.CDV && (
    <div className="alert alert-info">This volume is a Carrier Direct Volume (CDV).</div>
)}
```

`volumeClass` is immutable after creation — toggle is hidden in edit mode.

#### CDV-specific fields (conditional)

```jsx
{formData.volumeClass === consts.volumeClass.CDV && (
    <fieldset className="cdv-config">
        <legend>CDV Configuration</legend>

        <FormControl
            label="CDV Extent Size"
            hint="Allocation unit carved from the CDV. Power-of-2, 64 MB – 64 GB.">
            <Controller
                name="cdvConfig.cdvExtentSizeMiB"
                control={control}
                rules={{ required: formData.volumeClass === consts.volumeClass.CDV }}
                render={({ field }) => (
                    <Select
                        {...field}
                        options={CDV_EXTENT_SIZE_OPTIONS}
                        placeholder="Select CDV extent size"
                    />
                )}
            />
        </FormControl>

        <FormControl
            label="Allocator Size (GB)"
            hint="Space reserved at the start of the CDV for allocator metadata. Integer >= 1, default 1 GB. Determines how many CDV extents can be tracked: (allocatorSizeGiB * 1 GB - 4 KB) / 24.">
            <Controller
                name="cdvConfig.allocatorSizeGiB"
                control={control}
                rules={{
                    required: formData.volumeClass === consts.volumeClass.CDV,
                    validate: v => (Number.isInteger(Number(v)) && Number(v) >= 1) || 'Must be a positive integer',
                }}
                render={({ field }) => (
                    <Input type="number" min={1} {...field} />
                )}
            />
        </FormControl>

        <FormControl
            label="Max TPVs"
            hint="Maximum number of TPVs this CDV can host. Default 512. Can be changed after creation. If lowered below the current TPV count, existing TPVs are unaffected; new TPV creation is blocked until the count drops below the new limit.">
            <Controller
                name="cdvConfig.maxTPVs"
                control={control}
                render={({ field }) => (
                    <Input type="number" min={1} {...field} />
                )}
            />
        </FormControl>
    </fieldset>
)}
```

No special submit-path fork needed. The existing `VolumesService.create(buildVolumePayload(data))` sends the full payload including `volumeClass: 'CDV'` and `cdvConfig`. The backend `createVolume` handler branches on `volumeClass` (§1.4).

### 8.7 TPV Table Page

New file: `public/javascripts/components/pages/thinProvisioning/ThinProvisioning.jsx`

```jsx
/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/* global React, consts */

import FiltSortTable from '../../filtsort-table/FiltSortTable.jsx';
import { useAlerts } from '../../core/Alert.jsx';
import { useConfirmationDialog } from '../../shared/ConfirmationDialog.jsx';
import { VolumesService } from '../../services/api/volumes.service.js';
import { extractErrorMsg } from '../../utils.js';
import NewButton from '../../shared/NewButton.jsx';
import CreateTPVModal from './CreateTPVModal.jsx';

const { useRef, useState, useEffect } = React;

const ThinProvisioning = () => {
    const { successAlert, errorAlert } = useAlerts();
    const [confirm] = useConfirmationDialog();
    const [selectedTPVs, setSelectedTPVs] = useState([]);
    const [showCreateModal, setShowCreateModal] = useState(false);
    const [editTPV, setEditTPV] = useState({});
    const tableRef = useRef();

    useEffect(() => {
        const interval = setInterval(() => reloadTable(false), 3000);
        return () => clearInterval(interval);
    }, []);

    const reloadTable = (deselectMissingRows = true) => {
        if (tableRef.current) {
            tableRef.current.reloadRows(deselectMissingRows);
            tableRef.current.reloadTotal();
        }
    };

    const columns = [
        { name: 'Name',         field: 'name',                      placeholder: 'Search by Name' },
        { name: 'Parent CDV',   field: 'tpvConfig.cdvId',           placeholder: 'Filter by CDV',
          value: tpv => tpv.tpvConfig?.cdvName || tpv.tpvConfig?.cdvId },
        { name: 'Virtual Size', field: 'tpvConfig.virtualSizeGB',   placeholder: 'Filter by Size',
          value: tpv => `${tpv.tpvConfig?.virtualSizeGB ?? '—'} GB` },
        { name: 'Max Size',     field: 'tpvConfig.maxVirtualSizeGB',
          value: tpv => `${tpv.tpvConfig?.maxVirtualSizeGB ?? '—'} GB` },
        { name: 'Client',       field: 'tpvConfig.exclusiveClient',
          value: tpv => tpv.tpvConfig?.exclusiveClient || <em>Detached</em> },
        { name: 'Status',       field: 'status', value: tpv => tpv.status },
    ];

    const loadTPVs = async(filter, sort, page, count) => {
        return await VolumesService.loadVolumes(
            { ...filter, volumeClass: consts.volumeClass.TPV },
            sort, page, count
        );
    };

    const loadTotal = async(filter) => {
        return await VolumesService.loadTotal({ ...filter, volumeClass: consts.volumeClass.TPV });
    };

    const deleteSelected = async() => {
        const names = selectedTPVs.map(t => t.name).join(', ');
        const confirmed = await confirm({ message: `Delete TPV(s): ${names}?` });
        if (!confirmed) return;

        const response = await VolumesService.deleteTPV(selectedTPVs.map(t => t._id));
        if (response.success) {
            successAlert('TPV(s) deleted');
            reloadTable();
        } else {
            errorAlert(extractErrorMsg(response.error));
        }
    };

    return (
        <div>
            <div className="page-header">
                <h1>Thin-Provisioned Volumes</h1>
                <div className="actions">
                    <NewButton onClick={() => { setEditTPV({}); setShowCreateModal(true); }} label="New TPV" />
                    <button
                        className="btn btn-danger"
                        disabled={!selectedTPVs.length}
                        onClick={deleteSelected}>
                        Delete
                    </button>
                </div>
            </div>

            <FiltSortTable
                ref={tableRef}
                columns={columns}
                loadRows={loadTPVs}
                loadTotal={loadTotal}
                onSelectionChange={setSelectedTPVs}
                onRowDoubleClick={tpv => { setEditTPV(tpv); setShowCreateModal(true); }}
            />

            {showCreateModal && (
                <CreateTPVModal
                    tpv={editTPV}
                    onClose={() => setShowCreateModal(false)}
                    onSuccess={() => {
                        setShowCreateModal(false);
                        reloadTable();
                        successAlert('TPV saved');
                    }}
                />
            )}
        </div>
    );
};

export default ThinProvisioning;
```

### 8.8 TPV Create/Edit Modal

New file: `public/javascripts/components/pages/thinProvisioning/CreateTPVModal.jsx`

```jsx
/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/* global React, ReactHookForm, consts */

import Modal from '../../core/Modal.jsx';
import FormControl from '../../core/FormControl.jsx';
import Input from '../../core/Input.jsx';
import Select from '../../core/Select.jsx';
import { VolumesService } from '../../services/api/volumes.service.js';
import { extractErrorMsg } from '../../utils.js';

const { useForm, Controller } = ReactHookForm;
const { useState, useEffect } = React;

// Power-of-2 values from 64 KB to 64 MB for TPV extent size
const TPV_EXTENT_SIZE_OPTIONS = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536].map(kb => ({
    value: kb,
    label: kb >= 1024 ? `${kb / 1024} MB` : `${kb} KB`,
}));

const CreateTPVModal = ({ tpv = {}, onClose, onSuccess }) => {
    const isCreate = !tpv._id;
    const [cdvOptions, setCdvOptions] = useState([]);
    const [isSubmitting, setIsSubmitting] = useState(false);
    const [submitError, setSubmitError] = useState(null);

    const { register, handleSubmit, formState, control, watch } = useForm({
        mode: 'all',
        defaultValues: {
            name:                          tpv.name || '',
            description:                   tpv.description || '',
            'tpvConfig.cdvId':             tpv.tpvConfig?.cdvId || '',
            'tpvConfig.tpvExtentSizeKB':   tpv.tpvConfig?.tpvExtentSizeKB || '',
            'tpvConfig.virtualSizeGB':     tpv.tpvConfig?.virtualSizeGB || '',
            'tpvConfig.maxVirtualSizeGB':  tpv.tpvConfig?.maxVirtualSizeGB || 1000,
        }
    });

    const selectedCdvId = watch('tpvConfig.cdvId');
    const selectedCdv   = cdvOptions.find(c => c.value === selectedCdvId);

    useEffect(() => {
        VolumesService.getCDVs().then(res =>
            setCdvOptions(res.map(cdv => ({
                value: cdv._id,
                label: `${cdv.name} (${cdv.tpvCount || 0}/${cdv.cdvConfig?.maxTPVs} TPVs, ${cdv.capacity} GB)`,
                cdv,
            })))
        );
    }, []);

    const onSubmit = handleSubmit(async (data) => {
        setIsSubmitting(true);
        setSubmitError(null);

        const payload = {
            name:        data.name,
            description: data.description,
            volumeClass: consts.volumeClass.TPV,
            tpvConfig: {
                cdvId:            data['tpvConfig.cdvId'],
                tpvExtentSizeKB:  Number(data['tpvConfig.tpvExtentSizeKB']),
                virtualSizeGB:    Number(data['tpvConfig.virtualSizeGB']),
                maxVirtualSizeGB: Number(data['tpvConfig.maxVirtualSizeGB']),
            },
        };

        const response = isCreate
            ? await VolumesService.create(payload)
            : await VolumesService.updateTPV({ _id: tpv._id, ...payload });

        if (response.success) {
            onSuccess();
        } else {
            setSubmitError(extractErrorMsg(response.error));
        }
        setIsSubmitting(false);
    });

    return (
        <Modal
            title={isCreate ? 'Create TPV' : `Edit TPV: ${tpv.name}`}
            onClose={onClose}
            footer={
                <>
                    <button className="btn btn-default" onClick={onClose}>Cancel</button>
                    <button
                        className="btn btn-primary"
                        onClick={onSubmit}
                        disabled={!formState.isValid || isSubmitting}>
                        {isCreate ? 'Create' : 'Update'}
                    </button>
                </>
            }>

            <FormControl label="Name" error={formState.errors.name?.message}>
                <Input {...register('name', { required: 'Name is required' })} />
            </FormControl>

            <FormControl label="Description">
                <Input {...register('description')} />
            </FormControl>

            <FormControl label="Parent CDV" error={formState.errors['tpvConfig.cdvId']?.message}>
                <Controller
                    name="tpvConfig.cdvId"
                    control={control}
                    rules={{ required: 'Parent CDV is required' }}
                    render={({ field }) => (
                        <Select
                            {...field}
                            options={cdvOptions}
                            placeholder="Select CDV"
                            isDisabled={!isCreate}
                        />
                    )}
                />
                {selectedCdv && (
                    <small className="text-muted">
                        CDV extent: {selectedCdv.cdv.cdvConfig?.cdvExtentSizeMiB} MB
                    </small>
                )}
            </FormControl>

            <FormControl
                label="TPV Extent Size"
                hint={selectedCdv
                    ? `Power-of-2, 64 KB – ${selectedCdv.cdv.cdvConfig?.cdvExtentSizeMiB * 1024} KB (<= CDV extent size)`
                    : 'Power-of-2, 64 KB – 64 MB'}
                error={formState.errors['tpvConfig.tpvExtentSizeKB']?.message}>
                <Controller
                    name="tpvConfig.tpvExtentSizeKB"
                    control={control}
                    rules={{
                        required: 'TPV extent size is required',
                        validate: v => {
                            const cdvMB = selectedCdv?.cdv.cdvConfig?.cdvExtentSizeMiB;
                            if (cdvMB && v > cdvMB * 1024)
                                return `Cannot exceed CDV extent size (${cdvMB * 1024} KB)`;
                            return true;
                        },
                    }}
                    render={({ field }) => (
                        <Select
                            {...field}
                            options={TPV_EXTENT_SIZE_OPTIONS.filter(o =>
                                !selectedCdv || o.value <= selectedCdv.cdv.cdvConfig?.cdvExtentSizeMiB * 1024
                            )}
                            placeholder="Select TPV extent size"
                            isDisabled={!isCreate}
                        />
                    )}
                />
            </FormControl>

            <FormControl
                label="Virtual Size (GB)"
                hint={selectedCdv ? `Max: ${selectedCdv.cdv.capacity} GB (parent CDV capacity)` : undefined}
                error={formState.errors['tpvConfig.virtualSizeGB']?.message}>
                <Input
                    type="number"
                    min={1}
                    {...register('tpvConfig.virtualSizeGB', {
                        required: 'Virtual size is required',
                        min: { value: 1, message: 'Must be at least 1 GB' },
                        validate: v => {
                            if (selectedCdv && Number(v) > selectedCdv.cdv.capacity)
                                return `Cannot exceed parent CDV capacity (${selectedCdv.cdv.capacity} GB)`;
                            return true;
                        },
                    })}
                />
            </FormControl>

            <FormControl label="Max Virtual Size (GB)" hint="Hard cap for future grows (default: 1000 GB).">
                <Input
                    type="number"
                    min={1}
                    {...register('tpvConfig.maxVirtualSizeGB', {
                        min: { value: 1, message: 'Must be at least 1 GB' },
                    })}
                />
            </FormControl>

            {submitError && <div className="alert alert-danger">{submitError}</div>}
        </Modal>
    );
};

export default CreateTPVModal;
```

Note: TPV create calls `VolumesService.create(payload)` (same as CDV) — both go through `POST /volumes/save`.

### 8.9 VolumesService Additions

File: `public/javascripts/components/services/api/volumes.service.js`

```js
// Add to the VolumesService object:

async updateTPV(tpv) {
    return await apiService.post('/tpv/update', tpv);
},

async deleteTPV(tpvIds) {
    return await apiService.post('/tpv/delete', tpvIds);
},

async extendTPV(payload) {
    return await apiService.post('/tpv/extend', payload);
},

async getCDVs(filter = {}) {
    return await apiService.get('/all/0/0', {
        filter: { ...filter, volumeClass: 'CDV' },
        projection: {}
    });
},
```

CDV creation uses the existing `create(volume)` method. TPV creation also uses the existing `create(volume)` method. Only update, delete, and extend need new methods (because they have different endpoint paths or payload shapes).

### 8.10 Route Handlers in volumes.js

File: `nvmesh-management/routes/volumes.js`

```js
// POST /volumes/tpv/update
router.post('/tpv/update', isAdminRole, async function(req, res) {
    try {
        const result = await volumeModule.updateTPV(req.body, req.user);
        createAuditRequestLog(req, 'updateTPV');
        res.json(result);
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

// POST /volumes/tpv/delete
router.post('/tpv/delete', isAdminRole, async function(req, res) {
    try {
        const result = await volumeModule.deleteTPVs(req.body, req.user);
        createAuditRequestLog(req, 'deleteTPV');
        res.json(result);
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});

// POST /volumes/tpv/extend
router.post('/tpv/extend', isAdminRole, async function(req, res) {
    try {
        const result = await volumeModule.extendTPV(req.body, req.user);
        createAuditRequestLog(req, 'extendTPV');
        res.json(result);
    } catch (err) {
        res.json({ success: false, error: err.message });
    }
});
```

### 8.11 Attach Dialog — TPV Informational Note

File: `public/javascripts/components/pages/clients/AttachDetachModal.jsx`

When a TPV is selected in the volume list, show an informational banner (no functional change):

```jsx
{vol.volumeClass === consts.volumeClass.TPV && (
    <div className="alert alert-info" style={{ marginTop: 4, padding: '4px 8px', fontSize: '0.85em' }}>
        <i className="fa fa-info-circle" />{' '}
        CDV <strong>{vol.tpvConfig?.cdvName || vol.tpvConfig?.cdvId}</strong> will
        also be attached as a hidden volume to provide physical storage.
    </div>
)}
```

`tpvConfig.cdvName` is populated by the backend via a `$lookup` aggregation (§1.4).

---

## Part 9 — File Checklist

### New files

- `nvmesh-management/routes/thinProvisioning.js` — Express GET handler for `/thin-provisioning/tpv`
- `nvmesh-management/modules/cdvTomaAutoAttach.js` — CDV auto-attach to TOMA nodes
- `nvmesh-management/models/kafkaMessages/CDVAllocatorFreeAll.js` — Kafka message: force-reclaim all TPV extents
- `nvmesh-management/models/kafkaMessages/CDVCapacityWarning.js` — Kafka message: CDV near-full notification from TOMA
- `public/javascripts/components/pages/thinProvisioning/ThinProvisioning.jsx` — TPV list page
- `public/javascripts/components/pages/thinProvisioning/ThinProvisioning.js` — Compiled output
- `public/javascripts/components/pages/thinProvisioning/CreateTPVModal.jsx` — TPV create/edit dialog
- `public/javascripts/components/pages/thinProvisioning/CreateTPVModal.js` — Compiled output
- `nvmesh-kernel/clnt/tpv/nvmeibc_tpv.h` — TPV data structures
- `nvmesh-kernel/clnt/tpv/nvmeibc_tpv.c` — TPV attach/detach, block device
- `nvmesh-kernel/clnt/tpv/nvmeibc_tpv_allocator.c` — TPV extent map, alloc/free
- `nvmesh-kernel/clnt/tpv/nvmeibc_tpv_io.c` — TPV IO dispatch
- `nvmesh-kernel/clnt/tpv/nvmeibc_tpv_persist.c` — TPV allocator state persistence
- `nvmesh-kernel/clnt/tpv/nvmeibc_tpv_recovery.c` — TPV cold recovery
- `nvmesh-kernel/toma/nvmeibt_cdv_allocator.c` — TOMA CDV.allocator implementation
- `nvmesh-kernel/clnt/block/unitest/test_tpv_lifecycle.c` — Simulator test
- `nvmesh-kernel/clnt/block/unitest/test_tpv_allocator_recovery.c` — Simulator test
- `nvmesh-kernel/clnt/block/unitest/test_tpv_allocator_election.c` — Simulator test
- `nvmesh-kernel/clnt/block/unitest/test_tpv_watermark.c` — Simulator test
- `nvmesh-kernel/tools/tpv_inspect.py` — Python observability script
- `nvmesh-kernel/tools/cdv_inspect.py` — Python observability script

### Modified files

- `nvmesh-management/consts.js` — Add `componentsPages.tpv`, `volumeClass`, extent size value arrays
- `nvmesh-management/app.js` — Mount `thinProvisioningRouter` at `/thin-provisioning`
- `nvmesh-management/modules/volume.js` — CDV/TPV branching in `createVolume` and `updateVolume`; new `updateTPV`, `deleteTPVs`, `extendTPV`; `cdvName` `$lookup` for TPV queries
- `nvmesh-management/modules/client.js` — Add `attachTPV`, `detachTPV` helpers
- `nvmesh-management/modules/kafka.js` — Add `CDVCapacityWarning` consumer; add `sendCDVAllocatorFreeAll`
- `nvmesh-management/routes/volumes.js` — Add `/tpv/update`, `/tpv/delete`, `/tpv/extend` handlers
- `nvmesh-management/validationSchemes/definitions/volume.js` — Add `volumeClass`, `cdvConfig`, `tpvConfig`, `tpvCount` fields
- `nvmesh-management/models/kafkaMessages/VolumeMessage.js` — Add `volumeClass`, `tpvConfig`, `cdvConfig`, `isHidden` fields
- `nvmesh-management/models/kafkaMessages/AttachVolumes.js` — Add `cdvConf` inline for TPV attaches
- `public/javascripts/components/Router.jsx` — Add `tpv` -> `ThinProvisioning.js` entry
- `public/javascripts/components/shared/Sidebar.jsx` — Add "Thin Provisioning" top-level nav section
- `public/javascripts/components/pages/volumes/Volumes.jsx` — Add filter checkboxes (Show regular volumes / Show CDVs); CDV TPV count sub-label
- `public/javascripts/components/pages/volumes/createEditModal/CreateEditVolumeModal.jsx` — CDV toggle + CDV config fieldset
- `public/javascripts/components/services/api/volumes.service.js` — Add `updateTPV`, `deleteTPV`, `extendTPV`, `getCDVs`
- `public/javascripts/components/pages/clients/AttachDetachModal.jsx` — TPV informational banner
- `nvmesh-kernel/clnt/nvmeibc_main_capi_manipulate_vols.inc.c` — Parse `volumeClass`, `is_hidden`; route CDV/TPV attach
- `nvmesh-kernel/toma/nvmeibt_recovery.c` — Call `cdv_allocator_cold_recovery` after EC recovery
- `nvmesh-kernel/common_public/nvmeib_public_procfs.h` — (If needed) no struct changes expected
- `nvmesh-kernel/clnt/block/unitest/toma/nvmeibt_toma_simu.h/.c` — Add CDV allocator sim state, ADMIN channel handlers, failover trigger
- `nvmesh-kernel/clnt/block/unitest/mgmt/nvmeibm_mgmt_simu.h/.c` — Add CDV/TPV config descriptors, MCS message generation
- `nvmesh-kernel/clnt/block/unitest/nvmesh_sim.h` — Add `sim_tpv_state` to client simulator

---

## Part 5 — TPV Encryption

### 5.1 Overview

TPV encryption follows architecture decision #18: encryption is at the TPV level, not the CDV level. The CDV stores raw (unencrypted) extents; each TPV independently manages its own LUKS container in its own virtual address space. The LUKS header is written as the first bytes of the TPV — the client-side TPV allocator binds those logical bytes to whichever CDV extent it allocates first, on demand. **No pre-allocation of CDV extents is required.**

The management-side workflow mirrors regular volume encryption as closely as possible: the same "Encryption" dropdown button (Init Encryption, Add/Rotate/Delete Passphrase, Acknowledge Error), the same REST endpoints (`POST /volumes/initEncryption`, etc.), the same Kafka message types and payload schema, and the same TOMA-side `cryptsetup` execution pattern. The **only** TPV-specific difference on the TOMA is which block-device path cryptsetup runs against — `/dev/nvmesh-tpv/<tpv_name>` instead of `/dev/nvmesh/e_<name>` — because the TPV itself is attached on the TOMA node (exclusively, with preempt) before the Kafka command arrives. There is no dm-linear wrapper, no shadow-volume clone, and no CDV-geometry plumbing in the Kafka payload.

### 5.2 How Regular Volume Encryption Works (reference)

For comparison, the regular volume encryption flow:

```
UI (Volumes.jsx)                   Encrypt dropdown → Init Encryption
  ↓
VolumesService.initEncryption()    POST /volumes/initEncryption
  ↓
routes/volumes.js                  Audit log + call encryptionModule
  ↓
volumeEncryption.js                runEncryptionCommand():
  ├─ Fetch volume by UUID          Verify isEncrypted, !isInitialized
  ├─ chooseTOMAForEncryption()     Round-robin across zone TOMAs
  ├─ setEncryptionCommand()        DB: status = PENDING_SEND, $inc commandIndex
  ├─ sendEncryptionCommandToTOMA() Build InitEncryption Kafka message, send to TOMA_COMMANDS topic
  └─ updateLastCommandSent()       DB: status = SENT
  ↓
TOMA (nvmeibt_kafka.c)             toma_CMD_handler():
  ├─ Validate bootTime, volume     Check encrypt_idx, no concurrent op
  ├─ Create shadow volume          Attach same chunks as e_<name>
  ├─ Write passphrase to file      /root/nvmesh_toma_tmp/old_passphrase_<shadow>
  ├─ cryptsetup luksFormat          On /dev/nvmesh/e_<name>
  ├─ Cleanup                        Detach shadow, delete passphrase file
  └─ Send response via Kafka       encryptionCommandResponse to management topic
  ↓
kafkaRouter.js                     Route to volumeEncryption.handleCommandResponse()
  ↓
volumeEncryption.js                DB: status = EXECUTED, isInitialized = true, isReady = true
```

**Key TOMA details:**
- Shadow volume `e_<name>` is created by re-attaching the same disk chunks as a separate block device
- `cryptsetup luksFormat --sector-size=4096 --key-slot=<slot> --key-size=<keySize> --key-file=<file> /dev/nvmesh/e_<name>`
- Passphrase ops (add/rotate/delete) use the same shadow mechanism with `luksAddKey`, `luksChangeKey`, `luksRemoveKey`
- TOMA response codes: `SUCCESS(1)`, `CMD_ERR(2)`, `TOMA_ERR(3)`, `UNSEEN(4)`, `MANUAL_ACTION_NEEDED(5)`

### 5.3 TPV Encryption — Architecture Decisions

19. **No CDV-level encryption**: The CDV remains unencrypted. Each TPV independently manages its own LUKS container within its allocated CDV extents. This means different TPVs on the same CDV can have different encryption keys.
20. **LUKS header location**: The LUKS header occupies the first bytes of the TPV's virtual address space. The default encryption header size is 16 MB — well within the minimum TPV extent size of 64 KB and the minimum CDV extent size of 64 MB. The client-side TPV allocator materialises the backing CDV extent on the first write, which is `cryptsetup luksFormat`'s header write during Init Encryption; no upfront CDV reservation is required.
21. **~~First-extent pre-allocation~~ (removed)**: Earlier revisions of this plan called for a management-issued `CDV_ALLOC_EXTENT` round-trip before TPV insert so the LUKS header was guaranteed backing storage. This has been **removed**. The TPV's own first-write path triggers extent allocation transparently via the existing client-↔-TOMA admin channel (see §3). No `firstExtentIndex` field, no management-side IB admin message, no pre-insert allocator dependency. The only CDV-capacity failure path is "CDV full at first write", which surfaces to the client as the existing `CDV_ALLOC_CDV_FULL` response during cryptsetup's header I/O.
22. **TOMA selection for TPV encryption**: Instead of zone-based round-robin (regular volumes), TPV encryption commands are sent to a TOMA node that hosts one of the CDV's first pRAID RW disk segments (and therefore can attach the CDV). Preference order: the CDV's current allocator TOMA (best locality for extent allocation during LUKS header write), then a random pick from the remaining candidates.
23. **Attach the TPV exclusively on the TOMA**: Before sending the encryption Kafka command, management attaches the TPV itself to the chosen TOMA node using the existing client-attach path (`clientModule.attachTPV`) with `{preempt: true, mode: EXCLUSIVE_READ_WRITE}`. The TOMA's client kernel module then exposes the TPV at `/dev/nvmesh-tpv/<tpv_name>` — the exact same device path a real client would see. Preempt is used so any stale holder (e.g., a crashed client that was never cleaned up) is fenced; refusing to run encryption while a live client holds the TPV is a higher-layer policy decision not enforced by management today.
24. **No shadow and no dm-linear**: Because the TPV is already locally attached on the TOMA when the Kafka command arrives, TOMA skips `nvmeibt_attach_vol_for_encryption` entirely and runs `cryptsetup` directly against `/dev/nvmesh-tpv/<tpv_name>`. No dm-linear wrapper, no shadow-clone, no CDV-byte-offset arithmetic.
25. **Reuse existing REST endpoints and Kafka schema**: The same endpoints (`POST /volumes/initEncryption`, `addPassphrase`, `deletePassphrase`, `rotatePassphrase`) and Kafka message types work for both regular volumes and TPVs. The Kafka payload schema is **unchanged** — no new `cdvName`/`cdvByteOffset`/`shadowSizeSectors` fields. The TOMA side distinguishes TPV from regular volume by the chunk-less shape of the block-device record: `(vol->from_config.n_chunks == 0) && !vol->from_config.is_cdv`. TOMA's `struct nvmeibt_block_device` has no kernel-client `type` enum; a TPV is the only class that arrives with `chunks: []` from management.
26. **Management auto-detaches after response**: When the Kafka encryption response arrives at `handleCommandResponse`, management calls the symmetric `clientModule.detachTPV` so the TOMA releases the TPV. The real client (user) can then attach it normally.

### 5.4 TPV Encryption Flow

```
UI (ThinProvisioning.jsx)          Encrypt dropdown → Init Encryption
  ↓
VolumesService.initEncryption()    POST /volumes/initEncryption  (same endpoint)
  ↓
routes/volumes.js                  Audit log + call encryptionModule
  ↓
volumeEncryption.js                runEncryptionCommand():
  ├─ Fetch volume by UUID          Verify isEncrypted, !isInitialized
  ├─ chooseTOMAForEncryption()     ← NEW BRANCH: for TPV, pick TOMA on CDV's first pRAID
  ├─ attachTPVToTOMAForEncryption()← NEW STEP: clientModule.attachTPV(toma._id, clientUUID, tpv, {preempt: true})
  ├─ setEncryptionCommand()        DB: status = PENDING_SEND, $inc commandIndex,
  │                                    ALSO stamp encryption.command.tpvAutoAttachedTOMA = toma._id
  ├─ sendEncryptionCommandToTOMA() Kafka payload UNCHANGED — same envelope as regular volumes
  └─ updateLastCommandSent()       DB: status = SENT  (unchanged)
  ↓
TOMA (nvmeibt_kafka.c)             toma_CMD_handler():
  ├─ Validate bootTime             (unchanged)
  ├─ Detect TPV mode               vol->from_config.n_chunks == 0 && !is_cdv
  ├─ Skip shadow attach            /dev/nvmesh-tpv/<tpv_name> already exists
  ├─ Write passphrase to file      (unchanged)
  ├─ cryptsetup luksFormat          On /dev/nvmesh-tpv/<tpv_name>
  │                                 (client-side TPV allocator materialises CDV extent(s)
  │                                  transparently during LUKS header write)
  └─ Send response via Kafka       encryptionCommandResponse  (unchanged)
  ↓
kafkaRouter.js                     Route to volumeEncryption.handleCommandResponse()
  ↓
volumeEncryption.js                DB: status = EXECUTED, isInitialized = true, isReady = true
  └─ detachTPVFromTOMAForEncryption() ← NEW STEP: release TPV from TOMA; clear tpvAutoAttachedTOMA
```

### 5.5 ~~CDV Extent Pre-allocation for Encrypted TPVs~~ (removed)

Previous revisions of this plan required management to pre-allocate the first CDV data extent via `CDV_ALLOC_EXTENT` before inserting the encrypted TPV, storing the result in `tpvConfig.firstExtentIndex` for later use by the TOMA's dm-linear wrapper. Both the pre-allocation and the `firstExtentIndex` field have been **removed**. Writing the LUKS header triggers the normal client-side first-write allocation path; the CDV allocator sees an ordinary TPV extent request and no special management pre-handshake exists. Any "CDV full" failure is surfaced by the client's first write and funnels into the existing encryption error response path.

### 5.6 TOMA Selection for TPV Encryption

**New function: `chooseTOMAForTPVEncryption(tpvVolume, callback)`** in `volumeEncryption.js`:

1. Look up the parent CDV via `tpvConfig.cdvId`.
2. Find candidate TOMA nodes: servers that own an RW disk segment in the CDV's first pRAID and are `tomaStatus === UP`.
3. Prefer the CDV's current allocator TOMA (read from `cdv.currentAllocatorTomaHostname`, populated by `client.js::handleAttachSatelliteRequest`) if present in the candidates — same-node allocator affinity minimises cross-node round-trips during the LUKS header write. Otherwise pick a random candidate.
4. Return the selected TOMA with its `bootTime` and `topics`.

**Integration**: `chooseTOMAForEncryption()` gains a branch:
```js
if (volume.volumeClass === consts.volumeClass.TPV) {
    return scope.chooseTOMAForTPVEncryption(volume, callback);
}
// ... existing zone round-robin for regular volumes
```

### 5.7 Kafka Message Changes

**None.** The `EncryptionCommandMessage` base class and all four subclasses (`InitEncryption`, `AddPassphrase`, `DeletePassphrase`, `RotatePassphrase`) are unchanged. The TOMA distinguishes TPV from regular volume by the chunk-less shape of the block-device entry: `(vol->from_config.n_chunks == 0) && !vol->from_config.is_cdv`. TOMA already maintains `from_config.n_chunks` and `from_config.is_cdv` on every locally-attached volume; no new fields or JSON keys are needed.

### 5.8 TOMA Changes (`nvmeibt_kafka.c`, `nvmeibt_recovery.c`)

#### Branch in `start_encrypt_action`

Inside `start_encrypt_action()` the existing code builds a `cryptsetup` command targeting `/dev/nvmesh/<shadow_vol_name>` and calls `nvmeibt_attach_vol_for_encryption(vol, shadow_vol_name, encrypt_params)` to create the shadow and drive the exec. For TPVs (detected by `(vol->from_config.n_chunks == 0) && !vol->from_config.is_cdv`) the branch:

1. Builds the same `cryptsetup` command string, but with device path `/dev/nvmesh-tpv/<vol->from_config.client_blkdev_name>`.
2. Calls `nvmeibt_start_encrypt_for_tpv(vol, encrypt_params)` instead of `nvmeibt_attach_vol_for_encryption`.

`nvmeibt_start_encrypt_for_tpv` (new, `nvmeibt_recovery.c`) skips the shadow-clone and attach-WQ scheduling entirely. It sets `encrypt_params->exec_ctx.blkdev = vol; encrypt_params->origin_vol = vol; vol->encrypt_params = encrypt_params;` allocates stdout/stderr buffers, sets a TPV-specific exec-done callback `tpv_encrypt_after_exec_cb`, and invokes `nvmeibt_run_exec_on_blkdev(exec_ctx)` directly.

`tpv_encrypt_after_exec_cb` is a trimmed copy of `detach_shadow_vol_for_encryption_finalize`: it builds and sends the Kafka response via `nvmeibt_kafka_send_encrypt_cmd_response`, frees the stdout/stderr buffers and the `encrypt_params`, and clears `vol->encrypt_params`. It does **not** free `vol` — `vol` is the real TPV, owned by TOMA's block-device hash and still attached; management's post-response detach will remove it.

No changes to the `encrypt_cmd_t` struct, no changes to `parse_CMD()`, no new JSON keys.

### 5.9 Management Module Changes

#### `modules/volume.js` — `createTPV()`

Accept `isEncrypted` and `encryption.headerSize` on the input. When `isEncrypted`:
- Set `isReady: false` (flips to true only after `initEncryption` succeeds).
- Add `encryption: { headerSize, isInitialized: false }` sub-document.
- Set `action: INIT_ENCRYPTION_REQUIRED` so the UI surfaces the pending-init state.

No CDV-side work at creation time — no pre-allocation, no extent reservation, no new Kafka chatter. `prepareCDVForCreate()` strips any incoming `isEncrypted`/`encryption` on a CDV payload (Architecture Decision #18).

#### `modules/volumeEncryption.js` — new attach/detach helpers

```js
scope.attachTPVToTOMAForEncryption = (dbVolume, executingTOMA, cb) => {
    // Look up the client doc for executingTOMA._id (TOMA nodes also register
    // as clients because the same host runs both modules).
    // Call clientModule.attachTPV(toma._id, clientDoc.uuid, tpv._id,
    //                             {preempt: true}, cb);
    // syncFlush is deliberately NOT passed — attachTPV's applySyncFlush step
    // persists sourceUUID on the TPV and would leak the encryption-time value
    // past detach. Default behaviour keeps sync_flush on, matching real client
    // attaches.
    // Verify tpvConfig.exclusiveClient now matches the TOMA; surface a
    // retriable error otherwise.
};

scope.detachTPVFromTOMAForEncryption = (tpvName, tomaId, cb) => {
    // Symmetric: clientModule.detachTPV(tomaId, clientDoc.uuid, tpvName, cb).
    // Idempotent and best-effort; recovery cleans up stragglers on restart.
};
```

#### `modules/volumeEncryption.js` — `runEncryptionCommand()` orchestration

Inserted between `chooseTOMAForEncryption()` and `setEncryptionCommand()`:
```js
(callback) => {
    if (dbVolume.volumeClass !== consts.volumeClass.TPV) return callback();
    scope.attachTPVToTOMAForEncryption(dbVolume, executingTOMA, callback);
},
```

`setEncryptionCommand()` additionally stamps `encryption.command.tpvAutoAttachedTOMA = executingTOMA._id` when the volume is a TPV, so the response handler and crash-recovery path know to detach.

#### `modules/volumeEncryption.js` — `handleCommandResponse()`

After updating the DB with the command result, if the just-updated document is a TPV and `encryption.command.tpvAutoAttachedTOMA` is set, invoke `detachTPVFromTOMAForEncryption(...)` and then `$unset` that field. Regular volumes are unaffected.

#### `modules/volumeEncryption.js` — `cleanupTPVAutoAttachesAfterStartup()` (new)

Scan for TPVs with `encryption.command.tpvAutoAttachedTOMA` set and `encryption.command.status === EXECUTED`. Drive the symmetric detach for each — covers the "management died between response receipt and detach" window. Invoked from `sanityAndRecover.js` alongside `resendStaleEncryptionCommands`.

#### `modules/volumeEncryption.js` — `chooseTOMAForEncryption()`

Branch on `volume.volumeClass === 'TPV'` to use `chooseTOMAForTPVEncryption()` instead of zone round-robin.

#### `modules/volumeEncryption.js` — `sendEncryptionCommandToTOMA()`

Unchanged. The Kafka payload is identical to the regular-volume case.

#### `modules/volumeEncryption.js` — `verifyEncryptionCommand()`

Unchanged — the existing checks (`isEncrypted`, `isInitialized`, `action`) apply identically to TPVs. No additional "client already attached" guard is enforced today; preempt handles reattachment cleanly, and any interlock against encrypting a TPV currently in use by a live client is a higher-layer policy choice that this plan defers.

### 5.10 UI Changes

#### `ThinProvisioning.jsx`

Add the Encryption dropdown and modals, mirroring `Volumes.jsx`:

**New imports:**
```jsx
import { DropdownButton, DropdownButtonItem } from '../../shared/DropdownButton.jsx';
import InitEncryptionModal from '../volumes/InitEncryptionModal.jsx';
import PassphraseModal from '../volumes/PassphraseModal.jsx';
```

**New state variables:**
```jsx
const [showInitEncryptionModal, setShowInitEncryptionModal] = useState(false);
const [showPassphraseModal, setShowPassphraseModal] = useState(false);
const [initData, setInitData] = useState({});
const [passphraseCommandName, setPassphraseCommandName] = useState('');
const [passphraseData, setPassphraseData] = useState({});
```

**New handlers** (identical pattern to `Volumes.jsx`):
- `handleInitEncryption()` — open InitEncryptionModal with `{ slot: 1, keySize: 512 }`
- `handleInitEncryptionSubmit(data)` — call `VolumesService.initEncryption(payload)`
- `handleAddPassphrase()`, `handleRotatePassphrase()`, `handleDeletePassphrase()` — open PassphraseModal
- `handlePassphraseSubmit(data)` — route to `VolumesService.addPassphrase/rotatePassphrase/deletePassphrase`
- `handleAckEncryptionError()` — call `VolumesService.acknowledgeEncryptionError(payload)`

**Toolbar addition** (after the Delete button):
```jsx
<DropdownButton label="Encryption"
    disabled={!currUser.isAdmin || !selectedTPVs.length || selectedTPVs.some(v => !v.isEncrypted)}>
    <DropdownButtonItem label="Init Encryption" onClick={handleInitEncryption}
        disabled={selectedTPVs.some(v => !v.isEncrypted || v.encryption.isInitialized ||
            [consts.encryptionCommandStatuses.SENT, consts.encryptionCommandStatuses.PENDING_SEND]
                .includes(v.encryption.command?.status))}/>
    <DropdownButtonItem label="Add Passphrase" onClick={handleAddPassphrase}
        disabled={isPassphraseCmdDisabled}/>
    <DropdownButtonItem label="Rotate Passphrase" onClick={handleRotatePassphrase}
        disabled={isPassphraseCmdDisabled}/>
    <DropdownButtonItem label="Delete Passphrase" onClick={handleDeletePassphrase}
        disabled={isPassphraseCmdDisabled}/>
    <DropdownButtonItem label="Acknowledge Error" onClick={handleAckEncryptionError}
        disabled={selectedTPVs.some(v => !v.isEncrypted ||
            !v.encryption.command?.response?.error ||
            !!v.encryption.command?.response?.acknowledged)}/>
</DropdownButton>
```

**New column** — add `Encryption` status column showing init state / command status.

#### `CreateTPVModal.jsx`

Add encryption toggle (same pattern as `CreateEditVolumeModal.jsx`):

```jsx
<div className="form-group">
    <label>Encrypted</label>
    <input type="checkbox" {...register('isEncrypted')} disabled={isEdit} />
</div>
{isEncrypted && (
    <div className="form-group">
        <label>Encryption Header Size (MB)</label>
        <input type="number" {...register('encryption.headerSize')} min={1} max={100} defaultValue={16} />
    </div>
)}
```

### 5.11 Encryption Column in TPV Table

Add an `Encryption` column to the `ThinProvisioning.jsx` table:

```jsx
{
    name: 'Encryption',
    field: 'isEncrypted',
    className: 'fixed-size-column sx-column',
    rowClassName: 'fixed-size-column',
    value: tpvRow => {
        if (!tpvRow.isEncrypted) return '—';
        if (!tpvRow.encryption?.isInitialized) return <label className="label bg-yellow">Init Required</label>;
        const cmdStatus = tpvRow.encryption?.command?.status;
        if (cmdStatus === 'sent' || cmdStatus === 'pendingSend')
            return <label className="label bg-blue">In Progress</label>;
        if (tpvRow.encryption?.command?.response?.error && !tpvRow.encryption?.command?.response?.acknowledged)
            return <label className="label bg-red">Error</label>;
        return <label className="label bg-green">Encrypted</label>;
    },
},
```

### 5.12 CLI Changes (`nvmesh-infra/xlro/tools/cli`)

The CLI is declarative: TPV commands are generated from `xlro/core/entities/rest.yaml` by the auto-generation framework in `rest_click.py`/`rest_custom.py`. Encryption on regular volumes (`Volume` entity, API version 8, `rest.yaml:787`) already exposes `isEncrypted`/`encryption` in `create.params` plus `initEncryption`, `addPassphrase`, `deletePassphrase`, `rotatePassphrase` ops. The TPV entity (`rest.yaml:1265`) does **not** inherit these and must be extended explicitly.

#### 5.12.1 Create-time parameters

Add `isEncrypted` and `encryption` to the `TPV.ops.create.params` list so `nvmesh tpv create --is-encrypted --encryption-header-size 16 …` is accepted:

```yaml
TPV:
  …
  ops:
    create:
      params:
        - capacity
        - tpvConfig
        - isEncrypted           # ← add
        - encryption            # ← add (nested: encryption.headerSize)
```

The Pydantic/SdkObject expansion in `rest_click.RestGroup` will turn `encryption` into `--encryption-header-size`.

#### 5.12.2 Encryption operations on the TPV entity

`Volume.ops.{initEncryption,addPassphrase,deletePassphrase,rotatePassphrase}` already exist and target `POST /volumes/<op>`, which — after the §5.9 change — handles TPVs identically to regular volumes. Two viable options:

- **Option A (minimal, recommended):** rely on `nvmesh volume initEncryption <tpv_name> …` etc. Volume ops don't filter by `volumeClass`, and management routes by `_id`/`uuid`. Pro: zero CLI work. Con: poor discoverability — a TPV-focused user won't find the commands under `nvmesh tpv`.

- **Option B (preferred for UX):** duplicate the four encryption ops under `TPV.ops` using the same payload templates as `Volume.ops`. Since the route is shared (`route: volumes` is inherited at the entity level), the payloads and wait logic are copy-paste from `Volume.ops.initEncryption` etc. Adds ~60 lines to `rest.yaml`, no code changes to `rest_custom.py`.

This plan assumes **Option B** for symmetry with the `Volumes.jsx`/`ThinProvisioning.jsx` UI split.

#### 5.12.3 Display fields

Extend `TPV.display` (`rest.yaml:1273`) with `isEncrypted` and `encryption` so `nvmesh tpv show` surfaces init state and command status (consumed from `encryption.isInitialized` and `encryption.command.status` in the management DB).

#### 5.12.4 Golden files

- `current.api` — append the new TPV sub-commands and their parameters (auto-regenerated by running the CLI snapshot tool after `rest.yaml` changes).
- `current.display` — append the new TPV display fields.

Both files live under `nvmesh-infra/xlro/tools/cli/` and are verified in CI.

#### 5.12.5 SDK

No changes. The `Volume`/`TPV` SdkObject classes under `xlro/core/entities/` already inherit `isEncrypted`, `encryption`, and the passphrase fields from the common volume schema. `TPV._get_filter` continues to inject `volumeClass: 'TPV'` and is unaffected.

### 5.13 CSI Driver Changes (`nvmesh-csi-driver`)

The CSI driver's `_do_create_tpv()` (`driver/controller_service.py:245`) branches early from `do_create_volume()` at line 114 and therefore **never reaches** the `isEncrypted` handling at line 152. Two distinct gaps must be closed.

#### 5.13.1 StorageClass parameter parsing

The regular-volume path parses `encryption: dmcrypt` and `encryption.headerSize` in `_handle_volume_req_parameters()` (lines 476–484) before constructing the `NVMeshVolume`. This is not invoked for TPVs. Add equivalent parsing in `_do_create_tpv()` after the existing `_parse_tpv_params()` call:

```python
if parameters.get("encryption") == "dmcrypt":
    tpv.isEncrypted = True
    if "encryption.headerSize" in parameters:
        tpv.encryption = {"headerSize": int(parameters["encryption.headerSize"])}
```

Unknown `encryption` values should log a warning and continue unencrypted, matching line 481.

#### 5.13.2 Post-create init-encryption call

Immediately after `NVMeshMgmtAPI.create_tpv()` succeeds (line 280) and `volume_uuid` is resolved, invoke the same init-encryption flow used by regular volumes, with TPV-aware rollback:

```python
if getattr(tpv, "isEncrypted", None) and volume_api.apiVersion >= Consts.ApiVersion.API_VERSION_9:
    try:
        self.init_encrypted_volume(secrets, zone, volume_context, log)
    except Exception as ex:
        log.error(f"Failed to initialize encryption for TPV {nvmesh_vol_name}; rolling back. Error: {ex}")
        NVMeshMgmtAPI.delete_tpv(volume_api, nvmesh_vol_name, zone, log, …)
        raise
```

`volume_context` must be constructed **before** this call (not at line 294), because `init_encrypted_volume` reads `volume_name` and `volume_uuid` from it. The existing `init_encrypted_volume()` body (lines 173–186) works unchanged: it polls management for `isInitialized`-required state, calls `POST /volumes/initEncryption`, then waits for `isReady`. Management's TOMA selection (`chooseTOMAForTPVEncryption`, §5.6) transparently picks a CDV-attached TOMA.

#### 5.13.3 Rollback path

`NVMeshMgmtAPI.delete_volume` (line 159) uses the regular `/volumes` delete route and won't work on a TPV — TPV delete must go through `POST /volumes/tpv/delete` (see §3.x). Ensure `nvmesh_mgmt_api.py` exposes a `delete_tpv(...)` helper that posts to the correct route, and call it from the rollback branch above. The existing `NVMeshMgmtAPI.create_tpv` already does the symmetric mapping.

#### 5.13.4 Secrets plumbing

`init_encrypted_volume` pulls the passphrase from the CSI `secrets` field via `secret_manager.get_passphrase()`. The `secrets` object is already captured at the top of `do_create_volume` (line 107) but is **not** passed to `_do_create_tpv()` today. Extend the signature of `_do_create_tpv()` to accept `secrets` and thread it through from line 115.

#### 5.13.5 StorageClass documentation

Update `deploy/kubernetes/helm/.../templates/storageclass.yaml` with an example encrypted-TPV `StorageClass`:

```yaml
parameters:
  volumeClass: TPV
  cdvNameRegex: "^pool-gold-"
  encryption: dmcrypt
  encryption.headerSize: "16"
  csi.storage.k8s.io/node-publish-secret-name: nvmesh-tpv-secret
  csi.storage.k8s.io/node-publish-secret-namespace: default
```

#### 5.13.6 Node-side LUKS open

`node_service.py` gates LUKS open on `is_encrypted_volume = "encryption" in volume.metadata` (line 755). Because `volume_context` in `_do_create_tpv()` already copies `reqDict["parameters"]` (line 305), the `encryption: dmcrypt` parameter propagates through to node-stage/publish unchanged — **no node-side code changes required**.

#### 5.13.7 Integration tests

Add to `test/integration/`:

- `test_encrypted_tpv_create.py` — StorageClass with `volumeClass: TPV` + `encryption: dmcrypt` → PVC bound, TPV `isReady: true`, LUKS header present on first CDV extent.
- `test_encrypted_tpv_rollback.py` — force `init_encrypted_volume` failure (invalid secret) → CSI returns error, TPV is deleted via `/volumes/tpv/delete`, no orphan CDV extent.
- `test_encrypted_tpv_attach.py` — attach encrypted TPV → `/dev/mapper/…` exists on the node, file I/O succeeds, detach cleans up the dm-crypt device.

### 5.14 Implementation Steps

#### Phase 1 — Management Backend (can be developed and tested independently)

**Step 1: TPV creation with encryption support** (`modules/volume.js`)
- Extend `createTPV()` to accept `isEncrypted` and `encryption.headerSize`
- Set `isReady: false` and `action: INIT_ENCRYPTION_REQUIRED` when encrypted
- Add `encryption: { headerSize, isInitialized: false }` sub-document
- `prepareCDVForCreate()`: strip incoming `isEncrypted`/`encryption` on CDVs

**Step 2: TOMA selection + attach/detach orchestration** (`modules/volumeEncryption.js`)
- Implement `chooseTOMAForTPVEncryption(volume, callback)` — first-pRAID candidates, prefer allocator TOMA
- Add branch in `chooseTOMAForEncryption()` for `volumeClass === 'TPV'`
- Add `attachTPVToTOMAForEncryption` / `detachTPVFromTOMAForEncryption` helpers (thin wrappers around `clientModule.attachTPV`/`detachTPV` with `preempt: true`)
- Insert attach step in `runEncryptionCommand()` after TOMA selection
- Stamp `encryption.command.tpvAutoAttachedTOMA` in `setEncryptionCommand()` for TPVs
- Drive detach in `handleCommandResponse()` after status update
- Add `cleanupTPVAutoAttachesAfterStartup()` and invoke from `sanityAndRecover.js`

**Step 3: Kafka envelope** — **no changes**. The base class and subclasses are reused verbatim.

**Step 4: Integration test** (backend only)
- Create an encrypted TPV via `POST /volumes/save`
- Verify DB record: `isReady: false`, `isEncrypted: true`, `encryption.isInitialized: false`, `action: INIT_ENCRYPTION_REQUIRED`
- Call `POST /volumes/initEncryption` with the TPV's UUID
- Verify the TPV becomes attached to the chosen TOMA (`tpvConfig.exclusiveClient === toma._id`) and `encryption.command.tpvAutoAttachedTOMA` is stamped
- Simulate TOMA response: verify DB transitions to `isReady: true`, `encryption.isInitialized: true`, TPV is detached from TOMA, `tpvAutoAttachedTOMA` cleared

#### Phase 2 — UI

**Step 5: CreateTPVModal encryption fields** (`CreateTPVModal.jsx`)
- Add `isEncrypted` checkbox (disabled on edit)
- Add `encryption.headerSize` input (visible when encrypted, default 16)
- Pass fields through in `onFormSubmit()`

**Step 6: ThinProvisioning.jsx encryption toolbar** (`ThinProvisioning.jsx`)
- Import and render `InitEncryptionModal`, `PassphraseModal`
- Add all encryption state variables and handlers (copy pattern from `Volumes.jsx`)
- Add `<DropdownButton label="Encryption">` with all five items
- Add `isPassphraseCmdDisabled` computed variable
- Add Encryption status column to table

**Step 7: UI integration test**
- Create encrypted TPV from UI → verify "Init Required" status
- Init Encryption from toolbar → verify modal, submission, status change
- Passphrase operations → verify modal variations and success/error alerts
- Acknowledge Error → verify error state clears

#### Phase 3 — TOMA

**Step 8: TOMA branch in `start_encrypt_action`** (`nvmeibt_kafka.c`)
- Detect TPV via `(vol->from_config.n_chunks == 0) && !vol->from_config.is_cdv`
- Retarget the `cryptsetup` command to `/dev/nvmesh-tpv/<vol->from_config.client_blkdev_name>`
- Call `nvmeibt_start_encrypt_for_tpv(vol, encrypt_params)` instead of `nvmeibt_attach_vol_for_encryption`
- No changes to `encrypt_cmd_t` or `parse_CMD`
- Use a local name other than `dev_dir` for the device-directory local — it collides with the `dev_dir` macro defined in `nvmeibt_local_disk.h:316` (`#define dev_dir TOMA_ROOT_DIR "dev/"`). The implementation uses `enc_dev_dir` / `enc_dev_name`.

**Step 9: TPV exec entry point** (`nvmeibt_recovery.c`, `nvmeibt_recovery.h`)
- Add `nvmeibt_start_encrypt_for_tpv(vol, encrypt_params)` — skips `create_shadow_vol`/WQ attach, points `exec_ctx->blkdev` at the TPV, sets a TPV-specific exec-done callback, runs `nvmeibt_run_exec_on_blkdev`
- Add `tpv_encrypt_after_exec_cb(exec_ctx)` — sends Kafka response, frees buffers and encrypt_params; does NOT free the TPV (management still owns it until detach)
- Export `nvmeibt_start_encrypt_for_tpv` in `nvmeibt_recovery.h`

**Step 10: TOMA integration test**
- Send a mock `initEncryption` Kafka message for a TPV that management has pre-attached to this TOMA
- Verify `cryptsetup luksFormat` runs against `/dev/nvmesh-tpv/<name>` (no dm-linear, no shadow volume)
- Verify the TOMA's client kernel module allocates CDV extents on demand during the LUKS header write
- Verify the response Kafka message carries the correct result code

#### Phase 4 — CLI

**Step 11: TPV entity encryption params and ops** (`nvmesh-infra/xlro/core/entities/rest.yaml`)
- Add `isEncrypted`, `encryption` to `TPV.ops.create.params`
- Duplicate `initEncryption`, `addPassphrase`, `deletePassphrase`, `rotatePassphrase` ops from `Volume.ops` into `TPV.ops` (payloads are identical — the route `/volumes/<op>` is shared)
- Extend `TPV.display` with `isEncrypted` and `encryption`
- Regenerate `current.api` and `current.display` golden files

**Step 12: CLI smoke test**
- `nvmesh tpv create --is-encrypted --encryption-header-size 16 --cdv <cdv> --capacity …` → verify payload includes `isEncrypted: true`
- `nvmesh tpv initEncryption <tpv> --passphrase … --slot 1` → verify `/volumes/initEncryption` POST
- `nvmesh tpv show <tpv>` → verify encryption status column appears

#### Phase 5 — CSI Driver

**Step 13: StorageClass parameter parsing** (`driver/controller_service.py`)
- Thread `secrets` from `do_create_volume` into `_do_create_tpv`
- Parse `encryption: dmcrypt` and `encryption.headerSize` after `_parse_tpv_params`; set `tpv.isEncrypted` and `tpv.encryption`

**Step 14: Post-create init-encryption call** (`driver/controller_service.py`)
- Build `volume_context` before the encryption branch (move up from line 294)
- After `create_tpv()` succeeds, call `init_encrypted_volume(secrets, zone, volume_context, log)` if `tpv.isEncrypted` and `apiVersion >= API_VERSION_9`
- On failure, call a new `NVMeshMgmtAPI.delete_tpv()` helper (not `delete_volume`) and re-raise

**Step 15: Management API helper** (`driver/nvmesh_mgmt_api.py`)
- Add `delete_tpv(volume_api, name, zone, log, backoff)` that POSTs `/volumes/tpv/delete`; mirror the retry/backoff shape of `delete_volume`

**Step 16: StorageClass example and docs** (`deploy/kubernetes/helm/.../templates/storageclass.yaml`)
- Add an encrypted-TPV `StorageClass` example with `encryption: dmcrypt` and secret references

**Step 17: CSI integration tests** (`test/integration/`)
- `test_encrypted_tpv_create.py`, `test_encrypted_tpv_rollback.py`, `test_encrypted_tpv_attach.py` (scope in §5.13.7)

#### Phase 6 — End-to-end

**Step 18: Full flow test**
- Create CDV → create encrypted TPV (via UI / CLI / CSI, one scenario per interface) → Init Encryption → verify LUKS header on CDV extent
- Add/Rotate/Delete passphrase → verify each command lifecycle
- Error scenarios: TOMA down, CDV full, concurrent encryption attempts, CSI rollback on passphrase fetch failure

### 5.15 Risks and Open Questions

1. **TOMA must be registered as a client**: Attaching the TPV on the TOMA for encryption uses `clientModule.attachTPV`, which requires a `client` document with `_id` = the TOMA's hostname. NVMesh clusters today run the client kernel module on TOMA nodes as a matter of course, so this record exists. If an operator deploys a TOMA-only node without the client module, encryption commands on TPVs backed by that node's CDV pRAID will fail with a diagnostic pointing at the missing client registration. Mitigation: document the prerequisite; `chooseTOMAForTPVEncryption` could also de-prioritise TOMAs without client registrations.

2. **Preempt semantics**: The attach uses `preempt: true`, which will fence a real live client holding the TPV. No additional interlock prevents running encryption while a user I/O load is active. For `initEncryption` on a freshly created TPV this is fine (no user data yet). For later passphrase ops (`addPassphrase` / `rotatePassphrase` / `deletePassphrase`) the operator is responsible for knowing the TPV is quiescent. A future hardening step could reject these ops if `tpvConfig.exclusiveClient` is a non-TOMA client.

3. **Crash recovery**: If management dies between receiving the Kafka response and issuing the detach, the TPV is left attached to the TOMA and its real client cannot reattach. `cleanupTPVAutoAttachesAfterStartup` scans for this state on startup and drives the missing detach. The same function also backstops the rarer case where the response write to DB succeeded but the detach call itself crashed.

4. **CDV full during LUKS header write**: `cryptsetup luksFormat` writes ~16 MB of LUKS metadata at offset 0. This triggers CDV extent allocation through the client's normal first-write path; on `CDV_ALLOC_CDV_FULL` the client I/O fails, cryptsetup exits non-zero, and the existing TOMA encryption response builder reports `CMD_ERR`. No new error path.

5. **Client-side LUKS open at attach**: After encryption init the TPV is reattached to a real client; the client's management agent runs `cryptsetup open` against `/dev/nvmesh-tpv/<tpv_name>` — the same flow as regular encrypted volumes, just on a different device prefix. Verify the client agent's cryptsetup invocation is path-agnostic (`/dev/nvmesh/` vs `/dev/nvmesh-tpv/`).

6. **Passphrase operations after TPV extend**: Extending a TPV changes only `virtualSizeGB`; the LUKS header stays at offset 0 of the TPV's address space and is bound to whichever CDV extent backs that offset. Passphrase ops target only the LUKS header and are unaffected by subsequent extent allocations.

7. **~~dm-linear naming collisions, CDV extent 0 conflict~~** — removed along with the dm-linear and pre-allocation paths they referenced.

---

## Part 10 — CDV and TPV Allocation Statistics in Management UI

### 10.1 Overview

The management UI currently shows no runtime allocation statistics for CDVs or TPVs. This section designs the end-to-end flow: what stats exist, how they travel from kernel to management, how they are stored, and how the UI presents them.

**CDV screen — new columns:**

| Column | Meaning | Source |
|--------|---------|--------|
| Allocated Extents | CDV data extents currently assigned to TPVs | TOMA |
| Free Extents | Data extents available for new allocation | TOMA (derived: `total − allocated`) |
| Max Additional | Maximum additional data extents if CDV were expanded | Computed from `cdvConfig` |

**TPV screen — new columns:**

| Column | Meaning | Source |
|--------|---------|--------|
| CDV Extents | Number of CDV data extents held by this TPV | TOMA |
| TPV Extents In Use | TPV extents with data written (mapped in xarray) | Client |

### 10.2 Architecture Decisions

**Decision #26 — Push via Kafka events, not polling.**
Management does not poll TOMA or clients for statistics. TOMA and client push stats via Kafka, following the existing NVMesh pattern where all kernel → management communication is event-driven. Two new Kafka message types are added.

**Decision #27 — Stats stored in volume documents, no new collections.**
Runtime stats are stored as a `runtimeStats` sub-object on the existing volume document in MongoDB. This keeps stats co-located with the volume they describe and avoids new collections. The `runtimeStats` sub-object is always treated as stale-able (best-effort, not transactional).

**Decision #28 — TOMA is authority for CDV-level stats; client is authority for TPV-extent-level stats.**
TOMA owns the CDV allocator and knows how many CDV extents are allocated, free, and assigned to each TPV. The client owns the TPV allocator and knows how many TPV extents within its CDV extents are in use. Each authority pushes its own stats — no cross-component queries.

### 10.3 Data Model — MongoDB Extensions

**CDV document — new `runtimeStats` sub-object:**

```javascript
{
    // existing fields: _id, uuid, volumeClass: 'CDV', capacity, cdvConfig, tpvCount, ...
    runtimeStats: {
        allocatedExtents: Number,     // CDV data extents currently allocated to TPVs
        totalDataExtents: Number,     // total CDV data extents (physical capacity / extent size)
        maxAddressableExtents: Number, // allocator area addressing limit
        lastUpdated: Date,            // timestamp of last TOMA report
    }
}
```

Derived values (computed by UI, not stored):
- `freeExtents = totalDataExtents − allocatedExtents`
- `maxAdditional = maxAddressableExtents − totalDataExtents`

**TPV document — new `runtimeStats` sub-object:**

```javascript
{
    // existing fields: _id, uuid, volumeClass: 'TPV', tpvConfig, ...
    runtimeStats: {
        cdvExtents: Number,        // CDV data extents held by this TPV (from TOMA)
        tpvExtentsInUse: Number,   // mapped TPV extents with data (from client)
        tpvExtentsTotal: Number,   // virtual address space in TPV extents (from client)
        lastUpdated: Date,
    }
}
```

### 10.4 CDV Stats: TOMA → Management

#### Data source

TOMA's in-memory `nvmeibt_cdv_alloc` struct provides:
- `n_allocated` — CDV extents currently allocated
- `total_data_extents` — total CDV data capacity in extents
- `extents` xdlist — per-extent entries with `tpv_uuid`, from which per-TPV CDV extent counts are derived

The `maxAddressableExtents` is computed from the allocator area geometry: one 4 KiB header block + one 4 KiB record per extent slot = `(allocatorSizeGiB × 1 GiB / 4 KiB) − 1`.

#### New Kafka message: `TOMAToManagement_TP.cdvAllocatorStats`

Published by TOMA after every `CDV_ALLOC_EXTENT` and `CDV_FREE_EXTENT` operation. Uses the existing Kafka dedup mechanism with `cdv_uuid` as the unique key, so rapid alloc/free sequences coalesce into a single message per CDV.

```json
{
    "messageType": "cdvAllocatorStats",
    "cdvUUID": "<uuid>",
    "allocatedExtents": 42,
    "totalDataExtents": 100,
    "maxAddressableExtents": 262143,
    "perTPV": [
        { "tpvUUID": "<uuid-1>", "cdvExtents": 12 },
        { "tpvUUID": "<uuid-2>", "cdvExtents": 30 }
    ]
}
```

The `perTPV` array is built by iterating the `extents` xdlist and counting entries per `tpv_uuid`. This runs on TOMA's single main thread, so no additional locking is needed.

Trigger frequency is appropriate because:
- CDV extent allocations are infrequent (one alloc per `cdvExtentSizeMiB` of new writes — 64 MB minimum)
- The dedup mechanism further coalesces multiple operations into one message

#### TOMA implementation (`nvmeibt_cdv_alloc.c`)

New function `nvmeibt_cdv_alloc_publish_stats(cdv_uuid)`:

```c
static void nvmeibt_cdv_alloc_publish_stats(const char *cdv_uuid)
{
    struct nvmeibt_cdv_alloc *alloc;
    KAFKA_PRODUCER_MSG_HEADER_VAR(hdr);

    alloc = cdv_alloc_lookup(cdv_uuid);
    if (!alloc || !alloc->ondisk_loaded)
        return;

    /* Build per-TPV breakdown by scanning extent list */
    /* ... count extents per tpv_uuid ... */

    /* Build JSON payload */
    /* ... nvmeibt_Str_sprintf with allocatedExtents, totalDataExtents,
           maxAddressableExtents, perTPV array ... */

    nvmeibt_kafka_outgoing_msgs_queue_add(
        cdv_uuid,                                   /* unique_key: coalesces per CDV */
        json_buf,
        NVMEIBT_KAFKA_OUTGOING_MSGS_PRIORITY_LOW);  /* low priority: stats, not alerts */
}
```

Called at the end of `handle_cdv_alloc_extent()` and `handle_cdv_free_extent()`, after the existing capacity-warning check.

#### Management handler

**`consts.js`** — add message type:
```javascript
TOMAToManagement_TP: {
    cdvCapacityWarning: 'cdvCapacityWarning',
    cdvAllocatorStats: 'cdvAllocatorStats',    // NEW
}
```

**`kafkaRouter.js`** — add routing (next to existing `cdvCapacityWarning` case):
```javascript
case consts.kafkaMessageTypes.TOMAToManagement_TP.cdvAllocatorStats:
    volumeModule.handleCDVAllocatorStats(message, callback);
    break;
```

**`modules/volume.js`** — new handler:
```javascript
scope.handleCDVAllocatorStats = (message, callback) => {
    const db = app.get('db');
    const volumeCollection = db.collection('volume');
    const now = new Date();

    // Update CDV document with aggregate stats
    volumeCollection.updateOne(
        { uuid: message.cdvUUID, volumeClass: consts.volumeClass.CDV },
        { $set: {
            'runtimeStats.allocatedExtents': message.allocatedExtents,
            'runtimeStats.totalDataExtents': message.totalDataExtents,
            'runtimeStats.maxAddressableExtents': message.maxAddressableExtents,
            'runtimeStats.lastUpdated': now,
        }},
        () => {}
    );

    // Update each TPV's CDV extent count
    for (const entry of (message.perTPV || [])) {
        volumeCollection.updateOne(
            { uuid: entry.tpvUUID, volumeClass: consts.volumeClass.TPV },
            { $set: {
                'runtimeStats.cdvExtents': entry.cdvExtents,
                'runtimeStats.lastUpdated': now,
            }},
            () => {}
        );
    }

    callback();
};
```

### 10.5 TPV Stats: Client → Management

#### Data source

The client kernel driver's `nvmeibc_tpv_allocator` struct provides:
- `cdv_extents_count` — CDV extents held by this TPV
- `free_tpv_extent_count` — free TPV extent slots within allocated CDV extents
- `virtual_extents_total` — total virtual extents in the TPV
- TPV extents in use = `(cdv_extents_count × n_slots) − free_tpv_extent_count`, where `n_slots = cdv_extent_size_mib × 1024 / tpv_extent_size_kb`

These are exposed via `/proc/nvmeibc/tpv/<name>/allocator` (already implemented in `nvmeibc_tpv_proc.c`).

#### Reporting mechanism

The management agent (userspace process on each client node) already reads `/proc/nvmeibc/` for volume health monitoring and sends `ClientToManagement.keepalive` messages to management via Kafka on a configurable interval (default 10 seconds).

**Approach:** The management agent reads `/proc/nvmeibc/tpv/*/allocator` for each attached TPV and emits a new Kafka message type `ClientToManagement.tpvStats` on the same keepalive cycle.

#### New Kafka message: `ClientToManagement.tpvStats`

```json
{
    "messageType": "tpvStats",
    "clientID": "<client-hostname>",
    "tpvs": [
        {
            "tpvUUID": "<uuid>",
            "cdvExtents": 3,
            "tpvExtentsInUse": 1842,
            "tpvExtentsTotal": 262144
        }
    ]
}
```

One message per client per keepalive cycle, containing stats for all attached TPVs on that client.

#### Management handler

**`consts.js`** — add message type:
```javascript
ClientToManagement: {
    keepalive: 'keepalive',
    updateAttachmentStatus: 'updateAttachmentStatus',
    tpvStats: 'tpvStats',  // NEW
    // ...
}
```

**`kafkaRouter.js`** — add routing in `routeClientMessage()`:
```javascript
case msgType.tpvStats:
    volumeModule.handleTPVStats(message, callback);
    break;
```

**`modules/volume.js`** — new handler:
```javascript
scope.handleTPVStats = (message, callback) => {
    const db = app.get('db');
    const volumeCollection = db.collection('volume');
    const now = new Date();

    for (const entry of (message.payload?.tpvs || [])) {
        volumeCollection.updateOne(
            { uuid: entry.tpvUUID, volumeClass: consts.volumeClass.TPV },
            { $set: {
                'runtimeStats.tpvExtentsInUse': entry.tpvExtentsInUse,
                'runtimeStats.tpvExtentsTotal': entry.tpvExtentsTotal,
                'runtimeStats.lastUpdated': now,
            }},
            () => {}
        );
    }

    callback();
};
```

Note: `runtimeStats.cdvExtents` on the TPV document is written by the CDV stats handler (§10.4), not here. Both paths write `lastUpdated`, and the latest writer wins — this is acceptable since both timestamps will be close in time and the field is informational.

### 10.6 "Max Additional Extents" Computation

**Max addressable extents** is the upper limit on how many CDV data extents the allocator area can track, regardless of current CDV physical capacity. It is determined by the on-disk allocator format (one 4 KiB record per extent, plus a 4 KiB header):

$$\text{maxAddressable} = \frac{\text{allocatorSizeGiB} \times 1\,\text{GiB}}{4\,\text{KiB}} - 1$$

For the default `allocatorSizeGiB = 1`: 262,143 extent slots.

**Total data extents** is how many CDV data extents actually exist given current CDV capacity:

$$\text{totalDataExtents} = \frac{\text{CDV capacity} - \text{allocatorSizeGiB} \times 1\,\text{GiB}}{\text{cdvExtentSizeMiB} \times 1\,\text{MiB}}$$

**Max additional** is the gap — how many more data extents could exist if the CDV volume were expanded:

$$\text{maxAdditional} = \text{maxAddressable} - \text{totalDataExtents}$$

The design intent is to include both `maxAddressable` and `totalDataExtents` in the TOMA stats message (§10.4) and store both in `runtimeStats`, so the UI can compute `maxAdditional` as a simple subtraction.

**Shipped status:** the TOMA→management stats payload and `handleCDVAllocatorStats()` currently carry only `allocatedExtents` and `totalDataExtents` — `maxAddressableExtents` is **not** yet included, so the "Max Additional" column described in §10.7 cannot be populated end-to-end. The CDV page ships with an **"Over-Provision"** column (showing `overprovisionRatio = virtual-capacity-demand / totalDataExtents`) in its place; "Max Additional" remains planned. Closing this gap requires adding `maxAddressableExtents` to the Kafka stats message and to the Mongo `runtimeStats` write.

If `maxAdditional` were 0, the CDV would have reached its allocator addressing limit and could not benefit from expansion without increasing `allocatorSizeGiB` (which requires CDV recreation).

### 10.7 UI Changes

#### CDV screen — `pages/thinProvisioning/CDVs.jsx`

Shipped columns (visible only when the CDV filter is active):

| Column | Source | Notes |
|---|---|---|
| **Allocated Extents** | `runtimeStats.allocatedExtents` | Direct from TOMA stats. |
| **Free Extents** | `runtimeStats.totalDataExtents − runtimeStats.allocatedExtents` | Computed in the cell renderer. |
| **Over-Provision** | `overprovisionRatio` (server-side computed) | Virtual-capacity demand over physical data-extent capacity. |

"Max Additional" (design §10.6) is **not yet shipped** — it waits on `maxAddressableExtents` being added to the Kafka stats message. "Over-Provision" currently occupies the third column slot.

All three columns show `—` until TOMA has processed the first alloc/free for the CDV (`runtimeStats` absent). A newly created CDV with no TPVs will show `—` until the first TPV allocates an extent.

#### TPV screen — `ThinProvisioning.jsx`

Add two new columns after "Virtual Size":

```jsx
{
    name: 'CDV Extents',
    field: 'runtimeStats.cdvExtents',
    filterable: false,
    className: 'fixed-size-column sx-column',
    rowClassName: 'fixed-size-column',
    value: tpvRow => tpvRow.runtimeStats?.cdvExtents != null
        ? tpvRow.runtimeStats.cdvExtents
        : '—',
},
{
    name: 'TPV Extents In Use',
    field: 'runtimeStats.tpvExtentsInUse',
    filterable: false,
    className: 'fixed-size-column sx-column',
    rowClassName: 'fixed-size-column',
    value: tpvRow => {
        const stats = tpvRow.runtimeStats;
        if (!stats || stats.tpvExtentsInUse == null) return '—';
        return stats.tpvExtentsTotal
            ? `${stats.tpvExtentsInUse} / ${stats.tpvExtentsTotal}`
            : stats.tpvExtentsInUse;
    },
},
```

The "TPV Extents In Use" column shows `inUse / total` format (e.g., `1842 / 262144`) so the user can gauge how full the TPV's virtual address space is.

#### Staleness indicator

Both screens show `—` when `runtimeStats` is absent (TPV not yet attached, or CDV has never had an allocation). No special staleness UI is needed — the 3-second table auto-reload (already in `ThinProvisioning.jsx` and `Volumes.jsx`) will pick up updates within seconds of TOMA reporting.

### 10.8 Implementation Steps

**Step 1: TOMA Kafka publisher** (`toma/nvmeibt_cdv_alloc.c`)
- Add `nvmeibt_cdv_alloc_publish_stats()` function
- Call from `handle_cdv_alloc_extent()` and `handle_cdv_free_extent()` after existing capacity-warning logic
- Build JSON with `allocatedExtents`, `totalDataExtents`, `maxAddressableExtents`, `perTPV` array
- Publish via `nvmeibt_kafka_outgoing_msgs_queue_add()` with low priority and CDV UUID dedup key
- Add `cdvAllocatorStats` to TOMA's Kafka message type enum

**Step 2: Management handler** (`modules/volume.js`, `kafkaRouter.js`, `consts.js`)
- Add `cdvAllocatorStats` to `consts.kafkaMessageTypes.TOMAToManagement_TP`
- Add routing case in `kafkaRouter.js` `routeTOMAMessage()`
- Implement `handleCDVAllocatorStats()` — update CDV and TPV volume documents

**Step 3: Management agent TPV stats** (management agent codebase — outside this repo)
- Add `/proc/nvmeibc/tpv/*/allocator` parsing to the agent's keepalive cycle
- Emit `ClientToManagement.tpvStats` Kafka message per keepalive interval
- Add `tpvStats` to `consts.kafkaMessageTypes.ClientToManagement`
- Add routing case in `kafkaRouter.js` `routeClientMessage()`
- Implement `handleTPVStats()` — update TPV volume documents

**Step 4: UI columns** (`Volumes.jsx`, `ThinProvisioning.jsx`)
- Add Allocated / Free / Max Additional columns to CDV table
- Add CDV Extents / TPV Extents In Use columns to TPV table
- Columns show `—` when `runtimeStats` is absent

---

## Part 11 — TPV Hot Upgrade (NDU)

### 11.1 Problem Statement

During a Non-Disruptive Upgrade (NDU), the `nvmeibc` kernel module is unloaded and a
new version is loaded in its place.  Regular NVMesh volumes survive this transition
transparently — the ATOM module (`nvmeiba`) owns their gendisk and request queue,
buffers incoming BIOs while `nvmeibc` is absent, and replays them when the new
instance adopts the orphaned volumes.

TPVs do **not** benefit from this mechanism today.  Their gendisk and queue are
allocated directly by `nvmeibc_tpv_blkdev_register()` (via `blk_alloc_disk()` /
`add_disk()`), and `nvmeibc_tpv_fops` has `.owner = THIS_MODULE` (nvmeibc).
During NDU the current code path is:

1. `__detach_all_volumes_of_inst_work()` calls `nvmeibc_tpv_detach_all_for_inst()` unconditionally — even when `w->is_upgrade` is true.
2. `nvmeibc_tpv_detach()` calls `del_gendisk()`, `put_disk()`, `kfree(tpv)`.
3. The TPV block device disappears from `/dev/`.  Any application with an open file descriptor gets EIO.
4. After the new module loads and management re-sends the attach command, a fresh TPV is created and the block device reappears — but with a different dev_t, breaking mounts and LVM.

Goal: Make TPVs survive NDU with the same guarantees as regular volumes — the block
device stays in `/dev/`, opens are preserved, BIOs are buffered (not failed), and the
new module instance resumes IO transparently.

### 11.2 Design Overview

Integrate TPV gendisk/queue lifecycle with ATOM, mirroring the pattern used by
regular volumes in `nvmeibc_block_api_os.c`:

1. **Embed** `nvmeiba_atom_os_api` in `struct nvmeibc_tpv`.
2. **Allocate** the gendisk and queue through ATOM (so `.owner` is `nvmeiba` and the
   disk survives module unload).
3. On NDU **abandon**: flush persistent state, cancel workers, orphan the atom via
   `nvmeiba_os_api_orphan_abandon()`.  ATOM switches `submit_bio` to
   `nvmeiba_b_req_push` and buffers new BIOs.
4. On NDU **adopt**: the new `nvmeibc` instance discovers orphaned TPV atoms in ATOM's
   registry, reconnects them to the (also-adopted) CDV, re-initialises work structs
   with new function pointers, atomically restores the TPV `submit_bio`, and drains
   the buffered BIO list.

### 11.3 Struct Changes

#### `struct nvmeibc_tpv` — embed ATOM atom

```c
 struct nvmeibc_tpv {
+    struct nvmeiba_atom_os_api    atom;        /* MUST be first for container_of */
     struct nvmeibc_volume        *cdv_vol;
     struct nvmeibc_tpv_allocator  allocator;
-    struct gendisk               *disk;
-    struct request_queue         *queue;
     /* ... rest unchanged ... */
+
+    /* Per-TPV copy of fops, kept in kzalloc'd memory so it survives NDU.
+     * .owner = nvmeiba module, .open/.close = nvmeiba handlers,
+     * .submit_bio = nvmeibc_tpv_submit_bio_wrapper (set at adopt/attach). */
+    struct block_device_operations tpv_live_fops;
 };
```

After this change `tpv->disk` and `tpv->queue` are accessed via `tpv->atom.disk` and
`tpv->atom.queue` respectively.  A pair of accessor macros keeps call sites clean:

```c
#define tpv_disk(tpv)   ((tpv)->atom.disk)
#define tpv_queue(tpv)  ((tpv)->atom.queue)
```

#### `enum nvmeibc_tpv_state` — add orphan state

```c
 enum nvmeibc_tpv_state {
     TPV_ATTACHING  = 0,
     TPV_ATTACHED   = 1,
     TPV_DETACHING  = 2,
+    TPV_ORPHAN     = 3,   /* NDU: nvmeibc gone, ATOM buffering BIOs */
 };
```

### 11.4 Registration Through ATOM

Replace `nvmeibc_tpv_blkdev_register()` internals.  Instead of calling
`blk_alloc_disk()` + `add_disk()` directly, delegate to ATOM:

1. Call `nvmeiba_os_api_constructor(&tpv->atom, tpv_name, false)`.
   ATOM allocates the gendisk and queue with `.owner = nvmeiba_module`.
2. Populate `tpv_live_fops` by copying ATOM's `default_fops` (obtained from the
   `nvmeiba_to_c_handover` returned by `nvmeiba_os_do_on_nvmeibc_up()`), then
   override `.submit_bio = nvmeibc_tpv_submit_bio_wrapper`.
3. Set `tpv->atom.disk->fops = &tpv->tpv_live_fops`.
4. On older kernels (`KS_REQUEST_QUEUE_HAS_REQUEST_FN`): also set
   `tpv->atom.queue->make_request_fn = nvmeibc_tpv_make_request`.
5. Configure queue limits (block size, chunk_sectors, etc.) as today.
6. Set `tpv->atom.disk->private_data = tpv`, `tpv->atom.queue->queuedata = tpv`.
7. `set_capacity(disk, 0)` → `add_disk(disk)` → `set_capacity(disk, capacity)`.
8. Transition atom status to `nvmeiba_status_live`.

The TPV now appears at `/dev/nvmesh-tpv/<name>` with ATOM-managed reference counting
(`nvmeiba_bdev_open` / `nvmeiba_bdev_close`), and `.owner = nvmeiba` ensures module
reference counting keeps nvmeiba (not nvmeibc) pinned by open handles.

### 11.5 Abandon Sequence (Old nvmeibc Going Down)

Triggered from `__detach_all_volumes_of_inst_work()` when `w->is_upgrade == true`.

Replace the current unconditional `nvmeibc_tpv_detach_all_for_inst(w->p)` call:

```c
 /* in __detach_all_volumes_of_inst_work() */
-nvmeibc_tpv_detach_all_for_inst(w->p);
+if (w->is_upgrade)
+    nvmeibc_tpv_abandon_all_for_inst(w->p);
+else
+    nvmeibc_tpv_detach_all_for_inst(w->p);
```

**`nvmeibc_tpv_abandon_all_for_inst(cinst)`** iterates every active TPV for the
instance and, for each:

| Step | Action | Why |
|------|--------|-----|
| 1 | `cancel_work_sync(&tpv->cdv_alloc_work)` | Stop background CDV\_extent requests — TOMA connection is about to go away. |
| 2 | `cancel_work_sync(&tpv->persist_work)` | Ensure no persist work is in-flight before the flush in step 3. |
| 3 | `cancel_delayed_work_sync(&tpv->load_state_work)` | Stop deferred state loader. |
| 4 | If `tpv->dirty`: `nvmeibc_tpv_flush_state(tpv)` | Write the latest L1/L2 tree to the CDV while the CDV is still attached.  If flush fails, log a warning — the tree extent may be stale, and recovery (step A3 below) will reconcile on adopt. |
| 5 | `nvmeiba_os_api_orphan_abandon(&tpv->atom)` | ATOM switches `disk->fops` to `upgrade_fops` (`nvmeiba_b_req_push`). New BIOs are now buffered by ATOM. |
| 6 | Drain in-flight IOs: wait until no `nvmeibc_tpv_make_request` call is executing. Use a per-TPV `atomic_t io_inflight` counter (see §11.7). | Ensures all BIOs currently in the TPV I/O path complete before we disconnect the CDV. |
| 7 | `tpv->atom.queue->queuedata = NULL` | Disconnect nvmeibc context.  Any stale reference to queuedata from a racing code path sees NULL. |
| 8 | `tpv->cdv_vol = NULL` | CDV volume object will be freed when the CDV itself is abandoned/detached. |
| 9 | `nvmeibc_tpv_list_remove(tpv)` | Remove from module-local `nvmeibc_tpv_active_list` (safe: `list_del_init` leaves `list_node` self-linked). |
| 10 | `nvmeibc_tpv_proc_deregister(tpv)` | Remove `/proc/nvmeibc/tpv/<name>/` entries.  They belong to nvmeibc's procfs tree which is going away. |
| 11 | `atomic_set(&tpv->state, TPV_ORPHAN)` | Mark as orphaned. |

**Ordering constraint:** TPV abandon **must** run before CDV abandon because step 4
(flush) issues synchronous IO to the CDV.  This is already satisfied by the current
code ordering in `__detach_all_volumes_of_inst_work()` where TPV teardown precedes
volume teardown.

After abandon, the `nvmeibc_tpv` allocation is anchored in memory by its embedded
`tpv->atom` entry in ATOM's global `all.list`.  The following state survives nvmeibc
unload:

| Preserved | Destroyed |
|-----------|-----------|
| `atom` (disk, queue, pender, users, status=orphan) | `cdv_vol` pointer (nulled) |
| `allocator` (xarray extent\_map, free lists, CDV extent refs) | work\_struct function pointers (stale text addresses) |
| `tpv_uuid`, `tpv_name`, `virtual_size` | `/proc` entries |
| `allocator_toma_id`, `allocator_generation` | `queue->queuedata` (nulled) |
| `pending_bios` (bios parked before abandon) | Module-local list membership |
| `tpv_live_fops` (memory allocation, but `.submit_bio` is stale) | |
| `dirty` flag, `sync_flush` flag | |

### 11.6 Adopt Sequence (New nvmeibc Coming Up)

#### Discovery

After the new `nvmeibc` module initialises and calls `nvmeiba_os_do_on_nvmeibc_up()`
(which returns `n_orphan_osapi > 0`), it runs the regular volume adoption loop.  CDVs
are adopted as part of this loop (they are regular hidden-attach volumes with ATOM
atoms).

TPV adoption runs **after** CDV adoption, because adopt needs a valid `cdv_vol`
pointer.  Two possible triggers:

- **Trigger A (preferred): Management re-sends `AttachVolumes`** — Management sees
  the client reconnect and re-sends the full volume configuration.  When the TPV
  `AttachVolumes` message arrives at `__setup_tpv()` → `nvmeibc_tpv_attach()`, the
  attach function detects the ATOM orphan and adopts instead of creating fresh.
- **Trigger B (proactive scan):** After CDV adoption completes, scan ATOM's list for
  atoms with `status == nvmeiba_status_orphan` whose `disk_name` starts with the TPV
  prefix (`nvmesh-tpv/`).  For each, adopt immediately without waiting for management.
  This provides faster resume at the cost of running before management confirms the
  TPV should still be attached.

**Trigger A is recommended** because it matches the regular volume pattern (management
drives the attach), handles the case where management intentionally does not re-attach
a TPV, and requires minimal new code paths.

#### `nvmeibc_tpv_attach()` — Adopt Path

Extend the existing idempotency check at the top of `nvmeibc_tpv_attach()`:

```c
 struct nvmeibc_tpv *nvmeibc_tpv_attach(...)
 {
-    /* 0. Idempotency: return existing if already in active list */
-    struct nvmeibc_tpv *existing = nvmeibc_tpv_find_by_uuid(tpv_uuid);
-    if (existing) { ... return existing; }
+    /* 0a. Idempotency: return existing if already in active list */
+    struct nvmeibc_tpv *existing = nvmeibc_tpv_find_by_uuid(tpv_uuid);
+    if (existing) { ... return existing; }
+
+    /* 0b. NDU orphan: check ATOM for an orphaned TPV with matching name */
+    {
+        char dev_name[DISK_NAME_LEN];
+        snprintf(dev_name, sizeof(dev_name), "%s/%.30s",
+                 NVMEIBC_TPV_DISK_PREFIX, tpv_name);
+        struct nvmeiba_atom_os_api *orphan_atom =
+            nvmeiba_os_api_orphan_adopt(dev_dir, dev_name);
+        if (orphan_atom) {
+            struct nvmeibc_tpv *tpv = container_of(orphan_atom,
+                                                    struct nvmeibc_tpv, atom);
+            return nvmeibc_tpv_adopt(tpv, cdv, ...);
+        }
+    }
 
     /* 1. Fresh creation path (unchanged) ... */
 }
```

#### `nvmeibc_tpv_adopt()` — Reconnection Steps

| Step | Action | Notes |
|------|--------|-------|
| A1 | Verify `tpv->tpv_uuid` matches `tpv_uuid` argument | Sanity check: ATOM matched by name; verify UUID too. |
| A2 | `tpv->cdv_vol = cdv` | Reconnect to the (now-adopted) CDV volume object. |
| A3 | Re-initialise work structs: `INIT_WORK(&tpv->cdv_alloc_work, nvmeibc_tpv_cdv_alloc_work_fn)`, `INIT_WORK(&tpv->persist_work, nvmeibc_tpv_persist_work_fn)`, `INIT_DELAYED_WORK(&tpv->load_state_work, nvmeibc_tpv_load_state_work_fn)` | Function pointers in work\_struct pointed to old module text.  `INIT_WORK` resets the function pointer and reinitialises the work struct internals.  The work is not scheduled here — it was not pending (cancelled in step 3 of abandon). |
| A4 | Refresh `allocator_toma_id` / `allocator_generation` from CDV cache | Same as fresh attach: `spin_lock(cdv->spinlock)`, copy `cdv->cdv_allocator_toma_id`.  May have changed during NDU window. |
| A5 | `tpv->atom.queue->queuedata = tpv` | Reconnect queue context. |
| A6 | Populate `tpv_live_fops`: copy ATOM `default_fops`, set `.submit_bio = nvmeibc_tpv_submit_bio_wrapper` | Must use the *new* module's function pointer (not the stale one from the old module). |
| A7 | Atomically redirect new BIOs: `spin_lock(&tpv->atom.pender.lock)`, set `tpv->atom.disk->fops = &tpv->tpv_live_fops`, `wmb()`, `spin_unlock(...)` | After this point, new BIOs from the kernel go directly to TPV's make\_request — not to ATOM's buffer. |
| A8 | Drain ATOM pending list: pop every BIO from `tpv->atom.pender.bio_list`, submit via `nvmeibc_tpv_make_request()` | These are BIOs that arrived during the NDU window.  They are replayed in order. |
| A9 | Drain TPV pending bios: `nvmeibc_tpv_retry_pending_bios(tpv)` | These are BIOs that were parked before abandon because the CDV extent pool was empty.  Now that the CDV is reconnected, `cdv_alloc_work` can run again. |
| A10 | `nvmeibc_tpv_list_add(tpv)` | Re-add to module-local active list. |
| A11 | `nvmeibc_tpv_proc_register(tpv)` | Re-create `/proc/nvmeibc/tpv/<name>/` entries. |
| A12 | `atomic_set(&tpv->state, TPV_ATTACHED)` | Resume normal operation. |
| A13 | Schedule `cdv_alloc_work` if free pool is below `low_watermark` | Kick background pre-fetch now that TOMA connectivity is restored. |
| A14 | Optionally schedule `load_state_work` for state reconciliation | Verifies in-memory extent map against the CDV tree extent. Handles the edge case where the flush in abandon step 4 failed and the tree is stale — recovery (TOMA `CDV_LIST_EXTENTS`) adopts any orphan extents. |

**Ordering constraint:** A7 must happen before A8.  The lock in A7 is the same lock
that `nvmeiba_b_req_push` holds when adding BIOs to the pender list.  By holding the
lock while swapping fops, we guarantee that no BIO is lost between the redirect and
the drain: any BIO that arrived before the lock was acquired is on the pender list and
will be drained in A8; any BIO arriving after the lock release goes to TPV's make\_request.

### 11.7 In-Flight IO Draining

ATOM provides `nvmeiba_atom_drain_io()` which waits for an IO refcount to reach zero.
For regular volumes this refcount is managed by `nvmeibc_block_api_os.c` (incremented
on make\_request entry, decremented on BIO completion).

TPVs need an analogous mechanism.  Two options:

**Option A — Use ATOM's existing scalable refcount.**
- `nvmeibc_tpv_make_request` entry: `nvmeiba_atom_io_ref_get(&tpv->atom)`
- BIO completion (endio callback or sync return): `nvmeiba_atom_io_ref_put(&tpv->atom)`
- Abandon step 6: `nvmeiba_atom_drain_io(&tpv->atom)`
- Pro: Reuses existing ATOM infrastructure.
- Con: Requires TPV to track every forwarded-to-CDV BIO's completion, which it currently does not intercept (the BIO goes to CDV and completes via CDV's endio).

**Option B — TPV-local `atomic_t io_inflight` counter.**
- Lighter weight.  Increment on `nvmeibc_tpv_make_request` entry, decrement at the
  point where the TPV no longer needs `tpv->cdv_vol` (i.e., after `nvmeibc_tpv_cdv_submit_bio` returns — at that point the BIO is in CDV's hands).
- Abandon drain: `while (atomic_read(&tpv->io_inflight)) msleep(1);`
- Pro: Simple, no endio interception needed.
- Con: Custom drain loop (but trivial).

**Recommendation: Option B.** The TPV make\_request hands off BIOs to the CDV (via
`submit_bio_noacct`/`generic_make_request`), and from that point the BIO's lifetime is
CDV's concern.  We only need to ensure no code is executing between make\_request entry
and the CDV hand-off.  An `atomic_t` counter with increment-on-entry /
decrement-after-CDV-submit is sufficient and avoids coupling TPV to ATOM's refcount
API.

### 11.8 CDV Ordering During NDU

Both CDV (a regular volume) and its TPVs go through the NDU abandon/adopt cycle.  The
ordering requirements are:

| Phase | Required Order | Reason |
|-------|---------------|--------|
| Abandon | TPVs first, then CDV | TPV flush (step 4) issues synchronous IO to the CDV. |
| Adopt | CDV first, then TPVs | TPV adopt (step A2) needs a valid `cdv_vol` pointer. |

**Abandon ordering** is already guaranteed: `__detach_all_volumes_of_inst_work()` calls
`nvmeibc_tpv_abandon_all_for_inst()` before entering the volume detach loop.

**Adopt ordering** is naturally guaranteed by Trigger A: management sends
`AttachVolumes` for the CDV before the TPV.  The CDV attach goes through the regular
volume adopt path (ATOM orphan → reconnect → live).  The subsequent TPV
`AttachVolumes` calls `nvmeibc_tpv_attach()` which finds the CDV already adopted.
If the CDV attach message hasn't arrived yet (race), the existing `tpv_cdv_retry`
mechanism (deferred retry with `TPV_CDV_RETRY_MAX` attempts) handles it.

### 11.9 Pending BIO Lifecycle Across NDU

Three separate BIO parking lists interact during NDU:

| List | Owner | Contents During NDU |
|------|-------|---------------------|
| `tpv->atom.pender.bio_list` | ATOM | BIOs arriving while nvmeibc is absent.  Drained in adopt step A8. |
| `tpv->pending_bios` | TPV | BIOs parked before abandon because the free TPV\_extent pool was empty.  Survive in memory.  Retried in adopt step A9 (may park again if pool is still empty — `cdv_alloc_work` will eventually replenish). |
| `tpv->pending_l1_flush_bios` | TPV | BIOs waiting for L1 flush (sync\_flush mode only).  The flush completed (or was forced) in abandon step 4.  If any remain, they are retried in adopt step A9. |

No BIOs are lost or failed during NDU.

### 11.10 `nvmeibc_tpv_submit_bio_wrapper` and Module Text Safety

The `submit_bio` function pointer stored in `tpv_live_fops.submit_bio` points to
`nvmeibc_tpv_submit_bio_wrapper`, which lives in nvmeibc's `.text` section.  When
nvmeibc is unloaded, this address becomes invalid.

This is safe because:

1. During abandon (step 5), ATOM atomically replaces `disk->fops` with
   `upgrade_fops`, which has `.submit_bio = nvmeiba_b_req_push` (in ATOM's `.text`).
2. Between abandon and adopt, no code path reaches `tpv_live_fops.submit_bio`.
3. During adopt (step A6), the **new** module's `nvmeibc_tpv_submit_bio_wrapper`
   address is written into `tpv_live_fops.submit_bio` before `disk->fops` is swapped
   back to `tpv_live_fops` (step A7).

The `tpv_live_fops` memory itself (part of the kzalloc'd `nvmeibc_tpv`) persists — only
the function pointer value is stale between module unload and adopt step A6.

### 11.11 ATOM Discovery — Distinguishing TPV Atoms from Regular Atoms

ATOM's `nvmeiba_os_api_orphan_adopt(dev_dir, dev_name)` matches by directory and name.
TPV disk names use the `nvmesh-tpv/` prefix (e.g., `nvmesh-tpv/my-tpv-01`), which is
distinct from regular volume names.  No additional type field in the atom is needed.

For proactive scan (Trigger B, if implemented in the future): iterate ATOM's list via
`nvmeiba_os_api_exec_for_each_atom()`, filter by `strncmp(atom->dev_name, "nvmesh-tpv/", 11)`.

### 11.12 Implementation Checklist

#### New files
*(none)*

#### Modified files

| File | Changes |
|------|---------|
| `clnt/tpv/nvmeibc_tpv.h` | Embed `nvmeiba_atom_os_api atom` in `struct nvmeibc_tpv`; add `tpv_live_fops` field; add `TPV_ORPHAN` state; add `atomic_t io_inflight`; add `tpv_disk()`/`tpv_queue()` accessor macros; declare `nvmeibc_tpv_abandon_all_for_inst()`. |
| `clnt/tpv/nvmeibc_tpv.c` | Rewrite `nvmeibc_tpv_blkdev_register()` to use ATOM constructor + `add_disk()`.  Rewrite `nvmeibc_tpv_blkdev_unregister()` to use ATOM destructor.  Add `nvmeibc_tpv_adopt()` (steps A1–A14).  Add `nvmeibc_tpv_abandon_all_for_inst()` (steps 1–11).  Extend `nvmeibc_tpv_attach()` with orphan-adopt check (step 0b).  Replace all `tpv->disk` / `tpv->queue` with `tpv_disk(tpv)` / `tpv_queue(tpv)`. |
| `clnt/tpv/nvmeibc_tpv_io.c` | Add `io_inflight` increment at `nvmeibc_tpv_make_request` entry and decrement after CDV hand-off.  Replace `tpv->queue` references. |
| `clnt/tpv/nvmeibc_tpv_cdv.c` | Replace `tpv->disk` / `tpv->queue` with accessors. |
| `clnt/tpv/nvmeibc_tpv_persist.c` | Replace `tpv->disk` / `tpv->queue` with accessors. |
| `clnt/tpv/nvmeibc_tpv_proc.c` | Replace `tpv->disk` / `tpv->queue` with accessors. |
| `clnt/tpv/nvmeibc_tpv_test.c` | Adapt test harness for ATOM-based registration. |
| `clnt/main/cc_api/nvmeibc_main_capi_manipulate_vols.inc.c` | In `__detach_all_volumes_of_inst_work()`: branch on `w->is_upgrade` to call `nvmeibc_tpv_abandon_all_for_inst()` vs `nvmeibc_tpv_detach_all_for_inst()`. |
| `clnt/block/nvmeibc_block_api_os.c` *(possibly)* | Expose helper to obtain `nvmeiba_to_c_handover.fops` pointer for TPV `tpv_live_fops` initialisation, if not already accessible. |

#### Not modified
| File | Why |
|------|-----|
| `clnt/atom/*` | No changes to ATOM itself — the existing orphan-abandon / orphan-adopt API is sufficient. |
| Management server | Management already re-sends `AttachVolumes` on client reconnect; no protocol changes needed. |

### 11.13 Edge Cases and Failure Modes

| Scenario | Handling |
|----------|----------|
| **Flush fails during abandon** (CDV IO error) | Log warning, proceed with abandon.  In-memory extent map is still valid.  On adopt, step A14 runs recovery (`CDV_LIST_EXTENTS`) which reconciles any divergence between the tree extent and TOMA's allocator state. |
| **CDV not yet adopted when TPV attach arrives** | Existing `tpv_cdv_retry` mechanism retries up to `TPV_CDV_RETRY_MAX` times at `TPV_CDV_RETRY_MS` intervals.  Works unchanged because the retry looks up the CDV by UUID in the volume list. |
| **Management does not re-send TPV attach** (TPV was deleted during NDU window) | The orphaned TPV atom remains in ATOM's list indefinitely.  Acceptable for the NDU window (seconds).  If management explicitly sends a detach for the TPV, the new nvmeibc can look up the orphan by name, adopt it, and immediately detach it (which calls `del_gendisk` properly). |
| **Module crash during abandon** (between flush and orphan) | Equivalent to a hard restart — TPV block device is gone.  Recovery on next clean attach rebuilds from the tree extent + `CDV_LIST_EXTENTS`.  No worse than today. |
| **Two TPVs on same CDV, different clients** | Each TPV is per-client-instance.  `nvmeibc_tpv_abandon_all_for_inst(cinst)` only touches TPVs whose `cdv_vol->p == cinst`.  Independent. |
| **Allocator identity changes during NDU** | On adopt (step A4), `allocator_toma_id` is refreshed from the CDV cache.  `cdv_alloc_work` (step A13) sends requests with the current generation; stale-generation responses (`CDV_ALLOC_WRONG_GEN`) are handled normally. |
| **open() / close() during NDU** | Handled by ATOM's `nvmeiba_bdev_open()` / `nvmeiba_bdev_close()`.  The atom's `users.n_opens` refcount tracks open handles.  The block device remains in `/dev/` throughout. |

### 11.14 Testing

| Test | Method |
|------|--------|
| **Basic NDU round-trip** | Attach CDV + TPV, write data, trigger NDU (unload + reload nvmeibc), verify `/dev/` device persists, read data back. |
| **IO continuity across NDU** | Start sustained write workload (fio), trigger NDU mid-flight, verify no IO errors and all writes landed.  Expect latency spike during NDU window. |
| **Multiple TPVs on same CDV** | Two TPVs, both active during NDU, both survive. |
| **Dirty state flush failure** | Inject CDV IO error during abandon flush; verify recovery on adopt reconciles extent map via `CDV_LIST_EXTENTS`. |
| **CDV retry on adopt** | Delay CDV adoption (slow management message); verify TPV attach retries and succeeds. |
| **Orphan cleanup on non-reattach** | Adopt + immediately detach a TPV that management didn't re-send. |
| **Self-test adaptation** | Extend `nvmeibc_tpv_test.c` with a `tpv_ktest_ndu_roundtrip` that simulates abandon → adopt at the allocator level (mock ATOM orphan/adopt). |

## Part 12 — Graceful IO Rejection and Pending-Bio Timeout

### 12.1 Problem Statement

Regular volumes use `io_max_retry_secs` (a module parameter) to control how long IOs
are retried before being failed with `-EIO`.  The per-volume `max_retry_jiffies` field
is set to different values at different lifecycle stages:

| Stage | Timeout | Behaviour |
|-------|---------|-----------|
| Attach (before IO enabled) | 30 s | Bios parked in resubmitter; fail after 30 s if topology never comes up |
| Normal operation | ~∞ (or module param) | Bios retried until disk recovers or timeout expires |
| Detach | 10 ms | In-flight bios get a tiny grace period, then `-EIO` |
| NDU upgrade | 0 ms | Immediate fail; ATOM buffers and replays later |

TPV has **none of this**.  `nvmeibc_tpv_make_request` returns `-EIO` immediately when
`state != TPV_ATTACHED`, and bios parked on `pending_bios` (waiting for CDV\_extent
allocation) or during `!state_loaded` (waiting for L1/L2 tree load) have **no timeout
at all** — they can wait indefinitely.

### 12.2 Design

Add `max_retry_jiffies` to `struct nvmeibc_tpv` and a `timeout_work` (delayed\_work)
that fires when parked bios have waited too long.

#### Timeout values by state

TPV uses the same `nvmeibc_io_max_retry_secs` module parameter as regular volumes.
When the parameter is 0 (default), the same fallback values apply:

| State | `max_retry_jiffies` | Source |
|-------|---------------------|--------|
| Attaching (state\_loaded == false) | `(nvmeibc_io_max_retry_secs ?: 30) * HZ` | Same as `IO_TIME_OUT_ATTACH` in `nvmeibc_block.c` |
| Attached (normal operation) | `(nvmeibc_io_max_retry_secs ?: (1 << 20)) * HZ` | Same as `IO_TIME_OUT_NORMAL` in `nvmeibc_block.c` |
| Detaching | `HZ / 100` (10 ms) | Matches regular volume detach drain |

When `nvmeibc_io_max_retry_secs` is nonzero, the configured value is used at all
stages (attach, normal, detach override to 10 ms is always applied).

#### Timeout work lifecycle

1. **Schedule** — whenever a bio is parked on `pending_bios` (either from the
   `!state_loaded` path or the `-EAGAIN` extent-pool-empty path) and `timeout_work`
   is not already pending, schedule it with delay = `max_retry_jiffies`.
2. **Fire** — `nvmeibc_tpv_timeout_work_fn` runs: fail all bios on `pending_bios`
   and `pending_l1_flush_bios` with `-EIO`.  If the TPV is still in a normal state
   and new bios get parked afterwards, a new timeout cycle starts.
3. **Cancel** — when bios are successfully drained (by `retry_pending_bios` or
   `forward_l1_flush_bios`), cancel `timeout_work`.  If some bios were re-parked
   (partial pool refill), the parking code re-schedules.
4. **Detach fast-drain** — `nvmeibc_tpv_detach` sets `max_retry_jiffies = HZ / 100`,
   then calls `mod_delayed_work` to re-arm `timeout_work` at the new short delay.
   The timeout fires within 10 ms and fails everything.  `nvmeibc_tpv_detach` then
   proceeds to `cancel_delayed_work_sync` and fails any stragglers directly.

#### Transition: attach → normal

When `load_state_work_fn` sets `state_loaded = true` in `nvmeibc_tpv_persist.c`, it
also updates `max_retry_jiffies` to the normal-operation value.  This ensures that
bios parked after state is loaded (CDV extent pool empty) use the long timeout, not
the 30-second attach timeout.

### 12.3 Struct Changes

```c
 struct nvmeibc_tpv {
     /* ... existing fields ... */
+
+    /*
+     * IO timeout for parked bios — mirrors regular volume max_retry_jiffies.
+     * Uses nvmeibc_io_max_retry_secs module param (shared with regular
+     * volumes); when 0, falls back to IO_TIME_OUT_ATTACH / IO_TIME_OUT_NORMAL.
+     * Reduced to HZ / 100 at detach for fast drain.
+     */
+    unsigned long                 max_retry_jiffies;
+
+    /*
+     * Timeout sweep for pending_bios / pending_l1_flush_bios.
+     * Scheduled when the first bio is parked; fires after max_retry_jiffies
+     * to fail all parked bios with -EIO.  Cancelled when bios are drained
+     * successfully.
+     */
+    struct delayed_work           timeout_work;
 };
```

### 12.4 IO Path Changes

#### Parking a bio (`tpv_handle_one_bio`, `-EAGAIN` path)

```c
 if (rv == -EAGAIN) {
     unsigned long flags;

     spin_lock_irqsave(&tpv->pending_bio_lock, flags);
+    if (bio_list_empty(&tpv->pending_bios))
+        schedule_delayed_work(&tpv->timeout_work, tpv->max_retry_jiffies);
     bio_list_add(&tpv->pending_bios, bio);
     spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);
     return -EAGAIN;
 }
```

#### Parking a bio (`nvmeibc_tpv_make_request`, `!state_loaded` path)

```c
 if (!tpv->state_loaded) {
+    if (bio_list_empty(&tpv->pending_bios))
+        schedule_delayed_work(&tpv->timeout_work, tpv->max_retry_jiffies);
     bio_list_add(&tpv->pending_bios, bio);
     spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);
     return REQ_RET_ZERO;
 }
```

#### Draining bios (`retry_pending_bios`, `forward_l1_flush_bios`)

After successfully draining all parked bios, cancel the timeout:

```c
 void nvmeibc_tpv_retry_pending_bios(struct nvmeibc_tpv *tpv)
 {
     /* ... drain loop ... */
+    /* If all bios were dispatched (none re-parked), cancel the timeout.
+     * If some were re-parked by tpv_handle_one_bio, the parking code
+     * already re-scheduled timeout_work. */
+    cancel_delayed_work(&tpv->timeout_work);
 }
```

#### Timeout fire

```c
void nvmeibc_tpv_timeout_work_fn(struct work_struct *work)
{
    struct nvmeibc_tpv *tpv = container_of(to_delayed_work(work),
                                            struct nvmeibc_tpv, timeout_work);
    struct bio_list  expired;
    struct bio      *bio;
    unsigned long    flags;

    bio_list_init(&expired);

    spin_lock_irqsave(&tpv->pending_bio_lock, flags);
    bio_list_merge(&expired, &tpv->pending_bios);
    bio_list_init(&tpv->pending_bios);
    bio_list_merge(&expired, &tpv->pending_l1_flush_bios);
    bio_list_init(&tpv->pending_l1_flush_bios);
    spin_unlock_irqrestore(&tpv->pending_bio_lock, flags);

    while ((bio = bio_list_pop(&expired)) != NULL)
        bio_endio(bio, -EIO);
}
```

### 12.5 Detach Path

```c
 void nvmeibc_tpv_detach(struct nvmeibc_tpv *tpv)
 {
     atomic_set(&tpv->state, TPV_DETACHING);
     nvmeibc_tpv_list_remove(tpv);

+    /* Shorten timeout so any parked bios fail within 10 ms. */
+    tpv->max_retry_jiffies = HZ / 100;
+    mod_delayed_work(system_wq, &tpv->timeout_work, tpv->max_retry_jiffies);

     cancel_delayed_work_sync(&tpv->load_state_work);
     cancel_work_sync(&tpv->cdv_alloc_work);
     cancel_work_sync(&tpv->persist_work);
+    cancel_delayed_work_sync(&tpv->timeout_work);
     /* ... flush, unregister, fail remaining bios ... */
 }
```

### 12.6 Modified Files

| File | Changes |
|------|---------|
| `clnt/tpv/nvmeibc_tpv.h` | Add `max_retry_jiffies`, `timeout_work` fields; declare `nvmeibc_tpv_timeout_work_fn()`. |
| `clnt/tpv/nvmeibc_tpv.c` | Init `max_retry_jiffies` and `timeout_work` in attach; shorten timeout + mod\_delayed\_work + cancel in detach. |
| `clnt/tpv/nvmeibc_tpv_io.c` | Schedule `timeout_work` when parking bios; cancel when draining; add `nvmeibc_tpv_timeout_work_fn()`. |
| `clnt/tpv/nvmeibc_tpv_persist.c` | Update `max_retry_jiffies` to normal value when `state_loaded` is set. |

---

## 13. NVMesh-CLI CDV/TPV Support

### 13.1 Overview

CDV and TPV are exposed as top-level CLI groups (`nvmesh cdv` and `nvmesh tpv`), matching the separate GUI sections. They are `volumeClass`-discriminated sub-types of `Volume`, sharing the `/volumes` REST route.

**Files to modify:**

| File | Change |
|------|--------|
| `nvmesh-infra/xlro/core/entities/volume.py` | Add `CDVConfig`, `TPVConfig`, extend `Volume`, add `CDV`/`TPV` entity subclasses |
| `nvmesh-infra/xlro/core/entities/rest.yaml` | Add `CDV` and `TPV` entity blocks to version "15" |
| `nvmesh-infra/xlro/tools/cli/rest_click.py` | Extend `waitable` tuple with `'cdv'` and `'tpv'` |
| `nvmesh-infra/xlro/tools/cli/rest_custom.py` | Add `CDVGroup` and `TPVGroup` |
| `nvmesh-infra/xlro/tools/cli/current.api` | Append CDV/TPV golden-file entries |
| `nvmesh-infra/xlro/tools/cli/current.display` | Append CDV/TPV golden-file entries |

### 13.2 SDK Layer (`volume.py`)

Add after `EncryptionObj` (~line 70):

```python
class CDVConfig(SdkObject):
    cdvExtentSizeMiB : int    # power-of-2: 64–65536 MB
    allocatorSizeGiB : int    # default 1
    maxTPVs         : int    # default 512

class TPVConfig(SdkObject):
    cdvId           : str    # required; parent CDV name/_id
    tpvExtentSizeKB : int    # power-of-2: 64–65536 KB
    cdvName         : str    # readonly; denormalized from parent CDV for display
    exclusiveClient : str    # readonly; set by management on attach, cleared on detach
    # virtualSizeGB removed — TPV virtual size is now stored in Volume.capacity
    # (bytes), consistent with regular volumes.  The CLI's `tpv create` takes a
    # top-level `capacity` param, not a nested tpvConfig field.
    # maxVirtualSizeGB removed — no longer part of the data model
```

Note: `SdkObject` field names must use the exact camelCase the server expects (`cdvId`, `tpvExtentSizeKB`) — nested fields bypass the `rest2infra` mapping. Virtual size lives on `Volume.capacity`; `cdvName` and `exclusiveClient` are populated by management for display purposes and are not writable from the CLI.

Add to `Volume` body after `metadata` (~line 501):

```python
volumeClass : str                    = PropertySpec(str)
cdvConfig   : Optional[CDVConfig]    = PropertySpec(CDVConfig)
tpvConfig   : Optional[TPVConfig]    = PropertySpec(TPVConfig)
tpvCount    : int                    = PropertySpec(int, readonly=True)
```

Add `CDV` and `TPV` subclasses after the `Volume` class:

```python
@sdk_entity(sourcetypes=[SourceTypes.LOCAL, SourceTypes.MANAGEMENT])
class CDV(Volume):
    """Capacity Data Volume — a Volume with volumeClass='CDV'."""

    @classmethod
    def _get_filter(cls, mgmt=None, **kwargs):
        return [MongoObj('volumeClass', 'CDV')] + super()._get_filter(mgmt, **kwargs)


@sdk_entity(sourcetypes=[SourceTypes.LOCAL, SourceTypes.MANAGEMENT])
class TPV(Volume):
    """Thin-Provisioned Volume — a Volume with volumeClass='TPV'."""

    @classmethod
    def _get_filter(cls, mgmt=None, **kwargs):
        return [MongoObj('volumeClass', 'TPV')] + super()._get_filter(mgmt, **kwargs)
```

`MongoObj` is already imported at line 49. Overriding `_get_filter` ensures every `_sdk_get` call (show, dicts_by_name, delete pre-fetch) is automatically restricted to the correct `volumeClass`.

### 13.3 Entity Metadata (`rest.yaml`)

Add inside the version `"15"` `entities:` block.

**CDV:**

```yaml
CDV:
  route: volumes
  dbkey: name
  rest2infra:
    <<: *volume_r2i
  # All standard volume params are mutable on a CDV.
  # cdvConfig (cdvExtentSizeMiB, allocatorSizeGiB, maxTPVs) is immutable post-creation
  # and therefore lives only under ops.create.params, not here.
  params: *volume_params
  display:
    - name
    - description
    - health
    - status
    - action
    - capacity
    - tpvCount
    - cdvConfig
    - runtimeStats       # populated by TOMA stats handler — see §10.4
  ops:
    rebuild:
      help: Rebuild a CDV
      route: rebuildVolumes
      style: keys
      payload: "{{entities}}"
    delete:
      confirm: |
        WARNING: Deleting a CDV also removes its backing allocation. This is irreversible.
        Are you sure you want to continue?
      payload: "[{% for e in entities %}{ '_id': '{{e.name}}', 'uuid': '{{e.uuid}}'}, {% endfor %}]"
      wait:
        <<: *wait_for_vol_delete
    create:
      wait:
        prop_name: 'status'
        values: ['online', 'offline', 'degraded']
        is_matching: true
      params:
        - cdvConfig          # recurses into CDVConfig → --cdv-config-* options; create-only
        - RAIDlevel
        - stripeWidth
        - stripeSize
        - numberOfMirrors
        - dataBlocks
        - parityBlocks
        - protectionLevel
        - ignoreNodeSeparation
```

**TPV:**

```yaml
TPV:
  route: volumes
  dbkey: name
  rest2infra:
    <<: *volume_r2i
  # Only name and description are mutable post-creation; tpvConfig is immutable
  # and therefore lives only under ops.create.params.
  params:
    - name
    - description
  display:
    - name
    - description
    - health
    - status
    - action
    - tpvConfig
  ops:
    delete:
      confirm: |
        WARNING: You are about to delete a Thin-Provisioned Volume.
        Are you sure you want to continue?
      route: tpv/delete
      style: entities
      payload: "[{% for e in entities %}{'_id': '{{e.name}}'}, {% endfor %}]"
      wait:
        <<: *wait_for_vol_delete
    extend:
      help: Extend the virtual size of a TPV
      route: tpv/extend
      style: one
      opts:
        newSizeGb:
          type: float
          required: true
      payload: "{'tpvId': '{{entities[0].name}}', 'newSizeGB': {{new_size_gb}}}"
    create:
      wait:
        prop_name: 'status'
        values: ['online', 'offline', 'degraded', 'unavailable']
        is_matching: true
      params:
        - capacity           # top-level Volume.capacity — virtual size in bytes
        - tpvConfig          # recurses into TPVConfig → --tpv-config-* options; create-only
        - isEncrypted        # per-TPV LUKS; see TPV_EncryptionPlan.md
        - encryption         # encryption sub-object (e.g., headerSize)
```

Design notes:
- CDV uses `*volume_params` as its base, making all standard volume fields updatable. `cdvConfig` is deliberately excluded from `params` (create-only).
- TPV base `params` is minimal (name + description only). `tpvConfig`, `capacity`, and encryption fields are create-only.
- Virtual size is `Volume.capacity` (bytes), not a nested `tpvConfig.virtualSizeGB` — consistent with regular volumes.
- TPV `delete` uses `route: tpv/delete` so `_delete_many → do_operation('delete')` POSTs to `/volumes/tpv/delete` automatically.
- CDV `rebuild` follows the same pattern as Volume rebuild (`route: rebuildVolumes`, `style: keys`).
- TPV `extend` is a standard op with `style: one`; `newSizeGB` payload field uses the literal camelCase key name (`newSizeGb` in CLI opts, rendered as `newSizeGB` in the route payload).
- TPV `update` is handled in Python (§13.5) because it must go to `/volumes/tpv/update`, not `/volumes/update`.

### 13.4 `rest_click.py` — Extend `waitable`

At line ~485, extend the tuple so CDV and TPV get auto-generated `wait` commands:

```python
# Before:
waitable = ('client', 'target', 'drive', 'volume')
# After:
waitable = ('client', 'target', 'drive', 'volume', 'cdv', 'tpv')
```

### 13.5 Custom CLI Behavior (`rest_custom.py`)

Append at the end of the file:

```python
class CDVGroup(RestGroup):
    logger = logging.getLogger('CDVGroup')

    def _process_kwargs(self, rest_ctx, kwargs):
        processed = super()._process_kwargs(rest_ctx, kwargs)
        if click.get_current_context().command.name == 'create':
            processed['volumeClass'] = 'CDV'
        return processed


class TPVGroup(RestGroup):
    logger = logging.getLogger('TPVGroup')

    def _process_kwargs(self, rest_ctx, kwargs):
        processed = super()._process_kwargs(rest_ctx, kwargs)
        if click.get_current_context().command.name == 'create':
            processed['volumeClass'] = 'TPV'
        return processed

    @rest_callback
    def do_create(self, **kwargs):
        ctx = click.get_current_context()
        if ctx.command.name == 'update':
            # TPV update must POST to /volumes/tpv/update, not /volumes/update.
            # Both 'create' and 'update' share do_create as callback
            # (set by RestGroup.set_update_create()), so we intercept here.
            _, rest_ctx = get_rest_context(ctx)
            obj = ctx.obj
            prop_values = self._process_kwargs(obj, kwargs)
            keyprop = rest_ctx.rest_info.rest2infra.get(
                rest_ctx.rest_info.dbkey, rest_ctx.rest_info.dbkey)
            names = prop_values.pop(keyprop, [])
            payload = [{'_id': n, **{k: v for k, v in prop_values.items()
                                     if k == 'description'}}
                       for n in names]
            err, out = obj.entity._makePost(obj.manager, ['tpv', 'update'], payload)
            if err:
                raise Exception(f'TPV update failed: {err}')
            response = True
            for r in (out or []):
                if r.get('success'):
                    success_msg(f'[{r.get("_id", "")}] success')
                else:
                    response = False
                    failure_msg(self.format_failure(r))
            return out if response else False
        return super().do_create(**kwargs)
```

### 13.6 `current.api` Additions

Append after the last `[API 8]` line (using `[API 15]` since CDV/TPV are version-15 features):

```
[API 15] cdv count
[API 15] cdv create
[API 15] cdv create capacity SIZE ? (None)
[API 15] cdv create cdv-config-allocator-size-gb INT ? (None)
[API 15] cdv create cdv-config-cdv-extent-size-mb INT ? (None)
[API 15] cdv create cdv-config-max-tpvs INT ? (None)
[API 15] cdv create cli-property STRING ? ... (None)
[API 15] cdv create cli-template STRING ? (None)
[API 15] cdv create data-blocks INT ? (None)
[API 15] cdv create description STRING ? (None)
[API 15] cdv create drive-classes ID/NAME ? ... (None)
[API 15] cdv create ignore-node-separation ? (None)
[API 15] cdv create limit-by-disks STRING ? ... ([])
[API 15] cdv create limit-by-nodes STRING ? ... ([])
[API 15] cdv create name STRING (None)
[API 15] cdv create number-of-mirrors INT ? (None)
[API 15] cdv create parity-blocks INT ? (None)
[API 15] cdv create protection-level ECSEPARATIONTYPE ? (None)
[API 15] cdv create raid-level RAIDLEVEL ? (None)
[API 15] cdv create relative-rebuild-priority INT ? (None)
[API 15] cdv create stripe-size INT ? (None)
[API 15] cdv create stripe-width INT ? (None)
[API 15] cdv create target-classes ID/NAME ? ... (None)
[API 15] cdv create timeout INT ? (60)
[API 15] cdv create wait ? (False)
[API 15] cdv delete
[API 15] cdv delete name STRING ... (None)
[API 15] cdv delete timeout INT ? (60)
[API 15] cdv delete wait ? (False)
[API 15] cdv delete yes ? (False)
[API 15] cdv show
[API 15] cdv show fields STRING ? (None)
[API 15] cdv show limit INT ? (None)
[API 15] cdv show name STRING ? ... (None)
[API 15] cdv show output-format tabular|rows|json|list ? (None)
[API 15] cdv show skip INT ? (0)
[API 15] cdv rebuild
[API 15] cdv rebuild name STRING ... (None)
[API 15] cdv update
[API 15] cdv update capacity SIZE ? (None)
[API 15] cdv update cli-property STRING ? ... (None)
[API 15] cdv update cli-template STRING ? (None)
[API 15] cdv update crc-enabled ? (None)
[API 15] cdv update description STRING ? (None)
[API 15] cdv update drive-classes ID/NAME ? ... (None)
[API 15] cdv update enabled-nvmf-clients STRING ? ... (None)
[API 15] cdv update is-read-only ? (None)
[API 15] cdv update limit-by-disks STRING ? ... (None)
[API 15] cdv update limit-by-nodes STRING ? ... (None)
[API 15] cdv update mdv-spec-disk-classes ID/NAME ? ... (None)
[API 15] cdv update mdv-spec-limit-by-disks ID/NAME ? ... (None)
[API 15] cdv update mdv-spec-limit-by-nodes ID/NAME ? ... (None)
[API 15] cdv update mdv-spec-server-classes ID/NAME ? ... (None)
[API 15] cdv update mdv-spec-vpg ID/NAME ? (None)
[API 15] cdv update name STRING (None)
[API 15] cdv update nvmf-enabled ? (None)
[API 15] cdv update relative-rebuild-priority INT ? (None)
[API 15] cdv update target-classes ID/NAME ? ... (None)
[API 15] cdv update volume-security-group ID/NAME ? ... (None)
[API 15] cdv wait
[API 15] cdv wait boolean ? (False)
[API 15] cdv wait missing STRING ? (None)
[API 15] cdv wait name STRING ... (None)
[API 15] cdv wait poll INT ? (1)
[API 15] cdv wait property STRING (None)
[API 15] cdv wait timeout INT ? (20)
[API 15] cdv wait value STRING ... (None)
[API 15] tpv count
[API 15] tpv create
[API 15] tpv create cli-property STRING ? ... (None)
[API 15] tpv create cli-template STRING ? (None)
[API 15] tpv create description STRING ? (None)
[API 15] tpv create name STRING (None)
[API 15] tpv create timeout INT ? (60)
[API 15] tpv create tpv-config-cdv-id STRING ? (None)
[API 15] tpv create tpv-config-tpv-extent-size-kb INT ? (None)
[API 15] tpv create tpv-config-virtual-size-gb FLOAT ? (None)
[API 15] tpv create wait ? (False)
[API 15] tpv delete
[API 15] tpv delete name STRING ... (None)
[API 15] tpv delete timeout INT ? (60)
[API 15] tpv delete wait ? (False)
[API 15] tpv delete yes ? (False)
[API 15] tpv extend
[API 15] tpv extend name STRING ... (None)
[API 15] tpv extend new-size-gb FLOAT (None)
[API 15] tpv show
[API 15] tpv show fields STRING ? (None)
[API 15] tpv show limit INT ? (None)
[API 15] tpv show name STRING ? ... (None)
[API 15] tpv show output-format tabular|rows|json|list ? (None)
[API 15] tpv show skip INT ? (0)
[API 15] tpv update
[API 15] tpv update cli-property STRING ? ... (None)
[API 15] tpv update cli-template STRING ? (None)
[API 15] tpv update description STRING ? (None)
[API 15] tpv update name STRING (None)
[API 15] tpv wait
[API 15] tpv wait boolean ? (False)
[API 15] tpv wait missing STRING ? (None)
[API 15] tpv wait name STRING ... (None)
[API 15] tpv wait poll INT ? (1)
[API 15] tpv wait property STRING (None)
[API 15] tpv wait timeout INT ? (20)
[API 15] tpv wait value STRING ... (None)
```

### 13.7 `current.display` Additions

Append:

```
15:CDV:action
15:CDV:capacity
15:CDV:cdvConfig
15:CDV:description
15:CDV:health
15:CDV:name
15:CDV:status
15:CDV:tpvCount
15:TPV:action
15:TPV:description
15:TPV:health
15:TPV:name
15:TPV:status
15:TPV:tpvConfig
```

### 13.8 Known Pitfalls

- **YAML anchor scope**: `*volume_r2i` and `*wait_for_vol_delete` are file-scoped anchors (not version-scoped), so they are safely reachable from version "15".
- **`SdkObject` field names**: `TPVConfig` fields must use the exact camelCase the server expects (`cdvId`, `tpvExtentSizeKB`, `virtualSizeGB`) — nested fields bypass `rest2infra` mapping.
- **`volumeClass` passthrough**: `volumeClass` has no `rest2infra` entry, so `infra2rest.get('volumeClass', 'volumeClass')` returns `'volumeClass'` — exactly what the server expects.
- **TPV create required fields**: Server validates `tpvConfig.cdvId` and `tpvConfig.virtualSizeGB` are present. Server error messages will surface if omitted; consider adding a `default_templates.yaml` entry.
- **CDV create required**: Server requires `capacity` and `cdvConfig.cdvExtentSizeMiB`. Consider a template entry.
- **`_get_filter` prepend order**: `[MongoObj('volumeClass', ...)] + super()._get_filter(...)` — the volumeClass filter comes first; the server ANDs all filter objects, so order doesn't affect correctness.

---

## Part 14 — CSI Driver TPV Orchestration

### 14.1 Overview

`nvmesh-csi-driver` (Python, CSI gRPC) gains the ability to provision and attach TPVs to Kubernetes Pods. CDV lifecycle stays out of the CSI driver entirely — CDVs are created and operated by cluster admins via the GUI/CLI; the CSI driver only consumes them.

Two-phase rollout:

- **Phase A (MVP) — named CDV.** StorageClass parameters pin each TPV to a specific, admin-pre-created CDV by name. Minimal surface, shippable first.
- **Phase B — regex-matched CDV pool.** StorageClass declares a pool via a name regex; the driver selects a CDV on each `CreateVolume` based on `tpvCount < maxTPVs` and free capacity. Phase A is folded into Phase B as the trivial single-match regex.

Snapshot/clone and auto-create-CDV are explicitly out of scope.

### 14.2 StorageClass Parameters

| Parameter | Phase | Required | Purpose |
|---|---|---|---|
| `volumeClass` | A | yes | Must be `TPV`. Absent or `REGULAR` → today's regular-volume path. |
| `cdvName` | A | one of two | Exact CDV name (Phase A). |
| `cdvNameRegex` | B | one of two | JavaScript-compatible regex matched against CDV `name` (Phase B). Mutually exclusive with `cdvName`. |
| `tpvExtentSizeKB` | A | optional | Power-of-2, [64, 65536]. Default from driver config. Must satisfy `tpvExtentSizeKB ≤ parent.cdvExtentSizeMiB × 1024`. |

The TPV virtual-size ceiling is the parent CDV's `capacity` — enforced server-side in `modules/volume.js` (`createTPV`: "TPV capacity cannot exceed parent CDV capacity") and again on `/tpv/extend`. No driver-side cap.

Access mode: `SINGLE_NODE_WRITER` only. The driver rejects `MULTI_NODE_*` with CSI `InvalidArgument` before hitting management, to match TPV's `exclusiveClient` semantics.

Topology: the driver's existing `ZoneTopologyFetcher` is used to filter CDV candidates by zone, and the TPV's `accessible_topology` on the returned `Volume` is set to the chosen CDV's zone.

### 14.3 Management API — No Server-Side Enhancement Needed for MVP

The existing `GET /volumes/all/:page/:count?filter=<JSON>` route (`nvmesh-management/routes/volumes.js:108`, backed by `volumeModule.getAllVolumes` at `modules/volume.js:44`) passes the decoded JSON filter directly into the MongoDB aggregation `$match` stage. MongoDB natively supports `$regex`, so the CSI driver can request:

```http
GET /volumes/all/0/0?filter={"volumeClass":"CDV","name":{"$regex":"^pool-gold-"}}
             &projection={"_id":1,"name":1,"cdvConfig":1,"tpvCount":1,"chunks.zone":1,"status":1,"health":1}
             &sort={"tpvCount":1}
```

and get back the candidate CDVs already filtered and sorted server-side. Fields not expressible in Mongo (e.g. free-capacity ratio derived from TOMA events) are post-filtered in the driver.

**Pool-selection policy in the driver (Phase B):**

1. Issue the `/volumes/all` query above with the user-supplied regex.
2. Drop CDVs whose `tpvCount ≥ cdvConfig.maxTPVs`, whose `status` is not `online`, or whose zone is excluded by topology constraints.
3. Prefer CDVs not currently in capacity warning (see `TOMAToManagement_TP.cdvCapacityWarning` in Part 1).
4. Pick the remaining CDV with the lowest `tpvCount` (load-spreading). Ties: lowest `_id` (deterministic).

**Considered but rejected — new server endpoint `GET /volumes/cdvs?pool=<regex>`:** adds surface, duplicates existing filter semantics, and delivers nothing the Mongo filter doesn't already give us. Skip it.

**Possible future optimization (not now):** if the `{volumeClass, name}` compound query becomes hot, add a Mongo index. Measured on realistic CDV counts first; don't add index bloat speculatively.

**Security note:** `/volumes/all` is behind authenticated session middleware but not `isAdminRole`. A client-supplied regex is a mild ReDoS vector against MongoDB. The CSI driver uses a service account that is already trusted; no change required. If the endpoint is ever exposed more broadly, add regex length + complexity limits in the route.

### 14.4 CSI Driver File Changes

| File | Change |
|---|---|
| `driver/nvmesh_mgmt_api.py` | Add `list_cdvs(name_regex, zone=None, projection=…)`, `create_tpv(...)`, `extend_tpv(uuid, gb)`, `delete_tpv(uuid)`, `get_volume_class(volume_id)`. |
| `driver/controller_service.py` | Branch on `volumeClass` in `CreateVolume`, `ControllerExpandVolume`, `DeleteVolume`. Pre-check `tpvConfig.exclusiveClient` in `ControllerPublishVolume`. |
| `driver/topology_service.py` | Expose `zones_matching(topology_requirement)` helper reused by the pool selector. |
| `driver/consts.py` | Add `VOLUME_CLASS_TPV/CDV/REGULAR`, extent-size bounds, parameter-name constants. |
| `driver/config.py` | Add `TPV_DEFAULT_EXTENT_KB`. |
| `deploy/kubernetes/helm/.../templates/storageclass.yaml` | Add example TPV StorageClass (`volumeClass: TPV`, `cdvNameRegex: "^pool-"`). |
| `test/integration/` | *Planned*. New cases: TPV create/attach/detach/extend/delete; pool with 0 matches → `ResourceExhausted`; access-mode rejection; extend past CDV capacity → `FailedPrecondition`. Not yet present in the repo as of 2026-04-19. |

### 14.5 Controller RPC Changes (detail)

**`CreateVolume`:**

```
if params.volumeClass == TPV:
    candidates = mgmt.list_cdvs(name_regex=params.cdvNameRegex or f"^{params.cdvName}$",
                                zone=pick_zone(req.accessibility_requirements))
    cdv = pool_selector(candidates)          # §14.3
    if cdv is None:
        raise CsiError(ResourceExhausted, "no eligible CDV matches pool")
    validate_extent_size(params.tpvExtentSizeKB, cdv.cdvConfig.cdvExtentSizeMiB)
    tpv = mgmt.create_tpv(name=req.name,
                          cdv_uuid=cdv._id,
                          virtual_size_gb=ceil_gib(req.capacity_range),
                          tpv_extent_kb=params.tpvExtentSizeKB or cfg.default)
    ctx = {"volumeClass": "TPV", "cdvUuid": cdv._id, "cdvName": cdv.name, ...}
    return Volume(..., accessible_topology=[{zone: cdv.zone}], volume_context=ctx)
else:
    # existing path, unchanged
```

**`ControllerPublishVolume`:** no new management call. Pre-check `tpvConfig.exclusiveClient` and fail-fast with CSI `FAILED_PRECONDITION` if already claimed by a different node. The existing `/clients/attach` call is unchanged — management's `attachTPV()` (`modules/client.js`) internally performs the two-phase CDV-hidden + TPV-exclusive attach. **Do not** have the CSI driver attach the CDV directly; that path is reserved for `cdvTomaAutoAttach.js` and `/clients/attach` rejects CDVs from external callers.

**`ControllerUnpublishVolume`:** unchanged. Management's `detachTPV()` handles CDV ref-count decrement and conditional CDV detach.

**`ControllerExpandVolume`:** branch to `POST /volumes/tpv/extend` for `volumeClass == TPV`. Virtual-only expansion; no physical provisioning. Management enforces the upper bound (TPV virtual size cannot exceed parent CDV capacity).

**`DeleteVolume`:** branch to `POST /volumes/tpv/delete` for TPVs. Management enforces "must be detached"; the driver surfaces any resulting error as CSI `FailedPrecondition`.

### 14.6 Node RPC Changes

None. A TPV presents as an ordinary NVMesh block device after attach; mount, format, `NodeGetVolumeStats`, and `NodeExpandVolume` all work unchanged. Filesystem resize after `ControllerExpandVolume` exercises the existing `NodeExpandVolume` path — which re-reads the block device size from sysfs — and is expected to work because the kernel client's `extendTPV` updates the gendisk capacity in-place.

### 14.7 Capacity Warnings Surfaced to Kubernetes — *planned*

**Not yet shipped.** The CSI driver cannot subscribe to the Kafka `CDVCapacityWarning` topic directly. The planned approach is to extend the driver's existing management-WebSocket client (`mgmt_websocket_client.py`) — which already receives CDV state updates — to watch the `capacityWarning` field and, when set, emit a Kubernetes `Event` of type `Warning` on each PVC whose `volume_context.cdvUuid` points at the affected CDV. No new RPC surface; reuses the `kubernetes` client already imported by the driver. No grep match for `capacityWarning` or `CDVCapacityWarning` in the driver today (as of 2026-04-19) — this remains open.

### 14.8 Immutability and ModifyVolume — *deferred*

**Not yet shipped.** `ControllerModifyVolume` (CSI 1.10+) is not implemented in `controller_service.py`. When added, the intent is: accept only `description`; every `tpvConfig` field is immutable and must be rejected with `InvalidArgument`; `cdvConfig` is fully immutable. Tracking this until the CSI spec version used by the driver's protos catches up and Kubernetes consumers start issuing `ControllerModifyVolume` RPCs in practice.

### 14.9 Known Pitfalls

- **Reference-ID namespace:** do not generate `tpv:<uuid>` reference IDs in the CSI driver. Management constructs those internally in `attachTPV()`; the driver passes its own CSI reference ID exactly as it does for regular volumes.
- **CDV auto-managed attach:** management's `/clients/attach` rejects direct CDV attach requests (`volumeClass === 'CDV'` → error). The CSI driver must never issue one — that path belongs to `cdvTomaAutoAttach.js` only.
- **Extent-size compatibility:** `tpvExtentSizeKB` must be a power of two and ≤ `cdvExtentSizeMiB × 1024`. Validate client-side to produce a clean CSI `InvalidArgument` rather than a generic 500 from management.
- **Access-mode coercion:** some CSI consumers pass `MULTI_NODE_READER_ONLY` for read workloads. Do not silently downgrade — reject, because a second reader would violate `exclusiveClient` and succeed only sporadically depending on management race windows.
- **Pool empty result:** zero candidates after filtering → `ResourceExhausted` (retriable by k8s), not `FailedPrecondition`. Admin action (extending the pool) resolves it without manifest changes.
- **WebSocket vs. REST consistency:** the pool selector reads CDV state via REST; the capacity-warning watcher reads via WebSocket. Treat WebSocket as the source of truth for `capacityWarning` (more timely) and REST for `tpvCount` (authoritative counter). Do not cross-reconcile on every `CreateVolume` — the small race window is harmless because management re-validates `tpvCount < maxTPVs` server-side.

---

## Section 15 — Post-MVP Features

This section collects well-defined enhancements deferred from the initial TPV release. Each item is scoped, has a known implementation path, and does not require architectural changes to the MVP design.

