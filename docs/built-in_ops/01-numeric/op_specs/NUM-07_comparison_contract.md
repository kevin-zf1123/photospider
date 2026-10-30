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

Numeric profile: strict is the exact reference defined by each operation. Discrete results are exact and bitwise identical in every profile.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and shared requirements; explicit rules below and in the named family contract take precedence.

## Interface

Six independent operations: equal, not_equal, less, less_equal, greater and greater_equal. Each has strict, accelerated_apple_silicon and accelerated_x86_64 keys under numeric, with bitwise identical results. There is no comparison-mode parameter.

Inputs `a` and `b` have identical dtype and shape; supported dtypes are UInt8, Int64, Float32 and Float64. Output `values` has the same shape and dtype UInt8, with false as 0 and true as 1. The output carries no domain description; results do not acquire image mask, alpha or color semantics. Use explicit cast/broadcast for type and shape adaptation.

Each input Result contains one tensor member with any valid key. Complete sample shape includes batch axes, has rank 1..8, positive extents and at most 2^40 samples. The output uses schema `photospider.tensor` v1, member `samples`, and the complete shape as ordinary axes. Invalid type or shape is a compile/preflight error.

## Mathematics

Integers compare by integer order; no input integer is converted to float. Floating comparisons use numerical equality and order: signed zeros compare equal, same-sign infinities compare equal and infinities otherwise follow the extended order. If either input is NaN, not_equal yields 1 and each other predicate yields 0, for quiet and signaling NaNs regardless of payload. No NaN payload is propagated into the UInt8 result. No floating trap or flag is raised.

## Behavior

Each output sample depends on the samples of both inputs at the same coordinate, even when classification of one sample determines the result. Inputs may have different batch or atomic-axis grids. All valid numerical inputs produce a successful comparison; integer comparison and floating classification introduce no overflow or rounding.

## Acceptance

Independent integer relations and IEEE classification. Cases: integer extrema beyond 2^53, signed zeros, infinities, signaling and quiet NaNs.

## Individual predicates

| Spec | Predicate |
| --- | --- |
| [NUM-07A equal](NUM-07A_equal.md) | `a == b` |
| [NUM-07B not_equal](NUM-07B_not_equal.md) | `a != b` |
| [NUM-07C less](NUM-07C_less.md) | `a < b` |
| [NUM-07D less_equal](NUM-07D_less_equal.md) | `a <= b` |
| [NUM-07E greater](NUM-07E_greater.md) | `a > b` |
| [NUM-07F greater_equal](NUM-07F_greater_equal.md) | `a >= b` |

`is_close` and `select` ([NUM-07G](NUM-07G_is_close.md), [NUM-07H](NUM-07H_select.md)) also provide strict and two accelerated profile keys. `select` copies the chosen branch's raw bits and requires caller-supplied 0/1 conditions.
