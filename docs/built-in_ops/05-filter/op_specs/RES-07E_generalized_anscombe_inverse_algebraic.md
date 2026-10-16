---
spec_schema_version: 1
id: RES-07E
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: RES-07
function: generalized_anscombe_inverse_algebraic
proposed_operation_keys:
- restoration.generalized_anscombe_inverse_algebraic_strict
- restoration.generalized_anscombe_inverse_algebraic_accelerated_apple_silicon
- restoration.generalized_anscombe_inverse_algebraic_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-07E: generalized_anscombe_inverse_algebraic

Generalized algebraic inverse.

Inherits the [RES-07 family contract](RES-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`stabilized` → `noiseless_mean_observation`.

Unless the member defines an explicit color interpretation, ports contain raw numerical Tensor samples; channel count does not imply RGB or alpha. Color descriptions are explicit operation parameters, never attached Result metadata. Positive extents, axis validation and explicit broadcasting follow the common contract. Host-created backing defaults to tiled planar without requiring a conformance declaration. Associated FilterBands/FrequencyGrid structures remain Proposed static Tensor schemas; each requested output records its actual tile Needs.

## Static Parameters and Valid Domains

Use a, b, and s as defined for member D; require z≥0 and an explicit `negative_estimate=clamp|error` policy.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Compute λ=z²/4-3/8-(s/a)² and apply the policy when λ<0. Output RN_t(b+a*λ), using λ=0 when clamped; thus zero latent signal returns offset b, not zero. Subtract the offset externally to obtain signal.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Pointwise.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Mathematical sample support is rounded to the current plan's member tiles for payload Needs, authorization and invalidation. Control and validation samples use ordinary payload Needs and participate in the same evidence. Empty payload demand reads no samples. Output structure derives only from static parameters and input schemas. The kernel has no support-role or Exact/Conservative/Unknown dependency mode.

## Algorithm, Resources, and Execution

O(PC), using exact rational arithmetic.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Tensor or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

With s=0, a=1, and b=0 this reduces to member B. At λ=0 return b. Do not round λ before multiplying by gain.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only a DirectedMPFR result whose interval is closed to a unique rounded value is a strict golden. If the interval cannot determine rounding, report Inconclusive; do not substitute an approximation.

`res07.restoration.generalized_anscombe_algebraic(image, *, gain, offset=0, read_sigma=0, negative_estimate='clamp', dtype='float64')` → [Source file](../../../../oracle/ops/filter/res07/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S21 · Mäkitalo, Foi: inverse Anscombe research](../research-sources.md#s21)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
