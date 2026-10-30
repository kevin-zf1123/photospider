---
spec_schema_version: 1
id: RES-12A
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-12
function: paired_notch_denoise
proposed_operation_keys: []
numeric_reference: F
oracle_scope: composed_math_references
---

# RES-12A: paired_notch_denoise

Explicit paired-notch suppression of periodic interference.

Inherits the [RES-12 family contract](RES-12_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Ports, shapes, and semantics

`input` → `output` with the same shape.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and legal domains

All grid and notch parameters from FRQ05C; FRQ01 norm=backward; input is treated as periodic. No window is applied implicitly (the caller may add one explicitly; a window with zeros cannot be silently inverted).

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

FRQ02A → explicitly generate the full FRQ05C response and select the corresponding half → FRQ06A → FRQ02B. Each node publishes its declared output stage; this helper is class F and preserves the called node interfaces. The response must preserve boundary conjugacy during construction; do not repair violations by discarding the imaginary component.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

complete-input-plane demand; centers are static Control. Reflection padding/cropping requires a separate helper and must not silently change the boundary.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PC log P) candidate work with native transform execution. Long-range spatial effects and ringing from narrow notches are algorithm behavior.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; a failed call publishes no successful Result export, while previously retired call exports remain valid and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

A depth-zero chain does not guarantee a general bitwise round trip. A sinusoid over an integer number of periods demonstrates notch attenuation; loss of real texture at the same frequency is a quality-negative case.

The functions below cover sub-formulas or stages of this composition; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim to model complete metadata, Region, or resource integration. RN stages must not disappear when nodes are fused.

`frq05.spectral.notch_response(height, width, *, centers, sigma_y, sigma_x, depth=1, dy=1, dx=1)` → [source](../../../../oracle/ops/filter/frq05/spectral.py).
`frq02.spectral.rfft2(image, *, norm='backward', dtype='float64')` → [source](../../../../oracle/ops/filter/frq02/spectral.py).
`frq02.spectral.irfft2(real, imag, original_width, *, norm='backward', dtype='float64')` → [source](../../../../oracle/ops/filter/frq02/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

This file does not propose a native arithmetic key ready for registration. The authoring helper must explicitly connect finalized members. Any external engine must first pass resource, licensing, actual-golden, and numerical-configuration gates.

Float64 paths retain native Float64 inputs, arithmetic, outputs and exponent range; no Float32 conversion or NUM strict fallback is introduced.

## Conceptual DAG (not an existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → native arithmetic with declared algorithmic stages → complete tiles → ordered call retirement`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. complete-input behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
