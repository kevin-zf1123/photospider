---
spec_schema_version: 1
id: NUM-04D
parent_id: NUM-04
function: exp
proposed_operation_keys:
  - numeric.exp_strict
  - numeric.exp_accelerated_apple_silicon
  - numeric.exp_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04D: exp

Numeric profile: strict is the exact reference defined below. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact. Inherits the [NUM baseline](NUM_common_contract.md) and the [NUM-04 unary contract](NUM-04_unary_contract.md); explicit rules below take precedence.

Computes the natural exponential e^x of each Float32/Float64 value, preserving dtype and shape. Integer inputs require explicit conversion. There are no static parameters. Keys: `numeric.exp_strict`, `numeric.exp_accelerated_apple_silicon`, `numeric.exp_accelerated_x86_64`.

## Mathematics

Strict is RN_dtype(exp(exact(x))), correctly rounded directly to the output dtype. Accelerated final outputs obey the shared FP32 four-ULP contract, including Float64 outputs compared directly on the FP32 absolute-error scale. A reference of zero, in the FP32 subnormal range or outside the FP32 range is evaluated strictly. NaN, infinity, classification and signed zero match strict exactly.

Both +-0 map to exactly 1. +Inf maps to +Inf, -Inf maps to +0, and input NaNs are quieted with payload and sign retained. Correct floating overflow maps to +Inf; correct underflow may produce a positive subnormal or +0. All are successful numeric results; there is no domain failure for a generic floating input. A fixed profile gives identical bits for the same inputs and dtype regardless of batching or scheduling; platform profiles can differ within the accepted tolerance.

## Acceptance

Check exp(+-0)=1, both infinities, signed NaN payload quieting, and positive/negative finite inputs with independently resolved destination rounding. Include neighbors around the overflow boundary, the normal/subnormal boundary and the underflow-to-zero boundary for both dtypes, and hard-to-round inputs. Strict compares bits; accelerated checks finite ULP distance and exact classification/NaN/zero behavior separately against an independent high-precision interval reference.

Fixture: `[0,-Inf,+Inf]` gives `[1,+0,+Inf]`.
