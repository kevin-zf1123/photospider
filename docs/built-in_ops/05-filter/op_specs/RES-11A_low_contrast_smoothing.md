---
spec_schema_version: 1
id: RES-11A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-11
function: low_contrast_smoothing
proposed_operation_keys:
- restoration.low_contrast_smoothing
numeric_reference: F
oracle_scope: mathematical_reference
---

# RES-11A: low_contrast_smoothing

Selective smoothing in low-contrast regions.

Inherits the [RES-11 family contract](RES-11_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Ports, shapes, and semantics

`input, application_mask` → `output`; the mask has the same shape or uses explicit component broadcasting, and each consumed mask value is finite and in [0,1].

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and legal domains

radius ≥ 1; range_threshold τ > 0; strength ∈ [0,1] (0.5 is an illustrative value, not a default); boundary=reflect_half; axes.

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

If M=0 or strength=0, bit-copy the center without reading the neighborhood. Otherwise, compute R=max-min in native precision; a NaN determining R propagates to the output rather than selecting the copy branch. When R≤τ, let B be a native window mean and produce RN_t(I+M*strength*(B-I)); when R>τ, bit-copy I. No random dither is applied; display quantization and dithering require explicit FMT nodes.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

The mask is Control. An activated window has a maximum halo of r. Range selection requires all window Data; reading only the samples that determine the extrema is insufficient.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PCA); order statistics may be optimized. A scalar field is not implicitly interpreted as luminance.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; a failed call publishes no successful Result export, while previously retired call exports remain valid and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

A flat quantization staircase is smoothed when its range is within threshold. A range exactly equal to τ activates smoothing; fine texture with range >τ is retained. M=0 must not read poisoned neighbors.

The reference evaluates the mathematical formula on small inputs. Existing ExactRational/DirectedMPFR fixtures remain ideal mathematical references, not bitwise goldens for native execution. Acceptance compares native results using dtype-aware absolute/relative error and algorithm invariants under [RES native floating-point contract](RES_native_contract.md); it separately verifies runtime schemas, support and ownership.

`res11.restoration.low_contrast_smoothing(image, mask, *, radius=1, range_threshold=1, strength=Fraction(1, 2), dtype='float64')` → [source](../../../../oracle/ops/filter/res11/restoration.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

The proposed native key is unregistered and unimplemented. Accuracy follows RES_native_contract; no NUM strict fallback is required.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S22 · scikit-image restoration](../research-sources.md#s22)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
