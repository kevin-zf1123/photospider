---
spec_schema_version: 1
id: NUM-06B
parent_id: NUM-06
function: remap_range
proposed_operation_keys:
  - numeric.remap_range_strict
  - numeric.remap_range_accelerated_apple_silicon
  - numeric.remap_range_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_manual_acceptance
repository_branch: ops-impl
repository_commit: current working tree
---

# NUM-06B: remap_range

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Map a source interval to a target interval, linearly extrapolating outside the
source interval. Inputs in port order are `input`, `source_lower`, `source_upper`,
`target_lower`, `target_upper`. All five are dynamic arrays of the same shape
and dtype, Float32 or Float64. Output `values` preserves dtype/shape with empty
facets. There are no static numeric parameters or implicit broadcasts/casts.
Shared scalar bounds use explicit broadcast. Clipping uses a separate clamp.

Source bounds must satisfy source_lower<source_upper. Target bounds may be
increasing, decreasing or equal. Mathematical formula for finite inputs is

    target_lower + (input-source_lower) *
        (target_upper-target_lower) / (source_upper-source_lower)

All five complete operands are read and validated for every nonempty request.
Inherit [binary execution conventions](NUM-05_binary_contract.md), extending
Whole support and invalidation to all five ports. Invalid bounds outside a
consumer projection also fail the invocation with Run scope and no Atom key.
Empty reads no payload and invokes no callback. The common three independent CPU profiles apply.

## Numerical and validation rules

All four bounds must be finite. A NaN/infinite bound or source_lower>=source_upper
fails evaluation with InvalidArgument, FailureReason::InvalidDomain and diagnostic tag InvalidBounds, and the global
coordinate and offending port/value bits. Validate bounds even when input is
NaN. Valid bounds permit any floating input: quiet input NaN with payload/sign
preserved; map infinities according to the sign of the exact slope. A constant
target interval maps non-NaN infinities to the target constant.

For ordinary finite inputs, correctly round the entire exact rational formula
directly to output dtype once. Strict is bitwise reproducible; accelerated floating results use the shared FP32-scaled bound;
intermediate rounded subtraction/multiplication/division does not define the
operation. Mathematical output overflow yields the correctly signed infinity
as a successful numeric result; gradual underflow preserves the result sign.
Exact non-special zero results are +0.

After bounds validation and input-NaN propagation, equal target bounds return
target_lower bits, taking precedence over endpoint selection and infinite input.
This includes opposite-sign target zeros. Otherwise exact source endpoints
return their corresponding target endpoint bits, then infinite inputs follow
the slope rule, then ordinary finite inputs use the exact formula.

Use exact dyadic/rational arithmetic or a certified equivalent to avoid spurious
intermediate overflow. Charge exact arithmetic scratch/work and return
ResourceExhausted if budgets are insufficient. Read all five sources even for
endpoint and constant paths. Check cancellation during extended arithmetic and
at least every 64 simple samples; publish no partial failed observation.

## Acceptance and current status

Conceptual public fixture: input=[0,0.5,1,2], source bounds=[0,1] and target
bounds=[0,255] broadcast to shape [4] produces [0,127.5,255,510]. Test descending
and constant targets, exact endpoints, signed zeros, infinities and NaN payloads,
invalid bounds concurrent with input NaN, near-equal source bounds, output
overflow/subnormals and representable results with overflowing naive differences.
Use an independent exact rational oracle with direct Float32/Float64 rounding;
verify strict bits and accelerated final FP32-scaled accuracy. Execute shared disjoint support, validation,
invalidation, owner/cache, budget/cancellation and public entry-point cases when
implemented. Output storage covers the complete logical array, then the executor projects
the consumer coordinates. Account five full input collections, complete output
and fixed scratch; sparse consumers may require substantially more memory/work.

This remains distinct from `encode_range`. The three versioned keys are
registered with closed matching-shape/input-dtype inference, pure metadata
validation and five explicit dynamic inputs.
Exact rational numerator/denominator construction uses a bounded final hardware
quotient in accelerated profiles only when its enclosure passes the final-error
gate; unresolved cases use exact rounding. Whole execution,
endpoint precedence, invalid-bound diagnostics and
the public broadcast-to-remap-to-clamp workflow are implemented. Historical pre-Whole local strict/Apple runs passed the 2826-case Fraction
oracle, endpoint/invalid-bound cases, sparse support and resource cleanup.
Ubuntu WSL Clang 18 strict/x86 passed the same oracle and pre-Whole workflows
on 2026-09-14; the installed consumer passed that earlier implementation.
Neither is evidence for the current Whole execution path. Direct invocation
also preserved caller floating flags while a negative nonzero exact result
underflowed to negative zero. Independently reviewed adjacent-value rounding
checks covered 4198-bit numerators and exact half-way boundaries. This does not
change the Proposed status of this specification.

## Whole validation

The maintained synchronous callback uses no dependency maps or continuation.
Per-value numeric counters are N/A. The numerical comparison and rational engine
are unchanged; Scalar/NEON/AVX2 profile rules remain as specified. The local
strict/Apple Whole validation passed 2,826 independent Fraction/bit cases per
profile, public composition and error/layout/typed/cache/resource checks.
See [NUM-06 Whole measurements](../range-whole.md) for commands, memory costs,
public/core timings and profiler scope. This does not change Proposed status.
