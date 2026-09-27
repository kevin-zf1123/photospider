---
spec_schema_version: 1
id: RES-14
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-14A
- RES-14B
- RES-14C
- RES-14D
---

# RES-14: Dark-channel dehazing

Airlight estimation, transmission estimation, and application are separate explicit operations. This draft targets only the physical mixing model in linear RGB. The selected RGB group must have no associated alpha plane. For alpha-bearing input, the caller must explicitly extract straight color, composite onto a chosen background, or remove alpha; alpha is neither silently discarded nor scanned across the image to guess opacity.

This specification defines the target mathematics and interface contract only; it makes no claim about current implementation or registration. It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md). No member is marked Accepted or production-implemented.

## Member classification

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-14A | [Dark-channel and airlight estimation](RES-14A_dark_channel_airlight.md) | primitive | order/select exact |
| RES-14B | [Dark-channel transmission estimation](RES-14B_dark_channel_transmission.md) | primitive | E |
| RES-14C | [Transmission-model application](RES-14C_apply_dehaze.md) | primitive | E |
| RES-14D | [Complete dark-channel dehazing workflow](RES-14D_dark_channel_dehaze.md) | authoring_helper | S |

Letter suffixes identify distinct mathematical or interface objects; they are not quality tiers that a backend may choose arbitrarily. Primitives may propose strict and accelerated profiles; helpers only orchestrate explicit stages.

## Shared mathematics, units, and profiles

All semantic entry points require an explicitly selected, opaque RGB group with linear transfer and honor FMT metadata_mode; three channels alone do not imply color semantics. Every consumed RGB sample must be finite and nonnegative; bypasses follow each member’s declared read rules. HDR values above 1 are valid. An encoded-domain artistic variant requires a separate profile and must not apply a silent gamma transform. Every airlight component must be >0. Transmission is a prior estimate, not ground-truth depth.

## Shared execution and numerical obligations

E denotes one final RN_t rounding of the exact expression; B denotes explicitly generated and fixed coefficients; S denotes declared staged rounding. See [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Members must still validate their actual parameters, control models, and solver domains. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copies, and exact discrete branches inherit NUM.

Positive-weight normalized filters, signed kernels, local regression, and global inverse problems have different semantics and do not share one alpha-blur rule. See [color composition](FILTER_color_composition.md). Spatial extension/anchor/origin follow the [boundary contract](FILTER_boundary_contract.md); structured outputs follow the [collection contract](FILTER_collections_contract.md). Shapes depend only on descriptors; actual dependencies and dirty propagation are declared by each member.

## Dependencies and acceptance

Each member’s support set is normative. A halo does not mean the operation can run independently on an arbitrary cropped ROI, and paging does not cancel a Whole-image dependency. Staged rounding, global connectivity, transforms, and iterative state must follow each member’s definition. Multi-output requests are independent; validation of an unrequested component’s payload must not introduce a hidden whole-image dependency.

All members must follow the [acceptance protocol](FILTER_oracle_protocol.md). This round’s mathematical references and associated self-checks are documented in the [oracle instructions](../../../../oracle/ops/filter/README.md); no new Photospider runtime implementation was executed. D1/D2 describe draft maturity, not pass rates.


For RES-14 members, verify mathematical conformance separately from dehazing quality by use case. Use multiple relevant scores with each critical metric passing its own threshold; do not let a weighted aggregate hide a failure. Metric formulas, reference datasets/pairs, scored regions, aggregation, and thresholds are implementation-time details and must be fixed before claiming quality acceptance. Candidate use cases include skies, bright walls, textured scenes, and scene edges; these examples do not define final metric coverage or thresholds.

## References and conformance notes

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)

Boundary, tie-breaking, numeric domains, public schemas, and proposed keys follow the applicable contracts. Algorithm sources do not establish parameter defaults or grant third-party software licenses.
