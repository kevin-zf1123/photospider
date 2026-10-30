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

It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-14A | [Dark-channel and airlight estimation](RES-14A_dark_channel_airlight.md) | primitive | order/select exact |
| RES-14B | [Dark-channel transmission estimation](RES-14B_dark_channel_transmission.md) | primitive | E |
| RES-14C | [Transmission-model application](RES-14C_apply_dehaze.md) | primitive | E |
| RES-14D | [Complete dark-channel dehazing workflow](RES-14D_dark_channel_dehaze.md) | authoring_helper | S |

Letter suffixes identify distinct mathematical or interface objects, not quality tiers.

## Mathematics

All semantic entry points require an opaque RGB group selected by explicit `input_description` and `group` parameters. RGB geometry and linear transfer interpretation are explicit static parameters under FMT; three channels alone do not imply color semantics. Every consumed RGB sample must be finite and nonnegative; bypasses follow each member’s declared read rules. HDR values above 1 are valid. An encoded-domain artistic variant requires a separate profile and must not apply a silent gamma transform. Every selected or consumed airlight component must be >0 as a caller-guaranteed numerical premise, including the source-dependent selected airlight in RES-14A; production execution performs no positivity check. Transmission is a prior estimate, not ground-truth depth.

## Numeric Requirements

E denotes one final RN_t rounding of the exact expression; B denotes explicitly generated and fixed coefficients; S denotes declared staged rounding. See [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copies, and exact discrete branches inherit NUM.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. A halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).


## References

[S23 · He, Sun, Tang: Single Image Haze Removal](../research-sources.md#s23)
