# ADR 0012: Operations and Data Definitions Use Exact In-Process C ABIs

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Context

The kernel needs an operation extension point and a directly related data
definition extension point without exposing compiler/runtime implementation
objects. Correctness validation must remain strong without claiming that a C
ABI makes native code safe or isolated.

## Decision

The operation ABI and data-provider ABI are independent version-one C
contracts. Their C++ helpers add no second binary contract.

### Operation ABI

An operation descriptor contains one length-framed key, exact input count,
flags, estimated bytes, output element type, one closed shape rule, one closed
Region rule, optional halo radius, cacheability, one synchronous execute
callback, and opaque descriptor-owned state.

The host copies these values into `OperationTraits` and semantic IR. It does
not copy callback, DSO, or opaque-state identity into IR or digests. A callback
receives bounded dense whole-Region input views, bounded facet records,
selected local backend, cooperative cancellation observation, and a host-owned
single-publication output sink. The host copies output facets/bytes before
callback return and rebuilds a validated Value.

### Data-definition ABI

A data provider publishes only bounded schema records: key, element type, and
maximum rank. The registry copies and freezes them. This ABI directly supports
operation/Value vocabulary; it does not read files, create runtime Values, or
own persistence.

### Exact validation and lifetime

Load and registration validate before publication:

- exact ABI version and exact structure size;
- natural pointer/array alignment;
- pointer/count pairs, maximum record/key/rank bounds, and checked arithmetic;
- length-framed keys without embedded NUL/control bytes;
- closed enum/flag/rule vocabulary and legal trait combinations;
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
