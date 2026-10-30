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

This specification defines the mathematics and interface contract. Both RES-01 members have native bundled implementations. It inherits the [FILTER common contract](FILTER_common_contract.md), [RES native floating-point contract](RES_native_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). Implementation does not change the Proposed acceptance status.

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-01A | [High-pass MAD noise scale](RES-01A_noise_mad_highpass.md) | primitive | F |
| RES-01B | [Calibrated Poisson–Gaussian variance field](RES-01B_calibrated_noise_variance.md) | primitive | F |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. Each primitive uses one native fp32/fp64 key; a helper orchestrates explicit algorithmic stages.

## Shared Mathematics, Units, and Profiles

See each member file listed above for its formula. Members do not share undeclared implicit preprocessing. Parameters with the same name are reusable only when their units, boundaries, normalization, and RN stages match.

## Shared Execution and Numeric Obligations

All members use numeric class F under [RES native floating-point contract](RES_native_contract.md). The input dtype selects native fp32 or fp64 arithmetic and stored state. Caller domain premises remain unchecked; shape, support, copy branches and algorithmic stages remain normative. There is no strict/accelerated quality split or NUM four-ULP fallback.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; each member defines mathematical support and fixed compiled reads.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable. Only selected computation and required algorithmic control contribute payload reads; no payload-validation phase is introduced.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S22 · scikit-image restoration](../research-sources.md#s22);[S18 · Buades, Coll, Morel: Non-Local Means Denoising](../research-sources.md#s18)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
