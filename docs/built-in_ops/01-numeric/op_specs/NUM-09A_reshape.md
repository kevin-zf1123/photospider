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

The mapping defines numerical selection; execution uses CPU Whole for all formal
profile keys. Every nonempty request collects and validates the complete active
input and computes the complete output before projection. Empty reads nothing.
Every active source edit invalidates the complete output, and any source, typed,
resource or domain failure affects the Run. Output key/shape/dtype and tuple
identity are unchanged; legacy unsuffixed keys are separate implementations.

## Layout decision and lifetime

View requires one affine owner for the complete input and complete output.
Compatible fragments of the same owner may be joined after proving their address
maps. Multiple owners fail Domain/Run InvalidArgument/InvalidDomain with
ViewUnavailable. Auto may collect multiple owners and falls back to one complete
packed output when reshape is not affine. Dense always copies the complete
output. A sparse request cannot make a globally non-affine View succeed.
Negative/zero strides and singleton axes remain legal. Reshape proves maximal
contiguous source chunks and target axis boundaries without enumerating pixels.

Views retain input storage/resources until final release, including oversized
source backings. New dense output owns N*dtype_size bytes, even for partial
demand. No writable alias or cross-owner address map is fabricated. Auto only
handles unavailable views; it never hides validation, budget or cancellation errors.

## Resources and errors

View mapping is O(rank) after input preparation; Dense is O(N*rank) with a fixed
state and bounded coordinate vectors (rank<=8). Same-owner fragment joining is
bounded by fragment count and rank and consumes host work. Collecting inputs may
own their complete packed payloads. The fixed state and complete output capacity
are admitted; cancellation/work checks occur per copied element and before
publication. No partial failed output is published. Malformed schema remains a
compile/preflight error. Layout availability is evaluated at Run time.

## Acceptance and current status

Conceptual fixture: input shape [2,3], logical elements [0,1,2,3,4,5], target
shape [3,2] -> [[0,1],[2,3],[4,5]]. An independent integer flatten/unflatten
oracle must agree for every selected coordinate. Include a physically transposed
input, reversed axes, zero strides, unaligned offsets and owner-fragmented sources.
Check whole versus regional logical equality and explicit view success/failure,
auto fallback and dense results without comparing incidental physical addresses.

All nine formal profile keys use Whole. Layout operations remain cacheable=false
because content caches cannot witness physical owner/stride partitions. Current
workflow, independent oracle, resource validation and measured performance are in
[NUM-09 Whole execution](../layouts-whole.md). Earlier 2026-09-14 regional
strict/Apple/WSL checks predate this implementation and are not Whole acceptance.
