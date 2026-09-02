# ADR 0012: Operations and Data Definitions Use Exact In-Process C ABIs

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Context

The kernel needs an operation extension point and a directly related data
definition extension point without exposing compiler/runtime implementation
objects. Correctness validation must remain strong without claiming that a C
ABI makes native code safe or isolated.

## Decision

The operation ABI is an exact version-two C contract; the data-provider ABI
remains an independent version-one contract. Their C++ helpers add no second
binary contract. Operation ABI v1 is not retained as an adapter or decoder.

### Operation ABI

An operation descriptor contains one length-framed key, exact input count,
flags, estimated bytes, output element type, one closed shape rule (including
an explicitly bounded fixed shape), one closed Region rule, optional halo
radius, cacheability, a bounded parameter-schema pointer/count, one synchronous
execute callback, and opaque descriptor-owned state. Each parameter schema
record publishes a unique key, exact closed type, and required/optional bit.

The host copies these values into `OperationTraits` and semantic IR. It does
not copy callback, DSO, or opaque-state identity into IR or digests. A callback
receives bounded dense whole-Region input views plus plan-derived input-demand
offsets/extents, a canonical array of already schema-validated parameter
values, bounded facet records, selected local backend, cooperative cancellation
observation, and a host-owned single-publication output sink. The host copies
output facets/bytes before callback return and rebuilds a validated Value.

The C++ registry treats a Fixed rule as a logical descriptor contract: rank is
1..8, every extent is nonzero, and the element type and rule are closed. It
does not multiply the logical extents during trait publication. An embedding
callback may therefore publish a valid zero-stride broadcast Value even when
the corresponding dense element or byte product overflows. `estimated_bytes`
remains the callback's independent modeled resource-admission value and is
copied into the physical plan. The C DSO descriptor remains deliberately
narrower because ABI v2 publishes no strides: a Fixed DSO output must have
representable contiguous signed strides and a complete uint64 byte count, and
the loader rejects it transactionally otherwise. Preserve and Match semantics
are unchanged.

The unchanged `int` callback result has a closed version-two vocabulary:
success, ordinary failure, cooperative cancellation, and backend unavailable.
An explicit backend-unavailable result becomes `BackendUnavailable`; only a
GPU attempt whose copied traits permit CPU fallback may retry on CPU. Ordinary
failure and every unknown nonzero integer remain `OperationFailed` and never
trigger fallback. A backend-unavailable callback must not invoke the output
sink. If it does, an accepted output becomes a terminal `OperationFailed`
contract violation and a rejected output preserves the sink's exact typed
failure; neither path exposes `BackendUnavailable` or retries on CPU. Host
cancellation remains authoritative. Callback signature and descriptor layout
remain unchanged.

### Data-definition ABI

A data provider publishes only bounded schema records: key, element type, and
maximum rank. The registry copies and freezes them. This ABI directly supports
operation/Value vocabulary; it does not read files, create runtime Values, or
own persistence.

### Exact validation and lifetime

Load and registration validate before publication:

- exact ABI version and exact structure size;
- natural pointer/array alignment;
- pointer/count pairs, maximum record/key/rank/parameter bounds, and checked
  arithmetic;
- length-framed keys without embedded NUL/control bytes;
- closed enum/flag/rule/parameter-type vocabulary, unique parameter keys,
  required-item presence, exact source type, and legal trait combinations;
- descriptor-only C++ fixed-shape validation, plus independent dense
  stride/byte representability for stride-free C DSO fixed descriptors;
- required callbacks, single output publication, exact output element/shape,
  bounded facet array/key/version/payload, and byte count;
- callback exception fencing and exactly-once destroy ownership.

This version intentionally rejects trailing structure bytes rather than
treating them as forward compatibility. A malformed table publishes no
partial registry entry; multi-record publication uses copy-then-swap so
allocation failure also publishes no prefix. Libraries load only from explicit
process-startup configuration; registries become read-only before
compiler/executor use and destroy plugin-owned tables before unloading.

Operation/provider DSOs execute in process with the same trust as the host.
ABI validation prevents malformed interoperability; it is not a sandbox,
signature, certificate, trust chain, package admission, heartbeat, or process
supervisor.

## Boundary

There is no policy ABI/SDK/DSO, external scheduler, plugin path over IPC,
isolated plugin process, or native-code security product. The daemon never
selects a plugin path.

## Consequences

- Installed C11/C++17 consumers can author same-trust operation/data-definition
  DSOs.
- Compiler traits remain copied portable values.
- Exact validation, exception fences, and destroy-before-unload preserve
  correctness and cleanup.
- ABI version changes are explicit breaking changes in the 0.x package.
