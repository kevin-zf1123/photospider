# Plugin ABI and ownership

## 1. Core summary (TL;DR)

Photospider loads trusted operation modules through an exact-size versioned C table and copies declarations before publishing them; built-in operations register through the C++ `OperationRegistry` instead. The structured Result C ABI uses one Result object per port, and every Result is a static directory of 1..16 typed Tensor members that carry numeric and image samples. A Result stores structure and data only: callbacks request inputs through explicit member tile Needs, and the host records each completed computation unit's Needs outside the Result. Package 0.35.0 uses the Result operation C ABI 5 and OperationTraits 27. ABI 5 provides member-local tile intervals, static publication scopes, Run-shared stages, validator and diagnostic registration, explicit resource selection during metadata resolution, authorized validation reads, and `fail_domain`; Result records carry no facet or domain metadata.

## 2. Mental model & intuition

```mermaid
sequenceDiagram
    participant Host
    participant Module
    participant Callback
    participant Services
    Host->>Module: Read Result operation table
    Host->>Host: Validate and copy declarations
    Host->>Host: Publish immutable definition and library lease
    Host->>Callback: Lend query and phase-scoped services
    Callback->>Services: Request member tile or stage Needs, Result facts, tensor reads
    Services-->>Callback: Return authorized reads and owning windows
    Callback-->>Host: Return status, a Need yield, or a publication
    Host->>Host: Check cancellation, sticky failures, and publication
    Host->>Host: Record the unit's admitted Needs as a TileRecord
```

The registry owns each immutable definition and its dynamic-library lease. An invocation snapshot retains the definition while callbacks run. The Result coordinator resolves named outputs, input projections, input tile Needs, shared stages, and backend services, and records the admitted Needs of each completed computation unit in a `TileRecord`. The Result owns the schema and tensor storage; a tensor may use tiled CPU storage, affine CPU storage, or another backing described by its physical layout.

## 3. Formal contracts & APIs

### Entry points and core model

```c
#include "photospider/plugin/result_operation_plugin_api.h"

typedef struct ps_result_operation_plugin_api_v5 {
  uint32_t struct_size, abi_version;
  const ps_result_operation_v5* operations;
  uint32_t operation_count;
  void* context;
  void (*destroy)(void*);
  const ps_result_validation_definition_v5* validators;
  uint32_t validator_count;
  const ps_result_diagnostic_definition_v5* diagnostics;
  uint32_t diagnostic_count;
} ps_result_operation_plugin_api_v5;

const ps_result_operation_plugin_api_v5*
ps_result_operation_plugin_get_api_v5(void);
```

`result_operation_plugin_api.h` is the standalone C11 operation-plugin header. It owns the Result element-type and parameter-type enums, parameter descriptor/value records, schema, tensor, tile-grid, tile-Need and shared-stage records, extension identity and resource identity/view records, validator and diagnostic definitions, and capability flags. `PS_RESULT_EXPORT` is defined here and uses the shared `PHOTOSPIDER_OPERATION_PLUGIN_BUILD` producer switch. Native GPU declarations come from the separate `native_gpu_api.h` and use their own ABI version. The C tile helpers in `result_tile_helpers.h` build member Needs from borrowed grid records.

```cpp
#include <utility>
#include "photospider/data/result.hpp"

ps::SchemaTemplate schema;
ps::ResultTensorSpec pixels;
pixels.key = "pixels";
pixels.batch_axes = {2, 3};
pixels.descriptor = {ps::ElementType::Float32, {4, 1080, 1920}};
pixels.layout.spatial = true;
pixels.layout.channel_axis = 0;
pixels.layout.height_axis = 1;
pixels.layout.width_axis = 2;
schema.tensors.push_back(std::move(pixels));
```

A Result schema declares 1..16 Tensor members, a domain, and optional publication scopes. A tensor descriptor describes cell axes and element type; `batch_axes` prefixes those axes to form its complete sample shape, whose rank is at most eight. Spatial axis indices in `ResultTensorLayout` are cell-relative. Storage order and row pitch describe physical layout, not semantic schema identity; the default `TensorStorageOrder::Tiled` stores each tile separately with planar spatial channels inside it. Every batch and cell extent must be positive and every member shape is fixed before execution; schema validation does not require a dense byte allocation or a representable product of all extents. Extents of kind `PS_RESULT_EXTENT_INPUT_AXIS_V5` or `PS_RESULT_EXTENT_INPUT_ELEMENTS_V5` are evaluated from the static domains of inputs during preparation.

Result object identity, schema, descriptor facts, and tensor members belong to an owning Result reference. Operation resources belong to the prepared operation and plan, not to the Result. An association retains ordered source ObjectIds as history. Dependency facts belong to the execution's `TileRecord` table, which records the actual Needs of each computation unit; the Result carries no dependency record. An association does not itself retain source payload. A retained Result, tensor window, or published view keeps the backing owners required for its reads.

Result operation ABI 5 is an independent C table. A module exports `ps_result_operation_plugin_get_api_v5`; the loader checks table alignment, exact structure size, and ABI version before attaching the table destroy hook. Only the ABI 5 getter is recognized; no compatibility header or forwarding entry point exists, and older record layouts have no compatibility prefix. Import validates pointer/count pairs and alignment, record sizes (the outer record size is checked before any nested pointer is read), operation constraints, and bounded strict UTF-8 operation and parameter names. Validator and diagnostic definitions in the table are registered together with the operations. Each port's `validators` array, each shared stage's `validators` array, and each operation's `diagnostics` array name extension identities (key, version, implementation) that must resolve to a definition in the same import or in the registry; a missing identity or a version/implementation conflict rejects the import. The loader imports and validates every record into a private registry candidate before it swaps the complete candidate into the live registry. A repeated input with `repeated_maximum` set is rejected when both `repeated_match` and `resolve_metadata` are absent. Any validation or duplicate-key failure leaves the live registry unchanged. Successful imported definitions retain the plugin table owner while their records may be used; the table destroy callback runs when that retained lifetime ends. A failed import, duplicate key, or owner allocation releases the attached table through its destroy hook before closing the native library. C++ and C operation interfaces both use Result ports and the same tile Need model.

### Outputs, shared stages, and metadata

Each `ps_result_output_v5` declares its name, Result port, input projection, flags, and payload bound. Every output executes as one computation unit per member tile of its compiled grid; there is no execution-granularity field. C import sets `cacheable` only when both purity flags are present. An operation without the side-effect-free flag must declare at least one mandatory shared stage, and it is then uncached. Input ports constrain Result schemas or selected tensor-member predicates.

`ps_result_shared_stage_v5` declares one Run-shared stage: key, schema, sorted input projection, backend, `mandatory`, cumulative `state_bytes`, `workspace_bytes`, `maximum_work`, `maximum_output_payload_bytes`, `maximum_stages`, and validators. A stage has no public output identity. The host copies these declarations during registration and preparation, and creates at most one producer per Run, node, and stage. The `start` and `poll` callbacks select a stage through `ps_result_query_v5::shared_stage`; a tile callback sees `UINT32_MAX` there. A stage callback ignores `output_index`, `tensor_slot`, and `tile_unit`. The executor's stage contract is described in [Run-shared stages](Global-Results.md#run-shared-stages).

The optional `resolve_metadata` callback receives borrowed compiled input metadata, parameters, and output prototypes. It calls the synchronous metadata sink once per output index; the sink copies and validates nested schema records before returning. The resolver may refine a Result port/schema but cannot change the output flags or payload bound. `set_shared_stages` replaces all stage prototypes once with owned static declarations. `set_resources` lists the `ps_result_resource_identity_v5` entries (byte length and SHA-256) that the operation needs; the compiler selects them from the caller's `ResourceSet`, and `start`, `poll`, and validator `prepare` later receive those bytes as borrowed `ps_result_resource_view_v5` records. There is no ambient resource lookup. Sink failures are sticky, and preparation fails if an output is omitted. Operation keys, output names and projections, Result schema identity, and declared constraints remain fixed.

`PS_RESULT_OUTPUT_PAYLOAD_BOUND_V5` is the only output flag. It enables `maximum_output_payload_bytes`; an explicit zero bound permits borrowed storage only. Unknown bits are rejected. If the flag is clear, the byte field must be zero. The host counts unique physical owners once per output per Run, cumulatively across tile actors, and excludes input and shared-stage owners. Root still accounts for source backing, workspace, metadata, and operation resources. The check occurs at publication, so it does not prevent earlier callback allocations.

### Queries and tile Needs

A `start` or `poll` callback receives a `ps_result_query_v5` with resolved ports, the selected output index, backend, the selected member `tensor_slot`, `tile_unit` and `tile_extent`, the shared-stage selector and declarations, one compiled grid group per input plus the output grid group, parameters, and selected resource views. A grid group holds one `ps_result_tile_grid_v5` per member in schema order. A grid record gives the member's full shape, batch rank and `batch_axis_mask`, the height and width axes (the rank value marks a missing tiled axis), the tile extent, tile rows and columns, and the tile count. `read_tensor` copies authorized samples. Retained tensor handles preserve their certified tensor capability until release or operation-state destruction.

A callback requests input through `need_tiles` and then returns `PS_RESULT_NEED_V5`:

```c
typedef struct ps_result_tile_interval_v5 {
  uint64_t begin, end;
} ps_result_tile_interval_v5;

typedef struct ps_result_tile_need_v5 {
  uint32_t struct_size, input, tensor_slot, descriptor_only;
  const ps_result_tile_interval_v5* intervals;
  uint64_t interval_count;
} ps_result_tile_need_v5;
```

| Form | Grant |
| --- | --- |
| `descriptor_only = 0`, `interval_count > 0` | After grid-bound validation, the samples of the listed member tiles, closed over `atomic_trailing_axes`, plus the input's descriptor facts. |
| `descriptor_only = 0`, `interval_count = 0` | Nothing. The host creates no dependency, no descriptor grant, and no producer work. |
| `descriptor_only = 1` | Captured descriptor facts only. `tensor_slot` must be zero and there are no intervals. |

Intervals are half-open local tile indices in the producer member's compiled grid. One call accepts at most 65,536 intervals. Overlapping and adjacent intervals coalesce; a reversed or out-of-grid interval fails. The host copies and normalizes the intervals under Root work and capacity limits and validates the complete envelope before starting any producer. Failed requests are sticky. The coordinator converts no sample rectangles itself: C modules derive intervals from the borrowed grid records, and C++ operations can use `photospider/plugin/tile_helpers.hpp`. When the unit completes, the host merges its admitted Needs into one `TileRecord`, merging intervals only within one producer member.

`result_tile_helpers.h` provides `ps_result_need_descriptor_v5`, `ps_result_to_whole_v5` (all members as complete intervals), and `ps_result_need_regions_v5` (tiles intersecting explicit rectangles of one member). `ps_result_need_regions_v5` closes atomic axes, turns contiguous rows and batches into intervals, and admits at most 65,536 intervals before scratch allocation. When every supplied rectangle is empty, it submits a zero-interval Need, which requests nothing. `ps_result_output_region_v5` resolves the selected output member tile. `ps_result_to_whole_v5` only submits Needs: the outer poll returns Need, and a later poll acquires the authorized owning windows. The C API provides equivalent tile publication through window acquire/retain/release and `publish_tensor_view`; it has no `ps_result_to_tile_v5` convenience function.

### Shared-stage services

`need_shared_stage(stage)` requests every member tile of a prepared stage. `need_shared_tiles(stage, slot, intervals, count)` requests member-local stage tile intervals; repeat it for distinct members. A call with zero intervals grants no samples and does not start the stage. The two calls cannot both name the same stage in one poll, and a repeated or unknown ordinal fails. `acquire_shared_window(stage, slot, region, ...)` returns a retained window from a supplied stage Result. It grants no access to the stage's original inputs, and the window follows the ordinary retain and release lifetime. When the current query is itself a shared stage, the host checks each C tile Need against that stage's prepared `input_indices` projection.

### Status, fallback, and publication

Ordinary Result C callbacks and services return `int` values defined by `ps_result_status_v5`: codes 0 through 6 mean success, failure, cancellation, backend unavailable, resource exhausted, type mismatch, and invalid argument. Poll yield codes (`PS_RESULT_NEED_V5`, `PS_RESULT_PUBLISH_V5`) and `PS_RESULT_VIEW_UNAVAILABLE_V5` follow their respective service contracts. Native GPU services use their separate GPU result codes. Operation capability flags are `PS_RESULT_FLAG_DETERMINISTIC_V5`, `PS_RESULT_FLAG_SIDE_EFFECT_FREE_V5`, `PS_RESULT_FLAG_CPU_V5`, `PS_RESULT_FLAG_GPU_V5`, `PS_RESULT_FLAG_CPU_FALLBACK_V5`, and `PS_RESULT_FLAG_SHARE_BLOCKS_ACROSS_OUTPUTS_V5`. The fallback flag requires both CPU and GPU capability flags.

Both Result C table callbacks, `start` and `poll`, execute within the structured executor's poll-phase fallback gate. A retry requires explicit CPU fallback opt-in, deterministic, side-effect-free traits, and a retry-safe attempt with no published output, checkpoint, block or discovery callback, native dispatch, non-retryable sticky operation/service failure, cancellation, or stale-state failure. Other errors remain terminal. A retry discards the attempt's continuation, input capabilities, recorded Needs, and input facts, then restarts the same computation unit on CPU; completed producer units, spent Root work, and stage accounting remain. The retried unit is fallback-tainted and does not enter the checkpoint, block-state, or completed-result caches. Stage computation is never retried after factory entry. The separate C++ `OperationRegistry::start_result` entry point validates the operation's declared backend support before it creates a continuation; it does not determine whether an execution context has a usable GPU lane or device. During structured execution, the coordinator checks GPU lane and device availability before entering a GPU-targeted start factory, then handles `BackendUnavailable` through its ordinary fallback rules. After successful tensor, view, or Result publication, a callback return of `BackendUnavailable` is converted to `OperationFailed`; `begin_result` alone does not count as publication. `publish_result` accepts only `complete` values 0 or 1 and permits one successful publication per poll; a shared-stage continuation can publish another prefix on a later poll. An ordinary tile publication must be complete and cover exactly its selected member tile, with other members empty. A valid duplicate in one poll latches `OperationFailed`. An earlier invalid service failure remains sticky if the callback then attempts a valid publication or reports success. Actual host cancellation may supersede a valid duplicate-publication failure at the execution boundary; a callback-returned `Cancelled` does not.

### Callback lifetime, resources, and execution

The query and service table are borrowed for one `start` or `poll` callback. A saved phase context expires when that callback returns; C services are callable only during that callback. The poll lease invalidates its translated service handles at callback return and keeps tombstones until the continuation is destroyed, so stale handles can be rejected without reusing their addresses. Operation state remains owned by the continuation and is destroyed before the retained operation definition releases its DSO lease. Retained tensor windows keep their own explicit release and destruction lifetimes. Entry-thread services run on the callback entry thread. CPU range and tile workers may call `cancelled` and write admitted scratch; owning window `row` and `rectangle` access may also run concurrently on CPU workers without reentering execution services. Window data pointers remain valid until the final corresponding handle is released; release follows worker completion. The metadata sink is borrowed only during resolver entry. The host keeps service failures sticky even if the callback later reports success.

C++ Result operation factories and scalar poll callbacks are exception-fenced at their registry and continuation boundaries. `std::bad_alloc` becomes `ResourceExhausted`; a standard exception becomes `OperationFailed` with `HostException` and its `what()` text, or an empty message when `what()` is null. A non-standard exception becomes `OperationFailed` with a fixed `HostException` diagnostic. A failure already recorded by phase services takes precedence; exceptions from a host failure observer do not replace that selected failure. A direct shared-stage continuation whose validator rejects a later tile retires its projected producer with the final guarded poll failure; an escaped earlier prefix reports that failure through `production_status()` and still reads its certified tiles.

`ResultProgramPhase::acquire_native_tensor` and the C service `acquire_native_tensor_window` expose the current GPU callback's authorized tensor Need as an owning window backed for native access. The GPU callback then binds its row or rectangle to a native dispatch; the API does not dispatch on the caller's behalf. Without a GPU lane, acquisition returns `BackendUnavailable`. Missing slots, uncovered regions, and descriptor-only authorization fail as sticky `InvalidArgument` protocol errors. CPU or incompatible-device backing is packed as needed and uploaded under Root accounting. A compatible same-device affine backing can be reused without upload. Fragmented backing may require both materialization and upload, which are counted separately. The window keeps the source Result owner, schema, descriptor, cancellation state, and native storage alive across callback phases and external owner release. C callers release the returned handle through the ordinary window release service. Native upload reuse and caching are described in [Native GPU input access](Global-Results.md#native-gpu-input-access).

Ordinary CPU poll callbacks can use the CPU range service, `ps_cpu_parallel_service_v2`. CPU-staged coordinators submit tile computations through `ps_cpu_tiles_service_v2`; CPU tile callbacks run as indivisible tile tasks. CPU tile workers and GPU callbacks do not receive the range service. Parallel work completes before its service call returns, and nested parallel submission is prohibited. Kernel callbacks inherit the floating-point environment of their worker and restore it on return; numeric operations establish their own environment inside executing workers. When a native GPU lane is available, callbacks receive its native services subject to the operation capability. GPU calls run on the active callback thread, and dispatch retains referenced owners until completion. C++ `ResultProgramPhase::gpu_status` reads the active invocation's host status and expires with the phase.

The execution Root accounts admitted work, stages, metadata, payload capacity, and retained owners. A callback that exceeds a bound receives a resource or stage failure; issued work is not refunded. Result publication validates schema, sample coordinates, cancellation, and resource ownership before exposing an immutable result.

Owning tensor-window reads also debit the captured execution Root. C row and rectangle services charge an upper bound computed by `ResultWindowAccess::read_work` before reading; a row uses factor 1 and a rectangle factor 2. This is Root-lifetime work because a retained window outlives the borrowed poll phase. It is separate from the per-run dependency/program work limit configured for one execution.

### C validators, diagnostics, and domain failures

A `ps_result_validation_definition_v5` supplies an extension identity, `scratch_bytes`, `requires_complete`, and three callbacks. `prepare` receives a `ps_result_validation_preparation_v5` (output flag, port, shared-stage ordinal, schema, parameters, selected resource views) and returns an owned state pointer; `destroy_prepared` releases it, including after a failed `prepare`, and always runs before the module owner retires. `validate` receives a `ps_result_validation_context_v5` whose services exist only on the entry thread until the call returns: `tensor_regions` describes the newly authorized coverage of a member, `read_tensor` copies at most 65,536 bytes per call in logical order, and `consume_work`/`cancelled` connect to the host budget. `complete` reports whether the validated coverage is complete, as defined in [Port validators](Global-Results.md#port-validators-and-diagnostic-reports). A read outside the authorized coverage fails and stays sticky even if the callback ignores the return code. The C bridge preserves the first service or `fail_domain` failure over a later callback status or exception.

A `ps_result_diagnostic_definition_v5` declares a fixed `payload_bytes` record with `validate` and `merge` callbacks; `merge` writes a complete destination and must not modify its inputs. An operation lists the diagnostic identities it may emit in `ps_result_operation_v5::diagnostics` and reports through `report_diagnostic(type_index, bytes, size)` on its run services.

`fail_domain(status_code, domain_code)` on the run services and on the validation context records the first domain failure with a nonzero status code and an opaque 64-bit domain code. The host copies the code into `FailureDetail::domain_code` without interpreting it.

### SDK targets

The shared `Photospider::operation_sdk` target provides C++ Result helpers, including `ps::plugin::element_type_value`, `tile_helpers.hpp`, `tensor_helpers.hpp` (`sdk::to_whole`, `sdk::to_tile`, `sdk::collect`, `sdk::publish_dense_tensor`), and `whole_stage.hpp` (`sdk::with_whole_stage`, `sdk::shared_result_tile`). These helpers are described in [SDK acquisition helpers](Global-Results.md#sdk-acquisition-helpers). The Result operation table remains a standalone C11 interface, and its DSO callbacks do not require C++. Native GPU services are separately declared in `native_gpu_api.h` under ABI 1, and the CPU parallel and CPU tiles services are ABI 2. Callback outcome codes do not carry C-supplied error strings; host status, timing, and count diagnostics remain separate from the bounded reports supplied through `report_diagnostic`.

`Photospider::ops_sdk` is the operations-domain SDK. It links only `Photospider::kernel` and installs public headers under `include/photospider/ops/` (export macro `PHOTOSPIDER_OPS_SDK_API`): color descriptions and ICC/OCIO resources, explicit `TensorColorGroup` configuration, transfer/RGB/white codecs, exact domain arithmetic, description editing, the `ops.scalar-range` validator, the 3D LUT bake contract and quality reports, numeric diagnostics, and the Layer composite type. CMake option `PHOTOSPIDER_BUILD_OPS_SDK` defaults to the value of `PHOTOSPIDER_BUILD_OPS`; bundled operations require it. `PHOTOSPIDER_BUILD_OPS` defaults to `OFF`, so a default kernel-only configuration builds neither. The kernel itself contains none of these domain types.

## 4. Non-goals & explicit boundaries

- Operation modules are trusted in-process code. ABI validation does not sandbox or authenticate them.
- C++ consumers build against the installed package 0.35.0 headers. Resource facilities are declared in `photospider/core/`: `core/cancellation.hpp`, `core/resources.hpp`, and `core/resource_allocator.hpp`. WorkflowDocument schema 7 and OperationTraits 27 define the current compiler contract. The Result operation C table is ABI 5. The C import boundary rejects invalid external records, and internal execution states are represented only after validation.
- Built-in operations register through the unfrozen C++ `OperationDefinition` registration path in the installed headers, and the C importer accepts only exact ABI 5 structure sizes. A C operation has no persistent operation-owned preparation state. Its metadata resolver can replace static shared-stage declarations, including their input projections and workspace bounds, through `set_shared_stages`. C validators keep their own prepared state. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) (Proposed) records the target: one prefix-negotiated C ABI shared by built-in and external modules, separately versioned service tables, and a module-side header-only C++ SDK.
- The ABI carries no semantic kind, facet, or attached description. Domain interpretation is an operation parameter or operation-domain configuration, and domain legality is a bound validator.
- The ABI has no whole-result execution mode, no runtime member count, and no dynamic tile End. Complete-input algorithms request every member tile or use a shared stage.
- Production image operation availability follows the source and registry classifications in [Image operations](Image-Operations.md).
- GPU execution requires a native-backend build and an available device. A table or capability declaration alone does not prove hardware execution.
- The Result C table exposes Result ports and tensor members only. IPC plugin loading and daemon protocol remain outside this ABI.

## 5. Consequences

Malformed or incompatible tables fail before registry publication. A module built against an incompatible earlier ABI, such as ABI 4, is rejected and must be rebuilt against the ABI 5 header. A module rejected after native loading releases its library owner. A published definition keeps the library loaded until its final invocation or state owner retires. Service, resource, and cancellation failures remain terminal. Backend failures also remain terminal except for the explicitly opted-in, retry-safe GPU `BackendUnavailable` case described above.

Synchronous native dispatch retains input owners until device completion, so cancellation can wait for an in-flight call. Long CPU callbacks must observe cancellation through the services. Native modules run with host-process privileges, so only trusted code should be loaded.
