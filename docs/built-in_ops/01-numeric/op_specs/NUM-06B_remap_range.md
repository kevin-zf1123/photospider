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

All five operands are read and validated for each requested coordinate. Inherit
[binary execution conventions](NUM-05_binary_contract.md), extending exact Data
support Q and validation/invalidation to all five ports. No unrequested bounds
are checked. The common three independent CPU profiles apply.

## Numerical and validation rules

All four bounds must be finite. A NaN/infinite bound or source_lower>=source_upper
fails evaluation with InvalidArgument, FailureReason::InvalidDomain and diagnostic tag InvalidBounds, and the global
coordinate and offending port/value bits. Validate bounds even when input is
NaN. Valid bounds permit any floating input: quiet input NaN with payload/sign
preserved; map infinities according to the sign of the exact slope. A constant
target interval maps non-NaN infinities to the target constant.

For ordinary finite inputs, correctly round the entire exact rational formula
directly to output dtype once. All three CPU profiles are bitwise equivalent;
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
verify all CPU profiles bitwise. Execute shared disjoint support, validation,
invalidation, owner/cache, budget/cancellation and public entry-point cases when
implemented. Output storage covers requested coordinates only, with all actual
source and result capacities accounted under the shared contract.

This remains distinct from `encode_range`. The three versioned keys are
registered with closed matching-shape/input-dtype inference, pure metadata
validation and five explicit dynamic inputs.
Exact rational evaluation, endpoint precedence, invalid-bound diagnostics and
the public broadcast-to-remap-to-clamp workflow are implemented. Local strict
and Apple runs passed the 2826-case independent Fraction oracle, endpoint and
invalid-bound cases, sparse support, resource and cancellation cleanup.
Ubuntu WSL Clang 18 strict/x86 passed the same 2826-case oracle and public
workflows on 2026-09-14; the installed public consumer passed. Direct invocation
also preserved caller floating flags while a negative nonzero exact result
underflowed to negative zero. Independently reviewed adjacent-value rounding
checks covered 4198-bit numerators and exact half-way boundaries. This does not
change the Proposed status of this specification.
