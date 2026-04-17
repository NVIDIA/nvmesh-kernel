# Embed CDV Allocator Identity in RAFT — Implementation Plan

## Overview

Move `(allocator_toma_id, allocator_generation)` out of the bespoke unicast delivery path (`RAFT_MSG_CDV_ALLOC_NOTIFY` + receive-side monotonicity guard) and into the RAFT-replicated pRAID topology record. The leader writes the fields during `nvmeibt_topology_calc_topology`; RAFT `AppendEntries` delivers them to every follower via the existing TOPO TLV; every TOMA converges on the allocator identity through the same linearized topology-apply path that already carries all other pRAID state changes.

The design rationale is in `ThinProvisioningImplementation.md` §2.6. This document is the **implementation plan** and supersedes every mechanism listed in §2.6 "What this supersedes".

---

## Problem Statement

The current design elects an allocator on the RAFT leader, then delivers the decision to the chosen TOMA via a single unicast `RAFT_MSG_CDV_ALLOC_NOTIFY`. The delivery is best-effort: if the unicast is lost, dropped, or arrives before the chosen TOMA's receive dispatch is ready, `handle_notify` never runs, the chosen TOMA never transitions to `AWAITING_SATELLITE_ATTACH`, Stage A (`attachSatelliteRequest`) never fires, and the satellite is never attached to the allocator. There is no retry mechanism and no leader-side verification.

Observed failure (cold-start, fresh CDV, 4-node cluster, commit `0f4db106`):

- Leader elects a non-leader TOMA; unicast never observed by the recipient.
- Leader's `push_to_registrants` nevertheless tells its local clients "allocator=node3".
- Clients send `CDV_LIST_EXTENTS` to node3.
- `handle_cdv_list_extents → nvmeibt_cdv_alloc_list_for_tpv → find_or_create_alloc` lazily creates an `alloc` entry on node3 with empty `allocator_toma_id`, empty `satellite_uuid`, `state=NOT_ALLOCATOR`. The scan then falls back to the legacy CDV path `/dev/nvmesh/<cdv>` — unattached — and retries the `ENOENT` forever.
- Sticky rule on subsequent leader heartbeats keeps re-affirming node3. No new unicast is sent. The cluster is stuck.

Patching the unicast path (retries, ACKs, leader-driven polling) would reintroduce a second consensus channel in parallel with RAFT. The correct fix is to stop running a second channel.

---

## Goals

1. Allocator identity is a property of the RAFT-committed topology, not a separate in-memory cache reachable only through a custom message.
2. Every TOMA — not just the leader and the elected allocator — converges on the same allocator identity at the same RAFT log index.
3. Stage A (`attachSatelliteRequest`) is triggered exclusively from the topology-apply path. No client request path can bootstrap allocator state.
4. Zero custom retry / ACK / poll logic. RAFT's existing `AppendEntries` replay handles delivery.
5. Cold start, re-election, and network partition behavior are subsumed by existing RAFT semantics (quorum gate, log-ordered apply).

---

## Non-goals

- Changing the satellite-attach handshake with management (Stage A/B over Kafka). That remains as specified in `SatelliteVolumeForCDVAlloc.md`.
- Changing the client-side `CDV topology push` (TOMA → client). Clients continue to learn the allocator identity via the existing topology push.
- Making the allocator identity survive independently of the pRAID topology. The CDV on-disk header already holds a durable copy; nothing changes there.
- Fixing the "dead allocator, candidates not re-derived from alive RAFT members" defect. That is covered by the separate RAFT-members-as-candidates change.

---

## Design

### State schema

Two new fields on the pRAID topology record. They are meaningful only for a CDV's first pRAID (`stripe_idx == 0`, `blkdev->from_config.is_cdv`); on every other pRAID they are zero-initialized and ignored.

**In-memory** (`toma/nvmeibt_praid_basics.h`):

```c
struct nvmeibt_praid_topo_ctx {
    /* ...existing fields... */
    char     allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];   // "" if not a CDV pRAID
    uint64_t allocator_generation;                          //  0 if unset
};
```

**Wire / persistence** (`toma/nvmeibt_praid_basics.h`, `nvmeibt_praid_serialized_topo`):

Append two trailing fields to the serialized topo header:

```c
struct nvmeibt_praid_serialized_topo {
    /* ...existing fields... */
    char     allocator_toma_id[NVMEIBT_CDV_HOSTNAME_LEN];
    uint64_t allocator_generation;
    /* then existing segs[0] tail */
} __attribute__((packed, aligned(8)));
```

This extension lengthens the header. Because followers parse into in-memory structures via explicit field copies (not a direct memcpy of the whole header), appending fields at the end is backward-compatible for reads: an older peer sending the pre-extension layout will zero-fill the new fields on the receiver (see §Backward compatibility).

### Leader-side election

`nvmeibt_cdv_alloc_elect()` signature change:

```c
/* Writes allocator_toma_id + allocator_generation into *out_topo_ctx.
 * Returns 1 if the staged topo_ctx was mutated (caller must treat this
 * pRAID as changed), 0 if sticky (no change), -EINVAL on bad input. */
int nvmeibt_cdv_alloc_elect(const char *cdv_uuid,
                            const char **candidates,
                            int n_candidates,
                            struct nvmeibt_praid_topo_ctx *out_topo_ctx);
```

Behavior:

- Read `out_topo_ctx->allocator_toma_id` and `out_topo_ctx->allocator_generation` (previous values staged into `calculated_praid_lot`).
- Sticky: if the current `allocator_toma_id` is in `candidates`, return 0 without mutating.
- Otherwise: pick a candidate (random tie-break), set `out_topo_ctx->allocator_toma_id = chosen`, increment `out_topo_ctx->allocator_generation`, return 1.

No in-memory `cdv_alloc_hash` writes, no `set_generation`, no `push_to_registrants`, no self-apply short-circuit, no `send_notify_to_elected`. The staged `calculated_praid_lot` flows through the existing `nvmeibt_praid_leader_we_have_a_new_baseline → serialize → AppendEntries → commit` pipeline, identical to any other topology change.

Caller (`nvmeibt_topology_calc_topology`):

```c
if (is_first_praid && blkdev->from_config.is_cdv) {
    /* build candidates from alive RAFT members */
    int rv = nvmeibt_cdv_alloc_elect(cdv_uuid, candidates, n_candidates,
                                     &praid_leader->calculated_praid_lot.topo_ctx);
    if (rv > 0) {
        /* Mark the pRAID as changed so the existing is_topo_changed path
         * picks it up and ships it in the next AppendEntries. */
        NVMEIBT_PRAID_MARK_TOPO_RECALC_REQUIRED(<tag>, praid);
    }
}
```

All the "notify the chosen TOMA" / "push to registrants" code is removed from the calc path.

### Follower-side apply (new hook)

A single hook in the topology-apply path — runs on **every** TOMA on every committed topology change:

```c
/* toma/nvmeibt_cdv_alloc.c */
void nvmeibt_cdv_alloc_on_topo_applied(struct nvmeibt_praid *praid,
                                       const struct nvmeibt_praid_topo_ctx *prev_applied,
                                       const struct nvmeibt_praid_topo_ctx *new_applied);
```

Invoked from the existing `update_applied_topology` path for every pRAID, immediately after `baseline/committed → applied_praid_lot` copy. For each CDV first pRAID, compare `prev_applied` vs `new_applied`:

| Transition                                                          | Action                                                                                        |
|---------------------------------------------------------------------|-----------------------------------------------------------------------------------------------|
| Generation unchanged                                                 | No-op.                                                                                        |
| Generation increased, `allocator_toma_id == my_hostname`, state ≠ ACTIVE | Promote: `state → AWAITING_SATELLITE_ATTACH`; `satellite_attach_request_id = gen`; `cdv_send_attach_satellite_request()`. |
| Generation increased, `allocator_toma_id != my_hostname`, previous was me | Demote: drain `io_wq`; close fds; clear satellite_dev_path / satellite_uuid; `state → NOT_ALLOCATOR`. |
| Generation increased, previous and new are both other TOMAs         | Record new identity in local `cdv_alloc_hash` for status/observability; no role transition.   |
| Generation decreased                                                 | Unreachable under RAFT log ordering; log an error, treat as no-op.                            |

This hook is the **sole** driver of `AWAITING_SATELLITE_ATTACH` transitions. `handle_cdv_alloc_extent`'s lazy re-fire of Stage A when it observes `state == AWAITING_SATELLITE_ATTACH` becomes a pure idempotency safety net — it cannot bootstrap state.

### Lazy bootstrap removal

Remove the `find_or_create_alloc` call inside `nvmeibt_cdv_alloc_list_for_tpv`. A client request must not be able to create a zero-initialized alloc entry on a TOMA that has not yet applied the topology naming it the allocator. If the entry does not exist, reject the client request:

- `CDV_LIST_EXTENTS` → return `WRONG_GEN` with `allocator_generation = 0`. Client retries; once topology-apply has run, the entry exists.
- `CDV_ALLOC_EXTENT`, `CDV_FREE_EXTENT` → same.

This removes the path that currently produces a ghost alloc entry with empty `allocator_toma_id` and lets `cdv_worker_open_fd` fall through to the legacy `/dev/nvmesh/<cdv>` path.

### Client propagation (unchanged)

The CDV topology push TOMAs already send to registered clients (§2.5) continues to carry `(allocator_toma_id, allocator_generation)`. The push is re-triggered whenever the applied topology changes for the CDV's first pRAID — which happens through the normal topology-apply path, unchanged by this work.

---

## Phased delivery

### Phase 1 — schema, no behavior change

1. Add `allocator_toma_id` + `allocator_generation` to `nvmeibt_praid_topo_ctx` and `nvmeibt_praid_serialized_topo`. Initialize to zero in `nvmeibt_praid_lot_init`.
2. Add serialize/deserialize for the two new fields in `praid_leader_serialize_topo` and `nvmeibt_praid_upd_committed_topo`.
3. Bump `sw_compatibility_ver`.
4. Land. Every TOMA replicates the extra fields as zero. No functional change.

### Phase 2 — leader writes fields; apply hook is a no-op

1. Change `nvmeibt_cdv_alloc_elect` signature to take `out_topo_ctx`. Update the single caller in `nvmeibt_topology_calc_topology`.
2. The new hook `nvmeibt_cdv_alloc_on_topo_applied` is wired in the apply path but returns immediately (logs diff only). No role transitions yet.
3. Land. The leader now stages allocator identity into the committed topology; followers see it via `/proc` but do not act on it. Verify identity propagation via per-TOMA `/proc/nvmeibs/toma_status/cdv_detailed` — every TOMA should show the same `(allocator_toma_id, allocator_generation)` for every CDV.

### Phase 3 — apply hook drives state transitions

1. `nvmeibt_cdv_alloc_on_topo_applied` performs the promote/demote transitions described above.
2. Remove `find_or_create_alloc` from `nvmeibt_cdv_alloc_list_for_tpv` (and from any sibling client-request-driven path that does it). These paths now reject with `WRONG_GEN` when the alloc entry is absent.
3. Land. At this point, elections drive Stage A purely through RAFT topology propagation. Verify end-to-end: fresh cluster, create CDV + TPV, confirm `<CDV>-mgmt` is attached to the elected allocator and scans succeed.

### Phase 4 — retire the unicast path

1. Delete `RAFT_MSG_CDV_ALLOC_NOTIFY` from `nvmeibt_raft_msg_fmt.h`.
2. Delete `nvmeibt_raft_send_cdv_alloc_notify` and its RAFT-dispatch case (`case RAFT_MSG_CDV_ALLOC_NOTIFY` in the append-entries dispatcher).
3. Delete `nvmeibt_cdv_alloc_handle_notify`, `nvmeibt_cdv_alloc_send_notify_to_elected`, `nvmeibt_cdv_alloc_set_generation`, `nvmeibt_cdv_alloc_get_allocator`, and the `out_proposed_gen` out-param on `elect()`.
4. Simplify `elect()` to its minimal form (sticky check + random pick, writing into `out_topo_ctx`).
5. Remove the monotonicity guard (`if (gen <= alloc->allocator_generation)`) — not needed; RAFT ordering guarantees monotonicity.
6. Remove `nvmeibt_cdv_alloc_push_to_registrants` from the leader-side calc path. Keep it as a helper invoked from the apply hook when the applied identity changes (local clients need the updated topology push).
7. Bump `sw_compatibility_ver`. Confirm mNDU rules gate old+new TOMAs from co-existing during the transition (Phase 4 cannot interoperate with Phase 0).

### Phase 5 — client-side cleanup (optional, parallel)

1. Remove the client-side defensive handling added during the unicast era, if any (e.g. retrying `CDV_LIST_EXTENTS` on `WRONG_GEN` with a separate timer — now covered by the existing topology-push listener).
2. No protocol changes; purely code simplification.

---

## Code changes by file

### Headers

- `toma/nvmeibt_praid_basics.h`
  - Add two fields to `struct nvmeibt_praid_topo_ctx`.
  - Add two trailing fields to `struct nvmeibt_praid_serialized_topo`.

- `toma/nvmeibt_cdv_alloc.h`
  - Change `nvmeibt_cdv_alloc_elect` signature (Phase 2).
  - Add `nvmeibt_cdv_alloc_on_topo_applied` (Phase 2).
  - Remove `nvmeibt_cdv_alloc_handle_notify`, `..._send_notify_to_elected`, `..._set_generation`, `..._get_allocator` declarations (Phase 4).

- `toma/nvmeibt_raft.h` / `toma/nvmeibt_raft_msg_fmt.h`
  - Remove `nvmeibt_raft_send_cdv_alloc_notify` declaration and `RAFT_MSG_CDV_ALLOC_NOTIFY` enum value (Phase 4).

### Leader-side

- `toma/nvmeibt_topology.c`
  - Simplify the CDV election block: build candidates, call `cdv_alloc_elect(..., &calculated_praid_lot.topo_ctx)`, mark pRAID for recalc on return 1. Remove `send_notify_to_elected`, `get_allocator`, `push_to_registrants` from the calc site (Phase 2–4).

- `toma/nvmeibt_cdv_alloc.c`
  - Rewrite `nvmeibt_cdv_alloc_elect` to operate on the staged `topo_ctx` and return 0/1. (Phase 2)
  - Delete `handle_notify`, `send_notify_to_elected`, `set_generation`, `get_allocator`, plus the `cdv_notify_self`, `cdv_notify_send`, `cdv_notify_apply`, `cdv_notify_stale`, `cdv_notify_oom` trace tags. (Phase 4)
  - Add `nvmeibt_cdv_alloc_on_topo_applied(praid, prev, new)`. (Phase 2 stub, Phase 3 real logic)
  - Remove the lazy `find_or_create_alloc` inside `nvmeibt_cdv_alloc_list_for_tpv` (and any sibling). (Phase 3)

- `toma/nvmeibt_raft.c`
  - Remove the `case RAFT_MSG_CDV_ALLOC_NOTIFY` dispatcher entry. (Phase 4)
  - Remove `nvmeibt_raft_send_cdv_alloc_notify` definition. (Phase 4)

### Follower apply path

- `toma/nvmeibt_praid.c` (or wherever `update_applied_topology` lives)
  - At the end of the apply step for each pRAID, capture `prev = applied_praid_lot.topo_ctx` before the copy and invoke `nvmeibt_cdv_alloc_on_topo_applied(praid, &prev, &applied_praid_lot.topo_ctx)`. Same call site runs on leader and followers.

### Serialization

- `toma/nvmeibt_praid.c`
  - `praid_leader_serialize_topo`: write the two new fields into the wire buffer.
  - `nvmeibt_praid_upd_committed_topo` (or the deserializer side): read them, defaulting to zero if the sender's `segs_num` implies the older layout (see §Backward compatibility).

### Observability

- `toma/nvmeibt_cdv_alloc.c` (`nvmeibt_cdv_alloc_print_status` and the `/proc/nvmeibs/toma_status/cdv_detailed` entry)
  - Source `(allocator_toma_id, allocator_generation)` from `applied_praid_lot.topo_ctx` rather than from the local `cdv_alloc_hash` cache. The cache entry may lag the applied state by a few microseconds during the apply hook, and sourcing from applied state is the more reliable view for operators.

### Tests

- `toma/unitest/` — mgmt_sim and peer_toma_simu
  - Remove harness code for `RAFT_MSG_CDV_ALLOC_NOTIFY`.
  - Extend topology assertions: after an election test run, every simulated TOMA's `applied_praid_lot.topo_ctx.allocator_*` matches the expected value.

---

## Backward compatibility

### Wire-format extension

`nvmeibt_praid_serialized_topo` gains 72 bytes (64-byte hostname + 8-byte generation). The struct is `__attribute__((packed, aligned(8)))` with a trailing `segs[0]`, so appending fields **before** `segs[0]` is a breaking change for any parser that locates `segs[]` by the hard-coded `NVMEIBT_PRAID_SERIALIZED_TOPO_HDR_SIZE_V0x310`. Two alternatives:

**Option X — version the header.** Increment the serialized-topo header version constant alongside `sw_compatibility_ver`. The parser branches on header size/version: old size → zero-fill new fields; new size → read them. This is the pattern already used for `topo_idx_updated`'s addition (see the existing `NVMEIBT_PRAID_SERIALIZED_TOPO_HDR_SIZE_V0x310` guard).

**Option Y — place the new fields at the end of the variable-length record**, after the `segs[]` tail, by adding a post-segs trailer. More awkward because `segs[0]` is a flex array; would require restructuring the serialized layout.

**Pick Option X.** Define `NVMEIBT_PRAID_SERIALIZED_TOPO_HDR_SIZE_VNEXT` for the extended header and gate on peer `sw_compatibility_ver` just like the existing v0x310 compatibility shim.

### mNDU interaction

This change bumps the topology wire format. Gate the rollout via `sw_compatibility_ver` so that old-format TOMAs and new-format TOMAs cannot coexist in the same RAFT group during Phase 4. Phases 1–3 can interoperate with an older TOMA that does not emit or read the new fields — such a TOMA effectively behaves as if no allocator identity is set, which is tolerable during the rollout window because the existing unicast path is still active alongside it.

### On-disk header (CDV volume's first block)

No change. The CDV on-disk header continues to hold its own `(allocator_toma_id, allocator_generation)` as the durable source of truth across full-cluster restart. Whether the satellite is used as the metadata store (§1.5 design) or the in-CDV path (legacy) is orthogonal to this work.

---

## Correctness argument

The claim is: every TOMA observes the same allocator identity at the same committed log index, Stage A is fired exactly by the elected TOMA on every generation increase where it is the new allocator, and no custom retry is needed.

1. **Delivery.** RAFT guarantees that a committed log entry is eventually applied by every member of the quorum. A partitioned follower catches up on reconnect via standard log-replay. The allocator-identity fields are embedded in the TOPO TLV of such entries, so they are subject to the same guarantee.
2. **Ordering.** The leader is the sole writer of `allocator_generation`, and each write occurs inside a single topology-commit transaction. Followers apply log entries in order, so a lower-generation write cannot be observed after a higher-generation one on any follower.
3. **Apply-exactly-once for role transitions.** The apply hook diffs `prev_applied vs new_applied`. Because `applied_praid_lot` is updated in a single non-reentrant step per committed entry, the hook sees each generation increase exactly once per follower. Stage A is fired from this hook on the single transition that changes the allocator to the local hostname.
4. **No lazy bootstrap.** With `find_or_create_alloc` removed from the client-request paths, no TOMA ever creates an alloc entry without an accompanying applied-topology transition. The ghost-entry failure mode (`allocator_toma_id == ""`, `satellite_uuid == ""`, scan looping on `/dev/nvmesh/<cdv>`) is structurally impossible.
5. **Split-brain.** `nvmeibt_raft_has_majority()` gate in `handle_cdv_alloc_extent` (§2.7) continues to fence a minority-partitioned TOMA that still believes it is the allocator. The satellite's EXCLUSIVE_READ_WRITE reservation fence (§1.5.4.8) continues to reject residual writes.
6. **Cold start.** On full-cluster restart, `applied_praid_lot` is empty on every TOMA. The first post-restart leader's `calc_topology` reads the pre-restart `baseline_praid_lot.topo_ctx` (persisted to disk via the existing RAFT persistence path) and re-commits it if needed. The elected allocator's first apply-hook invocation transitions it to `AWAITING_SATELLITE_ATTACH` and fires Stage A — identically to any in-flight election.

---

## Testing

1. **Unit (simulator).** In `unitest/mgmt_sim` / `peer_toma_simu`, extend the existing allocator-election test to assert:
   - Every simulated TOMA's `applied_praid_lot.topo_ctx.allocator_*` matches the leader's after the AppendEntries commit.
   - The elected TOMA's `cdv_alloc_hash[cdv_uuid].state == AWAITING_SATELLITE_ATTACH` immediately after apply.
   - The elected TOMA's `cdv_send_attach_satellite_request` was invoked exactly once for the new generation.
   - A re-election bumps generation once; only the new allocator transitions to `AWAITING_SATELLITE_ATTACH`; the old allocator transitions to `NOT_ALLOCATOR`.

2. **Integration (physical or simulator-backed 4-node cluster).**
   - Cold start: create CDV + satellite + TPV. Verify `<CDV>-mgmt` is attached to exactly one TOMA, that TOMA matches `applied.allocator_toma_id` on every other TOMA, and TPV client operations succeed.
   - Kill the allocator TOMA. Verify (a) next topology commit names a different TOMA, (b) that TOMA attaches the satellite, (c) TPV I/O resumes without client-visible hangs beyond RAFT detection latency.
   - Partition the allocator TOMA into a minority. Verify it cannot serve ALLOC (quorum gate) and that the majority partition re-elects and resumes service.
   - Dropped message injection: drop `AppendEntries` containing the allocator-identity update for N heartbeats, then allow. Verify the catch-up replica eventually applies and transitions correctly. (Regression coverage for the original failure.)

3. **Upgrade.** Rolling upgrade of one TOMA from Phase-0 to Phase-4 in a quorum of otherwise Phase-0 TOMAs: gate by `sw_compatibility_ver` — should be refused by mNDU until the full cluster upgrades.

4. **Observability.** Confirm `/proc/nvmeibs/toma_status/cdv_detailed` on every TOMA reports identical `allocator_toma_id`/`allocator_generation` for the same CDV.

---

## Open questions

1. Does the existing `update_applied_topology` entry point give us a natural site to hook `nvmeibt_cdv_alloc_on_topo_applied`, or does it split across too many call sites? If the latter, consolidate first.
2. Where in the leader's serialize path is the right point to write the two new fields — alongside `registrants_sync_cmd` in the topo-ctx serializer, or as a trailer? (Option X above assumes topo-ctx serializer.)
3. Should the CDV-level allocator identity also be carried in `topo_config_praid_and_segs_wire_conf_buf` (the TOPO_CONFIG TLV) for persistence across full-cluster restart, or is the per-entry persist on disk sufficient? The existing design keeps the durable copy in the CDV on-disk header (§2.6); this work does not change that.
4. The satellite-attach handshake (§1.5.4) publishes `allocatorGenerationLastAttached` to Mongo. With RAFT-committed identity, management has an independent ordering source (the Stage A request's `allocatorGeneration` field). Are the two stores consistent under all re-election timings, or do we need a reconcile step? (Likely no new work — the generation monotonicity invariant is preserved — but worth checking in code.)
