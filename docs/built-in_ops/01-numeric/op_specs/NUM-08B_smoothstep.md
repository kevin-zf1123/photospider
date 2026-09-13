---
spec_schema_version: 1
id: NUM-08B
parent_id: NUM-08
function: smoothstep
proposed_operation_keys:
  - numeric.smoothstep_strict
  - numeric.smoothstep_accelerated_apple_silicon
  - numeric.smoothstep_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-08B: smoothstep

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Generate a cubic transition from zero to one. Dynamic inputs in order are
`input`, `edge0`, `edge1`, with identical rank-1..8 positive shape and dtype,
Float32 or Float64. Output `values` preserves shape/dtype with empty facets.
There are no static numeric parameters, implicit broadcasts/casts or color/alpha
inference. Shared scalar edges use explicit broadcast. The polynomial is cubic;
no quintic smootherstep mode is part of this operation.

## Numeric contract

Read all three inputs at each requested coordinate and validate both edges as
finite with edge0<edge1. Invalid edges fail even when input is NaN. With valid
edges, quiet an input NaN preserving payload/sign; otherwise:

- input<=edge0 yields +0, including input=-Inf.
- input>=edge1 yields +1, including input=+Inf.
- An interior finite input yields RN_dtype(t*t*(3-2*t)), where
  t=(input-edge0)/(edge1-edge0) is an exact mathematical rational.

There is only final destination rounding; no intermediate rounded t or polynomial
steps define the result. All three CPU profiles are bitwise equivalent, with
outputs in [0,1]. The exact source midpoint yields 0.5. Interior values rounding
to zero yield +0. Positive source width must not overflow merely because naive
subtraction would exceed the floating range.

Use exact rational arithmetic or a certified equivalent with correct final
rounding, including hard boundaries. Work is per requested sample plus actual
exact/refinement work; charge all temporary capacities. Exhausted work/memory
budgets yield ResourceExhausted, never an unverified approximation.

## Execution and errors

Inherit [binary execution conventions](NUM-05_binary_contract.md), extending
exact Q Data support to all three ports. Typed-validation support is separate;
empty Q reads nothing, and no unrequested invalid edge causes failure. Endpoint
clamping still requires both edge dependencies and validation. Output inference
uses input metadata; returned fragments own packed storage with correct global
origins. Changes to any retained input/validation support invalidate the affected
observations. Common floating-environment, cache, ownership and cancellation
rules apply, including checks at least every 64 simple elements and refinement
steps, and release of unpublished work on failure.

Compile/preflight rejects unsupported dtype, shape mismatch or unexpected
parameters. Dynamic invalid edges fail with InvalidArgument, FailureReason::InvalidDomain and diagnostic tag InvalidEdges,
global coordinate and offending edge bits. Valid generic input NaN/Inf produces
a successful numeric output. Other upstream/typed/resource/cancellation failures
use the inherited Status/observation publication rules.

## Acceptance and current implementation

Conceptual fixture: input=[-1,0,0.25,0.5,0.75,1,2] with broadcast edges 0/1
yields [0,0,0.15625,0.5,0.84375,1,1]. Use exact rational final rounding as an
independent oracle. Include input ±Inf, NaN payloads, equal/reversed/nonfinite
edges concurrent with NaN input, enormous edge spans, tiny spans, midpoint and
endpoint neighbors, signed zeros and subnormals. Execute public WorkflowDocument
fixtures and shared disjoint/strided/resource/validation/lifetime cases when
implemented; no runtime test is claimed here.

The existing [field.smoothstep](../../../../plugins/ops/01-numeric/field_smoothstep.cpp)
uses rank-2 Float32/Float64 input, static Float64 edges and Float32 coverage output.
It does not implement this generic dynamic-edge, dtype-preserving contract or
establish whole-formula correct rounding. New versioned keys remain unimplemented.
