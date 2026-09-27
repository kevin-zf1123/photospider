---
spec_schema_version: 1
id: RES-08B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-08
function: tikhonov_periodic
proposed_operation_keys:
- restoration.tikhonov_periodic_strict
- restoration.tikhonov_periodic_accelerated_apple_silicon
- restoration.tikhonov_periodic_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-08B: tikhonov_periodic

Periodic Tikhonov restoration.

Inherits the [RES-08 family contract](RES-08_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`observation,psf` → `estimate`.

Except at a semantic entry point explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color descriptions do not broaden numeric-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation/reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the FilterBands/FrequencyGrid drafts.

## Static Parameters and Valid Domains

Explicitly supply λ≥0, `regularizer=identity|gradient|laplacian`, dx/dy>0, `singular=error|zero`, axis, and anchor.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Minimize `||Ax-y||²+λ||D x||²`. Let R=|D|²: identity gives 1; gradient gives `4sin²(πkx/W)/dx²+4sin²(πky/H)/dy²`; Laplacian is the square of that gradient expression. `x=IFFT(conj(H)*FFT(y)/(|H|²+λR))` is one exact expression. Gradient and Laplacian do not penalize DC; R itself is not the signed operator D.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Whole demand. The regularizer derives from static parameters and the grid, not image samples.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty request reads no payload. Selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any expansion requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, Resources, and Execution

Same level as member A. For a small rational image, directly solve the exact system (AᵀA+λDᵀD)x=Aᵀy as an independent oracle.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

Delta PSF with λ=0 yields y. A gradient regularizer preserves a constant image (the PSF has unit DC). Identity regularization with λ=1 and delta PSF yields y/2. Apply the singular policy when H and D are both zero.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational uses rational expressions with direct IEEE rounding. For transcendental functions, only a DirectedMPFR result whose interval is closed to a unique rounded value is a strict golden. If the interval cannot determine rounding, report Inconclusive; do not substitute an approximation.

`restoration.tikhonov(observation, psf, *, lam, regularizer='gradient', dx=1, dy=1, anchor=(0, 0), singular='error', dtype='float64')` → [Source file](../../../../oracle/ops/filter/oracles/restoration.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

The keys above are naming proposals only; strict and both CPU accelerated profiles are unregistered and unimplemented. Accelerated profiles must meet the NUM final-output FP32-scaled four-ULP bound and pass fallback checks; the error budget cannot accumulate per tap, axis, stage, or iteration. Approximation must not change thresholds, ordering, boundaries, copies, or output support where exact selection is required.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is Whole or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S09 · FFTW: DFT definition](../research-sources.md#s09)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
