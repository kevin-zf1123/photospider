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
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-06A: clamp

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Inputs `input`, `lower` and `upper` are dynamic arrays with exactly matching
shapes and dtypes. Support UInt8, Int64, Float32 and Float64; output `values`
preserves dtype and shape with empty facets. Shared limits are explicitly
broadcast to the array shape. There are no implicit casts or broadcasting,
and no static numeric limit parameters.

The three independently named CPU versions follow the common bitwise-equivalent
basic-operation rule. Read and validate all three complete operands for every nonempty request.
Empty requests read no payload and invoke no callback. Shape is rank 1..8 with positive
extents. Inherit [binary execution conventions](NUM-05_binary_contract.md),
extending Whole Data support and validation/invalidation to all three ports.

At every logical coordinate, validate lower<=upper. Equal limits and infinite
floating limits are allowed. A NaN limit or lower>upper fails the complete invocation;
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
full-input dependencies; an invalid bound outside the consumer projection also
fails the invocation. Failures have Run scope and no Atom key. No partial output
is published, under the inherited terminal rules.

Work is O(full logical elements); output bytes are full logical elements times
dtype size, even for sparse consumers. All input collections coexist with the
complete output and fixed arithmetic workspace. Any input edit invalidates all
observed output coordinates.
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

## Current implementation status

The three versioned keys use closed matching-shape/input-dtype inference and
pure metadata validation for their three inputs. The implementation reads
`input`, `lower` and `upper` arrays, validates bounds across the complete input, preserves selected bits and
reports `InvalidBounds` with coordinate and bound information. It uses raw
IEEE/integer order keys and shared scalar/NEON/AVX2 comparison facilities.
The public composition example is in `examples/numeric_workflow/ranges.cpp`.

Historical pre-Whole local strict/Apple runs passed the broadcast composition,
Atom-isolated invalid bounds, sparse support, upstream endpoint dependencies,
work limits and cancellation cleanup. Ubuntu WSL Clang 18
strict/x86 and the installed consumer passed that pre-Whole implementation on
2026-09-14; these are not validations of the current Whole path. Additional checks cover
recognized typed validation, bound cache edits, arbitrary strides, global ROI
origins and schema failures. This does not change the Proposed status of
this specification.

## Whole validation

The maintained synchronous callback uses no dependency maps or continuation.
Per-value numeric counters are N/A. The numerical comparison and rational engine
are unchanged; Scalar/NEON/AVX2 profile rules remain as specified. The local
strict/Apple Whole validation passed 2,826 independent Fraction/bit cases per
profile, public composition and error/layout/typed/cache/resource checks.
See [NUM-06 Whole measurements](../range-whole.md) for commands, memory costs,
public/core timings and profiler scope. This does not change Proposed status.
