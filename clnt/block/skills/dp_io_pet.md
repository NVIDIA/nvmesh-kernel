<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: GPL-2.0-only OR Apache-2.0
-->

---
name: dp-io-pet
description: Use when adding, reviewing, or interpreting NVMesh datapath IO PET journal messages emitted with NVMEIBC_IO_PET_MSG* macros.
---

# Datapath IO PET

## Use This When

- Adding or reviewing `NVMEIBC_IO_PET_MSG*` instrumentation in the IO datapath.
- Interpreting PET viewer output from `io.pet` / `.pet` traces.
- Mapping a printed PET message back to the emitting callsite.
- Deciding whether a value should get a `<union ...>`, `<enum ...>`, or `<errno>` viewer annotation.

## Core Model

PET is a per-entity journal, usually per IO operation. Messages are appended to `struct nvmeib_pet_journal` during execution and flushed on commit according to the journal's worst severity and controller policy.

For the block IO path:

- Public IO macros live in `clnt/nvmeibc_io_pet.h`.
- Generic journal storage, type selection, and commit logic live in `common/pet/nvmeib_pet_specification.h`.
- Kernel/controller plumbing lives in `clnt/nvmeibc_io_pet.c`.
- Design docs live under `common/pet/documentation/`.

The IO macro stores one fixed `struct nvmeib_pet_message_description` record in the common `nvmeib_pet_messages` ELF section. The runtime journal stores `message_id = message_index + 1`, with `0` reserved for rotation spacers. The viewer uses the dictionary layout from that record to decode serialized argument bytes, then uses the format string only for presentation and annotations.

## Message API

Use the severity shortcuts when severity is static:

```c
NVMEIBC_IO_PET_MSG_NORM(&o->journal,
	"operation.create(short_volume_id=%d, type=%d<enum nvmeib_block_io_op>, vlba_start_s=0x%llx)",
	short_volume_id, o->op, get_op_start_lba(o));

NVMEIBC_IO_PET_MSG_WARN(&o->journal,
	"read_jmdc.response(si=%hhu) = %d",
	numeric_downcast(u8, si), rv);
```

Use `NVMEIBC_IO_PET_MSG()` when severity is data-dependent:

```c
enum nvmeib_pet_severity severity =
	rv ? NVMEIB_PET_SEVERITY_WARNING : NVMEIB_PET_SEVERITY_NORMAL;

NVMEIBC_IO_PET_MSG(&o->journal,
	"operation.destroy(rv=%d<errno>)",
	severity, rv);
```

The macro returns written bytes (`u16`). Most production callsites ignore it. A
zero return means the message was not written, usually because the journal is
inactive or the stream is invalid. Do not use zero as the normal "journal full"
signal: active rotating journals can keep writing by replacing older suffix
records.

Severity guide:

| Severity | Use for |
|---|---|
| `NORM` | Normal execution, expected in 99.999% of IOs. |
| `WARN` | Transient error, retry, delay, resubmit, or another unusual but recoverable condition. |
| `ERROR` | User data write failed. |
| `CRIT` | Data integrity error or corruption was discovered. |

## Format And Type Rules

The message string is `printf`-checked at compile time. The PET viewer also understands optional type annotations placed immediately after a conversion specifier:

```c
"binfo=0x%x<union nvmeib_blkset_info>"
"status=%hhu<enum nvmeibc_block_lock_status>"
"rv=%d<errno>"
```

Static validation contract:

- `nvmeib_pet_journal_add_msg_verify_format()` is a `printf`-attribute helper. It catches ordinary `printf` mistakes, but ordinary `printf` rules include integer promotions.
- PET does not store promoted varargs. PET serializes a packed payload whose fields are `typeof(arg)` and whose payload size is `sum(sizeof(arg))`.
- In the current dictionary/viewer path, the format string is still used to infer the serialized payload width. Therefore `%x` with `numeric_downcast(u8, value)` is legal for `printf` but invalid for the current PET viewer: the viewer will expect 4 bytes while PET stored 1 byte.
- Until layout-driven dictionaries are implemented, the format string must describe the serialized PET payload width, not only what `printf` would accept after vararg promotion.
- Do not add viewer-side guessing to compensate for mismatches. A payload-size mismatch means the callsite format, the argument type, or the generated dictionary is wrong/stale.
- The planned long-term fix is documented in `common/pet/documentation/pet_static_layout_validation.md`: dictionary layout metadata will drive binary parsing, and format strings will be used only for presentation.

Rules:

- Keep the `printf` specifier correct for the actual argument width.
- Use `%hhu` / `%hhx` for `u8`, `s8`, `uint8_t`, `int8_t`, and `numeric_downcast(u8|s8, ...)`.
- Use `%hu` / `%hx` for `u16`, `s16`, `uint16_t`, `int16_t`, and `numeric_downcast(u16|s16, ...)`.
- Use `%u` / `%x` for 32-bit integer payloads.
- Use `%lu` / `%lx` or `%llu` / `%llx` only when the stored expression type is actually 64-bit or pointer-sized as appropriate.
- Add `<enum ...>` for compact enum values that should render as names.
- Add `<union ...>` for raw packed fields where bit expansion is useful.
- Add `<errno>` for return values where errno decoding helps.
- Pass raw union storage such as `.all` and annotate with the matching union type.
- Use `numeric_downcast(u8, value)` with `%hhu<enum ...>` for packed byte-sized enums.
- PET supports 1 to 12 message arguments.
- Do not pass `float`, `double`, `char *`, or `char const *` as PET arguments.
- Do not rely on side effects in message arguments. Arguments are skipped when the journal is inactive, but evaluated when active even if the journal is later discarded by severity policy.
  In an active rotating journal, a message may also be evaluated and written,
  then later rotated out of the visible suffix.

Bad and good examples:

```c
/* Bad: stores 1 byte, format describes 4 bytes. */
NVMEIBC_IO_PET_MSG_NORM(&o->journal,
	"disk_io.request(flags=0x%x)",
	numeric_downcast(u8, flags));

/* Good: stores 1 byte, format describes 1 byte. */
NVMEIBC_IO_PET_MSG_NORM(&o->journal,
	"disk_io.request(flags=0x%hhx)",
	numeric_downcast(u8, flags));
```

Common IO PET annotations currently used:

```text
<errno>
<enum nvmeib_block_io_op>
<enum nvmeib_gen_cmd_op>
<enum nvmeibc_disk_locks_opr>
<enum nvmeibc_rdma_intent>
<enum nvmeibc_block_lock_status>
<enum e_cmds_stage>
<union nvmeib_blkset_info>
<union nvmeib_lock_id>
<union nvmeibc_raid1_io_pet_status>
<union operation_dbg_cntrs>
<union nvmeibc_block_dp_ec_data_block_md>
<union jblock_md>
```

## Existing Message Families

Use these prefixes consistently:

- `operation.create`, `operation.execute`, `operation.end`, `operation.destroy`
- `disk_io.request`, `disk_io.response`
- `read_pb.request`, `read_pb.response`
- `write_pb.request`, `write_pb.response`
- `lock.request`, `lock.response`
- `rdma.request`, `rdma.response`
- `sync_start`, `sync_end`
- `read_jmdc.*`, `free_jrnl_ents.*`, `send_recovered_blkset.*`

Prefix meanings:

| Prefix | Use for |
|---|---|
| `operation.*` | Lifecycle of one IO operation. |
| `disk_io.*` | Disk command request/response around one segment command. |
| `read_pb.*` | Piggybacked read-lock or blockset-info data on a disk read. |
| `write_pb.*` | Piggybacked blockset-info write on a disk write. |
| `lock.*` | Lock state-machine request/response. |
| `rdma.*` | Standalone RDMA operation not naturally tied to `disk_io.*`. |
| `sync_*` | Sync/recovery operation lifecycle. |
| `read_jmdc.*` | Journal metadata read during EC recovery. |
| `free_jrnl_ents.*` | Journal cleanup request/response. |
| `send_recovered_blkset.*` | Recovered blockset writeback path. |

Important callsite areas:

- `clnt/block/datapath_utils_generic/operation/nvmeibc_block_dp_submit_bio_func.h`
- `clnt/block/datapath_utils_generic/nvmeibc_block_dp_operation.c`
- `clnt/block/datapath_utils_generic/nvmeibc_block_dp_io_generic_cmds.c`
- `clnt/block/datapath_utils_generic/nvmeibc_block_dp_io_req_rel_locks.c`
- `clnt/block/datapath_ec/recov/`
- App-side analogues under `app/clnt/block/data_path/`, `app/clnt/block/managers/`, and `app/clnt/block/volume/`

## Rotation And Protected Context

PET journals are bounded. The current journal buffer format keeps:

- a protected prefix, usually the entity header plus early context messages
- a rotating suffix, which retains the latest later messages

After writing the messages that make the IO understandable, call:

```c
nvmeib_pet_journal_protect_prefix(&o->journal);
```

The protect call extends the retained prefix to the current journal end and
makes later writes rotate in the suffix. Use it after stable context such as
operation type, debug id, volume/topology identity, LBA range, and initial
metadata. The suffix must still have enough room for rotation after protection.

When adding messages, keep the protected context compact and high value. Later
request/response or state-machine breadcrumbs can live in the rotating suffix.
If a failure produces many suffix messages, older suffix breadcrumbs may be
gone while the protected context remains.

## Reading PET Viewer Output

The reader unrotates messages inside each entity when rotation wrapped the
physical journal. Viewer output may still be globally interleaved by timestamp.
Read it by `entity=[N]` to reconstruct one operation lifecycle.

For observed simulator trace shapes, use `clnt/block/skills/dp_io_pet_patterns.md`. It catalogs the unique execution-flow patterns from `app/*_kc_*pet.log` and normalizes away raw values such as timestamps, addresses, lock ids, txids, and pointers.

Entity skeleton:

The viewer may print entity metadata as `size=<bytes> skeleton=<hash>`. The skeleton is a stable hash of the entity's commit id plus ordered PET message ids. It represents the execution-flow shape of the entity while ignoring argument values such as addresses, txids, lock ids, and timestamps. Use it to group IOs that followed the same PET message path.

Example decoded fields:

```text
binfo=0xff000015<{bits: {txid: 0x15, dirty: 0xff0}, all: 0xff000015}>
contending=0x100f321f<{bits: {idx_in_praid: 0xf, lock_id: 0xf321, is_stale: 0x1, is_read: 0, reserved: 0}, all: 0x100f321f}>
raid_cur_stage=4<DO_IO_AND_PAR>
lock_status=4<TAKEN>
rv=0<errno>
```

Interpretation pattern:

- Group by `entity=[N]`.
- Identify lifecycle anchors first: `operation.create`, `operation.execute`, `operation.end`, `operation.destroy`.
- Then inspect disk/RDMA/lock sub-events between those anchors.
- Treat typed expansions as the semantic values; raw hex is still useful for exact comparisons.
- If two entities refer to the same address, lock id, or blockset info, they may be related even when the operation timelines are separate.
- If an entity rotated, missing older suffix messages may be a retention effect.
  The protected prefix should still contain enough context to understand the
  remaining suffix.

## Adding A New Message

Checklist:

- Keep the event name stable and grep-friendly: `component.action(...)`.
- Prefer one compact message over several low-signal messages.
- Choose severity intentionally. Use warning or worse only when the operation experienced a real problem or unusual recovery path.
- Use typed annotations for packed unions and enums that a human would otherwise need to decode manually.
- Keep argument count under 12.
- Guard detailed per-block or content dumps with `nvmeib_pet_journal_is_verbose()`.
- Avoid strings and expensive formatting. PET stores compact typed values, not string payloads.
- Consider the finite journal buffer. The default kernel PET buffer is
  intentionally small; protect only compact context and leave room for the
  rotating suffix.

## Useful Searches

```bash
rg -n "NVMEIBC_IO_PET_MSG(_NORM|_WARN|_ERROR|_CRIT)?\\(" nvmesh.kernel/clnt app/clnt app/test/simulator/client
rg -n "<(enum|union) [^>]+>|<errno>" nvmesh.kernel/clnt/block app/clnt/block
```
