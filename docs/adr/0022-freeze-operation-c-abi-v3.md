# ADR 0022: Freeze One Prefix-Negotiated C ABI for All Operations

- Status: Proposed
- Reader mirror: [Chinese](zh/0022-freeze-operation-c-abi-v3.zh.md)
- Related: [ADR 0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md), [ADR 0014](0014-compiler-document-and-plan-versions-are-independent.md), [ADR 0015](0015-breaking-product-boundary-scope-reset.md), [ADR 0016](0016-workflow-inputs-and-execution-bindings.md)

## 1. Core summary (TL;DR)

Built-in operations register through an unfrozen C++ registration interface, while external modules use Result operation C ABI 2, which accepts only exact structure sizes. ABI 2 already resolves metadata statically and propagates host-managed resources, but it lacks persistent preparation state, preparation-derived extra workspace, a generic immutable resource handle, temporary storage creation and the C++ declarative inference rules. Photospider will freeze one versioned C ABI, v3, as the only binary contract between the host and every operation module; a header-only C++ SDK compiles into each module, and built-in operations use the same import path as external modules. Structures negotiate by stable prefix, services are separately versioned tables, and the frozen surface covers only what the current built-in operations need plus a reserved slot for declared kernel capabilities.

## 2. Mental model and intuition

```text
   built-in ops library            external module (.so/.dylib)
   (static or shared)              (dynamically loaded)
          |                                  |
   module-specific getter            exported v3 entry point
   (unique symbol or fn pointer)             |
          \                                  /
           +---------- v3 module table -----+
                           |
             host import: whole-table validation
             (prefix sizes, required capabilities,
              unknown required content -> reject)
                 /                      \
          all records valid          any record invalid
                 |                          |
     copy into private registry      reject module, publish nothing,
     candidate, swap atomically      run destroy hook, keep error location
                 |
   compiler: pure prepare callbacks (metadata + static parameters only)
                 |
   executor: start/poll callbacks with borrowed v3 service tables
                 |
   host validates publication, relations, budgets, cancellation
```

The host owns compilation, scheduling, dependency tracking, budgets, cancellation, caching, publication and module lifetime. An operation module owns its formulas, parameter definitions, metadata inference, input requests and dependency declarations. The host may keep private C++ registry records; those records are not part of the frozen contract.

Inside a module, C++ is allowed. STL objects, exceptions and unstable kernel C++ symbols never cross the boundary. The SDK supplies builders, RAII wrappers, typed continuations, window wrappers and common inference rules, all compiled into the module that uses them.

## 3. Formal contracts and APIs

The concrete v3 header is a later design step. The shape below shows the negotiation rules; field and type names are illustrative.

```c
/* Illustrative shape; the v3 header defines the final names. */
typedef struct ps_op_module_v3 {
  uint32_t struct_size;   /* bytes the module filled; host reads min(size, sizeof) */
  uint32_t abi_major;     /* 3; a different major is rejected */
  uint32_t abi_minor;     /* informational; capability checks decide acceptance */
  uint32_t operation_count;
  uint32_t operation_stride;          /* extensible record array */
  const struct ps_op_operation_v3 *operations;
  void (*destroy)(const struct ps_op_module_v3 *);
} ps_op_module_v3;

typedef struct ps_op_service_query_v3 {
  uint32_t struct_size;
  /* Returns a separately versioned service table, or NULL if unsupported.
     Called outside per-sample loops; the table lives for the stated scope. */
  const void *(*get_service)(const void *host, const char *service_id,
                             uint32_t major, uint32_t minimum_minor);
} ps_op_service_query_v3;
```

### D1. One frozen binary contract

The versioned C ABI is the only frozen binary contract for operation modules. The C++ SDK is source-level support and compiles on the module side. Allocation and destruction responsibility is explicit for every handle, and the host keeps a module's owner alive while any of its code or tables may still run. The compatibility policy for applications that call `Compiler`, `ExecutionContext` and other C++ APIs is managed separately; this decision does not freeze the kernel C++ ABI.

### D2. Capabilities the v3 surface provides

1. **Inference.** A pure prepare callback returns a resolved output contract. The SDK supplies common rules for shape, dtype, halo and spatial factor. The current `OperationShapeRule`, `OperationDtypeRule`, `OperationSemanticRule` and similar enums are not copied into the C ABI. Preparation depends only on input metadata and static parameters; it does not read runtime samples, execute upstream nodes or perform file I/O.
2. **Schema timing.** Preparation may derive a concrete schema from input metadata, including constrained schema inheritance. The structure is fixed before execution. At run time an operation may determine declared dynamic row counts, coverage and completion. Adding fields or changing dtype after reading samples is outside v3.
3. **Memory and state.** The host provides storage that persists across polls. Three lifetimes are distinct: static preparation state, mutable state for one execution, and published immutable data. Execution workspace is charged through host resource admission. The existing boundary between preparation storage and execution budgets stays as it is.
4. **Temporary I/O.** Services create, extend, read and write temporary storage. The callback returns a Need, the host performs the I/O, and the next poll receives the reply. Storage backends and paths are host implementation details.
5. **Dependencies and regions.** The host exposes compact managed relations and Footprint construction, covering current Gather, merge and projection uses. Dependency precision, holes and cost characteristics are preserved. An arbitrary plugin relation callback is not the default representation.
6. **Resources.** A generic immutable resource handle covers current resource uses first; model and engine policy follows D8. The host may manage facts such as `semantic_key` and association internally rather than exposing every field of the C++ query.

Callback errors remain structured with sticky failure, and cancellation, stale-work and publication rules continue to apply. CPU/GPU lanes, windows, views, joint execution, blocks and checkpoints keep their current behavior. Pure utility and algorithm code belongs in the SDK or operation helper libraries; migration does not move it into the kernel.

### D3. Compatibility policy

1. Within one major version, an older module runs on a newer host. A newer module runs on an older host when every capability it actually requires is present; platform and external runtime conditions still apply.
2. Each structure has a stable minimum prefix. The host reads only fields that are completely present, and every field that can be absent has a defined default. Extensible record arrays carry count and stride. Extensible substructures are referenced by pointer. Embedded fixed value types keep their layout. Output structures respect the caller's capacity.
3. A small base entry point returns independently versioned service tables. A table pointer stays valid for its documented scope. Service lookup happens outside per-sample loops. Changing an existing exact-sized service contract requires a new service version.
4. Unknown required content is rejected. Unknown optional content is ignored only where the contract explicitly allows it. The host never guesses an unknown dtype, required schema semantic, access right or publication mode. Interface support and actual device or resource availability are checked separately.
5. Changing an existing layout, signature, meaning, ownership or threading rule raises the corresponding major version. The minor version is not the sole capability test. Operation behavior versions are managed separately from ABI versions.
6. Moving from v2 to v3 requires rebuilding modules; no v2 compatibility layer is added. The host validates a submitted module table as a whole and admits it as a whole; it never skips unsupported parts silently. Failure cleanup, module lifetime and error location reporting are preserved.
7. Reserved fields are not the primary extension mechanism. Semantic identity is computed from canonical field encodings, never from raw C structure bytes, padding, pointers or physical strides. An absent field and an explicit default with the same meaning produce the same identity.

### D4. One import path for built-in and external operations

Built-in operations ship as an independent library that may be linked statically. External modules are usually loaded dynamically. The two obtain their module table differently and then share import, validation, registration and execution. A static module exposes a uniquely named getter or a function pointer; the host does not require all static modules to export one identically named symbol.

### D5. Result ports only

Every workflow operation port carries a Result. `Value` is numeric storage inside Results and implementations; it does not become a second Value or planar execution interface. Result-only ports do not imply a single output, Whole execution, a complete bundle or forced copying: named outputs, partial requests, legal views and publication policies follow each operation's contract. Modules execute inside the trusted host process; ABI checks provide no isolation.

### D6. Element types

The pipeline element types are exactly UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64. An unknown dtype is rejected and never reinterpreted as bytes or another type. Existing encodings are not reused for new meanings, and unimplemented types are not pre-declared. ABI lengths, capacities, offsets and handles continue to use `uint32_t` and `uint64_t`, which are distinct from tensor dtypes. Private complex arithmetic inside an operation or fp16/int8 inside a model does not extend the public pipeline; any public conversion is explicit.

### D7. Time-independent kernel

The host application owns time representation, exact time comparison, frame selection, windows, weights, execution order, history state and history invalidation. The kernel keeps explicit multi-input operations, ordinary batch axes and explicit `state_in`/`state_out` Results; "single image" does not mean "only one image input". Continuations, temporary I/O, blocks, checkpoints and caches stay generic. A plan whose schemas are compatible can run with different immutable input bindings, as defined in ADR 0016. When a host computes weights from rational time and converts them to Float64, that rounding boundary is explicit; it is not equivalent to a single rounding of the original rational expression. Sequence and time policy operations move to host-side libraries or workflow builders.

### D8. External resources and engines

1. A resource is an explicit, immutable, content-addressed object or closed set. It has an owner, may be sharded or shared, and its hashed content stays stable while in use. A mutable path is never an execution identity.
2. The kernel provides generic carriage. Trusted plugins or host adapters interpret models and engines; the kernel has no built-in inference framework and no dedicated infer or decode service. Data, resource bytes, executable implementations and derived sessions are separate.
3. A semantic resource is fixed with the compiled node. Changing a model, weights or configuration requires preparing again, while underlying immutable resources may be shared. Static preparation does not download or execute a model to infer shapes; heavy parsing and initialization happen in an explicit phase.
4. Resource content, algorithm instance and execution implementation have separate identities. Result reuse covers the configuration and backend that actually affect output. Paths and provenance that do not affect computation do not enter the mathematical identity.
5. Bounded, explicitly managed read-only caches and sessions may be reused. Mutable instances have explicit concurrency or exclusivity rules. Resource, configuration or device changes invalidate dependent state. A model's hidden history state passes only through explicit Results.
6. Native wrappers keep their NUM/FMT precision contracts. An external instance is validated on its own; it is not automatically strict and does not inherit a uniform error bound. The default demand is Whole; local halo use requires proof. Tiled inference whose output differs is a separate instance or algorithm version.
7. Cross-request result caching is enabled by default only for instances validated as deterministic under the pure computation and identity contract. Measured-consistent and stochastic instances default to off. A fixed seed does not prove determinism. Weight and session reuse is separate from result caching.
8. Each instance declares what the host meters, what the engine can meter, which upper bounds are reliable and what remains uncovered; the host decides what to accept. Resources used by an uninterruptible call are released after that call ends, and cancellation forbids illegal publication. Isolation and forced termination stay outside the kernel, consistent with ADR 0015. Thread pools and concurrency capacity are declared explicitly.

### D9. Image representation

Canonical images follow the accepted [FMT common contract](../built-in_ops/02-format-color/op_specs/FMT_common_contract.md): a generic tensor in a Result with metadata, straight color, alpha as an independent plane in the same tensor, and mandatory planar storage. Operations may use premultiplied working values internally and restore straight values before publishing a canonical image.

1. The generic image interface carries Float32 and Float64 equally without implicit narrowing. Each operation or backend declares its supported set and fails explicitly otherwise. Integer encodings stay under the existing FMT and explicit-encoding rules.
2. Logical axis roles and legal permutations follow the existing tensor and spatial model. Neither HWC nor CHW is mandatory. `height_axis`, `width_axis` and `channel_axis` name logical cell dimensions; they do not grant arbitrary physical layouts. Role count and legality, and agreement between metadata and actual structure, are validated.
3. Color groups and alpha indices on the channel axis place channels and alpha inside one tensor. Alpha need not be last, and images without alpha are valid. Structural completeness of metadata does not make an operation read every color group or alpha sample.
4. The FMT-04 premultiplied boundary payload remains a generic Result payload. It is not a canonical image, and ordinary image consumers reject it. The kernel may still carry general premultiplied numeric data.
5. A canonical image actually satisfies planar storage, alongside generic affine views with negative or zero strides. Raw tensors keep their legal layouts. A layout change constructs a legal view or materializes; a spatial flag, metadata or an HWC shape alone never claims physical planar storage.
6. Existing image and ColorArray consumers, schemas, keys and formulas migrate explicitly to the tensor-metadata, straight and planar target, without old-and-new dual interfaces. A pure ABI bridge preserves behavior; deliberate changes that adopt the new image contract are listed separately and validated on their own.

### D10. Region containers

The C ABI and the kernel's `Region` and `Footprint` containers have separate layouts. v3 does not freeze the C++ container layout, and container optimizations follow later measurement.

### D11. Declared kernel capabilities

Capabilities have two layers: a fine-grained computation IR and coarse named primitives built on it. The long-term target is compiling capability compositions to executable binaries. v3 reserves a versioned extension position in the operation declaration and defines no capability format.

An operation may provide a capability implementation path, a callback path or both. A callback is optional: when one exists the host may use it as declared, and an operation that declares a capability need not also provide a complete reference callback. If no capability implementation is available to the host and no executable callback exists, the host reports the operation as unsupported; it never succeeds silently or assumes a CPU reference implementation. The negotiation or compilation stage that rejects it, and the error representation, are left to the interface design. Independent numerical validation and native execution acceptance remain required regardless of callbacks.

The current `data_movement` field and `OperationSemanticRule` enum are not frozen in their present form. The canonical encoding of a capability declaration enters semantic identity; pointers, function addresses and opaque state do not. Optimizations that replace one implementation path with another preserve source metadata assertions, intermediate validation, forced materialization or views, and the first error coordinate and failure order. Numeric rewrites such as merging rounded conversions, replacing division by reciprocal multiplication or introducing FMA require separate proof of equivalence.

### Acceptance

- **Behavior parity.** Representative migrations cover identity and channel extraction/assembly (schema, layout, views, ownership); Gaussian (preparation, workspace, cross-poll coefficients, regions, native GPU); expression, Bezier and sequence operations (static input-set reduction); STMap (Gather, projection, merge, memory); FFT, component and statistics factories (temporary I/O, dynamic fields); joint, block and checkpoint; and an independently installed PixelOE. After the representative set, every in-scope operation migrates.
- **Compatibility.** Fixtures compiled against separate older headers exercise old-module/new-host, new-module/old-host, required and optional capabilities, multi-element array stride, short output buffers, unknown extensions, failure cleanup and module lifetime. A small `struct_size` forged with the current header is insufficient.
- **Images.** Float32/Float64, HWC/CHW logical orders, non-final alpha, no alpha, multiple groups, hidden RGB, boundary-payload rejection and conversion, planar physical access, ROI and channel dependencies, legal views and materialization, deliberate contract changes and native backend support limits.
- **External engines.** Wrapper semantics, comparison of the frozen artifact with an independent direct engine run, and task-level effect; plus ownership, concurrency, cancellation, errors, resources and cache identity. Mocks or offline oracles do not replace real inference, decoding or quality validation.

## 4. Non-goals and explicit boundaries

- v3 is a contract decision. Package 0.33.0 still implements Result operation C ABI 2 and the unfrozen C++ `OperationDefinition` registration path described in [Plugin ABI](../kernel-architecture/Plugin-ABI.md).
- The kernel C++ API used by applications is not frozen by this decision.
- No v2 compatibility layer, shim or dual registration path is provided.
- No new element types: Float16, Int32, UInt32, UInt64 and native complex stay outside the pipeline.
- No native Sequence or RationalTime type, time-indexed frame service, frame-rate or missing-frame policy, playback, seek, replay or implicit cross-call time state in the kernel.
- No built-in inference framework, automatic license judgment or artifact approval product. License records stay separate from resource and execution identity.
- No sandbox, signature check, isolated worker or forced termination; modules remain trusted in-process code.
- No capability format, first capability family, flat-box or bitmap Region container, or performance promise is defined here.
- Canonical straight and planar images are not reopened as an open product choice, and the current Float32 HWC premultiplied image path is not the target contract.

## 5. Consequences

Every module, built-in or external, must be rebuilt against the v3 header; a v2 module is rejected at load. Built-in operations stop using the C++ registration interface and kernel-private headers, so helpers they need move into the SDK or operation helper libraries. The host keeps the cost of maintaining defaults for every absent field and of validating whole module tables before publication.

Prefix negotiation lets a structure grow without rejecting older modules, but only additions with defined defaults are minor changes. Any change to an existing field's meaning, ownership or threading rule is a major version, and the host rejects a module that requires content it does not understand. A module that requires a missing service or device capability is rejected or reported as unsupported rather than run with guessed behavior.

Moving built-in operations onto C callbacks adds a function-pointer call per block or tile. Per-row window, per-coordinate read and per-element cancellation or work callbacks are the expensive paths; they need batched forms before migration, and the C-bridge overhead is measured against direct C++ on Gaussian and PixelOE. The existing premultiplied image operations change their public contract when they adopt straight images; those changes are reported and validated separately from behavior-preserving ABI migration.

Host applications that need temporal behavior implement frame selection, weighting and state threading themselves, using explicit inputs and `state_in`/`state_out` Results. Operations that wrap models or engines carry their own validation, metering declarations and determinism evidence before result caching is enabled for them.
