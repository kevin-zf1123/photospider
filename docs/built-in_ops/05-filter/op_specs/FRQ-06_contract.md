---
spec_schema_version: 1
id: FRQ-06
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-06A
research_sources:
- S09
---

# FRQ-06: Complex Spectrum Multiplication

The inputs must share a sampling grid and use explicit broadcasting. The response carries no FFT amplitude normalization to be multiplied.

Inherits [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | member specification | kind | numeric reference |
| --- | --- | --- | --- |
| FRQ-06A | [Pointwise Complex-Frequency Multiplication](FRQ-06A_frequency_multiply.md) | primitive | E |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

FRQ-06A has strict and accelerated CPU keys. Direct nodes take the [FRQ-01 explicit grid encoding](FRQ-01_contract.md#interface) for F, a required canonical sorted `broadcast_axes` String (empty means none), and `response_unit=dimensionless`. H has the F dtype or Float64; its two components have matching dtype. Shape, packing and index-order compatibility is structural; sampling-unit and numerical association remain caller premises. Optional Int64 `max_work` limits each invocation. The SDK in `photospider/ops/frequency.hpp` checks both supplied static grids before appending the node.

## Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09)
