---
spec_schema_version: 1
id: NUM-04-trigonometric
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

# NUM-04: shared trigonometric contract

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Sin, cos, tan, sinpi, cospi and tanpi each have their own specification and three independent
strict/Apple Silicon CPU accelerated/x86-64 CPU accelerated keys. Inherit the
[unary contract](NUM-04_unary_contract.md) and [exp quality/fallback requirements](NUM-04D_exp.md),
with each trigonometric function's own formula and special-value table.

Input/output are same-dtype Float32/Float64, with unchanged shape and generic
output facets. Integer inputs require explicit cast. There are no static numeric
parameters. The operation name determines argument interpretation; no angle_unit
parameter or unit-dispatch alias is retained.

## Angle interpretation and exact landmarks

For sin/cos/tan, the mathematical argument is the exact real value of the supplied
float. There is no angle snapping, epsilon recognition, degree conversion or
pretence that a rounded floating pi equals mathematical pi. All finite arguments,
including the largest finite values, are within the selected operation's input
scope; unverified accelerated reduction ranges use strict fallback.

For sinpi/cospi/tanpi, x means the exact real angle pi*x. Never first multiply by
a rounded pi in the input dtype. Exact reduction of the binary rational multiplier
modulo two (sin/cos) or one (tan) handles enormous integers and half-integers
without converting their value to Int64 or overflowing an intermediate angle.

The landmark conventions are explicit in every profile:

- Sinpi at integer multipliers returns zero with the multiplier's sign, including
  -0. At n+1/2 it returns exactly (-1)^n.
- Cospi at integer multipliers returns exactly (-1)^n. At half-integers it returns
  +0.
- Tanpi at integer multipliers returns zero with the multiplier's sign. At
  n+1/2 it returns the fixed positive quiet NaN, as explicitly selected by the
  maintainer; no one-sided infinity is chosen.
- Tanpi at n+1/4 and n+3/4 returns exactly +1 and -1 respectively.
- For quarter-integer sinpi/cospi landmarks, correctly round the appropriate signed
  sqrt(1/2) directly to the output dtype. This is an exact special-angle path,
  not an arbitrary snapped radian value.

The zero-sign conventions are normative; numerical oddness/periodicity must not
be upgraded into an incompatible bit-level zero-sign identity. Nonfinite inputs
are classified before angle reduction: ±Inf -> fixed positive quiet NaN, input
NaN -> quiet payload/sign-preserving NaN. These are successful numeric results.

## Numerical quality and implementation

Strict returns the correctly rounded mathematical sin/cos/tan of the selected
exact argument, directly in output dtype. Accelerated nonzero finite values
are within four FP32-scaled representable steps of strict; NaN/Inf/zero
classification and sign, and the named landmark paths, match strict exactly.
For finite normal/subnormal neighbors the ULP allowance applies normally.

Range reduction accuracy is part of the final error bound. Reduction using
only a rounded pi or 2*pi is not a sufficient full-range algorithm. Publish
verified backend/library versions and covered reduction domains; otherwise use
strict. Correct-rounding reference work can refine directed enclosures until
rounding is resolved, with explicit treatment of exact boundaries and landmarks.
All precision growth, reduction work and fallback is host-accounted; exhaustion
is ResourceExhausted rather than an inaccurate result or a silent range limit.

The common ROI, typed-validation, owner/cache, cancellation and diagnostic
requirements apply. Work is per full-input sample plus actual reduction/refinement
cost, with actual temporary capacities charged. Platform-specific keys do not
execute on incompatible platforms. No GPU/Metal or private worker pool is added.

## Shared acceptance

Use an independent high-precision directed oracle with exact input-float
interpretation and separately exact pi-multiple semantics. Strict compares bits;
accelerated compares ULP distance plus exact special-value/landmark results.
Include huge arguments, neighbors of reduction boundaries, signed subnormals,
NaN payloads, and midpoint rounding cases. A sampled small-angle test does not
establish a full-range backend bound.

For pi multiples, test positive/negative large integers, half/quarter-integers
and neighboring representable values. Check that radian pi rounded to dtype is
not silently treated as the exact pi-multiple input 1. Validate actual strict
fallback for unsupported fast argument ranges and common public execution,
Whole demand and lifetime/resource cases. Implementation evidence is recorded
separately below; Proposed specification status does not imply missing runtime keys.


## LLVM libc candidate backend (source review, 2026-09-14)

LLVM libc provides a CPU `sinpif` entry point delegating to its internal
`math::sinpif(float)` implementation. Its [official accuracy table](https://libc.llvm.org/headers/math/index.html)
marks Float32 sinpi/cospi/tanpi as correctly rounded in all four rounding modes;
the corresponding Float64 cells are empty at the time of this review. Thus the
Float32 implementations are candidates for strict as well as accelerated
profiles. This does not select a Float64 backend or establish a Photospider
platform conformance result. Clang compiler selection alone does not select
LLVM libc as the linked math library.

The [sinpif implementation](https://github.com/llvm/llvm-project/blob/main/libc/src/__support/math/sinpif.h)
reduces the multiplier before polynomial/table evaluation and handles large
integer inputs directly. Its signaling-NaN branch returns a canonical NaN;
the operator adapter must preserve the specified input payload/sign instead.
It must also enforce this contract's landmarks, floating environment and
resource behavior independently of a candidate library's defaults.

The upstream [sinpif exhaustive test source](https://github.com/llvm/llvm-project/blob/main/libc/test/src/math/exhaustive/sinpif_test.cpp)
uses an MPFR Sinpi oracle and exercises rounding modes. This source review did
not run those tests. Adoption requires a pinned LLVM revision, recorded build
options and target features, and actual conformance checks on both CPU targets;
mutable upstream links describe candidate evidence, not a frozen dependency.

## Maintained implementation and validation

The maintained keys are registered in `plugins/ops/01-numeric/numeric_unary.cpp`
and exposed through `photospider/numeric/unary.hpp`. Bit-level special cases
precede controlled hardware elementary arithmetic. Strict transcendental results
use directed Q128..Q4096 enclosures. Accelerated ordinary results use private
SLEEF binary64 kernels and conservative final-error checks within the
[admitted ranges](NUM_accelerated_contract.md#image-budget-and-extended-domains).
Pi and rational-pi arguments undergo exact quadrant reduction before approximation.
Rejected candidates use strict evaluation . Unresolved
strict rounding may return `ResourceExhausted`. Nonempty requests use Whole execution with full-input typed validation,
complete packed output allocation and Run-scoped arithmetic errors. Empty requests
read no payload. Per-value fallback/evaluation diagnostics are unavailable (N/A)
on this callback path; numerical fallback behavior is unchanged.

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.0-p12; revalidated 2026-09-21)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
