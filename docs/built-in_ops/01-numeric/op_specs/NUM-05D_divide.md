---
spec_schema_version: 1
id: NUM-05D
parent_id: NUM-05
function: divide
proposed_operation_keys:
  - numeric.divide_strict
  - numeric.divide_accelerated_apple_silicon
  - numeric.divide_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05D: divide

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute elementwise `a/b`. Inputs `a` and `b` must have identical shapes
and dtypes. Support Float32 and Float64 only; output `values` preserves
the input dtype and shape and has no attached domain description. There are no static numeric
parameters. Inherit the [binary contract](NUM-05_binary_contract.md), including
exact per-input demand, validation, resources, lifetime, errors and acceptance.

## Numeric semantics

For finite floating operands, strict correctly rounds the exact mathematical
result directly to the output dtype, with ties to even and gradual underflow.
Accelerated floating results obey the shared final FP32 4 ULP contract. Finite overflow produces signed infinity as a
successful numeric output. Input NaNs follow the shared payload/sign priority
before non-NaN special cases are considered; both source operands are still read.

| Non-NaN operands | Result |
| --- | --- |
| Zero divided by zero, or infinity divided by infinity | Canonical positive quiet NaN |
| Finite nonzero divided by zero | Infinity with XOR operand sign |
| Infinity divided by finite (including zero) | Infinity with XOR operand sign |
| Finite divided by infinity | Zero with XOR operand sign |

Every zero or infinite non-NaN result has the XOR of operand signs, including
underflow and signed-zero operands.

Integer inputs require explicit cast. This function has no truncating or
flooring integer quotient mode.

## Acceptance and unsuffixed contract

Conceptual public fixture: `[1,2,3] / [2,2,2] -> [0.5,1,1.5]`, repeated for supported dtypes.
Include signed-zero operand combinations, extrema, subnormal/normal boundaries,
all infinity combinations and NaNs with distinct payloads in both input orders.
Use exact rational rounding as the floating oracle. Integer input ports fail
TypeMismatch at compile/preflight. Requested floating overflow returns signed
infinity successfully; computation covers only selected output tiles. Apply all
shared public execution and resource cases.

The unsuffixed `numeric.divide` uses fixed CPU tile tasks with its separate unchecked finite Float32/Float64 contract. See [Numeric operations](../../../kernel-architecture/Numeric-Operations.md).

## Maintained implementation

The three keys are registered by `plugins/ops/01-numeric/numeric_binary.cpp`,
with independently named constructors in `photospider/ops/numeric/binary.hpp`.
The fixed adapter declares both intersecting input tile sets, including when a numeric identity determines a result.
Floating elementary operations use controlled correctly rounded hardware
arithmetic after exact special-value classification, with exact fallback; integer
operations use exact arithmetic under the caller premise of representable
integer results; fixed arithmetic-capacity failures remain operational. Accelerated ordinary positive-base
power and angle results use SLEEF binary64 enclosures within the shared admitted
ranges. atan2pi divides an angle enclosure by an enclosed pi. Only rejected
candidates dispatch the certified strict backend . See [math implementation](../math-implementation.md) for
rounding, fixed scratch admission, cancellation and unresolved-refinement limits.
