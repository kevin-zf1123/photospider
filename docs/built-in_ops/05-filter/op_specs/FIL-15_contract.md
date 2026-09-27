---
spec_schema_version: 1
id: FIL-15
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- FIL-15A
- FIL-15B
- FIL-15C
research_sources:
- S07
---

# FIL-15: Gaussian/Laplacian pyramids

The binomial kernel, sampling phase, and paired expansion are fixed; approximate round-trip error is not described as lossless.

Inherits the [FILTER common contract](FILTER_common_contract.md), [NUM numerical/precision contract](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT format/color contract](../../02-format-color/op_specs/FMT_common_contract.md).No member has been marked Accepted or implemented for production.

## Members

| ID | Operation spec | Composition type | Numerical reference |
| --- | --- | --- | --- |
| FIL-15A | [Gaussian pyramid](FIL-15A_gaussian_pyramid.md) | primitive | S |
| FIL-15B | [Laplacian pyramid](FIL-15B_laplacian_pyramid.md) | primitive | S |
| FIL-15C | [Laplacian pyramid reconstruction](FIL-15C_reconstruct_laplacian.md) | primitive | S |

Member suffixes denote distinct mathematical/interface objects, not backend-selectable quality tiers. Primitives may propose separately named strict and accelerated variants; helpers only orchestrate explicit stages; external engines need fixed resources and approval before an executable configuration can be specified.

## Shared mathematics, units, and profiles


### pyramid_binomial5_v1
The one-dimensional kernel is h=[1,4,6,4,1]/16; the 2D kernel is its exact outer product; boundary=reflect_half. Reduce first applies the exact 2D convolution and RN_t, then samples even coordinates (2y,2x), with ceil-sized output. For expand, define the coarse image on an infinite integer grid and extend it with reflect_half. At each original target coordinate q, accumulate only offsets d∈[-2,2]² for which q-d is even, using coarse coordinate (q-d)/2 and weight 4*h[dy]*h[dx], then sum exactly and round once to RN_t. This differs from cropping the target first and then extending its boundary. Both axes are scaled by two. Read target_shape from the saved original-level descriptor; do not guess parity. One expand/reduce pair is not an inverse; the Laplacian residual compensates for this rather than assuming inverse filtering.


## Shared execution and numerical obligations

E means one final RN_t rounding of the complete exact expression, B means explicitly generated and fixed coefficients, and S means explicitly declared stage rounding; see the [numerical reference](FILTER_numeric_reference.md). Except for NUM finite-domain preconditions explicitly stated by a member, FILTER imposes no category-wide finite-only rule. Ordinary floating-point arithmetic produces NaN/Inf under the corresponding NUM special-value rules; ordinary floating-point overflow is not generally converted to operation failure. Member-defined strict bits, FP32-scaled four-ULP bounds, gradual underflow, copy, and exact discrete branches continue to inherit NUM.

Positive-weight normalization, signed kernels, local regression, and global inverse problems differ and cannot share one alpha-blurring rule. See [color composition](FILTER_color_composition.md). Spatial extension/anchor/origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collections contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and acceptance

Each member’s support set is normative. A halo does not mean arbitrary cropped ROIs can run independently; paging does not eliminate a whole-image dependency.Follow each member’s definition for staged rounding, global connectivity, transforms, and iterative state.Independently requested outputs must not turn validation of an unrequested component payload into a hidden whole-image dependency.

All members must pass the [common acceptance protocol](FILTER_oracle_protocol.md). Mathematical references and related self-tests for this work are documented in the [oracle README](../../../../oracle/ops/filter/README.md); no new implementation was run in the Photospider runtime. D1/D2 indicate draft maturity, not pass rates.

## Sources and review

[S07 · Paris, Hasinoff, Kautz: Local Laplacian Filters](../research-sources.md#s07)
