---
spec_schema_version: 1
id: GRD-08
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: implemented_cpu_fixed_abi_9
clarification_status: complete
---

# GRD-08: Native HSL and HSV adjustments

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-08A](GRD-08A_hsl.md) | Native HSL adjustment | `grade.hsl` |
| [GRD-08B](GRD-08B_hsv.md) | Native HSV adjustment | `grade.hsv` |

## Current parameters

Each member takes two dynamic tensor inputs, `input` and `controls`, and returns `values`. Select the three native coordinates with one of the following parameter forms:

- Raw selection: `axis: Int64` is the nonnegative component axis in the tensor cell descriptor, before any batch axes. `components: String` contains exactly three distinct, in-range canonical decimal indices separated by commas. Their order is the native coordinate order in the member signature; other components are copied.
- Described selection: `input_description: String` is the serialized FMT tensor description and `group: String` names its explicit three-coordinate color group. This form excludes `axis` and `components`; component roles determine native order. The selected model must match the operation, use `relative-v1` coordinates and straight association, and retain the native units stated by the member. Profile or configured descriptions require the common analytic endpoint binding.

`hue_unit: String` is required and accepts `radian` or `pi_multiple`. The stored hue and its increment use this same unit; an explicit description hue unit must agree. The parameter asserts units and does not convert or wrap hue.

## Shared mathematics and behavior

These members receive native HSL/HSV coordinates. RGB conversion uses the corresponding FMT equations in explicitly chosen coordinates. The bounded adjustment domain is stated per member.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
