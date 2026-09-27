---
spec_schema_version: 1
id: FRQ-01A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
parent_id: FRQ-01
function: fft2
proposed_operation_keys:
- frequency.fft2_strict
- frequency.fft2_accelerated_apple_silicon
- frequency.fft2_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S09
- S10
- S25
---

# FRQ-01A: fft2

Two-dimensional complex forward transform.Status: **Proposed**.

Inherits [FRQ-01 family contract](FRQ-01_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). This member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`real,imag` with identical shape and dtype → `out_real, out_imag` ; outputs a full, unshifted grid FrequencyGrid/v1.For real input, explicitly set the imaginary component to zero or use FRQ-02A..

Unless this member defines a semantic color entry point, ports are raw numerical fields: channel count does not imply RGB or alpha semantics, and attached color metadata does not widen numeric-domain validation. Axis, positive extent, explicit broadcasting, canonical planar publication, and applicable metadata preservation or reconstruction follow the shared contract. Each requested component/output declares its own demand. Associated collections use the FrequencyGrid/v1 or FilterBands/v1 logical schema defined by the shared contract.

## Static Parameters and Valid Domain

Required parameters: transformed axes; `norm` (`backward` or `ortho`); positive `spacing_y/x`; explicit `origin_y/x`; and a unit (`pixel` or a caller-defined unit). The inverse consumes `norm` from the compatible grid descriptor and rejects a conflicting override.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All applicable constructor parameters must be supplied explicitly. Parameters that do not apply to the selected profile must be omitted. Constructor defaults are not provided; supplied arguments are stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

`F[ky,kx]=RN_t_components(s * Σ_{y,x}z[y,x]*exp(-2πi*(ky*y/H+kx*x/W)))`. The complex products, trigonometric values, and scale inside the summation are exact real expressions. Each output component is rounded only once, at the end; this is not a DFT using RN64 twiddle factors. π is the mathematical constant.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data, Control, Validation, and Descriptors

Whole demand applies to each transform group. Even a single requested coefficient q reads all real and imaginary samples in that group, except terms proved exactly zero may be removed in the exact plan. Distinct batches do not read one another.

Boundary coordinates are defined against the complete logical image; ROI or tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain a Control dirty witness. Exact support may not be replaced by a convenient rectangular or Whole read. Any expansion must be explicitly requested as a Conservative plan. Output descriptors are derived from static parameters and input descriptors, never from input sample values.

## Algorithm, Resources, and Execution

Use a direct O(P²C) certified reference. A native O(PC logP) FFT is permitted only with a final rounding enclosure or fallback. Round-trip error is not a proof that each DFT output is correctly rounded. Global scratch and plan lifetime are budgeted.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. The shared contract defines work, memory, and stage admission, cancellation, failure status, and owner lifetime. Failure publishes no partial Value or CompleteBundle. Execution does not spill automatically, lower precision silently, or invoke a retired implementation as fallback.

## Oracle, Fixtures, and Acceptance

Acceptance fixtures include an impulse at (0,0), whose forward transform under backward normalization is all real 1 and imaginary 0 (under ortho: real 1/sqrt(H*W)); a constant single plane with DC=H*W*c under backward or sqrt(H*W)*c under ortho and all other coefficients zero; 1×1; odd/even 2×3 single-frequency inputs; round trips that are numerically close without general bit identity; and Parseval checks that distinguish normalization modes.

The reference function evaluates the mathematical formula on small inputs. It is neither a production kernel nor a complete port-schema or preflight simulator. ExactRational rounds exact rational expressions directly to IEEE format. For transcendental functions, only results from a DirectedMPFR interval that determines the final rounding are strict goldens. Return Inconclusive when the interval cannot determine rounding; do not substitute an approximation.

`spectral.dft2(real, imag, *, inverse, norm, dtype)` → [source file](../../../../oracle/ops/filter/oracles/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md). These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated variants must satisfy NUM’s final FP32-scaled four-ULP bound and fallback rules. Do not accumulate the error budget across taps, axes, stages, or iterations. Approximation may not change exact decisions, thresholds, ordering, boundaries, copy behavior, or output support.

Float64 accelerated variants retain Float64 inputs, outputs, and exponent range; they do not convert through Float32. Float32 inputs outside the supported scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → this member’s declared Data and Control demands → exact expression / declared RN stages → requested owned output / complete result`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. Whole demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10); [S25 · GNU MPFR manual](../research-sources.md#s25)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

NUM special-value and overflow semantics apply. Resource admission and failure follow the shared FILTER contract.
