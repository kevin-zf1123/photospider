---
spec_schema_version: 1
id: NUM-03B
parent_id: NUM-03
function: broadcast
operation_family: numeric.broadcast
proposed_operation_keys:
  - numeric.broadcast_strict
  - numeric.broadcast_accelerated_apple_silicon
  - numeric.broadcast_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-03B: broadcast

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The maintainer selected a separate broadcast operator that expands an array to
a target shape through an explicit axis mapping. Constant scalar filling has
its own NUM-03A specification. This draft records only that selected boundary;
shape/mapping constraints, storage, metadata, demand and error requirements.
Follow the common three-version NUM rule; copying must preserve
the strict logical output bits.

The selected axis map has one entry for each input axis, specifying a distinct
output axis. Arbitrary axis order is allowed, so a map can combine permutation
and expansion. It is injective and never merges two input axes into one output
axis. For example, input shape `[3]` maps to output `[2,3,4]` with axis map `[1]`.

Each mapped input extent must equal its target extent or be one. Length-one
axes expand; equal non-singleton axes preserve their coordinate. Periodic tiling,
merging/dropping axes and implicit alignment are excluded. The target inherits
constant's static positive rank-1..8 shape, total elements <=2^40, all-four-dtype
bit preservation, and static view/dense layout with view default.

## Interface and exact mapping

One input `input` is a UInt8/Int64/Float32/Float64 array of rank 1..8. One output
`values` has the requested static shape and exactly the input dtype. Both ranks
and shapes are known at compilation; output rank cannot be smaller than input
rank because the map is injective and includes every input axis.

Required static String parameters are `shape`, `axis_map`, and `layout`.
Shape uses NUM-03A's canonical comma-separated positive decimal extents. Axis
map uses one comma-separated nonnegative output-axis index per input axis,
without duplicates, negative indices, omitted axes or implicit inference. Layout
is view/dense; the constructor writes view by default. Direct nodes provide all
parameters. For input shape I, output shape O and map a, output coordinate o
reads source coordinate s:

```text
s[j] = 0            if I[j] == 1
s[j] = o[a[j]]      otherwise
values[o] = bitcopy(input[s])
```

Unmapped output axes replicate the same source. Static validation checks every
`0<=a[j]<rank(O)`, injectivity and `I[j]==1 || I[j]==O[a[j]]` before reading data.
No multiplication/expansion arithmetic is applied to the stored values. Generic
NaN payloads, infinities, signed zeros and integer extrema are preserved exactly.

The output has empty facets. Input metadata and actually read typed fragments
retain the existing recognized semantic validators; copying cannot strip an
invalid input's semantic obligations to make it valid. For example, image-v2
source reads require the relevant complete-pixel channel validation support.
Any additional required validation region must be declared and retained in the
witness. Opaque facets are not interpreted as a new image/array type and are
not copied onto the reshaped generic output.

## Demand and dirty mapping

For a requested output set Q, Data support is the exact set of mapped coordinates
`{s(o) | o in Q}`. Deduplicate repeated source samples. For rectangular Q, each
input axis maps its output interval or collapses to `[0,1)` for a singleton.
For disjoint requests, use the exact union rather than reading the bounding gap.
Source typed-validation closure is a separate additional dependency, including
full image channels when the applicable current semantic validator requires it.
Empty Q requests no input payload; schema/parameter checks still occur.

A changed input region R invalidates the output coordinates whose mapped s lies
in R, plus observations whose retained validation support intersects R. For a
non-singleton mapped axis, translate the input interval to that output axis;
for a broadcast/unmapped output axis, extend across its entire target extent.
This gives a Cartesian replication of changed source support. A changed typed
validation sample can invalidate related outputs even if its byte is not the
copied Data sample. Preserve upstream transitive witnesses and layout/metadata
identity rather than assuming equal output bytes establish freshness.

## View and dense storage

Both layouts support regional demand. The view path requests only the source
fragments needed by Q and its validation obligations; view does not authorize
whole-input computation. For each published output fragment, preserve an owning
reference to its source backing. Non-singleton mapped output axes inherit the
corresponding source byte stride; expanded singleton and unmapped axes use zero
stride. Map the output storage origin back to its validated source address to
set byte offset, including negative strides and nonzero origins.

If disjoint source coverage has different owners, publish corresponding owned
output fragments rather than fabricating one contiguous buffer. Output coverage
is the requested global Q; no implicit missing/zero-filled cells or writable
aliases are exposed. The implementation may share owners across repeated output
regions, but must retain the exact source and validation witness for each result.

Dense mode packs only requested output elements in logical row-major order,
reading through arbitrary valid source strides and repeating source bits where
needed. Copy from exact deduplicated source support; do not read another source
sample merely to fill a SIMD tail. Neither mode assumes the input backing byte
length equals its logical element product.

View adds layout/fragment metadata and can add zero output payload bytes, but
it retains actual source owners, which may be much larger than the requested
logical fragment. This is not a zero-retained-memory guarantee. Dense mode owns
its copied output, with upstream retention still governed by execution/cache
requirements. Both output owners can outlive the context; final-owner release
and unpublished failure cleanup follow the host contract.

## Work, resources and numerical profiles

For M requested outputs, U distinct required source elements and rank r<=8,
dense copying costs O(M*r) coordinate work with O(M*b) output payload; optimized
loops may reduce coordinate overhead. View costs region/set mapping and owner
metadata, not a dense fill of all replicated elements. Charge actual fragment
decomposition and typed validation work. Fragment count may grow for disjoint
or tiled inputs; it is bounded by the existing set/metadata/stage limits, with
ResourceExhausted rather than widening unauthorized source support.

All three keys preserve identical logical element bits. Permuted/view strides
follow the same selected layout contract; no approximate math, NaN conversion
or numeric tolerance is needed. SIMD is a byte-copy optimization and must not
change payload bits or read bounds. Unsupported platform keys return
BackendUnavailable instead of silently selecting another operator.

Use host workers, allocator/admission and work/stage budgets. Reserve dense
bytes before allocation, and account referenced owners and metadata separately
from new payload. Do not reserve M*b merely to construct a valid view. Poll
cancellation before reads, between mapping/refinement blocks, at least every
4096 dense elements and before publishing. Sticky host failures cannot turn
into success via a different layout or fallback. Cache keys include operation/
profile, shape, axis map, layout and exact witnessed input data/metadata. Cache-off
does not alter ownership, values or validation. No disk backing or private pool
is required and no RSS bound is claimed.

## Errors and acceptance

Malformed/missing shape, layout or axis map and total shape size above 2^40
use InvalidArgument at compile/direct preflight. Input dtype/rank, map length,
incompatible extents or a source dimension that would need dropping use
TypeMismatch. Duplicate/out-of-range map indices are InvalidArgument. Preserve
recognized semantic validation, upstream identity, cancellation/stale and
ResourceExhausted code/reason/scope. Generic floating NaN/Inf is not an error.

| Test | Independent expected behavior |
| --- | --- |
| B01 | `[10,20,30]` shape `[3]` -> `[2,3,4]`, map `[1]`: output `[i,j,k]` equals source[j]; only 3 unique source values are needed for the full result |
| B02 | Input `[2,3]` -> output `[3,4,2]`, map `[2,0]`: output `[j,k,i]` equals input[i,j] |
| B03 | Input `[1,3]` -> `[4,3]` succeeds; `[2,3]` -> `[4,3]` under the same map fails without tiling |
| B04 | Duplicate map, map length/range errors, rank shrink, empty/invalid shapes and product limits reject before data reads |
| B05 | Four dtypes, arbitrary NaN payloads, infinities, signed zeros and integer extrema match bitwise across view/dense and platform profiles |
| B06 | Negative/zero source strides, offset origin, unaligned storage and several source owners produce valid regional views |
| B07 | Nonzero/disjoint Q reads exact mapped support without bounding gaps; typed input additionally reads required validation closure |
| B08 | A changed source element invalidates every replicated output location and required typed-validation dependents, but not unrelated output locations |
| B09 | Empty request reads nothing; warm/cache-off, cancellation and failed allocation/work/stage paths retain correct witnesses and release unpublished owners |
| B10 | View retains source bytes after original/context destruction; final owner releases them; dense has correct independent packed payload |
| B11 | Large view versus insufficient dense budget, fragmented source/set budget exhaustion, and unsupported accelerated-platform behavior |

The public target workflow binds a small indexed integer matrix, creates the
selected broadcast key with shape/map/layout, compiles and requests full plus
nonzero/disjoint outputs. Check logical indices, strides/owners, actual source
reads and dirty support. Use the explicit coordinate formula as an independent
oracle. The eventual implementation supplies a runnable target and real command;
none is invented for these unimplemented keys.

## Implementation boundary

Zero-stride Value storage is already supported, but per-node shape String and
axis-map inference, general regional broadcast and profile registrations remain
target work. These need compiler-visible validation/inference and exact staged
input mappings, not hidden callback-only shape changes. This specification does
not accept a shared ABI extension or authorize product code changes. Platform
benchmarks and product integration tests have not been run for the new operators.

The Value runtime permits zero-stride views, but that capability does not by
itself establish a registered general broadcast operation.

- [NUM-03 category](../core.md).
- [NUM-03A constant](NUM-03A_constant.md).
- [Existing shape/region traits](../../../../include/photospider/plugin/operation_registry.hpp).
