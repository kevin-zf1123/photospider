---
spec_schema_version: 1
id: GRD-01
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-01: Linear exposure

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-01A](GRD-01A_exposure.md) | Uniform linear exposure | `grade.exposure` |
| [GRD-01B](GRD-01B_exposure_field.md) | Exposure from a spatial EV field | `grade.exposure_field` |

## Shared mathematics and behavior

EV measures stops in linear coordinates. Source reference units remain unchanged. Successive exposure operators retain their own output rounding.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
