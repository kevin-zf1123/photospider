---
spec_schema_version: 1
id: NUM-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05: shared binary contract

Numeric profile: strict is the exact reference defined by each operation. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and shared requirements; explicit rules below and in the named family contract take precedence.

The independent operations are add, subtract, multiply, divide, minimum, maximum, pow, atan2 and atan2pi. Each has strict, Apple Silicon CPU accelerated and x86-64 CPU accelerated keys. There is no generic function-mode dispatcher.

| Spec | Supported dtypes | Accelerated quality |
| --- | --- | --- |
| [NUM-05A add](NUM-05A_add.md) | All four | Bitwise strict equivalence |
| [NUM-05B subtract](NUM-05B_subtract.md) | All four | Bitwise strict equivalence |
| [NUM-05C multiply](NUM-05C_multiply.md) | All four | Bitwise strict equivalence |
| [NUM-05D divide](NUM-05D_divide.md) | Float32/64 | Bitwise strict equivalence |
| [NUM-05E minimum](NUM-05E_minimum.md) | All four | Bitwise strict equivalence |
| [NUM-05F maximum](NUM-05F_maximum.md) | All four | Bitwise strict equivalence |
| [NUM-05G pow](NUM-05G_pow.md) | Float32/64 | <=4 FP32-scaled ULP; exact special classification |
| [NUM-05H atan2](NUM-05H_atan2.md) | Float32/64 | <=4 ULP; exact special directions |
| [NUM-05I atan2pi](NUM-05I_atan2pi.md) | Float32/64 | <=4 ULP; exact special directions |

## Interface

Each operation has named inputs `a`, `b` and output `values`; atan2/atan2pi use `y` and `x` in that order. Each bound input Result contains exactly one tensor member in slot 0; the Result schema IDs may differ. Input rank is 1..8, extents are positive, and the full sample shapes and dtypes of both inputs match. Use explicit broadcast and cast operations for shape/type adaptation. The `values` output is a Result using schema `photospider.tensor`, tensor key `samples`, the complete input sample shape as ordinary axes, and no attached domain description; output batch topology is dropped. No implicit broadcast, cast or unit inference occurs.

Unsupported dtype, mismatched shape and invalid static parameters are rejected before numeric reads. The operator-specific files define supported dtypes, output dtype, mathematical rules, integer overflow and any permitted accelerated approximation.

## Floating and operand policy

Floating operations follow NUM-04's IEEE-like result policy: domain errors, division by zero and floating overflow produce specified NaN/infinity outputs as successful observations. Both operands at the requested coordinate are read, including when a special numeric identity determines the result; no short-circuit changes upstream failure or data obligations.

The sole input NaN, or the first input when both are NaN, propagates with its payload and sign, with the quiet bit set. This priority applies regardless of which NaN was signaling. A newly generated NaN uses the fixed positive quiet pattern specified by NUM-04. Pow special rules are stated in its file and override only their stated cases. Minimum/maximum keep this NaN priority.

- Add/subtract/multiply support all four dtypes and preserve dtype. Floating results are correctly rounded to that dtype. Integer exact results outside the dtype range violate an unchecked caller precondition; no wrap, saturation or implicit promotion is specified.
- Divide supports Float32/Float64 only, preserving dtype. Integer conversion is explicit; there is no integer quotient mode.
- Minimum/maximum support all four dtypes and preserve dtype, with bitwise equivalent profiles. Mixed signed zeros give -0 for minimum and +0 for maximum; same-sign zeros keep that sign.

Integer results must fit the destination dtype as an unchecked caller precondition.

## Acceptance

Compare against exact integer/rational or high-precision references. Cases: supported dtypes, special bits, signed zeros and NaN payloads.
