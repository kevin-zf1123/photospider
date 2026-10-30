---
spec_schema_version: 1
id: NUM-04C
parent_id: NUM-04
function: sqrt
proposed_operation_keys:
  - numeric.sqrt_strict
  - numeric.sqrt_accelerated_apple_silicon
  - numeric.sqrt_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04C: sqrt

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes the nonnegative square root of each Float32/Float64 value, preserving shape and dtype. Integer input is TypeMismatch; conversion is explicit. There are no static parameters. Keys: `numeric.sqrt_strict`, `numeric.sqrt_accelerated_apple_silicon`, `numeric.sqrt_accelerated_x86_64`.

## Mathematics

| Input | Output |
| --- | --- |
| Positive finite x | Correctly rounded mathematical sqrt(x), directly in input/output dtype |
| +0 / -0 | Same signed zero |
| +infinity | +infinity |
| Negative finite x or -infinity | Canonical positive quiet NaN from the unary contract; successful observation |
| NaN | Quiet input NaN, preserving payload and sign |

Strict requires correctly rounded bits; accelerated follows the shared FP32-scaled final-result bound. Float32 output is the mathematical root rounded directly to Float32. Every numeric input has a defined result; domain cases are not failures. NUM-01's expression sqrt has a different input/output/error contract.

## Acceptance

sqrt(0)=+0, sqrt(-0)=-0, sqrt(1)=1, sqrt(4)=2, correctly rounded sqrt(2), negative inputs and -Inf give canonical NaN, +Inf unchanged, payload-preserving quieting of signaling NaNs, smallest positive subnormal and largest finite input, and destination rounding boundaries. The reference locates the destination candidate by comparing x against squares of adjacent rounding midpoints with exact binary-rational arithmetic, including ties-to-even.

Fixture: `[-1,-0,0,4,+Inf]` gives `[canonical_NaN,-0,+0,2,+Inf]` with the same dtype.
