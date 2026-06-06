# PET Message Rotation With Protected Prefix

## Purpose

PET journals are fixed-size per-entity buffers. Rotation lets a journal keep
accepting messages after the append pointer reaches the physical end of the
buffer, while preserving an explicitly protected prefix.

The intended IO PET shape is:

```text
[ operation context ][ latest execution history ]
  protected prefix      rotating suffix
```

The prefix describes the entity: operation type, debug id, topology, LBA range,
or other context that makes later messages readable. The suffix keeps the most
recent messages and may overwrite older suffix messages.

## Entity Layout

Each flushed entity starts with a packed stream header:

```c
struct __attribute__((packed)) nvmeib_pet_stream_header {
    u64 commit_id;
    u16 journal_size;
};
```

`journal_size` is the number of committed bytes in this entity. The writer
copies this header during `nvmeib_pet_stream_commit()` and shrinks the flushed
`iovec` to `journal_size`.

The viewer scans only the range:

```text
[entity_start, entity_start + journal_size)
```

It does not use a message counter and it does not infer the scan boundary from
the commit id.

## Record Layout

Every record in the committed range starts with:

```c
struct __attribute__((packed)) nvmeib_pet_msg_header {
    u16 section_offset;
    union {
        struct __attribute__((packed)) {
            u64 timestamp;
            u8 args_n_bytes;
        } msg;
        struct __attribute__((packed)) {
            u64 bytes;
            u8 unused;
        } spacer;
    };
};
```

Normal message:

```text
section_offset != 0
msg.timestamp = message timestamp
msg.args_n_bytes = serialized argument payload bytes
payload follows the header
```

The stored message id is the raw dictionary section offset plus one. That keeps
`section_offset == 0` reserved for rotation spacers.

Spacer:

```text
section_offset = 0
spacer.bytes = payload bytes after this spacer header
spacer.unused = 0
payload bytes are skipped by the viewer
```

The total spacer record size is:

```text
sizeof(struct nvmeib_pet_msg_header) + spacer.bytes
```

Small tails that cannot hold a full spacer header are not parsed. The writer
hides them by reducing `journal_size`.

## Stream State

The writer keeps three byte offsets:

```text
max_written_bytes  highest committed byte in data; becomes journal_size
write_offset       next physical byte to allocate
protected_prefix   bytes in [0, protected_prefix) are retained by rotation
```

At stream creation, all three offsets start at `NVMEIB_PET_ENTITY_HEADER_SIZE`
for an active journal. The protected area initially contains only the stream
header.

`nvmeib_pet_journal_protect_prefix()` extends the protected area to the current
end of written data:

```text
before protect:
  [ header ][ context messages ][ unwritten suffix ................. ]
            ^ max_written_bytes

after protect:
  [ header ][ context messages ][ rotating suffix .................. ]
                              ^ protected_prefix/write_offset
```

Inactive journals ignore the protect call. Active journals assert that after
protection the suffix still has room for at least two max-size messages
(`NVMEIB_PET_MIN_ROTATABLE_N_BYTES`).

## Allocation

`nvmeib_pet_stream_alloc()` is the public stream allocator used by message
macros. It validates message size and stream invariants, then uses two paths.

Fast append:

```text
if write_offset + size reaches or extends EOF
and still fits in data.iov_len:
    return current write_offset
    write_offset = write_offset + size
    max_written_bytes = write_offset
```

This is the expected good path. It does not write a spacer because there is no
old live data after the new message.

Slow rotation:

```text
__nvmeib_pet_stream_allocate_rotate()
```

The slow path is used only when fast append cannot cover the write. It:

- wraps to `protected_prefix` when the physical tail cannot fit the message
- shrinks `max_written_bytes` before wrap when the old tail becomes EOF
- scans existing records to find how many bytes can be consumed
- consumes enough space for the new message, and when needed, a spacer header
- writes a spacer for header-sized leftovers
- hides tiny EOF leftovers by reducing `max_written_bytes`

Normal messages are never split across the physical end of the buffer.

## Viewer Contract

After every successful allocation and commit, the viewer must be able to scan
the committed range as:

```text
(msg)(msg|spacer)*(msg|eof)
```

`eof` is `journal_size`. Bytes after `journal_size` are outside the entity and
must not be read.

The viewer parses records linearly:

```text
if section_offset != 0:
    read args_n_bytes payload bytes
    emit raw message

if section_offset == 0:
    require unused == 0
    skip spacer.bytes payload bytes
```

The viewer never searches blindly for the next nonzero offset. Stale payload
bytes are safe only because the writer either covers them with a spacer or
moves EOF before them.

## Timestamp Order

Rotation can make physical order differ from logical timestamp order inside one
entity:

```text
physical: [ protected ][ newer prefix ][ older suffix ]
logical:  [ protected ][ older suffix ][ newer prefix ]
                         ^ timestamp drop
```

`PetArchiveReader.read_entity()` unrotates messages for each entity by detecting
the timestamp drop created by rotation. This is per-entity behavior. Viewer
commands may still apply their own global timestamp sort across entities unless
the user passes `--no-sort`.

## API Notes

- `nvmeib_pet_journal_protect_prefix()` should be called after the messages
  that make the entity understandable are written.
- Writes to an active rotating journal can continue after the physical buffer
  fills. Do not treat a zero return as the normal "journal full" signal.
- `worst_severity` is monotonic once a message is written. A later rotation may
  remove that message from the visible journal, but it does not lower the
  recorded worst severity.

## Testing And Demo Artifacts

The PET test suite includes deterministic and random rotation coverage. The
random rotation test writes small 512-byte journals and produces artifacts under
the build rotation directory:

```text
build/rotations/seed_*.pet
build/rotations/seed_*.expected.txt
build/rotations/seed_*.viewer.txt
```

`make demo` generates the PET files and expected raw output, runs the Python
viewer on each generated journal, and compares expected output against viewer
output.

## Future Work

The implemented format intentionally does not add a second entity envelope,
magic value, generation counter, or dropped-message counters. Those may be
added later if corruption handling or richer rotation statistics require them.
