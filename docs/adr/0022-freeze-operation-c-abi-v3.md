# ADR 0022: Freeze One Prefix-Negotiated C ABI for All Operations

- Status: Proposed
- Implementation boundary (2026-10-20): This remains a historical Proposed design, not the current binary contract. Package 0.39 implements exact-size operation module ABI 8, fixed invocation ABI 8 and native kernel service ABI 2. Its static lowering and fixed completion model replaces the staged continuation surface below; prefix negotiation and the proposed universal import policy are not implied. See [Plugin ABI](../kernel-architecture/Plugin-ABI.md).
- Reader mirror: [Chinese](zh/0022-freeze-operation-c-abi-v3.zh.md)
- Related: [ADR 0012](0012-operation-plugins-use-a-separately-versioned-pure-c-abi.md), [ADR 0014](0014-compiler-document-and-plan-versions-are-independent.md), [ADR 0015](0015-breaking-product-boundary-scope-reset.md), [ADR 0016](0016-workflow-inputs-and-execution-bindings.md), [ADR 0023](0023-track-dependencies-between-fixed-tiles.md)

## 1. Core summary (TL;DR)

Package 0.36.0 registers built-in operations through an unfrozen C++ registry, while external modules use Result operation C ABI 6 (OperationTraits 28), which accepts only exact structure sizes. ABI 6 already resolves metadata statically, selects immutable resources by explicit identity, requests inputs through tile Needs, and registers validators and diagnostics, but a C operation has no persistent preparation state, no stable-prefix negotiation and no base-entry-point `get_service` mechanism for querying and negotiating service tables. CPU parallel, CPU tile and native GPU services already have independent ABI versions. Photospider will freeze one versioned C ABI as the only binary contract between the host and every operation module; a header-only C++ SDK compiles into each module, built-in operations use the same import path as external modules, structures negotiate by stable prefix, services are separately versioned tables, and the frozen surface covers only what the retained built-in operations need.

## 2. Mental model and intuition

```text
   built-in ops library            external module (.so/.dylib)
   (static or shared)              (dynamically loaded)
          |                                  |
   module-specific getter            exported frozen-ABI entry point
   (unique symbol or fn pointer)             |
          \                                  /
           +------- frozen module table ----+
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
   executor: start/poll callbacks with borrowed versioned service tables
                 |
   host validates publication, tile records, budgets, cancellation
```

The host owns compilation, scheduling, dependency tracking, budgets, cancellation, caching, publication and module lifetime. An operation module owns its formulas, parameter definitions, metadata inference, input requests and dependency declarations. The host may keep private C++ registry records; those records are not part of the frozen contract.

Inside a module, C++ is allowed. STL objects, exceptions and unstable kernel C++ symbols never cross the boundary. The SDK supplies builders, RAII wrappers, typed continuations, window wrappers and common inference rules, all compiled into the module that uses them.

## 3. Formal contracts and APIs

The concrete header of the frozen ABI is a later design step, and its version number is assigned when it is frozen. The shape below shows only the negotiation rules; it is a target, not a declaration in any current header, and its field and type names are illustrative. The current exact-size ABI 8 declarations are in `photospider/plugin/operation_module_api.h` and are described in [Plugin ABI](../kernel-architecture/Plugin-ABI.md).

```c
/* Illustrative target shape; not declared in any current header. */
typedef struct ps_op_module {
  uint32_t struct_size;   /* bytes the module filled; host reads min(size, sizeof) */
  uint32_t abi_major;     /* major of the frozen ABI; a different major is rejected */
  uint32_t abi_minor;     /* informational; capability checks decide acceptance */
  uint32_t operation_count;
  uint32_t operation_stride;          /* extensible record array */
  const struct ps_op_operation *operations;
  void (*destroy)(const struct ps_op_module *);
} ps_op_module;

typedef struct ps_op_service_query {
  uint32_t struct_size;
  /* Returns a separately versioned service table, or NULL if unsupported.
     Called outside per-sample loops; the table lives for the stated scope. */
  const void *(*get_service)(const void *host, const char *service_id,
                             uint32_t major, uint32_t minimum_minor);
} ps_op_service_query;
```

### D1. One frozen binary contract

The versioned C ABI is the only frozen binary contract for operation modules. The C++ SDK is source-level support and compiles on the module side. Allocation and destruction responsibility is explicit for every handle, and the host keeps a module's owner alive while any of its code or tables may still run. The compatibility policy for applications that call `Compiler`, `ExecutionContext` and other C++ APIs is managed separately; this decision does not freeze the kernel C++ ABI.

The module-side header-only SDK does not exist yet. `Photospider::operation_sdk` provides C++ helpers against the current headers, and `Photospider::ops_sdk` is an operations-domain library for color descriptions, codecs, LUT contracts and numeric diagnostics; neither is the module-side ABI SDK this decision requires.

### D2. Capabilities the frozen surface provides

1. **Inference.** A pure prepare callback returns a resolved output contract, including the input projection derived from static parameters (which inputs the computation actually needs) and an upper bound on additional execution workspace derived during preparation. The SDK supplies common rules for shape, dtype, halo and spatial factor; these rule vocabularies stay in the SDK and are not copied into the C ABI. Preparation depends only on input metadata and static parameters; it does not read runtime samples, execute upstream nodes or perform file I/O. In package 0.36.0, the C++ `OperationPreparation` already returns a derived projection and `additional_workspace_bytes`; the ABI 6 `resolve_metadata` callback returns output schemas, resource identities and replacement shared-stage declarations, including stage projections and workspace bounds.
2. **Schema timing.** Preparation may derive a concrete schema from input metadata, including constrained schema inheritance. The structure is fixed before execution. At run time only production coverage and completion change; data-dependent active counts are ordinary payload in static-capacity Tensor members. Adding fields or changing dtype after reading samples is outside the frozen ABI.
3. **Memory and state.** The host provides storage that persists across polls. Three lifetimes are distinct: static preparation state, mutable state for one execution, and published immutable data. Execution workspace is charged through host resource admission. The existing boundary between preparation storage and execution budgets stays as it is. In package 0.36.0, C++ operations keep static preparation state in `PreparedOperation::state()`; a C operation has per-execution continuation state (`state_bytes`) and no static preparation state.
4. **Dependencies.** Needs name an input port, member slot and compact tile intervals, or explicitly request descriptor-only access. Complete coverage is an interval selection. Static structure comes from the plan. Empty payload sets grant nothing. Frozen evidence records actual Needs without widening access; coordinate-to-tile conversion belongs to SDK helpers.
5. **Resources.** An immutable resource is selected by explicit identity (byte length and SHA-256) from the `ResourceSet` supplied to compilation, and callbacks receive borrowed views of exactly the selected resources; there is no ambient lookup. ABI 6 already provides this through the metadata sink's `set_resources`; the frozen ABI keeps it, and model and engine policy follows D8. The host may manage facts such as `semantic_key` and association internally rather than exposing every field of the C++ query.
6. **Validation and diagnostics.** Ports bind explicit validators, operations declare bounded diagnostic reports, and callbacks attach opaque domain failure codes. ABI 6 already provides these; the frozen ABI keeps them.

Callback errors remain structured with sticky failure, and cancellation, stale-work and publication rules continue to apply. CPU/GPU lanes, windows, views, blocks and checkpoints keep their current behavior. Pure utility and algorithm code belongs in the SDK or operation helper libraries; migration does not move it into the kernel.

### D3. Compatibility policy

1. Within one major version, an older module runs on a newer host. A newer module runs on an older host when every capability it actually requires is present; platform and external runtime conditions still apply.
2. Each structure has a stable minimum prefix. The host reads only fields that are completely present, and every field that can be absent has a defined default. Extensible record arrays carry count and stride. Extensible substructures are referenced by pointer. Embedded fixed value types keep their layout. Output structures respect the caller's capacity.
3. A small base entry point returns independently versioned service tables. A table pointer stays valid for its documented scope. Service lookup happens outside per-sample loops. Changing an existing exact-sized service contract requires a new service version.
4. Unknown required content is rejected. Unknown optional content is ignored only where the contract explicitly allows it. The host never guesses an unknown dtype, required schema semantic, access right or publication mode. Interface support and actual device or resource availability are checked separately.
5. Changing an existing layout, signature, meaning, ownership or threading rule raises the corresponding major version. The minor version is not the sole capability test. Operation behavior versions are managed separately from ABI versions.
6. Moving from Result operation C ABI 6 to the frozen ABI requires rebuilding modules; no ABI 6 compatibility layer is added. The host validates a submitted module table as a whole and admits it as a whole; it never skips unsupported parts silently. Failure cleanup, module lifetime and error location reporting are preserved.
7. Reserved fields are not the primary extension mechanism. Semantic identity is computed from canonical field encodings, never from raw C structure bytes, padding, pointers or physical strides. An absent field and an explicit default with the same meaning produce the same identity.

### D4. One import path for built-in and external operations

Built-in operations ship as an independent library that may be linked statically. External modules are usually loaded dynamically. The two obtain their module table differently and then share import, validation, registration and execution. A static module exposes a uniquely named getter or a function pointer; the host does not require all static modules to export one identically named symbol. In package 0.36.0, built-in operations still register through `OperationRegistry::register_operation`, and only external modules use the ABI 6 getter.

### D5. Result ports only

Every workflow operation port carries a Result. `Tensor` is numeric storage inside Results and implementations; it does not become a second Tensor or planar execution interface. Result-only ports do not imply a single output, a complete bundle or forced copying: named outputs, partial requests, legal views and publication policies follow each operation's contract. Modules execute inside the trusted host process; ABI checks provide no isolation.

### D6. Element types

The pipeline element types are exactly UInt8, Int8, UInt16, Int16, Int64, Float32 and Float64. An unknown dtype is rejected and never reinterpreted as bytes or another type. Existing encodings are not reused for new meanings, and unimplemented types are not pre-declared. ABI lengths, capacities, offsets and handles continue to use `uint32_t` and `uint64_t`, which are distinct from tensor dtypes. Private complex arithmetic inside an operation or fp16/int8 inside a model does not extend the public pipeline; any public conversion is explicit.

### D7. Time-independent kernel

The host application owns time representation, exact time comparison, frame selection, windows, weights, execution order, history state and history invalidation. The kernel keeps explicit multi-input operations, ordinary batch axes and explicit `state_in`/`state_out` Results; "single image" does not mean "only one image input". Continuations, blocks, checkpoints and caches stay generic. A plan whose schemas are compatible can run with different immutable input bindings, as defined in ADR 0016. When a host computes weights from rational time and converts them to Float64, that rounding boundary is explicit; it is not equivalent to a single rounding of the original rational expression. Sequence and time policy operations move to host-side libraries or workflow builders.

### D8. External resources and engines

1. A resource is an explicit, immutable, content-addressed object or closed set. It has an owner, may be sharded or shared, and its hashed content stays stable while in use. A mutable path is never an execution identity.
2. The kernel provides generic carriage. Trusted plugins or host adapters interpret models and engines; the kernel has no built-in inference framework and no dedicated infer or decode service. Data, resource bytes, executable implementations and derived sessions are separate.
3. A semantic resource is fixed with the compiled node. Changing a model, weights or configuration requires preparing again, while underlying immutable resources may be shared. Static preparation does not download or execute a model to infer shapes; heavy parsing and initialization happen in an explicit phase.
4. Resource content, algorithm instance and execution implementation have separate identities. Result reuse covers the configuration and backend that actually affect output. Paths and provenance that do not affect computation do not enter the mathematical identity.
5. Bounded, explicitly managed read-only caches and sessions may be reused. Mutable instances have explicit concurrency or exclusivity rules. Resource, configuration or device changes invalidate dependent state. A model's hidden history state passes only through explicit Results.
6. Native wrappers keep their NUM/FMT precision contracts. An external instance is validated on its own; it is not automatically strict and does not inherit a uniform error bound. The default algorithm acquires complete input coverage through ordinary tile Needs and a Run-shared stage; local halo use requires proof. Tiled inference whose output differs is a separate instance or algorithm version.
7. Cross-request result caching is enabled by default only for instances validated as deterministic under the pure computation and identity contract. Measured-consistent and stochastic instances default to off. A fixed seed does not prove determinism. Weight and session reuse is separate from result caching.
8. Each instance declares what the host meters, what the engine can meter, which upper bounds are reliable and what remains uncovered; the host decides what to accept. Resources used by an uninterruptible call are released after that call ends, and cancellation forbids illegal publication. Isolation and forced termination stay outside the kernel, consistent with ADR 0015. Thread pools and concurrency capacity are declared explicitly.

### D9. Image representation

Images are static Tensor members with explicit structural axes. Host-created backing defaults to tiled planar; ordinary image operations default to straight alpha through their implementation, parameters and authoring helpers. These defaults require no conformance declaration, schema label or automatic validator. The kernel does not infer RGBA, inspect alpha association, or associate/unassociate samples.

Logical HWC/CHW and other legal axis orders remain independent of physical mapping. Negative and zero strides, broadcast, multiple fragments and external backing remain legal after address, ownership and synchronization checks. Default storage does not require a full-canvas reservation or reject a compatible view.

Explicit alpha conversions and association options still select real calculations. Alpha-aware resampling performs necessary weighting inside the operation. Domain channel groups remain explicit ops configuration, never kernel schema or storage groups. Float32/Float64 support depends on the operation/backend; no implicit narrowing is introduced.

### D10. Region containers

The C ABI and the kernel's `Region` and `Footprint` containers have separate layouts. The frozen ABI does not freeze the C++ container layout, and container optimizations follow later measurement.

### D11. Operation-specific metadata rules

Operation-specific declarations in the current tables, including SDK-owned inference rules, are not frozen in their present form. Operation-specific inference moves into each module's prepare callback and SDK helpers, and the kernel validates the resolved contract. The frozen ABI defines no declared kernel capability format and reserves no extension slot for one.

### Acceptance

Package 0.36.0 has not completed acceptance of the frozen ABI as a whole. The criteria below retain the capabilities already delivered and define the remaining requirements for accepting a frozen ABI implementation.

- **Behavior parity.** Representative migrations cover identity and channel extraction/assembly (schema, layout, views, ownership); expression, Bezier and sequence operations (static input-set reduction); LUT3D bake (static Tensor reports, gated Needs); STMap (two-stage tile Needs, tile records, memory); block and checkpoint (dependency restoration); and an independently installed PixelOE (native GPU). After the representative set, every retained operation in 01-numeric, 02-format-color, STMap and PixelOE migrates. Today the retained 01-numeric, 02-format-color and STMap operations are delivered on CPU through the C++ registry; the independently installed PixelOE consumer and native GPU coverage remain deferred.
- **Compatibility.** Fixtures compiled against separate older headers exercise old-module/new-host, new-module/old-host, required and optional capabilities, multi-element array stride, short output buffers, unknown extensions, failure cleanup and module lifetime. A small `struct_size` forged with the current header is insufficient. These fixtures require a frozen header and therefore do not exist yet.
- **External consumer.** An independently built and installed consumer, such as PixelOE, loads the frozen ABI. This validation runs only after kernel development completes; no installed-consumer test runs before that stage.
- **Images.** Float32/Float64, HWC/CHW logical orders, non-final alpha, no alpha, multiple groups, hidden RGB, explicit association conversion without default conformance rejection, planar physical access, ROI and channel dependencies, legal views and materialization, deliberate contract changes and native backend support limits.
- **External engines.** Wrapper semantics, comparison of the frozen artifact with an independent direct engine run, and task-level effect; plus ownership, concurrency, cancellation, errors, resources and cache identity. Mocks or offline oracles do not replace real inference, decoding or quality validation.

## 4. Non-goals and explicit boundaries

- The frozen ABI is a contract decision. Package 0.36.0 implements Result operation C ABI 6, which accepts only exact structure sizes, and the unfrozen C++ `OperationDefinition` registration path described in [Plugin ABI](../kernel-architecture/Plugin-ABI.md). ABI 6 is not the frozen ABI.
- The kernel C++ API used by applications is not frozen by this decision.
- No ABI 6 compatibility layer, shim or dual registration path is provided.
- No new element types: Float16, Int32, UInt32, UInt64 and native complex stay outside the pipeline.
- No native Sequence or RationalTime type, time-indexed frame service, frame-rate or missing-frame policy, playback, seek, replay or implicit cross-call time state in the kernel.
- No built-in inference framework, automatic license judgment or artifact approval product. License records stay separate from resource and execution identity.
- No sandbox, signature check, isolated worker or forced termination; modules remain trusted in-process code.
- No declared kernel capability format or extension slot, flat-box or bitmap Region container, or performance promise is defined here.
- No color, alpha, transfer or sampling description is attached to Result data or propagated by the kernel.
- Canonical straight and planar images are not reopened as an open product choice.

## 5. Consequences

Every module, built-in or external, must be rebuilt against the frozen ABI header; an ABI 6 module is rejected at load. Built-in operations stop using the C++ registration interface and kernel-private headers, so helpers they need move into the SDK or operation helper libraries. The host keeps the cost of maintaining defaults for every absent field and of validating whole module tables before publication.

Prefix negotiation lets a structure grow without rejecting older modules, but only additions with defined defaults are minor changes. Any change to an existing field's meaning, ownership or threading rule is a major version, and the host rejects a module that requires content it does not understand. A module that requires a missing service or device capability is rejected or reported as unsupported rather than run with guessed behavior.

Moving built-in operations onto C callbacks adds a function-pointer call per block or tile. Per-row window, per-coordinate read and per-element cancellation or work callbacks are the expensive paths; they need batched forms before migration, and the C-bridge overhead is measured against direct C++ on representative 01-numeric operations and PixelOE. The existing premultiplied image operations change their public contract when they adopt straight images; those changes are reported and validated separately from behavior-preserving ABI migration.

Host applications that need temporal behavior implement frame selection, weighting and state threading themselves, using explicit inputs and `state_in`/`state_out` Results. Operations that wrap models or engines carry their own validation, metering declarations and determinism evidence before result caching is enabled for them.
