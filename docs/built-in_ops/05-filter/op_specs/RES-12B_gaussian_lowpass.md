---
spec_schema_version: 1
id: RES-12B
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-12
function: gaussian_lowpass
proposed_operation_keys: []
numeric_reference: F
oracle_scope: composed_math_references
---

# RES-12B: gaussian_lowpass

Explicit low-pass baseline for screen-pattern suppression.

Inherits the [RES-12 family contract](RES-12_contract.md), [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; native arithmetic follows RES_native_contract without NUM exact-expression or four-ULP requirements.

## Ports, shapes, and semantics

`input` → `output`.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static parameters and legal domains

All FIL-04B parameters are explicit. No default resizing or automatic frequency estimation is used; axes are explicit.

Use native IEEE arithmetic under RES_native_contract; no NUM whole-expression rounding or NaN payload priority is required. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **F**, native fp32/fp64 under [RES_native_contract](RES_native_contract.md).

Reuse the gaussian_baked64_v1 low-pass filter as a named authoring preset, not as a new mathematical primitive. This delegation preserves the existing FIL coefficient profile and interface accuracy; it does not impose NUM strict-expression or four-ULP requirements on new RES-native kernels, and does not silently redefine FIL arithmetic. For downsampling, connect an explicit sampling node from the geometry category; this filter alone does not recover detail hidden by the screen pattern.

Preserve the declared algorithmic stages, masks, support, copy branches and iteration order. Within a stage, native arithmetic, FMA, vectorization and reduction regrouping are permitted under [RES native floating-point contract](RES_native_contract.md). An explicitly called member keeps its own interface contract.

## Fixed reads, algorithmic control and schemas

Local Gaussian actual support; no whole-image spectral read.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

As specified by FIL04B; select quality parameters to match the screen-pattern period to the sampling grid.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; a failed call publishes no successful Result export, while previously retired call exports remain valid and final RET results require successful Run completion. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

The periodic checker pattern is attenuated, while genuine high-frequency texture is also affected. The two comparison images support use-case quality scoring and are not mathematical oracles.

The functions below cover sub-formulas or stages of this composition; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim to model complete metadata, Region, or resource integration. RN stages must not disappear when nodes are fused.

`fil04.transcend.gaussian_kernel(sigma, radius)` → [source](../../../../oracle/ops/filter/fil04/transcend.py).
`fil02.spatial.separable(image, kx, ky, anchor=(0, 0), **kwargs)` → [source](../../../../oracle/ops/filter/fil02/spatial.py).

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
