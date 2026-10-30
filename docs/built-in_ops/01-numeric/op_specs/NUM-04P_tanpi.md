---
spec_schema_version: 1
id: NUM-04P
parent_id: NUM-04
function: tanpi
proposed_operation_keys:
  - numeric.tanpi_strict
  - numeric.tanpi_accelerated_apple_silicon
  - numeric.tanpi_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04P: tanpi

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md), the [trigonometric contract](NUM-04_trigonometric_contract.md) and the [unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes the mathematical tan(pi*x) using exact pi and the exact real value of the input float. One input `input` and one output `values` have identical shape and dtype, Float32 or Float64. The output carries no domain description. There are no static parameters, implicit casts or rounded-pi multiplication in the definition. Keys: `numeric.tanpi_strict`, `numeric.tanpi_accelerated_apple_silicon`, `numeric.tanpi_accelerated_x86_64`.

## Mathematics

| Input | Output |
| --- | --- |
| Integer x, including signed zero | Zero with sign(x) |
| n+1/2 | Fixed positive quiet NaN; success |
| n+1/4 / n+3/4 | Exactly +1 / -1 |
| +-Inf | Fixed positive quiet NaN; success |
| NaN | Quiet input NaN preserving payload/sign |

Strict rounds the exact mathematical function directly to the output dtype. Accelerated finite nonzero results allow at most four FP32-scaled ULP; classification, zero signs and named landmarks match strict exactly. All finite inputs are supported, including large integers and subnormal inputs. Quarter-angle landmarks and zero conventions follow the trigonometric contract.
Exact half-integer poles yield NaN; values merely near a pole use the actual function.

## Acceptance

Fixture: `[0,0.25,0.5,0.75,1]` gives `[+0,1,canonical_NaN,-1,+0]`. Cases: exact bit patterns at landmarks, huge positive/negative integers, adjacent representable values around half and quarter integers, subnormals, NaN payloads and signed zeros, with ordinary and midpoint-sensitive arguments checked against an independent high-precision reference.
