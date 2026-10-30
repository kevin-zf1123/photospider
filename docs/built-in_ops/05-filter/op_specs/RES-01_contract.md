---
spec_schema_version: 1
id: RES-01
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: implemented
parent_id: 05-filter
members:
- RES-01A
- RES-01B
---

# RES-01: Noise Estimation

MAD estimation is distinct from explicitly calibrated camera variance; neither guarantees that texture and noise can be identified separately.

It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-01A | [High-pass MAD noise scale](RES-01A_noise_mad_highpass.md) | primitive | F |
| RES-01B | [Calibrated Poisson–Gaussian variance field](RES-01B_calibrated_noise_variance.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers. Each primitive uses one native fp32/fp64 key.

## Numeric Requirements

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).

## References

[S22 · scikit-image restoration](../research-sources.md#s22); [S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)
