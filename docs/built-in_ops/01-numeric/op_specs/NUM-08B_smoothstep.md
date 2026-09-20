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
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
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
endpoint neighbors, signed zeros and subnormals. The public WorkflowDocument
fixture and shared disjoint/strided/resource/validation/lifetime cases are
covered by the manual target described below.

The three versioned keys implement the generic dynamic-edge contract through
the existing `MatchAllInputs` and `validate_dependency` path; no metadata
specialization callback is used. The exact cubic uses a 104-limb workspace
(6656 bits), scalar `u128` multiplication and profile-specific NEON/AVX2
comparison helpers, with every temporary owned by the host continuation.

The public interpolation workflow checks midpoint and endpoint semantics,
invalid-edge atom isolation, sparse all-port support, upstream edge failure,
typed closure, cache/layout behavior, empty demand, sNaN/floating-environment
preservation and bounded refinement cleanup. Local strict and Apple profile
runs passed 5242 independent Fraction cases per profile. Ubuntu WSL Clang
strict/x86 passed the same oracle and manual checks; the installed consumer
also passed locally. The status is implementation
evidence only and does not change this specification's Proposed status; the
workflow is a manual target with no CTest or integration-test registration.

The existing `field.smoothstep` remains a distinct rank-2 operation with static
Float64 edges and Float32 coverage output. The versioned `numeric.smoothstep`
keys above provide this dynamic-edge, dtype-preserving exact contract.
