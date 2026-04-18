# TPV — Per-Client CDV Preemption: Execution Plan

**Design reference:** `TPV_ThinProvisioningImplementation.md` §2.10 is authoritative. This document is the phased execution plan derived from it. If the two disagree, §2.10 wins and this plan is updated.

**Governing shape:** two orthogonal primitives.

- **Primitive A — CDV admission floor.** A monotonic `u64` per CDV, replicated from management to every TOMA. Gates **new** `REGISTER`s at registration time; leaves in-flight registrants untouched.
- **Primitive B — targeted registrant termination.** A new Kafka message `ManagementToTOMA.preemptClientFromCDV(clientID, cdvUUID, newFloor)`. TOMA raises the floor first, then terminates the named client's `reg_ctx` on every local CDV segment.

Combined, these fence one client from a `SHARED_READ_WRITE` CDV without disturbing the other SHARED holders and without touching the I/O hot path. Everything else — the earlier P1/P2/P3 triad, per-TPV cookies, `ForceDetachTPV` as a distinct primitive, `RevokeClientFromVolume` as a parallel admission concept — is superseded (§2.10.7).

---

## Phase map

| Phase | Steps | Scope |
|---|---|---|
| 1 — Management schema & attach-path stamping | 1–3 | `cdvConfig.admissionFloor` on the CDV doc, `attachTPV` stamps `reservationModeVersion`, `volumeAttachmentActions.EVICTING` |
| 2 — Kafka plumbing                          | 4–6 | New `PreemptClientFromCDV` + response messages, ACK aggregation, Kafka-router wiring |
| 3 — TOMA admission floor & handler          | 7–10 | Eager per-CDV state, floor seeding, `preemptClientFromCDV` handler, new `REGISTER` predicate + reason code |
| 4 — Client kernel cleanup barrier           | 11–13 | Propagate `reservation_mode_version` on CDV attach, teardown TPVs on `NCBD_PREEMPTED`, handle `BELOW_CDV_FLOOR` |
| 5 — Management preempt flow                 | 14–16 | `preemptClientFromCDV(cdv, client)`, hook into force-detach + stale-client cleanup, attach-gate during `EVICTING` |
| 6 — mNDU + CLI + CSI surface                | 17–19 | interop-db gate, `nvmesh client preempt-from-cdv`, CSI no-op audit |
| 7 — Testing & stabilization                 | 20–23 | Unit, integration, adversarial, failover; feature-flag flip |

Phases 1–3 can run largely in parallel (different sub-repos). Phase 4 depends on Phase 3's reason code. Phase 5 depends on Phases 1 and 2. Phase 6 depends on Phase 5. Phase 7 depends on everything.

---

## Phase 1 — Management schema and attach-path stamping

### Step 1. CDV schema — `nvmesh-management/models/volume.js`, `validationSchemes/definitions/volume.js`

- Add `cdvConfig.admissionFloor: { type: Number, default: 0, min: 0 }`.
- Immutable via user REST paths. The existing `updateCDV` path (in `modules/volume.js`, routed through `updateVolumes`) already silently ignores `cdvConfig` on update — verify, and extend the schema validator with an explicit deny for `admissionFloor` to produce a clean 400 instead of a silent drop.
- `prepareCDVForCreate` sets `admissionFloor: 0` on insert.
- `0` is the sentinel "no gate" — pre-feature CDVs have no field, which Mongoose coerces to `0` on read, matching pre-feature behavior.

### Step 2. Attach-path stamping — `nvmesh-management/modules/client.js:attachTPV` (line 5049)

On the two-phase attach, the hidden CDV `AttachVolumes` payload must carry `reservationModeVersion = cdv.cdvConfig.admissionFloor`:

```js
// Phase 1 of attachTPV: hidden CDV attach
const attachCDVPayload = {
    // … existing fields …
    reservationModeVersion: cdv.cdvConfig.admissionFloor || 0,
};
```

Verify that the `AttachVolumes` Kafka message already has a `reservationModeVersion` slot on the per-volume entry (it does — used today for existing preempt flow). If absent on the CDV branch specifically, extend the message builder.

The TPV (phase 2) entry is unaffected — TPVs are not CDV segments, they have no admission floor of their own.

### Step 3. `EVICTING` state — `nvmesh-management/consts.js`

```js
consts.volumeAttachmentActions = {
    ATTACHING:       'attaching',
    DETACHING:       'detaching',
    REATTACHING:     're-attaching',
    DETACHING_STALE: 'detaching-stale',
    EVICTING:        'evicting',          // NEW
};
```

Audit all switch/case and string-compare sites on `volumeAttachmentActions` and ensure the new state is handled (typically: fall-through to a safe default that refuses user-initiated work while the state is set). Places to check:

- `client.js:attachTPV` — attach-gate (Step 15).
- `client.js:detach*` — evicting + detach is a no-op (detach runs as part of the eviction).
- `routes/clients.js` — any admin REST path that reports attachment state must surface `evicting`.
- UI services that filter by `action` — add the new value; `Clients.jsx` / `Volumes.jsx` attachment lists should display "Evicting from CDV".

---

## Phase 2 — Kafka plumbing

### Step 4. New message definitions — `nvmesh-management/models/kafkaMessages/`

`PreemptClientFromCDV.js`:

```js
class PreemptClientFromCDV {
    constructor({ clientID, clientUUID, cdvID, cdvUUID, newFloor }) { … }
    toJSON() {
        return {
            messageType: 'preemptClientFromCDV',
            clientID, clientUUID,
            cdvID, cdvUUID,
            newFloor,
        };
    }
}
```

`PreemptClientFromCDVResponse.js` — parse-only (management consumes):

```js
class PreemptClientFromCDVResponse {
    static parse(payload) {
        return {
            tomaID:    payload.tomaID,
            cdvUUID:   payload.cdvUUID,
            clientID:  payload.clientID,
            newFloor:  payload.newFloor,
            success:   !!payload.success,
            error:     payload.error || null,
            terminatedRegistrants: payload.terminatedRegistrants || 0,
        };
    }
}
```

### Step 5. ACK aggregation — `nvmesh-management/modules/client.js`

Reuse the fan-in pattern in `sendReservationModeChangeMessageToAllTargets` (line 1511): send one message per TOMA serving `cdvC`, wait for all responses with a bounded timeout (`Config.preemptAckTimeout`, default 30 s; add if missing).

- Partial-failure policy: if any TOMA fails to ACK within the timeout, the preempt is **not** marked complete. Management retries the Kafka publish to the unresponsive TOMAs with exponential backoff (1 s, 2 s, 4 s, 8 s, 16 s). The handler is idempotent (see Step 9), so replay is safe.
- After 5 retries, the operation fails with `CDV_PREEMPT_TOMA_UNRESPONSIVE`; the `EVICTING` state remains set so a follow-up retry from an operator or a restarted management instance can resume.

### Step 6. Kafka router — `nvmesh-management/kafkaRouter.js`

Route `preemptClientFromCDVResponse` to the new consumer in `client.js:handlePreemptResponse(parsed, ack)`. Pattern-match on existing consumer registrations.

---

## Phase 3 — TOMA admission floor and handler

### Step 7. Per-CDV state location — `nvmesh-kernel/toma/nvmeibt_cdv_alloc.h`, `nvmeibt_cdv_alloc.c`

Extend `struct nvmeibt_cdv_alloc` with:

```c
u64  admission_floor;          // NEW
bool admission_floor_seeded;   // NEW
```

Today `nvmeibt_cdv_alloc` is created lazily on the first `CDV_ALLOC_EXTENT`. Change to **eager** creation on CDV topology arrival (the path that today delivers `cdv_extent_size_mb`, `allocator_size_gb`, etc., for CDV bookkeeping). This places the admission floor in the same struct as the rest of the CDV's per-TOMA metadata.

- If eager creation turns out to be too invasive (e.g., the CDV-arrival path does not currently have a per-CDV hook on every TOMA, only on the allocator TOMA), fall back to a sibling hash `nvmeibt_cdv_state` keyed by `cdv_uuid`, populated from the same topology message. Cost: one extra hash lookup per `REGISTER` on a CDV segment. Prototype before committing.

### Step 8. Floor seeding on CDV topology arrival

Extend the CDV-metadata topology message (management → TOMA) with `admission_floor`. On receipt:

```c
cdv->admission_floor = msg.admission_floor;
cdv->admission_floor_seeded = true;
```

If the topology push arrives while `nvmeibt_cdv_alloc` does not yet exist (rare), allocate and seed atomically under the per-CDV lock.

### Step 9. `preemptClientFromCDV` handler — `nvmesh-kernel/toma/nvmeibt_kafka.c`

```c
static int handle_preempt_client_from_cdv(const char *msg_json)
{
    // parse clientID, cdvUUID, newFloor
    cdv = nvmeibt_cdv_alloc_lookup(cdv_uuid);
    if (!cdv) { ack(success=false, error="cdv_not_found"); return 0; }

    mutex_lock(&cdv->handler_lock);

    // Step 1 (ORDER MATTERS): raise floor FIRST.
    cdv->admission_floor = max_t(u64, cdv->admission_floor, newFloor);

    // Step 2: terminate registrants on every CDV segment for this client.
    for_each_seg_active_of_cdv(cdv, seg_active) {
        reg = nvmeibt_register_lookup_by_client(seg_active, clientID);
        if (reg) {
            nvmeibt_register_terminate_reg_ctx(reg, /*is_deleting_seg_active=*/false);
            terminated++;
        }
    }

    mutex_unlock(&cdv->handler_lock);

    // Drain happens inside terminate_reg_ctx's existing machinery.
    ack(success=true, terminatedRegistrants=terminated, newFloor=cdv->admission_floor);
    return 0;
}
```

Order (floor first, termination second) is a correctness invariant — see §2.10.4. Add a block comment on this function explaining why, and debug-assert that `admission_floor >= newFloor` at the end of step 1 before entering step 2.

**Idempotency:** a second message with the same or lower `newFloor` is a no-op on the floor (because `max`); `nvmeibt_register_lookup_by_client` returns NULL for an already-terminated client, so no double-termination.

### Step 10. New `REGISTER` predicate — `nvmesh-kernel/toma/nvmeibt_register.c`

In `handle_register_registrant_on_disk_segment` (line 2539), **before** the existing `is_valid_register_req` check:

```c
if (nvmeibt_seg_active_is_cdv(seg_active)) {
    u64 floor = nvmeibt_cdv_get_admission_floor(seg_active);
    if (incoming_reg_ctx->reservation_mode_version < floor) {
        reg_refusal_reason = NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR;
        goto reject;
    }
}
```

`nvmeibt_seg_active_is_cdv(seg_active)` is a one-line helper (`volume_class == CDV`) added to `nvmeibt_seg_active.h`. `nvmeibt_cdv_get_admission_floor(seg_active)` looks up the per-CDV state from the segment's parent CDV UUID.

Extend `enum NVMEIBT_CLIENT_TR_REASON` with `NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR`. Update `nvmeibt_client_tr_reason_to_string()` for log output.

The predicate runs on `REGISTER` only — not on any I/O path. The I/O hot path is unchanged.

---

## Phase 4 — Client kernel cleanup barrier

### Step 11. Propagate `reservation_mode_version` on CDV attach — `nvmesh-kernel/clnt/nvmeibc_main_capi_manipulate_vols.inc.c`

The MCS `AttachVolumes` payload already carries `reservationModeVersion` for regular-volume preempt. For the hidden CDV case (`volume_class == NVC_CDV && is_hidden == 1`, §3.2), parse and stash it so the subsequent `REGISTER` builder reads it from the CDV's `nvmeibc_volume`:

```c
// in the CDV attach branch
cdv_vol->reservation_mode_version = msg_vol->reservation_mode_version;
```

Then in the `REGISTER` header construction (the existing code at `toma/clnt/nvmeibt_client_protocol.h:389`), read from `cdv_vol->reservation_mode_version` instead of the volume-wide default.

### Step 12. CDV-preempted propagation to TPVs — `nvmesh-kernel/clnt/tpv/nvmeibc_tpv.c`

New function:

```c
void nvmeibc_tpv_handle_cdv_preempted(struct nvmeibc_volume *cdv)
{
    struct nvmeibc_tpv *tpv, *tmp;
    // list-walk under cdv_tpv_list_lock; nvmeibc_tpv_detach is async-safe.
    list_for_each_entry_safe(tpv, tmp, &cdv->tpv_list, cdv_tpv_link) {
        _NW(tpv_cdv_preempted, "Tearing down TPV @TPV_NAME due to CDV preempt",
            NTSTR("TPV_NAME", tpv->tpv_name));
        nvmeibc_tpv_detach(tpv);
    }
}
```

Hook it from the CDV block-device status transition in `clnt/nvmeibc_block.c:1079` (`case 'P':` handling):

```c
case 'P':
    // … existing code that sets NCBD_PREEMPTED …
    if (dev->volume_class == NVC_CDV)
        nvmeibc_tpv_handle_cdv_preempted(dev->vol);
    break;
```

`nvmeibc_tpv_detach` (existing) already: cancels `cdv_alloc_work`, `persist_work`, `bio_timeout_work`; fails parked bios on `pending_bios` and `pending_l1_flush_bios` with `-EIO`; transitions to `TPV_DETACHING`; unregisters `gendisk`; frees the xarray. This is the cleanup barrier that makes the §2.10.4 Path 2 / Path 3 safety arguments hold.

Trace macros: `_NI`/`_NW`/`_NE` (per project convention). No `pr_*`.

### Step 13. Handle `BELOW_CDV_FLOOR` register failure — `nvmesh-kernel/clnt/nvmeibc_register.c`

A `REGISTER` rejected with `NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR` is treated like `NCBD_PREEMPTED`:

```c
case NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR:
    _NW(reg_below_cdv_floor, "CDV register refused: below admission floor");
    nvmeibc_block_update_status(nd, 'P');   // triggers the CDV-preempted path (Step 12)
    break;
```

No new client-to-management RPC. The client's management agent observes the DB-side detach (Step 14 `cleanupDB`) and completes local cleanup via the normal `DetachVolumes` / re-attach flow.

---

## Phase 5 — Management preempt flow

### Step 14. `preemptClientFromCDV(cdvUUID, clientID, cb)` — `nvmesh-management/modules/volume.js`

Exported entry point. Signature callable from both the force-detach path and the stale-client cleanup path.

```js
scope.preemptClientFromCDV = (cdvUUID, clientID, cb) => {
    async.auto({
        lock:        next => lockUtils.acquireCDVLock(cdvUUID, next),
        cdv:         ['lock', (r, next) => volumeCollection.findOne({ uuid: cdvUUID }, next)],
        newFloor:    ['cdv', (r, next) => {
            const newFloor = (r.cdv.cdvConfig.admissionFloor || 0) + 1;
            volumeCollection.updateOne(
                { uuid: cdvUUID },
                { $max: { 'cdvConfig.admissionFloor': newFloor } },
                err => next(err, newFloor)
            );
        }],
        markEvicting: ['newFloor', (r, next) => {
            clientCollection.updateOne(
                { _id: clientID, 'attachments.volumeID': r.cdv._id },
                { $set: { 'attachments.$.action': consts.volumeAttachmentActions.EVICTING } },
                next
            );
        }],
        fanOut:      ['markEvicting', (r, next) => scope.sendPreemptToAllTomasOfCDV(
            r.cdv, clientID, r.newFloor, next
        )],
        cleanupDB:   ['fanOut', (r, next) => scope.clearEvictedClientState(
            r.cdv, clientID, next  // remove attachment, clear exclusiveClient on TPVs
        )],
        release:     ['cleanupDB', (r, next) => lockUtils.releaseCDVLock(cdvUUID, next)],
    }, cb);
};
```

Notes:
- Steps run serially; `async.auto` is used for readability.
- `lockUtils.acquireCDVLock` must be the same lock that serializes other per-CDV ops (extent pre-allocation for encrypted TPVs, CDV update, etc.). Verify the lock exists; add if missing.
- On any error, the `EVICTING` state remains — an operator or a restarted management instance can resume by calling the entry point again with the same args. Floor bump uses `$max`, not `$set`, so retry is idempotent.

### Step 15. Wire into existing eviction paths — `nvmesh-management/modules/client.js`

Three call sites / changes:

1. **TPV force-detach** (`client.js:detachTPV` and callers): before clearing `tpvConfig.exclusiveClient`, call `preemptClientFromCDV(cdv, client)`. The cleanup-DB step of the preempt handles the `exclusiveClient` clear internally; the existing `detachTPV` branch that does it today becomes a no-op via refactor.

2. **Stale-client cleanup** (`client.js:removeAlreadyDetachedAttachments`, line 138): when the path identifies a client that has been gone long enough to warrant full cleanup, and any of its CDV attachments hold `tpv:*` references, call `preemptClientFromCDV` per (client, CDV) pair instead of falling through to the existing "just remove the attachment" cleanup. This closes the Path 1 data-path hole for the stale-client case (not just the operator-initiated case).

3. **Attach-path gate** (`client.js:attachTPV`, line 5049): at the top, before the two-phase attach machinery, refuse with system message `CLIENT_EVICTING_FROM_CDV` if:
   ```js
   client.attachments.some(a =>
       a.volumeID === cdv._id &&
       a.action === consts.volumeAttachmentActions.EVICTING)
   ```
   Produces a retriable error — the client retries after the eviction clears.

### Step 16. Admin REST surface — `nvmesh-management/routes/clients.js`

New endpoint `POST /clients/:clientID/preemptFromCDV/:cdvID`. Admin-only (`isAdminRole` middleware). Body: `{ reason: string }` for audit logging. Calls `volume.preemptClientFromCDV(cdvID, clientID, cb)`.

Audit log entry: `TPVClientPreemptedFromCDV { cdv, client, reason, floor, timestamp, operator }`.

---

## Phase 6 — mNDU, CLI, CSI surface

### Step 17. mNDU gate — `nvmesh-interop-db`

Add a capability flag across components:

```js
// Models/Component.js or equivalent
{ component: 'toma',       supportsCdvAdmissionFloor: true, fromVersion: 'X.Y.0' }
{ component: 'client',     supportsCdvAdmissionFloor: true, fromVersion: 'X.Y.0' }
{ component: 'management', supportsCdvAdmissionFloor: true, fromVersion: 'X.Y.0' }
```

Management consults this before publishing `preemptClientFromCDV`: if any TOMA in the CDV's target set, or any client attached to the CDV, is below the threshold, the preempt is refused with `MIXED_VERSION_CLUSTER_NOT_SUPPORTED`. The operator must complete the rolling upgrade first. Upgrade-compatibility tuple lands in `nvmesh-interop-db` per the project's standard `Upgrade` model.

### Step 18. CLI surface — `nvmesh-infra/xlro/tools/cli`

New command `nvmesh client preempt-from-cdv <client> <cdv> [--reason …]` mapped to the REST endpoint from Step 16.

Location: `xlro/core/entities/rest.yaml`, `Client` entity (not `TPV` — preemption is a client-scope operation that happens to affect TPVs):

```yaml
Client:
  ops:
    preemptFromCdv:
      help: Forcibly evict a client from a CDV (fences all of the client's TPVs on that CDV)
      route: clients/{{entities[0].name}}/preemptFromCDV/{{cdv}}
      style: one
      opts:
        cdv:
          type: CDV
          required: true
        reason:
          type: str
          required: true
          help: Audit-log reason (free-form)
      payload: "{'reason': '{{reason}}'}"
```

Regenerate `current.api`. No SDK changes needed.

### Step 19. CSI audit — `nvmesh-csi-driver`

No code change. Audit verifies:
- The driver does not initiate preempts. Kubernetes pod eviction / node taint events do not map to per-client CDV preempt; management-side stale-client cleanup (Step 15.2) covers these paths.
- A CSI-created TPV whose client is preempted mid-I/O produces a surfaceable `NodePublishVolume` / `NodeStageVolume` error on the next operation, so Kubernetes treats the volume as unhealthy and reschedules the pod.

Document this contract in the CSI `README.md`. No behavior change.

---

## Phase 7 — Testing and stabilization

### Step 20. Unit tests (management) — `nvmesh-management/test/testThinProvisioning.js`

New `describe('CDV preempt client')`:

- Floor initialized to 0 on CDV create; stamped on every CDV `AttachVolumes` message (mock Kafka producer, capture payload).
- `preemptClientFromCDV` bumps floor exactly once, marks `EVICTING`, clears `EVICTING` and `exclusiveClient` on ACK.
- Double-preempt of the same client is a no-op (floor monotonicity via `$max`).
- Attach request during `EVICTING` is refused with `CLIENT_EVICTING_FROM_CDV`.
- ACK timeout scenario: Kafka stub drops the response; management retries with backoff; final failure leaves `EVICTING` set; a subsequent call resumes and completes.
- mNDU gate: one TOMA at a below-threshold version → preempt refused with `MIXED_VERSION_CLUSTER_NOT_SUPPORTED`.

### Step 21. Integration tests (kernel + management)

Requires a two-client NVMesh cluster with one CDV and two TPVs (one per client).

- **Golden path.** Two clients each doing sequential I/O at ~100 MB/s. Evict client A. Assert: A's I/O stops within 50 ms (tail-latency histogram); B's I/O is uninterrupted (zero-deviation band across the preempt window).
- **Offline client.** Evict a client that is unreachable (simulate via iptables drop on both Kafka and RDMA from the client's NICs). Assert: floor bump advances, Mongo state advances, DB cleanup completes. Restore connectivity; observe client `REGISTER` attempts rejected with `BELOW_CDV_FLOOR`; client-kernel tears down TPV; operator flow proceeds.
- **TPV reassignment.** Force-reassign TPV X from client A to client B. Assert: B can write to X with no observable race; A's TPV X device is gone (`/dev/nvmesh/X` removed); A's CDV device is `NCBD_PREEMPTED`.

### Step 22. Adversarial tests

- **Stale retry.** Client A ignores `DetachVolumes` and continues to retry `REGISTER` with its cached old version V. Assert every retry is rejected with `BELOW_CDV_FLOOR`, no admission, no `reg_ctx` re-created.
- **Raw-bio escape attempt.** Client A, after eviction, calls `nvmeibc_tpv_cdv_submit_bio` via the `unitest` harness with raw CDV offsets bypassing the TPV layer. Assert all bios fail at target registrant-lookup (because `reg_ctx` was terminated in Step 9).
- **Re-attach during EVICTING.** Client A attempts to re-attach during the `EVICTING` window. Assert refusal with `CLIENT_EVICTING_FROM_CDV`; assert admission after `EVICTING` clears.

### Step 23. Failover tests

- **TOMA failover mid-eviction.** Old primary ACKed the floor bump and started drain when it died. New primary is re-seeded from management's CDV metadata (floor is current) and has no `reg_ctx` for the evicted client (terminated by design). Assert no replay of stale I/O from the evicted client after failover.
- **Management failover mid-eviction.** `EVICTING` state persists in Mongo. New management instance observes it, detects that the Kafka fan-out did not fully ACK (via a scheduled sweep over attachments with `action === 'evicting'`), and resumes. Assert eventual DB cleanup.

### Feature flag

`management.cdvPerClientPreempt.enabled` (settings file). Off by default during Phases 1–7. Flipped on cluster-wide after Step 23 passes on the three-node reference cluster.

While the flag is off:
- Schema field `admissionFloor` is still written (value 0), so turning the flag on later is a no-op on existing CDVs.
- `attachTPV` still stamps `reservationModeVersion: 0` on CDV attach.
- `preemptClientFromCDV` refuses with `FEATURE_DISABLED`.
- TOMA predicate runs but always admits (floor is 0).

---

## Risks and open questions

1. **Eager vs. lazy TOMA CDV state (Step 7).** Eager creation is preferred but may require a non-trivial plumbing change on every TOMA's CDV-arrival path. If the prototype shows this is expensive, fall back to a sibling hash; cost is one extra lookup per `REGISTER` on a CDV segment. Decide after prototyping Step 7.

2. **Floor seeding race on hidden-CDV attach.** A client's hidden CDV attach triggers a `REGISTER` before the topology push that carries `admission_floor` has reached this TOMA. Mitigation: the management-side `AttachVolumes` message already carries the target floor per Step 2; the TOMA can seed `admission_floor` opportunistically from the first `AttachVolumes` for the CDV if `admission_floor_seeded == false`. Verify this path in Step 10.

3. **Existing registrants grandfathered.** A client already attached at version V, never reattached, remains admissible even after the floor bumps beyond V. This is deliberate (survivor-immunity invariant) but means a cooperative client does not learn about the new floor until it naturally disconnects. **Question:** should we add a slow path that re-stamps survivors on the next admin-initiated event (e.g., a background sweep)? Proposed: no. Any need to bump all survivors is exactly the volume-wide preempt case, which we already support via the existing `ReservationModeChange` mechanism.

4. **Concurrent evictions on one CDV.** Handled by the per-CDV lock (§2.10.4) plus monotonic floor. Verify in Step 20 unit tests.

5. **Attach-with-preempt interaction.** `attachTPV(preempt=true)` today bumps `cdv.reservation.version` and disturbs every survivor. **Question:** should this path switch to the narrow per-client primitive too? Proposed: yes, as a follow-on after Phase 7 stabilizes. Minimal risk, but the behavior change deserves its own validation window. Tracked separately.

6. **CDV deletion while `EVICTING` is in flight.** CDV delete path must either wait for eviction to clear or cancel it cleanly. Proposed: refuse CDV delete while any client's attachment on it has `action === 'evicting'`. Operator resolves by waiting or via a new admin endpoint for manually clearing stuck state (out of scope for Phase 7; file as follow-on).

7. **Admission-floor overflow.** `u64` floor with one bump per eviction-event per CDV. At one eviction per second: 584 billion years before wraparound. Not a concern; no wraparound logic needed.

8. **Kafka-replay idempotency.** TOMA handler is idempotent (floor uses `max`, register-lookup returns NULL for already-terminated client). Management retry path is idempotent (floor write uses `$max`). Verify no other code path re-reads `admissionFloor` with `$inc` semantics.

9. **`cdvTomaAutoAttach.js` interaction.** When the last `tpv:*` reference is removed during eviction cleanup (Step 14 `cleanupDB`), `cdvTomaAutoAttach` may choose to detach the CDV from the TOMA entirely. Verify this does not race with an in-flight `preemptClientFromCDV` handler on the same TOMA. The per-CDV lock in management serializes the outer flow; the TOMA handler runs to completion before ACKing, after which the DB cleanup proceeds. Safe, but cover with a targeted integration test.

10. **Observability.** Add:
    - `/proc/nvmeibt/cdv/<uuid>/admission_floor` on TOMA.
    - `nvmesh cdv show <cdv>` (CLI) displays `admissionFloor` in the `cdvConfig` block.
    - Management UI (`Volumes.jsx` CDV detail panel) surfaces preempt history: timestamp, preempted client, reason, floor transition.
    - Trace tags: `_NI` on `preemptClientFromCDV` receipt; `_NW` on `BELOW_CDV_FLOOR` rejection; `_NE` on termination failure.

11. **Naming review** — explicit flag-for-review items from the §2.10 author:
    - `cdvConfig.admissionFloor` namespaces clearly as CDV-specific. Alternative: top-level `cdv.admissionFloor` since it's not user-facing config.
    - Per-CDV state location on TOMA — eager extension of `nvmeibt_cdv_alloc` preferred, sibling hash is a one-paragraph swap.
    - Reason code `NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR` — happy to rename if a better convention exists in the TR-reason enum.

---

## Done criteria

- All 23 steps merged behind the feature flag. Each step independently reviewed and unit-tested.
- Step 21 golden-path integration test: ≤ 50 ms eviction latency on the evicted client, **zero** observable interruption on surviving clients (measured as 99.9th-percentile latency deviation across the preempt window).
- Step 22 adversarial tests all pass.
- Step 23 failover tests all pass on a three-node cluster.
- mNDU gate in place (Step 17) and verified against a mixed-version cluster.
- Feature flag flipped on cluster-wide in the reference cluster; soak test runs for 72 hours without regression.
- `TPV_ThinProvisioningImplementation.md` §2.10 cross-references this plan; after the flag flips on by default, the section is reframed as shipped design (not an open gap).
