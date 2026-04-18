# TPV Encryption — Full Execution Plan

**Scope:** everything required to ship encrypted Thin-Provisioned Volumes (TPVs) end-to-end: management backend, Web UI, TOMA kernel-space, NVMesh CLI, and the Kubernetes CSI driver.

**Reference:** `nvmesh-kernel/design/ThinProvisioningImplementation.md` Part 5 is the authoritative design. This document is the execution plan derived from it.

**Governing invariant (Architecture Decision #18):** CDVs and `<CDV>-mgmt` satellites are **never** encrypted at the NVMesh volume layer. Encryption is per-TPV — a LUKS container living inside the TPV's CDV extents, with the LUKS header in the TPV's first pre-allocated extent. Management refuses to enable encryption on any volume of class `CDV` or `CDV_MGMT`.

---

## Phase map

| Phase | Steps | Scope |
|-------|-------|-------|
| 1 — Backend        | 1–4   | `createTPV()` encryption support, TOMA selection, Kafka message extension, backend test |
| 2 — UI             | 5–7   | `CreateTPVModal` encryption fields, `ThinProvisioning.jsx` toolbar + handlers, UI test |
| 3 — TOMA           | 8–10  | Kafka parsing extension, dm-linear shadow path, TOMA test |
| 4 — CLI            | 11–12 | `rest.yaml` TPV encryption params + ops, golden-file refresh, CLI smoke test |
| 5 — CSI Driver     | 13–17 | StorageClass parsing, post-create init-encryption, `delete_tpv` rollback helper, SC example, integration tests |
| 6 — End-to-end     | 18    | UI / CLI / CSI triangulated flow + error scenarios |

Phases 1–3 are the kernel/server core (pre-existing plan). Phases 4–5 are the management-surface additions that make the feature reachable from both the operator CLI and the Kubernetes provisioning path.

---

## Phase 1 — Management Backend

### Step 1. `createTPV()` encryption support — `nvmesh-management/modules/volume.js`

- Extend `createTPV()` to accept `isEncrypted: boolean` and `encryption: { headerSize: number }` on the input payload.
- On the encrypted path, before inserting the TPV document:
  1. Validate `isEncrypted === true` requires `volumeClass === 'TPV'` (reject on CDV / CDV_MGMT with HTTP 400).
  2. Run `preAllocateFirstExtent(volume, cb)` — send a `CDV_ALLOC_EXTENT` IB admin message to the CDV allocator TOMA for this TPV's UUID. On success, capture the returned extent index.
  3. If the allocator is not yet elected (see `project_cdv_auto_attach.md` — the window between CDV attach and first topology push), fail with a retriable error; the user retries once TOMA is up.
- Insert the TPV document with:
  - `isEncrypted: true`
  - `encryption: { headerSize: <requested or 16>, isInitialized: false }`
  - `isReady: false` (flips to `true` only after `initEncryption` completes)
  - `tpvConfig.firstExtentIndex: <returned index>`

### Step 2. TOMA selection for TPV encryption — `nvmesh-management/modules/volumeEncryption.js`

- Implement `chooseTOMAForTPVEncryption(tpvVolume, cb)`:
  1. Look up the parent CDV via `tpvConfig.cdvId`.
  2. Find candidate TOMA nodes: servers in the CDV's first pRAID with `tomaStatus === UP`.
  3. Prefer the CDV's current allocator TOMA (`cdvAllocator.tomaId`); fall back to a random pick from the remaining candidates.
  4. Return `{tomaId, bootTime, topics}`.
- In `chooseTOMAForEncryption()`, add the branch:
  ```js
  if (volume.volumeClass === consts.volumeClass.TPV) {
      return scope.chooseTOMAForTPVEncryption(volume, callback);
  }
  ```
  (the existing zone-round-robin path remains the default.)

### Step 3. Kafka message extension — `nvmesh-management/models/kafkaMessages/EncryptionCommandMessage.js`

- Extend `EncryptionCommandMessage.toJSON()` to include four optional fields when present: `cdvName`, `cdvUUID`, `cdvByteOffset`, `shadowSizeSectors`.
- No subclass changes (`InitEncryption.js`, `AddPassphrase.js`, `DeletePassphrase.js`, `RotatePassphrase.js` inherit from the base).
- In `volumeEncryption.sendEncryptionCommandToTOMA()`, compute and set these on the command object when `dbVolume.volumeClass === 'TPV'`:
  ```js
  const cdv = await volumeCollection.findOne({ _id: dbVolume.tpvConfig.cdvId });
  const A = cdv.cdvConfig.allocatorSizeGB * 1024 * 1024 * 1024;
  const E = cdv.cdvConfig.cdvExtentSizeMB * 1024 * 1024;
  encryptionObj.cdvName         = cdv._id;
  encryptionObj.cdvUUID         = cdv.uuid;
  encryptionObj.cdvByteOffset   = A + dbVolume.tpvConfig.firstExtentIndex * E;
  encryptionObj.shadowSizeSectors = E / 512;
  ```

### Step 4. Backend integration test — `nvmesh-management/test/`

- `POST /volumes/save` with an encrypted TPV payload → DB record `{isReady: false, isEncrypted: true, encryption: {isInitialized: false, headerSize: 16}, tpvConfig.firstExtentIndex: <n>}`.
- Stub the TOMA Kafka producer and capture published messages.
- `POST /volumes/initEncryption` → captured message carries `cdvName`, `cdvUUID`, `cdvByteOffset`, `shadowSizeSectors`.
- Inject a synthetic `encryptionCommandResponse` → DB transitions to `{isReady: true, encryption.isInitialized: true}`.
- Negative case: `isEncrypted: true` on a `CDV` payload → HTTP 400.

---

## Phase 2 — UI

### Step 5. `CreateTPVModal.jsx` encryption fields

- Add an `isEncrypted` checkbox (disabled on edit; TPVs are immutable post-creation for everything but `description` and `virtualSizeGB`).
- When `isEncrypted` is checked, reveal an `encryption.headerSize` number input (MB, default 16, min 1, max 100).
- Pass the fields through in `onFormSubmit()` exactly as `CreateEditVolumeModal.jsx` already does for regular volumes.

### Step 6. `ThinProvisioning.jsx` encryption toolbar and column

- Copy encryption state variables, handlers, and modal wiring from `Volumes.jsx`:
  - State: `showInitEncryptionModal`, `showPassphraseModal`, `initData`, `passphraseCommandName`, `passphraseData`.
  - Handlers: `handleInitEncryption`, `handleInitEncryptionSubmit`, `handleAddPassphrase`, `handleRotatePassphrase`, `handleDeletePassphrase`, `handlePassphraseSubmit`, `handleAckEncryptionError`.
  - Render `<InitEncryptionModal/>` and `<PassphraseModal/>` at the bottom of the page.
- Add an `<DropdownButton label="Encryption">` containing the five items, with disable logic derived from `selectedTPVs` (admin only; all selected rows must be `isEncrypted` and match the per-item state machine).
- Add an `Encryption` column rendering:
  - `—` when `!isEncrypted`
  - `Init Required` (yellow) when `isEncrypted && !encryption.isInitialized`
  - `In Progress` (blue) when `command.status` is `sent` or `pendingSend`
  - `Error` (red) when `command.response.error && !command.response.acknowledged`
  - `Encrypted` (green) otherwise.

### Step 7. UI integration test

- Create an encrypted TPV from the modal → row appears with `Init Required`.
- Click Init Encryption → modal → submit → verify row transitions to `In Progress`, then `Encrypted` after the stubbed response.
- Add / Rotate / Delete passphrase → per-op modal variations, success and error banners.
- Trigger an error response → row flips to `Error` → Acknowledge Error clears it.

---

## Phase 3 — TOMA

### Step 8. Kafka parsing extension — `nvmesh-kernel/toma/nvmeibt_kafka.c`

- Extend `parse_CMD()` to extract the four optional fields from the JSON payload.
- Add to `encrypt_cmd_t`:
  ```c
  char     cdv_name[MAX_VOL_NAME_LEN];
  char     cdv_uuid[UUID_STR_LEN];
  uint64_t cdv_byte_offset;
  uint64_t shadow_size_sectors;
  bool     is_tpv_encryption;         // set when cdv_name[0] != '\0'
  ```
- Preserve backward compat: when the fields are absent (regular volume), `cdv_name[0] == '\0'` and `is_tpv_encryption == false`.

### Step 9. dm-linear shadow path — `nvmeibt_kafka.c` / `nvmeibt_recovery.c`

- In `start_encrypt_action()`: branch on `is_tpv_encryption`.
- TPV branch:
  1. Verify `/dev/nvmesh/<cdv_name>` exists (CDV attached locally).
  2. Construct table: `"0 <shadow_size_sectors> linear /dev/nvmesh/<cdv_name> <start_sector>"` where `start_sector = cdv_byte_offset / 512`.
  3. `dmsetup create tpv_enc_<volume_name> --table "<table>"`.
  4. Wait for `/dev/mapper/tpv_enc_<volume_name>` (bounded poll).
  5. Run `cryptsetup` (same command builder as regular encryption) against `/dev/mapper/tpv_enc_<volume_name>`.
  6. On both success and failure: `dmsetup remove tpv_enc_<volume_name>` (idempotent cleanup in a `goto out:` block).
- Passphrase file handling, response struct building, and error codes: unchanged.
- Use `_NI`/`_NW`/`_NE` tracing macros — no `pr_*` (per project convention, see `feedback_kernel_tracing.md`).

### Step 10. TOMA integration test

- Send a synthetic `initEncryption` Kafka message carrying all four CDV fields.
- Verify:
  - `dmsetup create` runs with expected table string.
  - `cryptsetup luksFormat` references `/dev/mapper/tpv_enc_<name>`.
  - `dmsetup remove` runs in both success and failure paths.
  - Response Kafka message carries the correct result code.
- Repeat for `addPassphrase`, `deletePassphrase`, `rotatePassphrase`.

---

## Phase 4 — CLI

### Step 11. TPV entity encryption params and ops — `nvmesh-infra/xlro/core/entities/rest.yaml`

Three edits on the `TPV` entity (~ line 1265):

1. **Create-time params.** Append to `TPV.ops.create.params`:
   ```yaml
   - capacity
   - tpvConfig
   - isEncrypted
   - encryption          # expands to --encryption-header-size via SdkObject traversal
   ```

2. **Encryption operations.** Duplicate the four ops from `Volume.ops` (API v8, `rest.yaml:794–863`) into `TPV.ops` verbatim — the route `/volumes/<op>` is shared at the REST layer and `volumeEncryption.js` branches on `volumeClass`:
   - `initEncryption` (opts: `passphrase` required, `slot` int, `numberOfSlots` int, `keySize` choices 256/512)
   - `addPassphrase` (opts: `currentPassphrase`, `newPassphrase` required; `slot` int)
   - `deletePassphrase` (opts: `currentPassphrase` required; `slot` int)
   - `rotatePassphrase` (opts: `currentPassphrase`, `newPassphrase` required; `slot` int)

   All four use `style: entities` and the `[{ '_id': '{{e.name}}', 'uuid': '{{e.uuid}}', … }]` payload template.

3. **Display fields.** Append to `TPV.display`:
   ```yaml
   - isEncrypted
   - encryption
   ```

### Step 12. Regenerate golden files and smoke-test

- Regenerate `nvmesh-infra/xlro/tools/cli/current.api` and `current.display` (CI gates on these).
- Smoke tests (against a dev management server):
  - `nvmesh tpv create --is-encrypted --encryption-header-size 16 --cdv <cdv> --capacity 10GiB …` → TPV created with `isReady: false, isEncrypted: true`.
  - `nvmesh tpv initEncryption <tpv> --passphrase … --slot 1 --key-size 512` → DB transitions as in Step 4.
  - `nvmesh tpv addPassphrase …`, `rotatePassphrase …`, `deletePassphrase …` → verify each command's request/response lifecycle.
  - `nvmesh tpv show <tpv>` → Encryption column reflects init + command state.

No changes to `rest_custom.py` (`TPVGroup` already injects `volumeClass: 'TPV'` on create and routes `update`/`delete` correctly), to the SDK `TPV` entity class (`isEncrypted` / `encryption` are inherited from the common volume schema), or to `cli.py`.

---

## Phase 5 — CSI Driver

### Step 13. Thread `secrets` into `_do_create_tpv` and parse StorageClass encryption params — `nvmesh-csi-driver/driver/controller_service.py`

- Change the early-branch call at line 115 to pass `secrets`:
  ```python
  if reqDict.get("parameters", {}).get(Consts.TPVParams.VOLUME_CLASS) == Consts.VolumeClass.TPV:
      return self._do_create_tpv(log, nvmesh_vol_name, reqDict, capacity_bytes, vol_metadata, topology_requirements, secrets)
  ```
- Add `secrets` to the `_do_create_tpv` signature.
- After `_parse_tpv_params` (line 251) and before building `tpv`, parse the StorageClass encryption hints:
  ```python
  if parameters.get("encryption") == "dmcrypt":
      tpv_is_encrypted = True
      header_size = parameters.get("encryption.headerSize")
  elif parameters.get("encryption"):
      log.warning(f'Unknown encryption type "{parameters["encryption"]}" for TPV {nvmesh_vol_name}; ignoring')
      tpv_is_encrypted = False
      header_size = None
  else:
      tpv_is_encrypted = False
      header_size = None
  ```
- When constructing the `NVMeshVolume`, set:
  ```python
  if tpv_is_encrypted:
      tpv.isEncrypted = True
      if header_size is not None:
          tpv.encryption = {"headerSize": int(header_size)}
  ```

### Step 14. Post-create init-encryption call — `controller_service.py`

- Move `volume_context` construction (currently at line 294) **above** the init-encryption branch so it is populated before the call.
- Immediately after `create_tpv()` resolves `volume_uuid` and after `volume_context` is built, add:
  ```python
  if getattr(tpv, "isEncrypted", False) and volume_api.apiVersion >= Consts.ApiVersion.API_VERSION_9:
      try:
          self.init_encrypted_volume(secrets, zone, volume_context, log)
      except Exception as ex:
          log.error(
              f"Failed to initialize encryption for TPV {nvmesh_vol_name} in zone {zone}. "
              f"Rollback - Deleting TPV. Error: {ex}"
          )
          NVMeshMgmtAPI.delete_tpv(
              volume_api, nvmesh_vol_name, zone, log,
              backoff=BackoffDelayWithStopEvent(self.stop_event, **Config.DELETE_VOLUME_BACKOFF),
          )
          raise
  ```
- `init_encrypted_volume()` body needs no change: it polls `wait_for_encrypted_volume_to_require_init`, calls `do_init_encryption` (which POSTs `/volumes/initEncryption`), then `wait_for_encrypted_volume_to_be_ready`. Management handles TOMA selection internally (Step 2).

### Step 15. `delete_tpv` management helper — `nvmesh-csi-driver/driver/nvmesh_mgmt_api.py`

- Add a `delete_tpv(volume_api, name, zone, log, backoff)` function that POSTs `/volumes/tpv/delete` with payload `[{ "_id": name }]`.
- Mirror `delete_volume`'s retry / backoff / zone-refresh shape.
- Do **not** reuse `delete_volume` — the routes differ and management validates `volumeClass` on the TPV route.

### Step 16. StorageClass example and Helm chart — `deploy/kubernetes/helm/nvmesh-csi-driver/templates/storageclass.yaml`

Add an encrypted-TPV variant:
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
No node-side changes: `node_service.py` line 755 (`is_encrypted_volume = "encryption" in volume.metadata`) already gates LUKS open on the presence of the StorageClass parameter, which `_do_create_tpv` already copies into `volume_context` at line 305.

### Step 17. Integration tests — `nvmesh-csi-driver/test/integration/`

- `test_encrypted_tpv_create.py` — PVC bound; management DB shows `isReady: true, isEncrypted: true, encryption.isInitialized: true`; first CDV extent contains a LUKS magic header.
- `test_encrypted_tpv_rollback.py` — force a secret-fetch failure (missing `csi.storage.k8s.io/node-publish-secret-name`); CSI returns `InvalidArgument`; no TPV remains in the DB; CDV `tpvCount` is unchanged from baseline.
- `test_encrypted_tpv_attach.py` — pod scheduled with the PVC; `/dev/mapper/<vol>` exists on the node; file I/O succeeds; detach cleans up the dm-crypt target.
- `test_encrypted_tpv_delete.py` — delete PVC while encrypted → TPV removed via `/volumes/tpv/delete` (not `/volumes/delete`); `CDVAllocatorFreeAll` Kafka message observed.

No sanity-suite changes — `nvmesh-cluster-sim` does not model encryption.

---

## Phase 6 — End-to-end

### Step 18. Full-flow verification

Triangulate the interfaces so each is exercised at least once:

1. **UI flow:** create CDV → create encrypted TPV from `CreateTPVModal` → `Init Encryption` from `ThinProvisioning.jsx` toolbar → attach to a client → write + read data → Rotate Passphrase → Delete TPV.
2. **CLI flow:** create CDV → `nvmesh tpv create --is-encrypted …` → `nvmesh tpv initEncryption …` → attach via `nvmesh client attach` → `nvmesh tpv addPassphrase / deletePassphrase` → `nvmesh tpv delete`.
3. **CSI flow:** deploy encrypted-TPV StorageClass → create PVC → pod writes to encrypted volume → delete pod → delete PVC → verify DB cleanup.

### Error scenarios

- TOMA holding the CDV allocator is down → encrypted-TPV create fails with a retriable error; retry once TOMA is back up succeeds (tests Step 1 pre-allocation path).
- CDV full at pre-allocation time → create fails with a capacity error; no partial TPV in DB.
- Concurrent `initEncryption` commands on two TPVs sharing the same CDV on the same TOMA → both succeed; `dmsetup` names don't collide because `tpv_enc_<volume_name>` is unique per TPV.
- CSI secret missing / wrong passphrase → rollback path exercised (Step 17 `test_encrypted_tpv_rollback.py`).
- Kill management mid-`initEncryption` → on restart, TPV is `isReady: false, isInitialized: false`; retry `initEncryption` completes successfully (existing command-idempotency from regular-volume encryption).
- TPV extend after init → LUKS header is in the first (pre-allocated) extent; new CDV extents allocated on extend carry no LUKS metadata; extend succeeds and the client observes the larger virtual size. Re-run `rotatePassphrase` after extend — header still at offset 0, passphrase op succeeds.

---

## Risks and open questions

Carried from `ThinProvisioningImplementation.md` §5.15 and extended with CLI/CSI-specific risks:

1. **First-extent pre-allocation timing** — async dependency on TOMA availability during `createTPV`. Documented limitation; retriable.
2. **dm-linear naming collisions** — none (TPV names are unique per MongoDB `_id`).
3. **Client-side LUKS open on attach** — same flow as regular encrypted volumes; verify at attach time that `/dev/nvmesh/<tpv_name>` is reachable to the client agent.
4. **CDV extent 0 conflict** — allocator L1 lives at CDV data-extent 0; the pre-allocated encryption extent is a data extent with index ≥ 0 returned by `CDV_ALLOC_EXTENT`, so no conflict.
5. **Passphrase ops after extend** — LUKS header is pinned to the first extent; unaffected by subsequent allocations.
6. **CLI duplication cost** — copying four encryption ops into `TPV` adds ~60 yaml lines. Acceptable; the alternative (force CLI users to `nvmesh volume initEncryption <tpv>`) is a UX regression against the `Volumes.jsx` / `ThinProvisioning.jsx` UI split.
7. **CSI rollback completeness** — `delete_tpv` must be idempotent and tolerate the TPV already being gone (e.g. if the failure occurred after DB insert but before response). Mirror `delete_volume`'s tolerant error handling.
8. **CSI `secrets` propagation** — adding `secrets` to `_do_create_tpv`'s signature is a local refactor; no callers other than `do_create_volume` (line 115).
9. **Sanity vs. integration split** — `nvmesh-cluster-sim` does not model encryption; all encrypted-TPV tests live under `test/integration/` and require a real NVMesh cluster.

---

## Done criteria

- All 18 steps merged behind feature flag (or in a single feature branch), each with unit / integration coverage.
- `current.api` and `current.display` CI checks pass.
- Triangulated E2E scenarios in Step 18 pass on a three-node NVMesh cluster with both `IB` and `RoCE` transports.
- `ThinProvisioningImplementation.md` §5 updated and cross-linked from this plan (already done).
