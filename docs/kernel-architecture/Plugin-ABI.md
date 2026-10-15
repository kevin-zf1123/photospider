# Plugin ABI and ownership

## 1. Core summary (TL;DR)

Photospider loads trusted operation modules through versioned C tables and copies declarations before publishing them. The structured Result C ABI uses one Result object per port; typed tensor members carry numeric and image samples, while fields carry primitive records. Package 0.34.0 uses the Result operation C ABI 4 and OperationTraits 26. ABI 4 adds validator and diagnostic registration, explicit resource selection during metadata resolution, authorized validation reads, and `fail_domain`; Result records carry no facet or domain metadata.

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
    Callback->>Services: Request Result facts and tensor coverage
    Services-->>Callback: Return authorized reads and owning windows
    Callback-->>Host: Return status or a staged Need
    Host->>Host: Check cancellation, sticky failures, and publication
```

The registry owns each immutable definition and its dynamic-library lease. An invocation snapshot retains the definition while callbacks run. The Result coordinator resolves named outputs, tensor projections, field I/O, dependency relations, and backend services. Result owns the schema and tensor storage; a tensor may use affine CPU storage or another backing described by its physical layout.

### Entry points and core model

```c
#include "photospider/plugin/result_operation_plugin_api.h"

typedef struct ps_result_operation_plugin_api_v4 {
  uint32_t struct_size, abi_version;
  const ps_result_operation_v4* operations;
  uint32_t operation_count;
  void* context;
  void (*destroy)(void*);
  const ps_result_validation_definition_v4* validators;
  uint32_t validator_count;
  const ps_result_diagnostic_definition_v4* diagnostics;
  uint32_t diagnostic_count;
} ps_result_operation_plugin_api_v4;

const ps_result_operation_plugin_api_v4*
ps_result_operation_plugin_get_api_v4(void);
```

`result_operation_plugin_api.h` is the standalone C11 operation-plugin header. It owns the Result element-type and parameter-type enums, parameter descriptor/value records, extension identity and resource identity/view records, validator and diagnostic definitions, and capability flags. `PS_RESULT_EXPORT` is defined here and uses the shared `PHOTOSPIDER_OPERATION_PLUGIN_BUILD` producer switch. Native GPU declarations come from the separate `native_gpu_api.h` and use their own ABI version.

```cpp
#include <utility>
#include "photospider/data/result.hpp"

ps::SchemaTemplate schema;
ps::ResultTensorSpec pixels;
pixels.key = "pixels";
pixels.batch_axes = {2, 3};
pixels.descriptor = {ps::ElementType::Float32, {1080, 1920, 4}};
pixels.layout.spatial = true;
pixels.layout.height_axis = 0;
pixels.layout.width_axis = 1;
pixels.layout.channel_axis = 2;
schema.tensors.push_back(std::move(pixels));
```

A Result schema declares tensor members, primitive fields, and a domain, with at most 16 combined tensor and field members. A tensor descriptor describes cell axes and element type; `batch_axes` prefixes those axes to form its complete sample shape, whose rank is at most eight. Spatial axis indices in `ResultTensorLayout` are cell-relative. Storage order and row pitch describe physical layout, not semantic schema identity. Tensor payload is separate from packed field records. For a non-spatial tensor, every batch and cell extent must be positive and the complete sample rank must be 1 through 8; schema validation does not require a dense byte allocation or a representable product of all extents. A packed field has a record rank of at most 7. Schema validation computes its row byte size and rejects a product above `INT64_MAX`, but it does not allocate dense storage for the field's complete row count.

Result object identity, schema, descriptor facts, fields, tensor members, and relations belong to an owning Result reference. Operation resources belong to the prepared operation and plan, not to the Result. An association retains ordered source ObjectIds as history; dependency support belongs to relation rows. An association does not itself retain source payload. A retained Result, tensor window, or published view keeps the backing owners required for its reads.

Result operation ABI 4 is an independent C table. A module exports `ps_result_operation_plugin_get_api_v4`; the loader checks table alignment, exact structure size, and ABI version before attaching the table destroy hook. The ABI 3 getter is not accepted, and no compatibility header or forwarding entry point exists. Import validates pointer/count pairs and alignment, record sizes (the outer record size is checked before any nested pointer is read), operation constraints, and bounded strict UTF-8 operation and parameter names. Validator and diagnostic definitions in the table are registered together with the operations. Each port's `validators` array and each operation's `diagnostics` array name extension identities (key, version, implementation) that must resolve to a definition in the same import or in the registry; a missing identity or a version/implementation conflict rejects the import. The loader imports and validates every record into a private registry candidate before it swaps the complete candidate into the live registry. A repeated input with `repeated_maximum` set is rejected when both `repeated_match` and `resolve_metadata` are absent. Any validation or duplicate-key failure leaves the live registry unchanged. Successful imported definitions retain the plugin table owner while their records may be used; the table destroy callback runs when that retained lifetime ends. A failed import, duplicate key, or owner allocation releases the attached table through its destroy hook before closing the native library. The loader accepts the Result ABI 4 entry point and rejects a module that does not provide it. `Value` is typed numeric backing used by Result storage and codecs; it does not define an operation or workflow input/output type. C++ and C operation interfaces both use Result ports. Dependency records describe Result support evidence rather than an alternate operation callback table. `test_result_plugin.cpp::continuation_owns_library` starts a Result Object Need, lets the registry leave scope without a publication observer, and then resumes the continuation successfully; the imported definition retains its module owner through callback completion.

Each Result output descriptor declares its name, Result port, input projection, Whole or Regional execution mode, and output policies. Input ports constrain Result schemas or selected tensor-member predicates. The optional `resolve_metadata` callback receives borrowed compiled input metadata, parameters, and output prototypes. It calls the synchronous metadata sink once per output index; the sink copies and validates nested schema records before returning. The resolver may refine a Result port/schema but cannot change the output flags or payload bound. The `output` view supplied to each `start` and `poll` reflects those registered policies. Sink failures are sticky, and preparation fails if an output is omitted. The sink's `set_resources` call lists the `ps_result_resource_identity_v4` entries (byte length and SHA-256) that the operation needs; the compiler selects them from the caller's `ResourceSet`, and `start`, `poll`, and validator `prepare` later receive those bytes as borrowed `ps_result_resource_view_v4` records. There is no ambient resource lookup. Operation keys, output names and projections, Result schema identity, and declared constraints remain fixed.

The output flags select view and payload policies:

| Flag | Contract |
| --- | --- |
| `PS_RESULT_OUTPUT_PRESERVE_VIEWS_V4` | Allows a CPU, Atomic output to preserve a compatible immutable input view. In compiled structured CPU Whole execution, the coordinator prepares payload-authorized Need boxes before the computation poll. Auto may collect only when an affine view is unavailable. Direct callback entry does not synthesize Needs or prepare views. |
| `PS_RESULT_OUTPUT_REQUIRE_INPUT_VIEWS_V4` | Requires `PRESERVE_VIEWS` and CPU Whole execution. The coordinator rejects an unavailable affine view before the computation callback. |
| `PS_RESULT_OUTPUT_PAYLOAD_BOUND_V4` | Enables `maximum_output_payload_bytes`. The bound applies to CPU Whole or staged output; a zero value is an explicit zero-byte bound. |

Unknown bits are rejected. If `PAYLOAD_BOUND` is clear, the byte field must be zero. The host checks each published Result's physical backing, deduplicates owners, and excludes backing that remains live through exposed Results or authorized tensor grants. Root still accounts for source and collected input backing, workspace, metadata, and operation resources. The check occurs at publication, so it does not prevent earlier callback allocations. Singleton callback queries expose the registered output policy.

The nested `ps_result_output_v4` record carries these output fields within operation table ABI 4. The importer requires the exact current output-structure size and rejects a record with any other `struct_size`, so C modules are built against the current header. The development package version is not frozen.

A `start` or `poll` callback receives resolved ports, the selected output index, requested coverage, tile geometry, parameters, selected resource views, and backend. `read_tensor` copies authorized samples. Retained tensor handles preserve their certified tensor capability until release or operation-state destruction.

Ordinary Result C callbacks and services return `int` values defined by `ps_result_status_v4`: codes 0 through 6 mean success, failure, cancellation, backend unavailable, resource exhausted, type mismatch, and invalid argument. Poll yield codes and `VIEW_UNAVAILABLE` follow their respective service contracts. Native GPU services use their separate GPU result codes. Operation capability flags are `PS_RESULT_FLAG_DETERMINISTIC_V4`, `PS_RESULT_FLAG_SIDE_EFFECT_FREE_V4`, `PS_RESULT_FLAG_CPU_V4`, `PS_RESULT_FLAG_GPU_V4`, and `PS_RESULT_FLAG_CPU_FALLBACK_V4`. The fallback flag requires both CPU and GPU capability flags.

Both Result C table callbacks, `start` and `poll`, execute within the structured executor's poll-phase fallback gate. A retry requires deterministic, side-effect-free traits and a retry-safe attempt with no published output, field I/O, native dispatch, or sticky operation/service, cancellation, or stale-state failure. Other errors remain terminal. The separate C++ `OperationRegistry::start_result` entry point validates the operation's declared backend support before it creates a continuation; it does not determine whether an execution context has a usable GPU lane or device. During structured execution, the coordinator checks GPU lane and device availability before entering a GPU-targeted start factory, then handles `BackendUnavailable` through its ordinary fallback rules. After successful tensor/view/field/Result publication, a callback return of `BackendUnavailable` is converted to `OperationFailed`; `begin_result` alone does not count as publication. `publish_result` accepts only `complete` values 0 or 1 and permits one successful publication per poll; the next poll can publish another prefix. A valid duplicate in one poll latches `OperationFailed`. An earlier invalid service failure remains sticky if the callback then attempts a valid publication or reports success. Actual host cancellation may supersede a valid duplicate-publication failure at the execution boundary; a callback-returned `Cancelled` does not.

### Callback lifetime, resources, and execution

The query and service table are borrowed for one `start` or `poll` callback. A saved phase context expires when that callback returns; C services are callable only during that callback. The poll lease invalidates its translated service handles at callback return and keeps tombstones until the continuation is destroyed, so stale handles can be rejected without reusing their addresses. Operation state remain owned by the continuation and are destroyed before the retained operation definition releases its DSO lease. Retained tensor windows keep their own explicit release and destruction lifetimes. Entry-thread services run on the callback entry thread. CPU range and tile workers may call `cancelled` and write admitted scratch; owning window `row` and `rectangle` access may also run concurrently on CPU workers without reentering execution services. Window data pointers remain valid until the final corresponding handle is released; release follows worker completion. The metadata sink is borrowed only during resolver entry. The host keeps service failures sticky even if the callback later reports success.

C++ Result operation factories and scalar poll callbacks are exception-fenced at their registry and continuation boundaries. `std::bad_alloc` becomes `ResourceExhausted`; a standard exception becomes `OperationFailed` with `HostException` and its `what()` text, or an empty message when `what()` is null. A non-standard exception becomes `OperationFailed` with a fixed `HostException` diagnostic. A failure already recorded by phase services takes precedence; exceptions from a host failure observer do not replace that selected failure.

`ResultProgramPhase::acquire_native_tensor` and the C service `acquire_native_tensor_window` expose the current GPU callback's authorized tensor Need as an owning window backed for native access. The GPU callback then binds its row or rectangle to a native dispatch; the API does not dispatch on the caller's behalf. Without a GPU lane, acquisition returns `BackendUnavailable`. Missing slots, uncovered regions, and Descriptor-only authorization fail as sticky `InvalidArgument` protocol errors. CPU or incompatible-device backing is packed as needed and uploaded under Root accounting. A compatible same-device affine backing can be reused without upload. Fragmented backing may require both materialization and upload, which are counted separately. The window keeps the source Result owner, schema, descriptor, cancellation state, and native storage alive across callback phases and external owner release. C callers release the returned handle through the ordinary window release service. The executor may reuse raw native backing within one Run when a live weak proof still identifies the same CPU storage and the complete physical view key matches; this reuse does not retain the source CPU owner, Result schema, or association. Across Runs, an optional byte-bounded content cache keys device instance/build and the logical samples, Region, dtype, and shape. The key excludes the Result schema and association; a hit is rebound to the current Result schema and association. Optional hashing consumes the coordinator shared cache-work budget, and insufficient optional work skips cache lookup/retention. Capacity pressure can reclaim retransmittable Run entries and then evict context-retained native entries. `host_access_count` records actual CPU exposure of live native-backed storage, deduplicated within the callback scope; ordinary metadata inspection, native-address acquisition, view forwarding, and upload hashing do not count as host reads.

Whole CPU callbacks can use the CPU range service. CPU tile callbacks use the coordinator's tile service. When a native GPU lane is available, callbacks receive its native services subject to the operation capability. GPU calls run on the active callback thread, and dispatch retains referenced owners until completion. C++ `ResultProgramPhase::gpu_status` reads the active invocation's host status and expires with the phase. CPU tile callbacks run as indivisible tile tasks.

The execution Root accounts admitted work, stages, metadata, payload capacity, and retained owners. A callback that exceeds a bound receives a resource or stage failure; issued work is not refunded. Result publication validates schema, sample coordinates, cancellation, and resource ownership before exposing an immutable result.

Owning tensor-window reads also debit the captured execution Root. C row and rectangle services charge an upper bound computed by `ResultWindowAccess::read_work` before reading; a row uses factor 1 and a rectangle factor 2. This is Root-lifetime work because a retained window outlives the borrowed poll phase. It is separate from the per-run dependency/program work limit configured for one execution.

### C validators, diagnostics, and domain failures

A `ps_result_validation_definition_v4` supplies an extension identity, `scratch_bytes`, `requires_complete`, and three callbacks. `prepare` receives a `ps_result_validation_preparation_v4` (port schema, parameters, selected resource views) and returns an owned state pointer; `destroy_prepared` releases it, including after a failed `prepare`, and always runs before the module owner retires. `validate` receives a `ps_result_validation_context_v4` whose services exist only on the entry thread until the call returns: `tensor_regions` and `field_range` describe the newly authorized coverage, `read_tensor` and `read_field` copy at most 65,536 bytes per call in logical order, and `consume_work`/`cancelled` connect to the host budget. A read outside the authorized coverage fails and stays sticky even if the callback ignores the return code. The C bridge preserves the first service or `fail_domain` failure over a later callback status or exception.

A `ps_result_diagnostic_definition_v4` declares a fixed `payload_bytes` record with `validate` and `merge` callbacks; `merge` writes a complete destination and must not modify its inputs. An operation lists the diagnostic identities it may emit in `ps_result_operation_v4::diagnostics` and reports through `report_diagnostic(type_index, bytes, size)` on its run services.

`fail_domain(status_code, domain_code)` on the run services and on the validation context records the first domain failure with a nonzero status code and an opaque 64-bit domain code. The host copies the code into `FailureDetail::domain_code` without interpreting it.

### SDK targets

The shared `Photospider::operation_sdk` target provides C++ Result helpers, including `ps::plugin::element_type_value`. The Result operation table remains a standalone C11 interface, and its DSO callbacks do not require C++. Native GPU services are separately declared in `native_gpu_api.h` under ABI 1, and the CPU parallel and tile services keep ABI 1. Callback outcome codes do not carry C-supplied error strings; host status, timing, and count diagnostics remain separate from the bounded reports supplied through `report_diagnostic`.

`Photospider::ops_sdk` is the operations-domain SDK. It links only `Photospider::kernel` and installs public headers under `include/photospider/ops/` (export macro `PHOTOSPIDER_OPS_SDK_API`): color descriptions and ICC/OCIO resources, transfer/RGB/white codecs, exact domain arithmetic, description editing, the 3D LUT bake contract and quality reports, numeric diagnostics, and the Layer composite type. CMake option `PHOTOSPIDER_BUILD_OPS_SDK` defaults to the value of `PHOTOSPIDER_BUILD_OPS`; bundled operations require it. `PHOTOSPIDER_BUILD_OPS` defaults to `OFF`, so a default kernel-only configuration builds neither. The kernel itself contains none of these domain types.

## 4. Non-goals & explicit boundaries

- Operation modules are trusted in-process code. ABI validation does not sandbox or authenticate them.
- C++ consumers build against the installed package 0.34.0 headers. Resource facilities are declared in `photospider/core/`: `core/cancellation.hpp`, `core/resources.hpp`, `core/resource_allocator.hpp`, and `core/data_movement.hpp`, which holds the data-movement enums. WorkflowDocument schema 6 and OperationTraits 26 define the current compiler contract. The Result operation C table is ABI 4. The C import boundary rejects invalid external records, and internal execution states are represented only after validation.
- Built-in operations register through the unfrozen C++ `OperationDefinition` registration path in the installed headers, and the C importer accepts only exact ABI 4 structure sizes. [ADR 0022](../adr/0022-freeze-operation-c-abi-v3.md) (Proposed) records the target: one prefix-negotiated C ABI shared by built-in and external modules, separately versioned service tables, and an optional callback path beside declared capabilities.
- The ABI carries no semantic kind, facet, or attached description. Domain interpretation is an operation parameter, and domain legality is a bound validator.
- Production image operation availability follows the source and registry classifications in [Image operations](Image-Operations.md).
- GPU execution requires a native-backend build and an available device. A table or capability declaration alone does not prove hardware execution.
- The Result C table exposes Result ports and tensor members only. IPC plugin loading and daemon protocol remain outside this ABI.

## 5. Consequences

Malformed or incompatible tables fail before registry publication. A module rejected after native loading releases its library owner. A published definition keeps the library loaded until its final invocation or state owner retires. Service, resource, and cancellation failures remain terminal. Backend failures also remain terminal except for the explicitly opted-in, retry-safe GPU `BackendUnavailable` case described above.

Synchronous native dispatch retains input owners until device completion, so cancellation can wait for an in-flight call. Long CPU callbacks must observe cancellation through the services. Native modules run with host-process privileges, so only trusted code should be loaded.
