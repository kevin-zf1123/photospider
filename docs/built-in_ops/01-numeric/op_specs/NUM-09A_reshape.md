---
spec_schema_version: 1
id: NUM-09A
parent_id: NUM-09
function: reshape
proposed_operation_keys:
  - array.reshape_strict
  - array.reshape_accelerated_apple_silicon
  - array.reshape_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-09A: reshape

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Change shape while preserving the row-major logical element sequence, regardless
of physical input strides. Input `input` and output `values` have the same dtype,
UInt8, Int64, Float32 or Float64. Every element is copied or aliased bit-for-bit,
including signaling NaNs, payloads, infinities and signed zeros. No arithmetic,
implicit cast or NaN quieting occurs. All three CPU versions are bitwise equivalent.

Static String `shape` is a required canonical comma-separated list of positive
decimal extents, rank 1..8, with product equal to the input logical element count.
There is no -1 inference, zero extent, automatic axis insertion or order parameter.
Static String `layout` is auto/view/dense; constructors write auto by default,
and direct nodes specify it. Input rank is also 1..8 with positive extents.
Input/output logical element count is at most 2^40. Output facets are empty;
no image-axis, color or other semantic metadata is inferred for the new shape.
Recognized input facets retain their actual-read validation obligations.

## Exact coordinate mapping and support

Let input shape be I and target shape O. For an output coordinate o, define

    k = sum_j o[j] * product_{m>j} O[m]
    s[j] = floor(k / product_{m>j} I[m]) mod I[j]

Read input exactly at s. This is a bijection on the full logical element domain.
Use checked integer indexing and shape products, never floating point. For
requested Q, Data support is the exact mapped set S(Q); reshaping a rectangle
can require multiple input rectangles or runs. Add recognized typed-input
Validation closure separately. Empty Q reads nothing. No source bounding gap,
Whole input evaluation or implicit missing value is introduced by this operator.

Inverse flatten/unflatten maps changed input Data to output invalidation exactly;
retained typed-validation witnesses add their required invalidation. Shape/layout,
dtype and witnessed source metadata/data belong to cache identity. Region and
storage origins always use global logical coordinates, independent of byte offset.

## Layout decision and lifetime

Use the executor's normalized requested rectangles as the units of layout
decision. For each such rectangle, a view requires one input backing owner,
one valid byte offset and one stride vector that represent all mapped elements
in that rectangle. Validate address bounds with signed strides. Negative and
zero strides are legal when the full address mapping satisfies this condition.
Do not subdivide the rectangle solely to manufacture a collection of tiny views.

- view: if the condition holds, publish an immutable owning view; otherwise
  fail the requested observation with InvalidArgument and diagnostic ViewUnavailable.
- auto: use that view when possible; otherwise copy the rectangle into owned
  packed row-major storage. Different requested rectangles may choose differently.
- dense: always copy the requested rectangle into owned packed row-major storage.

A small regional request can be viewable even when the whole output is not;
viewability concerns the actual requested rectangle and available source storage.
This does not change logical values. Auto fallback must not erase upstream,
validation, resource or cancellation failures. It reacts only to an unrepresentable
view mapping. Report actual view/copied elements through host diagnostics without
adding output ports. Output descriptors remain static, independent of the choice.

Retain all actual source owners referenced by views; a tiny view may retain a
large allocation. Copied outputs own new bytes, with upstream retention governed
by execution/cache requirements. Values remain valid after context destruction
until final-owner release. Never expose writable aliases, fabricate contiguous
storage across owners or authorize unrequested source coordinates.

## Resources and errors

Coordinate mapping costs O(M*r) for M requested elements and r<=8, or less with
proved run/coalescing logic. Account exact source-set decomposition, temporary
index/run metadata, owner retention and output fragments. Dense output payload
is M*dtype_size; view adds metadata and retained owners without a dense payload.
Reserve actual capacities before allocation. Do not reserve the full logical
array for a partial request. Excessive mapping complexity or metadata capacity
fails ResourceExhausted rather than widening source reads.

Check cancellation before source reads, between mapping blocks, at least every
4096 copied elements and before publication. Failed observations publish no
partial output; release unpublished state and preserve prior terminal observations.
Malformed shape/layout, product mismatch, invalid rank/extents and index overflow
fail compile/preflight; physical view availability is checked when source layout
is available. Dtype/schema, upstream, typed-validation and resource errors retain
the host's existing categories.

## Acceptance and current status

Conceptual fixture: input shape [2,3], logical elements [0,1,2,3,4,5], target
shape [3,2] -> [[0,1],[2,3],[4,5]]. An independent integer flatten/unflatten
oracle must agree for every selected coordinate. Include a physically transposed
input, reversed axes, zero strides, unaligned offsets and owner-fragmented sources.
Check whole versus regional logical equality and explicit view success/failure,
auto fallback and dense results without comparing incidental physical addresses.

The current nine `array.*` keys use per-node shape/permutation/count
parameters and the public `reshape_node` helper in
`photospider/numeric/layouts.hpp`. `TransformLayout::Auto` chooses a
per-request-rectangle view when one affine owner can represent it and otherwise
packs; `View` reports `ViewUnavailable`, while `Dense` always packs. Direct
bindings remain whole dense values; a non-contiguous workflow input is produced
by the public `transpose_node`, and direct `OperationRegistry::invoke` can
exercise a strided `Value`.

On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed the public manual examples and 636 independent integer/raw-bit
oracle cases per profile. The installed public consumer passed. Coverage includes
exact support/dirty mapping, whole versus regional layout policy, unaligned and
negative/zero strides, shared versus independent owners, ignored singleton steps,
full slice endpoint validation, typed Validation closures, schema/Empty behavior,
work/cancellation/capacity failures, fenv and escaped Value lifetime. Focused
compiler/dependency/fragments/resources units and independent scoped review passed.
Layout operations are `cacheable=false` because the content cache does not witness
physical owner/stride partitions. Managed metadata and its remaining host-container
boundaries are documented in [Managed Resources](../../../kernel-architecture/Managed-Resources.md).
The manual target is not registered in integration tests. Specification status
remains Proposed; no performance claim follows from correctness checks.
