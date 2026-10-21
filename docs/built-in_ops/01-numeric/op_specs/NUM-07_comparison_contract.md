---
spec_schema_version: 1
id: NUM-07-comparison
parent_id: NUM-07
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07: exact comparisons

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Provide six independent operations: equal, not_equal, less, less_equal, greater
and greater_equal. Each has strict, accelerated_apple_silicon and
accelerated_x86_64 keys under numeric, with bitwise identical results.
There is no comparison-mode parameter.

Inputs `a` and `b` must have identical dtype and shape. Support UInt8, Int64,
Float32 and Float64. Output `values` has the same shape and dtype UInt8,
representing false by 0 and true by 1. Output carries no domain description; comparison
results do not implicitly acquire image mask, alpha or color semantics.
Use explicit cast/broadcast for type and shape adaptation.

Each input Result contains one tensor member with any valid key. Complete sample shape includes batch axes, has rank 1..8, positive extents and at most 2^40 samples. The output uses schema `photospider.tensor` v1, member `samples`, and the complete shape as ordinary axes. No input integer is converted to float for comparison.

The fixed execution contract below takes precedence over inherited complete-input descriptions. Each requested output tile becomes one CPU task with the intersecting tiles of both inputs as fixed reads, even when classification determines the result from one sample. Inputs may have different batch or atomic-axis grids. Prepare validates the exact per-port dtype, read slots, output region and scratch reservation. Empty selection emits no task and requires no input payload.

Floating comparisons use numerical equality/order. Signed zeros compare equal;
same-sign infinities compare equal and infinities otherwise follow the ordinary
extended order. If either input is NaN, not_equal yields 1 and each other
predicate yields 0. This applies to quiet and signaling NaNs regardless of their
payloads. Bit-level classification prevents leaking floating traps/flags. No NaN
payload is propagated into the UInt8 result. Structural input validation occurs before issue.

All valid generic numerical inputs produce a successful comparison. Integer
comparison and floating classification introduce no arithmetic overflow or
rounding error. Invalid type/shape is a compile/preflight error; resource,
cancellation and upstream failures retain operational Status handling.

Each selected output tile has a separate dense allocation containing one byte per sample. Comparisons use a fixed `ComparisonMath` workspace in preallocated Host scratch for each active worker lane. Work is proportional to selected output samples; the invocation processes at most four samples between cancellation queries. All mutable arithmetic state belongs to the invocation, while prepared grids are immutable across Runs. Successful Result owners survive the execution context.

Acceptance uses independent integer relations and IEEE classification. Cover integer extrema beyond 2^53, signed zeros, infinities, signaling and quiet NaNs, different input grids, signed strides, sparse requests, cancellation, malformed invocation signatures and retained output ownership.

## Individual predicates

| Spec | Predicate |
| --- | --- |
| [NUM-07A equal](NUM-07A_equal.md) | `a == b` |
| [NUM-07B not_equal](NUM-07B_not_equal.md) | `a != b` |
| [NUM-07C less](NUM-07C_less.md) | `a < b` |
| [NUM-07D less_equal](NUM-07D_less_equal.md) | `a <= b` |
| [NUM-07E greater](NUM-07E_greater.md) | `a > b` |
| [NUM-07F greater_equal](NUM-07F_greater_equal.md) | `a >= b` |

## Current implementation status

The six predicates, `is_close` and `select` each provide three fixed CPU profile keys through invocation ABI 8. Metadata specialization checks shape/dtype and `is_close` tolerances before execution. Comparison kernels use raw IEEE classification and integer order keys. `is_close` uses bounded exact dyadic arithmetic. `select` copies the chosen branch's raw bits and requires caller-supplied 0/1 conditions.

Profile availability is checked before issue. See [NUM-07 fixed execution](../comparison-execution.md). Specification status remains Proposed; registration and source availability do not establish cross-platform runtime validation or performance.
