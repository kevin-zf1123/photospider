---
spec_schema_version: 1
id: NUM-04N
parent_id: NUM-04
function: sinpi
proposed_operation_keys:
  - numeric.sinpi_strict
  - numeric.sinpi_accelerated_apple_silicon
  - numeric.sinpi_accelerated_x86_64
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

# NUM-04N: sinpi

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Compute the mathematical sin(pi*x) using exact pi and the exact real value
of the input float. Inherit the [trigonometric contract](NUM-04_trigonometric_contract.md)
and [unary contract](NUM-04_unary_contract.md), including execution, resources,
NaN handling, platform availability, strict fallback and acceptance obligations.

One input `input` and one output `values` have identical shape and dtype,
Float32 or Float64. Output facets are empty. There are no static numeric
parameters, implicit casts or rounded-pi multiplication in the definition.

## Function-specific values and quality

| Input | Output |
| --- | --- |
| Integer x, including signed zero | Zero with sign(x) |
| n+1/2 | Exactly (-1)^n |
| ±Inf | Fixed positive quiet NaN; success |
| NaN | Quiet input NaN preserving payload/sign |

Strict rounds the exact mathematical function directly to output dtype.
Accelerated finite nonzero results allow at most four FP32-scaled ULP;
classification, zero signs and named landmarks match strict exactly. All
finite inputs are supported, including large integers and subnormal inputs.
The shared contract defines exact quarter-angle rounding and zero conventions.

Output remains in [-1,1] in every profile while meeting the error bound.

## Acceptance and current status

Conceptual public fixture: `[0,0.5,1,-0.5,-1]` -> `[+0,1,+0,-1,-0]`. Bind the array to input in a
WorkflowDocument, compile each selected key and inspect values through
ExecutionContext in both dtypes. Validate the exact bit patterns at landmarks,
huge positive/negative integers, adjacent representable values around half and
quarter integers, subnormals, NaN payloads and signed zeros. Use an independent
high-precision oracle for ordinary arguments and midpoint-sensitive cases.

Apply shared disjoint demand, typed-validation closure, invalidation, ownership,
cache, cancellation and resource checks. Verify unsupported accelerated ranges
actually use strict fallback and report it. Float32 LLVM libc is a candidate
as documented in the shared contract; it supplies no implicit Float64 coverage.
The maintained public run command and current validation boundary are documented below;
local timing is recorded in the implementation notes.

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
