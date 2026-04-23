# TPV — Per-Client CDV Preemption: Execution Plan

## Table of Contents

- [Phase map](#phase-map)
- [Phase 1 — Management schema and attach-path stamping](#phase-1--management-schema-and-attach-path-stamping)
  - [Step 1. CDV schema — `nvmesh-management/models/volume.js`, `validationSchemes/definitions/volume.js`](#step-1-cdv-schema--nvmesh-managementmodelsvolumejs-validationschemesdefinitionsvolumejs)
  - [Step 2. Attach-path stamping — `nvmesh-management/modules/client.js:attachTPV` (line 5049)](#step-2-attach-path-stamping--nvmesh-managementmodulesclientjsattachtpv-line-5049)
  - [Step 3. `EVICTING` state — `nvmesh-management/consts.js`](#step-3-evicting-state--nvmesh-managementconstsjs)
- [Phase 2 — Kafka plumbing](#phase-2--kafka-plumbing)
  - [Step 4. New message definitions — `nvmesh-management/models/kafkaMessages/`](#step-4-new-message-definitions--nvmesh-managementmodelskafkamessages)
  - [Step 5. ACK aggregation — `nvmesh-management/modules/client.js`](#step-5-ack-aggregation--nvmesh-managementmodulesclientjs)
  - [Step 6. Kafka router — `nvmesh-management/kafkaRouter.js`](#step-6-kafka-router--nvmesh-managementkafkarouterjs)
- [Phase 3 — TOMA admission floor and handler](#phase-3--toma-admission-floor-and-handler)
  - [Step 7. Per-CDV state location — `nvmesh-kernel/toma/nvmeibt_cdv_alloc.h`, `nvmeibt_cdv_alloc.c`](#step-7-per-cdv-state-location--nvmesh-kerneltomannvmeibt_cdv_alloch-nvmeibt_cdv_allocc)
  - [Step 8. Floor seeding on CDV topology arrival](#step-8-floor-seeding-on-cdv-topology-arrival)
  - [Step 9. `preemptClientFromCDV` handler — `nvmesh-kernel/toma/nvmeibt_kafka.c`](#step-9-preemptclientfromcdv-handler--nvmesh-kerneltomannvmeibt_kafkac)
  - [Step 10. New `REGISTER` predicate — `nvmesh-kernel/toma/nvmeibt_register.c`](#step-10-new-register-predicate--nvmesh-kerneltomannvmeibt_registerc)
- [Phase 4 — Client kernel cleanup barrier](#phase-4--client-kernel-cleanup-barrier)
  - [Step 11. Propagate `reservation_mode_version` on CDV attach — `nvmesh-kernel/clnt/nvmeibc_main_capi_manipulate_vols.inc.c`](#step-11-propagate-reservation_mode_version-on-cdv-attach--nvmesh-kernelclntnvmeibc_main_capi_manipulate_volsincc)
  - [Step 12. CDV-preempted propagation to TPVs — `nvmesh-kernel/clnt/tpv/nvmeibc_tpv.c`](#step-12-cdv-preempted-propagation-to-tpvs--nvmesh-kernelclnttpvnvmeibc_tpvc)
  - [Step 13. Handle `BELOW_CDV_FLOOR` register failure — `nvmesh-kernel/clnt/nvmeibc_register.c`](#step-13-handle-below_cdv_floor-register-failure--nvmesh-kernelclntnvmeibc_registerc)
- [Phase 5 — Management preempt flow](#phase-5--management-preempt-flow)
  - [Step 14. `preemptClientFromCDV(cdvUUID, clientID, cb)` — `nvmesh-management/modules/volume.js`](#step-14-preemptclientfromcdvcdvuuid-clientid-cb--nvmesh-managementmodulesvolumejs)
  - [Step 14b. Reaper for stuck `EVICTING` state — `nvmesh-management/modules/volume.js` + `bootstrapper.js`](#step-14b-reaper-for-stuck-evicting-state--nvmesh-managementmodulesvolumejs--bootstrapperjs)
  - [Step 15. Wire into existing eviction paths — `nvmesh-management/modules/client.js`](#step-15-wire-into-existing-eviction-paths--nvmesh-managementmodulesclientjs)
  - [Step 16. Admin REST surface — `nvmesh-management/routes/clients.js`](#step-16-admin-rest-surface--nvmesh-managementroutesclientsjs)
- [Phase 6 — mNDU, CLI, CSI surface](#phase-6--mndu-cli-csi-surface)
  - [Step 17. mNDU gate — `nvmesh-interop-db`](#step-17-mndu-gate--nvmesh-interop-db)
  - [Step 18. CLI surface — `nvmesh-infra/xlro/tools/cli`](#step-18-cli-surface--nvmesh-infraxlrotoolscli)
  - [Step 19. CSI audit — `nvmesh-csi-driver`](#step-19-csi-audit--nvmesh-csi-driver)
  - [Step 19b. UI — EVICTING indicator — `nvmesh-management/public/javascripts/components/pages/thinProvisioning/ThinProvisioning.jsx`](#step-19b-ui--evicting-indicator--nvmesh-managementpublicjavascriptscomponentspagesthinstrovisioningthinprovisioningjsx)
- [Phase 7 — Testing and stabilization](#phase-7--testing-and-stabilization)
  - [Step 20. Unit tests (management) — `nvmesh-management/test/testThinProvisioning.js`](#step-20-unit-tests-management--nvmesh-managementtesttestthinprovisioningjs)
  - [Step 21. Integration tests (kernel + management)](#step-21-integration-tests-kernel--management)
  - [Step 22. Adversarial tests](#step-22-adversarial-tests)
  - [Step 23. Failover tests](#step-23-failover-tests)
  - [Feature flag](#feature-flag)
- [Risks and open questions](#risks-and-open-questions)
  - [1. Eager vs. lazy TOMA CDV state (Step 7)](#1-eager-vs-lazy-toma-cdv-state-step-7)
  - [2. Existing registrants grandfathered](#2-existing-registrants-grandfathered)
  - [3. Concurrent evictions on one CDV](#3-concurrent-evictions-on-one-cdv)
  - [3a. Eventual-consistency window across TOMAs](#3a-eventual-consistency-window-across-tomas)
  - [3b. Cooperative survivor losing its registration](#3b-cooperative-survivor-losing-its-registration)
  - [4. CDV deletion while `EVICTING` is in flight](#4-cdv-deletion-while-evicting-is-in-flight)
  - [5. Kafka-replay idempotency](#5-kafka-replay-idempotency)
  - [6. `cdvTomaAutoAttach.js` interaction](#6-cdvtomautoattachjs-interaction)
  - [7. Observability](#7-observability)
- [Follow-on TODOs (post-initial-implementation)](#follow-on-todos-post-initial-implementation)
  - [F1. mNDU capability gate via `nvmesh-interop-db`](#f1-mndu-capability-gate-via-nvmesh-interop-db)
  - [F2. Feature flag `management.cdvPerClientPreempt.enabled`](#f2-feature-flag-managementcdvperclientpreemptenabled)
  - [F3. Span `handler_lock` across REGISTER floor-check + hash-insert](#f3-span-handler_lock-across-register-floor-check--hash-insert)
  - [F4. LOCKDEP-style annotations for per-CDV `handler_lock`](#f4-lockdep-style-annotations-for-per-cdv-handler_lock)
  - [F5. Ancillary code-quality](#f5-ancillary-code-quality)
  - [F6. UI surfacing of preempt history on CDV detail panel](#f6-ui-surfacing-of-preempt-history-on-cdv-detail-panel)
- [Done criteria](#done-criteria)

**Design reference:** `TPV_ThinProvisioningImplementation.md` §2.10 is authoritative. This document is the phased execution plan derived from it. If the two disagree, §2.10 wins and this plan is updated.

**Governing shape:** two orthogonal primitives.

- **Primitive A — CDV admission floor.** A monotonic `u64` per CDV, replicated from management to every TOMA. Gates **new** `REGISTER`s at registration time; leaves in-flight registrants untouched.
- **Primitive B — targeted registrant termination.** A new Kafka message `ManagementToTOMA.preemptClientFromCDV(clientID, cdvUUID, newFloor)`. TOMA raises the floor first, then terminates the named client's `reg_ctx` on every local CDV segment.

Combined, these fence one client from a `SHARED_READ_WRITE` CDV without disturbing the other SHARED holders and without touching the I/O hot path. Everything else — the earlier P1/P2/P3 triad, per-TPV cookies, `ForceDetachTPV` as a distinct primitive, `RevokeClientFromVolume` as a parallel admission concept — is superseded (§2.10.7).

---

## Phase map

| Phase | Steps | Scope |
|---|---|---|
| 1 — Management schema & attach-path stamping | 1–3 | `cdvConfig.admissionFloor` on the CDV doc, `attachTPV` EVICTING gate + floor stamping, `volumeAttachmentActions.EVICTING` |
| 2 — Kafka plumbing                          | 4–6 | New `PreemptClientFromCDV` + response messages, ACK aggregation, Kafka-router wiring |
| 3 — TOMA admission floor & handler          | 7–10 | Eager per-CDV state, dual-path floor seeding, `preemptClientFromCDV` handler, new `REGISTER` predicate + reason code |
| 4 — Client kernel cleanup barrier           | 11–13 | Propagate `reservation_mode_version` on CDV attach, teardown TPVs on `NCBD_PREEMPTED`, handle `BELOW_CDV_FLOOR` |
| 5 — Management preempt flow                 | 14–16 | `preemptClientFromCDV(cdv, client)` + reaper for stuck `EVICTING` state; hook into force-detach, stale-client cleanup, and attach-with-preempt |
| 6 — mNDU + CLI + CSI + UI surface           | 17–19b | interop-db gate, `nvmesh client preempt-from-cdv`, CSI no-op audit, UI Evicting badge + dial-alarm count |
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

Two sub-steps, in order. Both are required: the gate prevents handing out a stale-possible floor to a client that is still being evicted; the stamping gives the TOMA the authoritative value it will enforce on the resulting `REGISTER`.

**Sub-step 2a — EVICTING gate.** At the top of `attachTPV`, after `loadTPVAndCDV` resolves `cdv`, refuse the attach before any Kafka goes out if the client has an in-flight eviction on this specific CDV:

```js
// Runs before attachCDV in the async.series above.
function checkNotEvictingFromCDV(cb) {
    clientCollection.findOne({ _id: clientID }, { projection: { attachments: 1 } }, (err, client) => {
        if (err) return cb(new MongoError(err).log());
        const evicting = (client.attachments || []).some(a =>
            a.volumeID === cdv._id &&
            a.action === consts.volumeAttachmentActions.EVICTING);
        if (evicting) {
            return cb(new SystemMessage(systemMessages.CLIENT_EVICTING_FROM_CDV)
                .addInfo(Entities.Client.ID, clientID)
                .addInfo(Entities.Volume.ID, cdv._id));
        }
        cb();
    });
}
```

The check is per-`(client, CDV)` — an EVICTING state on a different CDV does not block attaches to unrelated CDVs. The error is retriable: the client retries once the eviction clears (Step 14 `cleanupDB`).

This sub-step is the single source of truth for "refuse during eviction." The attach-path gate discussed later in Step 15 bullet 3 is removed in favor of this placement, because the check must happen *before* floor stamping — otherwise a client whose eviction completes between gate-check and floor-stamp could be handed the new floor and race the cleanup.

**Sub-step 2b — Floor stamping.** Once the gate passes, the hidden CDV `AttachVolumes` payload carries `reservationModeVersion = cdv.cdvConfig.admissionFloor`:

```js
// attachCDV in attachTPV: stamp the current floor on the CDV attach.
scope.attachVolumes(clientID, clientUUID, [{
    uuid: cdv.uuid,
    name: cdv._id,
    referenceID: `tpv:${tpv.uuid}`,
    reservation: {
        mode: consts.reservationModeNames.SHARED_READ_WRITE,
        version: cdv.cdvConfig.admissionFloor || 0,     // NEW: stamp floor
    },
}], () => cb());
```

The `reservation.version` field on an `AttachVolumes` entry already exists and flows through `enrichAttachRequestVolume` / `setVolumeReservation` (`client.js:2383, 2393`) to the Kafka `AttachVolumes` payload and onward to the client kernel's `REGISTER` header. Verify end-to-end (there is already test coverage for the preempt flow on regular volumes; extend it to CDV attaches).

The TPV (phase 2) entry is unaffected — TPVs are not CDV segments and have no admission floor of their own.

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
u64           admission_floor;          // NEW
bool          admission_floor_seeded;   // NEW
struct mutex  handler_lock;             // NEW — see Lock ordering below
```

Today `nvmeibt_cdv_alloc` is created lazily on the first `CDV_ALLOC_EXTENT`. Change to **eager** creation on CDV topology arrival (the path that today delivers `cdv_extent_size_mib`, `allocator_size_gib`, etc., for CDV bookkeeping). This places the admission floor in the same struct as the rest of the CDV's per-TOMA metadata.

- If eager creation turns out to be too invasive (e.g., the CDV-arrival path does not currently have a per-CDV hook on every TOMA, only on the allocator TOMA), fall back to a sibling hash `nvmeibt_cdv_state` keyed by `cdv_uuid`, populated from the same topology message. Cost: one extra hash lookup per `REGISTER` on a CDV segment. Prototype before committing.

**Lock ordering invariant.** The new per-CDV `handler_lock` is acquired **before** any per-`seg_active` lock on every code path that takes both. Specifically:

- Step 9 handler: `handler_lock` → per-segment lock (inside `nvmeibt_register_terminate_reg_ctx`).
- Step 10 REGISTER predicate: `handler_lock` → per-segment lock (existing admission-path lock).

Audit existing callers of `nvmeibt_register_terminate_reg_ctx` and every REGISTER-path lock acquisition to confirm no path takes per-segment before per-CDV. If any does, it must be refactored. A lock-dependency lint (LOCKDEP annotations in debug builds) is cheap and should be added.

### Step 8. Floor seeding on CDV topology arrival

Two seeding paths, either of which is sufficient. This removes any ordering dependency between topology push and client `REGISTER` arrival.

**Path A (primary) — CDV-metadata topology message.** Extend the management → TOMA CDV-metadata push with `admission_floor`. On receipt:

```c
cdv->admission_floor = max_t(u64, cdv->admission_floor, msg.admission_floor);
cdv->admission_floor_seeded = true;
```

Uses `max_t` rather than plain assignment so this path is safe to interleave with the `preemptClientFromCDV` handler (Step 9) which also raises the floor.

**Path B (fallback) — first `AttachVolumes` for the CDV.** A client's hidden-CDV attach can race ahead of the topology push: the MCS `AttachVolumes` message arrives at TOMA before the CDV-metadata push on this node. To avoid rejecting the first `REGISTER` for lack of a seeded floor, piggyback on `AttachVolumes`:

```c
// In the CDV attach branch of the AttachVolumes handler:
if (!cdv->admission_floor_seeded) {
    cdv->admission_floor = attach_msg.reservation.version;
    cdv->admission_floor_seeded = true;
}
```

This is safe because `AttachVolumes` carries management's authoritative floor (Step 2b). If the CDV-metadata push later arrives with the same or newer value, the `max_t` in Path A keeps state monotonic; an older value is ignored.

If `nvmeibt_cdv_alloc` does not yet exist on this TOMA when either path fires, allocate and seed atomically under the per-CDV lock.

### Step 9. `preemptClientFromCDV` handler — `nvmesh-kernel/toma/nvmeibt_kafka.c`

```c
static int handle_preempt_client_from_cdv(const char *msg_json)
{
    // parse clientID, cdvUUID, newFloor
    cdv = nvmeibt_cdv_alloc_lookup(cdv_uuid);
    if (!cdv) {
        // Topology push hasn't reached this TOMA yet. Create the entry
        // on the fly, seeded from the authoritative newFloor in the
        // message. This eliminates a class of spurious retries when a
        // preempt races ahead of the topology delivery.
        cdv = nvmeibt_cdv_alloc_create_with_floor(cdv_uuid, newFloor);
        if (!cdv) { ack(success=false, error="alloc_failed"); return 0; }
    }

    mutex_lock(&cdv->handler_lock);

    // Step 1 (ORDER MATTERS): raise floor FIRST.
    cdv->admission_floor = max_t(u64, cdv->admission_floor, newFloor);

    // Step 2: terminate registrants on every CDV segment for this client.
    // Linear walk over active_registrants is acceptable here — preempt is
    // a control-plane event (rare), not an I/O-path operation. A dedicated
    // active_registrants_hash_by_client index was considered and rejected:
    // the register/unregister hot path pays a maintenance cost that isn't
    // recouped by the preempt path's savings.
    for_each_seg_active_of_cdv(cdv, seg_active) {
        NVMEIB_HASH_FOREACH(reg, seg_active->active_registrants_hash_by_handle) {
            if (reg->client_id != clientID) continue;
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

**Order invariant (floor first, termination second).** See §2.10.4. Add a block comment and debug-assert `admission_floor >= newFloor` between steps 1 and 2.

**Idempotency.** A second message with the same or lower `newFloor` is a no-op on the floor (because `max_t`); a second termination pass finds no matching `reg_ctx` and is a no-op there too.

**Lock discipline.** The `handler_lock` is acquired before any per-segment lock (see Step 7 Lock ordering invariant). `nvmeibt_register_terminate_reg_ctx` internally takes the per-segment lock; that is compatible with the ordering because this handler acquires per-CDV first.

### Step 10. New `REGISTER` predicate — `nvmesh-kernel/toma/nvmeibt_register.c`

In `handle_register_registrant_on_disk_segment` (line 2539), **before** the existing `is_valid_register_req` check:

```c
if (nvmeibt_seg_active_is_cdv(seg_active)) {
    struct nvmeibt_cdv_alloc *cdv = nvmeibt_cdv_alloc_for_seg(seg_active);
    // CRITICAL: take handler_lock across (floor-check + hash-insert) so a
    // concurrent preemptClientFromCDV handler (Step 9) cannot observe an
    // intermediate state where we passed the predicate but haven't yet
    // inserted into active_registrants. See §2.10.4 "handler step order".
    mutex_lock(&cdv->handler_lock);
    if (incoming_reg_ctx->reservation_mode_version < cdv->admission_floor) {
        mutex_unlock(&cdv->handler_lock);
        reg_refusal_reason = NVMEIBT_CLIENT_TR_REASON_BELOW_CDV_FLOOR;
        goto reject;
    }
    // … existing admission checks and hash insert under the existing
    // per-segment lock run while handler_lock is still held …
    mutex_unlock(&cdv->handler_lock);   // released only after the hash insert
}
```

The `handler_lock` must span **both** the floor-check and the `active_registrants_hash` insert. Without this span, the race is:

1. REGISTER reads floor = V, passes the check.
2. Context switch; preempt handler runs, raises floor to V+1, walks segments, finds no reg_ctx for clientA (the REGISTER hasn't inserted yet), terminates nothing.
3. REGISTER resumes, inserts reg_ctx.

Result: clientA has a fresh reg_ctx that is never terminated, defeating the preempt. Spanning the lock prevents step 2 from interleaving between the read and the insert.

`nvmeibt_seg_active_is_cdv(seg_active)` is a one-line helper (`volume_class == CDV`) added to `nvmeibt_seg_active.h`. `nvmeibt_cdv_alloc_for_seg(seg_active)` looks up the per-CDV state from the segment's parent CDV UUID.

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

**Idempotency invariant.** `nvmeibc_tpv_detach` must be callable more than once per TPV without harm. Two paths can invoke it concurrently or sequentially:

1. This step, on CDV `NCBD_PREEMPTED` (client-kernel initiated).
2. The subsequent management-initiated `DetachVolumes` (observed after Step 14 `cleanupDB` removes the attachment from Mongo and the client's management agent syncs state).

The detach path must gate its mutating work on `state != TPV_DETACHING && state != TPV_DETACHED`; a second entry observes the state transition from the first and returns without re-running teardown. Verify in Step 15 (kernel self-test: call `nvmeibc_tpv_detach` twice on the same TPV; second call is a no-op, no double-free, no use-after-free).

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
        lock:         next => lockUtils.acquireCDVLock(cdvUUID, next),
        cdv:          ['lock', (r, next) => volumeCollection.findOne({ uuid: cdvUUID }, next)],

        // CRITICAL: mark EVICTING FIRST, bump floor SECOND.
        //
        // If management crashes between these two writes, the reaper
        // (Step 14b) observes the EVICTING state on restart and resumes
        // from the floor-bump step. The reverse order leaves no
        // recoverable signal: a bumped floor with no EVICTING state is
        // indistinguishable from a completed eviction, so no recovery
        // path can know to terminate the stale client's reg_ctx on TOMA.
        markEvicting: ['cdv', (r, next) => {
            clientCollection.updateOne(
                { _id: clientID, 'attachments.volumeID': r.cdv._id },
                { $set: { 'attachments.$.action': consts.volumeAttachmentActions.EVICTING } },
                next
            );
        }],
        newFloor:     ['markEvicting', (r, next) => {
            const newFloor = (r.cdv.cdvConfig.admissionFloor || 0) + 1;
            volumeCollection.updateOne(
                { uuid: cdvUUID },
                { $max: { 'cdvConfig.admissionFloor': newFloor } },
                err => next(err, newFloor)
            );
        }],
        fanOut:       ['newFloor', (r, next) => scope.sendPreemptToAllTomasOfCDV(
            r.cdv, clientID, r.newFloor, next
        )],
        cleanupDB:    ['fanOut', (r, next) => scope.clearEvictedClientState(
            r.cdv, clientID, next  // remove attachment, clear exclusiveClient on TPVs
        )],
        release:      ['cleanupDB', (r, next) => lockUtils.releaseCDVLock(cdvUUID, next)],
    }, cb);
};
```

Notes:
- Steps run serially; `async.auto` is used for readability.
- `lockUtils.acquireCDVLock` must be the same lock that serializes other per-CDV ops (extent pre-allocation for encrypted TPVs, CDV update, etc.). Verify the lock exists; add if missing.
- On any error, the `EVICTING` state remains — an operator or the reaper (Step 14b) resumes by calling this entry point again with the same args. Floor bump uses `$max`, not `$set`, so retry is idempotent.
- §2.10.3 describes the (markEvicting, newFloor) pair as "atomic." This ordering plus the reaper is the operational equivalent of atomicity: any interleaving of a management crash with the two writes is recoverable. A true Mongo transaction across the `volume` and `client` collections is available and can be used if operator experience suggests the reaper's latency is unacceptable.

### Step 14b. Reaper for stuck `EVICTING` state — `nvmesh-management/modules/volume.js` + `bootstrapper.js`

On management startup and on a periodic timer (default 60 s), scan for attachments whose `action === 'evicting'` and resume each:

```js
scope.reapEvictingAttachments = (cb) => {
    clientCollection.find(
        { 'attachments.action': consts.volumeAttachmentActions.EVICTING }
    ).toArray((err, clients) => {
        if (err) return cb(new MongoError(err).log());
        async.eachSeries(clients, (client, nextClient) => {
            const evictingAttachments = (client.attachments || []).filter(
                a => a.action === consts.volumeAttachmentActions.EVICTING
            );
            async.eachSeries(evictingAttachments, (attachment, nextAttach) => {
                // Resume is safe because every step downstream is idempotent:
                //   - floor bump uses $max
                //   - TOMA handler uses max_t on the floor and a second
                //     terminate pass is a no-op
                //   - cleanupDB is a no-op if attachment is already gone
                scope.preemptClientFromCDV(attachment.cdvUUID, client._id, nextAttach);
            }, nextClient);
        }, cb);
    });
};
```

Wired into `bootstrapper.js` startup sequence after the Mongo connection is established, and into a `setInterval` (cleared on shutdown). The reaper bypasses the feature flag — stuck EVICTING states must be resumable regardless of whether the feature flag is currently on.

Reaper must also run if management observes a `CDV_PREEMPT_TOMA_UNRESPONSIVE` error from an earlier preempt: the 5-retry ACK loop in Step 5 gives up, but the EVICTING state persists, and the reaper picks it up on the next tick (useful if the unresponsive TOMA comes back later).

### Step 15. Wire into existing eviction paths — `nvmesh-management/modules/client.js`

Three call sites / changes:

1. **TPV force-detach** (`client.js:detachTPV` and callers): before clearing `tpvConfig.exclusiveClient`, call `preemptClientFromCDV(cdv, client)`. The cleanup-DB step of the preempt handles the `exclusiveClient` clear internally; the existing `detachTPV` branch that does it today becomes a no-op via refactor.

2. **Stale-client cleanup** (`client.js:removeAlreadyDetachedAttachments`, line 138): when the path identifies a client that has been gone long enough to warrant full cleanup, and any of its CDV attachments hold `tpv:*` references, call `preemptClientFromCDV` per (client, CDV) pair instead of falling through to the existing "just remove the attachment" cleanup. This closes the Path 1 data-path hole for the stale-client case (not just the operator-initiated case).

3. **Attach-with-preempt** (`client.js` caller of `attachTPV` with `reservation.preempt === PREEMPT`, line 2403 / 3516): today's path bumps the CDV's `reservation.version` volume-wide via the register-side bump at the new attacher's `REGISTER`, disturbing every survivor on the CDV. Replace with the narrow primitive: before the new client's `attachTPV` runs, call `preemptClientFromCDV(cdv, previousHolderClientID)` — where `previousHolderClientID` is read from `tpv.tpvConfig.exclusiveClient` on the target TPV. Once the per-client preempt clears, the new attach proceeds through Step 2 and stamps the current floor on the incoming client's CDV attach. No survivor impact, no `reservation.version` bump on the CDV.

   Keep the existing `reservation.preempt` flag semantics on the API surface for backward compatibility; the management-side implementation changes but the REST contract does not.

   **Failure propagation.** If the inline `preemptClientFromCDV` fails (typically `CDV_PREEMPT_TOMA_UNRESPONSIVE` after 5 retries), the attach must **not** proceed. Proceeding would stamp the new floor on the new client's CDV attach while the old client's `reg_ctx` still exists on some TOMAs — both clients end up writing to the same CDV extents concurrently, corrupting TPV data. Correct behavior: `attachTPV` returns the preempt error to the caller (REST or CLI), leaves `EVICTING` set, and lets the reaper (Step 14b) retry. The operator investigates the unresponsive TOMA.

   **Latency budget.** The inline preempt is synchronous: its latency is bounded by `Config.preemptAckTimeout` (default 30 s) plus the 5-retry exponential backoff — worst case on the order of a minute before the attach fails with `CDV_PREEMPT_TOMA_UNRESPONSIVE`. Happy path is sub-second (one round of Kafka delivery + handler time). Callers (REST, CSI driver, operator CLI) must allow the full worst-case timeout; document this in the REST contract for `POST /clients/:id/attach` with preempt semantics.

The attach-path EVICTING gate lives in Step 2a, not here — it must run before floor stamping, not alongside these eviction-initiating paths.

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

### Step 19b. UI — EVICTING indicator — `nvmesh-management/public/javascripts/components/pages/thinProvisioning/ThinProvisioning.jsx`

`EVICTING` is a per-`(client, CDV)` attachment state (`client.attachments[cdv].action`), not a TPV `status` value. TPV `status` remains `online | offline | degraded | unavailable` and the TPV state dial is **not** extended. Adding a fifth slice would (a) mix two orthogonal axes on one visual, and (b) flicker, because `EVICTING` is transient (sub-second happy path, at most a minute under worst-case preempt retry).

**Primary placement — row-level badge.** Add an `Evicting` badge in the attachment cell of the TPV row, following the column pattern the TPV-encryption plan uses for `encryption.command.status`:

```jsx
// In ThinProvisioning.jsx, per-row attachment rendering:
const exclusiveClient = tpv.tpvConfig.exclusiveClient;
const clientAttachment = exclusiveClient
    ? clientsById[exclusiveClient]?.attachments.find(a =>
        a.volumeID === tpv.tpvConfig.cdvId)
    : null;

if (clientAttachment?.action === consts.volumeAttachmentActions.EVICTING) {
    return <label className="label bg-yellow">Evicting</label>;
}
// …existing render of client name / status…
```

The badge disappears automatically when the eviction completes (action clears to `null` after Step 14 `cleanupDB`) or when the TPV row no longer has an `exclusiveClient`.

**Dashboard dial — count EVICTING TPVs as alarm.** The existing TPV state dial on the dashboard aggregates TPVs into its slices from `status`. The dial has no `offline` slice, so the natural mapping ("TPV has a holder that cannot serve I/O right now") cannot be shown directly. Simplest treatment: in the dial's aggregation function, **a TPV whose `exclusiveClient` has `action === 'evicting'` on the parent CDV is counted in the `alarm` slice** alongside the TPVs whose `status` already indicates alarm. The count drops back out of `alarm` automatically when the eviction clears (either `cleanupDB` in Step 14 or the reaper in Step 14b).

No new alarm type, no alarm-raise/clear wiring, no new infrastructure — just an aggregation tweak in the dashboard's dial data source.

**TPV row `status` column (no change).** During EVICTING the TPV's own `status` is whatever it was before — typically `online` if the CDV is healthy. The fact that the client's I/O is being fenced is orthogonal to the TPV's health. When the previous holder's attachment is removed and no new attachment yet exists, the TPV's existing status-computation path will naturally return `unavailable` (or the equivalent "no exclusive client" state).

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

### 1. Eager vs. lazy TOMA CDV state (Step 7)

The admission floor must be readable on **every** TOMA that serves a CDV segment, and readable at `REGISTER` time — which is the first interaction a new client has with the CDV. This has no analogue in the current code.

**Today's state (baseline).** `nvmeibt_cdv_alloc` — the only per-CDV state on TOMA — is created lazily on the first `CDV_ALLOC_EXTENT` IB admin message from a client. Before that first allocation arrives, there is no per-CDV state on the TOMA at all. Two consequences:

- The CDV-metadata topology push (management → TOMA) that runs today alongside `cdvTomaAutoAttach.js` does not create or update per-CDV state on the TOMA. It's consumed only for segment-level bookkeeping (first-pRAID composition, etc.).
- `REGISTER` for a CDV segment today has no CDV-level state to consult. The existing admission predicate in `nvmeibt_register.c` operates on `seg_active` alone.

Both of these must change for the admission floor to work as specified.

**Option A — eager per-CDV state.** Extend `nvmeibt_cdv_alloc` creation from "lazy on first alloc request" to "eager on CDV topology arrival." Concretely, when the CDV topology/metadata message is processed on any TOMA (leader or follower), the handler creates or updates the `nvmeibt_cdv_alloc` entry with the current `admission_floor` and the existing CDV geometry fields. The rest of the struct (extent list, allocator generation, pending-return list, etc.) remains populated only when this TOMA is the active allocator.

- **Pro:** one per-CDV struct per TOMA; the admission floor sits next to the existing CDV geometry; no separate hash to maintain.
- **Con:** more invasive — the topology handler on each TOMA needs a per-CDV hook it currently lacks. Touches `nvmeibt_cdv_alloc.c`, the topology-message handler in `nvmeibt_kafka.c` (or wherever the management → TOMA CDV metadata is parsed today), and any cold-start scan that rebuilds per-CDV state.
- **Invariant:** an `nvmeibt_cdv_alloc` entry where this TOMA is not the allocator holds only the floor (and the geometry already pushed by topology), not the extent list or generation. No risk of "fake allocator" because `handle_cdv_alloc_extent` is gated on `nvmeibt_raft_is_raft_valid()` per the existing allocator design.

**Option B — sibling hash `nvmeibt_cdv_state`.** Leave `nvmeibt_cdv_alloc` lazy. Introduce a second per-CDV hash keyed by `cdv_uuid`, populated eagerly on CDV topology arrival and holding `{admission_floor, admission_floor_seeded}` plus any future non-allocator per-CDV state. Every `REGISTER` on a CDV segment does one extra hash lookup.

- **Pro:** narrow code change — new file, new hash, one new lookup site. Does not disturb the lazy allocator semantics.
- **Con:** two per-CDV hashes. Future additions to per-CDV state have to choose between them, and the choice is not load-bearing — it creates the kind of drift that accumulates into maintenance debt.

**Decision criterion.** Prototype Option A in a short spike. If the spike shows the topology-handler change requires more than ~200 LoC of new plumbing across more than two files, fall back to Option B. The `REGISTER`-path cost of Option B is one hash lookup (not a linked-list walk, not a lock roundtrip), which is invisible in profile; the real cost is maintenance.

**Orthogonal consideration — RAFT replication.** The admission floor itself is sourced from management, not RAFT. It does **not** need RAFT consensus to be correct: every TOMA gets the same value from the same Kafka topology push, and on leader failover the new leader rereads the CDV document from Mongo at management's request. RAFT is not a dependency of the floor. The existing `nvmeibt_cdv_alloc_notify` RAFT-unicast mechanism (for allocator identity) is unrelated and should not be pressed into service for the floor.

### 2. Existing registrants grandfathered

A client already attached at floor V, never reattached, remains admissible after the floor bumps past V. This is the survivor-immunity invariant (§2.10.4) and is deliberate.

A cooperative client that wants to learn the current floor contacts management via the normal attach/refresh path; management stamps the current floor on the next `AttachVolumes`. No background sweep is required. If a future feature needs to bump all survivors, the existing volume-wide `ReservationModeChange` mechanism handles that case — it is not a job for the per-client primitive.

### 3. Concurrent evictions on one CDV

Handled by the per-CDV lock (§2.10.4) plus monotonic floor. Verify in Step 20 unit tests.

### 3a. Eventual-consistency window across TOMAs

A single `preemptClientFromCDV` is fan-out to every TOMA of the CDV; each TOMA processes independently. Between the first ACK and the last, the evicted client's already-mapped CDV writes can still land on TOMAs whose handler hasn't run yet. The window is bounded by Kafka delivery + handler time per TOMA (happy path: sub-second; worst case: the ACK-timeout window).

Invariant A ("evicted client cannot write to the CDV") holds **strictly only after all ACKs land** and management clears `EVICTING`. During the window:

- No new client can take over the evicted TPV — management gates the reassigning attach on `EVICTING` clearing (Step 2a), so both-writing races are structurally impossible.
- Survivor clients on the same CDV are unaffected (Invariant B still holds instantaneously).
- The only effect is that a stale writer's I/O tail bleeds into already-allocated extents for a bounded period. For the stale-writer scenarios this design targets (rogue or unresponsive client), this matches or improves on today's volume-wide preempt behavior.

Document this bound explicitly in operator-facing material. For use cases requiring strict atomicity (none currently), a two-phase-commit variant is available as a future extension but out of scope here.

### 3b. Cooperative survivor losing its registration

A survivor reg_ctx that is reaped by TOMA (timeout, reconnect cycle, or any cause) causes the client-kernel to retry `REGISTER` with its cached `reservation_mode_version`. If the CDV floor has advanced since the original attach, the predicate returns `BELOW_CDV_FLOOR`, which Step 13 maps to `NCBD_PREEMPTED` → full TPV teardown.

This is correct per the design's philosophy (no new client↔management RPC, management is the only source of current-floor truth), but it's a bigger disruption than the pre-feature "re-register with cached version" behavior on non-CDV volumes.

**Accepted trade-off.** The alternatives — teach the client to consult management on `BELOW_CDV_FLOOR` before tearing down, or widen the predicate to a tolerance window — add complexity that isn't justified for a rare event. The cooperative survivor's TPV will come back after a management-driven re-attach; the disruption is visible to the workload but bounded in duration and frequency.

### 4. CDV deletion while `EVICTING` is in flight

Follow the same policy NVMesh applies today to regular-volume deletion in degraded / offline / mid-operation states: no blanket block, operator responsibility. Concretely: a CDV delete issued while any of its attachments has `action === 'evicting'` is permitted if the regular-volume delete path in the same scenario would permit it. Left open until the broader "delete while in-flight op" policy for TPV / CDV is pinned down.

### 5. Kafka-replay idempotency

TOMA handler is idempotent (floor uses `max`, register-lookup returns NULL for already-terminated client). Management retry path is idempotent (floor write uses `$max`). Verify no other code path re-reads `admissionFloor` with `$inc` semantics.

### 6. `cdvTomaAutoAttach.js` interaction

When the last `tpv:*` reference is removed during eviction cleanup (Step 14 `cleanupDB`), `cdvTomaAutoAttach` may detach the CDV from the TOMA entirely. Verify this does not race with an in-flight `preemptClientFromCDV` handler on the same TOMA. The per-CDV lock in management serializes the outer flow; the TOMA handler runs to completion before ACKing, after which the DB cleanup proceeds. Safe, but cover with a targeted integration test.

### 7. Observability

- `nvmesh cdv show <cdv>` (CLI) displays `admissionFloor` in the `cdvConfig` block.
- Management UI (`Volumes.jsx` CDV detail panel) surfaces preempt history: timestamp, preempted client, reason, floor transition.
- Trace tags: `_NI` on `preemptClientFromCDV` receipt; `_NW` on `BELOW_CDV_FLOOR` rejection; `_NE` on termination failure.

---

## Follow-on TODOs (post-initial-implementation)

These items were deliberately scoped out of the initial implementation. None affects correctness of the per-client preempt primitive; they are hardening, operator-experience, and code-cleanup items.

### F1. mNDU capability gate via `nvmesh-interop-db`  *(from Step 17)*

**Status:** `systemMessages.MIXED_VERSION_CLUSTER_NOT_SUPPORTED` is declared but unused. No gating logic exists in `preemptClientFromCDV`.

**What's missing:**
- Add a capability flag to the `Component` (or `ComponentVersion`) model in `nvmesh-interop-db`: `supportsCdvAdmissionFloor` (or equivalent), set `true` from the release that ships this feature onward.
- Add a helper query in `dbAPI.js` that returns true iff every component (management, TOMA, client) currently in the cluster supports the capability.
- Add a guard at the top of `modules/client.js:preemptClientFromCDV` that returns `MIXED_VERSION_CLUSTER_NOT_SUPPORTED` when the capability is not cluster-wide.

**Why deferred:** Adding the capability flag is coordinated with the interop-db team and requires a migration. Per-client preempt is safe to ship without the gate as long as cluster rollout completes before any preempt is issued — which is the normal mNDU discipline.

### F2. Feature flag `management.cdvPerClientPreempt.enabled`

**Status:** Not implemented. `preemptClientFromCDV` runs unconditionally when invoked; the schema field `cdvConfig.admissionFloor` is always written on CDV create.

**What's missing:**
- Add the flag to `generalSettings` (or the equivalent settings mechanism).
- Short-circuit `preemptClientFromCDV` with a specific system message when off.
- Test path: flag-off mode writes `admissionFloor: 0` into `cdvConfig` (so a later flip-on is a no-op on existing CDVs), `attachTPV` still stamps `reservation.version: 0` on the CDV attach (no-op gate on TOMA), and `preemptClientFromCDV` refuses with `FEATURE_DISABLED`.

**Why deferred:** The plan §5 "Feature flag" describes this as the rollout mechanism. It is a rollout-ops concern, not a correctness concern; the current code is safe to enable always (the admission floor starts at 0 and only advances on explicit preempt).

### F3. Span `handler_lock` across REGISTER floor-check + hash-insert

**Status:** Known trade-off, documented in `toma/nvmeibt_register.c:check_cdv_admission_floor` FOLLOW-ON comment.

**Current shape:** The REGISTER predicate takes `handler_lock`, checks the floor, unlocks, returns. The subsequent hash insert in `register_on_disk_segment` runs unlocked.

**Why safe today:** The preempt handler walks `active_registrants_hash_by_handle` under `handler_lock`. A REGISTER that slips past the floor check between floor-raise and hash-insert installs a reg_ctx that the handler's linear walk catches. Management does not clear `EVICTING` until all TOMAs ACK — so the management-side "preempt complete" state is only declared after any racing REGISTER has been caught.

**What hardening would add:** span `handler_lock` across the hash insert. Requires threading a locked `cdv_alloc *` pointer through `is_valid_register_req` → `register_on_disk_segment` → the active-registrants-hash insert, and unlocking at the insert's completion. Larger refactor touching existing code that already has its own lock discipline. Skip unless profiling or a live race shows the race is observable.

### F4. LOCKDEP-style annotations for per-CDV `handler_lock`

**Status:** Not added.

**What's missing:** Debug-build annotations (LOCKDEP in kernel code; equivalent mechanism in TOMA userspace) to assert the lock ordering invariant `handler_lock` → per-`seg_active` locks on every call site that takes both. Cheap to add; catches accidental ordering violations introduced by future refactors.

**Why deferred:** The invariant holds by construction in this change (every call site verified); the annotation is a regression guard, not a current-correctness requirement.

### F5. Ancillary code-quality

- **Unused system message:** `MIXED_VERSION_CLUSTER_NOT_SUPPORTED` is declared in `systemMessages.js` (id 1960) but never referenced. Either wire it into F1's gate or remove until F1 lands. Currently harmless clutter.
- **Retry-backoff timer handle:** In `sendPreemptToAllTomasOfCDV`'s retry loop, the `setTimeout` scheduled during exponential backoff is not stored on `entry.timer`, so `resolvePendingPreempt` cannot cancel it. The guard `if (!pendingPreempts.has(key)) return;` at the top of the backoff callback makes this safe — at worst, a few wasted republish cycles when all ACKs arrive during a backoff window. Storing the backoff timer handle and cancelling it on resolve eliminates the wasted work.
- **`clientUUID` propagation:** `new PreemptClientFromCDV(clientID, null, cdv._id, cdv.uuid, newFloor)` passes `null` for clientUUID. TOMA does not read it today (it matches by hostname via `registrant_node_id.str`). If a future code path wants the UUID, look it up from `clientCollection.findOne({_id: clientID}, {projection: {uuid: 1}})` at the top of `preemptClientFromCDV` and pass it through.

### F6. UI surfacing of preempt history on CDV detail panel

**Status:** Per-row `Evicting` badge on `ThinProvisioning.jsx` TPV table is implemented. Dashboard dial alarm count is implemented. Historical preempt log (who was evicted when, by whom, with what reason) is not.

**What's missing:** Persist each successful preempt in an audit-log collection (or piggyback on the existing `auditLog` the REST endpoint already writes) and render it as a tab on the CDV detail panel in `Volumes.jsx`.

**Why deferred:** Operator-experience enhancement. The data needed (timestamp, clientID, reason, floor transition) is already captured in the audit log written by the admin REST endpoint; the UI piece is a separate workstream.

---

## Done criteria

- All 23 steps merged behind the feature flag. Each step independently reviewed and unit-tested.
- Step 21 golden-path integration test: ≤ 50 ms eviction latency on the evicted client, **zero** observable interruption on surviving clients (measured as 99.9th-percentile latency deviation across the preempt window).
- Step 22 adversarial tests all pass.
- Step 23 failover tests all pass on a three-node cluster.
- mNDU gate in place (Step 17) and verified against a mixed-version cluster.
- Feature flag flipped on cluster-wide in the reference cluster; soak test runs for 72 hours without regression.
- `TPV_ThinProvisioningImplementation.md` §2.10 cross-references this plan; after the flag flips on by default, the section is reframed as shipped design (not an open gap).
