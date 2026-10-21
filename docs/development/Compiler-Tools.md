# psc and psx

`psc` is the compilation driver; `psx` loads and executes an already compiled Plan. Frontend, validation, optimization, planning and execution remain independently callable library components. The driver owns file paths, JSON manifests, diagnostics, session retention and output publication. It follows the compiler-driver separation used by Clang: selecting a target determines which library stages run. It does not embed LLVM or use LLVM IR.

The drivers use physical Plan v5 and fixed invocation ABI 8. External operations provide static preparation, task lowering and fixed kernels through [operation module ABI 8](../kernel-architecture/Plugin-ABI.md). Input-only IR can run with an empty operation registry.

## Build and operation loading

`PHOTOSPIDER_BUILD_TOOLS=ON` (default) builds `psc` and `psx`. Build the two targets in an existing configured directory:

```sh
cmake --build build/kernel-dev --target psc psx -j 2
```

When `PHOTOSPIDER_BUILD_OPS=ON`, both tools register the built-in operation library before loading explicit `--load-ops` libraries. With ops OFF the registry starts empty. Each repeated `--load-ops PATH` loads a trusted in-process ABI 8 module, in command-line order. The registry is then frozen. Duplicate libraries, operation names or incompatible contracts fail. `--list-operations` prints loaded keys, traits versions and CPU/GPU capabilities. Libraries are not named or loaded by artifacts. `--resources` supplies immutable resource identities and bytes; per-run Tensor inputs belong in `psx --bindings`.

The tool-private JSON codec requires yyjson 0.13.0, commit `6447536015f3d600f3d65323b10976103b337ca7`, with its MIT license. Before configuring, follow the [yyjson preparation instructions](../../third_party/YYJSON.md) to place `yyjson.c`, `yyjson.h` and `LICENSE` in `third_party/yyjson/`. This directory is ignored by Git and absent from repository checkouts; CMake does not download the source. Set `PHOTOSPIDER_BUILD_TOOLS=OFF` to disable the tools and their yyjson requirement. yyjson is not a kernel header or exported dependency. No YAML input is supported.

## Compilation and versions

```sh
# No optimizer: DAG -> validate IR0 -> planner -> Plan.
psc input.json --planner static-tiles -o output.psxp
# Import becomes this session's IR0, then one optional optimizer.
psc input.psir -O optimize --emit ir -o optimized.psir
# Save both immutable versions.
psc input.json -O optimize --emit ir -o optimized.psir --save-ir 0=frontend.psir
# Repeated names remain repeated stages.
psc input.json -O optimize -O optimize --save-ir 1=once.psir -o twice.psxp
```

`psc` uses the default registered `dag@1` frontend for DAG JSON and imports PSIR directly. Frontend registration and name selection are available through the library `FrontendRegistry` and `Compiler::analyze`/`compile`; the CLI keeps its existing input identification.

Add `--load-ops` and `--resources` as required by the input contracts. `IR0` is the validated input or frontend result. Each successful optimizer invocation produces the next numbered version. No optimizer runs unless `-O` is present. The built-in `optimize@1` is an optional dependency-path deduplication example with no options. The planner performs the necessary deduplication itself on a private lowering copy. Repeated references to one Call ID designate one invocation; different Call IDs remain different calls. CFG loop activations stay independent.

All versions have the same `ValidatedIR` type. A transformation receives immutable input and returns a separate `IRProgram`, which is validated before publication. Content identity excludes provenance; a no-op can produce a new version with the same digest. Provenance records parent identity, producer/version and resolved options. IR import preserves provenance but restarts session numbering at zero. Raw and optimized `.psir` are equally valid planner inputs. The C++ `OptimizerRegistry` exposes registration before freeze. Each definition declares supported IR forms and an `option_schema` with typed defaults, optional inclusive numeric bounds, optional choices and descriptions. Registration checks the schema; invocation resolves defaults and checks all overrides before entering the callback. The compiler revalidates each returned program and checks that its public input/output names and schemas are unchanged. Structural validation does not prove numerical, control-flow or side-effect equivalence; transforms need semantic tests. Names are at most 1024 bytes, versions 128, options 1024 entries, keys 1024 and string values 8192. There is no dynamic optimizer ABI or numeric optimization level.

| Option | Behavior |
| --- | --- |
| `<input>` / `-` | One file or stdin; content identifies DAG JSON or PSIR. |
| `-x dag\|ir` | Assert input type; disagreement with content is an error. |
| `--emit plan\|ir\|ir:N` | Default `plan`; `ir` uses all requested passes; `ir:N` runs the first N. |
| `-o PATH` | Primary artifact, or `-` for stdout. Defaults to current-directory `stem.psxp` or `stem.irN.psir`; stdin requires explicit `-o`. |
| `--check` | Read, frontend if needed, and validate; no optimizer, planner or artifact output. Conflicting stage/output options fail. |
| `-O NAME` | Append one invocation, preserving order and repetitions. |
| `--opt-option N:key=value` | Typed override for the Nth invocation, starting at 1. |
| `--list-optimizers`, `--help-optimizer NAME` | Catalog with supported forms, or option types/defaults/ranges/choices and descriptions. |
| `--save-ir N=PATH` | Additional snapshot; repeatable, multiple destinations allowed. Cannot extend the selected endpoint or use stdout. |
| `--release-ir-after-save N` | Remove a retained version only after all its requested saves succeed; requires a requested save. Does not delete files. |
| `--print-pipeline` | Print planned actions without loading operations, invoking static hooks or compiling. Format detection may read the input and check JSON encoding and its format tag/version, but does not validate workflow fields. PSIR detection accepts leading semicolon comments. stdin requires `-x`. |

Sessions retain successful IR versions through completion by default. At most two snapshot exporters run concurrently with the sequential optimizer/planner pipeline. Each exporter owns an immutable copy; explicit release removes session retention, while active consumers retain their copy. The revision record keeps its content digest, parent identity, producer version and resolved options after IR storage is released. The driver drains exporters before destroying graph, registry or resource owners. Snapshots successfully published before a later failure remain available. The primary artifact is published only after all required stages and snapshots succeed. Failed transformations never modify earlier versions.

## Physical planning

These options are accepted only when generating a Plan:

| Option | Default / meaning |
| --- | --- |
| `--planner NAME` | Registry lookup, default `static-tiles`; the shipped backend expands demanded tiles in dependency order. Unknown names are argument errors; conflicts with `--check` and IR output targets. |
| `--backend cpu\|gpu` | `cpu`; selects the fixed CPU or GPU lowering target. Preparation must support that exact placement. |
| `--tile-extent N` | 128, positive power of two. |
| `--outputs a,b` | Select named outputs and their dependency closure. |
| `--requests FILE` | Select outputs/member tile intervals; conflicts with `--outputs`. |
| `--max-tasks N` | 4194304, positive physical task bound; metadata and incoming tiles have separate structural bounds. |
| `--max-dependency-edges N` | 16777216, positive static edge bound. |

`psc --list-planners` lists registered names, versions, descriptions and supported IR forms. `psc --help-planner NAME` displays one definition and the shared planning switches. These queries require no input file, load no operation libraries and invoke no planning callbacks. Unknown help names return argument errors. `--print-pipeline` resolves the selected definition and displays its name and version.

Library callers select a name with `PlanningOptions::planner = "static-tiles"`; both `Compiler::plan` and `compile` use this option. Pass a frozen `PlannerRegistry` as the third `Compiler` constructor argument to supply custom backends; omitted registries use the lazy default catalog. Registration rejects duplicate names, missing callbacks and invalid IR forms. `--backend` independently controls CPU/GPU placement.

A `PlannerDefinition` callback receives the original `ValidatedIR`, a callback-local `PlanBuilder` and the shared `PlanningOptions`. The builder exposes read-only linked declarations and editable physical Task/Tile/Call/Block tables. The compiler checks CFG, schemas, operation contracts, placement, normalized output selection and the emitted task count before publishing a Plan. Backends are responsible for semantic mapping. See the [planner construction contract](../kernel-architecture/Compiler-and-Execution.md#physical-plan). CLI catalogs contain compiled-in registrations; `--load-ops` registers operation libraries only.

Control flow and mandatory effect tasks survive output selection. Library `PlanningOptions::output_names` distinguishes all outputs (`nullopt`) from none (empty vector). Missing entries in the library `output_requests` map mean all tiles of a selected output. CLI request manifests explicitly select their named outputs. Runtime requests may only narrow saved coverage.

## Execution

```sh
psx output.psxp --load-ops ./operations.dylib \
  --resources resources.json --bindings bindings.json -o output.results
```

Plan import takes a non-null frozen `OperationRegistry` and explicit resources; it does not require a compiler or optimizer registry from the caller. The caller retains the operation environment for the Plan's lifetime. Import links saved operations/resources and validates saved physical tables, including derived adjacency, liveness and reservations. It invokes no optimizer, lowerer or planner. `ExecutionContext::prepare` checks the exact placement and kernel invocation contracts; execution uses `submit` and `join`. An unavailable placement fails before Run submission.

Each declared Plan input must be bound exactly once, including inputs that a selected output does not consume. Each static Tensor slot must have one complete raw-le file with exact dtype, full sample shape (batch axes followed by cell shape) and byte count. The binding reader stages the full raw file under the execution context's ResourceBudget. For `DirectTiles`, it copies required regions into separate aligned dense Host tiles with zero byte offset and the compiled capacity. For `DenseHost`, it retains one complete dense member allocation when any member tile is requested; the Plan's explicit COPY tasks materialize selected tiles during execution. Other incompatible physical layouts fail with `TypeMismatch`. `--check` imports and prepares the Plan, linking resources and fixed kernels, then stops before Run submission and export. Bindings may be omitted in check mode; if a manifest is supplied, every declaration and slot is still checked. Execution always requires all declared inputs.

| Option | Default / meaning |
| --- | --- |
| `--bindings FILE` | Required for execution if the Plan declares inputs; optional with `--check`. |
| `--outputs a,b` / `--requests FILE` | Output subset or member intervals contained in the compiled coverage. |
| `--threads N` | CPU workers; omitted uses the context default. Explicit range is 1..4096. |
| `--parallelism N` | Maximum concurrent tasks for this Run, range 1..4096; omitted uses the context worker count. Call retirement remains in Plan order. |
| `--max-live-bytes N` | 268435456; positive controlled Payload capacity. |
| `-o DIRECTORY` | Default current-directory `plan-stem.results`; stdin requires explicit output. Directory must be new. |

`psx` rejects optimization, backend and tile-extent options. Recompile for larger coverage or a different physical target. Output is a result manifest plus little-endian binary fragments with actual coverage. Missing regions are not filled. Logical window copying respects signed/zero strides; bit patterns are retained, including NaNs and signed zero. No persistent execution service or recovery state is created.

## Implementation boundaries

The executable entry points call separate driver libraries. Their parameter schemas and typed invocations are independent; runtime binding options are absent from `psc`, and compiler options are absent from `psx`.

| Source area | Responsibility |
| --- | --- |
| `src/tools/psc/` | Compiler invocation, action graph, IR session and DAG JSON frontend I/O. |
| `src/tools/psx/` | Executor invocation, Plan loading, runtime bindings and Results export. |
| `src/tools/common/` | Argument tokenization, diagnostics, process signals, file publication, JSON/schema helpers, operation loading, resources and request manifests. |
| `src/lib/compiler/frontend.cpp`, `frontend_registry.cpp`, `frontend/DAG/dag.cpp` | Frontend name dispatch, catalog, common IR binding and DAG lowering. |
| `src/lib/compiler/optimizer.cpp` | Transformation orchestration, output validation and public-interface preservation. |
| `src/lib/compiler/optimizer_registry.cpp`, `optimizers/deduplicate.cpp` | Catalog/option schemas and the separate optional example transform. |
| `src/lib/compiler/planner.cpp`, `planner_registry.cpp`, `planners/static_tiles.cpp` | Named backend dispatch, catalog, result validation and the registered static tile planner. |
| `src/lib/compiler/plan_linking.cpp`, `plan_validation.cpp`, `plan_selection.cpp` | Saved-Plan linking, physical-table verification and coverage narrowing without lowering. |

The common tool library owns neither a compiler session nor an execution context. Constructing a library `Compiler` for binding or planning does not initialize the default optimizer catalog; only an explicit optimization or catalog query needs it.

## Files, limits and diagnostics

An output cannot alias any input, resource, library or another output; checks include symlink resolution and hardlink equivalence. Ordinary outputs are fully encoded, written to a private sibling staging directory and atomically renamed into place. Existing ordinary output files may be replaced. Result directories are staged and published with an exclusive rename on macOS/Linux/Windows; unsupported platforms report an I/O error. Existing result directories are never reused. Publication is atomic visibility, not a crash-durability guarantee. stdin/stdout use binary artifact bytes; text diagnostics, verbose events and timing use stderr. Informational commands (`--help`, inventories and pipeline printing) use stdout and produce no artifact.

Input JSON and artifact text are bounded to 64 MiB. JSON is limited to 128 nesting levels and 4194304 DOM values; duplicate keys, unknown fields, invalid Unicode, wrong types and numeric overflow fail. Requests allow at most 65536 intervals and normalize them once per member. Each static resource is 1..64 MiB and must match its declared SHA-256. Binding staging reserves capacity before file reading; output staging uses budgeted fragments of at most 1 MiB, with at most 1048576 fragments per Tensor. The managed budget covers declared buffers, not process RSS, filesystem caches or plugin-private allocations.

Both tools preserve literal `=` characters inside option values, including attached short forms such as `-oalpha=beta`. A standalone `--` begins positional arguments unless consumed as an option value. Diagnostic selection respects the same boundaries even for malformed invocations. Repeated `--diagnostic-format` remains an argument error; its first occurrence determines how that error is rendered.

Common diagnostic and informational switches are `--diagnostic-format text|json`, `--color auto|always|never`, `--time-report`, `--version` and `-h`/`--help`. Compiler-specific help lists `-v`/`--verbose` for optimizer-stage and IR-release events, `--error-limit N` for collected compiler failures, and warning controls. `psc` accepts these controls; `psx` rejects `-v`/`--verbose`, `--error-limit` and all switches beginning with `-W` as unknown options (exit 2). JSON diagnostics are version-1 JSONL events with `severity`, stable `id`, `stage`, `message`, and known `source`, `ir_revision`, `status_code`, `node_id` or `input_id`. Parser positions appear in the message. Unknown locations are omitted. Non-UTF-8 system/path text is replaced only when rendering diagnostics. Semantic JSON strings, including output names and schema identities, are rejected when invalid; they are never silently renamed. Timing reports wall microseconds; overlapping export times are not additive.

`psc` stops at the first sequential error, drains already scheduled saves, and reports each collected failure up to `--error-limit` (default 20; 0 means unlimited). The exit code is determined by the highest-priority failure, independently of the reporting limit. If later compilation or main-artifact publication fails, `PS-SNAPSHOT-SAVED` notes identify snapshots that remain published. Concurrent failures select a stable category priority: interruption, internal error, I/O, argument error, then compile/runtime failure; equal-priority failures use IR revision, stage name and source path order. The sole available warning group, `unused-optimizer`, is enabled by default and reports passes beyond the selected `ir:N` endpoint; `-Wall`, `-Wextra`, `-Werror`, `-Wno-error`, `-W[no-]unused-optimizer` and `-W[no-]error=unused-optimizer` control it. Specific group settings override global settings; the last setting of equal specificity wins. Future warning groups are not silently accepted.

| Exit | Meaning |
| --- | --- |
| 0 | Success |
| 1 | Invalid input, compilation/execution/resource failure or promoted warning |
| 2 | Invalid/conflicting arguments |
| 3 | Internal error, including invalid IR or Plan produced by a compiler callback |
| 4 | File/stdout I/O failure |
| 130 | SIGINT/cooperative interruption |

SIGINT is observed between compilation stages, during file operations, and by an execution cancellation monitor. In-process operation callbacks must cooperate; the tools do not forcibly terminate them.

See [CLI file formats](Compiler-Tool-Formats.md), [compiler architecture](../kernel-architecture/Compiler-and-Execution.md) and [version axes](Compiler-Version-Contract.md). CLI regression tests invoke both binaries against an external resource-consuming ABI fixture; they cover ops OFF/ON registration, raw/optimized IR round trips, exact numeric results, partial coverage, path conflicts and malformed inputs. Platform claims require corresponding platform runs.
