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

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

Require an explicit `mode=hard|soft`; every threshold≥0. Under the positive-extent rule, this member does not accept an empty threshold table when levels=0; use the identity-bypass helper without creating this node.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **E+copy**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Hard threshold: abs(d)<T yields +0; otherwise copy d, so equality retains the coefficient. Soft threshold: abs(d)≤T yields +0; otherwise `RN_t(sign(d)*(abs(d)-T))`. At T=0 all detail coefficients are bit-copied, with no extra finite validation. For nonzero T, NaN coefficients propagate under NUM and infinite coefficients follow the stated arithmetic; they are not rejected by a finite-only rule. LL adds no arithmetic validation.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

Compilation checks the complete static band directory and selected output schemas. Lowering includes only each selected band, its threshold control and required upstream tasks. Runtime associations are caller premises, and unrelated sibling payloads are not numerical-validation inputs. An unchanged LL output preserves its bits using explicit COPY when materialized; an authoring workflow may explicitly reuse its existing Result. Whole-band mathematical dependencies remain unless locality is separately specified.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(total coefficients), with exact subtraction/comparison. Do not treat sigma as per-band standard deviation without a declared basis norm.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

At d=T, hard mode retains the coefficient and soft mode clears it to zero. Preserve the sign of negative details. At T=0, preserve NaN and -0 bits. The inverse may consume the edited bands.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only a DirectedMPFR result whose interval is closed to a unique rounded value is a strict golden. If the interval cannot determine rounding, report Inconclusive; do not substitute an approximation.

`frq10.multiscale.threshold_bands(bands, thresholds, *, mode='soft', dtype='float64')` → [Source file](../../../../oracle/ops/filter/frq10/multiscale.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
