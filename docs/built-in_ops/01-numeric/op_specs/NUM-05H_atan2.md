---
spec_schema_version: 1
id: NUM-05H
parent_id: NUM-05
function: atan2
proposed_operation_keys:
  - numeric.atan2_strict
  - numeric.atan2_accelerated_apple_silicon
  - numeric.atan2_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05H: atan2

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [binary contract](NUM-05_binary_contract.md); explicit rules below take precedence.

Inputs are named `y` (first operand) and `x` (second operand), with identical shape and dtype, Float32 or Float64. Output `values` preserves shape and dtype, has no attached domain description, and expresses the oriented angle in radians. There is no static angle-unit parameter or degree output. The binary contract applies with y/x in place of a/b for both-operand dependency and NaN priority. Integer inputs require explicit cast. Keys: `numeric.atan2_strict`, `numeric.atan2_accelerated_apple_silicon`, `numeric.atan2_accelerated_x86_64`.

## Mathematics

For finite inputs away from the origin, the principal angle theta is the polar argument of x+i*y with range [-pi,pi]. Strict rounds the mathematical `atan2(y,x)` directly to the output dtype; an already-rounded radian angle is never divided by a rounded pi. Both inputs are read even when a special case fixes the result.

If either input is NaN, it is quieted and preserved; if both are NaN, y's payload and sign win. Otherwise the following exact table applies. Here s is the sign of y, including signed zero; s*0 means zero with that sign. Every listed result is a successful numeric output. For atan2 the result is RN_dtype(theta), where theta is the listed theta/pi times exact pi.

| Condition | theta/pi |
| --- | --- |
| y=+-0 and x positive or +0 | s*0 |
| y=+-0 and x negative or -0 | s*1 |
| x=+-0 and y finite nonzero | s*1/2 |
| y=+-Inf and x finite | s*1/2 |
| y=+-Inf and x=+Inf | s*1/4 |
| y=+-Inf and x=-Inf | s*3/4 |
| y finite and x=+Inf | s*0 |
| y finite and x=-Inf | s*1 |

The zero-y rows also cover infinite x according to its sign. The four signed-zero origin combinations keep their directional information. Ordinary finite nonzero inputs use their exact real ratio and direction; an overflowing or underflowing y/x intermediate must not destroy quadrant or accuracy information.

Accelerated finite nonzero outputs are within four FP32-scaled ULP of strict. NaN/Inf/zero classification and signs match strict, and every table entry matches the strict rounded output exactly. Correctly rounded endpoints and neighbors near pi follow the rounding contract; rounded radians are not clamped to a floating approximation of pi.

## Acceptance

Fixture: y=[+0,-0,1,-1], x=[-0,-0,+0,+0] gives [pi,-pi,pi/2,-pi/2] (each independently correctly rounded). Cases: all sign combinations at axes, origin and infinities, NaN payload priority, huge and subnormal ratios, quadrant boundaries, diagonal neighbors and hard rounding cases, against an independent high-precision angle reference with direct destination rounding.
