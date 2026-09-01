# Data Model

## Source and compiler values

`WorkflowDocument` contains a version, bounded nodes, typed scalar parameters,
input edges, and named outputs. It is caller-owned compiler input, not a file
format or storage object.

`SemanticGraphIR`, `OptimizedGraphIR`, and `ExecutionPlan` are immutable stage
values with separate typed digests. They contain copied operation traits and
stable keys, never callbacks, DSO handles, runtime allocations, or daemon ids.

## Runtime Value

The current dense `Value` contains:

- `ValueDescriptor`: `UInt8`, `Int64`, or `Float64` plus rank-1-to-8 nonzero
  shape;
- one rank-matching logical `Region`;
- `StridedLayout`: byte offset and one signed byte stride per axis;
- up to 64 unique versioned `ValueFacet` key/payload records;
- one shared immutable owned byte vector.

`Value::create` checks rank, shape, Region containment, element vocabulary,
stride count, signed offset/span arithmetic, overflow, element tail, complete
buffer bounds, facet keys/versions, duplicate keys, and bounded facet payloads
before atomic publication. Negative and zero strides are accepted only when
the addressed byte range stays inside the buffer. Facets are sorted by key;
copies share immutable bytes and expose no writable pointer.

`Region` uses unsigned offset/extent pairs in descriptor-axis order. It is a
logical subset, never a byte range. Interval addition and element-count
multiplication are checked.

## Results and data definitions

`ExecutionResult` is a caller-owned map of named Values plus raw diagnostics.
It has no durable identity, retention, receipt, serialization, or recovery
contract.

The data-definition registry copies a schema key, element type, and maximum
rank from startup configuration or a trusted DSO, then freezes. It does not
construct Values or provide storage.
