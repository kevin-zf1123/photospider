---
spec_schema_version: 1
id: FRQ-04B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: partially_implemented
parent_id: FRQ-04
function: apply_window
proposed_operation_keys:
- frequency.apply_window_strict
- frequency.apply_window_accelerated_apple_silicon
- frequency.apply_window_accelerated_x86_64
numeric_reference: E
oracle_scope: mathematical_reference
research_sources:
- S14
- S15
---

# FRQ-04B: apply_window

Apply a window explicitly.Status: **Proposed**.

Inherits [FRQ-04 family contract](FRQ-04_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). This member and its family contract define the complete target behavior. RN stages, gradual underflow, and accelerated error limits are inherited and may not be weakened.

## Ports, Shape, and Semantics

`input, window` → `output`. The window is Float64 and has the same spatial shape. Non-spatial axes may be broadcast only through an explicit `broadcast_axes` list.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

Required parameters: axis and the `broadcast_axes` list. Do not divide automatically by coherent gain or energy.

Floating-point values, including NaN and infinities, and floating-point overflow follow the corresponding NUM operation. This specification does not impose a category-wide finite-only input rule or turn floating-point overflow into an operation failure. Copy/bypass and unconsumed samples follow this member’s stated demand semantics. All applicable constructor parameters must be supplied explicitly. Parameters that do not apply to the selected profile must be omitted. Constructor defaults are not provided; supplied arguments are stored in the direct node. Real parameters denote their stored Float64 values; decimal text is not treated as an exact real parameter.

## Mathematical Reference and Rounding

Numeric reference: **E** ; E/B/S definitions are in [FILTER_numeric_reference](FILTER_numeric_reference.md).

At each point, compute `RN_t(input*window)`.A zero window coefficient does not suppress normal NUM arithmetic: the corresponding multiplication still reads and processes the input, including NaN/Inf propagation.

When this formula invokes another member, its RN stages must be retained as stated here or in the family contract; internal temporary operations may not introduce undeclared rounding. Signed zero, numerical-domain premises and explicit valid-input no-solution outcomes, gradual underflow, and output range follow the shared contract and NUM.

## Fixed reads, algorithmic control and schemas

Pointwise arithmetic consumes the input and corresponding window coordinates. Preserve the declared non-spatial member layout; downstream interpretation is supplied explicitly by the caller.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

O(PC), exact product.

In complexity notation, P=HW, C is the number of independent planes, A is the tap count, and T is the iteration count. Arbitrary-precision limb complexity is additional and is not a measured benchmark. The shared contract defines work, memory, and stage admission, cancellation, failure status, and owner lifetime. A failed call publishes no successful Result export; previously retired call exports remain valid, and final RET results require successful Run completion. Execution does not spill automatically, lower precision silently, or invoke a retired implementation as fallback.

## Oracle, Fixtures, and Acceptance

A +0 Hann endpoint multiplied by a positive finite input produces +0; multiplied by a negative finite input it produces −0. Zero operands retain the NUM multiplication sign rules. NaN inputs follow NUM even where the corresponding window coefficient is zero. No brightness compensation is applied.

The reference function evaluates the mathematical formula on small inputs. It is neither a production kernel nor a complete port-schema or preflight simulator. ExactRational rounds exact rational expressions directly to IEEE format. For transcendental functions, only results from a DirectedMPFR interval that determines the final rounding are strict goldens. Return Inconclusive when the interval cannot determine rounding; do not substitute an approximation.

`frq04.spectral.apply_window(image, win, *, dtype)` → [source file](../../../../oracle/ops/filter/frq04/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md). These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

The listed keys are registered by `register_frequency` in `plugins/ops/05-filter/frequency_operations.cpp`; `frequency_local.cpp` evaluates certified/exact products, preserving NUM multiplication zero signs. CPU strict and target-gated Apple Silicon/x86-64 profiles exist; local evidence covers Apple CPU only, with no GPU backend. Overall acceptance retains the family's frozen-ABI H08 root-work/refinement-stage limitation. Accelerated variants must satisfy NUM’s final FP32-scaled four-ULP bound and fallback rules. Do not accumulate the error budget across taps, axes, stages, or iterations. Approximation may not change exact decisions, thresholds, ordering, boundaries, copy behavior, or output support.

Float64 accelerated variants retain Float64 inputs, outputs, and exponent range; they do not convert through Float32. Float32 inputs outside the supported scale range use the NUM strict fallback.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → unchecked arithmetic with declared RN stages → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S14 · SciPy Hann window](../research-sources.md#s14); [S15 · SciPy Tukey window](../research-sources.md#s15)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

NUM special-value and overflow semantics apply. Resource admission and failure follow the shared FILTER contract.
