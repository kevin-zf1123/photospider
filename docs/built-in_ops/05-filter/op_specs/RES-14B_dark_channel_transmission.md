---
spec_schema_version: 1
id: RES-14B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: RES-14
function: dark_channel_transmission
proposed_operation_keys:
- restoration.dark_channel_transmission_strict
- restoration.dark_channel_transmission_accelerated_apple_silicon
- restoration.dark_channel_transmission_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
---

# RES-14B: dark_channel_transmission

Dark-channel transmission estimation.

Inherits the [RES-14 family contract](RES-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The family and member formulas together form the complete draft; this member cannot override RN, underflow, or accelerated error baselines.

## Ports, shapes, and semantics

The explicitly selected, linear opaque RGB `input` and Float64 `[3]` `airlight` → single-plane `transmission`. The selected color group has no associated alpha plane; reject alpha-associated structure at preflight without scanning alpha samples. Input RGB must be finite and nonnegative.

Except for semantic entry points explicitly specified by this member, inputs are raw numerical fields: channel count does not imply RGB or alpha, and attached color metadata does not expand numerical-domain validation. Axes, positive extents, explicit broadcasting, canonical planar publication, and preservation or reconstruction of applicable metadata follow the common contract. Dependencies for components and multi-output requests are declared per request; associated collections follow the proposed FilterBands/FrequencyGrid schemas.

## Static parameters and legal domains

radius ≥ 0; omega ∈ [0,1] (0.95 is illustrative, not a default); boundary=truncate; axes/component_axis; metadata_mode=respect|override as in RES-14A. For omega>0, each consumed airlight component must be finite and >0. At omega=0, the exact constant-one branch consumes no input or airlight samples.

Inherit NaN, Inf, overflow, signed-zero, payload, rounding, and precision semantics from the corresponding NUM operation. A member must still validate finite parameters, finite control models, or narrower mathematical domains when specified. Copy/bypass and unconsumed data follow their dependency contracts. Every parameter must be supplied explicitly; constructors provide no defaults, except constants fixed by a named mathematical profile. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real.

## Mathematical reference and rounding boundaries

Numeric classification: **E**. See [FILTER_numeric_reference](FILTER_numeric_reference.md) for the meanings of E/B/S.

`t(q)=RN_t(clamp(1-omega*min_(p∈Bq,c)(I(p,c)/A_c),0,1))`. Compare quotients exactly without prior rounding; the clamp is an explicit part of the estimation model. Refinement uses FIL-07A followed by an explicit clamp; it is not hidden in this node.

When a formula calls another member, preserve the RN stages produced by that call as specified here or by the family contract. Internal temporary operations must not add undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the common contract and NUM.

## Data / Control / Validation / Descriptor

The radius-r halo of input and all three airlight components are Control. When omega=0, the result is the explicit constant 1 and input/airlight sample values are not read; descriptors and metadata are still validated.

Boundary coordinates are defined against the full logical image; ROI/tile edges do not become new boundaries. An empty demand reads no payload. Selection conditions retain their Control dirty witnesses. Exact support must not be replaced by a convenient bounding rectangle or Whole read; any expansion requires an explicit Conservative plan. Output descriptors are derived from static parameters and input descriptors, not sample values.

## Algorithm, resources, and execution

O(PA*3), with exact ratio ordering.

Here P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. Shared contracts govern work/memory/stage admission, cancellation, failure status, and owner lifetime; failures publish no partial Value or CompleteBundle. Do not spill automatically, silently reduce precision, or fall back to retired implementations.

## Oracle, fixtures, and acceptance

If I=A at every neighbor, t=1-omega. omega=0 yields 1. For HDR input, a minimum ratio above 1 may clamp to 0. A≤0 fails, except for the omega=0 bypass.

The reference function evaluates the numerical formula on small inputs; it is neither a production kernel nor a complete port-schema/preflight simulator. ExactRational rounds rational expressions directly to IEEE format. For transcendental functions, only a DirectedMPFR result whose interval endpoints round to the same value is a strict golden. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`restoration.transmission(image, airlight, *, radius=1, omega=0.95, dtype='float64')` → [source](../../../../oracle/ops/filter/oracles/restoration.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence classes. The listed self-checks validate only the reference program. Parameter boundaries, full rank/batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and ownership still require the [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing an identity or constant fixture does not accept the whole algorithm or every configuration.

## Backend and registration gates

The keys above are naming proposals only; neither strict nor either CPU accelerated profile is registered or implemented. Accelerated profiles must satisfy NUM final-output bounds of four ULP at FP32 scale and provide strict fallback. The error allowance does not accumulate per tap, axis, stage, or iteration. Thresholds, ordering, boundaries, copies, and output support requiring exact selection cannot change through approximation.

Float64 acceleration retains Float64 inputs, outputs, and exponent range; it does not first convert to Float32. Values outside the Float32-scale range use strict NUM fallback. No kernel, CPU/ISA performance, or differential acceptance runs for this package have been performed.

## Conceptual DAG (not an existing API)

`static + descriptors → preflight / demand → Data and Control declared by this member → exact expression / declared RN stages → requested owned output / complete result`

This sketch introduces no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-filling of missing data. Whole behavior and multi-stage buffers are governed by this member and its family contract.

## References and conformance notes

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)

External sources motivate the algorithm or definitions. The finite window, rounding, tie-breaking, units, and execution profile are explicit contract choices; bitwise identity with any library or commercial product is not claimed.
