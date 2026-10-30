---
spec_schema_version: 1
id: GRD-08
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-08: Native HSL and HSV adjustments

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-08A](GRD-08A_hsl.md) | Native HSL adjustment | `grade.hsl` |
| [GRD-08B](GRD-08B_hsv.md) | Native HSV adjustment | `grade.hsv` |

## Shared mathematics and behavior

These members receive native HSL/HSV coordinates. RGB conversion uses the corresponding FMT equations in explicitly chosen coordinates. The bounded adjustment domain is stated per member.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
