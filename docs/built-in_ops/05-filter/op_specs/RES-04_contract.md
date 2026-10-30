---
spec_schema_version: 1
id: RES-04
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D2_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-04A
- RES-04B
---

# RES-04: Native BM3D/CBM3D

The native algorithm requires a fixed mathematical profile; an algorithm name alone is not a complete pixel-level specification.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-04A | [Native grayscale BM3D profile](RES-04A_bm3d_native.md) | primitive | F |
| RES-04B | [Native color CBM3D profile](RES-04B_cbm3d_native.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S20 · BM3D author project](../research-sources.md#s20)
