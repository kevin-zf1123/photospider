---
spec_schema_version: 1
id: NUM-04-trigonometric
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-04: shared trigonometric contract

Numeric profile: strict is the exact reference defined by each operation. Floating arithmetic in accelerated profiles follows the shared [final FP32 four-ULP contract](NUM_accelerated_contract.md). Discrete results, copies, selected endpoints and special values are exact.

Inherit the [NUM baseline](NUM_common_contract.md) for registration and shared requirements; explicit rules below and in the named family contract take precedence.

Sin, cos, tan, sinpi, cospi and tanpi each have their own specification and three independent strict/Apple Silicon CPU accelerated/x86-64 CPU accelerated keys. They inherit the [unary contract](NUM-04_unary_contract.md) and the accuracy rules of [exp](NUM-04D_exp.md), with each function's own formula and special-value table.

## Interface

Input and output are same-dtype Float32/Float64 with unchanged shape and generic output structure. Integer inputs require explicit cast. There are no static numeric parameters. The operation name determines argument interpretation; there is no angle_unit parameter.

## Angle interpretation and exact landmarks

For sin/cos/tan, the mathematical argument is the exact real value of the supplied float. There is no angle snapping, epsilon recognition, degree conversion or identification of a rounded floating pi with mathematical pi. All finite arguments, including the largest finite values, are in scope.

For sinpi/cospi/tanpi, x means the exact real angle pi*x. The multiplier is never first multiplied by a rounded pi in the input dtype. Exact reduction of the binary rational multiplier modulo two (sin/cos) or one (tan) handles enormous integers and half-integers without converting their value to Int64 or overflowing an intermediate angle.

The landmark conventions hold in every profile:

- Sinpi at integer multipliers returns zero with the multiplier's sign, including -0. At n+1/2 it returns exactly (-1)^n.
- Cospi at integer multipliers returns exactly (-1)^n. At half-integers it returns +0.
- Tanpi at integer multipliers returns zero with the multiplier's sign. At n+1/2 it returns the fixed positive quiet NaN; no one-sided infinity is chosen.
- Tanpi at n+1/4 and n+3/4 returns exactly +1 and -1 respectively.
- For quarter-integer sinpi/cospi landmarks, the appropriate signed sqrt(1/2) is correctly rounded directly to the output dtype.

The zero-sign conventions are normative; numerical oddness/periodicity does not imply a bit-level zero-sign identity. Nonfinite inputs are classified before angle reduction: +-Inf -> fixed positive quiet NaN, input NaN -> quiet payload/sign-preserving NaN. These are successful numeric results.

## Numerical quality

Strict returns the correctly rounded mathematical sin/cos/tan of the selected exact argument, directly in the output dtype. Accelerated nonzero finite values are within four FP32-scaled representable steps of strict; NaN/Inf/zero classification and sign, and the named landmark results, match strict exactly. Range reduction accuracy is part of the final error bound.

## Acceptance

Compare against an independent high-precision directed reference with exact input-float interpretation and separately exact pi-multiple semantics. Strict compares bits; accelerated compares ULP distance plus exact special-value/landmark results. Cases include huge arguments, neighbors of reduction boundaries, signed subnormals, NaN payloads and midpoint rounding cases.

For pi multiples, test positive/negative large integers, half/quarter-integers and neighboring representable values. The radian pi rounded to dtype is not treated as the exact pi-multiple input 1.
