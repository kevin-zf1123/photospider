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
implementation_status: implemented
repository_branch: ops-specs
repository_commit: 30478d33
implementation_branch: numeric-optimize
implementation_base_commit: eb0e90c8
implementation_updated: 2026-09-21
---

# NUM-15A: derivative_1d

Numeric profile: strict retains the exact reference defined below. Floating
arithmetic in accelerated profiles follows the shared
[final FP32 four-ULP contract](NUM_accelerated_contract.md), including its
range/fallback rules. Discrete results, copies, selected endpoints and special
values remain exact.

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
whole rational quotient exactly and correctly round once to output dtype; accelerated uses the shared FP32-scaled bound. Neither an overflowing naive
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
implemented. Implementation evidence below records the checks actually run.


## Implementation and executable acceptance

Six suffixed calculus keys are registered in `numeric_calculus.cpp`; public
helpers are `derivative_1d_node` and `integrate_1d_node` in
`photospider/numeric/calculus.hpp`. Regional execution first requests step as
Control/Validation for dependent observations. Integration always requests
initial as Data/Validation and does not request step or samples for index zero.
Positive outputs retain their actual sample support even when initial is NaN.

Derivative uses exact integer differences and a positive step magnitude,
including the exact interior factor two; the sign includes step's sign. The
4352-bit ratio scratch covers all required alignment/refinement. Integration
retains an exact unweighted sum, source classification, and first/last samples.
A separate conversion workspace forms `2*sum-first-last`, multiplies by the
full exact step coefficient, adds aligned initial, then rounds with scale
-2149. Its numerator needs at most 4237 bits and rounding scratch at most 4238.
Initial-only output is a raw copy, while positive exact-zero outputs are +0.

Integration transports at most 64 samples per window and scans to the largest
requested index once per regional invocation. Every window retains complete
per-observation associations; dense boundary requests can incur quadratic
metadata work. State, output plan and metadata are accounted; work/capacity/stage
limits reject excessive requests. There are no persistent checkpoints, and
separate executions or execute_atoms observations can repeat numeric work.

The manual `photospider_numeric_calculus` target, `calculus.cpp`, and independent
`calculus_oracle.py` are documented in
[the workflow example](../../../../examples/numeric_workflow/README.md).
Local Clang 21 strict/Apple and Ubuntu WSL Clang 18 strict/AVX2 passed 1,810
independent Fraction/raw-bit cases per profile on 2026-09-19 and the manual
workflow checks: sparse stencil/center-NaN exclusion, raw initial-only output,
invalid-step Atom isolation and producer order, required upstream failure after
initial NaN, all-port strides/fenv, Empty/schema, WorkLimit/cancellation and
release. A 4096-input integral with four sparse outputs uses 64 windows and
exact sample/step/initial dirty support. Installed strict/Apple consumers,
focused compiler unit, formatting/lint and scoped math/entry reviews passed.
The manual target has no CTest/integration registration; specification status
remains Proposed independently of implementation evidence.
