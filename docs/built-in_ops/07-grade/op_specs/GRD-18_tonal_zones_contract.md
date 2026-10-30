---
spec_schema_version: 1
id: GRD-18
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-18: Soft tonal zones and grading

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-18A](GRD-18A_tonal_zone_masks.md) | Soft tonal-zone weights | `grade.tonal_zone_masks` |
| [GRD-18B](GRD-18B_tonal_zone_grade.md) | Exposure and offset by tonal zone | `grade.tonal_zone_grade` |

## Shared mathematics and behavior

Zones are elementwise functions of an explicit feature: linear Y, model lightness or log exposure. Their edge values use that feature unit. They do not infer percentiles or local statistics.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
