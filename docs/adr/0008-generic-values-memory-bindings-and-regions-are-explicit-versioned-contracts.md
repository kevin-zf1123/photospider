# ADR 0008: Value, Layout, and Region Are Explicit Validated Contracts

- Status: Accepted, narrowed by ADR 0015
- Date: 2026-09-01 boundary revision

## Accepted target amendment by ADR 0016

The accepted target adds Float32 and the S1 image/scalar binding constraints.
See [ADR 0016](0016-workflow-inputs-and-execution-bindings.md). Existing version
and representation descriptions below remain the implementation baseline until
#257 delivers the target; decision acceptance does not report runtime changes.

## Context

Typed compilation and local heterogeneous execution need a runtime value that
does not infer logical shape or addressed bytes from a raw pointer. The
breaking baseline intentionally keeps this contract small enough to validate
completely.

## Decision

One immutable dense `Value` contains exactly:

- a `ValueDescriptor` with one closed `ElementType` and rank-1-to-8 nonzero
  shape;
- one rank-matching logical `Region` of unsigned half-open intervals;
- one `StridedLayout` with byte offset and one signed byte stride per axis;
- zero to 64 versioned `ValueFacet` records with unique printable-ASCII keys
  and bounded opaque payloads;
- one shared immutable owned byte vector.

The closed element vocabulary is `UInt8`, `Int64`, and `Float64`. Signed
strides permit reversed or broadcast views only when complete addressed-range
validation proves that every element byte remains inside the owned vector.

`Value::create` publishes atomically after checking rank, nonzero extents,
Region rank/containment, element vocabulary, stride count, signed
multiply/add overflow, offset bounds, element tail, complete addressed byte
range, facet key/version uniqueness, per-facet payload size, and aggregate
facet payload size. Facets are sorted by key before publication. Failure
returns no partial Value. Copies share immutable bytes and never expose a
writable pointer.

`Region` is rank-general logical coverage, not a byte range. Construction and
containment use checked unsigned addition; `element_count` uses checked
multiplication. An empty interval makes the complete Region empty.

### Identity and local transfer

Logical descriptor/Region/layout facts are independent from allocation
address, backend label, and optional reproducibility digests. `Value` itself
has no durable or daemon identity.

When local execution crosses backend labels it creates another validated Value
with copied immutable bytes. Transfer/residency observations live in the
owning `ExecutionRun`, not in a persistent Value registry.

### Data definitions

The data-definition ABI may register a bounded schema key, element type, and
maximum rank for operation/Value semantics. Registry records are copied and
frozen at startup. The ABI does not add a storage or construction service.

## Boundary

The current contract has no blocked layout, readiness fence, writable producer
binding, general serialization API, durable artifact, receipt, retention,
recovery, or storage-service semantics. Facets are bounded semantic records,
not memory owners or extension-code handles.

## Consequences

- Compiler, operation, and runtime checks share one small dense Value model.
- Malformed or duplicate facets fail before Value publication.
- Negative/broadcast strides remain safe through complete range validation.
- Cross-backend copies cannot publish malformed layout or stale result state.
- A Value or digest never implies persistence or authorization.
