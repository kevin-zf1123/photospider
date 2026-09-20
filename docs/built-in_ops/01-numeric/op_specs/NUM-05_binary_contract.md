---
spec_schema_version: 1
id: NUM-05
kind: shared_operator_contract
category: 01-numeric
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-05: shared binary contract

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

The selected independent operations are add, subtract, multiply, divide,
minimum, maximum, pow, atan2 and atan2pi. Each receives strict, Apple Silicon CPU
accelerated and x86-64 CPU accelerated keys under the common NUM version rule.
There is no generic function-mode dispatcher.

Both inputs must have exactly the same shape and dtype; output shape is preserved.
Use explicit broadcast and cast operations for shape/type adaptation. The
operator-specific files decide supported dtypes and output dtype, mathematical
rules, integer overflow and any permitted accelerated approximation. All are
Proposed; specification acceptance is independent of implementation status.

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

## Selected floating and operand policy

Floating operations follow NUM-04's IEEE-like result policy: domain errors,
division by zero and floating overflow produce specified NaN/infinity outputs
as successful observations. Both operands at the requested coordinate are read
and validated, including cases where a special numeric identity determines the
result. No implicit short-circuit changes upstream failure/data obligations.

Normally propagate the sole input NaN, or the first input when both are NaN,
preserving its payload/sign and setting the quiet bit. This priority applies
regardless of which NaN was signaling. A newly generated NaN uses the fixed
positive quiet pattern specified by NUM-04. Pow special rules are stated in its
individual file and override only their
explicitly stated cases. Minimum/maximum retain this NaN priority.

Confirmed operator-specific decisions:

- Add/subtract/multiply support all four dtypes and preserve dtype. Floating
  results are correctly rounded to that dtype; integer exact results outside
  UInt8/Int64 fail the complete invocation, without wrap, saturation
  or implicit promotion.
- Divide supports Float32/Float64 only, preserving dtype. Integer conversion is
  explicit; there is no implicit integer quotient mode.
- Minimum/maximum support all four dtypes and preserve dtype, with bitwise
  equivalent profiles. Mixed signed zeros choose -0 for minimum and +0 for
  maximum; same-sign zeros retain that sign. They propagate an input NaN. If both
  inputs are NaN, quiet and
  preserve the first input's payload/sign, following the common priority.

## Interface, demand and execution

Each operation has named inputs `a`, `b` and output `values`, except atan2/atan2pi
use `y` and `x` in that order. Rank is 1..8 with positive
extents; input shapes and dtypes match exactly. Output dtype follows the operator
file, and output facets are empty. No implicit broadcast, cast or unit inference
occurs. Compile/preflight rejects unsupported dtype, mismatched shape or unknown
static parameters before reading numeric inputs.

Every nonempty request collects and validates both complete inputs and computes
one complete packed owned output through a synchronous Whole callback. Empty Q
reads neither input and invokes no callback. Any changed source coordinate
invalidates all observed output coordinates. Special numerical identities retain
both input obligations. Typed validation covers complete inputs before arithmetic.

Read arbitrary legal immutable strides/offsets, including zero/negative strides,
unaligned elements and shifted origins. The executor projects the complete output
to the consumer's global coordinates. Integer overflow anywhere, including outside
Q, fails the complete invocation with Run scope and no Atom key. Failure releases
all unpublished output and scratch; already terminal observations keep their owners.

Inherit [NUM-04 execution conventions](NUM-04_unary_contract.md) for fenv,
rounding, ownership and cancellation. Capacity includes both full input collections,
N*b complete output bytes and one fixed arithmetic workspace. Work is O(N) for
simple operations plus explicitly charged refinement. Sparse requests may cost
more work and memory. Numeric per-value/fallback counters are N/A on this path.

Acceptance uses independent exact integer/rational or high-precision mathematical
oracles, not the implementation helper. Exercise all supported dtypes, signed
zeros, subnormals, extrema, NaN payload priority, valid strided inputs, disjoint
requests, typed validation, Whole overflow failure and resource/cancellation cleanup.
A conceptual WorkflowDocument fixture binds both arrays, compiles a selected key,
and reads requested values through ExecutionContext. Implementation delivery
provides actual runnable public fixtures and results as documented below.

## Current implementation comparison

Current add/subtract/multiply/divide use same-shape Float32/Float64 arrays, Whole
execution and a finite-only arithmetic helper. Current minimum/maximum use
Elementwise registration but also reject nonfinite data via their shared reader.
None of those legacy names establishes the newly proposed versioned contracts.

- [Current arithmetic helper](../../../../plugins/ops/01-numeric/numeric_algorithms.hpp).
- [Current minimum](../../../../plugins/ops/01-numeric/numeric_minimum.cpp).
- [Current maximum](../../../../plugins/ops/01-numeric/numeric_maximum.cpp).
- [Unary IEEE-style conventions](NUM-04_unary_contract.md).
- [NUM category](../core.md).

## Maintained implementation

All 27 keys are registered by `plugins/ops/01-numeric/numeric_binary.cpp`,
with independently named constructors in `photospider/numeric/binary.hpp`.
The shared Whole adapter retains and validates both complete inputs, including
when a numeric identity determines a result.
Floating elementary operations use controlled correctly rounded hardware
arithmetic after exact special-value classification, with exact fallback; integer
operations retain checked exact arithmetic. Accelerated ordinary positive-base
power and angle results use SLEEF binary64 enclosures within the shared admitted
ranges. atan2pi divides an angle enclosure by an enclosed pi. Only rejected
candidates dispatch the certified strict backend . See [math implementation](../math-implementation.md) for
rounding, scratch, work accounting and unresolved-refinement limits.

The [public example and commands](../../../../examples/numeric_workflow/README.md)
include this operation, an editable add/multiply composition, independent
integer/Fraction/MPFR oracles and direct error/resource checks. The manual target
is excluded from default builds and has no CTest/integration registration.

The combined family passed 14,174 independent cases per profile on native
Clang strict/Apple and Ubuntu WSL Clang strict/AVX2, plus expanded manual and
local installed-consumer checks. [Measured validation scope](../math-implementation.md#num-05-validation-and-native-timing)
records oracle versions, native timings and limitations.
