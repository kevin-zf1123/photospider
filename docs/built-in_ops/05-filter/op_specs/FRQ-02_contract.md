---
spec_schema_version: 1
id: FRQ-02
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-02A
- FRQ-02B
- FRQ-02C
research_sources:
- S10
---

# FRQ-02: Real DFT and Hermitian Grid

One axis is compressed. The original width must be explicit, and conjugate pairs in boundary columns must be preserved.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-02A | [Real-input half spectrum](FRQ-02A_rfft2.md) | primitive | F |
| FRQ-02B | [Hermitian half-spectrum to real field](FRQ-02B_irfft2.md) | primitive | F |
| FRQ-02C | [Explicit Hermitian-symmetry projection](FRQ-02C_project_hermitian.md) | primitive | E |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

`frequency.rfft2` and `frequency.irfft2` use the F-profile arithmetic of [FRQ-01](FRQ-01_contract.md#interface) and its static grid parameters. `project_hermitian` is an E-profile operation with strict and accelerated keys. The SDK is `photospider/ops/frequency.hpp`.

The real transforms require `packing=r2c_x`, `index_order=unshifted`, and an explicit original spatial width W. Both forward and inverse transforms emit or consume columns 0 through floor(W/2).

## Mathematics

DC and even-width Nyquist boundary columns use the smaller y index of each conjugate pair as their common representative. The opposite imaginary value is its negation, and self-conjugate imaginary intersections are structural +0. No numerical validation is applied to this packing.

For odd W, the inverse completes missing columns using `F[y,x]=conj(F[(-y)%H,W-x])` and emits the real component of the full inverse DFT. Independently edited interior coefficients are supported under the caller's boundary conjugacy premise; boundary pairs are neither validated nor repaired. `backward` and `ortho` scaling always use the explicit original H and W. Float32 inputs use native binary32 arithmetic and Float64 inputs binary64.

## FrequencyGrid/v1 logical schema

The proposed FrequencyGrid/v1 SDK/workflow representation pairs real and imaginary coefficient Results with explicit static transform configuration: original spatial shape, transformed axes, sampling spacing, coordinate origin/unit, normalization, index order and packing. Shapes and the directory are available before coefficient computation. Real and imaginary tensors have compatible schemas, while each materialized tile owns a distinct allocation. A selected transform coefficient plane has its specified whole-plane mathematical dependency unless locality is explicitly guaranteed. Edited coefficients may come from any conforming producer; numerical and payload association premises are caller obligations. Results carry no implicit frequency-grid metadata or numerical validity certificate.

## Numeric Requirements

FRQ-02A/B use ordinary floating-point F semantics, including intermediate rounding and overflow as specified by [FRQ-01](FRQ-01_contract.md#shared-mathematics-units-and-profiles). FRQ-02C retains E semantics: exact-expression output rounding, NUM exceptional-value rules and its named accelerated bounds.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Transform coefficient planes have a whole-plane dependency. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S10 · FFTW: real-data array format](../research-sources.md#s10)
