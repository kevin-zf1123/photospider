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

This specification defines the intended mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member has been marked Accepted or as a production implementation.

## Members

| ID | Function Spec | Kind | Numeric Reference |
| --- | --- | --- | --- |
| RES-04A | [Native grayscale BM3D profile](RES-04A_bm3d_native.md) | primitive | native profile; comparison pending |
| RES-04B | [Native color CBM3D profile](RES-04B_cbm3d_native.md) | primitive | native profile; comparison pending |

Suffix letters denote distinct mathematical/interface objects, not quality tiers that a backend may choose arbitrarily. A primitive may propose strict and accelerated profiles; a helper only orchestrates explicit stages.

## Shared Mathematics, Units, and Profiles


### Blocking Profile and Acceptance Gates
Implement BM3D/CBM3D natively, and compare against a pinned third-party implementation. The exact third-party implementation and version, along with native profile and resource details, remain unresolved; do not claim a comparison has been executed. Algorithm papers do not establish software licensing terms.
Pin version/source or binary SHA-256, CPU/ISA and thread count, randomness (expected none), noise units, profile, block dimensions/stride, search/group sizes and sorting ties, distance cutoff, 2D/3D transforms and norms, hard-stage thresholds, Wiener-stage pilot, Kaiser or other aggregation weights, boundary/padding, color-transform matrix, and input range. A third-party comparison establishes only the recorded comparison; it does not prove native NUM four-ULP compliance. Visual similarity is not conformance.


## Shared Execution and Numeric Obligations

E denotes evaluation of the exact expression followed by one final RN_t; B denotes explicitly generated, fixed coefficients; S denotes explicitly staged rounding. See the [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Members must still validate their actual parameter, control-model, and solution-domain constraints. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches inherit NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have different semantics and must not be treated as one alpha-blurring operation. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shape depends only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and Acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrary cropped ROI; paging does not eliminate a whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Outputs are independently requestable; validating payloads of an unrequested component must not introduce a hidden whole-image dependency.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Oracle reference scope is documented in the [oracle guide](../../../../oracle/ops/filter/README.md); it does not replace runtime acceptance. D1/D2 indicate draft maturity, not acceptance status.

## References and Conformance Notes

[S20 · BM3D author project](../research-sources.md#s20)

Boundary rules, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant licenses to third-party software.
