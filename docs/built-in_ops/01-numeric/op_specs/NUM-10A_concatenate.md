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
---

# NUM-10A: concatenate

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

Intersect requested Q with each input's output slab and translate to exact
source Data support. Unhit input ports have empty support and are not evaluated.
Empty Q reads no input. Retain recognized typed-input validation closure separately
for each hit port; no numeric scan of unhit input data is added. Changed source
Data maps forward by its prefix offset; changed retained validation support
invalidates the observations that used it. Cache identity includes input order,
metadata, axis/layout/profile and actual Data/Validation witnesses.

## Storage and resources

View mode publishes immutable fragments referencing the corresponding input
owners, adjusting global output origins and byte offsets while preserving source
strides. Multiple owners in one output request are explicitly allowed; this
operator does not inherit reshape's single-view-per-request-rectangle constraint.
Fragment boundaries include slab/owner boundaries as needed, with exact output
coverage and no writable aliases, missing cells or fabricated cross-owner buffer.
A view can retain owners much larger than the requested element bytes.

Dense mode copies each requested output rectangle into owned packed row-major
storage. Read only its translated supports, through valid arbitrary source
strides/offsets including negative/zero strides and unaligned values. Neither
mode evaluates unhit ports merely to assemble a convenient Whole result.

Prefix metadata is O(K), and demand splitting is proportional to intersected
slabs/fragments. Dense data work is O(M*r) for M requested output elements;
views require layout/owner metadata, not dense M-element copying. Charge actual
prefix/split metadata, source owners, validation work, scratch and output bytes.
Fragmentation may grow with upstream owners and disjoint demands; exceeding host
limits yields ResourceExhausted, without broadening reads. Reserve before
allocation, check cancellation per mapping block and at least every 4096 copied
elements, and publish only complete successful observations. Final-owner release,
cache-off behavior and floating bit transport follow
[broadcast's copy/view contract](NUM-03B_broadcast.md).

## Errors, acceptance and implementation gaps

Compile/preflight rejects input count, dtype/rank/non-axis shape mismatch,
invalid axis/layout, sum/product overflow or output size above 2^40. Runtime
upstream/typed-validation/resource/cancellation errors retain their existing
Status categories. Failure publishes no partial output for that observation.

Conceptual fixture: A=[[1,2],[3,4]], B=[[5],[6]], axis=1 produces
[[1,2,5],[3,4,6]]. Independent prefix-coordinate mapping and raw-bit copying
are the oracle. A request solely in B's output slab must not read A, even if A's
upstream would fail. Cover slab-crossing/disjoint requests, all dtypes/sNaN bits,
non-leading axes, owner fragmentation, negative strides, repeated use of the same
input object, metadata cap, view/dense equality, source invalidation, cancellation
and output lifetime after context destruction. The public WorkflowDocument manual target and independent oracle below
provide executable acceptance for these behavior boundaries.

The current three profile keys use the public `concatenate_node` helper in
`photospider/numeric/indexing.hpp`. Per-node specialization validates the
non-axis shape relation and axis-length sum. Static dependency pieces partition
the output into disjoint slabs, with `DependencyAxis::translation` applied per piece;
`repeated_match=false` is permitted only with the metadata specializer.

The manual indexing workflow covers slab-crossing/disjoint requests, view and
dense output, exact support, owner fragmentation, typed validation, raw bit
transport and resource/cancellation paths. On 2026-09-14, local AppleClang 21 strict/Apple and Ubuntu WSL Clang 18
strict/AVX2 passed the complete manual workflows and 3858 independent
coordinate/contributor/Fraction cases per profile. The installed public consumer
passed. Checks include exact reads and dirty support, typed actual-read closure,
raw/quiet NaN and zero rules, strided input, four fenv modes, changed-index cache
replanning, Empty, cancellation, work/state limits and failed-attempt diagnostics.
Focused compiler/dependency/fragments/resources units and independent scoped
reviews passed. Manual acceptance has no integration-test registration.
Specification status remains Proposed; no performance claim is inferred.
