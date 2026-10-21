---
spec_schema_version: 1
id: NUM-07H
parent_id: NUM-07
function: select
proposed_operation_keys:
  - numeric.select_strict
  - numeric.select_accelerated_apple_silicon
  - numeric.select_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07H: select

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Select an element from one of two arrays using a boolean-valued UInt8 condition.
Inputs in order are `condition`, `when_true`, `when_false`; all have the same
rank-1..8 positive shape. Both value branches share one dtype chosen from UInt8,
Int64, Float32 and Float64. Output `values` uses the branch dtype, unchanged
shape and no attached domain description. Metadata specialization derives output dtype from the value branches.
There are no static numeric parameters or implicit broadcasts/casts.

The caller supplies condition bytes exactly 0 or 1 at every computed coordinate. This is an unchecked numerical precondition; no error outcome is promised for other bytes. Zero selects `when_false`; one selects `when_true`. The kernel copies the selected element's exact bits, including NaN payload, signaling NaN, infinity and signed zero, without arithmetic quieting. All three CPU profiles are bitwise equivalent and require their named host capability.

## Fixed tasks and input dependencies

Metadata specialization checks UInt8 conditions, equal branch dtypes and equal complete shapes, including batch axes. Each requested output tile lowers to one CPU task. All three input ports declare every intersecting tile, using their own grids. Both branch owners remain dependencies even when every condition selects the same branch. Upstream operational failure in a declared branch can prevent issue. The kernel reads the selected branch sample and writes its raw bits into a separate preallocated dense output tile.

Prepare validates complete fixed slot coverage, each port's dtype and output layout. Empty selection emits no task or payload reads. Sparse requests compute only requested output tiles, without constructing a full internal output.

## Storage and resources

Inputs use the Plan's admitted immutable affine layouts, including signed strides and shifted origins. Output allocations contain only selected complete tiles. The kernel uses bounded stack coordinates and cached views, with zero task scratch and no payload allocation during invocation. Cancellation is queried at most every 1024 samples. Failures discard unpublished output after accepted callbacks return; successful Result owners remain valid after context destruction.

## Acceptance and implementation status

Conceptual fixture: condition=[1,0,1], when_true=[10,20,30], when_false=[1,2,3] produces [10,2,30]. Check all-true, all-false and alternating conditions, all four branch dtypes, distinct signaling-NaN payloads and signed zero. Include different batch/atomic input grids, signed strides, nonzero tile origins, sparse/empty selections, upstream failure on a declared branch, cancellation and retained output owners. Invalid condition bytes have no required diagnostic outcome.

All three profile definitions use fixed invocation ABI 8. Validate available profiles through temporary public execution checks. See [NUM-07 fixed execution](../comparison-execution.md); specification status remains Proposed.
