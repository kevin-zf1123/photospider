---
spec_schema_version: 1
id: NUM-15A
parent_id: NUM-15
function: derivative_1d
proposed_operation_keys:
  - numeric.derivative_1d_strict
  - numeric.derivative_1d_accelerated_apple_silicon
  - numeric.derivative_1d_accelerated_x86_64
category: 01-numeric
kind: primitive
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
repository_branch: ops-specs
repository_commit: 30478d33
---

# NUM-15A: derivative_1d

Inherit the [NUM baseline](NUM_common_contract.md) for specification status,
registration, shared execution and acceptance requirements; explicit rules below
and in the named family contract take precedence.

Differentiate one-dimensional samples using dynamic inputs `samples` [N] and
`step` [1], both Float32 or both Float64. Require 2<=N<=2^40 and finite nonzero
step, allowing negative step. Output values has shape [N], preserves dtype and
has empty facets. The starting coordinate does not affect this operation;
callers preserve or connect sampling-axis information separately. There are no
static numeric parameters or implicit casts/axis extraction.

## Difference formula and numeric semantics

Use first-order one-sided endpoints and central interior differences:

    D[0]   = (samples[1]-samples[0]) / step
    D[N-1] = (samples[N-1]-samples[N-2]) / step
    D[i]   = (samples[i+1]-samples[i-1]) / (2*step), 0<i<N-1

For N=2 both results use the same one-sided quotient. There is no edge-order
parameter or automatic higher-order stencil. For finite samples, evaluate the
whole rational quotient exactly and correctly round once to output dtype; all
three CPU versions are bitwise equivalent. Neither an overflowing naive
subtraction nor 2*step intermediate changes the mathematical result.

Exact zero results are +0. Nonzero exact results that underflow retain their sign;
final overflow yields signed infinity successfully. Among required samples,
propagate the first NaN in ascending source index order, quieting it and preserving
payload/sign. Without source NaNs, same-sign infinities in the subtraction yield
fixed positive quiet NaN, otherwise subtraction/division determine the infinity
sign. Step is finite and nonzero, so it cannot introduce a numeric NaN after
successful validation. Fixed NaN bits follow [NUM-04](NUM-04_unary_contract.md).

## Dependencies, errors and invalidation

For a nonempty output request, first read/validate step[0]. Invalid zero/nonfinite
step fails with InvalidArgument, FailureReason::InvalidDomain and diagnostic
InvalidSampleStep, before source samples are evaluated. Retain that Control and
validation witness. Empty Q reads neither step nor samples.

Each requested output reads only its stated two-point stencil. Union those
sample coordinates exactly, with recognized typed Validation closure separately.
The center sample is not part of an interior stencil; its invalid numeric value
does not affect that output unless another requested stencil or typed-validation
obligation requires it. Other samples are not evaluated. Source changes invalidate
precisely output stencils containing the changed index; step changes invalidate
all derivative outputs. Metadata/profile and retained witnesses form cache identity.

## Algorithms, resources and acceptance

Read arbitrary legal source/control strides and byte offsets. Use exact dyadic
subtraction/division or a certified equivalent, charging actual arithmetic and
source/set metadata work. Cost is O(requested stencils) plus exact arithmetic;
deduplicate overlapping source reads. Return owned packed requested fragments
with correct global origins, not a full derivative array for a sparse request.
Check cancellation per processing block and refinement. Resource/upstream/typed
failures retain their categories; failed observations publish no partial result.
Cache-off and result lifetime follow common immutable owner requirements.

Conceptual fixture: samples=[0,1,4], step=[1] -> [1,2,3]. This approximates the
derivative at endpoints; it does not claim the exact derivative of an unknown
underlying continuous function. For samples=[-MAX,0,MAX], step=[MAX], all outputs
are 1 despite overflowing naive intermediate differences. Verify with independent
exact rational arithmetic. Test negative step, N=2, step subnormals, NaN priority,
interior center NaN outside the requested stencil, infinities, signed zeros,
disjoint source-read witnesses, dirty inverse stencils, budgets and cancellation.

Deliver actual public WorkflowDocument execution and owner-lifetime checks when
implemented. These versioned keys are not registered; no runtime tests are claimed.
