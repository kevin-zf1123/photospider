---
spec_schema_version: 1
id: RES-12
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-12A
- RES-12B
---

# RES-12: Moiré and screen-pattern suppression

Explicit notch editing and low-pass screen-pattern suppression are separate workflows; automatic recognition of unknown patterns is not delegated to the implementation.

This specification defines the target mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member is marked Accepted or production-implemented.

## Member classification

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-12A | [Explicit paired-notch periodic-interference suppression](RES-12A_paired_notch_denoise.md) | authoring_helper | S |
| RES-12B | [Gaussian low-pass filter (workflow helper)](RES-12B_gaussian_lowpass.md) | authoring_helper | B+E |

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

[S09 · FFTW: DFT definition](../research-sources.md#s09); [S26 · scikit-image filters](../research-sources.md#s26)

Boundary, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant third-party software licenses.


Quality evaluation is separate from formula conformance. Define use-case-specific scores with each critical metric passing its own threshold; do not let a weighted aggregate hide a failure. Metric formulas, datasets/reference pairs, scored regions, aggregation, and thresholds are implementation-time details and must be fixed before claiming quality acceptance. Do not infer effectiveness against all periodic interference or lossless retention of suppressed texture.
