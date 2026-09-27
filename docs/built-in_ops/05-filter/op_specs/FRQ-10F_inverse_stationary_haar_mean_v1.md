---
spec_schema_version: 1
id: FRQ-10F
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: not_implemented
parent_id: FRQ-10
function: inverse_stationary_haar_mean_v1
proposed_operation_keys:
- frequency.inverse_stationary_haar_mean_v1_strict
- frequency.inverse_stationary_haar_mean_v1_accelerated_apple_silicon
- frequency.inverse_stationary_haar_mean_v1_accelerated_x86_64
numeric_reference: S
oracle_scope: mathematical_reference
research_sources:
- S16
- S17
---

# FRQ-10F: inverse_stationary_haar_mean_v1

stationary_haar_mean_v1 reconstruction .Status: **Proposed**.

Inherits the [FRQ-10 family contract](FRQ-10_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`bands` FilterBands/v1 → `output` with the original shape.

Unless this member defines a semantic color entry point, ports are raw numerical fields: channel count does not imply RGB or alpha semantics, and attached color metadata does not widen numeric-domain validation. Axis, positive extent, explicit broadcasting, canonical planar publication, and applicable metadata preservation or reconstruction follow the shared contract. Each requested component/output declares its own demand. Associated collections use the FrequencyGrid/v1 or FilterBands/v1 logical schema defined by the shared contract.

## Static Parameters and Valid Domain

Obtain axes, levels, and profile from the bands descriptor. Reject conflicting parameters; do not infer the input shape.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **S**; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

Inverse uses the reverse-order synthesis and crop defined by **stationary_haar_mean_v1** in the family contract, including every RN_t stage in that profile. At levels=0, preserve the identity bit-copy and owner. Do not validate non-finite payloads in undemanded bands.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data, Control, Validation, and Descriptors

Descriptors for every band, including shape, role, level, orientation, phase, and reconstruction metadata, are derived from input descriptors and the static profile without computing coefficient payloads. A band request computes only that band and required predecessor bands; sibling bands are not materialized by default. Lazy execution is guaranteed at band granularity only; intra-band Region execution is unspecified unless explicitly stated. Validate only demanded band payloads, while validating the complete immutable collection structure and all band descriptors. Each required band payload is a full plane unless this member explicitly guarantees local computation. Coefficient edits produce a new immutable FilterBands/v1 collection and may be supplied by any producer when its descriptors and reconstruction metadata are valid; producer identity is not required.

Boundary coordinates are defined against the complete logical image; ROI or tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain a Control dirty witness. Exact support may not be replaced by a convenient rectangular or Whole read. Any expansion must be explicitly requested as a Conservative plan. Output descriptors are derived from static parameters and input descriptors, never from input sample values.

## Algorithm, Resources, and Execution

O(PC * levels). Account for intermediate planes and all published band owners in the budget.

In the complexity notation, P=HW, C=independent planes, A=taps, and T=steps. Arbitrary-precision limb bit complexity is accounted for separately and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure Status; and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, Fixtures, and Acceptance

Acceptance fixtures: constant input yields zero detail bands; analytical 2x2 basis; odd 3x5 shape; levels=0 bit-copy; analysis/reconstruction agrees with an independent lifting reference. Require bit identity for an unmodified round trip only on exactly representable fixtures.

The reference function evaluates the numerical formula on small inputs; it is not a production kernel or a complete Ports-schema/preflight simulator. ExactRational evaluates rational expressions and rounds directly to IEEE format. For transcendental functions, use a DirectedMPFR result as a strict golden only when the interval is closed. If the interval does not determine rounding, report Inconclusive rather than substituting an approximation.

`multiscale.wavelet(image, levels, profile, inverse, bands, dtype)` → [source](../../../../oracle/ops/filter/oracles/multiscale.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated implementations must satisfy the NUM final-value Float32-scaled four-ULP bound and fallback rules. Error budgets may not be accumulated per tap, axis, stage, or iteration. Approximation may not change exact decisions such as thresholds, ordering, boundaries, copies, or output support.

Float64 accelerated execution retains Float64 inputs, outputs, and exponent range; it does not convert to Float32 first. Values outside the Float32-scaled range use the strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance runs were performed for this package.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → member-declared Data and Control → exact expression / declared RN stages → requested owned output / complete result`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. Whole demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S16 · PyWavelets signal extension modes](../research-sources.md#s16) ; [S17 · PyWavelets 2D DWT/IDWT](../research-sources.md#s17)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

The three named profiles and six analysis/synthesis members are defined in this family contract. Band-level lazy evaluation follows the shared collection contract.
