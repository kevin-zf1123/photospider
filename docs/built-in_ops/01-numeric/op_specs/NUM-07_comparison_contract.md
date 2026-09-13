---
spec_schema_version: 1
id: NUM-07-comparison
parent_id: NUM-07
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-07: exact comparisons

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Provide six independent operations: equal, not_equal, less, less_equal, greater
and greater_equal. Each has strict, accelerated_apple_silicon and
accelerated_x86_64 keys under numeric, with bitwise identical results.
There is no comparison-mode parameter.

Inputs `a` and `b` must have identical dtype and shape. Support UInt8, Int64,
Float32 and Float64. Output `values` has the same shape and dtype UInt8,
representing false by 0 and true by 1. Output facets are empty; comparison
results do not implicitly acquire image mask, alpha or color semantics.
Use explicit cast/broadcast for type and shape adaptation.

Inherit [binary execution requirements](NUM-05_binary_contract.md) for exact
per-port Q demand, typed-validation closure, invalidation, strides, returned
fragments, immutable ownership, resources, cancellation, errors and public
acceptance. The explicit exception is output dtype UInt8 for all input dtypes.
Both inputs are read even when one value determines the result. Shape is rank
1..8 with positive extents. No input integer is converted to float for comparison.

Floating comparisons use numerical equality/order. Signed zeros compare equal;
same-sign infinities compare equal and infinities otherwise follow the ordinary
extended order. If either input is NaN, not_equal yields 1 and each other
predicate yields 0. This applies to quiet and signaling NaNs regardless of their
payloads. Bit-level classification prevents leaking floating traps/flags. No NaN
payload is propagated into the UInt8 result; typed input validation still applies.

All valid generic numerical inputs produce a successful comparison. Integer
comparison and floating classification introduce no arithmetic overflow or
rounding error. Invalid type/shape is a compile/preflight error; resource,
cancellation, upstream and typed-semantic failures retain shared Status handling.

Output storage costs one byte per requested result plus fragment overhead; work
is O(requested elements), with bounded actual processing scratch. Integer extrema
and values beyond 2^53 require exact comparison. Accept using independently
specified relations, all signed-zero/Inf/NaN combinations, different payloads,
strided/disjoint demands and shared resource/ownership checks.

## Individual predicates

| Spec | Predicate |
| --- | --- |
| [NUM-07A equal](NUM-07A_equal.md) | `a == b` |
| [NUM-07B not_equal](NUM-07B_not_equal.md) | `a != b` |
| [NUM-07C less](NUM-07C_less.md) | `a < b` |
| [NUM-07D less_equal](NUM-07D_less_equal.md) | `a <= b` |
| [NUM-07E greater](NUM-07E_greater.md) | `a > b` |
| [NUM-07F greater_equal](NUM-07F_greater_equal.md) | `a >= b` |

No target runtime implementation or test is claimed.
