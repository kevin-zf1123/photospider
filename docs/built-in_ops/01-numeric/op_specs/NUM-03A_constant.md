---
spec_schema_version: 1
id: NUM-03A
parent_id: NUM-03
function: constant
operation_family: numeric.constant
proposed_operation_keys:
  - numeric.constant_strict
  - numeric.constant_accelerated_apple_silicon
  - numeric.constant_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-03A: constant

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The maintainer selected constant as a separate operator from NUM-03B broadcast.
It consumes one scalar and fills a specified output shape. The three independently
named strict/CPU-accelerated versions follow the common NUM bit-equivalence rule.
Input `value` is a dynamic scalar Value of shape `[1]`, with UInt8, Int64,
Float32 or Float64 dtype. Output preserves the input dtype and every scalar bit,
including floating NaN payloads, infinities and signed zero. No arithmetic or
implicit conversion is performed. This draft does not authorize implementation
or ADR changes.

Target shape is a static parameter with rank 1..8 and positive extents. Empty
axes and runtime shape changes are outside this version. Dtype is inferred from
the scalar input; each node's output descriptor is known at compilation.
The product of all extents must be at most 2^40, checked without integer overflow.

The static `layout` choice is `view` or `dense`, defaulting to view. View output
has zero stride on all axes and only one immutable scalar backing the logical
array. Dense output copies the scalar into packed storage for the requested
region. Neither layout changes the logical dtype or any element bits.

## Static parameters and outputs

The node has one input, `value`, and one named output, `values`. Shape is encoded
as a required canonical comma-separated decimal String, for example `"480,640,4"`.
There are 1..8 positive extents, no empty entries, signs or implicit dimensions,
and no inferred element count. The authoring constructor accepts a shape vector
and writes this canonical parameter. Required String `layout` is `view` or
`dense`; the constructor writes `view` by default. Direct nodes supply both.
There is no output dtype parameter because it is exactly the input dtype.

Output is a generic Value with empty facets. Valid input metadata is checked by
the existing generic port/recognized-facet rules. Typed inputs retain their
normal semantic validation obligations on the read scalar; invalid typed NaN
cannot bypass them by being copied. Unfaceted numeric inputs preserve every
bit, including signaling/quiet NaN payloads, without a floating conversion,
arithmetic instruction or isfinite-based rejection. Logical contents are always
`values[index] = bitcopy(value[0])`.

## Demand and representation

Every nonempty request reads scalar index zero once per admitted computation;
empty demand reads nothing. Output shape/dtype are statically known. There is
no output-coordinate-dependent source lookup or numeric failure. Input changes
invalidate the complete logical output; retain its exact scalar and transitive
validation witness. Shape/layout changes produce a new compiled identity.

The default view path can return a Whole logical Value backed by one owned
scalar-sized copy, with byte offset zero, origin zero and all byte strides zero.
It does not allocate or enumerate the logical element count. Any requested
subregion addresses the same copied bits. Full logical coverage on this path
does not mean dense execution or dense storage, and the output must not claim
`requires_dense_output`. The scalar-sized owned copy avoids requiring retention
of an arbitrarily large source allocation solely because the input was a view.

Dense mode uses per-region dependency execution and copies the scalar bits into
packed requested fragments, preserving their global Regions/storage origins.
A full dense output enumerates and stores every logical element; a sparse request
does not allocate the intervening bounding-box gap. No implicit fill outside
the published coverage is used. A consumer that requires contiguous full storage
must explicitly request dense output and have sufficient resources.

Both layouts are immutable. Do not expose a writable alias, and do not assume
that backing byte length equals shape product for view output. Published owners
survive the invocation/context and release storage at the final owner. Source
views with offsets, unaligned data, negative/zero strides are read by their
validated logical scalar address. Unpublished allocations are released on error.

## Execution, budgets and numerical versions

All three operation keys are bitwise equivalent in logical elements. The selected
layout contract also applies to all three; acceleration cannot quiet NaNs,
canonicalize zeros, clamp infinities or change dtype. The view path only needs
a byte copy; accelerated dense paths can use vectorized repeated-byte/element
fills with no floating arithmetic and no out-of-coverage stores. There is no
need for a numerical tolerance or approximate fallback. Incompatible platform
keys return BackendUnavailable rather than silently switching keys.

For element size b, view output payload is b bytes plus O(rank) descriptor/layout
metadata. Dense output payload is b*M for M requested elements. The maximum
logical byte count is 2^40*b, but is not a reservation in view mode. Scalar
transport, descriptors, fragments, certificates and retained upstream owners
remain subject to actual host resource accounting. View work is O(rank+b);
dense work is O(M*b). Fragment/scheduling overhead is separately charged.

Reserve before allocation, use existing host workers and work/admission limits,
and check all byte/count products before dense allocation. Poll cancellation
before reading, before publication and at least every 4096 copied elements or
smaller admitted fill block. Resource failures stay sticky and do not silently
switch a requested dense result to view. Cache identity includes key/profile,
shape, layout, input metadata and all scalar bits (including NaN payload/sign).
Cache-off changes neither active owner lifetime nor representation semantics.
No disk backing, private thread pool or mutable global scalar cache is required.

## Errors and acceptance

Bad/missing shape/layout parameters or total elements above 2^40 use
InvalidArgument during compile/direct preflight. Unsupported input dtype or
shape other than `[1]` uses TypeMismatch. Input metadata/typed validation retains
existing statuses. Generic NaN/Inf does not constitute an error. Preserve
upstream source identity, cancellation/stale errors and ResourceExhausted
capacity/work/stage reasons; no partial failed array is reported as success.

| Test | Expected behavior |
| --- | --- |
| C01 | Int64 `[7]`, shape `[2,3]`: six logical sevens; view stores one 8-byte scalar with zero strides |
| C02 | Same fixture with dense layout: packed 48-byte result; nonzero ROI only fills its requested coverage |
| C03 | Exhaustive UInt8 values, Int64 extrema, Float32/64 ±0, infinities and multiple NaN payloads preserve exact bits |
| C04 | Output dtype matches the scalar; all output facets are empty; recognized invalid typed input is still rejected |
| C05 | Rank 1 and 8, singleton axes, product exactly 2^40 and one above; zero/negative/malformed shape parameters reject |
| C06 | Large view succeeds within scalar-sized payload budget; full dense request fails an insufficient budget without fallback to view |
| C07 | Changed scalar bits invalidate all output; identical numeric NaNs with different payloads do not share the wrong cached bits |
| C08 | Strided/offset source scalar, context destruction with live output, final-owner release and no retained oversized source backing through the output copy |
| C09 | Empty demand performs no input read; view/dense and three platform keys agree logically; cancellation and resource failure release unpublished buffers |

The target public workflow declares a scalar binding, creates the selected
constant key with shape/layout, compiles, executes and inspects logical values,
byte strides and owned storage sizes. Repeat with dense and a nonzero region;
do not test a view by assuming its raw bytes contain a dense repeated array.
Implementation delivery provides real build/run commands and measured results.

## Current implementation gaps

The existing Value representation and fixed-shape C++ embedding tests already
support zero-stride arrays. However, current Fixed traits store shape in an
operation definition; they do not by themselves parse a different static shape
String at every node. The target needs compiler-visible per-node shape inference
and layout-dependent Whole/view versus regional/dense planning. Resolve these
through a reviewed implementation contract, without claiming a currently
implemented generic shape parser or silently registering shape-specific aliases.

This session read the Value and registry tests; no product implementation,
execution test or CPU benchmark was performed for these new keys.

The current `core.constant` is a static Float64 scalar producer; `field.constant`
is a static Float32/Float64 rank-2 field generator. Neither provides this general
dynamic-scalar-to-array interface.

- [NUM-03 category](../core.md).
- [Existing scalar producer](../../../../plugins/ops/00-foundation/core_constant.cpp).
- [Existing field producer](../../../../plugins/ops/03-generation/field_constant.cpp).
- [Operator template](../../00-foundation/spec-template.md).
