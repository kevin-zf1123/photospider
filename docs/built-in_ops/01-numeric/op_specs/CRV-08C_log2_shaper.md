---
spec_schema_version: 1
id: CRV-08C
parent_id: CRV-08
function: log2_shaper
proposed_operation_keys:
  - curve.log2_shaper_strict
  - curve.log2_shaper_accelerated_apple_silicon
  - curve.log2_shaper_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
clarification_status: complete
---

# CRV-08C: log2_shaper

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

## Interface and formula

Ordered inputs are input, lower and upper Results, each with one tensor member under any schema id/version/key. Complete `sample_shape()` includes batch axes. Input is Float32/Float64, rank 1..8, positive extents and <=2^40 elements. Bounds are same-dtype [1] scalars. Output `values` uses `photospider.tensor` v1/member `samples`, preserving complete input shape/dtype as ordinary axes with no domain description.

The exact mathematical formula is (log2(input)-log2(lower))/(log2(upper)-log2(lower)), rounded as one expression. There are no static mode, dtype or clipping parameters. Bounds must be finite and satisfy 0<lower<upper as an unchecked caller precondition. Input IEEE special values remain part of the valid numerical interface; overflow/underflow are successful IEEE results.

Inherit the [family contract](CRV-08_shaper.md#mathematical-formulas-and-ieee-results) for exact endpoints, NaN quieting/payload/sign, infinity and zero extensions, monotonicity and profile precision. x=lower selects +0 and x=upper selects 1. Both signed zeros map to -Inf; negative inputs map to canonical quiet NaN; +Inf maps to +Inf. These transformations do not change a color description or perform tone mapping.

## Fixed execution and errors

Each selected output tile reads its matching input region and both scalar bounds through their source grids. It computes only selected global cells, uses preallocated ExactShaper scratch and writes the declared output tile. Metadata and exact read/write/scratch signatures are verified during preparation. Empty reads no payload. The [fixed family contract](CRV-08_shaper.md#fixed-log-execution) defines layout support, source closure, resource admission, Run ownership and ordered call publication.

The engine preserves whole-expression exact/directed arithmetic and uses fixed state for retry, range and terminal failures. Refinement is bounded to 128..4096 fractional bits; unresolved precision or capacity returns ResourceExhausted. Cancellation remains terminal. Numerical bound preconditions have no diagnostic/precedence guarantee; a nonzero-significand guard protects integer decomposition. Shape/dtype mismatch is TypeMismatch, and unsupported profiles fail BackendUnavailable.

## Public acceptance

L=1,U=16, input=[0.5,1,2,4,16,32] gives [-0.25,0,0.25,0.5,1,1.25]. Author the node with `log2_shaper_node`, bind Results and execute through Compiler/ExecutionContext. Compare against the independent whole-formula MPFR/Fraction reference, including IEEE payloads, exact landmarks, extremes, subnormal results and monotonic groups. Rounded round trips alone are insufficient.

Check selected reads/allocation, both scalar dependencies, source-grid closure, signed/zero/unaligned layouts, malformed preparation, fenv, concurrent rebinding, resource limits, cancellation and output lifetime. See [family acceptance](CRV-08_shaper.md#acceptance-and-implementation). Keep numerical proof separate from public execution and report profiles actually exercised.
