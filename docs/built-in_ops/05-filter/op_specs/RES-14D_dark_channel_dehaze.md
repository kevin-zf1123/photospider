---
spec_schema_version: 1
id: RES-14D
kind: authoring_helper
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: dark_channel_dehaze
proposed_operation_keys: []
numeric_reference: S
oracle_scope: composed_math_references
---

# RES-14D: dark_channel_dehaze

Complete dark-channel dehazing workflow.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; this member cannot override RN, underflow, or accelerated error baselines.

## Ports, shapes, and semantics

The explicitly selected, linear, opaque RGB `input` with no associated alpha plane → `output, transmission, airlight`, independently observable according to requested outputs. Reject alpha-associated structure at preflight; do not drop alpha or inspect its samples to infer opacity. RGB samples must be finite and nonnegative.

Except for semantic entry points explicitly specified by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color metadata does not expand numerical-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Dependencies for components and multi-output requests are declared per request; associated collections follow the proposed FilterBands/FrequencyGrid schemas.

## Static parameters and legal domains

All parameters of stages A/B/C; refine=none|guided_scalar. For refinement, construct the guide explicitly from RGB; this profile defines guide=RN_t((R+G+B)/3) per pixel, where t is the main RGB/transmission dtype; this makes the guide and transmission valid same-dtype FIL-07A inputs. FIL-07A radius and epsilon are explicit. Explicitly clamp the guided result to [0,1] with RN_t.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. A member must still validate finite parameters, finite control models, or narrower mathematical domains when specified. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **S**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

A → B → optional guided filter plus clamp → C. Rounding is explicit at every stage. An airlight-only request does not run apply. Requesting output requires Whole A, so the complete restoration is Whole. The guide gray is the arithmetic mean defined by this profile; it is not claimed to be photometric luminance.

When a formula calls another member, preserve the RN stages produced by that call as specified here or by the family contract. Internal temporary operations must not add undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

For airlight, demand Whole A. Raw transmission combines A’s Whole demand with its local demand. B can be local when supplied an existing A, but the complete D workflow is not a local filter.

Boundary coordinates are defined against the full logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain their Control dirty witnesses. Exact support must not be replaced by a convenient bounding rectangle or Whole read; any expansion requires an explicit Conservative plan. Output descriptors are derived from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

Complexity and resource costs add across composed stages. Do not automatically increase omega to avoid failures in sky regions.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; failures publish no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

A constant nonblack scene may be amplified or distorted and serves as a prior-model negative case. Quality scoring covers skies and bright walls. Validate A/B/C against independent oracles and as a composition. Reject alpha-associated input during preflight; do not silently discard alpha.

The functions below cover sub-formulas or stages of this composition; named workflow tests in tests.py cover only their recorded configurations. They are not Photospider DAG builders and do not claim to model complete metadata, Region, or resource integration. RN stages must not disappear when nodes are fused.

`restoration.airlight(image, *, radius=1, top_fraction=0.001, request_airlight=True)` → [source](../../../../oracle/ops/filter/oracles/restoration.py).
`restoration.transmission(image, airlight, *, radius=1, omega=0.95, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/restoration.py).
`restoration.apply_dehaze(image, airlight, transmission, *, t_floor=0.1, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/restoration.py).
`spatial.guided(image, guide, radius_y=1, radius_x=1, *, epsilon=0.01, metric_scale=None, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

This file does not propose a native arithmetic key ready for registration. The authoring helper must explicitly connect finalized members. Any external engine must first pass resource, licensing, actual-golden, and numerical-configuration gates.

Float64 acceleration retains Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU/ISA performance, or differential acceptance runs for this package have been performed.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
