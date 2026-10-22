---
spec_schema_version: 1
id: RES-07F
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-07
function: generalized_anscombe_inverse_mean
proposed_operation_keys:
- restoration.generalized_anscombe_inverse_mean_strict
- restoration.generalized_anscombe_inverse_mean_accelerated_apple_silicon
- restoration.generalized_anscombe_inverse_mean_accelerated_x86_64
numeric_reference: E
oracle_scope: diagnostic_inverse_not_strict_golden
---

# RES-07F: generalized_anscombe_inverse_mean

Poisson–Gaussian mean-domain inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`denoised_stabilized_mean` → `noiseless_mean_observation`.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Use a, b, and s as defined for member D; require z≥0.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Solve for λ using the exact inverse of family mρ. Output RN_t(b+aλ); on the lower branch, b is converted from the parameter’s RN value to dtype t by bit-preserving conversion. The σ=0 case is consistent with the exact Poisson root definition, while gain/offset application remains one whole expression.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

Pointwise; budget and cancellation apply to the actual infinite-integral and summation work.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

Native registration requires certificates for Gaussian lower-tail truncation, the Poisson tail, and monotone root solving. This package provides only a small-input quadrature diagnostic and domain fixture.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

ρ=0 reduces to the Poisson case. The mρ(0) threshold at λ=0 differs from Anscombe. A finite ±3σ Gaussian window is not an exact tail treatment.

The current reference returns HighPrecisionDiagnostic: it numerically evaluates the mean function and solves for its root, but has no joint proof for the infinite Poisson tail, Gaussian integral tail, and final rounding interval. Its values cannot serve as strict goldens. Before strict/native registration, add a DirectedMPFR or equivalent certificate; algebraic-inverse fallback is forbidden. The current diagnostic coverage is limited to the s=0 degenerate case and the lower branch with positive read noise; it does not cover full root solving for positive read noise and positive λ.

`res07.restoration.generalized_mean_inverse(z, *, gain, offset=0, read_sigma=0, dps=40, max_lambda=12)` → [Source file](../../../../oracle/ops/filter/res07/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
