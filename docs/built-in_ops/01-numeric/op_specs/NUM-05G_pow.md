---
spec_schema_version: 1
id: NUM-05G
parent_id: NUM-05
function: pow
proposed_operation_keys:
  - numeric.pow_strict
  - numeric.pow_accelerated_apple_silicon
  - numeric.pow_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05G: pow

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [binary contract](NUM-05_binary_contract.md); explicit rules below take precedence.

Computes a^b for same-shape, same-dtype Float32/Float64 inputs `a` and `b`. Output `values` preserves shape and dtype and has no attached domain description. There are no static parameters, integer output mode or implicit cast/broadcast. Keys: `numeric.pow_strict`, `numeric.pow_accelerated_apple_silicon`, `numeric.pow_accelerated_x86_64`.

## Special-case priority

Both operands are read; then this ordered decision table applies. NaN/infinity outputs are successful observations. These rules do not change NUM-01's finite expression contract.

1. If b is either signed zero, return +1, including when a is NaN.
2. If a is +1, return +1, including when b is NaN.
3. Otherwise propagate any NaN by the shared payload/sign priority, quieting it.
4. If b is infinite, return +1 when |a|=1. For |a|>1, +Inf exponent yields +Inf and -Inf exponent yields +0; for |a|<1 these outputs are reversed.
5. For zero a and finite nonzero b, positive b yields zero and negative b yields infinity. The result is negative only when a is -0 and b is an odd integer; otherwise its sign is positive.
6. For infinite a and finite nonzero b, positive b yields infinity and negative b yields zero. The result is negative only when a is -Inf and b is an odd integer; otherwise its sign is positive, including noninteger exponents.
7. Finite strictly negative a with a noninteger finite exponent yields the fixed positive quiet NaN. Otherwise the real mathematical power is evaluated.

In particular 0^0, NaN^0 and 1^NaN are +1; (-1)^(+-Inf) is +1. Integer and oddness classification uses the exact floating value without narrowing the exponent to Int64. Float32 integers of magnitude at least 2^24 and Float64 integers of magnitude at least 2^53 are all even; adjacent representable values below these thresholds keep their exact parity.

## Rounding

Strict correctly rounds the exact real power directly to the output dtype; it is not defined by a rounded exp(b*ln(a)) or sequential rounded multiplication. Finite overflow/underflow produces correctly signed infinity/subnormal/zero. Accelerated nonzero finite results are within four FP32-scaled ULP of strict; special-case outputs and NaN/Inf/zero classification and sign match strict exactly. The four-ULP bound applies to the final power, not to independent exp/log steps. Results do not vary with request batching or vector width.

## Acceptance

Fixture: a=[2,-2,-2,0], b=[3,3,0.5,0] gives [8,-8,canonical_NaN,1]. Cases: all table branches, both signed zeros and infinities, NaNs with distinct payloads, large exponent parity, neighbors of 1, small bases, overflow/underflow boundaries and difficult rounding cases in both dtypes. Finite results are certified by exact integer/rational cases or directed high-precision power; special values and payloads are modeled separately. Both inputs are dependencies even for NaN^0 and 1^NaN.
