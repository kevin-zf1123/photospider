---
spec_schema_version: 1
id: RES-11B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-11
function: pairwise_block_boundary_smoothing
proposed_operation_keys:
- restoration.pairwise_block_boundary_smoothing_strict
- restoration.pairwise_block_boundary_smoothing_accelerated_apple_silicon
- restoration.pairwise_block_boundary_smoothing_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
---

# RES-11B: pairwise_block_boundary_smoothing

Pairwise smoothing across known block boundaries.

Inherits the [RES-11 family contract](RES-11_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; this member cannot override RN, underflow, or accelerated error baselines.

## Ports, shapes, and semantics

`input, application_mask` → same-shape `output`; the mask is finite and in [0,1], with any component broadcasting declared explicitly.

Except for semantic entry points explicitly specified by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color metadata does not expand numerical-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Dependencies for components and multi-output requests are declared per request; associated collections follow the proposed FilterBands/FrequencyGrid schemas.

## Static parameters and legal domains

block_width and block_height ≥ 2; integer origin_x and origin_y; threshold τ > 0; strength ∈ [0, 1/2] (0.25 is illustrative, not a default); axes; fixed execution order: horizontal x pass, then vertical y pass.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. A member must still validate finite parameters, finite control models, or narrower mathematical domains when specified. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

For each adjacent pair (l,r) on a block boundary, provided both lie inside the image, if abs(r-l) ≤ τ then set delta = strength*min(Ml,Mr)*(r-l), evaluated exactly, and produce l+=RN_t(l+delta), r-=RN_t(r-delta); otherwise copy both values. Pairs within a pass do not overlap because each block dimension is at least 2. The second, y pass reads the rounded output of the x pass. All other samples are bitwise copies. If min(Ml,Mr)=0 or strength=0, copy both values without computing the difference. A zero mask at either endpoint suppresses the whole pair, preserving every unmasked sample bit-for-bit.

When a formula calls another member, preserve the RN stages produced by that call as specified here or by the family contract. Internal temporary operations must not add undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

The support is the union of the two fixed passes, with a maximum halo of one per axis (a corner may follow a two-pass dependency). The mask and pair difference are Control. This is not global deblocking or codec dequantization.

Boundary coordinates are defined against the full logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain their Control dirty witnesses. Exact support must not be replaced by a convenient bounding rectangle or Whole read; any expansion requires an explicit Conservative plan. Output descriptors are derived from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

O(PC), with two S stages and synchronized pair updates. No automatic texture-based block detection is introduced.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; failures publish no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For the two-pixel input [0,1], block_width=2, origin_x=1, strength=0.25, and τ=1, the output is [0.25,0.75]. Equality at τ activates correction; a zero mask preserves both samples; shifting the block origin changes which edge is selected.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational rounds rational expressions directly to IEEE format. For transcendental functions, only a DirectedMPFR result whose interval endpoints round to the same value is a strict golden. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`restoration.pairwise_block_boundary_smoothing(image, mask, *, block_height=8, block_width=8, origin_y=0, origin_x=0, threshold=1, strength=Fraction(1, 4), dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/restoration.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU accelerated profile is registered or implemented. Accelerated profiles must satisfy NUM final-output bounds of four ULP at FP32 scale and provide strict fallback. The error allowance does not accumulate per tap, axis, stage, or iteration. Thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot change through approximation.

Float64 acceleration retains Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU/ISA performance, or differential acceptance runs for this package have been performed.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S22 · scikit-image restoration](../research-sources.md#s22)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
