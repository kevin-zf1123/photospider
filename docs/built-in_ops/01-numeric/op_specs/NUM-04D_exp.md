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
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-04D: exp

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

For each observed Float32/Float64 x, compute the natural exponential e^x and
preserve dtype/shape. Inherit [NUM-04 common requirements](NUM-04_unary_contract.md)
for the parameter-free input/values interface, generic output, exact regional
and typed-validation demand, NaN bits, lifetime, resource accounting and public
acceptance. Integer inputs require explicit conversion.

## Numeric profile

Strict is RN_dtype(exp(exact(x))), correctly rounded directly to output dtype.
Accelerated final outputs obey the shared FP32 4 ULP contract, including
Float64 outputs compared directly on the FP32 absolute-error scale. Zero, FP32
subnormal-range and out-of-FP32-range references use strict. NaN, infinity,
classification and signed zero match strict exactly.

Both ±0 map to exactly 1. +Inf maps to +Inf, -Inf maps to +0, and input NaNs
are quieted with payload/sign retained. Correct floating overflow maps to +Inf;
correct underflow may produce positive subnormal or +0, all as successful numeric
results. There is no domain Status failure for a generic floating input.

An accelerated argument range without an established bound/classification path
falls back to strict. Report actual operation/profile/platform/ISA, evaluated
elements and strict fallback reason/count through host-owned diagnostics. No
third output is added. Resource/cancellation/upstream failure cannot be erased
by a mathematical fallback. Fixed profile, inputs and dtype must give identical
bits across batching/SIMD widths and scheduling; platform profiles can differ
within the accepted tolerance.

## Reference and implementation requirements

Classify nonfinite/zero operands by bits first. Strict implementation must resolve
correct rounding, using proved correctly rounded math or high-precision directed
enclosures with explicit tie handling. Range reduction and overflow/underflow
classification must avoid constructing an enormous exp value merely to recognize
an out-of-range result. Fixed precision and measured residual alone are not a
correct-rounding proof. Charge every refinement and allocated limb buffer; stop
with ResourceExhausted rather than guess if the available budget cannot resolve it.

Accelerated implementations publish their actual library/algorithm/version and
supported argument ranges with accuracy justification; no blanket std::exp or
vector-library name establishes the four-ULP bound. Unverified cases take strict
fallback. Work is per requested sample plus actual reduction/refinement cost;
scratch and temporary growth remain under inherited host budgets. No unrequested
SIMD tail is evaluated to fill a vector.

## Independent acceptance

Check exact exp(±0)=1, both infinities, signed NaN payload quieting, and a set of
positive/negative finite inputs with independently resolved destination rounding.
Include neighbors around the overflow boundary, normal/subnormal boundary and
underflow-to-zero boundary for both dtypes. Strict compares bits. Accelerated
checks finite ULP distance and separate exact classification/NaN/zero behavior.
Include hard-to-round inputs and forced input-range fallback with diagnostics.

Use a public WorkflowDocument fixture binding `[0,-Inf,+Inf]`; values must be
`[1,+0,+Inf]`, with generic output facets. Also execute nonzero/disjoint regions,
changed NaN payloads, altered host rounding settings and common cache/lifetime/
budget/cancellation cases. An independent high-precision interval oracle is
required; calling the production function twice is not acceptance.

The maintained implementation and public run command are documented below; local timing is recorded in the implementation notes.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses exact pointwise Data,
separate typed validation and Atom-scoped failures. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.0-p12; revalidated 2026-09-21)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
