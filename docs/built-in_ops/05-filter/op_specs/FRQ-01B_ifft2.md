---
spec_schema_version: 1
id: FRQ-01B
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-01
function: ifft2
proposed_operation_keys:
- frequency.ifft2
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S09
- S10
- S25
---

# FRQ-01B: ifft2

Two-dimensional complex inverse transform.Status: **Proposed**.

Inherits [FRQ-01 family contract](FRQ-01_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). This member and its family contract define the complete target behavior. The ordinary floating-point F contract in FRQ-01 governs this transform; it explicitly replaces NUM correct-rounding, exact exceptional-value ordering and accelerated-ULP requirements for this member.

## Ports, Shape, and Semantics

`real,imag` with identical shape and dtype → `out_real, out_imag` ; input must declare a full, unshifted FrequencyGrid, output is a generic spatial field.

Ports carry Results with fixed Tensor member schemas. Unless the member defines an explicit color interpretation, samples are raw numerical values; channel count does not imply RGB or alpha. Color interpretation is explicit configuration. Positive extents, axes and explicit broadcasting are checked during static construction. Materialized output tiles have independent allocations. FilterBands and FrequencyGrid are proposed SDK/workflow representations under the collection contract; lowering records each selected output's fixed reads and writes.

## Static Parameters and Valid Domain

Required parameters: transformed axes; `norm` (`backward` or `ortho`); positive `spacing_y/x`; explicit `origin_y/x`; and a unit (`pixel` or a caller-defined unit). The inverse consumes `norm` from the compatible grid descriptor and rejects a conflicting override.

Inputs and outputs have matching Float32 or Float64 dtype. All static configuration is explicit. Arithmetic uses the ordinary floating-point rules in [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles), including intermediate overflow and implementation-dependent NaN payloads and zero signs. Floating invalid arithmetic does not itself cause an operation failure. Resource and upstream failures remain operational.

## Mathematical Reference and Rounding

Numeric reference: **F**, ordinary floating-point approximation; see [numeric reference](FILTER_numeric_reference.md).

The target is `z[y,x] = s * sum(F[ky,kx] * exp(+2*pi*i*(ky*y/H+kx*x/W)))`. The implementation approximates this mathematical inverse DFT with a floating-point FFT.

Twiddle generation, butterfly operations and normalization incur ordinary floating-point rounding. There is no final correct-rounding proof, interval validation or exact recomputation. Small residuals at mathematical zeros are allowed. Float32 outputs are converted from binary64 working values. The mathematical reference supplies error measurements, not mandatory output bits.

## Fixed reads, algorithmic control and schemas

complete-input demand applies to each transform group. Even a single requested coefficient q reads all real and imaginary samples in that group. Distinct batches do not read one another.

Boundary coordinates use the complete logical image; ROI and tile edges are not new image boundaries. Lowering closes mathematical sample support on fixed input member-tile grids. Data-dependent selection uses a finite static superset of every address the task may consume; runtime selection does not expand the Plan. Data and control are algorithmic inputs, with no separate Validation read set. Empty retained selections introduce no payload tasks. Output schemas derive only from static parameters and input schemas. Numerical, encoding and association premises remain unchecked under the common contract.

## Algorithm, Resources, and Execution

The CPU implementation uses radix-2/radix-4, bounded mixed-radix codelets and Bluestein convolution. It uses one full complex working plane, a linear line buffer and bounded column/convolution workspace. Native binary64 arithmetic is used for both input dtypes. The mathematical direct oracle is O(P²); the production transform has O(P log P) scaling with bounded small-radix constants, where P=HW.

Work blocks are admitted before computation and check cancellation independently at intervals of at most 1024 declared scalar units. No refinement stages are requested. Scratch, root tables and outputs are resource-accounted. A failed Run publishes no final Result; existing exports from retired calls retain their owners. See [FRQ-01 runtime integration](FRQ-01_contract.md#runtime-integration-and-remaining-acceptance).

## Oracle, Fixtures, and Acceptance

Acceptance fixtures include an impulse at (0,0), whose forward transform under backward normalization is all real 1 and imaginary 0 (under ortho: real 1/sqrt(H*W)); a constant single plane with DC=H*W*c under backward or sqrt(H*W)*c under ortho and all other coefficients zero; 1×1; odd/even 2×3 single-frequency inputs; round trips that are numerically close without general bit identity; and Parseval checks that distinguish normalization modes.

Production results are compared with the F-profile error criteria in [the acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance), rather than bitwise equality. The reference function evaluates the mathematical formula on small inputs. It is neither a production kernel nor a complete port-schema or preflight simulator. ExactRational rounds exact rational expressions directly to IEEE format. For transcendental functions, only results from a DirectedMPFR interval that determines the final rounding are strict goldens. Return Inconclusive when the interval cannot determine rounding; do not substitute an approximation.

`frq01.spectral.dft2(real, imag, *, inverse, norm, dtype)` → [source file](../../../../oracle/ops/filter/frq01/spectral.py).

Fixture mapping and evidence scope: see [oracle coverage](../oracle-coverage.md).

Reproduction steps and proof levels are in [oracle README](../../../../oracle/ops/filter/README.md). These self-tests validate only the reference program. Parameter limits, full rank and batch behavior, metadata, compiled ROI/tile selection, budgets, cancellation, and owner lifetime require validation through [runtime acceptance protocol](FILTER_oracle_protocol.md). Passing identity or constant fixtures does not establish acceptance of the full algorithm or every configuration.

## Backend and Registration Status

`frequency.ifft2` is registered by `register_frequency` in `frequency_operations.cpp` and executed by `frequency_transform.cpp`. The portable CPU implementation has no strict/accelerated profile selector. Target-platform performance and bit patterns are not implied by registration. No GPU backend is provided. The SDK `frequency::transform` takes the member, inputs and explicit grid configuration.

## Conceptual DAG (Not an Existing API)

`static parameters + schemas → compile fixed reads/tasks → prepare → ordinary floating-point FFT → complete tiles → ordered call retirement`

This diagram adds no implicit color conversion, hidden alpha premultiplication, automatic spectral correction, or zero-fill for missing data. complete-input demand and multistage buffering are defined by this member and its family contract.

## Sources and Open Items

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10); [S25 · GNU MPFR manual](../research-sources.md#s25)

External sources support the algorithmic definition. The finite support, rounding, tie-breaking, units, parameters, and execution profile specified here are project choices and do not claim bitwise equivalence with any library or commercial product.

The F-profile special-value and overflow rules apply. Resource admission and failure follow the shared FILTER contract.
