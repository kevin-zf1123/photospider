---
spec_schema_version: 1
id: FIL-10
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- FIL-10A
- FIL-10B
---

# FIL-10: Second-Order Laplacians

The four-neighbor and nine-point profiles are separate and define their own boundaries and scale normalization.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member is marked Accepted or implemented in production.

## Members

| ID | Function specification | Kind | Numerical reference |
| --- | --- | --- | --- |
| FIL-10A | [Four-neighbor Laplacian](FIL-10A_laplacian4.md) | primitive | E |
| FIL-10B | [Nine-point isotropic approximate Laplacian](FIL-10B_laplacian8_isotropic.md) | primitive | E |

Member suffixes identify distinct mathematical or interface objects, not backend-selectable quality tiers. A primitive may propose separately named strict and accelerated variants. A helper only orchestrates explicit stages. An external engine requires pinned resources and approval before it can have an executable configuration.

## Shared mathematics, units, and profiles

See each member specification listed above for its formula. Members do not share undeclared preprocessing; parameters with the same name are reusable only when their units, boundary, normalization, and RN stages match.

## Shared execution and numerical obligations

E means evaluate the exact expression and round once to RN_t; B means explicitly generated, fixed coefficients; S means declared intermediate rounding stages. See the [numeric reference](FILTER_numeric_reference.md). FILTER has no category-wide finite-only rule except for finite-domain preconditions explicitly required by a member or NUM. Ordinary floating-point arithmetic produces NaN/Inf according to the corresponding NUM rules; ordinary floating-point overflow is not converted wholesale into operation failure. Member-defined strict bits, FP32-scaled four-ULP bounds, gradual underflow, copy behavior, and exact discrete branches continue to follow NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems have distinct semantics and must not share a blanket alpha-blurring rule. See [color composition](FILTER_color_composition.md). Spatial extension, anchors, and origins follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and acceptance

Each member’s support set is normative. Having a halo does not imply independent execution on an arbitrarily cropped ROI, and paged access does not remove a whole-image dependency. Follow each member’s declared staged rounding, global connectivity, transforms, and iterative state. Independently requested outputs must not make payload validation of an unrequested component a hidden whole-image dependency.

All members must follow the [acceptance protocol](FILTER_oracle_protocol.md). The [oracle README](../../../../oracle/ops/filter/README.md) describes mathematical references and recorded self-checks; no new Photospider runtime implementation was executed in this work. D1/D2 indicate draft maturity, not acceptance status.
