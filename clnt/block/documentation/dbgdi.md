# Debug DI (`dbgdi`)

This document describes the **debug data integrity** subsystem in the NVMesh
block client: what it stores, where it lives, and the invariants that govern
who is allowed to write to it. It also captures the analysis behind
NVMESH-4505 and NVMESH-8666, two issues that share the same root cause
(uncoordinated mutation of a buffer that mirror legs share by design).

Code references are relative to `clnt/block/` unless otherwise noted.

---

## 1. What dbgdi is

`dbgdi` is a per-IO **diagnostic stamp** that the client embeds inside user
data pages so that, if an on-disk DI mismatch is later discovered, post-mortem
tooling can answer "who wrote this block, when, in which IO operation, on
which leg, with which metadata?" The stamp is *destructive*: dbgdi mode
overwrites bytes **512..4095** of every 4 KB block with a fixed-format
struct, while preserving the first **512 bytes** of user data
(`struct t_dont_touch_original`, named for intent — those bytes are
deliberately left untouched). Because 3.5 KB out of every 4 KB is replaced
with diagnostic metadata, dbgdi is a debug build option
(`enable_di_debug_mode`), not a production feature.

Two on-block regions:

- **Writer/reader log** — a small ring buffer of fixed-size records appended
  per significant event (write submitted, sync overwrite, read poison, etc.).
- **Core stamp** — a structured snapshot of the most recent submission and
  completion for the current op (per-leg slot for writes, single slot for
  reads).

Both regions live in the same 4 KB user data page. That co-location is the
source of the invariant violations described below.

Code: `clnt/block/datapath_utils_debug_di/`.

---

## 2. The data block layout

`struct t_data_blk` (4096 bytes) is the layout dbgdi imposes on a data block
when debug mode is enabled:

```text
offset    size    field
   0      512    dont_touch_original   (preserved user data, intentionally untouched)
 512       20    log.header            (magic, head, tail, size, full/over flags, last_rec_type)
 532     1548    log.buf[1548]         (records, ring-buffer)
2080       24    core.rd               (read-side stamp: op, ch_type, comp_code, container ptr, magic)
2104     1992    core.wr[3]            (one per write leg: pre/post snapshots)
```

The log header at offset `+0x4` holds `head` (producer cursor) and at `+0xc`
holds `size` (capacity) — those exact offsets matter for the NVMESH-8666
lockup analysis below.

Both `log` and `core` live in the same page. A CPU mutating either region
while DMA is reading the page from another leg is undefined behavior at the
device interface and a CPU/CPU race for any reader that walks the log.

---

## 3. Producer paths

Mutations to the dbgdi regions originate from these call sites:

| Caller | What it writes | When |
| ------ | -------------- | ---- |
| `dp_dbgdi_do_add_info` → `__dbgdi_do_add_info` → `data_blk_fill_for_write` | `t_db_who_cmd_core_cell` into `core.wr[]`, then writer record into `log` | Per leg, just before `dp_cmds_execute_cmd` posts the disk command |
| `data_blk_fill_for_sync` (inside the same wrapper, recovery ops only) | `t_db_who_cmd_core_cell` into `core.wr[]`, sync record + recovery-specific record into `log` | Per leg, on recovery writes (`RECOVER_STALE`, `RECOVER_READFAIL`, `RECOVER_DB`, `RECOVER_ROLLBACK`, etc.) |
| `__inject_debug_di_with_sync_info` (`datapath_mirror/nvmeibc_block_dp_mirror_sync.c`) | `dp_dbgdi_copy_sync_overwritten` / `dp_dbgdi_clear_sync_overwritten` — sync-overwritten record into `log` | After `__set_write_buffer_to_valid_source` aliases the read buffer to the write commands |
| `dp_dbgdi_do_add_info_core_pre` (per-leg, called from disk.c, ib_net_io.c, ib_net_nordda.c) | `core.wr[i].pre` snapshot | Submission of leg *i* |
| `dp_dbgdi_do_add_info_core_post` (per-leg, called from disk.c, ib_net.c) | `core.wr[i].post` snapshot, *and reads `log` to look up the writer record* | Completion of leg *i* |
| `dp_dbgdi_do_rdr_info` | reader record into `log` | Per leg on read completion (per-leg buffers, no sharing — safe) |
| `dp_dbgdi_do_add_restore_info` | clear/restore-history mark into `log` | Reed-Solomon block restoration |

The completion-side `core_post` is unusual because it is both a *writer*
(stamps the core area) and a *reader* (walks `log` to find the matching
writer record so the dlba/jlba can be folded into the post stamp).

---

## 4. The walker

`dbgdi_log_get_record_by_type(log, type, buf, buf_size)` is the workhorse
reader. It walks the ring from `tail` to `head`, advancing by each record's
own size, and returns the first record matching `type`.

The hot loop is essentially:

```text
cur = tail
while cur != head:
    entry = read at &log->buf[cur]
    if entry.type == type: return entry
    cur = (cur + entry.size) % log->size
```

This is the loop that hung CPU 1 in the NVMESH-8666 incident. The exit
condition is `cur == head`, which is *only* reached if the per-iteration
stride (the record's `size` field) is well-formed and `head` lies on the
stride orbit. Either of those properties failing means the loop never
terminates.

---

## 5. The shared-buffer invariant on mirror writes

Mirror writes intentionally point all leg commands at the **same** backing
data page. This is required for correctness: if different legs observed
different bytes, mirror would corrupt itself. The price is that the page
becomes a shared mutable resource accessed by:

- one CPU on each leg's submission path (mutating the page just before posting)
- the NIC/disk on each leg's DMA path (reading the page after posting)
- one CPU on each leg's completion path (mutating the page in core_post)

The invariant the dbgdi code must respect is therefore:

> A given shared mirror buffer is mutated by at most **one** CPU, and that
> mutation completes **before any leg is posted**.

Without that invariant, a CPU mutates the page while another leg's DMA reads
the same page. The result on disk is torn data; the result in RAM is a
half-updated `log` that any subsequent walker will see as structurally
inconsistent.

EC writes do not have this constraint because each EC leg has its own buffer.
JBOD has only one leg, so the invariant is trivially satisfied.

### 5.1 Logical first vs. physical first

A natural-looking fix is to gate dbgdi mutation to the *first* mirror leg.
But "first" has two distinct meanings, and they don't always agree:

- **Logical first** — the command with the smallest index in the cmds array
  (`&rldr[0]` after stage matching).
- **Physical first** — the command that is actually posted on the wire
  first.

The synchronous send loop in `__send_cmds_of_stage` iterates strictly in
array order and skips `do_not_send` legs:

```c
for (ci = first_cmd; ci <= last_cmd; ci++) {
    if (unlikely(cmds[ci].do_not_send)) continue;
    rv = dp_cmds_tryexec_cmd(cmds, ci, error_on_no_execution);
}
```

In the common case logical first == physical first, and gating to "logical
first" is sound. **Four scenarios break the equality:**

1. **Logical first is `do_not_send`.** The send loop skips it; cmds[1] is
   posted first. cmds[0] never enters `dp_cmds_execute_cmd`, so the gate
   never fires for any leg, and the writer record is silently dropped.
2. **Logical first fails synchronously and is retried later.** cmds[0]
   returns `-EAGAIN`, the loop continues to cmds[1] which posts. A retry
   path later resubmits cmds[0]; the dbgdi mutation now runs while cmds[1]
   is on the wire and possibly mid-DMA — **CPU vs. DMA race on the same
   shared page.**
3. **Multi-stage transitions on layered RAID** (RAID-50/60). The same
   raid leader serves both `WRITE_JOURNAL` and `DO_IO_AND_PAR`. Each
   stage's logical first can mutate the shared buffer while a previous
   stage's legs are still in flight.
4. **Sub-block / unaligned writes (PET-style hazard).** Even with one CPU
   mutator, the dbgdi stamp is large (≥1.5 KB header + records) and not
   atomic at the device interface; the device can observe a partial update
   regardless of CPU memory barriers, which only fence CPU-vs-CPU
   ordering, not CPU-vs-device.

CPU memory barriers do not fix any of these — there is no portable way to
fence DMA from CPU code without waiting for completion (which would
serialize legs and defeat mirror's parallelism).

### 5.2 Consequence: dbgdi cannot safely mutate a shared mirror buffer

Once the four scenarios above are accounted for, "first leg only" is no
longer a sound invariant for mirror writes. The only sound invariant is the
stronger one:

> dbgdi must **never** mutate a shared mirror buffer.

Mirror writes therefore lose their dbgdi diagnostics entirely. EC and JBOD
keep theirs (per-leg buffers; no sharing). Reads keep theirs on all RAID
types (per-leg buffers).

The helper that encodes this is
`dp_dbgdi_can_mutate_shared_buf(cmd)` in
`datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.c`: returns `true` for EC
and JBOD, `false` for any R1 multi-leg mirror.

---

## 6. NVMESH-4505: the partial solution

NVMESH-4505 introduced a gate inside `__data_blk_fill_for_write`:

```c
if (!nvmeibc_raid_is_ec(pr) &&
    cmd != &rldr[dp_cmds_get_first_cmd_of_stage(rldr, E_CMDS_STAGE_DO_IO_AND_PAR)])
    return;
```

The comment said: *"we inject writer record for only the first write
command."* Literally accurate, but narrow. It only protected:

- the `dbgdi_log_add_rec(... DBG_DI_WRITE ...)` append at the bottom of
  `__data_blk_fill_for_write`, for normal writes only.

It did **not** protect:

1. `t_db_who_cmd_core_cell(&d->core, cmd)` in the wrapper
   `data_blk_fill_for_write` — runs *before* the gate, on every leg.
2. `data_blk_fill_for_sync` — for recovery writes (`RECOVER_STALE`,
   `RECOVER_READFAIL`, `RECOVER_DB`, `RECOVER_ROLLBACK`, etc.), every leg
   appended sync and recovery records to `d->log` with no gate at all.
3. `__inject_debug_di_with_sync_info` for mirror sync writes that share an
   NDB via `__set_write_buffer_to_valid_source`.
4. `dp_dbgdi_do_add_info_core_pre` / `core_post` — called per leg from
   `nvmeibc_disk.c`, `nvmeibc_ib_net.c`, `nvmeibc_ib_net_io.c`,
   `nvmeibc_ib_net_nordda.c`.

NVMESH-4505 was sufficient to clear the most obvious symptom (the FIFO MD
corruption it was reported against) because that symptom involved the
writer-record append specifically. The other paths kept hammering the shared
page, just less visibly.

---

## 7. NVMESH-8666: hard lockup in the walker

### 7.1 The crash signature

Test `CITest.test_ci(d_type=stop_target)` test #3 with `debug_di=True` on a
3-way mirror produced a hard lockup (NMI watchdog) on the IO completion
thread:

```text
RIP: dbgdi_log_get_record_by_type+0x5a
... __dp_dbgdi_get_dlba_and_jlba
... dp_dbgdi_do_add_info_core_post
... dp_dbgdi_add_info_core_post_with_magic
... nvmeibc_ib_net_complete_iocmd_reuse
... process_io_rsp / process_rsp
... nordda_recv_completion
... poll_cq_and_process_common
... qthread_func
```

The faulting bytes `41 f7 76 0c` decode to `divl 0xc(%r14)` — the modulus
operation in the walker's `(cur + len) % size` step. Not a divide-by-zero
(would be #DE, not a hard lockup); the CPU was simply spinning. R14 pointed
at a `struct dbgdi_log` whose `size` was non-zero but whose
`head` / record-length combination produced an unreachable exit condition.

### 7.2 Why the loop never exits

Four ways the `cur == head` exit fails forever:

1. **Length-0 record at `cur`**: `(cur + 0) % size = cur`, cursor never
   advances.
2. **`head` outside `[0, size)`**: cursor cycles in `[0, size)` but never lands
   on `head`.
3. **Stride-orbit miss**: cursor's stride visits only `gcd(stride, size)`
   residues; if `head` was bumped off-boundary by a torn write, it falls
   outside that orbit.
4. **Concurrent producer**: every iteration re-reads `head`/`size` from the
   shared page; a producer mutating those between iterations can keep the
   cursor chasing a moving target.

The walker had no hop bound and trusted `head`, `size`, and per-record
lengths from a buffer that other CPUs were allowed to mutate.

### 7.3 Why 3-way is special

NVMESH-4505 left several per-leg mutators ungated (section 6). Each
additional leg multiplies:

- the number of concurrent CPU mutators on the shared page
  (2 → 3 in 2-way → 3-way means C(3,2) = 3 mutator pairs vs. C(2,2) = 1),
- the rate at which the per-IO log fills (~1.5× more records per IO),
- the probability that the buffer wraps within a single IO (`over = 1`
  becomes the common case rather than the edge case), exposing the
  wrap-edge bookkeeping which is where the off-stride `head` failure mode
  lives.

In 2-way the same race exists but typically degrades to *silent wrong-answer*
outcomes (the walker terminates with a corrupt-but-plausible record) rather
than lockup. The bug was latent in 2-way and observable in 3-way.

The walker's exit failure modes form a spectrum, not a binary:

| Reading from corrupt log | Walker outcome | Visible signature |
| --- | --- | --- |
| `len == 0` (torn write) | infinite loop | hard lockup |
| `head` unreachable | infinite loop | hard lockup |
| `len` lands on `head` by chance | early exit | wrong record returned silently |
| record type at `cur` accidentally matches search key | early exit "found" | wrong record passes the type check |
| `size` corrupted | reads past `buf[]` | quiet leak from `core` area |

Lockup is the loud face of the bug; silent wrong-answer was almost certainly
also occurring in 2-way without ever paging anyone.

---

## 8. The new guard

The fix consists of two orthogonal pieces — one that closes the producer
race, one that hardens the consumer against any corrupt log it encounters.

### 8.1 Producer side: dbgdi mutation disabled for mirror writes

A single helper, `dp_dbgdi_can_mutate_shared_buf(cmd)`, names the only
sound invariant: "this command's data buffer is private to this leg." It
returns `true` for EC (per-leg buffers) and JBOD (single leg), `false` for
any multi-leg R1 mirror. All three dbgdi mutation entry points consult it:

- **Writer record + per-leg `core.wr[]` cell**, in `data_blk_fill_for_write`.
  The whole function (including the `t_db_who_cmd_core_cell` call that
  NVMESH-4505 missed) is now gated by the helper. Mirror writes return
  immediately with no mutation.
- **Sync record append**, in `__inject_debug_di_with_sync_info`. Mirror
  sync writes share an NDB via `__set_write_buffer_to_valid_source`; the
  helper short-circuits the entire stamp loop. EC sync continues to
  inject as before (per-leg buffers).
- **Core pre/post**, in `dp_dbgdi_should_add_info_core`. Mirror writes
  return `false` regardless of leg. Reads keep core stamps on all RAID
  types (per-leg buffers).

This is stronger than "first leg only" and is required because of the four
scenarios in §5.1 — once any of them can occur, no per-leg gate is sound
for a shared buffer.

The cost is that mirror writes lose their dbgdi diagnostics. The
alternatives (CPU memory barriers; serializing legs; per-leg buffers
for mirror) either don't work or violate mirror's correctness invariant.

### 8.2 Consumer side: defensive walker

Independent of the producer fix, the walker
(`dbgdi_log_get_record_by_type` and friends) now treats the log as
untrusted input. It requires:

- `__dbgdi_log_header_sane(log)` — magic matches, `size >= sizeof(entry)`
  and `<= sizeof(buf)`, `head < size`, `tail < size`.
- Per-iteration validation of `entry.size` (rejects torn or zero-length
  records).
- A hop bound of `size / sizeof(dbgdi_log_entry)`, making the loop
  terminating-by-construction regardless of input.

This protects against already-corrupted blocks left by older builds,
partial DMA, and any future regression that re-introduces a producer-side
mutation. Without the producer fix the consumer would still be DOS-able;
without the consumer fix the producer fix wouldn't help on existing
on-disk blocks. Both must land.

---

## 9. Things that look like dbgdi bugs but aren't

- **EC writes still take per-leg core stamps**: by design, EC has per-leg
  buffers, so the shared-buffer invariant doesn't apply. The helper and
  `dp_dbgdi_should_add_info_core` return `true` for EC.
- **JBOD always allows mutation**: `replicas == 1`, there is no other leg
  to race with.
- **Mirror writes have no dbgdi**: this is intentional, not a missing
  feature. See §5 and §8.1. Diagnostics on mirror writes are deliberately
  sacrificed to preserve the shared-buffer invariant.
- **`f43c3bbe0d` mapping legs over `CORE_DBGDI_WR_MAX_MIRROR`**: that fix
  was about the per-leg `core.wr[]` index collision (`% 2` mapping legs
  {0, 2} to the same slot in 3-way). It is structurally separate from the
  shared-buffer race and is now superseded — mirror writes don't touch
  `core.wr[]` at all.

---

## 10. Implementation pointers

| Concern | Location |
| ------- | -------- |
| Helper "can this command's dbgdi safely mutate the data page" | `datapath_utils_debug_di/nvmeibc_block_dp_dbgdi.c::dp_dbgdi_can_mutate_shared_buf` |
| Writer record + core_cell gate | `…dp_dbgdi.c::data_blk_fill_for_write` |
| Mirror-sync gate | `datapath_mirror/nvmeibc_block_dp_mirror_sync.c::__inject_debug_di_with_sync_info` |
| Core pre/post predicate | `…dp_dbgdi.c::dp_dbgdi_should_add_info_core` |
| Defensive walker | `datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.c::dbgdi_log_get_record_by_type`, `__dbgdi_log_header_sane` |
| Layout | `datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_blk.h` (`struct t_data_blk`) |
| Log layout | `datapath_utils_debug_di/nvmeibc_block_dp_dbgdi_log.h` (`struct dbgdi_log`, `struct dbgdi_log_entry`) |

Tests:
`clnt/block/unitest/nvmeibc_block_dp_dbgdi_tests.c` covers helper behavior
(mirror returns false, EC/JBOD return true, reads not gated by RAID type),
walker corruption modes (length-0, out-of-range head, oversized size,
missing magic, hop bound), and that mirror sync writes do not mutate the
shared NDB.
