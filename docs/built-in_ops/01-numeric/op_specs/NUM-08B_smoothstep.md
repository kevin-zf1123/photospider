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

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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

Read all three complete inputs for every nonempty request and validate both edges as
finite with edge0<edge1. Invalid edges fail even when input is NaN. With valid
edges, quiet an input NaN preserving payload/sign; otherwise:

- input<=edge0 yields +0, including input=-Inf.
- input>=edge1 yields +1, including input=+Inf.
- An interior finite input yields RN_dtype(t*t*(3-2*t)), where
  t=(input-edge0)/(edge1-edge0) is an exact mathematical rational.

There is only final destination rounding; no intermediate rounded t or polynomial
steps define the result. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound, with
outputs in [0,1]. The exact source midpoint yields 0.5. Interior values rounding
to zero yield +0. Positive source width must not overflow merely because naive
subtraction would exceed the floating range.

Use exact rational arithmetic or a certified equivalent with correct final
rounding, including hard boundaries. Work is per full logical sample plus actual
exact/refinement work; charge all temporary capacities. Exhausted work/memory
budgets yield ResourceExhausted, never an unverified approximation.

## Execution and errors

Inherit [binary execution conventions](NUM-05_binary_contract.md), extending
Whole support to all three inputs. All inputs receive complete typed validation.
Empty reads no payload and invokes no callback. Invalid edges outside a consumer
projection also fail the whole invocation, with Run scope and no Atom key.
Endpoint clamping retains both edge obligations. Any input edit invalidates all
observed outputs. Metadata inference is unchanged; a complete packed owned output
is allocated before consumer projection. Capacity includes three full input
collections, N × dtype-width output and one fixed interpolation workspace.
Sparse consumers may require more memory and work. Check cancellation every
element, inside exact arithmetic and before publication; release unpublished
owners on failure. Per-value numeric diagnostics are N/A.

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
synchronous Whole execution and the existing matching-input metadata rules.
Exact cubic construction uses a 104-limb workspace
(6656 bits), scalar `u128` multiplication and profile-specific NEON/AVX2
comparison helpers, with every temporary owned by the callback allocator.
Accelerated final quotient candidates must pass a conservative final-error gate;
unresolved results use exact rounding. Project accepted values onto the proven
[0,1] output range to preserve the upper/lower edge contract.

The current public workflow checks midpoint/endpoint semantics, Whole invalid
edges outside the projection, full support, upstream edge failure, typed input
validation, cache/layout behavior, Empty, sNaN/fenv preservation and resource
cleanup. Local strict/Apple Whole runs pass 5,244 independent Fraction cases per
profile. Historical WSL and installed-consumer passes were for the pre-Whole
implementation. Current commands and timings are in
[NUM-08 measurements](../interpolation-whole.md). Specification status remains
Proposed; the manual target has no CTest/integration registration.

The existing `field.smoothstep` remains a distinct rank-2 operation with static
Float64 edges and Float32 coverage output. The versioned `numeric.smoothstep`
keys above provide the dynamic-edge, dtype-preserving contract with exact strict
rounding and the shared accelerated final FP32 bound.
