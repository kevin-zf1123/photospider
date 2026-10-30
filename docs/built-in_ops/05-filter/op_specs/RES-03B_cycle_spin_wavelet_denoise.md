---
spec_schema_version: 1
id: RES-03B
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: implemented
parent_id: RES-03
function: cycle_spin_wavelet_denoise
proposed_operation_keys: []
numeric_reference: F
oracle_scope: composed_math_references
---

# RES-03B: cycle_spin_wavelet_denoise

Wavelet denoising with a fixed shift set.

Inherits the [RES-03 family contract](RES-03_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family formula and this member together define the complete draft; native fp32/fp64 arithmetic follows RES_native_contract; NUM strict bits and FP32-scaled four-ULP guarantees do not apply.

## Ports, Shape, and Semantics

`input,thresholds` → `output` with the original shape. Threshold schema is the same as member A. At levels=0, explicitly bypass input and do not read thresholds.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domains

`profile=haar_mean_lifting_v1|cdf53_float_lifting_v1`; specify levels and a nonempty list of unique integer (y,x) shifts. Callers guarantee uniqueness after reduction modulo H/W; no runtime numerical-domain check is required. The complete shift list is explicit and nonrandom. Boundary analysis follows the profile; do not silently switch to periodic wavelets.

Use native IEEE fp32/fp64 arithmetic under [RES native floating-point contract](RES_native_contract.md). No cross-backend NaN payload or exact-expression rounding is promised. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass behavior and unconsumed data follow the dependency contract. Supply every parameter explicitly; constructors provide no defaults, except for constants fixed by a mathematical profile. Interpret real parameters as their stored Float64 values, not as infinitely precise decimal text.

## Mathematical Reference and Rounding Boundaries

Numeric classification: **F**, native fp32/fp64. See [RES native floating-point contract](RES_native_contract.md).

For each shift s, first apply the wrap permutation S_s to the input; then run FRQ10 analysis → RES-03A thresholding → FRQ10 inverse → S_-s. Compute their mean with native input-dtype accumulation and division; stable or pairwise accumulation is permitted. Average only over the specified shift set; a finite set does not guarantee full translation invariance.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

complete-input-plane and collection demand. Shifts and thresholds are Control. The workflow must retain ownership of any bands still referenced downstream.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(shifts*PC) for decimated transforms. Branches may run serially according to budget, but the native accumulation state must be budgeted and retained.

In complexity expressions, P=HW, C is the number of independent planes, A is the number of taps, and T is the number of steps. The shared protocol defines work, memory, and stage admission; cancellation; failure status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or call a retired implementation as a fallback.

## Oracle, Fixtures, and Acceptance

A single shift (0,0) is equivalent to the chain without cycle spinning. T=0 does not guarantee bit identity for arbitrary floating-point analysis/reconstruction. Full-period shift coverage can test translation equivariance, subject to the boundary and algorithm; define the comparison by complete enumeration.

The function below covers subformulas/stages of this composition. Named workflow checks in tests.py cover only their recorded configurations; they are not a Photospider DAG builder and do not claim complete metadata, Region, or resource integration. Stage RN boundaries cannot disappear by merging nodes.

`frq10.multiscale.cycle_spin(image, thresholds, *, levels=1, profile='haar_mean_lifting_v1', shifts=((0, 0),), mode='soft', dtype='float64')` → [Source file](../../../../oracle/ops/filter/frq10/multiscale.py).


These are standalone oracle helper signatures, not operator constructors; their Python convenience defaults do not define operation parameter defaults.

The oracle README (../../../../oracle/ops/filter/README.md) documents reference scope. Runtime acceptance under the [runtime acceptance protocol](FILTER_oracle_protocol.md) must cover parameter boundaries, full rank/batch coverage, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership.

## Backend and Registration Gates

`ps::restoration::cycle_spin_wavelet_denoise` is available in `photospider/ops/restoration.hpp`. It connects the registered FRQ10 and RES-03A operations with explicit `restoration.wrap_shift` and `restoration.accumulate_native` stages; it does not register a monolithic cycle-spin key. Parameters and ownership follow [CPU implementation and public API](RES-03_contract.md#cpu-implementation-and-public-api).

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Execution Stages

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This diagram introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whether execution is complete-input or requires multistage buffers is specified by this member and its family contract.

## References and Conformance Notes

[S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17);[S22 · scikit-image restoration](../research-sources.md#s22)

External sources provide algorithmic or definitional background. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
