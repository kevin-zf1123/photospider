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
numeric_reference: B+E
oracle_scope: composed_math_references
---

# RES-12B: gaussian_lowpass

Explicit low-pass baseline for screen-pattern suppression.

Inherits the [RES-12 family contract](RES-12_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; this member cannot override RN, underflow, or accelerated error baselines.

## Ports, shapes, and semantics

`input` → `output`.

Except for semantic entry points explicitly specified by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color metadata does not expand numerical-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Dependencies for components and multi-output requests are declared per request; associated collections follow the proposed FilterBands/FrequencyGrid schemas.

## Static parameters and legal domains

All FIL-04B parameters are explicit. No default resizing or automatic frequency estimation is used; axes are explicit.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. A member must still validate finite parameters, finite control models, or narrower mathematical domains when specified. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **B+E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

Reuse the gaussian_baked64_v1 low-pass filter as a named authoring preset, not as a new mathematical primitive. For downsampling, connect an explicit sampling node from the geometry category; this filter alone does not recover detail hidden by the screen pattern.

When a formula calls another member, preserve the RN stages produced by that call as specified here or by the family contract. Internal temporary operations must not add undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

Local Gaussian actual support; no whole-image spectral read.

Boundary coordinates are defined against the full logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain their Control dirty witnesses. Exact support must not be replaced by a convenient bounding rectangle or Whole read; any expansion requires an explicit Conservative plan. Output descriptors are derived from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

As specified by FIL04B; select quality parameters to match the screen-pattern period to the sampling grid.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; failures publish no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

The periodic checker pattern is attenuated, while genuine high-frequency texture is also affected. The two comparison images support use-case quality scoring and are not mathematical oracles.

The functions below cover sub-formulas or stages of this composition; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim to model complete metadata, Region, or resource integration. RN stages must not disappear when nodes are fused.

`transcend.gaussian_kernel(sigma, radius)` → [source](../../../../oracle/ops/filter/oracles/transcend.py).
`spatial.separable(image, kx, ky, anchor=(0, 0), **kwargs)` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

This file does not propose a native arithmetic key ready for registration. The authoring helper must explicitly connect finalized members. Any external engine must first pass resource, licensing, actual-golden, and numerical-configuration gates.

Float64 acceleration retains Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU/ISA performance, or differential acceptance runs for this package have been performed.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
