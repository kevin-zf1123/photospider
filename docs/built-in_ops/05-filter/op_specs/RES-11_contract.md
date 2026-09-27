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

This specification defines the target mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member is marked Accepted or production-implemented.

## Member classification

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-11A | [Low-contrast region smoothing](RES-11A_low_contrast_smoothing.md) | primitive | E |
| RES-11B | [Pairwise block-boundary smoothing](RES-11B_pairwise_block_boundary_smoothing.md) | primitive | S |

Letter suffixes identify distinct mathematical or interface objects; they are not quality tiers that a backend may choose arbitrarily. Primitives may propose strict and accelerated profiles; helpers only orchestrate explicit stages.

## Shared mathematics, units, and profiles

Each member file listed above defines its own formula. Members share no undeclared preprocessing; a parameter name may be reused only when its units, boundary, normalization, and RN stages agree.

## Shared execution and numerical obligations

E denotes one final RN_t rounding of the exact expression; B denotes explicitly generated and fixed coefficients; S denotes declared staged rounding. See [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Members must still validate their actual parameters, control models, and solver domains. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copies, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics and do not share one alpha-blur rule. See [color composition](FILTER_color_composition.md). Spatial extension/anchor/origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and acceptance

Each member’s support set is normative. A halo does not mean the operation can run independently on an arbitrary cropped ROI, and paging does not cancel a Whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Multi-output requests are independent; validation of an unrequested component’s payload must not introduce a hidden whole-image dependency.

All members must follow the [acceptance protocol](FILTER_oracle_protocol.md). This round’s mathematical references and associated self-checks are documented in the [oracle instructions](../../../../oracle/ops/filter/README.md); no new Photospider runtime implementation was executed. D1/D2 describe draft maturity, not pass rates.

## References and conformance notes

[S22 · scikit-image restoration](../research-sources.md#s22)

Boundary, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant third-party software licenses.


For RES-11A and RES-11B, verify mathematical conformance separately from intended-use quality. Quality acceptance uses multiple per-use scores with separately passing key metrics; do not use a weighted aggregate to hide a failing metric. Metric formulas, datasets, aggregation, regions, and thresholds are implementation-time details and must be fixed before claiming quality acceptance.
