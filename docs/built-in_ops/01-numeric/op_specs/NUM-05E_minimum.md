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
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
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
Output `values` preserves shape and dtype with empty facets. There are no static
numeric parameters. Inherit the [binary contract](NUM-05_binary_contract.md)
for both-operand demand, typed validation, errors, resources and execution.

## Numeric semantics

All three profiles are bitwise identical. Integer comparison preserves the
selected value exactly, including Int64 extrema, without conversion to floating
point. Floating infinities participate in the ordinary numerical order.

If either operand is NaN, propagate that input's payload/sign and quiet it.
If both are NaN, propagate the first input `a` under the shared priority rule.
This is not a NaN-ignoring operation. Both source operands are still read and
validated, even if a NaN determines the output.

For mixed signed zeros, return -0. For same-sign zeros, retain that sign.
For equal nonzero operands, return their common value. These exact selection
rules introduce no rounding tolerance or numeric overflow failure.

## Acceptance and current implementation

Bind a=[-2,0,5] and b=[1,0,3] through a public WorkflowDocument and inspect the
expected values [-2,0,3] in both ordinary and disjoint requests. Include all four signed
zero combinations, infinities, each single-NaN placement, two NaNs with different
payload/sign bits, signaling NaNs, and Int64 values beyond Float64's exact range.
Use an independent integer/bit-classification oracle; apply shared resource,
validation, lifetime, invalidation and cancellation cases.

Current legacy minimum has Elementwise registration but rejects nonfinite values
through its shared reader. Its existing zero tie handling does not establish
this complete same-sign/mixed-sign contract. The maintained versioned keys implement these new dtype and propagation rules;
see the execution entry below.

## Maintained implementation

The three keys are registered by `plugins/ops/01-numeric/numeric_binary.cpp`,
with independently named constructors in `photospider/numeric/binary.hpp`.
The shared adapter retains both inputs as exact pointwise Data and separately
retains typed validation, including when a numeric identity determines a result.
Floating elementary operations use controlled correctly rounded hardware
arithmetic after exact special-value classification, with exact fallback; integer
operations retain checked exact arithmetic. Accelerated ordinary positive-base
power and angle results use SLEEF binary64 enclosures within the shared admitted
ranges. atan2pi divides an angle enclosure by an enclosed pi. Only rejected
candidates dispatch the certified strict backend and report strict fallback. See [math implementation](../math-implementation.md) for
rounding, scratch, work accounting and unresolved-refinement limits.

The [public example and commands](../../../../examples/numeric_workflow/README.md)
include this operation, an editable add/multiply composition, independent
integer/Fraction/MPFR oracles and direct error/resource checks. The manual target
is excluded from default builds and has no CTest/integration registration.

The combined family passed 14,174 independent cases per profile on native
Clang strict/Apple and Ubuntu WSL Clang strict/AVX2, plus expanded manual and
local installed-consumer checks. [Measured validation scope](../math-implementation.md#num-05-validation-and-native-timing)
records oracle versions, native timings and limitations.
