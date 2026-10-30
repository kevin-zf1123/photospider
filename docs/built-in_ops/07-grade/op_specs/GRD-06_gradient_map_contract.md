---
spec_schema_version: 1
id: GRD-06
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-06: Feature-to-color gradient mapping

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-06A](GRD-06A_gradient_map.md) | Map a feature through an RGB ramp | `grade.gradient_map` |
| [GRD-06B](GRD-06B_gradient_map_image.md) | Image gradient mapping | `grade.gradient_map_image` |

## Shared mathematics and behavior

A ramp maps one feature to three RGB values. Its color entries are RGB only. Alpha ramps and effect mixing are independently composed.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
