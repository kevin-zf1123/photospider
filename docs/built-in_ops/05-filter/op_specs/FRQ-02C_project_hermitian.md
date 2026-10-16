---
spec_schema_version: 1
id: FRQ-02C
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
parent_id: FRQ-02
function: project_hermitian
proposed_operation_keys:
- frequency.project_hermitian_strict
- frequency.project_hermitian_accelerated_apple_silicon
- frequency.project_hermitian_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S10
---

# FRQ-02C: project_hermitian

Explicit Hermitian-symmetry projection.Status: **Proposed**.

Inherits [FRQ-02 family contract](FRQ-02_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). This member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`real,imag` full, unshifted grid → `out_real, out_imag` with the same grid descriptor.

Unless the member defines an explicit color interpretation, ports contain raw numerical Tensor samples; channel count does not imply RGB or alpha. Color descriptions are explicit operation parameters, never attached Result metadata. Positive extents, axis validation and explicit broadcasting follow the common contract. Host-created backing defaults to tiled planar without requiring a conformance declaration. Associated FilterBands/FrequencyGrid structures remain Proposed static Tensor schemas; each requested output records its actual tile Needs.

## Static Parameters and Valid Domain

Required parameters are axes and a compatible grid descriptor. Do not infer missing pairs directly from half-spectrum data.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All applicable constructor parameters must be supplied explicitly. Parameters that do not apply to the selected profile must be omitted. Constructor defaults are not provided; supplied arguments are stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

For each pair k and −k, compute `a=RN_t((ReF[k]+ReF[−k])/2)` and `b=RN_t((ImF[k]−ImF[−k])/2)`. Use the lower row-major index as representative and set the other output to `(a,−b)`; zero imaginary values become +0. At self-conjugate locations, output imaginary +0 and copy the real component bitwise. Arithmetic follows NUM; this projection does not validate or claim that the input pair was conjugate. An output containing NaN may be invalid for `irfft` structural validation.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data, Control, Validation, and Descriptors

Each requested coefficient q depends on its conjugate partner. Demand is the exact pair support. A matching full-grid shape does not replace grid-compatibility validation.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Mathematical sample support is rounded to the current plan's member tiles for payload Needs, authorization and invalidation. Control and validation samples use ordinary payload Needs and participate in the same evidence. Empty payload demand reads no samples. Output structure derives only from static parameters and input schemas. The kernel has no support-role or Exact/Conservative/Unknown dependency mode.

## Algorithm, Resources, and Execution

O(PC), using exact rational pair expressions. The two related outputs do not create an additional publication.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. The shared contract defines work, memory, and stage admission, cancellation, failure status, and owner lifetime. Failure publishes no partial Tensor or CompleteBundle. Execution does not spill automatically, lower precision silently, or invoke a retired implementation as fallback.

## Oracle, Fixtures, and Acceptance

Acceptance must verify bitwise idempotence on the specified NUM domain, +0 imaginary output at self-conjugate locations, and correctly rounded pair averages without avoidable intermediate overflow. A projected output containing NaN may fail the exact Hermitian structural check when consumed by irfft; the projection does not assert false conjugacy or bypass that validation.

The reference function evaluates the mathematical formula on small inputs. It is neither a production kernel nor a complete port-schema or preflight simulator. ExactRational rounds exact rational expressions directly to IEEE format. For transcendental functions, only results from a DirectedMPFR interval that determines the final rounding are strict goldens. Return Inconclusive when the interval cannot determine rounding; do not substitute an approximation.

`frq02.spectral.project_hermitian(real, imag, *, dtype)` → [source file](../../../../oracle/ops/filter/frq02/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md). These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM’s final FP32-scaled four-ULP bound and fallback rules. Do not accumulate the error budget across taps, axes, stages, or iterations. Approximation may not change exact decisions, thresholds, ordering, boundaries, copy behavior, or output support.

Float64 accelerated variants retain Float64 inputs, outputs, and exponent range; they do not convert through Float32. Float32 inputs outside the supported scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → this member’s declared Data and Control demands → exact expression / declared RN stages → requested owned output / complete result`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S10 · FFTW: real-data array format](../research-sources.md#s10)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

NUM special-value and overflow semantics apply. Resource admission and failure follow the shared FILTER contract.
