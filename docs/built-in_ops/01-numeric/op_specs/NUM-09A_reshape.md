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
of physical input strides. Input `input` is a single-tensor Result with any
schema id/member key. Its complete `sample_shape()` has rank 1..8, positive
extents and at most 2^40 elements. The output `values` is a Result tensor with
the same dtype, UInt8, Int8, UInt16, Int16, Int64, Float32 or Float64. Every
element is copied or aliased bit-for-bit,
including signaling NaNs, payloads, infinities and signed zeros. No arithmetic,
implicit cast or NaN quieting occurs. All three CPU versions are bitwise equivalent.

Static String `shape` is a required canonical comma-separated list of positive
decimal extents, rank 1..8, with product equal to the input logical element count.
There is no -1 inference, zero extent, automatic axis insertion or order parameter.
Static String `layout` is auto/view/dense; constructors write auto by default,
and direct nodes specify it. Input/output logical element count is at most 2^40.
The published Result uses `photospider.tensor` v1/member `samples`; the target
shape is its complete ordinary-axis shape. Output facets and batch-axis metadata
are empty. Recognized input facets retain their actual-read validation
obligations.

## Exact coordinate mapping and support

Let input shape be I and target shape O. For an output coordinate o, define

k = sum_j o[j] * product_{m>j} O[m]
    s[j] = floor(k / product_{m>j} I[m]) mod I[j]

The mapping defines numerical selection; execution uses CPU Whole for all formal
profile keys. The compiler validates the static shape and schema before runtime.
On nonempty work, the program requests the source with a full Tensor Need using
Data, Validation and Descriptor roles (role 13), which triggers typed-payload
validation. It publishes the complete Result
in global output coordinates. A query `Q` limits observed dependencies and
downstream reads; it does not produce a packed ROI Result. Edits to observed
source support invalidate the recorded dependency. Empty output has empty
coverage and support. Source, typed, resource and domain failures remain visible
to the Run.

## Layout decision and lifetime

View requires one affine owner for the complete input and output. Compatible
fragments of the same owner may join after an address-map proof. Multiple owners
make explicit View unavailable. Auto materializes a complete packed output only
when a view is unavailable; Dense always materializes it. A sparse query
cannot make a globally non-affine View succeed. Negative and zero strides and
singleton axes remain legal. Reshape proves maximal contiguous source chunks
and target axis boundaries without enumerating elements. Published views retain
their source storage and resources after context retirement.

Views retain input storage/resources until final release, including oversized
source backings. New dense output owns N*dtype_size bytes, even for partial
demand. No writable alias or cross-owner address map is fabricated. Auto only
handles unavailable views; it never hides validation, budget or cancellation errors.

## Resources and errors

View mapping is O(rank) after the authorized input window is available; Dense is
O(N*rank) with bounded coordinate vectors (rank<=8). Same-owner fragment
joining is bounded by fragment count and rank and consumes host work. The
program reads authorized source windows and admits the complete packed output
capacity; it does not collect the complete source into a second input buffer.
Cancellation and work checks run during copied elements and before publication.
No partial failed output is published. Malformed schema is a compile/preflight
error. Layout availability is evaluated at Run time.

## Acceptance and current status

Conceptual fixture: input shape [2,3], logical elements [0,1,2,3,4,5], target
shape [3,2] -> [[0,1],[2,3],[4,5]]. An independent integer flatten/unflatten
oracle must agree for every selected coordinate. Include a physically transposed
input, reversed axes, zero strides, unaligned offsets and owner-fragmented sources.
Check whole versus regional logical equality and explicit view success/failure,
auto fallback and dense results without comparing incidental physical addresses.

All nine formal profile keys use Whole and disable cross-run content caching
because content alone does not establish physical owner/stride identity;
same-Run sharing remains available. The current Result workflow, oracle and
resource checks are in [NUM-09 Whole execution](../layouts-whole.md).
