---
spec_schema_version: 1
id: FIL-02
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- FIL-02A
- FIL-02B
research_sources:
- S01
---
# FIL-02: Separable kernels

Exact separable 2D operators. Two passes are an implementation strategy and do not imply intermediate rounding.

Inherit [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). Every member remains Proposed and is not implemented unless its metadata says otherwise.

## Members

| ID | Member | Kind | Numeric reference |
| --- | --- | --- | --- |
| FIL-02A | [Separable convolution](FIL-02A_separable_convolution.md) | primitive | E |
| FIL-02B | [Separable correlation](FIL-02B_separable_correlation.md) | primitive | E |

Member suffixes identify distinct mathematical or interface objects, not interchangeable quality levels. Primitives may propose separately named strict and accelerated keys; authoring helpers only compose explicit stages; external engines require pinned resource dependencies.

## Shared mathematics and profiles

Each member file defines its complete formula. Members do not share implicit preprocessing. A parameter name is reusable only when its units, boundary, normalization, and rounding stages agree.

## Execution and numeric obligations

E means exact expression followed by one final RN_t; B means explicitly generated, fixed coefficients; S means declared intermediate rounding stages. See [numeric reference](FILTER_numeric_reference.md). FILTER has no blanket finite-only rule. Inherit each corresponding NUM operation's NaN/Inf, signed-zero, payload, overflow, rounding, and strict/accelerated rules. Ordinary floating-point overflow does not by itself become operation failure. Retain member-specific NUM finite-domain preconditions, exact discrete branches, copy behavior, and gradual underflow requirements.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have distinct alpha semantics; do not apply one generic alpha filter. See [color composition](FILTER_color_composition.md). Spatial extension, anchor, and origin follow [boundary contract](FILTER_boundary_contract.md); structured outputs follow [collections contract](FILTER_collections_contract.md). Shapes derive from descriptors; actual demand and dirty mapping are member-specific.

## Demand and acceptance

A declared support is normative. A halo does not make an arbitrarily cropped ROI an independent image boundary; paging does not remove a whole-image dependency. Preserve staged rounding, global connectivity, transforms, and iterative state as specified. Independent output requests must not cause hidden payload validation for unrequested components.

All members use the [acceptance protocol](FILTER_oracle_protocol.md). Mathematical reference self-checks are described in the [oracle README](../../../../oracle/ops/filter/README.md); they do not establish Photospider runtime implementation. D1/D2 indicate document maturity, not acceptance.

## Sources and review

Project choices for boundaries, ties, numeric domains, explicit parameters, public schemas, and proposed keys are defined by the member specifications. Algorithm sources do not define project defaults or grant third-party license.


## Sources

[OpenCV Image Filtering](../research-sources.md#s01).
