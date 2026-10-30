---
spec_schema_version: 1
id: FRQ-01A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-01
function: fft2
proposed_operation_keys:
- frequency.fft2
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S09
- S10
- S25
---

# FRQ-01A: fft2

Two-dimensional complex forward transform.

Inherits [FRQ-01 family contract](FRQ-01_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The ordinary floating-point F contract in FRQ-01 governs this transform; it explicitly replaces NUM correct-rounding, exact exceptional-value ordering and accelerated-ULP requirements for this member.

## Interface

Operation key: `frequency.fft2`.

### Ports

`real,imag` with identical shape and dtype → `out_real, out_imag` ; outputs a full, unshifted grid FrequencyGrid/v1.For real input, explicitly set the imaginary component to zero or use FRQ-02A..

### Parameters

Direct-node encoding of the grid parameters is given in the [FRQ-01 interface](FRQ-01_contract.md#interface). Required parameters: transformed axes; `norm` (`backward` or `ortho`); positive `spacing_y/x`; explicit `origin_y/x`; and a unit (`pixel` or a caller-defined unit). The inverse consumes `norm` from the compatible grid descriptor and rejects a conflicting override.

Inputs and outputs have matching Float32 or Float64 dtype. All static configuration is explicit. Arithmetic uses the ordinary floating-point rules in [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles), including intermediate overflow and implementation-dependent NaN payloads and zero signs.; invalid arithmetic does not itself fail the operation.

## Mathematics

Numeric reference: **F**, ordinary floating-point approximation; see [numeric reference](FILTER_numeric_reference.md).

The target is `F[ky,kx] = s * sum(z[y,x] * exp(-2*pi*i*(ky*y/H+kx*x/W)))`. The implementation approximates this mathematical DFT with a floating-point FFT.

Twiddle generation, butterfly operations and normalization incur ordinary floating-point rounding. There is no final correct-rounding proof, interval validation or exact recomputation. Small residuals at mathematical zeros are allowed. Float32 data arithmetic uses native binary32; Float64 uses binary64. Dimension-derived coefficients are generated in binary64 and converted to the working dtype. The mathematical reference supplies error measurements, not mandatory output bits.

## Behavior

Each transform group depends on its complete input. Even a single requested coefficient q reads all real and imaginary samples in that group. Distinct batches do not read one another.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Acceptance fixtures include an impulse at (0,0), whose forward transform under backward normalization is all real 1 and imaginary 0 (under ortho: real 1/sqrt(H*W)); a constant single plane with DC=H*W*c under backward or sqrt(H*W)*c under ortho and all other coefficients zero; 1×1; odd/even 2×3 single-frequency inputs; round trips that are numerically close without general bit identity; and Parseval checks that distinguish normalization modes.

Production results are compared with the F-profile error criteria in [the acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance), rather than bitwise equality.

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10); [S25 · GNU MPFR manual](../research-sources.md#s25)
