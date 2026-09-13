---
spec_schema_version: 1
id: NUM-06A
parent_id: NUM-06
function: clamp
proposed_operation_keys:
  - numeric.clamp_strict
  - numeric.clamp_accelerated_apple_silicon
  - numeric.clamp_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: target_contract_not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-06A: clamp

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inputs `input`, `lower` and `upper` are dynamic arrays with exactly matching
shapes and dtypes. Support UInt8, Int64, Float32 and Float64; output `values`
preserves dtype and shape with empty facets. Shared limits are explicitly
broadcast to the array shape. There are no implicit casts or broadcasting,
and no static numeric limit parameters.

The three independently named CPU versions follow the common bitwise-equivalent
basic-operation rule. Read all three operands at each requested coordinate;
unrequested coordinates are not evaluated. Shape is rank 1..8 with positive
extents. Inherit [binary execution conventions](NUM-05_binary_contract.md),
extending exact Q Data support and validation/invalidation to all three ports.

At every requested coordinate, validate lower<=upper. Equal limits and infinite
floating limits are allowed. A NaN limit or lower>upper fails that observation;
an input NaN does not hide invalid bounds. With valid bounds, an input NaN
preserves payload/sign and is quieted under the common policy.

After valid bounds and input-NaN handling, choose lower when input<lower,
upper when input>upper, otherwise return input. Comparisons treat signed zeros
as equal; an in-range input retains its bits, including -0. In particular
clamp(-0,+0,+0)=-0. This is not defined by composing minimum and maximum.
No selected integer or finite floating value is converted or rounded.

## Errors, resources and acceptance

Unsupported dtypes or mismatched shapes fail compile/preflight. Invalid dynamic
bounds fail evaluation with InvalidArgument and FailureReason::InvalidDomain and diagnostic tag InvalidBounds, reporting
the global coordinate and offending bound port/value bits. Validate all three
input dependencies; an unrequested invalid bound has no effect. A failed
observation publishes no partial output, under the inherited terminal rules.

Work is O(requested elements); output bytes are requested elements times dtype
size plus actual fragment overhead. Bound validation adds no Whole-array pass.
Temporary state is bounded per processing chunk; account actual source owners,
output/scratch and validation closure under the host budgets. Preserve floating
environment, check cancellation at least every 64 elements, and release
unpublished allocations on failure.

Conceptual public fixture: input=[-1,0.5,2], lower=[0,0,0], upper=[1,1,1]
returns [0,0.5,1]. Bind all inputs in WorkflowDocument and check values through
ExecutionContext when implemented. Repeat supported dtype fixtures, UInt8/Int64
extrema, equal bounds, signed-zero permutations, infinite limits/input, input
NaN payloads and invalid bounds concurrent with input NaN. Use exact comparison
and bit-selection as an independent oracle. Verify disjoint support, typed
validation, invalidation, lifetime, resource exhaustion and cancellation as
defined by the shared execution contract.

## Current implementation gap

The current legacy numeric.clamp instead uses one Float32/Float64
input with static Float64 min/max parameters and finite-only Whole execution.
It does not implement this new dynamic limit contract.

These versioned keys remain unimplemented. No public runtime test is claimed.
