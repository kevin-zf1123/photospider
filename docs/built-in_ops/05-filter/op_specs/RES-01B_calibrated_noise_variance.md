---
spec_schema_version: 1
id: RES-01B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-01
function: calibrated_noise_variance
proposed_operation_keys:
- restoration.calibrated_noise_variance_strict
- restoration.calibrated_noise_variance_accelerated_apple_silicon
- restoration.calibrated_noise_variance_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-01B: calibrated_noise_variance

Calibrated Poisson–Gaussian variance field.

Inherits the [RES-01 family contract](RES-01_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`mean_signal` → `variance,sigma`, both with the same shape and Float32/Float64 dtype.

Unless the member defines an explicit color interpretation, ports contain raw numerical Tensor samples; channel count does not imply RGB or alpha. Color descriptions are explicit operation parameters, never attached Result metadata. Positive extents, axis validation and explicit broadcasting follow the common contract. Host-created backing defaults to tiled planar without requiring a conformance declaration. Associated FilterBands/FrequencyGrid structures remain Proposed static Tensor schemas; each requested output records its actual tile Needs.

## Static Parameters and Valid Domains

Require gain a>0, finite offset b, and read_sigma s≥0, with units defined by y=a*P+b+N(0,s²). Require an explicit `below_offset=clamp|error` policy.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Let λ=(mean_signal-b)/a; apply below_offset when it is below zero. Define variance=`RN_t(a²*max(λ,0)+s²)` and sigma=`RN_t(sqrt(a²*max(λ,0)+s²))`; do not take sqrt of the rounded variance. The input estimates expected signal; it does not claim to be the true mean of a single noisy observation.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Pointwise. A sigma-only request performs the same domain checks and does not publish variance.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Mathematical sample support is rounded to the current plan's member tiles for payload Needs, authorization and invalidation. Control and validation samples use ordinary payload Needs and participate in the same evidence. Empty payload demand reads no samples. Output structure derives only from static parameters and input schemas. The kernel has no support-role or Exact/Conservative/Unknown dependency mode.

## Algorithm, Resources, and Execution

O(PC), with exact variance and a certified square root.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Tensor or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

For mean_signal=b+4a and s=0, variance=4a² and sigma=2a. Under below-offset clamping, only the read-noise variance remains.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only a DirectedMPFR result whose interval is closed to a unique rounded value is a strict golden. If the interval cannot determine rounding, report Inconclusive; do not substitute an approximation.

`res01.restoration.calibrated_variance(image, *, gain, offset=0, read_sigma=0, below_offset='clamp', dtype='float64')` → [Source file](../../../../oracle/ops/filter/res01/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
