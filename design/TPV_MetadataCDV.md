# TPV Metadata CDV — Minor Design

> **Status: design proposal.** Extends `TPV_ThinProvisioningImplementation.md`. Anything not called out here is inherited from that document unchanged. For the MVP, the data/metadata split is a **one-way choice made at TPV create time** — a single-CDV TPV cannot be converted to split later, and a split TPV cannot be merged.

---

## 1. Goal

Let a TPV store its user data on one CDV and its L1/L2 mapping tree on a second CDV.

**Why.** The TPV metadata path is dominated by small (4 KiB) partial-page writes to L1/L2 leaves (§3.4.3). That pattern runs well on mirror-class volumes (RAID1 / 3-way mirror) and poorly on EC: EC partial-stripe writes require read-modify-write of the parity blocks, multiplying RDMA ops and latency. Putting metadata on a mirror and data on EC lets the admin pick the right storage class for each workload shape:

- **Data CDV:** large extents, cost-optimised, EC.
- **Metadata CDV:** small extents, latency-optimised, mirrored.

On the allocation hot path — a write to an unmapped virtual extent — the kernel must allocate an L2 slot (if the L1 pointer is null) and install a leaf before the data write completes (sync-flush mode, §3.4). Today both operations hit the same CDV; on an EC CDV, the leaf flush is the long pole. Splitting lets the leaf flush run at mirror latency while the data write runs on EC.

This does not change the TPV on-wire or in-memory abstraction. A TPV still presents as one virtual block device to the client, still has a single `tpvConfig.exclusiveClient`, and is still an EXCLUSIVE_READ_WRITE attachment. Only the physical placement of the mapping tree changes.

---

## 2. Building blocks reused

The CDV is the only storage primitive. **No new `volumeClass`**, no new TOMA allocator variant. A metadata CDV is an ordinary `volumeClass: 'CDV'`; what makes it a "metadata CDV" is that one or more TPVs list it in `tpvConfig.metaCdvId`. Admins pick RAID level and `cdvExtentSizeMib` to suit the role — a mirror-backed CDV with a small extent size is recommended for metadata, but not enforced.

Everything the main design specifies at and below the CDV level — satellite allocator volume (§1.5), RAFT-elected allocator identity (§2.6), `cdv_extent_md` ownership tracking, `cdvTomaAutoAttach`, `CDVAllocatorFreeAll`, `cdvCapacityWarning`, per-CDV admission floor (§2.10) — applies to each CDV independently. A split-mode TPV participates in two independent CDV lifecycles.

---

## 3. Data model

### 3.1 CDV document — unchanged

The CDV schema gains nothing. A CDV does not know how many TPVs use it for data vs. metadata; `tpvCount` counts every TPV that references it in either role. The one knob admins may want to tune differently for metadata CDVs is `cdvConfig.cdvExtentSizeMib` — smaller extents (e.g. 64 MiB) waste less capacity when the metadata footprint is well below one extent. No schema work needed for that; the existing 64 MiB–64 GiB power-of-2 range already covers it.

### 3.2 TPV `tpvConfig` — three fields become six

```js
tpvConfig: {
    // --- Data side (existing three, renamed for clarity) ---
    dataCdvId:             { type: String, required: true },
    dataCdvUUID:           { type: String, required: true },
    dataTpvExtentSizeKB:   { type: Number, required: true },   // power-of-2, 64–65536 KB

    // --- Metadata side (new three; absent in single-CDV mode) ---
    metaCdvId:             { type: String, default: null },
    metaCdvUUID:           { type: String, default: null },
    metaTpvExtentSizeKB:   { type: Number, default: null },    // power-of-2, 64–65536 KB

    // --- Virtual geometry ---
    virtualSizeGB:         { type: Number, required: true },
    metaVirtualSizeGB:     { type: Number, default: null },    // split mode only; auto-computed
    maxVirtualSizeGB:      { type: Number, default: 1000 },

    // --- Attachment state (unchanged) ---
    exclusiveClient:       { type: String, default: null },
    exclusiveClientUUID:   { type: String, default: null },
}
```

`metaCdvId === null` is the single-CDV mode discriminator. When null, the TPV behaves exactly as the base design: L1 in slot 0 of its first data CDV extent, L2 tables inline with data slots.

**Legacy-name note.** The existing fields `cdvId`, `cdvUUID`, `tpvExtentSizeKB` are renamed to the `data…` variants. Because TPV is not yet GA (§3.4.2), dev volumes are reformatted; no Mongo migration.

**Pairing rules (MVP — one-way only).**
- `(dataCdvId, metaCdvId)` is fixed at create time and immutable.
- `dataCdvId === metaCdvId` is rejected — if the admin wants both on one CDV, they pick single-CDV mode.
- Cannot add a metaCdv to an existing single-CDV TPV. Cannot remove the metaCdv from a split TPV.
- Either CDV may be shared across many TPVs (bounded by that CDV's own `maxTPVs`). `tpvCount` on a shared metadata CDV counts TPVs that use it for metadata.

### 3.3 Metadata capacity — auto-sized, hidden from the create dialog

Formula (in `modules/volume.js`, applied on create and on every `extendTPV`):

```
raw_L2_bytes   = ceil(virtualSizeGB × 2^20 / dataTpvExtentSizeKB) × 8
raw_L1_bytes   = ceil(raw_L2_bytes / metaTpvExtentSizeKB / 2^10) × 8  +  sizeof(tpv_l1_header)
raw_total_B    = raw_L1_bytes + raw_L2_bytes
safety_B       = ceil(raw_total_B × 1.10)                        // 10% fragmentation/headroom
metaVirtualSizeGB = max(1, ceil(safety_B / 2^30))                // 1 GiB allocation granularity
```

`1 GiB` is the NVMesh volume allocation quantum (`MIN_VOLUME_CAPACITY = 1` in `nvmesh-management/utils.js`; all volume capacities are integer GiB). Rounding up to that matches how every other volume in the system is sized.

**Examples.**
| `virtualSizeGB` | `dataTpvExtentSizeKB` | raw L2 | +10% + round → `metaVirtualSizeGB` |
|---:|---:|---:|---:|
| 128 | 64 | 16 MiB | 1 GiB |
| 1 024 | 64 | 128 MiB | 1 GiB |
| 10 240 | 64 | 1.28 GiB | 2 GiB |
| 102 400 | 64 | 12.8 GiB | 15 GiB |
| 1 024 | 1 024 | 8 MiB | 1 GiB |

**Expand path.** `extendTPV({tpvId, newSizeGB})` recomputes `metaVirtualSizeGB` from the new `virtualSizeGB`. If the result exceeds the current `metaVirtualSizeGB`:

1. Extend the metadata TPV first by calling the same underlying extend logic on the meta side. If that fails (e.g. metadata CDV exhausted), the TPV extend aborts with no change. Metadata-before-data ordering guarantees we never grow the virtual address space beyond what the tree can describe.
2. Then extend the data side (existing logic).
3. Update `tpvConfig.virtualSizeGB` + `metaVirtualSizeGB`, then send one `UpdateVolume` MCS to the client carrying both new sizes.

Capacity on the metadata CDV is checked the same way as the data CDV — `createTPV` / `extendTPV` both compare against the CDV's free capacity and `maxTPVs`.

---

## 4. Create / attach / detach / delete

All flows are straight extensions of §1.4. The delta is that every operation that touched "the CDV" now touches either "the data CDV" or "both CDVs".

### 4.1 Create

`POST /volumes/save` with `volumeClass: 'TPV'`. `createVolume()` branches on whether `tpvConfig.metaCdvId` is present:

- **Single-CDV mode** (`metaCdvId` absent): exactly as today.
- **Split mode**:
  1. Validate both CDVs exist, are online, each has `tpvCount < maxTPVs`, and `dataCdvId !== metaCdvId`.
  2. Validate `dataTpvExtentSizeKB ≤ dataCdv.cdvExtentSizeMib × 1024` and `metaTpvExtentSizeKB ≤ metaCdv.cdvExtentSizeMib × 1024`.
  3. Compute `metaVirtualSizeGB` per §3.3; check `virtualSizeGB ≤ dataCdv.capacity` and `metaVirtualSizeGB ≤ metaCdv.capacity`.
  4. Insert TPV record. Atomically `$inc` `tpvCount` on both CDVs.
  5. No Kafka traffic for CDV changes — the TPV is not yet attached.

### 4.2 Attach — both CDVs hidden-attached

`client.js:attachTPV` (§1.4) gains a second CDV hidden-attach step when `tpvConfig.metaCdvId` is set:

1. Ensure the data CDV is attached on all its TOMA candidates (existing path, drives allocator election on the data CDV).
2. Ensure the metadata CDV is attached on all its TOMA candidates.
3. Hidden-attach the data CDV to the client (`isHidden: true`, `SHARED_READ_WRITE`, `tpv:<tpvUUID>` referenceID). Wait for `block_devices` confirmation.
4. Hidden-attach the metadata CDV to the client (same attributes, same `tpv:<tpvUUID>` referenceID, different attachment record).
5. Send `AttachVolumes` for the TPV with `EXCLUSIVE_READ_WRITE`, both `dataCdvConf` and `metaCdvConf` inline (see §5.1).
6. Set `tpvConfig.exclusiveClient = clientID`.

The `tpv:<tpvUUID>` referenceID is reused on both CDV attachment records — the same id, with the same meaning: "this client has TPV `<tpvUUID>` attached, and therefore needs this CDV". Detach removes the id from both records. The existing "detach the CDV only when no `tpv:*` and no `toma:*` ref remain" rule applies per CDV independently.

### 4.3 Detach (voluntary and involuntary)

Mirror of attach. `detachTPV` and `cleanupTPVReferencesForDetachedClient` remove the `tpv:<tpvUUID>` ref from **both** CDV attachments and conditionally hidden-detach each CDV if its own ref list is empty.

The "CDV cleanup on involuntary detach" invariant (§1.4) applies to both CDVs: a client that loses access to a TPV must lose access to both CDVs backing it. A TPV whose metadata CDV is detached but whose data CDV is not is as corrupting as the reverse — either half can host stale RDMA writes.

### 4.4 Delete

`deleteTPVs` sends **two** `CDVAllocatorFreeAll` Kafka messages — one to each CDV's current allocator — carrying the same `tpvUUID`. Each allocator independently reclaims the extents it owns for that TPV. `tpvCount` is decremented on both CDVs. Order does not matter; each `CDVAllocatorFreeAll` is idempotent at the TOMA side.

### 4.5 Eviction (per-CDV preemption, `TPV_PerClientCDVPreemption.md`)

Per-CDV preemption is an **eviction of a client from a CDV**. For a split-mode TPV, the TPV is unusable without either CDV. Therefore:

- **Evicting a client from the data CDV** tears down every TPV whose `dataCdvId` points at that CDV on that client (existing rule, unchanged).
- **Evicting a client from the metadata CDV** tears down every TPV whose `metaCdvId` points at that CDV on that client (new rule).
- The management-side "evict TPV X from client A" helper (force-detach) translates into **two** `preemptClientFromCDV` fan-outs, one per CDV. Order: metadata CDV first, then data CDV. The metadata-first order prevents the window in which the client briefly has its data CDV torn down but still holds the metadata CDV and could issue a stray tree write.

No new kernel hook — `nvmeibc_tpv_handle_cdv_preempted` (§3.8.1) already walks the per-CDV TPV list; registering each TPV on *two* CDV lists means either preempt trigger finds it and calls `nvmeibc_tpv_detach`. Idempotency of `nvmeibc_tpv_detach` (also a §3.8 requirement) covers the case where both preempts fire.

---

## 5. MCS / Kafka extensions

### 5.1 `AttachVolumes` for split-mode TPV

Today the TPV `AttachVolumes` payload inlines one `cdvConf`. For split mode, add a sibling `metaCdvConf`:

```js
{
  volumeClass: 'TPV',
  tpvConfig:     { ...full tpvConfig with six fields... },
  dataCdvConf:   { uuid, name, chunks, ... },     // was cdvConf
  metaCdvConf:   { uuid, name, chunks, ... } || null,
}
```

`cdvConf` is renamed to `dataCdvConf` for symmetry (kernel reads both). In single-CDV mode, `metaCdvConf` is null and the kernel falls back to in-data L1/L2 placement.

### 5.2 CM codec (binary attach record)

The main design repurposes `mdvUUID` to carry the (single) parent CDV UUID without adding to the kernel ABI. Split mode needs a **second** CDV UUID.

**Decision: add a new CM field, `metaCdvUUID`.** Cleaner than stacking more repurposed fields on top of the existing TPV overloading, and it documents intent on the wire. Ships with a matching kernel version gate — older kernels refuse to attach a split-mode TPV.

Other split-mode geometry (`metaTpvExtentSizeKB`, `metaVirtualSizeGB`) piggybacks on the inline `metaCdvConf` blob — no CM-level change needed since it is a JSON sidecar already.

### 5.3 `CDVAllocatorFreeAll` — unchanged per message

No schema change. Two messages are sent per split-mode TPV delete, each carrying the correct `(cdvUUID, tpvUUID, allocatorSizeGib, cdvExtentSizeMib)` for its CDV.

---

## 6. Kernel changes — delta from §3

### 6.1 Two allocators per TPV, two `cdv_alloc_work` instances

`struct nvmeibc_tpv_allocator` today has one `cdv_vol` reference and one free-slot pool — both for the single CDV. In split mode, the TPV owns:

- `data_alloc` — the existing allocator, scoped to the data CDV. Serves all data-extent allocations. No L1/L2 anywhere on this side.
- `meta_alloc` — a second allocator instance, scoped to the metadata CDV. Serves L2-table allocations only. **All extents on the metadata CDV are L2 tables; none hold data.**

Each allocator has its own `free_tpv_extents`, `cdv_extent_list`, `cdv_alloc_work`, `low_watermark`, and pending-return list. The pre-fetch cadence can differ — the metadata side is driven by how fast new L1 indices are touched, not by user I/O rate.

The L1 table lives in slot 0 of the **first metadata CDV extent** ever allocated to the TPV. This is the natural generalisation of §3.4.1 — the "L1 extent" becomes the first metadata-side extent; on the data side, slot 0 of the first extent is now just a regular data slot. `is_l1_extent` lives on the metadata-side `cdv_extent_ref`.

### 6.2 IO path — `make_request`

```c
// virt_idx → (L1_idx, L2_idx)  as today
// L1 and L2 reads go via meta_alloc (reads cached in-memory via xarray; 4 KiB
// dirty-page writes targeted at the metadata CDV)
// data read/write targets phys_offset computed from L2[L2_idx], which is a
// byte offset on the DATA CDV
phys_offset_data = L2[L2_idx].cdv_offset;   // byte offset on data_alloc->cdv_vol
nvmeibc_cdv_submit_bio(data_alloc->cdv_vol, phys_offset_data, bio);
```

The semantics of `tpv_tree_entry.cdv_offset` (§3.4.2) need a one-bit tag or a convention:

- **L1 entries** point at L2 tables, which live on the **metadata CDV**. Offset is relative to the metadata CDV.
- **L2 leaves** point at data slots, which live on the **data CDV**. Offset is relative to the data CDV.

The existing decoding math `extent_idx = (cdv_offset − A) / E + 1` stays correct as long as the kernel knows which CDV each offset refers to — which is implied by the level in the tree (L1 entry → metadata CDV geometry; L2 leaf → data CDV geometry). No on-disk format bit is needed.

### 6.3 `flush_state`, `load_state`, partial-page flush (§3.4.3)

All L1/L2 tree writes redirect to the metadata CDV. The 4 KiB dirty-page bitmaps are unchanged in concept; they just target a different block device. `load_state` reads from the metadata CDV's first allocated extent. `cdv_sync_read/write` helpers take a `struct nvmeibc_volume *cdv` parameter already, so the fan-out is a local change.

### 6.4 Recovery (`nvmeibc_tpv_recovery`)

Querying TOMA for "extents owned by this TPV" becomes two queries — one per CDV allocator — via `CDV_LIST_EXTENTS`. Orphan reconciliation runs independently on each side:

- Data-side orphan: in TOMA, not in any L2 leaf. Reclaim.
- Metadata-side orphan: in TOMA, not referenced by L1 and not the current L1 host. Reclaim.

### 6.5 `/proc` entries

`/proc/nvmeibc/tpv/<name>/` gains:

- `data_allocator` and `meta_allocator` replacing today's single `allocator` file when split.
- `status` reflects both allocators' TOMA IDs and generations.
- `extent_map` unchanged (logical virt → data-phys mapping; that's what the user wants to see).

### 6.6 Self-tests (`nvmeibc_tpv_test.c`)

Add one split-mode case per existing test (alloc/free, persist, exhaustion, double-free, recovery), using two stub CDV buffers. Table-driven is sufficient — the logic is the same, only the target CDV differs.

---

## 7. CSI driver

`nvmesh-csi-driver` StorageClass parameters extend symmetrically:

| Parameter | Purpose |
|---|---|
| `cdvName` / `cdvNameRegex` | Data CDV selection (existing). |
| `metaCdvName` / `metaCdvNameRegex` | Metadata CDV selection (new). Absent → single-CDV mode. |
| `dataTpvExtentSizeKB` | (renamed from `tpvExtentSizeKB`; old name accepted for back-compat in the driver). |
| `metaTpvExtentSizeKB` | Metadata extent size; optional, default from driver config. |

Pool selection runs the §14.3 algorithm **twice**, independently — once per regex. Both must return at least one eligible CDV; if either pool is empty, the driver returns `ResourceExhausted` and the PVC stays pending.

Topology: both CDVs must be in a zone that satisfies `accessibility_requirements`. If they are in different zones, the intersection is used; empty intersection → `ResourceExhausted`.

The CSI driver does **not** compute `metaVirtualSizeGB`. It passes `virtualSizeGB`, `dataTpvExtentSizeKB`, `metaTpvExtentSizeKB`, `dataCdvId`, `metaCdvId` to `POST /volumes/save`; management auto-computes the metadata size.

`ControllerExpandVolume` is unchanged on the wire — it still posts `newSizeGB` to `/volumes/tpv/extend`. Management handles the metadata-side extend.

`DeleteVolume`, `ControllerPublish/Unpublish` — no CSI-side change; the two-CDV orchestration is entirely server-side.

---

## 8. CLI (`nvmesh-infra/xlro/tools/cli`)

The NVMesh-CLI is rest-driven through `rest.yaml` (see CLAUDE.md). Split-mode TPVs need:

- **`rest.yaml` — `TPV` entity `ops.create.params`** extended with `metaCdvId`, `metaTpvExtentSizeKB` (the existing `tpvConfig` sub-object recursion picks up the new fields automatically once `TPVConfig` declares them). `virtualSizeGB` is still the sole user-facing size input.
- **`TPVConfig` SdkObject** (`xlro/core/entities/`) gains the three new fields, matching the Mongo schema camelCase exactly (`metaCdvId`, `metaTpvExtentSizeKB`, `metaVirtualSizeGB`) — the last is server-populated and read-only on display.
- **`TPV` entity `display`** gains `metaCdvName` (via a `$lookup` alongside the existing `cdvName` lookup for data) and the two extent sizes.
- **Golden files** `current.api` and `current.display` regenerated to reflect the new flags and display columns.
- **`rest_custom.py`** — no new `TPVGroup` override needed; the auto-generated `create` form handles the new optional params cleanly. If both `--tpv-config-meta-cdv-id` and `--tpv-config-data-cdv-id` are omitted, the CLI errors with the standard missing-required-param message for `dataCdvId`.
- **`CDV` entity** unchanged — the CLI does not need to know a CDV's intended role.

Example invocation:

```
nvmesh tpv create myTPV \
  --capacity 1TiB \
  --tpv-config-data-cdv-id data-pool-01 \
  --tpv-config-data-tpv-extent-size-k-b 64 \
  --tpv-config-meta-cdv-id meta-pool-01 \
  --tpv-config-meta-tpv-extent-size-k-b 64
```

`nvmesh tpv show` renders the six fields; `nvmesh tpv extend` is unchanged (virtual-size-only).

---

## 9. UI

Create TPV dialog (`CreateTPVModal.jsx`) grows a single toggle: **"Split data and metadata"** (off by default). When off, the dialog is identical to today.

When on:

- The existing CDV selector is relabelled **"Data CDV"**.
- A second CDV selector appears: **"Metadata CDV"** (filters out the CDV selected as data; recommends mirror-class CDVs via an info tooltip, does not enforce).
- A second extent size appears: **"Metadata extent size (KB)"**, same 64–65536 power-of-2 range.
- `metaVirtualSizeGB` is **not** shown. The computed value is displayed as a read-only info line ("metadata capacity: X GiB, auto-sized") beneath the metadata CDV picker.
- On submit, if the computed metadata capacity would exceed the metadata CDV's free capacity, the dialog shows the standard "no capacity" error before the server call.

TPV list page (`ThinProvisioning.jsx`) gains two columns: **Data CDV**, **Metadata CDV** (the latter renders as "—" for single-CDV TPVs). Column set is otherwise unchanged.

---

## 10. Encryption and zero-on-free

Per-TPV LUKS (`TPV_EncryptionPlan.md`) wraps the **data path only** — writes go through the LUKS block layer, which sits above the TPV gendisk and ultimately targets data-CDV physical offsets. L1/L2 tree writes are mapping pointers — they carry no user plaintext and no ciphertext. **The metadata CDV stays plaintext regardless of `isEncrypted`**. This is by design and mirrors the rationale for leaving the `<CDV>-mgmt` satellite plaintext (§1.5.4, architecture decision 18).

`cdv_extent_zero_on_free` (off by default; §3.9) is similarly irrelevant on the metadata CDV: the records are pointer tables, not user data, and a freshly reallocated L2 slot is fully overwritten by the first leaf install. **Enabling the flag on a metadata CDV would be a pure latency cost with no confidentiality gain.** If the flag is configured cluster-wide, its effect on the metadata CDV is harmless but wasteful. A future tuning knob could gate it per-CDV; not for this MVP.

---

## 11. Scope and non-goals

**In scope (MVP).**

- Create-time choice between single-CDV and split-CDV TPV.
- Auto-sized metadata capacity, auto-grown on TPV extend.
- Per-CDV lifecycle reuse: attach, detach, delete, eviction, capacity warning, allocator election — all from the main design — apply to each CDV independently.
- CSI driver split-pool support (two regexes).

**Out of scope.**

- Converting between single-CDV and split-CDV after create. One-way choice at create time; field `metaCdvId` is immutable.
- Moving a TPV's metadata from one CDV to another online.
- Mixing a TPV's metadata across multiple metadata CDVs. One data CDV, one metadata CDV, period.
- Placing L2 tables on the data CDV and L1 on the metadata CDV (or any hybrid). Metadata CDV holds **all** L1/L2.
- Changing `metaTpvExtentSizeKB` post-create.

---

## 12. Resolved design choices

- **10% safety multiplier on `metaVirtualSizeGB` — kept.** It's virtual capacity only; no physical cost until extents are actually allocated. Generous headroom is free.
- **New CM field `metaCdvUUID`** — added rather than repurposed. Cleaner than further overloading existing fields; kernel version-gated.
- **No new CDV annotation field.** The existing `description` field on the CDV is sufficient for admins to tag a CDV as "metadata" or "data". UI and CLI surface `description` already.

---

## 13. Implementation plan

High-level ordering; each bullet is a self-contained commit or small series.

**Phase 1 — management schema + create path**

- Rename `tpvConfig.{cdvId,cdvUUID,tpvExtentSizeKB}` to the `data…` variants; update all readers.
- Add `tpvConfig.{metaCdvId,metaCdvUUID,metaTpvExtentSizeKB,metaVirtualSizeGB}`.
- Add auto-sizing helper + extend-time recompute in `modules/volume.js`.
- Reject `dataCdvId === metaCdvId`; enforce immutability of the split choice.
- Unit tests for auto-sizing formula, capacity checks, pairing rules.

**Phase 2 — MCS / Kafka wire**

- New CM field `metaCdvUUID` in `VolumeMessage.js:preparePayload`.
- Rename `cdvConf` to `dataCdvConf`; add sibling `metaCdvConf` in `AttachVolumes.js`.
- Kernel version gate: old kernels reject split-mode attaches.

**Phase 3 — attach / detach / delete orchestration**

- `client.js:attachTPV` extended to hidden-attach both CDVs (metadata-first) and stamp `tpv:<tpvUUID>` on both attachment records.
- `detachTPV` and all involuntary-detach paths clean both CDV attachments.
- `deleteTPVs` sends two `CDVAllocatorFreeAll` messages and decrements both `tpvCount`s.

**Phase 4 — kernel client**

- Split `struct nvmeibc_tpv_allocator` into `data_alloc` + `meta_alloc`; second `cdv_alloc_work`, second pending-return list.
- Route L1/L2 reads/writes through `meta_alloc`; data bios through `data_alloc`.
- Adapt `flush_state` / `load_state` / partial-page flush / recovery to target the metadata CDV.
- Update `/proc` entries and self-tests.

**Phase 5 — eviction**

- Register split-mode TPVs on both CDVs' per-CDV TPV lists so `nvmeibc_tpv_handle_cdv_preempted` fires from either side.
- Management `preemptClientFromCDV` helper: when force-evicting a TPV, fan out to both CDVs (metadata first).

**Phase 6 — CSI driver**

- StorageClass params: `metaCdvName`, `metaCdvNameRegex`, `metaTpvExtentSizeKB`.
- Pool selection run twice; zone intersection enforced.
- Rename `tpvExtentSizeKB` → `dataTpvExtentSizeKB` (accept old name for one release).

**Phase 7 — CLI**

- `rest.yaml` + `TPVConfig` SdkObject updates for the new fields.
- Regenerate `current.api` and `current.display` golden files.
- Add CLI integration test for split-mode create / show / delete.

**Phase 8 — UI**

- "Split data and metadata" toggle in `CreateTPVModal.jsx`.
- Second CDV picker + metadata extent size input; auto-sized capacity read-out.
- New columns in `ThinProvisioning.jsx` TPV list.

**Phase 9 — docs + release**

- Update the main `TPV_ThinProvisioningImplementation.md` to cross-reference this doc and note the `tpvConfig` field rename.
- Release-notes entry covering the mNDU consideration: split-mode TPVs require Phase 2's new kernel; plan upgrade order accordingly.
