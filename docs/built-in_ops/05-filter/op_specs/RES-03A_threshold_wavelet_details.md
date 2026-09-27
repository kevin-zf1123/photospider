---
spec_schema_version: 1
id: RES-03A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-03
function: threshold_wavelet_details
proposed_operation_keys:
- restoration.threshold_wavelet_details_strict
- restoration.threshold_wavelet_details_accelerated_apple_silicon
- restoration.threshold_wavelet_details_accelerated_x86_64
numeric_reference: E+copy
oracle_scope: mathematical_reference
---

# RES-03A: threshold_wavelet_details

Wavelet detail thresholding.

Inherits the [RES-03 family contract](RES-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`bands` FilterBands/v1 and `thresholds` Float64 `[levels,3]` → new `bands`; LL residual is bit-copied. Per level, order is LH_y, HL_x, HH.

Except at a semantic entry point explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color descriptions do not broaden numeric-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation/reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the FilterBands/FrequencyGrid drafts.

## Static Parameters and Valid Domains

Require an explicit `mode=hard|soft`; every threshold≥0. Under the positive-extent rule, this member does not accept an empty threshold table when levels=0; use the identity-bypass helper without creating this node.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **E+copy**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Hard threshold: abs(d)<T yields +0; otherwise copy d, so equality retains the coefficient. Soft threshold: abs(d)≤T yields +0; otherwise `RN_t(sign(d)*(abs(d)-T))`. At T=0 all detail coefficients are bit-copied, with no extra finite validation. For nonzero T, NaN coefficients propagate under NUM and infinite coefficients follow the stated arithmetic; they are not rejected by a finite-only rule. LL adds no arithmetic validation.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Validate the complete descriptor association without reading payloads. Materialize only the requested band, its threshold control and its required upstream band dependencies. Sibling-band payloads are not demanded. A requested unchanged LL band retains its immutable owner; thresholding another band does not read LL values. Band-internal Region execution is not additionally promised.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty request reads no payload. Selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any expansion requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, Resources, and Execution

O(total coefficients), with exact subtraction/comparison. Do not treat sigma as per-band standard deviation without a declared basis norm.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

At d=T, hard mode retains the coefficient and soft mode clears it to zero. Preserve the sign of negative details. At T=0, preserve NaN and -0 bits. The inverse may consume the edited bands.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only a DirectedMPFR result whose interval is closed to a unique rounded value is a strict golden. If the interval cannot determine rounding, report Inconclusive; do not substitute an approximation.

`multiscale.threshold_bands(bands, thresholds, *, mode='soft', dtype='float64')` → [Source file](../../../../oracle/ops/filter/oracles/multiscale.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is Whole or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
