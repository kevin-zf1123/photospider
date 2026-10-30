---
spec_schema_version: 1
id: NUM-07G
parent_id: NUM-07
function: is_close
proposed_operation_keys:
  - numeric.is_close_strict
  - numeric.is_close_accelerated_apple_silicon
  - numeric.is_close_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07G: is_close

Numeric profile: strict is the exact reference. The result is discrete and bitwise identical across the three CPU profiles. Inherits the [NUM baseline](NUM_common_contract.md) and the [comparison contract](NUM-07_comparison_contract.md); explicit rules below take precedence.

## Interface

Compares matching Float32/Float64 inputs `a` and `b` using the symmetric formula

```text
abs(a-b) <= atol + rtol * max(abs(a),abs(b))
```

The two inputs have identical dtype and shape. Output `values` is UInt8 with unchanged shape and no attached domain description: 1 means close, 0 means not close.

| Parameter | Type and domain | Default |
| --- | --- | --- |
| `atol` | Float64, finite and nonnegative | Required |
| `rtol` | Float64, finite and nonnegative | Required |

Negative zero is numerically zero. Missing or wrongly typed parameters fail compilation; finite/nonnegative tolerances are unchecked numerical premises. There is no other static parameter, implicit cast or broadcast. Keys: `numeric.is_close_strict`, `numeric.is_close_accelerated_apple_silicon`, `numeric.is_close_accelerated_x86_64`.

## Mathematics

Finite inputs and tolerances denote their exact binary rational values. The entire formula is compared exactly, including the equality boundary, with no intermediate floating rounding, overflow or underflow. No epsilon is added.

Before the finite formula: any NaN yields 0; same-sign infinities yield 1; otherwise an infinity paired with any different value yields 0. Both signed zeros yield 1. NaN payloads do not affect the output. Both operands remain dependencies even when classification decides the result.

## Acceptance

Fixture: a=[1,2,3], b=[1,2.25,4], atol=0.25, rtol=0 gives [1,1,0]. Cases: symmetry, exact tolerance boundaries and adjacent floats, zero tolerances, signed zeros, infinities, all NaN positions, unequal input dtypes rejected. For a=MAX_FLOAT64, b=-MAX_FLOAT64, atol=0 and rtol=1.5 the result is 0 (naive floating arithmetic can overflow both sides to infinity and return 1). Verify with independent exact rational arithmetic, including tiny subnormal differences and products.
