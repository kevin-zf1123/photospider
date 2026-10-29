---
spec_schema_version: 1
id: FRQ-07B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-07
function: circular_fft_convolution
proposed_operation_keys:
- frequency.circular_fft_convolution
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S11
- S09
---

# FRQ-07B: circular_fft_convolution

Fixed periodic circular convolution .Status: **Proposed**.

Inherits the [FRQ-07 family contract](FRQ-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. The family's local F profile takes precedence over inherited strict rounding and accelerated-error limits; structural, resource and lifecycle requirements remain applicable.

## Ports, Shape, and Semantics

`input,kernel` → `output` with the input shape.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

Axes and anchor are explicit. The period is fixed to H/W and must not be inferred from the kernel. Normalization is none|sum|l1; bias is 0.

NaN, infinities, signed zero, underflow and overflow follow the selected native execution path and established floating-point environment. No finite-only payload scan or numerical-domain operation failure is introduced. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **F**, as defined by the [FRQ-07 family contract](FRQ-07_contract.md). The mathematical reference remains direct convolution.

The mathematical target is `sum_{j,i} K[j,i]*input[(y+ay-j)%H,(x+ax-i)%W]/N`. The period remains H/W. When the kernel exceeds the period, every tap contributes through modulo indexing or folding with ordinary working-dtype accumulation; no repeated tap may be discarded.

Kernel normalization is evaluated in binary64 before conversion to the native working dtype. Float64 execution is never narrowed to Float32. Float32-input execution may use binary64 arithmetic to retain the Float64 kernel's range, with final conversion to Float32. Explicit FMA is permitted. Correct rounding, fixed ULP bounds and cross-platform bit patterns are not promised. Document absolute/relative error criteria; validate shapes, anchors, boundaries and support independently.

## Fixed reads, algorithmic control and schemas

Complete-input demand covers the input plane and kernel for both numerical strategies. The direct small-kernel strategy retains this demand and key; it does not introduce a sparse Region mode.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

The FFT path costs O(P C log P + A); direct evaluation costs O(P C A). Reserve the selected strategy's transform workspace, immutable plans, accumulation buffers and unpublished outputs. P denotes plane pixels, C independent planes and A kernel taps.

The current native implementation uses direct convolution when Kh<=15, Kw<=15 and Kh*Kw<=81; otherwise it uses the configured FFT strategy. Both strategies implement the same declared convolution under the F profile and the same unsuffixed operation key. Static shape, anchor, normalization, padding and block constraints remain mandatory, including for the direct strategy. The kernel is dynamic: every invocation consumes and normalizes its supplied coefficients and performs direct preparation or the kernel FFT. Shape-dependent plans and coefficient tables may be reused; kernel spectra are not reused across invocations.

The shared protocol defines work, memory and stage admission, cancellation, failure Status and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision or call retired implementations as fallback.

## Oracle, Fixtures, and Acceptance

For length-2 input [1,2] and kernel [1,2,3] with anchor 0, output is [8,10]. This differs from constant-boundary linear convolution. Larger kernels fold repeated taps.

The reference evaluates the mathematical formula on small inputs; it is not a production kernel or complete schema/preflight simulator. Exact rational or directed high-precision references diagnose numerical error; their rounded bits do not impose bitwise acceptance on this F profile. Compare outputs with documented absolute/relative error criteria, and verify discrete shapes, anchors, boundaries and support independently.

`fil01.spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='convolve', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype)` → [source](../../../../oracle/ops/filter/fil01/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The native operation is implemented and registered as `frequency.circular_fft_convolution`. CPU implementations share this unsuffixed key and the family F profile. Separate strict or accelerated suffixes are not registered.

Implementation and registration are complete; this does not establish complete numerical, runtime or performance acceptance.

## Execution Outline

`static parameters + schemas → compile fixed reads/tasks → prepare → ordinary native floating-point computation → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S11 · SciPy fftconvolve](../research-sources.md#s11) ; [S09 · FFTW: DFT definition](../research-sources.md#s09)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

Special values and overflow follow the family's native F profile and selected arithmetic. Resource admission and failure follow the shared FILTER contract.
