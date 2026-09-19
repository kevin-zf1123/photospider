---
spec_schema_version: 1
id: NUM-04I
parent_id: NUM-04
function: floor
proposed_operation_keys:
  - numeric.floor_strict
  - numeric.floor_accelerated_apple_silicon
  - numeric.floor_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-04I: floor

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

For a finite floating x, return the greatest integer less than or equal to x,
stored in the original floating dtype. UInt8 and Int64 are exact identity.
Shape and dtype are preserved; there are no operation parameters or implicit
conversion to an integer output array. Inherit the
[NUM-04 common contract](NUM-04_unary_contract.md) for the input/values interface,
regional/typed-validation support, output facets, resources and lifetime.

## Exact semantics

Both input signed zeros keep their sign. Any finite zero result uses the input
sign. Positive finite inputs below one yield +0; negative fractional values yield the next lower integer.
Infinities return unchanged; input NaNs are quieted with payload/sign preserved.
All generic numeric inputs have a successful numeric result. Integer extrema
are copied exactly and are never passed through Float64.

All three versions must have identical bits. Implement directed integral
rounding through a verified primitive or exponent/fraction-bit logic; the result
does not depend on the caller's current rounding mode. Large finite floating
numbers whose representation is already integral return unchanged. Special
inputs are classified by bits first; no NaN payload conversion is allowed.
No arithmetic overflow or ULP tolerance is needed for this operation.

## Resources and acceptance

Work is O(M), element scratch O(1) and output payload M*b, plus common mapping
and typed-validation costs. SIMD must preserve zero signs and NaN quieting and
must not evaluate outside requested coverage. Inherited sticky failure,
cancellation, cache-off and final-owner rules apply.

Analytic public fixture: [-1.5,-0,+0,0.25,1.5] -> [-2,-0,+0,+0,1].
Verify output dtype is unchanged. Cover all UInt8 values, Int64 values above
2^53 and extrema, floating neighbors of integers, subnormals, signed zeros,
infinities and NaN payloads. Compare to an independent rational integral oracle
and explicit bit handling; check all three keys bitwise and apply shared public
ROI/read-witness/lifetime/resource validation.

The maintained implementation is in the numeric operator registry. Its public
WorkflowDocument -> Compiler -> ExecutionContext run command is documented below.
This specification alone does not claim a product test run.

## Maintained implementation and validation

This operation is registered in `plugins/ops/01-numeric/numeric_unary.cpp` and
exposed through `photospider/numeric/unary.hpp`. It uses exact pointwise Data,
separate typed validation and Atom-scoped failures. Its numerical path follows
[the shared implementation notes](../math-implementation.md).

The [public workflow and commands](../../../../examples/numeric_workflow/README.md)
cover this operation. The combined NUM-04 family suite passed 7,524 independent
integer/Fraction/MPFR cases per profile: Clang 21 strict/Apple locally (MPFR 4.2.2)
and Clang 18 strict/AVX2 in Ubuntu WSL (MPFR 4.2.1). Expanded manual checks and
local installed consumers passed. [Validation and native timing](../math-implementation.md#num-04-validation-and-native-timing)
record the scope and limitations. Manual targets have no CTest/integration
registration; MPFR is used only by the independent Python oracle.
