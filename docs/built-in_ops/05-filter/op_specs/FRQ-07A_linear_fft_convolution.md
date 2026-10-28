---
spec_schema_version: 1
id: FRQ-07A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D2
implementation_status: not_implemented
parent_id: FRQ-07
function: linear_fft_convolution
proposed_operation_keys:
- frequency.linear_fft_convolution_strict
- frequency.linear_fft_convolution_accelerated_apple_silicon
- frequency.linear_fft_convolution_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S11
- S09
---

# FRQ-07A: linear_fft_convolution

Linear convolution family .Status: **Proposed**.

Inherits the [FRQ-07 family contract](FRQ-07_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`input,kernel` → `output`, and FIL01Awith the same Float data and Float64 kernel as FIL01A .

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

Axes; explicit anchor; shape=same|full|valid; fixed constant-zero boundary; normalization none|sum|l1; bias 0; fft_padding_shape contains two Int64 values, each at least H+Kh-1/W+Kw-1 respectively. The constructor may choose any sizes satisfying these conditions. No implicit periodic folding.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E**; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

The result exactly matches the FIL01A zero-extension direct E formula, including its origin. Padding shape affects execution only and does not change the strict mathematical value. An ordinary F-profile FFT followed by rounded products and an ordinary inverse transform does not establish the exact direct strict output. Such a chain is a separate workflow with its explicitly declared numerical stages.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

The transform requires the complete input plane and complete kernel. Requests must declare this whole-plane demand; no sparse exact plan is implied.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

The FFT candidate stage costs O(Ppad log(Ppad) C), where Ppad is the padded
plane size. A direct fallback requires O(PCA) arithmetic operations; arbitrary
precision adds its own limb and refinement costs. Certification and a conforming
fallback can require additional work and storage, so the candidate stage alone
does not establish an overall worst-case bound. Reserve complete padded planes,
complex scratch, immutable plans and certification/fallback storage covered by
the resource model. Follow the [family's strict evaluation requirements](FRQ-07_contract.md#use-of-ordinary-fft-primitives)
and the shared executor ownership boundary.

In the complexity notation, P=HW, C=independent planes, A=taps, and T=steps. Arbitrary-precision limb bit complexity is accounted for separately and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure Status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, Fixtures, and Acceptance

For an asymmetric even-sized kernel, all supported shapes must be bitwise consistent with FIL01A. Changing valid padding must not change strict results; padding one element too small fails preflight.

The reference function evaluates the numerical formula on small inputs; it is not a production kernel or a complete Ports-schema/preflight simulator. ExactRational evaluates rational expressions and rounds directly to IEEE format. For transcendental functions, use a DirectedMPFR result as a strict golden only when the interval is closed. If the interval does not determine rounding, report Inconclusive rather than substituting an approximation.

`fil01.spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='convolve', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype)` → [source](../../../../oracle/ops/filter/fil01/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The keys above are naming proposals only; strict and both CPU-accelerated variants are not registered or implemented. Accelerated implementations must satisfy the NUM final-value Float32-scaled four-ULP bound and fallback rules. Error budgets may not be accumulated per tap, axis, stage, or iteration. Approximation may not change exact decisions such as thresholds, ordering, boundaries, copies, or output support.

Float64 accelerated execution retains Float64 inputs, outputs, and exponent range; it does not convert to Float32 first. Values outside the Float32-scaled range use the strict NUM fallback. No kernel, CPU, or ISA performance or differential acceptance runs were performed for this package.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S11 · SciPy fftconvolve](../research-sources.md#s11) ; [S09 · FFTW: DFT definition](../research-sources.md#s09)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.
