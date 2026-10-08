---
spec_schema_version: 1
id: FRQ-06
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1
implementation_status: not_implemented
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

Letter suffixes identify distinct mathematical or interface objects, not quality tiers that a backend may choose arbitrarily. Primitives may propose separately named strict and accelerated variants. Helpers only orchestrate explicit stages. An external engine requires fixed resources and approval before it can have an executable configuration.

## Shared Mathematics, Units, and Profiles

Each member formula is defined in its linked specification. Members do not share undeclared implicit preprocessing. Parameters with the same name are reusable only when their units, boundary behavior, normalization, and RN stages match.

## Shared Execution and Numeric Requirements

E denotes an exact expression rounded once to RN_t; B denotes explicitly generated and fixed coefficients; S denotes explicit staged rounding. See [numeric reference](FILTER_numeric_reference.md). NUM defines NaN/Inf and floating-point overflow behavior; this family adds no category-wide finite-only rule. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics; do not apply one alpha-blurring rule to all of them. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; each member defines actual dependencies and dirty propagation.

## Dependencies and Acceptance

The member support set is normative. Having a halo does not mean that an arbitrarily cropped ROI can run independently; paged access does not remove a whole-image dependency. Follow each member definition for staged rounding, global connectivity, transforms, and iterative state. Independently requested outputs must not turn validation of an unrequested component payload into a hidden whole-image dependency.

All members must follow the [common acceptance protocol](FILTER_oracle_protocol.md). See the [oracle usage guide](../../../../oracle/ops/filter/README.md) for mathematical references and associated self-tests. D1/D2 indicate draft maturity, not acceptance rates.

## Sources and Review

[S09 · FFTW: DFT definition](../research-sources.md#s09)

Project boundary behavior, tie-breaking, numeric domains, parameters, public schemas, and proposed operation keys are defined by these specifications. Algorithm sources do not determine parameter defaults or grant rights to third-party software.
