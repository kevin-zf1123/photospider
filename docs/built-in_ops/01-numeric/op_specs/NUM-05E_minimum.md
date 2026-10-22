---
spec_schema_version: 1
id: NUM-05E
parent_id: NUM-05
function: minimum
proposed_operation_keys:
  - numeric.minimum_strict
  - numeric.minimum_accelerated_apple_silicon
  - numeric.minimum_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
---

# NUM-05E: minimum

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Return the lesser value of matching elements in inputs `a` and `b`.
Support UInt8, Int64, Float32 and Float64; shapes and dtypes match exactly.
Output `values` preserves shape and dtype with no attached domain description. There are no static
numeric parameters. Inherit the [binary contract](NUM-05_binary_contract.md)
for both-operand demand, structural validation, errors, resources and execution.

## Numeric semantics

All three profiles are bitwise identical. Integer comparison preserves the
selected value exactly, including Int64 extrema, without conversion to floating
point. Floating infinities participate in the ordinary numerical order.

If either operand is NaN, propagate that input's payload/sign and quiet it.
If both are NaN, propagate the first input `a` under the shared priority rule.
This is not a NaN-ignoring operation. Both source operands remain declared
reads even if a NaN determines the output; their schemas are checked during
static preparation, without runtime sample validation.

For mixed signed zeros, return -0. For same-sign zeros, retain that sign.
For equal nonzero operands, return their common value. These exact selection
rules introduce no rounding tolerance or numeric overflow failure.

## Acceptance and current implementation

Bind a=[-2,0,5] and b=[1,0,3] through a public WorkflowDocument and inspect the
expected values [-2,0,3] in both ordinary and disjoint requests. Include all four signed
zero combinations, infinities, each single-NaN placement, two NaNs with different
payload/sign bits, signaling NaNs, and Int64 values beyond Float64's exact range.
Use an independent integer/bit-classification oracle; apply shared resource,
static validation, lifetime, compatible rebinding and cancellation cases.

The unsuffixed `numeric.minimum` key uses fixed CPU tile tasks and an unchecked finite Float32/Float64 domain. It returns negative zero for every zero pair. The suffixed keys below have the separate dtype and IEEE propagation rules specified here.

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
