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

struct nvmeib_pet_base_controller{
    //flush should be callable from the "interrupt context"
    void (*flush)(struct nvmeib_pet_base_controller const* self, enum nvmeib_pet_severity severity, struct iovec const data);
    struct iovec (*get_buffer)(struct nvmeib_pet_base_controller const* self);
    void (*put_buffer)(struct nvmeib_pet_base_controller const* self, struct iovec data);
};
```

The concrete platform is hidden behind the “`struct nvmeib_pet_base_controller`” interface. 

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

## Kaitai & Viewer

The serialization format is really simple and platform independent. It took me 6 days to implement it efficiently. The performance is pretty good: \~50 messages per nanosecond. 

```c
struct nvmeib_pet_message<size_t n_args>
{
    u16 offset;
    u8 /*enum nvmeib_pet_store_type*/ type[1 + n_args]; 
    u64 value[1 + n_args];                      
};

u16 write(struct nvmeib_pet_message<size_t n_args>const& msg, struct nvmeib_pet_stream& out)
{
    out.write(offset); //2 bytes
	  out.write(n_args); //1 bytes
	  for(id : array_size(msg.type)) {
		  out.write(type); //1 byte
		  out.write(value); //1,2,4 or 8 bytes
	  }
	  return written;
}
```

**Note:** since the format string already contains the data type description, we may avoid writing the value type, but then we cannot use [Kaitai](https://kaitai.io/).

[Kaitai Struct](https://kaitai.io/) is a declarative language used to describe various binary data structures. [Kaitai Struct](https://kaitai.io/) project was used to generate Python source code to parse generated data. The following is slightly redacted version of the file, we use in the PET project

```c
meta:
  id: nvmeib_pet_archive
  file-extension: pet
  endian: le
enums:
  pet_store_type:
    0: pet_store_type_s_byte
    1: pet_store_type_u_byte
types:
  pet_variant:
    seq:
      - id: type
        type: u1
        enum: pet_store_type
      - id: sv1
        type: s1
        if: type == pet_store_type::pet_store_type_s_byte
      - id: uv1
        type: u1
        if: type == pet_store_type::pet_store_type_u_byte
      - id: sv2
        type: s2
      message:
    seq:
      - id: offset
        type: u2
      - id: num_args
        type: u1
      - id: timestamp
        type: pet_variant
      - id: args
        type: pet_variant
        repeat: expr
        repeat-expr: num_args
  entity:
    seq:
      - id: num_messages
        type: u2
      - id: messages
        type: message
        repeat: expr
        repeat-expr: num_messages
instances:
  entities:
    type: entity
    repeat: eos
```

#### Shortcuts or time to market

In this case, the price is moderate and we still need to pay it; 

The productization(5 days) task is ahead of us. The preprocessor generated dictionaries are already integrated into our build system and deployment process. In case, we will switch to the “standard” tracing implementation \- all this work should be deleted. 

Another area is integration with the pager. The data written into the “io.pet” channel is “semi sorted”:

* It is sorted within a single entity buffer  
* It is NOT sorted between different entities buffers. 

The pager must introduce a special treatment to this channel. It will have to sort all messages within “nvmeibc\_io\_pet0.\*” files and only then to show them.

**Note:** regardless of the current project decision, I think we should evaluate Kaitai in other areas too. It is capable of describing Serjio or providing a “plug-in” for our “pager” functionality. 