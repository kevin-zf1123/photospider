---
spec_schema_version: 1
id: RES-13
kind: family_contract
category: 05-filter
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
parent_id: 05-filter
members:
- RES-13A
- RES-13B
- RES-13C
---

# RES-13: Background estimation and flat-field correction

The non-flat morphological background model, background subtraction, and correction from measured flat fields are separate operations.

It inherits the [FILTER common contract](FILTER_common_contract.md), [NUM](../../01-numeric/op_specs/NUM_common_contract.md), and [FMT](../../02-format-color/op_specs/FMT_common_contract.md).

## Members

| ID | Function spec | Composition type | Numeric reference |
| --- | --- | --- | --- |
| RES-13A | [Non-flat spherical-structure background](RES-13A_nonflat_ball_opening.md) | primitive | B+S |
| RES-13B | [Explicit background subtraction](RES-13B_subtract_background.md) | primitive | E |
| RES-13C | [Measured flat/dark correction](RES-13C_flat_field_correct.md) | primitive | E |

Letter suffixes identify distinct mathematical or interface objects, not quality tiers.

## Numeric Requirements

E denotes one final RN_t rounding of the exact expression; B denotes explicitly generated and fixed coefficients; S denotes declared staged rounding. See [numeric reference](FILTER_numeric_reference.md). Ordinary arithmetic propagates NaN, Inf, floating-point overflow, signed zero, payload, rounding, and precision according to the corresponding NUM operation. Callers guarantee the member's finite-parameter, control-model and numerical-domain premises; production execution does not validate those premises. Strict bits, final FP32-scaled four-ULP bounds, gradual underflow, copies, and exact discrete branches inherit NUM.

Alpha semantics, spatial extension and structured outputs follow [color composition](FILTER_color_composition.md), the [boundary contract](FILTER_boundary_contract.md) and the [collection contract](FILTER_collections_contract.md).

## Behavior and Acceptance

Each member's support set is normative. A halo does not imply independent execution on an arbitrary cropped ROI. Staged rounding, global connectivity, transforms, and iterative state follow each member's definition. Outputs are independently requestable. Members are accepted against the [acceptance protocol](FILTER_oracle_protocol.md).


## References

[S24 · scikit-image rolling-ball example](../research-sources.md#s24)
