---
spec_schema_version: 1
id: NUM-10A
parent_id: NUM-10
function: concatenate
proposed_operation_keys:
  - array.concatenate_strict
  - array.concatenate_accelerated_apple_silicon
  - array.concatenate_accelerated_x86_64
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

# NUM-10A: concatenate

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Concatenate an ordered sequence of 2..256 input arrays along a static axis.
Inputs are named input_0 through input_(K-1), in that order. All share one dtype
(UInt8, Int64, Float32 or Float64) and rank 1..8, with positive extents and equal
lengths on every non-concatenation axis. Output `values` retains dtype/rank and
has empty facets. Concatenation-axis length is the checked sum of input lengths;
output logical element count must not exceed 2^40.

Required static Int64 `axis` is a nonnegative index less than rank, with no
negative-index shorthand. Required static String `layout` is view/dense;
constructors write view by default and direct nodes specify it. There are no
other numeric parameters, implicit casts, broadcasting or inserted axes.
Input metadata is validated for every port before execution even when that
port is not requested later. Element bits, including sNaNs, are preserved across
all three bitwise-equivalent CPU profiles; no arithmetic quieting is performed.

## Exact mapping and dependencies

Let L[k] be input k's axis length and P[k]=sum_{j<k} L[j], with P[0]=0.
For output o, select the unique k with P[k]<=o[axis]<P[k]+L[k], and read input k
at s[axis]=o[axis]-P[k], s[j]=o[j] otherwise. Prefix offsets depend only on static
metadata, not input samples. Positive axis lengths make this partition unique.

Every nonempty request reads and validates all inputs before callback, including
ports outside the consumer projection. Any source edit invalidates the complete
output; upstream/typed failures affect the Run. Empty reads nothing. Numerical
selection still uses the prefix mapping above and preserves raw bits.

## Storage and resources

View requires the complete output to fit one affine owner. All inputs must share
that storage and have compatible offsets/strides across prefix boundaries.
Compatible same-owner fragments may join; multiple owners or non-affine mapping
fail Domain/Run InvalidArgument/InvalidDomain with ViewUnavailable. Dense
collects as needed and owns N*dtype_size bytes for the complete output, including
for sparse demand. The helper still defaults to View; independent allocations
should select Dense explicitly. No writable or fabricated cross-owner view is
published. Views retain the complete source backing/resources.

Prefix storage is bounded by256 ports; coordinate state by rank8. Dense mapping
costs O(N*(rank+log K)) with raw block copies. Host work/cancellation checks occur
per output and before publication. Collected full inputs and full output are
charged. Concatenate is cacheable=false because content identity cannot witness
physical View availability. Budget/upstream failures never trigger a fallback.

## Errors, acceptance and implementation gaps

Compile/preflight rejects input count, dtype/rank/non-axis shape mismatch,
invalid axis/layout, sum/product overflow or output size above 2^40. Runtime
upstream/typed-validation/resource/cancellation errors retain their existing
Status categories. Failure publishes no partial output for that observation.

Conceptual fixture: A=[[1,2],[3,4]], B=[[5],[6]], axis=1 produces
[[1,2,5],[3,4,6]]. Independent prefix-coordinate mapping and raw-bit copying
are the oracle. A request solely in B's output slab still reads A and fails if A's
upstream fails. Cover slab-crossing/disjoint requests, all dtypes/sNaN bits,
non-leading axes, owner fragmentation, negative strides, repeated use of the same
input object, metadata cap, view/dense equality, source invalidation, cancellation
and output lifetime after context destruction. The public WorkflowDocument manual target and independent oracle below
provide executable acceptance for these behavior boundaries.

All formal profile keys use CPU Whole. Current public workflows, independent
coordinate/contributor/Fraction oracles, failure/resource checks and performance
are in [NUM-10 Whole execution](../indexing-whole.md). Earlier 2026-09-14
regional strict/Apple/WSL and installed checks predate this migration.
