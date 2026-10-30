---
spec_schema_version: 1
id: FRQ-02A
kind: primitive
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: FRQ-02
function: rfft2
proposed_operation_keys:
- frequency.rfft2
numeric_reference: F
oracle_scope: mathematical_reference
research_sources:
- S10
---

# FRQ-02A: rfft2

Real-input half spectrum.

Inherits [FRQ-02 family contract](FRQ-02_contract.md), [FILTER shared contract](FILTER_common_contract.md), [NUM numeric semantics and precision](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT metadata and straight-color semantics](../../02-format-color/op_specs/FMT_common_contract.md). The ordinary floating-point F contract in FRQ-01 governs this transform; it explicitly replaces NUM correct-rounding, exact exceptional-value ordering and accelerated-ULP requirements for this member.

## Interface

Operation key: `frequency.rfft2`.

### Ports

`input`, a floating-point real field → `real, imag`; x extent is floor(W/2)+1 and y extent remains H.

### Parameters

Use the axis, normalization, spacing, and origin parameters defined by FRQ-01A. Packing is fixed to `r2c_x`.

Inputs and outputs have matching Float32 or Float64 dtype. All static configuration is explicit. Arithmetic uses the ordinary floating-point rules in [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles), including intermediate overflow and implementation-dependent NaN payloads and zero signs.; invalid arithmetic does not itself fail the operation.

## Mathematics

Numeric reference: **F**, ordinary floating-point approximation; see [numeric reference](FILTER_numeric_reference.md).

The target is the DFT in FRQ-01A with imaginary input zero, restricted to `0 <= kx <= floor(W/2)`. Boundary columns share one representative per conjugate pair; the other imaginary component is its negation. Self-conjugate imaginary intersections are structural +0. This construction needs no rounding certificate or conjugacy-validation scan.

Twiddle generation, butterfly operations and normalization incur ordinary floating-point rounding. There is no final correct-rounding proof, interval validation or exact recomputation. Small residuals at mathematical zeros are allowed. Float32 data arithmetic uses native binary32; Float64 uses binary64. Dimension-derived coefficients are generated in binary64 and converted to the working dtype. The mathematical reference supplies error measurements, not mandatory output bits.

## Behavior

Each output depends on the complete real input plane, even when only DC is requested.

ROI and tile edges are not image boundaries; boundary coordinates use the complete logical image.

## Acceptance

Both odd W=5 and even W=6 can produce K=3 or 4; never infer the original W from K. For H=3, the imaginary value at ky=1 on the kx=0 column is generally nonzero and is the negative of the value at ky=2.

Production results are compared with the F-profile error criteria in [the acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance), rather than bitwise equality.

## References

[S10 · FFTW: real-data array format](../research-sources.md#s10)
