---
spec_schema_version: 1
id: NUM-07F
parent_id: NUM-07
function: greater_equal
proposed_operation_keys:
  - numeric.greater_equal_strict
  - numeric.greater_equal_accelerated_apple_silicon
  - numeric.greater_equal_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
---

# NUM-07F: greater_equal

Numeric profile: strict is the exact reference. The result is discrete and bitwise identical across the three CPU profiles. Inherits the [NUM baseline](NUM_common_contract.md) and the [exact comparison contract](NUM-07_comparison_contract.md) in full; explicit rules below take precedence.

## Interface

For each coordinate in a requested output tile, returns UInt8 1 when `a >= b` is true, otherwise UInt8 0. Inputs `a` and `b` have identical shape and dtype (UInt8, Int64, Float32 or Float64). Output `values` has that shape and no attached domain description. There are no static parameters or implicit cast/broadcast. Keys: `numeric.greater_equal_strict`, `numeric.greater_equal_accelerated_apple_silicon`, `numeric.greater_equal_accelerated_x86_64`.

## Mathematics

The result is 1 exactly when the numerical relation `a >= b` holds. This is numerical comparison, not bit-pattern comparison: signed zeros compare equal, NaN handling and exact integer comparison follow the comparison contract.

## Acceptance

Fixture: a=[1,2,3], b=[2,2,2] gives [0,1,1], for all supported dtypes. Cases: Int64 extrema and values above 2^53, signed zeros, subnormal neighbors, infinities, and signaling/quiet NaNs in either input position, against an independent exact relation and bit classification.
