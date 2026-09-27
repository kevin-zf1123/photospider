---
spec_schema_version: 1
id: FIL-13
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- FIL-13A
- FIL-13B
research_sources:
- S06
---

# FIL-13: Canny and hysteresis connectivity

Spatial filtering is not global connectivity; final edges require coordination across tiles.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT format/color contract](../../02-format-color/op_specs/FMT_common_contract.md).No member has been marked Accepted or implemented for production.

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-13A | [Static Canny workflow](FIL-13A_canny_quantized4.md) | authoring_helper | S |
| FIL-13B | [Weak/strong edge connectivity](FIL-13B_hysteresis_edges.md) | primitive | discrete exact |

Member suffixes denote distinct mathematical/interface objects, not backend-selectable quality tiers. Primitives may propose separately named strict and accelerated variants; helpers only orchestrate explicit stages; external engines need fixed resources and approval before an executable configuration can be specified.

## Shared mathematics, units, and profiles

This draft uses quantized four-direction `quantized4_v1` NMS and is not bit-compatible with libraries that interpolate NMS directions. The low threshold must be strictly greater than zero so an all-zero response does not become weak; comparisons include endpoints.

## Shared execution and numerical obligations

E means one final RN_t rounding of the complete exact expression, B means explicitly generated and fixed coefficients, and S means explicitly declared stage rounding; see the [numerical reference](FILTER_numeric_reference.md). Except for NUM finite-domain preconditions explicitly stated by a member, FILTER imposes no category-wide finite-only rule. Ordinary floating-point arithmetic produces NaN/Inf under the corresponding NUM special-value rules; ordinary floating-point overflow is not generally converted to operation failure. Member-defined strict bits, FP32-scaled four-ULP bounds, gradual underflow, copy, and exact discrete branches continue to inherit NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems differ and cannot share one alpha-blurring rule. See [color composition](FILTER_color_composition.md). Spatial extension/anchor/origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and acceptance

Each member’s support set is normative. A halo does not mean arbitrary cropped ROIs can run independently; paging does not eliminate a whole-image dependency.Follow each member’s definition for staged rounding, global connectivity, transforms, and iterative state.Independently requested outputs must not turn validation of an unrequested component payload into a hidden whole-image dependency.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and related self-tests for this work are documented in the [oracle README](../../../../oracle/ops/filter/README.md); no new implementation was run in the Photospider runtime. D1/D2 indicate draft maturity, not pass rates.

## Sources and review

[S06 · scikit-image feature](../research-sources.md#s06)
