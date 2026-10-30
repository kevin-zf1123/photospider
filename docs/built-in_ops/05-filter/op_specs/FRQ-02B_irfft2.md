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

Hermitian half-spectrum to real field.

Inherits [FRQ-02 family contract](FRQ-02_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The ordinary floating-point F contract in FRQ-01 governs this transform; it explicitly replaces NUM correct-rounding, exact exceptional-value ordering and accelerated-ULP requirements for this member.

## Interface

Operation key: `frequency.irfft2`.

### Ports

`real, imag` FrequencyGrid/v1 `r2c_x` → `output` with the original spatial shape.

### Parameters

The grid descriptor must provide `original_shape`, `norm`, and transformed axes. Reject shifted half-grids. Preflight fails if the original width is missing.

Inputs and outputs have matching Float32 or Float64 dtype. All static configuration is explicit. Arithmetic uses the ordinary floating-point rules in [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles), including intermediate overflow and implementation-dependent NaN payloads and zero signs.; invalid arithmetic does not itself fail the operation.

## Mathematics

Numeric reference: **F**, ordinary floating-point approximation; see [numeric reference](FILTER_numeric_reference.md).

Callers guarantee conjugacy of redundant DC and Nyquist boundary pairs, treating signed zeros as equal. Complete missing columns by `F[-ky,-kx]=conj(F[ky,kx])` and return the real part of the ordinary floating-point inverse DFT. The implementation performs no conjugacy-validation or repair pass. Independently edited interior coefficients are supported; a forward-transform producer is not required.

Twiddle generation, butterfly operations and normalization incur ordinary floating-point rounding. There is no final correct-rounding proof, interval validation or exact recomputation. Small residuals at mathematical zeros are allowed. Float32 data arithmetic uses native binary32; Float64 uses binary64. Dimension-derived coefficients are generated in binary64 and converted to the working dtype. The mathematical reference supplies error measurements, not mandatory output bits.

## Behavior

Inverse reconstruction requires fixed reads covering the complete half-spectrum. Conjugacy is an unchecked numerical premise and adds no validation reads. The inverse does not implicitly project or repair coefficient pairs; use the explicit projection member when that operation is intended.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Reconstruct small rfft fixtures. The kx=0 column pair ky=1: 1+i and ky=2: 1−i is valid; changing the second value to 1+i is invalid. A nonzero imaginary component at a self-conjugate location is invalid.

Production results are compared with the F-profile error criteria in [the acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance), rather than bitwise equality.

## References

[S10 · FFTW: real-data array format](../research-sources.md#s10)
