# Plugin ABI and ownership

[Chinese reader version](zh/Plugin-ABI.zh.md).

## Scope and ownership

Trusted native modules export operation module ABI 8. The loader copies static declarations into `OperationRegistry`; C modules and C++ `OperationDefinition` callbacks then use the same `OperationProgram` builder, physical Plan validator and fixed invocation ABI. OperationTraits uses version 30.

The registry retains the module while definitions, static preparations or prepared kernels need its code. The compiler owns static metadata and the physical task graph. `PreparedPlan` owns linked kernels and resources; each Run owns its bindings, output allocations, scratch and completion state. CPU kernels receive fixed affine views and write preallocated outputs under the [unchecked execution contract](Compiler-and-Execution.md).

## Module and declaration records

```c
#include "photospider/plugin/operation_module_api.h"

/* The module supplies this function and an immutable table. */
PS_OPERATION_MODULE_EXPORT const ps_operation_module_v8*
ps_operation_module_get_api_v8(void);
```

`operation_module_api.h` is a standalone C11 header. It includes `fixed_invocation_api.h` for status, layout, invocation and prepared-kernel records. Define `PHOTOSPIDER_OPERATION_PLUGIN_BUILD` when building the producer. `Photospider::operation_sdk` supplies the include directory and requires no kernel linkage for a C producer.

The module table contains its exact `struct_size`, `abi_version = 8`, 1..1024 operation records, a context and a required `destroy` callback. Each operation declares its key, capability flags, input contracts, output prototypes, scalar parameter vocabulary, optional static preparation and resource validation, mandatory lowering, and optional kernel preparation. COPY-only or entirely elided operations may omit kernel preparation.

| Record | Static meaning |
| --- | --- |
| `ps_schema_v8` | Schema identity/version, 1..16 tensor members and optional schema publication metadata |
| `ps_tensor_spec_v8` | Element type, cell shape, batch axes, atomic trailing axes and spatial layout; complete rank is at most eight |
| `ps_port_contract_v8` | Schema identity and optional member dtype/rank predicates |
| `ps_parameter_spec_v8` | Exact Int64, Float64, Bool or String type, required flag and optional numeric bounds |
| `ps_operation_output_v8` | Output name, complete prototype and optional original-input projection |
| `ps_resource_id_v8` | Immutable resource byte length and SHA-256 |

C strings carry explicit byte lengths. Empty strings use `{NULL, 0}`. Keys are strict UTF-8; parameter string values are arbitrary bytes. Operation keys allow 1024 bytes; other declaration keys, kernel symbols, contracts and placement strings allow 128 bytes. Arrays use naturally aligned nonnull pointers exactly when their count is nonzero. Records with `struct_size` require an exact size match.

The compatibility policy retains exact-size validation. Each ABI version fixes its record layouts, function signatures, numeric encodings and field semantics, including ownership and threading rules. Changes to those contracts require a new version of the affected ABI. New names for existing numeric values leave the binary contract unchanged. The host accepts the versions documented here; structures have no prefix or trailing-field negotiation.

The standalone C headers define the complete numeric vocabulary. Enum constants name values carried in fixed-width integer fields; the ABI does not embed C enum objects. `ps_element_type_v8` defines these codes:

| Constant | Code | `element_type_mask` bit |
| --- | --- | --- |
| `PS_ELEMENT_UINT8_V8` | 1 | `PS_ELEMENT_MASK_UINT8_V8 = 0x01` |
| `PS_ELEMENT_INT64_V8` | 2 | `PS_ELEMENT_MASK_INT64_V8 = 0x02` |
| `PS_ELEMENT_FLOAT64_V8` | 3 | `PS_ELEMENT_MASK_FLOAT64_V8 = 0x04` |
| `PS_ELEMENT_FLOAT32_V8` | 4 | `PS_ELEMENT_MASK_FLOAT32_V8 = 0x08` |
| `PS_ELEMENT_INT8_V8` | 5 | `PS_ELEMENT_MASK_INT8_V8 = 0x10` |
| `PS_ELEMENT_UINT16_V8` | 6 | `PS_ELEMENT_MASK_UINT16_V8 = 0x20` |
| `PS_ELEMENT_INT16_V8` | 7 | `PS_ELEMENT_MASK_INT16_V8 = 0x40` |

A port selects one nonzero element code or a bitwise OR of `PS_ELEMENT_MASK_*_V8` constants. Both fields zero add no dtype restriction; setting both nonzero is invalid. Each mask bit is `1u << (code - 1)`; `PS_ELEMENT_MASK_ALL_V8` is `0x7f`, and other bits are rejected. Zero is a port wildcard, not a tensor element type.

Schema scopes use `PS_SCHEMA_COMPLETE_BUNDLE_V8 = 1`, `PS_SCHEMA_INDEPENDENT_CHUNKS_V8 = 2` or `PS_SCHEMA_STABLE_PREFIX_V8 = 3`. These values describe schema publication metadata; fixed execution still completes whole tasks and publishes Results at call retirement or RET. Repeated inputs use `PS_REPEATED_MATCH_DTYPE_SHAPE_V8 = 1` to require the selected tensor members to share dtype and full logical shape, including batch axes. `PS_REPEATED_MATCH_NONE_V8 = 0` permits differences and requires a static prepare callback for an active group. Disabled groups have a null template and zero bounds/match code. Boolean fields use 0 and 1.

An operation has at most 1024 inputs, 64 outputs and 128 parameter declarations. Repeated inputs add one trailing homogeneous contract after the fixed input prefix, with explicit minimum/maximum bounds. `input_count = UINT32_MAX` and a null projection pointer select all original inputs; zero selects no payload inputs. Static preparation can narrow the registered projection. Schema-only dependence is declared separately during lowering.

`load_plugin(path)` accepts an explicit path of 1..4096 bytes with no embedded NUL. Missing ABI 8 entry points fail `TypeMismatch`. A null, misaligned, wrong-size or wrong-version module table fails `InvalidArgument`. The loader checks the module header before attaching its destroy hook and reading nested records. It validates every declaration in a private candidate registry, then publishes all operations atomically. Malformed declarations, duplicate keys or a concurrent freeze leave the registry unchanged.

## Static preparation and resources

For C++ operations, `OperationTraits` declares input arity and port constraints, repeated-input rules, named output prototypes and projections, parameter types/bounds, purity, supported backends and extension identities. `OperationPreparation` returns output specializations, required resource identities and immutable state. These static contracts enter compiler identities; callback addresses and mutable invocation state do not.

Lowering declares each task's scratch by memory domain and each output tile's allocation contract. Shared intermediate computation uses private SSA values and task edges. An operation with `side_effect_free = false` must lower exactly one terminal effect task; the planner retains that call even when its outputs are unselected. Block admission derives output capacity and per-lane scratch from the physical tables before executing kernels.

`ps_operation_v8::prepare` receives `ps_static_query_v8`, synchronous preparation services and an initially empty `ps_operation_state_v8` output. The host copies input schemas before entering static callbacks and preserves parameter values, including floating-point bit patterns. The callback may set each output once, retain its default prototype by omitting it, and declare resource identities through `require_resource`.

The returned state is immutable and shared across output selections, lowering, kernel preparation and concurrent Runs. A nonnull context requires a release callback. A null context may still have a release callback and retains its cleanup owner. Ownership of a populated state transfers to the host on both success and failure; the host releases it exactly once. Retained callback inputs must be copied into operation-owned storage.

After schema specialization, the host resolves declared resource identities from the supplied `ResourceSet`. Missing resources reject preparation. An optional `validate_resources` callback inspects only those immutable bytes and the static state, once per preparation, including Empty output demand and artifact rebinding. Kernel preparation receives borrowed views of the same retained resources. Lowering has no resource bytes or sample payload.

Preparation services copy supplied metadata before returning. They accept calls only on the callback thread and retain the first failure. An ignored service failure takes precedence over a later callback status or exception. The preparation sink permits at most 1,048,576 calls and 4096 resource declarations; schema and output bounds apply independently.

## Lowering into a physical Plan

`ps_operation_v8::lower` receives resolved input/output schemas, parameters, static state, tile extent, backend, member grids, selected output tile intervals and construction limits. Each grid names its original port/member and gives its full shape, tile count, rows, columns and batch-axis mask. The requested intervals are half-open member-local tile indices.

`ps_operation_builder_v8` exposes the checked construction operations:

- `add_task` declares a kernel symbol/contract, placement, scratch by memory domain and an optional terminal effect.
- `add_copy` declares an explicit transfer with one matching read and write. Its backend is chosen independently of numerical tasks: a native GPU Plan may contain CPU COPY tasks for Host/Shared transfers. A CpuExact request requires CPU tasks. GPU lowering may explicitly mix CPU/GPU numerical tasks when the operation advertises the respective capabilities; Device endpoints require GPU COPY placement. The builder declares one dense Shared scratch segment per Host endpoint of a GPU COPY, sized to its output bytes. This declaration participates in serialized Plan validation and per-lane admission.
- `add_temporary`, `write_temporary` and `read_tile` connect private SSA tiles within the call.
- `read_input`, `read_input_schema` and `write_output` declare payload reads, metadata dependence and exclusive output tiles.
- `depends_on` adds an ordering edge; `set_parameters` copies opaque task parameter bytes.

IDs are local to the call's builder. Every materialized output is a complete fixed-grid tile with one producer. The host validates output coverage, topology, call membership, placement and allocation bounds before publishing the Plan. Builder failures are sticky. Service calls and aggregate parameter bytes are bounded by `maximum_work`; table records and references use the builder's record/edge limits. A successful callback cannot override a failed builder operation.

Private tasks provide shared intermediate computation. Their tile edges determine readiness, while call order determines retirement. Schema-only reads create no payload dependency. Serialized Plan import restores the saved tables and rebinds static preparation; it does not call lowering.

## Kernel preparation and invocation

```c
/* Signature of ps_operation_v8::prepare_kernel. */
ps_kernel_status_v8 prepare_kernel(
    void* user_data, const ps_kernel_preparation_v8* query,
    ps_prepared_kernel_v8* result);
```

The query contains the kernel symbol/contract, exact placement, complete static metadata, actual read/write views, task parameter bytes, scratch bounds, resources and the native-service pointer. Each view includes its original port/member, tile index, domain, compiled tile byte extent, byte offset and signed affine layout. Private writes use `port = UINT32_MAX`. A Device input may bind a subview of a larger allocation; its runtime capacity and offset include the checked allocation base. The declared layout still limits sample access.

The C++ `KernelPreparation` query borrows its operation name, input metadata, scalar parameter map and encoded task parameter vector from immutable owners. Its output metadata and read/write view arrays are temporary preparation storage. All remain valid through the callback; copying the query preserves the borrowed references and does not extend their lifetime. Retained provider state must copy the individual values it needs. C bridge arrays retain their existing callback-scoped lifetime.

The module validates its invocation signature here and returns an immutable prepared context with direct CPU and/or GPU entry points. A nonnull kernel context requires release. A populated kernel output transfers to the host even when preparation reports failure; failed preparation and malformed returned ABI records release that context. Successful kernel owners retain the static preparation, resource backing and defining module.

CPU invocation borrows inputs, exclusive outputs, scratch, task parameters and a cancellation query until synchronous return. Success means every declared output sample has been initialized and all accesses have ended. The host restores the callback's floating-point environment. Kernels are reentrant; mutable state belongs to the invocation's scratch or output allocations.

`ps_kernel_status_v8` carries an operational code, reason, optional domain code and a bounded NUL-terminated message. Status values come from `fixed_invocation_api.h`: `ps_kernel_code_v8` defines categories (`PS_KERNEL_NOT_FOUND_V8 = 2`, `PS_KERNEL_CANCELLED_V8 = 6`), and `ps_kernel_reason_v8` defines reasons from `PS_KERNEL_REASON_NONE_V8 = 0` through `PS_KERNEL_REASON_VIEW_UNAVAILABLE_V8 = 13`. The host preserves `domain_code` as an opaque operation-owned value. Unknown codes/reasons or unterminated messages become `Internal`. Runtime sample validation is outside this protocol.

The executor also links asynchronous GPU entry points over Host, Shared or Device storage and arms a completion slot before submission. Acceptance retains invocation views and scratch through the matching terminal callback; rejection transfers no borrowing. Inline completion is valid. Context-wide GPU credits return only after completion or local rejection settlement. Native Shared buffers are allocated before block issue and may be accessed by CPU kernels or explicit COPY tasks. Native preparation links Metal MSL or Vulkan SPIR-V kernels over Shared and Device storage. Device allocations expose opaque identities and remain unmapped; completed outputs retain their owners through Result publication. The executor prepares COPY tasks internally, without calling an operation kernel callback.

`ps_kernel_completion_v8::report_timing` is an optional timing request and receiver. A nonnull receiver may be called once, serially before `finish`, with a borrowed fixed-size `ps_kernel_device_timing_v8`; rejection must not report timing. The host copies the record into the armed slot without allocation. Reporting never settles the task or releases borrowing, and invalid keys or duplicate reports cannot settle another submission. Unavailable timing is omitted or uses clock domain zero; a failed timing query does not change task success. Metal uses system mach nanoseconds. Vulkan reports raw ticks, valid bits, nanoseconds per tick and a process-local clock instance. Compute the pair's delta modulo the valid bits; cross-submission Vulkan comparisons require calibration beyond this interface. Neither clock is the Run's host steady clock. Native implementations report command intervals after quiescence; the existing completion/visibility contract remains in force.

### Typed CPU invocation 9

Trusted C++ registrations may supply `OperationDefinition::prepare_kernel_v9` for a CPU `KernelDesc` with `abi_version=9`. The independent C records in `fixed_invocation_v9_api.h` are `ps_prepared_kernel_v9` and `ps_kernel_invocation_v9`. Both require their own exact `sizeof(record)` and version 9; the envelope contains an ordinary exact-size/version-8 `views` record and a `ps_kernel_work_v9` service. No version-8 record is enlarged or interpreted as a version-9 prefix. Preparation and invocation use typed entry points. A descriptor with no matching factory is rejected, without ABI downgrade. Calling the v8 invoke overload on a v9-only preparation is rejected before provider entry.

The thread-safe work service admits `{work, stages}` against the current Run and its shared Context root before numerical work. Zero/zero polls cancellation or the sticky Run budget failure. Budget rejection returns `ResourceExhausted` with `WorkLimit` or `StageLimit`; cancellation can return `Cancelled`. The service context is borrowed only until synchronous CPU return, is specific to this invocation/Run, and must never be retained in prepared state or passed to another Run. The host retains owners through CPU return. Provider arithmetic must establish its required rounding/underflow environment; the host fence saves and restores the callback environment. Capacity exceptions retain their resource-failure classification.

FRQ-01..06 use this C++ CPU path exclusively. Existing FRQ invocation-8 Plans require recompilation. Other invocation-8 CPU/GPU operations and operation module ABI 8 remain supported unchanged. The module-8 transport does not export the C++ v9 factory; native kernel service ABI 2 and psir/psxp v5 layouts are unchanged.

## Native kernel preparation

Include `photospider/plugin/native_kernel_api.h` to use `ps_native_gpu_service_v2` from the kernel preparation query. Check the service version and backend before calling `prepare(context, program, query, result)`. The service is borrowed for the enclosing preparation callback. A successful call transfers one immutable `ps_prepared_kernel_v8` owner; a failed call leaves a valid output record empty.

`ps_native_program_v2` describes one dispatch with source, entry point, bindings, constants, a three-dimensional grid and optional group dimensions. Metal accepts MSL source of 1..262144 bytes and a UTF-8 entry of 1..128 bytes. Up to 31 distinct buffer indexes are available, including the separate constant index when constants are present; constants occupy at most 4096 bytes. Grid dimensions are positive and bounded by `UINT32_MAX`. Zero group dimensions request device-selected sizing; an explicit group requires three positive dimensions within device and pipeline limits. Explicit groups round up the dispatched grid, so the shader must guard padded invocations.

Vulkan accepts aligned SPIR-V 1.0..1.5 modules containing 20..262144 bytes in complete words. The selected compute entry must declare a positive fixed `LocalSize`. Zero group dimensions select that module size; explicit dimensions must match it. Preparation checks device group, grid, descriptor and buffer-range limits. Constants are copied into one immutable coherent uniform buffer during preparation, and its actual native capacity remains charged to the device root for the prepared owner's lifetime.

Each binding selects an input, output or scratch view from the fixed invocation. Input/output `view` is an invocation array index; scratch `view` is its memory-domain code. Both native adapters accept Shared views and Device handles and check domains and ranges against the preparation signature. Device byte offsets select positions inside the native buffer without arithmetic on the opaque identity. Every output must be bound exactly once over its full accessible range. Input data remains immutable. Shader code must obey these declarations and initialize all output samples.

Preparation compiles the pipeline and copies binding records, constants and dispatch dimensions. Metal uses MSL 2.4, disables fast math and prefixes the source with `#pragma clang fp contract(off)`. The returned kernel retains the pipeline and device independently of the pipeline cache and caller-owned descriptor memory. Submission resolves actual device-owned buffers and commits one command buffer. Metal uses a command-buffer completion handler. Vulkan allocates independent command, descriptor and fence objects for each submission; one device completion worker drains accepted nodes in queue order. It releases submission resources before delivering the terminal completion. Both paths establish visibility before ending the borrowed invocation. A device command failure makes the device unavailable and aborts the Run; accepted submissions retain their owners until their own terminal completion.

Native pipeline keys and prepared context blocks are charged to the device's explicit metadata root. A caller's thread-local allocation scope does not change that owner; an unmanaged device stays unmanaged. Pipeline-cache construction and context allocation can each evict cached pipelines and retry admission once under metadata pressure. Prepared kernels retain their pipeline independently, so eviction can make room for another context without invalidating accepted work. Exhaustion after reclamation returns `ResourceExhausted` with no transferred kernel owner.

`test_fixed_native` uses a C11 operation module and real Metal shaders to exercise prepared pipelines, dependent and independent tile tasks, Plan import, output selection, concurrent input bindings and explicit Host/Shared COPY chains. Its direct service checks cover malformed descriptors, a prepared pipeline surviving cache eviction, Shared scratch, padded three-dimensional groups and copying the full 4096-byte constant payload. `test_fixed_async` separately exercises controlled rejection, malformed completion, cancellation drain and backend failure. Its backend-failure cases retain accepted work until completion, stop further issue and preserve GPU placement even when a CPU entry exists. These cases use controlled completion rather than native-device fault injection. `test_native_gpu` checks device-root accounting, cache reclamation, full threadgroups with barriers, the Metal contraction policy and completion visibility after accepted work is cancelled. `test_fragment_atlas_gpu` runs fixed native kernels against sparse atlas payloads and checks sample bits and holes at ranks 1, 3 and 8. `test_vulkan_gpu` covers independent SPIR-V inputs and outputs, three-dimensional dispatch, concurrent reuse of prepared kernels, constant capacity, metadata reclamation and rollback, allocation rollback, fixed Plan execution and Device destruction from its completion callback.

## Lifetime and failure boundaries

| Owner | Release condition |
| --- | --- |
| Callback input arrays and service tables | Callback return; retaining these borrowed addresses is invalid |
| Static operation state | Last preparation/kernel owner releases, after kernel context cleanup |
| Prepared kernel context | Last kernel owner releases after all borrowing invocations complete |
| Module table/code | Last definition/static/kernel lease releases; module destroy precedes native unload |
| Input/output/resource backing | Last owning handle releases, independently of registry lifetime |

The host fences preparation/lowering exceptions and converts allocation failures to `ResourceExhausted`. C callbacks and release functions must obey their nonthrowing contract. Service failure selection and cleanup remain active on rejected preparations. A callback that never returns can prevent drain; cancellation requires cooperation and does not authorize early release.

Modules execute with host-process privileges. The ABI provides no sandbox, signature admission, IPC loading, runtime dependency discovery, continuation polling, implicit transfers, numerical certification or backend retry. Backend selection and COPY topology belong to compilation. The [compiler and executor contract](Compiler-and-Execution.md) defines ordered call retirement, final RET publication and cancellation drain.
