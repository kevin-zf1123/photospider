---
spec_schema_version: 1
id: RES-13A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-13
function: nonflat_ball_opening
proposed_operation_keys:
- restoration.nonflat_ball_opening_strict
- restoration.nonflat_ball_opening_accelerated_apple_silicon
- restoration.nonflat_ball_opening_accelerated_x86_64
numeric_reference: B+S
oracle_scope: mathematical_reference
---

# RES-13A: nonflat_ball_opening

Background estimation with a non-flat spherical structuring element.

Inherits the [RES-13 family contract](RES-13_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; this member cannot override RN, underflow, or accelerated error baselines.

## Ports, shapes, and semantics

Single-plane `input` → `background`.

Except for semantic entry points explicitly specified by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color metadata does not expand numerical-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Dependencies for components and multi-output requests are declared per request; associated collections follow the proposed FilterBands/FrequencyGrid schemas.

## Static parameters and legal domains

Integer radius r ≥ 1; height h > 0 in input-intensity units; axes; boundary=reflect_half; disk footprint satisfying dx²+dy² ≤ r².

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. A member must still validate finite parameters, finite control models, or narrower mathematical domains when specified. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **B+S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

Define b(d)=RN64(h*(sqrt(1-(dx²+dy²)/r²)-1)) ≤ 0, with b(0)=0 and b=-h at the edge. First compute erosion e(q)=RN_t(min_d(E(I,q+d)-b(d))), then dilation B(q)=RN_t(max_d(E(e,q-d)+b(d))). This profile is named nonflat_ball_opening_v1 with explicit min/max and boundary semantics; it does not claim bitwise equivalence to every ImageJ or scikit-image rolling-ball implementation.

When a formula calls another member, preserve the RN stages produced by that call as specified here or by the family contract. Internal temporary operations must not add undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

The two stages have a worst-case support radius of 2r. Extend the erosion over the full logical domain for the second stage; do not crop it to the requested ROI. b is static Control.

Boundary coordinates are defined against the full logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain their Control dirty witnesses. Exact support must not be replaced by a convenient bounding rectangle or Whole read; any expansion requires an explicit Conservative plan. Output descriptors are derived from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

O(PA) per stage. Computing b requires certified square roots; stored b values may be compared exactly. There are two S stages.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; failures publish no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

Constants are preserved; a small bright spot may be removed. Spatial radius r is distinct from intensity height h. Verify single-pixel boundary behavior. Test anti-extensivity at general boundaries under this definition rather than assuming the infinite-domain result applies.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational rounds rational expressions directly to IEEE format. For transcendental functions, only a DirectedMPFR result whose interval endpoints round to the same value is a strict golden. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`restoration.ball_background(image, *, radius=1, height=1, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/restoration.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU accelerated profile is registered or implemented. Accelerated profiles must satisfy NUM final-output bounds of four ULP at FP32 scale and provide strict fallback. The error allowance does not accumulate per tap, axis, stage, or iteration. Thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot change through approximation.

Float64 acceleration retains Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU/ISA performance, or differential acceptance runs for this package have been performed.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S24 · scikit-image rolling-ball example](../research-sources.md#s24)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
