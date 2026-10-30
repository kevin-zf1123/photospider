---
spec_schema_version: 1
id: FRQ-03
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: implemented
parent_id: 05-filter
members:
- FRQ-03A
- FRQ-03B
research_sources:
- S09
- S10
---

# FRQ-03: Frequency reordering

This is a pure permutation. It changes neither sample values, normalization, conjugacy, nor the original spatial grid.

Inherits [FILTER shared contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md) and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Specification | kind | Numeric reference |
| --- | --- | --- | --- |
| FRQ-03A | [Centered frequency shift](FRQ-03A_fftshift.md) | primitive | bitwise copy |
| FRQ-03B | [Inverse centered frequency shift](FRQ-03B_ifftshift.md) | primitive | bitwise copy |

Suffix letters identify distinct mathematical or interface objects, not quality tiers.

## Interface

Both keys are strict-only. Direct nodes take the explicit grid parameters of [FRQ-01](FRQ-01_contract.md#interface), plus `axes`, a required canonical sorted comma-separated String of selected axes. The SDK helper `frequency::shift` updates the returned caller-owned index order.

## Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Copy behavior inherits NUM.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each output reads only its own component's permuted tiles. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S10 · FFTW: real-data array format](../research-sources.md#s10)
