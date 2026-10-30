---
spec_schema_version: 1
id: RES-11
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-11A
- RES-11B
---

# RES-11: Low-contrast and block-boundary smoothing

Selective smoothing in quantized flat regions and pairwise correction at known block boundaries are separate operations.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-11A | [Low-contrast region smoothing](RES-11A_low_contrast_smoothing.md) | primitive | F |
| RES-11B | [Pairwise block-boundary smoothing](RES-11B_pairwise_block_boundary_smoothing.md) | primitive | F |

Letter suffixes identify distinct mathematical or interface objects, not quality tiers. Primitives use native fp32/fp64 arithmetic.

## Numeric Requirements

All members use native class F under [RES_native_contract](RES_native_contract.md). Shape, support, copies and declared algorithmic stages remain normative. Numerical/model premises are caller obligations, without a production validation scan or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. A halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S22 · scikit-image restoration](../research-sources.md#s22)
