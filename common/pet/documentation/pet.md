# Per Entity Tracing

*“PET” will be used for abbreviation.*  

# Brainstorming

The main “traditional logging” problem: 

1. We cannot turn-on the datapath logging in the production(high I/O rate \+ low disk endurance).  
2. We cannot analyze the datapath problems, without logging.

In the I/O context, logging approach has additional problems:

1. “Append only” property. 99.999999% I/O operations are ended successfully. So we can NOT discard the successful operations logs. For problematic operations, we also need the “non-problematic” part.   
2. Referring to an I/O execution context is expensive. We should allocate an unique id and associate it with any message, referring to that context.  
   

The datapath cannot afford this. The idea is to create a small area 1-2 KB per I/O operation, so the log messages would be written there. If the operation ends with success, all messages are discarded, otherwise the messages are written to the local disk. Once the operation context is described, the rest of the messages may use short identifiers.

## Pros & Cons

Cons:

* Increased memory usage  
* Not a silver bullet: does not help to solve “distributed” and probably many other problems 

Pros:

* Provides the detailed log of the failed IO. 

# Design

## The state of the art

Today, we use the following components to trace the operation execution:

1. The preprocessor   
   1. Based on the source code, generates:  
      1. Compact message representation & serialization  
      2. The message “write to  log” functionality  
      3. The messages and the tokens dictionary  
   2. The preprocessor has at least 3 different backends:   
      1. User space POSIX based multi threaded application (kernel simulator \+ TOMA)  
      2. User space cooperative multithreading (UM \+ UM simulator)  
      3. Kernel modules  
2. The tracing subsystem  
   The tracing subsystem is responsible for storing the traces in the persistent storage. Obviously, we have at least 3 different subsystems. The “kernel tracing” subsystem is pretty complex one, since it deals with:  
* Per CPU trace channels  
* Memory management  
  * The trace buffers are preallocated on the modules init  
  * The trace buffers are reused  
  * The trace buffers are written to the disk, with zero data copy  
* The trace daemon \- the user space process, which listens to the trace channels and writes the data to the persistent storage. It also implements the log rotation policy  
3. The pager   
   Allows to view the messages and provides some indexing services.

The proposed solution does NOT use the components above, but provides an alternative implementation. “Time to market” is/was the main reason. Also, the proposed solution allows integrations, with the components above, in some future.

## Solution

### The main points

1. The messages are embedded within the executable. The messages are stored in some specific data section.  
2. At runtime, it is possible to calculate the message offset from the beginning of the section in O(1).  
3. The messages use C “printf” format specification.   
4. GCC/CLang extensions or C11 standard allow implementing “compile time function overloading”. Thus, we can have a clean API for the fundamental type serialization.   
5. The entity buffer is single threaded.  
6. The number of messages, sent via PET functionality, should be 0(zero) in production.

### PET data hierarchy

PET is easier to understand as three related views. `journalbuf` belongs to the
C implementation view; when discussing the archive or viewer, it is usually
enough to talk about committed journals/entities and messages.

#### Dictionary build-time view

```text
executable/module
  1:1    -> nvmeib_pet_messages ELF section
             1:N -> message_description
  1:0..1 -> DWARF debug info
             1:N -> type

dictionary
  1:N    -> message_description
  1:0..N -> type
```

The dictionary is generated from one executable/module. It copies
`message_description` records from the PET ELF section and the requested
user-defined `type` records from DWARF.

#### Runtime C implementation view

```text
journal
  1:1 -> journalbuf
         1:N -> message or spacer

message
  N:1 -> message_description
```

`struct nvmeib_pet_journal` is the per-entity runtime object. Its
`struct nvmeib_pet_journalbuf` is the bounded byte buffer that stores the
physical records, including rotation spacers. A stored message does not copy the
description; it stores `message_id = message_index + 1`.

#### Archive and viewer view

```text
archive
  1:N -> committed journal/entity
         1:N -> message

committed journal/entity
  N:1 -> dictionary

message
  N:1 -> dictionary.message_description

dictionary.message_description
  N:0..N -> dictionary.type
```

Each committed journal/entity carries a `commit_id`; the viewer uses it to pick
the matching dictionary. The message id selects a `message_description` inside
that dictionary, and struct/union/enum rendering uses the optional
DWARF-derived `type` entries stored beside the message descriptions.

### Current journal storage

The current PET journal buffer is a bounded byte journal. A committed entity starts
with:

```c
struct __attribute__((packed)) nvmeib_pet_trace_clock {
    u64 tsc_offset;
    u32 tsc_khz;
};

struct __attribute__((packed)) nvmeib_pet_journalbuf_header {
    u64 commit_id;
    u16 journalbuf_size;
    struct nvmeib_pet_trace_clock trace_clock;
};
```

`journalbuf_size` is the committed scan boundary. The viewer parses only bytes
inside this range and treats the records after the header as either normal
messages or rotation spacers. `trace_clock` lets the viewer convert the raw TSC
ticks stored in each message header to nanoseconds:

```text
ns = (ticks + tsc_offset) * 1000000 / tsc_khz
```

Every record in the committed range starts with:

```c
struct __attribute__((packed)) nvmeib_pet_journalbuf_message_header {
    u16 message_id;
    union {
        struct __attribute__((packed)) {
            u64 timestamp;
            u8 args_n_bytes;
        } msg;
        struct __attribute__((packed)) {
            u64 unused;
            u8 args_n_bytes;
        } spacer;
    };
};
```

Normal messages store a `message_id` value. `message_id == 0` is reserved for a
spacer record; real messages store `message_index + 1`, where `message_index` is
the index of a fixed `struct nvmeib_pet_message_description` record in the
`nvmeib_pet_messages` ELF section. A spacer does not describe a message; it
tells the viewer how many payload bytes to skip.

Normal message record:

```text
message_id != 0
message_index = message_id - 1
msg.timestamp = raw TSC ticks
msg.args_n_bytes = serialized argument payload bytes
payload follows the header
```

Spacer record:

```text
message_id = 0
spacer.unused = 0
spacer.args_n_bytes = payload bytes after this spacer header
payload bytes are skipped by the viewer
```

The journal buffer keeps:

* `max_written_bytes` - the highest byte written; committed as `journalbuf_size`
* `write_offset` - the next physical byte to allocate
* `protected_prefix` - the range `[0, protected_prefix)` retained by rotation

### Protected prefix and rotation

PET journals are fixed-size per-entity buffers. Rotation lets a journal keep
accepting messages after the append pointer reaches the physical end of the
buffer, while preserving an explicitly protected prefix:

```text
[ operation context ][ latest execution history ]
  protected prefix      rotating suffix
```

The prefix describes the entity: operation type, debug id, topology, LBA range,
or other context that makes later messages readable. The suffix keeps the most
recent messages and may overwrite older suffix messages.

At journal buffer creation, all three offsets start at
`NVMEIB_PET_JOURNALBUF_HEADER_SIZE` for an active journal. The protected area
initially contains only the journal buffer header.

The user may call `nvmeib_pet_journal_protect_prefix()` after writing the
messages that describe the entity context:

```text
before protect:
  [ header ][ context messages ][ unwritten suffix ................. ]
            ^ max_written_bytes

after protect:
  [ header ][ context messages ][ rotating suffix .................. ]
                              ^ protected_prefix/write_offset
```

Later messages are written into the rotating suffix. If the suffix wraps, older
suffix messages may be replaced by newer messages or spacers, but the protected
prefix stays visible. Active journals assert that after protection the suffix
still has room for at least two max-size messages
(`NVMEIB_PET_MIN_ROTATABLE_N_BYTES`).

The fast allocation path appends at `write_offset`, advances
`max_written_bytes`, and does not write a spacer because there is no old live
data after the new message. The slow rotation path is used only when fast append
cannot cover the write. It wraps to `protected_prefix` when needed, scans
existing records to find bytes that may be consumed, writes spacers for
header-sized leftovers, and hides tiny EOF leftovers by reducing
`max_written_bytes`. Normal messages are never split across the physical end of
the buffer.

The viewer contract is:

```text
(msg)(msg|spacer)*(msg|eof)
```

where `eof` is `journalbuf_size`. Messages inside one entity are unrotated by
raw tick drop detection and converted to nanoseconds before they are returned
to higher-level viewer code.

The viewer parses records linearly:

```text
if message_id != 0:
    read args_n_bytes payload bytes
    emit raw message

if message_id == 0:
    require unused == 0
    skip args_n_bytes payload bytes
```

The viewer never searches blindly for the next nonzero offset. Stale payload
bytes are safe only because the writer either covers them with a spacer or
moves EOF before them.

Rotation can make physical order differ from logical timestamp order inside one
entity:

```text
physical: [ protected ][ newer prefix ][ older suffix ]
logical:  [ protected ][ older suffix ][ newer prefix ]
                         ^ timestamp drop
```

`PetArchiveReader.read_entity()` unrotates messages for each entity by detecting
the raw tick drop created by rotation, then converts ticks to nanoseconds. This
is per-entity behavior. Viewer commands may still apply their own global
timestamp sort across entities unless the user passes `--no-sort`.

API and testing notes:

* `nvmeib_pet_journal_protect_prefix()` should be called after the messages
  that make the entity understandable are written.
* Writes to an active rotating journal can continue after the physical buffer
  fills. Do not treat a zero return as the normal "journal full" signal.
* `worst_severity` is monotonic once a message is written. A later rotation may
  remove that message from the visible journal, but it does not lower the
  recorded worst severity.
* `make -C nvmesh.kernel/common/pet demo` generates random rotation journals under
  `build/rotations/` and compares Python viewer output with expected output.

### Usage example

```c
NVMEIBC_IO_PET_MSG_NORM(
    &o->journal, 
    "read operation started; o=%p, dbg_id=%u, short volume id=%u, topology=%llu, vlba=0x%llx, nlbas=%llu, dbg_cntrs=0x%llx", 
    o, o->dbg_id, vol_id, topo, start_lba, nlbas, o->dbg_cntrs.raw);
```

1. “**`NVMEIBC_IO_PET_MSG_NORM”`** macro is used to write data to some managed buffer, called a “journal”.  
2. The format string, as mentioned above, is using “printf” spec. The message and the arguments are verified during the compilation process.

#### Shortcuts or time to market

1. The messages are part of the executable.  
2. Python ELF package is used to extract the messages from any ELF file (40 lines of Python code).   
3. Since, “printf” format was used, it took only 4 lines of Python code to use the “sprintf” function from the “[libc.so](http://libc.so).6” library, to create “human readable messages".

In my opinion, the interface (macro) itself is “optimal”. If, in the future we will decide to switch to the preprocessor tool, then we will have to

1. Create one more backend, which works on top of “journal”.   
2. Translate all messages from “printf” spec to “NVMESH trace” format. I believe we will add \~100 messages to the code. 

### UM/Kernel/Posix platforms integration

```c
enum nvmeib_pet_severity{
    NVMEIB_PET_SEVERITY_NORMAL   = 0, //periodic dump to see what is going on
    NVMEIB_PET_SEVERITY_WARNING  = 1, //the operation experienced some delay or probably visited resubmitted
    NVMEIB_PET_SEVERITY_ERROR    = 2, //the operation received a network/disk error
    NVMEIB_PET_SEVERITY_CRITICAL = 3, //DI problem was found, something unexpected
};

struct nvmeib_pet_buffer{
    struct iovec data;
    s16 release_cpu;
};

struct nvmeib_pet_base_controller{
    //flush should be callable from the "interrupt context"
    void (*flush)(struct nvmeib_pet_base_controller const* self, enum nvmeib_pet_severity severity, struct iovec const data);
    struct nvmeib_pet_buffer (*get_buffer)(struct nvmeib_pet_base_controller const* self);
    void (*put_buffer)(struct nvmeib_pet_base_controller const* self, struct nvmeib_pet_buffer buffer);
};
```

The concrete platform is hidden behind the “`struct nvmeib_pet_base_controller`” interface. 

The `nvmeib_pet_buffer` contains the trace data `iovec` and an optional release token for controllers that need to return accounting to the exact CPU or shard that admitted the buffer.

The “PET” framework does not care about the memory management; The buffer may come from some preallocated memory or allocated just-in-time. The platform should provide the memory. The “PET” should behave correctly, in case there is no memory.

The “flush” functionality has the following contract:

* “flush” should be callable even from an interrupt context  
* The “PET” will NOT wait for “flush” completion. After the “flush” is called, the PET immediately calls “put\_buffer”.   
* If needed, the user may call “get\_buffer” once again.

#### Shortcuts or time to market

Let’s assume that we implemented for every platform the optimal memory management and flush functionality. So, we need to reimplement 3 functions (flush, get/put buffer). So far, I spent 4 days to implement 5 different PET controllers:

1. Kernel   
   1. “msgloop” functionality is used allocate messages and write data to the user space (1.5 day)  
   2. trace daemon was modified to read the data from a new kernel file and store it on the disk(2 days)  
2. Kernel simulator  
   1. A thin wrapper on top of malloc/free & regular file (2 hours)  
3. PET tests  
   1. A verification & performance controllers were implemented (3 hours)  
4. Other platforms  
   1. “Do nothing” stabs were implemented.   
   2. DPLib was taken into consideration. The UM will pass the concrete implementation to it. Thus all “syncs” will just work.

### Kernel implementation details

Today, all log channels are created from the “common” module. The trace daemon is listening on all channels. In case, the channel is not in use (non converged mode), the trace daemon “wastes” some memory.

The PET project follows the existing practices. The “/proc/nvmeib/io.pet/io.pet” file is created on the “common” module initialization. On “nvmeibc” module initialization, the “struct io\_pet\_controller“  instance is created and stored under “struct nvmeibc\_control\_api” instance. From there, it is propagated to the volumes and block devices, during their initialization.

The kernel implementation exposes control over the minimal “flush” severity level and the single PET buffer size. “Io.pet msgloop” instance may hold a maximum 256 buffers, waiting to be written to disk. In case of “overflow” the submitted buffers content will be lost. The error counter will be incremented.
