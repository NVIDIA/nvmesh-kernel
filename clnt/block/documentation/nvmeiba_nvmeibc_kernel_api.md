<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

# nvmeiba & nvmeibc — Linux Kernel Block Device API Interaction

## Overview

NVMesh exposes block devices to the Linux kernel through two cooperating kernel
modules:

- **nvmeiba** (`clnt/atom/`) — the "atom" module. Owns the `struct gendisk`
  and the `block_device_operations` fops table. Stays loaded across NDU.
- **nvmeibc** (`clnt/block/`) — the main client module. Owns I/O execution,
  topology, locking, and recovery. Created and destroyed per upgrade cycle.

The two modules are linked by C-style inheritance: `struct nvmeibc_os_api`
(defined in `clnt/block/nvmeibc_block_api_os.h`) embeds
`struct nvmeiba_atom_os_api` as its **first member**. The atom pointer and the
`nvmeibc_os_api` pointer are therefore the same address; `container_of`
converts between them.

```
struct nvmeibc_os_api {
    struct nvmeiba_atom_os_api atom;   // ← must be first; same address as nvmeibc_os_api
    const struct nvmeibc_os_apis_container *driver_context;
    struct nvmeibc_os_api_self_ref {
        struct block_device *bdev_during_detach; // kernel API variant-dependent
    } unsafe_self_ref;
    // ... nvmeibc-only fields (stats, procfs, uuid, flags, …)
};
```

---

## Global Registry: `nvmeiba_all_os_apis`

nvmeiba holds a single static global object `all` of type
`struct nvmeiba_all_os_apis` (`clnt/atom/nvmeiba_main.c`):

```c
struct nvmeiba_all_os_apis {
    spinlock_t  lock;
    struct list_head list;          // linked list of all atoms (attached, detaching, orphan)
    struct {
        int osapi;                  // total atoms
        int orphan_osapi;           // currently orphaned (upgrading)
        int nvmeibc;                // number of nvmeibc instances connected
    } n;
    struct { … } proc;              // /proc/nvmeiba/ entries
    struct block_device_operations default_fops;    // normal I/O
    struct block_device_operations upgrade_fops;    // buffers BIOs during NDU
    struct block_device_operations detaching_fops;  // rejects BIOs during detach
};
```

Every atom (one per attached volume) lives on this list from
`nvmeiba_os_api_constructor()` to `nvmeiba_os_api_destructor()`.

The module holds a self-reference (`__module_get`) while the list is non-empty,
preventing `rmmod` as long as any volume is attached.

---

## Atom Status State Machine

The `nvmeiba_status` enum (`clnt/atom/nvmeiba_nvmesh_api.h`) encodes the
current relationship between the atom and the kernel as a bitfield:

`nvmeiba_status_hidden` is a legacy internal enum name. The old hidden-volume
product feature was removed; this state now simply means the atom exists
without a registered kernel block device yet.

| Status | Value (bits) | `gendisk` | Meaning |
|---|---|---|---|
| `nvmeiba_status_illegal` | `0x0` | — | Zeroed, uninitialized |
| `nvmeiba_status_hidden` | `0x1` (init) | `NULL` | Legacy "no-gendisk" atom state, used for recovery-only attach and similar pre-registration paths; no kernel block device, no I/O |
| `nvmeiba_status_orphan` | `0x3` (init\|kernel) | non-NULL | NDU in progress — nvmeibc is gone, gendisk stays, BIOs buffered |
| `nvmeiba_status_live` | `0x7` (init\|kernel\|nvmeibc) | non-NULL | Normal operation — nvmeibc receives all BIOs |
| `nvmeiba_status_detaching` | `0xB` (init\|kernel\|detach) | `NULL` becoming | Volume detaching — BIOs auto-failed, gendisk being removed |

State transitions:

```
                  attach (normal)
[illegal] ──────────────────────────────► [live]
    │                                       │
    │ recovery-only attach                  │ NDU (upgrade)
    ▼                                       ▼
[hidden/no-gendisk] ───────────────────► [orphan]
                                            │
                                            │ new nvmeibc adopts
                                            ▼
                                         [live]
                                            │
                                            │ detach
                                            ▼
                                       [detaching]
                                            │
                                            │ last close()
                                            ▼
                                         (freed)
```

---

## Block Device Creation (`attach`)

### Who creates the `struct gendisk`

nvmeibc creates the `nvmeibc_os_api` allocation (`block_api_os_create`,
`clnt/block/nvmeibc_block_api_os.c:2022`), which contains the embedded atom.
nvmeibc then calls into nvmeiba to register the atom in the global list
(`nvmeiba_os_api_constructor → nvmeiba_os_apis_add`).

The kernel gendisk is allocated and initialized by nvmeibc in
`block_api_os_init` (line 1381). For a **fresh attach**:

1. `__alloc_disk_and_maybe_queue()` — calls `blk_alloc_disk()` or
   `alloc_disk()` depending on kernel version.
2. `disk_id_allocator_alloc()` — assigns a major/minor number from nvmeibc's
   internal bitmap.
3. Request queue parameters are configured:
   - `blk_queue_logical_block_size()` / `blk_queue_physical_block_size()`
   - `blk_queue_max_hw_sectors()`, `blk_queue_io_opt()`
   - `QUEUE_FLAG_NOMERGES`, `QUEUE_FLAG_NONROT`, etc.
4. `disk->fops` is set to nvmeibc's own `bdev_fops_io` table (nvmeibc-specific
   fops, **not** the global nvmeiba fops described below).
5. `disk->private_data = atom` — the gendisk carries a back-pointer to the atom.
6. `set_capacity(disk, …)` — sets the volume size in 512-byte sectors.
7. `atom->status = nvmeiba_status_live` (line 1453).

### Registering with the kernel: `add_disk`

`block_api_os_start_accepting_kernel_io` (line 1499) is called after
`block_api_os_init`. For a fresh attach it calls
`__add_disk_io_starts_b4_func_ends` (line 1465):

```c
atomic_set(&os->atom.gendisk_status, 2);   // "adding"
rv = add_disk(disk);                        // ← registers gendisk with the kernel
                                            //   I/O can arrive immediately after this
atomic_set(&os->atom.gendisk_status, 3);   // "added, I/O possible"
```

`gendisk_status` tracks readiness:
- `1` = allocated but not yet added
- `2` = `add_disk()` in progress
- `3` = fully registered; `is_gendisk_ready_for_io(atom)` returns true

After `add_disk` the volume appears in `/dev/<instance>/<vol_name>` and the
kernel may issue `open()` calls and BIOs immediately.

---

## The `block_device_operations` (fops) Table

nvmeiba owns **three** global fops variants, all sharing the same `open` and
`release` handlers but differing in `submit_bio`:

| Variant | `submit_bio` | Used when |
|---|---|---|
| `all.default_fops` | `NULL` (or `nvmeibc_b_req_reject` on older kernels) | nvmeibc not yet connected |
| `all.upgrade_fops` | `nvmeiba_b_req_push` | NDU in progress — BIOs buffered in `atom->pender` |
| `all.detaching_fops` | `nvmeiba_b_req_reject` | Volume detaching — BIOs immediately failed |

### `open` and `release` — always nvmeiba's

All three fops variants share the same `open` (`__fops_interface_open`) and
`release` (`__fops_interface_close`) handlers, which live in nvmeiba and never
change. This is critical: even when nvmeibc is gone during NDU, the kernel can
still call `open()` and `release()` on the gendisk and nvmeiba handles them
safely.

**`open` (`nvmeiba_bdev_open`):**
1. Allocates a `nvmeiba_pid_owner` struct recording `pgid`, task name,
   and open mode.
2. `atomic_inc_return(&atom->users.n_opens)` — increments the open counter.
3. Appends the record to `atom->users.pids` under `users.lock`.

**`release` (`nvmeiba_bdev_close`):**
1. Finds the `nvmeiba_pid_owner` by `pgid` in `users.pids` and removes it.
2. `atomic_dec_return(&atom->users.n_opens)` — decrements the counter.
3. **Deferred destructor check** (line 88):
   ```c
   if ((counter == 0) && (atom->disk == NULL)) {
       nvmeiba_os_api_destructor(atom);
   }
   ```
   This is the deferred-free path: if the disk was already removed
   (`atom->disk = NULL`) but there were still open handles, the memory is freed
   here on the last `release()`.

### `submit_bio` — three implementations

**`nvmeiba_b_req_push`** (upgrade path):
- Acquires `atom->pender.lock`.
- Checks `nvmeiba_os_api_is_queue_orphan()` (tests whether `fops->submit_bio`
  still points to itself) — guards against the race where the new nvmeibc
  atomically replaces `submit_bio` and starts draining the list simultaneously.
- If still orphan: appends the BIO to `atom->pender.bio_list`.
- If adoption just completed: forwards the BIO directly to the new handler.

**`nvmeiba_b_req_reject`** (detaching path):
```c
bio_io_error(bio);   // immediately fails the BIO with -EIO
```

---

## Open Count and the `unsafe_self_ref` Mechanism

When a volume is in use by a user process (filesystem mount, `dd`, etc.) and
a **force detach** is requested, nvmeibc needs to guarantee that the
`nvmeibc_os_api` structure is not freed by a concurrent `release()` callback
while nvmeibc is still running its detach teardown.

The solution is `unsafe_self_ref.bdev_during_detach` in `nvmeibc_os_api`. Before
beginning the destroy sequence, nvmeibc takes a kernel-level reference to its
own block device (`block_api_os_get`, line 204). This reference increments
`atom->users.n_opens` using the special `SELF_REF_PID`. It is released
(`block_api_os_put`, line 275) only when the detach teardown is complete.

This means `nvmeiba_os_api_destructor` (and thus `kfree`) is deferred until
**both** conditions hold:
- `atom->users.n_opens == 0` (all user and self references released)
- `atom->disk == NULL` (gendisk removed)

---

## Normal Detach

Normal detach is invoked when management detaches a volume gracefully with no
new I/O in flight.

### Step 1: Stop accepting new I/O

`block_api_os_stop_accepting_kernel_io(os, reason='D')` (line 1048):

```c
__set_make_req_to_reject(atom);
wmb();
```

This atomically replaces `fops->submit_bio` with `nvmeiba_b_req_reject`.
Any BIO arriving after the memory barrier is immediately failed with `-EIO`.

### Step 2: Drain in-flight I/O

`block_api_os_drain_io(os)` (line 1179):

- Asserts that atom is still `nvmeiba_status_live` (reject has been installed,
  but status has not yet changed).
- Calls `nvmeiba_atom_drain_io()` which waits on the request queue's
  `io_refcount` scalable reference counter — a count of BIOs currently executing
  inside nvmeibc. This blocks until all in-flight BIOs complete.
- Since `is_abandoning = false` (queue is not orphan), calls:
  ```c
  __set_atom_status_detaching(atom);
  // atom->status = nvmeiba_status_detaching
  ```

### Step 3: Destroy OS resources

`block_api_os_destroy(os)` (line 1631):

- Status must be `nvmeiba_status_detaching` here (asserted).
- Calls `nvmeiba_atom_io_resources_destructor(atom)` → `__kill_gen_disk(atom)`:
  1. Takes `atom->disk_lock`, sets `atom->disk = NULL`, releases lock.
  2. If `is_gendisk_ready_for_io(atom)` (gendisk_status == 3):
     - `del_gendisk(disk)` — marks the block device as shutting down in the
       kernel; prevents new references from being taken.
     - `disk_id_allocator_free()` — releases the minor number.
     - `blk_cleanup_disk(disk)` (or `put_disk(disk)` on older kernels) —
       drops the last reference to the gendisk; the kernel frees it once all
       existing references are released.
  3. If `atom->queue` still exists and the kernel needs explicit cleanup:
     `blk_cleanup_queue(queue)`.
- Destroys `/proc` entries, frees I/O stats.
- Destroys the request queue data context (`reqq_data_destroy`).

### Step 4: Free the atom (or defer)

```c
if (!can_other_threads_open_atom) {
    nvmeiba_os_api_destructor(atom);  // kfree inline — no user holds open handle
} else {
    // atom->disk is NULL; destructor fires inside nvmeiba_bdev_close()
    // on the last release() call from the kernel
}
```

`can_other_threads_open_atom` is true when `atom->disk != NULL &&
is_gendisk_ready_for_io(atom)` was true at the start of `block_api_os_destroy`.
In that case, because `atom->disk` was just set to `NULL` inside
`__kill_gen_disk`, the deferred destructor in `nvmeiba_bdev_close` will fire
on the last `release()` call.

---

## Force Detach (Device Still Open by User)

Force detach follows the same sequence as normal detach but the block device
may be held open by a filesystem mount or an application.

Key differences:

1. nvmeibc takes a self-reference **before** starting the teardown so that the
   atom memory is not freed while teardown is executing:
   ```c
   block_api_os_get(os);   // increments n_opens with SELF_REF_PID
   ```

2. After `block_api_os_stop_accepting_kernel_io(reason='D')`, all new BIOs
   are failed immediately by `nvmeiba_b_req_reject`. The user-space process
   will start receiving I/O errors.

3. After `block_api_os_drain_io`, all nvmeibc-owned in-flight BIOs have
   completed. The atom transitions to `nvmeiba_status_detaching`.

4. `block_api_os_destroy` removes the gendisk (`del_gendisk` + `blk_cleanup_disk`).
   The block device disappears from `/dev/`. The user process still holds an
   open file descriptor, but it is now backed by a dead device. Any subsequent
   I/O on it fails with `-EIO` or `-ENXIO` (kernel behaviour after `del_gendisk`).

5. nvmeibc releases its self-reference:
   ```c
   block_api_os_put(os);   // decrements n_opens; if counter reaches 0 and disk==NULL → kfree
   ```

6. The atom memory is freed when the user process finally calls `close()` and
   `n_opens` drops to zero. At that point `atom->disk == NULL`, so
   `nvmeiba_bdev_close` calls `nvmeiba_os_api_destructor(atom)` → `kfree`.

---

## NDU: Orphan Abandon (old nvmeibc going down)

NDU is triggered by `service nvmeshclient restart` with an upgrade marker file
present. The sequence from the block device perspective:

### Step 1: nvmeibc signals nvmeiba it is going down

`nvmeiba_os_do_on_nvmeibc_down()` (line 394, `nvmeiba_main.c`):

- Decrements `all.n.nvmeibc`.
- If any atoms are in `nvmeiba_status_detaching` (a force detach was in
  progress while the upgrade was triggered), replaces their `submit_bio` with
  the nvmeiba-owned `nvmeiba_b_req_reject` so that rejections continue working
  after nvmeibc's code is unloaded:
  ```c
  nvmeiba_os_api_set_detaching_abandoned(atom);
  // → disk->fops = &all.detaching_fops (contains nvmeiba_b_req_reject)
  ```

### Step 2: Stop accepting new I/O — switch to buffering

`block_api_os_stop_accepting_kernel_io(os, reason='U')` (line 1048):

```c
nvmeiba_os_api_orphan_abandon(atom);
```

`nvmeiba_os_api_orphan_abandon` (line 323, `nvmeiba_atom_os_api.c`):

```c
nvmeiba_os_apis_set_upgrade_pops(&atom->disk->fops);
// → disk->fops = &all.upgrade_fops (submit_bio = nvmeiba_b_req_push)
nvmeiba_os_apis_abandon_by(atom);
// → A->n.orphan_osapi++
```

After the memory barrier, every new BIO arriving from the kernel is appended
to `atom->pender.bio_list`. **The gendisk is not removed. The volume still
exists in `/dev/`.** Users with open handles remain unaffected.

### Step 3: Drain in-flight nvmeibc I/O

`block_api_os_drain_io(os)`:

- `nvmeiba_atom_drain_io()` waits for the `io_refcount` to reach zero — all
  BIOs currently being executed by nvmeibc complete.
- `is_abandoning = nvmeiba_os_api_is_queue_orphan(atom)` → **true**.
- Status transitions:
  ```c
  __set_atom_status_orphan(atom);
  // atom->status = nvmeiba_status_orphan
  ```

### Step 4: `block_api_os_destroy` — **do not free I/O resources**

```c
} else if ((atom->status == nvmeiba_status_orphan) && (!os->is_init_error)) {
    atom->queue->queuedata = NULL;   // disconnect nvmeibc context from queue
    // NOTE: nvmeiba_atom_io_resources_destructor is NOT called
```

The gendisk, the request queue, and the atom struct itself are **preserved**.
Only nvmeibc-owned data (stats, procfs, queue context) is freed.

Because `atom->disk` is still non-NULL and `n_opens` may be non-zero, the atom
is not freed here. The atom stays on the `all.list` with status `nvmeiba_status_orphan`.

nvmeibc is now fully unloaded. The kernel continues forwarding every new BIO to
`nvmeiba_b_req_push`, which appends them to `atom->pender.bio_list`.

---

## NDU: Orphan Adopt (new nvmeibc coming up)

### Step 1: new nvmeibc handshake

`nvmeiba_os_do_on_nvmeibc_up()` (line 375, `nvmeiba_main.c`):

- Increments `all.n.nvmeibc`.
- Returns a `nvmeiba_to_c_handover` struct:
  - `n_orphan_osapi` — how many orphans exist that must be adopted.
  - `fops` — pointer to `all.default_fops` (nvmeibc will overwrite `submit_bio`
    on each adopted atom).
  - `protocol_version` — nvmeiba ABI version (CRC-signed, immutable).

### Step 2: Find and claim the orphan

`block_api_os_create()` (line 2022) checks whether an orphan exists for this
volume by calling `nvmeiba_os_api_orphan_adopt(dev_dir, dev_name)` (line 1968):

```c
struct nvmeiba_atom_os_api *orphan = nvmeiba_os_apis_adopt_by(dev_dir, dev_name);
```

`nvmeiba_os_apis_adopt_by` scans `all.list` for an atom with:
- `status == nvmeiba_status_orphan`
- matching `dev_name` and `dev_dir` (client instance directory prefix)

It decrements `all.n.orphan_osapi` and returns the atom. No allocation of a
new `nvmeibc_os_api` is needed — the existing one is reused.

### Step 3: Reconnect the request queue

`block_api_os_init()` (line 1381) detects the atom is an orphan:

```c
if (atom->status == nvmeiba_status_orphan) {
    reqq_data_connect_to_q(q_data, atom->queue, &atom->disk->fops);
}
```

- The existing `atom->queue` is reused (not reallocated).
- Queue context (`queue->queuedata`) is reconnected to the new nvmeibc instance.
- Queue parameters are reconfigured if the topology changed.
- `atom->gendisk_status` is set to `3` (fully ready) — marking that
  `disk_id_allocator_mark()` already registered the minor from the previous
  attach; we do not call `add_disk` again.
- `atom->status = nvmeiba_status_live` (line 1453).

### Step 4: Redirect new BIOs and flush the pending list

`block_api_os_start_accepting_kernel_io()` (line 1499):

```c
if (atom->status == nvmeiba_status_orphan) {
    __atom_adoption_start_accepting_io(atom);
}
```

`__atom_adoption_start_accepting_io` (line 1120):

1. **`__adopt_atom_redirect_new_bio(atom)`** — atomically under
   `atom->pender.lock`, replaces `disk->fops` back to nvmeibc's live fops
   table. After the memory barrier, new BIOs go directly to nvmeibc.

2. **`__adopt_atom_submit_pending_list(atom)`** — drains the pending list:
   ```c
   while ((bio = bio_list_pop(&p->bio_list)) != NULL) {
       CALL_SUBMIT_BIO_FN(fops_atom->queue, fops_atom->disk, bio);
       p->n_bios--;
   }
   ```
   All BIOs that arrived during the NDU window are now submitted to the new
   nvmeibc instance for normal execution.

3. `atom->status = nvmeiba_status_live`.

From the user perspective, the volume was never removed from `/dev/`. BIOs
were queued, not failed. The application experiences a latency spike bounded
by the NDU window (target: ≤1.5 seconds) but no I/O errors.

---

## When is `del_gendisk` / `blk_cleanup_disk` Called?

These calls happen **only** during a real detach — not during NDU.

| Scenario | `del_gendisk` called? | `blk_cleanup_disk` called? |
|---|---|---|
| Normal detach | Yes, inside `__kill_gen_disk` | Yes (or `put_disk` on older kernels) |
| Force detach (device held open) | Yes | Yes; atom freed later on last `release()` |
| NDU abandon | **No** | **No** — gendisk preserved for adoption |
| NDU adopt (new nvmeibc) | **No** — gendisk reused | **No** |
| Adoption failure (error) | Yes — treated as force-detach | Yes |

### `__kill_gen_disk` detail

```c
static void __kill_gen_disk(struct nvmeiba_atom_os_api *atom)
{
    // 1. Atomically clear atom->disk under disk_lock
    spin_lock_irqsave(&atom->disk_lock, flags);
    disk = atom->disk;
    atom->disk = NULL;
    spin_unlock_irqrestore(&atom->disk_lock, flags);

    if (is_gendisk_ready_for_io(atom)) {   // gendisk_status == 3
        del_gendisk(disk);                  // removes /dev/ entry; blocks new opens
        disk_id_allocator_free(…, disk);    // releases minor number
        blk_cleanup_disk(disk);             // drops last kernel reference
    } else {
        kfree(disk);                        // partially constructed, never add_disk'd
    }
}
```

Setting `atom->disk = NULL` under `disk_lock` is the key synchronization point.
Any concurrent `/proc` read that needs the `disk_name` takes this lock and
handles the NULL case. The deferred destructor in `nvmeiba_bdev_close` checks
`atom->disk == NULL` to know the gendisk is gone.

---

## Detaching: When is the Block Device Finally Closed / Freed?

"Closed" has two meanings here:

### 1. When does the kernel block device disappear from `/dev/`?

At `del_gendisk(disk)`. This is called inside `__kill_gen_disk`, which is
called from `nvmeiba_atom_io_resources_destructor` → `block_api_os_destroy`.

`block_api_os_destroy` is reached after all in-flight I/O is drained
(`block_api_os_drain_io`). For a normal detach, this happens synchronously
during the detach state machine. For a force detach, it still happens during
the same detach call — the drain waits for in-flight I/O to complete regardless
of whether the user has open handles.

After `del_gendisk`, the device node is gone. Any file descriptors still held
by user space point to a dead device.

### 2. When is the atom memory (`kfree`) released?

This depends on whether the device was held open:

**No open handles** (`n_opens == 0` at destroy time):
- `block_api_os_destroy` calls `nvmeiba_os_api_destructor(atom)` → `kfree`
  inline.

**Open handles exist** (`n_opens > 0` at destroy time):
- `atom->disk = NULL` (set in `__kill_gen_disk`).
- Each subsequent `release()` from the kernel (user `close()` or mount teardown)
  calls `nvmeiba_bdev_close` → `__dec_ref_and_destroy_if_needed`:
  ```c
  if ((counter == 0) && (atom->disk == NULL)) {
      nvmeiba_os_api_destructor(atom);  // ← kfree here, on last close()
  }
  ```
- The atom remains in memory until the last user close. During this time
  `atom->status == nvmeiba_status_detaching` and `atom->disk == NULL`.
  The nvmeibc module may already be unloaded (NDU scenario), but because the
  remaining code path is entirely in nvmeiba, this is safe.

### NDU-detaching edge case

When a force-detach is in progress at the same time as NDU (nvmeibc going
down), the atom is in `nvmeiba_status_detaching`. `nvmeiba_os_do_on_nvmeibc_down`
handles this:
```c
if (atom->status == nvmeiba_status_detaching)
    nvmeiba_os_api_set_detaching_abandoned(atom);
    // → disk->fops = &all.detaching_fops  (nvmeiba_b_req_reject)
```
This replaces the nvmeibc-owned reject function with the nvmeiba-owned one,
so rejection continues after nvmeibc unloads. The atom will be freed on
the last user `release()` as described above.

---

## Summary Diagram

```
USER SPACE                     KERNEL                     nvmeiba          nvmeibc
─────────────────────────────────────────────────────────────────────────────────────

── NORMAL OPERATION ──────────────────────────────────────────────────────────────
open("/dev/mc/vol1")     →  fops.open()          →  nvmeiba_bdev_open()
                                                      n_opens++, record pid
write(fd, …)             →  fops.submit_bio()    →                    →  nvmeibc_b_req_make()
                                                                           (execute I/O)
close(fd)                →  fops.release()       →  nvmeiba_bdev_close()
                                                      n_opens--

── NORMAL DETACH ─────────────────────────────────────────────────────────────────
                                                  stop_accepting(reason='D')
                                                      fops.submit_bio = nvmeiba_b_req_reject
                                                  drain_io(): wait io_refcount==0
                                                  atom->status = detaching
                                                  destroy():
                                                      del_gendisk()       ← /dev/ gone
                                                      blk_cleanup_disk()
                                                      nvmeiba_os_api_destructor() → kfree

── FORCE DETACH (device is mounted) ─────────────────────────────────────────────
                                                  block_api_os_get()  (self ref)
                                                  stop_accepting(reason='D')
                                                      fops.submit_bio = nvmeiba_b_req_reject
write(fd, …)             →  submit_bio()         →  nvmeiba_b_req_reject() → -EIO
                                                  drain_io(): wait io_refcount==0
                                                  atom->status = detaching
                                                  destroy():
                                                      del_gendisk()       ← /dev/ gone
                                                      blk_cleanup_disk()
                                                      atom->disk = NULL
                                                      block_api_os_put()  (self ref released)
                                                                           if n_opens==0: kfree
close(fd)                →  fops.release()       →  nvmeiba_bdev_close()
                                                      n_opens--
                                                      if n_opens==0 && disk==NULL: kfree

── NON-DISRUPTIVE UPGRADE (NDU) ─────────────────────────────────────────────────

  OLD nvmeibc going down:
                                                  nvmeibc_down():
                                                      detaching atoms: fops = detaching_fops
                                                  stop_accepting(reason='U')
                                                      fops.submit_bio = nvmeiba_b_req_push
                                                      A->n.orphan_osapi++
write(fd, …)             →  submit_bio()         →  nvmeiba_b_req_push()
                                                      → bio_list_add(pender)  ← BIO queued
                                                  drain_io(): wait io_refcount==0
                                                  atom->status = orphan
                                                  destroy(): queue->queuedata=NULL
                                                             gendisk PRESERVED
  OLD nvmeibc UNLOADED
  (/dev/ entry still exists; open handles unaffected)

  NEW nvmeibc loading:
                                                  nvmeibc_up():
                                                      returns n_orphan_osapi
                                                  block_api_os_create():
                                                      nvmeiba_os_api_orphan_adopt()
                                                      A->n.orphan_osapi--
                                                  block_api_os_init():
                                                      reconnect queue->queuedata
                                                      atom->status = live
                                                  start_accepting_kernel_io():
                                                      fops.submit_bio = nvmeibc_b_req_make
                                                      drain pender.bio_list → nvmeibc
write(fd, …)             →  submit_bio()         →                    →  nvmeibc_b_req_make()
                                                                           (normal I/O resumed)
```

---

## NDU: Orphan Never Adopted

An orphan atom may never be adopted if the new nvmeibc fails to load, fails
mid-adoption, or simply never attaches the volume. The consequences differ by
how far the adoption got before failing.

### Scenario A: nvmeibc never loads (or loads but doesn't attach the volume)

The atom stays on `all.list` indefinitely with `status = nvmeiba_status_orphan`
and `atom->disk` non-NULL (gendisk intact, volume visible in `/dev/`).

**BIO accumulation.** Every kernel BIO hits `nvmeiba_b_req_push`, which appends
it to `atom->pender.bio_list`. There is no capacity limit, no timeout, and no
backpressure mechanism. `n_pend_bio` in `/proc/nvmeiba/status` increases
without bound. Under write load, each BIO holds references to the submitting
process's pages — those pages cannot be reclaimed by the VM. Sustained I/O
against an un-adopted orphan can drive the node into OOM.

**Application behaviour.** Callers of `write()` / `read()` block in the kernel
waiting for `bio_end_io()`, which never arrives. The application hangs
indefinitely with no error signal until the BIO is either executed or failed.

**nvmeiba cannot be unloaded.** `nvmeiba_os_apis_add` calls
`__module_get(default_fops.owner)` on the first atom and `module_put` only
when `n.osapi` drops to zero. `rmmod nvmeiba` is blocked by the kernel for as
long as any atom lives. The module exit function (`nvmeiba_all_os_apis_exit`)
emits `WARN + error 1007` if called with live atoms, but under normal conditions
the kernel simply refuses the unload.

**Upgrade marker persists.** From `ndu.md`: *"The upgrade process is one way
ticket — we don't have rollback — only roll forward."* The marker file
`/var/run/nvmesh/nvmeshclient/upgrade` is only deleted on success or reboot.
Every subsequent `service nvmeshclient restart` will again attempt upgrade
(find and adopt orphans) rather than performing a fresh attach.

**Recovery without reboot.** `block_api_os_autofail_upgrade_io` is an explicit
escape hatch, documented in `nvmeibc_block_api_os.h:145`:
> *"Aux function: In case hot upgrade fails, autofail up to `n_autofails`
> orphan IO to be able to revert to warm upgrade. `n_autofails == -1` means
> all IOs."*

It is exposed via an ioctl debug command (`=<n_bios> ptr=0x<atom_addr>`,
handled in `nvmeibc_block_api_nvmeshioctl.c:224`). Passing `-1` drains the
entire `pender.bio_list`, calling `bio_io_error()` on each BIO. Blocked
applications then receive `-EIO` and unblock. This requires a live nvmeibc
instance to issue the ioctl.

**Recovery with reboot.** The kernel reclaims all memory on reboot; pending
BIOs and their page references are simply abandoned.

### Scenario B: nvmeibc claims the atom but init fails (`is_init_error`)

This is the most dangerous case.

`block_api_os_create` calls `nvmeiba_os_apis_adopt_by`, which removes the atom
from `A->n.orphan_osapi` (counter decremented) but does **not** change
`atom->status` — it remains `nvmeiba_status_orphan`. If `block_api_os_init` or
any subsequent step fails, `block_api_os_destroy` is called with
`os->is_init_error = true`.

Inside `block_api_os_destroy`, because `is_init_error=true` and the atom's
status is still `nvmeiba_status_orphan`, the code takes the *"Regular detach /
init error / adopt error"* branch:

```c
nvmeiba_atom_io_resources_destructor(atom);
// → __kill_gen_disk() → del_gendisk() + blk_cleanup_disk()
```

**The gendisk is destroyed.** The volume disappears from `/dev/`. The source
comment at line 1644 acknowledges this is unresolved:
> `// Daniel: Todo, think how to handle this case. It is very tricky,`
> `// In fact attach of live upgrade causes force-detach.`

Then at the bottom of `block_api_os_destroy`:

```c
} else if (atom->status == nvmeiba_status_orphan) {
    /* Dont call destructor. Each atom holds resources for future adoption
       or will free itself on adoption error */
}
```

The atom is **not freed**. It stays on `all.list` with `status = orphan` but
`atom->disk = NULL` — an inconsistent state. `__is_atom_matching` checks
`status == orphan`, so a future nvmeibc could theoretically "find" this atom,
but `atom->disk` is NULL and there is nothing to reconnect to.

**Pending BIOs are stranded.** `pender.bio_list` still holds all BIOs queued
during the NDU window. Because `__adopt_atom_submit_pending_list` was never
reached, those BIOs are never submitted and never failed. The submitting
processes hang forever.

**Recovery.** There is no userspace escape hatch: `block_api_os_autofail_upgrade_io`
requires a valid `nvmeibc_os_api` pointer from a running nvmeibc instance,
and no live nvmeibc exists. **Node reboot is the only recovery.**

### Summary

| Scenario | Gendisk | Pending BIOs | Application | nvmeiba unloadable | Recovery |
| --- | --- | --- | --- | --- | --- |
| nvmeibc never loads | intact | accumulate without bound | **hang** | No | autofail ioctl (needs live nvmeibc) or reboot |
| Volume not attached (other volumes adopted) | intact | accumulate | **hang** | No | autofail ioctl or reboot |
| nvmeibc loads, init fails (partial adopt) | **removed** (`del_gendisk`) | stranded forever | **hang** | No | **reboot only** |

---

## Key Invariants

1. **`fops.open` and `fops.release` always point to nvmeiba.** They never
   change across NDU or detach. nvmeiba always safely handles `open()`/`close()`
   regardless of nvmeibc's state.

2. **The gendisk is never removed during NDU.** Only a real detach calls
   `del_gendisk`. NDU only replaces `fops.submit_bio`.

3. **`atom->disk = NULL` is the destruction signal.** Setting it atomically
   under `disk_lock` makes `nvmeiba_bdev_close` aware that the block device is
   gone, enabling the deferred `kfree`.

4. **`n_opens > 0` prevents `kfree`.** The atom memory outlives `del_gendisk`
   for as long as user processes hold open file descriptors. nvmeibc being
   unloaded does not affect this — the surviving code path is entirely in nvmeiba.

5. **Orphan atoms stay on `all.list`.** They are findable by the new nvmeibc
   instance by name+directory. Adoption removes them from the orphan count.

6. **Detaching atoms during NDU** get their `submit_bio` replaced with the
   nvmeiba-owned reject handler (`nvmeiba_os_api_set_detaching_abandoned`) so
   rejections continue safely after nvmeibc unloads.

7. **An un-adopted orphan accumulates BIOs without bound.** There is no
   timeout, no backpressure, and no automatic failure. Memory grows with every
   queued BIO. The only runtime escape is `block_api_os_autofail_upgrade_io`.

8. **Partial adoption failure leaves the atom in an inconsistent state.**
   `del_gendisk` is called (gendisk gone) but the atom struct stays on
   `all.list` with `status = orphan` and `disk = NULL`. Pending BIOs are
   stranded with no path to execute or fail them. Reboot is required.
