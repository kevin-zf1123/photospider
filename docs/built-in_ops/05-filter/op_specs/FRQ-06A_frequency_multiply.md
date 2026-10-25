---
spec_schema_version: 1
id: FRQ-06A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-06
function: frequency_multiply
proposed_operation_keys:
- frequency.frequency_multiply_strict
- frequency.frequency_multiply_accelerated_apple_silicon
- frequency.frequency_multiply_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S09
---

# FRQ-06A: frequency_multiply

Pointwise complex-frequency multiplication. Status: **Proposed**.

Inherits the [FRQ-06 family contract](FRQ-06_contract.md), [FILTER common contract](FILTER_common_contract.md), [NUM numerical and accuracy contract](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md); this member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`f_real,f_imag,h_real,h_imag` → `out_real,out_imag`; F dtype is Float32/Float64 and H may be Float64. Packing, full/half representation, index order, and original grid must be compatible.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

`broadcast_axes` is explicit and may not broadcast across frequency axes. The response tag must be dimensionless. Reject F and H with different `index_order` values.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All parameters listed here are mandatory constructor arguments; constants fixed by a named mathematical profile are not configurable parameters. Every argument is stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E**; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

`Re=RN_t(fr*hr-fi*hi)`, `Im=RN_t(fr*hi+fi*hr)`. Each component evaluates its exact products and sum with one final RN; this is not three rounded complex-multiply operations. Real-only output still needs fi/hi because of the cross terms. If `H_imag` is statically declared identically zero, a separate exact optimization certificate may be used.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

Lowering closes pointwise arithmetic support for the selected output components on fixed input tile grids. Static shape, transformed-axis, packing and index-order compatibility are checked before execution. Sampling-unit and numerical association premises remain caller obligations.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(PC), with exact two-product sums. Preserve half-spectrum real-reconstruction compatibility only when both structure and samples are valid; do not assume H is Hermitian.

In the complexity notation, P=HW, C=independent planes, A=taps, and T=steps. Arbitrary-precision limb bit complexity is accounted for separately and is not a measured benchmark. The shared protocol defines work, memory, and stage admission; cancellation; failure Status; and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Do not spill automatically, silently lower precision, or call retired implementations as fallback.

## Oracle, Fixtures, and Acceptance

Fixture: (1+2i)*(3+4i)=-5+10i; also cover zero imaginary parts and signed zero. Incompatible static original shapes fail construction; invalid numerical spacing is an oracle-only domain diagnostic and not a production payload check.

The reference function evaluates the numerical formula on small inputs; it is not a production kernel or a complete Ports-schema/preflight simulator. ExactRational evaluates rational expressions and rounds directly to IEEE format. For transcendental functions, use a DirectedMPFR result as a strict golden only when the interval is closed. If the interval does not determine rounding, report Inconclusive rather than substituting an approximation.

`frq06.spectral.multiply(fr, fi, hr, hi, *, dtype)` → [source](../../../../oracle/ops/filter/frq06/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md) These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md) Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The listed keys are registered by `register_frequency` in `plugins/ops/05-filter/frequency_operations.cpp`; `frequency_local.cpp` evaluates exact/certified two-product sums with one final RN per component and written-operand NaN priority. CPU strict and target-gated Apple Silicon/x86-64 profiles exist; local evidence covers Apple CPU only, with no GPU backend. Typed CPU9 supplies the family's H08 cumulative arithmetic/limb and refinement-stage admission; old FRQ ABI8 Plans require recompilation. Accelerated implementations must satisfy the NUM final-value Float32-scaled four-ULP bound and fallback rules. Error budgets may not be accumulated per tap, axis, stage, or iteration. Approximation may not change exact decisions such as thresholds, ordering, boundaries, copies, or output support.

Float64 accelerated execution retains Float64 inputs, outputs, and exponent range; it does not convert to Float32 first. Values outside the Float32-scaled range use the strict NUM fallback. Runtime and performance evidence is scoped to the configurations actually executed; registration alone does not establish cross-platform acceptance.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S09 · FFTW: DFT definition](../research-sources.md#s09)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

NUM special-value and overflow semantics apply. Resource admission and failure follow the shared FILTER contract.
