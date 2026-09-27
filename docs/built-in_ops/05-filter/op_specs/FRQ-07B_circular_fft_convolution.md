---
spec_schema_version: 1
id: FRQ-07B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
parent_id: FRQ-07
function: circular_fft_convolution
proposed_operation_keys:
- frequency.circular_fft_convolution_strict
- frequency.circular_fft_convolution_accelerated_apple_silicon
- frequency.circular_fft_convolution_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S11
- S09
---

# FRQ-07B: circular_fft_convolution

Fixed periodic circular convolution .Status: **Proposed**.

Inherits the [FRQ-07 family contract](FRQ-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`input,kernel` → `output` with the input shape.

Unless this member defines a semantic color entry point, ports are raw numerical fields: channel count does not imply RGB or alpha semantics, and attached color metadata does not widen numeric-domain validation. Axis, positive extent, explicit broadcasting, canonical planar publication, and applicable metadata preservation or reconstruction follow the shared contract. Each requested component/output declares its own demand. Associated collections use the FrequencyGrid/v1 or FilterBands/v1 logical schema defined by the shared contract.

## Static Parameters and Valid Domain

Axes and anchor are explicit. The period is fixed to H/W and must not be inferred from the kernel. Normalization is none|sum|l1; bias is 0.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E**; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

`RN_t(ΣK[j,i]*input[(y+ay-j)%H,(x+ax-i)%W]/N)`. When the kernel exceeds the period, fold taps modulo the period using an exact sum; do not discard out-of-range taps or keep only the last one.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data, Control, Validation, and Descriptors

Whole demand for the input plane and kernel. A future direct Region mode requires a separate key and explicit plan.

Boundary coordinates are defined against the complete logical image; ROI or tile edges do not become new boundaries. Empty demand reads no payload; selection conditions retain a Control dirty witness. Exact support may not be replaced by a convenient rectangular or Whole read. Any expansion must be explicitly requested as a Conservative plan. Output descriptors are derived from static parameters and input descriptors, never from input sample values.

## Algorithm, Resources, and Execution

FFT candidate complexity is O(P C log P + A); kernel folding is exact. The `same` shape is not called `linear_same`.

In the complexity notation, P=HW, C=independent planes, A=taps, and T=steps. Arbitrary-precision limb bit complexity is accounted for separately and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure Status; and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, Fixtures, and Acceptance

For length-2 input [1,2] and kernel [1,2,3] with anchor 0, output is [8,10]. This differs from constant-boundary linear convolution. Larger kernels fold repeated taps.

The reference function evaluates the numerical formula on small inputs; it is not a production kernel or a complete Ports-schema/preflight simulator. ExactRational evaluates rational expressions and rounds directly to IEEE format. For transcendental functions, use a DirectedMPFR result as a strict golden only when the interval is closed. If the interval does not determine rounding, report Inconclusive rather than substituting an approximation.

`spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='convolve', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype)` → [source](../../../../oracle/ops/filter/oracles/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated implementations must satisfy the NUM final-value Float32-scaled four-ULP bound and fallback rules. Error budgets may not be accumulated per tap, axis, stage, or iteration. Approximation may not change exact decisions such as thresholds, ordering, boundaries, copies, or output support.

Float64 accelerated execution retains Float64 inputs, outputs, and exponent range; it does not convert to Float32 first. Values outside the Float32-scaled range use the strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance runs were performed for this package.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → member-declared Data and Control → exact expression / declared RN stages → requested owned output / complete result`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. Whole demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S11 · SciPy fftconvolve](../research-sources.md#s11) ; [S09 · FFTW: DFT definition](../research-sources.md#s09)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

NUM special-value and overflow semantics apply. Resource admission and failure follow the shared FILTER contract.
