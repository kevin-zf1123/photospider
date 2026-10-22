---
spec_schema_version: 1
id: NUM-05A
parent_id: NUM-05
function: add
proposed_operation_keys:
  - numeric.add_strict
  - numeric.add_accelerated_apple_silicon
  - numeric.add_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05A: add

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute elementwise `a+b`. Inputs `a` and `b` must have identical shapes
and dtypes. Support UInt8, Int64, Float32 and Float64; output `values` preserves
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
| Opposite signed infinities | Canonical positive quiet NaN |
| Infinity plus finite, or same signed infinities | That infinity |

Exact cancellation produces +0. Adding -0 to -0 produces -0; other sums
of signed zeros produce +0. A nonzero exact result rounding to zero retains
its mathematical sign.

For UInt8/Int64, the caller supplies operands whose exact result fits the destination dtype. This is an unchecked precondition. Valid results are computed exactly without floating conversion, wrapping, saturation or promotion.

## Acceptance and unsuffixed contract

Conceptual public fixture: `[1,2,3] + [4,5,6] -> [5,7,9]`, repeated for supported dtypes.
Include signed-zero operand combinations, extrema, subnormal/normal boundaries,
all infinity combinations and NaNs with distinct payloads in both input orders.
Use exact rational rounding as the floating oracle and arbitrary-precision
integers for representable integer cases. Overflow violates the caller precondition and has no promised diagnostic outcome. Apply all shared public execution and resource cases.

The unsuffixed `numeric.add` key uses fixed CPU tile tasks and an unchecked finite Float32/Float64 domain. Its preconditions and output selection are described in [Numeric operations](../../../kernel-architecture/Numeric-Operations.md); the suffixed keys below have this separate IEEE contract.

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
