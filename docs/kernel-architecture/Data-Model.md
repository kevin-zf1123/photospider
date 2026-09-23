# Data Model

## Source and compiler values

`WorkflowDocument` contains a version, bounded nodes, typed scalar parameters,
input edges, and named outputs. It is caller-owned compiler input, not a file
format or storage object.

`SemanticGraphIR`, `OptimizedGraphIR`, and `ExecutionPlan` are immutable stage
values with separate typed digests. They contain copied operation traits and
stable keys, never callbacks, DSO handles, runtime allocations, or daemon ids.

## Runtime Value

A generic regional `Value` contains:

- `ValueDescriptor`: `UInt8`, `Int64`, `Float64`, or `Float32` plus rank-1-to-8 nonzero
  shape;
- one rank-matching logical `Region`;
- `StridedLayout`: logical origin, byte offset and one signed byte stride per axis;
- up to 64 unique versioned `ValueFacet` key/payload records;
- one shared immutable `CpuStorage` owner.

`Value::create` checks rank, shape, Region containment, element vocabulary,
stride count, signed offset/span arithmetic, overflow, element tail, valid-Region
buffer bounds, facet keys/versions, duplicate keys, and bounded facet payloads
before atomic publication. Negative and zero strides are accepted only when
the addressed byte range stays inside the buffer. Facets are sorted by key;
copies share immutable bytes and expose no writable pointer.

`Region` uses unsigned offset/extent pairs in descriptor-axis order. It is a
logical subset, never a byte range. Interval addition and element-count
multiplication are checked.

`Value::as_float64()` is a strict scalar accessor: in addition to the exact
Float64 descriptor, contiguous layout, and storage bounds, the Value Region
must be rank one and exactly `{offset=0, extent=1}`. Empty, partial, and offset
Regions remain legal general Value coverage but return `TypeMismatch` through
this accessor.

`BufferAllocator` obtains a reservation before allocating exact-capacity CPU
bytes. `MutableBuffer` and `MutableValue` are move-only; publication consumes
the writer and retains the lease with immutable storage. `Value::from_storage`
validates origin-relative coverage without copying; `view` restricts coverage
while sharing ownership, and `byte_address` checks logical coordinates.
`bytes()` returns a borrowed `ByteView`; `copy_bytes()` explicitly copies into
caller-owned memory. Storage and its reservation may outlive the allocator.
`test_storage` verifies partial/reversed views and last-owner lease release.
Execution-wide resource admission retains allocation leases to the last owner.
Generic numeric regional sources use this storage; their synchronous streaming
sinks receive a borrowed `ValueView` that expires when the callback returns.
Image execution uses the structural storage contract below.

## Structural image storage

Package 0.19 uses one image memory contract, defined in
[Tensor storage and region access](../kernel-specs/Tensor-Storage-and-Region-Access.md).
`PlanarImage` is the physical owner for a rank-two or rank-three generic tensor
with explicit image axes, component groups, facets and resources. It is not an
RGB/Layer semantic carrier and imposes no color arithmetic. Each image reserves
one continuous virtual address range. Continuous planes and DAG-sized tiled
planes both store contiguous row samples. In tiled storage, right edges pad to
tile width, bottom edges retain only valid rows, and every block starts on a host
page boundary.

Virtual reservation, page backing, metadata capacity and valid samples are
separate. Pages are explicitly prepared and admitted before operator access.
Only successful publication makes its exact samples valid; unproduced samples
in a supplied page remain unreadable through the API. Published samples are
immutable. Produced backing and its leases survive until the last image/window
owner retires; budget exhaustion does not evict live pages or replay producers.

`PlanarImage::import_value` is the explicit interleaved/strided import boundary.
`acquire` provides a retained exact read window with bounded `row_run` spans.
`read` explicitly copies a requested region into caller-owned packed storage.
A host-prepared transactional write window supplies only authorized output
spans. It commits on successful operation completion or rolls back unpublished
resources on failure. The complete reservation is never a readable `ByteView`.
A raw numeric interpretation does not bypass these physical access rules.

Image execution pins external source owners against publication for the Run,
then admits their stable retained capacity. Ordinary retained read windows still
allow disjoint publication. Shared accounting recognizes repeated owners and
same-context result rebinding without charging the same backing twice.

## Results and data definitions

`ExecutionResult` owns named generic `values`, named planar `images`, structured
results where supported, and raw diagnostics. Images have no automatic dense
Value export. Results retain their storage leases but have no durable identity,
receipt, serialization or recovery contract.

The data-definition registry copies a schema key, element type, and maximum
rank from startup configuration or a trusted DSO, then freezes. Provider load
accepts only an exact nonempty 1..4096-byte path without embedded NUL before
the platform loader; malformed paths are `InvalidArgument`, while a valid path
that cannot be loaded is `NotFound`. It does not construct Values or provide
storage.

## Workflow inputs and binding snapshots

Schema 3 uses `WorkflowInputDeclaration` and tagged `WorkflowInput` sources:
`WorkflowNodeOutput` or `WorkflowInputReference`. Node ids and declaration ids
have independent namespaces. Declarations are unique by nonzero id and exact
1..128-byte printable ASCII name without spaces, bounded by 4096 and copied in
id order through `input_declarations()` on every compiler stage.

A generic numeric declaration fixes a UInt8/Int64/Float64/Float32 descriptor,
whole Region, zero byte offset, positive canonical row-major strides and an
exact facet set. Dense byte count B is checked without allocating payload: B > 0,
B - 1 <= INT64_MAX and B <= SIZE_MAX; every stored stride also fits int64.
General Values retain their existing strided/partial-Region behavior.

An image declaration instead carries `planar_layout` and an empty affine
layout. Its axes, storage mode, row pitch and component groups are compiler
metadata. `OperationOutputTraits.planar_layout` declares each supported planar
output, independently from its inputs. Layout and capability changes affect
compiler identity; runtime addresses do not. The one DAG tile geometry comes
from PlanningOptions and is checked against every bound image.

`ExecutionBindings` contains exact-name entries. Generic inputs select a Value,
RegionalSource or non-image InputSnapshot. Planar inputs select `image` and no
other storage alternative. Source metadata and required names are checked before
callbacks; image binding checks descriptor, facets, structural layout and tile
geometry. Plans never retain input pixel addresses. Independent bindings can
execute a current plan repeatedly or concurrently.

The CPU planar callback path supports explicit single-output Whole/Elementwise
operations with at least one planar input. Unsupported traits and unmigrated
image operations fail explicitly. Legacy image/Layer structured schemas cannot
provide an alternative image storage path.
The old packed-image binding, snapshot and ValueFragments paths are not alternate
image implementations. Non-image Value capabilities remain. Image demand,
streaming, frozen/atom and GPU entry points require their own structural
migration; unsupported entries reject without executing legacy image code.

Float32 uses element code 4 and preserves all IEEE binary32 bit patterns in a
generic Value. Scalar/operation contracts supply any consumed numerical domain
checks. The former typed-image domains in [Image operations](Image-Operations.md)
and [ADR 0016](../adr/0016-workflow-inputs-and-execution-bindings.md) do not authorize
legacy image execution under the planar contract.

CpuStorage denotes CPU-accessible immutable storage and may own a completed Metal shared buffer. Its bytes are readable after publication; native owners and reservation leases can outlive ExecutionContext. Device handles remain private.
