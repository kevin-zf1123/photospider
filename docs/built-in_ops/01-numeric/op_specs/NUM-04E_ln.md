---
spec_schema_version: 1
id: NUM-04E
parent_id: NUM-04
function: ln
proposed_operation_keys:
  - numeric.ln_strict
  - numeric.ln_accelerated_apple_silicon
  - numeric.ln_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04E: ln

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes the natural logarithm of each Float32/Float64 value, preserving shape and dtype. Accuracy requirements follow [exp](NUM-04D_exp.md); the formula and special-value table below replace exp's. There is no parameter, arbitrary-base mode or `log` alias. Integer conversion is explicit. Keys: `numeric.ln_strict`, `numeric.ln_accelerated_apple_silicon`, `numeric.ln_accelerated_x86_64`.

## Mathematics

Strict positive finite results are RN_dtype(ln(exact(x))), rounded directly to the output dtype. Accelerated nonzero finite results differ by at most four representable steps; NaN/infinity/zero classification and sign match strict.

| Input | Output |
| --- | --- |
| Positive finite x other than 1 | Natural logarithm under the chosen quality profile |
| 1 | Exact +0 |
| +0 or -0 | -infinity, successful numeric observation |
| Negative finite x or -infinity | Fixed positive quiet NaN, successful observation |
| +infinity | +infinity |
| NaN | Quiet input NaN preserving payload and sign |

There is no domain epsilon around zero or one. Tiny positive subnormals are valid inputs and are not flushed to zero.

## Acceptance

ln(1)=+0, both signed zeros -> -Inf, negative finite and -Inf -> canonical NaN, +Inf unchanged, signed NaN payloads, smallest positive subnormal, powers of two, neighbors immediately above and below one, and large positive finite inputs. Strict compares bits; accelerated compares ULP distance plus exact classification. A negative sample outside the requested region does not affect other outputs.

Fixture: `[1,+0,-0,-1,+Inf]` gives `[+0,-Inf,-Inf,canonical_NaN,+Inf]`.
