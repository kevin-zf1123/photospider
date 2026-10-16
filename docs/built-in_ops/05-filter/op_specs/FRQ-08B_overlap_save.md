---
spec_schema_version: 1
id: FRQ-08B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: not_implemented
parent_id: FRQ-08
function: overlap_save
proposed_operation_keys:
- frequency.overlap_save_strict
- frequency.overlap_save_accelerated_apple_silicon
- frequency.overlap_save_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S12
---

# FRQ-08B: overlap_save

Overlap-save convolution .Status: **Proposed**.

Inherits the [FRQ-08 family contract](FRQ-08_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`input,kernel` → `output`, using zero extension and full/same/valid shapes as in FIL01A.

Unless the member defines an explicit color interpretation, ports contain raw numerical Tensor samples; channel count does not imply RGB or alpha. Color descriptions are explicit operation parameters, never attached Result metadata. Positive extents, axis validation and explicit broadcasting follow the common contract. Host-created backing defaults to tiled planar without requiring a conformance declaration. Associated FilterBands/FrequencyGrid structures remain Proposed static Tensor schemas; each requested output records its actual tile Needs.

## Static Parameters and Valid Domain

Axes, anchor, output shape, and normalization none|sum|l1 are explicit. block_payload_height and block_payload_width are positive. fft_shape is at least payload+kernel-1. The block grid is anchored at the global input origin and does not restart at an ROI. A fixed kernel has no radius field.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E**; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

Strict results exactly match FIL01A direct E. Each block reads its payload plus Kh-1/Kw-1 overlap. Discard contaminated edges, then select valid outputs relative to the global convolution origin; discarded edges are not final valid output.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, dynamic-domain failures, gradual underflow, and output range follow the shared contract and NUM.

## Data, Control, Validation, and Descriptors

The initial Conservative demand includes blocks involved in requested outputs and the ranges actually read by their FFTs, including gaps/halos; it must not claim pointwise Exact support. The full kernel is Control; empty Q reads no block.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Mathematical sample support is rounded to the current plan's member tiles for payload Needs, authorization and invalidation. Control and validation samples use ordinary payload Needs and participate in the same evidence. Empty payload demand reads no samples. Output structure derives only from static parameters and input schemas. The kernel has no support-role or Exact/Conservative/Unknown dependency mode.

## Algorithm, Resources, and Execution

Candidate complexity: O(blocks * Pfft log Pfft * C). Block scheduling and work order do not change strict bits. Account for overlapping output accumulators and unpublished tiles in the budget.

In the complexity notation, P=HW, C=independent planes, A=taps, and T=steps. Arbitrary-precision limb bit complexity is accounted for separately and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure Status; and owner lifetime. Failures do not publish partial Values or CompleteBundles. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, Fixtures, and Acceptance

Verify agreement with direct convolution for at least three block shapes, impulses crossing block boundaries, whole-image versus ROI-tile requests, and kernels larger than the payload. A low budget must not silently switch to a non-equivalent approximation.

The reference function evaluates the numerical formula on small inputs; it is not a production kernel or a complete Ports-schema/preflight simulator. ExactRational evaluates rational expressions and rounds directly to IEEE format. For transcendental functions, use a DirectedMPFR result as a strict golden only when the interval is closed. If the interval does not determine rounding, report Inconclusive rather than substituting an approximation.

`fil01.spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='convolve', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype)` → [source](../../../../oracle/ops/filter/fil01/spatial.py).
`frq08.spectral.block_partition(height, width, payload_height, payload_width, kernel_height, kernel_width, *, save=False)` → [source](../../../../oracle/ops/filter/frq08/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, ROI/dirty propagation, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated implementations must satisfy the NUM final-value Float32-scaled four-ULP bound and fallback rules. Error budgets may not be accumulated per tap, axis, stage, or iteration. Approximation may not change exact decisions such as thresholds, ordering, boundaries, copies, or output support.

Float64 accelerated execution retains Float64 inputs, outputs, and exponent range; it does not convert to Float32 first. Values outside the Float32-scaled range use the strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance runs were performed for this package.

## Conceptual DAG (Not an Existing API)

`static + descriptors → preflight / demand → member-declared Data and Control → exact expression / declared RN stages → requested owned output / complete result`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S12 · SciPy oaconvolve](../research-sources.md#s12)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.
