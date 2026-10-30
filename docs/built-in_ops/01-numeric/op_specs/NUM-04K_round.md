---
spec_schema_version: 1
id: NUM-04K
parent_id: NUM-04
function: round
proposed_operation_keys:
  - numeric.round_strict
  - numeric.round_accelerated_apple_silicon
  - numeric.round_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04K: round

Numeric profile: strict is the exact reference defined below. Discrete results are exact and bitwise identical across the three keys. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Rounds each floating value to the nearest integral value, with exact halfway cases choosing the even integer. Shape and dtype are preserved; UInt8 and Int64 inputs are identity bit copies. There is no rounding-mode or decimal-place parameter and no implicit Int64 output conversion. Keys: `numeric.round_strict`, `numeric.round_accelerated_apple_silicon`, `numeric.round_accelerated_x86_64`.

## Mathematics

For finite x the result is the integer n minimizing |x-n|, with a tie selecting even n, stored in the original dtype and using the input sign for any zero result. Thus `[-2.5,-1.5,-0.5,-0,+0,0.5,1.5,2.5]` gives `[-2,-2,-0,-0,+0,+0,2,2]`. Infinities are unchanged. NaNs retain payload and sign and are quieted. Finite floats that are already integral remain unchanged. No integer passes through floating arithmetic. The result is independent of the caller's rounding mode. A ties-away-from-zero primitive does not implement this operation.

## Acceptance

The halfway vector above, neighbors on both sides of each halfway point, all integer types and extrema, Float32/Float64 subnormals and large integral values, signed NaNs and infinities, checked against an independent exact-rational distance/tie reference with explicit signed-zero/NaN bit rules.
