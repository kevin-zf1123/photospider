---
spec_schema_version: 1
id: FRQ-05C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: not_implemented
parent_id: FRQ-05
function: paired_notch_response
proposed_operation_keys:
- frequency.paired_notch_response_strict
- frequency.paired_notch_response_accelerated_apple_silicon
- frequency.paired_notch_response_accelerated_x86_64
numeric_reference: B
oracle_scope: mathematical_reference
research_sources:
- S09
- S26
---

# FRQ-05C: paired_notch_response

Paired periodic-frequency notch response.Status: **Proposed**.

Inherits [FRQ-05 family contract](FRQ-05_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). This member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

No dynamic input → real/imag response.

Unless the member defines an explicit color interpretation, ports contain raw numerical Tensor samples; channel count does not imply RGB or alpha. Color descriptions are explicit operation parameters, never attached Result metadata. Positive extents, axis validation and explicit broadcasting follow the common contract. Host-created backing defaults to tiled planar without requiring a conformance declaration. Associated FilterBands/FrequencyGrid structures remain Proposed static Tensor schemas; each requested output records its actual tile Needs.

## Static Parameters and Valid Domain

Require a nonempty static center list `(fy,fx)` in cycles per unit within the Nyquist fundamental domain; `sigma_y/x > 0`; and `depth ∈ [0,1]`. Reject duplicate centers and duplicate conjugacy orbits, including periodic equivalents and explicitly supplied opposite centers. Canonicalize the caller-supplied order for the descriptor only; the exact product is order-independent.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All applicable constructor parameters must be supplied explicitly. Parameters that do not apply to the selected profile must be omitted. Constructor defaults are not provided; supplied arguments are stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **B** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

For each center c, include both +c and −c, counting a self-conjugate center once. Define torus difference δf as the nearest representative modulo frequency period 1/d; choose the positive representative on a half-period tie. Compute `N_c=1−depth*exp(−.5*((δfy/sy)²+(δfx/sx)²))`; H is the exact product of all Nc, with the complete expression rounded once to RN64. Periodic distance makes the response continuous around Nyquist; do not truncate Euclidean distance to the rectangular array.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data, Control, Validation, and Descriptors

No image-data input. The complete center list is Control input. Conjugate frequency pairs share identical response bits.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Mathematical sample support is rounded to the current plan's member tiles for payload Needs, authorization and invalidation. Control and validation samples use ordinary payload Needs and participate in the same evidence. Empty payload demand reads no samples. Output structure derives only from static parameters and input schemas. The kernel has no support-role or Exact/Conservative/Unknown dependency mode.

## Algorithm, Resources, and Execution

O(P × number_of_notches); exponential and product refinement are subject to the execution budget.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. The shared contract defines work, memory, and stage admission, cancellation, failure status, and owner lifetime. Failure publishes no partial Tensor or CompleteBundle. Execution does not spill automatically, lower precision silently, or invoke a retired implementation as fallback.

## Oracle, Fixtures, and Acceptance

With depth=0, the response is all 1. A center at zero contributes one notch rather than a squared notch. The responses for paired ±c centers match. Frequencies on opposite sides of the Nyquist edge are equivalent.

The reference function evaluates the mathematical formula on small inputs. It is neither a production kernel nor a complete port-schema or preflight simulator. ExactRational rounds exact rational expressions directly to IEEE format. For transcendental functions, only results from a DirectedMPFR interval that determines the final rounding are strict goldens. Return Inconclusive when the interval cannot determine rounding; do not substitute an approximation.

`frq05.spectral.notch_response(height, width, *, centers, sigma_y, sigma_x, depth, spacing_y, spacing_x)` → [source file](../../../../oracle/ops/filter/frq05/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md). These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM’s final FP32-scaled four-ULP bound and fallback rules. Do not accumulate the error budget across taps, axes, stages, or iterations. Approximation may not change exact decisions, thresholds, ordering, boundaries, copy behavior, or output support.

Float64 accelerated variants retain Float64 inputs, outputs, and exponent range; they do not convert through Float32. Float32 inputs outside the supported scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → this member’s declared Data and Control demands → exact expression / declared RN stages → requested owned output / complete result`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

NUM special-value and overflow semantics apply. Resource admission and failure follow the shared FILTER contract.
