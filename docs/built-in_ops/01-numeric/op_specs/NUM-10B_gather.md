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
implementation_status: implemented_manual_acceptance
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-10B: gather

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
Output is complete dense storage in every profile.

## Whole demand and support

For every nonempty demand, collect and validate the complete source and complete
index vector. Every index must lie in [0,input.shape[axis]); any invalid index
fails Domain/Run even if its output coordinate was not requested. Empty reads
nothing. All source/index edits invalidate the complete output. Repeated indices
still select identical raw source values in their original output order.

## Storage, resources and errors

The complete output is packed and owned, costing N*dtype_size bytes; complete
source/index collection may add their full payloads. Index plan metadata is16*M
bytes (one key/position pair per entry), with no source-set dedup descriptors.
Plan lookup is direct by output-axis position; mapped source coordinates are
reused in bounded rank8 vectors. Work is O(M+N*rank), with cancellation/work
checks during index scan and each output copy. Capacity, typed or upstream
failures remain failures and publish no partial result. Arbitrary legal input
strides/origins preserve raw bits, including unaligned samples and sNaNs.

Compile/preflight rejects dtype/rank/shape/axis violations and output count above
the array cap. Any invalid index fails with InvalidArgument,
FailureReason::InvalidDomain and diagnostic IndexOutOfBounds, reporting the
index-array position, actual index and source extent. No partial failed
observation is published. Source/typed/resource/cancellation failures retain
existing categories and inherited [array ownership rules](NUM-09A_reshape.md).

## Acceptance and implementation status

Conceptual fixture: input=[[10,11,12],[20,21,22]], indices=[2,0,2], axis=1
produces [[12,10,12],[22,20,22]]. Use independent integer mapping and raw-bit
comparison, plus source-read logs proving distinct-source dedup and precise index
support. A request only at output axis position 0 must still reject an invalid index
at position 1. Include both extreme legal indices, duplicates, non-leading axes,
index/source strides, sNaN bits, disjoint requests, invalidation after index
changes, resource exhaustion, cancellation, cache-off and owner lifetime.

All formal profile keys use CPU Whole. Current public workflows, independent
coordinate/contributor/Fraction oracles, failure/resource checks and performance
are in [NUM-10 Whole execution](../indexing-whole.md). Earlier 2026-09-14
regional strict/Apple/WSL and installed checks predate this migration.
