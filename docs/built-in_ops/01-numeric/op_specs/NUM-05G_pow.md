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
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-05G: pow

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute a^b for same-shape, same-dtype Float32/Float64 inputs `a` and `b`.
Output `values` preserves shape/dtype and has empty facets. No static numeric
parameters, integer output mode or implicit cast/broadcast is provided. Inherit
[binary execution requirements](NUM-05_binary_contract.md).

## Special-case priority

Read and validate both operands, then apply this ordered decision table. Numeric
NaN/infinity outputs are successful observations; resource/upstream/typed errors
retain Status semantics. These rules do not change NUM-01's finite expression
contract.

1. If b is either signed zero, return +1, including when a is NaN.
2. If a is +1, return +1, including when b is NaN.
3. Otherwise propagate any NaN by the shared payload/sign priority, quieting it.
4. If b is infinite, return +1 when |a|=1. For |a|>1, +Inf exponent yields
   +Inf and -Inf exponent yields +0; for |a|<1 these outputs are reversed.
5. For zero a and finite nonzero b, positive b yields zero and negative b
   yields infinity. The result is negative only when a is -0 and b is an odd
   integer; otherwise its sign is positive.
6. For infinite a and finite nonzero b, positive b yields infinity and negative
   b yields zero. The result is negative only when a is -Inf and b is an odd
   integer; otherwise its sign is positive, including noninteger exponents.
7. Finite strictly negative a with a noninteger finite exponent yields the fixed
   positive quiet NaN. Otherwise evaluate the real mathematical power.

In particular 0^0, NaN^0 and 1^NaN are +1; (-1)^±Inf is +1. Integer/oddness
classification uses the exact floating value without narrowing the exponent to
Int64. Float32 integers of magnitude at least 2^24 and Float64 integers of
magnitude at least 2^53 are all even; adjacent representable values below these
thresholds must retain their exact parity.

## Rounding, backend and resources

Strict correctly rounds the exact real power directly to output dtype. It is
not defined by rounded exp(b*ln(a)) or sequential rounded multiplication.
Finite overflow/underflow produces correctly signed infinity/subnormal/zero.
Accelerated nonzero finite results permit at most four output-dtype ULP from
strict; special-case outputs and NaN/Inf/zero classification/sign match strict
exactly. Unsupported accuracy ranges use strict fallback with host diagnostics,
following [exp's quality/resource rules](NUM-04D_exp.md). Profile results do not
vary with request batching, vector width or scheduling.

Classify clear overflow/underflow without constructing impractically enormous
powers. Correct rounding may use validated libraries or directed enclosures with
exact-boundary handling. Charge all refinement/temporary storage and return
ResourceExhausted if the budget cannot resolve the result. A fast exp/log
composition requires a final pow error guarantee, not independent step tolerances.

## Acceptance and current status

Conceptual public fixture: a=[2,-2,-2,0], b=[3,3,0.5,0] gives
[8,-8,canonical_NaN,1]. Exercise all table branches, both signed zeros and
infinities, NaNs with distinct payloads, large exponent parity, neighbors of 1,
small bases, overflow/underflow boundaries and difficult rounding cases in both
dtypes. Independently certify finite results by exact integer/rational cases or
directed high-precision power; model special values and payloads separately.

Test both upstream dependencies even for NaN^0 and 1^NaN; apply shared regional,
resource, cancellation, typed validation and lifetime fixtures through the public
entry point when implemented. No versioned pow key or runtime test is delivered
by this specification-only change.
