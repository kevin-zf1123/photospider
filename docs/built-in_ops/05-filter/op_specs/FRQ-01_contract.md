---
spec_schema_version: 1
id: FRQ-01
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-01A
- FRQ-01B
research_sources:
- S09
- S10
- S25
---

# FRQ-01: Two-dimensional complex DFT

The mathematical DFT is separate from its FFT algorithm. Outputs are ordinary floating-point approximations, without a correctly rounded final-output requirement.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-01A | [Two-dimensional complex forward transform](FRQ-01A_fft2.md) | primitive | F |
| FRQ-01B | [Two-dimensional complex inverse transform](FRQ-01B_ifft2.md) | primitive | F |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

`frequency.fft2` and `frequency.ifft2` are CPU operations with no strict/accelerated profile selector; `frequency.rfft2` and `frequency.irfft2` (FRQ-02) share the same static configuration. The SDK helper in `photospider/ops/frequency.hpp` is `transform(document, member, inputs, grid)`.

Direct nodes supply the following static parameters:

| Parameter | Type | Meaning |
| --- | --- | --- |
| `schema_revision` | Int64 | Must be 1. |
| `original_shape` | String | Canonical comma-separated original shape, including batch axes. |
| `y_axis`, `x_axis` | Int64 | Transformed axes. |
| `packing` | String | `full` for complex transforms, `r2c_x` for real transforms. |
| `index_order` | String | Coefficient index order; transform input is unshifted. |
| `norm` | String | `backward` or `ortho`. |
| `unit` | String | Coordinate unit. |
| `grid_id` | String | FrequencyGrid identifier. |
| `spacing_y`, `spacing_x` | Float64 | Positive sample spacing. |
| `origin_y`, `origin_x` | Float64 | Coordinate origin. |

## Shared Mathematics, Units, and Profiles

Each combination of untransformed axes forms an independent transform group. Spacing and origin annotate coordinates but do not change the DFT phase formula; physical-origin phase compensation requires an explicit multiplication. Under `backward` normalization, the forward scale is 1 and inverse scale is 1/(HW); under `ortho`, both scales are 1/sqrt(HW). Full-complex real and imaginary components have identical shapes. Coefficient outputs use FrequencyGrid/v1.

The four transforms use the **F** ordinary floating-point contract. Float32 inputs use native binary32 working values, butterflies, accumulation, real recombination and output scaling; Float64 uses binary64. Dimension-derived angles, trigonometric coefficients and normalization constants are computed in binary64 and converted to the working dtype before data arithmetic. Ordinary rounding occurs at machine operations, coefficient conversion and normalization. Correctly rounded mathematical outputs, per-component ULP bounds and cross-library, cross-algorithm or cross-platform bit identity are not required. Near-zero coefficients may contain roundoff residuals. Tests compare numerical errors against an independent reference as defined by the [acceptance protocol](FILTER_oracle_protocol.md#ordinary-floating-point-transform-acceptance).

Gradual underflow follows the CPU floating environment. Intermediate overflow and invalid arithmetic may produce Inf or NaN even if an exact expression would be finite. NaN payload/priority, exceptional-value propagation and signed-zero bits follow the executed FFT, without NUM exact-expression ordering or recovery. No finite-only validation scan is introduced. Explicit RFFT structural imaginary zeros and boundary conjugate construction remain specified in FRQ-02.

Float32 has the intermediate range and rounding of native single-precision arithmetic. It can overflow or underflow sooner than an implementation that promotes Float32 data to binary64; no value-dependent precision retry occurs. The acceptance tolerances are unchanged. Native dtype computation follows the usual approximate-FFT model without promising OpenCV/MATLAB algorithm or bit identity.

## FrequencyGrid/v1 logical schema

The proposed FrequencyGrid/v1 SDK/workflow representation pairs real and imaginary coefficient Results with explicit static transform configuration: original spatial shape, transformed axes, sampling spacing, coordinate origin/unit, normalization, index order and packing. Shapes and the directory are available before coefficient computation. Real and imaginary tensors have compatible schemas, while each materialized tile owns a distinct allocation. A selected transform coefficient plane has its specified whole-plane mathematical dependency unless locality is explicitly guaranteed. Edited coefficients may come from any conforming producer; numerical and payload association premises are caller obligations. Results carry no implicit frequency-grid metadata or numerical validity certificate.

## Numeric Requirements

F denotes the ordinary floating-point transform rules above. The E/B/S rules and NUM strict/accelerated bounds apply to other members according to their individual specifications.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not permit independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow the member definition. Outputs are independently requestable.

Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10); [S25 · GNU MPFR manual](../research-sources.md#s25)
