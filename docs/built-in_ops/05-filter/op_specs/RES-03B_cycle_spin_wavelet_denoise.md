---
spec_schema_version: 1
id: RES-03B
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-03
function: cycle_spin_wavelet_denoise
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
---

# RES-03B: cycle_spin_wavelet_denoise

Wavelet denoising with a fixed shift set.

Inherits the [RES-03 family contract](RES-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numeric/accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; RN, underflow, and accelerated error baselines cannot be overridden here.

## Ports, Shape, and Semantics

`input,thresholds` → `output` with the original shape. Threshold schema is the same as member A. At levels=0, explicitly bypass input and do not read thresholds.

Except at a semantic entry point explicitly defined by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color descriptions do not broaden numeric-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation/reconstruction of applicable metadata follow the common contract. Components and multiple outputs declare dependencies per request; associated collections follow the FilterBands/FrequencyGrid drafts.

## Static Parameters and Valid Domains

`profile=haar_mean_lifting_v1|cdf53_float_lifting_v1`; specify levels and a nonempty list of unique integer (y,x) shifts. Reject duplicates after reduction modulo H/W. The complete shift list is explicit and nonrandom. Boundary analysis follows the profile; do not silently switch to periodic wavelets.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operations. Validate any finite-parameter, finite-control-model, or narrower mathematical domain explicitly required by this member. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

For each shift s, first apply the wrap permutation S_s to the input; then run FRQ10 analysis → RES-03A thresholding → FRQ10 inverse → S_-s. Compute the exact mean of all rounded reconstructions, then RN_t once. Average only over the specified shift set; a finite set does not guarantee full translation invariance.

If a formula calls another member, preserve any RN stage at the call site as specified here or by the family contract. Internal temporary operations must not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Whole-plane and collection demand. Shifts and thresholds are Control. The workflow must retain ownership of any bands still referenced downstream.

Boundary coordinates are defined against the complete logical image; ROI/tile edges do not become new boundaries. An empty request reads no payload. Selection conditions retain Control dirty witnesses. Exact support must not be replaced by a convenient rectangular or Whole read; any expansion requires an explicit Conservative plan. Output descriptors derive from static parameters and input descriptors, not input sample values.

## Algorithm, Resources, and Execution

O(shifts*PC) for decimated transforms. Branches may run serially according to budget, but the final exact accumulator must be retained.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. Failure publishes no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

A single shift (0,0) is equivalent to the chain without cycle spinning. T=0 does not guarantee bit identity for arbitrary floating-point analysis/reconstruction. Full-period shift coverage can test translation equivariance, subject to the boundary and algorithm; define the comparison by complete enumeration.

The function below covers subformulas/stages of this composition. Named workflow checks in tests.py cover only their recorded configurations; they are not a Photospider DAG builder and do not claim complete metadata, Region, or resource integration. Stage RN boundaries cannot disappear by merging nodes.

`multiscale.cycle_spin(image, thresholds, *, levels=1, profile='haar_mean_lifting_v1', shifts=((0, 0),), mode='soft', dtype='float64')` → [Source file](../../../../oracle/ops/filter/oracles/multiscale.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, ROI/dirty behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

This file proposes no native arithmetic key ready for registration. An authoring helper must explicitly connect finalized members. An external engine must first pass resource, licensing, real-golden, and numeric-configuration gates.

Float64 accelerated profiles retain Float64 inputs, outputs, and exponent range; they do not first convert to Float32. Values outside the Float32 scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is Whole or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
