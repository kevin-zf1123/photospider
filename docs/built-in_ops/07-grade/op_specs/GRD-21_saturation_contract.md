---
spec_schema_version: 1
id: GRD-21
kind: shared_operator_contract
category: 07-grade
status: Proposed
document_maturity: D1_draft
implementation_status: not_implemented
clarification_status: complete
---

# GRD-21: Saturation and versioned vibrance

Inherit [GRD common](GRD_common_contract.md) for port representation, native floating computation, explicit color interpretation and tile/ROI behavior. Every member below is a separately callable tile operator.

## Members

| Member | Function | Proposed key |
| --- | --- | --- |
| [GRD-21A](GRD-21A_saturation_linear.md) | Linear-RGB saturation | `grade.saturation_linear` |
| [GRD-21B](GRD-21B_saturation_opponent.md) | Opponent-space saturation | `grade.saturation_opponent` |
| [GRD-21C](GRD-21C_vibrance_oklab.md) | OKLab vibrance | `grade.vibrance_oklab` |

## Shared mathematics and behavior

Linear saturation moves RGB along a ray around working-basis Y. Opponent saturation scales a,b. Vibrance uses the explicitly defined low-chroma gain. Optional skin protection is an external effect mask.

## References and acceptance

Member pages define analytic cases and their input/output domains. Compare against the [strict oracle](../../../../oracle/ops/07-grade/README.md) using the common native floating criteria. [Research sources](../research-sources.md) distinguish public equations from project-defined artistic choices.
