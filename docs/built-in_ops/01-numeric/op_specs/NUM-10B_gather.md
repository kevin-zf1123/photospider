---
spec_schema_version: 1
id: NUM-10B
parent_id: NUM-10
function: gather
proposed_operation_keys:
  - array.gather_strict
  - array.gather_accelerated_apple_silicon
  - array.gather_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-10B: gather

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Gather slices of `input` along one required static Int64 `axis`, using dynamic
Int64 `indices` of shape [M]. Axis is a nonnegative index less than input rank.
Output `values` has input rank and shape except output.shape[axis]=M. Indices
may repeat and their order determines output slice order. This is a single-axis
gather, not multidimensional coordinate-tuple gather_nd.

At output o, source s[j]=o[j] for j!=axis and s[axis]=indices[o[axis]]. Index
shape is static metadata while index values may change between executions.
Input/output dtype is UInt8, Int64, Float32 or Float64 and element bits are
preserved, including sNaN payloads and signed zeros. Output facets are empty;
actual-read typed input validation remains required. Shapes are positive rank
1..8, with input/output logical count bounded by the NUM array limit 2^40 and
1<=M<=2^40 subject to that output limit. No implicit broadcast, cast, rank
squeezing, negative-axis shorthand or static layout parameter is provided.
Output is dense per requested rectangle in every profile.

## Staged demand and exact support

For requested Q, let J be the set of axis-coordinate values occurring in Q.
Read indices exactly on J as retained Control support and require every observed
index in [0,input.shape[axis]). Negative indices, clipping and wrap are not
supported. Unrequested indices are not checked and do not cause failure. Empty
Q reads neither input nor indices.

After index validation, compute the exact source Data set S(Q) by the mapping
above; deduplicate identical complete source coordinates. Repeated output slices
reuse those reads. Distinct rows sharing the same axis index are still distinct
source coordinates. Add required typed-validation closure separately; never
replace irregular source sets with bounding gaps or Whole reads silently.

A change to an observed index invalidates and replans the output coordinates
using it. A change to source Data invalidates every witnessed output that maps
to it, including duplicates. Retain index/source/validation witnesses and include
metadata, axis/profile and relevant dependencies in cache identity.

## Storage, resources and errors

Read legal immutable source/control layouts through checked addresses. Copy raw
bits into owned packed output fragments with correct global Region/storage
origins. Result owners can outlive execution context; no writable alias, hidden
zero or unrequested SIMD tail read is permitted. Profiles are bitwise equivalent;
platform-specific names reject incompatible platforms.

For Mq requested elements and Jq distinct used indices, mapping is O(Mq*r+Jq),
with actual dedup/set work charged separately. Account requested output payload,
indices/control witnesses, source dedup metadata, source/validation owners and
scratch. Allocation does not scale with the input bounding interval merely
because indices are far apart. Large irregular sets may fail ResourceExhausted;
never widen support to evade a budget. Poll cancellation per stage/mapping block
and at least every 4096 copied elements. Release unpublished state on failure.

Compile/preflight rejects dtype/rank/shape/axis violations and output count above
the array cap. An observed invalid index fails with InvalidArgument,
FailureReason::InvalidDomain and diagnostic IndexOutOfBounds, reporting the
index-array position, actual index and source extent. No partial failed
observation is published. Source/typed/resource/cancellation failures retain
existing categories and inherited [array ownership rules](NUM-09A_reshape.md).

## Acceptance and implementation status

Conceptual fixture: input=[[10,11,12],[20,21,22]], indices=[2,0,2], axis=1
produces [[12,10,12],[22,20,22]]. Use independent integer mapping and raw-bit
comparison, plus source-read logs proving distinct-source dedup and precise index
support. A request only at output axis position 0 must not read an invalid index
at position 1. Include both extreme legal indices, duplicates, non-leading axes,
index/source strides, sNaN bits, disjoint requests, invalidation after index
changes, resource exhaustion, cancellation, cache-off and owner lifetime.

Deliver public WorkflowDocument execution when implemented. No versioned runtime
implementation or test is claimed by this specification.
