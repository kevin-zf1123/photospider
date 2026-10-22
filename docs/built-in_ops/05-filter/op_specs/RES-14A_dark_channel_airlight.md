---
spec_schema_version: 1
id: RES-14A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: dark_channel_airlight
proposed_operation_keys:
- restoration.dark_channel_airlight_strict
numeric_reference: order/select exact
oracle_scope: mathematical_reference
---

# RES-14A: dark_channel_airlight

Dark-channel and airlight estimation.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; this member cannot override RN, underflow, or accelerated error baselines.

## Ports, shapes, and semantics

The explicitly selected, linear opaque RGB `input` with no associated alpha plane → single-plane `dark_channel` with the same H/W; Float64 `[3]` `airlight`; and Int64 `[2]` `selected_pixel`. Finite nonnegative RGB samples are caller premises; no numerical input scan is performed. Alpha-bearing groups fail structural preflight; do not scan alpha samples to infer opacity.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and legal domains

radius ≥ 0; top_fraction f ∈ (0,1] (0.001 is illustrative, not a default); axes/component_axis; boundary=truncate; `input_description` (canonical TensorDescription v4/v5 String) and an explicit RGB `group`.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **exact ordering/selection**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for E/B/S.

D(q)=min_(p∈Bq,c) I(p,c). For airlight, select the k=max(1,ceil(f*H*W)) pixels with largest D, breaking ties by earlier row-major index. Among them, select one pixel with the largest exact RGB sum, again breaking ties by row-major order. A is that pixel’s RGB converted exactly to Float64; do not independently maximize each channel across candidates. When airlight is selected, callers guarantee that each component of this selected A is strictly positive. Production execution does not test this numerical premise or add epsilon; the premise does not constrain a dark-channel-only request.

When a formula calls another member, preserve the RN stages produced by that call as specified here or by the family contract. Internal temporary operations must not add undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the common contract and NUM.

## Fixed reads, algorithmic control and schemas

A dark_channel-only request has local radius-r demand and no complete-input sort. Airlight or selected_pixel requires the complete-input input and dark map. A request for D alone must not force airlight estimation.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

D costs O(PA*3); top-k/sort costs O(P log P). k is computable from the descriptor. The selected-airlight positivity premise applies only when that output is selected; there is no numerical input or output validation scan.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; a failed call publishes no successful Result export, while previously retired call exports remain valid and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

For constant positive RGB, D equals the minimum channel, A equals that RGB, and the selected index is (0,0). An all-black input is valid for D alone; selecting A for that input violates the positive-airlight premise and has no production failure guarantee. Any diagnostic for that case belongs to the oracle input checker. Even if top_fraction<1/P, select one pixel.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational rounds rational expressions directly to IEEE format. For transcendental functions, only a DirectedMPFR result whose interval endpoints round to the same value is a strict golden. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`res14.restoration.airlight(image, *, radius=1, top_fraction=0.001, request_airlight=True)` → [source](../../../../oracle/ops/filter/res14/restoration.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU accelerated profile is registered or implemented. Accelerated profiles must satisfy NUM final-output bounds of four ULP at FP32 scale and provide strict fallback. The error allowance does not accumulate per tap, axis, stage, or iteration. Thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot change through approximation.

Float64 acceleration retains Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU/ISA performance, or differential acceptance runs for this package have been performed.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
