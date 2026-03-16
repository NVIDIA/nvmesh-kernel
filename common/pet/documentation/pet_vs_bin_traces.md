# PET vs Binary Traces (`nvmeib_utils_bin_traces.h`)

## Scope
This document compares:
- PET (Per Entity Tracing) used via `NVMEIBC_IO_PET_MSG*` in `clnt/nvmeibc_io_pet.h`
- Existing binary trace wrappers (`_NF/_ND/_NT/_NI/_NW/_NE` and related) in `common/nvmeib_utils_bin_traces.h`

## Executive Summary
- PET is a per-operation journal: it accumulates compact records in a private buffer and flushes once on completion (typically when severity is high enough).
- Binary traces are global channel events: each call emits immediately to trace channels (optionally mirrored to dmesg), optimized for continuous system-wide observability.
- PET is better for reconstructing one I/O lifecycle with low good-path persistence.
- Binary traces are better for broad, always-on diagnostics, control-plane/state-machine visibility, and existing pager/channel workflows.
- Migration is asymmetric: binary traces can be moved to PET patterns, but PET cannot be losslessly migrated to binary traces because traces do not support discarding a whole accumulated message set at commit time.

## Side-by-Side Comparison
| Dimension | PET | Binary traces (`nvmeib_utils_bin_traces.h`) |
|---|---|---|
| Primary API shape | `NVMEIBC_IO_PET_MSG(journal, "printf...", severity, args...)` with severity shortcuts (`_NORM/_WARN/_ERROR/_CRIT`) | `_NF/_ND/_NT/_NI/_NW/_NE(name, fmt, ...)`, `_N*_dmesg`, `_N*_SCOPE`, `_N*_to_user` |
| Record ownership | Per-entity journal object (`struct nvmeib_pet_journal`) tied to operation context | Global/per-CPU trace channels (longterm, shortterm, goodpath, metrics, eph, eternal) |
| Write timing | Append to private buffer during execution; flush/put on `nvmeib_pet_journal_commit()` | Event emitted at call site into configured trace channel |
| Persistence policy | Flush decision can depend on worst severity seen in journal (`worst_severity`) and controller policy (`minimal_severity`) | Channel choice is explicit at callsite (`NVMEIB_LOG_LONGTERM`, `..._GOODPATH`, `..._ETERNAL`, etc.); dmesg mirroring configurable per macro |
| Buffer model | Explicit `get_buffer/put_buffer/flush` controller API; can be no-memory/no-buffer safely | Trace backend/channel infra from `nvmeib_trace.h` + generated tracepoints; no per-operation private journal |
| Message identity | Message text embedded in dedicated ELF section, message id derived from section offset | Trace event identity is `name` (mapped to generated trace function symbol) |
| Format contract | `printf`-like format verified via `__attribute__((format(printf,...)))` helper; args serialized as typed compact variants | Custom trace format language (`@TAG`-style placeholders) used by binary tracer/event pipeline |
| Argument/type limits | PET message constructor supports 1..12 args (`NVMEIB_PET_MSG_1..12`); forbids float/double/char*/const char* | Kernel trace path explicitly works around zero-arg limitation and mentions LTTNG max-args pressure (10) |
| Zero-arg behavior | Public PET constructors are 1..12 args (no dedicated zero-arg message macro) | Wrapper has explicit logic/comments for no-arg traces (dummy char in kernel path) |
| Runtime disable path | If journal not activated (`iov_base == NULL`), PET writes are skipped; UM controller returns empty buffer by default | `DISABLE_ALL_TRACING` maps trace macros to dummy no-op definitions (`kr_incs_dummy_empty_traces.h`) |
| dmesg integration | No direct PET dmesg mirror in API; PET flush is controller-managed | Built-in `_N*_dmesg` and mirror macros (`nvmeib_dmesg_mirror_*`) |
| Decoder/tooling | PET has dedicated extractor/viewer (`nvmeib_pet_messages.py`) + [Kaitai](https://kaitai.io/) schema/parser | Binary trace ecosystem is integrated with trace channels/events and existing tracing pipeline |
| Struct/union rendering | Supports tagged `printf` args like `%...<struct X>` / `%...<union Y>` and resolves them through DWARF in the PET viewer | Uses dictionary-driven tokens (`@TOKEN`), including composite tokens and optional custom formatter functions (`fmt_func`) in pager tooling |
| Backend abstraction | Clear runtime backend API: `struct nvmeib_pet_base_controller` (`flush/get_buffer/put_buffer`) | No single explicit runtime backend API; behavior is largely produced by generated trace code and compile-time backend selection |
| Build-time overhead | No extra trace-preprocessor stage for PET macros in normal build; main cost is offline decode tooling when analyzing logs | Extra build pipeline in `scripts/trace.mk`: per-file first-pass preprocess + trace JSON + generated include + dictionary merge + `traces_ids` generation |
| Multi-threading model | Journal/entity buffer is designed as single-owner at a time; simulator path has a concurrent-access detector and treats concurrent add as a bug (`BUG_ON`) | Global trace API routes writers to per-CPU channel buffers (`get_cpu_var(...)` / `put_cpu_var(...)` in generated probes), enabling concurrent producers across CPUs |
| Batch discard semantics | Can accumulate many messages and discard the whole set if commit policy decides not to flush (for example below severity threshold) | No equivalent "discard the set" stage: events are emitted immediately, so suppression can only happen before emission |

## Key Architectural Differences
### 1. Data flow model
- PET:
  1. `NVMEIBC_IO_PET_MSG*` builds compact typed message + timestamp.
  2. Message appended into per-operation journal stream.
  3. On commit, journal flushes once and releases buffer.
- Binary traces:
  1. `_N*` wrapper chooses channel/severity macro.
  2. Event is emitted immediately to trace backend/channel.
  3. Optional simultaneous dmesg mirror depending on macro/config.

### 2. Filtering philosophy
- PET performs *post-factum* filtering by journal worst severity (`nvmeibc_io_pet_minimal_severity` in controller path).
- Binary traces perform *preselected routing* by macro/channel used at instrumentation point.

### 3. Context strategy
- PET stores the full per-entity timeline in one buffer, making reconstruction of a single failing I/O straightforward.
- Binary traces emphasize globally mergeable event streams across subsystems/channels.

## Struct Rendering and Build Costs
### PET struct/union printing (DWARF-based)
- PET format strings can include explicit type tags such as `<union ...>` / `<struct ...>` in `%` specifiers.
- `nvmeib_pet_messages.py` parses those tags and loads type definitions from DWARF (`DwarfRuntime`), including struct/union members and bitfields.
- Decoding is applied by `ArgDecoder` at view time, so output can show both raw numeric value and expanded typed content.
- If DWARF metadata or the tagged type is unavailable, PET still renders raw values (reduced readability, no field expansion).
- This debug-metadata-based introspection approach is not unique to PET: Linux kernel BPF tooling widely relies on BTF (BPF Type Format) for typed introspection/pretty-print workflows (including struct/union metadata), which is conceptually similar even though the format differs from DWARF.

### Trace-side equivalent for complex types
- Binary traces do not use DWARF for runtime/post-runtime formatting.
- They rely on dictionary tokens (`@TOKEN`) and can represent structured output through composite dictionary entries (`"composite"`), with optional custom formatters loaded from formatter libraries (`fmt_func` + `dlsym` path in pager tooling).
- Example usage exists in tracer unit tests (`@TEST_COMPOSITE` and custom formatter tokens).

### Cost profile (including compilation-time penalty)
- Binary traces add compile-time work in the module build:
- `scripts/trace.mk` runs a first preprocessing pass (`-D__FIRST_PASS__`) per source file.
- It generates per-file `.trace.json`, per-file generated headers (`*.c_gen_events.h`), merged dictionaries, and `traces_ids` artifacts.
- This pipeline increases incremental/full build time versus plain C compilation.
- PET logging macros compile as regular C macros without this trace-generation pipeline.
- PET's additional complexity/cost is mostly shifted to analysis tooling (dictionary extraction + optional DWARF decode), not to module compilation itself.

### Backend implications (why pre_processor generates multiple targets)
- PET backend integration is explicit and runtime-pluggable through `nvmeib_pet_base_controller` callbacks.
- Trace macros (`NVMEIB_LOG_*`) resolve to generated symbols from `gen_events.h`, and the generator emits backend-specific code paths (for example `_NVMEIB_TRACE_BACKEND_KERNEL` vs `_NVMEIB_TRACE_BACKEND_USER` in `gen_probes2.py`).
- Because traces do not expose one clean runtime backend interface, preprocessing/generation must materialize backend/channel specific targets ahead of compilation (per-module/per-file generated includes and merged artifacts in `scripts/trace.mk`).

### Argument evaluation semantics
- PET top-level macro (`NVMEIBC_IO_PET_MSG`) wraps formatting/serialization inside `if (nvmeib_pet_journal_is_activated(...))`, so `__VA_ARGS__` are not evaluated when the journal is inactive.
- PET lower-level add path (`nvmeib_pet_journal_add_msg(...)`) assumes the message object already exists; as noted in `nvmeib_pet_specification.h`, at that stage arguments are already evaluated.
- Traces: generated trace functions perform level checks *inside* the function body (for example `if (scope >= lvl)` in generated code), but function-call arguments are evaluated before entering that function. So expensive/side-effectful trace arguments still execute even when the runtime level check drops the event.
- With `DISABLE_ALL_TRACING`, dummy macros in `kr_incs_dummy_empty_traces.h` collapse many trace calls to empty expressions, which removes most argument evaluation.
- Practical rule for both systems: avoid side effects in log arguments; precompute only when needed or guard explicitly around expensive argument expressions.

### Multi-threading implications
- PET journal writes are intentionally single-owner for a given entity buffer. The PET docs call the entity buffer single-threaded, and simulator/debug paths include concurrent-access detection that fails on concurrent append attempts.
- This is a good fit for "one operation, one journal owner" workflows (for example an I/O context carrying its journal through its lifecycle).
- Trace instrumentation is system-wide and generated kernel probes use per-CPU channel state (`get_cpu_var(...)`/`put_cpu_var(...)`) for event emission, so multiple CPUs can emit concurrently with reduced cross-CPU contention.
- Operationally: PET gives strong per-operation locality but requires ownership discipline for each journal; traces scale naturally as many-callsite, many-thread producers in shared infrastructure.

## Practical Guidance for This Repository
Use PET when:
- You need end-to-end history of one I/O/operation instance.
- You want to keep good-path persistence low and flush mainly on problematic outcomes.
- You are instrumenting deep datapath execution where message locality per operation matters.

Use binary traces when:
- You need system-wide observability across components/channels.
- You need existing dmesg mirror paths, user-facing trace streams, or standard trace tooling.
- You need event families that are already part of generated trace dictionaries/pipelines.

Use both when:
- Binary trace gives coarse global breadcrumbs, and PET gives local deep context for the same failure path.

## Migration Patterns
### 1) Binary trace (`_N*`) -> PET
This is the feasible migration direction.

Typical current pattern:

```c
_NW(lock_timeout, "lock timeout, sgmnt=@SEG, rv=@RV", sgmnt, rv);
```

PET equivalent (same logical event, explicit severity):

```c
NVMEIBC_IO_PET_MSG_WARN(
    &o->journal,
    "lock timeout(sgmnt=%hhu, rv=%d)",
    sgmnt,
    rv);
```

Guidelines:
- Keep the semantic event name stable in the message prefix (for grepability and post-processing).
- Replace `@TAG` tracer placeholders with explicit `printf` fields and explicit casts where needed.
- Important: trace tags/tokens are not just formatting sugar. In traces they are first-class filter keys for `pager.py` (for example `@TOKEN = ...`, `has @TOKEN`, `fmt like ...`, `func = ...`, composite-token filters), and filtering is done on binary data.
- If you migrate a trace event to PET-only, you lose that token-level `pager.py` filtering path for that event. Keep at least coarse trace breadcrumbs for fields you still need to query quickly at scale.
- Pick PET severity intentionally (`_NORM/_WARN/_ERROR/_CRIT`) instead of relying on trace-level conventions.
- Ensure argument count/type fit PET constraints (1..12 args, no float/double/string pointers).
### 2) PET -> Binary traces: non-equivalent / not lossless
- PET is designed around deferred commit of a message set (journal) and may drop the whole set at commit time.
- Binary traces emit each event immediately, so they cannot represent "build a set, then discard all of it" as a native semantic.
- Because of that semantic mismatch, PET -> binary traces is not a true migration. At best it is a behavioral rewrite that changes persistence and noise characteristics.
- If trace visibility is needed for PET paths, keep traces only as coarse breadcrumbs and retain PET as the source of detailed per-entity history.

### 3) Per-CPU migration pattern (binary traces -> PET)
- For high-rate trace producers, PET journals can be provisioned per CPU and reused.
- Producer threads write into the current per-CPU journal; when full/committed, replace that full buffer with an empty one and continue.
- This preserves lock-light concurrent behavior while enabling PET commit/drop semantics on each per-CPU journal.
- Practical result: trace-style producer scaling with PET-style deferred persistence control.

### 4) Common Migration Traps
- Assuming bidirectional equivalence: PET -> traces loses commit-time discard semantics by design.
- Losing per-entity timeline: moving PET -> traces removes automatic single-buffer lifecycle reconstruction.
- Over-persisting good path: moving traces -> PET usually reduces persistence, but only if commit threshold/config is set appropriately.
- Losing filterability: moving traces -> PET can remove token-aware `pager.py` filtering workflows if equivalent trace tokens are not kept.
- Type drift: PET compile-time checks reject unsupported types early; preserve explicit integer widths/casts during translation.
- Message identity drift: PET identity is section offset-derived; binary tracer identity is event name-derived. Keep a stable naming scheme during mixed operation.

## Source References
- `clnt/nvmeibc_io_pet.h`
- `clnt/nvmeibc.lds`
- `common/pet/nvmeib_pet_specification.h`
- `clnt/nvmeibc_io_pet.c`
- `common/pet/nvmeib_pet_messages.py`
- `common/pet/pet.md`
- `common/nvmeib_utils_bin_traces.h`
- `common_public/nvmeib_trace.h`
- `common/compat/kr_incs_dummy_empty_traces.h`
- `clnt/block/unitest/nvmeibc_tracer_unitest.c`
- `tools/traces_post_processor/formatter.c`
- `scripts/trace.mk`
- `tools/pre_processor/tracer_pp.py`
- `tools/pre_processor/gen_probes2.py`
- `https://www.kernel.org/doc/html/latest/bpf/btf.html`
