# TPV Encryption — Full Execution Plan

**Scope:** everything required to ship encrypted Thin-Provisioned Volumes (TPVs) end-to-end: management backend, Web UI, TOMA kernel-space, NVMesh CLI, and the Kubernetes CSI driver.

**Reference:** `nvmesh-kernel/design/TPV_ThinProvisioningImplementation.md` Part 5 is the authoritative design. This document is the execution plan derived from it.

**Governing invariant (Architecture Decision #18):** CDVs and `<CDV>-mgmt` satellites are **never** encrypted at the NVMesh volume layer. Encryption is per-TPV — a LUKS container living inside the TPV's virtual address space; the header is written at offset 0 of the TPV on first use, which the client-side TPV allocator lazily binds to a CDV extent. No upfront extent reservation is required. Management refuses to enable encryption on any volume of class `CDV` or `CDV_MGMT`.

**Simplification history:** earlier drafts of this plan required management to pre-allocate the TPV's first CDV extent via `CDV_ALLOC_EXTENT`, plumb `cdvName` / `cdvByteOffset` / `shadowSizeSectors` through a new Kafka payload, and have TOMA build a `dm-linear` shadow on the CDV. All of that is gone. The executing TOMA instead attaches the TPV to itself (client-style, preempt + EXCLUSIVE_READ_WRITE) before the Kafka command is sent, and runs `cryptsetup` directly against `/dev/nvmesh/<name>`. Items 21, 25 in Part 5 §5.3 mark the removals explicitly.

---

## Comparison: Regular volume encryption vs TPV encryption

At a single glance, the two flows only diverge in two places: (a) management does an attach/detach around the Kafka command for TPVs, and (b) TOMA runs `cryptsetup` against a different device path. The Kafka payload, the REST endpoint, the DB schema for `encryption.command.*`, the response message, the UI modal components, and the client-side LUKS-open flow are all shared.

### Side-by-side table

| Stage | Regular volume | TPV |
|---|---|---|
| **CLI — init** | `nvmesh volume init-encryption --passphrase <p> --slot 1 --key-size 512 name <vol>` | `nvmesh tpv init-encryption --passphrase <p> --slot 1 --key-size 512 name <tpv>` |
| **CLI — passphrase ops** | `nvmesh volume add-passphrase / rotate-passphrase / delete-passphrase …` | `nvmesh tpv add-passphrase / rotate-passphrase / delete-passphrase …` |
| **REST endpoint** | `POST /volumes/initEncryption` (and the three passphrase routes) | **Same endpoint, same payload** — management branches internally on `volumeClass` |
| **Pre-command verify** (`verifyEncryptionCommand`) | `isEncrypted`, `isInitialized`, `action === INIT_ENCRYPTION_REQUIRED` | Same checks, unchanged |
| **TOMA selection** (`chooseTOMAForEncryption`) | Zone round-robin: any TOMA in the volume's zone with `tomaStatus === UP`, indexed by `lock.encryptionCommandIndex` | `chooseTOMAForTPVEncryption`: TOMAs owning an RW disk segment on the parent CDV's first pRAID; prefer the current CDV allocator TOMA, random among the rest |
| **Pre-Kafka attach** | None | `attachTPVToTOMAForEncryption`: `clientModule.attachTPV(toma._id, tomaClientUUID, tpv._id, {preempt: true})` so `/dev/nvmesh/<tpv>` materialises on the TOMA node. `syncFlush` is deliberately **not** passed — `attachTPV`'s `applySyncFlush` step persists `sourceUUID` on the TPV doc, so overriding here would leak the encryption-time value past detach. |
| **DB stamp** (`setEncryptionCommand`) | `encryption.command.{status=PENDING_SEND, name, executingTOMA, bootTime}`, `$inc commandIndex` | Same fields **plus** `encryption.command.tpvAutoAttachedTOMA = toma._id` so recovery and response handler can drive the symmetric detach |
| **Kafka message type** | `InitEncryption` / `AddPassphrase` / `DeletePassphrase` / `RotatePassphrase` (identical) | Identical — same base class, same subclasses, same JSON schema. No `cdvName` / `cdvByteOffset` / `shadowSizeSectors` — **unchanged envelope** |
| **Kafka payload fields** | `bootTime`, `volumeName`, `volumeUUID`, `encryptionCommandIndex`, `passphrase`/`currentPassphrase`/`newPassphrase`, `slot`, `keySize`, `numberOfSlots` | Identical set of fields |
| **TOMA entry** | `start_encrypt_action()` in `nvmeibt_kafka.c` | Same function; branches when `vol->from_config.n_chunks == 0 && !vol->from_config.is_cdv` (TOMA has no `type` field — TPV detection uses the chunk-less shape of the management record) |
| **TOMA local device setup** | `create_shadow_vol()` clones `vol->chunks[]` → schedules attach via `launch_attach_detach_task` → `/dev/nvmesh/e_<vol>` appears after WQ completion | **None.** TPV is already attached by management; `/dev/nvmesh/<tpv>` is already present |
| **TOMA exec device path** | `/dev/nvmesh/e_<vol>` (the shadow) | `/dev/nvmesh/<tpv>` (the real TPV) |
| **TOMA exec wrapper** | `nvmeibt_attach_vol_for_encryption` → attach WQ → `attach_shadow_vol_for_encryption_finalize` → `nvmeibt_run_exec_on_blkdev` | `nvmeibt_start_encrypt_for_tpv` → **directly** `nvmeibt_run_exec_on_blkdev` (no attach WQ, no shadow clone) |
| **TOMA cryptsetup command** | `cryptsetup luksFormat --sector-size=4096 --key-slot=<s> --key-size=<k> --key-file=<f> /dev/nvmesh/e_<vol>` | `cryptsetup luksFormat --sector-size=4096 --key-slot=<s> --key-size=<k> --key-file=<f> /dev/nvmesh/<tpv>` |
| **First I/O behaviour** | Writes land on the shadow which shares chunks with the origin — direct disk I/O | Writes land on the TPV; the client-side TPV allocator materialises CDV extents on demand during the LUKS header write (via `CDV_ALLOC_EXTENT` on the admin channel) |
| **TOMA local device teardown** | `nvmeibt_detach_vol_for_encryption` (triggered from exec callback) → detach WQ → `detach_shadow_vol_for_encryption_finalize` → shadow freed | **None.** TPV stays attached; `tpv_encrypt_after_exec_cb` only frees `encrypt_params` and buffers |
| **TOMA response** | `encryptionCommandResponse` Kafka message (SUCCESS / CMD_ERR / TOMA_ERR / UNSEEN / MANUAL_ACTION_NEEDED) | Identical response envelope |
| **Post-response management work** (`handleCommandResponse`) | DB update: `status = EXECUTED`, `isInitialized = true`, `isReady = true` | Same DB update **plus** `detachTPVFromTOMAForEncryption(tpv, toma._id)` and `$unset encryption.command.tpvAutoAttachedTOMA` |
| **Crash recovery on management restart** | `resendStaleEncryptionCommands` (asks TOMA to retransmit response for `PENDING_SEND` states) | `resendStaleEncryptionCommands` **plus** `cleanupTPVAutoAttachesAfterStartup` (detaches TPVs stranded with `tpvAutoAttachedTOMA` set and `status === EXECUTED`) |
| **Intra-call failure (Kafka broker down etc.)** | Command stays `PENDING_SEND`; retry picked up by `resendStaleEncryptionCommands` | `runEncryptionCommand` final-callback detaches the TPV when `tpvAttached && !kafkaSent`, so retry starts from a clean reservation state |
| **Client attach flow after init** | Client attaches volume normally → client's LUKS agent `cryptsetup open /dev/nvmesh/<vol>` → mapper device mounted | Client attaches TPV normally → client's LUKS agent `cryptsetup open /dev/nvmesh/<tpv>` → mapper device mounted |
| **Shares per-CDV TOMA set?** | No — encryption can run on any TOMA in the zone | Yes — encryption runs on one of the CDV's first-pRAID TOMAs (locality with the CDV allocator) |

### Sequence diagram — regular volume encryption

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant CLI as CLI / UI
    participant Mgmt as Mgmt (volumeEncryption.js)
    participant DB as MongoDB
    participant Kafka
    participant TOMA as TOMA kernel
    participant Shadow as /dev/nvmesh/e_<vol>

    User->>CLI: init-encryption <vol>
    CLI->>Mgmt: POST /volumes/initEncryption
    Mgmt->>Mgmt: verifyEncryptionCommand()
    Mgmt->>Mgmt: chooseTOMAForEncryption() (zone round-robin)
    Mgmt->>DB: setEncryptionCommand → status=PENDING_SEND
    Mgmt->>Kafka: InitEncryption(base envelope)
    Mgmt->>DB: updateLastCommandSent → status=SENT
    Kafka->>TOMA: InitEncryption
    TOMA->>TOMA: start_encrypt_action (regular branch)
    TOMA->>TOMA: create_shadow_vol, launch_attach_detach_task (ATTACH)
    TOMA->>Shadow: shadow appears
    TOMA->>Shadow: cryptsetup luksFormat /dev/nvmesh/e_<vol>
    TOMA->>TOMA: nvmeibt_detach_vol_for_encryption (DETACH WQ)
    TOMA-->>Shadow: shadow freed
    TOMA->>Kafka: encryptionCommandResponse (SUCCESS)
    Kafka->>Mgmt: response
    Mgmt->>DB: handleCommandResponse → status=EXECUTED, isInitialized=true, isReady=true
    Note over User,Mgmt: Later: user attaches volume; client-side LUKS opens /dev/nvmesh/<vol>
```

### Sequence diagram — TPV encryption

```mermaid
sequenceDiagram
    autonumber
    actor User
    participant CLI as CLI / UI
    participant Mgmt as Mgmt (volumeEncryption.js)
    participant DB as MongoDB
    participant Client as clientModule (attachTPV/detachTPV)
    participant Kafka
    participant TOMA as TOMA kernel
    participant Dev as /dev/nvmesh/<tpv>

    User->>CLI: init-encryption <tpv>
    CLI->>Mgmt: POST /volumes/initEncryption
    Mgmt->>Mgmt: verifyEncryptionCommand()
    Mgmt->>Mgmt: chooseTOMAForTPVEncryption() (CDV first-pRAID, prefer allocator)
    Mgmt->>Client: attachTPV(toma, preempt=true, EXCL_RW)
    Client->>TOMA: standard TPV attach Kafka to TOMA node (CDV ref + TPV EXCL_RW)
    Note over TOMA,Dev: /dev/nvmesh/<tpv> materialises on the TOMA node
    Mgmt->>DB: setEncryptionCommand → status=PENDING_SEND, tpvAutoAttachedTOMA=toma._id
    Mgmt->>Kafka: InitEncryption (same envelope as regular volume)
    Mgmt->>DB: updateLastCommandSent → status=SENT
    Kafka->>TOMA: InitEncryption
    TOMA->>TOMA: start_encrypt_action detects TPV (n_chunks==0 && !is_cdv)
    TOMA->>TOMA: nvmeibt_start_encrypt_for_tpv (no shadow, no WQ attach)
    TOMA->>Dev: cryptsetup luksFormat /dev/nvmesh/<tpv>
    Note over Dev,TOMA: client-side TPV allocator materialises CDV extents on demand via CDV_ALLOC_EXTENT during LUKS header writes
    TOMA->>Kafka: encryptionCommandResponse (SUCCESS)
    Kafka->>Mgmt: response
    Mgmt->>DB: handleCommandResponse → status=EXECUTED, isInitialized=true, isReady=true
    Mgmt->>Client: detachTPV(toma, tpv)
    Mgmt->>DB: $unset encryption.command.tpvAutoAttachedTOMA
    Note over User,Mgmt: Later: user attaches TPV; client-side LUKS opens /dev/nvmesh/<tpv>
```

### Detach driver audit

A TPV auto-attached to a TOMA for encryption **must** be released once encryption concludes, otherwise the real client cannot reattach. The matrix below enumerates every failure shape and names the code path responsible for the detach:

| Scenario | Attach happened? | Kafka delivered? | Driver that detaches |
|---|---|---|---|
| Happy path (success response) | yes | yes | `handleCommandResponse` (after DB update) |
| TOMA-side error response (CMD_ERR / TOMA_ERR / etc.) | yes | yes | `handleCommandResponse` (same path — runs regardless of `result`) |
| Response lost, re-requested by `resendStaleEncryptionCommands` | yes | yes | `handleCommandResponse` on retransmit |
| Management crash between DB response-update and the detach call | yes | yes | `cleanupTPVAutoAttachesAfterStartup` (on next startup; scoped to `status === EXECUTED`) |
| `chooseTOMAForEncryption` fails | no | — | N/A — attach never happened |
| `attachTPVToTOMAForEncryption` itself fails | no (or partially rolled back by attachTPV) | — | N/A — `tpvAttached` stays false |
| `setEncryptionCommand` DB write fails | yes | no | `runEncryptionCommand` final callback (`tpvAttached && !kafkaSent`) — detaches and `$unset`s the stamp (best-effort if never stamped) |
| `sendEncryptionCommandToTOMA` Kafka send fails | yes | no | Same: final-callback detach |
| `updateLastCommandSent` fails | yes | **yes** | **Not** final-callback — TOMA is already running cryptsetup; yanking the device now would corrupt the op. Falls through to the happy-path driver: TOMA will respond, `handleCommandResponse` detaches |
| TOMA goes down mid-cryptsetup (no response ever arrives) | yes | yes | Existing cluster-wide limitation — same for regular encryption. Operator retries; the retry's `attachTPV(preempt: true)` fences the stale reservation via `preemptClientFromCDV` and proceeds on a surviving TOMA |

Two of these are new in the TPV flow and warrant explicit callouts in review:
- **`runEncryptionCommand` final-callback detach** uses two closure bools — `tpvAttached` (set after `attachTPVToTOMAForEncryption` succeeds) and `kafkaSent` (set after `sendEncryptionCommandToTOMA` succeeds). The detach runs **only when** `tpvAttached && !kafkaSent`. Detaching after Kafka delivery would pull the block device out from under TOMA's in-flight `cryptsetup`.
- **`cleanupTPVAutoAttachesAfterStartup`** is scoped to `encryption.command.status === EXECUTED`. TPVs still in `PENDING_SEND` or `SENT` on startup are assumed to be in-flight (TOMA may yet respond) and are left alone; the existing `resendStaleEncryptionCommands` path drives eventual response retransmission, and the response handler then performs the detach.

---

## Phase map

| Phase | Steps | Scope |
|-------|-------|-------|
| 1 — Backend        | 1–4   | `createTPV()` encryption flags; TOMA selection; attach/detach orchestration; backend test |
| 2 — UI             | 5–7   | `CreateTPVModal` encryption fields; `ThinProvisioning.jsx` toolbar + handlers; UI test |
| 3 — TOMA           | 8–10  | TPV branch in `start_encrypt_action`; `nvmeibt_start_encrypt_for_tpv`; TOMA test |
| 4 — CLI            | 11–12 | `rest.yaml` TPV encryption params + ops; golden-file refresh; CLI smoke test |
| 5 — CSI Driver     | 13–17 | StorageClass parsing; post-create init-encryption; `delete_tpv` rollback helper; SC example; integration tests |
| 6 — End-to-end     | 18    | UI / CLI / CSI triangulated flow + error scenarios |

---

## Phase 1 — Management Backend

### Step 1. `createTPV()` encryption support — `nvmesh-management/modules/volume.js`

- Extend `createTPV()` to accept `isEncrypted: boolean` and `encryption: { headerSize: number }` on the input payload.
- Insert the TPV document with:
  - `isEncrypted: true`
  - `encryption: { headerSize: <requested or 16>, isInitialized: false }`
  - `isReady: false` (flips to `true` only after `initEncryption` completes)
  - `action: INIT_ENCRYPTION_REQUIRED`
- **No CDV-side work at creation time.** No `CDV_ALLOC_EXTENT`, no `firstExtentIndex`, no retriable "allocator not elected" failure path. The TPV is a regular insert; the LUKS header is written later on first cryptsetup I/O via the existing client-side TPV allocator.
- `prepareCDVForCreate()`: strip any incoming `isEncrypted` / `encryption` on CDV payloads (Architecture Decision #18).

### Step 2. TOMA selection — `nvmesh-management/modules/volumeEncryption.js`

- Implement `chooseTOMAForTPVEncryption(tpvVolume, cb)`:
  1. Look up the parent CDV via `tpvConfig.cdvId`.
  2. Find candidate TOMA nodes: servers hosting an RW disk segment in the CDV's first pRAID with `tomaStatus === UP`.
  3. Prefer the CDV's current allocator TOMA (read from `cdv.currentAllocatorTomaHostname`, populated by `client.js::handleAttachSatelliteRequest`); otherwise pick randomly from the candidates.
  4. Return `{_id, bootTime, topics}`.
- `chooseTOMAForEncryption()` gains the branch:
  ```js
  if (volume.volumeClass === consts.volumeClass.TPV) {
      return scope.chooseTOMAForTPVEncryption(volume, callback);
  }
  ```

### Step 3. Attach / detach orchestration — `nvmesh-management/modules/volumeEncryption.js`

- `attachTPVToTOMAForEncryption(dbVolume, executingTOMA, cb)`:
  1. Look up the `client` document whose `_id` matches `executingTOMA._id` (TOMA nodes also run the client kernel module and register as clients).
  2. Call `clientModule.attachTPV(executingTOMA._id, clientDoc.uuid, dbVolume._id, { preempt: true }, ...)`. `syncFlush` is deliberately omitted — `attachTPV`'s `applySyncFlush` unconditionally persists the TPV's `sourceUUID`, and any override here would leak past the post-encryption detach.
  3. Reload the TPV; verify `tpvConfig.exclusiveClient === executingTOMA._id`. Surface a retriable error otherwise.
- `detachTPVFromTOMAForEncryption(tpvName, tomaId, cb)` — symmetric: `clientModule.detachTPV(tomaId, clientDoc.uuid, tpvName, ...)`. Best-effort; `cleanupTPVAutoAttachesAfterStartup` covers strays.
- In `runEncryptionCommand`, insert an attach step between `chooseTOMAForEncryption` and `setEncryptionCommand` when `dbVolume.volumeClass === 'TPV'`.
- `setEncryptionCommand` stamps `encryption.command.tpvAutoAttachedTOMA = executingTOMA._id` for TPVs so response handling and recovery can find it.
- `handleCommandResponse`: after the status update, if the volume is a TPV and `encryption.command.tpvAutoAttachedTOMA` is set, call `detachTPVFromTOMAForEncryption` and `$unset` the field. No action for regular volumes.
- `cleanupTPVAutoAttachesAfterStartup`: scan for TPVs with `encryption.command.tpvAutoAttachedTOMA` present and `status === EXECUTED`; drive the detach for each. Invoked alongside `resendStaleEncryptionCommands` from `sanityAndRecover.js`.

**The Kafka envelope is unchanged.** No new fields on `EncryptionCommandMessage`, no `cdvName` / `cdvByteOffset` / `shadowSizeSectors`, no subclass changes.

### Step 4. Backend integration test — `nvmesh-management/test/`

- `POST /volumes/save` with an encrypted TPV payload → DB record `{isReady: false, isEncrypted: true, encryption: {isInitialized: false, headerSize: 16}, action: "initEncryptionRequired"}`. No `firstExtentIndex`.
- Stub the TOMA Kafka producer + the `clientModule.attachTPV/detachTPV` helpers.
- `POST /volumes/initEncryption` → verify `attachTPV` is called with `preempt: true` on the chosen TOMA; verify `tpvConfig.exclusiveClient` matches; verify `encryption.command.tpvAutoAttachedTOMA` is stamped; verify the Kafka message uses the standard envelope.
- Inject a synthetic `encryptionCommandResponse` → DB transitions to `{isReady: true, encryption.isInitialized: true}`; verify `detachTPV` is called and `tpvAutoAttachedTOMA` is cleared.
- Negative case: `isEncrypted: true` on a `CDV` payload → rejected.
- Crash test: between `setEncryptionCommand` and `sendEncryptionCommandToTOMA`, kill the request → on resend, `cleanupTPVAutoAttachesAfterStartup` (for EXECUTED commands) and `resendStaleEncryptionCommands` (for PENDING/SENT) converge the state.

---

## Phase 2 — UI

### Step 5. `CreateTPVModal.jsx` encryption fields

- Add an `isEncrypted` checkbox (disabled on edit; TPVs are immutable post-creation for everything but `description` and `virtualSizeGB`).
- When `isEncrypted` is checked, reveal an `encryption.headerSize` number input (MB, default 16, min 1, max 100).
- Pass the fields through in `onFormSubmit()`.

### Step 6. `ThinProvisioning.jsx` encryption toolbar and column

- Copy encryption state, handlers, and modal wiring from `Volumes.jsx`. Reuse the existing `InitEncryptionModal` and `PassphraseModal` components verbatim.
- Add a `<DropdownButton label="Encryption">` containing Init/Add/Rotate/Delete/Ack, with disable logic derived from `selectedTPVs`.
- Add an `Encryption` column rendering:
  - `—` when `!isEncrypted`
  - `Init Required` (yellow) when `isEncrypted && !encryption.isInitialized`
  - `In Progress` (blue) when `command.status` is `sent` or `pendingSend`
  - `Error` (red) when `command.response.error && !command.response.acknowledged`
  - `Encrypted` (green) otherwise.

### Step 7. UI integration test

- Create an encrypted TPV → row appears with `Init Required`.
- Click Init Encryption → modal → submit → verify row transitions to `In Progress` then `Encrypted` after the stubbed response.
- Add / Rotate / Delete passphrase → per-op modal variations.
- Trigger an error response → row flips to `Error` → Acknowledge Error clears it.

---

## Phase 3 — TOMA

### Step 8. Branch in `start_encrypt_action` — `nvmesh-kernel/toma/nvmeibt_kafka.c`

- Detect TPV via `(vol->from_config.n_chunks == 0) && !vol->from_config.is_cdv`. `struct nvmeibt_block_device` on the TOMA has no kernel-client `type` enum; TPVs are distinguishable from every other class because only they arrive with `chunks: []` (management's `createTPV` serialises `n_chunks = 0`). No new parsing, no new `encrypt_cmd_t` fields, no new JSON keys.
- Build the `cryptsetup` command against `/dev/nvmesh/<vol->from_config.client_blkdev_name>` instead of `/dev/nvmesh/<shadow>`.
- Call `nvmeibt_start_encrypt_for_tpv(vol, encrypt_params)` instead of `nvmeibt_attach_vol_for_encryption`.

### Step 9. TPV exec entry point — `nvmesh-kernel/toma/nvmeibt_recovery.c` / `nvmeibt_recovery.h`

- `nvmeibt_start_encrypt_for_tpv(vol, encrypt_params)` (new, exported):
  - Sets `encrypt_params->exec_ctx.blkdev = vol`, `origin_vol = vol`, `vol->encrypt_params = encrypt_params`.
  - Allocates `child_stdout_buf` / `child_stderr_buf`; sets `exec_ctx->run_exec_on_blkdev_cb_func = tpv_encrypt_after_exec_cb`; sets `timeout_ms = 100000`.
  - Calls `nvmeibt_run_exec_on_blkdev(exec_ctx)` directly — no WQ attach, no shadow.
- `tpv_encrypt_after_exec_cb(exec_ctx)` (new, static):
  - Mirrors the response-generation half of `detach_shadow_vol_for_encryption_finalize` (`nvmeibt_kafka_send_encrypt_cmd_response` by `toma_rv` / `exec_rv`).
  - Frees the stdout/stderr buffers and `encrypt_params`; clears `vol->encrypt_params`.
  - **Does NOT free `vol`** — it is the real TPV, still owned by TOMA's block-device hash and still attached (management detaches it after the Kafka response).

### Step 10. TOMA integration test

- Pre-condition the TPV as "already attached to this TOMA" (simulating the management-issued preempt attach).
- Send a synthetic `initEncryption` Kafka message with the standard envelope.
- Verify `cryptsetup luksFormat` runs against `/dev/nvmesh/<name>` (no dm-linear).
- Verify the client-side TPV allocator allocates CDV extents on demand during the LUKS header write.
- Verify the response Kafka message carries the correct result code.
- Repeat for `addPassphrase`, `deletePassphrase`, `rotatePassphrase`.

---

## Phase 4 — CLI

### Step 11. TPV entity encryption params and ops — `nvmesh-infra/xlro/core/entities/rest.yaml`

Three edits on the `TPV` entity:

1. **Create-time params.** Append to `TPV.ops.create.params`:
   ```yaml
   - isEncrypted
   - encryption          # expands to --encryption-header-size via SdkObject traversal
   ```

2. **Encryption operations.** Duplicate the four ops from `Volume.ops` (API v8) into `TPV.ops` verbatim — the route `/volumes/<op>` is shared at the REST layer and `volumeEncryption.js` branches on `volumeClass`:
   - `initEncryption` (opts: `passphrase` required, `slot` int, `numberOfSlots` int, `keySize` choices 256/512)
   - `addPassphrase` (opts: `currentPassphrase`, `newPassphrase` required; `slot` int)
   - `deletePassphrase` (opts: `currentPassphrase` required; `slot` int)
   - `rotatePassphrase` (opts: `currentPassphrase`, `newPassphrase` required; `slot` int)

3. **Display fields.** Append to `TPV.display`: `isEncrypted`, `encryption`.

### Step 12. Regenerate golden files and smoke-test

- Regenerate `nvmesh-infra/xlro/tools/cli/current.api` and `current.display`.
- Smoke tests against a dev management server:
  - `nvmesh tpv create --is-encrypted --encryption-header-size 16 --cdv <cdv> --capacity 10GiB …` → TPV created with `isReady: false, isEncrypted: true`.
  - `nvmesh tpv initEncryption <tpv> --passphrase … --slot 1 --key-size 512` → DB transitions as in Step 4.
  - `nvmesh tpv addPassphrase …`, `rotatePassphrase …`, `deletePassphrase …` → each request/response lifecycle.
  - `nvmesh tpv show <tpv>` → Encryption column reflects init + command state.

No changes to `rest_custom.py`, the SDK `TPV` entity class, or `cli.py`.

---

## Phase 5 — CSI Driver

### Step 13. Thread `secrets` and parse StorageClass encryption — `nvmesh-csi-driver/driver/controller_service.py`

- Thread `secrets` into `_do_create_tpv(...)`.
- Parse `encryption: dmcrypt` + `encryption.headerSize` from StorageClass `parameters` after `_parse_tpv_params`; set `tpv.isEncrypted` / `tpv.encryption`.

### Step 14. Post-create init-encryption — `controller_service.py`

- Build `volume_context` before the init-encryption branch.
- After `create_tpv()` resolves `volume_uuid`, if `tpv.isEncrypted` and `apiVersion >= API_VERSION_9`, call `init_encrypted_volume(secrets, zone, volume_context, log)`. On exception, roll back via `NVMeshMgmtAPI.delete_tpv(...)` and re-raise.
- `init_encrypted_volume` body is unchanged — it polls for init-required, POSTs `/volumes/initEncryption`, waits for ready. Management handles TOMA selection, attach, detach (Steps 2–3).

### Step 15. `delete_tpv` management helper — `nvmesh-csi-driver/driver/nvmesh_mgmt_api.py`

- `delete_tpv(volume_api, name, zone, log, backoff)` POSTs `/volumes/tpv/delete`; mirror `delete_volume`'s retry / backoff shape.

### Step 16. StorageClass example — `deploy/kubernetes/helm/nvmesh-csi-driver/templates/storage-classes.yaml`

```yaml
parameters:
  volumeClass: TPV
  cdvNameRegex: "^pool-gold-"
  tpvExtentSizeKB: "256"
  encryption: dmcrypt
  encryption.headerSize: "16"
  csi.storage.k8s.io/node-publish-secret-name: nvmesh-tpv-secret
  csi.storage.k8s.io/node-publish-secret-namespace: default
```

No node-side changes: `node_service.py` gates LUKS open on `"encryption" in volume.metadata`, already populated from `volume_context`.

### Step 17. Integration tests — `nvmesh-csi-driver/test/integration/`

- `test_encrypted_tpv_create.py` — PVC bound; management DB shows `isReady: true, isEncrypted: true, encryption.isInitialized: true`; LUKS magic header readable at offset 0 of the TPV block device.
- `test_encrypted_tpv_rollback.py` — force a secret-fetch failure; CSI returns `InvalidArgument`; no TPV remains; CDV `tpvCount` unchanged.
- `test_encrypted_tpv_attach.py` — pod scheduled; `/dev/mapper/<vol>` exists; I/O succeeds; detach cleans up dm-crypt.
- `test_encrypted_tpv_delete.py` — delete PVC while encrypted → TPV removed via `/volumes/tpv/delete`; `CDVAllocatorFreeAll` observed.

No sanity-suite changes — `nvmesh-cluster-sim` does not model encryption.

---

## Phase 6 — End-to-end

### Step 18. Full-flow verification

1. **UI flow:** create CDV → create encrypted TPV from `CreateTPVModal` → `Init Encryption` → verify TPV is briefly attached to the chosen TOMA (`tpvConfig.exclusiveClient === toma._id`) during the run, then detached → attach to a client → write/read → Rotate Passphrase → Delete TPV.
2. **CLI flow:** create CDV → `nvmesh tpv create --is-encrypted …` → `nvmesh tpv initEncryption …` → attach via `nvmesh client attach` → `nvmesh tpv addPassphrase / deletePassphrase` → `nvmesh tpv delete`.
3. **CSI flow:** deploy encrypted-TPV StorageClass → PVC → pod writes → delete pod → delete PVC → DB cleanup.

### Error scenarios

- Chosen TOMA not registered as a client (rare — operator removed the client kernel module) → encryption command fails with a diagnostic "TOMA node not registered as a client"; management falls back to another TOMA on retry.
- Chosen TOMA dies mid-cryptsetup → the Kafka command times out or responds with `TOMA_ERR`; management marks the command failed, `cleanupTPVAutoAttachesAfterStartup` drives the detach on TOMA recovery / management restart.
- Management dies between response receipt and detach → on restart, `cleanupTPVAutoAttachesAfterStartup` scans for `encryption.command.tpvAutoAttachedTOMA` and detaches.
- CDV full during LUKS header write → cryptsetup exits non-zero; response is `CMD_ERR`; TPV ends in `action: initEncryptionRequired` with an acknowledged error.
- CSI secret missing / wrong passphrase → CSI rollback deletes the TPV (Step 15).
- TPV extend after init → LUKS header stays at offset 0 of the TPV's address space; extend succeeds; passphrase ops still target the header and succeed.

---

## Risks and open questions

1. **TOMA must be registered as a client.** The attach uses the existing client-attach path and requires a `client` document with `_id` = TOMA hostname. Standard NVMesh clusters run the client kernel module on every TOMA node; verify in the deployment runbook.
2. **Preempt fences a live client.** A real client holding the TPV at the moment an encryption command is run will be preempted. Acceptable for fresh-TPV `initEncryption`; for later passphrase ops, the operator is responsible for quiescing the TPV. A stricter interlock (reject if `exclusiveClient` is a non-TOMA client) can be layered in later.
3. **Crash window: management dies after detach-cmd-sent but before Mongo update.** The TPV is detached but `tpvAutoAttachedTOMA` is still set. `cleanupTPVAutoAttachesAfterStartup` re-issues detach — idempotent, no harm.
4. **Client-side LUKS open at attach** — same flow as regular encrypted volumes. Verify the client agent's cryptsetup invocation handles both `/dev/nvmesh/` and `/dev/nvmesh/` device prefixes.
5. **Passphrase ops after extend** — LUKS header is pinned to TPV offset 0; unaffected by subsequent extent allocations.
6. **CLI duplication cost** — copying four encryption ops into `TPV` adds ~60 yaml lines. Acceptable.
7. **CSI rollback completeness** — `delete_tpv` must tolerate the TPV already being gone.
8. **CSI `secrets` propagation** — local refactor, single caller (`do_create_volume`).
9. **Sanity vs. integration split** — all encrypted-TPV tests live under `test/integration/`; `nvmesh-cluster-sim` does not model encryption.

---

## Done criteria

- All 18 steps merged behind feature flag or in a single feature branch.
- `current.api` and `current.display` CI checks pass.
- Triangulated E2E scenarios in Step 18 pass on a three-node NVMesh cluster with both IB and RoCE transports.
- `TPV_ThinProvisioningImplementation.md` §5 updated (done) and cross-linked from this plan (done).
