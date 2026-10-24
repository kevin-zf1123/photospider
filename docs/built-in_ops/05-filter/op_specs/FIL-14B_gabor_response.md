---
spec_schema_version: 1
id: FIL-14B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: FIL-14
function: gabor_response
proposed_operation_keys:
- filter.gabor_response_strict
- filter.gabor_response_accelerated_apple_silicon
- filter.gabor_response_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S01
- S26
---

# FIL-14B: gabor_response

Gabor correlation response. Status **Proposed / D1_draft**.

Inherits the [FIL-14 family contract](FIL-14_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT metadata/straight-semantics contract](../../02-format-color/op_specs/FMT_common_contract.md). The family contract and this member together form the complete draft. Do not override the RN, underflow, or accelerated-error baseline.

## Ports, shape, and semantics

`input,kernel_real,kernel_imag` → `response_real,response_imag`; kernel arrays have the same shape and are Float64.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands uses explicit SDK collections and fixed Result members under the collection contract; FrequencyGrid remains a separate proposed representation; lowering records each selected output's fixed reads and writes.

## Static parameters and valid domains

Axes and anchor are explicit; boundary=reflect_half, cval=0; normalization=none and bias=0 are fixed.

Exceptional values and floating-point overflow follow the corresponding NUM operations and the [FILTER common contract](FILTER_common_contract.md). Do not reject NaN/Inf or turn ordinary floating-point overflow into failure under a category-wide rule. Static structural, parameter-type and capacity-arithmetic errors retain pre-execution diagnostics. Numerical-domain premises are unchecked; resource, cancellation and upstream failures retain their contracts. Every parameter must explicitly state its type, valid domain, and value; constructors provide no implicit defaults. Floating-point parameters are interpreted using their actually stored values.

## Mathematical reference and rounding boundaries

Numerical class: **E**; see [FILTER_numeric_reference](FILTER_numeric_reference.md) for the E/B/S notation.

Apply two FIL-01B correlations, each with one exact expression and one final RN_t. Correlation does not conjugate the complex kernel: this is two real-kernel correlations, not a Hermitian inner product.

When a formula calls another member, preserve the RN stages produced at the call site as specified here or by the family contract; internal temporaries must not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

A real-only request does not read the imaginary kernel or its Data. The two paths share input reads only when needed.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, resources, and execution

O(PCA), with two exact sums.

In complexity notation, P=HW, C is the number of independent planes, A the number of taps, and T the number of steps. Arbitrary-precision limb bit complexity is additional and is not a measured benchmark. The shared protocol defines work/memory/stage admission, cancellation, failure state, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, fixtures, and acceptance

An impulse produces the kernel flipped according to correlation direction. A phase shift of π/2 gives the corresponding sign/swap relation. Test direction with an asymmetric kernel.

Reference functions evaluate small numerical examples; they are neither production kernels nor complete port-schema/preflight simulators. ExactRational rounds rational expressions directly to IEEE formats. For transcendental functions, only DirectedMPFR results whose interval endpoints round to the same value are strict goldens. If rounding cannot be determined, report Inconclusive rather than substituting an approximation.

`fil01.spatial.convolve2d(image, kernel, anchor=(0, 0), *, direction='correlate', normalization='none', bias=0, boundary='reflect_half', cval=0, output_shape='same', dtype='float64')` → [source](../../../../oracle/ops/filter/fil01/spatial.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

See the [oracle README](../../../../oracle/ops/filter/README.md) for reproduction and evidence levels. The listed self-tests validate only the reference program; parameter boundaries, full rank/batch, metadata, compiled ROI/tile behavior, budgets, cancellation, and ownership still require [runtime acceptance](FILTER_oracle_protocol.md). Passing an identity/constant fixture does not accept the entire algorithm or every configuration.

## Backend and registration

`gabor.cpp` implements the three CPU profile keys listed above through two fixed reflect-half correlations. Baked kernels remain Float64, outputs retain the input dtype, and axes/anchors are explicit. Real and imaginary outputs have independent read sets, so a real-only selection does not require imaginary-kernel payload. Optional structural `expected_shape`/`expected_dtype` parameters bind responses to the SDK bank directory.

These are CPU/Host fixed-invocation-ABI-8 implementations. Strict and accelerated variants share certified arithmetic and exact fallback. Float64 inputs retain their precision and exponent range; final accelerated error does not accumulate across stages, and exact ordering, copying, exceptional values and gradual underflow are preserved. The available Apple Silicon profile was exercised against independent references. The guarded x86_64 profile is implemented but was not executed on the Apple test host.

Runtime checks use the public compiler, preparation, submission and Result-read interfaces. Numerical references, selected/empty output requests and relevant metadata, resource, cancellation and ownership paths have separate runtime evidence. Implementation does not change Proposed acceptance status or draft maturity.

## Execution model

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This sketch preserves explicit color interpretation, alpha arithmetic, spectral formulas and missing-coverage semantics. Computation scope and multistage storage follow this member and its family contract; shared work is represented by explicit tasks and private SSA outputs.

## Rationale and open decisions

[S01 · OpenCV Image Filtering](../research-sources.md#s01)；[S26 · scikit-image filters](../research-sources.md#s26)

External sources provide algorithmic or definitional background. The finite windows, rounding, ties, units, defaults, and execution profile here are explicit project choices and do not claim bitwise identity with any library or commercial software.
