---
spec_schema_version: 1
id: FRQ-02B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-02
function: irfft2
proposed_operation_keys:
- frequency.irfft2
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S10
---

# FRQ-02B: irfft2

Hermitian half-spectrum to real field.Status: **Proposed**.

Inherits [FRQ-02 family contract](FRQ-02_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). This member and its family contract define the complete target behavior. The ordinary floating-point F contract in FRQ-01 governs this transform; it explicitly replaces NUM correct-rounding, exact exceptional-value ordering and accelerated-ULP requirements for this member.

## Ports, Shape, and Semantics

`real, imag` FrequencyGrid/v1 `r2c_x` → `output` with the original spatial shape.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

The grid descriptor must provide `original_shape`, `norm`, and transformed axes. Reject shifted half-grids. Preflight fails if the original width is missing.

Inputs and outputs have matching Float32 or Float64 dtype. All static configuration is explicit. Arithmetic uses the ordinary floating-point rules in [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles), including intermediate overflow and implementation-dependent NaN payloads and zero signs. Floating invalid arithmetic does not itself cause an operation failure. Resource and upstream failures remain operational.

## Mathematical Reference and Rounding

Numeric reference: **F**, ordinary floating-point approximation; see [numeric reference](FILTER_numeric_reference.md).

Callers guarantee conjugacy of redundant DC and Nyquist boundary pairs, treating signed zeros as equal. Complete missing columns by `F[-ky,-kx]=conj(F[ky,kx])` and return the real part of the ordinary floating-point inverse DFT. The implementation performs no conjugacy-validation or repair pass. Independently edited interior coefficients are supported; a forward-transform producer is not required.

Twiddle generation, butterfly operations and normalization incur ordinary floating-point rounding. There is no final correct-rounding proof, interval validation or exact recomputation. Small residuals at mathematical zeros are allowed. Float32 outputs are converted from binary64 working values. The mathematical reference supplies error measurements, not mandatory output bits.

## Fixed reads, algorithmic control and schemas

Inverse reconstruction requires fixed reads covering the complete half-spectrum. Conjugacy is an unchecked numerical premise and adds no validation reads. The inverse does not implicitly project or repair coefficient pairs; use the explicit projection member when that operation is intended.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

The CPU implementation uses radix-2/radix-4, bounded mixed-radix codelets and Bluestein convolution. It uses one full complex working plane, a linear line buffer and bounded column/convolution workspace. Native binary64 arithmetic is used for both input dtypes. The mathematical direct oracle is O(P²); the production transform has O(P log P) scaling with bounded small-radix constants, where P=HW.

Work blocks are admitted before computation and check cancellation independently at intervals of at most 1024 declared scalar units. No refinement stages are requested. Scratch, root tables and outputs are resource-accounted. A failed Run publishes no final Result; existing exports from retired calls retain their owners. See [FRQ-01 runtime integration](FRQ-01_contract.md#runtime-integration-and-remaining-acceptance).

## Oracle, Fixtures, and Acceptance

Reconstruct small rfft fixtures. The kx=0 column pair ky=1: 1+i and ky=2: 1−i is valid; changing the second value to 1+i is invalid. A nonzero imaginary component at a self-conjugate location is invalid.

Production results are compared with the F-profile error criteria in [the acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance), rather than bitwise equality. The reference function evaluates the mathematical formula on small inputs. It is neither a production kernel nor a complete port-schema or preflight simulator. ExactRational rounds exact rational expressions directly to IEEE format. For transcendental functions, only results from a DirectedMPFR interval that determines the final rounding are strict goldens. Return Inconclusive when the interval cannot determine rounding; do not substitute an approximation.

`frq02.spectral.irfft2(real, imag, original_width, *, norm, dtype)` → [source file](../../../../oracle/ops/filter/frq02/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md). These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

`frequency.irfft2` is registered by `register_frequency` in `frequency_operations.cpp` and executed by `frequency_transform.cpp`. The portable CPU implementation has no strict/accelerated profile selector. Target-platform performance and bit patterns are not implied by registration. No GPU backend is provided. The SDK `frequency::transform` takes the member, inputs and explicit grid configuration.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → ordinary floating-point FFT → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S10 · FFTW: real-data array format](../research-sources.md#s10)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

The F-profile special-value and overflow rules apply. Resource admission and failure follow the shared FILTER contract.
