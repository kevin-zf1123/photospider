---
spec_schema_version: 1
id: NUM-04Q
parent_id: NUM-04
function: sinc
proposed_operation_keys:
  - numeric.sinc_strict
  - numeric.sinc_accelerated_apple_silicon
  - numeric.sinc_accelerated_x86_64
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

# NUM-04Q: sinc

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

For finite nonzero x, compute the mathematical `sin(x)/x`. Interpret the
input float as its exact real value; pi denotes exact mathematical pi.
Extend the function at both signed zeros by +1 and at both infinities by +0.
Input NaNs preserve payload/sign and are quieted. All these are successful
numeric outputs. This is an independently named operation with no normalization
or angle-unit parameter.

## Interface and numerical quality

One input `input` and output `values` have identical shape and dtype, Float32 or
Float64. Integer inputs require explicit cast. Output facets are empty. Inherit
all [unary contract](NUM-04_unary_contract.md) requirements for exact regional
demand, typed validation, ownership, invalidation, errors, cache, resources and
cancellation. Both platform accelerated keys reject incompatible platforms.

Strict correctly rounds the entire mathematical quotient directly to the output
dtype. It is not defined as a rounded sine followed by a rounded division.
Accelerated finite nonzero results permit at most four FP32-scaled ULP from
strict; NaN/Inf/zero classification and sign and specified landmarks match strict
exactly. Use [exp's fallback rules](NUM-04D_exp.md) when an approximation cannot
meet the bound, reporting strict fallback. Insufficient refinement or memory
budget yields ResourceExhausted rather than an unverified numeric result.

Avoid spurious intermediate overflow and loss of accuracy around zero. A stable
series or bounded approximation may be used, but acceptance concerns the whole
function. In the normalized function, constructing pi*x in the input dtype is
not a full-range algorithm. A correctly rounded sinpi backend alone does not
establish correctly rounded sincpi or a four-ULP final quotient.

The function is even for real inputs. Nonzero exact results that underflow to
zero retain their mathematical sign. NaN sign preservation follows the common
bit policy independently of mathematical evenness.

## Acceptance and status

Use an independent directed high-precision oracle for the whole quotient,
refining until destination rounding is resolved. Test ±0 -> +1, ±Inf -> +0,
NaN payloads, tiny subnormals, the transition away from the flat region around
one, largest finite inputs, oscillatory sign changes and hard rounding cases.
Verify exact special bits and accelerated final error, not just an internal
sine error. Apply shared strided/disjoint demand, resource, typed-validation,
lifetime and cancellation tests through actual public WorkflowDocument execution
through the maintained public workflow; local timing is recorded in the implementation notes.

There are no finite nonzero binary floating inputs that are exact roots of
sin(x). Rounded approximations to multiples of pi are not snapped to zero.

Conceptual public fixture: [-0,+0,-Inf,+Inf] -> [1,1,+0,+0], with
NaN fixtures checked separately using exact input payload bits.

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
